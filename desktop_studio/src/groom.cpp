// Desktop Groom Studio - guide edit ops. Our own code (MIT).
// Semantics follow tools/hairworks/strand_edits.py (docstring): guides are matched by ROOT position (edit frame,
// metres) within `tol`; ops run in order; duplicates join at the end of the list.
#include "groom.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#include "curls.h"

double r4(double v) { return std::round(v * 10000.0) / 10000.0; }

static std::vector<float> falloffWeights(int K, const std::string& falloff, int from) {
    std::vector<float> w(K, 1.0f);
    for (int k = 0; k < K; k++) {
        float t = K > 1 ? float(k) / float(K - 1) : 1.0f;
        if (falloff == "linear") w[k] = t;
        else if (falloff == "tip") w[k] = t * t;
        if (k < from) w[k] = 0.0f;
    }
    return w;
}

static std::vector<V3> resample(const std::vector<V3>& q, int K) {
    std::vector<float> d(q.size(), 0.0f);
    for (size_t i = 1; i < q.size(); i++) d[i] = d[i - 1] + len(q[i] - q[i - 1]);
    std::vector<V3> out(K, q[0]);
    if (d.back() < 1e-9f) return out;
    for (int k = 0; k < K; k++) {
        float t = d.back() * k / float(K - 1);
        size_t j = 1;
        while (j < q.size() - 1 && d[j] < t) j++;
        float a = (t - d[j - 1]) / std::max(d[j] - d[j - 1], 1e-12f);
        a = std::clamp(a, 0.0f, 1.0f);
        out[k] = q[j - 1] + (q[j] - q[j - 1]) * a;
    }
    return out;
}

std::vector<V3> shortenGuide(const Guide& p, float keep) {
    int K = (int)p.size();
    std::vector<float> d(K, 0.0f);
    for (int i = 1; i < K; i++) d[i] = d[i - 1] + len(p[i] - p[i - 1]);
    float L = d.back() * std::clamp(keep, 0.02f, 1.0f);
    int k = int(std::lower_bound(d.begin(), d.end(), L) - d.begin());
    if (k >= K) return p;
    k = std::max(k, 1);
    float t = (L - d[k - 1]) / std::max(d[k] - d[k - 1], 1e-12f);
    std::vector<V3> q(p.begin(), p.begin() + k);
    q.push_back(p[k - 1] + (p[k] - p[k - 1]) * t);
    return resample(q, K);
}

void Groom::init(const DeskPack* p) {
    pack = p;
    N0 = (int)p->guidesEdit.size();
    K = p->K;
    ops.clear();
    redo.clear();
    replay();
}

int Groom::baseOf(int i) const {
    int b = i;
    int guard = 0;
    while (b >= N0 && src[b] >= 0 && guard++ < 100000) b = src[b];
    return b < N0 ? b : 0;
}

V3 Groom::renderPoint(int i, int k) const {
    int b = baseOf(i);
    V3 d = pack->guides[b][k] - pack->guidesEdit[b][k];
    return P[i][k] + d;
}

Guide Groom::renderGuide(int i) const {
    Guide g(K);
    for (int k = 0; k < K; k++) g[k] = renderPoint(i, k);
    return g;
}

int Groom::numAlive() const {
    int n = 0;
    for (char a : alive) n += a ? 1 : 0;
    return n;
}

std::vector<int> Groom::match(const json& roots) {
    std::vector<int> out;
    for (auto& r : roots) {
        V3 q{r[0].get<float>(), r[1].get<float>(), r[2].get<float>()};
        int best = -1;
        float bd = 1e9f;
        for (size_t i = 0; i < P.size(); i++) {
            if (!alive[i]) continue;
            float d = len(P[i][0] - q);
            if (d < bd) { bd = d; best = (int)i; }
        }
        if (best >= 0 && bd <= tol) {
            if (std::find(out.begin(), out.end(), best) == out.end()) out.push_back(best);
        } else {
            unmatched++;
        }
    }
    return out;
}

