r"""backitems - colliders for things carried on the back (scabbards, swords) that the hair falls on (2026-10-14).

Opt-in config key  growth.back_items = [ {item}, ... ]  (never a default; builds without it are untouched).
Hair of a character who carries a sword on the back clips through the sheath / hilt because the outfit fit
(collision.outfit_shapes) only sees the appearance meshes, and the scabbard is a runtime item on its own skeleton.
Each item is fitted from its mesh in REST pose (static capsules bound to one body bone; the scabbard sways in game,
see HAIRWORK.md "Back items" for the measured amount):

  {"name": "scabbard",
   "mesh": "characters\\models\\main_npc\\ciri\\model\\item_07_wa__ciri.w2mesh",   skinned: vertices are in model space
   "bone": "torso3",                     collider bone (default torso3)
   "seg": 0.09, "reach": 0.14, "max_spheres": 6, "pad": 0.0, "r_min": 0.012, "r_max": 0.045}
  {"name": "sword", "mesh": "items\\weapons\\unique\\sword__ciri\\sword__ciri.w2mesh", "static": true,
   "slot": {"rig": "...ciri_scabbard.w2rig", "bone": "steel_sword_back", "parent": "torso3"},   mesh space -> slot bone
   "mesh_z": [-0.30, 0.07]}              keep only this part of the mesh (the hilt; the blade is inside the sheath)

The mesh -> model-space mapping for static meshes is p_rig = p_mesh @ R_slot + t_slot (slot bone bind in the item's
rig), p_model = p_rig @ R_parent + t_parent (parent = the character's own bone the scabbard component is attached to,
torso3 for Ciri / Geralt). VERIFIED for Ciri: the sword blade axis lands parallel to the scabbard chain and the
rig-space bone positions reproduce the scabbard mesh bind positions to 1 mm.

fit(...) -> (shapes [(bone, centre, radius)], capsules [(i, j)] local indices); `world_points` is used by the Studio
export too.
"""
import numpy as np
from scipy.spatial import cKDTree


def static_transform(item, main_rig=None):
    """(R, t) with p_model = p_mesh @ R + t for a static item mesh bound to a slot bone of the item's rig (None = identity)"""
    sl = item.get('slot')
    if not sl:
        return np.eye(3), np.zeros(3)
    import rig
    names, _par, M = rig.load_rig(sl['rig'])
    B = M[names.index(sl['bone'])]
    pn, _pp, PM = rig.load_rig(sl.get('parent_rig') or main_rig)
    Bp = PM[pn.index(sl.get('parent', item.get('bone', 'torso3')))]
    R = B[:3, :3] @ Bp[:3, :3]
    return R, B[3, :3] @ Bp[:3, :3] + Bp[3, :3]


def world_points(item, main_rig=None):
    """model-space vertices (n, 3) of one back item (LOD0 / non-shadow chunks)."""
    import gamefiles as G
    rel = item['mesh']
    if item.get('static'):
        chunks = G.load_static_mesh(rel)
        P = np.concatenate([c['pos'] for c in chunks])
        z = item.get('mesh_z')
        if z:
            P = P[(P[:, 2] >= z[0]) & (P[:, 2] <= z[1])]
        R, t = static_transform(item, main_rig)
        return P @ R + t
    m = G.load_mesh(rel)
    return np.concatenate([m['chunks'][ci]['pos'] for ci in m['lod0']])


def _chain(item, P, ax, t_lo=None, t_hi=None, tag=''):
    """spheres at stations along axis ax through the points P"""
    c0 = P.mean(0)
    t = (P - c0) @ ax
    t0, t1 = float(t.min()), float(t.max())
    seg = item.get('seg', 0.09)
    n = int(min(item.get('max_spheres', 6), max(2, np.ceil((t1 - t0) / seg) + 1)))
    back = np.asarray(item.get('back', [0.0, -1.0, 0.0]), float)
    back /= np.linalg.norm(back)
    out = []
    hw = max(0.025, 0.35 * (t1 - t0) / (n - 1))                     # half width of the slab one station summarises
    inset = min(0.02, (t1 - t0) / 4.0)
    for ts in np.linspace(t0 + inset, t1 - inset, n):              # end spheres: their caps reach the extremes
        w = np.abs(t - ts) <= hw
        S = P[w]
        if len(S) < item.get('min_verts', 8):
            continue
        d = (S - c0) @ back
        lo, hi = np.percentile(d, 3), np.percentile(d, 97)        # lo = the front side, hi = the back side
        r = float(np.clip((hi - lo) / 2.0, item.get('r_min', 0.012), item.get('r_max', 0.045))) + item.get('pad', 0.0)
        # centre: slab centroid in the plane across the back direction, the back surface kept on the back side
        cen = S.mean(0)
        cen = cen + (ts - (cen - c0) @ ax) * ax                       # on the station plane
        cen = cen - ((cen - c0) @ back) * back + (c0 @ back) * back
        cen = cen + back * ((hi - r) - ((cen - c0) @ back))
        out.append((item.get('bone', 'torso3'), cen, r))
    return out, (t0, t1)


