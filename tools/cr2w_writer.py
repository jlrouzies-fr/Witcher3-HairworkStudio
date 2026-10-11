"""
cr2w_writer - lossless CR2W model + append-only writer/patcher (Witcher 3, v164 5.0; also loads v162/v163).

Load a CR2W file into an editable model and save it again. Unchanged files re-serialise byte-identically;
edits are applied append-only: existing name / import / export indices never move, so opaque custom blobs
(scene section events, cutscene templates, component trailing data, SAppearanceAttachment data, material
params) that reference table indices stay valid.

    m = CR2WModel(open(path, 'rb').read())
    root = m.edit(0)                         # decoded export node, marked dirty
    apps = root.get('appearances')           # Arr of Struct
    ...
    data = m.save()                          # bytes; tables, hashes, CRCs, sizes recomputed

Value model (type-driven; the type string of the owning property decides the encoding):
    Prim (Int*/Uint*/Float/Double/Bool)  python int / float
    CName, enums                          str (CName); EnumV(str) for enum values, BitsV(list) for bitfields
    String                                WStr(text[, ansi]) or a plain str (5.0 rule: latin-1 if possible)
    StringAnsi                            AStr(text)
    handle:/ptr:/soft:                    cr2w_reader.Ref(kind 'export'/'import'/'null', idx, type)
    array:/static:/[N]                    Arr(items)  (Uint8/Int8 arrays -> bytes)
    struct / class values                 Struct(type, [Prop(name, type, value), ...])
    CGUID / IdTag / EntityHandle          bytes (16 / 17 / 17)
    LocalizedString / CDateTime / DeferredDataBuffer   int
    DataBuffer / SharedDataBuffer         bytes
    CVariant                              Variant(inner_type, value)
    EngineTransform / EngineQsTransform   Transform(flags, [floats])
    TagList                               Tags([names])
    anything the decoder could not prove  RawV(bytes)   (kept verbatim; references the file's own tables)

Every decoded property is re-encoded immediately and compared with the original bytes; if the encoder does
not reproduce them the value is kept as RawV (counted in model.stats['raw_props']). Likewise a whole export
that does not re-encode exactly is kept verbatim (stats['raw_exports']). Round-trip identity is therefore
guaranteed; the stats measure encoder coverage.

CLI:  py cr2w_writer.py <file> [--out f]   round-trip a file (full re-encode) and report identity + stats.
"""
import copy
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cr2w_reader import PRIM, Ref  # noqa: E402

TABLE_ENTRY = {0: 1, 1: 8, 2: 8, 3: 16, 4: 24, 5: 24, 6: 24}
HEADER_SIZE = 0x28 + 10 * 12  # 0xA0


def fnv1a32(data: bytes) -> int:
    h = 0x811C9DC5
    for c in data:
        h ^= c
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def name_hash(raw: bytes) -> int:
    """CR2W name-table hash: fnv1a32 over the UTF-8 name INCLUDING its terminating 0; the empty name
    (index 0, 'None') stores 0."""
    if not raw:
        return 0
    return fnv1a32(raw + b'\0')


def latin1_ok(s):
    """5.0 stores a String as latin-1 (negative vlq length; empty = 0x80) when every char is < 256."""
    return all(ord(c) < 256 for c in str(s))


def read_vlq2(d, p):
    """CDPR compressed int -> (magnitude, negative_flag, new_pos). Keeps the sign of zero (0x80)."""
    b = d[p]
    p += 1
    neg = bool(b & 0x80)
    val = b & 0x3F
    shift = 6
    if b & 0x40:
        while True:
            b = d[p]
            p += 1
            val |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
    return val, neg, p


def write_vlq2(mag, neg=False):
    first = mag & 0x3F
    mag >>= 6
    out = bytearray([first | (0x80 if neg else 0) | (0x40 if mag else 0)])
    while mag:
        b = mag & 0x7F
        mag >>= 7
        out.append(b | (0x80 if mag else 0))
    return bytes(out)


# ----------------------------------------------------------------------------------------------
# value classes


class Prop:
    __slots__ = ('name', 'type', 'value')

    def __init__(self, name, type_, value):
        self.name, self.type, self.value = name, type_, value

    def __repr__(self):
        return f'Prop({self.name}:{self.type}={self.value!r})'


class PropList:
    """Ordered property list with dict-like helpers (base of Struct / ExportNode / ObjItem)."""

    def prop(self, name):
        for p in self.props:
            if p.name == name:
                return p
        return None

    def get(self, name, default=None):
        p = self.prop(name)
        return p.value if p else default

    def type_of(self, name):
        p = self.prop(name)
        return p.type if p else None

    def has(self, name):
        return self.prop(name) is not None

    def names(self):
        return [p.name for p in self.props]

    def set(self, name, value, type_=None, after=None, before=None):
        """Set (or insert) a property. A new property needs type_; it is inserted after `after` /
        before `before` when given (and present), else appended."""
        p = self.prop(name)
        if p:
            p.value = value
            if type_:
                p.type = type_
            return p
        if not type_:
            raise ValueError(f'new property {name} needs a type')
        p = Prop(name, type_, value)
        idx = len(self.props)
        names = self.names()
        if after and after in names:
            idx = names.index(after) + 1
        elif before and before in names:
            idx = names.index(before)
        self.props.insert(idx, p)
        return p

    def remove(self, name):
        n = len(self.props)
        self.props = [p for p in self.props if p.name != name]
        return n != len(self.props)


class Struct(PropList):
    __slots__ = ('type', 'props', 'lead', 'term', 'blob')

    def __init__(self, type_, props=None, lead=True, term=True, blob=None):
        self.type, self.props, self.lead, self.term, self.blob = type_, list(props or []), lead, term, blob

    def __repr__(self):
        return f'Struct({self.type}, {self.names()})'


class ObjItem(PropList):
    """One object of a custom object list (scene section events, cutscene template lists)."""
    __slots__ = ('cls', 'props', 'term', 'extra')

    def __init__(self, cls, props=None, term=True, extra=b''):
        self.cls, self.props, self.term, self.extra = cls, list(props or []), term, extra


class ExportNode(PropList):
    """Decoded export chunk: [00] props [00 00] custom.  custom = ('raw', bytes) | ('objlists', lists, rest)
    | ('matparams', [(name, type, value)], rest)."""
    __slots__ = ('lead', 'props', 'term', 'custom')

    def __init__(self, lead=True, props=None, term=True, custom=('raw', b'')):
        self.lead, self.props, self.term, self.custom = lead, list(props or []), term, custom


class Arr:
    __slots__ = ('items',)

    def __init__(self, items=None):
        self.items = list(items or [])

    def __iter__(self):
        return iter(self.items)

    def __len__(self):
        return len(self.items)

    def __getitem__(self, i):
        return self.items[i]

    def append(self, v):
        self.items.append(v)

    def __repr__(self):
        return f'Arr({len(self.items)})'


