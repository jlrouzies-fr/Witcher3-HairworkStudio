r"""calib - compare Desktop Groom Studio shot renders with the in-game HairLab captures of the same build.

  py desktop_studio\tools\calib.py <cfg> <agent out dir> <tag> [--sheet out.jpg]
  py desktop_studio\tools\calib.py <cfg> <agent out dir> <tag> --game-label <label> [--round L237] --shape-sheet s.jpg

Shape mode (2026-10-11, Studio sim vs game): --game-label takes the in-game crops of another label of the SAME fur
(e.g. guillaume_l237_l124ref = the approved guillaume_l124_locks_hug re-shot unchanged) instead of desk.json's own.
For every shot it compares the in-game hair silhouette (magenta twin) with the Studio mask pass: IoU, game-only /
Studio-only area, and the lower edge of the hair (per column, the lowest hair pixel; mean / p10 / p90 of Studio - game
in mm, + = the Studio hangs lower). Writes <tag>_shot<n>_GS.png at native size (in-game | Studio | silhouettes: white
= both, cyan = game only, red = Studio only) and the --shape-sheet (one row per shot). --before <dir>:<tag> adds
<tag>_shot<n>_BA.png and makes the sheet in-game | Studio before | Studio now (e.g. bone_stiffness 0 vs 1).

For every shot that has an in-game head crop of THIS build (desk.json shots[n].game) and its magenta twin (game_mask),
hair pixels are compared where BOTH the Studio hair mask (red pass <tag>_shot<n>_mask.png, eroded) and the in-game
magenta-twin mask (eroded) say hair, on both images (falls back to the Studio mask alone when that overlap is small).
Prints mean hair colour (sRGB), luminance p10 / p50 / p90 (p90 = highlights) and the ratio Studio / game, and writes
a sheet: vanilla | in-game build | Studio, one row per shot. Read-only on the repo; outputs go where you ask.

Calibration set (builds whose rounds still have raw in-game crops; the rig defaults are fitted on all of them, README
"Rendering", earlier calibration): vesemir_l232_mirror_ear_back, guillaume_l148_ringlets, emhyr_l149_plain, iris_l154_a.
"""
import argparse
import json
import os

import numpy as np
from PIL import Image

CALIB_SET = ['vesemir_l232_mirror_ear_back', 'guillaume_l148_ringlets', 'emhyr_l149_plain', 'iris_l154_a']
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def stats(img, mask):
    a = np.asarray(img.convert('RGB'), np.float64)
    px = a[mask]
    if len(px) < 50:
        return None
    lum = px @ [0.2126, 0.7152, 0.0722]
    return dict(n=int(len(px)), mean=px.mean(0).round(1).tolist(), p10=round(float(np.percentile(lum, 10)), 1),
                p50=round(float(np.percentile(lum, 50)), 1), p90=round(float(np.percentile(lum, 90)), 1))


def _mask_magenta(path, size):
    # magenta hue at any brightness (shaded magenta strands go down to ~(60, 0, 60)): R and B well above G, R ~ B
    t = np.asarray(Image.open(path).convert('RGB').resize(size), int)
    r, g, b = t[..., 0], t[..., 1], t[..., 2]
    lo = np.minimum(r, b)
    return (lo > 45) & (g < 0.55 * lo) & (np.abs(r - b) < 0.45 * np.maximum(r, b))


def _mask_red(path):
    m = np.asarray(Image.open(path).convert('RGB'), int)
    return (m[..., 0] > 120) & (m[..., 1] < 60) & (m[..., 2] < 60)


def _clean(m, r=3):
    """drop single stray strands / speckles: open then close with a small square (PIL min / max filters)"""
    from PIL import ImageFilter
    im = Image.fromarray((m * 255).astype(np.uint8))
    k = 2 * r + 1
    im = im.filter(ImageFilter.MinFilter(k)).filter(ImageFilter.MaxFilter(k))
    im = im.filter(ImageFilter.MaxFilter(k)).filter(ImageFilter.MinFilter(k))
    return np.asarray(im) > 0


def _lower_edge(m):
    """per column: row of the lowest hair pixel (nan where the column has no hair)"""
    h = m.shape[0]
    any_ = m.any(0)
    low = h - 1 - np.argmax(m[::-1], axis=0)
    return np.where(any_, low, np.nan)


