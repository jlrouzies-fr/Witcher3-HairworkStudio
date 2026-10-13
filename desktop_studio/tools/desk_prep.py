r"""desk_prep - build the input folder the Desktop Groom Studio opens (one per config).

  py desktop_studio\tools\desk_prep.py <cfg> [--fur <built.redfur>] [--edit <cfg>_EDIT.json] [--out <dir>]
  py desktop_studio\tools\desk_prep.py <cfg> --build    # force a pipeline rebuild of the fur first
  py desktop_studio\tools\desk_prep.py <cfg> --check    # exit 1 when the pack is stale (config / edits changed, old pack)
  py desktop_studio\tools\desk_prep.py --list            # configs that have a built redfur + edit export

Reads pipeline outputs (nothing is installed):
  * the config's CURRENT build (canonical_fur): build\desktop_studio\furs\<cfg>__<comp>.redfur, rebuilt with
    hairpipe.build(assemble=True) whenever the hash of chars\<cfg>.json + its edits file differs from the stamp
    <cfg>.src.json (lab round copies are never used for a config that can be built): guides, growth faces, UVs,
    per-guide bone indices / weights, bone table, collision spheres / capsules, physical / graphical / LOD blocks.
  * the strand-editor export <cfg>_EDIT.json (build\hairbatch\edit\ or results\<round>\): edit-frame guides (the ops
    frame the web Studio and strand_edits.py use) and the head / hat / outfit meshes.
  * the body / outfit / eye meshes of the config's appearance (the one HairLab spawns), LOD0, per material, with the
    spec-workflow metal colour and the template's colour shifts baked into the albedo (outfit_textured).
  * the density / colour textures the fur imports (game texture cache via texread, else the data-pack copy).
  * the reference build for the performance budget: APPROVED.json "approved" entry of the character if it has one,
    else the loaded config itself.

Writes <out>\desk.json + <out>\tex_<n>.png (default out = build\desktop_studio\<cfg>\; build\ is git-ignored).
"""
import argparse
import glob
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HW = os.path.join(ROOT, 'tools', 'hairworks')
sys.path.insert(0, HW)
sys.path.insert(0, os.path.join(ROOT, 'tools'))

import hwpreview_pack as HP  # noqa: E402

OUT_BASE = os.path.join(ROOT, 'build', 'desktop_studio')
FUR_DIR = os.path.join(OUT_BASE, 'furs')
PREP_VERSION = 8          # bump when desk.json changes; the Studio re-runs desk_prep for older packs (src/app.cpp kPrepVersion)


def _rel(p):
    try:
        return os.path.relpath(p, ROOT)
    except ValueError:        # other drive (--out in a temp dir)
        return p


# ------------------------------------------------------------------------------------------------ canonical build
# The pack always reads the CURRENT pipeline build of the config: build\desktop_studio\furs\<cfg>__<comp>.redfur, made by
# hairpipe.build(assemble=True) (nothing installed) and tagged with a hash of its inputs (the config + its edits file)
# in <cfg>.src.json. Lab round copies (build\hairlab\<round>\furs) are snapshots of an older pipeline / config state and
# are never used for a config that can be built (2026-10-11: the L124 lab copy of guillaume_l124_locks_hug differed from
# the shipped fur by 5 mm median / 29 mm p99; the pipeline rebuild is byte-identical to mods\modGuillaumeHairworks).
def src_files(cfg):
    """the inputs a build of <cfg> depends on: the config JSON and its strand-edits file (strand_edits.edits_for)"""
    p = os.path.join(HW, 'chars', cfg + '.json')
    if not os.path.exists(p):
        return []
    out = [p]
    try:
        import strand_edits
        C = json.load(open(p))
        C['name'] = cfg
        e = strand_edits.edits_for(C)
        if e:
            out.append(e)
    except SystemExit:        # edits file named but missing: the build fails the same way
        pass
    return out


def src_hash(cfg):
    import hashlib
    h = hashlib.sha1()
    for p in src_files(cfg):
        h.update(os.path.basename(p).encode() + b'\0')
        h.update(open(p, 'rb').read())
    return h.hexdigest()


def _stamp_path(cfg):
    return os.path.join(FUR_DIR, cfg + '.src.json')


def is_beard_fur(path):
    """2026-10-11 (beard HairWorks, tools/hairworks/beard.py): a config with the opt-in "beard" key builds a second fur
    <cfg>__hb_*.redfur next to the hair fur; the Studio's groom is the hair fur, the beard fur is extra pack data."""
    b = os.path.basename(path)[:-len('.redfur')]
    return '__' in b and b.split('__', 1)[1].lower().startswith('hb_')


def _hair_first(paths):
    return sorted(paths, key=lambda p: (is_beard_fur(p), p))


def _has_mod_src(cfg):
    C = dict(load_cfg(cfg))
    C.setdefault('mod', 'mod%sHairworks' % cfg.capitalize())      # hairpipe.load_config default
    return bool(mod_info(C, cfg.split('_')[0]).get('bundle'))


def canonical_fur(cfg, rebuild=False, log=print):
    """the config's current build: build\\desktop_studio\\furs\\<cfg>__*.redfur when its stamp matches the inputs' hash,
    else rebuilt with the pipeline now. A config without a chars\\<cfg>.json cannot be built: newest copy (warned)."""
    if not src_files(cfg):
        f = find_fur(cfg)
        if f:
            log('  WARNING: no config chars\\%s.json, using the newest build copy %s' % (cfg, _rel(f)))
        return f
    h = src_hash(cfg)
    furs = _hair_first(glob.glob(os.path.join(FUR_DIR, cfg + '__*.redfur')))
    furs = [f for f in furs if os.path.basename(f).split('__', 1)[0] == cfg]
    st = {}
    try:
        st = json.load(open(_stamp_path(cfg)))
    except Exception:
        pass
    edit_ok = os.path.exists(os.path.join(ROOT, 'build', 'hairbatch', 'edit', cfg + '.json')) or \
        glob.glob(os.path.join(ROOT, 'build', 'hairbatch', 'edit', cfg + '__*.json'))
    if furs and not rebuild and st.get('hash') == h and edit_ok and _has_mod_src(cfg):
        return furs[0]
    log('  %s: %s -> building it with the pipeline (hairpipe.build, nothing installed)' % (
        cfg, 'rebuild asked' if rebuild else 'no current build' if not furs or not st else
        'config / edits changed since the last build' if st.get('hash') != h else 'editor export missing'))
    for f in furs:
        os.remove(f)
    built = build_fur(cfg, log=log)
    os.makedirs(FUR_DIR, exist_ok=True)
    json.dump(dict(cfg=cfg, hash=h, files=[_rel(p) for p in src_files(cfg)], furs=[_rel(p) for p in built]),
              open(_stamp_path(cfg), 'w'), indent=1)
    return _hair_first(built)[0]


def pack_stale(cfg, out=None):
    """None when the desk pack of <cfg> is current, else the reason. The Studio makes the same test before opening a
    pack (src/app.cpp packUpToDate: prep_version, and a source file newer than desk.json)."""
    p = os.path.join(out or os.path.join(OUT_BASE, cfg), 'desk.json')
    if not os.path.exists(p):
        return 'no pack'
    try:
        D = json.load(open(p))
    except Exception:
        return 'unreadable pack'
    if D.get('prep_version', 0) < PREP_VERSION:
        return 'pack version %s < %d' % (D.get('prep_version'), PREP_VERSION)
    if src_files(cfg) and D.get('src_hash') != src_hash(cfg):
        return 'config / edits changed since the pack was made'
    if not os.path.exists(os.path.join(ROOT, D.get('fur_path') or '?')):
        return 'its fur is gone'
    return None


def find_fur(cfg):
    pats = [os.path.join(ROOT, 'build', 'desktop_studio', 'furs', cfg + '__*.redfur'),
            os.path.join(ROOT, 'build', 'hairlab', '*', 'furs', cfg + '__*.redfur'),
            os.path.join(ROOT, 'build', 'hairbatch', 'furs', cfg + '__*.redfur'),
            os.path.join(ROOT, 'results', '*', cfg + '__*.redfur')]
    hits = []
    for p in pats:
        hits += glob.glob(p)
    hits = [h for h in hits if os.path.basename(h).split('__', 1)[0] == cfg]
    hits.sort(key=os.path.getmtime, reverse=True)
    hits.sort(key=is_beard_fur)          # stable: newest hair fur first, a beard fur only when there is no hair fur
    return hits[0] if hits else None


