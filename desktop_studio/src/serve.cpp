// Hairwork Studio - live headless session for the MCP bridge (desktop_studio/mcp/hairwork_studio_mcp.py).
// `HairworkStudio.exe --serve [--out <dir>]`: no window shown, no input. One JSON command per line on stdin, one
// "@@HWS {json}" reply line on stdout. Every --agent step works as a command (agentStep), plus the session commands
// below (open, render, guides, faces, densify, root_to_scalp, pick_region, undo / redo, baseline renders, ...).
// Our own code (Studio License).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <functional>
#include <random>
#include <unordered_map>

#include "app.h"

namespace fs = std::filesystem;
using namespace DirectX;

bool writePng(const std::string& path, int w, int h, const unsigned char* rgba);
unsigned char* loadImage(const std::string& path, int* w, int* h);
void freeImage(unsigned char* p);

static void reply(const json& j) {
    std::string s = "@@HWS " + j.dump() + "\n";
    DWORD n = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), DWORD(s.size()), &n, nullptr);
}

static bool readLine(std::string& line) {
    static std::string buf;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        size_t p = buf.find('\n');
        if (p != std::string::npos) {
            line = buf.substr(0, p);
            buf.erase(0, p + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        char tmp[65536];
        DWORD n = 0;
        if (!ReadFile(h, tmp, sizeof tmp, &n, nullptr) || n == 0) return false;
        buf.append(tmp, n);
    }
}

static void settleRest(App& a, double seconds) {
    a.motion.preset = MotionPreset::Rest;
    a.teleport = true;
    int n = std::max(1, int(seconds * 60.0 + 0.5));
    for (int i = 0; i < n; i++) a.step(1.0 / 60.0);
}

static float faceAreaMm2(const Groom& g, const Face& f) {
    V3 a = g.P[f.g[0]][0], b = g.P[f.g[1]][0], c = g.P[f.g[2]][0];
    return 0.5f * len(cross(b - a, c - a)) * 1e6f;
}

static json v3j(V3 v) { return {std::round(v.x * 1e4f) / 1e4f, std::round(v.y * 1e4f) / 1e4f, std::round(v.z * 1e4f) / 1e4f}; }

static float guideLen(const Guide& G) {
    float L = 0;
    for (size_t k = 1; k < G.size(); k++) L += len(G[k] - G[k - 1]);
    return L;
}

// camera of a command: "shot": id from the pack, "view": orbit preset, or explicit "eye" / "target" / "fovy"
static bool cameraOf(App& a, const json& c, json& cam) {
    if (c.contains("eye") && c.contains("target")) {
        cam = {{"eye", c["eye"]}, {"target", c["target"]}, {"fovy", c.value("fovy", 20.0f)}, {"view", "custom"}};
        return true;
    }
    if (c.value("view", std::string()) == "top") {
        // top-front camera from this character's head (works for short and tall characters alike)
        V3 t{a.pack.headC.x, a.pack.headC.y, a.pack.headTop - 0.03f};
        cam = {{"eye", {t.x, t.y + 0.45f, t.z + 0.39f}}, {"target", {t.x, t.y, t.z}}, {"fovy", 22.0f}, {"view", "top"}};
        return true;
    }
    if (c.contains("view") && c["view"].is_string() && !c.contains("shot")) {
        Camera keep = a.cam;
        a.setView(c["view"].get<std::string>());
        XMFLOAT3 e = a.cam.eye();
        cam = {{"eye", {e.x, e.y, e.z}}, {"target", {a.cam.target.x, a.cam.target.y, a.cam.target.z}}, {"fovy", a.cam.fov},
               {"view", c["view"]}};
        a.cam = keep;
        return true;
    }
    std::string k = std::to_string(c.value("shot", 3));
    if (!a.pack.shots.contains(k)) return false;
    cam = a.pack.shots[k];
    return true;
}

// every command may run on the build as opened ("baseline": true): the edits are set aside and restored afterwards
struct BaselineScope {
    App& a;
    bool on;
    std::vector<json> ops, redo;
    std::map<std::string, json> lookOver;   // the look too: "as opened" = the build's own colours / physics
    BaselineScope(App& app, bool b) : a(app), on(b) {
        if (!on) return;
        ops = a.groom.ops;
        redo = a.groom.redo;
        lookOver = a.look.over;
        a.look.over.clear();
        a.groom.ops.clear();
        a.groom.replay();
        a.editsChanged();
        settleRest(a, 0.5);
    }
    ~BaselineScope() {
        if (!on) return;
        a.groom.ops = ops;
        a.groom.redo = redo;
        a.look.over = lookOver;
        a.groom.replay();
        a.editsChanged();
        settleRest(a, 0.5);
    }
};


// ------------------------------------------------------------------------------------------------ hairline tools
static V3 nrmz(V3 v) { float l = len(v); return l > 1e-9f ? v * (1.0f / l) : V3{0, 0, 1}; }
static V3 rotAxis(V3 v, V3 k, float a) {   // Rodrigues, k unit
    float c = std::cos(a), s = std::sin(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}
// painted hairline of the head mesh: edges between a scalp vertex and a skin vertex (posed); inner = the scalp end
struct Hairline { std::vector<V3> pt, inner; std::vector<V3> pos; std::vector<float> flag; };
static bool hairlineOf(const App& a, Hairline& h) {
    std::vector<std::array<int, 3>> tris;
    if (!a.scalpMesh(h.pos, h.flag, tris)) return false;
    std::set<std::pair<int, int>> seen;
    for (auto& t : tris)
        for (int c = 0; c < 3; c++) {
            int i = t[c], j = t[(c + 1) % 3];
            if ((h.flag[i] > 0.5f) == (h.flag[j] > 0.5f)) continue;
            if (!seen.insert({std::min(i, j), std::max(i, j)}).second) continue;
            int in = h.flag[i] > 0.5f ? i : j;
            h.pt.push_back((h.pos[i] + h.pos[j]) * 0.5f);
            h.inner.push_back(h.pos[in]);
        }
    return !h.pt.empty();
}
static bool onScalp(const Hairline& h, V3 p) {
    int best = -1; float bd = 1e30f;
    for (size_t i = 0; i < h.pos.size(); i++) { float d = len(h.pos[i] - p); if (d < bd) { bd = d; best = int(i); } }
    return best >= 0 && h.flag[best] > 0.5f;
}
static int nearestHl(const Hairline& h, V3 p, float& d) {
    int best = -1; d = 1e30f;
    for (size_t i = 0; i < h.pt.size(); i++) { float q = len(h.pt[i] - p); if (q < d) { d = q; best = int(i); } }
    return best;
}
// arc length from the root to each control point
static std::vector<float> arcLen(const Guide& G) {
    std::vector<float> s(G.size(), 0.0f);
    for (size_t k = 1; k < G.size(); k++) s[k] = s[k - 1] + len(G[k] - G[k - 1]);
    return s;
}
// turn the root part of a guide around the scalp normal: segments starting within span_m turn by ang (fading out over
// the last 30 % of the span), later segments keep their direction, so the strand bends near the root and the rest is
// only shifted by about span * sin(ang) (no swinging tips)
static Guide turnRoot(const Guide& G, V3 nrm, float ang, float spanM) {
    auto s = arcLen(G);
    Guide Q = G;
    for (size_t k = 1; k < G.size(); k++) {
        float f = s[k - 1] / std::max(spanM, 1e-4f);
        float w = f <= 0.7f ? 1.0f : f >= 1.0f ? 0.0f : 1.0f - (f - 0.7f) / 0.3f;
        Q[k] = Q[k - 1] + rotAxis(G[k] - G[k - 1], nrm, ang * w);
    }
    return Q;
}
// move a guide's root by off, the offset fading to 0 at fade_m along the strand (the tip stays where it was)
static Guide moveRoot(const Guide& G, V3 off, float fadeM) {
    auto s = arcLen(G);
    Guide Q = G;
    for (size_t k = 0; k < G.size(); k++) Q[k] = G[k] + off * std::max(0.0f, 1.0f - s[k] / std::max(fadeM, 1e-4f));
    return Q;
}
static json ptsJson(const Guide& Q) {
    json pts = json::array();
    for (auto& q : Q) pts.push_back({r4(q.x), r4(q.y), r4(q.z)});
    return pts;
}
// root tangent of a guide (edit frame)
static V3 rootTangent(const App& a, int g);
// head surface as a radial map around the head centre (posed head mesh): surface radius toward a direction = largest
// vertex radius within ~10 degrees. Guide points closer to the centre than that (+ standoff) are inside the head.
struct HeadRadial {
    std::vector<V3> dir;
    std::vector<float> rad;
    V3 c;
    bool ok = false;
    float surface(V3 d) const {
        // median radius inside the cone: the skull, not the ear rims / nose that stick out of it
        std::vector<float> v;
        for (size_t i = 0; i < dir.size(); i++)
            if (dot(dir[i], d) > 0.996f) v.push_back(rad[i]);
        if (v.empty()) return -1;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    }
};
static HeadRadial headRadial(const App& a) {
    HeadRadial h;
    h.c = a.pack.headC;
    std::vector<V3> pos; std::vector<float> fl; std::vector<std::array<int, 3>> tr;
    if (!a.scalpMesh(pos, fl, tr)) return h;
    for (auto& p : pos) { V3 d = p - h.c; float r = len(d); if (r > 1e-4f) { h.dir.push_back(d * (1.0f / r)); h.rad.push_back(r); } }
    h.ok = !h.dir.empty();
    return h;
}
// push guide points (not the root) that are inside the head out to surface + standoff (edit frame ~ posed frame; the
// standoff covers the few mm between them). Returns the number of points moved.
static int pushOut(const HeadRadial& h, Guide& G, float standoff, float depth = 0.008f) {
    if (!h.ok) return 0;
    int n = 0;
    for (size_t k = 1; k < G.size(); k++) {
        V3 d = G[k] - h.c;
        float r = len(d);
        if (r < 1e-5f || G[k].z < h.c.z - 0.01f) continue;   // skull only (the head mesh also holds the neck / shoulders)
        V3 u = d * (1.0f / r);
        float s = h.surface(u);
        if (s > 0 && r < s - depth) n++;   // clearly inside (deeper than depth): the guide passes through the head
    }
    if (!n) return 0;
    for (size_t k = 1; k < G.size(); k++) {
        V3 d = G[k] - h.c;
        float r = len(d);
        if (r < 1e-5f || G[k].z < h.c.z - 0.01f) continue;
        V3 u = d * (1.0f / r);
        float s = h.surface(u);
        if (s > 0 && r < s + standoff) G[k] = h.c + u * (s + standoff);
    }
    return n;
}

// 2026-10-14 read-only: deepest dip (m) of the straight SPANS between CVs under the skin, sampled every ~2 mm. pushOut
// tests CVs only, so a long span (e.g. a 40 mm first segment across the forehead) can cut through the curved skull
// between two CVs that both sit outside it; HairWorks then draws that hair half hidden by the skin (broken specks).
static float spanDip(const HeadRadial& h, const Guide& G) {
    if (!h.ok) return 0;
    float worst = 0;
    for (size_t k = 1; k < G.size(); k++) {
        V3 a = G[k - 1], b = G[k];
        int n = std::max(1, (int)(len(b - a) / 0.002f));
        for (int j = 1; j < n; j++) {
            V3 p = a + (b - a) * ((float)j / n);
            V3 d = p - h.c;
            float r = len(d);
            if (r < 1e-5f || p.z < h.c.z - 0.01f) continue;
            float s = h.surface(d * (1.0f / r));
            if (s > 0 && s - r > worst) worst = s - r;
        }
    }
    return worst;
}

// how much the root part (first span_m) points away from the scalp: degrees above the tangent plane (frizz = high)
static float liftDeg(const App& a, int g, float spanM) {
    auto& G = a.groom.P[g];
    auto sl = arcLen(G);
    size_t k = 1;
    while (k + 1 < G.size() && sl[k] < spanM) k++;
    V3 n = nrmz(G[0] - a.pack.headC), d = nrmz(G[k] - G[0]);
    return std::asin(std::clamp(dot(d, n), -1.0f, 1.0f)) * 180 / 3.14159265f;
}
// guides with points in front of the face (eyes / nose / mouth box in front of the head): hair hanging over the face
static int overFace(const App& a, json* list = nullptr) {
    const V3 c = a.pack.headC;
    int n = 0;
    for (size_t g = 0; g < a.groom.P.size(); g++) {
        if (!a.groom.alive[g]) continue;
        for (auto& p : a.groom.P[g])
            if (p.y > c.y + 0.07f && std::fabs(p.x - c.x) < 0.045f && p.z < c.z + 0.055f && p.z > c.z - 0.09f) {
                n++;
                if (list && list->size() < 50) list->push_back({{"i", g}, {"root", v3j(a.groom.P[g][0])}});
                break;
            }
    }
    return n;
}
// new guide: template g copied to root q (put on the scalp at the template's root radius), root part turned toward
// want (strength), hugging the scalp over span_m; returns the duplicate + reshape ops
static void plantGuide(const App& a, int g, V3 q, V3 want, float strength, float spanM, float liftM, int& id, int grp, std::vector<json>& ops,
                       const HeadRadial* hr = nullptr, float maxLen = 0) {
    V3 rs = a.groom.P[g][0];
    q = a.pack.headC + nrmz(q - a.pack.headC) * len(rs - a.pack.headC);
    Guide G = a.groom.P[g];
    for (auto& p : G) p = p + (q - rs);
    V3 n2 = nrmz(q - a.pack.headC);
    want = want - n2 * dot(want, n2);
    if (len(want) > 1e-6f) {
        want = nrmz(want);
        V3 tg = rootTangent(a, g);
        float ang = std::clamp(std::atan2(dot(cross(tg, want), n2), dot(tg, want)) * strength, -1.2f, 1.2f);
        G = turnRoot(G, n2, ang, spanM);
    }
    auto sl = arcLen(G);
    float r0 = len(G[0] - a.pack.headC);
    for (size_t k = 1; k < G.size(); k++) {
        float f = sl[k] / std::max(spanM, 1e-4f);
        if (f >= 1.0f) break;
        V3 rel = G[k] - a.pack.headC;
        float rk = len(rel), rt = r0 + liftM * f;
        if (rk > rt) {
            V3 dl = a.pack.headC + nrmz(rel) * (rk + (rt - rk) * (1.0f - f)) - G[k];
            for (size_t j = k; j < G.size(); j++) G[j] = G[j] + dl * (j == k ? 1.0f : std::max(0.0f, 1.0f - (sl[j] - sl[k]) / std::max(spanM, 1e-4f)));
        }
    }
    if (hr) pushOut(*hr, G, 0.003f);   // a template copied to another root can pass through the head: keep it outside
    if (maxLen > 0 && guideLen(G) > maxLen) G = shortenGuide(G, maxLen / guideLen(G));   // fringes: no copies reaching the eyes
    V3 dd = q - rs;
    ops.push_back({{"op", "duplicate"}, {"root", {r4(rs.x), r4(rs.y), r4(rs.z)}}, {"d", {r4(dd.x), r4(dd.y), r4(dd.z)}}, {"id", ++id}, {"grp", grp}});
    ops.push_back({{"op", "reshape"}, {"root", {r4(G[0].x), r4(G[0].y), r4(G[0].z)}}, {"points", ptsJson(G)}, {"id", ++id}, {"grp", grp}});
}
// first quarter direction projected on the scalp tangent plane
static V3 rootTangent(const App& a, int g) {
    auto& G = a.groom.P[g];
    int k = std::max(1, int(G.size()) / 4);
    V3 n = nrmz(G[0] - a.pack.headC), d = G[k] - G[0];
    return nrmz(d - n * dot(d, n));
}

// ------------------------------------------------------------------------------------------------ rebuild_from_cards
// 2026-10-12 opt-in Studio tool: delete the guides of a region and trace new ones from the vanilla hair cards (same
// ideas as tools/hairworks/growth.py build: card_lanes -> visible_card_points -> sample_scalp -> guides -> faces_for).
// Lanes = iso-u lines of each UV island of a card, root = the v end nearer the scalp, trimmed where the texture alpha
// has no hair; roots = Poisson-disk points on the head mesh within reach of visible near-skin card hair; each root
// follows a lane passing near it; growth faces = Delaunay of the roots in the polar map around the head centre.
namespace rebuild {
struct Lane { std::vector<V3> p, raw; float L = 0; std::array<float, 2> uv0{}, uv1{}; };

static std::vector<V3> resampleN(const std::vector<V3>& q, int n, float* Lout = nullptr) {
    std::vector<float> d(q.size(), 0.0f);
    for (size_t i = 1; i < q.size(); i++) d[i] = d[i - 1] + len(q[i] - q[i - 1]);
    if (Lout) *Lout = d.back();
    std::vector<V3> out(n, q[0]);
    if (d.back() < 1e-9f || n < 2) return out;
    size_t j = 1;
    for (int k = 0; k < n; k++) {
        float t = d.back() * k / float(n - 1);
        while (j < q.size() - 1 && d[j] < t) j++;
        float a = std::clamp((t - d[j - 1]) / std::max(d[j] - d[j - 1], 1e-12f), 0.0f, 1.0f);
        out[k] = q[j - 1] + (q[j] - q[j - 1]) * a;
    }
    return out;
}

struct Grid {   // uniform hash grid over points
    float cs;
    std::unordered_map<long long, std::vector<int>> m;
    explicit Grid(float c) : cs(c) {}
    static long long key(int x, int y, int z) { return ((long long)(x + 8192) << 28) | ((long long)(y + 8192) << 14) | (long long)(z + 8192); }
    int cell(float f) const { return (int)std::floor(f / cs); }
    void add(V3 p, int i) { m[key(cell(p.x), cell(p.y), cell(p.z))].push_back(i); }
    template <class F> void around(V3 p, float r, F fn) const {
        int R = (int)std::ceil(r / cs);
        int cx = cell(p.x), cy = cell(p.y), cz = cell(p.z);
        for (int x = cx - R; x <= cx + R; x++) for (int y = cy - R; y <= cy + R; y++) for (int z = cz - R; z <= cz + R; z++) {
            auto it = m.find(key(x, y, z));
            if (it != m.end()) for (int i : it->second) fn(i);
        }
    }
};

// Bowyer-Watson Delaunay of 2D points -> triangles (indices)
static std::vector<std::array<int, 3>> delaunay(const std::vector<std::array<double, 2>>& pts) {
    struct T { int a, b, c; double cx, cy, r2; };
    std::vector<std::array<double, 2>> P = pts;
    double mnx = 1e30, mny = 1e30, mxx = -1e30, mxy = -1e30;
    for (auto& p : P) { mnx = std::min(mnx, p[0]); mny = std::min(mny, p[1]); mxx = std::max(mxx, p[0]); mxy = std::max(mxy, p[1]); }
    double dm = std::max(mxx - mnx, mxy - mny) * 20 + 1, mx = (mnx + mxx) / 2, my = (mny + mxy) / 2;
    int n = (int)P.size();
    P.push_back({mx - dm, my - dm}); P.push_back({mx + dm, my - dm}); P.push_back({mx, my + dm});
    auto mk = [&](int a, int b, int c) {
        double ax = P[a][0], ay = P[a][1], bx = P[b][0], by = P[b][1], cx = P[c][0], cy = P[c][1];
        double d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
        T t{a, b, c, 0, 0, 1e300};
        if (std::fabs(d) < 1e-18) return t;
        double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d;
        double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d;
        t.cx = ux; t.cy = uy; t.r2 = (ax - ux) * (ax - ux) + (ay - uy) * (ay - uy);
        return t;
    };
    std::vector<T> tri{mk(n, n + 1, n + 2)};
    for (int i = 0; i < n; i++) {
        double x = P[i][0], y = P[i][1];
        std::vector<T> keep;
        std::map<std::pair<int, int>, int> edges;
        for (auto& t : tri) {
            double dx = x - t.cx, dy = y - t.cy;
            if (dx * dx + dy * dy < t.r2) {
                int v[3] = {t.a, t.b, t.c};
                for (int e = 0; e < 3; e++) edges[{std::min(v[e], v[(e + 1) % 3]), std::max(v[e], v[(e + 1) % 3])}]++;
            } else keep.push_back(t);
        }
        for (auto& [e, cnt] : edges) if (cnt == 1) keep.push_back(mk(e.first, e.second, i));
        tri.swap(keep);
    }
    std::vector<std::array<int, 3>> out;
    for (auto& t : tri) if (t.a < n && t.b < n && t.c < n) out.push_back({t.a, t.b, t.c});
    return out;
}
// rebase2 (2026-10-12, opt-in): zone callbacks the dispatcher builds from t6::Zones (defined further down) when a
// rebuild uses exclude_ears / exclude_temples_mm / scalp_grow_mm; nullptr = old behaviour
struct Hooks {
    std::function<bool(V3)> excludeRoot;   // true = no root here (ear box + pad, temples)
    std::function<bool(V3)> forehead;      // true = front forehead (scalp_grow never grows there)
    std::function<int(Guide&)> post;       // per new strand: ears routing; returns 1 = changed, -1 = drop
    std::function<bool(const Guide&)> faceBlocked;   // interpolated strand (face centroid / edge middle) crosses an ear
};
}  // namespace rebuild

static json rebuildFromCards(App& a, const json& c, int& grpId, const rebuild::Hooks* hk = nullptr) {
    using namespace rebuild;
    json r;
    Groom& gr = a.groom;
    const DeskPack& pk = a.pack;
    const int K = gr.K;
    const V3 C = pk.headC;
    const json where = c.value("where", json::object({{"all", true}}));
    // ---- region: the selector keys of selectGuides that apply to a point (the root)
    auto inRegion = [&](V3 p) {
        float zr = p.z - pk.headTop;
        if (where.value("back", false) && !(p.y < C.y)) return false;
        if (where.value("front", false) && !(p.y >= C.y)) return false;
        if (where.value("left", false) && !(p.x > 0)) return false;
        if (where.value("right", false) && !(p.x <= 0)) return false;
        if (where.contains("z_min") && zr < where["z_min"].get<float>()) return false;
        if (where.contains("z_max") && zr > where["z_max"].get<float>()) return false;
        if (where.contains("near") && where["near"].is_array() && where["near"].size() == 3) {
            V3 q{where["near"][0].get<float>(), where["near"][1].get<float>(), where["near"][2].get<float>()};
            if (len(p - q) > where.value("radius", 0.01f)) return false;
        }
        return true;
    };
    // ---- params
    float laneW = c.value("lane_w_mm", 6.0f) / 1000, alphaTip = c.value("alpha_tip", 0.08f), alphaVis = c.value("alpha_vis", 0.3f);
    float reach = c.value("reach_mm", 12.0f) / 1000, nearM = c.value("near_mm", 12.0f) / 1000, maxd = c.value("max_dist_mm", 30.0f) / 1000;
    float minRem = c.value("min_length_mm", 30.0f) / 1000, keepOff = std::clamp(c.value("offset_keep", 0.3f), 0.0f, 1.0f);
    float zminRel = c.value("zmin_rel", 0.20f), hugK = std::clamp(c.value("hug", 0.35f), 0.0f, 1.0f);
    float dedupe = c.value("dedupe_mm", 3.0f) / 1000;
    float spacing = c.value("spacing_mm", 9.0f) / 1000;
    bool replace = c.value("replace", true), balanced = c.value("pick", std::string("balanced")) == "balanced";
    bool keepFace = c.value("allow_face", false), ignoreBudget = c.value("ignore_budget", false);
    int count = c.value("count", 0);
    float rootBias = c.value("root_bias_mm", 40.0f) / 1000;   // prefer lanes that START near the root (inner layers) over lanes passing over it
    // layer "inner" (2026-10-12): raised / stacked cards (1-4 cm off the scalp, Fringilla). Card hair counts up to raise_mm
    // above the skin; a root is under card hair when card hair lies straight above it (lateral reach_mm); lanes are
    // searched above the root (lateral near_mm, height 0..raise_mm) and the innermost layer wins (weight
    // exp(-height / inner_bias_mm)); the hug band (pressed toward the scalp) widens to hug_band_mm (= raise_mm)
    const bool inner = c.value("layer", std::string("near")) == "inner";
    const float raise = c.value("raise_mm", 45.0f) / 1000, innerBias = c.value("inner_bias_mm", 8.0f) / 1000;
    const float hugBand = c.value("hug_band_mm", inner ? raise * 1000 : 35.0f) / 1000;
    // rebase2 (2026-10-12, all opt-in; without these keys nothing changes):
    //  trace "flow": every strand is a streamline of the card flow field (mean root->tip texture direction of the card
    //    lanes around it, clustered so opposite flows at a parting do not mix), holding a per-strand fraction of the local
    //    card layer height (volume of raised / stacked cards kept, layered: flow_layer_min..flow_layer_max), length = the
    //    picked lane's remaining length. Neighbouring roots give neighbouring, non-crossing strands (sleek bobs, swept
    //    fringes, tufts); the lane copy + hug of the default trace is skipped.
    //  density_mode "coverage": root spacing from the visible card coverage (alpha-weighted card area per scalp area
    //    above the root, coverage_radius_mm) instead of the build's hair budget: coverage_spacing_mm at >= coverage_ref,
    //    sparser where cards are thin, no root under coverage_min; budget_target is then only a maximum.
    //  scalp_grow_mm: roots also on skin within this distance of the painted scalp where card hair lies on the skin
    //    (grow_card_mm), never on the front forehead (temples, sideburns, nape of short cuts).
    //  eye_mode "cut": strands reaching an eye are cut before it (fringes kept) instead of rejected.
    //  exclude_ears / exclude_temples_mm / ear_route: see the dispatcher (Hooks).
    const bool orientVote = c.value("orient", std::string("near")) == "vote";
    const float orientMargin = c.value("orient_margin_mm", 15.0f) / 1000;
    int nVoted = 0;
    const bool flow = c.value("trace", std::string("lane")) == "flow";
    const float flowR = c.value("flow_radius_mm", 10.0f) / 1000, flowStep = c.value("flow_step_mm", 3.0f) / 1000;
    const float flowInertia = std::clamp(c.value("flow_inertia", 0.35f), 0.0f, 0.95f), flowHeight = c.value("flow_height", 1.0f);
    const float flowLayerMin = c.value("flow_layer_min", 0.35f), flowLayerMax = c.value("flow_layer_max", 1.15f);
    const float flowRise = c.value("flow_rise_mm", 25.0f) / 1000, flowLenScale = c.value("flow_length_scale", 1.0f);
    const float flowHold = std::clamp(c.value("flow_hold", 0.5f), 0.0f, 1.0f);
    const bool flowFaceFull = c.value("flow_face_full", true);
    const float flowClear = c.value("flow_clear_mm", 5.0f) / 1000;
    const float flowWhorl = std::clamp(c.value("flow_whorl", 0.0f), 0.0f, 1.0f), flowWhorlAgree = c.value("flow_whorl_agree", 0.55f);
    int nWhorl = 0;
    const bool flowTipStop = c.value("flow_tip_stop", true);
    const bool flowStartLane = c.value("flow_start", std::string("mean")) == "lane";
    const float flowStartMix = std::clamp(c.value("flow_start_mix", 1.0f), 0.0f, 1.0f);
    const bool coverage = c.value("density_mode", std::string("budget")) == "coverage";
    const float covSpacing = c.value("coverage_spacing_mm", 7.0f) / 1000, covRef = c.value("coverage_ref", 1.0f);
    const float covMin = c.value("coverage_min", 0.08f), covR = c.value("coverage_radius_mm", 10.0f) / 1000;
    const float covFloor = c.value("coverage_floor", 0.25f);   // spacing never above covSpacing / sqrt(covFloor)
    const float scalpGrow = c.value("scalp_grow_mm", 0.0f) / 1000, growCard = c.value("grow_card_mm", 8.0f) / 1000;
    const bool eyeCut = c.value("eye_mode", std::string("reject")) == "cut";
    // rebase3 (2026-10-13, all opt-in; without these keys nothing changes):
    //  part_detect: the part line = where the lateral card flow on the top diverges (per front->back row, the x where
    //    the flow's sideways component turns from -x to +x). Roots grow up to both edges of it (part_band_mm, even
    //    without visible card hair straight above) and start combed AWAY from it (part_comb); no whorl crossing there.
    //  fringe: lanes whose tips hang over the forehead (front, below their root, off the painted scalp) are fringe
    //    lanes; front roots within fringe_reach_mm of one copy a fringe lane (from the hairline, forward / side-swept,
    //    held fringe_clear_mm off the skin) instead of the flow / lane trace.
    //  alpha_density: a root survives with probability smoothstep(alpha_lo, alpha_hi, mean card alpha in the column
    //    above it, alpha_radius_mm): roots only where the card texture is opaque, ragged where it fades.
    //  clump_var: clumps of varied size (clump_min_mm..clump_max_mm), member tips pulled to the clump's centre strand
    //    (clump_pull, tapered tips), strand length varied (len_var), flyaways (fly_frac of the strands at the edge of
    //    the card hair get a sideways drift of fly_mm toward the tip and are shortened).
    //  nape_cut: no roots on the back below the lowest vanilla card roots (per azimuth bin, nape_pct percentile of the
    //    lane roots near the skin) minus nape_cut_mm.
    const bool partDetect = c.value("part_detect", false);
    const float partBand = c.value("part_band_mm", 18.0f) / 1000, partComb = std::clamp(c.value("part_comb", 0.7f), 0.0f, 1.0f);
    const int partExt = std::max(0, (int)std::lround(c.value("part_extend_mm", 10.0f) / 10));
    const float partLayer = c.value("part_layer", 1.0f), partClear = c.value("part_clear_mm", 4.0f) / 1000;
    const float partDepth = c.value("part_depth_mm", 70.0f) / 1000, partMinFlow = c.value("part_min_flow", 0.25f);
    const bool fringeMode = c.value("fringe", false);
    const float fringeReach = c.value("fringe_reach_mm", 20.0f) / 1000, fringeClear = c.value("fringe_clear_mm", 3.0f) / 1000;
    const float fringeDrop = c.value("fringe_drop_mm", 10.0f) / 1000;
    const bool alphaDensity = c.value("alpha_density", false);
    const float adLo = c.value("alpha_lo", 0.15f), adHi = c.value("alpha_hi", 0.6f), adR = c.value("alpha_radius_mm", 6.0f) / 1000;
    const float clumpVar = std::clamp(c.value("clump_var", 0.0f), 0.0f, 1.0f);
    const float clumpMin = c.value("clump_min_mm", 4.0f) / 1000, clumpMax = c.value("clump_max_mm", 14.0f) / 1000;
    const float clumpPull = std::clamp(c.value("clump_pull", 0.6f), 0.0f, 1.0f) * clumpVar, lenVar = std::clamp(c.value("len_var", 0.25f), 0.0f, 0.8f) * clumpVar;
    const float flyFrac = std::clamp(c.value("fly_frac", 0.3f), 0.0f, 1.0f) * (clumpVar > 0 ? 1.0f : 0.0f), flyMm = c.value("fly_mm", 12.0f) / 1000;
    const bool napeCut = c.value("nape_cut", false);
    const float napeOff = c.value("nape_cut_mm", 0.0f) / 1000, napePct = std::clamp(c.value("nape_pct", 0.05f), 0.0f, 0.5f);
    auto hsh = [](V3 p, float s) { float u = std::sin(p.x * (12.9898f + s) * 1000 + p.y * (78.233f + s) * 1000 + p.z * (37.719f + 2 * s) * 1000) * 43758.5453f; return u - std::floor(u); };
    // ---- head mesh (edit frame)
    const Mesh* head = nullptr;
    for (auto& m : pk.meshes) if (m.kind == "head") { head = &m; break; }
    if (!head) { r["error"] = "pack has no head mesh"; return r; }
    // fast signed distance to the head mesh (nearest vertex + its outward normal, hash grid)
    std::vector<V3> hN(head->v.size(), V3{0, 0, 0});
    for (auto& t : head->t) { V3 n = cross(head->v[t[1]] - head->v[t[0]], head->v[t[2]] - head->v[t[0]]); for (int q = 0; q < 3; q++) hN[t[q]] = hN[t[q]] + n; }
    for (size_t i = 0; i < hN.size(); i++) { hN[i] = nrmz(hN[i]); if (dot(head->v[i] - C, hN[i]) < 0) hN[i] = hN[i] * -1.0f; }
    Grid hg(0.01f);
    for (size_t i = 0; i < head->v.size(); i++) hg.add(head->v[i], (int)i);
    // skin_exact (rebase3, opt-in): signed distance to the nearest head TRIANGLE (the nearest-vertex plane misjudges a
    // coarse head mesh by mm: hugged strands sank under the skin at Ves's part)
    const bool skinExact = c.value("skin_exact", false);
    std::vector<std::vector<int>> vTri;
    if (skinExact) { vTri.resize(head->v.size()); for (size_t ti = 0; ti < head->t.size(); ti++) for (int q = 0; q < 3; q++) vTri[head->t[ti][q]].push_back((int)ti); }
    auto closestTri = [&](V3 p, V3 a, V3 b, V3 cc) {   // Ericson, closest point on triangle
        V3 ab = b - a, ac = cc - a, ap = p - a;
        float d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0 && d2 <= 0) return a;
        V3 bp = p - b; float d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0 && d4 <= d3) return b;
        float vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
        V3 cp = p - cc; float d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0 && d5 <= d6) return cc;
        float vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
        float va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (cc - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        float dn = 1.0f / (va + vb + vc);
        return a + ab * (vb * dn) + ac * (vc * dn);
    };
    auto headSD = [&](V3 p, V3* nrm) -> float {
        int bi = -1; float bd = 1e9f;
        for (float rad = 0.01f; bi < 0 && rad <= 0.16f; rad *= 2)
            hg.around(p, rad, [&](int i) { float d = len(head->v[i] - p); if (d < bd) { bd = d; bi = i; } });
        if (bi < 0) { if (nrm) *nrm = nrmz(p - C); return 1.0f; }
        if (skinExact) {
            float best = 1e9f; V3 bq{}, bn{};
            std::set<int> seenT;
            hg.around(p, bd + 0.006f, [&](int i) {
                if (len(head->v[i] - p) > bd + 0.006f) return;
                for (int ti : vTri[i]) {
                    if (!seenT.insert(ti).second) continue;
                    auto& t = head->t[ti];
                    V3 q = closestTri(p, head->v[t[0]], head->v[t[1]], head->v[t[2]]);
                    float d = len(q - p);
                    if (d < best) { best = d; bq = q; bn = cross(head->v[t[1]] - head->v[t[0]], head->v[t[2]] - head->v[t[0]]); }
                }
            });
            if (best < 1e8f) {
                bn = nrmz(bn); if (dot(bn, hN[bi]) < 0) bn = bn * -1.0f;
                if (nrm) *nrm = bn;
                float sd = dot(p - bq, bn);
                return (sd >= 0 ? 1.0f : -1.0f) * best;
            }
        }
        if (nrm) *nrm = hN[bi];
        return dot(p - head->v[bi], hN[bi]);
    };
    // ---- lanes from every hair-card island
    std::vector<Lane> lanes;
    std::vector<V3> vis;   // visible near-skin card points
    std::vector<std::pair<V3, float>> covS;   // density_mode coverage: card samples + alpha x area
    std::vector<std::pair<V3, float>> adS;    // alpha_density: every card sample near the skin + its alpha
    std::mt19937 rng(2);
    std::uniform_real_distribution<float> U01(0.0f, 1.0f);
    int nCards = 0, nIslands = 0, nDup = 0;
    std::map<std::string, std::tuple<unsigned char*, int, int>> tex;
    for (auto& m : pk.meshes) {
        if (m.kind != "hair_cards" || m.uv.size() != m.v.size() || m.t.empty()) continue;
        nCards++;
        if (!tex.count(m.texFile)) { int w = 0, h = 0; unsigned char* im = m.texFile.empty() ? nullptr : loadImage(pk.dir + "\\" + m.texFile, &w, &h); tex[m.texFile] = {im, w, h}; }
        auto [im, tw, th] = tex[m.texFile];
        auto alphaAt = [&, im = im, tw = tw, th = th](float u, float v) -> float {
            if (!im) return 1.0f;
            u -= std::floor(u); v -= std::floor(v);
            int x = std::clamp(int(u * tw), 0, tw - 1), y = std::clamp(int(v * th), 0, th - 1);
            return im[(size_t(y) * tw + x) * 4 + 3] / 255.0f;
        };
        float aspect = im ? float(th) / float(tw) : 1.0f;
        // visible points (area-weighted samples with alpha > alpha_vis, within 3 cm of the skin)
        {
            std::vector<double> acc;
            double tot = 0;
            for (auto& t : m.t) { tot += 0.5 * len(cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]])); acc.push_back(tot); }
            int ns = std::min(20000, std::max(500, int(tot * 4e6)));
            for (int s = 0; s < ns && tot > 0; s++) {
                double x = U01(rng) * tot;
                size_t ti = std::lower_bound(acc.begin(), acc.end(), x) - acc.begin();
                if (ti >= m.t.size()) ti = m.t.size() - 1;
                auto& t = m.t[ti];
                float uu = U01(rng), vv = U01(rng);
                if (uu + vv > 1) { uu = 1 - uu; vv = 1 - vv; }
                V3 X = m.v[t[0]] + (m.v[t[1]] - m.v[t[0]]) * uu + (m.v[t[2]] - m.v[t[0]]) * vv;
                float tu = m.uv[t[0]][0] + (m.uv[t[1]][0] - m.uv[t[0]][0]) * uu + (m.uv[t[2]][0] - m.uv[t[0]][0]) * vv;
                float tv = m.uv[t[0]][1] + (m.uv[t[1]][1] - m.uv[t[0]][1]) * uu + (m.uv[t[2]][1] - m.uv[t[0]][1]) * vv;
                float al = alphaAt(tu, tv);
                if (alphaDensity) {
                    float sda = headSD(X, nullptr);
                    if (inner ? (sda > -0.004f && sda < raise) : std::fabs(sda) < 0.03f) adS.push_back({X, al});
                }
                if (coverage && al > alphaTip) {   // alpha-weighted card area (m2) for density_mode coverage
                    float sdc = headSD(X, nullptr);
                    if (inner ? (sdc > -0.004f && sdc < raise) : std::fabs(sdc) < 0.03f) covS.push_back({X, float(al * tot / ns)});
                }
                if (al <= alphaVis) continue;
                float sdx = headSD(X, nullptr);
                if (inner ? (sdx > -0.004f && sdx < raise) : std::fabs(sdx) < 0.03f) vis.push_back(X);
            }
        }
        // UV islands: same signed uv area + connected (mirrored halves share one strip of the atlas)
        int nv = (int)m.v.size();
        std::vector<int> par(nv);
        for (int i = 0; i < nv; i++) par[i] = i;
        std::function<int(int)> fnd = [&](int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; };
        std::vector<int> sg(m.t.size());
        for (size_t ti = 0; ti < m.t.size(); ti++) {
            auto& t = m.t[ti];
            float ax = m.uv[t[1]][0] - m.uv[t[0]][0], ay = m.uv[t[1]][1] - m.uv[t[0]][1];
            float bx = m.uv[t[2]][0] - m.uv[t[0]][0], by = m.uv[t[2]][1] - m.uv[t[0]][1];
            sg[ti] = (ax * by - ay * bx) >= 0 ? 1 : 0;
        }
        std::map<std::pair<int, int>, std::vector<int>> isl;
        for (int s = 0; s < 2; s++) {
            for (int i = 0; i < nv; i++) par[i] = i;
            for (size_t ti = 0; ti < m.t.size(); ti++) if (sg[ti] == s) { auto& t = m.t[ti]; par[fnd(t[1])] = fnd(t[0]); par[fnd(t[2])] = fnd(t[0]); }
            for (size_t ti = 0; ti < m.t.size(); ti++) if (sg[ti] == s) isl[{fnd(m.t[ti][0]), s}].push_back((int)ti);
        }
        // orient "vote" (rebase2, opt-in): islands whose two ends lie at nearly the same distance from the skin (fringe
        // cards lying on the forehead, short stacked cards) take the root side the confident islands of the same atlas
        // column (u bucket, then the whole mesh) agree on, instead of the end nearer the skin
        std::map<int, int> voteU; int voteAll = 0;
        auto islandEnds = [&](const std::vector<int>& tl, float& u0, float& u1, float& v0, float& v1, V3& lo, V3& hi) {
            std::set<int> vs;
            for (int ti : tl) for (int q = 0; q < 3; q++) vs.insert(m.t[ti][q]);
            if (vs.size() < 4) return false;
            u0 = 1e9f; u1 = -1e9f; v0 = 1e9f; v1 = -1e9f;
            for (int v : vs) { u0 = std::min(u0, m.uv[v][0]); u1 = std::max(u1, m.uv[v][0]); v0 = std::min(v0, m.uv[v][1]); v1 = std::max(v1, m.uv[v][1]); }
            if (v1 - v0 < 0.02f) return false;
            lo = hi = V3{0, 0, 0}; int nl = 0, nh = 0;
            for (int v : vs) {
                if (m.uv[v][1] < v0 + 0.15f * (v1 - v0)) { lo = lo + m.v[v]; nl++; }
                if (m.uv[v][1] > v1 - 0.15f * (v1 - v0)) { hi = hi + m.v[v]; nh++; }
            }
            lo = lo * (1.0f / std::max(nl, 1)); hi = hi * (1.0f / std::max(nh, 1));
            return true;
        };
        auto uBucket = [](float u0, float u1) { float u = (u0 + u1) / 2; u -= std::floor(u); return std::clamp(int(u * 8), 0, 7); };
        if (orientVote)
            for (auto& [lab, tl] : isl) {
                float u0, u1, v0, v1; V3 lo, hi;
                if (!islandEnds(tl, u0, u1, v0, v1, lo, hi)) continue;
                float sh = std::fabs(headSD(hi, nullptr)), sl = std::fabs(headSD(lo, nullptr));
                if (std::fabs(sh - sl) < orientMargin) continue;
                int vt = sh < sl ? 1 : -1;   // +1 = root at v1
                voteU[uBucket(u0, u1)] += vt; voteAll += vt;
            }
        for (auto& [lab, tl] : isl) {
            std::set<int> vs;
            for (int ti : tl) for (int q = 0; q < 3; q++) vs.insert(m.t[ti][q]);
            if (vs.size() < 4) continue;
            float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
            for (int v : vs) { u0 = std::min(u0, m.uv[v][0]); u1 = std::max(u1, m.uv[v][0]); v0 = std::min(v0, m.uv[v][1]); v1 = std::max(v1, m.uv[v][1]); }
            if (v1 - v0 < 0.02f) continue;
            nIslands++;
            V3 lo{0, 0, 0}, hi{0, 0, 0}; int nl = 0, nh = 0;
            for (int v : vs) {
                if (m.uv[v][1] < v0 + 0.15f * (v1 - v0)) { lo = lo + m.v[v]; nl++; }
                if (m.uv[v][1] > v1 - 0.15f * (v1 - v0)) { hi = hi + m.v[v]; nh++; }
            }
            lo = lo * (1.0f / std::max(nl, 1)); hi = hi * (1.0f / std::max(nh, 1));
            bool flip = std::fabs(headSD(hi, nullptr)) + 0.01f < std::fabs(headSD(lo, nullptr));
            if (orientVote) {
                float sh = std::fabs(headSD(hi, nullptr)), sl = std::fabs(headSD(lo, nullptr));
                if (std::fabs(sh - sl) < orientMargin) {
                    int vb = voteU.count(uBucket(u0, u1)) ? voteU[uBucket(u0, u1)] : 0;
                    int vt = std::abs(vb) >= 2 ? vb : voteAll;
                    if (vt != 0) { bool f2 = vt > 0; if (f2 != flip) nVoted++; flip = f2; }
                }
            }
            float lenM = len(hi - lo);
            float uvm = (v1 - v0) / std::max(lenM, 0.01f);
            float widthM = (u1 - u0) / std::max(uvm * aspect, 1e-6f);
            int L = std::clamp((int)std::lround(widthM / laneW), 1, 8);
            int ns = std::clamp(int(2.5f * lenM / 0.006f), 24, 160);
            // barycentric hits of (u, v) in the island's triangles: overlapping uv copies (mirrored halves of one
            // card sharing a strip of the atlas with the same winding) give several 3D points
            auto hits = [&](float uu, float vv, std::vector<std::pair<float, V3>>& out) {
                out.clear();
                for (int ti : tl) {
                    auto& t = m.t[ti];
                    float ax = m.uv[t[0]][0], ay = m.uv[t[0]][1];
                    float e0x = m.uv[t[1]][0] - ax, e0y = m.uv[t[1]][1] - ay, e1x = m.uv[t[2]][0] - ax, e1y = m.uv[t[2]][1] - ay;
                    float qx = uu - ax, qy = vv - ay;
                    float den = e0x * e1y - e0y * e1x;
                    if (std::fabs(den) < 1e-12f) continue;
                    float b1 = (qx * e1y - qy * e1x) / den, b2 = (e0x * qy - e0y * qx) / den, b0 = 1 - b1 - b2;
                    float mn = std::min({b0, b1, b2});
                    if (mn < -0.15f) continue;
                    out.push_back({mn, m.v[t[0]] * b0 + m.v[t[1]] * b1 + m.v[t[2]] * b2});
                }
            };
            std::vector<std::pair<float, V3>> H;
            for (int li = 0; li < L; li++) {
                float uu = u0 + (li + 0.5f) / L * (u1 - u0);
                auto vAt = [&](int s) { float f = float(s) / (ns - 1); return flip ? v1 - f * (v1 - v0) : v0 + f * (v1 - v0); };
                // seeds: every distinct copy at the first sample that hits the island -> one lane per copy
                std::vector<V3> seeds;
                for (int s = 0; s < ns && seeds.empty(); s++) {
                    hits(uu, vAt(s), H);
                    for (auto& [mn, X] : H) {
                        if (mn < -0.02f) continue;
                        bool near_ = false;
                        for (auto& q : seeds) if (len(q - X) < 0.015f) near_ = true;
                        if (!near_ && seeds.size() < 4) seeds.push_back(X);
                    }
                }
                if (seeds.empty()) seeds.push_back(V3{1e9f, 1e9f, 1e9f});   // only near-miss hits: free start
                for (V3 seed : seeds) {
                    std::vector<V3> pts; std::vector<float> al, vsv;
                    bool havePrev = seed.x < 1e8f; V3 prev = seed;
                    for (int s = 0; s < ns; s++) {
                        float vv = vAt(s);
                        hits(uu, vv, H);
                        float bestMn = -1e9f, bestD = 1e9f; V3 bestX{}; bool found = false;
                        for (auto& [mn, X] : H) {
                            if (mn >= -0.02f) {
                                float d = havePrev ? len(X - prev) : 0;
                                if (!found || d < bestD) { bestD = d; bestX = X; bestMn = mn; found = true; }
                            } else if (!found && mn > bestMn) { bestMn = mn; bestX = X; }
                        }
                        if (H.empty()) continue;
                        if (havePrev && len(bestX - prev) > 0.02f) { if (pts.empty()) continue; break; }   // jump: keep the root-side piece
                        pts.push_back(bestX); al.push_back(alphaAt(uu, vv)); vsv.push_back(vv);
                        prev = bestX; havePrev = true;
                    }
                    if (pts.size() < 4) continue;
                    // keep the part where the texture shows hair (smoothed alpha along the lane)
                    int first = -1, last = -1;
                    for (size_t s = 0; s < al.size(); s++) {
                        float ka = (al[s] + al[s > 0 ? s - 1 : s] + al[s + 1 < al.size() ? s + 1 : s]) / 3;
                        if (ka > alphaTip) { if (first < 0) first = (int)s; last = (int)s; }
                    }
                    if (first < 0 || last - first < 3) continue;
                    std::vector<V3> q(pts.begin() + first, pts.begin() + last + 1);
                    if (len(q.back() - q.front()) < 0.015f) continue;
                    Lane ln;
                    ln.raw = q;
                    ln.uv0 = {uu, vsv[first]}; ln.uv1 = {uu, vsv[last]};
                    lanes.push_back(std::move(ln));
                }
            }
        }
    }
    for (auto& [k, t] : tex) if (std::get<0>(t)) freeImage(std::get<0>(t));
    if (lanes.empty()) { r["error"] = "no card lanes (pack has no vanilla hair cards with uvs: re-run desk_prep)"; return r; }
    // chain card segments (some meshes split every card into short islands): a lane tip continues into the lane whose
    // root is within chain_mm in 3D AND continues in the atlas (uv within 0.03), mutual nearest (growth.chain_segments)
    int nChained = 0;
    {
        float ptol = c.value("chain_mm", 8.0f) / 1000;
        auto uvd = [](std::array<float, 2> a, std::array<float, 2> b) {
            float du = std::fabs(a[0] - b[0]), dv = std::fabs(a[1] - b[1]);
            du -= std::floor(du); dv -= std::floor(dv);
            return std::max(std::min(du, 1 - du), std::min(dv, 1 - dv));
        };
        // ends 2i (root end) / 2i+1 (tip end); link mutual nearest ends (any orientation), then orient each chain
        size_t n = lanes.size();
        auto endP = [&](int e) { return (e & 1) ? lanes[e >> 1].raw.back() : lanes[e >> 1].raw.front(); };
        auto endUv = [&](int e) { return (e & 1) ? lanes[e >> 1].uv1 : lanes[e >> 1].uv0; };
        Grid eg(0.01f);
        for (size_t e = 0; e < 2 * n; e++) eg.add(endP((int)e), (int)e);
        std::vector<int> best(2 * n, -1);
        for (size_t e = 0; e < 2 * n; e++) {
            float bd = ptol;
            V3 p = endP((int)e);
            eg.around(p, ptol, [&](int f) {
                if ((f >> 1) == int(e >> 1)) return;
                float d = len(endP(f) - p);
                if (d < bd && uvd(endUv((int)e), endUv(f)) < 0.03f) { bd = d; best[e] = f; }
            });
        }
        std::vector<int> link(2 * n, -1);
        for (size_t e = 0; e < 2 * n; e++) if (best[e] >= 0 && best[best[e]] == (int)e) link[e] = best[e];
        std::vector<Lane> out;
        std::vector<char> seen(n, 0);
        for (size_t i = 0; i < n; i++) {
            if (seen[i]) continue;
            // walk out through end 0 to the chain's free end
            int cur = (int)i, e = 0, guard = 0;
            std::set<int> vis{cur};
            while (link[2 * cur + e] >= 0 && guard++ < 256) {
                int f = link[2 * cur + e], j = f >> 1;
                if (vis.count(j)) break;
                vis.insert(j); cur = j; e = 1 - (f & 1);
            }
            // cur's end e is free: enter there and walk the chain
            std::vector<V3> pts;
            int enter = e, parts = 0, fwdParts = 0;
            guard = 0;
            while (!seen[cur] && guard++ < 256) {
                seen[cur] = 1; parts++;
                if (enter == 0) fwdParts++;
                const auto& R = lanes[cur].raw;
                if (enter == 0) pts.insert(pts.end(), R.begin() + (pts.empty() ? 0 : 1), R.end());
                else pts.insert(pts.end(), R.rbegin() + (pts.empty() ? 0 : 1), R.rend());
                int leave = 2 * cur + (1 - enter);
                if (link[leave] < 0) break;
                cur = link[leave] >> 1; enter = link[leave] & 1;
            }
            if (parts > 1) {
                nChained += parts - 1;
                if (orientVote) { if (2 * fwdParts < parts) std::reverse(pts.begin(), pts.end()); }   // the parts' own (voted) root sides decide
                else if (std::fabs(headSD(pts.back(), nullptr)) + 0.005f < std::fabs(headSD(pts.front(), nullptr))) std::reverse(pts.begin(), pts.end());
            }
            Lane L = lanes[i];
            L.raw = pts;
            out.push_back(std::move(L));
        }
        lanes.swap(out);
        for (auto& ln : lanes) ln.p = resampleN(ln.raw, 48, &ln.L);
    }
    // stacked / layered cards: a lane running within dedupe_mm of an earlier (longer) one adds nothing
    {
        std::vector<int> ord(lanes.size());
        for (size_t i = 0; i < ord.size(); i++) ord[i] = (int)i;
        std::sort(ord.begin(), ord.end(), [&](int x, int y) { return lanes[x].L > lanes[y].L; });
        std::vector<Lane> kept;
        for (int i : ord) {
            bool dup = false;
            for (auto& k : kept) {
                if (len(k.p[0] - lanes[i].p[0]) > 0.02f) continue;
                float s = 0; int nn = 0;
                for (int j = 0; j < 48; j += 6) {
                    float bd = 1e9f;
                    for (auto& q : k.p) bd = std::min(bd, len(q - lanes[i].p[j]));
                    s += bd; nn++;
                }
                if (s / nn < dedupe) { dup = true; break; }
            }
            if (dup) nDup++; else kept.push_back(lanes[i]);
        }
        lanes.swap(kept);
    }
    // dense lane points for the root -> lane search
    std::vector<V3> dense; std::vector<int> sid, jj; std::vector<float> rem;
    for (size_t i = 0; i < lanes.size(); i++)
        for (int j = 0; j < 48; j++) { dense.push_back(lanes[i].p[j]); sid.push_back((int)i); jj.push_back(j); rem.push_back((47 - j) * lanes[i].L / 47); }
    Grid dg(0.01f);
    for (size_t i = 0; i < dense.size(); i++) dg.add(dense[i], (int)i);
    // trace flow: per lane point its root->tip tangent, height over the skin and lane length before it
    std::vector<V3> denseT; std::vector<float> denseH, denseB;
    if (flow)
        for (size_t q = 0; q < dense.size(); q++) {
            const Lane& ln = lanes[sid[q]];
            int j = jj[q];
            denseT.push_back(nrmz(ln.p[std::min(j + 1, 47)] - ln.p[std::max(j - 1, 0)]));
            denseH.push_back(headSD(dense[q], nullptr));
            denseB.push_back(j * ln.L / 47);
        }
    Grid vg(0.01f);
    {
        std::vector<V3> nv;
        for (auto& v : vis) nv.push_back(v);
        vis.swap(nv);
    }
    for (size_t i = 0; i < vis.size(); i++) vg.add(vis[i], (int)i);
    // painted scalp of the head (Studio scalp flags): roots only there, never on the forehead / temples under a
    // fringe that hangs within reach of the skin
    const Mesh* sm = nullptr; const std::vector<float>* sf = nullptr;
    if (c.value("scalp_only", true))
        for (auto& [mi, f] : a.headScalp) if (mi >= 0 && mi < (int)pk.meshes.size() && f.size() == pk.meshes[mi].v.size()) { sm = &pk.meshes[mi]; sf = &f; break; }
    Grid sgd(0.01f);
    if (sm) for (size_t i = 0; i < sm->v.size(); i++) sgd.add(sm->v[i], (int)i);
    auto onScalpE = [&](V3 p) {
        if (!sm) return true;
        int bi = -1; float bd = 1e9f;
        for (float rad = 0.01f; bi < 0 && rad <= 0.08f; rad *= 2)
            sgd.around(p, rad, [&](int i) { float d = len(sm->v[i] - p); if (d < bd) { bd = d; bi = i; } });
        return bi >= 0 && (*sf)[bi] > 0.5f;
    };
    int offScalp = 0, nGrown = 0, nExRoot = 0;
    // scalp_grow_mm: painted vertices for the distance test
    Grid pvg(0.01f);
    if (scalpGrow > 0 && sm) for (size_t i = 0; i < sm->v.size(); i++) if ((*sf)[i] > 0.5f) pvg.add(sm->v[i], (int)i);
    auto growOk = [&](V3 p) {
        if (scalpGrow <= 0 || !sm) return false;
        if (hk && hk->forehead && hk->forehead(p)) return false;
        bool nearP = false;
        pvg.around(p, scalpGrow, [&](int i) { if (!nearP && len(sm->v[i] - p) < scalpGrow) nearP = true; });
        if (!nearP) return false;
        bool card = false;
        vg.around(p, growCard, [&](int i) { if (!card && len(vis[i] - p) < growCard) card = true; });
        return card;
    };
    // density_mode coverage: alpha-weighted card area in the column above a skin point / column area
    Grid cvg(0.01f);
    for (size_t i = 0; i < covS.size(); i++) cvg.add(covS[i].first, (int)i);
    auto covAt = [&](V3 p) {
        V3 n; headSD(p, &n);
        float colH = inner ? raise : 0.03f, s = 0;
        cvg.around(p, covR + colH, [&](int i) {
            V3 d = covS[i].first - p; float h = dot(d, n);
            if (h < -0.004f || h > colH || len(d - n * h) > covR) return;
            s += covS[i].second;
        });
        return s / (3.14159265f * covR * covR);
    };
    // ---- rebase3 helpers (only computed when their key is on)
    float fwd3 = 1.0f, eyeZ3 = C.z - 0.03f;
    {
        V3 es{0, 0, 0}; int en = 0;
        for (auto& em : pk.meshes) if (em.texFile.find("eye") != std::string::npos) for (auto& v : em.v) { es = es + v; en++; }
        if (en) { es = es * (1.0f / en); fwd3 = es.y >= C.y ? 1.0f : -1.0f; eyeZ3 = es.z; }
    }
    std::vector<V3> laneT;   // root->tip tangent per dense lane point
    if (partDetect || fringeMode)
        for (size_t q = 0; q < dense.size(); q++) { const Lane& ln = lanes[sid[q]]; int j = jj[q]; laneT.push_back(nrmz(ln.p[std::min(j + 1, 47)] - ln.p[std::max(j - 1, 0)])); }
    // part_detect: per 1 cm row (front->back) the x where the sideways flow diverges
    std::map<int, float> partX; json partJ = json::array(), partDbg = json::array();
    if (partDetect) {
        std::map<int, std::vector<std::array<float, 3>>> rows;   // row -> (x, tx, w)
        for (size_t q = 0; q < dense.size(); q++) {
            V3 p = dense[q];
            if (p.z < pk.headTop - partDepth || fwd3 * (p.y - C.y) < -0.07f) continue;
            if (std::fabs(headSD(p, nullptr)) > 0.02f) continue;
            V3 t = laneT[q];
            float horiz = std::sqrt(t.x * t.x + t.y * t.y);
            if (horiz < 0.2f) continue;
            rows[(int)std::floor((p.y - C.y) / 0.01f)].push_back({p.x - C.x, t.x, 1.0f});
        }
        for (auto& [row, v] : rows) {
            if (v.size() < 30) continue;
            float bestS = 0, bestX = 0; bool got = false;
            auto txAt = [&](float x, float& wsum) {
                float s = 0; wsum = 0;
                for (auto& e : v) { float d = e[0] - x; float w = std::exp(-d * d / (2 * 0.006f * 0.006f)); s += e[1] * w; wsum += w; }
                return wsum > 1e-6f ? s / wsum : 0.0f;
            };
            // last x with card data before this one (a part line often has no card over it: bridge gaps <= 20 mm)
            float prevX = -1, prevT = 0;
            for (float x = -0.06f; x <= 0.0601f; x += 0.002f) {
                float w; float tx = txAt(x, w);
                if (w <= 3) continue;
                if (prevX > -0.5f && x - prevX <= 0.0205f && prevT < 0 && tx >= 0) {
                    float wl, wr; float tl = txAt(prevX - 0.012f, wl), tr = txAt(x + 0.012f, wr);
                    float s = std::min(-tl, tr);
                    if (s > partMinFlow && s > bestS) { bestS = s; bestX = 0.5f * (x + prevX); got = true; }
                }
                prevX = x; prevT = tx;
            }
            if (got) { partX[row] = C.x + bestX; partJ.push_back({row, r4(C.x + bestX), r4(bestS)}); }
            if (c.value("part_debug", false)) {
                json sc = json::array();
                for (float x = -0.06f; x <= 0.0601f; x += 0.005f) { float w; float tx = txAt(x, w); sc.push_back({r4(x), r4(tx), int(w)}); }
                partDbg.push_back({{"row", row}, {"n", v.size()}, {"scan", sc}});
            }
        }
        // keep the longest run of consecutive rows whose x moves smoothly (one part line)
        std::map<int, float> best, cur;
        int last = -1000; float lastX = 0;
        for (auto& [row, x] : partX) {
            if (row > last + 3 || std::fabs(x - lastX) > 0.006f * (row - last) + 0.006f) { if (cur.size() > best.size()) best = cur; cur.clear(); }
            else for (int g = last + 1; g < row; g++) cur[g] = lastX + (x - lastX) * float(g - last) / float(row - last);   // bridge
            cur[row] = x; last = row; lastX = x;
        }
        if (cur.size() > best.size()) best = cur;
        partX = best.size() >= 3 ? best : std::map<int, float>{};
    }
    // part root: away direction (tangent to the skin, sideways from the line)
    auto partAway = [&](V3 R, V3& away) -> bool {
        if (partX.empty() || R.z < pk.headTop - partDepth - 0.01f) return false;
        int row = (int)std::floor((R.y - C.y) / 0.01f);
        int r0 = partX.begin()->first, r1 = partX.rbegin()->first;
        if (row < r0 - partExt || row > r1 + partExt) return false;
        row = std::clamp(row, r0, r1);
        auto it = partX.lower_bound(row);
        if (it == partX.end()) return false;
        float dx = R.x - it->second;
        if (std::fabs(dx) > partBand) return false;
        V3 n; headSD(R, &n);
        V3 s{dx >= 0 ? 1.0f : -1.0f, 0, 0};
        away = nrmz(s - n * dot(s, n));
        return true;
    };
    // part side of a point: -1 / +1 within 2 x part_band_mm of the line, 0 elsewhere
    auto partSide = [&](V3 R) -> int {
        if (partX.empty() || R.z < pk.headTop - partDepth - 0.01f) return 0;
        int row = (int)std::floor((R.y - C.y) / 0.01f);
        int r0 = partX.begin()->first, r1 = partX.rbegin()->first;
        if (row < r0 - partExt || row > r1 + partExt) return 0;
        auto it = partX.lower_bound(std::clamp(row, r0, r1));
        if (it == partX.end()) return 0;
        float dx = R.x - it->second;
        if (std::fabs(dx) > 2 * partBand) return 0;
        return dx >= 0 ? 1 : -1;
    };
    auto partDx = [&](V3 R) -> float {
        int row = std::clamp((int)std::floor((R.y - C.y) / 0.01f), partX.begin()->first, partX.rbegin()->first);
        return R.x - partX.lower_bound(row)->second;
    };
    const float partGap = c.value("part_gap_mm", 3.0f) / 1000, partCross = c.value("part_cross_mm", 0.0f) / 1000;
    const bool partSplit = c.value("part_split", true);
    // fringe lanes: tips hang over the forehead (front, below their root, off the painted scalp)
    std::vector<char> fringeLane(lanes.size(), 0);
    int nFringeLanes = 0, frRej[4] = {0, 0, 0, 0};
    const float fringeFront = c.value("fringe_front_mm", 25.0f) / 1000, fringeRootBack = c.value("fringe_root_back_mm", 40.0f) / 1000;
    if (fringeMode)
        for (size_t i = 0; i < lanes.size(); i++) {
            V3 r0 = lanes[i].p[0], tp = lanes[i].p[47];
            if (fwd3 * (r0.y - C.y) < -fringeRootBack) { frRej[0]++; continue; }
            if (fwd3 * (tp.y - C.y) < fringeFront) { frRej[1]++; continue; }
            if (tp.z > r0.z - fringeDrop || tp.z < pk.headTop - 0.2f) { frRej[2]++; continue; }
            if (onScalpE(tp)) { frRej[3]++; continue; }
            fringeLane[i] = 1; nFringeLanes++;
        }
    // nape_cut: lowest vanilla card roots per azimuth bin on the back
    std::vector<float> napeZ(16, -1e9f);
    if (napeCut) {
        std::vector<std::vector<float>> bz(16);
        for (auto& ln : lanes) {
            V3 p = ln.p[0];
            if (fwd3 * (p.y - C.y) > 0.02f || std::fabs(headSD(p, nullptr)) > 0.012f) continue;
            float az = std::atan2(p.y - C.y, p.x - C.x);
            int b = std::clamp(int((az + 3.14159265f) / (2 * 3.14159265f) * 16), 0, 15);
            bz[b].push_back(p.z);
        }
        for (int b = 0; b < 16; b++) if (bz[b].size() >= 3) {
            std::sort(bz[b].begin(), bz[b].end());
            napeZ[b] = bz[b][std::min(bz[b].size() - 1, size_t(napePct * bz[b].size()))] - napeOff;
        }
        for (int pass = 0; pass < 16; pass++)   // fill empty bins from neighbours
            for (int b = 0; b < 16; b++) if (napeZ[b] < -1e8f) {
                float l = napeZ[(b + 15) % 16], rr = napeZ[(b + 1) % 16];
                if (l > -1e8f || rr > -1e8f) napeZ[b] = std::max(l, rr);
            }
    }
    auto belowNape = [&](V3 p) {
        if (!napeCut || fwd3 * (p.y - C.y) > 0.02f) return false;
        float az = std::atan2(p.y - C.y, p.x - C.x);
        float f = (az + 3.14159265f) / (2 * 3.14159265f) * 16 - 0.5f;
        int b0 = (int)std::floor(f); float w = f - b0;
        float z0 = napeZ[(b0 + 16) % 16], z1 = napeZ[(b0 + 17) % 16];
        if (z0 < -1e8f || z1 < -1e8f) return false;
        return p.z < z0 * (1 - w) + z1 * w;
    };
    // alpha_density: mean card alpha in the column above a skin point
    Grid adg(0.01f);
    for (size_t i = 0; i < adS.size(); i++) adg.add(adS[i].first, (int)i);
    auto alphaCol = [&](V3 p) {
        V3 n; headSD(p, &n);
        float colH = inner ? raise : 0.03f, s = 0, w = 0;
        adg.around(p, adR + colH, [&](int i) {
            V3 d = adS[i].first - p; float h = dot(d, n);
            if (h < -0.004f || h > colH || len(d - n * h) > adR) return;
            s += adS[i].second; w += 1;
        });
        return w > 0 ? s / w : 0.0f;
    };
    int nNape = 0, nAlphaDrop = 0, nPartAdd = 0, nAlphaRoot = 0;
    int nFringe = 0, nPartComb = 0, nClumps = 0, nFly = 0, nPartFaces = 0;
    std::vector<float> candCov, candAlpha;
    // candidate scalp points (head mesh, upper part, within reach of visible card hair, in the region)
    std::vector<V3> cand;
    {
        std::vector<double> acc; double tot = 0;
        for (auto& t : head->t) {
            V3 ctr = (head->v[t[0]] + head->v[t[1]] + head->v[t[2]]) * (1.0f / 3);
            double ar = ctr.z > pk.headTop - zminRel - 0.02f ? 0.5 * len(cross(head->v[t[1]] - head->v[t[0]], head->v[t[2]] - head->v[t[0]])) : 0;
            tot += ar; acc.push_back(tot);
        }
        std::mt19937 rs(1);
        for (int s = 0; s < 80000 && tot > 0; s++) {
            double x = std::uniform_real_distribution<double>(0, tot)(rs);
            size_t ti = std::min(head->t.size() - 1, size_t(std::lower_bound(acc.begin(), acc.end(), x) - acc.begin()));
            auto& t = head->t[ti];
            float uu = std::uniform_real_distribution<float>(0, 1)(rs), vv = std::uniform_real_distribution<float>(0, 1)(rs);
            if (uu + vv > 1) { uu = 1 - uu; vv = 1 - vv; }
            V3 X = head->v[t[0]] + (head->v[t[1]] - head->v[t[0]]) * uu + (head->v[t[2]] - head->v[t[0]]) * vv;
            if (X.z < pk.headTop - zminRel || !inRegion(X)) continue;
            bool ok = false;
            if (inner) {
                V3 nx; headSD(X, &nx);
                vg.around(X, reach + raise, [&](int i) {
                    if (ok) return;
                    V3 d = vis[i] - X; float h = dot(d, nx);
                    if (h > -0.004f && h < raise && len(d - nx * h) < reach) ok = true;
                });
            } else vg.around(X, reach, [&](int i) { if (!ok && len(vis[i] - X) < reach) ok = true; });
            float alphaKeep = 1.0f;
            if (!ok && partDetect) { V3 aw; if (partAway(X, aw) && onScalpE(X)) { ok = true; nPartAdd++; } }
            if (ok && partDetect && partSide(X) != 0 && std::fabs(partDx(X)) < partGap / 2) ok = false;
            if (ok && napeCut && belowNape(X)) { nNape++; ok = false; }
            if (ok && alphaDensity) {
                float ac = alphaCol(X);
                float t = std::clamp((ac - adLo) / std::max(adHi - adLo, 1e-3f), 0.0f, 1.0f); t = t * t * (3 - 2 * t);
                if (t <= 0) { nAlphaDrop++; ok = false; } else alphaKeep = t;
            }
            if (ok && !onScalpE(X)) { if (growOk(X)) nGrown++; else { offScalp++; ok = false; } }
            if (ok && hk && hk->excludeRoot && hk->excludeRoot(X)) { nExRoot++; ok = false; }
            if (ok && coverage) {
                float cv = covAt(X);
                if (cv < covMin) ok = false; else candCov.push_back(cv);
            }
            if (ok) { cand.push_back(X); if (alphaDensity) candAlpha.push_back(alphaKeep); }
        }
    }
    if (cand.empty()) { r["error"] = "no scalp point under visible card hair in this region"; return r; }
    // old guides: which go (replace) and which stay
    std::vector<int> oldAlive, gone, stay;
    for (size_t i = 0; i < gr.P.size(); i++) if (gr.alive[i]) oldAlive.push_back((int)i);
    for (int g : oldAlive) (replace && inRegion(gr.P[g][0]) ? gone : stay).push_back(g);
    HeadRadial hr = headRadial(a);
    int id = 0;
    for (auto& o : gr.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    int grp = 1000 + (++grpId);
    // uv of a point: closest current growth face (barycentric)
    auto uvAt = [&](V3 p) -> std::array<float, 2> {
        float bd = 1e9f; std::array<float, 2> best{0.5f, 0.5f};
        for (auto& F : gr.faces) {
            V3 A = gr.P[F.g[0]][0], B = gr.P[F.g[1]][0], Cc = gr.P[F.g[2]][0];
            V3 n = cross(B - A, Cc - A);
            float nn = dot(n, n);
            if (nn < 1e-14f) continue;
            V3 q = p - n * (dot(p - A, n) / nn);
            float w2 = dot(cross(B - A, q - A), n) / nn, w0 = dot(cross(Cc - B, q - B), n) / nn, w1 = 1 - w0 - w2;
            w0 = std::max(w0, 0.0f); w1 = std::max(w1, 0.0f); w2 = std::max(w2, 0.0f);
            float s = w0 + w1 + w2; w0 /= s; w1 /= s; w2 /= s;
            float d = len(p - (A * w0 + B * w1 + Cc * w2));
            if (d < bd) { bd = d; best = {w0 * F.uv[0] + w1 * F.uv[2] + w2 * F.uv[4], w0 * F.uv[1] + w1 * F.uv[3] + w2 * F.uv[5]}; }
        }
        return best;
    };
    // growth uv of the new guides. Builds with a density map (pixel density from the card alpha, colour from the card
    // diffuse) take density / colour at the interpolated face uv: per-guide uvs from the old faces cross card gaps
    // (patchy, bald spots). All new guides get ONE uv on a flat texel whose density = the build's mean face density
    // (so the budget estimate holds) and whose colour is the median hair colour. uv_mode "nearest" = old behaviour.
    bool oneUv = false; std::array<float, 2> theUv{0.5f, 0.5f};
    if (c.value("uv_mode", std::string("uniform")) == "uniform" && !pk.densityFile.empty()) {
        int w = 0, h = 0, cw = 0, ch = 0;
        unsigned char* dm = loadImage(pk.dir + "\\" + pk.densityFile, &w, &h);
        unsigned char* cm = pk.colorFile.empty() ? nullptr : loadImage(pk.dir + "\\" + pk.colorFile, &cw, &ch);
        if (dm) {
            int chn = pk.densityChannel == "GREEN" ? 1 : pk.densityChannel == "BLUE" ? 2 : pk.densityChannel == "ALPHA" ? 3 : 0;
            double mean = 0;
            for (float v : pk.densityFace) mean += v;
            float target = c.contains("density") ? c["density"].get<float>() : pk.densityFace.empty() ? 1.0f : float(mean / pk.densityFace.size());
            auto D = [&](int x, int y) { x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1); return dm[(size_t(y) * w + x) * 4 + chn] / 255.0f; };
            auto lum = [&](float u, float v) {
                if (!cm) return 0.0f;
                int x = std::clamp(int(u * cw), 0, cw - 1), y = std::clamp(int(v * ch), 0, ch - 1);
                const unsigned char* q = &cm[(size_t(y) * cw + x) * 4];
                return 0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2];
            };
            std::vector<float> lums;
            int step = std::max(1, std::min(w, h) / 256);
            // the colour the build shows: the colour texture at the build's own growth-face corners
            for (auto& fu : pk.uv) for (int q = 0; q < 3; q++) { float u = fu[2 * q] - std::floor(fu[2 * q]), v = fu[2 * q + 1] - std::floor(fu[2 * q + 1]); lums.push_back(lum(u, v)); }
            float medL = 0;
            if (!lums.empty()) { std::nth_element(lums.begin(), lums.begin() + lums.size() / 2, lums.end()); medL = lums[lums.size() / 2]; }
            float bestS = 1e9f;
            for (int y = 2; y < h - 2; y += step)
                for (int x = 2; x < w - 2; x += step) {
                    float d0 = D(x, y), var = 0;
                    for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) var = std::max(var, std::fabs(D(x + dx, y + dy) - d0));
                    float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
                    float sc = std::fabs(d0 - target) * 4 + var * 2 + (cm ? std::fabs(lum(u, v) - medL) / 255.0f : 0);
                    if (sc < bestS) { bestS = sc; theUv = {u, v}; }
                }
            oneUv = true;
            r["uv"] = {{"uv", {r4(theUv[0]), r4(theUv[1])}}, {"density", r4(D(int(theUv[0] * w), int(theUv[1] * h)))}, {"target", r4(target)}};
            freeImage(dm);
        }
        if (cm) freeImage(cm);
    }
    // eyes (meshes with an eye texture): a strand passing within eye_mm of an eyeball pokes the eye
    std::vector<V3> eyeV;
    for (auto& em : pk.meshes) if (em.texFile.find("eye") != std::string::npos) for (auto& v : em.v) eyeV.push_back(v);
    float eyeM = c.value("eye_mm", 10.0f) / 1000;
    auto overFaceG = [&](const Guide& G) {
        for (size_t k = 1; k < G.size(); k++)
            for (auto& e : eyeV) if (len(G[k] - e) < eyeM) return true;
        return false;
    };
    // ---- trace flow: streamline from root R through the card flow field, length Lt, layer fraction fl
    auto traceFlow = [&](V3 R, float Lt, float fl, Guide& out, int startQ) -> bool {
        V3 nR; headSD(R, &nR);
        // start direction: lane points in the column above the root (lanes that start near it weigh more), clustered
        // (two passes weighting by agreement) so a parting's two opposite flows do not average out
        std::vector<std::pair<int, float>> nb;
        const float colH = inner ? raise : 0.03f;
        dg.around(R, flowR + colH, [&](int q) {
            V3 d = dense[q] - R; float h = dot(d, nR);
            if (h < -0.006f || h > colH) return;
            float lat = len(d - nR * h);
            if (lat > flowR) return;
            float w = std::exp(-lat * lat / (0.5f * flowR * flowR)) * (rootBias > 0 ? std::exp(-denseB[q] / rootBias) : 1.0f) *
                      std::exp(-std::max(0.0f, h) / (inner ? std::max(innerBias, 1e-4f) : 0.015f));
            nb.push_back({q, w});
        });
        if (nb.empty()) return false;
        V3 d0{0, 0, 0}; float wsum = 0;
        for (auto& [q, w] : nb) { d0 = d0 + denseT[q] * w; wsum += w; }
        const float agree = len(d0) / std::max(wsum, 1e-9f);   // ~1 one flow, low = whorl / parting (flows diverge)
        for (int it = 0; it < 2 && len(d0) > 1e-9f; it++) {
            V3 dn = nrmz(d0); d0 = V3{0, 0, 0};
            for (auto& [q, w] : nb) { float al = std::max(0.0f, dot(denseT[q], dn)); d0 = d0 + denseT[q] * (w * al * al); }
        }
        if (len(d0) < 1e-9f) return false;
        V3 D = nrmz(d0);
        // flow_start lane: start along the picked lane (balanced pick as the lane trace: minority flows such as a
        // fringe layered over the top keep their share), blended with the cluster mean by flow_start_mix
        if (startQ >= 0 && flowStartLane) D = nrmz(denseT[startQ] * flowStartMix + D * (1 - flowStartMix));
        V3 awF; const bool partR = partDetect && partAway(R, awF);
        // part_cross_mm: roots this close to the line cross it (then follow the far side): the line is covered, thin
        if (partR && partCross > 0 && std::fabs(partDx(R)) < partCross) awF = awF * -1.0f;
        if (partR) { D = nrmz(awF * partComb + D * (1 - partComb)); if (dot(D, awF) < 0.2f) D = nrmz(D + awF * (0.2f - dot(D, awF))); nPartComb++; }
        // flow_whorl: at a whorl / parting every strand leaves the centre and the skin there shows. A share of
        // those roots starts against the flow: it crosses the centre, then follows the flow on the far side.
        if (flowWhorl > 0 && agree < flowWhorlAgree && !partR) {
            float u = std::sin(R.x * 39346.1f + R.y * 11113.7f + R.z * 52711.3f) * 24634.6345f; u -= std::floor(u);
            if (u < flowWhorl) { D = D * -1.0f; nWhorl++; }
        }
        std::vector<V3> pts{R};
        V3 X = R;
        float s = 0, miss = 0;
        while (s < Lt - 1e-4f) {
            float h = std::min(flowStep, Lt - s);
            V3 acc{0, 0, 0}; float ws = 0, hsum = 0, rsum = 0;
            auto gather = [&](float rad) {
                dg.around(X, rad, [&](int q) {
                    V3 d = dense[q] - X; float dd = dot(d, d);
                    if (dd > rad * rad) return;
                    float al = dot(denseT[q], D);
                    if (al <= 0) return;   // only the flow going our way (partings, crossing layers)
                    float w = std::exp(-dd / (0.5f * rad * rad)) * al * al;
                    acc = acc + denseT[q] * w; ws += w; hsum += denseH[q] * w; rsum += rem[q] * w;
                });
            };
            gather(flowR);
            bool wide = false;
            if (ws < 1e-3f) { acc = V3{0, 0, 0}; ws = 0; hsum = 0; rsum = 0; gather(2 * flowR); wide = true; }
            V3 Dn = D; float Hloc = -1;
            if (ws > 1e-6f) {
                Dn = nrmz(acc); Hloc = hsum / ws;
                miss = wide ? miss + 0.5f * h : 0;
                // the lanes around end here (card tips): the strand ends too (no tails past the cards)
                if (flowTipStop && s > 0.25f * Lt && rsum / ws < 0.5f * h) break;
            } else { miss += h; }
            if (miss > flowR) break;   // past the card hair: the strand ends
            D = nrmz(D * flowInertia + Dn * (1 - flowInertia));
            V3 Y = X + D * h;
            V3 nY; float sdY = headSD(Y, &nY);
            if (Y.z > pk.headTop - 0.16f && sdY < hugBand + 0.02f) {
                // hold this strand's share of the local card layer height, rising from the root over flow_rise_mm
                float ramp = std::min(1.0f, (s + h) / std::max(flowRise, 1e-4f)); ramp = ramp * ramp * (3 - 2 * ramp);
                // off the painted scalp (face, temples, cheeks) no upper layer covers a low strand: it lies on the skin
                // as a dark smudge, and HairWorks interpolates under the skin there. Hold the full card height instead.
                float flE = (flowFaceFull && !onScalpE(Y)) ? std::max(fl, 1.0f) : fl;
                float tgt = Hloc >= 0 ? 0.0025f + flE * flowHeight * std::max(0.0f, Hloc - 0.0025f) : std::max(sdY, 0.0025f);
                if (flE > fl) tgt = std::max(tgt, flowClear);
                if (partR) tgt = std::max(tgt, partClear);   // part_detect: the combed strands stand clear of the skin
                float want = 0.0025f + (tgt - 0.0025f) * ramp;
                Y = Y + nY * ((want - sdY) * flowHold);
            }
            if (sdY < 0.002f && sdY > -0.05f) Y = Y + nY * (0.002f - sdY);
            Y = X + nrmz(Y - X) * h;
            D = nrmz(Y - X);
            pts.push_back(Y); X = Y; s += h;
        }
        if (pts.size() < 4 || s < 0.012f) return false;
        out = resampleN(pts, K);
        return true;
    };
    auto layerOf = [&](V3 R) {   // per-root layer fraction (deterministic hash of the root)
        float u = std::sin(R.x * 12989.8f + R.y * 78233.0f + R.z * 37719.0f) * 43758.5453f;
        u -= std::floor(u);
        return flowLayerMin + (flowLayerMax - flowLayerMin) * u;
    };
        int eyeCutN = 0, earRouted = 0, earDropped = 0, flowFail = 0, earFaces = 0;
    // eye_mode cut: the strand is cut before the first point within eye_mm of an eyeball (rejected when < 4 points stay)
    auto eyeCutG = [&](Guide& G) -> bool {
        for (size_t k = 1; k < G.size(); k++) {
            bool near_ = false;
            for (auto& e : eyeV) if (len(G[k] - e) < eyeM) { near_ = true; break; }
            if (!near_) continue;
            if (k < 4) return false;
            std::vector<V3> part(G.begin(), G.begin() + k);
            G = resampleN(part, K);
            eyeCutN++;
            return true;
        }
        return true;
    };
    // ---- one attempt at a spacing -> op
    json stats;
    auto attempt = [&](float spc) -> json {
        std::mt19937 rp(7);
        eyeCutN = earRouted = earDropped = flowFail = earFaces = nWhorl = 0; nFringe = nAlphaRoot = nPartComb = nClumps = nFly = nPartFaces = 0;
        std::vector<int> order(cand.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
        std::shuffle(order.begin(), order.end(), std::mt19937(1));
        Grid pg(spc);
        std::vector<V3> taken;
        for (int g : stay) { taken.push_back(gr.P[g][0]); pg.add(gr.P[g][0], (int)taken.size() - 1); }
        std::vector<V3> roots;
        for (int i : order) {
            V3 X = cand[i];
            bool ok = true;
            // coverage: local spacing from the card coverage above the root (thin cards = sparser roots)
            float sp = coverage ? spc / std::sqrt(std::clamp(candCov[i] / std::max(covRef, 1e-3f), covFloor, 1.0f)) : spc;
            pg.around(X, sp, [&](int q) { if (ok && len(taken[q] - X) < sp) ok = false; });
            if (!ok) continue;
            taken.push_back(X); pg.add(X, (int)taken.size() - 1);
            // alpha_density: the root survives with the column's opacity (its spot stays taken: thin, not refilled)
            if (alphaDensity && hsh(X, 3.1f) >= candAlpha[i]) { nAlphaRoot++; continue; }
            roots.push_back(X);
        }
        std::vector<Guide> NG;
        std::vector<V3> orphanRoots;
        int noLane = 0, throughHead = 0, onFace = 0, copied = 0;
        for (V3 R : roots) {
            V3 awayP{0, 0, 0}; const bool isPart = partDetect && partAway(R, awayP);
            Guide G; bool fringeG = false;
            // fringe: a front root near a fringe lane copies it (from the hairline, forward / side-swept over the brow)
            if (fringeMode && fwd3 * (R.y - C.y) > -fringeRootBack) {
                std::map<int, std::pair<int, float>> fb;
                dg.around(R, fringeReach, [&](int q) {
                    if (!fringeLane[sid[q]] || rem[q] < minRem) return;
                    float d = len(dense[q] - R);
                    if (d >= fringeReach) return;
                    float sc = rem[q] - 2.0f * d;
                    auto it = fb.find(sid[q]);
                    if (it == fb.end() || sc > it->second.second) fb[sid[q]] = {q, sc};
                });
                if (!fb.empty()) {
                    double tot = 0;
                    for (auto& [l, v] : fb) tot += std::max(rem[v.first], 1e-3f);
                    double x = std::uniform_real_distribution<double>(0, tot)(rp);
                    int pick = -1;
                    for (auto& [l, v] : fb) { x -= std::max(rem[v.first], 1e-3f); pick = v.first; if (x <= 0) break; }
                    const Lane& ln = lanes[sid[pick]];
                    std::vector<V3> part(ln.p.begin() + jj[pick], ln.p.end());
                    G = resampleN(part, K);
                    V3 off = R - G[0];
                    for (int k = 0; k < K; k++) { float t = float(k) / (K - 1); G[k] = G[k] + off * (keepOff + (1 - keepOff) * (1 - t) * (1 - t)); }
                    G[0] = R;
                    for (int k = 1; k < K; k++) {
                        V3 nn; float sd = headSD(G[k], &nn);
                        float want = fringeClear * std::min(1.0f, k / std::max(1.0f, 0.2f * (K - 1)));
                        if (sd < want && sd > -0.05f) G[k] = G[k] + nn * (want - sd);
                    }
                    fringeG = true; nFringe++;
                }
            }
            if (!fringeG) {
            std::vector<int> cs;
            std::unordered_map<int, float> hq;   // inner layer: height of the lane point above the root
            if (inner) {
                V3 nr; headSD(R, &nr);
                dg.around(R, nearM + raise, [&](int q) {
                    V3 d = dense[q] - R; float h = dot(d, nr);
                    if (h > -0.004f && h < raise && len(d - nr * h) < nearM) { cs.push_back(q); hq[q] = std::max(0.0f, h); }
                });
            } else dg.around(R, nearM, [&](int q) { if (len(dense[q] - R) < nearM) cs.push_back(q); });
            if (cs.empty()) {
                int bq = -1; float bd = maxd;
                dg.around(R, maxd, [&](int q) { float d = len(dense[q] - R); if (d < bd) { bd = d; bq = q; } });
                if (bq >= 0) cs.push_back(bq);
            }
            // per lane its best point (score = remaining length - 2 * distance)
            std::map<int, std::pair<int, float>> best;
            for (int q : cs) {
                if (rem[q] < minRem) continue;
                if (isPart && dot(laneT[q], awayP) < -0.2f) continue;   // part_detect: only the flow leaving the line
                float sc = rem[q] - 2.0f * len(dense[q] - R);
                auto it = best.find(sid[q]);
                if (it == best.end() || sc > it->second.second) best[sid[q]] = {q, sc};
            }
            if (best.empty()) { orphanRoots.push_back(R); continue; }
            int pick = -1;
            auto wOf = [&](int q) {
                float before = jj[q] * lanes[sid[q]].L / 47;   // lane length before this point (0 = the lane starts here)
                float w = std::max(rem[q], 1e-3f) * (rootBias > 0 ? std::exp(-before / rootBias) : 1.0f);
                if (inner) { auto it = hq.find(q); if (it != hq.end()) w *= std::exp(-it->second / std::max(innerBias, 1e-4f)); }
                return w;
            };
            if (balanced) {
                double tot = 0;
                for (auto& [l, v] : best) tot += wOf(v.first);
                double x = std::uniform_real_distribution<double>(0, tot)(rp);
                for (auto& [l, v] : best) { x -= wOf(v.first); pick = v.first; if (x <= 0) break; }
            } else {
                float bs = -1e9f;
                for (auto& [l, v] : best) if (v.second > bs) { bs = v.second; pick = v.first; }
            }
            const Lane& ln = lanes[sid[pick]];
            if (flow) {
                if (!traceFlow(R, rem[pick] * flowLenScale, isPart ? std::min(layerOf(R), partLayer) : layerOf(R), G, pick)) { flowFail++; orphanRoots.push_back(R); continue; }
            } else {
            std::vector<V3> part(ln.p.begin() + jj[pick], ln.p.end());
            G = resampleN(part, K);
            V3 off = R - G[0];
            for (int k = 0; k < K; k++) { float t = float(k) / (K - 1); G[k] = G[k] + off * (keepOff + (1 - keepOff) * (1 - t) * (1 - t)); }
            G[0] = R;
            }
            }   // !fringeG
            // hug: the layer near the skin (3.5 cm band) is pressed toward the scalp (sd -> 3 mm + hug * sd)
            if (hugK < 1.0f && !flow && !fringeG) {
                auto sl = arcLen(G);
                for (int k = 1; k < K; k++) {
                    V3 nrm; float sd = headSD(G[k], &nrm);
                    if (sd > hugBand || G[k].z < pk.headTop - 0.16f) continue;
                    float want = std::max(0.003f, 0.003f + hugK * sd);
                    G[k] = G[k] + nrm * (want - sd);
                }
                Guide R2 = G;   // keep segment lengths
                for (int k = 1; k < K; k++) R2[k] = R2[k - 1] + nrmz(G[k] - R2[k - 1]) * (sl[k] - sl[k - 1]);
                G = R2;
            }
            Guide T = G;
            int inside = pushOut(hr, T, 0.003f);
            if (inside > K / 3) { throughHead++; continue; }
            if (inside) G = T;
            if (!keepFace && eyeCut && !eyeCutG(G)) { onFace++; continue; }
            if (!keepFace && overFaceG(G)) { onFace++; continue; }
            if (hk && hk->post) { int pr = hk->post(G); if (pr < 0) { earDropped++; continue; } if (pr > 0) earRouted++; }
            NG.push_back(G);
        }
        // roots with no card lane near them (partings, hairline gaps between cards): copy of the nearest traced guide,
        // so the scalp there is not left without hair
        {
            size_t nTraced = NG.size();
            float copyMax = c.value("copy_mm", 25.0f) / 1000;
            for (V3 R : orphanRoots) {
                int bi = -1; float bd = copyMax;
                for (size_t i = 0; i < nTraced; i++) { float d = len(NG[i][0] - R); if (d < bd) { bd = d; bi = (int)i; } }
                if (bi < 0) { noLane++; continue; }
                Guide G = NG[bi];
                V3 off = R - G[0];
                for (auto& p : G) p = p + off;
                if (pushOut(hr, G, 0.003f) > K / 3) { throughHead++; continue; }
                if (!keepFace && eyeCut && !eyeCutG(G)) { onFace++; continue; }
                if (!keepFace && overFaceG(G)) { onFace++; continue; }
                if (hk && hk->post) { int pr = hk->post(G); if (pr < 0) { earDropped++; continue; } if (pr > 0) earRouted++; }
                NG.push_back(G);
                copied++;
            }
        }
        // clump_var: varied clumps (tips pulled to the clump's centre strand = tapered points), varied length, flyaways
        if (clumpVar > 0 && !NG.empty()) {
            const int n = (int)NG.size();
            auto trimTo = [&](Guide& G, float f) {
                if (f >= 0.999f) return;
                auto sl = arcLen(G); float Lt = sl.back() * f;
                std::vector<V3> part{G[0]};
                for (int k = 1; k < K; k++) {
                    if (sl[k] <= Lt) { part.push_back(G[k]); continue; }
                    float w = (Lt - sl[k - 1]) / std::max(sl[k] - sl[k - 1], 1e-9f);
                    part.push_back(G[k - 1] + (G[k] - G[k - 1]) * w); break;
                }
                if (part.size() >= 2) G = resampleN(part, K);
            };
            // clumps: greedy over a hashed order, each centre with its own radius
            std::vector<int> ord(n), cl(n, -1);
            for (int i = 0; i < n; i++) ord[i] = i;
            std::sort(ord.begin(), ord.end(), [&](int x, int y) { return hsh(NG[x][0], 5.3f) < hsh(NG[y][0], 5.3f); });
            Grid rg(0.01f);
            for (int i = 0; i < n; i++) rg.add(NG[i][0], i);
            for (int ci : ord) {
                if (cl[ci] >= 0) continue;
                cl[ci] = ci; nClumps++;
                float rad = clumpMin + (clumpMax - clumpMin) * hsh(NG[ci][0], 6.7f);
                rg.around(NG[ci][0], rad, [&](int j) { if (cl[j] < 0 && len(NG[j][0] - NG[ci][0]) < rad) cl[j] = ci; });
            }
            // card-edge strands: few visible card points around the root
            std::vector<int> vc(n, 0);
            for (int i = 0; i < n; i++) vg.around(NG[i][0], 0.008f, [&](int q) { if (len(vis[q] - NG[i][0]) < 0.008f) vc[i]++; });
            std::vector<int> vs = vc; std::sort(vs.begin(), vs.end());
            const int edgeN = std::max(1, int(0.35f * vs[vs.size() / 2]));
            std::vector<Guide> src = NG;
            for (int i = 0; i < n; i++) {
                Guide& G = NG[i];
                const Guide& Cg = src[cl[i]];
                if (cl[i] != i) {
                    V3 off = G[0] - Cg[0];
                    for (int k = 1; k < K; k++) {
                        float t = float(k) / (K - 1);
                        V3 tgt = Cg[k] + off * (1 - t);
                        G[k] = G[k] + (tgt - G[k]) * (clumpPull * std::pow(t, 1.5f));
                    }
                }
                float lf = 1.0f - lenVar * hsh(G[0], 8.9f);
                bool fly = vc[i] < edgeN && hsh(G[0], 11.3f) < flyFrac;
                if (fly) {
                    V3 n0; headSD(G[0], &n0);
                    V3 dir = nrmz(G[K - 1] - G[0]);
                    V3 side = nrmz(cross(n0, dir)) * (hsh(G[0], 13.1f) < 0.5f ? -1.0f : 1.0f);
                    for (int k = 1; k < K; k++) { float t = float(k) / (K - 1); G[k] = G[k] + side * (flyMm * t * t) + n0 * (0.3f * flyMm * t * t); }
                    lf *= 0.7f; nFly++;
                }
                trimTo(G, lf);
                Guide T = G;
                if (pushOut(hr, T, 0.003f)) G = T;
            }
        }
        // growth faces: Delaunay of kept + new roots in the polar map around the head centre (growth.faces_for)
        std::vector<V3> RR; std::vector<int> who;   // who: >= 0 new guide index, < 0 -(old guide + 1)
        for (int g : stay) { RR.push_back(gr.P[g][0]); who.push_back(-(g + 1)); }
        for (size_t i = 0; i < NG.size(); i++) { RR.push_back(NG[i][0]); who.push_back((int)i); }
        std::vector<std::array<double, 2>> xy;
        for (auto& p : RR) {
            V3 d = p - C; float rr = std::max(len(d), 1e-6f);
            double th = std::acos(std::clamp(d.z / rr, -1.0f, 1.0f)), ph = std::atan2(d.y, d.x);
            xy.push_back({th * std::cos(ph), th * std::sin(ph)});
        }
        const float spf = coverage ? spc / std::sqrt(std::max(covFloor, 0.05f)) : spc;   // coverage: the sparsest local spacing
        float edgeAbs = std::max(c.value("edge_mm", 34.0f) / 1000, 3.0f * spf), rmax = std::max(0.022f, 2.2f * spf);
        float minArea = std::min(1e-5f, 0.05f * spc * spc);
        std::set<std::array<int, 3>> oldF;
        for (auto& f : gr.faces) { auto q = f.g; std::sort(q.begin(), q.end()); oldF.insert(q); }
        // kept old faces (2D) to avoid overlapping them with new triangles at the region border
        std::vector<std::array<std::array<double, 2>, 3>> keptOld;
        std::map<int, int> posOf;
        for (size_t i = 0; i < who.size(); i++) if (who[i] < 0) posOf[-who[i] - 1] = (int)i;
        std::set<int> goneS(gone.begin(), gone.end());
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
                bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
                if (!(neg && pos)) return true;
            }
            return false;
        };
        json faces = json::array();
        std::vector<char> used(NG.size(), 0);
        for (auto& t : delaunay(xy)) {
            if (who[t[0]] < 0 && who[t[1]] < 0 && who[t[2]] < 0) continue;   // old-only: the old faces stay as they are
            V3 A = RR[t[0]], B = RR[t[1]], D = RR[t[2]];
            float la = len(B - D), lb = len(A - D), lc = len(A - B);
            float e = std::max({la, lb, lc}), ar = 0.5f * len(cross(B - A, D - A));
            if (e > edgeAbs || ar <= minArea || la * lb * lc / std::max(4 * ar, 1e-12f) > rmax) continue;
            if (partDetect && partSplit) {   // a face across the part line interpolates hair from both combs: none
                int s0 = partSide(A), s1 = partSide(B), s2 = partSide(D);
                if (s0 * s1 < 0 || s1 * s2 < 0 || s0 * s2 < 0) { nPartFaces++; continue; }
            }
            if (!keptOld.empty() && insideOld((xy[t[0]][0] + xy[t[1]][0] + xy[t[2]][0]) / 3, (xy[t[0]][1] + xy[t[1]][1] + xy[t[2]][1]) / 3)) continue;
            if (hk && hk->faceBlocked) {   // HairWorks grows hair between the 3 guides: none of it may cross an ear
                auto gOf = [&](int v) -> const Guide& { return who[v] >= 0 ? NG[who[v]] : gr.P[-who[v] - 1]; };
                const Guide &g0 = gOf(t[0]), &g1 = gOf(t[1]), &g2 = gOf(t[2]);
                Guide m3(K), m01(K), m12(K), m20(K);
                for (int k = 0; k < K; k++) {
                    m3[k] = (g0[k] + g1[k] + g2[k]) * (1.0f / 3);
                    m01[k] = (g0[k] + g1[k]) * 0.5f; m12[k] = (g1[k] + g2[k]) * 0.5f; m20[k] = (g2[k] + g0[k]) * 0.5f;
                }
                if (hk->faceBlocked(m3) || hk->faceBlocked(m01) || hk->faceBlocked(m12) || hk->faceBlocked(m20)) { earFaces++; continue; }
            }
            std::array<int, 3> q = t;
            if (dot(A - C, cross(B - A, D - A)) < 0) std::swap(q[1], q[2]);   // outward winding
            json fj = json::array();
            for (int v : q) {
                if (who[v] >= 0) { fj.push_back(who[v]); used[who[v]] = 1; }
                else { V3 p = gr.P[-who[v] - 1][0]; fj.push_back({r4(p.x), r4(p.y), r4(p.z)}); }
            }
            faces.push_back(fj);
        }
        // guides without any face would render bare: drop them (indices remapped)
        std::vector<int> remap(NG.size(), -1);
        json guides = json::array(), base = json::array(), uvs = json::array();
        int orphan = 0;
        for (size_t i = 0; i < NG.size(); i++) {
            if (!used[i]) { orphan++; continue; }
            remap[i] = (int)guides.size();
            guides.push_back(ptsJson(NG[i]));
            // base (skinning / fur-frame offset): the nearest old guide
            int bg = -1; float bd = 1e9f;
            for (int g : oldAlive) { float d = len(gr.P[g][0] - NG[i][0]); if (d < bd) { bd = d; bg = g; } }
            V3 bp = gr.P[bg][0];
            base.push_back({r4(bp.x), r4(bp.y), r4(bp.z)});
            auto uv = oneUv ? theUv : uvAt(NG[i][0]);
            uvs.push_back({r4(uv[0]), r4(uv[1])});
        }
        for (auto& f : faces) for (auto& v : f) if (v.is_number_integer()) v = remap[v.get<int>()];
        json del = json::array();
        for (int g : gone) { V3 p = gr.P[g][0]; del.push_back({r4(p.x), r4(p.y), r4(p.z)}); }
        stats = {{"spacing_mm", std::round(spc * 10000) / 10}, {"roots", roots.size()}, {"new_guides", guides.size()},
                 {"new_faces", faces.size()}, {"deleted_guides", gone.size()}, {"kept_guides", stay.size()},
                 {"copied_from_neighbour", copied}, {"rejected", {{"no_lane_near_root", noLane}, {"through_head", throughHead}, {"near_eyes", onFace}, {"no_face", orphan}}}};
        if (flow) stats["flow_failed_to_copy"] = flowFail;
        if (eyeCut) stats["eye_cut"] = eyeCutN;
        if (flowWhorl > 0) stats["whorl_crossed"] = nWhorl;
        if (fringeMode) stats["fringe_strands"] = nFringe;
        if (partDetect) { stats["part_combed_flow"] = nPartComb; stats["part_faces_split"] = nPartFaces; }
        if (alphaDensity) stats["alpha_roots_dropped"] = nAlphaRoot;
        if (clumpVar > 0) { stats["clumps"] = nClumps; stats["flyaways"] = nFly; }
        if (hk && hk->post) { stats["ear_routed"] = earRouted; stats["ear_dropped"] = earDropped; }
        if (hk && hk->faceBlocked) stats["ear_faces_dropped"] = earFaces;
        return json{{"op", "rebuild"}, {"delete", del}, {"guides", guides}, {"base", base}, {"uv", uvs}, {"faces", faces}, {"id", ++id}, {"grp", grp}};
    };
    auto ops0 = gr.ops;
    auto tryAt = [&](float spc) {
        gr.ops = ops0;
        gr.ops.push_back(attempt(spc));
        gr.redo.clear(); gr.replay(); a.editsChanged();
        return (int)gr.ops.back()["guides"].size();
    };
    if (coverage && !c.contains("spacing_mm")) spacing = covSpacing;
    int got = tryAt(spacing);
    float target = c.value("budget_target", 1.0f);
    int covSteps = 0;
    if (coverage) {   // coverage sets the count; budget_target (if given) is only a maximum
        while (c.contains("budget_target") && a.bud.ratio > target * 1.02f && covSteps < 12) { spacing *= 1.08f; got = tryAt(spacing); covSteps++; }
    } else if (!c.contains("spacing_mm") && count <= 0 && a.bud.ratio > 0) {
        // default density: the hair budget of the build as opened (budget_target 1.0; faces ~ area / spacing^2)
        for (int it = 0; it < 3; it++) {
            float ratio = float(a.bud.ratio);
            if (std::fabs(ratio - target) < 0.04f * target) break;
            spacing = std::clamp(spacing * std::sqrt(ratio / target), 0.003f, 0.05f);
            got = tryAt(spacing);
        }
    }
    if (count > 0 && got > 0) {   // a target count: rescale the spacing (count ~ area / spacing^2), twice
        for (int it = 0; it < 2 && got > 0; it++) {
            spacing = std::clamp(spacing * std::sqrt(float(got) / float(count)), 0.003f, 0.05f);
            got = tryAt(spacing);
        }
    }
    int budgetSteps = 0;
    while (!ignoreBudget && a.bud.overLimit && budgetSteps < 12) { spacing *= 1.1f; got = tryAt(spacing); budgetSteps++; }
    a.smap.ensure(gr.P.size());
    if (c.value("debug_lanes", false)) {
        json L = json::array();
        for (auto& ln : lanes) L.push_back({v3j(ln.p[0]), v3j(ln.p[24]), v3j(ln.p[47])});
        std::ofstream(fs::path(pk.dir) / "_rebuild_lanes.json") << L.dump();
    }
    json uvInfo = r.is_object() && r.contains("uv") ? r["uv"] : json();
    r = stats;
    if (!uvInfo.is_null()) r["uv"] = uvInfo;
    r["lanes"] = lanes.size(); r["chained_segments"] = nChained; r["stacked_lanes_dropped"] = nDup; r["card_parts"] = nCards; r["islands"] = nIslands;
    r["visible_points"] = vis.size(); r["scalp_candidates"] = cand.size(); r["candidates_off_painted_scalp"] = offScalp;
    r["painted_scalp"] = sm != nullptr;
    if (budgetSteps) r["spacing_raised_for_budget"] = budgetSteps;
    if (coverage) {
        r["coverage"] = {{"base_spacing_mm", std::round(spacing * 10000) / 10}, {"samples", covS.size()}, {"raised_for_budget_target", covSteps}};
        if (!candCov.empty()) {
            std::vector<float> cv = candCov; std::sort(cv.begin(), cv.end());
            r["coverage"]["p10_p50_p90"] = {r4(cv[cv.size() / 10]), r4(cv[cv.size() / 2]), r4(cv[cv.size() * 9 / 10])};
        }
    }
    if (scalpGrow > 0) r["candidates_grown_off_painted"] = nGrown;
    if (hk && hk->excludeRoot) r["candidates_excluded_zones"] = nExRoot;
    if (flow) r["trace"] = "flow";
    if (orientVote) r["islands_reoriented_by_vote"] = nVoted;
    if (partDetect) { r["part_rows"] = partJ; r["part_line_rows"] = partX.size(); r["part_candidates_added"] = nPartAdd; if (!partDbg.empty()) r["part_debug"] = partDbg; }
    if (fringeMode) { r["fringe_lanes"] = nFringeLanes; r["fringe_lanes_rejected"] = {{"root_behind", frRej[0]}, {"tip_not_front", frRej[1]}, {"tip_not_below", frRej[2]}, {"tip_on_scalp", frRej[3]}}; }
    if (napeCut) { json nz = json::array(); for (float z : napeZ) nz.push_back(z < -1e8f ? json(nullptr) : json(r4(z))); r["nape_cut_z"] = nz; r["nape_candidates_cut"] = nNape; }
    if (alphaDensity) { r["alpha_candidates_dropped"] = nAlphaDrop; r["alpha_samples"] = adS.size(); }
    return r;
}

// ------------------------------------------------------------------------------------------------ free_budget,
// fill_below_hairline, centre_parting, bun fill (2026-10-12, opt-in Studio tools). Shared helpers first.
namespace t6 {
using rebuild::Grid;

static int maxOpId(const Groom& g) {
    int id = 0;
    for (auto& o : g.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    return id;
}
static json rootJ(V3 p) { return {r4(p.x), r4(p.y), r4(p.z)}; }

// head skin as stored in the pack (edit frame, as rebuild_from_cards uses it) + painted-scalp flags + outward normals
struct HeadSkin {
    const Mesh* m = nullptr;
    const std::vector<float>* flag = nullptr;
    std::vector<V3> n;
    Grid g{0.01f};
    V3 C{};
    int nearest(V3 p, float maxR = 0.16f) const {
        int bi = -1; float bd = 1e9f;
        for (float r = 0.01f; bi < 0 && r <= maxR; r *= 2)
            g.around(p, r, [&](int i) { float d = len(m->v[i] - p); if (d < bd) { bd = d; bi = i; } });
        return bi;
    }
    float sd(V3 p, V3* nrm = nullptr) const {
        int i = nearest(p);
        if (i < 0) { if (nrm) *nrm = nrmz(p - C); return 1.0f; }
        if (nrm) *nrm = n[i];
        return dot(p - m->v[i], n[i]);
    }
    bool painted(int i) const { return flag && i >= 0 && (*flag)[i] > 0.5f; }
};
static HeadSkin headSkin(const App& a) {
    HeadSkin h;
    h.C = a.pack.headC;
    for (size_t mi = 0; mi < a.pack.meshes.size(); mi++) {
        auto& m = a.pack.meshes[mi];
        if (m.kind != "head") continue;
        auto it = a.headScalp.find(int(mi));
        bool fl = it != a.headScalp.end() && it->second.size() == m.v.size();
        if (!h.m || fl) { h.m = &m; h.flag = fl ? &it->second : nullptr; }
        if (fl) break;
    }
    if (!h.m) return h;
    h.n.assign(h.m->v.size(), V3{0, 0, 0});
    for (auto& t : h.m->t) { V3 nn = cross(h.m->v[t[1]] - h.m->v[t[0]], h.m->v[t[2]] - h.m->v[t[0]]); for (int q = 0; q < 3; q++) h.n[t[q]] = h.n[t[q]] + nn; }
    for (size_t i = 0; i < h.n.size(); i++) { h.n[i] = nrmz(h.n[i]); if (dot(h.m->v[i] - h.C, h.n[i]) < 0) h.n[i] = h.n[i] * -1.0f; }
    for (size_t i = 0; i < h.m->v.size(); i++) h.g.add(h.m->v[i], (int)i);
    return h;
}

// the build's typical hair texel: among its own growth-face corner uvs, the one whose colour (rootColorTex) is nearest
// the median corner colour and, with a density map, whose density is nearest the mean face density. New edge faces of
// the fills take it ("edge_uv": [u, v]); copying the source guide's corner uv gave a blue-grey slab on Ciri (atlas uvs)
static json typicalUv(const App& a) {
    const auto& pk = a.pack;
    if (pk.uv.empty() || pk.colorFile.empty()) return json();
    int cw = 0, ch = 0, w = 0, h = 0;
    unsigned char* cm = loadImage(pk.dir + "\\" + pk.colorFile, &cw, &ch);
    if (!cm) return json();
    unsigned char* dm = pk.densityFile.empty() ? nullptr : loadImage(pk.dir + "\\" + pk.densityFile, &w, &h);
    int chn = pk.densityChannel == "GREEN" ? 1 : pk.densityChannel == "BLUE" ? 2 : pk.densityChannel == "ALPHA" ? 3 : 0;
    auto px = [](unsigned char* im, int W, int H, float u, float v) {
        u -= std::floor(u); v -= std::floor(v);
        return &im[(size_t(std::clamp(int(v * H), 0, H - 1)) * W + std::clamp(int(u * W), 0, W - 1)) * 4];
    };
    std::vector<std::array<float, 5>> cs;   // u, v, r, g, b
    for (auto& fu : pk.uv) for (int q = 0; q < 3; q++) {
        const unsigned char* c = px(cm, cw, ch, fu[2 * q], fu[2 * q + 1]);
        cs.push_back({fu[2 * q], fu[2 * q + 1], float(c[0]), float(c[1]), float(c[2])});
    }
    float med[3];
    for (int k = 0; k < 3; k++) {
        std::vector<float> v; for (auto& x : cs) v.push_back(x[2 + k]);
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); med[k] = v[v.size() / 2];
    }
    double mean = 0; for (float v : pk.densityFace) mean += v;
    float target = pk.densityFace.empty() ? 1.0f : float(mean / pk.densityFace.size());
    float best = 1e30f; std::array<float, 2> uv{0.5f, 0.5f};
    for (auto& x : cs) {
        float sc = std::fabs(x[2] - med[0]) + std::fabs(x[3] - med[1]) + std::fabs(x[4] - med[2]);
        if (dm) sc += 255.0f * 2 * std::fabs(px(dm, w, h, x[0], x[1])[chn] / 255.0f - target);
        if (sc < best) { best = sc; uv = {x[0], x[1]}; }
    }
    freeImage(cm); if (dm) freeImage(dm);
    return json::array({r4(uv[0]), r4(uv[1])});
}

// opt-in "edge_uv": "local" (2026-10-13, Lambert front fill looked grey with the median texel): the uv of the existing
// growth face nearest X (centroid of its roots), so a new strand takes the colour of the hair around it
static json localUv(const Groom& g, const V3& X) {
    float best = 1e30f; json uv;
    for (auto& f : g.faces) {
        if (!g.alive[f.g[0]] || !g.alive[f.g[1]] || !g.alive[f.g[2]]) continue;
        V3 c = (g.P[f.g[0]][0] + g.P[f.g[1]][0] + g.P[f.g[2]][0]) * (1.0f / 3);
        float d = len(c - X);
        if (d < best) { best = d; uv = json::array({r4((f.uv[0] + f.uv[2] + f.uv[4]) / 3), r4((f.uv[1] + f.uv[3] + f.uv[5]) / 3)}); }
    }
    return uv;
}

// eyes, brows and ears (as hairline_fill finds them; forward = the side the eyes are on, +y or -y)
struct Zones {
    V3 C{}, eLo{1e9f, 1e9f, 1e9f}, eHi{-1e9f, -1e9f, -1e9f}, Ca{};
    bool haveEye = false;
    float eyeZ = 0, eyeY = 0, browZ = 0, fwd = 1;
    V3 earLo[2], earHi[2];
    bool hasEar[2] = {false, false};
    bool inEar(V3 p, float pad) const {
        for (int sd = 0; sd < 2; sd++)
            if (hasEar[sd] && p.x > earLo[sd].x - pad && p.x < earHi[sd].x + pad && p.y > earLo[sd].y - pad && p.y < earHi[sd].y + pad &&
                p.z > earLo[sd].z - pad && p.z < earHi[sd].z + pad) return true;
        return false;
    }
    bool overFace(V3 p) const {   // in front of the face below the brows
        return haveEye && fwd * (p.y - eyeY) > -0.03f && p.z < browZ && std::fabs(p.x - C.x) < (eHi.x - eLo.x) / 2 + 0.035f;
    }
};
static Zones zonesOf(const App& a, const json& c) {
    Zones z;
    z.C = a.pack.headC;
    for (auto& m : a.pack.meshes) if (m.texFile.find("eye") != std::string::npos)
        for (auto& v : m.v) { z.eLo = {std::min(z.eLo.x, v.x), std::min(z.eLo.y, v.y), std::min(z.eLo.z, v.z)}; z.eHi = {std::max(z.eHi.x, v.x), std::max(z.eHi.y, v.y), std::max(z.eHi.z, v.z)}; }
    z.haveEye = z.eLo.x < 1e8f;
    z.eyeZ = z.haveEye ? (z.eLo.z + z.eHi.z) / 2 : z.C.z - 0.03f;
    z.eyeY = z.haveEye ? (z.eLo.y + z.eHi.y) / 2 : z.C.y + 0.08f;
    z.fwd = z.eyeY >= z.C.y ? 1.0f : -1.0f;
    z.browZ = z.haveEye ? z.eHi.z + c.value("brow_clear_mm", 12.0f) / 1000 : z.C.z;
    z.Ca = {z.C.x, z.eyeY - z.fwd * 0.075f, z.eyeZ};
    for (int sd = 0; sd < 2; sd++) {
        float best = 0;
        auto lat = [&](V3 v) { return sd == 0 ? v.x - z.C.x : z.C.x - v.x; };
        auto back = [&](V3 v) { return z.fwd * (v.y - z.eyeY); };   // < 0 = behind the eyes
        for (auto& m : a.pack.meshes) if (m.kind == "head") for (auto& v : m.v) {
            if (v.z < z.eyeZ - 0.06f || v.z > z.eyeZ + 0.015f || back(v) > -0.05f || back(v) < -0.15f) continue;
            best = std::max(best, lat(v));
        }
        if (best <= 0) continue;
        z.earLo[sd] = {1e9f, 1e9f, 1e9f}; z.earHi[sd] = {-1e9f, -1e9f, -1e9f};
        for (auto& m : a.pack.meshes) if (m.kind == "head") for (auto& v : m.v) {
            if (lat(v) < best - 0.012f || v.z < z.eyeZ - 0.09f || v.z > z.eyeZ + c.value("ear_top_mm", 50.0f) / 1000 || back(v) > -0.04f || back(v) < -0.17f) continue;
            z.earLo[sd] = {std::min(z.earLo[sd].x, v.x), std::min(z.earLo[sd].y, v.y), std::min(z.earLo[sd].z, v.z)};
            z.earHi[sd] = {std::max(z.earHi[sd].x, v.x), std::max(z.earHi[sd].y, v.y), std::max(z.earHi[sd].z, v.z)};
            z.hasEar[sd] = true;
        }
    }
    return z;
}

// visible card hair: area-weighted samples on the game's hair cards where the card texture alpha shows hair
static std::vector<V3> cardHair(const App& a, float alphaVis) {
    std::vector<V3> out;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    std::map<std::string, std::tuple<unsigned char*, int, int>> tex;
    for (auto& m : a.pack.meshes) {
        if (m.kind != "hair_cards" || m.t.empty()) continue;
        bool hasUv = m.uv.size() == m.v.size();
        if (hasUv && !tex.count(m.texFile)) { int w = 0, h = 0; unsigned char* im = m.texFile.empty() ? nullptr : loadImage(a.pack.dir + "\\" + m.texFile, &w, &h); tex[m.texFile] = {im, w, h}; }
        unsigned char* im = nullptr; int tw = 0, th = 0;
        if (hasUv) std::tie(im, tw, th) = tex[m.texFile];
        std::vector<double> acc; double tot = 0;
        for (auto& t : m.t) { tot += 0.5 * len(cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]])); acc.push_back(tot); }
        int ns = std::min(30000, std::max(500, int(tot * 6e6)));
        for (int s = 0; s < ns && tot > 0; s++) {
            size_t ti = std::min(m.t.size() - 1, size_t(std::lower_bound(acc.begin(), acc.end(), U(rng) * tot) - acc.begin()));
            auto& t = m.t[ti];
            float uu = U(rng), vv = U(rng);
            if (uu + vv > 1) { uu = 1 - uu; vv = 1 - vv; }
            if (im) {
                float tu = m.uv[t[0]][0] + (m.uv[t[1]][0] - m.uv[t[0]][0]) * uu + (m.uv[t[2]][0] - m.uv[t[0]][0]) * vv;
                float tv = m.uv[t[0]][1] + (m.uv[t[1]][1] - m.uv[t[0]][1]) * uu + (m.uv[t[2]][1] - m.uv[t[0]][1]) * vv;
                tu -= std::floor(tu); tv -= std::floor(tv);
                int x = std::clamp(int(tu * tw), 0, tw - 1), y = std::clamp(int(tv * th), 0, th - 1);
                if (im[(size_t(y) * tw + x) * 4 + 3] / 255.0f <= alphaVis) continue;
            }
            out.push_back(m.v[t[0]] + (m.v[t[1]] - m.v[t[0]]) * uu + (m.v[t[2]] - m.v[t[0]]) * vv);
        }
    }
    for (auto& [k, t] : tex) if (std::get<0>(t)) freeImage(std::get<0>(t));
    return out;
}

// points every ~3 mm along every alive guide (edit frame); interp: also the strands HairWorks interpolates at each growth
// face centroid and edge midpoint (where hair actually grows between guides)
static void addPoly(std::vector<V3>& out, const Guide& G) {
    for (size_t k = 1; k < G.size(); k++) {
        float L = len(G[k] - G[k - 1]);
        int n = std::max(1, int(L / 0.003f));
        for (int j = 0; j < n; j++) out.push_back(G[k - 1] + (G[k] - G[k - 1]) * (float(j) / n));
    }
    if (!G.empty()) out.push_back(G.back());
}
static std::vector<V3> strandCloud(const App& a, bool interp) {
    const Groom& gr = a.groom;
    std::vector<V3> out;
    for (size_t g = 0; g < gr.P.size(); g++) if (gr.alive[g]) addPoly(out, gr.P[g]);
    if (!interp) return out;
    std::set<std::pair<int, int>> edges;
    const int K = gr.K;
    for (auto& F : gr.faces) {
        Guide m(K);
        for (int k = 0; k < K; k++) m[k] = (gr.P[F.g[0]][k] + gr.P[F.g[1]][k] + gr.P[F.g[2]][k]) * (1.0f / 3);
        addPoly(out, m);
        for (int q = 0; q < 3; q++) {
            int x = F.g[q], y = F.g[(q + 1) % 3];
            if (!edges.insert({std::min(x, y), std::max(x, y)}).second) continue;
            for (int k = 0; k < K; k++) m[k] = (gr.P[x][k] + gr.P[y][k]) * 0.5f;
            addPoly(out, m);
        }
    }
    return out;
}
// lateral distance from skin point X (normal n) to the nearest cloud point lying 0..maxH above it (gap in the hair cover)
static float hairGap(const std::vector<V3>& cl, const Grid& cg, V3 X, V3 n, float maxH, float search) {
    float bd = search;
    cg.around(X, search, [&](int i) {
        V3 d = cl[i] - X;
        float h = dot(d, n);
        if (h < -0.003f || h > maxH) return;
        float l = len(d - n * h);
        if (l < bd) bd = l;
    });
    return bd;
}
// root part of G lies on the scalp (radius of the root + up to lift at span)
static void hugRoot(Guide& G, V3 C, float spanM, float lift) {
    auto sl = arcLen(G);
    float r0 = len(G[0] - C);
    for (size_t k = 1; k < G.size(); k++) {
        float f = sl[k] / std::max(spanM, 1e-4f);
        if (f >= 1.0f) break;
        V3 rel = G[k] - C;
        float rk = len(rel), rt = r0 + lift * f;
        if (rk > rt) {
            V3 dl = C + nrmz(rel) * (rk + (rt - rk) * (1.0f - f)) - G[k];
            for (size_t j = k; j < G.size(); j++) G[j] = G[j] + dl * (j == k ? 1.0f : std::max(0.0f, 1.0f - (sl[j] - sl[k]) / std::max(spanM, 1e-4f)));
        }
    }
}
// root part of G lies on the SKIN (signed distance along the skin's own normal: follows concave temples / foreheads, which
// a radius from the head centre does not): points within span_m go down to base + lift * f above the skin, the rest of
// the strand is carried by the move of the last hugged point; segment lengths kept
static void hugSkin(Guide& G, const HeadSkin& hs, float spanM, float lift, float base = 0.0015f) {
    if (!hs.m || G.size() < 3) return;
    auto sl = arcLen(G);
    Guide Q = G;
    V3 dEnd{0, 0, 0};
    size_t k = 1;
    for (; k < G.size(); k++) {
        float f = sl[k] / std::max(spanM, 1e-4f);
        if (f >= 1.0f) break;
        V3 n;
        float sd = hs.sd(G[k], &n), want = base + lift * f;
        if (sd > want) Q[k] = G[k] - n * ((sd - want) * (1.0f - f * f));
        dEnd = Q[k] - G[k];
    }
    for (; k < G.size(); k++) Q[k] = G[k] + dEnd;
    Guide R = Q;
    for (size_t j = 1; j < G.size(); j++) R[j] = R[j - 1] + nrmz(Q[j] - R[j - 1]) * (sl[j] - sl[j - 1]);
    G = R;
}
// guides (index >= from) that no growth face uses: they render no hair (rooted too far outside the growth mesh)
static int faceless(const Groom& gr, size_t from) {
    std::set<int> used;
    for (auto& F : gr.faces) for (int q = 0; q < 3; q++) used.insert(F.g[q]);
    int n = 0;
    for (size_t g = from; g < gr.P.size(); g++) if (gr.alive[g] && !used.count((int)g)) n++;
    return n;
}
// template copied by ROTATION about the skull centre Ca (keeps its lie on the curved skull), root put exactly on E
static Guide copyRotated(const Guide& T, V3 Ca, V3 E) {
    V3 ua = nrmz(T[0] - Ca), ub = nrmz(E - Ca), ax = cross(ua, ub);
    float sn = len(ax), cs = dot(ua, ub);
    Guide G = T;
    if (sn > 1e-6f) { ax = ax * (1.0f / sn); float ang = std::atan2(sn, cs); for (auto& p : G) p = Ca + rotAxis(p - Ca, ax, ang); }
    V3 d = E - G[0];
    for (auto& p : G) p = p + d;
    return G;
}
// drop the last planted guides (opsPer ops each) until the budget ratio is <= maxRatio and under the export limit
static int dropForBudget(App& a, int& n, int opsPer, float maxRatio, bool ignore) {
    a.computeBudget();
    int dropped = 0;
    auto over = [&]() { return a.bud.overLimit || a.bud.ratio > maxRatio; };
    while (!ignore && over() && n > 0) {
        for (int q = 0; q < opsPer && !a.groom.ops.empty(); q++) a.groom.ops.pop_back();
        n--; dropped++;
        if (dropped % 4 == 0 || n == 0) { a.groom.replay(); a.editsChanged(); }
    }
    if (dropped) { a.groom.replay(); a.editsChanged(); }
    return dropped;
}
}  // namespace t6

// free_budget: make room in the hair budget (64 x density hairs per GROWTH FACE, whatever its size). Deleting an interior
// guide re-fans its hole (v faces -> v - 2), so it frees exactly what one planted guide costs. (1) merge: a guide whose
// root lies within merge_mm of an earlier guide's root and whose shape is within shape_mm (mean point distance) goes;
// (2) thin: interior guides (closed ring of faces, not on the growth-mesh edge = hairline) in the densest areas (mean
// incident face area < dense_factor x median, smallest first, never two neighbours in one pass) go until target_ratio.
static json freeBudget(App& a, const json& c, int& grpId) {
    using namespace t6;
    json r;
    Groom& gr = a.groom;
    a.computeBudget();
    double ratio0 = a.bud.ratio;
    size_t faces0 = gr.faces.size();
    int guides0 = gr.numAlive();
    float target = c.value("target_ratio", 1.10f);
    float mergeM = c.value("merge_mm", 2.5f) / 1000, shapeM = c.value("shape_mm", 6.0f) / 1000, dense = c.value("dense_factor", 0.8f);
    int maxDel = c.value("max_delete", 150);
    bool doMerge = c.value("merge", true), doThin = c.value("thin", true);
    std::set<int> elig, prot;
    for (int g : a.selectGuides(c.value("where", json::object({{"all", true}})))) elig.insert(g);
    if (c.contains("protect")) for (int g : a.selectGuides(c["protect"])) prot.insert(g);
    bool hasLine = c.contains("keep_line") && c["keep_line"].is_array() && c["keep_line"].size() == 2;
    V3 l0{}, l1{};
    if (hasLine) { l0 = {c["keep_line"][0][0], c["keep_line"][0][1], c["keep_line"][0][2]}; l1 = {c["keep_line"][1][0], c["keep_line"][1][1], c["keep_line"][1][2]}; }
    float lineM = c.value("keep_line_mm", 15.0f) / 1000;
    auto nearLine = [&](V3 p) {
        if (!hasLine) return false;
        V3 L = l1 - l0;
        float t = std::clamp(dot(p - l0, L) / std::max(dot(L, L), 1e-9f), 0.0f, 1.0f);
        return len(l0 + L * t - p) < lineM;
    };
    auto deletable = [&](int g) { return gr.alive[g] && elig.count(g) && !prot.count(g) && !nearLine(gr.P[g][0]); };
    int id = maxOpId(gr), grp = 1000 + (++grpId);
    auto delOp = [&](int g) { return json{{"op", "delete"}, {"roots", json::array({rootJ(gr.P[g][0])})}, {"id", ++id}, {"grp", grp}}; };
    json list = json::array();
    int merged = 0, thinned = 0;
    const int K = gr.K;
    if (doMerge) {
        Grid rg(std::max(mergeM, 0.002f));
        for (size_t g = 0; g < gr.P.size(); g++) if (gr.alive[g]) rg.add(gr.P[g][0], (int)g);
        std::vector<char> gone(gr.P.size(), 0);
        std::vector<json> ops;
        for (int g = (int)gr.P.size() - 1; g >= 0 && merged < maxDel; g--) {   // later guides (copies) go, earlier stay
            if (!gr.alive[g] || gone[g] || !deletable(g)) continue;
            int partner = -1;
            rg.around(gr.P[g][0], mergeM, [&](int h) {
                if (partner >= 0 || h >= g || !gr.alive[h] || gone[h] || len(gr.P[h][0] - gr.P[g][0]) >= mergeM) return;
                float s = 0;
                for (int k = 0; k < K; k++) s += len(gr.P[h][k] - gr.P[g][k]);
                if (s / K < shapeM) partner = h;
            });
            if (partner < 0) continue;
            gone[g] = 1;
            ops.push_back(delOp(g));
            merged++;
            if (list.size() < 60) list.push_back({{"i", g}, {"why", "merged"}, {"into", partner}, {"root", v3j(gr.P[g][0])}});
        }
        if (!ops.empty()) { for (auto& o : ops) gr.ops.push_back(o); gr.redo.clear(); gr.replay(); a.editsChanged(); }
    }
    int passes = 0, cands = 0;
    float medArea = 0;
    while (doThin && a.bud.ratio > target && merged + thinned < maxDel && passes++ < 8) {
        auto& F = gr.faces;
        std::map<std::pair<int, int>, int> ec;
        std::vector<std::vector<int>> inc(gr.P.size());
        std::vector<float> area(F.size());
        for (size_t f = 0; f < F.size(); f++) {
            for (int q = 0; q < 3; q++) { int x = F[f].g[q], y = F[f].g[(q + 1) % 3]; ec[{std::min(x, y), std::max(x, y)}]++; inc[x].push_back((int)f); }
            area[f] = faceAreaMm2(gr, F[f]);
        }
        std::vector<float> as = area;
        std::sort(as.begin(), as.end());
        medArea = as.empty() ? 0 : as[as.size() / 2];
        std::vector<std::pair<float, int>> cand;
        for (size_t g = 0; g < gr.P.size(); g++) {
            if (!deletable((int)g) || inc[g].size() < 3) continue;
            bool interior = true;
            float m = 0;
            for (int f : inc[g]) {
                m += area[f];
                for (int q = 0; q < 3; q++) {
                    int x = F[f].g[q], y = F[f].g[(q + 1) % 3];
                    if ((x == (int)g || y == (int)g) && ec[{std::min(x, y), std::max(x, y)}] != 2) interior = false;
                }
            }
            m /= inc[g].size();
            if (interior && m < dense * medArea) cand.push_back({m, (int)g});
        }
        cands = std::max(cands, (int)cand.size());
        if (cand.empty()) break;
        std::sort(cand.begin(), cand.end());
        double need = (a.bud.ratio - target) / a.bud.ratio * F.size();   // faces to remove
        int want = std::max(1, int(std::ceil(need / 2.0)));
        std::set<int> blocked;
        std::vector<json> ops;
        for (auto& [m, g] : cand) {
            if (want <= 0 || merged + thinned >= maxDel) break;
            if (blocked.count(g)) continue;
            ops.push_back(delOp(g));
            thinned++; want--;
            for (int f : inc[g]) for (int q = 0; q < 3; q++) blocked.insert(F[f].g[q]);
            if (list.size() < 60) list.push_back({{"i", g}, {"why", "dense"}, {"face_mm2", std::round(m * 10) / 10}, {"root", v3j(gr.P[g][0])}});
        }
        if (ops.empty()) break;
        for (auto& o : ops) gr.ops.push_back(o);
        gr.redo.clear(); gr.replay(); a.editsChanged();
    }
    a.smap.ensure(gr.P.size());
    r["ratio_before"] = ratio0; r["ratio_after"] = a.bud.ratio; r["target_ratio"] = target;
    r["faces_before"] = faces0; r["faces_after"] = gr.faces.size(); r["guides_before"] = guides0; r["guides_after"] = gr.numAlive();
    r["merged"] = merged; r["thinned"] = thinned; r["thin_candidates"] = cands; r["median_face_mm2"] = std::round(medArea * 10) / 10;
    r["deleted"] = list;
    if (a.bud.ratio > target + 1e-4) r["note"] = "target not reached: raise dense_factor (more areas count as dense), max_delete, or widen where";
    return r;
}

// fill_below_hairline: guides on bald skin BELOW the painted hairline where the vanilla cards put hair (receding temples):
// candidate skin points = head skin off the painted scalp, within below_mm of it, under visible card hair (reach_mm), no
// strand (guide or interpolated) lying over them (cover_mm). Poisson at spacing_mm, nearest the scalp first so new edge
// faces chain outward. Each is a copy of a nearby guide (same side, rotated about the skull centre, root on the skin),
// optionally shortened, root part hugging the skin; copies through the head, sticking up, over the face / ears rejected.
static json fillBelowHairline(App& a, const json& c, int& grpId, const std::string& out) {
    using namespace t6;
    json r;
    Groom& gr = a.groom;
    const V3 C = a.pack.headC;
    HeadSkin hs = headSkin(a);
    if (!hs.m) { r["error"] = "pack has no head mesh"; return r; }
    if (!hs.flag) { r["error"] = "no painted scalp on the head mesh (nothing is 'below the hairline')"; return r; }
    Zones z = zonesOf(a, c);
    float spacing = c.value("spacing_mm", 5.0f) / 1000, below = c.value("below_mm", 30.0f) / 1000, reach = c.value("reach_mm", 10.0f) / 1000;
    float cover = c.value("cover_mm", 3.0f) / 1000, coverH = c.value("cover_height_mm", 12.0f) / 1000, minL = c.value("min_length", 0.04f);
    float tmax = c.value("template_mm", 35.0f) / 1000, lift = c.value("lift_mm", 1.5f) / 1000, spanM = c.value("span_mm", 30.0f) / 1000;
    float lenScale = c.value("length_scale", 1.0f), maxLen = c.value("max_length", 0.0f), maxRatio = c.value("max_ratio", 1.2f);
    float earPad = c.value("ear_pad_mm", 5.0f) / 1000, tuftM = c.value("tuft_mm", 12.0f) / 1000;
    int maxNew = c.value("max_new", 120);
    bool dry = c.value("list_only", false), ignore = c.value("ignore_budget", false);
    std::string side = c.value("side", std::string("both"));
    json where = c.value("where", json::object());
    a.computeBudget();
    if (a.bud.ratio > maxRatio && !ignore) { r["error"] = "hair budget ratio already above max_ratio: run studio_free_budget first"; return r; }
    Grid sg(0.01f);
    for (size_t i = 0; i < hs.m->v.size(); i++) if (hs.painted((int)i)) sg.add(hs.m->v[i], (int)i);
    auto distScalp = [&](V3 p) { float bd = 1e9f; sg.around(p, below, [&](int i) { bd = std::min(bd, len(hs.m->v[i] - p)); }); return bd; };
    std::vector<V3> cards = cardHair(a, c.value("alpha_vis", 0.3f));
    if (cards.empty()) { r["error"] = "this pack has no vanilla hair cards (re-run desk_prep)"; return r; }
    Grid cg(0.01f);
    for (size_t i = 0; i < cards.size(); i++) cg.add(cards[i], (int)i);
    std::vector<V3> cloud = strandCloud(a, c.value("interp", true));
    Grid clg(0.01f);
    for (size_t i = 0; i < cloud.size(); i++) clg.add(cloud[i], (int)i);
    auto inWhere = [&](V3 p) {
        if (side == "left" && p.x < C.x + 0.01f) return false;
        if (side == "right" && p.x > C.x - 0.01f) return false;
        float zr = p.z - a.pack.headTop;
        if (where.contains("z_min") && zr < where["z_min"].get<float>()) return false;
        if (where.contains("z_max") && zr > where["z_max"].get<float>()) return false;
        if (where.contains("near") && where["near"].is_array() && where["near"].size() == 3) {
            V3 q{where["near"][0].get<float>(), where["near"][1].get<float>(), where["near"][2].get<float>()};
            if (len(p - q) > where.value("radius", 0.03f)) return false;
        }
        return true;
    };
    // candidate skin points (area-weighted samples; with cameras: the bare skin SEEN in their renders)
    std::vector<std::pair<float, V3>> cand;
    int nOff = 0, nNoCard = 0, nCovered = 0, nPix = 0;
    int nRegion = 0, nFace = 0, nPainted = 0;
    // the filters every candidate passes (both modes). seen = the render showed this point as bare non-scalp skin: the
    // overlay tests the scalp per texel, finer than the per-vertex flag (~1 cm), so a seen point right at the hairline
    // can have a 'painted' nearest vertex - it is still bare skin no other tool fills
    auto skinOk = [&](V3 X, int& vi, bool seen) {
        if (X.z < z.eyeZ - 0.07f || !inWhere(X)) { nRegion++; return -1.0f; }
        if (z.overFace(X) || z.inEar(X, earPad)) { nFace++; return -1.0f; }
        vi = hs.nearest(X);
        if (vi < 0 || (!seen && hs.painted(vi))) { nPainted++; return -1.0f; }
        float ds = distScalp(X);
        if (ds > below) { nOff++; return -1.0f; }
        bool card = false;
        cg.around(X, reach, [&](int i) { if (!card && len(cards[i] - X) < reach) card = true; });
        if (!card) { nNoCard++; return -1.0f; }
        return ds;
    };
    std::vector<json> cams;
    if (c.contains("shots") && c["shots"].is_array()) for (auto& s : c["shots"]) cams.push_back(json{{"shot", s}});
    else if (c.contains("shot") || c.contains("view") || c.contains("eye")) cams.push_back(c);
    if (!cams.empty()) {
        // closed loop (as fill_scalp): the scalp overlay draws skin off the painted scalp flat grey (97, 97, 97); those
        // pixels are bare skin, ray-cast to the posed skin and carried to the edit frame by the nearest guide root
        std::vector<int> alive0;
        for (size_t g = 0; g < gr.P.size(); g++) if (gr.alive[g]) alive0.push_back((int)g);
        int stepPx = std::max(2, c.value("sample_px", 4)), size = c.value("size", 900);
        for (auto& cc : cams) {
            json cam;
            if (!cameraOf(a, cc, cam)) { r["error"] = "no such camera"; return r; }
            std::string png = out + "\\_fillbelow.png";
            bool so = a.scalpOverlay;
            a.scalpOverlay = true;
            bool ok = a.renderShot(cam, png, size);
            a.scalpOverlay = so;
            int w = 0, h = 0;
            unsigned char* im = ok ? loadImage(png, &w, &h) : nullptr;
            if (!im) { r["error"] = "render failed"; return r; }
            XMFLOAT3 eye{cam["eye"][0], cam["eye"][1], cam["eye"][2]}, tg{cam["target"][0], cam["target"][1], cam["target"][2]};
            XMMATRIX V = XMMatrixLookAtRH(XMLoadFloat3(&eye), XMLoadFloat3(&tg), XMVectorSet(0, 0, 1, 0));
            XMMATRIX P = XMMatrixPerspectiveFovRH(XMConvertToRadians(cam.value("fovy", 11.16f)), 1.0f, 0.05f, 100.0f);
            XMMATRIX IVP = XMMatrixInverse(nullptr, V * P);
            for (int y = 0; y < h; y += stepPx)
                for (int x = 0; x < w; x += stepPx) {
                    const unsigned char* q = &im[(size_t(y) * w + x) * 4];
                    if (std::abs(q[0] - 97) > 1 || std::abs(q[1] - 97) > 1 || std::abs(q[2] - 97) > 1) continue;
                    float nx = (x + 0.5f) / w * 2 - 1, ny = 1 - (y + 0.5f) / h * 2;
                    XMVECTOR ra = XMVector3TransformCoord(XMVectorSet(nx, ny, 0, 1), IVP), rb = XMVector3TransformCoord(XMVectorSet(nx, ny, 1, 1), IVP);
                    XMFLOAT3 o, dir, hit;
                    XMStoreFloat3(&o, ra);
                    XMStoreFloat3(&dir, XMVector3Normalize(XMVectorSubtract(rb, ra)));
                    int mi;
                    if (!a.skinRay(o, dir, hit, mi)) continue;
                    V3 H{hit.x, hit.y, hit.z};
                    int g0 = -1; float bd = 1e30f;
                    for (int g : alive0) { float dd = len(a.posedPoint(g, 0) - H); if (dd < bd) { bd = dd; g0 = g; } }
                    if (g0 < 0) continue;
                    V3 X = H + (gr.P[g0][0] - a.posedPoint(g0, 0));
                    nPix++;
                    int vi;
                    float ds = skinOk(X, vi, true);
                    if (ds < 0) continue;
                    bool dup = false;
                    for (auto& cq : cand) if (len(cq.second - X) < spacing * 0.5f) { dup = true; break; }
                    if (!dup) cand.push_back({ds, X});
                }
            freeImage(im);
            std::error_code ec;
            fs::remove(png, ec);
        }
    } else {
        const Mesh& m = *hs.m;
        std::vector<double> acc; double tot = 0;
        for (auto& t : m.t) {
            V3 ctr = (m.v[t[0]] + m.v[t[1]] + m.v[t[2]]) * (1.0f / 3);
            tot += ctr.z > z.eyeZ - 0.07f ? 0.5 * len(cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]])) : 0.0;
            acc.push_back(tot);
        }
        std::mt19937 rs(5);
        std::uniform_real_distribution<float> U(0.0f, 1.0f);
        for (int s = 0; s < 80000 && tot > 0; s++) {
            size_t ti = std::min(m.t.size() - 1, size_t(std::lower_bound(acc.begin(), acc.end(), U(rs) * tot) - acc.begin()));
            auto& t = m.t[ti];
            float uu = U(rs), vv = U(rs);
            if (uu + vv > 1) { uu = 1 - uu; vv = 1 - vv; }
            V3 X = m.v[t[0]] + (m.v[t[1]] - m.v[t[0]]) * uu + (m.v[t[2]] - m.v[t[0]]) * vv;
            if (X.z < z.eyeZ - 0.07f || !inWhere(X) || z.overFace(X) || z.inEar(X, earPad)) continue;
            int vi = hs.nearest(X);
            if (vi < 0 || hs.painted(vi)) continue;
            float ds = distScalp(X);
            if (ds > below) { nOff++; continue; }
            bool card = false;
            cg.around(X, reach, [&](int i) { if (!card && len(cards[i] - X) < reach) card = true; });
            if (!card) { nNoCard++; continue; }
            if (hairGap(cloud, clg, X, hs.n[vi], coverH, cover + coverH) < cover) { nCovered++; continue; }
            cand.push_back({ds, X});
        }
    }
    std::sort(cand.begin(), cand.end(), [](const std::pair<float, V3>& x, const std::pair<float, V3>& y) { return x.first < y.first; });
    std::vector<int> alive, longs;
    for (size_t g = 0; g < gr.P.size(); g++) if (gr.alive[g]) { alive.push_back((int)g); if (guideLen(gr.P[g]) >= minL) longs.push_back((int)g); }
    if (longs.empty()) { r["error"] = "no guide longer than min_length"; return r; }
    HeadRadial hr = headRadial(a);
    json euv = typicalUv(a);
    if (euv.is_null()) euv = "src";
    bool localEuv = c.value("edge_uv", std::string()) == "local";
    int id = maxOpId(gr), grp = 1000 + (++grpId), n = 0, noTpl = 0, through = 0, tuft = 0, face = 0, ear = 0;
    std::vector<json> ops;
    std::vector<V3> taken;
    json planned = json::array();
    for (auto& [ds, X] : cand) {
        if (n >= maxNew) break;
        bool close = false;
        for (auto& q : taken) if (len(q - X) < spacing) { close = true; break; }
        for (int g : alive) if (!close && len(gr.P[g][0] - X) < spacing * 0.8f) close = true;
        if (close) continue;
        float sideX = X.x - C.x;
        std::vector<std::pair<float, int>> tc;
        for (int g : longs) {
            V3 rr = gr.P[g][0];
            float dd = len(rr - X);
            if (dd > tmax || (std::fabs(sideX) > 0.012f && (rr.x - C.x) * sideX < 0)) continue;
            tc.push_back({dd, g});
        }
        std::sort(tc.begin(), tc.end());
        if (tc.empty()) { noTpl++; continue; }
        int why = 0;
        bool done = false;
        for (size_t t = 0; t < tc.size() && t < 8 && !done; t++) {
            int g = tc[t].second;
            Guide G = copyRotated(gr.P[g], z.Ca, X);
            float L = guideLen(G), wantL = L * lenScale;
            if (maxLen > 0) wantL = std::min(wantL, maxLen);
            if (wantL < L * 0.999f) G = shortenGuide(G, wantL / L);
            hugSkin(G, hs, spanM, lift);
            if (pushOut(hr, G, 0.003f) > 0) { why = 1; continue; }
            bool up = false, fc = false, er = false;
            auto sl = arcLen(G);
            for (size_t k = 1; k < G.size(); k++) {
                if (sl[k] < 0.04f && hs.sd(G[k]) > tuftM) up = true;
                if (z.overFace(G[k])) fc = true;
                if (z.inEar(G[k], earPad)) er = true;
            }
            if (up) { why = 2; continue; }
            if (fc) { why = 3; continue; }
            if (er) { why = 4; continue; }
            V3 rs = gr.P[g][0], dd = X - rs;
            ops.push_back({{"op", "duplicate"}, {"root", rootJ(rs)}, {"d", rootJ(dd)}, {"edge_face", true}, {"edge_uv", localEuv && !localUv(gr, X).is_null() ? localUv(gr, X) : euv}, {"id", ++id}, {"grp", grp}});
            ops.push_back({{"op", "reshape"}, {"root", rootJ(G[0])}, {"points", ptsJson(G)}, {"id", ++id}, {"grp", grp}});
            taken.push_back(X);
            if (planned.size() < 200) planned.push_back({{"root", v3j(X)}, {"below_hairline_mm", std::round(ds * 10000) / 10}, {"template", g}, {"length", std::round(guideLen(G) * 1e4f) / 1e4f}});
            n++; done = true;
        }
        if (!done) { if (why == 1) through++; else if (why == 2) tuft++; else if (why == 3) face++; else if (why == 4) ear++; else noTpl++; }
    }
    r["candidates"] = cand.size();
    if (!cams.empty()) r["bare_skin_pixels_seen"] = nPix;
    r["skipped_samples"] = {{"too_far_below_hairline", nOff}, {"no_vanilla_card_hair", nNoCard}, {"already_covered", nCovered},
                            {"outside_where_or_below_eyes", nRegion}, {"face_or_ear", nFace}, {"on_painted_scalp", nPainted}};
    r["rejected"] = {{"no_template_same_side", noTpl}, {"through_head", through}, {"sticks_up", tuft}, {"over_face_eyes", face}, {"over_ear", ear}};
    if (dry) { r["would_plant"] = n; r["planned"] = planned; return r; }
    size_t f0 = gr.faces.size(), g0 = gr.P.size();
    for (auto& o : ops) gr.ops.push_back(o);
    gr.redo.clear(); gr.replay(); a.smap.ensure(gr.P.size()); a.editsChanged();
    int dropped = dropForBudget(a, n, 2, maxRatio, ignore);
    if (dropped) r["dropped_for_budget"] = dropped;
    r["planted"] = n; r["new_faces"] = (long)gr.faces.size() - (long)f0; r["planned"] = planned; r["edge_uv"] = euv;
    r["planted_without_growth_face"] = faceless(gr, g0);
    return r;
}

// centre_parting: shape a parting line and comb the hair away from it on both sides. Line = "line" [[p0],[p1]] (edit
// frame), or automatic: x from the vanilla cards (the thinnest column of card hair over the front top = the vanilla
// parting; "x" / x_offset_mm override), from the painted front hairline back over the top for length_mm. Guides rooted
// within band_mm: roots closer than gap_mm / 2 move out to the gap edge (their side; roots on the line by their own
// flow), root part turned away from the line (strength, weaker with distance), points still across the line pushed
// back (clear_mm), roots within hug_mm laid flat; growth faces spanning the line removed (split_faces) so no hair is
// interpolated across it.
static json centreParting(App& a, const json& c, int& grpId) {
    using namespace t6;
    json r;
    Groom& gr = a.groom;
    const V3 C = a.pack.headC;
    HeadSkin hs = headSkin(a);
    Zones z = zonesOf(a, c);
    const float fwd = z.fwd;
    auto theta = [&](V3 p) { return std::atan2(p.z - C.z, fwd * (p.y - C.y)); };   // 0 = forward, pi/2 = up
    V3 p0, p1;
    if (c.contains("line") && c["line"].is_array() && c["line"].size() == 2) {
        p0 = {c["line"][0][0], c["line"][0][1], c["line"][0][2]};
        p1 = {c["line"][1][0], c["line"][1][1], c["line"][1][2]};
    } else {
        if (!hs.m) { r["error"] = "pack has no head mesh (give line)"; return r; }
        float plane = gr.mirrorPlane(), px = plane;
        if (c.contains("x")) px = c["x"].get<float>();
        else if (c.value("from_cards", true)) {
            std::vector<V3> cards = cardHair(a, 0.3f);
            float search = c.value("search_mm", 25.0f) / 1000;
            std::vector<float> xs, cnt;
            for (float xb = plane - search; xb <= plane + search + 1e-5f; xb += 0.001f) {
                int n = 0;
                for (auto& p : cards)
                    if (std::fabs(p.x - xb) < 0.0025f && p.z > a.pack.headTop - 0.05f && fwd * (p.y - C.y) > -0.03f) n++;
                xs.push_back(xb); cnt.push_back((float)n);
            }
            float best = 1e30f;
            for (size_t i = 0; i < xs.size(); i++) {
                float s = 0; int m = 0;
                for (int d = -2; d <= 2; d++) if (i + d < xs.size()) { s += cnt[i + d]; m++; }
                s = s / std::max(m, 1) + 0.5f * std::fabs(xs[i] - plane) * 1000;   // ties: nearest the head's mid plane
                if (s < best) { best = s; px = xs[i]; }
            }
            r["detected_from_cards"] = {{"x", r4(px)}, {"mid_plane_x", r4(plane)}, {"card_hair_in_column", best}};
        }
        px += c.value("x_offset_mm", 0.0f) / 1000;
        // head vertices on the plane x = px (painted scalp if any), ordered front -> back over the top
        std::vector<std::pair<float, V3>> col;
        for (size_t i = 0; i < hs.m->v.size(); i++) {
            V3 v = hs.m->v[i];
            if (std::fabs(v.x - px) > 0.004f || v.z < z.eyeZ + 0.02f) continue;
            if (hs.flag && !hs.painted((int)i)) continue;
            col.push_back({theta(v), v});
        }
        if (col.size() < 3) { r["error"] = "no scalp on the parting plane (give line)"; return r; }
        std::sort(col.begin(), col.end(), [](const std::pair<float, V3>& x, const std::pair<float, V3>& y) { return x.first < y.first; });
        p0 = col.front().second;
        p0 = p0 + nrmz(V3{0, fwd, 0}) * (c.value("front_mm", 0.0f) / 1000);
        float want = c.value("length_mm", 110.0f) / 1000, s = 0;
        p1 = col.back().second;
        for (size_t i = 1; i < col.size(); i++) {
            s += len(col[i].second - col[i - 1].second) * 0.5f + std::fabs(col[i].first - col[i - 1].first) * len(col[i].second - C) * 0.5f;
            if (s >= want) { p1 = col[i].second; break; }
        }
    }
    V3 Np = nrmz(cross(p1 - p0, V3{0, 0, 1}));
    if (len(cross(p1 - p0, V3{0, 0, 1})) < 1e-5f) { r["error"] = "parting line is vertical"; return r; }
    float th0 = theta(p0), th1 = theta(p1);
    auto tOf = [&](V3 p) { return (theta(p) - th0) / std::max(th1 - th0, 1e-4f); };
    auto sOf = [&](V3 p) { return dot(p - p0, Np); };
    float tLo = c.value("t_min", -0.05f), tHi = c.value("t_max", 1.1f);
    float back = c.value("back", 0.5f), splitM = c.value("split_mm", 8.0f) / 1000;
    V3 lineBack = nrmz(p1 - p0);
    float band = c.value("band_mm", 50.0f) / 1000, gap = c.value("gap_mm", 2.5f) / 1000, strength = std::clamp(c.value("strength", 0.9f), 0.0f, 1.0f);
    float spanM = c.value("span_mm", 35.0f) / 1000, clearM = c.value("clear_mm", 2.0f) / 1000, hugM = c.value("hug_mm", 12.0f) / 1000;
    float lift = c.value("lift_mm", 2.0f) / 1000, maxTurn = c.value("max_turn_deg", 120.0f) * 3.14159265f / 180;
    bool split = c.value("split_faces", true), lay = c.value("mode", std::string("lay")) != "turn";
    float layM = c.value("lay_mm", 50.0f) / 1000, layFar = std::clamp(c.value("lay_far", 0.4f), 0.0f, 1.0f);
    float rejoin = c.value("rejoin_mm", 50.0f) / 1000, steer = std::clamp(c.value("steer", 0.35f), 0.0f, 1.0f);
    const V3 Gb = C + V3{0, -fwd * 0.12f, -0.03f};   // back of the head (where pulled-back hair goes)
    auto ix = a.selectGuides(c.value("where", json::object({{"all", true}})));
    int id = maxOpId(gr), grp = 1000 + (++grpId);
    std::vector<json> ops;
    int nFaces = 0, nComb = 0, nMoved = 0, nCross = 0, nLaid = 0, nFace = 0, nWhole = 0;
    if (split) {
        for (auto& F : gr.faces) {
            V3 A = gr.P[F.g[0]][0], B = gr.P[F.g[1]][0], D = gr.P[F.g[2]][0];
            float sa = sOf(A), sb = sOf(B), sd = sOf(D);
            bool pos = sa > 0 || sb > 0 || sd > 0, neg = sa < 0 || sb < 0 || sd < 0;
            V3 cen = (A + B + D) * (1.0f / 3);
            float tc = tOf(cen);
            if (!(pos && neg) || tc < tLo || tc > tHi || std::fabs(sOf(cen)) > splitM) continue;
            ops.push_back({{"op", "delete_face"}, {"roots", {rootJ(A), rootJ(B), rootJ(D)}}, {"id", ++id}, {"grp", grp}});
            nFaces++;
        }
    }
    HeadRadial hr = headRadial(a);
    for (int g : ix) {
        const Guide& G = gr.P[g];
        V3 R = G[0];
        float s = sOf(R), t = tOf(R);
        if (t < tLo || t > tHi || std::fabs(s) > band) continue;
        float sg = s > 0.0003f ? 1.0f : s < -0.0003f ? -1.0f : (dot(G[G.size() / 2] - R, Np) >= 0 ? 1.0f : -1.0f);
        float w = std::sqrt(std::clamp(1.0f - std::fabs(s) / band, 0.0f, 1.0f));
        Guide Q = G;
        if (std::fabs(s) < gap / 2) { Q = moveRoot(Q, Np * (sg * (gap / 2 - std::fabs(s))), 0.03f); nMoved++; }
        // target flow at the root: away from the line, plus back along it (back, 0.5: vanilla partings comb the hair
        // to the side AND back; pure sideways sends front strands over the temples / eyes)
        // "back" is LOCAL: toward the back end of the line from this root, in its tangent plane (at the front hairline
        // that points up over the top; the global line direction points into the skull there and left only "sideways",
        // which walked laid fringe strands across the forehead)
        V3 nrm = nrmz(Q[0] - C), bk = p1 - Q[0];
        bk = bk - nrm * dot(bk, nrm);
        bk = len(bk) > 1e-5f ? nrmz(bk) : lineBack;
        V3 want = Np * sg + bk * back;
        want = want - nrm * dot(want, nrm);
        if (len(want) > 1e-6f && lay && hs.m) {
            // lay (default): the first lay_mm of the strand is walked ALONG THE SKIN in the combed direction (start =
            // the root's own flow turned toward want by strength x w, then parallel-transported over the skull: a
            // geodesic), lift_mm above the skin; the rest of the strand is carried (translated, keeps hanging); a strand
            // whose end would then fall on the face is laid whole. Turning only the root part (mode "turn") left long fringe strands hanging over the forehead.
            want = nrmz(want);
            int k4 = std::max(1, int(Q.size()) / 4);
            V3 d = Q[k4] - Q[0], tg = d - nrm * dot(d, nrm);
            tg = len(tg) > 1e-6f ? nrmz(tg) : want;
            float ang = std::atan2(dot(cross(tg, want), nrm), dot(tg, want));
            ang = std::clamp(ang * strength * w, -maxTurn, maxTurn);
            V3 dir = rotAxis(tg, nrm, ang);
            auto sq = arcLen(Q);
            auto layOut = [&](float layW) {
                Guide L = Q;
                V3 dv = dir;
                size_t k = 1;
                for (; k < Q.size() && sq[k] <= layW + 1e-5f; k++) {
                    float seg = sq[k] - sq[k - 1];
                    V3 p = L[k - 1] + dv * seg, n;
                    float sdv = hs.sd(p, &n);
                    if (std::fabs(sdv) > 0.03f) break;   // off the skin mesh (nearest vertex far away / wrong side): stop laying
                    p = p - n * (sdv - (0.0015f + lift * std::min(1.0f, sq[k] / std::max(std::min(layW, sq.back()), 1e-4f))));
                    p = L[k - 1] + nrmz(p - L[k - 1]) * seg;
                    V3 nd = p - L[k - 1];
                    hs.sd(p, &n);
                    nd = nd - n * dot(nd, n);
                    if (len(nd) > 1e-7f) dv = nrmz(nd);
                    // steer toward the combed field: away from the line + toward the back of the head, the back part
                    // growing along the strand (a plain surface walk is a great circle: laid 15 cm it ran sideways, then
                    // DOWN the temple onto the cheek - Iris)
                    V3 gb = Gb - p, des = Np * sg + (gb - n * dot(gb, n)) * (1.0f / std::max(len(gb), 1e-4f)) * (back + 2.0f * sq[k] / std::max(sq.back(), 1e-4f));
                    des = des - n * dot(des, n);
                    if (len(des) > 1e-6f && steer > 0) dv = nrmz(dv * (1.0f - steer) + nrmz(des) * steer);
                    L[k] = p;
                }
                // the rest REJOINS its old path: offset from the old point fading out over rejoin_mm, lengths kept.
                // Rotating it with the seam turn sent hanging ends into the air, translating it whole stood long side
                // hair out like a wing (Iris tests): a combed strand still ends where the style takes it (bun, nape)
                if (k < Q.size() && k >= 1) {
                    V3 off = L[k - 1] - Q[k - 1];
                    float s0 = sq[k - 1];
                    Guide T = L;
                    for (size_t j = k; j < Q.size(); j++) {
                        float f = rejoin > 0 ? std::max(0.0f, 1.0f - (sq[j] - s0) / rejoin) : 1.0f;
                        T[j] = Q[j] + off * (f * f * (3 - 2 * f));
                    }
                    for (size_t j = k; j < Q.size(); j++) L[j] = L[j - 1] + nrmz(T[j] - L[j - 1]) * (sq[j] - sq[j - 1]);
                }
                return L;
            };
            auto overF = [&](const Guide& X) { for (auto& p : X) if (z.overFace(p)) return true; return false; };
            // fringe: the old path past the laid part runs over the forehead (in front of the eyes' plane - 35 mm, below
            // brow + 50 mm): rejoining it would put the fringe back on the face, so such strands are laid whole
            float layW = layM * (layFar + (1.0f - layFar) * w);   // farther from the line: shorter laid part
            bool fringe = false;
            for (size_t k = 1; k < Q.size() && !fringe; k++)
                if (sq[k] > layW && z.haveEye && fwd * (Q[k].y - z.eyeY) > -0.035f && Q[k].z < z.browZ + 0.05f) fringe = true;
            Guide L = layOut(fringe ? 1e9f : layW);
            if (!fringe && overF(L) && !overF(G)) fringe = true, L = layOut(1e9f);   // its end would fall on the face
            if (fringe) nWhole++;
            Q = L;
            nLaid++;
        } else if (len(want) > 1e-6f) {
            want = nrmz(want);
            int k4 = std::max(1, int(Q.size()) / 4);
            V3 d = Q[k4] - Q[0], tg = nrmz(d - nrm * dot(d, nrm));
            float ang = std::atan2(dot(cross(tg, want), nrm), dot(tg, want));
            ang = std::clamp(ang * strength * w, -maxTurn, maxTurn);
            if (std::fabs(ang) > 0.01f) Q = turnRoot(Q, nrm, ang, spanM);
        }
        // points still across the line (within the parting's extent) pushed back to its side, lengths kept
        auto sl = arcLen(Q);
        bool crossed = false;
        Guide T = Q;
        for (size_t k = 1; k < Q.size(); k++) {
            float tk = tOf(Q[k]);
            if (tk < tLo || tk > tHi) continue;
            float d = sOf(Q[k]) * sg;
            if (d < clearM) { T[k] = Q[k] + Np * (sg * (clearM - d)); crossed = true; }
        }
        if (crossed) {
            nCross++;
            Q = T;
            Guide R2 = Q;
            for (size_t k = 1; k < Q.size(); k++) R2[k] = R2[k - 1] + nrmz(Q[k] - R2[k - 1]) * (sl[k] - sl[k - 1]);
            Q = R2;
        }
        if (std::fabs(s) < hugM && !lay) hugSkin(Q, hs, spanM, lift);
        pushOut(hr, Q, 0.003f);
        for (auto& p : Q) if (z.overFace(p)) { nFace++; break; }
        float mv = 0;
        for (size_t k = 0; k < Q.size(); k++) mv += len(Q[k] - G[k]);
        if (mv < 1e-4f) continue;
        ops.push_back({{"op", "reshape"}, {"root", rootJ(R)}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
        nComb++;
    }
    r["line"] = {v3j(p0), v3j(p1)};
    r["combed"] = nComb; r["roots_moved_to_gap"] = nMoved; r["crossings_pushed_back"] = nCross; r["faces_split"] = nFaces;
    r["laid"] = nLaid; r["laid_whole_strand"] = nWhole; r["combed_over_face"] = nFace;
    if (c.value("list_only", false)) return r;
    for (auto& o : ops) gr.ops.push_back(o);
    gr.redo.clear(); gr.replay(); a.editsChanged();
    return r;
}

#include "serve_r3a.inc"   // regrow / route_behind_ears / twist_roll / plant_fringe / parting_roots (2026-10-12, opt-in)

json App::serveCommand(const json& c, const std::string& out, bool& quit) {
    std::string d = c.value("do", "");
    json r = {{"do", d}};
    auto status_ = [&]() {
        computeBudget();
        return json{{"config", configName}, {"fur", fur ? "beard" : "hair"}, {"has_beard", beardOn}, {"guides", groom.numAlive()}, {"faces", groom.faces.size()}, {"ops", groom.ops.size()},
                    {"redo", groom.redo.size()}, {"unmatched", groom.unmatched}, {"guides_over_face", overFace(*this)},
                    {"budget", {{"hairs", bud.hairs}, {"hairs_close", bud.hairsClose}, {"ref", bud.refName}, {"ratio", bud.ratio},
                                {"limit", look.limit}, {"over_limit", bud.overLimit}}}};
    };
    if (d == "quit") { quit = true; return r; }
    if (c.value("dry_run", false) && loaded) {
        // try the command, measure (status + optional scalp coverage "measure": {camera keys}), then restore the edits
        auto ops0 = groom.ops, redo0 = groom.redo;
        json c2 = c;
        c2.erase("dry_run");
        json m = c2.value("measure", json());
        c2.erase("measure");
        json res = serveCommand(c2, out, quit);
        json meas;
        if (m.is_object()) {
            json sc = m;
            sc["do"] = "scalp_coverage";
            sc["tag"] = "_dry";
            meas = serveCommand(sc, out, quit);
        }
        groom.ops = ops0;
        groom.redo = redo0;
        groom.replay();
        editsChanged();
        settleRest(*this, 0.5);
        computeBudget();
        return {{"do", d}, {"dry_run", true}, {"result", res}, {"measure", meas}, {"note", "nothing was kept"}};
    }
    if (d == "open") {
        bool ok = c.contains("project") ? loadProject(c["project"]) : openConfig(c.value("cfg", ""), c.value("reprep", false));
        if (!ok) { r["error"] = "cannot open: " + status; return r; }
        settleRest(*this, 0.5);
        r["status"] = status_();
        return r;
    }
    if (!loaded) { r["error"] = "no build open (use open)"; return r; }
    if (d == "status") { r["status"] = status_(); return r; }
    if (d == "shots_list") {
        json l = json::array();
        for (auto& [k, sh] : pack.shots.items())
            l.push_back({{"id", std::stoi(k)}, {"view", sh.value("view", "")}, {"eye", sh["eye"]}, {"target", sh["target"]},
                         {"fovy", sh.value("fovy", 11.16f)}, {"ref", sh.value("ref", json())}});
        r["shots"] = l;
        bool cards = false;
        for (auto& m : pack.meshes) if (m.kind == "hair_cards") cards = true;
        r["vanilla_cards"] = cards;   // render "vanilla": true works (the game's hair cards in the Studio)
        return r;
    }
    if (d == "render") {
        json cam;
        if (!cameraOf(*this, c, cam)) { r["error"] = "no such camera"; return r; }
        BaselineScope bs(*this, c.value("baseline", false));
        std::string ov = c.value("overlay", "none"), png = c.value("path", out + "\\render.png");
        bool so = scalpOverlay, go = showGuideOverlay, sh = showHair, vh = vanillaHair, gf = guideFront, gb = guideBody;
        guideFront = c.value("guides_front", guideFront);   // guide views for this render (default: the session's guide_view)
        guideBody = c.value("guides_body", guideBody);
        showHair = c.value("hair", true);   // "hair": false = skin / scalp only (check the scalp mask)
        vanillaHair = c.value("vanilla", false);   // "vanilla": true = the game's hair cards instead of the groom
        if (vanillaHair) showHair = false;
        int cz = colorize;
        scalpOverlay = ov == "scalp";
        showGuideOverlay = ov == "guides";
        if (ov == "mask") { colorize = 4; maskPass = true; step(0.0); }
        bool ok = renderShot(cam, png, c.value("size", 1400));
        if (ov == "mask") { colorize = cz; maskPass = false; step(0.0); }
        if (ov == "guides" && guideVisOn() && gvis.valid) r["guides_shown"] = gvis.nShown;
        scalpOverlay = so; showGuideOverlay = go; showHair = sh; vanillaHair = vh; guideFront = gf; guideBody = gb;
        r["ok"] = ok; r["png"] = png; r["camera"] = cam; r["hairs_live"] = st.hairs;
        return r;
    }
    if (d == "undo" || d == "redo") {
        for (int i = 0; i < c.value("n", 1); i++) d == "undo" ? groom.undo() : groom.redoOp();
        editsChanged();
        settleRest(*this, 0.5);
        r["status"] = status_();
        return r;
    }
    if (d == "reset_edits") {
        groom.ops.clear(); groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["status"] = status_();
        return r;
    }
    if (d == "guides") {
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        int lim = c.value("limit", 200);
        json l = json::array();
        for (int i : ix) {
            if ((int)l.size() >= lim) break;
            auto& G = groom.P[i];
            l.push_back({{"i", i}, {"root", v3j(G[0])}, {"tip", v3j(G.back())}, {"length", std::round(guideLen(G) * 1e4f) / 1e4f},
                         {"edited", groom.edited.count(i) > 0}});
        }
        r["count"] = ix.size(); r["guides"] = l;
        r["frame"] = {{"head_centre", v3j(pack.headC)}, {"head_top_z", pack.headTop}, {"axes", "+X character's left, +Y forward, +Z up (m)"}};
        return r;
    }
    if (d == "guide") {
        int i = c.value("index", -1);
        if (i < 0 || i >= (int)groom.P.size() || !groom.alive[i]) { r["error"] = "no such guide"; return r; }
        json pts = json::array();
        for (auto& p : groom.P[i]) pts.push_back(v3j(p));
        r["index"] = i; r["points"] = pts; r["length"] = guideLen(groom.P[i]); r["edited"] = groom.edited.count(i) > 0;
        return r;
    }
    if (d == "faces") {
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        std::set<int> in(ix.begin(), ix.end());
        float minA = c.value("min_area_mm2", 0.0f);
        int lim = c.value("limit", 200);
        std::vector<std::pair<float, int>> fl;
        for (size_t f = 0; f < groom.faces.size(); f++) {
            auto& F = groom.faces[f];
            if (!in.count(F.g[0]) && !in.count(F.g[1]) && !in.count(F.g[2])) continue;
            float A = faceAreaMm2(groom, F);
            if (A >= minA) fl.push_back({A, int(f)});
        }
        std::sort(fl.rbegin(), fl.rend());
        std::vector<float> all;
        for (auto& F : groom.faces) all.push_back(faceAreaMm2(groom, F));
        std::sort(all.begin(), all.end());
        json l = json::array();
        for (auto& [A, f] : fl) {
            if ((int)l.size() >= lim) break;
            auto& F = groom.faces[f];
            V3 cen = (groom.P[F.g[0]][0] + groom.P[F.g[1]][0] + groom.P[F.g[2]][0]) * (1.0f / 3);
            l.push_back({{"id", f}, {"guides", {F.g[0], F.g[1], F.g[2]}}, {"centroid", v3j(cen)}, {"area_mm2", std::round(A * 10) / 10},
                         {"uv", {r4(F.uv[0]), r4(F.uv[1]), r4(F.uv[2]), r4(F.uv[3]), r4(F.uv[4]), r4(F.uv[5])}}});
        }
        r["count"] = fl.size(); r["faces"] = l;
        r["median_area_mm2"] = all.empty() ? 0 : all[all.size() / 2];
        return r;
    }
    if (d == "densify") {
        // one duplicate of a face's guide at the face centroid (per_face > 1: spread inside the face)
        std::vector<int> fids;
        if (c.contains("faces")) for (auto& f : c["faces"]) fids.push_back(f);
        else {
            auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
            std::set<int> in(ix.begin(), ix.end());
            for (size_t f = 0; f < groom.faces.size(); f++) {
                auto& F = groom.faces[f];
                if ((in.count(F.g[0]) || in.count(F.g[1]) || in.count(F.g[2])) && faceAreaMm2(groom, F) >= c.value("min_area_mm2", 0.0f))
                    fids.push_back(int(f));
            }
        }
        int per = std::max(1, c.value("per_face", 1)), id = 0, grp = 1000 + (++grpId);
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        static const float W[4][3] = {{1 / 3.f, 1 / 3.f, 1 / 3.f}, {0.6f, 0.2f, 0.2f}, {0.2f, 0.6f, 0.2f}, {0.2f, 0.2f, 0.6f}};
        int added = 0;
        std::vector<json> ops;
        for (int f : fids) {
            if (f < 0 || f >= (int)groom.faces.size()) continue;
            auto& F = groom.faces[f];
            for (int q = 0; q < per && q < 4; q++) {
                V3 p = groom.P[F.g[0]][0] * W[q][0] + groom.P[F.g[1]][0] * W[q][1] + groom.P[F.g[2]][0] * W[q][2];
                int src = F.g[0];   // copy the face guide whose root is nearest the new point
                for (int k = 1; k < 3; k++) if (len(groom.P[F.g[k]][0] - p) < len(groom.P[src][0] - p)) src = F.g[k];
                V3 rt = groom.P[src][0], dd = p - rt;
                ops.push_back({{"op", "duplicate"}, {"root", {r4(rt.x), r4(rt.y), r4(rt.z)}}, {"d", {r4(dd.x), r4(dd.y), r4(dd.z)}}, {"id", ++id}, {"grp", grp}});
                added++;
            }
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); smap.ensure(groom.P.size()); editsChanged(); settleRest(*this, 0.5);
        r["faces"] = fids.size(); r["added"] = added; r["status"] = status_();
        return r;
    }
    if (d == "root_to_scalp") {
        // carry each guide so its root lands on the scalp: at "point", or at height z (relative to the head top) on the
        // scalp surface sampled by the other guides' roots in the same direction around the head
        auto ix = selectGuides(c.value("where", json::object()));
        std::set<int> mv(ix.begin(), ix.end());
        int id = 0, grp = 1000 + (++grpId), n = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        for (int i : ix) {
            V3 r0 = groom.P[i][0], tgt;
            if (c.contains("point")) tgt = {c["point"][0], c["point"][1], c["point"][2]};
            else {
                float zt = pack.headTop + c.value("z", -0.03f);
                float az = std::atan2(r0.y - pack.headC.y, r0.x - pack.headC.x);
                double best = 1e30;
                bool found = false;
                for (size_t g = 0; g < groom.P.size(); g++) {
                    if (!groom.alive[g] || mv.count(int(g))) continue;
                    V3 q = groom.P[g][0];
                    float a2 = std::atan2(q.y - pack.headC.y, q.x - pack.headC.x), da = std::fabs(std::remainder(a2 - az, 6.2831853f));
                    double sc = std::fabs(q.z - zt) + 0.06 * da;
                    if (sc < best) { best = sc; tgt = q; found = true; }
                }
                if (!found) continue;
                tgt.z = zt;   // keep the requested height; the neighbour gives the surface radius in that direction
                V3 rel = tgt - pack.headC, dir0 = r0 - pack.headC;
                float rad = std::sqrt(rel.x * rel.x + rel.y * rel.y), dl = std::sqrt(dir0.x * dir0.x + dir0.y * dir0.y);
                if (dl > 1e-6f) { tgt.x = pack.headC.x + dir0.x / dl * rad; tgt.y = pack.headC.y + dir0.y / dl * rad; }
            }
            V3 off = tgt - r0;
            ops.push_back({{"op", "translate"}, {"roots", json::array({{r4(r0.x), r4(r0.y), r4(r0.z)}})}, {"offset", {r4(off.x), r4(off.y), r4(off.z)}},
                           {"falloff", "carry"}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["moved"] = n; r["status"] = status_();
        return r;
    }
    if (d == "apply") {
        // generic op on selected guides: {"type": "translate" | "set_point" | "reshape" | "shorten" | ..., "where", op keys}
        auto ix = selectGuides(c.value("where", json::object()));
        int id = 0, grp = 1000 + (++grpId);
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::string ty = c.value("type", "");
        bool single = ty == "set_point" || ty == "reshape";
        std::vector<json> ops;
        for (int i : ix) {
            json o = c;
            o.erase("do"); o.erase("where"); o.erase("type"); o.erase("show");
            o["op"] = ty;
            V3 r0 = groom.P[i][0];
            if (single) o["root"] = {r4(r0.x), r4(r0.y), r4(r0.z)};
            else o["roots"] = json::array({{r4(r0.x), r4(r0.y), r4(r0.z)}});
            o["id"] = ++id; o["grp"] = grp;
            ops.push_back(o);
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["guides"] = ix.size(); r["status"] = status_();
        return r;
    }
    if (d == "guide_view") {
        // guide visibility views (guidevis.cpp): set the modes (kept for the session: renders with overlay "guides", the
        // UI) and, with camera keys, compute the shown guides from that camera; "select": true selects them (optionally
        // only those with a drawn point inside rect [x, y, w, h] 0..1), as the UI's box select with "Select visible only"
        if (c.contains("front")) guideFront = c["front"].get<bool>();
        if (c.contains("body")) guideBody = c["body"].get<bool>();
        if (c.contains("front_pct")) guideFrontPct = std::clamp(c["front_pct"].get<float>(), 1.0f, 100.0f);
        if (c.contains("tol_mm")) guideTolMm = std::clamp(c["tol_mm"].get<float>(), 0.5f, 50.0f);
        if (c.contains("select_visible")) selectVisibleOnly = c["select_visible"].get<bool>();
        r["front"] = guideFront; r["body"] = guideBody; r["front_pct"] = guideFrontPct; r["tol_mm"] = guideTolMm;
        r["select_visible"] = selectVisibleOnly;
        json cam;
        if ((c.contains("shot") || c.contains("view") || c.contains("eye")) && cameraOf(*this, c, cam)) {
            if (!guideVisOn()) { r["error"] = "no guide view on (front / body)"; return r; }
            int size = c.value("size", 1400);
            XMFLOAT3 eye{cam["eye"][0], cam["eye"][1], cam["eye"][2]}, tg{cam["target"][0], cam["target"][1], cam["target"][2]};
            XMFLOAT4X4 Vm, Pm;
            XMStoreFloat4x4(&Vm, XMMatrixLookAtRH(XMLoadFloat3(&eye), XMLoadFloat3(&tg), XMVectorSet(0, 0, 1, 0)));
            XMStoreFloat4x4(&Pm, XMMatrixPerspectiveFovRH(XMConvertToRadians(cam.value("fovy", 11.16f)), 1.0f, 0.05f, 100.0f));
            computeGuideVis(Vm, Pm, size, size, nullptr, nullptr, false);
            XMMATRIX VP = XMLoadFloat4x4(&Vm) * XMLoadFloat4x4(&Pm);
            bool rect = c.contains("rect") && c["rect"].is_array() && c["rect"].size() == 4;
            float rx = rect ? c["rect"][0].get<float>() : 0, ry = rect ? c["rect"][1].get<float>() : 0;
            float rw = rect ? c["rect"][2].get<float>() : 1, rh = rect ? c["rect"][3].get<float>() : 1;
            auto inside = [&](V3 p) {
                XMVECTOR q = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), VP);
                float w = XMVectorGetW(q);
                if (w <= 1e-5f) return false;
                float sx = XMVectorGetX(q) / w * 0.5f + 0.5f, sy = 1 - (XMVectorGetY(q) / w * 0.5f + 0.5f);
                return sx >= rx && sx <= rx + rw && sy >= ry && sy <= ry + rh;
            };
            std::vector<int> vis;
            int partial = 0;
            for (int i = 0; i < gvis.N; i++) {
                if (!groom.alive[i] || !gvis.shown[i]) continue;
                bool in = false, cut = false;
                for (int k = 0; k < gvis.K; k++) {
                    if (!gvis.pt[size_t(i) * gvis.K + k]) { cut = true; continue; }
                    if (!in && k > 0 && inside(gvis.pts[size_t(i) * gvis.K + k])) in = true;
                }
                if (!in) continue;
                vis.push_back(i);
                partial += cut;
            }
            int lim = c.value("limit", 0);
            json l = json::array();
            for (int i : vis) { if (lim > 0 && (int)l.size() >= lim) break; l.push_back(i); }
            r["camera"] = cam; r["alive"] = groom.numAlive(); r["shown"] = gvis.nShown; r["in_rect"] = vis.size();
            r["clipped_by_body"] = partial; r["ms"] = std::round(gvis.ms * 10) / 10;
            r["ms_parts"] = {{"main_thread_total", std::round(gvis.msMain * 10) / 10}, {"body_raster", std::round(gvis.msBody * 10) / 10}, {"hair_splat", std::round(gvis.msHair * 10) / 10}};
            r["body_tris"] = gvis.nTri; r["buffer_px"] = gvis.res;
            if (lim > 0) r["guides"] = l;
            if (c.value("select", false)) { sel = std::set<int>(vis.begin(), vis.end()); r["selected"] = sel.size(); }
        }
        return r;
    }
    if (d == "pick_region") {
        // guides whose posed roots project inside rect [x, y, w, h] (0..1) of the camera, and faces with their centroid inside
        json cam;
        if (!cameraOf(*this, c, cam)) { r["error"] = "no such camera"; return r; }
        XMFLOAT3 eye{cam["eye"][0], cam["eye"][1], cam["eye"][2]}, tg{cam["target"][0], cam["target"][1], cam["target"][2]};
        XMMATRIX VP = XMMatrixLookAtRH(XMLoadFloat3(&eye), XMLoadFloat3(&tg), XMVectorSet(0, 0, 1, 0)) *
                      XMMatrixPerspectiveFovRH(XMConvertToRadians(cam.value("fovy", 11.16f)), 1.0f, 0.05f, 100.0f);
        float rx = c["rect"][0], ry = c["rect"][1], rw = c["rect"][2], rh = c["rect"][3];
        auto inside = [&](V3 p) {
            XMVECTOR q = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), VP);
            float w = XMVectorGetW(q);
            if (w <= 1e-5f) return false;
            float sx = XMVectorGetX(q) / w * 0.5f + 0.5f, sy = 1 - (XMVectorGetY(q) / w * 0.5f + 0.5f);
            return sx >= rx && sx <= rx + rw && sy >= ry && sy <= ry + rh;
        };
        json gl = json::array(), fl = json::array();
        for (size_t g = 0; g < groom.P.size(); g++)
            if (groom.alive[g] && inside(posedPoint(int(g), 0)))
                gl.push_back({{"i", g}, {"root", v3j(groom.P[g][0])}, {"length", std::round(guideLen(groom.P[g]) * 1e4f) / 1e4f},
                              {"edited", groom.edited.count(int(g)) > 0}});
        for (size_t f = 0; f < groom.faces.size(); f++) {
            auto& F = groom.faces[f];
            V3 cen = (posedPoint(F.g[0], 0) + posedPoint(F.g[1], 0) + posedPoint(F.g[2], 0)) * (1.0f / 3);
            if (inside(cen)) fl.push_back({{"id", f}, {"guides", {F.g[0], F.g[1], F.g[2]}}, {"area_mm2", std::round(faceAreaMm2(groom, F) * 10) / 10}});
        }
        r["guides"] = gl; r["faces"] = fl;
        r["note"] = "roots facing away from the camera are included too (no occlusion test)";
        return r;
    }
    if (d == "hairline_check" || d == "align_to_hairline") {
        Hairline hl;
        if (!hairlineOf(*this, hl)) { r["error"] = "no scalp mask on the head mesh"; return r; }
        json where = c.value("where", json::object({{"all", true}}));
        auto ix = selectGuides(where);
        if (d == "align_to_hairline") {
            // roots on skin outside the painted scalp (or every selected one with "force") move onto the hairline, inset_mm inside
            float inset = c.value("inset_mm", 2.0f) / 1000.0f;
            bool force = c.value("force", false);
            int id = 0, grp = 1000 + (++grpId), n = 0;
            for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
            std::vector<json> ops;
            for (int g : ix) {
                V3 rp = posedPoint(g, 0), re = groom.P[g][0];
                if (!force && onScalp(hl, rp)) continue;
                float dd;
                int b = nearestHl(hl, rp, dd);
                if (b < 0 || (!force && dd * 1000 < c.value("tolerance_mm", 8.0f))) continue;
                V3 tp = hl.pt[b] + nrmz(hl.inner[b] - hl.pt[b]) * inset;
                V3 off = tp - rp;   // posed and edit frames differ by a local shift only
                // default: only the root part moves (offset fades out over fade_mm, 40), the strand keeps its fall;
                // "carry": true moves the whole guide
                Guide Q = moveRoot(groom.P[g], off, c.value("carry", false) ? 1e9f : c.value("fade_mm", 40.0f) / 1000);
                ops.push_back({{"op", "reshape"}, {"root", {r4(re.x), r4(re.y), r4(re.z)}}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
                n++;
            }
            for (auto& o : ops) groom.ops.push_back(o);
            groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
            r["moved"] = n; r["status"] = status_();
            return r;
        }
        // check: roots off the scalp, flow outliers (root direction vs its 8 nearest neighbours), optional marked image
        float maxAng = c.value("max_angle_deg", 60.0f) * 3.14159265f / 180;
        bool hasPart = c.contains("parting") && c["parting"].is_array() && c["parting"].size() == 2;
        V3 pp0{}, pp1{};
        if (hasPart) { pp0 = {c["parting"][0][0], c["parting"][0][1], c["parting"][0][2]}; pp1 = {c["parting"][1][0], c["parting"][1][1], c["parting"][1][2]}; }
        float partBand = c.value("parting_band_mm", 8.0f) / 1000;
        auto nearPart = [&](V3 q) {
            if (!hasPart) return false;
            V3 L = pp1 - pp0;
            float t = std::clamp(dot(q - pp0, L) / std::max(dot(L, L), 1e-9f), 0.0f, 1.0f);
            return len(pp0 + L * t - q) < partBand;
        };
        float liftMin = c.value("lift_deg", 35.0f);
        json lifted = json::array();
        std::vector<int> liftI;
        std::vector<int> alive;
        for (size_t g = 0; g < groom.P.size(); g++) if (groom.alive[g]) alive.push_back(int(g));
        std::vector<V3> tan(groom.P.size());
        for (int g : alive) tan[g] = rootTangent(*this, g);
        json off = json::array(), odd = json::array();
        std::vector<int> offI, oddI;
        double angSum = 0;
        int angN = 0;
        for (int g : ix) {
            V3 rp = posedPoint(g, 0);
            if (!onScalp(hl, rp)) {
                float dd;
                nearestHl(hl, rp, dd);
                off.push_back({{"i", g}, {"root", v3j(groom.P[g][0])}, {"to_hairline_mm", std::round(dd * 10000) / 10}});
                offI.push_back(g);
            }
            float lf = liftDeg(*this, g, 0.02f);
            if (lf > liftMin && guideLen(groom.P[g]) < c.value("lift_max_length", 0.25f)) {
                lifted.push_back({{"i", g}, {"root", v3j(groom.P[g][0])}, {"lift_deg", std::round(lf)}, {"length", std::round(guideLen(groom.P[g]) * 1e4f) / 1e4f}});
                liftI.push_back(g);
            }
            if (nearPart(groom.P[g][0])) continue;   // the two sides of a parting flow apart on purpose
            std::vector<std::pair<float, int>> nb;
            for (int h : alive) if (h != g) nb.push_back({len(groom.P[h][0] - groom.P[g][0]), h});
            size_t kk = std::min<size_t>(8, nb.size());
            std::partial_sort(nb.begin(), nb.begin() + kk, nb.end());
            V3 m{0, 0, 0};
            for (size_t q = 0; q < kk; q++) m = m + tan[nb[q].second];
            if (len(m) / 8 < 0.5f) continue;   // the neighbourhood itself has no clear flow (crown whorl, parting line)
            float ang = std::acos(std::clamp(dot(tan[g], nrmz(m)), -1.0f, 1.0f));
            angSum += ang;
            angN++;
            if (ang > maxAng) {
                odd.push_back({{"i", g}, {"root", v3j(groom.P[g][0])}, {"angle_deg", std::round(ang * 1800 / 3.14159265f) / 10},
                               {"length", std::round(guideLen(groom.P[g]) * 1e4f) / 1e4f}});
                oddI.push_back(g);
            }
        }
        r["checked"] = ix.size();
        r["roots_off_scalp"] = off; r["roots_off_scalp_count"] = off.size();
        r["flow_outliers"] = odd; r["flow_outliers_count"] = odd.size();
        r["mean_flow_deviation_deg"] = angN ? angSum / angN * 180 / 3.14159265 : 0;
        r["lifted"] = lifted; r["lifted_count"] = lifted.size();
        json fl = json::array();
        r["guides_over_face"] = overFace(*this, &fl);
        r["over_face"] = fl;
        r["legend"] = "image: magenta dots = painted hairline, red rings = roots off the scalp, yellow rings = flow outliers, cyan rings = lifted (frizz)";
        json cam;
        if ((c.contains("shot") || c.contains("view") || c.contains("eye")) && cameraOf(*this, c, cam)) {
            std::string png = c.value("path", out + "\\hairline.png");
            int size = c.value("size", 1400);
            bool so = scalpOverlay;
            scalpOverlay = c.value("overlay", std::string("none")) == "scalp";
            bool ok = renderShot(cam, png, size);
            scalpOverlay = so;
            int w = 0, h = 0;
            unsigned char* im = ok ? loadImage(png, &w, &h) : nullptr;
            if (im) {
                XMFLOAT3 eye{cam["eye"][0], cam["eye"][1], cam["eye"][2]}, tg{cam["target"][0], cam["target"][1], cam["target"][2]};
                XMMATRIX VP = XMMatrixLookAtRH(XMLoadFloat3(&eye), XMLoadFloat3(&tg), XMVectorSet(0, 0, 1, 0)) *
                              XMMatrixPerspectiveFovRH(XMConvertToRadians(cam.value("fovy", 11.16f)), 1.0f, 0.05f, 100.0f);
                V3 E{eye.x, eye.y, eye.z};
                auto proj = [&](V3 p, int& x, int& y) {
                    if (dot(p - pack.headC, E - p) <= 0) return false;
                    XMVECTOR q = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), VP);
                    float ww = XMVectorGetW(q);
                    if (ww <= 1e-5f) return false;
                    x = int((XMVectorGetX(q) / ww * 0.5f + 0.5f) * w);
                    y = int((1 - (XMVectorGetY(q) / ww * 0.5f + 0.5f)) * h);
                    return x >= 0 && y >= 0 && x < w && y < h;
                };
                auto put = [&](int x, int y, unsigned char R, unsigned char G, unsigned char B) {
                    if (x < 0 || y < 0 || x >= w || y >= h) return;
                    unsigned char* z = &im[(size_t(y) * w + x) * 4];
                    z[0] = R; z[1] = G; z[2] = B; z[3] = 255;
                };
                auto ring = [&](V3 p, unsigned char R, unsigned char G, unsigned char B) {
                    int x, y;
                    if (!proj(p, x, y)) return;
                    for (int a = 0; a < 48; a++)
                        for (int rr = 6; rr <= 8; rr++) put(x + int(rr * std::cos(a * 0.1309f)), y + int(rr * std::sin(a * 0.1309f)), R, G, B);
                };
                for (auto& p : hl.pt) {
                    int x, y;
                    if (proj(p, x, y)) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) put(x + dx, y + dy, 255, 0, 255);
                }
                for (int g : offI) ring(posedPoint(g, 0), 255, 40, 40);
                for (int g : oddI) ring(posedPoint(g, 0), 255, 220, 0);
                for (int g : liftI) ring(posedPoint(g, 0), 0, 200, 255);
                writePng(png, w, h, im);
                freeImage(im);
                r["png"] = png;
            }
        }
        return r;
    }
    if (d == "flow_comb") {
        // turn the root part of guides around the scalp normal toward a target flow: "away_from": [[p0], [p1]] (parting line:
        // each side combs away from it), "direction": [x, y, z], or the mean of the 8 nearest guides (default). strength 0..1,
        // span_mm = root length that turns (30; the rest keeps its direction), max_turn_deg caps the turn (90)
        auto ix = selectGuides(c.value("where", json::object()));
        std::string mode = c.contains("away_from") ? "away" : c.contains("direction") ? "dir" : "nb";
        float strength = std::clamp(c.value("strength", 1.0f), 0.0f, 1.0f), spanM = std::clamp(c.value("span_mm", 30.0f), 3.0f, 200.0f) / 1000;
        float maxTurn = c.value("max_turn_deg", 90.0f) * 3.14159265f / 180;
        float minAng = c.value("min_angle_deg", 0.0f) * 3.14159265f / 180;
        std::vector<V3> tan(groom.P.size());
        std::vector<int> alive;
        for (size_t g = 0; g < groom.P.size(); g++)
            if (groom.alive[g]) { alive.push_back(int(g)); tan[g] = rootTangent(*this, int(g)); }
        int id = 0, grp = 1000 + (++grpId), n = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        for (int g : ix) {
            auto G = groom.P[g];
            V3 root = G[0], nrm = nrmz(root - pack.headC), want{0, 0, 0};
            if (mode == "away") {
                V3 p0{c["away_from"][0][0], c["away_from"][0][1], c["away_from"][0][2]}, p1{c["away_from"][1][0], c["away_from"][1][1], c["away_from"][1][2]};
                V3 L = nrmz(p1 - p0), q = root - p0, perp = q - L * dot(q, L);
                want = perp - nrm * dot(perp, nrm);
            } else if (mode == "dir") {
                V3 dv{c["direction"][0], c["direction"][1], c["direction"][2]};
                want = dv - nrm * dot(dv, nrm);
            } else {
                std::vector<std::pair<float, int>> nb;
                for (int h : alive) if (h != g) nb.push_back({len(groom.P[h][0] - root), h});
                size_t kk = std::min<size_t>(8, nb.size());
                std::partial_sort(nb.begin(), nb.begin() + kk, nb.end());
                for (size_t q = 0; q < kk; q++) want = want + tan[nb[q].second];
            }
            if (len(want) < 1e-6f) continue;
            want = nrmz(want);
            V3 t = tan[g];
            float ang = std::atan2(dot(cross(t, want), nrm), dot(t, want));
            if (std::fabs(ang) < minAng) continue;
            ang = std::clamp(ang * strength, -maxTurn, maxTurn);
            Guide Q = turnRoot(G, nrm, ang, spanM);
            ops.push_back({{"op", "reshape"}, {"root", {r4(root.x), r4(root.y), r4(root.z)}}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["combed"] = n; r["status"] = status_();
        return r;
    }
    if (d == "push_out") {
        // guides passing INSIDE the head (their hair shows up as dark lines where it comes out through the skin, e.g. on
        // the forehead): points under the skin are pushed out to standoff_mm (3) above it
        HeadRadial hr = headRadial(*this);
        if (!hr.ok) { r["error"] = "no head mesh"; return r; }
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        float so = c.value("standoff_mm", 3.0f) / 1000;
        int id = 0, grp = 1000 + (++grpId), n = 0, pts = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        json l = json::array(), sp = json::array();
        int nSpan = 0;
        for (int g : ix) {
            Guide Q = groom.P[g];
            // read-only report: spans dipping under the skin between CVs (> 1 mm); delete those guides or re-space their CVs
            float dip = spanDip(hr, Q);
            if (dip > 0.001f) { nSpan++; if (sp.size() < 60) sp.push_back({{"i", g}, {"dip_mm", r4(dip * 1000)}}); }
            int m = pushOut(hr, Q, so, c.value("depth_mm", 8.0f) / 1000);
            if (!m) continue;
            V3 r0 = Q[0];
            // "delete": true removes such guides (usually bad copies; pushed out they hang as stray hair)
            if (c.value("delete", false)) ops.push_back({{"op", "delete"}, {"roots", json::array({{r4(r0.x), r4(r0.y), r4(r0.z)}})}, {"id", ++id}, {"grp", grp}});
            else ops.push_back({{"op", "reshape"}, {"root", {r4(r0.x), r4(r0.y), r4(r0.z)}}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
            if (l.size() < 60) l.push_back({{"i", g}, {"points_inside", m}});
            n++; pts += m;
        }
        if (!c.value("list_only", false)) {
            for (auto& o : ops) groom.ops.push_back(o);
            groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        }
        r["guides_inside"] = n; r["points_moved"] = pts; r["guides"] = l;
        r["spans_under_skin"] = nSpan; r["span_guides"] = sp; r["status"] = status_();
        return r;
    }
    if (d == "resample") {
        // 2026-10-14 opt-in: give EVERY guide n CVs (3..32), evenly spaced along the smooth (centripetal Catmull-Rom)
        // curve through its current CVs, root and tip exact. One "resample" op (the Studio keeps one CV count for all
        // guides; "where" is not taken). More CVs = curls / waves / lengthening keep their shape (6+ CVs per curl turn);
        // render cost grows with the CVs ((n-1) x splineMultiplier + 1 vertices per hair). Reports the shape change.
        int n = std::clamp(c.value("n", 16), 3, kMaxCvs);
        int K0 = groom.K;
        if (n == K0) { r["changed"] = 0; r["cvs"] = n; r["note"] = "already " + std::to_string(n) + " CVs"; r["status"] = status_(); return r; }
        float mx = 0, sum = 0;
        int cnt = 0, worst = -1;
        for (size_t i = 0; i < groom.P.size(); i++) {
            if (!groom.alive[i]) continue;
            Guide Q = resampleSmooth(groom.P[i], n);
            float a, m;
            curveDeviation(groom.P[i], Q, a, m);
            if (a > mx) { mx = a; worst = (int)i; }
            sum += m; cnt++;
        }
        int id = 0, grp = 1000 + (++grpId);
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        groom.ops.push_back({{"op", "resample"}, {"n", n}, {"id", id + 1}, {"grp", grp}});
        groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["changed"] = cnt; r["cvs_before"] = K0; r["cvs"] = groom.K;
        r["max_dev_mm"] = r4(mx * 1000.0); r["mean_dev_mm"] = r4(cnt ? sum / cnt * 1000.0 : 0.0); r["worst_guide"] = worst;
        r["render_vertices_per_hair"] = (groom.K - 1) * std::max(1, pack.splineMultiplier) + 1;
        r["status"] = status_();
        return r;
    }
    if (d == "lengthen") {
        // make guides longer (factor > 1) or shorter (< 1) keeping the point count: the CVs are re-placed evenly along the
        // SMOOTH curve through the old CVs (centripetal Catmull-Rom, not the chords: chord resampling of few CVs cut the
        // waves into straight hay-like spikes); past the old tip the strand continues along its tip tangent, bending
        // toward gravity (droop 0..1) a little more at each CV. Under 12 CVs it warns: resample first (studio_resample).
        auto ix = selectGuides(c.value("where", json::object()));
        float fac = std::clamp(c.value("factor", 1.2f), 0.3f, 3.0f), droop = std::clamp(c.value("droop", 0.5f), 0.0f, 1.0f);
        int id = 0, grp = 1000 + (++grpId), n = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        HeadRadial hr = headRadial(*this);
        for (int g : ix) {
            const Guide& G = groom.P[g];
            size_t K = G.size();
            if (K < 3) continue;
            std::vector<V3> dense = smoothDense(G, 24);
            float L = 0;
            for (size_t q = 1; q < dense.size(); q++) L += len(dense[q] - dense[q - 1]);
            if (L < 1e-4f) continue;
            float stepL = L * fac / float(K - 1);
            std::vector<float> sv;
            for (size_t k = 0; k < K; k++) sv.push_back(std::min(L * fac * float(k) / float(K - 1), L));
            Guide Q = sampleSmoothAt(G, sv);
            V3 lastDir = nrmz(dense.back() - dense[dense.size() - 2]);
            for (size_t k = 1; k < K; k++) {
                float s = L * fac * float(k) / float(K - 1);
                if (s <= L + 1e-6f) continue;
                // extension: the remaining part of this step past the old tip, then whole steps, each turning toward down
                float prevS = L * fac * float(k - 1) / float(K - 1);
                float seg = prevS < L ? s - L : stepL;
                V3 from = prevS < L ? dense.back() : Q[k - 1];
                V3 dir = nrmz(lastDir * (1 - droop) + V3{0, 0, -1} * droop);
                Q[k] = from + dir * seg;
                lastDir = dir;
            }
            Q[0] = G[0];
            pushOut(hr, Q, 0.003f);
            V3 r0 = G[0];
            ops.push_back({{"op", "reshape"}, {"root", {r4(r0.x), r4(r0.y), r4(r0.z)}}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["changed"] = n; r["status"] = status_();
        if (groom.K < 12 && fac > 1.05f) r["warning"] = "guides have only " + std::to_string(groom.K) + " CVs: studio_resample (n 16-24) first keeps the shape when lengthening";
        return r;
    }
    if (d == "clear_ear") {
        // tuck hair behind the ears: ears are found on the head mesh (most lateral head vertices at eye height, behind
        // the eyes). Guide points within radius_mm of an ear and in front of / over it are moved back behind the ear
        // (and out of the head), smoothly along the strand, lengths kept. side: "both" | "left" | "right".
        V3 eLo{1e9f, 1e9f, 1e9f}, eHi{-1e9f, -1e9f, -1e9f};
        for (auto& m : pack.meshes) if (m.texFile.find("eye") != std::string::npos)
            for (auto& v : m.v) { eLo = {std::min(eLo.x, v.x), std::min(eLo.y, v.y), std::min(eLo.z, v.z)}; eHi = {std::max(eHi.x, v.x), std::max(eHi.y, v.y), std::max(eHi.z, v.z)}; }
        if (eLo.x > 1e8f) { r["error"] = "no eye mesh in this pack (needed to place the ears)"; return r; }
        float eyeZ = (eLo.z + eHi.z) / 2, eyeY = (eLo.y + eHi.y) / 2;
        V3 earC[2], earLo[2], earHi[2]; bool has[2] = {false, false};
        for (int sd = 0; sd < 2; sd++) {
            float best = 0;
            std::vector<V3> cand;
            for (auto& m : pack.meshes) if (m.kind == "head") for (auto& v : m.v) {
                if (v.z < eyeZ - 0.06f || v.z > eyeZ + 0.015f || v.y > eyeY - 0.05f || v.y < eyeY - 0.15f) continue;
                float lx = sd == 0 ? v.x - pack.headC.x : pack.headC.x - v.x;
                if (lx <= 0) continue;
                cand.push_back(v); best = std::max(best, lx);
            }
            V3 acc{0, 0, 0}; int n = 0;
            for (auto& v : cand) { float lx = sd == 0 ? v.x - pack.headC.x : pack.headC.x - v.x; if (lx > best - 0.012f) { acc = acc + v; n++; } }
            if (n) { earC[sd] = acc * (1.0f / n); has[sd] = true; }
            // whole ear: head vertices lateral beyond the skull (within 2.5 cm of the outermost); long pointed (elf)
            // ears reach far above eye height
            earLo[sd] = earC[sd]; earHi[sd] = earC[sd];
            if (n) for (auto& m : pack.meshes) if (m.kind == "head") for (auto& v : m.v) {
                float lx = sd == 0 ? v.x - pack.headC.x : pack.headC.x - v.x;
                if (lx < best - 0.035f || v.z < eyeZ - 0.09f || v.z > eyeZ + 0.12f || v.y > eyeY - 0.04f || v.y < eyeY - 0.17f) continue;
                earLo[sd] = {std::min(earLo[sd].x, v.x), std::min(earLo[sd].y, v.y), std::min(earLo[sd].z, v.z)};
                earHi[sd] = {std::max(earHi[sd].x, v.x), std::max(earHi[sd].y, v.y), std::max(earHi[sd].z, v.z)};
            }
        }
        std::string side = c.value("side", "both");
        float R = c.value("radius_mm", 30.0f) / 1000, back = c.value("back_mm", 0.0f) / 1000, tuck = c.value("tuck_mm", 12.0f) / 1000;
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        HeadRadial hr = headRadial(*this);
        int id = 0, grp = 1000 + (++grpId), n = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        json ears = json::array();
        for (int sd = 0; sd < 2; sd++) if (has[sd]) ears.push_back({{"side", sd == 0 ? "left" : "right"}, {"centre", v3j(earC[sd])}, {"box_lo", v3j(earLo[sd])}, {"box_hi", v3j(earHi[sd])}});
        // delete_near_mm: guides ROOTED within this distance of an ear box whose path crosses the ear (outside it)
        // are deleted instead of moved - moving them only bares the temple (vanilla grows no hair there)
        float delNear = c.value("delete_near_mm", 0.0f) / 1000;
        int deleted = 0, facesOff = 0;
        // remove_faces: growth faces with a corner rooted ON the ear (box + margin_mm): the growth mesh often covers
        // the ear, so hair grows out of it - no move can clear that
        if (c.value("remove_faces", false)) {
            float mg = c.value("margin_mm", 4.0f) / 1000;
            std::set<int> onEar;
            for (int g = 0; g < (int)groom.P.size(); g++) {
                if (!groom.alive[g]) continue;
                V3 p = groom.P[g][0];
                for (int sd = 0; sd < 2; sd++) {
                    if (!has[sd] || (side == "left" && sd != 0) || (side == "right" && sd != 1)) continue;
                    if (p.x >= earLo[sd].x - mg && p.x <= earHi[sd].x + mg && p.y >= earLo[sd].y - mg && p.y <= earHi[sd].y + mg &&
                        p.z >= earLo[sd].z - mg && p.z <= earHi[sd].z + mg) onEar.insert(g);
                }
            }
            for (size_t f = 0; f < groom.faces.size(); f++) {
                auto& F = groom.faces[f];
                if (!onEar.count(F.g[0]) && !onEar.count(F.g[1]) && !onEar.count(F.g[2])) continue;
                json rr = json::array();
                for (int q = 0; q < 3; q++) { V3 p = groom.P[F.g[q]][0]; rr.push_back({r4(p.x), r4(p.y), r4(p.z)}); }
                ops.push_back({{"op", "delete_face"}, {"roots", rr}, {"id", ++id}, {"grp", grp}});
                facesOff++;
            }
            for (int g : onEar) {   // the guides themselves render as hair too
                V3 p = groom.P[g][0];
                ops.push_back({{"op", "delete"}, {"roots", json::array({{r4(p.x), r4(p.y), r4(p.z)}})}, {"id", ++id}, {"grp", grp}});
            }
            r["guides_rooted_on_ear"] = onEar.size();
        }
        std::set<int> gone;
        for (auto& o : ops) if (o["op"] == "delete") for (int g = 0; g < (int)groom.P.size(); g++) {
            V3 p = groom.P[g][0]; auto& q = o["roots"][0];
            if (std::fabs(p.x - q[0].get<float>()) < 1e-4f && std::fabs(p.y - q[1].get<float>()) < 1e-4f && std::fabs(p.z - q[2].get<float>()) < 1e-4f) { gone.insert(g); break; }
        }
        for (int g : ix) {
            if (gone.count(g)) continue;
            const Guide& G = groom.P[g];
            size_t K = G.size();
            if (K < 3) continue;
            if (delNear > 0) {
                bool kill = false;
                for (int sd = 0; sd < 2 && !kill; sd++) {
                    if (!has[sd] || (side == "left" && sd != 0) || (side == "right" && sd != 1)) continue;
                    auto boxD = [&](V3 p) { V3 q{std::clamp(p.x, earLo[sd].x, earHi[sd].x), std::clamp(p.y, earLo[sd].y, earHi[sd].y), std::clamp(p.z, earLo[sd].z, earHi[sd].z)}; return len(p - q); };
                    if (boxD(G[0]) > delNear) continue;
                    for (size_t k = 1; k < K && !kill; k++) {
                        V3 p = G[k];
                        float lx = sd == 0 ? p.x - earLo[sd].x : earHi[sd].x - p.x;   // outside the ear's inner face
                        if (lx > -0.003f && p.y >= earLo[sd].y - 0.005f && p.y <= earHi[sd].y + 0.005f && p.z >= earLo[sd].z - 0.005f && p.z <= earHi[sd].z + 0.01f) kill = true;
                    }
                }
                if (kill) {
                    V3 r0 = G[0];
                    ops.push_back({{"op", "delete"}, {"roots", json::array({{r4(r0.x), r4(r0.y), r4(r0.z)}})}, {"id", ++id}, {"grp", grp}});
                    deleted++;
                    continue;
                }
            }
            Guide Q = G;
            bool any = false;
            for (int sd = 0; sd < 2; sd++) {
                if (!has[sd] || (side == "left" && sd != 0) || (side == "right" && sd != 1)) continue;
                for (size_t k = 1; k < K; k++) {
                    // distance to the ear box (not its centre): covers tall pointed ears
                    V3 q{std::clamp(Q[k].x, earLo[sd].x, earHi[sd].x), std::clamp(Q[k].y, earLo[sd].y, earHi[sd].y), std::clamp(Q[k].z, earLo[sd].z, earHi[sd].z)};
                    float dd = len(Q[k] - q);
                    if (dd > R || Q[k].y < earLo[sd].y - back * 0.4f) continue;   // already behind the ear
                    float w = 1.0f - dd / R;
                    w = std::min(1.0f, w * 1.6f);
                    // tuck UNDER the ear: press the point against the skull on the inner side of the ear (vanilla hair
                    // runs between ear and skull, the ear stays in front). Moving it back along the strand does not work:
                    // the length-keeping re-walk pulls it onto the same line.
                    float inner = sd == 0 ? earLo[sd].x - tuck : earHi[sd].x + tuck;
                    bool outside = sd == 0 ? Q[k].x > inner : Q[k].x < inner;
                    if (outside) { Q[k].x = Q[k].x + (inner - Q[k].x) * w; any = true; }
                    if (back > 0 && Q[k].y > earHi[sd].y) { Q[k].y = Q[k].y + ((earLo[sd].y - back) - Q[k].y) * w * 0.5f; any = true; }
                }
            }
            if (!any) continue;
            for (int it = 0; it < 2; it++) { Guide T = Q; for (size_t k = 1; k + 1 < K; k++) T[k] = Q[k] * 0.5f + (Q[k - 1] + Q[k + 1]) * 0.25f; Q = T; }
            auto sl = arcLen(G);
            Guide R2 = Q;
            for (size_t k = 1; k < K; k++) R2[k] = R2[k - 1] + nrmz(Q[k] - R2[k - 1]) * (sl[k] - sl[k - 1]);
            pushOut(hr, R2, 0.003f);
            V3 r0 = G[0];
            ops.push_back({{"op", "reshape"}, {"root", {r4(r0.x), r4(r0.y), r4(r0.z)}}, {"points", ptsJson(R2)}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["changed"] = n; r["deleted"] = deleted; r["faces_removed"] = facesOff; r["ears"] = ears; r["status"] = status_();
        return r;
    }
    if (d == "smooth_flow") {
        // declump: each guide's shape (points relative to its root, resampled to the same arc fractions) is blended
        // with the mean shape of the guides whose roots lie within radius_mm. Neighbours then flow together like a
        // smooth card surface instead of clumps / crossing strands. strength 0..1, passes N. Roots stay put.
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        float rad = c.value("radius_mm", 12.0f) / 1000, st = std::clamp(c.value("strength", 0.6f), 0.0f, 1.0f);
        int passes = std::clamp(c.value("passes", 2), 1, 6);
        std::vector<char> sel(groom.P.size(), 0);
        for (int g : ix) sel[g] = 1;
        std::vector<Guide> cur = groom.P;
        HeadRadial hr = headRadial(*this);
        for (int p = 0; p < passes; p++) {
            std::vector<Guide> nxt = cur;
            for (int g : ix) {
                const Guide& G = cur[g];
                size_t K = G.size();
                if (K < 3) continue;
                // average SEGMENT DIRECTIONS of neighbours (same arc fraction), not positions: strands rooted on a
                // curved scalp keep their own place, only their flow aligns
                V3 acc[64]; size_t KK = std::min<size_t>(K, 64);
                for (size_t k = 0; k < KK; k++) acc[k] = V3{0, 0, 0};
                float wsum = 0;
                for (size_t h = 0; h < cur.size(); h++) {
                    const Guide& H = cur[h];
                    if (H.size() != K || !groom.alive[h]) continue;
                    float dd = len(H[0] - G[0]);
                    if (dd > rad) continue;
                    float w = 1.0f - dd / rad;
                    for (size_t k = 1; k < KK; k++) acc[k] = acc[k] + nrmz(H[k] - H[k - 1]) * w;
                    wsum += w;
                }
                if (wsum <= 0) continue;
                auto sl = arcLen(G);
                Guide R2 = G;
                for (size_t k = 1; k < K; k++) {
                    V3 own = nrmz(G[k] - G[k - 1]);
                    V3 dir = k < KK ? nrmz(own * (1 - st) + nrmz(acc[k]) * st) : own;
                    R2[k] = R2[k - 1] + dir * (sl[k] - sl[k - 1]);
                }
                pushOut(hr, R2, 0.003f);
                nxt[g] = R2;
            }
            cur = nxt;
        }
        int id = 0, grp = 1000 + (++grpId), n = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        for (int g : ix) {
            V3 r0 = groom.P[g][0];
            ops.push_back({{"op", "reshape"}, {"root", {r4(r0.x), r4(r0.y), r4(r0.z)}}, {"points", ptsJson(cur[g])}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["changed"] = n; r["status"] = status_();
        return r;
    }
    if (d == "make_bun") {
        // 2026-10-12 opt-in Studio tool (buns that hold): find the vanilla bun in the game's hair cards (the most
        // protruding card mass off the skull, or "centre" + "radius_mm"), then reshape the gathered guides: root ->
        // along the scalp to the bun base -> wound around the bun axis (turns, shrinking radius, end tucked inside).
        // Guides rooted in front (keep_front_mm forward of the head centre, forward = away from the bun) stay as they
        // are. Hold: per-guide stiffness map s = r = 1 on the bun guides (painted map, exported as stiffnessTex /
        // rootStiffnessTex) + global stiffness / curves raised (kept guides get map value keep_s so they stay softer).
        // Reshape ops only (pipeline-compatible, undoable); the look / stiffness-map change is saved in the project.
        std::vector<V3> cardV;
        for (auto& cm : pack.meshes) if (cm.kind == "hair_cards") for (auto& v : cm.v) cardV.push_back(v);
        if (cardV.empty()) { r["error"] = "this pack has no vanilla hair cards (re-run desk_prep)"; return r; }
        HeadRadial hr = headRadial(*this);
        const V3 C = pack.headC;
        auto surf = [&](V3 u) { float s = hr.ok ? hr.surface(u) : -1; return s > 0 ? s : 0.09f; };
        // ---- bun detection
        V3 bc{0, 0, 0};
        float rad = c.value("radius_mm", 0.0f) / 1000;
        std::vector<V3> bunV;
        if (c.contains("centre") && c["centre"].is_array() && c["centre"].size() == 3) {
            bc = V3{c["centre"][0].get<float>(), c["centre"][1].get<float>(), c["centre"][2].get<float>()};
            float rr = rad > 0 ? rad * 1.5f : 0.05f;
            for (auto& v : cardV) if (len(v - bc) < rr) bunV.push_back(v);
        } else {
            std::vector<std::pair<float, int>> so;
            float zb = c.value("z_below_mm", 110.0f) / 1000;
            for (int i = 0; i < (int)cardV.size(); i++) {
                V3 dd = cardV[i] - C; float rr = len(dd);
                if (rr < 1e-4f || cardV[i].z < C.z - zb) continue;
                so.push_back({rr - surf(dd * (1.0f / rr)), i});
            }
            if (so.empty()) { r["error"] = "no card vertices"; return r; }
            std::sort(so.begin(), so.end(), [](const std::pair<float, int>& x, const std::pair<float, int>& y) { return x.first > y.first; });
            size_t nt = std::max<size_t>(5, so.size() / 50);
            V3 seed{0, 0, 0};
            for (size_t i = 0; i < nt; i++) seed = seed + cardV[so[i].second];
            seed = seed * (1.0f / nt);
            for (auto& pr : so) if (pr.first > 0.012f && len(cardV[pr.second] - seed) < 0.065f) bunV.push_back(cardV[pr.second]);
            if (bunV.size() < 10) { r["error"] = "no bun found in the cards (give centre + radius_mm)"; return r; }
            for (auto& v : bunV) bc = bc + v;
            bc = bc * (1.0f / bunV.size());
        }
        V3 ax = nrmz(bc - C);
        if (c.contains("axis") && c["axis"].is_array() && c["axis"].size() == 3)
            ax = nrmz(V3{c["axis"][0].get<float>(), c["axis"][1].get<float>(), c["axis"][2].get<float>()});
        float sk = surf(ax);
        V3 B = C + ax * sk;                         // skull point under the bun
        std::vector<float> perp; float depth = 0.0f;
        for (auto& v : bunV) { V3 q = v - B; float a = dot(q, ax); depth = std::max(depth, a); perp.push_back(len(q - ax * a)); }
        std::sort(perp.begin(), perp.end());
        if (rad <= 0) rad = perp.empty() ? 0.03f : perp[perp.size() * 7 / 10];
        rad = std::clamp(rad * c.value("radius_scale", 1.0f), 0.012f, 0.07f);
        depth = std::clamp(c.value("depth_mm", depth * 1000) / 1000 * c.value("depth_scale", 1.0f), 0.015f, 0.09f);
        V3 e1 = nrmz(cross(ax, std::fabs(ax.z) < 0.9f ? V3{0, 0, 1} : V3{1, 0, 0})), e2 = cross(ax, e1);
        // ---- lobes (2026-10-12, opt-in "lobes" / "auto_lobes"): a multi-lobe bun = several gathered bundles, each with its
        // own centre / axis / radius / depth and a lift (distance of its base off the skull: Anna's crest sits up to 12 cm
        // off the crown). Each gathered root goes to the nearest lobe (distance to its base / weight). Without these
        // keys there is one lobe = the bun above (old behaviour, same numbers).
        struct Lobe { V3 bc, ax, B, e1, e2; float rad, depth, lift, w, turns; };
        std::vector<Lobe> lobes;
        auto lobeFrame = [&](Lobe& L) {
            L.B = C + L.ax * surf(L.ax);
            L.e1 = nrmz(cross(L.ax, std::fabs(L.ax.z) < 0.9f ? V3{0, 0, 1} : V3{1, 0, 0})); L.e2 = cross(L.ax, L.e1);
        };
        // size a lobe from the card vertices around its centre: radius = 70th percentile of the lateral distance, the
        // lobe spans [a_c - r, a_c + r] along its axis (a_c = axial position of the centre over the skull)
        auto lobeFromCards = [&](Lobe& L, float search, float radIn, float depthIn) {
            std::vector<float> pp;
            for (auto& v : cardV) if (len(v - L.bc) < search) { V3 q = v - L.bc; float a = dot(q, L.ax); pp.push_back(len(q - L.ax * a)); }
            std::sort(pp.begin(), pp.end());
            float r = radIn > 0 ? radIn : (pp.empty() ? 0.03f : pp[pp.size() * 7 / 10]);
            r = std::clamp(r * c.value("radius_scale", 1.0f), 0.008f, 0.07f);
            float ac = dot(L.bc - L.B, L.ax);
            L.rad = r;
            L.lift = std::clamp(ac - r, 0.0f, c.value("max_lift_mm", 120.0f) / 1000);
            L.depth = std::clamp((depthIn > 0 ? depthIn : 2 * r) * c.value("depth_scale", 1.0f), 0.015f, 0.09f);
        };
        if (c.contains("lobes") && c["lobes"].is_array() && !c["lobes"].empty()) {
            for (auto& lj : c["lobes"]) {
                if (!lj.contains("centre") || !lj["centre"].is_array() || lj["centre"].size() != 3) continue;
                Lobe L;
                L.bc = V3{lj["centre"][0].get<float>(), lj["centre"][1].get<float>(), lj["centre"][2].get<float>()};
                L.ax = nrmz(L.bc - C);
                if (lj.contains("axis") && lj["axis"].is_array() && lj["axis"].size() == 3)
                    L.ax = nrmz(V3{lj["axis"][0].get<float>(), lj["axis"][1].get<float>(), lj["axis"][2].get<float>()});
                lobeFrame(L);
                lobeFromCards(L, lj.value("search_mm", 50.0f) / 1000, lj.value("radius_mm", 0.0f) / 1000, lj.value("depth_mm", 0.0f) / 1000);
                if (lj.contains("lift_mm")) L.lift = lj["lift_mm"].get<float>() / 1000;
                L.w = std::max(0.05f, lj.value("weight", 1.0f));
                L.turns = lj.value("turns", c.value("turns", 2.0f));
                lobes.push_back(L);
            }
        } else if (c.value("auto_lobes", 0) >= 2) {
            // k-means (k = auto_lobes) on the card vertices standing > 12 mm off the skull within lobe_search_mm of the bun
            int kk = std::min(4, c.value("auto_lobes", 2));
            float srch = c.value("lobe_search_mm", 90.0f) / 1000;
            std::vector<V3> pts;
            for (auto& v : cardV) {
                V3 dd = v - C; float rr = len(dd);
                if (rr < 1e-4f || len(v - bc) > srch) continue;
                if (rr - surf(dd * (1.0f / rr)) > 0.012f) pts.push_back(v);
            }
            if ((int)pts.size() >= kk * 8) {
                std::vector<V3> cen;
                // seeds: spread along the main spread direction of the points (deterministic)
                V3 mu{0, 0, 0}; for (auto& p : pts) mu = mu + p; mu = mu * (1.0f / pts.size());
                V3 far1 = pts[0]; float fd = -1;
                for (auto& p : pts) if (len(p - mu) > fd) { fd = len(p - mu); far1 = p; }
                cen.push_back(far1);
                while ((int)cen.size() < kk) {
                    V3 best = pts[0]; float bd = -1;
                    for (auto& p : pts) { float m = 1e9f; for (auto& q : cen) m = std::min(m, len(p - q)); if (m > bd) { bd = m; best = p; } }
                    cen.push_back(best);
                }
                std::vector<int> lab(pts.size(), 0);
                for (int it = 0; it < 40; it++) {
                    for (size_t i = 0; i < pts.size(); i++) { float m = 1e9f; for (int j = 0; j < kk; j++) { float dj = len(pts[i] - cen[j]); if (dj < m) { m = dj; lab[i] = j; } } }
                    for (int j = 0; j < kk; j++) { V3 s{0, 0, 0}; int nj = 0; for (size_t i = 0; i < pts.size(); i++) if (lab[i] == j) { s = s + pts[i]; nj++; } if (nj) cen[j] = s * (1.0f / nj); }
                }
                for (int j = 0; j < kk; j++) {
                    Lobe L; L.bc = cen[j]; L.ax = nrmz(cen[j] - C); lobeFrame(L);
                    std::vector<float> pp; std::vector<V3> mine;
                    for (size_t i = 0; i < pts.size(); i++) if (lab[i] == j) mine.push_back(pts[i]);
                    for (auto& v : mine) { V3 q = v - L.bc; float a = dot(q, L.ax); pp.push_back(len(q - L.ax * a)); }
                    std::sort(pp.begin(), pp.end());
                    float r = pp.empty() ? 0.025f : pp[pp.size() * 7 / 10];
                    lobeFromCards(L, 0.0f, r, 0.0f);
                    L.w = std::max(0.2f, mine.size() / float(pts.size()) * kk);
                    L.turns = c.value("turns", 2.0f);
                    lobes.push_back(L);
                }
            }
        }
        bool multi = !lobes.empty();
        if (!multi) { Lobe L{bc, ax, B, e1, e2, rad, depth, 0.0f, 1.0f, c.value("turns", 2.0f)}; lobes.push_back(L); }
        V3 fwd = nrmz(V3{-ax.x, -ax.y, 0});
        // overall frame for the wrap band (mean of the lobe centres)
        V3 axAll = ax, BAll = B;
        if (multi) {
            V3 m{0, 0, 0}; for (auto& L : lobes) m = m + L.bc; m = m * (1.0f / lobes.size());
            axAll = nrmz(m - C); BAll = C + axAll * surf(axAll);
            fwd = nrmz(V3{-axAll.x, -axAll.y, 0});
        }
        V3 we1 = nrmz(cross(axAll, std::fabs(axAll.z) < 0.9f ? V3{0, 0, 1} : V3{1, 0, 0})), we2 = cross(axAll, we1);
        auto lobeOf = [&](V3 R0) {
            int bi = 0; float bd = 1e9f;
            for (size_t j = 0; j < lobes.size(); j++) { float dd = len(R0 - (lobes[j].B + lobes[j].ax * lobes[j].lift)) / lobes[j].w; if (dd < bd) { bd = dd; bi = (int)j; } }
            return bi;
        };
        // deterministic per-root hash in [0,1) (wrap / loose picks do not depend on the guide order or the rnd sequence)
        auto rootHash = [](V3 p, unsigned salt) {
            unsigned h = (unsigned)(int)std::lround(p.x * 1e4f) * 73856093u ^ (unsigned)(int)std::lround(p.y * 1e4f) * 19349663u ^ (unsigned)(int)std::lround(p.z * 1e4f) * 83492791u ^ salt * 2654435761u;
            h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            return (h >> 8) / float(1 << 24);
        };
        float wrapFrac = std::clamp(c.value("wrap_frac", 0.0f), 0.0f, 1.0f), looseFrac = std::clamp(c.value("loose_frac", 0.0f), 0.0f, 1.0f);
        auto isWrap = [&](V3 R0) { return wrapFrac > 0 && rootHash(R0, 7u) < wrapFrac; };
        auto isLoose = [&](V3 R0) { return looseFrac > 0 && !isWrap(R0) && rootHash(R0, 11u) < looseFrac; };
        // wrap band ring: around the base of all lobes (radius encloses them), at the lowest lobe lift + wrap_height_mm
        float wrapR = 0, wrapH = 1e9f;
        for (auto& L : lobes) {
            V3 q = (L.B + L.ax * L.lift) - BAll; q = q - axAll * dot(q, axAll);
            wrapR = std::max(wrapR, len(q) + L.rad * 0.8f);
            wrapH = std::min(wrapH, dot(L.B + L.ax * L.lift - BAll, axAll));
        }
        wrapR = c.value("wrap_mm", wrapR * 1000) / 1000 * c.value("wrap_scale", 1.0f);
        wrapH = std::max(0.0f, wrapH) + c.value("wrap_height_mm", 8.0f) / 1000;
        // ---- params
        float turns = c.value("turns", 2.0f), keepFront = c.value("keep_front_mm", 35.0f) / 1000;
        float stand = c.value("standoff_mm", 4.0f) / 1000, tuck = std::clamp(c.value("tuck", 0.25f), 0.0f, 1.0f);
        float wind = c.value("direction", 1.0f) >= 0 ? 1.0f : -1.0f;
        float arcUp = c.value("arc_up", 0.6f), jit = c.value("messy", 0.0f);
        float core = std::clamp(c.value("core", 0.0f), 0.0f, 1.0f), crossDeg = c.value("cross_deg", 30.0f), counter = c.value("counter_frac", 0.25f);
        float crossFrac = std::clamp(c.value("cross_frac", 0.35f * core), 0.0f, 1.0f);
        float smooth = std::clamp(c.value("smooth", 0.0f), 0.0f, 1.0f), smoothLayer = std::clamp(c.value("smooth_layer", 0.15f), 0.0f, 0.45f);
        float sweep = c.value("smooth_sweep", 0.0f);
        float smoothTop = c.value("smooth_top", 0.85f), tuckR = std::clamp(c.value("tuck_r", 0.45f), 0.0f, 1.0f);
        // keep_side: side locks rooted low in front of the ears stay as they are (Ciri's temple locks)
        bool keepSide = c.contains("keep_side_z_mm");
        float keepSideZ = c.value("keep_side_z_mm", 0.0f) / 1000, keepSideF = c.value("keep_side_fwd_mm", 0.0f) / 1000;
        bool dry = c.value("dry_run", false) || c.value("detect_only", false);
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        int id = 0, grp = 1000 + (++grpId), n = 0, kept = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops; std::vector<int> bunG, keptG;
        unsigned seedR = 12345u;
        auto rnd = [&]() { seedR = seedR * 1664525u + 1013904223u; return (seedR >> 8) / float(1 << 24); };
        const int K = groom.K;
        auto keptRoot = [&](V3 R0) { return dot(R0 - C, fwd) > keepFront || (keepSide && dot(R0 - C, fwd) > keepSideF && R0.z - C.z < keepSideZ); };
        // the strand of a gathered root: along the scalp to the bun base, then wound around the bun axis
        auto bunPath = [&](V3 R0) -> Guide {
            const Lobe& LB = lobes[lobeOf(R0)];
            const V3 ax = LB.ax, e1 = LB.e1, e2 = LB.e2, B = LB.B + LB.ax * LB.lift;   // lobe-local frame (lift 0 = old)
            const float rad = LB.rad, depth = LB.depth, turns = LB.turns;
            bool wrapS = isWrap(R0), looseS = isLoose(R0);
            // entry angle = the root's own side around the bun axis (hair arrives from its side)
            V3 q = R0 - B; q = q - ax * dot(q, ax);
            // smooth: the side seen from the head centre (roots right under a raised lobe sit near its axis, where the
            // angle around B jumps between neighbours = crossing helices that HairWorks blends into loops)
            if (smooth > 0) { q = R0 - C; q = q - ax * dot(q, ax); }
            float phi = std::atan2(dot(q, e2), dot(q, e1));
            if (smooth > 0 && sweep > 0) {
                // sweep (opt-in smooth_sweep, rad): the axis of a raised lobe meets the scalp right under it, so roots
                // all round that point start their helix at every angle (a vortex; HairWorks interpolates the hair
                // between such guides into flat tangled ribbons). Here every strand enters from the neck side of the
                // lobe, spread by +-sweep with its left / right position: parallel wraps like a French twist.
                V3 dn = V3{0, 0, -1} - ax * dot(V3{0, 0, -1}, ax);
                float side = std::clamp((R0.x - C.x) / 0.07f, -1.0f, 1.0f);
                phi = std::atan2(dot(dn, e2), dot(dn, e1)) + wind * sweep * side;
            }
            float layer = 0.55f + 0.45f * rnd();
            V3 E = B + ax * (0.004f + stand) + (e1 * std::cos(phi) + e2 * std::sin(phi)) * (rad * 0.95f);
            float wphi = 0;
            if (wrapS) {
                V3 qw = R0 - BAll; qw = qw - axAll * dot(qw, axAll);
                wphi = std::atan2(dot(qw, we2), dot(qw, we1));
                E = BAll + axAll * wrapH + (we1 * std::cos(wphi) + we2 * std::sin(wphi)) * wrapR;
            }
            // scalp path: slerp of directions from the root to the entry point, at skull radius + standoff
            std::vector<V3> pl;
            V3 u0 = nrmz(R0 - C), u1 = nrmz(E - C);
            // arc_up: the path bends over the top (side roots go above / behind the ears, not across them)
            V3 um = nrmz(u0 + u1 + V3{0, 0, (arcUp + 3.0f * arcUp * std::max(0.0f, 0.35f - u0.z)) * (1.0f - std::max(0.0f, dot(u0, ax)))});   // low (ear-level) roots arc higher
            auto slerp = [](V3 a, V3 b, float t) {
                float o = std::acos(std::clamp(dot(a, b), -1.0f, 1.0f));
                return o < 1e-3f ? b : nrmz(a * (std::sin((1 - t) * o) / std::sin(o)) + b * (std::sin(t * o) / std::sin(o)));
            };
            float om = std::acos(std::clamp(dot(u0, um), -1.0f, 1.0f)) + std::acos(std::clamp(dot(um, u1), -1.0f, 1.0f));
            int ns = std::max(2, int(om / 0.12f));
            pl.push_back(R0);
            for (int s = 1; s <= ns; s++) {
                float t = s / float(ns);
                V3 u = t < 0.5f ? slerp(u0, um, t * 2) : slerp(um, u1, t * 2 - 1);
                float rr = surf(u) + stand * std::min(1.0f, t * 4);
                pl.push_back(C + u * rr);
            }
            size_t coilStart = pl.size();
            if (wrapS) {
                // wrap band (opt-in wrap_frac): the strand winds wrap_turns around the base of the whole bun like a braid
                // laid round it (3 phase groups: radial / axial zig-zag wrap_braid_mm, 3 crossings per turn), then its
                // end dives into the core of its lobe
                float wt = c.value("wrap_turns", 1.2f), bamp = c.value("wrap_braid_mm", 4.0f) / 1000, wwid = c.value("wrap_width_mm", 10.0f) / 1000;
                float grpPh = 2.0943951f * std::floor(rootHash(R0, 13u) * 3), hoff = wwid * (rootHash(R0, 17u) - 0.5f);
                float wd = rootHash(R0, 19u) < counter ? -wind : wind;
                int nw = std::max(10, int(wt * 20));
                for (int s = 1; s <= nw; s++) {
                    float t = s / float(nw), an = wphi + wd * 6.2831853f * wt * t;
                    float zz = std::sin(an * 3 + grpPh);
                    float rr = wrapR + bamp * zz, aa = wrapH + hoff + bamp * std::cos(an * 3 + grpPh);
                    pl.push_back(BAll + axAll * aa + (we1 * std::cos(an) + we2 * std::sin(an)) * rr);
                }
                V3 M = B + ax * (depth * 0.45f);
                pl.push_back(pl.back() * 0.5f + M * 0.5f);
                pl.push_back(M);
            }
            // coil: angle phi -> phi + wind * 2 pi * turns, radius rad * layer shrinking, rising out, end tucked inward
            int nc = wrapS ? 0 : std::max(8, int(turns * 16));
            if (core > 0 && !wrapS) {
                // full core (2026-10-12, opt-in "core" 0..1): the old coil kept every strand at 30-100 % of the radius with
                // its end highest = a ring around a dark funnel seen from behind (ciri_bun_1). Here each strand spirals
                // in to its OWN end radius (fr, biased to the axis: core^2 of the strands end inside the inner third),
                // the height follows a dome (highest at the axis), the last part dips into the core; every wrap is tilted
                // by a random rotation about the bun's mid point (cross_deg) and counter_frac of them wind the other way,
                // so wraps cross like the vanilla messy bun instead of lying in neat rings.
                float u1r = rnd(), u2r = rnd(), u3r = rnd(), u4r = rnd();
                float fr = std::pow(u1r, 1.0f + 2.5f * core) * 0.85f;            // end radius fraction (small = core)
                float tn = turns * (0.65f + 0.7f * u2r);                          // own number of turns
                float wd = (u3r < counter) ? -wind : wind;
                float r0 = rad * (0.75f + 0.25f * layer);
                V3 M = B + ax * (depth * 0.45f);
                float tilt = crossDeg * 3.14159265f / 180 * (2 * u4r - 1);
                float ta = 6.2831853f * rnd();
                V3 tax = e1 * std::cos(ta) + e2 * std::sin(ta);
                int ncc = std::max(10, int(tn * 18));
                // cross-over (cross_frac of the strands): first straight over the dome top to the far rim, then wind from
                // there. Strand ENDS at the axis alone left the centre dark (HairWorks thins the tips): the full-width
                // middle of these strands covers the core, and they cross the wraps like the vanilla messy bun
                float ph0 = phi;
                if (rnd() < crossFrac) {
                    float side = 0.35f * (rnd() - 0.5f);
                    V3 sd = e1 * std::cos(phi + 1.5708f) + e2 * std::sin(phi + 1.5708f);
                    pl.push_back(B + ax * (depth * 0.75f) + sd * (r0 * side) + (e1 * std::cos(phi) + e2 * std::sin(phi)) * (r0 * 0.45f));
                    pl.push_back(B + ax * (depth * 1.0f) + sd * (r0 * side));
                    pl.push_back(B + ax * (depth * 0.75f) + sd * (r0 * side) - (e1 * std::cos(phi) + e2 * std::sin(phi)) * (r0 * 0.45f));
                    ph0 = phi + 3.14159265f;
                }
                for (int s = 1; s <= ncc; s++) {
                    float t = s / float(ncc);
                    float an = ph0 + wd * 6.2831853f * tn * t;
                    float x = fr + (1.0f - fr) * std::pow(1.0f - t, 0.8f);         // radius fraction, spiralling in
                    float rr = r0 * x;
                    float dome = 0.2f + 0.8f * std::sqrt(std::max(0.0f, 1.0f - x * x));
                    float aa = depth * dome * (t > 0.85f ? 1.0f - (t - 0.85f) * 2.0f * core : 1.0f);   // end dips into the core
                    if (jit > 0) { rr *= 1.0f + jit * 0.5f * (rnd() - 0.5f); aa += depth * jit * 0.25f * (rnd() - 0.5f); }
                    V3 P = B + ax * aa + (e1 * std::cos(an) + e2 * std::sin(an)) * rr;
                    P = M + rotAxis(P - M, tax, tilt * std::min(1.0f, t * 3));    // tilt grows in over the first wrap
                    pl.push_back(P);
                }
                nc = 0;
            }
            if (smooth > 0 && core <= 0 && !wrapS) {
                // smooth twist (2026-10-12, opt-in "smooth" 0..1): vanilla's sculpted crest (Anna) is a twisted roll of
                // PARALLEL strands. Each strand winds a helix at a near-constant radius on the lobe surface (layer
                // spread only smooth_layer), same hand and pitch for all, phase = its entry side, so neighbouring roots
                // give neighbouring parallel helices; the end eases in to tuck_r x radius over the last 25 % (the old
                // coil shrank sharply at t > 0.9 = a tight kinked spiral that HairWorks drew as frizz).
                float lay = 1.0f - smoothLayer * (1.0f - layer) / 0.45f;
                float a0 = depth * 0.1f, a1 = depth * smoothTop;
                int nh = std::max(12, int(turns * 24));
                for (int s = 1; s <= nh; s++) {
                    float t = s / float(nh);
                    float an = phi + wind * 6.2831853f * turns * t;
                    float ease = t > 0.75f ? (t - 0.75f) / 0.25f : 0.0f;
                    float rr = rad * lay * (1.0f - (1.0f - tuckR) * ease * ease);
                    float aa = a0 + (a1 - a0) * t;
                    pl.push_back(B + ax * aa + (e1 * std::cos(an) + e2 * std::sin(an)) * rr);
                }
                nc = 0;
            }
            for (int s = 1; s <= nc; s++) {
                float t = s / float(nc);
                float an = phi + wind * 6.2831853f * turns * t;
                float rr = rad * layer * (1.0f - (1.0f - tuck) * 0.6f * t);
                float aa = depth * (0.15f + 0.65f * std::sin(t * 3.14159f * 0.5f) * layer);
                if (t > 0.9f) rr *= 1.0f - (t - 0.9f) * 10 * (1 - tuck);
                if (jit > 0) { rr *= 1.0f + jit * (rnd() - 0.5f); aa += depth * jit * 0.3f * (rnd() - 0.5f); }
                pl.push_back(B + ax * aa + (e1 * std::cos(an) + e2 * std::sin(an)) * rr);
            }
            if (looseS && pl.size() > coilStart + 4) {
                // loose strand (opt-in loose_frac): the coil is cut after (1 - loose_len) of it and the rest of its length
                // leaves the bun as a wisp: outward from the lobe axis, drooping (loose_droop) with a little wiggle
                float ll = std::clamp(c.value("loose_len", 0.35f), 0.05f, 0.9f), droop = c.value("loose_droop", 0.6f);
                size_t nCoil = pl.size() - coilStart, keep = coilStart + std::max<size_t>(2, size_t(nCoil * (1 - ll)));
                float Lr = 0;
                for (size_t k = keep; k < pl.size(); k++) Lr += len(pl[k] - pl[k - 1]);
                Lr *= c.value("loose_length_scale", 0.6f);
                pl.resize(keep);
                V3 P = pl.back(), qq = P - B; qq = qq - ax * dot(qq, ax);
                float h1 = rootHash(R0, 23u), h2 = rootHash(R0, 29u);
                V3 dir = nrmz(nrmz(qq) + ax * 0.3f + (e1 * (h1 - 0.5f) + e2 * (h2 - 0.5f)) * 0.8f);
                int nt = 8;
                for (int s = 1; s <= nt; s++) {
                    float w = std::sin(s * 1.3f + h1 * 6.28f) * 0.25f;
                    dir = nrmz(dir + V3{0, 0, -droop / nt * 2} + (e1 * w + e2 * (-w)) * 0.3f);
                    P = P + dir * (Lr / nt);
                    pl.push_back(P);
                }
            }
            if (smooth > 0 && pl.size() > 4) {
                // smooth: Laplacian passes on the path (root fixed) remove the corner where the scalp path meets the
                // raised lobe base and any kink before the K-point resample
                int passes = int(std::round(smooth * 8));
                for (int it = 0; it < passes; it++) {
                    std::vector<V3> q2 = pl;
                    for (size_t k = 1; k + 1 < pl.size(); k++) {
                        q2[k] = pl[k] * 0.5f + (pl[k - 1] + pl[k + 1]) * 0.25f;
                        // keep the smoothed scalp path over the skull (a later pushOut per point would kink it)
                        V3 u = nrmz(q2[k] - C);
                        float rmin = surf(u) + stand * std::min(1.0f, k / 3.0f);
                        if (len(q2[k] - C) < rmin) q2[k] = C + u * rmin;
                    }
                    pl = q2;
                }
            }
            Guide Q(K);
            auto res = rebuild::resampleN(pl, K);
            for (int k = 0; k < K; k++) Q[k] = res[k];
            Q[0] = R0;
            pushOut(hr, Q, 0.003f);
            return Q;
        };
        // densify_only: the guides already form the bun (an earlier make_bun): only the fill pass below runs
        bool densOnly = c.value("densify_only", false), fill = c.value("fill", densOnly);
        for (int g : ix) {
            V3 R0 = groom.P[g][0];
            if (keptRoot(R0)) { keptG.push_back(g); kept++; continue; }
            bunG.push_back(g);
            n++;
            if (densOnly) continue;
            Guide Q = bunPath(R0);
            ops.push_back({{"op", "reshape"}, {"root", {r4(R0.x), r4(R0.y), r4(R0.z)}}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
        }
        r["bun"] = {{"centre", v3j(bc)}, {"axis", v3j(ax)}, {"base", v3j(B)}, {"radius_mm", std::round(rad * 10000) / 10},
                    {"depth_mm", std::round(depth * 10000) / 10}, {"card_points", bunV.size()}};
        if (multi) {
            json lj = json::array();
            std::vector<int> cnt(lobes.size(), 0);
            for (int g : bunG) cnt[lobeOf(groom.P[g][0])]++;
            for (size_t j = 0; j < lobes.size(); j++) {
                auto& L = lobes[j];
                lj.push_back({{"centre", v3j(L.bc)}, {"axis", v3j(L.ax)}, {"base", v3j(L.B + L.ax * L.lift)}, {"radius_mm", std::round(L.rad * 10000) / 10},
                              {"depth_mm", std::round(L.depth * 10000) / 10}, {"lift_mm", std::round(L.lift * 10000) / 10}, {"weight", L.w}, {"turns", L.turns}, {"guides", cnt[j]}});
            }
            r["lobes"] = lj;
        }
        if (wrapFrac > 0 || looseFrac > 0) {
            int nw = 0, nl = 0;
            for (int g : bunG) { nw += isWrap(groom.P[g][0]); nl += isLoose(groom.P[g][0]); }
            r["wrap"] = {{"guides", nw}, {"radius_mm", std::round(wrapR * 10000) / 10}, {"height_mm", std::round(wrapH * 10000) / 10}};
            r["loose_guides"] = nl;
        }
        r["gathered"] = n; r["kept_front"] = kept;
        if (dry) return r;
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged();
        // fill (2026-10-12): thin crown / sides after gathering. Skin points of the gathered region (head vertices on the
        // vanilla card footprint: within fill_reach_mm of a card vertex, not on the ears, not under the bun, above head
        // centre - fill_z_below_mm) get the lateral gap to the nearest strand point lying on them; the widest gap gets a
        // new guide (duplicate of the nearest gathered guide, its own bun path from there, edge faces), gaps update, and
        // so on while a gap is wider than cover_mm, up to fill_max_new, within max_ratio of the hair budget.
        std::vector<int> newG;
        if (fill && (!bunG.empty() || c.value("fill_any_template", false))) {
            t6::HeadSkin hs = t6::headSkin(*this);
            t6::Zones zz = t6::zonesOf(*this, c);
            float reachF = c.value("fill_reach_mm", 20.0f) / 1000, coverF = c.value("cover_mm", 7.0f) / 1000, maxH = stand + 0.012f;
            float zBelow = c.value("fill_z_below_mm", 60.0f) / 1000, spacingF = c.value("fill_spacing_mm", 8.0f) / 1000;
            float maxRatio = c.value("max_ratio", 1.2f);
            int maxNew = c.value("fill_max_new", 60);
            rebuild::Grid cvg(0.01f);
            for (size_t i = 0; i < cardV.size(); i++) cvg.add(cardV[i], (int)i);
            std::vector<V3> cloud;
            for (size_t g = 0; g < groom.P.size(); g++) if (groom.alive[g]) t6::addPoly(cloud, groom.P[g]);
            rebuild::Grid clg(0.01f);
            for (size_t i = 0; i < cloud.size(); i++) clg.add(cloud[i], (int)i);
            std::vector<V3> sx, sn;
            std::vector<float> gapv;
            if (hs.m) for (size_t i = 0; i < hs.m->v.size(); i++) {
                V3 v = hs.m->v[i];
                if (v.z < C.z - zBelow || keptRoot(v) || zz.inEar(v, 0.006f) || zz.overFace(v)) continue;
                bool under = false;
                for (auto& L : lobes) if (len(v - L.B) < L.rad * 0.9f) under = true;
                if (under) continue;
                bool foot = false;
                cvg.around(v, reachF, [&](int q) { if (!foot && len(cardV[q] - v) < reachF) foot = true; });
                if (!foot) continue;
                sx.push_back(v); sn.push_back(hs.n[i]);
                gapv.push_back(t6::hairGap(cloud, clg, v, hs.n[i], maxH, 0.04f));
            }
            float gap0 = 0;
            for (float gv : gapv) gap0 = std::max(gap0, gv);
            int planted = 0;
            std::vector<json> fops;
            std::vector<V3> taken;
            for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
            // frontier order: among the gaps wider than cover_mm, the skin point NEAREST an existing / planted root (but at
            // least fill_spacing_mm from it) goes first, so the planting grows outward ring by ring and each new root can
            // fan growth faces onto the previous ring (a root far outside the growth mesh gets no face = no hair)
            std::vector<V3> roots;
            for (size_t g = 0; g < groom.P.size(); g++) if (groom.alive[g]) roots.push_back(groom.P[g][0]);
            std::vector<float> dRoot(sx.size(), 1e9f);
            for (size_t i = 0; i < sx.size(); i++) for (auto& q : roots) dRoot[i] = std::min(dRoot[i], len(q - sx[i]));
            int onEar = 0, inHead = 0;
            json euv = t6::typicalUv(*this);
            if (euv.is_null()) euv = "src";
            while (planted < maxNew) {
                int bi = -1; float bs = 1e9f;
                for (size_t i = 0; i < sx.size(); i++)
                    if (gapv[i] > coverF && dRoot[i] >= spacingF && dRoot[i] < bs) { bs = dRoot[i]; bi = (int)i; }
                if (bi < 0) break;
                V3 X = sx[bi];
                gapv[bi] = 0;
                int src = -1; float bd = 1e9f;
                for (int g : bunG) { float dd = len(groom.P[g][0] - X); if (dd < bd) { bd = dd; src = g; } }
                if (src < 0)   // fill_any_template (nothing gathered: plant a whole new bun on a bare crown, Anna)
                    for (size_t g = 0; g < groom.P.size(); g++) if (groom.alive[g]) { float dd = len(groom.P[g][0] - X); if (dd < bd) { bd = dd; src = (int)g; } }
                if (src < 0) break;
                Guide Q = bunPath(X);
                bool ear = false;
                for (auto& p : Q) if (zz.inEar(p, c.value("ear_pad_mm", 8.0f) / 1000)) { ear = true; break; }
                if (ear) { onEar++; continue; }   // its path would cross the ear
                // long scalp paths (front roots) resampled to K points: the chords between them cut through the skull, the
                // hair dips in and out of the head = a flat blue-grey sheet on Ciri's crown. Reject them.
                bool chord = false;
                for (size_t k = 1; k < Q.size() && !chord && hr.ok; k++) {
                    V3 m = (Q[k] + Q[k - 1]) * 0.5f, dm = m - C;
                    float rm = len(dm), sf = rm > 1e-4f && m.z > C.z - 0.01f ? hr.surface(dm * (1.0f / rm)) : -1;
                    if (sf > 0 && rm < sf - c.value("chord_mm", 3.0f) / 1000) chord = true;
                }
                if (chord) { inHead++; continue; }
                for (size_t i = 0; i < sx.size(); i++) dRoot[i] = std::min(dRoot[i], len(X - sx[i]));
                V3 rs = groom.P[src][0], dd = X - rs;
                fops.push_back({{"op", "duplicate"}, {"root", {r4(rs.x), r4(rs.y), r4(rs.z)}}, {"d", {r4(dd.x), r4(dd.y), r4(dd.z)}}, {"edge_face", true}, {"edge_uv", euv}, {"id", ++id}, {"grp", grp}});
                fops.push_back({{"op", "reshape"}, {"root", {r4(X.x), r4(X.y), r4(X.z)}}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
                taken.push_back(X);
                planted++;
                std::vector<V3> np;
                t6::addPoly(np, Q);
                for (size_t i = 0; i < sx.size(); i++) {
                    if (gapv[i] <= coverF) continue;
                    for (auto& p : np) {
                        V3 d = p - sx[i];
                        float h = dot(d, sn[i]);
                        if (h < -0.003f || h > maxH) continue;
                        gapv[i] = std::min(gapv[i], len(d - sn[i] * h));
                    }
                }
            }
            size_t n0 = groom.P.size();
            for (auto& o : fops) groom.ops.push_back(o);
            groom.replay(); editsChanged();
            int dropped = t6::dropForBudget(*this, planted, 2, maxRatio, c.value("ignore_budget", false));
            for (size_t g = n0; g < groom.P.size(); g++) if (groom.alive[g]) newG.push_back((int)g);
            float gap1 = 0;
            for (float gv : gapv) gap1 = std::max(gap1, gv);
            r["fill"] = {{"skin_points", sx.size()}, {"planted", planted}, {"dropped_for_budget", dropped}, {"widest_gap_mm_before", std::round(gap0 * 10000) / 10},
                         {"widest_gap_mm_after_plan", std::round(gap1 * 10000) / 10}, {"rejected_path_over_ear", onEar}, {"rejected_path_through_head", inHead},
                         {"planted_without_growth_face", t6::faceless(groom, n0)}, {"edge_uv", euv}};
        }
        if (c.value("hold", true)) {
            float keepS = c.value("keep_s", 0.6f);
            smap.ensure(groom.P.size());
            for (int g : newG) bunG.push_back(g);
            float looseS = c.value("loose_s", 0.45f);
            for (int g : bunG) { smap.guideS[g] = isLoose(groom.P[g][0]) ? looseS : 1.0f; smap.guideR[g] = 1.0f; }
            for (int g : keptG) smap.guideS[g] = keepS;
            stiffDirty = true;
            look.setF("physicalMaterials.stiffness.stiffness", 1.0f);
            look.setF("physicalMaterials.stiffness.stiffnessStrength", 1.0f);
            look.setF("physicalMaterials.stiffness.rootStiffness", 1.0f);
            look.setF("physicalMaterials.stiffness.tipStiffness", c.value("tip_stiffness", 0.8f));
            look.setF("physicalMaterials.stiffness.bendStiffness", 1.0f);
            json one = {{"X", 1.0}, {"Y", 1.0}, {"Z", 1.0}, {"W", 1.0}};
            look.setJson("physicalMaterials.stiffness.stiffnessCurve", one);
            look.setJson("physicalMaterials.stiffness.stiffnessStrengthCurve", one);
            look.setJson("physicalMaterials.stiffness.bendStiffnessCurve", one);
            look.enforceCaps(); computeBudget();
            r["hold"] = {{"bun_guides_s_r", 1.0}, {"kept_guides_s", keepS}, {"global", "stiffness 1, strength 1, root 1, bend 1, flat curves"}};
        }
        settleRest(*this, 0.5);
        r["status"] = status_();
        return r;
    }
    if (d == "conform_to_cards") {
        // drastic reshape toward vanilla: every point (past the root part) is pulled onto the game's own hair-card
        // surface (nearest card vertex, lifted outward by offset_mm). Kills waves / flips / volume the vanilla style
        // does not have (hair behind the ears sticking up, crests, bobs). strength 0..1; segment lengths are kept.
        std::vector<V3> cardV;
        for (auto& cm : pack.meshes) if (cm.kind == "hair_cards") for (auto& v : cm.v) cardV.push_back(v);
        if (cardV.empty()) { r["error"] = "this pack has no vanilla hair cards (re-run desk_prep)"; return r; }
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        float st = std::clamp(c.value("strength", 0.8f), 0.0f, 1.0f), off = c.value("offset_mm", 2.0f) / 1000;
        bool centre = c.value("mode", std::string("surface")) == "centreline";
        float inner = std::clamp(c.value("inner_lift", 0.15f), 0.0f, 1.0f);
        float tol = c.value("tol_mm", 4.0f) / 1000, rootPart = std::clamp(c.value("root_part", 0.15f), 0.0f, 0.9f);
        // coarse grid over card vertices (1 cm cells)
        const float cs = 0.01f;
        std::unordered_map<long long, std::vector<int>> grid;
        auto key = [&](int x, int y, int z) { return ((long long)(x + 4096) << 26) | ((long long)(y + 4096) << 13) | (long long)(z + 4096); };
        auto cell = [&](float f) { return (int)std::floor(f / cs); };
        for (int i = 0; i < (int)cardV.size(); i++) grid[key(cell(cardV[i].x), cell(cardV[i].y), cell(cardV[i].z))].push_back(i);
        auto nearest = [&](V3 p, float& bd) {
            int bi = -1; bd = 1e9f;
            int cx = cell(p.x), cy = cell(p.y), cz = cell(p.z);
            for (int R = 1; R <= 4 && bi < 0; R++)
                for (int x = cx - R; x <= cx + R; x++) for (int y = cy - R; y <= cy + R; y++) for (int z = cz - R; z <= cz + R; z++) {
                    auto it = grid.find(key(x, y, z));
                    if (it == grid.end()) continue;
                    for (int i : it->second) { float dd = len(cardV[i] - p); if (dd < bd) { bd = dd; bi = i; } }
                }
            return bi;
        };
        int id = 0, grp = 1000 + (++grpId), n = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        HeadRadial hr = headRadial(*this);
        double moved = 0; int farOff = 0;
        for (int g : ix) {
            const Guide& G = groom.P[g];
            size_t K = G.size();
            if (K < 3) continue;
            auto sl = arcLen(G);
            float L = sl.back();
            if (L < 1e-4f) continue;
            // centreline mode (curls / rolls / waves): conform the heavily smoothed centre line, keep the residual
            Guide C0 = G;
            if (centre) for (int it = 0; it < 8; it++) { Guide T = C0; for (size_t k = 1; k + 1 < K; k++) T[k] = C0[k] * 0.4f + (C0[k - 1] + C0[k + 1]) * 0.3f; C0 = T; }
            const Guide& SRC = centre ? C0 : G;
            Guide Q = SRC;
            bool any = false;
            for (size_t k = 1; k < K; k++) {
                float u = sl[k] / L;
                float w = u <= rootPart ? 0.0f : std::min(1.0f, (u - rootPart) / std::max(rootPart, 0.1f));
                if (w <= 0) continue;
                float bd;
                int bi = nearest(SRC[k], bd);
                if (bi < 0) { farOff++; continue; }
                if (bd <= tol) continue;
                // smooth target: mean of the card vertices within 8 mm of the nearest one (one card vertex = lumpy)
                V3 acc{0, 0, 0}; int na = 0;
                int cx = cell(cardV[bi].x), cy = cell(cardV[bi].y), cz = cell(cardV[bi].z);
                for (int x = cx - 1; x <= cx + 1; x++) for (int y = cy - 1; y <= cy + 1; y++) for (int z = cz - 1; z <= cz + 1; z++) {
                    auto it = grid.find(key(x, y, z));
                    if (it == grid.end()) continue;
                    for (int i : it->second) if (len(cardV[i] - cardV[bi]) < 0.008f) { acc = acc + cardV[i]; na++; }
                }
                V3 cp = na ? acc * (1.0f / na) : cardV[bi];
                V3 out = nrmz(cp - pack.headC);
                V3 tgt = cp + out * off;
                // a point already INSIDE the card shell (closer to the head than the card surface) is hair lying on
                // the scalp under the cards: barely lift it (lifting crown hair to the card surface bares the scalp)
                float inside = len(SRC[k] - pack.headC) < len(tgt - pack.headC) ? inner : 1.0f;
                Q[k] = SRC[k] + (tgt - SRC[k]) * (st * w * inside);
                any = true;
            }
            if (!any) continue;
            if (centre) for (size_t k = 1; k < K; k++) Q[k] = Q[k] + (G[k] - C0[k]);   // curl back on
            // smooth along the strand (3 passes, root fixed) so neighbouring snaps do not zig-zag
            for (int it = 0; it < (centre ? 0 : 3); it++) {
                Guide T = Q;
                for (size_t k = 1; k + 1 < K; k++) T[k] = Q[k] * 0.5f + (Q[k - 1] + Q[k + 1]) * 0.25f;
                Q = T;
            }
            // keep each segment's length (re-walk along the new directions)
            Guide R2 = Q;
            for (size_t k = 1; k < K; k++) R2[k] = R2[k - 1] + nrmz(Q[k] - R2[k - 1]) * (sl[k] - sl[k - 1]);
            pushOut(hr, R2, 0.003f);
            for (size_t k = 0; k < K; k++) moved += len(R2[k] - G[k]);
            V3 r0 = G[0];
            ops.push_back({{"op", "reshape"}, {"root", {r4(r0.x), r4(r0.y), r4(r0.z)}}, {"points", ptsJson(R2)}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["changed"] = n; r["mean_point_move_mm"] = n ? std::round(moved / n * 1000 / 10 * 10) / 10 : 0;
        r["points_without_card_nearby"] = farOff; r["status"] = status_();
        return r;
    }
    if (d == "hug_cards") {
        // flatten volume the vanilla cards do not have (billows behind the ears / down the neck). The vanilla outline =
        // VISIBLE card hair (area samples of the card triangles where the texture alpha > alpha_vis; the card quads
        // themselves reach far past the painted hair). Seen from the head centre, the outer visible radius per
        // direction (shell_pct of the samples in a cone_deg cone) is a hard limit: every guide point past root_part is
        // clamped to at most max_mm outside it along its head radial; points in a direction with no vanilla hair at all
        // (hair on the neck under the ear, ends flaring sideways past the cards) are pulled to within max_mm +
        // footprint_mm of the nearest visible card hair (footprint: false = leave them). The last tip_part may stand
        // tip_mm more out (vanilla's flick). Constrained re-walk = segment lengths kept, spare length slides along the
        // shell. Points inside the shell are not moved, unless inside_mm > 0: points below head centre + inside_z_mm that
        // are farther than inside_mm from any visible card hair are pulled to it as well (hair hanging where vanilla
        // shows the neck / skin, e.g. behind the ear).
        std::vector<V3> cardV;
        {
            float alphaVis = c.value("alpha_vis", 0.3f);
            std::mt19937 rng(7);
            std::uniform_real_distribution<float> U01(0.0f, 1.0f);
            std::map<std::string, std::tuple<unsigned char*, int, int>> tex;
            for (auto& m : pack.meshes) {
                if (m.kind != "hair_cards" || m.t.empty()) continue;
                bool hasUv = m.uv.size() == m.v.size();
                if (hasUv && !tex.count(m.texFile)) { int w = 0, h = 0; unsigned char* im = m.texFile.empty() ? nullptr : loadImage(pack.dir + "\\" + m.texFile, &w, &h); tex[m.texFile] = {im, w, h}; }
                auto [im, tw, th] = hasUv ? tex[m.texFile] : std::tuple<unsigned char*, int, int>{nullptr, 0, 0};
                std::vector<double> acc; double tot = 0;
                for (auto& t : m.t) { tot += 0.5 * len(cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]])); acc.push_back(tot); }
                int ns = std::min(120000, std::max(2000, int(tot * 2e7)));
                for (int k = 0; k < ns && tot > 0; k++) {
                    size_t ti = std::lower_bound(acc.begin(), acc.end(), U01(rng) * tot) - acc.begin();
                    if (ti >= m.t.size()) ti = m.t.size() - 1;
                    auto& t = m.t[ti];
                    float uu = U01(rng), vv = U01(rng);
                    if (uu + vv > 1) { uu = 1 - uu; vv = 1 - vv; }
                    if (im) {
                        float tu = m.uv[t[0]][0] + (m.uv[t[1]][0] - m.uv[t[0]][0]) * uu + (m.uv[t[2]][0] - m.uv[t[0]][0]) * vv;
                        float tv = m.uv[t[0]][1] + (m.uv[t[1]][1] - m.uv[t[0]][1]) * uu + (m.uv[t[2]][1] - m.uv[t[0]][1]) * vv;
                        tu -= std::floor(tu); tv -= std::floor(tv);
                        int x = std::clamp(int(tu * tw), 0, tw - 1), y = std::clamp(int(tv * th), 0, th - 1);
                        if (im[(size_t(y) * tw + x) * 4 + 3] / 255.0f <= alphaVis) continue;
                    }
                    cardV.push_back(m.v[t[0]] + (m.v[t[1]] - m.v[t[0]]) * uu + (m.v[t[2]] - m.v[t[0]]) * vv);
                }
            }
            for (auto& [k, t] : tex) if (std::get<0>(t)) freeImage(std::get<0>(t));
        }
        if (cardV.empty()) { r["error"] = "this pack has no vanilla hair cards (re-run desk_prep)"; return r; }
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        float maxOut = c.value("max_mm", 2.0f) / 1000, tipMm = c.value("tip_mm", 6.0f) / 1000;
        float rootPart = std::clamp(c.value("root_part", 0.1f), 0.0f, 0.9f), tipPart = std::clamp(c.value("tip_part", 0.12f), 0.0f, 0.5f);
        float pct = std::clamp(c.value("shell_pct", 0.9f), 0.5f, 1.0f), coneDeg = c.value("cone_deg", 4.0f);
        float st = std::clamp(c.value("strength", 1.0f), 0.0f, 1.0f);
        bool foot = c.value("footprint", true);
        float footMm = c.value("footprint_mm", 4.0f) / 1000;
        float insideMm = c.value("inside_mm", 0.0f) / 1000, insideZ = c.value("inside_z_mm", 0.0f) / 1000;   // 0 = off
        // 1 cm grid over the visible card samples (nearest visible hair for points outside the footprint)
        const float cs = 0.01f;
        std::unordered_map<long long, std::vector<int>> grid;
        auto gkey = [&](int x, int y, int z) { return ((long long)(x + 4096) << 26) | ((long long)(y + 4096) << 13) | (long long)(z + 4096); };
        auto gcell = [&](float f) { return (int)std::floor(f / cs); };
        for (int i = 0; i < (int)cardV.size(); i++) grid[gkey(gcell(cardV[i].x), gcell(cardV[i].y), gcell(cardV[i].z))].push_back(i);
        auto nearestV = [&](V3 p, float& bd) {
            int bi = -1; bd = 1e9f;
            int cx = gcell(p.x), cy = gcell(p.y), cz = gcell(p.z);
            for (int R = 1; R <= 6 && bi < 0; R++)
                for (int x = cx - R; x <= cx + R; x++) for (int y = cy - R; y <= cy + R; y++) for (int z = cz - R; z <= cz + R; z++) {
                    if (std::max({std::abs(x - cx), std::abs(y - cy), std::abs(z - cz)}) < R - 1) continue;
                    auto it = grid.find(gkey(x, y, z));
                    if (it == grid.end()) continue;
                    for (int i : it->second) { float dd = len(cardV[i] - p); if (dd < bd) { bd = dd; bi = i; } }
                }
            return bi;
        };
        bool dry = c.value("dry_run", false) || c.value("list_only", false);
        const V3 hc = pack.headC;
        // direction bins (lat / lon, 1 degree) of the card vertices: radius lists
        const float PI = 3.14159265f, B = PI / 180;
        const int NL = 180, NO = 360;
        std::vector<std::vector<float>> bins(NL * NO);
        auto latlon = [&](V3 u, int& la, int& lo) {
            la = std::clamp(int((std::asin(std::clamp(u.z, -1.0f, 1.0f)) + PI / 2) / B), 0, NL - 1);
            lo = std::clamp(int((std::atan2(u.y, u.x) + PI) / B), 0, NO - 1);
        };
        for (auto& v : cardV) { V3 q = v - hc; float rl = len(q); if (rl < 1e-4f) continue; int la, lo; latlon(q * (1.0f / rl), la, lo); bins[la * NO + lo].push_back(rl); }
        std::unordered_map<long long, float> cache;
        auto shell = [&](V3 u) {   // card shell radius toward u, -1 when no card lies in that direction
            int la, lo; latlon(u, la, lo);
            long long kk = (long long)la * NO + lo;
            auto ci = cache.find(kk);
            if (ci != cache.end()) return ci->second;
            float cosA = std::cos(coneDeg * B);
            int dl = int(std::ceil(coneDeg)) + 1;
            std::vector<float> v;
            for (int a = std::max(0, la - dl); a <= std::min(NL - 1, la + dl); a++) {
                float lat = (a + 0.5f) * B - PI / 2;
                int dlo = std::min(NO / 2, int(std::ceil(dl / std::max(0.05f, std::cos(lat)))));
                for (int o = lo - dlo; o <= lo + dlo; o++) {
                    int oo = ((o % NO) + NO) % NO;
                    auto& bv = bins[a * NO + oo];
                    if (bv.empty()) continue;
                    float lon = (oo + 0.5f) * B - PI;
                    V3 bu{std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
                    if (dot(bu, u) < cosA) continue;
                    v.insert(v.end(), bv.begin(), bv.end());
                }
            }
            float s = -1;
            if (v.size() >= 3) {
                size_t m = std::min(v.size() - 1, size_t(pct * (v.size() - 1) + 0.5f));
                std::nth_element(v.begin(), v.begin() + m, v.end());
                s = v[m];
            }
            cache[kk] = s;
            return s;
        };
        HeadRadial hr = headRadial(*this);
        int id = 0, grp = 1000 + (++grpId), n = 0, clampedPts = 0, noShell = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        double outBefore = 0, outAfter = 0, maxBefore = 0, maxAfter = 0, moved = 0; int movedN = 0;
        for (int g : ix) {
            const Guide& G = groom.P[g];
            size_t K = G.size();
            if (K < 3) continue;
            auto sl = arcLen(G);
            float L = sl.back();
            if (L < 1e-4f) continue;
            auto allow = [&](size_t k) {   // allowed distance outside the shell at point k
                float u = sl[k] / L;
                float t = tipPart > 0 ? std::clamp((u - (1.0f - tipPart)) / tipPart, 0.0f, 1.0f) : 0.0f;
                return maxOut + tipMm * t;
            };
            V3 tgtN{0, 0, 0};
            auto excess = [&](V3 p, size_t k, float& s) {   // how far p stands outside the allowed shell (m), s = shell
                V3 q = p - hc; float rl = len(q);
                s = rl > 1e-4f ? shell(q * (1.0f / rl)) : -1;
                if (s > 0) {
                    float e = rl - (s + allow(k));
                    if (e > 0 || insideMm <= 0 || p.z > hc.z + insideZ) return e;
                    // inside the shell but nowhere near visible vanilla hair (hair on the neck where vanilla shows skin)
                    float bd; int bi = nearestV(p, bd);
                    if (bi < 0 || bd <= insideMm) return e;
                    tgtN = cardV[bi]; s = -1;
                    return bd - insideMm;
                }
                if (!foot) return -1.0f;
                float bd; int bi = nearestV(p, bd);   // no vanilla hair in this direction: distance to the nearest visible hair
                if (bi < 0) return -1.0f;
                tgtN = cardV[bi];
                return bd - (allow(k) + footMm);
            };
            auto clampP = [&](V3 p, size_t k, float w) {
                float s; float e = excess(p, k, s);
                if (e <= 0) return p;
                if (s > 0) { V3 q = p - hc; float rl = len(q); return hc + q * ((rl - e * w) / rl); }
                return p + nrmz(tgtN - p) * (e * w);
            };
            bool any = false;
            for (size_t k = 1; k < K; k++) {
                float s; float e = excess(G[k], k, s);
                if (s < 0) noShell++;
                if (e > 0 && sl[k] / L > rootPart) { any = true; outBefore += e; maxBefore = std::max(maxBefore, (double)e); }
            }
            if (!any) continue;
            // constrained walk: each segment keeps its length, its end is pulled back inside the allowed shell
            Guide R2 = G;
            for (size_t k = 1; k < K; k++) {
                float u = sl[k] / L;
                float w = u <= rootPart ? 0.0f : std::min(1.0f, (u - rootPart) / std::max(rootPart, 0.05f));
                float seg = sl[k] - sl[k - 1];
                V3 dir = nrmz(G[k] - G[k - 1]);
                V3 p = R2[k - 1] + dir * seg;
                if (w > 0) for (int it = 0; it < 4; it++) {
                    V3 q = clampP(p, k, w * st);
                    if (len(q - p) < 1e-5f) break;
                    clampedPts += it == 0;
                    p = R2[k - 1] + nrmz(q - R2[k - 1]) * seg;
                }
                R2[k] = p;
            }
            // light smoothing (root and tip fixed in place) then one more clamp: no kinks where the clamp starts
            for (int it = 0; it < 2; it++) {
                Guide T = R2;
                for (size_t k = 2; k + 1 < K; k++) T[k] = R2[k] * 0.5f + (R2[k - 1] + R2[k + 1]) * 0.25f;
                R2 = T;
            }
            for (size_t k = 1; k < K; k++) if (sl[k] / L > rootPart) R2[k] = clampP(R2[k], k, st);
            pushOut(hr, R2, 0.003f);
            for (size_t k = 1; k < K; k++) {
                float s; float e = excess(R2[k], k, s);
                if (e > 0 && sl[k] / L > rootPart) { outAfter += e; maxAfter = std::max(maxAfter, (double)e); }
                moved += len(R2[k] - G[k]); movedN++;
            }
            V3 r0 = G[0];
            ops.push_back({{"op", "reshape"}, {"root", {r4(r0.x), r4(r0.y), r4(r0.z)}}, {"points", ptsJson(R2)}, {"id", ++id}, {"grp", grp}});
            n++;
        }
        auto mm = [](double m) { return std::round(m * 10000) / 10; };
        r["changed"] = n; r["clamped_points"] = clampedPts; r["points_without_shell"] = noShell; r["visible_card_samples"] = cardV.size();
        r["excess_sum_mm_before"] = mm(outBefore); r["excess_sum_mm_after"] = mm(outAfter);
        r["excess_max_mm_before"] = mm(maxBefore); r["excess_max_mm_after"] = mm(maxAfter);
        r["mean_point_move_mm"] = movedN ? mm(moved / movedN) : 0;
        if (dry) { r["status"] = status_(); return r; }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["status"] = status_();
        return r;
    }
    if (d == "rebuild_from_cards") {
        bool hasCards = false;
        for (auto& m : pack.meshes) if (m.kind == "hair_cards") hasCards = true;
        if (!hasCards) { r["error"] = "this pack has no vanilla hair cards (re-run desk_prep)"; return r; }
        // rebase2 (2026-10-12, opt-in) zone keys: exclude_ears (roots off the ear boxes + ear_pad_mm; ear boxes as
        // route_behind_ears: ear_lat_mm / ear_top_mm / ear_box), ear_route behind (default) | drop | none for strands
        // crossing an ear, exclude_temples_mm (no roots in front of the ears below eye level + this, outside the eye
        // span: bare temples), scalp_grow_mm needs the front forehead zone
        rebuild::Hooks hk;
        bool useHk = false;
        t6::Zones zz = t6::zonesOf(*this, c);
        r3::earOverride(*this, zz, c);
        t6::HeadSkin hsk = t6::headSkin(*this);
        const float earPad = c.value("ear_pad_mm", 10.0f) / 1000, templeMm = c.value("exclude_temples_mm", 0.0f) / 1000;
        const float eyeHalf = zz.haveEye ? (zz.eHi.x - zz.eLo.x) / 2 : 0.045f;
        const bool exEars = c.value("exclude_ears", false);
        if (exEars || templeMm > 0) {
            useHk = true;
            hk.excludeRoot = [&, earPad, templeMm, eyeHalf, exEars](V3 p) {
                if (exEars && zz.inEar(p, earPad)) return true;
                if (templeMm > 0 && p.z < zz.eyeZ + templeMm && std::fabs(p.x - zz.C.x) > eyeHalf + 0.01f) {
                    int sd = p.x > zz.C.x ? 0 : 1;
                    float frontY = zz.hasEar[sd] ? (zz.fwd > 0 ? zz.earHi[sd].y : zz.earLo[sd].y) : zz.eyeY - zz.fwd * 0.07f;
                    if (zz.fwd * (p.y - frontY) > -0.005f) return true;
                }
                return false;
            };
        }
        if (exEars && c.value("ear_route", std::string("behind")) != "none") {
            useHk = true;
            const bool dropE = c.value("ear_route", std::string("behind")) == "drop";
            const float behind = c.value("behind_mm", 8.0f);
            hk.post = [&, earPad, dropE, behind](Guide& G) {
                if (!r3::routeBehindEars(zz, G, earPad, behind, hsk)) return 0;
                if (dropE && r3::crossesEar(zz, G, 0.0f)) return -1;
                return 1;
            };
        }
        if (exEars && c.value("ear_faces", true)) {   // growth faces whose interpolated hair would cross an ear go
            useHk = true;
            hk.faceBlocked = [&](const Guide& G) { return r3::crossesEar(zz, G, 0.0f); };
        }
        if (c.value("scalp_grow_mm", 0.0f) > 0) {
            useHk = true;
            hk.forehead = [&, eyeHalf](V3 p) { return zz.fwd * (p.y - zz.C.y) > 0 && std::fabs(p.x - zz.C.x) < eyeHalf + 0.015f; };
        }
        json res = rebuildFromCards(*this, c, grpId, useHk ? &hk : nullptr);
        if (useHk) {
            json eb = json::array();
            for (int sd = 0; sd < 2; sd++) if (zz.hasEar[sd]) eb.push_back({v3j(zz.earLo[sd]), v3j(zz.earHi[sd])});
            res["ear_boxes"] = eb;
        }
        if (res.contains("error")) { r["error"] = res["error"]; return r; }
        {   // community packs share this op parametrically (never its baked guides): remember how it was made
            for (size_t i = groom.ops.size(); i-- > 0;)
                if (groom.ops[i].value("op", std::string()) == "rebuild") {
                    json p = c;
                    p.erase("do");
                    groom.ops[i]["rebuild"] = {{"kind", "from_cards"}, {"params", p}, {"studio_version", GSD_VERSION}};
                    break;
                }
            groom.replay();
        }
        settleRest(*this, 0.5);
        r["result"] = res; r["status"] = status_();
        return r;
    }
    if (d == "keep_cards") {
        // 2026-10-12 opt-in PREVIEW of mixing cards + strands (Hjalmar's attached lock): the vanilla hair cards picked here
        // stay drawn together with the groom (render / compare / shots). Picks: `near` [x,y,z] (edit frame, e.g. a
        // studio_pick scalp_edit or a card point) + `radius_mm` (25); `by` "component" (default: every connected card
        // strip with a vertex in the sphere) or "triangle" (triangles whose centroid is in it); `z_max` / `z_min` (m,
        // absolute) limit the picked triangles; `add` (default false = replace the earlier picks); `clear` = off.
        // Nothing is written to the edits / project: the game build does it with the pipeline's split_cards key (mode
        // "boxes" / include points), which this reports as a hint.
        if (c.value("clear", false)) { keepCards.clear(); r["kept_triangles"] = 0; return r; }
        // uv_min / uv_max [u, v]: only triangles whose centroid uv (unwrapped, as in the mesh) lies in the box - card
        // elements with their own texture tile (Hjalmar's braid: u -7..-5); with a uv box `near` is optional
        bool uvBox = c.contains("uv_min") && c.contains("uv_max");
        if (!c.contains("near") && !uvBox) { r["error"] = "keep_cards needs near [x,y,z] or uv_min + uv_max (or clear: true)"; return r; }
        V3 p = c.contains("near") ? V3{c["near"][0].get<float>(), c["near"][1].get<float>(), c["near"][2].get<float>()} : V3{0, 0, 0};
        float rad = c.contains("near") ? c.value("radius_mm", 25.0f) / 1000 : 1e9f;
        float u0 = uvBox ? c["uv_min"][0].get<float>() : 0, v0 = uvBox ? c["uv_min"][1].get<float>() : 0;
        float u1 = uvBox ? c["uv_max"][0].get<float>() : 0, v1 = uvBox ? c["uv_max"][1].get<float>() : 0;
        bool byTri = c.value("by", std::string("component")) == "triangle";
        float zmax = c.value("z_max", 1e9f), zmin = c.value("z_min", -1e9f);
        if (!c.value("add", false)) keepCards.clear();
        int nComp = 0, nTri = 0, nMesh = 0;
        json comps = json::array();
        for (size_t mi = 0; mi < pack.meshes.size(); mi++) {
            const Mesh& m = pack.meshes[mi];
            if (m.kind != "hair_cards" || m.t.empty()) continue;
            nMesh++;
            std::vector<int> par(m.v.size());
            for (size_t i = 0; i < par.size(); i++) par[i] = int(i);
            std::function<int(int)> fd = [&](int x) { while (par[x] != x) x = par[x] = par[par[x]]; return x; };
            for (auto& t : m.t) { par[fd(t[1])] = fd(t[0]); par[fd(t[2])] = fd(t[0]); }
            auto cen = [&](const std::array<int, 3>& t) { return (m.v[t[0]] + m.v[t[1]] + m.v[t[2]]) * (1.0f / 3); };
            std::set<int> hit;   // picked component roots
            if (!byTri)
                for (size_t i = 0; i < m.v.size(); i++) if (len(m.v[i] - p) < rad) hit.insert(fd(int(i)));
            auto& mask = keepCards[int(mi)];
            mask.resize(m.t.size(), 0);
            std::map<int, std::pair<int, std::pair<V3, V3>>> info;   // component -> (tris, bbox)
            for (size_t ti = 0; ti < m.t.size(); ti++) {
                V3 q = cen(m.t[ti]);
                if (q.z > zmax || q.z < zmin) continue;
                if (uvBox) {
                    if (m.uv.size() != m.v.size()) continue;
                    const auto& t3 = m.t[ti];
                    float cu = (m.uv[t3[0]][0] + m.uv[t3[1]][0] + m.uv[t3[2]][0]) / 3, cv = (m.uv[t3[0]][1] + m.uv[t3[1]][1] + m.uv[t3[2]][1]) / 3;
                    if (cu < u0 || cu > u1 || cv < v0 || cv > v1) continue;
                }
                bool in = byTri || !c.contains("near") ? len(q - p) < rad : hit.count(fd(m.t[ti][0])) > 0;
                if (!in || mask[ti]) continue;
                mask[ti] = 1; nTri++;
                int k = fd(m.t[ti][0]);
                auto it = info.find(k);
                if (it == info.end()) info[k] = {1, {q, q}};
                else {
                    it->second.first++;
                    auto& b = it->second.second;
                    b.first = {std::min(b.first.x, q.x), std::min(b.first.y, q.y), std::min(b.first.z, q.z)};
                    b.second = {std::max(b.second.x, q.x), std::max(b.second.y, q.y), std::max(b.second.z, q.z)};
                }
            }
            for (auto& kv : info) {
                nComp++;
                if (comps.size() < 40)
                    comps.push_back({{"mesh", int(mi)}, {"tris", kv.second.first}, {"min", v3j(kv.second.second.first)},
                                     {"max", v3j(kv.second.second.second)}});
            }
            bool any = false;
            for (auto b : mask) any = any || b;
            if (!any) keepCards.erase(int(mi));
        }
        if (!nMesh) { r["error"] = "this pack has no vanilla hair cards (re-run desk_prep)"; return r; }
        int total = 0;
        for (auto& kv : keepCards) for (auto b : kv.second) total += b;
        r["picked_triangles"] = nTri; r["kept_triangles"] = total; r["components"] = comps; r["component_count"] = nComp;
        r["pipeline_hint"] = "game build: split_cards {mode: boxes, boxes: [{min, max}] in mm relative to the head centre, "
                             "by: component} around these components (hairpipe.py split_tri_mask); strands under the kept "
                             "cards can be deleted with studio_delete where.near";
        r["head_centre"] = v3j(pack.headC);
        return r;
    }
    if (d == "outside_vanilla") {
        // guides rooted where the game's own hair cards do not reach the head (farther than tol_mm from every card
        // vertex): hair on temples / cheeks / forehead that vanilla does not have. list, or delete (delete: true).
        std::vector<V3> cardV;
        for (auto& cm : pack.meshes) if (cm.kind == "hair_cards") for (auto& v : cm.v) cardV.push_back(v);
        if (cardV.empty()) { r["error"] = "this pack has no vanilla hair cards (re-run desk_prep)"; return r; }
        float tol = c.value("tol_mm", 15.0f) / 1000;
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        json l = json::array();
        std::vector<json> ops;
        int id = 0, grp = 1000 + (++grpId);
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        for (int g : ix) {
            V3 rt = groom.P[g][0];
            float bd = 1e9f;
            for (auto& cv : cardV) bd = std::min(bd, len(cv - rt));
            if (bd <= tol) continue;
            // raised cards (1-3 cm off the scalp): covered when a card vertex lies above this root along the head
            // radius (within tol sideways, up to 4 cm out)
            {
                V3 rr = rt - pack.headC; float rl = len(rr); V3 ru = rr * (1.0f / std::max(rl, 1e-6f));
                bool under = false;
                for (auto& cv : cardV) {
                    V3 cc = cv - pack.headC; float along = cc.x * ru.x + cc.y * ru.y + cc.z * ru.z - rl;
                    if (along < -0.004f || along > 0.04f) continue;
                    V3 side = cc - ru * (along + rl);
                    if (len(side) < tol) { under = true; break; }
                }
                if (under) continue;
            }
            if (l.size() < 100) l.push_back({{"i", g}, {"root", v3j(rt)}, {"to_cards_mm", std::round(bd * 10000) / 10}});
            ops.push_back({{"op", "delete"}, {"roots", json::array({{r4(rt.x), r4(rt.y), r4(rt.z)}})}, {"id", ++id}, {"grp", grp}});
        }
        r["count"] = ops.size(); r["guides"] = l;
        if (c.value("delete", false) && !ops.empty()) {
            for (auto& o : ops) groom.ops.push_back(o);
            groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
            r["deleted"] = ops.size();
        }
        r["status"] = status_();
        return r;
    }
    if (d == "feather_hairline") {
        // soften a hard hairline: growth faces at the open edge of the growth mesh (a face edge used by no other face)
        // inside "where" are removed: the outer row fully, the faces touching it with keep (0.5) of them kept, so the hair
        // density fades in instead of starting as a wall of roots. Guides stay. rows: 1 or 2.
        auto ix = selectGuides(c.value("where", json::object({{"front", true}})));
        std::set<int> in(ix.begin(), ix.end());
        std::map<std::pair<int, int>, int> ec;
        for (auto& F : groom.faces)
            for (int q = 0; q < 3; q++) { int a = F.g[q], b = F.g[(q + 1) % 3]; ec[{std::min(a, b), std::max(a, b)}]++; }
        std::set<int> edgeG;
        for (auto& [e, n] : ec) if (n == 1) { edgeG.insert(e.first); edgeG.insert(e.second); }
        int rows = c.value("rows", 2);
        float keep = std::clamp(c.value("keep", 0.5f), 0.0f, 1.0f);
        int id = 0, grp = 1000 + (++grpId), n1 = 0, n2 = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        std::set<int> row1;
        auto del = [&](const Face& F) {
            json rr = json::array();
            for (int q = 0; q < 3; q++) { V3 p = groom.P[F.g[q]][0]; rr.push_back({r4(p.x), r4(p.y), r4(p.z)}); }
            ops.push_back({{"op", "delete_face"}, {"roots", rr}, {"id", ++id}, {"grp", grp}});
        };
        for (size_t f = 0; f < groom.faces.size(); f++) {
            auto& F = groom.faces[f];
            bool inW = in.count(F.g[0]) && in.count(F.g[1]) && in.count(F.g[2]);
            if (!inW) continue;
            bool onEdge = false;
            for (int q = 0; q < 3; q++) { int a = F.g[q], b = F.g[(q + 1) % 3]; if (ec[{std::min(a, b), std::max(a, b)}] == 1) onEdge = true; }
            if (onEdge) { del(F); row1.insert(int(f)); n1++; }
        }
        if (rows >= 2) {
            unsigned h = 12345;
            for (size_t f = 0; f < groom.faces.size(); f++) {
                if (row1.count(int(f))) continue;
                auto& F = groom.faces[f];
                if (!(in.count(F.g[0]) && in.count(F.g[1]) && in.count(F.g[2]))) continue;
                if (!(edgeG.count(F.g[0]) || edgeG.count(F.g[1]) || edgeG.count(F.g[2]))) continue;
                h = h * 1103515245u + 12345u;
                if ((h >> 16) % 1000 >= unsigned(keep * 1000)) { del(F); n2++; }
            }
        }
        if (!c.value("list_only", false)) {
            for (auto& o : ops) groom.ops.push_back(o);
            groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        }
        r["outer_row_removed"] = n1; r["second_row_removed"] = n2; r["status"] = status_();
        return r;
    }
    if (d == "trim_growth") {
        // stray interpolated hair: growth faces whose surface lies on skin outside the painted scalp (centroid farther
        // than tol_mm past the hairline) and / or stretched faces (longest edge > max_edge_mm, or > edge_factor x median)
        // that bridge a gap (parting, hairline). Removed with delete_face ops (list_only: just report them).
        Hairline hl;
        bool haveHl = hairlineOf(*this, hl);
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        std::set<int> in(ix.begin(), ix.end());
        // default: divergent faces only (the in-between hair HairWorks fans across guides that go different ways)
        bool offScalp = c.value("off_scalp", false) && haveHl, stretched = c.value("stretched", false), divergent = c.value("divergent", true);
        float divDeg = c.value("div_deg", 50.0f), spreadK = c.value("tip_spread", 4.0f);
        int nDiv = 0;
        float tol = c.value("tol_mm", 4.0f) / 1000, maxEdge = c.value("max_edge_mm", 0.0f) / 1000, factor = c.value("edge_factor", 2.5f);
        std::vector<float> edges;
        for (auto& F : groom.faces)
            edges.push_back(std::max({len(groom.P[F.g[0]][0] - groom.P[F.g[1]][0]), len(groom.P[F.g[1]][0] - groom.P[F.g[2]][0]), len(groom.P[F.g[2]][0] - groom.P[F.g[0]][0])}));
        std::vector<float> es = edges;
        std::sort(es.begin(), es.end());
        float med = es.empty() ? 0.01f : es[es.size() / 2];
        if (maxEdge <= 0) maxEdge = med * factor;
        int id = 0, grp = 1000 + (++grpId);
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        json list = json::array();
        int nOff = 0, nStr = 0;
        for (size_t f = 0; f < groom.faces.size(); f++) {
            auto& F = groom.faces[f];
            if (!in.count(F.g[0]) && !in.count(F.g[1]) && !in.count(F.g[2])) continue;
            std::string why;
            if (offScalp) {
                V3 cen = (posedPoint(F.g[0], 0) + posedPoint(F.g[1], 0) + posedPoint(F.g[2], 0)) * (1.0f / 3);
                if (!onScalp(hl, cen)) {
                    float dd;
                    nearestHl(hl, cen, dd);
                    if (dd > tol) { why = "off_scalp"; nOff++; }
                }
            }
            if (why.empty() && stretched && edges[f] > maxEdge) { why = "stretched"; nStr++; }
            if (why.empty() && divergent) {
                if (faceDivergent(groom, F, edges[f], divDeg, spreadK)) { why = "divergent"; nDiv++; }
            }
            if (why.empty()) continue;
            json rr = json::array();
            for (int q = 0; q < 3; q++) { V3 p = groom.P[F.g[q]][0]; rr.push_back({r4(p.x), r4(p.y), r4(p.z)}); }
            if (list.size() < 80) list.push_back({{"face", f}, {"why", why}, {"guides", {F.g[0], F.g[1], F.g[2]}}, {"edge_mm", std::round(edges[f] * 10000) / 10}});
            ops.push_back({{"op", "delete_face"}, {"roots", rr}, {"id", ++id}, {"grp", grp}});
        }
        r["median_edge_mm"] = med * 1000; r["max_edge_mm"] = maxEdge * 1000;
        r["off_scalp"] = nOff; r["stretched"] = nStr; r["divergent"] = nDiv; r["faces"] = list;
        if (!c.value("list_only", false)) {
            for (auto& o : ops) groom.ops.push_back(o);
            groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
            r["removed"] = ops.size();
        }
        r["status"] = status_();
        return r;
    }
    if (d == "wind_check") {
        // read-only measured max-wind check (windcheck.cpp): restores the groom / simulation / motion state, adds no ops
        if (c.contains("threshold_px")) windThr = c["threshold_px"].get<float>() * 600.0f / std::max(1, c.value("size", 600));
        json res = windCheck(c, out);
        for (auto& [k, v] : res.items()) r[k] = v;
        r["status"] = status_();
        return r;
    }
    if (d == "band_sim") {
        // read-only (2026-10-14): the SIMULATED guide CVs (GPU read-back, every 1/60 s step) tested against a headband with
        // the headband op's geometry (r3::findBand / BandProf::crosses): per wind amount, the number of guides passing
        // OUTSIDE the band per step (max / mean over time, over `repeats` runs: the runtime's wind is random), the guides
        // that cross most often, and the sway envelope (per guide max displacement from rest of its CVs that rest below
        // sway_below_z, default head centre - 0.2 m: the hair below the shoulders). "outside" = beyond the inner surface minus
        // margin (inside the band's thickness counts: what studio_headband fixes); "over_outer" = beyond the band's outer
        // radius at that azimuth (visibly on top of the band). Params: amounts ([0, 1.5, 5]),
        // seconds (6), repeats (2), wind_noise (fur value), margin_mm (0.75 = headband clearance 1.5 / 2), worst (15),
        // sway_below_z, band params (mesh, z_range, max_height_mm, min_coverage). Restores simulation / motion / look.
        r3::BandProf BP;
        if (!r3::findBand(*this, c, BP, r)) return r;
        r["band"] = {{"mesh", pack.meshes[BP.mi].name}, {"z_min", BP.zMin}, {"z_max", BP.zMax}, {"mean_radius_mm", BP.rMean * 1000}, {"coverage", BP.cover}};
        const float margin = c.value("margin_mm", 0.75f) / 1000;
        const double seconds = std::clamp(c.value("seconds", 6.0), 0.5, 60.0);
        const int nSteps = std::max(1, int(seconds * 60.0 + 0.5));
        const int reps = std::clamp(c.value("repeats", 2), 1, 8), nWorst = std::clamp(c.value("worst", 15), 1, 200);
        const float swayZ = c.value("sway_below_z", pack.headC.z - 0.2f);
        json amounts = c.value("amounts", json::array({0.0, 1.5, 5.0}));
        const char* kNoise = "physicalMaterials.simulation.windNoise";
        auto lookOver = look.over;
        bool sSim = simulate, sPaused = paused, sWind = windOn;
        Motion sMotion = motion;
        double sAcc = simAcc;
        if (c.contains("wind_noise") && c["wind_noise"].is_number()) look.setF(kNoise, c["wind_noise"].get<float>());
        r["wind_noise"] = look.f(kNoise, 0.0f);
        const size_t NG = groom.P.size();
        hideGuide.clear(); hideFace.clear();
        rebuildHair();
        simulate = false; paused = false; windOn = false;
        motion.preset = MotionPreset::Rest; motion.speed = 1; motion.amount = 1; motion.t = 0; simAcc = 0; teleport = true;
        for (int i = 0; i < 3; i++) step(1.0 / 60.0);
        std::vector<float> xyz;
        int PG = 0;
        std::string info;
        bool ok = hw.readGuides(xyz, PG, &info) && PG > 0 && xyz.size() >= guideOfAsset.size() * size_t(PG) * 3;
        auto restore = [&]() {
            look.over = lookOver;
            hideGuide.clear(); hideFace.clear();
            rebuildHair();
            motion = sMotion; motion.preset = MotionPreset::Rest; windOn = false; teleport = true;
            for (int i = 0; i < 30; i++) step(1.0 / 60.0);
            motion = sMotion; simulate = sSim; paused = sPaused; windOn = sWind; simAcc = sAcc; aoRefreshAll = true;
        };
        if (!ok) { restore(); r["error"] = "guide read-back failed: " + info; return r; }
        const size_t NA = guideOfAsset.size();
        std::vector<V3> rest(xyz.size() / 3);
        for (size_t i = 0; i < rest.size(); i++) rest[i] = {xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]};
        auto strand = [&](const std::vector<float>& v, size_t a, Guide& G) {
            G.resize(PG);
            for (int j = 0; j < PG; j++) { size_t q = (a * PG + j) * 3; G[j] = {v[q], v[q + 1], v[q + 2]}; }
        };
        Guide G;
        int restCross = 0, restOver = 0;
        for (size_t a = 0; a < NA; a++) { strand(xyz, a, G); if (BP.crosses(G, margin)) restCross++; if (BP.excess(G, true) > 0) restOver++; }
        r["rest_crossing"] = restCross; r["rest_over_outer"] = restOver;
        r["cvs_per_guide"] = PG;
        json res = json::array();
        for (auto& am : amounts) {
            float wind = am.get<float>();
            std::vector<int> stepsOut(NG, 0), stepsOver(NG, 0);
            std::vector<float> sway(NG, 0.0f), exMax(NG, -1e9f), exOMax(NG, -1e9f);
            int maxNow = 0, maxStep = -1, maxOver = 0; double sumNow = 0, sumOver = 0; long nStepsAll = 0;
            for (int rep = 0; rep < reps; rep++) {
                rebuildHair();
                simulate = true; paused = false; windOn = wind > 0;
                motion.preset = MotionPreset::Rest; motion.speed = 1; motion.amount = 1; motion.windStrength = wind; motion.t = 0; simAcc = 0; teleport = true;
                for (int s = 0; s < nSteps; s++) {
                    step(1.0 / 60.0);
                    int pg = PG;
                    if (!hw.readGuides(xyz, pg) || pg != PG || xyz.size() < NA * size_t(PG) * 3) continue;
                    int now = 0, over = 0;
                    for (size_t a = 0; a < NA; a++) {
                        int g = guideOfAsset[a];
                        strand(xyz, a, G);
                        float ex = BP.excess(G);
                        exMax[g] = std::max(exMax[g], ex);
                        if (ex > -margin) { now++; stepsOut[g]++; }
                        float exO = BP.excess(G, true);
                        exOMax[g] = std::max(exOMax[g], exO);
                        if (exO > 0) { over++; stepsOver[g]++; }
                        for (int j = 0; j < PG; j++) {
                            const V3& p0 = rest[a * PG + j];
                            if (p0.z < swayZ) sway[g] = std::max(sway[g], len(G[j] - p0));
                        }
                    }
                    if (now > maxNow) { maxNow = now; maxStep = s; }
                    maxOver = std::max(maxOver, over);
                    sumNow += now; sumOver += over; nStepsAll++;
                }
            }
            int ever = 0, everOver = 0;
            std::vector<std::pair<long, int>> wl;   // worst: most steps over the outer radius, then most steps outside
            for (size_t g = 0; g < NG; g++) { if (stepsOver[g] > 0) everOver++; if (stepsOut[g] > 0) { ever++; wl.push_back({long(stepsOver[g]) * 100000 + stepsOut[g], int(g)}); } }
            std::sort(wl.rbegin(), wl.rend());
            json worst = json::array();
            for (size_t i = 0; i < wl.size() && (int)i < nWorst; i++) {
                const V3& rt = groom.P[wl[i].second][0];
                worst.push_back({{"guide", wl[i].second}, {"steps_outside", stepsOut[wl[i].second]}, {"steps_over_outer", stepsOver[wl[i].second]},
                                 {"max_excess_mm", std::round(exMax[wl[i].second] * 1e4) / 10}, {"max_over_outer_mm", std::round(exOMax[wl[i].second] * 1e4) / 10}, {"root", {rt.x, rt.y, rt.z}}});
            }
            std::vector<float> sw;
            for (size_t a = 0; a < NA; a++) { int g = guideOfAsset[a]; bool low = false; for (int j = 0; j < PG; j++) if (rest[a * PG + j].z < swayZ) { low = true; break; } if (low) sw.push_back(sway[g]); }
            std::sort(sw.begin(), sw.end());
            auto pct = [&](double q) { return sw.empty() ? 0.0 : std::round(sw[std::min(sw.size() - 1, size_t(q * (sw.size() - 1) + 0.5))] * 1e4) / 10; };
            res.push_back({{"amount", wind}, {"steps", nSteps}, {"repeats", reps}, {"max_crossing", maxNow}, {"max_at_step", maxStep},
                           {"mean_crossing", nStepsAll ? std::round(sumNow / nStepsAll * 100) / 100 : 0.0}, {"guides_ever_outside", ever},
                           {"max_over_outer", maxOver}, {"mean_over_outer", nStepsAll ? std::round(sumOver / nStepsAll * 100) / 100 : 0.0}, {"guides_ever_over_outer", everOver}, {"worst", worst},
                           {"sway_guides", (int)sw.size()}, {"sway_mm", {{"p50", pct(0.5)}, {"p90", pct(0.9)}, {"max", pct(1.0)}}}});
        }
        r["runs"] = res;
        r["sway_below_z"] = swayZ;
        restore();
        r["status"] = status_();
        return r;
    }
    if (d == "collision_band") {
        // opt-in (2026-10-14, Ciri wavy headband): collision spheres / capsules that reach OUT through a headband push the
        // simulated hair over it (band_sim: 47 guides over the band with no wind, 0 with collision off; stiffness 1 does not
        // help). Detects the band (r3::findBand, same params as headband) and shrinks every sphere whose surface, inside the
        // band's height range, comes closer than clearance_mm (2) to the band's inner surface; capsules (sphere pairs) are
        // sampled between their ends and both ends shrink until they fit. A sphere left under min_r_mm (3) gets 0.1 mm (off).
        // The radii are project state (colR, saved as "collision_r", exported in the geo json and written into the fur by
        // desk_export.set_geometry, so the Studio export and the pipeline's studio_geo build carry them). list_only: report
        // only. set {"<sphere>": r_m | -1}: explicit radii (-1 = back to the pack's). reset: clear all overrides.
        auto finish = [&]() {
            json cr = json::object();
            for (auto& [i, rr] : colR) cr[std::to_string(i)] = {{"r", rr}, {"r_pack", pack.spheres[i].r}};
            r["collision_r"] = cr;
            rebuildHair(); settleRest(*this, 0.5);
            r["status"] = status_();
            return r;
        };
        if (c.value("reset", false)) { colR.clear(); r["reset"] = true; return finish(); }
        if (c.contains("set")) {
            for (auto& [k, v] : c["set"].items()) {
                int i = std::stoi(k);
                if (i < 0 || i >= (int)pack.spheres.size()) continue;
                float rr = v.get<float>();
                if (rr < 0) colR.erase(i); else colR[i] = rr;
            }
            return finish();
        }
        r3::BandProf BP;
        if (!r3::findBand(*this, c, BP, r)) return r;
        const float clr = c.value("clearance_mm", 2.0f) / 1000, rMin = c.value("min_r_mm", 3.0f) / 1000;
        const int NS = (int)pack.spheres.size();
        const V3 C = BP.C;
        std::vector<float> R0(NS), R(NS);
        for (int i = 0; i < NS; i++) { R0[i] = colR.count(i) ? colR[i] : pack.spheres[i].r; R[i] = R0[i]; }
        // most a sphere reaches beyond (band inner radius - clearance) inside the band's height range, m (<= 0: fits)
        auto pierce = [&](V3 cc, float rr) {
            float ex = -1e9f;
            const int NLA = 24, NLO = 48;
            for (int a = 0; a <= NLA; a++) {
                float th = 3.14159265f * a / NLA, sz = std::cos(th), sr = std::sin(th);
                for (int o = 0; o < (a == 0 || a == NLA ? 1 : NLO); o++) {
                    float ph = 6.2831853f * o / NLO;
                    V3 p{cc.x + rr * sr * std::cos(ph), cc.y + rr * sr * std::sin(ph), cc.z + rr * sz};
                    float rI, zL, zH;
                    if (!BP.at(p, rI, zL, zH) || p.z < zL || p.z > zH) continue;
                    ex = std::max(ex, (float)std::hypot(p.x - C.x, p.y - C.y) - (rI - clr));
                }
            }
            return ex;
        };
        json list = json::array();
        int noC = 0;
        std::vector<float> before(NS, -1e9f);
        for (int i = 0; i < NS; i++) {
            const Sphere& s = pack.spheres[i];
            if (!s.hasC) { noC++; continue; }
            float ex = pierce(s.c, R[i]);
            before[i] = ex;
            if (ex <= 0) continue;
            float lo = 0, hi = R[i];
            for (int it = 0; it < 24; it++) { float m = 0.5f * (lo + hi); if (pierce(s.c, m) > 0) hi = m; else lo = m; }
            R[i] = lo;
        }
        int capFixed = 0;
        for (auto& cp : pack.capsules) {
            int a = cp[0], b = cp[1];
            if (a < 0 || b < 0 || a >= NS || b >= NS || !pack.spheres[a].hasC || !pack.spheres[b].hasC) continue;
            bool fixedHere = false;
            for (int it = 0; it < 80; it++) {
                float ex = -1e9f;
                for (int q = 1; q < 10; q++) {
                    float t = q / 10.0f;
                    ex = std::max(ex, pierce(pack.spheres[a].c * (1 - t) + pack.spheres[b].c * t, R[a] * (1 - t) + R[b] * t));
                }
                if (ex <= 0) break;
                R[a] *= 0.95f; R[b] *= 0.95f; fixedHere = true;
            }
            capFixed += fixedHere;
        }
        std::map<int, float> nr = colR;
        for (int i = 0; i < NS; i++) {
            if (R[i] >= R0[i] - 1e-5f) continue;
            float v = R[i] < rMin ? 0.0001f : R[i];
            nr[i] = v;
            const Sphere& s = pack.spheres[i];
            list.push_back({{"sphere", i}, {"bone", s.bone}, {"c", {s.c.x, s.c.y, s.c.z}}, {"r_old_mm", R0[i] * 1000}, {"r_new_mm", v * 1000},
                            {"excess_mm", before[i] > -1e8f ? before[i] * 1000 : 0.0f}, {"off", v < rMin}});
        }
        r["band"] = {{"mesh", pack.meshes[BP.mi].name}, {"z_min", BP.zMin}, {"z_max", BP.zMax}};
        r["changed"] = list; r["capsules_shrunk"] = capFixed; r["spheres_without_centre"] = noC;
        r["clearance_mm"] = clr * 1000;
        if (c.value("list_only", false)) { r["status"] = status_(); return r; }
        colR = nr;
        return finish();
    }
    if (d == "check_loops") {
        // read-only: guides whose strand comes back to itself (see guideLoop); curls are exempt
        loopArc = c.value("min_arc_mm", loopArc); loopClose = c.value("return_mm", loopClose); loopCurlR = c.value("curl_radius_mm", loopCurlR); loopTurn = c.value("circling_deg", loopTurn);
        checkLoops();
        json l = json::array();
        for (size_t i = 0; i < loopList.size(); i++) l.push_back({{"guide", loopList[i]}, {"reason", loopWhy[i]}});
        r["flagged"] = l; r["count"] = (int)loopList.size(); r["status"] = status_();
        return r;
    }
    if (d == "dedupe") {
        // remove guides that duplicate another one (root within tol_mm and tip within 4 * tol_mm): copies left by repeated
        // plant / fill / duplicate calls add budget without adding hair
        float tol = c.value("tol_mm", 0.6f) / 1000;
        std::vector<int> dup;
        for (size_t g = 0; g < groom.P.size(); g++) {
            if (!groom.alive[g]) continue;
            for (size_t h = 0; h < g; h++) {
                if (!groom.alive[h]) continue;
                if (len(groom.P[g][0] - groom.P[h][0]) < tol && len(groom.P[g].back() - groom.P[h].back()) < tol * 4) { dup.push_back(int(g)); break; }
            }
        }
        if (!dup.empty() && !c.value("list_only", false)) {
            sel = std::set<int>(dup.begin(), dup.end());
            opDelete();
            settleRest(*this, 0.5);
        }
        json l = json::array();
        for (int g : dup) l.push_back(g);
        r["duplicates"] = l; r["removed"] = c.value("list_only", false) ? 0 : int(dup.size()); r["status"] = status_();
        return r;
    }
    if (d == "eye_check") {
        // hair over the eyes, measured on the rendered strands: hair-only mask pass (red) per camera, eye boxes from the
        // eye meshes (posed, split left / right, padded pad_mm) projected to the image; covered = hair pixels / box pixels
        std::vector<int> shotsL;
        for (auto& v : c.value("shots", json::array({4, 3, 15}))) shotsL.push_back(v.get<int>());
        float pad = c.value("pad_mm", 2.0f) / 1000;
        int size = c.value("size", 700);
        V3 lo[2] = {{1e9f, 1e9f, 1e9f}, {1e9f, 1e9f, 1e9f}}, hi[2] = {{-1e9f, -1e9f, -1e9f}, {-1e9f, -1e9f, -1e9f}};
        bool found = false;
        for (size_t mi = 0; mi < pack.meshes.size(); mi++) {
            if (pack.meshes[mi].texFile.find("eye") == std::string::npos) continue;
            std::vector<V3> pos; std::vector<float> fl; std::vector<std::array<int, 3>> tr;
            for (auto& v : pack.meshes[mi].v) {
                int sd = v.x > pack.headC.x ? 0 : 1;   // 0 = character's left (+X)
                lo[sd] = {std::min(lo[sd].x, v.x), std::min(lo[sd].y, v.y), std::min(lo[sd].z, v.z)};
                hi[sd] = {std::max(hi[sd].x, v.x), std::max(hi[sd].y, v.y), std::max(hi[sd].z, v.z)};
                found = true;
            }
        }
        if (!found) { r["error"] = "no eye mesh in this pack"; return r; }
        json res = json::array();
        double worst = 0;
        BaselineScope bs(*this, c.value("baseline", false));
        for (int sh : shotsL) {
            std::string k = std::to_string(sh);
            if (!pack.shots.contains(k)) continue;
            auto& cam = pack.shots[k];
            // hair pixels = pixels that change when the hair is removed (works for the groom and the vanilla cards):
            // render with the hair ("vanilla": true = the game's hair cards), then without, same frame
            bool sh0 = showHair, vh0 = vanillaHair, sim0 = simulate;
            bool van = c.value("vanilla", false);
            simulate = false;   // no simulation step between the two renders
            bool shd0 = rig.shadows; float ao0 = rig.aoStrength;
            rig.shadows = false; rig.aoStrength = 0;   // hair shadows on the skin would count as covered
            vanillaHair = van; showHair = !van;
            std::string mp = out + "\\_eyeA.png", mq = out + "\\_eyeB.png";
            bool ok = renderShot(cam, mp, size);
            vanillaHair = false; showHair = false;
            ok = ok && renderShot(cam, mq, size);
            showHair = sh0; vanillaHair = vh0; simulate = sim0; rig.shadows = shd0; rig.aoStrength = ao0;
            int w = 0, h = 0, w2 = 0, h2 = 0;
            unsigned char* imA = ok ? loadImage(mp, &w, &h) : nullptr;
            unsigned char* imB = ok ? loadImage(mq, &w2, &h2) : nullptr;
            if (!imA || !imB || w != w2 || h != h2) { if (imA) freeImage(imA); if (imB) freeImage(imB); continue; }
            unsigned char* im = imA;
            for (size_t i = 0; i < size_t(w) * h; i++) {
                int dsum = std::abs(imA[i * 4] - imB[i * 4]) + std::abs(imA[i * 4 + 1] - imB[i * 4 + 1]) + std::abs(imA[i * 4 + 2] - imB[i * 4 + 2]);
                im[i * 4] = dsum > 45 ? 255 : 0; im[i * 4 + 1] = 0; im[i * 4 + 2] = 0;
            }
            freeImage(imB);
            XMFLOAT3 eye{cam["eye"][0], cam["eye"][1], cam["eye"][2]}, tg{cam["target"][0], cam["target"][1], cam["target"][2]};
            XMMATRIX VP = XMMatrixLookAtRH(XMLoadFloat3(&eye), XMLoadFloat3(&tg), XMVectorSet(0, 0, 1, 0)) *
                          XMMatrixPerspectiveFovRH(XMConvertToRadians(cam.value("fovy", 11.16f)), 1.0f, 0.05f, 100.0f);
            V3 E{eye.x, eye.y, eye.z};
            json je = {{"shot", sh}, {"view", cam.value("view", "")}};
            for (int sd = 0; sd < 2; sd++) {
                V3 a = lo[sd] - V3{pad, pad, pad}, b = hi[sd] + V3{pad, pad, pad};
                V3 cen = (a + b) * 0.5f;
                // eye facing away from the camera (side / back views): not visible, skip
                if (dot(V3{0, 1, 0}, nrmz(E - cen)) < 0.15f) { je[sd == 0 ? "eye_char_left" : "eye_char_right"] = "not visible"; continue; }
                // three-quarter views: the far eye sits behind the nose / temple hair, measure only the near one
                if (std::fabs(E.x - pack.headC.x) > 0.3f && ((E.x > pack.headC.x) != (sd == 0))) { je[sd == 0 ? "eye_char_left" : "eye_char_right"] = "far side, skipped"; continue; }
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (int q = 0; q < 8; q++) {
                    V3 p{q & 1 ? b.x : a.x, q & 2 ? b.y : a.y, q & 4 ? b.z : a.z};
                    XMVECTOR v = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), VP);
                    float ww = XMVectorGetW(v);
                    float sx = (XMVectorGetX(v) / ww * 0.5f + 0.5f) * w, sy = (1 - (XMVectorGetY(v) / ww * 0.5f + 0.5f)) * h;
                    x0 = std::min(x0, sx); y0 = std::min(y0, sy); x1 = std::max(x1, sx); y1 = std::max(y1, sy);
                }
                int X0 = std::clamp(int(x0), 0, w - 1), X1 = std::clamp(int(x1), 0, w - 1), Y0 = std::clamp(int(y0), 0, h - 1), Y1 = std::clamp(int(y1), 0, h - 1);
                long tot = 0, hair = 0;
                for (int y = Y0; y <= Y1; y++)
                    for (int x = X0; x <= X1; x++) {
                        const unsigned char* q = &im[(size_t(y) * w + x) * 4];
                        tot++;
                        if (q[0] > 128 && q[1] < 100 && q[2] < 100) hair++;
                    }
                double f = tot ? double(hair) / tot : 0;
                worst = std::max(worst, f);
                je[sd == 0 ? "eye_char_left" : "eye_char_right"] = {{"covered", std::round(f * 1000) / 1000}, {"box_px", {X0, Y0, X1, Y1}}};
                if (c.value("culprits", true) && f > 0.01) {
                    // which guides put hair in front of this eye: strand points projecting inside the box and nearer to the
                    // camera than the eye (the interpolated hair around them follows them)
                    float eyeDist = len(cen - E);
                    std::vector<std::pair<int, int>> cnt;
                    for (size_t g = 0; g < groom.P.size(); g++) {
                        if (!groom.alive[g]) continue;
                        int hits = 0;
                        for (int k = 1; k < groom.K; k++) {
                            V3 p = posedPoint(int(g), k);
                            if (len(p - E) > eyeDist) continue;
                            XMVECTOR v = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), VP);
                            float ww = XMVectorGetW(v);
                            if (ww <= 1e-5f) continue;
                            float sx = (XMVectorGetX(v) / ww * 0.5f + 0.5f) * w, sy = (1 - (XMVectorGetY(v) / ww * 0.5f + 0.5f)) * h;
                            if (sx >= X0 - 6 && sx <= X1 + 6 && sy >= Y0 - 6 && sy <= Y1 + 6) hits++;
                        }
                        if (hits) cnt.push_back({hits, int(g)});
                    }
                    std::sort(cnt.rbegin(), cnt.rend());
                    json cl = json::array();
                    for (size_t q = 0; q < cnt.size() && q < 12; q++) {
                        int g = cnt[q].second;
                        cl.push_back({{"i", g}, {"points_in_front", cnt[q].first}, {"root", v3j(groom.P[g][0])}, {"length", std::round(guideLen(groom.P[g]) * 1e4f) / 1e4f}});
                    }
                    je[sd == 0 ? "eye_char_left" : "eye_char_right"]["culprits"] = cl;
                }
            }
            freeImage(im);
            std::error_code ec; fs::remove(mp, ec); fs::remove(mq, ec);
            res.push_back(je);
        }
        r["shots"] = res;
        r["worst_covered"] = std::round(worst * 1000) / 1000;
        r["note"] = "covered = fraction of the padded eye box hidden by hair strands (rendered); vanilla Guillaume ~0; keep < 0.05";
        return r;
    }
    if (d == "lay_down") {
        // frizz fix without deleting: guides whose root part lifts off the scalp (lift_deg > min_lift_deg) are laid onto
        // it: the root part (span_mm) bends down to lie at target_lift_deg along the local flow (8 neighbours' mean, or
        // "direction"), the rest of the strand follows unchanged in shape
        auto ix = selectGuides(c.value("where", json::object({{"all", true}})));
        float minLift = c.value("min_lift_deg", 35.0f), target = c.value("target_lift_deg", 8.0f) * 3.14159265f / 180;
        float spanM = c.value("span_mm", 25.0f) / 1000, maxL = c.value("max_length", 10.0f);
        std::vector<V3> tan(groom.P.size());
        std::vector<int> alive;
        for (size_t g = 0; g < groom.P.size(); g++) if (groom.alive[g]) { alive.push_back(int(g)); tan[g] = rootTangent(*this, int(g)); }
        int id = 0, grp = 1000 + (++grpId), n = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        json done = json::array();
        int curled = 0;
        for (int g : ix) {
            float lf = liftDeg(*this, g, spanM);
            if (lf < minLift || guideLen(groom.P[g]) > maxL) continue;
            if (c.value("keep_curls", true)) {
                // styled hair (rolls / ringlets): total turning per metre high = a baked curl, leave it alone
                auto& G0 = groom.P[g];
                float turn = 0;
                for (size_t k = 2; k < G0.size(); k++)
                    turn += std::acos(std::clamp(dot(nrmz(G0[k - 1] - G0[k - 2]), nrmz(G0[k] - G0[k - 1])), -1.0f, 1.0f));
                if (turn / std::max(guideLen(G0), 1e-3f) > c.value("curl_rad_per_m", 25.0f)) { curled++; continue; }
            }
            Guide G = groom.P[g];
            V3 nrm = nrmz(G[0] - pack.headC), want{0, 0, 0};
            if (c.contains("direction")) want = V3{c["direction"][0], c["direction"][1], c["direction"][2]};
            else {
                std::vector<std::pair<float, int>> nb;
                for (int h : alive) if (h != g) nb.push_back({len(groom.P[h][0] - G[0]), h});
                size_t kk = std::min<size_t>(8, nb.size());
                std::partial_sort(nb.begin(), nb.begin() + kk, nb.end());
                for (size_t q = 0; q < kk; q++) want = want + tan[nb[q].second];
            }
            want = want - nrm * dot(want, nrm);
            if (len(want) < 1e-6f) want = tan[g];
            want = nrmz(want);
            // target direction of the root part: along the flow, target degrees above the scalp
            V3 dir = nrmz(want * std::cos(target) + nrm * std::sin(target));
            auto sl = arcLen(G);
            Guide Q = G;
            V3 d0 = nrmz(G[std::min<size_t>(1, G.size() - 1)] - G[0]);
            // rotation taking the current root direction onto dir, applied fully over the span, fading to 0 after it
            V3 ax = cross(d0, dir);
            float sa = len(ax), ang = std::atan2(sa, dot(d0, dir));
            if (sa < 1e-6f) continue;
            ax = ax * (1.0f / sa);
            for (size_t k = 1; k < G.size(); k++) {
                float f = sl[k - 1] / std::max(spanM, 1e-4f);
                float w = f <= 0.7f ? 1.0f : f >= 1.4f ? 0.0f : 1.0f - (f - 0.7f) / 0.7f;
                Q[k] = Q[k - 1] + rotAxis(G[k] - G[k - 1], ax, ang * w);
            }
            ops.push_back({{"op", "reshape"}, {"root", {r4(G[0].x), r4(G[0].y), r4(G[0].z)}}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
            done.push_back({{"i", g}, {"lift_deg", std::round(lf)}});
            n++;
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged(); settleRest(*this, 0.5);
        r["laid"] = n; r["skipped_curled"] = curled; r["guides"] = done; r["status"] = status_();
        return r;
    }
    if (d == "fill_scalp") {
        // closed loop for bald spots: render the scalp overlay from the camera, sample the visible green pixels, ray-cast each
        // to the scalp, and plant a guide there (template = a nearby long guide, root part turned to the local flow = mean
        // root direction of the 8 nearest guides, hugging the scalp). spacing_mm between new roots, max_new cap.
        json cam;
        if (!cameraOf(*this, c, cam)) { r["error"] = "no such camera"; return r; }
        int size = c.value("size", 900), maxNew = c.value("max_new", 60);
        float spacing = c.value("spacing_mm", 4.0f) / 1000, minL = c.value("min_length", 0.15f);
        std::string png = out + "\\_fill.png";
        bool so = scalpOverlay;
        scalpOverlay = true;
        bool ok = renderShot(cam, png, size);
        scalpOverlay = so;
        int w = 0, h = 0;
        unsigned char* im = ok ? loadImage(png, &w, &h) : nullptr;
        if (!im) { r["error"] = "render failed"; return r; }
        XMFLOAT3 eye{cam["eye"][0], cam["eye"][1], cam["eye"][2]}, tg{cam["target"][0], cam["target"][1], cam["target"][2]};
        XMMATRIX V = XMMatrixLookAtRH(XMLoadFloat3(&eye), XMLoadFloat3(&tg), XMVectorSet(0, 0, 1, 0));
        XMMATRIX P = XMMatrixPerspectiveFovRH(XMConvertToRadians(cam.value("fovy", 11.16f)), 1.0f, 0.05f, 100.0f);
        XMMATRIX IVP = XMMatrixInverse(nullptr, V * P);
        std::vector<V3> pts;   // edit-frame scalp points of visible green pixels
        int stepPx = std::max(2, c.value("sample_px", 4)), greens = 0;
        std::vector<int> alive, longs;
        for (size_t g = 0; g < groom.P.size(); g++)
            if (groom.alive[g]) { alive.push_back(int(g)); if (guideLen(groom.P[g]) >= minL) longs.push_back(int(g)); }
        for (int y = 0; y < h; y += stepPx)
            for (int x = 0; x < w; x += stepPx) {
                const unsigned char* q = &im[(size_t(y) * w + x) * 4];
                if (!(q[1] > 200 && q[0] < 60 && q[2] < 60)) continue;
                greens++;
                float nx = (x + 0.5f) / w * 2 - 1, ny = 1 - (y + 0.5f) / h * 2;
                XMVECTOR ra = XMVector3TransformCoord(XMVectorSet(nx, ny, 0, 1), IVP), rb = XMVector3TransformCoord(XMVectorSet(nx, ny, 1, 1), IVP);
                XMFLOAT3 o, dir, hit;
                XMStoreFloat3(&o, ra);
                XMStoreFloat3(&dir, XMVector3Normalize(XMVectorSubtract(rb, ra)));
                int mi;
                if (!skinRay(o, dir, hit, mi)) continue;
                V3 H{hit.x, hit.y, hit.z};
                // posed -> edit frame: shift of the nearest guide root
                int g0 = -1; float bd = 1e30f;
                for (int g : alive) { float dd = len(posedPoint(g, 0) - H); if (dd < bd) { bd = dd; g0 = g; } }
                if (g0 < 0) continue;
                V3 E = H + (groom.P[g0][0] - posedPoint(g0, 0));
                bool close = false;
                for (auto& p : pts) if (len(p - E) < spacing) { close = true; break; }
                if (!close) pts.push_back(E);
            }
        freeImage(im);
        std::error_code ec;
        fs::remove(png, ec);
        std::vector<V3> tan(groom.P.size());
        for (int g : alive) tan[g] = rootTangent(*this, g);
        HeadRadial hrF = headRadial(*this);
        int id = 0, grp = 1000 + (++grpId), n = 0, rejected = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        for (auto& E : pts) {
            if (n >= maxNew) break;
            std::vector<std::pair<float, int>> nb;
            for (int g : alive) nb.push_back({len(groom.P[g][0] - E), g});
            size_t kk = std::min<size_t>(8, nb.size());
            std::partial_sort(nb.begin(), nb.begin() + kk, nb.end());
            V3 want{0, 0, 0};
            for (size_t q = 0; q < kk; q++) want = want + tan[nb[q].second];
            if (c.contains("direction")) want = V3{c["direction"][0], c["direction"][1], c["direction"][2]};
            // template: long guide within 3 cm whose root flows most like the local flow
            V3 wn = nrmz(want);
            std::vector<std::pair<float, int>> cand;
            float tmax = c.value("template_mm", 12.0f) / 1000;   // nearby templates only: a far copy is shifted a lot
            for (int g : longs) {
                float dd = len(groom.P[g][0] - E);
                if (dd > tmax) continue;
                cand.push_back({dd + 0.02f * (1.0f - dot(tan[g], wn)), g});
            }
            std::sort(cand.begin(), cand.end());
            // a template copied to this root must not pass through the head (its hair would come out of the skin)
            if (cand.empty()) {   // under brims / hats: allow farther templates (still rejected if they pass through the head)
                for (int g : longs) {
                    float dd = len(groom.P[g][0] - E);
                    if (dd <= tmax * 2.5f) cand.push_back({dd + 0.02f * (1.0f - dot(tan[g], wn)), g});
                }
                std::sort(cand.begin(), cand.end());
            }
            int best = -1;
            for (auto& [sc, g] : cand) {
                Guide T = groom.P[g];
                V3 rs = T[0], q = pack.headC + nrmz(E - pack.headC) * len(rs - pack.headC);
                for (auto& p : T) p = p + (q - rs);
                // reject copies that pass through the head, or that rise above their root (a tuft sticking up)
                bool up = false;
                for (auto& p : T) if (p.z > T[0].z + 0.015f) { up = true; break; }
                if (!up && pushOut(hrF, T, 0.003f) == 0) { best = g; break; }
            }
            if (best < 0) { rejected++; continue; }
            plantGuide(*this, best, E, want, c.value("strength", 1.0f), c.value("span_mm", 25.0f) / 1000, c.value("lift_mm", 3.0f) / 1000, id, grp, ops, &hrF, c.value("max_length", 0.0f));
            n++;
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); smap.ensure(groom.P.size()); editsChanged();
        // budget guard: drop the last planted guides (2 ops each) until the hair budget is within the limit
        int dropped = 0;
        computeBudget();
        while (bud.overLimit && n > 0 && !c.value("ignore_budget", false)) {
            groom.ops.pop_back(); groom.ops.pop_back();
            n--; dropped++;
            if (dropped % 4 == 0 || n == 0) { groom.replay(); editsChanged(); computeBudget(); }
        }
        if (dropped) { groom.replay(); editsChanged(); computeBudget(); r["dropped_for_budget"] = dropped; }
        settleRest(*this, 0.5);
        r["green_samples"] = greens; r["spots"] = pts.size(); r["planted"] = n; r["no_template_outside_head"] = rejected; r["status"] = status_();
        return r;
    }
    if (d == "plant_parting") {
        // new long guides rooted along both edges of a parting line [[p0], [p1]] (edit frame), each a copy of the nearest
        // long guide on its side, root turned to flow away from the line: covers the split with clean hair
        if (!c.contains("line")) { r["error"] = "line [[x,y,z],[x,y,z]] required"; return r; }
        V3 p0{c["line"][0][0], c["line"][0][1], c["line"][0][2]}, p1{c["line"][1][0], c["line"][1][1], c["line"][1][2]};
        float step = c.value("spacing_mm", 6.0f) / 1000, minL = c.value("min_length", 0.12f), spanM = c.value("span_mm", 25.0f) / 1000;
        std::vector<float> offs;
        for (auto& o : c.value("offsets_mm", json::array({4.0}))) offs.push_back(o.get<float>() / 1000);
        std::string sides = c.value("sides", std::string("both"));
        V3 L = p1 - p0;
        float LL = len(L);
        if (LL < 1e-4f) { r["error"] = "line too short"; return r; }
        L = L * (1.0f / LL);
        std::vector<int> longs;
        for (size_t g = 0; g < groom.P.size(); g++) if (groom.alive[g] && guideLen(groom.P[g]) >= minL) longs.push_back(int(g));
        if (longs.empty()) { r["error"] = "no guide longer than min_length"; return r; }
        int id = 0, grp = 1000 + (++grpId), n = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        HeadRadial hrP = headRadial(*this);
        int noTemplate = 0, throughHead = 0, tuft = 0;
        for (float t = 0; t <= LL + 1e-5f; t += step) {
            V3 a = p0 + L * t, nrm = nrmz(a - pack.headC), side = nrmz(cross(nrm, L));
            for (int sg = -1; sg <= 1; sg += 2) {
                if ((sides == "plus" && sg < 0) || (sides == "minus" && sg > 0)) continue;
                for (float o : offs) {
                    V3 q = a + side * (o * float(sg));
                    // template: a long guide on this side of the line within 3 cm, preferring ones whose root already flows
                    // away from the line (score = distance + penalty for a wrong direction); its root radius puts the new
                    // root on the scalp
                    V3 wantS = side * float(sg);
                    int best = -1;
                    float bd = 1e30f, bdist = 1e30f;
                    for (int g : longs) {
                        V3 rr = groom.P[g][0];
                        if (dot(rr - a, side) * float(sg) < 0) continue;
                        float dd = len(rr - q);
                        if (dd > 0.03f) continue;
                        float sc = dd + 0.02f * (1.0f - dot(rootTangent(*this, g), wantS));
                        if (sc < bd) { bd = sc; best = g; bdist = dd; }
                    }
                    bd = bdist;
                    if (best < 0 || bd > 0.03f) { noTemplate++; continue; }
                    V3 rs = groom.P[best][0];
                    q = pack.headC + nrmz(q - pack.headC) * len(rs - pack.headC);
                    Guide G = groom.P[best];
                    for (auto& p : G) p = p + (q - rs);
                    V3 n2 = nrmz(q - pack.headC), want = side * float(sg);
                    want = nrmz(want - n2 * dot(want, n2));
                    V3 tg = rootTangent(*this, best);
                    float ang = std::atan2(dot(cross(tg, want), n2), dot(tg, want));
                    ang = std::clamp(ang * c.value("strength", 0.7f), -1.2f, 1.2f);
                    Guide Q = turnRoot(G, n2, ang, spanM);
                    // hug: the root part lies on the scalp (radius root + up to lift_mm at span), no standing tufts
                    {
                        auto sl = arcLen(Q);
                        float r0 = len(Q[0] - pack.headC), lift = c.value("lift_mm", 3.0f) / 1000;
                        for (size_t k = 1; k < Q.size(); k++) {
                            float f = sl[k] / std::max(spanM, 1e-4f);
                            if (f >= 1.0f) break;
                            V3 rel = Q[k] - pack.headC;
                            float rk = len(rel), rt = r0 + lift * f;
                            if (rk > rt) {
                                V3 nq = pack.headC + nrmz(rel) * (rk + (rt - rk) * (1.0f - f));
                                V3 dl = nq - Q[k];
                                for (size_t j = k; j < Q.size(); j++) Q[j] = Q[j] + dl * (j == k ? 1.0f : std::max(0.0f, 1.0f - (sl[j] - sl[k]) / std::max(spanM, 1e-4f)));
                            }
                        }
                    }
                    if (pushOut(hrP, Q, 0.003f) > 0) { throughHead++; continue; }   // this template would pass through the head here
                    bool up = false;
                    for (auto& p : Q) if (p.z > Q[0].z + 0.015f) { up = true; break; }
                    if (up) { tuft++; continue; }   // would stick up as a tuft
                    V3 dd = q - rs;
                    ops.push_back({{"op", "duplicate"}, {"root", {r4(rs.x), r4(rs.y), r4(rs.z)}}, {"d", {r4(dd.x), r4(dd.y), r4(dd.z)}}, {"id", ++id}, {"grp", grp}});
                    ops.push_back({{"op", "reshape"}, {"root", {r4(Q[0].x), r4(Q[0].y), r4(Q[0].z)}}, {"points", ptsJson(Q)}, {"id", ++id}, {"grp", grp}});
                    n++;
                }
            }
        }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); smap.ensure(groom.P.size()); editsChanged();
        // budget guard: drop the last planted guides (2 ops each) until the hair budget is within the limit
        int dropped = 0;
        computeBudget();
        while (bud.overLimit && n > 0 && !c.value("ignore_budget", false)) {
            groom.ops.pop_back(); groom.ops.pop_back();
            n--; dropped++;
            if (dropped % 4 == 0 || n == 0) { groom.replay(); editsChanged(); computeBudget(); }
        }
        if (dropped) { groom.replay(); editsChanged(); computeBudget(); r["dropped_for_budget"] = dropped; }
        settleRest(*this, 0.5);
        r["planted"] = n; r["status"] = status_();
        r["rejected"] = {{"no_long_guide_within_3cm_on_that_side", noTemplate}, {"copy_through_head", throughHead}, {"copy_sticks_up", tuft},
                         {"hint", "short hair: lower min_length (e.g. 0.05); no side found: check the line points (studio_pick scalp_edit)"}};
        return r;
    }
    if (d == "hairline_fill") {
        // 2026-10-12 opt-in: fuller, cleaner hairline from guides only (no cards). Along the painted hairline (or the vanilla
        // card footprint edge when the head has no painted scalp: scalpMesh falls back to it), stations every spacing_mm;
        // rows of new guides at offsets_mm inside the line, each a copy of the nearest long-enough guide on the same side
        // (its shape and flow kept), shorter toward the edge (taper), hugging the scalp; copies over the eyes / face / ears,
        // through the head or sticking up are rejected. New roots past the growth-mesh edge get faces fanned onto it
        // (duplicate "edge_face"), so interpolated hair renders there. straighten: stations on a smoothed line.
        Hairline hl;
        if (!hairlineOf(*this, hl)) { r["error"] = "no scalp mask and no vanilla card footprint on the head mesh"; return r; }
        std::string part = c.value("part", std::string("all"));
        int rows = std::clamp(c.value("rows", 2), 1, 4);
        std::vector<float> offs;
        for (auto& o : c.value("offsets_mm", json::array({2.0, 5.0, 8.0, 11.0}))) offs.push_back(o.get<float>() / 1000);
        if ((int)offs.size() > rows) offs.resize(rows);
        rows = (int)offs.size();
        float spacing = c.value("spacing_mm", 5.0f) / 1000, minL = c.value("min_length", 0.06f);
        float taper = std::clamp(c.value("taper", 0.6f), 0.1f, 1.0f), lift = c.value("lift_mm", 2.0f) / 1000, spanM = c.value("span_mm", 25.0f) / 1000;
        float tmax = c.value("template_mm", 25.0f) / 1000, maxRatio = c.value("max_ratio", 1.2f);
        bool straight = c.value("straighten", false), dry = c.value("list_only", false) || c.value("dry_run", false);
        float smoothR = c.value("smooth_mm", 12.0f) / 1000;
        int maxNew = c.value("max_new", 240);
        computeBudget();
        if (bud.ratio > maxRatio && !c.value("ignore_budget", false)) {
            r["error"] = "hair budget ratio already above max_ratio: nothing planted"; r["status"] = status_(); return r;
        }
        const V3 C = pack.headC;
        // eyes / ears (found as clear_ear finds them)
        V3 eLo{1e9f, 1e9f, 1e9f}, eHi{-1e9f, -1e9f, -1e9f};
        for (auto& m : pack.meshes) if (m.texFile.find("eye") != std::string::npos)
            for (auto& v : m.v) { eLo = {std::min(eLo.x, v.x), std::min(eLo.y, v.y), std::min(eLo.z, v.z)}; eHi = {std::max(eHi.x, v.x), std::max(eHi.y, v.y), std::max(eHi.z, v.z)}; }
        bool haveEye = eLo.x < 1e8f;
        float eyeZ = haveEye ? (eLo.z + eHi.z) / 2 : C.z - 0.03f, eyeY = haveEye ? (eLo.y + eHi.y) / 2 : C.y + 0.08f;
        V3 earLo[2], earHi[2]; bool hasEar[2] = {false, false};
        for (int sd = 0; sd < 2; sd++) {
            float best = 0;
            for (auto& m : pack.meshes) if (m.kind == "head") for (auto& v : m.v) {
                if (v.z < eyeZ - 0.06f || v.z > eyeZ + 0.015f || v.y > eyeY - 0.05f || v.y < eyeY - 0.15f) continue;
                float lx = sd == 0 ? v.x - C.x : C.x - v.x;
                best = std::max(best, lx);
            }
            if (best <= 0) continue;
            earLo[sd] = {1e9f, 1e9f, 1e9f}; earHi[sd] = {-1e9f, -1e9f, -1e9f};
            for (auto& m : pack.meshes) if (m.kind == "head") for (auto& v : m.v) {
                float lx = sd == 0 ? v.x - C.x : C.x - v.x;
                if (lx < best - 0.012f || v.z < eyeZ - 0.09f || v.z > eyeZ + c.value("ear_top_mm", 50.0f) / 1000 || v.y > eyeY - 0.04f || v.y < eyeY - 0.17f) continue;
                earLo[sd] = {std::min(earLo[sd].x, v.x), std::min(earLo[sd].y, v.y), std::min(earLo[sd].z, v.z)};
                earHi[sd] = {std::max(earHi[sd].x, v.x), std::max(earHi[sd].y, v.y), std::max(earHi[sd].z, v.z)};
                hasEar[sd] = true;
            }
        }
        float earPad = c.value("ear_pad_mm", 5.0f) / 1000;
        auto inEar = [&](V3 p) {
            for (int sd = 0; sd < 2; sd++)
                if (hasEar[sd] && p.x > earLo[sd].x - earPad && p.x < earHi[sd].x + earPad && p.y > earLo[sd].y - earPad &&
                    p.y < earHi[sd].y + earPad && p.z > earLo[sd].z - earPad && p.z < earHi[sd].z + earPad) return true;
            // also the column in front of the ear, below its top (strands hanging down past the ear / sideburn)
            for (int sd = 0; sd < 2; sd++) {
                if (!hasEar[sd]) continue;
                float inner = std::min(std::fabs(earLo[sd].x - C.x), std::fabs(earHi[sd].x - C.x)) - 0.015f;
                if (std::fabs(p.x - C.x) > inner && p.y > earLo[sd].y - earPad && p.y < earHi[sd].y + c.value("ear_front_mm", 60.0f) / 1000 &&
                    p.z < earHi[sd].z - c.value("ear_drop_mm", 8.0f) / 1000 && p.z > earLo[sd].z - 0.03f) return true;
            }
            return false;
        };
        float browZ = haveEye ? eHi.z + c.value("brow_clear_mm", 12.0f) / 1000 : C.z;
        auto overFacePt = [&](V3 p) {   // in front of the face below the brows (eyes, cheeks): hair must not hang there
            return p.y > eyeY - 0.03f && p.z < browZ && std::fabs(p.x - C.x) < (haveEye ? (eHi.x - eLo.x) / 2 + 0.035f : 0.07f);
        };
        // part of the hairline a posed point belongs to (azimuth around the head, 0 = front)
        float frontDeg = c.value("front_deg", 32.0f), templeDeg = c.value("temple_deg", 68.0f), earDeg = c.value("ear_deg", 125.0f);
        // azimuth centre: the skull centre (behind the eyes), not pack.headC (bbox centre, near the forehead in y)
        const V3 Ca{C.x, eyeY - c.value("skull_back_mm", 75.0f) / 1000, eyeZ};
        auto partOf = [&](V3 p) -> std::string {
            float az = std::atan2(p.x - Ca.x, p.y - Ca.y) * 180 / 3.14159265f, aa = std::fabs(az);
            if (p.z < eyeZ - 0.07f) return "";   // jaw / neck line
            if (aa <= frontDeg) return "front";
            if (aa <= templeDeg) return az > 0 ? "left_temple" : "right_temple";
            if (aa <= earDeg) return "ears";
            return "";
        };
        auto wanted = [&](const std::string& pp) {
            if (pp.empty()) return false;
            if (part == "all") return true;
            if (part == "temples") return pp == "left_temple" || pp == "right_temple";
            if (part == "sides") return pp != "front";
            return pp == part;
        };
        // stations: hairline points of the wanted part, optionally smoothed (local mean over smooth_mm = a fitted smooth
        // curve), Poisson-thinned at spacing_mm
        std::vector<int> cand;
        for (size_t i = 0; i < hl.pt.size(); i++) if (wanted(partOf(hl.pt[i]))) cand.push_back(int(i));
        struct St { V3 p, in; std::string part; };
        std::vector<St> st;
        for (int i : cand) {
            V3 p = hl.pt[i], in{0, 0, 0};
            if (straight) {
                V3 acc{0, 0, 0}; int n = 0;
                for (int j : cand) if (len(hl.pt[j] - hl.pt[i]) < smoothR) { acc = acc + hl.pt[j]; in = in + nrmz(hl.inner[j] - hl.pt[j]); n++; }
                p = C + nrmz(acc * (1.0f / n) - C) * len(hl.pt[i] - C);
            } else {
                for (int j : cand) if (len(hl.pt[j] - hl.pt[i]) < 0.006f) in = in + nrmz(hl.inner[j] - hl.pt[j]);
            }
            V3 nr = nrmz(p - C);
            in = nrmz(in - nr * dot(in, nr));
            bool close = false;
            for (auto& s : st) if (len(s.p - p) < spacing) { close = true; break; }
            if (!close) st.push_back({p, in, partOf(hl.pt[i])});
        }
        std::vector<int> alive, longs;
        for (size_t g = 0; g < groom.P.size(); g++)
            if (groom.alive[g]) { alive.push_back(int(g)); if (guideLen(groom.P[g]) >= minL) longs.push_back(int(g)); }
        if (longs.empty()) { r["error"] = "no guide longer than min_length"; return r; }
        HeadRadial hrF = headRadial(*this);
        int id = 0, grp = 1000 + (++grpId), n = 0;
        for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
        std::vector<json> ops;
        json planned = json::array();
        std::map<std::string, int> perPart;
        int covered = 0, offScalp = 0, noTpl = 0, through = 0, tuft = 0, face = 0, ear = 0;
        std::vector<V3> newRoots;
        float coverR = c.value("covered_mm", 0.6f * spacing * 1000) / 1000;
        // innermost row first: the growth mesh grows outward row by row (edge faces attach to the previous row)
        for (int ri = rows - 1; ri >= 0; ri--) {
            float o = offs[ri];
            float keep = rows > 1 ? taper + (1.0f - taper) * float(ri) / float(rows - 1) : taper;
            for (auto& s : st) {
                if (n >= maxNew) break;
                V3 qP = s.p + s.in * o;   // posed
                qP = C + nrmz(qP - C) * len(s.p - C);
                if (!onScalp(hl, qP)) { offScalp++; continue; }
                // posed -> edit frame: shift of the nearest guide root
                int g0 = -1; float bd = 1e30f;
                for (int g : alive) { float dd = len(posedPoint(g, 0) - qP); if (dd < bd) { bd = dd; g0 = g; } }
                if (g0 < 0) continue;
                V3 E = qP + (groom.P[g0][0] - posedPoint(g0, 0));
                bool cov = bd < coverR;
                for (auto& q : newRoots) if (len(q - nrmz(E - Ca)) * 0.09f < coverR) cov = true;   // unit directions from the skull centre
                for (int g : alive) if (len(groom.P[g][0] - E) < coverR) { cov = true; break; }
                if (cov) { covered++; continue; }
                float side = E.x - C.x;
                std::vector<std::pair<float, int>> tc;
                for (int g : longs) {
                    V3 rr = groom.P[g][0];
                    float dd = len(rr - E);
                    if (dd > tmax) continue;
                    if (std::fabs(side) > 0.012f && (rr.x - C.x) * side < 0) continue;   // same side of the head
                    // prefer guides rooted farther inside the line (the interior flow, not another edge stray)
                    tc.push_back({dd - 0.3f * std::max(0.0f, dot(rr - E, s.in)), g});
                }
                std::sort(tc.begin(), tc.end());
                if (tc.empty()) { noTpl++; continue; }
                bool done = false;
                int why = 0;
                for (size_t t = 0; t < tc.size() && t < 6 && !done; t++) {
                    int g = tc[t].second;
                    V3 rs = groom.P[g][0];
                    // copy by ROTATION about the skull centre (root -> new root): the strand keeps its lie on the curved
                    // skull (a plain translation down the side makes it flare out or dive in)
                    V3 ua = nrmz(rs - Ca), ub = nrmz(E - Ca);
                    V3 q = Ca + ub * len(rs - Ca);
                    V3 ax = cross(ua, ub);
                    float sn = len(ax), cs = dot(ua, ub);
                    Guide G = groom.P[g];
                    if (sn > 1e-6f) { ax = ax * (1.0f / sn); float ang = std::atan2(sn, cs); for (auto& p : G) p = Ca + rotAxis(p - Ca, ax, ang); }
                    for (auto& p : G) p = p + (q - G[0]);
                    if (keep < 0.999f) G = shortenGuide(G, keep);
                    auto sl = arcLen(G);
                    float r0 = len(G[0] - C);
                    for (size_t k = 1; k < G.size(); k++) {   // hug: root part on the scalp
                        float f = sl[k] / std::max(spanM, 1e-4f);
                        if (f >= 1.0f) break;
                        V3 rel = G[k] - C;
                        float rk = len(rel), rt = r0 + lift * f;
                        if (rk > rt) {
                            V3 dl = C + nrmz(rel) * (rk + (rt - rk) * (1.0f - f)) - G[k];
                            for (size_t j = k; j < G.size(); j++) G[j] = G[j] + dl * (j == k ? 1.0f : std::max(0.0f, 1.0f - (sl[j] - sl[k]) / std::max(spanM, 1e-4f)));
                        }
                    }
                    if (pushOut(hrF, G, 0.003f) > 0) { why = 1; continue; }
                    // tuft: the first tuft_span_mm of the copy stands farther than tuft_mm off the skull surface
                    bool up = false, fc = false, er = false;
                    {
                        auto sl2 = arcLen(G);
                        float s0 = hrF.ok ? hrF.surface(nrmz(G[0] - hrF.c)) : -1;
                        float base = s0 > 0 ? len(G[0] - hrF.c) - s0 : 0;
                        for (size_t k = 1; k < G.size() && sl2[k] < c.value("tuft_span_mm", 40.0f) / 1000; k++) {
                            float sk = hrF.ok ? hrF.surface(nrmz(G[k] - hrF.c)) : -1;
                            if (sk > 0 && len(G[k] - hrF.c) - sk - base > c.value("tuft_mm", 12.0f) / 1000) up = true;
                        }
                    }
                    V3 sh = qP - E;   // edit -> posed for the face / ear boxes
                    for (size_t k = 0; k < G.size(); k++) {   // both frames (the posed / edit shift is not exact): root too, no hair rooted on / in front of the ear
                        if (k && (overFacePt(G[k] + sh) || overFacePt(G[k]))) fc = true;
                        if (inEar(G[k] + sh) || inEar(G[k])) er = true;
                    }   // root too: no hair rooted on / in front of the ear
                    if (up) { why = 2; continue; }
                    if (fc) { why = 3; continue; }
                    if (er) { why = 4; continue; }
                    V3 dd = q - rs;
                    ops.push_back({{"op", "duplicate"}, {"root", {r4(rs.x), r4(rs.y), r4(rs.z)}}, {"d", {r4(dd.x), r4(dd.y), r4(dd.z)}}, {"edge_face", c.value("edge_faces", true)}, {"id", ++id}, {"grp", grp}});
                    ops.push_back({{"op", "reshape"}, {"root", {r4(G[0].x), r4(G[0].y), r4(G[0].z)}}, {"points", ptsJson(G)}, {"id", ++id}, {"grp", grp}});
                    newRoots.push_back(nrmz(E - Ca));
                    if (planned.size() < 400) planned.push_back({{"root", v3j(G[0])}, {"part", s.part}, {"row", ri}, {"template", g}, {"length", std::round(guideLen(G) * 1e4f) / 1e4f}});
                    perPart[s.part]++;
                    n++; done = true;
                }
                if (!done) { if (why == 1) through++; else if (why == 2) tuft++; else if (why == 3) face++; else if (why == 4) ear++; else noTpl++; }
            }
        }
        r["stations"] = st.size(); r["rows"] = rows;
        {
            std::map<std::string, int> hp;
            for (auto& q : hl.pt) { std::string pp = partOf(q); hp[pp.empty() ? "other" : pp]++; }
            r["hairline_points"] = hp; r["skull_c"] = v3j(Ca);
            json eb = json::array();
            for (int sd = 0; sd < 2; sd++) if (hasEar[sd]) eb.push_back({v3j(earLo[sd]), v3j(earHi[sd])});
            r["ear_boxes"] = eb;
        }
        r["per_part"] = perPart;
        r["rejected"] = {{"already_covered", covered}, {"off_scalp", offScalp}, {"no_template_same_side", noTpl}, {"through_head", through},
                         {"sticks_up", tuft}, {"over_face_eyes", face}, {"over_ear", ear}};
        if (dry) { r["would_plant"] = n; r["planned"] = planned; r["status"] = status_(); return r; }
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); smap.ensure(groom.P.size()); editsChanged();
        int dropped = 0;
        computeBudget();
        auto over = [&]() { return bud.overLimit || bud.ratio > maxRatio; };
        while (over() && n > 0 && !c.value("ignore_budget", false)) {   // outermost rows were planted last: they go first
            groom.ops.pop_back(); groom.ops.pop_back();
            n--; dropped++;
            if (dropped % 4 == 0 || n == 0) { groom.replay(); editsChanged(); computeBudget(); }
        }
        if (dropped) { groom.replay(); editsChanged(); computeBudget(); r["dropped_for_budget"] = dropped; r["budget_note"] = "stopped at max_ratio / budget limit"; }
        settleRest(*this, 0.5);
        r["planted"] = n; r["faces"] = groom.faces.size(); r["status"] = status_();
        return r;
    }
    if (d == "regrow" || d == "route_behind_ears" || d == "twist_roll" || d == "plant_fringe" || d == "parting_roots" || d == "unzigzag" || d == "hairnet" || d == "headband" ||
        d == "layer_lift" || d == "inflate") {
        json res = r3Command(*this, c, grpId, d);
        if (res.contains("error")) { r["error"] = res["error"]; return r; }
        smap.ensure(groom.P.size());
        settleRest(*this, 0.5);
        r["result"] = res; r["status"] = status_();
        return r;
    }
    if (d == "free_budget" || d == "fill_below_hairline" || d == "centre_parting") {
        json res = d == "free_budget" ? freeBudget(*this, c, grpId) : d == "fill_below_hairline" ? fillBelowHairline(*this, c, grpId, out) : centreParting(*this, c, grpId);
        if (res.contains("error")) { r["error"] = res["error"]; return r; }
        smap.ensure(groom.P.size());
        settleRest(*this, 0.5);
        r["result"] = res; r["status"] = status_();
        return r;
    }
    if (d == "look_get") {
        json o = json::object();
        auto keys = c.value("keys", json::array());
        if (keys.empty()) {
            // no keys: every look / physics value of the build (path -> current value) + what changed
            r["changed"] = look.exportLook()["set"];
            std::function<void(const json&, const std::string&)> walk = [&](const json& j, const std::string& pre) {
                for (auto& [k, v] : j.items()) {
                    std::string p = pre.empty() ? k : pre + "." + k;
                    if (v.is_object() && !v.contains("Red") && !v.contains("X")) walk(v, p);
                    else o[p] = look.get(p);
                }
            };
            walk(json{{"physicalMaterials", pack.physical}, {"graphicalMaterials", pack.graphical}}, "");
        }
        for (auto& k : keys) {
            std::string ks = k.get<std::string>();
            // a prefix (e.g. "graphicalMaterials.color"): every leaf under it with its CURRENT value
            json node = json{{"physicalMaterials", pack.physical}, {"graphicalMaterials", pack.graphical}};
            bool ok = true;
            size_t s0 = 0;
            while (ok) {
                size_t e = ks.find('.', s0);
                std::string part = ks.substr(s0, e == std::string::npos ? std::string::npos : e - s0);
                if (!node.is_object() || !node.contains(part)) ok = false; else node = json(node[part]);
                if (e == std::string::npos) break;
                s0 = e + 1;
            }
            if (ok && node.is_object() && !node.contains("Red") && !node.contains("X")) {
                std::function<void(const json&, const std::string&)> walk = [&](const json& j, const std::string& pre) {
                    for (auto& [kk, v] : j.items()) {
                        std::string p = pre + "." + kk;
                        if (v.is_object() && !v.contains("Red") && !v.contains("X")) walk(v, p);
                        else o[p] = look.get(p);
                    }
                };
                walk(node, ks);
            } else if (!ok) o[ks] = json{{"error", "unknown path"}};
            else o[ks] = look.get(ks);
        }
        r["values"] = o;
        return r;
    }
    if (d == "materials_list") {
        json l = json::array();
        for (auto& p : presets) l.push_back(p.name);
        r["presets"] = l;
        r["current"] = presetSel < (int)presets.size() ? presets[presetSel].name : "";
        return r;
    }
    if (d == "save_edits") {
        std::string p = c.value("path", out + "\\edits.json");
        std::ofstream(p) << groom.editsText(true);
        r["path"] = p; r["status"] = status_();
        return r;
    }
    // any --agent step; a view / eye camera becomes a temporary pack shot for steps that take "shot"
    json stp = c;
    bool tmpShot = false;
    if ((c.contains("view") || c.contains("eye")) && !c.contains("shot") && !c.contains("shots") && d != "bench") {
        json cam;
        if (cameraOf(*this, c, cam)) { pack.shots["999"] = cam; stp["shot"] = 999; tmpShot = true; }
    }
    int rc = 0;
    {
        BaselineScope bs(*this, c.value("baseline", false));
        r = agentStep(stp, out, rc);
    }
    if (tmpShot) pack.shots.erase("999");
    bool edit = d == "ops" || d == "curl" || d == "delete" || d == "shorten" || d == "duplicate" || d == "look" || d == "material" ||
                d == "hold_curls" || d == "stiffness" || d == "fur";
    if (edit) { smap.ensure(groom.P.size()); settleRest(*this, 0.5); r["status"] = status_(); }
    if (rc) r["error"] = r.value("error", std::string("step failed (code ") + std::to_string(rc) + ")");
    return r;
}

int App::runServe(const std::string& out) {
    fs::create_directories(out);
    if (!sdkOk) { reply({{"error", "HairWorks runtime not loaded: " + status}}); return 3; }
    reply({{"ready", true}, {"out", out}});
    std::string line;
    bool quit = false;
    while (!quit && readLine(line)) {
        if (line.empty()) continue;
        json c, r;
        try {
            c = json::parse(line);
            r = serveCommand(c, out, quit);
        } catch (std::exception& e) {
            r = {{"error", std::string("exception: ") + e.what()}};
        }
        reply(r);
    }
    return 0;
}
