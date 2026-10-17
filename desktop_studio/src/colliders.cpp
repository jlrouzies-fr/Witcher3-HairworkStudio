// Hairwork Studio - collider fit / edit / clearance report (2026-10-14). Our own code (Studio License).
//
// The fur's collision spheres + capsules are pack.spheres / pack.capsules (bone, bone-local offset, radius). This file:
//  - fits them to the geometry shown (outfit meshes, hood, scabbard, sword) by running desktop_studio/tools/desk_colfit.py
//    (the pipeline's own fitters, so a Studio fit and a build-time fit of the same meshes agree),
//  - edits them by hand (add / move / resize / delete / mirror / link, undo / redo),
//  - keeps the change in the project ("colliders") and in the geo json of the export ("colliders": desk_export.set_colliders),
//  - reports the clearance (guides ending inside a collider, capsules poking out of the skin).
// HairWorks collides with capsules only (sphere pairs): a sphere that is not part of a capsule gets a twin 1 mm away and a
// capsule to it, as collision.fit does. Vanilla maximum 85 spheres / 75 capsules (PIPELINE.md).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "app.h"
#include "../third_party/imgui/imgui.h"

namespace fs = std::filesystem;
using namespace DirectX;

static const int kMaxSpheres = 85, kMaxCapsules = 75;

static XMMATRIX bindMat(const Bone& b) {
    XMFLOAT4X4 m;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) m.m[r][c] = b.bind[r * 4 + c];
    return XMLoadFloat4x4(&m);
}

static json v3j(V3 v, float k = 1.0f) { return {std::round(v.x * k * 100) / 100, std::round(v.y * k * 100) / 100, std::round(v.z * k * 100) / 100}; }
static json lj(V3 v) { return {std::round(v.x * 1e5f) / 1e5f, std::round(v.y * 1e5f) / 1e5f, std::round(v.z * 1e5f) / 1e5f}; }
static V3 jv3(const json& a) { return {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()}; }

V3 App::colLocalToWorld(int bone, V3 l) const {
    XMVECTOR p = XMVector3TransformCoord(XMVectorSet(l.x, l.y, l.z, 1), bindMat(pack.bones[bone]));
    XMFLOAT3 o;
    XMStoreFloat3(&o, p);
    return {o.x, o.y, o.z};
}

V3 App::colWorldToLocal(int bone, V3 c) const {
    XMVECTOR p = XMVector3TransformCoord(XMVectorSet(c.x, c.y, c.z, 1), XMMatrixInverse(nullptr, bindMat(pack.bones[bone])));
    XMFLOAT3 o;
    XMStoreFloat3(&o, p);
    return {o.x, o.y, o.z};
}

// 2026-10-16: a collider follows the bone of the region it sits in. Spheres placed around the head but bound to torso3
// (the old default for new spheres / flats) do not turn with the head in game and eat the hair when Geralt moves
// (geralt_user_1949: 18 headband-ring spheres on torso3). Rule: above the neck bone's origin + 20 mm -> head; within
// 40 mm below it -> neck2 / neck (whichever origin is nearer); else torso3; with the nearest bone origin as the fallback.
int App::colBoneFor(V3 w) const {
    int head = pack.boneIndex("head"), neck = pack.boneIndex("neck"), neck2 = pack.boneIndex("neck2"), torso = pack.boneIndex("torso3");
    auto bz = [&](int b) { return pack.bones[b].bind[14]; };
    float neckZ = neck >= 0 ? bz(neck) : pack.headC.z - 0.09f;
    if (head >= 0 && w.z >= neckZ + 0.02f) return head;
    if (w.z >= neckZ - 0.04f) {
        if (neck2 >= 0 && neck >= 0) return std::fabs(w.z - bz(neck2)) < std::fabs(w.z - bz(neck)) ? neck2 : neck;
        if (neck >= 0) return neck;
    }
    if (torso >= 0) return torso;
    int best = 0; float bd = 1e9f;
    for (size_t b = 0; b < pack.bones.size(); b++) {
        V3 o{pack.bones[b].bind[12], pack.bones[b].bind[13], pack.bones[b].bind[14]};
        float d = len(o - w);
        if (d < bd) { bd = d; best = (int)b; }
    }
    return best;
}

void App::colRecompute(int i) {
    Sphere& s = pack.spheres[i];
    s.c = colLocalToWorld(s.bone, s.local);
    s.hasC = true;
}

V3 App::colPosed(int i) const {
    const Sphere& s = pack.spheres[i];
    V3 c = s.hasC ? s.c : colLocalToWorld(s.bone, s.local);
    if (s.bone < 0 || s.bone >= (int)boneM.size()) return c;
    XMFLOAT3 o;
    XMStoreFloat3(&o, XMVector3TransformCoord(XMVectorSet(c.x, c.y, c.z, 1), XMLoadFloat4x4(&boneM[s.bone])));
    return {o.x, o.y, o.z};
}

// ---------------------------------------------------------------------------------------------------- state <-> json
json App::colToJson() const {
    json sp = json::array(), cp = json::array(), cr = json::object();
    for (auto& s : pack.spheres)
        sp.push_back({{"bone", pack.bones[s.bone].name}, {"local", lj(s.local)}, {"r", std::round(s.r * 1e5f) / 1e5f}, {"grp", s.grp}, {"tag", s.tag}});
    for (auto& c : pack.capsules) cp.push_back({c[0], c[1]});
    for (auto& [i, r] : colR) cr[std::to_string(i)] = r;
    json out = {{"spheres", sp}, {"capsules", cp}, {"collision_r", cr}};
    if (!colFlats.empty()) {
        json fl = json::object();
        for (auto& [k, f] : colFlats)
            fl[std::to_string(k)] = {{"bone", pack.bones[f.bone].name}, {"c", lj(f.c)}, {"w", f.w}, {"h", f.h}, {"t", f.t},
                                     {"yaw", f.yaw}, {"pitch", f.pitch}, {"roll", f.roll}, {"count", f.count}};
        out["flats"] = fl;
    }
    return out;
}

bool App::colFromJson(const json& j, std::string* err) {
    std::vector<Sphere> sp;
    std::vector<std::array<int, 2>> cp;
    for (auto& s : j.value("spheres", json::array())) {
        int b = pack.boneIndex(s.value("bone", std::string()));
        if (b < 0) { if (err) *err = "bone " + s.value("bone", std::string()) + " is not in this fur"; return false; }
        Sphere q{b, jv3(s["local"]), s["r"].get<float>()};
        q.grp = s.value("grp", -1);
        q.tag = s.value("tag", std::string());
        sp.push_back(q);
    }
    for (auto& c : j.value("capsules", json::array())) {
        int a = c[0], b = c[1];
        if (a < 0 || b < 0 || a >= (int)sp.size() || b >= (int)sp.size()) { if (err) *err = "capsule index out of range"; return false; }
        cp.push_back({a, b});
    }
    pack.spheres = sp;
    pack.capsules = cp;
    for (int i = 0; i < (int)pack.spheres.size(); i++) colRecompute(i);
    colR.clear();
    colFlats.clear();
    if (j.contains("flats") && j["flats"].is_object())
        for (auto& [k, v] : j["flats"].items()) {
            ColFlat f;
            f.bone = std::max(0, pack.boneIndex(v.value("bone", std::string())));
            f.c = jv3(v["c"]);
            f.w = v.value("w", 0.2f); f.h = v.value("h", 0.15f); f.t = v.value("t", 0.01f);
            f.yaw = v.value("yaw", 0.0f); f.pitch = v.value("pitch", 0.0f); f.roll = v.value("roll", 0.0f);
            f.count = v.value("count", 0);
            colFlats[std::stoi(k)] = f;
        }
    if (j.contains("collision_r") && j["collision_r"].is_object())
        for (auto& [k, v] : j["collision_r"].items()) { int i = std::stoi(k); if (i >= 0 && i < (int)pack.spheres.size()) colR[i] = v.get<float>(); }
    return true;
}

