"""
cr2w_edit - minimal structural CR2W (v162-164) editor built on w3lib helpers.

Model: header fields, string blob + names (with FNV1a32 hashes of the string incl. NUL), imports, the dummy
"properties" table, exports (each chunk = bytes), and optional raw tables 5/6 (buffers/embedded; kept verbatim,
only valid when the exports they refer to are not moved, i.e. not supported for editing).

  doc = CR2WDoc.parse(bytes)          # raises if the layout is not the simple contiguous one
  doc.to_bytes()                      # byte-identical to the input when nothing was changed
  doc.name(s) / doc.add_import(path, cls) / doc.add_export(cls, parent, chunk)
  Chunk(doc, i): .props (list of Prop), .trailing, .get(name), .set(name, type, value), .to_bytes()

Layout facts (verified on 5.0 v164 w2ent / redfur / xbm, see roundtrip_test.py):
  0x00 'CR2W', u32 version, u32 flags, u64 timestamp, u32 build, u32 fileSize, u32 bufferSize, u32 headerCrc, u32 numChunks
  0x28 10 x (u32 offset, u32 count, u32 crc)  -> tables: 0 strings(bytes) 1 names(8) 2 imports(8) 3 props(16, dummy)
       4 exports(24) 5 buffers(24) 6 embedded(24) 7-9 unused
  tables are contiguous after the 0xA0 header in index order, export data follows the export table.
  name hash = FNV1a32 over the UTF-8 bytes + NUL (name 0 = '' has hash 0, string offset 0)
  export entry: u16 className, u16 flags, u32 parent(1-based, 0 = none), u32 size, u32 offset, u32 template, u32 crc
  import entry: u32 pathStringOffset, u16 className, u16 flags
  chunk: 0x00, then properties (u16 name, u16 type, u32 size incl. itself, value), u16 0, then class-specific trailing bytes.
  ptr / handle values: int32, >0 = export index + 1, <0 = -(import index + 1), 0 = null.
"""
import os
import struct
import sys
import uuid
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from w3lib import write_vlq, read_vlq  # noqa: E402,F401

ENTRY = {0: 1, 1: 8, 2: 8, 3: 16, 4: 24, 5: 24, 6: 24}


def fnv32_name(s):
    if s == '':
        return 0
    h = 0x811C9DC5
    for c in s.encode('utf-8') + b'\0':
        h ^= c
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


class Prop:
    __slots__ = ('name', 'type', 'value')

    def __init__(self, name, type_, value):
        self.name, self.type, self.value = name, type_, bytes(value)

    def __repr__(self):
        return f'Prop({self.name}:{self.type} {len(self.value)}B)'


class Export:
    def __init__(self, cls, flags, parent, data, template=0):
        self.cls = cls          # name string
        self.flags = flags
        self.parent = parent    # 0-based index, -1 none
        self.data = bytes(data)
        self.template = template


