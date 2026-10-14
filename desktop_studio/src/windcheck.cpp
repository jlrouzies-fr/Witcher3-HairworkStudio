// Hairwork Studio - Check > Max-wind strays: a measured, repeatable wind check (serve "wind_check", MCP studio_wind_check).
// Our own code (Studio License); uses only the public HairWorks API through HairRuntime (no SDK code here).
//
// 1. rest: simulation off, exact hair-only mask pass (colorize red, meshes black) per camera -> rest hair mask, its
//    Euclidean distance transform (px from the rest silhouette) and a CPU z-buffer of the posed meshes (occlusion).
// 2. wind run: a fresh HairWorks instance (rebuild), teleport, motion.t = 0, fixed 1/60 s steps, wind = amount (the
//    toolbar maximum is 5). After every step the simulated guide CVs are read back from the GPU and projected into every
//    camera (visible points only): per guide the farthest distance from the rest silhouette. At `frames` evenly spaced
//    steps the mask pass is rendered: per camera / pixel the farthest distance reached by hair.
// 3. the same run again: run-to-run difference of the read-back CVs and of the flagged pixels (repeatability), and a
//    calm run (no wind, gravity only) as reference.
// 4. clusters of flagged pixels (> threshold), attributed per sample frame to the guides whose SIMULATED polyline passes
//    there, else to the growth faces whose interpolated hair (barycentric between the 3 simulated guides) covers them.
// 5. confirmation by elimination: the candidates are hidden (left out of the asset) and the same wind re-run; groups
//    that remove a cluster's pixels are bisected down to single guides / faces. A culprit is a guide or face whose hiding
//    removes a significant part of its cluster ("confirmed" = most of it, >= 50 %).
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <set>

#include "app.h"

namespace fs = std::filesystem;
using namespace DirectX;

bool writePng(const std::string& path, int w, int h, const unsigned char* rgba);

namespace {

struct WindCancelled {};

struct WCam {
    std::string name;
    XMFLOAT3 eye, tgt;
    float fovy = 40;
    XMFLOAT4X4 view, proj, vp;
    std::vector<float> zbuf;          // view depth of the nearest mesh per pixel (1e9 = none)
    std::vector<uint8_t> rest;        // rest hair mask
    std::vector<float> dt;            // px to the nearest rest hair pixel (0 inside)
    float tdist = 0.5f;               // eye -> target (m)
};

struct Proj { float x, y, w; };

static inline Proj wproj(const WCam& c, float S, V3 p) {
    XMVECTOR v = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), XMLoadFloat4x4(&c.vp));
    float w = XMVectorGetW(v);
    if (w <= 1e-4f) return {-1e9f, -1e9f, w};
    return {(XMVectorGetX(v) / w * 0.5f + 0.5f) * S, (1.0f - (XMVectorGetY(v) / w * 0.5f + 0.5f)) * S, w};
}

// exact Euclidean distance transform (Felzenszwalb & Huttenlocher: squared distance, two 1D passes)
static void edt1(const double* f, double* d, int n, std::vector<int>& v, std::vector<double>& z) {
    int k = 0;
    v[0] = 0; z[0] = -1e30; z[1] = 1e30;
    auto sect = [&](int q, int p) { return ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * q - 2.0 * p); };
    for (int q = 1; q < n; q++) {
        double s = sect(q, v[k]);
        while (s <= z[k]) { k--; s = sect(q, v[k]); }
        k++;
        v[k] = q; z[k] = s; z[k + 1] = 1e30;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < q) k++;
        double dq = double(q - v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}

static std::vector<float> distanceTransform(const std::vector<uint8_t>& in, int S) {
    const double INF = 1e20;
    std::vector<double> g(size_t(S) * S), col(S), dcol(S), row(S);
    std::vector<float> res(size_t(S) * S);
    std::vector<int> v(S + 1);
    std::vector<double> z(S + 2);
    for (size_t i = 0; i < g.size(); i++) g[i] = in[i] ? 0.0 : INF;
    for (int x = 0; x < S; x++) {
        for (int y = 0; y < S; y++) col[y] = g[size_t(y) * S + x];
        edt1(col.data(), dcol.data(), S, v, z);
        for (int y = 0; y < S; y++) g[size_t(y) * S + x] = dcol[y];
    }
    for (int y = 0; y < S; y++) {
        edt1(&g[size_t(y) * S], row.data(), S, v, z);
        for (int x = 0; x < S; x++) res[size_t(y) * S + x] = float(std::sqrt(std::max(0.0, std::min(row[x], 1e12))));
    }
    return res;
}

static void zbufferMeshes(WCam& c, int S, const std::vector<GVertex>& tris) {
    c.zbuf.assign(size_t(S) * S, 1e9f);
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        Proj p[3];
        bool ok = true;
        for (int q = 0; q < 3; q++) {
            const XMFLOAT3& P = tris[i + q].pos;
            p[q] = wproj(c, float(S), V3{P.x, P.y, P.z});
            if (p[q].w < 0.06f) ok = false;
        }
        if (!ok) continue;
        float x0 = std::min({p[0].x, p[1].x, p[2].x}), x1 = std::max({p[0].x, p[1].x, p[2].x});
        float y0 = std::min({p[0].y, p[1].y, p[2].y}), y1 = std::max({p[0].y, p[1].y, p[2].y});
        int ix0 = std::max(0, int(std::floor(x0))), ix1 = std::min(S - 1, int(std::ceil(x1)));
        int iy0 = std::max(0, int(std::floor(y0))), iy1 = std::min(S - 1, int(std::ceil(y1)));
        if (ix0 > ix1 || iy0 > iy1) continue;
        float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
        if (std::fabs(area) < 1e-9f) continue;
        float ia = 1.0f / area;
        for (int y = iy0; y <= iy1; y++)
            for (int x = ix0; x <= ix1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float l0 = ((p[1].x - px) * (p[2].y - py) - (p[2].x - px) * (p[1].y - py)) * ia;
                float l1 = ((p[2].x - px) * (p[0].y - py) - (p[0].x - px) * (p[2].y - py)) * ia;
                float l2 = 1.0f - l0 - l1;
                if (l0 < 0 || l1 < 0 || l2 < 0) continue;
                float iw = l0 / p[0].w + l1 / p[1].w + l2 / p[2].w;
                float w = 1.0f / iw;
                float& zb = c.zbuf[size_t(y) * S + x];
                if (w < zb) zb = w;
            }
    }
}

static inline bool hairPx(const unsigned char* px) { return px[0] >= 128 && px[0] > px[1] + 80 && px[0] > px[2] + 80; }

// one wind (or calm) run: what was measured
struct RunOut {
    bool readOk = false;
    std::vector<std::vector<float>> pixMax;          // [cam][pixel] farthest distance from the rest silhouette reached by hair (0 = never outside)
    std::vector<std::vector<int>> frameCount;        // [cam][frame] pixels > threshold in that frame
    std::vector<std::vector<std::vector<int>>> framePix;   // [frame][cam] pixels > threshold (attribution runs only)
    std::vector<std::vector<V3>> frameGuides;        // [frame] groom guide g * PG + cv (simulated, NaN when hidden)
    std::vector<std::vector<float>> gPx;             // [cam][guide] farthest visible CV distance from the rest silhouette (px)
    std::vector<float> gDisp;                        // [guide] farthest 3D CV displacement from rest (m)
    std::vector<float> gRise;                        // [guide] highest lift of a CV above its own rest height (m)
    std::vector<float> gReach;                       // [guide] farthest CV from its own (current) root (m): > length = stretched
    std::vector<float> gBestPx, gBestW;              // [guide] max over cameras of gPx, view depth of that CV
    std::vector<int> gBestCam, gBestStep;
    std::vector<std::vector<V3>> gWorst;             // [guide] simulated CVs at its worst step
    std::vector<float> trace;                        // all read-back CVs, all steps (repeatability reference run)
    double maxDiff = 0, sumDiff = 0;                 // vs reference trace (m)
    std::vector<float> stepDiff;                     // max CV difference per step vs the reference trace (m)
    std::vector<float> guideDiff;                    // max CV difference per guide (groom index) vs the reference (m)
    long long nDiff = 0;
    float rootErr = 0;                               // first step: farthest read-back root from its rest root (m): layout check
};

}   // namespace

// ================================================================================================ fiber plumbing (UI)
static void WINAPI windFiberProc(void* p) {
    App& a = *(App*)p;
    try {
        a.windResult = a.windCheck(a.windParams, a.windOut);
    } catch (WindCancelled&) {
        a.windResult = {{"error", "cancelled"}};
    } catch (std::exception& e) {
        a.windResult = {{"error", e.what()}};
    }
    a.windBusy = false;
    for (;;) SwitchToFiber(a.mainFiber);
}

void App::windStart(const json& params) {
    if (windBusy || !loaded || !sdkOk) return;
    if (!mainFiber) mainFiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
    if (!mainFiber) { status = "wind check: cannot start (fiber)"; return; }
    windParams = params;
    windOut = set.repo + "\\build\\desktop_studio\\windcheck";
    windCancel = false;
    windProgress = 0;
    windPhase = "starting";
    windFiber = CreateFiberEx(1 << 20, 16 << 20, 0, windFiberProc, this);
    if (!windFiber) { status = "wind check: cannot start (fiber)"; return; }
    windBusy = true;
}

void App::windResume() {
    if (!windFiber) { windBusy = false; return; }
    windInside = true;
    SwitchToFiber(windFiber);
    windInside = false;
    if (!windBusy) {
        DeleteFiber(windFiber);
        windFiber = nullptr;
        if (windResult.contains("error")) status = "Max-wind strays: " + windResult["error"].get<std::string>();
        else status = "Max-wind strays: " + std::to_string(windRows.size()) + " culprit(s)";
    }
}

void App::windYield(float progress, const char* phase) {
    windProgress = progress;
    windPhase = phase;
    if (!windFiber || !windInside) return;   // serve / agent: blocking run
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (windCancel) throw WindCancelled();
    if (std::chrono::duration<double>(now - last).count() < 0.03) return;
    SwitchToFiber(mainFiber);
    last = std::chrono::steady_clock::now();
    if (windCancel) throw WindCancelled();
}

void App::windDeleteCulprits() {
    if (windGuides.empty() && windFaces.empty()) return;
    int id = 0, grp = 1000 + (++grpId);
    for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    std::vector<json> ops;
    for (int f : windFaces) {
        if (f < 0 || f >= (int)groom.faces.size()) continue;
        bool skip = false;
        for (int q = 0; q < 3; q++) if (windGuides.count(groom.faces[f].g[q])) skip = true;   // goes with the guide
        if (skip) continue;
        json rr = json::array();
        for (int q = 0; q < 3; q++) { V3 p = groom.P[groom.faces[f].g[q]][0]; rr.push_back({r4(p.x), r4(p.y), r4(p.z)}); }
        ops.push_back({{"op", "delete_face"}, {"roots", rr}, {"id", ++id}, {"grp", grp}});
    }
    std::vector<int> gl;
    for (int g : windGuides) if (g >= 0 && g < (int)groom.P.size() && groom.alive[g]) gl.push_back(g);
    if (!gl.empty()) ops.push_back({{"type", "delete"}, {"roots", groom.rootsOf(gl)}, {"id", ++id}, {"grp", grp}});
    for (auto& o : ops) groom.ops.push_back(o);
    groom.redo.clear();
    groom.replay();
    editsChanged();
    sel.clear();
    status = std::to_string(gl.size()) + " culprit guide(s) and " + std::to_string(ops.size() - (gl.empty() ? 0 : 1)) + " face(s) deleted (one undo step)";
    windGuides.clear();
    windFaces.clear();
    windRows.clear();
}