// growth faces of deleted guides are removed; each hole (closed boundary loop, kept in the faces' winding) is fanned
void Groom::removeGuides(const std::vector<int>& dead) {
    for (int i : dead) alive[i] = 0;
    std::vector<Face> keep, gone;
    for (auto& f : faces) {
        if (alive[f.g[0]] && alive[f.g[1]] && alive[f.g[2]]) keep.push_back(f);
        else gone.push_back(f);
    }
    std::map<std::pair<int, int>, int> cnt;
    std::map<int, std::array<float, 2>> uvOf;
    for (auto& f : gone)
        for (int e = 0; e < 3; e++) {
            int a = f.g[e], b = f.g[(e + 1) % 3];
            cnt[{a, b}]++;
            if (alive[a] && !uvOf.count(a)) uvOf[a] = {f.uv[2 * e], f.uv[2 * e + 1]};
        }
    std::map<int, int> next;
    for (auto& f : gone)
        for (int e = 0; e < 3; e++) {
            int a = f.g[e], b = f.g[(e + 1) % 3];
            if (!alive[a] || !alive[b]) continue;
            if (cnt[{a, b}] == 1 && !cnt.count({b, a})) next[a] = b;
        }
    // the dead guide's ring: edges between two alive neighbours of a removed face are the hole boundary
    std::set<int> used;
    for (auto& [s, n0] : next) {
        if (used.count(s)) continue;
        std::vector<int> loop{s};
        used.insert(s);
        int cur = n0;
        bool closed = false;
        for (int guard = 0; guard < 400; guard++) {
            if (cur == s) { closed = true; break; }
            if (used.count(cur) || !next.count(cur)) break;
            loop.push_back(cur);
            used.insert(cur);
            cur = next[cur];
        }
        if (!closed || loop.size() < 3) continue;
        for (size_t i = 1; i + 1 < loop.size(); i++) {
            Face f;
            f.g = {loop[0], loop[i], loop[i + 1]};
            for (int c = 0; c < 3; c++) {
                auto uv = uvOf[f.g[c]];
                f.uv[2 * c] = uv[0];
                f.uv[2 * c + 1] = uv[1];
            }
            keep.push_back(f);
        }
    }
    faces.swap(keep);
}

static std::array<float, 3> closestBary(V3 p, V3 a, V3 b, V3 c, float& dist) {
    // projection onto the triangle plane, barycentrics clamped to the triangle
    V3 n = cross(b - a, c - a);
    float nn = dot(n, n);
    if (nn < 1e-14f) { dist = 1e9f; return {1, 0, 0}; }
    V3 q = p - n * (dot(p - a, n) / nn);
    float w2 = dot(cross(b - a, q - a), n) / nn;
    float w0 = dot(cross(c - b, q - b), n) / nn;
    float w1 = 1.0f - w0 - w2;
    w0 = std::max(w0, 0.0f); w1 = std::max(w1, 0.0f); w2 = std::max(w2, 0.0f);
    float s = w0 + w1 + w2;
    w0 /= s; w1 /= s; w2 /= s;
    V3 r = a * w0 + b * w1 + c * w2;
    dist = len(p - r);
    return {w0, w1, w2};
}

// a duplicated guide splits the growth face its root lands on into three, so HairWorks interpolates around it
void Groom::insertGuide(int ni) {
    V3 r = P[ni][0];
    int best = -1;
    float bd = 1e9f;
    std::array<float, 3> bw{};
    for (size_t f = 0; f < faces.size(); f++) {
        auto& F = faces[f];
        float d;
        auto w = closestBary(r, P[F.g[0]][0], P[F.g[1]][0], P[F.g[2]][0], d);
        if (d < bd) { bd = d; best = (int)f; bw = w; }
    }
    if (best < 0 || bd > 0.02f) return;
    Face F = faces[best];
    float u = bw[0] * F.uv[0] + bw[1] * F.uv[2] + bw[2] * F.uv[4];
    float v = bw[0] * F.uv[1] + bw[1] * F.uv[3] + bw[2] * F.uv[5];
    faces.erase(faces.begin() + best);
    for (int e = 0; e < 3; e++) {
        Face n;
        int a = e, b = (e + 1) % 3;
        n.g = {F.g[a], F.g[b], ni};
        n.uv = {F.uv[2 * a], F.uv[2 * a + 1], F.uv[2 * b], F.uv[2 * b + 1], u, v};
        faces.push_back(n);
    }
}

