r"""studio_geo - opt-in config key "studio_geo" (2026-10-12): a Groom Studio project's result as a pipeline build.

A Studio project can hold ops the pipeline cannot replay (strand_edits.py does not know `rebuild`, the
rebuild_from_cards / regrow op, and Studio tools like make_bun / twist_roll / hug_cards come out as thousands of
reshape / duplicate / delete_face ops matched by root position). The Studio's own export (desk_export.py, Export or
`--agent` step "export") writes <char>_<tag>_geo.json: the guides / growth faces / UVs / bone weights exactly as the
Studio shows them after ALL ops. With

    "studio_geo": {"geo": "tools/hairworks/studio_geo/<variant>_geo.json.gz",     (.json or .json.gz, repo-relative)
                   "source": "<the Studio project, for the record>",
                   "stiffness": "<png>", "density_png": "<png>", "colour_png": "<png>"}   (only for a painted map)

hairpipe builds the config as usual (bones, collision, material, density texture import, look including the Studio's
look changes as look.extra_set) and then replaces the fur's geometry with the Studio's, with the same function the
Studio export uses (desk_export.set_geometry), plus the painted stiffness map / re-baked density and colour atlas when
given (as desk_export does). Preview == Studio export == pipeline build.

Rules: base the config on the SAME config the Studio project was opened on (the geo's bone slots index that build's
bone table; checked). Default configs never set the key, so every other build is unchanged.
Make one: tools/hairworks/studio_to_config.py (Studio --agent export -> geo + config).
"""
import gzip
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def _path(p):
    return p if os.path.isabs(p) else os.path.join(ROOT, p)


def load_geo(p):
    p = _path(p)
    if p.endswith('.gz'):
        with gzip.open(p, 'rt', encoding='utf-8') as fh:
            return json.load(fh)
    with open(p, encoding='utf-8') as fh:
        return json.load(fh)


def _desk_export():
    d = os.path.join(ROOT, 'desktop_studio', 'tools')
    if d not in sys.path:
        sys.path.insert(0, d)
    import desk_export
    return desk_export


def _adaptive(g, k):
    """keep k of the guide's own CVs: root + tip, then greedily the CV farthest from the current polyline (2026-10-15,
    L353): the crown lift / root bends survive, where even arc-length spacing cut them (Yennefer l350_opt flat crown)"""
    import numpy as np
    keep = [0, len(g) - 1]
    while len(keep) < k:
        keep.sort()
        best = (-1.0, None)
        for a, b in zip(keep[:-1], keep[1:]):
            A, AB = g[a], g[b] - g[a]
            den = max(float(AB @ AB), 1e-12)
            for i in range(a + 1, b):
                t = min(1.0, max(0.0, float((g[i] - A) @ AB) / den))
                d = float(np.linalg.norm(g[i] - (A + t * AB)))
                if d > best[0]:
                    best = (d, i)
        if best[1] is None:
            break
        keep.append(best[1])
    return g[sorted(keep)]


def resample(geo, k, log=print, mode='uniform', power=1.5):
    """opt-in "resample_cvs": k (2026-10-14) - every guide re-sampled to k control points, evenly along its length.
    The game draws each hair as (CVs - 1) x splineMultiplier segments, so K 20 -> 10 halves the GPU cost of a Studio
    rebuild (Yennefer mcp33 52 ms vs plain 12 ms); it also keeps the fur under the 65535 control-point total the
    game's HairWorks can index (wavy-band Ciri 2927 x 24 = 70248 did not render at all)."""
    import numpy as np
    out = []
    for g in geo['guides']:
        g = np.asarray(g, float)
        if mode == 'adaptive' and len(g) > k:
            out.append(_adaptive(g, k).tolist())
            continue
        s = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))])
        t = np.linspace(0.0, s[-1], k)
        if mode == 'root':
            # 2026-10-18 opt-in 'root' (Geralt L806): CVs closer together near the root, t = L * (i / (K-1)) ** power.
            # 40-50 cm guides with 8 even CVs have a 6-7 cm first segment: on the front crown that straight segment
            # cuts under the skull and the root part of the hair is drawn inside the head (bare crown in the Studio).
            t = s[-1] * np.linspace(0.0, 1.0, k) ** float(power)
        out.append([[float(np.interp(x, s, g[:, j])) for j in range(3)] for x in t])
    n0 = sum(len(g) for g in geo['guides'])
    geo = dict(geo, guides=out, K=k)
    log('  studio_geo resample_cvs %d: %d -> %d control points' % (k, n0, k * len(out)))
    return geo


def band_snap(geo, bs, ub, log=print):
    """2026-10-18 opt-in "band_snap": {} (Geralt headband L805, LOCAL ONLY): with 8 CVs the CV spacing (35-40 mm) is
    longer than the band (~31 mm), so many strands have no CV under the band. Collision acts on CVs only, so in the game
    the spline between the CV above and the CV below swings out over the band. For each guide that passes through the
    band zone (band height at its azimuth +- "pad", default 2 mm) with no CV in it, the CVs are re-spaced along the guide
    (smooth warp, root and tip fixed) so that the nearest inner CV lands at the middle of that crossing. Guides rooted in
    the band zone (front hairline under the band) are left as they are. K stays. The band mesh is scalp_push.under_band's unless
    "mesh" / "chunk" / "mods" are given."""
    import numpy as np
    import gamefiles as G
    src = dict(ub or {}, **{k: v for k, v in bs.items() if k in ('mesh', 'chunk', 'mods')})
    with G.outfit_source(src.get('mods') or []):
        B = G.load_mesh(src['mesh'])['chunks'][int(src.get('chunk', 0))]['pos']
    nb = 72
    cy = float(B[:, 1].mean())
    edges = np.linspace(-np.pi, np.pi, nb + 1)
    bb = np.clip(np.digitize(np.arctan2(B[:, 1] - cy, B[:, 0]), edges) - 1, 0, nb - 1)
    mid = (edges[:-1] + edges[1:]) / 2
    z0, z1, ro = (np.full(nb, np.nan) for _ in range(3))
    for i in range(nb):
        m = bb == i
        if m.any():
            z0[i], z1[i], ro[i] = B[m, 2].min(), B[m, 2].max(), np.hypot(B[m, 0], B[m, 1] - cy).max()
    ok = ~np.isnan(z0)
    z0, z1, ro = (np.interp(mid, mid[ok], a[ok], period=2 * np.pi) for a in (z0, z1, ro))
    pad = float(bs.get('pad', 0.002))
    out, moved = [], 0
    for g in geo['guides']:
        g = np.asarray(g, float)
        K = len(g)
        s = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))])
        t = np.linspace(0.0, s[-1], 40 * K)
        P = np.stack([np.interp(t, s, g[:, j]) for j in range(3)], 1)
        bi = np.clip(np.digitize(np.arctan2(P[:, 1] - cy, P[:, 0]), edges) - 1, 0, nb - 1)
        inz = (P[:, 2] >= z0[bi] - pad) & (P[:, 2] <= z1[bi] + pad) & (np.hypot(P[:, 0], P[:, 1] - cy) < ro[bi] + 0.03)
        if inz[0]:
            out.append(g.tolist())                          # rooted in the band zone (front hairline): left as is
            continue
        d = np.diff(np.concatenate([[0], inz.astype(int), [0]]))
        runs = [(a, b) for a, b in zip(np.where(d == 1)[0], np.where(d == -1)[0])
                if not ((s[1:] >= t[a]) & (s[1:] <= t[b - 1])).any()]          # crossings with no CV in them
        if runs:
            a, b = runs[0]
            sm = 0.5 * (t[a] + t[b - 1])
            c = 1 + int(np.argmin(np.abs(s[1:K - 1] - sm)))
            # smooth warp of the CV arc positions (1 at CV c, 0 at the root and the tip): the spacing stays even
            # enough that the spline through the CVs does not overshoot (moving one CV alone did, L805 Studio sim)
            i = np.arange(K, dtype=float)
            w = np.where(i <= c, i / c, (K - 1 - i) / max(K - 1 - c, 1))
            sn = s + (sm - s[c]) * w
            g = np.stack([np.interp(sn, s, g[:, j]) for j in range(3)], 1)
            moved += 1
        out.append(g.tolist())
    log('  studio_geo band_snap: %d guides re-spaced so a CV lies in their band-zone crossing (%d guides)' % (moved, len(out)))
    return dict(geo, guides=out)