def find_edit(cfg, comp=None):
    c = [os.path.join(ROOT, 'build', 'hairbatch', 'edit', cfg + '.json')]
    if comp:
        c.insert(0, os.path.join(ROOT, 'build', 'hairbatch', 'edit', '%s__%s.json' % (cfg, comp)))
    c += sorted(glob.glob(os.path.join(ROOT, 'results', '*', cfg + '_EDIT.json')), key=os.path.getmtime, reverse=True)
    for p in c:
        if os.path.exists(p):
            return p
    return None


def load_cfg(cfg):
    p = os.path.join(HW, 'chars', cfg + '.json')
    return json.load(open(p)) if os.path.exists(p) else {}


def mod_info(C, character):
    """in-game redfur path + w2ent paths of the character's release mod (mods\\<mod>\\content\\blob0.bundle)."""
    from w3lib import bundle_entries
    names = [C.get('mod')] if C.get('mod') else []
    names.append('mod%sHairworks' % character.capitalize())
    for nm in names:
        # last: the pack-local copy build_fur writes (release installs have no mods folder of built characters)
        for base in (os.path.join(ROOT, 'mods', nm), os.path.join(ROOT, 'mods', 'wip', nm), os.path.join(OUT_BASE, 'modsrc', nm)):
            b = os.path.join(base, 'content', 'blob0.bundle')
            if os.path.exists(b):
                ents = [e.name for e in bundle_entries(b)]
                return dict(mod=nm, bundle=os.path.relpath(b, ROOT),
                            redfur=[e for e in ents if e.endswith('.redfur')],
                            w2ent=[e for e in ents if e.endswith('.w2ent')])
    return dict(mod=C.get('mod') or ('mod%sHairworks' % character.capitalize()), bundle=None, redfur=[], w2ent=[])


def est_hairs(D, density=None, close=False):
    """HairWorks hair count estimate: 64 hairs per growth face x density x per-face density-map mean
    (when a density texture is set) [x detailLODDensity in close-ups]."""
    vol = (D['physical'] or {}).get('volume') or {}
    dens = float(vol.get('density', 1.0) if density is None else density)
    face = (D.get('density') or {}).get('face')
    s = float(np.sum(face)) if face else float(len(D['F']))
    n = 64.0 * dens * s
    if close:
        lod = D.get('lod') or {}
        if lod.get('enableLOD', 1):
            n *= float((lod.get('detailLOD') or {}).get('detailLODDensity', 1.0))
    return n


def budget_of(D):
    vol = (D['physical'] or {}).get('volume') or {}
    sw = (D['physical'] or {}).get('strandWidth') or {}
    lod = D.get('lod') or {}
    K = int(D['K'])
    cvs = (K - 1) * int(D.get('splineMultiplier', 1)) + 1
    h = est_hairs(D)
    hc = est_hairs(D, close=True)
    return dict(config=D.get('config'), faces=len(D['F']), guides=len(D['guides']), K=K, cvs=cvs,
                density=float(vol.get('density', 1.0)), width=float(sw.get('width', 1.0)),
                detailLODDensity=float((lod.get('detailLOD') or {}).get('detailLODDensity', 1.0)),
                hairs=round(h), hairs_close=round(hc), cost=round(hc * cvs * float(sw.get('width', 1.0))))


# ------------------------------------------------------------------------------------------------ material presets
# name -> (how, source). Values = the config's look run through hairpipe (DEFAULT_LOOK + look keys -> look_params) on
# top of its mat_src material, i.e. what a build with those keys writes. Parameter values only (the Yen-mod look is the
# pipeline's own re-creation in yen_material.json; no third-party file is read or shipped).
PRESETS = [
    ('Default conversion (thick basic strands)', 'default', 'hairpipe.DEFAULT_LOOK: the plain default conversion (geralt_v1halftail_long material)'),
    ('Fine strands (thin, soft)', 'plan:yen_material', 'lab/plans/yen_material.json = the look of vesemir_r31a (approved)'),
    ('Fine strands, keep dynamics (look only)', 'plan:yen_strands_only', 'lab/plans/yen_strands_only.json (Yen look without the dynamics keys)'),
    ('Fine strands, sheen (alternative shading)', 'cfg:emhyr_l132_struct_m2', 'chars/emhyr_l132_struct_m2.json (m2 = Yennefer-mod graphical values)'),
    ('Coarse short (short hair, beards)', 'cfg:lambert_l76_ez10_vol5d_wr15_b07_f8', 'chars/lambert_l76_ez10_vol5d_wr15_b07_f8.json (Lambert, user: good now; stiff root, free tips)'),
    ('Curly / wavy (holds curls)', 'cfg:oriana_l141_a', 'chars/oriana_l141_a.json (strong-curl line, stiff enough to hold curls)'),
    ('Tied / braided sheen (glossy)', 'cfg:triss_l82_fe_user1b', 'chars/triss_l82_fe_user1b.json (approved)'),
    ('Grey / aged (low specular)', 'cfg:avallach_l85b_hl70_wv_ln_hc', 'chars/avallach_l85b_hl70_wv_ln_hc.json (Avallach white hair, settled)'),
]
# never part of a material preset: density / hair count belong to the character (performance budget)
PRESET_SKIP = {('physicalMaterials', 'volume', 'density'), ('physicalMaterials', 'volume', 'usePixelDensity'),
               ('physicalMaterials', 'volume', 'densityTexChannel'), ('physicalMaterials', 'volume', 'lengthScale')}


def _tree_value(typ, v):
    if typ == 'Color':
        return {'Red': int(v[0]), 'Green': int(v[1]), 'Blue': int(v[2]), 'Alpha': int(v[3]) if len(v) > 3 else 255}
    if typ == 'Vector':
        v = list(v) + [0.0] * (4 - len(v))
        return {'X': float(v[0]), 'Y': float(v[1]), 'Z': float(v[2]), 'W': float(v[3])}
    if typ in ('Bool', 'Uint32'):
        return int(v)
    if typ == 'Float':
        return float(v)
    return v


def _flatten(d, pre, out):
    for k, v in d.items():
        p = pre + (k,)
        if isinstance(v, dict) and not ('@import' in v or 'Red' in v or 'X' in v):
            _flatten(v, p, out)
        elif not (isinstance(v, dict) and '@import' in v):
            out['.'.join(p)] = v


def material_preset(how):
    import hairpipe as H
    import gamefiles as GF
    if how == 'default':
        look = {}
    elif how.startswith('plan:'):
        pl = json.load(open(os.path.join(HW, 'lab', 'plans', how[5:] + '.json')))
        look = {}
        for k, v in pl.items():
            if k.startswith('look.extra_set.'):
                look.setdefault('extra_set', {})[k[len('look.extra_set.'):]] = v
            elif k.startswith('look.'):
                look[k[5:]] = v
    else:
        look = dict(json.load(open(os.path.join(HW, 'chars', how[4:] + '.json'))).get('look', {}))
    keys = {('look.' + k): v for k, v in look.items() if k != 'extra_set'}
    for k, v in look.get('extra_set', {}).items():
        keys['look.extra_set.' + k] = v
    for k in ('look.density', 'look.length_scale', 'look.use_card_texture', 'look.card_density_only', 'look.hair_rgb'):
        keys.pop(k, None)
    L = dict(H.DEFAULT_LOOK)
    L.update(look)
    L['use_card_texture'] = False
    L['card_density_only'] = False
    sets, _ = H.look_params(L, None)
    t = HP.parse_fur(GF.get(L['mat_src']))
    vals = {}
    _flatten({'physicalMaterials': t.get('physicalMaterials') or {}, 'graphicalMaterials': t.get('graphicalMaterials') or {}}, (), vals)
    for path, (typ, v) in sets.items():
        if not typ.startswith('handle'):
            vals['.'.join(path)] = _tree_value(typ, v)
    for k in list(vals):
        if tuple(k.split('.')) in PRESET_SKIP:
            del vals[k]
    return dict(values=vals, config_keys=keys)


def materials(log=print):
    out = {}
    for name, how, src in PRESETS:
        try:
            m = material_preset(how)
            m['source'] = src
            out[name] = m
        except Exception as e:  # a missing config / game file only drops that preset
            log('  preset %s skipped: %r' % (name, e))
    return out


# ------------------------------------------------------------------------------------------------ HairLab shot cameras
SHOTS = [0, 2, 3, 4, 5, 15]
CROP = 1400.0 / 2160.0     # HairLab head crop = 1400 px square at the centre of a 3840 x 2160 frame (capture.HEAD_BOX); W3 fov is vertical


