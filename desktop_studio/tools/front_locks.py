r"""front_locks - forward locks rooted ON the painted scalp, traced from the game's own front hair cards (opt-in,
Studio-only; 2026-10-15, Triss session triss2).

  py desktop_studio\tools\front_locks.py <cfg> --geo <dump_geo.json> --out <ops.json> --hairline-sweeps --uv u,v
  then: hws.py studio_ops '{"file": "<ops.json>"}'   (one undoable 'rebuild' op: new guides + their own growth faces)

Why: the long lore-friendly Triss cards hang curtains FORWARD over the shoulders onto the chest and sweep from the
front hairline past the temples; the strand build sends every guide back over the shoulders. The fill tools copy
neighbouring guides (so they also go back) or plant short forehead fringes. This tool:
  1. picks the card strips whose tips hang in front of the shoulders (tip y > front_y, tip z < tip_z_max) and, with
     --hairline-sweeps, the strips from the front hairline / parting that pass over bare skin in front (temples);
  2. traces lanes along each strip (iso-u lines of the card UV, root = small v end; stacked duplicates dropped);
  3. roots every lane ON the painted scalp (replica of App::computeHeadScalp): root-mode edge walks the lane while it
     lies over the painted scalp and roots it edge_back_mm before it leaves; otherwise the lane top is projected, and
     an off-scalp root moves to the nearest painted-hairline point inset_mm inside. Lock roots also need painted scalp
     root_margin_mm around them. The first part lies on the scalp, then the strand follows the lane forward;
  4. smooths each guide and keeps it outside the head: nearest-vertex push-out (never along the mesh's inner surfaces),
     below the ears an outer shell (jaw radius kept for drape_mm) and no tuck back toward the neck (tuck_mm);
  5. lane lock: each lane becomes 3 parallel guides (two lock_w_mm apart, side vector parallel-transported along the
     lane so they never cross; one lock_h_mm behind) and ONE growth face, so the interpolated hair is a ribbon along
     the lane and never blends with a guide going elsewhere (no web through the neck / shoulder);
  6. rejects locks whose guides or blends enter the face box or come within eye_mm of an eye, shorter than min_len
     (short sweeps end as tufts), or rooted within min_root_mm of another root.
Run studio_hairline_check / studio_eye_check after applying (the Studio's own checks are the reference).
"""
import argparse
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


# ------------------------------------------------------------------------------------------------ pack / scalp mask
def load_pack(cfg):
    p = os.path.join(ROOT, 'build', 'desktop_studio', cfg, 'desk.json')
    with open(p, encoding='utf-8') as f:
        return json.load(f), os.path.dirname(p)


def scalp_mask(D, pdir):
    """Replica of App::computeHeadScalp (agent.cpp): per head vertex, painted scalp or not."""
    from PIL import Image
    from scipy.ndimage import uniform_filter
    ht = D['head_tex']
    V = np.array(ht['v'], float); UV = np.array(ht['uv'], float); T = np.array(ht['t'], int)
    im = np.array(Image.open(os.path.join(pdir, ht['file'])).convert('RGB')).astype(float)
    h, w = im.shape[:2]
    lum = uniform_filter(0.3 * im[..., 0] + 0.59 * im[..., 1] + 0.11 * im[..., 2], 9, mode='nearest')
    cx = np.clip((UV[:, 0] * w).astype(int), 0, w - 1); cy = np.clip((UV[:, 1] * h).astype(int), 0, h - 1)
    L = lum[cy, cx]
    C = np.array(D['head']['c'], float)
    sk = (V[:, 1] > C[1] + 0.06) & (V[:, 2] > C[2] - 0.06) & (V[:, 2] < C[2] - 0.01)
    skin = float(np.median(L[sk])) if sk.any() else 140.0
    thr = min(95.0, 0.62 * skin)
    eyes = [np.array(o['v'], float) for o in D.get('outfit_tex', []) if 'eye' in o.get('file', '')]
    eyeTop = max(e[:, 2].max() for e in eyes) if eyes else None
    browZ = eyeTop + 0.035 if eyeTop is not None else C[2] + 0.05
    frontLow = (V[:, 1] > C[1] + 0.05) & (V[:, 2] < browZ)
    dark = (~frontLow) & (L < thr)
    keys = {}
    wid = np.zeros(len(V), int)
    for i, p in enumerate(V):
        wid[i] = keys.setdefault(tuple(np.round(p * 1e4).astype(int)), len(keys))
    nw = len(keys)
    wd = np.zeros(nw, bool); wz = np.zeros(nw)
    wd[wid[dark]] = True
    wz[wid] = V[:, 2]
    adj = [[] for _ in range(nw)]
    for t in T:
        for c in range(3):
            a, b = wid[t[c]], wid[t[(c + 1) % 3]]
            adj[a].append(b); adj[b].append(a)
    zTop = wz.max()
    inn = np.zeros(nw, bool)
    q = [i for i in range(nw) if wd[i] and wz[i] > zTop - 0.02]
    inn[q] = True
    while q:
        a = q.pop()
        for b in adj[a]:
            if not inn[b] and wd[b] and wz[b] > C[2]:
                inn[b] = True; q.append(b)
    flag = inn[wid]
    # vertex normals (outward from the head centre)
    N = np.zeros_like(V)
    for t in T:
        n = np.cross(V[t[1]] - V[t[0]], V[t[2]] - V[t[0]])
        N[t] += n
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-12)
    flip = ((V - C) * N).sum(1) < 0
    N[flip] *= -1
    # one mesh may hold both eyes: one centre per side of the head's mid plane
    eyeC = [e[m].mean(0) for e in eyes for m in (e[:, 0] < C[0], e[:, 0] >= C[0]) if m.sum() > 3]
    return dict(V=V, T=T, N=N, flag=flag, C=C, browZ=browZ, eyes=eyeC, skin=skin, thr=thr)


