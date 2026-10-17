// Desktop Groom Studio - guide edit ops. Our own code (MIT).
// Semantics follow tools/hairworks/strand_edits.py (docstring): guides are matched by ROOT position (edit frame,
// metres) within `tol`; ops run in order; duplicates join at the end of the list.
#include "groom.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>

#include "curls.h"

double r4(double v) { return std::round(v * 10000.0) / 10000.0; }

bool faceDivergent(const Groom& g, const Face& F, float rootSp, float divDeg, float spreadK) {
    V3 dv[3];
    for (int q = 0; q < 3; q++) {
        auto& G = g.P[F.g[q]];
        V3 d = G[G.size() / 2] - G[0];
        float l = len(d);
        dv[q] = l > 1e-9f ? d * (1.0f / l) : V3{0, 0, 1};
    }
    float mx = 0;
    for (int q = 0; q < 3; q++) mx = std::max(mx, std::acos(std::clamp(dot(dv[q], dv[(q + 1) % 3]), -1.0f, 1.0f)) * 57.2958f);
    float tipSp = 0;
    for (int q = 0; q < 3; q++) tipSp = std::max(tipSp, len(g.P[F.g[q]].back() - g.P[F.g[(q + 1) % 3]].back()));
    return mx > divDeg && tipSp > spreadK * std::max(rootSp, 1e-4f);
}

LoopInfo guideLoop(const Guide& p, float arcMin, float closeMax, float curlR, float turnDeg) {
    LoopInfo out;
    if (p.size() < 3) return out;
    float L = 0;
    for (size_t k = 1; k < p.size(); k++) L += len(p[k] - p[k - 1]);
    if (L < arcMin) return out;
    // (1) return: resample evenly (4 mm, at most 250 points) so sparse guides cannot step over a return; for each point the FIRST
    // later point (after arcMin of hair) within closeMax closes a loop whose radius is the farthest loop point from its centroid
    float step = std::max(0.004f, L / 250.0f);
    std::vector<V3> q;
    float next = 0, acc = 0;
    for (size_t k = 1; k < p.size(); k++) {
        float seg = len(p[k] - p[k - 1]);
        while (next <= acc + seg && seg > 1e-9f) { q.push_back(p[k - 1] + (p[k] - p[k - 1]) * ((next - acc) / seg)); next += step; }
        acc += seg;
    }
    q.push_back(p.back());
    int n = (int)q.size(), minGap = std::max(1, (int)std::ceil(arcMin / step));
    for (int i = 0; i + minGap < n; i++)
        for (int j = i + minGap; j < n; j++) {
            float d = len(q[j] - q[i]);
            if (d >= closeMax) continue;
            V3 c;
            for (int k = i; k <= j; k++) c = c + q[k];
            c = c * (1.0f / (j - i + 1));
            float R = 0;
            for (int k = i; k <= j; k++) R = std::max(R, len(q[k] - c));
            if (R > curlR && (!out.flag || R > out.radius)) { out.flag = true; out.arc = (j - i) * step; out.dist = d; out.radius = R; }
            break;   // first return only: later ones are the same loop (or more turns of a curl)
        }
    if (out.flag) return out;
    // (2) circling: net turning (about the best axis) of the corners that are not tight curl turns reaches turnDeg
    std::vector<V3> cr;
    std::vector<float> ang, loose;
    for (size_t k = 1; k + 1 < p.size(); k++) {
        V3 a = p[k] - p[k - 1], b = p[k + 1] - p[k];
        float la = len(a), lb = len(b);
        if (la < 1e-6f || lb < 1e-6f) continue;
        a = a * (1.0f / la); b = b * (1.0f / lb);
        V3 x = cross(a, b);
        float th = std::atan2(len(x), dot(a, b));
        float r = 0.5f * (la + lb) / std::max(2.0f * std::sin(std::min(th, 3.14159f) * 0.5f), 1e-6f);
        cr.push_back(x); ang.push_back(th); loose.push_back(r > 0.03f ? 1.0f : 0.0f);
    }
    V3 ax;
    for (size_t k = 0; k < cr.size(); k++) if (loose[k] > 0) ax = ax + cr[k];
    float al = len(ax);
    if (al < 1e-12f) return out;
    ax = ax * (1.0f / al);
    float tot = 0;
    for (size_t k = 0; k < cr.size(); k++) if (loose[k] > 0) tot += dot(cr[k], ax) >= 0 ? ang[k] : -ang[k];
    out.turn = std::fabs(tot) * 57.2958f;
    if (out.turn < turnDeg) return out;
    // helical curl exemption (sparse CVs per turn make curl corners look "loose"): the strand never climbs back up more than
    // 30 mm (z up) AND, over the loose-corner span, it advances along the turning axis at least 0.75x its spread across it.
    // A whirl over the head climbs; a flat circle does not advance along its axis.
    float lowest = p[0].z, climb = 0;
    for (auto& v : p) { lowest = std::min(lowest, v.z); climb = std::max(climb, v.z - lowest); }
    int k0 = -1, k1 = -1;
    for (size_t k = 0; k < cr.size(); k++) if (loose[k] > 0) { if (k0 < 0) k0 = (int)k; k1 = (int)k; }
    float hmin = 1e9f, hmax = -1e9f;
    V3 cen;
    int cnt = 0;
    for (int k = k0; k <= k1 + 2 && k < (int)p.size(); k++) { float h = dot(p[k], ax); hmin = std::min(hmin, h); hmax = std::max(hmax, h); cen = cen + (p[k] - ax * h); cnt++; }
    cen = cen * (1.0f / std::max(cnt, 1));
    float spread = 0;
    for (int k = k0; k <= k1 + 2 && k < (int)p.size(); k++) spread = std::max(spread, len(p[k] - ax * dot(p[k], ax) - cen));
    if (climb < 0.03f && (hmax - hmin) >= 0.75f * spread) return out;
    out.flag = true;
    return out;
}

static std::vector<float> falloffWeights(int K, const std::string& falloff, int from) {
    std::vector<float> w(K, 1.0f);
    for (int k = 0; k < K; k++) {
        float t = K > 1 ? float(k) / float(K - 1) : 1.0f;
        if (falloff == "linear") w[k] = t;
        else if (falloff == "tip") w[k] = t * t;
        else if (falloff == "point") { w[k] = std::max(0.0f, 1.0f - std::abs(k - from) / 2.0f); continue; }   // one CV (+ half on neighbours)
        if (k < from) w[k] = 0.0f;
    }
    return w;
}

