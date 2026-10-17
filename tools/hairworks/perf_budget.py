r"""perf_budget - in-game performance CHECK (information only since 2026-10-15) for HairWorks geometry (Studio export / studio_to_config). Offline.

  py tools\hairworks\perf_budget.py check <geo.json[.gz] | _HW.json> [--density D] [--lod-density 1.4]
  py tools\hairworks\perf_budget.py ops   <geo.json[.gz] | _HW.json> -o fix_ops.json   # Studio ops that fix the errors

Why (L255, user review 5, 2026-10-13): Crach dropped the game to ~0.1 FPS and Dettlaff to ~2 FPS, while builds with
far more hair ran fine (Oriana: 5819 faces x density 2.6, about 0.97 M hairs). In the Studio both bench like any other
build (143 / 123 fps). The two slow builds were the ONLY L255 builds whose guides had zero-length / near-zero
segments (Crach 14 zero, 706 under 0.5 mm, 20 guides shorter than 5 mm; Dettlaff 21 zero, 85 under 0.5 mm, plus 6
roots shared by 2-3 guides = 16 zero-area growth faces). Every other L255 build had 0 zero-length segments. The game's
HairWorks solver normalises each segment (rest length ~0 -> division by ~0) and interpolates hair across growth faces
(zero area); this geometry is invalid input, so it is a hard error here, whatever the hair count.

ERRORS (geometry defects; since 2026-10-15 the export / studio_to_config only WARN on them, never refuse):
  - a guide segment shorter than ZERO_SEG_MM (0.1 mm; L255: Crach 14, Dettlaff 21, every other build 0)
  - a guide shorter than MIN_GUIDE_MM (5 mm) from root to tip
  - two guides rooted within DUP_ROOT_MM (0.2 mm) of each other
  - a growth face with area under MIN_FACE_MM2 (0.1 mm2)
WARNINGS (reported, never block): hair metres (faces x 64 x density x close-up LOD density x mean guide length) above
  HAIR_M_MAX (budget, information only since 2026-10-15), segments under MIN_SEG_MM (0.5 mm; 1-5 guides in several fast builds, Crach 86),
  hair metres above HAIR_M_WARN, hairs above HAIRS_WARN.

`ops` writes Studio edit ops (`reshape` = the same strand resampled to even arc length, same point count, so the shape
is kept and no segment is near zero; `delete` = guides under MIN_GUIDE_MM and the extra copies of a shared root). Apply
them with `studio_ops` / the agent `ops` step, then save the project under a new name.
"""
import argparse
import gzip
import json
import sys

import numpy as np

ZERO_SEG_MM = 0.1      # error: a (near) zero-length segment
MIN_SEG_MM = 0.5       # warning (and fixed by `ops`): very short segment
MIN_GUIDE_MM = 5.0
DUP_ROOT_MM = 0.2
MIN_FACE_MM2 = 0.1
HAIR_M_WARN = 120000.0
HAIR_M_MAX = 250000.0
HAIRS_WARN = 600000.0
# Path-tracing GPU cost (Witcher 3 5.0, RTX 5090, PresentMon GPU ms/frame, one NPC; Vesemir 300 guides / 552 faces /
# clump 0.8 = 14 ms, vanilla ~12 ms). Path-traced HairWorks renders a fixed RTNHairsPerPatch=32 hairs per growth face,
# so `density` barely matters and the FACE COUNT does. Lambert clump 0.3 -> 0.8 saved ~4 ms (1.0: ~4.5 ms), Triss clump
# 0.3 -> 0.7 ~1 ms (441 faces: 16.4 ms). Lambert shipped (547 guides, 1007 faces, clump 0.3) ~27 ms; Keira (1271 faces,
# clump 0.3) ~+10 % GPU, accepted. Guillaume (816 guides, 1516 faces, 25 pts/guide, tortuosity 1.76): 32 ms with curl
# radius 0.02 / pitch 0.045, 15.5 ms curl off, 18 ms with pitch 0.09 / radius 0.015 + clump 1.0.
PT_FACES_WARN = 600         # target ~500-550 faces (Vesemir 552 = 14 ms)
PT_FACES_STRONG = 900
PT_CLUMP_LOW = 0.5          # clump scale below this ...
PT_CLUMP_FACES = 450        # ... with more faces than this is slow
PT_TORT_WARN = 1.5          # mean path length / root-to-tip distance
PT_TORT_PTS = 20            # points per guide ...
PT_TORT_PTS_TORT = 1.4      # ... with at least this tortuosity
# rough relative cost score = kfaces x clump factor x curl factor x wave factor (heuristic, calibrated on the builds above:
# Vesemir 0.55 low, Keira 2.7 high, Lambert 4.5 high, Guillaume curled 4.1 high)
PT_COST_MEDIUM = 1.2
PT_COST_HIGH = 2.5


