// Hairwork Studio - GPU cost model of moving path-traced hair + the stiffness / hair-count auto-balance (see gpu_cost.h).
// Our own code. The formula mirrors tools/hairworks/toolarch/gpu_cost_model.py (phi / hold / drawn_hairs / bad_zone).
#include "gpu_cost.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

#include "app.h"

namespace {
const char* P_STIFF = "physicalMaterials.stiffness.stiffness";
const char* P_ROOT = "physicalMaterials.stiffness.rootStiffness";
const char* P_TIP = "physicalMaterials.stiffness.tipStiffness";
const char* P_BEND = "physicalMaterials.stiffness.bendStiffness";
const char* P_STR = "physicalMaterials.stiffness.stiffnessStrength";
const char* P_CURVE = "physicalMaterials.stiffness.stiffnessCurve";
const char* P_DLD = "levelOfDetail.detailLOD.detailLODDensity";
const char* P_DENS = "physicalMaterials.volume.density";
double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }
}  // namespace

json GpuFeat::toJson() const {
    return {{"faces", faces}, {"hpp", hpp}, {"K", K}, {"sm", sm}, {"len_m", lenM}, {"dld", dld}, {"lod_on", lodOn}, {"keep_extra", keepExtra},
            {"density", density}, {"stiffness", stiffness}, {"root", root}, {"tip", tip}, {"bend", bend}, {"strength", strength},
            {"curve", {curve[0], curve[1], curve[2], curve[3]}}};
}

GpuFeat GpuFeat::fromJson(const json& j) {
    GpuFeat f;
    f.faces = j.value("faces", 0.0); f.hpp = j.value("hpp", 32.0); f.K = j.value("K", 13.0); f.sm = j.value("sm", 3.0);
    f.lenM = j.value("len_m", 0.2); f.dld = j.value("dld", 1.0); f.lodOn = j.value("lod_on", 1.0); f.keepExtra = j.value("keep_extra", 1.0);
    f.density = j.value("density", 1.0); f.stiffness = j.value("stiffness", 0.5); f.root = j.value("root", 0.0); f.tip = j.value("tip", 0.0);
    f.bend = j.value("bend", 0.0); f.strength = j.value("strength", 1.0);
    if (j.contains("curve") && j["curve"].is_array() && j["curve"].size() >= 4)
        for (int i = 0; i < 4; i++) f.curve[i] = j["curve"][i].get<double>();
    return f;
}

double GpuModel::drawnHairs(const GpuFeat& f) const {
    double keep = f.lodOn > 0.5 ? clampd(1.0 + (f.dld - 1.0) * keepT, 0, 1) : clampd(f.density, 0, 1);
    return f.faces * f.hpp * keep * f.keepExtra;
}

double GpuModel::hold(const GpuFeat& f) const {
    double cbar = (f.curve[1] + f.curve[2] + f.curve[3]) / 3.0;
    double rf = rho + (1.0 - rho) * clampd(f.root, 0, 1);
    double h = std::max({f.stiffness * cbar * rf, kb * f.bend * rf, kt * f.tip});
    return h * std::pow(std::max(f.strength, 0.05), q);
}

double GpuModel::phi(const GpuFeat& f, double kap) const {
    double H = drawnHairs(f) / 1e4, S = (f.K - 1) * f.sm / 36.0;
    double L = useLen ? std::min(f.lenM / 0.25, Lcap) : 1.0;
    double soft = 1.0 / (1.0 + std::exp(std::min((hold(f) - s0) / w, 60.0)));   // logistic knee (gpu_cost_model.py soft)
    return a * H * S + kap * b * H * std::pow(S, g) * L * soft;
}