static std::vector<V3> resample(const std::vector<V3>& q, int K) {
    std::vector<float> d(q.size(), 0.0f);
    for (size_t i = 1; i < q.size(); i++) d[i] = d[i - 1] + len(q[i] - q[i - 1]);
    std::vector<V3> out(K, q[0]);
    if (d.back() < 1e-9f) return out;
    for (int k = 0; k < K; k++) {
        float t = d.back() * k / float(K - 1);
        size_t j = 1;
        while (j < q.size() - 1 && d[j] < t) j++;
        float a = (t - d[j - 1]) / std::max(d[j] - d[j - 1], 1e-12f);
        a = std::clamp(a, 0.0f, 1.0f);
        out[k] = q[j - 1] + (q[j] - q[j - 1]) * a;
    }
    return out;
}

std::vector<V3> shortenGuide(const Guide& p, float keep) {
    int K = (int)p.size();
    std::vector<float> d(K, 0.0f);
    for (int i = 1; i < K; i++) d[i] = d[i - 1] + len(p[i] - p[i - 1]);
    float L = d.back() * std::clamp(keep, 0.02f, 1.0f);
    int k = int(std::lower_bound(d.begin(), d.end(), L) - d.begin());
    if (k >= K) return p;
    k = std::max(k, 1);
    float t = (L - d[k - 1]) / std::max(d[k] - d[k - 1], 1e-12f);
    std::vector<V3> q(p.begin(), p.begin() + k);
    q.push_back(p[k - 1] + (p[k] - p[k - 1]) * t);
    return resample(q, K);
}

// centripetal Catmull-Rom (alpha 0.5, Barry-Goldman) on segment p1-p2; phantom end points mirror the end segments
static V3 crPoint(V3 p0, V3 p1, V3 p2, V3 p3, float u) {
    auto kn = [](V3 a, V3 b, float t) { return t + std::max(std::sqrt(len(b - a)), 1e-4f); };
    float t0 = 0, t1 = kn(p0, p1, t0), t2 = kn(p1, p2, t1), t3 = kn(p2, p3, t2);
    float t = t1 + (t2 - t1) * u;
    auto L = [](V3 a, V3 b, float ta, float tb, float t) { return a * ((tb - t) / (tb - ta)) + b * ((t - ta) / (tb - ta)); };
    V3 a1 = L(p0, p1, t0, t1, t), a2 = L(p1, p2, t1, t2, t), a3 = L(p2, p3, t2, t3, t);
    V3 b1 = L(a1, a2, t0, t2, t), b2 = L(a2, a3, t1, t3, t);
    return L(b1, b2, t1, t2, t);
}

std::vector<V3> smoothDense(const Guide& p, int perSeg) {
    int K = (int)p.size();
    if (K < 3) return p;
    std::vector<V3> out;
    out.reserve(size_t(K - 1) * perSeg + 1);
    for (int i = 0; i + 1 < K; i++) {
        V3 p0 = i > 0 ? p[i - 1] : p[0] * 2.0f - p[1];
        V3 p3 = i + 2 < K ? p[i + 2] : p[K - 1] * 2.0f - p[K - 2];
        if (len(p[i + 1] - p[i]) < 1e-7f) { for (int s = 0; s < perSeg; s++) out.push_back(p[i]); continue; }
        for (int s = 0; s < perSeg; s++) out.push_back(s == 0 ? p[i] : crPoint(p0, p[i], p[i + 1], p3, float(s) / perSeg));
    }
    out.push_back(p[K - 1]);
    return out;
}

std::vector<V3> sampleSmoothAt(const Guide& p, const std::vector<float>& s) {
    std::vector<V3> q = smoothDense(p, 24);
    std::vector<float> d(q.size(), 0.0f);
    for (size_t i = 1; i < q.size(); i++) d[i] = d[i - 1] + len(q[i] - q[i - 1]);
    std::vector<V3> out(s.size(), q[0]);
    V3 tipDir = q.size() > 1 ? q.back() - q[q.size() - 2] : V3{0, 0, -1};
    float tl = len(tipDir);
    tipDir = tl > 1e-9f ? tipDir * (1.0f / tl) : V3{0, 0, -1};
    size_t j = 1;
    for (size_t k = 0; k < s.size(); k++) {
        float t = s[k];
        if (t >= d.back()) { out[k] = q.back() + tipDir * (t - d.back()); continue; }
        if (t <= 0) { out[k] = q[0]; continue; }
        if (k == 0 || s[k] < s[k - 1]) j = 1;
        while (j < q.size() - 1 && d[j] < t) j++;
        float a = std::clamp((t - d[j - 1]) / std::max(d[j] - d[j - 1], 1e-12f), 0.0f, 1.0f);
        out[k] = q[j - 1] + (q[j] - q[j - 1]) * a;
    }
    return out;
}

Guide resampleSmooth(const Guide& p, int n) {
    n = std::clamp(n, 2, kMaxCvs);
    const int K = (int)p.size();
    if (K < 2) return Guide(n, p.empty() ? V3{} : p[0]);
    if (n < K || K < 3) {   // fewer CVs: evenly along the arc of the smooth curve
        std::vector<V3> q = smoothDense(p, 24);
        float L = 0;
        for (size_t i = 1; i < q.size(); i++) L += len(q[i] - q[i - 1]);
        std::vector<float> s(n);
        for (int k = 0; k < n; k++) s[k] = L * k / float(n - 1);
        Guide out = sampleSmoothAt(p, s);
        out.front() = p.front();
        out.back() = p.back();
        return out;
    }
    // more CVs: every old CV is kept exactly; each old segment gets new CVs on the smooth curve in proportion to its arc
    // length (largest remainder), so the strand does not change shape (only gains points along the same curve)
    const int per = 24;
    std::vector<V3> q = smoothDense(p, per);   // K-1 segments x per samples (+ tip)
    std::vector<float> segL(K - 1, 0.0f);
    float L = 0;
    for (int j = 0; j + 1 < K; j++) {
        for (int s = 0; s < per; s++) { size_t a = size_t(j) * per + s; segL[j] += len(q[a + 1] - q[a]); }
        L += segL[j];
    }
    std::vector<int> m(K - 1, 1);
    int extra = (n - 1) - (K - 1);
    std::vector<std::pair<float, int>> rem;
    if (L > 1e-9f) {
        int used = 0;
        for (int j = 0; j + 1 < K; j++) {
            float want = segL[j] / L * float(n - 1) - 1.0f;   // ideal extra subdivisions of this segment
            int w = std::max(0, (int)std::floor(want));
            w = std::min(w, extra - used);
            m[j] += w; used += w;
            rem.push_back({want - float(w), j});
        }
        std::sort(rem.begin(), rem.end(), [](auto& x, auto& y) { return x.first > y.first; });
        for (size_t r = 0; used < extra; r = (r + 1) % rem.size()) { m[rem[r].second]++; used++; }
    } else {
        for (int u = 0; u < extra; u++) m[u % (K - 1)]++;
    }
    Guide out;
    out.reserve(n);
    for (int j = 0; j + 1 < K; j++) {
        out.push_back(p[j]);
        // arc positions inside the segment's dense samples
        std::vector<float> d(per + 1, 0.0f);
        for (int s = 0; s < per; s++) { size_t a = size_t(j) * per + s; d[s + 1] = d[s] + len(q[a + 1] - q[a]); }
        for (int t = 1; t < m[j]; t++) {
            float target = d[per] * float(t) / float(m[j]);
            int s = 1;
            while (s < per && d[s] < target) s++;
            float a = std::clamp((target - d[s - 1]) / std::max(d[s] - d[s - 1], 1e-12f), 0.0f, 1.0f);
            size_t b = size_t(j) * per + s;
            out.push_back(q[b - 1] + (q[b] - q[b - 1]) * a);
        }
    }
    out.push_back(p[K - 1]);
    return out;
}

