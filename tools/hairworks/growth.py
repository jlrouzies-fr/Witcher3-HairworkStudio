r"""growth - guide hairs + growth mesh for a CFurComponent, generated from a character's vanilla hair-card mesh.

Pipeline (all positions in entity model space = redfur space, Z up, face +Y; no transform needed: calibrated on
Geralt's vanilla pony / long / crown furs, guide-to-card distance 8-11 mm median):
  1. cards: connected components of the card mesh LOD0 (accessory islands removed). Each card is parametrised by its
     own UVs (atlas v = along the strand, root = v-min end in 101 / 115 of Vesemir's cards, decided per card by
     distance to the scalp). Lanes = iso-u lines traced through the card triangles in UV space (3D by barycentric
     interpolation), every LANE_W metres across the card; each lane is trimmed where the card alpha (texture) dies.
  2. chaining through gather accessories (ties, clasps): lanes that END at the accessory continue along a lane that
     STARTS there (the tail) -> top hair runs into the tie and on into the ponytail, as the cards do.
  3. roots: Poisson-disk samples of the scalp (head mesh) within REACH of VISIBLE card points (card surface samples
     whose texture alpha > ALPHA_VIS) -> the hairline follows the cards' real coverage (receded, M-shaped) instead of
     the card geometry (which overhangs into a smooth arc).
  4. guides: each root follows the lane with the most hair left near it (score = remaining length - 2 * distance),
     blended from the root onto the lane; then 'hug': near the scalp the hair layer is compressed (sd -> c0 + k*sd) so
     strands lie on the skull instead of standing off it as the outer card layers do.
  5. growth mesh: Delaunay of the roots on a sphere projection, long edges / slivers dropped.
"""
import os
import sys

import numpy as np
from scipy.spatial import cKDTree, Delaunay

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gamefiles as G


def resample(p, n):
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
    if d[-1] <= 0:
        return np.repeat(p[:1], n, 0), 0.0
    t = np.linspace(0, d[-1], n)
    return np.stack([np.interp(t, d, p[:, k]) for k in range(3)], 1), d[-1]


# ------------------------------------------------------------------------------------------ head geometry
class Head:
    """head mesh + derived landmarks (all character independent: from the mesh bounds)."""

    def __init__(self, mesh_rel, chunk=0):
        m = G.load_mesh(mesh_rel)
        c = m['chunks'][chunk]
        self.v = c['pos']
        self.t = c['idx'].reshape(-1, 3)
        self.top = float(self.v[:, 2].max())
        cran = self.v[self.v[:, 2] > self.top - 0.12]
        self.c = np.array([0.0, float(cran[:, 1].mean()), self.top - 0.14])
        nrm = np.zeros_like(self.v)
        a, b, cc = self.v[self.t[:, 0]], self.v[self.t[:, 1]], self.v[self.t[:, 2]]
        fn = np.cross(b - a, cc - a)
        for k in range(3):
            np.add.at(nrm, self.t[:, k], fn)
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
        nrm[((self.v - self.c) * nrm).sum(1) < 0] *= -1
        self.n = nrm
        self.tree = cKDTree(self.v)

    def signed(self, P):
        """approx. signed distance of points to the head surface (+ outside), and the surface normal there."""
        d, i = self.tree.query(P)
        sd = ((P - self.v[i]) * self.n[i]).sum(-1)
        return sd, self.n[i], d


# ------------------------------------------------------------------------------------------ cards -> lanes
def card_lanes(ch, head, alpha_img=None, lane_w=0.012, nsamp=24, alpha_tip=0.08, drop=None, edge_filter=True):
    """ch: mesh chunk (pos, uv, idx). Returns list of dict(p=(n,3) root->tip, uv=(n,2), card=label, a=(n,) alpha)."""
    P, UV = ch['pos'], ch['uv']
    tri = ch['idx'].reshape(-1, 3)
    if drop is not None:
        tri = tri[~drop[tri].any(1)]
    # split cards into UV-consistent islands: mirrored halves (fan / cap cards reuse one UV strip for both sides) have
    # opposite signed UV area; tracing an iso-u line over both would jump from one side of the head to the other.
    ua = UV[tri[:, 1]] - UV[tri[:, 0]]; ub = UV[tri[:, 2]] - UV[tri[:, 0]]
    sgn = (ua[:, 0] * ub[:, 1] - ua[:, 1] * ub[:, 0]) >= 0
    lab = np.full(ch['nv'], -1)
    tri_lab = np.zeros(len(tri), int)
    for sg in (True, False):
        ts = tri[sgn == sg]
        if not len(ts):
            continue
        l_ = G.components(ch['nv'], ts.reshape(-1))
        tri_lab[sgn == sg] = l_[ts[:, 0]] * 2 + int(sg)
    used = np.unique(tri)
    out = []
    aspect = 1.0 if alpha_img is None else alpha_img.shape[0] / alpha_img.shape[1]   # texels: u unit = w px, v unit = h px

    def alpha_at(uv):
        if alpha_img is None:
            return np.ones(len(uv))
        h, w = alpha_img.shape
        x = np.clip(((uv[:, 0] % 1.0) * w).astype(int), 0, w - 1)
        y = np.clip(((uv[:, 1] % 1.0) * h).astype(int), 0, h - 1)
        return alpha_img[y, x]

    for l in np.unique(tri_lab):
        tl = tri[tri_lab == l]
        vs = np.unique(tl)
        if len(vs) < 4:
            continue
        U = UV[vs]
        u0, u1 = U[:, 0].min(), U[:, 0].max()
        v0, v1 = U[:, 1].min(), U[:, 1].max()
        if v1 - v0 < 0.02:
            continue
        # root = the v-end nearer the scalp (usually v-min)
        lo = P[vs[U[:, 1] < v0 + 0.15 * (v1 - v0)]].mean(0)
        hi = P[vs[U[:, 1] > v1 - 0.15 * (v1 - v0)]].mean(0)
        sd_lo, _, _ = head.signed(lo[None])
        sd_hi, _, _ = head.signed(hi[None])
        flip = abs(sd_hi[0]) + 0.01 < abs(sd_lo[0])
        # physical size of the card: v extent from the geometry, u extent scaled by the card's own texel ratio
        len_m = float(np.linalg.norm(hi - lo))
        span = np.ptp(P[vs], axis=0).max()
        uv_m = (v1 - v0) / max(len_m, 0.01)                 # uv units per metre along the strand
        width_m = (u1 - u0) / max(uv_m * aspect, 1e-6)
        L = int(max(1, min(6, round(width_m / lane_w))))
        ns = int(min(160, max(nsamp, 2.5 * max(len_m, span) / 0.006)))
        A = UV[tl[:, 0]]; B = UV[tl[:, 1]]; C = UV[tl[:, 2]]
        for li in range(L):
            uu = u0 + (li + 0.5) / L * (u1 - u0)
            vv = np.linspace(v0, v1, ns)
            if flip:
                vv = vv[::-1]
            Q = np.stack([np.full(ns, uu), vv], 1)
            pts, ok = [], []
            prev = None
            for q in Q:
                # barycentric in every triangle of the island; among the triangles containing q take the one whose
                # 3D point continues the lane (overlapping UV copies), else the nearest-in-UV one
                v0_ = B - A; v1_ = C - A; v2_ = q - A
                d00 = (v0_ * v0_).sum(1); d01 = (v0_ * v1_).sum(1); d11 = (v1_ * v1_).sum(1)
                d20 = (v2_ * v0_).sum(1); d21 = (v2_ * v1_).sum(1)
                den = d00 * d11 - d01 * d01
                den = np.where(np.abs(den) < 1e-14, 1e-14, den)
                b1 = (d11 * d20 - d01 * d21) / den; b2 = (d00 * d21 - d01 * d20) / den; b0 = 1 - b1 - b2
                mn = np.minimum(np.minimum(b0, b1), b2)
                inside = np.where(mn >= -0.02)[0]
                if len(inside) == 0:
                    k = int(np.argmax(mn))
                    if mn[k] < -0.15:
                        ok.append(False); pts.append(np.zeros(3)); continue
                    cand = [k]
                else:
                    cand = inside
                X = b0[cand, None] * P[tl[cand, 0]] + b1[cand, None] * P[tl[cand, 1]] + b2[cand, None] * P[tl[cand, 2]]
                j = 0 if prev is None else int(np.argmin(np.linalg.norm(X - prev, axis=1)))
                prev = X[j]
                ok.append(True)
                pts.append(X[j])
            ok = np.array(ok)
            if ok.sum() < 4:
                continue
            pts = np.array(pts)[ok]; Qk = Q[ok]
            # split at jumps (> 2.5 cm between samples) and keep the root-side piece
            seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
            jump = np.where(seg > max(0.02, 6 * np.median(seg)))[0]
            if len(jump):
                pts, Qk = pts[:jump[0] + 1], Qk[:jump[0] + 1]
                if len(pts) < 4:
                    continue
            a = alpha_at(Qk)
            # column coverage: trim the tip where the strand texture dies (smoothed alpha along the lane)
            if alpha_img is not None:
                ka = np.convolve(a, np.ones(3) / 3, 'same')
                live = np.where(ka > alpha_tip)[0]
                if len(live) < 3:
                    continue
                last = live[-1] + 1
                pts, Qk, a = pts[:last], Qk[:last], a[:last]
            if np.linalg.norm(pts[-1] - pts[0]) < 0.015:
                continue
            if edge_filter and abs(Qk[-1, 1] - Qk[0, 1]) < 0.04:          # traced along a card edge (v ~ constant): not a strand
                continue
            out.append(dict(p=pts, uv=Qk, a=a, card=int(l)))
    return out


def chain_segments(lanes, head, pos_tol=0.008, uv_tol=0.03, log=print):
    """Join card segments into whole strands. Some meshes (Yennefer) split every card into ~3 cm islands (vertices
    duplicated per row): a lane then stops after one segment. Ends are joined when they meet in 3D (pos_tol) AND
    continue in the atlas (uv_tol), mutual nearest only; each chain is then oriented root = the end nearer the scalp."""
    if len(lanes) < 2:
        return lanes
    E = []                                    # (lane, end 0/1)
    for i, s in enumerate(lanes):
        E.append((i, 0)); E.append((i, 1))
    P = np.array([lanes[i]['p'][0 if e == 0 else -1] for i, e in E])
    U = np.array([lanes[i]['uv'][0 if e == 0 else -1] % 1.0 for i, e in E])
    tr = cKDTree(P)
    best = -np.ones(len(E), int)
    for k, (i, e) in enumerate(E):
        cand = [c for c in tr.query_ball_point(P[k], pos_tol) if E[c][0] != i and np.abs(U[c] - U[k]).max() < uv_tol]
        if cand:
            best[k] = min(cand, key=lambda c: np.linalg.norm(P[c] - P[k]))
    link = {}
    for k in range(len(E)):
        c = best[k]
        if c >= 0 and best[c] == k:
            link[k] = c
    seen = set()
    out = []
    joined = 0
    for i in range(len(lanes)):
        if i in seen:
            continue
        # walk out through end 0 to the chain's free end
        cur, e = i, 0
        visited = {i}
        while 2 * cur + e in link:
            j, f = E[link[2 * cur + e]]
            if j in visited:
                break
            visited.add(j)
            cur, e = j, 1 - f
        # cur's end e is free: enter there and walk the chain
        seq = []
        enter = e
        while cur not in seen:
            seen.add(cur)
            seq.append((cur, enter == 0))     # forward if entered at its stored first point
            leave = 2 * cur + (1 - enter)
            if leave not in link:
                break
            cur, enter = E[link[leave]]
        if len(seq) == 1:
            out.append(lanes[seq[0][0]])
            continue
        joined += len(seq) - 1
        ps, us, as_ = [], [], []
        for li, fwd in seq:
            s = lanes[li]
            sl = slice(None) if fwd else slice(None, None, -1)
            ps.append(s['p'][sl]); us.append(s['uv'][sl]); as_.append(s['a'][sl])
        p = np.concatenate(ps); uv = np.concatenate(us); a = np.concatenate(as_)
        sd0 = abs(head.signed(p[:1])[0][0]); sd1 = abs(head.signed(p[-1:])[0][0])
        if sd1 + 0.005 < sd0:
            p, uv, a = p[::-1], uv[::-1], a[::-1]
        out.append(dict(p=p, uv=uv, a=a, card=lanes[seq[0][0]]['card']))
    if joined:
        log('  chained %d card segments -> %d lanes (was %d)' % (joined, len(out), len(lanes)))
    return out


def gather_points(ch, select):
    """accessory islands (whole components hit by `select(uv, pos)`): [(centre, radius, vertex mask)]."""
    sel = select(ch['uv'], ch['pos'])
    lab = G.components(ch['nv'], ch['idx'])
    res = []
    for k in np.unique(lab[sel]):
        m = lab == k
        Q = ch['pos'][m]
        res.append((Q.mean(0), float(np.ptp(Q, 0).max() / 2), m))
    return res


def chain_lanes(lanes, centre, radius, rng, extra=0.015, min_tail=0.05):
    """lanes whose tip is inside the gather sphere continue along a lane that starts inside it (the tail)."""
    r = radius + extra
    ins = [i for i, s in enumerate(lanes) if np.linalg.norm(s['p'][-1] - centre) < r and np.linalg.norm(s['p'][0] - centre) > r]
    outs = [i for i, s in enumerate(lanes) if np.linalg.norm(s['p'][0] - centre) < r and np.linalg.norm(s['p'][-1] - centre) > r + min_tail]
    if not ins or not outs:
        return lanes, 0
    O = np.array([lanes[i]['p'][0] for i in outs])
    new = list(lanes)
    for i in ins:
        s = lanes[i]
        d = np.linalg.norm(O - s['p'][-1], axis=1)
        cand = np.argsort(d)[:4]
        j = outs[int(rng.choice(cand))]
        t = lanes[j]
        bridge = np.linspace(s['p'][-1], t['p'][0], 4)[1:-1]
        p = np.concatenate([s['p'], bridge, t['p']])
        uv = np.concatenate([s['uv'], np.repeat(s['uv'][-1:], len(bridge), 0), t['uv']])
        a = np.concatenate([s['a'], np.repeat(s['a'][-1:], len(bridge), 0), t['a']])
        new.append(dict(p=p, uv=uv, a=a, card=s['card'], chained=True))
    return new, len(ins)


# ------------------------------------------------------------------------------------------ roots on the scalp
def visible_card_points(ch, alpha_img, n=40000, alpha_vis=0.3, seed=2, drop=None, edge_filter=True):
    """random points on the card surface whose texture alpha is above alpha_vis (the hair you can actually see)."""
    rng = np.random.default_rng(seed)
    P, UV = ch['pos'], ch['uv']
    tri = ch['idx'].reshape(-1, 3)
    if drop is not None:
        tri = tri[~drop[tri].any(1)]
    A = P[tri[:, 1]] - P[tri[:, 0]]; B = P[tri[:, 2]] - P[tri[:, 0]]
    area = 0.5 * np.linalg.norm(np.cross(A, B), axis=1) + 1e-12
    k = rng.choice(len(tri), n, p=area / area.sum())
    u, v = rng.random(n), rng.random(n)
    f = u + v > 1
    u[f], v[f] = 1 - u[f], 1 - v[f]
    X = P[tri[k, 0]] + u[:, None] * A[k] + v[:, None] * B[k]
    Uq = UV[tri[k, 0]] + u[:, None] * (UV[tri[k, 1]] - UV[tri[k, 0]]) + v[:, None] * (UV[tri[k, 2]] - UV[tri[k, 0]])
    h, w = alpha_img.shape
    a = alpha_img[np.clip(((Uq[:, 1] % 1) * h).astype(int), 0, h - 1), np.clip(((Uq[:, 0] % 1) * w).astype(int), 0, w - 1)]
    return X[a > alpha_vis]


