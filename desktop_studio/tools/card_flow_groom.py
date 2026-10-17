"""Card-flow groom: trace guides through the flow field of SHINGLE hair cards (many small single-quad cards).

Why: studio_rebuild_from_cards chains card lanes end to end and orients each island by skin distance. On shingle
cards (Ves: 1421 one-quad islands, 2-4 cm each) the chains fold back (zig-zag guides 10-40 cm long) and 28 % of the
islands get the wrong root end. Here the root -> tip axis of every card triangle comes from the texture itself
(+v of the hair atlas = towards the tips), so no orientation guess and no chaining is needed.

Output: a Studio project whose single op is a `rebuild` with delete_all (Groom::applyRebuild), so the Studio /
studio_geo export see plain guides + growth faces.

Usage (opt-in tool, nothing calls it by default):
  py desktop_studio/tools/card_flow_groom.py --pack build/desktop_studio/<cfg> --template <any project of that cfg>
     --out <new>.groomproj.json --params '<json>'
  then studio_open the project, check (hairline_check, trim_growth off_scalp, eye_check, check_loops) and studio_save.
Ves ves_mcp_6 params (2026-10-15): see build/desktop_studio/mcp/ves1/ves_mcp_6/cardflow_params.json, i.e.
  h0_mm 3 (roots 3 mm off the faceted skin: below ~1 mm the head hides the hair = old crown holes), shell_pct 88,
  layer 0.85-1, rise_mm 14, hmax_mm 40, h_monotone 1 (tips do not dive into the volume: Studio shadow blotches),
  end3d_mm 8, crowd_mm 3 / crowd_n 5, temple cap hmax_side_mm 10 (side_z0 0.75, side_z1 0.9, side_front_mm -70),
  ears = studio_clear_ear centres. Look (mcp_6): lengthNoise 0.2 (studio_look_set after
  opening); density stays 3.0 because this Studio build does not draw volume.density changes, so a lower density
  cannot be judged here.
Options not used for Ves: rsmooth (radial smoothing, shrinks the shell), part_rise_mm (opened a hole, not a part line),
  temple_stop, end_foot_mm (no effect on her), fringe_gaps (dropping hairline faces opened the scalp).
Ves ves_mcp_7 (2026-10-15, less bulk, choppy): the mcp_6 params plus shell_pct 75, edge_h_mm 6 + edge_ymin 0.045 (past
  the painted hairline at the front the hair lies down at <= 6 mm instead of keeping its height), tip_drop 0.5 (the last
  half of each strand sinks halfway to the skin: layered, tapered ends), fringe_wisps {period_mm 7, pull 0.85, ymin .05,
  zmin 1.66, t0 .4, no_drop} (fringe ends gathered into separate locks). Params: ves_mcp_7/cardflow_params.json, look
  values: ves_mcp_7/look_set.json.
Ves ves_mcp_8 (2026-10-15, even volume, no crown mushroom): mcp_7 plus spacing_mm 4.35, rise_mm 6, tip_drop .25,
  edge_h_mm 16, back_hmax {pts [[-30,6],[-10,9],[5,14],[25,20],[50,23],[90,23]] (elevation deg from the head centre ->
  max height mm), y0_mm 0, blend_mm 25} (behind the ears the cap tapers to the nape: no lip), h_floor {pts
  [[0,0],[25,14],[50,19],[75,21],[90,22]], after_mm 3} (minimum height on the scalp: fills the flat top front).
"""
import json, math, os, sys, argparse
import numpy as np
from PIL import Image
from scipy.spatial import cKDTree, Delaunay
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components


def load_pack(pack_dir):
    D = json.load(open(os.path.join(pack_dir, "desk.json")))
    return D


def _closest_on_tris(p, a, b, c):
    ab = b - a; ac = c - a; ap = p - a
    d1 = (ab * ap).sum(1); d2 = (ac * ap).sum(1)
    bp = p - b; d3 = (ab * bp).sum(1); d4 = (ac * bp).sum(1)
    cp = p - c; d5 = (ab * cp).sum(1); d6 = (ac * cp).sum(1)
    vc = d1 * d4 - d3 * d2; vb = d5 * d2 - d1 * d6; va = d3 * d6 - d5 * d4
    den = np.where(np.abs(va + vb + vc) < 1e-20, 1e-20, va + vb + vc)
    v = vb / den; w = vc / den
    res = a + ab * v[:, None] + ac * w[:, None]
    # regions
    m = (d1 <= 0) & (d2 <= 0); res[m] = a[m]
    m = (d3 >= 0) & (d4 <= d3); res[m] = b[m]
    m = (d6 >= 0) & (d5 <= d6); res[m] = c[m]
    m = (vc <= 0) & (d1 >= 0) & (d3 <= 0)
    t = d1 / np.where(np.abs(d1 - d3) < 1e-20, 1e-20, d1 - d3); res[m] = (a + ab * t[:, None])[m]
    m = (vb <= 0) & (d2 >= 0) & (d6 <= 0)
    t = d2 / np.where(np.abs(d2 - d6) < 1e-20, 1e-20, d2 - d6); res[m] = (a + ac * t[:, None])[m]
    m = (va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0)
    t = (d4 - d3) / np.where(np.abs((d4 - d3) + (d5 - d6)) < 1e-20, 1e-20, (d4 - d3) + (d5 - d6)); res[m] = (b + (c - b) * t[:, None])[m]
    return res


