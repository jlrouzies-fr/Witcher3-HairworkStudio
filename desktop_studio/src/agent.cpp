// Desktop Groom Studio - headless agent runner (no UI): JSON script -> edits, material, stiffness, simulation, HairLab
// shot renders (+ PAIR with the vanilla still), budget report, export. Our own code (MIT). Format: README "Agent CLI".
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "app.h"
#include "lighting.h"

namespace fs = std::filesystem;
using namespace DirectX;

bool writePng(const std::string& path, int w, int h, const unsigned char* rgba);
unsigned char* loadImage(const std::string& path, int* w, int* h);
void freeImage(unsigned char* p);

static void runSim(App& a, double seconds) {
    int n = std::max(1, int(seconds * 60.0 + 0.5));
    for (int i = 0; i < n; i++) a.step(1.0 / 60.0);
}

bool App::renderShot(const json& shot, const std::string& outPng, int size) {
    XMFLOAT3 eye{shot["eye"][0], shot["eye"][1], shot["eye"][2]}, tgt{shot["target"][0], shot["target"][1], shot["target"][2]};
    float fovy = shot.value("fovy", 11.16f);
    XMStoreFloat4x4(&ovView, XMMatrixLookAtRH(XMLoadFloat3(&eye), XMLoadFloat3(&tgt), XMVectorSet(0, 0, 1, 0)));
    XMStoreFloat4x4(&ovProj, XMMatrixPerspectiveFovRH(XMConvertToRadians(fovy), 1.0f, 0.05f, 100.0f));
    ovFovDeg = fovy;
    camOverride = true;
    aoRefreshAll = true;   // shots: every sky-occlusion map up to date with this frame's hair
    if (!gfx.beginOffscreen(size)) { camOverride = false; return false; }
    drawScene();
    st = hw.stats();
    bool ok = gfx.endOffscreen(outPng);
    camOverride = false;
    return ok;
}

// left = vanilla still (HairLab head crop), right = ours, like the HairLab PAIR images
static bool makePair(const std::string& left, const std::string& right, const std::string& out) {
    int lw, lh, rw, rh;
    unsigned char* L = loadImage(left, &lw, &lh);
    unsigned char* R = loadImage(right, &rw, &rh);
    if (!L || !R) { if (L) freeImage(L); if (R) freeImage(R); return false; }
    int h = rh, w = rw;
    std::vector<unsigned char> px(size_t(w) * 2 * h * 4, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = x * lw / w, sy = y * lh / h;   // nearest resample of the still to the render size
            memcpy(&px[(size_t(y) * w * 2 + x) * 4], &L[(size_t(sy) * lw + sx) * 4], 4);
        }
        memcpy(&px[(size_t(y) * w * 2 + w) * 4], &R[size_t(y) * w * 4], size_t(w) * 4);
    }
    freeImage(L);
    freeImage(R);
    return writePng(out, w * 2, h, px.data());
}

static MotionPreset motionByName(const std::string& n) {
    std::string a = n;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    for (int i = 0; i < (int)MotionPreset::Count; i++) {
        std::string m = motionName(MotionPreset(i));
        std::transform(m.begin(), m.end(), m.begin(), ::tolower);
        if (m.find(a) == 0) return MotionPreset(i);
    }
    return MotionPreset::Idle;
}