// ================================================================================================ the check
json App::windCheck(const json& P, const std::string& out) {
    auto T0 = std::chrono::steady_clock::now();
    const int S = std::clamp(P.value("size", 600), 128, 2048);
    const float scale = S / 600.0f;
    const float thr = P.value("threshold_px", 12.0f) * scale;
    const double seconds = std::clamp(P.value("seconds", 6.0), 0.5, 60.0);
    const int nSteps = std::max(1, int(seconds * 60.0 + 0.5));
    const int nFrames = std::clamp(P.value("frames", 30), 1, nSteps);
    const float amount = P.value("amount", 5.0f);
    const double ramp = std::clamp(P.value("ramp_s", 0.0), 0.0, 10.0);   // wind strength ramps 0 -> amount over this time
    const bool confirm = P.value("confirm", true), doCalm = P.value("calm", true);
    const int reps = P.value("repeat", true) ? std::clamp(P.value("repeats", 6), 1, 16) : 1;   // baseline runs (wind is random)
    const int maxRuns = P.value("max_runs", 64);   // elimination tests (each = half as many runs as the baseline)
    const float strayMin = P.value("stray_px", 24.0f) * scale;   // candidate: this much farther out than its neighbours
    const int maxCand = std::clamp(P.value("max_candidates", 120), 1, 400);
    const float minFrac = P.value("min_removal", 0.15f);
    const int minCluster = std::max(4, int(P.value("min_cluster_px", 40.0f) * scale * scale));
    const bool pngs = P.value("png", true);
    fs::create_directories(out);

    // ---- cameras around the head (top, front-top, back, both sides, front) or the caller's
    std::vector<WCam> cams;
    V3 t{pack.headC.x, pack.headC.y, pack.headTop - 0.07f};
    struct Pre { const char* n; float dx, dy, dz; };
    const Pre pre[] = {{"top", 0.0f, 0.14f, 0.53f}, {"front_top", 0.0f, 0.45f, 0.30f}, {"back", 0.0f, -0.55f, 0.10f},
                       {"left", 0.55f, 0.0f, 0.06f}, {"right", -0.55f, 0.0f, 0.06f}, {"front", 0.0f, 0.55f, 0.03f}};
    auto addPre = [&](const Pre& p) { WCam c; c.name = p.n; c.eye = {t.x + p.dx, t.y + p.dy, t.z + p.dz}; c.tgt = {t.x, t.y, t.z}; c.fovy = 40; cams.push_back(c); };
    json camsP = P.value("cameras", json(6));
    if (camsP.is_number()) { for (int i = 0; i < std::clamp(camsP.get<int>(), 1, 6); i++) addPre(pre[i]); }
    else if (camsP.is_array()) {
        for (auto& e : camsP) {
            if (e.is_string()) { for (auto& p : pre) if (e.get<std::string>() == p.n) addPre(p); }
            else if (e.is_object() && e.contains("eye") && e.contains("target")) {
                WCam c; c.name = e.value("name", "cam" + std::to_string(cams.size()));
                c.eye = {e["eye"][0], e["eye"][1], e["eye"][2]}; c.tgt = {e["target"][0], e["target"][1], e["target"][2]}; c.fovy = e.value("fovy", 40.0f);
                cams.push_back(c);
            }
        }
    }
    if (cams.empty()) for (auto& p : pre) addPre(p);
    const int C = (int)cams.size();
    for (auto& c : cams) {
        XMMATRIX V = XMMatrixLookAtRH(XMLoadFloat3(&c.eye), XMLoadFloat3(&c.tgt), XMVectorSet(0, 0, 1, 0));
        XMMATRIX Pm = XMMatrixPerspectiveFovRH(XMConvertToRadians(c.fovy), 1.0f, 0.05f, 100.0f);
        XMStoreFloat4x4(&c.view, V); XMStoreFloat4x4(&c.proj, Pm); XMStoreFloat4x4(&c.vp, V * Pm);
        V3 d{c.eye.x - c.tgt.x, c.eye.y - c.tgt.y, c.eye.z - c.tgt.z};
        c.tdist = len(d);
    }
    auto mmPerPx = [&](const WCam& c, float w) { return 1000.0f * 2.0f * w * std::tan(XMConvertToRadians(c.fovy) * 0.5f) / S; };

    // ---- state we change (restored on exit, also when cancelled)
    struct Saved {
        App& a;
        bool simulate, paused, windOn, maskPass, showMeshes, showHair, vanillaHair, scalpOverlay, showGuideOverlay, sdkGuides, showCollision, showGrowth, shadows, camOverride, stepOnce, turntable;
        int colorize;
        Motion motion;
        double simAcc;
        std::map<std::string, json> lookOver;   // look overrides (wind_noise override)
        XMFLOAT4X4 ovView, ovProj;
        float ovFov;
        explicit Saved(App& x) : a(x) {
            simulate = a.simulate; paused = a.paused; windOn = a.windOn; maskPass = a.maskPass; showMeshes = a.showMeshes; showHair = a.showHair;
            vanillaHair = a.vanillaHair; scalpOverlay = a.scalpOverlay; showGuideOverlay = a.showGuideOverlay; sdkGuides = a.sdkGuides;
            showCollision = a.showCollision; showGrowth = a.showGrowth; shadows = a.rig.shadows; camOverride = a.camOverride; stepOnce = a.stepOnce;
            turntable = a.turntable; colorize = a.colorize; motion = a.motion; simAcc = a.simAcc; ovView = a.ovView; ovProj = a.ovProj; ovFov = a.ovFovDeg;
            lookOver = a.look.over;
        }
        ~Saved() {
            a.hideGuide.clear(); a.hideFace.clear();
            a.look.over = lookOver;
            a.simulate = simulate; a.paused = paused; a.windOn = windOn; a.maskPass = maskPass; a.showMeshes = showMeshes; a.showHair = showHair;
            a.vanillaHair = vanillaHair; a.scalpOverlay = scalpOverlay; a.showGuideOverlay = showGuideOverlay; a.sdkGuides = sdkGuides;
            a.showCollision = showCollision; a.showGrowth = showGrowth; a.rig.shadows = shadows; a.camOverride = camOverride; a.stepOnce = stepOnce;
            a.turntable = turntable; a.colorize = colorize; a.ovView = ovView; a.ovProj = ovProj; a.ovFovDeg = ovFov;
            a.rebuildHair();   // a fresh instance of the groom as it is (no hidden guides)
            // settle like the serve commands do (rest motion, teleport), then the caller's motion / clock back
            a.motion = motion;
            a.motion.preset = MotionPreset::Rest;
            a.windOn = false;
            a.teleport = true;
            bool in = a.windInside;
            a.windInside = true;
            for (int i = 0; i < 30; i++) a.step(1.0 / 60.0);
            a.windInside = in;
            a.motion = motion;
            a.windOn = windOn;
            a.simAcc = simAcc;
            a.aoRefreshAll = true;
        }
    } saved(*this);

    // wind noise: the fur's own value by default (what the game does). The runtime's wind noise is random per run
    // (measured 2026-10-14 on Ciri, windNoise 2: two identical runs already differ by 30 mm at step 2, by 0.07 mm
    // with no wind), so "wind_noise": 0 gives a deterministic run; the default keeps the noise and repeats instead.
    const char* kNoise = "physicalMaterials.simulation.windNoise";
    const float furNoise = look.f(kNoise, 0.0f);
    if (P.contains("wind_noise") && P["wind_noise"].is_number()) look.setF(kNoise, P["wind_noise"].get<float>());
    const float windNoise = look.f(kNoise, 0.0f);

    // hair-only mask pass: flat red hair, black meshes, nothing else drawn into it
    colorize = 4; maskPass = true; showMeshes = true; showHair = true; vanillaHair = false; scalpOverlay = false;
    showGuideOverlay = false; sdkGuides = false; showCollision = false; showGrowth = false; rig.shadows = false; turntable = false; stepOnce = false;
    const int K = groom.K;
    const size_t NG = groom.P.size(), NF = groom.faces.size();

    auto renderMask = [&](const WCam& c, std::vector<unsigned char>& rgba) {
        ovView = c.view; ovProj = c.proj; ovFovDeg = c.fovy;
        camOverride = true;
        aoRefreshAll = true;
        bool ok = gfx.beginOffscreen(S);
        if (ok) { drawScene(); ok = gfx.endOffscreen("", &rgba); }
        camOverride = false;
        return ok && rgba.size() == size_t(S) * S * 4;
    };

    // ---- rest: sim off
    json readInfo;
    int PG = K;
    std::vector<V3> restCV(NG * size_t(K));   // rest CVs (groom index, PG per guide once known)
    hideGuide.clear(); hideFace.clear();
    rebuildHair();
    simulate = false; paused = false; windOn = false;
    motion.preset = MotionPreset::Rest; motion.speed = 1; motion.amount = 1; motion.t = 0; simAcc = 0; teleport = true;
    for (int i = 0; i < 3; i++) step(1.0 / 60.0);
    std::vector<float> xyz;
    std::string info;
    bool readOk = hw.readGuides(xyz, PG, &info);
    readInfo["info"] = info;
    readInfo["ok"] = readOk;
    if (readOk) {
        restCV.assign(NG * size_t(PG), V3{});
        double err = 0;
        for (size_t a = 0; a < guideOfAsset.size(); a++) {
            int g = guideOfAsset[a];
            for (int j = 0; j < PG; j++) restCV[size_t(g) * PG + j] = {xyz[(a * PG + j) * 3], xyz[(a * PG + j) * 3 + 1], xyz[(a * PG + j) * 3 + 2]};
            // the refined strand passes through the guide's CVs (every spl-th point)
            int spl = K > 1 ? (PG - 1) / (K - 1) : 1;
            for (int k = 0; k < K; k++) err = std::max(err, double(len(restCV[size_t(g) * PG + size_t(k) * spl] - posedPoint(g, k))));
        }
        readInfo["cvs_per_guide"] = PG;
        if (getenv("HWS_WIND_DEBUG")) {
            json d0 = json::array(), g0 = json::array(), g1 = json::array();
            for (int j = 0; j < PG; j++) d0.push_back({xyz[j * 3], xyz[j * 3 + 1], xyz[j * 3 + 2]});
            for (int k = 0; k < K; k++) { V3 q = posedPoint(guideOfAsset[0], k); g0.push_back({q.x, q.y, q.z}); }
            for (int j = 0; j < PG; j++) g1.push_back({xyz[(PG + j) * 3], xyz[(PG + j) * 3 + 1], xyz[(PG + j) * 3 + 2]});
            readInfo["debug_asset0"] = d0; readInfo["debug_groom0"] = g0; readInfo["debug_asset1"] = g1; readInfo["K"] = K;
            readInfo["splineMultiplier"] = pack.splineMultiplier;
        }
        readInfo["rest_vs_groom_max_mm"] = std::round(err * 1e4) / 10;
        if (err > 0.002) { readOk = false; readInfo["ok"] = false; readInfo["why"] = "read-back rest CVs differ from the groom (layout not understood)"; }
    }
    if (!readOk) {   // no read-back: fall back to the CPU rest guides (no per-guide measurement, image attribution only)
        PG = K;
        restCV.assign(NG * size_t(K), V3{});
        for (size_t g = 0; g < NG; g++) if (groom.alive[g]) for (int k = 0; k < K; k++) restCV[g * K + k] = posedPoint(int(g), k);
    }
    std::vector<GVertex> meshT;
    posedMeshTris(meshT);
    std::vector<unsigned char> rgba;
    json restCams = json::array();
    for (int c = 0; c < C; c++) {
        windYield(0.01f * c / C, "rest masks");
        if (!renderMask(cams[c], rgba)) throw std::runtime_error("offscreen render failed");
        cams[c].rest.assign(size_t(S) * S, 0);
        int n = 0;
        for (size_t i = 0; i < cams[c].rest.size(); i++) if (hairPx(&rgba[i * 4])) { cams[c].rest[i] = 1; n++; }
        cams[c].dt = distanceTransform(cams[c].rest, S);
        zbufferMeshes(cams[c], S, meshT);
        restCams.push_back({{"camera", cams[c].name}, {"rest_hair_px", n}});
    }

    // ---- one run (fresh instance, identical step sequence)
    std::vector<int> sampleStep(nFrames);
    for (int j = 0; j < nFrames; j++) sampleStep[j] = std::clamp(int(std::lround(double(j + 1) * nSteps / nFrames)) - 1, 0, nSteps - 1);
    int runs = 0;
    float progLo = 0.02f, progHi = 0.3f;
    auto run = [&](float wind, const std::set<int>& hg, const std::set<int>& hf, const std::vector<char>& camOn, bool keepFrames,
                   bool keepTrace, const std::vector<float>* ref, const char* phase) {
        RunOut R;
        runs++;
        hideGuide.assign(NG, 0); hideFace.assign(NF, 0);
        for (int g : hg) if (g >= 0 && g < (int)NG) hideGuide[g] = 1;
        for (int f : hf) if (f >= 0 && f < (int)NF) hideFace[f] = 1;
        if (hg.empty()) hideGuide.clear();
        if (hf.empty()) hideFace.clear();
        rebuildHair();
        simulate = true; paused = false; windOn = wind > 0;
        motion.preset = MotionPreset::Rest; motion.speed = 1; motion.amount = 1; motion.windStrength = wind; motion.t = 0; simAcc = 0; teleport = true;
        R.pixMax.assign(C, {});
        R.frameCount.assign(C, std::vector<int>(nFrames, 0));
        for (int c = 0; c < C; c++) if (camOn[c]) R.pixMax[c].assign(size_t(S) * S, 0.0f);
        R.gPx.assign(C, std::vector<float>(NG, 0.0f));
        R.gDisp.assign(NG, 0.0f); R.gRise.assign(NG, 0.0f); R.gReach.assign(NG, 0.0f); R.gBestPx.assign(NG, 0.0f); R.gBestW.assign(NG, 0.5f); R.gBestCam.assign(NG, -1); R.gBestStep.assign(NG, -1);
        R.gWorst.assign(NG, {});
        if (keepFrames) { R.framePix.assign(nFrames, std::vector<std::vector<int>>(C)); R.frameGuides.assign(nFrames, {}); }
        R.readOk = readOk;
        int fr = 0;
        std::vector<int> ga;
        for (int s = 0; s < nSteps; s++) {
            motion.windStrength = ramp > 0 ? float(wind * std::min(1.0, (s + 1) / (60.0 * ramp))) : wind;
            step(1.0 / 60.0);
            windYield(progLo + (progHi - progLo) * float(s) / nSteps, phase);
            int pg = PG;
            bool got = readOk && hw.readGuides(xyz, pg) && pg == PG;
            if (readOk && !got) R.readOk = false;
            if (got) {
                if (keepTrace) R.trace.insert(R.trace.end(), xyz.begin(), xyz.end());
                if (ref) {
                    size_t off = size_t(s) * xyz.size();
                    if (R.stepDiff.size() <= size_t(s)) R.stepDiff.resize(size_t(s) + 1, 0.0f);
                    if (off + xyz.size() <= ref->size())
                        for (size_t i = 0; i < xyz.size(); i += 3) {
                            float dx = xyz[i] - (*ref)[off + i], dy = xyz[i + 1] - (*ref)[off + i + 1], dz = xyz[i + 2] - (*ref)[off + i + 2];
                            double d = std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz);
                            R.maxDiff = std::max(R.maxDiff, d); R.sumDiff += d; R.nDiff++;
                            R.stepDiff[s] = std::max(R.stepDiff[s], float(d));
                            {
                                size_t ai = i / 3 / size_t(PG);
                                if (ai < guideOfAsset.size()) {
                                    if (R.guideDiff.empty()) R.guideDiff.assign(NG, 0.0f);
                                    float& gd = R.guideDiff[guideOfAsset[ai]];
                                    gd = std::max(gd, float(d));
                                }
                            }
                        }
                }
                if (s == 0)
                    for (size_t a = 0; a < guideOfAsset.size(); a++) {
                        int g = guideOfAsset[a];
                        V3 p{xyz[a * PG * 3], xyz[a * PG * 3 + 1], xyz[a * PG * 3 + 2]};
                        R.rootErr = std::max(R.rootErr, len(p - restCV[size_t(g) * PG]));
                    }
                for (size_t a = 0; a < guideOfAsset.size(); a++) {
                    int g = guideOfAsset[a];
                    bool better = false;
                    for (int j = 0; j < PG; j++) {
                        V3 p{xyz[(a * PG + j) * 3], xyz[(a * PG + j) * 3 + 1], xyz[(a * PG + j) * 3 + 2]};
                        R.gDisp[g] = std::max(R.gDisp[g], len(p - restCV[size_t(g) * PG + j]));
                        R.gRise[g] = std::max(R.gRise[g], p.z - restCV[size_t(g) * PG + j].z);
                        R.gReach[g] = std::max(R.gReach[g], len(p - V3{xyz[a * PG * 3], xyz[a * PG * 3 + 1], xyz[a * PG * 3 + 2]}));
                        for (int c = 0; c < C; c++) {
                            Proj q = wproj(cams[c], float(S), p);
                            if (q.w <= 0) continue;
                            float d;
                            int ix = int(q.x), iy = int(q.y);
                            if (ix >= 0 && iy >= 0 && ix < S && iy < S) {
                                size_t pi = size_t(iy) * S + ix;
                                if (q.w > cams[c].zbuf[pi] + 0.004f) continue;   // behind the head / body
                                d = cams[c].dt[pi];
                            } else {
                                int cx = std::clamp(ix, 0, S - 1), cy = std::clamp(iy, 0, S - 1);
                                d = cams[c].dt[size_t(cy) * S + cx] + std::hypot(q.x - (cx + 0.5f), q.y - (cy + 0.5f));
                            }
                            if (d > R.gPx[c][g]) R.gPx[c][g] = d;
                            if (d > R.gBestPx[g]) { R.gBestPx[g] = d; R.gBestW[g] = q.w; R.gBestCam[g] = c; R.gBestStep[g] = s; better = true; }
                        }
                    }
                    if (better) {
                        R.gWorst[g].resize(PG);
                        for (int j = 0; j < PG; j++) R.gWorst[g][j] = {xyz[(a * PG + j) * 3], xyz[(a * PG + j) * 3 + 1], xyz[(a * PG + j) * 3 + 2]};
                    }
                }
            }
            if (fr < nFrames && s == sampleStep[fr]) {
                if (keepFrames) {
                    auto& FG = R.frameGuides[fr];
                    FG.assign(NG * size_t(PG), V3{NAN, NAN, NAN});
                    if (got)
                        for (size_t a = 0; a < guideOfAsset.size(); a++)
                            for (int j = 0; j < PG; j++) FG[size_t(guideOfAsset[a]) * PG + j] = {xyz[(a * PG + j) * 3], xyz[(a * PG + j) * 3 + 1], xyz[(a * PG + j) * 3 + 2]};
                }
                for (int c = 0; c < C; c++) {
                    if (!camOn[c]) continue;
                    windYield(progLo + (progHi - progLo) * float(s) / nSteps, phase);
                    if (!renderMask(cams[c], rgba)) throw std::runtime_error("offscreen render failed");
                    auto& pm = R.pixMax[c];
                    int cnt = 0;
                    for (size_t i = 0; i < pm.size(); i++) {
                        if (!hairPx(&rgba[i * 4])) continue;
                        float d = cams[c].dt[i];
                        if (d <= 0) continue;
                        if (d > pm[i]) pm[i] = d;
                        if (d > thr) { cnt++; if (keepFrames) R.framePix[fr][c].push_back(int(i)); }
                    }
                    R.frameCount[c][fr] = cnt;
                }
                fr++;
            }
        }
        return R;
    };
    auto unionCount = [&](const RunOut& R, int c, const std::vector<uint8_t>* region) {
        int n = 0;
        if (R.pixMax[c].empty()) return 0;
        for (size_t i = 0; i < R.pixMax[c].size(); i++) if (R.pixMax[c][i] > thr && (!region || (*region)[i])) n++;
        return n;
    };

    std::vector<char> allCams(C, 1);
    // merge run R into the envelope E: per pixel / per guide maxima, sample frames appended (attribution sees them all)
    auto merge = [&](RunOut& E, const RunOut& R) {
        if (E.pixMax.empty()) { E = R; E.trace.clear(); return; }
        E.readOk = E.readOk && R.readOk;
        for (int c = 0; c < C; c++) {
            if (E.pixMax[c].empty()) E.pixMax[c] = R.pixMax[c];
            else if (!R.pixMax[c].empty()) for (size_t i = 0; i < E.pixMax[c].size(); i++) E.pixMax[c][i] = std::max(E.pixMax[c][i], R.pixMax[c][i]);
            E.frameCount[c].insert(E.frameCount[c].end(), R.frameCount[c].begin(), R.frameCount[c].end());
            for (size_t g = 0; g < NG; g++) E.gPx[c][g] = std::max(E.gPx[c][g], R.gPx[c][g]);
        }
        for (size_t g = 0; g < NG; g++) {
            E.gDisp[g] = std::max(E.gDisp[g], R.gDisp[g]);
            E.gRise[g] = std::max(E.gRise[g], R.gRise[g]);
            E.gReach[g] = std::max(E.gReach[g], R.gReach[g]);
            if (R.gBestPx[g] > E.gBestPx[g]) {
                E.gBestPx[g] = R.gBestPx[g]; E.gBestW[g] = R.gBestW[g]; E.gBestCam[g] = R.gBestCam[g]; E.gBestStep[g] = R.gBestStep[g]; E.gWorst[g] = R.gWorst[g];
            }
        }
        E.framePix.insert(E.framePix.end(), R.framePix.begin(), R.framePix.end());
        E.frameGuides.insert(E.frameGuides.end(), R.frameGuides.begin(), R.frameGuides.end());
    };
    // The runtime's wind is not repeatable (measured 2026-10-14, Ciri cr18: two identical max-wind runs differ by 30 mm
    // at step 2 and up to ~200 mm later, with the fur's windNoise 2 and also with windNoise 0; with no wind they agree
    // to 0.07 mm; the difference grows with the wind strength). So one run is one sample of a random process: the
    // measurement is the envelope (per pixel / per guide maximum) of `repeats` runs, the noise floor of a cluster is the
    // difference between the envelopes of the two halves of them, and every elimination test is an envelope of half
    // as many runs, compared with the half-envelopes.
    const int half = reps >= 2 ? reps / 2 : 1;
    RunOut A, H[2], Calm;
    std::vector<std::vector<float>> runGuidePx;   // [run][guide] max excursion of each baseline run
    json rep;
    std::vector<float> trace0;
    for (int k = 0; k < reps; k++) {
        progLo = 0.02f + 0.40f * k / reps; progHi = 0.02f + 0.40f * (k + 1) / reps;
        std::string ph = "wind run " + std::to_string(k + 1) + "/" + std::to_string(reps);
        RunOut R = run(amount, {}, {}, allCams, true, k == 0 && reps > 1 && readOk, k == 1 && !trace0.empty() ? &trace0 : nullptr, ph.c_str());
        if (k == 0) trace0.swap(R.trace);
        if (k == 1 && R.nDiff) {   // single run vs single run, CV by CV
            rep["cv_max_diff_mm"] = std::round(R.maxDiff * 1e6) / 1000;
            rep["cv_mean_diff_mm"] = std::round(R.sumDiff / R.nDiff * 1e6) / 1000;
            int f0 = -1, f1 = -1;
            json perSec = json::array(), first = json::array();
            for (size_t i = 0; i < R.stepDiff.size(); i++) {
                float d = R.stepDiff[i];
                if (f0 < 0 && d > 0) f0 = int(i);
                if (f1 < 0 && d > 1e-3f) f1 = int(i);
                if (i % 60 == 0) perSec.push_back(0.0);
                perSec.back() = std::max(perSec.back().get<double>(), std::round(d * 1e5) / 100);
                if (i < 8) first.push_back(std::round(d * 1e5) / 100);
            }
            rep["first_diff_step"] = f0; rep["first_step_over_1mm"] = f1;
            rep["cv_max_diff_mm_first_steps"] = first;
            rep["cv_max_diff_mm_per_second"] = perSec;
            int over10 = 0;
            for (float d : R.guideDiff) over10 += d > 0.01f;
            rep["guides_diff_over_10mm"] = over10;
            rep["identical"] = R.maxDiff == 0;
        }
        if (k == 1) { trace0.clear(); trace0.shrink_to_fit(); }
        if (k == 1) {   // single runs: masks of run 1 vs run 2
            json pc = json::array();
            for (int c = 0; c < C; c++) {
                int a = unionCount(A, c, nullptr), b = unionCount(R, c, nullptr), both = 0, any = 0;
                for (size_t i = 0; i < A.pixMax[c].size(); i++) {
                    bool fa = A.pixMax[c][i] > thr, fb = R.pixMax[c][i] > thr;
                    both += fa && fb; any += fa || fb;
                }
                pc.push_back({{"camera", cams[c].name}, {"run1_px", a}, {"run2_px", b}, {"iou", any ? std::round(1000.0 * both / any) / 1000 : 1.0}});
            }
            rep["single_runs_per_camera"] = pc;
        }
        runGuidePx.push_back(R.gBestPx);
        if (reps >= 2 && k < 2 * half) merge(H[k / half], R);
        merge(A, R);
    }
    // the noise floor: envelope of half the runs vs the other half
    if (reps >= 2) {
        json pc = json::array();
        for (int c = 0; c < C; c++) {
            int a = unionCount(H[0], c, nullptr), b = unionCount(H[1], c, nullptr), both = 0, any = 0;
            for (size_t i = 0; i < H[0].pixMax[c].size(); i++) {
                bool fa = H[0].pixMax[c][i] > thr, fb = H[1].pixMax[c][i] > thr;
                both += fa && fb; any += fa || fb;
            }
            pc.push_back({{"camera", cams[c].name}, {"half_a_px", a}, {"half_b_px", b}, {"diff_px", b - a},
                          {"iou", any ? std::round(1000.0 * both / any) / 1000 : 1.0}, {"envelope_px", unionCount(A, c, nullptr)}});
        }
        rep["runs"] = reps; rep["runs_per_half"] = half;
        rep["half_envelopes_per_camera"] = pc;
        // per guide: spread of its max excursion over the runs
        float gmax = 0;
        for (size_t g = 0; g < NG; g++) {
            float lo = 1e9f, hi = 0;
            for (auto& v : runGuidePx) { lo = std::min(lo, v[g]); hi = std::max(hi, v[g]); }
            if (hi > thr) gmax = std::max(gmax, hi - lo);
        }
        rep["guide_excursion_max_spread_px"] = std::round(gmax * 10) / 10;
        rep["note"] = rep.value("identical", false) ? "identical runs: deterministic"
                    : "the runtime's wind differs run to run (not seedable from the API): values are maxima over all runs, removals are judged against the half-envelope difference (noise_px)";
    }
    if (doCalm) {
        progLo = 0.42f; progHi = 0.48f;
        Calm = run(0.0f, {}, {}, allCams, false, false, nullptr, "calm run (no wind)");
    }

    // ---- clusters of flagged pixels (baseline run A), per camera
    struct Cluster {
        int cam, id;
        std::vector<int> pix;
        std::vector<uint8_t> region;      // pixels dilated: where a re-run is counted
        int base = 0, noise = 0;          // flagged px inside region: run A, |A - B|
        float maxPx = 0;
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
        long long pf = 0, pfGuide = 0, pfFace = 0;   // pixel-frames: total, explained by a guide polyline, by a face
        json result;
    };
    std::vector<Cluster> cl;
    std::vector<std::vector<int>> cluOf(C);
    const int dil = std::max(1, int(std::lround(2 * scale))), regDil = std::max(2, int(std::lround(6 * scale)));
    for (int c = 0; c < C; c++) {
        cluOf[c].assign(size_t(S) * S, -1);
        std::vector<uint8_t> fl(size_t(S) * S, 0), fd(size_t(S) * S, 0);
        for (size_t i = 0; i < fl.size(); i++) fl[i] = A.pixMax[c][i] > thr;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                if (!fl[size_t(y) * S + x]) continue;
                for (int dy = -dil; dy <= dil; dy++)
                    for (int dx = -dil; dx <= dil; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && yy >= 0 && xx < S && yy < S) fd[size_t(yy) * S + xx] = 1;
                    }
            }
        std::vector<int> lab(size_t(S) * S, -1), stack;
        int nl = 0;
        for (size_t s0 = 0; s0 < fd.size(); s0++) {
            if (!fd[s0] || lab[s0] >= 0) continue;
            Cluster K0;
            K0.cam = c;
            stack.assign(1, int(s0));
            lab[s0] = nl;
            while (!stack.empty()) {
                int i = stack.back();
                stack.pop_back();
                if (fl[i]) K0.pix.push_back(i);
                int x = i % S, y = i / S;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= S || yy >= S) continue;
                        int j = yy * S + xx;
                        if (fd[j] && lab[j] < 0) { lab[j] = nl; stack.push_back(j); }
                    }
            }
            nl++;
            if ((int)K0.pix.size() < minCluster) continue;
            K0.region.assign(size_t(S) * S, 0);
            for (int i : K0.pix) {
                int x = i % S, y = i / S;
                K0.maxPx = std::max(K0.maxPx, A.pixMax[c][i]);
                K0.x0 = std::min(K0.x0, x); K0.y0 = std::min(K0.y0, y); K0.x1 = std::max(K0.x1, x); K0.y1 = std::max(K0.y1, y);
                for (int dy = -regDil; dy <= regDil; dy++)
                    for (int dx = -regDil; dx <= regDil; dx++) {
                        int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && yy >= 0 && xx < S && yy < S && dx * dx + dy * dy <= regDil * regDil) K0.region[size_t(yy) * S + xx] = 1;
                    }
            }
            K0.id = (int)cl.size();
            for (int i : K0.pix) cluOf[c][i] = K0.id;
            if (reps >= 2) {   // a test run set is half as many runs: compare it with the half-envelopes
                int h0 = unionCount(H[0], c, &K0.region), h1 = unionCount(H[1], c, &K0.region);
                K0.base = (h0 + h1 + 1) / 2;
                K0.noise = std::abs(h0 - h1);
            } else K0.base = unionCount(A, c, &K0.region);
            cl.push_back(std::move(K0));
        }
    }

    // ---- attribution (run A sample frames): simulated guide polylines, then interpolated face hair
    windYield(0.5f, "attribution");
    {
        const float rG = 2.5f * scale;
        std::vector<int> lastG(size_t(S) * S, -1), lastF(size_t(S) * S, -1);
        std::vector<uint8_t> expl(size_t(S) * S, 0), cur(size_t(S) * S, 0);
        std::vector<Proj> pr;
        for (int f = 0; f < nFrames; f++) {
            const auto& FG = A.frameGuides[f];
            if (FG.empty()) continue;
            for (int c = 0; c < C; c++) {
                const auto& L = A.framePix[f][c];
                if (L.empty()) continue;
                bool anyClu = false;
                for (int i : L) if (cluOf[c][i] >= 0) { cur[i] = 1; anyClu = true; cl[cluOf[c][i]].pf++; }
                if (!anyClu) continue;
                // guides: thick projected polyline, visible CVs only
                for (size_t g = 0; g < NG; g++) {
                    if (!groom.alive[g] || std::isnan(FG[g * PG].x)) continue;
                    pr.resize(PG);
                    for (int j = 0; j < PG; j++) pr[j] = wproj(cams[c], float(S), FG[g * PG + j]);
                    for (int j = 0; j + 1 < PG; j++) {
                        Proj a = pr[j], b = pr[j + 1];
                        if (a.w <= 0 || b.w <= 0) continue;
                        int x0 = std::max(0, int(std::floor(std::min(a.x, b.x) - rG))), x1 = std::min(S - 1, int(std::ceil(std::max(a.x, b.x) + rG)));
                        int y0 = std::max(0, int(std::floor(std::min(a.y, b.y) - rG))), y1 = std::min(S - 1, int(std::ceil(std::max(a.y, b.y) + rG)));
                        if (x0 > x1 || y0 > y1 || (x1 - x0) * (y1 - y0) > 200000) continue;
                        float ex = b.x - a.x, ey = b.y - a.y, l2 = ex * ex + ey * ey;
                        for (int y = y0; y <= y1; y++)
                            for (int x = x0; x <= x1; x++) {
                                size_t i = size_t(y) * S + x;
                                if (!cur[i] || lastG[i] == int(g)) continue;
                                float px = x + 0.5f, py = y + 0.5f;
                                float tt = l2 > 1e-6f ? std::clamp(((px - a.x) * ex + (py - a.y) * ey) / l2, 0.0f, 1.0f) : 0.0f;
                                float qx = a.x + ex * tt - px, qy = a.y + ey * tt - py;
                                if (qx * qx + qy * qy > rG * rG) continue;
                                float w = a.w + (b.w - a.w) * tt;
                                if (w > cams[c].zbuf[i] + 0.004f) continue;
                                lastG[i] = int(g);
                                if (!expl[i]) { expl[i] = 1; cl[cluOf[c][i]].pfGuide++; }
                            }
                    }
                }
                // faces: the interpolated hair at every CV level (and half levels) between the 3 simulated guides
                for (size_t fx = 0; fx < NF; fx++) {
                    const Face& F = groom.faces[fx];
                    if (!groom.alive[F.g[0]] || !groom.alive[F.g[1]] || !groom.alive[F.g[2]]) continue;
                    if (std::isnan(FG[size_t(F.g[0]) * PG].x) || std::isnan(FG[size_t(F.g[1]) * PG].x) || std::isnan(FG[size_t(F.g[2]) * PG].x)) continue;
                    for (int h = 0; h < 2 * PG - 1; h++) {
                        int j0 = h / 2, j1 = std::min(PG - 1, j0 + (h & 1));
                        Proj p3[3];
                        bool ok = true;
                        for (int q = 0; q < 3; q++) {
                            V3 P0 = FG[size_t(F.g[q]) * PG + j0], P1 = FG[size_t(F.g[q]) * PG + j1];
                            p3[q] = wproj(cams[c], float(S), (P0 + P1) * 0.5f);
                            if (p3[q].w <= 0.05f) ok = false;
                        }
                        if (!ok) continue;
                        float xa = std::min({p3[0].x, p3[1].x, p3[2].x}), xb = std::max({p3[0].x, p3[1].x, p3[2].x});
                        float ya = std::min({p3[0].y, p3[1].y, p3[2].y}), yb = std::max({p3[0].y, p3[1].y, p3[2].y});
                        int ix0 = std::max(0, int(std::floor(xa))), ix1 = std::min(S - 1, int(std::ceil(xb)));
                        int iy0 = std::max(0, int(std::floor(ya))), iy1 = std::min(S - 1, int(std::ceil(yb)));
                        if (ix0 > ix1 || iy0 > iy1) continue;
                        float area = (p3[1].x - p3[0].x) * (p3[2].y - p3[0].y) - (p3[2].x - p3[0].x) * (p3[1].y - p3[0].y);
                        if (std::fabs(area) < 0.25f) {   // degenerate on screen: its corners
                            for (int q = 0; q < 3; q++) {
                                int x = int(p3[q].x), y = int(p3[q].y);
                                if (x < 0 || y < 0 || x >= S || y >= S) continue;
                                size_t i = size_t(y) * S + x;
                                if (!cur[i] || expl[i] == 1 || lastF[i] == int(fx)) continue;
                                lastF[i] = int(fx);
                                if (expl[i] != 2) { expl[i] = 2; cl[cluOf[c][i]].pfFace++; }
                            }
                            continue;
                        }
                        float ia = 1.0f / area;
                        for (int y = iy0; y <= iy1; y++)
                            for (int x = ix0; x <= ix1; x++) {
                                size_t i = size_t(y) * S + x;
                                if (!cur[i] || expl[i] == 1 || lastF[i] == int(fx)) continue;
                                float px = x + 0.5f, py = y + 0.5f;
                                float l0 = ((p3[1].x - px) * (p3[2].y - py) - (p3[2].x - px) * (p3[1].y - py)) * ia;
                                float l1 = ((p3[2].x - px) * (p3[0].y - py) - (p3[0].x - px) * (p3[2].y - py)) * ia;
                                float l2 = 1.0f - l0 - l1;
                                const float e = -0.02f;
                                if (l0 < e || l1 < e || l2 < e) continue;
                                float w = 1.0f / (l0 / p3[0].w + l1 / p3[1].w + l2 / p3[2].w);
                                if (w > cams[c].zbuf[i] + 0.004f) continue;
                                lastF[i] = int(fx);
                                if (expl[i] != 2) { expl[i] = 2; cl[cluOf[c][i]].pfFace++; }
                            }
                    }
                    if ((fx & 255) == 0) windYield(0.5f + 0.05f * float(f) / nFrames, "attribution");
                }
                for (int i : L) { cur[i] = 0; expl[i] = 0; lastG[i] = -1; lastF[i] = -1; }
            }
        }
    }

    // ---- per-strand measurement from the read-back. At max wind the whole hair moves, so out-of-place pixels alone
    // do not name strays: a stray is a strand that goes much farther out than the strands rooted around it.
    // Guides: their own simulated CVs (every step). Faces: the interpolated hair between their 3 simulated guides
    // (7 barycentric hairs per face, every sample frame of every baseline run).
    auto cvDist = [&](int c, V3 p) -> float {   // px outside the rest silhouette, -1 when behind the head / body
        Proj q = wproj(cams[c], float(S), p);
        if (q.w <= 0) return -1;
        int ix = int(q.x), iy = int(q.y);
        if (ix >= 0 && iy >= 0 && ix < S && iy < S) {
            size_t pi = size_t(iy) * S + ix;
            if (q.w > cams[c].zbuf[pi] + 0.004f) return -1;
            return cams[c].dt[pi];
        }
        int cx = std::clamp(ix, 0, S - 1), cy = std::clamp(iy, 0, S - 1);
        return cams[c].dt[size_t(cy) * S + cx] + std::hypot(q.x - (cx + 0.5f), q.y - (cy + 0.5f));
    };
    std::vector<std::vector<float>> fPx(C, std::vector<float>(NF, 0.0f));
    std::vector<float> fBest(NF, 0.0f), fBestW(NF, 0.5f);
    std::vector<int> fBestCam(NF, -1);
    std::vector<std::vector<V3>> fWorst(NF);
    std::vector<char> faceOk(NF, 0);
    for (size_t f = 0; f < NF; f++) {
        const Face& F = groom.faces[f];
        faceOk[f] = groom.alive[F.g[0]] && groom.alive[F.g[1]] && groom.alive[F.g[2]];
    }
    if (readOk) {
        static const float bw[7][3] = {{1 / 3.f, 1 / 3.f, 1 / 3.f}, {.5f, .5f, 0}, {.5f, 0, .5f}, {0, .5f, .5f},
                                       {2 / 3.f, 1 / 6.f, 1 / 6.f}, {1 / 6.f, 2 / 3.f, 1 / 6.f}, {1 / 6.f, 1 / 6.f, 2 / 3.f}};
        std::vector<V3> poly(PG);
        for (size_t fr = 0; fr < A.frameGuides.size(); fr++) {
            const auto& FG = A.frameGuides[fr];
            if (FG.empty()) continue;
            windYield(0.5f + 0.04f * float(fr) / A.frameGuides.size(), "face hair");
            for (size_t f = 0; f < NF; f++) {
                if (!faceOk[f]) continue;
                const Face& F = groom.faces[f];
                if (std::isnan(FG[size_t(F.g[0]) * PG].x) || std::isnan(FG[size_t(F.g[1]) * PG].x) || std::isnan(FG[size_t(F.g[2]) * PG].x)) continue;
                for (auto& w : bw) {
                    for (int j = 0; j < PG; j++)
                        poly[j] = FG[size_t(F.g[0]) * PG + j] * w[0] + FG[size_t(F.g[1]) * PG + j] * w[1] + FG[size_t(F.g[2]) * PG + j] * w[2];
                    bool better = false;
                    for (int c = 0; c < C; c++) {
                        float m = 0;
                        for (int j = 1; j < PG; j++) m = std::max(m, cvDist(c, poly[j]));
                        if (m > fPx[c][f]) fPx[c][f] = m;
                        if (m > fBest[f]) { fBest[f] = m; fBestCam[f] = c; better = true; }
                    }
                    if (better) fWorst[f] = poly;
                }
            }
        }
    }
    // stray score: own excursion - median excursion of the 16 guides rooted nearest, per camera, max over cameras
    std::vector<int> aliveG;
    for (size_t g = 0; g < NG; g++) if (groom.alive[g]) aliveG.push_back(int(g));
    auto nearestRoots = [&](V3 r, int self) {
        std::vector<std::pair<float, int>> nb;
        for (int h : aliveG) if (h != self) nb.push_back({len(groom.P[h][0] - r), h});
        size_t m = std::min<size_t>(16, nb.size());
        std::partial_sort(nb.begin(), nb.begin() + m, nb.end());
        std::vector<int> o;
        for (size_t i = 0; i < m; i++) o.push_back(nb[i].second);
        return o;
    };
    auto neighbourMedian = [&](const std::vector<int>& nb, int c) {
        std::vector<float> v;
        for (int h : nb) v.push_back(A.gPx[c][h]);
        if (v.empty()) return 0.0f;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    std::vector<float> strayScore(NG, 0.0f), strayMed(NG, 0.0f), fStray(NF, 0.0f), fStrayMed(NF, 0.0f);
    std::vector<int> strayCam(NG, -1), fStrayCam(NF, -1);
    if (readOk) {
        for (int g : aliveG) {
            auto nb = nearestRoots(groom.P[g][0], g);
            for (int c = 0; c < C; c++) {
                float md = neighbourMedian(nb, c), sc = A.gPx[c][g] - md;
                if (sc > strayScore[g]) { strayScore[g] = sc; strayCam[g] = c; strayMed[g] = md; }
            }
        }
        for (size_t f = 0; f < NF; f++) {
            if (!faceOk[f] || fBest[f] <= thr) continue;
            const Face& F = groom.faces[f];
            auto nb = nearestRoots((groom.P[F.g[0]][0] + groom.P[F.g[1]][0] + groom.P[F.g[2]][0]) * (1.0f / 3), -1);
            for (int c = 0; c < C; c++) {
                // the face's hair has to go farther than at least one of its own guides: else it is that guide's flight
                float own = std::min({A.gPx[c][F.g[0]], A.gPx[c][F.g[1]], A.gPx[c][F.g[2]]});
                if (fPx[c][f] < own + thr) continue;
                float md = neighbourMedian(nb, c), sc = fPx[c][f] - md;
                if (sc > fStray[f]) { fStray[f] = sc; fStrayCam[f] = c; fStrayMed[f] = md; }
            }
        }
    }

    // ---- candidates: guides / faces whose stray score passes stray_px (default 2 x threshold)
    struct Item {
        int kind, id;                       // 0 guide, 1 face
        float score = 0, px = 0;
        int cam = -1;                       // camera of the footprint
        std::vector<int> region;            // footprint: flagged envelope pixels along its worst strand (that camera)
        int base = 0, noise = 0;            // flagged px in the footprint: half-envelope mean, |half A - half B|
        float azim = 0;
    };
    std::vector<Item> items;
    {
        std::vector<Item> all;
        for (int g : aliveG) if (strayScore[g] >= strayMin) all.push_back({0, g, strayScore[g], A.gBestPx[g]});
        for (size_t f = 0; f < NF; f++) {
            if (fStray[f] < strayMin) continue;
            const Face& F = groom.faces[f];
            bool covered = true;   // all 3 guides are candidates already: the face goes with them
            for (int q = 0; q < 3; q++) covered = covered && strayScore[F.g[q]] >= strayMin;
            if (!covered) all.push_back({1, int(f), fStray[f], fBest[f]});
        }
        std::stable_sort(all.begin(), all.end(), [](const Item& a, const Item& b) { return a.score > b.score; });
        int nf = 0;
        for (auto& it : all) {
            if ((int)items.size() >= maxCand) break;
            if (it.kind == 1 && nf >= maxCand / 3) continue;
            nf += it.kind == 1;
            items.push_back(it);
        }
    }
    const float rr = std::max(2.0f, 3.0f * scale);
    // footprint: pixels along its worst strand that hair reaches (envelope > threshold) AND that lie farther from the rest
    // silhouette than its neighbours get (floor = their median excursion in that camera): the part of the flight that
    // is its own, so other hair blowing nearby does not hide its removal
    auto footprint = [&](const std::vector<V3>& W, int c, float floor, std::vector<int>& outPix) {
        outPix.clear();
        if (W.size() < 2) return;
        std::vector<uint8_t> mark;
        std::vector<Proj> pr(W.size());
        for (size_t j = 0; j < W.size(); j++) pr[j] = wproj(cams[c], float(S), W[j]);
        for (size_t j = 0; j + 1 < W.size(); j++) {
            Proj a = pr[j], b = pr[j + 1];
            if (a.w <= 0 || b.w <= 0) continue;
            int x0 = std::max(0, int(std::floor(std::min(a.x, b.x) - rr))), x1 = std::min(S - 1, int(std::ceil(std::max(a.x, b.x) + rr)));
            int y0 = std::max(0, int(std::floor(std::min(a.y, b.y) - rr))), y1 = std::min(S - 1, int(std::ceil(std::max(a.y, b.y) + rr)));
            if (x0 > x1 || y0 > y1 || (x1 - x0) * (y1 - y0) > 400000) continue;
            float ex = b.x - a.x, ey = b.y - a.y, l2 = ex * ex + ey * ey;
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    size_t i = size_t(y) * S + x;
                    if (A.pixMax[c][i] <= thr || cams[c].dt[i] <= floor) continue;
                    float px = x + 0.5f, py = y + 0.5f;
                    float tt = l2 > 1e-6f ? std::clamp(((px - a.x) * ex + (py - a.y) * ey) / l2, 0.0f, 1.0f) : 0.0f;
                    float qx = a.x + ex * tt - px, qy = a.y + ey * tt - py;
                    if (qx * qx + qy * qy > rr * rr) continue;
                    if (mark.empty()) mark.assign(size_t(S) * S, 0);
                    if (!mark[i]) { mark[i] = 1; outPix.push_back(int(i)); }
                }
        }
    };
    auto regionCount = [&](const RunOut& R, int c, const std::vector<int>& reg) {
        if (c < 0 || R.pixMax[c].empty()) return 0;
        int n = 0;
        for (int i : reg) n += R.pixMax[c][i] > thr;
        return n;
    };
    std::vector<std::vector<std::vector<int>>> footAll(items.size());   // [item][camera] footprint (sorted pixel indices)
    for (size_t k = 0; k < items.size(); k++) {
        auto& it = items[k];
        const std::vector<V3>& W = it.kind == 0 ? A.gWorst[it.id] : fWorst[it.id];
        footAll[k].assign(C, {});
        size_t bestN = 0;
        V3 rc = it.kind == 0 ? groom.P[it.id][0]
                             : (groom.P[groom.faces[it.id].g[0]][0] + groom.P[groom.faces[it.id].g[1]][0] + groom.P[groom.faces[it.id].g[2]][0]) * (1.0f / 3);
        auto nb = nearestRoots(rc, it.kind == 0 ? it.id : -1);
        for (int c = 0; c < C; c++) {
            footprint(W, c, std::max(thr, neighbourMedian(nb, c)), footAll[k][c]);
            std::sort(footAll[k][c].begin(), footAll[k][c].end());
            if (footAll[k][c].size() > bestN) { bestN = footAll[k][c].size(); it.cam = c; }
        }
        if (it.cam >= 0) it.region = footAll[k][it.cam];
        V3 r = it.kind == 0 ? groom.P[it.id][0] : groom.P[groom.faces[it.id].g[0]][0];
        it.azim = std::atan2(r.y - pack.headC.y, r.x - pack.headC.x);
        if (reps >= 2 && it.cam >= 0) {
            int h0 = regionCount(H[0], it.cam, it.region), h1 = regionCount(H[1], it.cam, it.region);
            it.base = (h0 + h1 + 1) / 2;
            it.noise = std::abs(h0 - h1);
        } else if (it.cam >= 0) it.base = regionCount(A, it.cam, it.region);
    }

    // ---- elimination (re-run the same wind with candidates hidden; every test is an envelope of as many runs as a
    // half-envelope of the baseline, and a removal counts only above the half-envelope noise of that footprint):
    //  1. all candidates hidden: the ones whose out-of-place hair does not go away are not culprits (other hair there)
    //  2. batches of candidates whose footprints do not overlap in any camera, hidden together: each one whose own
    //     footprint empties is a culprit by itself
    //  3. the rest, grouped by overlapping footprints (a clump), hidden per group, then halved while a half alone
    //     still removes it: what only goes away together is reported as a group
    const int minFoot = std::max(6, int(8 * scale * scale));
    struct Test { std::vector<float> removal; json side; };
    std::vector<json> testsLog;
    int testRuns = 0;
    std::vector<float> itemRem(items.size(), NAN);           // removal in the last test that hid the item
    std::vector<int> itemTestN(items.size(), 0);             // size of that test's hidden set
    std::vector<json> itemSide(items.size());
    auto rootOf = [&](int k) {
        if (items[k].kind == 0) return groom.P[items[k].id][0];
        const Face& F = groom.faces[items[k].id];
        return (groom.P[F.g[0]][0] + groom.P[F.g[1]][0] + groom.P[F.g[2]][0]) * (1.0f / 3);
    };
    auto rootDist = [&](int a, int b) { return len(rootOf(a) - rootOf(b)); };
    auto strandLen = [&](int g) { float l = 0; for (int k = 1; k < K; k++) l += len(groom.P[g][k] - groom.P[g][k - 1]); return l; };
    auto test = [&](const std::vector<int>& grp, const char* why) {
        std::set<int> hg, hf;
        std::vector<char> on(C, 0);
        for (int k : grp) { (items[k].kind == 0 ? hg : hf).insert(items[k].id); if (items[k].cam >= 0) on[items[k].cam] = 1; }
        progLo = 0.56f + 0.42f * std::min(1.0f, float(testRuns) / std::max(1, maxRuns));
        progHi = 0.56f + 0.42f * std::min(1.0f, float(testRuns + 1) / std::max(1, maxRuns));
        testRuns++;
        RunOut R;
        for (int q = 0; q < half; q++) merge(R, run(amount, hg, hf, on, false, false, nullptr, "confirming (hide + re-run)"));
        Test t;
        t.removal.assign(items.size(), 0.0f);
        json lg = {{"step", why}, {"hidden", json::array()}, {"removal", json::object()}};
        for (int k : grp) {
            const Item& it = items[k];
            if (it.base > 0) t.removal[k] = 1.0f - float(regionCount(R, it.cam, it.region)) / it.base;
            std::string nm = (it.kind ? "face " : "guide ") + std::to_string(it.id);
            lg["hidden"].push_back(nm);
            lg["removal"][nm] = std::round(t.removal[k] * 1000) / 1000;
        }
        // side effect: guides left in that now fly clearly farther than in any baseline half (hair interaction couples
        // neighbours: deleting a flyer can let the guides next to it fly instead)
        if (R.readOk) {
            std::vector<std::pair<float, int>> up;
            for (size_t g = 0; g < NG; g++) {
                if (!groom.alive[g] || hg.count(int(g))) continue;
                float b0 = reps >= 2 ? std::max(H[0].gBestPx[g], H[1].gBestPx[g]) : A.gBestPx[g];
                float nz = reps >= 2 ? std::fabs(H[0].gBestPx[g] - H[1].gBestPx[g]) : 0.0f;
                float inc = R.gBestPx[g] - b0;
                if (inc > std::max(2 * thr, 2 * nz) && R.gBestPx[g] > strayMin) up.push_back({inc, int(g)});
            }
            std::sort(up.rbegin(), up.rend());
            t.side = json::array();
            for (size_t i = 0; i < up.size() && i < 12; i++) {
                int g = up[i].second;
                t.side.push_back({{"guide", g}, {"px", std::round(R.gBestPx[g])}, {"reach_mm", std::round(R.gReach[g] * 1000)}, {"length_mm", std::round(strandLen(g) * 1000)}, {"baseline_px", std::round((reps >= 2 ? std::max(H[0].gBestPx[g], H[1].gBestPx[g]) : A.gBestPx[g]))}});
            }
            if (!t.side.empty()) lg["others_fly_farther"] = t.side;
        }
        // each side effect goes to the hidden member rooted nearest to it (members of a batch are far apart)
        std::map<int, json> mine;
        if (t.side.is_array())
            for (auto& sd : t.side) {
                V3 rg = groom.P[sd["guide"].get<int>()][0];
                int bk = -1;
                float bd = 0.08f;
                for (int k : grp) { float d = len(rootOf(k) - rg); if (d < bd) { bd = d; bk = k; } }
                if (bk >= 0) mine[bk].push_back(sd);
            }
        for (int k : grp) { itemRem[k] = t.removal[k]; itemTestN[k] = (int)grp.size(); itemSide[k] = mine.count(k) ? mine[k] : json::array(); }
        testsLog.push_back(lg);
        return t;
    };
    auto need = [&](int k) {
        const Item& it = items[k];
        return it.base >= minFoot ? std::max(minFrac, float(2 * it.noise + 3) / it.base) : 2.0f;
    };
    const float spacing = P.value("batch_spacing_mm", 40.0f) / 1000.0f;
    auto overlap = [&](int a, int b) {   // footprints share >= 20 % of the smaller one in some camera, or roots within 6 mm
        for (int c = 0; c < C; c++) {
            const auto& x = footAll[a][c];
            const auto& y = footAll[b][c];
            if (x.empty() || y.empty()) continue;
            size_t i = 0, j = 0, n = 0;
            while (i < x.size() && j < y.size()) { if (x[i] < y[j]) i++; else if (y[j] < x[i]) j++; else { n++; i++; j++; } }
            if (n * 5 >= std::min(x.size(), y.size())) return true;
        }
        return rootDist(a, b) < 0.006f;
    };
    struct Found { std::vector<int> grp; Test t; bool group; };
    std::vector<Found> found;
    std::vector<int> notConfirmed;
    std::vector<int> testable;
    for (int k = 0; k < (int)items.size(); k++) (items[k].base >= minFoot ? testable : notConfirmed).push_back(k);
    json allTest;
    if (confirm && !testable.empty()) {
        std::vector<int> live(testable);
        // 1. independent batches (greedy colouring: no two members with overlapping footprints or roots closer than
        //    batch_spacing_mm, strongest strays first)
        std::vector<std::vector<char>> ov(items.size(), std::vector<char>(items.size(), 0)), ovB(ov);
        for (size_t a = 0; a < live.size(); a++)
            for (size_t b = a + 1; b < live.size(); b++) {
                int x = live[a], y = live[b];
                bool o = overlap(x, y);
                ov[x][y] = ov[y][x] = o;
                ovB[x][y] = ovB[y][x] = o || rootDist(x, y) < spacing;
            }
        std::vector<int> ord(live);
        std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) { return items[a].score > items[b].score; });
        std::vector<std::vector<int>> batches;
        for (int k : ord) {
            bool put = false;
            for (auto& bt : batches) {
                bool ok = true;
                for (int j : bt) ok = ok && !ovB[k][j];
                if (ok) { bt.push_back(k); put = true; break; }
            }
            if (!put) batches.push_back({k});
        }
        std::vector<int> single;   // confirmed alone
        std::vector<char> done(items.size(), 0);
        for (auto& bt : batches) {
            if (testRuns >= maxRuns) break;
            Test t = test(bt, "independent batch");
            for (int k : bt)
                if (t.removal[k] >= need(k)) { found.push_back({{k}, t, false}); done[k] = 1; }
                else done[k] = 2;   // tested, not alone
        }
        // 2. what is left: connected groups of overlapping footprints
        std::vector<int> rest;
        for (int k : live) if (done[k] != 1) rest.push_back(k);
        std::vector<int> comp(items.size(), -1);
        int nc = 0;
        for (int k : rest) {
            if (comp[k] >= 0) continue;
            std::vector<int> st{k};
            comp[k] = nc;
            while (!st.empty()) {
                int a = st.back();
                st.pop_back();
                for (int b : rest) if (comp[b] < 0 && ov[a][b]) { comp[b] = nc; st.push_back(b); }
            }
            nc++;
        }
        // a group: keep the members it removes; halve them (neighbours by root azimuth stay together) while a half alone
        // still removes its own; what goes away only with the others hidden is a group culprit
        std::function<void(const std::vector<int>&, const Test&)> groupProbe = [&](const std::vector<int>& grp, const Test& tg) {
            std::vector<int> sig;
            for (int k : grp) (tg.removal[k] >= need(k) ? sig : notConfirmed).push_back(k);
            if (sig.empty()) return;
            if (sig.size() == 1) { found.push_back({sig, tg, false}); return; }
            if (testRuns + 2 > maxRuns) { found.push_back({sig, tg, true}); return; }
            std::stable_sort(sig.begin(), sig.end(), [&](int a, int b) { return items[a].azim < items[b].azim; });
            std::vector<int> a(sig.begin(), sig.begin() + sig.size() / 2), b(sig.begin() + sig.size() / 2, sig.end());
            Test ta = test(a, "half of a group"), tb = test(b, "half of a group");
            std::vector<int> ra, rb, residual;
            for (int k : a) (ta.removal[k] >= need(k) ? ra : residual).push_back(k);
            for (int k : b) (tb.removal[k] >= need(k) ? rb : residual).push_back(k);
            if (ra.empty() && rb.empty()) { found.push_back({sig, tg, true}); return; }
            if (!ra.empty()) groupProbe(ra, ta);
            if (!rb.empty()) groupProbe(rb, tb);
            if (!residual.empty()) found.push_back({residual, tg, true});
        };
        for (int c0 = 0; c0 < nc; c0++) {
            std::vector<int> grp;
            for (int k : rest) if (comp[k] == c0) grp.push_back(k);
            if (grp.size() == 1) { notConfirmed.push_back(grp[0]); continue; }   // alone and not confirmed alone
            if (testRuns >= maxRuns) { for (int k : grp) notConfirmed.push_back(k); continue; }
            Test t = test(grp, "overlapping group");
            groupProbe(grp, t);
        }
    } else
        for (int k : testable) notConfirmed.push_back(k);
    std::set<int> inFound;
    for (auto& F0 : found) for (int k : F0.grp) inFound.insert(k);
    {
        std::vector<int> nc2;
        for (int k : notConfirmed) if (!inFound.count(k)) nc2.push_back(k);
        notConfirmed.swap(nc2);
    }

    // ---- causes
    std::vector<float> glen(NG, 0);
    for (size_t g = 0; g < NG; g++) if (groom.alive[g]) for (int k = 1; k < K; k++) glen[g] += len(groom.P[g][k] - groom.P[g][k - 1]);
    float baseStiff = look.f("physicalMaterials.stiffness.stiffness", 0.5f), baseRoot = look.f("physicalMaterials.stiffness.rootStiffness", 0.5f);
    auto neighbourLen = [&](int g) {
        std::vector<std::pair<float, int>> nb;
        for (size_t h = 0; h < NG; h++) if (groom.alive[h] && int(h) != g) nb.push_back({len(groom.P[h][0] - groom.P[g][0]), int(h)});
        size_t m = std::min<size_t>(8, nb.size());
        std::partial_sort(nb.begin(), nb.begin() + m, nb.end());
        std::vector<float> ls;
        for (size_t i = 0; i < m; i++) ls.push_back(glen[nb[i].second]);
        std::sort(ls.begin(), ls.end());
        return ls.empty() ? glen[g] : ls[ls.size() / 2];
    };
    auto rootSpOf = [&](const Face& F) {
        return std::max({len(groom.P[F.g[0]][0] - groom.P[F.g[1]][0]), len(groom.P[F.g[1]][0] - groom.P[F.g[2]][0]), len(groom.P[F.g[2]][0] - groom.P[F.g[0]][0])});
    };
    auto guideCauses = [&](int g, json& det) {
        std::vector<std::string> cs;
        LoopInfo li = guideLoop(groom.P[g]);
        if (li.flag) {
            char b[96];
            if (li.radius > 0) snprintf(b, sizeof b, "loop (returns to %.0f mm after %.0f mm of hair)", li.dist * 1000, li.arc * 1000);
            else snprintf(b, sizeof b, "loop (circles %.0f deg)", li.turn);
            cs.push_back(b);
        }
        float nl = neighbourLen(g);
        det["length_mm"] = std::round(glen[g] * 1000);
        det["neighbour_median_length_mm"] = std::round(nl * 1000);
        if (nl > 1e-4f && glen[g] > 1.4f * nl) { char b[64]; snprintf(b, sizeof b, "over-long (x%.1f of its neighbours)", glen[g] / nl); cs.push_back(b); }
        std::vector<int> dv;
        for (size_t f = 0; f < NF; f++) {
            const Face& F = groom.faces[f];
            if (F.g[0] != g && F.g[1] != g && F.g[2] != g) continue;
            if (!groom.alive[F.g[0]] || !groom.alive[F.g[1]] || !groom.alive[F.g[2]]) continue;
            if (faceDivergent(groom, F, rootSpOf(F))) dv.push_back(int(f));
        }
        if (!dv.empty()) { cs.push_back("divergent face fan (" + std::to_string(dv.size()) + " face(s))"); det["divergent_faces"] = dv; }
        float s = 1, r = 1;
        if (smap.painted && g < (int)smap.guideS.size()) { s = smap.guideS[g]; r = smap.guideR[g]; }
        det["stiffness_map"] = {{"s", std::round(s * 100) / 100}, {"r", std::round(r * 100) / 100}, {"painted", smap.painted}};
        det["stiffness"] = std::round(baseStiff * s * 1000) / 1000;
        det["root_stiffness"] = std::round(baseRoot * r * 1000) / 1000;
        if (smap.painted && (s < 0.6f || r < 0.6f)) { char b[80]; snprintf(b, sizeof b, "soft stiffness (map s %.2f r %.2f)", s, r); cs.push_back(b); }
        float rise = 0;
        for (auto& p : groom.P[g]) rise = std::max(rise, p.z - groom.P[g][0].z);
        det["rest_rise_mm"] = std::round(rise * 1000);
        if (cs.empty()) cs.push_back("other (no loop / length / fan / stiffness defect: lifted by the wind)");
        return cs;
    };

    // ---- report
    json camsR = json::array();
    for (int c = 0; c < C; c++) {
        int outside = 0, flagged = unionCount(A, c, nullptr);
        float mx = 0;
        for (float v : A.pixMax[c]) { if (v > 0) outside++; mx = std::max(mx, v); }
        json clj = json::array();
        for (auto& K0 : cl) {
            if (K0.cam != c) continue;
            json e = {{"cluster", K0.id}, {"px", (int)K0.pix.size()}, {"max_px", std::round(K0.maxPx * 10) / 10},
                      {"max_mm", std::round(K0.maxPx * mmPerPx(cams[c], cams[c].tdist) * 10) / 10},
                      {"bbox", {K0.x0, K0.y0, K0.x1, K0.y1}}, {"region_px", K0.base}, {"noise_px", K0.noise},
                      {"pixel_frames", K0.pf},
                      {"by_guides", K0.pf ? std::round(1000.0 * K0.pfGuide / K0.pf) / 1000 : 0.0},
                      {"by_faces", K0.pf ? std::round(1000.0 * K0.pfFace / K0.pf) / 1000 : 0.0},
                      {"unexplained", K0.pf ? std::round(1000.0 * (K0.pf - K0.pfGuide - K0.pfFace) / K0.pf) / 1000 : 0.0}};
            for (auto& [k, v] : K0.result.items()) e[k] = v;
            clj.push_back(e);
        }
        json e = {{"camera", cams[c].name}, {"eye", {cams[c].eye.x, cams[c].eye.y, cams[c].eye.z}}, {"target", {cams[c].tgt.x, cams[c].tgt.y, cams[c].tgt.z}},
                  {"fovy", cams[c].fovy}, {"mm_per_px_at_head", std::round(mmPerPx(cams[c], cams[c].tdist) * 1000) / 1000},
                  {"flagged_px", flagged}, {"outside_rest_px", outside}, {"max_px", std::round(mx * 10) / 10}, {"clusters", clj}};
        if (doCalm) e["calm_flagged_px"] = unionCount(Calm, c, nullptr);
        if (reps >= 2) { e["half_a_flagged_px"] = unionCount(H[0], c, nullptr); e["half_b_flagged_px"] = unionCount(H[1], c, nullptr); }
        camsR.push_back(e);
    }
    // read-back flyers: every guide whose own simulated CVs leave the rest silhouette by more than the threshold
    json flyers = json::array();
    if (readOk) {
        std::vector<std::pair<float, int>> fl;
        for (size_t g = 0; g < NG; g++) if (A.gBestPx[g] > thr) fl.push_back({A.gBestPx[g], int(g)});
        std::sort(fl.rbegin(), fl.rend());
        for (size_t i = 0; i < fl.size() && i < 60; i++) {
            int g = fl[i].second, c = A.gBestCam[g];
            json cs = json::array();
            for (int q = 0; q < C; q++) if (A.gPx[q][g] > thr) cs.push_back(cams[q].name);
            json e = {{"guide", g}, {"max_px", std::round(fl[i].first * 10) / 10}, {"max_mm", std::round(fl[i].first * mmPerPx(cams[c], A.gBestW[g]) * 10) / 10},
                      {"camera", cams[c].name}, {"cameras", cs}, {"time_s", std::round((A.gBestStep[g] + 1) / 60.0 * 100) / 100},
                      {"disp_mm", std::round(A.gDisp[g] * 1000)}, {"root", {r4(groom.P[g][0].x), r4(groom.P[g][0].y), r4(groom.P[g][0].z)}}};
            if (doCalm) e["calm_px"] = std::round(Calm.gBestPx[g] * 10) / 10;
            if (runGuidePx.size() > 1) {   // how often / how far in each run (the wind is random)
                std::vector<float> v;
                int n = 0;
                for (auto& rg : runGuidePx) { v.push_back(rg[g]); n += rg[g] > thr; }
                std::sort(v.begin(), v.end());
                e["runs_flagged"] = std::to_string(n) + "/" + std::to_string(v.size());
                e["median_px"] = std::round(v[v.size() / 2] * 10) / 10;
                e["min_px"] = std::round(v[0] * 10) / 10;
            }
            flyers.push_back(e);
        }
    }
    // strays (read-back ranking, before any confirmation): guides and faces by stray score
    json strays = json::array();
    if (readOk) {
        std::vector<std::pair<float, int>> st;   // id >= 0 guide, < 0 face (-1 - f)
        for (int g : aliveG) if (strayScore[g] > thr) st.push_back({strayScore[g], g});
        for (size_t f = 0; f < NF; f++) if (fStray[f] > thr) st.push_back({fStray[f], -1 - int(f)});
        std::sort(st.rbegin(), st.rend());
        for (size_t i = 0; i < st.size() && i < 60; i++) {
            int id = st[i].second;
            json e;
            if (id >= 0) {
                int c = strayCam[id];
                e = {{"kind", "guide"}, {"id", id}, {"stray_px", std::round(strayScore[id] * 10) / 10}, {"camera", cams[c].name},
                     {"own_px", std::round(A.gPx[c][id] * 10) / 10}, {"neighbours_median_px", std::round(strayMed[id] * 10) / 10},
                     {"max_px", std::round(A.gBestPx[id] * 10) / 10}, {"rise_mm", std::round(A.gRise[id] * 1000)}, {"disp_mm", std::round(A.gDisp[id] * 1000)}};
                if (runGuidePx.size() > 1) {
                    int n = 0;
                    for (auto& rg : runGuidePx) n += rg[id] > thr;
                    e["runs_flagged"] = std::to_string(n) + "/" + std::to_string(runGuidePx.size());
                }
            } else {
                int f = -1 - id, c = fStrayCam[f];
                const Face& F = groom.faces[f];
                e = {{"kind", "face"}, {"id", f}, {"guides", {F.g[0], F.g[1], F.g[2]}}, {"stray_px", std::round(fStray[f] * 10) / 10},
                     {"camera", cams[c].name}, {"own_px", std::round(fPx[c][f] * 10) / 10}, {"neighbours_median_px", std::round(fStrayMed[f] * 10) / 10},
                     {"guides_px", {std::round(A.gPx[c][F.g[0]] * 10) / 10, std::round(A.gPx[c][F.g[1]] * 10) / 10, std::round(A.gPx[c][F.g[2]] * 10) / 10}},
                     {"max_px", std::round(fBest[f] * 10) / 10}};
            }
            strays.push_back(e);
        }
    }

    // ---- culprits
    auto itemJson = [&](int k, json& e, std::vector<std::string>& causes) {
        const Item& it = items[k];
        std::vector<int> gl;
        if (it.kind == 0) gl.push_back(it.id);
        else for (int q = 0; q < 3; q++) gl.push_back(groom.faces[it.id].g[q]);
        e = {{"kind", it.kind ? "face" : "guide"}, {"id", it.id}, {"guides", gl}, {"stray_px", std::round(it.score * 10) / 10}};
        json camN = json::array();
        float mpx = 0, mmm = 0;
        if (it.kind == 0) {
            int g = it.id, c = A.gBestCam[g];
            mpx = A.gBestPx[g];
            if (c >= 0) mmm = mpx * mmPerPx(cams[c], A.gBestW[g]);
            for (int q = 0; q < C; q++) if (A.gPx[q][g] > thr) camN.push_back(cams[q].name);
            e["rise_mm"] = std::round(A.gRise[g] * 1000);
            e["disp_mm"] = std::round(A.gDisp[g] * 1000);
            if (doCalm) e["calm_px"] = std::round(Calm.gBestPx[g] * 10) / 10;
            // its faces whose interpolated hair flies clearly farther than one of their guides (the sheet between them):
            // deleting the guide removes them too
            json sf = json::array();
            for (size_t f = 0; f < NF; f++) {
                const Face& F = groom.faces[f];
                if (!faceOk[f] || (F.g[0] != g && F.g[1] != g && F.g[2] != g) || fBestCam[f] < 0) continue;
                int c = fBestCam[f];
                float own = std::min({A.gPx[c][F.g[0]], A.gPx[c][F.g[1]], A.gPx[c][F.g[2]]});
                if (fBest[f] > thr && fBest[f] > own + thr)
                    sf.push_back({{"face", f}, {"guides", {F.g[0], F.g[1], F.g[2]}}, {"max_px", std::round(fBest[f] * 10) / 10},
                                  {"lowest_guide_px", std::round(own * 10) / 10}});
            }
            if (!sf.empty()) e["straying_faces"] = sf;
            if (runGuidePx.size() > 1) {
                int n = 0;
                for (auto& rg : runGuidePx) n += rg[g] > thr;
                e["runs_flagged"] = std::to_string(n) + "/" + std::to_string(runGuidePx.size());
            }
        } else {
            int f = it.id, c = fBestCam[f];
            mpx = fBest[f];
            if (c >= 0 && !fWorst[f].empty()) {   // depth of its farthest point in that camera
                float w = 0.5f, dm = -1;
                for (auto& p : fWorst[f]) { float d = cvDist(c, p); if (d > dm) { dm = d; w = wproj(cams[c], float(S), p).w; } }
                mmm = mpx * mmPerPx(cams[c], w);
            }
            for (int q = 0; q < C; q++) if (fPx[q][f] > thr) camN.push_back(cams[q].name);
        }
        e["max_px"] = std::round(mpx * 10) / 10;
        e["max_mm"] = std::round(mmm * 10) / 10;
        e["cameras"] = camN;
        if (it.cam >= 0) e["footprint"] = {{"camera", cams[it.cam].name}, {"px", (int)it.region.size()}, {"flagged_px", it.base}, {"noise_px", it.noise}};
        int g0 = gl[0];
        e["root"] = {r4(groom.P[g0][0].x), r4(groom.P[g0][0].y), r4(groom.P[g0][0].z)};
        json det;
        causes.clear();
        if (it.kind == 0) causes = guideCauses(it.id, det);
        else {
            const Face& F = groom.faces[it.id];
            float tipSp = 0;
            for (int q = 0; q < 3; q++) tipSp = std::max(tipSp, len(groom.P[F.g[q]].back() - groom.P[F.g[(q + 1) % 3]].back()));
            det["root_spacing_mm"] = std::round(rootSpOf(F) * 1000 * 10) / 10;
            det["tip_spread_mm"] = std::round(tipSp * 1000);
            if (faceDivergent(groom, F, rootSpOf(F))) causes.push_back("divergent face fan (tips " + std::to_string(int(tipSp * 1000)) + " mm apart)");
            for (int q = 0; q < 3; q++) {
                json d2;
                auto c2 = guideCauses(F.g[q], d2);
                if (c2.size() == 1 && c2[0].rfind("other", 0) == 0) continue;
                for (auto& s : c2) causes.push_back("guide " + std::to_string(F.g[q]) + ": " + s);
            }
            if (causes.empty()) causes.push_back("other (interpolated hair between its guides flies; no fan / guide defect)");
            det["length_mm"] = std::round(glen[g0] * 1000);
        }
        e["length_mm"] = det.value("length_mm", 0.0);
        e["cause"] = causes[0];
        e["causes"] = causes;
        e["detail"] = det;
    };
    // every candidate is a culprit (its own simulated strand / face hair measurably strays); the elimination gives the
    // verdict: clears (hiding it removes its out-of-place hair), clump (only together with others), replaced (hiding it
    // lets neighbours fly instead: hair interaction), not cleared (other hair still goes there), untested
    json culprits = json::array(), groups = json::array();
    windRows.clear(); windGuides.clear(); windFaces.clear();
    std::vector<std::string> verdict(items.size(), confirm ? "not tested" : "not confirmed (confirm off)");
    std::vector<int> groupOf(items.size(), -1);
    std::vector<float> remOf(items.size(), NAN);
    std::vector<json> sideOf(items.size());
    for (size_t k = 0; k < items.size(); k++) { remOf[k] = itemRem[k]; sideOf[k] = itemSide[k]; }
    for (auto& F0 : found) {
        if (!F0.group) {
            int k = F0.grp[0];
            remOf[k] = F0.t.removal[k];
            verdict[k] = remOf[k] >= 0.5f ? "clears" : "reduces";
            continue;
        }
        int gid = (int)groups.size();
        json mem = json::array();
        float best = 0, mpx = 0;
        std::map<std::string, int> causeN;
        for (int k : F0.grp) {
            verdict[k] = "clump";
            groupOf[k] = gid;
            remOf[k] = F0.t.removal[k];
            best = std::max(best, F0.t.removal[k]);
            mem.push_back({{"kind", items[k].kind ? "face" : "guide"}, {"id", items[k].id}});
        }
        groups.push_back({{"group", gid}, {"members", mem}, {"size", mem.size()}, {"max_removal", std::round(best * 1000) / 1000},
                          {"others_fly_farther", F0.t.side.is_null() ? json::array() : F0.t.side},
                          {"note", "hiding them together removes their out-of-place hair; no half of them alone does"}});
    }
    for (size_t k = 0; k < items.size(); k++) {
        if (verdict[k] == "clears" || verdict[k] == "reduces" || verdict[k] == "clump") continue;
        if (items[k].base < minFoot) verdict[k] = "untested (its out-of-place hair is hidden / too small in every camera)";
        else if (!std::isnan(remOf[k])) {
            if (!sideOf[k].is_null() && !sideOf[k].empty()) verdict[k] = "replaced (hiding it lets other guides fly farther)";
            else verdict[k] = "not cleared (other hair still reaches its place)";
        }
    }
    std::vector<int> ordK(items.size());
    for (size_t k = 0; k < items.size(); k++) ordK[k] = int(k);
    std::stable_sort(ordK.begin(), ordK.end(), [&](int a, int b) { return items[a].score > items[b].score; });
    std::vector<std::vector<std::string>> groupCauses(groups.size());
    for (int k : ordK) {
        json e;
        std::vector<std::string> cs;
        itemJson(k, e, cs);
        e["verdict"] = verdict[k];
        e["confirmed"] = verdict[k] == "clears" || verdict[k] == "reduces" || verdict[k] == "clump";
        if (groupOf[k] >= 0) e["group"] = groupOf[k];
        if (!std::isnan(remOf[k])) {
            e["removal"] = std::round(remOf[k] * 1000) / 1000;
            e["removal_needed"] = std::round(std::min(need(k), 1.0f) * 1000) / 1000;
        }
        if (!sideOf[k].is_null() && !sideOf[k].empty()) e["others_fly_farther"] = sideOf[k];
        if (groupOf[k] >= 0) {
            groups[groupOf[k]]["members_detail"].push_back(e);
            groupCauses[groupOf[k]].push_back(cs[0]);
        }
        culprits.push_back(e);
        // UI row
        WindRow row;
        row.kind = e["kind"];
        row.id = e["id"];
        row.guides = e["guides"].get<std::vector<int>>();
        row.confirmed = e["confirmed"];
        std::string camsS;
        for (auto& n : e["cameras"]) camsS += (camsS.empty() ? "" : ", ") + n.get<std::string>();
        std::string vd = verdict[k];
        if (groupOf[k] >= 0) vd = "clump " + std::to_string(groupOf[k] + 1);
        if (!std::isnan(remOf[k])) { char b[48]; snprintf(b, sizeof b, " (%.0f %% removed)", remOf[k] * 100); vd += b; }
        char t2[400];
        snprintf(t2, sizeof t2, "%s %d: %.0f px / %.0f mm out, %.0f px beyond its neighbours [%s] - %s - %s", row.kind.c_str(), row.id,
                 e["max_px"].get<double>(), e["max_mm"].get<double>(), items[k].score, camsS.c_str(), vd.c_str(), e["cause"].get<std::string>().c_str());
        row.text = t2;
        windRows.push_back(row);
        if (row.confirmed) {   // Select / Delete culprits: the confirmed ones
            if (row.kind == "guide") windGuides.insert(row.id);
            else windFaces.insert(row.id);
        }
    }
    for (size_t gi = 0; gi < groups.size(); gi++) {
        std::map<std::string, int> n;
        for (auto& c : groupCauses[gi]) n[c.substr(0, c.find(" ("))]++;
        std::string s2;
        for (auto& [c, m] : n) s2 += (s2.empty() ? "" : ", ") + c + " x" + std::to_string(m);
        groups[gi]["causes"] = s2;
    }
    json unconf = json::array();   // (kept for the report layout: ids of the candidates not confirmed)
    for (int k : ordK) if (verdict[k] != "clears" && verdict[k] != "reduces" && verdict[k] != "clump") unconf.push_back((items[k].kind ? "face " : "guide ") + std::to_string(items[k].id));

    // ---- debug PNGs: rest outline (white), excursion heat (yellow -> red above the threshold, blue below), culprit CVs (green)
    json pngList = json::array();
    if (pngs) {
        for (int c = 0; c < C; c++) {
            std::vector<unsigned char> img(size_t(S) * S * 4, 0);
            const auto& rest = cams[c].rest;
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++) {
                    size_t i = size_t(y) * S + x;
                    unsigned char* q = &img[i * 4];
                    q[3] = 255;
                    if (cams[c].zbuf[i] < 1e8f) { q[0] = q[1] = q[2] = 22; }
                    if (rest[i]) {
                        bool edge = false;
                        for (int d = 0; d < 4 && !edge; d++) {
                            int xx = x + (d == 0) - (d == 1), yy = y + (d == 2) - (d == 3);
                            if (xx < 0 || yy < 0 || xx >= S || yy >= S || !rest[size_t(yy) * S + xx]) edge = true;
                        }
                        if (edge) { q[0] = q[1] = q[2] = 255; } else { q[0] = q[1] = q[2] = 60; }
                    }
                    float v = A.pixMax[c][i];
                    if (v > thr) {
                        float u = std::clamp((v - thr) / (3 * thr), 0.0f, 1.0f);
                        q[0] = 255; q[1] = (unsigned char)(230 * (1 - u)); q[2] = 0;
                    } else if (v > 0) { q[0] = 40; q[1] = 90; q[2] = 200; }
                }
            std::vector<const std::vector<V3>*> worst;
            for (int g : windGuides) if (g < (int)NG && !A.gWorst[g].empty()) worst.push_back(&A.gWorst[g]);
            for (int f : windFaces) if (f < (int)NF && !fWorst[f].empty()) worst.push_back(&fWorst[f]);
            for (auto* Wp : worst) {
                const auto& W = *Wp;
                Proj pa = wproj(cams[c], float(S), W[0]);
                for (size_t j = 1; j < W.size(); j++) {
                    Proj pb = wproj(cams[c], float(S), W[j]);
                    int n = int(std::max(std::fabs(pb.x - pa.x), std::fabs(pb.y - pa.y))) + 1;
                    for (int s2 = 0; s2 <= n && n < 4000; s2++) {
                        float tt = float(s2) / n;
                        int x = int(pa.x + (pb.x - pa.x) * tt), y = int(pa.y + (pb.y - pa.y) * tt);
                        if (x < 0 || y < 0 || x >= S || y >= S) continue;
                        unsigned char* q = &img[(size_t(y) * S + x) * 4];
                        q[0] = 30; q[1] = 255; q[2] = 60;
                    }
                    pa = pb;
                }
            }
            std::string p = out + "\\windcheck_" + configName + "_" + cams[c].name + ".png";
            if (writePng(p, S, S, img.data())) pngList.push_back(p);
        }
    }

    json r;
    r["params"] = {{"seconds", seconds}, {"frames", nFrames}, {"steps", nSteps}, {"size", S}, {"threshold_px", thr}, {"amount", amount},
                   {"min_cluster_px", minCluster}, {"min_removal", minFrac}, {"max_runs", maxRuns}, {"cameras", C}, {"wind_noise", windNoise}, {"fur_wind_noise", furNoise}, {"ramp_s", ramp}, {"repeats", reps}, {"runs_per_test", half},
                   {"stray_px", strayMin}, {"max_candidates", maxCand}};
    r["readback"] = readInfo;
    r["rest"] = restCams;
    if (reps >= 2) r["repeatability"] = rep;
    r["cameras"] = camsR;
    r["flyers"] = flyers;
    r["strays"] = strays;
    if (P.contains("inspect") && P["inspect"].is_array()) {   // per-guide numbers of the guides asked for
        json ins = json::array();
        for (auto& v : P["inspect"]) {
            if (!v.is_number_integer()) continue;
            int g = v.get<int>();
            if (g < 0 || g >= (int)NG) continue;
            json e = {{"guide", g}, {"alive", bool(groom.alive[g])}};
            if (groom.alive[g]) {
                json pc = json::object();
                for (int c = 0; c < C; c++) pc[cams[c].name] = std::round(A.gPx[c][g] * 10) / 10;
                e["px_per_camera"] = pc;
                e["max_px"] = std::round(A.gBestPx[g] * 10) / 10;
                e["stray_px"] = std::round(strayScore[g] * 10) / 10;
                e["rise_mm"] = std::round(A.gRise[g] * 1000);
                e["disp_mm"] = std::round(A.gDisp[g] * 1000);
                e["root"] = {r4(groom.P[g][0].x), r4(groom.P[g][0].y), r4(groom.P[g][0].z)};
                if (doCalm) e["calm_px"] = std::round(Calm.gBestPx[g] * 10) / 10;
                json det;
                e["causes"] = guideCauses(g, det);
                e["detail"] = det;
                json fl = json::array();   // its faces whose interpolated hair strays
                for (size_t f = 0; f < NF; f++) {
                    const Face& F = groom.faces[f];
                    if ((F.g[0] == g || F.g[1] == g || F.g[2] == g) && fStray[f] > 0)
                        fl.push_back({{"face", f}, {"guides", {F.g[0], F.g[1], F.g[2]}}, {"stray_px", std::round(fStray[f] * 10) / 10}, {"max_px", std::round(fBest[f] * 10) / 10}});
                }
                e["straying_faces"] = fl;
            }
            ins.push_back(e);
        }
        r["inspect"] = ins;
    }
    r["culprits"] = culprits;
    r["groups"] = groups;
    json cand = json::array();
    for (auto& it : items)
        cand.push_back({{"kind", it.kind ? "face" : "guide"}, {"id", it.id}, {"stray_px", std::round(it.score * 10) / 10},
                        {"camera", it.cam >= 0 ? cams[it.cam].name : ""}, {"footprint_px", (int)it.region.size()}, {"flagged_px", it.base}, {"noise_px", it.noise}});
    r["candidates"] = cand;
    r["not_confirmed"] = unconf;
    if (!allTest.is_null()) r["all_candidates_hidden"] = allTest;
    r["tests"] = testsLog;
    r["runs"] = runs;
    r["png"] = pngList;
    r["elapsed_s"] = std::round(std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count() * 10) / 10;
    windYield(1.0f, "done");
    return r;
}
