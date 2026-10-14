r"""hairpipe - one entry point: vanilla hair cards of a character -> HairWorks fur mod.

  py tools\hairworks\hairpipe.py <char> discover          # appearances, hair components, heads, bodies, beards
  py tools\hairworks\hairpipe.py <char> plot [OUT.png]    # offline guide / lane plots (no game)
  py tools\hairworks\hairpipe.py <char> build [--install] # generate redfur(s), patch templates, package the mod
  py tools\hairworks\hairpipe.py <char> capture [--variant v] [--baseline]   # in-game shots via capture.py
  py tools\hairworks\hairpipe.py <char> uninstall

<char> = tools\hairworks\chars\<char>.json (see PIPELINE.md for every key). Output mod: _modCreation\<mod>\content\
{blob0.bundle, buffers0.bundle, metadata.store} (legacy format, loads in 5.0). --install copies it to <game>\mods
(refuses while witcher3.exe runs, checks for path conflicts with other installed mods).
"""
import json
import os
import shutil
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
ROOT = os.path.dirname(TOOLS)
GAME = os.environ.get("HWFA_GAME") or os.path.dirname(ROOT)   # HWFA_GAME: game folder of a release install
sys.path.insert(0, HERE)
sys.path.insert(0, TOOLS)
import gamefiles as G

DEFAULT_LOOK = {
    'mat_src': 'characters\\models\\geralt\\coif\\model\\geralt_v1halftail_long.redfur',
    'root_color': [190, 188, 182], 'tip_color': [222, 220, 214], 'texture_brightness': 1.0,
    'root_tip_weight': 0.5, 'root_tip_falloff': 0.3,
    'density': 1.3, 'length_noise': 0.25, 'length_scale': 1.0,
    'width': 1.0, 'width_root': 0.9, 'width_tip': 0.3, 'width_noise': 0.2,
    'clump_scale': 0.35, 'clump_roundness': 1.0, 'clump_noise': 0.5,
    'wave_scale': 0.15, 'wave_freq': 3.0,
    'stiffness': 1.0, 'root_stiffness': 1.0, 'bend_stiffness': 1.0, 'tip_stiffness': 0.4,
    # dynamics (R3): None = keep the source material's value. stiffness / bend curves = multipliers root -> tip
    'stiffness_curve': None, 'bend_curve': None, 'damping': None, 'stiffness_damping': None, 'inertia_scale': None,
    'spec_color': [120, 117, 110], 'spec_primary': 0.1, 'spec_power_primary': 600.0, 'spec_secondary': 0.05,
    'spec_power_secondary': 40.0, 'spec_env': 0.02, 'glint': 0.04, 'diffuse_blend': None,
    'shadow_sigma': 2.0, 'shadow_density': 0.5, 'ambient_env': 0.7,
    'use_card_texture': True,
}
DEFAULT_GROWTH = {'root_spacing': 0.0125, 'reach': 0.012, 'alpha_vis': 0.3, 'lane_w': 0.012, 'K': 8,
                  'hug': True, 'hug_c0': 0.003, 'hug_k': 0.35, 'edge': 0.034, 'rmax': 0.022, 'zmin_rel': 0.20,
                  'coverage': [0.25, 0.4, 0.58], 'sphere_pad': 0.004}


def load_config(name):
    p = os.path.join(HERE, 'chars', name + '.json')
    C = json.load(open(p))
    C['name'] = name
    C.setdefault('captures', 'analysis\\hairworks_%s\\captures' % name)
    C.setdefault('mod', 'mod%sHairworks' % name.capitalize())
    C.setdefault('appearance', '')
    C['_look_explicit'] = set(C.get('look', {}).keys())
    look = dict(DEFAULT_LOOK); look.update(C.get('look', {})); C['look'] = look
    gr = dict(DEFAULT_GROWTH); gr.update(C.get('growth', {})); C['growth'] = gr
    return C


def _studio_hair_pts(C):
    """all guide points (model space, every 2nd) of the Studio geometry the config applies (opt-in growth.outfit_hair)"""
    import studio_geo
    g_ = studio_geo.load_geo(C['studio_geo']['geo'])
    return np.concatenate([np.asarray(s_, float).reshape(-1, 3)[::2] for s_ in g_['guides']])


def discover(C, log=print):
    import discover as D
    G.use_mods(C.get('source_mods', []))     # R81: read the character's files from an installed hair mod first
    info = D.discover(C['template'], C.get('discover_appearance', C.get('appearance')) or None, log, reach=C.get('hair_reach', 0.6), top_tol=C.get('hair_top_tol', 0.05))
    for tpl in C.get('hair_item_templates', []):
        # 2026-10-11 opt-in (Geralt's card hairstyles): the hair lives in an ITEM template (barber coif) that the NPC
        # entity does not include; its hair-card components are added to the discovered hair (head / rig stay the NPC's)
        for comp, cls, mesh, fur in D.mesh_comps(tpl):
            if D.classify(mesh) == 'hair':
                info['hair'].append(dict(template=tpl, comp=comp, cls=cls, mesh=mesh, fur=fur, used_by=['item']))
    if C.get('hair'):
        want = [(h.get('template'), h.get('comp')) for h in C['hair']]
        tpl_only = {t.lower() for t, c in want if t and not c}      # R35: select by hair template alone
        info['hair'] = [h for h in info['hair'] if (h['template'], h['comp']) in want or (None, h['comp']) in want
                        or (h['template'] or '').lower() in tpl_only]
    return info


def uv_box(box):
    u0, v0, u1, v1 = box
    return lambda uv, pos: (uv[:, 0] >= u0) & (uv[:, 0] <= u1) & (uv[:, 1] >= v0) & (uv[:, 1] <= v1)


def acc_mask(acc, ch):
    """R81 accessory selection on a mesh chunk -> bool vertex mask. 'uv_box' [u0,v0,u1,v1] (classic), or 'pos_boxes'
    [[xmin,xmax,ymin,ymax,zmin,zmax], ...] in mesh space ('mirror_x': true also takes -x) for pieces that are not
    separated in the atlas (Avallach's braid: unwelded quads / triangles in front of each ear), optionally limited to
    small components ('max_comp_verts') so the regular hair cards passing through the box are not taken.
    'materials' [ids] (2026-10-14, hairband Ciri: the headband is its own leather material in the card mesh) takes
    every vertex of the chunks with those material ids."""
    if acc.get('materials') is not None:
        vm = ch.get('vmat')
        m = np.isin(vm if vm is not None else np.full(len(ch['pos']), ch['mat']), acc['materials'])
    elif acc.get('uv_box'):
        m = uv_box(acc['uv_box'])(ch['uv'], ch['pos'])
    else:
        P = ch['pos']
        m = np.zeros(len(P), bool)
        for b in acc.get('pos_boxes', []):
            for sx in ((1, -1) if acc.get('mirror_x') else (1,)):
                x = P[:, 0] * sx
                m |= (x >= b[0]) & (x <= b[1]) & (P[:, 1] >= b[2]) & (P[:, 1] <= b[3]) & (P[:, 2] >= b[4]) & (P[:, 2] <= b[5])
    if acc.get('min_voxel_tris'):
        # detail geometry (braids, ties): keep only triangles in 12 mm cells holding >= min_voxel_tris triangle centroids
        tri = ch['idx'].reshape(-1, 3)
        cen = ch['pos'][tri].mean(1)
        vox = np.floor(cen / acc.get('voxel', 0.012)).astype(np.int64)
        key = (vox[:, 0] + 50000) * 10 ** 10 + (vox[:, 1] + 50000) * 10 ** 5 + (vox[:, 2] + 50000)
        _u, inv, cnt = np.unique(key, return_inverse=True, return_counts=True)
        dense = np.zeros(len(ch['pos']), bool)
        dense[tri[cnt[inv] >= acc['min_voxel_tris']].reshape(-1)] = True
        m &= dense
    if acc.get('max_comp_verts') or acc.get('inside_frac'):
        lab = G.components(ch['nv'], ch['idx'])
        ids, cnt = np.unique(lab, return_counts=True)
        ok = np.ones(len(ids), bool)
        if acc.get('max_comp_verts'):
            ok &= cnt <= acc['max_comp_verts']
        if acc.get('inside_frac'):            # the piece must lie (almost) wholly inside the boxes
            ins = np.bincount(np.searchsorted(ids, lab), weights=m.astype(float), minlength=len(ids))
            ok &= ins / cnt >= acc['inside_frac']
        m &= np.isin(lab, ids[ok])
    return m


def card_texture(mesh):
    for base, mp in G.materials(mesh):
        d = mp.get('Diffuse')
        if isinstance(d, dict) and d.get('@import'):
            return d['@import']
    raise SystemExit('no Diffuse texture on ' + mesh)


def coverage_targets(R, F, levels, rng, interior=()):
    """per-face wanted card coverage: faces touching the region boundary (hairline / partings) get the sparse
    levels, interior faces the dense one -> wispy hairline, full interior."""
    from collections import Counter
    e = Counter()
    for a, b, c in F:
        for u, v in ((a, b), (b, c), (c, a)):
            e[(min(u, v), max(u, v))] += 1
    bnd = set()
    for (u, v), n in e.items():
        if n == 1:
            bnd.update((u, v))
    bnd -= set(int(i) for i in interior)        # part_tail_dense: the ponytail parting is not a hairline
    ring = np.full(len(R), 9)
    ring[list(bnd)] = 0
    adj = {}
    for a, b, c in F:
        for u, v in ((a, b), (b, c), (c, a)):
            adj.setdefault(u, set()).add(v); adj.setdefault(v, set()).add(u)
    for r in range(1, 3):
        for i in np.where(ring == r - 1)[0]:
            for j in adj.get(i, ()):
                if ring[j] > r:
                    ring[j] = r
    fr = ring[F].min(1)
    t = np.array([levels[min(int(x), len(levels) - 1)] for x in fr], float)
    return t + rng.uniform(-0.06, 0.06, len(t)), fr


def look_params(L, tex):
    pm, gm = 'physicalMaterials', 'graphicalMaterials'
    s = {}
    F = lambda x: ('Float', float(x))
    col = lambda c: ('Color', tuple(c) + (255,))
    s[(pm, 'volume', 'density')] = F(L['density'])
    s[(pm, 'volume', 'lengthNoise')] = F(L['length_noise'])
    s[(pm, 'volume', 'lengthScale')] = F(L['length_scale'])
    s[(pm, 'strandWidth', 'width')] = F(L['width'])
    s[(pm, 'strandWidth', 'widthRootScale')] = F(L['width_root'])
    s[(pm, 'strandWidth', 'widthTipScale')] = F(L['width_tip'])
    s[(pm, 'strandWidth', 'widthNoise')] = F(L['width_noise'])
    s[(pm, 'clumping', 'clumpScale')] = F(L['clump_scale'])
    s[(pm, 'clumping', 'clumpRoundness')] = F(L['clump_roundness'])
    s[(pm, 'clumping', 'clumpNoise')] = F(L['clump_noise'])
    s[(pm, 'waveness', 'waveScale')] = F(L['wave_scale'])
    s[(pm, 'waveness', 'waveFreq')] = F(L['wave_freq'])
    s[(pm, 'stiffness', 'stiffness')] = F(L['stiffness'])
    s[(pm, 'stiffness', 'rootStiffness')] = F(L['root_stiffness'])
    s[(pm, 'stiffness', 'bendStiffness')] = F(L['bend_stiffness'])
    s[(pm, 'stiffness', 'tipStiffness')] = F(L['tip_stiffness'])
    V = lambda v: ('Vector', tuple(v))
    if L.get('stiffness_curve') is not None:
        s[(pm, 'stiffness', 'stiffnessCurve')] = V(L['stiffness_curve'])
    if L.get('bend_curve') is not None:
        s[(pm, 'stiffness', 'bendStiffnessCurve')] = V(L['bend_curve'])
    if L.get('stiffness_damping') is not None:
        s[(pm, 'stiffness', 'stiffnessDamping')] = F(L['stiffness_damping'])
    if L.get('damping') is not None:
        s[(pm, 'simulation', 'damping')] = F(L['damping'])
    if L.get('inertia_scale') is not None:
        s[(pm, 'simulation', 'inertiaScale')] = F(L['inertia_scale'])
    s[(gm, 'color', 'rootColor')] = col(L['root_color'])
    s[(gm, 'color', 'tipColor')] = col(L['tip_color'])
    s[(gm, 'color', 'textureBrightness')] = F(L['texture_brightness'])
    s[(gm, 'color', 'rootTipColorWeight')] = F(L['root_tip_weight'])
    s[(gm, 'color', 'rootTipColorFalloff')] = F(L['root_tip_falloff'])
    s[(gm, 'specular', 'specularColor')] = col(L['spec_color'])
    s[(gm, 'specular', 'specularPrimary')] = F(L['spec_primary'])
    s[(gm, 'specular', 'specularPowerPrimary')] = F(L['spec_power_primary'])
    s[(gm, 'specular', 'specularSecondary')] = F(L['spec_secondary'])
    s[(gm, 'specular', 'specularPowerSecondary')] = F(L['spec_power_secondary'])
    s[(gm, 'specular', 'specularEnvScale')] = F(L['spec_env'])
    s[(gm, 'glint', 'glintStrength')] = F(L['glint'])
    s[(gm, 'shadow', 'shadowSigma')] = F(L['shadow_sigma'])
    s[(gm, 'shadow', 'shadowDensityScale')] = F(L['shadow_density'])
    if L.get('ambient_env') is not None:
        s[(gm, 'color', 'ambientEnvScale')] = F(L['ambient_env'])
    if L.get('diffuse_blend') is not None:
        s[(gm, 'diffuse', 'diffuseBlend')] = F(L['diffuse_blend'])
    tx = {}
    if L['use_card_texture'] or L.get('card_density_only'):
        # card alpha drives strand density (sparse where the cards are sparse)
        tx[(pm, 'volume', 'densityTex')] = tex
        s[(pm, 'volume', 'densityTexChannel')] = ('EHairTextureChannel', 'ALPHA')
        s[(pm, 'volume', 'usePixelDensity')] = ('Bool', 1)
    if L['use_card_texture'] and not L.get('card_density_only'):
        # card diffuse = strand colour (root / tip colours then do not tint); card_density_only keeps the fur's own
        # root / tip colours (tinted hair: red, blonde) while the density still follows the cards
        tx[(gm, 'color', 'rootColorTex')] = tex
        tx[(gm, 'color', 'tipColorTex')] = tex
    for k, v in L.get('extra_set', {}).items():
        s[tuple(k.split('.'))] = tuple(v)
    return s, tx


BONES = ['head', 'neck', 'hroll', 'torso3', 'r_shoulder', 'l_shoulder']


def bone_table(C, info, log=print, extra=()):
    """bind poses of the character's own skeleton (rig from the template, or C['rig']); C['bone_source'] (a vanilla
    redfur of the same skeleton) is the alternative."""
    import redfur, rig
    if C.get('bone_source'):
        return redfur.bone_table(C['bone_source'], C.get('bones'))
    r = C.get('rig') or next((x for x in info.get('rigs', []) if 'base_entities' in x), None) or info['rigs'][0]
    bt = rig.bone_table(r, list(C.get('bones', BONES)) + [b for b in extra if b not in C.get('bones', BONES)])
    log('bones from %s: %s' % (r, [b['name'] for b in bt]))
    assert bt and bt[0]['name'] == 'head', bt
    return bt


def fur_path(C, comp):
    return C.get('fur_dir', os.path.dirname(C['template']).replace('npc_entities', 'models') + '\\hairworks') + '\\' + comp + '.redfur'


