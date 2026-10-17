r"""desk_colfit - fit HairWorks collision spheres / capsules to the geometry the Studio shows (2026-10-14).

Called by the Studio (src/colliders.cpp, Colliders section / studio_fit_colliders). Reuses the pipeline's fitters
(collision.outfit_shapes for outfit meshes, backitems.fit_item for scabbards / swords / hood rows), so a Studio fit and a
build-time fit of the same meshes agree. Nothing here is written to a build: the result goes back to the Studio, which keeps
it in the project ("colliders") and writes it into the fur at export (desk_export.set_colliders).

  py desk_colfit.py list <cfg>                    -> JSON on stdout: the fit targets of the config (outfit meshes, back items)
  py desk_colfit.py fit <request.json> <result.json>
  py desk_colfit.py groups <cfg>                  -> (re)build build/desktop_studio/<cfg>/colgroups.json (~40 s)

Colliders are split in groups: 0 = base (cranium, neck, ears: kept), 1 = outfit / back item (replaced by a fit),
2 = manual (kept when keep_manual). A pack made by the pipeline does not say which spheres are group 1; `groups` runs the
pipeline's generate() once (deterministic, nothing written to a build) and caches its outfit flags next to desk.json.

Sphere budget (HairWorks vanilla max 85 spheres / 75 capsules in one fur): every sphere that is not part of a capsule gets a
twin 1 mm away and a capsule to it (only capsules collide, collision.fit); twins count. The fit shrinks the per-target sphere
counts until base + fitted + twins fit.
"""
import json
import os
import sys

import numpy as np
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HW = os.path.join(ROOT, 'tools', 'hairworks')
sys.path.insert(0, HW)
sys.path.insert(0, os.path.join(ROOT, 'tools'))
OUT_BASE = os.path.join(ROOT, 'build', 'desktop_studio')
BAND = 0.008          # a target vertex counts as covered within this distance of a collider surface (m)


def _quiet(*a, **k):
    pass


def _ctx(cfg, log=_quiet):
    import hairpipe as H
    import gamefiles as G
    C = H.load_config(cfg)
    info = H.discover(C, log=_quiet)
    return H, G, C, info


def _outfit_paths(C, info):
    app = C.get('appearance') or next(iter(info.get('outfit', {})), '')
    return list((C.get('outfits') or {}).get(app) or info.get('outfit', {}).get(app, []))


def targets(cfg):
    """fit targets of a config: [{id, label, kind: outfit | item, path, default, note}]"""
    H, G, C, info = _ctx(cfg)
    out = []
    mods = (C.get('growth') or {}).get('outfit_mods') or []
    for rel in _outfit_paths(C, info):
        n = os.path.basename(rel.replace('\\', '/'))
        out.append(dict(id='outfit:' + n, label=n[:-len('.w2mesh')] if n.endswith('.w2mesh') else n, kind='outfit', path=rel,
                        default=True, note='outfit mesh (collar, armour, hood, body)' + (' [from ' + ', '.join(mods) + ']' if mods else '')))
    for it in (C.get('growth') or {}).get('back_items') or []:
        nm = it.get('name', os.path.basename(it['mesh']))
        out.append(dict(id='item:' + nm, label=nm, kind='item', path=it['mesh'], default=not it.get('display_only'),
                        note=('hood / cape rows of the outfit mesh' if it.get('rows') else 'part of an outfit mesh' if it.get('box') and not it.get('show') else
                              'sword' if it.get('static') else 'scabbard') + (' (shown only in the pipeline)' if it.get('display_only') else '')))
    return out


# -------------------------------------------------------------------------------------------------------- group flags
def groups(cfg, pack_dir=None, log=_quiet):
    """-> {n, flags[], caps[]}: the pipeline's outfit flag of every sphere of the config's current build"""
    pack_dir = pack_dir or os.path.join(OUT_BASE, cfg)
    side = os.path.join(pack_dir, 'colgroups.json')
    dj = json.load(open(os.path.join(pack_dir, 'desk.json')))
    n = len(dj['spheres'])
    if os.path.exists(side):
        g = json.load(open(side))
        if g.get('n') == n and g.get('src_hash') == dj.get('src_hash'):
            return g
    import hairpipe as H
    C = H.load_config(cfg)
    st = H.generate(C, None, log=_quiet)
    col = st['col']
    if len(col['rad']) != n:
        raise SystemExit('colgroups: the pipeline build of %s has %d spheres, the pack %d: re-run desk_prep' % (cfg, len(col['rad']), n))
    g = dict(n=n, src_hash=dj.get('src_hash'), flags=[bool(x) for x in col['outfit_flag']], caps=[int(x) for x in col['cap']])
    json.dump(g, open(side, 'w'))
    return g


