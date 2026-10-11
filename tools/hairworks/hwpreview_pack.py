r"""hwpreview_pack - data pack for the "HairWorks Preview" renderer (tools\hairworks\hw_preview.html).

  python3 tools/hairworks/hwpreview_pack.py <cfg>_EDIT.json <built>.redfur [-o <cfg>_HW.json]
  python3 tools/hairworks/hwpreview_pack.py --dir results/R76          # every <cfg>_EDIT.json + <cfg>__<comp>.redfur pair

The pack = what HairWorks gets from the .redfur (guide curves, growth faces, per-corner UVs, bones, collision spheres /
capsules, the physical / graphical materials and LOD block, splineMultiplier) + what the viewer needs around it (head /
hat / outfit meshes from the strand-editor export, head box) + the card texture the fur references (density and/or
colour map), downsampled to <= 512 px WebP (data URI), with its alpha sampled per growth corner and averaged per face.

Builds call this through strand_edits.export_hw_pack (build\hairbatch\hw\<cfg>[__<comp>].json, copied by lab/hairlab.py
to results\<round>\<cfg>[__<comp>]_HW.json). Offline (no game) the card texture comes from data\hair\textures (the
512 px data-pack copy of the game's xbm). Nothing here changes a build: it only reads the fur bytes.

Format (version 1, JSON):
  {format: "hwpack", version: 1, config, character, comp, fur, K, splineMultiplier,
   head: {top, c}, head_bone: [x,y,z],
   guides: [[[x,y,z] x K] ...]  (m, 0.1 mm),  F: [[a,b,c] ...],  uv: [[u0,v0,u1,v1,u2,v2] per face],
   bones: [{name, bind (4x4 rows, row-vector convention: world = local @ R + T)}],
   spheres: [{bone, local, c (world, bind pose), r}],  capsules: [[i,j] ...],
   physical, graphical, lod, materialSets (raw dicts as stored; texture handles {"@import": path}),
   textures: {path: {w, h, src: "data:image/webp;base64,...", mean_rgb, mean_alpha}},
   density: {tex, channel, pixel, corner: [[a0,a1,a2] per face], face: [mean per face]} (when a densityTex is set),
   meshes: [...] (strand-editor export: head / hat / outfit, decimated)}
"""
import base64
import glob
import io
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, HERE)
sys.path.insert(0, TOOLS)
PACK_TEX = os.path.join(ROOT, 'data', 'hair', 'textures')
TEX_MAX = 512
CHANNEL = {'RED': 0, 'GREEN': 1, 'BLUE': 2, 'ALPHA': 3}


def _r(a, n=4):
    return np.round(np.asarray(a, float), n).tolist()


def _vec(d, keys='XYZ'):
    return [float(d[k]) for k in keys]


def _mat(m):
    return [[float(m[r][c]) for c in 'XYZW'] for r in 'XYZW']


def parse_fur(data):
    """fur bytes -> CR2W tree dict of the CFurMeshResource (cr2w_reader)."""
    from cr2w_reader import CR2WFile
    return CR2WFile(data).tree()[0]


def imports(o, out=None):
    out = [] if out is None else out
    if isinstance(o, dict):
        if '@import' in o:
            if o['@import'] not in out:
                out.append(o['@import'])
        else:
            for v in o.values():
                imports(v, out)
    elif isinstance(o, list):
        for v in o:
            imports(v, out)
    return out


def pack_texture(path):
    """PIL RGBA of a texture: the data-pack copy (data\\hair\\textures, 512 px) or None."""
    from PIL import Image
    idx_p = os.path.join(ROOT, 'data', 'hair', 'textures_index.json')
    if not os.path.exists(idx_p):
        return None
    idx = {k.lower(): v for k, v in json.load(open(idx_p)).items()}
    fn = idx.get(path.lower())
    if not fn or not os.path.exists(os.path.join(PACK_TEX, fn)):
        return None
    return Image.open(os.path.join(PACK_TEX, fn)).convert('RGBA')