def band_split(C, geo, bs, ub, log=print):
    """2026-10-18 opt-in "band_split": {} (Geralt headband L808, LOCAL ONLY): no strand crosses the band any more. Long
    strands that run from the crown down over the back of the head pass under the band, but collision holds only their
    CVs, so in the game the hair drawn between CVs keeps lying over the band (L7xx-L807: CVs in the band, outer lid ring,
    3 mm clearance - still clipping). Every guide rooted above the band whose strand enters the band zone at an azimuth
    in "az_deg" ([-180, 0] = the back half) is cut in two:
      - upper: root .. "tuck" (0.6) of the way through the band (ends tucked under it), K CVs closer near the root
        ("power_upper" 2, as resample_mode 'root');
      - lower: a NEW guide rooted on the head below the band (same azimuth, pulled "pull" (0.15) toward the back),
        "depth" below the band's lower edge - from "gap_mm" (4) for strands that enter the band close to their root up to
        "depth_mm" (80) for the longest crown run, never below "ear_top" + "ear_clear_mm" (8) beside the ears - then the
        original strand from that height down to the tip (same fall, same tip), K CVs ("power_lower" 1.5).
    Faces whose 3 guides are all cut keep the upper guides and get a twin face on the lower guides (same uv: density /
    colour / stiffness of the source spot); mixed faces use the lower guides. Bone weights copy the source guide.
    The band mesh is scalp_push.under_band's; the head is scalp_push.head (default the config's head_mesh)."""
    import numpy as np
    import gamefiles as G
    import growth
    src = dict(ub or {}, **{k: v for k, v in bs.items() if k in ('mesh', 'chunk', 'mods')})
    with G.outfit_source(src.get('mods') or []):
        B = G.load_mesh(src['mesh'])['chunks'][int(src.get('chunk', 0))]['pos']
    head = growth.Head(bs.get('head') or C['head_mesh'])
    nb = 72
    cx, cy = float(B[:, 0].mean()), float(B[:, 1].mean())
    edges = np.linspace(-np.pi, np.pi, nb + 1)
    mid = (edges[:-1] + edges[1:]) / 2
    bb = np.clip(np.digitize(np.arctan2(B[:, 1] - cy, B[:, 0] - cx), edges) - 1, 0, nb - 1)

    def per(f):
        v = np.array([f(bb == i) if (bb == i).any() else np.nan for i in range(nb)])
        ok = ~np.isnan(v)
        return np.interp(mid, mid[ok], v[ok], period=2 * np.pi)
    z0, z1 = per(lambda m: B[m, 2].min()), per(lambda m: B[m, 2].max())
    binof = lambda a: np.clip(np.digitize(a, edges) - 1, 0, nb - 1)
    lo, hi = (np.radians(float(x)) for x in bs.get('az_deg', [-180, 0]))
    tuck, pull = float(bs.get('tuck', 0.6)), float(bs.get('pull', 0.15))
    gap, depth = float(bs.get('gap_mm', 4)) / 1000, float(bs.get('depth_mm', 80)) / 1000
    ear_top = float(bs.get('ear_top', 1.778)) + float(bs.get('ear_clear_mm', 8)) / 1000
    S = np.array([np.asarray(g, float) for g in geo['guides']])
    N, K = S.shape[:2]

    def resample(P, n, power):
        # CVs closer near the root (power, as resample_mode 'root'): even CVs cut under the skull on the crown
        s = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))])
        t = s[-1] * np.linspace(0.0, 1.0, n) ** power
        return np.stack([np.interp(t, s, P[:, j]) for j in range(3)], 1)
    pw_up, pw_lo = float(bs.get('power_upper', 2.0)), float(bs.get('power_lower', 1.5))

    def on_head(a, z):
        # head surface point at azimuth a (round the band axis) and height z: bisection along the horizontal ray
        u = np.array([np.cos(a), np.sin(a), 0.0])
        r0, r1 = 0.0, 0.2
        for _ in range(30):
            rm = 0.5 * (r0 + r1)
            p = np.array([cx, cy, z]) + u * rm
            if head.signed(p[None])[0][0] < 0:
                r0 = rm
            else:
                r1 = rm
        return np.array([cx, cy, z]) + u * r1

    cut, cross = {}, {}
    for i in range(N):
        g = S[i]
        s = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))])
        t = np.linspace(0.0, s[-1], 60 * K)
        P = np.stack([np.interp(t, s, g[:, j]) for j in range(3)], 1)
        bi = binof(np.arctan2(P[:, 1] - cy, P[:, 0] - cx))
        if P[0, 2] <= z1[bi[0]] + 0.002:
            continue                                         # rooted in / below the band
        inz = (P[:, 2] <= z1[bi]) & (P[:, 2] >= z0[bi])
        if not inz.any():
            continue
        k = int(np.argmax(inz))
        a = float(np.arctan2(P[k, 1] - cy, P[k, 0] - cx))
        out = np.where(P[k:, 2] < z0[bi[k:]])[0]
        if not len(out):
            continue
        cross[i] = (k, k + int(out[0]), a, P)
        if lo <= a <= hi:
            cut[i] = cross[i]
    if bs.get('grow_mixed') and cut:
        # 2026-10-19 opt-in (L812): a face mixing cut and uncut guides was moved onto the lower guides, which bared the
        # scalp above the band (temples); also cut the uncut guides of such faces that cross the band (any azimuth)
        for _ in range(int(bs.get('grow_passes', 2))):
            add = {x for f in geo['F'] for x in map(int, f) if x in cross and x not in cut
                   and any(int(y) in cut for y in f)}
            if not add:
                break
            cut.update({x: cross[x] for x in add})
            log('  studio_geo band_split grow_mixed: +%d neighbour guides cut' % len(add))
    if not cut:
        log('  studio_geo band_split: no guide crosses the band in the sector')
        return geo
    ent = np.array([v[0] for v in cut.values()], float)
    e0, e1 = ent.min(), max(ent.max(), ent.min() + 1)
    upper, lower, skipped = {}, {}, 0
    sep, placed = float(bs.get('root_sep_mm', 0)) / 1000, []
    for i, (k, kx, a, P) in cut.items():
        ab = np.arctan2(np.sin(a), np.cos(a))
        a2 = ab + pull * (np.arctan2(np.sin(-np.pi / 2 - ab), np.cos(-np.pi / 2 - ab)))
        bz = float(z0[binof(np.array([a2]))[0]])
        side = abs(np.cos(a2)) > 0.5                          # within 60 deg of the sides: the ears are below
        dmax = min(depth, bz - ear_top) if side else depth
        zr = bz - gap - (k - e0) / (e1 - e0) * max(dmax - gap, 0.0)
        later = np.where(P[kx:, 2] <= zr)[0]
        if not len(later):
            skipped += 1
            continue
        j = kx + int(later[0])
        root = on_head(a2, zr)
        if sep:
            # opt-in "root_sep_mm" (L816): strands entering the band in the same azimuth bin got the SAME lower root;
            # the Studio matches edit ops by root, so every op on one twin hit the other (149 shared roots on split5)
            for t in range(1, 40):
                if not placed or min(np.linalg.norm(p - root) for p in placed) >= sep:
                    break
                root = on_head(a2 + (1 if t % 2 else -1) * ((t + 1) // 2) * sep / 0.09, zr - 0.0005 * (t // 2))
            placed.append(root)
        kt = k + int(round(tuck * (kx - k)))
        upper[i] = resample(P[:kt + 1], K, pw_up)
        lower[i] = resample(np.vstack([root[None], P[j:]]), K, pw_lo)
    ids = sorted(upper)
    new_index = {i: N + n for n, i in enumerate(ids)}
    guides = [upper[i].tolist() if i in upper else S[i].tolist() for i in range(N)] + [lower[i].tolist() for i in ids]
    bidx = list(geo['bone_idx']) + [geo['bone_idx'][i] for i in ids]
    bw = list(geo['bone_w']) + [geo['bone_w'][i] for i in ids]
    F, UV = [], []
    twins = mixed = 0
    for f, uv in zip(geo['F'], geo['uv']):
        f = [int(x) for x in f]
        n = sum(x in upper for x in f)
        if n == 3:
            F += [f, [new_index[x] for x in f]]
            UV += [uv, uv]
            twins += 1
        elif n:
            F.append([new_index.get(x, x) for x in f])
            UV.append(uv)
            mixed += 1
        else:
            F.append(f)
            UV.append(uv)
    Rn = np.array([lower[i][0] for i in ids])
    log('  studio_geo band_split: %d guides cut at the band (az %.0f..%.0f deg, %d skipped: tip above the new root), '
        '%d twin faces, %d mixed faces on the lower guides; new roots z %.3f..%.3f'
        % (len(ids), np.degrees(lo), np.degrees(hi), skipped, twins, mixed, Rn[:, 2].min(), Rn[:, 2].max()))
    return dict(geo, guides=guides, F=F, uv=UV, bone_idx=bidx, bone_w=bw)


def _free_below(main, frozen, fb, B, cx, cy, edges, nb, log=print):
    """2026-10-19 opt-in frozen_fur.free_below {"gap_mm": 3, "overlap_mm": 3, "min_mm": 15} (Geralt headband L818, LOCAL ONLY).
    L817 in game: the frozen fur (simulate 0) also drew the long temple / side locks that run under the band and hang to the
    shoulders, so they stayed rigid ("too straight"), went into the armour and could no longer fall in front of the shoulders
    as in the simulated L806. Every frozen guide is cut where it first goes gap_mm below the band's LOWER edge (per azimuth,
    from the band mesh): the frozen fur keeps root .. cut + overlap_mm; the rest (when longer than min_mm) becomes a new
    SIMULATED guide rooted at the cut, and every frozen face whose 3 guides all have such a lower part gets a copy (same uv)
    in the simulated fur. Frozen faces whose 3 guides start below the band move to the simulated fur whole."""
    import numpy as np
    gap, ov, mn = (float(fb.get(k, d)) / 1000 for k, d in (('gap_mm', 3), ('overlap_mm', 3), ('min_mm', 15)))
    # opt-in "lift_mm" (2026-10-25, Ciri L1090; user: "above band is very white VS right under", "because of the weird cut
    # that the top part is not the same as hair part"): the simulated lower part starts lift_mm further UP its own strand
    # (under the frozen top, behind the band), so its root (root colour, the path tracer's root roughness over the first
    # ~5 % of the strand) no longer shows right under the band; there the hair is mid-strand like the top fur above it.
    lift = float(fb.get('lift_mm', 0)) / 1000
    bb = np.clip(np.digitize(np.arctan2(B[:, 1] - cy, B[:, 0] - cx), edges) - 1, 0, nb - 1)
    mid = (edges[:-1] + edges[1:]) / 2
    v = np.array([B[bb == i, 2].min() if (bb == i).any() else np.nan for i in range(nb)])
    ok = ~np.isnan(v)
    z0 = np.interp(mid, mid[ok], v[ok], period=2 * np.pi)
    G = [np.asarray(g, float) for g in frozen['guides']]
    K = len(G[0])

    def cut(P, a, b):
        s = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))])
        t = np.linspace(a, b, K)
        return np.stack([np.interp(t, s, P[:, c]) for c in range(3)], 1)

    upper, lower, whole = [], {}, set()
    for i, P in enumerate(G):
        s = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))])
        ss = np.linspace(0, s[-1], 400)
        Q = np.stack([np.interp(ss, s, P[:, c]) for c in range(3)], 1)
        zb = z0[np.clip(np.digitize(np.arctan2(Q[:, 1] - cy, Q[:, 0] - cx), edges) - 1, 0, nb - 1)] - gap
        below = np.where(Q[:, 2] < zb)[0]
        if not len(below):
            upper.append(P); continue
        sc = float(ss[below[0]])
        if sc <= 1e-4:                                 # rooted below the band (a 'mixed' face copy): simulated whole, frozen stub
            whole.add(i); lower[i] = P; upper.append(cut(P, 0, min(s[-1], ov))); continue
        upper.append(cut(P, 0, min(s[-1], sc + ov)))
        if s[-1] - sc > mn:
            lower[i] = cut(P, max(0.0, sc - lift), s[-1])
    F = [[int(x) for x in f] for f in frozen['F']]
    # faces only partly below the band: each corner guide without a lower part gets a short simulated stub at its own tip
    # (3 mm along its end), so the lower copy of the face blends long -> short like the original hair (without it, L818's
    # first build left a dark gap under the band behind the ear)
    stub = {}
    for f in F:
        if any(x in lower for x in f) and not all(x in whole for x in f):
            for x in f:
                if x not in lower and x not in stub:
                    P = G[x]; d = P[-1] - P[-2]; d = d / max(np.linalg.norm(d), 1e-9)
                    stub[x] = P[-1] + np.linspace(0, ov, K)[:, None] * d[None]
    nstub = len(stub)
    lower.update(stub)
    keepF, keepU, addF, addU, wholeF, wholeU = [], [], [], [], [], []
    for f, u in zip(F, frozen['uv']):
        if all(x in whole for x in f):
            wholeF.append(f); wholeU.append(u); continue
        keepF.append(f); keepU.append(u)
        if all(x in lower for x in f):
            addF.append(f); addU.append(u)
    # opt-in "max_face_mm" (2026-10-21, Geralt Ciri look L1070): a frozen face under the band across the forehead joins a
    # right-temple and a left-temple guide; its lower copy then joins the two side locks and draws its hair through the head
    # (user: "hairs look like they come from inside the head", out of the cheek). Lower copies whose lower-part roots lie
    # further apart than this are dropped.
    mx = fb.get('max_face_mm')
    if mx:
        def span(f):
            R = np.array([lower[x][0] for x in f])
            return max(np.linalg.norm(R[0] - R[1]), np.linalg.norm(R[1] - R[2]), np.linalg.norm(R[0] - R[2]))
        keep = [k for k, f in enumerate(addF) if span(f) <= mx / 1000]
        kw = [k for k, f in enumerate(wholeF) if span(f) <= mx / 1000]
        log('  studio_geo frozen_fur free_below max_face_mm %g: dropped %d lower face copies + %d whole faces spanning wider'
            % (mx, len(addF) - len(keep), len(wholeF) - len(kw)))
        addF, addU = [addF[k] for k in keep], [addU[k] for k in keep]
        wholeF, wholeU = [wholeF[k] for k in kw], [wholeU[k] for k in kw]
    n0 = len(main['guides'])
    ids = sorted(lower)
    nid = {o: n0 + k for k, o in enumerate(ids)}
    src = lambda o: lower[o]
    main = dict(main, guides=list(main['guides']) + [src(o).tolist() for o in ids],
                bone_idx=list(main['bone_idx']) + [frozen['bone_idx'][o] for o in ids],
                bone_w=list(main['bone_w']) + [frozen['bone_w'][o] for o in ids],
                F=list(main['F']) + [[nid[x] for x in f] for f in addF + wholeF], uv=list(main['uv']) + addU + wholeU)
    frozen = dict(frozen, guides=[P.tolist() for P in upper], F=keepF, uv=keepU)
    log('  studio_geo frozen_fur free_below: %d frozen guides cut %.0f mm under the band (+%.0f mm overlap), %d rooted below it, '
        '%d stubs; %d simulated lower guides on %d faces; %d faces moved whole; frozen fur %d faces, simulated %d guides / %d faces'
        % (len(lower) - len(whole) - nstub, 1000 * gap, 1000 * ov, len(whole), nstub, len(lower), len(addF), len(wholeF), len(keepF),
           len(main['guides']), len(main['F'])))
    return main, frozen


def drop_faces(geo, df, log=print):
    """2026-10-22 opt-in studio_geo.drop_faces {"file": json, "tol_mm": 0.5} (Geralt Ciri look L1074, LOCAL ONLY). User on L1072
    in dialogue: a wide horizontal sheet on the side lock. Fan faces (roots 1.5 cm apart, tips 8-12 cm) whose guides land on
    the shoulder at different heights when the head turns and tilts draw their hair sideways; other faces cover the same area.
    The file lists faces of the built simulated fur by the root points of their 3 guides ({"faces": [[[x,y,z] x3], ...]},
    made by geralt_headband/sheet_faces.py); those faces are dropped, the guides stay."""
    import numpy as np
    from scipy.spatial import cKDTree
    L = json.load(open(_path(df['file'])))['faces']
    R = np.array([g[0] for g in geo['guides']], float)
    d, ix = cKDTree(R).query(np.array(L, float).reshape(-1, 3))
    tol = float(df.get('tol_mm', 0.5)) / 1000
    if (d > tol).any():
        raise SystemExit('studio_geo drop_faces: %d listed roots match no guide within %g mm' % (int((d > tol).sum()), tol * 1000))
    drop = {tuple(sorted(t)) for t in ix.reshape(-1, 3).tolist()}
    keep = [k for k, f in enumerate(geo['F']) if tuple(sorted(int(x) for x in f)) not in drop]
    log('  studio_geo drop_faces %s: dropped %d of %d faces (%d listed)' % (df['file'], len(geo['F']) - len(keep), len(geo['F']), len(L)))
    return dict(geo, F=[geo['F'][k] for k in keep], uv=[geo['uv'][k] for k in keep])


