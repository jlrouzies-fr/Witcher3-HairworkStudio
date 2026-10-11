"""
w3lib - minimal Witcher 3 file-format helpers (bundles, CR2W, metadata.store).

Written for game version 5.0 ("Remastered", CR2W v164, bundle TOC v5, metadata.store v7),
while *writing* the legacy formats used by local mods (bundle v3 "POTATO70" + metadata.store v6),
the same formats REDkit / WolvenKit produce for 4.0x mods.
"""
import os
import struct
import zlib

import lz4.block

# ----------------------------------------------------------------------------------------------
# helpers


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for c in data:
        h ^= c
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def read_vlq(d, p):
    """CDPR 'compressed int': byte0 = sign(0x80) | more(0x40) | 6 bits, next bytes = more(0x80) | 7 bits."""
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


def write_vlq(val):
    neg = val < 0
    val = abs(val)
    first = val & 0x3F
    val >>= 6
    out = bytearray()
    out.append(first | (0x80 if neg else 0) | (0x40 if val else 0))
    while val:
        b = val & 0x7F
        val >>= 7
        out.append(b | (0x80 if val else 0))
    return bytes(out)


def decompress(z, size, comp):
    if comp == 0:
        return z
    if comp == 1:
        return zlib.decompress(z)
    if comp in (4, 5):
        return lz4.block.decompress(z, uncompressed_size=size)
    raise ValueError(f'unsupported compression {comp}')


# ----------------------------------------------------------------------------------------------
# bundles


class BundleEntry:
    def __init__(self, name, size, zsize, offset, comp):
        self.name, self.size, self.zsize, self.offset, self.comp = name, size, zsize, offset, comp


def bundle_entries(path):
    """List entries of a bundle. Supports legacy (v3, 0x140 TOC entries) and 5.0 (v5, 0x130 entries, 64-bit offsets)."""
    with open(path, 'rb') as f:
        hdr = f.read(0x20)
        if hdr[:8] != b'POTATO70':
            raise ValueError(f'{path}: not a bundle')
        _bsize, _dummy, toc = struct.unpack_from('<III', hdr, 8)
        ver, = struct.unpack_from('<H', hdr, 0x14)
        t = f.read(toc)
    out = []
    if ver >= 5:
        for off in range(0, toc, 0x130):
            name = t[off:off + 0x100].split(b'\0')[0].decode('latin1')
            doff, size, zsize = struct.unpack_from('<QII', t, off + 0x110)
            comp, = struct.unpack_from('<I', t, off + 0x124)
            if name:
                out.append(BundleEntry(name, size, zsize, doff, comp))
    else:
        for off in range(0, toc, 0x140):
            name = t[off:off + 0x100].split(b'\0')[0].decode('latin1')
            size, zsize, doff = struct.unpack_from('<III', t, off + 0x114)
            comp, = struct.unpack_from('<I', t, off + 0x13C)
            if name:
                out.append(BundleEntry(name, size, zsize, doff, comp))
    return out


def bundle_read(path, entry):
    with open(path, 'rb') as f:
        f.seek(entry.offset)
        z = f.read(entry.zsize)
    return decompress(z, entry.size, entry.comp)


def find_in_bundles(bundle_dir, rel_path):
    """Return (bundle_path, entry) for the first bundle containing rel_path (case-insensitive)."""
    rel = rel_path.replace('/', '\\').lower()
    for name in sorted(os.listdir(bundle_dir)):
        if not name.endswith('.bundle') or name in ('movies.bundle', 'buffers.bundle', 'texture.cache'):
            continue
        p = os.path.join(bundle_dir, name)
        for e in bundle_entries(p):
            if e.name.lower() == rel:
                return p, e
    return None, None


LEGACY_ALIGN = 0x1000
LEGACY_HDR_TAIL = bytes.fromhex('0300010000131313131313131313')[:12]  # bytes 0x14..0x1F as written by REDkit/WolvenKit