void curveDeviation(const Guide& a, const Guide& b, float& maxD, float& meanD) {
    std::vector<V3> qa = smoothDense(a, 8), qb = smoothDense(b, 8);
    maxD = 0; meanD = 0;
    for (auto& x : qa) {
        float bd = 1e9f;
        for (size_t j = 0; j + 1 < qb.size(); j++) {
            V3 e = qb[j + 1] - qb[j];
            float ee = dot(e, e);
            float t = ee > 1e-14f ? std::clamp(dot(x - qb[j], e) / ee, 0.0f, 1.0f) : 0.0f;
            bd = std::min(bd, len(x - (qb[j] + e * t)));
        }
        maxD = std::max(maxD, bd);
        meanD += bd;
    }
    if (!qa.empty()) meanD /= float(qa.size());
}

void Groom::init(const DeskPack* p) {
    pack = p;
    N0 = (int)p->guidesEdit.size();
    K = p->K;
    ops.clear();
    redo.clear();
    replay();
}

int Groom::baseOf(int i) const {
    int b = i;
    int guard = 0;
    while (b >= N0 && src[b] >= 0 && guard++ < 100000) b = src[b];
    return b < N0 ? b : 0;
}

V3 Groom::renderPoint(int i, int k) const {
    if (i < 0 || i >= (int)P.size() || k < 0 || k >= (int)P[i].size()) return V3{};   // stale index (crash 2026-10-15, stage input)
    int b = baseOf(i);
    const int K0 = pack->K;
    if (b >= (int)pack->guides.size() || b >= (int)pack->guidesEdit.size()
        || (int)pack->guides[b].size() < K0 || (int)pack->guidesEdit[b].size() < K0) return P[i][k];
    if (K == K0) return P[i][k] + (pack->guides[b][k] - pack->guidesEdit[b][k]);
    // resampled groom: the build's per-CV fur-frame offset taken at the same fraction of the guide
    float f = K > 1 ? float(k) * float(K0 - 1) / float(K - 1) : 0.0f;
    int k0 = std::clamp(int(f), 0, K0 - 1), k1 = std::min(k0 + 1, K0 - 1);
    float a = f - float(k0);
    V3 d0 = pack->guides[b][k0] - pack->guidesEdit[b][k0], d1 = pack->guides[b][k1] - pack->guidesEdit[b][k1];
    return P[i][k] + d0 * (1.0f - a) + d1 * a;
}

Guide Groom::renderGuide(int i) const {
    Guide g(K);
    for (int k = 0; k < K; k++) g[k] = renderPoint(i, k);
    return g;
}

int Groom::numAlive() const {
    int n = 0;
    for (char a : alive) n += a ? 1 : 0;
    return n;
}

std::vector<int> Groom::match(const json& roots) {
    std::vector<int> out;
    // many roots: bucket the alive roots in a uniform grid of cell >= tol once per call (perf 2026-10-15, 3000-guide
    // Yennefer); the nearest root within tol is always in the 3x3x3 neighbour cells, ties keep the lowest index, so the
    // result equals the brute-force scan below
    if (roots.size() >= 4 && P.size() >= 64 && tol > 0) {
        float cell = tol * 1.01f;
        auto key = [&](int x, int y, int z) { return (int64_t(x) * 73856093) ^ (int64_t(y) * 19349663) ^ (int64_t(z) * 83492791); };
        std::vector<std::pair<int64_t, int>> grid;
        grid.reserve(P.size());
        for (size_t i = 0; i < P.size(); i++)
            if (alive[i]) grid.push_back({key(int(std::floor(P[i][0].x / cell)), int(std::floor(P[i][0].y / cell)), int(std::floor(P[i][0].z / cell))), (int)i});
        std::sort(grid.begin(), grid.end());
        for (auto& r : roots) {
            V3 q{r[0].get<float>(), r[1].get<float>(), r[2].get<float>()};
            int cx = int(std::floor(q.x / cell)), cy = int(std::floor(q.y / cell)), cz = int(std::floor(q.z / cell));
            int best = -1;
            float bd = 1e9f;
            for (int a = -1; a <= 1; a++)
                for (int b = -1; b <= 1; b++)
                    for (int c = -1; c <= 1; c++) {
                        int64_t kk = key(cx + a, cy + b, cz + c);
                        auto lo = std::lower_bound(grid.begin(), grid.end(), std::pair<int64_t, int>{kk, INT_MIN});
                        for (auto it = lo; it != grid.end() && it->first == kk; ++it) {
                            int i = it->second;
                            if (i < 0 || int(std::floor(P[i][0].x / cell)) != cx + a || int(std::floor(P[i][0].y / cell)) != cy + b ||
                                int(std::floor(P[i][0].z / cell)) != cz + c) continue;   // hash collision
                            float d = len(P[i][0] - q);
                            if (d < bd || (d == bd && i < best)) { bd = d; best = i; }
                        }
                    }
            if (best >= 0 && bd <= tol) {
                if (std::find(out.begin(), out.end(), best) == out.end()) out.push_back(best);
            } else {
                unmatched++;
            }
        }
        return out;
    }
    for (auto& r : roots) {
        V3 q{r[0].get<float>(), r[1].get<float>(), r[2].get<float>()};
        int best = -1;
        float bd = 1e9f;
        for (size_t i = 0; i < P.size(); i++) {
            if (!alive[i]) continue;
            float d = len(P[i][0] - q);
            if (d < bd) { bd = d; best = (int)i; }
        }
        if (best >= 0 && bd <= tol) {
            if (std::find(out.begin(), out.end(), best) == out.end()) out.push_back(best);
        } else {
            unmatched++;
        }
    }
    return out;
}

