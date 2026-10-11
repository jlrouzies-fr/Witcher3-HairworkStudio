r"""beard - opt-in beard / moustache HairWorks (config key "beard"), 2026-10-11.

Until 2026-10-11 beards stayed vanilla. This module converts a character's card beard (the `hb_*` CMeshComponent,
pbr_hair cards on the beard_01 / mustaches_01 atlas) into a SEPARATE HairWorks fur, the way CDPR made Geralt's own
beard (`items\bodyparts\geralt_items\heads\h_02_mg__geralt.w2ent`: CMeshComponent hb_01_mg__geralt swapped to a
CFurComponent whose mesh = the card fallback and furMesh = geralt_beard01.redfur). Without the key nothing changes:
hairpipe.build never imports this module, the hair fur / template / mod of every config stay byte-identical.

Config (all keys optional, `"beard": {}` = defaults):
  "beard": {
    "comp": null,             # beard component name (default: the hb_* component of the appearance; one per build)
    "hair": true,             # false = beard only: the hair stays vanilla cards (no hair fur, template patched for the beard)
    "keep_brows": true,       # eyebrow cards baked in the same hb_ mesh (Lambert, Regis, ...) stay cards: extra mesh
                              #   <mesh>_brows.w2mesh drawn with the fur, fallback <mesh>_nobrows.w2mesh (no doubling with HW off)
    "alpha_vis": 0.25,        # card atlas alpha above which a card sample counts as visible hair
    "root_sd": 0.008,         # visible card samples within this skin distance form the root region
    "reach": 0.005,           # a head vertex is a root when a root-region sample lies within reach
    "radius": 0.005,          # direction / length field radius
    "K": 5,                   # points per guide (Geralt's beards: 4)
    "env": 0.004,             # a traced guide stops when it leaves the visible card volume by more than env
    "len_scale": 1.0, "min_len": 0.003, "max_len": 0.06,
    "lift": 0.5,              # first-step mix of the skin normal into the card direction
    "hairs_cm2": 60,          # strand density target (hairs per cm2): density = hairs_cm2 x mean face area / 64
    "simulate": false,        # Geralt's beards are not simulated (fully skinned): keep false for short beards
    "mat_src": "characters\\models\\geralt\\coif\\model\\geralt_beard03.redfur",
    "look": {"root_color": [..], "tip_color": [..], "density": .., "width": .., "width_root": .., "width_tip": ..,
             "clump_scale": .., "wave_scale": .., "extra_set": {"a.b.c": [type, value]}}
  }

Design (HAIRWORK.md "Beards"):
  * growth mesh = the HEAD MESH's own vertices / triangles under the visible beard cards (welded at UV seams). Roots
    lie exactly on the skin (Geralt: 0.01 mm), and the mouth opening is a hole of the head mesh, so no growth face
    bridges the lips (a Delaunay of the roots would stretch hairs across an open mouth).
  * skinning = each root's head-vertex bone weights (4 influences: jaw, chin, lips, mouth corners, head, neck), bone
    table = the head mesh's own bone names + rig matrices (identical to Geralt's beard fur: max |diff| 6e-5). The beard
    then deforms exactly like the face skin when talking; HairWorks interpolates the hairs over the skin triangles.
  * guides = streamlines of the card direction field: every visible card sample carries its card's strand tangent
    (atlas v, oriented away from the skin); a root follows the field for the local card length, stays outside the skin,
    and stops when it leaves the card volume (env). Resampled to K points.
  * material = Geralt's beard material (textures dropped; usePixelDensity off, lengthScale 1) + look overrides.
"""
import os
import sys

import numpy as np
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import gamefiles as G

DEFAULT = dict(comp=None, hair=True, keep_brows=True, alpha_vis=0.25, root_sd=0.008, reach=0.005, radius=0.005, K=5,
               env=0.004, len_scale=1.0, min_len=0.003, max_len=0.06, lift=0.5, hairs_cm2=60.0, simulate=False,
               mat_src='characters\\models\\geralt\\coif\\model\\geralt_beard03.redfur', look={},
               # L247 opt-ins (absent = the L246beard build): subdivide (midpoint splits of the head mesh), fade (m: guide
               # length taper by distance to the region outline), max_sd (m: strands held within this skin distance),
               # density_tex (beard atlas alpha as density texture on per-face card patches, tex_scale)
               subdivide=0, fade=0.0, max_sd=None, density_tex=False, tex_scale=0.5,
               # L248 opt-ins: remesh (m: even Poisson root spacing -> even hair density), tip_tex (tip colour from the
               # beard atlas RGB: per-strand grey / white variation; tip_brightness = textureBrightness)
               remesh=None, tip_tex=False, tip_brightness=0.6, fade_min_loop=0,
               # L249 opt-ins (zones from the lip bones: below / moustache / side / cheek): zone_spacing {zone: m} (remesh
               # root spacing), zone_len {zone: factor}, zone_max_sd {zone: m}
               zone_spacing=None, zone_len=None, zone_max_sd=None)
DEFAULT_LOOK = dict(root_color=[40, 32, 26], tip_color=[52, 44, 36])
BROW = ('eyebrow',)


