r"""scalp_recolour - opt-in config key "scalp_recolour": the painted scalp / hairline of the character's head diffuse is
recoloured to the HairWorks hair colour, so skin seen through gaps in the fur reads as hair (in shadow), not as the
vanilla card-hair paint (HAIRWORK.md "Corinne scalp recolour: opt-in config key scalp_recolour (2026-10-16)").

Generic version of scalp_tex.py (which keeps its per-character CHARS table and hand-tuned UV boxes): the head mesh and
its diffuse(s) come from the config (head_mesh, else hairpipe.discover's head of the config's appearance); size, mip
count and streamed mips from the game's texture.cache entry. scalp_tex.py supplies the DXT1 codec, the mip chain
re-encode (untouched 4x4 blocks keep the game's bytes), the stub (resident mips + CRC) and the OKLab maths.

Config key (every field optional; root / tip default to look.root_color / look.tip_color):
  "scalp_recolour": {"root": [122,116,129], "tip": [155,149,163],   target colour = root..tip at `mix`, game sRGB
                     "mix": 0.35,        0 = root, 1 = tip
                     "lum": 0.85,        OKLab lightness factor on the target (scalp sits in shadow: a bit darker)
                     "chroma": 1.0,      chroma factor on the target
                     "keep": 1.0,        strand pattern kept as lightness detail (1 = the original absolute contrast)
                     "strand_px": 3,     high-pass sigma of that detail (px at 1024)
                     "thr": 0.35,        paint fraction that counts as paint for the connected regions
                     "seed_el": 60,      crown seeds: texels above this elevation (deg) around the head centre
                     "grow": 10,         px at 1024 the paint regions grow (soft hairline fringe)
                     "dilate": 32, "feather": 4,   px at 1024: zone past the mesh triangles (island gutters, mips)
                     "perp": 0.45,       colour gate: max distance from the skin -> paint line (fraction of its length)
                     "skin_rg": 24, "skin_luma": 105, "skin_sigma": 24,   'clearly skin' pixels, local skin field
                     "solid": 1.0, "solid_px": 4, "solid_lo": .45, "solid_hi": .75}   inside the painted area the skin
                         BETWEEN the painted strands goes to the hair colour too (0 = unmix only: strand-patterned skin
                         stays pink, seen at Corinne's parting in the Studio); blur of the paint share, smoothstep lo..hi
Without the key nothing in hairpipe / batch / desk_prep changes (approved builds byte-identical).

a = paint fraction: OKLab projection of the pixel on the line local skin S -> paint P (S = normalised convolution of
clearly-skin pixels, P = median of the darkest quarter of the crown texels), x a gate on the distance from that line.
Zone (geometry finds the skull, colour finds the paint): the head's LOD0 triangles are rasterised in UV with their 3D
positions; the connected regions of a > thr that contain a crown texel (elevation > seed_el) are kept, so brows, eye
shadow, nostrils, lips, the mouth island and the ears (paint-like colours, not connected to the skull paint) stay out;
grown by `grow`, extended `dilate` px past the island edges (gutters), feathered. w = zone x a x gate.
Recolour (OKLab unmix): new = lab + w (T - P): each pixel moves by its paint share only (pure paint -> target, a
half-painted hairline pixel half way, skin untouched: no halo, no overshoot). The unmix reads the strand pattern as
coverage, so it is put back as lightness detail: L -= keep x zone x gate x highpass(a) x (S_L - P_L).

  py tools\hairworks\scalp_recolour.py <cfg> info                 head meshes / diffuse textures / cache entries
  py tools\hairworks\scalp_recolour.py <cfg> preview [--k v ...]  previews -> build\scalp_<char>\previews\
  py tools\hairworks\scalp_recolour.py <cfg> build [--identity]   standalone mod build\scalp_<char>\modScalp_<char>
  py tools\hairworks\scalp_recolour.py <cfg> verify [--identity]  re-read it, compare to the game (identity: byte-equal)
(--k v overrides a key of the config's scalp_recolour, e.g. --lum 0.8; a config without the key uses the defaults)
"""
import argparse
import json
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)
import scalp_tex as ST
import texcache as TC

KEY = 'scalp_recolour'
DEF = dict(root=None, tip=None, mix=0.35, lum=0.85, chroma=1.0, keep=1.0, thr=0.35, seed_el=60.0,
           grow=10.0, dilate=32.0, strand_px=3.0, feather=4.0, perp=0.45, skin_rg=24.0, skin_luma=105.0, skin_sigma=24.0,
           solid=1.0, solid_px=4.0, solid_lo=0.45, solid_hi=0.75)