class Head:
    def __init__(self, D, pack_dir):
        ht = D["head_tex"]
        self.V = np.array(ht["v"]); self.T = np.array(ht["t"]); self.UV = np.array(ht["uv"])
        self.C = np.array(D["head"]["c"]); self.top = D["head"]["top"]
        im = np.asarray(Image.open(os.path.join(pack_dir, ht["file"])).convert("RGB")).astype(np.float32)
        self.lum_img = 0.3 * im[..., 0] + 0.59 * im[..., 1] + 0.11 * im[..., 2]
        H, W = self.lum_img.shape
        # 9x9 box blur like the Studio's lumAt
        k = 4
        pad = np.pad(self.lum_img, k, mode="edge")
        cs = pad.cumsum(0).cumsum(1)
        cs = np.pad(cs, ((1, 0), (1, 0)))
        n = 2 * k + 1
        self.blur = (cs[n:, n:] - cs[:-n, n:] - cs[n:, :-n] + cs[:-n, :-n]) / (n * n)
        self.W, self.H = W, H
        # eye top (meshes whose texture name has 'eye')
        eye_top = -1e9
        for o in D["outfit_tex"]:
            if "eye" in o["file"]:
                eye_top = max(eye_top, max(v[2] for v in o["v"]))
        self.eye_top = eye_top
        self.eyes = [np.array(o["v"]) for o in D["outfit_tex"] if "eye" in o["file"]]
        self.brow_z = eye_top + 0.035 if eye_top > -1e8 else self.C[2] + 0.05
        # face-sample KD (dense)
        a, b, c = self.V[self.T[:, 0]], self.V[self.T[:, 1]], self.V[self.T[:, 2]]
        nrm = np.cross(b - a, c - a)
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
        s = np.sign((((a + b + c) / 3 - self.C) * nrm).sum(1))
        s[s == 0] = 1
        nrm *= s[:, None]
        self.FN = nrm
        key = np.round(self.V * 1e4).astype(np.int64)
        _, inv = np.unique(key, axis=0, return_inverse=True)
        inv = inv.reshape(-1)
        area = np.linalg.norm(np.cross(b - a, c - a), axis=1, keepdims=True)
        acc = np.zeros((inv.max() + 1, 3))
        for k in range(3):
            np.add.at(acc, inv[self.T[:, k]], nrm * area)
        acc /= np.maximum(np.linalg.norm(acc, axis=1, keepdims=True), 1e-12)
        self.VN = acc[inv]
        P, N, TI, BA = [], [], [], []
        n = 8
        for i in range(n + 1):
            for j in range(n + 1 - i):
                u, v = i / n, j / n
                w = 1 - u - v
                P.append(a * w + b * u + c * v); N.append(nrm); TI.append(np.arange(len(self.T))); BA.append(np.tile([w, u, v], (len(self.T), 1)))
        self.SP = np.concatenate(P); self.SN = np.concatenate(N); self.STI = np.concatenate(TI); self.SBA = np.concatenate(BA)
        self.tree = cKDTree(self.SP)
        self._scalp_mask()

    def lum_at(self, uv):
        x = np.clip((uv[..., 0] * self.W).astype(int), 0, self.W - 1)
        y = np.clip((uv[..., 1] * self.H).astype(int), 0, self.H - 1)
        return self.blur[y, x]

    def _scalp_mask(self):
        V, UV, C = self.V, self.UV, self.C
        sel = (V[:, 1] > C[1] + 0.06) & (V[:, 2] > C[2] - 0.06) & (V[:, 2] < C[2] - 0.01)
        skin = np.median(self.lum_at(UV[sel])) if sel.any() else 140
        self.thr = min(95.0, 0.62 * skin)
        front_low = (V[:, 1] > C[1] + 0.05) & (V[:, 2] < self.brow_z)
        dark = (~front_low) & (self.lum_at(UV) < self.thr)
        key = np.round(V * 1e4).astype(np.int64)
        _, wid, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
        inv = inv.reshape(-1)
        nw = inv.max() + 1
        wdark = np.zeros(nw, bool); wz = np.zeros(nw)
        np.logical_or.at(wdark, inv, dark)
        wz[inv] = V[:, 2]
        T = self.T
        rows = np.concatenate([inv[T[:, 0]], inv[T[:, 1]], inv[T[:, 2]]])
        cols = np.concatenate([inv[T[:, 1]], inv[T[:, 2]], inv[T[:, 0]]])
        A = coo_matrix((np.ones(len(rows)), (rows, cols)), shape=(nw, nw)).tocsr()
        A = A + A.T
        z_top = wz.max()
        inn = wdark & (wz > z_top - 0.02)
        q = list(np.nonzero(inn)[0])
        while q:
            i = q.pop()
            for j in A.indices[A.indptr[i]:A.indptr[i + 1]]:
                if not inn[j] and wdark[j] and wz[j] > C[2]:
                    inn[j] = True; q.append(j)
        self.vmask = inn[inv]
        self.vtree = cKDTree(V)
        self.front_low = front_low
        print("scalp mask: skin %.0f thr %.0f brow %.3f, %d / %d vertices" % (skin, self.thr, self.brow_z, self.vmask.sum(), len(V)))

    def foot(self, P):
        d, i = self.tree.query(P)
        n = self.SN[i]
        h = ((P - self.SP[i]) * n).sum(-1)
        return P - n * h[..., None], n, h, i

    def closest(self, p):
        """exact closest point on the head mesh to one point p: (point, outward normal, signed height, tri)."""
        _, ii = self.tree.query(p, k=24)
        tris = np.unique(self.STI[ii])
        a = self.V[self.T[tris, 0]]; b = self.V[self.T[tris, 1]]; c = self.V[self.T[tris, 2]]
        q = _closest_on_tris(p, a, b, c)
        dd = np.linalg.norm(q - p, axis=1)
        k = np.argmin(dd)
        t = self.T[tris[k]]
        A, B, Cc = self.V[t[0]], self.V[t[1]], self.V[t[2]]
        v0, v1, v2 = B - A, Cc - A, q[k] - A
        d00, d01, d11, d20, d21 = v0 @ v0, v0 @ v1, v1 @ v1, v2 @ v0, v2 @ v1
        den = d00 * d11 - d01 * d01
        if abs(den) < 1e-20:
            n = self.FN[tris[k]]
        else:
            bv = (d11 * d20 - d01 * d21) / den; bw = (d00 * d21 - d01 * d20) / den; bu = 1 - bv - bw
            n = bu * self.VN[t[0]] + bv * self.VN[t[1]] + bw * self.VN[t[2]]
            n /= max(np.linalg.norm(n), 1e-12)
        return q[k], n, float((p - q[k]) @ self.FN[tris[k]]), tris[k]

    def on_scalp(self, si):
        """sample index -> painted scalp test as the Studio shader does (triangle verts in mask + texel darker than thr)."""
        t = self.T[self.STI[si]]
        ba = self.SBA[si]
        uv = (self.UV[t] * ba[..., None]).sum(-2)
        texel = self.lum_at(uv) < self.thr
        vm = self.vmask[t].sum(-1) >= 2
        # the Studio's hairline_check: flag of the nearest head vertex
        _, vi = self.vtree.query(self.SP[si])
        return texel & vm & self.vmask[vi]