def hairline(S):
    V, T, f = S['V'], S['T'], S['flag']
    pt, inner, seen = [], [], set()
    for t in T:
        for c in range(3):
            i, j = t[c], t[(c + 1) % 3]
            if f[i] == f[j]:
                continue
            k = (min(i, j), max(i, j))
            if k in seen:
                continue
            seen.add(k)
            pt.append((V[i] + V[j]) / 2); inner.append(V[i] if f[i] else V[j])
    return np.array(pt), np.array(inner)


class Skin:
    """signed distance to the head mesh by nearest vertex + its normal (as the Studio's HeadSkin)."""
    def __init__(self, S):
        from scipy.spatial import cKDTree
        self.V, self.N, self.flag = S['V'], S['N'], S['flag']
        self.kd = cKDTree(self.V)

    def sd(self, p):
        _, i = self.kd.query(p)
        return float(np.dot(p - self.V[i], self.N[i])), self.N[i], i

    def on_scalp(self, p):
        _, i = self.kd.query(p)
        return bool(self.flag[i])

    def build_shell(self, z_top, drape_mm=30.0, dz=0.005, naz=72):
        V = self.V
        self.drape_mm = drape_mm
        self.sh_z0, self.sh_dz, self.sh_naz, self.sh_top = float(V[:, 2].min()), dz, naz, z_top
        ns = int((z_top - self.sh_z0) / dz) + 2
        self.sh_c = np.zeros((ns, 2)); self.sh_r = np.zeros((ns, naz))
        si = ((V[:, 2] - self.sh_z0) / dz).astype(int)
        for k in range(ns):
            m = np.abs(si - k) <= 2
            if not m.any():
                continue
            P = V[m, :2]; c = P.mean(0); self.sh_c[k] = c
            d = P - c; r = np.linalg.norm(d, axis=1)
            az = ((np.arctan2(d[:, 1], d[:, 0]) + np.pi) / (2 * np.pi) * naz).astype(int) % naz
            np.maximum.at(self.sh_r[k], az, r)
        for _ in range(2):   # a sector without vertices takes its neighbours' radius
            self.sh_r = np.maximum(self.sh_r, 0.5 * (np.roll(self.sh_r, 1, 1) + np.roll(self.sh_r, -1, 1)))
        # hair hangs: below the jaw it keeps the jaw's radius for drape_mm (no tuck into the neck under the jaw)
        w = max(1, int(self.drape_mm / 1000 / dz))
        self.sh_r = np.array([self.sh_r[k:k + w + 1].max(0) for k in range(ns)])

    def shell_out(self, p, standoff):
        if p[2] >= self.sh_top:
            return p
        k = int((p[2] - self.sh_z0) / self.sh_dz)
        if k < 0 or k >= len(self.sh_c):
            return p
        c = self.sh_c[k]; d = p[:2] - c; r = np.linalg.norm(d)
        if r < 1e-6:
            return p
        a = int((np.arctan2(d[1], d[0]) + np.pi) / (2 * np.pi) * self.sh_naz) % self.sh_naz
        need = max(self.sh_r[k, (a + j) % self.sh_naz] for j in (-1, 0, 1)) + standoff
        if r < need:
            p = p.copy(); p[:2] = c + d / r * need
        return p

    def on_scalp_margin(self, p, r):
        """on the scalp and so are 8 points r around it (the Studio's own check may pick another nearby vertex)"""
        if not self.on_scalp(p):
            return False
        if r <= 0:
            return True
        _, n, _ = self.sd(p)
        u = np.cross(n, [0, 0, 1.0])
        if np.linalg.norm(u) < 1e-6:
            u = np.cross(n, [1.0, 0, 0])
        u = u / np.linalg.norm(u); v = np.cross(n, u)
        return all(self.on_scalp(self.project(p + r * (np.cos(t) * u + np.sin(t) * v), 0.0))
                   for t in np.linspace(0, 2 * np.pi, 8, endpoint=False))

    def project(self, p, standoff=0.0):
        for _ in range(3):
            d, n, _ = self.sd(p)
            p = p - n * (d - standoff)
        return p