void App::colPushUndo() {
    colUndoSt.push_back(colToJson());
    if (colUndoSt.size() > 200) colUndoSt.erase(colUndoSt.begin());
    colRedoSt.clear();
}

void App::colChanged() {
    pack.colModified = true;
    colOpsAt = groom.ops.size();
    colSel = colSel >= (int)pack.spheres.size() ? -1 : colSel;
    colSel2 = colSel2 >= (int)pack.spheres.size() ? -1 : colSel2;
    editsChanged();
}

bool App::colUndo() {
    if (colUndoSt.empty()) return false;
    colRedoSt.push_back(colToJson());
    json s = colUndoSt.back();
    colUndoSt.pop_back();
    colFromJson(s);
    colChanged();
    if (colUndoSt.empty() && pack.spheres.size() == pack.spheres0.size()) {}   // stays "modified" (a project may hold other changes)
    status = "Colliders: undo (" + std::to_string(pack.spheres.size()) + " spheres)";
    return true;
}

bool App::colRedo() {
    if (colRedoSt.empty()) return false;
    colUndoSt.push_back(colToJson());
    json s = colRedoSt.back();
    colRedoSt.pop_back();
    colFromJson(s);
    colChanged();
    status = "Colliders: redo (" + std::to_string(pack.spheres.size()) + " spheres)";
    return true;
}

void App::colSetEdit(bool on) {
    if (on == colEdit) return;
    colEdit = on;
    colDrag = false;
    if (on) { showCollision = true; enterEditPose(); }
}

// Ctrl+Z / Ctrl+Y: the collider change wins when no groom op was made after it
bool App::colUndoKey(bool redo) {
    if (redo) return !colRedoSt.empty() && groom.redo.empty() && colRedo();
    return !colUndoSt.empty() && groom.ops.size() == colOpsAt && colUndo();
}

// ---------------------------------------------------------------------------------------------------- primitives
int App::colAddSphere(int bone, V3 world, float r, int grp, const std::string& tag, bool twin) {
    Sphere s{bone, colWorldToLocal(bone, world), r};
    s.grp = grp;
    s.tag = tag;
    pack.spheres.push_back(s);
    int i = (int)pack.spheres.size() - 1;
    colRecompute(i);
    if (twin) {
        Sphere t = pack.spheres[i];
        t.local.x += 0.001f;
        t.tag = tag + " (twin)";
        pack.spheres.push_back(t);
        colRecompute(i + 1);
        pack.capsules.push_back({i, i + 1});
    }
    return i;
}

void App::colRemove(const std::set<int>& del) {
    if (del.empty()) return;
    std::vector<int> map(pack.spheres.size(), -1);
    std::vector<Sphere> sp;
    for (int i = 0; i < (int)pack.spheres.size(); i++) if (!del.count(i)) { map[i] = (int)sp.size(); sp.push_back(pack.spheres[i]); }
    std::vector<std::array<int, 2>> cp;
    for (auto& c : pack.capsules) if (map[c[0]] >= 0 && map[c[1]] >= 0) cp.push_back({map[c[0]], map[c[1]]});
    std::map<int, float> nr;
    for (auto& [i, r] : colR) if (i >= 0 && i < (int)map.size() && map[i] >= 0) nr[map[i]] = r;
    colR = nr;
    pack.spheres = sp;
    pack.capsules = cp;
    colSel = colSel >= 0 && colSel < (int)map.size() ? map[colSel] : -1;
    colSel2 = colSel2 >= 0 && colSel2 < (int)map.size() ? map[colSel2] : -1;
}

// a sphere's twin: the other end of a capsule that sits within 3 mm of it on the same bone and has no other capsule
static std::set<int> twinsOf(const App& a, const std::set<int>& base) {
    std::set<int> out;
    for (int i : base) {
        for (auto& c : a.pack.capsules) {
            int j = c[0] == i ? c[1] : c[1] == i ? c[0] : -1;
            if (j < 0 || j == i || base.count(j)) continue;
            const Sphere &si = a.pack.spheres[i], &sj = a.pack.spheres[j];
            if (si.bone != sj.bone || len(a.colLocalToWorld(si.bone, si.local) - a.colLocalToWorld(sj.bone, sj.local)) > 0.003f) continue;
            int n = 0;
            for (auto& d : a.pack.capsules) if (d[0] == j || d[1] == j) n++;
            if (n == 1) out.insert(j);
        }
    }
    return out;
}

void App::colDeleteSel() {
    if (colSel < 0 || colSel >= (int)pack.spheres.size()) return;
    colPushUndo();
    std::set<int> del{colSel};
    for (int j : twinsOf(*this, del)) del.insert(j);
    colRemove(del);
    colSel = colSel2 = -1;
    colChanged();
    status = "Collider deleted (" + std::to_string(pack.spheres.size()) + " spheres left)";
}

static std::string swapSide(const std::string& n) {
    if (n.rfind("r_", 0) == 0) return "l_" + n.substr(2);
    if (n.rfind("l_", 0) == 0) return "r_" + n.substr(2);
    return n;
}

// the mirror image about the mid-sagittal plane of the head (the Studio's mirror plane), on the opposite-side bone when the
// bone has one (r_shoulder <-> l_shoulder). Returns the new sphere's index, -1 when the sphere sits on the plane.
int App::colMirrorSphere(int i, std::string* why) {
    const Sphere s = pack.spheres[i];
    float px = groom.mirrorPlane();
    V3 c = s.hasC ? s.c : colLocalToWorld(s.bone, s.local);
    if (std::fabs(c.x - px) < 0.003f) { if (why) *why = "on the mirror plane"; return -1; }
    int b = pack.boneIndex(swapSide(pack.bones[s.bone].name));
    if (b < 0) b = s.bone;
    V3 m{2 * px - c.x, c.y, c.z};
    return colAddSphere(b, m, s.r, 2, "mirror of " + std::to_string(i), true);
}

// ---------------------------------------------------------------------------------------------------- fit targets
static std::string repoPy(const App& a, const char* script) {
    return a.set.python + " \"" + a.set.repo + "\\desktop_studio\\tools\\" + script + "\"";
}

bool App::colScan() {
    std::string out;
    int rc = runCapture(repoPy(*this, "desk_colfit.py") + " list " + configName, set.repo, out);
    size_t p = out.find("[{");
    if (rc != 0 || p == std::string::npos) { colMsg = "Could not list the meshes (see Log)"; logf("desk_colfit list: %s", out.c_str()); return false; }
    try { colTargets = json::parse(out.substr(p, out.find('\n', p) - p)); } catch (std::exception& e) { colMsg = std::string("list: ") + e.what(); return false; }
    colPick.clear();
    for (auto& t : colTargets) colPick[t.value("id", "")] = t.value("default", true);
    colMsg = std::to_string(colTargets.size()) + " meshes found";
    return true;
}

