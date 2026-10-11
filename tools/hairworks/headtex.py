r"""headtex - HEAD-SPACE hair textures (growth.head_tex, opt-in; see HAIRWORK.md "head_tex").

HairWorks samples densityTex / rootColorTex / tipColorTex at every hair root's UV = the barycentric interpolation of the
growth face's per-CORNER UVs. The default pipeline gives each face an arbitrary patch of the vanilla card atlas
(atlas.face_uvs), so density and colour cannot be controlled BY POSITION on the head. head_tex replaces the corner UVs
with a continuous head-space projection and writes our own textures in that space:

  u = 0.5 + s * azimuth / span      azimuth = atan2(dx, dy) around head.c, 0 = front, + = subject's left (as hairline_curve);
                                    the texture covers +-span/2 degrees (default 380: the back seam at +-180 deg carries
                                    10 deg of margin on both sides, faces that cross it are unwrapped and rasterised twice)
  v = (el_max - el) / (el_max - el_min)     el = elevation (hairline_curve convention), top of the texture = crown
  densityTex   DXT5 (alpha = density, densityTexChannel ALPHA): for every texel the growth face it falls in is found
               (rasterised in head space) and the CURRENT density is sampled there = bilinear card alpha at the barycentric
               interpolation of that face's OLD UVs (so density_floor / hairline_fade results are kept) x optional `fade`
  rootColorTex DXT1: the hair colour (look.root_color), blending to `skin` within `skin_band_deg` of the hairline
  tipColorTex  DXT1 128x128 flat: look.tip_color
All three are inline xbm (one mip, all data in the bundle, no texture.cache), cloned from vanilla inline textures
(DiffuseNoMips DXT1 proxy / the hair card stub for DXT5).

Config (growth.head_tex = true or a dict):
  size [1024]           DENSITY texture size (int or [w, h]); per-root fidelity grows with it (512: p50 error .05, 2048: .01)
  colour_size [512]     root colour texture size (same uv, any size)
  span [380]            minimum degrees of azimuth the texture covers (grows to hold the widest face that crosses the back seam)
  el_margin [2]         degrees of elevation margin below the lowest root
  pole_deg [40]         faces whose azimuth span exceeds this (crown / pole) get ONE uv point (uniform density)
  fade {band_deg, lo, phi_max}   density x (lo + (1-lo) * clip(sdeg / band, 0, 1)), sdeg = el - hairline_curve(phi), |phi| < phi_max
  skin [r,g,b]          colour at / in front of the hairline (sRGB bytes); with skin_band_deg [3]: smoothstep(0, band, sdeg)
  skin_taper_deg [8]    azimuth taper at the ends of the defined hairline curve (the curve only exists for some azimuths)
  phi_max [110]
  colour [true]         false = no colour textures (magenta twins; flat rootColor / tipColor stay in force)
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, TOOLS)

CARD_STUB = r'characters\models\common\textures\hair\hair_long_04_d01.xbm'      # TCM_DXTAlpha stub (template)
DXT1_PROXY = r'environment\textures_entity_proxy\rocky_path_proxy_d.xbm'        # TCM_DXTNoAlpha inline (template)

DEFAULTS = dict(size=1024, colour_size=512, span=380.0, el_margin=2.0, pole_deg=40.0, phi_max=110.0, skin_band_deg=3.0,
                skin_taper_deg=8.0, colour=True, seed=5)


def cfg_of(C):
    ht = C['growth'].get('head_tex')
    if not ht:
        return None
    d = dict(DEFAULTS)
    if isinstance(ht, dict):
        d.update(ht)
    s = d['size']
    d['w'], d['h'] = (int(s[0]), int(s[1])) if isinstance(s, (list, tuple)) else (int(s), int(s))
    assert d['w'] % 4 == 0 and d['h'] % 4 == 0
    cs = d['colour_size']
    d['cw'], d['ch'] = (int(cs[0]), int(cs[1])) if isinstance(cs, (list, tuple)) else (int(cs), int(cs))
    assert d['cw'] % 4 == 0 and d['ch'] % 4 == 0
    return d


# ---------------------------------------------------------------------------------------------- geometry

def proj(P, c):
    """positions -> (azimuth deg, elevation deg) around c; same convention as growth.hairline_curve"""
    d = np.asarray(P, float) - np.asarray(c, float)
    return (np.degrees(np.arctan2(d[..., 0], d[..., 1])),
            np.degrees(np.arctan2(d[..., 2], np.hypot(d[..., 0], d[..., 1]))))


def bilerp(img, uv):
    """bilinear sample (texel centres at (i + .5) / n), clamped; img (h, w) float, uv (N, 2)"""
    h, w = img.shape
    x = np.clip(uv[:, 0] * w - 0.5, 0, w - 1)
    y = np.clip(uv[:, 1] * h - 0.5, 0, h - 1)
    x0 = np.minimum(x.astype(int), w - 2) if w > 1 else np.zeros(len(x), int)
    y0 = np.minimum(y.astype(int), h - 2) if h > 1 else np.zeros(len(y), int)
    fx, fy = x - x0, y - y0
    x1 = np.minimum(x0 + 1, w - 1)
    y1 = np.minimum(y0 + 1, h - 1)
    return (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x1] * fx * (1 - fy)
            + img[y1, x0] * (1 - fx) * fy + img[y1, x1] * fx * fy)


class Mapping:
    """head-space UV mapping: (azimuth, elevation) <-> (u, v)"""

    def __init__(self, cfg, el_min, el_max, sgn):
        self.w, self.h, self.span, self.sgn = cfg['w'], cfg['h'], cfg['span'], sgn
        self.el_min, self.el_max = el_min, el_max

    def uv(self, phi, el):
        return 0.5 + self.sgn * np.asarray(phi) / self.span, (self.el_max - np.asarray(el)) / (self.el_max - self.el_min)

    def inv(self, u, v):
        return self.sgn * (np.asarray(u) - 0.5) * self.span, self.el_max - np.asarray(v) * (self.el_max - self.el_min)


def corner_phi_el(P3, c):
    """(M,3,3) corner positions -> unwrapped azimuth (M,3) (mean in [-180, 180)), elevation (M,3), centroid phi / el"""
    phi, el = proj(P3, c)
    ref = phi[:, :1]
    ph = ref + ((phi - ref + 180.0) % 360.0 - 180.0)
    ph = ph - 360.0 * np.round(ph.mean(1, keepdims=True) / 360.0)
    cp, ce = proj(P3.mean(1), c)
    return ph, el, cp, ce


# ---------------------------------------------------------------------------------------------- hairline field

def hairline_field(head, ch, alpha, phi_max):
    """-> (phi bins, line elevation per bin (nan = undefined)); the same call hairpipe's hairline_fade makes"""
    import growth
    bins, line, _p, _P, _ph, _el = growth.hairline_curve(head, ch, alpha, phi_max=phi_max)
    return bins, line


