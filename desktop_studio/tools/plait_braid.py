"""plait_braid.py geo.json desk.json out_ops.json [params json]

Three-strand plait made of guides (Cerys an Craite pass cerys1, 2026-10-15). Studio-side helper, no Studio C++ change.
Input: `studio_dump_geo` of the current state (after rebuild_from_cards / fill_scalp and `studio_resample {n:32}`) and
the character's desk.json (head mesh + vanilla hair cards). Output: Studio `reshape` ops (absolute points, same K, roots
unchanged) for `studio_ops {file}`.

Every guide keeps its own path over the head to the nape (cut where it passes below tie_z + 12 mm at the back; side /
back roots that sweep forward over the face are cut before they turn forward), then walks to the braid tie and follows
one of three plait bundles down the vanilla braid's centre line (median x / y of the card vertices with y < -0.06 per
1 cm of height, smoothed; straight down below the cards). Bundle k (roots split in thirds by x):
    C(s) + a sin(th + phi_k) X + d sin(2 (th + phi_k)) Y + rb (U X + W Y),   th = 2 pi s / period, phi_k = 0, 2pi/3, 4pi/3
X = character +x across the braid, Y = out from the back, (U, W) = the guide's place inside its bundle (root rank ->
disk). Amplitudes fade into the tie at the braid end (tail), then a short tuft flares (tuft, tuft_flare).
Head paths are wrapped around the skull: points clearly under the skin (> 4 mm) above the ears step out radially until
`standoff` above it (max 6 cm; beside / below the ears the test is skipped, it pushed hair into side "wings"). After the
ops, delete the few guides that still pass through the head (studio_push_out list_only: inside, or span dip > 12 mm).
Limit: 32 CVs per guide (kMaxCvs) -> about 4 CVs per half period, so period >= ~0.12 m with n_head 9.

Cerys final params (cerys_mcp_2): {"period":0.12,"n_head":9,"a0":0.016,"a1":0.008,"rb0":0.009,"rb1":0.005,
"len_var":0.05,"end_z":1.18,"tuft":0.05,"tail":0.03}  (card c_16_wa__cerys, tie_z 1.585, standoff 0.004).
"""
import json, sys, math
import numpy as np
from scipy.spatial import cKDTree

geo = json.load(open(sys.argv[1])); desk = json.load(open(sys.argv[2])); out = sys.argv[3]
P_ = dict(period=0.16, a0=0.013, a1=0.006, d_ratio=0.45, rb0=0.0075, rb1=0.004, n_head=8, tie_z=1.585,
          end_z=1.20, len_var=0.18, standoff=0.004, card='c_16_wa__cerys', bundle_split='x', tail=0.04,
          tie_dx=0.0, tie_dy=0.0, tuft=0.0, tuft_flare=1.6, seed=7, cut_y=-0.03, gather=0.0, wrap_x=0.06, wrap_z=1.70)
if len(sys.argv) > 4:
    P_.update(json.loads(sys.argv[4]))
K = geo['K']
G = np.array(geo['guides'], float)            # (n, K, 3) edit frame
n = len(G)
rng = np.random.default_rng(P_['seed'])

# ---- head mesh (skull clearance) ----
hm = [m for m in desk['meshes'] if m.get('kind') == 'head'][0]
HV = np.array(hm['v'], float).reshape(-1, 3); HT = np.array(hm['t'], int).reshape(-1, 3)
HN = np.zeros_like(HV)
fn = np.cross(HV[HT[:, 1]] - HV[HT[:, 0]], HV[HT[:, 2]] - HV[HT[:, 0]])
for j in range(3):
    np.add.at(HN, HT[:, j], fn)
HN /= np.linalg.norm(HN, axis=1, keepdims=True) + 1e-12
hc = np.array(desk['head']['c'], float)
flip = np.sum((HV - hc) * HN, axis=1) < 0      # make normals point out of the head
HN[flip] *= -1
tree = cKDTree(HV)


def sdist(p):
    d, j = tree.query(p, k=6)
    return np.array([min(np.dot(p[i] - HV[jj], HN[jj]) for jj in j[i]) for i in range(len(p))])


