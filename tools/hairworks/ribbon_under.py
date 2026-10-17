r"""ribbon_under - opt-in config key "ribbon_under" (2026-10-13, Ciri): THIN custom strand-looking ribbons drawn with the fur
under the HairWorks hair where the scalp looks empty (crown behind the parting, thin spots on top / back). Not vanilla
cards: new geometry, narrow tapered ribbons lying along the head surface, following the flow of the nearest guides, UV
mapped to narrow strips of the character's own card atlas so the alpha gives single strand edges.

config: "ribbon_under": {"width_mm": 4.5, "len_mm": [30, 70], "spacing_mm": 7, "lift_mm": 1.5,
                         "regions": ["crown", "top", "back"], "cover_mm": 10, "cover_need": 14, "seed": 7, "max": 560}
Everything optional. The mesh is the character's hair mesh with its LOD0 chunk's vertex slots / indices rewritten
(same vertex format and bones as the cards), written as <mesh>_ribbons.w2mesh by hairpipe.build.
"""
import struct
import zlib

import numpy as np

import gamefiles as G
import w3lib

# atlas strips (u ranges) with good single strand alpha, hair_long_03_d01 (root at v ~ 0.01, tip at v ~ 1)
STRIPS = [(0.02, 0.20), (0.48, 0.60), (0.79, 0.92)]
REGIONS = {
    'top': lambda v, h: (v[:, 2] > h.top - 0.085) & (v[:, 1] < h.c[1] + 0.02),
    'crown': lambda v, h: (v[:, 2] > h.top - 0.12) & (np.abs(v[:, 0]) < 0.05) & (v[:, 1] < h.c[1] + 0.005),
    'back': lambda v, h: (v[:, 1] < h.c[1] - 0.01) & (v[:, 2] > h.top - 0.17),
}


def _sample_surface(head, rng, n):
    a, b, c = head.v[head.t[:, 0]], head.v[head.t[:, 1]], head.v[head.t[:, 2]]
    ar = np.linalg.norm(np.cross(b - a, c - a), axis=1)
    ti = rng.choice(len(ar), n, p=ar / ar.sum())
    r1, r2 = rng.random(n), rng.random(n)
    s = np.sqrt(r1)
    w = np.stack([1 - s, s * (1 - r2), s * r2], 1)
    P = a[ti] * w[:, :1] + b[ti] * w[:, 1:2] + c[ti] * w[:, 2:]
    N = (head.n[head.t[ti, 0]] * w[:, :1] + head.n[head.t[ti, 1]] * w[:, 1:2] + head.n[head.t[ti, 2]] * w[:, 2:])
    return P, N / np.linalg.norm(N, axis=1, keepdims=True)


