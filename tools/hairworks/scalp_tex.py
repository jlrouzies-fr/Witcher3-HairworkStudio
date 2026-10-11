r"""scalp_tex - head diffuse d01 override for NPC hair work (texture only, hair untouched).

Characters (--char, default triss): triss = scalp RECOLOUR (below); sheala = soft HAIRLINE PAINT (`hairline`, see
HAIRWORK.md "Hairline paint (Sheala)"); vesemir = scalp recolour white -> grey (2048 d01, lightness moved too, see
HAIRWORK.md "Vesemir scalp recolour"). Per-character data = CHARS table; build dir build\scalp (triss) or build\scalp_<char>.
  py tools\hairworks\scalp_tex.py build --char vesemir           -> build\scalp_vesemir\modVesemirScalp
  py tools\hairworks\scalp_tex.py hairline --char sheala        paint previews (full, forehead crop, alpha) + hairline_alpha.npy
  py tools\hairworks\scalp_tex.py build --char sheala [--identity] [--front 3 --back 6 --op_line .6 --op_back .85 ...]
  (extract / build / verify / install / uninstall take --char too)

Triss commands:

  py tools\hairworks\scalp_tex.py extract            game d01 -> build\scalp\orig\ (raw mips, stub, d01.png)
  py tools\hairworks\scalp_tex.py mask               UV-island x hair-paint mask -> build\scalp\mask*.png/.npy
  py tools\hairworks\scalp_tex.py recolour           recoloured image + previews (before/after, scalp crop)
  py tools\hairworks\scalp_tex.py build [--identity] build\scalp\modTrissScalp\content (blob0.bundle, metadata.store, texture.cache)
  py tools\hairworks\scalp_tex.py verify [dir]       re-read a built mod's cache + stub, compare pixel for pixel to the game's
  py tools\hairworks\scalp_tex.py install | uninstall   copy to / remove <game>\mods\modTrissScalp (refuses while witcher3.exe runs)

Recolour: OKLab, luminance kept, (a,b) moved from the painted crimson to the HairWorks hair colour (root..tip mix),
the paint's own chroma variation kept (scaled by the chroma ratio) so the strand pattern survives. Weight = UV mask
(head mesh triangles, dilated + feathered, minus the mouth island / eyebrow area) x hair-paint colour gate.
Every 4x4 DXT1 block the weight does not touch keeps the game's ORIGINAL block bytes (face bit-identical).
Texture cache entry (v6, from modCiri + the game's own v7 entry): one block per streamed mip `u32 zsize, u32 rawsize,
u8 blocks_left, zlib(raw DXT1)`, then one block with the resident mips (7..12 = 696 B); the mip table holds the
cumulative END offsets of those blocks.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import zlib

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
ROOT = os.path.dirname(TOOLS)
GAME = os.environ.get("HWFA_GAME") or os.path.dirname(ROOT)   # HWFA_GAME: game folder of a release install
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)
import texcache as T
import w3lib

# Per-character data. HairWorks colours (game sRGB 0..255): Triss triss_l1.json root (50,30,22) / tip (62,40,30);
# Sheala chars\sheala_v8.json root (19,18,17) / tip (23,22,21) (hair mesh c_01_wa__sheala, component
# c_07_ma__emhyr_hair, appearance __q310_tortured). Build dirs: triss build\scalp (unchanged), others build\scalp_<char>.
CHARS = {
    'triss': dict(
        mode='recolour',
        rel=r'characters\models\main_npc\triss\h_01_wa__triss\h_01_wa__triss_d01.xbm',
        mesh=r'characters\models\main_npc\triss\h_01_wa__triss\h_01_wa__triss.w2mesh',
        mod='modTrissScalp', build='scalp', root=(50, 30, 22), tip=(62, 40, 30), prev='triss_scalp'),
    'sheala': dict(
        mode='hairline',
        rel=r'characters\models\main_npc\sheala\h_01_wa__sheala\h_01_wa__sheala_d01.xbm',
        mesh=r'characters\models\main_npc\sheala\h_01_wa__sheala\h_01_wa__sheala.w2mesh',
        mod='modShealaHairline', build='scalp_sheala', root=(19, 18, 17), tip=(23, 22, 21), prev='sheala_hairline',
        config='sheala_v8',
        # visual hair colour at her hairline in game (results\L81\pairs\sheala_*front_PAIR.jpg, left half = vanilla,
        # mean of the hairline pixels = (30,29,27)); the HairWorks table colour 19..23 renders about that after lighting
        paint=(30, 29, 27)),
    # Vesemir (2026-10-10, opt-in, separate mod; vesemir_r31a untouched): his head d01 paints the skull a light
    # grey-white (vanilla card hair is bright); our r31a fur = card texture x .40 is darker, so the scalp reads WHITE
    # through gaps ("white scalp again on the back", R33). Recolour = OKLab: chroma -> target, LIGHTNESS -> target too
    # (`lum_to`), variation kept, so the white paint becomes grey stubble / skin. d01 is 2048 x 2048, 12 mips,
    # resident 6 (6 streamed), stub 1389 B.
    # Target grey: r31a colour = card texture x textureBrightness .40 (root/tip [200,192,182]/[228,222,212] do NOT
    # tint with use_card_texture, HAIRWORK.md 7.2). hair_long_04_d01 mean RGB where alpha > .5 = [128.5,123.7,120.9]
    # -> fur diffuse [51,49,48]. Scalp paint = [108,108,106] (2.1x brighter -> reads white). Calibrated on the R31
    # capture analysis\hairworks_vesemir\captures\20261002_173322_r31a_shot3_head.png (_modCreation): forehead skin
    # diffuse luma 137 renders ~203 (gain ~1.38), our fur on top renders median ~[92,88,86]; scalp diffuse for the same
    # on-screen grey = 92 / 1.38 = ~67 -> target [68,66,64] (card hue, = fur diffuse x 1.33). Paint 108 renders ~150
    # = the "white scalp". --mix is moot (root = tip).
    'vesemir': dict(
        mode='recolour',
        rel=r'characters\models\main_npc\vesemir\h_01_mb__vesemir\h_01_mb__vesemir_d01.xbm',
        mesh=r'characters\models\main_npc\vesemir\h_01_mb__vesemir\h_01_mb__vesemir.w2mesh',
        mod='modVesemirScalp', build='scalp_vesemir', prev='vesemir_scalp',
        w=2048, nmip=12, nstream=6,
        # 2026-10-10 user (L226 'almost no improvement'): ~38 % darker than [68,66,64], same hue; temples 100 % (L228)
        root=(42, 41, 40), tip=(42, 41, 40), temple_strength=1.0,
        lum_to=True, gate='grey', regions='vesemir', config='vesemir_r31a'),
}
import tempfile
_SCRATCH = os.path.join(tempfile.gettempdir(), 'hairwork_scratch')
W = H = 4096
NMIP = 13
NSTREAM = 7
RES_BYTES = 696
CHAR = REL = MESH = MODNAME = BUILD = ORIG = MODDIR = PREV = ROOT_RGB = TIP_RGB = None


def set_char(name):
    """select the per-character globals used by every command"""
    global CHAR, REL, MESH, MODNAME, BUILD, ORIG, MODDIR, PREV, ROOT_RGB, TIP_RGB, W, H, NMIP, NSTREAM
    CHAR = CHARS[name]
    W = H = CHAR.get('w', 4096)
    NMIP = CHAR.get('nmip', 13)
    NSTREAM = CHAR.get('nstream', 7)
    REL, MESH, MODNAME = CHAR['rel'], CHAR['mesh'], CHAR['mod']
    BUILD = os.path.join(ROOT, 'build', CHAR['build'])
    ORIG = os.path.join(BUILD, 'orig')
    MODDIR = os.path.join(BUILD, MODNAME)
    PREV = os.environ.get('SCALP_PREVIEW_DIR') or os.path.join(_SCRATCH, CHAR['prev'])
    ROOT_RGB, TIP_RGB = CHAR['root'], CHAR['tip']


set_char('triss')

DEF = dict(mix=0.35,        # 0 = root colour, 1 = tip colour
           keep=1.0,        # 1 = scale the paint's chroma variation by the chroma ratio, 0 = flat target chroma
           lum=1.0,         # luminance factor (1 = keep)
           dilate=64,       # px at 4096: how far past the mesh triangles the zone reaches
           feather=24)      # px


# ---------------------------------------------------------------------------------------------- DXT1

def _e565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


def dxt1_decode_blocks(raw, nbx, nby):
    """raw bytes -> (nby*4, nbx*4, 3) uint8"""
    b = np.frombuffer(raw, dtype=np.uint8).reshape(nby, nbx, 8)
    c0 = b[..., 0].astype(np.uint16) | (b[..., 1].astype(np.uint16) << 8)
    c1 = b[..., 2].astype(np.uint16) | (b[..., 3].astype(np.uint16) << 8)
    idx = (b[..., 4].astype(np.uint32) | (b[..., 5].astype(np.uint32) << 8) | (b[..., 6].astype(np.uint32) << 16)
           | (b[..., 7].astype(np.uint32) << 24))
    p0, p1 = _e565(c0), _e565(c1)
    four = (c0 > c1)[..., None]
    pal = np.zeros((nby, nbx, 4, 3), np.float32)
    pal[..., 0, :] = p0
    pal[..., 1, :] = p1
    pal[..., 2, :] = np.where(four, (2 * p0 + p1) / 3, (p0 + p1) / 2)
    pal[..., 3, :] = np.where(four, (p0 + 2 * p1) / 3, 0)
    sh = (np.arange(16, dtype=np.uint32) * 2)
    ii = ((idx[..., None] >> sh) & 3).astype(np.int64)                    # (nby,nbx,16)
    px = np.take_along_axis(pal, ii[..., None].repeat(3, -1), axis=2)      # (nby,nbx,16,3)
    px = np.floor(px + 0.5).astype(np.uint8).reshape(nby, nbx, 4, 4, 3).transpose(0, 2, 1, 3, 4)
    return px.reshape(nby * 4, nbx * 4, 3)


def _q565(c):
    c = np.clip(np.floor(c + 0.5), 0, 255).astype(np.int64)
    r = np.clip((c[..., 0] * 31 + 127) // 255, 0, 31)
    g = np.clip((c[..., 1] * 63 + 127) // 255, 0, 63)
    b = np.clip((c[..., 2] * 31 + 127) // 255, 0, 31)
    return ((r << 11) | (g << 5) | b).astype(np.int64)


def dxt1_encode(px):
    """px (N,16,3) float 0..255 -> (N,8) uint8 blocks. PCA endpoints + 2 least-squares refinements, 4-colour mode."""
    N = px.shape[0]
    px = px.astype(np.float64)
    mean = px.mean(1, keepdims=True)
    d = px - mean
    cov = np.einsum('npi,npj->nij', d, d)
    _w, v = np.linalg.eigh(cov)
    ax = v[:, :, -1]                                                       # (N,3) principal axis
    t = np.einsum('npi,ni->np', d, ax)
    lo = mean[:, 0] + ax * t.min(1)[:, None]
    hi = mean[:, 0] + ax * t.max(1)[:, None]
    flat = (t.max(1) - t.min(1)) < 1e-6
    best = None
    for it in range(3):
        q0, q1 = _q565(hi), _q565(lo)
        sw = q0 < q1
        q0, q1 = np.where(sw, q1, q0), np.where(sw, q0, q1)
        e0, e1 = _e565(q0), _e565(q1)
        pal = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1)  # (N,4,3)
        err = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)      # (N,16,4)
        ix = err.argmin(-1)
        tot = np.take_along_axis(err, ix[..., None], 2)[..., 0].sum(1)
        # q0 == q1 -> 3-colour mode in the decoder, only index 0/1/2 valid and idx3 = black: force index 0
        same = q0 == q1
        ix = np.where(same[:, None], 0, ix)
        if best is None:
            best = [tot.copy(), q0.copy(), q1.copy(), ix.copy()]
        else:
            m = tot < best[0]
            best[0] = np.where(m, tot, best[0])
            best[1] = np.where(m, q0, best[1])
            best[2] = np.where(m, q1, best[2])
            best[3] = np.where(m[:, None], ix, best[3])
        # refine: palette weights for indices (0->c0, 1->c1, 2->2/3 c0, 3->1/3 c0) ; c0 = hi side (index 0)
        a = np.array([1.0, 0.0, 2 / 3, 1 / 3])[ix]                         # weight of c0
        bb = 1 - a
        aa, bbv, ab = (a * a).sum(1), (bb * bb).sum(1), (a * bb).sum(1)
        det = aa * bbv - ab * ab
        ok = np.abs(det) > 1e-9
        det = np.where(ok, det, 1.0)
        rA = np.einsum('np,npc->nc', a, px)
        rB = np.einsum('np,npc->nc', bb, px)
        nh = (bbv[:, None] * rA - ab[:, None] * rB) / det[:, None]
        nl = (aa[:, None] * rB - ab[:, None] * rA) / det[:, None]
        hi = np.where(ok[:, None], nh, hi)
        lo = np.where(ok[:, None], nl, lo)
    _tot, q0, q1, ix = best
    same = q0 == q1
    out = np.zeros((N, 8), np.uint8)
    out[:, 0], out[:, 1] = q0 & 255, q0 >> 8
    out[:, 2], out[:, 3] = q1 & 255, q1 >> 8
    # selector bits: our palette order is [c0, c1, 2/3, 1/3]; DXT1 index order is 0=c0, 1=c1, 2=(2c0+c1)/3, 3=(c0+2c1)/3
    word = np.zeros(N, np.uint64)
    for i in range(16):
        word |= ix[:, i].astype(np.uint64) << np.uint64(2 * i)
    for k in range(4):
        out[:, 4 + k] = ((word >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    return out


def blocks_of(img):
    """(h,w,3) -> (nby, nbx, 16, 3) with h, w multiples of 4"""
    h, w = img.shape[:2]
    return img.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h // 4, w // 4, 16, 3)


# ---------------------------------------------------------------------------------------------- cache io

def parse_entry_blocks(path, e, mips):
    """decompress every block of a cache entry -> list of raw byte strings (7 streamed mips + resident)."""
    with open(path, 'rb') as f:
        f.seek(e.page * 4096)
        b = f.read(e.zsize)
    mi, n = struct.unpack_from('<ii', e.raw, 32)
    ends = list(mips[mi:mi + (n & 0xFFFF)]) + [e.zsize]
    out, p = [], 0
    for en in ends:
        zs, rs, left = struct.unpack_from('<IIB', b, p)
        raw = zlib.decompress(b[p + 9:p + 9 + zs])
        assert len(raw) == rs
        out.append(raw)
        p = en
    return out


def mip_layout():
    """[(w, h, nbx, nby, offset in the 13-mip concatenation, nbytes)]"""
    out, off = [], 0
    for k in range(NMIP):
        w, h = max(1, W >> k), max(1, H >> k)
        nbx, nby = max(1, w // 4), max(1, h // 4)
        out.append((w, h, nbx, nby, off, nbx * nby * 8))
        off += nbx * nby * 8
    return out


def split_mips(blocks):
    """7 streamed blocks + resident -> list of 13 raw mip byte strings"""
    mips = list(blocks[:NSTREAM])
    res = blocks[NSTREAM]
    for (w, h, nbx, nby, _o, nb) in mip_layout()[NSTREAM:]:
        mips.append(res[:nb])
        res = res[nb:]
    assert not res
    return mips


def load_orig():
    """game d01: dict(mips=[13 raw], stub=bytes, entry=Entry)"""
    c = T.TextureCache(T.GAME_CACHE)
    e = c.by_name[REL.lower()]
    mips = split_mips(parse_entry_blocks(c.path, e, c.mips))
    import gamefiles as g
    stub = g.get(REL)
    return dict(mips=mips, stub=stub, entry=e)


def decode_mip(raw, k):
    w, h, nbx, nby, _o, _n = mip_layout()[k]
    im = dxt1_decode_blocks(raw, nbx, nby)
    return im[:h, :w] if (w < 4 or h < 4) else im


def pack_entry(mips):
    """13 raw mips -> (blob bytes, mip table ends list). Streamed blocks zlib'ed one each, resident block last."""
    blocks = list(mips[:NSTREAM]) + [b''.join(mips[NSTREAM:])]
    blob = bytearray()
    ends = []
    for i, raw in enumerate(blocks):
        z = zlib.compress(raw, 9)
        blob += struct.pack('<IIB', len(z), len(raw), len(blocks) - 1 - i) + z
        ends.append(len(blob))
    return bytes(blob), ends[:-1]