def sample_scalp(head, vis_pts, r=0.0125, reach=0.012, zmin_rel=0.20, seed=1, max_sd=0.03):
    """Poisson-disk roots on the head mesh where visible card hair lies within `reach` of the skin (and the visible
    point itself is not more than max_sd off the skin: outer layers floating away do not define the hairline)."""
    rng = np.random.default_rng(seed)
    hv, ht = head.v, head.t
    A = hv[ht[:, 1]] - hv[ht[:, 0]]; B = hv[ht[:, 2]] - hv[ht[:, 0]]
    area = 0.5 * np.linalg.norm(np.cross(A, B), axis=1)
    keep = hv[ht].mean(1)[:, 2] > head.top - zmin_rel - 0.02
    area = area * keep
    N = 80000
    tri = rng.choice(len(ht), N, p=area / area.sum())
    u, v = rng.random(N), rng.random(N)
    f = u + v > 1
    u[f], v[f] = 1 - u[f], 1 - v[f]
    Pp = hv[ht[tri, 0]] + u[:, None] * A[tri] + v[:, None] * B[tri]
    sd, _, _ = head.signed(vis_pts)
    near = vis_pts[np.abs(sd) < max_sd]
    d, _ = cKDTree(near).query(Pp)
    Pp = Pp[(d < reach) & (Pp[:, 2] > head.top - zmin_rel)]
    order = rng.permutation(len(Pp))
    Pp = Pp[order]
    chosen, grid = [], {}

    def key(p): return tuple((p // r).astype(int))
    for p in Pp:
        k = key(p)
        ok = True
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for q in grid.get((k[0] + dx, k[1] + dy, k[2] + dz), ()):
                        if np.linalg.norm(p - q) < r:
                            ok = False
                            break
                    if not ok:
                        break
                if not ok:
                    break
            if not ok:
                break
        if ok:
            chosen.append(p)
            grid.setdefault(k, []).append(p)
    return np.array(chosen)


# ------------------------------------------------------------------------------------------ guides
def guides(roots, lanes, K=8, maxd=0.03, near=0.012, min_rem=0.04, pick='longest', pick_power=1.0, rng=None,
           blend=0.0, dense_n=48, lane_ok=None, offset_keep=0.0):
    """one guide per root, following a card lane that passes within `near` of the root.

    pick = 'longest' (R1-R5): the lane with the most hair left (score = remaining - 2 * distance). On Vesemir this
    used 87 of 378 lanes, all the long outer cards: every guide ran to the same outer silhouette (one straight
    sheet, uniform ends, no layering, no width at the bottom).
    pick = 'balanced' (R6): one candidate point per lane (its best score), then a random lane with probability
    proportional to remaining_length ** pick_power (0 = uniform over the cards passing here). Neighbouring roots
    then follow short inner, mid and long outer cards alike: the interpolated hairs fill the layers between them
    (volume) and the ends spread over the cards' real length range (layered, no straight cut line).
    blend > 0: the guide also keeps the lane's own position near the root (0 = fully pulled to the root).
    lane_ok: optional per-lane mask; lanes marked False are never followed (e.g. cap cards ending on the skull).
    offset_keep (R78): the root's offset from the lane fades to this fraction at the tip instead of 0. With 0 (default,
    approved builds) every root following one lane ends on the lane's own tip: neighbours converge to one point
    (Bowl 406 / 415 guides share a tip with >= 2 others) and the hairs between them collapse into spikes / bald gaps."""
    dense, sid, jj, rem = [], [], [], []
    R40 = []
    for i, s in enumerate(lanes):
        if lane_ok is not None and not lane_ok[i]:
            R40.append(None)
            continue
        q, L = resample(s['p'], dense_n)
        R40.append(q)
        seg = L / (dense_n - 1)
        for j in range(dense_n):
            dense.append(q[j]); sid.append(i); jj.append(j); rem.append((dense_n - 1 - j) * seg)
    dense = np.array(dense); sid = np.array(sid); jj = np.array(jj); rem = np.array(rem)
    tr = cKDTree(dense)
    rng = rng or np.random.default_rng(7)
    out, kept, src = [], [], []
    for ri, r in enumerate(roots):
        cand = tr.query_ball_point(r, near)
        if not cand:
            d, c = tr.query(r)
            if d > maxd:
                continue
            cand = [c]
        cand = np.array(cand)
        dist = np.linalg.norm(dense[cand] - r, axis=1)
        score = rem[cand] - 2.0 * dist
        if pick == 'balanced':
            best = {}
            for c, sc in zip(cand, score):
                if rem[c] >= min_rem and (sid[c] not in best or sc > best[sid[c]][1]):
                    best[sid[c]] = (c, sc)
            if not best:
                continue
            cs = np.array([v[0] for v in best.values()])
            w = np.maximum(rem[cs], 1e-3) ** pick_power
            c = cs[rng.choice(len(cs), p=w / w.sum())]
        else:
            c = cand[np.argmax(score)]
        if rem[c] < min_rem:
            continue
        i, j = sid[c], jj[c]
        part, _ = resample(R40[i][j:], K)
        t = np.linspace(0, 1, K)[:, None]
        g = part + (r - part[0]) * (offset_keep + (1.0 - offset_keep) * (1 - t) ** 2) * (1.0 - blend)
        g[0] = r
        out.append(g); kept.append(ri); src.append((i, j))
    return out, np.array(kept), src


def hug(G_, head, c0=0.003, k=0.35, band=0.035, k_front=None, central_front=True, zmin=None, zmin_ymax=None):
    """compress the hair layer onto the scalp: points (not roots) within `band` of the skin get signed distance
    c0 + k * sd (never inside: >= c0). Outer card layers float 1-3 cm off the skull; strands would stand off it.
    zmin (R6): below this height (the ear line: the neck / nape) the layer is NOT compressed, only kept out of the
    skin (>= c0): the cards float 2-4 cm off the nape on purpose (collar fullness); pulling them onto the neck gave
    the thin, flat collar region of r3d."""
    out = []
    for g in G_:
        g = g.copy()
        sd, n, d = head.signed(g)
        for i in range(1, len(g)):
            if d[i] > 0.06 or sd[i] > band:
                continue
            ramp = min(1.0, i / 2.0)
            if zmin is not None and g[i][2] < zmin and (zmin_ymax is None or g[i][1] < zmin_ymax):
                if sd[i] < c0:
                    g[i] = g[i] + n[i] * (c0 - sd[i])
                continue
            kk = k_front if (k_front is not None and g[0][1] > head.c[1] + 0.03 and (abs(g[0][0]) < 0.05 or not central_front)) else k   # central front sweep keeps its lift; temples stay flat
            target = max(c0, c0 + kk * sd[i]) * ramp + sd[i] * (1 - ramp)
            target = max(target, c0 * ramp)
            g[i] = g[i] + n[i] * (target - sd[i])
        out.append(g)
    return out


def sweeps_forward(g, fwd=0.02, drop=0.02):  # fwd/drop overridable via cfg swept_fwd
    """a guide whose tip ends >= fwd metres in front of its root without hanging down (tip not >= drop lower): a lock
    sweeping toward the face. For combed-back styles (config growth.swept_back) these are wrong-way lanes."""
    return bool(g[-1][1] - g[0][1] > fwd and g[-1][2] > g[0][2] - drop)


def flare_guides(G_, head, rng, k=0.3, noise=0.5, t0=0.35, front_y=0.03, back_weight=False):
    """volume + curled ends: from t0 along the guide, push the points horizontally away from the head axis by
    k * length * ((t - t0) / (1 - t0))^2 (random per guide), so the lower hair spreads over the collar and the tips
    flick outward like the vanilla cards. Front-sweep guides (roots in front of the head centre) are left alone."""
    out = []
    for g in G_:
        g = g.copy()
        if g[0][1] > head.c[1] + front_y:
            out.append(g); continue
        d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))]
        L = d[-1]; t = d / max(L, 1e-9)
        f = max(0.0, k * (1.0 + noise * rng.standard_normal()))
        o = g[:, :2] - head.c[None, :2]
        o /= np.maximum(np.linalg.norm(o, axis=1, keepdims=True), 1e-9)
        a = np.clip((t - t0) / (1 - t0), 0, 1) ** 2 * f * L
        if back_weight:                      # sideways spread near the face makes stray hairs: favour the back
            a = a * np.clip(-o[:, 1], 0.3, 1.0)
        g[:, :2] += o * a[:, None]
        out.append(g)
    return out


def curl_tips(G_, rng, length=0.05, amp=0.012, waves=1.0, head=None, mode='x'):
    """soft S-curl on the last `length` metres of each guide (the cards paint wavy tips): sideways (x) offset
    amp * sin(pi * waves * u) * u, u = distance from the curl start / length, random sign / phase per guide.
    mode 'radial' (needs head): the offset goes horizontally away from the head axis, always outward (a flick of the
    ends over the collar like the cards' painted tips) instead of a random-sign sideways wiggle.
    mode 'helix' (R21): ringlet curls round the strand's own end direction (see below)."""
    out = []
    for g in G_:
        g = g.copy()
        d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))]
        u = np.clip((d - (d[-1] - length)) / length, 0, 1)
        a = amp * rng.uniform(0.5, 1.0) * rng.choice([-1.0, 1.0])
        ph = rng.uniform(0, 0.5)
        if mode == 'helix':
            # real ringlets: the last `length` m spiral round the strand's own end direction, radius growing from 0 to
            # |a| (u^0.7), `waves` turns, random phase / handedness per guide. Needs enough CVs (K ~21+) to resolve.
            k0 = int(np.searchsorted(d, d[-1] - length))
            t = g[-1] - g[max(k0 - 1, 0)]
            t = t / max(np.linalg.norm(t), 1e-9)
            ref = np.array([0.0, 0.0, 1.0]) if abs(t[2]) < 0.9 else np.array([1.0, 0.0, 0.0])
            n1 = np.cross(t, ref); n1 /= max(np.linalg.norm(n1), 1e-9); n2 = np.cross(t, n1)
            ang = 2 * np.pi * waves * u * np.sign(a) + 2 * np.pi * ph * 2
            r = abs(a) * u ** 0.7
            g += r[:, None] * (np.cos(ang)[:, None] * n1[None, :] + np.sin(ang)[:, None] * n2[None, :])
        elif mode == 'radial' and head is not None:
            o = g[-1, :2] - head.c[:2]
            o = o / max(np.linalg.norm(o), 1e-9)
            g[:, :2] += (abs(a) * np.sin(np.pi * waves * u + ph * u) * u)[:, None] * o[None, :]
        else:
            g[:, 0] += a * np.sin(np.pi * waves * u + ph * u) * u
        out.append(g)
    return out


def comb(G_, F, iters=6, alpha=0.5, length_mix=0.3, outlier=25.0, split=None, movable=None):
    """automatic 'comb' (R29 rethink): neighbouring guides (growth-mesh edges) are pulled toward each other's SHAPE
    (offsets from their own root), so the interpolated hairs between them stay together instead of fanning into
    single strands. Roots never move. Per iteration every guide moves `alpha` of the way to the mean shape of its
    neighbours, weighted up for neighbours it disagrees with by more than `outlier` degrees (the p90 tail is what
    differs from Geralt's groomed furs: R29 coherence table); lengths move `length_mix` toward the neighbour mean.
    G_ = list of (K,3) guides with the same K, F = (M,3) faces over the guide indices.
    split (deg, R30): neighbours whose ROOT directions (first quarter of the guide, measured once before combing)
    differ by more than `split` are not combed together at all: they lie on opposite sides of a parting / crown
    whorl (Keira R29: combing across her side part twisted the crown and opened bald patches). None = old behaviour.
    movable (R33): bool mask; only these guides are combed (the others stay as traced but still act as neighbours),
    e.g. comb the crown only and leave the nape / lower curtain full."""
    G = np.array(G_, float)
    n, K, _ = G.shape
    nb = [set() for _ in range(n)]
    for a, b, c in F:
        for u, v in ((a, b), (b, c), (c, a)):
            nb[u].add(v); nb[v].add(u)
    nb = [np.array(sorted(x), int) for x in nb]
    if split is not None:
        q = max(2, K // 4)
        r = G[:, q] - G[:, 0]
        r /= np.maximum(np.linalg.norm(r, axis=1, keepdims=True), 1e-9)
        nb = [j[np.degrees(np.arccos(np.clip((r[j] * r[i]).sum(1), -1, 1))) <= split] for i, j in enumerate(nb)]
    for _ in range(iters):
        O = G - G[:, :1]                                    # shapes relative to the root
        L = np.linalg.norm(np.diff(G, axis=1), axis=2).sum(1)
        D = np.diff(G, axis=1); D /= np.maximum(np.linalg.norm(D, axis=2, keepdims=True), 1e-9)
        new = G.copy()
        for i in range(n):
            if not len(nb[i]) or (movable is not None and not movable[i]):
                continue
            j = nb[i]
            ang = np.degrees(np.arccos(np.clip((D[j] * D[i][None]).sum(2), -1, 1))).mean(1)
            w = 1.0 + np.clip((ang - outlier) / outlier, 0, 3)
            Lj = L[j]
            Oj = O[j] * (L[i] / np.maximum(Lj, 1e-6))[:, None, None]      # neighbour shapes at this guide's length
            target = (Oj * w[:, None, None]).sum(0) / w.sum()
            o = (1 - alpha) * O[i] + alpha * target
            Lt = (1 - length_mix) * L[i] + length_mix * float((Lj * w).sum() / w.sum())
            lo = np.linalg.norm(np.diff(o, axis=0), axis=1).sum()
            new[i] = G[i, 0] + o * (Lt / max(lo, 1e-6))
        G = new
    return [g for g in G]


def jitter_lengths(G_, rng, lo=0.8, hi=1.0, K=None):
    """per-guide length variation: trim each guide to a random fraction of its length (ragged ends, no straight
    cut line), resampled to the same number of points."""
    out = []
    for g in G_:
        f = rng.uniform(lo, hi)
        d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))]
        t = np.linspace(0, d[-1] * f, len(g) if K is None else K)
        out.append(np.stack([np.interp(t, d, g[:, k]) for k in range(3)], 1))
    return out


def faces_for(R, centre, edge_abs=0.034, rmax=0.022, min_area=1e-5):
    d = R - centre
    rr = np.linalg.norm(d, axis=1)
    th = np.arccos(np.clip(d[:, 2] / rr, -1, 1)); ph = np.arctan2(d[:, 1], d[:, 0])
    xy = np.stack([th * np.cos(ph), th * np.sin(ph)], 1)
    tri = Delaunay(xy).simplices
    keep = []
    for a, b, c in tri:
        e = max(np.linalg.norm(R[a] - R[b]), np.linalg.norm(R[b] - R[c]), np.linalg.norm(R[c] - R[a]))
        A = 0.5 * np.linalg.norm(np.cross(R[b] - R[a], R[c] - R[a]))
        la, lb, lc = np.linalg.norm(R[b] - R[c]), np.linalg.norm(R[a] - R[c]), np.linalg.norm(R[a] - R[b])
        circ = la * lb * lc / max(4 * A, 1e-12)
        if e <= edge_abs and A > min_area and circ <= rmax:
            keep.append((a, b, c))
    keep = np.array(keep)
    used = np.unique(keep)
    remap = -np.ones(len(R), int); remap[used] = np.arange(len(used))
    keep = remap[keep]
    R2 = R[used]
    a_ = R2[keep[:, 1]] - R2[keep[:, 0]]; b_ = R2[keep[:, 2]] - R2[keep[:, 0]]
    nrm = np.cross(a_, b_)
    flip = ((R2[keep[:, 0]] - centre) * nrm).sum(1) < 0
    keep[flip] = keep[flip][:, [0, 2, 1]]
    return keep, used


def part_tail(F, used, tail, log=print):
    """parting between the ponytail and the loose hair (vanilla shows a gap between the tail hanging from the tie and
    the nape hair): HairWorks interpolates hairs inside each growth triangle from its three guides, so a triangle
    with both tail guides (lanes chained through a gather) and loose guides fills the gap with in-between hairs.
    Drop those mixed triangles (NVIDIA: cut the growth mesh where guides must not interpolate), then drop guides left
    without a face. tail = per-guide flag (indexed like the guides before faces_for)."""
    t = np.asarray(tail, bool)[used]
    F_all = F
    ft = t[F]
    keep = ft.all(1) | (~ft).all(1)
    F = F[keep]
    u2 = np.unique(F)
    remap = -np.ones(len(used), int); remap[u2] = np.arange(len(u2))
    log('  part_tail: %d tail guides, dropped %d mixed tail/loose faces, %d guides orphaned' % (int(t.sum()), int((~keep).sum()), len(used) - len(u2)))
    part = np.unique(remap[np.unique(np.asarray(F_all)[~keep])])
    part_tail.last_part_verts = part[part >= 0]          # guides on the parting (new indices), see build()
    return remap[F], np.asarray(used)[u2]


def ear_front(g, head):
    """a guide passing in front of the ear below the temple (a lock hanging beside the face)."""
    q = g[2:]
    c = head.c
    return bool(np.any((q[:, 1] > c[1] + 0.018) & (q[:, 2] < head.top - 0.11) & (np.abs(q[:, 0]) > 0.056)))



def regroom(G_, lanes, head, roots=None, k=24, sigma=0.015, length_k=6, momentum=0.5, standoff=0.004, blend=1.0,
            far=0.03, log=print):
    """R41 REGROOM (user: "rethink how the hair is placed"): instead of copying one card lane per guide, every guide is
    re-grown from its root as a streamline of ONE smoothed flow field built from all card lanes (the vanilla
    haircut's overall direction at every point around the head). Crossing / overlapping / flared cards average out,
    so neighbouring guides can no longer twist around each other (temple ropes), cross at a whorl or fan apart.
      field(x)  = gaussian-weighted (sigma) mean of the unit tangents of the k nearest card-lane points;
                  beyond `far` from every card point the field turns to gravity
      length    = median arc length of the length_k nearest guides (by root): smooth length map, layering kept
      step      = previous direction * momentum + field * (1 - momentum), never into the head (sd >= standoff)
      blend     = 1 fully regrown, < 1 mixes with the traced guide (same arc-length parameter)."""
    from scipy.spatial import cKDTree
    P, T = [], []
    for s in lanes:
        p = np.asarray(s['p'], float)
        if len(p) < 2:
            continue
        t = np.gradient(p, axis=0)
        n = np.linalg.norm(t, axis=1, keepdims=True)
        ok = n[:, 0] > 1e-7
        P.append(p[ok]); T.append(t[ok] / n[ok])
    if not P:
        return G_
    P = np.concatenate(P); T = np.concatenate(T)
    tree = cKDTree(P)
    kk = min(k, len(P))
    down = np.array([0.0, 0.0, -1.0])

    def field(x):
        d, i = tree.query(x, k=kk)
        d = np.atleast_1d(d); i = np.atleast_1d(i)
        w = np.exp(-(d / sigma) ** 2)
        v = (T[i] * w[:, None]).sum(0)
        nv = np.linalg.norm(v)
        v = v / nv if nv > 1e-9 else down
        g = np.clip((d.min() - far) / far, 0.0, 1.0)          # far from every card: fall with gravity
        v = v * (1 - g) + down * g
        return v / max(np.linalg.norm(v), 1e-9)

    def arclen(g):
        return float(np.linalg.norm(np.diff(g, axis=0), axis=1).sum())

    def resample(g, n):
        d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))]
        if d[-1] < 1e-9:
            return np.repeat(g[:1], n, 0)
        t = np.linspace(0, d[-1], n)
        return np.stack([np.interp(t, d, g[:, c]) for c in range(3)], 1)

    R = np.array([g[0] for g in G_])
    L = np.array([arclen(g) for g in G_])
    rt = cKDTree(R)
    _, nb = rt.query(R, k=min(length_k, len(R)))
    nb = np.atleast_2d(nb)
    Ls = np.median(L[nb], axis=1)
    out = []
    for gi, g in enumerate(G_):
        n_cv = len(g)
        step = Ls[gi] / max(n_cv - 1, 1)
        x = g[0].copy()
        pts = [x.copy()]
        prev = None
        for _ in range(n_cv - 1):
            f = field(x)
            d = f if prev is None else prev * momentum + f * (1 - momentum)
            d /= max(np.linalg.norm(d), 1e-9)
            sd, nrm, _dd = head.signed(x[None])
            sd = float(sd[0]); nrm = nrm[0]
            if sd < standoff + 0.002 and d @ nrm < 0:           # never steer into the scalp
                d = d - (d @ nrm) * nrm
                d /= max(np.linalg.norm(d), 1e-9)
            x = x + d * step
            sd2, nrm2, _ = head.signed(x[None])
            if sd2[0] < standoff:
                x = x + nrm2[0] * (standoff - sd2[0])
            pts.append(x.copy())
            prev = d
        ng = np.array(pts)
        if blend < 1.0:
            ng = blend * ng + (1 - blend) * resample(g, n_cv)
        out.append(ng)
    log('  regroom: %d guides re-grown along the smoothed card flow field (k %d, sigma %.3f, momentum %.2f, blend %.2f)'
        % (len(out), kk, sigma, momentum, blend))
    return out

