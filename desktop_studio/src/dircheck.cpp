// Direction guard (2026-10-16, user rule: "agents / tools should NEVER allow strands going wrong direction").
// checkDirection: every alive guide's tip part is checked
//   - "up": hanging hair (points below head centre + up_above_mm) whose tip segments rise (dz > up_thr);
//   - "against": tip segments heading against the mean direction of the guides rooted within nb_mm (dot < against_thr);
//   - with "wind": the same "up" test on the simulated guides (GPU read-back) under wind (heading wind_yaw_deg, strength
//     wind_strength) after wind_s seconds, counted when a guide rises in >= 1/3 of the sampled steps.
//   Curly guides (tip part turning more than curl_turn_deg in total) are not judged: curls rise on purpose.
// fixDirection: offending guides get the mean shape of their non-offending neighbours (relative to the root, at their own
// length) from fix_from (0.25) of the length on, blended in, plus a gravity floor on hanging tip segments; plain reshape
// ops in the group grp (the caller's undo step).
// Serve guard (runServe): the rest test runs before and after every command that adds ops; guides that became offenders
// through the command are fixed in the same undo step ("fix", default), or the command is undone ("refuse"), or only
// reported ("report"); "off" skips. Deliberate shape tools (bun, braid, curl, twist, fur) are not guarded.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <fstream>
#include <unordered_map>
#include "app.h"

namespace {
std::vector<float> arcL(const Guide& G) {
    std::vector<float> s(G.size(), 0);
    for (size_t k = 1; k < G.size(); k++) s[k] = s[k - 1] + len(G[k] - G[k - 1]);
    return s;
}
V3 nz(V3 v) { float l = len(v); return l > 1e-9f ? v * (1.0f / l) : V3{0, 0, 0}; }
V3 dirAt(const Guide& G, const std::vector<float>& sl, float s) {
    size_t k = 1;
    while (k + 1 < G.size() && sl[k] < s) k++;
    return nz(G[k] - G[k - 1]);
}
// roots on a hash grid (cell = radius): neighbours within r
struct RootGrid {
    float cell;
    std::unordered_map<long long, std::vector<int>> m;
    static long long key(int x, int y, int z) { return ((long long)(x + 4096) << 26) | ((long long)(y + 4096) << 13) | (long long)(z + 4096); }
    RootGrid(const Groom& g, float r) : cell(std::max(r, 1e-3f)) {
        for (size_t i = 0; i < g.P.size(); i++) {
            if (!g.alive[i] || g.P[i].empty()) continue;
            const V3& p = g.P[i][0];
            m[key(int(std::floor(p.x / cell)), int(std::floor(p.y / cell)), int(std::floor(p.z / cell)))].push_back((int)i);
        }
    }
    template <class F> void within(V3 p, float r, const Groom& g, F f) const {
        int cx = int(std::floor(p.x / cell)), cy = int(std::floor(p.y / cell)), cz = int(std::floor(p.z / cell));
        for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
            auto it = m.find(key(cx + dx, cy + dy, cz + dz));
            if (it == m.end()) continue;
            for (int j : it->second) { float d = len(g.P[j][0] - p); if (d <= r) f(j, d); }
        }
    }
};
long long rootKey(V3 p) {   // 0.2 mm quantised root position (guide identity across replays)
    auto q = [](float v) { return (long long)std::llround(v * 5000.0f) & 0x1FFFFF; };
    return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
}
}  // namespace

// "hanging" part of the head: below head centre + up_above, or off the top cap (elevation seen from the head centre
// below top_cap = sin 45 deg: the back / sides / front of the skull, where hair must go down, not up)
static bool hangingAt(V3 p, V3 hc, float upAbove, float topCap) {
    if (p.z < hc.z + upAbove) return true;
    V3 r = p - hc; float l = len(r);
    return l > 1e-6f && r.z / l < topCap;
}

struct DirParams {
    float tipPart = 0.5f, upThr = 0.35f, upAbove = 0.02f, topCap = 0.7f, againstThr = -0.2f, nb = 0.02f, windUpThr = 0.3f, windTurn = 0.5f, curlTurn = 5.2f;
    explicit DirParams(const json& P) {
        tipPart = P.value("tip_part", 0.5f); upThr = P.value("up_thr", 0.35f); upAbove = P.value("up_above_mm", 20.0f) / 1000;
        topCap = P.value("top_cap", 0.7f);
        againstThr = P.value("against_thr", -0.2f); nb = P.value("nb_mm", 20.0f) / 1000; windUpThr = P.value("wind_up_thr", 0.3f);
        windTurn = P.value("wind_turn", 0.5f); curlTurn = P.value("curl_turn_deg", 300.0f) * 3.14159265f / 180.0f;
    }
};