def _shot_files(character):
    """{(shot, vanilla): newest raw shot json} for the character (twins excluded)."""
    best = {}
    for p in glob.glob(os.path.join(ROOT, 'results', '*', 'raw', '*_shot*.json')):
        if '_' + character + '_' not in os.path.basename(p):
            continue
        try:
            d = json.load(open(p))
        except Exception:
            continue
        if d.get('char') != character or d.get('twin_of') or 'shot' not in d:
            continue
        key = (int(d['shot']), bool(d.get('vanilla')))
        if key not in best or os.path.getmtime(p) > os.path.getmtime(best[key]):
            best[key] = p
    return best


def shot_cameras(D, log=print):
    """HairLab shot cameras mapped into the fur's mesh space through the head bone (game head axes ax/ay/az = rows 0-2
    of the head bind pose). ref = the vanilla head crop of that shot when one exists for this character."""
    names = [b['name'] for b in D['bones']]
    if 'head' not in names:
        return {}
    B = np.array(D['bones'][names.index('head')]['bind'], float)
    hp = B[3, :3]
    own = _shot_files(D['character'])
    files = own if any(k[0] in SHOTS for k in own) else _shot_files('guillaume')
    out = {}
    for n in SHOTS:
        p = files.get((n, True)) or files.get((n, False))
        if not p:
            continue
        d = json.load(open(p))
        ax, ay, az = (np.array(d['head'][k], float) for k in ('ax', 'ay', 'az'))
        h = np.array(d['head']['pos'], float)

        def to_mesh(w):
            w = np.asarray(w, float) - h
            return hp + np.array([np.dot(w, ax), np.dot(w, ay), np.dot(w, az)]) @ B[:3, :3]
        eye = to_mesh(d['cam']['pos'])
        tgt = to_mesh(d.get('target') or d['head']['pos'])
        fov = float(d['cam'].get('fov', 30.0))
        fovy = 2 * np.degrees(np.arctan(np.tan(np.radians(fov / 2)) * CROP))
        ref = None
        vb = own.get((n, True))
        if vb and os.path.exists(vb[:-5] + '_head.png'):
            ref = os.path.relpath(vb[:-5] + '_head.png', ROOT)
        # the in-game capture of THIS build (and its magenta twin = exact hair mask), for calibration
        game = gmask = None
        for g in sorted(glob.glob(os.path.join(ROOT, 'results', '*', 'raw', '*_%s_shot%d_head.png' % (D['config'], n))), key=os.path.getmtime):
            game = os.path.relpath(g, ROOT)
            tw = g.replace('_%s_shot' % D['config'], '_%s__m_shot' % D['config'])
            gmask = os.path.relpath(tw, ROOT) if os.path.exists(tw) else None
        out[str(n)] = dict(view=d.get('view'), eye=[round(float(x), 5) for x in eye], target=[round(float(x), 5) for x in tgt],
                           fovy=round(float(fovy), 4), ref=ref, game=game, game_mask=gmask, src=os.path.relpath(p, ROOT))
    log('  shots: %d HairLab cameras (%s), %d vanilla reference stills' % (
        len(out), 'own' if files is own else 'generic guillaume rig', sum(1 for v in out.values() if v['ref'])))
    return out


def build(cfg, fur=None, edit=None, out=None, log=print, rebuild=False):
    fur = fur or canonical_fur(cfg, rebuild=rebuild, log=log)
    if not fur:
        raise SystemExit('no built redfur for %s (build it first: hairlab round --dry / hairpipe build)' % cfg)
    comp = os.path.basename(fur)[:-len('.redfur')].split('__', 1)[1] if '__' in os.path.basename(fur) else None
    edit = edit or find_edit(cfg, comp)
    ed = json.load(open(edit)) if edit else None
    data = open(fur, 'rb').read()
    D = HP.build_pack(ed, data, tex_loader=None, fur_name=os.path.basename(fur), log=lambda *a: None)
    t = HP.parse_fur(data)
    out = out or os.path.join(OUT_BASE, cfg)
    os.makedirs(out, exist_ok=True)
    for old in glob.glob(os.path.join(out, 'tex_*.png')) + glob.glob(os.path.join(out, 'head_d.png')):
        os.remove(old)              # textures of an earlier prep (the pack is rewritten below; export\ is kept)
    C = load_cfg(cfg)
    character = D.get('character') or C.get('lab', {}).get('char') or cfg.split('_')[0]
    D['config'] = cfg
    D['character'] = character
    D['comp'] = comp
    D['format'] = 'deskpack'
    D['prep_version'] = PREP_VERSION
    D['version'] = 1
    D['fur_path'] = _rel(fur)
    D['edit_path'] = _rel(edit) if edit else None
    # staleness: the Studio re-preps when one of these files is newer than desk.json (src/app.cpp packUpToDate)
    D['src_files'] = [_rel(p) for p in src_files(cfg)]
    D['src_hash'] = src_hash(cfg) if D['src_files'] else None
    D['bone_idx'] = [[int(v[k]) for k in 'XYZW'] for v in t['boneIndices']]
    D['bone_w'] = [[round(float(v[k]), 5) for k in 'XYZW'] for v in t['boneWeights']]
    for b, par in zip(D['bones'], t.get('boneParents', [-1] * len(D['bones']))):
        b['parent'] = int(par)
    # edit-frame guides (same order as the fur's guides when the EDIT export belongs to this build)
    eg = ed.get('guides') if ed else None
    if eg and len(eg) == len(D['guides']):
        D['guides_edit'] = eg
    else:
        if ed:
            log('  edit export %s does not match the fur (%d vs %d guides): ops will use the fur frame' % (
                edit, len(eg or []), len(D['guides'])))
        D['guides_edit'] = D['guides']
    # textures -> PNG next to desk.json (full resolution when the game cache is readable)
    D['textures'] = {}
    for i, path in enumerate(HP.imports([D['physical'], D['graphical']])):
        img = HP.game_texture(path)
        if img is None:
            log('  texture %s not available' % path)
            continue
        fn = 'tex_%d.png' % i
        img.convert('RGBA').save(os.path.join(out, fn))
        D['textures'][path] = dict(file=fn, w=img.size[0], h=img.size[1])
        vol = (D['physical'] or {}).get('volume') or {}
        if (vol.get('densityTex') or {}).get('@import') == path:
            ch = vol.get('densityTexChannel', 'RED')
            F = np.array(D['F'], int)
            UV = np.array(D['uv'], float).reshape(-1, 3, 2)
            corner, face = HP.density_maps(F, UV, img, ch)
            D['density'] = dict(tex=path, file=fn, channel=ch, pixel=bool(vol.get('usePixelDensity', 0)),
                                corner=corner, face=face)
    D['mod'] = mod_info(C, character)
    D['look_cfg'] = C.get('look', {})
    # performance reference: the approved build of this character, else this build
    import approved as A                 # APPROVED.json with per-hairstyle entries (2026-10-11)
    ref_cfg = A.approved_for(A.load(), character, C.get('style'))
    ref = None
    if ref_cfg and ref_cfg != cfg:
        rf = find_fur(ref_cfg)
        if rf:
            R = HP.build_pack(None, open(rf, 'rb').read(), tex_loader=None, log=lambda *a: None)
            R['config'] = ref_cfg
            vol = (R['physical'] or {}).get('volume') or {}
            path = (vol.get('densityTex') or {}).get('@import')
            if path:
                img = HP.game_texture(path)
                if img is not None:
                    _, face = HP.density_maps(np.array(R['F'], int), np.array(R['uv'], float).reshape(-1, 3, 2),
                                              img, vol.get('densityTexChannel', 'RED'))
                    R['density'] = dict(face=face)
            ref = budget_of(R)
            ref['kind'] = 'approved'
    if ref is None:
        ref = budget_of(D)
        ref['kind'] = 'approved (this build)' if ref_cfg == cfg else 'this build (no approved build for %s)' % character
    D['budget_ref'] = ref
    D['shots'] = shot_cameras(D, log)
    try:
        D['head_tex'] = head_textured(cfg, out, log)
    except Exception as e:   # game files not readable: the Studio falls back to the untextured EDIT head
        log('  head texture skipped: %r' % (e,))
    try:
        D['outfit_tex'] = outfit_textured(D, C, out, log)
    except Exception as e:   # same fallback for the body / outfit meshes
        log('  outfit textures skipped: %r' % (e,))
    try:
        try:
            D['beard_cards'] = beard_cards(C, out, log)
        except Exception as e:   # a broken beard mesh must not cost the hair cards below
            log('  beard cards: skipped (%r)' % (e,))
            D['beard_cards'] = []
        # vanilla hair cards of the appearance (from the player's own game files): the Studio's "Vanilla hair" view
        try:
            col = (D.get('graphical') or {}).get('color') or {}
            rc, tc = col.get('rootColor') or {}, col.get('tipColor') or {}
            tint = [(rc.get(k, 128) + tc.get(k, 128)) / 2 for k in ('Red', 'Green', 'Blue')] if rc or tc else None
            D['hair_cards'] = beard_cards(C, out, log, kinds=('hair',), part='hair', tint=tint)
        except Exception as e:
            log('  hair cards: skipped (%s)' % e)
            D['hair_cards'] = []
    except Exception as e:   # never breaks a prep; the renderer does not draw these yet (HAIRWORK.md "Beards")
        log('  beard cards skipped: %r' % (e,))
    bf = [f for f in glob.glob(os.path.join(os.path.dirname(fur), cfg + '__*.redfur')) if is_beard_fur(f)]
    if bf:
        BP = HP.build_pack(None, open(bf[0], 'rb').read(), tex_loader=None, fur_name=os.path.basename(bf[0]), log=lambda *a: None)
        Bt = HP.parse_fur(open(bf[0], 'rb').read())
        D['beard_fur'] = dict(fur_path=_rel(bf[0]), guides=BP['guides'], F=BP['F'], uv=BP['uv'], bones=BP['bones'],
                              bone_idx=[[int(v[k]) for k in 'XYZW'] for v in Bt['boneIndices']],
                              bone_w=[[round(float(v[k]), 5) for k in 'XYZW'] for v in Bt['boneWeights']],
                              physical=BP['physical'], graphical=BP['graphical'])
        log('  beard fur: %s (%d guides, %d faces)' % (_rel(bf[0]), len(BP['guides']), len(BP['F'])))
    D['materials'] = materials(log)
    D['budget_self'] = budget_of(D)
    p = os.path.join(out, 'desk.json')
    with open(p, 'w') as fh:
        json.dump(D, fh, separators=(',', ':'))
    log('desk pack: %s (%d guides, %d faces, %d bones, %d spheres, %d capsules, %d textures; ref %s %s hairs, %s close-up)' % (
        _rel(p), len(D['guides']), len(D['F']), len(D['bones']), len(D['spheres']),
        len(D['capsules']), len(D['textures']), ref['config'], ref['hairs'], ref['hairs_close']))
    return p


