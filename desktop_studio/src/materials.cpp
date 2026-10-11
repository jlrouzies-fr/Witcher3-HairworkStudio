// Desktop Groom Studio - material presets and stiffness map. Our own code (MIT).
#include "materials.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

bool isColourPath(const std::string& p) {
    static const char* c[] = {"graphicalMaterials.color.rootColor", "graphicalMaterials.color.tipColor",
                              "graphicalMaterials.specular.specularColor", "graphicalMaterials.color.textureBrightness",
                              "graphicalMaterials.color.rootColorTex", "graphicalMaterials.color.tipColorTex"};
    for (auto s : c) if (p == s) return true;
    return false;
}

static bool isLeaf(const json& v) {
    if (!v.is_object()) return true;
    if (v.contains("@import")) return true;
    return v.contains("Red") || v.contains("X");     // Color / Vector values are leaves
}

static void flat(const json& j, const std::string& pre, json& out) {
    for (auto& [k, v] : j.items()) {
        std::string p = pre + "." + k;
        if (isLeaf(v)) {
            if (v.is_object() && v.contains("@import")) continue;   // texture handles: never part of a material preset
            out[p] = v;
        } else {
            flat(v, p, out);
        }
    }
}

json flattenMaterial(const json& physical, const json& graphical) {
    json out = json::object();
    if (physical.is_object()) flat(physical, "physicalMaterials", out);
    if (graphical.is_object()) flat(graphical, "graphicalMaterials", out);
    // per-character things that are not "material": density map handling stays with the character
    for (auto k : {"physicalMaterials.volume.densityTexChannel", "physicalMaterials.volume.usePixelDensity",
                   "physicalMaterials.simulation.useCollision"})
        out.erase(k);
    return out;
}

std::vector<MaterialPreset> loadPresets(const DeskPack& pk, const std::string& dir, const json& builtins) {
    std::vector<MaterialPreset> v;
    MaterialPreset cur;
    cur.name = "Character's current (" + pk.config + ")";
    cur.kind = "current";
    cur.values = flattenMaterial(pk.physical, pk.graphical);
    v.push_back(cur);
    if (builtins.is_object())
        for (auto& [name, b] : builtins.items()) {
            MaterialPreset p;
            p.name = name;
            p.kind = "builtin";
            p.values = b.value("values", json::object());
            p.configKeys = b.value("config_keys", json::object());
            v.push_back(p);
        }
    if (fs::exists(dir))
        for (auto& e : fs::directory_iterator(dir)) {
            if (e.path().extension() != ".json") continue;
            try {
                std::ifstream f(e.path());
                json j = json::parse(f);
                MaterialPreset p;
                p.name = j.value("name", e.path().stem().string()) + " (custom)";
                p.kind = "custom";
                p.values = j.value("values", json::object());
                p.configKeys = j.value("config_keys", json::object());
                p.file = e.path().string();
                v.push_back(p);
            } catch (...) {}
        }
    return v;
}

int applyPreset(Look& look, const MaterialPreset& p, bool keepColours) {
    int n = 0;
    // values the preset does not mention go back to the character's build
    std::vector<std::string> drop;
    for (auto& [path, v] : look.over)
        if ((path.rfind("physicalMaterials.", 0) == 0 || path.rfind("graphicalMaterials.", 0) == 0) && !p.values.contains(path) &&
            !(keepColours && isColourPath(path)))
            drop.push_back(path);
    for (auto& d : drop) look.over.erase(d);
    for (auto& [path, v] : p.values.items()) {
        if (keepColours && isColourPath(path)) continue;
        look.setJson(path, v);
        n++;
    }
    look.enforceCaps();
    return n;
}