def write_legacy_bundle(path, files):
    """
    Write a legacy (v3) bundle. files: list of (rel_path, data bytes). Compressed with LZ4HC (type 5).
    Returns list of dicts describing each stored file (for metadata.store generation).
    """
    toc_size = 0x140 * len(files)
    data_start = 0x20 + toc_size
    offset = (data_start + LEGACY_ALIGN - 1) // LEGACY_ALIGN * LEGACY_ALIGN
    blobs, stored = [], []
    for rel, data in files:
        z = lz4.block.compress(data, mode='high_compression', compression=12, store_size=False)
        comp = 5
        if len(z) >= len(data):
            z, comp = data, 0
        stored.append(dict(name=rel, size=len(data), zsize=len(z), offset=offset, comp=comp, crc=zlib.crc32(data)))
        blobs.append((offset, z))
        offset = (offset + len(z) + LEGACY_ALIGN - 1) // LEGACY_ALIGN * LEGACY_ALIGN
    bundle_size = offset  # aligned end, as REDkit writes it
    toc = bytearray()
    for s in stored:
        e = bytearray(0x140)
        nm = s['name'].encode('latin1')
        e[:len(nm)] = nm
        struct.pack_into('<III', e, 0x114, s['size'], s['zsize'], s['offset'])
        struct.pack_into('<II', e, 0x138, s['crc'], s['comp'])
        toc += e
    with open(path, 'wb') as f:
        f.write(b'POTATO70' + struct.pack('<III', bundle_size, 0, toc_size) + LEGACY_HDR_TAIL)
        f.write(toc)
        for off, z in blobs:
            f.seek(off)
            f.write(z)
    return dict(name=os.path.basename(path), bundle_size=bundle_size, data_start=data_start, files=stored)


def write_legacy_metadata_store(path, bundles):
    """
    Write a v6 metadata.store for the given legacy bundles (list of dicts returned by write_legacy_bundle).
    Buffer files (*.N.buffer): put them in a LATER bundle than their owner; the owner's FileInfo gets firstBuffer /
    numBuffers and the `buffers` table lists them (layout checked against modCiri, REDkit-made).
    Layout reverse-engineered from REDkit output (and verified by round-tripping a REDkit-made mod):
      magic '\\x03VTM', u32 version=6, u32 maxZSize, u32 maxSize,
      vlq+strings, vlq+FileInfo[32], vlq+FileEntry[20], vlq+BundleInfo[24], vlq+u32 buffers[],
      vlq+Dir[8], vlq+FileDir[12], vlq+Hash[16]
    """
    strings = bytearray(b'\0')

    def add_str(s):
        o = len(strings)
        strings.extend(s.encode('latin1') + b'\0')
        return o

    files = []  # (bundle_idx, file dict)
    bundle_name_off = []
    file_path_off = []
    for bi, b in enumerate(bundles):
        bundle_name_off.append(add_str(b['name']))
        for fd in b['files']:
            files.append((bi + 1, fd))
            file_path_off.append(add_str(fd['name']))
    # directory tree (buffer files `<owner>.N.buffer` only get FileInfo/FileEntry rows + the owner's buffers list,
    # like REDkit's output: they are not in the dir / hash tables)
    import re
    is_buf = [re.search(r'\.\d+\.buffer$', fd['name']) is not None for (_b, fd) in files]
    by_name = {fd['name'].lower(): i for i, (_b, fd) in enumerate(files)}
    dirs = [('', 0)]  # (name, parent)
    dir_index = {(): 0}
    file_dir = []     # (fileinfo index, dir, filename)
    for i, (_bi, fd) in enumerate(files):
        if is_buf[i]:
            continue
        parts = fd['name'].split('\\')
        key = ()
        for part in parts[:-1]:
            nkey = key + (part,)
            if nkey not in dir_index:
                dir_index[nkey] = len(dirs)
                dirs.append((part, dir_index[key]))
            key = nkey
        file_dir.append((i + 1, dir_index[key], parts[-1]))
    # REDkit stores the root dir name as an empty string placed right after the file paths
    dir_name_off = [add_str('')] + [add_str(n) for (n, _p) in dirs[1:]]
    file_name_off = [add_str(fn) for (_i, _d, fn) in file_dir]
    # buffers: per owner file, its .1.buffer, .2.buffer ... in order (fileinfo indices, 1-based)
    buffers, buf_first = [], {}
    for i, (_bi, fd) in enumerate(files):
        if is_buf[i]:
            continue
        n = 1
        while (fd['name'].lower() + '.%d.buffer' % n) in by_name:
            buf_first.setdefault(i, (len(buffers), 0))
            buffers.append(by_name[fd['name'].lower() + '.%d.buffer' % n] + 1)
            buf_first[i] = (buf_first[i][0], buf_first[i][1] + 1)
            n += 1

    out = bytearray()
    max_z = max(fd['zsize'] for _b, fd in files)
    max_s = max(fd['size'] for _b, fd in files)
    out += b'\x03VTM' + struct.pack('<III', 6, max_z, max_s)
    out += write_vlq(len(strings)) + strings
    # FileInfo: pathOffset, 0, zsize, size, firstEntry, compression, firstBuffer, numBuffers
    out += write_vlq(len(files) + 1) + bytes(32)
    for i, (_bi, fd) in enumerate(files):
        fb, nb = buf_first.get(i, (0, 0))
        out += struct.pack('<8I', file_path_off[i], 0, fd['zsize'], fd['size'], i + 1, fd['comp'], fb, nb)
    # FileEntry: fileIndex, bundleIndex, offset, zsize, next
    out += write_vlq(len(files) + 1) + bytes(20)
    for i, (bi, fd) in enumerate(files):
        out += struct.pack('<5I', i + 1, bi, fd['offset'], fd['zsize'], 0)
    # BundleInfo: nameOffset, firstFileEntry, numFileEntries, dataBlockSize, dataBlockOffset, burst
    out += write_vlq(len(bundles) + 1) + bytes(24)
    first = 1
    for bi, b in enumerate(bundles):
        n = len(b['files'])
        out += struct.pack('<6I', bundle_name_off[bi], first, n, b['bundle_size'] - b['data_start'], b['data_start'], 0)
        first += n
    out += write_vlq(len(buffers)) + b''.join(struct.pack('<I', x) for x in buffers)
    out += write_vlq(len(dirs))
    for i, (_n, parent) in enumerate(dirs):
        out += struct.pack('<Ii', dir_name_off[i], parent)
    out += write_vlq(len(file_dir))
    for i, (fi, d, _fn) in enumerate(file_dir):
        out += struct.pack('<III', fi, d, file_name_off[i])
    hashes = sorted((fnv1a64(fd['name'].lower().encode('latin1')), i + 1) for i, (_b, fd) in enumerate(files)
                    if not is_buf[i])
    out += write_vlq(len(hashes))
    for h, idx in hashes:
        out += struct.pack('<QQ', h, idx)
    with open(path, 'wb') as f:
        f.write(out)