def build(cfg, head, ch, alpha_img, gathers=(), seed=5, log=print):
    """cfg: dict(root_spacing, reach, lane_w, K, hug_c0, hug_k, ...). Returns dict(S, F, roots, dirs, lanes, info)."""
    rng = np.random.default_rng(seed)
    drop = np.zeros(ch['nv'], bool)
    for _c, _r, m in gathers:
        drop |= m
    for m in cfg.get('_extra_drop', []):
        drop |= m
    lanes = card_lanes(ch, head, alpha_img, lane_w=cfg.get("lane_w", 0.012), drop=drop, edge_filter=cfg.get("edge_lane_filter", True),
                       alpha_tip=cfg.get('alpha_tip', 0.08))    # R81: higher = lanes stop where the card visibly ends
    if cfg.get('chain_segments', True):     # R39 switch: chaining can make guides longer than the cards
        lanes = chain_segments(lanes, head, log=log)
    if cfg.get('split_arcs'):
        # R78: cap / bowl cards arc from one side of the head over the crown to the other; with root = the end nearer
        # the scalp a guide then climbs from one temple over the top and hangs down the OTHER side (Bowl: median guide
        # 23 cm for a bowl cut, 217 / 414 guides > 2x their chord). Such lanes are split at their highest point into
        # two lanes rooted at the apex, so the hair falls from the crown down both sides like a real cut.
        dz = cfg['split_arcs'] if isinstance(cfg['split_arcs'], float) else 0.03
        out_, ns = [], 0
        for s_ in lanes:
            p_ = s_['p']; k_ = int(np.argmax(p_[:, 2]))
            if 2 <= k_ <= len(p_) - 3 and p_[0, 2] < p_[k_, 2] - dz and p_[-1, 2] < p_[k_, 2] - dz:
                a_ = dict(s_); b_ = dict(s_)
                a_['p'], a_['uv'], a_['a'] = p_[k_:], s_['uv'][k_:], s_['a'][k_:]
                b_['p'], b_['uv'], b_['a'] = p_[:k_ + 1][::-1], s_['uv'][:k_ + 1][::-1], s_['a'][:k_ + 1][::-1]
                out_ += [a_, b_]; ns += 1
            else:
                out_.append(s_)
        log('  split_arcs: %d over-the-top lanes split at their apex (%d -> %d lanes)' % (ns, len(lanes), len(out_)))
        lanes = out_
    nl = len(lanes)
    nchain = 0
    for c, r, _m in gathers:
        lanes, n = chain_lanes(lanes, c, r, rng)
        nchain += n
    vis = visible_card_points(ch, alpha_img, alpha_vis=cfg.get('alpha_vis', 0.3), drop=drop)
    roots = sample_scalp(head, vis, r=cfg.get('root_spacing', 0.0125), reach=cfg.get('reach', 0.012),
                         zmin_rel=cfg.get('zmin_rel', 0.20), max_sd=cfg.get('root_max_sd', 0.03))
    if cfg.get('root_back_y') is not None:
        # 2026-10-06 (split_cards): scalp roots only behind head.c.y + root_back_y (hair hanging from under an updo)
        roots = roots[roots[:, 1] < head.c[1] + cfg['root_back_y']]
    if cfg.get('root_below') is not None:
        roots = roots[roots[:, 2] < head.top - cfg['root_below']]      # ... and below top - root_below (not on the crown under the updo)
    lane_ok = None
    if cfg.get('drop_skull_lanes'):
        # cap / under-layer cards whose lane ENDS on the skull above the ear line are hidden under the outer cards in
        # the card version; followed by guides they end as stubs on the crown (fuzzy crown in r6a). Dropped.
        zl = head.top - cfg.get('skull_lane_zrel', 0.15)
        lane_ok = []
        for s in lanes:
            sd_t = head.signed(s['p'][-1:])[0][0]
            lane_ok.append(not (s['p'][-1][2] > zl and abs(sd_t) < cfg.get('skull_lane_sd', 0.015)))
        log('  skull-ending lanes dropped: %d of %d' % (len(lanes) - sum(lane_ok), len(lanes)))
    if cfg.get('lane_attach'):
        # 2026-10-06 (split_cards, Anna: updo on the vanilla cards, strands only for the hanging hair): the hanging locks
        # start 2-6 cm off the skull under the updo, where no `reach`-limited scalp root exists, so they got no guide.
        # Every lane start (root end) within `dist` of the skull gets a root on the scalp point nearest to it (greedy
        # Poisson thinning, `spacing`, against the existing roots too); the guide then leaves the skull under the updo
        # cards and joins the lane (guides(): offset to the lane fades out over the first third)
        la = cfg['lane_attach'] if isinstance(cfg['lane_attach'], dict) else {}
        st_ = np.array([s_['p'][0] for i_, s_ in enumerate(lanes) if lane_ok is None or lane_ok[i_]])
        zone_ = (head.v[:, 2] > head.top - la.get('zmin_rel', 0.22)) & (head.v[:, 1] < head.c[1] + la.get('front_y', 0.0))
        zv_ = head.v[zone_]
        d_, vi_ = cKDTree(zv_).query(st_)         # nearest scalp point inside the root zone (back of the head, above the nape)
        cand = zv_[vi_]
        ok_ = d_ < la.get('dist', 0.07)
        spc = la.get('spacing', cfg.get('root_spacing', 0.0125))
        kd_ = cKDTree(roots) if len(roots) else None
        added = []
        for q in cand[ok_][rng.permutation(int(ok_.sum()))]:
            if kd_ is not None and kd_.query(q)[0] < spc:
                continue
            if added and np.min(np.linalg.norm(np.array(added) - q, axis=1)) < spc:
                continue
            added.append(q)
        if added:
            roots = np.vstack([roots, np.array(added)]) if len(roots) else np.array(added)
        log('  lane_attach: %d extra scalp roots under the upper ends of %d lanes (dist %.0f mm)' % (len(added), int(ok_.sum()), la.get('dist', 0.07) * 1000))
    Gd, kept, src = guides(roots, lanes, K=cfg.get('K', 8), maxd=cfg.get('lane_maxd', 0.03), pick=cfg.get('lane_pick', 'longest'),
                           pick_power=cfg.get('lane_pick_power', 1.0), rng=np.random.default_rng(seed + 2),
                           blend=cfg.get('lane_blend', 0.0), near=cfg.get('lane_near', 0.012), lane_ok=lane_ok,
                           offset_keep=cfg.get('lane_offset_keep', 0.0), min_rem=cfg.get('lane_min_rem', 0.04))
    if cfg.get('length_jitter'):
        Gd = jitter_lengths(Gd, rng, 1.0 - cfg['length_jitter'], 1.0)
    if cfg.get('regroom'):
        rg = cfg['regroom'] if isinstance(cfg['regroom'], dict) else {}
        Gd = regroom(Gd, lanes, head, k=rg.get('k', 24), sigma=rg.get('sigma', 0.015), length_k=rg.get('length_k', 6),
                     momentum=rg.get('momentum', 0.5), standoff=rg.get('standoff', 0.004), blend=rg.get('blend', 1.0),
                     far=rg.get('far', 0.03), log=log)
    if cfg.get('hug', True):
        zmin = head.top - cfg['hug_zmin_rel'] if cfg.get('hug_zmin_rel') is not None else None
        ymax = head.c[1] + cfg['hug_zmin_y'] if cfg.get('hug_zmin_y') is not None else None
        Gd = hug(Gd, head, c0=cfg.get('hug_c0', 0.003), k=cfg.get('hug_k', 0.35), k_front=cfg.get('hug_front_k'),
                 central_front=cfg.get('hug_central_front', True), zmin=zmin, zmin_ymax=ymax)
    if cfg.get('flare'):
        Gd = flare_guides(Gd, head, rng, k=cfg['flare'], noise=cfg.get('flare_noise', 0.5), t0=cfg.get('flare_t0', 0.35), back_weight=cfg.get('flare_back_weight', False))
    if cfg.get('tip_curl'):
        Gd = curl_tips(Gd, np.random.default_rng(seed + 7), length=cfg.get('tip_curl_len', 0.05), amp=cfg['tip_curl'], waves=cfg.get('tip_curl_waves', 1.0), head=head, mode=cfg.get('tip_curl_mode', 'x'))
    # R81: keep_face_locks = do not drop the locks hanging beside the face (the R1 'ear_front' rule removed Triss's /
    # Keira's / Shani's side locks, which are part of their styles)
    keep = [i for i, g in enumerate(Gd) if (cfg.get('keep_face_locks') or not ear_front(g, head)) and
            not (cfg.get('swept_back') and sweeps_forward(g, fwd=cfg.get('swept_fwd', 0.02)))]
    if cfg.get('swept_back'):
        log('  swept-back style: dropped %d forward-sweeping guides' % sum(1 for g in Gd if sweeps_forward(g, fwd=cfg.get('swept_fwd', 0.02))))
    Gd = [Gd[i] for i in keep]; src = [src[i] for i in keep]
    R = np.array([g[0] for g in Gd])
    F, used = faces_for(R, head.c, cfg.get('edge', 0.034), cfg.get('rmax', 0.022))
    if cfg.get('part_tail'):
        F, used = part_tail(F, used, [bool(lanes[src[k][0]].get('chained')) for k in range(len(src))], log)
    Gd = [Gd[i] for i in used]; src = [src[i] for i in used]
    R = R[used]
    if cfg.get('comb'):
        cb = cfg['comb'] if isinstance(cfg['comb'], dict) else {}
        mov = None
        if cb.get('top_only') is not None:
            # comb only guides rooted within `top_only` m of the highest root (the crown); nape / lower roots untouched
            zr = np.array([g[0][2] for g in Gd])
            mov = zr > zr.max() - cb['top_only']
            log('  comb top_only %.3f m: %d of %d guides combed' % (cb['top_only'], int(mov.sum()), len(Gd)))
        Gd = comb(Gd, F, iters=cb.get('iters', 6), alpha=cb.get('alpha', 0.5), length_mix=cb.get('length_mix', 0.3),
                  outlier=cb.get('outlier', 25.0), split=cb.get('split'), movable=mov)
        log('  comb: %d guides combed (%s)' % (len(Gd), cb or 'defaults'))
        if cb.get('split') is not None and cb.get('cut', True):
            # cut the growth mesh along the parting: a face spanning two flow regions would interpolate hairs that
            # fan across the part (same idea as part_tail); orphaned guides are dropped
            K = len(Gd[0]); q = max(2, K // 4)
            r = np.array([g[q] - g[0] for g in Gd]); r /= np.maximum(np.linalg.norm(r, axis=1, keepdims=True), 1e-9)
            ang = lambda u, v: np.degrees(np.arccos(np.clip((r[u] * r[v]).sum(-1), -1, 1)))
            bad = (ang(F[:, 0], F[:, 1]) > cb['split']) | (ang(F[:, 1], F[:, 2]) > cb['split']) | (ang(F[:, 2], F[:, 0]) > cb['split'])
            F = F[~bad]
            u2 = np.unique(F)
            remap = -np.ones(len(Gd), int); remap[u2] = np.arange(len(u2))
            log('  comb split: cut %d faces across partings (> %g deg), %d guides orphaned' % (int(bad.sum()), cb['split'], len(Gd) - len(u2)))
            F = remap[F]; Gd = [Gd[i] for i in u2]; src = [src[i] for i in u2]; R = R[u2]
            if cfg.get('part_tail') and getattr(part_tail, 'last_part_verts', None) is not None:
                pv = remap[np.asarray(part_tail.last_part_verts, int)]
                part_tail.last_part_verts = pv[pv >= 0]
    D = np.array([(g[1] - g[0]) / max(np.linalg.norm(g[1] - g[0]), 1e-9) for g in Gd])
    S = [dict(p=g, w=[(0, 1.0)]) for g in Gd]
    log('lanes %d (+%d chained through gathers), visible card pts %d, scalp roots %d, guides %d, faces %d' % (
        nl, nchain, len(vis), len(roots), len(S), len(F)))
    part_verts = getattr(part_tail, 'last_part_verts', None) if cfg.get('part_tail') else None
    return dict(S=S, F=F, roots=R, dirs=D, lanes=lanes, src=src, vis=vis, all_roots=roots, centre=head.c,
                edge=cfg.get('edge', 0.034), rmax=cfg.get('rmax', 0.022),
                part_verts=(part_verts.tolist() if part_verts is not None else []))


def fill_holes(res, head, ch, alpha_img, hole=0.008, need=0.012, zmin=0.17, k=3, max_new=400, near_skin=0.02,
               seed=17, log=print, ear_clear=0.0, guard=True, guard_dist=0.01, cover=0.0):
    """2026-10-04 (HairLab, user: "fill gaps by adding / duplicating strands where there are holes"; the L16 masks showed
    new ROOTS closed Lambert's missing area while density / width did not). Scalp points (random samples of the head
    surface above top - zmin, not on the face) that have VANILLA hair over them (a visible card point lying within
    `near_skin` of the skin, within `need` of the point) but no guide root within `hole` get a new guide: root = that
    point, shape = the inverse-distance blend of the k nearest guides' root-relative curves. Poisson-spaced (>= hole),
    at most max_new. The growth mesh is rebuilt (reindex). -> (res, number added)."""
    rng = np.random.default_rng(seed)
    S = res['S']
    G = np.array([np.asarray(s['p'], float) for s in S])
    R = G[:, 0]
    V, T = head.v, head.t
    A = V[T[:, 1]] - V[T[:, 0]]; B = V[T[:, 2]] - V[T[:, 0]]
    area = 0.5 * np.linalg.norm(np.cross(A, B), axis=1) + 1e-12
    n = 30000
    ti = rng.choice(len(T), n, p=area / area.sum())
    u, v = rng.random(n), rng.random(n)
    f = u + v > 1
    u[f], v[f] = 1 - u[f], 1 - v[f]
    P = V[T[ti, 0]] + u[:, None] * A[ti] + v[:, None] * B[ti]
    P = P[P[:, 2] > head.top - zmin]
    P = P[~face_roots(P, head)]
    if ear_clear:
        # keep the ears free: no new root within ear_clear of the ear (vanilla cards hanging over the ear count as
        # "hair over the scalp" there and the first fill grew hair over Lambert's ears)
        E = ear_points(head)
        if len(E):
            P = P[cKDTree(E).query(P)[0] > ear_clear]
    vis_all = visible_card_points(ch, alpha_img, n=60000, alpha_vis=0.35, edge_filter=False)
    vis = vis_all[np.abs(head.signed(vis_all)[0]) < near_skin]
    if not len(vis) or not len(P):
        return res, 0
    has = cKDTree(vis).query(P)[0] < need
    far = cKDTree(R).query(P)[0] > hole
    if cover:
        # 2026-10-05 (L31: bare scalp beside Keira's / Shani's parting although roots exist there - every strand
        # runs AWAY from it): test coverage by OUR rendered hairs near the skin, not by roots
        from silhouette import hair_points
        Hp = hair_points(G, np.asarray(res['F'], int), n=4, k_sub=2)
        Hp = Hp[np.abs(head.signed(Hp)[0]) < near_skin]
        nf0 = int(far.sum())
        far = far | (cKDTree(Hp).query(P)[0] > cover)
        log('  fill_holes cover %.0f mm: %d faces, %d hair points near the skin, uncovered scalp points %d -> %d' % (
            cover * 1000, len(res['F']), len(Hp), nf0, int(far.sum())))
    cand = P[has & far]
    rng.shuffle(cand)
    picked = []
    kd_new = None
    for q in cand:
        if len(picked) >= max_new:
            break
        if picked and np.min(np.linalg.norm(np.array(picked) - q, axis=1)) < hole:
            continue
        picked.append(q)
    if not picked:
        return res, 0
    kd = cKDTree(R)
    rel = G - R[:, None, :]
    new = []
    # 2026-10-05 guard (Keira: added hairline guides copied side-curtain shapes and hung over the face; Lambert: temple
    # guides swept forward = the "spikes"): reject an added guide that (a) passes in front of the face farther than
    # guard_dist from visible card hair, (b) touches the face skin, (c) on the sides (|x| > 4 cm) ends > 3 mm in front
    # of its root
    kvis = cKDTree(vis_all) if guard else None
    rejected = 0
    for q in picked:
        d, idx = kd.query(q, k=min(k, len(R)))
        d, idx = np.atleast_1d(d), np.atleast_1d(idx)
        w = 1.0 / np.maximum(d, 1e-4)
        shape = (rel[idx] * w[:, None, None]).sum(0) / w.sum()
        p_new = q[None, :] + shape
        if guard:
            inface = (p_new[:, 1] > head.c[1] + 0.03) & (p_new[:, 2] < head.top - 0.07) & (np.abs(p_new[:, 0]) < 0.085)
            bad = False
            if inface[1:].any():
                if (kvis.query(p_new[1:][inface[1:]])[0] > guard_dist).any():
                    bad = True
                if (head.signed(p_new[1:][inface[1:]])[0] < 0.012).any():
                    bad = True
            if abs(q[0]) > 0.04 and p_new[-1][1] - q[1] > 0.003:
                bad = True
            if bad:
                rejected += 1
                continue
        s_ = dict(S[int(idx[0])])
        s_['p'] = p_new
        new.append(s_)
    if guard:
        log('  fill_holes guard: %d added guides rejected (over the face / on the skin / sweeping forward)' % rejected)
    r2 = dict(res)
    r2['S'] = list(S) + new
    log('  fill_holes: %d guides added where vanilla has hair but no root within %.0f mm (%d candidates)' % (
        len(new), hole * 1000, len(cand)))
    return reindex(r2, list(range(len(r2['S'])))), len(new)


def bare_roots(roots, head, ch, alpha_img, dist=0.008, near_skin=0.015, front_y=0.0, below=0.0):
    """2026-10-05 (user: "a few strands starting too much on his forehead"): roots with no VISIBLE vanilla card hair
    lying on the skin within `dist` = roots on bare skin. Only roots in front of the crown centre + front_y and lower
    than top - below are tested (the forehead / hairline). -> bool mask (True = bare)."""
    R = np.asarray(roots, float)
    vis = visible_card_points(ch, alpha_img, n=80000, alpha_vis=0.35, edge_filter=False)
    vis = vis[np.abs(head.signed(vis)[0]) < near_skin]
    if not len(vis):
        return np.zeros(len(R), bool)
    d = cKDTree(vis).query(R)[0]
    zone = (R[:, 1] > head.c[1] + front_y) & (R[:, 2] < head.top - below)
    return zone & (d > dist)


def split_divergent(G, F, head, ratio=4.0, front_only=True):
    """2026-10-05 (Keira / Shani: hairs over the face for 20 h): a rendered hair is the blend of its triangle's 3
    guides; where they part (left / right of a parting, side curtain vs fringe) the blend runs down the middle = over
    the face. Drop growth faces whose guide-tip spread exceeds `ratio` x their root spread (front half only by default).
    -> (kept faces, number dropped)"""
    G = np.asarray(G, float)
    F = np.asarray(F, int)
    keep = []
    for f in F:
        tips, roots = G[f, -1], G[f, 0]
        dv = max(np.linalg.norm(tips[i] - tips[j]) for i in range(3) for j in range(i + 1, 3))
        dr = max(np.linalg.norm(roots[i] - roots[j]) for i in range(3) for j in range(i + 1, 3))
        front = (roots[:, 1] > head.c[1]).any()
        keep.append(not (dv > ratio * max(dr, 1e-4) and (front or not front_only)))
    keep = np.array(keep, bool)
    return F[keep], int((~keep).sum())


def split_skin_blend(G, F, head, min_ratio=4.0, dist=0.02, gap=0.02, front_y=-0.03, below=0.03, face_x=0.1, n=5, log=print):
    """2026-10-06 (L126, Shani: clumps of thin strands over the forehead where no guide is drawn; Groom Studio 'pick hair'
    traced them to growth faces whose roots sit just BEHIND split_divergent's front zone (root y 0-1 cm below head.c[1],
    tip/root spread 11-29) with two fringe guides sweeping across the forehead and one side guide going down: the blends
    of the three run over the forehead skin far from every guide). Opt-in second pass: among faces with tip/root spread >
    `min_ratio`, drop a face when some interpolated hair (barycentric grid `n`, k >= 2) lies within `dist` of the head
    surface in front (y > head.c[1] + front_y, z < top - below, |x| < face_x) AND farther than `gap` from the three guides'
    own points at that k (a hair with no guide near it). Faces keep their guides. -> (kept faces, number dropped)"""
    keep = skin_blend_keep(G, F, head, min_ratio, dist, gap, front_y, below, face_x, n)
    log('  split_skin_blend: %d growth faces whose blended hairs lie on the forehead far from their guides dropped' % int((~keep).sum()))
    return np.asarray(F, int)[keep], int((~keep).sum())


def skin_blend_keep(G, F, head, min_ratio, dist, gap, front_y, below, face_x, n):
    G = np.asarray(G, float)
    F = np.asarray(F, int)
    K = G.shape[1]
    ws = np.array([(i / n, j / n, 1 - i / n - j / n) for i in range(n + 1) for j in range(n + 1 - i)])
    ws = ws[(ws.max(1) < 0.999)]
    keep = np.ones(len(F), bool)
    for fi, f in enumerate(F):
        tips, roots = G[f, -1], G[f, 0]
        dv = max(np.linalg.norm(tips[i] - tips[j]) for i in range(3) for j in range(i + 1, 3))
        dr = max(np.linalg.norm(roots[i] - roots[j]) for i in range(3) for j in range(i + 1, 3))
        if dv <= min_ratio * max(dr, 1e-4):
            continue
        Gf = G[f]                                              # (3, K, 3)
        B = np.einsum('wi,ikc->wkc', ws, Gf)[:, 2:, :]         # (W, K-2, 3) blended hair points past the root
        zone = (B[..., 1] > head.c[1] + front_y) & (B[..., 2] < head.top - below) & (np.abs(B[..., 0]) < face_x)
        if not zone.any():
            continue
        # distance of every blended point to the three guide points at the same k
        d3 = np.linalg.norm(B[:, :, None, :] - np.transpose(Gf[:, 2:, :], (1, 0, 2))[None, :, :, :], axis=-1).min(2)
        P = B[zone]
        sd = head.signed(P)[0]
        near = np.zeros(zone.shape, bool)
        near[zone] = sd < dist
        if (near & (d3 > gap)).any():
            keep[fi] = False
    return keep


def fringe_lane(res, head, min_ratio=4.0, dist=0.02, gap=0.02, front_y=-0.03, below=0.03, face_x=0.1, n=5, spacing=0.025,
                max_per_edge=8, min_gap=0.0015, margin=None, jitter=0.0015, x_min=None, log=print):
    """2026-10-06 (L126b, Shani: the forehead fringe is the blend fan of divergent faces whose roots sit just behind
    split_divergent's zone; the fan has no guide of its own). Opt-in: for every face split_skin_blend would drop, each
    edge whose two guide tips are more than `spacing` apart gets intermediate GUIDES (1 - n copies by linear
    interpolation of the two guides' points, root snapped onto the scalp, rejected where they touch the skin / head), so
    the fan is carried by real guides about `spacing` apart at the tips; the faces between them are no longer 'far from
    their guides' and survive split_skin_blend, only the faces across the original gap are dropped. -> (res, number added)"""
    S = res['S']
    G = np.array([np.asarray(s_['p'], float) for s_ in S])
    F = np.asarray(res['F'], int)
    bad = ~skin_blend_keep(G, F, head, min_ratio, dist, gap, front_y, below, face_x, n)
    edges = {}
    for f in F[bad]:
        for a_, b_ in ((f[0], f[1]), (f[1], f[2]), (f[0], f[2])):
            k = (min(a_, b_), max(a_, b_))
            edges[k] = float(np.linalg.norm(G[a_, -1] - G[b_, -1]))
    new, skin = [], 0
    R = G[:, 0]
    chains = []
    for (a_, b_), D in edges.items():
        chain = []
        m = min(int(np.ceil(D / spacing)) - 1, max_per_edge)
        for i in range(1, m + 1):
            t = i / (m + 1)
            q = (1 - t) * G[a_] + t * G[b_]
            if jitter:
                # alternate the lane roots +- jitter off the a-b line (along the surface) so consecutive triples have a real area
                dl = G[b_][0] - G[a_][0]
                nr0 = head.signed(q[0][None, :])[1][0]
                pp = np.cross(nr0, dl)
                q = q + (pp / max(np.linalg.norm(pp), 1e-9) * jitter * (1 if i % 2 else -1))[None, :]
            sd, nrm, _ = head.signed(q[0][None, :])
            q = q - (nrm[0] * sd[0])[None, :] * np.linspace(1, 0, q.shape[0])[:, None]   # root onto the scalp, tip untouched
            if margin is not None:
                # push the lerped points that fell inside the head back out to `margin` above the skin, then smooth the curve
                for _ in range(3):
                    sd_, nr_, _d = head.signed(q[1:])
                    q[1:] += nr_ * np.maximum(margin - sd_, 0)[:, None]
                    q[2:-1] = 0.25 * q[1:-2] + 0.5 * q[2:-1] + 0.25 * q[3:]
                sd_, nr_, _d = head.signed(q[1:])
                q[1:] += nr_ * np.maximum(margin - sd_, 0)[:, None]
            elif (head.signed(q[2:])[0] < 0.002).any():
                skin += 1
                continue
            if x_min is not None and q[-1, 0] < x_min:      # keep only the lanes whose tip ends on the swoop side (x >= x_min)
                continue
            if np.min(np.linalg.norm(R - q[0], axis=1)) < min_gap or (new and np.min(np.linalg.norm(np.array([x['p'][0] for x in new]) - q[0], axis=1)) < min_gap):
                continue
            s_ = dict(S[a_ if t < 0.5 else b_])
            s_['p'] = q
            new.append(s_)
            chain.append(s_)
        if chain:
            chains.append((S[a_], chain, S[b_]))
    log('  fringe_lane: %d faces flagged, %d edges, %d lane guides added (%d rejected on the skin / head)' % (int(bad.sum()), len(edges), len(new), skin))
    if not new:
        return res, 0
    # no re-triangulation (it would orphan the collinear lane guides): append the guides, keep every existing face
    out = dict(res)
    out['S'] = list(S) + new
    Dn = np.array([(s_['p'][1] - s_['p'][0]) / max(np.linalg.norm(s_['p'][1] - s_['p'][0]), 1e-9) for s_ in new])
    out['roots'] = np.concatenate([np.asarray(res['roots'], float), np.array([s_['p'][0] for s_ in new])])
    out['dirs'] = np.concatenate([np.asarray(res['dirs'], float), Dn])
    out['F'] = np.asarray(res['F'], int)
    # the lane roots lie on a line a few mm long: Delaunay + the area / circumradius limits drop their faces, so the fan between the two
    # original guides is stitched explicitly: triangles over each three consecutive guides of the chain a, lane_1 .. lane_m, b
    ix = {id(s_): i for i, s_ in enumerate(out['S'])}
    Rn = np.array([np.asarray(s_['p'], float)[0] for s_ in out['S']])
    cen = np.asarray(res.get('centre', head.c), float)
    add = []
    for a_d, chain, b_d in chains:
        ids = [ix.get(id(a_d)) if (x_min is None or a_d['p'][-1][0] >= x_min) else -1] + [ix.get(id(c_)) for c_ in chain] + [ix.get(id(b_d)) if (x_min is None or b_d['p'][-1][0] >= x_min) else -1]
        ids = [i_ for i_ in ids if i_ != -1]
        if None in ids or len(ids) < 3:
            continue
        for i in range(len(ids) - 2):
            add.append((ids[i], ids[i + 1], ids[i + 2]))
    if add:
        add = np.array(add, int)
        nrm = np.cross(Rn[add[:, 1]] - Rn[add[:, 0]], Rn[add[:, 2]] - Rn[add[:, 0]])
        flip = ((Rn[add[:, 0]] - cen) * nrm).sum(1) < 0
        add[flip] = add[flip][:, [0, 2, 1]]
        out = dict(out)
        out['F'] = np.concatenate([np.asarray(out['F'], int), add])
        out['lane_F'] = add.copy()       # exempt from the final split_skin_blend drop (hairpipe)
    log('  fringe_lane: %d fan faces stitched' % len(add))
    return out, len(new)


def split_fill(res, head, ratio=4.0, front_only=True, frac=0.4, iters=2, min_gap=0.003, log=print, dropper=None):
    """2026-10-05 (L31: dark bald strip at Keira's / Shani's parting): split_divergent DROPS the faces between
    diverging guides, so nothing is rendered there. Here each dropped face gets, per corner, a COPY of that corner's
    guide (same root-relative shape, no blending) rooted `frac` of the way toward the face centroid. After the
    re-triangulation, faces between a guide and its copy are not divergent and render hair; only the faces across the
    parting line stay divergent and are dropped by the final split_divergent. Repeated `iters` times (the gap halves).
    -> (res, number added)"""
    added = 0
    for it in range(iters):
        S = res['S']
        G = np.array([np.asarray(s['p'], float) for s in S])
        F = np.asarray(res['F'], int)
        Fk, nd = dropper(G, F) if dropper else split_divergent(G, F, head, ratio=ratio, front_only=front_only)   # dropper: L126 opt-in (skin_blend fill)
        if not nd:
            break
        kept = set(map(tuple, Fk.tolist()))
        drop = [f for f in F.tolist() if tuple(f) not in kept]
        R = G[:, 0]
        roots = list(R)
        kd = cKDTree(R)
        new = []
        for f in drop:
            cen = R[f].mean(0)
            for i in f:
                q = R[i] + frac * (cen - R[i])
                if kd.query(q)[0] < min_gap:
                    continue
                if new and np.min(np.linalg.norm(np.array([n_['p'][0] for n_ in new]) - q, axis=1)) < min_gap:
                    continue
                s_ = dict(S[i])
                s_['p'] = q[None, :] + (G[i] - R[i][None, :])
                new.append(s_)
        if not new:
            break
        r2 = dict(res)
        r2['S'] = list(S) + new
        res = reindex(r2, list(range(len(r2['S']))))
        added += len(new)
        log('  split_fill pass %d: %d dropped faces -> %d guide copies added' % (it + 1, nd, len(new)))
    return res, added


def cover_fill(res, head, ch, alpha_img, gap=0.004, spacing=0.006, reach=0.03, max_shift=0.02, front_y=0.02,
               below=0.03, max_new=300, passes=2, log=print):
    """2026-10-05 (L32, build/front_cover.py: bare forehead between Shani's parting and her sweep, bald temple patch on
    Keira, where vanilla cards LIE ON the forehead): visible card points in front of the face (y > centre + front_y,
    z < top - below) farther than `gap` from any of our rendered hairs are targets. For each target (Poisson-spaced
    `spacing`) the guide passing nearest to it (closest point past the root, within `reach`) is COPIED, shifted by the
    offset to the target (<= max_shift) and its root snapped back onto the scalp - the copy runs through the gap with
    the same shape. Copies touching the skin are rejected. -> (res, number added)"""
    from silhouette import hair_points
    vis = visible_card_points(ch, alpha_img, n=120000, alpha_vis=0.35, edge_filter=False)
    vis = vis[(vis[:, 1] > head.c[1] + front_y) & (vis[:, 2] < head.top - below)]
    added = 0
    for ps in range(passes):
        S = res['S']
        G = np.array([np.asarray(s['p'], float) for s in S])
        Hp = hair_points(G, np.asarray(res['F'], int), n=4, k_sub=2)
        d = cKDTree(Hp).query(vis)[0]
        T = vis[d > gap]
        if not len(T):
            break
        rng = np.random.default_rng(5 + ps)
        T = T[rng.permutation(len(T))]
        picked = []
        for t in T:
            if picked and np.min(np.linalg.norm(np.array(picked) - t, axis=1)) < spacing:
                continue
            picked.append(t)
            if len(picked) >= max_new:
                break
        P = G[:, 2:, :].reshape(-1, 3)          # guide points past the root
        own = np.repeat(np.arange(len(G)), G.shape[1] - 2)
        kd = cKDTree(P)
        new, rej = [], 0
        for t in picked:
            dd, ii = kd.query(t)
            if dd > reach:
                continue
            g = own[ii]
            j = ii % (G.shape[1] - 2) + 2
            delta = t - G[g, j]
            nd = np.linalg.norm(delta)
            if nd > max_shift:
                delta *= max_shift / nd
            r = G[g, 0] + delta
            sd, nrm, _ = head.signed(r[None, :])
            r = r - nrm[0] * sd[0]                # snap the root onto the scalp
            p_new = G[g] + (r - G[g, 0])[None, :]
            if (head.signed(p_new[2:])[0] < 0.002).any():
                rej += 1
                continue
            s_ = dict(S[g])
            s_['p'] = p_new
            new.append(s_)
        if not new:
            break
        r2 = dict(res)
        r2['S'] = list(S) + new
        res = reindex(r2, list(range(len(r2['S']))))
        added += len(new)
        log('  cover_fill pass %d: %d uncovered card points in front, %d targets -> %d shifted guide copies (%d rejected on the skin)' % (
            ps + 1, len(T), len(picked), len(new), rej))
    return res, added


def hairline_curve(head, ch, alpha_img, phi_max=110.0, bin_deg=4.0, near_skin=0.015, cover=0.006, gap=0.012, smooth=3,
                   seed=3):
    """vanilla hairline as a curve: scalp samples in spherical coordinates around the head centre (phi = azimuth,
    0 = front, + = subject's left (+x); el = elevation). A sample is covered when a visible card point lies on the skin
    (within near_skin) within `cover` of it. Per azimuth bin (|phi| < phi_max) the line is where coverage, walking
    down from the top, first stops for more than `gap`. Median-smoothed over `smooth` bins.
    -> (phi centres deg, line elevation deg, line points xyz, scalp samples, their phi / el)"""
    rng = np.random.default_rng(seed)
    V, T = head.v, head.t
    A = V[T[:, 1]] - V[T[:, 0]]; B = V[T[:, 2]] - V[T[:, 0]]
    area = 0.5 * np.linalg.norm(np.cross(A, B), axis=1) + 1e-12
    ti = rng.choice(len(T), 60000, p=area / area.sum())
    u, v = rng.random(60000), rng.random(60000)
    f = u + v > 1
    u[f], v[f] = 1 - u[f], 1 - v[f]
    P = V[T[ti, 0]] + u[:, None] * A[ti] + v[:, None] * B[ti]
    c = np.array(head.c, float)
    P = P[P[:, 2] > head.top - 0.16]
    d = P - c
    phi = np.degrees(np.arctan2(d[:, 0], d[:, 1]))
    el = np.degrees(np.arctan2(d[:, 2], np.hypot(d[:, 0], d[:, 1])))
    vis = visible_card_points(ch, alpha_img, n=120000, alpha_vis=0.35, edge_filter=False)
    vis = vis[np.abs(head.signed(vis)[0]) < near_skin]
    cov = cKDTree(vis).query(P)[0] < cover
    bins = np.arange(-phi_max, phi_max + 1e-6, bin_deg)
    line = np.full(len(bins), np.nan)
    rad = np.degrees(gap / 0.09)
    for i, b in enumerate(bins):
        m = np.abs(phi - b) < bin_deg / 2
        if m.sum() < 20:
            continue
        e, cv = el[m], cov[m]
        o = np.argsort(-e)
        e, cv = e[o], cv[o]
        if not cv[:5].any():
            continue
        last = e[0]
        for ej, cj in zip(e, cv):
            if cj:
                last = ej
            elif last - ej > rad:
                break
        line[i] = last
    ok = ~np.isnan(line)
    if smooth and ok.sum() > smooth:
        L2 = line.copy()
        for i in range(len(line)):
            w = line[max(0, i - smooth // 2): i + smooth // 2 + 1]
            w = w[~np.isnan(w)]
            if len(w):
                L2[i] = np.median(w)
        line = L2
    pts = []
    kd = cKDTree(np.c_[phi, el])
    for b, e_ in zip(bins, line):
        pts.append(P[kd.query([b, e_])[1]] if not np.isnan(e_) else np.full(3, np.nan))
    return bins, line, np.array(pts), P, phi, el


def hairline_side_edge(head, ch, alpha_img, bins, line, el_min=-10.0, bin_deg=3.0, near_skin=0.015, pct=3.0, min_pts=10,
                       lead_deg=10.0, smooth=3):
    """2026-10-06 (hairline_cards sides): the hairline where it runs (nearly) vertically - temples, sideburns, around the
    ears - which hairline_curve cannot give (elevation per azimuth is NaN there). Per side (+1 = x>0, -1 = x<0) and
    elevation bin (centres el_min .. the front curve's end elevation, bin_deg apart): the most FORWARD azimuth |phi| of
    the visible card points near the skin (alpha > .35, within near_skin; the `pct` percentile of |phi| so single stray
    cards do not count), looking only at |phi| >= the front curve's last valid azimuth - lead_deg (what lies in front of
    that is the front hair). Bins with < min_pts points are interpolated (ends held); median-smoothed over `smooth` bins.
    -> {side: dict(el=bin centres (ascending), phi=edge |phi| per bin, phi_end=, el_end=)} (side missing when the front
    curve has no valid bin on that side)"""
    c = np.array(head.c, float)
    vis = visible_card_points(ch, alpha_img, n=200000, alpha_vis=0.35, edge_filter=False)
    vis = vis[np.abs(head.signed(vis)[0]) < near_skin]
    d = vis - c
    phi = np.degrees(np.arctan2(d[:, 0], d[:, 1]))
    el = np.degrees(np.arctan2(d[:, 2], np.hypot(d[:, 0], d[:, 1])))
    ok = ~np.isnan(line)
    out = {}
    for sgn in (1, -1):
        m = ok & (np.sign(bins) == sgn)
        if not m.any():
            continue
        k = np.argmax(np.abs(bins[m]))
        phi_end, el_end = float(abs(bins[m][k])), float(line[m][k])
        cen = np.arange(el_min, el_end + 1e-6, bin_deg)
        if len(cen) < 2:
            continue
        sel = (np.sign(d[:, 0]) == sgn) & (np.abs(phi) >= phi_end - lead_deg)
        ph, e = np.abs(phi[sel]), el[sel]
        edge = np.full(len(cen), np.nan)
        for i, ce in enumerate(cen):
            mm = np.abs(e - ce) < bin_deg / 2
            if mm.sum() >= min_pts:
                edge[i] = np.percentile(ph[mm], pct)
        g = ~np.isnan(edge)
        if g.sum() < 2:
            continue
        edge = np.interp(cen, cen[g], edge[g])
        if smooth and len(edge) > smooth:
            e2 = edge.copy()
            for i in range(len(edge)):
                e2[i] = np.median(edge[max(0, i - smooth // 2): i + smooth // 2 + 1])
            edge = e2
        out[sgn] = dict(el=cen, phi=edge, phi_end=phi_end, el_end=el_end)
    return out


def hairline_row(res, head, ch, alpha_img, phi_max=110.0, tol_deg=1.5, inset_deg=1.0, spacing=0.004, rows=2,
                 row_step_deg=1.5, log=print):
    """2026-10-05 (user: Lambert's hairline is not "straightish, well defined"): guides rooted in front of / below the
    vanilla hairline curve (more than tol_deg) are dropped; then `rows` rows of guides are rooted along the curve
    (inset_deg + row_step_deg * r above it, `spacing` apart), each a copy of the nearest guide rooted behind the line
    (shape unchanged, no blend), so the edge is one even line combed like the hair behind it. -> (res, dropped, added)"""
    bins, line, pts, P, phi_s, el_s = hairline_curve(head, ch, alpha_img, phi_max=phi_max)
    S = res['S']
    G = [np.asarray(s['p'], float) for s in S]
    R = np.array([g[0] for g in G])
    c = np.array(head.c, float)
    d = R - c
    rphi = np.degrees(np.arctan2(d[:, 0], d[:, 1]))
    rel = np.degrees(np.arctan2(d[:, 2], np.hypot(d[:, 0], d[:, 1])))
    ok = ~np.isnan(line)
    if ok.sum() < 2 or len(pts) == 0:
        # 2026-10-06 (Oriana split: 'array of sample points is empty'): after split_cards removes the roll cards the remaining
        # cards may give no usable hairline curve -> leave the guides alone instead of crashing
        log('  hairline_row: no usable vanilla hairline curve (%d valid bins), skipped' % int(ok.sum()))
        return res, [], []
    lin = np.interp(rphi, bins[ok], line[ok])
    zone = np.abs(rphi) < phi_max
    bad = zone & (rel < lin - tol_deg)
    keep = [i for i in range(len(S)) if not bad[i]]
    behind = np.array([i for i in keep if zone[i] and rel[i] >= lin[i] + inset_deg])
    kdb = cKDTree(R[behind]) if len(behind) else None
    kd_s = cKDTree(np.c_[phi_s, el_s])
    new = []
    for r in range(rows):
        # walk the curve in small azimuth steps and place a root every `spacing` metres along it
        fine = np.arange(bins[ok][0], bins[ok][-1], 0.25)
        fl = np.interp(fine, bins[ok], line[ok]) + inset_deg + row_step_deg * r
        Q = P[kd_s.query(np.c_[fine, fl])[1]]
        last = None
        for q in Q:
            if last is not None and np.linalg.norm(q - last) < spacing:
                continue
            last = q
            if kdb is None:
                break
            j = behind[kdb.query(q)[1]]
            g = G[j]
            sd, nrm, _ = head.signed(q[None, :])
            q2 = q - nrm[0] * sd[0]
            s_ = dict(S[j])
            s_['p'] = g + (q2 - g[0])[None, :]
            new.append(s_)
    r2 = dict(res)
    r2['S'] = [S[i] for i in keep] + new
    log('  hairline_row: %d guides in front of the vanilla hairline dropped, %d rooted along it (%d rows, %.0f mm apart)' % (
        int(bad.sum()), len(new), rows, spacing * 1000))
    return reindex(r2, list(range(len(r2['S'])))), int(bad.sum()), len(new)


def inflate(strands, head, k=1.2, add=0.003, zrel=0.16, ramp=0.3, log=print):
    """2026-10-06 (user: Lambert "a bit more volume, like 20 % more"): push strand points out along the scalp normal so
    their standoff becomes sd * k + add, ramped in over the first `ramp` of the arc length (root fixed), only above
    head.top - zrel (hanging parts below are left alone). -> new strands"""
    out, nm = [], 0
    for P in strands:
        P = np.asarray(P, float)
        seg = np.r_[0.0, np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))]
        t = seg / max(seg[-1], 1e-9)
        w = np.clip(t / max(ramp, 1e-6), 0, 1)
        sd, n, _ = head.signed(P)
        w = w * (P[:, 2] > head.top - zrel)
        d = (np.maximum(sd, 0) * (k - 1) + add) * w
        if d.any():
            nm += 1
        out.append(P + n * d[:, None])
    log('  inflate: %d guides pushed out (standoff x %.2f + %.0f mm)' % (nm, k, add * 1000))
    return out


def extend_across(strands, head, z_rel=0.07, target_x=0.05, side=1, max_add=0.05, hug=0.006, log=print):
    """2026-10-06 (user, Olgierd: top hairs rooted on one side stop mid-head; they should reach nearly the other side):
    guides rooted on the top (root z > head.top - z_rel) whose tip has not reached `side * target_x` (x relative to the
    head centre) are extended from the tip, continuing their last direction projected on the scalp tangent plane and
    kept `hug` off the scalp, until the tip reaches target_x or max_add metres are added. Then resampled to the same
    point count."""
    c = np.array(head.c, float)
    out, n = [], 0
    for P in strands:
        P = np.asarray(P, float)
        if P[0, 2] < head.top - z_rel or side * (P[-1, 0] - c[0]) >= target_x:
            out.append(P); continue
        d = P[-1] - P[-3] if len(P) > 2 else P[-1] - P[0]
        if side * d[0] <= 0:                      # not heading toward the target side: leave it
            out.append(P); continue
        step = 0.004
        Q = [p for p in P]
        added = 0.0
        while added < max_add and side * (Q[-1][0] - c[0]) < target_x:
            sd, nn, _ = head.signed(Q[-1][None])
            nrm = nn[0] / max(np.linalg.norm(nn[0]), 1e-9)
            t = d - nrm * float(d @ nrm)
            t /= max(np.linalg.norm(t), 1e-9)
            q = Q[-1] + t * step
            sq, nq, _ = head.signed(q[None])
            nq = nq[0] / max(np.linalg.norm(nq[0]), 1e-9)
            q = q + nq * (hug - float(sq[0])) * 0.5
            d = q - Q[-1]; Q.append(q); added += step
        out.append(resample(np.array(Q), len(P))[0]); n += 1
    log('  extend_across: %d top guides lengthened toward x %+.0f mm (max +%.0f mm)' % (n, side * target_x * 1000, max_add * 1000))
    return out


def lengthen_front(strands, head, y_min=0.0, z_min_rel=0.2, factor=1.5, hug=0.004, x_min=None, x_max=None, factor_other=None, log=print):
    """2026-10-06 (Ves: vanilla has a LONG bang swept over the right eye, the card guides give a short blunt fringe): guides
    rooted in the front (root y > c.y + y_min) and upper head (root z > top - z_min_rel) are lengthened by (factor - 1) x
    their length: the tip is continued along the mean direction of the last 3 segments, kept `hug` off the skin / head,
    then resampled to the same point count (so the whole strand is longer). Run BEFORE sweep_front."""
    c = np.array(head.c, float)
    out, n = [], 0
    for P in strands:
        P = np.asarray(P, float)
        if P[0, 1] < c[1] + y_min or P[0, 2] < head.top - z_min_rel:
            out.append(P); continue
        seg = np.linalg.norm(np.diff(P, axis=0), axis=1)
        L = seg.sum()
        rx = P[0, 0] - c[0]
        inside = (x_min is None or rx >= x_min) and (x_max is None or rx <= x_max)
        fac = factor if inside else (factor_other if factor_other is not None else 1.0)
        if fac < 1.0:                                   # the other half of the bang is shortened (resampled shorter)
            dd0 = np.r_[0, np.cumsum(seg)]
            t0 = np.linspace(0, dd0[-1] * fac, len(P))
            out.append(np.stack([np.interp(t0, dd0, P[:, i]) for i in range(3)], 1)); n += 1
            continue
        if fac <= 1.0:
            out.append(P); continue
        d = P[-1] - P[-4]; d /= max(np.linalg.norm(d), 1e-9)
        q = [P[-1]]; add = 0.0; step = 0.004
        while add < (fac - 1) * L:
            nxt = q[-1] + d * step
            sd, nn, _ = head.signed(nxt[None])
            if sd[0] < hug:
                nxt = nxt + nn[0] / max(np.linalg.norm(nn[0]), 1e-9) * (hug - sd[0])
            d = nxt - q[-1]; d /= max(np.linalg.norm(d), 1e-9)
            q.append(nxt); add += step
        Q = np.vstack([P, np.array(q[1:])])
        dd = np.r_[0, np.cumsum(np.linalg.norm(np.diff(Q, axis=0), axis=1))]
        t = np.linspace(0, dd[-1], len(P))
        out.append(np.stack([np.interp(t, dd, Q[:, i]) for i in range(3)], 1)); n += 1
    log('  lengthen_front: %d front guides lengthened x%.2f' % (n, factor))
    return out


def sweep_front(strands, head, y_min=0.01, tip_y_min=0.0, start=0.2, angle_deg=55.0, z_min_rel=0.14, centre=0.012,
                hug=0.005, hug_max=0.03, force=0, log=print):
    """2026-10-06 (Oriana: vanilla hair is parted in the middle and swept back over the ears with NO bangs; the card lanes
    of the front fall straight down over the forehead): each guide rooted in the front half (root y > c.y + y_min) and
    in the upper head (root z > top - z_min_rel) is rotated about the vertical head axis (through head.c), AWAY from the
    parting (toward +x if its root is at +x, toward -x otherwise; weight ramps up over `centre` m either side of the
    parting). The rotation grows from 0 at `start` of the arc length to `angle_deg` at the tip (smoothstep) and is scaled
    by how far in front the tip is (tip y > c.y + tip_y_min: full weight 3 cm in front). Lengths are kept (rotation);
    afterwards points closer than `hug` to the skin move out, points farther than `hug_max` move in (the standoff
    follows the skull, as droop_sides). Strands with a rear tip (hair that already goes back) are untouched."""
    c = np.array(head.c, float)
    out, n = [], 0
    for P in strands:
        P = np.asarray(P, float).copy()
        sx = P[0, 0] - c[0]
        if P[0, 1] < c[1] + y_min or P[0, 2] < head.top - z_min_rel or P[-1, 1] < c[1] + tip_y_min:
            out.append(P); continue
        wt = float(np.clip((P[-1, 1] - c[1] - tip_y_min) / 0.03, 0.0, 1.0))
        sgn = float(force) if force else float(np.clip(sx / centre, -1.0, 1.0))      # force +1 / -1: ALL front guides sweep to that side (Ves: one long bang)
        if abs(sgn) < 1e-3:
            out.append(P); continue
        seg = np.linalg.norm(np.diff(P, axis=0), axis=1)
        s = np.r_[0, np.cumsum(seg)] / max(seg.sum(), 1e-9)
        Q = P.copy()
        for i in range(len(P)):
            if s[i] <= start:
                continue
            u = np.clip((s[i] - start) / max(1 - start, 1e-6), 0, 1)
            th = np.radians(angle_deg) * wt * (u * u * (3 - 2 * u)) * abs(sgn) * (-sgn / max(abs(sgn), 1e-9))
            # front point (angle 90 deg from +x) moves toward +x for sgn > 0: clockwise seen from above (negative angle)
            d = P[i, :2] - c[:2]
            ca, sa = np.cos(th), np.sin(th)
            Q[i, 0] = c[0] + ca * d[0] - sa * d[1]
            Q[i, 1] = c[1] + sa * d[0] + ca * d[1]
            sd, nn, _ = head.signed(Q[i:i + 1])
            nn = nn[0] / max(np.linalg.norm(nn[0]), 1e-9)
            if sd[0] < hug:
                Q[i] += nn * (hug - sd[0])
            elif sd[0] > hug_max and u > 0:
                Q[i] -= nn * (sd[0] - hug_max) * 0.6 * u
        R = Q.copy()
        for i in range(1, len(P)):
            d = R[i] - R[i - 1]; L = np.linalg.norm(d)
            if L > 1e-9:
                R[i] = R[i - 1] + d / L * seg[i - 1]
        out.append(R); n += 1
    log('  sweep_front: %d front guides swept sideways away from the parting (up to %.0f deg)' % (n, angle_deg))
    return out


def cap_front(strands, head, y_min=0.05, z_max=0.06, shorten_to=0.0, log=print):
    """2026-10-06 (Oriana, L123 result: strands covered the black headband): a guide is cut at its first point that lies in
    front (y > c.y + y_min) AND below `z_max` (m above the head centre = the headband's lower edge); the strand ends just
    before it (resampled back to the same point count). Guides rooted below the band line are left alone."""
    c = np.array(head.c, float)
    out, n = [], 0
    for P in strands:
        P = np.asarray(P, float)
        if P[0, 2] < c[2] + z_max:
            out.append(P); continue
        bad = np.where((P[:, 1] > c[1] + y_min) & (P[:, 2] < c[2] + z_max))[0]
        bad = bad[bad > 0]
        if len(bad) == 0:
            out.append(P); continue
        k = int(bad[0])
        seg = np.linalg.norm(np.diff(P, axis=0), axis=1)
        keep = float(np.clip((seg[:k - 1].sum() + 0.5 * seg[k - 1]) / max(seg.sum(), 1e-9), 0.05, 1.0)) if k > 1 else 0.25
        L = seg.sum() * keep
        d = np.r_[0, np.cumsum(seg)]
        t = np.linspace(0, L, len(P))
        out.append(np.stack([np.interp(t, d, P[:, i]) for i in range(3)], 1)); n += 1
    log('  cap_front: %d guides cut where they reach below the headband line (z %.0f mm above the head centre, y > %.0f mm)' % (n, z_max * 1000, y_min * 1000))
    return out


def roll_ends(strands, head, start=0.55, radius=0.02, turns=1.2, shrink=0.55, low_below_top=0.0, back_y=None, side=-1.0,
              min_len=0.04, band=False, start_below_top=None, log=print):
    """2026-10-06 (user, Iris / Oriana: "make the ends curl so strongly that they roll on themselves like in non hairwork"):
    for guides whose tip is below head.top - low_below_top (and, with back_y, behind head.c.y + back_y), the part after
    `start` (fraction of arc length) is replaced by a SPIRAL of the same arc length: radius `radius` shrinking to
    radius * shrink over `turns` turns, in the plane spanned by the strand tangent at `start` and the horizontal outward
    direction (side -1 = rolls toward the head, +1 = flips outward). Point count and total length are kept.
    2026-10-09 (Iris L209, opt-in): band=True rolls every guide in the SAME plane (vertical, front-back: axis = head x),
    outward = straight back (-y), so neighbours roll together as one nape roll; start_below_top (m) starts the roll where
    the strand first drops below head.top - start_below_top (a level band) instead of at the arc fraction `start`."""
    c = np.array(head.c, float)
    out, n = [], 0
    for P in strands:
        P = np.asarray(P, float)
        K = len(P)
        if P[-1, 2] > head.top - low_below_top or (back_y is not None and P[-1, 1] > c[1] + back_y):
            out.append(P); continue
        seg = np.linalg.norm(np.diff(P, axis=0), axis=1)
        s = np.r_[0, np.cumsum(seg)]
        L = s[-1]
        if L < min_len:
            out.append(P); continue
        i0 = int(np.searchsorted(s, start * L))
        if start_below_top is not None:
            lo = np.nonzero(P[:, 2] < head.top - start_below_top)[0]
            i0 = max(i0, int(lo[0])) if len(lo) else K
        i0 = min(max(i0, 1), K - 2)
        t = P[i0] - P[i0 - 1]
        if band:
            t[0] = 0
        t /= max(np.linalg.norm(t), 1e-9)
        if band:
            rad = np.array([0.0, -1.0, 0.0])
        else:
            rad = P[i0] - c; rad[2] = 0; rad /= max(np.linalg.norm(rad), 1e-9)
        u = rad - t * float(rad @ t); un = np.linalg.norm(u)
        if un < 0.2:
            out.append(P); continue
        u /= un
        rem = L - s[i0]
        # spiral r(th) = R * (1 - (1 - shrink) * th / th_max), starts tangent to t, curls toward side * u
        thmax = 2 * np.pi * turns
        th = np.linspace(0, thmax, 400)
        r = radius * (1 - (1 - shrink) * th / thmax)
        # centre offset so that the curve starts at P[i0] heading along t: point = r*(sin th * t' + (1-cos th) * u')
        pt = np.stack([r * np.sin(th) * 1.0, r * (1 - np.cos(th))], 1)
        arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(pt, axis=0), axis=1))]
        k_ = np.linspace(0, min(rem, arc[-1]), K - i0)
        xs = np.interp(k_, arc, pt[:, 0]); ys = np.interp(k_, arc, pt[:, 1])
        Q = P.copy()
        for j in range(K - i0):
            Q[i0 + j] = P[i0] + t * xs[j] + side * u * ys[j]
        out.append(Q); n += 1
    log('  roll_ends: %d guide ends rolled into spirals (radius %.0f mm, %.1f turns, from %.0f %%)' % (n, radius * 1000, turns, start * 100))
    return out


def swirl(strands, head, cx=0.0, cy=-0.04, angle=35.0, radius=0.09, z_min_rel=0.14, hug=0.004, log=print):
    """2026-10-06 (Ves: vanilla back = one smooth crown whorl; ours = short strands pointing every way, scalp showing between):
    guides rooted within `radius` of the whorl centre (x = c.x + cx, y = c.y + cy, upper head z > top - z_min_rel) are rotated
    about the vertical axis through the root by `angle` degrees (weight fades to 0 at `radius`): radial strands become a
    spiral. Lengths kept, standoff re-fitted."""
    c = np.array(head.c, float)
    ctr = np.array([c[0] + cx, c[1] + cy])
    out, n = [], 0
    for P in strands:
        P = np.asarray(P, float)
        d = np.linalg.norm(P[0, :2] - ctr)
        if d > radius or P[0, 2] < head.top - z_min_rel:
            out.append(P); continue
        w = 1.0 - d / radius
        th = np.radians(angle) * w
        ca, sa = np.cos(th), np.sin(th)
        Q = P.copy()
        v = P[:, :2] - P[0, :2]
        Q[:, 0] = P[0, 0] + ca * v[:, 0] - sa * v[:, 1]
        Q[:, 1] = P[0, 1] + sa * v[:, 0] + ca * v[:, 1]
        sd, nn, _ = head.signed(Q)
        for i in range(1, len(Q)):
            if sd[i] < hug:
                Q[i] += nn[i] / max(np.linalg.norm(nn[i]), 1e-9) * (hug - sd[i])
        out.append(Q); n += 1
    log('  swirl: %d crown guides rotated %.0f deg about the whorl (radius %.0f mm)' % (n, angle, radius * 1000))
    return out


def bun_roll(strands, head, cy=-0.115, cz=-0.035, radius=0.028, x_half=0.07, turns=1.0, z_min_rel=0.2, keep_every=25,
             entry_deg=75.0, hug=0.005, log=print):
    """2026-10-06 (Iris: "hair swept back to the bun, ends rolled on themselves"): guides rooted on the upper head (root z >
    top - z_min_rel) are rebuilt as: a path from the root to the TOP of a horizontal roll tube (axis along x at y = c.y + cy,
    z = c.z + cz, radius `radius`, x = the root's x clipped to +-x_half), interpolated in cylindrical coordinates about the head
    axis (so it lies along the skull), then `turns` turns around the tube (outer back side first, then under, then up the head
    side). Every `keep_every`-th guide is left alone (thin loose strands at the temples). Point count unchanged."""
    c = np.array(head.c, float)
    out, n = [], 0
    for gi, P in enumerate(strands):
        P = np.asarray(P, float)
        K = len(P)
        if P[0, 2] < head.top - z_min_rel or (keep_every and gi % keep_every == 0):
            out.append(P); continue
        r0 = P[0] - c
        x1 = float(np.clip(r0[0], -x_half, x_half))
        th0 = np.radians(entry_deg)
        n_path = max(3, int(K * 0.55)); n_roll = K - n_path
        # entry on the tube
        ent = np.array([x1, cy - radius * np.cos(th0), cz + radius * np.sin(th0)])
        ph0, rho0 = np.arctan2(r0[0], -r0[1] + 1e-9), np.hypot(r0[0], r0[1])
        ph1, rho1 = np.arctan2(ent[0], -ent[1] + 1e-9), np.hypot(ent[0], ent[1])
        dph = (ph1 - ph0 + np.pi) % (2 * np.pi) - np.pi
        Q = np.zeros((K, 3))
        for i in range(n_path):
            u = i / (n_path - 1)
            e = u * u * (3 - 2 * u)
            ph, rho, z = ph0 + dph * e, rho0 + (rho1 - rho0) * e, r0[2] + (ent[2] - r0[2]) * e
            Q[i] = c + np.array([rho * np.sin(ph), -rho * np.cos(ph), z])
        for j in range(1, n_roll + 1):
            u = j / n_roll
            th = th0 - 2 * np.pi * turns * u
            rr = radius * (1 - 0.35 * u)
            Q[n_path - 1 + j if n_path - 1 + j < K else K - 1] = c + np.array([x1 + 0.004 * u, cy - rr * np.cos(th), cz + rr * np.sin(th)])
        Q[0] = P[0]
        sd, nn, _ = head.signed(Q)
        for i in range(1, n_path):
            if sd[i] < hug:
                Q[i] += nn[i] / max(np.linalg.norm(nn[i]), 1e-9) * (hug - sd[i])
        out.append(Q); n += 1
    log('  bun_roll: %d guides rebuilt as head path + %.1f turns around a roll (r %.0f mm at y %+.0f z %+.0f mm)' % (
        n, turns, radius * 1000, cy * 1000, cz * 1000))
    return out


def droop_sides(strands, head, side_x=0.045, drop=0.025, start=0.45, hug=0.004, only_side=None, log=print):
    """2026-10-06 (user, Olgierd: side hair ends stick up / out): for each guide whose tip lies out on the side of the
    head (|tip.x - c.x| > side_x; only_side +1 / -1 limits it to one side), bend the part after `start` (fraction of arc
    length) down by up to `drop` metres (smooth ramp to the tip, scaled by how far out to the side the tip is), then
    pull any point that ends up farther than `hug` above the scalp back toward it (never inside). Lengths kept."""
    c = np.array(head.c, float)
    out, n = [], 0
    for P in strands:
        P = np.asarray(P, float).copy()
        tx = P[-1, 0] - c[0]
        if abs(tx) <= side_x or (only_side and np.sign(tx) != only_side):
            out.append(P); continue
        seg = np.linalg.norm(np.diff(P, axis=0), axis=1)
        s = np.r_[0, np.cumsum(seg)] / max(seg.sum(), 1e-9)
        k = np.clip((abs(tx) - side_x) / max(side_x, 1e-6), 0.0, 1.0) ** 0.5
        Q = P.copy()
        for i in range(len(P)):
            if s[i] <= start:
                continue
            w = ((s[i] - start) / max(1 - start, 1e-6)) ** 1.5
            Q[i, 2] -= drop * k * w
            sd, nn, _ = head.signed(Q[i:i + 1])
            if sd[0] > hug:
                Q[i] -= nn[0] / max(np.linalg.norm(nn[0]), 1e-9) * (sd[0] - hug) * w * 0.7
        # keep the original segment lengths
        R = Q.copy()
        for i in range(1, len(P)):
            d = R[i] - R[i - 1]; L = np.linalg.norm(d)
            if L > 1e-9:
                R[i] = R[i - 1] + d / L * seg[i - 1]
        out.append(R); n += 1
    log('  droop_sides: %d side guides bent down (drop %.0f mm from %.0f %% of length)' % (n, drop * 1000, start * 100))
    return out


def curl_strands(strands, radius=0.008, pitch=0.03, start=0.35, ramp=0.2, seed=3, z_max=None, z_fade=0.03, low_z=None,
                 radius2=None, pitch2=None, low_fade=0.04, back_y=None, coherent=None, log=print):
    """2026-10-06 (Guillaume's vanilla ringlets): wind each guide into a helix around its own (smoothed) axis from
    `start` (fraction of arc length) on: radius ramps in over `ramp`, one turn per `pitch` metres of arc, a random
    phase and handedness per guide. Point count unchanged; root part untouched. HairWorks clumps hairs to the guides,
    so the clump follows the helix -> ringlets."""
    rng = np.random.default_rng(seed)
    out = []
    for P in strands:
        P = np.asarray(P, float)
        K = len(P)
        seg = np.linalg.norm(np.diff(P, axis=0), axis=1)
        s = np.r_[0, np.cumsum(seg)]
        L = max(s[-1], 1e-6)
        A = P.copy()
        for _ in range(2):                                  # smoothed axis
            A[1:-1] = (A[:-2] + A[1:-1] + A[2:]) / 3
        T = np.gradient(A, axis=0)
        T /= np.maximum(np.linalg.norm(T, axis=1, keepdims=True), 1e-9)
        ref = np.array([0.0, 0.0, 1.0])
        ph0, hand = rng.uniform(0, 2 * np.pi), (1 if rng.random() < 0.5 else -1)
        if coherent is not None:
            # 2026-10-06 (L148, Guillaume: vanilla ringlets are tight, regular and ALIGNED; random phase / handedness per guide
            # = frizz): one handedness, phase = a smooth function of the root's x (coherent = turns per metre across x), so
            # neighbouring guides wind in step and the clumps read as regular coils
            hand = 1
            ph0 = float(P[0, 0]) * 2 * np.pi * coherent
        ph_state = [None, 0.0]
        Q = P.copy()
        for k in range(K):
            f = s[k] / L
            if f <= start:
                continue
            w = min((f - start) / max(ramp, 1e-6), 1.0)
            if z_max is not None:                           # only the hanging part (no curl on the top / crown)
                w *= float(np.clip((z_max - A[k][2]) / max(z_fade, 1e-6), 0.0, 1.0))
            if back_y is not None and radius2 is None:
                w *= float(np.clip((back_y - A[k][1]) / max(low_fade, 1e-6), 0.0, 1.0))
            r_ = ref if abs(T[k] @ ref) < 0.9 else np.array([1.0, 0.0, 0.0])
            u = np.cross(T[k], r_); u /= max(np.linalg.norm(u), 1e-9)
            v = np.cross(T[k], u)
            # 2026-10-06 (Margarita): below `low_z` (absolute z, fade `low_fade`) and behind back_y the ringlets are tighter
            # (radius2 / pitch2); the phase is integrated so the helix stays continuous
            wl = 0.0
            if low_z is not None and radius2 is not None:
                wl = float(np.clip((low_z - A[k][2]) / max(low_fade, 1e-6), 0.0, 1.0))
                if back_y is not None:
                    wl *= float(np.clip((back_y - A[k][1]) / max(low_fade, 1e-6), 0.0, 1.0))
            r_k = radius + ((radius2 - radius) * wl if radius2 is not None else 0.0)
            p_k = pitch + ((pitch2 - pitch) * wl if pitch2 is not None else 0.0)
            if k == 0 or f <= start or ph_state[0] is None:
                ph_state[0] = ph0
                ph_state[1] = s[k]
            else:
                ph_state[0] += hand * 2 * np.pi * (s[k] - ph_state[1]) / p_k
                ph_state[1] = s[k]
            a = ph_state[0]
            Q[k] = A[k] + (P[k] - A[k]) + r_k * w * (np.cos(a) * u + np.sin(a) * v)
        out.append(Q)
    log('  curl: %d guides wound into helices (radius %.0f mm, pitch %.0f mm, from %.0f %% of length)' % (
        len(out), radius * 1000, pitch * 1000, start * 100))
    return out


def sink_roots(strands, head, depth=0.004, log=print):
    """2026-10-06 (user: a better deeper-root alternative; prepend + resample pushed the root wave under the skin and
    made strands leave the forehead perpendicular): only the ROOT POINT moves under the skin (along -normal by depth);
    every other point, the strand length / point count / wave parametrisation stay. The first segment then dives into
    the skin at a shallow angle (atan(depth / segment length)) and the visible start moves a little back along the
    strand."""
    out = []
    for P in strands:
        P = np.asarray(P, float).copy()
        _sd, n, _ = head.signed(P[:1])
        n0 = n[0] / max(np.linalg.norm(n[0]), 1e-9)
        P[0] = P[0] - n0 * (depth + max(float(_sd[0]), 0.0))
        out.append(P)
    log('  sink_roots: %d guide roots moved %.1f mm under the skin (rest of each strand unchanged)' % (len(out), depth * 1000))
    return out


def bury_roots(strands, head, frac=0.07, max_depth=0.006, min_depth=0.001, angle_deg=None, log=print):
    """2026-10-06 (user: "hairs start a bit under the skin, like the first 5-10 %"): prepend a straight segment from
    root - n * depth (n = scalp normal at the root, depth = frac * strand length clipped to min_depth..max_depth) to the
    root, then resample to the original point count (arc-length uniform). The visible part keeps its shape; the root
    (thinnest, harshest part) sits under the skin. -> new strands"""
    out, dd = [], []
    for P in strands:
        P = np.asarray(P, float)
        L = float(np.linalg.norm(np.diff(P, axis=0), axis=1).sum())
        d = float(np.clip(frac * L, min_depth, max_depth))
        _sd, n, _ = head.signed(P[:1])
        n0 = n[0] / max(np.linalg.norm(n[0]), 1e-9)
        q0 = P[0] - n0 * d
        if angle_deg:
            # 2026-10-06 (user: deeper burying showed MORE roots, "it should also move them a bit backward"): the
            # straight-down segment made every strand leave the skin perpendicular. Bury along the strand's own
            # direction instead: the hidden part runs under the skin from in front of the root, depth d, entering at
            # angle_deg to the scalp, so the strand emerges at a grazing angle and its visible start moves back.
            t = P[min(2, len(P) - 1)] - P[0]
            t = t - n0 * float(t @ n0)
            tn = float(np.linalg.norm(t))
            if tn > 1e-6:
                # L87 (user: "you sure you moved backward, not more forward?"): a straight chord forward leaves the
                # curved forehead and re-surfaced 2-3 cm in front of the line. The hidden path now follows the scalp:
                # points stepped forward along -t, each snapped onto the head and pushed under it, the depth growing
                # from 0 at the root to d at the far end.
                a = np.radians(angle_deg)
                run = d / np.tan(a)
                hid = []
                for f in (1.0, 2 / 3, 1 / 3):
                    p_ = P[0] - (t / tn) * run * f
                    sd_, nn_, _ = head.signed(p_[None])
                    nn_ = nn_[0] / max(np.linalg.norm(nn_[0]), 1e-9)
                    hid.append(p_ - nn_ * (float(sd_[0]) + d * f))
                Q = np.vstack(hid + [P])
                out.append(resample(Q, len(P))[0])
                dd.append(d)
                continue
        Q = np.vstack([q0, P])
        out.append(resample(Q, len(P))[0])
        dd.append(d)
    log('  bury_roots: %d guides start under the skin (depth %.1f-%.1f mm, mean %.1f mm)' % (
        len(out), min(dd) * 1000, max(dd) * 1000, float(np.mean(dd)) * 1000))
    return out


def smooth_strands(strands, iters=6, alpha=0.5, keep=2, log=print):
    """2026-10-05 (user: Shani's hairs "ondulate in too angled ways, unnatural"): guide polylines had joint turns of
    63 deg at p90 (approved Vesemir / Regis ~35). Laplacian smoothing of points keep..K-2 (root and first segment fixed,
    tip follows), then every strand is rescaled per segment back to its original segment lengths so the length and
    root direction are kept. -> (strands, p90 joint turn before, after)"""
    def turns(P):
        D = np.diff(P, axis=1); D /= np.maximum(np.linalg.norm(D, axis=2, keepdims=True), 1e-9)
        return np.degrees(np.arccos(np.clip((D[:, 1:] * D[:, :-1]).sum(2), -1, 1)))
    G = np.array([np.asarray(p, float) for p in strands])
    t0 = np.percentile(turns(G), 90)
    seg = np.linalg.norm(np.diff(G, axis=1), axis=2)
    S = G.copy()
    for _ in range(int(iters)):
        L = S.copy()
        L[:, keep:-1] = (1 - alpha) * S[:, keep:-1] + alpha * 0.5 * (S[:, keep - 1:-2] + S[:, keep + 1:])
        L[:, -1] = S[:, -1] + alpha * 0.5 * (S[:, -2] - S[:, -1]) * 0  # tip: carried by the re-lengthening below
        S = L
    out = np.empty_like(S)
    out[:, 0] = G[:, 0]
    for j in range(1, S.shape[1]):
        d = S[:, j] - S[:, j - 1]
        d /= np.maximum(np.linalg.norm(d, axis=1, keepdims=True), 1e-9)
        out[:, j] = out[:, j - 1] + d * seg[:, j - 1:j]
    t1 = np.percentile(turns(out), 90)
    log('  smooth_strands: joint turn p90 %.1f -> %.1f deg (%d iters, alpha %.2f)' % (t0, t1, iters, alpha))
    return list(out), t0, t1


def add_guide_copies(res, head, entries, tol=0.004, log=print):
    """entries: [{src: [x, y, z] root of the source guide, d: [dx, dy, dz]}] -> copies of the source guides shifted by d,
    root snapped onto the scalp, copies touching the skin skipped. Re-triangulated. -> (res, number added)"""
    S = res['S']
    R = np.array([np.asarray(s['p'], float)[0] for s in S])
    kd = cKDTree(R)
    new, miss, skin = [], 0, 0
    for e in entries:
        dd, i = kd.query(np.asarray(e['src'], float))
        if dd > tol:
            miss += 1
            continue
        p = np.asarray(S[i]['p'], float)
        r = p[0] + np.asarray(e['d'], float)
        sd, nrm, _ = head.signed(r[None, :])
        r = r - nrm[0] * sd[0]
        q = p + (r - p[0])[None, :]
        if (head.signed(q[2:])[0] < 0.002).any():
            skin += 1
            continue
        s_ = dict(S[i])
        s_['p'] = q
        new.append(s_)
    log('  extra_guides: %d copies added (%d sources not found, %d on the skin)' % (len(new), miss, skin))
    if not new:
        return res, 0
    r2 = dict(res)
    r2['S'] = list(S) + new
    return reindex(r2, list(range(len(r2['S'])))), len(new)


def hairline_follow(res, head, k=5, zone_x=0.035, log=print):
    """2026-10-05 (Lambert: spiky temple hairline): growth-mesh BOUNDARY guides on the front / temples take the
    root-relative shape of their k nearest interior guides (the backward sweep) instead of their own (down / forward).
    -> number changed"""
    S, F = res['S'], np.asarray(res['F'], int)
    from collections import Counter
    ec = Counter()
    for a, b, c_ in F:
        for e in ((a, b), (b, c_), (a, c_)):
            ec[tuple(sorted(e))] += 1
    bnd = set()
    for (a, b), n in ec.items():
        if n == 1:
            bnd.update((a, b))
    G = np.array([np.asarray(s['p'], float) for s in S])
    R = G[:, 0]
    interior = np.array([i for i in range(len(S)) if i not in bnd])
    if not len(interior):
        return 0
    kd = cKDTree(R[interior])
    rel = G - R[:, None, :]
    n = 0
    for i in bnd:
        if not (R[i][1] > head.c[1] - 0.01 and (abs(R[i][0]) > zone_x or R[i][1] > head.c[1] + 0.03)):
            continue
        d, j = kd.query(R[i], k=min(k, len(interior)))
        j = interior[np.atleast_1d(j)]
        w = 1.0 / np.maximum(np.atleast_1d(d), 1e-4)
        S[i]['p'] = R[i][None, :] + (rel[j] * w[:, None, None]).sum(0) / w.sum()
        n += 1
    log('  hairline_follow: %d hairline guides take the interior sweep' % n)
    return n


def face_filter(res, head, ch, alpha_img, cfg, log=print):
    """2026-10-06 (user, Shani: fans of rendered hair with no guide in them on the forehead): HairWorks renders
    interpolated hairs inside every growth face, so a long thin face spanning distant guides (after hairline_row removed
    the guides in front of the hairline) fills a fan. Drop faces (opt-in, growth.face_filter), guides stay (like
    split_divergent; HairWorks does not need every guide in a face):
      max_edge   m, drop faces with any edge longer than this
      max_ratio  longest edge / shortest altitude, drops thin slivers
      skin_front drop faces whose centroid lies in front of / below the vanilla hairline curve (hairline_curve, same
                 visible-card coverage), i.e. over bare skin; skin_margin_deg shifts the line (+ = drop more)
    -> res with F filtered"""
    F = np.asarray(res['F'], int)
    R = np.asarray(res['roots'], float)
    a, b, c = R[F[:, 0]], R[F[:, 1]], R[F[:, 2]]
    e = np.stack([np.linalg.norm(b - c, axis=1), np.linalg.norm(a - c, axis=1), np.linalg.norm(a - b, axis=1)], 1)
    emax = e.max(1)
    area = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
    alt = 2 * area / np.maximum(emax, 1e-9)           # shortest altitude = onto the longest edge
    why = np.zeros(len(F), int)
    if cfg.get('max_edge'):
        why[(emax > cfg['max_edge']) & (why == 0)] = 1
    if cfg.get('max_ratio'):
        why[(emax / np.maximum(alt, 1e-9) > cfg['max_ratio']) & (why == 0)] = 2
    if cfg.get('len_ratio') or cfg.get('tip_edge') or cfg.get('mid_edge'):
        # 2026-10-06 (Ves / Iris L123: feathery clumps, flat shards): faces whose 3 guides are different LAYERS (length ratio
        # > len_ratio) or whose tips / middles are far apart (> tip_edge / mid_edge m) blend into sheets -> dropped
        Gs = np.array([np.asarray(s_['p'], float) for s_ in res['S']])
        Lg = np.linalg.norm(np.diff(Gs, axis=1), axis=2).sum(1)
        lf = Lg[F]
        if cfg.get('len_ratio'):
            why[(lf.max(1) / np.maximum(lf.min(1), 1e-6) > cfg['len_ratio']) & (why == 0)] = 4
        for key, idx_ in (('tip_edge', -1), ('mid_edge', Gs.shape[1] // 2)):
            if cfg.get(key):
                T = Gs[:, idx_]
                ta, tb, tc = T[F[:, 0]], T[F[:, 1]], T[F[:, 2]]
                te = np.max([np.linalg.norm(tb - tc, axis=1), np.linalg.norm(ta - tc, axis=1), np.linalg.norm(ta - tb, axis=1)], 0)
                why[(te > cfg[key]) & (why == 0)] = 5
    if cfg.get('skin_front'):
        bins, line, _p, _P, _ph, _el = hairline_curve(head, ch, alpha_img, phi_max=cfg.get('phi_max', 110.0))
        ok = ~np.isnan(line)
        if ok.sum() >= 2:
            d = (a + b + c) / 3 - np.array(head.c, float)
            ph = np.degrees(np.arctan2(d[:, 0], d[:, 1]))
            el = np.degrees(np.arctan2(d[:, 2], np.hypot(d[:, 0], d[:, 1])))
            sd = el - np.interp(ph, bins[ok], line[ok])
            front = (np.abs(ph) <= bins[ok].max()) & (sd < cfg.get('skin_margin_deg', 0.0)) & (np.abs(ph) < cfg.get('phi_max', 110.0))
            why[front & (why == 0)] = 3
        else:
            log('  face_filter: no vanilla hairline curve, skin_front skipped')
    keep = why == 0
    log('  face_filter: %d of %d growth faces dropped (max_edge %d, max_ratio %d, skin_front %d, len_ratio %d, tip / mid edge %d)' %
        (int((~keep).sum()), len(F), int((why == 1).sum()), int((why == 2).sum()), int((why == 3).sum()), int((why == 4).sum()), int((why == 5).sum())))
    out = dict(res)
    out['F'] = F[keep]
    out['part_verts'] = res.get('part_verts', [])
    return out


def reindex(res, keep):
    """keep a subset of guides (indices into res['S']) and rebuild the growth mesh over them."""
    S = [res['S'][i] for i in keep]
    R = np.array([s['p'][0] for s in S])
    c = res.get('centre')
    F, used = faces_for(R, c, res.get('edge', 0.034), res.get('rmax', 0.022), res.get('min_area', 1e-5))
    S = [S[i] for i in used]
    R = R[used]
    D = np.array([(s['p'][1] - s['p'][0]) / max(np.linalg.norm(s['p'][1] - s['p'][0]), 1e-9) for s in S])
    out = dict(res)
    out.update(S=S, F=F, roots=R, dirs=D, part_verts=[])   # re-triangulated: the part_tail cut and its vertex list are lost
    return out


# ------------------------------------------------------------------------------------------ R78 geometric checks
def card_samples(pos, tri, n=3):
    """points on every card triangle (vertices + an n x n barycentric grid): the vanilla hair volume."""
    a, b, c = pos[tri[:, 0]], pos[tri[:, 1]], pos[tri[:, 2]]
    out = [pos[np.unique(tri)]]
    for i in range(n + 1):
        for j in range(n + 1 - i):
            if (i, j) in ((0, 0), (n, 0), (0, n)):
                continue
            out.append(a + (b - a) * (i / n) + (c - a) * (j / n))
    return np.concatenate(out)


def cut_strand(p, k):
    """strand cut after point k, resampled back to len(p) points."""
    p = np.asarray(p, float)
    K = len(p)
    q = p[:max(k, 1) + 1]
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(q, axis=0), axis=1))]
    if d[-1] < 1e-6:
        return p
    t = np.linspace(0, d[-1], K)
    return np.stack([np.interp(t, d, q[:, c]) for c in range(3)], 1)


