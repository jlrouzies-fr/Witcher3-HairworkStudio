// Hairwork Studio - path tracing GPU cost estimator (CPU BVH ray cast over the hairs HairWorks would generate). Our own code.
// Path-traced HairWorks (Witcher 3 5.0) renders a fixed 32 interpolated hairs per growth face; cost follows ray traversal
// work over the hair segment BVH. This builds the same hair set roughly, casts rays and counts node visits.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "deskpack.h"

struct PtParams {
    double clump = 1, clumpNoise = 0, width = 1, wRoot = 1, wTip = 1, waveScale = 0, waveStrand = 0, waveFreq = 3;
};
struct PtOptions {
    int rays = 4096;            // per camera
    int stopHits = 1000;          // any-hit: stop after N hits (mimics transparency)
    int clumpModel = 1;         // 0 pull toward nearest guide by (1-clump), 1 scatter by (1-clump), 2 none
    int maxFaces = 1600;        // subsample above this
    double footprint = 0.006;   // minimum ray radius (m), pixel footprint at ~1.8 m
};
struct PtInput {
    std::vector<Guide> guides;
    std::vector<std::array<int, 3>> faces;
    PtParams p;
    PtOptions o;
};
struct PtResult {
    bool valid = false;
    double metric = 0;       // meanVisits * hitFrac * bbox fraction of the frame
    double meanVisits = 0, meanTests = 0, hitFrac = 0, coverage = 0, bboxFrac = 0, aabbArea = 0, hitsPerRay = 0;
    int segments = 0, faces = 0;
    double seconds = 0;
};

PtResult ptEstimate(const PtInput& in, const std::atomic<bool>* cancel = nullptr);
// ms/frame = a + b*metric (desktop_studio/data/pt_calibration.json "fit", built-in fallback)
struct PtCalib { double a = 3.985, b = 0.017807, vesemir = 14.1; std::string key = "mean_visits"; double ms(const PtResult& r) const; };
PtCalib ptLoadCalib(const std::string& repoRoot);
PtParams ptParamsFromHwpack(const json& hw);
int ptCli(const std::string& hwpackPath, const std::string& opts);   // --pt-estimate: prints JSON, returns exit code

// background worker with debounce
struct PtWorker {
    PtWorker();
    ~PtWorker();
    void request(PtInput in);               // replaces any pending request; computed after a short quiet period
    bool get(PtResult& r) const;            // latest finished result
    bool busy() const { return pending_.load() || running_.load(); }
private:
    void loop();
    std::thread th_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    PtInput in_;
    bool has_ = false, quit_ = false;
    std::chrono::steady_clock::time_point stamp_;
    PtResult res_;
    std::atomic<bool> cancel_{false}, pending_{false}, running_{false};
};