def cfg(C):
    b = C.get('beard')
    if b is None or b is False:
        return None
    o = dict(DEFAULT)
    o.update(b if isinstance(b, dict) else {})
    lk = dict(DEFAULT_LOOK)
    lk.update(o.get('look') or {})
    o['look'] = lk
    return o


def find(C, info, comp=None):
    """-> (template, component, mesh) of the beard of the config's appearance, or None."""
    import discover as D
    app = C.get('appearance') or ''
    apps = info['appearances']
    tpls = apps.get(app) or [t for v in apps.values() for t in v]
    for t in tpls:
        for name, cls, mesh, _fur in D.mesh_comps(t):
            if cls != 'CMeshComponent':
                continue
            if (comp and name == comp) or (not comp and D.classify(mesh) == 'beard'):
                return t, name, mesh
    return None


def card_classes(ch, names, brow_frac=0.2):
    """per vertex: component label + 'brow' / 'beard' class of its component. A card is an eyebrow card when more than
    brow_frac of its skin weight is on *eyebrow* bones (the inner brow cards of Avallac'h / Eredin / Oriana are mostly on
    `head`, so a dominant-bone test missed them; beard cards carry no eyebrow weight at all)."""
    lab = G.components(ch['nv'], ch['idx'])
    cls = {}
    for l in np.unique(lab):
        vs = lab == l
        tot = brow = 0.0
        for k in range(4):
            for bi, w in zip(ch['bi'][vs, k], ch['bw'][vs, k]):
                if w > 0:
                    nm = names[bi] if bi < len(names) else '?'
                    tot += float(w)
                    if any(b in nm for b in BROW):
                        brow += float(w)
        cls[l] = 'brow' if tot > 0 and brow / tot > brow_frac else 'beard'
    return lab, np.array([cls[l] for l in lab])


def _alpha_at(alpha, uv):
    h, w = alpha.shape
    x = np.clip(((uv[:, 0] % 1.0) * w).astype(int), 0, w - 1)
    y = np.clip(((uv[:, 1] % 1.0) * h).astype(int), 0, h - 1)
    return alpha[y, x]


def card_samples(ch, alpha, keep_vert, skin_tree, skin_n, o, n=6):
    """visible samples of the converted cards -> dict(p, t (oriented strand tangent), card, sd, length per sample)."""
    P, UV = ch['pos'], ch['uv']
    tri = ch['idx'].reshape(-1, 3)
    tri = tri[keep_vert[tri].all(1)]
    lab = G.components(ch['nv'], ch['idx'])
    a, b, c = P[tri[:, 0]], P[tri[:, 1]], P[tri[:, 2]]
    ua, ub, uc = UV[tri[:, 0]], UV[tri[:, 1]], UV[tri[:, 2]]
    # strand tangent of each triangle = dP/dv (atlas v runs along the strands, PIPELINE.md step 2)
    e1, e2 = b - a, c - a
    d1, d2 = ub - ua, uc - ua
    det = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    det = np.where(np.abs(det) < 1e-12, 1e-12, det)
    dPdv = (e2 * d1[:, :1] - e1 * d2[:, :1]) / det[:, None]
    dPdu_raw = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) / det[:, None]
    dPdv_raw = dPdv.copy()
    dPdv /= np.maximum(np.linalg.norm(dPdv, axis=1, keepdims=True), 1e-12)
    pts, tans, cards, uvs_, tri_i = [], [], [], [], []
    ti = np.arange(len(tri))
    for i in range(n + 1):
        for j in range(n + 1 - i):
            u_, v_ = i / n, j / n
            p = a + e1 * u_ + e2 * v_
            uv = ua + d1 * u_ + d2 * v_
            ok = _alpha_at(alpha, uv) > o['alpha_vis']
            pts.append(p[ok]); tans.append(dPdv[ok]); cards.append(lab[tri[ok, 0]])
            uvs_.append(uv[ok]); tri_i.append(ti[ok])
    p = np.concatenate(pts); t = np.concatenate(tans); cd = np.concatenate(cards)
    suv = np.concatenate(uvs_); sti = np.concatenate(tri_i)
    J = np.stack([dPdu_raw[sti], dPdv_raw[sti]], axis=2)          # (n, 3, 2): d position / d (u, v)
    box = {}
    for l in np.unique(lab[tri[:, 0]]):
        uu = UV[lab == l]
        box[l] = (uu.min(0), uu.max(0))
    d, vi = skin_tree.query(p)
    sd = d
    # orient every card: v must run away from the skin (root = skin end). Per card from the sd / projection correlation,
    # cards with no clear trend take the atlas majority.
    sign = {}
    votes = []
    for l in np.unique(cd):
        m = cd == l
        tm = t[m].mean(0)
        tm /= max(np.linalg.norm(tm), 1e-12)
        pr = p[m] @ tm
        if m.sum() >= 4 and np.ptp(pr) > 1e-4:
            r = np.corrcoef(pr, sd[m])[0, 1] if np.std(sd[m]) > 1e-6 else 0.0
            sign[l] = (1.0 if r >= 0 else -1.0, abs(r))
            votes.append(1.0 if r >= 0 else -1.0)
    maj = 1.0 if (np.sum(votes) >= 0) else -1.0
    s = np.array([sign[l][0] if (l in sign and sign[l][1] > 0.3) else maj for l in cd])
    t = t * s[:, None]
    # card length along its oriented tangent
    L = np.zeros(len(p))
    for l in np.unique(cd):
        m = cd == l
        tm = t[m].mean(0)
        tm /= max(np.linalg.norm(tm), 1e-12)
        pr = p[m] @ tm
        L[m] = float(np.ptp(pr)) if m.sum() > 1 else 0.0
    return dict(p=p, t=t, card=cd, sd=sd, L=L, uv=suv, J=J, box=box, majority=maj, oriented=int(sum(1 for v in sign.values() if v[1] > 0.3)),
                cards=int(len(np.unique(cd))))