def write_cache_v6(path, name, blob, ends, total_raw):
    page = 0
    pad = bytes(-len(blob) % 4096)
    used = (len(blob) + len(pad)) // 4096
    strings = name.encode('latin1') + b'\0'
    ent = struct.pack('<IiIIIIHHHHiiQBBH', T.name_hash(name), 0, page, len(blob), total_raw, 16, W, H, NMIP, 1,
                      0, len(ends), 0, 7, 4, 0)
    assert len(ent) == 52
    with open(path, 'wb') as f:
        f.write(blob + pad)
        f.write(struct.pack('<%dI' % len(ends), *ends))
        f.write(strings)
        f.write(ent)
        f.write(struct.pack('<QIIII4sI', 0, used, 1, len(strings), len(ends), b'HCXT', 6))


def make_stub(stub, mips):
    """game stub with the resident mips (7..12) replaced"""
    res = b''.join(mips[NSTREAM:])
    assert len(res) == RES_BYTES
    d = bytearray(stub)
    d[-RES_BYTES:] = res
    c = w3lib.CR2W(bytes(d))
    c.fix_crcs()
    return bytes(c.d)


# ---------------------------------------------------------------------------------------------- colour

def srgb2lin(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin2srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055) * 255.0


_M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                [0.2119034982, 0.6806995451, 0.1073969566],
                [0.0883024619, 0.2817188376, 0.6299787005]])