// ------------------------------------------------------------------------------------------------ mirror (op semantics of
// tools/hairworks/strand_edits.py: source side copied across x = plane_x, target side deleted, centre band kept)
void Groom::buildHead() {
    if (!headV.empty()) return;
    for (auto& m : pack->meshes) {
        if (m.kind != "head" && m.name != "head") continue;
        headV = m.v;
        headN.assign(m.v.size(), V3{0, 0, 0});
        for (auto& t : m.t) {
            V3 n = cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]]);
            for (int c = 0; c < 3; c++) headN[t[c]] = headN[t[c]] + n;
        }
        for (size_t i = 0; i < headN.size(); i++) {
            float l = len(headN[i]);
            headN[i] = l > 1e-12f ? headN[i] * (1.0f / l) : V3{0, 0, 1};
            if (dot(headV[i] - pack->headC, headN[i]) < 0) headN[i] = headN[i] * -1.0f;
        }
        break;
    }
}

float Groom::headSigned(V3 p, V3* nrm) const {
    if (headV.empty()) { if (nrm) *nrm = V3{0, 0, 1}; return 1.0f; }
    size_t best = 0;
    float bd = 1e9f;
    for (size_t i = 0; i < headV.size(); i++) { float d = dot(p - headV[i], p - headV[i]); if (d < bd) { bd = d; best = i; } }
    if (nrm) *nrm = headN[best];
    return dot(p - headV[best], headN[best]);
}

float Groom::mirrorPlane() {
    if (planeCache < 1e8f) return planeCache;
    buildHead();
    if (headV.empty()) return planeCache = 0.0f;
    std::vector<V3> cr;
    float top = -1e9f;
    for (auto& v : headV) top = std::max(top, v.z);
    for (auto& v : headV) if (v.z > top - 0.12f) cr.push_back(v);
    float bestE = 1e9f, bestC = 0;
    for (float c = -0.02f; c <= 0.02001f; c += 0.0005f) {
        double e = 0;
        for (auto& v : cr) {
            V3 m{2 * c - v.x, v.y, v.z};
            float bd = 1e9f;
            for (auto& w : cr) bd = std::min(bd, dot(m - w, m - w));
            e += std::sqrt(bd);
        }
        if (e < bestE) { bestE = (float)e; bestC = c; }
    }
    return planeCache = bestC;
}

