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
import json
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
               zone_spacing=None, zone_len=None, zone_max_sd=None,
               # L250 opt-in (Zoltan: scalp hair inside the hb_ mesh): scalp = {z: m, <any beard key>, look: {}}: cards whose centroid
               # lies above z become a SECOND fur '<comp>_scalp' on the head mesh (own look / simulate), the beard fur keeps the rest
               scalp=None,
               # L287 opt-in: up_edge {band m, len_min, sparse, zmin}: soft UPPER border (cheek line / under the nose): guide length ramps
               # from len_min (at the border) to 1 over `band`, Poisson spacing grows up to `sparse` x (remesh only)
               up_edge=None,
               # L287 opt-ins: close (rings: dilate then erode the root-region vertex mask on the head mesh graph = fills holes, e.g. the
               # chin patch where cards are thin), zone_env {zone: m} (envelope per zone), zone_grav {zone: [outward, down]} (guide
               # direction bias: moustache drooping down / outward over the mouth)
               squeeze=0.0, close=0,
               # L288 opt-ins: lip_gap {below, above, x} (m around the mouth line: no roots on the lips -> parting under the moustache),
               # no_under (skin normal z below this: no roots, e.g. -0.5 = jaw underside / neck), up_edge.drop / up_edge.jitter
               lip_gap=None, mouth=None, no_under=None, root_zmin=None, zone_env=None, zone_grav=None,
               density_gain=None, density_gamma=1.0,   # 2026-10-16 opt-in (density_tex): boosted atlas alpha as own xbm
               density_flat=None,   # 2026-10-16 opt-in (L505): a flat density map (keep probability 0..1 for every hair) as own xbm, no atlas
               extra_bones=None)   # 2026-10-15 opt-in: body bones (neck / torso3 / torso2) in the fur for chest colliders
DEFAULT_LOOK = dict(root_color=[40, 32, 26], tip_color=[52, 44, 36])
# beard.studio_defaults presets by name (studio_defaults()); 'l583' = L583 M16 winner, with the look's colours desaturated by the
# config author (Olgierd: root 78,50,43 / tip 152,106,85 instead of 80,50,42 / 160,104,80), not by the preset.
STUDIO_DEFAULT_PRESETS = {'l583': {'textureBrightness': 1.6, 'detailLODWidth': 0.3},      # superseded by l592 (too light once the beard has its length)
                          'l592': {'textureBrightness': 1.7, 'detailLODWidth_factor': 0.6, 'translate': True}}   # dlw: 2026-10-17 x.6 of the Studio's close-up width (Olgierd .5 -> .3 as before; Svanrige's default 1.0 -> .6, not .3: a black .16-wide beard went wispy)   # tb 1.7: L614 (lit beard .72 of the hair's lightness vs .63 at 1.4; the Studio's .94; 2.0 adds nothing)
# 2026-10-17 "l592" = TRANSLATION of the Studio project into game values (FINDINGS 12.9 / 12.10, user: "whatever edits I do in the
# Studio must be reflected in the game"): every game value is the user's Studio value times a factor measured on Olgierd
# (L589-L592, Studio default render vs the path tracer), never a hand-written per-character value:
#   length  = Studio lengthScale as it is, capped at 1.0                 (L599: the game honours it 1:1 below 1 and clamps above; the x1.86 of L590-L592 was wrong)
#   width   = Studio width x 1.36 (.405 -> .55)                          (L591 / L592: fewer, fatter strands render as strands, not as a block)
#   count   = Studio density (1.0 when absent) x 0.6 as a flat density map (the game ignores the density scalar)
#   colour  = Studio colour x (root .95 / 1 / 1.024, tip .906 / 1 / 1.0625) (L589 H2 / L592 T2: the path tracer saturates the beard orange, the tip more)
#   textureBrightness 1.7, detailLODWidth .3                             (the Studio default render applies neither; 1.7 from L614)
#   widthTipScale, waves, clumps, stiffness: the Studio values as they are
L592 = {'length': 1.0, 'length_cap': 1.0, 'width': 0.55 / 0.405, 'count': 0.6,
        # per end, exactly what L592 T2 sent for the Studio's 80,50,42 / 160,104,80 (76,50,43 / 145,104,85): one shared factor
        # left the unedited L593 row more orange than its Studio render (coordinator): the tip needs more red taken out than the root
        # 2026-10-17 L614 (FINDINGS 13.4), measured against the head hair in the same frame: the Studio beard is +7-9 degrees
        # warmer than its hair at equal saturation; the game beard was the hair's hue and more saturated -> more green, less blue
        'colour_root': (1.0, 1.08, 0.93), 'colour_tip': (0.95, 1.077, 0.975), 'desat': 0.4, 'dark_lum': 50.0}   # L617: 40 % toward grey -> lit saturation .33 / .31 (Studio .33 / .29)


def l592_dark(rgb):
    """0..1 weight of the colour corrections for a beard colour: 1 at and above luminance L592['dark_lum'] (Olgierd's
    80,50,42 = 58 and brighter: unchanged), falling to 0 for black (2026-10-17, Svanrige's 18,16,15: the brightness lift and
    the desaturation that fit a red-brown beard turned a black beard grey)"""
    lum = 0.2126 * float(rgb[0]) + 0.7152 * float(rgb[1]) + 0.0722 * float(rgb[2])
    return max(0.0, min(1.0, lum / float(L592.get('dark_lum', 50.0))))


def l592_lod_tb(C, sd, root_rgb=None):
    """-> (detailLODWidth, textureBrightness) the game gets for a beard with studio_defaults sd. A preset 'detailLODWidth' is
    taken as is (l583 / true: .3); 'detailLODWidth_factor' (l592) scales the Studio's close-up width (the look's value, the
    Studio default 1.0 when the user never touched it), so a beard the user sees thick in the Studio keeps its proportion.
    textureBrightness's lift above 1 is weighted by the root colour's luminance (l592_dark): black beards are not lifted."""
    st = studio_look_set(C) if sd.get('translate') else {}
    es0 = ((C.get('beard') or {}).get('look') or {}).get('extra_set') or {} if isinstance(C.get('beard'), dict) else {}   # studio_to_config writes the Studio's values here
    key = 'levelOfDetail.detailLOD.detailLODWidth'
    if 'detailLODWidth' in sd or 'detailLODWidth_factor' not in sd:
        dlw = float(sd.get('detailLODWidth', 0.3))
    else:
        dlw = _sv(st, key, _sv(es0, key, 1.0)) * float(sd['detailLODWidth_factor'])
    tb = float(sd.get('textureBrightness', 1.0))
    if sd.get('translate'):
        rc = st.get('graphicalMaterials.color.rootColor')
        rgb = root_rgb if root_rgb is not None else (list(rc[1])[:3] if isinstance(rc, list) and len(rc) == 2 and isinstance(rc[1], (list, tuple)) else None)
        if rgb is not None and len(rgb) == 3:
            tb = 1.0 + (tb - 1.0) * l592_dark(rgb)
    return dlw, tb


def l592_colour(rgb, end='root'):
    w = l592_dark(rgb)
    v = [min(255.0, max(0.0, float(c) * (1.0 + (f - 1.0) * w))) for c, f in zip(rgb, L592['colour_' + end])]
    # 2026-10-17 (FINDINGS 13.4, L616 / L617): the path tracer renders the beard more saturated than the Studio's raster at the
    # same colour (lit saturation .40 vs .33, and a 10 % pull toward grey moves it by .03 only): the translated colour is
    # pulled L592['desat'] toward its own luminance after the factors
    d = float(L592.get('desat', 0.0)) * w
    if d > 0:
        lum = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2]
        v = [c + (lum - c) * d for c in v]
    return [int(round(c)) for c in v]


def studio_look_set(C):
    """the Studio project's beard look 'set' (SDK path -> [type, value]) of a studio_geo_beard config, {} without one"""
    sg = C.get('studio_geo_beard') if isinstance(C, dict) else None
    if not sg or not sg.get('look'):
        return {}
    import studio_geo
    try:
        return json.load(open(studio_geo._path(sg['look']))).get('set', {}) or {}
    except (OSError, ValueError):
        return {}


