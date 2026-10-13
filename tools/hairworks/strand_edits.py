r"""strand_edits - hand edits of individual HairWorks guide strands (user tool, R59+).

Flow:  build -> build\hairbatch\edit\<cfg>.json (EXPORT: final guides + growth faces + head / hat / outfit meshes)
       -> results\<round>\<cfg>_EDIT.json (lab/hairlab.py) -> "Strand Editor" artifact (user moves / lowers / shortens /
       deletes strands) -> edits JSON -> tools\hairworks\edits\<character>.json -> next build APPLIES it.

Edits file (version 1):
  {"version": 1, "character": "dandelion", "source": "dandelion_v14", "tol": 0.004, "push_out": false,
   "ops": [
     {"type": "translate", "roots": [[x,y,z], ...], "offset": [dx,dy,dz], "falloff": "carry", "from": 0},
     {"type": "set_point", "root": [x,y,z], "index": k, "pos": [x,y,z], "carry": true},
     {"type": "shorten", "roots": [[x,y,z], ...], "keep": 0.7},
     {"type": "delete", "roots": [[x,y,z], ...]},
     {"op": "duplicate", "root": [x,y,z], "d": [dx,dy,dz], "id": 3},
     {"type": "reshape", "root": [x,y,z], "points": [[x,y,z] x K]},
     {"type": "mirror", "source": "+x", "plane_x": "auto", "band": 0.015, "ymin": -0.09}]}
Guides are identified by ROOT POSITION (metres, the game's mesh space), never by index: each listed root matches the
nearest current guide root within `tol` (default 4 mm). Ops run IN ORDER and each op's roots are the roots as they are
when that op runs (after a root-moving translate the next op lists the moved roots) - so two edit files concatenate.
  translate falloff: "carry"  points k >= from move by the full offset (from 0 = the whole strand, root included)
                     "linear" point k moves by offset * k / (K-1) (root fixed, tip full: the strand bends)
                     "tip"    like linear but quadratic (only the lower part really moves)
  set_point: point `index` goes to `pos`; carry = the points below it move by the same delta.
  shorten:   the strand is cut at `keep` x its length and resampled back to K points (same CV count).
  reshape:   (2026-10-10, Desktop Groom Studio) the guide matched by `root` takes `points` (absolute, same K): the
             baked result of the desktop curl / roll / wave tools. Only present in edit files that use those tools.
  mirror:    (2026-10-10, opt-in, no roots) rebuilds one side of the head from the other. Guides whose ROOT is more
             than `band` (default .015) from the mid-sagittal plane x = plane_x on the TARGET side are deleted; the
             guides on the `source` side ("+x" or "-x") are copied, reflected x -> 2 plane_x - x (whole strand), their
             roots snapped onto the scalp keeping the source root's offset from the skin (whole strand shifted: the
             segment lengths are kept), and points that end up under the skin (< `clear`, default .002) are pushed out
             along the normal with the segment lengths restored from the root. The centre band stays as it is (no seam,
             no doubled strands). plane_x: a number, or "auto" (default; needs the head mesh: the x that best maps the
             cranium, z > top - .12, onto itself; 0 without a head). Optional root limits ymin / ymax / zmin / zmax
             (mesh space, before reflection; e.g. ymin keeps the back / tie region as it is). Opt-in `conform` (m, e.g.
             .03; needs the head): surface warp for asymmetric heads / ears - a point near the skin moves by the offset
             between the reflected source-side head vertex and the target side's nearest vertex (fades out at
             `conform`), so the copy follows the target side's own ear / skull shape. Opt-in `lateral` {cell, min_lat,
             max_off} (needs the head): a mirrored point keeps the source point's lateral offset from the head's OUTER
             surface (height field |x - plane| over y, z), so hair behind one ear flap stays behind the other, flatter
             ear (Vesemir L232). Opt-in `ear_shift` {min_lat, zlo, zhi, radius, fade}: points near the target ear
             move by the ICP translation between the reflected source ear and the target ear. Copies keep the source's
             growth attributes (like duplicate) and are appended; later ops can target them by root.
  delete:    the guide is removed and the growth mesh re-triangulated (growth.reindex).
  duplicate: ("op" or "type") the guide matched by `root` is COPIED (same shape and growth attributes), the copy is
             translated by `d` and its root snapped back onto the scalp along the head normal (like
             growth.extra_guides), and it joins the guide set at the END of the list; the growth mesh is
             re-triangulated so HairWorks interpolates around it. `id` is informational (editor numbering).
             Later ops reference the copy by its ROOT POSITION like any guide: a move / shorten / delete of a copy
             lists the copy's root (= source root + d, snapped), so NO special reference scheme is needed; ops are
             applied in order so the copy exists from the duplicate op onward. Exactly one copy per duplicate op.
"""
import json
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
EDIT_OUT = os.path.join(ROOT, 'build', 'hairbatch', 'edit')
EDITS_DIR = os.path.join(HERE, 'edits')


