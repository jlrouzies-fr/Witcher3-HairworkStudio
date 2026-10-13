// ------------------------------------------------------------------------------------------------ r3 tools
// 2026-10-12 opt-in Studio tools (session r3_a), included by serve.cpp after the t6 helpers. All write plain undoable ops
// (reshape / delete_face / rebuild); nothing changes unless the command is called.
//   regrow             rebuild_from_cards + exclusion zones (ears, spheres, front hairline inset), zigzag lanes replaced,
//                      strands passing in front of / over an ear routed BEHIND it
//   route_behind_ears  the same ear routing on existing guides
//   twist_roll         gather guides into a rolled twist along a path (nape roll ending at a point)
//   plant_fringe       new short guides from the front hairline lying on the forehead (fringe / face-framing locks)
//   parting_roots      new roots in rows along a parting line, laid away from it on both sides; growth faces never
//                      cross the line (a clean parting without bare scalp)
namespace r3 {
using rebuild::Grid;
using t6::HeadSkin;
using t6::Zones;

static V3 j3(const json& j) { return V3{j[0].get<float>(), j[1].get<float>(), j[2].get<float>()}; }
static float hash01(V3 p, unsigned salt = 0) {
    unsigned h = unsigned(int(p.x * 1e4f)) * 73856093u ^ unsigned(int(p.y * 1e4f)) * 19349663u ^ unsigned(int(p.z * 1e4f)) * 83492791u ^ (salt * 2654435761u);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (h & 0xffffff) / float(0x1000000);
}
// opt-in bigger ear boxes: ear_lat_mm (head vertices within this of the most lateral point at ear height; zonesOf uses 12,
// too thin for long / pointed ears), ear_top_mm (above the eyes), ear_box [[lo],[hi]] (explicit, character's left ear;
// mirrored in x about the head centre for the other side)
static void earOverride(const App& a, Zones& z, const json& c) {
    if (c.contains("ear_box")) {
        V3 lo = V3{c["ear_box"][0][0].get<float>(), c["ear_box"][0][1].get<float>(), c["ear_box"][0][2].get<float>()};
        V3 hi = V3{c["ear_box"][1][0].get<float>(), c["ear_box"][1][1].get<float>(), c["ear_box"][1][2].get<float>()};
        z.earLo[0] = lo; z.earHi[0] = hi;
        z.earLo[1] = {2 * z.C.x - hi.x, lo.y, lo.z}; z.earHi[1] = {2 * z.C.x - lo.x, hi.y, hi.z};
        z.hasEar[0] = z.hasEar[1] = true;
        return;
    }
    if (!c.contains("ear_lat_mm")) return;
    float latW = c["ear_lat_mm"].get<float>() / 1000, top = c.value("ear_top_mm", 50.0f) / 1000;
    for (int sd = 0; sd < 2; sd++) {
        float best = 0;
        auto lat = [&](V3 v) { return sd == 0 ? v.x - z.C.x : z.C.x - v.x; };
        auto back = [&](V3 v) { return z.fwd * (v.y - z.eyeY); };
        for (auto& m : a.pack.meshes) if (m.kind == "head") for (auto& v : m.v) {
            if (v.z < z.eyeZ - 0.06f || v.z > z.eyeZ + 0.015f || back(v) > -0.05f || back(v) < -0.15f) continue;
            best = std::max(best, lat(v));
        }
        if (best <= 0) continue;
        V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        bool any = false;
        for (auto& m : a.pack.meshes) if (m.kind == "head") for (auto& v : m.v) {
            if (lat(v) < best - latW || v.z < z.eyeZ - 0.09f || v.z > z.eyeZ + top || back(v) > -0.04f || back(v) < -0.17f) continue;
            lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
            hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
            any = true;
        }
        if (any) { z.earLo[sd] = lo; z.earHi[sd] = hi; z.hasEar[sd] = true; }
    }
}
// project a point onto the skin + standoff (nearest-vertex normal)
static V3 onSkin(const HeadSkin& hs, V3 p, float standoff) {
    V3 n;
    float sd = hs.sd(p, &n);
    return p - n * (sd - standoff);
}
// keep every point (not the root) at least standoff above the skin (head mesh incl. neck: works below the head centre)
static int skinOut(const HeadSkin& hs, Guide& G, float standoff) {
    int n = 0;
    for (size_t k = 1; k < G.size(); k++) {
        V3 nn;
        float sd = hs.sd(G[k], &nn);
        if (sd < standoff && sd > -0.05f) { G[k] = G[k] + nn * (standoff - sd); n++; }
    }
    return n;
}
// walk along the skin from p: each step goes along dir(p, n) projected on the tangent plane, then back onto skin+standoff
template <class F> static std::vector<V3> skinWalk(const HeadSkin& hs, V3 p, float length, float step, float standoff, F dirAt) {
    std::vector<V3> out;
    p = onSkin(hs, p, standoff);
    out.push_back(p);
    float s = 0;
    while (s < length - 1e-5f) {
        float h = std::min(step, length - s);
        V3 n;
        hs.sd(p, &n);
        V3 d = dirAt(p, n, s);
        d = d - n * dot(d, n);
        if (len(d) < 1e-6f) break;
        V3 q = onSkin(hs, p + nrmz(d) * h, standoff);
        q = p + nrmz(q - p) * h;   // exact step length
        out.push_back(q);
        p = q; s += h;
    }
    return out;
}
// resample a polyline at given arc positions (absolute, m)
static Guide sampleAt(const std::vector<V3>& q, const std::vector<float>& at) {
    std::vector<float> d(q.size(), 0.0f);
    for (size_t i = 1; i < q.size(); i++) d[i] = d[i - 1] + len(q[i] - q[i - 1]);
    Guide out;
    size_t j = 1;
    for (float t : at) {
        t = std::clamp(t, 0.0f, d.back());
        while (j < q.size() - 1 && d[j] < t) j++;
        if (q.size() < 2) { out.push_back(q[0]); continue; }
        float a = std::clamp((t - d[j - 1]) / std::max(d[j] - d[j - 1], 1e-12f), 0.0f, 1.0f);
        out.push_back(q[j - 1] + (q[j] - q[j - 1]) * a);
    }
    return out;
}
static std::vector<float> uniformAt(float L, int K, float bias = 1.0f) {
    std::vector<float> at(K);
    for (int k = 0; k < K; k++) at[k] = L * std::pow(float(k) / (K - 1), bias);
    return at;
}
static Guide smoothKeep(const Guide& G, int passes, float w = 0.5f) {
    auto sl = arcLen(G);
    Guide Q = G;
    for (int it = 0; it < passes; it++) { Guide T = Q; for (size_t k = 1; k + 1 < Q.size(); k++) T[k] = Q[k] * (1 - w) + (Q[k - 1] + Q[k + 1]) * (w / 2); Q = T; }
    Guide R = Q;
    for (size_t k = 1; k < G.size(); k++) R[k] = R[k - 1] + nrmz(Q[k] - R[k - 1]) * (sl[k] - sl[k - 1]);
    return R;
}

// ---- growth faces for new guides: Delaunay of the new roots + nearby alive roots in the polar map (as rebuild_from_cards)
struct StitchOpt {
    float edgeMm = 22, rmaxMm = 16, zoneMm = 30;
    bool oldOnly = false;                    // also old-only triangles in the zone that no remaining old face covers
    std::function<bool(V3, V3)> edgeOk;      // e.g. a parting: false = this edge may not exist
    json uv;                                 // [u, v] for every new guide (default: typical hair texel)
};
// returns the rebuild op (guides without any face are dropped and reported)
static json stitchOp(App& a, const std::vector<Guide>& NG, const StitchOpt& o, int& id, int grp, json& stats, const std::set<int>& deleteGuides = {},
                     const std::vector<std::array<int, 3>>& deleteFaces = {}) {
    using rebuild::delaunay;
    Groom& gr = a.groom;
    const V3 C = a.pack.headC;
    std::set<std::array<int, 3>> delF;
    for (auto f : deleteFaces) { std::sort(f.begin(), f.end()); delF.insert(f); }
    std::vector<V3> RR; std::vector<int> who;   // >= 0 new index, < 0 -(old + 1)
    float zone = o.zoneMm / 1000;
    for (size_t g = 0; g < gr.P.size(); g++) {
        if (!gr.alive[g] || deleteGuides.count((int)g)) continue;
        bool nearNew = false;
        for (auto& G : NG) if (len(G[0] - gr.P[g][0]) < zone + 0.02f) { nearNew = true; break; }
        if (nearNew) { RR.push_back(gr.P[g][0]); who.push_back(-int(g) - 1); }
    }
    size_t nOld = RR.size();
    for (size_t i = 0; i < NG.size(); i++) { RR.push_back(NG[i][0]); who.push_back((int)i); }
    std::vector<std::array<double, 2>> xy;
    for (auto& p : RR) {
        V3 d = p - C; float rr = std::max(len(d), 1e-6f);
        double th = std::acos(std::clamp(d.z / rr, -1.0f, 1.0f)), ph = std::atan2(d.y, d.x);
        xy.push_back({th * std::cos(ph), th * std::sin(ph)});
    }
    std::map<int, int> posOf;
    for (size_t i = 0; i < nOld; i++) posOf[-who[i] - 1] = (int)i;
    std::vector<std::array<std::array<double, 2>, 3>> keptOld;
    for (auto& f : gr.faces) {
        if (deleteGuides.count(f.g[0]) || deleteGuides.count(f.g[1]) || deleteGuides.count(f.g[2])) continue;
        auto q = f.g; std::sort(q.begin(), q.end());
        if (delF.count(q)) continue;
        int n = posOf.count(f.g[0]) + posOf.count(f.g[1]) + posOf.count(f.g[2]);
        if (n < 3) continue;
        keptOld.push_back({xy[posOf[f.g[0]]], xy[posOf[f.g[1]]], xy[posOf[f.g[2]]]});
    }
    auto insideOld = [&](double x, double y) {
        for (auto& t : keptOld) {
            double d1 = (x - t[1][0]) * (t[0][1] - t[1][1]) - (t[0][0] - t[1][0]) * (y - t[1][1]);
            double d2 = (x - t[2][0]) * (t[1][1] - t[2][1]) - (t[1][0] - t[2][0]) * (y - t[2][1]);
            double d3 = (x - t[0][0]) * (t[2][1] - t[0][1]) - (t[2][0] - t[0][0]) * (y - t[0][1]);
            bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
            if (!(neg && pos)) return true;
        }
        return false;
    };
    float edgeAbs = o.edgeMm / 1000, rmax = o.rmaxMm / 1000;
    json faces = json::array();
    std::vector<char> used(NG.size(), 0);
    int nOldOnly = 0, rejEdge = 0;
    for (auto& t : delaunay(xy)) {
        bool anyNew = who[t[0]] >= 0 || who[t[1]] >= 0 || who[t[2]] >= 0;
        V3 A = RR[t[0]], B = RR[t[1]], D = RR[t[2]];
        if (!anyNew) {
            if (!o.oldOnly) continue;
            V3 cen = (A + B + D) * (1.0f / 3);
            bool nearNew = false;
            for (auto& G : NG) if (len(G[0] - cen) < zone) { nearNew = true; break; }
            if (!nearNew) continue;
            std::array<int, 3> q = {-who[t[0]] - 1, -who[t[1]] - 1, -who[t[2]] - 1}; std::sort(q.begin(), q.end());
            bool exists = false;
            for (auto& f : gr.faces) { auto qq = f.g; std::sort(qq.begin(), qq.end()); if (qq == q && !delF.count(qq)) { exists = true; break; } }
            if (exists) continue;
        }
        float la = len(B - D), lb = len(A - D), lc = len(A - B);
        float e = std::max({la, lb, lc}), ar = 0.5f * len(cross(B - A, D - A));
        if (e > edgeAbs || ar <= 1e-7f || la * lb * lc / std::max(4 * ar, 1e-12f) > rmax) continue;
        if (o.edgeOk && (!o.edgeOk(A, B) || !o.edgeOk(B, D) || !o.edgeOk(A, D))) { rejEdge++; continue; }
        if (!keptOld.empty() && insideOld((xy[t[0]][0] + xy[t[1]][0] + xy[t[2]][0]) / 3, (xy[t[0]][1] + xy[t[1]][1] + xy[t[2]][1]) / 3)) continue;
        std::array<int, 3> q = t;
        if (dot(A - C, cross(B - A, D - A)) < 0) std::swap(q[1], q[2]);
        json fj = json::array();
        for (int v : q) {
            if (who[v] >= 0) { fj.push_back(who[v]); used[who[v]] = 1; }
            else { V3 p = gr.P[-who[v] - 1][0]; fj.push_back({r4(p.x), r4(p.y), r4(p.z)}); }
        }
        if (!anyNew) nOldOnly++;
        faces.push_back(fj);
    }
    std::vector<int> remap(NG.size(), -1);
    json guides = json::array(), base = json::array(), uvs = json::array();
    json uv = o.uv.is_array() ? o.uv : t6::typicalUv(a);
    if (!uv.is_array()) uv = json::array({0.5, 0.5});
    int orphan = 0;
    for (size_t i = 0; i < NG.size(); i++) {
        if (!used[i]) { orphan++; continue; }
        remap[i] = (int)guides.size();
        guides.push_back(ptsJson(NG[i]));
        int bg = -1; float bd = 1e9f;
        for (size_t g = 0; g < gr.P.size(); g++) if (gr.alive[g]) { float d = len(gr.P[g][0] - NG[i][0]); if (d < bd) { bd = d; bg = (int)g; } }
        V3 bp = bg >= 0 ? gr.P[bg][0] : NG[i][0];
        base.push_back({r4(bp.x), r4(bp.y), r4(bp.z)});
        uvs.push_back(uv);
    }
    json keepFaces = json::array();
    for (auto& f : faces) {
        bool ok = true;
        for (auto& v : f) if (v.is_number_integer()) { int m = remap[v.get<int>()]; if (m < 0) ok = false; else v = m; }
        if (ok) keepFaces.push_back(f);
    }
    json del = json::array();
    for (int g : deleteGuides) del.push_back(t6::rootJ(gr.P[g][0]));
    stats = {{"new_guides", guides.size()}, {"new_faces", keepFaces.size()}, {"old_only_faces", nOldOnly}, {"no_face_dropped", orphan},
             {"edges_rejected", rejEdge}, {"uv", uv}};
    return json{{"op", "rebuild"}, {"delete", del}, {"guides", guides}, {"base", base}, {"uv", uvs}, {"faces", keepFaces}, {"id", ++id}, {"grp", grp}};
}

// ---- ears: route strand points that pass in front of / over an ear behind it (lengths kept)
static int routeBehindEars(const Zones& z, Guide& G, float pad, float behindMm, const HeadSkin& hs) {
    bool any = false;
    Guide Q = G;
    for (int sd = 0; sd < 2; sd++) {
        if (!z.hasEar[sd]) continue;
        V3 lo = z.earLo[sd], hi = z.earHi[sd];
        // back edge of the ear (fwd = +1: smallest y)
        float backY = z.fwd > 0 ? lo.y : hi.y, frontY = z.fwd > 0 ? hi.y : lo.y;
        float inner = sd == 0 ? lo.x : hi.x;   // the ear's skull-side face
        for (size_t k = 1; k < Q.size(); k++) {
            V3 p = Q[k];
            float lat = sd == 0 ? p.x - (inner - 0.004f) : (inner + 0.004f) - p.x;   // > 0: outside the skull side of the ear
            if (lat < 0) continue;
            if (p.z < lo.z - pad || p.z > hi.z + pad) continue;
            float fw = z.fwd * (p.y - backY);   // > 0 = in front of the ear's back edge
            if (fw < -pad * 0.5f || z.fwd * (p.y - frontY) > pad) continue;
            // weight: full inside the box in z, fading over pad
            float wz = p.z < lo.z ? 1 - (lo.z - p.z) / pad : p.z > hi.z ? 1 - (p.z - hi.z) / pad : 1.0f;
            wz = std::clamp(wz, 0.0f, 1.0f);
            float target = backY - z.fwd * behindMm / 1000;
            Q[k].y = p.y + (target - p.y) * wz;
            any = true;
        }
    }
    if (!any) return 0;
    auto sl = arcLen(G);
    for (int it = 0; it < 3; it++) { Guide T = Q; for (size_t k = 1; k + 1 < Q.size(); k++) T[k] = Q[k] * 0.5f + (Q[k - 1] + Q[k + 1]) * 0.25f; Q = T; }
    Guide R = Q;
    for (size_t k = 1; k < G.size(); k++) R[k] = R[k - 1] + nrmz(Q[k] - R[k - 1]) * (sl[k] - sl[k - 1]);
    skinOut(hs, R, 0.003f);
    G = R;
    return 1;
}
// split routing: a strand entering the ear zone in front of the ear's middle (+ bias) goes IN FRONT of the ear, the others
// behind it; returns +1 front / -1 behind / 0 untouched. Growth faces mixing +1 and -1 guides must go (HairWorks would
// interpolate hair across the ear).
static int routeSplitEars(const Zones& z, Guide& G, float pad, float gapMm, float biasMm, const HeadSkin& hs, int passes = 1) {
    int cls = 0, sides[2] = {0, 0};
    for (int pass = 0; pass < std::max(1, passes); pass++) {
    Guide Q = G;
    bool moved = false;
    for (int sd = 0; sd < 2; sd++) {
        if (!z.hasEar[sd]) continue;
        V3 lo = z.earLo[sd], hi = z.earHi[sd];
        float backY = z.fwd > 0 ? lo.y : hi.y, frontY = z.fwd > 0 ? hi.y : lo.y, midY = (lo.y + hi.y) / 2;
        float inner = sd == 0 ? lo.x : hi.x;
        int& side = sides[sd];
        for (size_t k = 1; k < Q.size(); k++) {
            V3 p = Q[k];
            float lat = sd == 0 ? p.x - (inner - 0.004f) : (inner + 0.004f) - p.x;
            if (lat < 0 || p.z < lo.z - pad || p.z > hi.z + pad) continue;
            if (z.fwd * (p.y - backY) < -pad * 0.5f || z.fwd * (p.y - frontY) > pad) continue;
            if (!side) side = z.fwd * (p.y - midY) > biasMm / 1000 ? 1 : -1;
            float wz = p.z < lo.z ? 1 - (lo.z - p.z) / pad : p.z > hi.z ? 1 - (p.z - hi.z) / pad : 1.0f;
            wz = std::clamp(wz, 0.0f, 1.0f);
            float target = side > 0 ? frontY + z.fwd * gapMm / 1000 : backY - z.fwd * gapMm / 1000;
            float dy = target - p.y;
            if (side > 0 ? z.fwd * dy < 0 : z.fwd * dy > 0) continue;   // already on its side
            Q[k].y = p.y + dy * (pass == 0 ? wz : std::max(wz, 0.5f));
            moved = true;
        }
        if (side) cls = side;
    }
    if (!cls) return 0;
    if (!moved) break;
    auto sl = arcLen(G);
    for (int it = 0; it < (pass == 0 ? 3 : 1); it++) { Guide T = Q; for (size_t k = 1; k + 1 < Q.size(); k++) T[k] = Q[k] * 0.5f + (Q[k - 1] + Q[k + 1]) * 0.25f; Q = T; }
    Guide R = Q;
    for (size_t k = 1; k < G.size(); k++) R[k] = R[k - 1] + nrmz(Q[k] - R[k - 1]) * (sl[k] - sl[k - 1]);
    skinOut(hs, R, 0.003f);
    G = R;
    }
    return cls;
}
static bool crossesEar(const Zones& z, const Guide& G, float pad) {
    for (int sd = 0; sd < 2; sd++) {
        if (!z.hasEar[sd]) continue;
        V3 lo = z.earLo[sd], hi = z.earHi[sd];
        float inner = sd == 0 ? lo.x : hi.x;
        for (size_t k = 1; k < G.size(); k++) {
            V3 p = G[k];
            float lat = sd == 0 ? p.x - inner : inner - p.x;
            if (lat > -0.002f && p.y > lo.y - pad && p.y < hi.y + pad && p.z > lo.z - pad && p.z < hi.z + pad) return true;
        }
    }
    return false;
}

// ---- parting line helpers (polyline on the scalp)
struct Line {
    std::vector<V3> p;
    V3 C{};
    // nearest point, signed side (+1 / -1), distance, tangent, side direction (unit, tangent to the scalp)
    void closest(V3 q, V3& foot, float& dist, float& side, V3& tan, V3& sdir, float* arc = nullptr) const {
        float bd = 1e9f, acc = 0, bestArc = 0;
        for (size_t i = 0; i + 1 < p.size(); i++) {
            V3 a = p[i], b = p[i + 1], ab = b - a;
            float L2 = std::max(dot(ab, ab), 1e-12f), t = std::clamp(dot(q - a, ab) / L2, 0.0f, 1.0f);
            V3 f = a + ab * t;
            float d = len(q - f);
            if (d < bd) { bd = d; foot = f; tan = nrmz(ab); bestArc = acc + t * std::sqrt(L2); }
            acc += std::sqrt(L2);
        }
        V3 n = nrmz(foot - C);
        sdir = nrmz(cross(tan, n));
        side = dot(q - foot, sdir) >= 0 ? 1.0f : -1.0f;
        dist = bd;
        if (arc) *arc = bestArc;
    }
    float length() const { float L = 0; for (size_t i = 0; i + 1 < p.size(); i++) L += len(p[i + 1] - p[i]); return L; }
    V3 at(float s, V3* tan = nullptr) const {
        for (size_t i = 0; i + 1 < p.size(); i++) {
            float L = len(p[i + 1] - p[i]);
            if (s <= L || i + 2 == p.size()) { if (tan) *tan = nrmz(p[i + 1] - p[i]); return p[i] + (p[i + 1] - p[i]) * std::clamp(s / std::max(L, 1e-9f), 0.0f, 1.0f); }
            s -= L;
        }
        return p.back();
    }
};

// lay a strand's first lay_m along the skin from its root in direction away (+ along the strand's own flow toward the end
// of the lay), the rest rejoins the old path (offset fading over rejoin_m)
static Guide layAway(const HeadSkin& hs, const Guide& G, V3 away, float layM, float rejoinM, float standoff, float steer, V3 flowEnd) {
    auto sl = arcLen(G);
    float L = sl.back();
    layM = std::min(layM, L * 0.7f);
    auto W = skinWalk(hs, G[0], layM, 0.004f, standoff, [&](V3 p, V3 n, float s) {
        float f = s / std::max(layM, 1e-4f);
        V3 d = away * (1 - steer * f) + flowEnd * (steer * f);
        return d;
    });
    std::vector<float> at;
    int K = (int)G.size();
    Guide Q = G;
    float wl = 0; for (size_t i = 1; i < W.size(); i++) wl += len(W[i] - W[i - 1]);
    V3 wEnd = W.back();
    // old path point at arc layM
    Guide oldAt = sampleAt(std::vector<V3>(G.begin(), G.end()), {layM});
    V3 off = wEnd - oldAt[0];
    for (int k = 1; k < K; k++) {
        if (sl[k] <= wl) Q[k] = sampleAt(W, {sl[k]})[0];
        else {
            float f = std::clamp((sl[k] - wl) / std::max(rejoinM, 1e-4f), 0.0f, 1.0f);
            float w = 1 - f * f * (3 - 2 * f);
            Q[k] = G[k] + off * w;
        }
    }
    Q[0] = G[0];
    Guide R = Q;
    for (int k = 1; k < K; k++) R[k] = R[k - 1] + nrmz(Q[k] - R[k - 1]) * (sl[k] - sl[k - 1]);
    return R;
}
}  // namespace r3