class Cards:
    def __init__(self, D, pack_dir, alpha_ref=0.3, n=4):
        hc = D["hair_cards"][0]
        V = np.array(hc["v"]); T = np.array(hc["t"]); UV = np.array(hc["uv"])
        tex = np.asarray(Image.open(os.path.join(pack_dir, hc["file"])).convert("RGBA")).astype(np.float32) / 255
        alpha = tex[..., 3]
        Ht, Wt = alpha.shape
        a, b, c = T[:, 0], T[:, 1], T[:, 2]
        e1 = V[b] - V[a]; e2 = V[c] - V[a]
        d1 = UV[b] - UV[a]; d2 = UV[c] - UV[a]
        det = d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]
        ok = np.abs(det) > 1e-9
        det = np.where(ok, det, 1)
        Pv = (-d2[:, 0:1] * e1 + d1[:, 0:1] * e2) / det[:, None]
        P, TT, AL, VF, SC = [], [], [], [], []
        for i in range(n + 1):
            for j in range(n + 1 - i):
                u, v = (i + 0.33) / (n + 1), (j + 0.33) / (n + 1)
                if u + v > 1: continue
                w = 1 - u - v
                p = V[a] * w + V[b] * u + V[c] * v
                uv = UV[a] * w + UV[b] * u + UV[c] * v
                fr = uv - np.floor(uv)
                x = np.clip((fr[:, 0] * Wt).astype(int), 0, Wt - 1); y = np.clip((fr[:, 1] * Ht).astype(int), 0, Ht - 1)
                P.append(p); TT.append(Pv); AL.append(alpha[y, x]); VF.append(fr[:, 1]); SC.append(np.linalg.norm(Pv, axis=1))
        P = np.concatenate(P); TT = np.concatenate(TT); AL = np.concatenate(AL); VF = np.concatenate(VF); SC = np.concatenate(SC)
        okk = np.concatenate([ok] * (len(P) // len(ok)))
        keep = okk & (AL > alpha_ref)
        self.P = P[keep]
        t = TT[keep]
        self.T = t / np.maximum(np.linalg.norm(t, axis=1, keepdims=True), 1e-9)
        self.A = AL[keep]
        self.rem = ((1 - VF) * SC)[keep]   # texture-implied hair still to come past this point (m)
        print("card samples (visible):", len(self.P))


def poisson(points, r, rng, order=None):
    tree = cKDTree(points)
    idx = order if order is not None else rng.permutation(len(points))
    taken = np.zeros(len(points), bool); blocked = np.zeros(len(points), bool)
    out = []
    for i in idx:
        if blocked[i]: continue
        out.append(i)
        for j in tree.query_ball_point(points[i], r):
            blocked[j] = True
    return np.array(out)


def resample(poly, K):
    seg = np.linalg.norm(np.diff(poly, axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)])
    t = np.linspace(0, s[-1], K)
    return np.stack([np.interp(t, s, poly[:, k]) for k in range(3)], 1), s[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", required=True)
    ap.add_argument("--template", required=True, help="project to copy the header from")
    ap.add_argument("--out", required=True)
    ap.add_argument("--params", default="{}")
    a = ap.parse_args()
    prm = dict(spacing_mm=4.5, step_mm=2.5, R_mm=9.0, inertia=0.55, inner_bias_mm=10.0, layer_min=0.25, layer_max=0.8,
               shell_pct=75, rise_mm=25.0, h0_mm=0.8, hmax_mm=26.0, min_len_mm=35.0, max_len_mm=140.0, end_support_mm=7.0,
               end_need=3, reach_mm=30.0, edge_mm=14.0, diverge_deg=100.0, seed=7, K=13, len_scale=1.0,
               rem_mix=0.0, eye_mm=12.0, part_cross=0.0, h_tip_mm=None)
    prm.update(json.loads(a.params))
    rng = np.random.default_rng(prm["seed"])
    D = load_pack(a.pack)
    head = Head(D, a.pack)
    cards = Cards(D, a.pack)
    cfoot, cn, ch, _ = head.foot(cards.P)
    cards.h = ch
    # tangential flow of each sample on the skin
    tt = cards.T - (cards.T * cn).sum(1, keepdims=True) * cn
    cards.Tt = tt / np.maximum(np.linalg.norm(tt, axis=1, keepdims=True), 1e-9)
    ftree = cKDTree(cfoot)
    ctree = cKDTree(cards.P)
    # candidate roots: head samples on the painted scalp with card hair above them
    scal = head.on_scalp(np.arange(len(head.SP)))
    cand = np.nonzero(scal)[0]
    d, _ = ftree.query(head.SP[cand])
    cand = cand[d < prm["reach_mm"] / 1000]
    _, vi = head.vtree.query(head.SP[cand] + head.SN[cand] * prm["h0_mm"] / 1000)
    cand = cand[head.vmask[vi]]
    pick = cand[poisson(head.SP[cand], prm["spacing_mm"] / 1000, rng)]
    roots = head.SP[pick]; rn = head.SN[pick]
    print("scalp samples", scal.sum(), "candidates", len(cand), "roots", len(roots))
    R = prm["R_mm"] / 1000
    step = prm["step_mm"] / 1000
    guides, lens, stops, reasons = [], [], {}, []
    eyesP = np.concatenate(head.eyes) if head.eyes else np.zeros((0, 3))
    eye_tree = cKDTree(eyesP) if len(eyesP) else None

    def flow_at(f, h_target, prev):
        ids = ftree.query_ball_point(f, R)
        if len(ids) < prm["end_need"]:
            return None, 0, None
        ids = np.array(ids)
        lat = np.linalg.norm(cfoot[ids] - f, axis=1)
        if prm.get("layer_match_mm") and h_target > 0:
            hz = np.exp(-np.abs(cards.h[ids] - h_target) / (prm["layer_match_mm"] / 1000))
        else:
            hz = np.exp(-np.maximum(cards.h[ids], 0) / (prm["inner_bias_mm"] / 1000))
        w = cards.A[ids] * np.exp(-(lat / (0.5 * R)) ** 2) * hz
        t = cards.Tt[ids]
        if prev is not None:
            # cluster: keep the samples flowing the same way as prev (partings / whorls do not average out)
            same = (t @ prev) > 0
            if same.sum() >= prm["end_need"]:
                w = w * np.where(same, 1.0, 0.05)
        v = (w[:, None] * t).sum(0)
        nv = np.linalg.norm(v)
        sp = prm["shell_pct"]
        if prm.get("shell_pct_side") is not None:
            uz = (f - head.C); uz = uz[2] / max(np.linalg.norm(uz), 1e-9)
            tz = min(1.0, max(0.0, (uz - prm.get("side_z0", 0.15)) / (prm.get("side_z1", 0.6) - prm.get("side_z0", 0.15))))
            sp = prm["shell_pct_side"] + (prm["shell_pct"] - prm["shell_pct_side"]) * tz
        shell = np.percentile(cards.h[ids], sp)
        rem = np.average(cards.rem[ids], weights=w + 1e-9)
        return (v / nv if nv > 1e-9 else None), len(ids), (shell, rem)

    rise_g = np.full(len(roots), prm["rise_mm"] / 1000)
    if prm.get("part_rise_mm"):
        # roots on a parting (neighbours flow the opposite way) rise slowly so the part stays visible
        d0 = np.zeros((len(roots), 3))
        for gi, r0 in enumerate(roots):
            v, _, _ = flow_at(r0, 0, None)
            if v is not None:
                v2, _, _ = flow_at(r0, 0, v)
                d0[gi] = v2 if v2 is not None else v
        rt = cKDTree(roots)
        npart = 0
        for gi, l in enumerate(rt.query_ball_point(roots, prm.get("part_r_mm", 8) / 1000)):
            if np.linalg.norm(d0[gi]) == 0: continue
            if roots[gi][2] < prm.get("part_zmin", 1.72): continue
            if any((d0[j] @ d0[gi]) < prm.get("part_cos", -0.2) for j in l if j != gi):
                rise_g[gi] = prm["part_rise_mm"] / 1000; npart += 1
        print("part roots", npart)
    for gi, (r0, n0) in enumerate(zip(roots, rn)):
        layer = rng.uniform(prm["layer_min"], prm["layer_max"])
        f = r0.copy(); n = n0.copy()
        v, cnt, info = flow_at(f, 0, None)
        if v is None:
            R0 = R; R = 2 * R0
            v, cnt, info = flow_at(f, 0, None)
            R = R0
        if v is None:
            stops["no_flow"] = stops.get("no_flow", 0) + 1
            continue
        # initial direction: the dominant cluster around the root
        prev = v
        v2, _, _ = flow_at(f, 0, prev)
        if v2 is not None: prev = v2
        rem0 = info[1]
        Lmax = prm["max_len_mm"] / 1000
        if prm["rem_mix"] > 0:
            Lmax = min(Lmax, max(prm["min_len_mm"] / 1000, prm["rem_mix"] * rem0))
        Lmax *= prm["len_scale"]
        pts = [r0 + n0 * prm["h0_mm"] / 1000]
        s = 0.0; dirv = prev; reason = "max_len"; shell_s = None
        h_cur = layer * (info[0] if info else 0.01)
        while s < Lmax:
            v, cnt, info = flow_at(f, h_cur, dirv)
            if v is None:
                if s >= prm["min_len_mm"] / 1000:
                    reason = "end_of_cards"; break
                v = dirv; info = (info[0] if info else 0.01, 0) if info else (0.01, 0)
            d = prm["inertia"] * dirv + (1 - prm["inertia"]) * v
            d = d - (d @ n) * n
            nd = np.linalg.norm(d)
            if nd < 1e-9: reason = "stall"; break
            d /= nd
            f2 = f + d * step
            f, n, _, _ = head.closest(f2)
            if prm.get("ears"):
                hit = False
                for ec in prm["ears"]:
                    if abs(f[0]) > prm.get("ear_x", 0.076) and np.linalg.norm(f - np.array(ec)) < prm.get("ear_r_mm", 28) / 1000:
                        hit = True
                if hit:
                    reason = "ear"; break
            dirv = d
            s += step
            shell = info[0] if info else 0.01
            shell_s = shell if shell_s is None else shell_s + prm.get("shell_smooth", 0.25) * (shell - shell_s)
            shell = shell_s
            hmax = prm["hmax_mm"] / 1000
            wside = 0.0
            if prm.get("hmax_side_mm") is not None:
                uz = (f - head.C); uz = uz[2] / max(np.linalg.norm(uz), 1e-9)
                tz = min(1.0, max(0.0, (uz - prm.get("side_z0", 0.15)) / (prm.get("side_z1", 0.6) - prm.get("side_z0", 0.15))))
                wside = 1 - tz
                if prm.get("side_front_mm") is not None:
                    fy = (f[1] - head.C[1] - prm["side_front_mm"] / 1000) / 0.02
                    wside *= min(1.0, max(0.0, fy))
                hmax = (prm["hmax_mm"] + (prm["hmax_side_mm"] - prm["hmax_mm"]) * wside) / 1000
            bh = prm.get("back_hmax")
            if bh:
                # close crop behind the ears: the height cap follows the elevation (crown -> nape) so the back hugs the skull
                ub = f - head.C; ub = ub / max(np.linalg.norm(ub), 1e-9)
                el_ = np.degrees(np.arcsin(ub[2]))
                ptsb = np.array(bh["pts"], float)
                cap = np.interp(el_, ptsb[:, 0], ptsb[:, 1]) / 1000
                wb = min(1.0, max(0.0, (head.C[1] + bh.get("y0_mm", 0) / 1000 - f[1]) / (bh.get("blend_mm", 25) / 1000)))
                if cap < hmax:
                    hmax = hmax + (cap - hmax) * wb
            target = min(max(layer * shell, prm["h0_mm"] / 1000), hmax)
            off_scalp = False
            if prm.get("edge_h_mm") is not None:
                _, vi_ = head.vtree.query(f)
                off_scalp = (not head.vmask[vi_]) and f[1] > prm.get("edge_ymin", -1.0)
            if prm.get("h_monotone") and s > rise_g[gi] and wside < 0.5 and not off_scalp:
                target = max(target, h_cur * prm.get("h_monotone"))
            hf = prm.get("h_floor")
            if hf and not off_scalp and s > hf.get("after_mm", 6) / 1000:
                # even volume: minimum height over the skin by elevation (fills a flat top front next to a fuller back)
                uf = f - head.C; uf = uf / max(np.linalg.norm(uf), 1e-9)
                pf = np.array(hf["pts"], float)
                target = max(target, np.interp(np.degrees(np.arcsin(uf[2])), pf[:, 0], pf[:, 1]) / 1000)
            target = min(target, hmax)
            if off_scalp:
                # past the painted hairline (forehead / temple skin) the hair lies down on the skin like the cards do
                target = min(target, prm["edge_h_mm"] / 1000)
            ramp = min(1.0, s / rise_g[gi])
            ramp = ramp * ramp * (3 - 2 * ramp)
            h = prm["h0_mm"] / 1000 + (target - prm["h0_mm"] / 1000) * ramp
            p = f + n * h
            h_cur = target
            if prm.get("end3d_mm") and s >= prm["min_len_mm"] / 1000:
                if ctree.query(p)[0] > prm["end3d_mm"] / 1000:
                    reason = "left_cards_3d"; break
            if eye_tree is not None and eye_tree.query(p)[0] < prm["eye_mm"] / 1000:
                reason = "eye"; break
            if prm.get("end_foot_mm") and s >= prm["min_len_mm"] / 1000:
                if ftree.query(f)[0] > prm["end_foot_mm"] / 1000:
                    reason = "off_card_feet"; break
            ts = prm.get("temple_stop")
            if ts and s >= prm["min_len_mm"] / 1000 and abs(p[0]) > ts["xmin"] and p[1] > ts["ymin"] and p[2] < ts["zmin"]:
                reason = "temple"; break
            pts.append(p)
        stops[reason] = stops.get(reason, 0) + 1
        poly = np.array(pts)
        if len(poly) < 3:
            continue
        for _ in range(prm.get("smooth_passes", 3)):
            poly[1:-1] = 0.5 * poly[1:-1] + 0.25 * (poly[:-2] + poly[2:])
        g, L = resample(poly, prm["K"])
        if L < 0.012:
            stops["too_short"] = stops.get("too_short", 0) + 1
            continue
        guides.append(g); lens.append(L); reasons.append(reason)
    G = np.array(guides)
    L = np.array(lens)
    if prm.get("tip_drop"):
        # the ends lie down: lower the height above the skin over the last part of each strand (tapered outline)
        t0 = prm.get("tip_drop_t0", 0.5); a_ = prm["tip_drop"]; hmin = prm["h0_mm"] / 1000
        K_ = G.shape[1]; tt_ = np.linspace(0, 1, K_)
        for gi in range(len(G)):
            for k in range(1, K_):
                if tt_[k] <= t0: continue
                u_ = (tt_[k] - t0) / (1 - t0); w_ = a_ * u_ * u_ * (3 - 2 * u_)
                q, n, hh, _ = head.closest(G[gi, k])
                if hh > hmin:
                    G[gi, k] = q + n * (hmin + (hh - hmin) * (1 - w_))
    tj = prm.get("tip_jitter")
    if tj:
        # separated, spiky groups: per-guide random length cut and a random outward lift of the end (tips break the outline)
        rj = np.random.default_rng(tj.get("seed", 11)); K_ = G.shape[1]; tt_ = np.linspace(0, 1, K_)
        t1 = tj.get("t0", 0.55)
        for gi in range(len(G)):
            cut = 1 - rj.uniform(0, tj.get("len_var", 0))
            if cut < 0.999:
                g2, _ = resample(G[gi][: max(3, int(np.ceil(cut * (K_ - 1))) + 1)], K_)
                G[gi] = g2
            if rj.uniform() < tj.get("frac", 0.3):
                lift = rj.uniform(0.3, 1.0) * tj.get("lift_mm", 5) / 1000
                for k in range(K_):
                    if tt_[k] <= t1: continue
                    u_ = (tt_[k] - t1) / (1 - t1)
                    q, n, hh, _ = head.closest(G[gi, k])
                    G[gi, k] = G[gi, k] + n * lift * u_ * u_
    if prm.get("rsmooth"):
        # smooth the outer surface: blend each point's distance from the head centre with its neighbours' (no lumps / shelves)
        Cc = head.C; N_, K_ = G.shape[:2]
        for it in range(prm["rsmooth"]):
            P = G.reshape(-1, 3) - Cc; r = np.linalg.norm(P, axis=1); U = P / r[:, None]
            gid = np.repeat(np.arange(N_), K_); kk = np.tile(np.arange(K_), N_)
            nb = cKDTree(U).query_ball_point(U, prm.get("rsmooth_ang", 0.05))
            rn_ = r.copy()
            for i, l in enumerate(nb):
                if kk[i] < 2: continue
                l = [j for j in l if gid[j] != gid[i] and kk[j] >= prm.get("rsmooth_kmin", 2)]
                if len(l) >= 4:
                    w = min(1.0, (kk[i] - 1) / 3.0) * prm.get("rsmooth_w", 0.5)
                    ref = np.percentile(r[l], prm["rsmooth_pct"]) if prm.get("rsmooth_pct") else np.mean(r[l])
                    rn_[i] = (1 - w) * r[i] + w * max(ref, r[i] if kk[i] < 4 else 0)
            G = (U * rn_[:, None] + Cc).reshape(N_, K_, 3)
    if prm.get("crowd_mm"):
        # converging flows bundle many guides into one line (a spike): shorten guides where they crowd
        K = G.shape[1]
        cut = 0
        alive_pts = np.ones(G.shape[:2], bool)
        order = rng.permutation(len(G))
        for it in range(2):
            pts = G.reshape(-1, 3)[alive_pts.reshape(-1)]
            owner = np.repeat(np.arange(len(G)), K)[alive_pts.reshape(-1)]
            tr = cKDTree(pts)
            for gi in order:
                for k in range(3, K):
                    if not alive_pts[gi, k]: break
                    nb = tr.query_ball_point(G[gi, k], prm["crowd_mm"] / 1000)
                    others = len(set(owner[nb]) - {gi})
                    if others >= prm.get("crowd_n", 6):
                        # keep at least min_keep of the length
                        kk = max(k, int(math.ceil(prm.get("crowd_keep", 0.45) * (K - 1))))
                        if kk < K - 1:
                            alive_pts[gi, kk + 1:] = False
                            cut += 1
                        break
        newG = []
        for gi in range(len(G)):
            n_alive = alive_pts[gi].sum()
            if n_alive < K:
                g, _ = resample(G[gi, :n_alive], K)
                newG.append(g)
            else:
                newG.append(G[gi])
        G = np.array(newG)
        L = np.linalg.norm(np.diff(G, axis=1), axis=2).sum(1)
        print("crowd: shortened", cut)
    print("guides", len(G), "stops", stops)
    print("length pct 5/25/50/75/95/max", np.percentile(L, [5, 25, 50, 75, 95, 100]).round(3))
    wisp_id = None
    fw = prm.get("fringe_wisps")
    if fw:
        # fringe -> separate wisps: guides whose tips end on the forehead are grouped in bands across the forehead and
        # their ends pulled together (pointed locks); faces that join two bands are dropped below (gaps between locks)
        tips = G[:, -1]
        _, vt = head.vtree.query(tips)
        fr = (~head.vmask[vt]) & (tips[:, 1] > fw.get("ymin", 0.07)) & (tips[:, 2] > fw.get("zmin", 1.69))
        wisp_id = np.full(len(G), -1)
        wisp_id[fr] = np.floor(tips[fr, 0] / (fw["period_mm"] / 1000)).astype(int)
        K_ = G.shape[1]; tt_ = np.linspace(0, 1, K_); t0 = fw.get("t0", 0.45)
        u_ = np.clip((tt_ - t0) / (1 - t0), 0, 1); wk = fw.get("pull", 0.7) * u_ * u_ * (3 - 2 * u_)
        for wid in np.unique(wisp_id[fr]):
            idx = np.nonzero(wisp_id == wid)[0]
            if len(idx) < 2: continue
            mean = G[idx].mean(0)
            for i in idx:
                G[i] = G[i] * (1 - wk[:, None]) + mean * wk[:, None]
        print("fringe wisps", len(np.unique(wisp_id[fr])), "guides", int(fr.sum()))
    # growth faces: stereographic map around the head centre
    C = head.C
    u = G[:, 0] - C
    u /= np.linalg.norm(u, axis=1, keepdims=True)
    XY = u[:, :2] / (1 + u[:, 2:3])
    tri = Delaunay(XY).simplices
    e = lambda i, j: np.linalg.norm(G[tri[:, i], 0] - G[tri[:, j], 0], axis=1)
    emax = np.maximum(np.maximum(e(0, 1), e(1, 2)), e(2, 0))
    d0 = G[:, 3] - G[:, 0]
    d0 /= np.maximum(np.linalg.norm(d0, axis=1, keepdims=True), 1e-9)
    cosmin = np.minimum(np.minimum((d0[tri[:, 0]] * d0[tri[:, 1]]).sum(1), (d0[tri[:, 1]] * d0[tri[:, 2]]).sum(1)), (d0[tri[:, 2]] * d0[tri[:, 0]]).sum(1))
    keepf = (emax < prm["edge_mm"] / 1000) & (cosmin > math.cos(math.radians(prm["diverge_deg"])))
    fg = prm.get("fringe_gaps")
    if fg:
        # separate the fringe into wisps: drop growth faces in bands across the fringe (faces whose strands end on the
        # forehead skin), so HairWorks does not fill one continuous sheet there
        tips = G[:, -1]
        _, vt = head.vtree.query(tips)
        on_face = (~head.vmask[vt]) & (tips[:, 1] > fg.get("ymin", 0.07)) & (tips[:, 2] > fg.get("zmin", 1.69))
        tf = on_face[tri].sum(1) >= fg.get("need", 2)
        xc = tips[tri].mean(1)[:, 0]
        per = fg["period_mm"] / 1000; gap = fg["gap_mm"] / 1000
        drop = tf & (np.mod(xc - fg.get("phase_mm", 0) / 1000, per) < gap)
        print("fringe faces", int(tf.sum()), "dropped for wisps", int((drop & keepf).sum()))
        keepf = keepf & ~drop
    if wisp_id is not None:
        wi = wisp_id[tri]
        if fw.get("drop_all_only", False):
            mixed = (wi >= 0).all(1) & (wi.max(1) != wi.min(1))
        else:
            mixed = np.array([len(set(int(x) for x in r if x >= 0)) >= 2 for r in wi], bool)
        if fw.get("no_drop"):
            mixed[:] = False
        print("faces joining two fringe wisps dropped", int((mixed & keepf).sum()))
        keepf = keepf & ~mixed
    # wind faces outward
    F = tri[keepf]
    a_, b_, c_ = G[F[:, 0], 0], G[F[:, 1], 0], G[F[:, 2], 0]
    nr = np.cross(b_ - a_, c_ - a_)
    flip = ((nr * ((a_ + b_ + c_) / 3 - C)).sum(1)) < 0
    F[flip] = F[flip][:, [0, 2, 1]]
    print("faces", len(F), "dropped long", int((emax >= prm["edge_mm"] / 1000).sum()), "dropped divergent", int(((emax < prm["edge_mm"] / 1000) & ~keepf).sum()))
    # project
    tpl = json.load(open(a.template))
    base_roots = np.array([g[0] for g in D["guides_edit"]])
    bt = cKDTree(base_roots)
    _, bi = bt.query(G[:, 0])
    uv = D.get("_uv_uniform", [0.9934, 0.2004])
    op = {"op": "rebuild", "id": 2, "grp": 1, "delete_all": True,
          "guides": np.round(G, 5).tolist(), "base": np.round(base_roots[bi], 5).tolist(),
          "uv": [uv] * len(G), "faces": F.tolist()}
    tpl["ops"] = [op]
    tpl["look"] = json.loads(a.params).get("_look", tpl.get("look", {}))
    json.dump(tpl, open(a.out, "w"))
    json.dump({"params": prm, "guides": len(G), "faces": len(F), "stops": stops,
               "length_pct": np.percentile(L, [5, 25, 50, 75, 95, 100]).round(4).tolist(), "reasons": reasons}, open(a.out + ".report.json", "w"), indent=1)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