# ------------------------------------------------------------------------------------------------ apply
def _resample(q, K):
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(q, axis=0), axis=1))]
    if d[-1] < 1e-9:
        return np.repeat(q[:1], K, 0)
    t = np.linspace(0, d[-1], K)
    return np.stack([np.interp(t, d, q[:, c]) for c in range(3)], 1)


def shorten(p, keep):
    """strand cut at keep x its arc length, resampled back to len(p) points."""
    p = np.asarray(p, float)
    K = len(p)
    d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
    L = d[-1] * float(np.clip(keep, 0.02, 1.0))
    k = int(np.searchsorted(d, L))
    if k >= K:
        return p.copy()
    k = max(k, 1)
    t = (L - d[k - 1]) / max(d[k] - d[k - 1], 1e-12)
    q = np.vstack([p[:k], p[k - 1] + t * (p[k] - p[k - 1])])
    return _resample(q, K)


def weights(K, falloff='carry', frm=0):
    k = np.arange(K, dtype=float)
    if falloff == 'linear':
        w = k / max(K - 1, 1)
    elif falloff == 'tip':
        w = (k / max(K - 1, 1)) ** 2
    elif falloff == 'point':   # one CV (+ half on its neighbours), Studio point drag (2026-10-13)
        return np.clip(1.0 - np.abs(k - int(frm)) / 2.0, 0.0, None)
    else:
        w = np.ones(K)
    w[k < int(frm)] = 0.0
    return w


def mirror_plane(head):
    """x of the head's mid-sagittal plane: the x that maps the cranium vertices (z > top - .12) best onto themselves"""
    from scipy.spatial import cKDTree
    if head is None or getattr(head, 'v', None) is None:
        return 0.0
    v = np.asarray(head.v, float)
    cr = v[v[:, 2] > float(getattr(head, 'top', v[:, 2].max())) - 0.12]
    tr = cKDTree(cr)
    best = None
    for c in np.arange(-0.02, 0.02001, 0.0005):
        m = cr.copy(); m[:, 0] = 2 * c - m[:, 0]
        e = tr.query(m)[0].mean()
        if best is None or e < best[0]:
            best = (e, c)
    return float(best[1])


def _restore_lengths(q, seg):
    for k in range(1, len(q)):
        d = q[k] - q[k - 1]
        n = np.linalg.norm(d)
        if n > 1e-12:
            q[k] = q[k - 1] + d / n * seg[k - 1]
    return q


def _ear_shift(q, head, px, sgn, seg, o, op):
    """mirror op, opt-in `ear_shift` {min_lat .075, zlo .2, zhi .04, radius .03, fade .02}: the two ears of a head are
    not mirror images (Vesemir: the left ear sits ~5 mm flatter and further back). T = rigid translation (ICP) of the
    reflected source-side ear vertices (|x - plane| > min_lat, top - zlo < z < top - zhi) onto the target side's;
    mirrored points within `radius` of the target ear move by T (fading to 0 over `fade`). Root fixed, lengths kept."""
    from scipy.spatial import cKDTree
    if '_ear_T' not in op or not hasattr(head, '_ear_tree'):
        hv = np.asarray(head.v, float)
        top = float(getattr(head, 'top', hv[:, 2].max()))
        ml, zlo, zhi = o.get('min_lat', 0.075), o.get('zlo', 0.2), o.get('zhi', 0.04)
        band = (hv[:, 2] > top - zlo) & (hv[:, 2] < top - zhi)
        src = hv[band & ((hv[:, 0] - px) * sgn > ml)].copy()
        tgt = hv[band & ((hv[:, 0] - px) * -sgn > ml)]
        if len(src) < 10 or len(tgt) < 10:
            op['_ear_T'] = None
        else:
            src[:, 0] = 2 * px - src[:, 0]
            tt = cKDTree(tgt)
            T = tgt.mean(0) - src.mean(0)
            for _ in range(20):
                _, j = tt.query(src + T)
                T = T + np.mean(tgt[j] - (src + T), axis=0)
            op['_ear_T'] = [float(x) for x in T]
            head._ear_tree = tt
    if op.get('_ear_T') is None:
        return q
    T = np.asarray(op['_ear_T']); tt = head._ear_tree
    r, fd = o.get('radius', 0.03), o.get('fade', 0.02)
    d, _ = tt.query(q)
    w = np.clip(1.0 - (d - r) / fd, 0, 1)
    w[0] = 0
    q = q + w[:, None] * T
    return _restore_lengths(q, seg)