bool saveCustomPreset(const Look& look, const std::string& name, const std::string& dir, std::string& err) {
    if (name.empty()) { err = "give the preset a name"; return false; }
    std::string safe;
    for (char c : name) safe += (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
    fs::create_directories(dir);
    json all = flattenMaterial(look.pack->physical, look.pack->graphical);
    for (auto& [p, v] : look.over)
        if (p.rfind("physicalMaterials.", 0) == 0 || p.rfind("graphicalMaterials.", 0) == 0) all[p] = v;
    json keys = json::object();
    for (auto& [p, v] : all.items()) {
        if (isColourPath(p)) continue;
        // hairpipe config keys: any material value goes through look.extra_set (typed in the exporter)
        keys["look.extra_set." + p] = v;
    }
    json j = {{"name", name}, {"source", look.pack->config}, {"values", all}, {"config_keys", keys},
              {"note", "Desktop Groom Studio material preset: parameter values only (no third-party files)."}};
    std::ofstream(dir + "\\" + safe + ".json") << j.dump(1);
    return true;
}

// ------------------------------------------------------------------------------------------------ stiffness map
void StiffnessMap::reset(size_t n) {
    guideS.assign(n, 1.0f);
    guideR.assign(n, 1.0f);
    painted = false;
    rgba.assign(size_t(size) * size * 4, 255);
    overlapTexels = 0;
}

void StiffnessMap::ensure(size_t n) {
    if (guideS.size() < n) { guideS.resize(n, 1.0f); guideR.resize(n, 1.0f); }
}

bool StiffnessMap::uniform() const {
    for (size_t i = 0; i < guideS.size(); i++)
        if (std::fabs(guideS[i] - 1.0f) > 1e-3f || std::fabs(guideR[i] - 1.0f) > 1e-3f) return false;
    return true;
}

void UVAtlas::build(const Groom& g) {
    size_t F = g.faces.size();
    int cells = int((F + 1) / 2), n = std::max(1, (int)std::ceil(std::sqrt((double)cells)));
    float c = 1.0f / n, p = 2.0f / size;          // 2 texel padding
    uv.resize(F);
    for (size_t f = 0; f < F; f++) {
        int cell = int(f / 2), cx = cell % n, cy = cell / n;
        float x0 = cx * c + p, y0 = cy * c + p, x1 = (cx + 1) * c - p, y1 = (cy + 1) * c - p;
        std::array<float, 6> t;
        if (f % 2 == 0) t = {x0, y0, x1 - 2 * p, y0, x0, y1 - 2 * p};
        else t = {x1, y1, x0 + 2 * p, y1, x1, y0 + 2 * p};
        // keep the winding sign of the original uv triangle (the redfur convention: signed area < 0)
        auto& o = g.faces[f].uv;
        float so = (o[2] - o[0]) * (o[5] - o[1]) - (o[4] - o[0]) * (o[3] - o[1]);
        float sn = (t[2] - t[0]) * (t[5] - t[1]) - (t[4] - t[0]) * (t[3] - t[1]);
        if ((so < 0) != (sn < 0)) { std::swap(t[2], t[4]); std::swap(t[3], t[5]); }
        uv[f] = t;
    }
}

void UVAtlas::bake(const Groom& g, const unsigned char* src, int sw, int sh, int channel, std::vector<unsigned char>& out) const {
    out.assign(size_t(size) * size * 4, 0);
    for (size_t f = 0; f < g.faces.size() && f < uv.size(); f++) {
        const auto& t = uv[f];
        const auto& o = g.faces[f].uv;
        float u[3] = {t[0] * size, t[2] * size, t[4] * size}, v[3] = {t[1] * size, t[3] * size, t[5] * size};
        float den = (v[1] - v[2]) * (u[0] - u[2]) + (u[2] - u[1]) * (v[0] - v[2]);
        if (std::fabs(den) < 1e-9f) continue;
        int x0 = std::max(0, (int)std::floor(std::min({u[0], u[1], u[2]})) - 2), x1 = std::min(size - 1, (int)std::ceil(std::max({u[0], u[1], u[2]})) + 2);
        int y0 = std::max(0, (int)std::floor(std::min({v[0], v[1], v[2]})) - 2), y1 = std::min(size - 1, (int)std::ceil(std::max({v[0], v[1], v[2]})) + 2);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float a = ((v[1] - v[2]) * (px - u[2]) + (u[2] - u[1]) * (py - v[2])) / den;
                float b = ((v[2] - v[0]) * (px - u[2]) + (u[0] - u[2]) * (py - v[2])) / den;
                float cc = 1 - a - b;
                const float e = -0.12f;     // dilate into the padding
                if (a < e || b < e || cc < e) continue;
                float su = a * o[0] + b * o[2] + cc * o[4], sv = a * o[1] + b * o[3] + cc * o[5];
                su -= std::floor(su);
                sv -= std::floor(sv);
                int sx = std::clamp(int(su * sw), 0, sw - 1), sy = std::clamp(int(sv * sh), 0, sh - 1);
                const unsigned char* s = src + (size_t(sy) * sw + sx) * 4;
                unsigned char* d = &out[(size_t(y) * size + x) * 4];
                if (channel < 0) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
                else { d[0] = d[1] = d[2] = d[3] = s[channel]; }
            }
    }
}