// total turning angle of the tip part (radians): curls turn far more than a hook
static float tipTurn(const Guide& G, const std::vector<float>& s, float tipPart) {
    float L = s.back(), t = 0;
    for (size_t k = 2; k < G.size(); k++) {
        if (s[k - 1] < L * (1 - tipPart)) continue;
        V3 a = nz(G[k - 1] - G[k - 2]), b = nz(G[k] - G[k - 1]);
        t += std::acos(std::clamp(dot(a, b), -1.0f, 1.0f));
    }
    return t;
}

// rest offenders: guide -> reason ("up" / "against")
static std::map<int, std::string> restOffenders(const App& a, const DirParams& D, const std::vector<int>* only = nullptr) {
    std::map<int, std::string> out;
    const Groom& g = a.groom;
    const V3 hc = a.pack.headC;
    size_t N = g.P.size();
    std::vector<std::vector<float>> sl(N);
    for (size_t i = 0; i < N; i++) if (g.alive[i] && g.P[i].size() >= 3) sl[i] = arcL(g.P[i]);
    RootGrid grid(g, D.nb);
    std::vector<int> ids;
    if (only) ids = *only; else for (size_t i = 0; i < N; i++) ids.push_back((int)i);
    for (int i : ids) {
        if (i < 0 || i >= (int)N || !g.alive[i] || g.P[i].size() < 3) continue;
        const Guide& G = g.P[i];
        float L = sl[i].back();
        if (L < 0.03f) continue;   // short hair (hairline fuzz, stubble): direction not judged
        if (tipTurn(G, sl[i], D.tipPart) > D.curlTurn) continue;   // curls rise on purpose
        std::vector<int> nbs;
        grid.within(G[0], D.nb, g, [&](int j, float) { if (j != i && !sl[j].empty()) nbs.push_back(j); });
        int up = 0, ag = 0, tot = 0;
        for (size_t k = 1; k < G.size(); k++) {
            if (sl[i][k] < L * (1 - D.tipPart)) continue;
            V3 d = nz(G[k] - G[k - 1]);
            tot++;
            if (hangingAt(G[k], hc, D.upAbove, D.topCap) && d.z > D.upThr) up++;
            V3 m{0, 0, 0};   // neighbours' mean direction at the same arc length
            for (int j : nbs) {
                if (sl[j].back() < sl[i][k] * 0.7f) continue;
                m = m + dirAt(g.P[j], sl[j], sl[i][k]);
            }
            if (len(m) > 1e-6f && dot(d, nz(m)) < D.againstThr) ag++;
        }
        if (tot && up * 3 >= tot && up >= 1) out[i] = "up";
        else if (tot && ag * 2 > tot) out[i] = "against";
    }
    return out;
}

