r"""gamefiles - read game files for the hair pipeline: bundle index, CR2W trees, skinned meshes (positions, UVs, skin).

  get(rel) -> bytes                      any file of the 5.0 bundles (index cached in build\hairpipe_index.pkl)
  get_buffer(rel) -> bytes               '<mesh>.1.buffer' (buffers.bundle / any *buffers* bundle)
  tree(rel) -> list of decoded exports   cr2w_reader
  load_mesh(rel) -> dict(chunks=[dict(pos, uv, bi, bw, idx, mat, nv, rec)], lod0=[chunk ids], t=tree0, bonePositions)
  components(nv, idx) -> label per vertex (connected components = hair cards / accessory islands)
  find(pattern) -> [paths]               regex over every bundle path
"""
import os
import pickle
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
ROOT = os.path.dirname(TOOLS)
GAME = os.environ.get("HWFA_GAME") or os.path.dirname(ROOT)   # HWFA_GAME: game folder of a release install
BUNDLES = os.path.join(GAME, 'content', 'content0', 'bundles')
sys.path.insert(0, TOOLS)
import w3lib
from cr2w_reader import CR2WFile

_index = None
_tree_cache = {}


def index():
    """{lower path: (bundle path, entry)} over every bundle (cached on disk by bundle sizes / mtimes)."""
    global _index
    if _index is not None:
        return _index
    names = sorted(n for n in os.listdir(BUNDLES) if n.endswith('.bundle') and n != 'movies.bundle')
    key = [(n, os.path.getsize(os.path.join(BUNDLES, n)), int(os.path.getmtime(os.path.join(BUNDLES, n)))) for n in names]
    cache = os.path.join(ROOT, 'build', 'hairpipe_index.pkl')
    if os.path.exists(cache):
        try:
            k, idx = pickle.load(open(cache, 'rb'))
            if k == key:
                _index = idx
                return idx
        except Exception:
            pass
    idx = {}
    for n in names:
        p = os.path.join(BUNDLES, n)
        for e in w3lib.bundle_entries(p):
            idx.setdefault(e.name.lower(), (p, e))
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    pickle.dump((key, idx), open(cache, 'wb'))
    _index = idx
    return idx


_over = {}
_over_key = ()


def use_mods(names):
    """R81 (user installed a third-party hair mod, e.g. long hair for Triss): files of these mods are read INSTEAD of
    the game's (same paths), so a character's discovery / cards / template come from the hairstyle the user plays with.
    names = mod folder names under <game>\\mods (or absolute content folders). Order = priority (first wins)."""
    global _over_key
    key = tuple(names or [])
    if key == _over_key:
        return
    _over_key = key
    _over.clear()
    _tree_cache.clear()
    for n in names or []:
        c = n if os.path.isabs(n) else os.path.join(GAME, 'mods', n, 'content')
        if not os.path.isdir(c):
            raise SystemExit('source mod not found: %s' % c)
        for f in sorted(os.listdir(c)):
            if f.endswith('.bundle'):
                bp = os.path.join(c, f)
                for e in w3lib.bundle_entries(bp):
                    _over.setdefault(e.name.lower(), (bp, e))


def exists(rel):
    return rel.lower() in _over or rel.lower() in index()


def get(rel):
    bp, e = _over.get(rel.lower()) or index()[rel.lower()]
    return w3lib.bundle_read(bp, e)


def get_vanilla(rel):
    """2026-10-05: the GAME's file at rel, ignoring use_mods overrides (Triss: patch CDPR's template, never ship a
    third-party mod's file)."""
    bp, e = index()[rel.lower()]
    return w3lib.bundle_read(bp, e)


def get_buffer(rel):
    return get(rel)


def find(pattern):
    r = re.compile(pattern, re.I)
    return sorted(p for p in index() if r.search(p))


def tree(rel):
    if rel.lower() not in _tree_cache:
        _tree_cache[rel.lower()] = CR2WFile(get(rel)).tree()
    return _tree_cache[rel.lower()]


def _walk_bytes(v, out):
    if isinstance(v, (bytes, bytearray)):
        out.append(bytes(v))
    elif isinstance(v, (tuple, list)):
        for x in v:
            _walk_bytes(x, out)
    elif isinstance(v, dict):
        for x in v.values():
            _walk_bytes(x, out)
    elif hasattr(v, '__dict__'):
        _walk_bytes(v.__dict__, out)