def build(cfg, head, guides, log=print):
    """-> dict(pos (n,3), uv (n,2), tris (m,3), nribbons). guides: list of (k,3) root->tip polylines."""
    from scipy.spatial import cKDTree
    rng = np.random.default_rng(cfg.get('seed', 7))
    wmm = cfg.get('width_mm', 4.5) / 1000.0
    lo, hi = [x / 1000.0 for x in cfg.get('len_mm', [30, 70])]
    sp = cfg.get('spacing_mm', 7) / 1000.0
    lift = cfg.get('lift_mm', 1.5) / 1000.0
    regs = cfg.get('regions', ['crown', 'top', 'back'])
    cov_r = cfg.get('cover_mm', 10) / 1000.0
    need = cfg.get('cover_need', 14)
    mx = cfg.get('max', 560)
    gp, gd = [], []
    for g in guides:
        g = np.asarray(g, float)
        for i in range(len(g) - 1):
            seg = g[i + 1] - g[i]
            L = np.linalg.norm(seg)
            for t in np.arange(0, 1, 0.003 / max(L, 1e-6)):
                gp.append(g[i] + seg * t)
                gd.append(seg / max(L, 1e-9))
    gp, gd = np.array(gp), np.array(gd)
    gtree = cKDTree(gp)
    sd_g, _n, _d = head.signed(gp)
    ctree = cKDTree(gp[sd_g < 0.012])
    P, N = _sample_surface(head, rng, 60000)
    # scalp_mask (2026-10-13, user: no fake hair on the neck / face): head-texture-space mask (white = painted scalp);
    # roots and every ribbon point must stay over it
    on_scalp = None
    if cfg.get('scalp_mask'):
        from PIL import Image
        mk = np.asarray(Image.open(cfg['scalp_mask']).convert('L')) > 127
        huv = G.load_mesh(cfg['head_mesh'])['chunks'][0]['uv']
        vf = mk[np.clip((huv[:, 1] * mk.shape[0]).astype(int), 0, mk.shape[0] - 1),
                np.clip((huv[:, 0] * mk.shape[1]).astype(int), 0, mk.shape[1] - 1)]
        vtree = cKDTree(head.v)
        on_scalp = lambda Q: vf[vtree.query(np.atleast_2d(Q))[1]]
    ok = np.zeros(len(P), bool)
    for r in regs:
        ok |= REGIONS[r](P, head)
    # 2026-10-13 (user): nothing on the sides of the face or under / in front of the ears: off the centre line only above the
    # ear top and behind the ears
    side = np.abs(P[:, 0]) > cfg.get('side_x_mm', 45) / 1000.0
    ok &= ~side | ((P[:, 2] > head.top - cfg.get('ear_top_mm', 85) / 1000.0) & (P[:, 1] < head.c[1] - 0.015))
    if on_scalp is not None:
        ok &= on_scalp(P)
    P, N = P[ok], N[ok]
    cnt = np.array([len(x) for x in ctree.query_ball_point(P, cov_r)])
    acc = np.clip(1.0 - cnt / float(need), 0, 1)
    keep = rng.random(len(P)) < acc ** 0.7
    P, N = P[keep], N[keep]
    # targets (2026-10-13): scalp points measured as visible in the Studio (green scalp overlay); placed first, no
    # coverage test, same side-of-face exclusion
    nt = 0
    if cfg.get('targets'):
        T = np.asarray(cfg['targets'], float).reshape(-1, 3)
        sdv, tn, _ = head.signed(T)
        T = T - tn * sdv[:, None]
        tside = np.abs(T[:, 0]) > cfg.get('side_x_mm', 45) / 1000.0
        tok = ~tside | ((T[:, 2] > head.top - cfg.get('ear_top_mm', 85) / 1000.0) & (T[:, 1] < head.c[1] - 0.015))
        if on_scalp is not None:
            tok &= on_scalp(T)
        T, tn = T[tok], tn[tok]
        nt = len(T)
        P, N = np.concatenate([T, P]), np.concatenate([tn, N])
    sel, grid = [], {}
    for i in list(range(nt)) + list(nt + rng.permutation(len(P) - nt)):
        k = tuple((P[i] / sp).astype(int))
        near = False
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for j in grid.get((k[0] + dx, k[1] + dy, k[2] + dz), ()):
                        if np.linalg.norm(P[j] - P[i]) < sp:
                            near = True
        if not near:
            grid.setdefault(k, []).append(i)
            sel.append(i)
    sel = sel[:mx]
    log('  ribbon_under: %d under-covered scalp points, %d ribbons' % (len(P), len(sel)))
    pos, uv, tris, nrib = [], [], [], 0
    for i in sel:
        p = P[i].copy()
        dist, _gi = gtree.query(p)
        if dist > 0.09:
            continue
        if cfg.get('max_tris') and len(tris) + 2 * max(cfg.get('min_seg', 3), int(np.ceil(hi / (cfg.get('seg_mm', 12) / 1000.0)))) > cfg['max_tris']:
            break
        nrib += 1
        L = rng.uniform(lo, hi)
        nseg = max(cfg.get('min_seg', 3), int(np.ceil(L / (cfg.get('seg_mm', 12) / 1000.0))))
        w = wmm * rng.uniform(0.75, 1.25)
        strips = cfg.get('strips', STRIPS)
        s0 = strips[rng.integers(len(strips))]
        uw = min(s0[1] - s0[0], 0.045)
        u0 = rng.uniform(s0[0], s0[1] - uw)
        v0 = rng.uniform(0.02, 0.5)
        vl = L / 0.30 * 0.35
        path, d_prev, q = [], None, p
        for s in range(nseg + 1):
            sdv, nn, _ = head.signed(q[None])
            nn = nn[0]
            q = q - nn * sdv[0] + nn * lift
            _, gj = gtree.query(q)
            d = gd[gj] - nn * np.dot(gd[gj], nn)
            d /= max(np.linalg.norm(d), 1e-9)
            if d_prev is not None:
                if np.dot(d, d_prev) < 0:
                    d = -d
                d = d * 0.5 + d_prev * 0.5
                d /= max(np.linalg.norm(d), 1e-9)
            if on_scalp is not None and s > 0 and not on_scalp(q)[0]:
                break
            path.append((q.copy(), nn, d))
            d_prev = d
            q = q + d * (L / nseg)
        if len(path) < 2:
            nrib -= 1
            continue
        nseg = len(path) - 1
        base = len(pos)
        for s, (q, nn, d) in enumerate(path):
            t = s / nseg
            ww = w * (1.0 - t ** 2.0) * 0.5 + 1e-5
            b = np.cross(nn, d)
            b /= max(np.linalg.norm(b), 1e-9)
            pos += [q - b * ww, q + b * ww]
            v = v0 + vl * t
            uv += [(u0, v), (u0 + uw, v)]
        for s in range(nseg):
            a = base + 2 * s
            tris += [(a, a + 1, a + 2), (a + 1, a + 3, a + 2)]
    return dict(pos=np.array(pos), uv=np.array(uv), tris=np.array(tris, int).reshape(-1, 3), nribbons=nrib)


