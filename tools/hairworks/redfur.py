r"""redfur - write a CFurMeshResource (.redfur, CR2W v164) from generated guides.

  data = write(strands, faces, uvs, bones, collision, look)
    strands : [dict(p=(K,3) root->tip, w=[(bone slot, weight), ...])]
    faces   : (F,3) indices into strands (outward CCW), uvs: (F*3,2) per face CORNER (as every vanilla redfur)
    bones   : [dict(name, bind 4x4 rows, rig 4x4 rows = inverse(bind), parent, eps)]
    collision: dict(pos (local to bone), idx, rad, cap)
    look    : dict(mat_src=<vanilla redfur path to copy physical/graphical/LOD/materialSets from>,
                   textures={('physicalMaterials','volume','densityTex'): xbm path, ...},   # handles (imports)
                   set={('graphicalMaterials','color','rootColor'): ('Color', (r,g,b,a)) | ('Float', x) |
                        ('Bool', 0/1) | ('Uint32', n) | ('EHairTextureChannel', 'ALPHA') ...},
                   drop_textures=True)   # source texture handles are dropped unless re-set in `textures`
  Paths in `set` / `textures` that start with physicalMaterials / graphicalMaterials are applied to the top level
  AND to materialSets (the engine reads both; vanilla keeps them identical).
Rules learnt (keep): endIndices are INCLUSIVE last-vertex indices; positions W=1; uv signed area < 0 per face;
bindPoses = inverse(boneRigMatrices); vanilla Geralt furs store levelOfDetail.priority = 0.
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from cr2w_edit import CR2WDoc, Chunk, v_import
import gamefiles as G

TPL = os.path.join(HERE, 'tpl', 'fur02.redfur')     # LEGACY skeleton, private dev repo only (third-party file, never released)
# Release skeleton: a vanilla CD Projekt fur read at run time from the user's own game bundles (nothing is bundled).
GAME_SKELETON = r'characters\models\geralt\coif\model\c_04_mg__long_fur.redfur'
ROOT_DIR = os.path.dirname(os.path.dirname(HERE))


def _nm(doc, s):
    return doc.name(s)


def enc_vec(doc, vals, names='XYZW'):
    b = bytearray(b'\0')
    for n, v in zip(names, vals):
        b += struct.pack('<HHIf', _nm(doc, n), _nm(doc, 'Float'), 8, float(v))
    return bytes(b + b'\0\0')


def enc_mat(doc, rows):
    b = bytearray(b'\0')
    for n, r in zip('XYZW', rows):
        v = enc_vec(doc, r)
        b += struct.pack('<HHI', _nm(doc, n), _nm(doc, 'Vector'), 4 + len(v)) + v
    return bytes(b + b'\0\0')


def enc_color(doc, rgba):
    b = bytearray(b'\0')
    for n, v in zip(('Red', 'Green', 'Blue', 'Alpha'), rgba):
        b += struct.pack('<HHIB', _nm(doc, n), _nm(doc, 'Uint8'), 5, int(v))
    return bytes(b + b'\0\0')


def arr(items):
    return struct.pack('<I', len(items)) + b''.join(items)


def parse_struct(v):
    assert v[0] == 0, v[:4]
    p, out = 1, []
    while True:
        ni, = struct.unpack_from('<H', v, p)
        if ni == 0:
            break
        ti, sz = struct.unpack_from('<HI', v, p + 2)
        out.append([ni, ti, v[p + 8:p + 4 + sz]])
        p += 4 + sz
    assert p + 2 == len(v), (p, len(v))
    return out


def build_struct(items):
    b = bytearray(b'\0')
    for ni, ti, val in items:
        b += struct.pack('<HHI', ni, ti, len(val) + 4) + val
    return bytes(b + b'\0\0')


def copy_struct(sd, v, dd, drop_textures=True):
    out = []
    for ni, ti, val in parse_struct(v):
        tn = sd.names[ti]
        if tn.startswith('handle:') or tn.startswith('soft:'):
            if drop_textures:
                continue
            path = sd.imports[-struct.unpack('<i', val)[0] - 1]
            imp = dd.add_import(path['path'] if isinstance(path, dict) else path.path, 'CBitmapTexture')
            val = v_import(imp)
        nn, nt = dd.name(sd.names[ni]), dd.name(tn)
        if len(val) == 2 and tn not in ('Int16', 'Uint16'):          # enum value = name index
            val = struct.pack('<H', dd.name(sd.names[struct.unpack('<H', val)[0]]))
        elif len(val) >= 3 and val[0] == 0 and val[-2:] == b'\0\0' and tn not in ('Float', 'Uint32', 'Int32', 'Bool'):
            try:
                val = copy_struct(sd, val, dd, drop_textures)
            except AssertionError:
                pass
        out.append([nn, nt, val])
    return build_struct(out)


def enc_value(doc, typ, value):
    if typ == 'Color':
        return enc_color(doc, value)
    if typ == 'Float':
        return struct.pack('<f', value)
    if typ == 'Bool':
        return struct.pack('<B', int(value))
    if typ == 'Vector':
        return enc_vec(doc, list(value) + [0.0] * (4 - len(value)))
    if typ == 'Uint32':
        return struct.pack('<I', int(value))
    if typ.startswith('E'):                        # enum by value name
        return struct.pack('<H', doc.name(value))
    if typ == 'handle:CBitmapTexture':
        return v_import(doc.add_import(value, 'CBitmapTexture'))
    raise ValueError(typ)


def set_or_add(doc, v, path, typ, val):
    items = parse_struct(v)
    nm = path[0]
    for it in items:
        if doc.names[it[0]] == nm:
            if len(path) == 1:
                it[1] = doc.name(typ)
                it[2] = val
            else:
                it[2] = set_or_add(doc, it[2], path[1:], typ, val)
            return build_struct(items)
    if len(path) == 1:
        items.append([doc.name(nm), doc.name(typ), val])
    elif nm in STRUCT_TYPES:
        # 2026-10-05: create a missing sub-struct (levelOfDetail.culling is absent in c_04_mg__long_fur)
        items.append([doc.name(nm), doc.name(STRUCT_TYPES[nm]), set_or_add(doc, b'\0\0\0', path[1:], typ, val)])
    else:
        raise KeyError(('missing parent struct', path))
    return build_struct(items)


STRUCT_TYPES = {'culling': 'SFurCulling'}


def remove_path(doc, v, path):
    items = parse_struct(v)
    out = []
    for it in items:
        if doc.names[it[0]] == path[0]:
            if len(path) == 1:
                continue
            it[2] = remove_path(doc, it[2], path[1:])
        out.append(it)
    return build_struct(out)


def skeleton_mode(look=None):
    """'legacy' (tpl/fur02.redfur, byte-identical approved builds) or 'game' (vanilla fur from the game bundles).
    Order: look['skeleton'], env HWFA_SKELETON, then legacy if tpl/fur02.redfur exists (private dev repo), else game
    (release packages never contain the file)."""
    m = (look or {}).get('skeleton') or os.environ.get('HWFA_SKELETON') or ('legacy' if os.path.isfile(TPL) else 'game')
    if m not in ('legacy', 'game'):
        raise ValueError('skeleton must be legacy or game: ' + str(m))
    if m == 'legacy' and not os.path.isfile(TPL):
        raise FileNotFoundError('tpl/fur02.redfur is not part of release packages; use skeleton "game"')
    return m


def load_skeleton(mode):
    if mode == 'legacy':
        return CR2WDoc.parse(open(TPL, 'rb').read())
    # game: every field write() does not set itself is made explicit here, so the vanilla fur contributes no geometry,
    # constraints, textures or authoring metadata: pin constraints + import* removed, the 4 material structs emptied
    # (write() copies the mat_src ones), source texture imports dropped, splineMultiplier 3 (the legacy skeleton's value).
    doc = CR2WDoc.parse(G.get(GAME_SKELETON))
    ch = Chunk(doc, 0)
    for k in ('pinConstraintsLocalPosArray', 'pinConstraintsIndexArray', 'pinConstraintsRadiusArray',
              'importUnitsScale', 'importFile', 'importFileTimeStamp'):
        ch.remove(k)
    for k in ('physicalMaterials', 'graphicalMaterials', 'levelOfDetail', 'materialSets'):
        pr = ch.get(k)
        if pr is not None:
            pr.value = build_struct([])
    doc.imports = []
    ch.set('splineMultiplier', 'Uint32', struct.pack('<I', 3))
    ch.commit()
    return doc


def write(strands, faces, uvs, bones, collision, look, auto_hide=100.0):
    doc = load_skeleton(skeleton_mode(look))
    ch = Chunk(doc, 0)
    pts = np.concatenate([s['p'] for s in strands])
    ends, n = [], 0
    for s in strands:
        n += len(s['p'])
        ends.append(n - 1)                 # INCLUSIVE last vertex of each guide (vanilla: last end == len(positions)-1)
    faces = np.asarray(faces).reshape(-1, 3)
    uvs = np.asarray(uvs).reshape(-1, 2)
    assert len(uvs) == faces.size, (len(uvs), faces.size)
    ch.set('positions', 'array:2,0,Vector', arr([enc_vec(doc, list(p) + [1.0]) for p in pts]))
    ch.set('endIndices', 'array:2,0,Uint32', arr([struct.pack('<I', e) for e in ends]))
    ch.set('faceIndices', 'array:2,0,Uint32', arr([struct.pack('<I', int(i)) for i in faces.reshape(-1)]))
    ch.set('uvs', 'array:2,0,Vector2', arr([enc_vec(doc, uv, 'XY') for uv in uvs]))
    bi, bw = [], []
    for s in strands:
        sl = (list(s['w']) + [(0, 0.0)] * 4)[:4]
        bi.append(enc_vec(doc, [float(a) for a, _ in sl]))
        bw.append(enc_vec(doc, [float(b) for _, b in sl]))
    ch.set('boneIndices', 'array:2,0,Vector', arr(bi))
    ch.set('boneWeights', 'array:2,0,Vector', arr(bw))
    ch.set('boneCount', 'Uint32', struct.pack('<I', len(bones)))
    ch.set('boneNames', 'array:2,0,CName', arr([struct.pack('<H', _nm(doc, b['name'])) for b in bones]))
    ch.set('boneRigMatrices', 'array:2,0,Matrix', arr([enc_mat(doc, b['rig']) for b in bones]))
    ch.set('boneParents', 'array:2,0,Int32', arr([struct.pack('<i', b.get('parent', -1)) for b in bones]))
    ch.set('boneVertexEpsilons', 'array:71,0,Float', arr([struct.pack('<f', b.get('eps', 0.151)) for b in bones]), after='bindPoses')
    ch.set('bindPoses', 'array:2,0,Matrix', arr([enc_mat(doc, b['bind']) for b in bones]))
    col = collision or dict(pos=[], idx=[], rad=[], cap=[])
    ch.set('boneSphereLocalPosArray', 'array:2,0,Vector', arr([enc_vec(doc, list(p) + [0.0]) for p in col['pos']]))
    ch.set('boneSphereIndexArray', 'array:2,0,Uint32', arr([struct.pack('<I', int(i)) for i in col['idx']]))
    ch.set('boneSphereRadiusArray', 'array:2,0,Float', arr([struct.pack('<f', r) for r in col['rad']]))
    ch.set('boneCapsuleIndices', 'array:2,0,Uint32', arr([struct.pack('<I', int(i)) for i in col['cap']]))
    mn, mx = pts.min(0) - 0.02, pts.max(0) + 0.02
    bb = lambda v, nm_: struct.pack('<HHI', _nm(doc, nm_), _nm(doc, 'Vector'), 4 + 51) + enc_vec(doc, list(v) + [1.0])
    ch.set('boundingBox', 'Box', b'\0' + bb(mn, 'Min') + bb(mx, 'Max') + b'\0\0')
    ch.set('autoHideDistance', 'Float', struct.pack('<f', auto_hide))
    # materials: copy the vanilla source's structs, then textures + overrides
    src = look['mat_src']
    if os.path.isfile(src) or os.path.isfile(os.path.join(ROOT_DIR, src)):
        # a local .redfur (e.g. the Yennefer mod's fur in analysis/hairworks/extracted): material presets (R32)
        sd = CR2WDoc.parse(open(src if os.path.isfile(src) else os.path.join(ROOT_DIR, src), 'rb').read())
    else:
        sd = CR2WDoc.parse(G.get(src))
    sch = Chunk(sd, 0)
    drop = look.get('drop_textures', True)
    for key in ('physicalMaterials', 'graphicalMaterials', 'levelOfDetail', 'materialSets'):
        p = sch.get(key)
        if p is not None:
            ch.get(key).value = copy_struct(sd, p.value, doc, drop)
    if 'splineMultiplier' in look:
        ch.set('splineMultiplier', 'Uint32', struct.pack('<I', look['splineMultiplier']))

    def apply(path, typ, val):
        roots = [path]
        if path[0] in ('physicalMaterials', 'graphicalMaterials'):
            roots.append(('materialSets',) + tuple(path))
        for pth in roots:
            pr = ch.get(pth[0])
            if pr is None:
                continue
            if typ is None:
                pr.value = remove_path(doc, pr.value, list(pth[1:]))
            else:
                pr.value = set_or_add(doc, pr.value, list(pth[1:]), typ, val)
    for path in look.get('remove', []):
        apply(tuple(path), None, None)
    for path, xbm in look.get('textures', {}).items():
        apply(tuple(path), 'handle:CBitmapTexture', enc_value(doc, 'handle:CBitmapTexture', xbm))
    for path, (typ, value) in look.get('set', {}).items():
        apply(tuple(path), typ, enc_value(doc, typ, value))
    ch.commit()
    doc.version, doc.timestamp, doc.build = 164, 0, 0
    return doc.to_bytes()


def bone_table(src_redfur, names=None):
    """bone table (name, bind, rig, parent, eps) from a vanilla redfur of the same skeleton."""
    t = G.tree(src_redfur)[0]
    mat = lambda m: [[m[r][c] for c in 'XYZW'] for r in 'XYZW']
    nm = t['boneNames']
    sel = list(range(len(nm))) if names is None else [nm.index(n) for n in names]
    out = []
    for j in sel:
        pj = t['boneParents'][j]
        par = sel.index(pj) if pj in sel else -1
        out.append(dict(name=nm[j], bind=mat(t['bindPoses'][j]), rig=mat(t['boneRigMatrices'][j]), parent=par,
                        eps=t['boneVertexEpsilons'][j] if 'boneVertexEpsilons' in t else 0.151))
    return out


def verify(data):
    """decode check: CR2W v164, 0 raw props / export errors, CRCs ok, endIndices inclusive, uv per corner."""
    sys.path.insert(0, os.path.dirname(HERE))
    import w3lib
    from cr2w_reader import CR2WFile
    f = CR2WFile(data)
    s = f.summary()
    assert s['raw_props'] == 0 and s['export_errors'] == 0 and s['version'] == 164, s
    bad, ok = w3lib.CR2W(data).verify_crcs()
    assert bad == 0 and ok
    t = f.tree()[0]
    assert t['endIndices'][-1] == len(t['positions']) - 1
    assert len(t['uvs']) == len(t['faceIndices'])
    return t