def shape_compare(D, a, glabel):
    """silhouette of the Studio mask pass vs the in-game magenta twin of the same fur, per shot"""
    import glob
    res, rows = {}, []
    for n, sh in sorted(D.get('shots', {}).items(), key=lambda kv: int(kv[0])):
        ours = os.path.join(a.out, '%s_shot%s.png' % (a.tag, n))
        mk = os.path.join(a.out, '%s_shot%s_mask.png' % (a.tag, n))
        pat = os.path.join(ROOT, 'results', a.round or '*', 'raw', '*_%s_shot%s_head.png' % (glabel, n))
        gl = sorted(glob.glob(pat), key=os.path.getmtime)
        if not (os.path.exists(ours) and os.path.exists(mk) and gl):
            continue
        g = gl[-1]
        gm_p = g.replace('_%s_shot' % glabel, '_%s__m_shot' % glabel)
        S = Image.open(ours).convert('RGB')
        G = Image.open(g).convert('RGB').resize(S.size)
        sm = _clean(_mask_red(mk))
        gm = _clean(_mask_magenta(gm_p, S.size)) if os.path.exists(gm_p) else None
        if gm is None or gm.sum() < 500:
            continue
        H = S.size[1]
        eye, tgt = np.array(sh['eye']), np.array(sh['target'])
        mm_px = 1000.0 * 2 * np.linalg.norm(eye - tgt) * np.tan(np.radians(sh['fovy'] / 2)) / H
        inter, union = (sm & gm).sum(), (sm | gm).sum()
        le_s, le_g = _lower_edge(sm), _lower_edge(gm)
        ok = ~np.isnan(le_s) & ~np.isnan(le_g)
        d = (le_s[ok] - le_g[ok]) * mm_px
        r = dict(view=sh.get('view'), iou=round(float(inter / max(union, 1)), 3),
                 game_only=round(float((gm & ~sm).sum() / max(gm.sum(), 1)), 3),
                 studio_only=round(float((sm & ~gm).sum() / max(gm.sum(), 1)), 3),
                 area_ratio=round(float(sm.sum() / max(gm.sum(), 1)), 3),
                 low_mean_mm=round(float(d.mean()), 1) if len(d) else None,
                 low_p90_mm=round(float(np.percentile(d, 90)), 1) if len(d) else None,
                 low_p10_mm=round(float(np.percentile(d, 10)), 1) if len(d) else None,
                 lowest_mm=round(float((np.nanmax(le_s) - np.nanmax(le_g)) * mm_px), 1),
                 mm_per_px=round(float(mm_px), 3), game=os.path.relpath(g, ROOT))
        res[n] = r
        print('shot %-3s %-13s IoU %.3f  game-only %.3f  studio-only %.3f  area x%.2f  lower edge Studio-game mean %s p10 %s p90 %s lowest %s mm' % (
            n, r['view'], r['iou'], r['game_only'], r['studio_only'], r['area_ratio'], r['low_mean_mm'], r['low_p10_mm'],
            r['low_p90_mm'], r['lowest_mm']))
        ov = np.zeros(sm.shape + (3,), np.uint8)
        ov[gm & sm] = (235, 235, 235)
        ov[gm & ~sm] = (0, 200, 230)
        ov[sm & ~gm] = (230, 40, 40)
        O = Image.fromarray(ov)
        W = S.size[0]
        tri = Image.new('RGB', (W * 3, H))
        for j, im in enumerate((G, S, O)):
            tri.paste(im, (j * W, 0))
        tri.save(os.path.join(a.out, '%s_shot%s_GS.png' % (a.tag, n)))
        if a.before:
            # before / after: in-game | Studio before (other run) | Studio now, native size
            bd, bt = a.before.rsplit(':', 1)
            bp = os.path.join(bd, '%s_shot%s.png' % (bt, n))
            if os.path.exists(bp):
                tri = Image.new('RGB', (W * 3, H))
                for j, im in enumerate((G, Image.open(bp).convert('RGB').resize(S.size), S)):
                    tri.paste(im, (j * W, 0))
                tri.save(os.path.join(a.out, '%s_shot%s_BA.png' % (a.tag, n)))
        rows.append(tri)
    if a.shape_sheet and rows:
        w = 600
        sheet = Image.new('RGB', (w * 3, w * len(rows)), (18, 20, 22))
        for i, tri in enumerate(rows):
            sheet.paste(tri.resize((w * 3, w)), (0, i * w))
        sheet.save(a.shape_sheet, quality=92)
        print('shape sheet: %s (columns: %s)' % (a.shape_sheet, 'in-game | Studio before | Studio now' if a.before else
                                                  'in-game | Studio | silhouettes white both, cyan game only, red Studio only'))
    if res:
        v = [r['low_mean_mm'] for r in res.values() if r['low_mean_mm'] is not None]
        print('mean IoU %.3f, mean lower-edge offset %+.1f mm (+ = Studio lower)' % (
            np.mean([r['iou'] for r in res.values()]), np.mean(v) if v else float('nan')))
    json.dump(res, open(os.path.join(a.out, '%s_shape.json' % a.tag), 'w'), indent=1)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cfg')
    ap.add_argument('out')
    ap.add_argument('tag')
    ap.add_argument('--sheet')
    ap.add_argument('--game-label', help='shape mode: in-game crops of this label (same fur), e.g. guillaume_l237_l124ref')
    ap.add_argument('--round', help='shape mode: only the crops of that round (default: newest)')
    ap.add_argument('--before', help='shape mode: <dir>:<tag> of an earlier Studio run; the sheet becomes in-game | before | now')
    ap.add_argument('--shape-sheet', help='shape mode: sheet path (in-game | Studio | silhouettes)')
    a = ap.parse_args()
    D = json.load(open(os.path.join(ROOT, 'build', 'desktop_studio', a.cfg, 'desk.json')))
    if a.game_label:
        shape_compare(D, a, a.game_label)
        return
    rows, res = [], {}
    for n, sh in sorted(D.get('shots', {}).items(), key=lambda kv: int(kv[0])):
        ours = os.path.join(a.out, '%s_shot%s.png' % (a.tag, n))
        mk = os.path.join(a.out, '%s_shot%s_mask.png' % (a.tag, n))
        if not os.path.exists(ours):
            continue
        S = Image.open(ours).convert('RGB')
        r = {}
        if os.path.exists(mk):
            m = np.asarray(Image.open(mk).convert('RGB'), int)
            sm = (m[..., 0] > 120) & (m[..., 1] < 60) & (m[..., 2] < 60)
            r['studio'] = stats(S, sm)
        G = None
        if sh.get('game'):
            # the magenta twin is a second NPC (pose / position differ a little), so its mask does not sit on the
            # build's own frame; the Studio camera matches the capture, so the Studio hair mask, eroded, is used on both
            G = Image.open(os.path.join(ROOT, sh['game'])).convert('RGB').resize(S.size)
            if 'studio' in r:
                from PIL import ImageFilter
                er = np.asarray(Image.fromarray((sm * 255).astype(np.uint8)).filter(ImageFilter.MinFilter(15))) > 0
                if sh.get('game_mask'):
                    # ... intersected with the (dilated) magenta-twin mask: pixels that are hair in BOTH frames
                    t = np.asarray(Image.open(os.path.join(ROOT, sh['game_mask'])).convert('RGB').resize(S.size), int)
                    gm = ((t[..., 0] > 150) & (t[..., 1] < 90) & (t[..., 2] > 150)).astype(np.uint8) * 255
                    both = er & (np.asarray(Image.fromarray(gm).filter(ImageFilter.MinFilter(9))) > 0)
                    if both.sum() > 2000:
                        er = both
                r['studio'] = stats(S, er)
                r['game'] = stats(G, er)
        if r.get('studio') and r.get('game'):
            r['ratio_p50'] = round(r['studio']['p50'] / max(r['game']['p50'], 1), 3)
            r['ratio_p90'] = round(r['studio']['p90'] / max(r['game']['p90'], 1), 3)
        res[n] = r
        print('shot %-3s %-13s game %s | studio %s | p50 x%s p90 x%s' % (
            n, sh.get('view'), r.get('game') and (r['game']['mean'], r['game']['p50'], r['game']['p90']),
            r.get('studio') and (r['studio']['mean'], r['studio']['p50'], r['studio']['p90']), r.get('ratio_p50'), r.get('ratio_p90')))
        V = Image.open(os.path.join(ROOT, sh['ref'])).convert('RGB').resize(S.size) if sh.get('ref') else Image.new('RGB', S.size)
        rows.append([V, G or Image.new('RGB', S.size), S])
    if a.sheet and rows:
        w = 520
        sheet = Image.new('RGB', (w * 3, w * len(rows)), (18, 20, 22))
        for i, row in enumerate(rows):
            for j, im in enumerate(row):
                sheet.paste(im.resize((w, w)), (j * w, i * w))
        sheet.save(a.sheet, quality=90)
        print('sheet: %s (columns: vanilla | in-game build | Studio)' % a.sheet)
    json.dump(res, open(os.path.join(a.out, '%s_calib.json' % a.tag), 'w'), indent=1)


if __name__ == '__main__':
    main()
