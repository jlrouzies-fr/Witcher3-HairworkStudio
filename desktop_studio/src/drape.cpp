// Hairwork Studio - "drape hair over outfit" (2026-10-14). Our own code (Studio License).
//
// Guide points that lie inside (or within pad of) the selected scene meshes (hood, scabbard, sword, outfit pieces, outfit-mod
// meshes) are moved to the OUTSIDE of the mesh surface and the rest of the strand is carried with them, so the hair drapes over
// the piece. Meshes are read as a radial height map around the body axis: for every (azimuth, height) cell the OUTERMOST radius
// of the selected triangles (sampled every ~2 mm). A point whose radius is smaller than that + pad is under / in the piece.
// Per guide, from the first offending segment on, the strand is re-walked CV by CV: the old segment vector is kept (shape and
// length), and when the new point would be inside, the segment direction is turned toward "outward" by the smallest amount that
// brings the point and 3 samples of the segment outside. The root part (first root_mm) never moves. The result is written as
// one "reshape" op per changed guide (the same op studio_push_out emits), so it is undoable, saved in the project and replayed
// by the export (desk_export) without a new op type.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "app.h"

using namespace DirectX;

namespace {

struct RadialMap {
    int na = 360;
    float dz = 0.002f, z0 = 0, cx = 0, cy = 0;
    int nz = 0;
    std::vector<float> r, rn;   // outermost / innermost surface radius per cell
    void init(float cx_, float cy_, float zlo, float zhi) {
        cx = cx_; cy = cy_;
        z0 = zlo - 0.01f;
        nz = std::max(1, int((zhi - zlo + 0.02f) / dz) + 1);
        r.assign(size_t(na) * nz, -1.0f);
        rn.assign(size_t(na) * nz, 1e9f);
    }
    int azBin(float x, float y) const {
        float a = std::atan2(y - cy, x - cx);   // -pi..pi
        int i = int(std::floor((a + 3.14159265f) / (6.2831853f / na)));
        return ((i % na) + na) % na;
    }
    void add(V3 p) {
        int iz = int(std::floor((p.z - z0) / dz));
        if (iz < 0 || iz >= nz) return;
        float rr = std::hypot(p.x - cx, p.y - cy);
        size_t cell = size_t(azBin(p.x, p.y)) * nz + iz;
        if (rr > r[cell]) r[cell] = rr;
        if (rr < rn[cell]) rn[cell] = rr;
    }
    // innermost surface radius around p (3 x 3 cells), -1 = no piece there
    float queryIn(V3 p) const {
        int iz = int(std::floor((p.z - z0) / dz));
        if (iz < 0 || iz >= nz) return -1.0f;
        int ia = azBin(p.x, p.y);
        float best = 1e9f;
        for (int da = -1; da <= 1; da++)
            for (int dzz = -1; dzz <= 1; dzz++) {
                int z = iz + dzz;
                if (z < 0 || z >= nz) continue;
                best = std::min(best, rn[size_t((ia + da + na) % na) * nz + z]);
            }
        return best > 1e8f ? -1.0f : best;
    }
    // outermost surface radius around p (3 x 3 cells), -1 = no piece there
    float query(V3 p) const {
        int iz = int(std::floor((p.z - z0) / dz));
        if (iz < 0 || iz >= nz) return -1.0f;
        int ia = azBin(p.x, p.y);
        float best = -1.0f;
        for (int da = -1; da <= 1; da++)
            for (int dzz = -1; dzz <= 1; dzz++) {
                int z = iz + dzz;
                if (z < 0 || z >= nz) continue;
                best = std::max(best, r[size_t((ia + da + na) % na) * nz + z]);
            }
        return best;
    }
};

void sampleTriangle(RadialMap& m, V3 a, V3 b, V3 c) {
    float e = std::max({len(b - a), len(c - b), len(a - c)});
    int n = std::clamp(int(std::ceil(e / 0.002f)), 1, 80);
    for (int i = 0; i <= n; i++)
        for (int j = 0; j <= n - i; j++) {
            float u = float(i) / n, v = float(j) / n;
            m.add(a * (1 - u - v) + b * u + c * v);
        }
}

V3 unit(V3 v, V3 dflt = {0, -1, 0}) { float l = len(v); return l > 1e-9f ? v * (1.0f / l) : dflt; }

struct Selection {
    std::vector<int> idx;           // pack.meshes indices
    std::vector<std::string> names;
    std::string error;
};

std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

bool drapeCandidate(const Mesh& m) {
    if (m.kind == "head" || m.kind == "hair_cards" || m.kind == "beard" || m.kind == "headgear" || m.v.empty() || m.t.empty()) return false;
    std::string tf = lower(m.texFile);
    if (tf.find("eye") != std::string::npos || tf.find("teeth") != std::string::npos || tf.find("tongue") != std::string::npos) return false;
    if (m.alphaTest) return false;
    return true;
}

// the hood of the modCiri outfit: chunks with a good part of their triangles in the hood box of the back_items config
bool hoodChunk(const Mesh& m) {
    { std::string nm = lower(m.name); if (nm.find("scabbard") != std::string::npos || nm.find("sword") != std::string::npos) return false; }
    const float b[6] = {-0.16f, 0.16f, -0.26f, -0.115f, 1.33f, 1.65f};
    int n = 0;
    for (auto& t : m.t) {
        V3 c = (m.v[t[0]] + m.v[t[1]] + m.v[t[2]]) * (1.0f / 3);
        if (c.x >= b[0] && c.x <= b[1] && c.y >= b[2] && c.y <= b[3] && c.z >= b[4] && c.z <= b[5]) n++;
    }
    return n >= 100;
}

Selection pickMeshes(const App& a, const json& want) {
    Selection s;
    const auto& M = a.pack.meshes;
    auto label = [&](size_t i) { return "#" + std::to_string(i) + " " + M[i].name + " [" + M[i].texFile + "]"; };
    std::set<int> got;
    for (auto& w : want) {
        if (w.is_number_integer()) {
            int i = w.get<int>();
            if (i < 0 || i >= (int)M.size() || !drapeCandidate(M[i])) { s.error = "mesh #" + std::to_string(i) + " is not a drapeable mesh"; return s; }
            got.insert(i);
            continue;
        }
        std::string k = lower(w.get<std::string>());
        int n = 0;
        for (size_t i = 0; i < M.size(); i++) {
            if (!drapeCandidate(M[i])) continue;
            bool hit;
            if (k == "all") hit = true;
            else if (k == "outfit") hit = M[i].kind == "outfit";
            else if (k == "hood") hit = hoodChunk(M[i]);
            else hit = lower(M[i].name + " " + M[i].texFile + " " + M[i].path).find(k) != std::string::npos;
            if (hit) { got.insert((int)i); n++; }
        }
        if (!n) { s.error = "no mesh matches '" + k + "'"; return s; }
    }
    for (int i : got) { s.idx.push_back(i); s.names.push_back(label(i)); }
    if (s.idx.empty()) s.error = "no mesh selected";
    return s;
}

// vertex positions of mesh mi: bind pose (rest) or posed with the same head / neck / root blend the Studio draws
std::vector<V3> meshPositions(const App& a, int mi, bool posed) {
    const Mesh& m = a.pack.meshes[mi];
    if (!posed) return m.v;
    const DeskPack& sk = a.skel;
    XMMATRIX H = a.motion.headM(sk), Nk = a.motion.neckM(sk), R = a.motion.rootM(sk);
    float neckZ = 1.62f;
    int nb = sk.boneIndex("neck");
    if (nb >= 0) neckZ = sk.bones[nb].bind[14];
    std::vector<V3> P(m.v.size());
    for (size_t i = 0; i < m.v.size(); i++) {
        XMVECTOR p = XMVectorSet(m.v[i].x, m.v[i].y, m.v[i].z, 1);
        float hz = std::clamp((m.v[i].z - (neckZ - 0.03f)) / 0.08f, 0.0f, 1.0f);
        XMVECTOR q = hz >= 1.0f ? XMVector3TransformCoord(p, H) : XMVectorLerp(XMVector3TransformCoord(p, R), XMVector3TransformCoord(p, Nk), hz);
        XMFLOAT3 o;
        XMStoreFloat3(&o, q);
        P[i] = {o.x, o.y, o.z};
    }
    return P;
}

bool buildMap(const App& a, const Selection& sel, bool posed, const json& c, RadialMap& map, std::string* err) {
    float zlo = 1e9f, zhi = -1e9f;
    std::vector<std::vector<V3>> pos;
    bool boxed = c.contains("box") && c["box"].is_array() && c["box"].size() == 6;
    float bx[6] = {0, 0, 0, 0, 0, 0};
    if (boxed) for (int i = 0; i < 6; i++) bx[i] = c["box"][i].get<float>();
    for (int mi : sel.idx) {
        pos.push_back(meshPositions(a, mi, posed));
        for (auto& p : pos.back()) { zlo = std::min(zlo, p.z); zhi = std::max(zhi, p.z); }
    }
    if (zhi < zlo) { if (err) *err = "empty meshes"; return false; }
    V3 ax = a.pack.headC;                       // body axis: the neck bone when the fur has one (the head centre sits ahead of the spine)
    { int nb = a.pack.boneIndex("neck"); if (nb >= 0) ax = {a.pack.bones[nb].bind[12], a.pack.bones[nb].bind[13], 0}; }
    if (c.contains("axis") && c["axis"].is_array() && c["axis"].size() >= 2) ax = {c["axis"][0].get<float>(), c["axis"][1].get<float>(), 0};
    map.init(ax.x, ax.y, zlo, zhi);
    for (size_t s = 0; s < sel.idx.size(); s++) {
        const Mesh& m = a.pack.meshes[sel.idx[s]];
        for (auto& t : m.t) {
            V3 A = pos[s][t[0]], B = pos[s][t[1]], C = pos[s][t[2]];
            if (boxed) {
                V3 ce = (m.v[t[0]] + m.v[t[1]] + m.v[t[2]]) * (1.0f / 3);   // the box is in the rest pose
                if (ce.x < bx[0] || ce.x > bx[1] || ce.y < bx[2] || ce.y > bx[3] || ce.z < bx[4] || ce.z > bx[5]) continue;
            }
            sampleTriangle(map, A, B, C);
        }
    }
    return true;
}

// tube: a long thin piece (sword hilt) fitted along its own main axis, radius per 1 cm slice. Hair is moved sideways off that
// axis, not away from the body axis (2026-10-15: the radial push threw all the back hair over the hilt into a wing)
struct Tube {
    V3 c{0, 0, 0}, ax{0, 0, 1};
    float t0 = 0, dt = 0.01f;
    std::vector<float> R;
    bool fit(const std::vector<V3>& v) {
        if (v.size() < 8) return false;
        V3 m{0, 0, 0};
        for (auto& p : v) m = m + p;
        c = m * (1.0f / v.size());
        double C[3][3] = {};
        for (auto& p : v) { V3 d = p - c; float e[3] = {d.x, d.y, d.z}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) C[i][j] += e[i] * e[j]; }
        double x[3] = {0.3, 0.3, 1};   // power iteration: main axis
        for (int it = 0; it < 60; it++) {
            double y[3] = {0, 0, 0};
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) y[i] += C[i][j] * x[j];
            double n = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
            if (n < 1e-12) return false;
            for (int i = 0; i < 3; i++) x[i] = y[i] / n;
        }
        ax = {float(x[0]), float(x[1]), float(x[2])};
        float lo = 1e9f, hi = -1e9f;
        for (auto& p : v) { float t = dot(p - c, ax); lo = std::min(lo, t); hi = std::max(hi, t); }
        t0 = lo;
        R.assign(size_t((hi - lo) / dt) + 1, 0.0f);
        for (auto& p : v) { float t = dot(p - c, ax); size_t b = std::min(R.size() - 1, size_t((t - t0) / dt)); R[b] = std::max(R[b], len(p - c - ax * t)); }
        return true;
    }
    // how far p must move off the axis to clear the piece + pad (0 = clear)
    float under(V3 p, float pad) const {
        float t = dot(p - c, ax);
        float f = (t - t0) / dt;
        if (f < -pad / dt || f > R.size() + pad / dt) return 0;
        int b = std::clamp(int(std::floor(f)), 0, int(R.size()) - 1);
        float r = 0;
        for (int k = b - 1; k <= b + 1; k++) if (k >= 0 && k < (int)R.size()) r = std::max(r, R[k]);
        float need = r + pad - len(p - c - ax * t);
        return need > 1e-4f ? need : 0;
    }
    // side: 0 = straight off the axis (neighbour strands can split to both sides -> in-between hair through the piece);
    // +1 = every strand passes on the BODY side (between the back / head and the piece), -1 = all on the outer side
    int side = 0;
    float bx = 0, by = 0;   // body axis (x, y)
    V3 push(V3 p, float pad) const {   // displacement that clears p
        float t = dot(p - c, ax);
        float f = (t - t0) / dt;
        int b = std::clamp(int(std::floor(f)), 0, int(R.size()) - 1);
        float r = 0;
        for (int k = b - 1; k <= b + 1; k++) if (k >= 0 && k < (int)R.size()) r = std::max(r, R[k]);
        r += pad;
        V3 v = p - c - ax * t;   // offset from the axis
        if (side == 0) { float l = len(v); return l >= r ? V3{0, 0, 0} : out(p) * (r - l); }
        V3 toBody = V3{bx, by, p.z} - (c + ax * t);
        V3 d = toBody - ax * dot(toBody, ax);
        d = unit(d) * float(side);
        float vd = dot(v, d), disc = vd * vd - dot(v, v) + r * r;
        if (disc <= 0) return {0, 0, 0};
        float s = -vd + std::sqrt(disc);   // move along d until |v + s d| = r
        return s > 0 ? d * s : V3{0, 0, 0};
    }
    V3 out(V3 p) const {
        V3 d = p - c - ax * dot(p - c, ax);
        return len(d) > 1e-6f ? d * (1.0f / len(d)) : unit(cross(ax, {0, 0, 1}));
    }
};