def load(path):
    op = gzip.open if path.endswith('.gz') else open
    with op(path, 'rt', encoding='utf-8') as fh:
        return json.load(fh)


def base_density(cfg):
    """density of a built config (HairLab hw pack build/hairbatch/hw/<cfg>.json), None when not built."""
    import os
    hp = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'build', 'hairbatch', 'hw', cfg + '.json')
    try:
        return float(json.load(open(hp))['physical']['volume']['density'])
    except Exception:
        return None


def _density(d, density):
    if density is not None:
        return float(density)
    try:
        return float(d['physical']['volume']['density'])
    except (KeyError, TypeError):
        return 1.0


def path_trace_warnings(d, G, nfaces, m):
    """Path-tracing GPU cost warnings (never errors); fills m with tortuosity / clump_scale / pt_cost / pt_cost_level / pt_note."""
    tort = []
    for g in G:
        if len(g) < 2:
            continue
        path = float(np.linalg.norm(np.diff(g, axis=0), axis=1).sum())
        tort.append(path / max(float(np.linalg.norm(g[-1] - g[0])), 1e-9))
    mt = float(np.mean(tort)) if tort else 1.0
    pts = float(np.mean([len(g) for g in G])) if G else 0.0
    ph = d.get('physical') or {}
    clump = float((ph.get('clumping') or {}).get('clumpScale', 1.0))
    wv = ph.get('waveness') or {}
    w_scale, w_strand = float(wv.get('waveScale', 1.0)), float(wv.get('waveStrand', 0.0))
    clump_f = 1.0 + max(0.0, 0.8 - clump) / 0.5
    curl_f = 1.0 + max(0.0, mt - 1.3) * 3.0
    wave_f = 1.0 + 2.0 * w_strand * max(0.0, 0.4 - w_scale) / 0.4
    score = nfaces / 1000.0 * clump_f * curl_f * wave_f
    level = 'high' if score >= PT_COST_HIGH else 'medium' if score >= PT_COST_MEDIUM else 'low'
    m.update({'tortuosity': round(mt, 3), 'clump_scale': clump, 'pt_cost': round(score, 2), 'pt_cost_level': level,
              'pt_note': 'est. extra GPU cost: %s (score %.1f)' % (level, score)})
    w = []
    if nfaces > PT_FACES_STRONG:
        w.append('very many growth faces (%d > %d): path-traced hair cost scales with faces, not density; expect a large GPU cost' % (nfaces, PT_FACES_STRONG))
    elif nfaces > PT_FACES_WARN:
        w.append('many growth faces (%d > %d): path-traced hair cost scales with faces, not density; aim for ~550' % (nfaces, PT_FACES_WARN))
    if clump < PT_CLUMP_LOW and nfaces > PT_CLUMP_FACES:
        w.append('low clumping (%.2f) with many faces (%d) is slow under path tracing; try clump 0.8' % (clump, nfaces))
    if mt > PT_TORT_WARN or (pts > PT_TORT_PTS and mt > PT_TORT_PTS_TORT):
        w.append('tight curl is expensive under path tracing (tortuosity %.2f, %.0f points/guide); loosen pitch / radius' % (mt, pts))
    return w