# -------------------------------------------------------------------------------------------------------- geometry
def cap_dist(P, a, b, ra, rb):
    """distance of points P to the surface of the capsule (a, ra)-(b, rb); < 0 inside"""
    ab = b - a
    L2 = float(ab @ ab)
    if L2 < 1e-12:
        return np.linalg.norm(P - a, axis=1) - ra
    t = np.clip(((P - a) @ ab) / L2, 0, 1)
    q = a + t[:, None] * ab
    return np.linalg.norm(P - q, axis=1) - (ra * (1 - t) + rb * t)


def coverage(P, S, caps):
    """fraction of the points P within BAND of the union of the capsules (sphere pairs of S = [(centre, r)])"""
    if not len(P):
        return None
    best = np.full(len(P), 1e9)
    for i, j in caps:
        best = np.minimum(best, cap_dist(P, S[i][0], S[j][0], S[i][1], S[j][1]))
    return float((best <= BAND).mean())


def _remap_bone(name, allowed, names, par):
    n = name
    while n not in allowed:
        if n not in names:
            n = None
            break
        p = par[names.index(n)]
        n = names[p] if p >= 0 else None
        if n is None:
            break
    if n is None:
        n = 'torso3' if 'torso3' in allowed else next(iter(allowed))
    return n


# -------------------------------------------------------------------------------------------------------- fit
def fit(req, log=print):
    import collision
    import backitems
    import growth
    import rig
    cfg = req['cfg']
    pack_dir = req.get('pack_dir') or os.path.join(OUT_BASE, cfg)
    dj = json.load(open(os.path.join(pack_dir, 'desk.json')))
    H, G, C, info = _ctx(cfg)
    gw = C.get('growth') or {}
    mods = gw.get('outfit_mods') or []
    bones = dj['bones']
    bnames = [b['name'] for b in bones]
    allowed = set(bnames)
    head = growth.Head(C.get('head_mesh') or info['heads'][0])
    hair = np.asarray(json.load(open(req['hair'])), float).reshape(-1, 3) if req.get('hair') else None
    main = C.get('rig') or next((x for x in info.get('rigs', []) if 'base_entities' in x), None) or info['rigs'][0]
    rnames, rpar, _M = rig.load_rig(main)

    # ---- current set with groups
    cur = req['spheres']
    capsCur = [tuple(c) for c in req['capsules']]
    flags = None
    if any(s.get('grp') is None for s in cur):
        g = groups(cfg, pack_dir)
        flags = g['flags']
        if len(flags) != len(cur):
            raise SystemExit('the Studio collider set (%d) differs from the pack (%d): reset the colliders first' % (len(cur), len(flags)))
    grp = [int(s['grp']) if s.get('grp') is not None else (1 if flags[i] else 0) for i, s in enumerate(cur)]
    keep_manual = req.get('keep_manual', True)
    keepi = [i for i, s in enumerate(cur) if grp[i] == 0 or (grp[i] == 2 and keep_manual)]
    oldi = [i for i in range(len(cur)) if i not in set(keepi)]
    remap = {o: n for n, o in enumerate(keepi)}
    kept_caps = [(remap[a], remap[b]) for a, b in capsCur if a in remap and b in remap]
    nbase = len(keepi)
    S_budget = int(req.get('budget', {}).get('spheres', 85))
    C_budget = int(req.get('budget', {}).get('capsules', 75))

    # ---- targets
    want = list(req.get('targets') or [])
    tg = {t['id']: t for t in targets(cfg)}
    for t in want:
        if t not in tg:
            raise SystemExit('unknown fit target %r (have %s)' % (t, sorted(tg)))
    out_paths = [tg[t]['path'] for t in want if tg[t]['kind'] == 'outfit']
    items = {it.get('name', os.path.basename(it['mesh'])): it for it in gw.get('back_items') or []}
    sel_items = [dict(items[t[5:]]) for t in want if t.startswith('item:') and t[5:] in items]
    pad = req.get('pad_mm')
    pad = None if pad is None else float(pad) / 1000.0
    reach = req.get('reach_mm')
    reach = None if reach is None else float(reach) / 1000.0
    nmax0 = int(gw.get('outfit_spheres', 36))
    notes = []

    # ---- points of every target (coverage) + the fit at a scale
    def outfit_target_points():
        P, N, Bn = collision.outfit_points(out_paths, skip_bad=bool(mods), log=_quiet)
        c = head.c
        z0, z1 = head.top - 0.62, head.top - 0.17
        sel = (P[:, 2] > z0) & (P[:, 2] < z1) & (P[:, 1] < c[1] + 0.03) & (np.abs(P[:, 0]) < 0.28)
        P = P[sel]
        rch = reach if reach is not None else gw.get('outfit_reach', 0.06)
        if hair is not None and len(hair) and rch:
            P = P[cKDTree(hair).query(P)[0] < rch]
        return P

    item_pts = {}
    for it in sel_items:
        with G.outfit_source(it.get('mods', mods) if it.get('mods', mods) else []):
            P = backitems.world_points(it, main)
        if it.get('box'):
            b = it['box']
            P = P[(P[:, 0] >= b[0]) & (P[:, 0] <= b[1]) & (P[:, 1] >= b[2]) & (P[:, 1] <= b[3]) & (P[:, 2] >= b[4]) & (P[:, 2] <= b[5])]
        rch = reach if reach is not None else it.get('reach', 0.14)
        if rch and hair is not None and len(hair) and len(P):
            P = P[cKDTree(hair).query(P)[0] < rch]
        item_pts[it.get('name', it['mesh'])] = P
    out_pts = None
    if out_paths:
        with G.outfit_source(mods):
            out_pts = outfit_target_points()

    pd = 0.002 if pad is None else pad          # outfit_pad: added to every fitted radius (collision.fit)

    def run(scale):
        shapes, caps, spans = [], [], {}          # shapes [(bone, centre, r, target)], caps local pairs
        if out_paths:
            with G.outfit_source(mods):
                sp, cp = collision.outfit_shapes(head, out_paths, nmax=max(1, int(round(nmax0 * scale))), log=_quiet, hair_pts=hair,
                                                 reach=reach if reach is not None else gw.get('outfit_reach', 0.06), skip_bad=bool(mods))
            b0 = len(shapes)
            shapes += [(b, c, r + pd, 'outfit') for b, c, r in sp]
            caps += [(b0 + i, b0 + j) for i, j in cp]
            spans['outfit'] = (b0, len(shapes))
        for it in sel_items:
            it2 = dict(it)
            if reach is not None:
                it2['reach'] = reach
            it2['max_spheres'] = max(2, int(round(it.get('max_spheres', 6) * scale)))
            if it2.get('rows'):
                it2['seg'] = it.get('seg', 0.09) / max(scale, 0.3)
            with G.outfit_source(it.get('mods', mods) if it.get('mods', mods) else []):
                P = backitems.world_points(it2, main)
            sp, cp = backitems.fit_item(it2, P, hair, log=_quiet)
            b0 = len(shapes)
            shapes += [(b, c, r + pd, 'item:' + it.get('name', it['mesh'])) for b, c, r in sp]    # the pipeline adds outfit_pad to back items too
            caps += [(b0 + i, b0 + j) for i, j in cp]
            if len(sp) == 1:
                caps.append((b0, b0))
            spans['item:' + it.get('name', it['mesh'])] = (b0, len(shapes))
        capped = {x for c in caps for x in c}
        twins = [i for i in range(len(shapes)) if i not in capped]
        return shapes, caps, spans, twins

    scale, res = 1.0, None
    for _ in range(14):
        shapes, caps, spans, twins = run(scale)
        ns = nbase + len(shapes) + len(twins)
        nc = len(kept_caps) + len(caps) + len(twins)
        if ns <= S_budget and nc <= C_budget:
            res = (shapes, caps, spans, twins)
            break
        scale *= 0.85
    if res is None:
        raise SystemExit('cannot fit the colliders in the budget (%d spheres / %d capsules, %d kept)' % (S_budget, C_budget, nbase))
    shapes, caps, spans, twins = res
    if scale < 0.999:
        notes.append('budget: per-target sphere counts reduced to %.0f %% of the config values' % (scale * 100))

    # ---- assemble: kept (original order) + fitted + twins of the fitted
    outS = []
    for i in keepi:
        s = cur[i]
        outS.append(dict(bone=s['bone'], local=s['local'], c=s.get('c'), r=s['r'], grp=grp[i], tag=s.get('tag', '')))
    base_fit = len(outS)
    for b, c, r, tag in shapes:
        bn = _remap_bone(b, allowed, rnames, rpar)
        if bn != b:
            notes.append('bone %s is not in this fur: collider bound to %s' % (b, bn))
        B = np.array(bones[bnames.index(bn)]['bind'])
        loc = (np.asarray(c) - B[3, :3]) @ np.linalg.inv(B[:3, :3])
        outS.append(dict(bone=bnames.index(bn), local=[round(float(x), 5) for x in loc], c=[round(float(x), 5) for x in c],
                         r=round(float(r), 5), grp=1, tag=tag))
    outC = list(kept_caps) + [(base_fit + a, base_fit + b) for a, b in caps if a != b]
    for t in twins:
        k = len(outS)
        s = dict(outS[base_fit + t])
        s['local'] = [round(s['local'][0] + 0.001, 5)] + list(s['local'][1:])
        B = np.array(bones[s['bone']]['bind'])
        s['c'] = [round(float(x), 5) for x in np.asarray(s['local']) @ B[:3, :3] + B[3, :3]]
        s['tag'] = s['tag'] + ' (twin)'
        outS.append(s)
        outC.append((base_fit + t, k))
    # single-sphere (i, i) pairs from backitems.fit are replaced by twins above (only capsules collide, a twin pair is proven)

    # ---- report
    def world(sph):
        return [(np.asarray(s['c'], float), float(s['r'])) for s in sph]
    for s_ in cur:
        if s_.get('c') is None:
            B_ = np.array(bones[s_['bone']]['bind'])
            s_['c'] = (np.asarray(s_['local']) @ B_[:3, :3] + B_[3, :3]).tolist()
    for s_ in outS[:nbase]:
        if s_.get('c') is None:
            s_['c'] = cur[keepi[outS.index(s_)]]['c']
    Snew = world(outS)
    allC = [tuple(c) for c in outC]
    Sbefore = world(cur)
    rep = {}
    for name, (a, b) in spans.items():
        # both numbers use the WHOLE collider set (kept + fitted) against this target's vertices, so "before" (the replaced
        # colliders + the kept ones) and "after" compare like for like
        P = out_pts if name == 'outfit' else item_pts.get(name[5:])
        ok = P is not None and len(P)
        rep[name] = dict(spheres=b - a, vertices=int(len(P)) if P is not None else 0,
                         coverage_pct=round(100 * coverage(P, Snew, allC), 1) if ok else None,
                         coverage_before_pct=round(100 * coverage(P, Sbefore, capsCur), 1) if ok else None)
    return dict(ok=True, spheres=outS, capsules=[list(map(int, c)) for c in outC], notes=notes,
                report=dict(targets=rep, total_spheres=len(outS), total_capsules=len(outC), base=nbase, fitted=len(shapes), twins=len(twins),
                            replaced=len(oldi), scale=round(scale, 3), budget=[S_budget, C_budget]))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    if a[0] == 'list':
        print(json.dumps(targets(a[1])))
    elif a[0] == 'groups':
        g = groups(a[1])
        print(json.dumps(dict(n=g['n'], outfit=sum(g['flags']))))
    elif a[0] == 'fit':
        req = json.load(open(a[1]))
        try:
            res = fit(req)
        except SystemExit as e:
            res = dict(ok=False, error=str(e.code))
        json.dump(res, open(a[2], 'w'))
        print('ok' if res.get('ok') else 'error: ' + res.get('error', '?'))
        sys.exit(0 if res.get('ok') else 1)


if __name__ == '__main__':
    main()