# ------------------------------------------------------------------------------------------------ card lanes
def card_components(D):
    from scipy.sparse import coo_matrix
    from scipy.sparse.csgraph import connected_components
    out = []
    for hc in D.get('hair_cards', []):
        V = np.array(hc['v'], float); T = np.array(hc['t'], int); UV = np.array(hc['uv'], float)
        n = len(V)
        r = np.concatenate([T[:, 0], T[:, 1], T[:, 2]]); c = np.concatenate([T[:, 1], T[:, 2], T[:, 0]])
        k, lab = connected_components(coo_matrix((np.ones(len(r)), (r, c)), shape=(n, n)), directed=False)
        tlab = lab[T[:, 0]]
        for cc in range(k):
            tm = T[tlab == cc]
            if len(tm) < 4:
                continue
            out.append(dict(V=V, T=tm, UV=UV))
    return out


def lane_of(comp, q, nbins, along='v'):
    """iso-u lane at fraction q of the strip width: (P list root->tip) or None."""
    V, T, UV = comp['V'], comp['T'], comp['UV']
    # barycentric samples of every triangle
    bs = np.array([(a, b, 1 - a - b) for a in np.linspace(0, 1, 5) for b in np.linspace(0, 1, 5) if a + b <= 1.0001])
    P = np.einsum('sk,tkd->tsd', bs, V[T]).reshape(-1, 3)
    U = np.einsum('sk,tkd->tsd', bs, UV[T]).reshape(-1, 2)
    a_i, c_i = (1, 0) if along == 'v' else (0, 1)
    va = U[:, a_i]; ua = U[:, c_i]
    lo, hi = va.min(), va.max()
    if hi - lo < 1e-4:
        return None
    edges = np.linspace(lo, hi, nbins + 1)
    pts = []
    for b in range(nbins):
        m = (va >= edges[b]) & (va <= edges[b + 1])
        if m.sum() < 3:
            continue
        u0, u1 = ua[m].min(), ua[m].max()
        un = (ua[m] - u0) / max(u1 - u0, 1e-6)
        mm = np.abs(un - q) < 0.15
        if mm.sum() < 2:
            mm = np.abs(un - q) < 0.3
        if mm.sum() < 1:
            continue
        pts.append(P[m][mm].mean(0))
    if len(pts) < 4:
        return None
    return np.array(pts)   # small v first = root end


def edge_cut(Ln, last, back_mm):
    """lane from back_mm before its last on-scalp point (index last) to its tip"""
    if last < 1:
        return Ln
    Q0 = resample(Ln[:last + 1], 40)
    sQ = arclen(Q0)
    kk = min(int(np.searchsorted(sQ, max(0.0, sQ[-1] - back_mm / 1000))), len(Q0) - 1)
    return np.vstack([Q0[kk:], Ln[last + 1:]])


def arclen(Q):
    return np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(Q, axis=0), axis=1))])


def resample(Q, K):
    s = arclen(Q)
    t = np.linspace(0, s[-1], K)
    return np.stack([np.interp(t, s, Q[:, d]) for d in range(3)], 1)


def smooth(Q, passes=2, w=0.5, keep_root=True):
    Q = Q.copy()
    for _ in range(passes):
        R = Q.copy()
        R[1:-1] = Q[1:-1] * (1 - w) + (Q[:-2] + Q[2:]) * (w / 2)
        if keep_root:
            R[0] = Q[0]
        Q = R
    return Q