def read_metadata_store_v6(path):
    """Parse a v6 metadata.store into python structures (used to validate the writer)."""
    d = open(path, 'rb').read()
    assert d[:4] == b'\x03VTM'
    ver, max_z, max_s = struct.unpack_from('<III', d, 4)
    p = 16
    n, p = read_vlq(d, p)
    strings = d[p:p + n]
    p += n
    r = dict(version=ver, max_z=max_z, max_s=max_s, strings=strings)
    for key, size, fmt in (('fileinfo', 32, '<8I'), ('fileentry', 20, '<5I'), ('bundle', 24, '<6I'),
                           ('buffers', 4, '<I'), ('dirs', 8, '<Ii'), ('filedirs', 12, '<III'), ('hashes', 16, '<QQ')):
        n, p = read_vlq(d, p)
        r[key] = [struct.unpack_from(fmt, d, p + size * i) for i in range(n)]
        p += size * n
    r['end'] = p
    r['len'] = len(d)
    return r


# ----------------------------------------------------------------------------------------------
# CR2W


class CR2W:
    """Read-only view + in-place patching of a CR2W file (Witcher 3)."""

    def __init__(self, data: bytes):
        self.d = bytearray(data)
        d = self.d
        if d[:4] != b'CR2W':
            raise ValueError('not a CR2W file')
        self.version, = struct.unpack_from('<I', d, 4)
        self.tables = [list(struct.unpack_from('<III', d, 0x28 + i * 12)) for i in range(10)]
        soff, ssize, _ = self.tables[0]
        self.strings = bytes(d[soff:soff + ssize])
        noff, ncount, _ = self.tables[1]
        self.names = []
        for i in range(ncount):
            so, _h = struct.unpack_from('<II', d, noff + i * 8)
            self.names.append(self._str(so))
        eoff, ecount, _ = self.tables[4]
        self.exports = []
        for i in range(ecount):
            cls, _fl, parent, size, off, _tmpl, _crc = struct.unpack_from('<HHIIIII', d, eoff + i * 24)
            self.exports.append(dict(cls=self.names[cls], parent=parent - 1, size=size, off=off))

    def _str(self, o):
        e = self.strings.index(b'\0', o)
        return self.strings[o:e].decode('utf-8', 'replace')

    def props(self, i):
        """Generic property list of export i: list of (name, type, value_offset, value_len)."""
        e = self.exports[i]
        d = self.d
        p, end = e['off'], e['off'] + e['size']
        out = []
        if d[p] != 0:
            return out
        p += 1
        while p + 2 <= end:
            ni, = struct.unpack_from('<H', d, p)
            if ni == 0:
                break
            ti, sz = struct.unpack_from('<HI', d, p + 2)
            out.append((self.names[ni], self.names[ti], p + 8, sz - 4))
            p += 4 + sz
        return out

    def prop(self, i, name):
        for n, t, o, ln in self.props(i):
            if n == name:
                return t, o, ln
        return None

    def get_ptr(self, i, name):
        r = self.prop(i, name)
        if not r:
            return -1
        return struct.unpack_from('<i', self.d, r[1])[0] - 1

    def get_ptr_array(self, i, name):
        r = self.prop(i, name)
        if not r:
            return []
        cnt, = struct.unpack_from('<I', self.d, r[1])
        return [struct.unpack_from('<i', self.d, r[1] + 4 + 4 * k)[0] - 1 for k in range(cnt)]

    def set_ptr_array_item(self, i, name, k, target):
        t, o, ln = self.prop(i, name)
        cnt, = struct.unpack_from('<I', self.d, o)
        assert 0 <= k < cnt
        struct.pack_into('<i', self.d, o + 4 + 4 * k, target + 1)

    def get_cname(self, i, name):
        r = self.prop(i, name)
        if not r:
            return None
        return self.names[struct.unpack_from('<H', self.d, r[1])[0]]

    def get_floats(self, i, name):
        r = self.prop(i, name)
        if not r:
            return None
        cnt, = struct.unpack_from('<I', self.d, r[1])
        return list(struct.unpack_from('<%df' % cnt, self.d, r[1] + 4))

    def replace_export_data(self, i, new_chunk: bytes):
        """
        Replace export i's whole data chunk (may change size). Later chunks are shifted, the header's
        fileSize/bufferSize (0x18/0x1C) updated and all CRCs recomputed. Only for files without buffers.
        """
        d = self.d
        e = self.exports[i]
        assert self.tables[5][1] == 0, 'files with buffers are not supported'
        old_off, old_size = e['off'], e['size']
        delta = len(new_chunk) - old_size
        d[old_off:old_off + old_size] = new_chunk
        eoff, _ecount, _ = self.tables[4]
        for k, x in enumerate(self.exports):
            if k == i:
                x['size'] = len(new_chunk)
            elif x['off'] > old_off:
                x['off'] += delta
            struct.pack_into('<II', d, eoff + k * 24 + 8, x['size'], x['off'])
        for field in (0x18, 0x1C):
            v, = struct.unpack_from('<I', d, field)
            struct.pack_into('<I', d, field, v + delta)
        self.fix_crcs()

    def fix_crcs(self):
        """Recompute export chunk CRCs, table CRCs and the header CRC after in-place edits."""
        d = self.d
        eoff, ecount, _ = self.tables[4]
        for i, e in enumerate(self.exports):
            struct.pack_into('<I', d, eoff + i * 24 + 20, zlib.crc32(d[e['off']:e['off'] + e['size']]))
        entry_size = {0: 1, 1: 8, 2: 8, 3: 16, 4: 24, 5: 24, 6: 32}
        for ti, (off, cnt, _crc) in enumerate(self.tables):
            if cnt == 0:
                continue
            crc = zlib.crc32(d[off:off + cnt * entry_size[ti]])
            self.tables[ti][2] = crc
            struct.pack_into('<I', d, 0x28 + ti * 12 + 8, crc)
        h = bytearray(d[:0xA0])
        struct.pack_into('<I', h, 0x20, 0xDEADBEEF)
        struct.pack_into('<I', d, 0x20, zlib.crc32(bytes(h)))

    def verify_crcs(self):
        d = self.d
        eoff, ecount, _ = self.tables[4]
        bad = 0
        for i, e in enumerate(self.exports):
            if zlib.crc32(d[e['off']:e['off'] + e['size']]) != struct.unpack_from('<I', d, eoff + i * 24 + 20)[0]:
                bad += 1
        h = bytearray(d[:0xA0])
        stored, = struct.unpack_from('<I', h, 0x20)
        struct.pack_into('<I', h, 0x20, 0xDEADBEEF)
        return bad, stored == zlib.crc32(bytes(h))
