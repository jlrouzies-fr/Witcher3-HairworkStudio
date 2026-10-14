r"""collision - HairWorks collision spheres fitted to the CHARACTER'S OWN meshes (never Geralt's).

  col = fit(head, body_points, ear_points, bones, roots)
  head  : growth.Head (head mesh, landmarks)      body_points: (N,3) torso / shoulder vertices (body mesh)
  ear_points: (N,3) or None (ear mesh if the character has one, else the head mesh's ear region is used)
Spheres: up to 7 inscribed cranium spheres tangent to the root region (greedy cover), a 2-sphere neck capsule from
the head mesh's neck ring, 4 ear spheres per side enclosing the ear flap, 3 collar / shoulder spheres inscribed in the
body mesh. Positions are stored local to their bone (pos_local = (world - bind.pos) @ inv(bind.rot)).
"""
import numpy as np
from scipy.spatial import cKDTree


def _ray_hits(P, V, T, axis, sign):
    d = np.zeros(3); d[axis] = sign
    a, b, c = V[T[:, 0]], V[T[:, 1]], V[T[:, 2]]
    e1, e2 = b - a, c - a
    h = np.cross(d, e2)
    det = (e1 * h).sum(1)
    ok = np.abs(det) > 1e-12
    inv = np.where(ok, 1.0 / np.where(ok, det, 1), 0)
    out = np.zeros(len(P), int)
    for i, p in enumerate(P):
        s = p - a
        u = (s * h).sum(1) * inv
        q = np.cross(s, e1)
        v = (q * d).sum(1) * inv
        t = (e2 * q).sum(1) * inv
        out[i] = int((ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-6)).sum())
    return out


def _inside(P, V, T):
    votes = np.zeros(len(P), int)
    for axis, sign in ((0, 1), (0, -1), (1, 1), (1, -1), (2, 1)):
        votes += (_ray_hits(P, V, T, axis, sign) % 2 == 1)
    return votes >= 4


def head_spheres(head, roots, nmax=7, margin=0.002, rmin=0.025):
    hv, ht = head.v, head.t
    lo = np.array([-0.11, head.c[1] - 0.09, head.top - 0.27])
    hi = np.array([0.11, head.c[1] + 0.13, head.top])
    g = np.stack(np.meshgrid(*[np.arange(lo[k], hi[k], 0.006) for k in range(3)], indexing='ij'), -1).reshape(-1, 3)
    d0, _ = head.tree.query(g)
    g = g[d0 > rmin + margin]
    C = g[_inside(g, hv, ht)]
    R = head.tree.query(C)[0] - margin
    keep = R >= rmin
    C, R = C[keep], R[keep]
    tt = cKDTree(roots)
    covered = np.zeros(len(roots), bool)
    chosen = []
    for _ in range(nmax):
        best, bi = 0, None
        for i in range(len(C)):
            idx = tt.query_ball_point(C[i], R[i] + 0.007)
            if not idx:
                continue
            idx = np.array(idx)
            dd = np.linalg.norm(roots[idx] - C[i], axis=1)
            gain = int((~covered[idx[np.abs(dd - R[i]) < 0.007]]).sum())
            if gain > best:
                best, bi = gain, i
        if bi is None or best < 6:
            break
        idx = np.array(tt.query_ball_point(C[bi], R[bi] + 0.007))
        dd = np.linalg.norm(roots[idx] - C[bi], axis=1)
        covered[idx[np.abs(dd - R[bi]) < 0.007]] = True
        chosen.append((C[bi].copy(), float(R[bi])))
    return chosen


def face_sphere(head, pad=0.006, back=0.08):
    """2026-10-04 (HairLab L11, Shani): ONE sphere hugging the forehead / brow / eye band so fringe strands rest `pad`
    off the face instead of lying on the skin in game (vanilla cards float over the face). Centre = band centroid moved
    `back` metres into the head along -y; radius = 95th percentile distance of the band points + pad.
    Returns (centre, radius) or None."""
    V, c, top = head.v, head.c, head.top
    band = V[(V[:, 1] > c[1] + 0.02) & (V[:, 2] > top - 0.15) & (V[:, 2] < top - 0.05) & (np.abs(V[:, 0]) < 0.065)]
    if len(band) < 20:
        return None
    cen = band.mean(0) - np.array([0.0, back, 0.0])
    r = float(np.percentile(np.linalg.norm(band - cen, axis=1), 95))
    return cen, r + pad