class Skin:
    """welded head mesh (chunk 0) with per-vertex skin weights and outward normals."""

    def __init__(self, head_rel):
        m = G.load_mesh(head_rel)
        c = m['chunks'][0]
        key = np.round(c['pos'] / 1e-5).astype(np.int64)
        _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
        inv = inv.reshape(-1)
        self.v = c['pos'][first]
        self.bi = c['bi'][first]
        self.bw = c['bw'][first]
        t = inv[c['idx'].reshape(-1, 3)]
        t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
        self.t = t
        top = float(self.v[:, 2].max())
        cran = self.v[self.v[:, 2] > top - 0.12]
        self.c = np.array([0.0, float(cran[:, 1].mean()), top - 0.14])
        fn = np.cross(self.v[t[:, 1]] - self.v[t[:, 0]], self.v[t[:, 2]] - self.v[t[:, 0]])
        n = np.zeros_like(self.v)
        for k in range(3):
            np.add.at(n, t[:, k], fn)
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
        flip = ((self.v - self.c) * n).sum(1) < 0
        # the head mesh is wound consistently: orient by the majority, not per vertex (chin underside points down)
        if flip.mean() > 0.5:
            n = -n
        self.n = n
        self.tree = cKDTree(self.v)
        self.names = G.mesh_bones(head_rel)['names']
        self.rig = G.mesh_bones(head_rel)['rig']

    def signed(self, P):
        d, i = self.tree.query(P)
        return ((P - self.v[i]) * self.n[i]).sum(-1), self.n[i]

    def subdivide(self):
        """opt-in (beard.subdivide): one midpoint split of every triangle (4 x faces, half the edge length -> a finer
        region outline). New vertices: edge midpoints, normal = mean of the two, skin weights = the two vertices'
        weights averaged (top 4, renormalised)."""
        V, N, BI, BW = list(self.v), list(self.n), list(self.bi), list(self.bw)
        mid = {}

        def m(a, b):
            k = (min(a, b), max(a, b))
            if k not in mid:
                acc = {}
                for vv in (a, b):
                    for q in range(4):
                        if self.bw[vv, q] > 0:
                            acc[int(self.bi[vv, q])] = acc.get(int(self.bi[vv, q]), 0.0) + 0.5 * float(self.bw[vv, q])
                top = sorted(acc.items(), key=lambda x: -x[1])[:4]
                tot = sum(w for _, w in top) or 1.0
                bi = [j for j, _ in top] + [0] * (4 - len(top))
                bw = [w / tot for _, w in top] + [0.0] * (4 - len(top))
                nn = self.n[a] + self.n[b]
                mid[k] = len(V)
                V.append(0.5 * (self.v[a] + self.v[b])); N.append(nn / max(np.linalg.norm(nn), 1e-12))
                BI.append(np.array(bi)); BW.append(np.array(bw))
            return mid[k]
        T = []
        for a, b, c in self.t:
            ab, bc, ca = m(a, b), m(b, c), m(c, a)
            T += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
        self.v, self.n = np.array(V), np.array(N)
        self.bi, self.bw = np.array(BI).astype(int), np.array(BW, float)
        self.t = np.array(T, int)
        self.tree = cKDTree(self.v)


def _field(S, tree, x, r):
    idx = tree.query_ball_point(x, r)
    if len(idx) < 2:
        _, idx = tree.query(x, k=3)
        idx = list(np.atleast_1d(idx))
    idx = np.array(idx)
    d = np.linalg.norm(S['p'][idx] - x, axis=1)
    w = np.exp(-2.0 * (d / r) ** 2) + 1e-6
    v = (S['t'][idx] * w[:, None]).sum(0)
    nv = np.linalg.norm(v)
    return v / nv if nv > 1e-9 else None, float((S['L'][idx] * w).sum() / w.sum())


def _resample(pts, K):
    pts = np.asarray(pts, float)
    seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
    s = np.concatenate([[0.0], np.cumsum(seg)])
    if s[-1] <= 1e-9:
        return np.repeat(pts[:1], K, axis=0)
    q = np.linspace(0.0, s[-1], K)
    return np.stack([np.interp(q, s, pts[:, k]) for k in range(3)], axis=1)