# ------------------------------------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cfg')
    ap.add_argument('--geo', required=True, help='studio_dump_geo JSON of the current groom (edit frame)')
    ap.add_argument('--out', required=True, help='ops JSON to write ({"ops": [...]})')
    ap.add_argument('--front-y', type=float, default=0.04, help='lane tip y (m, edit frame, face side +) to count as front')
    ap.add_argument('--tip-z-max', type=float, default=1.50, help='lane tip below this z = hangs in front of the shoulder')
    ap.add_argument('--hairline-sweeps', action='store_true', help='also strips starting at the front hairline')
    ap.add_argument('--lanes', type=str, default='0.15,0.5,0.85', help='u fractions across each strip (3 = one lock face)')
    ap.add_argument('--min-root-mm', type=float, default=1.5, help='min distance between any two roots')
    ap.add_argument('--lock', choices=['lane', 'strip'], default='lane',
                    help='lane: every card lane (u fractions of --lanes, stacked copies dropped) becomes one lock = 3 '
                         'parallel guides (two side by side lock_w_mm apart, one lock_h_mm behind) + ONE growth face, so the '
                         'interpolated hair is a ribbon along the lane and never blends with a guide going elsewhere; '
                         'strip: the 3 lanes of one card strip are the lock')
    ap.add_argument('--lock-w-mm', type=float, default=9.0, help='lane lock: width at the root')
    ap.add_argument('--lock-h-mm', type=float, default=4.0, help='lane lock: depth of the third root behind the pair')
    ap.add_argument('--lock-fan', type=float, default=0.6, help='lane lock: the width grows by this factor toward the tip')
    ap.add_argument('--bins', type=int, default=24)
    ap.add_argument('--dedupe-mm', type=float, default=3.0, help='lanes closer than this (mean) are duplicates')
    ap.add_argument('--reach-mm', type=float, default=20.0, help='lane upper end farther than this from the head = a lower card segment: skipped')
    ap.add_argument('--max-move-mm', type=float, default=25.0, help='root moved farther than this to reach the painted scalp: skipped')
    ap.add_argument('--orient', choices=['v', 'centre'], default='v')
    ap.add_argument('--root-mode', choices=['top', 'edge'], default='edge',
                    help='top: root at the lane upper end (moved onto the scalp when needed); edge: root on the lane where it '
                         'still lies over the painted scalp, edge_back_mm before it leaves it (sweeps from a parting: roots spread '
                         'along the hairline, the strand lies on the scalp, crosses the hairline and falls forward)')
    ap.add_argument('--edge-back-mm', type=float, default=15.0)
    ap.add_argument('--inset-mm', type=float, default=4.0, help='roots moved to the hairline sit this far inside it')
    ap.add_argument('--standoff-mm', type=float, default=2.5, help='points kept at least this far outside the head')
    ap.add_argument('--lift-mm', type=float, default=1.5, help='second point height over the scalp')
    ap.add_argument('--eye-mm', type=float, default=22.0, help='no guide point closer than this to an eye centre')
    ap.add_argument('--trim', action='store_true', help='cut locks before the face box / eyes instead of dropping them')
    ap.add_argument('--min-len', type=float, default=0.15, help='shorter locks are dropped (short sweeps stand up as tufts)')
    ap.add_argument('--face-x', type=float, default=0.035, help='face box half width (m) around the head centre x')
    ap.add_argument('--face-z', type=str, default='1.585,1.712', help='face box z range: no lane point inside the box in front')
    ap.add_argument('--face-y', type=float, default=0.06, help='face box: points with y above this (in front of the face)')
    ap.add_argument('--shell-top', type=float, default=1.62,
                    help='below this z, guides stay outside the outer shell of the head/neck (no dents at the jaw/neck)')
    ap.add_argument('--drape-mm', type=float, default=30.0, help='shell: radius kept for this height below the jaw')
    ap.add_argument('--tuck-mm', type=float, default=1.0, help='below the ears: max inward step toward the head axis per point')
    ap.add_argument('--root-margin-mm', type=float, default=2.5, help='lock roots need painted scalp this far around them')
    ap.add_argument('--smooth-rounds', type=int, default=3, help='smooth + push-out rounds of each guide')
    ap.add_argument('--smooth-passes', type=int, default=4)
    ap.add_argument('--length-scale', type=float, default=1.0)
    ap.add_argument('--max-new', type=int, default=300, help='max locks (growth faces)')
    ap.add_argument('--inside-mm', type=float, default=2.0, help='growth faces: drop faces whose interpolated hairs pass this deep inside the head / neck')
    ap.add_argument('--uv', type=str, default='', help='u,v for every new face corner (density / colour texel)')
    ap.add_argument('--side', choices=['both', 'plus', 'minus'], default='both')
    ap.add_argument('--list', action='store_true', help='print lanes only')
    a = ap.parse_args()

    D, pdir = load_pack(a.cfg)
    S = scalp_mask(D, pdir)
    sk = Skin(S)
    sk.build_shell(a.shell_top, a.drape_mm)
    hlp, hli = hairline(S)
    C = S['C']
    G = json.load(open(a.geo, encoding='utf-8'))
    K = int(G['K'])
    old = np.array([g[0] for g in G['guides']], float)

    comps = card_components(D)
    fz0, fz1 = map(float, a.face_z.split(','))
    qs = [float(x) for x in a.lanes.split(',')]
    why = {}

    def rej(k):
        why[k] = why.get(k, 0) + 1

    def root_of(Ln):
        """(lane cut to start at the root, root on the painted scalp, moved m) or (None, reason, 0)"""
        top = Ln[0]
        if abs(sk.sd(top)[0]) > a.reach_mm / 1000:
            return None, 'lane_top_off_head', 0
        if a.root_mode == 'edge':
            # walk the lane while it lies over the painted scalp (projected point on scalp, within reach of the skin);
            # a lane that starts off the scalp keeps the top mode (root moved onto the scalp below)
            last = -1
            for k in range(len(Ln)):
                d_, _, _ = sk.sd(Ln[k])
                if abs(d_) > a.reach_mm / 1000 or not sk.on_scalp(sk.project(Ln[k].copy(), 0.0)):
                    break
                last = k
            if last >= 0:
                Ln = edge_cut(Ln, last, a.edge_back_mm)
                top = Ln[0]
        R0 = sk.project(top.copy(), 0.0)
        moved = 0.0
        if not sk.on_scalp(R0):
            # off the painted scalp: nearest painted-hairline point, inset_mm inside
            b = int(np.argmin(np.linalg.norm(hlp - R0, axis=1)))
            inn = hli[b] - hlp[b]
            R1 = sk.project(hlp[b] + inn / max(np.linalg.norm(inn), 1e-9) * (a.inset_mm / 1000), 0.0)
            if not sk.on_scalp(R1):
                R1 = sk.project(hli[b].copy(), 0.0)
            if not sk.on_scalp(R1):   # nearest painted-scalp vertex itself
                fv = np.where(sk.flag)[0]
                R1 = sk.V[fv[int(np.argmin(np.linalg.norm(sk.V[fv] - R0, axis=1)))]].copy()
            moved = float(np.linalg.norm(R1 - R0))
            R0 = R1
        if not sk.on_scalp(R0):
            return None, 'root_off_scalp', 0
        if moved > a.max_move_mm / 1000:
            return None, 'root_too_far_from_scalp', 0
        return Ln, R0, moved

    def keep_out(Q):
        """push points out of the head (nearest-vertex normal) and, below the ears, out of the outer shell"""
        ax = C[:2]
        for k in range(1, len(Q)):
            Q[k] = sk.shell_out(Q[k], a.standoff_mm / 1000)
            if Q[k][2] < a.shell_top and k > 1:
                # hanging hair does not tuck back toward the neck: the distance to the head's vertical axis never
                # drops by more than tuck_mm per point below the ears
                dk = Q[k][:2] - ax; rk = np.linalg.norm(dk); rp = np.linalg.norm(Q[k - 1][:2] - ax) - a.tuck_mm / 1000
                if rk < rp and rk > 1e-6:
                    Q[k][:2] = ax + dk / rk * rp
            d, n, _ = sk.sd(Q[k])
            want = a.lift_mm / 1000 if k <= 1 else a.standoff_mm / 1000
            # the head mesh has inner surfaces (mouth, neck rim) whose normals face the axis: never push along those
            if -0.06 < d < want and (Q[k][2] >= a.shell_top or np.dot(n[:2], Q[k][:2] - ax) > 0):
                Q[k] = Q[k] + n * (want - d)
        return Q

    def guide_of(Ln, R0):
        """K points: root on the scalp -> along the scalp (lift_mm .. standoff_mm) to the lane -> the lane to its tip"""
        j = int(np.argmin(np.linalg.norm(Ln - R0, axis=1)))
        lead = []
        tgt = Ln[j]
        nstep = max(1, int(np.linalg.norm(tgt - R0) / 0.004))
        for k in range(1, nstep + 1):
            p = R0 + (tgt - R0) * (k / nstep)
            d, n, _ = sk.sd(p)
            want = a.lift_mm / 1000 + (a.standoff_mm - a.lift_mm) / 1000 * min(1.0, k / nstep)
            if d < want:
                p = p + n * (want - d)
            lead.append(p)
        Q = np.vstack([R0[None], np.array(lead), Ln[j + 1:]])
        if a.length_scale != 1.0:
            Q = Q[arclen(Q) <= arclen(Q)[-1] * a.length_scale]
        if len(Q) < 3 or arclen(Q)[-1] < a.min_len:
            return None
        Q = resample(Q, max(K * 3, 60))
        for _ in range(a.smooth_rounds):   # smooth (no kinks where the lead joins the lane / where cards fold), stay outside
            Q = keep_out(smooth(Q, a.smooth_passes, 0.5))
            Q[0] = R0
        return resample(Q, K)

    # one LOCK per card strip: the strip's lanes (u fractions qs) become the lock's guides and ONE growth face, so the
    # interpolated hair fills the strip like the card does and never blends with a guide going elsewhere
    cand = []
    for comp in comps:
        lanes = []
        for q in qs:
            Ln = lane_of(comp, q, a.bins)
            if Ln is None:
                break
            # root end: small v (the atlas's root -> tip direction; fringe strips have both ends near the skin, so a
            # distance test is wrong for them) or, with --orient centre, the end nearer the head centre
            if a.orient == 'centre' and np.linalg.norm(Ln[-1] - C) < np.linalg.norm(Ln[0] - C):
                Ln = Ln[::-1]
            lanes.append(Ln)
        if len(lanes) != len(qs):
            rej('no_lane'); continue
        mid = lanes[len(lanes) // 2]
        tip = mid[-1]
        hang_front = tip[1] > a.front_y and tip[2] < a.tip_z_max
        sweep = False
        if a.hairline_sweeps and mid[0][1] > C[1] - 0.04 and mid[0][2] > S['browZ']:
            # a strip from the front hairline / parting that passes over bare skin in front of the head (forehead
            # corner, temple) within 15 mm of it: the hair that frames the face, wherever its tip ends
            for q_ in mid:
                if q_[1] > a.front_y and S['browZ'] - 0.06 < q_[2] < 1.77:
                    d_, _, _ = sk.sd(q_)
                    if 0 < d_ < 0.015 and not sk.on_scalp(sk.project(q_.copy(), 0.0)):
                        sweep = True; break
        if not (hang_front or sweep):
            rej('not_front'); continue
        cand.append((lanes, resample(mid, 16)))
    # stacked duplicate strips (the lore cards carry 2-3 copies of many strips)
    strips = []
    for lanes, R in cand:
        if any(np.linalg.norm(R - R2, axis=1).mean() < a.dedupe_mm / 1000 for _, R2 in strips):
            rej('stacked_duplicate'); continue
        strips.append((lanes, R))

    new, roots, meta, faces = [], [], [], []
    if a.lock == 'lane':
        # every lane of the front strips, stacked copies dropped
        L1 = []
        for lanes, _ in strips:
            for Ln in lanes:
                R = resample(Ln, 16)
                if any(np.linalg.norm(R - R2, axis=1).mean() < a.dedupe_mm / 1000 for _, R2 in L1):
                    rej('stacked_lane'); continue
                L1.append((Ln, R))
        for Ln, _ in L1:
            side = 1 if Ln[0][0] - C[0] >= 0 else -1
            if (a.side == 'plus' and side < 0) or (a.side == 'minus' and side > 0):
                rej('side'); continue
            r = root_of(Ln)
            if r[0] is None:
                rej(r[1]); continue
            Ln2, R0, moved = r
            Gd = guide_of(Ln2, R0)
            if Gd is None:
                rej('short'); continue
            _, nR, _ = sk.sd(R0)
            d0 = Gd[min(2, len(Gd) - 1)] - Gd[0]
            d0 = d0 - nR * np.dot(d0, nR)
            d0 = d0 / max(np.linalg.norm(d0), 1e-9)          # lane direction on the scalp at the root
            t = np.cross(nR, d0); t = t / max(np.linalg.norm(t), 1e-9)   # sideways on the scalp
            w2 = a.lock_w_mm / 2000
            sl = arclen(Gd); L = sl[-1]
            sib = []
            for sg in (1, -1):
                Rs_ = sk.project(R0 + t * (sg * w2), 0.0)
                Q = Gd.copy()
                side_k = t.copy()
                for k in range(len(Q)):
                    tk = Gd[min(k + 1, len(Gd) - 1)] - Gd[max(k - 1, 0)]
                    tk = tk / max(np.linalg.norm(tk), 1e-9)
                    # parallel transport of the sideways vector along the lane: it never flips where the lane turns
                    # toward it (a flip crosses the lock's guides and the interpolated hair scribbles)
                    sk_ = side_k - tk * np.dot(side_k, tk)
                    if np.linalg.norm(sk_) > 1e-6:
                        side_k = sk_ / np.linalg.norm(sk_)
                    Q[k] = Q[k] + side_k * (sg * w2 * (1 + a.lock_fan * sl[k] / max(L, 1e-6)))
                Q[0] = Rs_
                sib.append(Q)
            Rb = sk.project(R0 - d0 * (a.lock_h_mm / 1000), 0.0)
            back = np.vstack([Rb[None], Gd])
            back = resample(back, K)
            lock = [sib[0], sib[1], back]
            bad = None
            for Q in lock:
                if not sk.on_scalp_margin(Q[0], a.root_margin_mm / 1000):
                    bad = 'root_off_scalp'; break
                r0 = Q[0].copy()
                Q[:] = keep_out(smooth(keep_out(Q), 2, 0.5)); Q[0] = r0
            if bad:
                rej(bad); continue
            if a.trim:
                # a lock that reaches the face box / an eye is cut just before it (kept when still min_len long)
                def blocked(q):
                    if q[1] > a.face_y and abs(q[0] - C[0]) < a.face_x and fz0 < q[2] < fz1:
                        return True
                    return any(np.linalg.norm(q - e) < a.eye_mm / 1000 for e in S['eyes'])
                kcut = min([k for Q in lock for k in range(len(Q)) if blocked(Q[k])] or [K])
                if kcut < K:
                    if min(arclen(Q[:kcut])[-1] if kcut > 1 else 0.0 for Q in lock) < a.min_len:
                        rej('trimmed_too_short'); continue
                    lock = [resample(Q[:kcut], K) for Q in lock]
                    Gd = lock[2]
                    rej('trimmed')
            # the interpolated hair blends the 3 guides: test the blends too (pairs and centre), not only the guides
            Lk = np.array([resample(Q, K) for Q in lock])
            P = np.array(lock + [Gd] + [(Lk[0] + Lk[1]) / 2, (Lk[0] + Lk[2]) / 2, (Lk[1] + Lk[2]) / 2, Lk.mean(0)])
            if ((P[..., 1] > a.face_y) & (np.abs(P[..., 0] - C[0]) < a.face_x) & (P[..., 2] > fz0) & (P[..., 2] < fz1)).any():
                rej('over_face'); continue
            if S['eyes'] and min(np.linalg.norm(P.reshape(-1, 3) - e, axis=1).min() for e in S['eyes']) < a.eye_mm / 1000:
                rej('near_eye'); continue
            Rs = [Q[0] for Q in lock]
            if roots and min(np.linalg.norm(np.array(roots) - q, axis=1).min() for q in Rs) < a.min_root_mm / 1000:
                rej('root_gap'); continue
            if len(old) and min(np.linalg.norm(old - q, axis=1).min() for q in Rs) < a.min_root_mm / 1000:
                rej('on_old_root'); continue
            base_i = len(new)
            for Q in lock:
                new.append(resample(Q, K)); roots.append(Q[0])
                meta.append(dict(side=side, moved_mm=round(moved * 1000, 1), len=round(float(arclen(Q)[-1]), 3),
                                 tip=np.round(Q[-1], 3).tolist()))
            f3 = [base_i, base_i + 1, base_i + 2]
            P3 = np.array(Rs)
            nrm = np.cross(P3[1] - P3[0], P3[2] - P3[0])
            if np.dot(nrm, P3.mean(0) - C) < 0:
                f3 = [f3[0], f3[2], f3[1]]
            faces.append(f3)
            if len(faces) >= a.max_new:
                break
        strips_iter = []
    else:
        strips_iter = strips
    for lanes, _ in strips_iter:
        side = 1 if lanes[len(lanes) // 2][0][0] - C[0] >= 0 else -1
        if (a.side == 'plus' and side < 0) or (a.side == 'minus' and side > 0):
            rej('side'); continue
        Gs, Rs, mv, bad = [], [], [], None
        for Ln in lanes:
            r = root_of(Ln)
            if r[0] is None:
                bad = r[1]; break
            Ln2, R0, moved = r
            # lanes of one strip moved to the same hairline point: spread them along the line between the lane tops
            for _ in range(4):
                if not Rs or min(np.linalg.norm(R0 - q) for q in Rs) >= a.min_root_mm / 1000:
                    break
                w = lanes[-1][0] - lanes[0][0]
                w = w / max(np.linalg.norm(w), 1e-9)
                R0 = sk.project(R0 + w * (a.min_root_mm / 1000), 0.0)
            if Rs and min(np.linalg.norm(R0 - q) for q in Rs) < a.min_root_mm / 1000:
                bad = 'roots_coincide'; break
            if not sk.on_scalp(R0):
                bad = 'root_off_scalp'; break
            Gd = guide_of(Ln2, R0)
            if Gd is None:
                bad = 'short'; break
            Gs.append(Gd); Rs.append(R0); mv.append(moved)
        if bad:
            rej(bad); continue
        P = np.array(Gs)
        # face box / eyes
        if ((P[..., 1] > a.face_y) & (np.abs(P[..., 0] - C[0]) < a.face_x) & (P[..., 2] > fz0) & (P[..., 2] < fz1)).any():
            rej('over_face'); continue
        if S['eyes'] and min(np.linalg.norm(P.reshape(-1, 3) - e, axis=1).min() for e in S['eyes']) < a.eye_mm / 1000:
            rej('near_eye'); continue
        # interpolated hairs (barycentric blends of the guides) must stay outside the head / neck
        inside = False
        for w in ((1 / 3, 1 / 3, 1 / 3), (.5, .5, 0), (.5, 0, .5), (0, .5, .5)):
            Hh = sum(P[i] * w[i] for i in range(min(3, len(P))))
            if any(-0.06 < sk.sd(Hh[k])[0] < -a.inside_mm / 1000 for k in range(2, len(Hh))):
                inside = True; break
        if inside:
            rej('blend_inside_head'); continue
        if roots and min(np.linalg.norm(np.array(roots) - q, axis=1).min() for q in Rs) < a.min_root_mm / 1000:
            rej('root_gap'); continue
        if len(old) and min(np.linalg.norm(old - q, axis=1).min() for q in Rs) < a.min_root_mm / 1000:
            rej('on_old_root'); continue
        base_i = len(new)
        for Gd, R0, moved in zip(Gs, Rs, mv):
            new.append(Gd); roots.append(R0)
            meta.append(dict(side=side, moved_mm=round(moved * 1000, 1), len=round(float(arclen(Gd)[-1]), 3),
                             tip=np.round(Gd[-1], 3).tolist()))
        f3 = list(range(base_i, base_i + len(Gs)))
        if len(f3) == 3:
            P3 = np.array(Rs)
            nrm = np.cross(P3[1] - P3[0], P3[2] - P3[0])
            if np.dot(nrm, P3.mean(0) - C) < 0:
                f3 = [f3[0], f3[2], f3[1]]
            faces.append(f3)
        if len(faces) >= a.max_new:
            break

    report = dict(cards_components=len(comps), strips_front=len(strips), locks=len(faces), guides=len(new), rejected=why,
                  roots_moved_to_hairline=sum(1 for m in meta if m['moved_mm'] > 0), K=K,
                  scalp=dict(skin=round(S['skin']), thr=round(S['thr'], 1), browZ=round(S['browZ'], 4)))
    if a.list or not new:
        report['lock_list'] = meta
        print(json.dumps(report, indent=1))
        return 0 if new else 1
    used = sorted({g for f in faces for g in f})
    if len(used) < len(new):
        report['dropped_no_face'] = len(new) - len(used)
    remap = {g: i for i, g in enumerate(used)}
    new = [new[g] for g in used]; meta = [meta[g] for g in used]
    faces = [[remap[g] for g in f] for f in faces]
    # base (skinning source) = nearest old guide root; uv = given or the old faces' typical uv
    base = []
    for Gd in new:
        b = int(np.argmin(np.linalg.norm(old - Gd[0], axis=1)))
        base.append([round(float(x), 4) for x in old[b]])
    if a.uv:
        uv = [float(x) for x in a.uv.split(',')]
    else:
        uvs = np.array([f['uv'] for f in G['faces']], float).reshape(-1, 2)
        uv = np.median(uvs, 0).tolist()
    op = {'op': 'rebuild', 'tool': 'front_locks', 'delete': [],
          'guides': [[[round(float(x), 4) for x in p] for p in Gd] for Gd in new],
          'base': base, 'uv': [uv for _ in new], 'faces': faces}
    with open(a.out, 'w', encoding='utf-8') as f:
        json.dump({'ops': [op]}, f)
    report.update(guides=len(new), faces=len(faces), uv=uv, out=a.out, lock_list=meta)
    print(json.dumps(report, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