// growth faces of deleted guides are removed; each hole (closed boundary loop, kept in the faces' winding) is fanned
void Groom::removeGuides(const std::vector<int>& dead) {
    for (int i : dead) alive[i] = 0;
    std::vector<Face> keep, gone;
    for (auto& f : faces) {
        if (alive[f.g[0]] && alive[f.g[1]] && alive[f.g[2]]) keep.push_back(f);
        else gone.push_back(f);
    }
    std::map<std::pair<int, int>, int> cnt;
    std::map<int, std::array<float, 2>> uvOf;
    for (auto& f : gone)
        for (int e = 0; e < 3; e++) {
            int a = f.g[e], b = f.g[(e + 1) % 3];
            cnt[{a, b}]++;
            if (alive[a] && !uvOf.count(a)) uvOf[a] = {f.uv[2 * e], f.uv[2 * e + 1]};
        }
    std::map<int, int> next;
    for (auto& f : gone)
        for (int e = 0; e < 3; e++) {
            int a = f.g[e], b = f.g[(e + 1) % 3];
            if (!alive[a] || !alive[b]) continue;
            if (cnt[{a, b}] == 1 && !cnt.count({b, a})) next[a] = b;
        }
    // the dead guide's ring: edges between two alive neighbours of a removed face are the hole boundary
    std::set<int> used;
    for (auto& [s, n0] : next) {
        if (used.count(s)) continue;
        std::vector<int> loop{s};
        used.insert(s);
        int cur = n0;
        bool closed = false;
        for (int guard = 0; guard < 400; guard++) {
            if (cur == s) { closed = true; break; }
            if (used.count(cur) || !next.count(cur)) break;
            loop.push_back(cur);
            used.insert(cur);
            cur = next[cur];
        }
        if (!closed || loop.size() < 3) continue;
        for (size_t i = 1; i + 1 < loop.size(); i++) {
            Face f;
            f.g = {loop[0], loop[i], loop[i + 1]};
            for (int c = 0; c < 3; c++) {
                auto uv = uvOf[f.g[c]];
                f.uv[2 * c] = uv[0];
                f.uv[2 * c + 1] = uv[1];
            }
            keep.push_back(f);
        }
    }
    faces.swap(keep);
}

static std::array<float, 3> closestBary(V3 p, V3 a, V3 b, V3 c, float& dist) {
    // projection onto the triangle plane, barycentrics clamped to the triangle
    V3 n = cross(b - a, c - a);
    float nn = dot(n, n);
    if (nn < 1e-14f) { dist = 1e9f; return {1, 0, 0}; }
    V3 q = p - n * (dot(p - a, n) / nn);
    float w2 = dot(cross(b - a, q - a), n) / nn;
    float w0 = dot(cross(c - b, q - b), n) / nn;
    float w1 = 1.0f - w0 - w2;
    w0 = std::max(w0, 0.0f); w1 = std::max(w1, 0.0f); w2 = std::max(w2, 0.0f);
    float s = w0 + w1 + w2;
    w0 /= s; w1 /= s; w2 /= s;
    V3 r = a * w0 + b * w1 + c * w2;
    dist = len(p - r);
    return {w0, w1, w2};
}

// a duplicated guide splits the growth face its root lands on into three, so HairWorks interpolates around it
void Groom::insertGuide(int ni, bool edgeFace, bool srcUv, const float* fixUv, const float* newUv) {
    V3 r = P[ni][0];
    int best = -1;
    float bd = 1e9f;
    std::array<float, 3> bw{};
    for (size_t f = 0; f < faces.size(); f++) {
        auto& F = faces[f];
        float d;
        auto w = closestBary(r, P[F.g[0]][0], P[F.g[1]][0], P[F.g[2]][0], d);
        if (d < bd) { bd = d; best = (int)f; bw = w; }
    }
    // 2026-10-12 opt-in (Studio hairline_fill, duplicate op "edge_face": true): a root OUTSIDE the growth mesh (past its
    // open edge) gets new faces fanned onto the nearest open edges instead of folding the nearest face, so the hair
    // interpolated there grows from the new root outward (the growth mesh extends to the planted hairline)
    if (edgeFace && best >= 0 && bd > 0.0015f) {
        std::map<std::pair<int, int>, int> ec;
        std::map<std::pair<int, int>, std::pair<int, int>> owner;   // open edge -> (face, corner a)
        for (size_t f = 0; f < faces.size(); f++)
            for (int q = 0; q < 3; q++) {
                int a = faces[f].g[q], b = faces[f].g[(q + 1) % 3];
                auto k = std::make_pair(std::min(a, b), std::max(a, b));
                ec[k]++; owner[k] = {(int)f, q};
            }
        struct Cand { float d; int f, q; };
        std::vector<Cand> cs;
        for (auto& [k, n] : ec) {
            if (n != 1) continue;
            auto [f, q] = owner[k];
            const Face& F = faces[f];
            V3 A = P[F.g[q]][0], B = P[F.g[(q + 1) % 3]][0], C = P[F.g[(q + 2) % 3]][0];
            if (len(A - r) > 0.025f || len(B - r) > 0.025f) continue;
            // the root must lie on the open side of the edge (opposite the face's third corner)
            V3 e = B - A, m = (A + B) * 0.5f, nrm = cross(e, C - A);
            V3 out = cross(e, nrm);   // in the face plane, perpendicular to the edge
            if (dot(out, C - m) > 0) out = out * -1.0f;
            if (dot(out, r - m) <= 0) continue;
            float dl = std::max(len(A - r), len(B - r));
            cs.push_back({dl, f, q});
        }
        std::sort(cs.begin(), cs.end(), [](const Cand& x, const Cand& y) { return x.d < y.d; });
        // 2026-10-12 opt-in (duplicate "edge_uv": "src", Studio fill_below_hairline / make_bun fill): the new root's corner
        // takes the uv of the guide it was copied from (its colour / density texel) instead of the open edge's midpoint,
        // whose texel at the growth-mesh border can be another colour (light / dark patches on the planted hair)
        // "edge_uv": [u, v] = this uv on ALL corners of the new edge faces (Studio fills pass the build's typical hair
        // texel: on atlased builds the source guide's corner can sit on any colour - a blue-grey slab on Ciri's crown)
        bool haveSrcUv = fixUv != nullptr;
        float su = fixUv ? fixUv[0] : 0, sv = fixUv ? fixUv[1] : 0;
        if (!haveSrcUv && srcUv && ni < (int)src.size() && src[ni] >= 0)
            for (auto& F : faces) {
                for (int q = 0; q < 3 && !haveSrcUv; q++) if (F.g[q] == src[ni]) { su = F.uv[2 * q]; sv = F.uv[2 * q + 1]; haveSrcUv = true; }
                if (haveSrcUv) break;
            }
        int added = 0;
        std::set<int> used;
        for (auto& c : cs) {
            if (added >= 2) break;
            const Face F = faces[c.f];
            int a = F.g[c.q], b = F.g[(c.q + 1) % 3];
            if (added == 1 && !(used.count(a) || used.count(b))) continue;   // second face: the neighbouring open edge only
            Face n;
            float u = 0.5f * (F.uv[2 * c.q] + F.uv[2 * ((c.q + 1) % 3)]), v = 0.5f * (F.uv[2 * c.q + 1] + F.uv[2 * ((c.q + 1) % 3) + 1]);
            if (haveSrcUv) { u = su; v = sv; }
            // 2026-10-15 opt-in (duplicate "new_uv", Studio fade_hairline extend_mm): only the NEW root's corner takes this
            // uv (a sparse texel); the old edge keeps its own, so the density ramps from the old edge out to the new root
            if (newUv) { u = newUv[0]; v = newUv[1]; }
            n.g = {b, a, ni};   // reversed edge = same winding as the neighbour
            n.uv = {F.uv[2 * ((c.q + 1) % 3)], F.uv[2 * ((c.q + 1) % 3) + 1], F.uv[2 * c.q], F.uv[2 * c.q + 1], u, v};
            if (fixUv) n.uv = {su, sv, su, sv, su, sv};
            faces.push_back(n);
            used.insert(a); used.insert(b);
            added++;
        }
        if (added) return;
    }
    if (best < 0 || bd > 0.02f) return;
    Face F = faces[best];
    float u = bw[0] * F.uv[0] + bw[1] * F.uv[2] + bw[2] * F.uv[4];
    float v = bw[0] * F.uv[1] + bw[1] * F.uv[3] + bw[2] * F.uv[5];
    if (newUv) { u = newUv[0]; v = newUv[1]; }
    faces.erase(faces.begin() + best);
    for (int e = 0; e < 3; e++) {
        Face n;
        int a = e, b = (e + 1) % 3;
        n.g = {F.g[a], F.g[b], ni};
        n.uv = {F.uv[2 * a], F.uv[2 * a + 1], F.uv[2 * b], F.uv[2 * b + 1], u, v};
        faces.push_back(n);
    }
}