// re-comb: mean shape of the non-offending neighbours (relative, at this guide's length), from fix_from of the length on;
// hanging tip segments get a gravity floor (dz <= -min_drop)
static json recombOps(App& a, const std::set<int>& bad, const json& P, int grp, int& id, int& fixedN) {
    json ops = json::array();
    Groom& g = a.groom;
    const V3 hc = a.pack.headC;
    float nb = P.value("fix_nb_mm", 25.0f) / 1000, from = P.value("fix_from", 0.25f), minDrop = P.value("min_drop", 0.2f);
    float upAbove = P.value("up_above_mm", 20.0f) / 1000, topCap = P.value("top_cap", 0.7f);
    const int M = 24;
    auto resample = [&](const Guide& G) {
        auto s = arcL(G); float L = std::max(s.back(), 1e-4f);
        std::vector<V3> o(M); size_t k = 1;
        for (int q = 0; q < M; q++) {
            float t = L * q / (M - 1);
            while (k + 1 < G.size() && s[k] < t) k++;
            float f = std::clamp((t - s[k - 1]) / std::max(s[k] - s[k - 1], 1e-6f), 0.0f, 1.0f);
            o[q] = (G[k - 1] + (G[k] - G[k - 1]) * f - G[0]) * (1.0f / L);
        }
        return o;
    };
    RootGrid grid(g, nb * 3.01f);   // cell covers the widest search radius
    for (int i : bad) {
        const Guide& G = g.P[i];
        auto s = arcL(G); float L = s.back();
        std::vector<V3> m(M, V3{0, 0, 0}); float ws = 0;
        // non-offending neighbours within nb; widened (x2, x3) while fewer than 3 are found (clusters of offenders)
        for (float rad = nb; rad <= nb * 3.01f; rad += nb) {
            std::fill(m.begin(), m.end(), V3{0, 0, 0}); ws = 0; int cnt = 0;
            float sig = rad * 0.5f;
            grid.within(G[0], rad, g, [&](int j, float d) {
                if (j == i || bad.count(j) || g.P[j].size() < 3) return;
                float w = std::exp(-0.5f * (d / sig) * (d / sig));
                auto r = resample(g.P[j]);
                for (int q = 0; q < M; q++) m[q] = m[q] + r[q] * w;
                ws += w; cnt++;
            });
            if (cnt >= 3) break;
        }
        Guide R = G;
        if (ws > 0) {
            std::vector<V3> Pm(M), own = resample(G);
            for (int q = 0; q < M; q++) {
                float u = float(q) / (M - 1);
                float w = std::clamp((u - from) / std::max(1 - from, 1e-3f) * 2.0f, 0.0f, 1.0f);   // own root part, neighbours' tip
                Pm[q] = G[0] + (own[q] * (1 - w) + m[q] * (w / ws)) * L;
            }
            std::vector<float> ps(M, 0);
            for (int q = 1; q < M; q++) ps[q] = ps[q - 1] + len(Pm[q] - Pm[q - 1]);
            float L2 = std::max(ps.back(), 1e-4f);
            size_t j = 1;
            for (size_t k = 1; k < G.size(); k++) {
                float t = s[k] / L * L2;
                while (j + 1 < (size_t)M && ps[j] < t) j++;
                float f = std::clamp((t - ps[j - 1]) / std::max(ps[j] - ps[j - 1], 1e-6f), 0.0f, 1.0f);
                R[k] = Pm[j - 1] + (Pm[j] - Pm[j - 1]) * f;
            }
        }
        // constrained re-walk (own segment lengths) with the gravity floor on hanging tip segments
        for (size_t k = 1; k < G.size(); k++) {
            float seg = s[k] - s[k - 1];
            V3 d = nz(R[k] - R[k - 1]);
            if (len(d) < 1e-6f) d = nz(G[k] - G[k - 1]);
            if (s[k] > L * from && hangingAt(R[k - 1], hc, upAbove, topCap) && d.z > -minDrop) {
                float h = std::sqrt(std::max(0.0f, d.x * d.x + d.y * d.y));
                float hz = std::sqrt(std::max(0.0f, 1 - minDrop * minDrop));
                d = h > 1e-6f ? V3{d.x / h * hz, d.y / h * hz, -minDrop} : V3{0, 0, -1};
            }
            R[k] = R[k - 1] + d * seg;
        }
        json pts = json::array();
        for (auto& p : R) pts.push_back({p.x, p.y, p.z});
        ops.push_back({{"op", "reshape"}, {"root", {G[0].x, G[0].y, G[0].z}}, {"points", pts}, {"id", ++id}, {"grp", grp}});
        fixedN++;
    }
    return ops;
}

static int maxOpId(const Groom& g) {
    int id = 0;
    for (auto& o : g.ops) if (o.contains("id") && o["id"].is_number_integer()) id = std::max(id, o["id"].get<int>());
    return id;
}

