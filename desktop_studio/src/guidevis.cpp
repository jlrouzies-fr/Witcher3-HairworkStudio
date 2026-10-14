// Desktop Groom Studio - guide visibility views (2026-10-14, user: "only show guides facing the camera (like first
// layer), only show guides not hidden (behind the body geometry)"). Our own code (MIT).
//
// A CPU pass on small software depth buffers (longest side kVisRes px), split over worker threads:
//   body : the posed non-alpha meshes (head, body, outfit; not the alpha beard / hair cards), perspective-correct depth
//   hair : every live guide segment splatted as a thick cone-shaped line (kSplatMm radius, depth growing with the
//          lateral distance so a neighbour of the same layer a few mm to the side does not hide a guide)
// "Hide behind the body": a guide point is drawn when it is not behind the body depth (tolerance 4 mm + 2 px); only
// segments with both ends visible are drawn.
// "Front layer only": a point is front-most when its depth is within guideTolMm of min(body, hair) at its pixel. A
// guide is shown when at least guideFrontPct % of its points that the body does not hide (root excluded, >= 2) are
// front-most, and is then drawn whole (clipped by the body test when both modes are on). No root-normal pre-filter:
// long back hair brought over the shoulder faces the camera although its root faces away; the depth test alone is
// right in both cases.
// Picking (click / box / brush, comb / curl / paint brushes) uses the same flags when "Select visible only" is on.
//
// Viewport frames run the pass on a background worker (the result is one or two frames behind while the camera or
// the pose moves; the frame itself never waits). Off-screen renders and serve / agent calls compute it inline.
// Unchanged inputs (camera, pose, guides, body, settings: one hash) reuse the last result.
#include "app.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

using namespace DirectX;

namespace {
constexpr int kVisRes = 384;          // longest side of the visibility buffers (px)
constexpr float kSplatMm = 2.5f;      // world radius of a guide's splat in the hair buffer
constexpr float kConeSlope = 1.5f;    // splat depth growth (m per lateral m)
constexpr float kNear = 0.02f;

using clk = std::chrono::steady_clock;
float msSince(clk::time_point a) { return std::chrono::duration<float, std::milli>(clk::now() - a).count(); }

int threadCount() { return std::clamp(int(std::thread::hardware_concurrency()) - 2, 1, 8); }

// fn(thread, begin, end) over [0, n) on T threads (the caller's thread is one of them)
void parallel(int T, int n, const std::function<void(int, int, int)>& fn) {
    std::vector<std::thread> th;
    int per = (n + T - 1) / T;
    for (int t = 1; t < T; t++) th.emplace_back(fn, t, std::min(n, t * per), std::min(n, (t + 1) * per));
    fn(0, 0, std::min(n, per));
    for (auto& x : th) x.join();
}

struct Buf {
    int w = 0, h = 0;
    std::vector<float> z;   // view depth (m), 1e30 = empty
    void reset(int W, int H) { w = W; h = H; z.assign(size_t(W) * H, 1e30f); }
    float at(float x, float y) const {
        int ix = int(x), iy = int(y);
        if (ix < 0 || iy < 0 || ix >= w || iy >= h) return 1e30f;
        return z[size_t(iy) * w + ix];
    }
};

struct Pt { float x, y, d; };   // buffer pixel coordinates + view depth

struct Projector {
    XMMATRIX V, P;
    float W, H;
    bool operator()(float px, float py, float pz, Pt& o) const {
        XMVECTOR v = XMVector3TransformCoord(XMVectorSet(px, py, pz, 1), V);
        float d = -XMVectorGetZ(v);   // right-handed view: forward = -z
        if (d < kNear) return false;
        XMVECTOR c = XMVector4Transform(XMVectorSetW(v, 1), P);
        float w = XMVectorGetW(c);
        if (w <= 1e-6f) return false;
        o.x = (XMVectorGetX(c) / w * 0.5f + 0.5f) * W;
        o.y = (1.0f - (XMVectorGetY(c) / w * 0.5f + 0.5f)) * H;
        o.d = d;
        return true;
    }
};

// inclusive edge test (watertight: a pixel centre on a shared edge is covered by both triangles), 1/z interpolation
void rasterTri(Buf& b, const Pt& a, const Pt& p, const Pt& q) {
    float x0 = std::min({a.x, p.x, q.x}), x1 = std::max({a.x, p.x, q.x}), y0 = std::min({a.y, p.y, q.y}), y1 = std::max({a.y, p.y, q.y});
    int ix0 = std::max(0, int(std::ceil(x0 - 0.5f))), ix1 = std::min(b.w - 1, int(std::floor(x1 - 0.5f)));
    int iy0 = std::max(0, int(std::ceil(y0 - 0.5f))), iy1 = std::min(b.h - 1, int(std::floor(y1 - 0.5f)));
    if (ix0 > ix1 || iy0 > iy1) return;
    float area = (p.x - a.x) * (q.y - a.y) - (p.y - a.y) * (q.x - a.x);
    if (std::fabs(area) < 1e-9f) return;
    float ia = 1.0f / area, za = 1.0f / a.d, zp = 1.0f / p.d, zq = 1.0f / q.d;
    for (int y = iy0; y <= iy1; y++) {
        float cy = y + 0.5f;
        for (int x = ix0; x <= ix1; x++) {
            float cx = x + 0.5f;
            float w0 = ((p.x - cx) * (q.y - cy) - (p.y - cy) * (q.x - cx)) * ia;
            float w1 = ((q.x - cx) * (a.y - cy) - (q.y - cy) * (a.x - cx)) * ia;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f) continue;
            float d = 1.0f / (w0 * za + w1 * zp + w2 * zq);
            float& o = b.z[size_t(y) * b.w + x];
            if (d < o) o = d;
        }
    }
}