def _lateral_match(p, q, head, px, sgn, seg, o):
    """mirror op, opt-in `lateral` {cell .004, min_lat .05, max_off .03}: the head's OUTER surface as a height field
    of lateral distance |x - plane| over (y, z) (max over the vertices within `cell`), per side. A mirrored point keeps
    the source point's lateral offset from that outer surface: x moves by T(y, z) - S(y, z). Points behind an ear flap
    stay behind the other (flatter / shifted) ear instead of lying over it. Only points more than min_lat from the
    plane and within max_off (laterally) of the surface; root fixed; segment lengths restored."""
    from scipy.spatial import cKDTree
    cell, min_lat, max_off = o.get('cell', 0.004), o.get('min_lat', 0.05), o.get('max_off', 0.03)
    if not hasattr(head, '_lat_cache'):
        hv = np.asarray(head.v, float)
        sides = {}
        for sg_ in (1.0, -1.0):
            V = hv[(hv[:, 0] - px) * sg_ > min_lat - 0.02]
            sides[sg_] = (cKDTree(V[:, 1:]), (V[:, 0] - px) * sg_)
        head._lat_cache = sides
    ts, ls = head._lat_cache[sgn]
    tt, lt = head._lat_cache[-sgn]

    def surf(tree, lats, yz):
        idx = tree.query_ball_point(yz, cell)
        return np.array([lats[i].max() if len(i) else np.nan for i in idx])
    q = q.copy()
    lp = (p[:, 0] - px) * sgn
    S = surf(ts, ls, p[:, 1:]); T = surf(tt, lt, q[:, 1:])
    ok = (lp > min_lat) & np.isfinite(S) & np.isfinite(T) & (np.abs(lp - S) < max_off)
    ok[0] = False
    d = np.where(ok, T - S, 0.0)
    q[:, 0] += -sgn * d                       # target side lateral = -sgn direction
    return _restore_lengths(q, seg)