def check(d, density=None, lod_density=1.4):
    """d = a Studio _geo.json or a HairLab _HW.json (guides + F). Returns {metrics, errors, warnings, bad}."""
    G = [np.asarray(g, float) for g in d['guides']]
    F = np.asarray(d['F'], int).reshape(-1, 3)
    errors, warnings = [], []
    bad_seg, zero, short, segs, lens = [], [], [], 0, []
    for i, g in enumerate(G):
        s = np.linalg.norm(np.diff(g, axis=0), axis=1)
        segs += len(s)
        L = float(s.sum())
        lens.append(L)
        if L * 1000 < MIN_GUIDE_MM:
            short.append(i)
        elif s.size and s.min() * 1000 < MIN_SEG_MM:
            bad_seg.append(i)
            if s.min() * 1000 < ZERO_SEG_MM:
                zero.append(i)
    roots = np.array([g[0] for g in G])
    key = np.round(roots / (DUP_ROOT_MM / 1000.0)).astype(np.int64)
    seen, dups = {}, []
    for i, k in enumerate(map(tuple, key)):
        if k in seen:
            dups.append(i)
        else:
            seen[k] = i
    a, b, c = roots[F[:, 0]], roots[F[:, 1]], roots[F[:, 2]]
    area = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1) * 1e6
    tiny = np.nonzero(area < MIN_FACE_MM2)[0].tolist()
    dens = _density(d, density)
    hairs = len(F) * 64 * dens * lod_density
    hair_m = hairs * (float(np.mean(lens)) if lens else 0.0)
    m = {'guides': len(G), 'faces': int(len(F)), 'density': dens, 'lod_density': lod_density, 'hairs_close': round(hairs),
         'hair_m_close': round(hair_m), 'mean_len_m': round(float(np.mean(lens)), 4) if lens else 0,
         'guides_zero_segments': len(zero), 'guides_short_segments': len(bad_seg), 'guides_short': len(short), 'dup_roots': len(dups), 'tiny_faces': len(tiny)}
    if zero:
        errors.append('%d guides have a zero-length segment (under %.1f mm: invalid solver input)' % (len(zero), ZERO_SEG_MM))
    if bad_seg:
        warnings.append('%d guides have a segment under %.1f mm' % (len(bad_seg), MIN_SEG_MM))
    if short:
        errors.append('%d guides are shorter than %.0f mm' % (len(short), MIN_GUIDE_MM))
    if dups:
        errors.append('%d guides share a root with another guide (within %.1f mm)' % (len(dups), DUP_ROOT_MM))
    if tiny:
        errors.append('%d growth faces have an area under %.1f mm2' % (len(tiny), MIN_FACE_MM2))
    if hair_m > HAIR_M_MAX:
        warnings.append('hair metres %.0f > %.0f (faces x 64 x density x %.1f x mean length)' % (hair_m, HAIR_M_MAX, lod_density))
    elif hair_m > HAIR_M_WARN:
        warnings.append('hair metres %.0f > %.0f (heavy)' % (hair_m, HAIR_M_WARN))
    if hairs > HAIRS_WARN:
        warnings.append('close-up hairs %.0f > %.0f' % (hairs, HAIRS_WARN))
    warnings += path_trace_warnings(d, G, len(F), m)
    return {'metrics': m, 'errors': errors, 'warnings': warnings,
            'bad': {'segments': bad_seg, 'short': short, 'dups': dups, 'tiny_faces': tiny}}


def resample(g, n=None):
    """Same strand, n points at even arc length (shape kept, no near-zero segment)."""
    g = np.asarray(g, float)
    n = n or len(g)
    s = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(g, axis=0), axis=1))])
    t = np.linspace(0.0, s[-1], n)
    return np.stack([np.interp(t, s, g[:, k]) for k in range(3)], 1)


def fix_ops(d, r=None):
    r = r or check(d)
    G = d['guides']
    rr = lambda p: [round(float(x), 5) for x in p]
    ops = []
    dead = sorted(set(r['bad']['short']) | set(r['bad']['dups']))
    if dead:
        ops.append({'op': 'delete', 'roots': [rr(G[i][0]) for i in dead]})
    for i in r['bad']['segments']:
        if i in dead:
            continue
        ops.append({'op': 'reshape', 'root': rr(G[i][0]), 'points': [rr(p) for p in resample(G[i])]})
    return ops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['check', 'ops'])
    ap.add_argument('geo')
    ap.add_argument('--density', type=float)
    ap.add_argument('--lod-density', type=float, default=1.4)
    ap.add_argument('-o', '--out')
    a = ap.parse_args()
    d = load(a.geo)
    r = check(d, a.density, a.lod_density)
    if a.cmd == 'ops':
        ops = fix_ops(d, r)
        json.dump({'ops': ops}, open(a.out, 'w'), indent=0)
        print('%d ops -> %s' % (len(ops), a.out))
    out = dict(r)
    out['bad'] = {k: len(v) for k, v in r['bad'].items()}
    print(json.dumps(out, indent=1))
    sys.exit(1 if (a.cmd == 'check' and r['errors']) else 0)


if __name__ == '__main__':
    main()