class WStr(str):
    """String value. ansi=True: latin-1 (negative vlq length), False: UTF-16, None: 5.0 rule (latin1_ok)."""

    def __new__(cls, s, ansi=None):
        o = str.__new__(cls, s)
        o.ansi = ansi
        return o


class AStr(str):
    """StringAnsi value. raw = exact stored bytes (incl. NUL) when loaded; neg = vlq sign flag."""

    def __new__(cls, s, raw=None, neg=False):
        o = str.__new__(cls, s)
        o.raw = raw
        o.neg = neg
        return o


class EnumV(str):
    pass


class BitsV(list):
    pass


class Tags(list):
    pass


class RawV:
    __slots__ = ('data', 'why')

    def __init__(self, data, why=''):
        self.data, self.why = bytes(data), why

    def __repr__(self):
        return f'RawV({len(self.data)}, {self.why!r})'


class Variant:
    __slots__ = ('inner', 'value')

    def __init__(self, inner, value):
        self.inner, self.value = inner, value


class Transform:
    __slots__ = ('flags', 'vals', 'qs')

    def __init__(self, flags, vals, qs=False):
        self.flags, self.vals, self.qs = flags, list(vals), qs


class DecodeFail(Exception):
    pass


def elem_type(typ):
    """Element type of array:/static:/[N] type strings."""
    if typ.startswith('array:'):
        return typ.split(',', 2)[2]
    if typ.startswith('static:'):
        return typ[7:].split(',', 1)[1]
    if typ.startswith('['):
        return typ[1:].split(']', 1)[1]
    return None


def is_enum_type(typ):
    return typ.startswith('E') and len(typ) > 1 and typ[1].isupper()


# ----------------------------------------------------------------------------------------------
# export record


class XExport:
    __slots__ = ('cls', 'flags', 'parent', 'template', 'raw', 'node', 'dirty', 'crc', 'why_raw')

    def __init__(self, cls, flags, parent, template, raw, crc=None):
        self.cls, self.flags, self.parent, self.template = cls, flags, parent, template
        self.raw, self.node, self.dirty, self.crc, self.why_raw = raw, None, False, crc, None


# ----------------------------------------------------------------------------------------------
# model