EXTRA_TEXTURES = {}      # path -> PIL RGBA: textures a build writes itself (split_cards.tint), not in the game's cache


def game_texture(path):
    """PIL RGBA from the game's texture.cache (PC only), else the data-pack copy."""
    if path in EXTRA_TEXTURES:
        return EXTRA_TEXTURES[path]
    try:
        from texread import load_xbm
        return load_xbm(path).convert('RGBA')
    except Exception:
        return pack_texture(path)


def tex_entry(img):
    from PIL import Image
    im = img.convert('RGBA')
    s = TEX_MAX / max(im.size)
    if s < 1:
        im = im.resize((max(1, round(im.size[0] * s)), max(1, round(im.size[1] * s))), Image.LANCZOS)
    b = io.BytesIO()
    im.save(b, 'WEBP', quality=88, method=6)
    a = np.asarray(im, np.float32) / 255.0
    return dict(w=im.size[0], h=im.size[1], src='data:image/webp;base64,' + base64.b64encode(b.getvalue()).decode(),
                mean_rgb=_r(a[..., :3].reshape(-1, 3).mean(0), 4), mean_alpha=round(float(a[..., 3].mean()), 4))


def sample(arr, uv):
    """nearest-texel lookup, uv per row (u right, v down as stored; wraps)."""
    h, w = arr.shape[:2]
    x = np.clip(((uv[..., 0] % 1.0) * w).astype(int), 0, w - 1)
    y = np.clip(((uv[..., 1] % 1.0) * h).astype(int), 0, h - 1)
    return arr[y, x]


def density_maps(F, UV, img, channel, n=48):
    """per-corner channel value and per-face mean over the face's uv triangle (n fixed stratified samples)."""
    a = np.asarray(img.convert('RGBA'), np.float32)[..., CHANNEL.get(channel, 3)] / 255.0
    corner = sample(a, UV.reshape(-1, 2)).reshape(-1, 3)
    rng = np.random.default_rng(5)                 # own generator: never touches a build's random state
    w = rng.random((n, 2))
    flip = w.sum(1) > 1
    w[flip] = 1 - w[flip]
    bw = np.c_[1 - w.sum(1), w]                    # (n,3) barycentric
    pts = np.einsum('sk,fkc->fsc', bw, UV)         # (F,n,2)
    face = sample(a, pts).mean(1)
    return _r(corner, 3), _r(face, 3)