struct Test {
    const RadialMap& map;
    float pad, maxPush;
    const Tube* tube = nullptr;
    bool shell = false;   // thin pieces (sword, scabbard): only points between the inner and outer surface are inside; the space
                          // between the body axis and the piece is free (2026-10-15: the solid test pushed all the back hair over the hilt)
    // 0 = clear, else how far (m) the point is under the surface + pad (0 < v <= maxPush); too-deep points count as clear
    float under(V3 p) const {
        if (tube) { float n = tube->under(p, pad); return n <= maxPush ? n : 0; }
        float ro = map.query(p);
        if (ro < 0) return 0;
        float rp = std::hypot(p.x - map.cx, p.y - map.cy);
        if (shell) { float ri = map.queryIn(p); if (ri < 0 || rp < ri - pad) return 0; }
        float need = ro + pad - rp;
        return need > 1e-4f && need <= maxPush ? need : 0;
    }
    bool segOk(V3 a, V3 b) const {
        if (under(b) > 0) return false;
        for (int i = 1; i <= 3; i++) if (under(a + (b - a) * (i / 4.0f)) > 0) return false;
        return true;
    }
};

float arcTo(const Guide& G, int k) {
    float s = 0;
    for (int i = 1; i <= k; i++) s += len(G[i] - G[i - 1]);
    return s;
}