def sdeg_of(phi, el, bins, line):
    ok = ~np.isnan(line)
    return el - np.interp(phi, bins[ok], line[ok])


def smoothstep(x):
    x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)


# ---------------------------------------------------------------------------------------------- encoders

def _e565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float64)


_flat_cache = {}


def dxt1_flat_block(rgb):
    """8 bytes of a DXT1 block that decodes to (nearly) the single colour rgb: 565 endpoints can only hit 5/6-bit
    values, but index 2 / 3 mix them 2:1 / 1:2, so a search over endpoint pairs gets within ~1 level (a plain
    565 round-trip is off by up to 4 levels, a lot on a dark hair colour)."""
    key = tuple(int(round(float(x))) for x in rgb)
    if key in _flat_cache:
        return _flat_cache[key]
    t = np.array(key, float)

    def near(v, k):
        qs = []
        for ch_i, (mx, scale) in enumerate(((31, 255), (63, 255), (31, 255))):
            q0 = int(round(v[ch_i] * mx / scale))
            qs.append([min(max(q0 + d, 0), mx) for d in range(-k, k + 1)])
        out = []
        for r in qs[0]:
            for g in qs[1]:
                for b in qs[2]:
                    out.append((r << 11) | (g << 5) | b)
        return np.array(sorted(set(out)), np.int64)

    best = [1e18, None]

    def consider(A, B, mode):
        """A, B int arrays of 565 codes (A > B or A == B for mode 0): palette entry `mode` vs t"""
        A, B = np.asarray(A), np.asarray(B)
        e0, e1 = _e565(A), _e565(B)
        pal = (e0, e1, np.floor((2 * e0 + e1) / 3 + 0.5), np.floor((e0 + 2 * e1) / 3 + 0.5))[mode]
        err = ((pal - t) ** 2).sum(-1)
        ok = (A > B) | ((A == B) & (mode == 0))
        err = np.where(ok, err, 1e18)
        i = int(err.argmin())
        if err[i] < best[0]:
            best[0], best[1] = float(err[i]), (int(A[i]), int(B[i]), mode)
    c0s = near(t, 3)
    for a in c0s:                                             # idx 0: c0 itself is nearest, any smaller c1 will do
        consider([a], [max(a - 1, 0)] if a > 0 else [a], 0)
    for a in c0s:                                             # idx 2 = (2 c0 + c1) / 3
        tgt = np.clip(3 * t - 2 * _e565(np.array(a)), 0, 255)
        bs = near(tgt, 1)
        consider(np.full(len(bs), a), bs, 2)
    for b in c0s:                                             # idx 3 = (c0 + 2 c1) / 3
        tgt = np.clip(3 * t - 2 * _e565(np.array(b)), 0, 255)
        as_ = near(tgt, 1)
        consider(as_, np.full(len(as_), b), 3)
    if best[1] is None:                                       # (black / nothing found) fall back to a plain round-trip
        q = int(((int(t[0] * 31 / 255 + .5)) << 11) | (int(t[1] * 63 / 255 + .5) << 5) | int(t[2] * 31 / 255 + .5))
        best = (0, (q, q, 0))
    a, b, idx = best[1]
    word = 0
    for i in range(16):
        word |= idx << (2 * i)
    blk = struct.pack('<HHI', a, b, word)
    _flat_cache[key] = blk
    return blk


