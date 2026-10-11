r"""gapfill - opt-in (growth key "gap_fill"; HairLab L145 / L147): guides where the vanilla cards cover the skin but no strand passes.

  "gap_fill": {"gap": 0.008,        # a skin vertex is bare when no strand point is within gap (m) ...
               "card": 0.012,       # ... while a vanilla card vertex is within card (m)
               "z_below": 0.06, "front": 0.20,   # region: skin above head centre z - z_below, behind centre y + front
               "min_verts": 3,
               "spacing": 0.0}      # > 0: one guide per `spacing` m of bare skin (dozens) instead of one per 12 mm cluster

Each bare spot gets a copy of the nearest strand (growth.add_guide_copies: root snapped onto the scalp, copies touching
the skin skipped; the copy keeps the strand's shape, so it is combed like its neighbour) and the growth mesh is
re-triangulated. Nothing runs unless the key is set."""
import numpy as np
from scipy.spatial import cKDTree


def fill(res, head, cards, o, log=print):
    import growth
    gap, cd = float(o.get('gap', 0.008)), float(o.get('card', 0.012))
    c = np.asarray(head.c, float)
    S = res['S']
    dense = np.concatenate([np.linspace(a, b, 4, endpoint=False) for s_ in S for a, b in zip(np.asarray(s_['p'])[:-1], np.asarray(s_['p'])[1:])])
    V = head.v[(head.v[:, 2] > c[2] - float(o.get('z_below', 0.06))) & (head.v[:, 1] < c[1] + float(o.get('front', 0.2)))]
    bare = (cKDTree(cards).query(V)[0] < cd) & (cKDTree(dense).query(V)[0] > gap)
    B = V[bare]
    if not len(B):
        log('  gap_fill: no bare skin')
        return res
    R = np.array([np.asarray(s_['p'], float)[0] for s_ in S])
    kd = cKDTree(R)
    tb = cKDTree(B)
    sp = float(o.get('spacing', 0.0))
    pts = []
    if sp > 0:
        taken = np.zeros(len(B), bool)
        for i in range(len(B)):
            if taken[i]:
                continue
            pts.append(B[i])
            for q in tb.query_ball_point(B[i], sp):
                taken[q] = True
    else:
        lab = np.full(len(B), -1)
        k = 0
        for i in range(len(B)):
            if lab[i] >= 0:
                continue
            st = [i]
            while st:
                j = st.pop()
                if lab[j] >= 0:
                    continue
                lab[j] = k
                st += [q for q in tb.query_ball_point(B[j], 0.012) if lab[q] < 0]
            k += 1
        for l in range(k):
            m = B[lab == l]
            if len(m) >= int(o.get('min_verts', 3)):
                pts.append(m.mean(0))
    ent = []
    for t0 in pts:
        _d, i = kd.query(t0)
        ent.append({'src': R[i].tolist(), 'd': (t0 - R[i]).tolist()})
    log('  gap_fill: %d bare spots (gap %.0f mm, %d bare skin vertices)' % (len(ent), gap * 1000, len(B)))
    if not ent:
        return res
    res2, n = growth.add_guide_copies(res, head, ent, tol=1e-6, log=log)
    return res2


def drop_long(res, head, o, log=print):
    """opt-in (growth key "drop_long" {len .35, front_y 0, z_rel .08}): drops guides rooted in front of head centre y + front_y and above
    centre z + z_rel whose arc length exceeds `len` (m): the front parting clump that stands up / folds (Emhyr L151: 2 guides of
    406 / 423 mm among 250-300 mm neighbours). Growth mesh re-triangulated (growth.reindex)."""
    import growth
    c = np.asarray(head.c, float)
    keep = []
    for i, s_ in enumerate(res['S']):
        p = np.asarray(s_['p'], float)
        L = float(np.linalg.norm(np.diff(p, axis=0), axis=1).sum())
        r = p[0] - c
        bad = L > float(o.get('len', 0.35)) and r[1] > float(o.get('front_y', 0.0)) and r[2] > float(o.get('z_rel', 0.08))
        if not bad:
            keep.append(i)
    log('  drop_long: %d of %d guides dropped (front parting, longer than %.0f mm)' % (len(res['S']) - len(keep), len(res['S']), float(o.get('len', 0.35)) * 1000))
    return res if len(keep) == len(res['S']) else growth.reindex(res, keep)


def drop_tangled(res, head, o, log=print):
    """opt-in (growth key "drop_tangled" {ratio 3}): drops guides folded back on themselves: arc length / root-to-tip chord > ratio
    (Emhyr L152: hairpin guides, ratio 3-13, turns up to 172 deg, tips piled on a few shared points; they surface as a light crumpled
    patch at the front parting from the front camera). Growth mesh re-triangulated (growth.reindex)."""
    import growth
    keep = []
    for i, s_ in enumerate(res['S']):
        p = np.asarray(s_['p'], float)
        L = float(np.linalg.norm(np.diff(p, axis=0), axis=1).sum())
        ch = float(np.linalg.norm(p[-1] - p[0]))
        if L / max(ch, 1e-6) <= float(o.get('ratio', 3.0)):
            keep.append(i)
    log('  drop_tangled: %d of %d guides dropped (path / chord > %.1f)' % (len(res['S']) - len(keep), len(res['S']), float(o.get('ratio', 3.0))))
    return res if len(keep) == len(res['S']) else growth.reindex(res, keep)