def zones(skin, P):
    """L249 per-root zones from the head mesh's lip bones: 'below' = under the lower lip (chin / jaw / neck),
    'moustache' = above the upper lip within 35 mm of the mid line, 'side' = sideburns (|x| > 55 mm above the lower
    lip), else 'cheek'."""
    def bone(n):
        return np.linalg.inv(np.array(skin.rig[skin.names.index(n)], float))[3, :3] if n in skin.names else None
    up, lo = bone('upper_lip'), bone('lowwer_lip')
    P = np.atleast_2d(P)
    z = np.full(len(P), 'cheek', dtype=object)
    if up is None or lo is None:
        return z
    z[(P[:, 2] > lo[2] - 0.002) & (np.abs(P[:, 0]) > 0.055)] = 'side'
    z[(P[:, 2] > lo[2]) & (P[:, 2] < up[2] + 0.02) & (np.abs(P[:, 0]) < 0.035) & (P[:, 1] > up[1] - 0.03)] = 'moustache'
    z[P[:, 2] < lo[2] - 0.008] = 'below'
    return z


def remesh(skin, T, sp, log=print, seed=7, edge_k=2.2, cen_k=0.35, zone_sp=None):
    """opt-in (beard.remesh = root spacing in m, L248): HairWorks grows a fixed number of hairs PER FACE, so the head
    mesh's uneven triangles gave uneven hair density (Lambert: faces at the mouth corners 3x smaller than the median ->
    3x the hairs there = the dark specks of L247beard). New roots = a Poisson-disk sample (spacing sp) of the root-region
    triangles T, skin weights / normals interpolated barycentrically (top 4 bones), faces = Delaunay in an azimuthal
    projection about the beard's mean direction (long edges / slivers dropped) minus faces whose centroid leaves the root region (across the
    mouth). The new points are appended to skin; -> triangles indexing skin.v."""
    rng = np.random.default_rng(seed)
    a, b, c = skin.v[T[:, 0]], skin.v[T[:, 1]], skin.v[T[:, 2]]
    A = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
    n = int(A.sum() / (sp * sp) * 8) + 200
    ti = rng.choice(len(T), size=n, p=A / A.sum())
    r1, r2 = np.sqrt(rng.random(n)), rng.random(n)
    w = np.stack([1 - r1, r1 * (1 - r2), r1 * r2], 1)
    P = w[:, :1] * a[ti] + w[:, 1:2] * b[ti] + w[:, 2:] * c[ti]
    keep = []
    spi = np.full(n, sp)
    if zone_sp:
        # L249 opt-in: smaller root spacing (= smaller faces = more hairs per area, HairWorks grows per face) per zone
        zz = zones(skin, P)
        for zn, v in zone_sp.items():
            spi[zz == zn] = float(v)
    for i in range(n):
        if not keep or np.min(np.linalg.norm(P[keep] - P[i], axis=1)) >= spi[i]:
            keep.append(i)
    keep = np.array(keep)
    Pk, wk, tk = P[keep], w[keep], ti[keep]
    BI, BW, N = [], [], []
    for q in range(len(keep)):
        acc = {}
        nn = np.zeros(3)
        for j in range(3):
            v = T[tk[q], j]
            nn += wk[q, j] * skin.n[v]
            for k in range(4):
                if skin.bw[v, k] > 0:
                    acc[int(skin.bi[v, k])] = acc.get(int(skin.bi[v, k]), 0.0) + wk[q, j] * float(skin.bw[v, k])
        top = sorted(acc.items(), key=lambda x: -x[1])[:4]
        tot = sum(x for _, x in top) or 1.0
        BI.append([j for j, _ in top] + [0] * (4 - len(top)))
        BW.append([x / tot for _, x in top] + [0.0] * (4 - len(top)))
        N.append(nn / max(np.linalg.norm(nn), 1e-12))
    # Delaunay in an azimuthal projection whose pole is the beard's mean direction from the head centre (growth.faces_for
    # uses the head's vertical pole: the beard sits near its other end, where that projection tears)
    from scipy.spatial import Delaunay
    d = Pk - skin.c
    z = d.mean(0); z /= np.linalg.norm(z)
    x = np.cross(z, [0.0, 0.0, 1.0]); x /= max(np.linalg.norm(x), 1e-9); y = np.cross(z, x)
    dn = d / np.linalg.norm(d, axis=1, keepdims=True)
    th = np.arccos(np.clip(dn @ z, -1, 1)); ph = np.arctan2(dn @ y, dn @ x)
    tri = Delaunay(np.stack([th * np.cos(ph), th * np.sin(ph)], 1)).simplices
    e = np.max([np.linalg.norm(Pk[tri[:, i]] - Pk[tri[:, (i + 1) % 3]], axis=1) for i in range(3)], axis=0)
    ar = 0.5 * np.linalg.norm(np.cross(Pk[tri[:, 1]] - Pk[tri[:, 0]], Pk[tri[:, 2]] - Pk[tri[:, 0]]), axis=1)
    spf = spi[keep][tri].max(1)
    tri = tri[(e <= spf * edge_k) & (ar > spf * spf * 0.05)]
    used = np.unique(tri)
    rm = -np.ones(len(Pk), int); rm[used] = np.arange(len(used))
    F = rm[tri]
    Pu = Pk[used]
    cen = Pu[F].mean(1)
    dc, _ = cKDTree(P).query(cen)
    ok = dc < cen_k * spi[keep][used][F].max(1)
    F = F[ok]
    base = len(skin.v)
    skin.v = np.concatenate([skin.v, Pu])
    skin.n = np.concatenate([skin.n, np.array(N)[used]])
    skin.bi = np.concatenate([skin.bi, np.array(BI, int)[used]])
    skin.bw = np.concatenate([skin.bw, np.array(BW, float)[used]])
    skin.tree = cKDTree(skin.v)
    log('  beard: remesh %.1f mm: %d roots (Poisson) on %d head triangles, %d faces (%d dropped off the region)' % (
        sp * 1000, len(used), len(T), len(F), int((~ok).sum())))
    return F + base


