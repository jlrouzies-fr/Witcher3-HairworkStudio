r"""silhouette - offline coverage comparison: vanilla cards (alpha-textured) vs our rendered hairs, orthographic views.

Vanilla: points sampled on every card triangle, kept where the card texture alpha (data pack WebP) >= ALPHA.
Ours:    interpolated hairs (barycentric blends of the 3 guides of each growth face, as HairWorks draws them),
         rasterised as polylines `width` wide.
Masks are 1 px = 1 mm, per view: front (looking at the face), back, left, right, top. Metrics per view: iou, missing
(vanilla-only / vanilla), extra (ours-only / vanilla). A hair mask hides the head where the head is visible in vanilla
too, so only hair-vs-hair is compared (head pixels are not counted as missing).
  masks(P_cards, T, UV, alpha, G, F, head) -> {view: (vanilla bool, ours bool, head bool)}
"""
import numpy as np

ALPHA = 0.35
VIEWS = {   # name: (u axis, v axis, depth axis, u sign, toward-camera sign) in game space (x right, y = face, z up)
    'front': (0, 2, 1, -1, 1), 'back': (0, 2, 1, 1, -1), 'right': (1, 2, 0, 1, 1), 'left': (1, 2, 0, -1, -1),
    'top': (0, 1, 2, 1, 1)}


def card_points(P, T, UV, alpha, n=10):
    a, b, c = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    ua, ub, uc = UV[T[:, 0]], UV[T[:, 1]], UV[T[:, 2]]
    h, w = alpha.shape
    pts = []
    for i in range(n + 1):
        for j in range(n + 1 - i):
            s, t = i / n, j / n
            p = a + (b - a) * s + (c - a) * t
            uv = ua + (ub - ua) * s + (uc - ua) * t
            x = np.clip(((uv[:, 0] % 1.0) * w).astype(int), 0, w - 1)
            y = np.clip(((uv[:, 1] % 1.0) * h).astype(int), 0, h - 1)
            pts.append(p[alpha[y, x] >= ALPHA])
    return np.concatenate(pts)


def hair_points(G, F, n=6, k_sub=3):
    """dense points along the interpolated hairs (n: barycentric grid per face, k_sub: samples per guide segment)."""
    W = []
    for i in range(n + 1):
        for j in range(n + 1 - i):
            W.append((i / n, j / n, 1 - i / n - j / n))
    W = np.array(W)
    A, B, C = G[F[:, 0]], G[F[:, 1]], G[F[:, 2]]
    H = (W[None, :, 0, None, None] * A[:, None] + W[None, :, 1, None, None] * B[:, None] + W[None, :, 2, None, None] * C[:, None]).reshape(-1, G.shape[1], 3)
    t = np.linspace(0, 1, k_sub, endpoint=False)
    seg = H[:, :-1, None, :] + (H[:, 1:, None, :] - H[:, :-1, None, :]) * t[None, None, :, None]
    return np.concatenate([seg.reshape(-1, 3), H[:, -1]])


def _raster(pts, view, box, r=0):
    u, v, _d, sg, _c = VIEWS[view]
    (u0, u1), (v0, v1) = box
    W, Hh = int(round((u1 - u0) * 1000)) + 1, int(round((v1 - v0) * 1000)) + 1
    m = np.zeros((Hh, W), bool)
    x = np.round(((pts[:, u] * sg) - u0) * 1000).astype(int)
    y = np.round((v1 - pts[:, v]) * 1000).astype(int)
    ok = (x >= 0) & (x < W) & (y >= 0) & (y < Hh)
    m[y[ok], x[ok]] = True
    if r:
        from scipy import ndimage
        m = ndimage.binary_dilation(m, iterations=r)
    return m


def _close(m, it=2):
    from scipy import ndimage
    return ndimage.binary_closing(m, iterations=it)


def head_points(V, T, n=6):
    a, b, c = V[T[:, 0]], V[T[:, 1]], V[T[:, 2]]
    out = []
    for i in range(n + 1):
        for j in range(n + 1 - i):
            out.append(a + (b - a) * (i / n) + (c - a) * (j / n))
    return np.concatenate(out)


def _depth(pts, view, box):
    """per-pixel nearest depth toward the camera (larger = nearer), -inf where empty."""
    u, v, d, sg, cs = VIEWS[view]
    (u0, u1), (v0, v1) = box
    W, Hh = int(round((u1 - u0) * 1000)) + 1, int(round((v1 - v0) * 1000)) + 1
    z = np.full((Hh, W), -np.inf)
    x = np.round(((pts[:, u] * sg) - u0) * 1000).astype(int)
    y = np.round((v1 - pts[:, v]) * 1000).astype(int)
    ok = (x >= 0) & (x < W) & (y >= 0) & (y < Hh)
    np.maximum.at(z, (y[ok], x[ok]), pts[ok, d] * cs)
    return z


def _visible(pts, view, box, headz):
    """hair points not hidden behind the head (depth test against the head's nearest surface)."""
    u, v, d, sg, cs = VIEWS[view]
    (u0, u1), (v0, v1) = box
    x = np.clip(np.round(((pts[:, u] * sg) - u0) * 1000).astype(int), 0, headz.shape[1] - 1)
    y = np.clip(np.round((v1 - pts[:, v]) * 1000).astype(int), 0, headz.shape[0] - 1)
    return pts[pts[:, d] * cs >= headz[y, x] - 0.002]


def masks(cardpts, hairpts, headv, headt=None):
    hp_dense = head_points(headv, headt) if headt is not None else headv
    allp = np.concatenate([cardpts, hairpts, headv])
    out = {}
    for view, (u, v, _d, sg, _c) in VIEWS.items():
        uu = allp[:, u] * sg
        box = ((uu.min() - 0.005, uu.max() + 0.005), (allp[:, v].min() - 0.005, allp[:, v].max() + 0.005))
        hz = _depth(hp_dense, view, box)
        from scipy import ndimage
        hz = ndimage.maximum_filter(hz, size=5)            # fill gaps between head sample points
        van = _close(_raster(_visible(cardpts, view, box, hz), view, box, 1), 2)
        our = _close(_raster(_visible(hairpts, view, box, hz), view, box, 1), 2)
        hd = _close(_raster(headv, view, box, 1), 3)
        out[view] = (van, our, hd)
    return out


def score(ms):
    res = {}
    for view, (van, our, hd) in ms.items():
        a = max(int(van.sum()), 1)
        res[view] = dict(iou=round(float((van & our).sum() / max((van | our).sum(), 1)), 3),
                         missing=round(float((van & ~our).sum() / a), 3), extra=round(float((our & ~van).sum() / a), 3))
    return res


def overlay(ms, path, title=''):
    from PIL import Image, ImageDraw
    tiles = []
    for view, (van, our, hd) in ms.items():
        img = np.full(van.shape + (3,), 245, np.uint8)
        img[hd] = (225, 200, 185)
        img[van & our] = (60, 170, 70)
        img[van & ~our] = (220, 40, 40)
        img[our & ~van] = (40, 100, 230)
        im = Image.fromarray(img)
        ImageDraw.Draw(im).text((4, 4), view, fill=(0, 0, 0))
        tiles.append(im)
    H = max(t.height for t in tiles)
    out = Image.new('RGB', (sum(t.width for t in tiles) + 10 * len(tiles), H + 18), (255, 255, 255))
    x = 0
    for t in tiles:
        out.paste(t, (x, 18)); x += t.width + 10
    ImageDraw.Draw(out).text((4, 2), title + '   green = both, red = vanilla only (missing), blue = ours only (extra)', fill=(0, 0, 0))
    out.save(path)
