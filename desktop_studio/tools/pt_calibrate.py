"""Fit ms_est = a + b*metric for the path tracing cost estimator (pt_estimate.cpp).
Runs HairworkStudio.exe --pt-estimate over the measured builds for several estimator variants, prints r^2 and
per-build predicted vs measured, and writes desktop_studio/data/pt_calibration.json (best variant).
  py desktop_studio\\tools\\pt_calibrate.py [--write]
"""
import json, subprocess, sys, itertools
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "desktop_studio" / "build" / "Release" / "HairworkStudio.exe"
HW = ROOT / "build" / "hairbatch" / "hw"
MEAS = {
    "vesemir_l273_ref": 14.1, "lambert_l260_aio": 27, "lambert_l273_lean20": 21.1, "lambert_l274_clump": 17.2,
    "lambert_l275_c10": 16.7, "lambert_l277_c08": 23.8, "lambert_l277_c08noinfl": 20.1, "lambert_l278_rs14": 20.1,
    "lambert_l278_c10d2rs14": 17.4, "guillaume_l260_aio": 32, "guillaume_l270_nocurl": 15.5, "guillaume_l270_curl10": 19.3,
    "guillaume_l273_p09r15": 18.9, "guillaume_l275_p09r15c10": 18.1, "triss_l82_fe_user1b": 16.4, "triss_l275_c07": 17.6,
    "keira_l260_aio": 15.5,
}


def run(cfg, opts):
    out = subprocess.run([str(EXE), "--pt-estimate", str(HW / (cfg + ".json")), "--pt-opts", opts], capture_output=True, text=True).stdout
    return json.loads(out.strip().splitlines()[-1])


def fit(xs, ys):
    n = len(xs); mx = sum(xs) / n; my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs); sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    b = sxy / sxx if sxx else 0.0; a = my - b * mx
    ss = sum((y - my) ** 2 for y in ys); res = sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys))
    return a, b, 1 - res / ss if ss else 0.0, (res / n) ** 0.5


def main():
    cfgs = list(MEAS)
    ys = [MEAS[c] for c in cfgs]
    results = []
    for model, stop, foot in itertools.product((1,), (1000,), (0.006,)):
        opts = "model=%d,stop=%d,foot=%g" % (model, stop, foot)
        rows = {c: run(c, opts) for c in cfgs}
        for key in ("metric", "mean_visits", "mean_tests", "hits_per_ray", "coverage", "aabb_area", "faces", "segments"):
            xs = [rows[c][key] for c in cfgs]
            a, b, r2, rmse = fit(xs, ys)
            results.append((r2, rmse, opts, key, a, b, rows))
            print("%-28s %-12s r2=%.3f rmse=%.2f" % (opts, key, r2, rmse), flush=True)
    results.sort(key=lambda t: -t[0])
    r2, rmse, opts, key, a, b, rows = results[0]
    print("\nBEST", opts, key, "a=%.3f b=%.5g r2=%.3f rmse=%.2f ms" % (a, b, r2, rmse))
    print("%-28s %8s %8s %8s %6s %6s" % ("cfg", "measured", "pred", "metric", "clump", "faces"))
    for c in cfgs:
        print("%-28s %8.1f %8.1f %8.4g %6.2f %6d" % (c, MEAS[c], a + b * rows[c][key], rows[c][key], rows[c]["clump"], rows[c]["faces"]))
    if "--write" in sys.argv:
        doc = {
            "note": "path tracing cost calibration: ms = fit.a + fit.b * metric, metric from pt_estimate.cpp (RTX 5090, W3 5.0 PT, PresentMon median GPU ms, +-3 ms noise)",
            "vesemir_ms": 14.1,
            "variant": {"options": opts, "metric_key": key},
            "fit": {"a": a, "b": b, "r2": r2, "rmse_ms": rmse},
            "entries": [{"cfg": c, "measured_gpu_ms": MEAS[c], "metric": rows[c][key], "predicted_ms": a + b * rows[c][key]} for c in cfgs],
        }
        (ROOT / "desktop_studio" / "data" / "pt_calibration.json").write_text(json.dumps(doc, indent=1))
        print("written")


if __name__ == "__main__":
    main()