def guides(skin, S, roots, o, log=print, lfac=None):
    tree = cKDTree(S['p'])
    out, lens, stopped = [], [], 0
    zl, zsd = o.get('zone_len') or {}, o.get('zone_max_sd') or {}
    zr = zones(skin, skin.v[np.asarray(roots)]) if (zl or zsd) else None
    K = int(o['K'])
    r = o['radius']
    for ri, vi in enumerate(roots):
        p0 = skin.v[vi]
        n0 = skin.n[vi]
        f0, L = _field(S, tree, p0, r)
        L = float(np.clip(L * o['len_scale'], o['min_len'], o['max_len']))
        msd = o.get('max_sd')
        if zr is not None:
            L *= float(zl.get(zr[ri], 1.0))          # L249 opt-in: per-zone length factor
            msd = zsd.get(zr[ri], msd)               # L249 opt-in: per-zone skin hold
        if lfac is not None:
            L = max(L * float(lfac[ri]), 0.0008)
        d = n0 if f0 is None else f0 + o['lift'] * n0
        d /= max(np.linalg.norm(d), 1e-12)
        nsub = 4 * (K - 1)
        h = L / nsub
        pts = [p0]
        p = p0.copy()
        for k in range(nsub):
            f, _ = _field(S, tree, p, r)
            if f is not None:
                d = 0.55 * f + 0.45 * d
                d /= max(np.linalg.norm(d), 1e-12)
            q = p + h * d
            sd, nq = skin.signed(q[None])
            floor = 0.0004 + 0.6 * h * (k + 1) / nsub        # rises off the skin a little along the strand
            if sd[0] < floor:
                q = q + (floor - sd[0]) * nq[0]
            elif msd is not None and sd[0] > msd:
                q = q - (sd[0] - msd) * nq[0]          # L247 opt-in: no tufts standing off the skin
            if k > nsub * 0.3 and tree.query(q)[0] > o['env']:
                stopped += 1
                break
            pts.append(q)
            p = q
        if np.linalg.norm(pts[-1] - pts[0]) < o['min_len'] * 0.5 and lfac is None:
            pts = [p0, p0 + o['min_len'] * d]
        elif len(pts) < 2:
            pts = [p0, p0 + 0.0008 * d]
        g = _resample(pts, K)
        out.append(g)
        lens.append(float(np.linalg.norm(np.diff(g, axis=0), axis=1).sum()))
    lens = np.array(lens) if lens else np.zeros(1)
    log('  beard: %d guides x %d points, length mm p10 %.1f / p50 %.1f / p90 %.1f / max %.1f; %d stopped at the card envelope' % (
        len(out), K, *np.percentile(lens * 1000, [10, 50, 90, 100]), stopped))
    return out


def bone_table(skin, used, rig_rel=None):
    """head mesh bone names + rig matrices (as Geralt's beard furs); `used` = bone indices of the head mesh."""
    names = [skin.names[i] for i in sorted(set(used) | {skin.names.index('head')})]
    names.remove('head')
    names = ['head'] + names
    par = {}
    if rig_rel:
        import rig
        rn, rp, _M = rig.load_rig(rig_rel)
        for i, nm in enumerate(rn):
            p = rp[i]
            while p >= 0 and rn[p] not in names:
                p = rp[p]
            par[nm] = rn[p] if p >= 0 else None
    out = []
    for nm in names:
        R = np.array(skin.rig[skin.names.index(nm)], float)
        pn = par.get(nm)
        out.append(dict(name=nm, rig=R.tolist(), bind=np.linalg.inv(R).tolist(),
                        parent=names.index(pn) if pn in names else -1, eps=0.151))
    return out


def face_uvs(V, F, c):
    """per-corner growth UVs (no texture reads them): spherical projection about the head centre, mirrored per face
    where needed so every face has a negative signed uv area (redfur rule)."""
    d = V - c
    u = np.arctan2(d[:, 0], d[:, 1]) / (2 * np.pi) + 0.5
    v = 0.5 - np.arctan2(d[:, 2], np.hypot(d[:, 0], d[:, 1])) / np.pi
    uv = np.stack([u, v], 1)[F]                      # (F, 3, 2)
    a = (uv[:, 1, 0] - uv[:, 0, 0]) * (uv[:, 2, 1] - uv[:, 0, 1]) - (uv[:, 2, 0] - uv[:, 0, 0]) * (uv[:, 1, 1] - uv[:, 0, 1])
    uv[a > 0, :, 0] *= -1.0
    return uv.reshape(-1, 2)


