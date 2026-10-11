// Desktop Groom Studio - curl helpers. Our own code (MIT).
#include "curls.h"

#include <algorithm>
#include <cmath>

static const float PI = 3.14159265f;

static V3 norm(V3 a) { float l = len(a); return l > 1e-9f ? a * (1.0f / l) : V3{0, 0, 1}; }
static V3 rotate(V3 v, V3 axis, float ang) {   // Rodrigues
    float c = std::cos(ang), s = std::sin(ang);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1 - c));
}
static float hash01(unsigned x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return (x & 0xffffff) / float(0x1000000);
}

json CurlParams::toJson() const {
    json j = {{"mode", mode}, {"start", start}, {"start_below_top", startBelowTop}, {"radius", radius}, {"turns", turns},
            {"ramp", ramp}, {"shrink", shrink}, {"jitter", jitter}, {"hand", hand}, {"side", side}, {"band", band},
            {"zigzag", zigzag}, {"axis", {axis.x, axis.y, axis.z}}};
    auto rd = [](double v) { return std::round(v * 1e5) / 1e5; };
    for (auto& [k, v] : j.items()) {
        if (v.is_number_float()) v = rd(v.get<double>());
        if (v.is_array()) for (auto& x : v) if (x.is_number_float()) x = rd(x.get<double>());
    }
    return j;
}

CurlParams CurlParams::fromJson(const json& j) {
    CurlParams c;
    c.mode = j.value("mode", c.mode);
    c.start = j.value("start", c.start);
    c.startBelowTop = j.value("start_below_top", c.startBelowTop);
    c.radius = j.value("radius", c.radius);
    c.turns = j.value("turns", c.turns);
    c.ramp = j.value("ramp", c.ramp);
    c.shrink = j.value("shrink", c.shrink);
    c.jitter = j.value("jitter", c.jitter);
    c.hand = j.value("hand", c.hand);
    c.side = j.value("side", c.side);
    c.band = j.value("band", c.band);
    c.zigzag = j.value("zigzag", c.zigzag);
    if (j.contains("axis") && j["axis"].is_array()) c.axis = {j["axis"][0], j["axis"][1], j["axis"][2]};
    return c;
}

float curlCvsPerTurn(int K, const CurlParams& c) {
    float n = (K - 1) * (1.0f - c.start);
    return c.turns > 0 ? n / c.turns : 1e9f;
}

static int startIndex(const Guide& p, const CurlParams& c, float headTop, std::vector<float>& s) {
    int K = (int)p.size();
    s.assign(K, 0.0f);
    for (int k = 1; k < K; k++) s[k] = s[k - 1] + len(p[k] - p[k - 1]);
    int i0 = int(std::lower_bound(s.begin(), s.end(), c.start * s.back()) - s.begin());
    if (c.mode == "roll" && c.startBelowTop >= 0) {
        int lo = K;
        for (int k = 0; k < K; k++) if (p[k].z < headTop - c.startBelowTop) { lo = k; break; }
        i0 = std::max(i0, lo);
    }
    return std::clamp(i0, 1, K - 2);
}

// keep every segment's length: walk the target shape and re-place each point at the original segment length
static Guide restoreLengths(const Guide& orig, const Guide& target) {
    Guide q = target;
    q[0] = orig[0];
    for (size_t k = 1; k < q.size(); k++) {
        float L = len(orig[k] - orig[k - 1]);
        q[k] = q[k - 1] + norm(target[k] - q[k - 1]) * L;
    }
    return q;
}

Guide applyCurl(const Guide& p, const CurlParams& c, V3 headC, float headTop, unsigned seed) {
    const int K = (int)p.size();
    if (K < 4) return p;
    std::vector<float> s;
    int i0 = startIndex(p, c, headTop, s);
    float L = s.back();
    if (L < 0.02f) return p;
    float jit = c.jitter * (hash01(seed) - 0.5f) * 2.0f;           // -1..1 scaled
    float phase = c.jitter * hash01(seed * 7919u + 13u) * 2 * PI;
    float R = c.radius * (1.0f + 0.35f * jit);
    Guide out = p;
    if (c.mode == "roll") {
        // bend: from i0 on, each segment turns by its length / r(phi) around a horizontal axis (constant curvature
        // spiral whose radius shrinks to R * shrink), so the end rolls on itself; lengths are kept by construction
        V3 t = norm(p[i0] - p[i0 - 1]);
        V3 out_;
        if (c.band) out_ = {0, -1, 0};
        else { out_ = p[i0] - headC; out_.z = 0; out_ = norm(out_); }
        V3 axis = (len(c.axis) > 1e-6f) ? norm(c.axis) : norm(cross(t, out_));
        if (len(cross(t, out_)) < 0.2f && len(c.axis) < 1e-6f) return p;
        float sgn = float(c.side < 0 ? -1 : 1);
        float total = 2 * PI * c.turns, phi = 0;
        for (int k = i0; k < K; k++) {
            V3 seg = p[k] - p[k - 1];
            float Lk = len(seg);
            float frac = std::min(phi / std::max(total, 1e-3f), 1.0f);
            float r = std::max(R * (1.0f - (1.0f - c.shrink) * frac), 0.002f);
            phi = std::min(phi + Lk / r, total);
            V3 dir = rotate(norm(seg), axis, sgn * phi);
            out[k] = out[k - 1] + dir * Lk;
        }
        return out;
    }
    // ringlet / wave: offset around the strand's own centre line in a parallel-transport frame, then restore lengths
    Guide target = p;
    V3 t0 = norm(p[std::min(i0 + 1, K - 1)] - p[i0 - 1]);
    V3 ref = std::fabs(t0.z) < 0.9f ? V3{0, 0, 1} : V3{1, 0, 0};
    V3 u = norm(cross(t0, ref));
    if (c.mode == "wave") {
        // wave plane: horizontal, tangent to the head (side flips it)
        V3 rad = p[i0] - headC; rad.z = 0;
        u = norm(cross(V3{0, 0, 1}, norm(rad)));
        if (c.side > 0) u = u * -1.0f;
    }
    for (int k = i0; k < K; k++) {
        float tt = (s[k] - s[i0]) / std::max(L - s[i0], 1e-6f);   // 0..1 along the curled part
        V3 tk = norm(p[std::min(k + 1, K - 1)] - p[k - 1]);
        // transport u to be perpendicular to the local tangent
        u = norm(u - tk * dot(u, tk));
        V3 v = cross(tk, u);
        float ramp = c.ramp > 0 ? std::min(tt / c.ramp, 1.0f) : 1.0f;
        if (c.mode == "ringlet") {
            float th = float(c.hand) * (2 * PI * c.turns * tt + phase);
            target[k] = p[k] + (u * std::cos(th) + v * std::sin(th) - u) * (R * ramp);
        } else {
            float x = c.turns * tt + phase / (2 * PI);
            float w = c.zigzag ? (4.0f * std::fabs(x - std::floor(x + 0.5f)) - 1.0f) : std::sin(2 * PI * x);
            target[k] = p[k] + u * (w * R * ramp);
        }
    }
    return restoreLengths(p, target);
}
