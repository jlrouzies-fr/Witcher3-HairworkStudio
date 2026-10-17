// ------------------------------------------------------------------------------------------------ braid (2026-10-15)
// Opt-in Studio tool (studio_braid / inspector "Braid tool"): replaces small vanilla braid cards (Donar's side beard
// braids) with a HairWorks 3-strand plait. Included by serve.cpp after the t6 / r3 helpers.
//
// Shape. The braid follows a centreline (derived from the card strips: PCA axis, slice centroids, slice width, or
// explicit control points). Three lobes interleave as a real plait: lobe i at arc length s sits at
//   lateral = a(s) sin(theta + 2 pi i / 3),  depth = b(s) sin(2 (theta + 2 pi i / 3)),  theta = 2 pi s / period
// which never makes two lobes meet (the classic figure-eight plait). A tie narrows the plait at its end and a tassel
// (loose brush) hangs below it.
//
// Hair. Guides have only K CVs (the beard has 10), far too few for a 5-15 mm plait period over a 10 cm braid. So each
// lobe is made of overlapping PIECES: short guides (K CVs over piece_mm, ~6 CVs per period) whose roots sit ON the
// lobe path (they are skinned like the base guide, so the braid is rigid with the jaw, like the vanilla cards). Each
// piece = guides_per_lobe guides on a small ring around the lobe centre + growth faces ONLY inside that ring (a fan):
// no face links two lobes, two pieces or the beard (no stray interpolated hair). With piece_mm >= the braid length
// the lobes are whole-length guides instead (they then swing as a unit; needs enough CVs: studio_resample).
// Hold: per-guide stiffness map s = r = stiffness (1) on every braid guide (the global look is not touched).
//
// Written as ONE "rebuild" op (delete nothing, guides + base + uv + faces), so it is undoable, saved in projects and
// written by the Studio export like any other guide. The op carries {"rebuild": {"kind": "braid", "params"}} so a
// later tool (a Ciri bun = lobes wound around an axis instead of plaited along a line) can reuse the piece builder.

namespace braid {

static V3 j3(const json& j) { return V3{j[0].get<float>(), j[1].get<float>(), j[2].get<float>()}; }

// principal axes of a point set (power iteration on the covariance), e0 = largest
static void pca(const std::vector<V3>& P, V3& mean, V3& e0, V3& e1, V3& e2) {
    mean = {0, 0, 0};
    for (auto& p : P) mean = mean + p;
    mean = mean * (1.0f / std::max<size_t>(1, P.size()));
    double C[3][3] = {{0}};
    for (auto& p : P) {
        double d[3] = {p.x - mean.x, p.y - mean.y, p.z - mean.z};
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) C[i][j] += d[i] * d[j];
    }
    auto power = [&](V3 v, const V3* ortho) {
        for (int it = 0; it < 64; it++) {
            V3 w{float(C[0][0] * v.x + C[0][1] * v.y + C[0][2] * v.z), float(C[1][0] * v.x + C[1][1] * v.y + C[1][2] * v.z),
                 float(C[2][0] * v.x + C[2][1] * v.y + C[2][2] * v.z)};
            if (ortho) w = w - *ortho * dot(w, *ortho);
            v = nrmz(w);
        }
        return v;
    };
    e0 = power(V3{0.1f, 0.2f, 1.0f}, nullptr);
    e1 = power(nrmz(cross(e0, V3{0, 0, 1}) + V3{0.01f, 0.02f, 0.03f}), &e0);
    e2 = nrmz(cross(e0, e1));
}

struct Path {           // braid centreline, top (root) -> bottom (tie), plus per-sample width / frame
    std::vector<V3> c;  // samples every ds
    std::vector<float> w;
    std::vector<V3> T, U, V;
    float ds = 0.00025f;
    float length() const { return c.size() > 1 ? ds * (c.size() - 1) : 0.0f; }
    int at(float s) const { return std::clamp(int(std::lround(s / ds)), 0, int(c.size()) - 1); }
};