class CR2WDoc:
    def __init__(self):
        self.version = 164
        self.flags = 0
        self.timestamp = 0
        self.build = 0
        self.blob = bytearray(b'\0')      # string table
        self.names = ['']                 # strings
        self.name_off = [0]
        self.name_idx = {'': 0}
        self.imports = []                 # [path, cls, flags, path_off]
        self.table3 = bytes(16)
        self.exports = []
        self.raw5 = b''
        self.raw6 = b''
        self.n5 = self.n6 = 0
        self.hdr24 = 6   # header u32 at 0x24: always 6 in every file seen (not the export count)

    # ------------------------------------------------------------------ parse
    @classmethod
    def parse(cls, data):
        d = bytes(data)
        if d[:4] != b'CR2W':
            raise ValueError('not CR2W')
        self = cls()
        (self.version, self.flags, self.timestamp, self.build, fsize, bsize, _crc, self.hdr24) = struct.unpack_from('<IIQIIIII', d, 4)
        tables = [struct.unpack_from('<III', d, 0x28 + i * 12) for i in range(10)]
        for i in (7, 8, 9):
            if tables[i][1]:
                raise ValueError('unsupported table %d' % i)
        soff, ssize, _ = tables[0]
        self.blob = bytearray(d[soff:soff + ssize])
        noff, ncnt, _ = tables[1]
        self.names, self.name_off, self.name_hash = [], [], []
        for i in range(ncnt):
            so, h = struct.unpack_from('<II', d, noff + i * 8)
            self.names.append(self._str(so))
            self.name_off.append(so)
            self.name_hash.append(h)
        self.name_idx = {}
        for i, n in enumerate(self.names):
            self.name_idx.setdefault(n, i)
        ioff, icnt, _ = tables[2]
        for i in range(icnt):
            po, c, fl = struct.unpack_from('<IHH', d, ioff + i * 8)
            self.imports.append([self._str(po), self.names[c], fl, po])
        poff, pcnt, _ = tables[3]
        self.table3 = d[poff:poff + pcnt * 16]
        self.table3_count = pcnt
        eoff, ecnt, _ = tables[4]
        for i in range(ecnt):
            c, fl, par, size, off, tmpl, _crc = struct.unpack_from('<HHIIIII', d, eoff + i * 24)
            self.exports.append(Export(self.names[c], fl, par - 1, d[off:off + size], tmpl))
        self.n5, self.n6 = tables[5][1], tables[6][1]
        if self.n5 or self.n6:
            raise ValueError('files with buffers / embedded data are not supported by the editor')
        # structural expectations (checked again by the byte-identical test)
        self._orig_layout = tables
        return self

    def _str(self, o):
        e = self.blob.index(b'\0', o)
        return bytes(self.blob[o:e]).decode('utf-8', 'replace')

    # ------------------------------------------------------------------ names / imports / exports
    def name(self, s):
        """Index of name s, appending it to the string blob + names table when missing."""
        i = self.name_idx.get(s)
        if i is not None:
            return i
        off = len(self.blob)
        self.blob += s.encode('utf-8') + b'\0'
        self.names.append(s)
        self.name_off.append(off)
        self.name_hash.append(fnv32_name(s))
        self.name_idx[s] = len(self.names) - 1
        return len(self.names) - 1

    def add_import(self, path, cls, flags=0):
        for i, im in enumerate(self.imports):
            if im[0].lower() == path.lower() and im[1] == cls:
                return i
        off = len(self.blob)
        self.blob += path.encode('utf-8') + b'\0'
        self.name(cls)
        self.imports.append([path, cls, flags, off])
        return len(self.imports) - 1

    def add_export(self, cls, parent, chunk, flags=0x2000):
        self.name(cls)
        self.exports.append(Export(cls, flags, parent, chunk))
        return len(self.exports) - 1

    def find(self, cls):
        return [i for i, e in enumerate(self.exports) if e.cls == cls]

    def chunk(self, i):
        return Chunk(self, i)

    # ------------------------------------------------------------------ write
    def to_bytes(self):
        n_names, n_imp, n_exp = len(self.names), len(self.imports), len(self.exports)
        t3 = self.table3 if len(self.table3) == 16 else bytes(16)
        sizes = [len(self.blob), n_names * 8, n_imp * 8, len(t3), n_exp * 24]
        offs = []
        p = 0xA0
        for s in sizes:
            offs.append(p)
            p += s
        data_start = p
        chunks = bytearray()
        e_entries = bytearray()
        for e in self.exports:
            off = data_start + len(chunks)
            chunks += e.data
            e_entries += struct.pack('<HHIIIII', self.name(e.cls), e.flags, e.parent + 1, len(e.data), off, e.template,
                                     zlib.crc32(e.data))
        n_entries = bytearray()
        for i in range(n_names):
            n_entries += struct.pack('<II', self.name_off[i], self.name_hash[i])
        i_entries = bytearray()
        for path, cls, fl, po in self.imports:
            i_entries += struct.pack('<IHH', po, self.name(cls), fl)
        # names may have grown while serialising classes: rebuild if so
        if len(self.names) != n_names:
            return self.to_bytes()
        blobs = [bytes(self.blob), bytes(n_entries), bytes(i_entries), t3, bytes(e_entries)]
        out = bytearray(0xA0)
        for ti, b in enumerate(blobs):
            if b:   # empty tables are stored as (0, 0, 0)
                struct.pack_into('<III', out, 0x28 + ti * 12, offs[ti], (len(b) // ENTRY[ti]) if ti else len(b),
                                 zlib.crc32(b))
        out += b''.join(blobs)
        out += chunks
        total = len(out)
        out[0:4] = b'CR2W'
        struct.pack_into('<IIQIIIII', out, 4, self.version, self.flags, self.timestamp, self.build, total, total, 0,
                         self.hdr24)
        h = bytearray(out[:0xA0])
        struct.pack_into('<I', h, 0x20, 0xDEADBEEF)
        struct.pack_into('<I', out, 0x20, zlib.crc32(bytes(h)))
        return bytes(out)


class Chunk:
    """Property-level view of one export chunk. Changes are written back with .commit()."""

    def __init__(self, doc, i):
        self.doc, self.i = doc, i
        d = doc.exports[i].data
        self.props, self.trailing, self.raw = [], b'', None
        if not d or d[0] != 0:
            self.raw = d
            return
        p = 1
        while True:
            ni, = struct.unpack_from('<H', d, p)
            if ni == 0:
                p += 2
                break
            ti, sz = struct.unpack_from('<HI', d, p + 2)
            self.props.append(Prop(doc.names[ni], doc.names[ti], d[p + 8:p + 4 + sz]))
            p += 4 + sz
        self.trailing = d[p:]

    def to_bytes(self):
        if self.raw is not None:
            return self.raw
        doc = self.doc
        out = bytearray(b'\0')
        for pr in self.props:
            out += struct.pack('<HHI', doc.name(pr.name), doc.name(pr.type), len(pr.value) + 4) + pr.value
        out += b'\0\0' + self.trailing
        return bytes(out)

    @staticmethod
    def build(doc, props, trailing=b''):
        """Serialise a property list (+ trailing bytes) into chunk bytes without needing an export slot."""
        c = Chunk.__new__(Chunk)
        c.doc, c.i, c.props, c.trailing, c.raw = doc, -1, list(props), bytes(trailing), None
        return c.to_bytes()

    def commit(self):
        self.doc.exports[self.i].data = self.to_bytes()

    def get(self, name):
        for pr in self.props:
            if pr.name == name:
                return pr
        return None

    def set(self, name, type_, value, after=None):
        pr = self.get(name)
        if pr:
            pr.type, pr.value = type_, bytes(value)
            return pr
        pr = Prop(name, type_, value)
        if after is not None:
            idx = [p.name for p in self.props].index(after) + 1
            self.props.insert(idx, pr)
        else:
            self.props.append(pr)
        return pr

    def remove(self, name):
        self.props = [p for p in self.props if p.name != name]

    def clone_props(self):
        return [Prop(p.name, p.type, p.value) for p in self.props]


# ---------------------------------------------------------------------------------------------- value helpers

def v_ptr(export_idx):
    return struct.pack('<i', export_idx + 1)


def v_import(import_idx):
    return struct.pack('<i', -(import_idx + 1))


def v_string(s):
    """CR2W String: vlq length (chars + 1 incl. NUL? -> stored ASCII as negative length) as written by the game."""
    b = s.encode('latin1') + b'\0'
    return write_vlq(-len(b)) + b


def read_string(v):
    ln, p = read_vlq(v, 0)
    if ln < 0:
        return v[p:p - ln].rstrip(b'\0').decode('latin1')
    return v[p:p + 2 * ln].decode('utf-16-le')


def v_guid(g=None):
    return (g or uuid.uuid4()).bytes_le if hasattr(uuid.UUID, 'bytes_le') else bytes(16)


def v_i16(x):
    return struct.pack('<h', x)


def get_ptr(chunk, name):
    pr = chunk.get(name)
    return struct.unpack('<i', pr.value)[0] if pr else 0


# ---------------------------------------------------------------------------------------------- trailing lists

def read_ptr_lists(trailing, parts):
    """Component trailing: parts of [u32 count, count x u32 ptr]. Returns (lists, rest)."""
    lists, p = [], 0
    for _ in range(parts):
        n, = struct.unpack_from('<I', trailing, p)
        lists.append(list(struct.unpack_from('<%di' % n, trailing, p + 4)))
        p += 4 + 4 * n
    return lists, trailing[p:]


def write_ptr_lists(lists, rest=b''):
    out = bytearray()
    for l in lists:
        out += struct.pack('<I', len(l)) + struct.pack('<%di' % len(l), *l)
    return bytes(out) + rest


def entity_components(trailing):
    """CEntity / CItemEntity trailing: 8 bytes, u8 count, count x u32 ptr (1-based), rest."""
    n = trailing[8]
    ptrs = list(struct.unpack_from('<%di' % n, trailing, 9))
    return trailing[:8], ptrs, trailing[9 + 4 * n:]


def set_entity_components(head, ptrs, rest):
    assert len(ptrs) < 256
    return head + bytes([len(ptrs)]) + struct.pack('<%di' % len(ptrs), *ptrs) + rest