class CR2WModel:
    def __init__(self, data, name='', decode_all=False):
        d = self.d = bytes(data)
        self.fname = name
        if d[:4] != b'CR2W':
            raise ValueError('not CR2W')
        (self.version, self.flags, self.timestamp, self.build, self.file_size, self.buffer_size, self.crc,
         self.num_chunks) = struct.unpack_from('<IIQIIIII', d, 4)
        self.tables = [struct.unpack_from('<III', d, 0x28 + i * 12) for i in range(10)]
        self.stats = dict(raw_props=0, raw_exports=0, reader_raw=0, decoded_exports=0)
        self.raw_reasons = []
        soff, ssize, _ = self.tables[0]
        self.strings = bytearray(d[soff:soff + ssize])
        noff, ncnt, _ = self.tables[1]
        self.names, self.name_raw, self.name_off, self.name_hash_orig = [], [], [], []
        for i in range(ncnt):
            so, h = struct.unpack_from('<II', d, noff + i * 8)
            raw = self._rawstr(so)
            self.name_off.append(so)
            self.name_raw.append(raw)
            self.names.append(raw.decode('utf-8', 'replace'))
            self.name_hash_orig.append(h)
        self._nidx = {}
        for i, n in enumerate(self.names):
            self._nidx.setdefault(n, i)
        ioff, icnt, _ = self.tables[2]
        self.imports = []
        for i in range(icnt):
            po, cls, fl = struct.unpack_from('<IHH', d, ioff + i * 8)
            raw = self._rawstr(po)
            self.imports.append(dict(path=raw.decode('utf-8', 'replace'), raw=raw, off=po, cls=self.names[cls],
                                     cls_idx=cls, flags=fl))
        poff, pcnt, _ = self.tables[3]
        self.prop_table = d[poff:poff + pcnt * 16]
        eoff, ecnt, _ = self.tables[4]
        self.exports = []
        self.data_start = None
        end = HEADER_SIZE
        for i in range(ecnt):
            cls, fl, parent, size, off, tmpl, crc = struct.unpack_from('<HHIIIII', d, eoff + i * 24)
            x = XExport(self.names[cls], fl, parent - 1, tmpl, d[off:off + size], crc)
            self.exports.append(x)
            if self.data_start is None:
                self.data_start = off
            end = max(end, off + size)
            if i and off != self._prev_end:
                raise ValueError('export chunks not contiguous: unsupported layout')
            self._prev_end = off + size
        self.export_cls_idx = [struct.unpack_from('<H', d, eoff + i * 24)[0] for i in range(ecnt)]
        boff, bcnt, _ = self.tables[5]
        self.buffers = [list(struct.unpack_from('<IIIIII', d, boff + i * 24)) for i in range(bcnt)]
        emoff, emcnt, _ = self.tables[6]
        self.embedded = [list(struct.unpack_from('<IIQII', d, emoff + i * 24)) for i in range(emcnt)]
        self.data_end = end if ecnt else self._tables_end_orig()
        self.tail = d[self.data_end:]
        self._check_layout()
        self.new_strings = []      # appended strings (bytes) after the original table
        self._nested = []          # (export idx, container, prop name, submodel)
        if decode_all:
            for i in range(len(self.exports)):
                self.node(i)

    # ------------------------------------------------------------------ basics
    def _rawstr(self, o):
        e = self.strings.index(b'\0', o)
        return bytes(self.strings[o:e])

    def _tables_end_orig(self):
        p = HEADER_SIZE
        for i in range(7):
            o, c, _ = self.tables[i]
            if c:
                p = max(p, o + c * TABLE_ENTRY[i])
        return p

    def _check_layout(self):
        """The writer emits tables 0..6 contiguously from 0xA0 in index order, then export chunks.
        Verify the source file uses that layout (all 5.0 files do)."""
        p = HEADER_SIZE
        self.empty_table_zero = {}
        for i in range(7):
            o, c, _ = self.tables[i]
            if c == 0:
                self.empty_table_zero[i] = (o == 0)
                continue
            if o != p:
                raise ValueError(f'table {i} not contiguous ({o} != {p}): unsupported layout')
            p = o + c * TABLE_ENTRY[i]
        if self.exports and self.data_start != p:
            raise ValueError('export data does not follow the tables: unsupported layout')
        for i in range(7, 10):
            if self.tables[i] != (0, 0, 0):
                raise ValueError('tables 7-9 not empty')

    def nidx(self, name):
        """Index of a name; appends it (append-only) when missing."""
        i = self._nidx.get(name)
        if i is None:
            i = len(self.names)
            raw = name.encode('utf-8')
            self.names.append(name)
            self.name_raw.append(raw)
            self.name_off.append(None)
            self.name_hash_orig.append(None)
            self._nidx[name] = i
        return i

    def name(self, i):
        if 0 <= i < len(self.names):
            return self.names[i]
        raise DecodeFail(f'bad name index {i}')

    # ------------------------------------------------------------------ decoding (lossless)
    def node(self, i):
        x = self.exports[i]
        if x.node is None and x.why_raw is None:
            self._decode_export(i)
        return x.node

    def edit(self, i):
        n = self.node(i)
        if n is None:
            raise ValueError(f'export {i} ({self.exports[i].cls}) is kept raw ({self.exports[i].why_raw}); not editable')
        self.exports[i].dirty = True
        return n

    def find(self, cls):
        return [i for i, x in enumerate(self.exports) if x.cls == cls]

    def _decode_export(self, i):
        x = self.exports[i]
        d = x.raw
        try:
            n = self._decode_chunk(d, x.cls)
            enc = self._encode_node(n, probe=True)
            if enc != d:
                raise DecodeFail('export re-encode mismatch')
            x.node = n
            self.stats['decoded_exports'] += 1
        except Exception as ex:  # keep verbatim
            x.why_raw = repr(ex)[:120]
            self.stats['raw_exports'] += 1
            self.raw_reasons.append(('export', x.cls, x.why_raw))

    def _decode_chunk(self, d, cls):
        end = len(d)
        if not d or d[0] != 0:
            return ExportNode(lead=False, props=[], term=False, custom=('raw', d))
        props, p, term = self._read_props(d, 1, end)
        node = ExportNode(lead=True, props=props, term=term, custom=('raw', d[p:]))
        if p < end:
            try:
                if (cls.startswith('CStoryScene') and cls.endswith('Section')) or cls == 'CCutsceneTemplate':
                    lists, q = self._read_objlists(d, p, end)
                    if lists is not None:
                        node.custom = ('objlists', lists, d[q:])
                elif cls == 'CMaterialInstance':
                    params, q = self._read_matparams(d, p, end)
                    node.custom = ('matparams', params, d[q:])
            except Exception:
                node.custom = ('raw', d[p:])
        return node

    def _read_objlists(self, d, p, end):
        lists = []
        while p + 4 <= end:
            cnt, = struct.unpack_from('<I', d, p)
            if cnt > 100000:
                break
            q = p + 4
            items = []
            ok = True
            for _ in range(cnt):
                if q + 7 > end:
                    ok = False
                    break
                sz, ci = struct.unpack_from('<IH', d, q)
                if sz < 7 or q + sz > end or d[q + 6] != 0:
                    ok = False
                    break
                cls = self.name(ci)
                props, r, term = self._read_props(d, q + 7, q + sz)
                items.append(ObjItem(cls, props, term, d[r:q + sz]))
                q += sz
            if not ok:
                break
            lists.append(items)
            p = q
        if not lists:
            return None, p
        return lists, p

    def _read_matparams(self, d, p, end):
        cnt, = struct.unpack_from('<I', d, p)
        q = p + 4
        params = []
        for _ in range(cnt):
            sz, ni, ti = struct.unpack_from('<IHH', d, q)
            typ = self.name(ti)
            nm = self.name(ni)
            vb = d[q + 8:q + sz]
            try:
                v, r = self._read_value(d, typ, q + 8, q + sz)
                if r != q + sz or self._enc(typ, v, probe=True) != vb:
                    raise DecodeFail('matparam')
            except Exception:
                v = RawV(vb, 'matparam')
                self.stats['raw_props'] += 1
            params.append((nm, typ, v))
            q += sz
        return params, q

    def _read_props(self, d, p, end):
        props = []
        while p + 2 <= end:
            ni, = struct.unpack_from('<H', d, p)
            if ni == 0:
                return props, p + 2, True
            ti, sz = struct.unpack_from('<HI', d, p + 2)
            name = self.name(ni)
            typ = self.name(ti)
            vstart, vend = p + 8, p + 4 + sz
            if vend > end or sz < 4:
                raise DecodeFail(f'prop {name} size {sz} overflows')
            vb = d[vstart:vend]
            val = None
            try:
                v, q = self._read_value(d, typ, vstart, vend)
                if q != vend:
                    raise DecodeFail('size')
                val = v
            except Exception:
                val = self._alt_value(d, typ, vstart, vend)
            if val is None:
                val = RawV(vb, 'reader-raw')
                self.stats['reader_raw'] += 1
            else:
                try:
                    ok = self._enc(typ, val, probe=True) == vb
                except Exception:
                    ok = False
                if not ok:
                    val = RawV(vb, 'encoder-mismatch')
                    self.stats['raw_props'] += 1
                    self.raw_reasons.append(('prop', name, typ))
            props.append(Prop(name, typ, val))
            p = vend
        return props, p, False

    def _alt_value(self, d, typ, p, end):
        n = end - p
        try:
            if n == 2:
                ni, = struct.unpack_from('<H', d, p)
                return EnumV(self.name(ni) if ni else '')
            if n % 2 == 0 and n >= 2 and struct.unpack_from('<H', d, end - 2)[0] == 0:
                return BitsV([self.name(struct.unpack_from('<H', d, q)[0]) for q in range(p, end - 2, 2)])
        except Exception:
            return None
        return None

    def _read_struct(self, d, p, end, typ):
        lead = False
        if d[p] == 0:
            lead = True
            p += 1
        props, p, term = self._read_props(d, p, end)
        return Struct(typ, props, lead, term), p

    def _read_value(self, d, typ, p, end):
        if typ in PRIM:
            fmt, n = PRIM[typ]
            v = struct.unpack_from(fmt, d, p)[0]
            if v != v:  # NaN: python may quiet signalling-NaN payloads on re-pack -> keep the exact bytes
                return RawV(d[p:p + n], 'nan'), p + n
            return v, p + n
        if typ == 'CName':
            ni, = struct.unpack_from('<H', d, p)
            return self.name(ni), p + 2
        if typ == 'String':
            ln, neg, p = read_vlq2(d, p)
            if neg:
                return WStr(d[p:p + ln].decode('latin1'), ansi=True), p + ln
            raw = d[p:p + 2 * ln]
            return WStr(raw.decode('utf-16-le', 'surrogatepass'), ansi=False), p + 2 * ln
        if typ == 'StringAnsi':
            ln, neg, p = read_vlq2(d, p)
            raw = d[p:p + ln]
            return AStr(raw.rstrip(b'\0').decode('latin1'), raw=raw, neg=neg), p + ln
        if typ == 'SMeshTypeResourceLODLevel':
            return struct.unpack_from('<f', d, p)[0], p + 4
        if typ == 'SAppearanceAttachment':
            st, p = self._read_struct(d, p, end, typ)
            n, = struct.unpack_from('<I', d, p)
            if n < 4:
                raise DecodeFail('attachment blob size')
            st.blob = d[p + 4:p + n]
            return st, p + n
        if typ == 'LocalizedString':
            return struct.unpack_from('<I', d, p)[0], p + 4
        if typ == 'CGUID':
            return d[p:p + 16], p + 16
        if typ in ('IdTag', 'EntityHandle'):
            return d[p:p + 17], p + 17
        if typ == 'CDateTime':
            return struct.unpack_from('<Q', d, p)[0], p + 8
        if typ.startswith('handle:') or typ.startswith('ptr:'):
            v, = struct.unpack_from('<i', d, p)
            t = typ.split(':', 1)[1]
            if v > 0:
                return Ref('export', v - 1, t), p + 4
            if v < 0:
                return Ref('import', -v - 1, t), p + 4
            return Ref('null', -1, t), p + 4
        if typ.startswith('soft:'):
            v, = struct.unpack_from('<H', d, p)
            return Ref('import' if v else 'null', v - 1, typ[5:]), p + 2
        if typ.startswith('array:') or typ.startswith('static:') or typ.startswith('['):
            et = elem_type(typ)
            cnt, = struct.unpack_from('<I', d, p)
            p += 4
            if typ.startswith('array:') and et in ('Uint8', 'Int8'):
                if p + cnt > end:
                    raise DecodeFail('byte array overflow')
                return d[p:p + cnt], p + cnt
            items = []
            for _ in range(cnt):
                v, p = self._read_value(d, et, p, end)
                items.append(v)
            return Arr(items), p
        if typ == 'EngineTransform':
            fl = d[p]
            p += 1
            vals = []
            for bit in (1, 2, 4):
                if fl & bit:
                    vals += struct.unpack_from('<3f', d, p)
                    p += 12
            return Transform(fl, vals), p
        if typ == 'EngineQsTransform':
            fl = d[p]
            p += 1
            vals = []
            for bit in (1, 2, 4):
                if fl & bit:
                    vals += struct.unpack_from('<4f', d, p)
                    p += 16
            return Transform(fl, vals, qs=True), p
        if typ == 'TagList':
            cnt, neg, p = read_vlq2(d, p)
            if neg:
                raise DecodeFail('neg taglist')
            tags = Tags()
            for _ in range(cnt):
                ni, = struct.unpack_from('<H', d, p)
                tags.append(self.name(ni))
                p += 2
            return tags, p
        if typ == 'DeferredDataBuffer':
            return struct.unpack_from('<H', d, p)[0], p + 2
        if typ in ('DataBuffer', 'SharedDataBuffer'):
            n, = struct.unpack_from('<I', d, p)
            return d[p + 4:p + 4 + n], p + 4 + n
        if typ == 'CVariant':
            ni, = struct.unpack_from('<H', d, p)
            sz, = struct.unpack_from('<I', d, p + 2)
            inner = self.name(ni)
            v, q = self._read_value(d, inner, p + 6, p + 2 + sz)
            if q != p + 2 + sz:
                raise DecodeFail('variant size')
            return Variant(inner, v), p + 2 + sz
        if is_enum_type(typ):
            ni, = struct.unpack_from('<H', d, p)
            return EnumV(self.name(ni) if ni else ''), p + 2
        return self._read_struct(d, p, end, typ)

    # ------------------------------------------------------------------ encoding
    def _ni(self, name, probe):
        """Name index for encoding. probe=True must not add names (used while verifying decodes)."""
        if probe:
            i = self._nidx.get(name)
            if i is None:
                raise DecodeFail(f'name {name!r} missing')
            return i
        return self.nidx(name)

    def _enc_props(self, props, term, probe):
        out = bytearray()
        for p in props:
            vb = self._enc(p.type, p.value, probe)
            out += struct.pack('<HHI', self._ni(p.name, probe), self._ni(p.type, probe), len(vb) + 4)
            out += vb
        if term:
            out += b'\0\0'
        return out

    def _encode_node(self, n, probe=False):
        out = bytearray()
        if n.lead:
            out += b'\0'
            out += self._enc_props(n.props, n.term, probe)
        c = n.custom
        if c[0] == 'raw':
            out += c[1]
        elif c[0] == 'objlists':
            for items in c[1]:
                out += struct.pack('<I', len(items))
                for it in items:
                    body = b'\0' + bytes(self._enc_props(it.props, it.term, probe)) + it.extra
                    out += struct.pack('<IH', len(body) + 6, self._ni(it.cls, probe)) + body
            out += c[2]
        elif c[0] == 'matparams':
            out += struct.pack('<I', len(c[1]))
            for nm, typ, v in c[1]:
                vb = self._enc(typ, v, probe)
                out += struct.pack('<IHH', len(vb) + 8, self._ni(nm, probe), self._ni(typ, probe)) + vb
            out += c[2]
        return bytes(out)

    def _enc(self, typ, v, probe=False):
        if isinstance(v, RawV):
            return v.data
        if isinstance(v, EnumV):
            return struct.pack('<H', self._ni(v, probe) if v != '' else 0)
        if isinstance(v, BitsV):
            return b''.join(struct.pack('<H', self._ni(x, probe)) for x in v) + b'\0\0'
        if typ in PRIM:
            return struct.pack(PRIM[typ][0], v)
        if typ == 'CName':
            return struct.pack('<H', self._ni(v, probe))
        if typ == 'String':
            ansi = getattr(v, 'ansi', None)
            if ansi is None:  # plain str: 5.0 rule = latin-1 when every char < 256, else UTF-16
                ansi = latin1_ok(v)
            if ansi:
                b = str(v).encode('latin1')
                return write_vlq2(len(b), True) + b
            b = str(v).encode('utf-16-le', 'surrogatepass')
            return write_vlq2(len(b) // 2, False) + b
        if typ == 'StringAnsi':
            raw = getattr(v, 'raw', None)
            if raw is None:
                raw = str(v).encode('latin1') + b'\0'
            return write_vlq2(len(raw), getattr(v, 'neg', False)) + raw
        if typ == 'SMeshTypeResourceLODLevel':
            return struct.pack('<f', v)
        if typ == 'SAppearanceAttachment':
            sb = self._enc_struct(v, probe)
            blob = v.blob or b''
            return sb + struct.pack('<I', len(blob) + 4) + blob
        if typ == 'LocalizedString':
            return struct.pack('<I', v)
        if typ == 'CGUID':
            assert len(v) == 16
            return bytes(v)
        if typ in ('IdTag', 'EntityHandle'):
            assert len(v) == 17
            return bytes(v)
        if typ == 'CDateTime':
            return struct.pack('<Q', v)
        if typ.startswith('handle:') or typ.startswith('ptr:'):
            return struct.pack('<i', self._ref_val(v))
        if typ.startswith('soft:'):
            if v is None or v.kind == 'null':
                return b'\0\0'
            if v.kind != 'import':
                raise ValueError('soft handle must reference an import')
            return struct.pack('<H', v.idx + 1)
        if typ.startswith('array:') or typ.startswith('static:') or typ.startswith('['):
            et = elem_type(typ)
            if isinstance(v, (bytes, bytearray)):
                return struct.pack('<I', len(v)) + bytes(v)
            items = v.items if isinstance(v, Arr) else list(v)
            return struct.pack('<I', len(items)) + b''.join(self._enc(et, x, probe) for x in items)
        if typ in ('EngineTransform', 'EngineQsTransform'):
            n = 4 if typ == 'EngineQsTransform' else 3
            cnt = sum(1 for bit in (1, 2, 4) if v.flags & bit) * n
            assert len(v.vals) == cnt
            return bytes([v.flags]) + struct.pack('<%df' % cnt, *v.vals)
        if typ == 'TagList':
            return write_vlq2(len(v)) + b''.join(struct.pack('<H', self._ni(x, probe)) for x in v)
        if typ == 'DeferredDataBuffer':
            return struct.pack('<H', v)
        if typ in ('DataBuffer', 'SharedDataBuffer'):
            return struct.pack('<I', len(v)) + bytes(v)
        if typ == 'CVariant':
            vb = self._enc(v.inner, v.value, probe)
            return struct.pack('<HI', self._ni(v.inner, probe), len(vb) + 4) + vb
        if is_enum_type(typ):
            return struct.pack('<H', self._ni(v, probe) if v != '' else 0)
        if isinstance(v, Struct):
            return self._enc_struct(v, probe)
        raise ValueError(f'cannot encode {typ} from {type(v).__name__}')

    def _enc_struct(self, v, probe):
        return (b'\0' if v.lead else b'') + bytes(self._enc_props(v.props, v.term, probe))

    def _ref_val(self, v):
        if v is None or v.kind == 'null':
            return 0
        if v.kind == 'export':
            if not 0 <= v.idx < len(self.exports):
                raise ValueError(f'bad export ref {v.idx}')
            return v.idx + 1
        if v.kind == 'import':
            if not 0 <= v.idx < len(self.imports):
                raise ValueError(f'bad import ref {v.idx}')
            return -(v.idx + 1)
        raise ValueError(v.kind)

    # ------------------------------------------------------------------ edit API
    def add_import(self, path, cls, flags=0, reuse=True):
        """Append an import (or reuse an existing one with the same path + class). Returns Ref."""
        if reuse:
            for i, im in enumerate(self.imports):
                if im['path'].lower() == path.lower() and im['cls'] == cls:
                    return Ref('import', i, cls)
        self.imports.append(dict(path=path, raw=path.encode('utf-8'), off=None, cls=cls, cls_idx=None, flags=flags))
        return Ref('import', len(self.imports) - 1, cls)

    def import_ref(self, path, cls=None):
        for i, im in enumerate(self.imports):
            if im['path'].lower() == path.lower() and (cls is None or im['cls'] == cls):
                return Ref('import', i, im['cls'])
        return None

    def add_export(self, cls, node=None, parent=-1, flags=0, template=0):
        """Append a new export (index = len(exports)); parent is a 0-based export index or -1."""
        x = XExport(cls, flags, parent, template, b'')
        x.node = node or ExportNode(lead=True, props=[], term=True, custom=('raw', b''))
        x.dirty = True
        self.exports.append(x)
        self.export_cls_idx.append(None)
        return len(self.exports) - 1

    def nested(self, export_idx, container, prop_name):
        """Open a nested CR2W stored in a byte-array / buffer property (flatCompiledData, cookedEffects[].buffer).
        The sub-model is written back automatically by save() when it was edited (sub.dirty())."""
        holder = container if container is not None else self.node(export_idx)
        if holder is None:
            raise ValueError(f'export {export_idx} is kept raw; cannot open nested data')
        data = holder.get(prop_name)
        if not isinstance(data, (bytes, bytearray)) or data[:4] != b'CR2W':
            raise ValueError(f'{prop_name} is not a nested CR2W')
        sub = CR2WModel(data, name=f'{self.fname}:{prop_name}')
        self._nested.append((export_idx, holder, prop_name, sub))
        return sub

    def export_refs(self):
        """{export idx: set of referencing export idx} over all decoded handles. Second value: True when some
        exports / values are raw (references hidden in raw bytes cannot be seen)."""
        refs = {}
        hidden = False

        def walk(v, owner):
            nonlocal hidden
            if isinstance(v, Ref):
                if v.kind == 'export':
                    refs.setdefault(v.idx, set()).add(owner)
            elif isinstance(v, PropList):
                for p in v.props:
                    walk(p.value, owner)
            elif isinstance(v, Arr):
                for x in v.items:
                    walk(x, owner)
            elif isinstance(v, Variant):
                walk(v.value, owner)
            elif isinstance(v, RawV):
                hidden = True
        for i in range(len(self.exports)):
            n = self.node(i)
            if n is None:
                hidden = True
                continue
            walk(n, i)
            c = n.custom
            if c[0] == 'objlists':
                for items in c[1]:
                    for it in items:
                        walk(it, i)
            elif c[0] == 'matparams':
                for _nm, _t, v in c[1]:
                    walk(v, i)
            if c[0] != 'raw' and c[-1] or c[0] == 'raw' and c[1]:
                hidden = True  # custom trailing bytes may hold export indices
        return refs, hidden

    def orphans(self):
        """Exports no longer reachable from a root export through decoded handles (e.g. the
        CEquipmentDefinition of an appearanceParams entry the patch removed). Append-only writing cannot
        delete them without renumbering; they stay in the file, owned by their parent, unreferenced."""
        refs, hidden = self.export_refs()
        reach = set()
        stack = [i for i, x in enumerate(self.exports) if x.parent < 0]
        fwd = {}
        for t, owners in refs.items():
            for o in owners:
                fwd.setdefault(o, set()).add(t)
        while stack:
            i = stack.pop()
            if i in reach:
                continue
            reach.add(i)
            stack += list(fwd.get(i, ()))
        # children of a reachable object that nothing references (components owned via parent links whose
        # handles live in raw blobs) are counted reachable when the raw data could reference them
        out = [i for i in range(len(self.exports)) if i not in reach]
        return out, hidden

    def dirty(self):
        return any(x.dirty for x in self.exports) or bool(self.new_strings) or any(
            im['off'] is None for im in self.imports) or any(o is None for o in self.name_off) or any(
            s.dirty() for _, _, _, s in self._nested)

    # ------------------------------------------------------------------ save
    def _add_string(self, raw):
        off = len(self.strings) + sum(len(s) + 1 for s in self.new_strings)
        self.new_strings.append(raw)
        return off

    def save(self, reencode_all=False):
        # 1. nested models
        for ei, holder, pn, sub in self._nested:
            if sub.dirty():
                holder.set(pn, sub.save())
                self.exports[ei].dirty = True
        # 2. export chunks (may append names)
        chunks = []
        for i, x in enumerate(self.exports):
            if reencode_all and x.node is None and x.why_raw is None:
                self.node(i)
            if (x.dirty or reencode_all) and x.node is not None:
                chunks.append(self._encode_node(x.node))
            else:
                chunks.append(x.raw)
        # 3. class name indices (may append names)
        exp_cls = []
        for i, x in enumerate(self.exports):
            ci = self.export_cls_idx[i]
            if ci is None or self.names[ci] != x.cls:
                ci = self.nidx(x.cls)
            exp_cls.append(ci)
        imp_cls = []
        for im in self.imports:
            ci = im['cls_idx']
            if ci is None or self.names[ci] != im['cls']:
                ci = self.nidx(im['cls'])
            imp_cls.append(ci)
        # 4. strings: original table untouched, new strings appended
        for i in range(len(self.names)):
            if self.name_off[i] is None:
                self.name_off[i] = self._add_string(self.name_raw[i])
        for im in self.imports:
            if im['off'] is None:
                im['off'] = self._add_string(im['raw'])
        strings = bytes(self.strings) + b''.join(s + b'\0' for s in self.new_strings)
        self.strings = bytearray(strings)
        self.new_strings = []
        # 5. tables
        t_names = b''.join(struct.pack('<II', self.name_off[i], name_hash(self.name_raw[i]))
                           for i in range(len(self.names)))
        t_imports = b''.join(struct.pack('<IHH', im['off'], imp_cls[k], im['flags'])
                             for k, im in enumerate(self.imports))
        t_props = self.prop_table
        counts = [len(strings), len(self.names), len(self.imports), len(t_props) // 16, len(self.exports),
                  len(self.buffers), len(self.embedded)]
        sizes = [counts[i] * TABLE_ENTRY[i] for i in range(7)]
        offs = []
        p = HEADER_SIZE
        for i in range(7):
            if counts[i] == 0:
                offs.append(0 if self.empty_table_zero.get(i, True) else p)
            else:
                offs.append(p)
                p += sizes[i]
        data_start = p
        exp_offs = []
        for c in chunks:
            exp_offs.append(p)
            p += len(c)
        data_end = p
        delta = data_end - self.data_end
        t_exports = b''.join(
            struct.pack('<HHIIIII', exp_cls[i], x.flags, x.parent + 1, len(chunks[i]), exp_offs[i], x.template,
                        zlib.crc32(chunks[i]))
            for i, x in enumerate(self.exports))
        t_buffers = b''.join(struct.pack('<IIIIII', *b) for b in self.buffers)
        emb = []
        for e in self.embedded:
            e = list(e)
            if e[3] >= self.data_end:
                e[3] += delta
            emb.append(struct.pack('<IIQII', *e))
        t_emb = b''.join(emb)
        tables = [strings, t_names, t_imports, t_props, t_exports, t_buffers, t_emb]
        hdr = bytearray(HEADER_SIZE)
        file_size = data_end if self.file_size == self.data_end else self.file_size + delta
        buffer_size = self.buffer_size + delta
        struct.pack_into('<4sIIQIIIII', hdr, 0, b'CR2W', self.version, self.flags, self.timestamp, self.build,
                         file_size, buffer_size, 0xDEADBEEF, self.num_chunks)
        for i in range(10):
            if i < 7:
                struct.pack_into('<III', hdr, 0x28 + i * 12, offs[i], counts[i], zlib.crc32(tables[i]))
            else:
                struct.pack_into('<III', hdr, 0x28 + i * 12, 0, 0, 0)
        struct.pack_into('<I', hdr, 0x20, zlib.crc32(bytes(hdr)))
        out = bytearray(hdr)
        for i in range(7):
            out += tables[i]
        assert len(out) == data_start
        for c in chunks:
            out += c
        out += self.tail
        return bytes(out)


# ----------------------------------------------------------------------------------------------
# v163 (mod) -> v164 (5.0) value conversion


# (owner class/struct, property) -> 5.0 type, from report_overrides.md "v163->v164 property type changes"
TYPE_RENAMES = {
    ('CCutsceneTemplate', 'burnedAudioTrackName'): 'String',
    ('CExtAnimCutsceneSoundEvent', 'soundEventName'): 'String',
    ('CStorySceneEventSound', 'soundEventName'): 'String',
    ('CStorySceneLine', 'soundEventName'): 'String',
    ('SDismembermentWoundSingleSpawn', 'soundEvents'): 'array:2,0,String',
    ('SMeshCookedData', 'bonePositions'): 'array:47,0,Vector',
    ('SMeshCookedData', 'renderChunks'): 'array:47,0,Uint8',
    ('SMeshCookedData', 'renderLODs'): 'array:47,0,Float',
}
# enum / CName values spelled differently by the 4.x editor (mod) and 5.0
ENUM_RESPELL = {
    'Dontforceanything': "Don't force anything",
    'f320': 'f/32.0',
}
# 4.x-only properties that 5.0 does not write
DROP_PROPS = {
    ('CQuestScriptBlock', 'forceKeepLoadingScreen'),
    ('CQuestConditionBlock', 'forceKeepLoadingScreen'),
}


def _norm(s):
    return re.sub(r'[^0-9a-z]', '', s.lower())


class Converter:
    """Copy values from a source model (e.g. a v163 mod file) into a destination model (a v164 5.0 file).

    - imports are re-created / reused in dst by (path, class);
    - export handles are deep-copied (the target export and everything its handles reach) and memoised;
    - type renames (StringAnsi -> String, array:46 -> array:47 ...) per TYPE_RENAMES;
    - enum spelling per ENUM_RESPELL, then a normalised match against names already in dst;
    - RawV values cannot be converted (they hold source table indices) -> ConvertError.
    """

    def __init__(self, src, dst, type_renames=None, enum_respell=None, drop_props=None, export_parent=None):
        self.src, self.dst = src, dst
        self.type_renames = dict(TYPE_RENAMES if type_renames is None else type_renames)
        self.enum_respell = dict(ENUM_RESPELL if enum_respell is None else enum_respell)
        self.drop_props = set(DROP_PROPS if drop_props is None else drop_props)
        self.export_map = {}
        self.export_parent = export_parent  # default parent for copied root-less exports
        self._dst_norm = None
        self.log = []

    def _enum(self, v):
        if v == '':
            return v
        if v in self.enum_respell:
            return self.enum_respell[v]
        if v in self.dst._nidx:
            return v
        if self._dst_norm is None:
            self._dst_norm = {}
            for n in self.dst.names:
                self._dst_norm.setdefault(_norm(n), n)
        m = self._dst_norm.get(_norm(v))
        if m and m != v:
            self.log.append(('enum-respell', v, m))
            return m
        return v

    def prop_type(self, owner, name, typ):
        return self.type_renames.get((owner, name), typ)

    def props(self, owner, props):
        out = []
        for p in props:
            if (owner, p.name) in self.drop_props:
                self.log.append(('drop', owner, p.name))
                continue
            nt = self.prop_type(owner, p.name, p.type)
            out.append(Prop(p.name, nt, self.value(p.type, nt, p.value)))
        return out

    def value(self, typ, ntyp, v):
        if isinstance(v, RawV):
            raise ConvertError(f'cannot convert raw value of type {typ} ({v.why})')
        if isinstance(v, EnumV):
            return EnumV(self._enum(v))
        if isinstance(v, BitsV):
            return BitsV(self._enum(x) for x in v)
        if typ == 'StringAnsi' and ntyp == 'String':
            return WStr(str(v), ansi=latin1_ok(v))
        if typ == 'StringAnsi':
            return AStr(str(v), raw=getattr(v, 'raw', None), neg=getattr(v, 'neg', False))
        if isinstance(v, Ref):
            return self.ref(v)
        if isinstance(v, Struct):
            st = Struct(v.type, self.props(v.type, v.props), v.lead, v.term, v.blob)
            if v.blob:
                raise ConvertError(f'{v.type} carries a custom blob with table indices; remap it explicitly')
            return st
        if isinstance(v, Arr):
            et, net = elem_type(typ), elem_type(ntyp)
            return Arr(self.value(et, net, x) for x in v.items)
        if isinstance(v, Variant):
            return Variant(v.inner, self.value(v.inner, v.inner, v.value))
        if isinstance(v, Tags):
            return Tags(v)
        if isinstance(v, Transform):
            return Transform(v.flags, v.vals, v.qs)
        if is_enum_type(typ) and isinstance(v, str):
            return EnumV(self._enum(v))
        if isinstance(v, WStr):
            return WStr(str(v), ansi=latin1_ok(v))
        return copy.copy(v)

    def ref(self, r):
        if r.kind == 'null':
            return Ref('null', -1, r.type)
        if r.kind == 'import':
            im = self.src.imports[r.idx]
            return self.dst.add_import(im['path'], im['cls'], im['flags'])
        return Ref('export', self.export(r.idx), r.type)

    def export(self, i, parent=None):
        """Deep-copy source export i into dst (memoised). Returns the dst export index."""
        if i in self.export_map:
            return self.export_map[i]
        x = self.src.exports[i]
        node = self.src.node(i)
        if node is None:
            raise ConvertError(f'source export {i} ({x.cls}) is raw')
        if node.custom[0] != 'raw' or node.custom[1]:
            raise ConvertError(f'source export {i} ({x.cls}) has custom trailing data with table indices')
        if parent is None:
            if x.parent in self.export_map:
                parent = self.export_map[x.parent]
            elif self.export_parent is not None:
                parent = self.export_parent
            else:  # owned by an export that is not copied: attach to the dst root object (export 0)
                parent = 0 if x.parent >= 0 and self.dst.exports else -1
        ni = self.dst.add_export(x.cls, None, parent, x.flags, x.template)
        self.export_map[i] = ni
        self.dst.exports[ni].node = ExportNode(node.lead, self.props(x.cls, node.props), node.term, ('raw', b''))
        return ni


class ConvertError(Exception):
    pass


LIST_KEYS = [('guid',), ('GUID',), ('name',), ('appearance', 'componentName'), ('voicetag', 'appearance'),
             ('componentName',), ('entryID',), ('id',), ('tag',)]


def _keyval(v):
    if isinstance(v, (bytes, bytearray)):
        return bytes(v).hex()
    if isinstance(v, Ref):
        return (v.kind, v.idx if v.kind == 'export' else None)
    if isinstance(v, Struct):
        return tuple((p.name, _keyval(p.value)) for p in v.props)
    return str(v)


class Merger(Converter):
    """Make a destination subtree semantically equal to a source subtree while REUSING the destination's
    structure: list elements are matched by key (guid / name / appearance+componentName / ...), handles to
    exports are merged into the existing destination export, equal scalars keep the destination encoding.
    Only what differs is replaced; new things are converted (Converter) and appended.

    Typical use (apply a mod's appearance set onto the 5.0 entity):
        mg = Merger(mod_model, van_model)
        mg.merge_prop(van_model.edit(0), mod_model.node(0), 'appearances', 'CEntityTemplate')
    keep_dst_props: property names never removed from destination structs even when the source lacks them
    (5.0-only properties).
    """

    def __init__(self, src, dst, keep_dst_props=('blasBuffer', 'dialogsetChangeTo'), **kw):
        super().__init__(src, dst, **kw)
        self.keep_dst_props = set(keep_dst_props)
        self.merged_exports = {}
        self.changes = []
        self._pool = None

    def _get_src_key(self, k):
        return k

    def merge_prop(self, dst_holder, src_holder, name, owner):
        if self._pool is None:
            self._build_pool()
        sp, dp = src_holder.prop(name), dst_holder.prop(name)
        if sp is None:
            if dp is not None and name not in self.keep_dst_props:
                dst_holder.remove(name)
                self.changes.append(('remove-prop', owner, name))
            return
        if (owner, sp.name) in self.drop_props:
            return
        nt = self.prop_type(owner, name, sp.type)
        if dp is None:
            # insert after the closest preceding source property that exists in dst
            snames = [p.name for p in src_holder.props]
            after = None
            for n in reversed(snames[:snames.index(name)]):
                if dst_holder.has(n):
                    after = n
                    break
            if after is None:
                dst_holder.props.insert(0, Prop(name, nt, self.fresh(sp.type, nt, sp.value, f"{owner}.{name}")))
            else:
                dst_holder.set(name, self.fresh(sp.type, nt, sp.value, f"{owner}.{name}"), nt, after=after)
            self.changes.append(('add-prop', owner, name))
            return
        dp.value = self.merge(dp.type, dp.value, sp.type, nt, sp.value, f'{owner}.{name}')
        if dp.type != nt:
            self.changes.append(('retype', owner, name, dp.type, nt))
            dp.type = nt

    def merge_props(self, dst_holder, src_holder, owner):
        for p in list(src_holder.props):
            self.merge_prop(dst_holder, src_holder, p.name, owner)
        snames = set(src_holder.names())
        for p in list(dst_holder.props):
            if p.name not in snames and p.name not in self.keep_dst_props:
                dst_holder.remove(p.name)
                self.changes.append(('remove-prop', owner, p.name))
        # follow the source order for the properties both sides have (dst-only kept ones stay in place)
        return dst_holder

    def merge(self, dtyp, dv, styp, ntyp, sv, where=''):
        if isinstance(dv, Struct) and isinstance(sv, Struct) and dv.type == sv.type:
            self.merge_props(dv, sv, dv.type)
            return dv
        if isinstance(dv, Arr) and isinstance(sv, Arr):
            return self.merge_list(dtyp, dv, styp, ntyp, sv, where)
        if isinstance(dv, Ref) and isinstance(sv, Ref) and dv.kind == 'export' and sv.kind == 'export':
            if self.dst.exports[dv.idx].cls == self.src.exports[sv.idx].cls:
                self.merge_export(dv.idx, sv.idx)
                return dv
        if isinstance(dv, Variant) and isinstance(sv, Variant) and dv.inner == sv.inner:
            dv.value = self.merge(dv.inner, dv.value, sv.inner, sv.inner, sv.value, where)
            return dv
        nv = self.value(styp, ntyp, sv)
        if self._same(dv, nv):
            return dv
        self.changes.append(('set', where))
        return nv

    def _same(self, a, b):
        if isinstance(a, RawV) or isinstance(b, RawV):
            return False
        if isinstance(a, Ref) and isinstance(b, Ref):
            return a.kind == b.kind and a.idx == b.idx
        if isinstance(a, Transform) and isinstance(b, Transform):
            return a.flags == b.flags and a.vals == b.vals
        if isinstance(a, (Struct, Arr, Variant)) or isinstance(b, (Struct, Arr, Variant)):
            return False
        if isinstance(a, str) and isinstance(b, str):
            return str(a) == str(b)  # equal text: keep the destination's encoding (latin1/utf16, enum)
        try:
            return type(a) is type(b) and a == b
        except Exception:
            return False

    def merge_export(self, di, si):
        if self.merged_exports.get(di) == si:
            return
        self.merged_exports[di] = si
        self.export_map.setdefault(si, di)
        sn = self.src.node(si)
        if sn is None:
            raise ConvertError(f'source export {si} is raw')
        dn = self.dst.edit(di)
        self.merge_props(dn, sn, self.dst.exports[di].cls)

    def fresh(self, styp, ntyp, sv, where=''):
        """Build a new destination value from a source value by merging into an empty skeleton, so that
        GUID-keyed elements inside it that already exist elsewhere in dst are moved (exports reused)."""
        if isinstance(sv, Struct) and not sv.blob:
            st = Struct(sv.type, [], sv.lead, sv.term)
            self.merge_props(st, sv, sv.type)
            return st
        if isinstance(sv, Arr) and any(isinstance(x, Struct) for x in sv.items):
            return self.merge_list(ntyp, Arr([]), styp, ntyp, sv, where)
        return self.value(styp, ntyp, sv)

    def _build_pool(self):
        """Snapshot of every GUID-keyed list element in dst, taken BEFORE any merge mutates lists."""
        self._pool = {}

        def walk(v):
            if isinstance(v, PropList):
                for p in v.props:
                    walk(p.value)
            elif isinstance(v, Arr):
                for it in v.items:
                    if isinstance(it, Struct):
                        for c in (('guid',), ('GUID',)):
                            g = it.get(c[0])
                            if isinstance(g, (bytes, bytearray)) and len(g) == 16:
                                self._pool.setdefault((it.type, c, (bytes(g).hex(),)), it)
                    walk(it)
        for i in range(len(self.dst.exports)):
            n = self.dst.node(i)
            if n is not None:
                walk(n)

    def _pool_take(self, x, cand, k):
        """GUID-keyed struct with the same key anywhere in dst (decoded exports), taken at most once."""
        if cand not in (('guid',), ('GUID',)) or not isinstance(x, Struct):
            return None
        if self._pool is None:
            self._build_pool()
        it = self._pool.pop((x.type, cand, k), None)
        return it

    def _list_key(self, items):
        if not items or not all(isinstance(x, Struct) for x in items):
            return None
        for cand in LIST_KEYS:
            if all(all(x.has(k) for k in cand) for x in items):
                keys = [tuple(_keyval(x.get(k)) for k in cand) for x in items]
                if len(set(keys)) == len(keys):
                    return cand
        return None

    def _src_key(self, x, cand):
        out = []
        for k in cand:
            v = x.get(k)
            if isinstance(v, Ref) and v.kind == 'import':
                im = self.src.imports[v.idx]
                out.append(('import', im['path'].lower()))
            else:
                out.append(_keyval(v))
        return tuple(out)

    def _dst_key(self, x, cand):
        out = []
        for k in cand:
            v = x.get(k)
            if isinstance(v, Ref) and v.kind == 'import':
                out.append(('import', self.dst.imports[v.idx]['path'].lower()))
            else:
                out.append(_keyval(v))
        return tuple(out)

    def merge_list(self, dtyp, dv, styp, ntyp, sv, where):
        et, nst, net = elem_type(dtyp), elem_type(styp), elem_type(ntyp)
        d_items, s_items = dv.items, sv.items
        cand = self._list_key(s_items)
        if cand and (not d_items or self._list_key(d_items) == cand):
            dmap = {self._dst_key(x, cand): x for x in d_items}
            out = []
            for x in s_items:
                k = self._src_key(x, cand)
                moved = self._pool_take(x, cand, k) if k not in dmap else None
                if k in dmap:
                    out.append(self.merge(et, dmap.pop(k), nst, net, x, f'{where}[{k}]'))
                elif moved is not None:
                    # same GUID-keyed element lives elsewhere in dst (e.g. a story phase moved to another
                    # community entry): move + merge it, reusing its exports instead of copying new ones
                    out.append(self.merge(et, moved, nst, net, x, f'{where}[{k}]'))
                    self.changes.append(('move-item', where, k))
                else:
                    out.append(self.fresh(nst, net, x, where))
                    self.changes.append(('add-item', where, k))
            for k in dmap:
                self.changes.append(('remove-item', where, k))
            dv.items = out
            return dv
        if (s_items and d_items and len(s_items) == len(d_items) and
                all(isinstance(x, (Struct, Ref)) for x in s_items + d_items)):
            dv.items = [self.merge(et, a, nst, net, b, f'{where}[{i}]') for i, (a, b) in enumerate(zip(d_items, s_items))]
            return dv
        if all(isinstance(x, Ref) and x.kind == 'export' for x in s_items + d_items) and d_items:
            # pair handles positionally while classes match, copy the rest
            out = []
            for i, x in enumerate(s_items):
                if i < len(d_items) and self.dst.exports[d_items[i].idx].cls == self.src.exports[x.idx].cls:
                    out.append(self.merge(et, d_items[i], nst, net, x, f'{where}[{i}]'))
                else:
                    out.append(self.value(nst, net, x))
            dv.items = out
            return dv
        nv = [self.value(nst, net, x) for x in s_items]
        if len(nv) == len(d_items) and all(self._same(a, b) for a, b in zip(d_items, nv)):
            return dv
        self.changes.append(('set-list', where, len(d_items), len(nv)))
        dv.items = nv
        return dv


def roundtrip(data, name=''):
    """Full decode + re-encode of every export. Returns (identical, model)."""
    m = CR2WModel(data, name)
    out = m.save(reencode_all=True)
    return out == bytes(data), m, out


def verify_crcs(data):
    """(header crc ok, table crcs ok, export crcs ok) for a CR2W file."""
    d = bytes(data)
    h = bytearray(d[:HEADER_SIZE])
    stored, = struct.unpack_from('<I', h, 0x20)
    struct.pack_into('<I', h, 0x20, 0xDEADBEEF)
    hok = zlib.crc32(bytes(h)) == stored
    tok = True
    for i in range(7):
        o, c, crc = struct.unpack_from('<III', d, 0x28 + i * 12)
        if c and zlib.crc32(d[o:o + c * TABLE_ENTRY[i]]) != crc:
            tok = False
    eoff, ecnt, _ = struct.unpack_from('<III', d, 0x28 + 4 * 12)
    eok = True
    for i in range(ecnt):
        _c, _f, _p, size, off, _t, crc = struct.unpack_from('<HHIIIII', d, eoff + i * 24)
        if zlib.crc32(d[off:off + size]) != crc:
            eok = False
    return hok, tok, eok


if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    ok, m, out = roundtrip(data, sys.argv[1])
    print('identical' if ok else 'DIFFERENT', len(data), len(out), m.stats)
    for r in m.raw_reasons[:20]:
        print(' raw', r)
    if '--out' in sys.argv:
        open(sys.argv[sys.argv.index('--out') + 1], 'wb').write(out)