def face_shell(head, r=0.022, spacing=0.016, pad=0.003, zlo=0.17, zhi=0.04, xmax=0.075, front_y=0.0):
    """2026-10-05 (user: "real collision spheres that 100 % stop hairs going through the head / eyes"): HairWorks only
    has spheres / capsules, so the face is covered by a SHELL of spheres whose surface lies on the skin: for points on
    the head surface in front of the crown centre (+ front_y), between top - zlo and top - zhi, |x| < xmax, a sphere of
    radius r centred r - pad behind the skin along the surface normal. Points are thinned to `spacing`.
    -> [(centre, radius)]"""
    V = head.v
    c = head.c
    sel = (V[:, 1] > c[1] + front_y) & (V[:, 2] < head.top - zhi) & (V[:, 2] > head.top - zlo) & (np.abs(V[:, 0]) < xmax)
    P = V[sel]
    if not len(P):
        return []
    sd, nrm, _ = head.signed(P)
    order = np.argsort(-P[:, 2])
    picked = []
    for i in order:
        if picked and min(np.linalg.norm(P[i] - P[j]) for j in picked) < spacing:
            continue
        picked.append(i)
    return [(P[i] - nrm[i] * (r - pad), r) for i in picked]


def neck_spheres(head, legacy=False):
    out = []
    last = None
    for dz in (0.28, 0.315):
        z = head.top - dz
        s = head.v[(head.v[:, 2] > z - 0.012) & (head.v[:, 2] < z + 0.012)]
        s = s[s[:, 1] < head.c[1] + 0.04]
        if len(s) < 8:
            # head mesh ends above this slice (wa / mb / ms / da / child heads). R36 fix: the old fallback sat at
            # y = crown centre + 2 cm (= BEHIND: the face is at -y), ~5 cm behind the neck axis with r 4.3 cm, a ball
            # sticking out of the nape that pushed the hair up (user, R36). Now: on the neck axis (the slice above, or
            # 3.5 cm in front of the crown centre, measured on the ma / ha / wa heads), r 3.5 cm (inside the neck).
            if legacy:      # approved builds made before the fix (vesemir r31a): exact old sphere
                out.append((np.array([0.0, head.c[1] + 0.02, z]), 0.043))
                continue
            cy = last[1] if last is not None else head.c[1] - 0.035
            out.append((np.array([0.0, cy, z]), 0.035))
            continue
        c = s[:, :2].mean(0)
        r = np.percentile(np.linalg.norm(s[:, :2] - c, axis=1), 25)
        out.append((np.array([c[0], c[1], z]), float(r) - 0.004))
        last = c
    return out


def ear_spheres(head, side, n=4, xmin=0.079, pad=0.004, pts=None, rmax=0.03):
    hv = head.v if pts is None else pts
    c = head.c
    sel = (side * hv[:, 0] > xmin) & (hv[:, 2] > head.top - 0.24) & (hv[:, 2] < head.top - 0.08) & \
          (hv[:, 1] > c[1] - 0.05) & (hv[:, 1] < c[1] + 0.06)
    E = hv[sel]
    if len(E) < 10:
        # fallback (R22, c04ma had 0 ear spheres: the fixed |x| > 7.9 cm missed a narrower head): the ear is the most
        # lateral part of the head in that height / depth band, so take the points within 1.8 cm of the band's widest x
        band = (side * (hv[:, 0] - c[0]) > 0.03) & (hv[:, 2] > head.top - 0.27) & (hv[:, 2] < head.top - 0.06) & \
               (hv[:, 1] > c[1] - 0.08) & (hv[:, 1] < c[1] + 0.08)
        if band.sum() < 10:
            print('  ear_spheres side %+d: no ear points even with the wide fallback band' % side)
            return []
        xs = side * (hv[band, 0] - c[0])
        E = hv[band][xs > xs.max() - 0.018]
        print('  ear_spheres side %+d: fallback band used (%d points, widest %.3f m from the head centre)' % (side, len(E), xs.max()))
        if len(E) < 10:
            return []
    zlo, zhi = np.percentile(E[:, 2], [2, 98])
    out = []
    edges = np.linspace(zlo, zhi, n + 1)
    for i in range(n):
        m = (E[:, 2] >= edges[i] - 0.004) & (E[:, 2] <= edges[i + 1] + 0.004)
        if m.sum() < 4:
            continue
        P = E[m]
        cc = P.mean(0)
        r = float(np.percentile(np.linalg.norm(P - cc, axis=1), 90)) + pad
        out.append((cc, min(max(r, 0.014), rmax)))
    return out