// ------------------------------------------------------------------------------------------------ mirror (op semantics of
// tools/hairworks/strand_edits.py: source side copied across x = plane_x, target side deleted, centre band kept)
void Groom::buildHead() {
    if (!headV.empty()) return;
    for (auto& m : pack->meshes) {
        if (m.kind != "head" && m.name != "head") continue;
        headV = m.v;
        headN.assign(m.v.size(), V3{0, 0, 0});
        for (auto& t : m.t) {
            V3 n = cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]]);
            for (int c = 0; c < 3; c++) headN[t[c]] = headN[t[c]] + n;
        }
        for (size_t i = 0; i < headN.size(); i++) {
            float l = len(headN[i]);
            headN[i] = l > 1e-12f ? headN[i] * (1.0f / l) : V3{0, 0, 1};
            if (dot(headV[i] - pack->headC, headN[i]) < 0) headN[i] = headN[i] * -1.0f;
        }
        break;
    }
}

float Groom::headSigned(V3 p, V3* nrm) const {
    if (headV.empty()) { if (nrm) *nrm = V3{0, 0, 1}; return 1.0f; }
    size_t best = 0;
    float bd = 1e9f;
    for (size_t i = 0; i < headV.size(); i++) { float d = dot(p - headV[i], p - headV[i]); if (d < bd) { bd = d; best = i; } }
    if (nrm) *nrm = headN[best];
    return dot(p - headV[best], headN[best]);
}

float Groom::mirrorPlane() {
    if (planeCache < 1e8f) return planeCache;
    buildHead();
    if (headV.empty()) return planeCache = 0.0f;
    std::vector<V3> cr;
    float top = -1e9f;
    for (auto& v : headV) top = std::max(top, v.z);
    for (auto& v : headV) if (v.z > top - 0.12f) cr.push_back(v);
    float bestE = 1e9f, bestC = 0;
    for (float c = -0.02f; c <= 0.02001f; c += 0.0005f) {
        double e = 0;
        for (auto& v : cr) {
            V3 m{2 * c - v.x, v.y, v.z};
            float bd = 1e9f;
            for (auto& w : cr) bd = std::min(bd, dot(m - w, m - w));
            e += std::sqrt(bd);
        }
        if (e < bestE) { bestE = (float)e; bestC = c; }
    }
    return planeCache = bestC;
}

// explicit form of a mirror op for the current state: source side, numeric plane_x and the roots of the guides mirrored
json Groom::resolveMirror(const json& op) {
    buildHead();
    json r = op;
    r["type"] = "mirror";
    r.erase("op");
    if (!op.contains("source") || !op["source"].is_string()) r["source"] = "+x";
    float px = op.contains("plane_x") && op["plane_x"].is_number() ? op["plane_x"].get<float>() : mirrorPlane();
    r["plane_x"] = r4(px);
    float sgn = r["source"].get<std::string>()[0] == '-' ? -1.0f : 1.0f;
    float band = op.value("band", 0.015f);
    json roots = json::array();
    for (size_t i = 0; i < P.size(); i++) {
        if (!alive[i]) continue;
        V3 p = P[i][0];
        if ((op.contains("ymin") && p.y < op["ymin"].get<float>()) || (op.contains("ymax") && p.y > op["ymax"].get<float>()) ||
            (op.contains("zmin") && p.z < op["zmin"].get<float>()) || (op.contains("zmax") && p.z > op["zmax"].get<float>())) continue;
        float off = (p.x - px) * sgn;
        if (std::fabs(off) > band && off > 0) roots.push_back({r4(p.x), r4(p.y), r4(p.z)});
    }
    r["source_roots"] = roots;
    return r;
}