def mirror_side(P, alive, op, head=None):
    """the `mirror` op (see the module doc) -> ([(source index, mirrored (K,3) points)], [target indices to delete])"""
    px = op.get('plane_x', 'auto')
    px = mirror_plane(head) if px in (None, 'auto') else float(px)
    op['_plane_x'] = px                                      # for logs / the Studio
    sgn = -1.0 if str(op.get('source', '+x')).startswith('-') else 1.0
    band = float(op.get('band', 0.015))
    clear = float(op.get('clear', 0.002))

    def inside_limits(r):
        return all(lim is None or ok for lim, ok in (
            (op.get('ymin'), r[1] >= (op.get('ymin') or 0)), (op.get('ymax'), r[1] <= (op.get('ymax') or 0)),
            (op.get('zmin'), r[2] >= (op.get('zmin') or 0)), (op.get('zmax'), r[2] <= (op.get('zmax') or 0))))
    src, gone = [], []
    for i in np.flatnonzero(alive):
        r = P[i][0]
        off = (r[0] - px) * sgn
        if abs(off) <= band or not inside_limits(r):
            continue
        (src if off > 0 else gone).append(int(i))
    new = []
    for i in src:
        p = np.array(P[i], float)
        seg = np.linalg.norm(np.diff(p, axis=0), axis=1)
        q = p.copy(); q[:, 0] = 2 * px - q[:, 0]
        if head is not None:
            sd0 = head.signed(p[:1])[0][0]
            sd, nrm, _ = head.signed(q[:1])
            q = q - nrm[0] * (sd[0] - sd0)                   # root back onto the scalp, same offset as the source
            cf = float(op.get('conform') or 0)
            if cf > 0 and getattr(head, 'v', None) is not None:
                # opt-in surface warp: the two sides of a head (its ears!) are not exact mirrors, so a pure reflection
                # can run over the other ear. Each point near the skin (source point within `conform` of head vertex
                # j) moves by (target vertex nearest to reflect(v_j)) - reflect(v_j): the reflected strand follows the
                # target side's own shape. Weight fades to 0 at `conform`; displacements smoothed along the strand;
                # the root stays (snapped above); segment lengths restored.
                from scipy.spatial import cKDTree
                hv = np.asarray(head.v, float)
                if not hasattr(head, '_mirror_tree'):
                    head._mirror_tree = cKDTree(hv)
                tr = head._mirror_tree
                dj, j = tr.query(p)
                rv = hv[j].copy(); rv[:, 0] = 2 * px - rv[:, 0]
                _, kk = tr.query(rv)
                disp = (hv[kk] - rv) * np.clip(1.0 - dj / cf, 0, 1)[:, None]
                disp[0] = 0
                for _ in range(2):
                    disp[1:-1] = 0.25 * disp[:-2] + 0.5 * disp[1:-1] + 0.25 * disp[2:]
                q[1:] += disp[1:]
                q = _restore_lengths(q, seg)
            es = op.get('ear_shift')
            if es and getattr(head, 'v', None) is not None:
                q = _ear_shift(q, head, px, sgn, seg, es if isinstance(es, dict) else {}, op)
            lat = op.get('lateral')
            if lat and getattr(head, 'v', None) is not None:
                q = _lateral_match(p, q, head, px, sgn, seg, lat if isinstance(lat, dict) else {})
            sdk, nk, _ = head.signed(q[1:])
            low = sdk < clear
            if low.any():
                q[1:][low] += nk[low] * (clear - sdk[low])[:, None]
                q = _restore_lengths(q, seg)
        new.append((i, q))
    return new, gone


def export_head(D):
    """a head object (v, t, top, c, signed) from an EDIT export's 'head' mesh (as growth.Head), or None"""
    from scipy.spatial import cKDTree
    m = next((m for m in D.get('meshes', []) if m.get('kind') == 'head' or m.get('name') == 'head'), None)
    if m is None:
        return None
    v = np.asarray(m['v'], float); t = np.asarray(m['t'], int).reshape(-1, 3)
    c = np.asarray(D.get('head', {}).get('c', v.mean(0)), float)
    nrm = np.zeros_like(v)
    fn = np.cross(v[t[:, 1]] - v[t[:, 0]], v[t[:, 2]] - v[t[:, 0]])
    for k in range(3):
        np.add.at(nrm, t[:, k], fn)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
    nrm[((v - c) * nrm).sum(1) < 0] *= -1
    tree = cKDTree(v)

    class _H:
        pass
    h = _H(); h.v, h.t, h.c, h.n, h.top = v, t, c, nrm, float(v[:, 2].max())

    def signed(P_):
        d, i = tree.query(P_)
        return ((P_ - v[i]) * nrm[i]).sum(-1), nrm[i], d
    h.signed = signed
    return h