// Drape one guide. The strand is densified (a point every ds), then relaxed as a chain: segment lengths are kept (position based
// constraints, root part fixed) while every point is kept outside the piece (radial projection to surface + pad). The strand
// therefore bends out BEFORE the edge of a roll and lies over it, with its tip pulled up by the extra path. The CVs are read back
// at their original arc lengths. -> true when the guide changed
bool drapeGuide(const Test& T, const RadialMap& map, const Guide& G, float rootM, int iters, float ds, Guide& out, int& moved, float& lenChange, float& residual) {
    const int K = (int)G.size();
    // any CV or span sample under the piece after the root part?
    int k0 = 1;
    while (k0 < K - 1 && arcTo(G, k0) < rootM) k0++;
    bool hit = false;
    for (int k = k0; k < K && !hit; k++) hit = !T.segOk(G[k - 1], G[k]);
    if (!hit) return false;
    // dense polyline
    std::vector<V3> P;
    std::vector<float> S, cvArc(K, 0.0f);
    std::vector<int> cvAt(K, 0);
    P.push_back(G[0]); S.push_back(0);
    for (int k = 1; k < K; k++) {
        float L = len(G[k] - G[k - 1]);
        int n = std::max(1, (int)std::ceil(L / ds));
        for (int j = 1; j <= n; j++) { P.push_back(G[k - 1] + (G[k] - G[k - 1]) * (float(j) / n)); S.push_back(S.back() + L / n); }
        cvAt[k] = (int)P.size() - 1;
        cvArc[k] = S.back();
    }
    const int N = (int)P.size();
    std::vector<char> fixedP(N, 0);
    for (int i = 0; i < N; i++) if (S[i] <= rootM + 1e-6f) fixedP[i] = 1;
    std::vector<float> rest(N, 0.0f);
    for (int i = 1; i < N; i++) rest[i] = S[i] - S[i - 1];
    auto project = [&](V3& p) -> bool {
        float need = T.under(p);
        if (need <= 0) return false;
        if (T.tube) { p = p + T.tube->push(p, T.pad); return true; }
        V3 up = unit({p.x - map.cx, p.y - map.cy, 0}, {0, -1, 0});
        p = p + up * need;
        return true;
    };
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < N; i++) if (!fixedP[i]) project(P[i]);
        for (int sweep = 0; sweep < 2; sweep++) {
            for (int q = 1; q < N; q++) {
                int i = sweep == 0 ? q : N - q;   // forward then backward sweeps
                V3 a = P[i - 1], b = P[i];
                V3 d = b - a;
                float l = len(d);
                if (l < 1e-9f) continue;
                float corr = (l - rest[i]) / l;
                bool fa = fixedP[i - 1], fb = fixedP[i];
                if (fa && fb) continue;
                if (fa) P[i] = b - d * corr;
                else if (fb) P[i - 1] = a + d * corr;
                else { P[i - 1] = a + d * (0.5f * corr); P[i] = b - d * (0.5f * corr); }
            }
        }
    }
    residual = 0;
    for (int i = 0; i < N; i++) if (!fixedP[i]) { float need = T.under(P[i]); residual = std::max(residual, need); project(P[i]); }
    // new polyline arc lengths -> CVs at their original arc positions
    std::vector<float> S2(N, 0.0f);
    for (int i = 1; i < N; i++) S2[i] = S2[i - 1] + len(P[i] - P[i - 1]);
    out = G;
    moved = 0;
    lenChange = 0;
    int seg = 1;
    for (int k = 1; k < K; k++) {
        float target = cvArc[k];
        if (cvArc[k] <= rootM + 1e-6f) continue;   // fixed root part keeps its CVs
        // the last point may be shorter after relaxation: clamp to the end
        while (seg < N - 1 && S2[seg] < target) seg++;
        float s0 = S2[seg - 1], s1 = S2[seg];
        float f = s1 > s0 ? std::clamp((target - s0) / (s1 - s0), 0.0f, 1.0f) : 1.0f;
        V3 q = P[seg - 1] + (P[seg] - P[seg - 1]) * f;
        if (len(q - out[k]) > 1e-5f) moved++;
        out[k] = q;
    }
    float L0 = 0, L1 = 0;
    for (int k = 1; k < K; k++) { L0 += len(G[k] - G[k - 1]); L1 += len(out[k] - out[k - 1]); }
    lenChange = L0 > 1e-9f ? std::fabs(L1 - L0) / L0 * 100 : 0;
    return moved > 0;
}