// centreline through ctl (top -> bottom), width per control point, width axis hint wAx
static Path makePath(const std::vector<V3>& ctl, const std::vector<float>& wCtl, V3 wAx) {
    Path P;
    Guide g(ctl.begin(), ctl.end());
    std::vector<V3> dense = smoothDense(g, 24);
    float L = 0;
    std::vector<float> d(dense.size(), 0.0f);
    for (size_t i = 1; i < dense.size(); i++) d[i] = d[i - 1] + len(dense[i] - dense[i - 1]);
    L = d.back();
    // arc length of each control point (nearest dense sample) for the width interpolation
    std::vector<float> sc(ctl.size(), 0.0f);
    for (size_t k = 0; k < ctl.size(); k++) {
        float bd = 1e9f;
        for (size_t i = 0; i < dense.size(); i++) { float q = len(dense[i] - ctl[k]); if (q < bd) { bd = q; sc[k] = d[i]; } }
    }
    int n = std::max(2, int(L / P.ds) + 1);
    std::vector<float> s(n);
    for (int i = 0; i < n; i++) s[i] = std::min(L, i * P.ds);
    P.c = sampleSmoothAt(g, s);
    P.w.resize(n);
    for (int i = 0; i < n; i++) {
        size_t k = 1;
        while (k < sc.size() - 1 && sc[k] < s[i]) k++;
        float a = std::clamp((s[i] - sc[k - 1]) / std::max(sc[k] - sc[k - 1], 1e-6f), 0.0f, 1.0f);
        P.w[i] = wCtl[k - 1] + (wCtl[k] - wCtl[k - 1]) * a;
    }
    P.T.resize(n); P.U.resize(n); P.V.resize(n);
    for (int i = 0; i < n; i++) {
        int i0 = std::max(0, i - 8), i1 = std::min(n - 1, i + 8);
        V3 T = nrmz(P.c[i1] - P.c[i0]);
        V3 U = wAx - T * dot(wAx, T);
        if (len(U) < 1e-4f) U = cross(T, V3{0, 1, 0});
        U = nrmz(U);
        if (i > 0 && dot(U, P.U[i - 1]) < 0) U = U * -1.0f;
        P.T[i] = T; P.U[i] = U; P.V[i] = nrmz(cross(T, U));
    }
    return P;
}

// card strips -> braid groups: components (connected triangles) of the given triangle sets, grouped when they lie within
// join_mm of each other; per group: control points + widths from slices along its principal axis
struct CardBraid { std::vector<V3> ctl; std::vector<float> wid; V3 wAx; float tieS = -1, tasselLen = 0; std::vector<std::pair<int, int>> tris; int comps = 0; };