def apply_ops(P, ops, tol=0.004, head=None):
    """P: list of (K,3) arrays (guides root -> tip), modified copies returned. `head` (optional, has .signed) snaps the
    root of duplicated guides back onto the scalp. Duplicates are appended to P_new / alive.
    -> (P_new, alive mask, stats dict(ops, guides, unmatched, deleted, duplicated, dup_src = [(new index, source
    index)]))."""
    from scipy.spatial import cKDTree
    P = [np.array(p, float) for p in P]
    alive = np.ones(len(P), bool)
    touched = set()
    unmatched = 0
    n_ops = 0
    dup_src = []

    def match(roots):
        nonlocal unmatched
        idx = np.flatnonzero(alive)
        if not len(idx):
            unmatched += len(roots); return []
        R = np.array([P[i][0] for i in idx])
        d, j = cKDTree(R).query(np.asarray(roots, float).reshape(-1, 3))
        out = []
        for di, ji in zip(np.atleast_1d(d), np.atleast_1d(j)):
            if di <= tol:
                if idx[ji] not in out:
                    out.append(int(idx[ji]))
            else:
                unmatched += 1
        return out

    for op in ops:
        t = op.get('type') or op.get('op')
        if t == 'mirror':
            new, gone = mirror_side(P, alive, op, head)
            alive[gone] = False
            for src, q in new:
                P.append(q)
                alive = np.append(alive, True)
                dup_src.append((len(P) - 1, src))
            touched.update(range(len(P) - len(new), len(P)))
            n_ops += 1
            continue
        roots = op.get('roots')
        if roots is None and op.get('root') is not None:
            roots = [op['root']]
        if not roots:
            continue
        g = match(roots)
        if not g:
            continue
        n_ops += 1
        if t == 'translate':
            off = np.asarray(op.get('offset', [0, 0, 0]), float)
            for i in g:
                P[i] = P[i] + weights(len(P[i]), op.get('falloff', 'carry'), op.get('from', 0))[:, None] * off
        elif t == 'set_point':
            i = g[0]
            k = int(np.clip(op.get('index', 0), 0, len(P[i]) - 1))
            delta = np.asarray(op['pos'], float) - P[i][k]
            if op.get('carry', True):
                P[i][k:] += delta
            else:
                P[i][k] += delta
            g = [i]
        elif t == 'shorten':
            for i in g:
                P[i] = shorten(P[i], op.get('keep', 0.8))
        elif t == 'reshape':
            # 2026-10-10 opt-in (Desktop Groom Studio curl / roll / wave tools, baked): the matched guide gets these
            # absolute points (same frame, same point count; ignored when the count differs)
            i = g[0]
            q = np.asarray(op.get('points', []), float)
            if q.shape == P[i].shape:
                P[i] = q
            g = [i]
        elif t == 'delete':
            alive[g] = False
        elif t == 'duplicate':
            i = g[0]
            p = P[i]
            r = p[0] + np.asarray(op.get('d', [0, 0, 0]), float)
            if head is not None:
                sd, nrm, _ = head.signed(r[None, :])
                r = r - nrm[0] * sd[0]
            P.append(p + (r - p[0])[None, :])
            alive = np.append(alive, True)
            dup_src.append((len(P) - 1, i))
            g = [len(P) - 1]
        else:
            n_ops -= 1
            continue
        touched.update(g)
    return P, alive, dict(ops=n_ops, guides=len(touched), unmatched=unmatched, deleted=int((~alive).sum()),
                          duplicated=len(dup_src), dup_src=dup_src)


def _rel(p):
    try:
        return os.path.relpath(p, ROOT)
    except ValueError:                       # other drive (temp dir in tests)
        return p


def edits_for(C):
    """edits file of a config: C['edits'] (relative to tools\\hairworks\\edits or absolute), else edits\\<char>.json."""
    name = C.get('edits')
    if name is False:
        return None
    if name:
        if not os.path.isabs(name) and name.replace('\\', '/').startswith('edits/'):
            name = name[6:]                # "edits/<file>" also accepted (2026-10-06: was silently skipped)
        p = name if os.path.isabs(name) else os.path.join(EDITS_DIR, name)
        if not os.path.exists(p) and not os.path.exists(p + '.json'):
            raise SystemExit('edits file not found: %s' % p)
    else:
        p = os.path.join(EDITS_DIR, C['name'].split('_')[0] + '.json')
    if not os.path.exists(p) and not p.endswith('.json') and os.path.exists(p + '.json'):
        p += '.json'
    return p if os.path.exists(p) else None


def apply_to_growth(C, res, col=None, log=print, head=None):
    """applies the config's edits file to res['S'] in place (deletes -> growth.reindex). -> (res, edits dict or None,
    base guides before the edits or None)."""
    p = edits_for(C)
    if not p:
        return res, None, None
    E = json.load(open(p))
    ops = E.get('ops', [])
    base = [np.array(s['p'], float) for s in res['S']]
    P, alive, st = apply_ops(base, ops, E.get('tol', 0.004), head)
    n0 = len(res['S'])
    for s, q in zip(res['S'], P):
        s['p'] = q
    if st['duplicated']:
        S2 = list(res['S'])
        for ni, si in st['dup_src']:          # a copy keeps the growth attributes of its source
            s_ = dict(S2[si]); s_['p'] = P[ni]; S2.append(s_)
        res = dict(res); res['S'] = S2
    if E.get('push_out') and col is not None:
        import collision
        collision.push_out(res['S'], col)
    if (~alive).any() or st['duplicated']:
        import growth
        res = growth.reindex(res, list(np.flatnonzero(alive)))
    else:
        res['roots'] = np.array([s['p'][0] for s in res['S']])
        res['dirs'] = np.array([(s['p'][1] - s['p'][0]) / max(np.linalg.norm(s['p'][1] - s['p'][0]), 1e-9)
                                for s in res['S']])
    log('  edits: %d ops applied to %d guides (%d unmatched, %d deleted, %d duplicated) from %s' % (
        st['ops'], st['guides'], st['unmatched'], st['deleted'], st['duplicated'], _rel(p)))
    return res, E, base