def _sv(st, key, default):
    v = st.get(key)
    return float(v[1]) if isinstance(v, list) and len(v) == 2 and isinstance(v[1], (int, float)) else default
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
    sd = studio_defaults(o)
    if sd is not None:
        # 2026-10-16 "game like the Studio" defaults for beard furs (FINDINGS 9.5 / L521 rows S4 + S6, user OK): a flat
        # density map (every hair the Studio draws is kept; the look's density when it is below 1), detailLODWidth .3 and
        # textureBrightness 1.0 (the Studio's default render applies neither the vanilla atlas alpha nor textureBrightness).
        # Written by studio_to_config as "beard.studio_defaults": true (opt-out: false, or a dict with own values); absent
        # = the old behaviour, so approved / older configs rebuild byte-identical.
        if sd.get('translate'):
            # l592 translation (see L592 above): the hair count and the colours are decided here (the density map and the
            # generated fur's colours); length / width follow in studio_beard() where the Studio look is applied
            st = studio_look_set(C)
            if o.get('density_flat') is None and not o.get('colour_map'):
                o['density_flat'] = round(L592['count'], 4)   # a Studio density override is applied in generate() against the built density
            for ck in ('root_color', 'tip_color'):
                if isinstance(lk.get(ck), (list, tuple)) and len(lk[ck]) == 3:
                    lk[ck] = l592_colour(lk[ck], ck.split('_')[0])
        if o.get('density_flat') is None and not o.get('colour_map'):
            d = lk.get('density')
            o['density_flat'] = sd['density_flat'] if sd.get('density_flat') is not None else (float(d) if isinstance(d, (int, float)) and 0 < d < 1 else 1.0)
        es = lk['extra_set'] = dict(lk.get('extra_set') or {})   # a copy: the config's own extra_set keeps the Studio's values (l592_lod_tb reads them)
        rgb0 = (b.get('look') or {}).get('root_color') if isinstance(b, dict) else None   # the Studio's colour, before l592_colour
        dlw_, tb_ = l592_lod_tb(C, sd, rgb0 if isinstance(rgb0, (list, tuple)) and len(rgb0) == 3 else None)
        es['levelOfDetail.detailLOD.detailLODWidth'] = ['Float', dlw_]
        es['graphicalMaterials.color.textureBrightness'] = ['Float', tb_]
    return o


def studio_defaults(o):
    """-> the "game like the Studio" defaults of a beard config (None = off, {} = true with the default values): beard.studio_defaults true = detailLODWidth .3,
    textureBrightness 1.0, density_flat 1.0 (or the look's density < 1); a dict overrides those values."""
    sd = o.get('studio_defaults') if isinstance(o, dict) else None
    if not sd:
        return None
    if isinstance(sd, str):
        # 2026-10-17 named presets (FINDINGS 12): "l583" = the values that matched the Studio default render of Olgierd's beard
        # in game (L583 row M16: textureBrightness 1.6; the game beard renders ~40 % darker than the Studio at 1.0). Opt-in by
        # name; `true` keeps the 2026-10-16 values until the user makes the preset the standard.
        if sd not in STUDIO_DEFAULT_PRESETS:
            raise SystemExit('beard.studio_defaults: unknown preset %r (known: %s)' % (sd, ', '.join(sorted(STUDIO_DEFAULT_PRESETS))))
        return dict(STUDIO_DEFAULT_PRESETS[sd])
    return dict(sd) if isinstance(sd, dict) else {}


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


def card_classes(ch, names, brow_frac=0.2, keep_boxes=None, keep_min_verts=1, keep_below=None):
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
        # opt-in keep_card_boxes (2026-10-15, Donar's side braids): a beard card whose vertices ALL lie inside one box
        # [x0, x1, y0, y1, z0, z1] (mesh space) stays a card ('keep'), drawn with the fur like the kept eyebrows;
        # keep_card_min_verts / keep_card_below (m: its lowest vertex under this z) skip small / short beard cards there
        if keep_boxes and cls[l] == 'beard' and int(vs.sum()) >= keep_min_verts:
            q = np.asarray(ch['pos'])[vs][:, :3]
            if keep_below is not None and q[:, 2].min() >= keep_below:
                q = None
        if keep_boxes and cls[l] == 'beard' and int(vs.sum()) >= keep_min_verts and q is not None:
            for b in keep_boxes:
                if ((q[:, 0] >= b[0]) & (q[:, 0] <= b[1]) & (q[:, 1] >= b[2]) & (q[:, 1] <= b[3]) & (q[:, 2] >= b[4]) & (q[:, 2] <= b[5])).all():
                    cls[l] = 'keep'
                    break
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


def upper_border(skin, T, band_r=0.015, zmin=None, log=print):
    """L287: outline vertices of the root-region triangles T that face UP (the region lies below them, within band_r) and lie
    above zmin (default: upper lip bone z - 6 mm, so the lower lip / mouth-hole edge is not a border). -> (n, 3) points."""
    from collections import Counter
    ec = Counter()
    for a_, b_, c_ in T:
        for e_ in ((a_, b_), (b_, c_), (c_, a_)):
            ec[tuple(sorted(e_))] += 1
    bnd = np.array(sorted({v for e_, n_ in ec.items() if n_ == 1 for v in e_}), int)
    if zmin is None:
        up = np.linalg.inv(np.array(skin.rig[skin.names.index('upper_lip')], float))[3, :3] if 'upper_lip' in skin.names else None
        zmin = (up[2] - 0.006) if up is not None else 1e9
    used = np.unique(T)
    tr = cKDTree(skin.v[used])
    out = []
    for b in bnd:
        p = skin.v[b]
        if p[2] < zmin:
            continue
        idx = tr.query_ball_point(p, band_r)
        if len(idx) > 3 and skin.v[used][idx][:, 2].mean() < p[2] - 0.002:
            out.append(p)
    log('  beard: up_edge: %d outline vertices, %d on the upper border (z > %.3f)' % (len(bnd), len(out), zmin))
    return np.array(out) if out else np.zeros((0, 3))


def _smooth(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3 - 2 * x)


def mouth_fns(skin, mo):
    """L290 opt-in (beard.mouth): -> (mz, in_cut(P), in_soul(P)). mz = mean z of the corner-lip bones; in_cut = the box
    |x| < mo.x, mz - mo.below < z < mz + mo.above (lips + corners: no roots, no growth faces, also from remesh samples);
    in_soul = the band under it (mo.soul {z0, z1, x}: mz - z1 < z < mz - z0) where hair is sparse and short."""
    cz = [np.linalg.inv(np.array(skin.rig[skin.names.index(n)], float))[3, 2] for n in
          ('left_corner_lip1', 'right_corner_lip1', 'left_corner_lip2', 'right_corner_lip2') if n in skin.names]
    mz = float(np.mean(cz)) if cz else 1e9
    xc, bl, ab = float(mo.get('x', 0.046)), float(mo.get('below', 0.018)), float(mo.get('above', 0.008))
    so = mo.get('soul') or {}
    def in_cut(P):
        P = np.atleast_2d(P)
        return (np.abs(P[:, 0]) < xc) & (P[:, 2] > mz - bl) & (P[:, 2] < mz + ab)
    def in_soul(P):
        P = np.atleast_2d(P)
        if not so:
            return np.zeros(len(P), bool)
        return (np.abs(P[:, 0]) < float(so.get('x', 0.03))) & (P[:, 2] < mz - float(so.get('z0', 0.018))) & (P[:, 2] > mz - float(so.get('z1', 0.045)))
    return mz, in_cut, in_soul


def _cut_below(g, zlim):
    """truncate a guide where it first drops below zlim (linear crossing), resampled to the same point count."""
    z = g[:, 2]
    if z.min() >= zlim:
        return g
    i = int(np.argmax(z < zlim))
    if i == 0:
        return g
    t = (z[i - 1] - zlim) / max(z[i - 1] - z[i], 1e-9)
    pts = np.concatenate([g[:i], (g[i - 1] + t * (g[i] - g[i - 1]))[None]])
    return _resample(pts, len(g))


def _lscm(V, T):
    """least-squares conformal map of the triangles T (indices into V) -> (uv (len(V), 2), piece id per triangle). Each
    connected piece is pinned at its two farthest-apart vertices (their 3D distance apart) and shifted right of the last."""
    from scipy.sparse import coo_matrix, csr_matrix
    from scipy.sparse.csgraph import connected_components
    from scipy.sparse.linalg import lsqr
    nv = len(V)
    ij = np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]])
    ncomp, lab = connected_components(csr_matrix((np.ones(len(ij)), (ij[:, 0], ij[:, 1])), shape=(nv, nv)), directed=False)
    tcomp = lab[T[:, 0]]
    UV = np.zeros((nv, 2))
    ox = 0.0
    pieces = sorted(set(tcomp.tolist()))
    remap_c = {c: k for k, c in enumerate(pieces)}
    for c in pieces:
        Tc = T[tcomp == c]
        vs = np.unique(Tc)
        loc = -np.ones(nv, int); loc[vs] = np.arange(len(vs))
        Tl = loc[Tc]
        P = V[vs]
        a, b, cc = P[Tl[:, 0]], P[Tl[:, 1]], P[Tl[:, 2]]
        e1 = b - a; l1 = np.linalg.norm(e1, axis=1, keepdims=True); ex = e1 / np.maximum(l1, 1e-12)
        nrm = np.cross(e1, cc - a); A2 = np.linalg.norm(nrm, axis=1)
        ey = np.cross(nrm / np.maximum(A2[:, None], 1e-12), ex)
        x = np.stack([np.zeros(len(Tl)), l1[:, 0], ((cc - a) * ex).sum(1)], 1)
        y = np.stack([np.zeros(len(Tl)), np.zeros(len(Tl)), ((cc - a) * ey).sum(1)], 1)
        Wr = np.stack([x[:, 2] - x[:, 1], x[:, 0] - x[:, 2], x[:, 1] - x[:, 0]], 1) / np.sqrt(np.maximum(A2, 1e-18))[:, None]
        Wi = np.stack([y[:, 2] - y[:, 1], y[:, 0] - y[:, 2], y[:, 1] - y[:, 0]], 1) / np.sqrt(np.maximum(A2, 1e-18))[:, None]
        m = len(vs)
        # pins: two farthest-apart vertices (approx.: farthest from the first, then farthest from that)
        p0 = int(np.argmax(np.linalg.norm(P - P[0], axis=1))); p1 = int(np.argmax(np.linalg.norm(P - P[p0], axis=1)))
        if p0 == p1:
            continue
        pin = {p0: (0.0, 0.0), p1: (float(np.linalg.norm(P[p1] - P[p0])), 0.0)}
        free = np.array([k for k in range(m) if k not in pin]); fi = -np.ones(m, int); fi[free] = np.arange(len(free))
        rows, cols, vals = [], [], []
        rhs = np.zeros(2 * len(Tl))
        nf = len(free)
        for j in range(3):
            vj = Tl[:, j]
            for t in range(len(Tl)):
                k = vj[t]
                wr, wi = Wr[t, j], Wi[t, j]
                if k in pin:
                    s_, t_ = pin[k]
                    rhs[2 * t] -= wr * s_ - wi * t_
                    rhs[2 * t + 1] -= wi * s_ + wr * t_
                else:
                    f = fi[k]
                    rows += [2 * t, 2 * t, 2 * t + 1, 2 * t + 1]; cols += [f, nf + f, f, nf + f]; vals += [wr, -wi, wi, wr]
        M = coo_matrix((vals, (rows, cols)), shape=(2 * len(Tl), 2 * nf)).tocsr()
        sol = lsqr(M, rhs, atol=1e-12, btol=1e-12, iter_lim=20000)[0]
        uv = np.zeros((m, 2))
        uv[free, 0], uv[free, 1] = sol[:nf], sol[nf:]
        for k, (s_, t_) in pin.items():
            uv[k] = (s_, t_)
        uv[:, 0] += ox - uv[:, 0].min()
        ox = uv[:, 0].max() + 1.0
        UV[vs] = uv
    return UV, np.array([remap_c[c] for c in tcomp])