def crown_volume(strands, head, ch, alpha_img, zrel=0.09, k=0.8, max_move=0.015, cone=0.12, ramp=0.4, fade=0.0, front_y=None,
                 carry=0.0, smooth=0, both=False, min_sd=0.002):
    """carry / smooth (2026-10-04, L16 lesson: frizzy sides + a shelf across the back): the push is computed per point,
    so a strand leaving the push zone kinked back in (the shelf) and neighbouring points got uneven pushes (zigzag).
    carry: the offset reached keeps carrying down the strand (x carry per point) instead of dropping to 0;
    smooth: moving-average passes of the offset along the strand."""
    if carry or smooth or both:
        return _crown_volume_smooth(strands, head, ch, alpha_img, zrel, k, max_move, cone, ramp, fade, front_y, carry, smooth,
                                    both, min_sd)
    return _crown_volume(strands, head, ch, alpha_img, zrel, k, max_move, cone, ramp, fade, front_y)


def _crown_volume_smooth(strands, head, ch, alpha_img, zrel, k, max_move, cone, ramp, fade, front_y, carry, smooth,
                         both=False, min_sd=0.002):
    # both (2026-10-04, Lambert: bulges over the ears + front strands floating): also PULL points that stand farther
    # off the scalp than the cards there, never below min_sd
    vis = visible_card_points(ch, alpha_img, n=60000, alpha_vis=0.35, edge_filter=False)
    c = np.array(head.c, float)
    vis = vis[vis[:, 2] > head.top - zrel - 0.03]
    dv = vis - c
    uv = dv / np.linalg.norm(dv, axis=1, keepdims=True)
    sdv = head.signed(vis)[0]
    kd = cKDTree(uv)
    chord = 2 * np.sin(cone / 2)
    out, n = [], 0
    for p in strands:
        p = np.asarray(p, float).copy()
        K = len(p)
        if front_y is not None and p[0][1] > c[1] + front_y:
            out.append(p)
            continue
        sd, nrm, _ = head.signed(p)
        D = np.zeros(K)
        for j in range(1, K):
            if p[j][2] < head.top - zrel:
                continue
            wz = 1.0 if fade <= 0 else min(1.0, (p[j][2] - (head.top - zrel)) / fade)
            u = (p[j] - c) / max(np.linalg.norm(p[j] - c), 1e-9)
            idx = kd.query_ball_point(u, chord)
            if len(idx) < 8:
                continue
            lo = max(-max_move, min_sd - sd[j]) if both else 0.0
            D[j] = np.clip(k * (float(np.median(sdv[idx])) - sd[j]), min(lo, 0.0), max_move) * min(1.0, (j / (K - 1)) / max(ramp, 1e-3)) * wz
        if carry:
            for j in range(2, K):
                D[j] = (max(D[j], D[j - 1] * carry) if D[j] >= 0 else min(D[j], D[j - 1] * carry)) if D[j - 1] * D[j] >= 0 else D[j]
        for _ in range(int(smooth)):
            E = D.copy()
            E[1:-1] = 0.25 * D[:-2] + 0.5 * D[1:-1] + 0.25 * D[2:]
            E[-1] = 0.5 * (D[-1] + D[-2])
            E[0] = 0.0
            D = E
        if np.abs(D).max() > 1e-4:
            n += 1
            # one smooth direction per strand point: the head normal, averaged along the strand (no zigzag)
            N = nrm.copy()
            N[1:-1] = (nrm[:-2] + 2 * nrm[1:-1] + nrm[2:]) / 4.0
            N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
            p = p + N * D[:, None]
        out.append(p)
    return out, n