def _root_lift(geo, frozen, rl, log=print):
    """2026-10-25 opt-in frozen_fur.root_lift {"mesh", "chunk", "mods", "above_mm": 6, "below_mm": 8, "match_mm": 3}
    (Geralt headband L1086, presplit builds; LOCAL ONLY). User: "under band is dark", same on Ciri. free_below rooted the
    simulated strands 3 mm under the band's lower edge, so their roots (root colour, the path tracer's root roughness:
    Graphics Control range .05 = about 1 cm) show right under the band. Every simulated guide rooted from "below_mm" under
    the lower edge up to the edge is extended UP along the frozen strand it continues (the nearest frozen polyline point
    within "match_mm" of its root; the frozen tip overlaps it) until "above_mm" above the lower edge, i.e. behind the band;
    then resampled to its CV count. Same as free_below gap_mm -above_mm on a split build."""
    import numpy as np
    import gamefiles as G
    with G.outfit_source(rl.get('mods') or []):
        B = G.load_mesh(rl['mesh'])['chunks'][int(rl.get('chunk', 0))]['pos']
    nb = 72
    cx, cy = float(B[:, 0].mean()), float(B[:, 1].mean())
    edges = np.linspace(-np.pi, np.pi, nb + 1)
    mid = (edges[:-1] + edges[1:]) / 2
    bb = np.clip(np.digitize(np.arctan2(B[:, 1] - cy, B[:, 0] - cx), edges) - 1, 0, nb - 1)
    v = np.array([B[bb == i, 2].min() if (bb == i).any() else np.nan for i in range(nb)])
    ok = ~np.isnan(v)
    z0 = np.interp(mid, mid[ok], v[ok], period=2 * np.pi)
    zlo = lambda p: z0[int(np.clip(np.digitize(np.arctan2(p[1] - cy, p[0] - cx), edges) - 1, 0, nb - 1))]
    up, below, mt = (float(rl.get(k, d)) / 1000 for k, d in (('above_mm', 6), ('below_mm', 8), ('match_mm', 3)))
    # the band's inner radius per azimuth: the lifted part is pulled "clear_mm" (2) inside it (L1086 first build: the
    # frozen strands it follows lie over the band, 13 guides drew on the band's outer face)
    rB = np.hypot(B[:, 0] - cx, B[:, 1] - cy)
    rv = np.array([rB[bb == i].min() if (bb == i).any() else np.nan for i in range(nb)])
    rin = np.interp(mid, mid[ok], rv[ok], period=2 * np.pi) - float(rl.get('clear_mm', 2)) / 1000

    def clamp(P, zl):
        a = np.clip(np.digitize(np.arctan2(P[:, 1] - cy, P[:, 0] - cx), edges) - 1, 0, nb - 1)
        r = np.hypot(P[:, 0] - cx, P[:, 1] - cy)
        m = (P[:, 2] >= zl - 0.001) & (r > rin[a])
        s = np.where(m, rin[a] / np.maximum(r, 1e-9), 1.0)
        return np.stack([cx + (P[:, 0] - cx) * s, cy + (P[:, 1] - cy) * s, P[:, 2]], 1), int(m.sum())

    def dense(P, n=400):
        s = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))])
        t = np.linspace(0, s[-1], n)
        return np.stack([np.interp(t, s, P[:, k]) for k in range(3)], 1)
    FP = [dense(np.asarray(g, float)) for g in frozen['guides']]
    from scipy.spatial import cKDTree
    allp = np.concatenate(FP); own = np.concatenate([np.full(len(p), i) for i, p in enumerate(FP)])
    pos = np.concatenate([np.arange(len(p)) for p in FP])
    tree = cKDTree(allp)
    S = [np.asarray(g, float) for g in geo['guides']]
    K = len(S[0])
    lifted, nomatch, clamped, moved = 0, 0, 0, []
    for i, P in enumerate(S):
        r = P[0]; zl = zlo(r)
        if not (zl - below <= r[2] <= zl):
            continue
        d, j = tree.query(r)
        if d > mt:
            nomatch += 1
            continue
        Q = FP[own[j]][:pos[j] + 1][::-1]                      # frozen strand from the match point back up to its root
        over = np.where(Q[:, 2] > zl + up)[0]
        ext = Q[1:over[0] if len(over) else len(Q)][::-1]      # points above the old root, up to above_mm over the edge
        if not len(ext):
            continue
        W = np.concatenate([ext, P])
        s = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(W, axis=0), axis=1))])
        t = np.linspace(0, s[-1], K)
        S[i], nc = clamp(np.stack([np.interp(t, s, W[:, k]) for k in range(3)], 1), zl)
        clamped += nc
        lifted += 1; moved.append(np.linalg.norm(S[i][0] - r))
    log('  studio_geo frozen_fur root_lift: %d simulated guides re-rooted %.0f mm above the band edge (moved median %.1f mm), '
        '%d CVs pulled inside the band, %d without a frozen match within %.0f mm'
        % (lifted, up * 1000, 1000 * np.median(moved) if moved else 0, clamped, nomatch, mt * 1000))
    geo = dict(geo, guides=[g.tolist() for g in S])
    if rl.get('frozen_end_mm') is None:
        return geo, frozen
    # opt-in "frozen_end_mm" (user on L1086 in the Studio: "you can see the cut"): the frozen strands still hung 6 mm below
    # the band (free_below cut 3 mm under it + 3 mm overlap), their white tips ending in one hard line over the simulated
    # hair. Each frozen strand now ends where it first goes below the band edge + frozen_end_mm (behind the band), K CVs.
    fe = float(rl['frozen_end_mm']) / 1000
    FG, cut = [], 0
    for g in frozen['guides']:
        P = dense(np.asarray(g, float))
        below = np.where(P[:, 2] < zlo(P[0]) + fe)[0] if P[0][2] >= zlo(P[0]) + fe else []
        if len(below) and below[0] > 1:
            P = P[:below[0]]; cut += 1
            s = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))])
            t = np.linspace(0, s[-1], len(g))
            FG.append(np.stack([np.interp(t, s, P[:, k]) for k in range(3)], 1).tolist())
        else:
            FG.append(g)
    # frozen CVs within the band's height that lie outside its inner surface (L1122 / L1086: white strands rooted at the
    # band crossed its face at the back) are pulled inside it like the lifted roots
    vt = np.array([B[bb == i, 2].max() if (bb == i).any() else np.nan for i in range(nb)])
    ztop = np.interp(mid, mid[ok], vt[ok], period=2 * np.pi)
    pulled = 0
    for k, g in enumerate(FG):
        P = np.asarray(g, float)
        a = np.clip(np.digitize(np.arctan2(P[:, 1] - cy, P[:, 0] - cx), edges) - 1, 0, nb - 1)
        r = np.hypot(P[:, 0] - cx, P[:, 1] - cy)
        m = (P[:, 2] >= z0[a] - 0.001) & (P[:, 2] <= ztop[a] + 0.001) & (r > rin[a])
        if m.any():
            s = np.where(m, rin[a] / np.maximum(r, 1e-9), 1.0)
            FG[k] = np.stack([cx + (P[:, 0] - cx) * s, cy + (P[:, 1] - cy) * s, P[:, 2]], 1).tolist()
            pulled += int(m.sum())
    log('  studio_geo frozen_fur root_lift frozen_end_mm %g: %d of %d frozen strands now end behind the band, %d frozen CVs '
        'pulled inside the band' % (fe * 1000, cut, len(FG), pulled))
    return geo, dict(frozen, guides=FG)


def frozen_split(geo, fz, ub, log=print):
    """2026-10-19 opt-in "frozen_fur": {} (Geralt headband L812, LOCAL ONLY; user: "block any hair above and a bit below
    to move at all", L808 in game: the crown hair above the band still moved and the side strands rested over the band).
    The stiffness maps cannot hold harder (measured L808: per-guide values already at max), so the guides rooted more than
    "above_mm" (2) above the band's top edge become a SECOND fur that is not simulated (simulate 0, skinned only, like
    Geralt's beards): it keeps its rest shape whatever the motion. Faces whose 3 guides are all frozen go to that fur;
    every other face stays in the simulated fur (with copies of the frozen guides it uses). Returns (main geo, frozen geo);
    hairpipe writes the frozen geo as <comp> + "suffix" ("_top") next to the main fur."""
    if fz.get('presplit') and fz.get('geo'):
        # 2026-10-24 opt-in "presplit": true (Geralt headband L1122, LOCAL ONLY): the geo IS the built simulated fur (a Studio
        # export of it), so no second split / free_below (L1122 first try: 15 guides moved out, 44 stubs added)
        frozen = dict(json.load(gzip.open(_path(fz['geo']), 'rt')))
        frozen.pop('budget', None)
        log('  studio_geo frozen_fur presplit: simulated fur as given (%d guides, %d faces), frozen fur %s (%d guides, %d faces)'
            % (len(geo['guides']), len(geo['F']), fz['geo'], len(frozen['guides']), len(frozen['F'])))
        if fz.get('root_lift'):
            geo, frozen = _root_lift(geo, frozen, fz['root_lift'], log)
        return geo, frozen
    import numpy as np
    import gamefiles as G
    src = dict(ub or {}, **{k: v for k, v in fz.items() if k in ('mesh', 'chunk', 'mods')})
    with G.outfit_source(src.get('mods') or []):
        B = G.load_mesh(src['mesh'])['chunks'][int(src.get('chunk', 0))]['pos']
    nb = 72
    cx, cy = float(B[:, 0].mean()), float(B[:, 1].mean())
    edges = np.linspace(-np.pi, np.pi, nb + 1)
    mid = (edges[:-1] + edges[1:]) / 2
    bb = np.clip(np.digitize(np.arctan2(B[:, 1] - cy, B[:, 0] - cx), edges) - 1, 0, nb - 1)
    v = np.array([B[bb == i, 2].max() if (bb == i).any() else np.nan for i in range(nb)])
    ok = ~np.isnan(v)
    z1 = np.interp(mid, mid[ok], v[ok], period=2 * np.pi)
    R = np.array([g[0] for g in geo['guides']], float)
    bi = np.clip(np.digitize(np.arctan2(R[:, 1] - cy, R[:, 0] - cx), edges) - 1, 0, nb - 1)
    top = R[:, 2] > z1[bi] + float(fz.get('above_mm', 2)) / 1000
    F = [[int(x) for x in f] for f in geo['F']]
    # opt-in "mixed": true (L813): faces with ANY frozen guide go to the frozen fur too (their other guides copied into
    # it): in L812 the simulated copies on those boundary faces sagged in game (gap above the band, strands out)
    ftop = [(any if fz.get('mixed') else all)(top[x] for x in f) for f in F]

    def sub(keep_f, keep_g):
        ids = sorted(set(keep_g) | {x for f, k in zip(F, keep_f) if k for x in f})
        nid = {o: n for n, o in enumerate(ids)}
        out = dict(geo, guides=[geo['guides'][i] for i in ids], bone_idx=[geo['bone_idx'][i] for i in ids],
                   bone_w=[geo['bone_w'][i] for i in ids],
                   F=[[nid[x] for x in f] for f, k in zip(F, keep_f) if k],
                   uv=[u for u, k in zip(geo['uv'], keep_f) if k])
        return out, ids
    main, mids = sub([not k for k in ftop], [i for i in range(len(R)) if not top[i]])
    frozen, _ = sub(ftop, [])
    if fz.get('free_below') is not None:
        main, frozen = _free_below(main, frozen, fz['free_below'], B, cx, cy, edges, nb, log)
    if fz.get('geo'):
        # 2026-10-24 opt-in "geo" (Geralt headband L1120, LOCAL ONLY): the frozen fur as the user edited it in the Studio
        # (its top fur export, *_beard_geo.json gzipped), in place of the split's frozen output; the simulated fur stays split
        frozen = dict(json.load(gzip.open(_path(fz['geo']), 'rt')))
        frozen.pop('budget', None)
        log('  studio_geo frozen_fur geo %s: %d guides, %d faces' % (fz['geo'], len(frozen['guides']), len(frozen['F'])))
    log('  studio_geo frozen_fur: %d guides rooted > %.0f mm above the band -> frozen fur (%d faces, %d guides); '
        'simulated fur %d guides (%d frozen copies on its boundary faces), %d faces'
        % (int(top.sum()), float(fz.get('above_mm', 2)), len(frozen['F']), len(frozen['guides']), len(main['guides']),
           int(sum(top[i] for i in mids)), len(main['F'])))
    return main, frozen


def frozen_floor(frozen, fl, head_rel, head_mods, log=print):
    """2026-10-25 opt-in frozen_fur.floor {"mm": 1.0, "max_mm": 8} (Ciri wavy headband L1084, LOCAL ONLY). User on L964:
    bare scalp on the crown from above. Cause (measured): the 8-CV Studio export chords the crown curve, so the first 6 cm
    of the crown strands run a median 2 mm INSIDE the skull (69 % of their points under the scalp, l313's 12-CV build 44 %);
    a simulated fur sags out of it, the frozen top keeps that rest shape and its hair is hidden in the head. Here every
    frozen CV past the root that lies less than "mm" off the scalp moves out along the head normal to "mm" (capped at
    "max_mm"); the frozen part is root .. band (+ overlap) on K CVs, so its chords then stay on the scalp."""
    import numpy as np
    import gamefiles as G
    import growth
    with G.outfit_source(head_mods or []):
        head = growth.Head(head_rel)
    mm, cap = float(fl.get('mm', 1.0)) / 1000, float(fl.get('max_mm', 8)) / 1000
    S = np.array([np.asarray(g, float) for g in frozen['guides']])
    P = S[:, 1:].reshape(-1, 3)
    sd, nrm, _ = head.signed(P)
    mv = np.clip(mm - sd, 0.0, cap) * (sd < mm)
    S[:, 1:] = (P + nrm * mv[:, None]).reshape(S[:, 1:].shape)
    log('  studio_geo frozen_fur floor %.1f mm: %d of %d frozen CVs moved out (median %.1f, max %.1f mm); under the scalp %.0f %% -> %.0f %%'
        % (mm * 1000, int((mv > 0).sum()), len(P), 1000 * np.median(mv[mv > 0]) if (mv > 0).any() else 0.0,
           1000 * mv.max(), 100 * (sd < 0).mean(), 100 * (head.signed(S[:, 1:].reshape(-1, 3))[0] < 0).mean()))
    if fl.get('root_mm') is not None:
        # opt-in (L1121, Ciri parting holes): the frozen roots sit a median 2 mm UNDER the scalp and CV1 1 mm over it,
        # so every strand stays hidden for its first ~6 mm; along a parting all strands start and run away from it,
        # which left a bare line that more guides there did not close (dry: 1582 parting guides, parting bare 34 -> 27 %).
        # Root CVs deeper than "root_mm" (signed, mm; 0 = on the skin) move out along the head normal (capped at max_mm).
        rm = float(fl['root_mm']) / 1000
        sd0, n0, _ = head.signed(S[:, 0])
        mv0 = np.clip(rm - sd0, 0.0, cap) * (sd0 < rm)
        S[:, 0] = S[:, 0] + n0 * mv0[:, None]
        log('  studio_geo frozen_fur floor root_mm %.1f: %d of %d roots moved out (median %.1f mm)'
            % (rm * 1000, int((mv0 > 0).sum()), len(S), 1000 * np.median(mv0[mv0 > 0]) if (mv0 > 0).any() else 0.0))
    frozen = dict(frozen, guides=S.tolist())
    if fl.get('split_hidden'):
        frozen = _split_hidden(frozen, fl['split_hidden'], head, mm, cap, log)
    return frozen