// ---------------------------------------------------------------------------------------------------- fit
json App::colFit(const json& p) {
    json r;
    if (!loaded || fur == 1) { r["error"] = "open a build in Hair mode first"; return r; }
    if (colTargets.empty() && !colScan()) { r["error"] = colMsg; return r; }
    std::vector<std::string> want;
    if (p.contains("meshes") && p["meshes"].is_array() && !p["meshes"].empty()) {
        for (auto& m : p["meshes"]) {
            std::string k = m.get<std::string>(), kl = k;
            std::transform(kl.begin(), kl.end(), kl.begin(), ::tolower);
            if (kl == "all" || kl == "default") {
                for (auto& t : colTargets) if (kl == "all" || t.value("default", true)) want.push_back(t["id"]);
                continue;
            }
            std::string found;
            for (auto& t : colTargets) {
                std::string id = t["id"], lb = t["label"], il = id, ll = lb;
                std::transform(il.begin(), il.end(), il.begin(), ::tolower);
                std::transform(ll.begin(), ll.end(), ll.begin(), ::tolower);
                if (id == k || il == kl || ll == kl) { found = id; break; }
            }
            if (found.empty()) { r["error"] = "unknown mesh '" + k + "'"; json ids = json::array(); for (auto& t : colTargets) ids.push_back(t["id"]); r["available"] = ids; return r; }
            want.push_back(found);
        }
    } else {
        for (auto& [k, v] : colPick) if (v) want.push_back(k);
    }
    if (want.empty()) { r["error"] = "no mesh ticked"; return r; }
    std::string dir = set.repo + "\\build\\desktop_studio\\" + configName;
    // hair points: every 2nd CV of every guide, fur frame (the frame of the colliders)
    {
        json pts = json::array();
        for (size_t i = 0; i < groom.P.size(); i++) {
            if (!groom.alive[i]) continue;
            Guide g = groom.renderGuide((int)i);
            for (size_t k = 0; k < g.size(); k += 2) pts.push_back({std::round(g[k].x * 1e4f) / 1e4f, std::round(g[k].y * 1e4f) / 1e4f, std::round(g[k].z * 1e4f) / 1e4f});
        }
        std::ofstream(dir + "\\colfit_hair.json") << pts.dump();
    }
    json req;
    req["cfg"] = configName;
    req["pack_dir"] = dir;
    req["targets"] = want;
    req["budget"] = {{"spheres", std::min(p.value("max_spheres", kMaxSpheres), kMaxSpheres)}, {"capsules", std::min(p.value("max_capsules", kMaxCapsules), kMaxCapsules)}};
    req["pad_mm"] = p.contains("pad_mm") && p["pad_mm"].is_number() ? json(p["pad_mm"]) : json(colPad);
    req["reach_mm"] = p.contains("reach_mm") && p["reach_mm"].is_number() ? json(p["reach_mm"]) : (colReachCfg ? json() : json(colReach));
    req["keep_manual"] = p.value("keep_manual", true);
    req["hair"] = dir + "\\colfit_hair.json";
    json sp = json::array(), cp = json::array();
    for (size_t i = 0; i < pack.spheres.size(); i++) {
        const Sphere& s = pack.spheres[i];
        V3 c = s.hasC ? s.c : colLocalToWorld(s.bone, s.local);
        float rr = colR.count((int)i) ? colR[(int)i] : s.r;
        sp.push_back({{"bone", s.bone}, {"local", lj(s.local)}, {"c", lj(c)}, {"r", rr}, {"grp", s.grp < 0 ? json() : json(s.grp)}, {"tag", s.tag}});
    }
    for (auto& c : pack.capsules) cp.push_back({c[0], c[1]});
    req["spheres"] = sp;
    req["capsules"] = cp;
    std::ofstream(dir + "\\colfit_req.json") << req.dump();
    std::string out;
    std::string cmd = repoPy(*this, "desk_colfit.py") + " fit \"" + dir + "\\colfit_req.json\" \"" + dir + "\\colfit_res.json\"";
    logf("> %s", cmd.c_str());
    status = "Fitting the colliders (the first fit of a build also runs the pipeline once: ~1 min)...";
    int rc = runCapture(cmd, set.repo, out);
    json res;
    try { std::ifstream f(dir + "\\colfit_res.json"); res = json::parse(f); } catch (...) {}
    if (rc != 0 || !res.is_object() || !res.value("ok", false)) {
        r["error"] = res.is_object() && res.contains("error") ? res["error"] : json("desk_colfit failed: " + out.substr(std::max<int>(0, (int)out.size() - 400)));
        colMsg = "Fit failed: " + r["error"].get<std::string>();
        logf("%s", colMsg.c_str());
        return r;
    }
    colPushUndo();
    std::vector<Sphere> ns;
    for (auto& s : res["spheres"]) {
        Sphere q{s["bone"].get<int>(), jv3(s["local"]), s["r"].get<float>()};
        q.grp = s.value("grp", 1);
        q.tag = s.value("tag", std::string());
        ns.push_back(q);
    }
    pack.spheres = ns;
    pack.capsules.clear();
    for (auto& c : res["capsules"]) pack.capsules.push_back({c[0].get<int>(), c[1].get<int>()});
    for (int i = 0; i < (int)pack.spheres.size(); i++) colRecompute(i);
    colR.clear();
    colSel = colSel2 = -1;
    colChanged();
    colReportJ = res["report"];
    colReportJ["notes"] = res.value("notes", json::array());
    r["report"] = colReportJ;
    r["spheres"] = (int)pack.spheres.size();
    r["capsules"] = (int)pack.capsules.size();
    colMsg = "Fitted: " + std::to_string(pack.spheres.size()) + " spheres, " + std::to_string(pack.capsules.size()) + " capsules";
    status = colMsg;
    return r;
}

// ---------------------------------------------------------------------------------------------------- list
json App::colList() {
    json sp = json::array();
    std::vector<std::vector<int>> caps(pack.spheres.size());
    for (auto& c : pack.capsules) { caps[c[0]].push_back(c[1]); if (c[1] != c[0]) caps[c[1]].push_back(c[0]); }
    int n[3] = {0, 0, 0}, nu = 0;
    json misbound = json::array();
    for (size_t i = 0; i < pack.spheres.size(); i++) {
        const Sphere& s = pack.spheres[i];
        V3 c = s.hasC ? s.c : colLocalToWorld(s.bone, s.local);
        float rr = colR.count((int)i) ? colR[(int)i] : s.r;
        sp.push_back({{"i", i}, {"bone", pack.bones[s.bone].name}, {"grp", s.grp}, {"tag", s.tag}, {"local_mm", v3j(s.local, 1000)}, {"c_mm", v3j(c, 1000)},
                      {"r_mm", std::round(rr * 1e4f) / 10}, {"capsules_with", caps[i]}});
        int want = colBoneFor(c);   // 2026-10-16: a sphere bound to a bone far from where it sits does not follow that body part in game
        if (want != s.bone && !(pack.bones[s.bone].name == "neck" || pack.bones[s.bone].name == "neck2") == !(pack.bones[want].name == "neck" || pack.bones[want].name == "neck2")) {
            sp.back()["bone_suggested"] = pack.bones[want].name;
            misbound.push_back({{"i", (int)i}, {"bone", pack.bones[s.bone].name}, {"suggested", pack.bones[want].name}, {"c_mm", v3j(c, 1000)}, {"tag", s.tag}});
        }
        if (s.grp >= 0 && s.grp <= 2) n[s.grp]++; else nu++;
        if (caps[i].empty()) {}
    }
    int inactive = 0;
    for (size_t i = 0; i < pack.spheres.size(); i++) if (caps[i].empty()) inactive++;
    json cp = json::array();
    for (auto& c : pack.capsules) cp.push_back({c[0], c[1]});
    json flatsJ = json::array();
    for (auto& [k, f] : colFlats) {
        json ix = json::array();
        for (int i = 0; i < (int)pack.spheres.size(); i++) if (colFlatOf(i) == k) ix.push_back(i);
        flatsJ.push_back({{"tag", "flat#" + std::to_string(k)}, {"flat", k}, {"bone", pack.bones[f.bone].name}, {"centre_mm", v3j(f.c, 1000)},
                          {"width_mm", f.w * 1000}, {"height_mm", f.h * 1000}, {"thickness_mm", f.t * 1000}, {"yaw_deg", f.yaw},
                          {"pitch_deg", f.pitch}, {"roll_deg", f.roll}, {"count", f.count}, {"capsules", (int)ix.size() / 2}, {"sphere_indices", ix}});
    }
    return {{"spheres", sp}, {"capsules", cp}, {"counts", {{"spheres", pack.spheres.size()}, {"capsules", pack.capsules.size()}, {"limit_spheres", kMaxSpheres}, {"limit_capsules", kMaxCapsules},
                                                           {"base", n[0]}, {"outfit_or_fitted", n[1]}, {"manual", n[2]}, {"unclassified", nu}, {"without_capsule", inactive}}},
            {"modified", pack.colModified}, {"edit_mode", colEdit}, {"selected", colSel}, {"flats", flatsJ}, {"misbound", misbound}, {"misbound_count", misbound.size()}};
}