def wrap_out(p, standoff):
    """radial projection from the head centre: points under the skin (any depth) step outward until standoff above it"""
    q = p.copy()
    for i in range(len(q)):
        r = q[i] - hc; rn = np.linalg.norm(r)
        if rn > 0.16 or rn < 1e-6:
            continue
        if abs(q[i, 0]) > P_['wrap_x'] and q[i, 2] < P_['wrap_z']:   # beside / behind the ears: sdist unreliable
            continue
        u = r / rn
        if sdist(q[i:i+1])[0] > -0.004:          # only points clearly under the skin
            continue
        p0 = q[i].copy()
        for _ in range(30):                        # at most 6 cm
            q[i] = q[i] + 0.002 * u
            if sdist(q[i:i+1])[0] >= standoff:
                break
        else:
            q[i] = p0
    return q


def push_out(p, standoff):
    d, j = tree.query(p, k=6)
    out_p = p.copy()
    for i in range(len(p)):
        best = None
        for jj in j[i]:
            s = np.dot(p[i] - HV[jj], HN[jj])
            if best is None or s < best[0]:
                best = (s, jj)
        # only when the point is near the surface of the head (within 4 cm)
        if d[i][0] < 0.04 and best[0] < standoff:
            jj = j[i][0]
            s = np.dot(out_p[i] - HV[jj], HN[jj])
            out_p[i] = out_p[i] + (standoff - s) * HN[jj]
    return out_p


# ---- braid centre line from the cards ----
card = [c for c in desk['hair_cards'] if c['name'] == P_['card']][0]
V = np.array(card['v'], float).reshape(-1, 3)
zs = np.arange(P_['tie_z'], P_['end_z'] - 1e-4, -0.01)
C = []
for z in zs:
    m = (V[:, 2] <= z + 0.008) & (V[:, 2] > z - 0.008) & (V[:, 1] < -0.06)
    if m.sum() < 5:
        m = (V[:, 2] <= z + 0.02) & (V[:, 2] > z - 0.02) & (V[:, 1] < -0.06)
    if m.sum() == 0:                                  # below the cards: continue straight down
        C.append([C[-1][0], C[-1][1], z]); continue
    C.append([np.median(V[m, 0]), np.median(V[m, 1]), z])
C = np.array(C)
# smooth (keep ends)
for _ in range(6):
    C[1:-1, :2] = 0.25 * C[:-2, :2] + 0.5 * C[1:-1, :2] + 0.25 * C[2:, :2]
C[:, 0] += P_['tie_dx']; C[:, 1] += P_['tie_dy']
seg = np.linalg.norm(np.diff(C, axis=0), axis=1); S = np.concatenate([[0], np.cumsum(seg)]); Lb = S[-1]


def centre(s):
    s = np.clip(s, 0, Lb)
    return np.array([np.interp(s, S, C[:, k]) for k in range(3)]).T


def frame(s):
    e = 0.005
    t = centre(np.minimum(s + e, Lb)) - centre(np.maximum(s - e, 0))
    t /= np.linalg.norm(t, axis=-1, keepdims=True)
    X = np.array([1.0, 0, 0]) - t * t[..., :1]
    X /= np.linalg.norm(X, axis=-1, keepdims=True)
    Y = np.cross(t, X)
    return t, X, Y


# ---- bundles ----
R = G[:, 0]
key = R[:, 0]
order = np.argsort(key)
bundle = np.zeros(n, int)
thirds = np.array_split(order, 3)
bundle[thirds[0]] = 2      # character right (-x) -> phase 4pi/3 (x = -0.87 a)
bundle[thirds[1]] = 0      # centre
bundle[thirds[2]] = 1      # character left (+x) -> phase 2pi/3
phi = np.array([0.0, 2 * math.pi / 3, 4 * math.pi / 3])
# disk offset inside the bundle by root rank (x across, z down)
U = np.zeros(n); W = np.zeros(n)
for b in range(3):
    ids = np.where(bundle == b)[0]
    rx = np.argsort(np.argsort(R[ids, 0])) / max(1, len(ids) - 1) * 2 - 1
    rz = np.argsort(np.argsort(-R[ids, 2] + 0.3 * R[ids, 1])) / max(1, len(ids) - 1) * 2 - 1
    U[ids] = rx * np.sqrt(1 - rz ** 2 / 2); W[ids] = rz * np.sqrt(1 - rx ** 2 / 2)