def _split_hidden(frozen, sh, head, mm, cap, log=print):
    """opt-in frozen_fur.floor.split_hidden {"share": .4, "passes": 2, "len_mm": 40} (Ciri wavy headband L1085, LOCAL ONLY).
    User on L1084: grey / pink patches with straight edges still on the back crown. Measured: crown faces of the frozen top
    whose guides fan apart (parting, whorl) draw blended hair that runs under the scalp (the blend of points on a curved
    skull lies inside it), so whole triangles draw hidden hair (318 faces >= 40 % hidden in their first 40 mm). A face
    split at its centre keeps the fanned pair on its new edges, so the EDGES are split instead: every edge whose 50/50
    blend has >= "share" of its first "len_mm" under the scalp gets a midpoint guide (that blend, floored like the rest);
    each face is cut 1->2 / 1->3 / 1->4 by its split edges (uv at the edge midpoints). "passes" times. Frozen fur only
    (not simulated): costs draw only."""
    import numpy as np
    share, L = float(sh.get('share', 0.4)), float(sh.get('len_mm', 40)) / 1000
    S = [np.asarray(g, float) for g in frozen['guides']]
    F = [[int(x) for x in f] for f in frozen['F']]
    UV = [np.asarray(u, float).reshape(3, 2) for u in frozen['uv']]
    bidx, bw = list(frozen['bone_idx']), list(frozen['bone_w'])

    def hid(H):
        s = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(H, axis=0), axis=1))])
        t = np.linspace(0.002, min(L, s[-1]), 40)
        return float((head.signed(np.stack([np.interp(t, s, H[:, k]) for k in range(3)], 1))[0] < -0.0005).mean())
    n0f, added = len(F), 0
    for _ in range(int(sh.get('passes', 2))):
        mid = {}
        for f in F:
            for k in range(3):
                e = tuple(sorted((f[k], f[(k + 1) % 3])))
                if e in mid:
                    continue
                H = (S[e[0]] + S[e[1]]) / 2
                if hid(H) < share:
                    mid[e] = None
                    continue
                sd, nrm, _ = head.signed(H[1:])
                H[1:] += nrm * (np.clip(mm - sd, 0.0, cap) * (sd < mm))[:, None]
                mid[e] = len(S); S.append(H); added += 1
                acc = {}
                for g in e:
                    for i_, w_ in zip(bidx[g], bw[g]):
                        acc[int(round(i_))] = acc.get(int(round(i_)), 0.0) + float(w_) / 2.0
                top = sorted(acc.items(), key=lambda kv: -kv[1])[:4]
                tot = sum(w_ for _, w_ in top) or 1.0
                bidx.append([float(i_) for i_, _ in top] + [0.0] * (4 - len(top)))
                bw.append([w_ / tot for _, w_ in top] + [0.0] * (4 - len(top)))
        if not any(v is not None for v in mid.values()):
            break
        F2, UV2 = [], []
        for f, u in zip(F, UV):
            m = [mid[tuple(sorted((f[k], f[(k + 1) % 3])))] for k in range(3)]   # m[k] on edge f[k] - f[k+1]
            um = [(u[k] + u[(k + 1) % 3]) / 2 for k in range(3)]
            ns = [k for k in range(3) if m[k] is not None]
            if not ns:
                F2.append(f); UV2.append(u); continue
            if len(ns) == 3:
                tris = [((f[0], u[0]), (m[0], um[0]), (m[2], um[2])), ((f[1], u[1]), (m[1], um[1]), (m[0], um[0])),
                        ((f[2], u[2]), (m[2], um[2]), (m[1], um[1])), ((m[0], um[0]), (m[1], um[1]), (m[2], um[2]))]
            else:
                k = ns[0] if len(ns) == 1 else [x for x in range(3) if x not in ns][0]
                a, b, c = f[k], f[(k + 1) % 3], f[(k + 2) % 3]
                ua, ub, uc = u[k], u[(k + 1) % 3], u[(k + 2) % 3]
                if len(ns) == 1:       # edge a-b split at m: (a, m, c), (m, b, c)
                    tris = [((a, ua), (m[k], um[k]), (c, uc)), ((m[k], um[k]), (b, ub), (c, uc))]
                else:                  # edge a-b kept, b-c and c-a split
                    mb, mc = m[(k + 1) % 3], m[(k + 2) % 3]
                    ubm, ucm = um[(k + 1) % 3], um[(k + 2) % 3]
                    tris = [((a, ua), (b, ub), (mb, ubm)), ((a, ua), (mb, ubm), (mc, ucm)), ((mc, ucm), (mb, ubm), (c, uc))]
            for t in tris:
                F2.append([t[0][0], t[1][0], t[2][0]]); UV2.append(np.array([t[0][1], t[1][1], t[2][1]]))
        F, UV = F2, UV2
    log('  studio_geo frozen_fur split_hidden (share %.2f): +%d edge guides, frozen faces %d -> %d'
        % (share, added, n0f, len(F)))
    return dict(frozen, guides=[g.tolist() for g in S], F=F, uv=[[float(x) for x in u.ravel()] for u in UV],
                bone_idx=bidx, bone_w=bw)


def frozen_fur_bytes(data, geo, look_set=None):
    """the frozen fur of frozen_split: the finished main fur (look, maps, bones) with the frozen geometry, not simulated.
    look_set (2026-10-26 opt-in frozen_fur.look_set, Ciri L1102): {"levelOfDetail.detailLOD.detailLODDensity": ["Float", .5], ...}
    set on the frozen fur only (the PT draws 32 hairs per growth face, so the split crown faces drew a solid shell)"""
    from cr2w_edit import CR2WDoc, Chunk
    DE = _desk_export()
    doc = CR2WDoc.parse(data)
    ch = Chunk(doc, 0)
    DE.set_geometry(doc, ch, dict(geo, colliders=None, collision_r=None))
    DE.apply_value(doc, ch, ['physicalMaterials', 'simulation', 'simulate'], 'Bool', DE.encode(doc, 'Bool', 0))
    for k, (typ, val) in (look_set or {}).items():
        DE.apply_value(doc, ch, k.split('.'), typ, DE.encode(doc, typ, val))
    ch.commit()
    return doc.to_bytes()


def _under_share(geo, head):
    """share of guide points within 15 mm of the head (above the ear line) that lie UNDER its surface"""
    import numpy as np
    P = np.concatenate([np.asarray(g, float)[1:] for g in geo['guides']])
    sd = head.signed(P)[0]
    near = (np.abs(sd) < 0.015) & (P[:, 2] > head.top - 0.20)
    return float((sd[near] < 0).mean()) if near.any() else 0.0


def undercoat(geo, uc, log=print):
    """2026-10-18 opt-in "undercoat" (Geralt headband L675, LOCAL ONLY): split_faces closed the crown gaps but every split
    face adds FULL-LENGTH hair that also hangs down the back (L674: 533 -> 763 faces, player GPU 43 -> 100 ms). Here the
    groom's faces stay untouched and a separate layer of SHORT hair is added on the crown: one planted guide at the
    centroid of every face larger than "min_area_mm2" above "z_min" (the centre blend of its 3 guides, cut to "length_mm"
    and resampled to the fur's K CVs, so it lies along the crown flow), faces = Delaunay of the planted roots only
    (stereographic projection round the head, 3D edges <= "max_edge_mm"), so no face mixes a short and a long guide
    (toolarch/undercoat.py: mixed faces sink). Each undercoat face samples the atlas at one point (degenerate UV
    triangle = the centre of the face its first root came from), so density / stiffness / colour stay those of that spot.
    Planted spots: the centroid of every picked face plus ("edges", default true) the midpoint of each of its edges.
      {"z_min": 1.84, "min_area_mm2": 60, "length_mm": 45, "max_edge_mm": 16, "edges": true}"""
    import numpy as np
    from scipy.spatial import Delaunay
    S = np.array([np.asarray(g, float) for g in geo['guides']])
    F = np.asarray(geo['F'], int)
    UV = np.asarray(geo['uv'], float).reshape(-1, 3, 2)
    K = S.shape[1]
    R = S[:, 0]
    ar = 0.5 * np.linalg.norm(np.cross(R[F[:, 1]] - R[F[:, 0]], R[F[:, 2]] - R[F[:, 0]]), axis=1) * 1e6
    cz = R[F].mean(1)[:, 2]
    pick = np.where((ar > float(uc.get('min_area_mm2', 60))) & (cz > float(uc.get('z_min', 1.84))))[0]
    L = float(uc.get('length_mm', 45)) / 1000.0
    # planted points: the centroid of every picked face and (opt-in "edges", default on) the midpoint of its edges,
    # each edge once: barycentric (gid, weight) lists + the UV of that spot in the picked face
    pts, seen = [], set()
    for fi in pick:
        f = [int(x) for x in F[fi]]
        pts.append(([(g, 1 / 3.0) for g in f], UV[fi].mean(0)))
        if uc.get('edges', True):
            for k in range(3):
                a, b = f[k], f[(k + 1) % 3]
                if (min(a, b), max(a, b)) in seen:
                    continue
                seen.add((min(a, b), max(a, b)))
                pts.append(([(a, 0.5), (b, 0.5)], (UV[fi][k] + UV[fi][(k + 1) % 3]) / 2))
    new, bi_, bw_, uvc = [], [], [], []
    for gw, uv in pts:
        g = sum(w * S[gi] for gi, w in gw)
        s = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))])
        t = np.linspace(0.0, min(L, s[-1]), K)
        new.append(np.stack([np.interp(t, s, g[:, j]) for j in range(3)], 1))
        acc = {}
        for gi, w in gw:
            for i_, w_ in zip(geo['bone_idx'][gi], geo['bone_w'][gi]):
                acc[int(round(i_))] = acc.get(int(round(i_)), 0.0) + float(w_) * w
        top = sorted(acc.items(), key=lambda kv: -kv[1])[:4]
        tot = sum(w_ for _, w_ in top) or 1.0
        bi_.append([float(i_) for i_, _ in top] + [0.0] * (4 - len(top)))
        bw_.append([w_ / tot for _, w_ in top] + [0.0] * (4 - len(top)))
        uvc.append(uv)
    if len(new) < 3:
        log('  studio_geo undercoat: %d planted roots, nothing added' % len(new))
        return geo
    N = np.array(new)
    roots = N[:, 0]
    c = np.array([0.0, float(np.median(R[:, 1])), float(R[:, 2].max()) - 0.12])
    d = roots - c
    d /= np.linalg.norm(d, axis=1, keepdims=True)
    xy = d[:, :2] / (1.0 + d[:, 2:3])                       # stereographic from the bottom pole: the crown sits at the centre
    tri = Delaunay(xy).simplices
    e = np.max([np.linalg.norm(roots[tri[:, i]] - roots[tri[:, (i + 1) % 3]], axis=1) for i in range(3)], axis=0)
    tri = tri[e <= float(uc.get('max_edge_mm', 16)) / 1000.0]
    n0 = len(S)
    Fn = (tri + n0).tolist()
    UVn = [[float(uvc[a][0]), float(uvc[a][1])] * 3 for a, _, _ in tri]
    log('  studio_geo undercoat (z > %.3f, faces > %g mm2, %g mm hair): +%d short guides, +%d growth faces (%d -> %d)'
        % (float(uc.get('z_min', 1.84)), float(uc.get('min_area_mm2', 60)), L * 1000, len(N), len(Fn), len(F), len(F) + len(Fn)))
    return dict(geo, guides=np.concatenate([S, N]).tolist(), F=F.tolist() + Fn, uv=[list(u) for u in geo['uv']] + UVn,
                bone_idx=list(geo['bone_idx']) + bi_, bone_w=list(geo['bone_w']) + bw_)


def split_faces(geo, sf, log=print):
    """2026-10-18 opt-in "split_faces" (Geralt headband L674, LOCAL ONLY): path-traced HairWorks grows a FIXED 32 hairs per
    growth face (RTNHairsPerPatch), while the Studio preview spreads its hairs by face area; so large faces read sparse in
    game only (Geralt crown after the GPU thinning: 195 faces above z 1.84, median 132 mm2 = ~24 PT hairs / cm2 vs 52 on
    the rest; magenta twin L673: scalp gaps between the crown clumps). Every face whose root triangle is larger than
    "min_area_mm2" and whose centroid lies above "z_min" (m, model space) is split at its centroid into 3 faces; the new
    guide is the barycentric centre blend of the face's 3 guides (the hair HairWorks already draws there), its bone
    weights the blend of theirs, its UV the centre of the face's UV triangle (atlas maps stay valid). One pass per
    "passes". {"z_min": 1.84, "min_area_mm2": 100, "passes": 1}
    Opt-in box (2026-10-26, Ciri L1116: the forehead strip between the band and the hairline had 28-32 mm2 faces vs 7 mm2
    median, so 16 PT hairs per face left bare skin): "z_max", "y_max" and "x_abs_max" also bound the face centroid
    (model space). Absent = no bound."""
    import numpy as np
    _zmax, _ymax, _xmax = (float(sf[k]) if k in sf else None for k in ('z_max', 'y_max', 'x_abs_max'))

    def _in_box(c):
        return ((_zmax is None or c[2] <= _zmax) and (_ymax is None or c[1] <= _ymax)
                and (_xmax is None or abs(c[0]) <= _xmax))
    S = [np.asarray(g, float) for g in geo['guides']]
    F = [list(map(int, f)) for f in geo['F']]
    UV = [list(map(float, u)) for u in geo['uv']]
    BI = [list(b) for b in geo['bone_idx']]
    BW = [list(w) for w in geo['bone_w']]
    n0, nf0 = len(S), len(F)
    for _ in range(int(sf.get('passes', 1))):
        nF, nUV = [], []
        for f, uv in zip(F, UV):
            a, b, c = f
            ra, rb, rc = S[a][0], S[b][0], S[c][0]
            area = 0.5 * np.linalg.norm(np.cross(rb - ra, rc - ra)) * 1e6
            if area <= float(sf.get('min_area_mm2', 100)) or (ra[2] + rb[2] + rc[2]) / 3 <= float(sf.get('z_min', 1.84)) \
                    or not _in_box((ra + rb + rc) / 3):
                nF.append(f); nUV.append(uv)
                continue
            m = len(S)
            S.append((S[a] + S[b] + S[c]) / 3.0)
            acc = {}
            for g in (a, b, c):
                for i_, w_ in zip(BI[g], BW[g]):
                    acc[int(round(i_))] = acc.get(int(round(i_)), 0.0) + float(w_) / 3.0
            top = sorted(acc.items(), key=lambda kv: -kv[1])[:4]
            tot = sum(w_ for _, w_ in top) or 1.0
            BI.append([float(i_) for i_, _ in top] + [0.0] * (4 - len(top)))
            BW.append([w_ / tot for _, w_ in top] + [0.0] * (4 - len(top)))
            pu = [(uv[0], uv[1]), (uv[2], uv[3]), (uv[4], uv[5])]
            cu = ((pu[0][0] + pu[1][0] + pu[2][0]) / 3, (pu[0][1] + pu[1][1] + pu[2][1]) / 3)
            for (i, j), (ui, uj) in zip(((a, b), (b, c), (c, a)), ((pu[0], pu[1]), (pu[1], pu[2]), (pu[2], pu[0]))):
                nF.append([i, j, m]); nUV.append([ui[0], ui[1], uj[0], uj[1], cu[0], cu[1]])
        F, UV = nF, nUV
    log('  studio_geo split_faces (z > %.3f, > %g mm2): %d -> %d guides, %d -> %d growth faces'
        % (float(sf.get('z_min', 1.84)), float(sf.get('min_area_mm2', 100)), n0, len(S), nf0, len(F)))
    return dict(geo, guides=[g.tolist() for g in S], F=F, uv=UV, bone_idx=BI, bone_w=BW)


