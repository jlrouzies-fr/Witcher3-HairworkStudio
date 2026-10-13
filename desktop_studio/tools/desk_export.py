r"""desk_export - package a Desktop Groom Studio edit into a ready mod folder, starting from the character's OWN build.

  py desktop_studio\tools\desk_export.py --cfg <cfg> --edits <x_edits.json> --look <x_look.json> --geo <x_geo.json>
                                         --mod <modName> --tag <tag> [--limit 1.2] [--stiffness <map.png>]

What it does (nothing is installed; output = build\desktop_studio\<cfg>\export\):
  1. Loads the build's own redfur (desk_prep.find_fur: build\hairlab\<round>\furs\<cfg>__<comp>.redfur). Everything the
     Studio did not touch stays byte-for-byte: bone table, collision, physicalMaterials / graphicalMaterials /
     levelOfDetail / materialSets, density texture import, splineMultiplier.
  2. Geometry: only when the edit list is not empty, the guides / growth faces / UVs / bone weights / bounding box are
     replaced by what the Studio shows (<x>_geo.json, fur frame).
  3. Look: only the values in <x>_look.json "set" (= values that differ from the build) are written, to the top level
     and to materialSets (as redfur.write does). The CR2W type of each value is taken from the fur itself (or from the
     material source fur of the preset), never guessed when it exists.
  4. Stiffness map (opt-in, only when painted): an inline DXT1 xbm (R = stiffness, G = root stiffness) next to the
     redfur in the bundle, referenced by physicalMaterials.stiffness.stiffnessTex / rootStiffnessTex with
     stiffnessTexChannel RED / rootStiffnessTexChannel GREEN (fields of the 5.0 CFurMeshResource; vanilla
     woman_succubus growthmesh uses them). Because the build's UVs come from the card atlas (many faces share texels),
     a painted map switches the fur to UNIQUE per-face UVs: the Studio re-bakes the density map (and the colour map when
     the fur uses one) into that atlas (--density-png / --colour-png) and they ship as two more inline xbm.
  5. Budget guard: refuses when the Studio's hair estimate (close-up) is above --limit x the reference build.
     Performance guard (2026-10-13, tools/hairworks/perf_budget.py): when the geometry is replaced, refuses on invalid
     solver input (zero-length segments, guides under 5 mm, shared roots, zero-area growth faces: Crach ~0.1 FPS and
     Dettlaff ~2 FPS in game, L255) or hair metres over the cap; `py tools\hairworks\perf_budget.py ops <geo>` writes
     the fix ops. --allow-perf exports anyway (not for releases).
  6. Packages <mod>\content\blob0.bundle + metadata.store with w3lib (the pipeline's writer) + the character's w2ent
     from its release mod (mods\<mod>), INSTALL_INSTRUCTIONS.txt, and a zip. Also writes <char>_<tag>_config.json: the
     build's config + "edits" + the look changes as look.extra_set keys (a pipeline config proposal; rename it to a new
     <char>_l<round>_<tag> before using it in a HairLab plan). A build of a non-default hairstyle ("style" config key)
     uses <char>_<style> in those names: <char>_<style>_<tag>_config.json, variant <char>_<style>_l<round>_<tag>.
  7. --release (opt-in, "Export for release (requires Core)" in the Studio): the same self-contained mod (same redfur
     and textures as the dev export) plus a requirement script content\scripts\local\hwfa_req_<mod>_<crc>.ws that calls
     the "Hairwork for All - Core" functions (core/hwcore.py: HairworkForAllCore_Required_GetItFromNexus13996 and
     HairworkForAllCore_v<major>_<minor>): without the Core the game's script compiler fails naming the missing
     function, so the game will refuse to start without the Core. REQUIREMENTS.txt and hairworkforall_mod.json
     ("requires_core": <version>) are written next to content\, and INSTALL_INSTRUCTIONS.txt gets the requirement line.
     Second requirement (2026-10-11): Graphics Control (Nexus) with PT hair > 'Hair root transparency mode' = 0
     (hwcore.gc_requirement_line; path-traced hair shows see-through gaps in mode 1), in all three files.
     Without --release the export is unchanged (dev export).
"""
import argparse
import copy
import glob
import json
import os
import shutil
import struct
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HW = os.path.join(ROOT, 'tools', 'hairworks')
sys.path.insert(0, HERE)
sys.path.insert(0, HW)
sys.path.insert(0, os.path.join(ROOT, 'tools'))

