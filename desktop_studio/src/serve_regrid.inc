// regrid (2026-10-16, tool architect; opt-in, Studio-only, one undoable `rebuild` op): re-grid a region of the groom
// from a smooth flow field. Why: Shani's crown ropes were measured to be groups of neighbouring guides bundling and
// diverging (crown direction scatter 22.7 deg median vs 12-16 in pipeline packs), on faces of wildly uneven area
// (0.9-49 mm2) that connect guides going different ways; the in-between hair of such faces bundles, fans or dives
// into the skull. Local shape smoothing (untwist, 18 mm) keeps the 20-30 mm locks, depth tools keep the directions.
// What it does, for the guides in `where` (minus `protect`, fringe / face-side guides and guides shorter than min_len):
//   1. new roots: Poisson-disk samples on the painted scalp within reach_mm of the old roots of the region, at
//      spacing_mm (default: the spacing that keeps the region's guide count x count_scale), sunk to the groom's own
//      root depth under the mesh (the packs put roots about 6 mm under the head mesh);
//   2. new shapes: the Gaussian-weighted mean (sigma flow_mm / 2, neighbours within flow_mm) of the OLD guides' shapes
//      (points relative to the root, normalised by length, resampled at even arc fractions); neighbours whose root
//      direction opposes the nearest old guide's do not vote (partings / whorls keep their two combs); length = the
//      weighted mean length; re-walked with K even segments and pushed out of the head;
//   3. new faces: Delaunay of the new roots + the kept roots around the region in the polar map around the head
//      centre (as rebuild_from_cards), old-only triangles skipped, edge / circumradius limits, no triangle over a kept
//      old face, and no face whose guides go opposite ways at the root (the blend of such a pair lies inside the skull);
//   4. uv / base of each new guide from the nearest old guide.
// list_only reports the plan (counts, spacing) without changing anything.
// roots on a hash grid (cell = radius): neighbours within r
template <class T> struct RootGridT {
    float cell;
    std::unordered_map<long long, std::vector<std::pair<V3, T>>> m;
    explicit RootGridT(float r) : cell(std::max(r, 1e-3f)) {}
    static long long key(int x, int y, int z) { return ((long long)(x + 4096) << 26) | ((long long)(y + 4096) << 13) | (long long)(z + 4096); }
    void add(V3 p, T v) { m[key(int(std::floor(p.x / cell)), int(std::floor(p.y / cell)), int(std::floor(p.z / cell)))].push_back({p, v}); }
    template <class F> void within(V3 p, float r, F f) const {
        int cx = int(std::floor(p.x / cell)), cy = int(std::floor(p.y / cell)), cz = int(std::floor(p.z / cell));
        int R = std::max(1, int(std::ceil(r / cell)));
        for (int dx = -R; dx <= R; dx++) for (int dy = -R; dy <= R; dy++) for (int dz = -R; dz <= R; dz++) {
            auto it = m.find(key(cx + dx, cy + dy, cz + dz));
            if (it == m.end()) continue;
            for (auto& [q, v] : it->second) { float d = len(q - p); if (d <= r) f(v, d); }
        }
    }
};