def hairline_fill(C, geo, hf, log=print):
    """2026-10-26 opt-in "hairline_fill" (Ciri wavy headband L1118, LOCAL ONLY). User: bare forehead above the band,
    "whatever is needed, more roots, more guides there to cover". Measured on the Studio geo: across the forehead centre
    (|x| < 35 mm; the face is +y in model space) the lowest roots sit 27-31 mm above the band's top edge, below them only
    the side locks (|x| > 40 mm) are rooted, so the strip has no hair of its own (L1116 split nothing there). New roots
    are planted on the head surface in the box (|x| <= "x_abs_max", y >= "y_min", "hz_min_mm" .. "hz_max_mm" above the
    band top), "spacing_mm" apart and at least "gap_mm" from every existing root. Each new guide copies the shape of the
    "k" nearest guides rooted above it on the same side of the parting (root z > new root + "above_mm", same sign of the
    first segment's x: the crown sweeps left / right from a centre parting), inverse-distance blended, translated to the
    new root (opt-in "merge_mm": the offset fades to 0 over that arc length, so the strand joins the hair above;
    without it L1118 dry put copied side sweeps over the face; opt-in "clear_mm": CVs past the root lifted until the
    straight segments clear the scalp by that much, the 8-CV chords otherwise cut through the forehead; "max_lift_mm"
    caps that lift per CV; "cross_parting": mm above the band under which faces may join the two sweeps; "plane":
    "stereo" triangulates in a projection around the head centre, for a box on top such as a parting (L1120: user, the
    holes were on top along the centre parting, not on the forehead; above_mm negative = source guides from anywhere);
    "only_bare_mm": roots only on scalp points with no existing hair within that radius; "length_mm": new guides cut to
    that length and resampled to K CVs, so they lie on the scalp (L1121); "hug_mm" / "lift_mm" / "rise_mm": new guides
    put back on a set distance off the scalp, rising from hug to lift over rise (L1122)).
    Faces: Delaunay of the new + nearby existing roots in the front (x, z) plane, triangles with >= 1 new root and all
    edges <= "max_edge_mm", never joining guides that sweep to opposite sides. UV of a new face = the UV centre of the
    nearest existing face (degenerate), bone weights blended from the source guides. Runs before scalp_push, so the new
    guides get the same push / band clamp / frozen split / floor as the rest. Absent = unchanged.
      {"x_abs_max": 0.045, "y_min": 0.03, "hz_min_mm": 3, "hz_max_mm": 35, "spacing_mm": 4, "gap_mm": 4, "k": 3,
       "above_mm": 3, "max_edge_mm": 12, "root_in_mm": 0.5}"""
    import numpy as np
    import gamefiles as G
    import growth
    from scipy.spatial import Delaunay, cKDTree
    sp = C['studio_geo'].get('scalp_push') or {}
    ub = sp.get('under_band') or {}
    src = dict(ub, **{k: v for k, v in hf.items() if k in ('mesh', 'chunk', 'mods')})
    with G.outfit_source(src.get('mods') or []):
        B = G.load_mesh(src['mesh'])['chunks'][int(src.get('chunk', 0))]['pos']
    with G.outfit_source(sp.get('head_mods') or []):
        head = growth.Head(hf.get('head') or sp.get('head') or C['head_mesh'])
    nb = 72
    cx, cy = float(B[:, 0].mean()), float(B[:, 1].mean())
    edges = np.linspace(-np.pi, np.pi, nb + 1)
    mid = (edges[:-1] + edges[1:]) / 2
    bb = np.clip(np.digitize(np.arctan2(B[:, 1] - cy, B[:, 0] - cx), edges) - 1, 0, nb - 1)
    v = np.array([B[bb == i, 2].max() if (bb == i).any() else np.nan for i in range(nb)])
    ok = ~np.isnan(v)

    def ztop(Q):
        return np.interp(np.arctan2(Q[:, 1] - cy, Q[:, 0] - cx), mid[ok], v[ok], period=2 * np.pi)
    S = np.array([np.asarray(g, float) for g in geo['guides']])
    F = np.asarray(geo['F'], int)
    UV = np.asarray(geo['uv'], float).reshape(-1, 3, 2)
    K = S.shape[1]
    R = S[:, 0]
    xm, ym = float(hf.get('x_abs_max', 0.045)), float(hf.get('y_min', 0.03))
    h0, h1 = float(hf.get('hz_min_mm', 3)) / 1000, float(hf.get('hz_max_mm', 35)) / 1000
    sp_m, gap = float(hf.get('spacing_mm', 4)) / 1000, float(hf.get('gap_mm', 4)) / 1000
    # candidate roots: random points on the head triangles in the box, thinned to "spacing" (greedy, fixed seed)
    V, T = head.v, head.t
    A_, B_, C_ = V[T[:, 0]], V[T[:, 1]], V[T[:, 2]]
    ar = 0.5 * np.linalg.norm(np.cross(B_ - A_, C_ - A_), axis=1)
    rng = np.random.default_rng(int(hf.get('seed', 1)))
    n = np.maximum(1, np.round(ar / 0.25e-6)).astype(int)
    ti = np.repeat(np.arange(len(T)), n)
    r1, r2 = rng.random(len(ti)), rng.random(len(ti))
    sw = r1 + r2 > 1
    r1[sw], r2[sw] = 1 - r1[sw], 1 - r2[sw]
    P = A_[ti] + r1[:, None] * (B_[ti] - A_[ti]) + r2[:, None] * (C_[ti] - A_[ti])
    hz = P[:, 2] - ztop(P)
    P = P[(np.abs(P[:, 0]) <= xm) & (P[:, 1] >= ym) & (hz >= h0) & (hz <= h1)]
    P = P[cKDTree(R).query(P)[0] >= gap]
    if hf.get('only_bare_mm'):
        # opt-in (L1121, user: "why dont you just draw some guides there to fill the gaps"): keep only scalp points that
        # no hair of the existing faces covers (blended hairs, first 6 cm, 0.3-30 mm off the scalp, within that radius)
        Wb = np.array([(i / 6, j / 6, 1 - i / 6 - j / 6) for i in range(7) for j in range(7 - i)])
        fsel = np.where(np.linalg.norm(R[F].mean(1)[:, None, :] - P[None, ::max(1, len(P) // 400), :], axis=2).min(1) < 0.08)[0]             if len(P) else np.zeros(0, int)
        H = np.einsum('wk,fkcd->fwcd', Wb, S[F[fsel]]).reshape(-1, K, 3)
        seg = np.linalg.norm(np.diff(H, axis=1), axis=2)
        ss = np.concatenate([np.zeros((len(H), 1)), np.cumsum(seg, 1)], 1)
        tq = np.linspace(0.0, 0.06, 31)
        Q = np.concatenate([np.stack([np.interp(tq[tq <= s_[-1]], s_, h[:, j]) for j in range(3)], 1) for h, s_ in zip(H, ss)])             if len(H) else np.zeros((0, 3))
        sdq = head.signed(Q)[0] if len(Q) else np.zeros(0)
        Q = Q[(sdq > 0.0003) & (sdq < 0.03)]
        Q = Q - head.signed(Q)[1] * head.signed(Q)[0][:, None] if len(Q) else Q
        if len(Q):
            P = P[cKDTree(Q).query(P)[0] > float(hf['only_bare_mm']) / 1000]
    P = P[rng.permutation(len(P))]
    keep = []
    for p in P:
        if not keep or np.min(np.linalg.norm(np.array(keep) - p, axis=1)) >= sp_m:
            keep.append(p)
    if len(keep) < 3:
        log('  studio_geo hairline_fill: %d new roots, nothing added' % len(keep))
        return geo
    NR = np.array(keep)
    _, nrm, _ = head.signed(NR)
    NR = NR - nrm * float(hf.get('root_in_mm', 0.5)) / 1000
    # sweep side of every guide: sign of the first segment's x
    side = np.sign(S[:, 1, 0] - S[:, 0, 0])
    k = int(hf.get('k', 3))
    above = float(hf.get('above_mm', 3)) / 1000
    merge = hf.get('merge_mm')
    reg = (R[:, 1] >= ym - 0.02) & (np.abs(R[:, 0]) <= xm + 0.03)
    new, bi_, bw_, nside = [], [], [], []
    for r in NR:
        cand = np.where(reg & (R[:, 2] > r[2] + above))[0]
        d = np.linalg.norm(R[cand] - r, axis=1)
        o = cand[np.argsort(d)]
        s0 = side[o[0]]
        o = [i for i in o if side[i] == s0][:k]
        w = 1.0 / np.maximum(np.linalg.norm(R[o] - r, axis=1), 1e-4)
        w /= w.sum()
        shape = np.einsum('i,ikd->kd', w, S[o] - S[o][:, :1])
        g = r + shape
        if merge:
            # offset fades out along the arc: the strand joins the blended source path
            src_path = np.einsum('i,ikd->kd', w, S[o])
            s = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(src_path, axis=0), axis=1))])
            f = np.clip(1.0 - s / (float(merge) / 1000), 0.0, 1.0)[:, None]
            g = src_path + (r - src_path[0]) * f
        if hf.get('length_mm'):
            # opt-in (L1121): a SHORT guide, cut to "length_mm" and resampled to the fur's K CVs, so its segments are
            # ~length / (K-1) long and lie on the scalp (a full-length copy's first 4 cm segment runs under it)
            s = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))])
            tl = np.linspace(0.0, min(float(hf['length_mm']) / 1000, s[-1]), K)
            g = np.stack([np.interp(tl, s, g[:, j]) for j in range(3)], 1)
        if hf.get('hug_mm') is not None:
            # opt-in (L1122, user on L1121 r0pl: "they are weirdly raising on the sides, and the parting gain no volume"):
            # a copied shape keeps its source's curve, so a copy planted higher on the dome leaves the scalp and stands up
            # as a wing over the temple. Every CV past the root is put back at a set distance off the scalp: "hug_mm" at
            # the root rising to "lift_mm" (default = hug) over "rise_mm" of arc, back towards hug_mm at the tip, so the
            # parting hair rises out of the parting and lies ON the hair beside it (volume) instead of under it.
            hug = float(hf['hug_mm']) / 1000
            lift = float(hf.get('lift_mm', hf['hug_mm'])) / 1000
            rise = float(hf.get('rise_mm', 15)) / 1000
            s = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))])
            Ltot = max(s[-1], 1e-6)
            off = hug + (lift - hug) * np.minimum(1.0, s / rise) * (1.0 - 0.6 * s / Ltot)
            for _ in range(4):
                sd_, n_, _ = head.signed(g[1:])
                g[1:] = g[1:] - n_ * (sd_ - off[1:])[:, None]
        if hf.get('clear_mm') is not None:
            # opt-in: the 8-CV chords cut through the convex forehead (L1118 dry: 65 % of the new strip hair under the
            # skin); lift the CVs past the root until every chord point lies "clear_mm" off the scalp
            cl = float(hf['clear_mm']) / 1000
            tt = np.linspace(0.0, 1.0, 9)[1:]
            g0 = g.copy()
            cap = float(hf['max_lift_mm']) / 1000 if hf.get('max_lift_mm') is not None else None
            for _ in range(12):
                moved = False
                for a in range(K - 1):
                    q = g[a] + tt[:, None] * (g[a + 1] - g[a])
                    sd_, n_, _ = head.signed(q)
                    j = int(np.argmin(sd_))
                    if sd_[j] >= cl - 1e-5:
                        continue
                    dfc = cl - sd_[j]
                    if a == 0:
                        g[1] = g[1] + n_[j] * dfc / max(tt[j], 0.25)
                    else:
                        g[a] = g[a] + n_[j] * dfc
                        g[a + 1] = g[a + 1] + n_[j] * dfc
                    moved = True
                if cap is not None:
                    # opt-in "max_lift_mm" (L1119): L1118 clear 1.5 mm lifted the front strands up to ~1 cm, they stood
                    # off the forehead (puffs in front 3/4 and from above); the lift per CV is capped
                    dv = g - g0
                    ln = np.linalg.norm(dv, axis=1, keepdims=True)
                    g = g0 + dv * np.minimum(1.0, cap / np.maximum(ln, 1e-9))
                if not moved:
                    break
        new.append(np.asarray(g, float))
        nside.append(s0)
        acc = {}
        for gi, wi in zip(o, w):
            for i_, w_ in zip(geo['bone_idx'][gi], geo['bone_w'][gi]):
                acc[int(round(i_))] = acc.get(int(round(i_)), 0.0) + float(w_) * wi
        top = sorted(acc.items(), key=lambda kv: -kv[1])[:4]
        tot = sum(w_ for _, w_ in top) or 1.0
        bi_.append([float(i_) for i_, _ in top] + [0.0] * (4 - len(top)))
        bw_.append([w_ / tot for _, w_ in top] + [0.0] * (4 - len(top)))
    N = np.array(new)
    n0 = len(S)
    # faces: Delaunay of new + nearby old roots in the front (x, z) plane
    near = np.where((R[:, 1] >= ym - 0.01) & (np.abs(R[:, 0]) <= xm + 0.015) & (R[:, 2] - ztop(R) <= h1 + 0.015)
                    & (R[:, 2] - ztop(R) >= h0 - 0.015))[0]
    if hf.get('length_mm'):
        near = near[:0]          # short guides: faces only between the new roots (an old root inside a hole cut them up)
    allR = np.concatenate([R[near], NR])
    gid = np.concatenate([near, n0 + np.arange(len(NR))])
    gside = np.concatenate([side[near], np.array(nside)])
    if hf.get('plane') == 'stereo':
        # opt-in (L1120, Ciri parting on top): the (x, z) plane folds the top of the head; project from below its centre
        c0 = np.array([0.0, float(np.median(R[:, 1])), float(R[:, 2].max()) - 0.12])
        d_ = allR - c0
        d_ /= np.linalg.norm(d_, axis=1, keepdims=True)
        tri = Delaunay(d_[:, :2] / (1.0 + d_[:, 2:3])).simplices
    else:
        tri = Delaunay(allR[:, [0, 2]]).simplices
    e = np.max([np.linalg.norm(allR[tri[:, i]] - allR[tri[:, (i + 1) % 3]], axis=1) for i in range(3)], axis=0)
    has_new = (gid[tri] >= n0).all(1) if hf.get('length_mm') else (gid[tri] >= n0).any(1)   # short + long faces sink
    one_side = (gside[tri] == gside[tri][:, :1]).all(1)
    if hf.get('cross_parting'):
        # opt-in (L1119): L1118 left a bare triangle at the parting above the band (no face joins the two sweeps);
        # faces across the parting are allowed when all 3 roots lie below "cross_parting" mm above the band
        lim = float(hf['cross_parting']) / 1000
        one_side |= ((allR[tri][:, :, 2] - ztop(allR[tri].reshape(-1, 3)).reshape(-1, 3)) < lim).all(1)
    tri = tri[has_new & one_side & (e <= float(hf.get('max_edge_mm', 12)) / 1000)]
    Fn = gid[tri].tolist()
    allS = np.concatenate([S, N])
    fc = R[F].mean(1)
    tree = cKDTree(fc)
    UVn = []
    for f in Fn:
        c = allS[f, 0].mean(0)
        u = UV[tree.query(c)[1]].mean(0)
        UVn.append([float(u[0]), float(u[1])] * 3)
    log('  studio_geo hairline_fill (|x| <= %.0f mm, %g..%g mm above the band, %g mm apart): +%d guides, +%d growth faces (%d -> %d)'
        % (xm * 1000, h0 * 1000, h1 * 1000, sp_m * 1000, len(N), len(Fn), len(F), len(F) + len(Fn)))
    return dict(geo, guides=allS.tolist(), F=F.tolist() + Fn, uv=[list(u) for u in geo['uv']] + UVn,
                bone_idx=list(geo['bone_idx']) + bi_, bone_w=list(geo['bone_w']) + bw_)