def _tex_png(rel, out, cache, log, maxsize=1024):
    """game xbm -> PNG in the pack folder (once per texture); returns the file name or None"""
    from texread import load_xbm
    if not rel:
        return None
    if rel in cache:
        return cache[rel]
    name = None
    try:
        img = load_xbm(rel).convert('RGB')
        if max(img.size) > maxsize:
            k = maxsize / max(img.size)
            img = img.resize((max(1, int(img.width * k)), max(1, int(img.height * k))))
        name = 'tex_%s.png' % os.path.splitext(os.path.basename(rel))[0]
        img.save(os.path.join(out, name))
    except Exception as e:
        log('  texture %s skipped: %r' % (rel, e))
    cache[rel] = name
    return name


def head_textured(cfg, out, log=print):
    """the character's real head mesh (LOD0, from the game files) with UVs + its diffuse (+ normal) texture"""
    import hairpipe as H
    import gamefiles as GF
    from texread import load_xbm
    C = H.load_config(cfg)
    info = H.discover(C, log=lambda *a: None)
    h = C.get('head_mesh') or (info.get('heads') or [None])[0]
    if not h:
        return None
    m = GF.load_mesh(h)
    ch = GF.merge_chunks(m, m['lod0'])
    dif = nrm = None
    for _base, prm in GF.materials(h):
        d, n = _mat_maps(prm)
        if d:
            dif, nrm = d, n
            break
    if not dif:
        return None
    img = load_xbm(dif).convert('RGB')
    if max(img.size) > 1024:
        img = img.resize((1024, 1024))
    img.save(os.path.join(out, 'head_d.png'))
    nfile = _tex_png(nrm, out, {}, log) if nrm else None
    idx = np.asarray(ch['idx'], int).reshape(-1, 3)
    log('  head: %s (%d verts, %d tris) + %s%s' % (h, ch['nv'], len(idx), dif, ' + normal' if nfile else ''))
    return dict(file='head_d.png', normal=nfile, mesh=h, v=np.round(ch['pos'], 5).tolist(), uv=np.round(ch['uv'], 5).tolist(),
                t=idx.tolist())


# ------------------------------------------------------------------------------------------------ appearance meshes
# Body / outfit / eye meshes come from the SAME appearance the game (and HairLab's capture: [HairTest] App = the
# config's "appearance") spawns: every CMeshComponent of the appearance's included templates, LOD0 chunks, one part
# per material. Instanced .w2mi materials are resolved through their base chain. What the viewport shader cannot draw
# is baked into the part's albedo PNG (approximations, preview only):
#   * spec-workflow materials (pbr_spec*): metals keep a dark diffuse and get their colour from SpecularTexture x
#     SpecularColor (Guillaume's silver plate / gold trim) -> albedo = lerp(diffuse, specular, metalness ~ spec luminance)
#   * colour shifts: the entity template's coloringEntries (appearance + component -> colorShift1 / 2) applied in HSL
#     to the TintMask regions (R = shift 1, G or B = shift 2), grey kept when "ColorShift_ KeepGray" (Guillaume's
#     red / blue cloth -> dark). Saturation and luminance only: the hue convention is unverified (added as degrees it
#     turned the gold of Oriana's necklace blue, which the game does not), so hue is not applied.
#   * pattern materials (pbr_bob_pattern*): the tiled motif (pattern alpha) tinted with bg_spec (Oriana's brocade)
#   * *_fresnel materials (pearls): albedo pulled towards SpecularColor by FresnelStrenght
# Materials the opaque mesh shader would draw wrongly are skipped: eye shadow, eyelashes, hair cards, transparent /
# decal layers, sheer veils (pbr_voal) and the wet skin_eye overlay (all alpha-blended in game). Not reproduced: detail
# maps, translucency, reflections.
METAL_GAIN = 0.7       # specular colour -> albedo for metals (the game's plate is lit grey-blue, not white)
SKIP_MAT = ('eye_shadow', 'eyeshadow', 'eyelash', 'pbr_hair', 'transparent', 'decal', 'skin_eye', 'glass', 'pbr_voal')
PATTERN_DENSITY = 2.0  # pattern repeats per Pattern_tile unit (matched by eye to Oriana's brocade in the L237 captures)


def _resolve_mat(m):
    """mesh material entry (inline CMaterialInstance or an imported .w2mi) -> (base chain, merged params)"""
    import gamefiles as GF
    chain, layers, cur = [], [], m
    for _ in range(10):
        if not isinstance(cur, dict):
            break
        if '@import' in cur and '@class' not in cur:
            p = cur['@import']
            chain.append(p)
            if not p.endswith('.w2mi'):
                break
            try:
                cur = GF.tree(p)[0]
            except Exception:
                break
            continue
        layers.append(cur.get('@matparams') or {})
        b = (cur.get('baseMaterial') or {}).get('@import')
        if not b:
            break
        chain.append(b)
        if not b.endswith('.w2mi'):
            break
        try:
            cur = GF.tree(b)[0]
        except Exception:
            break
    params = {}
    for L in reversed(layers):          # the instance overrides its base
        params.update(L)
    return chain, params


def _imp(prm, *keys):
    for k in keys:
        v = prm.get(k)
        if isinstance(v, dict) and v.get('@import'):
            return v['@import']
    return None


def _mat_maps(prm):
    return _imp(prm, 'Diffuse', 'Diffusemap', 'diffuse', 'DiffuseTexture'), _imp(prm, 'Normal', 'Normalmap', 'normal')


def _rgb_to_hsl(c):
    mx, mn = c.max(-1), c.min(-1)
    l = (mx + mn) / 2
    d = mx - mn
    s = np.where(d < 1e-6, 0, d / np.maximum(1e-6, 1 - np.abs(2 * l - 1)))
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    dd = np.maximum(d, 1e-6)
    h = np.where(mx == r, ((g - b) / dd) % 6, np.where(mx == g, (b - r) / dd + 2, (r - g) / dd + 4)) * 60.0
    return np.where(d < 1e-6, 0, h), np.clip(s, 0, 1), l


