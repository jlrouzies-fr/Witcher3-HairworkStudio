r"""rig - bind poses from a skeleton (.w2rig) -> redfur bone table (works for any skeleton: man_base, woman_base, ...).

  bones = bone_table(rig_path, ['head', 'neck', 'torso3', 'r_shoulder', 'l_shoulder'])
CSkeleton trailing = per bone 48 bytes: f32 pos[3], f32 pad, f32 quat[4] (x,y,z,w), f32 scale[3], f32 pad, local to
the parent (parentIndices). World (model-space) bind = local @ parent (row-vector convention, like the redfur
matrices). rig matrix = inverse(bind). Checked against geralt_v1halftail_pony.redfur on man_base (py rig.py --check).
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gamefiles as G
from cr2w_reader import CR2WFile


def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
                     [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
                     [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]])


def load_rig(rel):
    f = CR2WFile(G.get(rel))
    t = f.tree()[0]
    names = [b['name'] for b in t['bones']]
    par = t['parentIndices']
    tr = f.exports[0].trailing
    n = len(names)
    A = np.frombuffer(tr[:n * 48], dtype='<f4').reshape(n, 12)
    M = [None] * n
    for i in range(n):
        loc = np.eye(4)
        loc[:3, :3] = qmat(A[i, 4:8]) * A[i, 8:11][:, None]
        loc[3, :3] = A[i, 0:3]
        M[i] = loc if par[i] < 0 else loc @ M[par[i]]
    return names, par, M


def bone_table(rel, wanted):
    names, par, M = load_rig(rel)
    out = []
    for w in wanted:
        if w not in names:
            continue
        i = names.index(w)
        B = M[i]
        # parent inside the subset, else -1 (vanilla furs keep only the bones they use)
        p = par[i]
        while p >= 0 and names[p] not in wanted:
            p = par[p]
        out.append(dict(name=w, bind=B.tolist(), rig=np.linalg.inv(B).tolist(), parent=-1 if p < 0 else p, eps=0.151))
    sub = [b['name'] for b in out]
    for b in out:
        if b['parent'] >= 0:
            b['parent'] = sub.index(names[b['parent']]) if names[b['parent']] in sub else -1
    return out


if __name__ == '__main__':
    if '--check' in sys.argv:
        t = G.tree('characters\\models\\geralt\\coif\\model\\geralt_v1halftail_pony.redfur')[0]
        mat = lambda m: np.array([[m[r][c] for c in 'XYZW'] for r in 'XYZW'])
        bt = {b['name']: b for b in bone_table('characters\\base_entities\\man_base\\man_base.w2rig', t['boneNames'])}
        for j, n in enumerate(t['boneNames']):
            ref = mat(t['bindPoses'][j])
            got = np.array(bt[n]['bind'])
            print('%-11s max |diff| %.5f' % (n, np.abs(ref - got).max()))