def params(C, over=None):
    p = dict(DEF)
    v = C.get(KEY)
    if isinstance(v, dict):
        p.update(v)
    for k, x in (over or {}).items():
        if x is not None:
            p[k] = x
    look = C.get('look') or {}
    p['root'] = list(p['root'] or look.get('root_color') or (128, 128, 128))
    p['tip'] = list(p['tip'] or look.get('tip_color') or p['root'])
    return p


def char_of(C):
    return (C.get('lab') or {}).get('char') or C['name'].split('_')[0]


def build_dir(C):
    return os.path.join(ROOT, 'build', 'scalp_' + char_of(C))


# ---------------------------------------------------------------------------------------------- head lookup

def heads(C):
    """head meshes of the config's appearance (config head_mesh first)"""
    import hairpipe as H
    if C.get('head_mesh'):
        return [C['head_mesh']]
    info = H.discover(C, log=lambda *a: None)
    return list(info.get('heads') or [])


def head_textures(C):
    """[(head mesh, diffuse xbm rel)] distinct diffuses of the head meshes' materials (every LOD material)"""
    import gamefiles as G
    out, seen = [], set()
    for h in heads(C):
        for _base, prm in G.materials(h):
            d = None
            for k in ('Diffuse', 'Diffusemap', 'diffuse', 'DiffuseTexture'):
                x = prm.get(k)
                if isinstance(x, dict) and x.get('@import'):
                    d = x['@import']
                    break
            if d and d.lower() not in seen:
                seen.add(d.lower())
                out.append((h, d))
    return out


_CACHE = None


def game_cache():
    global _CACHE
    if _CACHE is None:
        _CACHE = TC.TextureCache(TC.GAME_CACHE)
    return _CACHE


def select(C, mesh, rel):
    """point scalp_tex's globals at this texture (size / mips / streamed count from the game's cache entry)"""
    e = game_cache().by_name.get(rel.lower())
    assert e is not None, 'no texture.cache entry for %s' % rel
    _mi, n = struct.unpack_from('<ii', e.raw, 32)
    assert e.w == e.h, 'square textures only (%s %dx%d)' % (rel, e.w, e.h)
    key = 'cfg:%s:%s' % (C['name'], os.path.basename(rel).lower())
    ST.CHARS[key] = dict(mode='recolour', rel=rel, mesh=mesh, mod='modScalp_' + char_of(C),
                         build=os.path.relpath(os.path.join(build_dir(C), os.path.splitext(os.path.basename(rel))[0]),
                                               os.path.join(ROOT, 'build')),
                         prev='scalp_' + char_of(C), w=e.w, nmip=e.mips, nstream=n & 0xFFFF,
                         root=(0, 0, 0), tip=(0, 0, 0), config=C['name'])
    ST.set_char(key)
    res = sum(L[5] for L in ST.mip_layout()[ST.NSTREAM:])
    assert res == ST.RES_BYTES, 'resident mip bytes %d != %d (scalp_tex.make_stub layout)' % (res, ST.RES_BYTES)
    return e


# ---------------------------------------------------------------------------------------------- zone (geometry)

def texel_map(mesh, S):
    """rasterise every LOD0 triangle of the head at S x S: (pos (S,S,3) float32, cov (S,S) bool)"""
    import gamefiles as G
    m = G.load_mesh(mesh)
    pos = np.zeros((S, S, 3), np.float32)
    cov = np.zeros((S, S), bool)
    for ci in m['lod0']:
        ch = m['chunks'][ci]
        uv, P, T = ch['uv'], ch['pos'], ch['idx'].reshape(-1, 3)
        px = np.c_[uv[:, 0] * S, uv[:, 1] * S]
        for t in T:
            q = px[t]
            area = (q[1, 0] - q[0, 0]) * (q[2, 1] - q[0, 1]) - (q[2, 0] - q[0, 0]) * (q[1, 1] - q[0, 1])
            if abs(area) < 1e-9:
                continue
            ax, bx = int(max(0, np.floor(q[:, 0].min()))), int(min(S, np.ceil(q[:, 0].max()) + 1))
            ay, by = int(max(0, np.floor(q[:, 1].min()))), int(min(S, np.ceil(q[:, 1].max()) + 1))
            if bx <= ax or by <= ay:
                continue
            yy, xx = np.mgrid[ay:by, ax:bx]
            fx, fy = xx + 0.5 - q[0, 0], yy + 0.5 - q[0, 1]
            l1 = (fx * (q[2, 1] - q[0, 1]) - (q[2, 0] - q[0, 0]) * fy) / area
            l2 = ((q[1, 0] - q[0, 0]) * fy - fx * (q[1, 1] - q[0, 1])) / area
            l0 = 1 - l1 - l2
            k = (l0 >= -0.01) & (l1 >= -0.01) & (l2 >= -0.01)
            if not k.any():
                continue
            pos[yy[k], xx[k]] = l0[k][:, None] * P[t[0]] + l1[k][:, None] * P[t[1]] + l2[k][:, None] * P[t[2]]
            cov[yy[k], xx[k]] = True
    return pos, cov