def fit_item(item, P, hair_pts, log=print):
    """stations along the principal axis of the part of the item the hair can reach; sphere radius = half the item's
    thickness along the back direction, centred so the BACK surface (where the hair lies) is kept.
    "box" [xmin,xmax,ymin,ymax,zmin,zmax] keeps only vertices inside (model space); "rows": n splits the kept part into n
    horizontal bands, each fitted as its own chain along x (a hood / cape sheet hanging behind the neck)."""
    name = item.get('name', item['mesh'])
    if item.get('box'):
        b = item['box']
        P = P[(P[:, 0] >= b[0]) & (P[:, 0] <= b[1]) & (P[:, 1] >= b[2]) & (P[:, 1] <= b[3]) & (P[:, 2] >= b[4]) & (P[:, 2] <= b[5])]
    reach = item.get('reach', 0.14)
    if reach and hair_pts is not None and len(hair_pts):           # "reach": 0 = no hair filter (a whole item, rest pose)
        P = P[cKDTree(hair_pts).query(P)[0] < reach]
    if len(P) < 30:
        log('  back item %s: only %d vertices within %.0f cm of the hair, skipped' % (name, len(P), reach * 100))
        return [], []
    if item.get('rows'):
        z0, z1 = float(P[:, 2].min()), float(P[:, 2].max())
        out, caps = [], []
        for k in range(int(item['rows'])):
            lo = z0 + (z1 - z0) * k / item['rows']
            hi = z0 + (z1 - z0) * (k + 1) / item['rows']
            S = P[(P[:, 2] >= lo) & (P[:, 2] <= hi + (1e-9 if k == item['rows'] - 1 else 0))]
            if len(S) < item.get('min_verts', 8):
                continue
            sp, _ = _chain(item, S, np.array([1.0, 0.0, 0.0]))
            caps += [(len(out) + i, len(out) + i + 1) for i in range(len(sp) - 1)]
            out += sp
        log('  back item %s: %d spheres in %d rows (z %.2f..%.2f, %d vertices near the hair), radii %s mm' % (
            name, len(out), item['rows'], z0, z1, len(P), [int(round(r * 1000)) for _b, _c, r in out]))
        return out, caps
    c0 = P.mean(0)
    ax = np.linalg.svd(P - c0, full_matrices=False)[2][0]
    if item.get('axis'):
        ax = np.asarray(item['axis'], float); ax /= np.linalg.norm(ax)
    if ax[2] < 0:
        ax = -ax
    out, (t0, t1) = _chain(item, P, ax)
    log('  back item %s: %d spheres along %s, t %.2f..%.2f m, radii %s mm (%d vertices near the hair)' % (
        name, len(out), np.round(ax, 2).tolist(), t0, t1, [int(round(r * 1000)) for _b, _c, r in out], len(P)))
    return out, [(i, i + 1) for i in range(len(out) - 1)]


def fit(items, hair_pts, main_rig=None, log=print, mods=()):
    """-> (shapes, capsules). mods = default installed outfit mods to read the item meshes from (item "mods" overrides);
    only used to FIT, nothing of them is shipped."""
    import gamefiles as G
    shapes, caps = [], []
    for it in items:
        if it.get('display_only'):
            continue                  # shown in the Studio only (desk_prep), no collider
        with G.outfit_source(it.get('mods', mods) if it.get('mods', mods) else []):
            P = world_points(it, main_rig)
        sp, cp = fit_item(it, P, hair_pts, log=log)
        base = len(shapes)
        shapes += sp
        caps += [(base + i, base + j) for i, j in cp]
        if len(sp) == 1:
            caps.append((base, base))
    return shapes, caps
