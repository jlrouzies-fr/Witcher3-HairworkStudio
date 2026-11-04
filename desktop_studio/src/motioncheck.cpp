// Motion check (2026-10-18, Geralt headband: hair calm in the Studio, flying out on the running / fighting player).
// motionReport: the hair settles at rest (teleport + settle_s), then each motion preset runs from that settled state (no
// teleport: the Sprint-stop / Dodge / Sword presets start in the rest pose) while every simulated guide CV is read back and
// expressed in the HEAD frame (inverse of the head skinning matrix of that step). Displacement = distance from the
// settled rest in the head frame = how far the hair moves relative to the head: what reads as "flying out" in game.
// Params: presets (["Run", "SprintStop", "Dodge", "SwordSwings"]), seconds (8), warmup_s (1: not measured), settle_s (2),
// step_hz (60 = Studio; 30 = one simulation step per frame at the game's 30 FPS cap), amount (1), wind (0 = no wind;
// > 0: Studio wind of that strength on top, heading wind_yaw_deg), fly_mm (80: a guide "flies" when its tip leaves the
// rest by more), set {sdk path: number} (simulation values for this report only, e.g.
// "physicalMaterials.simulation.damping"; restored afterwards), series (true: per 0.25 s mean / p95 tip displacement).
// Restores the look, motion and simulation state.
#include <algorithm>
#include <cmath>
#include "app.h"

using namespace DirectX;

