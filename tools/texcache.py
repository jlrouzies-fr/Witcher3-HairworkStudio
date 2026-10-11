"""
texcache - Witcher 3 texture.cache reader (v6 legacy mods, v7 game 5.0) and v6 writer.

Footer (last 32 bytes): u64 crc, u32 usedPages, u32 entryCount, u32 stringTableSize, u32 mipTableCount, 'HCXT', u32 ver.
Before the footer: u32 mipOffsets[mipTableCount], string table, entries[entryCount] (52 bytes each).
Entry: u32 hash, i32 nameOffset, u32 page (4 KiB units), u32 zsize, u32 size, u32 align, u16 w, u16 h,
       u16 mips, u16 slices, i32 mipOffsetIndex, i32 numMipOffsets, u64 timestamp, u8 type, u8 fmt?, u16 flags.
Each entry's data at page*4096: u32 zsize?, u32 size, u8 ?, then zlib stream (DXT pixels for GUI images).
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.environ.get("HWFA_GAME") or os.path.dirname(os.path.dirname(HERE))   # HWFA_GAME: game folder of a release install
GAME_CACHE = os.path.join(GAME, 'content', 'content0', 'texture.cache')


class Entry:
    __slots__ = ('raw', 'name', 'hash', 'page', 'zsize', 'size', 'w', 'h', 'mips')

    def __init__(self, raw, strs):
        self.raw = raw
        self.hash, no, self.page, self.zsize, self.size, _al, self.w, self.h, self.mips = \
            struct.unpack_from('<IiIIIIHHH', raw)
        self.name = strs[no:strs.index(b'\0', no)].decode('latin1')


class TextureCache:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(size - 32)
            self.crc, self.used, n, strsz, mipn, magic, self.version = struct.unpack('<QIIII4sI', f.read(32))
            assert magic == b'HCXT'
            tail = n * 52 + strsz + mipn * 4
            f.seek(size - 32 - tail)
            t = f.read(tail)
        self.mips = struct.unpack_from('<%dI' % mipn, t)
        strs = t[mipn * 4:mipn * 4 + strsz]
        p = mipn * 4 + strsz
        self.entries = [Entry(t[p + 52 * i:p + 52 * i + 52], strs) for i in range(n)]
        self.by_name = {e.name.lower(): e for e in self.entries}

    def read_blob(self, e):
        """Raw stored blob (header + compressed data) for a v6 entry (no mip table)."""
        with open(self.path, 'rb') as f:
            f.seek(e.page * 4096)
            return f.read(e.zsize + 9)


def name_hash(name):
    """Entry hash: Java-style h = h*31 + c over the stored path (verified on VladimirUI's caches)."""
    h = 0
    for c in name.encode('latin1'):
        h = (h * 31 + c) & 0xFFFFFFFF
    return h


def write_v6(path, items):
    """
    Write a v6 texture.cache. items: list of (src TextureCache (v6), src name, dst name or None to keep).
    Pixel data is copied as stored; renamed entries get a recomputed hash.
    """
    strings = bytearray()
    entries = bytearray()
    names = []
    with open(path, 'wb') as out:
        for src, nm, dst in items:
            e = src.by_name[nm.lower()]
            assert src.version == 6 and not src.mips, 'only v6 sources without mip tables'
            dst = dst or e.name
            blob = src.read_blob(e)
            page = out.tell() // 4096
            out.write(blob)
            out.write(bytes(-out.tell() % 4096))
            raw = bytearray(e.raw)
            struct.pack_into('<IiI', raw, 0, name_hash(dst), len(strings), page)
            strings += dst.encode('latin1') + b'\0'
            entries += raw
            names.append(dst)
        used = out.tell() // 4096
        out.write(strings)
        out.write(entries)
        # crc: VladimirUI's caches carry a crc that 5.0 evidently doesn't check (tiles load); write 0
        out.write(struct.pack('<QIIII4sI', 0, used, len(names), len(strings), 0, b'HCXT', 6))
    return names


if __name__ == '__main__':
    tc = TextureCache(sys.argv[1])
    print(f'v{tc.version} {len(tc.entries)} entries')
    for e in tc.entries[:int(sys.argv[2]) if len(sys.argv) > 2 else 20]:
        print(f'  {e.name} {e.w}x{e.h}')
