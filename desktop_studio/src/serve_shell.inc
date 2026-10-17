// conform_shell (2026-10-16, opt-in, Studio-only tool; plain reshape ops = pipeline-compatible):
// turns a messy set of guides (uneven depth from the scalp, lumps, crossing strands) into an evenly layered, rounded
// hairstyle. 1) a smooth OUTER surface seen from the head centre (vanilla visible card hair, the current guide envelope,
// or a blend), 2) N concentric shells between the scalp and that surface at even depth fractions, 3) every guide gets a
// shell (by its current depth or ranked so each shell is evenly populated among its neighbours) and is re-walked after
// its root part ON that shell along a smooth tangent flow field (averaged guide directions and / or the card texture
// direction, optional parting plane that is never smoothed across). Root and segment lengths are kept.
//
// Maps live on an isotropic Fibonacci sphere of directions (no lat / lon pole on the crown).
namespace cshell {

struct SphNodes {
    std::vector<V3> d;                       // node directions
    std::vector<std::vector<int>> nb;        // neighbours within nbAng (incl. self)
    float sp = 0;                            // mean node spacing (rad)
    // bucket accelerator: cube face + 24 x 24 cells
    static const int CN = 24;
    std::vector<std::vector<int>> cells;
    static int cellOf(V3 u) {
        float ax = std::fabs(u.x), ay = std::fabs(u.y), az = std::fabs(u.z);
        int f; float a, b, m;
        if (ax >= ay && ax >= az) { f = u.x > 0 ? 0 : 1; m = ax; a = u.y; b = u.z; }
        else if (ay >= az) { f = u.y > 0 ? 2 : 3; m = ay; a = u.x; b = u.z; }
        else { f = u.z > 0 ? 4 : 5; m = az; a = u.x; b = u.y; }
        int i = std::clamp(int((a / m * 0.5f + 0.5f) * CN), 0, CN - 1), j = std::clamp(int((b / m * 0.5f + 0.5f) * CN), 0, CN - 1);
        return (f * CN + i) * CN + j;
    }
    void build(float spacingDeg) {
        const float PI = 3.14159265f;
        sp = spacingDeg * PI / 180;
        int n = std::max(200, int(4 * PI / (sp * sp)));
        const float ga = PI * (3 - std::sqrt(5.0f));
        d.resize(n);
        for (int i = 0; i < n; i++) {
            float z = 1 - 2 * (i + 0.5f) / n, r = std::sqrt(std::max(0.0f, 1 - z * z)), t = ga * i;
            d[i] = {r * std::cos(t), r * std::sin(t), z};
        }
        cells.assign(6 * CN * CN, {});
        for (int i = 0; i < n; i++) cells[cellOf(d[i])].push_back(i);
        float c2 = std::cos(2.6f * sp);
        nb.assign(n, {});
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) if (dot(d[i], d[j]) >= c2) nb[i].push_back(j);
    }
    int nearest(V3 u) const {
        int c = cellOf(u), best = -1; float bd = -2;
        for (int i : cells[c]) { float t = dot(d[i], u); if (t > bd) { bd = t; best = i; } }
        if (best < 0) for (int i = 0; i < (int)d.size(); i++) { float t = dot(d[i], u); if (t > bd) { bd = t; best = i; } }
        // walk to the true nearest (cell borders)
        for (bool moved = true; moved;) {
            moved = false;
            for (int j : nb[best]) { float t = dot(d[j], u); if (t > bd + 1e-7f) { bd = t; best = j; moved = true; } }
        }
        return best;
    }
};

// capsule frame: above the head centre a point is (direction from the centre, distance); below it (hair hanging down
// the sides / neck) it is (azimuth + height mapped onto the lower hemisphere, horizontal distance from the vertical axis).
// Shells are then spheres on top and vertical cylinders below, as a bob is - not spheres wrapping under the chin.
struct Capsule {
    V3 c; float R0 = 0.12f;
    bool key(V3 p, V3& u, float& r) const {
        V3 q = p - c;
        if (q.z >= 0) { r = len(q); if (r < 1e-4f) return false; u = q * (1.0f / r); return true; }
        r = std::sqrt(q.x * q.x + q.y * q.y);
        if (r < 1e-4f) return false;
        float pol = 1.5707963f + std::min(-q.z / R0, 1.35f);
        u = {q.x / r * std::sin(pol), q.y / r * std::sin(pol), std::cos(pol)};
        return true;
    }
    V3 point(V3 u, float r) const {
        if (u.z >= 0) return c + u * r;
        float h = std::sqrt(u.x * u.x + u.y * u.y);
        if (h < 1e-6f) return c + u * r;
        float pol = std::acos(std::clamp(u.z, -1.0f, 1.0f));
        return {c.x + u.x / h * r, c.y + u.y / h * r, c.z - (pol - 1.5707963f) * R0};
    }
    static V3 normal(V3 u) {   // outward normal of the shell at key u
        if (u.z >= 0) return u;
        float h = std::sqrt(u.x * u.x + u.y * u.y);
        return h < 1e-6f ? u : V3{u.x / h, u.y / h, 0};
    }
};