GpuModel gpuLoadModel(const std::string& repoRoot) {
    GpuModel m;
    std::string p = repoRoot + "\\desktop_studio\\data\\gpu_cost_model.json";
    std::ifstream in(p);
    if (!in) { m.err = "model file missing: " + p; return m; }
    try {
        json j = json::parse(in);
        const json& P = j["params"];
        m.a = P.value("a", m.a); m.b = P.value("b", m.b); m.w = P.value("w", m.w); m.s0 = P.value("s0", m.s0); m.rho = P.value("rho", m.rho); m.q = P.value("q", m.q);
        m.kb = P.value("kb", m.kb); m.kt = P.value("kt", m.kt); m.g = P.value("g", m.g); m.Lcap = P.value("Lcap", m.Lcap);
        m.keepT = P.value("keep_t", m.keepT); m.useLen = P.value("uselen", 0) != 0;
        json kap = j.value("kappa", json::object());   // a named copy: items() of a temporary would dangle
        for (auto& [k, v] : kap.items()) m.kappa[k] = v.get<double>();
        json vf = j.value("vesemir", json::object());
        m.ves = GpuFeat::fromJson(vf);
        if (vf.contains("curve") && vf["curve"].is_array())
            for (int i = 0; i < 4 && i < (int)vf["curve"].size(); i++) m.ves.curve[i] = vf["curve"][i].get<double>();
        const json& E = j["error"];
        m.errAbsAnch = E["anchored"].value("abs_ms", m.errAbsAnch); m.errRelAnch = E["anchored"].value("rel", m.errRelAnch);
        m.errAbsGlob = E["global"].value("abs_ms", m.errAbsGlob); m.errRelGlob = E["global"].value("rel", m.errRelGlob);
        m.fitRows = j.value("fit_rows", 0);
        for (auto& s : j.value("no_data", json::array())) m.noData.push_back(s.get<std::string>());
        m.loaded = true;
    } catch (std::exception& e) {
        m.err = std::string("model file unreadable: ") + e.what();
    }
    return m;
}

std::vector<std::string> gpuBadZone(const GpuFeat& f) {
    std::vector<std::string> why;
    if (f.strength < 0.999) why.push_back("stiffness strength below 1 (Svanrige: +65 ms from that key alone, L584)");
    if (f.stiffness <= 0.25 + 1e-6 && f.tip < 0.01) why.push_back("stiffness .25 or lower with tip stiffness 0 (Geralt .186 / 0: +43 ms, L634)");
    if (f.curve[3] <= 0.05) why.push_back("stiffness curve reaching 0 at the tip (Junior 1 / .4 / .1 / 0: 309 ms, L641)");
    return why;
}