def load_mesh(rel):
    """Skinned w2mesh (5.0): stream A = 8 bytes quantised position + 4 bone indices + 4 weights, stream B = half2 UV."""
    f = CR2WFile(get(rel))
    t = f.tree()[0]
    buf = get_buffer(rel + '.1.buffer')
    cd = t['cookedData']
    sc = np.array([cd['quantizationScale'][k] for k in 'XYZ'])
    off = np.array([cd['quantizationOffset'][k] for k in 'XYZ'])
    allb = []
    _walk_bytes(f.exports[0].props['cookedData'], allb)
    nch = len(t['chunks'])
    rc = [b for b in allb if len(b) >= 1 and b[0] == nch and (len(b) - 1) % nch == 0][0]
    recsz = (len(rc) - 1) // nch
    ibo = cd['indexBufferOffset']
    out = []
    for ci, ch in enumerate(t['chunks']):
        rec = rc[1 + ci * recsz:1 + (ci + 1) * recsz]
        offs = struct.unpack_from('<4I', rec, 1)
        nv = ch['numVertices']
        A = np.frombuffer(buf, dtype=np.uint8, count=nv * 16, offset=offs[0]).reshape(nv, 16)
        q = A[:, :8].copy().view('<u2').reshape(nv, 4)[:, :3].astype(np.float64) / 65535.0
        uv = np.frombuffer(buf, dtype='<f2', count=nv * 2, offset=offs[1]).reshape(nv, 2).astype(float)
        fi = ch.get('firstIndex', 0)
        ni = ch['numIndices']
        idx = np.frombuffer(buf, dtype='<u2', count=ni, offset=ibo + fi * 2).astype(int)
        out.append(dict(pos=q * sc + off, uv=uv, bi=A[:, 8:12].astype(int), bw=A[:, 12:16].astype(float) / 255.0,
                        idx=idx, mat=ch.get('materialID', 0), nv=nv, rec=rec, shadow=bool(ch.get('useForShadowmesh'))))
    try:
        lod0 = mesh_bones(rel)['lods'][0]          # real LOD chunk lists from the mesh custom data
    except Exception:
        lod0 = lod0_chunks(cd, nch)
    bp = np.array([[b['X'], b['Y'], b['Z']] for b in cd.get('bonePositions', [])])
    return dict(chunks=out, lod0=lod0, t=t, bonePositions=bp, rc=rc)


def lod0_chunks(cd, nch):
    """chunk ids of the first render LOD. cookedData.renderLODs = one distance per LOD; the per-LOD chunk lists live in
    the mesh custom data (not decoded). Heuristic, true for every hair mesh checked (Vesemir 3 chunks / 3 LODs,
    Yennefer 4 chunks / 3 LODs = 2 materials in LOD0): the extra chunks all belong to LOD0."""
    n = len(cd.get('renderLODs', [0])) or 1
    return list(range(max(1, nch - n + 1)))


def merge_chunks(m, ids):
    """one chunk dict (pos, uv, bi, bw, idx, nv) from several chunks of a load_mesh result (indices re-based)."""
    cs = [m['chunks'][i] for i in ids]
    if len(cs) == 1:
        return cs[0]
    off = np.cumsum([0] + [c['nv'] for c in cs[:-1]])
    return dict(pos=np.concatenate([c['pos'] for c in cs]), uv=np.concatenate([c['uv'] for c in cs]),
                bi=np.concatenate([c['bi'] for c in cs]), bw=np.concatenate([c['bw'] for c in cs]),
                idx=np.concatenate([c['idx'] + o for c, o in zip(cs, off)]), nv=int(sum(c['nv'] for c in cs)),
                mat=cs[0]['mat'], parts=list(zip(ids, off.tolist())))


def components(nv, idx):
    par = np.arange(nv)

    def find_(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    for a, b, c in np.asarray(idx).reshape(-1, 3):
        for u, v in ((a, b), (b, c)):
            ru, rv = find_(u), find_(v)
            if ru != rv:
                par[ru] = rv
    return np.array([find_(i) for i in range(nv)])


def materials(rel):
    """[(base material path, {param: value})] of a mesh."""
    out = []
    for m in tree(rel)[0].get('materials', []):
        base = (m.get('baseMaterial') or {}).get('@import', '')
        out.append((base, m.get('@matparams', {})))
    return out


def mesh_bones(rel):
    """bone names (in vertex bone-index order) + rig matrices of a skinned w2mesh, from the CMesh custom data:
    LODs [vlq n, {vlq k, u16 chunk * k, f32 dist}], names [vlq n, u16 name], matrices [vlq n, 16 f32],
    epsilons [vlq n, f32], mapping [vlq n, u32]  (layout from tools/de5_lib.py)."""
    import cr2w_writer as W
    f = CR2WFile(get(rel))
    t = f.exports[0].trailing
    p = 0
    n, _ng, p = W.read_vlq2(t, p)
    lods = []
    for _ in range(n):
        k, _ng, p = W.read_vlq2(t, p)
        lods.append(list(struct.unpack_from('<%dH' % k, t, p)))
        p += 2 * k + 4
    n, _ng, p = W.read_vlq2(t, p)
    names = [f.name(struct.unpack_from('<H', t, p + 2 * i)[0]) for i in range(n)]
    p += 2 * n
    n, _ng, p = W.read_vlq2(t, p)
    mats = np.frombuffer(t[p:p + 64 * n], dtype='<f4').reshape(n, 4, 4)
    return dict(names=names, rig=mats, lods=lods)