import desk_prep  # noqa: E402
import perf_budget  # noqa: E402
from cr2w_edit import CR2WDoc, Chunk, v_import  # noqa: E402
import redfur as RF  # noqa: E402

ENUM_TYPES = {'densityTexChannel': 'EHairTextureChannel', 'stiffnessTexChannel': 'EHairTextureChannel',
              'rootStiffnessTexChannel': 'EHairTextureChannel'}


def find_type(doc, struct_bytes, path):
    """CR2W type name of path (list) inside a struct value, or None."""
    try:
        items = RF.parse_struct(struct_bytes)
    except AssertionError:
        return None
    for ni, ti, val in items:
        if doc.names[ni] == path[0]:
            if len(path) == 1:
                return doc.names[ti]
            return find_type(doc, val, path[1:])
    return None


def resolve_type(doc, ch, path, hint, value, log):
    p = ch.get(path[0])
    t = find_type(doc, p.value, path[1:]) if p is not None else None
    if t is None:
        ms = ch.get('materialSets')
        if ms is not None and path[0] in ('physicalMaterials', 'graphicalMaterials'):
            t = find_type(doc, ms.value, list(path))
    if t is None:
        if hint in ('Color', 'Vector', 'Float'):
            t = hint
        elif path[-1] in ENUM_TYPES:
            t = ENUM_TYPES[path[-1]]
        elif isinstance(value, float):
            t = 'Float'
        else:
            log('  skipped %s: unknown CR2W type for %r' % ('.'.join(path), value))
            return None
    return t


def encode(doc, typ, value):
    if typ == 'Color':
        v = list(value) + [255] * (4 - len(value))
        return RF.enc_color(doc, v[:4])
    if typ == 'Vector':
        return RF.enc_vec(doc, (list(value) + [0, 0, 0, 0])[:4])
    if typ == 'Float':
        return struct.pack('<f', float(value))
    if typ == 'Bool':
        return struct.pack('<B', int(value))
    if typ in ('Uint32', 'Int32'):
        return struct.pack('<I' if typ == 'Uint32' else '<i', int(value))
    if typ.startswith('E'):
        return struct.pack('<H', doc.name(str(value)))
    raise ValueError(typ)


def apply_value(doc, ch, path, typ, val):
    roots = [tuple(path)]
    if path[0] in ('physicalMaterials', 'graphicalMaterials'):
        roots.append(('materialSets',) + tuple(path))
    for pth in roots:
        pr = ch.get(pth[0])
        if pr is None:
            continue
        pr.value = RF.set_or_add(doc, pr.value, list(pth[1:]), typ, val)


def set_geometry(doc, ch, geo):
    G = [np.asarray(g, float) for g in geo['guides']]
    pts = np.concatenate(G)
    ends, n = [], 0
    for g in G:
        n += len(g)
        ends.append(n - 1)
    F = np.asarray(geo['F'], int).reshape(-1, 3)
    U = np.asarray(geo['uv'], float).reshape(-1, 2)
    ev = RF.enc_vec
    ch.set('positions', 'array:2,0,Vector', RF.arr([ev(doc, list(p) + [1.0]) for p in pts]))
    ch.set('endIndices', 'array:2,0,Uint32', RF.arr([struct.pack('<I', e) for e in ends]))
    ch.set('faceIndices', 'array:2,0,Uint32', RF.arr([struct.pack('<I', int(i)) for i in F.reshape(-1)]))
    ch.set('uvs', 'array:2,0,Vector2', RF.arr([ev(doc, uv, 'XY') for uv in U]))
    ch.set('boneIndices', 'array:2,0,Vector', RF.arr([ev(doc, [float(x) for x in b]) for b in geo['bone_idx']]))
    ch.set('boneWeights', 'array:2,0,Vector', RF.arr([ev(doc, [float(x) for x in b]) for b in geo['bone_w']]))
    mn, mx = pts.min(0) - 0.02, pts.max(0) + 0.02
    bb = lambda v, nm: struct.pack('<HHI', doc.name(nm), doc.name('Vector'), 4 + 51) + ev(doc, list(v) + [1.0])
    ch.set('boundingBox', 'Box', b'\0' + bb(mn, 'Min') + bb(mx, 'Max') + b'\0\0')