_M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
                [1.9779984951, -2.4285922050, 0.4505937099],
                [0.0259040371, 0.7827717662, -0.8086757660]])


def rgb2oklab(rgb):
    l = srgb2lin(rgb.astype(np.float64))
    lms = np.cbrt(l @ _M1.T)
    return lms @ _M2.T


def oklab2rgb(lab):
    lms = (lab @ np.linalg.inv(_M2).T) ** 3
    return lin2srgb(lms @ np.linalg.inv(_M1).T)


def smooth(x, a, b):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def hair_weight(rgb):
    """soft 'this pixel is dark crimson hair paint' gate (0 skin / mouth / lip, 1 paint)"""
    r, g, b = [rgb[..., i].astype(np.float32) for i in range(3)]
    rg = smooth(r - g, 26, 40)
    dark = 1 - smooth(r, 118, 140)
    gr = 1 - smooth(g / np.maximum(r, 1), 0.48, 0.60)
    br = 1 - smooth(b / np.maximum(r, 1), 0.40, 0.52)
    return rg * dark * gr * br


# ---------------------------------------------------------------------------------------------- commands

def ensure_dirs():
    os.makedirs(ORIG, exist_ok=True)
    os.makedirs(PREV, exist_ok=True)


def cmd_extract(a):
    ensure_dirs()
    o = load_orig()
    for k, m in enumerate(o['mips']):
        open(os.path.join(ORIG, 'mip%02d.dxt1' % k), 'wb').write(m)
    open(os.path.join(ORIG, 'stub.xbm'), 'wb').write(o['stub'])
    img = decode_mip(o['mips'][0], 0)
    Image.fromarray(img).save(os.path.join(ORIG, 'd01.png'))
    print('extracted', [len(m) for m in o['mips']], 'stub', len(o['stub']))
    return o


