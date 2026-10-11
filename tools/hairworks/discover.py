r"""discover - find a character's appearances, hair-card components, head / body meshes and accessory islands.

  py tools\hairworks\discover.py <character template .w2ent> [appearance]
  info = discover(template, appearance=None)
     -> dict(appearances={name: [included templates]},
             hair=[dict(template, comp, mesh, materials, used_by=[appearances])],   # card meshes (pbr_hair* materials)
             heads=[mesh paths], bodies=[mesh paths], beards=[...], skeleton_hint)
Rules: a hair-card component = CMeshComponent whose mesh has a pbr_hair* material and whose mesh name is not a beard
(hb_*) / eyebrow / eyelash; beards are reported separately and stay vanilla unless asked.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gamefiles as G


def _walk(o, fn):
    if isinstance(o, dict):
        fn(o)
        for v in o.values():
            _walk(v, fn)
    elif isinstance(o, list):
        for v in o:
            _walk(v, fn)


def appearances(template):
    apps = {}

    def fn(o):
        if 'includedTemplates' in o and isinstance(o.get('includedTemplates'), list):
            tm = [x.get('@import') if isinstance(x, dict) else None for x in o['includedTemplates']]
            apps[o.get('name') or '?%d' % len(apps)] = [t for t in tm if t]
    _walk(G.tree(template), fn)
    return apps


def mesh_comps(tpl):
    out = []
    try:
        tr = G.tree(tpl)
    except Exception as e:
        return out
    seen = set()

    def fn(e):
        if e.get('@class') in ('CMeshComponent', 'CFurComponent'):
            m = e.get('mesh')
            if isinstance(m, dict) and m.get('@import') and (e.get('name'), m['@import']) not in seen:
                seen.add((e.get('name'), m['@import']))
                fm = e.get('furMesh')
                out.append((e.get('name'), e['@class'], m['@import'], fm.get('@import') if isinstance(fm, dict) else None))
    _walk(tr, fn)
    return out


def classify(mesh):
    base = os.path.basename(mesh).lower()
    try:
        mats = [b.lower() for b, _ in G.materials(mesh)]
    except Exception:
        mats = []
    hairmat = any('pbr_hair' in m for m in mats)
    if base.startswith('hb_') or 'beard' in base:
        return 'beard' if hairmat else 'other'
    if 'eyelash' in base or 'eyebrow' in base or base.startswith('he_'):
        return 'eyes'
    if hairmat:
        return 'hair'
    if base.startswith('h_') and '_hq' in base or re.match(r'h_\d+_[a-z]+__', base):
        return 'head'
    if base.startswith(('b_', 'body', 'a_', 'l_', 't_', 'g_', 's_', 'i_')):
        return 'body'
    return 'other'


def discover(template, appearance=None, log=print, reach=0.6, top_tol=0.05):
    apps = appearances(template)
    if appearance:
        sel = {k: v for k, v in apps.items() if k == appearance}
        if not sel:
            raise SystemExit('appearance %r not in %s' % (appearance, sorted(apps)))
    else:
        sel = apps
    hair, heads, bodies, beards = {}, set(), set(), set()
    outfit = {}                     # appearance -> meshes that hair can touch (body, armour, items, fur trims)
    for an, tpls in sel.items():
        for tpl in tpls:
            for comp, cls, mesh, fur in mesh_comps(tpl):
                k = classify(mesh)
                if k == 'hair':
                    h = hair.setdefault((tpl, comp), dict(template=tpl, comp=comp, cls=cls, mesh=mesh, fur=fur, used_by=[]))
                    h['used_by'].append(an)
                elif k == 'head':
                    heads.add(mesh)
                elif k == 'beard':
                    beards.add(mesh)
                elif k == 'body':
                    bodies.add(mesh)
                if k in ('body', 'other') or (k == 'hair' and 'fur' in os.path.basename(mesh).lower()):
                    outfit.setdefault(an, [])
                    if mesh not in outfit[an]:
                        outfit[an].append(mesh)
    # hair cards must sit on the head: pbr_hair is also used by fur collars / trims on body parts
    if heads:
        top = max(float(G.load_mesh(h)['chunks'][0]['pos'][:, 2].max()) for h in heads)
        keep = {}
        for k, h in hair.items():
            bb = G.tree(h['mesh'])[0]['boundingBox']
            if bb['Max']['Z'] > top - top_tol and bb['Min']['Z'] > top - reach:   # reach: config hair_reach (long hair to the waist, 2026-10-05)
                keep[k] = h
            else:
                log('  skipped %s (pbr_hair material but not on the head: z %.2f..%.2f, head top %.2f)' % (
                    h['mesh'], bb['Min']['Z'], bb['Max']['Z'], top))
        hair = keep
    rigs = set()

    def fn(o):
        for v in o.values():
            if isinstance(v, dict) and str(v.get('@import', '')).endswith('.w2rig'):
                rigs.add(v['@import'])
    _walk(G.tree(template), fn)
    info = dict(appearances=apps, hair=list(hair.values()), heads=sorted(heads), bodies=sorted(bodies), beards=sorted(beards),
                rigs=sorted(rigs), outfit=outfit)
    return info


if __name__ == '__main__':
    t = sys.argv[1]
    info = discover(t, sys.argv[2] if len(sys.argv) > 2 else None)
    print('appearances (%d):' % len(info['appearances']))
    for k, v in info['appearances'].items():
        print('  %-32s %s' % (k, ' '.join(os.path.basename(x) for x in v)))
    print('hair card components:')
    for h in info['hair']:
        print('  %s  comp=%s (%s) mesh=%s  fur=%s  used by %d appearances' % (h['template'], h['comp'], h['cls'], h['mesh'], h['fur'], len(h['used_by'])))
    for k in ('heads', 'bodies', 'beards', 'rigs', 'outfit'):
        print(k + ':', info[k])