def _band_clamp(S, F, head, ub, log=print, nb=72, iters=6):
    """scalp_push opt-in "under_band": {"mesh": <band mesh rel>, "chunk": 1, "mods": [...], "clear": .001}. Pushing the hair
    out of a bigger head scalp also pushes it through a headband made for the smaller head (Ard band: only ~2.6 mm (p10)
    above the face-mod scalp; L670: band-zone hair outside the band 3.2 -> 21.2 %). Within the band's height at each
    azimuth (72 bins round the head axis), guide points and their interpolated hairs are pulled radially inside the
    band's inner ring minus "clear", never deeper than 0.5 mm above the head scalp. Roots stay. -> new S"""
    import numpy as np
    import gamefiles as G
    with G.outfit_source(ub.get('mods') or []):
        B = G.load_mesh(ub['mesh'])['chunks'][int(ub.get('chunk', 0))]['pos']
    cy = float(head.c[1])
    edges = np.linspace(-np.pi, np.pi, nb + 1)
    ab = np.arctan2(B[:, 1] - cy, B[:, 0])
    bb = np.clip(np.digitize(ab, edges) - 1, 0, nb - 1)
    mid = (edges[:-1] + edges[1:]) / 2
    rin, z0, z1 = (np.full(nb, np.nan) for _ in range(3))
    for i in range(nb):
        m = bb == i
        if m.any():
            rin[i], z0[i], z1[i] = np.hypot(B[m, 0], B[m, 1] - cy).min(), B[m, 2].min(), B[m, 2].max()
    ok = ~np.isnan(rin)
    rin, z0, z1 = (np.interp(mid, mid[ok], a[ok], period=2 * np.pi) for a in (rin, z0, z1))
    clear = float(ub.get('clear', 0.001))
    # 2026-10-19 opt-in "az_only": true (Oriana): her band (i_02_wa__oriana) is open at the back (forehead to behind the
    # ears, ~200 deg). The periodic interpolation above invents a band across the empty back bins and would pull the
    # back of the bob inside it; with az_only only bins that hold band vertices (+- 1 bin) clamp.
    has = ok | np.roll(ok, 1) | np.roll(ok, -1) if ub.get('az_only') else np.ones(nb, bool)
    # 2026-10-19 opt-in "end_trim": bins (Oriana L885, with az_only): the clamp stops this many bins BEFORE each end of the
    # band (no +-1 padding), so the side rolls that hide the open band's ends (as in vanilla) are not pulled under it
    # (L884: both band-end hooks showed over bare skin above the rolls)
    if ub.get('az_only') and ub.get('end_trim'):
        has = ok.copy()
        for _ in range(int(ub['end_trim'])):
            has = has & np.roll(has, 1) & np.roll(has, -1)
    # opt-in "reach": metres (Oriana): only hair lying on or just over the band (radius < inner ring + reach) is pulled;
    # curls / rolls standing further off (her side rolls cover the band ends, as in vanilla) are left alone
    reach = float(ub['reach']) if ub.get('reach') is not None else np.inf
    # 2026-10-19 opt-in "above": metres (Philippa L879): the zone reaches this far above the band's top edge (default
    # 0.001). Her crown-plait ends stop on the blindfold's top rim and drape over it in game; with above .006 those points
    # are pulled in against the scalp (never deeper than the 0.5 mm floor) so they end behind the rim.
    above = float(ub.get('above', 0.001))

    def pull(P):
        a = np.arctan2(P[:, 1] - cy, P[:, 0])
        bi = np.clip(np.digitize(a, edges) - 1, 0, nb - 1)
        r = np.maximum(np.hypot(P[:, 0], P[:, 1] - cy), 1e-6)
        zone = (P[:, 2] >= z0[bi] - 0.001) & (P[:, 2] <= z1[bi] + above) & has[bi] & (r < rin[bi] + reach)
        floor = r - head.signed(P)[0] + 0.0005          # the radius 0.5 mm above the scalp along this radial line
        target = np.minimum(r, np.maximum(rin[bi] - clear, floor))
        d = np.where(zone, target - r, 0.0)
        u = np.stack([P[:, 0] / r, (P[:, 1] - cy) / r, np.zeros_like(r)], 1)
        return u * d[:, None]

    def outside(S_):
        Pr = _rendered(S_, F) if len(F) else S_[:, 1:].reshape(-1, 3)
        return float((np.linalg.norm(pull(Pr), axis=1) > 0.0005).mean())

    if ub.get('tuck'):
        S = _band_tuck(S, F, head, B, ub['tuck'], log, nb)
    before = outside(S)
    S = S.copy()
    for k in range(1, S.shape[1]):
        S[:, k] += pull(S[:, k])
    if len(F):
        Fa = np.asarray(F, int).reshape(-1, 3)
        W = _bary()
        for _ in range(iters):
            H = _rendered(S, Fa, flat=False)                   # (faces, samples, K, 3)
            push = pull(H.reshape(-1, 3)).reshape(H.shape)
            push[:, :, 0] = 0.0
            if not np.abs(push).max() > 1e-5:
                break
            acc = np.zeros_like(S)
            wsum = np.zeros(S.shape[:2])
            for c in range(3):
                w = W[None, :, c, None]
                np.add.at(acc, Fa[:, c], (w[..., None] * push).sum(1))
                np.add.at(wsum, Fa[:, c], np.broadcast_to(w * w, push.shape[:3]).sum(1))
            step = acc / np.maximum(wsum, 1e-6)[..., None]
            step[:, 0] = 0.0
            S = S + step
    if int(ub.get('segments', 0)) > 0:
        # 2026-10-18 opt-in "segments": n (L719 Geralt 30 % longer): the band (~31 mm) is shorter than the CV spacing of a
        # long 8-CV strand (35-40 mm), so a strand can have no CV in the band zone and the drawn segment between the CV
        # above and the CV below passes OVER the band while every CV is clean (back guides over the band, linear segments:
        # L710 9, L719 15). n points per segment (and per in-between hair of the growth faces) are pulled too; the push is
        # shared by the two CVs of the segment ((1 - t) / t) and, for face hairs, by the face's guides (barycentric).
        ts = np.arange(1, int(ub['segments']) + 1) / (int(ub['segments']) + 1.0)
        Fa = np.asarray(F, int).reshape(-1, 3) if len(F) else np.arange(len(S)).reshape(-1, 1).repeat(3, 1)
        W = _bary() if len(F) else np.array([[1.0, 0.0, 0.0]])
        for _ in range(iters * 2):
            H = _rendered(S, Fa, flat=False)                   # (faces, samples, K, 3)
            acc = np.zeros_like(S)
            wsum = np.zeros(S.shape[:2])
            moved = 0.0
            for t in ts:
                P = H[:, :, :-1] * (1 - t) + H[:, :, 1:] * t   # (faces, samples, K - 1, 3)
                push = pull(P.reshape(-1, 3)).reshape(P.shape)
                moved = max(moved, float(np.abs(push).max()))
                for c in range(Fa.shape[1]):
                    w = W[None, :, c, None]                    # (1, samples, 1)
                    for k0, wk in ((0, 1 - t), (1, t)):
                        ww = w * wk
                        np.add.at(acc[:, k0:k0 + S.shape[1] - 1], Fa[:, c], (ww[..., None] * push).sum(1))
                        np.add.at(wsum[:, k0:k0 + S.shape[1] - 1], Fa[:, c], np.broadcast_to(ww * ww, push.shape[:3]).sum(1))
            if not moved > 1e-5:
                break
            step = acc / np.maximum(wsum, 1e-6)[..., None]
            step[:, 0] = 0.0
            S = S + step
    log('  scalp_push under_band (%s): hair points in the band zone outside the band %.1f %% -> %.1f %%'
        % (ub['mesh'].split('\\')[-1], 100 * before, 100 * outside(S)))
    return S


def _band_tuck(S, F, head, B, tk, log=print, nb=72):
    """2026-10-19 opt-in under_band "tuck": {} (Oriana L836): her front drape ended 17-25 mm above the band, so a bare skin
    strip showed between the hair and the band where vanilla's hair runs down UNDER it. Near-skin guide points ("near_mm"
    12) of the front ("az" [35, 145] deg round the head axis, 90 = forward, tapered over "edge_deg" 12) between the band
    top and "zone_mm" (30) above it are stretched down along the skin (skin distance kept) so the drape's lowest hair
    reaches "depth_mm" (4) under the band top; at the parting ("part_az" 90 +- "part_half_deg" 8) it stops "part_gap_mm"
    (8) above the band. Repeated "passes" (1) times (the lowest drawn hair is re-measured each pass). The under_band clamp
    then holds that hair behind the band. -> new S"""
    import numpy as np
    cy = float(head.c[1])
    edges = np.linspace(-np.pi, np.pi, nb + 1)
    mid = (edges[:-1] + edges[1:]) / 2
    binof = lambda P: np.clip(np.digitize(np.arctan2(P[:, 1] - cy, P[:, 0]), edges) - 1, 0, nb - 1)
    bb = binof(B)
    z1 = np.array([B[bb == i, 2].max() if (bb == i).any() else np.nan for i in range(nb)])
    ok = ~np.isnan(z1)
    z1 = np.interp(mid, mid[ok], z1[ok], period=2 * np.pi)
    a0, a1 = np.radians(tk.get('az', [35, 145]))
    edge = np.radians(tk.get('edge_deg', 12))
    pa, ph = np.radians(tk.get('part_az', 90)), np.radians(tk.get('part_half_deg', 8))
    depth, H = tk.get('depth_mm', 4) / 1000.0, tk.get('zone_mm', 30) / 1000.0
    near, part_gap = tk.get('near_mm', 12) / 1000.0, tk.get('part_gap_mm', 8) / 1000.0
    S = np.asarray(S, float).copy()
    S0 = S.copy()
    for _ in range(int(tk.get('passes', 1))):
        Hr = _rendered(S, F) if len(F) else S[:, 1:].reshape(-1, 3)
        sdh = head.signed(Hr)[0]
        bh = binof(Hr)
        lo = np.full(nb, np.nan)
        for i in range(nb):
            m = (bh == i) & (sdh < near) & (Hr[:, 2] > z1[i])
            if m.any():
                lo[i] = Hr[m, 2].min()
        okl = ~np.isnan(lo)
        lo = np.interp(mid, mid[okl], lo[okl], period=2 * np.pi)
        for k in range(1, S.shape[1]):
            P = S[:, k]
            a = np.arctan2(P[:, 1] - cy, P[:, 0])
            bi = binof(P)
            sd = head.signed(P)[0]
            dpart = np.abs(np.angle(np.exp(1j * (a - pa))))
            tgt = np.minimum(z1[bi] - depth + (part_gap + depth) * np.clip(1 - dpart / ph, 0, 1), lo[bi])
            top = z1[bi] + H
            w = np.clip(np.minimum(a - a0, a1 - a) / edge, 0, 1) * (sd < near) * (P[:, 2] >= tgt) * (P[:, 2] < top) \
                * (lo[bi] < top)
            znew = top - (top - P[:, 2]) * (top - tgt) / np.maximum(top - lo[bi], 1e-4)
            znew = np.where(P[:, 2] < lo[bi], tgt, znew)
            dz = (znew - P[:, 2]) * w
            if not (np.abs(dz) > 1e-5).any():
                continue
            Q = P.copy()
            Q[:, 2] += dz
            for _ in range(3):                                  # back to the old skin distance
                sq, nq, _ = head.signed(Q)
                Q = Q + nq * ((sd - sq) * (w > 0))[:, None]
            S[:, k] = Q
    mv = np.linalg.norm(S - S0, axis=2).max(1)
    log('  scalp_push under_band tuck: %d guides stretched down to the band (max %.1f mm)'
        % (int((mv > 0.001).sum()), 1000 * mv.max()))
    return S