def _crown_volume(strands, head, ch, alpha_img, zrel=0.09, k=0.8, max_move=0.015, cone=0.12, ramp=0.4, fade=0.0, front_y=None):
    """2026-10-04: give the upper head the cards' volume. For every strand point above top - zrel: the visible card
    points seen from the crown centre within `cone` (radians) of the point's direction give a target standoff (their
    median signed distance to the head); the point moves out along the head normal by k * (target - own standoff),
    clipped to [0, max_move], ramped from 0 at the root to 1 at 40 % of the strand. -> (strands, guides moved)."""
    vis = visible_card_points(ch, alpha_img, n=60000, alpha_vis=0.35, edge_filter=False)
    c = np.array(head.c, float)
    vis = vis[vis[:, 2] > head.top - zrel - 0.03]
    dv = vis - c
    uv = dv / np.linalg.norm(dv, axis=1, keepdims=True)
    sdv = head.signed(vis)[0]
    kd = cKDTree(uv)
    chord = 2 * np.sin(cone / 2)
    out, n = [], 0
    for p in strands:
        p = np.asarray(p, float).copy()
        K = len(p)
        sd, nrm, _ = head.signed(p)
        moved = False
        for j in range(1, K):
            if p[j][2] < head.top - zrel:
                continue
            # fade (2026-10-04): weight 0 at top - zrel rising to 1 over `fade` metres: no step at the lower edge
            wz = 1.0 if fade <= 0 else min(1.0, (p[j][2] - (head.top - zrel)) / fade)
            if front_y is not None and p[0][1] > c[1] + front_y:      # 2026-10-04: leave the fringe / face side alone
                continue
            u = (p[j] - c) / max(np.linalg.norm(p[j] - c), 1e-9)
            idx = kd.query_ball_point(u, chord)
            if len(idx) < 8:
                continue
            target = float(np.median(sdv[idx]))
            d = np.clip(k * (target - sd[j]), 0.0, max_move) * min(1.0, (j / (K - 1)) / max(ramp, 1e-3)) * wz
            if d > 1e-4:
                p[j] = p[j] + nrm[j] * d
                moved = True
        n += moved
        out.append(p)
    return out, n