def get_orig():
    if not os.path.exists(os.path.join(ORIG, 'mip%02d.dxt1' % (NMIP - 1))):
        return cmd_extract(None)
    return dict(mips=[open(os.path.join(ORIG, 'mip%02d.dxt1' % k), 'rb').read() for k in range(NMIP)],
                stub=open(os.path.join(ORIG, 'stub.xbm'), 'rb').read())


def uv_zone(p):
    """float (H,W) 0..1: head mesh UV triangles (LOD0 chunks), dilated + feathered, region-limited to the hair zones"""
    import gamefiles as g
    from scipy.ndimage import gaussian_filter
    m = g.load_mesh(MESH)
    S = 1024                      # work at 1k, the zone is soft anyway
    im = Image.new('L', (S, S), 0)
    dr = ImageDraw.Draw(im)
    for ci in m['lod0']:
        ch = m['chunks'][ci]
        uv, idx = ch['uv'], ch['idx'].reshape(-1, 3)
        for t in idx:
            pts = [(float(uv[i, 0]) * S, float(uv[i, 1]) * S) for i in t]
            dr.polygon(pts, fill=255)
    z = np.asarray(im, np.float32) / 255
    k = p['dilate'] * S / W
    z = gaussian_filter(z, k * 0.5)
    z = np.clip(z * 4, 0, 1)                          # ~ dilation by k px, soft edge
    z = gaussian_filter(z, p['feather'] * S / W * 0.5)
    u, v = np.meshgrid((np.arange(S) + .5) / S, (np.arange(S) + .5) / S)
    if CHAR.get('regions') == 'vesemir':
        z = z * vesemir_regions(u, v)
        zi = np.asarray(Image.fromarray((z * 255).astype(np.uint8)).resize((W, H), Image.BILINEAR), np.float32) / 255
        return zi
    # region limits (UV): skull islands at the bottom, hairline + temples at the top; mouth island / brows / ears out
    skull = smooth(v, 0.755, 0.785)
    top = (1 - smooth(v, 0.145, 0.175)) * smooth(u, 0.17, 0.20) * (1 - smooth(u, 0.80, 0.83))
    temple = (1 - smooth(v, 0.28, 0.31)) * (smooth(u, 0.15, 0.17) * (1 - smooth(u, 0.29, 0.32))
                                            + smooth(u, 0.71, 0.74) * (1 - smooth(u, 0.83, 0.85)))
    reg = np.maximum(skull, np.maximum(top, temple))
    z = z * reg
    zi = np.asarray(Image.fromarray((z * 255).astype(np.uint8)).resize((W, H), Image.BILINEAR), np.float32) / 255
    return zi


def vesemir_regions(u, v):
    """Vesemir d01 (2048) hair-paint zones in UV: the two skull half-islands (bottom, v > .78), the nape strips at the
    island corners (v .55-.64, u < .16 / > .84), the crown band at the top of the face island (v < .085, eyebrows at
    v ~.2 stay out) and the temples down to above the ears (u .14-.33 / .67-.86, v < .29). Cheek stubble, moustache,
    eyebrows and the mouth island (top-left, red) are outside."""
    skull = smooth(v, 0.765, 0.79)
    nape = (smooth(v, 0.54, 0.555) * (1 - smooth(v, 0.635, 0.65))
            * ((1 - smooth(u, 0.15, 0.17)) + smooth(u, 0.83, 0.85)))
    top = (1 - smooth(v, 0.075, 0.095)) * smooth(u, 0.22, 0.25) * (1 - smooth(u, 0.75, 0.78))
    temple = (1 - smooth(v, 0.27, 0.30)) * (smooth(u, 0.13, 0.15) * (1 - smooth(u, 0.32, 0.345))
                                            + smooth(u, 0.655, 0.68) * (1 - smooth(u, 0.85, 0.87)))
    # front zones (visible in front of the strands): 75 % strength, so the hairline keeps a lighter stubble fade.
    # 2026-10-10 (user, L228): CHARS 'temple_strength' (default .75) - temples / above the ears at 100 %
    return np.maximum.reduce([skull, nape, 0.75 * top, CHAR.get('temple_strength', 0.75) * temple])


def grey_weight(rgb):
    """soft 'this pixel is grey hair paint' gate (Vesemir): near-neutral (R-G small), not near-black; the pink skin
    (R-G 20-30) fades out, so the recolour follows the painted hair and its stubble edge, skin stays skin"""
    r, g, b = [rgb[..., i].astype(np.float32) for i in range(3)]
    neutral = 1 - smooth(r - g, 5, 26)              # wide ramp: soft stubble -> skin edge, no hard line
    lit = smooth(r, 35, 55)
    return neutral * lit


def compute_weight(p, rgb):
    zone = uv_zone(p)
    if CHAR.get('gate') == 'grey':
        return zone * grey_weight(rgb)
    return zone * hair_weight(rgb)


def cmd_mask(a):
    p = params(a)
    ensure_dirs()
    o = get_orig()
    rgb = decode_mip(o['mips'][0], 0)
    zone = uv_zone(p)
    wt = compute_weight(p, rgb)
    np.save(os.path.join(BUILD, 'weight.npy'), wt.astype(np.float16))
    Image.fromarray((zone * 255).astype(np.uint8)).resize((1024, 1024)).save(os.path.join(PREV, 'scalp_zone_1k.png'))
    Image.fromarray((wt * 255).astype(np.uint8)).resize((1024, 1024), Image.BOX).save(os.path.join(PREV, 'scalp_weight_1k.png'))
    ov = np.asarray(Image.fromarray(rgb).resize((1024, 1024), Image.BOX)).astype(np.float32)
    w1 = np.asarray(Image.fromarray((wt * 255).astype(np.uint8)).resize((1024, 1024), Image.BOX), np.float32)[..., None] / 255
    ov = ov * (1 - w1) + np.array([0, 255, 0], np.float32) * w1
    Image.fromarray(ov.astype(np.uint8)).save(os.path.join(PREV, 'scalp_weight_overlay_1k.png'))
    print('weight>0.01: %.2f %% of pixels, mean w %.4f' % (100 * (wt > 0.01).mean(), wt.mean()))
    return wt