def split_tri_mask(sc, ch, head, log=None):
    m_ = _split_tri_mask(sc, ch, head, log)
    df = sc.get('drop_front') if isinstance(sc, dict) else None
    if df:
        # 2026-10-06 (Oriana L133 diagnosis: the 3 big kept components 9235 / 9744 / 9843 span x -108..106 and include the vanilla
        # FRINGE cards, y up to +37 mm, z 29..71 above the head centre = the blunt fringe seen in game): triangles with centroid
        # y > df.y (m rel. head centre), |x| < df.x_abs and z > df.z_min are taken off the cards (the strands cover them)
        idx_ = np.asarray(ch['idx']).reshape(-1, 3)
        c_ = np.asarray(ch['pos'], float)[idx_].mean(1) - np.asarray(head.c, float)
        bad_ = (c_[:, 1] > df.get('y', 0.0)) & (np.abs(c_[:, 0]) < df.get('x_abs', 0.07)) & (c_[:, 2] > df.get('z_min', 0.0))
        if log:
            log('  split_cards drop_front: %d kept triangles taken off the cards' % int((m_ & bad_).sum()))
        m_ = m_ & ~bad_
    return m_


def _split_tri_mask(sc, ch, head, log=None):
    """split_cards (2026-10-06, Anna): bool per triangle of chunk `ch` = triangle stays on the VANILLA cards (the updo).
    mode 'z': triangle centroid above head.top - z_rel.
    mode 'components' (default): whole connected card components whose LOWEST vertex is above head.top - nape_rel
    (the updo cards end above the nape, the hanging locks reach below it); `include` / `exclude` = lists of [x, y, z]
    points, the component with a vertex within 6 mm of the point is forced in / out."""
    idx = np.asarray(ch['idx']).reshape(-1, 3)
    pos = np.asarray(ch['pos'], float)
    mode = sc.get('mode', 'components')
    if mode == 'z':
        return pos[idx].mean(1)[:, 2] > head.top - float(sc.get('z_rel', 0.10))
    if mode == 'boxes':
        # 2026-10-06 (Iris: the hair mesh is thousands of tiny strips, so 'components' keeps the whole cap): whole connected
        # components whose CENTROID lies in one of `boxes` = [{"min": [x, y, z], "max": [x, y, z]} ...] in mm relative to the
        # head centre (head.c, entity space; null = open; optional "le": [a, b, c, d] = also a x + b y + c z <= d; "sd_min" / "sd_max": mean stand-off from the skin, mm). `by`: "component" (default) or "triangle". include / exclude as below.
        lab = G.components(ch['nv'], ch['idx'])
        cen_t = pos[idx].mean(1) - np.asarray(head.c, float)
        by_tri = sc.get('by', 'component') == 'triangle'
        sd_t = head.signed(pos[idx].mean(1))[0]                     # stand-off of each triangle from the skin
        if by_tri:
            cen, sdc = cen_t, sd_t
        else:
            cen = np.zeros((lab.max() + 1, 3)); cnt = np.zeros(lab.max() + 1); sdc = np.zeros(lab.max() + 1)
            np.add.at(cen, lab[idx[:, 0]], cen_t); np.add.at(cnt, lab[idx[:, 0]], 1); np.add.at(sdc, lab[idx[:, 0]], sd_t)
            cen /= np.maximum(cnt, 1)[:, None]; sdc /= np.maximum(cnt, 1)
        ok = np.zeros(len(cen), bool)
        for bx in sc.get('boxes', []):
            lo = np.array([-np.inf if v is None else v / 1000.0 for v in bx.get('min', [None] * 3)])
            hi = np.array([np.inf if v is None else v / 1000.0 for v in bx.get('max', [None] * 3)])
            sel_ = np.all((cen >= lo) & (cen <= hi), axis=1)
            for pl_ in ([bx['le']] if bx.get('le') and not isinstance(bx['le'][0], (list, tuple)) else bx.get('le', [])):
                sel_ &= cen @ np.asarray(pl_[:3], float) * 1000.0 <= pl_[3]          # half-space a x + b y + c z <= d (mm)
            if bx.get('sd_min') is not None:
                sel_ &= sdc * 1000.0 >= bx['sd_min']                                  # mean stand-off from the skin >= sd_min mm
            if bx.get('sd_max') is not None:
                sel_ &= sdc * 1000.0 <= bx['sd_max']
            ok |= sel_
        if by_tri:
            return ok
        if sc.get('tri_above'):
            # 2026-10-06 (Iris L123: black shards across the forehead): the LOD chunks of the mesh have big cards whose centroid
            # lies in the box while the card reaches the front; components with more than `tri_above` triangles are selected
            # per triangle instead (the cut edge then lies on the box border)
            ntri_c = np.bincount(lab[idx[:, 0]], minlength=len(ok))
            tri_ok = np.zeros(len(idx), bool)
            for bx in sc.get('boxes', []):
                lo = np.array([-np.inf if v is None else v / 1000.0 for v in bx.get('min', [None] * 3)])
                hi = np.array([np.inf if v is None else v / 1000.0 for v in bx.get('max', [None] * 3)])
                t_ = np.all((cen_t >= lo) & (cen_t <= hi), axis=1)
                for pl_ in ([bx['le']] if bx.get('le') and not isinstance(bx['le'][0], (list, tuple)) else bx.get('le', [])):
                    t_ &= cen_t @ np.asarray(pl_[:3], float) * 1000.0 <= pl_[3]
                if bx.get('sd_min') is not None:
                    t_ &= sd_t * 1000.0 >= bx['sd_min']
                if bx.get('sd_max') is not None:
                    t_ &= sd_t * 1000.0 <= bx['sd_max']
                tri_ok |= t_
            big = ntri_c[lab[idx[:, 0]]] > int(sc['tri_above'])
            res_ = np.where(big, tri_ok, ok[lab[idx[:, 0]]])
            return res_
        from scipy.spatial import cKDTree as _KDb
        used = np.unique(idx)
        kd = _KDb(pos[used])
        for key, val in (('include', True), ('exclude', False)):
            for q in sc.get(key, []):
                d, j = kd.query(np.asarray(q, float))
                if d < 0.006:
                    ok[lab[used[j]]] = val
        return ok[lab[idx[:, 0]]]
    assert mode == 'components', 'split_cards.mode must be "z", "boxes" or "components"'
    nv = ch['nv']
    lab = G.components(nv, ch['idx'])
    zmin = np.full(lab.max() + 1, np.inf)
    np.minimum.at(zmin, lab[np.unique(idx)], pos[np.unique(idx), 2])
    kept_l = zmin > head.top - float(sc.get('nape_rel', 0.15))
    from scipy.spatial import cKDTree as _KDs
    used = np.unique(idx)
    kd = _KDs(pos[used])
    for key, val in (('include', True), ('exclude', False)):
        for q in sc.get(key, []):
            d, j = kd.query(np.asarray(q, float))
            if d < 0.006:
                kept_l[lab[used[j]]] = val
            elif log:
                log('  split_cards: %s point %s matches no card vertex (nearest %.1f mm)' % (key, list(q), d * 1000))
    return kept_l[lab[idx[:, 0]]]