def write_mesh(mesh_rel, R, log=print):
    """vanilla hair mesh with LOD0 chunk 0 rewritten to the ribbons (same vertex format / bones); other chunks empty."""
    from scipy.spatial import cKDTree
    d = bytearray(G.get(mesh_rel))
    buf = bytearray(G.get_buffer(mesh_rel + '.1.buffer'))
    m = G.load_mesh(mesh_rel)
    t = m['t']
    cd = t['cookedData']
    sc = np.array([cd['quantizationScale'][k] for k in 'XYZ'])
    off = np.array([cd['quantizationOffset'][k] for k in 'XYZ'])
    ibo = cd['indexBufferOffset']
    stats = []
    for ci, ch in enumerate(m['chunks']):
        tc = t['chunks'][ci]
        fi = tc.get('firstIndex', 0)
        tri = np.zeros((len(ch['idx']) // 3, 3), int)
        if ci == 0:
            n = min(len(R['pos']), ch['nv'])
            tr = R['tris']
            tr = tr[(tr < n).all(1)][:len(tri)]
            tri[:len(tr)] = tr
            offs = struct.unpack_from('<4I', ch['rec'], 1)
            A = np.frombuffer(buf, dtype=np.uint8, count=ch['nv'] * 16, offset=offs[0]).reshape(ch['nv'], 16).copy()
            _, nb = cKDTree(ch['pos']).query(R['pos'][:n])
            q = np.clip(np.round((R['pos'][:n] - off) / sc * 65535.0), 0, 65535).astype('<u2')
            A[:n, :6] = q.view(np.uint8).reshape(n, 6)
            A[:n, 8:16] = A[nb, 8:16]
            buf[offs[0]:offs[0] + ch['nv'] * 16] = A.tobytes()
            U = np.frombuffer(buf, dtype='<f2', count=ch['nv'] * 2, offset=offs[1]).reshape(ch['nv'], 2).copy()
            U[:n] = R['uv'][:n].astype('<f2')
            buf[offs[1]:offs[1] + ch['nv'] * 4] = U.astype('<f2').tobytes()
            stats.append((n, len(tr), len(tri)))
        else:
            stats.append((0, 0, len(tri)))
        buf[ibo + fi * 2: ibo + (fi + len(ch['idx'])) * 2] = tri.astype('<u2').tobytes()
    t5off, t5cnt, _ = struct.unpack_from('<III', d, 0x28 + 5 * 12)
    assert t5cnt == 1 and struct.unpack_from('<I', d, t5off + 16)[0] == len(buf)
    struct.pack_into('<I', d, t5off + 20, zlib.crc32(bytes(buf)))
    c = w3lib.CR2W(bytes(d))
    c.fix_crcs()
    out = bytes(c.d)
    bad, ok = w3lib.CR2W(out).verify_crcs()
    assert bad == 0 and ok
    return out, bytes(buf), stats