void Groom::applyMirror(const json& op) {
    buildHead();
    float px = op.contains("plane_x") && op["plane_x"].is_number() ? op["plane_x"].get<float>() : mirrorPlane();
    float sgn = op.value("source", std::string("+x"))[0] == '-' ? -1.0f : 1.0f;
    float band = op.value("band", 0.015f), clear = op.value("clear", 0.002f);
    auto inside = [&](V3 r) {
        if (op.contains("ymin") && r.y < op["ymin"].get<float>()) return false;
        if (op.contains("ymax") && r.y > op["ymax"].get<float>()) return false;
        if (op.contains("zmin") && r.z < op["zmin"].get<float>()) return false;
        if (op.contains("zmax") && r.z > op["zmax"].get<float>()) return false;
        return true;
    };
    std::vector<int> src, gone, centre;
    std::vector<int> side(P.size(), 0);   // +1 source, -1 target, 0 centre / untouched
    for (size_t i = 0; i < P.size(); i++) {
        if (!alive[i]) continue;
        float off = (P[i][0].x - px) * sgn;
        if (std::fabs(off) <= band || !inside(P[i][0])) continue;
        if (off > 0) { src.push_back((int)i); side[i] = 1; } else { gone.push_back((int)i); side[i] = -1; }
    }
    if (op.contains("source_roots") && op["source_roots"].is_array()) {   // explicit form (resolved at export): exactly these guides are the source
        for (int i : src) side[i] = 0;
        src.clear();
        for (int i : match(op["source_roots"])) if (alive[i] && side[i] == 0) { src.push_back(i); side[i] = 1; }
        gone.erase(std::remove_if(gone.begin(), gone.end(), [&](int i) { return side[i] > 0; }), gone.end());
    }
    std::map<int, int> copyOf;
    for (int i : src) {
        Guide p = P[i], q = p;
        std::vector<float> seg(K, 0.0f);
        for (int k = 1; k < K; k++) seg[k] = len(p[k] - p[k - 1]);
        for (auto& v : q) v.x = 2 * px - v.x;
        if (!headV.empty()) {
            V3 n0, n1;
            float sd0 = headSigned(p[0], &n0), sd1 = headSigned(q[0], &n1);
            for (auto& v : q) v = v - n1 * (sd1 - sd0);              // root back on the scalp, same offset as the source
            bool low = false;
            for (int k = 1; k < K; k++) {
                V3 nk;
                float s = headSigned(q[k], &nk);
                if (s < clear) { q[k] = q[k] + nk * (clear - s); low = true; }
            }
            if (low)
                for (int k = 1; k < K; k++) q[k] = q[k - 1] + [&] { V3 d = q[k] - q[k - 1]; float l = len(d); return l > 1e-12f ? d * (1.0f / l) : d; }() * seg[k];
        }
        P.push_back(q);
        alive.push_back(1);
        this->src.push_back(i);
        copyOf[i] = (int)P.size() - 1;
        edited.insert((int)P.size() - 1);
    }
    // faces: target-side faces go; faces of source + centre guides are mirrored (copies, reversed winding)
    std::vector<Face> keep;
    for (auto& f : faces) {
        bool tgt = side[f.g[0]] < 0 || side[f.g[1]] < 0 || side[f.g[2]] < 0;
        if (!tgt) keep.push_back(f);
        bool hasSrc = side[f.g[0]] > 0 || side[f.g[1]] > 0 || side[f.g[2]] > 0;
        if (hasSrc && !tgt) {
            Face m = f;
            for (int c = 0; c < 3; c++) if (side[f.g[c]] > 0) m.g[c] = copyOf[f.g[c]];
            std::swap(m.g[1], m.g[2]);
            std::swap(m.uv[2], m.uv[4]);
            std::swap(m.uv[3], m.uv[5]);
            keep.push_back(m);
        }
    }
    faces.swap(keep);
    for (int i : gone) alive[i] = 0;
}

// 2026-10-12 opt-in (Studio rebuild_from_cards): one op replaces a set of guides by guides traced from the vanilla
// cards. "delete": roots of the old guides removed (their faces go too, no hole fill); "guides": new guides (K points,
// edit frame); "base": per new guide the root of an existing guide that lends its skinning / fur-frame offset; "uv":
// per new guide its growth uv; "faces": growth triangles, each corner = index of a new guide or [x,y,z] root of a kept
// old guide. Studio-only (the HairLab pipeline does not know this op; Studio exports write the guides / faces shown).
void Groom::applyRebuild(const json& op) {
    auto rootOf = [](const json& r) { return V3{r[0].get<float>(), r[1].get<float>(), r[2].get<float>()}; };
    auto nearestAlive = [&](V3 q, float maxd) {
        int best = -1; float bd = maxd;
        for (size_t i = 0; i < P.size(); i++) if (alive[i]) { float d = len(P[i][0] - q); if (d <= bd) { bd = d; best = (int)i; } }
        return best;
    };
    // bases first (they may be among the deleted guides)
    std::vector<int> base;
    for (auto& b : op.value("base", json::array())) { int g = nearestAlive(rootOf(b), 1.0f); base.push_back(g < 0 ? 0 : g); }
    if (!op.contains("base"))   // baked community snapshot: the skinning source of each new guide is the nearest guide to its own root
        for (auto& pts : op.value("guides", json::array())) { int g = pts.empty() ? -1 : nearestAlive(rootOf(pts[0]), 1.0f); base.push_back(g < 0 ? 0 : g); }
    // uv of each kept guide (any face corner), before faces change
    std::map<int, std::array<float, 2>> uvOf;
    for (auto& f : faces) for (int c = 0; c < 3; c++) if (!uvOf.count(f.g[c])) uvOf[f.g[c]] = {f.uv[2 * c], f.uv[2 * c + 1]};
    if (op.value("delete_all", false)) {   // baked snapshot: every guide and face of the base goes
        for (size_t i = 0; i < P.size(); i++) alive[i] = 0;
        faces.clear();
    } else if (op.contains("delete") && !op["delete"].empty()) {
        auto dead = match(op["delete"]);
        for (int i : dead) alive[i] = 0;
        faces.erase(std::remove_if(faces.begin(), faces.end(), [&](const Face& f) { return !alive[f.g[0]] || !alive[f.g[1]] || !alive[f.g[2]]; }), faces.end());
    }
    std::vector<int> newIx;
    auto& gs = op["guides"];
    for (size_t n = 0; n < gs.size(); n++) {
        Guide G(K);
        auto& pts = gs[n];
        if ((int)pts.size() != K) { newIx.push_back(-1); continue; }
        for (int k = 0; k < K; k++) G[k] = rootOf(pts[k]);
        P.push_back(G);
        alive.push_back(1);
        src.push_back(n < base.size() ? base[n] : 0);
        int ni = (int)P.size() - 1;
        newIx.push_back(ni);
        edited.insert(ni);
        if (op.contains("uv") && n < op["uv"].size()) uvOf[ni] = {op["uv"][n][0].get<float>(), op["uv"][n][1].get<float>()};
        else uvOf[ni] = {0.5f, 0.5f};
    }
    size_t nf = (size_t)-1;
    for (auto& fj : op.value("faces", json::array())) {
        nf++;
        if (fj.size() != 3) continue;
        Face f;
        bool ok = true;
        for (int c = 0; c < 3; c++) {
            int g = -1;
            if (fj[c].is_number_integer()) { int n = fj[c]; g = n >= 0 && n < (int)newIx.size() ? newIx[n] : -1; }
            else if (fj[c].is_array()) g = nearestAlive(rootOf(fj[c]), tol);
            if (g < 0) { ok = false; break; }
            f.g[c] = g;
            auto it = uvOf.find(g);
            std::array<float, 2> uv = it != uvOf.end() ? it->second : std::array<float, 2>{0.5f, 0.5f};
            f.uv[2 * c] = uv[0];
            f.uv[2 * c + 1] = uv[1];
            if (op.contains("face_uv") && nf < op["face_uv"].size() && op["face_uv"][nf].size() == 6) {   // per-corner uv of the snapshot
                f.uv[2 * c] = op["face_uv"][nf][2 * c].get<float>();
                f.uv[2 * c + 1] = op["face_uv"][nf][2 * c + 1].get<float>();
            }
        }
        if (ok) faces.push_back(f); else unmatched++;
    }
}