json r4v(V3 q) { return {std::round(q.x * 1e4f) / 1e4f, std::round(q.y * 1e4f) / 1e4f, std::round(q.z * 1e4f) / 1e4f}; }

float capDistSigned(V3 p, V3 a, V3 b, float ra, float rb) {
    V3 ab = b - a;
    float l2 = dot(ab, ab);
    float t = l2 > 1e-12f ? std::clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return len(p - (a + ab * t)) - (ra * (1 - t) + rb * t);
}

void settle(App& a) {
    a.motion.preset = MotionPreset::Rest;
    a.teleport = true;
    for (int i = 0; i < 30; i++) a.step(1.0 / 60.0);
}

}  // namespace

// list of drapeable meshes: [{i, name, tex, kind, tris, z0, z1}]
json App::drapeList() const {
    json l = json::array();
    for (size_t i = 0; i < pack.meshes.size(); i++) {
        const Mesh& m = pack.meshes[i];
        if (!drapeCandidate(m)) continue;
        float z0 = 1e9f, z1 = -1e9f, y0 = 1e9f;
        for (auto& p : m.v) { z0 = std::min(z0, p.z); z1 = std::max(z1, p.z); y0 = std::min(y0, p.y); }
        l.push_back({{"i", (int)i}, {"name", m.name}, {"tex", m.texFile}, {"kind", m.kind}, {"tris", (int)m.t.size()},
                     {"z_range_m", {std::round(z0 * 1000) / 1000, std::round(z1 * 1000) / 1000}}, {"min_y_m", std::round(y0 * 1000) / 1000}, {"hood", hoodChunk(m)}});
    }
    return l;
}

