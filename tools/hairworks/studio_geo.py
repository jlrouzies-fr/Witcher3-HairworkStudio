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


def resample(geo, k, log=print, mode='uniform'):
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
        out.append([[float(np.interp(x, s, g[:, j])) for j in range(3)] for x in t])
    n0 = sum(len(g) for g in geo['guides'])
    geo = dict(geo, guides=out, K=k)
    log('  studio_geo resample_cvs %d: %d -> %d control points' % (k, n0, k * len(out)))
    return geo


def apply(C, data, redfur_rel, n_bones, log=print):
    """data = the built redfur bytes -> (new redfur bytes, extra files [(game path, bytes)])"""
    sg = C['studio_geo']
    DE = _desk_export()
    from cr2w_edit import CR2WDoc, Chunk, v_import
    import redfur as RF
    geo = load_geo(sg['geo'])
    if sg.get('resample_cvs'):
        geo = resample(geo, int(sg['resample_cvs']), log, sg.get('resample_mode', 'uniform'))   # opt-in 'adaptive' (L353)
    if sg.get('collision_r_shift') and geo.get('collision_r') and not geo.get('colliders'):   # a full collider set (2026-10-14) already has the new indices
        # 2026-10-14 opt-in {"from": 45, "old_outfit": 28}: this build has a different number of outfit colliders than the
        # config the Studio project was made on (outfit_mods / back_items), so the sphere radius overrides saved by the
        # project that sit after the outfit block (the head twins, index >= from) move by (this build - old_outfit)
        sh = sg['collision_r_shift']
        by = int(C.get('_n_outfit_spheres', sh['old_outfit'])) - int(sh['old_outfit'])
        geo = dict(geo)
        geo['collision_r'] = [dict(e, i=int(e['i']) + (by if int(e['i']) >= int(sh['from']) else 0))
                              for e in geo['collision_r']]
    if geo.get('unique_uv') and not sg.get('density_png'):
        # 2026-10-15 (Ewald l345): a project with a painted stiffness map exports its faces with UNIQUE atlas UVs; the
        # density then lives only in the re-baked atlas. Without it the build reads the old density texture at atlas
        # UVs (sparse, stringy hair) and drops the stiffness map. Make the config with studio_to_config.py.
        raise SystemExit('studio_geo: %s has unique (atlas) UVs but the config has no studio_geo.density_png / stiffness: '
                         'make it with tools/hairworks/studio_to_config.py' % sg['geo'])
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