def tex_uvs(V, F, S, alpha, o, log=print):
    """opt-in (beard.density_tex): per-corner UVs on the beard atlas. Each growth face gets the card patch of the visible
    card sample nearest its centroid: corner uv = sample uv + tex_scale x pinv(J) (corner - centroid), clamped to that
    card's atlas island, mirrored in u where needed for a negative signed uv area. -> (uvs (F*3, 2), mean alpha)."""
    tree = cKDTree(S['p'])
    C = V[F].mean(1)
    _, si = tree.query(C)
    out = np.zeros((len(F), 3, 2))
    for f in range(len(F)):
        s_ = si[f]
        Ji = np.linalg.pinv(S['J'][s_])                       # (2, 3)
        d = (V[F[f]] - C[f]) @ Ji.T * o['tex_scale']           # (3, 2)
        a = (d[1, 0] - d[0, 0]) * (d[2, 1] - d[0, 1]) - (d[2, 0] - d[0, 0]) * (d[1, 1] - d[0, 1])
        if a > 0:
            d[:, 0] *= -1.0
        lo, hi = S['box'][S['card'][s_]]
        out[f] = np.clip(S['uv'][s_] + d, lo, hi)
    # degenerate (clamped flat) faces: tiny negative-area triangle around the sample uv
    a = (out[:, 1, 0] - out[:, 0, 0]) * (out[:, 2, 1] - out[:, 0, 1]) - (out[:, 2, 0] - out[:, 0, 0]) * (out[:, 1, 1] - out[:, 0, 1])
    bad = a >= 0
    if bad.any():
        c0 = S['uv'][si[bad]]
        out[bad] = c0[:, None, :] + np.array([[0, 0], [0, 1e-3], [1e-3, 0]])[None]
    bary = [(1 / 3, 1 / 3), (.6, .2), (.2, .6), (.2, .2)]
    al = np.mean([_alpha_at(alpha, out[:, 0] + (out[:, 1] - out[:, 0]) * x + (out[:, 2] - out[:, 0]) * y) for x, y in bary])
    log('  beard: density texture uvs: %d faces on card patches (scale %.2f), %d degenerate fixed, mean atlas alpha %.2f' % (
        len(F), o['tex_scale'], int(bad.sum()), al))
    return out.reshape(-1, 2), float(al)


def material(o, area_mm2, log=print, tex=None, mean_alpha=1.0):
    pm, gm = 'physicalMaterials', 'graphicalMaterials'
    L = o['look']
    s = {}
    Fv = lambda x: ('Float', float(x))
    col = lambda cc: ('Color', tuple(int(x) for x in cc) + (255,))
    dens = L.get('density')
    if dens is None:
        dens = float(o['hairs_cm2']) / 100.0 * area_mm2 / 64.0 / max(mean_alpha, 0.05)
    s[(pm, 'simulation', 'simulate')] = ('Bool', 1 if o['simulate'] else 0)
    s[(pm, 'volume', 'usePixelDensity')] = ('Bool', 1 if tex else 0)
    textures = {}
    if o.get('tip_tex') and tex:
        textures[(gm, 'color', 'tipColorTex')] = tex
        s[(gm, 'color', 'textureBrightness')] = ('Float', float(o['tip_brightness']))
    if tex:
        textures[(pm, 'volume', 'densityTex')] = tex
        s[(pm, 'volume', 'densityTexChannel')] = ('EHairTextureChannel', 'ALPHA')
    s[(pm, 'volume', 'lengthScale')] = Fv(L.get('length_scale', 1.0))
    s[(pm, 'volume', 'density')] = Fv(dens)
    s[(gm, 'color', 'rootColor')] = col(L['root_color'])
    s[(gm, 'color', 'tipColor')] = col(L['tip_color'])
    for k, path in (('width', (pm, 'strandWidth', 'width')), ('width_root', (pm, 'strandWidth', 'widthRootScale')),
                    ('width_tip', (pm, 'strandWidth', 'widthTipScale')), ('width_noise', (pm, 'strandWidth', 'widthNoise')),
                    ('length_noise', (pm, 'volume', 'lengthNoise')), ('clump_scale', (pm, 'clumping', 'clumpScale')),
                    ('clump_roundness', (pm, 'clumping', 'clumpRoundness')), ('clump_noise', (pm, 'clumping', 'clumpNoise')),
                    ('wave_scale', (pm, 'waveness', 'waveScale')), ('wave_freq', (pm, 'waveness', 'waveFreq')),
                    ('root_tip_weight', (gm, 'color', 'rootTipColorWeight')),
                    ('root_tip_falloff', (gm, 'color', 'rootTipColorFalloff')),
                    ('ambient_env', (gm, 'color', 'ambientEnvScale')), ('spec_primary', (gm, 'specular', 'specularPrimary')),
                    ('shadow_sigma', (gm, 'shadow', 'shadowSigma')), ('shadow_density', (gm, 'shadow', 'shadowDensityScale')),
                    ('root_alpha_falloff', (gm, 'color', 'rootAlphaFalloff'))):
        if L.get(k) is not None:
            s[path] = Fv(L[k])
    if L.get('spec_color') is not None:
        s[(gm, 'specular', 'specularColor')] = col(L['spec_color'])
    for k, v in (L.get('extra_set') or {}).items():
        s[tuple(k.split('.'))] = tuple(v) if isinstance(v, (list, tuple)) else ('Float', float(v))
    log('  beard material: %s (textures dropped), density %.3f (%.0f hairs/cm2 over %.1f mm2 mean face), simulate %s' % (
        o['mat_src'], dens, float(o['hairs_cm2']), area_mm2, bool(o['simulate'])))
    return dict(mat_src=o['mat_src'], set=s, textures=textures, drop_textures=True)