def _hsl_to_rgb(h, s, l):
    c = (1 - np.abs(2 * l - 1)) * s
    hp = (h % 360) / 60.0
    x = c * (1 - np.abs(hp % 2 - 1))
    z = np.zeros_like(h)
    i = (np.floor(hp).astype(int) % 6)[..., None]
    rgb = np.select([i == k for k in range(6)],
                    [np.stack(v, -1) for v in ((c, x, z), (x, c, z), (z, c, x), (z, x, c), (x, z, c), (c, z, x))])
    return rgb + (l - c / 2)[..., None]


def _shift(c, cs):
    """CColorShift on RGB 0..1: saturation / luminance -100..100 scaled towards 0 / 1 (hue left as is, see above)"""
    h, s, l = _rgb_to_hsl(c)
    sa, lu = float(cs.get('saturation', 0)) / 100.0, float(cs.get('luminance', 0)) / 100.0
    s = s * (1 + sa) if sa < 0 else s + (1 - s) * sa
    l = l * (1 + lu) if lu < 0 else l + (1 - l) * lu
    return np.clip(_hsl_to_rgb(h, np.clip(s, 0, 1), np.clip(l, 0, 1)), 0, 1)


def _load_rgb(rel, size):
    from texread import load_xbm
    from PIL import Image
    im = load_xbm(rel).convert('RGB')
    if im.size != size:
        im = im.resize(size, Image.BILINEAR)
    return np.asarray(im, np.float32) / 255.0