def zone(C, mesh, p, S, a):
    """float (S,S) 0..1 UV zone + diagnostics. Geometry finds the skull, colour finds the paint: the connected regions
    of paint (a > thr) that contain a SEED texel (crown: elevation > seed_el around the head centre, or the back of
    the head: |azimuth| > 120 deg, elevation > seed_back_el); brows, eye shadow, nostrils, lips, the mouth island and
    the ears are paint-coloured too but not connected to the skull paint. Grown by `grow` px (the soft hairline
    fringe, where a < thr), extended past the island edges by `dilate` px (gutters: mips / bilinear never sample
    unrecoloured paint at a seam), feathered."""
    from scipy.ndimage import gaussian_filter, distance_transform_edt, label, binary_dilation
    cov, phi, el = head_geo(mesh, S)
    f = S / 1024.0
    paint = cov & (a > p['thr'])
    seed = paint & (el > p['seed_el'])
    lab_, n = label(paint, structure=np.ones((3, 3)))
    keep = np.unique(lab_[seed])
    keep = keep[keep > 0]
    reg = np.isin(lab_, keep)
    r = max(1, int(round(p['grow'] * f)))
    yy, xx = np.mgrid[-r:r + 1, -r:r + 1]
    reg = binary_dilation(reg, structure=(xx * xx + yy * yy) <= r * r) & cov
    z = reg.astype(np.float64)
    dist, (iy, ix) = distance_transform_edt(~cov, return_indices=True)
    near = (~cov) & (dist <= p['dilate'] * f)
    z[near] = z[iy[near], ix[near]]
    z = gaussian_filter(z, p['feather'] * f * 0.5)
    z = np.where(cov | near, z, 0.0)
    return np.clip(z, 0, 1), dict(cov=cov, phi=phi, el=el, seed=seed, ncomp=len(keep))


# ---------------------------------------------------------------------------------------------- colour

def skin_field(rgb, lab, p):
    """local skin colour (OKLab) by normalised convolution of 'clearly skin' pixels (pinkish and bright)"""
    from scipy.ndimage import gaussian_filter
    f = rgb.shape[0] / 1024.0
    r, g, b_ = [rgb[..., i].astype(np.float64) for i in range(3)]
    luma = 0.299 * r + 0.587 * g + 0.114 * b_
    sk = ((r - g) > p['skin_rg']) & (luma > p['skin_luma'])
    sk = sk.astype(np.float64)
    sig = p['skin_sigma'] * f
    out = np.zeros_like(lab)
    den = gaussian_filter(sk, sig) + 1e-9
    wide = gaussian_filter(sk, sig * 4) + 1e-9
    for c in range(3):
        near = gaussian_filter(lab[..., c] * sk, sig) / den
        far = gaussian_filter(lab[..., c] * sk, sig * 4) / wide
        wn = np.clip(den / 0.2, 0, 1)
        out[..., c] = near * wn + far * (1 - wn)
    return out, sk


def paint_ref(lab, sel):
    """OKLab paint reference: median of the darkest quarter of the selected texels (the crown: solid paint)"""
    L = lab[..., 0][sel]
    thr = np.percentile(L, 25)
    return np.median(lab[sel][L <= thr], axis=0)


def head_geo(mesh, S):
    """(cov, phi deg, el deg) per texel around growth.Head's centre (memoised)"""
    k = (mesh.lower(), S)
    if k not in _GEO:
        import growth
        head = growth.Head(mesh)
        pos, cov = texel_map(mesh, S)
        d = pos - head.c
        _GEO[k] = (cov, np.degrees(np.arctan2(d[..., 0], d[..., 1])),
                   np.degrees(np.arctan2(d[..., 2], np.hypot(d[..., 0], d[..., 1]))))
    return _GEO[k]