// scalar map on the nodes: value + valid flag
struct SMap {
    std::vector<float> v; std::vector<char> ok;
    // gaussian-weighted lookup; returns false when no valid node is near
    bool at(const SphNodes& S, V3 u, float& out) const {
        int n = S.nearest(u);
        double sw = 0, s = 0; float sig = 0.75f * S.sp;
        for (int j : S.nb[n]) {
            if (!ok[j]) continue;
            float a = std::acos(std::clamp(dot(S.d[j], u), -1.0f, 1.0f));
            double w = std::exp(-0.5 * (a / sig) * (a / sig));
            sw += w; s += w * v[j];
        }
        if (sw < 1e-3) return false;
        out = float(s / sw);
        return true;
    }
};
struct VMap {
    std::vector<V3> v; std::vector<char> ok;
    bool at(const SphNodes& S, V3 u, V3& out) const {
        int n = S.nearest(u);
        V3 s{0, 0, 0}; double sw = 0; float sig = 0.75f * S.sp;
        for (int j : S.nb[n]) {
            if (!ok[j]) continue;
            float a = std::acos(std::clamp(dot(S.d[j], u), -1.0f, 1.0f));
            float w = (float)std::exp(-0.5 * (a / sig) * (a / sig));
            s = s + v[j] * w; sw += w;
        }
        if (sw < 1e-3) return false;
        V3 nn = Capsule::normal(u);
        s = s - nn * dot(s, nn);
        if (len(s) < 1e-6f) return false;
        out = nrmz(s);
        return true;
    }
};

// per-node percentile of the radii of samples within coneDeg of the node
static SMap percentileMap(const SphNodes& S, const std::vector<V3>& pts, const Capsule& C, float pct, float coneDeg, int minN) {
    std::vector<std::vector<float>> acc(S.d.size());
    float cc = std::cos(coneDeg * 3.14159265f / 180);
    for (auto& p : pts) {
        V3 u; float r;
        if (!C.key(p, u, r)) continue;
        int n = S.nearest(u);
        for (int j : S.nb[n]) if (dot(S.d[j], u) >= cc) acc[j].push_back(r);
    }
    SMap m; m.v.assign(S.d.size(), 0); m.ok.assign(S.d.size(), 0);
    for (size_t i = 0; i < acc.size(); i++) {
        auto& a = acc[i];
        if ((int)a.size() < minN) continue;
        size_t k = std::min(a.size() - 1, size_t(pct * (a.size() - 1) + 0.5f));
        std::nth_element(a.begin(), a.begin() + k, a.end());
        m.v[i] = a[k]; m.ok[i] = 1;
    }
    return m;
}
static void fillMap(const SphNodes& S, SMap& m, int passes) {
    for (int p = 0; p < passes; p++) {
        auto ok2 = m.ok; auto v2 = m.v;
        for (size_t i = 0; i < S.d.size(); i++) {
            if (m.ok[i]) continue;
            double s = 0; int n = 0;
            for (int j : S.nb[i]) if (m.ok[j]) { s += m.v[j]; n++; }
            if (n >= 2) { v2[i] = float(s / n); ok2[i] = 1; }
        }
        m.ok = ok2; m.v = v2;
    }
}
static void smoothMap(const SphNodes& S, SMap& m, int passes, float a) {
    for (int p = 0; p < passes; p++) {
        auto v2 = m.v;
        for (size_t i = 0; i < S.d.size(); i++) {
            if (!m.ok[i]) continue;
            double s = 0; int n = 0;
            for (int j : S.nb[i]) if (m.ok[j]) { s += m.v[j]; n++; }
            if (n) v2[i] = m.v[i] * (1 - a) + float(s / n) * a;
        }
        m.v = v2;
    }
}
// lump removal: values standing more than clip above the heavily smoothed map are cut down to it (the rest is kept)
static void clipLumps(const SphNodes& S, SMap& m, int passes, float clip) {
    SMap h = m;
    smoothMap(S, h, passes, 0.8f);
    for (size_t i = 0; i < m.v.size(); i++) if (m.ok[i] && m.v[i] > h.v[i] + clip) m.v[i] = h.v[i] + clip;
}
static void smoothVMap(const SphNodes& S, VMap& m, int passes, float a, int fill) {
    for (int p = 0; p < passes + fill; p++) {
        auto v2 = m.v; auto ok2 = m.ok;
        for (size_t i = 0; i < S.d.size(); i++) {
            V3 s{0, 0, 0}; int n = 0;
            for (int j : S.nb[i]) if (m.ok[j]) { s = s + m.v[j]; n++; }
            if (!n) continue;
            if (!m.ok[i]) { if (p >= passes || n >= 2) { v2[i] = s * (1.0f / n); ok2[i] = 1; } continue; }
            if (p < passes) v2[i] = m.v[i] * (1 - a) + s * (a / n);
        }
        m.v = v2; m.ok = ok2;
        for (size_t i = 0; i < S.d.size(); i++) if (m.ok[i]) {
            V3 nn = Capsule::normal(S.d[i]);
            V3 t = m.v[i] - nn * dot(m.v[i], nn);
            if (len(t) < 1e-6f) m.ok[i] = 0; else m.v[i] = nrmz(t);
        }
    }
}

}  // namespace cshell