static std::vector<CardBraid> fromCards(const App& a, const std::map<int, std::vector<uint8_t>>& masks, const json& c, json& info) {
    std::vector<CardBraid> out;
    struct Comp { int mesh; std::vector<int> tris; std::vector<V3> v; V3 lo, hi; int grp = -1; };
    std::vector<Comp> comps;
    for (auto& [mi, mask] : masks) {
        if (mi < 0 || mi >= int(a.pack.meshes.size())) continue;
        const Mesh& m = a.pack.meshes[mi];
        std::vector<int> par(m.v.size());
        for (size_t i = 0; i < par.size(); i++) par[i] = int(i);
        std::function<int(int)> fd = [&](int x) { while (par[x] != x) x = par[x] = par[par[x]]; return x; };
        for (size_t ti = 0; ti < m.t.size() && ti < mask.size(); ti++) if (mask[ti]) { auto& t = m.t[ti]; par[fd(t[1])] = fd(t[0]); par[fd(t[2])] = fd(t[0]); }
        std::map<int, int> ci;
        for (size_t ti = 0; ti < m.t.size() && ti < mask.size(); ti++) {
            if (!mask[ti]) continue;
            int k = fd(m.t[ti][0]);
            auto it = ci.find(k);
            if (it == ci.end()) { ci[k] = int(comps.size()); comps.push_back({mi, {}, {}, {1e9f, 1e9f, 1e9f}, {-1e9f, -1e9f, -1e9f}}); it = ci.find(k); }
            Comp& C = comps[it->second];
            C.tris.push_back(int(ti));
            for (int q = 0; q < 3; q++) {
                V3 p = m.v[m.t[ti][q]];
                C.v.push_back(p);
                C.lo = {std::min(C.lo.x, p.x), std::min(C.lo.y, p.y), std::min(C.lo.z, p.z)};
                C.hi = {std::max(C.hi.x, p.x), std::max(C.hi.y, p.y), std::max(C.hi.z, p.z)};
            }
        }
    }
    if (comps.empty()) return out;
    // group: bounding boxes closer than join_mm
    float join = c.value("join_mm", 6.0f) / 1000;
    int ng = 0;
    for (size_t i = 0; i < comps.size(); i++) {
        if (comps[i].grp >= 0) continue;
        comps[i].grp = ng;
        std::vector<size_t> st{i};
        while (!st.empty()) {
            size_t q = st.back(); st.pop_back();
            for (size_t j = 0; j < comps.size(); j++) {
                if (comps[j].grp >= 0) continue;
                const Comp &A = comps[q], &B = comps[j];
                float gx = std::max({0.0f, A.lo.x - B.hi.x, B.lo.x - A.hi.x}), gy = std::max({0.0f, A.lo.y - B.hi.y, B.lo.y - A.hi.y}),
                      gz = std::max({0.0f, A.lo.z - B.hi.z, B.lo.z - A.hi.z});
                if (std::sqrt(gx * gx + gy * gy + gz * gz) < join) { comps[j].grp = ng; st.push_back(j); }
            }
        }
        ng++;
    }
    const int nSlice = std::clamp(c.value("slices", 16), 4, 64);
    for (int g = 0; g < ng; g++) {
        std::vector<V3> pts;
        int big = -1; size_t bigN = 0;
        CardBraid B;
        for (size_t i = 0; i < comps.size(); i++) {
            if (comps[i].grp != g) continue;
            pts.insert(pts.end(), comps[i].v.begin(), comps[i].v.end());
            for (int t : comps[i].tris) B.tris.push_back({comps[i].mesh, t});
            B.comps++;
            if (comps[i].tris.size() > bigN) { bigN = comps[i].tris.size(); big = int(i); }
        }
        if (pts.size() < 9) continue;
        V3 mean, e0, e1, e2;
        pca(pts, mean, e0, e1, e2);
        // top = the end nearer the head centre's height (braids hang down)
        if (e0.z > 0) e0 = e0 * -1.0f;   // e0 points down: t grows toward the tip
        float t0 = 1e9f, t1 = -1e9f;
        for (auto& p : pts) { float t = dot(p - mean, e0); t0 = std::min(t0, t); t1 = std::max(t1, t); }
        // body = the biggest component (the plaited strip); below its end = tie + tassel pieces
        float bt0 = 1e9f, bt1 = -1e9f;
        for (auto& p : comps[big].v) { float t = dot(p - mean, e0); bt0 = std::min(bt0, t); bt1 = std::max(bt1, t); }
        bool hasTassel = (t1 - bt1) > 0.004f && B.comps > 1;
        float bodyEnd = hasTassel ? bt1 : t1;
        // slices along the body: centroid + width (10..90 % of the lateral spread along e1)
        std::vector<std::vector<V3>> sl(nSlice);
        for (auto& p : comps[big].v) {   // body slices from the body strip only (tie / tassel strips overlap its end)
            float t = dot(p - mean, e0);
            int k = std::clamp(int((t - t0) / std::max(bodyEnd - t0, 1e-5f) * nSlice), 0, nSlice - 1);
            sl[k].push_back(p);
        }
        if (!hasTassel)
            for (auto& p : pts) {
                float t = dot(p - mean, e0);
                int k = std::clamp(int((t - t0) / std::max(bodyEnd - t0, 1e-5f) * nSlice), 0, nSlice - 1);
                sl[k].push_back(p);
            }
        std::vector<V3> ctl; std::vector<float> wid;
        for (int k = 0; k < nSlice; k++) {
            if (sl[k].size() < 2) continue;
            V3 cc{0, 0, 0};
            for (auto& p : sl[k]) cc = cc + p;
            cc = cc * (1.0f / sl[k].size());
            std::vector<float> lat;
            for (auto& p : sl[k]) lat.push_back(dot(p - cc, e1));
            std::sort(lat.begin(), lat.end());
            float w = lat[lat.size() * 9 / 10] - lat[lat.size() / 10];
            // the slice centroid moved onto the axis position of the slice (even spacing)
            float tc = t0 + (k + 0.5f) / nSlice * (bodyEnd - t0);
            V3 onAx = mean + e0 * tc;
            V3 off = cc - onAx; off = off - e0 * dot(off, e0);
            ctl.push_back(onAx + off);
            wid.push_back(std::max(w, 0.001f));
        }
        if (ctl.size() < 3) continue;
        // extend to the strip's exact ends (first / last slice centres are half a slice inside)
        V3 dTop = nrmz(ctl[0] - ctl[1]), dBot = nrmz(ctl.back() - ctl[ctl.size() - 2]);
        float half = 0.5f / nSlice * (bodyEnd - t0);
        ctl.insert(ctl.begin(), ctl[0] + dTop * half); wid.insert(wid.begin(), wid[0]);
        ctl.push_back(ctl.back() + dBot * half); wid.push_back(wid.back());
        // light smoothing of the widths (card strips are ragged)
        std::vector<float> w2 = wid;
        for (size_t k = 1; k + 1 < wid.size(); k++) w2[k] = 0.25f * wid[k - 1] + 0.5f * wid[k] + 0.25f * wid[k + 1];
        B.ctl = ctl; B.wid = w2; B.wAx = e1;
        B.tasselLen = hasTassel ? (t1 - bodyEnd) : 0.0f;
        out.push_back(B);
        json bi = {{"components", B.comps}, {"triangles", B.tris.size()}, {"top", v3j(ctl.front())}, {"tie", v3j(ctl.back())},
                   {"body_length_mm", std::round((bodyEnd - t0) * 10000) / 10}, {"tassel_mm", std::round(B.tasselLen * 10000) / 10}};
        json ws = json::array();
        for (float w : w2) ws.push_back(std::round(w * 10000) / 10);
        bi["width_mm"] = ws;
        info.push_back(bi);
    }
    return out;
}

}  // namespace braid