def remesh(skin, T, sp, log=print, seed=7, edge_k=2.2, cen_k=0.35, zone_sp=None, ub=None, up=None, mouth=None, flat=None):
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
    P_cut = np.zeros(n, bool)
    if mouth is not None:
        _mz, _cut, _soul = mouth_fns(skin, mouth)
    if ub is not None and len(ub) and up:
        # L287 opt-in: sparser roots toward the upper border (hairs per face is fixed, so bigger faces = fewer hairs / cm2)
        dub, _ = cKDTree(ub).query(P)
        bnd_ = float(up.get('band', 0.012))
        if up.get('mband') is not None and mouth is not None:
            bnd_ = np.where((np.abs(P[:, 0]) < float(up.get('mx', 0.04))) & (P[:, 2] > _mz - 0.02), float(up['mband']), bnd_)
        spf_ = float(up.get('sparse', 2.5))
        if up.get('msparse') is not None and mouth is not None:
            spf_ = np.where((np.abs(P[:, 0]) < float(up.get('mx', 0.04))) & (P[:, 2] > _mz - 0.02), float(up['msparse']), spf_)
        spi = spi * (1.0 + (spf_ - 1.0) * (1.0 - _smooth(dub / bnd_)))
    if mouth is not None:
        P_cut = _cut(P)
        spi = spi * np.where(_soul(P), float((mouth.get('soul') or {}).get('sparse', 2.0)), 1.0)
        log('  beard: mouth: mouth line z %.4f, %d remesh samples in the lip box rejected, %d in the soul-patch band' % (_mz, int(P_cut.sum()), int(_soul(P).sum())))
    if ub is not None and len(ub) and up and up.get('drop'):
        dub2, _ = cKDTree(ub).query(P)
        bnd2 = float(up.get('band', 0.012))
        if up.get('mband') is not None and mouth is not None:
            bnd2 = np.where((np.abs(P[:, 0]) < float(up.get('mx', 0.04))) & (P[:, 2] > _mz - 0.02), float(up['mband']), bnd2)
        dr_ = float(up['drop'])
        if up.get('mdrop') is not None and mouth is not None:
            dr_ = np.where((np.abs(P[:, 0]) < float(up.get('mx', 0.04))) & (P[:, 2] > _mz - 0.02), float(up['mdrop']), dr_)
        rej = rng.random(n) < dr_ * (1.0 - _smooth(dub2 / bnd2))
        P_ok = ~rej
    else:
        P_ok = np.ones(n, bool)
    P_ok &= ~P_cut
    for i in range(n):
        if not P_ok[i]:
            continue
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
    if flat == 'lscm':
        # L319 opt-in (beard.remesh_flat = "lscm"): Delaunay on a least-squares conformal flattening of the root-region
        # triangles T (per connected piece, pieces placed apart), each sample at its barycentric spot. The azimuthal
        # projection folds where the surface is not star-shaped from the head centre (Baron: chin front over the jaw
        # underside), so Delaunay joined far roots, the edge limit dropped them and the growth mesh was full of holes.
        UV, comp = _lscm(skin.v, T)
        q2 = (wk[:, :, None] * UV[T[tk]]).sum(1)
        tri = Delaunay(q2).simplices
        cs = comp[tk]
        same = (cs[tri[:, 0]] == cs[tri[:, 1]]) & (cs[tri[:, 1]] == cs[tri[:, 2]])
        log('  beard: remesh_flat lscm: %d pieces, %d Delaunay faces across pieces dropped' % (int(comp.max()) + 1, int((~same).sum())))
        tri = tri[same]
    else:
        d = Pk - skin.c
        z = d.mean(0); z /= np.linalg.norm(z)
        x = np.cross(z, [0.0, 0.0, 1.0]); x /= max(np.linalg.norm(x), 1e-9); y = np.cross(z, x)
        dn = d / np.linalg.norm(d, axis=1, keepdims=True)
        th = np.arccos(np.clip(dn @ z, -1, 1)); ph = np.arctan2(dn @ y, dn @ x)
        tri = Delaunay(np.stack([th * np.cos(ph), th * np.sin(ph)], 1)).simplices
    e = np.max([np.linalg.norm(Pk[tri[:, i]] - Pk[tri[:, (i + 1) % 3]], axis=1) for i in range(3)], axis=0)
    ar = 0.5 * np.linalg.norm(np.cross(Pk[tri[:, 1]] - Pk[tri[:, 0]], Pk[tri[:, 2]] - Pk[tri[:, 0]]), axis=1)
    spf = spi[keep][tri].max(1)
    _nt0 = len(tri)
    _ke, _ka = e <= spf * edge_k, ar > spf * spf * 0.05
    tri = tri[_ke & _ka]
    log('  beard: remesh Delaunay %d faces: %d over the edge limit, %d slivers' % (_nt0, int((~_ke).sum()), int((_ke & ~_ka).sum())))
    used = np.unique(tri)
    rm = -np.ones(len(Pk), int); rm[used] = np.arange(len(used))
    F = rm[tri]
    Pu = Pk[used]
    cen = Pu[F].mean(1)
    dc, _ = cKDTree(P).query(cen)
    ok = dc < cen_k * spi[keep][used][F].max(1)
    if mouth is not None:
        ok &= ~_cut(cen)
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
    zenv, zgrav = o.get('zone_env') or {}, o.get('zone_grav') or {}
    zr = zones(skin, skin.v[np.asarray(roots)]) if (zl or zsd or zenv or zgrav) else None
    K = int(o['K'])
    r = o['radius']
    for ri, vi in enumerate(roots):
        p0 = skin.v[vi]
        n0 = skin.n[vi]
        f0, L = _field(S, tree, p0, r)
        L = float(np.clip(L * o['len_scale'], o['min_len'], o['max_len']))
        msd = o.get('max_sd')
        env = o['env']
        gv = None
        if zr is not None:
            L *= float(zl.get(zr[ri], 1.0))          # L249 opt-in: per-zone length factor
            msd = zsd.get(zr[ri], msd)               # L249 opt-in: per-zone skin hold
            env = float(zenv.get(zr[ri], env))
            if zr[ri] in zgrav:
                gv = np.array([float(zgrav[zr[ri]][0]) * (1.0 if p0[0] >= 0 else -1.0), 0.0, -float(zgrav[zr[ri]][1])])
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
            if gv is not None:
                d = d + gv * (k + 1) / nsub
                d /= max(np.linalg.norm(d), 1e-12)
            q = p + h * d
            sd, nq = skin.signed(q[None])
            floor = 0.0004 + 0.6 * h * (k + 1) / nsub        # rises off the skin a little along the strand
            if sd[0] < floor:
                q = q + (floor - sd[0]) * nq[0]
            elif msd is not None and sd[0] > msd:
                q = q - (sd[0] - msd) * nq[0]          # L247 opt-in: no tufts standing off the skin
            if k > nsub * 0.3 and tree.query(q)[0] > env:
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


