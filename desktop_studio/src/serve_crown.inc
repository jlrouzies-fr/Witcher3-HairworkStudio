// crown_swirl (2026-10-16, opt-in, Studio-only tool; plain reshape ops): combs the crown guides into one coherent flow
// out of a crown point. Each guide keeps its root; its first swirl_mm leave the root in the swirl direction (away from
// the crown, turned by spiral_deg) lying lift_mm over the skull (rise slope-limited), then the strand rejoins its own
// old path over rejoin_mm (the shape further down is kept). Fringe / face-side guides (tip in front of the eyes' plane
// minus face_back_mm, or any point within eye_mm of the eye box) and 'protect' are left alone. One undo group.
static json crownSwirl(App& a, const json& c, int& grpId) {
    using namespace t6;
    json r;
    Groom& gr = a.groom;
    const V3 hc = a.pack.headC;
    Zones z = zonesOf(a, c);
    HeadRadial hr = headRadial(a);
    if (!hr.ok) { r["error"] = "no head mesh"; return r; }
    const float PI = 3.14159265f;
    // crown point: given, or on the skull crown_back_deg behind straight up
    V3 crown;
    if (c.contains("crown") && c["crown"].is_array() && c["crown"].size() == 3)
        crown = {c["crown"][0].get<float>(), c["crown"][1].get<float>(), c["crown"][2].get<float>()};
    else {
        float ang = c.value("crown_back_deg", 30.0f) * PI / 180;
        V3 d = nrmz(V3{0, -z.fwd * std::sin(ang), std::cos(ang)});
        float s = hr.surface(d);
        crown = hc + d * (s > 0 ? s : 0.1f);
    }
    float radius = c.value("radius_mm", 50.0f) / 1000, swirlMm = c.value("swirl_mm", 35.0f) / 1000, rejoinMm = c.value("rejoin_mm", 35.0f) / 1000;
    float spiral = c.value("spiral_deg", 20.0f) * PI / 180, lift = c.value("lift_mm", 4.0f) / 1000, slope = c.value("max_slope", 0.4f);
    float faceBack = c.value("face_back_mm", 30.0f) / 1000, eyeMm = c.value("eye_mm", 30.0f) / 1000;
    float st = std::clamp(c.value("strength", 1.0f), 0.0f, 1.0f);
    bool dry = c.value("list_only", false);
    std::set<int> prot;
    if (c.contains("protect")) for (int g : a.selectGuides(c["protect"])) prot.insert(g);
    auto faceSide = [&](int g) {
        auto& G = gr.P[g];
        if (!z.haveEye) return false;
        if (z.fwd * (G.back().y - z.eyeY) > -faceBack) return true;   // tip in front of the face plane
        for (auto& p : G) {
            float dx = std::max({z.eLo.x - p.x, 0.0f, p.x - z.eHi.x}), dy = std::max({z.eLo.y - p.y, 0.0f, p.y - z.eHi.y}),
                  dz = std::max({z.eLo.z - p.z, 0.0f, p.z - z.eHi.z});
            if (std::sqrt(dx * dx + dy * dy + dz * dz) < eyeMm) return true;
        }
        return false;
    };
    std::vector<int> ix = a.selectGuides(c.value("where", json::object({{"all", true}})));
    int id = 0, grp = 1000 + (++grpId), n = 0, nFace = 0, nProt = 0;
    for (auto& o : gr.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    std::vector<json> ops;
    double moved = 0; int movedN = 0;
    auto swirlDir = [&](V3 p) {
        V3 nn = nrmz(p - hc);
        V3 v = p - crown; v = v - nn * dot(v, nn);
        if (len(v) < 1e-5f) return V3{0, 0, 0};
        V3 u = nrmz(v), t = cross(nn, u);
        return nrmz(u * std::cos(spiral) + t * std::sin(spiral));
    };
    if (c.value("mode", std::string("swirl")) == "untwist") {
        // untwist (2026-10-16): each crown guide's shape (points relative to its root, resampled at the same arc
        // fractions) becomes the gaussian mean of the shapes of the crown guides rooted within nb_mm - shapes turn into a
        // smooth function of the root position, so neighbouring roots stay side by side all along (no crossing /
        // twisting ropes). Face-side guides neither change nor steer. passes repeat it. Lengths: each guide keeps its own.
        float nbMm = c.value("nb_mm", 18.0f) / 1000;
        int passes = std::max(1, c.value("passes", 3));
        std::vector<int> sel;
        for (int g : ix) {
            if (g < 0 || g >= (int)gr.P.size() || !gr.alive[g] || gr.P[g].size() < 3) continue;
            if (len(gr.P[g][0] - crown) > radius) continue;
            if (prot.count(g)) { nProt++; continue; }
            if (faceSide(g)) { nFace++; continue; }
            sel.push_back(g);
        }
        const int M = 24;
        auto resample = [&](const Guide& G) {   // M points at even arc fractions, relative to the root, normalised by length
            auto sl = arcLen(G); float L = std::max(sl.back(), 1e-4f);
            std::vector<V3> o(M);
            size_t k = 1;
            for (int i = 0; i < M; i++) {
                float s = L * i / (M - 1);
                while (k + 1 < G.size() && sl[k] < s) k++;
                float f = std::clamp((s - sl[k - 1]) / std::max(sl[k] - sl[k - 1], 1e-6f), 0.0f, 1.0f);
                o[i] = (G[k - 1] + (G[k] - G[k - 1]) * f - G[0]) * (1.0f / L);
            }
            return o;
        };
        std::map<int, Guide> cur;
        for (int g : sel) cur[g] = gr.P[g];
        float sig = std::max(nbMm * 0.5f, 1e-3f);
        for (int ps = 0; ps < passes; ps++) {
            std::map<int, std::vector<V3>> shp;
            for (int g : sel) shp[g] = resample(cur[g]);
            std::map<int, Guide> nxt;
            for (int g : sel) {
                std::vector<V3> m(M, V3{0, 0, 0}); float ws = 0;
                for (int h : sel) {
                    float d = len(gr.P[h][0] - gr.P[g][0]);
                    if (d > nbMm) continue;
                    float w = std::exp(-0.5f * (d / sig) * (d / sig));
                    for (int i = 0; i < M; i++) m[i] = m[i] + shp[h][i] * w;
                    ws += w;
                }
                const Guide& G = cur[g];
                auto sl = arcLen(G); float L = sl.back();
                Guide R2 = G;
                // mean shape at this guide's own length, then walked with its own segment lengths
                std::vector<V3> P(M);
                for (int i = 0; i < M; i++) P[i] = G[0] + m[i] * (L / ws);
                std::vector<float> ps2(M, 0);
                for (int i = 1; i < M; i++) ps2[i] = ps2[i - 1] + len(P[i] - P[i - 1]);
                float L2 = std::max(ps2.back(), 1e-4f);
                size_t j = 1;
                for (size_t k = 1; k < G.size(); k++) {
                    float s = sl[k] / L * L2;
                    while (j + 1 < (size_t)M && ps2[j] < s) j++;
                    float f = std::clamp((s - ps2[j - 1]) / std::max(ps2[j] - ps2[j - 1], 1e-6f), 0.0f, 1.0f);
                    R2[k] = P[j - 1] + (P[j] - P[j - 1]) * f;
                }
                for (size_t k = 1; k < G.size(); k++) R2[k] = R2[k - 1] + nrmz(R2[k] - R2[k - 1]) * (sl[k] - sl[k - 1]);
                pushOut(hr, R2, 0.003f);
                nxt[g] = R2;
            }
            cur = nxt;
        }
        for (int g : sel) {
            const Guide& G = gr.P[g];
            Guide R2 = cur[g];
            if (st < 1) {
                auto sl = arcLen(G);
                Guide B = R2;
                for (size_t k = 1; k < G.size(); k++) B[k] = G[k] + (R2[k] - G[k]) * st;
                for (size_t k = 1; k < G.size(); k++) R2[k] = R2[k - 1] + nrmz(B[k] - R2[k - 1]) * (sl[k] - sl[k - 1]);
            }
            for (size_t k = 1; k < G.size(); k++) { moved += len(R2[k] - G[k]); movedN++; }
            ops.push_back({{"op", "reshape"}, {"root", {r4(G[0].x), r4(G[0].y), r4(G[0].z)}}, {"points", ptsJson(R2)}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        ix.clear();   // skip the swirl loop
    }
    for (int g : ix) {
        if (g < 0 || g >= (int)gr.P.size() || !gr.alive[g] || gr.P[g].size() < 3) continue;
        const Guide& G = gr.P[g];
        if (len(G[0] - crown) > radius) continue;
        if (prot.count(g)) { nProt++; continue; }
        if (faceSide(g)) { nFace++; continue; }
        size_t K = G.size();
        auto sl = arcLen(G);
        Guide R2 = G;
        size_t kS = K - 1;
        for (size_t k = 1; k < K; k++) {
            float seg = sl[k] - sl[k - 1];
            V3 prev = R2[k - 1];
            V3 nn = nrmz(prev - hc);
            float w = std::clamp(sl[k - 1] / std::max(swirlMm, 1e-3f), 0.0f, 1.0f);
            w = w * w * (3 - 2 * w);
            V3 dO = nrmz(G[k] - G[k - 1]);
            V3 dOt = dO - nn * dot(dO, nn);
            V3 sw = swirlDir(prev);
            if (len(sw) < 1e-6f) sw = len(dOt) > 1e-6f ? nrmz(dOt) : dO;
            V3 tdir = len(dOt) > 1e-6f ? nrmz(sw * (1 - w) + nrmz(dOt) * w) : sw;
            V3 p = prev + tdir * seg;
            float rp = len(prev - hc);
            for (int it = 0; it < 2; it++) {   // lift_mm over the skull, slope-limited
                V3 d = nrmz(p - hc);
                float s = hr.surface(d);
                if (s <= 0) break;
                float dr = std::clamp(s + lift - rp, -slope * seg, slope * seg);
                p = hc + d * (rp + dr);
                p = prev + nrmz(p - prev) * seg;
            }
            R2[k] = p;
            if (sl[k] >= swirlMm) { kS = k; break; }
        }
        // rejoin the old path: the offset at the end of the swirl part fades over rejoin_mm; constrained re-walk
        V3 off = R2[kS] - G[kS];
        for (size_t k = kS + 1; k < K; k++) {
            float f = std::clamp(1 - (sl[k] - sl[kS]) / std::max(rejoinMm, 1e-3f), 0.0f, 1.0f);
            R2[k] = G[k] + off * f;
        }
        for (size_t k = 1; k < K; k++) R2[k] = R2[k - 1] + nrmz(R2[k] - R2[k - 1]) * (sl[k] - sl[k - 1]);
        if (st < 1) {
            Guide B = R2;
            for (size_t k = 1; k < K; k++) B[k] = G[k] + (R2[k] - G[k]) * st;
            for (size_t k = 1; k < K; k++) R2[k] = R2[k - 1] + nrmz(B[k] - R2[k - 1]) * (sl[k] - sl[k - 1]);
        }
        pushOut(hr, R2, 0.003f);
        for (size_t k = 1; k < K; k++) { moved += len(R2[k] - G[k]); movedN++; }
        V3 r0 = G[0];
        ops.push_back({{"op", "reshape"}, {"root", {r4(r0.x), r4(r0.y), r4(r0.z)}}, {"points", ptsJson(R2)}, {"id", ++id}, {"grp", grp}});
        n++;
    }
    r["crown"] = {r4(crown.x), r4(crown.y), r4(crown.z)};
    r["changed"] = n; r["face_side_kept"] = nFace; r["protected"] = nProt;
    r["mean_point_move_mm"] = movedN ? std::round(moved / movedN * 10000) / 10 : 0;
    if (dry) return r;
    for (auto& o : ops) gr.ops.push_back(o);
    gr.redo.clear(); gr.replay(); a.editsChanged();
    return r;
}