def encode_dxt1(img):
    """(h, w, 3) float 0..255 -> raw DXT1 bytes; flat 4x4 blocks are encoded exactly (dxt1_flat_block)"""
    import scalp_tex as S
    h, w = img.shape[:2]
    blk = S.blocks_of(img.astype(np.float64))                  # (nby, nbx, 16, 3)
    nby, nbx = blk.shape[:2]
    flat = blk.reshape(nby * nbx, 16, 3)
    out = np.zeros((nby * nbx, 8), np.uint8)
    isflat = (flat.max(1) - flat.min(1)).max(1) < 0.5
    if (~isflat).any():
        out[~isflat] = S.dxt1_encode(flat[~isflat])
    for i in np.where(isflat)[0]:
        out[i] = np.frombuffer(dxt1_flat_block(flat[i].mean(0)), np.uint8)
    return out.tobytes()


def decode_dxt1(raw, w, h):
    import scalp_tex as S
    return S.dxt1_decode_blocks(raw, w // 4, h // 4)


def encode_dxt5_alpha(a):
    """(h, w) float 0..255 -> raw DXT5 bytes (alpha block per 4x4: the better of the 8-level and the 6-level+0/255 mode;
    colour block = white)"""
    h, w = a.shape
    nby, nbx = h // 4, w // 4
    b = np.round(np.clip(a, 0, 255)).reshape(nby, 4, nbx, 4).transpose(0, 2, 1, 3).reshape(nby * nbx, 16)
    mn, mx = b.min(1), b.max(1)
    n = len(b)
    # mode A: a0 = max > a1 = min, 8 levels
    k = np.arange(8)
    palA = np.zeros((n, 8))
    palA[:, 0], palA[:, 1] = mx, mn
    for j in range(1, 7):
        palA[:, j + 1] = ((7 - j) * mx + j * mn) / 7.0
    # mode B: a0 = min <= a1 = max, 6 levels + 0 + 255
    palB = np.zeros((n, 8))
    palB[:, 0], palB[:, 1] = mn, mx
    for j in range(1, 5):
        palB[:, j + 1] = ((5 - j) * mn + j * mx) / 5.0
    palB[:, 6], palB[:, 7] = 0, 255

    def fit(pal):
        pal = np.floor(pal + 0.5)                              # decoders round; keep the encoder's model on the same grid
        err = (b[:, :, None] - pal[:, None, :]) ** 2
        ix = err.argmin(2)
        return ix, np.take_along_axis(err, ix[..., None], 2)[..., 0].sum(1)
    ixA, eA = fit(palA)
    ixB, eB = fit(palB)
    useB = (eB < eA) & (mn < mx)
    ix = np.where(useB[:, None], ixB, ixA)
    a0 = np.where(useB, mn, mx).astype(np.uint8)
    a1 = np.where(useB, mx, mn).astype(np.uint8)
    flat = mn == mx
    ix[flat] = 0
    a0[flat], a1[flat] = mx[flat].astype(np.uint8), mn[flat].astype(np.uint8)   # a0 == a1: 6-level mode, index 0 = a0
    bits = np.zeros(n, np.uint64)
    for i in range(16):
        bits |= ix[:, i].astype(np.uint64) << np.uint64(3 * i)
    out = np.zeros((n, 16), np.uint8)
    out[:, 0], out[:, 1] = a0, a1
    for q in range(6):
        out[:, 2 + q] = ((bits >> np.uint64(8 * q)) & np.uint64(255)).astype(np.uint8)
    out[:, 8:12] = 255                                          # c0 = c1 = 0xFFFF, indices 0
    return out.tobytes()


def decode_dxt5_alpha(raw, w, h):
    n = (w // 4) * (h // 4)
    blk = np.frombuffer(raw, np.uint8).reshape(n, 16)
    a0, a1 = blk[:, 0].astype(float), blk[:, 1].astype(float)
    bits = np.zeros(n, np.uint64)
    for q in range(6):
        bits |= blk[:, 2 + q].astype(np.uint64) << np.uint64(8 * q)
    ix = np.stack([((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(int) for i in range(16)], 1)
    pal = np.zeros((n, 8))
    pal[:, 0], pal[:, 1] = a0, a1
    big = a0 > a1
    for j in range(1, 7):
        pal[:, j + 1] = np.where(big, ((7 - j) * a0 + j * a1) / 7.0, 0)
    for j in range(1, 5):
        pal[:, j + 1] = np.where(big, pal[:, j + 1], ((5 - j) * a0 + j * a1) / 5.0)
    pal[:, 6] = np.where(big, pal[:, 6], 0)
    pal[:, 7] = np.where(big, pal[:, 7], 255)
    pal = np.floor(pal + 0.5)
    v = np.take_along_axis(pal, ix, 1)
    return v.reshape(h // 4, w // 4, 4, 4).transpose(0, 2, 1, 3).reshape(h, w)


# ---------------------------------------------------------------------------------------------- xbm

def make_xbm(template_rel, w, h, data, key_name, pitch):
    """CBitmapTexture CR2W from a vanilla inline / stub template: width / height / residentMipIndex 0 / textureCacheKey
    patched, trailing = [0, 1 mip, (w, h, pitch), size, align 16, data]. One mip, everything resident (no cache)."""
    import gamefiles as G
    import texcache as T
    import w3lib
    src = G.get(template_rel)
    c = w3lib.CR2W(src)
    e = c.exports[0]
    props = c.props(0)
    for name, val in (('width', w), ('height', h), ('textureCacheKey', T.name_hash(key_name))):
        t, o, ln = c.prop(0, name)
        assert ln == 4, name
        struct.pack_into('<I', c.d, o, val)
    r = c.prop(0, 'residentMipIndex')
    if r:
        c.d[r[1]] = 0
    last = props[-1]
    end_abs = last[2] + last[3]
    assert c.d[end_abs:end_abs + 2] == b'\0\0', 'property list terminator not found'
    props_len = end_abs + 2 - e['off']
    chunk = bytes(c.d[e['off']:e['off'] + props_len]) + struct.pack('<IIIIIII', 0, 1, w, h, pitch, len(data), 16) + data
    c.replace_export_data(0, chunk)
    out = bytes(c.d)
    bad, ok = w3lib.CR2W(out).verify_crcs()
    assert bad == 0 and ok, 'xbm CRC check failed'
    return out


def read_xbm(data):
    """-> dict(w, h, compression, group, key, mips=[(w, h, pitch)], raw) of an inline xbm (the checker for make_xbm)"""
    import cr2w_reader as R
    f = R.CR2WFile(data)
    e = f.exports[0]
    t = e.trailing
    _z, n = struct.unpack_from('<II', t, 0)
    mips = [struct.unpack_from('<3I', t, 8 + 12 * i) for i in range(n)]
    p = 8 + 12 * n
    size, align = struct.unpack_from('<II', t, p)
    raw = t[p + 8:p + 8 + size]
    assert p + 8 + size == len(t), (p + 8 + size, len(t))
    P = e.props
    return dict(w=P['width'][1], h=P['height'][1], compression=str(P['compression'][1]), group=str(P['textureGroup'][1]),
                key=P['textureCacheKey'][1], mips=mips, raw=raw, resident=P.get('residentMipIndex'), align=align)


# ---------------------------------------------------------------------------------------------- build

def build(C, res, uvs_old, head, ch, alpha, has_density, log=print, prefix='', base_path=None):
    """-> dict(uvs (M*3, 2), files [(path, bytes)], paths {density, root, tip}, + the arrays used by check())"""
    cfg = cfg_of(C)
    L = C['look']
    w, h = cfg['w'], cfg['h']
    S = res['S']
    F = np.asarray(res['F'], int)
    R = np.array([np.asarray(s['p'][0], float) for s in S])
    c = np.array(head.c, float)
    U_old = np.asarray(uvs_old, float).reshape(-1, 3, 2)
    M = len(F)
    P3 = R[F]                                                  # (M,3,3)
    ph, el, cph, cel = corner_phi_el(P3, c)
    span_f = ph.max(1) - ph.min(1)
    pole = span_f > cfg['pole_deg']
    cfg = dict(cfg, span=max(float(cfg['span']), 2.0 * float(np.abs(ph[~pole]).max()) + 2.0))     # the texture must hold the widest unwrapped face
    el_lo = float(np.floor(el.min() - cfg['el_margin']))
    el_hi = float(min(90.0, np.ceil(el.max() + cfg['el_margin'])))
    # handedness: the vanilla rule is signed uv area < 0 (atlas.face_uvs); take the sign that makes most faces so
    area_pos = None
    for sgn in (1.0, -1.0):
        mp = Mapping(cfg, el_lo, el_hi, sgn)
        u, v = mp.uv(ph, el)
        a = (u[:, 1] - u[:, 0]) * (v[:, 2] - v[:, 0]) - (u[:, 2] - u[:, 0]) * (v[:, 1] - v[:, 0])
        if (a[~pole] < 0).mean() >= 0.5:
            break
    mp = Mapping(cfg, el_lo, el_hi, sgn)
    U = np.stack(mp.uv(ph, el), -1)                            # (M,3,2)
    cu, cv = mp.uv(cph, cel)
    Uc = np.stack([cu, cv], -1)                                # centroid uv
    U[pole] = Uc[pole][:, None, :]                             # pole faces: one uv point (uniform density)
    a = (U[:, 1, 0] - U[:, 0, 0]) * (U[:, 2, 1] - U[:, 0, 1]) - (U[:, 2, 0] - U[:, 0, 0]) * (U[:, 1, 1] - U[:, 0, 1])
    log('  head_tex: %d growth faces, azimuth %+.0f..%+.0f, elevation %.0f..%.0f deg -> %dx%d texture covering %.0f deg of azimuth; %d pole faces '
        '(azimuth span > %.0f deg) collapsed to one uv point; signed uv area < 0 for %.0f%% of the others (sgn %+.0f)' % (
            M, ph.min(), ph.max(), el_lo, el_hi, w, h, cfg['span'], int(pole.sum()), cfg['pole_deg'], 100.0 * (a[~pole] < 0).mean(), sgn))

    # ---- density: rasterise every face in head space, value = the CURRENT density at the barycentric old uv
    old_img = alpha if has_density else np.ones((2, 2))
    dens = np.zeros((h, w))
    cov = np.zeros((h, w), bool)
    dx_shift = 360.0 / mp.span * w                         # one azimuth turn in texels
    nfill = 0
    for fi in range(M):
        if pole[fi]:
            continue
        px = U[fi, :, 0] * w - 0.5
        py = U[fi, :, 1] * h - 0.5
        den = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2])
        if abs(den) < 1e-12:
            continue
        wrote = False
        for sh in (-dx_shift, 0.0, dx_shift):
            qx = px + sh
            x0, x1 = max(int(np.floor(qx.min())), 0), min(int(np.ceil(qx.max())), w - 1)
            y0, y1 = max(int(np.floor(py.min())), 0), min(int(np.ceil(py.max())), h - 1)
            if x0 > x1 or y0 > y1:
                continue
            gy, gx = np.mgrid[y0:y1 + 1, x0:x1 + 1]
            l0 = ((py[1] - py[2]) * (gx - qx[2]) + (qx[2] - qx[1]) * (gy - py[2])) / den
            l1 = ((py[2] - py[0]) * (gx - qx[2]) + (qx[0] - qx[2]) * (gy - py[2])) / den
            l2 = 1 - l0 - l1
            ins = (l0 >= -1e-9) & (l1 >= -1e-9) & (l2 >= -1e-9)
            if not ins.any():
                continue
            b = np.stack([l0[ins], l1[ins], l2[ins]], 1)
            ouv = b @ U_old[fi]
            dens[gy[ins], gx[ins]] = bilerp(old_img, ouv)
            cov[gy[ins], gx[ins]] = True
            wrote = True
        if not wrote:                                          # thinner than a texel: write the centroid texel
            for sh in (0.0,):
                tx_ = int(np.clip(round(px.mean() - 0.0), 0, w - 1)); ty_ = int(np.clip(round(py.mean()), 0, h - 1))
                dens[ty_, tx_] = bilerp(old_img, U_old[fi].mean(0)[None, :])[0]
                cov[ty_, tx_] = True
                nfill += 1
    for fi in np.where(pole)[0]:                               # pole faces last: they win their texel
        pts = np.vstack([U_old[fi], U_old[fi].mean(0)[None, :]])
        v = float(bilerp(old_img, pts).mean())
        qx, qy = Uc[fi, 0] * w - 0.5, Uc[fi, 1] * h - 0.5
        for xx in (int(np.floor(qx)), int(np.floor(qx)) + 1):
            for yy in (int(np.floor(qy)), int(np.floor(qy)) + 1):
                if 0 <= xx < w and 0 <= yy < h:
                    dens[yy, xx] = v
                    cov[yy, xx] = True
    if not cov.all():                                          # bleed into uncovered texels (bilinear edges)
        from scipy.ndimage import distance_transform_edt
        _d, (iy, ix) = distance_transform_edt(~cov, return_indices=True)
        dens = dens[iy, ix]
    log('  head_tex density: %.1f%% of the texels inside growth faces (%d faces thinner than a texel), mean %.3f' % (
        100.0 * cov.mean(), nfill, float(dens[cov].mean())))

    # ---- fields in head space (analytic, per texel)
    gx_, gy_ = np.meshgrid((np.arange(w) + 0.5) / w, (np.arange(h) + 0.5) / h)
    tphi, tel = mp.inv(gx_, gy_)
    need_line = bool(cfg.get('fade')) or bool(cfg.get('skin'))
    bins = line = None
    sdeg = None
    lo_edge = hi_edge = None
    if need_line:
        bins, line = hairline_field(head, ch, alpha, cfg['phi_max'])
        ok = ~np.isnan(line)
        lo_edge, hi_edge = float(bins[ok].min()), float(bins[ok].max())
        # texture azimuth wraps at the back: only the |phi| < phi_max part is a hairline
        sdeg = sdeg_of(tphi, tel, bins, line)
        log('  head_tex hairline curve: defined for azimuth %+.0f..%+.0f deg, elevation %.1f..%.1f' % (
            lo_edge, hi_edge, float(np.nanmin(line)), float(np.nanmax(line))))
    gfade = np.ones((h, w))
    fd = cfg.get('fade')
    if fd:
        band, lo = fd.get('band_deg', 5.0), fd.get('lo', 0.1)
        pm = fd.get('phi_max', cfg['phi_max'])
        g = lo + (1 - lo) * np.clip(sdeg / band, 0, 1)
        gfade = np.where(np.abs(tphi) < pm, g, 1.0)
        dens = dens * gfade
        log('  head_tex fade: density x %.2f..1 over %.1f deg behind the hairline, |azimuth| < %.0f' % (lo, band, pm))
    dens8 = np.clip(np.round(dens * 255.0), 0, 255)

    # ---- colours
    import scalp_tex as S_
    carded = L.get('use_card_texture') and not L.get('card_density_only')
    root = L['hair_rgb'] if (carded and L.get('hair_rgb')) else L['root_color']
    tip = L['hair_rgb'] if (carded and L.get('hair_rgb')) else L['tip_color']
    root_lin = S_.srgb2lin(np.array(root, float))
    cw, chh = cfg['cw'], cfg['ch']
    rimg = np.tile(np.array(root, float), (chh, cw, 1))
    skin_w = np.zeros((chh, cw))
    if cfg.get('skin') and cfg['colour']:
        cx_, cy_ = np.meshgrid((np.arange(cw) + 0.5) / cw, (np.arange(chh) + 0.5) / chh)
        cphi, cel = mp.inv(cx_, cy_)
        csd = sdeg_of(cphi, cel, bins, line)
        skin = np.array(cfg['skin'], float)
        t = smoothstep(csd / max(cfg['skin_band_deg'], 1e-6))
        wphi = np.clip((cphi - lo_edge) / cfg['skin_taper_deg'], 0, 1) * np.clip((hi_edge - cphi) / cfg['skin_taper_deg'], 0, 1)
        wphi = np.where(np.abs(cphi) < cfg['phi_max'], wphi, 0.0)
        skin_w = (1 - t) * wphi                                # 1 = skin
        lin = S_.srgb2lin(skin)[None, None, :] * skin_w[..., None] + root_lin[None, None, :] * (1 - skin_w[..., None])
        rimg = S_.lin2srgb(lin)
        log('  head_tex skin %s: band %.1f deg (smoothstep from the hairline forward, blended in linear light), taper %.0f deg' % (
            list(cfg['skin']), cfg['skin_band_deg'], cfg['skin_taper_deg']))
    tw = th = 128
    timg = np.tile(np.array(tip, float), (th, tw, 1))

    # ---- encode + pack
    base = base_path
    paths = dict(density=prefix + base + '_ht_density.xbm', root=prefix + base + '_ht_root.xbm', tip=prefix + base + '_ht_tip.xbm')
    files = []
    raw_d = encode_dxt5_alpha(dens8)
    files.append((paths['density'], make_xbm(CARD_STUB, w, h, raw_d, paths['density'], w // 4 * 16)))
    if cfg['colour']:
        raw_r = encode_dxt1(rimg)
        raw_t = encode_dxt1(timg)
        files.append((paths['root'], make_xbm(DXT1_PROXY, cw, chh, raw_r, paths['root'], cw // 4 * 8)))
        files.append((paths['tip'], make_xbm(DXT1_PROXY, tw, th, raw_t, paths['tip'], tw // 4 * 8)))
    for p, d in files:
        log('  head_tex file %8d  %s' % (len(d), p))
    new_uvs = U.reshape(-1, 2)
    return dict(uvs=new_uvs, files=files, paths=paths, cfg=cfg, mapping=mp, U=U, U_old=U_old, F=F, pole=pole,
                dens=dens, dens8=dens8, root_img=rimg, tip_img=timg, skin_w=skin_w, gfade=gfade, raw_density=raw_d,
                root_rgb=list(root), tip_rgb=list(tip), cov=cov, files_by_path=dict(files), bins=bins, line=line, roots=R, head_c=c)


def apply_look(ht, look, log=print):
    """redirect the material's texture slots to the head-space textures (textureBrightness 1: the texture IS the colour)"""
    pm, gm = 'physicalMaterials', 'graphicalMaterials'
    look['textures'][(pm, 'volume', 'densityTex')] = ht['paths']['density']
    look['set'][(pm, 'volume', 'densityTexChannel')] = ('EHairTextureChannel', 'ALPHA')
    look['set'][(pm, 'volume', 'usePixelDensity')] = ('Bool', 1)
    if ht['cfg']['colour']:
        look['textures'][(gm, 'color', 'rootColorTex')] = ht['paths']['root']
        look['textures'][(gm, 'color', 'tipColorTex')] = ht['paths']['tip']
        look['set'][(gm, 'color', 'textureBrightness')] = ('Float', 1.0)
    log('  head_tex: densityTex%s -> head-space textures, textureBrightness 1' % (' / rootColorTex / tipColorTex' if ht['cfg']['colour'] else ''))


# ---------------------------------------------------------------------------------------------- check

def check(ht, alpha, has_density, out_dir=None, n_per_face=4, log=print):
    """per-root comparison: random points inside every growth face; OLD value = card alpha (bilinear) at the barycentric
    interpolation of the old UVs (x the fade gradient when `fade` is on); NEW value = the packed + DECODED head-space
    DXT5 density at the barycentric head-space UV. Also writes the three texture previews + a side-by-side image."""
    from PIL import Image
    w, h = ht['cfg']['w'], ht['cfg']['h']
    rng = np.random.default_rng(1)
    M = len(ht['F'])
    r1, r2 = rng.random((M, n_per_face)), rng.random((M, n_per_face))
    f = r1 + r2 > 1
    r1[f], r2[f] = 1 - r1[f], 1 - r2[f]
    bary = np.stack([1 - r1 - r2, r1, r2], -1)                 # (M,n,3)
    old_uv = np.einsum('mnk,mkc->mnc', bary, ht['U_old']).reshape(-1, 2)
    new_uv = np.einsum('mnk,mkc->mnc', bary, ht['U']).reshape(-1, 2)
    old_img = alpha if has_density else np.ones((2, 2))
    old = bilerp(old_img, old_uv)
    dec = decode_dxt5_alpha(ht['raw_density'], w, h).astype(float) / 255.0
    new = bilerp(dec, new_uv)
    mp = ht['mapping']
    if ht['cfg'].get('fade'):                                  # expected = old x gradient at the root's own sdeg
        fd = ht['cfg']['fade']
        pos = np.einsum('mnk,mkc->mnc', bary, ht['roots'][ht['F']]).reshape(-1, 3)
        phi, el = proj(pos, ht['head_c'])
        sd = sdeg_of(phi, el, ht['bins'], ht['line'])
        g = fd.get('lo', 0.1) + (1 - fd.get('lo', 0.1)) * np.clip(sd / fd.get('band_deg', 5.0), 0, 1)
        old = old * np.where(np.abs(phi) < fd.get('phi_max', ht['cfg']['phi_max']), g, 1.0)
    pole = np.repeat(ht['pole'], n_per_face)
    d = np.abs(old - new)
    st = {}
    for nm, m in (('regular faces', ~pole), ('pole faces', pole)):
        if m.any():
            st[nm] = dict(n=int(m.sum()), mean=float(d[m].mean()), p50=float(np.percentile(d[m], 50)), p90=float(np.percentile(d[m], 90)),
                          p99=float(np.percentile(d[m], 99)), max=float(d[m].max()), within_8_255=float((d[m] <= 8 / 255).mean()),
                          within_16_255=float((d[m] <= 16 / 255).mean()))
    for nm, s in list(st.items()):
        log('  check %-13s n=%d  |old-new| mean %.4f  p50 %.4f  p90 %.4f  p99 %.4f  max %.3f  <=8/255: %.1f%%  <=16/255: %.1f%%' % (
            nm, s['n'], s['mean'], s['p50'], s['p90'], s['p99'], s['max'], 100 * s['within_8_255'], 100 * s['within_16_255']))
    fm = np.abs(old.reshape(M, n_per_face).mean(1) - new.reshape(M, n_per_face).mean(1))
    st['per-face mean'] = dict(n=M, mean=float(fm.mean()), p50=float(np.percentile(fm, 50)), p90=float(np.percentile(fm, 90)),
                               max=float(fm.max()), within_8_255=float((fm <= 8 / 255).mean()))
    log('  check per-face mean (4 roots / face): |old-new| mean %.4f  p50 %.4f  p90 %.4f  max %.3f  <=8/255: %.1f%%' % (
        fm.mean(), np.percentile(fm, 50), np.percentile(fm, 90), fm.max(), 100 * st['per-face mean']['within_8_255']))
    quant = float(np.abs(dec - ht['dens8'] / 255.0).max())
    log('  check DXT5 round-trip: max |decoded - 8-bit density| %.4f (mean %.5f)' % (quant, float(np.abs(dec - ht['dens8'] / 255.0).mean())))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        Image.fromarray(np.clip(ht['dens8'], 0, 255).astype(np.uint8)).save(os.path.join(out_dir, 'density_headspace.png'))
        Image.fromarray(np.clip(dec * 255, 0, 255).astype(np.uint8)).save(os.path.join(out_dir, 'density_headspace_decoded.png'))
        Image.fromarray(np.clip(np.round(ht['root_img']), 0, 255).astype(np.uint8)).save(os.path.join(out_dir, 'rootColor_headspace.png'))
        Image.fromarray(np.clip(np.round(ht['tip_img']), 0, 255).astype(np.uint8)).save(os.path.join(out_dir, 'tipColor_headspace.png'))
        # decoded colour textures (what the game decodes)
        for key, fn in (('root', 'rootColor_decoded.png'),):
            if ht['cfg']['colour']:
                x = read_xbm(ht['files_by_path'][ht['paths'][key]])
                Image.fromarray(decode_dxt1(x['raw'], x['w'], x['h'])).save(os.path.join(out_dir, fn))
        # side by side: OLD per-root density scattered at the NEW uv positions | the head-space texture | difference
        ox, oy = (new_uv[:, 0] * w).astype(int).clip(0, w - 1), (new_uv[:, 1] * h).astype(int).clip(0, h - 1)
        left = np.zeros((h, w), np.uint8)
        left[oy, ox] = np.clip(old * 255, 1, 255).astype(np.uint8)
        from scipy.ndimage import grey_dilation
        left = grey_dilation(left, size=(3, 3))
        right = np.zeros((h, w), np.uint8)
        right[oy, ox] = np.clip(new * 255, 1, 255).astype(np.uint8)
        right = grey_dilation(right, size=(3, 3))
        diff = np.zeros((h, w), np.uint8)
        diff[oy, ox] = np.clip(d * 255 * 4, 0, 255).astype(np.uint8)
        diff = grey_dilation(diff, size=(3, 3))
        sheet = np.concatenate([left, right, diff], 1)
        Image.fromarray(sheet).save(os.path.join(out_dir, 'check_old_vs_new_vs_diff4x.png'))
        # texture with the face outlines
        tex = np.stack([np.clip(dec * 255, 0, 255)] * 3, -1).astype(np.uint8)
        Image.fromarray(tex).save(os.path.join(out_dir, 'density_decoded_rgb.png'))
    return st


def selftest():
    """encoders + xbm writer round trips (no game, no config)"""
    for rgb in [(23, 22, 21), (125, 105, 98), (228, 222, 212), (0, 0, 0), (255, 255, 255), (19, 18, 17), (150, 125, 118)]:
        px = decode_dxt1(dxt1_flat_block(rgb), 4, 4)
        err = int(np.abs(px.astype(int) - np.array(rgb)).max())
        assert err <= 1, (rgb, px[0, 0], err)
    rng = np.random.default_rng(0)
    a = np.clip(np.cumsum(rng.normal(0, 3, (64, 64)), 1) + 128, 0, 255)
    a[:8], a[8:16], a[20:30] = 0, 255, 77
    d = decode_dxt5_alpha(encode_dxt5_alpha(a), 64, 64)
    assert d[:8].max() == 0 and d[8:16].min() == 255 and np.abs(d[20:28] - 77).max() == 0, 'flat / extreme blocks must be exact'
    assert np.abs(d - np.round(a)).mean() < 2.5, np.abs(d - np.round(a)).mean()
    img = np.tile(np.array([23.0, 22.0, 21.0]), (64, 64, 1))
    x = make_xbm(DXT1_PROXY, 64, 64, encode_dxt1(img), 'a' + chr(92) + 'b.xbm', 64 // 4 * 8)
    r = read_xbm(x)
    assert (r['w'], r['h'], r['mips'], len(r['raw'])) == (64, 64, [(64, 64, 128)], 2048) and 'TCM_DXTNoAlpha' in r['compression']
    x = make_xbm(CARD_STUB, 64, 64, encode_dxt5_alpha(a), 'a' + chr(92) + 'c.xbm', 64 // 4 * 16)
    r = read_xbm(x)
    assert 'TCM_DXTAlpha' in r['compression'] and r['resident'][1] == 0 and len(r['raw']) == 4096
    print('headtex selftest OK')


# ---------------------------------------------------------------------------------------------- CLI

if __name__ == '__main__':
    import argparse
    import hairpipe as HP
    ap = argparse.ArgumentParser(description='offline head_tex build + per-root check (no install, no game)')
    ap.add_argument('cmd', choices=['check', 'selftest'])
    ap.add_argument('cfg', nargs='?', help='chars/<cfg>.json with growth.head_tex')
    ap.add_argument('--out', default=os.path.join(__import__('tempfile').gettempdir(), 'head_tex'))
    ap.add_argument('--prefix', default='')
    a = ap.parse_args()
    if a.cmd == 'selftest':
        selftest()
        sys.exit(0)
    C = HP.load_config(a.cfg)
    assert C['growth'].get('head_tex'), 'config has no growth.head_tex'
    st = HP.generate(C, prefix=a.prefix or ('hairtest' + chr(92) + C['name'].lower() + chr(92)))
    ht = st['head_tex']
    stats = check(ht, st['alpha'], bool(C['look']['use_card_texture'] or C['look'].get('card_density_only')), out_dir=os.path.join(a.out, C['name']))
    print('previews ->', os.path.join(a.out, C['name']))