json App::motionReport(const json& c) {
    json r;
    if (!sdkOk || !loaded) { r["error"] = "no simulation"; return r; }
    json presets = c.value("presets", json::array({"Run", "SprintStop", "Dodge", "SwordSwings"}));
    const double seconds = std::clamp(c.value("seconds", 8.0), 1.0, 60.0), warm = std::clamp(c.value("warmup_s", 1.0), 0.0, seconds - 0.5);
    const double settleS = std::clamp(c.value("settle_s", 2.0), 0.5, 20.0);
    const double hz = std::clamp(c.value("step_hz", 60.0), 15.0, 240.0), dt = 1.0 / hz;
    const float amount = c.value("amount", 1.0f), wind = c.value("wind", 0.0f), flyMm = c.value("fly_mm", 80.0f);
    const bool wantSeries = c.value("series", true);
    auto lookOver = look.over;
    bool sSim = simulate, sPaused = paused, sWind = windOn;
    Motion sMotion = motion;
    double sAcc = simAcc, sStep = simStep;
    json setOut = json::object();
    if (c.contains("set") && c["set"].is_object())
        for (auto& [k, v] : c["set"].items())
            if (v.is_number()) { look.setF(k, v.get<float>()); setOut[k] = v; }
    const char* simKeys[] = {"physicalMaterials.simulation.damping", "physicalMaterials.simulation.massScale", "physicalMaterials.simulation.inertiaLimit",
                             "physicalMaterials.stiffness.stiffnessDamping", "physicalMaterials.stiffness.stiffness", "physicalMaterials.stiffness.bendStiffness",
                             "physicalMaterials.stiffness.rootStiffness", "physicalMaterials.stiffness.tipStiffness", "physicalMaterials.simulation.windScaler",
                             "physicalMaterials.simulation.windNoise", "physicalMaterials.simulation.backStopRadius"};
    json used = json::object();
    for (auto k : simKeys) used[k] = std::round(look.f(k, -1.0f) * 1e4) / 1e4;
    r["sim_values"] = used;
    if (!setOut.empty()) r["set"] = setOut;
    simStep = dt;
    auto restore = [&]() {
        look.over = lookOver;
        simStep = sStep;
        rebuildHair();
        motion = sMotion; motion.preset = MotionPreset::Rest; windOn = false; teleport = true;
        for (int i = 0; i < 30; i++) step(1.0 / 60.0);
        motion = sMotion; simulate = sSim; paused = sPaused; windOn = sWind; simAcc = sAcc; teleport = true; aoRefreshAll = true;
    };
    std::vector<float> xyz;
    int PG = 0;
    const size_t NA = guideOfAsset.size();
    auto headInv = [&]() { return XMMatrixInverse(nullptr, motion.headM(skel)); };
    auto toHead = [](const float* p, const XMMATRIX& Hi) {
        XMVECTOR v = XMVector3TransformCoord(XMVectorSet(p[0], p[1], p[2], 1), Hi);
        return V3{XMVectorGetX(v), XMVectorGetY(v), XMVectorGetZ(v)};
    };
    json runs = json::array();
    for (auto& pj : presets) {
        std::string pn = pj.is_string() ? pj.get<std::string>() : std::string();
        MotionPreset pre = motionFromName(pn, MotionPreset::Count);
        if (pre == MotionPreset::Count) { runs.push_back({{"preset", pn}, {"error", "unknown preset"}}); continue; }
        // settle at rest
        rebuildHair();
        simulate = true; paused = false; windOn = wind > 0;
        if (c.contains("wind_yaw_deg")) motion.windYawDeg = c["wind_yaw_deg"].get<float>();
        motion.preset = MotionPreset::Rest; motion.speed = 1; motion.amount = 1; motion.windStrength = wind; motion.t = 0; simAcc = 0; teleport = true;
        int nSettle = std::max(1, int(settleS * hz + 0.5));
        for (int s = 0; s < nSettle; s++) step(dt);
        std::string info;
        if (!hw.readGuides(xyz, PG, &info) || PG < 2 || xyz.size() < NA * size_t(PG) * 3) { restore(); r["error"] = "guide read-back failed: " + info; return r; }
        XMMATRIX Hi = headInv();
        std::vector<V3> rest(NA * PG);
        for (size_t i = 0; i < NA * PG; i++) rest[i] = toHead(&xyz[i * 3], Hi);
        // the preset from the settled state (its clock starts at 0, no teleport)
        motion.preset = pre; motion.amount = amount; motion.t = 0; simAcc = 0;
        int nSteps = std::max(1, int(seconds * hz + 0.5)), nWarm = int(warm * hz + 0.5);
        std::vector<float> tipMax(NA, 0.0f), cvMax(NA, 0.0f), riseMax(NA, -1e9f);
        double tipSum = 0, cvSum = 0;
        long nTip = 0, nCv = 0;
        int frames = 0;
        json ser = json::array();
        std::vector<float> bucket;
        int bucketSteps = std::max(1, int(0.25 * hz + 0.5)), bucketN = 0;
        double peakSpeed = 0;
        V3 lastTr = motion.travel();
        for (int s = 0; s < nSteps; s++) {
            step(dt);
            V3 tr = motion.travel();
            peakSpeed = std::max(peakSpeed, double(len(tr - lastTr)) / dt);
            lastTr = tr;
            if (s < nWarm) continue;
            int pg = PG;
            if (!hw.readGuides(xyz, pg) || pg != PG || xyz.size() < NA * size_t(PG) * 3) continue;
            frames++;
            Hi = headInv();
            for (size_t a = 0; a < NA; a++) {
                for (int j = 1; j < PG; j++) {
                    size_t q = a * PG + j;
                    V3 p = toHead(&xyz[q * 3], Hi);
                    float d = len(p - rest[q]);
                    cvMax[a] = std::max(cvMax[a], d);
                    cvSum += d; nCv++;
                    if (j == PG - 1) {
                        tipMax[a] = std::max(tipMax[a], d);
                        riseMax[a] = std::max(riseMax[a], p.z - rest[q].z);
                        tipSum += d; nTip++;
                        if (wantSeries) bucket.push_back(d);
                    }
                }
            }
            if (wantSeries && ++bucketN >= bucketSteps) {
                std::sort(bucket.begin(), bucket.end());
                double m = 0;
                for (float v : bucket) m += v;
                m /= std::max<size_t>(1, bucket.size());
                float p95 = bucket.empty() ? 0.0f : bucket[std::min(bucket.size() - 1, size_t(0.95 * (bucket.size() - 1) + 0.5))];
                ser.push_back({std::round(motion.t * 100) / 100, std::round(m * 1e4) / 10, std::round(p95 * 1e4) / 10});
                bucket.clear(); bucketN = 0;
            }
        }
        if (!frames) { restore(); r["error"] = "simulated guides not read back"; return r; }
        std::vector<float> st = tipMax;
        std::sort(st.begin(), st.end());
        auto pct = [&](double q) { return std::round(st[std::min(st.size() - 1, size_t(q * (st.size() - 1) + 0.5))] * 1e4) / 10; };
        int fly = 0, rise = 0;
        for (size_t a = 0; a < NA; a++) { if (tipMax[a] * 1000 > flyMm) fly++; if (riseMax[a] > 0.05f) rise++; }
        json run = {{"preset", motionName(pre)}, {"frames", frames}, {"guides", (int)NA}, {"peak_root_speed_ms", std::round(peakSpeed * 100) / 100},
                    {"tip_mean_mm", std::round(tipSum / std::max<long>(1, nTip) * 1e4) / 10}, {"cv_mean_mm", std::round(cvSum / std::max<long>(1, nCv) * 1e4) / 10},
                    {"tip_max_mm", {{"p50", pct(0.5)}, {"p90", pct(0.9)}, {"p99", pct(0.99)}, {"max", pct(1.0)}}},
                    {"guides_fly", fly}, {"guides_tip_rise_50mm", rise}};
        if (wantSeries) run["series_t_mean_p95_mm"] = ser;
        runs.push_back(run);
    }
    r["runs"] = runs;
    r["step_hz"] = hz; r["seconds"] = seconds; r["warmup_s"] = warm; r["fly_mm"] = flyMm; r["wind"] = wind;
    r["metric"] = "tip / CV displacement from the settled rest in the head frame; tip_max = per-guide maximum over the run (percentiles over guides)";
    restore();
    return r;
}