GpuEstimate gpuEstimate(const GpuModel& m, const GpuFeat& f, const std::string& character, double budget) {
    GpuEstimate e;
    if (!m.loaded) return e;
    std::string ch = character;
    std::transform(ch.begin(), ch.end(), ch.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    auto it = m.kappa.find(ch);
    e.anchored = it != m.kappa.end();
    e.kappa = e.anchored ? it->second : 1.0;
    e.extra = m.phi(f, e.kappa) - m.phi(m.ves, 1.0);
    double H = m.drawnHairs(f) / 1e4, S = (f.K - 1) * f.sm / 36.0, Hv = m.drawnHairs(m.ves) / 1e4, Sv = (m.ves.K - 1) * m.ves.sm / 36.0;
    e.render = m.a * (H * S - Hv * Sv);
    e.motion = e.extra - e.render;
    e.err = e.anchored ? std::max(m.errAbsAnch, m.errRelAnch * std::fabs(e.extra)) : std::max(m.errAbsGlob, m.errRelGlob * std::fabs(e.extra));
    e.hold = m.hold(f);
    e.hairs = m.drawnHairs(f);
    e.bad = gpuBadZone(f);
    e.colour = !e.bad.empty() ? 2 : e.extra <= budget ? 0 : e.extra <= budget * 1.5 ? 1 : 2;
    e.valid = true;
    return e;
}

// ================================================================================================ App side
GpuFeat App::gpuFeatures() const {
    GpuFeat f;
    f.faces = (double)groom.faces.size();
    f.hpp = hw.ptHairsPerFaceGame > 0 ? hw.ptHairsPerFaceGame : 32;
    f.K = groom.K;
    f.sm = std::max(pack.splineMultiplier, 1);
    double ls = 0;
    int n = 0;
    for (size_t i = 0; i < groom.P.size(); i++) {
        if (i < groom.alive.size() && !groom.alive[i]) continue;
        const Guide& g = groom.P[i];
        if (g.size() < 2) continue;
        double l = 0;
        for (size_t k = 1; k < g.size(); k++) { V3 d = g[k] - g[k - 1]; l += std::sqrt(double(dot(d, d))); }
        ls += l; n++;
    }
    f.lenM = n ? ls / n : 0.2;
    f.dld = look.f(P_DLD, 1.0f);
    f.lodOn = look.f("levelOfDetail.enableLOD", 1.0f);
    f.density = look.f(P_DENS, 1.0f);
    // a density EDIT below 1 is exported as a flat density map (studio_to_config look.density_flat): the game thins by it
    f.keepExtra = (look.over.count(P_DENS) && f.density > 0 && f.density < 1) ? f.density : 1.0;
    f.stiffness = look.f(P_STIFF, 0.5f);
    f.root = look.f(P_ROOT, 0.0f);
    f.tip = look.f(P_TIP, 0.0f);
    f.bend = look.f(P_BEND, 0.0f);
    f.strength = look.f(P_STR, 1.0f);
    json c = look.get(P_CURVE);
    if (c.is_object()) { f.curve[0] = c.value("X", 1.0); f.curve[1] = c.value("Y", 1.0); f.curve[2] = c.value("Z", 1.0); f.curve[3] = c.value("W", 1.0); }
    return f;
}

void App::gpuEnsureModel() {
    if (gpuModelTried) return;
    gpuModelTried = true;
    gpuModel = gpuLoadModel(set.repo);
}

std::string App::gpuCharacter() const {
    std::string c = pack.character.empty() ? configName.substr(0, configName.find('_')) : pack.character;
    if (fur == 1) c += "_beard";   // beard furs are not in the model
    return c;
}

GpuEstimate App::gpuEstimateNow() {
    gpuEnsureModel();
    return gpuEstimate(gpuModel, gpuFeatures(), gpuCharacter(), gb.budget);
}

static json estJson(const GpuEstimate& e, double budget) {
    static const char* col[3] = {"green", "amber", "red"};
    return {{"extra_ms", std::round(e.extra * 100) / 100}, {"err_ms", std::round(e.err * 100) / 100}, {"render_ms", std::round(e.render * 100) / 100},
            {"motion_ms", std::round(e.motion * 100) / 100}, {"budget_ms", budget}, {"colour", col[std::clamp(e.colour, 0, 2)]},
            {"anchored", e.anchored}, {"kappa", e.kappa}, {"hold", std::round(e.hold * 1000) / 1000}, {"drawn_hairs", std::round(e.hairs)},
            {"bad_zone", e.bad}};
}

// keep share k of the current drawn hairs -> the look value of the chosen route
static void routeValue(const GpuModel& m, const GpuFeat& f, int route, double k, std::string& path, double& value) {
    if (route == 1) {   // density: flat density map at export (studio_to_config look.density_flat)
        path = P_DENS;
        value = clampd(f.keepExtra * k, 0.02, 1.0);
        return;
    }
    // close-up LOD density: keep = clamp(1 + (dld - 1) x t, 0, 1) at the head-shot distance (t = keepT, L480)
    double keep0 = f.lodOn > 0.5 ? clampd(1.0 + (f.dld - 1.0) * m.keepT, 0, 1) : 1.0;
    double target = keep0 * k;
    path = P_DLD;
    value = k >= 0.999 ? f.dld : std::max(0.0, 1.0 + (target - 1.0) / m.keepT);
}

static GpuFeat withKeep(const GpuModel& m, GpuFeat f, int route, double k) {
    std::string p;
    double v;
    routeValue(m, f, route, k, p, v);
    if (route == 1) f.keepExtra = v; else f.dld = v;
    return f;
}

void App::gbCompute() {
    gpuEnsureModel();
    std::map<std::array<double, 3>, std::array<double, 3>> motionOf;   // keep measured motion across recomputes (same physics)
    for (auto& r : gb.rows) if (r.motionMm >= 0) motionOf[{r.stiffness, r.tip, r.strength}] = {r.motionMm, r.tipMm, r.stuckPct};
    gb.rows.clear();
    if (!gpuModel.loaded || !loaded) return;
    GpuFeat f0 = gpuFeatures();
    std::string ch = gpuCharacter();
    std::vector<double> grid = {1.0, 0.8, 0.7, 0.6, 0.5, 0.45, 0.4, 0.35, 0.3, 0.25};
    auto addRow = [&](double s, double tip, double strength, bool current, bool custom) {
        GbRow r;
        r.stiffness = s; r.tip = tip; r.strength = strength; r.current = current; r.custom = custom;
        GpuFeat f = f0;
        f.stiffness = s; f.tip = tip; f.strength = strength;
        GpuEstimate full = gpuEstimate(gpuModel, f, ch, gb.budget);
        r.extraFull = full.extra;
        r.badWhy = full.bad;
        r.bad = !full.bad.empty();
        r.noData = !r.bad && s < 0.35 - 1e-6;
        // largest share of the hairs that fits the budget (bisection; the cost grows with the share)
        double lo = 0.25, hi = 1.0;
        if (full.extra <= gb.budget) r.keep = 1.0;
        else if (gpuEstimate(gpuModel, withKeep(gpuModel, f, gb.route, lo), ch, gb.budget).extra > gb.budget) r.keep = lo;
        else {
            for (int it = 0; it < 40; it++) {
                double mid = 0.5 * (lo + hi);
                if (gpuEstimate(gpuModel, withKeep(gpuModel, f, gb.route, mid), ch, gb.budget).extra <= gb.budget) lo = mid; else hi = mid;
            }
            r.keep = lo;
        }
        GpuEstimate e = gpuEstimate(gpuModel, withKeep(gpuModel, f, gb.route, r.keep), ch, gb.budget);
        r.extra = e.extra; r.err = e.err; r.hairs = e.hairs;
        r.fits = e.extra <= gb.budget + 1e-6;
        auto mo = motionOf.find({r.stiffness, r.tip, r.strength});
        if (mo != motionOf.end()) { r.motionMm = mo->second[0]; r.tipMm = mo->second[1]; r.stuckPct = mo->second[2]; }
        gb.rows.push_back(r);
    };
    // the groom as it is (current physics, every hair) first; then the options (tip raised to .1 and strength 1: the
    // measured-safe side, FLOWING_HAIR_GPU.md 7), the custom stiffness, and the measured-bad soft pair for reference (red)
    addRow(f0.stiffness, f0.tip, f0.strength, true, false);
    double tip = std::max(f0.tip, 0.1);
    for (double s : grid) addRow(s, tip, 1.0, false, false);
    addRow(gb.customStiff, tip, 1.0, false, true);
    addRow(0.186, 0.0, 1.0, false, false);
    // recommended: the lowest stiffness that fits the budget while keeping at least minKeep of the hairs, outside the bad zone
    // and not below .35 (the measured floor, FLOWING_HAIR_GPU.md R2: no row measures stiffness .25-.35 with tip .1)
    int best = -1;
    for (int i = 0; i < (int)gb.rows.size(); i++) {
        const GbRow& r = gb.rows[i];
        if (r.current || r.custom || r.bad || r.noData || !r.fits || r.keep + 1e-6 < gb.minKeep) continue;
        if (best < 0 || r.stiffness < gb.rows[best].stiffness) best = i;
    }
    if (best >= 0) gb.rows[best].recommended = true;
}

// mean CV displacement of every guide from its settled rest under the Studio wind (wind_motion.py metric), for the
// physics of one row; the look and the motion state are restored afterwards
json App::gbMeasureMotion(double stiffness, double tip, double strength, float windStrength, const std::vector<float>& yaws, float windS) {
    json r;
    if (!sdkOk || !loaded) { r["error"] = "no simulation"; return r; }
    auto over0 = look.over;
    look.setF(P_STIFF, (float)stiffness);
    look.setF(P_TIP, (float)tip);
    look.setF(P_STR, (float)strength);
    float yaw0 = motion.windYawDeg, pit0 = motion.windPitchDeg, ws0 = motion.windStrength, sp0 = motion.speed, am0 = motion.amount;
    auto pre0 = motion.preset;
    double t0 = motion.t;
    bool sim0 = simulate, wind0 = windOn, paused0 = paused;
    int steps = std::max(30, int(windS * 60));
    std::vector<float> xyz;
    int PGn = 0;
    std::map<int, std::vector<V3>> rest;
    std::vector<double> perGuide;   // sum over headings of the mean CV displacement
    std::vector<double> tipSum;
    std::vector<int> gid;
    int nHead = 0;
    for (int di = -1; di < (int)yaws.size(); di++) {
        bool isRest = di < 0;
        motion.windYawDeg = isRest ? yaw0 : yaws[di];
        motion.windPitchDeg = 2.85f;
        rebuildHair();
        simulate = true; paused = false; windOn = !isRest;
        motion.preset = MotionPreset::Rest; motion.speed = 1; motion.amount = 1; motion.windStrength = isRest ? 0.0f : windStrength;
        motion.t = 0; simAcc = 0; teleport = true;
        int nSteps = isRest ? std::max(60, steps) : steps;
        for (int s = 0; s < nSteps; s++) step(1.0 / 60.0);
        if (!hw.readGuides(xyz, PGn) || PGn < 2) continue;
        if (isRest) {
            for (size_t aI = 0; aI < guideOfAsset.size(); aI++) {
                int g = guideOfAsset[aI];
                if (g < 0 || g >= (int)groom.alive.size() || !groom.alive[g]) continue;
                std::vector<V3> cv(PGn);
                for (int j = 0; j < PGn; j++) cv[j] = {xyz[(aI * PGn + j) * 3], xyz[(aI * PGn + j) * 3 + 1], xyz[(aI * PGn + j) * 3 + 2]};
                rest[(int)aI] = cv;
            }
            perGuide.assign(guideOfAsset.size(), 0.0);
            tipSum.assign(guideOfAsset.size(), 0.0);
            continue;
        }
        nHead++;
        for (auto& [aI, cv] : rest) {
            double sum = 0;
            for (int j = 0; j < PGn; j++) {
                V3 p{xyz[(aI * PGn + j) * 3], xyz[(aI * PGn + j) * 3 + 1], xyz[(aI * PGn + j) * 3 + 2]};
                V3 d = p - cv[j];
                double dd = std::sqrt(double(dot(d, d)));
                sum += dd;
                if (j == PGn - 1) tipSum[aI] += dd;
            }
            perGuide[aI] += sum / PGn;
        }
    }
    motion.windYawDeg = yaw0; motion.windPitchDeg = pit0; motion.windStrength = ws0; motion.speed = sp0; motion.amount = am0;
    motion.preset = pre0; motion.t = t0;
    simulate = sim0; windOn = wind0; paused = paused0;
    look.over = over0;
    teleport = true;
    rebuildHair();
    for (int i = 0; i < 30; i++) step(1.0 / 60.0);
    if (rest.empty() || nHead == 0) { r["error"] = "simulated guides not read back"; return r; }
    double tot = 0, tipT = 0;
    int stuck = 0;
    for (auto& [aI, cv] : rest) {
        double m = perGuide[aI] / nHead * 1000.0;
        tot += m; tipT += tipSum[aI] / nHead * 1000.0;
        if (m < 2.0) stuck++;
    }
    double n = (double)rest.size();
    r["mean_mm"] = std::round(tot / n * 100) / 100;
    r["tip_mm"] = std::round(tipT / n * 10) / 10;
    r["stuck_pct"] = std::round(1000.0 * stuck / n) / 10;
    r["guides"] = (int)rest.size();
    r["headings"] = nHead;
    return r;
}

void App::gbMeasureRow(int i) {
    if (i < 0 || i >= (int)gb.rows.size()) return;
    GbRow& r = gb.rows[i];
    json m = gbMeasureMotion(r.stiffness, r.tip, r.strength, 1.0f, {0.0f, 90.0f, 180.0f, 270.0f}, 2.5f);
    if (m.contains("mean_mm")) { r.motionMm = m["mean_mm"]; r.tipMm = m["tip_mm"]; r.stuckPct = m["stuck_pct"]; }
}

json App::gbApply(int i) {
    json r;
    if (i < 0 || i >= (int)gb.rows.size()) { r["error"] = "no such row"; return r; }
    const GbRow row = gb.rows[i];
    if (row.bad) { r["error"] = "this row is in the measured-bad zone and is never applied: " + (row.badWhy.empty() ? std::string() : row.badWhy[0]); return r; }
    GpuFeat f0 = gpuFeatures();
    GpuBudget::Undo u;
    u.over = look.over; u.ops = groom.ops.size(); u.colUndo = colUndoSt.size();
    std::string p;
    double v;
    routeValue(gpuModel, f0, gb.route, row.keep, p, v);
    look.setF(P_STIFF, (float)row.stiffness);
    look.setF(P_TIP, (float)row.tip);
    if (row.strength != f0.strength) look.setF(P_STR, (float)row.strength);
    if (row.keep < 0.999) look.setF(p, (float)v);
    char b[200];
    snprintf(b, sizeof b, "stiffness %.3g, tip %.2g%s, %s %.3g (%.0f %% of the hairs)", row.stiffness, row.tip, row.strength != f0.strength ? ", strength 1" : "",
             gb.route == 1 ? "density" : "close-up LOD density", row.keep < 0.999 ? v : (gb.route == 1 ? f0.keepExtra : f0.dld), row.keep * 100);
    u.what = b;
    gb.undo.push_back(u);
    gb.lastApplied = b;
    computeBudget();
    gbCompute();
    status = std::string("Auto-balance applied: ") + b + " (Ctrl+Z undoes it)";
    r["applied"] = b;
    r["set"] = {{P_STIFF, row.stiffness}, {P_TIP, row.tip}, {P_STR, row.strength}};
    if (row.keep < 0.999) r["set"][p] = v;
    r["estimate"] = estJson(gpuEstimateNow(), gb.budget);
    return r;
}

bool App::gbUndo() {
    if (gb.undo.empty()) return false;
    look.over = gb.undo.back().over;
    std::string w = gb.undo.back().what;
    gb.undo.pop_back();
    computeBudget();
    if (gb.open) gbCompute();
    status = "Auto-balance undone (" + w + ")";
    return true;
}

// Ctrl+Z: an auto-balance apply made after the last groom op / collider change is undone first
bool App::gbUndoKey() {
    if (gb.undo.empty()) return false;
    const auto& u = gb.undo.back();
    if (groom.ops.size() != u.ops || colUndoSt.size() != u.colUndo) return false;
    return gbUndo();
}

static json rowJson(const GbRow& r) {
    json j = {{"stiffness", r.stiffness}, {"tip", r.tip}, {"strength", r.strength}, {"keep", std::round(r.keep * 1000) / 1000},
              {"drawn_hairs", std::round(r.hairs)}, {"extra_ms", std::round(r.extra * 100) / 100}, {"err_ms", std::round(r.err * 100) / 100},
              {"extra_ms_all_hairs", std::round(r.extraFull * 100) / 100}, {"fits", r.fits}, {"bad_zone", r.badWhy}};
    if (r.current) j["current"] = true;
    if (r.custom) j["custom"] = true;
    if (r.recommended) j["recommended"] = true;
    if (r.noData) j["no_data"] = "stiffness below .35: no measured row (extrapolated; never recommended)";
    if (r.motionMm >= 0) { j["motion_mm"] = r.motionMm; j["tip_mm"] = r.tipMm; j["stuck_pct"] = r.stuckPct; }
    return j;
}

json App::gpuEstimateCmd(const json& c) {
    json r;
    gpuEnsureModel();
    if (!gpuModel.loaded) { r["error"] = gpuModel.err; return r; }
    if (c.contains("budget")) gb.budget = c["budget"].get<float>();
    GpuFeat f = gpuFeatures();
    // "set": {path: value} = what-if values (nothing is changed on the groom)
    if (c.contains("set") && c["set"].is_object()) {
        for (auto& [k, v] : c["set"].items()) {
            if (!v.is_number()) continue;
            double x = v.get<double>();
            if (k == P_STIFF) f.stiffness = x; else if (k == P_ROOT) f.root = x; else if (k == P_TIP) f.tip = x; else if (k == P_BEND) f.bend = x;
            else if (k == P_STR) f.strength = x; else if (k == P_DLD) f.dld = x; else if (k == P_DENS) f.keepExtra = (x > 0 && x < 1) ? x : 1.0;
            else if (k == "keep") f.keepExtra *= clampd(x, 0, 1);
        }
    }
    GpuEstimate e = gpuEstimate(gpuModel, f, gpuCharacter(), gb.budget);
    r = estJson(e, gb.budget);
    r["features"] = f.toJson();
    r["character"] = gpuCharacter();
    r["note"] = std::string("ESTIMATE, not measured: model fitted on ") + std::to_string(gpuModel.fitRows) +
                " measured game rows (same-launch deltas over Vesemir; gpu_cost_model.py). " +
                (e.anchored ? "This character has measured rows: its own motion scale is used." : "This character has no measured rows: the global scale is used (wider error).");
    r["no_data"] = gpuModel.noData;
    return r;
}

json App::autoBalanceCmd(const json& c) {
    json r;
    gpuEnsureModel();
    if (!gpuModel.loaded) { r["error"] = gpuModel.err; return r; }
    if (c.value("undo", false)) { r["undone"] = gbUndo(); r["estimate"] = estJson(gpuEstimateNow(), gb.budget); return r; }
    if (c.contains("budget")) gb.budget = c["budget"].get<float>();
    if (c.contains("min_keep")) gb.minKeep = c["min_keep"].get<float>();
    if (c.contains("stiffness")) gb.customStiff = c["stiffness"].get<float>();
    if (c.contains("route")) gb.route = c["route"].is_string() ? (c["route"].get<std::string>() == "density" ? 1 : 0) : c["route"].get<int>();
    gbCompute();
    if (c.value("motion", false))
        for (int i = 0; i < (int)gb.rows.size(); i++) gbMeasureRow(i);
    json rows = json::array();
    for (auto& rw : gb.rows) rows.push_back(rowJson(rw));
    r["rows"] = rows;
    r["budget_ms"] = gb.budget;
    r["route"] = gb.route == 1 ? "density (flat density map at export)" : "close-up LOD density (levelOfDetail.detailLOD.detailLODDensity)";
    r["current"] = estJson(gpuEstimateNow(), gb.budget);
    int best = -1;
    for (int i = 0; i < (int)gb.rows.size(); i++) if (gb.rows[i].recommended) best = i;
    r["recommended"] = best;
    if (c.contains("apply")) {
        int i = -1;
        if (c["apply"].is_string() && c["apply"].get<std::string>() == "recommended") i = best;
        else if (c["apply"].is_number_integer()) i = c["apply"].get<int>();
        r["apply"] = gbApply(i);
    }
    r["note"] = "ESTIMATE, not measured. keep = share of the current drawn hairs that fits the budget (never below .25; guides are never deleted). "
                "Rows marked bad_zone are the measured-bad physics and are never applied. motion_mm = Studio wind simulation (wind 1, 4 headings, "
                "mean CV displacement from the settled rest; wind_motion.py metric), only with motion: true.";
    return r;
}
