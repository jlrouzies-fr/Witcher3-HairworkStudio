// Hairwork Studio - GPU cost model of moving path-traced hair + the stiffness / hair-count auto-balance. Our own code.
// The model is fitted OFFLINE on measured game rows (fpsrun GPU busy ms, same-launch deltas over Vesemir) by
// tools/hairworks/toolarch/gpu_cost_model.py, which writes desktop_studio/data/gpu_cost_model.json; this file evaluates the
// same formula (keep both in step). It is an ESTIMATE, never a measurement (FINE_STRANDS_GPU.md section 9).
#pragma once
#include <map>
#include <string>
#include <vector>

#include "deskpack.h"   // json

struct GpuFeat {
    double faces = 0, hpp = 32, K = 13, sm = 3, lenM = 0.2;
    double dld = 1, lodOn = 1, keepExtra = 1, density = 1;
    double stiffness = 0.5, root = 0.7, tip = 0.1, bend = 0.22, strength = 1;
    double curve[4] = {1, 1, 1, 1};
    json toJson() const;
    static GpuFeat fromJson(const json& j);
};

struct GpuModel {
    bool loaded = false;
    std::string err;
    double a = 0.2, b = 7, w = 0.07, s0 = 0.12, rho = 0.27, q = 0.74, kb = 0, kt = 1.5, g = 1, Lcap = 2, keepT = 0.8;
    bool useLen = false;
    std::map<std::string, double> kappa;   // per measured character: motion scale from its own rows
    GpuFeat ves;                           // Vesemir vesemir_l260_aio (the reference of every launch)
    double errAbsAnch = 2, errRelAnch = 0.3, errAbsGlob = 3, errRelGlob = 0.5;
    int fitRows = 0;
    std::vector<std::string> noData;
    double drawnHairs(const GpuFeat& f) const;
    double hold(const GpuFeat& f) const;
    double phi(const GpuFeat& f, double kappa) const;
};
GpuModel gpuLoadModel(const std::string& repoRoot);

struct GpuEstimate {
    bool valid = false;
    double extra = 0, err = 0, render = 0, motion = 0, kappa = 1, hold = 0, hairs = 0;
    bool anchored = false;               // the character has measured rows (its own motion scale)
    std::vector<std::string> bad;        // measured-bad physics present
    int colour = 0;                      // 0 green (fits the budget), 1 amber (over, within 1.5x), 2 red (over 1.5x or bad zone)
};
GpuEstimate gpuEstimate(const GpuModel& m, const GpuFeat& f, const std::string& character, double budget);
std::vector<std::string> gpuBadZone(const GpuFeat& f);

// auto-balance table (UI + MCP): one row per stiffness option
struct GbRow {
    double stiffness = 0.5, tip = 0.1, strength = 1;
    double keep = 1;                     // share of the current drawn hairs that fits the budget
    double hairs = 0, extraFull = 0, extra = 0, err = 0;
    bool fits = false, bad = false, current = false, custom = false, recommended = false;
    bool noData = false;                 // stiffness below .35: no measured row there (Geralt's knee lies between .35 and .25)
    std::vector<std::string> badWhy;
    double motionMm = -1, tipMm = -1, stuckPct = -1;   // Studio wind simulation (-1 = not measured yet)
};
struct GpuBudget {
    float budget = 6.0f;                 // ms over Vesemir (user-editable)
    float minKeep = 0.5f;                // the recommended row keeps at least this share of the hairs
    float customStiff = 0.4f;
    int route = 0;                       // 0 close-up LOD density (detailLODDensity), 1 density (flat density map at export)
    bool open = false;
    std::vector<GbRow> rows;
    int measuring = -1;                  // UI: next row whose motion is measured (one per frame), -1 = idle
    bool inside = false;
    struct Undo { std::map<std::string, json> over; size_t ops = 0, colUndo = 0; std::string what; };
    std::vector<Undo> undo;
    std::string lastApplied;
};