def _bary(n=4):
    import numpy as np
    return np.array([(i / n, j / n, 1 - i / n - j / n) for i in range(n + 1) for j in range(n + 1 - i)])


def _rendered(S, F, flat=True):
    """interpolated hairs of growth faces F (barycentric grid as growth.interp_push) -> points (flat: (N, 3), root excluded)"""
    import numpy as np
    F = np.asarray(F, int).reshape(-1, 3)
    W = _bary()
    H = W[None, :, 0, None, None] * S[F[:, 0]][:, None] + W[None, :, 1, None, None] * S[F[:, 1]][:, None] \
        + W[None, :, 2, None, None] * S[F[:, 2]][:, None]
    return H[:, :, 1:].reshape(-1, 3) if flat else H


def scalp_push(C, geo, sp, log=print):
    """2026-10-18 opt-in "scalp_push" (Geralt headband, LOCAL ONLY; L668 magenta-scalp test): the player's head can come
    from a head mod whose scalp has another shape than the vanilla head the Studio grooms on (mod0W2GeraltFaceandBody:
    back crown up to +7 mm, nape median +4 / max +12.6 mm outside the vanilla h_01). Hair lying on the vanilla scalp is
    then drawn UNDER that head's opaque scalp: the "white helmet" over the hair. The guides (growth.head_push, carried
    down the strand) and their interpolated rendered hairs (growth.interp_push) are moved out of THAT head; roots stay.
    The head mod's mesh is only read (gamefiles.outfit_source), nothing of it is written to the build.
    Only points on the scalp band (z above head top - "band") are pushed, the move is carried down the strand and capped
    per guide ("max_move"), so the hanging hair shifts with its root part instead of puffing out (the face mod's
    longer neck would otherwise push the hanging ends 30-70 mm out).
      {"head_mods": ["mod0W2GeraltFaceandBody"], "head": <rel, default C["head_mesh"]>, "margin": .003,
       "band": .13, "interp_margin": .002, "max_move": .012,
       "under_band": {"mesh": <headband mesh rel>, "chunk": 1, "mods": [...], "clear": .001},   (optional, _band_clamp)
       "collider_fit": {"clear": .001, "min_r": .002}}   (optional, _collider_fit: no collider holds the moved rest hair)"""
    import numpy as np
    import gamefiles as G
    import growth
    rel = sp.get('head') or C['head_mesh']
    with G.outfit_source(sp.get('head_mods') or []):
        head = growth.Head(rel)
    margin, cap = float(sp.get('margin', 0.003)), float(sp.get('max_move', 0.012))
    zmin = head.top - float(sp.get('band', 0.13))
    before = _under_share(geo, head)
    G0 = np.array([np.asarray(g, float) for g in geo['guides']])
    S = G0.copy()
    # opt-in "skip_hair": true (L817): the geo is a Studio export of a project whose base build was already pushed;
    # pushing the edited guides out of the head a second time moved 5-6 temple strands over the band (Studio project 0
    # over the band at rest, its config build 6). Only the under_band clamp and the collider fit run.
    for k in range(1, S.shape[1]) if not sp.get('skip_hair') else ():
        sd, nrm, _ = head.signed(S[:, k])
        need = np.where(S[:, k, 2] > zmin, np.maximum(margin - sd, 0.0), 0.0)
        S[:, k:] += (nrm * need[:, None])[:, None, :]
    F = geo.get('F') or []
    if sp.get('interp', True) and len(F) and not sp.get('skip_hair'):
        S, _st = growth.interp_push(list(S), F, head, margin=float(sp.get('interp_margin', 0.002)), max_move=cap, log=log)
        S = np.array(S)
    if sp.get('under_band'):
        S = _band_clamp(S, F, head, sp['under_band'], log)
    off = S - G0
    L = np.linalg.norm(off, axis=2, keepdims=True)
    S = G0 + off * np.minimum(1.0, cap / np.maximum(L, 1e-9))
    mv = np.linalg.norm(S - G0, axis=2).max(1)
    geo = dict(geo, guides=S.tolist())
    log('  studio_geo scalp_push (%s %s): guide points under that scalp %.1f %% -> %.1f %%, %d guides moved (median %.1f, max %.1f mm)'
        % ('+'.join(sp.get('head_mods') or ['vanilla']), rel.split('\\')[-1], 100 * before, 100 * _under_share(geo, head),
           int((mv > 0.0005).sum()), 1000 * float(np.median(mv[mv > 0.0005])) if (mv > 0.0005).any() else 0.0, 1000 * float(mv.max())))
    if sp.get('collider_fit') and geo.get('colliders'):
        geo = _collider_fit(geo, sp['collider_fit'], log)
    return geo


def _drape_smooth(G0, Gd, C, R, bc, pad, ds, log=print):
    """opt-in collider_fit.drape_smooth {"iters": 4, "carry": .85} (L1000, Geralt collar ring): drape_body pushes only the
    points inside a body collider and keeps segment lengths, so a strand whose middle meets the collar top gets a kink
    there and its tail swings back in under it (hw_preview back34: zigzag, tangled lower back). The push field D = draped -
    rest is smoothed along each strand (1-2-1) and carried down (a CV keeps at least `carry` x the push of the CV above it,
    roots stay), so the hair flows out over the collar edge; then drape again so nothing is left inside."""
    import numpy as np
    import body_colliders as BC
    ds = ds if isinstance(ds, dict) else {}
    it, carry = int(ds.get('iters', 4)), float(ds.get('carry', 0.85))
    for _ in range(it):
        D = Gd - G0
        S = D.copy()
        S[:, 1:-1] = 0.25 * D[:, :-2] + 0.5 * D[:, 1:-1] + 0.25 * D[:, 2:]
        S[:, -1] = 0.5 * (D[:, -2] + D[:, -1])
        S[:, 0] = 0.0
        for k in range(2, S.shape[1]):
            n0, n1 = np.linalg.norm(S[:, k - 1], axis=1), np.linalg.norm(S[:, k], axis=1)
            low = n1 < carry * n0
            S[low, k] = S[low, k - 1] * carry
        Gd, rep = BC.drape(G0 + S, C, R, bc, pad=pad)
    mv = np.linalg.norm(Gd - G0, axis=2).max(1)
    log('  studio_geo collider_fit drape_smooth: %d iters, carry %.2f: guides moved > 5 mm %d (max %.0f mm), inside after %d'
        % (it, carry, int((mv > 0.005).sum()), 1000 * float(mv.max()), rep['points_inside_after']))
    return Gd


def _drape_trim(Gd, G0, dt, log=print):
    """collider_fit.drape_trim (see _collider_fit): guides whose draped tail went flat end at the CV before it."""
    import numpy as np
    dt = dt if isinstance(dt, dict) else {}
    vmin, mmv = float(dt.get('vert', 0.5)), float(dt.get('min_mv', 0.003))
    ymin, zmax, kmin = float(dt.get('y_min', -0.06)), float(dt.get('z_max', 1.70)), int(dt.get('min_keep', 4))
    K = Gd.shape[1]
    cut, lost = 0, []
    for g in range(len(Gd)):
        s = np.diff(Gd[g], axis=0)
        vert = -s[:, 2] / np.maximum(np.linalg.norm(s, axis=1), 1e-9)
        mvk = np.linalg.norm(Gd[g] - G0[g], axis=1)
        ks = [k for k in range(kmin, K) if vert[k - 1] < vmin and mvk[k] > mmv
              and Gd[g, k, 1] > ymin and Gd[g, k, 2] < zmax]
        if not ks:
            continue
        k = ks[0]
        P = Gd[g, :k]
        arc = np.r_[0.0, np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))]
        t = np.linspace(0.0, arc[-1], K)
        full = float(np.linalg.norm(s, axis=1).sum())
        Gd[g] = np.stack([np.interp(t, arc, P[:, c]) for c in range(3)], 1)
        cut += 1
        lost.append(full - arc[-1])
    log('  studio_geo scalp_push.collider_fit drape_trim: %d guides end at the collar (cut median %.1f, max %.1f mm)'
        % (cut, 1000 * float(np.median(lost)) if lost else 0.0, 1000 * float(max(lost)) if lost else 0.0))
    return Gd


def drape_after_split(geo, cf, log=print):
    """opt-in collider_fit.drape_trim.after_split: true (L999, Geralt Ciri look): frozen_fur.free_below makes NEW simulated
    guides (the lower parts of the frozen ones, rooted under the band) after scalp_push, so the drape / drape_trim never saw
    them. L998 in game (dialogue): one of them (temple -> jaw, in front of the ear) ended 4 mm INSIDE the collar capsule
    (torso3): a narrow strip of hair jittering sideways. Re-run drape_body + drape_trim on the simulated fur after the split."""
    import numpy as np
    import rig
    import body_colliders as BC
    names, _par, M = rig.load_rig(cf.get('rig', 'characters\\base_entities\\man_base\\man_base.w2rig'))
    sp, caps = geo['colliders']['spheres'], [tuple(c) for c in geo['colliders']['capsules']]
    C = np.array([(np.array(list(s['local']) + [1.0]) @ np.asarray(M[names.index(s['bone_name'])], float))[:3] for s in sp])
    R = np.array([float(s['r']) for s in sp])
    fixed = {i for i, s in enumerate(sp) if s['bone_name'] != 'head'} | {int(i) for i in cf.get('drape_head', [])}
    bc = [(a, b) for a, b in caps if a in fixed and b in fixed] + [(i, i) for i in sorted(fixed)]
    G0 = np.array([np.asarray(g, float) for g in geo['guides']])
    d0 = BC.capsule_sdf(G0[:, 1:].reshape(-1, 3), C, R, bc)[0]
    Gd, _ = BC.drape(G0, C, R, bc, pad=float(cf['drape_body']))
    if cf.get('drape_smooth'):
        Gd = _drape_smooth(G0, Gd, C, R, bc, float(cf['drape_body']), cf['drape_smooth'], log)
    Gd = _drape_trim(Gd, G0, cf['drape_trim'], log)
    Gd, rep = BC.drape(Gd, C, R, bc, pad=float(cf['drape_body']))
    log('  studio_geo collider_fit drape_trim.after_split: points inside body colliders %d -> %d, %s'
        % (int((d0 < 0).sum()), rep['points_inside_after'], rep))
    return dict(geo, guides=Gd.tolist())