static json conformShell(App& a, const json& c, int& grpId) {
    using namespace cshell;
    using namespace t6;
    json r;
    Groom& gr = a.groom;
    const V3 hc = a.pack.headC;
    Capsule CP; CP.c = hc; CP.R0 = c.value("capsule_mm", 120.0f) / 1000;
    const float PI = 3.14159265f;
    auto mmv = [](double m) { return std::round(m * 10000) / 10; };
    // ---- parameters
    bool hasCards = false;
    for (auto& m : a.pack.meshes) if (m.kind == "hair_cards" && !m.t.empty()) hasCards = true;
    std::string src = c.value("surface", std::string(hasCards ? "cards" : "guides"));   // cards (default when the pack has them) | guides | blend
    float blendCards = std::clamp(c.value("cards_mix", 0.5f), 0.0f, 1.0f);   // surface "blend": share of the card shell
    int layers = std::clamp(c.value("layers", 4), 1, 16);
    std::string assign = c.value("assign", std::string("even"));          // even | depth
    float rankMm = c.value("rank_mm", 25.0f) / 1000;
    float pct = std::clamp(c.value("shell_pct", 0.9f), 0.5f, 1.0f);
    float coneDeg = c.value("cone_deg", 6.0f);
    int smoothP = std::max(0, c.value("smooth", 8));
    float lumpMm = c.value("lump_mm", 3.0f) / 1000;                      // < 0 = no lump clip
    float offsetMm = c.value("offset_mm", 0.0f) / 1000;                  // outer surface pushed out (+) / in (-)
    float innerMm = c.value("inner_mm", 3.0f) / 1000;                    // innermost shell off the scalp
    float thickMax = c.value("thick_mm", 14.0f) / 1000;                  // max depth from the outer surface to the innermost shell
    float rootMm = c.value("root_mm", 10.0f) / 1000, blendMm = c.value("blend_mm", 25.0f) / 1000;
    float flowSt = std::clamp(c.value("flow", 0.8f), 0.0f, 1.0f);        // 0 = keep each guide's own direction
    std::string flowSrc = c.value("flow_source", std::string("neighbours"));  // neighbours | guides | cards | blend
    float nbMm = c.value("flow_radius_mm", 15.0f) / 1000;                // neighbours: guides rooted within this radius
    float flowCards = std::clamp(c.value("flow_cards_mix", 0.5f), 0.0f, 1.0f);
    int flowSmooth = std::max(0, c.value("flow_smooth", 6));
    float st = std::clamp(c.value("strength", 1.0f), 0.0f, 1.0f);
    bool followMode = c.value("flow_mode", std::string("align")) == "follow";   // align: only turn strands that roughly agree
    float rootLift = std::clamp(c.value("root_lift", 0.15f), 0.0f, 1.0f);   // max sine of a step's rise off the shell (1 = keep)
    float maxSlope = std::clamp(c.value("max_slope", 0.35f), 0.05f, 1.0f);
    float tipFree = std::clamp(c.value("tip_free", 0.0f), 0.0f, 0.5f);  // last share of the length that may leave the shell
    bool exFringe = c.value("exclude_fringe", true);
    float fringeBack = c.value("fringe_back_mm", 15.0f) / 1000, fringeUp = c.value("fringe_up_mm", 30.0f) / 1000;
    float fringeSide = c.value("fringe_side_mm", 15.0f) / 1000;
    bool dry = c.value("list_only", false);
    float spacingDeg = std::clamp(c.value("node_deg", 3.0f), 1.5f, 8.0f);

    SphNodes S; S.build(spacingDeg);
    Zones z = zonesOf(a, c);

    // ---- guides in scope
    std::vector<int> ix = a.selectGuides(c.value("where", json::object({{"all", true}})));
    std::set<int> prot;
    if (c.contains("protect")) for (int g : a.selectGuides(c["protect"])) prot.insert(g);
    auto isFringe = [&](int g) -> int {   // first point over the forehead / eyes, -1 = none
        if (!z.haveEye) return -1;
        float halfW = (z.eHi.x - z.eLo.x) / 2 + fringeSide;   // eye box half width + margin: over the forehead / eyes only
        for (size_t k = 0; k < gr.P[g].size(); k++) {
            V3 p = gr.P[g][k];
            if (z.fwd * (p.y - z.eyeY) > -fringeBack && p.z < z.browZ + fringeUp && std::fabs(p.x - z.C.x) < halfW) return (int)k;
        }
        return -1;
    };
    std::vector<int> use; int nFringe = 0, nProt = 0;
    std::string fringeMode = c.value("fringe", std::string("upper"));   // upper | skip
    float fringeFade = c.value("fringe_fade_mm", 0.0f) / 1000;
    std::vector<int> fringeK(gr.P.size(), -1);
    float carryMm = c.value("fringe_carry_mm", 30.0f) / 1000, carryMax = c.value("fringe_carry_max_mm", 12.0f) / 1000;
    for (int g : ix) {
        if (g < 0 || g >= (int)gr.P.size() || !gr.alive[g] || gr.P[g].size() < 3) continue;
        if (prot.count(g)) { nProt++; continue; }
        int kf = exFringe ? isFringe(g) : -1;
        if (kf >= 0) {
            nFringe++;
            if (fringeMode == "skip" || kf <= 1) continue;
            fringeK[g] = kf;   // "upper": the part before the forehead is laid, the fringe itself is carried along unchanged
        }
        use.push_back(g);
    }
    if (use.empty()) { r["error"] = "no guides to conform (where / protect / exclude_fringe left none)"; return r; }

    // ---- scalp map (posed head mesh, skull part)
    SMap scalp;
    {
        std::vector<V3> pos; std::vector<float> fl; std::vector<std::array<int, 3>> tr;
        std::vector<V3> sk;
        if (a.scalpMesh(pos, fl, tr)) sk = pos;
        scalp = percentileMap(S, sk, CP, c.value("scalp_pct", 0.8f), coneDeg, 3);   // high percentile: the skull, not the eye sockets / mouth inside
        fillMap(S, scalp, 2);
        smoothMap(S, scalp, 2, 0.5f);
    }
    // ---- outer surface
    SMap outer;
    std::vector<V3> cardPts;
    if (src != "guides" || flowSrc == "cards" || flowSrc == "blend") cardPts = cardHair(a, c.value("alpha_vis", 0.3f));
    {
        // guide envelope: all points past the root part of every alive guide (also the fringe: the silhouette is all hair)
        std::vector<V3> gp;
        for (size_t g = 0; g < gr.P.size(); g++) {
            if (!gr.alive[g]) continue;
            auto sl = arcLen(gr.P[g]);
            for (size_t k = 1; k < gr.P[g].size(); k++) if (sl[k] > rootMm) gp.push_back(gr.P[g][k]);
        }
        SMap mg = percentileMap(S, gp, CP, pct, coneDeg, 4);
        if (src == "guides") outer = mg;
        else {
            if (cardPts.empty()) { r["error"] = "this pack has no vanilla hair cards (surface 'cards' / 'blend' need them)"; return r; }
            SMap mc = percentileMap(S, cardPts, CP, pct, coneDeg, 4);
            if (src == "cards") outer = mc;
            else {
                outer = mg;
                for (size_t i = 0; i < outer.v.size(); i++) {
                    if (mg.ok[i] && mc.ok[i]) outer.v[i] = mg.v[i] * (1 - blendCards) + mc.v[i] * blendCards;
                    else if (mc.ok[i]) { outer.v[i] = mc.v[i]; outer.ok[i] = 1; }
                }
            }
        }
        if (lumpMm >= 0) clipLumps(S, outer, std::max(12, smoothP * 2), lumpMm);
        smoothMap(S, outer, smoothP, 0.6f);
    }
    if (c.contains("outer_mm") && c["outer_mm"].is_number()) {   // user-picked: one constant radius above the scalp
        float om = c["outer_mm"].get<float>() / 1000;
        for (size_t i = 0; i < outer.v.size(); i++) if (scalp.ok[i]) { outer.v[i] = scalp.v[i] + om; outer.ok[i] = 1; }
    }
    // shell radius toward u at depth fraction t (0 = outer surface, 1 = innermost); false = no outer surface there
    auto shellR = [&](V3 u, float t, float& R) {
        float ro;
        if (!outer.at(S, u, ro)) return false;
        ro += offsetMm;
        float rs, th = thickMax;
        if (scalp.at(S, u, rs)) th = std::clamp(ro - (rs + innerMm), 0.0f, thickMax);
        R = ro - t * th;
        if (scalp.at(S, u, rs)) R = std::max(R, rs + innerMm);
        return true;
    };
    auto depthFrac = [&](V3 p, float& t) {
        V3 u; float rl;
        if (!CP.key(p, u, rl)) return false;
        float r0, r1;
        if (!shellR(u, 0, r0) || !shellR(u, 1, r1)) return false;
        t = r0 - r1 > 1e-4f ? std::clamp((r0 - rl) / (r0 - r1), 0.0f, 1.0f) : 0.0f;
        return true;
    };
    auto depthMm = [&](V3 p, float& dm) {   // distance below the outer surface (m, < 0 = outside it)
        V3 u; float rl;
        if (!CP.key(p, u, rl)) return false;
        float r0;
        if (!shellR(u, 0, r0)) return false;
        dm = r0 - rl;
        return true;
    };

    // ---- flow field (two maps when a parting plane splits the head)
    bool part = c.contains("parting_x") && c["parting_x"].is_number();
    float partX = part ? hc.x + c["parting_x"].get<float>() : 0;
    auto sideOf = [&](V3 p) { return part && p.x < partX ? 1 : 0; };
    VMap flow[2];
    for (auto& f : flow) { f.v.assign(S.d.size(), V3{0, 0, 0}); f.ok.assign(S.d.size(), 0); }
    std::vector<V3> sumG[2] = {std::vector<V3>(S.d.size(), V3{0, 0, 0}), std::vector<V3>(S.d.size(), V3{0, 0, 0})};
    for (int g : use) {
        auto& G = gr.P[g];
        auto sl = arcLen(G);
        int sd = sideOf(G[0]);
        for (size_t k = 1; k < G.size(); k++) {
            if (sl[k] < rootMm) continue;
            V3 u; float rl;
            if (!CP.key(G[k], u, rl)) continue;
            V3 nn = Capsule::normal(u), dd = G[k] - G[k - 1];
            dd = dd - nn * dot(dd, nn);
            if (len(dd) < 1e-6f) continue;
            sumG[sd][S.nearest(u)] = sumG[sd][S.nearest(u)] + nrmz(dd);
        }
    }
    for (int sd = 0; sd < 2; sd++) for (size_t i = 0; i < S.d.size(); i++) if (len(sumG[sd][i]) > 1e-6f) { flow[sd].v[i] = sumG[sd][i]; flow[sd].ok[i] = 1; }
    for (int sd = 0; sd < 2; sd++) smoothVMap(S, flow[sd], 1, 0.0f, 0);   // normalise / tangent
    int cardFlowN = 0;
    if ((flowSrc == "cards" || flowSrc == "blend") && !cardPts.empty()) {
        // card texture direction: per card triangle the 3D direction of increasing v, signed to agree with the guide flow
        // (or downward when there is none)
        std::vector<V3> sumC[2] = {std::vector<V3>(S.d.size(), V3{0, 0, 0}), std::vector<V3>(S.d.size(), V3{0, 0, 0})};
        for (auto& m : a.pack.meshes) {
            if (m.kind != "hair_cards" || m.uv.size() != m.v.size()) continue;
            for (auto& t : m.t) {
                V3 e1 = m.v[t[1]] - m.v[t[0]], e2 = m.v[t[2]] - m.v[t[0]];
                float du1 = m.uv[t[1]][0] - m.uv[t[0]][0], dv1 = m.uv[t[1]][1] - m.uv[t[0]][1];
                float du2 = m.uv[t[2]][0] - m.uv[t[0]][0], dv2 = m.uv[t[2]][1] - m.uv[t[0]][1];
                float det = du1 * dv2 - du2 * dv1;
                if (std::fabs(det) < 1e-9f) continue;
                V3 tv = (e2 * du1 - e1 * du2) * (1.0f / det);
                V3 cp = (m.v[t[0]] + m.v[t[1]] + m.v[t[2]]) * (1.0f / 3);
                V3 u; float rl;
                if (!CP.key(cp, u, rl) || len(tv) < 1e-6f) continue;
                V3 nn = Capsule::normal(u);
                tv = tv - nn * dot(tv, nn);
                if (len(tv) < 1e-6f) continue;
                tv = nrmz(tv);
                int sd = sideOf(cp);
                V3 ref;
                if (!flow[sd].at(S, u, ref)) ref = V3{0, 0, -1};
                if (dot(tv, ref) < 0) tv = tv * -1.0f;
                int n = S.nearest(u);
                sumC[sd][n] = sumC[sd][n] + tv;
                cardFlowN++;
            }
        }
        for (int sd = 0; sd < 2; sd++) {
            float wc = flowSrc == "cards" ? 1.0f : flowCards;
            for (size_t i = 0; i < S.d.size(); i++) {
                V3 gv = flow[sd].ok[i] ? flow[sd].v[i] : V3{0, 0, 0}, cv = len(sumC[sd][i]) > 1e-6f ? nrmz(sumC[sd][i]) : V3{0, 0, 0};
                if (flowSrc == "cards" && len(cv) < 1e-6f) { flow[sd].ok[i] = 0; continue; }
                V3 s = gv * (1 - wc) + cv * wc;
                if (len(s) > 1e-6f) { flow[sd].v[i] = s; flow[sd].ok[i] = 1; }
            }
        }
    }
    for (int sd = 0; sd < 2; sd++) smoothVMap(S, flow[sd], flowSmooth, 0.6f, 3);
    // parting: in the band near the plane the flow must lead away from it
    float partBand = c.value("parting_band_mm", 30.0f) / 1000;
    auto flowAt = [&](V3 p, int sd, V3& f) {
        V3 u; float rl;
        if (!CP.key(p, u, rl)) return false;
        if (!flow[sd].at(S, u, f)) return false;
        V3 nn = Capsule::normal(u);
        if (part && std::fabs(p.x - partX) < partBand && p.z > hc.z) {
            V3 away{sd == 1 ? -1.0f : 1.0f, 0, 0};
            away = away - nn * dot(away, nn);
            float w = 1 - std::fabs(p.x - partX) / partBand;
            if (dot(f, away) < 0) f = nrmz(f + away * (w * 2 * std::fabs(dot(f, away)) + 0.01f));
        }
        return true;
    };

    // ---- current depth of every guide (mean depth fraction past the root part) + wander (before)
    std::vector<float> tG(gr.P.size(), 0.5f);
    double wanderB = 0; int wanderN = 0;
    auto wander = [&](const Guide& G) {   // std of the distance below the outer surface along the strand (m), -1 = none
        auto sl = arcLen(G);
        double s = 0, s2 = 0; int n = 0;
        float L = sl.back();
        for (size_t k = 1; k < G.size(); k++) {
            if (sl[k] < rootMm + blendMm || sl[k] > L * (1 - tipFree)) continue;
            float dm; if (!depthMm(G[k], dm)) continue;
            s += dm; s2 += dm * dm; n++;
        }
        if (n < 2) return -1.0;
        double m = s / n;
        return std::sqrt(std::max(0.0, s2 / n - m * m));
    };
    for (int g : use) {
        auto& G = gr.P[g];
        auto sl = arcLen(G);
        double s = 0; int n = 0;
        for (size_t k = 1; k < G.size(); k++) { if (sl[k] < rootMm) continue; float t; if (depthFrac(G[k], t)) { s += t; n++; } }
        tG[g] = n ? float(s / n) : 0.5f;
        double w = wander(G); if (w >= 0) { wanderB += w; wanderN++; }
    }
    // ---- layer per guide
    std::vector<int> layerOf(gr.P.size(), 0);
    std::vector<int> pop(layers, 0);
    for (int g : use) {
        int L;
        if (layers == 1) L = 0;
        else if (assign == "depth") L = std::clamp(int(std::lround(tG[g] * (layers - 1))), 0, layers - 1);
        else {
            int below = 0, eq = 0, tot = 0;
            for (int h : use) {
                if (len(gr.P[h][0] - gr.P[g][0]) > rankMm) continue;
                tot++;
                if (tG[h] < tG[g]) below++; else if (tG[h] == tG[g]) eq++;
            }
            float q = tot ? (below + 0.5f * eq) / tot : 0.5f;
            L = std::clamp(int(q * layers), 0, layers - 1);
        }
        layerOf[g] = L; pop[L]++;
    }
    auto tOfLayer = [&](int L) { return layers == 1 ? 0.0f : float(L) / (layers - 1); };

    // ---- neighbour consensus flow: at arc length s a guide heads where the guides rooted around it head at the same s
    // (smooth_flow idea: parallel neighbours, the overall shape and the hem stay where they were)
    bool nbFlow = flowSrc == "neighbours";
    std::vector<std::vector<std::pair<int, float>>> nbs;
    std::vector<std::vector<float>> slOf;
    if (nbFlow) {
        nbs.resize(gr.P.size()); slOf.resize(gr.P.size());
        for (int g : use) slOf[g] = arcLen(gr.P[g]);
        float sig = std::max(nbMm * 0.6f, 1e-3f);
        for (int g : use) for (int h : use) {
            float dd = len(gr.P[h][0] - gr.P[g][0]);
            if (dd <= nbMm) nbs[g].push_back({h, std::exp(-0.5f * (dd / sig) * (dd / sig))});
        }
    }
    auto dirAtS = [&](int h, float s) {   // unit direction of guide h at arc length s (last segment past its tip)
        auto& G = gr.P[h]; auto& sl = slOf[h];
        size_t k = 1;
        while (k + 1 < G.size() && sl[k] < s) k++;
        return nrmz(G[k] - G[k - 1]);
    };
    auto nbFlowAt = [&](int g, float s, V3 own, V3 up, V3& f) {
        V3 acc{0, 0, 0};
        for (auto& [h, w] : nbs[g]) {
            if (slOf[h].back() < s * 0.6f) continue;   // much shorter neighbours do not steer the long ones' ends
            V3 d = dirAtS(h, s);
            if (dot(d, own) <= 0.0f) continue;          // across a parting / whorl: not a neighbour in flow
            acc = acc + d * w;
        }
        acc = acc - up * dot(acc, up);
        if (len(acc) < 1e-6f) return false;
        f = nrmz(acc);
        return true;
    };

    // ---- re-lay
    bool trimHem = c.value("trim_hem", false);
    float trimMm = c.value("trim_mm", 5.0f) / 1000;
    int nTrim = 0, nClamp = 0;
    float clampMm = c.value("clamp_mm", 1.0f) / 1000;   // < 0 = off
    HeadRadial hr = headRadial(a);
    int id = 0, grp = 1000 + (++grpId), n = 0, offShell = 0;
    for (auto& o : gr.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    std::vector<json> ops;
    double moved = 0, wanderA = 0; int movedN = 0, wanderAN = 0, outB = 0, outA = 0, layErrN = 0; double layErr = 0;
    for (int g : use) {
        const Guide& G = gr.P[g];
        size_t K = G.size();
        auto sl = arcLen(G);
        float L = sl.back();
        if (L < rootMm + 0.005f) continue;
        float t = tOfLayer(layerOf[g]);
        int sd = sideOf(G[0]);
        int kOff = -1;   // first point below the head centre past the vanilla outline (hem / collar)
        Guide R2 = G;
        bool any = false;
        int kf = fringeK[g];
        for (size_t k = 1; k < K; k++) {
            float seg = sl[k] - sl[k - 1];
            if (kf >= 0 && (int)k >= kf) {
                // the fringe follows its laid root part at the top (no gap to the layers) and returns to its own place
                // over fringe_carry_mm (the part over the eye stays where it was)
                V3 off = R2[kf - 1] - G[kf - 1];
                if (len(off) > carryMax) off = nrmz(off) * carryMax;
                float w = std::clamp(1 - (sl[k] - sl[kf - 1]) / std::max(carryMm, 1e-3f), 0.0f, 1.0f);
                R2[k] = G[k] + off * w;
                continue;
            }
            if (sl[k] <= rootMm && rootLift >= 1) { R2[k] = R2[k - 1] + nrmz(G[k] - G[k - 1]) * seg; continue; }
            float b = std::clamp((sl[k] - rootMm) / std::max(blendMm, 1e-3f), 0.0f, 1.0f);
            b = b * b * (3 - 2 * b);
            float u01 = sl[k] / L;
            if (tipFree > 0 && u01 > 1 - tipFree) b *= std::clamp((1 - u01) / tipFree, 0.0f, 1.0f);
            if (kf >= 0 && fringeFade > 0) b *= std::clamp((sl[kf] - sl[k]) / fringeFade, 0.0f, 1.0f);
            V3 prev = R2[k - 1];
            V3 dOrig = nrmz(G[k] - G[k - 1]);
            V3 uk; float rp;
            V3 up = CP.key(prev, uk, rp) ? Capsule::normal(uk) : V3{0, 0, 1};
            // tangential direction: the guide's own, turned toward the flow where they roughly agree (strands crossing
            // the flow are straightened, never reversed)
            V3 dt = dOrig - up * dot(dOrig, up);
            float tl = len(dt);
            V3 tdir = tl > 1e-6f ? dt * (1.0f / tl) : V3{0, 0, 0};
            V3 f;
            if (nbFlow ? nbFlowAt(g, sl[k], dOrig, up, f) : flowAt(prev, sd, f)) {
                if (tl < 1e-6f) tdir = f;
                else {
                    float agree = (followMode || nbFlow) ? 1.0f : std::max(0.0f, dot(tdir, f));
                    float w = flowSt * b * std::sqrt(agree);
                    V3 m = tdir * (1 - w) + f * w;
                    if (len(m) > 1e-6f) tdir = nrmz(m);
                }
            }
            if (len(tdir) < 1e-6f) tdir = dOrig;
            float dr0 = std::clamp(dot(dOrig, up), -1.0f, rootLift) * seg;   // original radial step, lift capped (lumps = lifted roots)
            V3 p = prev + tdir * (seg * std::sqrt(std::max(0.0f, 1 - std::min(0.95f, dr0 * dr0 / (seg * seg))))) + up * dr0;
            for (int it = 0; it < 3; it++) {
                V3 u; float rl;
                if (!CP.key(p, u, rl)) break;
                float rt;
                if (!shellR(u, t, rt)) {
                    if (it == 0) { offShell++; if (kOff < 0 && kf < 0 && p.z < hc.z && b >= 1) kOff = (int)k; }
                    break;
                }
                // radial step: blend of the original one and the step toward the shell, slope-limited (no spikes / dives)
                float lim = maxSlope * seg;
                float dr = dr0 * (1 - b) + std::clamp(rt - rp, -lim, lim) * b;
                V3 pn = CP.point(u, rp + dr);
                p = prev + nrmz(pn - prev) * seg;
                any = true;
            }
            R2[k] = p;
        }
        if (!any) continue;
        // hem (opt-in trim_hem): strands reaching past the vanilla outline below the head (collar clipping) are scaled
        // down to end trim_mm past their last point inside it; every segment keeps its direction
        if (trimHem && kOff > 1) {
            float Lc = sl[kOff - 1] + trimMm;
            if (Lc < L) {
                float sc = Lc / L;
                for (size_t k = 1; k < K; k++) R2[k] = R2[k - 1] + nrmz(R2[k] - R2[k - 1]) * ((sl[k] - sl[k - 1]) * sc);
                for (auto& x : sl) x *= sc;
                L = sl.back(); nTrim++;
            }
        }
        // strength blend, then re-walk the segment lengths
        if (st < 1) {
            Guide B = R2;
            for (size_t k = 1; k < K; k++) B[k] = G[k] + (R2[k] - G[k]) * st;
            for (size_t k = 1; k < K; k++) R2[k] = R2[k - 1] + nrmz(B[k] - R2[k - 1]) * (sl[k] - sl[k - 1]);
        }
        // light smoothing (root part and tip fixed) against step kinks, lengths re-walked
        for (int it = 0; it < 2; it++) {
            Guide T = R2;
            for (size_t k = 2; k + 1 < K; k++) if (sl[k] > rootMm && (kf < 0 || (int)k < kf - 1)) T[k] = R2[k] * 0.5f + (R2[k - 1] + R2[k + 1]) * 0.25f;
            for (size_t k = 1; k < K; k++) T[k] = T[k - 1] + nrmz(T[k] - T[k - 1]) * (sl[k] - sl[k - 1]);
            R2 = T;
        }
        // hard outer limit (clamp_mm >= 0): nothing past the root part stands more than clamp_mm outside the outer
        // surface (strands sticking out of the crown), constrained re-walk keeps the segment lengths
        if (clampMm >= 0) for (int it = 0; it < 2; it++)
            for (size_t k = 1; k < K; k++) {
                if (kf >= 0 && (int)k >= kf) break;   // the fringe keeps its own place
                V3 u; float rl, r0;
                if (!CP.key(R2[k], u, rl) || !shellR(u, 0, r0)) continue;
                float seg = len(R2[k] - R2[k - 1]);
                if (rl > r0 + clampMm) {
                    V3 q = CP.point(u, r0 + clampMm);
                    V3 nk = R2[k - 1] + nrmz(q - R2[k - 1]) * seg;
                    V3 dlt = nk - R2[k];
                    for (size_t j = k; j < K; j++) R2[j] = R2[j] + dlt;   // carry the rest along
                    nClamp++;
                }
            }
        pushOut(hr, R2, 0.003f);
        double w = wander(R2); if (w >= 0) { wanderA += w; wanderAN++; }
        for (size_t k = 1; k < K; k++) {
            float dm;
            if (sl[k] > rootMm && depthMm(G[k], dm) && dm < -0.002f) outB++;
            if (sl[k] > rootMm && depthMm(R2[k], dm) && dm < -0.002f) outA++;
            if (sl[k] > rootMm + blendMm && depthMm(R2[k], dm)) { float tt; depthFrac(R2[k], tt); layErr += std::fabs(tt - t); layErrN++; }
        }
        for (size_t k = 1; k < K; k++) { moved += len(R2[k] - G[k]); movedN++; }
        if (c.value("debug_guide", -1) == g) {
            json dbg = json::array();
            for (size_t k = 0; k < K; k++) {
                V3 u0, u1; float r0 = 0, r1 = 0, s0 = -1, s1 = -1, d0 = 0, d1 = 0;
                CP.key(G[k], u0, r0); CP.key(R2[k], u1, r1); shellR(u0, t, s0); shellR(u1, t, s1); depthMm(G[k], d0); depthMm(R2[k], d1);
                dbg.push_back({{"sl", mmv(sl[k])}, {"r_before", mmv(r0)}, {"r_after", mmv(r1)}, {"shell_at_after", mmv(s1)}, {"below_outer_before", mmv(d0)}, {"below_outer_after", mmv(d1)}});
            }
            r["debug"] = {{"guide", g}, {"layer", layerOf[g]}, {"t", t}, {"points", dbg}};
        }
        V3 r0 = G[0];
        ops.push_back({{"op", "reshape"}, {"root", {r4(r0.x), r4(r0.y), r4(r0.z)}}, {"points", ptsJson(R2)}, {"id", ++id}, {"grp", grp}});
        n++;
    }
    int outerN = 0; for (auto o : outer.ok) outerN += o;
    r["changed"] = n; r["in_scope"] = use.size(); r["fringe_guides"] = nFringe; r["hem_trimmed"] = nTrim; r["clamped_points"] = nClamp;
    if (dry) { json fl = json::array(); for (size_t g = 0; g < fringeK.size(); g++) if (fringeK[g] >= 0) fl.push_back(g); r["fringe_indices"] = fl; } r["protected"] = nProt;
    r["layers"] = layers; r["layer_population"] = pop; r["points_off_shell"] = offShell;
    r["outer_nodes"] = outerN; r["nodes"] = S.d.size(); r["card_flow_triangles"] = cardFlowN;
    r["depth_wander_mm_before"] = wanderN ? mmv(wanderB / wanderN) : 0;
    r["depth_wander_mm_after"] = wanderAN ? mmv(wanderA / wanderAN) : 0;
    r["mean_point_move_mm"] = movedN ? mmv(moved / movedN) : 0;
    r["points_2mm_outside_before"] = outB; r["points_2mm_outside_after"] = outA;
    r["mean_layer_error"] = layErrN ? std::round(layErr / layErrN * 1000) / 1000 : 0;   // depth fraction off the assigned shell
    if (dry) return r;
    for (auto& o : ops) gr.ops.push_back(o);
    gr.redo.clear(); gr.replay(); a.editsChanged();
    return r;
}