def _raster(U, F, n):
    """texel -> (face id, barycentrics) for n x n texels (texel centres), faces in uv U (F, 3, 2); unfilled texels take
    the nearest filled one (so bilinear / mip reads at face edges never see an empty texel)."""
    from scipy import ndimage
    fid = -np.ones((n, n), int)
    bar = np.zeros((n, n, 3))
    P = U * n - 0.5
    for f in range(len(F)):
        p0, p1, p2 = P[f]
        x0, y0 = np.floor(np.minimum(np.minimum(p0, p1), p2)).astype(int) - 1
        x1, y1 = np.ceil(np.maximum(np.maximum(p0, p1), p2)).astype(int) + 1
        x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, n - 1), min(y1, n - 1)
        if x1 < x0 or y1 < y0:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
        q = np.stack([xs, ys], -1).astype(float)
        e1, e2 = p1 - p0, p2 - p0
        den = e1[0] * e2[1] - e1[1] * e2[0]
        if abs(den) < 1e-12:
            continue
        r = q - p0
        b1 = (r[..., 0] * e2[1] - r[..., 1] * e2[0]) / den
        b2 = (e1[0] * r[..., 1] - e1[1] * r[..., 0]) / den
        b0 = 1 - b1 - b2
        ins = (b0 >= -1e-6) & (b1 >= -1e-6) & (b2 >= -1e-6)
        fid[ys[ins], xs[ins]] = f
        bar[ys[ins], xs[ins]] = np.stack([b0[ins], b1[ins], b2[ins]], -1)
    empty = fid < 0
    if empty.any() and (~empty).any():
        _d, (iy, ix) = ndimage.distance_transform_edt(empty, return_indices=True)
        fid = fid[iy, ix]
        bar = bar[iy, ix]
    return fid, bar


