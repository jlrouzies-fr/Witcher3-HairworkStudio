"""
cr2w_reader - full read-only decoder for Witcher 3 CR2W files (v162 / v163 next-gen 4.x / v164 5.0).

Decodes header, all tables, every export's property tree (recursively: structs, arrays, static arrays,
handles, soft handles, CName, String, enums, bitfields, buffers, DeferredDataBuffer, transforms ...),
the trailing (non-property) bytes of each export, embedded files (table 6), and nested CR2W blobs
such as CEntityTemplate.flatCompiledData.

Unknown / undecodable values fall back to {'_raw': hex, '_len': n} and decoding continues, because every
top-level property carries its own size.

Usage as a library:
    f = CR2WFile(data)
    f.exports[i].cls / .props (dict name -> Value) / .trailing
    f.tree()  -> JSON-able canonical tree (handles inlined into owners, imports as paths)
CLI:  py cr2w_reader.py <file> [--json out.json]
"""
import collections
import hashlib
import json
import struct
import sys
import zlib

# ----------------------------------------------------------------------------------------------
# primitives

PRIM = {
    'Bool': ('<B', 1), 'Int8': ('<b', 1), 'Uint8': ('<B', 1), 'Int16': ('<h', 2), 'Uint16': ('<H', 2),
    'Int32': ('<i', 4), 'Uint32': ('<I', 4), 'Int64': ('<q', 8), 'Uint64': ('<Q', 8), 'Float': ('<f', 4),
    'Double': ('<d', 8),
}

TABLE_ENTRY = {0: 1, 1: 8, 2: 8, 3: 16, 4: 24, 5: 24, 6: 24}


class DecodeError(Exception):
    pass


def read_vlq(d, p):
    b = d[p]
    p += 1
    neg = b & 0x80
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
    return (-val if neg else val), p


class Ref:
    """A handle / pointer to an export (idx >= 0) or import (kind='import')."""
    __slots__ = ('kind', 'idx', 'type')

    def __init__(self, kind, idx, type_):
        self.kind, self.idx, self.type = kind, idx, type_

    def __repr__(self):
        return f'Ref({self.kind},{self.idx},{self.type})'


class Raw:
    __slots__ = ('data', 'type', 'why')

    def __init__(self, data, type_, why=''):
        self.data, self.type, self.why = bytes(data), type_, why


class Export:
    def __init__(self, i, cls, flags, parent, size, off, template, crc):
        self.i, self.cls, self.flags, self.parent, self.size, self.off = i, cls, flags, parent, size, off
        self.template, self.crc = template, crc
        self.props = {}          # name -> (type, value)
        self.order = []          # property names in file order
        self.trailing = b''
        self.trailing_decoded = None
        self.trailing_error = None
        self.error = None
        self.children = []


