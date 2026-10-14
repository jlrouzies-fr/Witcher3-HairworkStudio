// Desktop Groom Studio - headless agent runner (no UI): JSON script -> edits, material, stiffness, simulation, HairLab
// shot renders (+ PAIR with the vanilla still), budget report, export. Our own code (MIT). Format: README "Agent CLI".
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "app.h"
std::string applyPackHeadless(App& a, const std::string& dir);   // community.cpp
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
    for (auto& stp : S.value("steps", json::array())) report["steps"].push_back(agentStep(stp, out, rc));
    return finishAgent(report, out, rc);
}

// one script step (shared by the --agent runner and the --serve session); rc gets the exit code of a failed step
json App::agentStep(const json& stp, const std::string& out, int& rc) {
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
            r["cvs_per_turn"] = curlCvsPerTurn(groom.K, curl);
            if (curlCvsPerTurn(groom.K, curl) < 6) r["warning"] = "curl under-sampled (< 6 CVs per turn)";
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
            rig.ambRoot = stp.value("amb_root", rig.ambRoot); rig.ambVar = stp.value("amb_var", rig.ambVar); rig.ambTv = stp.value("amb_tv", rig.ambTv);
            rig.sunSize = stp.value("sun_size", rig.sunSize); rig.shadowSoft = stp.value("shadow_soft", rig.shadowSoft);
            auto col3 = [&](const char* k, float* c) {
                if (stp.contains(k) && stp[k].is_array() && stp[k].size() == 3) for (int q = 0; q < 3; q++) c[q] = stp[k][q].get<float>();
            };
            col3("sun_col", rig.sunCol); col3("sky_col", rig.skyCol); col3("horizon_col", rig.horizonCol); col3("ground_col", rig.groundCol);
            hairDebug = stp.value("debug", hairDebug);
            if (stp.contains("overlay")) { scalpOverlay = stp["overlay"].is_string() && stp["overlay"].get<std::string>() == "scalp"; r["overlay"] = scalpOverlay ? "scalp" : "none"; }
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
            bool ovPrev = scalpOverlay;   // "overlay": "scalp" renders these shots with the scalp highlight
            if (stp.contains("overlay")) scalpOverlay = stp["overlay"].is_string() && stp["overlay"].get<std::string>() == "scalp";
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
            scalpOverlay = ovPrev;
        } else if (d == "scalp_coverage") {
            // {"do": "scalp_coverage", "shot": 15 | "shots": [3, 15], "size": 1400, "tag": "x", "rect": [x0, y0, x1, y1]}: shot with the scalp
            // overlay, count pure-ish green pixels (g>200, r<60, b<60) (whole image and optional rect), save the overlay image + mask PNG
            std::string tagS = stp.value("tag", configName);
            int size = stp.value("size", 1400);
            json shotsR = json::array();
            std::vector<int> list;
            if (stp.contains("shots")) for (auto& n : stp["shots"]) list.push_back(n.get<int>());
            else list.push_back(stp.value("shot", 3));
            for (int n : list) {
                std::string k = std::to_string(n);
                if (!pack.shots.contains(k)) { shotsR.push_back({{"shot", n}, {"error", "no camera"}}); continue; }
                bool ov0 = scalpOverlay;
                scalpOverlay = true;
                std::string png = out + "\\" + tagS + "_shot" + k + "_scalp.png";
                bool ok = renderShot(pack.shots[k], png, size);
                scalpOverlay = ov0;
                json e = {{"shot", n}, {"view", pack.shots[k].value("view", "")}, {"png", png}, {"ok", ok}};
                int w = 0, h = 0;
                unsigned char* px = ok ? loadImage(png, &w, &h) : nullptr;
                if (px) {
                    std::vector<unsigned char> mk(size_t(w) * h * 4, 0);
                    long total = 0, inRect = 0;
                    int rx0 = 0, ry0 = 0, rx1 = w, ry1 = h;
                    bool hasRect = stp.contains("rect") && stp["rect"].is_array() && stp["rect"].size() == 4;
                    if (hasRect) { rx0 = stp["rect"][0]; ry0 = stp["rect"][1]; rx1 = stp["rect"][2]; ry1 = stp["rect"][3]; }
                    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                        const unsigned char* q = &px[(size_t(y) * w + x) * 4];
                        bool g = q[1] > 200 && q[0] < 60 && q[2] < 60;
                        unsigned char* m = &mk[(size_t(y) * w + x) * 4];
                        m[3] = 255;
                        if (g) { m[0] = m[1] = m[2] = 255; total++; if (x >= rx0 && x < rx1 && y >= ry0 && y < ry1) inRect++; }
                    }
                    freeImage(px);
                    std::string mp = out + "\\" + tagS + "_shot" + k + "_scalp_mask.png";
                    writePng(mp, w, h, mk.data());
                    e["scalp_pixels"] = total;
                    e["image"] = {w, h};
                    e["mask"] = mp;
                    if (hasRect) { e["rect"] = {rx0, ry0, rx1, ry1}; e["scalp_pixels_in_rect"] = inRect; }
                    logf("scalp_coverage shot %s: %ld px", k.c_str(), total);
                }
                shotsR.push_back(e);
            }
            r["shots"] = shotsR;
        } else if (d == "pick") {
            // {"do": "pick", "shot": 15, "x": px, "y": px | "auto": "scalp", ["rect": [x0, y0, x1, y1]], "size": 1400, "n": 6, "tag": "x"}: what is under a pixel of a
            // shot - hair or scalp, scalp world position, growth face, nearest guides. Writes <tag>_shot<k>_pick.png (overlay + crosshair).
            std::string tagS = stp.value("tag", configName);
            int size = stp.value("size", 1400), nN = stp.value("n", 6);
            std::string k = std::to_string(stp.value("shot", 3));
            auto isGreen = [](const unsigned char* q) { return q[1] > 200 && q[0] < 60 && q[2] < 60; };
            if (!pack.shots.contains(k)) { r["error"] = "no camera"; rc = 7; }
            else {
                auto& sh = pack.shots[k];
                float px0 = stp.value("x", -1.0f), py0 = stp.value("y", -1.0f);
                if (stp.contains("auto") || px0 < 0) {
                    // the green pixel nearest the centroid of the visible scalp
                    bool ov0 = scalpOverlay; scalpOverlay = true;
                    std::string tp = out + "\\_pick_tmp.png";
                    renderShot(sh, tp, size);
                    scalpOverlay = ov0;
                    int w, h;
                    unsigned char* im = loadImage(tp, &w, &h);
                    if (im) {
                        double sx = 0, sy = 0; long n = 0;
                        int ax0 = 0, ay0 = 0, ax1 = w, ay1 = h;   // optional "rect": search only this region (e.g. the visible patch)
                        if (stp.contains("rect") && stp["rect"].is_array() && stp["rect"].size() == 4) { ax0 = stp["rect"][0]; ay0 = stp["rect"][1]; ax1 = stp["rect"][2]; ay1 = stp["rect"][3]; }
                        auto inR = [&](int x, int y) { return x >= ax0 && x < ax1 && y >= ay0 && y < ay1; };
                        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (inR(x, y) && isGreen(&im[(size_t(y) * w + x) * 4])) { sx += x; sy += y; n++; }
                        if (n) {
                            double cx = sx / n, cy = sy / n, bd = 1e30;
                            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                                if (!inR(x, y) || !isGreen(&im[(size_t(y) * w + x) * 4])) continue;
                                double dd = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                                if (dd < bd) { bd = dd; px0 = float(x); py0 = float(y); }
                            }
                            r["auto_centroid"] = {cx, cy};
                        }
                        freeImage(im);
                    }
                    std::error_code ec; fs::remove(tp, ec);
                }
                r["shot"] = stp.value("shot", 3); r["pixel"] = {px0, py0}; r["image"] = {size, size};
                XMFLOAT3 eye{sh["eye"][0], sh["eye"][1], sh["eye"][2]}, tgt{sh["target"][0], sh["target"][1], sh["target"][2]};
                float fovy = sh.value("fovy", 11.16f);
                XMMATRIX V = XMMatrixLookAtRH(XMLoadFloat3(&eye), XMLoadFloat3(&tgt), XMVectorSet(0, 0, 1, 0));
                XMMATRIX P = XMMatrixPerspectiveFovRH(XMConvertToRadians(fovy), 1.0f, 0.05f, 100.0f);
                XMMATRIX IVP = XMMatrixInverse(nullptr, V * P);
                float nx = (px0 + 0.5f) / size * 2 - 1, ny = 1 - (py0 + 0.5f) / size * 2;
                XMVECTOR ra = XMVector3TransformCoord(XMVectorSet(nx, ny, 0, 1), IVP), rb = XMVector3TransformCoord(XMVectorSet(nx, ny, 1, 1), IVP);
                XMFLOAT3 o, dir;
                XMStoreFloat3(&o, ra);
                XMStoreFloat3(&dir, XMVector3Normalize(XMVectorSubtract(rb, ra)));
                // top layer: hair-only mask pass (red = hair)
                bool isHair = false;
                {
                    int cz = colorize; bool sm = showMeshes, ov0 = scalpOverlay;
                    colorize = 4; maskPass = true; scalpOverlay = false;
                    step(0.0);
                    std::string mp = out + "\\_pick_mask.png";
                    if (renderShot(sh, mp, size)) {
                        int w, h;
                        unsigned char* im = loadImage(mp, &w, &h);
                        if (im) {
                            int xi = std::clamp(int(px0), 0, w - 1), yi = std::clamp(int(py0), 0, h - 1);
                            const unsigned char* q = &im[(size_t(yi) * w + xi) * 4];
                            isHair = q[0] > 128 && q[1] < 100 && q[2] < 100;
                            freeImage(im);
                        }
                    }
                    colorize = cz; showMeshes = sm; maskPass = false; scalpOverlay = ov0;
                    step(0.0);
                    std::error_code ec; fs::remove(mp, ec);
                }
                XMFLOAT3 hit;
                int mi = -1;
                bool onSkin = skinRay(o, dir, hit, mi);
                r["top"] = isHair ? "hair" : onSkin ? "scalp" : "background_or_outfit";
                r["hair_over_skin"] = isHair && onSkin;
                if (onSkin) {
                    r["skin_mesh"] = pack.meshes[mi].name;
                    r["scalp_world"] = {hit.x, hit.y, hit.z};
                    auto dist = [](V3 a, V3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
                    V3 H{hit.x, hit.y, hit.z};
                    // growth face: smallest barycentric overshoot (inside = 0) of the hit against the faces' posed root triangles
                    int bestF = -1; double bestD = 1e30; std::array<float, 3> bestB{};
                    for (size_t f = 0; f < groom.faces.size(); f++) {
                        auto& F = groom.faces[f];
                        bool ok = true;
                        for (int c = 0; c < 3; c++) if (F.g[c] < 0 || F.g[c] >= (int)groom.P.size() || !groom.alive[F.g[c]]) ok = false;
                        if (!ok) continue;
                        V3 A = posedPoint(F.g[0], 0), B = posedPoint(F.g[1], 0), C = posedPoint(F.g[2], 0);
                        XMVECTOR a = XMVectorSet(A.x, A.y, A.z, 0), b = XMVectorSet(B.x, B.y, B.z, 0), c = XMVectorSet(C.x, C.y, C.z, 0), p = XMVectorSet(H.x, H.y, H.z, 0);
                        XMVECTOR e0 = b - a, e1 = c - a, e2 = p - a;
                        float d00 = XMVectorGetX(XMVector3Dot(e0, e0)), d01 = XMVectorGetX(XMVector3Dot(e0, e1)), d11 = XMVectorGetX(XMVector3Dot(e1, e1));
                        float d20 = XMVectorGetX(XMVector3Dot(e2, e0)), d21 = XMVectorGetX(XMVector3Dot(e2, e1));
                        float den = d00 * d11 - d01 * d01;
                        if (std::fabs(den) < 1e-14f) continue;
                        float v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den, u = 1 - v - w;
                        float outside = std::max({-u, -v, -w, 0.0f});
                        XMVECTOR pr = a + e0 * v + e1 * w;
                        double dd = outside * std::sqrt(std::max(d00, d11)) + 0.25 * dist({XMVectorGetX(pr), XMVectorGetY(pr), XMVectorGetZ(pr)}, H);
                        if (dd < bestD) { bestD = dd; bestF = int(f); bestB = {u, v, w}; }
                    }
                    if (bestF >= 0) {
                        auto& F = groom.faces[bestF];
                        r["face"] = {{"id", bestF}, {"guides", {F.g[0], F.g[1], F.g[2]}}, {"bary", {bestB[0], bestB[1], bestB[2]}},
                                     {"inside", bestB[0] >= 0 && bestB[1] >= 0 && bestB[2] >= 0}};
                    }
                    std::vector<std::pair<double, int>> dl;
                    for (size_t g = 0; g < groom.P.size(); g++) if (groom.alive[g]) dl.push_back({dist(posedPoint(int(g), 0), H), int(g)});
                    std::sort(dl.begin(), dl.end());
                    json gl = json::array();
                    for (int i = 0; i < nN && i < (int)dl.size(); i++) gl.push_back({{"guide", dl[i].second}, {"root_dist_mm", dl[i].first * 1000.0}, {"edited", groom.edited.count(dl[i].second) > 0}});
                    r["nearest_guides_by_root"] = gl;
                    // the same point in the edit frame (guide roots / where / lines of edit tools): shifted like the nearest root
                    if (!dl.empty()) {
                        int g0 = dl[0].second;
                        V3 sh = groom.P[g0][0] - posedPoint(g0, 0);
                        r["scalp_edit"] = {r4(H.x + sh.x), r4(H.y + sh.y), r4(H.z + sh.z)};
                    }
                }
                // guides whose posed strand passes nearest the pixel in screen space (what you see at the pixel)
                {
                    XMMATRIX VP = V * P;
                    auto scr = [&](V3 p, float& sx, float& sy) {
                        XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), VP);
                        float w = XMVectorGetW(c);
                        if (w <= 1e-5f) return false;
                        sx = (XMVectorGetX(c) / w * 0.5f + 0.5f) * size; sy = (1 - (XMVectorGetY(c) / w * 0.5f + 0.5f)) * size;
                        return true;
                    };
                    std::vector<std::pair<double, int>> sl;
                    for (size_t g = 0; g < groom.P.size(); g++) {
                        if (!groom.alive[g]) continue;
                        double bd = 1e30;
                        V3 bp{};
                        float ax = 0, ay = 0, bx = 0, by = 0;
                        bool have = scr(posedPoint(int(g), 0), ax, ay);
                        for (int kk = 1; kk < groom.K; kk++) {
                            bool h2 = scr(posedPoint(int(g), kk), bx, by);
                            if (have && h2) {
                                float vx = bx - ax, vy = by - ay, L2 = vx * vx + vy * vy;
                                float t = L2 > 1e-6f ? std::clamp(((px0 - ax) * vx + (py0 - ay) * vy) / L2, 0.0f, 1.0f) : 0.0f;
                                double dx = ax + t * vx - px0, dy = ay + t * vy - py0, dd = std::sqrt(dx * dx + dy * dy);
                                if (dd < bd) { bd = dd; V3 pa = posedPoint(int(g), kk - 1), pb = posedPoint(int(g), kk); bp = pa + (pb - pa) * t; }
                            }
                            ax = bx; ay = by; have = h2;
                        }
                        // depth: strands behind the skin under the pixel (back of the head) are not what you see there
                        if (onSkin) {
                            V3 E{eye.x, eye.y, eye.z}, Hs{hit.x, hit.y, hit.z};
                            if (len(bp - E) > len(Hs - E) + 0.01f) continue;
                        }
                        if (bd < 1e29) sl.push_back({bd, int(g)});
                    }
                    std::sort(sl.begin(), sl.end());
                    json gl = json::array();
                    for (int i = 0; i < nN && i < (int)sl.size(); i++) gl.push_back({{"guide", sl[i].second}, {"screen_px", sl[i].first}, {"edited", groom.edited.count(sl[i].second) > 0}});
                    r["nearest_guides_on_screen"] = gl;
                }
                // marked images: overlay shot and plain shot with a magenta crosshair
                for (int pass = 0; pass < 2; pass++) {
                    bool ov0 = scalpOverlay; scalpOverlay = pass == 0;
                    std::string mp = out + "\\" + tagS + "_shot" + k + (pass == 0 ? "_pick.png" : "_pick_plain.png");
                    if (renderShot(sh, mp, size)) {
                        int w, h;
                        unsigned char* im = loadImage(mp, &w, &h);
                        if (im) {
                            int cx = int(px0), cy = int(py0);
                            for (int q = -16; q <= 16; q++) {
                                if (std::abs(q) < 5) continue;
                                for (int th = -1; th <= 1; th++) {
                                    int xs[2] = {cx + q, cx + th}, ys[2] = {cy + th, cy + q};
                                    for (int a = 0; a < 2; a++) {
                                        if (xs[a] < 0 || ys[a] < 0 || xs[a] >= w || ys[a] >= h) continue;
                                        unsigned char* z = &im[(size_t(ys[a]) * w + xs[a]) * 4];
                                        z[0] = 255; z[1] = 0; z[2] = 255; z[3] = 255;
                                    }
                                }
                            }
                            writePng(mp, w, h, im);
                            freeImage(im);
                            r[pass == 0 ? "png" : "png_plain"] = mp;
                        }
                    }
                    scalpOverlay = ov0;
                }
            }
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
        } else if (d == "apply_pack") {
            // {"do": "apply_pack", "dir": "<pack folder>"}: apply a community pack onto the loaded project (like Community > Apply)
            r["message"] = applyPackHeadless(*this, stp.value("dir", ""));
            r["unmatched"] = groom.unmatched;
        } else if (d == "dump_geo") {
            // {"do": "dump_geo", "path": "x.json"}: the groom as it is now (alive guides in order, faces by alive index, per-corner uv)
            std::vector<int> ix(groom.P.size(), -1);
            // "index" (2026-10-14): each dumped guide's Studio index (groom slot, what every other command takes); dump
            // order = alive guides only, so it drifts from the Studio index after any delete
            json G = json::array(), Fa = json::array(), Ix = json::array();
            for (size_t i = 0; i < groom.P.size(); i++) {
                if (!groom.alive[i]) continue;
                ix[i] = (int)G.size();
                Ix.push_back((int)i);
                json pts = json::array();
                for (auto& q : groom.P[i]) pts.push_back({r4(q.x), r4(q.y), r4(q.z)});
                G.push_back(pts);
            }
            for (auto& f : groom.faces) Fa.push_back({{"g", {ix[f.g[0]], ix[f.g[1]], ix[f.g[2]]}}, {"uv", {f.uv[0], f.uv[1], f.uv[2], f.uv[3], f.uv[4], f.uv[5]}}});
            std::ofstream(stp.value("path", out + "\\geo.json")) << json{{"K", groom.K}, {"guides", G}, {"index", Ix}, {"faces", Fa}}.dump();
            r["guides"] = G.size();
            r["faces"] = Fa.size();
        } else if (d == "fur") {
            // {"do": "fur", "fur": "beard" | "hair"}: Beard mode switch (builds with a beard fur): the beard fur becomes the
            // live groom (every step / command acts on it), the hair the preview; "hair" switches back
            std::string f = stp.value("fur", std::string("hair"));
            if (f != "hair" && f != "beard") { r["error"] = "fur must be hair or beard"; rc = 7; }
            else if (!setFur(f == "beard" ? 1 : 0)) r["error"] = beardOn ? "cannot switch while the wind check runs" : "this build has no beard fur";
            r["fur"] = fur ? "beard" : "hair";
            r["has_beard"] = beardOn;
            r["comp"] = pack.comp;
            r["guides"] = groom.numAlive();
            r["faces"] = groom.faces.size();
            r["ops"] = groom.ops.size();
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
        return r;
}