void Groom::applyMirror(const json& op) {
    buildHead();
    float px = op.contains("plane_x") && op["plane_x"].is_number() ? op["plane_x"].get<float>() : mirrorPlane();
    float sgn = op.value("source", std::string("+x"))[0] == '-' ? -1.0f : 1.0f;
    float band = op.value("band", 0.015f), clear = op.value("clear", 0.002f);
    auto inside = [&](V3 r) {
        if (op.contains("ymin") && r.y < op["ymin"].get<float>()) return false;
        if (op.contains("ymax") && r.y > op["ymax"].get<float>()) return false;
        if (op.contains("zmin") && r.z < op["zmin"].get<float>()) return false;
        if (op.contains("zmax") && r.z > op["zmax"].get<float>()) return false;
        return true;
    };
    std::vector<int> src, gone, centre;
    std::vector<int> side(P.size(), 0);   // +1 source, -1 target, 0 centre / untouched
    for (size_t i = 0; i < P.size(); i++) {
        if (!alive[i]) continue;
        float off = (P[i][0].x - px) * sgn;
        if (std::fabs(off) <= band || !inside(P[i][0])) continue;
        if (off > 0) { src.push_back((int)i); side[i] = 1; } else { gone.push_back((int)i); side[i] = -1; }
    }
    std::map<int, int> copyOf;
    for (int i : src) {
        Guide p = P[i], q = p;
        std::vector<float> seg(K, 0.0f);
        for (int k = 1; k < K; k++) seg[k] = len(p[k] - p[k - 1]);
        for (auto& v : q) v.x = 2 * px - v.x;
        if (!headV.empty()) {
            V3 n0, n1;
            float sd0 = headSigned(p[0], &n0), sd1 = headSigned(q[0], &n1);
            for (auto& v : q) v = v - n1 * (sd1 - sd0);              // root back on the scalp, same offset as the source
            bool low = false;
            for (int k = 1; k < K; k++) {
                V3 nk;
                float s = headSigned(q[k], &nk);
                if (s < clear) { q[k] = q[k] + nk * (clear - s); low = true; }
            }
            if (low)
                for (int k = 1; k < K; k++) q[k] = q[k - 1] + [&] { V3 d = q[k] - q[k - 1]; float l = len(d); return l > 1e-12f ? d * (1.0f / l) : d; }() * seg[k];
        }
        P.push_back(q);
        alive.push_back(1);
        this->src.push_back(i);
        copyOf[i] = (int)P.size() - 1;
        edited.insert((int)P.size() - 1);
    }
    // faces: target-side faces go; faces of source + centre guides are mirrored (copies, reversed winding)
    std::vector<Face> keep;
    for (auto& f : faces) {
        bool tgt = side[f.g[0]] < 0 || side[f.g[1]] < 0 || side[f.g[2]] < 0;
        if (!tgt) keep.push_back(f);
        bool hasSrc = side[f.g[0]] > 0 || side[f.g[1]] > 0 || side[f.g[2]] > 0;
        if (hasSrc && !tgt) {
            Face m = f;
            for (int c = 0; c < 3; c++) if (side[f.g[c]] > 0) m.g[c] = copyOf[f.g[c]];
            std::swap(m.g[1], m.g[2]);
            std::swap(m.uv[2], m.uv[4]);
            std::swap(m.uv[3], m.uv[5]);
            keep.push_back(m);
        }
    }
    faces.swap(keep);
    for (int i : gone) alive[i] = 0;
}

void Groom::applyOp(const json& op) {
    std::string kind = op.contains("type") ? op["type"].get<std::string>() : op.value("op", "");
    if (kind == "mirror") { applyMirror(op); return; }
    if (kind == "duplicate") {
        if (!op.contains("root")) return;
        auto g = match(json::array({op["root"]}));
        if (g.empty()) return;
        V3 d{0, 0, 0};
        if (op.contains("d")) d = {op["d"][0].get<float>(), op["d"][1].get<float>(), op["d"][2].get<float>()};
        Guide c = P[g[0]];
        for (auto& p : c) p = p + d;
        P.push_back(c);
        alive.push_back(1);
        src.push_back(g[0]);
        int ni = (int)P.size() - 1;
        edited.insert(ni);
        insertGuide(ni);
        return;
    }
    json roots = op.contains("roots") ? op["roots"] : (op.contains("root") ? json::array({op["root"]}) : json::array());
    if (roots.empty()) return;
    auto g = match(roots);
    if (g.empty()) return;
    if (kind == "curl") {
        CurlParams c = CurlParams::fromJson(op.value("params", json::object()));
        for (int i : g) {
            json r = {r4(P[i][0].x), r4(P[i][0].y), r4(P[i][0].z)};
            unsigned seed = unsigned(std::fabs(P[i][0].x) * 1e5f) * 73856093u ^ unsigned(std::fabs(P[i][0].y) * 1e5f) * 19349663u ^
                            unsigned(std::fabs(P[i][0].z) * 1e5f) * 83492791u;
            P[i] = applyCurl(P[i], c, pack->headC, pack->headTop, seed);
            json pts = json::array();
            for (auto& q : P[i]) pts.push_back({r4(q.x), r4(q.y), r4(q.z)});
            baked.push_back({{"type", "reshape"}, {"root", r}, {"points", pts}});
            edited.insert(i);
        }
        return;
    }
    if (kind == "reshape") {
        int i = g[0];
        auto& pts = op["points"];
        if ((int)pts.size() == K)
            for (int k = 0; k < K; k++) P[i][k] = {pts[k][0].get<float>(), pts[k][1].get<float>(), pts[k][2].get<float>()};
        g = {i};
    } else if (kind == "translate") {
        V3 off{op["offset"][0].get<float>(), op["offset"][1].get<float>(), op["offset"][2].get<float>()};
        auto w = falloffWeights(K, op.value("falloff", "carry"), op.value("from", 0));
        for (int i : g)
            for (int k = 0; k < K; k++) P[i][k] = P[i][k] + off * w[k];
    } else if (kind == "set_point") {
        int i = g[0];
        int k = std::clamp(op.value("index", 0), 0, K - 1);
        V3 pos{op["pos"][0].get<float>(), op["pos"][1].get<float>(), op["pos"][2].get<float>()};
        V3 dl = pos - P[i][k];
        int end = op.value("carry", true) ? K : k + 1;
        for (int j = k; j < end; j++) P[i][j] = P[i][j] + dl;
        g = {i};
    } else if (kind == "shorten") {
        for (int i : g) P[i] = shortenGuide(P[i], op.value("keep", 0.8f));
    } else if (kind == "delete") {
        removeGuides(g);
    } else {
        return;
    }
    for (int i : g) edited.insert(i);
}