json App::braidTool(const json& c) {
    using namespace braid;
    json r;
    Groom& gr = groom;
    if (gr.P.empty()) { r["error"] = "no guides"; return r; }
    const int K = gr.K;
    // ---------------- sources
    std::vector<CardBraid> src;
    json info = json::array();
    std::string from = c.value("from", std::string(c.contains("points") ? "points" : c.contains("near") ? "card" : "kept"));
    std::map<int, std::vector<uint8_t>> pickMask;
    if (from == "kept") {
        if (keepCards.empty()) { r["error"] = "no kept cards: pick the braid with studio_keep_cards (or give near / points)"; return r; }
        pickMask = keepCards;
    } else if (from == "card") {
        if (!c.contains("near")) { r["error"] = "from card needs near [x,y,z] (+ radius_mm)"; return r; }
        V3 p = j3(c["near"]);
        float rad = c.value("radius_mm", 25.0f) / 1000;
        float zmax = c.value("z_max", 1e9f), zmin = c.value("z_min", -1e9f);
        for (size_t mi = 0; mi < pack.meshes.size(); mi++) {
            const Mesh& m = pack.meshes[mi];
            if (m.kind != "hair_cards" || m.t.empty()) continue;
            std::vector<int> par(m.v.size());
            for (size_t i = 0; i < par.size(); i++) par[i] = int(i);
            std::function<int(int)> fd = [&](int x) { while (par[x] != x) x = par[x] = par[par[x]]; return x; };
            for (auto& t : m.t) { par[fd(t[1])] = fd(t[0]); par[fd(t[2])] = fd(t[0]); }
            std::set<int> hit;
            for (size_t i = 0; i < m.v.size(); i++) if (len(m.v[i] - p) < rad) hit.insert(fd(int(i)));
            if (hit.empty()) continue;
            auto& mask = pickMask[int(mi)];
            mask.assign(m.t.size(), 0);
            for (size_t ti = 0; ti < m.t.size(); ti++) {
                V3 q = (m.v[m.t[ti][0]] + m.v[m.t[ti][1]] + m.v[m.t[ti][2]]) * (1.0f / 3);
                if (q.z > zmax || q.z < zmin) continue;
                if (hit.count(fd(m.t[ti][0]))) mask[ti] = 1;
            }
        }
        if (pickMask.empty()) { r["error"] = "no card strip near that point"; return r; }
    }
    if (from == "kept" || from == "card") {
        src = fromCards(*this, pickMask, c, info);
        if (src.empty()) { r["error"] = "no braid found in the picked cards"; return r; }
        if (c.contains("only") && c["only"].is_number_integer()) {   // one braid of the found groups
            int k = c["only"];
            if (k < 0 || k >= int(src.size())) { r["error"] = "only: index out of range"; return r; }
            src = {src[k]};
        }
    } else if (from == "points") {
        if (!c["points"].is_array() || c["points"].size() < 2) { r["error"] = "points: at least 2 [x,y,z] (top -> bottom)"; return r; }
        CardBraid B;
        float w = c.value("width_mm", 6.0f) / 1000;
        for (auto& p : c["points"]) { B.ctl.push_back(j3(p)); B.wid.push_back(w); }
        if (B.ctl.size() == 2) B.ctl.insert(B.ctl.begin() + 1, (B.ctl[0] + B.ctl[1]) * 0.5f), B.wid.push_back(w);
        V3 ax = nrmz(B.ctl.back() - B.ctl.front());
        B.wAx = c.contains("width_axis") ? nrmz(j3(c["width_axis"])) : nrmz(cross(ax, V3{0, 1, 0}));
        B.tasselLen = c.value("tassel_mm", 15.0f) / 1000;
        src.push_back(B);
    } else { r["error"] = "from: kept | card | points"; return r; }
    // top_points [[x,y,z],..] (opt-in): each braid starts at the nearest of these points (e.g. the mouth corners, so the
    // braid comes out of the side of the moustache); card control points within top_blend_mm of it are dropped
    if (c.contains("top_points") && c["top_points"].is_array() && !c["top_points"].empty()) {
        float blend = c.value("top_blend_mm", 8.0f) / 1000;
        for (auto& B : src) {
            if (B.ctl.empty()) continue;
            V3 best = j3(c["top_points"][0]); float bd = 1e9f;
            for (auto& tp : c["top_points"]) { V3 q = j3(tp); float d = len(q - B.ctl[0]); if (d < bd) { bd = d; best = q; } }
            std::vector<V3> ctl{best}; std::vector<float> wid{B.wid[0]};
            for (size_t k = 0; k < B.ctl.size(); k++) if (len(B.ctl[k] - best) > blend) { ctl.push_back(B.ctl[k]); wid.push_back(B.wid[k]); }
            if (ctl.size() >= 3) { B.ctl = ctl; B.wid = wid; }
        }
    }
    // ---------------- parameters
    const float period = std::max(c.value("period_mm", 9.0f), 2.0f) / 1000;      // full plait period (a lobe returns to its side)
    const float widthScale = c.value("width_scale", 1.0f);
    const float widthMin = c.value("width_min_mm", 2.5f) / 1000, widthMax = c.value("width_max_mm", 14.0f) / 1000;
    const float taper = std::clamp(c.value("taper", 0.35f), 0.0f, 1.0f);           // width at the tie = (1 - taper) x
    const float topTaperM = c.value("top_taper_mm", 6.0f) / 1000;                     // lobes gather toward the root over this
    const float lobeFrac = std::clamp(c.value("lobe", 0.3f), 0.12f, 0.45f);         // lobe radius / braid width
    const float tight = std::clamp(c.value("tightness", 0.8f), 0.0f, 1.5f);         // over / under depth (x lobe radius)
    const float twist = c.value("twist_deg", 0.0f) * 3.14159265f / 180;               // phase offset
    const float spin = c.value("spin_per_mm", 0.0f);                                  // extra rotation of the plait plane (deg / mm)
    const int perLobe = std::clamp(c.value("guides_per_lobe", 3), 3, 8);
    const float ring = std::clamp(c.value("ring", 0.6f), 0.1f, 1.0f);              // guide ring radius / lobe radius
    const float cvPerPeriod = std::max(3.0f, c.value("cvs_per_period", 6.0f));
    float pieceM = c.value("piece_mm", 0.0f) / 1000;
    if (pieceM <= 0) pieceM = (K - 1) * period / cvPerPeriod;
    const float overlap = std::clamp(c.value("overlap", 2.0f), 1.0f, 6.0f);         // pieces covering each point of a lobe
    const float tieM = c.value("tie_mm", 3.0f) / 1000, tieW = c.value("tie_width", 0.55f);
    const float tasselScale = c.value("tassel_scale", 1.0f), tasselSpread = c.value("tassel_spread", 1.6f);
    const int tasselGuides = std::clamp(c.value("tassel_guides", 6), 0, 16);
    const float stiff = std::clamp(c.value("stiffness", 1.0f), 0.0f, 1.0f);
    const bool dropCards = c.value("drop_cards", from == "kept" || from == "card");
    const bool dry = c.value("list_only", false);
    if (dry) { r["braids"] = info; r["piece_mm"] = std::round(pieceM * 10000) / 10; r["cvs"] = K; return r; }
    // ---------------- base guide (skinning) + uv
    auto nearestAlive = [&](V3 q) {
        int best = -1; float bd = 1e9f;
        for (size_t i = 0; i < gr.P.size(); i++) if (gr.alive[i]) { float d = len(gr.P[i][0] - q); if (d < bd) { bd = d; best = (int)i; } }
        return best;
    };
    auto uvOfGuide = [&](int g) -> std::array<float, 2> {
        for (auto& F : gr.faces) for (int q = 0; q < 3; q++) if (F.g[q] == g) return {F.uv[2 * q], F.uv[2 * q + 1]};
        return {0.5f, 0.5f};
    };
    json guides = json::array(), base = json::array(), uvs = json::array(), faces = json::array();
    json made = json::array();
    int nPieces = 0;
    for (size_t bi = 0; bi < src.size(); bi++) {
        CardBraid& B = src[bi];
        std::vector<float> w = B.wid;
        for (auto& x : w) x = std::clamp(x * widthScale, widthMin, widthMax);
        Path P = makePath(B.ctl, w, B.wAx);
        const float L = P.length();
        if (L < 0.01f) continue;
        int bg = c.contains("base_root") ? nearestAlive(j3(c["base_root"])) : nearestAlive(P.c[0]);
        if (bg < 0) { r["error"] = "no base guide"; return r; }
        V3 bp = gr.P[bg][0];
        json baseJ = {r4(bp.x), r4(bp.y), r4(bp.z)};
        std::array<float, 2> uv0 = c.contains("uv") ? std::array<float, 2>{c["uv"][0].get<float>(), c["uv"][1].get<float>()} : uvOfGuide(bg);
        json uvJ = {r4(uv0[0]), r4(uv0[1])};
        // width profile: card width x taper toward the tie, narrowed at the root (lobes gather into the beard) and in the tie
        auto Wd = [&](float s) {
            float x = P.w[P.at(s)] * (1.0f - taper * std::clamp(s / L, 0.0f, 1.0f));
            if (topTaperM > 0 && s < topTaperM) x *= 0.55f + 0.45f * (s / topTaperM);
            float tieStart = L - tieM;
            if (s > tieStart) x *= 1.0f - (1.0f - tieW) * std::clamp((s - tieStart) / std::max(tieM, 1e-4f), 0.0f, 1.0f);
            return x;
        };
        // lobe centre + its local frame at arc length s
        auto lobeAt = [&](int lobe, float s, V3& Uo, V3& Vo) {
            s = std::clamp(s, 0.0f, L);
            int i = P.at(s);
            float W = Wd(s), rl = W * lobeFrac;
            float aL = std::max(0.0f, W * 0.5f - rl), bL = rl * tight;
            float th = 2 * 3.14159265f * s / period + twist + 2 * 3.14159265f * lobe / 3.0f;
            float sp = spin * 3.14159265f / 180 * s * 1000;
            V3 U = P.U[i] * std::cos(sp) + P.V[i] * std::sin(sp), V = cross(P.T[i], U);
            Uo = U; Vo = V;
            return P.c[i] + U * (aL * std::sin(th)) + V * (bL * std::sin(2 * th));
        };
        auto lobeR = [&](float s) { return Wd(std::clamp(s, 0.0f, L)) * lobeFrac; };
        int nb = 0;
        // pieces along each lobe: starts every pieceM / overlap, the last ones end at the tie
        float step = pieceM / overlap;
        float pieceLen = std::min(pieceM, L);
        for (int lobe = 0; lobe < 3; lobe++) {
            // stagger the lobes' piece starts so piece ends do not line up across the braid
            float s0 = -step * (lobe / 3.0f);
            for (float st = s0; st < L - 0.0015f; st += step) {
                float a0 = std::max(0.0f, st), a1 = std::min(L, st + pieceLen);
                if (a1 - a0 < pieceLen * 0.35f && st > 0) continue;   // a stub at the end: the previous piece covers it
                if (a1 - a0 < 0.002f) continue;
                int first = int(guides.size());
                for (int gi = 0; gi < perLobe; gi++) {
                    float ang = 2 * 3.14159265f * gi / perLobe + lobe * 0.7f;
                    json pts = json::array();
                    for (int k = 0; k < K; k++) {
                        float s = a0 + (a1 - a0) * k / (K - 1);
                        V3 U, V;
                        V3 q = lobeAt(lobe, s, U, V);
                        float rr = lobeR(s) * ring;
                        q = q + U * (rr * std::cos(ang)) + V * (rr * std::sin(ang));
                        pts.push_back({r4(q.x), r4(q.y), r4(q.z)});
                    }
                    guides.push_back(pts); base.push_back(baseJ); uvs.push_back(uvJ);
                }
                // growth faces only inside this lobe piece: a fan over its ring (perLobe 3 = one triangle)
                for (int gi = 1; gi + 1 < perLobe; gi++) faces.push_back({first, first + gi, first + gi + 1});
                nPieces++; nb++;
            }
        }
        // tassel: one brush below the tie (from the tie centre, spreading)
        float tl = B.tasselLen * tasselScale;
        if (c.contains("tassel_mm")) tl = c["tassel_mm"].get<float>() / 1000;
        int nt = 0;
        if (tl > 0.002f && tasselGuides >= 3) {
            V3 Tt = P.T.back(), Ut = P.U.back(), Vt = P.V.back();
            V3 o = P.c.back();
            float r0 = Wd(L) * 0.5f;
            int first = int(guides.size());
            int ringN = tasselGuides;
            json ctr = json::array();
            for (int k = 0; k < K; k++) {
                float s = tl * k / (K - 1);
                V3 q = o + Tt * (s - tieM * 0.5f);
                ctr.push_back({r4(q.x), r4(q.y), r4(q.z)});
            }
            guides.push_back(ctr); base.push_back(baseJ); uvs.push_back(uvJ);
            for (int gi = 0; gi < ringN; gi++) {
                float ang = 2 * 3.14159265f * gi / ringN;
                json pts = json::array();
                for (int k = 0; k < K; k++) {
                    float f = float(k) / (K - 1);
                    float s = tl * f;
                    float rr = r0 * (0.6f + (tasselSpread - 0.6f) * f * f);
                    V3 q = o + Tt * (s - tieM * 0.5f) + Ut * (rr * std::cos(ang)) + Vt * (rr * std::sin(ang) * 0.8f);
                    pts.push_back({r4(q.x), r4(q.y), r4(q.z)});
                }
                guides.push_back(pts); base.push_back(baseJ); uvs.push_back(uvJ);
            }
            for (int gi = 0; gi < ringN; gi++) faces.push_back({first, first + 1 + gi, first + 1 + (gi + 1) % ringN});
            nt = ringN + 1;
        }
        made.push_back({{"length_mm", std::round(L * 10000) / 10}, {"pieces", nb}, {"tassel_guides", nt}, {"tassel_mm", std::round(tl * 10000) / 10},
                        {"top", v3j(P.c.front())}, {"tie", v3j(P.c.back())}, {"base_guide_root", baseJ},
                        {"width_mm_top_mid_tie", {std::round(Wd(topTaperM) * 10000) / 10, std::round(Wd(L * 0.5f) * 10000) / 10, std::round(Wd(L) * 10000) / 10}}});
    }
    if (guides.empty()) { r["error"] = "nothing generated"; return r; }
    int id = 0;
    for (auto& o : gr.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    int grp = 1000 + (++grpId);
    json params = c;
    params.erase("do");
    json op = {{"op", "rebuild"}, {"delete", json::array()}, {"guides", guides}, {"base", base}, {"uv", uvs}, {"faces", faces},
               {"id", ++id}, {"grp", grp}, {"rebuild", {{"kind", "braid"}, {"params", params}, {"studio_version", GSD_VERSION}}}};
    size_t n0 = gr.P.size();
    gr.ops.push_back(op);
    gr.redo.clear();
    gr.replay();
    editsChanged();
    smap.ensure(gr.P.size());
    for (size_t g = n0; g < gr.P.size(); g++) { smap.guideS[g] = stiff; smap.guideR[g] = stiff; }
    stiffDirty = true;   // rasterise() decides painted (all 1 = the fur's constant stiffness)
    int dropped = 0;
    if (dropCards) {
        for (auto& B : src)
            for (auto& [mi, ti] : B.tris) {
                auto it = keepCards.find(mi);
                if (it != keepCards.end() && ti < int(it->second.size()) && it->second[ti]) { it->second[ti] = 0; dropped++; }
            }
        for (auto it = keepCards.begin(); it != keepCards.end();) {
            bool any = false;
            for (auto b : it->second) any = any || b;
            it = any ? std::next(it) : keepCards.erase(it);
        }
    }
    r["braids"] = made;
    r["cards"] = info;
    r["guide_index_range"] = {int(n0), int(gr.P.size())};   // e.g. studio_face_texel where {indices} = lighter / denser texel
    r["new_guides"] = guides.size();
    r["new_faces"] = faces.size();
    r["pieces"] = nPieces;
    r["piece_mm"] = std::round(pieceM * 10000) / 10;
    r["cvs"] = K;
    r["stiffness_map"] = stiff;
    r["kept_card_triangles_dropped"] = dropped;
    return r;
}