// ---------------------------------------------------------------------------------------------------- flats
// A flat = a panel of `count` parallel capsules side by side across its width, each running along its height, radius =
// thickness. At yaw = pitch = roll = 0 it faces +y (width along x, height along z); rotation = Rz(yaw) Rx(pitch) Ry(roll).
static V3 flatRot(V3 p, float yaw, float pitch, float roll) {
    const float d = 3.14159265f / 180;
    float c = std::cos(roll * d), s = std::sin(roll * d);
    p = {p.x * c + p.z * s, p.y, -p.x * s + p.z * c};
    c = std::cos(pitch * d), s = std::sin(pitch * d);
    p = {p.x, p.y * c - p.z * s, p.y * s + p.z * c};
    c = std::cos(yaw * d), s = std::sin(yaw * d);
    return {p.x * c - p.y * s, p.x * s + p.y * c, p.z};
}

int App::colFlatOf(int i) const {
    if (i < 0 || i >= (int)pack.spheres.size()) return -1;
    const std::string& t = pack.spheres[i].tag;
    if (t.rfind("flat#", 0) != 0) return -1;
    try { return std::stoi(t.substr(5)); } catch (...) { return -1; }
}

int App::colFlatMax(int count, float w, float t, int except) const {
    int ownS = 0, ownC = 0;
    if (except >= 0) {
        for (int i = 0; i < (int)pack.spheres.size(); i++) if (colFlatOf(i) == except) ownS++;
        for (auto& q : pack.capsules) if (colFlatOf(q[0]) == except || colFlatOf(q[1]) == except) ownC++;
    }
    int freeS = kMaxSpheres - ((int)pack.spheres.size() - ownS), freeC = kMaxCapsules - ((int)pack.capsules.size() - ownC);
    int fit = std::min(freeS / 2, freeC);
    int n = count > 0 ? count : std::max(1, (int)std::lround(w / std::max(1e-4f, 2 * t)));
    return std::min(n, fit);
}

std::string App::colFlatBuild(int id, const ColFlat& f0) {
    ColFlat f = f0;
    f.t = std::max(0.0005f, f.t);
    f.w = std::max(f.w, 2 * f.t);
    f.h = std::max(f.h, 2 * f.t);
    if (f.bone < 0 || f.bone >= (int)pack.bones.size()) return "bone is not in this fur";
    int n = colFlatMax(f0.count, f.w, f.t, colFlats.count(id) ? id : -1);
    if (n < 1) return "the sphere / capsule limit (85 / 75) leaves no room for a flat";
    std::set<int> del;
    for (int i = 0; i < (int)pack.spheres.size(); i++) if (colFlatOf(i) == id) del.insert(i);
    colRemove(del);
    std::string tag = "flat#" + std::to_string(id);
    float hu = f.w / 2 - f.t, hv = std::max(0.0005f, f.h / 2 - f.t);
    int first = (int)pack.spheres.size();
    for (int k = 0; k < n; k++) {
        float u = n == 1 ? 0 : -hu + 2 * hu * k / (n - 1);
        int a = colAddSphere(f.bone, f.c + flatRot({u, 0, -hv}, f.yaw, f.pitch, f.roll), f.t, 2, tag, false);
        int b = colAddSphere(f.bone, f.c + flatRot({u, 0, hv}, f.yaw, f.pitch, f.roll), f.t, 2, tag, false);
        pack.capsules.push_back({a, b});
    }
    f.count = f0.count;
    colFlats[id] = f;
    colSel = first;
    colSel2 = -1;
    return "";
}

// ---------------------------------------------------------------------------------------------------- edit ops
static std::vector<int> idxList(const json& c) {
    std::vector<int> v;
    if (c.contains("indices") && c["indices"].is_array()) for (auto& x : c["indices"]) v.push_back(x.get<int>());
    else if (c.contains("index") && c["index"].is_number_integer()) v.push_back(c["index"].get<int>());
    return v;
}

