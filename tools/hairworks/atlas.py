r"""atlas - drive HairWorks colour / density from the VANILLA hair-card texture (no new texture.cache).

Mechanism (verified on Geralt's vanilla redfurs): a .redfur's growth mesh carries one UV per face CORNER
(len(uvs) == len(faceIndices)), and its texture slots (densityTex, rootColorTex, tipColorTex, ...) are plain
imports of game .xbm files sampled at each hair root's interpolated UV. So every growth face can get its own little
UV triangle anywhere in any vanilla texture.

We map each face onto a patch of the card atlas (e.g. hair_long_04_d01.xbm) oriented like the card strands
(atlas v = along the hair, atlas u = across), at the card's physical texel scale. With
  rootColorTex = tipColorTex = the card diffuse   -> each hair takes the colour of the card strand under its root
  densityTex  = the card diffuse, channel ALPHA, usePixelDensity -> hairs only grow where the card has strands
every face reproduces the card's strand pattern (streaks + gaps = locks). The patch position picks the coverage:
patches high in a strand panel are dense, patches near the tips are sparse (wispy hairline).

  lib = PatchLibrary(xbm, exclude=[(u0, v0, u1, v1), ...])     # candidate patch centres + their alpha / luma
  uvs = face_uvs(roots, faces, dirs, density_target, lib, scale)   # (F*3, 2) per-corner UVs
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


class PatchLibrary:
    def __init__(self, xbm, exclude=(), patch_px=48, step_px=12, region=(0.0, 0.0, 1.0, 1.0), seed=3):
        from texread import load_xbm
        im = np.asarray(load_xbm(xbm)).astype(np.float64) / 255.0
        self.h, self.w = im.shape[:2]
        self.xbm = xbm
        a = im[..., 3]
        lum = im[..., :3].mean(2)
        from scipy.ndimage import uniform_filter
        ma = uniform_filter(a, patch_px)
        ml = uniform_filter(lum * a, patch_px) / np.maximum(ma, 1e-3)
        # strand contrast inside the patch (dense solid blocks have ~0: they read as a smooth shell)
        sa = np.sqrt(np.maximum(uniform_filter(a * a, patch_px) - ma * ma, 0))
        ys, xs = np.mgrid[patch_px // 2:self.h - patch_px // 2:step_px, patch_px // 2:self.w - patch_px // 2:step_px]
        u = (xs + 0.5) / self.w
        v = (ys + 0.5) / self.h
        keep = (u >= region[0]) & (u <= region[2]) & (v >= region[1]) & (v <= region[3])
        for (u0, v0, u1, v1) in exclude:
            keep &= ~((u >= u0) & (u <= u1) & (v >= v0) & (v <= v1))
        self.u, self.v = u[keep], v[keep]
        self.alpha = ma[ys, xs][keep]
        self.luma = ml[ys, xs][keep]
        self.contrast = sa[ys, xs][keep]
        self.rng = np.random.default_rng(seed)
        self.patch_px = patch_px

    def pick(self, target, tol=0.06, min_contrast=0.12):
        """a random patch centre whose mean alpha is within tol of target (falls back to the nearest)."""
        ok = (np.abs(self.alpha - target) < tol) & (self.contrast >= min_contrast)
        idx = np.where(ok)[0]
        if len(idx) == 0:
            idx = np.argsort(np.abs(self.alpha - target))[:20]
        i = self.rng.choice(idx)
        return self.u[i], self.v[i]

    def summary(self):
        h, _ = np.histogram(self.alpha, 10, (0, 1))
        return 'patches %d, alpha histogram %s' % (len(self.u), h.tolist())


def face_uvs(R, F, D, target, lib, uv_per_m=(2.0, 4.0), jitter=0.0):
    """R (N,3) roots, F (M,3) faces (outward CCW), D (N,3) hair direction at each root (first guide segment),
    target (M,) wanted coverage per face. Returns (M*3, 2) corner UVs, (M,2) patch centres.
    The patch is oriented with atlas +v along the hair and scaled like the card texture (uv units per metre)."""
    out = np.zeros((len(F), 3, 2))
    cen = np.zeros((len(F), 2))
    for k, (a, b, c) in enumerate(F):
        P = R[[a, b, c]]
        n = np.cross(P[1] - P[0], P[2] - P[0])
        n /= max(np.linalg.norm(n), 1e-12)
        t = D[[a, b, c]].mean(0)
        t = t - n * (t @ n)
        if np.linalg.norm(t) < 1e-6:
            t = np.cross(n, [1.0, 0, 0])
        t /= np.linalg.norm(t)
        bx = np.cross(n, t)
        m = P.mean(0)
        uc, vc = lib.pick(float(target[k]))
        q = np.stack([(P - m) @ bx * uv_per_m[0], (P - m) @ t * uv_per_m[1]], 1)
        uvk = np.array([uc, vc]) + q
        # winding rule of every vanilla redfur: signed uv area < 0 (mirror across u if needed)
        s = (uvk[1, 0] - uvk[0, 0]) * (uvk[2, 1] - uvk[0, 1]) - (uvk[2, 0] - uvk[0, 0]) * (uvk[1, 1] - uvk[0, 1])
        if s > 0:
            q[:, 0] *= -1
            uvk = np.array([uc, vc]) + q
        out[k] = np.clip(uvk, 0.001, 0.999)
        cen[k] = (uc, vc)
    return out.reshape(-1, 2), cen


if __name__ == '__main__':
    lib = PatchLibrary(sys.argv[1] if len(sys.argv) > 1 else 'characters\\models\\common\\textures\\hair\\hair_long_04_d01.xbm',
                       exclude=[(0.0, 0.08, 0.235, 0.5), (0.9, 0.75, 1.0, 1.0)])
    print(lib.summary())
    for t in (0.2, 0.4, 0.6, 0.8):
        sel = np.abs(lib.alpha - t) < 0.05
        print('alpha %.1f: %d patches, luma %.2f, contrast %.2f' % (t, sel.sum(), lib.luma[sel].mean(), lib.contrast[sel].mean()))