_GEO = {}


def target_lab(p):
    t = p['mix']
    rgb = np.array(p['root'], np.float64) * (1 - t) + np.array(p['tip'], np.float64) * t
    lab = ST.rgb2oklab(rgb[None])[0]
    lab[0] *= p['lum']
    lab[1:] *= p['chroma']
    return lab, rgb


def paint_fraction(lab, S_, P, p):
    """a = position of the pixel on the line local skin S -> paint P (OKLab, 0 skin .. 1 paint) x a gate on its distance
    from that line (red ear / lip / mouth colours are off the line)"""
    v = P[None, None, :] - S_
    vv = (v * v).sum(-1) + 1e-12
    a = np.clip(((lab - S_) * v).sum(-1) / vv, 0, 1)
    perp = np.sqrt(np.maximum(((lab - S_ - a[..., None] * v) ** 2).sum(-1) / vv, 0))
    return a, 1 - ST.smooth(perp, p['perp'] * 0.6, p['perp'])


def recolour(lab, S_, P, a, w, zg, p, fill=None):
    """unmix recolour (module doc): -> float RGB. w = zone x gate x a moves each pixel by its paint share (T - P);
    the strand pattern (which the unmix reads as paint coverage a) is put back as lightness detail: high-pass of a
    (sigma `strand_px`) x the original skin -> paint lightness step x `keep` (1 = the original absolute contrast)"""
    from scipy.ndimage import gaussian_filter
    T, _trgb = target_lab(p)
    new = lab + w[..., None] * (T - P)[None, None, :]
    f = lab.shape[0] / 1024.0
    hp = a - gaussian_filter(a, p['strand_px'] * f)
    det = p['keep'] * zg * hp * np.maximum(S_[..., 0] - P[0], 0)
    new[..., 0] -= det
    if fill is not None:
        # solid fill: where the neighbourhood is mostly paint, the skin between the strands goes to the target too
        # (strand pattern kept as the same lightness detail)
        tgt = np.broadcast_to(T, new.shape).copy()
        tgt[..., 0] -= det
        new = new + fill[..., None] * (tgt - new)
    return ST.oklab2rgb(new)


def compute(C, mesh, rel, p, rgb0):
    """-> (rec float RGB, weight, zone, zone diagnostics, colour diagnostics)"""
    S = rgb0.shape[0]
    lab = ST.rgb2oklab(rgb0)
    S_, sk = skin_field(rgb0, lab, p)
    cov, phi, el = head_geo(mesh, S)
    P = paint_ref(lab, cov & (el > p['seed_el']))
    a, gate = paint_fraction(lab, S_, P, p)
    z, zd = zone(C, mesh, p, S, a * gate)
    w = z * a * gate
    fill = None
    if p.get('solid', 0) > 0:
        from scipy.ndimage import gaussian_filter
        fill = z * p['solid'] * ST.smooth(gaussian_filter(a * gate, p['solid_px'] * S / 1024.0), p['solid_lo'], p['solid_hi'])
        w = np.maximum(w, fill)
    rec = recolour(lab, S_, P, a, z * a * gate, z * gate, p, fill)
    # pixels with w ~ 0 keep their exact value (no OKLab round trip noise)
    rec = np.where(w[..., None] > 1e-4, rec, rgb0.astype(np.float64))
    T, trgb = target_lab(p)
    return rec, w, z, zd, dict(P=P, T=T, trgb=trgb, skin=sk)


# ---------------------------------------------------------------------------------------------- build pieces

def textures_for(C, p=None, log=print, identity=False, previews=False):
    """-> [(rel, stub bytes, mips [raw], stats)] for every diffuse of the config's head(s)"""
    p = p or params(C)
    out = []
    for mesh, rel in head_textures(C):
        select(C, mesh, rel)
        o = ST.get_orig()
        if identity:
            mips, stats = list(o['mips']), []
        else:
            rgb0 = ST.decode_mip(o['mips'][0], 0)
            rec, w, z, zd, cd = compute(C, mesh, rel, p, rgb0)
            mips, stats = ST.encode_chain(o, rgb0, rec)
            sel = w > 0.5
            log('scalp_recolour %s: %s %dx%d, %d mips (%d streamed); re-encoded blocks per mip %s; paint mean %s -> %s'
                ' (target %s), w>0.5 %d px' % (C['name'], rel, ST.W, ST.H, ST.NMIP, ST.NSTREAM,
                                                [s[1] for s in stats], rgb0[sel].mean(0).round(1),
                                                rec[sel].mean(0).round(1), np.round(ST.oklab2rgb(cd['T'][None])[0], 1),
                                                int(sel.sum())))
            if previews:
                write_previews(C, mesh, rel, rgb0, rec, w, z, zd, cd, p)
        stub = ST.make_stub(o['stub'], mips)
        out.append((rel, stub, mips, stats))
    return out