def card_envelope_trim(strands, ch, dist=0.015, alpha_img=None, alpha_vis=0.35):
    """R78: cut every strand where it leaves the vanilla card volume by more than `dist` (the cards ARE the hairstyle:
    Eskel's back strands ran 6-15 cm below the cards into the collar and flared out, Avallach's ends stuck out
    backwards past the hood). Strands are shortened, never removed. -> (new strands, number cut).
    2026-10-04 (HairLab L08): with `alpha_img` the envelope is the VISIBLE card surface (texture alpha > alpha_vis),
    not the raw card geometry: Shani's fringe cards run over the eye but are transparent there, so the raw envelope
    kept our fringe strands hanging over the eye in every round."""
    tri = ch['idx'].reshape(-1, 3)
    if alpha_img is not None:
        kd = cKDTree(visible_card_points(ch, alpha_img, n=60000, alpha_vis=alpha_vis, edge_filter=False))
    else:
        kd = cKDTree(card_samples(ch['pos'], tri))
    out, n = [], 0
    for p in strands:
        p = np.asarray(p, float)
        d = kd.query(p)[0]
        bad = np.where(d[2:] > dist)[0]
        if len(bad):
            out.append(cut_strand(p, bad[0] + 1)); n += 1
        else:
            out.append(p)
    return out, n


