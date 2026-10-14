// Desktop Groom Studio - desk pack loader. Our own code (MIT).
#include "deskpack.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

float len(V3 a) { return std::sqrt(dot(a, a)); }

static V3 v3(const json& a) { return {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()}; }

static void guidesFrom(const json& j, std::vector<Guide>& out) {
    out.clear();
    out.reserve(j.size());
    for (auto& g : j) {
        Guide gg;
        gg.reserve(g.size());
        for (auto& p : g) gg.push_back(v3(p));
        out.push_back(std::move(gg));
    }
}

int DeskPack::boneIndex(const std::string& n) const {
    for (size_t i = 0; i < bones.size(); i++)
        if (bones[i].name == n) return (int)i;
    return -1;
}

bool DeskPack::load(const std::string& d, std::string& err) {
    dir = d;
    std::ifstream f(d + "\\desk.json", std::ios::binary);
    if (!f) { err = "cannot open " + d + "\\desk.json (run desk_prep.py first)"; return false; }
    json j;
    try { j = json::parse(f); } catch (std::exception& e) { err = std::string("desk.json: ") + e.what(); return false; }
    if (j.value("format", "") != "deskpack") { err = "not a desk pack"; return false; }
    config = j.value("config", "");
    character = j.value("character", "");
    comp = j.value("comp", "");
    furPath = j.value("fur_path", "");
    editPath = j["edit_path"].is_string() ? j["edit_path"].get<std::string>() : "";
    K = j["K"];
    splineMultiplier = j.value("splineMultiplier", 1);
    guidesFrom(j["guides"], guides);
    guidesFrom(j["guides_edit"], guidesEdit);
    for (auto& g : guides) g.resize(K, g.back());
    for (auto& g : guidesEdit) g.resize(K, g.back());
    for (auto& t : j["F"]) F.push_back({t[0], t[1], t[2]});
    for (auto& u : j["uv"]) { std::array<float, 6> a; for (int i = 0; i < 6; i++) a[i] = u[i]; uv.push_back(a); }
    for (auto& b : j["bone_idx"]) boneIdx.push_back({(float)b[0], (float)b[1], (float)b[2], (float)b[3]});
    for (auto& b : j["bone_w"]) boneW.push_back({(float)b[0], (float)b[1], (float)b[2], (float)b[3]});
    for (auto& b : j["bones"]) {
        Bone B;
        B.name = b["name"];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) B.bind[r * 4 + c] = b["bind"][r][c];
        B.parent = b.value("parent", -1);
        bones.push_back(B);
    }
    for (auto& s : j["spheres"]) {
        spheres.push_back({s["bone"], v3(s["local"]), s["r"]});
        if (s.contains("c")) { spheres.back().c = v3(s["c"]); spheres.back().hasC = true; }
    }
    for (auto& c : j["capsules"]) capsules.push_back({c[0], c[1]});
    physical = j["physical"];
    graphical = j["graphical"];
    lod = j["lod"].is_null() ? json::object() : j["lod"];
    mod = j["mod"];
    budgetRef = j["budget_ref"];
    budgetSelf = j["budget_self"];
    textures = j["textures"];
    lookCfg = j.value("look_cfg", json::object());
    {
        json rc = graphical.is_object() && graphical.contains("color") ? graphical["color"].value("rootColorTex", json()) : json();
        if (rc.is_object() && rc.contains("@import") && textures.contains(rc["@import"].get<std::string>()))
            colorFile = textures[rc["@import"].get<std::string>()].value("file", "");
    }
    if (j.contains("density") && j["density"].is_object()) {
        auto& dd = j["density"];
        densityFile = dd.value("file", "");
        densityChannel = dd.value("channel", "RED");
        densityPixel = dd.value("pixel", false);
        for (auto& v : dd["face"]) densityFace.push_back(v);
    }
    for (auto& m : j["meshes"]) {
        Mesh M;
        M.name = m.value("name", "");
        M.kind = m.value("kind", "");
        M.path = m.value("path", "");
        for (auto& p : m["v"]) M.v.push_back(v3(p));
        for (auto& t : m["t"]) M.t.push_back({t[0], t[1], t[2]});
        meshes.push_back(std::move(M));
    }
    if (j.contains("head_tex") && j["head_tex"].is_object()) {
        auto& h = j["head_tex"];
        Mesh M;
        M.name = "head_textured";
        M.kind = "head";
        M.texFile = h.value("file", "");
        if (h.contains("normal") && h["normal"].is_string()) M.nrmFile = h["normal"].get<std::string>();
        for (auto& p : h["v"]) M.v.push_back(v3(p));
        for (auto& u : h["uv"]) M.uv.push_back({u[0].get<float>(), u[1].get<float>()});
        for (auto& t : h["t"]) M.t.push_back({t[0], t[1], t[2]});
        // the decimated EDIT head is replaced by the real one
        meshes.erase(std::remove_if(meshes.begin(), meshes.end(), [](const Mesh& m) { return m.kind == "head"; }), meshes.end());
        meshes.insert(meshes.begin(), std::move(M));
    }
    if (j.contains("outfit_tex") && j["outfit_tex"].is_array() && !j["outfit_tex"].empty()) {
        // textured LOD0 outfit parts replace the decimated untextured outfit meshes they come from
        std::vector<std::string> done;
        for (auto& o : j["outfit_tex"]) {
            Mesh M;
            M.name = o.value("name", "");
            M.path = o.value("path", "");
            M.kind = "outfit";
            M.texFile = o.value("file", "");
            if (o.contains("normal") && o["normal"].is_string()) M.nrmFile = o["normal"].get<std::string>();
            for (auto& p : o["v"]) M.v.push_back(v3(p));
            for (auto& u : o["uv"]) M.uv.push_back({u[0].get<float>(), u[1].get<float>()});
            for (auto& t : o["t"]) M.t.push_back({t[0], t[1], t[2]});
            done.push_back(M.path);
            meshes.push_back(std::move(M));
        }
        // keep untextured parts only for outfit meshes that produced no textured part (w2mi-only materials)
        meshes.erase(std::remove_if(meshes.begin(), meshes.end(), [&](const Mesh& m) {
                         return m.kind == "outfit" && m.texFile.empty() && std::find(done.begin(), done.end(), m.path) != done.end();
                     }), meshes.end());
    }
    // vanilla beard / moustache cards (desk_prep beard_cards, 2026-10-11): alpha-tested textured parts
    for (const char* key : {"beard_cards", "hair_cards"}) {
        if (!j.contains(key) || !j[key].is_array()) continue;
        for (auto& o : j[key]) {
            Mesh M;
            M.name = o.value("name", "");
            M.path = o.value("path", "");
            M.kind = std::string(key) == "hair_cards" ? "hair_cards" : "beard";
            M.texFile = o.value("file", "");
            M.alphaTest = o.value("alpha", true);
            M.alphaRef = o.value("alpha_ref", 0.3f);
            for (auto& p : o["v"]) M.v.push_back(v3(p));
            for (auto& u : o["uv"]) M.uv.push_back({u[0].get<float>(), u[1].get<float>()});
            for (auto& t : o["t"]) M.t.push_back({t[0], t[1], t[2]});
            if (!M.v.empty() && M.uv.size() == M.v.size()) meshes.push_back(std::move(M));
        }
    }
    beardFur = j.contains("beard_fur") && j["beard_fur"].is_object() ? j["beard_fur"] : json();
    if (j.contains("head")) { headC = v3(j["head"]["c"]); headTop = j["head"]["top"]; }
    if (guides.empty() || guides.size() != guidesEdit.size()) { err = "pack has no guides"; return false; }
    if (boneIdx.size() != guides.size()) {
        boneIdx.assign(guides.size(), {0, 0, 0, 0});
        boneW.assign(guides.size(), {1, 0, 0, 0});
    }
    return true;
}