json App::drapeCommand(const std::string& d, const json& c) {
    json r = {{"do", d}};
    if (!loaded || fur == 1) { r["error"] = "open a build in Hair mode first"; return r; }
    if (d == "list_meshes") { r["meshes"] = drapeList(); r["note"] = "meshes: indices, name / texture / path substrings, 'hood' (chunks with the modCiri hood roll), 'outfit', 'all'"; return r; }
    json want = c.contains("meshes") && c["meshes"].is_array() ? c["meshes"] : json::array();
    if (want.empty()) { r["error"] = "meshes: [] is empty (see studio_list_meshes)"; r["available"] = drapeList(); return r; }
    Selection sel = pickMeshes(*this, want);
    if (!sel.error.empty()) { r["error"] = sel.error; r["available"] = drapeList(); return r; }
    r["meshes"] = sel.names;

    const float pad = c.value("pad_mm", 3.0f) / 1000, rootM = c.value("root_mm", 20.0f) / 1000, maxPush = c.value("max_push_mm", 150.0f) / 1000;
    const std::string where = c.value("where", std::string());   // reserved: guide selector object is read below

    // ---- report: how many guide points are inside / under the pieces (rest, or the simulation read back)
    if (d == "drape_report") {
        const std::string mode = c.value("mode", std::string("rest"));
        const bool sim = mode == "sim";
        RadialMap map;
        std::string err;
        json inf;
        long cvIn = 0, cvPad = 0, smpIn = 0, tot = 0;
        std::set<int> gIn, gPad, g5, g10, g20;
        long hist[6] = {0, 0, 0, 0, 0, 0};   // inside depth: <2, 2-5, 5-10, 10-20, 20-40, >40 mm
        float worst = 0;
        std::map<int, long> azBand;
        std::map<int, long> zBand;            // deep (>= 20 mm) points per 5 cm height band, for diagnosis
        std::vector<json> deepEx;
        int curJ = -1;
        std::vector<V3> cc;   // collider centres in the tested pose
        auto radOf = [&](int i) { return colR.count(i) ? colR[i] : pack.spheres[i].r; };
        long capHist[5] = {0, 0, 0, 0, 0};   // deep (>= 20 mm) points by signed distance to the nearest capsule: < -3, -3..3, 3..10, 10..30, > 30 mm
        auto count = [&](int g, V3 p, const RadialMap& mp) {
            float ro = mp.query(p);
            tot++;
            if (ro < 0) return;
            float need0 = ro - std::hypot(p.x - mp.cx, p.y - mp.cy), need1 = need0 + pad;
            if (need0 > 1e-4f && need0 <= maxPush) {
                cvIn++; gIn.insert(g); worst = std::max(worst, need0);
                float mm = need0 * 1000;
                hist[mm < 2 ? 0 : mm < 5 ? 1 : mm < 10 ? 2 : mm < 20 ? 3 : mm < 40 ? 4 : 5]++;
                if (mm >= 5) g5.insert(g);
                if (mm >= 10) g10.insert(g);
                if (mm >= 20 && !cc.empty()) {
                    float best = 1e9f;
                    for (auto& cp : pack.capsules) best = std::min(best, capDistSigned(p, cc[cp[0]], cc[cp[1]], radOf(cp[0]), radOf(cp[1])));
                    float bm = best * 1000;
                    capHist[bm < -3 ? 0 : bm < 3 ? 1 : bm < 10 ? 2 : bm < 30 ? 3 : 4]++;
                }
                if (mm >= 20) { g20.insert(g); zBand[(int)std::floor(p.z * 20)]++; azBand[(int)std::floor(std::atan2(p.y - mp.cy, p.x - mp.cx) * 180 / 3.14159265f / 30) * 30]++; if (deepEx.size() < 6 && (tot % 997) == 0) deepEx.push_back({{"guide", g}, {"p", {std::round(p.x * 1e3f) / 1e3f, std::round(p.y * 1e3f) / 1e3f, std::round(p.z * 1e3f) / 1e3f}}, {"depth_mm", std::round(mm * 10) / 10}, {"j", curJ},
                    {"authored_cv_at_j", (curJ >= 0 && groom.K > 1) ? json(r4v(groom.renderPoint(g, std::min(groom.K - 1, curJ / std::max(1, pack.splineMultiplier))))) : json()}}); }
            }
            if (need1 > 1e-4f && need1 <= maxPush) { cvPad++; gPad.insert(g); }
        };
        if (!sim) {
            if (!buildMap(*this, sel, false, c, map, &err)) { r["error"] = err; return r; }
            cc.resize(pack.spheres.size());
            for (size_t i = 0; i < cc.size(); i++) cc[i] = pack.spheres[i].hasC ? pack.spheres[i].c : colLocalToWorld(pack.spheres[i].bone, pack.spheres[i].local);
            for (size_t g = 0; g < groom.P.size(); g++) {
                if (!groom.alive[g]) continue;
                const Guide& G = groom.P[g];
                int k0 = 1;
                while (k0 < (int)G.size() - 1 && arcTo(G, k0) < rootM) k0++;
                for (int k = k0; k < (int)G.size(); k++)
                    for (int s = (k == k0 ? 0 : 1); s <= 3; s++) count((int)g, G[k - 1] + (G[k] - G[k - 1]) * (s / 3.0f), map);   // CVs + 2 samples per span
            }
            r["points"] = "CVs after the root part + 2 samples per span (edit frame, rest)";
        } else {
            std::string pn = c.value("preset", std::string("Rest (no motion)"));
            MotionPreset pre = motion.preset;
            for (int pi = 0; pi < (int)MotionPreset::Count; pi++) {
                std::string a1 = lower(motionName((MotionPreset)pi)), b1 = lower(pn);
                a1.erase(std::remove(a1.begin(), a1.end(), ' '), a1.end());
                b1.erase(std::remove(b1.begin(), b1.end(), ' '), b1.end());
                if (!b1.empty() && (a1 == b1 || a1.rfind(b1, 0) == 0)) pre = (MotionPreset)pi;
            }
            const int steps = std::clamp(c.value("steps", 240), 10, 3000);
            bool sSim = simulate, sPaused = paused, sWind = windOn;
            Motion sMotion = motion;
            double sAcc = simAcc;
            rebuildHair();
            simulate = true; paused = false; windOn = c.value("wind", 0.0f) > 0;
            motion.preset = pre; motion.speed = 1; motion.amount = c.value("amount", 1.0f); motion.windStrength = c.value("wind", 0.0f); motion.t = 0; simAcc = 0; teleport = true;
            std::vector<float> xyz;
            int PG = 0, frames = 0;
            const size_t NA = guideOfAsset.size();
            bool ok = true;
            for (int s = 0; s < steps && ok; s++) {
                step(1.0 / 60.0);
                if (s < steps / 2) continue;
                if (s % 6 != 0 && s != steps - 1) continue;
                int pg = PG;
                if (!hw.readGuides(xyz, pg) || pg <= 0 || xyz.size() < NA * size_t(pg) * 3) { ok = false; r["error"] = "guide read-back failed"; break; }
                PG = pg;
                RadialMap mp;
                cc.resize(pack.spheres.size());
                for (size_t i = 0; i < cc.size(); i++) cc[i] = colPosed((int)i);
                if (!buildMap(*this, sel, true, c, mp, &err)) { ok = false; r["error"] = err; break; }
                frames++;
                if (frames == 1 || s == steps - 1) {   // diagnostic: simulated CVs against the authored CVs (same frame?)
                    double sum = 0, mx = 0; long n = 0;
                    const int sm = std::max(1, pack.splineMultiplier);
                    for (size_t a2 = 0; a2 < NA; a2 += 7) {
                        int g = guideOfAsset[a2];
                        for (int k = 1; k < groom.K && k * sm < PG; k++) {
                            size_t q = (a2 * PG + k * sm) * 3;
                            V3 rp = groom.renderPoint(g, k);
                            float dd = len(V3{xyz[q], xyz[q + 1], xyz[q + 2]} - rp);
                            sum += dd; mx = std::max<double>(mx, dd); n++;
                        }
                    }
                    inf["sim_vs_authored_mm" + std::string(frames == 1 ? "_first" : "_last")] = {{"mean", std::round(sum / std::max<long>(1, n) * 1e4) / 10}, {"max", std::round(mx * 1e4) / 10}};
                }
                for (size_t a2 = 0; a2 < NA; a2++) {
                    int g = guideOfAsset[a2];
                    for (int j = std::max(1, PG * 2 / 5); j < PG; j++) {
                        size_t q = (a2 * PG + j) * 3;
                        curJ = j;
                        count(g, {xyz[q], xyz[q + 1], xyz[q + 2]}, mp);
                    }
                }
            }
            rebuildHair();
            motion = sMotion; simulate = sSim; paused = sPaused; windOn = sWind; simAcc = sAcc; teleport = true; aoRefreshAll = true;
            inf["preset"] = motionName(pre); inf["steps"] = steps; inf["frames_tested"] = frames; inf["cvs_per_guide"] = PG;
            r["sim"] = inf;
            r["points"] = "refined strand vertices (lower 60 %), each tested frame of the second half of the run";
            if (!ok) return r;
        }
        r["mode"] = mode;
        r["pad_mm"] = pad * 1000;
        r["guides_inside"] = (int)gIn.size();
        r["guides_inside_or_within_pad"] = (int)gPad.size();
        if (c.value("ids", false)) {   // guide ids per depth class, for targeted fixes (trim / reshape / stiffness)
            r["ids_deeper_5mm"] = std::vector<int>(g5.begin(), g5.end());
            r["ids_deeper_10mm"] = std::vector<int>(g10.begin(), g10.end());
            r["ids_deeper_20mm"] = std::vector<int>(g20.begin(), g20.end());
        }
        r["points_inside"] = (int)cvIn;
        r["points_inside_or_within_pad"] = (int)cvPad;
        r["points_tested"] = (int)tot;
        r["inside_depth_hist_points"] = {{"lt2mm", hist[0]}, {"2to5mm", hist[1]}, {"5to10mm", hist[2]}, {"10to20mm", hist[3]}, {"20to40mm", hist[4]}, {"gt40mm", hist[5]}};
        { json zb = json::object(); for (auto& [k, v] : zBand) zb[std::to_string(k * 5) + "cm"] = v; r["deep20_points_by_z_cm"] = zb; r["deep20_examples"] = deepEx; json ab = json::object(); for (auto& [k, v] : azBand) ab[std::to_string(k) + "deg"] = v; r["deep20_points_by_azimuth"] = ab; }
        r["deep20_vs_capsule_mm"] = {{"inside_capsule_lt_-3", capHist[0]}, {"on_surface_-3_3", capHist[1]}, {"3_10_outside", capHist[2]}, {"10_30_outside", capHist[3]}, {"gt_30_outside", capHist[4]}};
        r["guides_deeper_5mm"] = (int)g5.size();
        r["guides_deeper_10mm"] = (int)g10.size();
        r["guides_deeper_20mm"] = (int)g20.size();
        r["deepest_mm"] = std::round(worst * 1e4f) / 10;
        r["status"] = nullptr;
        return r;
    }

    // ---- shell colliders: chains of spheres that fill the shell of the selected meshes along an azimuth range, one chain per height
    if (d == "shell_colliders") {
        RadialMap gm;
        std::string err2;
        if (!buildMap(*this, sel, false, c, gm, &err2)) { r["error"] = err2; return r; }
        std::vector<float> rows;
        if (c.contains("rows_z") && c["rows_z"].is_array()) for (auto& v : c["rows_z"]) rows.push_back(v.get<float>());
        if (rows.empty()) { r["error"] = "rows_z: [heights in m] (e.g. [1.51])"; return r; }
        const float az0 = c.value("az_from", -165.0f), az1 = c.value("az_to", -20.0f), stepDeg = std::max(5.0f, c.value("step_deg", 30.0f));
        const float spad = c.value("pad_mm", 2.0f) / 1000, maxR = c.value("max_r_mm", 60.0f) / 1000, minR = c.value("min_r_mm", 8.0f) / 1000;
        int bone = pack.boneIndex(c.value("bone", std::string("torso3")));
        if (bone < 0) { r["error"] = "bone is not in this fur"; return r; }
        struct NS { V3 cen; float R; int row; };
        std::vector<NS> ns;
        for (size_t ri = 0; ri < rows.size(); ri++) {
            int n = (int)std::ceil((az1 - az0) / stepDeg);
            for (int k = 0; k <= n; k++) {
                float a = n > 0 ? az0 + (az1 - az0) * k / n : az0, rad = a * 3.14159265f / 180;
                V3 probe{gm.cx + std::cos(rad) * 0.15f, gm.cy + std::sin(rad) * 0.15f, rows[ri]};
                float ro = -1, rinr = 1e9f;
                for (float rr = 0.03f; rr < 0.35f; rr += 0.002f) {   // radial scan: outermost / innermost surface at this azimuth and height
                    V3 q{gm.cx + std::cos(rad) * rr, gm.cy + std::sin(rad) * rr, rows[ri]};
                    float o = gm.query(q), in = gm.queryIn(q);
                    if (o < 0) continue;
                    ro = std::max(ro, o); rinr = std::min(rinr, in);
                }
                (void)probe;
                if (ro < 0) continue;
                float outer = ro + spad, R = std::clamp((outer - rinr) * 0.5f, minR, maxR), cr = outer - R;
                ns.push_back({{gm.cx + std::cos(rad) * cr, gm.cy + std::sin(rad) * cr, rows[ri]}, R, (int)ri});
            }
        }
        if (ns.empty()) { r["error"] = "none of the rows meets the meshes"; return r; }
        std::set<int> del;
        const std::string rtag = lower(c.contains("replace") ? c["replace"].value("tag", std::string()) : std::string());
        const float rztol = (c.contains("replace") ? c["replace"].value("z_tol_mm", 15.0f) : 15.0f) / 1000;
        if (!rtag.empty())
            for (int i = 0; i < (int)pack.spheres.size(); i++) {
                const Sphere& sp = pack.spheres[i];
                if (lower(sp.tag).find(rtag) == std::string::npos) continue;
                V3 cen = sp.hasC ? sp.c : colLocalToWorld(sp.bone, sp.local);
                for (float zr : rows) if (std::fabs(cen.z - zr) <= rztol) del.insert(i);
            }
        int after = (int)pack.spheres.size() - (int)del.size() + (int)ns.size();
        r["new_spheres"] = (int)ns.size(); r["replaced_spheres"] = (int)del.size(); r["spheres_after"] = after;
        if (after > 85) { r["error"] = "the sphere limit (85) would be exceeded (" + std::to_string(after) + ")"; return r; }
        if (c.value("list_only", false)) { json l = json::array(); for (auto& e : ns) l.push_back({{"row", e.row}, {"c_mm", {std::round(e.cen.x * 1e4f) / 10, std::round(e.cen.y * 1e4f) / 10, std::round(e.cen.z * 1e4f) / 10}}, {"r_mm", std::round(e.R * 1e4f) / 10}}); r["spheres"] = l; return r; }
        colPushUndo();
        colRemove(del);
        std::vector<int> made;
        int lastRow = -1, prev = -1;
        for (auto& e : ns) {
            int i = colAddSphere(bone, e.cen, e.R, 2, "shell", false);
            if (e.row == lastRow && prev >= 0) pack.capsules.push_back({prev, i});
            made.push_back(i); prev = i; lastRow = e.row;
        }
        // a chain of one sphere needs a twin to collide at all
        std::vector<char> inCap(pack.spheres.size(), 0);
        for (auto& q : pack.capsules) inCap[q[0]] = inCap[q[1]] = 1;
        for (int i : made) if (!inCap[i]) {
            if ((int)pack.spheres.size() + 1 > 85) break;
            Sphere t = pack.spheres[i]; t.local.x += 0.001f; t.tag = "shell (twin)";
            pack.spheres.push_back(t); colRecompute((int)pack.spheres.size() - 1);
            pack.capsules.push_back({i, (int)pack.spheres.size() - 1});
        }
        colChanged();
        rebuildHair();
        r["applied"] = true;
        r["spheres"] = (int)pack.spheres.size(); r["capsules"] = (int)pack.capsules.size();
        return r;
    }

    // ---- grow colliders (fill the shell): the chosen back-side spheres are re-centred between the inner and the outer surface of the
    // selected meshes (same azimuth and height) and given the radius that spans both, so hair cannot get into the cloth volume
    // (a thin sheet of small spheres on the back surface lets hair fall in over the top edge of a rolled hood)
    if (d == "grow_colliders") {
        RadialMap gm;
        std::string err2;
        if (!buildMap(*this, sel, false, c, gm, &err2)) { r["error"] = err2; return r; }
        const float gpad = c.value("pad_mm", 2.0f) / 1000, maxR = c.value("max_r_mm", 60.0f) / 1000, minR = c.value("min_r_mm", 8.0f) / 1000;
        std::set<int> only;
        bool explicitList = c.contains("spheres") && c["spheres"].is_array();
        if (explicitList) for (auto& v : c["spheres"]) only.insert(v.get<int>());
        const std::string tagWant = lower(c.value("tag", std::string()));
        if (!explicitList && tagWant.empty() && !c.contains("sphere_box")) { r["error"] = "pick the spheres: spheres [indices], tag (the fit target, e.g. 'hood') or sphere_box [xmin,xmax,ymin,ymax,zmin,zmax]"; return r; }
        float sb[6] = {0, 0, 0, 0, 0, 0};
        const bool boxed = c.contains("sphere_box") && c["sphere_box"].is_array() && c["sphere_box"].size() == 6;
        if (boxed) for (int i = 0; i < 6; i++) sb[i] = c["sphere_box"][i].get<float>();
        json changes = json::array();
        struct Ch { int i; V3 cen; float r; };
        std::vector<Ch> ch;
        for (int i = 0; i < (int)pack.spheres.size(); i++) {
            const Sphere& sp = pack.spheres[i];
            V3 cen = sp.hasC ? sp.c : colLocalToWorld(sp.bone, sp.local);
            if (explicitList && !only.count(i)) continue;
            if (!explicitList && !tagWant.empty() && lower(sp.tag).find(tagWant) == std::string::npos) continue;
            if (!explicitList && boxed && (cen.x < sb[0] || cen.x > sb[1] || cen.y < sb[2] || cen.y > sb[3] || cen.z < sb[4] || cen.z > sb[5])) continue;
            float rad = colR.count(i) ? colR[i] : sp.r;
            float ro = gm.query(cen), ri = gm.queryIn(cen);
            if (ro < 0 || ri < 0) continue;
            float outer = ro + gpad, inner = ri;
            float R = std::clamp((outer - inner) * 0.5f, minR, maxR), cr = outer - R;   // the outer surface stays touched
            V3 dir = unit({cen.x - gm.cx, cen.y - gm.cy, 0}, {0, -1, 0});
            V3 nc = {gm.cx + dir.x * cr, gm.cy + dir.y * cr, cen.z};
            ch.push_back({i, nc, R});
            changes.push_back({{"sphere", i}, {"bone", pack.bones[sp.bone].name}, {"tag", sp.tag}, {"r_mm", std::round(rad * 1e4f) / 10}, {"new_r_mm", std::round(R * 1e4f) / 10},
                               {"moved_mm", std::round(len(nc - cen) * 1e4f) / 10}, {"surface_mm", {std::round(ri * 1e4f) / 10, std::round(ro * 1e4f) / 10}}});
        }
        r["pad_mm"] = gpad * 1000;
        r["changed"] = (int)ch.size();
        r["changes"] = changes;
        if (!ch.empty() && !c.value("list_only", false)) {
            colPushUndo();
            for (auto& e : ch) {
                Sphere& sp = pack.spheres[e.i];
                sp.local = colWorldToLocal(sp.bone, e.cen);
                sp.r = e.r;
                colR.erase(e.i);
                if (sp.grp != 2 && sp.grp >= 0) { sp.grp = 2; if (sp.tag.empty()) sp.tag = "edited"; }
                colRecompute(e.i);
            }
            colChanged();
            rebuildHair();
            r["applied"] = true;
        }
        return r;
    }

    // ---- drape
    RadialMap map;
    std::string err;
    if (!buildMap(*this, sel, false, c, map, &err)) { r["error"] = err; return r; }
    Test T{map, pad, maxPush};
    {   // shell: explicit, else on when every selected piece is a sword / scabbard
        bool thin = !sel.names.empty();
        for (auto& n : sel.names) { std::string l = lower(n); if (l.find("sword") == std::string::npos && l.find("scabbard") == std::string::npos) thin = false; }
        T.shell = c.value("shell", thin);
        r["shell"] = T.shell;
    }
    Tube tube;   // opt-in "tube": fit the pieces as one long thin piece, move hair sideways off its axis
    if (c.value("tube", false)) {
        std::vector<V3> v;
        for (int mi : sel.idx) { auto q = meshPositions(*this, mi, false); v.insert(v.end(), q.begin(), q.end()); }
        if (!tube.fit(v)) { r["error"] = "tube: could not fit the pieces"; return r; }
        std::string sd = c.value("tube_side", std::string("axis"));
        tube.side = sd == "body" ? 1 : sd == "outer" ? -1 : 0;
        tube.bx = map.cx; tube.by = map.cy;
        T.tube = &tube;
        r["tube_side"] = sd;
        float rmax = 0;
        for (float x : tube.R) rmax = std::max(rmax, x);
        r["tube"] = {{"axis", r4v(tube.ax)}, {"centre", r4v(tube.c)}, {"length_mm", std::round(tube.R.size() * tube.dt * 1000)}, {"max_r_mm", std::round(rmax * 1000)}};
    }
    auto ix = selectGuides(c.contains("where") && c["where"].is_object() ? c["where"] : json::object({{"all", true}}));
    int id = 0, grp = 1000 + (++grpId), nG = 0, nPts = 0;
    for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    std::vector<json> ops;
    float shiftMax = 0, lenChangePct = 0, resMax = 0;
    const int iters = std::clamp(c.value("iterations", 120), 5, 2000);
    const float ds = std::clamp(c.value("ds_mm", 5.0f), 2.0f, 20.0f) / 1000;
    std::vector<std::pair<float, int>> worstList;
    for (int g : ix) {
        const Guide& G = groom.P[g];
        Guide Q;
        int moved = 0;
        float lc = 0, res = 0;
        if (!drapeGuide(T, map, G, rootM, iters, ds, Q, moved, lc, res)) continue;
        float sh = 0;
        for (size_t k = 0; k < G.size(); k++) sh = std::max(sh, len(Q[k] - G[k]));
        shiftMax = std::max(shiftMax, sh);
        lenChangePct = std::max(lenChangePct, lc);
        resMax = std::max(resMax, res);
        worstList.push_back({sh, g});
        json pts = json::array();
        for (auto& q : Q) pts.push_back(r4v(q));
        ops.push_back({{"op", "reshape"}, {"root", r4v(G[0])}, {"points", pts}, {"id", ++id}, {"grp", grp}});
        nG++; nPts += moved;
    }
    std::sort(worstList.begin(), worstList.end(), [](auto& x, auto& y) { return x.first > y.first; });
    json wl = json::array();
    for (size_t i = 0; i < worstList.size() && i < 8; i++) wl.push_back({{"guide", worstList[i].second}, {"shift_mm", std::round(worstList[i].first * 1e4f) / 10}});
    r["largest_shifts"] = wl;
    if (!c.value("list_only", false) && !ops.empty()) {
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged(); settle(*this);
    } else if (!ops.empty()) {
        --grpId;
    }
    // opt-in "split_faces" (tube only): growth faces whose IN-BETWEEN hair runs through the piece (its guides pass on both sides of
    // the hilt, HairWorks fans hair straight through it; user 2026-10-15 close-up of the crossguard) are removed -> a parting
    if (T.tube && c.value("split_faces", false)) {
        int fid = 0;
        for (auto& o : groom.ops) if (o.contains("id")) fid = std::max(fid, o["id"].get<int>());
        const float fpad = c.value("face_pad_mm", 0.0f) / 1000;
        Test TF{map, fpad, 1.0f, &tube};
        const float bw[7][3] = {{1/3.f, 1/3.f, 1/3.f}, {.5f, .5f, 0}, {0, .5f, .5f}, {.5f, 0, .5f}, {.6f, .2f, .2f}, {.2f, .6f, .2f}, {.2f, .2f, .6f}};
        std::vector<json> fops;
        std::map<int, int> hitZ;   // where the through-hair hits: height in cm -> faces
        const float zLo = c.value("face_zmin", -1e9f), zHi = c.value("face_zmax", 1e9f);
        for (auto& F : groom.faces) {
            const Guide& A = groom.P[F.g[0]]; const Guide& B = groom.P[F.g[1]]; const Guide& C = groom.P[F.g[2]];
            size_t K = std::min({A.size(), B.size(), C.size()});
            bool through = false;
            for (size_t k = 1; k < K && !through; k++)
                for (auto& w : bw) {
                    V3 q = A[k] * w[0] + B[k] * w[1] + C[k] * w[2];
                    // a guide itself inside does not count (drape handles guides); only hair between guides
                    if (q.z < zLo || q.z > zHi) continue;
                    if (TF.under(q) > 0 && TF.under(A[k]) <= 0 && TF.under(B[k]) <= 0 && TF.under(C[k]) <= 0) { through = true; hitZ[int(std::floor(q.z * 100))]++; break; }
                }
            if (!through) continue;
            json rr = json::array();
            for (int q = 0; q < 3; q++) rr.push_back(r4v(groom.P[F.g[q]][0]));
            fops.push_back({{"op", "delete_face"}, {"roots", rr}, {"id", ++fid}, {"grp", grp}});
        }
        r["faces_through"] = (int)fops.size();
        { json h = json::object(); for (auto& [z, n] : hitZ) h[std::to_string(z)] = n; r["faces_through_by_z_cm"] = h; }
        if (!c.value("list_only", false) && !fops.empty()) {
            for (auto& o : fops) groom.ops.push_back(o);
            groom.redo.clear(); groom.replay(); editsChanged(); settle(*this);
        }
    }
    r["guides_draped"] = nG;
    r["points_moved"] = nPts;
    r["guides_considered"] = (int)ix.size();
    r["max_shift_mm"] = std::round(shiftMax * 1e4f) / 10;
    r["max_length_change_pct"] = std::round(lenChangePct * 100) / 100;
    r["max_residual_mm"] = std::round(resMax * 1e4f) / 10;
    r["pad_mm"] = pad * 1000;
    r["root_mm"] = rootM * 1000;
    r["applied"] = !c.value("list_only", false) && !ops.empty();
    return r;
}