def _albedo(dif, graph, prm, cshift, out, cache, log, maxsize=1024):
    """diffuse PNG of a part with the spec-workflow metal colour and the colour shift baked in -> file name or None"""
    import hashlib
    from PIL import Image
    spec = _imp(prm, 'SpecularTexture') if 'pbr_spec' in graph else None
    pattern = _imp(prm, 'pattern') if 'pattern' in graph else None
    mask = _imp(prm, 'TintMask')
    use_shift = bool(cshift and mask and float(prm.get('ColorShift_Enabled', 1.0)) > 0)
    fres = float(prm.get('FresnelStrenght', 0) or 0) if 'fresnel' in graph else 0.0
    if not spec and not use_shift and not pattern and not fres:
        return _tex_png(dif, out, cache, log, maxsize)
    key = json.dumps([dif, spec, pattern, prm.get('Pattern_tile') if pattern else None, METAL_GAIN, PATTERN_DENSITY, fres,
                      prm.get('SpecularColor') if spec else None, mask if use_shift else None,
                      cshift if use_shift else None, prm.get('ColorShift_ KeepGray') if use_shift else None],
                     sort_keys=True, default=str)
    if key in cache:
        return cache[key]
    name = None
    try:
        from texread import load_xbm
        img = load_xbm(dif).convert('RGB')
        if max(img.size) > maxsize:
            k = maxsize / max(img.size)
            img = img.resize((max(1, int(img.width * k)), max(1, int(img.height * k))))
        c = np.asarray(img, np.float32) / 255.0
        if spec:
            sc = prm.get('SpecularColor') or {}
            scol = np.array([sc.get('Red', 255), sc.get('Green', 255), sc.get('Blue', 255)], np.float32) / 255.0
            S = _load_rgb(spec, img.size) * scol
            lum = S @ np.array([0.299, 0.587, 0.114], np.float32)
            m = np.clip((lum - 0.22) / 0.33, 0, 1)[..., None]           # metalness proxy: bright specular = metal
            c = c * (1 - m) + S * (METAL_GAIN * m)
        if pattern:
            # pbr_bob_pattern*: a tiled pattern (normal map + motif in alpha, Pattern_tile) whose motif carries bg_spec
            # (Oriana's gold brocade on purple) -> motif x bg_spec mixed into the albedo, tiled in the diffuse's UV space
            from PIL import Image as _I
            t = prm.get('Pattern_tile') or {}
            tx, ty = (max(1, int(round(float(t.get(k, 1)) * PATTERN_DENSITY))) for k in ('X', 'Y'))
            pw = max(8, img.size[0] // tx), max(8, img.size[1] // ty)
            from texread import load_xbm as _lx
            P = np.asarray(_lx(pattern).convert('RGBA').resize(pw, _I.BILINEAR), np.float32) / 255.0
            rel = np.clip(P[..., 3] * 4.0, 0, 1)            # pattern alpha = the motif (0 / ~0.25)
            rel = np.tile(rel, (ty + 1, tx + 1))[:img.size[1], :img.size[0]]
            bs = prm.get('bg_spec') or {}
            gold = np.array([bs.get('Red', 200), bs.get('Green', 170), bs.get('Blue', 40)], np.float32) / 255.0
            c = c * (1 - rel[..., None] * 0.35) + gold * 0.6 * (rel[..., None] * 0.35)
        if fres:
            # *_fresnel materials (pearls): the sheen is a fresnel-weighted SpecularColor -> albedo pulled towards it
            sc = prm.get('SpecularColor') or {}
            scol = np.array([sc.get('Red', 200), sc.get('Green', 200), sc.get('Blue', 200)], np.float32) / 255.0
            k = min(0.8, fres)
            c = c * (1 - k) + np.maximum(scol, c) * k
        if use_shift:
            A = _load_rgb(mask, img.size)
            w1 = A[..., 0]
            w2 = np.maximum(A[..., 1], A[..., 2]) * (1 - w1)      # masks use G or B for the second region
            if float(prm.get('ColorShift_ KeepGray', 0)) > 0:
                _h, s0, _l = _rgb_to_hsl(c)
                g = np.clip(s0 / 0.15, 0, 1)                             # grey pixels keep their colour
                w1, w2 = w1 * g, w2 * g
            c1 = _shift(c, cshift['colorShift1']) if cshift.get('colorShift1') else c
            c2 = _shift(c, cshift['colorShift2']) if cshift.get('colorShift2') else c
            c = c * (1 - w1 - w2)[..., None] + c1 * w1[..., None] + c2 * w2[..., None]
        name = 'tex_%s__%s.png' % (os.path.splitext(os.path.basename(dif))[0], hashlib.sha1(key.encode()).hexdigest()[:8])
        Image.fromarray(np.clip(c * 255 + 0.5, 0, 255).astype(np.uint8)).save(os.path.join(out, name))
    except Exception as e:
        log('  albedo bake of %s skipped (%r): plain diffuse' % (dif, e))
        name = _tex_png(dif, out, cache, log, maxsize)
    cache[key] = name
    return name


def appearance_components(C, log=print):
    """[(component name, mesh, kind)] of the config's appearance (the one the game / HairLab capture spawns) +
    {component name: coloringEntry shifts} of that appearance"""
    import discover as DI
    import gamefiles as GF
    tpl = C.get('template')
    if not tpl:
        return [], {}
    apps = DI.appearances(tpl)
    app = C.get('appearance') or next(iter(apps), '')
    if app not in apps:
        log('  appearance %r not in %s: first one used' % (app, tpl))
        app = next(iter(apps), '')
    comps, seen = [], set()
    for t in apps.get(app, []):
        for name, _cls, mesh, _fur in DI.mesh_comps(t):
            if mesh.lower() in seen:
                continue
            seen.add(mesh.lower())
            comps.append((name, mesh, DI.classify(mesh)))
    shifts = {}
    for e in (GF.tree(tpl)[0].get('coloringEntries') or []):
        if e.get('appearance') == app and e.get('componentName'):
            shifts[e['componentName']] = dict(colorShift1=e.get('colorShift1'), colorShift2=e.get('colorShift2'))
    return comps, shifts


def outfit_textured(D, C, out, log=print):
    """every visible mesh of the character's appearance except the head (head_textured) and the hair / beard cards:
    body, outfit, items and the eyes, LOD0, one textured part per material (see the block comment above)."""
    import gamefiles as GF
    comps, shifts = appearance_components(C, log)
    if not comps:          # no template in the config: the EDIT export's outfit meshes
        comps = [(os.path.basename(M['path'])[:-7], M['path'], 'body') for M in D.get('meshes', [])
                 if M.get('kind') == 'outfit' and M.get('path')]
    head = (D.get('head_tex') or {}).get('mesh') or C.get('head_mesh')
    parts, cache = [], {}
    for comp, rel, kind in comps:
        if kind in ('hair', 'beard', 'head') or rel == head or 'shadowmesh' in rel.lower():
            continue
        try:
            m = GF.load_mesh(rel)
            mats = GF.tree(rel)[0].get('materials', [])
        except Exception as e:
            log('  %s skipped: %r' % (rel, e))
            continue
        groups, skipped = {}, set()
        for ci in m['lod0']:
            mi = m['chunks'][ci]['mat']
            if mi < len(mats):
                groups.setdefault(mi, []).append(ci)
        n = 0
        for mi, ids in sorted(groups.items()):
            chain, prm = _resolve_mat(mats[mi])
            names = ' '.join(chain).lower()
            if any(k in names for k in SKIP_MAT):
                skipped.add(os.path.basename(chain[0]) if chain else '?')
                continue
            dif, nrm = _mat_maps(prm)
            if not dif:
                skipped.add('%s (no diffuse)' % (os.path.basename(chain[0]) if chain else mi))
                continue
            graph = chain[-1].lower() if chain else ''
            dfile = _albedo(dif, graph, prm, shifts.get(comp), out, cache, log)
            if not dfile:
                continue
            nfile = _tex_png(nrm, out, cache, log) if nrm else None
            ch = GF.merge_chunks(m, ids)
            idx = np.asarray(ch['idx'], int).reshape(-1, 3)
            parts.append(dict(name=comp, path=rel, part=kind, file=dfile, normal=nfile, v=np.round(ch['pos'], 5).tolist(),
                              uv=np.round(ch['uv'], 5).tolist(), t=idx.tolist()))
            n += 1
        log('  %s: %s (%d textured parts%s%s)' % (kind, rel, n, ', colour shift' if comp in shifts else '',
                                                 '; skipped ' + ', '.join(sorted(skipped)) if skipped else ''))
    return parts


def beard_cards(C, out, log=print, kinds=('beard',), part='beard', tint=None):
    """2026-10-11: the vanilla beard / moustache cards (hb_* pbr_hair meshes) of the config's appearance, LOD0, one part
    per material, diffuse saved WITH its alpha (tex_<name>_rgba.png): alpha-tested cards. outfit_textured skips them
    (pbr_hair). Data only: drawing them needs an alpha-test hook in the renderer (not added yet)."""
    import gamefiles as GF
    from texread import load_xbm
    comps, _shifts = appearance_components(C, log)
    parts, cache = [], {}
    for comp, rel, kind in comps:
        if kind not in kinds:
            continue
        m = GF.load_mesh(rel)
        mats = GF.tree(rel)[0].get('materials', [])
        groups = {}
        for ci in m['lod0']:
            groups.setdefault(m['chunks'][ci]['mat'], []).append(ci)
        for mi, ids in sorted(groups.items()):
            if mi >= len(mats):
                continue
            _chain, prm = _resolve_mat(mats[mi])
            dif, _nrm = _mat_maps(prm)
            if not dif:
                continue
            if dif not in cache:
                img = load_xbm(dif).convert('RGBA')
                if max(img.size) > 1024:
                    img = img.resize((1024, 1024))
                if tint is not None:
                    # vanilla hair textures are grey, tinted in game by the appearance; no colour shift for the hair
                    # component in the template, so use the groom's root / tip colour (normalised, keeps brightness)
                    from PIL import Image as _I
                    a = np.asarray(img, np.float32)
                    lum = a[..., :3] @ np.array([0.299, 0.587, 0.114], np.float32)
                    t = np.asarray(tint, np.float32)
                    a[..., :3] = np.clip(lum[..., None] * (t / max(t.max(), 1e-3))[None, None, :], 0, 255)
                    img = _I.fromarray(a.astype(np.uint8), 'RGBA')
                cache[dif] = 'tex_%s_%s_rgba.png' % (os.path.splitext(os.path.basename(dif))[0], part)
                img.save(os.path.join(out, cache[dif]))
            ch = GF.merge_chunks(m, ids)
            parts.append(dict(name=comp, path=rel, part=part, alpha=True, alpha_ref=0.3, file=cache[dif],
                              v=np.round(ch['pos'], 5).tolist(), uv=np.round(ch['uv'], 5).tolist(),
                              t=np.asarray(ch['idx'], int).reshape(-1, 3).tolist()))
        log('  %s cards: %s (%d parts)' % (part, rel, len([p for p in parts if p['path'] == rel])))
    return parts


def build_fur(cfg, log=print):
    """builds the config's fur with the pipeline (hairpipe.build with assemble=True: nothing is installed or written to
    mods) -> build/desktop_studio/furs/<cfg>__<comp>.redfur (+ the EDIT export hairpipe writes as usual). -> paths"""
    import hairpipe as H
    C = H.load_config(cfg)
    files, _bufs, _tpls = H.build(C, log=log, assemble=True)
    os.makedirs(FUR_DIR, exist_ok=True)
    built = []
    for rel, data in files:
        if rel.endswith('.redfur'):
            p = os.path.join(FUR_DIR, '%s__%s' % (cfg, os.path.basename(rel)))
            open(p, 'wb').write(data)
            log('built ' + _rel(p))
            built.append(p)
    if not built:
        raise SystemExit('the build produced no redfur')
    # pack-local mod source for desk_export (redfur path + patched w2ent templates), used when mods/<mod> does not exist
    from w3lib import write_legacy_bundle
    keep = [(r, d) for r, d in files if r.endswith(('.redfur', '.w2ent'))]
    bdir = os.path.join(OUT_BASE, 'modsrc', C['mod'], 'content')
    os.makedirs(bdir, exist_ok=True)
    write_legacy_bundle(os.path.join(bdir, 'blob0.bundle'), keep)
    return built


# ------------------------------------------------------------------------------------------------ character catalog
STATUS_ORDER = {'APPROVED': 0, 'FINAL': 1, 'PICK': 2, 'IN PROGRESS': 3, 'ON HOLD': 4, 'NOT STARTED': 5}
FEMALE = ('sheala', 'yennefer', 'triss', 'keira', 'shani', 'ves', 'iris', 'oriana', 'margarita', 'anna', 'fringilla', 'tamara',
          'vivienne', 'priscilla', 'ciri', 'philippa', 'syanna', 'anarietta')
GENERIC = ('c04ma', 'bowl', 'noble', 'festival', 'h106', 'chappelle', 'soltis', 'elihal')


def _all_furs():
    furs = {}
    pats = [os.path.join(ROOT, 'build', 'desktop_studio', 'furs', '*__*.redfur'),
            os.path.join(ROOT, 'build', 'hairlab', '*', 'furs', '*__*.redfur'),
            os.path.join(ROOT, 'build', 'hairbatch', 'furs', '*__*.redfur'),
            os.path.join(ROOT, 'results', '*', '*__*.redfur')]
    for pat in pats:
        for p in glob.glob(pat):
            cfg, comp = os.path.basename(p)[:-len('.redfur')].split('__', 1)
            if is_beard_fur(p):
                continue                 # the beard fur of a "beard" config is not the config's groom
            t = os.path.getmtime(p)
            if cfg not in furs or t > furs[cfg][1]:
                furs[cfg] = (p, t, comp)
    return furs


def _characters_md():
    import re
    p = os.path.join(ROOT, '.claude', 'skills', 'witcher-3-hairwork', 'references', 'characters.md')
    out = {}
    if not os.path.exists(p):
        return out
    for line in open(p, encoding='utf-8'):
        if not line.startswith('| ') or line.startswith('| Character') or line.startswith('|---'):
            continue
        cols = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cols) < 3:
            continue
        cfgs = re.findall(r'`([a-z0-9_]+)`', cols[1])
        st = cols[2].lower()
        status = ('APPROVED' if 'approved' in st else
                  'FINAL' if ('settled' in st or 'user ok' in st or 'good now' in st) else
                  'ON HOLD' if ('paused' in st or 'not possible' in st or 'parked' in st) else
                  'PICK' if 'pick' in st else 'IN PROGRESS')
        key = cfgs[0].split('_')[0] if cfgs else re.sub(r'[^a-z0-9]', '', cols[0].lower().split('(')[0])
        out[key] = dict(name=cols[0].split('(')[0].strip(), cfgs=cfgs, status=status, issues=cols[3] if len(cols) > 3 else '')
    return out


PORTRAIT = (300, 400)      # 3:4 head-and-shoulders card portrait