# ------------------------------------------------------------------------------------------------ export
def decimate(V, T, max_tris=8000):
    """vertex-clustering decimation (grid snap, degenerate / duplicate triangles removed) to <= max_tris."""
    V = np.asarray(V, float); T = np.asarray(T, int).reshape(-1, 3)
    if len(T) <= max_tris:
        used = np.unique(T)
        remap = np.full(len(V), -1); remap[used] = np.arange(len(used))
        return V[used], remap[T]
    ext = float(np.ptp(V, 0).max()) or 1.0
    cell = ext / 200.0
    for _ in range(40):
        key = np.floor((V - V.min(0)) / cell).astype(np.int64)
        _, cid, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
        inv = inv.reshape(-1)
        Tc = inv[T]
        ok = (Tc[:, 0] != Tc[:, 1]) & (Tc[:, 1] != Tc[:, 2]) & (Tc[:, 0] != Tc[:, 2])
        Tc = Tc[ok]
        if len(Tc):
            _, ui = np.unique(np.sort(Tc, 1), axis=0, return_index=True)
            Tc = Tc[np.sort(ui)]          # duplicates removed, original winding kept
        if len(Tc) <= max_tris:
            break
        cell *= 1.25
    nc = inv.max() + 1
    Vc = np.zeros((nc, 3)); cnt = np.zeros(nc)
    np.add.at(Vc, inv, V); np.add.at(cnt, inv, 1)
    Vc /= np.maximum(cnt, 1)[:, None]
    used = np.unique(Tc)
    remap = np.full(nc, -1); remap[used] = np.arange(len(used))
    return Vc[used], remap[Tc]


def _r4(a):
    return np.round(np.asarray(a, float), 4).tolist()


def mesh_entry(name, V, T, max_tris, zmin=None):
    V = np.asarray(V, float); T = np.asarray(T, int).reshape(-1, 3)
    if zmin is not None and len(T):
        T = T[(V[T][:, :, 2] > zmin).any(1)]
    if not len(T):
        return None
    v, t = decimate(V, T, max_tris)
    return dict(name=name, v=_r4(v), t=t.astype(int).tolist())