int App::runAgent(const std::string& scriptPath) {
    json S;
    try { std::ifstream f(scriptPath); S = json::parse(f); } catch (std::exception& e) { logf("agent: cannot read %s: %s", scriptPath.c_str(), e.what()); return 2; }
    std::string out = S.value("out", set.repo + "\\build\\desktop_studio\\agent");
    fs::create_directories(out);
    json report;
    report["script"] = scriptPath;
    report["steps"] = json::array();
    int rc = 0;
    if (!sdkOk) { logf("agent: HairWorks runtime not loaded (%s)", status.c_str()); return 3; }
    if (S.contains("project")) { if (!loadProject(S["project"])) return 4; }
    else if (!openConfig(S.value("cfg", ""), S.value("reprep", false))) { logf("agent: cannot open %s", S.value("cfg", "").c_str()); return 4; }
    motion.preset = MotionPreset::Rest;
    teleport = true;
    runSim(*this, 0.1);
    for (auto& stp : S.value("steps", json::array())) {
        std::string d = stp.value("do", "");
        json r = {{"do", d}};
        if (d == "ops") {
            json ops = stp.contains("file") ? json() : stp.value("ops", json::array());
            if (stp.contains("file")) { std::ifstream f(stp["file"].get<std::string>()); ops = json::parse(f)["ops"]; }
            for (auto& o : ops) groom.ops.push_back(o);
            groom.redo.clear();
            groom.replay();
            editsChanged();
            r["ops"] = ops.size();
            r["unmatched"] = groom.unmatched;
        } else if (d == "select") {
            auto ix = selectGuides(stp.value("where", json::object()));
            sel = std::set<int>(ix.begin(), ix.end());
            r["selected"] = ix.size();
        } else if (d == "curl") {
            curl = CurlParams::fromJson(stp.value("params", json::object()));
            auto ix = stp.contains("where") ? selectGuides(stp["where"]) : selList();
            opCurl(ix);
            r["guides"] = ix.size();
            r["cvs_per_turn"] = curlCvsPerTurn(pack.K, curl);
            if (curlCvsPerTurn(pack.K, curl) < 6) r["warning"] = "curl under-sampled (< 6 CVs per turn)";
        } else if (d == "delete" || d == "shorten" || d == "duplicate") {
            auto ix = stp.contains("where") ? selectGuides(stp["where"]) : selList();
            sel = std::set<int>(ix.begin(), ix.end());
            if (d == "delete") opDelete();
            if (d == "shorten") { keepPct = stp.value("keep", 0.8f) * 100; opShorten(); }
            if (d == "duplicate") { dupStepMm = stp.value("step_mm", 6.0f); opDuplicate(); }
            r["guides"] = ix.size();
        } else if (d == "material") {
            keepColours = stp.value("keep_colours", true);
            std::string n = stp.value("name", "");
            int found = -1;
            for (int i = 0; i < (int)presets.size(); i++) if (presets[i].name.rfind(n, 0) == 0) { found = i; break; }
            if (found < 0) { r["error"] = "no preset " + n; rc = 5; }
            else { applyMaterial(found); r["preset"] = presets[found].name; }
        } else if (d == "look") {
            json setv = stp.value("set", json::object());
            for (auto& [k, v] : setv.items()) {
                if (v.is_number()) look.setF(k, v.get<float>()); else look.setJson(k, v);
            }
            look.enforceCaps();
            computeBudget();
        } else if (d == "hold_curls") {
            holdCurlsPhysics();
        } else if (d == "bone_stiffness") {
            // {"do": "bone_stiffness", "amount": 0..1}: W3 stiffnessBone emulation strength (0 = raw 1.2.1 runtime)
            hw.boneStiffEmu = std::clamp(stp.value("amount", 1.0f), 0.0f, 1.0f);
            r["amount"] = hw.boneStiffEmu;
        } else if (d == "dump_params") {
            // fur value vs SDK instance descriptor value, param by param (after one step so the descriptor is current)
            step(0.0);
            std::string p = stp.value("path", out + "\\params_dump.json");
            std::ofstream(p) << hw.paramDump(look);
            r["path"] = p;
        } else if (d == "simulate") {
            // {"do": "simulate", "on": false}: hair stays on the skinned rest shape (calibration: rest vs settled sim)
            simulate = stp.value("on", true);
            teleport = true;
            r["simulate"] = simulate;
        } else if (d == "stiffness") {
            auto ix = selectGuides(stp.value("where", json::object({{"all", true}})));
            smap.ensure(groom.P.size());
            for (int i : ix) {
                if (stp.contains("s")) smap.guideS[i] = stp["s"];
                if (stp.contains("r")) smap.guideR[i] = stp["r"];
            }
            stiffDirty = true;
            r["guides"] = ix.size();
        } else if (d == "rig") {
            if (stp.contains("tod")) applyTimeOfDay(rig, stp["tod"].get<int>());   // first: explicit keys below override it
            rig.sun = stp.value("sun", rig.sun); rig.sky = stp.value("sky", rig.sky); rig.exposure = stp.value("exposure", rig.exposure);
            rig.colourGain = stp.value("colour_gain", rig.colourGain); rig.specGain = stp.value("spec_gain", rig.specGain);
            rig.selfShadow = stp.value("self_shadow", rig.selfShadow);
            rig.floor = stp.value("floor", rig.floor);
            rig.texGain = stp.value("tex_gain", rig.texGain);
            rig.specPower = stp.value("spec_power", rig.specPower);
            rig.sunAz = stp.value("sun_az", rig.sunAz); rig.sunEl = stp.value("sun_el", rig.sunEl);
            rig.shadows = stp.value("shadows", rig.shadows);
            rig.horizon = stp.value("horizon", rig.horizon); rig.ground = stp.value("ground", rig.ground);
            rig.sigmaScale = stp.value("sigma_scale", rig.sigmaScale); rig.msSpread = stp.value("ms_spread", rig.msSpread);
            rig.ttGain = stp.value("tt_gain", rig.ttGain); rig.msSat = stp.value("ms_sat", rig.msSat); rig.msSatAmb = stp.value("ms_sat_amb", rig.msSatAmb); rig.envSpec = stp.value("env_spec", rig.envSpec); rig.meshEnvSpec = stp.value("mesh_env_spec", rig.meshEnvSpec);
            rig.skinSss = stp.value("skin_sss", rig.skinSss); rig.skinSpec = stp.value("skin_spec", rig.skinSpec);
            rig.skinGain = stp.value("skin_gain", rig.skinGain); rig.cavity = stp.value("cavity", rig.cavity);
            rig.aoStrength = stp.value("ao", rig.aoStrength); rig.quality = stp.value("quality", rig.quality); rig.toneMode = stp.value("tone", rig.toneMode); rig.hairEncoding = stp.value("hair_encoding", rig.hairEncoding);
            rig.sunSize = stp.value("sun_size", rig.sunSize); rig.shadowSoft = stp.value("shadow_soft", rig.shadowSoft);
            auto col3 = [&](const char* k, float* c) {
                if (stp.contains(k) && stp[k].is_array() && stp[k].size() == 3) for (int q = 0; q < 3; q++) c[q] = stp[k][q].get<float>();
            };
            col3("sun_col", rig.sunCol); col3("sky_col", rig.skyCol); col3("horizon_col", rig.horizonCol); col3("ground_col", rig.groundCol);
            hairDebug = stp.value("debug", hairDebug);
        } else if (d == "motion" || d == "settle") {
            motion.preset = d == "settle" ? MotionPreset::Rest : motionByName(stp.value("preset", "idle"));
            motion.speed = stp.value("speed", 1.0f);
            motion.amount = stp.value("amount", 1.0f);
            if (stp.value("reset", false)) teleport = true;
            motion.t = 0;
            runSim(*this, stp.value("seconds", 2.0));
            r["preset"] = motionName(motion.preset);
        } else if (d == "shots") {
            std::string tagS = stp.value("tag", configName);
            bool pair = stp.value("pair", true);
            int size = stp.value("size", 1400);
            json shots = json::array();
            for (auto& n : stp.value("shots", json::array({0, 2, 3, 4, 5, 15}))) {
                std::string k = std::to_string(n.get<int>());
                if (!pack.shots.contains(k)) { shots.push_back({{"shot", n}, {"error", "no camera"}}); continue; }
                auto& sh = pack.shots[k];
                std::string png = out + "\\" + tagS + "_shot" + k + ".png";
                bool ok = renderShot(sh, png, size);
                json e = {{"shot", n}, {"view", sh.value("view", "")}, {"png", png}, {"ok", ok}, {"hairs_live", st.hairs}};
                if (stp.value("mask", false)) {
                    // hair-only pass in flat red (SDK colorize mode) = exact hair mask for calibration / scoring
                    int cz = colorize; bool sm = showMeshes;
                    colorize = 4; maskPass = true;
                    step(0.0);
                    std::string mp = out + "\\" + tagS + "_shot" + k + "_mask.png";
                    if (renderShot(sh, mp, size)) e["mask"] = mp;
                    colorize = cz; showMeshes = sm; maskPass = false;
                    step(0.0);
                }
                if (pair && sh.contains("ref") && sh["ref"].is_string()) {
                    std::string ref = set.repo + "\\" + sh["ref"].get<std::string>();
                    std::string pp = out + "\\" + tagS + "_" + sh.value("view", k) + "_PAIR.png";
                    if (makePair(ref, png, pp)) e["pair"] = pp;
                }
                shots.push_back(e);
                logf("shot %s (%s): %s", k.c_str(), sh.value("view", "").c_str(), ok ? png.c_str() : "FAILED");
            }
            r["shots"] = shots;
        } else if (d == "bench") {
            // {"do": "bench", "w": 2560, "h": 1440, "frames": 240, "quality": 0..2, "view": "34"}: GPU-synced frame time of
            // the viewport scene (shadow + sky-occlusion passes, meshes, hair, simulation step; no UI, no present)
            int bw = stp.value("w", 2560), bh = stp.value("h", 1440), nf = stp.value("frames", 240);
            int q0 = rig.quality;
            if (stp.contains("quality")) rig.quality = stp["quality"].get<int>();
            if (stp.contains("view")) setView(stp["view"].get<std::string>());
            motion.preset = motionByName(stp.value("preset", "idle"));
            D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
            ID3D11Query* q = nullptr;
            gfx.dev->CreateQuery(&qd, &q);
            bool ok = gfx.beginOffscreen(bw, bh);
            XMStoreFloat4x4(&ovView, cam.view());
            XMStoreFloat4x4(&ovProj, XMMatrixPerspectiveFovRH(XMConvertToRadians(cam.fov), float(bw) / bh, 0.02f, 200.0f));
            ovFovDeg = cam.fov;
            camOverride = true;
            std::vector<double> ms;
            for (int f = 0; f < nf + 10 && ok; f++) {
                auto t0 = std::chrono::high_resolution_clock::now();
                step(1.0 / 60.0);
                drawScene();
                gfx.ctx->End(q);
                while (gfx.ctx->GetData(q, nullptr, 0, 0) == S_FALSE) {}
                auto t1 = std::chrono::high_resolution_clock::now();
                if (f >= 10) ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            if (ok) gfx.endOffscreen(out + "\\bench_q" + std::to_string(rig.quality) + ".png");
            camOverride = false;
            safeRelease(q);
            std::sort(ms.begin(), ms.end());
            double med = ms.empty() ? 0 : ms[ms.size() / 2], p95 = ms.empty() ? 0 : ms[std::min(ms.size() - 1, ms.size() * 95 / 100)];
            r["w"] = bw; r["h"] = bh; r["quality"] = rig.quality; r["ms_median"] = med; r["ms_p95"] = p95;
            r["fps_median"] = med > 0 ? 1000.0 / med : 0;
            logf("bench %dx%d quality %d: median %.2f ms (%.0f fps), p95 %.2f ms", bw, bh, rig.quality, med, med > 0 ? 1000.0 / med : 0, p95);
            rig.quality = q0;
        } else if (d == "save_project") {
            std::string p = stp.value("path", out + "\\" + configName + ".groomproj.json");
            saveProject(p);
            r["path"] = p;
        } else if (d == "export") {
            snprintf(tag, sizeof tag, "%s", stp.value("tag", std::string("agent1")).c_str());
            if (stp.contains("mod")) snprintf(modName, sizeof modName, "%s", stp["mod"].get<std::string>().c_str());
            std::string od;
            bool rel = stp.value("release", false);   // "release": true = Export for release (requires the Core)
            bool ok = exportAll(od, rel);
            r["ok"] = ok;
            r["dir"] = od;
            if (rel) { checkCore(true); r["core"] = {{"installed", core.installed}, {"version", core.version}, {"required", core.required}}; }
            if (!ok) { r["error"] = status; rc = 6; }
        } else if (d == "report") {
            // nothing: the budget is added to every report below
        } else {
            r["error"] = "unknown step";
            rc = 7;
        }
        computeBudget();
        r["budget"] = {{"hairs", bud.hairs}, {"hairs_close", bud.hairsClose}, {"ref", bud.refName}, {"ref_hairs_close", bud.refHairsClose},
                       {"ratio", bud.ratio}, {"over_limit", bud.overLimit}};
        logf("step %s: %s", d.c_str(), r.dump().c_str());
        report["steps"].push_back(r);
    }
    st = hw.stats();
    report["final"] = {{"config", configName}, {"guides", groom.numAlive()}, {"faces", groom.faces.size()}, {"ops", groom.ops.size()},
                       {"unmatched", groom.unmatched}, {"hairs_live", st.hairs},
                       {"budget", {{"hairs", bud.hairs}, {"hairs_close", bud.hairsClose}, {"ref", bud.refName}, {"ref_hairs", bud.refHairs},
                                   {"ref_hairs_close", bud.refHairsClose}, {"ratio", bud.ratio}, {"limit", look.limit}, {"over_limit", bud.overLimit},
                                   {"message", bud.msg}}},
                       {"material", presetSel < (int)presets.size() ? presets[presetSel].name : ""},
                       {"look_changes", look.exportLook()["set"].size()}, {"stiffness_map", smap.painted}};
    std::ofstream(out + "\\report.json") << report.dump(1);
    std::ofstream(out + "\\edits.json") << groom.editsText(true);
    logf("agent report: %s\\report.json  (budget %.0f %% of %s%s)", out.c_str(), bud.ratio * 100, bud.refName.c_str(), bud.overLimit ? ", OVER LIMIT" : "");
    return rc;
}