void Groom::applyOp(const json& op) {
    std::string kind = op.contains("type") ? op["type"].get<std::string>() : op.value("op", "");
    if (kind == "mirror") { applyMirror(op); return; }
    if (kind == "rebuild") { applyRebuild(op); return; }
    if (kind == "resample") {
        // 2026-10-14 opt-in (Studio resample): EVERY guide gets n CVs, evenly spaced along the arc of the smooth
        // (centripetal Catmull-Rom) curve through its old CVs, root and tip exact. The Studio keeps one CV count for all
        // guides, so the op has no roots; later ops use n points. Studio-only (the HairLab pipeline does not know it;
        // Studio exports write the guides shown, any CV count).
        int n = std::clamp(op.value("n", K), 3, kMaxCvs);
        if (n == K) return;
        for (auto& G : P) G = resampleSmooth(G, n);
        K = n;
        return;
    }
    if (kind == "duplicate") {
        if (!op.contains("root")) return;
        auto g = match(json::array({op["root"]}));
        if (g.empty()) return;
        V3 d{0, 0, 0};
        if (op.contains("d")) d = {op["d"][0].get<float>(), op["d"][1].get<float>(), op["d"][2].get<float>()};
        Guide c = P[g[0]];
        for (auto& p : c) p = p + d;
        P.push_back(c);
        alive.push_back(1);
        src.push_back(g[0]);
        int ni = (int)P.size() - 1;
        edited.insert(ni);
        float fuv[2];
        const json eu = op.contains("edge_uv") ? op["edge_uv"] : json();
        bool fix = eu.is_array() && eu.size() == 2;
        if (fix) { fuv[0] = eu[0].get<float>(); fuv[1] = eu[1].get<float>(); }
        float nuv[2];
        const json nu = op.contains("new_uv") ? op["new_uv"] : json();
        bool hasNu = nu.is_array() && nu.size() == 2;
        if (hasNu) { nuv[0] = nu[0].get<float>(); nuv[1] = nu[1].get<float>(); }
        insertGuide(ni, op.value("edge_face", false), eu.is_string() && eu.get<std::string>() == "src", fix ? fuv : nullptr, hasNu ? nuv : nullptr);
        return;
    }
    json roots = op.contains("roots") ? op["roots"] : (op.contains("root") ? json::array({op["root"]}) : json::array());
    if (roots.empty()) return;
    if (kind == "face_uv") {
        // 2026-10-15 opt-in (Studio face_texel): the growth-face corners of these guides take uv [u, v], i.e. another
        // texel of the fur's maps (root / tip colour, density, width, clump) = per-area colour / density control.
        // corners "guide" (default): only the corners at these guides (the hair interpolated toward neighbours blends
        // smoothly); "face": all 3 corners of every face touching them. Studio exports write the faces as shown; the
        // HairLab pipeline ignores this op.
        if (!op.contains("uv") || !op["uv"].is_array() || op["uv"].size() != 2) return;
        auto g = match(roots);
        if (g.empty()) return;
        std::set<int> sel(g.begin(), g.end());
        float u = op["uv"][0].get<float>(), v = op["uv"][1].get<float>();
        bool whole = op.value("corners", std::string("guide")) == "face";
        for (auto& F : faces) {
            bool any = false;
            for (int q = 0; q < 3; q++) any = any || sel.count(F.g[q]);
            if (!any) continue;
            for (int q = 0; q < 3; q++)
                if (whole || sel.count(F.g[q])) { F.uv[2 * q] = u; F.uv[2 * q + 1] = v; }
        }
        return;
    }
    if (kind == "delete_face") {
        // 2026-10-12 opt-in (Studio trim_growth): remove the growth face of these 3 guide roots (no hair is interpolated
        // there any more). Studio exports write the faces as shown; the HairLab pipeline ignores this op.
        if (roots.size() != 3) return;
        auto g3 = match(roots);
        if (g3.size() != 3) return;
        std::sort(g3.begin(), g3.end());
        faces.erase(std::remove_if(faces.begin(), faces.end(), [&](const Face& f) {
                        std::array<int, 3> q = f.g;
                        std::sort(q.begin(), q.end());
                        return q[0] == g3[0] && q[1] == g3[1] && q[2] == g3[2];
                    }), faces.end());
        return;
    }
    auto g = match(roots);
    if (g.empty()) return;
    if (kind == "curl") {
        CurlParams c = CurlParams::fromJson(op.value("params", json::object()));
        for (int i : g) {
            json r = {r4(P[i][0].x), r4(P[i][0].y), r4(P[i][0].z)};
            unsigned seed = unsigned(std::fabs(P[i][0].x) * 1e5f) * 73856093u ^ unsigned(std::fabs(P[i][0].y) * 1e5f) * 19349663u ^
                            unsigned(std::fabs(P[i][0].z) * 1e5f) * 83492791u;
            P[i] = applyCurl(P[i], c, pack->headC, pack->headTop, seed);
            json pts = json::array();
            for (auto& q : P[i]) pts.push_back({r4(q.x), r4(q.y), r4(q.z)});
            baked.push_back({{"type", "reshape"}, {"root", r}, {"points", pts}});
            edited.insert(i);
        }
        return;
    }
    if (kind == "reshape") {
        int i = g[0];
        // 2026-10-17 opt-in "dup": k (> 0) = the k-th alive guide (index order) whose root is the SAME point as guide i
        // (stacked copies share one root, so a root-addressed reshape always hit the first; push_out fix_spans)
        if (op.contains("dup") && op["dup"].get<int>() > 0) {
            std::vector<int> same;
            for (size_t j = 0; j < P.size(); j++)
                if (alive[j] && len(P[j][0] - P[i][0]) < 2e-4f) same.push_back((int)j);
            int k = op["dup"].get<int>();
            if (k >= (int)same.size()) return;
            i = same[k];
        }
        auto& pts = op["points"];
        if ((int)pts.size() == K)
            for (int k = 0; k < K; k++) P[i][k] = {pts[k][0].get<float>(), pts[k][1].get<float>(), pts[k][2].get<float>()};
        g = {i};
    } else if (kind == "translate") {
        V3 off{op["offset"][0].get<float>(), op["offset"][1].get<float>(), op["offset"][2].get<float>()};
        auto w = falloffWeights(K, op.value("falloff", "carry"), op.value("from", 0));
        for (int i : g)
            for (int k = 0; k < K; k++) P[i][k] = P[i][k] + off * w[k];
    } else if (kind == "set_point") {
        int i = g[0];
        int k = std::clamp(op.value("index", 0), 0, K - 1);
        V3 pos{op["pos"][0].get<float>(), op["pos"][1].get<float>(), op["pos"][2].get<float>()};
        V3 dl = pos - P[i][k];
        int end = op.value("carry", true) ? K : k + 1;
        for (int j = k; j < end; j++) P[i][j] = P[i][j] + dl;
        g = {i};
    } else if (kind == "shorten") {
        for (int i : g) P[i] = shortenGuide(P[i], op.value("keep", 0.8f));
    } else if (kind == "delete") {
        removeGuides(g);
    } else {
        return;
    }
    for (int i : g) edited.insert(i);
}