int App::finishAgent(json& report, const std::string& out, int rc) {
    st = hw.stats();
    report["final"] = {{"config", configName}, {"fur", fur ? "beard" : "hair"}, {"guides", groom.numAlive()}, {"faces", groom.faces.size()}, {"ops", groom.ops.size()},
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

// Scalp of the head mesh = where the head diffuse shows the painted short scalp hair (dark vs skin, 9 px averaged),
// on the mesh graph (vertices welded at UV seams) connected to the top of the head and above the head centre (cuts
// beard stubble, brows, eye sockets). Not from guide roots: those can sit inside the forehead.
void App::computeHeadScalp() {
    headScalp.clear();
    for (size_t mi = 0; mi < pack.meshes.size(); mi++) {
        const Mesh& m = pack.meshes[mi];
        if (m.kind != "head" || m.texFile.empty() || m.uv.size() != m.v.size()) continue;
        int w, h;
        unsigned char* im = loadImage(pack.dir + "\\" + m.texFile, &w, &h);
        if (!im) continue;
        auto lumAt = [&](float u, float v) {
            int cx = std::clamp(int(u * w), 0, w - 1), cy = std::clamp(int(v * h), 0, h - 1);
            double s = 0; int n = 0;
            for (int dy = -4; dy <= 4; dy++) for (int dx = -4; dx <= 4; dx++) {
                int x = std::clamp(cx + dx, 0, w - 1), y = std::clamp(cy + dy, 0, h - 1);
                const unsigned char* q = &im[(size_t(y) * w + x) * 4];
                s += 0.3 * q[0] + 0.59 * q[1] + 0.11 * q[2]; n++;
            }
            return s / n;
        };
        size_t nv = m.v.size();
        std::vector<char> dark(nv);
        // threshold relative to this character's skin (cheeks / nose below the eyes): dark = clearly darker than skin;
        // eyebrows / forehead band excluded in front of the head below brow height (eye meshes give the eye height)
        std::vector<double> skinL;
        for (size_t i = 0; i < nv; i++)
            if (m.v[i].y > pack.headC.y + 0.06f && m.v[i].z > pack.headC.z - 0.06f && m.v[i].z < pack.headC.z - 0.01f)
                skinL.push_back(lumAt(m.uv[i][0], m.uv[i][1]));
        double skin = 140;
        if (!skinL.empty()) { std::nth_element(skinL.begin(), skinL.begin() + skinL.size() / 2, skinL.end()); skin = skinL[skinL.size() / 2]; }
        double thr = std::min(95.0, 0.62 * skin);
        float eyeTop = -1e9f;
        for (auto& em : pack.meshes)
            if (em.texFile.find("eye") != std::string::npos) for (auto& v : em.v) eyeTop = std::max(eyeTop, v.z);
        float browZ = eyeTop > -1e8f ? eyeTop + 0.035f : pack.headC.z + 0.05f;
        for (size_t i = 0; i < nv; i++) {
            bool frontLow = m.v[i].y > pack.headC.y + 0.05f && m.v[i].z < browZ;
            dark[i] = !frontLow && lumAt(m.uv[i][0], m.uv[i][1]) < thr;
        }
        float thrLin = float(std::pow(thr / 255.0, 2.2));
        logf("scalp mask: skin luminance %.0f, threshold %.0f, brow height %.3f", skin, thr, browZ);
        freeImage(im);
        // weld by position, adjacency over welded ids
        std::map<std::array<long, 3>, int> wid;
        std::vector<int> id(nv);
        for (size_t i = 0; i < nv; i++) {
            std::array<long, 3> k{lround(m.v[i].x * 1e4), lround(m.v[i].y * 1e4), lround(m.v[i].z * 1e4)};
            auto it = wid.find(k);
            id[i] = it == wid.end() ? (wid[k] = int(wid.size())) : it->second;
        }
        int nw = int(wid.size());
        std::vector<char> wdark(nw, 0);
        std::vector<float> wz(nw, 0);
        for (size_t i = 0; i < nv; i++) { if (dark[i]) wdark[id[i]] = 1; wz[id[i]] = m.v[i].z; }
        std::vector<std::vector<int>> adj(nw);
        for (auto& t : m.t) for (int c = 0; c < 3; c++) { adj[id[t[c]]].push_back(id[t[(c + 1) % 3]]); adj[id[t[(c + 1) % 3]]].push_back(id[t[c]]); }
        float zTop = -1e9f;
        for (int i = 0; i < nw; i++) zTop = std::max(zTop, wz[i]);
        float zMin = pack.headC.z;
        std::vector<char> in(nw, 0);
        std::vector<int> q;
        for (int i = 0; i < nw; i++) if (wdark[i] && wz[i] > zTop - 0.02f) { in[i] = 1; q.push_back(i); }
        while (!q.empty()) {
            int a = q.back(); q.pop_back();
            for (int b : adj[a]) if (!in[b] && wdark[b] && wz[b] > zMin) { in[b] = 1; q.push_back(b); }
        }
        // no painted scalp on this head texture (e.g. Eredin): fall back to the vanilla hair-card footprint = head
        // vertices within 12 mm of a card vertex, above the head centre - 2 cm (where the game puts hair)
        int painted = 0;
        for (int i = 0; i < nw; i++) painted += in[i];
        bool fromCards = false;
        if (painted < 40) {
            std::vector<V3> cardV;
            for (auto& cm : pack.meshes) if (cm.kind == "hair_cards") for (auto& v : cm.v) cardV.push_back(v);
            if (!cardV.empty()) {
                std::fill(in.begin(), in.end(), 0);
                for (size_t i = 0; i < nv; i++) {
                    if (m.v[i].z < pack.headC.z - 0.02f) continue;
                    bool frontLow = m.v[i].y > pack.headC.y + 0.05f && m.v[i].z < browZ;
                    if (frontLow) continue;
                    for (auto& cv : cardV) if (len(cv - m.v[i]) < 0.012f) { in[id[i]] = 1; break; }
                }
                fromCards = true;
            }
        }
        std::vector<float> f(nv);
        int cnt = 0;
        // flag = 1 + per-pixel threshold (linear) for the shader, 0 = not scalp
        for (size_t i = 0; i < nv; i++) { f[i] = in[id[i]] ? 1.0f + thrLin : 0.0f; cnt += in[id[i]]; }
        headScalp[int(mi)] = f;
        logf("scalp mask: %s %d / %zu vertices%s", m.name.c_str(), cnt, nv, fromCards ? " (from the vanilla hair-card footprint: no painted scalp)" : "");
        if (fromCards) for (auto& x : f) if (x > 0.5f) x = 1.0f + 1.0f;   // card footprint: whole area counts (no texel test)
    }
}