json App::colEditOp(const json& c) {
    json r;
    std::string op = c.value("op", "");
    int N = (int)pack.spheres.size();
    if (op == "undo") { r["done"] = colUndo(); r["spheres"] = (int)pack.spheres.size(); return r; }
    if (op == "redo") { r["done"] = colRedo(); r["spheres"] = (int)pack.spheres.size(); return r; }
    if (op == "select") { colSel = c.value("index", -1); colSel2 = c.value("index2", -1); r["selected"] = colSel; return r; }
    if (op == "reset") {
        colPushUndo();
        pack.spheres = pack.spheres0;
        pack.capsules = pack.capsules0;
        for (int i = 0; i < (int)pack.spheres.size(); i++) colRecompute(i);
        colR.clear();
        colFlats.clear();
        colSel = colSel2 = -1;
        colChanged();
        pack.colModified = false;
        r["spheres"] = (int)pack.spheres.size();
        return r;
    }
    auto bad = [&](const std::vector<int>& v) { for (int i : v) if (i < 0 || i >= N) return true; return v.empty(); };
    if (op == "add") {
        std::string bn = c.value("bone", std::string());
        int b = c.contains("bone") && c["bone"].is_number_integer() ? c["bone"].get<int>() : c.contains("bone") ? pack.boneIndex(bn) : -2;
        if (b == -1 || b >= (int)pack.bones.size()) { r["error"] = "bone '" + bn + "' is not in this fur"; return r; }
        if (N + 2 > kMaxSpheres) { r["error"] = "the sphere limit (85) would be exceeded"; return r; }
        V3 w{0, 0, 0};
        if (c.contains("world_mm")) w = jv3(c["world_mm"]) * 0.001f;
        else if (c.contains("local_mm") && b >= 0) w = colLocalToWorld(b, jv3(c["local_mm"]) * 0.001f);
        else { r["error"] = "world_mm (or local_mm with a bone) needed"; return r; }
        if (b < 0) b = colBoneFor(w);   // no bone given: the bone of the region the sphere sits in (2026-10-16; was torso3)
        r["bone"] = pack.bones[b].name;
        colPushUndo();
        int i = colAddSphere(b, w, c.value("r_mm", 20.0f) / 1000, 2, "manual", c.value("twin", true));
        colSel = i;
        colChanged();
        r["index"] = i;
        r["spheres"] = (int)pack.spheres.size();
        return r;
    }
    if (op == "add_flat" || op.rfind("flat_", 0) == 0) {
        // the flat: by "tag" ("flat#k"), "flat" (k) or "index" (one of its spheres); add_flat makes a new one
        auto num = [&](std::initializer_list<const char*> ks, float& v, float k = 1.0f) {
            for (auto key : ks) if (c.contains(key) && c[key].is_number()) { v = c[key].get<float>() * k; return true; }
            return false;
        };
        auto angles = [&](ColFlat& f) {
            if (c.contains("normal") && c["normal"].is_array() && c["normal"].size() == 3) {
                V3 nn = jv3(c["normal"]);
                float l = len(nn);
                if (l > 1e-6f) {
                    nn = nn * (1 / l);
                    f.pitch = std::asin(std::clamp(nn.z, -1.0f, 1.0f)) * 180 / 3.14159265f;
                    f.yaw = std::atan2(-nn.x, nn.y) * 180 / 3.14159265f;
                }
            }
            if (c.contains("angle") && c["angle"].is_array() && c["angle"].size() >= 2) {
                f.yaw = c["angle"][0].get<float>(); f.pitch = c["angle"][1].get<float>();
                if (c["angle"].size() >= 3) f.roll = c["angle"][2].get<float>();
            }
            num({"yaw_deg", "yaw"}, f.yaw); num({"pitch_deg", "pitch"}, f.pitch); num({"roll_deg", "roll"}, f.roll);
            float d;
            if (num({"d_yaw_deg"}, d)) f.yaw += d;
            if (num({"d_pitch_deg"}, d)) f.pitch += d;
            if (num({"d_roll_deg"}, d)) f.roll += d;
        };
        auto sizes = [&](ColFlat& f) {
            num({"width_mm"}, f.w, 0.001f); num({"height_mm"}, f.h, 0.001f); num({"thickness_mm", "radius_mm"}, f.t, 0.001f);
            if (c.contains("count") && c["count"].is_number_integer()) f.count = std::max(0, c["count"].get<int>());
            if (c.contains("scale") && c["scale"].is_number()) { float k = c["scale"].get<float>(); f.w *= k; f.h *= k; }
        };
        auto place = [&](ColFlat& f) -> std::string {
            if (c.contains("bone")) {
                int b = c["bone"].is_number_integer() ? c["bone"].get<int>() : pack.boneIndex(c.value("bone", std::string()));
                if (b < 0 || b >= (int)pack.bones.size()) return "bone is not in this fur";
                f.bone = b;
            }
            if (c.contains("world_mm")) f.c = jv3(c["world_mm"]) * 0.001f;
            if (c.contains("centre_mm")) f.c = jv3(c["centre_mm"]) * 0.001f;
            if (c.contains("world_offset_mm")) f.c = f.c + jv3(c["world_offset_mm"]) * 0.001f;
            return "";
        };
        auto nextId = [&]() {
            int id = colFlats.empty() ? 1 : colFlats.rbegin()->first + 1;
            for (int i = 0; i < (int)pack.spheres.size(); i++) id = std::max(id, colFlatOf(i) + 1);
            return id;
        };
        auto done = [&](int id) {
            const ColFlat& f = colFlats[id];
            json sp = json::array();
            for (int i = 0; i < (int)pack.spheres.size(); i++) if (colFlatOf(i) == id) sp.push_back(i);
            r["tag"] = "flat#" + std::to_string(id);
            r["flat"] = id;
            r["capsules_in_flat"] = (int)sp.size() / 2;
            r["sphere_indices"] = sp;
            r["params"] = {{"bone", pack.bones[f.bone].name}, {"centre_mm", v3j(f.c, 1000)}, {"width_mm", f.w * 1000}, {"height_mm", f.h * 1000},
                           {"thickness_mm", f.t * 1000}, {"yaw_deg", f.yaw}, {"pitch_deg", f.pitch}, {"roll_deg", f.roll}, {"count", f.count}};
            r["spheres"] = (int)pack.spheres.size();
            r["capsules"] = (int)pack.capsules.size();
            colChanged();
        };
        if (op == "add_flat") {
            ColFlat f;
            f.bone = std::max(0, pack.boneIndex("torso3"));
            f.c = pack.headC;
            if (std::string e = place(f); !e.empty()) { r["error"] = e; return r; }
            if (!c.contains("bone")) f.bone = colBoneFor(f.c);   // 2026-10-16: the bone of the region (a head-height flat on torso3 does not turn with the head)
            sizes(f);
            angles(f);
            int id = nextId();
            colPushUndo();
            if (std::string e = colFlatBuild(id, f); !e.empty()) { colUndoSt.pop_back(); r["error"] = e; return r; }
            done(id);
            return r;
        }
        int id = -1;
        if (c.contains("tag") && c["tag"].is_string()) {
            std::string t = c["tag"];
            if (t.rfind("flat#", 0) == 0) try { id = std::stoi(t.substr(5)); } catch (...) {}
        }
        if (c.contains("flat") && c["flat"].is_number_integer()) id = c["flat"].get<int>();
        if (id < 0 && c.contains("index") && c["index"].is_number_integer()) id = colFlatOf(c["index"].get<int>());
        if (id < 0 || !colFlats.count(id)) { r["error"] = "no such flat (tag \"flat#k\", flat k or index of one of its spheres)"; return r; }
        ColFlat f = colFlats[id];
        if (op == "flat_delete") {
            colPushUndo();
            std::set<int> del;
            for (int i = 0; i < N; i++) if (colFlatOf(i) == id) del.insert(i);
            colRemove(del);
            colFlats.erase(id);
            colSel = colSel2 = -1;
            colChanged();
            r["deleted"] = (int)del.size();
            r["spheres"] = (int)pack.spheres.size();
            r["capsules"] = (int)pack.capsules.size();
            return r;
        }
        if (op == "flat_mirror") {
            ColFlat m = f;
            float px = groom.mirrorPlane();
            if (std::fabs(f.c.x - px) < 0.003f) { r["error"] = "the flat sits on the mirror plane"; return r; }
            m.c.x = 2 * px - f.c.x;
            m.yaw = -f.yaw;
            m.roll = -f.roll;
            int b = pack.boneIndex(swapSide(pack.bones[f.bone].name));
            if (b >= 0) m.bone = b;
            int nid = nextId();
            colPushUndo();
            if (std::string e = colFlatBuild(nid, m); !e.empty()) { colUndoSt.pop_back(); r["error"] = e; return r; }
            done(nid);
            r["mirror_of"] = "flat#" + std::to_string(id);
            return r;
        }
        if (op == "flat_move") { if (std::string e = place(f); !e.empty()) { r["error"] = e; return r; } }
        else if (op == "flat_resize") sizes(f);
        else if (op == "flat_rotate") angles(f);
        else if (op == "flat_set") { if (std::string e = place(f); !e.empty()) { r["error"] = e; return r; } sizes(f); angles(f); }
        else { r["error"] = "unknown flat op '" + op + "' (add_flat flat_move flat_resize flat_rotate flat_set flat_mirror flat_delete)"; return r; }
        colPushUndo();
        if (std::string e = colFlatBuild(id, f); !e.empty()) { colUndoSt.pop_back(); r["error"] = e; return r; }
        done(id);
        return r;
    }
    std::vector<int> ix = idxList(c);
    if (op == "link" || op == "unlink") {
        int a = c.value("a", -1), b = c.value("b", -1);
        if (a < 0 || b < 0 || a >= N || b >= N || a == b) { r["error"] = "a and b: two different sphere indices"; return r; }
        colPushUndo();
        if (op == "link") {
            if (std::none_of(pack.capsules.begin(), pack.capsules.end(), [&](auto& q) { return (q[0] == a && q[1] == b) || (q[0] == b && q[1] == a); }))
                pack.capsules.push_back({a, b});
        } else {
            pack.capsules.erase(std::remove_if(pack.capsules.begin(), pack.capsules.end(), [&](auto& q) { return (q[0] == a && q[1] == b) || (q[0] == b && q[1] == a); }), pack.capsules.end());
        }
        colChanged();
        r["capsules"] = (int)pack.capsules.size();
        return r;
    }
    if (op == "rebind") {
        // 2026-10-16: rebind spheres to the bone of the region they sit in (or to `bone`), keeping their rest-pose world
        // position (local recomputed through the target bone's bind pose). Default: every sphere; `indices` limits it.
        // Capsule partners are rebound together (a capsule must join spheres of one bone). list_only reports the plan.
        int tgt = -1;
        if (c.contains("bone")) {
            tgt = c["bone"].is_number_integer() ? c["bone"].get<int>() : pack.boneIndex(c.value("bone", std::string()));
            if (tgt < 0 || tgt >= (int)pack.bones.size()) { r["error"] = "bone is not in this fur"; return r; }
        }
        std::vector<int> all;
        if (c.contains("indices") || c.contains("index")) all = ix; else for (int i = 0; i < N; i++) all.push_back(i);
        std::set<int> todo(all.begin(), all.end());
        for (auto& q : pack.capsules) { if (todo.count(q[0])) todo.insert(q[1]); if (todo.count(q[1])) todo.insert(q[0]); }
        std::map<int, int> target;
        for (int i : todo) {
            const Sphere& s = pack.spheres[i];
            V3 w = s.hasC ? s.c : colLocalToWorld(s.bone, s.local);
            int b = tgt >= 0 ? tgt : colBoneFor(w);
            if (b != s.bone) target[i] = b;
        }
        // a capsule's two spheres take one bone: the one chosen for their midpoint
        for (auto& q : pack.capsules) {
            int b0 = target.count(q[0]) ? target[q[0]] : pack.spheres[q[0]].bone, b1 = target.count(q[1]) ? target[q[1]] : pack.spheres[q[1]].bone;
            if (b0 == b1) continue;
            V3 m = ((pack.spheres[q[0]].hasC ? pack.spheres[q[0]].c : colLocalToWorld(pack.spheres[q[0]].bone, pack.spheres[q[0]].local)) +
                    (pack.spheres[q[1]].hasC ? pack.spheres[q[1]].c : colLocalToWorld(pack.spheres[q[1]].bone, pack.spheres[q[1]].local))) * 0.5f;
            int b = tgt >= 0 ? tgt : colBoneFor(m);
            for (int k : {q[0], q[1]}) { if (pack.spheres[k].bone != b) target[k] = b; else target.erase(k); }
        }
        json list = json::array();
        for (auto& [i, b] : target) list.push_back({{"i", i}, {"from", pack.bones[pack.spheres[i].bone].name}, {"to", pack.bones[b].name}, {"tag", pack.spheres[i].tag}});
        r["rebound"] = list; r["count"] = target.size(); r["checked"] = todo.size();
        if (c.value("list_only", false) || target.empty()) return r;
        colPushUndo();
        for (auto& [i, b] : target) {
            Sphere& s = pack.spheres[i];
            V3 w = s.hasC ? s.c : colLocalToWorld(s.bone, s.local);
            s.bone = b;
            s.local = colWorldToLocal(b, w);
            if (s.grp != 2 && s.grp >= 0) s.grp = 2, s.tag = s.tag.empty() ? "rebound" : s.tag;
            colRecompute(i);
        }
        colChanged();
        r["spheres"] = (int)pack.spheres.size();
        return r;
    }
    if (bad(ix)) { r["error"] = "index / indices out of range (0.." + std::to_string(N - 1) + ")"; return r; }
    if (op == "delete") {
        colPushUndo();
        std::set<int> del(ix.begin(), ix.end());
        for (int j : twinsOf(*this, del)) del.insert(j);
        colRemove(del);
        colSel = colSel2 = -1;
        colChanged();
        r["deleted"] = (int)del.size();
        r["spheres"] = (int)pack.spheres.size();
        return r;
    }
    if (op == "mirror") {
        if (N + 2 * (int)ix.size() > kMaxSpheres) { r["error"] = "the sphere limit (85) would be exceeded"; return r; }
        colPushUndo();
        json made = json::array(), skipped = json::array();
        std::map<int, int> map;
        for (int i : ix) {
            std::string why;
            int k = colMirrorSphere(i, &why);
            if (k < 0) skipped.push_back({{"i", i}, {"why", why}}); else { map[i] = k; made.push_back({{"from", i}, {"to", k}}); }
        }
        // capsules between two mirrored spheres are mirrored too (the twin capsule colMirrorSphere made is kept)
        std::vector<std::array<int, 2>> add;
        for (auto& q : pack.capsules)
            if (map.count(q[0]) && map.count(q[1]) && q[0] != q[1]) add.push_back({map[q[0]], map[q[1]]});
        for (auto& q : add) pack.capsules.push_back(q);
        colChanged();
        r["mirrored"] = made;
        r["skipped"] = skipped;
        r["spheres"] = (int)pack.spheres.size();
        return r;
    }
    if (op == "move" || op == "resize" || op == "set" || op == "bone") {
        colPushUndo();
        for (int i : ix) {
            Sphere& s = pack.spheres[i];
            if (op == "move" || op == "set") {
                if (c.contains("local_mm")) s.local = jv3(c["local_mm"]) * 0.001f;                       // absolute, bone-local
                if (c.contains("offset_mm")) s.local = s.local + jv3(c["offset_mm"]) * 0.001f;            // relative, bone-local
                if (c.contains("world_mm")) s.local = colWorldToLocal(s.bone, jv3(c["world_mm"]) * 0.001f);   // absolute, world (rest)
                if (c.contains("world_offset_mm")) s.local = colWorldToLocal(s.bone, (s.hasC ? s.c : colLocalToWorld(s.bone, s.local)) + jv3(c["world_offset_mm"]) * 0.001f);
            }
            if ((op == "resize" || op == "set") && c.contains("r_mm")) s.r = std::max(0.0005f, c["r_mm"].get<float>() / 1000);
            if ((op == "resize" || op == "set") && c.contains("scale")) s.r = std::max(0.0005f, s.r * c["scale"].get<float>());
            if (op == "bone") {
                int b = c.contains("bone") && c["bone"].is_number_integer() ? c["bone"].get<int>() : pack.boneIndex(c.value("bone", std::string()));
                if (b < 0 || b >= (int)pack.bones.size()) { r["error"] = "bone is not in this fur"; return r; }
                V3 w = s.hasC ? s.c : colLocalToWorld(s.bone, s.local);
                s.bone = b;
                s.local = colWorldToLocal(b, w);
            }
            if (s.grp != 2 && s.grp >= 0) s.grp = 2, s.tag = s.tag.empty() ? "edited" : s.tag;   // hand-edited: a refit keeps it
            colRecompute(i);
            if (colR.count(i) && (op == "resize" || op == "set") && (c.contains("r_mm") || c.contains("scale"))) colR.erase(i);
        }
        colChanged();
        r["edited"] = ix;
        return r;
    }
    r["error"] = "unknown op '" + op + "' (add add_flat flat_move flat_resize flat_rotate flat_set flat_mirror flat_delete move set resize bone delete mirror link unlink select reset undo redo)";
    return r;
}

