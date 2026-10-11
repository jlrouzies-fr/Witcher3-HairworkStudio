r"""texread - decode a world texture (.xbm) of the game's v7 texture.cache to a PIL image (mip 0).

  img = load_xbm(r'characters\models\common\textures\hair\hair_long_04_d01.xbm')   # RGBA PIL image
Stored blob at page*4096: u32 zsize, u32 size, u8 ?, then one zlib stream per mip (mip 0 first).
Format from the decompressed size: w*h/2 = DXT1, w*h = DXT5 (BC3) or BC5/ATI2 for normal maps (decoded as DXT5 here,
use the green/alpha channels with care).
"""
import io, os, struct, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import texcache as T
from PIL import Image

_cache = None


def cache():
    global _cache
    if _cache is None:
        _cache = T.TextureCache(T.GAME_CACHE)
    return _cache


def raw_mip0(rel):
    c = cache()
    e = c.by_name[rel.lower()]
    with open(c.path, 'rb') as f:
        f.seek(e.page * 4096)
        blob = f.read(e.zsize + 64)
    d = zlib.decompressobj()
    data = d.decompress(blob[9:])
    return e, data


def dds_header(w, h, fourcc, linear):
    hdr = struct.pack('<4sIIIIIII44xII4sIIIIIIIIII', b'DDS ', 124, 0x81007, h, w, linear, 0, 1,
                      32, 4, fourcc, 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
    return hdr


def load_xbm(rel):
    e, data = raw_mip0(rel)
    w, h = e.w, e.h
    if len(data) >= w * h:
        fc, n = b'DXT5', w * h
    else:
        fc, n = b'DXT1', w * h // 2
    dds = dds_header(w, h, fc, n) + data[:n]
    return Image.open(io.BytesIO(dds)).convert('RGBA')


if __name__ == '__main__':
    im = load_xbm(sys.argv[1])
    im.save(sys.argv[2])
    print(im.size, sys.argv[2])