json App::checkDirection(const json& P) {
    json r;
    DirParams D(P);
    auto rest = restOffenders(*this, D);
    json lr = json::array();
    int nUp = 0, nAg = 0;
    for (auto& [i, why] : rest) {
        (why == "up" ? nUp : nAg)++;
        if (lr.size() < 300) lr.push_back({{"i", i}, {"why", why}, {"root", {groom.P[i][0].x, groom.P[i][0].y, groom.P[i][0].z}}});
    }
    r["rest_offenders"] = rest.size(); r["rest_up"] = nUp; r["rest_against"] = nAg; r["rest_list"] = lr;
    std::set<int> windBad;
    if (P.value("wind", true) && sdkOk) {
        // wind heading wind_yaw_deg (degrees clockwise from +y = face: 15 = from behind toward the front), strength
        // wind_strength (5 = toolbar max)
        float yaw0 = motion.windYawDeg, pit0 = motion.windPitchDeg, ws0 = motion.windStrength;
        bool sim0 = simulate, wind0 = windOn, paused0 = paused;
        motion.windYawDeg = P.value("wind_yaw_deg", 15.0f); motion.windPitchDeg = P.value("wind_pitch_deg", 2.85f);
        float wind = P.value("wind_strength", 5.0f);
        int steps = std::max(30, int(P.value("wind_s", 3.0f) * 60));
        float sp0 = motion.speed, am0 = motion.amount; MotionPreset pr0 = motion.preset;
        rebuildHair();
        simulate = true; paused = false; windOn = true;
        // wind_speed = gust clock speed (the Studio's motion speed; 1 = gusts every ~6 s)
        motion.preset = MotionPreset::Rest; motion.speed = P.value("wind_speed", 1.0f); motion.amount = 1; motion.windStrength = wind; motion.t = 0; simAcc = 0; teleport = true;
        std::string dumpPath = P.value("dump_path", std::string());
        json dump = json::object();
        if (!dumpPath.empty()) {
            json rest = json::array(), idx = json::array();
            for (size_t aI = 0; aI < guideOfAsset.size(); aI++) {
                int g = guideOfAsset[aI]; idx.push_back(g);
                json pts = json::array();
                if (g >= 0 && g < (int)groom.P.size()) for (int k = 0; k < groom.K; k++) { V3 q = posedPoint(g, k); pts.push_back({q.x, q.y, q.z}); }
                rest.push_back(pts);
            }
            dump["guide_of_asset"] = idx; dump["rest"] = rest; dump["K"] = groom.K; dump["head_c"] = {pack.headC.x, pack.headC.y, pack.headC.z};
            dump["samples"] = json::array();
        }
        const int K = groom.K;
        std::vector<int> upCount(groom.P.size(), 0);
        int samples = 0;
        std::vector<float> xyz; int PG = 0;
        for (int s = 0; s < steps; s++) {
            step(1.0 / 60.0);
            if (s < steps / 3 || s % 10) continue;   // after the ramp, every 10th step
            if (!hw.readGuides(xyz, PG) || PG < 2) continue;
            samples++;
            int spl = K > 1 ? std::max(1, (PG - 1) / (K - 1)) : 1;
            if (!dumpPath.empty()) {
                json sm = json::array();
                for (size_t aI = 0; aI < guideOfAsset.size(); aI++) {
                    json pts = json::array();
                    for (int j = 0; j < PG; j += spl) pts.push_back({xyz[(aI * PG + j) * 3], xyz[(aI * PG + j) * 3 + 1], xyz[(aI * PG + j) * 3 + 2]});
                    sm.push_back(pts);
                }
                dump["samples"].push_back({{"step", s}, {"pg", PG}, {"spl", spl}, {"guides", sm}});
            }
            for (size_t aI = 0; aI < guideOfAsset.size(); aI++) {
                int g = guideOfAsset[aI];
                if (g < 0 || g >= (int)groom.P.size() || !groom.alive[g]) continue;
                std::vector<V3> c;
                for (int j = 0; j < PG; j += spl) c.push_back({xyz[(aI * PG + j) * 3], xyz[(aI * PG + j) * 3 + 1], xyz[(aI * PG + j) * 3 + 2]});
                if (c.size() < 3) continue;
                float L = 0; for (size_t k = 1; k < c.size(); k++) L += len(c[k] - c[k - 1]);
                if (L < 0.03f) continue;
                float acc = 0; int up = 0, tot = 0;
                for (size_t k = 1; k < c.size(); k++) {
                    acc += len(c[k] - c[k - 1]);
                    if (acc < L * 0.2f) continue;
                    tot++;
                    V3 d = nz(c[k] - c[k - 1]);
                    // rising, and turned up against its rest direction (any height: crown tufts standing up count too)
                    float dzRest = k < (size_t)K ? nz(posedPoint(g, (int)k) - posedPoint(g, (int)k - 1)).z : d.z;
                    if (d.z > D.windUpThr && d.z - dzRest > D.windTurn) up++;
                }
                if (tot && up * 3 >= tot && up >= 1) upCount[g]++;
            }
        }
        for (size_t g = 0; g < upCount.size(); g++) if (samples && upCount[g] * 3 >= samples) windBad.insert((int)g);
        if (!dumpPath.empty()) { std::ofstream(dumpPath) << dump.dump(); r["dump_path"] = dumpPath; }
        motion.windYawDeg = yaw0; motion.windPitchDeg = pit0; motion.windStrength = ws0; motion.speed = sp0; motion.amount = am0;
        simulate = sim0; windOn = wind0; paused = paused0;
        motion.preset = MotionPreset::Rest; teleport = true;
        (void)pr0;
        for (int i = 0; i < 30; i++) step(1.0 / 60.0);
        json lw = json::array();
        for (int i : windBad) if (lw.size() < 300) lw.push_back({{"i", i}, {"root", {groom.P[i][0].x, groom.P[i][0].y, groom.P[i][0].z}}});
        r["wind_offenders"] = windBad.size(); r["wind_list"] = lw; r["wind_samples"] = samples;
        r["wind_yaw_deg"] = P.value("wind_yaw_deg", 15.0f); r["wind_strength"] = wind;
    }
    if (P.value("fix", false)) {
        std::set<int> bad;
        for (auto& [i, w] : rest) bad.insert(i);
        if (P.value("fix_wind", true)) for (int i : windBad) bad.insert(i);
        if (P.contains("protect")) for (int g : selectGuides(P["protect"])) bad.erase(g);
        int id = maxOpId(groom);
        static int dirGrp = 0;
        int grp = 900000 + (++dirGrp), fixedN = 0;
        json ops = recombOps(*this, bad, P, grp, id, fixedN);
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged();
        r["fixed"] = fixedN;
        auto after = restOffenders(*this, D);
        r["rest_offenders_after_fix"] = after.size();
    }
    return r;
}