void Groom::replay() {
    P = pack->guidesEdit;
    alive.assign(P.size(), 1);
    src.assign(P.size(), -1);
    edited.clear();
    unmatched = 0;
    faces.clear();
    for (size_t f = 0; f < pack->F.size(); f++) faces.push_back({pack->F[f], pack->uv[f]});
    baked.clear();
    for (auto& o : preOps) applyOp(o);
    for (auto& o : ops) {
        std::string k = o.contains("type") ? o["type"].get<std::string>() : o.value("op", "");
        if (k != "curl") baked.push_back(o);
        applyOp(o);
    }
}

void Groom::push(json op) {
    redo.clear();
    ops.push_back(std::move(op));
    replay();
}

void Groom::undo() {
    if (ops.empty()) return;
    json o = ops.back();
    ops.pop_back();
    redo.push_back(o);
    if (o.contains("grp"))
        while (!ops.empty() && ops.back().contains("grp") && ops.back()["grp"] == o["grp"]) { redo.push_back(ops.back()); ops.pop_back(); }
    replay();
}

void Groom::redoOp() {
    if (redo.empty()) return;
    json o = redo.back();
    redo.pop_back();
    ops.push_back(o);
    if (o.contains("grp"))
        while (!redo.empty() && redo.back().contains("grp") && redo.back()["grp"] == o["grp"]) { ops.push_back(redo.back()); redo.pop_back(); }
    replay();
}

json Groom::rootsOf(const std::vector<int>& ix) const {
    json a = json::array();
    for (int i : ix) a.push_back({r4(P[i][0].x), r4(P[i][0].y), r4(P[i][0].z)});
    return a;
}

json Groom::editsDoc(bool bake) const {
    json d;
    d["version"] = 1;
    d["character"] = pack->character;
    d["source"] = pack->config;
    d["comp"] = pack->comp;
    d["tol"] = std::round(double(tol) * 1e6) / 1e6;
    d["note"] = "Strand Editor export (Desktop Groom Studio). Copy to tools/hairworks/edits/" + pack->character + "_<tag>.json";
    json all = json::array();
    for (auto& o : preOps) all.push_back(o);
    for (auto& o : (bake ? baked : ops)) {
        json c = o;
        c.erase("grp");        // editor-only grouping
        all.push_back(c);
    }
    d["ops"] = all;
    return d;
}

std::string Groom::editsText(bool bake) const {
    json d = editsDoc(bake);
    json opsArr = d["ops"];
    d.erase("ops");
    std::string h = d.dump();
    h.pop_back();
    std::ostringstream s;
    s << h << ",\"ops\":[";
    for (size_t i = 0; i < opsArr.size(); i++) s << (i ? ",\n  " : "\n  ") << opsArr[i].dump();
    s << (opsArr.empty() ? "" : "\n") << "]}\n";
    return s.str();
}