def write_cache(path, items):
    """multi-entry v6 texture.cache (scalp_tex.write_cache_v6 layout, one entry per item): items [(rel, mips)]"""
    blob = bytearray()
    strings = bytearray()
    ents = bytearray()
    mtab = []
    for rel, mips in items:
        select_by_rel(rel)
        b, ends = ST.pack_entry(mips)
        page = len(blob) // 4096
        blob += b + bytes(-len(b) % 4096)
        ents += struct.pack('<IiIIIIHHHHiiQBBH', TC.name_hash(rel), len(strings), page, len(b), sum(len(m) for m in mips),
                            16, ST.W, ST.H, ST.NMIP, 1, len(mtab), len(ends), 0, 7, 4, 0)
        mtab += ends
        strings += rel.encode('latin1') + b'\0'
    with open(path, 'wb') as f:
        f.write(blob)
        f.write(struct.pack('<%dI' % len(mtab), *mtab))
        f.write(strings)
        f.write(ents)
        f.write(struct.pack('<QIIII4sI', 0, len(blob) // 4096, len(items), len(strings), len(mtab), b'HCXT', 6))


def select_by_rel(rel):
    for k, v in ST.CHARS.items():
        if k.startswith('cfg:') and v['rel'].lower() == rel.lower():
            ST.set_char(k)
            return
    raise KeyError(rel)


def add_to_mod(C, files, log=print):
    """hairpipe.build hook (only for configs with the key): stub xbm(s) appended to `files` (blob0.bundle);
    returns [(rel, mips)] for write_cache(content\\texture.cache)"""
    items = []
    have = {r.lower() for r, _ in files}
    for rel, stub, mips, _st in textures_for(C, params(C), log=log):
        if rel.lower() in have:
            log('scalp_recolour: %s already in the mod, skipped' % rel)
            continue
        files.append((rel, stub))
        have.add(rel.lower())
        items.append((rel, mips))
    return items


def recoloured_image(C, rel=None, maxsize=1024, log=print):
    """Studio (desk_prep.head_textured): PIL image of the recoloured diffuse `rel` (default: the first one), or None
    when the config's heads do not use it (cached in build_dir by the hash of the key's params)"""
    import hashlib
    p = params(C)
    th = [t for t in head_textures(C) if rel is None or t[1].lower() == rel.lower()]
    if not th:
        return None
    mesh, rel = th[0]
    h = hashlib.sha1(json.dumps([rel, p], sort_keys=True).encode()).hexdigest()[:10]
    fp = os.path.join(build_dir(C), 'studio_%s_%s.png' % (os.path.splitext(os.path.basename(rel))[0], h))
    if not os.path.exists(fp):
        select(C, mesh, rel)
        o = ST.get_orig()
        rgb0 = ST.decode_mip(o['mips'][0], 0)
        rec = compute(C, mesh, rel, p, rgb0)[0]
        os.makedirs(os.path.dirname(fp), exist_ok=True)
        Image.fromarray(np.clip(rec + 0.5, 0, 255).astype(np.uint8)).save(fp)
        log('  scalp_recolour: %s' % fp)
    img = Image.open(fp).convert('RGB')
    if max(img.size) > maxsize:
        img = img.resize((maxsize, maxsize))
    return img


# ---------------------------------------------------------------------------------------------- previews

def write_previews(C, mesh, rel, rgb0, rec, w, z, zd, cd, p):
    d = os.path.join(build_dir(C), 'previews')
    os.makedirs(d, exist_ok=True)
    S = rgb0.shape[0]
    A = Image.fromarray(rgb0)
    B = Image.fromarray(np.clip(rec + 0.5, 0, 255).astype(np.uint8))
    k = 1024 / S
    full = Image.new('RGB', (2048 + 8, 1024), (255, 255, 255))
    full.paste(A.resize((1024, 1024), Image.BOX), (0, 0))
    full.paste(B.resize((1024, 1024), Image.BOX), (1032, 0))
    full.save(os.path.join(d, 'before_after_full_1k.png'))
    B.save(os.path.join(d, 'after_full.png'))
    # 1:1 crops: hairline band (top), left / right skull islands (bottom); boxes from the weight's extent
    ys, xs = np.nonzero(w > 0.05)
    boxes = []
    top = ys < S * 0.5
    if top.any():
        boxes.append(('hairline', xs[top].min() - 16, max(0, ys[top].min() - 16), xs[top].max() + 16, ys[top].max() + 16))
    bot = ys >= S * 0.5
    if bot.any():
        xb, yb = xs[bot], ys[bot]
        L, R = xb < S / 2, xb >= S / 2
        if L.any():
            boxes.append(('skull_left', xb[L].min() - 16, yb[L].min() - 16, xb[L].max() + 16, yb[L].max() + 16))
        if R.any():
            boxes.append(('skull_right', xb[R].min() - 16, yb[R].min() - 16, xb[R].max() + 16, yb[R].max() + 16))
    for nm, x0, y0, x1, y1 in boxes:
        x0, y0, x1, y1 = max(0, x0), max(0, y0), min(S, x1), min(S, y1)
        pr = Image.new('RGB', (x1 - x0, 2 * (y1 - y0) + 8), (255, 255, 255))
        pr.paste(A.crop((x0, y0, x1, y1)), (0, 0))
        pr.paste(B.crop((x0, y0, x1, y1)), (0, y1 - y0 + 8))
        pr.save(os.path.join(d, 'crop_%s_1to1.png' % nm))
    # mask overlay: weight green, zone outline blue
    ov = np.asarray(A, np.float64).copy()
    ov = ov * (1 - w[..., None] * 0.8) + np.array([0, 255, 0]) * w[..., None] * 0.8
    edge = (z > 0.5) ^ np.roll(z > 0.5, 1, 0) | (z > 0.5) ^ np.roll(z > 0.5, 1, 1)
    ov[edge] = (40, 90, 255)
    Image.fromarray(ov.astype(np.uint8)).save(os.path.join(d, 'mask_overlay.png'))
    Image.fromarray((w * 255).astype(np.uint8)).save(os.path.join(d, 'weight.png'))
    Image.fromarray((z * 255).astype(np.uint8)).save(os.path.join(d, 'zone.png'))
    # UV wireframe QA: LOD0 triangles over the after image, zone edge blue
    import gamefiles as G
    m = G.load_mesh(mesh)
    wf = B.copy()
    dr = ImageDraw.Draw(wf)
    for ci in m['lod0']:
        ch = m['chunks'][ci]
        uv, T = ch['uv'], ch['idx'].reshape(-1, 3)
        for t in T:
            pts = [(float(uv[i, 0]) * S, float(uv[i, 1]) * S) for i in t]
            dr.line(pts + [pts[0]], fill=(255, 255, 0), width=1)
    wfa = np.asarray(wf).copy()
    wfa[edge] = (40, 90, 255)
    Image.fromarray(wfa).save(os.path.join(d, 'uv_wireframe_qa.png'))
    diff = np.abs(np.clip(rec + 0.5, 0, 255).astype(int) - rgb0.astype(int)).max(2)
    Image.fromarray(np.clip(diff * 3, 0, 255).astype(np.uint8)).save(os.path.join(d, 'diff_x3.png'))
    print('previews ->', d)


# ---------------------------------------------------------------------------------------------- standalone mod / verify

def mod_dir(C, identity=False):
    return os.path.join(build_dir(C), 'modScalp_%s%s' % (char_of(C), '_identity' if identity else ''))


def cmd_build(C, p, identity=False, log=print):
    import shutil
    import w3lib
    tx = textures_for(C, p, log=log, identity=identity, previews=not identity)
    out = mod_dir(C, identity)
    content = os.path.join(out, 'content')
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(content)
    write_cache(os.path.join(content, 'texture.cache'), [(r, m) for r, _s, m, _t in tx])
    b = w3lib.write_legacy_bundle(os.path.join(content, 'blob0.bundle'), [(r, s) for r, s, _m, _t in tx])
    w3lib.write_legacy_metadata_store(os.path.join(content, 'metadata.store'), [b])
    log('built %s (%d texture(s))' % (out, len(tx)))
    return out


def verify(content, identity=False, log=print):
    """re-read texture.cache + blob0.bundle of a content folder: every entry vs the game (identity: byte-equal)"""
    import w3lib
    c = TC.TextureCache(os.path.join(content, 'texture.cache'))
    assert c.version == 6
    bp = os.path.join(content, 'blob0.bundle')
    stubs = {e.name.lower(): w3lib.bundle_read(bp, e) for e in w3lib.bundle_entries(bp)}
    for e in c.entries:
        select_by_rel(e.name)
        o = ST.get_orig()
        assert e.hash == TC.name_hash(e.name) and (e.w, e.h, e.mips) == (ST.W, ST.H, ST.NMIP), 'entry header'
        mips = ST.split_mips(ST.parse_entry_blocks(c.path, e, c.mips))
        assert sum(len(m) for m in mips) == e.size == sum(L[5] for L in ST.mip_layout())
        stub = stubs[e.name.lower()]
        cr = w3lib.CR2W(stub)
        assert cr.verify_crcs() == (0, True), 'stub crc'
        diff = [i for i in range(len(stub) - ST.RES_BYTES) if stub[i] != o['stub'][i]]
        crc_at = (0x20, 0x60, cr.tables[4][0] + 20)
        assert all(any(q <= i < q + 4 for q in crc_at) for i in diff), 'stub header differs %s' % diff
        assert stub[-ST.RES_BYTES:] == b''.join(mips[ST.NSTREAM:]), 'stub resident mips != cache resident block'
        same = 0
        for k in range(ST.NMIP):
            r0, r1 = ST.decode_mip(o['mips'][k], k), ST.decode_mip(mips[k], k)
            be = o['mips'][k] == mips[k]
            same += be
            nb = len(mips[k]) // 8
            ch = sum(1 for i in range(nb) if mips[k][8 * i:8 * i + 8] != o['mips'][k][8 * i:8 * i + 8])
            log('  %s mip %2d %4dx%-4d bytes equal %-5s blocks changed %6d / %-6d maxdiff %d' % (
                os.path.basename(e.name), k, r0.shape[1], r0.shape[0], be, ch, nb,
                int(np.abs(r0.astype(int) - r1.astype(int)).max())))
        if identity:
            assert same == ST.NMIP and stub == o['stub'], 'IDENTITY FAILED'
            log('IDENTITY OK: %s all %d mips + stub byte-identical to the game' % (e.name, ST.NMIP))
        if identity:
            # the cache blob itself: our zlib(9) of the game's raw mips (the game's own blob is v7, not comparable)
            b, _ends = ST.pack_entry(o['mips'])
            with open(c.path, 'rb') as f:
                f.seek(e.page * 4096)
                assert f.read(e.zsize) == b, 'cache blob != pack_entry(game mips)'
    log('verify OK: %s (%d entries)' % (content, len(c.entries)))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cfg')
    ap.add_argument('cmd', choices=['info', 'preview', 'build', 'verify'])
    ap.add_argument('--identity', action='store_true')
    for k, v in DEF.items():
        if k not in ('root', 'tip'):
            ap.add_argument('--' + k, type=float, default=None)
    ap.add_argument('--root', type=lambda s: [float(x) for x in s.split(',')], default=None)
    ap.add_argument('--tip', type=lambda s: [float(x) for x in s.split(',')], default=None)
    a = ap.parse_args()
    import hairpipe as H
    C = H.load_config(a.cfg)
    p = params(C, {k: getattr(a, k) for k in DEF})
    if a.cmd == 'info':
        print('params', p)
        for mesh, rel in head_textures(C):
            e = game_cache().by_name.get(rel.lower())
            print('head %s\n  diffuse %s  cache %s' % (mesh, rel, '%dx%d %d mips, %d B' % (e.w, e.h, e.mips, e.size) if e else 'MISSING'))
    elif a.cmd == 'preview':
        textures_for(C, p, previews=True)
    elif a.cmd == 'build':
        out = cmd_build(C, p, identity=a.identity)
        verify(os.path.join(out, 'content'), identity=a.identity)
    elif a.cmd == 'verify':
        tx = head_textures(C)
        for mesh, rel in tx:
            select(C, mesh, rel)
        verify(os.path.join(mod_dir(C, a.identity), 'content'), identity=a.identity)


if __name__ == '__main__':
    main()