def params(a):
    p = dict(DEF)
    p.update(HL)
    p.update(HL_STR)
    for k in list(p):
        v = getattr(a, k, None)
        if v is not None:
            p[k] = v
    return p


def target_lab(p):
    t = p['mix']
    rgb = np.array(ROOT_RGB, np.float64) * (1 - t) + np.array(TIP_RGB, np.float64) * t
    return rgb2oklab(rgb[None])[0]


def recolour_image(rgb, wt, p, ref=None):
    """rgb uint8 (h,w,3), wt (h,w) 0..1 -> float RGB 0..255"""
    lab = rgb2oklab(rgb)
    tl = target_lab(p)
    sel = wt > 0.5
    if ref is None:
        ref = lab[sel].mean(0)
    cref = np.hypot(ref[1], ref[2])
    ctgt = np.hypot(tl[1], tl[2])
    s = ctgt / cref if p['keep'] else 0.0
    lum = p['lum']
    if CHAR.get('lum_to'):
        # opt-in (Vesemir): lightness also moved to the target (L x L_target / L_paint), so a light paint becomes the
        # darker fur grey with its strand contrast scaled along; chroma scale capped at 1 (neutral paint, cref ~0)
        lum = lum * tl[0] / ref[0]
        s = min(s, 1.0)
    new = lab.copy()
    new[..., 0] = lab[..., 0] * lum
    new[..., 1] = tl[1] + (lab[..., 1] - ref[1]) * s
    new[..., 2] = tl[2] + (lab[..., 2] - ref[2]) * s
    out = oklab2rgb(new)
    w = wt[..., None]
    return rgb.astype(np.float64) * (1 - w) + out * w, ref


def build_images(p):
    """-> dict(mips=[13 raw bytes], changed=[blocks], rgb0, rec0) using game blocks for untouched areas"""
    o = get_orig()
    rgb0 = decode_mip(o['mips'][0], 0)
    wt = compute_weight(p, rgb0)
    rec0, ref = recolour_image(rgb0, wt, p)
    return o, rgb0, wt, rec0, ref