def _collider_fit(geo, cf, log=print):
    """2026-10-18 opt-in scalp_push.collider_fit {"rig": <w2rig, default man_base>, "clear": .001, "min_r": .002, "passes": 6}:
    no collider may hold REST hair. L678 (stiffness 1): the 8 mm band-ring spheres (z 1.818) held the pushed / band-clamped
    hair up to 6 mm deep, 78 points beyond their centres, and in game the strands were thrown out over the band. Each sphere
    holding rest points (k >= 1) shrinks toward its far side (away from those points, so the new sphere lies inside the old
    one and still blocks the same side) until they are `clear` outside; a capsule through rest points shrinks both end
    spheres away from its axis the same way. Centres go back to bone-local through the rig bind (row vectors)."""
    import numpy as np
    import rig
    names, _par, M = rig.load_rig(cf.get('rig', 'characters\\base_entities\\man_base\\man_base.w2rig'))
    col = geo['colliders']
    sp = [dict(s) for s in col['spheres']]
    caps = [tuple(c) for c in col['capsules']]
    B = [np.asarray(M[names.index(s['bone_name'])], float) for s in sp]
    C = np.array([(np.array(list(s['local']) + [1.0]) @ b)[:3] for s, b in zip(sp, B)])
    R = np.array([float(s['r']) for s in sp])
    fixed = set()
    if cf.get('drape_body'):
        # opt-in "drape_body": <pad m> (L818): the body / armour colliders (every bone but the head) keep their size and the
        # rest hair is draped OUT of them instead (body_colliders.drape: follow-the-leader, segment lengths kept, roots
        # stay). L817 (30 % longer hem): the fit shrank the big back / shoulder spheres by 20-35 mm and in game the hair went
        # into the armour and hung straight past the shoulders.
        import body_colliders as BC
        fixed = {i for i, s in enumerate(sp) if s['bone_name'] != 'head'}
        # opt-in "drape_head": [sphere indices] (L104x, Geralt Ciri look): head spheres fitted under the cheek skin
        # (collar_shell.py face stage) hold the side locks that rest on the cheek; the refit below would shrink them to
        # nothing, so they keep their size and the rest hair is draped out of them like out of the body colliders.
        fixed |= {int(i) for i in cf.get('drape_head', [])}
        bc = [(a, b) for a, b in caps if a in fixed and b in fixed] + [(i, i) for i in sorted(fixed)]
        G0 = np.array([np.asarray(g, float) for g in geo['guides']])
        Gd, rep = BC.drape(G0, C, R, bc, pad=float(cf['drape_body']))
        if cf.get('drape_smooth'):
            Gd = _drape_smooth(G0, Gd, C, R, bc, float(cf['drape_body']), cf['drape_smooth'], log)
        if cf.get('drape_trim'):
            # opt-in "drape_trim": {"vert": .5, "min_mv": .003, "y_min": -.06, "z_max": 1.70, "min_keep": 4} (L930, Geralt
            # Ciri look): the drape swings tails that end ON the collar / shoulder spheres out flat (follow-the-leader keeps
            # the length, the push is straight up there), and in game (dialogue) those flat ends slide on the animated collar:
            # a horizontal flickering pack at the jaw. In the zone (draped CV y > y_min, z < z_max) a guide whose draped
            # segment k-1 -> k is flatter than `vert` (cos to straight down) and moved > min_mv ends at CV k-1 instead
            # (resampled to the same CV count); k < min_keep is left alone. The back keeps the full drape.
            Gd = _drape_trim(Gd, G0, cf['drape_trim'], log)
            Gd, rep = BC.drape(Gd, C, R, bc, pad=float(cf['drape_body']))
        geo = dict(geo, guides=Gd.tolist())
        log('  studio_geo scalp_push.collider_fit drape_body: %d body colliders kept, %s' % (len(fixed), rep))
    if cf.get('keep'):
        # opt-in "keep": [sphere indices] (L929, Ciri): these spheres keep their size, no drape. Ciri's ear spheres hold
        # strands rooted inside them; the fit shrank them to 1-12 mm and in game the ear tips showed through the side hair.
        fixed |= {int(i) for i in cf['keep']}
        log('  studio_geo scalp_push.collider_fit keep: %d spheres not refit' % len(cf['keep']))
    P = np.array([np.asarray(g, float)[1:] for g in geo['guides']]).reshape(-1, 3)
    clear, rmin = float(cf.get('clear', 0.001)), float(cf.get('min_r', 0.002))

    def cap_d(a, b):
        p, v = C[a], C[b] - C[a]
        t = np.clip(((P - p) @ v) / max(float(v @ v), 1e-12), 0.0, 1.0)
        q = p + t[:, None] * v
        return np.linalg.norm(P - q, axis=1) - (R[a] + (R[b] - R[a]) * t), q

    def depth():
        ds = (R[:, None] - np.linalg.norm(C[:, None] - P[None], axis=2)).max(1)
        dc = np.array([-cap_d(a, b)[0].min() for a, b in caps]) if caps else np.zeros(0)
        return ds, dc

    def shrink(i, pts, u):
        far = C[i] + R[i] * u
        lo, hi = 0.0, R[i]
        for _ in range(32):
            r = 0.5 * (lo + hi)
            if (np.linalg.norm(pts - (far - r * u), axis=1) >= r + clear).all():
                lo = r
            else:
                hi = r
        r = max(lo, min(rmin, R[i]))
        C[i], R[i] = far - r * u, r

    ds0, dc0 = depth()
    touched = set()
    for _ in range(int(cf.get('passes', 6))):
        moved = False
        for i in range(len(sp)):
            if i in fixed:
                continue
            m = np.linalg.norm(P - C[i], axis=1) < R[i] + clear
            if m.any() and R[i] > rmin:
                u = C[i] - P[m].mean(0)
                n = np.linalg.norm(u)
                shrink(i, P[m], u / n if n > 1e-9 else np.array([0.0, 0.0, 1.0]))
                touched.add(i)
                moved = True
        for a, b in caps:
            if a in fixed and b in fixed:
                continue
            d, q = cap_d(a, b)
            m = d < clear
            if m.any():
                # the tube bows into the hair between its end spheres (chord of the ring): both ends step away from the hair
                # (q - p is perpendicular to the axis) by half the missing depth; shrinking the sphere toward its far side
                # moves its near surface 2x, at the radius floor the centre moves instead
                u = (q[m] - P[m]).mean(0)
                n = np.linalg.norm(u)
                u = u / n if n > 1e-9 else np.array([0.0, 0.0, 1.0])
                half = 0.5 * (clear - float(d[m].min())) + 1e-5
                for i in (a, b):
                    if i in fixed:
                        continue
                    s = min(half, max(R[i] - rmin, 0.0))
                    C[i] = C[i] + s * u + 2.0 * (half - s) * u
                    R[i] -= s
                    touched.add(i)
                moved = True
        if not moved:
            break
    ds1, dc1 = depth()
    for i in touched:
        sp[i]['local'] = [float(x) for x in (np.append(C[i], 1.0) @ np.linalg.inv(B[i]))[:3]]
        sp[i]['r'] = float(R[i])
    log('  studio_geo scalp_push.collider_fit: %d spheres refit; rest hair inside spheres %d -> %d (max %.1f -> %.1f mm), '
        'capsules %d -> %d (max %.1f -> %.1f mm)'
        % (len(touched), int((ds0 > 0.0005).sum()), int((ds1 > 0.0005).sum()), 1000 * max(ds0.max(), 0), 1000 * max(ds1.max(), 0),
           int((dc0 > 0.0005).sum()), int((dc1 > 0.0005).sum()), 1000 * max(dc0.max(initial=0), 0), 1000 * max(dc1.max(initial=0), 0)))
    return dict(geo, colliders=dict(col, spheres=sp))


def apply(C, data, redfur_rel, n_bones, log=print):
    """data = the built redfur bytes -> (new redfur bytes, extra files [(game path, bytes)])"""
    sg = C['studio_geo']
    DE = _desk_export()
    from cr2w_edit import CR2WDoc, Chunk, v_import
    import redfur as RF
    geo = load_geo(sg['geo'])
    if sg.get('resample_cvs'):
        geo = resample(geo, int(sg['resample_cvs']), log, sg.get('resample_mode', 'uniform'),   # opt-in 'adaptive' (L353)
                       float(sg.get('resample_power', 1.5)))                                # opt-in 'root' (L806)
    if sg.get('band_split') is not None:
        sp_ = sg.get('scalp_push') or {}
        geo = band_split(C, geo, dict({'head': sp_.get('head')} if sp_.get('head') else {}, **sg['band_split']),
                         sp_.get('under_band'), log)
    if sg.get('band_snap') is not None:
        geo = band_snap(geo, sg['band_snap'], (sg.get('scalp_push') or {}).get('under_band'), log)
    if sg.get('collision_r_shift') and geo.get('collision_r') and not geo.get('colliders'):   # a full collider set (2026-10-14) already has the new indices
        # 2026-10-14 opt-in {"from": 45, "old_outfit": 28}: this build has a different number of outfit colliders than the
        # config the Studio project was made on (outfit_mods / back_items), so the sphere radius overrides saved by the
        # project that sit after the outfit block (the head twins, index >= from) move by (this build - old_outfit)
        sh = sg['collision_r_shift']
        by = int(C.get('_n_outfit_spheres', sh['old_outfit'])) - int(sh['old_outfit'])
        geo = dict(geo)
        geo['collision_r'] = [dict(e, i=int(e['i']) + (by if int(e['i']) >= int(sh['from']) else 0))
                              for e in geo['collision_r']]
    if sg.get('split_faces'):
        geo = split_faces(geo, sg['split_faces'], log)
    if sg.get('undercoat'):
        geo = undercoat(geo, sg['undercoat'], log)
    if sg.get('hairline_fill'):
        # a list = several fills in order (L1123: parting layer, then a short layer on the remaining bare spots)
        for hf_ in (sg['hairline_fill'] if isinstance(sg['hairline_fill'], list) else [sg['hairline_fill']]):
            geo = hairline_fill(C, geo, hf_, log)
    if sg.get('scalp_push'):
        geo = scalp_push(C, geo, sg['scalp_push'], log)
    if geo.get('unique_uv') and not sg.get('density_png'):
        # 2026-10-15 (Ewald l345): a project with a painted stiffness map exports its faces with UNIQUE atlas UVs; the
        # density then lives only in the re-baked atlas. Without it the build reads the old density texture at atlas
        # UVs (sparse, stringy hair) and drops the stiffness map. Make the config with studio_to_config.py.
        raise SystemExit('studio_geo: %s has unique (atlas) UVs but the config has no studio_geo.density_png / stiffness: '
                         'make it with tools/hairworks/studio_to_config.py' % sg['geo'])
    if sg.get('frozen_fur') is not None:
        geo, sg['_frozen_geo'] = frozen_split(geo, sg['frozen_fur'], (sg.get('scalp_push') or {}).get('under_band'), log)
        if sg['frozen_fur'].get('floor'):
            sp = sg.get('scalp_push') or {}
            sg['_frozen_geo'] = frozen_floor(sg['_frozen_geo'], sg['frozen_fur']['floor'], sp.get('head') or C['head_mesh'],
                                             sp.get('head_mods'), log)
        cf = (sg.get('scalp_push') or {}).get('collider_fit') or {}
        if isinstance(cf.get('drape_trim'), dict) and cf['drape_trim'].get('after_split') and cf.get('drape_body'):
            geo = drape_after_split(geo, cf, log)
    if sg.get('drop_faces'):
        geo = drop_faces(geo, sg['drop_faces'], log)
    bi =[int(round(x)) for b in geo['bone_idx'] for x in b]
    if bi and max(bi) >= n_bones:
        raise SystemExit('studio_geo: bone slot %d >= %d bones of this build: base the config on the config the Studio '
                         'project was made on' % (max(bi), n_bones))
    doc = CR2WDoc.parse(data)
    ch = Chunk(doc, 0)
    DE.set_geometry(doc, ch, geo)
    files = []
    base_rel = redfur_rel[:-len('.redfur')]
    if sg.get('stiffness'):
        rel = base_rel + '_stiffness.xbm'
        files.append((rel, DE.stiffness_xbm(_path(sg['stiffness']), rel)))
        imp = v_import(doc.add_import(rel, 'CBitmapTexture'))
        for pth, typ, val in ((['physicalMaterials', 'stiffness', 'stiffnessTex'], 'handle:CBitmapTexture', imp),
                              (['physicalMaterials', 'stiffness', 'rootStiffnessTex'], 'handle:CBitmapTexture', imp),
                              (['physicalMaterials', 'stiffness', 'stiffnessTexChannel'], 'EHairTextureChannel', struct.pack('<H', doc.name('RED'))),
                              (['physicalMaterials', 'stiffness', 'rootStiffnessTexChannel'], 'EHairTextureChannel', struct.pack('<H', doc.name('GREEN')))):
            DE.apply_value(doc, ch, pth, typ, val)
    if sg.get('density_png'):
        rel = base_rel + '_density%s.xbm' % sg.get('density_tag', '')   # opt-in tag 2026-10-16: several density variants in one batch mod
        dpng = _path(sg['density_png'])
        if C.get('look', {}).get('density_flat') is not None:
            # 2026-10-16 opt-in look.density_flat (hairpipe): a painted project keeps its re-baked atlas as the density
            # map, scaled by the flat share so the game thins the hair the way the Studio's density < 1 does
            from PIL import Image
            import numpy as np
            pf = float(np.clip(float(C['look']['density_flat']), 0.0, 1.0))
            arr = (np.asarray(Image.open(dpng).convert('RGB'), np.float64) * pf).round().clip(0, 255).astype(np.uint8)
            dpng = dpng[:-4] + '_flat%03d.png' % int(round(pf * 1000))
            Image.fromarray(arr).save(dpng)
            log('  studio_geo density_png x density_flat %.3f -> %s' % (pf, os.path.relpath(dpng, ROOT)))
        files.append((rel, DE.stiffness_xbm(dpng, rel, 2048)))
        imp = v_import(doc.add_import(rel, 'CBitmapTexture'))
        DE.apply_value(doc, ch, ['physicalMaterials', 'volume', 'densityTex'], 'handle:CBitmapTexture', imp)
        DE.apply_value(doc, ch, ['physicalMaterials', 'volume', 'densityTexChannel'], 'EHairTextureChannel', struct.pack('<H', doc.name('RED')))
    if sg.get('colour_png'):
        rel = base_rel + '_colour.xbm'
        files.append((rel, DE.stiffness_xbm(_path(sg['colour_png']), rel, 2048)))
        imp = v_import(doc.add_import(rel, 'CBitmapTexture'))
        for k in ('rootColorTex', 'tipColorTex'):
            DE.apply_value(doc, ch, ['graphicalMaterials', 'color', k], 'handle:CBitmapTexture', imp)
    ch.commit()
    out = doc.to_bytes()
    RF.verify(out)
    b = geo.get('budget', {})
    log('  studio_geo %s: %d guides, %d growth faces (Studio budget %.2f of %s)%s' % (
        sg['geo'], len(geo['guides']), len(geo['F']), b.get('ratio', 0), b.get('ref'),
        '; maps: ' + ', '.join(r for r, _ in files) if files else ''))
    return out, files