// serve guard, before the command: root keys of the current rest offenders (so only NEW ones are blamed on the command)
std::set<long long> App::directionGuardPre() {
    std::set<long long> s;
    if (!loaded) return s;
    DirParams D(json::object());
    for (auto& [i, w] : restOffenders(*this, D)) s.insert(rootKey(groom.P[i][0]));
    return s;
}

// serve guard, after the command: the guides it touched (ops' roots + new guides) that became offenders are fixed in the
// same undo step ("fix"), or the command's ops are dropped ("refuse"), or only reported ("report"). Returns a summary or null.
json App::directionGuard(size_t opsBefore, size_t guidesBefore, const std::string& mode, const std::set<long long>& pre) {
    if (mode == "off" || groom.ops.size() <= opsBefore) return json();
    std::set<int> touched;
    int grp = -1;
    std::set<long long> roots;
    for (size_t i = opsBefore; i < groom.ops.size(); i++) {
        auto& o = groom.ops[i];
        if (o.contains("grp") && o["grp"].is_number_integer()) grp = o["grp"].get<int>();
        if (o.contains("root") && o["root"].is_array() && o["root"].size() == 3)
            roots.insert(rootKey(V3{o["root"][0].get<float>(), o["root"][1].get<float>(), o["root"][2].get<float>()}));
        if (o.value("op", std::string()) == "rebuild") roots.clear(), guidesBefore = 0;   // whole-groom rebuild: check all
    }
    for (size_t g = 0; g < groom.P.size(); g++) {
        if (!groom.alive[g] || groom.P[g].empty()) continue;
        if (g >= guidesBefore || roots.count(rootKey(groom.P[g][0]))) touched.insert((int)g);
    }
    if (touched.empty()) return json();
    std::vector<int> ids(touched.begin(), touched.end());
    DirParams D(json::object());
    auto bad = restOffenders(*this, D, &ids);
    std::set<int> fresh;
    int preexisting = 0;
    for (auto& [i, w] : bad) { if (pre.count(rootKey(groom.P[i][0]))) preexisting++; else fresh.insert(i); }
    json r = {{"checked", ids.size()}, {"new_offenders", fresh.size()}, {"pre_existing", preexisting}};
    if (fresh.empty() || mode == "report") {
        json l = json::array();
        for (int i : fresh) if (l.size() < 50) l.push_back({{"i", i}, {"why", bad[i]}});
        r["list"] = l;
        return r;
    }
    if (mode == "refuse") {
        groom.ops.resize(opsBefore);
        groom.redo.clear(); groom.replay(); editsChanged();
        r["refused"] = true;
        return r;
    }
    int id = maxOpId(groom), fixedN = 0;
    if (grp < 0) grp = 900000;
    for (int pass = 0; pass < 2 && !fresh.empty(); pass++) {
        json ops = recombOps(*this, fresh, json::object(), grp, id, fixedN);
        for (auto& o : ops) groom.ops.push_back(o);
        groom.redo.clear(); groom.replay(); editsChanged();
        std::vector<int> f(fresh.begin(), fresh.end());
        auto again = restOffenders(*this, D, &f);
        fresh.clear();
        for (auto& [i, w] : again) fresh.insert(i);
    }
    r["fixed"] = fixedN;
    r["remaining"] = fresh.size();
    return r;
}