def generate(C, info, log=print):
    """-> dict(fur, template, comp, mesh, res, head, brows) or None when the appearance has no beard component."""
    import redfur
    import growth
    from texread import load_xbm
    import hairpipe as H
    o = cfg(C)
    hit = find(C, info, o.get('comp'))
    if not hit:
        log('  beard: no beard component in appearance %r: skipped' % C.get('appearance'))
        return None
    tpl, comp, mesh = hit
    m = G.load_mesh(mesh)
    ch = G.merge_chunks(m, m['lod0'])
    bnames = G.mesh_bones(mesh)['names']
    lab, cls = card_classes(ch, bnames)
    nb = len(np.unique(lab[cls == 'brow']))
    nc = len(np.unique(lab[cls == 'beard']))
    log('  beard: %s comp %s mesh %s: %d card components, %d eyebrow cards %s' % (
        tpl, comp, mesh, nc + nb, nb, 'kept as cards' if (nb and o['keep_brows']) else ('converted' if nb else '')))
    tex = H.card_texture(mesh)
    alpha = np.asarray(load_xbm(tex))[..., 3].astype(np.float64) / 255.0
    head_rel = C.get('head_mesh') or info['heads'][0]
    skin = Skin(head_rel)
    for _ in range(int(o['subdivide'])):
        skin.subdivide()
    conv = (cls == 'beard') if o['keep_brows'] else np.ones(len(cls), bool)
    S = card_samples(ch, alpha, conv, skin.tree, skin.n, o)
    log('  beard: %d visible card samples on %d cards (%d oriented by skin distance, atlas majority %+d), card length mm p50 %.1f' % (
        len(S['p']), S['cards'], S['oriented'], int(S['majority']), float(np.median(S['L'])) * 1000))
    # roots: welded head vertices with a root-region card sample within reach (and not nearer an eyebrow card)
    rr = S['sd'] < o['root_sd']
    rt = cKDTree(S['p'][rr])
    d, _ = rt.query(skin.v)
    root = d < o['reach']
    if nb and o['keep_brows']:
        Sb = card_samples(ch, alpha, cls == 'brow', skin.tree, skin.n, o)
        if len(Sb['p']):
            db, _ = cKDTree(Sb['p']).query(skin.v)
            near_brow = db < d
            log('  beard: %d candidate roots nearer an eyebrow card than a beard card dropped' % int((root & near_brow).sum()))
            root &= ~near_brow
    T = skin.t[root[skin.t].all(1)]
    if o.get('remesh'):
        T = remesh(skin, T, float(o['remesh']), log, edge_k=o.get('remesh_edge', 2.2), cen_k=o.get('remesh_cen', 0.35),
                   zone_sp=o.get('zone_spacing'))
    used = np.unique(T)
    remap = -np.ones(len(skin.v), int)
    remap[used] = np.arange(len(used))
    F = remap[T]
    # winding outward
    V = skin.v[used]
    fn = np.cross(V[F[:, 1]] - V[F[:, 0]], V[F[:, 2]] - V[F[:, 0]])
    flip = (fn * skin.n[used][F].mean(1)).sum(1) < 0
    F[flip] = F[flip][:, ::-1]
    area = 0.5 * np.linalg.norm(fn, axis=1)
    edges = np.linalg.norm(V[F[:, 0]] - V[F[:, 1]], axis=1)
    log('  beard: growth mesh = head mesh %s: %d roots, %d faces (%d flipped), edge mm p50 %.1f, face area mm2 p50 %.1f' % (
        head_rel, len(used), len(F), int(flip.sum()), float(np.median(edges)) * 1000, float(np.median(area)) * 1e6))
    lfac = None
    if o['fade']:
        # L247 opt-in: guides shorten toward the region outline (vertices of edges used by one face only) -> soft edge
        from collections import Counter
        ec = Counter()
        for a_, b_, c_ in F:
            for e_ in ((a_, b_), (b_, c_), (c_, a_)):
                ec[tuple(sorted(e_))] += 1
        bnd = sorted({v for e_, n_ in ec.items() if n_ == 1 for v in e_})
        if o.get('fade_min_loop'):
            # L248 opt-in: only outline loops with >= fade_min_loop vertices fade (small interior holes of a remeshed
            # growth mesh are not the beard's edge)
            from collections import defaultdict
            adj = defaultdict(list)
            for e_, n_ in ec.items():
                if n_ == 1:
                    adj[e_[0]].append(e_[1]); adj[e_[1]].append(e_[0])
            seen, keep_ = set(), []
            for v0 in adj:
                if v0 in seen:
                    continue
                cmp_, st = [], [v0]
                while st:
                    x0 = st.pop()
                    if x0 in seen:
                        continue
                    seen.add(x0); cmp_.append(x0); st += adj[x0]
                if len(cmp_) >= o["fade_min_loop"]:
                    keep_ += cmp_
            bnd = sorted(keep_)
        db_, _ = cKDTree(V[bnd]).query(V)
        x_ = np.clip(db_ / o['fade'], 0.0, 1.0)
        lfac = 0.15 + 0.85 * x_ * x_ * (3 - 2 * x_)
        log('  beard: fade %.0f mm: %d outline roots, length factor p10 %.2f / p50 %.2f' % (
            o['fade'] * 1000, len(bnd), np.percentile(lfac, 10), np.percentile(lfac, 50)))
    Gd = guides(skin, S, used, o, log, lfac=lfac)
    # skinning: the head vertex's own weights
    bone_used = set()
    for vi in used:
        for k in range(4):
            if skin.bw[vi, k] > 0:
                bone_used.add(int(skin.bi[vi, k]))
    rig_rel = C.get('rig') or next((x for x in info.get('rigs', []) if 'base_entities' in x), None)
    bones = bone_table(skin, bone_used, rig_rel)
    bn = [b['name'] for b in bones]
    strands = []
    for g, vi in zip(Gd, used):
        w = [(bn.index(skin.names[int(skin.bi[vi, k])]), float(skin.bw[vi, k])) for k in range(4) if skin.bw[vi, k] > 0]
        tot = sum(x for _, x in w) or 1.0
        strands.append({'p': g, 'w': [(j, x / tot) for j, x in w]})
    dom = {}
    for s_ in strands:
        j = max(s_['w'], key=lambda q: q[1])[0]
        dom[bn[j]] = dom.get(bn[j], 0) + 1
    log('  beard: %d bones (head mesh rig) %s; dominant bone per guide %s' % (
        len(bones), bn[:12] + (['...'] if len(bn) > 12 else []), sorted(dom.items(), key=lambda q: -q[1])[:8]))
    if o['density_tex']:
        uvs, mal = tex_uvs(V, F, S, alpha, o, log)
        look = material(o, float(area.mean()) * 1e6, log, tex=tex, mean_alpha=mal)
    else:
        uvs = face_uvs(V, F, skin.c)
        look = material(o, float(area.mean()) * 1e6, log)
    col = dict(pos=[], idx=[], rad=[], cap=[])
    data = redfur.write(strands, F, uvs, bones, col, look, auto_hide=C.get('auto_hide', 100.0))
    redfur.verify(data)
    head = growth.Head(head_rel)
    res = dict(S=strands, F=F, roots=V)
    log('fur %s (beard): %d guides, %d faces, %d bytes' % (comp, len(strands), len(F), len(data)))
    return dict(fur=data, template=tpl, comp=comp, mesh=mesh, res=res, head=head, brows=bool(nb and o['keep_brows']),
                tex=tex, skin=skin, samples=S)