bool DeskPack::beardPack(DeskPack& o) const {
    if (!beardFur.is_object() || !beardFur.contains("guides")) return false;
    const json& b = beardFur;
    o = DeskPack();
    o.dir = dir;
    o.config = config + " (beard)";
    o.character = character;
    o.furPath = b.value("fur_path", "");
    if (o.furPath.empty() || o.furPath == furPath) return false;   // a beard-only build: its beard IS the groom
    {   // component = file name after "<cfg>__" (hb_01_da__zoltan): the export finds the beard's in-game redfur by it
        std::string fn = o.furPath.substr(o.furPath.find_last_of("\\/") + 1);
        size_t s = fn.find("__"), e = fn.rfind(".redfur");
        o.comp = s == std::string::npos ? "" : fn.substr(s + 2, (e == std::string::npos ? fn.size() : e) - s - 2);
    }
    guidesFrom(b["guides"], o.guides);
    if (o.guides.empty()) return false;
    o.K = 0;
    for (auto& g : o.guides) o.K = std::max(o.K, (int)g.size());
    for (auto& g : o.guides) g.resize(o.K, g.back());
    o.guidesEdit = o.guides;
    for (auto& t : b["F"]) o.F.push_back({t[0], t[1], t[2]});
    for (auto& u : b["uv"]) { std::array<float, 6> a; for (int i = 0; i < 6; i++) a[i] = u[i]; o.uv.push_back(a); }
    for (auto& v : b["bone_idx"]) o.boneIdx.push_back({(float)v[0], (float)v[1], (float)v[2], (float)v[3]});
    for (auto& v : b["bone_w"]) o.boneW.push_back({(float)v[0], (float)v[1], (float)v[2], (float)v[3]});
    for (auto& bb : b["bones"]) {
        Bone B;
        B.name = bb["name"];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) B.bind[r * 4 + c] = bb["bind"][r][c];
        B.parent = bb.value("parent", -1);
        o.bones.push_back(B);
    }
    o.physical = b.value("physical", json::object());
    o.graphical = b.value("graphical", json::object());
    o.lod = json::object();
    o.textures = textures;
    o.headC = headC;
    o.headTop = headTop;
    if (o.boneIdx.size() != o.guides.size()) {
        o.boneIdx.assign(o.guides.size(), {0, 0, 0, 0});
        o.boneW.assign(o.guides.size(), {1, 0, 0, 0});
    }
    return !o.F.empty();
}

void swapFur(DeskPack& a, DeskPack& b) {
    using std::swap;
    swap(a.comp, b.comp); swap(a.furPath, b.furPath); swap(a.editPath, b.editPath);
    swap(a.K, b.K); swap(a.splineMultiplier, b.splineMultiplier);
    swap(a.guides, b.guides); swap(a.guidesEdit, b.guidesEdit); swap(a.F, b.F); swap(a.uv, b.uv);
    swap(a.boneIdx, b.boneIdx); swap(a.boneW, b.boneW); swap(a.bones, b.bones);
    swap(a.spheres, b.spheres); swap(a.capsules, b.capsules);
    swap(a.physical, b.physical); swap(a.graphical, b.graphical); swap(a.lod, b.lod);
    swap(a.budgetRef, b.budgetRef); swap(a.budgetSelf, b.budgetSelf); swap(a.lookCfg, b.lookCfg);
    swap(a.densityFile, b.densityFile); swap(a.densityChannel, b.densityChannel); swap(a.colorFile, b.colorFile);
    swap(a.densityPixel, b.densityPixel); swap(a.densityFace, b.densityFace);
}