FACE_ROOT_ZEYE = None    # L140: growth.face_root_zeye (m below the head top); None = FACE_ROOT_DX applies everywhere
FACE_ROOT_DX = 0.07      # 2026-10-06 (L138): config growth.face_root_dx overrides it (set per build in hairpipe.generate)


def face_roots(roots, head, dz=0.085, dx=None, dy=0.01):
    """R78: roots lying on the FACE (forehead below the hairline / brow / eye level): in front of the crown centre
    (y > c + dy), lower than top - dz, |x| < dx. Eskel had ~20 guides rooted at eye height that ran across his face to
    the other temple (a fringe card crossing the forehead, cut into its own lane and rooted where it touches the
    skin). -> bool mask."""
    R = np.asarray(roots, float)
    if dx is None and FACE_ROOT_ZEYE is not None:
        # 2026-10-06 (L140, Eredin): the reduced FACE_ROOT_DX only counts above eye level (z > top - FACE_ROOT_ZEYE: the
        # forehead corner / upper temple); below it the old 70 mm rule keeps dropping the roots (no wall over the cheekbone)
        dxs = np.where(R[:, 2] > head.top - FACE_ROOT_ZEYE, FACE_ROOT_DX, 0.07)
        return (R[:, 1] > head.c[1] + dy) & (R[:, 2] < head.top - dz) & (np.abs(R[:, 0]) < dxs)
    dx = FACE_ROOT_DX if dx is None else dx
    return (R[:, 1] > head.c[1] + dy) & (R[:, 2] < head.top - dz) & (np.abs(R[:, 0]) < dx)


def ear_points(head, ear_x=0.083, z_lo=0.21, z_hi=0.05):
    """head vertices of the ears: |x| > ear_x between top - z_lo and top - z_hi (the skull side is ~7.5 cm, ears
    stick out to ~9.5 cm on every sampled head)."""
    V = head.v
    return V[(np.abs(V[:, 0]) > ear_x) & (V[:, 2] > head.top - z_lo) & (V[:, 2] < head.top - z_hi)]


def under_layer(res, head, front_y=0.0, zrel=0.12, squash=0.35, min_sd=0.002, min_gap=0.003, max_new=400, log=print):
    """2026-10-05 (user, Lambert: "the under hairs part should be fuller, there is still a space between the hair and
    the scalp"): for guides rooted on the upper head (z > top - zrel, in front of the centre + front_y) a copy is added
    whose points are pulled toward the scalp - signed distance x squash, never below min_sd - so a lower layer fills the
    space under the lifted top layer. Copies are rooted next to the source (offset min_gap along the head, so faces_for
    keeps them). -> (res, added)"""
    S = res['S']
    new = []
    for s_ in S:
        if len(new) >= max_new:
            break
        p = np.asarray(s_['p'], float)
        r = p[0]
        if r[2] < head.top - zrel or r[1] < head.c[1] + front_y:
            continue
        sd, nrm, _ = head.signed(p)
        tgt = np.maximum(sd * squash, min_sd)
        q = p - nrm * (sd - tgt)[:, None]
        # shift the copy sideways so its root is not on top of the source root
        side = np.cross(nrm[0], np.array([0.0, 0.0, 1.0]))
        nside = np.linalg.norm(side)
        if nside > 1e-6:
            q = q + side / nside * min_gap
        s2 = dict(s_)
        s2['p'] = q
        new.append(s2)
    r2 = dict(res)
    r2['S'] = list(S) + new
    log('  under_layer: %d flattened copies under the top layer (squash %.2f, zrel %.2f)' % (len(new), squash, zrel))
    return reindex(r2, list(range(len(r2['S'])))), len(new)


def ear_zone(res, head, front_cut=0.02, front_reach=0.045, behind_drop=0.03, behind_reach=0.035, ear_x=0.083, log=print):
    """2026-10-05 (user: Lambert's left ear - long dark sideburn in front of it, stray strands hanging behind it; trims
    by distance to the ear never hit them because they pass beside the ear, not over it). Per side, from the ear
    vertices: ear top z, front y, back y.
      sideburn: a strand rooted in front of the ear (root y in [front, front + front_reach], same side) is cut where it
                goes below ear top - front_cut;
      behind:   a guide rooted behind the ear (root y in [back - behind_reach, back]) lower than ear top - behind_drop
                is dropped (vanilla's back hair covers that area as one mass; ours were sparse hanging strands).
    -> (res, cut, dropped)"""
    E = ear_points(head, ear_x)
    if not len(E):
        return res, 0, 0
    S = res['S']
    cut, drop = 0, []
    sides = {}
    for sgn in (-1, 1):
        Es = E[np.sign(E[:, 0]) == sgn]
        if len(Es):
            sides[sgn] = (Es[:, 2].max(), Es[:, 1].max(), Es[:, 1].min())
    for i, s_ in enumerate(S):
        p = np.asarray(s_['p'], float)
        r = p[0]
        sg = 1 if r[0] > 0 else -1
        if sg not in sides or abs(r[0]) < 0.05:
            continue
        ztop, yf, yb = sides[sg]
        if yf <= r[1] <= yf + front_reach:
            low = np.where(p[2:, 2] < ztop - front_cut)[0]
            if len(low):
                k = low[0] + 2
                q = np.asarray(p[:max(k, 2)], float)
                t = np.linspace(0, 1, len(p))
                tq = np.linspace(0, 1, len(q))
                s_['p'] = np.stack([np.interp(t, tq, q[:, d]) for d in range(3)], 1)
                cut += 1
        elif yb - behind_reach <= r[1] <= yb and r[2] < ztop - behind_drop:
            drop.append(i)
    log('  ear_zone: %d sideburn strands cut %.0f mm below the ear top, %d low guides behind the ear dropped' % (
        cut, front_cut * 1000, len(drop)))
    if drop:
        res = reindex(res, [i for i in range(len(S)) if i not in set(drop)])
    return res, cut, len(drop)


def ear_trim(strands, head, dist=0.008, ear_x=0.083):
    """R78 (Lambert: 34-45 temple strands swept back and then down OVER the ear; vanilla shows the whole ear): every
    strand is cut just before its first point within `dist` of an ear vertex (shortened, never removed; strands
    rooted on the ear itself are left alone). -> (new strands, number cut)."""
    E = ear_points(head, ear_x)
    if not len(E):
        return list(strands), 0
    kd = cKDTree(E)
    out, n = [], 0
    for p in strands:
        p = np.asarray(p, float)
        d = kd.query(p)[0]
        if d[0] < dist:
            out.append(p); continue
        bad = np.where(d[2:] < dist)[0]
        if len(bad):
            out.append(cut_strand(p, bad[0] + 1)); n += 1
        else:
            out.append(p)
    return out, n


def ear_rooted(roots, head, dist=0.008, ear_x=0.083):
    """R78: roots ON the ear (within `dist` of an ear vertex): no hair grows there; their guides hang over the ear."""
    E = ear_points(head, ear_x)
    if not len(E):
        return np.zeros(len(roots), bool)
    return cKDTree(E).query(np.asarray(roots, float))[0] < dist


def interp_push(strands, F, head, margin=0.0015, n=4, iters=8, max_move=0.012, log=print):
    """R81 (user: lots of hair clipping through the head): HairWorks collides only the GUIDES; every rendered hair is
    the barycentric blend of the 3 guides of its growth face, so where neighbouring guides diverge the blend cuts under
    the scalp although all 3 guides are outside (R80: guides 0.1-3 % of points inside, rendered hairs 2-27 % dipping
    inside). Rendered hairs are sampled per face (barycentric grid), every point deeper than -margin is pushed back to
    +margin along the head normal and the push is distributed to the 3 guides by their weights. Roots stay. Each guide
    point moves at most max_move in total. -> (new strands, stats)."""
    G = np.array([np.asarray(p, float) for p in strands])
    if not len(F):
        return list(G), dict(moved=0)
    G0 = G.copy()
    F = np.asarray(F, int).reshape(-1, 3)
    W = []
    for i in range(n + 1):
        for j in range(n + 1 - i):
            W.append((i / n, j / n, 1 - i / n - j / n))
    W = np.array(W)
    K = G.shape[1]

    def blend(G_):
        A, B, C = G_[F[:, 0]], G_[F[:, 1]], G_[F[:, 2]]
        return W[None, :, 0, None, None] * A[:, None] + W[None, :, 1, None, None] * B[:, None] + W[None, :, 2, None, None] * C[:, None]

    def inside_share(G_):
        H = blend(G_)[:, :, 1:].reshape(-1, 3)
        return float((head.signed(H)[0] < -0.002).mean())

    before = inside_share(G)
    for _ in range(iters):
        H = blend(G)                                     # (faces, samples, K, 3)
        P = H.reshape(-1, 3)
        sd, nrm, _d = head.signed(P)
        need = np.maximum(margin - sd, 0.0).reshape(H.shape[:3])
        need[:, :, 0] = 0.0
        if not need.any():
            break
        push = need[..., None] * nrm.reshape(H.shape)    # wanted move of every rendered hair point
        acc = np.zeros_like(G)
        wsum = np.zeros(G.shape[:2])
        for c in range(3):
            w = W[None, :, c, None]                      # (1, samples, 1)
            np.add.at(acc, F[:, c], (w[..., None] * push).sum(1))
            np.add.at(wsum, F[:, c], np.broadcast_to(w * w, need.shape).sum(1))
        step = acc / np.maximum(wsum, 1e-6)[..., None]
        step[:, 0] = 0.0
        G = G + step
        off = G - G0
        L = np.linalg.norm(off, axis=2, keepdims=True)
        G = G0 + off * np.minimum(1.0, max_move / np.maximum(L, 1e-9))
    after = inside_share(G)
    mv = np.linalg.norm(G - G0, axis=2)
    st = dict(inside_before=round(before, 4), inside_after=round(after, 4), guides_moved=int((mv.max(1) > 0.0005).sum()),
              mean_move_mm=round(float(mv[mv > 0.0005].mean() * 1000) if (mv > 0.0005).any() else 0.0, 2),
              max_move_mm=round(float(mv.max() * 1000), 2))
    log('  interp_push: rendered hair points inside the head %.1f %% -> %.1f %%, %d guides moved (mean %.1f mm, max %.1f mm)' % (
        100 * before, 100 * after, st['guides_moved'], st['mean_move_mm'], st['max_move_mm']))
    return list(G), st


def head_push(strands, head, margin=0.003, carry=True):
    """R81: guide points (not roots) closer than `margin` to the head mesh (or inside it) are moved out to `margin`
    along the head normal; with carry the move is passed down the rest of the strand (no kink). Complements the
    sphere colliders (collision.push_out), which leave the skin between spheres unprotected. -> (strands, moved)."""
    out, moved = [], 0
    for p in strands:
        p = np.asarray(p, float).copy()
        hit = False
        for k in range(1, len(p)):
            sd, n, _ = head.signed(p[k:k + 1])
            if sd[0] < margin:
                dlt = n[0] * (margin - sd[0])
                if carry:
                    p[k:] += dlt
                else:
                    p[k] += dlt
                hit = True
        out.append(p); moved += hit
    return out, moved


# ------------------------------------------------------------------------------------------ densify (2026-10-06)
DENSIFY_AUTO_DEFAULT = 1.0      # roots per cm^2 within r = 1 cm (approved Sheala ~0.3-0.6, Shani median 1.3, p10 0.3)


def _fit_to_cards(G, kd, Vp, w, ramp_frac=0.3, max_pull=0.03, head=None, start=1):
    """pull guide points (index >= start) toward the nearest visible card-surface point: weight w * ramp (0 at the root,
    1 from ramp_frac of the length), then rescale every segment to its original length (as smooth_strands)."""
    G = np.asarray(G, float).copy()
    n, K = G.shape[:2]
    seg = np.linalg.norm(np.diff(G, axis=1), axis=2)
    t = np.linspace(0, 1, K)
    rp = np.clip(t / max(ramp_frac, 1e-6), 0, 1)
    rp[:start] = 0.0
    P = G.reshape(-1, 3)
    d, i = kd.query(P)
    pull = Vp[i] - P
    pull[d > max_pull] = 0.0
    wt = np.tile(rp, n) * w
    P2 = (P + pull * wt[:, None]).reshape(n, K, 3)
    out = np.empty_like(P2)
    out[:, 0] = G[:, 0]
    for j in range(1, K):
        dd = P2[:, j] - out[:, j - 1]
        dd /= np.maximum(np.linalg.norm(dd, axis=1, keepdims=True), 1e-9)
        out[:, j] = out[:, j - 1] + dd * seg[:, j - 1:j]
    if head is not None:
        for j in range(2, K):               # keep off the skin (the correction is carried down the strand)
            sd, nrm, _ = head.signed(out[:, j])
            m = np.maximum(0.003 - sd, 0.0)
            out[:, j:] += (nrm * m[:, None])[:, None, :]
    return out


def behind_face(strands, head, y_max=0.02, z_below=0.06, squash=0.1, keep_every=12, hug=0.004, flatten=0.0, x_min=0.0, log=print):
    """2026-10-06 (L131, Eredin: clumps of long hair fell forward over the cheeks, vanilla keeps the hair behind the
    face with a few thin strands in front): points of a strand that are in front of head.c.y + y_max and lower than
    head.top - z_below (the top / hairline stays) are pulled back to that plane (the excess y is multiplied by `squash`),
    then pushed out of the skin (`hug`). Every `keep_every`-th strand is left alone (the thin strands in front)."""
    c = np.array(head.c, float)
    out, n = [], 0
    for i, P in enumerate(strands):
        P = np.asarray(P, float).copy()
        if keep_every and i % keep_every == 0:
            out.append(P); continue
        m = (P[:, 1] > c[1] + y_max) & (P[:, 2] < head.top - z_below)
        m[0] = False
        if m.any():
            P[m, 1] = c[1] + y_max + (P[m, 1] - c[1] - y_max) * squash
            sd, nn, _ = head.signed(P)
            if flatten:
                # 2026-10-06 (L134 -> L138, Eredin: squashing the front hair behind a plane left a bare strip from temple to
                # jaw): the moved points also lie down on the skin (standoff -> hug by `flatten`), so the hair drapes
                # along the temple / sideburn instead of standing off; only beyond |x| > x_min (the face stays free)
                fm = m & (np.abs(P[:, 0] - c[0]) > x_min) & (sd > hug)
                P[fm] -= nn[fm] * ((sd[fm] - hug) * flatten)[:, None]
                sd, nn, _ = head.signed(P)
            low = m & (sd < hug)
            P[low] += nn[low] * (hug - sd[low])[:, None]
            n += 1
        out.append(P)
    log('  behind_face: %d strands pulled behind y %+.0f mm (keep every %s)' % (n, y_max * 1000, keep_every))
    return out