static json r3Command(App& a, const json& c, int& grpId, const std::string& d) {
    using namespace r3;
    json r;
    Groom& gr = a.groom;
    const DeskPack& pk = a.pack;
    const V3 C = pk.headC;
    const int K = gr.K;
    HeadSkin hs = t6::headSkin(a);
    if (!hs.m) { r["error"] = "pack has no head mesh"; return r; }
    Zones z = t6::zonesOf(a, c);
    earOverride(a, z, c);
    int id = t6::maxOpId(gr), grp = 1000 + (++grpId);

    // ------------------------------------------------------------------ hairnet: hair lies close to the skull (hood fix)
    if (d == "hairnet") {
        float maxM = c.value("max_mm", 8.0f) / 1000, slope = c.value("slope", 0.6f), rootPart = c.value("root_part", 0.0f);
        float zLo = z.eyeZ + c.value("z_lo_mm", -20.0f) / 1000;   // below it the limit grows by slope per m (hair hangs free)
        float strength = c.value("strength", 1.0f);
        bool skipFace = c.value("skip_face", true);
        auto ix = a.selectGuides(c.value("where", json::object({{"all", true}})));
        int n = 0, pts = 0; double sumMove = 0;
        std::vector<json> ops;
        float xAbsMin = c.value("x_abs_min_mm", 0.0f) / 1000;   // only guides rooted this far off the mid plane (sides)
        for (int g : ix) {
            Guide G = gr.P[g];
            if (std::fabs(G[0].x - C.x) < xAbsMin) continue;
            auto sl = arcLen(G);
            float L = sl.back();
            Guide Q = G;
            bool any = false;
            for (size_t k = 1; k < G.size(); k++) {
                if (sl[k] < rootPart * L) continue;
                V3 p = Q[k], nn;
                if (skipFace && z.overFace(p)) continue;
                float lim = maxM + (p.z < zLo ? (zLo - p.z) * slope : 0.0f);
                float sd = hs.sd(p, &nn);
                if (sd <= lim || sd > 0.2f) continue;
                V3 q = p - nn * ((sd - lim) * strength);
                sumMove += len(q - p); pts++;
                Q[k] = q; any = true;
            }
            if (!any) continue;
            for (int it = 0; it < 2; it++) { Guide T = Q; for (size_t k = 1; k + 1 < Q.size(); k++) T[k] = Q[k] * 0.5f + (Q[k - 1] + Q[k + 1]) * 0.25f; Q = T; }
            Guide R = Q;
            for (size_t k = 1; k < G.size(); k++) R[k] = R[k - 1] + nrmz(Q[k] - R[k - 1]) * (sl[k] - sl[k - 1]);
            skinOut(hs, R, 0.003f);
            ops.push_back({{"op", "reshape"}, {"root", t6::rootJ(G[0])}, {"points", ptsJson(R)}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        if (!c.value("list_only", false) && !ops.empty()) { for (auto& o : ops) gr.ops.push_back(o); gr.redo.clear(); gr.replay(); a.editsChanged(); }
        r["changed"] = n; r["points"] = pts; r["mean_move_mm"] = pts ? sumMove / pts * 1000 : 0.0;
        return r;
    }

    // ------------------------------------------------------------------ layer_lift: break a fused "clay shell"
    // every guide gets its own lift off the skin (lift_min_mm .. lift_max_mm, skewed low by gamma) and a slow sideways
    // sway (sway_mm, waves along the strand), both ramping in from the root over ramp (fraction of the length); length kept.
    // Neighbouring guides at different heights overlap and shade each other: layers and strand groups instead of one surface
    if (d == "layer_lift") {
        float lo = c.value("lift_min_mm", 0.0f) / 1000, hi = c.value("lift_max_mm", 8.0f) / 1000, gam = c.value("gamma", 1.5f);
        float sway = c.value("sway_mm", 3.0f) / 1000, waves = c.value("waves", 1.0f), ramp = std::max(c.value("ramp", 0.35f), 0.01f);
        unsigned seed = c.value("seed", 1u);
        // fade_from: the lift fades back to 0 from this fraction of the length over fade_len (keeps buns / tied ends in place)
        float fadeFrom = c.value("fade_from", 1.0f), fadeLen = std::max(c.value("fade_len", 0.2f), 0.01f);
        auto ix = a.selectGuides(c.value("where", json::object({{"all", true}})));
        std::vector<json> ops;
        int n = 0; double sumLift = 0;
        for (int g : ix) {
            const Guide& G = gr.P[g];
            auto sl = arcLen(G);
            float L = sl.back();
            if (L < 0.02f) continue;
            float amp = lo + (hi - lo) * std::pow(hash01(G[0], seed), gam);
            float sw = sway * (hash01(G[0], seed + 11) * 2 - 1), ph = hash01(G[0], seed + 23) * 6.2832f;
            Guide Q = G;
            for (size_t k = 1; k < G.size(); k++) {
                float t = sl[k] / L, f = std::clamp(t / ramp, 0.0f, 1.0f);
                f = f * f * (3 - 2 * f);
                if (t > fadeFrom) { float u = std::clamp(1 - (t - fadeFrom) / fadeLen, 0.0f, 1.0f); f *= u * u * (3 - 2 * u); }
                V3 nn; hs.sd(G[k], &nn);
                V3 tg = nrmz(G[k] - G[k - 1]), b = nrmz(cross(nn, tg));
                Q[k] = G[k] + nn * (amp * f) + b * (sw * f * std::sin(6.2832f * waves * t + ph));
            }
            Guide R = Q;
            for (size_t k = 1; k < G.size(); k++) R[k] = R[k - 1] + nrmz(Q[k] - R[k - 1]) * (sl[k] - sl[k - 1]);
            skinOut(hs, R, 0.003f);
            ops.push_back({{"op", "reshape"}, {"root", t6::rootJ(G[0])}, {"points", ptsJson(R)}, {"id", ++id}, {"grp", grp}});
            n++; sumLift += amp;
        }
        if (!c.value("list_only", false) && !ops.empty()) { for (auto& o : ops) gr.ops.push_back(o); gr.redo.clear(); gr.replay(); a.editsChanged(); }
        r["changed"] = n; r["mean_lift_mm"] = n ? sumLift / n * 1000 : 0.0;
        return r;
    }

    // ------------------------------------------------------------------ inflate: fuller bun / tail
    // points (not roots) of the selected guides within radius_mm of centre move away from it by scale (smooth falloff
    // to the radius); guides get longer, so the mass grows instead of thinning
    if (d == "inflate") {
        if (!c.contains("centre")) { r["error"] = "inflate needs centre [x,y,z]"; return r; }
        V3 Ce = j3(c["centre"]);
        float rad = c.value("radius_mm", 50.0f) / 1000, sc = c.value("scale", 1.3f);
        auto ix = a.selectGuides(c.value("where", json::object({{"all", true}})));
        std::vector<json> ops;
        int n = 0, pts = 0;
        for (int g : ix) {
            Guide Q = gr.P[g];
            bool any = false;
            for (size_t k = 1; k < Q.size(); k++) {
                float dd = len(Q[k] - Ce);
                if (dd >= rad) continue;
                float w = 1 - (dd / rad) * (dd / rad);
                Q[k] = Ce + (Q[k] - Ce) * (1 + (sc - 1) * w);
                any = true; pts++;
            }
            if (!any) continue;
            skinOut(hs, Q, 0.003f);
            ops.push_back({{"op", "reshape"}, {"root", t6::rootJ(Q[0])}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        if (!c.value("list_only", false) && !ops.empty()) { for (auto& o : ops) gr.ops.push_back(o); gr.redo.clear(); gr.replay(); a.editsChanged(); }
        r["changed"] = n; r["points"] = pts;
        return r;
    }

    // ------------------------------------------------------------------ unzigzag (existing guides)
    if (d == "unzigzag") {
        float maxZig = c.value("max_zigzag", 1.5f), rad = c.value("radius_mm", 30.0f) / 1000, straight = c.value("straight", 1.25f);
        bool avoidEars = c.value("avoid_ears", true);
        float pad = c.value("ear_pad_mm", 4.0f) / 1000;
        auto ix = a.selectGuides(c.value("where", json::object({{"all", true}})));
        auto zigOf = [&](const Guide& G) { return guideLen(G) / std::max(len(G.back() - G[0]), 1e-4f); };
        std::set<int> zig(ix.begin(), ix.end());
        for (auto it = zig.begin(); it != zig.end();) it = zigOf(gr.P[*it]) > maxZig ? std::next(it) : zig.erase(it);
        std::vector<int> tpl;
        for (int g = 0; g < (int)gr.P.size(); g++)
            if (!zig.count(g) && zigOf(gr.P[g]) < straight && guideLen(gr.P[g]) > 0.04f && !(avoidEars && crossesEar(z, gr.P[g], pad))) tpl.push_back(g);
        Grid tg(0.02f);
        for (int g : tpl) tg.add(gr.P[g][0], g);
        int n = 0, none = 0, dropped = 0;
        std::vector<json> ops;
        for (int g : zig) {
            V3 R = gr.P[g][0];
            int bj = -1; float bd = rad;
            tg.around(R, rad, [&](int q) { float dd = len(gr.P[q][0] - R); if (dd < bd) { bd = dd; bj = q; } });
            if (bj < 0) {
                none++;
                if (c.value("drop_unmatched", false)) { ops.push_back({{"op", "delete"}, {"roots", json::array({t6::rootJ(R)})}, {"id", ++id}, {"grp", grp}}); dropped++; }
                continue;
            }
            Guide G = t6::copyRotated(gr.P[bj], z.Ca, R);
            skinOut(hs, G, 0.003f);
            ops.push_back({{"op", "reshape"}, {"root", t6::rootJ(R)}, {"points", ptsJson(G)}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        if (!c.value("list_only", false) && !ops.empty()) { for (auto& o : ops) gr.ops.push_back(o); gr.redo.clear(); gr.replay(); a.editsChanged(); }
        r["zigzag"] = zig.size(); r["replaced"] = n; r["no_template"] = none; r["dropped"] = dropped; r["templates"] = tpl.size();
        return r;
    }

    // ------------------------------------------------------------------ regrow / route_behind_ears
    if (d == "regrow" || d == "route_behind_ears") {
        float pad = c.value("ear_pad_mm", 8.0f) / 1000, behind = c.value("behind_mm", 8.0f);
        std::string earMode = c.value("ear_mode", std::string("behind"));   // behind | drop | keep
        if (d == "route_behind_ears") {
            auto ix = a.selectGuides(c.value("where", json::object({{"all", true}})));
            int n = 0, dropped = 0;
            std::vector<json> ops;
            if (earMode == "split") {
                std::map<int, int> cls;
                int nf = 0, nb = 0;
                for (int g : ix) {
                    Guide G = gr.P[g];
                    int s = routeSplitEars(z, G, pad, behind, c.value("split_bias_mm", 0.0f), hs, c.value("passes", 4));
                    if (!s) continue;
                    cls[g] = s; (s > 0 ? nf : nb)++;
                    ops.push_back({{"op", "reshape"}, {"root", t6::rootJ(G[0])}, {"points", ptsJson(G)}, {"id", ++id}, {"grp", grp}});
                }
                int delF = 0;
                for (auto& F : gr.faces) {
                    bool f = false, b = false;
                    for (int q = 0; q < 3; q++) { auto it = cls.find(F.g[q]); if (it != cls.end()) (it->second > 0 ? f : b) = true; }
                    if (!(f && b)) continue;
                    json rr = json::array();
                    for (int q = 0; q < 3; q++) rr.push_back(t6::rootJ(gr.P[F.g[q]][0]));
                    ops.push_back({{"op", "delete_face"}, {"roots", rr}, {"id", ++id}, {"grp", grp}});
                    delF++;
                }
                if (!c.value("list_only", false)) { for (auto& o : ops) gr.ops.push_back(o); gr.redo.clear(); gr.replay(); a.editsChanged(); }
                json eb = json::array();
                for (int sd = 0; sd < 2; sd++) if (z.hasEar[sd]) eb.push_back({v3j(z.earLo[sd]), v3j(z.earHi[sd])});
                r["front"] = nf; r["behind"] = nb; r["faces_split"] = delF; r["ear_boxes"] = eb;
                return r;
            }
            for (int g : ix) {
                Guide G = gr.P[g];
                if (routeBehindEars(z, G, pad, behind, hs)) {
                    if (earMode == "drop" && crossesEar(z, G, 0.0f)) {
                        ops.push_back({{"op", "delete"}, {"roots", json::array({t6::rootJ(G[0])})}, {"id", ++id}, {"grp", grp}});
                        dropped++; continue;
                    }
                    ops.push_back({{"op", "reshape"}, {"root", t6::rootJ(G[0])}, {"points", ptsJson(G)}, {"id", ++id}, {"grp", grp}});
                    n++;
                }
            }
            if (!c.value("list_only", false)) { for (auto& o : ops) gr.ops.push_back(o); gr.redo.clear(); gr.replay(); a.editsChanged(); }
            json eb = json::array();
            for (int sd = 0; sd < 2; sd++) if (z.hasEar[sd]) eb.push_back({v3j(z.earLo[sd]), v3j(z.earHi[sd])});
            r["routed"] = n; r["dropped"] = dropped; r["ear_boxes"] = eb;
            return r;
        }
        // regrow: run rebuild_from_cards, then edit its op
        json c2 = c;
        for (auto k : {"exclude_ears", "ear_pad_mm", "behind_mm", "ear_mode", "exclude", "hairline_inset_mm", "inset_front_only", "max_zigzag", "smooth_passes"}) c2.erase(k);
        c2["do"] = "rebuild_from_cards";
        json res = rebuildFromCards(a, c2, grpId);
        if (res.contains("error")) return res;
        if (gr.ops.empty() || gr.ops.back().value("op", "") != "rebuild") { r["error"] = "rebuild op missing"; return r; }
        json op = gr.ops.back();
        auto& gs = op["guides"];
        std::vector<Guide> NG;
        for (auto& pts : gs) { Guide G; for (auto& q : pts) G.push_back(j3(q)); NG.push_back(G); }
        std::vector<char> keep(NG.size(), 1);
        int exEar = 0, exSphere = 0, exInset = 0, zig = 0, routed = 0, earDropped = 0;
        // painted scalp vertices (for the inset): unpainted head vertices above the eyes
        std::vector<V3> unp;
        if (c.value("hairline_inset_mm", 0.0f) > 0 && hs.flag)
            for (size_t i = 0; i < hs.m->v.size(); i++) if (!hs.painted((int)i) && hs.m->v[i].z > z.eyeZ) unp.push_back(hs.m->v[i]);
        Grid ug(0.01f);
        for (size_t i = 0; i < unp.size(); i++) ug.add(unp[i], (int)i);
        float inset = c.value("hairline_inset_mm", 0.0f) / 1000;
        bool insetFront = c.value("inset_front_only", true);
        json excl = c.value("exclude", json::array());
        float maxZig = c.value("max_zigzag", 0.0f);
        for (size_t i = 0; i < NG.size(); i++) {
            V3 R = NG[i][0];
            if (c.value("exclude_ears", false) && z.inEar(R, pad)) { keep[i] = 0; exEar++; continue; }
            bool ex = false;
            for (auto& e : excl) if (e.contains("near") && len(R - j3(e["near"])) < e.value("radius_mm", 20.0f) / 1000) ex = true;
            if (ex) { keep[i] = 0; exSphere++; continue; }
            if (inset > 0 && (!insetFront || z.fwd * (R.y - C.y) > 0)) {
                bool close = false;
                ug.around(R, inset, [&](int q) { if (!close && len(unp[q] - R) < inset) close = true; });
                // ears / sideburn skin is unpainted too: only the hairline in front of the ears counts
                if (close && !z.inEar(R, 0.02f)) { keep[i] = 0; exInset++; continue; }
            }
        }
        // zigzag guides (chained card lanes folding back): replaced by a rotated copy of the nearest straight neighbour
        std::vector<char> isZig(NG.size(), 0);
        if (maxZig > 0)
            for (size_t i = 0; i < NG.size(); i++) if (keep[i]) {
                float L = guideLen(NG[i]), ch = len(NG[i].back() - NG[i][0]);
                if (L / std::max(ch, 1e-4f) > maxZig) isZig[i] = 1;
            }
        for (size_t i = 0; i < NG.size(); i++) if (isZig[i]) {
            int bj = -1; float bd = 0.03f;
            for (size_t j = 0; j < NG.size(); j++) if (keep[j] && !isZig[j]) { float dd = len(NG[j][0] - NG[i][0]); if (dd < bd) { bd = dd; bj = (int)j; } }
            if (bj < 0) { keep[i] = 0; continue; }
            NG[i] = t6::copyRotated(NG[bj], z.Ca, NG[i][0]);
            skinOut(hs, NG[i], 0.003f);
            zig++;
        }
        int sp = c.value("smooth_passes", 0);
        for (size_t i = 0; i < NG.size(); i++) {
            if (!keep[i]) continue;
            if (sp > 0) NG[i] = smoothKeep(NG[i], sp, 0.5f);
            if (earMode != "keep" && routeBehindEars(z, NG[i], pad, behind, hs)) {
                routed++;
                if (earMode == "drop" && crossesEar(z, NG[i], 0.0f)) { keep[i] = 0; earDropped++; }
            }
        }
        std::vector<int> remap(NG.size(), -1);
        json g2 = json::array(), b2 = json::array(), u2 = json::array();
        for (size_t i = 0; i < NG.size(); i++) if (keep[i]) {
            remap[i] = (int)g2.size();
            g2.push_back(ptsJson(NG[i]));
            b2.push_back(op["base"][i]);
            if (op.contains("uv") && i < op["uv"].size()) u2.push_back(op["uv"][i]);
        }
        json f2 = json::array();
        for (auto f : op["faces"]) {
            bool ok = true;
            for (auto& v : f) if (v.is_number_integer()) { int m = remap[v.get<int>()]; if (m < 0) ok = false; else v = m; }
            if (ok) f2.push_back(f);
        }
        op["guides"] = g2; op["base"] = b2; op["uv"] = u2; op["faces"] = f2;
        gr.ops.back() = op;
        gr.redo.clear(); gr.replay(); a.editsChanged(); a.smap.ensure(gr.P.size());
        r = res;
        r["regrow"] = {{"excluded_on_ear", exEar}, {"excluded_spheres", exSphere}, {"excluded_hairline_inset", exInset}, {"zigzag_replaced", zig},
                       {"routed_behind_ears", routed}, {"dropped_still_over_ear", earDropped}, {"guides", g2.size()}, {"faces", f2.size()}};
        json eb = json::array();
        for (int sd = 0; sd < 2; sd++) if (z.hasEar[sd]) eb.push_back({v3j(z.earLo[sd]), v3j(z.earHi[sd])});
        r["ear_boxes"] = eb;
        return r;
    }

    // ------------------------------------------------------------------ twist_roll
    if (d == "twist_roll") {
        if (!c.contains("path") || c["path"].size() < 2) { r["error"] = "twist_roll needs path [[x,y,z],...] (start -> end of the roll)"; return r; }
        Line P; P.C = C;
        for (auto& q : c["path"]) P.p.push_back(j3(q));
        // snap the path onto the vanilla roll: centroid of card vertices standing off the skin near each path point
        float fitR = c.value("fit_mm", 25.0f) / 1000;
        std::vector<V3> cardV;
        for (auto& m : pk.meshes) if (m.kind == "hair_cards") for (auto& v : m.v) cardV.push_back(v);
        json fitted = json::array();
        if (c.value("fit_cards", true) && !cardV.empty()) {
            for (auto& p : P.p) {
                V3 acc{0, 0, 0}; int n = 0;
                for (auto& v : cardV) if (len(v - p) < fitR && hs.sd(v) > c.value("fit_off_mm", 6.0f) / 1000) { acc = acc + v; n++; }
                if (n > 5) p = p * 0.3f + acc * (0.7f / n);
                fitted.push_back(v3j(p));
            }
        }
        // densify the path (Catmull-Rom) for a smooth axis
        {
            std::vector<V3> q;
            auto& p = P.p;
            for (size_t i = 0; i + 1 < p.size(); i++) {
                V3 p0 = p[i ? i - 1 : 0], p1 = p[i], p2 = p[i + 1], p3 = p[std::min(i + 2, p.size() - 1)];
                for (int s = 0; s < 8; s++) {
                    float t = s / 8.0f, t2 = t * t, t3 = t2 * t;
                    q.push_back((p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f);
                }
            }
            q.push_back(p.back());
            P.p = q;
        }
        float Lp = P.length();
        float rad = c.value("radius_mm", 0.0f) / 1000;
        if (rad <= 0 && !cardV.empty()) {   // from the cards: median distance of off-skin card vertices to the axis
            std::vector<float> ds;
            for (auto& v : cardV) {
                if (hs.sd(v) < 0.004f) continue;
                V3 f, t, sd; float dist, side;
                P.closest(v, f, dist, side, t, sd);
                if (dist < 0.035f) ds.push_back(dist);
            }
            if (!ds.empty()) { std::nth_element(ds.begin(), ds.begin() + ds.size() / 2, ds.end()); rad = ds[ds.size() / 2]; }
            rad = std::clamp(rad, 0.008f, 0.03f);
        }
        if (rad <= 0) rad = 0.015f;
        rad *= c.value("radius_scale", 1.0f);
        float pitch = c.value("pitch_mm", 45.0f) / 1000, hand = c.value("hand", 1.0f) >= 0 ? 1.0f : -1.0f;
        float standoff = c.value("standoff_mm", 3.0f) / 1000, layer = c.value("layer", 0.35f), endTurns = c.value("end_turns", 0.6f);
        float tuckR = c.value("tuck_r", 0.4f), curlMm = c.value("curl_mm", 0.0f) / 1000, curlTurns = c.value("curl_turns", 3.0f);
        float maxEnd = c.value("end_mm", 10.0f) / 1000;   // strands may run this far past the path end
        auto ix = a.selectGuides(c.value("where", json::object({{"back", true}})));
        // attach "azimuth": each root combs straight down into the axis point at its own angle around the head
        // (nearest-in-3D sends the whole crown to the path start); roots outside the path's angle range go to the nearer end
        bool attachAz = c.value("attach", std::string("nearest")) == "azimuth";
        // az_centre_fwd_mm: angles are taken around a point this far in front of the head centre, so the crown
        // (where the angle around the centre is unstable) combs backward instead of splitting into a bald whorl
        V3 AzC = C; AzC.y += z.fwd * c.value("az_centre_fwd_mm", 0.0f) / 1000;
        std::vector<float> axArc(P.p.size(), 0.0f), axAz(P.p.size(), 0.0f);
        for (size_t i = 0; i < P.p.size(); i++) {
            if (i) axArc[i] = axArc[i - 1] + len(P.p[i] - P.p[i - 1]);
            axAz[i] = std::atan2(P.p[i].x - AzC.x, P.p[i].y - AzC.y);
        }
        bool extendShort = c.value("extend_short", false);
        bool walkApproach = c.value("approach", std::string("walk")) != "chord";   // chord = the first version
        float minRoll = c.value("min_roll_mm", 40.0f) / 1000, maxApproach = c.value("max_approach_mm", 200.0f) / 1000;
        bool overEars = c.value("over_ears", false);
        float earPad = c.value("ear_pad_mm", 8.0f) / 1000;
        int nExt = 0, nWander = 0, nComp = 0, nOver = 0;
        std::vector<json> ops;
        std::vector<int> rolled;
        int nShort = 0, n = 0;
        json sample = json::array();
        for (int g : ix) {
            const Guide& G0 = gr.P[g];
            float L = guideLen(G0);
            V3 R = G0[0];
            // attach point: nearest point on the axis to the root, biased toward the path start by attach_bias
            V3 foot, tn, sdir; float dist, side, arc0;
            P.closest(R, foot, dist, side, tn, sdir, &arc0);
            if (attachAz) {
                float az = std::atan2(R.x - AzC.x, R.y - AzC.y), best = 1e9f;
                for (size_t i = 0; i < P.p.size(); i++) {
                    float dA = std::fabs(std::remainder(az - axAz[i], 2 * 3.14159265f));
                    if (dA < best) { best = dA; arc0 = axArc[i]; }
                }
            }
            arc0 = std::clamp(arc0 + c.value("attach_shift_mm", 0.0f) / 1000, 0.0f, Lp * 0.95f);
            V3 A = P.at(arc0, &tn);
            V3 toR = R - A; toR = toR - tn * dot(toR, tn); V3 n0 = nrmz(toR);   // from the axis toward the root
            float rr = rad * (1 - layer + layer * hash01(R, 7) * 2) ;
            V3 entry = A + n0 * rr;
            // approach: root -> entry along the skin (straight chord pushed out of the skin)
            float La = len(entry - R);
            if (La > L * 0.92f) {   // too short to reach the roll: leave it, or (extend_short) lengthen it to roll min_roll_mm
                if (!extendShort || La > maxApproach) { nShort++; continue; }
                L = La + minRoll; nExt++;
            }
            std::vector<V3> path;
            int na;
            if (walkApproach) {
                // walk ALONG THE SKIN toward the entry (a straight chord from the crown runs through the skull, and its
                // deep points snap to the nearest skin = the narrow sides: the crown bared in a fan)
                path.push_back(R);
                V3 p = R;
                float stopD = std::max(rr * 1.5f, 0.01f);
                auto walkTo = [&](V3 tgt, float stop) {
                    for (int it = 0; it < 120 && len(tgt - p) > stop; it++) {
                        V3 nn; hs.sd(p, &nn);
                        V3 d = tgt - p; d = d - nn * dot(d, nn);
                        if (len(d) < 1e-5f) break;
                        V3 q = onSkin(hs, p + nrmz(d) * 0.005f, standoff);
                        if (len(tgt - q) >= len(tgt - p)) break;   // no progress (target straight above)
                        path.push_back(q); p = q;
                    }
                };
                // over_ears: a root in front of the ear whose roll entry is behind it walks via a point above the ear top
                // (the straight skin walk from the temple ran through the ear: a see-through fan over the temple)
                if (overEars) {
                    int sd = R.x > C.x ? 0 : 1;
                    if (z.hasEar[sd]) {
                        V3 lo = z.earLo[sd], hi = z.earHi[sd];
                        float fr = z.fwd > 0 ? hi.y : lo.y, bk = z.fwd > 0 ? lo.y : hi.y;
                        if (z.fwd * (R.y - fr) > -earPad && z.fwd * (entry.y - bk) < 0) {
                            V3 W{sd == 0 ? hi.x : lo.x, (lo.y + hi.y) * 0.5f, hi.z + earPad};
                            W = onSkin(hs, W, standoff);
                            if (R.z < W.z + 0.07f) { walkTo(W, 0.006f); nOver++; }
                        }
                    }
                }
                walkTo(entry, stopD);
                int nb = std::max(2, int(len(entry - p) / 0.005f));
                for (int i = 1; i <= nb; i++) path.push_back(p + (entry - p) * (float(i) / nb));
                na = (int)path.size() - 1;
                float chord = La;
                La = guideLen(Guide(path.begin(), path.end()));
                // a walk that wanders (missed the entry, ran down the neck) is not used: leave that guide as it is
                if (La > chord * 1.6f + 0.02f || La > maxApproach * 1.5f) { nWander++; continue; }
                if (La > L * 0.92f) {
                    if (!extendShort || La > maxApproach) { nShort++; continue; }
                    if (L < La + minRoll) { L = La + minRoll; }
                }
            } else {
                na = std::max(4, int(La / 0.006f));
                for (int i = 0; i <= na; i++) {
                    float t = float(i) / na;
                    V3 p = R + (entry - R) * t;
                    V3 nn; float sd = hs.sd(p, &nn);
                    if (i > 0 && sd < standoff) p = p + nn * (standoff - sd);
                    path.push_back(p);
                }
            }
            float Lroll = L - guideLen(Guide(path.begin(), path.end()));
            // helix around the axis from arc0 toward the end
            float per = std::sqrt(std::pow(2 * 3.14159265f * rr, 2) + pitch * pitch);   // strand length per turn
            float th0 = 0, s = 0, arc = arc0;
            V3 b0 = nrmz(cross(tn, n0));
            float ds = 0.003f;
            float phase = 0;
            bool atEnd = false;
            float endUsed = 0;
            while (s < Lroll) {
                s += ds;
                float dTh = ds / per * 2 * 3.14159265f;
                if (!atEnd) {
                    arc += ds / per * pitch;
                    if (arc >= Lp) { arc = Lp; atEnd = true; }
                }
                phase += dTh * hand;
                V3 t2; V3 ax = P.at(arc, &t2);
                // frame transported: n = component of n0 orthogonal to the local tangent
                V3 nn = nrmz(n0 - t2 * dot(n0, t2)), bb = nrmz(cross(t2, nn));
                float rcur = rr;
                if (atEnd) {
                    endUsed += ds;
                    float f = std::clamp(endUsed / std::max(per * endTurns, 1e-4f), 0.0f, 1.0f);
                    rcur = rr * (1 - (1 - tuckR) * f);
                    if (endUsed > per * endTurns + maxEnd) break;
                }
                V3 p = ax + (nn * std::cos(phase) + bb * std::sin(phase)) * rcur;
                if (curlMm > 0) {
                    float cp = s / std::max(Lroll, 1e-4f) * curlTurns * 2 * 3.14159265f + hash01(R, 3) * 6.28f;
                    p = p + (nn * std::cos(phase + 1.57f) + bb * std::sin(phase + 1.57f)) * (curlMm * std::sin(cp));
                }
                path.push_back(p);
                (void)th0; (void)b0;
            }
            // resample to K: the approach gets ~ its share but at least 3 segments, the roll the rest
            std::vector<float> d0(path.size(), 0.0f);
            for (size_t i = 1; i < path.size(); i++) d0[i] = d0[i - 1] + len(path[i] - path[i - 1]);
            float Ltot = d0.back();
            float La2 = guideLen(Guide(path.begin(), path.begin() + na + 1));
            int ka = std::clamp(int(std::round((K - 1) * La2 / std::max(Ltot, 1e-4f) * 0.7f)), 3, K - 5);
            // walk: about one point per 2 cm of approach (long crown approaches got 3 points whose chords cut 5+ cm into the skull)
            if (walkApproach) ka = std::clamp(std::max(ka, int(std::ceil(La2 / 0.02f))), 3, K - 4);
            std::vector<float> at;
            for (int k = 0; k <= ka; k++) at.push_back(La2 * k / ka);
            for (int k = ka + 1; k < K; k++) at.push_back(La2 + (Ltot - La2) * float(k - ka) / (K - 1 - ka));
            Guide Q = sampleAt(path, at);
            if (walkApproach) for (int k = 1; k < ka; k++) Q[k] = onSkin(hs, Q[k], standoff);   // any depth
            skinOut(hs, Q, standoff * 0.7f);
            // what the Studio shows (and exports) is P + (fur guide - edit guide) of the BASE guide per point; on builds whose
            // post-process moved guides a lot (Iris: up to 15 cm) that old per-point offset bent the roll into hanging loops.
            // drop its varying part so the shown strand is the computed roll (+ the constant root offset)
            if (!c.value("keep_fur_delta", false)) {
                int b = gr.baseOf(g);
                if (b >= 0 && b < (int)a.pack.guides.size() && b < (int)a.pack.guidesEdit.size()) {
                    const Guide& Fg = a.pack.guides[b]; const Guide& Eg = a.pack.guidesEdit[b];
                    V3 d0 = Fg[0] - Eg[0];
                    for (int k = 1; k < K && k < (int)Fg.size() && k < (int)Eg.size(); k++) Q[k] = Q[k] - (Fg[k] - Eg[k]) + d0;
                    nComp++;
                }
            }
            ops.push_back({{"op", "reshape"}, {"root", t6::rootJ(R)}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
            rolled.push_back(g);
            n++;
            if (sample.size() < 3) sample.push_back({{"i", g}, {"approach_mm", std::round(La * 1000)}, {"roll_mm", std::round(Lroll * 1000)}});
        }
        if (c.value("list_only", false)) { r["would_roll"] = n; r["over_ears"] = nOver; r["too_short"] = nShort; r["extended"] = nExt; r["walk_wandered"] = nWander; return r; }
        for (auto& o : ops) gr.ops.push_back(o);
        gr.redo.clear(); gr.replay(); a.editsChanged();
        if (c.value("hold", true)) {
            a.smap.ensure(gr.P.size());
            for (int g : rolled) { a.smap.guideS[g] = 1.0f; a.smap.guideR[g] = 1.0f; }
            a.stiffDirty = true;
            // same global hold as make_bun: the strands keep their rolled rest shape under gravity
            a.look.setF("physicalMaterials.stiffness.stiffness", 1.0f);
            a.look.setF("physicalMaterials.stiffness.stiffnessStrength", 1.0f);
            a.look.setF("physicalMaterials.stiffness.rootStiffness", 1.0f);
            a.look.setF("physicalMaterials.stiffness.tipStiffness", c.value("tip_stiffness", 0.8f));
            a.look.setF("physicalMaterials.stiffness.bendStiffness", 1.0f);
            json one = {{"X", 1.0}, {"Y", 1.0}, {"Z", 1.0}, {"W", 1.0}};
            a.look.setJson("physicalMaterials.stiffness.stiffnessCurve", one);
            a.look.setJson("physicalMaterials.stiffness.stiffnessStrengthCurve", one);
            a.look.setJson("physicalMaterials.stiffness.bendStiffnessCurve", one);
            a.look.enforceCaps();
            r["hold"] = "rolled guides s/r 1; global stiffness 1, strength 1, root 1, bend 1, flat curves";
        }
        settleRest(a, 0.5);
        r["rolled"] = n; r["over_ears"] = nOver; r["too_short_left"] = nShort; r["extended"] = nExt; r["walk_wandered_left"] = nWander; r["fur_delta_dropped"] = nComp; r["radius_mm"] = std::round(rad * 10000) / 10; r["path_length_mm"] = std::round(Lp * 1000);
        r["fitted_path"] = fitted; r["sample"] = sample;
        return r;
    }

    // ------------------------------------------------------------------ plant_fringe
    if (d == "plant_fringe") {
        // stations: explicit roots, or along the front painted hairline between x_range, inset_mm inside
        std::vector<V3> roots;
        if (c.contains("roots")) for (auto& q : c["roots"]) roots.push_back(j3(q));
        else {
            Hairline hl;
            if (!hairlineOf(a, hl)) { r["error"] = "no painted hairline (give roots)"; return r; }
            float x0 = c.contains("x_range") ? c["x_range"][0].get<float>() : C.x - 0.03f, x1 = c.contains("x_range") ? c["x_range"][1].get<float>() : C.x + 0.03f;
            float spacing = c.value("spacing_mm", 4.0f) / 1000, inset = c.value("inset_mm", 4.0f) / 1000;
            for (float x = x0; x <= x1 + 1e-6f; x += spacing) {
                int bi = -1; float best = -1e9f;
                for (size_t i = 0; i < hl.pt.size(); i++) {
                    V3 p = hl.pt[i];
                    if (std::fabs(p.x - x) > spacing * 0.75f || p.z < z.browZ) continue;
                    float f = z.fwd * p.y;
                    if (f > best) { best = f; bi = (int)i; }
                }
                if (bi < 0) continue;
                V3 p = hl.pt[bi], in = nrmz(hl.inner[bi] - hl.pt[bi]);
                // inset toward the head top along the skin
                V3 up = nrmz(V3{0, 0, 1} - nrmz(p - C) * dot(V3{0, 0, 1}, nrmz(p - C)));
                (void)in;
                roots.push_back(onSkin(hs, p + up * inset, 0.0f));
            }
        }
        if (roots.empty()) { r["error"] = "no fringe stations"; return r; }
        float Lm = c.value("length_mm", 60.0f) / 1000, jit = c.value("length_jitter", 0.25f), sweep = c.value("sweep", 0.3f);
        float stand = c.value("standoff_mm", 2.5f) / 1000, liftTip = c.value("lift_tip_mm", 4.0f) / 1000, browGap = c.value("brow_gap_mm", 8.0f) / 1000;
        float wave = c.value("wave_mm", 0.0f) / 1000, upFirst = c.value("up_mm", 0.0f) / 1000;
        float spread = c.value("sweep_spread", 0.0f);
        float zStop = z.browZ + browGap;
        std::vector<Guide> NG;
        int stopped = 0;
        for (size_t i = 0; i < roots.size(); i++) {
            V3 R = roots[i];
            float L = Lm * (1 + jit * (hash01(R, 11) * 2 - 1));
            float sw = sweep + spread * (roots.size() > 1 ? (float(i) / (roots.size() - 1) - 0.5f) : 0);
            // direction: down the forehead (-z on the tangent plane), swept sideways (+x = character's left)
            auto W = skinWalk(hs, R, L, 0.003f, stand, [&](V3 p, V3 nn, float s) {
                V3 dn{0, 0, -1}, side{1, 0, 0};
                V3 dd = s < upFirst ? V3{0, 0, 1} * 0.3f + V3{0, z.fwd, 0} : dn + side * sw;
                return dd;
            });
            // stop above the brows; lift the tip off the skin
            std::vector<V3> Q;
            for (size_t k = 0; k < W.size(); k++) {
                if (k > 2 && W[k].z < zStop && z.fwd * (W[k].y - C.y) > 0.03f) { stopped++; break; }
                Q.push_back(W[k]);
            }
            if (Q.size() < 4) continue;
            float Lq = 0; for (size_t k = 1; k < Q.size(); k++) Lq += len(Q[k] - Q[k - 1]);
            float s = 0;
            for (size_t k = 1; k < Q.size(); k++) {
                s += len(W[k] - W[k - 1]);
                float f = s / std::max(Lq, 1e-4f);
                V3 nn; hs.sd(Q[k], &nn);
                Q[k] = Q[k] + nn * (liftTip * f * f);
                if (wave > 0) { V3 sd2 = nrmz(cross(nn, nrmz(Q[k] - Q[k - 1]))); Q[k] = Q[k] + sd2 * (wave * std::sin(f * 9.42f + hash01(R, 5) * 6.28f) * f); }
            }
            Guide G = sampleAt(Q, uniformAt(Lq, K, c.value("root_bias", 1.0f)));
            NG.push_back(G);
        }
        StitchOpt so;
        so.edgeMm = c.value("edge_mm", 16.0f); so.rmaxMm = c.value("rmax_mm", 12.0f); so.zoneMm = 20;
        if (c.contains("uv")) so.uv = c["uv"];
        json st;
        json op = stitchOp(a, NG, so, id, grp, st);
        r["stations"] = roots.size(); r["stopped_at_brow"] = stopped; r["stitch"] = st;
        if (c.value("list_only", false)) { json rl = json::array(); for (auto& q : roots) rl.push_back(v3j(q)); r["roots"] = rl; return r; }
        gr.ops.push_back(op);
        gr.redo.clear(); gr.replay(); a.editsChanged(); a.smap.ensure(gr.P.size());
        if (c.contains("stiffness")) {
            float sv = c["stiffness"].get<float>();
            for (size_t g = gr.P.size() - op["guides"].size(); g < gr.P.size(); g++) { a.smap.guideS[g] = sv; a.smap.guideR[g] = sv; }
            a.stiffDirty = true;
        }
        return r;
    }

    // ------------------------------------------------------------------ parting_roots
    if (d == "parting_roots") {
        Line Ln; Ln.C = C;
        if (c.contains("line")) for (auto& q : c["line"]) Ln.p.push_back(onSkin(hs, j3(q), 0.0f));
        else {
            // automatic: from the front painted hairline at x (default head centre x) back over the top along the mid plane
            float x0 = c.value("x", C.x);
            Hairline hl;
            V3 front{x0, C.y + z.fwd * 0.09f, C.z + 0.07f};
            if (hairlineOf(a, hl)) {
                float best = -1e9f;
                for (size_t i = 0; i < hl.pt.size(); i++) if (std::fabs(hl.pt[i].x - x0) < 0.006f && hl.pt[i].z > z.browZ && z.fwd * hl.pt[i].y > best) { best = z.fwd * hl.pt[i].y; front = hl.inner[i]; }
            }
            float Lt = c.value("length_mm", 110.0f) / 1000;
            auto W = skinWalk(hs, front, Lt, 0.005f, 0.0f, [&](V3 p, V3 nn, float s) { V3 dd{0, -z.fwd, 0}; dd.x = (x0 - p.x) * 20; return dd; });
            for (size_t i = 0; i < W.size(); i += 3) Ln.p.push_back(W[i]);
            if (Ln.p.back().x != W.back().x) Ln.p.push_back(W.back());
        }
        if (Ln.p.size() < 2) { r["error"] = "parting line needs 2+ points"; return r; }
        float Lline = Ln.length();
        float spacing = c.value("spacing_mm", 5.0f) / 1000, layM = c.value("lay_mm", 45.0f) / 1000, rejoin = c.value("rejoin_mm", 50.0f) / 1000;
        float band = c.value("band_mm", 25.0f) / 1000, stand = c.value("standoff_mm", 1.5f) / 1000, steer = c.value("steer", 0.5f);
        float tmin = c.value("t_min", 0.0f), tmax = c.value("t_max", 1.0f);
        std::vector<float> rows;
        for (auto& v : c.value("rows_mm", json::array({2.5, 6.0, 10.0}))) rows.push_back(v.get<float>() / 1000);
        float minL = c.value("min_length", 0.06f), gapMm = c.value("gap_mm", 1.5f) / 1000;
        std::string sides = c.value("sides", std::string("both"));   // both | plus | minus
        float back = c.value("back", 0.35f);   // the laid root part also goes toward the back end of the line
        std::vector<int> alive;
        for (size_t g = 0; g < gr.P.size(); g++) if (gr.alive[g]) alive.push_back((int)g);
        std::vector<json> ops;
        // 1) comb: old guides rooted within band whose root part crosses the line or lies across it -> laid away
        int combed = 0;
        std::map<int, float> sideOf;
        for (int g : alive) {
            V3 f, t, sd; float dist, side;
            Ln.closest(gr.P[g][0], f, dist, side, t, sd);
            sideOf[g] = side;
        }
        if (c.value("comb", true)) {
            for (int g : alive) {
                const Guide& G = gr.P[g];
                V3 f, t, sd; float dist, side, arc;
                Ln.closest(G[0], f, dist, side, t, sd, &arc);
                if (dist > band || arc < Lline * tmin - 0.01f || arc > Lline * tmax + 0.01f) continue;
                // does the first lay_m cross to the other side or run along the line instead of away?
                auto sl = arcLen(G);
                bool bad = false;
                for (size_t k = 1; k < G.size() && sl[k] < layM; k++) {
                    V3 f2, t2, sd2; float d2, s2;
                    Ln.closest(G[k], f2, d2, s2, t2, sd2);
                    if (s2 != side && d2 > 0.001f) { bad = true; break; }
                }
                V3 d1 = nrmz(G[std::min<size_t>(2, G.size() - 1)] - G[0]);
                if (dot(d1, sd * side) < 0.3f) bad = true;
                if (!bad) continue;
                V3 away = sd * side + t * (-back);   // t points from front to back of the line: -t*back? toward back = +t
                away = sd * side + t * back;
                V3 flowEnd = nrmz(G.back() - G[0]);
                // the far part of the strand must end on its own side
                Guide Q = layAway(hs, G, away, layM, rejoin, stand, steer, flowEnd);
                for (size_t k = 1; k < Q.size(); k++) {
                    V3 f2, t2, sd2; float d2, s2;
                    Ln.closest(Q[k], f2, d2, s2, t2, sd2);
                    if (s2 != side && d2 < 0.06f) Q[k] = Q[k] + sd2 * (side * (d2 + gapMm));
                }
                Q = smoothKeep(Q, 1, 0.4f);
                skinOut(hs, Q, 0.002f);
                ops.push_back({{"op", "reshape"}, {"root", t6::rootJ(G[0])}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
                combed++;
            }
        }
        // apply the comb first so new guides copy the combed shapes
        if (!c.value("list_only", false) && !ops.empty()) { for (auto& o : ops) gr.ops.push_back(o); gr.replay(); a.editsChanged(); }
        // 2) old faces spanning the line are split
        std::vector<std::array<int, 3>> delFaces;
        if (c.value("split", true)) {
            for (auto& F : gr.faces) {
                float s0 = sideOf.count(F.g[0]) ? sideOf[F.g[0]] : 1, s1 = sideOf.count(F.g[1]) ? sideOf[F.g[1]] : 1, s2 = sideOf.count(F.g[2]) ? sideOf[F.g[2]] : 1;
                if (s0 == s1 && s1 == s2) continue;
                V3 cen = (gr.P[F.g[0]][0] + gr.P[F.g[1]][0] + gr.P[F.g[2]][0]) * (1.0f / 3);
                V3 f, t, sd; float dist, side, arc;
                Ln.closest(cen, f, dist, side, t, sd, &arc);
                if (arc < Lline * tmin - 0.005f || arc > Lline * tmax + 0.005f) continue;
                delFaces.push_back(F.g);
            }
        }
        // 3) new roots: rows on each side, every spacing along the line
        std::vector<Guide> NG;
        int noTpl = 0, covered = 0;
        float covMm = c.value("covered_mm", 1.8f) / 1000;
        for (float s = Lline * tmin; s <= Lline * tmax + 1e-6f; s += spacing) {
            V3 tn; V3 S = Ln.at(s, &tn);
            V3 nS = nrmz(S - C), sdir = nrmz(cross(tn, nS));
            for (int sg = -1; sg <= 1; sg += 2) {
                if ((sides == "plus" && sg < 0) || (sides == "minus" && sg > 0)) continue;
                for (size_t ri = 0; ri < rows.size(); ri++) {
                    float off = rows[ri] + (ri % 2 ? spacing * 0.0f : 0);
                    float sAlong = s + (ri % 2 ? spacing * 0.5f : 0);
                    if (sAlong > Lline * tmax) continue;
                    V3 S2 = Ln.at(sAlong, &tn);
                    V3 E = onSkin(hs, S2 + sdir * (sg * off), 0.0f);
                    bool cov = false;
                    for (int g : alive) if (gr.alive[g] && len(gr.P[g][0] - E) < covMm) { cov = true; break; }
                    for (auto& G : NG) if (len(G[0] - E) < covMm) cov = true;
                    if (cov) { covered++; continue; }
                    // template: nearest alive guide on the same side beyond the rows, long enough
                    int best = -1; float bd = 1e9f;
                    for (int g : alive) {
                        if (!gr.alive[g] || guideLen(gr.P[g]) < minL) continue;
                        V3 f, t, sd; float dist, side;
                        Ln.closest(gr.P[g][0], f, dist, side, t, sd);
                        if (side != sg || dist < rows.back() * 0.5f || dist > band + 0.02f) continue;
                        float dd = len(gr.P[g][0] - E);
                        if (dd < bd) { bd = dd; best = g; }
                    }
                    if (best < 0) { noTpl++; continue; }
                    Guide T = t6::copyRotated(gr.P[best], z.Ca, E);
                    V3 away = sdir * float(sg) + tn * back;
                    Guide Q = layAway(hs, T, away, layM * (0.7f + 0.3f * (1 - off / std::max(rows.back(), 1e-4f))), rejoin, stand, steer, nrmz(T.back() - T[0]));
                    for (size_t k = 1; k < Q.size(); k++) {
                        V3 f2, t2, sd2; float d2, s2;
                        Ln.closest(Q[k], f2, d2, s2, t2, sd2);
                        if (s2 != sg && d2 < 0.06f) Q[k] = Q[k] + sd2 * (float(sg) * (d2 + gapMm));
                    }
                    Q = smoothKeep(Q, 1, 0.4f);
                    skinOut(hs, Q, 0.002f);
                    if (z.overFace(Q.back()) && c.value("no_face", true)) continue;
                    NG.push_back(Q);
                }
            }
        }
        StitchOpt so;
        so.edgeMm = c.value("edge_mm", 18.0f); so.rmaxMm = c.value("rmax_mm", 14.0f); so.zoneMm = c.value("zone_mm", 16.0f); so.oldOnly = true;
        so.edgeOk = [&](V3 A, V3 B) {
            V3 f, t, sd; float dA, sA, dB, sB;
            Ln.closest(A, f, dA, sA, t, sd); Ln.closest(B, f, dB, sB, t, sd);
            return sA == sB;
        };
        if (c.contains("uv")) so.uv = c["uv"];
        else if (c.value("uv_mode", std::string("nearest")) == "nearest" && !gr.faces.empty()) {
            // the uv of the old faces near the parting (keeps the local colour)
            V3 mid = Ln.at(Lline * 0.5f);
            float bd = 1e9f;
            for (auto& F : gr.faces) {
                V3 cen = (gr.P[F.g[0]][0] + gr.P[F.g[1]][0] + gr.P[F.g[2]][0]) * (1.0f / 3);
                float dd = len(cen - mid);
                if (dd < bd) { bd = dd; so.uv = json::array({r4(F.uv[0]), r4(F.uv[1])}); }
            }
        }
        json st;
        json op = stitchOp(a, NG, so, id, grp, st, {}, delFaces);
        // delete_face ops for the split, then the rebuild op
        json jl = json::array();
        for (auto& p : Ln.p) jl.push_back(v3j(p));
        r["line"] = jl; r["line_length_mm"] = std::round(Lline * 1000); r["combed"] = combed; r["split_faces"] = delFaces.size();
        r["planned"] = NG.size(); r["no_template"] = noTpl; r["already_covered"] = covered; r["stitch"] = st;
        if (c.value("list_only", false)) return r;
        int id2 = id;
        for (auto& F : delFaces) {
            json rr = json::array();
            for (int q = 0; q < 3; q++) rr.push_back(t6::rootJ(gr.P[F[q]][0]));
            gr.ops.push_back({{"op", "delete_face"}, {"roots", rr}, {"id", ++id2}, {"grp", grp}});
        }
        op["id"] = ++id2;
        gr.ops.push_back(op);
        gr.redo.clear(); gr.replay(); a.editsChanged(); a.smap.ensure(gr.P.size());
        a.computeBudget();
        r["budget_ratio"] = a.bud.ratio;
        return r;
    }
    r["error"] = "unknown r3 command";
    return r;
}
