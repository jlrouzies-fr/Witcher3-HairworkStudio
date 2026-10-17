// check_faces (2026-10-16, tool architect; opt-in, read-only unless fix): growth-face validator.
// HairWorks draws the hair of a growth face as barycentric blends of its 3 guides (the SDK simulates and collides the
// GUIDES only), so a face whose guides go around different sides of the head / face / neck draws hair straight through
// the skin, although every guide is clean (proven on Corinne mcp_18 and Keira mcp_1 at rest; the same under wind).
// This tool measures that on the interpolated hair itself:
//   - rest: barycentric hairs of every face (edit-frame guides posed) against a signed-distance field of the posed head
//     mesh (skull, face, neck): samples deeper than depth_mm inside the skin are penetrations. The 3 guides are tested
//     too, so "interp_only" faces are the ones only the in-between hair spoils.
//   - wind: the same on the simulated guides read back from the GPU (fresh simulation, wind_strength, each heading of
//     wind_yaw_deg, wind_s seconds, every 10th step after the first third).
//   - geometry: sliver faces (root triangle < sliver_mm2: HairWorks grows the same hair count on every face, so a sliver
//     renders as a dense bundle), big faces (> big_x x median area: sparse hair, scalp speckle), divergent faces (mid /
//     tip spread vs root spread, direction angle between the guides).
// fix: true deletes the faces named in fix_what (default ["penetration"]: rest + wind penetrations that are interp-only;
// "guides" as well = faces whose guides penetrate too; "sliver", "big", "divergent") with delete_face ops in one undo
// group. Deleting a face removes its in-between hair (coverage can open): check scalp coverage afterwards.
namespace fchk {

// signed distance to the posed head mesh on a voxel grid (cell m, computed within reach of the surface; +big outside
// the band = "far outside"; the sign comes from the angle-weighted pseudo-normal of the closest feature)
struct SkinField {
    V3 lo{}, hi{};
    float cell = 0.004f;
    int nx = 0, ny = 0, nz = 0;
    std::vector<float> d;
    bool ok = false;
    static constexpr float BIG = 1e9f;
    float at(V3 p) const {
        if (!ok) return BIG;
        float fx = (p.x - lo.x) / cell, fy = (p.y - lo.y) / cell, fz = (p.z - lo.z) / cell;
        int ix = int(std::floor(fx)), iy = int(std::floor(fy)), iz = int(std::floor(fz));
        if (ix < 0 || iy < 0 || iz < 0 || ix + 1 >= nx || iy + 1 >= ny || iz + 1 >= nz) return BIG;
        float tx = fx - ix, ty = fy - iy, tz = fz - iz;
        auto v = [&](int a, int b, int c) { return d[(size_t(c) * ny + b) * nx + a]; };
        float c00 = v(ix, iy, iz), c10 = v(ix + 1, iy, iz), c01 = v(ix, iy + 1, iz), c11 = v(ix + 1, iy + 1, iz);
        float d00 = v(ix, iy, iz + 1), d10 = v(ix + 1, iy, iz + 1), d01 = v(ix, iy + 1, iz + 1), d11 = v(ix + 1, iy + 1, iz + 1);
        float m = std::max({c00, c10, c01, c11, d00, d10, d01, d11});
        if (m >= BIG * 0.5f) {   // a corner outside the band: nearest sample instead of the blend
            int ax = tx < 0.5f ? ix : ix + 1, ay = ty < 0.5f ? iy : iy + 1, az = tz < 0.5f ? iz : iz + 1;
            return v(ax, ay, az);
        }
        float a = (c00 * (1 - tx) + c10 * tx) * (1 - ty) + (c01 * (1 - tx) + c11 * tx) * ty;
        float b = (d00 * (1 - tx) + d10 * tx) * (1 - ty) + (d01 * (1 - tx) + d11 * tx) * ty;
        return a * (1 - tz) + b * tz;
    }
};

// closest point of a triangle to p (Ericson, Real-Time Collision Detection 5.1.5) + the feature it lies on
static V3 closestOnTri(V3 p, V3 a, V3 b, V3 c, int& feat) {   // feat: 0..2 vertex a/b/c, 3..5 edge ab/bc/ca, 6 face
    V3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { feat = 0; return a; }
    V3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { feat = 1; return b; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { feat = 3; float v = d1 / (d1 - d3); return a + ab * v; }
    V3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { feat = 2; return c; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { feat = 5; float w = d2 / (d2 - d6); return a + ac * w; }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { feat = 4; float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); return b + (c - b) * w; }
    float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
    feat = 6;
    return a + ab * v + ac * w;
}

static SkinField buildSkin(const App& a, float cell, float reach) {
    SkinField S;
    std::vector<V3> pos; std::vector<float> fl; std::vector<std::array<int, 3>> tr;
    if (!a.scalpMesh(pos, fl, tr) || tr.empty()) return S;
    S.cell = cell;
    S.lo = S.hi = pos[0];
    for (auto& p : pos) { S.lo = {std::min(S.lo.x, p.x), std::min(S.lo.y, p.y), std::min(S.lo.z, p.z)}; S.hi = {std::max(S.hi.x, p.x), std::max(S.hi.y, p.y), std::max(S.hi.z, p.z)}; }
    float pad = reach + cell;
    S.lo = S.lo - V3{pad, pad, pad}; S.hi = S.hi + V3{pad, pad, pad};
    S.nx = int((S.hi.x - S.lo.x) / cell) + 2; S.ny = int((S.hi.y - S.lo.y) / cell) + 2; S.nz = int((S.hi.z - S.lo.z) / cell) + 2;
    size_t n = size_t(S.nx) * S.ny * S.nz;
    if (n > 40000000) return S;
    S.d.assign(n, SkinField::BIG);
    std::vector<float> best(n, 1e9f);
    std::vector<int> bestT(n, -1), bestF(n, 6);
    // pseudo-normals: face, angle-weighted vertex, edge (sum of the two faces)
    std::vector<V3> fn(tr.size()), vn(pos.size(), V3{0, 0, 0});
    std::map<std::pair<int, int>, V3> en;
    // winding: the mesh's triangle order is not known to be outward; take the majority vote against the head centre
    int votes = 0;
    for (size_t t = 0; t < tr.size(); t++) {
        V3 A = pos[tr[t][0]], B = pos[tr[t][1]], C = pos[tr[t][2]];
        V3 cen = (A + B + C) * (1.0f / 3);
        if (cen.z < a.pack.headC.z - 0.02f) continue;   // skull only for the vote (the neck is not star-shaped from the centre)
        votes += dot(cross(B - A, C - A), cen - a.pack.headC) >= 0 ? 1 : -1;
    }
    float flip = votes >= 0 ? 1.0f : -1.0f;
    for (size_t t = 0; t < tr.size(); t++) {
        V3 A = pos[tr[t][0]], B = pos[tr[t][1]], C = pos[tr[t][2]];
        V3 nn = cross(B - A, C - A) * flip;
        float l = len(nn);
        fn[t] = l > 1e-12f ? nn * (1.0f / l) : V3{0, 0, 1};
        for (int c = 0; c < 3; c++) {
            V3 p0 = pos[tr[t][c]], p1 = pos[tr[t][(c + 1) % 3]], p2 = pos[tr[t][(c + 2) % 3]];
            V3 e1 = nrmz(p1 - p0), e2 = nrmz(p2 - p0);
            float ang = std::acos(std::clamp(dot(e1, e2), -1.0f, 1.0f));
            vn[tr[t][c]] = vn[tr[t][c]] + fn[t] * ang;
            int i0 = tr[t][c], i1 = tr[t][(c + 1) % 3];
            auto key = std::make_pair(std::min(i0, i1), std::max(i0, i1));
            en[key] = en[key] + fn[t];
        }
    }
    int R = int(std::ceil(reach / cell));
    for (size_t t = 0; t < tr.size(); t++) {
        V3 A = pos[tr[t][0]], B = pos[tr[t][1]], C = pos[tr[t][2]];
        V3 tlo{std::min({A.x, B.x, C.x}), std::min({A.y, B.y, C.y}), std::min({A.z, B.z, C.z})};
        V3 thi{std::max({A.x, B.x, C.x}), std::max({A.y, B.y, C.y}), std::max({A.z, B.z, C.z})};
        int x0 = std::max(0, int((tlo.x - S.lo.x) / cell) - R), x1 = std::min(S.nx - 1, int((thi.x - S.lo.x) / cell) + R + 1);
        int y0 = std::max(0, int((tlo.y - S.lo.y) / cell) - R), y1 = std::min(S.ny - 1, int((thi.y - S.lo.y) / cell) + R + 1);
        int z0 = std::max(0, int((tlo.z - S.lo.z) / cell) - R), z1 = std::min(S.nz - 1, int((thi.z - S.lo.z) / cell) + R + 1);
        for (int z = z0; z <= z1; z++) for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
            V3 p{S.lo.x + x * cell, S.lo.y + y * cell, S.lo.z + z * cell};
            int feat;
            V3 q = closestOnTri(p, A, B, C, feat);
            float dd = len(p - q);
            if (dd > reach) continue;
            size_t i = (size_t(z) * S.ny + y) * S.nx + x;
            if (dd < best[i]) { best[i] = dd; bestT[i] = int(t); bestF[i] = feat; }
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (bestT[i] < 0) continue;
        int t = bestT[i], f = bestF[i];
        V3 nn;
        if (f == 6) nn = fn[t];
        else if (f < 3) nn = vn[tr[t][f]];
        else { int i0 = tr[t][f - 3], i1 = tr[t][(f - 2) % 3]; nn = en[std::make_pair(std::min(i0, i1), std::max(i0, i1))]; }
        int z = int(i / (size_t(S.nx) * S.ny)), y = int((i / S.nx) % S.ny), x = int(i % S.nx);
        V3 p{S.lo.x + x * cell, S.lo.y + y * cell, S.lo.z + z * cell};
        int feat;
        V3 q = closestOnTri(p, pos[tr[t][0]], pos[tr[t][1]], pos[tr[t][2]], feat);
        S.d[i] = dot(p - q, nn) >= 0 ? best[i] : -best[i];
    }
    S.ok = true;
    return S;
}

// barycentric weights of the interpolated hairs of a face: n subdivisions, corners (the guides themselves) left out
static std::vector<std::array<float, 3>> baryWeights(int n) {
    std::vector<std::array<float, 3>> w;
    for (int i = 0; i <= n; i++) for (int j = 0; j <= n - i; j++) {
        float a = float(i) / n, b = float(j) / n, c = 1 - a - b;
        if (a > 0.999f || b > 0.999f || c > 0.999f) continue;
        w.push_back({a, b, c});
    }
    return w;
}

// deepest penetration (m, > 0 = inside the skin) of a polyline, sampled at the points and the segment midpoints,
// skipping the first rootM of arc length (the root band sits at the fur's root depth, often a few mm under the mesh)
// at (2026-10-16, handback gate): the deepest sample's position, so the caller can tell skin (face / neck) from scalp
static float deepest(const SkinField& S, const std::vector<V3>& poly, size_t from, float rootM = 0, V3* at = nullptr) {
    float worst = 0, arc = 0;
    for (size_t k = from; k < poly.size(); k++) {
        if (k > 0) arc += len(poly[k] - poly[k - 1]);
        if (arc < rootM) continue;
        float v = S.at(poly[k]);
        if (v < SkinField::BIG * 0.5f && -v > worst) { worst = -v; if (at) *at = poly[k]; }
        if (k + 1 < poly.size()) {
            V3 m = (poly[k] + poly[k + 1]) * 0.5f;
            v = S.at(m);
            if (v < SkinField::BIG * 0.5f && -v > worst) { worst = -v; if (at) *at = m; }
        }
    }
    return worst;
}

struct FaceRec {
    float area = 0, restDepth = 0, windDepth = 0, spreadMid = 0, spreadTip = 0, angDeg = 0;
    int windDir = -1;
    bool guideRest = false, guideWind = false, twist = false;
    V3 restAt{}, windAt{};   // where the deepest interpolated sample sits (rest / wind)
};

}  // namespace fchk

static json checkFaces(App& a, const json& c, int& grpId) {
    using namespace fchk;
    using namespace t6;
    json r;
    Groom& gr = a.groom;
    const int K = gr.K;
    const size_t NF = gr.faces.size(), NG = gr.P.size();
    float depthMm = c.value("depth_mm", 3.0f);
    float depth = depthMm / 1000;   // raised below by the measured root offset (roots under the mesh)
    float rootM = c.value("root_mm", 12.0f) / 1000;
    float sliverMm2 = c.value("sliver_mm2", 3.0f), bigX = c.value("big_x", 4.0f);
    float spreadK = c.value("spread", 4.0f), divDeg = c.value("div_deg", 60.0f);
    int nSub = std::clamp(c.value("samples", 4), 2, 8);
    bool dry = c.value("list_only", false);
    SkinField S = buildSkin(a, c.value("cell_mm", 4.0f) / 1000, c.value("reach_mm", 30.0f) / 1000);
    if (!S.ok) { r["error"] = "no posed head mesh (scalpMesh)"; return r; }
    if (c.contains("dump_skin")) {   // debug: the posed head mesh as OBJ (metres)
        std::vector<V3> pos; std::vector<float> fl; std::vector<std::array<int, 3>> tr;
        a.scalpMesh(pos, fl, tr);
        std::ofstream o(c["dump_skin"].get<std::string>());
        for (auto& p : pos) o << "v " << p.x << " " << p.y << " " << p.z << '\n';
        for (auto& t : tr) o << "f " << t[0] + 1 << " " << t[1] + 1 << " " << t[2] + 1 << '\n';
        r["dump_skin"] = {{"vertices", pos.size()}, {"triangles", tr.size()}};
    }
    json dumpSim = json::object();
    auto ix = a.selectGuides(c.value("where", json::object({{"all", true}})));
    std::set<int> in(ix.begin(), ix.end());
    std::vector<char> faceIn(NF, 0);
    for (size_t f = 0; f < NF; f++) {
        auto& F = gr.faces[f];
        if (!gr.alive[F.g[0]] || !gr.alive[F.g[1]] || !gr.alive[F.g[2]]) continue;
        if (in.count(F.g[0]) || in.count(F.g[1]) || in.count(F.g[2])) faceIn[f] = 1;
    }
    auto W = baryWeights(nSub);
    std::vector<FaceRec> rec(NF);
    // ---- authored guides (edit frame + skinning): the face geometry metrics (area, spread, angle, twist). Penetration
    // is NOT judged on them: the SDK always simulates (and collides) the guides before drawing, so an authored guide
    // through an ear is pushed out at rest. Penetration is measured on the simulated guides below (rest = wind 0).
    std::vector<std::vector<V3>> PG(NG);
    std::vector<float> gRest(NG, 0), gWind(NG, 0), gAuth(NG, 0);
    for (size_t g = 0; g < NG; g++) {
        if (!gr.alive[g] || (int)gr.P[g].size() < K) continue;
        PG[g].resize(K);
        for (int k = 0; k < K; k++) PG[g][k] = a.posedPoint((int)g, k);
        gAuth[g] = deepest(S, PG[g], 1, rootM);
    }
    // skin field self-check: the roots sit on the skin, so their signed distance should be about 0
    json skinChk;
    {
        std::vector<float> rd;
        for (size_t g = 0; g < NG; g++) if (!PG[g].empty()) { float v = S.at(PG[g][0]); if (v < SkinField::BIG * 0.5f) rd.push_back(v * 1000); }
        std::sort(rd.begin(), rd.end());
        float rootOff = rd.empty() ? 0 : rd[rd.size() / 2] / 1000;
        if (rootOff < 0 && c.value("root_offset", true)) depth += -rootOff;
        int nAuth = 0; for (size_t g = 0; g < NG; g++) if (gAuth[g] > depth) nAuth++;
        float hcv = S.at(a.pack.headC);
        skinChk = {{"roots_in_band", rd.size()}, {"roots_signed_mm_p10", rd.empty() ? 0 : rd[rd.size() / 10]}, {"roots_signed_mm_p50", rd.empty() ? 0 : rd[rd.size() / 2]},
                   {"roots_signed_mm_p90", rd.empty() ? 0 : rd[rd.size() * 9 / 10]}, {"head_centre_mm", hcv < SkinField::BIG * 0.5f ? hcv * 1000 : -1e9f},
                   {"authored_guides_inside", nAuth}, {"cells", S.d.size()}, {"depth_threshold_mm", depth * 1000}, {"root_skip_mm", rootM * 1000}};
    }
    std::vector<float> areas;
    for (size_t f = 0; f < NF; f++) {
        if (!faceIn[f]) continue;
        auto& F = gr.faces[f];
        FaceRec& R = rec[f];
        R.area = faceAreaMm2(gr, F);
        areas.push_back(R.area);
        const auto &g0 = PG[F.g[0]], &g1 = PG[F.g[1]], &g2 = PG[F.g[2]];
        if (g0.empty() || g1.empty() || g2.empty()) continue;
        // divergence: spread at mid / tip vs the root spread, direction angle between the guides at mid length
        auto sp = [&](int k) { return std::max({len(g0[k] - g1[k]), len(g1[k] - g2[k]), len(g2[k] - g0[k])}); };
        float rs = std::max(sp(0), 0.002f);
        R.spreadMid = sp(K / 2) / rs; R.spreadTip = sp(K - 1) / rs;
        int km = K / 2;
        V3 d0 = nrmz(g0[km] - g0[km - 1]), d1 = nrmz(g1[km] - g1[km - 1]), d2 = nrmz(g2[km] - g2[km - 1]);
        float cmin = std::min({dot(d0, d1), dot(d1, d2), dot(d0, d2)});
        R.angDeg = std::acos(std::clamp(cmin, -1.0f, 1.0f)) * 180 / 3.14159265f;
        // twist: the guide triangle flips its winding (seen along the root normal) while still being a real triangle
        V3 nn = nrmz(cross(g1[0] - g0[0], g2[0] - g0[0]));
        float s0 = dot(cross(g1[0] - g0[0], g2[0] - g0[0]), nn);
        for (int k = 1; k < K - 1 && !R.twist; k++) {
            V3 cr = cross(g1[k] - g0[k], g2[k] - g0[k]);
            float sk = dot(cr, nn);
            if (sk * s0 < 0 && len(cr) > 0.3f * std::fabs(s0)) R.twist = true;
        }
    }
    std::sort(areas.begin(), areas.end());
    float medArea = areas.empty() ? 0 : areas[areas.size() / 2];
    // ---- simulated guides read back from the GPU: rest (wind 0, settled) and each wind heading
    json dirs = json::array();
    if (c.contains("wind_yaw_deg")) { if (c["wind_yaw_deg"].is_array()) dirs = c["wind_yaw_deg"]; else dirs.push_back(c["wind_yaw_deg"]); }
    else dirs = {15.0f, 105.0f, 195.0f, 285.0f};
    bool wind = c.value("wind", true) && !dirs.empty();
    int samples = 0;
    bool simOk = a.sdkOk;
    if (simOk) {
        float yaw0 = a.motion.windYawDeg, pit0 = a.motion.windPitchDeg, ws0 = a.motion.windStrength;
        bool sim0 = a.simulate, wind0 = a.windOn, paused0 = a.paused;
        float strength = c.value("wind_strength", 1.0f);   // 1 = the toolbar default the user looks at (x look windScaler); 5 = toolbar max, a stress test that overpowers the SDK collision
        int steps = std::max(30, int(c.value("wind_s", 2.5f) * 60));
        std::vector<float> xyz; int PGn = 0;
        // pass -1 = rest (settled, no wind), 0.. = the headings
        for (int di = -1; di < (wind ? (int)dirs.size() : 0); di++) {
            bool rest = di < 0;
            a.motion.windYawDeg = rest ? yaw0 : dirs[di].get<float>(); a.motion.windPitchDeg = c.value("wind_pitch_deg", 2.85f);
            a.rebuildHair();
            a.simulate = true; a.paused = false; a.windOn = !rest;
            a.motion.preset = MotionPreset::Rest; a.motion.speed = 1; a.motion.amount = 1; a.motion.windStrength = rest ? 0.0f : strength; a.motion.t = 0; a.simAcc = 0; a.teleport = true;
            int nSteps = rest ? std::max(60, steps) : steps;
            for (int s = 0; s < nSteps; s++) {
                a.step(1.0 / 60.0);
                if (rest ? (s + 1 < nSteps) : (s < steps / 3 || (s % 10 && s + 1 < nSteps))) continue;   // rest: the settled state only; wind: every 10th step + the last
                if (!a.hw.readGuides(xyz, PGn) || PGn < 2) continue;
                if (!rest) samples++;
                std::vector<std::vector<V3>> SG(NG);
                bool keepSim = c.contains("dump_sim") && (rest || s + 1 >= nSteps);
                for (size_t aI = 0; aI < a.guideOfAsset.size(); aI++) {
                    int g = a.guideOfAsset[aI];
                    if (g < 0 || g >= (int)NG || !gr.alive[g]) continue;
                    auto& cv = SG[g];
                    cv.resize(PGn);
                    for (int j = 0; j < PGn; j++) cv[j] = {xyz[(aI * PGn + j) * 3], xyz[(aI * PGn + j) * 3 + 1], xyz[(aI * PGn + j) * 3 + 2]};
                    float dd = deepest(S, cv, 1, rootM);
                    if (rest) gRest[g] = std::max(gRest[g], dd); else gWind[g] = std::max(gWind[g], dd);
                }
                if (keepSim) {   // debug: the simulated guides of this state (groom index -> PGn points)
                    json gj = json::object();
                    for (size_t g = 0; g < NG; g++) if (!SG[g].empty()) { json pts = json::array(); for (auto& q : SG[g]) pts.push_back({q.x, q.y, q.z}); gj[std::to_string(g)] = pts; }
                    dumpSim[rest ? "rest" : std::to_string(di)] = gj;
                }
                std::vector<V3> poly(PGn);
                for (size_t f = 0; f < NF; f++) {
                    if (!faceIn[f]) continue;
                    auto& F = gr.faces[f];
                    const auto &g0 = SG[F.g[0]], &g1 = SG[F.g[1]], &g2 = SG[F.g[2]];
                    if (g0.empty() || g1.empty() || g2.empty()) continue;
                    float worst = 0; V3 worstAt{};
                    for (auto& w : W) {
                        for (int j = 0; j < PGn; j++) poly[j] = g0[j] * w[0] + g1[j] * w[1] + g2[j] * w[2];
                        V3 at{};
                        float dd = deepest(S, poly, 1, rootM, &at);
                        if (dd > worst) { worst = dd; worstAt = at; }
                    }
                    if (rest) { if (worst > rec[f].restDepth) { rec[f].restDepth = worst; rec[f].restAt = worstAt; } }
                    else if (worst > rec[f].windDepth) { rec[f].windDepth = worst; rec[f].windDir = di; rec[f].windAt = worstAt; }
                }
            }
        }
        for (size_t f = 0; f < NF; f++) if (faceIn[f]) {
            auto& F = gr.faces[f];
            rec[f].guideRest = std::max({gRest[F.g[0]], gRest[F.g[1]], gRest[F.g[2]]}) > depth;
            rec[f].guideWind = std::max({gWind[F.g[0]], gWind[F.g[1]], gWind[F.g[2]]}) > depth;
        }
        a.motion.windYawDeg = yaw0; a.motion.windPitchDeg = pit0; a.motion.windStrength = ws0;
        a.simulate = sim0; a.windOn = wind0; a.paused = paused0;
        a.motion.preset = MotionPreset::Rest; a.teleport = true;
        a.rebuildHair();
        for (int i = 0; i < 30; i++) a.step(1.0 / 60.0);
    }
    // ---- classify
    auto mmv = [](float m) { return std::round(m * 10000) / 10; };
    json lPen = json::array(), lSliver = json::array(), lBig = json::array(), lDiv = json::array(), lGuides = json::array();
    int nPenRest = 0, nPenWind = 0, nPenInterp = 0, nPenGuides = 0, nSliver = 0, nBig = 0, nDiv = 0, nTwist = 0, nIn = 0;
    // 2026-10-16 (handback gate): where the hair goes in. "skin" = the deepest sample is nearest to a NON-scalp vertex of
    // the head mesh (face, ears, neck: visible hair through the skin); otherwise it is under the painted scalp (hair
    // inside the skull: invisible, the bald-patch mechanism), which the parting cover trade-off tolerates
    Hairline hlSkin;
    bool hlOk = hairlineOf(a, hlSkin);
    auto throughSkin = [&](V3 p) { return hlOk && !onScalp(hlSkin, p); };
    int nSkinRest = 0, nSkinWind = 0, nSkinInterp = 0;
    std::set<int> fixSet;
    std::set<std::string> what;
    if (c.contains("fix_what")) for (auto& s : c["fix_what"]) what.insert(s.get<std::string>()); else what = {"penetration"};
    for (size_t f = 0; f < NF; f++) {
        if (!faceIn[f]) continue;
        nIn++;
        auto& F = gr.faces[f];
        FaceRec& R = rec[f];
        bool penR = R.restDepth > depth, penW = R.windDepth > depth;
        bool guidesToo = (penR && R.guideRest) || (penW && R.guideWind);
        bool interpOnly = (penR || penW) && !guidesToo;
        if (penR) nPenRest++;
        if (penW) nPenWind++;
        if (interpOnly) nPenInterp++;
        if (guidesToo) nPenGuides++;
        bool sliver = R.area < sliverMm2, big = medArea > 0 && R.area > bigX * medArea;
        bool div = R.spreadMid > spreadK || R.angDeg > divDeg;
        if (sliver) nSliver++;
        if (big) nBig++;
        if (div) nDiv++;
        if (R.twist) nTwist++;
        V3 cen = (gr.P[F.g[0]][0] + gr.P[F.g[1]][0] + gr.P[F.g[2]][0]) * (1.0f / 3);
        json fj = {{"id", (int)f}, {"guides", {F.g[0], F.g[1], F.g[2]}}, {"centroid", v3j(cen)}, {"area_mm2", std::round(R.area * 10) / 10}};
        bool skinR = penR && throughSkin(R.restAt), skinW = penW && throughSkin(R.windAt);
        if (skinR) nSkinRest++;
        if (skinW) nSkinWind++;
        if ((skinR || skinW) && interpOnly) nSkinInterp++;
        if ((penR || penW) && lPen.size() < 400) {
            json pj = fj;
            pj["rest_depth_mm"] = mmv(R.restDepth); pj["wind_depth_mm"] = mmv(R.windDepth);
            if (R.windDir >= 0) pj["wind_yaw_deg"] = dirs[R.windDir];
            pj["interp_only"] = interpOnly; pj["guides_penetrate"] = guidesToo;
            pj["at"] = v3j(penW && R.windDepth >= R.restDepth ? R.windAt : R.restAt);
            pj["skin"] = skinR || skinW;
            lPen.push_back(pj);
        }
        if (what.count("skin") && (skinR || skinW) && interpOnly) fixSet.insert((int)f);
        if (sliver && lSliver.size() < 300) lSliver.push_back(fj);
        if (big && lBig.size() < 200) lBig.push_back(fj);
        if (div && lDiv.size() < 300) { json dj = fj; dj["spread_mid"] = std::round(R.spreadMid * 10) / 10; dj["spread_tip"] = std::round(R.spreadTip * 10) / 10; dj["angle_deg"] = std::round(R.angDeg); lDiv.push_back(dj); }
        if (what.count("penetration") && interpOnly) fixSet.insert((int)f);
        if (what.count("guides") && guidesToo) fixSet.insert((int)f);
        if (what.count("sliver") && sliver) fixSet.insert((int)f);
        if (what.count("big") && big) fixSet.insert((int)f);
        if (what.count("divergent") && div) fixSet.insert((int)f);
    }
    int nGIn = 0;
    for (size_t g = 0; g < NG; g++) {
        if (!gr.alive[g] || !in.count((int)g)) continue;
        if (gRest[g] > depth || gWind[g] > depth) {
            nGIn++;
            if (lGuides.size() < 300) lGuides.push_back({{"i", (int)g}, {"root", v3j(gr.P[g][0])}, {"rest_depth_mm", mmv(gRest[g])}, {"wind_depth_mm", mmv(gWind[g])}});
        }
    }
    if (c.contains("protect")) { for (int g : a.selectGuides(c["protect"])) for (size_t f = 0; f < NF; f++) { auto& F = gr.faces[f]; if (F.g[0] == g || F.g[1] == g || F.g[2] == g) fixSet.erase((int)f); } }
    r["faces_checked"] = nIn; r["median_area_mm2"] = std::round(medArea * 10) / 10; r["depth_mm"] = depthMm;
    r["penetration"] = {{"rest", nPenRest}, {"wind", nPenWind}, {"interp_only", nPenInterp}, {"guides_too", nPenGuides},
                        {"skin_rest", nSkinRest}, {"skin_wind", nSkinWind}, {"skin_interp_only", nSkinInterp}, {"list", lPen}};
    r["guides_inside"] = {{"count", nGIn}, {"list", lGuides}};
    r["sliver"] = {{"count", nSliver}, {"mm2", sliverMm2}, {"list", lSliver}};
    r["big"] = {{"count", nBig}, {"x_median", bigX}, {"list", lBig}};
    r["divergent"] = {{"count", nDiv}, {"twisted", nTwist}, {"list", lDiv}};
    r["wind"] = {{"on", wind && simOk}, {"yaw_deg", dirs}, {"samples", samples}};
    r["skin_check"] = skinChk; r["simulated"] = simOk;
    if (c.contains("dump_sim")) { std::ofstream o(c["dump_sim"].get<std::string>()); o << dumpSim.dump(); r["dump_sim"] = c["dump_sim"]; }
    r["fix_candidates"] = fixSet.size();
    if (!c.value("fix", false) || dry || fixSet.empty()) return r;
    int id = 0, grp = 1000 + (++grpId);
    for (auto& o : gr.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    int n = 0;
    for (int f : fixSet) {
        auto& F = gr.faces[f];
        gr.ops.push_back({{"op", "delete_face"}, {"roots", {rootJ(gr.P[F.g[0]][0]), rootJ(gr.P[F.g[1]][0]), rootJ(gr.P[F.g[2]][0])}}, {"id", ++id}, {"grp", grp}});
        n++;
    }
    gr.redo.clear(); gr.replay(); a.editsChanged();
    r["deleted_faces"] = n;
    return r;
}


// skin_guard (2026-10-16, tool architect; "hair must never go through the head / neck"): a hard geometric floor on the
// AUTHORED rest shape. Every control point of every guide in `where` whose signed distance to the posed skin field is
// under margin_mm (3) is moved out along the field gradient until it sits at margin_mm (root kept: roots lie on the
// scalp by construction, and the first root_mm (6) of arc length is left alone so the hairline does not lift), repeated
// `passes` (3) times because the field is only piecewise linear. Plain reshape ops in one undo group (edit frame; the
// displacement is computed in the posed frame as push_out does). Why a second tool next to push_out: push_out moves only
// points "clearly inside" (depth_mm 8 under the skull radial model) and knows nothing of the face / ears / neck; the
// skin field covers the whole head + neck mesh. list_only reports the counts without moving anything.
static json skinGuard(App& a, const json& c, int& grpId) {
    using namespace fchk;
    using namespace t6;
    json r;
    Groom& gr = a.groom;
    const size_t NG = gr.P.size();
    float margin = c.value("margin_mm", 3.0f) / 1000, rootMm = c.value("root_mm", 6.0f) / 1000;
    int passes = std::clamp(c.value("passes", 3), 1, 10);
    bool dry = c.value("list_only", false);
    SkinField S = buildSkin(a, c.value("cell_mm", 4.0f) / 1000, c.value("reach_mm", 30.0f) / 1000);
    if (!S.ok) { r["error"] = "no head mesh"; return r; }
    const float h = S.cell;
    auto grad = [&](V3 p) {
        float dx = S.at({p.x + h, p.y, p.z}) - S.at({p.x - h, p.y, p.z});
        float dy = S.at({p.x, p.y + h, p.z}) - S.at({p.x, p.y - h, p.z});
        float dz = S.at({p.x, p.y, p.z + h}) - S.at({p.x, p.y, p.z - h});
        V3 g{dx, dy, dz};
        if (std::fabs(dx) > SkinField::BIG * 0.1f || std::fabs(dy) > SkinField::BIG * 0.1f || std::fabs(dz) > SkinField::BIG * 0.1f) return V3{0, 0, 0};
        return nrmz(g);
    };
    auto ix = a.selectGuides(c.value("where", json::object({{"all", true}})));
    int id = 0, grp = 1000 + (++grpId);
    for (auto& o : gr.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    int nGuides = 0, nPoints = 0, nInsideBefore = 0;
    float maxMove = 0, deepestMove = 0;
    json list = json::array();
    std::vector<json> ops;
    for (int g : ix) {
        if (g < 0 || g >= (int)NG || !gr.alive[g] || gr.P[g].size() < 2) continue;
        Guide E = gr.P[g];
        std::vector<V3> Pp(E.size());
        for (size_t k = 0; k < E.size(); k++) Pp[k] = a.posedPoint(g, (int)k);
        auto sl = arcLen(Pp);
        bool moved = false; float gMove = 0, gDeep = 0;
        for (int pass = 0; pass < passes; pass++) {
            for (size_t k = 1; k < Pp.size(); k++) {
                if (sl[k] < rootMm) continue;
                float d = S.at(Pp[k]);
                if (d >= SkinField::BIG * 0.5f || d >= margin) continue;
                if (pass == 0) { gDeep = std::max(gDeep, margin - d); }
                V3 n = grad(Pp[k]);
                if (len(n) < 0.5f) continue;
                V3 dp = n * (margin - d);
                Pp[k] = Pp[k] + dp; E[k] = E[k] + dp;
                gMove = std::max(gMove, len(dp)); moved = true;
                if (pass == 0) nPoints++;
            }
        }
        if (!moved) continue;
        nGuides++; maxMove = std::max(maxMove, gMove); deepestMove = std::max(deepestMove, gDeep);
        if (list.size() < 60) list.push_back({{"i", g}, {"deepest_mm", std::round(gDeep * 10000) / 10}, {"move_mm", std::round(gMove * 10000) / 10}, {"root", rootJ(gr.P[g][0])}});
        if (!dry) ops.push_back({{"op", "reshape"}, {"root", rootJ(gr.P[g][0])}, {"points", ptsJson(E)}, {"id", ++id}, {"grp", grp}});
    }
    for (size_t g = 0; g < NG; g++) if (gr.alive[g] && gr.P[g].size() >= 2) { float dd = deepest(S, [&]{ std::vector<V3> Q(gr.P[g].size()); for (size_t k = 0; k < Q.size(); k++) Q[k] = a.posedPoint((int)g, (int)k); return Q; }(), 1, rootMm); if (dd > 0.0f) nInsideBefore++; }
    r["margin_mm"] = margin * 1000; r["guides_moved"] = nGuides; r["points_moved"] = nPoints;
    r["max_move_mm"] = std::round(maxMove * 10000) / 10; r["deepest_mm"] = std::round(deepestMove * 10000) / 10;
    r["guides_under_skin_before"] = nInsideBefore; r["list"] = list;
    if (dry || ops.empty()) return r;
    for (auto& o : ops) gr.ops.push_back(o);
    gr.redo.clear(); gr.replay(); a.editsChanged();
    r["unmatched"] = gr.unmatched;
    return r;
}