def resample(poly, m):
    d = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(poly, axis=0), axis=1))])
    if d[-1] < 1e-6:
        return np.repeat(poly[:1], m, axis=0)
    t = np.linspace(0, d[-1], m)
    return np.array([np.interp(t, d, poly[:, k]) for k in range(3)]).T


ops = []
nb = K - P_['n_head']
lens = []
for i in range(n):
    p = G[i]
    b = bundle[i]
    Lg = Lb * (1 - P_['len_var'] * rng.random())
    s = np.linspace(0, Lg, nb)                              # s[0] = tie entry
    th = 2 * math.pi * s / P_['period']
    f = np.clip(s / Lb, 0, 1)
    a = P_['a0'] + (P_['a1'] - P_['a0']) * f
    Lp = Lb - P_['tuft']                                     # plait part ends at the tie
    tailf = np.clip((Lp - s) / P_['tail'], 0, 1)            # amplitudes fade to 0 into the tie
    a = a * tailf
    d = a * P_['d_ratio']
    rb = P_['rb0'] + (P_['rb1'] - P_['rb0']) * np.clip(s / Lp, 0, 1)
    if P_['tuft'] > 0:                                       # gathered at the tie, then a small flared tuft
        tie_w = np.clip(1 - np.abs(s - Lp) / 0.012, 0, 1)
        tu = np.clip((s - Lp) / P_['tuft'], 0, 1)
        rb = rb * (1 - 0.45 * tie_w) * (1 + (P_['tuft_flare'] - 1) * tu)
    t, X, Y = frame(s)
    c = centre(s)
    bp = c + (a * np.sin(th + phi[b]))[:, None] * X + (d * np.sin(2 * (th + phi[b])))[:, None] * Y
    bp = bp + (rb * U[i])[:, None] * X + (rb * W[i])[:, None] * Y
    entry = bp[0]
    # head path: the strand until it passes below the tie height at the back (or its tip)
    cut = K
    for k in range(1, K):
        if p[k, 2] < P_['tie_z'] + 0.012 and p[k, 1] < P_['cut_y']:
            cut = k
            break
    head = p[:cut]
    # side / back roots whose strand sweeps forward over the face: keep only the part before it turns forward
    if p[0, 1] < 0.07:
        fw = np.where(head[:, 1] > max(p[0, 1], 0.0) + 0.02)[0]
        if len(fw):
            head = head[:max(2, fw[0])]
    # approach: from the last head point to the entry, bent around the skull
    q = head[-1]
    m_ap = 6
    ap = np.array([q + (entry - q) * (j / m_ap) for j in range(1, m_ap + 1)])
    path = resample(np.vstack([head, ap]), 60)
    for _ in range(3):                                # wrap around the skull, smooth, repeat
        path[1:] = wrap_out(path[1:], P_['standoff'] + 0.002)
        path[1:-1] = 0.25 * path[:-2] + 0.5 * path[1:-1] + 0.25 * path[2:]
    path[1:] = wrap_out(path[1:], P_['standoff'] + 0.002)
    hp = resample(path, P_['n_head'] + 1)          # last point = entry
    pts = np.vstack([hp[:-1], bp])
    if len(pts) < K:
        pts = np.vstack([pts, np.repeat(pts[-1:], K - len(pts), axis=0)])
    pts[1:] = push_out(pts[1:], P_['standoff'])
    pts[1:P_['n_head']+1] = wrap_out(pts[1:P_['n_head']+1], P_['standoff'])
    pts[0] = p[0]
    lens.append(np.linalg.norm(np.diff(pts, axis=0), axis=1).sum())
    ops.append({"type": "reshape", "root": [round(float(v), 5) for v in p[0]],
                "points": [[round(float(v), 5) for v in q_] for q_ in pts]})

json.dump({"ops": ops}, open(out, 'w'))
print('guides', n, 'bundles', np.bincount(bundle), 'braid length %.3f' % Lb, 'K', K,
      'strand length p5/50/95', np.percentile(lens, [5, 50, 95]).round(3))
print('centre', C[::5].round(3).tolist())