def stiffness_xbm(png, path, n=512):
    import headtex
    from PIL import Image
    im = Image.open(png).convert('RGB')
    im = im.resize((n, n), Image.LANCZOS)
    rgb = np.asarray(im, np.float64)
    return headtex.make_xbm(headtex.DXT1_PROXY, n, n, headtex.encode_dxt1(rgb), path, n // 4 * 8)


def hwcore():
    p = os.path.join(ROOT, 'core')
    if p not in sys.path:
        sys.path.insert(0, p)
    import hwcore as K
    return K


def core_unretarget(doc):
    r"""release export: imports that still point at a v1.0.0 Core texture copy (dlc\hairworkforall\core\textures\...)
    go back to the game's own texture (the Core ships no textures). -> number of imports changed (0 for furs built since
    the Core script, so the redfur stays byte-identical to the dev export's)."""
    K = hwcore()
    n = 0
    for im in doc.imports:
        van = K.legacy_vanilla(im[0])
        if van:
            off = len(doc.blob)
            doc.blob += van.encode('utf-8') + b'\0'
            im[0], im[3] = van, off
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    for k in ('cfg', 'edits', 'look', 'geo', 'mod', 'tag'):
        ap.add_argument('--' + k, required=True)
    ap.add_argument('--limit', type=float, default=1.2)
    ap.add_argument('--stiffness')
    ap.add_argument('--density-png', dest='density_png')
    ap.add_argument('--colour-png', dest='colour_png')
    ap.add_argument('--release', action='store_true', help='release export: requires the Hairwork for All Core')
    ap.add_argument('--allow-perf', action='store_true', help='export despite performance-budget errors (dev only)')
    a = ap.parse_args()
    log = print
    if not a.mod.startswith('mod') or not a.mod.replace('_', '').isalnum():
        raise SystemExit('mod folder name must start with "mod" and use letters, digits and _ only')
    geo = json.load(open(a.geo))
    b = geo.get('budget', {})
    if b.get('ratio', 0) > a.limit + 1e-6:
        raise SystemExit('BLOCKED: %.0f %% of the reference build %s (limit %.0f %%)' % (b['ratio'] * 100, b.get('ref'), a.limit * 100))
    edits = json.load(open(a.edits))
    look = json.load(open(a.look))
    if edits.get('ops') or geo.get('unique_uv'):
        dens = (look.get('set', {}).get('physicalMaterials.volume.density') or [None, None])[1] or perf_budget.base_density(a.cfg)
        pr = perf_budget.check(geo, density=dens)
        log('perf budget: %s%s' % (json.dumps(pr['metrics']), ''.join('\n  warning: ' + w for w in pr['warnings'])))
        if pr['errors']:
            msg = 'PERF BLOCKED: ' + '; '.join(pr['errors']) + r' (fix: py tools\hairworks\perf_budget.py ops <geo> -o fix.json)'
            if not a.allow_perf or a.release:
                raise SystemExit(msg)
            log('allowed (--allow-perf): ' + msg)
    fur = desk_prep.find_fur(a.cfg)
    if not fur:
        raise SystemExit('no built redfur for ' + a.cfg)
    D = json.load(open(os.path.join(ROOT, 'build', 'desktop_studio', a.cfg, 'desk.json')))
    mod = D.get('mod') or {}
    if not mod.get('redfur'):
        raise SystemExit('no release mod for %s (mods\\%s): the in-game redfur path and w2ent are unknown. Build it once '
                         'with the pipeline (hairpipe build) first.' % (D.get('character'), mod.get('mod')))
    src_bytes = open(fur, 'rb').read()
    doc = CR2WDoc.parse(src_bytes)
    ch = Chunk(doc, 0)
    if a.release:        # authoring metadata of a third-party legacy skeleton (dev repo only) must never ship
        for k in ('importFile', 'importFileTimeStamp', 'importUnitsScale'):
            ch.remove(k)
    changed = []
    if edits.get('ops') or geo.get('unique_uv'):
        set_geometry(doc, ch, geo)
        changed.append('geometry (%d guides, %d faces)' % (len(geo['guides']), len(geo['F'])))
    for path, (hint, value) in look.get('set', {}).items():
        p = path.split('.')
        typ = resolve_type(doc, ch, p, hint, value, log)
        if typ is None:
            continue
        apply_value(doc, ch, p, typ, encode(doc, typ, value))
        changed.append(path)
    redfur_rel = mod['redfur'][0]
    extra_files = []
    if a.stiffness:
        xbm_rel = redfur_rel[:-len('.redfur')] + '_stiffness.xbm'
        extra_files.append((xbm_rel, stiffness_xbm(a.stiffness, xbm_rel)))
        imp = v_import(doc.add_import(xbm_rel, 'CBitmapTexture'))
        for pth, typ, val in ((['physicalMaterials', 'stiffness', 'stiffnessTex'], 'handle:CBitmapTexture', imp),
                              (['physicalMaterials', 'stiffness', 'rootStiffnessTex'], 'handle:CBitmapTexture', imp),
                              (['physicalMaterials', 'stiffness', 'stiffnessTexChannel'], 'EHairTextureChannel', struct.pack('<H', doc.name('RED'))),
                              (['physicalMaterials', 'stiffness', 'rootStiffnessTexChannel'], 'EHairTextureChannel', struct.pack('<H', doc.name('GREEN')))):
            apply_value(doc, ch, pth, typ, val)
        changed.append('stiffness map ' + xbm_rel)
    # unique UVs (painted stiffness map): the density / colour textures were re-baked into the per-face atlas
    base_rel = redfur_rel[:-len('.redfur')]
    if a.density_png:
        rel = base_rel + '_density.xbm'
        extra_files.append((rel, stiffness_xbm(a.density_png, rel, 2048)))
        imp = v_import(doc.add_import(rel, 'CBitmapTexture'))
        apply_value(doc, ch, ['physicalMaterials', 'volume', 'densityTex'], 'handle:CBitmapTexture', imp)
        apply_value(doc, ch, ['physicalMaterials', 'volume', 'densityTexChannel'], 'EHairTextureChannel', struct.pack('<H', doc.name('RED')))
        changed.append('density map re-baked to unique UVs ' + rel)
    if a.colour_png:
        rel = base_rel + '_colour.xbm'
        extra_files.append((rel, stiffness_xbm(a.colour_png, rel, 2048)))
        imp = v_import(doc.add_import(rel, 'CBitmapTexture'))
        for k in ('rootColorTex', 'tipColorTex'):
            apply_value(doc, ch, ['graphicalMaterials', 'color', k], 'handle:CBitmapTexture', imp)
        changed.append('colour map re-baked to unique UVs ' + rel)
    ch.commit()
    if a.release:
        n_legacy = core_unretarget(doc)
        if n_legacy:
            changed.append('release: %d legacy Core v1 texture import(s) -> game textures' % n_legacy)
    out_bytes = doc.to_bytes() if changed else src_bytes
    RF.verify(out_bytes)
    log('redfur: %s -> %d bytes; changed: %s' % (os.path.relpath(fur, ROOT), len(out_bytes), ', '.join(changed) or 'nothing'))
    # package
    from w3lib import write_legacy_bundle, write_legacy_metadata_store, bundle_entries, bundle_read
    out_dir = os.path.join(ROOT, 'build', 'desktop_studio', a.cfg, 'export')
    mod_dir = os.path.join(out_dir, a.mod)
    if os.path.isdir(mod_dir):
        shutil.rmtree(mod_dir)
    content = os.path.join(mod_dir, 'content')
    os.makedirs(content)
    files = [(redfur_rel, out_bytes)] + extra_files
    src_bundle = os.path.join(ROOT, mod['bundle'])
    ents = {e.name: e for e in bundle_entries(src_bundle)}
    for w in mod.get('w2ent', []):
        files.append((w, bundle_read(src_bundle, ents[w])))
    bundles = [write_legacy_bundle(os.path.join(content, 'blob0.bundle'), files)]
    write_legacy_metadata_store(os.path.join(content, 'metadata.store'), bundles)
    chk = {e.name: e for e in bundle_entries(os.path.join(content, 'blob0.bundle'))}
    for rel, data in files:
        assert bundle_read(os.path.join(content, 'blob0.bundle'), chk[rel]) == data, rel
    with open(os.path.join(mod_dir, 'INSTALL_INSTRUCTIONS.txt'), 'w') as fh:
        fh.write('Witcher 3 HairWorks mod: %s (Desktop Groom Studio export, %s, tag %s)\n\n'
                 '1. Copy the "%s" folder into your Witcher 3 "mods" folder.\n'
                 '2. If mods\\%s already exists, replace it. Do not keep two folders that ship the same character\n'
                 '   (the one whose name sorts first wins).\n' % (a.mod, a.cfg, a.tag, a.mod, a.mod))
        if a.release:
            K = hwcore()
            req = K.required_version()
            fh.write('3. %s\n   Install it too (folder %s): the game will refuse to start without the Core (script\n'
                     '   compilation error "%s ... not found").\n'
                     % (K.requirement_line(req, ascii=True), K.CORE_MOD, K.REQUIRED_FN))
            fh.write('4. %s\n' % K.gc_requirement_line(ascii=True))
    if a.release:
        meta = K.write_release_files(mod_dir, a.mod, req, dict(config=a.cfg, tag=a.tag, exported_by='Hairwork Studio'),
                                     salt=(a.cfg, a.tag))
        log('release: %s (%s)' % (K.requirement_line(meta['requires_core'], ascii=True), meta['core_check']))
    zp = os.path.join(out_dir, '%s_%s.zip' % (a.mod, a.tag))
    with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED) as z:
        for dp, _, fns in os.walk(mod_dir):
            for fn in fns:
                full = os.path.join(dp, fn)
                z.write(full, os.path.relpath(full, out_dir))
    # pipeline config proposal (same keys as any config: edits + look.extra_set)
    C = json.load(open(os.path.join(HW, 'chars', a.cfg + '.json'))) if os.path.exists(os.path.join(HW, 'chars', a.cfg + '.json')) else {}
    C = copy.deepcopy(C)
    C.pop('lab', None)
    char = D.get('character') or a.cfg.split('_')[0]
    # hairstyles (2026-10-11): a build of another hairstyle than the default ("style" config key, kept in the proposal)
    # names its files <char>_<style>_<tag>; the variant to make from it is <char>_<style>_l<round>_<tag>
    if (C.get('style') or 'default') != 'default':
        char = '%s_%s' % (char, C['style'])
    edits_name = '%s_%s.json' % (char, a.tag)
    if edits.get('ops'):
        C['edits'] = edits_name
    lk = C.setdefault('look', {})
    xs = lk.setdefault('extra_set', {})
    for path, (hint, value) in look.get('set', {}).items():
        typ = resolve_type(doc, ch, path.split('.'), hint, value, lambda *x: None)
        if typ == 'Color' and isinstance(value, list) and len(value) < 4:
            value = list(value) + [255] * (4 - len(value))     # as encode() writes it into the exported redfur
        if typ:
            xs[path] = [typ, value]
    if a.release:
        C['core'] = True        # pipeline release mode (hairpipe build --release): writes the Core requirement script too
    if a.stiffness:
        C['note_stiffness_map'] = 'painted stiffness map: not a pipeline key yet; the mod above carries it'
    C['note'] = 'Desktop Groom Studio export of %s (tag %s). Rename to a NEW variant name %s_l<round>_<tag>; copy %s to tools\\hairworks\\edits\\%s.' % (
        a.cfg, a.tag, char, os.path.basename(a.edits), edits_name)
    if look.get('material_config_keys'):
        C['note_material'] = 'material preset %s, pipeline keys: %s' % (look.get('material_preset'), json.dumps(look['material_config_keys']))
    cp = os.path.join(out_dir, '%s_%s_config.json' % (char, a.tag))
    json.dump(C, open(cp, 'w'), indent=2)
    log('mod: %s\nzip: %s\nconfig proposal: %s' % (os.path.relpath(mod_dir, ROOT), os.path.relpath(zp, ROOT), os.path.relpath(cp, ROOT)))


if __name__ == '__main__':
    main()