static json regridRegion(App& a, const json& c, int& grpId) {
    using namespace t6;
    using namespace fchk;
    json r;
    Groom& gr = a.groom;
    const int K = gr.K;
    const V3 hc = a.pack.headC;
    const float PI = 3.14159265f;
    HeadRadial hr = headRadial(a);
    if (!hr.ok) { r["error"] = "no head mesh"; return r; }
    Zones z = zonesOf(a, c);
    bool dry = c.value("list_only", false);
    float flowMm = c.value("flow_mm", 30.0f) / 1000, reachMm = c.value("reach_mm", 10.0f) / 1000;
    float minLen = c.value("min_len", 0.03f), countScale = c.value("count_scale", 1.0f);
    float faceBack = c.value("face_back_mm", 30.0f) / 1000, eyeMm = c.value("eye_mm", 30.0f) / 1000;
    bool exFace = c.value("exclude_face", true);
    float oppose = c.value("oppose_dot", -0.2f);   // a face is rejected when two of its guides' root directions have dot < this
    float lenScale = c.value("length_scale", 1.0f);
    // ---- region
    std::vector<int> ix = a.selectGuides(c.value("where", json::object({{"z_min", -0.06}})));
    std::set<int> prot;
    if (c.contains("protect")) for (int g : a.selectGuides(c["protect"])) prot.insert(g);
    auto faceSide = [&](int g) {
        if (!z.haveEye) return false;
        auto& G = gr.P[g];
        if (z.fwd * (G.back().y - z.eyeY) > -faceBack) return true;
        for (auto& p : G) {
            float dx = std::max({z.eLo.x - p.x, 0.0f, p.x - z.eHi.x}), dy = std::max({z.eLo.y - p.y, 0.0f, p.y - z.eHi.y}),
                  dz = std::max({z.eLo.z - p.z, 0.0f, p.z - z.eHi.z});
            if (std::sqrt(dx * dx + dy * dy + dz * dz) < eyeMm) return true;
        }
        return false;
    };
    std::vector<int> gone;
    int nProt = 0, nFace = 0, nShort = 0, nCross = 0;
    bool keepCross = c.value("keep_cross", false);
    float crossDot = c.value("cross_dot", 0.0f);
    std::vector<int> cand0;
    for (int g : ix) {
        if (g < 0 || g >= (int)gr.P.size() || !gr.alive[g] || gr.P[g].size() < 3) continue;
        if (prot.count(g)) { nProt++; continue; }
        if (exFace && faceSide(g)) { nFace++; continue; }
        if (guideLen(gr.P[g]) < minLen) { nShort++; continue; }
        cand0.push_back(g);
    }
    if (keepCross) {
        // the minority comb (guides heading against the weighted consensus of the guides rooted within flow_mm, drawn
        // frame, at dir_frac of the length) is the crossing layer of a layered style (Shani: the sweep over the ear
        // lies across the side hair); a single flow field cannot hold two combs at one point, so those guides stay
        const int Mq = 24; float dfr = std::clamp(c.value("dir_frac", 0.3f), 0.05f, 0.95f);
        std::vector<V3> dd(gr.P.size(), V3{0, 0, 0});
        auto drawnG = [&](int g) { Guide D = gr.P[g]; for (int k = 0; k < (int)D.size(); k++) D[k] = gr.renderPoint(g, k); return D; };
        for (size_t g = 0; g < gr.P.size(); g++) {
            if (!gr.alive[g] || gr.P[g].size() < 2) continue;
            Guide D = drawnG((int)g); auto sl = arcLen(D); float L = std::max(sl.back(), 1e-4f), t = dfr * L;
            size_t k = 1; while (k + 1 < D.size() && sl[k] < t) k++;
            dd[g] = nrmz(D[k] - D[k - 1]);
        }
        RootGridT<int> ag(flowMm);
        for (size_t g = 0; g < gr.P.size(); g++) if (gr.alive[g] && gr.P[g].size() >= 2) ag.add(gr.P[g][0], (int)g);
        float sg2 = flowMm * 0.5f;
        for (int g : cand0) {
            V3 m{0, 0, 0}; std::vector<std::pair<int, float>> nb;
            ag.within(gr.P[g][0], flowMm, [&](int h, float d) { nb.push_back({h, std::exp(-0.5f * (d / sg2) * (d / sg2))}); });
            for (auto& [h, w] : nb) m = m + dd[h] * w;
            for (int it = 0; it < 2 && len(m) > 1e-9f; it++) { V3 mm = nrmz(m); m = V3{0, 0, 0}; for (auto& [h, w] : nb) if (dot(dd[h], mm) > 0) m = m + dd[h] * w; }
            if (len(m) > 1e-9f && dot(dd[g], nrmz(m)) < crossDot) { nCross++; continue; }
            gone.push_back(g);
        }
    } else gone = cand0;
    if (gone.size() < 4) { r["error"] = "fewer than 4 guides in the region (where / protect / exclude_face left " + std::to_string(gone.size()) + ")"; return r; }
    std::set<int> goneS(gone.begin(), gone.end());
    std::vector<int> stay;
    for (size_t g = 0; g < gr.P.size(); g++) if (gr.alive[g] && !goneS.count((int)g) && gr.P[g].size() >= 2) stay.push_back((int)g);
    // ---- scalp samples (posed head mesh, painted flag) within reach of the old roots; root depth under the mesh
    std::vector<V3> pos; std::vector<float> fl; std::vector<std::array<int, 3>> tr;
    if (!a.scalpMesh(pos, fl, tr)) { r["error"] = "no scalp mesh"; return r; }
    SkinField S = buildSkin(a, 0.004f, 0.025f);
    float rootOff = 0;
    {
        std::vector<float> rd;
        // measured on the DRAWN roots (edit root + the base's root offset): the edit roots sit on the mesh (0 mm) while the
        // drawn ones are about 6 mm under it on Shani; the new roots are designed in the drawn frame, so they must take
        // the drawn depth (first run: root_depth 0 put every new root 5.4 mm above the old ones = a raised crown layer)
        for (size_t g = 0; g < gr.P.size(); g++) if (gr.alive[g] && !gr.P[g].empty()) { float v = S.at(gr.renderPoint((int)g, 0)); if (v < SkinField::BIG * 0.5f) rd.push_back(v); }
        std::sort(rd.begin(), rd.end());
        if (!rd.empty()) rootOff = rd[rd.size() / 2];   // negative = under the mesh
    }
    RootGridT<int> og(0.02f);   // old roots of the region on a hash grid
    for (int g : gone) og.add(gr.P[g][0], g);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    std::vector<V3> cand, candN;
    double regionArea = 0;
    for (auto& t : tr) {
        bool painted = fl[t[0]] > 0.5f || fl[t[1]] > 0.5f || fl[t[2]] > 0.5f;
        if (!painted && !c.value("any_skin", false)) continue;
        V3 A = pos[t[0]], B = pos[t[1]], Cc = pos[t[2]];
        V3 n = cross(B - A, Cc - A);
        float area = 0.5f * len(n);
        if (area < 1e-9f) continue;
        n = n * (1.0f / len(n));
        if (dot(n, (A + B + Cc) * (1.0f / 3) - hc) < 0) n = n * -1.0f;   // outward
        int ns = std::max(1, int(area / 4e-6f));   // one sample per 4 mm2
        int inReach = 0;
        for (int s = 0; s < ns; s++) {
            float u = U(rng), v = U(rng);
            if (u + v > 1) { u = 1 - u; v = 1 - v; }
            V3 p = A + (B - A) * u + (Cc - A) * v;
            bool isNear = false;
            og.within(p, reachMm, [&](int, float) { isNear = true; });
            if (!isNear) continue;
            cand.push_back(p + n * rootOff); candN.push_back(n); inReach++;
        }
        regionArea += area * double(inReach) / ns;
    }
    if (cand.size() < 4) { r["error"] = "no painted scalp within reach_mm of the region's roots"; return r; }
    std::shuffle(cand.begin(), cand.end(), rng);
    // ---- spacing: keep the region's guide count unless given
    float spacing = c.value("spacing_mm", 0.0f) / 1000;
    if (spacing <= 0) spacing = std::sqrt(float(regionArea) / std::max(1.0f, float(gone.size()) * countScale) * 2.0f / std::sqrt(3.0f));
    spacing = std::clamp(spacing, 0.002f, 0.05f);
    auto poisson = [&](float sp) {
        std::vector<V3> out;
        RootGridT<int> pg(sp);
        for (auto& p : cand) {
            bool ok = true;
            pg.within(p, sp, [&](int, float) { ok = false; });
            if (!ok) continue;
            // keep clear of the kept roots too (no new root on top of a border guide)
            pg.add(p, (int)out.size()); out.push_back(p);
        }
        return out;
    };
    std::vector<V3> roots = poisson(spacing);
    for (int it = 0; it < 3 && !c.contains("spacing_mm"); it++) {   // count ~ 1 / spacing^2
        float want = float(gone.size()) * countScale;
        if (roots.empty() || std::fabs(float(roots.size()) - want) < 0.05f * want) break;
        spacing = std::clamp(spacing * std::sqrt(float(roots.size()) / want), 0.002f, 0.05f);
        roots = poisson(spacing);
    }
    // ---- shapes from the smooth flow field
    // Measured on Shani user_1523 (offline prototype on the drawn guides, 2026-10-16): the Gaussian mean of the voters'
    // POINTS is smooth across roots (new-guide direction scatter 5 deg median / 15 p90 at 25 % of the length) but it is
    // 27 % shorter than the mean length (diverging voters cancel), and re-walking it to the full length scrambled the
    // directions again (22 deg / 80 p90, as bad as the ropes). So the new strand is the integral of the weighted mean UNIT
    // TANGENT per arc fraction at the mean length (no cancellation, exact length): 3 deg median / 18 p90 at flow 30.
    // The vote gate is a consensus (weighted mean direction at dir_frac of the length, drawn frame, gate_iters rounds
    // dropping the voters heading against it) instead of "the nearest guide's edit-frame root direction": the edit-frame
    // root directions are scrambled by the per-CV offsets (19 deg median off the drawn ones), and the nearest guide is
    // often an outlier, so neighbouring new roots voted with different subsets.
    const int M = 24;
    float dirFrac = std::clamp(c.value("dir_frac", 0.3f), 0.05f, 0.95f);
    int gateIters = std::clamp(c.value("gate_iters", 2), 0, 5);
    auto resample = [&](const Guide& G) {   // M even-arc points of a drawn guide
        auto sl = arcLen(G); float L = std::max(sl.back(), 1e-4f);
        std::vector<V3> o(M); size_t k = 1;
        for (int q = 0; q < M; q++) {
            float t = L * q / (M - 1);
            while (k + 1 < G.size() && sl[k] < t) k++;
            float f = std::clamp((t - sl[k - 1]) / std::max(sl[k] - sl[k - 1], 1e-6f), 0.0f, 1.0f);
            o[q] = G[k - 1] + (G[k] - G[k - 1]) * f;
        }
        return o;
    };
    auto dirAt = [&](const std::vector<V3>& o, float frac) {   // unit tangent of a resampled guide at arc fraction frac
        int q = std::clamp(int(std::lround(frac * (M - 1))), 1, M - 1);
        return nrmz(o[q] - o[q - 1]);
    };
    // voters: the region's old guides plus the kept guides around it (continuity at the border)
    std::vector<int> voters = gone;
    RootGridT<int> vg(flowMm);
    for (int g : stay) { bool isNear = false; og.within(gr.P[g][0], flowMm, [&](int, float) { isNear = true; }); if (isNear) voters.push_back(g); }
    std::vector<std::vector<V3>> tang(gr.P.size());   // M-1 unit tangents at even arc steps (drawn frame)
    std::vector<std::vector<float>> hgt(gr.P.size());  // height over the skull surface at each of the M points (NaN below the skull)
    float hug = std::clamp(c.value("hug", 1.0f), 0.0f, 1.0f);
    int smoothPasses = std::clamp(c.value("smooth_passes", 2), 0, 20);
    auto skullH = [&](V3 p, float& h) {   // height of p over the skull along the head radial; false below the skull / no surface
        if (p.z < hc.z - 0.01f) return false;
        V3 d = p - hc; float rr = len(d); if (rr < 1e-5f) return false;
        float s = hr.surface(d * (1.0f / rr)); if (s <= 0) return false;
        h = rr - s; return true;
    };
    std::vector<float> lenOf(gr.P.size(), 0);
    std::vector<V3> rdir(gr.P.size(), V3{0, 0, 0});   // drawn-frame direction at dirFrac of the length, every alive guide
    // shapes are taken in the DRAWN frame (edit point + the base guide's fixed per-CV offset, Groom::renderPoint): the
    // Studio draws and exports edit points plus that offset, which on Shani is about -6.5 mm at the root and up to
    // 15 mm sideways on the first points, different per guide (Shani agent, 2026-10-16); shapes written in the edit
    // frame get scrambled near the root (bald parting, whorl holes). New points = drawn design - the base's offset.
    auto drawn = [&](int g) { Guide D = gr.P[g]; for (int k = 0; k < (int)D.size(); k++) D[k] = gr.renderPoint(g, k); return D; };
    for (size_t g = 0; g < gr.P.size(); g++) {
        if (!gr.alive[g] || gr.P[g].size() < 2) continue;
        Guide D = drawn((int)g);
        auto o = resample(D);
        rdir[g] = dirAt(o, dirFrac);
        lenOf[g] = guideLen(D);
    }
    // mode "field" (default): a SPATIAL flow field. The tangent at arc fraction t of a voter rooted elsewhere is not the
    // flow at the new root (Shani: guides rooted at the back crown arc UP over the top before falling forward over the
    // face; new temple roots averaging their 10-30 % tangents rose from the temple = standing tufts, and the temple's own
    // forward-over-the-ear sweep was lost: bare scalp above the ear). The field is the Gaussian mean of the unit tangents
    // of every voter SEGMENT near the current point (sigma flow_mm / 2), gated by the heading so far (segments heading
    // against it are the other comb and do not vote); each strand is traced from its root through that field at the
    // mean length of the voters rooted nearby, re-projected per step to the nearby segments' mean height over the skull.
    // mode "arc": the earlier per-arc-fraction mean (kept for comparison).
    std::string mode = c.value("mode", "field");
    struct Seg { int g; int q; V3 mid; V3 t; float h; };
    std::vector<Seg> segs;
    RootGridT<int> sg(flowMm);
    for (int g : voters) {
        auto o = resample(drawn(g));
        tang[g].resize(M - 1);
        for (int q = 0; q + 1 < M; q++) tang[g][q] = nrmz(o[q + 1] - o[q]);
        hgt[g].assign(M, std::numeric_limits<float>::quiet_NaN());
        for (int q = 0; q < M; q++) { float h; if (skullH(o[q], h)) hgt[g][q] = h; }
        vg.add(gr.P[g][0], g);
        for (int q = 0; q + 1 < M; q++) {
            Seg sgm{g, q, (o[q] + o[q + 1]) * 0.5f, tang[g][q], std::numeric_limits<float>::quiet_NaN()};
            float h; if (skullH(sgm.mid, h)) sgm.h = h;
            sg.add(sgm.mid, (int)segs.size()); segs.push_back(sgm);
        }
    }
    float sig = flowMm * 0.5f;
    std::vector<Guide> NG;
    std::vector<V3> ngDir;
    std::vector<int> ngBase;
    int noVote = 0, nOpp = 0;
    std::vector<int> nbg; std::vector<float> nbw;
    for (auto& rt : roots) {
        int nearest = -1; float bd = 1e9f;
        nbg.clear(); nbw.clear();
        vg.within(rt, flowMm, [&](int g, float d) {
            if (d < bd) { bd = d; nearest = g; }
            nbg.push_back(g); nbw.push_back(std::exp(-0.5f * (d / sig) * (d / sig)));
        });
        if (nearest < 0 || nbg.empty()) { noVote++; continue; }
        // consensus gate: the other comb of a parting / whorl does not vote
        std::vector<char> keep(nbg.size(), 1);
        for (int it = 0; it < gateIters; it++) {
            V3 m{0, 0, 0};
            for (size_t i = 0; i < nbg.size(); i++) if (keep[i]) m = m + rdir[nbg[i]] * nbw[i];
            if (len(m) < 1e-9f) break;
            m = nrmz(m);
            for (size_t i = 0; i < nbg.size(); i++) keep[i] = dot(rdir[nbg[i]], m) > 0.0f;
        }
        std::vector<V3> T(M - 1, V3{0, 0, 0}); double ws = 0, wl = 0;
        std::vector<double> H(M, 0), Hw(M, 0);
        for (size_t i = 0; i < nbg.size(); i++) {
            if (!keep[i]) continue;
            int g = nbg[i]; float w = nbw[i];
            for (int q = 0; q + 1 < M; q++) T[q] = T[q] + tang[g][q] * w;
            for (int q = 0; q < M; q++) if (hgt[g][q] == hgt[g][q]) { H[q] += w * hgt[g][q]; Hw[q] += w; }
            ws += w; wl += w * lenOf[g];
        }
        if (ws <= 1e-9) { noVote++; continue; }
        float L = float(wl / ws) * lenScale;
        std::vector<V3> P(M);
        P[0] = rt;
        // surface-following integration: the mean unit tangent of strands wrapping the skull in different planes is
        // straighter than any of them, so a free integral leaves the skull (measured on Shani: +3..+8 mm at p50 from CV3
        // on, +10 mm at p90 = a puffy ridge and tufts). Each step is re-projected along the head radial to the voters'
        // weighted mean height over the skull at that arc fraction (hug 1; 0 = free integral).
        V3 head{0, 0, 0};   // heading so far (field mode)
        if (mode == "field") {
            // the dominant comb of the SEGMENTS around the root (consensus rounds), not of the root directions: on
            // Shani's temple the roots nearby head back / down while the hair passing there (long sweep from the back
            // crown) heads forward over the ear; the root vote sent every new temple strand back and bared the ear
            std::vector<std::pair<int, float>> nearSeg;
            sg.within(rt, flowMm, [&](int si, float d) { nearSeg.push_back({si, std::exp(-0.5f * (d / sig) * (d / sig))}); });
            for (auto& [si, w] : nearSeg) head = head + segs[si].t * w;
            for (int it = 0; it < std::max(gateIters, 1) && len(head) > 1e-9f; it++) {
                V3 m = nrmz(head); head = V3{0, 0, 0};
                for (auto& [si, w] : nearSeg) if (dot(segs[si].t, m) > 0) head = head + segs[si].t * w;
            }
        }
        if (len(head) < 1e-9f) for (size_t i = 0; i < nbg.size(); i++) if (keep[i]) head = head + rdir[nbg[i]] * nbw[i];
        head = nrmz(head);
        for (int q = 1; q < M; q++) {
            float step = L / (M - 1);
            if (mode == "field") {
                V3 Tf{0, 0, 0}; double hs = 0, hw = 0, ws2 = 0;
                sg.within(P[q - 1], flowMm, [&](int si, float d) {
                    const Seg& S2 = segs[si];
                    float a = dot(S2.t, head);
                    if (a <= 0.0f) return;   // the other comb / hair coming back the other way
                    float w = std::exp(-0.5f * (d / sig) * (d / sig)) * a;
                    Tf = Tf + S2.t * w; ws2 += w;
                    if (S2.h == S2.h) { hs += w * S2.h; hw += w; }
                });
                V3 T1 = ws2 > 1e-9 ? nrmz(Tf) : head;
                P[q] = P[q - 1] + T1 * step;
                head = T1;
                float h0;
                if (hug > 0 && hw > 1e-9 && skullH(P[q], h0)) {
                    V3 d = P[q] - hc; float rr = len(d); V3 u = d * (1.0f / rr);
                    P[q] = hc + u * (rr + (float(hs / hw) - h0) * hug);
                }
            } else {
                P[q] = P[q - 1] + nrmz(T[q - 1]) * step;
                float h0;
                if (hug > 0 && Hw[q] > 1e-9 && skullH(P[q], h0)) {
                    float hm = float(H[q] / Hw[q]);
                    V3 d = P[q] - hc; float rr = len(d); V3 u = d * (1.0f / rr);
                    P[q] = hc + u * (rr + (hm - h0) * hug);
                }
            }
        }
        // the skull surface (median radius in a 5 deg cone) is piecewise constant, so the projected path jitters by a
        // few mm between steps: smooth the interior points (root and tip fixed), smooth_passes x 3-point average
        for (int it = 0; it < smoothPasses; it++) {
            std::vector<V3> Q = P;
            for (int q = 1; q + 1 < M; q++) Q[q] = (P[q - 1] + P[q] * 2.0f + P[q + 1]) * 0.25f;
            P = Q;
        }
        V3 nd = dirAt(P, dirFrac);
        // even K points along the integrated polyline (its arc length is exactly L)
        Guide G(K);
        G[0] = rt;
        for (int k = 1; k < K; k++) {
            float t = float(k) * (M - 1) / float(K - 1);
            int j = std::min(int(t), M - 2); float f = t - j;
            G[k] = P[j] + (P[j + 1] - P[j]) * f;
        }
        pushOut(hr, G, 0.003f);
        // back to the edit frame of the base guide (the rebuild op stores edit points; the base's offset is re-added when drawn)
        for (int k = 0; k < K && k < (int)gr.P[nearest].size(); k++) G[k] = G[k] - (gr.renderPoint(nearest, k) - gr.P[nearest][k]);
        NG.push_back(G); ngDir.push_back(nd); ngBase.push_back(nearest);
    }
    if (NG.size() < 3) { r["error"] = "fewer than 3 new guides could be shaped"; return r; }
    // ---- faces: Delaunay of new roots + kept roots isNear the region (polar map), as rebuild_from_cards
    std::vector<V3> RR; std::vector<int> who;   // who >= 0 new index, < 0 -(old + 1)
    std::vector<int> border;
    for (int g : stay) { bool isNear = false; og.within(gr.P[g][0], std::max(reachMm, spacing) * 3, [&](int, float) { isNear = true; }); if (isNear) border.push_back(g); }
    for (int g : border) { RR.push_back(gr.P[g][0]); who.push_back(-(g + 1)); }
    for (size_t i = 0; i < NG.size(); i++) { RR.push_back(NG[i][0]); who.push_back((int)i); }
    std::vector<std::array<double, 2>> xy;
    for (auto& p : RR) {
        V3 d = p - hc; float rr = std::max(len(d), 1e-6f);
        double th = std::acos(std::clamp(d.z / rr, -1.0f, 1.0f)), ph = std::atan2(d.y, d.x);
        xy.push_back({th * std::cos(ph), th * std::sin(ph)});
    }
    float edgeAbs = std::max(c.value("edge_mm", 0.0f) / 1000, 3.0f * spacing), rmax = std::max(0.022f, 2.2f * spacing);
    float minArea = std::min(1e-5f, 0.05f * spacing * spacing);
    std::vector<std::array<std::array<double, 2>, 3>> keptOld;
    std::map<int, int> posOf;
    for (size_t i = 0; i < who.size(); i++) if (who[i] < 0) posOf[-who[i] - 1] = (int)i;
    for (auto& f : gr.faces) {
        if (goneS.count(f.g[0]) || goneS.count(f.g[1]) || goneS.count(f.g[2])) continue;
        if (!posOf.count(f.g[0]) || !posOf.count(f.g[1]) || !posOf.count(f.g[2])) continue;
        keptOld.push_back({xy[posOf[f.g[0]]], xy[posOf[f.g[1]]], xy[posOf[f.g[2]]]});
    }
    auto insideOld = [&](double x, double y) {
        for (auto& t : keptOld) {
            double d1 = (x - t[1][0]) * (t[0][1] - t[1][1]) - (t[0][0] - t[1][0]) * (y - t[1][1]);
            double d2 = (x - t[2][0]) * (t[1][1] - t[2][1]) - (t[1][0] - t[2][0]) * (y - t[2][1]);
            double d3 = (x - t[0][0]) * (t[2][1] - t[0][1]) - (t[2][0] - t[0][0]) * (y - t[0][1]);
            bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos_ = d1 > 0 || d2 > 0 || d3 > 0;
            if (!(neg && pos_)) return true;
        }
        return false;
    };
    auto dirOf = [&](int v) { return who[v] >= 0 ? ngDir[who[v]] : rdir[-who[v] - 1]; };   // drawn-frame directions at dirFrac, old and new
    json faces = json::array();
    std::vector<char> used(NG.size(), 0);
    int nLong = 0;
    for (auto& t : rebuild::delaunay(xy)) {
        if (who[t[0]] < 0 && who[t[1]] < 0 && who[t[2]] < 0) continue;
        V3 A = RR[t[0]], B = RR[t[1]], D = RR[t[2]];
        float la = len(B - D), lb = len(A - D), lc = len(A - B);
        float e = std::max({la, lb, lc}), ar = 0.5f * len(cross(B - A, D - A));
        if (e > edgeAbs || ar <= minArea || la * lb * lc / std::max(4 * ar, 1e-12f) > rmax) { nLong++; continue; }
        if (!keptOld.empty() && insideOld((xy[t[0]][0] + xy[t[1]][0] + xy[t[2]][0]) / 3, (xy[t[0]][1] + xy[t[1]][1] + xy[t[2]][1]) / 3)) continue;
        V3 d0 = dirOf(t[0]), d1 = dirOf(t[1]), d2 = dirOf(t[2]);
        if (dot(d0, d1) < oppose || dot(d1, d2) < oppose || dot(d0, d2) < oppose) { nOpp++; continue; }
        std::array<int, 3> q = t;
        if (dot(A - hc, cross(B - A, D - A)) < 0) std::swap(q[1], q[2]);
        json fj = json::array();
        for (int v : q) {
            if (who[v] >= 0) { fj.push_back(who[v]); used[who[v]] = 1; }
            else { V3 p = gr.P[-who[v] - 1][0]; fj.push_back({r4(p.x), r4(p.y), r4(p.z)}); }
        }
        faces.push_back(fj);
    }
    // uv of each old guide (any face corner)
    std::map<int, std::array<float, 2>> uvOf;
    for (auto& f : gr.faces) for (int cc = 0; cc < 3; cc++) if (!uvOf.count(f.g[cc])) uvOf[f.g[cc]] = {f.uv[2 * cc], f.uv[2 * cc + 1]};
    std::vector<int> remap(NG.size(), -1);
    json guides = json::array(), base = json::array(), uvs = json::array();
    int orphan = 0;
    for (size_t i = 0; i < NG.size(); i++) {
        if (!used[i]) { orphan++; continue; }
        remap[i] = (int)guides.size();
        guides.push_back(ptsJson(NG[i]));
        V3 bp = gr.P[ngBase[i]][0];
        base.push_back({r4(bp.x), r4(bp.y), r4(bp.z)});
        auto it = uvOf.find(ngBase[i]);
        std::array<float, 2> uv = it != uvOf.end() ? it->second : std::array<float, 2>{0.5f, 0.5f};
        uvs.push_back({r4(uv[0]), r4(uv[1])});
    }
    for (auto& f : faces) for (auto& v : f) if (v.is_number_integer()) v = remap[v.get<int>()];
    json del = json::array();
    for (int g : gone) del.push_back(rootJ(gr.P[g][0]));
    r["region_guides"] = gone.size(); r["kept_border_guides"] = border.size(); r["protected"] = nProt; r["cross_kept"] = nCross; r["face_side_kept"] = nFace; r["short_kept"] = nShort;
    r["scalp_samples"] = cand.size(); r["region_area_mm2"] = std::round(regionArea * 1e6); r["spacing_mm"] = std::round(spacing * 10000) / 10;
    r["root_depth_mm"] = std::round(rootOff * 10000) / 10; r["new_guides"] = guides.size(); r["new_faces"] = faces.size();
    r["rejected"] = {{"no_voters", noVote}, {"opposed_faces", nOpp}, {"long_or_sliver_faces", nLong}, {"no_face", orphan}};
    if (dry) return r;
    int id = 0, grp = 1000 + (++grpId);
    for (auto& o : gr.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    gr.ops.push_back(json{{"op", "rebuild"}, {"delete", del}, {"guides", guides}, {"base", base}, {"uv", uvs}, {"faces", faces}, {"id", ++id}, {"grp", grp}});
    gr.redo.clear(); gr.replay(); a.editsChanged();
    r["unmatched"] = gr.unmatched;
    return r;
}