def brow_mask(ch_mesh_names):
    def fn(ch):
        _lab, cls = card_classes(ch, ch_mesh_names)
        return cls == 'brow'
    return fn


def add_to_build(C, info, files, bufs, tpls, P, log=print):
    """hairpipe.build hook (only called when C['beard'] is set): beard fur + template patch (+ eyebrow subset meshes).
    The template already patched for the hair (same head template, e.g. Lambert's h_01_ma__lambert.w2ent) is patched
    again in place in `files`; otherwise the vanilla beard template is patched and added."""
    import template
    import strand_edits
    import hairpipe as H
    B = generate(C, info, log)
    if B is None:
        return None
    fp = P(H.fur_path(C, B['comp']))
    files.append((fp, B['fur']))
    extra, fallback = [], None
    if B['brows']:
        names = G.mesh_bones(B['mesh'])['names']
        keep = brow_mask(names)
        bm = P(B['mesh'][:-len('.w2mesh')] + '_brows.w2mesh')
        w, b, stt = template.subset_mesh(B['mesh'], keep)
        log('  beard: eyebrow cards kept as %s: (verts, live tris, tris) per chunk %s' % (bm, stt))
        files.append((bm, w)); bufs.append((bm + '.1.buffer', b))
        extra.append((B['comp'] + '_brows', bm))
        fb = P(B['mesh'][:-len('.w2mesh')] + '_nobrows.w2mesh')
        w, b, stt = template.subset_mesh(B['mesh'], lambda ch: ~keep(ch))
        log('  beard: fallback (HairWorks off) = beard cards without the eyebrows %s: %s' % (fb, stt))
        files.append((fb, w)); bufs.append((fb + '.1.buffer', b))
        fallback = fb
    tp = P(B['template'])
    i = next((k for k, (rel, _d) in enumerate(files) if rel == tp), None)
    src = files[i][1] if i is not None else G.get(B['template'])
    new = template.patch(src, B['comp'], fp, extra, fallback)
    template.validate(new, B['template'])
    if i is not None:
        files[i] = (tp, new)
    else:
        files.append((tp, new))
        tpls.append((B['template'], tp))
    try:
        info_ = dict(info)
        ep = strand_edits.export_json(C, B['res'], B['head'], info_, comp=B['comp'], log=log, index=1)
        strand_edits.export_hw_pack(C, B['fur'], ep, tex=None, comp=B['comp'], index=1, log=log)
    except Exception as ex:                      # exports never break a build
        log('  beard exports FAILED: %r' % (ex,))
    return B


if __name__ == '__main__':
    # py tools\hairworks\beard.py <cfg>     -> dry generation report (no files written except the editor / preview exports)
    import hairpipe as H
    C = H.load_config(sys.argv[1])
    if C.get('beard') in (None, False):
        C['beard'] = {}
    info = H.discover(C, log=lambda *a: None)
    generate(C, info)