def export_json(C, res, head, info, comp=None, edits=None, base=None, out_dir=EDIT_OUT, log=print, index=0):
    """compact JSON for the Strand Editor: guides (metres, 0.1 mm), growth faces, head + hat / outfit meshes
    (decimated, cropped to the hair region). -> path."""
    import gamefiles as G
    os.makedirs(out_dir, exist_ok=True)
    zmin = head.top - 0.45
    meshes = []
    m = mesh_entry('head', head.v, head.t, 8000)
    if m:
        m['kind'] = 'head'; meshes.append(m)
    app = C.get('appearance') or next(iter(info.get('outfit', {})), '')
    outf = list((C.get('outfits') or {}).get(app) or info.get('outfit', {}).get(app, []))
    hats = list((C['growth'].get('drop_near_meshes') or {}).get('meshes', []))
    seen = set()
    for rel, kind in [(r, 'hat') for r in hats] + [(r, 'outfit') for r in outf]:
        if rel.lower() in seen:
            continue
        seen.add(rel.lower())
        try:
            mm = G.load_mesh(rel)
            V, T, o = [], [], 0
            for ci in mm['lod0']:
                ch = mm['chunks'][ci]
                V.append(ch['pos']); T.append(ch['idx'].reshape(-1, 3) + o); o += len(ch['pos'])
            e = mesh_entry(os.path.basename(rel), np.concatenate(V), np.concatenate(T),
                           6000 if kind == 'hat' else max(800, min(4000, 16000 // max(len(outf), 1))), zmin)
            if e:
                e['kind'] = kind; e['path'] = rel; meshes.append(e)
        except Exception as ex:          # an editor mesh must never break a build
            log('  edit export: mesh %s skipped (%r)' % (rel, ex))
    D = dict(version=1, config=C['name'], character=C['name'].split('_')[0], comp=comp,
             K=int(len(res['S'][0]['p'])) if res['S'] else 0,
             head=dict(top=round(float(head.top), 4), c=_r4(head.c)),
             guides=[_r4(s['p']) for s in res['S']],
             F=np.asarray(res['F'], int).tolist(), meshes=meshes)
    if edits:
        D['edits'] = edits
        if base is not None:
            D['guides_base'] = [_r4(p) for p in base]
    fn = C['name'] + ('' if not index else '__' + str(comp)) + '.json'
    path = os.path.join(out_dir, fn)
    s = json.dumps(D, separators=(',', ':'))
    open(path, 'w').write(s)
    log('  edit export: %s (%d guides, %d faces, %d meshes, %.0f KB)' % (
        _rel(path), len(D['guides']), len(D['F']), len(meshes), len(s) / 1024.0))
    return path


HW_OUT = os.path.join(ROOT, 'build', 'hairbatch', 'hw')


def export_hw_pack(C, fur_bytes, edit_path, tex=None, comp=None, index=0, out_dir=HW_OUT, log=print):
    """OPT-IN addition (R78, HairWorks Preview renderer): writes build\\hairbatch\\hw\\<cfg>[__<comp>].json = the
    strand-editor export (edit_path) + what the fur actually carries (guides, faces, per-corner uvs, bones, collision
    spheres / capsules, materials as written) + the card texture (`tex`, read from the game's texture.cache on the PC,
    else data\\hair) and its density per growth corner / face. Only READS the fur bytes: the build output is unchanged.
    Config key "hw_pack": false skips it. -> path or None."""
    if C.get('hw_pack') is False:
        return None
    import hwpreview_pack as HP
    edit = json.load(open(edit_path)) if edit_path and os.path.exists(edit_path) else None
    D = HP.build_pack(edit, fur_bytes, HP.game_texture, fur_name=os.path.basename(str(comp or C['name'])) + '.redfur',
                      log=log)
    if tex and tex not in D['textures']:          # the card texture even when the fur uses no texture handle
        img = HP.game_texture(tex)
        if img is not None:
            D['textures'][tex] = HP.tex_entry(img)
            D['card_texture'] = tex
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, C['name'] + ('' if not index else '__' + str(comp)) + '.json')
    s = HP.dumps(D)
    open(path, 'w').write(s)
    log('  hw pack: %s (%.0f KB)' % (os.path.relpath(path, ROOT), len(s) / 1024.0))
    return path


def clear_exports(cfg, out_dir=EDIT_OUT):
    import glob
    for p in glob.glob(os.path.join(out_dir, cfg + '.json')) + glob.glob(os.path.join(out_dir, cfg + '__*.json')) + \
            glob.glob(os.path.join(HW_OUT, cfg + '.json')) + glob.glob(os.path.join(HW_OUT, cfg + '__*.json')):
        try:
            os.remove(p)
        except OSError:
            pass


if __name__ == '__main__':
    # python3 tools/hairworks/strand_edits.py check <edits.json> <cfg>_EDIT.json   -> dry run: ops / matches / unmatched
    import sys
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == 'check':
        E = json.load(open(a[1])); D = json.load(open(a[2]))
        G = D.get('guides_base') if D.get('edits') and D.get('guides_base') else D['guides']
        P, alive, st = apply_ops(G, E.get('ops', []), E.get('tol', 0.004), head=export_head(D))
        for op in E.get('ops', []):
            if '_plane_x' in op:
                print('mirror: plane x = %.4f%s' % (op['_plane_x'], '  ear_shift T %s' % np.round(op['_ear_T'], 4)
                                                    if op.get('_ear_T') is not None else ''))
        print('edits: %d ops applied to %d guides (%d unmatched, %d deleted, %d duplicated) on %d guides of %s' % (
            st['ops'], st['guides'], st['unmatched'], st['deleted'], st['duplicated'], len(G), D.get('config')))
    else:
        print(__doc__)