// ---------------------------------------------------------------------------------------------------- clearance report
static float capDist(V3 p, V3 a, V3 b, float ra, float rb) {
    V3 ab = b - a;
    float l2 = dot(ab, ab);
    float t = l2 > 1e-12f ? std::clamp(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    V3 q = a + ab * t;
    return len(p - q) - (ra * (1 - t) + rb * t);
}

json App::colReport(const json& c) {
    json r;
    if (!loaded) { r["error"] = "nothing open"; return r; }
    const float tol = c.value("tol_mm", 1.0f) / 1000, pokeMm = c.value("poke_mm", 3.0f);
    const std::string mode = c.value("mode", std::string("rest"));
    const int NS = (int)pack.spheres.size();
    auto rad = [&](int i) { return colR.count(i) ? colR[i] : pack.spheres[i].r; };
    const int fromCv = c.value("from_cv", std::max(2, groom.K * 2 / 3));   // the lower third: where hair meets the outfit
    auto grpName = [&](int i) { int g = pack.spheres[i].grp; return g == 0 ? "base" : g == 1 ? "fitted/outfit" : g == 2 ? "manual" : "as built"; };

    // ---- 1. guides inside a collider
    struct Hit { int guide, cv, cap; float depth; };
    std::map<int, Hit> worst;   // per guide
    auto checkPoint = [&](int g, int k, V3 p, const std::vector<V3>& cc) {
        for (size_t q = 0; q < pack.capsules.size(); q++) {
            int a = pack.capsules[q][0], b = pack.capsules[q][1];
            float d = capDist(p, cc[a], cc[b], rad(a), rad(b));
            if (d < -tol) {
                auto it = worst.find(g);
                if (it == worst.end() || -d > it->second.depth) worst[g] = {g, k, (int)q, -d};
            }
        }
    };
    int guidesChecked = 0;
    json simInfo;
    if (mode == "rest") {
        std::vector<V3> cc(NS);
        for (int i = 0; i < NS; i++) cc[i] = pack.spheres[i].hasC ? pack.spheres[i].c : colLocalToWorld(pack.spheres[i].bone, pack.spheres[i].local);
        for (size_t g = 0; g < groom.P.size(); g++) {
            if (!groom.alive[g]) continue;
            guidesChecked++;
            for (int k = fromCv; k < groom.K; k++) checkPoint((int)g, k, groom.renderPoint((int)g, k), cc);
        }
    } else {
        // simulated: the SDK runs `steps` frames with a motion preset, the guide CVs are read back (as band_sim) and tested
        // against the colliders in the same pose. mode "sim" = the current preset, or "preset": a name (Idle, TurnHead, Nod, Walk, Run, SpinBody, Wind)
        std::string pn = c.value("preset", std::string());
        MotionPreset pre = motion.preset;
        for (int pi = 0; pi < (int)MotionPreset::Count; pi++) {
            std::string nm = motionName((MotionPreset)pi), a = nm, b = pn;
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            a.erase(std::remove(a.begin(), a.end(), ' '), a.end());
            b.erase(std::remove(b.begin(), b.end(), ' '), b.end());
            if (!b.empty() && a == b) pre = (MotionPreset)pi;
        }
        const int steps = std::clamp(c.value("steps", 180), 10, 3000);
        bool sSim = simulate, sPaused = paused, sWind = windOn;
        Motion sMotion = motion;
        double sAcc = simAcc;
        rebuildHair();
        simulate = true; paused = false; windOn = c.value("wind", 0.0f) > 0;
        motion.preset = pre; motion.speed = 1; motion.amount = c.value("amount", 1.0f); motion.windStrength = c.value("wind", 0.0f); motion.t = 0; simAcc = 0; teleport = true;
        std::vector<float> xyz;
        int PG = 0;
        const size_t NA = guideOfAsset.size();
        bool ok = true;
        std::vector<char> seen(groom.P.size(), 0);
        for (int s = 0; s < steps && ok; s++) {
            step(1.0 / 60.0);
            if (s < steps / 2) continue;                               // let the pose settle, test the second half
            int pg = PG;
            if (!hw.readGuides(xyz, pg) || pg <= 0 || xyz.size() < NA * size_t(pg) * 3) { ok = false; simInfo["error"] = "guide read-back failed"; break; }
            PG = pg;
            std::vector<V3> cc(NS);
            for (int i = 0; i < NS; i++) cc[i] = colPosed(i);
            for (size_t a = 0; a < NA; a++) {
                int g = guideOfAsset[a];
                if (!seen[g]) { seen[g] = 1; guidesChecked++; }
                for (int j = std::max(1, c.contains("from_cv") ? (int)std::lround(c["from_cv"].get<float>() * (PG - 1) / std::max(1, groom.K - 1)) : PG * 2 / 3); j < PG; j++) {
                    size_t q = (a * PG + j) * 3;
                    checkPoint(g, j, {xyz[q], xyz[q + 1], xyz[q + 2]}, cc);
                }
            }
        }
        rebuildHair();
        motion = sMotion; simulate = sSim; paused = sPaused; windOn = sWind; simAcc = sAcc; teleport = true; aoRefreshAll = true;
        simInfo["preset"] = motionName(pre);
        simInfo["steps"] = steps;
        simInfo["cvs_per_guide"] = PG;
    }
    std::vector<Hit> hs;
    for (auto& [g, h] : worst) hs.push_back(h);
    std::sort(hs.begin(), hs.end(), [](const Hit& a, const Hit& b) { return a.depth > b.depth; });
    json inside = json::array();
    std::map<std::string, int> byGrp;
    for (auto& h : hs) {
        int a = pack.capsules[h.cap][0];
        byGrp[grpName(a)]++;
        if ((int)inside.size() < c.value("worst", 20))
            inside.push_back({{"guide", h.guide}, {"cv", h.cv}, {"depth_mm", std::round(h.depth * 1e4f) / 10}, {"capsule", h.cap},
                              {"spheres", {a, pack.capsules[h.cap][1]}}, {"collider", grpName(a)}, {"tag", pack.spheres[a].tag}});
    }
    r["mode"] = mode;
    if (!simInfo.is_null()) r["sim"] = simInfo;
    r["guides_checked"] = guidesChecked;
    r["guides_inside"] = (int)hs.size();
    r["inside_depth_threshold_mm"] = tol * 1000;
    r["inside_by_collider_group"] = byGrp;
    r["inside_worst"] = inside;

    // ---- 2. capsules poking out of the skin: surface samples of every capsule against the nearest mesh vertex (outward
    // normal of that vertex: away from the body's vertical axis), > poke_mm outside = poking out
    struct MV { V3 p, n; };
    std::vector<MV> mv;
    for (auto& m : pack.meshes) {
        if (m.kind == "hair_cards" || m.kind == "beard" || m.kind == "headgear" || m.v.empty()) continue;
        std::vector<V3> nrm(m.v.size());
        for (auto& t : m.t) {
            V3 fn = cross(m.v[t[1]] - m.v[t[0]], m.v[t[2]] - m.v[t[0]]);
            for (int k = 0; k < 3; k++) nrm[t[k]] = nrm[t[k]] + fn;
        }
        for (size_t i = 0; i < m.v.size(); i++) {
            float l = len(nrm[i]);
            if (l < 1e-12f) continue;
            V3 n = nrm[i] * (1.0f / l), out{m.v[i].x, m.v[i].y - pack.headC.y, 0.0f};
            if (n.x * out.x + n.y * out.y < 0) n = n * -1.0f;
            mv.push_back({m.v[i], n});
        }
    }
    json poke = json::array();
    int nPoke = 0;
    for (size_t q = 0; q < pack.capsules.size(); q++) {
        int a = pack.capsules[q][0], b = pack.capsules[q][1];
        V3 A = pack.spheres[a].hasC ? pack.spheres[a].c : colLocalToWorld(pack.spheres[a].bone, pack.spheres[a].local);
        V3 B = pack.spheres[b].hasC ? pack.spheres[b].c : colLocalToWorld(pack.spheres[b].bone, pack.spheres[b].local);
        float ra = rad(a), rb = rad(b), worstP = -1e9f;
        V3 ax = B - A;
        float L = len(ax);
        V3 u = L > 1e-6f ? ax * (1.0f / L) : V3{0, 0, 1};
        V3 h = std::fabs(u.z) < 0.9f ? V3{0, 0, 1} : V3{1, 0, 0};
        V3 e1 = cross(u, h);
        e1 = e1 * (1.0f / len(e1));
        V3 e2 = cross(u, e1);
        int nt = std::max(1, int(L / 0.015f));
        for (int ti = 0; ti <= nt; ti++) {
            float t = float(ti) / nt, rr = ra * (1 - t) + rb * t;
            V3 cen = A + ax * t;
            for (int k = 0; k < 12; k++) {
                float an = 6.2831853f * k / 12;
                V3 p = cen + (e1 * std::cos(an) + e2 * std::sin(an)) * rr;
                float bd = 1e9f; int bi = -1;
                for (size_t i = 0; i < mv.size(); i++) { V3 d = p - mv[i].p; float dd = dot(d, d); if (dd < bd) { bd = dd; bi = (int)i; } }
                if (bi < 0 || bd > 0.012f * 0.012f) continue;   // farther from every vertex than 12 mm: the vertex normal says nothing (big triangles)
                worstP = std::max(worstP, dot(p - mv[bi].p, mv[bi].n));
            }
        }
        if (worstP * 1000 > pokeMm) {
            nPoke++;
            if ((int)poke.size() < c.value("worst", 20)) poke.push_back({{"capsule", q}, {"spheres", {a, b}}, {"poke_mm", std::round(worstP * 1e4f) / 10}, {"collider", grpName(a)}, {"tag", pack.spheres[a].tag}});
        }
    }
    std::sort(poke.begin(), poke.end(), [](const json& x, const json& y) { return x["poke_mm"].get<float>() > y["poke_mm"].get<float>(); });
    r["capsules_poking_out"] = nPoke;
    r["poke_threshold_mm"] = pokeMm;
    r["poke_worst"] = poke;
    r["poke_note"] = "indicative: capsule surface samples within 12 mm of a vertex of a shown mesh (head, body, outfit, scabbard, sword), outside it by that vertex normal";
    int noCap = 0;
    std::vector<char> inCap(NS, 0);
    for (auto& q : pack.capsules) inCap[q[0]] = inCap[q[1]] = 1;
    for (int i = 0; i < NS; i++) if (!inCap[i]) noCap++;
    r["spheres_without_capsule"] = noCap;
    r["counts"] = {{"spheres", NS}, {"capsules", (int)pack.capsules.size()}, {"limit_spheres", kMaxSpheres}, {"limit_capsules", kMaxCapsules}};
    return r;
}

// ---------------------------------------------------------------------------------------------------- MCP / serve
json App::colCommand(const std::string& d, const json& c) {
    json r = {{"do", d}};
    if (!loaded) { r["error"] = "nothing open"; return r; }
    if (d == "list_colliders") {
        if (c.value("scan", false) || (c.value("targets", false) && colTargets.empty())) colScan();
        json l = colList();
        for (auto& [k, v] : l.items()) r[k] = v;
        if (c.value("targets", false) || c.value("scan", false)) {
            json t = json::array();
            for (auto& x : colTargets) t.push_back({{"id", x["id"]}, {"label", x["label"]}, {"kind", x["kind"]}, {"default", x["default"]}, {"note", x["note"]}});
            r["fit_targets"] = t;
        }
        if (!colReportJ.is_null()) r["last_fit_report"] = colReportJ;
        return r;
    }
    if (d == "fit_colliders") {
        json res = colFit(c);
        for (auto& [k, v] : res.items()) r[k] = v;
        if (!res.contains("error")) { rebuildHair(); }
        return r;
    }
    if (d == "edit_collider") {
        json res = colEditOp(c);
        for (auto& [k, v] : res.items()) r[k] = v;
        if (!res.contains("error")) rebuildHair();
        r["counts"] = {{"spheres", (int)pack.spheres.size()}, {"capsules", (int)pack.capsules.size()}};
        return r;
    }
    if (d == "collider_report") {
        json res = colReport(c);
        for (auto& [k, v] : res.items()) r[k] = v;
        return r;
    }
    if (d == "drape_outfit" || d == "drape_report" || d == "list_meshes" || d == "grow_colliders" || d == "shell_colliders") {
        json res = drapeCommand(d, c);
        for (auto& [k, v] : res.items()) r[k] = v;
        return r;
    }
    r["error"] = "unknown collider command";
    return r;
}

// ---------------------------------------------------------------------------------------------------- viewport
int App::colPickAt(float mx, float my) const {
    int best = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < (int)pack.spheres.size(); i++) {
        V3 c = colPosed(i);
        XMFLOAT2 s = project(c);
        if (s.x < -1e5f) continue;
        float wpp = len(screenDeltaToWorld(1, 0, c));
        float pr = std::max(5.0f, pack.spheres[i].r / std::max(wpp, 1e-6f));
        float d = std::hypot(s.x - mx, s.y - my);
        if (d <= pr + 3) {
            float score = pr;                                 // the smallest circle under the cursor wins
            if (score < bestScore) { bestScore = score; best = i; }
        }
    }
    if (best >= 0) return best;
    float bd = 8.0f;
    for (int i = 0; i < (int)pack.spheres.size(); i++) {   // near an outline
        V3 c = colPosed(i);
        XMFLOAT2 s = project(c);
        if (s.x < -1e5f) continue;
        float wpp = len(screenDeltaToWorld(1, 0, c));
        float pr = pack.spheres[i].r / std::max(wpp, 1e-6f);
        float d = std::fabs(std::hypot(s.x - mx, s.y - my) - pr);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

bool App::colMouse() {
    if (!colEdit || !loaded || pack.spheres.empty()) return false;
    ImGuiIO& io = ImGui::GetIO();
    float mx = io.MousePos.x, my = io.MousePos.y;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int h = colPickAt(mx, my);
        if (h >= 0 && io.KeyCtrl && colSel >= 0 && h != colSel) colSel2 = h;
        else if (h >= 0) { colSel = h; colSel2 = -1; }
        else if (!io.KeyCtrl) colSel = colSel2 = -1;
        colDrag = h >= 0 && !io.KeyCtrl;
        colDragPushed = false;
        return true;
    }
    if (colDrag && ImGui::IsMouseDown(ImGuiMouseButton_Left) && colSel >= 0) {
        if (io.MouseDelta.x == 0 && io.MouseDelta.y == 0) return true;
        if (!colDragPushed) { colPushUndo(); colDragPushed = true; }
        Sphere& s = pack.spheres[colSel];
        V3 pc = colPosed(colSel);
        V3 np = pc + screenDeltaToWorld(io.MouseDelta.x, io.MouseDelta.y, pc);   // posed space, in the screen plane
        V3 rest = np;
        if (s.bone < (int)boneM.size()) {
            XMFLOAT3 o;
            XMStoreFloat3(&o, XMVector3TransformCoord(XMVectorSet(np.x, np.y, np.z, 1), XMMatrixInverse(nullptr, XMLoadFloat4x4(&boneM[s.bone]))));
            rest = {o.x, o.y, o.z};
        }
        s.local = colWorldToLocal(s.bone, rest);
        if (s.grp >= 0 && s.grp != 2) { s.grp = 2; if (s.tag.empty()) s.tag = "edited"; }
        colRecompute(colSel);
        pack.colModified = true;
        colOpsAt = groom.ops.size();
        if (ImGui::GetTime() - lastBuild > 0.2) assetDirty = true;
        return true;
    }
    if (colDrag && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        colDrag = false;
        if (colDragPushed) { colChanged(); status = "Collider " + std::to_string(colSel) + " moved"; }
        return true;
    }
    return false;
}

void App::colOverlay() {
    if (!colEdit || !loaded) return;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    for (int i = 0; i < (int)pack.spheres.size(); i++) {
        const Sphere& s = pack.spheres[i];
        V3 c = colPosed(i);
        XMFLOAT2 p = project(c);
        if (p.x < -1e5f) continue;
        float pr = s.r / std::max(len(screenDeltaToWorld(1, 0, c)), 1e-6f);
        bool sel = i == colSel, sel2 = i == colSel2;
        ImU32 col = sel ? ImGui::GetColorU32(ImVec4(0.94f, 0.63f, 0.25f, 1.0f))
                  : sel2 ? ImGui::GetColorU32(ImVec4(0.94f, 0.63f, 0.25f, 0.75f))
                  : s.grp == 0 ? ImGui::GetColorU32(ImVec4(0.55f, 0.68f, 0.85f, 0.55f))
                  : s.grp == 2 ? ImGui::GetColorU32(ImVec4(0.55f, 0.85f, 0.60f, 0.8f))
                  : ImGui::GetColorU32(ImVec4(0.92f, 0.78f, 0.45f, 0.7f));
        if (sel) fg->AddCircleFilled(ImVec2(p.x, p.y), pr, ImGui::GetColorU32(ImVec4(0.94f, 0.63f, 0.25f, 0.16f)), 40);
        fg->AddCircle(ImVec2(p.x, p.y), pr, col, 40, sel ? 2.5f : 1.0f);
        fg->AddCircleFilled(ImVec2(p.x, p.y), sel ? 3.5f : 2.0f, col, 12);
    }
    for (auto& q : pack.capsules) {   // a capsule = a line between its two spheres
        if (q[0] == q[1]) continue;
        XMFLOAT2 a = project(colPosed(q[0])), b = project(colPosed(q[1]));
        if (a.x < -1e5f || b.x < -1e5f) continue;
        bool hot = q[0] == colSel || q[1] == colSel;
        fg->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), ImGui::GetColorU32(ImVec4(0.94f, 0.63f, 0.25f, hot ? 0.9f : 0.28f)), hot ? 2.0f : 1.0f);
    }
    if (colSel >= 0 && colSel < (int)pack.spheres.size()) {
        const Sphere& s = pack.spheres[colSel];
        XMFLOAT2 p = project(colPosed(colSel));
        char b[160];
        snprintf(b, sizeof b, "#%d  %s  r %.1f mm", colSel, pack.bones[s.bone].name.c_str(), s.r * 1000);
        fg->AddText(ImVec2(p.x + 8, p.y - 18), ImGui::GetColorU32(ImVec4(0.94f, 0.63f, 0.25f, 1.0f)), b);
    }
}
