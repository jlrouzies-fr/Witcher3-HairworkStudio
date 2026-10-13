// Path tracing GPU cost estimator, see pt_estimate.h. Our own code.
#include "pt_estimate.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace {
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) { next(); next(); }
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    float f() { return float(next() >> 40) / float(1 << 24); }
};
struct Seg { V3 a, b; float r; };
struct Box { V3 lo, hi; };
struct Node { Box bb; int l = -1, r = -1, first = 0, count = 0; };

inline V3 vmin(V3 a, V3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline V3 vmax(V3 a, V3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline V3 norm(V3 a) { float l = std::sqrt(dot(a, a)); return l > 1e-12f ? a * (1.0f / l) : a; }
inline float area(const Box& b) { V3 d = b.hi - b.lo; return 2 * (d.x * d.y + d.y * d.z + d.z * d.x); }

struct Bvh {
    std::vector<Seg> segs;
    std::vector<Node> nodes;
    std::vector<int> idx;
    Box segBox(int i) const {
        const Seg& s = segs[i];
        V3 r{s.r, s.r, s.r};
        return {vmin(s.a, s.b) - r, vmax(s.a, s.b) + r};
    }
    int build(int lo, int hi, std::vector<Box>& bx, std::vector<V3>& ct) {
        int n = (int)nodes.size();
        nodes.push_back({});
        Box bb = bx[idx[lo]];
        V3 clo = ct[idx[lo]], chi = clo;
        for (int i = lo + 1; i < hi; i++) {
            bb.lo = vmin(bb.lo, bx[idx[i]].lo); bb.hi = vmax(bb.hi, bx[idx[i]].hi);
            clo = vmin(clo, ct[idx[i]]); chi = vmax(chi, ct[idx[i]]);
        }
        nodes[n].bb = bb;
        if (hi - lo <= 4) { nodes[n].first = lo; nodes[n].count = hi - lo; return n; }
        V3 e = chi - clo;
        int ax = (e.x > e.y && e.x > e.z) ? 0 : (e.y > e.z ? 1 : 2);
        auto key = [&](int i) { return ax == 0 ? ct[i].x : ax == 1 ? ct[i].y : ct[i].z; };
        int mid = (lo + hi) / 2;
        std::nth_element(idx.begin() + lo, idx.begin() + mid, idx.begin() + hi, [&](int a, int b) { return key(a) < key(b); });
        int l = build(lo, mid, bx, ct);
        int r = build(mid, hi, bx, ct);
        nodes[n].l = l; nodes[n].r = r;
        return n;
    }
    void build() {
        int n = (int)segs.size();
        std::vector<Box> bx(n);
        std::vector<V3> ct(n);
        idx.resize(n);
        for (int i = 0; i < n; i++) { bx[i] = segBox(i); ct[i] = (segs[i].a + segs[i].b) * 0.5f; idx[i] = i; }
        nodes.clear(); nodes.reserve(n / 2 + 8);
        if (n) build(0, n, bx, ct);
    }
};

inline bool slab(const Box& b, V3 o, V3 inv) {
    float t0 = 0, t1 = 1e9f;
    float lo[3] = {b.lo.x, b.lo.y, b.lo.z}, hi[3] = {b.hi.x, b.hi.y, b.hi.z}, oo[3] = {o.x, o.y, o.z}, ii[3] = {inv.x, inv.y, inv.z};
    for (int k = 0; k < 3; k++) {
        float a = (lo[k] - oo[k]) * ii[k], c = (hi[k] - oo[k]) * ii[k];
        if (a > c) std::swap(a, c);
        t0 = std::max(t0, a); t1 = std::min(t1, c);
        if (t0 > t1) return false;
    }
    return true;
}
// ray (o, unit d, t>0) vs capsule: distance between ray and segment <= r
inline bool hitSeg(const Seg& s, V3 o, V3 d) {
    V3 u = s.b - s.a, w = s.a - o;
    float uu = dot(u, u), ud = dot(u, d), wd = dot(w, d), wu = dot(w, u);
    float den = uu - ud * ud;
    float sc = den > 1e-14f ? (ud * wd - wu) / den : 0.0f;
    sc = std::min(1.0f, std::max(0.0f, sc));
    V3 p = w + u * sc;
    float t = std::max(0.0f, dot(p, d));
    V3 q = p - d * t;
    return dot(q, q) <= s.r * s.r;
}
}  // namespace

PtResult ptEstimate(const PtInput& in, const std::atomic<bool>* cancel) {
    auto t0 = std::chrono::steady_clock::now();
    PtResult R;
    const PtOptions& O = in.o;
    const PtParams& P = in.p;
    if (in.faces.empty() || in.guides.empty()) return R;
    int M = 2;
    for (auto& g : in.guides) M = std::max(M, (int)g.size());
    M = std::min(M, 24);
    std::vector<std::vector<V3>> G(in.guides.size());
    for (size_t i = 0; i < G.size(); i++) {
        const Guide& g = in.guides[i];
        if (g.size() < 2) continue;
        G[i].resize(M);
        for (int k = 0; k < M; k++) {
            float s = float(k) / (M - 1) * (g.size() - 1);
            int a = std::min((int)s, (int)g.size() - 2);
            float f = s - a;
            G[i][k] = g[a] * (1 - f) + g[a + 1] * f;
        }
    }
    int nf = (int)in.faces.size();
    int stride = std::max(1, (nf + O.maxFaces - 1) / O.maxFaces);
    Bvh bvh;
    bvh.segs.reserve(size_t(nf / stride + 1) * 32 * (M - 1));
    V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    double pull0 = std::max(0.0, 1.0 - P.clump);
    for (int fi = 0; fi < nf; fi += stride) {
        const auto& f = in.faces[fi];
        if (f[0] < 0 || f[1] < 0 || f[2] < 0 || f[0] >= (int)G.size() || f[1] >= (int)G.size() || f[2] >= (int)G.size()) continue;
        const auto &g0 = G[f[0]], &g1 = G[f[1]], &g2 = G[f[2]];
        if (g0.empty() || g1.empty() || g2.empty()) continue;
        Rng rng(fi + 1);
        R.faces++;
        for (int h = 0; h < 32; h++) {
            float r1 = std::sqrt(rng.f()), r2 = rng.f();
            float b0 = 1 - r1, b1 = r1 * (1 - r2), b2 = r1 * r2;
            const auto* gn = (b0 >= b1 && b0 >= b2) ? &g0 : (b1 >= b2 ? &g1 : &g2);
            float nz = float(1.0 - P.clumpNoise * rng.f());
            V3 sc = norm(V3{rng.f() - 0.5f, rng.f() - 0.5f, rng.f() - 0.5f});
            V3 wv = norm(V3{rng.f() - 0.5f, rng.f() - 0.5f, rng.f() - 0.5f});
            float ph = rng.f() * 6.2831853f;
            V3 prev{};
            for (int k = 0; k < M; k++) {
                float t = float(k) / (M - 1);
                V3 p = g0[k] * b0 + g1[k] * b1 + g2[k] * b2;
                if (O.clumpModel == 0) {
                    float pull = std::min(1.0f, float(pull0) * t * nz);
                    p = p * (1 - pull) + (*gn)[k] * pull;
                } else if (O.clumpModel == 1) {
                    p = p + sc * float(pull0 * 0.02 * t * nz);
                }
                if (P.waveScale > 0 && P.waveStrand > 0)
                    p = p + wv * float(P.waveScale * P.waveStrand * 0.01 * t * std::sin(6.2831853 * P.waveFreq * t + ph));
                if (k > 0) {
                    float wd = float(P.width * (P.wRoot + (P.wTip - P.wRoot) * t) * 0.005);
                    float r = std::max(wd * 0.5f, float(O.footprint));
                    bvh.segs.push_back({prev, p, r});
                }
                prev = p;
                lo = vmin(lo, p); hi = vmax(hi, p);
            }
        }
        if (cancel && cancel->load()) return R;
    }
    R.segments = (int)bvh.segs.size();
    if (!R.segments) return R;
    bvh.build();
    R.aabbArea = 0;
    for (size_t i = 0; i < bvh.segs.size(); i += 7) R.aabbArea += area(bvh.segBox((int)i));
    R.aabbArea *= 7.0 * stride;
    // 3 cameras at 1.8 m aimed at the hair bbox centre
    V3 c = (lo + hi) * 0.5f;
    const float dist = 1.8f, tanV = 0.57735f, tanH = tanV * 16.0f / 9.0f;
    const float ang[3] = {0.0f, 1.5707963f, 0.7853982f};   // side, front, 3/4 around the up axis (z)
    double sumVis = 0, sumTest = 0, sumHitFrac = 0, sumCov = 0, sumBox = 0, sumHPR = 0, sumMetric = 0;
    for (int cam = 0; cam < 3; cam++) {
        V3 dir = norm(V3{std::cos(ang[cam]), std::sin(ang[cam]), 0.15f});
        V3 o = c + dir * dist, fwd = dir * -1.0f;
        V3 right = norm(cross(fwd, V3{0, 0, 1})), up = cross(right, fwd);
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int k = 0; k < 8; k++) {
            V3 q{(k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z};
            V3 v = q - o;
            float z = std::max(dot(v, fwd), 0.05f);
            float tx = dot(v, right) / z, ty = dot(v, up) / z;
            x0 = std::min(x0, tx); x1 = std::max(x1, tx); y0 = std::min(y0, ty); y1 = std::max(y1, ty);
        }
        double boxFrac = std::min(1.0, double(x1 - x0) * (y1 - y0) / (4.0 * tanV * tanH));
        Rng rr(7 + cam);
        long long vis = 0, tests = 0, hitRays = 0, hits = 0;
        std::vector<int> stack;
        stack.reserve(64);
        for (int ri = 0; ri < O.rays; ri++) {
            float tx = x0 + (x1 - x0) * rr.f(), ty = y0 + (y1 - y0) * rr.f();
            V3 d = norm(fwd + right * tx + up * ty);
            V3 inv{std::abs(d.x) > 1e-9f ? 1 / d.x : 1e9f, std::abs(d.y) > 1e-9f ? 1 / d.y : 1e9f, std::abs(d.z) > 1e-9f ? 1 / d.z : 1e9f};
            int nh = 0;
            stack.clear(); stack.push_back(0);
            while (!stack.empty() && nh < O.stopHits) {
                int ni = stack.back(); stack.pop_back();
                const Node& n = bvh.nodes[ni];
                vis++;
                if (!slab(n.bb, o, inv)) continue;
                if (n.l < 0) {
                    for (int i = 0; i < n.count; i++) {
                        tests++;
                        if (hitSeg(bvh.segs[bvh.idx[n.first + i]], o, d)) { nh++; if (nh >= O.stopHits) break; }
                    }
                } else { stack.push_back(n.l); stack.push_back(n.r); }
            }
            if (nh) hitRays++;
            hits += nh;
        }
        double mv = double(vis) / O.rays, hf = double(hitRays) / O.rays;
        sumVis += mv; sumTest += double(tests) / O.rays; sumHitFrac += hf; sumBox += boxFrac; sumCov += hf * boxFrac;
        sumHPR += double(hits) / O.rays; sumMetric += mv * hf * boxFrac;
    }
    R.meanVisits = sumVis / 3; R.meanTests = sumTest / 3; R.hitFrac = sumHitFrac / 3; R.bboxFrac = sumBox / 3; R.coverage = sumCov / 3;
    R.hitsPerRay = sumHPR / 3;
    R.metric = sumMetric / 3;
    R.valid = true;
    R.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return R;
}

PtParams ptParamsFromHwpack(const json& hw) {
    PtParams p;
    const json& ph = hw.contains("physical") ? hw["physical"] : json::object();
    auto g = [&](const char* grp, const char* k, double d) {
        if (ph.contains(grp) && ph[grp].is_object() && ph[grp].contains(k) && ph[grp][k].is_number()) return ph[grp][k].get<double>();
        return d;
    };
    p.clump = g("clumping", "clumpScale", 1); p.clumpNoise = g("clumping", "clumpNoise", 0);
    p.width = g("strandWidth", "width", 1); p.wRoot = g("strandWidth", "widthRootScale", 1); p.wTip = g("strandWidth", "widthTipScale", 1);
    p.waveScale = g("waveness", "waveScale", 0); p.waveStrand = g("waveness", "waveStrand", 0); p.waveFreq = g("waveness", "waveFreq", 3);
    return p;
}

double PtCalib::ms(const PtResult& r) const {
    double m = key == "mean_visits" ? r.meanVisits : key == "mean_tests" ? r.meanTests : key == "hits_per_ray" ? r.hitsPerRay
             : key == "coverage" ? r.coverage : key == "aabb_area" ? r.aabbArea : key == "faces" ? r.faces : key == "segments" ? r.segments : r.metric;
    return a + b * m;
}

PtCalib ptLoadCalib(const std::string& repoRoot) {
    PtCalib c;
    std::ifstream f(repoRoot + "\\desktop_studio\\data\\pt_calibration.json");
    if (!f) return c;
    json j = json::parse(f, nullptr, false);
    if (j.is_object() && j.contains("fit") && j["fit"].is_object()) {
        c.a = j["fit"].value("a", c.a);
        c.b = j["fit"].value("b", c.b);
        c.vesemir = j.value("vesemir_ms", c.vesemir);
        if (j.contains("variant") && j["variant"].is_object()) c.key = j["variant"].value("metric_key", c.key);
    }
    return c;
}

int ptCli(const std::string& path, const std::string& opts) {
    std::ifstream f(path);
    if (!f) { printf("{\"error\":\"cannot open file\"}\n"); return 2; }
    json hw = json::parse(f, nullptr, false);
    if (!hw.is_object() || !hw.contains("guides") || !hw.contains("F")) { printf("{\"error\":\"not a hwpack\"}\n"); return 2; }
    PtInput in;
    for (auto& g : hw["guides"]) {
        Guide gg;
        for (auto& p : g) gg.push_back({p[0].get<float>(), p[1].get<float>(), p[2].get<float>()});
        in.guides.push_back(gg);
    }
    for (auto& t : hw["F"]) in.faces.push_back({t[0].get<int>(), t[1].get<int>(), t[2].get<int>()});
    in.p = ptParamsFromHwpack(hw);
    size_t i = 0;
    while (i < opts.size()) {
        size_t e = opts.find(',', i);
        if (e == std::string::npos) e = opts.size();
        std::string kv = opts.substr(i, e - i);
        i = e + 1;
        size_t q = kv.find('=');
        if (q == std::string::npos) continue;
        std::string k = kv.substr(0, q);
        double v = atof(kv.c_str() + q + 1);
        if (k == "stop") in.o.stopHits = (int)v;
        else if (k == "model") in.o.clumpModel = (int)v;
        else if (k == "rays") in.o.rays = (int)v;
        else if (k == "foot") in.o.footprint = v;
        else if (k == "maxfaces") in.o.maxFaces = (int)v;
    }
    PtResult r = ptEstimate(in);
    json o = {{"metric", r.metric}, {"mean_visits", r.meanVisits}, {"mean_tests", r.meanTests}, {"hit_frac", r.hitFrac},
              {"coverage", r.coverage}, {"bbox_frac", r.bboxFrac}, {"aabb_area", r.aabbArea}, {"hits_per_ray", r.hitsPerRay},
              {"segments", r.segments}, {"faces", r.faces}, {"seconds", r.seconds}, {"clump", in.p.clump}, {"valid", r.valid}};
    printf("%s\n", o.dump().c_str());
    fflush(stdout);
    return r.valid ? 0 : 1;
}

PtWorker::PtWorker() { th_ = std::thread([this] { loop(); }); }
PtWorker::~PtWorker() {
    {
        std::lock_guard<std::mutex> l(m_);
        quit_ = true;
    }
    cancel_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}
void PtWorker::request(PtInput in) {
    {
        std::lock_guard<std::mutex> l(m_);
        in_ = std::move(in);
        has_ = true;
        stamp_ = std::chrono::steady_clock::now();
    }
    pending_ = true;
    cancel_ = true;
    cv_.notify_all();
}
bool PtWorker::get(PtResult& r) const {
    std::lock_guard<std::mutex> l(m_);
    r = res_;
    return res_.valid;
}
void PtWorker::loop() {
    std::unique_lock<std::mutex> l(m_);
    while (!quit_) {
        if (!has_) { cv_.wait(l); continue; }
        auto due = stamp_ + std::chrono::milliseconds(250);
        if (std::chrono::steady_clock::now() < due) { cv_.wait_until(l, due); continue; }
        PtInput in = std::move(in_);
        has_ = false;
        cancel_ = false;
        running_ = true;
        l.unlock();
        PtResult r = ptEstimate(in, &cancel_);
        l.lock();
        running_ = false;
        if (!has_) pending_ = false;
        if (r.valid && !cancel_.load()) res_ = r;
    }
}