def build_pack(edit, fur_bytes, tex_loader=pack_texture, fur_name=None, log=print):
    t = parse_fur(fur_bytes)
    P = np.array([_vec(p) for p in t['positions']], float)
    G, s = [], 0
    for e in t['endIndices']:                      # INCLUSIVE last-vertex indices
        G.append(P[s:e + 1]); s = e + 1
    K = max(len(g) for g in G)
    F = np.array(t['faceIndices'], int).reshape(-1, 3)
    UV = np.array([[u['X'], u['Y']] for u in t['uvs']], float).reshape(-1, 3, 2)
    names = list(t['boneNames'])
    binds = [_mat(m) for m in t['bindPoses']]
    bones = [dict(name=n, bind=_r(b, 5)) for n, b in zip(names, binds)]
    spheres = []
    for lp, bi, r in zip(t.get('boneSphereLocalPosArray', []), t.get('boneSphereIndexArray', []),
                         t.get('boneSphereRadiusArray', [])):
        B = np.array(binds[int(bi)])
        loc = np.array(_vec(lp))
        spheres.append(dict(bone=int(bi), local=_r(loc, 5), c=_r(loc @ B[:3, :3] + B[3, :3], 5), r=round(float(r), 5)))
    cap = list(t.get('boneCapsuleIndices', []))
    capsules = [[int(cap[i]), int(cap[i + 1])] for i in range(0, len(cap) - 1, 2)]
    pm = t.get('physicalMaterials') or {}
    gm = t.get('graphicalMaterials') or {}
    head_bone = _r(np.array(binds[names.index('head')])[3, :3], 5) if 'head' in names else None
    D = dict(format='hwpack', version=1, fur=fur_name, K=int(K), splineMultiplier=int(t.get('splineMultiplier', 1) or 1),
             guides=[_r(g, 4) for g in G], F=F.tolist(), uv=_r(UV.reshape(-1, 6), 4),
             bones=bones, head_bone=head_bone, spheres=spheres, capsules=capsules,
             physical=pm, graphical=gm, lod=t.get('levelOfDetail'), materialSets=t.get('materialSets'),
             autoHideDistance=t.get('autoHideDistance'), textures={})
    if edit:
        for k in ('config', 'character', 'comp', 'head'):
            if k in edit:
                D[k] = edit[k]
        D['meshes'] = edit.get('meshes', [])
        if edit.get('edits'):
            D['edits_applied'] = len(edit['edits'].get('ops', []))
    else:
        D['meshes'] = []
    if 'head' not in D:
        D['head'] = dict(top=round(float(P[:, 2].max()), 4), c=_r([0.0, float(np.median(P[:, 1])), float(P[:, 2].max()) - 0.14], 4))
    for path in imports([pm, gm]):
        img = tex_loader(path) if tex_loader else None
        if img is None:
            log('  hw pack: texture %s not available (renderer falls back to root / tip colours, no pixel density)' % path)
            continue
        D['textures'][path] = tex_entry(img)
        if (pm.get('volume') or {}).get('densityTex', {}).get('@import') == path:
            ch = (pm.get('volume') or {}).get('densityTexChannel', 'RED')
            corner, face = density_maps(F, UV, img, ch)
            D['density'] = dict(tex=path, channel=ch, pixel=bool((pm.get('volume') or {}).get('usePixelDensity', 0)),
                                corner=corner, face=face)
    return D


def dumps(D):
    return json.dumps(D, separators=(',', ':'))


def write_pack(edit_path, fur_path, out=None, tex_loader=pack_texture, log=print):
    edit = json.load(open(edit_path)) if edit_path and os.path.exists(edit_path) else None
    D = build_pack(edit, open(fur_path, 'rb').read(), tex_loader, os.path.basename(fur_path), log)
    if out is None:
        base = edit_path[:-len('_EDIT.json')] if edit_path and edit_path.endswith('_EDIT.json') else fur_path.rsplit('.', 1)[0]
        out = base + '_HW.json'
    s = dumps(D)
    open(out, 'w').write(s)
    log('hw pack: %s (%d guides, %d faces, %d spheres, %d capsules, %d textures, %.0f KB)' % (
        os.path.relpath(out, ROOT), len(D['guides']), len(D['F']), len(D['spheres']), len(D['capsules']),
        len(D['textures']), len(s) / 1024.0))
    return out


def pairs(folder):
    """<cfg>_EDIT.json + <cfg>__<comp>.redfur (or <cfg>__<comp>_EDIT.json for 2nd+ components) in one folder."""
    out = []
    for fur in sorted(glob.glob(os.path.join(folder, '*.redfur'))):
        b = os.path.basename(fur)
        if '__' not in b:
            continue
        cfg, comp = b[:-len('.redfur')].split('__', 1)
        for e in (os.path.join(folder, '%s__%s_EDIT.json' % (cfg, comp)), os.path.join(folder, cfg + '_EDIT.json')):
            if os.path.exists(e):
                ed = json.load(open(e))
                if ed.get('comp') in (None, comp):
                    out.append((e, fur, e[:-len('_EDIT.json')] + '_HW.json'))
                    break
    return out


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == '--dir':
        for e, f, o in pairs(a[1]):
            write_pack(e, f, o)
    elif len(a) >= 2:
        write_pack(a[0], a[1], a[a.index('-o') + 1] if '-o' in a else None)
    else:
        print(__doc__)