class CR2WFile:
    def __init__(self, data, name='', depth=0):
        self.d = data = bytes(data)
        self.fname = name
        self.depth = depth
        if data[:4] != b'CR2W':
            raise DecodeError('not CR2W')
        (self.version, self.flags, self.timestamp, self.build, self.file_size, self.buffer_size, self.crc,
         self.num_chunks) = struct.unpack_from('<IIQIIIII', data, 4)
        self.tables = [struct.unpack_from('<III', data, 0x28 + i * 12) for i in range(10)]
        soff, ssize, _ = self.tables[0]
        self.strings = data[soff:soff + ssize]
        noff, ncnt, _ = self.tables[1]
        self.names = []
        self.name_hashes = []
        for i in range(ncnt):
            so, h = struct.unpack_from('<II', data, noff + i * 8)
            self.names.append(self._str(so))
            self.name_hashes.append(h)
        ioff, icnt, _ = self.tables[2]
        self.imports = []
        for i in range(icnt):
            po, cls, fl = struct.unpack_from('<IHH', data, ioff + i * 8)
            self.imports.append(dict(path=self._str(po), cls=self.names[cls], flags=fl))
        poff, pcnt, _ = self.tables[3]
        self.properties = []
        for i in range(pcnt):
            c, cf, pn, pf, h = struct.unpack_from('<HHHHQ', data, poff + i * 16)
            self.properties.append((c, cf, pn, pf, h))
        eoff, ecnt, _ = self.tables[4]
        self.exports = []
        for i in range(ecnt):
            cls, fl, parent, size, off, tmpl, crc = struct.unpack_from('<HHIIIII', data, eoff + i * 24)
            self.exports.append(Export(i, self.names[cls], fl, parent - 1, size, off, tmpl, crc))
        boff, bcnt, _ = self.tables[5]
        self.buffers = []
        for i in range(bcnt):
            fl, idx, off, dsz, msz, crc = struct.unpack_from('<IIIIII', data, boff + i * 24)
            self.buffers.append(dict(flags=fl, index=idx, off=off, disk=dsz, mem=msz, crc=crc))
        emoff, emcnt, _ = self.tables[6]
        self.embedded = []
        for i in range(emcnt):
            imp, path, ph, doff, dsz = struct.unpack_from('<IIQII', data, emoff + i * 24)
            self.embedded.append(dict(import_idx=imp - 1, path_off=path, path_hash=ph, off=doff, size=dsz))
        for e in self.exports:
            if 0 <= e.parent < len(self.exports):
                self.exports[e.parent].children.append(e.i)
        for e in self.exports:
            self._decode_export(e)
        # embedded files: decode recursively when they are CR2W
        self.embedded_files = []
        for em in self.embedded:
            blob = data[em['off']:em['off'] + em['size']]
            # the embedded resource's own depot path is a string-table offset (import_idx = owning import, 0-based)
            try:
                path = self._str(em['path_off'])
            except Exception:
                path = self.imports[em['import_idx']]['path'] if 0 <= em['import_idx'] < len(self.imports) else '?'
            em['path'] = path
            sub = None
            if blob[:4] == b'CR2W' and depth < 3:
                try:
                    sub = CR2WFile(blob, name=path, depth=depth + 1)
                except Exception as ex:  # noqa
                    sub = None
                    em['error'] = repr(ex)
            self.embedded_files.append((em, sub))

    # ------------------------------------------------------------------
    def _str(self, o):
        e = self.strings.index(b'\0', o)
        return self.strings[o:e].decode('utf-8', 'replace')

    def name(self, i):
        if 0 <= i < len(self.names):
            return self.names[i]
        raise DecodeError(f'bad name index {i}')

    # ------------------------------------------------------------------
    def _decode_export(self, e):
        d = self.d
        p, end = e.off, e.off + e.size
        try:
            if d[p] != 0:
                # no property block (some classes serialise only custom data)
                e.trailing = d[p:end]
                return
            p += 1
            p = self._read_props(p, end, e.props, e.order, top=True)
            e.trailing = d[p:end]
        except Exception as ex:
            e.error = repr(ex)
            e.trailing = d[p:end]
            return
        # known custom trailing layouts
        if e.trailing:
            try:
                if (e.cls.startswith('CStoryScene') and e.cls.endswith('Section')) or e.cls == 'CCutsceneTemplate':
                    lists, q = self._decode_objlists(p, end)
                    if lists is not None:
                        e.trailing_decoded = ('objlists', lists)
                        e.trailing = d[q:end]
                elif e.cls == 'CMaterialInstance':
                    params, q = self._decode_matparams(p, end)
                    e.trailing_decoded = ('matparams', params)
                    e.trailing = d[q:end]
            except Exception as ex:  # keep raw
                e.trailing_error = repr(ex)

    def _decode_objlists(self, p, end):
        """Sequence of [u32 count, count x (u32 size incl. header, u16 class, 0, props..., 0 0)] lists."""
        d = self.d
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
                props, order = {}, []
                r = self._read_props(q + 7, q + sz, props, order)
                item = {'_struct': cls, '_props': props, '_order': order}
                if r != q + sz:
                    item['_props']['@extra'] = ('bytes', d[r:q + sz])
                    item['_order'].append('@extra')
                items.append(item)
                q += sz
            if not ok:
                break
            lists.append(items)
            p = q
        if not lists:
            return None, p
        return lists, p

    def _decode_matparams(self, p, end):
        d = self.d
        cnt, = struct.unpack_from('<I', d, p)
        q = p + 4
        params = []
        for _ in range(cnt):
            sz, ni, ti = struct.unpack_from('<IHH', d, q)
            typ = self.name(ti)
            v, r = self._read_value(typ, q + 8, q + sz)
            params.append((self.name(ni), typ, v))
            q += sz
        return params, q

    def _read_props(self, p, end, out, order, top=False):
        d = self.d
        while p + 2 <= end:
            ni, = struct.unpack_from('<H', d, p)
            if ni == 0:
                return p + 2
            ti, sz = struct.unpack_from('<HI', d, p + 2)
            name = self.name(ni)
            typ = self.name(ti)
            vstart, vend = p + 8, p + 4 + sz
            if vend > end or sz < 4:
                raise DecodeError(f'prop {name} size {sz} overflows')
            try:
                val, q = self._read_value(typ, vstart, vend)
                if q != vend:
                    raise DecodeError(f'consumed {q - vstart}/{vend - vstart}')
            except Exception as ex:  # alternatives, then raw fallback
                val = self._alt_value(typ, vstart, vend)
                if val is None:
                    val = Raw(d[vstart:vend], typ, repr(ex)[:80])
            out[name] = (typ, val)
            order.append(name)
            p = vend
        return p

    def _alt_value(self, typ, p, end):
        """Second chance for a whole property value: plain enum (2 bytes) or bitfield (CName list + 0)."""
        d = self.d
        n = end - p
        try:
            if n == 2:
                ni, = struct.unpack_from('<H', d, p)
                return ('enum', self.name(ni) if ni else '')
            if n % 2 == 0 and n >= 2 and struct.unpack_from('<H', d, end - 2)[0] == 0:
                vals = [self.name(struct.unpack_from('<H', d, q)[0]) for q in range(p, end - 2, 2)]
                return ('bitfield', vals)
        except Exception:
            return None
        return None

    def _read_struct(self, p, end, typ):
        props, order = {}, []
        if self.d[p] == 0:  # class/struct values carry the same leading serialisation byte as exports
            p += 1
        p = self._read_props(p, end, props, order)
        return {'_struct': typ, '_props': props, '_order': order}, p

    def _read_value(self, typ, p, end):
        d = self.d
        if typ in PRIM:
            fmt, n = PRIM[typ]
            return struct.unpack_from(fmt, d, p)[0], p + n
        if typ == 'CName':
            ni, = struct.unpack_from('<H', d, p)
            return ('CName', self.name(ni)), p + 2
        if typ == 'String':
            ln, p = read_vlq(d, p)
            if ln < 0:
                s = d[p:p - ln].decode('latin1')
                return s, p - ln
            s = d[p:p + 2 * ln].decode('utf-16-le', 'replace')
            return s, p + 2 * ln
        if typ == 'StringAnsi':
            # vlq length (0x80 = empty), bytes include the terminating 0
            ln, p = read_vlq(d, p)
            ln = abs(ln)
            return d[p:p + ln].rstrip(b'\0').decode('latin1'), p + ln
        if typ == 'SMeshTypeResourceLODLevel':
            return struct.unpack_from('<f', d, p)[0], p + 4
        if typ == 'SAppearanceAttachment':
            # property list, then a u32-size-prefixed (size includes itself) custom attachment blob
            st, p = self._read_struct(p, end, typ)
            n, = struct.unpack_from('<I', d, p)
            blob = d[p + 4:p + n]
            st['_props']['@data'] = ('raw', ('bytes', blob))
            st['_order'].append('@data')
            return st, p + n
        if typ == 'LocalizedString':
            return ('LocString', struct.unpack_from('<I', d, p)[0]), p + 4
        if typ == 'CGUID':
            return ('GUID', d[p:p + 16].hex()), p + 16
        if typ == 'IdTag':
            return ('IdTag', d[p:p + 17].hex()), p + 17
        if typ == 'CDateTime':
            return ('DateTime', struct.unpack_from('<Q', d, p)[0]), p + 8
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
        if typ.startswith('array:'):
            et = typ.split(',', 2)[2]
            cnt, = struct.unpack_from('<I', d, p)
            p += 4
            if et in ('Uint8', 'Int8'):
                blob = d[p:p + cnt]
                return ('bytes', blob), p + cnt
            items = []
            for _ in range(cnt):
                v, p = self._read_value(et, p, end)
                items.append(v)
            return ('array', et, items), p
        if typ.startswith('static:'):
            n, et = typ[7:].split(',', 1)
            cnt, = struct.unpack_from('<I', d, p)
            p += 4
            items = []
            for _ in range(cnt):
                v, p = self._read_value(et, p, end)
                items.append(v)
            return ('array', et, items), p
        if typ.startswith('['):
            # fixed array: stored with a u32 element count like dynamic arrays
            n, et = typ[1:].split(']', 1)
            cnt, = struct.unpack_from('<I', d, p)
            p += 4
            items = []
            for _ in range(cnt):
                v, p = self._read_value(et, p, end)
                items.append(v)
            return ('array', et, items), p
        if typ == 'EngineTransform':
            fl = d[p]
            p += 1
            out = {}
            for bit, key in ((1, 'pos'), (2, 'rot'), (4, 'scale')):
                if fl & bit:
                    out[key] = struct.unpack_from('<3f', d, p)
                    p += 12
            return ('Transform', out), p
        if typ == 'EngineQsTransform':
            fl = d[p]
            p += 1
            out = {}
            for bit, key, n in ((1, 'pos', 4), (2, 'rot', 4), (4, 'scale', 4)):
                if fl & bit:
                    out[key] = struct.unpack_from('<%df' % n, d, p)
                    p += 4 * n
            return ('QsTransform', out), p
        if typ == 'TagList':
            cnt, p = read_vlq(d, p)
            tags = []
            for _ in range(cnt):
                ni, = struct.unpack_from('<H', d, p)
                tags.append(self.name(ni))
                p += 2
            return ('TagList', tags), p
        if typ == 'DeferredDataBuffer':
            return ('DeferredBuffer', struct.unpack_from('<H', d, p)[0]), p + 2
        if typ in ('DataBuffer', 'SharedDataBuffer'):
            n, = struct.unpack_from('<I', d, p)
            return ('bytes', d[p + 4:p + 4 + n]), p + 4 + n
        if typ == 'CVariant':
            ni, = struct.unpack_from('<H', d, p)
            sz, = struct.unpack_from('<I', d, p + 2)
            inner = self.name(ni)
            v, q = self._read_value(inner, p + 6, p + 2 + sz)
            return ('Variant', inner, v), p + 2 + sz
        if typ == 'EntityHandle':
            # u8 type + 16 byte guid (best guess); verified by size check at caller
            return ('EntityHandle', d[p:p + 17].hex()), p + 17
        if typ.startswith('E') and len(typ) > 1 and typ[1].isupper():
            # enum or bitfield. Plain enum: one CName index. Bitfield: CName list terminated by 0.
            ni, = struct.unpack_from('<H', d, p)
            if end - p == 2 or ni == 0:
                return ('enum', self.name(ni) if ni else ''), p + 2
            # ambiguous inside arrays; try enum first when the following bytes look like the next item
            return ('enum', self.name(ni)), p + 2
        # default: struct (serialised as a property list)
        return self._read_struct(p, end, typ)

    # ------------------------------------------------------------------
    def nested_cr2w(self):
        """Yield (label, CR2WFile) for nested CR2W blobs stored in byte-array properties (flatCompiledData...)."""
        for e in self.exports:
            for name in e.order:
                typ, v = e.props[name]
                if isinstance(v, tuple) and v[0] == 'bytes' and v[1][:4] == b'CR2W':
                    try:
                        yield f'{e.cls}#{e.i}.{name}', CR2WFile(v[1], name=f"{self.fname}:{name}", depth=self.depth + 1)
                    except Exception as ex:  # noqa
                        yield f'{e.cls}#{e.i}.{name}', ex

    # ------------------------------------------------------------------
    # canonical JSON-able tree
    def tree(self, inline=True, keyfn=None, flat=False):
        seen = set()
        roots = [e.i for e in self.exports if e.parent < 0]

        def conv(v):
            if isinstance(v, Ref):
                if v.kind == 'null':
                    return None
                if v.kind == 'import':
                    if 0 <= v.idx < len(self.imports):
                        return {'@import': self.imports[v.idx]['path']}
                    return {'@import': f'?{v.idx}'}
                if inline and v.idx not in seen and 0 <= v.idx < len(self.exports):
                    return exp(v.idx)
                if keyfn and 0 <= v.idx < len(self.exports):
                    return {'@ref': keyfn(v.idx)}
                return {'@ref': f'{self.exports[v.idx].cls}' if 0 <= v.idx < len(self.exports) else f'?{v.idx}'}
            if isinstance(v, Raw):
                return {'@raw': v.type, 'len': len(v.data), 'md5': hashlib.md5(v.data).hexdigest()[:12],
                        'why': v.why}
            if isinstance(v, tuple):
                k = v[0]
                if k == 'CName':
                    return v[1]
                if k == 'array':
                    return [conv(x) for x in v[2]]
                if k == 'bytes':
                    b = v[1]
                    if b[:4] == b'CR2W':
                        return {'@cr2w': len(b), 'md5': hashlib.md5(b).hexdigest()[:12]}
                    return {'@bytes': len(b), 'md5': hashlib.md5(b).hexdigest()[:12]}
                if k == 'enum':
                    return v[1]
                if k in ('Transform', 'QsTransform'):
                    return {kk: [round(x, 5) for x in vv] for kk, vv in v[1].items()}
                if k == 'TagList':
                    return {'@tags': v[1]}
                if k == 'raw':
                    return conv(v[1])
                if k == 'bitfield':
                    return {'@bits': v[1]}
                if k == 'Variant':
                    return {'@variant': v[1], 'v': conv(v[2])}
                return {'@' + k: v[1] if not isinstance(v[1], bytes) else v[1].hex()}
            if isinstance(v, dict) and '_struct' in v:
                return {n: conv(v['_props'][n][1]) for n in v['_order']}
            if isinstance(v, float):
                return round(v, 5)
            return v

        def exp(i):
            seen.add(i)
            e = self.exports[i]
            out = {'@class': e.cls}
            for n in e.order:
                out[n] = conv(e.props[n][1])
            if e.trailing_decoded:
                kind, val = e.trailing_decoded
                if kind == 'objlists':
                    out['@objlists'] = [[conv(x) | {'@class': x['_struct']} for x in lst] for lst in val]
                elif kind == 'matparams':
                    out['@matparams'] = {n: conv(v) for n, t, v in val}
            if e.trailing:
                out['@trailing'] = {'len': len(e.trailing), 'md5': hashlib.md5(e.trailing).hexdigest()[:12]}
            if e.error:
                out['@error'] = e.error
            kids = [k for k in e.children if k not in seen]
            if kids and not flat:
                out['@children'] = [exp(k) for k in kids if k not in seen]
            return out

        if flat:
            return {keyfn(e.i): exp(e.i) for e in self.exports}
        out = [exp(i) for i in roots if i not in seen]
        # anything not reachable
        for e in self.exports:
            if e.i not in seen:
                out.append(exp(e.i))
        return out

    def export_keys(self):
        """Stable identity per export: class + guid/GUID/name-like prop, else parent key + class ordinal."""
        keys = {}

        def ident(e):
            for n in ('guid', 'GUID', 'eventGuid', 'elementID', 'sectionId', 'name', 'componentName', 'entryID', 'id'):
                if n in e.props:
                    v = e.props[n][1]
                    if isinstance(v, tuple):
                        v = v[1]
                    if isinstance(v, dict) and '_struct' in v:
                        v = json.dumps({k: str(v['_props'][k][1]) for k in v['_order']})
                    if v not in (None, '', 0):
                        return f'{n}={v}'
            return None

        counts = collections.Counter()
        used = set()
        for e in self.exports:
            idv = ident(e)
            if idv:
                k = f'{e.cls}[{idv}]'
            else:
                pk = keys.get(e.parent, 'root') if e.parent >= 0 else 'root'
                c = (pk, e.cls)
                k = f'{pk}/{e.cls}#{counts[c]}'
                counts[c] += 1
            if k in used:
                n = 1
                while f'{k}~{n}' in used:
                    n += 1
                k = f'{k}~{n}'
            used.add(k)
            keys[e.i] = k
        # ordinal-keyed exports referenced by exactly one identified export: key them by that reference
        refs = collections.defaultdict(list)
        for e in self.exports:
            for n in e.order:
                v = e.props[n][1]
                items = v[2] if isinstance(v, tuple) and v[0] == 'array' else [v]
                for j, x in enumerate(items):
                    if isinstance(x, Ref) and x.kind == 'export':
                        refs[x.idx].append((e.i, n, j))
        for _ in range(3):
            for i, k in list(keys.items()):
                if '#' in k.split('/')[-1] and len(refs.get(i, [])) == 1:
                    src, n, j = refs[i][0]
                    sk = keys[src]
                    if '#' in sk.split('/')[-1] or src == i:
                        continue
                    nk = f'{sk}.{n}[{j}]'
                    if nk not in used:
                        used.discard(k)
                        used.add(nk)
                        keys[i] = nk
        return keys

    def flat(self):
        keys = self.export_keys()
        return self.tree(inline=False, keyfn=lambda i: keys[i], flat=True)

    def summary(self):
        return dict(version=self.version, names=len(self.names), imports=len(self.imports),
                    exports=len(self.exports), buffers=len(self.buffers), embedded=len(self.embedded),
                    file_size=self.file_size, len=len(self.d),
                    raw_props=sum(1 for e in self.exports for n in e.order if isinstance(e.props[n][1], Raw)),
                    export_errors=sum(1 for e in self.exports if e.error))


def iter_raw(f):
    """All (export, prop name, Raw) fallbacks in a file, for coverage checks."""
    for e in f.exports:
        for n in e.order:
            v = e.props[n][1]
            if isinstance(v, Raw):
                yield e, n, v


if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    f = CR2WFile(data, name=sys.argv[1])
    print(json.dumps(f.summary()))
    for e, n, v in iter_raw(f):
        print('RAW', e.cls, n, v.type, len(v.data), v.why)
    if '--json' in sys.argv:
        out = sys.argv[sys.argv.index('--json') + 1]
        json.dump(f.tree(), open(out, 'w'), indent=1, default=str)