void Groom::replay() {
    ck.valid = false;
    P = pack->guidesEdit;
    K = pack->K;
    alive.assign(P.size(), 1);
    src.assign(P.size(), -1);
    edited.clear();
    unmatched = 0;
    faces.clear();
    for (size_t f = 0; f < pack->F.size(); f++) faces.push_back({pack->F[f], pack->uv[f]});
    baked.clear();
    for (auto& o : preOps) applyOp(o);
    for (auto& o : ops) {
        std::string k = o.contains("type") ? o["type"].get<std::string>() : o.value("op", "");
        if (k == "mirror" && (!o.contains("source") || !o["source"].is_string() || !o.contains("source_roots"))) {
            json r = resolveMirror(o);       // explicit form so a shared pack replays identically (plane, side, source guide roots)
            baked.push_back(r);
            applyOp(r);
            continue;
        }
        if (k != "curl") baked.push_back(o);
        applyOp(o);
    }
    stOps = ops.size();
    stLast = ops.empty() ? json() : ops.back();
}

void Groom::checkpoint() {
    if (!(stOps == ops.size() && (ops.empty() || ops.back() == stLast))) replay();
    ck.valid = true;
    ck.nOps = ops.size();
    ck.P = P; ck.alive = alive; ck.src = src; ck.edited = edited; ck.faces = faces; ck.nBaked = baked.size(); ck.unmatched = unmatched;
}

void Groom::replayTail(size_t from) {
    if (!ck.valid || from != ck.nOps || ops.size() < from || baked.size() < ck.nBaked) { replay(); return; }
    P = ck.P; alive = ck.alive; src = ck.src; edited = ck.edited; faces = ck.faces; unmatched = ck.unmatched;
    baked.resize(std::min(baked.size(), ck.nBaked));   // tail ops only append to baked: drop the previous tail's entries
    for (size_t n = from; n < ops.size(); n++) {
        auto& o = ops[n];
        std::string k = o.contains("type") ? o["type"].get<std::string>() : o.value("op", "");
        if (k == "mirror" && (!o.contains("source") || !o["source"].is_string() || !o.contains("source_roots"))) {
            json r = resolveMirror(o);
            baked.push_back(r);
            applyOp(r);
            continue;
        }
        if (k != "curl") baked.push_back(o);
        applyOp(o);
    }
    stOps = ops.size();
    stLast = ops.empty() ? json() : ops.back();
}

void Groom::push(json op) {
    redo.clear();
    ops.push_back(std::move(op));
    replay();
}

void Groom::undo() {
    if (ops.empty()) return;
    json o = ops.back();
    ops.pop_back();
    redo.push_back(o);
    if (o.contains("grp"))
        while (!ops.empty() && ops.back().contains("grp") && ops.back()["grp"] == o["grp"]) { redo.push_back(ops.back()); ops.pop_back(); }
    replay();
}

void Groom::redoOp() {
    if (redo.empty()) return;
    json o = redo.back();
    redo.pop_back();
    ops.push_back(o);
    if (o.contains("grp"))
        while (!redo.empty() && redo.back().contains("grp") && redo.back()["grp"] == o["grp"]) { ops.push_back(redo.back()); redo.pop_back(); }
    replay();
}

json Groom::rootsOf(const std::vector<int>& ix) const {
    json a = json::array();
    for (int i : ix) a.push_back({r4(P[i][0].x), r4(P[i][0].y), r4(P[i][0].z)});
    return a;
}

json Groom::editsDoc(bool bake) const {
    json d;
    d["version"] = 1;
    d["character"] = pack->character;
    d["source"] = pack->config;
    d["comp"] = pack->comp;
    d["tol"] = std::round(double(tol) * 1e6) / 1e6;
    d["note"] = "Strand Editor export (Desktop Groom Studio). Copy to tools/hairworks/edits/" + pack->character + "_<tag>.json";
    json all = json::array();
    for (auto& o : preOps) all.push_back(o);
    for (auto& o : (bake ? baked : ops)) {
        json c = o;
        c.erase("grp");        // editor-only grouping
        all.push_back(c);
    }
    d["ops"] = all;
    return d;
}

std::string Groom::editsText(bool bake) const {
    json d = editsDoc(bake);
    json opsArr = d["ops"];
    d.erase("ops");
    std::string h = d.dump();
    h.pop_back();
    std::ostringstream s;
    s << h << ",\"ops\":[";
    for (size_t i = 0; i < opsArr.size(); i++) s << (i ? ",\n  " : "\n  ") << opsArr[i].dump();
    s << (opsArr.empty() ? "" : "\n") << "]}\n";
    return s.str();
}