struct VisIn {
    XMFLOAT4X4 V, P;
    int w = 0, h = 0, N = 0, K = 0;
    bool front = false, body = false;
    float pct = 50, tol = 5;
    uint64_t key = 0;
    std::vector<char> alive;
    std::vector<V3> pts;
    std::vector<GVertex> own[2];                 // body triangles owned by the job (worker)
    const std::vector<GVertex>* tri[2] = {nullptr, nullptr};
};

struct VisOut {
    uint64_t key = 0;
    int N = 0, K = 0, nShown = 0, nTri = 0, res = 0;
    float ms = 0, msBody = 0, msHair = 0;
    std::vector<uint8_t> pt, shown;
};

void runVis(const VisIn& in, VisOut& out) {
    auto t00 = clk::now();
    const int N = in.N, K = in.K, T = threadCount();
    float s = float(kVisRes) / float(std::max(in.w, in.h));
    int W = std::max(16, int(in.w * s + 0.5f)), H = std::max(16, int(in.h * s + 0.5f));
    Projector pr{XMLoadFloat4x4(&in.V), XMLoadFloat4x4(&in.P), float(W), float(H)};
    float cot = std::max(in.P._22, 1e-4f);
    auto pxWorld = [&](float d) { return d * 2.0f / (cot * float(H)); };   // metres per buffer pixel at depth d
    std::vector<Pt> sp(size_t(N) * K);
    std::vector<char> ok(size_t(N) * K, 0);
    parallel(T, N, [&](int, int b, int e) {
        for (int i = b; i < e; i++)
            if (in.alive[i])
                for (int k = 0; k < K; k++) {
                    const V3& p = in.pts[size_t(i) * K + k];
                    ok[size_t(i) * K + k] = pr(p.x, p.y, p.z, sp[size_t(i) * K + k]);
                }
    });
    // body depth + hair splats: each thread its share of the triangles and guides in its own buffers, then a min merge
    const std::vector<GVertex>* t0 = in.tri[0], * t1 = in.tri[1];
    size_t n0 = t0 ? t0->size() / 3 : 0, nTri = n0 + (t1 ? t1->size() / 3 : 0);
    std::vector<Buf> bodyB(T), hairB(T);
    std::vector<float> tb(T, 0), th(T, 0);
    parallel(T, T, [&](int t, int, int) {
        auto a0 = clk::now();
        Buf& b = bodyB[t];
        b.reset(W, H);
        size_t per = (nTri + T - 1) / T, q0 = std::min(nTri, t * per), q1 = std::min(nTri, (t + 1) * per);
        for (size_t q = q0; q < q1; q++) {
            const std::vector<GVertex>& L = q < n0 ? *t0 : *t1;
            size_t i = (q < n0 ? q : q - n0) * 3;
            Pt A, B, C;
            if (!pr(L[i].pos.x, L[i].pos.y, L[i].pos.z, A) || !pr(L[i + 1].pos.x, L[i + 1].pos.y, L[i + 1].pos.z, B) ||
                !pr(L[i + 2].pos.x, L[i + 2].pos.y, L[i + 2].pos.z, C)) continue;
            rasterTri(b, A, B, C);
        }
        tb[t] = msSince(a0);
        if (!in.front) return;
        auto a1 = clk::now();
        Buf& hb = hairB[t];
        hb.reset(W, H);
        int gp = (N + T - 1) / T, g0 = std::min(N, t * gp), g1 = std::min(N, (t + 1) * gp);
        for (int i = g0; i < g1; i++) {
            if (!in.alive[i]) continue;
            for (int k = 0; k + 1 < K; k++) {
                size_t a = size_t(i) * K + k;
                if (!ok[a] || !ok[a + 1]) continue;
                const Pt& A = sp[a], & B = sp[a + 1];
                float r = std::clamp(kSplatMm * 0.001f / pxWorld(0.5f * (A.d + B.d)), 0.5f, 6.0f);
                int ri = int(r - 0.5f + 0.999f);   // stamp half-size (0 = one pixel)
                float L = std::max(std::fabs(B.x - A.x), std::fabs(B.y - A.y));
                int n = std::max(1, int(std::ceil(L / std::max(0.75f, r * 0.75f))));
                for (int j = 0; j <= n; j++) {
                    float u = float(j) / n, x = A.x + (B.x - A.x) * u, y = A.y + (B.y - A.y) * u, d = A.d + (B.d - A.d) * u;
                    int cx = int(x), cy = int(y);
                    float pw = pxWorld(d) * kConeSlope;
                    for (int yy = std::max(0, cy - ri); yy <= std::min(H - 1, cy + ri); yy++)
                        for (int xx = std::max(0, cx - ri); xx <= std::min(W - 1, cx + ri); xx++) {
                            float ex = xx + 0.5f - x, ey = yy + 0.5f - y;
                            float dd = d + std::sqrt(ex * ex + ey * ey) * pw;
                            float& o = hb.z[size_t(yy) * W + xx];
                            if (dd < o) o = dd;
                        }
                }
            }
        }
        th[t] = msSince(a1);
    });
    Buf& body = bodyB[0];
    Buf& hair = hairB[0];
    parallel(T, H, [&](int, int b, int e) {   // merge rows
        for (int t = 1; t < T; t++)
            for (size_t i = size_t(b) * W; i < size_t(e) * W; i++) {
                body.z[i] = std::min(body.z[i], bodyB[t].z[i]);
                if (in.front) hair.z[i] = std::min(hair.z[i], hairB[t].z[i]);
            }
    });
    // per point / per guide flags
    out.pt.assign(size_t(N) * K, 1);
    out.shown.assign(N, 0);
    float tolF = in.tol * 0.001f, frac = std::clamp(in.pct / 100.0f, 0.0f, 1.0f);
    std::vector<int> cntT(T, 0);
    parallel(T, N, [&](int t, int b, int e) {
        for (int i = b; i < e; i++) {
            if (!in.alive[i]) continue;
            int front = 0, any = 0, seen = 0;
            for (int k = 0; k < K; k++) {
                size_t a = size_t(i) * K + k;
                bool vis = ok[a];
                if (vis) {
                    const Pt& p = sp[a];
                    float zb = body.at(p.x, p.y), pw = pxWorld(p.d);
                    bool bodyVis = p.d <= zb + 0.004f + 2.0f * pw;
                    if (in.front && k > 0 && bodyVis) {
                        seen++;
                        if (p.d <= std::min(zb, hair.at(p.x, p.y)) + tolF + pw) front++;
                    }
                    if (in.body) vis = bodyVis;
                }
                out.pt[a] = vis ? 1 : 0;
                if (vis) any++;
            }
            bool sh = in.front ? (front >= 2 && front >= int(std::ceil(frac * seen - 1e-4f))) : any > 0;
            if (in.front && in.body && any < 2) sh = false;   // nothing drawable left after the body clip
            out.shown[i] = sh ? 1 : 0;
            cntT[t] += sh;
        }
    });
    out.nShown = 0;
    for (int c : cntT) out.nShown += c;
    out.key = in.key;
    out.N = N;
    out.K = K;
    out.nTri = (int)nTri;
    out.res = std::max(W, H);
    out.msBody = *std::max_element(tb.begin(), tb.end());
    out.msHair = *std::max_element(th.begin(), th.end());
    out.ms = msSince(t00);
}

