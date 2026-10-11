r"""flare - opt-in layered, flared strand ends (growth key "flare_tips"; HairLab L132, Emhyr's nape).

  "flare_tips": {"z_below": 0.10,      # only strands whose tip is lower than head centre z - z_below (m)
                 "frac": 0.30,         # the outer 30 % of the arc length flicks
                 "out": 0.05,          # radial (away from the head axis) offset at the tip (m), x 0.5..1.5 random per strand
                 "side": 0.02,         # random tangential offset at the tip (m)
                 "layers": 3,          # strands are dealt into this many length layers ...
                 "cut": 0.18,          # ... the shortest layer loses this fraction of its length (layer i: cut * i / (layers-1))
                 "seed": 7,
                 "y_max": 9.0,         # skip strands whose tip is more than y_max in front of the head centre (use -0.02: nape only)
                 "x_max": 9.0}         # skip strands whose tip is more than x_max to the side

Every strand keeps its point count (resampled along its own arc after the cut). Nothing runs unless the key is set."""
import numpy as np


def _resample(p, s_new):
    p = np.asarray(p, float)
    seg = np.r_[0.0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
    return np.stack([np.interp(s_new, seg, p[:, k]) for k in range(3)], 1), seg[-1]


def flare_tips(strands, head, o, log=print):
    rng = np.random.default_rng(int(o.get('seed', 7)))
    c = np.asarray(head.c, float)
    zb = float(o.get('z_below', 0.10))
    frac, out, side = float(o.get('frac', 0.3)), float(o.get('out', 0.05)), float(o.get('side', 0.02))
    nl, cut = max(int(o.get('layers', 3)), 1), float(o.get('cut', 0.18))
    new, n = [], 0
    for p in strands:
        p = np.asarray(p, float)
        if p[-1][2] > c[2] - zb or p[-1][1] > c[1] + float(o.get('y_max', 9.0)) or abs(p[-1][0] - c[0]) > float(o.get('x_max', 9.0)):
            new.append(p)
            continue
        n += 1
        K = len(p)
        lay = int(rng.integers(0, nl))
        keep = 1.0 - (cut * lay / (nl - 1) if nl > 1 else 0.0)
        seg = np.r_[0.0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
        q, _L = _resample(p, np.linspace(0.0, seg[-1] * keep, K))
        t = np.linspace(0.0, 1.0, K)
        w = np.clip((t - (1.0 - frac)) / max(frac, 1e-6), 0.0, 1.0) ** 2
        r = q[-1][:2] - c[:2]
        nr = np.linalg.norm(r)
        r = r / nr if nr > 1e-6 else np.array([0.0, -1.0])
        tang = np.array([-r[1], r[0]])
        d = r * out * rng.uniform(0.5, 1.5) + tang * side * rng.uniform(-1.0, 1.0)
        q[:, 0] += w * d[0]
        q[:, 1] += w * d[1]
        q[:, 2] += w * out * 0.15 * rng.uniform(0.0, 1.0)         # a slight lift
        new.append(q)
    log('  flare_tips: %d of %d strands layered (%d layers, cut %.0f %%) and flared %.0f mm' % (n, len(strands), nl, cut * 100, out * 1000))
    return new