def _frame(im, kind):
    """3:4 head-and-shoulders window with the face centred. kind: 'head' = HairLab head crop (1400 px square, head in the
    middle), 'pairL' / 'pairR' = one half of a PAIR image (same framing, scaled), 'gallery' = a gallery cell"""
    from PIL import Image
    w, h = im.size
    if kind == 'pairL':
        im = im.crop((0, 0, w // 2, h)); w //= 2
    elif kind == 'pairR':
        im = im.crop((w // 2, 0, w, h)); w = w - w // 2
    if kind == 'gallery':
        cw = int(min(w, h * 0.75))
        x0, y0 = (w - cw) // 2, 0
        ch = min(h, int(cw * 4 / 3))
    else:
        cw = int(w * 0.66)          # head and shoulders
        ch = int(cw * 4 / 3)
        x0, y0 = (w - cw) // 2, int(h * 0.08)
        ch = min(ch, h - y0)
    return im.crop((x0, y0, x0 + cw, y0 + ch)).convert('RGB').resize(PORTRAIT, Image.LANCZOS)


def _newest(paths):
    paths = [p for p in paths if os.path.exists(p)]
    return max(paths, key=os.path.getmtime) if paths else None


def _gallery_img(char):
    for d in ('artifact_gallery_new', 'artifact_gallery'):
        g = os.path.join(ROOT, 'analysis', d, 'img', char + '.jpg')
        if os.path.exists(g):
            return g
    return None


def _thumb(char, cfgs, tdir):
    """card portraits from OUR images only (HairLab captures, PAIR shots, gallery sheets): build/desktop_studio/thumbs/
    <char>_ours.jpg (newest in-game front head crop of the character's builds, best build first) and <char>_vanilla.jpg
    (newest vanilla baseline front crop). -> {ours: path|None, vanilla: path|None, src_ours, src_vanilla}"""
    from PIL import Image
    os.makedirs(tdir, exist_ok=True)
    res = {}
    raw = os.path.join(ROOT, 'results', '*', 'raw')
    # ours: the first config (default / best / pick / newest variants) that has a front capture
    ours = None
    for cfg in cfgs:
        ours = _newest(glob.glob(os.path.join(raw, '*_%s_shot4_head.png' % cfg)))
        if ours:
            ours = (ours, 'head', 'capture ' + cfg)
            break
        pr = _newest(glob.glob(os.path.join(ROOT, 'results', '*', 'pairs', cfg + '_front_PAIR.jpg')))
        if pr:
            ours = (pr, 'pairR', 'pair ' + cfg)
            break
    van = _newest(glob.glob(os.path.join(raw, '*_%s_baseline_shot4_head.png' % char)))
    van = (van, 'head', 'vanilla capture') if van else None
    g = _gallery_img(char)
    if van is None and g is None:
        for cfg in cfgs:
            pr = _newest(glob.glob(os.path.join(ROOT, 'results', '*', 'pairs', cfg + '_front_PAIR.jpg')))
            if pr:
                van = (pr, 'pairL', 'vanilla half of pair ' + cfg)
                break
    if g:
        # gallery sheet: columns = rounds (vanilla first, newest last), first row = front 3/4
        if ours is None:
            ours = (g, 'gallery_last', 'gallery (newest round)')
        if van is None:
            van = (g, 'gallery_first', 'gallery (vanilla column)')
    for key, src in (('ours', ours), ('vanilla', van)):
        if not src:
            res[key] = None
            continue
        out = os.path.join(tdir, '%s_%s.jpg' % (char, key))
        if not (os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(src[0])):
            try:
                im = Image.open(src[0])
                if src[1].startswith('gallery'):
                    w, h = im.size
                    cell = h // 3
                    cw = int(cell * 0.93)
                    top = int(h * 0.018)        # skip the column label strip
                    im = im.crop((w - cw, top, w, cell) if src[1] == 'gallery_last' else (0, top, cw, cell))
                    im = _frame(im, 'gallery')
                else:
                    im = _frame(im, src[1])
                im.save(out, quality=88)
                if max(im.convert('L').getextrema()) < 25:     # a black / failed frame: no portrait from it
                    raise ValueError('dark frame')
            except Exception:
                res[key] = None
                continue
        res[key] = os.path.relpath(out, ROOT)
        res['src_' + key] = src[2]
    return res


def _style_of_cfgs(cfgs):
    """{cfg: hairstyle id} from the optional "style" config key (missing = "default")"""
    out = {}
    for c in cfgs:
        try:
            out[c] = json.load(open(os.path.join(HW, 'chars', c + '.json'), encoding='utf-8-sig')).get('style') or 'default'
        except Exception:
            out[c] = 'default'
    return out


def catalog(log=print):
    import time
    import approved as A
    furs = _all_furs()
    ad = A.load()
    approved = A.default_map(ad)           # the default hairstyle's approved build (the old "approved" strings)
    astyles = A.style_map(ad)              # {char: {style: cfg}} incl. "approved_styles"
    md = _characters_md()
    states = {}
    for p in glob.glob(os.path.join(HW, 'lab', 'state', '*.json')):
        b = os.path.basename(p)[:-5]
        if b.startswith(('bind_', 'prevcol_')) or b == 'README':
            continue
        try:
            states[b] = json.load(open(p))
        except Exception:
            pass
    allcfg = [os.path.basename(p)[:-5] for p in glob.glob(os.path.join(HW, 'chars', '*.json'))]
    cstyle = _style_of_cfgs(allcfg)
    tdir = os.path.join(OUT_BASE, 'thumbs')
    # tools/hairworks/roster.json (roster.py, 2026-10-11): every named story character with its hair type; characters
    # not worked on yet are listed as NOT STARTED so the picker shows the whole cast (filterable by hair_type)
    try:
        roster = json.load(open(os.path.join(HW, 'roster.json'), encoding='utf-8')).get('characters', {})
    except Exception:
        roster = {}
    out = []
    for ch in sorted(set(approved) | set(md) | set(states) | set(roster)):
        ro = roster.get(ch, {})
        if not ro and not any(c.split('_')[0] == ch for c in allcfg):
            continue                      # state files that are not characters (prevcal, ...)
        m = md.get(ch, {})
        if ro.get('status') == 'not_started' and ch not in approved and ch not in states:
            m = dict(m, status='NOT STARTED')      # only the roster's portrait baseline exists
        st = states.get(ch, {})
        best = (st.get('best') or {}).get('cfg')
        pick = next((c for c in m.get('cfgs', []) if c in furs), (m.get('cfgs') or [None])[0])
        ap = approved.get(ch)
        status = 'APPROVED' if ap else m.get('status') or ('IN PROGRESS' if st else 'NOT STARTED')
        if status == 'APPROVED' and not ap and pick:
            ap = pick                      # user-approved in characters.md, not yet in APPROVED.json (Keira)
        # approved > the user's pick (characters.md) for approved / final / pick; for characters still in progress the
        # newer of that pick and the round loop's best (lab/state best = automatic score best, not the user's pick)
        if ap:
            default = ap
        elif status in ('APPROVED', 'FINAL', 'PICK') and pick:
            default = pick
        else:
            cands = [c for c in (pick, best) if c]
            default = max(cands, key=lambda c: furs[c][1] if c in furs else 0) if cands else None
        cf = sorted([c for c in allcfg if c.split('_')[0] == ch and not (ro.get('status') == 'not_started' and
                                                                        c.endswith(('_baseline', '_baseline__m')))],
                    key=lambda c: -(furs[c][1] if c in furs else 0))
        # hairstyles (2026-10-11): configs without a "style" key are the default hairstyle. The character-level
        # fields (default / approved / pick / best / thumbs) describe the DEFAULT hairstyle; `variants` lists all.
        if any(cstyle.get(c, 'default') != 'default' for c in cf):
            dcf = [c for c in cf if cstyle.get(c, 'default') == 'default']
            if pick and cstyle.get(pick, 'default') != 'default':
                pick = next((c for c in m.get('cfgs', []) if c in dcf and c in furs), next((c for c in m.get('cfgs', []) if c in dcf), None))
            if best and cstyle.get(best, 'default') != 'default':
                best = None
            if default and cstyle.get(default, 'default') != 'default':
                cands = [c for c in (pick, best) if c]
                default = max(cands, key=lambda c: furs[c][1] if c in furs else 0) if cands else None
        variants = [dict(cfg=c, built=c in furs, time=furs[c][1] if c in furs else 0, approved=c == ap,
                         pick=c in m.get('cfgs', []), best=c == best,
                         prepped=os.path.exists(os.path.join(OUT_BASE, c, 'desk.json'))) for c in cf[:80]]
        rounds = st.get('rounds') or []
        last = rounds[-1].get('time') if rounds else None
        if not last and cf and cf[0] in furs:
            last = time.strftime('%Y-%m-%d %H:%M', time.localtime(furs[cf[0]][1]))
        comp = furs[default][2] if default in furs else next((furs[c][2] for c in cf if c in furs), '')
        th = _thumb(ch, [c for c in [default, best, pick] + cf[:12] if c], tdir)
        if ro and 'portrait' in ro and ro['portrait'] is None and not cf:
            th = {}                       # roster: no usable vanilla capture (Uma: empty frames), keep the silhouette
        # painted picker portrait (make_portraits.py, git-ignored build output) when it exists
        painted = os.path.join(OUT_BASE, 'portraits_ai', ch + '.jpg')
        out.append(dict(char=ch, name=m.get('name') or ro.get('name') or ch.capitalize(), status=status,
                        issues=m.get('issues', '') or ro.get('note', ''),
                        default=default, approved=ap, pick=pick, best=best, last_round=last,
                        female=ro['female'] if 'female' in ro else ('_w' in comp or comp.startswith(('cwa', 'wa', 'sh')) or ch in FEMALE),
                        generic=ch in GENERIC,
                        hair_type=ro.get('hair_type'), part=ro.get('part'), roster=bool(ro),
                        thumb=th.get('ours') or th.get('vanilla'), thumb_ours=th.get('ours'), thumb_vanilla=th.get('vanilla'),
                        thumb_painted=os.path.relpath(painted, ROOT) if os.path.exists(painted) else None,
                        thumb_src=dict(ours=th.get('src_ours'), vanilla=th.get('src_vanilla')), variants=variants, n_configs=len(cf)))
        for v in variants:
            v['style'] = cstyle.get(v['cfg'], 'default')
        out[-1]['hairstyles'] = _hairstyles(ch, out[-1], ro, cf, cstyle, astyles.get(ch, {}), m, best, furs, tdir)
    out.sort(key=lambda r: (r['generic'], STATUS_ORDER.get(r['status'], 9), r['name']))
    os.makedirs(OUT_BASE, exist_ok=True)
    p = os.path.join(OUT_BASE, 'catalog.json')
    json.dump(dict(characters=out, time=time.strftime('%Y-%m-%d %H:%M')), open(p, 'w'), indent=1)
    log('catalog: %d characters -> %s' % (len(out), os.path.relpath(p, ROOT)))


def _hairstyles(ch, rec, ro, cf, cstyle, ap_styles, m, best, furs, tdir):
    """per-hairstyle view of one character (Character -> Hairstyle -> iterations). Styles = roster.json `hairstyles`
    (default first) + any style id used by a config's "style" key. The default style repeats the character-level
    fields; another style gets its own variants (same records as the character's `variants`), approved
    (APPROVED.json approved_styles), pick (a characters.md pick of that style), best (lab state), default, status
    (NOT STARTED = no config: view only) and portraits (portraits_ai/<char>__<id>.jpg, thumbs/<char>_<id>_*.jpg)."""
    import time
    meta = [dict(s) for s in (ro.get('hairstyles') or []) if isinstance(s, dict) and s.get('id')]
    if not meta or meta[0]['id'] != 'default':
        meta = [s for s in meta if s['id'] != 'default']
        meta.insert(0, dict(id='default', name='Default', appearance_or_component=ro.get('appearance')))
    ids = [s['id'] for s in meta]
    for c in cf:
        sid = cstyle.get(c, 'default')
        if sid not in ids:
            ids.append(sid)
            meta.append(dict(id=sid, name=sid.replace('_', ' ').capitalize(), appearance_or_component=None))
    out = []
    for s in meta:
        sid = s['id']
        keep = {k: s.get(k) for k in ('id', 'name', 'appearance_or_component', 'component', 'appearances', 'template',
                                      'portrait_key') if s.get(k) is not None}
        # per-style hair type (roster `hair_type`, or Geralt's `hair_kind`: native_hairworks / cards); the picker
        # falls back to the character's hair_type when a style has none
        if s.get('hair_type') or s.get('hair_kind'):
            keep['hair_type'] = s.get('hair_type') or s.get('hair_kind')

        def _exists(rel):
            return rel if rel and os.path.exists(os.path.join(ROOT, rel)) else None
        pkey = s.get('portrait_key') or ('%s__%s' % (ch, sid))
        pp = os.path.join(OUT_BASE, 'portraits_ai', pkey + '.jpg')
        if sid == 'default':
            # the card shows the default style: a painting / vanilla crop made for it (roster portrait_key, e.g. Geralt's
            # geralt__half_with_tail) fills the character's own portrait when that is missing
            if not rec['thumb_painted']:
                rec['thumb_painted'] = _exists(s.get('thumb_painted')) or (os.path.relpath(pp, ROOT) if s.get('portrait_key') and os.path.exists(pp) else None)
            if not rec['thumb_vanilla'] and _exists(s.get('thumb_vanilla')):
                rec['thumb_vanilla'] = s['thumb_vanilla']
                rec['thumb'] = rec['thumb'] or rec['thumb_vanilla']
            out.append(dict(keep, status=rec['status'], default=rec['default'], approved=rec['approved'], pick=rec['pick'],
                            best=rec['best'], last_round=rec['last_round'], thumb_painted=rec['thumb_painted'],
                            thumb_ours=rec['thumb_ours'], thumb_vanilla=rec['thumb_vanilla'],
                            variants=[v for v in rec['variants'] if v.get('style', 'default') == 'default']))
            continue
        scf = [c for c in cf if cstyle.get(c, 'default') == sid]
        ap = ap_styles.get(sid)
        pick = next((c for c in m.get('cfgs', []) if c in scf), None)
        sbest = best if best in scf else None
        default = ap or pick or (max(scf, key=lambda c: furs[c][1] if c in furs else 0) if scf else None)
        status = 'APPROVED' if ap else 'PICK' if pick else 'IN PROGRESS' if scf else 'NOT STARTED'
        last = time.strftime('%Y-%m-%d %H:%M', time.localtime(furs[scf[0]][1])) if scf and scf[0] in furs else None
        th = _thumb('%s_%s' % (ch, sid), [c for c in [default] + scf[:12] if c], tdir)
        painted = _exists(s.get('thumb_painted')) or (os.path.relpath(pp, ROOT) if os.path.exists(pp) else None)
        vanilla = _exists(s.get('thumb_vanilla')) or th.get('vanilla')
        out.append(dict(keep, status=status, default=default, approved=ap, pick=pick, best=sbest, last_round=last,
                        thumb_painted=painted, thumb_ours=th.get('ours'), thumb_vanilla=vanilla,
                        variants=[dict(cfg=c, built=c in furs, time=furs[c][1] if c in furs else 0, approved=c == ap,
                                       pick=c == pick, best=c == sbest, style=sid,
                                       prepped=os.path.exists(os.path.join(OUT_BASE, c, 'desk.json'))) for c in scf[:80]]))
    return out


def list_cfgs():
    seen = {}
    for p in glob.glob(os.path.join(ROOT, 'build', 'hairlab', '*', 'furs', '*__*.redfur')) + \
            glob.glob(os.path.join(ROOT, 'build', 'hairbatch', 'furs', '*__*.redfur')):
        cfg = os.path.basename(p).split('__', 1)[0]
        seen[cfg] = max(seen.get(cfg, 0), os.path.getmtime(p))
    import approved as A
    appr = A.all_cfgs()
    for cfg in sorted(seen, key=lambda c: (c not in appr, c)):
        print(('* ' if cfg in appr else '  ') + cfg)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('cfg', nargs='?')
    ap.add_argument('--fur')
    ap.add_argument('--edit')
    ap.add_argument('--out')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--catalog', action='store_true', help='character catalog for the Studio start screen')
    ap.add_argument('--build', action='store_true', help='rebuild the fur with hairpipe even when it is current (nothing is installed)')
    ap.add_argument('--check', action='store_true', help='exit 0 when the pack of <cfg> is current, 1 when it needs a re-prep')
    a = ap.parse_args()
    if a.catalog:
        catalog()
    elif a.check and a.cfg:
        why = pack_stale(a.cfg, a.out)
        print(why or 'current')
        sys.exit(1 if why else 0)
    elif a.build and a.cfg:
        build(a.cfg, a.fur, a.edit, a.out, rebuild=True)
    elif a.list or not a.cfg:
        list_cfgs()
    else:
        build(a.cfg, a.fur, a.edit, a.out)