def encode_chain(o, rgb0, rec0):
    """new raw mips: every 4x4 block whose delta is below 0.5 level keeps the game's bytes"""
    delta = rec0 - rgb0.astype(np.float64)           # (H,W,3)
    new, stats = [], []
    d = delta
    for k in range(NMIP):
        w, h, nbx, nby, _o2, _n = mip_layout()[k]
        if k > 0:
            if w >= 1 and (d.shape[0] > 1 or d.shape[1] > 1):
                d = d.reshape(max(1, d.shape[0] // 2), 2 if d.shape[0] > 1 else 1,
                              max(1, d.shape[1] // 2), 2 if d.shape[1] > 1 else 1, 3).mean((1, 3))
        orig = decode_mip(o['mips'][k], k).astype(np.float64)
        if k == 0:
            tgt = rec0
        else:
            tgt = orig + d
        # pad small mips to 4x4 by edge replication
        ph, pw = nby * 4, nbx * 4
        dd = np.zeros((ph, pw, 3))
        dd[:d.shape[0], :d.shape[1]] = d
        # delta blocks
        db = np.abs(blocks_of(dd)).max((2, 3))
        touched = db >= 0.75
        raw = bytearray(o['mips'][k])
        n = int(touched.sum())
        if n:
            big = np.zeros((ph, pw, 3))
            big[:tgt.shape[0], :tgt.shape[1]] = tgt
            if tgt.shape[0] < 4 or tgt.shape[1] < 4:
                big = np.pad(tgt, ((0, 4 - tgt.shape[0]), (0, 4 - tgt.shape[1]), (0, 0)), mode='edge') \
                    if tgt.shape[0] < 4 and tgt.shape[1] < 4 else big
            bl = blocks_of(np.clip(big, 0, 255))
            ys, xs = np.nonzero(touched)
            enc = dxt1_encode(bl[ys, xs])
            arr = np.frombuffer(bytes(raw), np.uint8).reshape(nby, nbx, 8).copy()
            arr[ys, xs] = enc
            raw = bytearray(arr.tobytes())
        new.append(bytes(raw))
        stats.append((k, n, nbx * nby))
    return new, stats


def previews(rgb0, rec0, wt):
    ensure_dirs()
    rec = np.clip(rec0 + 0.5, 0, 255).astype(np.uint8)
    a = Image.fromarray(rgb0)
    b = Image.fromarray(rec)
    full = Image.new('RGB', (2048, 1024))
    full.paste(a.resize((1024, 1024), Image.BOX), (0, 0))
    full.paste(b.resize((1024, 1024), Image.BOX), (1024, 0))
    full.save(os.path.join(PREV, 'scalp_before_after_full.png'))
    # crops at 1:1 -> 2x2 grid: bottom-left skull island, right skull island, hairline
    f = W / 4096.0
    crops = [tuple(int(c * f) for c in r) for r in [(0, 3200, 1800, 4096), (2300, 3200, 4096, 4096), (600, 0, 3300, 1200)]]
    for i, (x0, y0, x1, y1) in enumerate(crops):
        pair = Image.new('RGB', (x1 - x0, 2 * (y1 - y0) + 8), (255, 255, 255))
        pair.paste(a.crop((x0, y0, x1, y1)), (0, 0))
        pair.paste(b.crop((x0, y0, x1, y1)), (0, y1 - y0 + 8))
        pair.thumbnail((1800, 1800), Image.LANCZOS)
        pair.save(os.path.join(PREV, 'scalp_before_after_crop%d.png' % i))
    # main scalp-area crop: whole bottom strip before | after, stacked
    x0, y0, x1, y1 = 0, int(3150 * f), W, H
    pair = Image.new('RGB', (x1, 2 * (y1 - y0) + 8), (255, 255, 255))
    pair.paste(a.crop((x0, y0, x1, y1)), (0, 0))
    pair.paste(b.crop((x0, y0, x1, y1)), (0, y1 - y0 + 8))
    if W > 2048:                                      # 4096 (Triss): half size; 2048 (Vesemir): 1:1
        pair = pair.resize((pair.width // 2, pair.height // 2), Image.LANCZOS)
    pair.save(os.path.join(PREV, 'scalp_before_after_scalp.png'))
    diff = np.abs(rec.astype(int) - rgb0.astype(int)).max(2)
    Image.fromarray(np.clip(diff * 4, 0, 255).astype(np.uint8)).resize((1024, 1024), Image.BOX).save(
        os.path.join(PREV, 'scalp_diff_1k.png'))
    print('previews ->', PREV)


def cmd_recolour(a):
    p = params(a)
    o, rgb0, wt, rec0, ref = build_images(p)
    previews(rgb0, rec0, wt)
    sel = wt > 0.5
    print('params', p, 'paint mean', rgb0[sel].mean(0).round(1), '-> new mean', rec0[sel].mean(0).round(1))
    print('target', np.array(ROOT_RGB) * (1 - p['mix']) + np.array(TIP_RGB) * p['mix'])


# ---------------------------------------------------------------------------------------------- hairline paint

HL = dict(front=3.0,        # deg in front of the line (toward the face) over which the paint fades 0.6 -> 0 (3 deg ~ 5 mm)
          back=6.0,         # deg behind the line with full scalp tint
          op_line=0.6,      # opacity at the line
          op_back=0.85,     # opacity behind the line
          ramp=2.0,         # deg behind the line over which op_line rises to op_back
          tail=3.0,         # deg past `back` over which the paint fades out again (hidden under the hair)
          phi_fade=20.0,    # deg at both ends of the front arc over which the paint fades out
          streak=0.5,       # 0 = flat smear, 1 = strong per-hair opacity variation
          streak_deg=0.7,   # azimuth width of one fine hair (deg; ~0.19 deg per texel at the forehead -> ~3-4 px)
          streak_len=1.5,   # length of a streak along the hair direction (deg of elevation; ~40 px per deg)
          tone=0.12,        # +- relative luminance jitter per streak
          seed=7,
          fill=6)           # px of nearest-texel fill past the island edges (stops DXT edge bleed / seams)
HL_STR = dict(rgb=None)     # 'r,g,b' override of the paint colour (default: CHARS[char]['paint'], else the root..tip mix by --mix)


def hairline_curve_data(a=None):
    """vanilla hairline of the character: (bins deg, line elevation deg) cached in BUILD\\hairline_curve.npz.
    growth.hairline_curve on the head mesh + the vanilla hair cards (visible card points near the skin)."""
    cp = os.path.join(BUILD, 'hairline_curve.npz')
    if os.path.exists(cp):
        z = np.load(cp)
        return z['bins'], z['line']
    import hairpipe, growth
    import gamefiles as g
    from texread import load_xbm
    C = hairpipe.load_config(CHAR['config'])
    info = hairpipe.discover(C, log=lambda *x: None)
    hair = info['hair'][0]
    m = g.load_mesh(hair['mesh'])
    ch = g.merge_chunks(m, C.get('lod0_chunks') or m['lod0'])
    head_mesh = C.get('head_mesh') or info['heads'][0]
    assert head_mesh.lower() == MESH.lower(), (head_mesh, MESH)
    head = growth.Head(head_mesh)
    tex = C.get('card_texture') or hairpipe.card_texture(hair['mesh'])
    alpha = np.asarray(load_xbm(tex))[..., 3].astype(np.float64) / 255.0
    bins, line, _pts, _P, _ph, _el = growth.hairline_curve(head, ch, alpha, phi_max=110.0)
    os.makedirs(BUILD, exist_ok=True)
    np.savez(cp, bins=bins, line=line)
    print('hairline curve cached:', cp)
    return bins, line


def hairline_geometry(p):
    """UV raster of the head mesh around the vanilla hairline -> (s, phi, cov, (x0, y0, x1, y1))
    s = elevation above the line (deg, + = behind the line / toward the crown), phi = azimuth (deg, + = subject's left);
    arrays are the bbox crop of the texels covered by head triangles, filled `fill` px past the island edges."""
    import growth
    from scipy.ndimage import distance_transform_edt
    bins, line = hairline_curve_data()
    ok = ~np.isnan(line)
    b_ok, l_ok = bins[ok], line[ok]
    head = growth.Head(MESH)
    mesh = __import__('gamefiles').load_mesh(MESH)
    ch0 = mesh['chunks'][0]
    uv = ch0['uv']
    P = head.v
    assert len(uv) == len(P)
    c = np.array(head.c, float)
    d = P - c
    vphi = np.degrees(np.arctan2(d[:, 0], d[:, 1]))
    vel = np.degrees(np.arctan2(d[:, 2], np.hypot(d[:, 0], d[:, 1])))
    vs = vel - np.interp(vphi, b_ok, l_ok)
    T = head.t
    ok_t = ((vs[T].min(1) < p['back'] + p['tail'] + 8) & (vs[T].max(1) > -p['front'] - 8)
            & (P[T][..., 2].min(1) > head.top - 0.16) & (np.abs(vphi[T]).min(1) < 135))
    px = np.c_[uv[:, 0] * W, uv[:, 1] * H]
    sel = T[ok_t]
    x0 = int(max(0, np.floor(px[sel][..., 0].min())) - p['fill'] - 2)
    x1 = int(min(W, np.ceil(px[sel][..., 0].max())) + p['fill'] + 2)
    y0 = int(max(0, np.floor(px[sel][..., 1].min())) - p['fill'] - 2)
    y1 = int(min(H, np.ceil(px[sel][..., 1].max())) + p['fill'] + 2)
    w_, h_ = x1 - x0, y1 - y0
    S = np.zeros((h_, w_), np.float32)
    PH = np.zeros((h_, w_), np.float32)
    cov = np.zeros((h_, w_), bool)
    for t in sel:
        q = px[t] - [x0, y0]
        area = (q[1, 0] - q[0, 0]) * (q[2, 1] - q[0, 1]) - (q[2, 0] - q[0, 0]) * (q[1, 1] - q[0, 1])
        if abs(area) < 1e-9:
            continue
        ax, bx = int(max(0, np.floor(q[:, 0].min()))), int(min(w_, np.ceil(q[:, 0].max()) + 1))
        ay, by = int(max(0, np.floor(q[:, 1].min()))), int(min(h_, np.ceil(q[:, 1].max()) + 1))
        if bx <= ax or by <= ay:
            continue
        yy, xx = np.mgrid[ay:by, ax:bx]
        fx, fy = xx + 0.5 - q[0, 0], yy + 0.5 - q[0, 1]
        l1 = (fx * (q[2, 1] - q[0, 1]) - (q[2, 0] - q[0, 0]) * fy) / area
        l2 = ((q[1, 0] - q[0, 0]) * fy - fx * (q[1, 1] - q[0, 1])) / area
        l0 = 1 - l1 - l2
        eps = -0.02
        m = (l0 >= eps) & (l1 >= eps) & (l2 >= eps)
        if not m.any():
            continue
        pos = l0[m][:, None] * P[t[0]] + l1[m][:, None] * P[t[1]] + l2[m][:, None] * P[t[2]]
        dd = pos - c
        phi = np.degrees(np.arctan2(dd[:, 0], dd[:, 1]))
        el = np.degrees(np.arctan2(dd[:, 2], np.hypot(dd[:, 0], dd[:, 1])))
        S[yy[m], xx[m]] = el - np.interp(phi, b_ok, l_ok)
        PH[yy[m], xx[m]] = phi
        cov[yy[m], xx[m]] = True
    if p['fill'] > 0:
        dist, (iy, ix) = distance_transform_edt(~cov, return_indices=True)
        near = (~cov) & (dist <= p['fill'])
        S[near] = S[iy[near], ix[near]]
        PH[near] = PH[iy[near], ix[near]]
        cov = cov | near
    return S, PH, cov, (x0, y0, x1, y1), (b_ok.min(), b_ok.max())


def value_noise(shape_xy, cell_x, cell_y, seed, origin_x=0.0, origin_y=0.0):
    """smooth 2D value noise 0..1 sampled at coords given as two float arrays (x, y); cells of cell_x by cell_y"""
    from scipy.ndimage import map_coordinates
    x, y = shape_xy
    rng = np.random.default_rng(seed)
    gx = int((x.max() - x.min()) / cell_x) + 4
    gy = int((y.max() - y.min()) / cell_y) + 4
    g = rng.random((gy, gx))
    cx = (x - x.min()) / cell_x + 1
    cy = (y - y.min()) / cell_y + 1
    return map_coordinates(g, [cy, cx], order=1, mode='nearest')


def hairline_alpha(p, S, PH, cov, span):
    """paint opacity (0..1) per texel of the crop from the elevation above the vanilla line (deg), see HL"""
    f, bk, ol, ob, rp, tl = p['front'], p['back'], p['op_line'], p['op_back'], max(p['ramp'], 1e-3), max(p['tail'], 1e-3)
    s = S.astype(np.float64)
    a = np.zeros_like(s)
    ph = PH.astype(np.float64)
    if p['streak'] > 0:
        # every fine hair ends at its own length in front of the line (per-azimuth jitter of the fade position)
        nl = value_noise((ph, s), p['streak_deg'] * 0.8, 1e6, p['seed'] + 3)
        sf = s - (2 * nl - 1) * f * 0.7 * p['streak']
    else:
        sf = s
    fr = s < 0
    a[fr] = ol * smooth(sf[fr], -f, 0.0)                 # 0 at -front, op_line at the line (per hair jittered)
    bh = (s >= 0) & (s < bk)
    a[bh] = ol + (ob - ol) * smooth(s[bh], 0.0, rp)
    tt = s >= bk
    a[tt] = ob * (1 - smooth(s[tt], bk, bk + tl))
    lo, hi = span
    pf = max(p['phi_fade'], 1e-3)
    a *= smooth(ph, lo, lo + pf) * (1 - smooth(ph, hi - pf, hi))
    a *= cov
    # fine vertical streaks: value noise stretched along the hair direction (elevation), narrow in azimuth
    if p['streak'] > 0:
        n1 = value_noise((ph, s), p['streak_deg'], p['streak_len'], p['seed'])
        n2 = value_noise((ph, s), p['streak_deg'] * 0.45, p['streak_len'] * 0.6, p['seed'] + 1)
        n = 0.65 * n1 + 0.35 * n2
        n = np.clip((n - 0.5) * 1.8 + 0.5, 0, 1)
        a = np.clip(a * (1 + p['streak'] * (2 * n - 1) * 1.2), 0, 1)
        tone = 1 + p['tone'] * (2 * value_noise((ph, s), p['streak_deg'] * 0.7, p['streak_len'] * 1.5, p['seed'] + 2) - 1)
    else:
        tone = np.ones_like(a)
    return a, tone


def hairline_colour(p):
    if p.get('rgb'):
        return np.array([float(v) for v in str(p['rgb']).split(',')])
    if CHAR.get('paint'):
        return np.array(CHAR['paint'], np.float64)       # visual in-game hair colour (use --rgb to override)
    t = p['mix']
    return np.array(ROOT_RGB, np.float64) * (1 - t) + np.array(TIP_RGB, np.float64) * t


def build_hairline(p):
    """-> (o, rgb0, alpha(H,W), rec0(H,W,3 float), ref) like build_images"""
    o = get_orig()
    rgb0 = decode_mip(o['mips'][0], 0)
    S, PH, cov, (x0, y0, x1, y1), span = hairline_geometry(p)
    a, tone = hairline_alpha(p, S, PH, cov, span)
    col = hairline_colour(p)
    alpha = np.zeros((H, W), np.float64)
    alpha[y0:y1, x0:x1] = a
    rec0 = rgb0.astype(np.float64)
    sub = rec0[y0:y1, x0:x1]
    paint = np.clip(col[None, None, :] * tone[..., None], 0, 255)
    rec0[y0:y1, x0:x1] = sub * (1 - a[..., None]) + paint * a[..., None]
    deg_px = None
    return o, rgb0, alpha, rec0, (col, (x0, y0, x1, y1), span)


def previews_hairline(rgb0, rec0, alpha, box):
    ensure_dirs()
    rec = np.clip(rec0 + 0.5, 0, 255).astype(np.uint8)
    A, B = Image.fromarray(rgb0), Image.fromarray(rec)
    full = Image.new('RGB', (2048, 1024))
    full.paste(A.resize((1024, 1024), Image.BOX), (0, 0))
    full.paste(B.resize((1024, 1024), Image.BOX), (1024, 0))
    full.save(os.path.join(PREV, 'hairline_before_after_full.png'))
    ys, xs = np.nonzero(alpha > 0.02)
    px0, px1 = max(0, xs.min() - 60), min(W, xs.max() + 60)
    py0, py1 = max(0, ys.min() - 60), min(H, ys.max() + 60)
    pair = Image.new('RGB', (px1 - px0, 2 * (py1 - py0) + 8), (255, 255, 255))
    pair.paste(A.crop((px0, py0, px1, py1)), (0, 0))
    pair.paste(B.crop((px0, py0, px1, py1)), (0, py1 - py0 + 8))
    pair.thumbnail((2000, 2400), Image.LANCZOS)
    pair.save(os.path.join(PREV, 'hairline_before_after_forehead.png'))
    # 1:1 detail of the densest part of the band (centre of the front arc)
    cx, cy = int(xs.mean()), int(ys.mean())
    r = 384
    dx0, dy0 = max(0, cx - r), max(0, cy - r)
    det = Image.new('RGB', (4 * r + 8, 2 * r), (255, 255, 255))
    det.paste(A.crop((dx0, dy0, dx0 + 2 * r, dy0 + 2 * r)), (0, 0))
    det.paste(B.crop((dx0, dy0, dx0 + 2 * r, dy0 + 2 * r)), (2 * r + 8, 0))
    det.save(os.path.join(PREV, 'hairline_detail_1to1.png'))
    Image.fromarray((np.clip(alpha, 0, 1) * 255).astype(np.uint8)).resize((1024, 1024), Image.BOX).save(
        os.path.join(PREV, 'hairline_alpha_1k.png'))
    print('previews ->', PREV, ' band bbox px', (px0, py0, px1, py1))


def cmd_hairline(a):
    if CHAR['mode'] != 'hairline':
        sys.exit('--char %s has no hairline mode' % CHAR.get('name'))
    p = params(a)
    o, rgb0, alpha, rec0, (col, box, span) = build_hairline(p)
    previews_hairline(rgb0, rec0, alpha, box)
    np.save(os.path.join(BUILD, 'hairline_alpha.npy'), alpha.astype(np.float16))
    print('params', p)
    print('paint colour', col.round(1), 'azimuth span %.0f..%.0f' % span, 'texels with paint > 0.02: %d' % int((alpha > 0.02).sum()),
          'max alpha %.2f' % alpha.max())
    return rec0


def cmd_build(a):
    p = params(a)
    o = get_orig()
    if a.identity:
        mips, stats = list(o['mips']), []
        print('IDENTITY build (unchanged d01)')
    elif CHAR['mode'] == 'hairline':
        o, rgb0, wt, rec0, (col, box, span) = build_hairline(p)
        previews_hairline(rgb0, rec0, wt, box)
        mips, stats = encode_chain(o, rgb0, rec0)
        print('re-encoded blocks per mip:', stats)
    else:
        o, rgb0, wt, rec0, ref = build_images(p)
        previews(rgb0, rec0, wt)
        mips, stats = encode_chain(o, rgb0, rec0)
        print('re-encoded blocks per mip:', stats)
    blob, ends = pack_entry(mips)
    stub = make_stub(o['stub'], mips)
    out = MODDIR + ('_identity' if a.identity else '')
    content = os.path.join(out, 'content')
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(content)
    write_cache_v6(os.path.join(content, 'texture.cache'), REL, blob, ends, sum(len(m) for m in mips))
    b = w3lib.write_legacy_bundle(os.path.join(content, 'blob0.bundle'), [(REL, stub)])
    w3lib.write_legacy_metadata_store(os.path.join(content, 'metadata.store'), [b])
    print('built', out, 'cache %d B, stub %d B' % (len(blob), len(stub)))
    return out


def cmd_verify(a):
    d = a.dir or (MODDIR + ('_identity' if a.identity else ''))
    content = os.path.join(d, 'content')
    o = get_orig()
    c = T.TextureCache(os.path.join(content, 'texture.cache'))
    e = c.by_name[REL.lower()]
    assert c.version == 6 and e.hash == T.name_hash(REL) and (e.w, e.h, e.mips) == (W, H, NMIP), 'entry header'
    mips = split_mips(parse_entry_blocks(c.path, e, c.mips))
    assert sum(len(m) for m in mips) == e.size == sum(L[5] for L in mip_layout())
    bp = os.path.join(content, 'blob0.bundle')
    ents = w3lib.bundle_entries(bp)
    assert len(ents) == 1 and ents[0].name.lower() == REL.lower()
    stub = w3lib.bundle_read(bp, ents[0])
    cr = w3lib.CR2W(stub)
    assert cr.verify_crcs() == (0, True), 'stub crc'
    diff = [i for i in range(len(stub) - RES_BYTES) if stub[i] != o['stub'][i]]
    # only the CRC words may differ (export chunk CRC covers the resident mips; table / header CRC follow)
    crc_at = (0x20, 0x60, cr.tables[4][0] + 20)        # header, export table, export 0 CRC (Triss: 461)
    assert all(any(q <= i < q + 4 for q in crc_at) for i in diff), 'stub header differs %s' % diff
    assert stub[-RES_BYTES:] == b''.join(mips[NSTREAM:]), 'stub resident mips != cache resident block'
    same = 0
    for k in range(NMIP):
        r0 = decode_mip(o['mips'][k], k)
        r1 = decode_mip(mips[k], k)
        eq = np.array_equal(r0, r1)
        bytes_eq = o['mips'][k] == mips[k]
        same += bytes_eq
        print('mip %2d  %5dx%-5d  pixels equal %s  dxt bytes equal %s  maxdiff %d' %
              (k, r0.shape[1], r0.shape[0], eq, bytes_eq, int(np.abs(r0.astype(int) - r1.astype(int)).max())))
    if a.identity:
        assert same == NMIP and stub == o['stub'], 'IDENTITY FAILED'
        print('IDENTITY OK: all %d mips byte-identical, stub byte-identical to the game xbm, cache entry parses' % NMIP)
    return True


def running():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq witcher3.exe'], capture_output=True, text=True).stdout
    return 'witcher3.exe' in out


def cmd_install(a):
    if running():
        sys.exit('witcher3.exe is running: not installing')
    src = os.path.join(MODDIR + ('_identity' if a.identity else ''), 'content')
    dst = os.path.join(GAME, 'mods', MODNAME, 'content')
    if os.path.isdir(os.path.dirname(dst)):
        shutil.rmtree(os.path.dirname(dst))
    shutil.copytree(src, dst)
    print('installed', dst)


def cmd_uninstall(a):
    dst = os.path.join(GAME, 'mods', MODNAME)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
        print('removed', dst)
    else:
        print('not installed')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['extract', 'mask', 'recolour', 'hairline', 'build', 'verify', 'install', 'uninstall'])
    ap.add_argument('dir', nargs='?')
    ap.add_argument('--char', default='triss', choices=sorted(CHARS))
    ap.add_argument('--identity', action='store_true')
    for k, v in list(DEF.items()) + list(HL.items()) + list(HL_STR.items()):
        ap.add_argument('--' + k, type=(type(v) if v is not None else str), default=None)
    a = ap.parse_args()
    set_char(a.char)
    globals()['cmd_' + a.cmd](a)


if __name__ == '__main__':
    main()