def side_cut(strands, head, depth=0.14, x_min=0.05, y_min=-0.01, log=print):
    """2026-10-06 (L131, Crach: locks hang down beside the face, vanilla hair is short and close to the head): a strand
    whose points beside the head (|x| > x_min, in front of head.c.y + y_min) go lower than head.top - depth is cut there
    (shortened, never removed; resampled to the same point count)."""
    c = np.array(head.c, float)
    out, n = [], 0
    for P in strands:
        P = np.asarray(P, float)
        bad = np.where((np.abs(P[:, 0]) > x_min) & (P[:, 1] > c[1] + y_min) & (P[:, 2] < head.top - depth))[0]
        bad = bad[bad >= 2]
        if len(bad):
            out.append(cut_strand(P, bad[0] - 1)); n += 1
        else:
            out.append(P)
    log('  side_cut: %d strands cut below %.0f mm under the head top beside the face' % (n, depth * 1000))
    return out


def straight_locks(strands, head, x_min=0.05, y_min=-0.005, root_z_below=0.04, length=0.1, every=3, short=0.03, hug=0.004, log=print):
    """2026-10-06 (L134, Crach: vanilla has long THIN STRAIGHT strands hanging at the temples in front of the ears, ours were
    curly tufts): strands rooted beside the face (|root x| > x_min, root y > head.c.y + y_min, root z < head.top -
    root_z_below) are replaced: every `every`-th one by a straight lock of `length` m hanging down from its root along the
    skin (pushed out of the skin by `hug`), the others cut to `short` m. Point count is kept."""
    c = np.array(head.c, float)
    out, n, k = [], 0, 0
    for P in strands:
        P = np.asarray(P, float)
        r = P[0]
        if abs(r[0] - c[0]) > x_min and r[1] > c[1] + y_min and r[2] < head.top - root_z_below:
            L = length if k % every == 0 else short
            k += 1
            t = np.linspace(0, 1, len(P))
            Q = np.tile(r, (len(P), 1))
            Q[:, 2] = r[2] - L * t
            sd, nn, _ = head.signed(Q)
            m = sd < hug
            Q[m] += nn[m] * (hug - sd[m])[:, None]
            out.append(Q); n += 1
        else:
            out.append(P)
    log('  straight_locks: %d temple strands straightened (%.0f mm every %d, else %.0f mm)' % (n, length * 1000, every, short * 1000))
    return out


def procedural_long(res, head, ch, alpha_img, spacing=0.005, reach=0.025, near_skin=0.04, zmin_rel=0.22, standoff=0.008,
                    front_back=-0.05, front_keep_every=10, front_keep_y=0.03, back_y=-0.11, z_noise=0.012, x_out=1.0,
                    inward=0.06, extra_len=0.10, side_x_add=0.015, face_dz=0.085, profile=False, outfit_pts=None, profile_from=0.12, profile_cap=0.07, profile_blend=0.06, hl_dz_c=None, hl_dz_s=None, seed=11, log=print):
    """2026-10-06 (L142, Eredin "simple long hair" rebuilt from scratch): the lane guides are REPLACED by procedural long
    straight hair. Roots: Poisson (`spacing`) over the scalp within `reach` of the vanilla cards' near-skin visible
    points (so temples and the hairline follow the vanilla coverage). Every strand: root -> a hang point H (lateral x
    = root x * x_out; y = the root's y for side roots but at most `front_back` m in front of the head centre = hair
    stays behind the face, blending to `back_y` behind the centre for roots near the middle; every
    `front_keep_every`-th front strand may hang to `front_keep_y` = the thin strands in front of the ear), the chord
    pushed out of the skin (`standoff`), then straight down to the vanilla cards' lowest z at that bearing (+- z_noise),
    drifting in by `inward` toward the shoulders. -> (res, n)"""
    from scipy.spatial import cKDTree as _KD
    rng = np.random.default_rng(seed)
    c = np.array(head.c, float)
    K = len(res['S'][0]['p']) if res.get('S') else 13
    vis = visible_card_points(ch, alpha_img, n=150000, alpha_vis=0.35, edge_filter=False)
    sdv = head.signed(vis)[0]
    near = vis[np.abs(sdv) < near_skin]
    hv, ht = head.v, head.t
    A = hv[ht[:, 1]] - hv[ht[:, 0]]; B = hv[ht[:, 2]] - hv[ht[:, 0]]
    area = 0.5 * np.linalg.norm(np.cross(A, B), axis=1) * (hv[ht].mean(1)[:, 2] > head.top - zmin_rel - 0.02)
    N = 300000
    tri = rng.choice(len(ht), N, p=area / area.sum())
    u, v = rng.random(N), rng.random(N)
    f = u + v > 1
    u[f], v[f] = 1 - u[f], 1 - v[f]
    Pp = hv[ht[tri, 0]] + u[:, None] * A[tri] + v[:, None] * B[tri]
    Pp = Pp[Pp[:, 2] > head.top - zmin_rel]
    Pp = Pp[_KD(near).query(Pp)[0] < reach]
    Pp = Pp[rng.permutation(len(Pp))]
    chosen, grid, r = [], {}, spacing
    for p in Pp:
        key = tuple((p // r).astype(int))
        ok = True
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for q in grid.get((key[0] + dx, key[1] + dy, key[2] + dz), ()):
                        if (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2 < r * r:
                            ok = False
                            break
                    if not ok: break
                if not ok: break
            if not ok: break
        if ok:
            chosen.append(p); grid.setdefault(key, []).append(p)
    R = np.array(chosen)
    if hl_dz_c is not None:
        # curved hairline (L148): the root band in front of the head is limited by dz(x) = hl_dz_c at the middle ->
        # hl_dz_s (default face_dz) at |x| >= 60 mm: a high forehead with the hairline curving down at the temples
        wx = np.clip(np.abs(R[:, 0]) / 0.06, 0, 1)
        wx = wx * wx * (3 - 2 * wx)
        dzx = hl_dz_c + ((hl_dz_s if hl_dz_s is not None else face_dz) - hl_dz_c) * wx
        bad = (R[:, 1] > head.c[1] + 0.01) & (R[:, 2] < head.top - dzx) & (np.abs(R[:, 0]) < 0.07)
    else:
        bad = face_roots(R, head, dz=face_dz)
    R = R[~bad]
    # lowest card z per bearing (10 deg bins, 5th percentile, median-smoothed)
    th_v = np.degrees(np.arctan2(vis[:, 0] - c[0], vis[:, 1] - c[1]))
    bins = np.arange(-180, 181, 10)
    zlow = np.full(len(bins) - 1, np.nan)
    for i in range(len(bins) - 1):
        m = (th_v >= bins[i]) & (th_v < bins[i + 1])
        if m.sum() > 30:
            zlow[i] = np.percentile(vis[m, 2], 3)
    zlow = np.where(np.isnan(zlow), np.nanmedian(zlow), zlow)
    zs = np.array([np.median(zlow[[(i + k) % len(zlow) for k in (-1, 0, 1)]]) for i in range(len(zlow))])

    def z_end(theta):
        return float(zs[min(int((theta + 180) // 10), len(zs) - 1)])
    prof = None
    if profile:
        # 2026-10-06 (L148, Eredin: in game the strands hung INSIDE the armour collar, y -0.12 vs the collar out to y -0.22 / -0.31
        # = a bare neck from the back): below `profile_from` under the head centre the vertical part follows the vanilla
        # cards' outer radius per (bearing 10 deg, z 25 mm) bin (70th percentile of the horizontal distance from the head
        # axis), and stays >= the outfit's radius + 12 mm there
        zb = np.arange(c[2] - 0.62, c[2] + 0.02, 0.025)
        rv = np.hypot(vis[:, 0] - c[0], vis[:, 1] - c[1])
        tb = np.arange(-180, 181, 10)
        prof = np.full((len(tb) - 1, len(zb) - 1), np.nan)
        for i in range(len(tb) - 1):
            mi = (th_v >= tb[i]) & (th_v < tb[i + 1])
            for j in range(len(zb) - 1):
                m = mi & (vis[:, 2] >= zb[j]) & (vis[:, 2] < zb[j + 1])
                if m.sum() >= 6:
                    prof[i, j] = np.percentile(rv[m], 70)
        prof_o = np.zeros_like(prof)
        if outfit_pts is not None and len(outfit_pts):
            th_o = np.degrees(np.arctan2(outfit_pts[:, 0] - c[0], outfit_pts[:, 1] - c[1]))
            ro = np.hypot(outfit_pts[:, 0] - c[0], outfit_pts[:, 1] - c[1])
            for i in range(len(tb) - 1):
                mi = (th_o >= tb[i] - 5) & (th_o < tb[i + 1] + 5)
                for j in range(len(zb) - 1):
                    m = mi & (outfit_pts[:, 2] >= zb[j]) & (outfit_pts[:, 2] < zb[j + 1])
                    if m.sum() >= 3:
                        prof_o[i, j] = ro[m].max() + 0.012
        # fill gaps: along z (nearest valid), then along bearing
        for i in range(prof.shape[0]):
            ok = ~np.isnan(prof[i])
            if ok.any():
                prof[i] = np.interp(np.arange(prof.shape[1]), np.where(ok)[0], prof[i][ok])
        okr = ~np.isnan(prof).all(1)
        for j in range(prof.shape[1]):
            col = prof[:, j]
            ok = ~np.isnan(col)
            if ok.any() and (~ok).any():
                idx = np.arange(len(col))
                col2 = np.interp(idx, idx[ok], col[ok], period=len(col))
                prof[:, j] = np.where(ok, col, col2)

        def prof_r(theta, z):
            i = min(int((theta + 180) // 10), prof.shape[0] - 1)
            jf = (z - zb[0]) / 0.025 - 0.5
            j0 = int(np.clip(np.floor(jf), 0, prof.shape[1] - 1)); j1 = min(j0 + 1, prof.shape[1] - 1)
            f_ = float(np.clip(jf - j0, 0, 1))
            return (prof[i, j0] * (1 - f_) + prof[i, j1] * f_, prof_o[i, j0] * (1 - f_) + prof_o[i, j1] * f_)
    out = []
    k_front = 0
    for r_ in R:
        x, y, z = r_ - c
        theta = np.degrees(np.arctan2(x, y))
        sx = 1.0 if x >= 0 else -1.0
        ax = abs(x)
        w_side = float(np.clip((ax - 0.03) / 0.04, 0.0, 1.0))          # 0 near the middle, 1 beside the head
        yh_side = min(y, front_back)
        if y > front_back and ax > 0.05:
            k_front += 1
            if front_keep_every and k_front % front_keep_every == 0:
                yh_side = min(y, front_keep_y)
        yh = (1 - w_side) * (back_y) + w_side * yh_side
        xh = sx * max(ax * x_out, 0.0) if w_side > 0 else x * x_out
        xh = x * x_out + sx * side_x_add * w_side
        zh = w_side * z + (1 - w_side) * min(z, -0.02)
        Hp = c + np.array([xh, yh, zh])
        zend = z_end(np.degrees(np.arctan2(Hp[0] - c[0], Hp[1] - c[1]))) + rng.uniform(-z_noise, z_noise)
        zend = min(zend - extra_len, c[2] + zh - 0.06)
        pts = []
        n1 = max(K // 2, 4)
        for i in range(n1 + 1):
            t = i / n1
            pts.append(r_ * (1 - t) + Hp * t)
        n2 = K - 1 - n1
        th_h = np.degrees(np.arctan2(Hp[0] - c[0], Hp[1] - c[1]))
        rho_h = float(np.hypot(Hp[0] - c[0], Hp[1] - c[1]))
        for j in range(1, n2 + 1):
            t = j / n2
            zz = Hp[2] + (zend - Hp[2]) * t
            if prof is not None and zz < c[2] - profile_from:
                w_ = float(np.clip((c[2] - profile_from - zz) / profile_blend, 0, 1))
                pc_, po_ = prof_r(th_h, zz)
                pc_ = min(pc_, rho_h + profile_cap)
                rho = rho_h * (1 - w_) + max(pc_, po_) * w_
                rho = max(rho, rho_h, po_) if w_ < 1 else max(rho, po_)
                pts.append(np.array([c[0] + rho * np.sin(np.radians(th_h)), c[1] + rho * np.cos(np.radians(th_h)), zz]))
                continue
            drift = inward * t * t
            pts.append(np.array([Hp[0] * (1 - drift), Hp[1] - 0.01 * t, zz]))
        P = np.array(pts)
        # keep off the skin: chord points pushed out along the head normal
        for _ in range(2):
            sd, nn, _d = head.signed(P)
            low = sd < standoff
            low[0] = False
            P[low] += nn[low] * (standoff - sd[low])[:, None]
        out.append(P)
    new = [dict(p=g, w=[(0, 1.0)]) for g in out]
    r2 = dict(res)
    r2['min_area'] = min(res.get('min_area', 1e-5), 0.05 * spacing * spacing)
    r2['S'] = new
    r2 = reindex(r2, list(range(len(new))))
    log('  procedural_long: %d roots (spacing %.1f mm, reach %.0f mm), hang behind the face, lowest card z %.0f..%.0f mm rel. head top' % (
        len(new), spacing * 1000, reach * 1000, (zs.min() - head.top) * 1000, (zs.max() - head.top) * 1000))
    return r2, len(new)


def densify(res, head, ch, alpha_img, spacing=0.005, k=4, w_fit=0.6, w_fit_orig=0.3, ramp=0.3, reach=0.012,
            near_skin=0.02, zmin_rel=0.20, auto=False, min_guides_per_cm2=None, max_new=3000, max_pull=0.03,
            seed=7, env=None, y_max=None, log=print):
    """2026-10-06 (user, Olgierd: 31 guides cannot carry a dense tuft; "detect, add and duplicate as many strands as
    needed and reposition to achieve the original look"). One guide per `spacing` over the scalp area the vanilla cards
    cover (visible card points within near_skin of the skin; candidates within `reach` of them), skipping candidates
    within `spacing` of an existing root. Each new guide = inverse-distance-weighted blend of the k nearest guides'
    shapes (offsets from their roots) placed at the new root; then every guide point is pulled toward the nearest visible
    card-surface point (w_fit new / w_fit_orig originals, ramped 0 -> 1 over the first `ramp` of the length), segment
    lengths kept. auto: only where the local root density (r = 1 cm) is below min_guides_per_cm2.
    -> (res, number added)"""
    vis = visible_card_points(ch, alpha_img, n=150000, alpha_vis=0.35, edge_filter=False)
    kd = cKDTree(vis)
    G0 = np.array([np.asarray(s['p'], float) for s in res['S']])
    R0 = G0[:, 0]
    rng = np.random.default_rng(seed)
    hv, ht = head.v, head.t
    A = hv[ht[:, 1]] - hv[ht[:, 0]]; B = hv[ht[:, 2]] - hv[ht[:, 0]]
    area = 0.5 * np.linalg.norm(np.cross(A, B), axis=1)
    area = area * (hv[ht].mean(1)[:, 2] > head.top - zmin_rel - 0.02)
    N = 200000
    tri = rng.choice(len(ht), N, p=area / area.sum())
    u, v = rng.random(N), rng.random(N)
    f = u + v > 1
    u[f], v[f] = 1 - u[f], 1 - v[f]
    Pp = hv[ht[tri, 0]] + u[:, None] * A[tri] + v[:, None] * B[tri]
    Pp = Pp[Pp[:, 2] > head.top - zmin_rel]
    near = vis[np.abs(head.signed(vis)[0]) < near_skin]
    Pp = Pp[cKDTree(near).query(Pp)[0] < reach]
    if y_max is not None:
        # 2026-10-06 (L148): candidate roots only behind head.c.y + y_max (a back / crown patch, never the front)
        Pp = Pp[Pp[:, 1] < head.c[1] + y_max]
    Pp = Pp[rng.permutation(len(Pp))]
    kdr = cKDTree(R0)
    if auto:
        cnt = np.array([len(x) for x in kdr.query_ball_point(Pp, 0.01)])
        dens = cnt / np.pi                      # roots per cm^2 inside r = 1 cm
        lim = min_guides_per_cm2 if min_guides_per_cm2 is not None else DENSIFY_AUTO_DEFAULT
        Pp = Pp[dens < lim]
    Pp = Pp[kdr.query(Pp)[0] >= spacing]
    chosen, grid = [], {}
    r = spacing
    for p in Pp:
        key = tuple((p // r).astype(int))
        ok = True
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for q in grid.get((key[0] + dx, key[1] + dy, key[2] + dz), ()):
                        if (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2 < r * r:
                            ok = False
                            break
                    if not ok:
                        break
                if not ok:
                    break
            if not ok:
                break
        if ok:
            chosen.append(p)
            grid.setdefault(key, []).append(p)
            if len(chosen) >= max_new:
                break
    if not chosen:
        log('  densify: no candidate roots (coverage already at %.0f mm spacing)' % (spacing * 1000))
        return res, 0
    C = np.array(chosen)
    kk = min(k, len(R0))
    dd, ii = kdr.query(C, k=kk)
    dd = dd.reshape(len(C), kk); ii = ii.reshape(len(C), kk)
    w = 1.0 / np.maximum(dd, 1e-4) ** 2
    w /= w.sum(1, keepdims=True)
    off = G0 - R0[:, None, :]
    Gn = C[:, None, :] + (w[:, :, None, None] * off[ii]).sum(1)
    Ls = np.linalg.norm(np.diff(G0, axis=1), axis=2).sum(1)      # blending shortens: restore the weighted length
    Ln = np.linalg.norm(np.diff(Gn, axis=1), axis=2).sum(1)
    Lt = (w * Ls[ii]).sum(1)
    Gn = C[:, None, :] + (Gn - C[:, None, :]) * (Lt / np.maximum(Ln, 1e-9))[:, None, None]

    def dist(G):
        return float(kd.query(G[:, 2:].reshape(-1, 3))[0].mean() * 1000)
    d0o, d0n = dist(G0), dist(Gn)
    Gn2 = _fit_to_cards(Gn, kd, vis, w_fit, ramp, max_pull, head)
    G02 = _fit_to_cards(G0, kd, vis, w_fit_orig, ramp, max_pull, head) if w_fit_orig > 0 else G0
    if env:
        # 2026-10-06 (L128, Dettlaff: curly cards = long coiled lanes, blended + straightened they flew out of the card
        # volume): opt-in `env` (m): every guide (original and new) is cut where it leaves the visible card surface by
        # more than env (card_envelope_trim logic on the same KD tree), resampled back to K points
        def _cut(G):
            out = []
            for p in G:
                bd = np.where(kd.query(p)[0][2:] > env)[0]
                out.append(cut_strand(p, bd[0] + 1) if len(bd) else p)
            return np.array(out)
        G02, Gn2 = _cut(G02), _cut(Gn2)
    d1o, d1n = dist(G02), dist(Gn2)
    bad = face_roots(Gn2[:, 0], head)
    Gn2 = Gn2[~bad]
    S = []
    for i, s in enumerate(res['S']):
        s2 = dict(s)
        s2['p'] = G02[i]
        S.append(s2)
    new = [dict(p=g, w=[(0, 1.0)]) for g in Gn2]
    r2 = dict(res)
    r2['min_area'] = min(res.get('min_area', 1e-5), 0.05 * spacing * spacing)   # dense roots: 3.5 mm triangles are ~5 mm^2
    r2['S'] = S + new
    r2 = reindex(r2, list(range(len(r2['S']))))
    log('  densify: spacing %.1f mm%s, guides %d -> %d (+%d, %d on the face dropped), mean guide-to-card distance: '
        'original %.2f -> %.2f mm (w %.2f), new %.2f -> %.2f mm (w %.2f)' % (
            spacing * 1000, ' auto' if auto else '', len(G0), len(r2['S']), len(Gn2), int(bad.sum()), d0o, d1o, w_fit_orig,
            d0n, d1n, w_fit))
    return r2, len(Gn2)