// the background worker of the viewport (one job at a time; leaked on purpose, it lives as long as the process)
struct Worker {
    std::mutex m;
    std::condition_variable cv;
    bool started = false, busy = false, has = false, done = false;
    VisIn job;
    VisOut res;
    void start() {
        started = true;
        std::thread([this]() {
            for (;;) {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [&]() { return has; });
                has = false;
                l.unlock();
                VisOut o;
                runVis(job, o);   // job is not touched by the main thread while busy
                job.own[0].clear();
                job.own[1].clear();
                l.lock();
                res = std::move(o);
                done = true;
                busy = false;
            }
        }).detach();
    }
};
Worker& worker() {
    static Worker* w = new Worker;
    return *w;
}
}   // namespace

static void adopt(App::GuideVis& g, VisOut& o) {
    g.key = o.key;
    g.N = o.N;
    g.K = o.K;
    g.nShown = o.nShown;
    g.nTri = o.nTri;
    g.res = o.res;
    g.ms = o.ms;
    g.msBody = o.msBody;
    g.msHair = o.msHair;
    g.pt.swap(o.pt);
    g.shown.swap(o.shown);
    g.valid = true;
}

void App::computeGuideVis(const XMFLOAT4X4& Vm, const XMFLOAT4X4& Pm, int w, int h, std::vector<GVertex>* t0,
                          std::vector<GVertex>* t1, bool async) {
    auto t00 = clk::now();
    const int N = (int)groom.P.size(), K = groom.K;
    if (!loaded || N == 0 || K < 2 || w <= 0 || h <= 0) { gvis.valid = false; return; }
    int T = threadCount();
    // posed points of this frame (the overlay draws them; the flags may come from a job one or two frames older)
    gvis.pts.resize(size_t(N) * K);
    gvis.ptsK = K;
    parallel(T, N, [&](int, int b, int e) {
        for (int i = b; i < e; i++)
            if (groom.alive[i]) for (int k = 0; k < K; k++) gvis.pts[size_t(i) * K + k] = posedPoint(i, k);
    });
    // one hash of every input: unchanged = keep the last result
    uint64_t key = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {   // FNV-1a over 32-bit words (+ tail bytes)
        auto c = (const unsigned char*)p;
        size_t i = 0;
        for (; i + 4 <= n; i += 4) { uint32_t v; memcpy(&v, c + i, 4); key ^= v; key *= 1099511628211ull; }
        for (; i < n; i++) { key ^= c[i]; key *= 1099511628211ull; }
    };
    mix(&Vm, sizeof Vm); mix(&Pm, sizeof Pm); mix(&w, sizeof w); mix(&h, sizeof h);
    bool fl[2] = {guideFront, guideBody};
    mix(fl, sizeof fl); mix(&guideFrontPct, 4); mix(&guideTolMm, 4);
    mix(groom.alive.data(), groom.alive.size()); mix(gvis.pts.data(), gvis.pts.size() * sizeof(V3));
    size_t nb[2] = {t0 ? t0->size() : 0, t1 ? t1->size() : 0};
    mix(nb, sizeof nb);
    for (auto* L : {t0, t1}) if (L) for (size_t i = 0; i < L->size(); i += 211) mix(&(*L)[i].pos, sizeof(XMFLOAT3));
    Worker& wk = worker();
    if (wk.started) {   // a finished background job
        std::lock_guard<std::mutex> l(wk.m);
        if (wk.done) { wk.done = false; adopt(gvis, wk.res); }
    }
    gvis.cached = gvis.valid && key == gvis.key && gvis.N == N && gvis.K == K;
    gvis.msMain = msSince(t00);
    if (gvis.cached) return;
    auto fill = [&](VisIn& in) {
        in.V = Vm; in.P = Pm; in.w = w; in.h = h; in.N = N; in.K = K;
        in.front = guideFront; in.body = guideBody; in.pct = guideFrontPct; in.tol = guideTolMm; in.key = key;
        in.alive.assign(groom.alive.begin(), groom.alive.end());
    };
    if (!async) {
        VisIn in;
        fill(in);
        in.pts = gvis.pts;
        if (!t0 && !t1) { posedMeshTris(in.own[0]); in.tri[0] = &in.own[0]; }
        else { in.tri[0] = t0; in.tri[1] = t1; }
        VisOut o;
        runVis(in, o);
        adopt(gvis, o);
        gvis.cached = false;
        gvis.msMain = msSince(t00);
        return;
    }
    if (!wk.started) wk.start();
    {
        std::lock_guard<std::mutex> l(wk.m);
        if (wk.busy) return;   // the running job finishes first; the next frame sends the newest state
        VisIn& in = wk.job;
        fill(in);
        in.pts = gvis.pts;
        if (t0 || t1) {   // the frame's own mesh lists (drawScene does not use them after the overlay): moved, not copied
            if (t0) in.own[0].swap(*t0); else in.own[0].clear();
            if (t1) in.own[1].swap(*t1); else in.own[1].clear();
        } else {
            posedMeshTris(in.own[0]);
            in.own[1].clear();
        }
        in.tri[0] = &in.own[0];
        in.tri[1] = &in.own[1];
        wk.busy = true;
        wk.has = true;
    }
    wk.cv.notify_one();
    gvis.msMain = msSince(t00);
}

bool App::guideShown(int i) const {
    if (!guideVisOn() || !gvis.valid || gvis.N != (int)groom.P.size() || i < 0 || i >= gvis.N) return true;
    return gvis.shown[i] != 0;
}

bool App::guidePointShown(int i, int k) const {
    if (!guideVisOn() || !gvis.valid || gvis.N != (int)groom.P.size() || i < 0 || i >= gvis.N || k < 0 || k >= gvis.K) return true;
    return gvis.shown[i] && gvis.pt[size_t(i) * gvis.K + k];
}

bool App::pickVisible(int i) const { return !visFilter() || guideShown(i); }
bool App::pickPointVisible(int i, int k) const { return !visFilter() || guidePointShown(i, k); }