def split_tint_texture(sc, tex, prefix, base, log=print):
    """split_cards.tint (opt-in): the vanilla card diffuse is GREY (the hue is a material tint on the cards), so a
    strand colour read from it stays grey. Writes a tinted copy of the diffuse (same uvs, so each strand still takes
    the card colour under its root, with the atlas' light / dark streaks) as an inline DXT1 xbm: per-channel gain so that
    its alpha-weighted mean = tint.rgb / tint.brightness. -> (xbm path, file bytes, PIL image, brightness)."""
    import headtex
    from texread import load_xbm
    from PIL import Image
    im = np.asarray(load_xbm(tex).convert('RGBA')).astype(np.float64)
    a = im[..., 3] > 0.3 * 255
    mean = im[a][:, :3].mean(0)
    b = float(sc['tint'].get('brightness', 0.5))
    gain = np.asarray(sc['tint']['rgb'], float) / b / mean
    n = int(sc['tint'].get('size', 1024))
    small = np.asarray(Image.fromarray(im.astype(np.uint8), 'RGBA').resize((n, n), Image.LANCZOS)).astype(np.float64)
    rgb = np.clip(small[..., :3] * gain, 0, 255)
    path = prefix + base + '_split_tint.xbm'
    data = headtex.make_xbm(headtex.DXT1_PROXY, n, n, headtex.encode_dxt1(rgb), path, n // 4 * 8)
    full = np.concatenate([np.clip(np.round(rgb), 0, 255), small[..., 3:]], -1).astype(np.uint8)
    log('  split_cards tint: card diffuse mean %s x gain %s -> mean %s (x brightness %.2f = %s), %dx%d DXT1 %d bytes, %s' % (
        mean.round(1).tolist(), gain.round(3).tolist(), (mean * gain).round(1).tolist(), b, (mean * gain * b).round(1).tolist(),
        n, n, len(data), path))
    return path, data, Image.fromarray(full, 'RGBA'), b


def _hwcore():
    p = os.path.join(ROOT, 'core')
    if p not in sys.path:
        sys.path.insert(0, p)
    import hwcore
    return hwcore


def core_unretarget(look, log=print):
    r"""release mode since Core 1.0.0 (script dependency): the fur stays self-contained. Only a texture handle that still
    points at a v1.0.0 Core copy (dlc\hairworkforall\core\textures\...) is mapped back to the game's own texture."""
    K = _hwcore()
    for k, v in list(look.get('textures', {}).items()):
        van = K.legacy_vanilla(v)
        if van:
            look['textures'][k] = van
            log('  core: legacy Core texture %s %s -> %s' % ('.'.join(k) if isinstance(k, tuple) else k, v, van))


def generate(C, hair=None, log=print, prefix=''):
    """-> dict(fur bytes, growth result, head, collision, uvs) for one hair component."""
    import growth, atlas, collision, redfur
    from texread import load_xbm
    growth.FACE_ROOT_DX = (C.get('growth') or {}).get('face_root_dx', 0.07)
    growth.FACE_ROOT_ZEYE = (C.get('growth') or {}).get('face_root_zeye')    # L138 opt-in; default = the old constant
    info = discover(C, log=lambda *a: None)
    hair = hair or info['hair'][0]
    mesh = hair['mesh']
    m = G.load_mesh(mesh)
    ch = G.merge_chunks(m, C.get('lod0_chunks') or m['lod0'])
    head_mesh = C.get('head_mesh') or info['heads'][0]
    head = growth.Head(head_mesh)
    tex = C.get('card_texture') or card_texture(mesh)
    alpha = np.asarray(load_xbm(tex))[..., 3].astype(np.float64) / 255.0
    split_info = None
    if C.get('split_cards'):
        # 2026-10-06 (user: Anna's updo stays on the vanilla cards, HairWorks only for the loose hair below it): the kept
        # (updo) triangles are removed from the card mesh BEFORE the lanes / roots / every later card-based step
        # (fill_holes, hairline_*, crown_volume ...), so no guide is ever grown from, or measured against, the updo
        sc_ = C['split_cards'] if isinstance(C['split_cards'], dict) else {}
        kept_ = split_tri_mask(sc_, ch, head, log=log)
        _t = ch['idx'].reshape(-1, 3)
        split_info = dict(kept=int(kept_.sum()), total=len(_t))
        log('  split_cards (%s): %d of %d card triangles stay on the vanilla cards (updo), %d feed the strands' % (
            sc_.get('mode', 'components'), kept_.sum(), len(_t), (~kept_).sum()))
        ch = dict(ch, idx=_t[~kept_].reshape(-1))
    gathers = []
    for acc in C.get('accessories', []):
        if acc.get('strand_source', True) is False:
            continue        # 2026-10-14 opt-in: kept as a mesh only, the strand generation stays as without it (Studio geo match)
        _am = acc_mask(acc, ch)
        for c, r, msk in growth.gather_points(ch, lambda uv, pos, _am=_am: _am):
            if acc.get('gather', True):
                gathers.append((c, r, msk))
            else:
                C['growth'].setdefault('_extra_drop', []).append(msk)
    res = growth.build(C['growth'], head, ch, alpha, gathers, log=log)
    if C['growth'].get('probe'):
        # 2026-10-05 physics calibration probe: every guide replaced by a STRAIGHT strand of `length` along a fixed
        # direction mix (head normal + `tilt` x outward horizontal), so gravity / stiffness / mass decide the droop
        pr = C['growth']['probe']
        L_, K_ = pr.get('length', 0.12), len(res['S'][0]['p'])
        t_ = np.linspace(0.0, 1.0, K_)[:, None]
        for s_ in res['S']:
            r_ = np.asarray(s_['p'][0], float)
            n_ = head.signed(r_[None, :])[1][0]
            h_ = np.array([r_[0] - head.c[0], r_[1] - head.c[1], 0.0])
            h_ /= max(np.linalg.norm(h_), 1e-9)
            d_ = n_ + pr.get('tilt', 0.0) * h_
            d_ /= max(np.linalg.norm(d_), 1e-9)
            s_['p'] = r_[None, :] + t_ * L_ * d_[None, :]
        log('  probe: %d guides replaced by straight %.0f mm strands' % (len(res['S']), L_ * 1000))
        if pr.get('band'):
            # keep one ring of roots (top - band[0] .. top - band[1]): sparse, each strand readable in side / back views
            b0, b1 = pr['band']
            keep = [i for i, s_ in enumerate(res['S']) if head.top - b1 <= s_['p'][0][2] <= head.top - b0]
            res = growth.reindex(res, keep)
            log('  probe band: %d guides kept' % len(res['S']))
    if C['growth'].get('drop_face_roots'):
        fr = C['growth']['drop_face_roots'] if isinstance(C['growth']['drop_face_roots'], dict) else {}
        bad = growth.face_roots([s_['p'][0] for s_ in res['S']], head, **fr)
        log('  drop_face_roots: %d of %d guides rooted on the face (forehead / brow / eyes) dropped' % (int(bad.sum()), len(bad)))
        if bad.any():
            res = growth.reindex(res, [i for i in range(len(bad)) if not bad[i]])
    body = None
    bm = C.get('body_mesh') or (info['bodies'][0] if info['bodies'] else None)
    if bm:
        body = np.concatenate([c['pos'] for c in G.load_mesh(bm)['chunks']])
    # outfit (armour / collar / pauldron) colliders, per appearance: C['outfits'] = {appearance: [meshes]} overrides
    # the discovered appearance meshes. One fit per outfit; see PIPELINE.md "Outfits".
    outfit = None
    if C['growth'].get('outfit_collision', True):
        app = C.get('appearance') or next(iter(info.get('outfit', {})), '')
        meshes = (C.get('outfits') or {}).get(app) or info.get('outfit', {}).get(app, [])
        if meshes:
            hp = np.concatenate([s_['p'] for s_ in res['S']])
            g_ = C['growth']
            if g_.get('outfit_hair') == 'studio_geo' and C.get('studio_geo'):
                hp = _studio_hair_pts(C)      # 2026-10-14 opt-in: fit the colliders to the hair the Studio geometry really has
            kw = {}
            if g_.get('outfit_old_fit'):          # R3 fit: all outfit vertices are targets, coarser radii
                kw = dict(radii=(0.02, 0.03, 0.045, 0.06), band=0.006, tol=0.003, min_gain=12)
                hp = None
            # 2026-10-14 opt-in growth.outfit_mods: ["modCiri"] = fit on the meshes of these INSTALLED outfit mods (what the
            # player really sees); nothing of them is shipped. Default: the game's own meshes.
            _om = g_.get('outfit_mods') or []
            with G.outfit_source(_om):
                outfit = collision.outfit_shapes(head, meshes, nmax=g_.get('outfit_spheres', 36), log=log, hair_pts=hp,
                                                 reach=g_.get('outfit_reach', 0.06), skip_bad=bool(_om), **kw)
    if C['growth'].get('back_items'):
        # 2026-10-14 opt-in growth.back_items (backitems.py): sword / scabbard capsules fitted from the item meshes in rest pose
        import backitems
        _main = C.get('rig') or next((x for x in info.get('rigs', []) if 'base_entities' in x), None) or info['rigs'][0]
        _bs, _bc = backitems.fit(C['growth']['back_items'], _studio_hair_pts(C) if (C['growth'].get('outfit_hair') == 'studio_geo' and C.get('studio_geo'))
                                 else np.concatenate([s_['p'] for s_ in res['S']]), main_rig=_main, log=log, mods=C['growth'].get('outfit_mods') or ())
        if _bs:
            _o0, _c0 = outfit if outfit else ([], [])
            outfit = (list(_o0) + _bs, list(_c0) + [(i + len(_o0), j + len(_o0)) for i, j in _bc])
    C['_n_outfit_spheres'] = len(outfit[0]) if outfit else 0
    extra = sorted({b for b, _c, _r in outfit[0]}) if outfit else []
    bones = bone_table(C, info, log, extra)
    # R61 (user): flat oval ears (small chained spheres) are the DEFAULT for every character; approved builds made
    # before keep the old fat ear spheres through growth.legacy_ears
    _legacy_ears = C['growth'].get('legacy_ears', True)   # R69 (user): oval ears reverted, the old ear spheres are the default again
    col = collision.fit(head, body, None, bones, res['all_roots'], pad=C['growth']['sphere_pad'], log=log, outfit=outfit,
                        outfit_pad=C['growth'].get('outfit_pad', 0.002), ear_collision=C['growth'].get('ear_collision', True),
                        legacy_neck=C['growth'].get('legacy_neck', False),
                        skull_spheres=C['growth'].get('skull_spheres', 7), face=C['growth'].get('face_sphere'), ear_colliders=C['growth'].get('ear_colliders', True), shell=C['growth'].get('face_shell'),
                        ear_pad=C['growth'].get('ear_pad', 0.004 if _legacy_ears else 0.005),
                        ear_rmax=C['growth'].get('ear_rmax', 0.03 if _legacy_ears else 0.018),
                        ear_chain=C['growth'].get('ear_chain', not _legacy_ears), clamp_skin=C['growth'].get('clamp_skin'),
                        mirror_ears=C['growth'].get('mirror_ears'))
    # strands that lie on the ear flap (temple tufts swept back over it) read as hair growing through the ear: drop
    # guides whose body touches an ear sphere (the cards pass over the ear because they are drawn above it)
    if C['growth'].get('ear_tuck') and col.get('ears'):
        # R52 (Emhyr: hair still clips through the ears with big ear spheres; vanilla slicks it BEHIND the ears).
        # A strand point near an ear (within its sphere + margin) and not already behind it (+y is the front) is moved
        # back to just behind the ear; the offset carries down the rest of the strand (no kink, nothing removed). The
        # hairs interpolated between tucked guides then pass behind the ear too.
        mg = C['growth']['ear_tuck'].get('margin', 0.012) if isinstance(C['growth']['ear_tuck'], dict) else 0.012
        nt = 0
        for s_ in res['S']:
            p = np.asarray(s_['p'], float).copy(); off = np.zeros(3); hit = False
            for k in range(1, len(p)):
                p[k] = p[k] + off
                for c_, r_ in col['ears']:
                    if np.linalg.norm(p[k] - c_) < r_ + mg and p[k][1] > c_[1] - r_ - mg:
                        dy = (c_[1] - r_ - mg) - p[k][1]
                        off = off + np.array([0.0, dy, 0.0]); p[k][1] += dy; hit = True
            if hit:
                s_['p'] = p; nt += 1
        log('  ear_tuck: %d guides tucked behind the ears' % nt)
    if C['growth'].get('drop_over_ear', True) and col.get('ears'):
        def on_ear(p):
            return any(np.linalg.norm(p[2:] - c, axis=1).min() < r + 0.004 for c, r in col['ears'])
        keep = [i for i, s_ in enumerate(res['S']) if not on_ear(s_['p'])]
        if len(keep) < len(res['S']):
            log('  dropped %d guides lying on the ears' % (len(res['S']) - len(keep)))
            res = growth.reindex(res, keep)
    def _trim(p, k, K):
        """guide cut after point k, resampled back to K points (same CV count, shorter strand)."""
        q = np.asarray(p, float)[:k + 1]
        d = np.r_[0, np.cumsum(np.linalg.norm(np.diff(q, axis=0), axis=1))]
        if d[-1] < 1e-6:
            return np.asarray(p, float)
        t = np.linspace(0, d[-1], K)
        return np.stack([np.interp(t, d, q[:, c]) for c in range(3)], 1)
    g_t = C['growth']
    if g_t.get('trim'):
        # R51 (user on Lambert / Eskel: long hair behind the collar, strands sticking out, fringe through the face).
        # Strands are SHORTENED, never removed:
        #  outfit   : cut where the strand reaches the outfit (collar) below the ear line - in vanilla those card ends
        #             are hidden inside the collar, as simulated strands they fall out over it
        #  max_off  : cut where the strand gets farther than max_off from the scalp (outward / horizontal strands)
        #  face     : cut where a strand in front of the face goes below the brow line (z < top - face_z)
        tr = g_t['trim']
        from scipy.spatial import cKDTree as _KD
        kdo = None
        if tr.get('outfit'):
            app_ = C.get('appearance') or next(iter(info.get('outfit', {})), '')
            om = (C.get('outfits') or {}).get(app_) or info.get('outfit', {}).get(app_, [])
            if om:
                with G.outfit_source(g_t.get('outfit_mods')):
                    kdo = _KD(collision.outfit_points(om, skip_bad=bool(g_t.get('outfit_mods')))[0])
        n_cut = {'outfit': 0, 'max_off': 0, 'face': 0, 'face_skin': 0}
        fs = tr.get('face_skin')          # 2026-10-04 (HairLab L10): vanilla hair never lies ON the face skin
        if fs is True:
            fs = {}
        for s_ in res['S']:
            p = np.asarray(s_['p'], float); K = len(p); cut = None; why = None
            sd = head.signed(p)[0]
            for k in range(2, K):
                if fs is not None and fs is not False and sd[k] < fs.get('dist', 0.012) and \
                        p[k][1] > head.c[1] + fs.get('front_y', 0.01) and p[k][2] < head.top - fs.get('below', 0.085) and \
                        abs(p[k][0]) < fs.get('ear_x', 0.085):
                    cut, why = k, 'face_skin'; break   # the strand touches the face (front, below the brow, on the skin)
                if kdo is not None and p[k][2] < head.top - tr.get('outfit_below', 0.18) and \
                        kdo.query(p[k])[0] < tr.get('outfit', 0.01):
                    cut, why = k, 'outfit'; break
                if tr.get('max_off') and sd[k] > tr['max_off']:
                    cut, why = k, 'max_off'; break
                if tr.get('front_len') and p[0][1] > head.c[1] + tr.get('front_y', 0.04) and p[k][2] < head.top - tr['front_len']:
                    cut, why = k, 'face'; break     # R57: guides ROOTED at the front hairline stop at the brow line
                if tr.get('face') and p[k][1] > head.c[1] + 0.03 and abs(p[k][0]) < tr.get('face_x', 0.065) and \
                        p[k][2] < head.top - tr['face']:
                    cut, why = k, 'face'; break
            if cut is not None:
                s_['p'] = _trim(p, max(cut - 1, 1), K); n_cut[why] += 1
        log('  trim: %s strands shortened (of %d)' % (n_cut, len(res['S'])))
        if tr.get('card_env'):
            # R78: strands that leave the vanilla card volume (by > card_env m) are cut there (see growth.card_envelope_trim)
            # trim.card_env_visible (2026-10-04): envelope = the visible (alpha-tested) card surface, see growth
            vis = tr.get('card_env_visible')
            new_, ne = growth.card_envelope_trim([s_['p'] for s_ in res['S']], ch, tr['card_env'],
                                                 alpha_img=alpha if vis else None,
                                                 alpha_vis=vis if isinstance(vis, float) else 0.35)
            for s_, p_ in zip(res['S'], new_):
                s_['p'] = p_
            log('  trim card_env %.0f mm%s: %d of %d strands cut where they leave the vanilla card volume' % (
                tr['card_env'] * 1000, ' (visible cards)' if vis else '', ne, len(res['S'])))
        if tr.get('face_env'):
            # 2026-10-05 (Shani: thin strands over the eye vanilla leaves free): in front of the face (below the brow,
            # in front of the crown centre) a strand is cut where it is farther than `dist` from any VISIBLE card point
            fe = tr['face_env'] if isinstance(tr['face_env'], dict) else {}
            from scipy.spatial import cKDTree as _KD2
            vis_ = growth.visible_card_points(ch, alpha, n=80000, alpha_vis=fe.get('alpha', 0.35), edge_filter=False)
            kdv = _KD2(vis_)
            nfe = 0
            for s_ in res['S']:
                p = np.asarray(s_['p'], float); K = len(p)
                inface = (p[:, 1] > head.c[1] + fe.get('front_y', 0.03)) & (p[:, 2] < head.top - fe.get('below', 0.08)) & \
                         (np.abs(p[:, 0]) < fe.get('face_x', 0.085))
                if not inface[2:].any():
                    continue
                d_ = kdv.query(p)[0]
                bad = np.where(inface[2:] & (d_[2:] > fe.get('dist', 0.01)))[0]
                if len(bad):
                    s_['p'] = _trim(p, max(bad[0] + 1, 1), K); nfe += 1
            log('  trim face_env: %d strands cut where they leave the visible cards in front of the face' % nfe)
        if tr.get('ear'):
            # R78: strands reaching the ear are cut before it (see growth.ear_trim)
            new_, ne = growth.ear_trim([s_['p'] for s_ in res['S']], head, tr['ear'], tr.get('ear_x', 0.083))
            for s_, p_ in zip(res['S'], new_):
                s_['p'] = p_
            log('  trim ear %.0f mm: %d of %d strands cut before the ear' % (tr['ear'] * 1000, ne, len(res['S'])))
            if tr.get('ear_drop_rooted', True):
                bad = growth.ear_rooted([s_['p'][0] for s_ in res['S']], head, tr['ear'], tr.get('ear_x', 0.083))
                log('  trim ear: %d guides rooted on the ear dropped' % int(bad.sum()))
                if bad.any():
                    res = growth.reindex(res, [i for i in range(len(bad)) if not bad[i]])
        if tr.get('hang'):
            # R59: in game HairWorks gravity drops a strand to hang straight down from its root, so the rest pose lies:
            # Eskel's fringe guides end above the brow at rest (face trims cut 2-9 of them) but fall over the eye in
            # game. Any guide passing in front of the face (|x| < hang_x, y > crown centre + 3 cm) is limited to the
            # arc length that, hanging straight down from its root, stops at the brow line (top - hang).
            nh = 0
            brow = head.top - tr['hang']
            for s_ in res['S']:
                p = np.asarray(s_['p'], float)
                front = (p[:, 1] > head.c[1] + 0.03) & (np.abs(p[:, 0]) < tr.get('hang_x', 0.07))
                if not front.any():
                    continue
                allow = p[0][2] - brow
                seg = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
                if seg[-1] > allow > 0.005:
                    k = int(np.searchsorted(seg, allow))
                    s_['p'] = _trim(p, max(k, 1), len(p)); nh += 1
            log('  trim hang: %d front strands limited to their drop-to-brow length' % nh)
    if C['growth'].get('flyaway'):
        # R50 (Lambert / Eskel stray strands): a guide whose TIP lies far from the tips of its root neighbours is a
        # stray (it followed a lone card sticking out); dropped, its neighbours fill the gap
        from scipy.spatial import cKDTree as _KD
        fa = C['growth']['flyaway']
        Rt = np.array([s_['p'][0] for s_ in res['S']]); Tp = np.array([s_['p'][-1] for s_ in res['S']])
        _, nb = _KD(Rt).query(Rt, k=min(fa.get('k', 8) + 1, len(Rt)))
        dev = np.array([np.linalg.norm(Tp[i] - np.median(Tp[nb[i][1:]], axis=0)) for i in range(len(Rt))])
        keep = [i for i in range(len(Rt)) if dev[i] <= fa.get('max_dev', 0.04)]
        log('  flyaway: dropped %d of %d guides (tip > %.0f mm from the neighbours\' median tip)' % (
            len(Rt) - len(keep), len(Rt), fa.get('max_dev', 0.04) * 1000))
        if len(keep) < len(Rt):
            res = growth.reindex(res, keep)
    if C['growth'].get('drop_near_meshes'):
        # R50 (user: Dandelion's front strand pokes through his hat): guides (and so the hairs between them) that
        # pass within `dist` of the given meshes (hat, hood, mask) are dropped; the vanilla cards stay under the hat
        from scipy.spatial import cKDTree as _KD
        dn = C['growth']['drop_near_meshes']
        HP = collision.outfit_points(dn['meshes'])[0]
        kd = _KD(HP)
        dist = dn.get('dist', 0.008)
        if dn.get('mode') == 'push':
            # R51 (user: don't drop the strand, lower it / start it further inside the hat): a guide point that is
            # within `dist` of the hat AND on its outer side is moved to `dist` inside the hat surface; the move is
            # carried down the rest of the strand (the strand is lowered as a whole, no kink)
            HPn = collision.outfit_points(dn['meshes'])[1].copy()
            # R53: orient the hat normals OUTWARD = away from the head centre. Mesh winding gave inward normals on
            # Dandelion's hat, so R52's push moved 165 strands OUT over the hat band instead of under it.
            flip = ((HP - head.c) * HPn).sum(1) < 0
            HPn[flip] *= -1
            log('  hat normals: %d of %d flipped to point away from the head' % (int(flip.sum()), len(HPn)))
            # brim height per azimuth around the head axis: only points ABOVE the brim are 'through the hat'; the
            # layers hanging below the brim are never touched (R50 drop removed them: Dandelion lost his right / back
            # layers, user)
            nb_ = 36
            az = lambda P_: ((np.arctan2(P_[..., 0], P_[..., 1] - head.c[1]) + np.pi) / (2 * np.pi) * nb_).astype(int) % nb_
            ah = az(HP)
            brim = np.full(nb_, np.inf)
            for b_ in range(nb_):
                if (ah == b_).any():
                    brim[b_] = HP[ah == b_, 2].min()
            brim[np.isinf(brim)] = np.min(brim[np.isfinite(brim)])
            above = dn.get('above_brim', 0.012)
            moved = 0
            if dn.get('tuck'):
                # R64 (user's first idea: "make it start more inside the hat"): every strand point at / above the
                # brim bottom (+ margin) is pulled onto the scalp (tuck mm off the skin) so the strand runs UNDER the
                # band and comes out below its lower edge; points below the brim are blended back to their own place
                # over 1.5 cm. Roots stay where they are. The band sits only a few mm off the skin, and strands at the
                # normal 5 mm standoff passed OUTSIDE it (the tuft on the band).
                tk = dn['tuck']; mg = dn.get('tuck_margin', 0.006)
                for s_ in res['S']:
                    p = np.asarray(s_['p'], float).copy(); hit = False
                    sd, nrm, _ = head.signed(p)
                    for k in range(1, len(p)):
                        bz = brim[az(p[k])] + mg
                        w = np.clip((p[k][2] - (bz - 0.015)) / 0.015, 0.0, 1.0)
                        if w > 0 and sd[k] > tk:
                            p[k] = p[k] - nrm[k] * (sd[k] - tk) * w; hit = True
                    if hit:
                        s_['p'] = p; moved += 1
                log('  hat tuck: %d strands pulled onto the scalp under the band' % moved)
            if dn.get('lower'):
                # R56 (user: "lower this hair part"): a point at / above the brim bottom that is outside the hat shell or
                # within `dist` inside it is moved straight DOWN to `lower` below the brim bottom at its azimuth; the
                # offset carries down the strand. Repeated until the check is clean (max 4 passes).
                for _pass in range(4):
                    n_bad = 0
                    for s_ in res['S']:
                        p = np.asarray(s_['p'], float).copy(); off = np.zeros(3); hit = False
                        for k in range(0 if dn.get('roots') else 1, len(p)):
                            p[k] = p[k] + off
                            bz = brim[az(p[k])]
                            if p[k][2] < bz - 0.002:
                                continue
                            if dn.get('front_deg') is not None and abs(np.degrees(np.arctan2(p[k][0], p[k][1] - head.c[1]))) > dn['front_deg']:
                                continue        # R58: only the front window (user: lower that piece, not the whole hairline)
                            d, j = kd.query(p[k])
                            if d < dn.get('near', dist) and float((p[k] - HP[j]) @ HPn[j]) > -dist:
                                dz = (bz - dn['lower']) - p[k][2]
                                off = off + np.array([0.0, 0.0, dz]); p[k][2] += dz; hit = True; n_bad += 1
                        if hit:
                            s_['p'] = p; moved += 1
                    if n_bad == 0:
                        break
            for s_ in ([] if (dn.get('lower') or dn.get('tuck')) else res['S']):
                p = np.asarray(s_['p'], float).copy()
                off = np.zeros(3); hit = False
                for k in range(len(p)):
                    p[k] = p[k] + off
                    if p[k][2] < brim[az(p[k])] + above:
                        continue
                    d, j = kd.query(p[k])
                    side = float((p[k] - HP[j]) @ HPn[j])        # > 0: outside the hat shell (normals point out)
                    if d < dn.get('near', dist) and side > -dist:
                        tgt = p[k] - HPn[j] * (side + dist)
                        off = off + (tgt - p[k]); p[k] = tgt; hit = True
                if hit:
                    s_['p'] = p; moved += 1
            log('  drop_near_meshes push: %d guides moved under %s' % (moved, ', '.join(dn['meshes'])))
            # self-check (R52): strand points still OUTSIDE the hat shell at / above the brim bottom after the push;
            # must be 0, else the hat clipping is still there (no eyes needed)
            left = 0
            for s_ in res['S']:
                for q_ in np.asarray(s_['p'], float)[(0 if dn.get('roots') else 1):]:
                    if q_[2] >= brim[az(q_)] - 0.002:
                        d2, j2 = kd.query(q_)
                        if d2 < 0.03 and float((q_ - HP[j2]) @ HPn[j2]) > 0:
                            left += 1
            log('  CHECK hat clipping: %d strand points outside the hat shell at / above the brim%s' % (
                left, '' if left == 0 else '  <-- NOT FIXED'))
            keep = list(range(len(res['S'])))
        else:
            keep = [i for i, s_ in enumerate(res['S']) if kd.query(np.asarray(s_['p'])[1:])[0].min() > dist]
        log('  drop_near_meshes: dropped %d of %d guides within %.0f mm of %s' % (len(res['S']) - len(keep), len(res['S']),
                                                                             dist * 1000, ', '.join(dn['meshes'])))
        if len(keep) < len(res['S']):
            res = growth.reindex(res, keep)
    if C['growth'].get('procedural_long'):
        # 2026-10-06 (L142, Eredin): replace the lane guides by procedural long straight hair (growth.procedural_long)
        pl = C['growth']['procedural_long'] if isinstance(C['growth']['procedural_long'], dict) else {}
        pl = dict(pl)
        if pl.get('profile'):
            _app = C.get('appearance') or next(iter(info.get('outfit', {})), '')
            _om = (C.get('outfits') or {}).get(_app) or info.get('outfit', {}).get(_app, [])
            if _om:
                pl['outfit_pts'] = collision.outfit_points(_om)[0]
        res, _npl = growth.procedural_long(res, head, ch, alpha, log=log, **pl)
    if C['growth'].get('densify'):
        # 2026-10-06 (user, Olgierd): extra guides over the whole area the vanilla cards cover (growth.densify), pulled
        # toward the card surface; the new guides get the same ear test as the lane guides
        dn_ = C['growth']['densify'] if isinstance(C['growth']['densify'], dict) else {}
        res, _nd = growth.densify(res, head, ch, alpha, spacing=dn_.get('spacing', 0.005), k=dn_.get('k', 4), w_fit=dn_.get('w_fit', 0.6),
                                  w_fit_orig=dn_.get('w_fit_orig', 0.3), ramp=dn_.get('ramp', 0.3), reach=dn_.get('reach', 0.012),
                                  near_skin=dn_.get('near_skin', 0.02), zmin_rel=dn_.get('zmin_rel', 0.20), auto=dn_.get('auto', False),
                                  min_guides_per_cm2=dn_.get('min_guides_per_cm2'), max_new=dn_.get('max_new', 3000), env=dn_.get('env'), max_pull=dn_.get('max_pull', 0.03), log=log)
        if C['growth'].get('drop_over_ear', True) and col.get('ears') and _nd:
            keep = [i for i, s_ in enumerate(res['S']) if not any(np.linalg.norm(np.asarray(s_['p'])[2:] - c, axis=1).min() < r + 0.004 for c, r in col['ears'])]
            if len(keep) < len(res['S']):
                log('  densify: dropped %d guides lying on the ears' % (len(res['S']) - len(keep)))
                res = growth.reindex(res, keep)
    if C['growth'].get('fill_holes'):
        # 2026-10-04 (HairLab, Lambert): new guides where vanilla has hair over the scalp but we have no root
        fh = C['growth']['fill_holes'] if isinstance(C['growth']['fill_holes'], dict) else {}
        res, _nf = growth.fill_holes(res, head, ch, alpha, hole=fh.get('hole', 0.008), need=fh.get('need', 0.012),
                                     zmin=fh.get('zmin', 0.17), k=fh.get('k', 3), max_new=fh.get('max_new', 400),
                                     near_skin=fh.get('near_skin', 0.02), log=log, ear_clear=fh.get('ear_clear', 0.0), guard=fh.get('guard', True), guard_dist=fh.get('guard_dist', 0.01), cover=fh.get('cover', 0.0))
        if C['growth'].get('drop_over_ear', True) and col.get('ears'):
            # the added guides get the same ear test as the original ones (2026-10-05)
            keep = [i for i, s_ in enumerate(res['S']) if not any(np.linalg.norm(np.asarray(s_['p'])[2:] - c, axis=1).min() < r + 0.004 for c, r in col['ears'])]
            if len(keep) < len(res['S']):
                log('  fill_holes: dropped %d added guides lying on the ears' % (len(res['S']) - len(keep)))
                res = growth.reindex(res, keep)
    if C['growth'].get('drop_bare_roots'):
        # 2026-10-05: drop guides rooted on bare forehead skin (no visible vanilla hair over the root)
        br = C['growth']['drop_bare_roots'] if isinstance(C['growth']['drop_bare_roots'], dict) else {}
        bad = growth.bare_roots([np.asarray(s_['p'])[0] for s_ in res['S']], head, ch, alpha, dist=br.get('dist', 0.008),
                                near_skin=br.get('near_skin', 0.015), front_y=br.get('front_y', 0.0), below=br.get('below', 0.0))
        log('  drop_bare_roots: %d of %d guides rooted on bare skin dropped' % (int(bad.sum()), len(bad)))
        if bad.any():
            res = growth.reindex(res, [i for i in range(len(bad)) if not bad[i]])
    if C['growth'].get('crown_volume'):
        # 2026-10-04 (HairLab, Shani "spike hill"): the upper sides lay 9 mm off the scalp where the cards stand 18-21 mm,
        # so the crown read as a peak instead of a dome. Push upper-head strand points out along the head normal toward
        # the LOCAL standoff of the visible cards (same direction from the crown centre), root fixed, ramped to the tip.
        cv = C['growth']['crown_volume'] if isinstance(C['growth']['crown_volume'], dict) else {}
        new_, nm = growth.crown_volume([s_['p'] for s_ in res['S']], head, ch, alpha, zrel=cv.get('zrel', 0.09),
                                       k=cv.get('k', 0.8), max_move=cv.get('max', 0.015), cone=cv.get('cone', 0.12), ramp=cv.get('ramp', 0.4), fade=cv.get('fade', 0.0), front_y=cv.get('front_y'), carry=cv.get('carry', 0.0), smooth=cv.get('smooth', 0), both=cv.get('both', False), min_sd=cv.get('min_sd', 0.002))
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
        log('  crown_volume: %d guides moved out toward the card standoff (k %.2f, max %.0f mm)' % (nm, cv.get('k', 0.8), cv.get('max', 0.015) * 1000))
    if C['growth'].get('hairline_follow'):
        hf = C['growth']['hairline_follow'] if isinstance(C['growth']['hairline_follow'], dict) else {}
        growth.hairline_follow(res, head, k=hf.get('k', 5), zone_x=hf.get('zone_x', 0.035), log=log)
    if C['growth'].get('cover_fill'):
        # 2026-10-05: copies of nearby guides shifted through the vanilla card hair we do not cover (front)
        cf = C['growth']['cover_fill'] if isinstance(C['growth']['cover_fill'], dict) else {}
        res, _nc = growth.cover_fill(res, head, ch, alpha, gap=cf.get('gap', 0.004), spacing=cf.get('spacing', 0.006),
                                     reach=cf.get('reach', 0.03), max_shift=cf.get('max_shift', 0.02), front_y=cf.get('front_y', 0.02),
                                     below=cf.get('below', 0.03), max_new=cf.get('max_new', 300), passes=cf.get('passes', 2), log=log)
    if C['growth'].get('hairline_row'):
        # 2026-10-05: crisp hairline along the vanilla hairline curve (growth.hairline_row)
        hr = C['growth']['hairline_row'] if isinstance(C['growth']['hairline_row'], dict) else {}
        res, _hd, _ha = growth.hairline_row(res, head, ch, alpha, phi_max=hr.get('phi_max', 110.0), tol_deg=hr.get('tol_deg', 1.5),
                                            inset_deg=hr.get('inset_deg', 1.0), spacing=hr.get('spacing', 0.004), rows=hr.get('rows', 2),
                                            row_step_deg=hr.get('row_step_deg', 1.5), log=log)
    if C['growth'].get('face_drop'):
        # 2026-10-05 (user: Triss "remove the strand in front of her face"; cutting left a blunt bang): guides with a point
        # in front of the face (y > centre + front_y, |x| < face_x, z < top - below) are DROPPED, not shortened
        fd = C['growth']['face_drop'] if isinstance(C['growth']['face_drop'], dict) else {}
        if fd.get('roots_only'):
            # only guides ROOTED low on the forehead (the hairline fringe); the sweep from higher up stays and covers
            bad = [i for i, s_ in enumerate(res['S']) if (lambda r: r[1] > head.c[1] + fd.get('front_y', 0.03) and
                   abs(r[0]) < fd.get('face_x', 0.06) and r[2] < head.top - fd.get('below', 0.04))(np.asarray(s_['p'], float)[0])]
        else:
            bad = None
        if bad is None:
            bad = [i for i, s_ in enumerate(res['S']) if any(
            q[1] > head.c[1] + fd.get('front_y', 0.04) and abs(q[0]) < fd.get('face_x', 0.05) and q[2] < head.top - fd.get('below', 0.09)
            for q in np.asarray(s_['p'], float)[2:])]
        log('  face_drop: %d guides passing in front of the face dropped' % len(bad))
        if bad:
            res = growth.reindex(res, [i for i in range(len(res['S'])) if i not in set(bad)])
    if C['growth'].get('under_layer'):
        ul = C['growth']['under_layer'] if isinstance(C['growth']['under_layer'], dict) else {}
        res, _nu = growth.under_layer(res, head, front_y=ul.get('front_y', 0.0), zrel=ul.get('zrel', 0.12), squash=ul.get('squash', 0.35),
                                      min_sd=ul.get('min_sd', 0.002), min_gap=ul.get('min_gap', 0.003), max_new=ul.get('max_new', 400), log=log)
    if C['growth'].get('ear_zone'):
        # 2026-10-05: sideburn length in front of the ear + sparse strands behind it (growth.ear_zone); after
        # hairline_row so its rows near the ear get cut too
        ez = C['growth']['ear_zone'] if isinstance(C['growth']['ear_zone'], dict) else {}
        res, _ec, _ed = growth.ear_zone(res, head, front_cut=ez.get('front_cut', 0.02), front_reach=ez.get('front_reach', 0.045),
                                        behind_drop=ez.get('behind_drop', 0.03), behind_reach=ez.get('behind_reach', 0.035), log=log)
    if C['growth'].get('smooth_strands'):
        # 2026-10-05: angular guide polylines -> smooth curves (growth.smooth_strands), before the head push-out
        ss = C['growth']['smooth_strands'] if isinstance(C['growth']['smooth_strands'], dict) else {}
        new_, _t0, _t1 = growth.smooth_strands([s_['p'] for s_ in res['S']], iters=ss.get('iters', 6), alpha=ss.get('alpha', 0.5),
                                               keep=ss.get('keep', 2), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('curl'):
        # 2026-10-06 (Guillaume: vanilla ringlets; HairWorks waves only wiggle): helical guides on the lower part
        cu = C['growth']['curl'] if isinstance(C['growth']['curl'], dict) else {}
        new_ = growth.curl_strands([s_['p'] for s_ in res['S']], radius=cu.get('radius', 0.008), pitch=cu.get('pitch', 0.03),
                                   start=cu.get('start', 0.35), ramp=cu.get('ramp', 0.2), seed=cu.get('seed', 3),
                                   z_max=(head.top - cu['below_top']) if cu.get('below_top') is not None else None, z_fade=cu.get('z_fade', 0.03),
                                   low_z=(head.top - cu['low_below_top']) if cu.get('low_below_top') is not None else None,
                                   radius2=cu.get('radius2'), pitch2=cu.get('pitch2'), low_fade=cu.get('low_fade', 0.04),
                                   back_y=(head.c[1] + cu['back_y']) if cu.get('back_y') is not None else None, coherent=cu.get('coherent'), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('roll_ends'):
        re_ = C['growth']['roll_ends'] if isinstance(C['growth']['roll_ends'], dict) else {}
        new_ = growth.roll_ends([s_['p'] for s_ in res['S']], head, start=re_.get('start', 0.55), radius=re_.get('radius', 0.02),
                                turns=re_.get('turns', 1.2), shrink=re_.get('shrink', 0.55), low_below_top=re_.get('low_below_top', 0.0),
                                back_y=re_.get('back_y'), side=re_.get('side', -1.0), min_len=re_.get('min_len', 0.04),
                                band=re_.get('band', False), start_below_top=re_.get('start_below_top'), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('inflate'):
        # 2026-10-06 (user: Lambert ~20 % more volume): standoff from the scalp scaled + offset (growth.inflate)
        fl = C['growth']['inflate'] if isinstance(C['growth']['inflate'], dict) else {}
        new_ = growth.inflate([s_['p'] for s_ in res['S']], head, k=fl.get('k', 1.2), add=fl.get('add', 0.003),
                              zrel=fl.get('zrel', 0.16), ramp=fl.get('ramp', 0.3), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    collision.push_out(res['S'], col)
    if C['growth'].get('extra_guides'):
        # 2026-10-05 (build/sim_fill.py): copies of guides chosen in the SIMULATED pose (preview physics) where the
        # settled hair leaves vanilla card hair uncovered. Each entry {src: root xyz of the source guide, d: offset};
        # the source is matched by root position, the copy keeps its shape, root snapped back onto the scalp.
        res, _ne = growth.add_guide_copies(res, head, C['growth']['extra_guides'], log=log)
    if C['growth'].get('split_divergent'):
        sd_ = C['growth']['split_divergent'] if isinstance(C['growth']['split_divergent'], dict) else {}
        if sd_.get('fill'):
            # 2026-10-05: fill the dropped faces with same-side guide copies (bald strip at the parting), see growth
            fl = sd_['fill'] if isinstance(sd_['fill'], dict) else {}
            res, _na = growth.split_fill(res, head, ratio=sd_.get('ratio', 4.0), front_only=sd_.get('front_only', True),
                                         frac=fl.get('frac', 0.4), iters=fl.get('iters', 2), min_gap=fl.get('min_gap', 0.003), log=log)
        F2, nd = growth.split_divergent([s_['p'] for s_ in res['S']], res['F'], head, ratio=sd_.get('ratio', 4.0),
                                        front_only=sd_.get('front_only', True))
        res = dict(res)
        res['F'] = F2
        log('  split_divergent: %d growth faces between diverging guides dropped (ratio %.1f)' % (nd, sd_.get('ratio', 4.0)))
        if sd_.get('skin_blend'):
            # 2026-10-06 (L126, opt-in): second pass for divergent faces just outside the front zone whose blended hairs run over the forehead
            sb_ = sd_['skin_blend'] if isinstance(sd_['skin_blend'], dict) else {}
            _sbk = dict(min_ratio=sb_.get('min_ratio', sd_.get('ratio', 4.0)), dist=sb_.get('dist', 0.02), gap=sb_.get('gap', 0.02),
                        front_y=sb_.get('front_y', -0.03), below=sb_.get('below', 0.03), face_x=sb_.get('face_x', 0.1), n=sb_.get('n', 5))
            if sb_.get('fill'):
                # fill instead of leaving the forehead bare: each dropped face gets same-side guide copies (growth.split_fill), then the final drop
                fl_ = sb_['fill'] if isinstance(sb_['fill'], dict) else {}
                res, _nf = growth.split_fill(res, head, frac=fl_.get('frac', 0.4), iters=fl_.get('iters', 2), min_gap=fl_.get('min_gap', 0.003), log=log,
                                             dropper=lambda G_, F_: growth.split_skin_blend(G_, F_, head, log=lambda *a_: None, **_sbk))
                res = dict(res)           # reindex rebuilt every face: drop the divergent ones again
                res['F'], _nd2 = growth.split_divergent([s_['p'] for s_ in res['S']], res['F'], head, ratio=sd_.get('ratio', 4.0),
                                                        front_only=sd_.get('front_only', True))
            if sb_.get('fringe_lane'):
                # 2026-10-06 (L126b, opt-in): give the fan between divergent guides real intermediate guides before the final drop
                fr_ = sb_['fringe_lane'] if isinstance(sb_['fringe_lane'], dict) else {}
                res, _nfl = growth.fringe_lane(res, head, spacing=fr_.get('spacing', 0.025), max_per_edge=fr_.get('max_per_edge', 8),
                                               min_gap=fr_.get('min_gap', 0.0015), margin=fr_.get('margin'), x_min=fr_.get('x_min'), log=log, **_sbk)
                res = dict(res)
                res['F'], _nd3 = growth.split_divergent([s_['p'] for s_ in res['S']], res['F'], head, ratio=sd_.get('ratio', 4.0),
                                                        front_only=sd_.get('front_only', True))
            F3, _nsb = growth.split_skin_blend([s_['p'] for s_ in res['S']], res['F'], head, min_ratio=sb_.get('min_ratio', sd_.get('ratio', 4.0)),
                                               dist=sb_.get('dist', 0.02), gap=sb_.get('gap', 0.02), front_y=sb_.get('front_y', -0.03),
                                               below=sb_.get('below', 0.03), face_x=sb_.get('face_x', 0.1), n=sb_.get('n', 5), log=log)
            if res.get('lane_F') is not None and len(res['lane_F']):
                F3 = np.concatenate([np.asarray(F3, int).reshape(-1, 3), np.asarray(res['lane_F'], int)])   # fringe_lane faces are kept
            res = dict(res)
            res['F'] = F3
    if C['growth'].get('drop_faces'):
        # 2026-10-06 (L156, opt-in): drop the growth faces listed as guide-index triples (Groom Studio pick-hair); guides stay
        _df = {tuple(sorted(int(i) for i in t_)) for t_ in C['growth']['drop_faces']}
        _F = np.asarray(res['F'], int)
        _k = np.array([tuple(sorted(f_.tolist())) not in _df for f_ in _F], bool)
        res = dict(res)
        res['F_push'] = _F              # interp_push keeps working on the ORIGINAL faces: no other guide moves
        res['F'] = _F[_k]
        log('  drop_faces: %d of %d listed growth faces dropped' % (int((~_k).sum()), len(_df)))
    if C['growth'].get('skin_faces'):
        # 2026-10-05 (user, Triss: thin wisps lying on the forehead under the parting): a rendered hair is the blend of
        # its growth face's 3 guides; drop faces whose centre hair (mean of the 3 guides) lies on the face skin
        # (within dist) in front of the face below top - below
        sf = C['growth']['skin_faces'] if isinstance(C['growth']['skin_faces'], dict) else {}
        G_ = np.array([np.asarray(s_['p'], float) for s_ in res['S']])
        F_ = np.asarray(res['F'], int)
        keep_f = []
        for f in F_:
            q = G_[f].mean(0)[2:]
            zone = (q[:, 1] > head.c[1] + sf.get('front_y', 0.02)) & (q[:, 2] < head.top - sf.get('below', 0.04)) & \
                   (np.abs(q[:, 0]) < sf.get('face_x', 0.06))
            keep_f.append(not (zone.any() and (head.signed(q[zone])[0] < sf.get('dist', 0.01)).any()))
        keep_f = np.array(keep_f, bool)
        res = dict(res)
        res['F'] = F_[keep_f]
        log('  skin_faces: %d growth faces whose blended hair lies on the forehead dropped' % int((~keep_f).sum()))
    if C['growth'].get('head_push'):
        # 2026-10-05: growth.head_push was set in configs since R84 (Keira k54 "head_push 5 mm") but NEVER called:
        # guide points closer than margin to the head MESH are moved out (exact head shape, not spheres)
        hp_ = C['growth']['head_push']
        new_, nh = growth.head_push([s_['p'] for s_ in res['S']], head, margin=hp_ if isinstance(hp_, float) else 0.003)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
        log('  head_push: %d guides moved out of the head mesh' % nh)
    if C['growth'].get('interp_push'):
        # R81: keep the RENDERED hairs (blends of 3 guides) out of the head, not only the guides (growth.interp_push)
        ip = C['growth']['interp_push'] if isinstance(C['growth']['interp_push'], dict) else {}
        new_, _st = growth.interp_push([s_['p'] for s_ in res['S']], res.get('F_push', res['F']), head, margin=ip.get('margin', 0.0015),
                                       max_move=ip.get('max_move', 0.012), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    # R59 strand editor: hand edits (tools\hairworks\edits\<char>.json, matched by root position) on the final
    # guides, then the editor export (build\hairbatch\edit\<cfg>.json) shows the edited guides. strand_edits.py.
    import strand_edits
    res, _edits, _base = strand_edits.apply_to_growth(C, res, col, log=log, head=head)
    if C['growth'].get('densify_after_edits'):
        # 2026-10-06 (L142, Olgierd: the user's hand deletions + the cards' crown / top-back left bare patches): the same
        # densify AFTER the hand edits (keys as growth.densify; use auto / min_guides_per_cm2 so only the sparse parts are
        # refilled). The edits are already applied, so they are kept.
        dn2 = C['growth']['densify_after_edits'] if isinstance(C['growth']['densify_after_edits'], dict) else {}
        res, _nd2 = growth.densify(res, head, ch, alpha, spacing=dn2.get('spacing', 0.005), k=dn2.get('k', 4), w_fit=dn2.get('w_fit', 0.6),
                                   w_fit_orig=dn2.get('w_fit_orig', 0.3), ramp=dn2.get('ramp', 0.3), reach=dn2.get('reach', 0.012),
                                   near_skin=dn2.get('near_skin', 0.02), zmin_rel=dn2.get('zmin_rel', 0.20), auto=dn2.get('auto', False),
                                   min_guides_per_cm2=dn2.get('min_guides_per_cm2'), max_new=dn2.get('max_new', 3000), env=dn2.get('env'), max_pull=dn2.get('max_pull', 0.03), y_max=dn2.get('y_max'), log=log)
    if C['growth'].get('swirl'):
        sw_ = C['growth']['swirl'] if isinstance(C['growth']['swirl'], dict) else {}
        new_ = growth.swirl([s_['p'] for s_ in res['S']], head, cx=sw_.get('cx', 0.0), cy=sw_.get('cy', -0.04), angle=sw_.get('angle', 35.0),
                            radius=sw_.get('radius', 0.09), z_min_rel=sw_.get('z_min_rel', 0.14), hug=sw_.get('hug', 0.004), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('bun_roll'):
        br_ = C['growth']['bun_roll'] if isinstance(C['growth']['bun_roll'], dict) else {}
        new_ = growth.bun_roll([s_['p'] for s_ in res['S']], head, cy=br_.get('cy', -0.115), cz=br_.get('cz', -0.035), radius=br_.get('radius', 0.028),
                               x_half=br_.get('x_half', 0.07), turns=br_.get('turns', 1.0), z_min_rel=br_.get('z_min_rel', 0.2),
                               keep_every=br_.get('keep_every', 25), entry_deg=br_.get('entry_deg', 75.0), hug=br_.get('hug', 0.005), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('lengthen_front'):
        lf_ = C['growth']['lengthen_front'] if isinstance(C['growth']['lengthen_front'], dict) else {}
        new_ = growth.lengthen_front([s_['p'] for s_ in res['S']], head, y_min=lf_.get('y_min', 0.0), z_min_rel=lf_.get('z_min_rel', 0.2),
                                     factor=lf_.get('factor', 1.5), hug=lf_.get('hug', 0.004), x_min=lf_.get('x_min'), x_max=lf_.get('x_max'),
                                     factor_other=lf_.get('factor_other'), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('sweep_front'):
        # 2026-10-06 (Oriana): front guides rotated about the head axis, away from the parting (growth.sweep_front)
        sw = C['growth']['sweep_front'] if isinstance(C['growth']['sweep_front'], dict) else {}
        new_ = growth.sweep_front([s_['p'] for s_ in res['S']], head, y_min=sw.get('y_min', 0.01), tip_y_min=sw.get('tip_y_min', 0.0),
                                  start=sw.get('start', 0.2), angle_deg=sw.get('angle', 55.0), z_min_rel=sw.get('z_min_rel', 0.14),
                                  centre=sw.get('centre', 0.012), hug=sw.get('hug', 0.005), hug_max=sw.get('hug_max', 0.03), force=sw.get('force', 0), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('thin_front'):
        # 2026-10-06 (Priscilla: a thick sheet falls over the right eye / cheek): keep only `keep` of the guides whose TIP lies in
        # the box in front of the face (tip y > c.y + y_min, tip z < c.z + z_max, optional x_min / x_max rel. head centre)
        tf = C['growth']['thin_front'] if isinstance(C['growth']['thin_front'], dict) else {}
        rng_t = np.random.default_rng(tf.get('seed', 5))
        keep_ = []
        for i_, s_ in enumerate(res['S']):
            q_ = np.asarray(s_['p'], float) - np.array(head.c, float)
            q_ = q_ if tf.get('any_point') else q_[-1:]        # any_point: ANY guide point in the box, else the tip only
            hit_ = bool(((q_[:, 1] > tf.get('y_min', 0.03)) & (q_[:, 2] < tf.get('z_max', 0.0)) &
                         (q_[:, 0] >= tf.get('x_min', -9)) & (q_[:, 0] <= tf.get('x_max', 9))).any())
            if not hit_ or rng_t.random() < tf.get('keep', 0.4):
                keep_.append(i_)
        log('  thin_front: %d of %d guides kept' % (len(keep_), len(res['S'])))
        if len(keep_) < len(res['S']):
            res = growth.reindex(res, keep_)
    if C['growth'].get('drop_cheek_roots'):
        # 2026-10-06 (user, Priscilla: "hairs coming literally from her cheek"): guides rooted on the face side below the eye line
        # (root z < c.z + z_max, y > c.y + y_min, |x| > x_min, all rel. head centre) are dropped
        dc_ = C['growth']['drop_cheek_roots'] if isinstance(C['growth']['drop_cheek_roots'], dict) else {}
        c_ = np.array(head.c, float)
        keep_ = []
        for i_, s_ in enumerate(res['S']):
            r_ = np.asarray(s_['p'])[0] - c_
            if r_[2] < dc_.get('z_max', 0.02) and r_[1] > dc_.get('y_min', -0.015) and abs(r_[0]) > dc_.get('x_min', 0.03):
                continue
            keep_.append(i_)
        log('  drop_cheek_roots: %d of %d guides rooted on the cheek dropped' % (len(res['S']) - len(keep_), len(res['S'])))
        if len(keep_) < len(res['S']):
            res = growth.reindex(res, keep_)
    if C['growth'].get('cap_front'):
        cf_ = C['growth']['cap_front'] if isinstance(C['growth']['cap_front'], dict) else {}
        if cf_.get('drop_roots'):
            # 2026-10-06 (Oriana L136 pick evidence: 112 guides are ROOTED on the forehead below the band line (root z 11-75 mm, y 0-29 mm,
            # |x|<90) and cap_front leaves guides rooted below the line alone: they ARE the blunt fringe seen in game): dropped
            c_ = np.array(head.c, float)
            keep_ = [i_ for i_, s_ in enumerate(res['S']) if not (
                np.asarray(s_['p'])[0][1] > c_[1] + cf_.get('root_y', 0.0) and np.asarray(s_['p'])[0][2] < c_[2] + cf_['drop_roots']
                and abs(np.asarray(s_['p'])[0][0] - c_[0]) < cf_.get('root_x', 0.09))]
            log('  cap_front drop_roots: %d forehead-rooted guides below z +%.0f mm dropped' % (len(res['S']) - len(keep_), cf_['drop_roots'] * 1000))
            if len(keep_) < len(res['S']):
                res = growth.reindex(res, keep_)
        new_ = growth.cap_front([s_['p'] for s_ in res['S']], head, y_min=cf_.get('y_min', 0.05), z_max=cf_.get('z_max', 0.06), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('extend_across'):
        ea = C['growth']['extend_across'] if isinstance(C['growth']['extend_across'], dict) else {}
        new_ = growth.extend_across([s_['p'] for s_ in res['S']], head, z_rel=ea.get('z_rel', 0.07), target_x=ea.get('target_x', 0.05),
                                    side=ea.get('side', 1), max_add=ea.get('max_add', 0.05), hug=ea.get('hug', 0.006), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('droop_sides'):
        # 2026-10-06 (user, Olgierd: "lower the ends of the hairs that still reach his side"): ends of guides that end out
        # on the side of the head are bent down and in, along the scalp (applied after the hand edits)
        ds = C['growth']['droop_sides'] if isinstance(C['growth']['droop_sides'], dict) else {}
        new_ = growth.droop_sides([s_['p'] for s_ in res['S']], head, side_x=ds.get('side_x', 0.045), drop=ds.get('drop', 0.025),
                                  start=ds.get('start', 0.45), hug=ds.get('hug', 0.004), only_side=ds.get('only_side'), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('ear_final'):
        # 2026-10-06 (L142, Crach / Dettlaff: hairs over the ears): AFTER every guide-adding step, strands reaching the ear
        # are cut before it and guides rooted on it dropped (growth.ear_trim / ear_rooted, as trim.ear but late)
        ef = C['growth']['ear_final'] if isinstance(C['growth']['ear_final'], dict) else {}
        d_ = ef.get('dist', 0.012)
        new_, ne = growth.ear_trim([s_['p'] for s_ in res['S']], head, d_, ef.get('ear_x', 0.083))
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
        bad = growth.ear_rooted([s_['p'][0] for s_ in res['S']], head, d_, ef.get('ear_x', 0.083))
        log('  ear_final: %d strands cut before the ear, %d guides rooted on it dropped' % (ne, int(bad.sum())))
        if bad.any():
            res = growth.reindex(res, [i for i in range(len(bad)) if not bad[i]])
    if C['growth'].get('drop_zone'):
        # 2026-10-06 (L142, Crach sideburns): guides rooted beside the face (|x| > x_min, in front of c.y + y_min, below
        # top - z_below) are dropped
        dz_ = C['growth']['drop_zone'] if isinstance(C['growth']['drop_zone'], dict) else {}
        R_ = np.array([np.asarray(s_['p'][0], float) for s_ in res['S']])
        bad = (np.abs(R_[:, 0]) > dz_.get('x_min', 0.05)) & (R_[:, 1] > head.c[1] + dz_.get('y_min', -0.02)) & (R_[:, 2] < head.top - dz_.get('z_below', 0.05))
        log('  drop_zone: %d of %d guides dropped (beside the face)' % (int(bad.sum()), len(bad)))
        if bad.any():
            res = growth.reindex(res, [i for i in range(len(bad)) if not bad[i]])
    if C['growth'].get('behind_face'):
        bf = C['growth']['behind_face'] if isinstance(C['growth']['behind_face'], dict) else {}
        new_ = growth.behind_face([s_['p'] for s_ in res['S']], head, y_max=bf.get('y_max', 0.02), z_below=bf.get('z_below', 0.06),
                                  squash=bf.get('squash', 0.1), keep_every=bf.get('keep_every', 12), hug=bf.get('hug', 0.004), flatten=bf.get('flatten', 0.0), x_min=bf.get('x_min', 0.0), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('straight_locks'):
        sl = C['growth']['straight_locks'] if isinstance(C['growth']['straight_locks'], dict) else {}
        new_ = growth.straight_locks([s_['p'] for s_ in res['S']], head, x_min=sl.get('x_min', 0.05), y_min=sl.get('y_min', -0.005),
                                     root_z_below=sl.get('root_z_below', 0.04), length=sl.get('length', 0.1), every=sl.get('every', 3),
                                     short=sl.get('short', 0.03), hug=sl.get('hug', 0.004), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('side_cut'):
        sc_ = C['growth']['side_cut'] if isinstance(C['growth']['side_cut'], dict) else {}
        new_ = growth.side_cut([s_['p'] for s_ in res['S']], head, depth=sc_.get('depth', 0.14), x_min=sc_.get('x_min', 0.05),
                               y_min=sc_.get('y_min', -0.01), log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    if C['growth'].get('face_filter'):
        # 2026-10-06 opt-in: drop long / thin / bare-skin growth faces (growth.face_filter); guides stay, as split_divergent
        res = growth.face_filter(res, head, ch, alpha, C['growth']['face_filter'], log=log)
    if C['growth'].get('drop_tangled'):
        import gapfill
        res = gapfill.drop_tangled(res, head, C['growth']['drop_tangled'] if isinstance(C['growth']['drop_tangled'], dict) else {}, log=log)
    if C['growth'].get('drop_long'):
        import gapfill
        res = gapfill.drop_long(res, head, C['growth']['drop_long'] if isinstance(C['growth']['drop_long'], dict) else {}, log=log)
    if C['growth'].get('gap_fill'):
        # 2026-10-06 opt-in (Emhyr L145): extra guides over skin the vanilla cards cover but no strand reaches (gapfill.py)
        import gapfill
        res = gapfill.fill(res, head, ch['pos'], C['growth']['gap_fill'] if isinstance(C['growth']['gap_fill'], dict) else {}, log=log)
    _edit_path, _idx = None, [h['comp'] for h in info['hair']].index(hair['comp']) if hair in info['hair'] else 0
    try:
        _edit_path = strand_edits.export_json(C, res, head, info, comp=hair['comp'], edits=_edits, base=_base, log=log,
                                              index=_idx)
    except Exception as ex:             # the editor export must never break a build
        log('  edit export FAILED: %r' % (ex,))
    rng = np.random.default_rng(11)
    lib = atlas.PatchLibrary(tex, exclude=[tuple(b) for b in C.get('atlas_exclude', [])] +
                             [tuple(a['uv_box']) for a in C.get('accessories', []) if a.get('uv_box')])
    inner = res.get('part_verts', []) if C['growth'].get('part_tail_dense') else ()
    target, ring = coverage_targets(res['roots'], res['F'], C['growth']['coverage'], rng, inner)
    if C['look']['use_card_texture']:
        uvs, cen = atlas.face_uvs(res['roots'], res['F'], res['dirs'], target, lib, tuple(C.get('uv_per_m', (2.0, 4.0))))
    else:
        uvs = np.tile([[0.5, 0.5], [0.5, 0.5], [0.5, 0.5]], (len(res['F']), 1))
    if C.get('density_floor') and (C['look']['use_card_texture'] or C['look'].get('card_density_only')):
        # 2026-10-05 (PIX + L64: see-through slits beside the jaw = HairWorks grows NO hairs where the density texture,
        # the card alpha, is near 0 at the faces' root UVs; usePixelDensity off closed them). Keep the texture, but a
        # growth face whose corner / centre UVs sample alpha below the floor gets all 3 UVs moved onto the nearest texel
        # with alpha >= floor (uniform density over that face, never 0).
        from scipy.spatial import cKDTree as _KDu
        fl = float(C['density_floor'])
        h_, w_ = alpha.shape
        step = max(1, min(h_, w_) // 256)
        yy, xx = np.mgrid[0:h_:step, 0:w_:step]
        dense = alpha[yy, xx] >= fl
        duv = np.c_[(xx[dense] + 0.5) / w_, (yy[dense] + 0.5) / h_]
        if len(duv):
            kdu = _KDu(duv)
            U = np.asarray(uvs, float).reshape(-1, 3, 2)

            def samp(q):
                return alpha[np.clip(((q[..., 1] % 1) * h_).astype(int), 0, h_ - 1), np.clip(((q[..., 0] % 1) * w_).astype(int), 0, w_ - 1)]
            cen_uv = U.mean(1)
            low = (samp(U).min(1) < fl) | (samp(cen_uv) < fl)
            if low.any():
                _d, j = kdu.query(cen_uv[low] % 1)
                U[low] = duv[j][:, None, :]
            uvs = U.reshape(-1, 2)
            log('  density_floor %.2f: %d of %d growth faces moved onto texels with alpha >= floor' % (fl, int(low.sum()), len(U)))
    if C['growth'].get('hairline_fade') and (C['look']['use_card_texture'] or C['look'].get('card_density_only')):
        # 2026-10-06 (user: hairline still "spiky roots" with PT root alpha mode 0; thin roots / cards / width 0 not
        # enough): a real (and the vanilla card) hairline THINS OUT toward the front instead of a full-density row of
        # opaque roots. HairWorks samples the density texture (card alpha) at every hair's interpolated root UV, so each
        # growth-face CORNER near the vanilla hairline curve gets a UV on a nearby texel whose alpha = target:
        # lo at the line (elevation - line <= 0) rising linearly to 1 at band_deg behind it. -> density gradient.
        import growth as _gr
        from scipy.spatial import cKDTree as _KDf
        hf_ = C['growth']['hairline_fade'] if isinstance(C['growth']['hairline_fade'], dict) else {}
        _pm = hf_.get('phi_max', 110.0)
        _bins, _line, _p, _P, _ph, _el = _gr.hairline_curve(head, ch, alpha, phi_max=_pm)
        _ok = ~np.isnan(_line)
        if _ok.sum() < 2:
            # 2026-10-06 (Guillaume L76 crashed: "array of sample points is empty"): no vanilla hairline curve -> no fade
            log('  hairline_fade: no vanilla hairline curve for this hair, skipped')
            _ok = None
    if C['growth'].get('hairline_fade') and (C['look']['use_card_texture'] or C['look'].get('card_density_only')) \
            and _ok is not None:
        R_ = np.asarray(res['roots'], float)
        d_ = R_ - np.array(head.c, float)
        rphi = np.degrees(np.arctan2(d_[:, 0], d_[:, 1]))
        rel = np.degrees(np.arctan2(d_[:, 2], np.hypot(d_[:, 0], d_[:, 1])))
        sdeg = rel - np.interp(rphi, _bins[_ok], _line[_ok])
        band, lo = hf_.get('band_deg', hf_.get('band', 5.0)), hf_.get('lo', 0.1)     # 'band' alias (2026-10-06: L76-L83 plans used it and silently got 5 deg)
        tgt = lo + (1 - lo) * np.clip(sdeg / band, 0, 1)
        zone = (np.abs(rphi) < _pm) & (sdeg < band)
        h_, w_ = alpha.shape
        step = max(1, min(h_, w_) // 512)
        yy, xx = np.mgrid[0:h_:step, 0:w_:step]
        a_ = alpha[yy, xx].ravel()
        use = a_ > 0.02
        W = hf_.get('alpha_weight', 0.5)            # UV distance traded against alpha mismatch
        kd = _KDf(np.c_[(xx.ravel()[use] + 0.5) / w_, (yy.ravel()[use] + 0.5) / h_, a_[use] * W])
        tuv = np.c_[(xx.ravel()[use] + 0.5) / w_, (yy.ravel()[use] + 0.5) / h_]
        U = np.asarray(uvs, float).reshape(-1, 3, 2).copy()
        Fa = np.asarray(res['F'], int)
        n_c = 0
        for fi, f in enumerate(Fa):
            for k in range(3):
                g = f[k]
                if zone[g]:
                    _d, j = kd.query([U[fi, k, 0] % 1, U[fi, k, 1] % 1, tgt[g] * W])
                    U[fi, k] = tuv[j]
                    n_c += 1
        uvs = U.reshape(-1, 2)
        log('  hairline_fade: %d face corners near the hairline moved onto texels of alpha %.2f..1 over %.1f deg (%d guides in the band)' % (
            n_c, lo, band, int(zone.sum())))
    L = dict(C['look'])
    if L.get('hair_rgb') and L['use_card_texture']:
        # seen in game (Yennefer y1): with rootColorTex / tipColorTex set, rootColor / tipColor do NOT tint the hair;
        # the texture colour x textureBrightness is what renders. So a target colour becomes a brightness scale
        # (hue cannot be changed this way: fine for black / grey / white hair).
        lum = float(np.dot(L['hair_rgb'], [0.2126, 0.7152, 0.0722])) / 255.0
        L['texture_brightness'] = round(lum / max(float(np.mean(lib.luma)), 1e-3), 3)
        log('  hair_rgb %s -> textureBrightness %.3f (card texture mean luma %.2f)' % (L['hair_rgb'], L['texture_brightness'], float(np.mean(lib.luma))))
    s, tx = look_params(L, tex)
    if L.get('preset'):
        # MATERIAL PRESET (R32): keep the WHOLE material of look.mat_src (a vanilla Geralt fur or a local .redfur such
        # as the Yennefer mod's) and override only the look keys this character's config sets explicitly (colour,
        # brightness, ...). Without it every DEFAULT_LOOK value overwrites the source material (R1-R31 behaviour).
        # Entries coming from non-explicit keys are found by re-running look_params with sentinel values.
        expl = set(C.get('_look_explicit', ()))
        if L.get('hair_rgb'):
            expl |= {'hair_rgb', 'texture_brightness'}
        Ls = dict(L)
        sent = {}
        for i, (k, v) in enumerate(L.items()):
            if k in expl or k in ('mat_src', 'use_card_texture', 'card_density_only', 'preset', 'extra_set'):
                continue
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                Ls[k] = 98765.0 + i
                sent[98765.0 + i] = k
            elif isinstance(v, (list, tuple)) and v and all(isinstance(x, (int, float)) for x in v):
                Ls[k] = [97000 + i] * len(v)
                sent[float(97000 + i)] = k
        s2, _ = look_params(Ls, tex)

        def flat(v):
            out = []
            for x in (v if isinstance(v, (list, tuple)) else [v]):
                out += flat(x) if isinstance(x, (list, tuple)) else [x]
            return out

        def srcs(v):
            return {sent[float(x)] for x in flat(v) if isinstance(x, (int, float)) and not isinstance(x, bool) and float(x) in sent}
        dropped = sorted(set().union(*[srcs(v2) for v2 in s2.values()])) if s2 else []
        s = {k2: v2 for k2, v2 in s.items() if not srcs(s2.get(k2, ()))}
        log('  preset: material from %s, explicit overrides %s; source values kept for %s' % (
            L['mat_src'], sorted(expl & set(L)), dropped))
    look = dict(mat_src=C['look']['mat_src'], set=s, textures=tx)
    split_files = []
    if (C.get('split_cards') or {}).get('tint') and L['use_card_texture'] and not L.get('card_density_only'):
        _tp, _td, _timg, _tb = split_tint_texture(C['split_cards'], tex, prefix, fur_path(C, hair['comp'])[:-len('.redfur')], log=log)
        split_files.append((_tp, _td))
        look['textures'][('graphicalMaterials', 'color', 'rootColorTex')] = _tp
        look['textures'][('graphicalMaterials', 'color', 'tipColorTex')] = _tp
        look['set'][('graphicalMaterials', 'color', 'textureBrightness')] = ('Float', _tb)
        import hwpreview_pack as _HP
        _HP.EXTRA_TEXTURES[_tp] = _timg           # offline preview packs: the tinted copy is not in the game's cache
    if C.get('no_colliders'):
        # 2026-10-05 (user: "are you SURE collision was REALLY off?"): useCollision 0 is only a material flag; this writes
        # every collision sphere with a 0.1 mm radius 2 m below the feet, so no collider exists at all
        col = dict(col)
        col['rad'] = [0.0001] * len(col['rad'])
        col['pos'] = [[0.0, 0.0, -2.0] for _ in col['pos']]
    if C['growth'].get('sink_roots'):
        # 2026-10-06: deeper roots without prepend/resample (keeps the root wave): only the root point goes under
        sr_ = C['growth']['sink_roots'] if isinstance(C['growth']['sink_roots'], dict) else {}
        new_ = growth.sink_roots([s_['p'] for s_ in res['S']], head, depth=sr_.get('depth', 0.004), log=log)
        for s_, p_ in zip(res['S'], new_): s_['p'] = p_
    if C['growth'].get('bury_roots'):
        # 2026-10-06 (user, after width_root 0 thinned whole strands and the hairline cards stuck out: "hairs start a bit
        # under the skin, like the first 5-10 %"): each guide gets a straight segment into the scalp along the root
        # normal (frac of its length, max_depth cap), resampled to the same point count; the growth mesh follows the
        # roots, so every rendered hair starts under the skin and its first part is hidden by the head
        br_ = C['growth']['bury_roots'] if isinstance(C['growth']['bury_roots'], dict) else {}
        # 2026-10-09 opt-in (Ves L184: hand-laid fringe): skip_y = guides rooted in front of this mesh y keep their roots
        # on the skin (not buried); every other guide is buried as before
        sel_ = [i_ for i_, s_ in enumerate(res['S']) if br_.get('skip_y') is None or s_['p'][0][1] <= br_['skip_y']]
        if len(sel_) < len(res['S']):
            log('  bury_roots skip_y %.3f: %d front guides keep their roots on the skin' % (br_['skip_y'], len(res['S']) - len(sel_)))
        new_ = growth.bury_roots([res['S'][i_]['p'] for i_ in sel_], head, frac=br_.get('frac', 0.07),
                                 max_depth=br_.get('max_depth', 0.006), min_depth=br_.get('min_depth', 0.001),
                                     angle_deg=br_.get('angle_deg'), log=log)
        for i_, p_ in zip(sel_, new_):
            res['S'][i_]['p'] = p_
    if C['growth'].get('flare_tips'):
        # 2026-10-06 opt-in (Emhyr L132): layered lengths + flicked-out ends below the head (flare.py)
        import flare
        new_ = flare.flare_tips([s_['p'] for s_ in res['S']], head, C['growth']['flare_tips'] if isinstance(C['growth']['flare_tips'], dict) else {}, log=log)
        for s_, p_ in zip(res['S'], new_):
            s_['p'] = p_
    extra_files, head_tex = [], None
    if C['growth'].get('head_tex'):
        # 2026-10-06 (hairline material, HAIRWORK.md "head_tex"): continuous head-space corner UVs + our own density /
        # rootColor / tipColor textures written into the mod (headtex.py); opt-in, default configs never set it
        import headtex
        head_tex = headtex.build(C, res, uvs, head, ch, alpha, bool(C['look']['use_card_texture'] or C['look'].get('card_density_only')),
                                 log=log, prefix=prefix, base_path=fur_path(C, hair['comp'])[:-len('.redfur')])
        uvs = head_tex['uvs']
        extra_files = head_tex['files']
        headtex.apply_look(head_tex, look, log=log)
    extra_files = list(extra_files) + split_files
    _F = res['F']
    if C.get('flip_faces'):
        # 2026-10-05 DIAGNOSTIC (user: same strands visible in 3/4, transparent from the front = view-dependent): flip
        # every growth triangle's winding (and its corner UVs) -> root normals point inward. If the game culls hairs
        # by root normal (backface culling forced on), what is culled from the front flips.
        _F = np.asarray(_F)[:, ::-1].copy()
        uvs = np.asarray(uvs).reshape(-1, 3, 2)[:, ::-1, :].reshape(-1, 2)
        log('  flip_faces: growth triangle winding reversed (root normals inward)')
    if C.get('ref_fur'):
        # 2026-10-06 opt-in (Yennefer): the Yennefer mod's own guides / faces / UVs / material / maps (yenref.py); our bones + colliders
        import yenref
        _S, _F, uvs, look, _rf = yenref.apply(C, bones, res, look, prefix, fur_path(C, hair['comp'])[:-len('.redfur')], log=log, head=head, cards=ch)
        res = dict(res, S=_S, F=_F)
        extra_files = list(extra_files) + _rf
    if C.get('core'):
        # 2026-10-11 opt-in release mode (core/README.md): the Core is a script dependency (requirement script written
        # with the mod folder below); the fur itself is the dev build's. Default builds never set it.
        core_unretarget(look, log=log)
    data = redfur.write(res['S'], _F, uvs, bones, col, look, auto_hide=C.get('auto_hide', 100.0))
    if C.get('studio_geo'):
        # 2026-10-12 opt-in: the guides / faces / UVs of a Groom Studio project (its export's _geo.json, incl. Studio-only
        # ops such as rebuild) replace the built geometry; material / bones / collision stay this build's (studio_geo.py)
        import studio_geo
        data, _sg_files = studio_geo.apply(C, data, fur_path(C, hair['comp']), len(bones), log=log)
        extra_files = list(extra_files) + _sg_files
    redfur.verify(data)
    try:                                # R78: HairWorks Preview data pack (reads the fur bytes only; never breaks a build)
        strand_edits.export_hw_pack(C, data, _edit_path, tex=tex, comp=hair['comp'], index=_idx, log=log)
    except Exception as ex:
        log('  hw pack FAILED: %r' % (ex,))
    log('fur %s: %d guides, %d faces, %d bytes, face coverage targets mean %.2f (hairline ring faces %d)' % (
        hair['comp'], len(res['S']), len(res['F']), len(data), float(target.mean()), int((ring == 0).sum())))
    return dict(fur=data, growth=res, head=head, col=col, uvs=uvs, hair=hair, tex=tex, info=info, extra_files=extra_files,
                head_tex=head_tex, ch=ch, alpha=alpha)


def comp_mask(ch, sel):
    """vertex mask: whole connected components hit by the vertex selection `sel`."""
    lab = G.components(ch['nv'], ch['idx'])
    return np.isin(lab, np.unique(lab[sel])) if sel.any() else np.zeros(ch['nv'], bool)


def underlayer_mask(ch, head, ul, accessories=()):
    """inner card layer: components whose median skin distance <= ul['max_sd'] (and max <= ul['max_sd_max']),
    centred below head.top - ul['below_top'] and behind head.c.y + ul['front'] (ears / nape / side of the neck);
    accessories excluded. Used for the hybrid card + fur look."""
    lab = G.components(ch['nv'], ch['idx'])
    used = np.unique(ch['idx'])
    sd, _n, _d = head.signed(ch['pos'])
    acc = np.zeros(ch['nv'], bool)
    for a in accessories:
        acc |= comp_mask(ch, a(ch['uv'], ch['pos']))
    keep = np.zeros(ch['nv'], bool)
    for l in np.unique(lab[used]):
        m = lab == l
        if acc[m].any():
            continue
        c = ch['pos'][m].mean(0)
        if np.median(np.abs(sd[m])) <= ul.get('max_sd', 0.008) and np.abs(sd[m]).max() <= ul.get('max_sd_max', 0.02)                 and c[2] < head.top - ul.get('below_top', 0.08) and c[1] < head.c[1] + ul.get('front', 0.03):
            keep |= m
    return keep


def build(C, install=False, log=print, prefix='', assemble=False):
    """prefix (batch capture, R22+): every NEW file (fur, subset meshes, the patched hair template) goes under
    `prefix` + its normal path, so several variants coexist in one mod; with assemble=True nothing is written:
    returns (files, bufs, [(vanilla hair template, prefixed copy)])."""
    import template
    from w3lib import write_legacy_bundle, write_legacy_metadata_store, bundle_entries, bundle_read
    P = (lambda p: prefix + p) if prefix else (lambda p: p)
    info = discover(C, log=lambda *a: None)
    import strand_edits
    strand_edits.clear_exports(C['name'])     # no stale editor export when this build fails
    files, bufs, tpls = [], [], []
    # 2026-10-11 opt-in beard HairWorks (beard.py): "beard": {"hair": false} keeps the hair on its vanilla cards
    _beard_only = isinstance(C.get('beard'), dict) and C['beard'].get('hair') is False
    for hair in ([] if _beard_only else info['hair']):
        st = generate(C, hair, log, prefix=prefix)
        fp = P(fur_path(C, hair['comp']))
        files.append((fp, st['fur']))
        files.extend(st.get('extra_files', []))      # head_tex textures (already under the prefix)
        extra = []
        for acc in C.get('accessories', []):
            if not acc.get('keep_mesh', True):
                continue
            new_mesh = P(hair['mesh'][:-len('.w2mesh')] + '_' + acc['name'] + '.w2mesh')
            w, b, stt = template.subset_mesh(hair['mesh'], lambda ch, acc=acc: acc_mask(acc, ch))
            log('  accessory %s kept as %s: (verts, live tris, tris) per chunk %s' % (acc['name'], new_mesh, stt))
            files.append((new_mesh, w))
            bufs.append((new_mesh + '.1.buffer', b))
            extra.append((hair['comp'] + '_' + acc['name'], new_mesh))
        if C.get('hole_cards'):
            # 2026-10-05 (user workaround for the see-through windows beside the cheeks / jaw that strands do not fill
            # in game): keep ONLY the vanilla card components whose centre lies beside the face (|x| x0..x1, in front of
            # the head centre + front, between top - zhi and top - zlo) as an extra card mesh drawn with the fur. Not
            # the old 'underlayer' (scalp card patches, rejected for women).
            import growth
            hc = C['hole_cards'] if isinstance(C['hole_cards'], dict) else {}
            head_ = growth.Head(C.get('head_mesh') or info['heads'][0])

            cents = np.array(hc.get('centroids', []), float).reshape(-1, 3)
            hverts = np.array(hc.get('verts', []), float).reshape(-1, 3)

            def hole_mask(ch, hc=hc, head_=head_, cents=cents):
                # TARGETED (user: "only fix the holes, nothing else"): with 'centroids' (build/hole_targets.py, from
                # the measured in-game windows) only the card components whose centre is within 1 mm of a listed
                # centre are kept; the zone rule below is only a fallback
                if len(hverts):          # triangle-level: the vertices of the card triangles on the measured windows
                    from scipy.spatial import cKDTree as _KDh
                    return _KDh(hverts).query(ch['pos'])[0] < 0.0002
                lab = G.components(ch['nv'], ch['idx'])
                used = np.unique(ch['idx'])
                keep = np.zeros(ch['nv'], bool)
                for l in np.unique(lab[used]):
                    m = lab == l
                    c = ch['pos'][m].mean(0)
                    if len(cents):
                        if np.min(np.linalg.norm(cents - c, axis=1)) < 0.001:
                            keep |= m
                    elif hc.get('x0', 0.045) <= abs(c[0]) <= hc.get('x1', 0.11) and c[1] > head_.c[1] + hc.get('front', -0.01) \
                            and head_.top - hc.get('zlo', 0.24) <= c[2] <= head_.top - hc.get('zhi', 0.08):
                        keep |= m
                return keep
            new_mesh = P(hair['mesh'][:-len('.w2mesh')] + '_holes.w2mesh')
            w, b, stt = template.subset_mesh(hair['mesh'], hole_mask)
            log('  hole_cards: vanilla cards beside the face kept as %s: (verts, live tris, tris) per chunk %s' % (new_mesh, stt))
            files.append((new_mesh, w))
            bufs.append((new_mesh + '.1.buffer', b))
            extra.append((hair['comp'] + '_holes', new_mesh))
        if C.get('ribbon_under'):
            # 2026-10-13 opt-in (Ciri): thin custom strand ribbons under the fur where the scalp looks empty (ribbon_under.py)
            import growth
            import ribbon_under
            rc_ = C['ribbon_under'] if isinstance(C['ribbon_under'], dict) else {}
            head_ = growth.Head(C.get('head_mesh') or info['heads'][0])
            if C.get('studio_geo'):
                import studio_geo
                _gd = studio_geo.load_geo(C['studio_geo']['geo'])['guides']
            else:
                _gd = [s_['p'] for s_ in st['growth']['S']]
            _R = ribbon_under.build(rc_, head_, _gd, log=log)
            new_mesh = P(hair['mesh'][:-len('.w2mesh')] + '_ribbons.w2mesh')
            w, b, stt = ribbon_under.write_mesh(hair['mesh'], _R, log=log)
            log('  ribbon_under: %d ribbons as %s: (verts, tris, slots) per chunk %s' % (_R['nribbons'], new_mesh, stt))
            files.append((new_mesh, w))
            bufs.append((new_mesh + '.1.buffer', b))
            extra.append((hair['comp'] + '_ribbons', new_mesh))
        if C.get('hairline_cards'):
            # 2026-10-06 (user: smooth out the spiky strand roots at the hairline; if thinner roots are not enough "use
            # cardboard system only on hairline", roots moved back): keep ONLY the vanilla card triangles on a band
            # along the vanilla hairline curve (elevation line - below_deg .. line + above_deg, |azimuth| < phi_max,
            # within near_skin of the scalp) as an extra card mesh drawn with the fur; their alpha-faded edge gives the
            # soft hairline. Pair with growth.hairline_row tol_deg=-X / inset_deg=X to move the strand roots X deg back.
            import growth
            from texread import load_xbm
            hl = C['hairline_cards'] if isinstance(C['hairline_cards'], dict) else {}
            head_ = growth.Head(C.get('head_mesh') or info['heads'][0])
            _m = G.load_mesh(hair['mesh'])
            _ch = G.merge_chunks(_m, C.get('lod0_chunks') or _m['lod0'])
            _al = np.asarray(load_xbm(C.get('card_texture') or card_texture(hair['mesh'])))[..., 3].astype(np.float64) / 255.0
            _pm = hl.get('phi_max', 110.0)
            _bins, _line, _p, _P, _ph, _el = growth.hairline_curve(head_, _ch, _al, phi_max=_pm)
            _ok = ~np.isnan(_line)

            # 2026-10-06 `sides: true` (opt-in; absent = exactly the front band above): the band also follows the side
            # hairline (temples, sideburns, around the ears). growth.hairline_side_edge = per side and elevation bin the most
            # forward azimuth of the visible near-skin card coverage; keep triangles side_front_deg in front of / side_back_deg
            # behind that edge (azimuth degrees), el from side_el_min up to the front curve's end elevation + above_deg.
            # The front band is then limited to the azimuths where the front curve exists (no clamp beyond its ends); the
            # two masks are unioned (the side band starts at the front curve's end elevation + above_deg, so they overlap).
            sides = bool(hl.get('sides'))
            side_edge = {}
            if sides:
                side_edge = growth.hairline_side_edge(
                    head_, _ch, _al, _bins, _line, el_min=hl.get('side_el_min', -10.0), bin_deg=hl.get('side_bin_deg', 3.0),
                    near_skin=hl.get('near_skin', 0.015), pct=hl.get('side_pct', 3.0), lead_deg=hl.get('side_lead_deg', 10.0))
                if hl.get('side_from_roots'):
                    # 2026-10-06 (user, L88: the side cards were "not even aligned with the hairwork"): the side edge from
                    # OUR guide roots (most forward |phi| per elevation bin) instead of the vanilla card coverage, so the
                    # card band sits exactly where the strands start, as on the front
                    _R = np.array([np.asarray(s_['p'], float)[0] for s_ in st['growth']['S']])
                    _d = _R - np.array(head_.c, float)
                    _rp = np.degrees(np.arctan2(_d[:, 0], _d[:, 1]))
                    _re = np.degrees(np.arctan2(_d[:, 2], np.hypot(_d[:, 0], _d[:, 1])))
                    for s_, e_ in side_edge.items():
                        bw = hl.get('side_bin_deg', 3.0) * 1.5
                        sel_ = (np.sign(_d[:, 0]) == s_) & (np.abs(_rp) >= e_['phi_end'] - hl.get('side_lead_deg', 10.0))
                        ed = np.full(len(e_['el']), np.nan)
                        for i_, ce in enumerate(e_['el']):
                            mm = sel_ & (np.abs(_re - ce) < bw)
                            if mm.sum() >= 2:
                                ed[i_] = np.percentile(np.abs(_rp[mm]), 10)
                        g_ = ~np.isnan(ed)
                        if g_.sum() >= 2:
                            e_['phi'] = np.interp(e_['el'], e_['el'][g_], ed[g_])
                log('  hairline_cards sides: edge |phi| per side (el %s): %s' % (
                    hl.get('side_el_min', -10.0), {s_: dict(phi_end=round(e_['phi_end'], 1), el_end=round(e_['el_end'], 1),
                                                           edge=[round(float(x), 1) for x in e_['phi']]) for s_, e_ in side_edge.items()}))
            hl_dbg = []

            def hairline_mask(ch, hl=hl, head_=head_):
                idx = ch['idx'].reshape(-1, 3)
                c0 = np.array(head_.c, float)
                tc = ch['pos'][idx].mean(1)
                d = tc - c0
                phi = np.degrees(np.arctan2(d[:, 0], d[:, 1]))
                el = np.degrees(np.arctan2(d[:, 2], np.hypot(d[:, 0], d[:, 1])))
                lin = np.interp(phi, _bins[_ok], _line[_ok])
                zone = np.abs(phi) < _pm
                if sides:
                    # front zone: the valid azimuth range, continued (line clamped) up to the side edge at the corner
                    lo_ = -(side_edge[-1]['phi'][-1] if -1 in side_edge else -_bins[_ok].min())
                    hi_ = side_edge[1]['phi'][-1] if 1 in side_edge else _bins[_ok].max()
                    zone &= (phi >= min(lo_, _bins[_ok].min() - 2.0)) & (phi <= max(hi_, _bins[_ok].max() + 2.0))
                sel = zone & (el >= lin - hl.get('below_deg', 3.0)) & (el <= lin + hl.get('above_deg', 8.0))
                near = np.abs(head_.signed(tc)[0]) < hl.get('near_skin', 0.015)
                sel &= near
                if not sides:
                    return ('tris', sel)
                side = np.zeros(len(sel), bool)
                fd = hl.get('side_front_deg', hl.get('below_deg', 3.0))
                bd = hl.get('side_back_deg', hl.get('above_deg', 8.0))
                for sgn, e_ in side_edge.items():
                    edge = np.interp(el, e_['el'], e_['phi'])
                    top = e_['el_end'] + hl.get('above_deg', 8.0)
                    side |= (np.sign(d[:, 0]) == sgn) & (np.abs(phi) >= edge - fd) & (np.abs(phi) <= edge + bd)                         & (el >= hl.get('side_el_min', -10.0)) & (el <= top)
                side &= near
                hl_dbg.append(dict(phi=phi, el=el, front=sel.copy(), side=side.copy(), near=near))
                log('  hairline_cards sides: %d triangles from the front band, %d from the side band (%d in both), union %d of %d' % (
                    sel.sum(), side.sum(), (sel & side).sum(), (sel | side).sum(), len(sel)))
                return ('tris', sel | side)
            new_mesh = P(hair['mesh'][:-len('.w2mesh')] + '_hairline.w2mesh')
            w, b, stt = template.subset_mesh(hair['mesh'], hairline_mask)
            log('  hairline_cards: vanilla card band along the hairline kept as %s: (verts, live tris, tris) per chunk %s' % (new_mesh, stt))
            if sides and hl_dbg:
                _dp = os.path.join(ROOT, 'build', 'hairline_cards_%s.npz' % C['name'])
                os.makedirs(os.path.dirname(_dp), exist_ok=True)
                _d0 = max(hl_dbg, key=lambda q: len(q['phi']))       # preview data (tools\hairworks\hairline_cards_preview.py)
                np.savez(_dp, **{k_: v_ for k_, v_ in _d0.items()}, bins=_bins, line=_line,
                         **{'edge_el_%d' % s_: e_['el'] for s_, e_ in side_edge.items()},
                         **{'edge_phi_%d' % s_: e_['phi'] for s_, e_ in side_edge.items()})
                log('  hairline_cards sides: preview data %s' % _dp)
            files.append((new_mesh, w))
            bufs.append((new_mesh + '.1.buffer', b))
            extra.append((hair['comp'] + '_hairline', new_mesh))
        fallback = None
        if C.get('split_cards'):
            # 2026-10-06 (Anna): the vanilla updo cards kept as an extra card mesh drawn with the fur (like hairline_cards);
            # with HairWorks off the fur component falls back to the complement, so the updo is drawn exactly once
            import growth
            sc_ = C['split_cards'] if isinstance(C['split_cards'], dict) else {}
            head_ = growth.Head(C.get('head_mesh') or info['heads'][0])
            new_mesh = P(hair['mesh'][:-len('.w2mesh')] + '_updo.w2mesh')
            w, b, stt = template.subset_mesh(hair['mesh'], lambda ch, head_=head_: ('tris', split_tri_mask(sc_, ch, head_)))
            log('  split_cards: vanilla updo cards kept as %s: (verts, live tris, tris) per chunk %s' % (new_mesh, stt))
            files.append((new_mesh, w))
            bufs.append((new_mesh + '.1.buffer', b))
            extra.append((hair['comp'] + '_updo', new_mesh))
            fb_mesh = P(hair['mesh'][:-len('.w2mesh')] + '_rest.w2mesh')
            w, b, stt = template.subset_mesh(hair['mesh'], lambda ch, head_=head_: ('tris', ~split_tri_mask(sc_, ch, head_)))
            log('  split_cards: fallback (HairWorks off) = complement %s: %s' % (fb_mesh, stt))
            files.append((fb_mesh, w)); bufs.append((fb_mesh + '.1.buffer', b))
            fallback = fb_mesh
        ul = C.get('underlayer')
        if ul and ul.get('enabled', True):
            # HYBRID: keep the inner card layer (close to the skin, behind the ears / nape / side of the neck) drawn
            # under the fur, skinned to the vanilla dangle like the tie, for volume and the vanilla silhouette. The
            # fur component's fallback mesh becomes the complement (outer cards + everything not kept), so with
            # HairWorks off the original card set is drawn exactly once.
            import growth
            head = growth.Head(C.get('head_mesh') or info['heads'][0])
            accs = [uv_box(a['uv_box']) for a in C.get('accessories', []) if a.get('keep_mesh', True) and a.get('uv_box')]
            inner = lambda ch: underlayer_mask(ch, head, ul, accs)
            def outer(ch):
                m = inner(ch)
                for a in accs:
                    m = m | comp_mask(ch, a(ch['uv'], ch['pos']))
                return ~m
            ul_mesh = P(hair['mesh'][:-len('.w2mesh')] + '_under.w2mesh')
            fb_mesh = P(hair['mesh'][:-len('.w2mesh')] + '_outer.w2mesh')
            w, b, stt = template.subset_mesh(hair['mesh'], inner)
            log('  underlayer cards kept as %s: (verts, live tris, tris) per chunk %s' % (ul_mesh, stt))
            files.append((ul_mesh, w)); bufs.append((ul_mesh + '.1.buffer', b))
            extra.append((hair['comp'] + '_under', ul_mesh))
            w, b, stt = template.subset_mesh(hair['mesh'], outer)
            log('  fallback (HairWorks off) = complement %s: %s' % (fb_mesh, stt))
            files.append((fb_mesh, w)); bufs.append((fb_mesh + '.1.buffer', b))
            fallback = fb_mesh
        elif C.get('fur_only'):
            # DIAGNOSTIC (R16): the fur component's mesh = an EMPTY copy of the cards (every triangle degenerate), so
            # with HairWorks on only the strands (+ kept accessories) draw. With HairWorks off the character is bald.
            fb_mesh = P(hair['mesh'][:-len('.w2mesh')] + '_empty.w2mesh')
            w, b, stt = template.subset_mesh(hair['mesh'], lambda ch: np.zeros(ch['nv'], bool))
            log('  fur_only: card mesh replaced by empty %s: %s' % (fb_mesh, stt))
            files.append((fb_mesh, w)); bufs.append((fb_mesh + '.1.buffer', b))
            fallback = fb_mesh
        elif C.get('accessories'):
            # without an underlayer the full card mesh stays the fallback (accessories then draw twice when HW is off)
            pass
        # template_from_vanilla (2026-10-05, Triss): patch CDPR's template, not the source_mods one (licence)
        _tsrc = G.get_vanilla(hair['template']) if C.get('template_from_vanilla') else G.get(hair['template'])
        if C.get('lore_guard'):
            # 2026-10-11 opt-in (Triss, release builds): the third-party hair mod (Nexus 6457) is a hard requirement.
            # A plain card component (the same path as the fur's mesh: the lore-friendly mesh when that mod is installed,
            # CDPR's short hair otherwise) + a scripted guard component that hides the cards (mod present) or the fur
            # (mod missing: vanilla Triss + an on-screen message). See lore_guard.py.
            import lore_guard
            extra = list(extra) + [(hair['comp'] + lore_guard.CARDS_SUFFIX, hair['mesh'])]
        if C.get('mod_guard'):
            # 2026-10-14 opt-in (hairband Ciri, release builds): the same requirement for any third-party hair mod, checked
            # by loading a file only that mod ships (mod_guard.py)
            import mod_guard
            extra = list(extra) + [(hair['comp'] + mod_guard.CARDS_SUFFIX, hair['mesh'])]
        new = template.patch(_tsrc, hair['comp'], fp, extra, fallback)
        if C.get('lore_guard'):
            new = template.add_script_component(new, lore_guard.CLASS, lore_guard.COMP)
        if C.get('mod_guard'):
            new = template.add_script_component(new, *mod_guard.names(C['mod_guard']))
        template.validate(new, hair['template'])
        files.append((P(hair['template']), new))
        tpls.append((hair['template'], P(hair['template'])))
    if C.get('beard') not in (None, False):
        # 2026-10-11 opt-in: the beard cards -> a second, separate fur (CFurComponent swapped in place like Geralt's
        # vanilla beard); without the key this block never runs and the hair build above is unchanged
        import beard
        beard.add_to_build(C, info, files, bufs, tpls, P, log)
    if assemble:
        return files, bufs, tpls
    mod = mod_src(C)
    content = os.path.join(mod, 'content')
    if os.path.isdir(mod):
        shutil.rmtree(mod)
    os.makedirs(content)
    bundles = [write_legacy_bundle(os.path.join(content, 'blob0.bundle'), files)]
    if bufs:
        bundles.append(write_legacy_bundle(os.path.join(content, 'buffers0.bundle'), bufs))
    write_legacy_metadata_store(os.path.join(content, 'metadata.store'), bundles)
    ents = {e.name: e for e in bundle_entries(os.path.join(content, 'blob0.bundle'))}
    for rel, data in files:
        assert bundle_read(os.path.join(content, 'blob0.bundle'), ents[rel]) == data
    log('built %s' % content)
    for rel, data in files + bufs:
        log('  %8d  %s' % (len(data), rel))
    if C.get('core'):
        # release mode (opt-in): requirement script content\scripts\local\hwfa_req_<id>.ws (calls the Core's functions:
        # without the Core the game's script compiler fails) + REQUIREMENTS.txt + hairworkforall_mod.json next to content\
        K = _hwcore()
        meta = K.write_release_files(mod, C['mod'], K.required_version(), dict(config=C['name']), salt=(C['name'],))
        if C.get('lore_guard'):
            import lore_guard
            lore_guard.write_script(mod, hair['comp'], hair['mesh'])
            log('lore guard: script + scripted component (Nexus 6457 required at runtime)')
        if C.get('mod_guard'):
            import mod_guard
            accs = [x['name'] for x in C.get('accessories', []) if x.get('keep_mesh', True)]
            mod_guard.write_script(mod, C['mod_guard'], hair['comp'], accs, C['mod'])
            log('mod guard: script + scripted component (%s required at runtime)' % C['mod_guard']['label'])
        log('release: %s (requires_core %s, %s)' % (K.requirement_line(meta['requires_core'], ascii=True),
                                                    meta['requires_core'], meta['core_check']))
    if install:
        do_install(C, files + bufs, log)
    return mod


def mod_src(C):
    """Repo folder of a character's built mod: mods/<mod> (finished characters) or mods/wip/<mod> (default for new builds)."""
    if os.environ.get('HWFA_MOD_OUT'):          # release builds (build_release_hairwork.py --allinone): own folder, repo mods untouched
        return os.path.join(os.environ['HWFA_MOD_OUT'], C['mod'])
    for d in (os.path.join(ROOT, 'mods', C['mod']), os.path.join(ROOT, 'mods', 'wip', C['mod'])):
        if os.path.isdir(d):
            return d
    return os.path.join(ROOT, 'mods', 'wip', C['mod'])


def running():
    out = subprocess.run(['tasklist'], capture_output=True, text=True).stdout.lower()
    return 'witcher3.exe' in out


def do_install(C, files, log=print):
    from w3lib import bundle_entries
    assert not running(), 'witcher3.exe is running: not installing'
    dst = os.path.join(GAME, 'mods', C['mod'])
    mine = {r.lower() for r, _ in files}
    for root, _, fs in os.walk(os.path.join(GAME, 'mods')):
        if os.path.abspath(root).startswith(os.path.abspath(dst)):
            continue
        for fn in fs:
            if fn.endswith('.bundle'):
                for e in bundle_entries(os.path.join(root, fn)):
                    assert e.name.lower() not in mine, ('conflict', root, e.name)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(mod_src(C), dst)
    log('installed -> %s' % dst)


if __name__ == '__main__':
    a = sys.argv[1:]
    C = load_config(a[0])
    cmd = a[1] if len(a) > 1 else 'build'
    if cmd == 'discover':
        import discover as D
        sys.argv = ['discover', C['template']] + ([C['appearance']] if C.get('appearance') else [])
        info = discover(C)
        for h in info['hair']:
            print('hair:', h['template'], h['comp'], h['mesh'], 'used by', h['used_by'])
        print('heads:', info['heads']); print('bodies:', info['bodies']); print('beards (kept vanilla):', info['beards'])
    elif cmd == 'plot':
        import plot
        st = generate(C)
        out = a[2] if len(a) > 2 else os.path.join(ROOT, 'build', 'hairpipe_%s.png' % C['name'])
        import collision
        app = C.get('appearance') or ''
        meshes = (C.get('outfits') or {}).get(app) or st['info'].get('outfit', {}).get(app, [])
        with G.outfit_source(C['growth'].get('outfit_mods')):
            op = collision.outfit_points(meshes, skip_bad=bool(C['growth'].get('outfit_mods')))[0] if meshes else None
        plot.plot(st['growth'], st['head'], out, col=st['col'], outfit_pts=op)
        print(out)
    elif cmd == 'build':
        if '--release' in a:
            # 2026-10-11 release export (opt-in): requires the Hairwork for All Core (core/README.md); --mod <name>
            # builds into another mod folder (a test name, so the character's own mod is never overwritten)
            C['core'] = True
            os.environ['HWFA_SKELETON'] = 'game'     # release builds never use the third-party legacy skeleton (tpl/fur02.redfur)
            C['name'] += '__release'        # own editor / preview exports: the dev build's build\hairbatch files stay
        if '--lore-guard' in a:
            C['lore_guard'] = True       # opt-in, release builds of Triss (see lore_guard.py)
        if '--mod-guard' in a:
            assert C.get('mod_guard_cfg'), 'config has no mod_guard_cfg (see mod_guard.py)'
            C['mod_guard'] = C['mod_guard_cfg']     # opt-in, release builds of third-party hair conversions
        if '--mod' in a:
            C['mod'] = a[a.index('--mod') + 1]
        build(C, '--install' in a)
    elif cmd == 'uninstall':
        assert not running()
        shutil.rmtree(os.path.join(GAME, 'mods', C['mod']), ignore_errors=True)
        print('removed', C['mod'])
    elif cmd == 'capture':
        import capture
        v = a[a.index('--variant') + 1] if '--variant' in a else ('baseline' if '--baseline' in a else 'current')
        capture.run(C['name'], v, wait='--wait' in a, verify='--verify' in a)