def torso_spheres(head, body):
    b = body[body[:, 2] > head.top - 0.6]
    tr = cKDTree(b)
    out = []
    z = head.top - 0.34
    for cc in ([0.12, head.c[1] - 0.05, z], [-0.12, head.c[1] - 0.05, z], [0.0, head.c[1] - 0.07, z - 0.01]):
        cc = np.array(cc)
        r = min(0.075, tr.query(cc)[0] - 0.003)
        if r > 0.02:
            out.append((cc, float(r)))
    return out


def fit(head, body, ears, bones, roots, pad=0.004, log=print, outfit=None, outfit_pad=0.002, ear_collision=True, legacy_neck=False,
        skull_spheres=7, ear_pad=0.004, ear_rmax=0.03, ear_chain=False, face=None, ear_colliders=True, shell=None,
        clamp_skin=None, mirror_ears=None):
    names = [b['name'] for b in bones]
    hs = head_spheres(head, roots, nmax=skull_spheres)
    if shell:                    # growth.face_shell: true or {r, spacing, pad, zlo, zhi, xmax, front_y}
        sh = face_shell(head, **(shell if isinstance(shell, dict) else {}))
        hs = hs + sh
        log('collision: face shell %d spheres' % len(sh))
    if face:                     # growth.face_sphere: pad (m), true, or {"pad", "back"}: see face_sphere()
        # L11 lesson: back 8 cm gave r 11 cm (wider than the head: the whole hair inflated into a ball); keep the
        # sphere small (back ~3 cm -> r ~5-6 cm) so it hugs the forehead only
        fd = face if isinstance(face, dict) else {}
        fs = face_sphere(head, pad=fd.get('pad', face if isinstance(face, float) else 0.006), back=fd.get('back', 0.03))
        if fs is not None:
            hs = hs + [fs]
            log('collision: face sphere c %s r %.3f' % (np.round(fs[0], 3), fs[1]))
    nk = neck_spheres(head, legacy=legacy_neck)
    es = []
    ear_runs = []
    for side in ((1, -1) if ear_collision else ()):   # R39: off for styles that cover the ears
        e_ = ear_spheres(head, side, pts=None if ears is None else np.concatenate([ears, head.v]), pad=ear_pad, rmax=ear_rmax)
        if ear_chain and e_:
            # R61 (user: Emhyr's ear spheres are far too big and push strands out): the ear as a FLAT OVAL = a chain of
            # small spheres (radius <= ear_rmax) sunk toward the head by half their radius, joined into capsules
            # top-to-bottom, instead of fat separate balls
            c0 = head.c
            e_ = [(cc - np.array([np.sign(cc[0]) * r * 0.5, 0.0, 0.0]), r) for cc, r in e_]
        ear_runs.append((len(es), len(e_)))
        es += e_
    if mirror_ears and ear_collision:
        # 2026-10-10 opt-in growth.mirror_ears {"source": "+x", "plane_x": "auto" | x, "offset": [dx, dy, dz]} (Vesemir L230 / L232): the target side's
        # ear spheres are replaced by the source side's reflected about x = plane_x. With strand_edits' mirror op the
        # mirrored hair then meets the same colliders as on the source side (Vesemir's auto-fitted left ear spheres
        # were bigger and pushed the mirrored strands out over the ear).
        me = mirror_ears if isinstance(mirror_ears, dict) else {'source': str(mirror_ears)}
        sg = -1.0 if str(me.get('source', '+x')).startswith('-') else 1.0
        px = me.get('plane_x', 'auto')
        if px in (None, 'auto'):
            import strand_edits
            px = strand_edits.mirror_plane(head)
        px = float(px)
        src = [(c, r) for c, r in es if (c[0] - px) * sg > 0]
        off = np.asarray(me.get('offset', [0.0, 0.0, 0.0]), float)     # e.g. the mirror op's ear_shift T (target ear vs reflected source ear)
        es = src + [(np.array([2 * px - c[0], c[1], c[2]]) + off, r) for c, r in src]
        ear_runs = [(0, len(src)), (len(src), len(src))]
        log('collision: mirror_ears %s about x = %.4f: %d ear spheres per side' % (me.get('source', '+x'), px, len(src)))
    ts = torso_spheres(head, body) if (outfit is None and body is not None and len(body)) else []

    def bone_for(c):
        if c[0] > 0.05 and 'r_shoulder' in names:
            return 'r_shoulder'
        if c[0] < -0.05 and 'l_shoulder' in names:
            return 'l_shoulder'
        return 'neck' if 'neck' in names else 'head'
    items = [('head', c, r) for c, r in hs] + [('neck' if 'neck' in names else 'head', c, r) for c, r in nk] + \
            ([('head', c, r) for c, r in es] if ear_colliders else []) + [(bone_for(c), c, r) for c, r in ts]   # ear_colliders False (2026-10-05): ear spheres only drop guides on the ears, no push in game
    col = dict(pos=[], idx=[], rad=[], cap=[], world=[], outfit_flag=[])
    for bn, c, r in items:
        j = names.index(bn)
        B = np.array(bones[j]['bind'])
        col['pos'].append(list((np.array(c) - B[3, :3]) @ np.linalg.inv(B[:3, :3])))
        col['idx'].append(j)
        col['rad'].append(r + pad)
        col['world'].append((np.array(c), r + pad))
        col['outfit_flag'].append(False)
    col['cap'] = [len(hs), len(hs) + 1]
    if outfit:
        shapes, caps = outfit
        base = len(col['pos'])
        for bn, c, r in shapes:
            if bn not in names:
                bn = 'torso3' if 'torso3' in names else names[0]
            j = names.index(bn)
            B = np.array(bones[j]['bind'])
            col['pos'].append(list((np.array(c) - B[3, :3]) @ np.linalg.inv(B[:3, :3])))
            col['idx'].append(j)
            col['rad'].append(r + outfit_pad)
            col['world'].append((np.array(c), r + outfit_pad))
            col['outfit_flag'].append(True)
        for i, j in caps:
            col['cap'] += [base + i, base + j]
        col['outfit_world'] = [(np.array(c), r + outfit_pad) for _b, c, r in shapes]
        col['outfit_caps'] = list(caps)
    col['ears'] = [(np.array(c), r) for c, r in es]
    if ear_chain:
        base_e = len(hs) + len(nk)
        for st, n in ear_runs:
            for j in range(n - 1):
                col['cap'] += [base_e + st + j, base_e + st + j + 1]
    # HairWorks collides with CAPSULES only (sphere pairs in boneCapsuleIndices): every vanilla redfur has unused
    # spheres and no (i, i) pair. Up to R2 only the neck capsule was live. Every sphere not yet in a capsule gets a
    # twin 1 mm away on the same bone, and the pair is a capsule (= that sphere).
    in_cap = set(col['cap'])
    for i in range(len(col['rad'])):
        if i in in_cap:
            continue
        k = len(col['rad'])
        col['pos'].append(list(np.array(col['pos'][i]) + np.array([0.001, 0.0, 0.0])))
        col['idx'].append(col['idx'][i])
        col['rad'].append(col['rad'][i])
        col['world'].append(col['world'][i])
        col['outfit_flag'].append(col['outfit_flag'][i])
        col['cap'] += [i, k]
    if clamp_skin:
        clamp_to_skin(col, head, max_out=clamp_skin if isinstance(clamp_skin, float) else 0.003, log=log)
    log('collision: %d cranium, %d neck, %d ear, %d torso spheres, %d outfit spheres, %d capsules' % (
        len(hs), len(nk), len(es), len(ts), len(outfit[0]) if outfit else 0, len(col['cap']) // 2))
    return col


def clamp_to_skin(col, head, max_out=0.003, zlow=0.26, rmin=0.006, iters=6, log=print):
    """2026-10-05 (user: where a collision sphere sticks out of the skin into view, the hair BEHIND it is not drawn -
    see-through patches at Guillaume's jaw, Keira's cheeks, Lambert's front top, Sheala's sides; measured bulge 7-34 mm).
    Every capsule (pair in col['cap']) is sampled; where its surface lies more than max_out outside the head mesh
    (only above head.top - zlow, i.e. head and jaw, not the shoulders) both radii shrink by the excess (>= rmin)."""
    rng = np.random.default_rng(1)
    dirs = rng.normal(size=(80, 3))
    dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
    cap = col['cap']
    pairs = [(cap[i], cap[i + 1]) for i in range(0, len(cap), 2)]
    shrunk = 0
    for _ in range(iters):
        changed = False
        for a, b in pairs:
            if col['outfit_flag'][a] or col['outfit_flag'][b]:
                continue      # outfit colliders legitimately stand outside the skin (collar)
            ca, cb = np.array(col['world'][a][0]), np.array(col['world'][b][0])
            ra, rb = col['rad'][a], col['rad'][b]
            P = []
            for t in np.linspace(0, 1, 7):
                P.append(ca * (1 - t) + cb * t + dirs * (ra * (1 - t) + rb * t))
            P = np.concatenate(P)
            P = P[P[:, 2] > head.top - zlow]
            if not len(P):
                continue
            ex = float(head.signed(P)[0].max()) - max_out
            if ex > 0.0005:
                for k in (a, b):
                    nr = max(rmin, col['rad'][k] - ex)
                    if nr < col['rad'][k] - 1e-6:
                        col['rad'][k] = nr
                        col['world'][k] = (col['world'][k][0], nr)
                        changed = True
                shrunk += 1
        if not changed:
            break
    log('collision: clamp_to_skin %d capsule shrinks (surface <= %.0f mm outside the skin above top - %.0f cm)' % (
        shrunk, max_out * 1000, zlow * 100))


def push_out(S, col, margin=0.002):
    for s in S:
        p = s['p']
        for k in range(1, len(p)):
            for _ in range(3):
                for c, r in col['world']:
                    d = p[k] - c
                    n = np.linalg.norm(d)
                    if n < r + margin:
                        p[k] = c + d / max(n, 1e-6) * (r + margin)


# ------------------------------------------------------------------------------------------ outfit (armour) colliders
def outfit_points(meshes, skip_bad=False, log=print):
    """LOD0 vertices of the outfit meshes with vertex normals and the name of the bone that dominates each
    vertex's skin weights (mesh bone table from the CMesh custom data)."""
    import gamefiles as G
    P, N, Bn = [], [], []
    for rel in meshes:
        try:
            m = G.load_mesh(rel)
            names = G.mesh_bones(rel)['names']
        except Exception as ex:
            if not skip_bad:
                raise
            log('  outfit mesh %s skipped (%r)' % (rel, ex))       # opt-in outfit_mods: some mod meshes are 4-vertex stubs
            continue
        for ci in m['lod0']:
            ch = m['chunks'][ci]
            v = ch['pos']
            tri = ch['idx'].reshape(-1, 3)
            fn = np.cross(v[tri[:, 1]] - v[tri[:, 0]], v[tri[:, 2]] - v[tri[:, 0]])
            nrm = np.zeros_like(v)
            for k in range(3):
                np.add.at(nrm, tri[:, k], fn)
            nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
            used = np.unique(tri)
            dom = ch['bi'][np.arange(ch['nv']), np.argmax(ch['bw'], 1)]
            P.append(v[used]); N.append(nrm[used])
            Bn += [names[d] if d < len(names) else 'torso3' for d in dom[used]]
    return np.concatenate(P), np.concatenate(N), np.array(Bn)


def outfit_shapes(head, meshes, region=None, radii=(0.015, 0.02, 0.03, 0.045, 0.06), nmax=36, band=0.008, tol=0.006,
                  min_gain=6, log=print, hair_pts=None, reach=0.08, skip_bad=False):
    """Spheres inscribed in the outfit surface (the armour / collar / pauldrons the hair falls on), each bound to the
    bone that dominates the skin of the vertices it touches, plus capsules between neighbouring spheres that stay
    inside the surface. Returns [(bone name, centre, radius)], [(i, j)] capsule pairs (indices into the list)."""
    P, N, Bn = outfit_points(meshes, skip_bad=skip_bad, log=log)
    c = head.c
    z0, z1 = head.top - 0.62, head.top - 0.17
    sel = (P[:, 2] > z0) & (P[:, 2] < z1) & (P[:, 1] < c[1] + 0.03) & (np.abs(P[:, 0]) < 0.28)
    if region is not None:
        sel &= region(P)
    P, N, Bn = P[sel], N[sel], Bn[sel]
    if len(P) < 50:
        return [], []
    # where the hair can actually touch: outfit vertices within `reach` of a guide point (hanging tips swing, so
    # generous) are the cover TARGETS; all outfit vertices still constrain the inscribed test below
    target = np.ones(len(P), bool)
    if hair_pts is not None and len(hair_pts):
        target = cKDTree(hair_pts).query(P)[0] < reach
    # outward = away from the body's vertical axis (x = 0, y = median y)
    axis = np.array([0.0, float(np.median(P[:, 1]))])
    out = np.stack([P[:, 0] - axis[0], P[:, 1] - axis[1]], 1)
    flip = (N[:, 0] * out[:, 0] + N[:, 1] * out[:, 1]) < 0
    N[flip] *= -1
    tr = cKDTree(P)
    # candidates on a 1.5 cm voxel subsample
    key = np.floor(P / 0.015).astype(int)
    _, first = np.unique(key, axis=0, return_index=True)
    C, R = [], []
    xy = cKDTree(P[:, :2])
    for i in first:
        for r in radii:
            cc = P[i] - N[i] * r
            if tr.query(cc)[0] < r - tol:                  # an outfit point sticks into the sphere
                continue
            if head.tree.query(cc)[0] < r - 0.004:         # cuts into the head / neck skin (collar opening, neck hole)
                continue
            # must sit UNDER the outfit: the sphere top may not rise above the outfit surface around it (spheres in
            # empty space next to a collar edge passed the test above and stuck out above the shoulders)
            near = xy.query_ball_point(cc[:2], max(r * 0.7, 0.015))
            if not near or cc[2] + r > P[near, 2].max() + 0.008:
                continue
            C.append(cc); R.append(r)
    if not C:
        return [], []
    C, R = np.array(C), np.array(R)
    covered = ~target                       # non-targets count as already covered (no gain)
    chosen = []
    for _ in range(nmax):
        best, bi, bidx = 0, None, None
        for k in range(len(C)):
            idx = np.array(tr.query_ball_point(C[k], R[k] + band))
            if not len(idx):
                continue
            d = np.linalg.norm(P[idx] - C[k], axis=1)
            on = idx[np.abs(d - R[k]) < band]
            g = int((~covered[on]).sum())
            if g > best:
                best, bi, bidx = g, k, on
        if bi is None or best < min_gain:
            break
        covered[bidx] = True
        names, cnt = np.unique(Bn[bidx], return_counts=True)
        chosen.append((str(names[np.argmax(cnt)]), C[bi].copy(), float(R[bi])))
    # capsules: each sphere to its nearest neighbour if the capsule's middle stays inside the surface
    caps = set()
    for i, (_b, ci, ri) in enumerate(chosen):
        d = [(np.linalg.norm(cj - ci), j) for j, (_bb, cj, rj) in enumerate(chosen) if j != i]
        for dist, j in sorted(d)[:2]:
            rj = chosen[j][2]
            if dist > ri + rj + 0.04:
                continue
            m = (ci + chosen[j][1]) / 2
            if tr.query(m)[0] >= (ri + rj) / 2 - tol:
                caps.add((min(i, j), max(i, j)))
    log('outfit colliders: %d spheres (%s), %d capsules, %.0f %% of %d outfit vertices within reach of the hair touched' % (
        len(chosen), ', '.join('%s:%d' % (n, sum(1 for b, _, _ in chosen if b == n)) for n in sorted({b for b, _, _ in chosen})),
        len(caps), 100 * (covered & target).sum() / max(target.sum(), 1), int(target.sum())))
    return chosen, sorted(caps)