void StiffnessMap::rasterise(const Groom& g, const std::vector<std::array<float, 6>>* uvs) {
    ensure(g.P.size());
    std::vector<float> acc(size_t(size) * size * 2, 0.0f), wsum(size_t(size) * size, 0.0f);
    std::vector<float> mn(size_t(size) * size, 9.0f), mx(size_t(size) * size, -9.0f);
    for (size_t fi = 0; fi < g.faces.size(); fi++) {
        const Face& f = g.faces[fi];
        const auto& fuv = (uvs && fi < uvs->size()) ? (*uvs)[fi] : f.uv;
        float u[3], v[3], s[3], r[3];
        for (int c = 0; c < 3; c++) {
            u[c] = (fuv[2 * c] - std::floor(fuv[2 * c])) * size;
            v[c] = (fuv[2 * c + 1] - std::floor(fuv[2 * c + 1])) * size;
            int gi = f.g[c];
            s[c] = guideS[gi];
            r[c] = guideR[gi];
        }
        int x0 = std::max(0, (int)std::floor(std::min({u[0], u[1], u[2]})) - 1), x1 = std::min(size - 1, (int)std::ceil(std::max({u[0], u[1], u[2]})) + 1);
        int y0 = std::max(0, (int)std::floor(std::min({v[0], v[1], v[2]})) - 1), y1 = std::min(size - 1, (int)std::ceil(std::max({v[0], v[1], v[2]})) + 1);
        float den = (v[1] - v[2]) * (u[0] - u[2]) + (u[2] - u[1]) * (v[0] - v[2]);
        if (std::fabs(den) < 1e-9f) continue;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float a = ((v[1] - v[2]) * (px - u[2]) + (u[2] - u[1]) * (py - v[2])) / den;
                float b = ((v[2] - v[0]) * (px - u[2]) + (u[0] - u[2]) * (py - v[2])) / den;
                float c = 1 - a - b;
                const float e = uvs ? -0.02f : -0.08f;   // dilation (smaller in the unique atlas: cells are separate)
                if (a < e || b < e || c < e) continue;
                size_t i = size_t(y) * size + x;
                float sv = a * s[0] + b * s[1] + c * s[2], rv = a * r[0] + b * r[1] + c * r[2];
                acc[2 * i] += sv;
                acc[2 * i + 1] += rv;
                wsum[i] += 1;
                mn[i] = std::min(mn[i], sv);
                mx[i] = std::max(mx[i], sv);
            }
    }
    rgba.assign(size_t(size) * size * 4, 255);
    overlapTexels = 0;
    for (size_t i = 0; i < wsum.size(); i++) {
        if (wsum[i] <= 0) continue;
        if (mx[i] - mn[i] > 0.15f) overlapTexels++;
        rgba[4 * i + 0] = (unsigned char)std::clamp(std::lround(acc[2 * i] / wsum[i] * 255.0f), 0L, 255L);
        rgba[4 * i + 1] = (unsigned char)std::clamp(std::lround(acc[2 * i + 1] / wsum[i] * 255.0f), 0L, 255L);
        rgba[4 * i + 2] = 255;
    }
    painted = !uniform();
}