def colour_map(V, F, U_old, alpha, o, skin, base, log=print):
    """opt-in beard.colour_map (2026-10-15, Baron): colour BY POSITION. Vanilla card beards are grey textures tinted per card
    in game (Baron: near-black cheeks / moustache, light grey chin centre); flat root / tip colours cannot place that, and
    the density_tex uvs are shared atlas patches. Here every growth face gets its own uv in the beard's azimuthal projection
    (pole = the beard's mean direction from the head centre, as remesh: one orientation for all faces), and two inline
    textures are written in that space:
      colour (DXT1, rootColorTex = tipColorTex, textureBrightness 1: the texture IS the colour; each hair takes the texel under
             its root): dark -> grey by a zone weight (|x| from the mid line, z below the mouth line) + per-texel speckle
             (salt and pepper: about one texel per hair)
      density (DXT5 alpha, only with density_tex): the beard atlas alpha re-baked from each face's old atlas patch.
    Keys (beard.colour_map): size 1024 (density), colour_size 512, margin .02, dark [r,g,b], grey [r,g,b],
      x0 .02 / x1 .05 (m: full grey within x0 of the mid line, none past x1), z_top .012 / z_fade .03 (m below the lower lip bone:
      grey starts z_top below it and is full z_fade further down), amount 1, speck_frac .15 / speck .5 (fraction of texels
      lifted toward grey and by how much), jitter .25 (per-texel brightness +-), seed 11, moustache {x .035, dark .75, speck .3}
      (optional: above the lower lip within x of the mid line the colour x dark and the specks x speck), flat "lscm"
      (optional: uvs from a conformal flattening of the growth mesh instead of the azimuthal projection).
    -> dict(uvs (F*3, 2), files [(path, bytes)], paths, images {path: PIL})."""
    import headtex
    from PIL import Image
    cm = o['colour_map'] if isinstance(o['colour_map'], dict) else {}
    n, nc = int(cm.get('size', 1024)), int(cm.get('colour_size', 512))
    if cm.get('flat') == 'lscm':
        # opt-in colour_map.flat = "lscm": uvs from a conformal flattening of the growth mesh itself (the azimuthal
        # projection folds the chin front over the jaw underside: both took the same texels)
        Q = _lscm(V, np.asarray(F))[0]
    else:
        d = V - skin.c
        z = d.mean(0); z /= np.linalg.norm(z)
        x = np.cross(z, [0.0, 0.0, 1.0]); x /= max(np.linalg.norm(x), 1e-9); y = np.cross(z, x)
        dn = d / np.linalg.norm(d, axis=1, keepdims=True)
        th = np.arccos(np.clip(dn @ z, -1, 1)); ph = np.arctan2(dn @ y, dn @ x)
        Q = np.stack([th * np.cos(ph), th * np.sin(ph)], 1)
    lo, hi = Q.min(0), Q.max(0)
    span = float((hi - lo).max()) * (1 + 2 * float(cm.get('margin', 0.02)))
    uv = 0.5 + (Q - (lo + hi) / 2) / span
    U = uv[F].copy()
    ar = lambda W: (W[:, 1, 0] - W[:, 0, 0]) * (W[:, 2, 1] - W[:, 0, 1]) - (W[:, 2, 0] - W[:, 0, 0]) * (W[:, 1, 1] - W[:, 0, 1])
    a = ar(U)
    if (a > 0).sum() > (a < 0).sum():                 # redfur rule: negative signed uv area
        U[..., 0] = 1.0 - U[..., 0]
        a = ar(U)
    bad = a >= 0
    if bad.any():                                    # the few faces flipped against the projection: a tiny triangle at the centroid
        c0 = U[bad].mean(1)
        U[bad] = c0[:, None, :] + np.array([[0, 0], [0, 1e-4], [1e-4, 0]])[None]
    # ---- colour by position
    mz, lo_lip = None, None
    if 'lowwer_lip' in skin.names:
        lo_lip = np.linalg.inv(np.array(skin.rig[skin.names.index('lowwer_lip')], float))[3, :3]
    mz = float(lo_lip[2]) if lo_lip is not None else float(V[:, 2].max() - 0.04)
    fid, bar = _raster(U, F, nc)
    P3 = np.einsum('hwk,hwkc->hwc', bar, V[F][fid])
    ax = np.abs(P3[..., 0])
    x0, x1 = float(cm.get('x0', 0.02)), float(cm.get('x1', 0.05))
    gx = _smooth((x1 - ax) / max(x1 - x0, 1e-6))
    zt, zf = float(cm.get('z_top', 0.012)), float(cm.get('z_fade', 0.03))
    gz = _smooth(((mz - zt) - P3[..., 2]) / max(zf, 1e-6))
    g = np.clip(gx * gz * float(cm.get('amount', 1.0)), 0, 1)
    rng = np.random.default_rng(int(cm.get('seed', 11)))
    r1, r2, r3 = rng.random((3, nc, nc))
    sp = (r1 < float(cm.get('speck_frac', 0.15))) * r2 * float(cm.get('speck', 0.5))
    md = cm.get('moustache')
    if md:
        # moustache zone (above the lower lip, |x| < md.x): darker (md.dark x) and fewer specks (md.speck x)
        mw = _smooth((float(md.get('x', 0.035)) - ax) / 0.01) * _smooth((P3[..., 2] - (mz + 0.002)) / 0.006)
        sp = sp * (1 - mw * (1 - float(md.get('speck', 0.3))))
    t = np.clip(g + (1 - g) * sp, 0, 1)
    dark = np.array(cm.get('dark', o['look']['root_color']), float)
    grey = np.array(cm.get('grey', o['look']['tip_color']), float)
    col = dark[None, None] * (1 - t[..., None]) + grey[None, None] * t[..., None]
    if md:
        col = col * (1 - mw[..., None] * (1 - float(md.get('dark', 0.75))))
    col = np.clip(col * (1 + float(cm.get('jitter', 0.25)) * (r3[..., None] - 0.5)), 0, 255)
    paths = dict(colour=base + '_cm_colour.xbm')
    files = [(paths['colour'], headtex.make_xbm(headtex.DXT1_PROXY, nc, nc, headtex.encode_dxt1(col), paths['colour'], nc // 4 * 8))]
    images = {paths['colour']: Image.fromarray(np.round(col).astype(np.uint8), 'RGB').convert('RGBA')}
    log('  beard: colour_map %dx%d: lower lip z %.4f, grey zone |x| < %.0f..%.0f mm, %.0f..%.0f mm below it; grey weight p50 %.2f / max %.2f; '
        'dark %s grey %s speck %.2f x %.2f jitter %.2f; %d faces re-oriented' % (
            nc, nc, mz, x0 * 1000, x1 * 1000, zt * 1000, (zt + zf) * 1000, float(np.median(g)), float(g.max()), dark.tolist(),
            grey.tolist(), float(cm.get('speck_frac', 0.15)), float(cm.get('speck', 0.5)), float(cm.get('jitter', 0.25)), int(bad.sum())))
    # ---- density re-baked into the same uvs (density_tex builds only)
    if U_old is not None:
        fid, bar = _raster(U, F, n)
        Uo = np.asarray(U_old, float).reshape(-1, 3, 2)
        q = np.einsum('hwk,hwkc->hwc', bar, Uo[fid])
        dens = _alpha_at(alpha, q.reshape(-1, 2)).reshape(n, n)
        d8 = np.clip(np.round(dens * 255.0), 0, 255)
        paths['density'] = base + '_cm_density.xbm'
        files.append((paths['density'], headtex.make_xbm(headtex.CARD_STUB, n, n, headtex.encode_dxt5_alpha(d8), paths['density'], n // 4 * 16)))
        im = np.zeros((n, n, 4), np.uint8); im[..., 3] = d8.astype(np.uint8)
        images[paths['density']] = Image.fromarray(im, 'RGBA')
        log('  beard: colour_map density %dx%d re-baked from the atlas patches: mean %.3f' % (n, n, float(dens.mean())))
    for p_, b_ in files:
        log('  beard: colour_map file %8d  %s' % (len(b_), p_))
    return dict(uvs=U.reshape(-1, 2), files=files, paths=paths, images=images)


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


def scalp_cards(ch, z):
    """L250: per-vertex bool, True on the cards whose centroid z is above `z` (the scalp hair of a scalp_beard hb_ mesh)."""
    lab = G.components(ch['nv'], ch['idx'])
    out = np.zeros(ch['nv'], bool)
    for l in np.unique(lab):
        v = lab == l
        if ch['pos'][v, 2].mean() > z:
            out[v] = True
    return out


def generate(C, info, log=print, part='beard', prefix=''):
    """-> dict(fur, template, comp, mesh, res, head, brows) or None when the appearance has no beard component."""
    import redfur
    import growth
    from texread import load_xbm
    import hairpipe as H
    o = cfg(C)
    sc = o.get('scalp')
    if part == 'scalp':
        if not sc:
            return None
        o = dict(o)
        o.update({k: v for k, v in sc.items() if k not in ('z', 'look')})
        lk = dict(o['look']); lk.update(sc.get('look') or {}); o['look'] = lk
    hit = find(C, info, o.get('comp'))
    if not hit:
        log('  beard: no beard component in appearance %r: skipped' % C.get('appearance'))
        return None
    tpl, comp, mesh = hit
    m = G.load_mesh(mesh)
    ch = G.merge_chunks(m, m['lod0'])
    bnames = G.mesh_bones(mesh)['names']
    lab, cls = card_classes(ch, bnames, keep_boxes=o.get('keep_card_boxes'), keep_min_verts=int(o.get('keep_card_min_verts', 1)), keep_below=o.get('keep_card_below'))
    nk = len(np.unique(lab[cls == 'keep']))
    if nk:
        log('  beard: keep_card_boxes: %d beard cards stay cards' % nk)
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
    conv = (cls == 'beard') if o['keep_brows'] else (cls != 'keep')
    if sc:
        szm = scalp_cards(ch, float(sc.get('z', 1.34)))
        conv = (conv & szm) if part == 'scalp' else (conv & ~szm)
        log('  beard: scalp split z > %.3f: %d scalp card vertices, part %s keeps %d' % (float(sc.get('z', 1.34)), int(szm.sum()), part, int(conv.sum())))
    S = card_samples(ch, alpha, conv, skin.tree, skin.n, o)
    log('  beard: %d visible card samples on %d cards (%d oriented by skin distance, atlas majority %+d), card length mm p50 %.1f' % (
        len(S['p']), S['cards'], S['oriented'], int(S['majority']), float(np.median(S['L'])) * 1000))
    # roots: welded head vertices with a root-region card sample within reach (and not nearer an eyebrow card)
    rr = S['sd'] < o['root_sd']
    rt = cKDTree(S['p'][rr])
    d, _ = rt.query(skin.v)
    root = d < o['reach']
    if o.get('close'):
        e = np.concatenate([skin.t[:, [0, 1]], skin.t[:, [1, 2]], skin.t[:, [2, 0]]])
        e = np.concatenate([e, e[:, ::-1]])
        n0 = int(root.sum())
        for _ in range(int(o['close'])):
            m2 = root.copy(); np.logical_or.at(m2, e[:, 0], root[e[:, 1]]); root = m2
        for _ in range(int(o['close'])):
            bad = np.zeros(len(root), bool); np.logical_or.at(bad, e[:, 0], ~root[e[:, 1]]); root = root & ~bad
        log('  beard: close %d rings: %d -> %d root vertices' % (int(o['close']), n0, int(root.sum())))
    if nb and o['keep_brows']:
        Sb = card_samples(ch, alpha, cls == 'brow', skin.tree, skin.n, o)
        if len(Sb['p']):
            db, _ = cKDTree(Sb['p']).query(skin.v)
            near_brow = db < d
            log('  beard: %d candidate roots nearer an eyebrow card than a beard card dropped' % int((root & near_brow).sum()))
            root &= ~near_brow
    T = skin.t[root[skin.t].all(1)]
    if o.get('lip_gap'):
        lg = o['lip_gap']
        cz = [np.linalg.inv(np.array(skin.rig[skin.names.index(n)], float))[3, 2] for n in ('left_corner_lip1', 'right_corner_lip1', 'left_corner_lip2', 'right_corner_lip2') if n in skin.names]
        mz = float(np.mean(cz)) if cz else 1e9
        gm = (skin.v[:, 2] > mz - float(lg.get('below', 0.016))) & (skin.v[:, 2] < mz + float(lg.get('above', 0.006))) & (np.abs(skin.v[:, 0]) < float(lg.get('x', 0.042)))
        log('  beard: lip_gap: mouth line z %.4f, %d root vertices removed' % (mz, int((root & gm).sum())))
        root &= ~gm
    if o.get('root_zmin') is not None:
        nz_ = root & (skin.v[:, 2] < float(o['root_zmin']))
        log('  beard: root_zmin %.3f: %d root vertices removed' % (float(o['root_zmin']), int(nz_.sum())))
        root &= ~nz_
    if o.get('no_under') is not None:
        nu = root & (skin.n[:, 2] < float(o['no_under']))
        log('  beard: no_under: %d root vertices (normal z < %.2f) removed' % (int(nu.sum()), float(o['no_under'])))
        root &= ~nu
    ub = upper_border(skin, T, zmin=(o['up_edge'] or {}).get('zmin'), log=log) if o.get('up_edge') else None
    if o.get('remesh'):
        T = remesh(skin, T, float(o['remesh']), log, edge_k=o.get('remesh_edge', 2.2), cen_k=o.get('remesh_cen', 0.35),
                   zone_sp=o.get('zone_spacing'), ub=ub, up=o.get('up_edge'), mouth=o.get('mouth'), flat=o.get('remesh_flat'))
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
    if ub is not None and len(ub):
        ue = o['up_edge']
        du, _ = cKDTree(ub).query(V)
        bnd3 = float(ue.get('band', 0.012))
        if ue.get('mband') is not None and o.get('mouth'):
            _mz3 = mouth_fns(skin, o['mouth'])[0]
            bnd3 = np.where((np.abs(V[:, 0]) < float(ue.get('mx', 0.04))) & (V[:, 2] > _mz3 - 0.02), float(ue['mband']), bnd3)
        lu = float(ue.get('len_min', 0.25)) + (1 - float(ue.get('len_min', 0.25))) * _smooth(du / bnd3)
        if ue.get('jitter'):
            rj = np.random.default_rng(11).random(len(V))
            jt_ = float(ue['jitter'])
            if ue.get('mjitter') is not None and o.get('mouth'):
                jt_ = np.where((np.abs(V[:, 0]) < float(ue.get('mx', 0.04))) & (V[:, 2] > _mz3 - 0.02), float(ue['mjitter']), jt_)
            lu = lu * (1.0 - jt_ * rj * (1.0 - _smooth(du / bnd3)))
        lfac = lu if lfac is None else lfac * lu
        log('  beard: up_edge length factor p10 %.2f / p50 %.2f' % (np.percentile(lu, 10), np.percentile(lu, 50)))
    mo = o.get('mouth')
    if mo:
        mz_, cut_, soul_ = mouth_fns(skin, mo)
        zrv = zones(skin, V)
        so = mo.get('soul') or {}
        if so:
            if so.get('len0') is not None:
                # L291: short hair at the top of the soul band lengthening into the chin beard (smoothstep over the band)
                t_ = np.clip(((mz_ - float(so.get('z0', 0.018))) - V[:, 2]) / max(float(so.get('z1', 0.045)) - float(so.get('z0', 0.018)), 1e-6), 0.0, 1.0)
                lf = np.where(soul_(V), float(so['len0']) + (1.0 - float(so['len0'])) * _smooth(t_), 1.0)
            else:
                lf = np.where(soul_(V), float(so.get('len', 0.35)), 1.0)
            lfac = lf if lfac is None else lfac * lf
        ms = mo.get('stop') or {}
        if ms:
            sl = np.where((zrv == 'moustache') & (np.abs(V[:, 0]) > float(ms.get('x', 0.02))), float(ms.get('side_len', 1.0)), 1.0)
            lfac = sl if lfac is None else lfac * sl
    if o.get('len_x_taper'):
        # L319 opt-in (Baron): guides rooted below the lower lip shorten with |x| from the mid line (rounded beard bottom
        # instead of a flat, full-width edge): factor 1 within x0, down to `min` past x1
        lt = o['len_x_taper']
        zz_ = zones(skin, V)
        tf = 1.0 - (1.0 - float(lt.get('min', 0.6))) * _smooth((np.abs(V[:, 0]) - float(lt.get('x0', 0.03))) /
                                                               max(float(lt.get('x1', 0.08)) - float(lt.get('x0', 0.03)), 1e-6))
        tf = np.where(zz_ == 'below', tf, 1.0)
        lfac = tf if lfac is None else lfac * tf
        log('  beard: len_x_taper: below-zone length factor p10 %.2f / p50 %.2f' % (np.percentile(tf[zz_ == 'below'], 10), np.percentile(tf[zz_ == 'below'], 50)))
    Gd = guides(skin, S, used, o, log, lfac=lfac)
    so_ = (mo or {}).get('soul') or {}
    if so_.get('down'):
        # L291 opt-in: guides rooted in the soul-patch band only grow down (z never rises along the strand)
        ns_ = 0
        for i, g in enumerate(Gd):
            if soul_(V[i:i + 1])[0]:
                g2 = g.copy()
                g2[:, 2] = np.minimum.accumulate(g2[:, 2])
                ns_ += int(np.abs(g2 - g).max() > 1e-6)
                Gd[i] = g2
        log('  beard: soul.down: %d guides clamped to grow down' % ns_)
    if mo and mo.get('stop'):
        ms = mo['stop']
        nc = 0
        xs, w_, drop_ = float(ms.get('x', 0.02)), float(ms.get('w', 0.02)), float(ms.get('side_drop', 0.0))
        for i, g in enumerate(Gd):
            if V[i, 2] > mz_ + 0.005 and abs(V[i, 0]) < xs + w_ + 0.03:
                zl_ = mz_ + float(ms.get('dz', 0.002)) - drop_ * _smooth((abs(V[i, 0]) - xs) / w_)
                if ms.get('jitter'):
                    zl_ -= float(ms['jitter']) * float(np.random.default_rng(100 + i).random())
                g2 = _cut_below(g, zl_)
                nc += int(g2 is not g)
                Gd[i] = g2
        if ms.get('lift'):
            _, nrm_ = skin.signed(V)
            for i, g in enumerate(Gd):
                if V[i, 2] > mz_ + 0.005 and abs(V[i, 0]) < xs + w_ + 0.03:
                    f_ = _smooth(np.linspace(0.0, 1.0, len(g)))[:, None]
                    Gd[i] = g + f_ * float(ms['lift']) * nrm_[i][None]
        log('  beard: mouth.stop: %d centre moustache guides cut at the lip line' % nc)
    if o.get('squeeze'):
        # L287 opt-in: guide points' x offset from the root scaled by (1 - squeeze) (narrower fan of a mohawk)
        Gd = [np.concatenate([g[:, :1] * 0 + g[:1, :1] + (g[:, :1] - g[:1, :1]) * (1 - float(o['squeeze'])), g[:, 1:]], axis=1) for g in Gd]
    # skinning: the head vertex's own weights
    bone_used = set()
    for vi in used:
        for k in range(4):
            if skin.bw[vi, k] > 0:
                bone_used.add(int(skin.bi[vi, k]))
    if o.get('extra_bones'):
        # 2026-10-15 opt-in (Ermion, long beard): body bones of the head mesh rig (e.g. neck / torso3 / torso2) added to the
        # fur's bone table, unweighted, so collision spheres can ride the chest instead of the head
        for nm in o['extra_bones']:
            if nm in skin.names:
                bone_used.add(skin.names.index(nm))
            else:
                log('  beard: extra_bones: %s not in the head mesh rig, skipped' % nm)
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
    cmap = None
    dgain_files = []
    if o['density_tex'] and o.get('density_gain') and not o.get('colour_map') and part == 'beard':
        # 2026-10-16 opt-in (Olgierd L494): the atlas alpha used as density texture leaves only ~27 % of the hairs (PT: 32
        # hairs per face, detailLODDensity saturates, L492); density_gain (+ density_gamma) writes a boosted copy of the
        # atlas alpha, min(1, gain x alpha^gamma), as the beard's own inline xbm on the same uvs. Absent: unchanged.
        import headtex
        import hairpipe as H_
        a2 = np.clip(np.power(alpha, float(o.get('density_gamma', 1.0))) * float(o['density_gain']), 0.0, 1.0)
        hh, ww = a2.shape
        d8 = np.round(a2 * 255.0)
        dp = prefix + H_.fur_path(C, comp)[:-len('.redfur')] + '_density.xbm'
        dgain_files.append((dp, headtex.make_xbm(headtex.CARD_STUB, ww, hh, headtex.encode_dxt5_alpha(d8), dp, ww // 4 * 16)))
        look['textures'][('physicalMaterials', 'volume', 'densityTex')] = dp
        log('  beard: density_gain %.2f gamma %.2f: atlas alpha mean %.3f -> %.3f (%dx%d, %s)' % (
            float(o['density_gain']), float(o.get('density_gamma', 1.0)), float(alpha.mean()), float(a2.mean()), ww, hh, dp))
        try:
            import hwpreview_pack as HP_
            from PIL import Image
            im_ = np.zeros((hh, ww, 4), np.uint8); im_[..., 3] = d8.astype(np.uint8)
            HP_.EXTRA_TEXTURES[dp] = Image.fromarray(im_, 'RGBA')
        except Exception:
            pass
    if o.get('density_flat') is not None and not o.get('colour_map') and part == 'beard':
        # 2026-10-16 opt-in (L505 beard report): a FLAT density map. In game the path tracer grows 32 hairs per face and keeps
        # each one when noise + .005 > densityTex(uv) (usePixelDensity); without a map every hair draws and a short beard
        # reads as a painted block (Svanrige L500 / L503 a, Olgierd L490 t1-t3), while the atlas alpha (density_tex) is
        # empty under some beards (Svanrige L503 b: invisible). density_flat = the share of hairs kept, everywhere.
        import headtex
        import hairpipe as H_
        pf = float(np.clip(float(o['density_flat']), 0.0, 1.0))
        sd_ = studio_defaults(o)
        if sd_ is not None and sd_.get('translate'):
            # l592 translation, hair COUNT (FINDINGS 12.10 gap closed 2026-10-17): a density the user set in the Studio is a
            # multiple of the fur's built density (what the Studio showed before the edit); the game ignores the scalar, so
            # the flat map carries it: keep = L592 count x Studio density / built density (the built density when no override)
            built = look['set'].get(('physicalMaterials', 'volume', 'density'))
            ov = _sv(studio_look_set(C), 'physicalMaterials.volume.density', None)
            if ov is not None and built and isinstance(built[1], (int, float)) and built[1] > 0:
                pf = float(np.clip(L592['count'] * ov / float(built[1]), 0.02, 1.0))
                log('  beard: l592 density: Studio %.3f / built %.3f x %.2f -> flat %.3f' % (ov, float(built[1]), L592['count'], pf))
        a2 = np.full((64, 64), pf, np.float32)
        d8 = np.round(a2 * 255.0)
        dp = prefix + H_.fur_path(C, comp)[:-len('.redfur')] + '_density.xbm'
        dgain_files.append((dp, headtex.make_xbm(headtex.CARD_STUB, 64, 64, headtex.encode_dxt5_alpha(d8), dp, 64 // 4 * 16)))
        look['textures'][('physicalMaterials', 'volume', 'densityTex')] = dp
        look['set'][('physicalMaterials', 'volume', 'densityTexChannel')] = ('EHairTextureChannel', 'ALPHA')
        look['set'][('physicalMaterials', 'volume', 'usePixelDensity')] = ('Bool', 1)
        log('  beard: density_flat %.2f: flat density map (%s), every hair kept with probability %.2f' % (pf, dp, pf))
        try:
            import hwpreview_pack as HP_
            from PIL import Image
            im_ = np.zeros((64, 64, 4), np.uint8); im_[..., 3] = d8.astype(np.uint8)
            HP_.EXTRA_TEXTURES[dp] = Image.fromarray(im_, 'RGBA')
        except Exception:
            pass
    if o.get('colour_map') and part == 'beard':
        # 2026-10-15 opt-in (Baron): colour by position + density re-baked into unique uvs (colour_map); the scalp part never
        import hairpipe as H_
        cmap = colour_map(V, F, uvs if o['density_tex'] else None, alpha, o, skin,
                          prefix + H_.fur_path(C, comp)[:-len('.redfur')], log)
        uvs = cmap['uvs']
        pm_, gm_ = 'physicalMaterials', 'graphicalMaterials'
        if 'density' in cmap['paths']:
            look['textures'][(pm_, 'volume', 'densityTex')] = cmap['paths']['density']
        look['textures'][(gm_, 'color', 'rootColorTex')] = cmap['paths']['colour']
        look['textures'][(gm_, 'color', 'tipColorTex')] = cmap['paths']['colour']
        look['set'][(gm_, 'color', 'textureBrightness')] = ('Float', 1.0)
        try:
            import hwpreview_pack as HP_
            HP_.EXTRA_TEXTURES.update(cmap['images'])      # preview / Studio packs: these textures are not in the game's cache
        except Exception:
            pass
    col = dict(pos=[], idx=[], rad=[], cap=[])
    data = redfur.write(strands, F, uvs, bones, col, look, auto_hide=C.get('auto_hide', 100.0))
    redfur.verify(data)
    head = growth.Head(head_rel)
    res = dict(S=strands, F=F, roots=V)
    log('fur %s (beard): %d guides, %d faces, %d bytes' % (comp, len(strands), len(F), len(data)))
    return dict(fur=data, template=tpl, comp=comp if part == 'beard' else (sc.get('name') or 'scalp_' + (comp[3:] if comp.startswith('hb_') else comp)), src_comp=comp, mesh=mesh, res=res, head=head, brows=bool((nb and o['keep_brows']) or nk), keep_brows=bool(o['keep_brows']), keep_boxes=o.get('keep_card_boxes'), keep_opts=(int(o.get('keep_card_min_verts', 1)), o.get('keep_card_below')),
                tex=tex, skin=skin, samples=S, extra_files=(cmap['files'] if cmap else []) + dgain_files)


def brow_mask(ch_mesh_names, keep_boxes=None, brows=True, keep_opts=(1, None)):
    def fn(ch):
        _lab, cls = card_classes(ch, ch_mesh_names, keep_boxes=keep_boxes, keep_min_verts=keep_opts[0], keep_below=keep_opts[1])
        return ((cls == 'brow') if brows else np.zeros(len(cls), bool)) | (cls == 'keep')
    return fn


def bake_density(geo, length_scale, bk, base_path, share, log=print):
    """2026-10-17 (FINDINGS 13.3) opt-in `beard.density_bake`: the density map the game reads is PAINTED from what the Studio
    shows instead of flat. Per texel of a per-face unique uv layout (LSCM flattening of the growth mesh, as colour_map.flat):
      keep = share x smooth((L_eff - len0) / (len1 - len0)) x smooth(d_edge / fade)
    L_eff = the guides' effective hair length (guide length x lengthScale, interpolated over the face): hairs the Studio draws
    as near-invisible stubble (< len0 mm) are dropped in the game, where every 2 mm hair is an opaque dot; d_edge = distance
    to the growth-mesh boundary (fade mm): the boundary thins out gradually instead of every boundary face being on or off.
    Keys: len0_mm (1), len1_mm (4), fade_mm (0 = none), size (1024). -> (uv per face corner (F*3, 2), [(path, bytes)], keep stats)"""
    import headtex
    G = [np.asarray(g, float) for g in geo['guides']]
    V = np.array([g[0] for g in G]); F = np.asarray(geo['F'], int).reshape(-1, 3)
    Lg = np.array([np.linalg.norm(np.diff(g, axis=0), axis=1).sum() for g in G]) * float(length_scale) * 1000.0
    len0, len1, fade = float(bk.get('len0_mm', 1.0)), float(bk.get('len1_mm', 4.0)), float(bk.get('fade_mm', 0.0))
    n = int(bk.get('size', 1024))
    # boundary distance per vertex (3D, mm): edges with one incident face
    from collections import Counter
    ec = Counter()
    for a, b, c in F:
        for e in ((a, b), (b, c), (c, a)):
            ec[tuple(sorted(e))] += 1
    bnd = [e for e, k in ec.items() if k == 1]
    dist = np.full(len(V), 1e9)
    if bnd and fade > 0:
        P = V
        for a, b in bnd:
            A, B = V[a], V[b]; AB = B - A; L2 = max(float(AB @ AB), 1e-12)
            t = np.clip(((P - A) @ AB) / L2, 0, 1)[:, None]
            dist = np.minimum(dist, np.linalg.norm(P - (A + t * AB), axis=1))
        dist *= 1000.0
    # unique uvs: LSCM flattening of the growth mesh, mapped into (0, 1) with a margin
    Q = _lscm(V, F)[0]
    lo, hi = Q.min(0), Q.max(0); span = float((hi - lo).max()) * 1.04
    uv = 0.5 + (Q - (lo + hi) / 2) / span
    U = uv[F].copy()
    ar = lambda W: (W[:, 1, 0] - W[:, 0, 0]) * (W[:, 2, 1] - W[:, 0, 1]) - (W[:, 2, 0] - W[:, 0, 0]) * (W[:, 1, 1] - W[:, 0, 1])
    if (ar(U) > 0).sum() > (ar(U) < 0).sum():
        U[..., 0] = 1.0 - U[..., 0]
    fid, bar = _raster(U, F, n)
    Lt = np.einsum('hwk,hwk->hw', bar, Lg[F][fid])
    kL = _smooth((Lt - len0) / max(len1 - len0, 1e-6)) if len1 > len0 else np.ones_like(Lt)
    if bk.get('coverage'):
        # 2026-10-17 (FINDINGS 13.3): per-guide visible coverage MEASURED on the Studio's own render (toolarch\studio_coverage.py:
        # the skin seen through the hair at every projected root, 3 views) -> keep = share x coverage^gamma: the game thins
        # where the Studio shows skin, whatever made it (short hairs, few hairs, thin hairs)
        cv = json.load(open(bk["coverage"] if os.path.isabs(bk["coverage"]) else os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), bk["coverage"])))
        cg = np.asarray(cv['coverage'], float)
        if len(cg) != len(V):
            raise SystemExit('density_bake.coverage: %d values for %d guides (%s)' % (len(cg), len(V), bk['coverage']))
        Ct = np.einsum('hwk,hwk->hw', bar, cg[F][fid])
        kL = kL * np.power(np.clip(Ct, 0.0, 1.0), float(bk.get('gamma', 1.0)))
    if fade > 0:
        Dt = np.einsum('hwk,hwk->hw', bar, dist[F][fid]); kE = _smooth(Dt / fade)
    else:
        kE = np.ones_like(Lt)
    keep = np.clip(float(share) * kL * kE, 0.0, 1.0)
    d8 = np.round(keep * 255.0)
    path = base_path + '_density.xbm'   # the flat map's own path: the fur already imports it (generate), the bytes are replaced
    files = [(path, headtex.make_xbm(headtex.CARD_STUB, n, n, headtex.encode_dxt5_alpha(d8), path, n // 4 * 16))]
    st = dict(faces=len(F), guides=len(V), share=float(share), len0=len0, len1=len1, fade=fade, boundary_edges=len(bnd),
              eff_len_mm_p50=round(float(np.median(Lg)), 2), keep_mean=round(float(keep.mean()), 3), keep_p10=round(float(np.percentile(keep, 10)), 3),
              faces_under_len0=int((Lg[F].mean(1) < len0).sum()))
    log('  beard: density_bake %s' % json.dumps(st))
    try:
        import hwpreview_pack as HP_
        from PIL import Image
        im_ = np.zeros((n, n, 4), np.uint8); im_[..., 3] = d8.astype(np.uint8)
        HP_.EXTRA_TEXTURES[path] = Image.fromarray(im_, 'RGBA')
    except Exception:
        pass
    return U.reshape(-1, 2), files, st


def studio_beard(C, fur, log=print, extra_files=None, base_path=None):
    """opt-in "studio_geo_beard" (2026-10-14): the Groom Studio's Beard-mode result (geometry + look values) replaces the
    generated beard fur's, with the Studio export's own function (desk_export.edit_fur). Made by studio_to_config.py."""
    import tempfile
    import studio_geo
    DE = studio_geo._desk_export()
    sg = C['studio_geo_beard']
    geo = studio_geo.load_geo(sg['geo'])
    look = json.load(open(studio_geo._path(sg['look']))) if sg.get('look') else {}
    sd = studio_defaults(cfg(C))
    if sd is not None and cfg(C).get('studio_defaults'):
        # 2026-10-16: the Studio look's own detailLODWidth / textureBrightness are replaced by the "game like the Studio"
        # values (L521: the Studio default render shows neither; S4 dlw .3 / S6 tb 1.0 were the closest rows)
        st = look.setdefault('set', {})
        dlw_, tb_ = l592_lod_tb(C, sd, (C['beard'].get('look') or {}).get('root_color') if isinstance(C.get('beard'), dict) else None)
        st['levelOfDetail.detailLOD.detailLODWidth'] = ['Float', dlw_]
        st['graphicalMaterials.color.textureBrightness'] = ['Float', tb_]
        log('  studio_geo_beard: studio_defaults -> detailLODWidth %.2f, textureBrightness %.2f (density_flat %s)' % (
            st['levelOfDetail.detailLOD.detailLODWidth'][1], st['graphicalMaterials.color.textureBrightness'][1], cfg(C).get('density_flat')))
    if sd is not None and sd.get('translate'):
        # l592 translation of the Studio look (see L592): length and width from the Studio's values (or the fur's own when the
        # look has none), colour keys the user set in the Studio through the same colour factor as the config colours
        st = look.setdefault('set', {})
        o = cfg(C)
        # the Studio shows the fur's built value where the look has no override: the config's look (length_scale / width) is that value
        lk0 = o.get('look', {})
        base_len = _sv(st, 'physicalMaterials.volume.lengthScale', float(lk0['length_scale']) if isinstance(lk0.get('length_scale'), (int, float)) else 1.0)
        base_w = _sv(st, 'physicalMaterials.strandWidth.width', float(lk0['width']) if isinstance(lk0.get('width'), (int, float)) else 0.3)
        # L599 (FINDINGS 12.11, measured): the game honours lengthScale 1:1 up to 1.0 and clamps above it (1.5 = 1.0 within the
        # noise floor; .3 / .6 differ strongly); the earlier x1.86 was an eyeball fit and made the user's .27 a .5 beard
        st['physicalMaterials.volume.lengthScale'] = ['Float', round(min(L592['length_cap'], base_len * L592['length']), 4)]
        st['physicalMaterials.strandWidth.width'] = ['Float', round(base_w * L592['width'], 4)]
        for ck in ('graphicalMaterials.color.rootColor', 'graphicalMaterials.color.tipColor'):
            v = st.get(ck)
            if isinstance(v, list) and len(v) == 2 and isinstance(v[1], (list, tuple)) and len(v[1]) >= 3:
                st[ck] = [v[0], l592_colour(list(v[1])[:3], 'tip' if ck.endswith('tipColor') else 'root') + list(v[1])[3:]]
        log('  studio_geo_beard l592 translation: lengthScale %.3f (Studio %.3f x %.2f), width %.3f (Studio %.3f x %.2f), density_flat %s, colours root x %s tip x %s' % (
            st['physicalMaterials.volume.lengthScale'][1], base_len, L592['length'], st['physicalMaterials.strandWidth.width'][1], base_w, L592['width'], o.get('density_flat'), tuple(round(x, 3) for x in L592['colour_root']), tuple(round(x, 3) for x in L592['colour_tip'])))
    bk = cfg(C).get('density_bake')
    if bk and extra_files is not None:
        # 2026-10-17 opt-in (FINDINGS 13.3): painted density map from the Studio's effective hair lengths + an edge fade, on
        # per-face unique uvs (replaces the geo's shared atlas uvs and the flat map's texture reference)
        import hairpipe as H_
        st = look.setdefault('set', {})
        o = cfg(C)
        ls_ = _sv(st, 'physicalMaterials.volume.lengthScale', float(o['look']['length_scale']) if isinstance(o['look'].get('length_scale'), (int, float)) else 1.0)
        share = float(o.get('density_flat') if o.get('density_flat') is not None else 1.0)
        uv2, files, _ = bake_density(geo, ls_, bk if isinstance(bk, dict) else {}, base_path, share, log)
        geo = dict(geo); geo['uv'] = uv2.tolist()
        mn = float(bk.get('min_len_mm', 0.0)) if isinstance(bk, dict) else 0.0
        if mn > 0 and bk.get('coverage'):
            # 2026-10-17 (FINDINGS 13.4, coordinator): where the Studio SHOWS hair (coverage >= min_len_cov, .6) but the effective
            # hair is shorter than what the game renders visibly, the guide is lengthened along its own shape so that
            # length x lengthScale = min_len_mm (roots fixed; the under-chin V of Olgierd 0850: 0.8 mm hairs invisible in game)
            import json as _j
            cg = np.asarray(_j.load(open(bk['coverage'] if os.path.isabs(bk['coverage']) else os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), bk['coverage'])))['coverage'], float)
            cthr = float(bk.get('min_len_cov', 0.6)); n_ = 0; G2 = []
            for gi, g in enumerate(geo['guides']):
                g = np.asarray(g, float); L_ = float(np.linalg.norm(np.diff(g, axis=0), axis=1).sum()) * 1000.0 * ls_
                if cg[gi] >= cthr and 0 < L_ < mn:
                    g = g[0] + (g - g[0]) * (mn / L_); n_ += 1
                G2.append(g.tolist())
            geo['guides'] = G2
            log('  beard: density_bake min_len %.1f mm (coverage >= %.2f): %d of %d guides lengthened' % (mn, cthr, n_, len(G2)))
        # the flat map written by generate() (same path, imported by the fur) is replaced by the baked one
        keep_ = [f for f in extra_files if f[0] != files[0][0]]
        if len(keep_) == len(extra_files):
            log('  beard: density_bake: no flat map at %s to replace (density_flat must be set, the preset does it)' % files[0][0])
        extra_files[:] = keep_ + files
        st['physicalMaterials.volume.usePixelDensity'] = ['Bool', 1]
    if sg.get('extra_set'):
        # 2026-10-17 opt-in (FINDINGS 12.5): "studio_geo_beard.extra_set" = SDK-path keys applied AFTER the Studio look, so a
        # lab row can change width / clumping / waves / lengthNoise of a Studio-geometry beard (beard.look.* and
        # beard.look.extra_set are replaced by the Studio look's own values here and never reached the game in L583 / L585).
        st = look.setdefault('set', {})
        for k, v in sg['extra_set'].items():
            st[k] = list(v) if isinstance(v, (list, tuple)) else ['Float', float(v)]
        log('  studio_geo_beard.extra_set: %s' % ', '.join('%s=%s' % (k, st[k][1]) for k in sg['extra_set']))
    fd, tmp = tempfile.mkstemp(suffix='.redfur')
    try:
        with os.fdopen(fd, 'wb') as fh:
            fh.write(fur)
        out, changed = DE.edit_fur(tmp, {'ops': ['studio']}, look, geo, False, True, None, log,
                                   w3_keys=bool(sg.get('w3_keys')))   # opt-in 2026-10-15 (desk_export.W3_KEY_TYPES); studio_to_config sets it for new configs
    finally:
        os.remove(tmp)
    log('  studio_geo_beard %s: %d guides, %d faces; changed: %s' % (sg['geo'], len(geo['guides']), len(geo['F']), ', '.join(changed)))
    return out


def add_to_build(C, info, files, bufs, tpls, P, log=print):
    """hairpipe.build hook (only called when C['beard'] is set): beard fur + template patch (+ eyebrow subset meshes).
    The template already patched for the hair (same head template, e.g. Lambert's h_01_ma__lambert.w2ent) is patched
    again in place in `files`; otherwise the vanilla beard template is patched and added."""
    import template
    import strand_edits
    import hairpipe as H
    # 2026-10-16 (Svanrige L508): density_flat textures go under the variant prefix too (two variants with different
    # density_flat values in one batch otherwise share one xbm path); only configs with density_flat change
    B = generate(C, info, log, prefix=P('') if (cfg(C).get('colour_map') or (cfg(C).get('density_tex') and cfg(C).get('density_gain'))
                                                 or cfg(C).get('density_flat') is not None) else '')
    if B is None:
        return None
    if C.get('studio_geo_beard'):
        B.setdefault('extra_files', [])
        B['fur'] = studio_beard(C, B['fur'], log, extra_files=B['extra_files'], base_path=P(H.fur_path(C, B['comp']))[:-len('.redfur')])
    fp = P(H.fur_path(C, B['comp']))
    files.append((fp, B['fur']))
    files.extend(B.get('extra_files') or [])      # opt-in colour_map textures (already under the prefix)
    extra, fallback = [], None
    if B['brows']:
        names = G.mesh_bones(B['mesh'])['names']
        keep = brow_mask(names, B.get('keep_boxes'), B.get('keep_brows', True), B.get('keep_opts', (1, None)))
        bm = P(B['mesh'][:-len('.w2mesh')] + '_brows.w2mesh')
        w, b, stt = template.subset_mesh(B['mesh'], keep)
        log('  beard: eyebrow cards kept as %s: (verts, live tris, tris) per chunk %s' % (bm, stt))
        files.append((bm, w)); bufs.append((bm + '.1.buffer', b))
        extra.append((B['comp'] + '_brows', bm))
        fb = P(B['mesh'][:-len('.w2mesh')] + '_nobrows.w2mesh')
        zs = cfg(C).get('scalp')
        if zs:
            zz = float(zs.get('z', 1.34))
            w, b, stt = template.subset_mesh(B['mesh'], lambda ch: ~keep(ch) & ~scalp_cards(ch, zz))
        else:
            w, b, stt = template.subset_mesh(B['mesh'], lambda ch: ~keep(ch))
        log('  beard: fallback (HairWorks off) = beard cards without the eyebrows %s: %s' % (fb, stt))
        files.append((fb, w)); bufs.append((fb + '.1.buffer', b))
        fallback = fb
    tp = P(B['template'])
    i = next((k for k, (rel, _d) in enumerate(files) if rel == tp), None)
    src = files[i][1] if i is not None else G.get(B['template'])
    new = template.patch(src, B['comp'], fp, extra, fallback)
    # opt-in beard.outer_only (2026-10-15, Baron): classes exported only in the outer table (CAnimMimicParam), see template.validate
    template.validate(new, B['template'], outer_only=tuple(cfg(C).get('outer_only') or ()))
    if i is not None:
        files[i] = (tp, new)
    else:
        files.append((tp, new))
        tpls.append((B['template'], tp))
    sc = cfg(C).get('scalp')
    if sc:
        # L250 opt-in: scalp hair = a second fur component cloned from the beard component (own cards subset as fallback)
        B2 = generate(C, info, log, part='scalp')
        zz = float(sc.get('z', 1.34))
        fp2 = P(H.fur_path(C, B2['comp']))
        files.append((fp2, B2['fur']))
        sm = P(B['mesh'][:-len('.w2mesh')] + '_scalpcards.w2mesh')
        w, b, stt = template.subset_mesh(B['mesh'], lambda ch: scalp_cards(ch, zz))
        log('  beard: scalp cards (HairWorks off fallback) %s: %s' % (sm, stt))
        files.append((sm, w)); bufs.append((sm + '.1.buffer', b))
        i = next(k for k, (rel, _d) in enumerate(files) if rel == tp)
        def _f(d):
            template._add_mesh_component(d, B['comp'], B2['comp'], sm)
            template._swap(d, B2['comp'], fp2, sm)
        files[i] = (tp, template._edit_both(files[i][1], _f))
        template.validate(files[i][1], B['template'])
        try:
            ep2 = strand_edits.export_json(C, B2['res'], B2['head'], dict(info), comp=B2['comp'], log=log, index=2)
            strand_edits.export_hw_pack(C, B2['fur'], ep2, tex=None, comp=B2['comp'], index=2, log=log)
        except Exception as ex:
            log('  scalp exports FAILED: %r' % (ex,))
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
