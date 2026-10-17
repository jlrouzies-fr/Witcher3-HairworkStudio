// handback_gate (2026-10-16, agent tools): the whole hand-back checklist of tools/hairworks/AGENT_DIFFICULTIES.md on the
// open project, in one command, with a pass / fail verdict an agent cannot argue with. Compiled into serve.cpp (after
// serve_faces.inc: it calls checkFaces, the hairline helpers, cameraOf and serveCommand). Our own code (Studio License).
//
// Items (each one reports its numbers, the verdict lists the failed ones):
//   roots      hairline_check: roots_off_scalp 0, guides_over_face 0 (checklist 2)
//   direction  check_direction at rest, and under wind yaw 15 / 105 / 195 / 285 at strengths 1 and 5: 0 offenders (3)
//   loops      check_loops 0 (3)
//   faces      check_faces at wind 1: no interpolated hair through the SKIN (face / ears / neck; "skin" penetrations,
//              see serve_faces.inc); penetrations under the painted scalp and divergent faces are reported only (4)
//   colliders  list_colliders misbound 0; small isolated blocking spheres (r < small_mm, overlapping nothing) reported (8)
//   scalp      scalp pixels per gate camera vs the reference project (or the build as opened): no camera worse than
//              reference x (1 + scalp_tol) + scalp_abs px (5)
//   silhouette hair-mask pixels outside the reference's silhouette (dilated flare_px) per camera: flare / loss (6, 8)
//   views      renders of every view (front, both 3/4, both sides, back, top-down, 3/4 above, low nape) into a contact
//              sheet <out>\gate\<tag>_sheet.png (6)
//   export     dev export through the Studio's own exporter (no hand assembly): must succeed; the exported config's
//              look.extra_set must carry every project look key with the same value; no Studio-only colour keys at 0;
//              beard on Coarse short when a beard fur has edits (10, 11). export_audit.py runs from the MCP layer.
// The verdict is recorded (gate.cpp) and guards later MCP / --agent saves and exports.
#include <chrono>
namespace hgate {

struct Img { int w = 0, h = 0; std::vector<unsigned char> px; bool ok() const { return w > 0; } };
static Img loadImg(const std::string& p) {
    Img im;
    int w = 0, h = 0;
    unsigned char* d = loadImage(p, &w, &h);
    if (!d) return im;
    im.w = w; im.h = h; im.px.assign(d, d + size_t(w) * h * 4);
    freeImage(d);
    return im;
}
static bool isGreen(const unsigned char* q) { return q[1] > 200 && q[0] < 60 && q[2] < 60; }
// mask pass (render overlay "mask"): meshes dark grey, backdrop (18,20,22), hair flat red (255,0,0)
static bool isHair(const unsigned char* q) { return q[0] > 200 && q[1] < 60 && q[2] < 60; }
static long countPx(const Img& im, bool (*pred)(const unsigned char*)) {
    long n = 0;
    for (size_t i = 0; i + 3 < im.px.size(); i += 4) if (pred(&im.px[i])) n++;
    return n;
}
static std::vector<unsigned char> maskOf(const Img& im, bool (*pred)(const unsigned char*)) {
    std::vector<unsigned char> m(size_t(im.w) * im.h, 0);
    for (size_t i = 0; i < m.size(); i++) m[i] = pred(&im.px[i * 4]) ? 1 : 0;
    return m;
}
// separable box dilation by r pixels
static std::vector<unsigned char> dilate(const std::vector<unsigned char>& m, int w, int h, int r) {
    std::vector<unsigned char> a(m.size(), 0), b(m.size(), 0);
    for (int y = 0; y < h; y++) {
        int run = -1;
        for (int x = 0; x < w; x++) {
            if (m[size_t(y) * w + x]) run = r;
            else if (run >= 0) run--;
            if (run >= 0) a[size_t(y) * w + x] = 1;
        }
        run = -1;
        for (int x = w - 1; x >= 0; x--) {
            if (m[size_t(y) * w + x]) run = r;
            else if (run >= 0) run--;
            if (run >= 0) a[size_t(y) * w + x] = 1;
        }
    }
    for (int x = 0; x < w; x++) {
        int run = -1;
        for (int y = 0; y < h; y++) {
            if (a[size_t(y) * w + x]) run = r;
            else if (run >= 0) run--;
            if (run >= 0) b[size_t(y) * w + x] = 1;
        }
        run = -1;
        for (int y = h - 1; y >= 0; y--) {
            if (a[size_t(y) * w + x]) run = r;
            else if (run >= 0) run--;
            if (run >= 0) b[size_t(y) * w + x] = 1;
        }
    }
    return b;
}

struct Cam { std::string name; json cam; };
// the gate cameras: HairLab shots of the pack when present (the in-game views), synthesised around the head otherwise;
// top-down, 3/4 above and the low nape camera are always synthesised (no HairLab shot looks there)
static std::vector<Cam> cameras(App& a, const json& c) {
    std::vector<Cam> v;
    V3 hc = a.pack.headC;
    float top = a.pack.headTop, d = c.value("cam_dist", 0.62f), fov = c.value("cam_fovy", 26.0f);
    V3 tgt{hc.x, hc.y, hc.z + 0.01f};
    auto orbit = [&](const std::string& name, float yawDeg, float pitchDeg) {
        float ya = yawDeg * 3.14159265f / 180, pa = pitchDeg * 3.14159265f / 180;
        V3 e{tgt.x + d * std::cos(pa) * std::sin(ya), tgt.y + d * std::cos(pa) * std::cos(ya), tgt.z + d * std::sin(pa)};
        return json{{"eye", {e.x, e.y, e.z}}, {"target", {tgt.x, tgt.y, tgt.z}}, {"fovy", fov}, {"view", name}};
    };
    auto shot = [&](int id, const std::string& name, float yaw) {
        std::string k = std::to_string(id);
        if (c.value("use_shots", true) && a.pack.shots.contains(k)) v.push_back({name, a.pack.shots[k]});
        else v.push_back({name, orbit(name, yaw, 5)});
    };
    shot(4, "front", 0);
    shot(3, "34_right", 45);      // character's right = -x
    shot(15, "34_left", -45);
    shot(5, "side_right", 90);
    shot(2, "side_left", -90);
    shot(0, "back", 180);
    V3 t2{hc.x, hc.y, top - 0.04f};
    v.push_back({"topdown", {{"eye", {t2.x, t2.y + 0.07f, t2.z + 0.60f}}, {"target", {t2.x, t2.y, t2.z}}, {"fovy", fov}, {"view", "topdown"}}});
    v.push_back({"34_above", {{"eye", {hc.x + 0.33f, hc.y + 0.33f, top + 0.40f}}, {"target", {hc.x, hc.y, hc.z + 0.03f}}, {"fovy", fov}, {"view", "34_above"}}});
    v.push_back({"34_above_left", {{"eye", {hc.x - 0.33f, hc.y + 0.33f, top + 0.40f}}, {"target", {hc.x, hc.y, hc.z + 0.03f}}, {"fovy", fov}, {"view", "34_above_left"}}});
    v.push_back({"low_nape", {{"eye", {hc.x, hc.y - 0.50f, hc.z - 0.22f}}, {"target", {hc.x, hc.y, hc.z - 0.05f}}, {"fovy", fov - 4}, {"view", "low_nape"}}});
    return v;
}

// contact sheet: tiles of `tile` px, `cols` per row, nearest resample
static bool sheet(const std::vector<std::string>& pngs, const std::string& out, int tile, int cols) {
    std::vector<Img> ims;
    for (auto& p : pngs) ims.push_back(loadImg(p));
    int n = (int)ims.size();
    if (!n) return false;
    int rows = (n + cols - 1) / cols, W = cols * tile, H = rows * tile;
    std::vector<unsigned char> px(size_t(W) * H * 4, 20);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    for (int i = 0; i < n; i++) {
        const Img& im = ims[i];
        if (!im.ok()) continue;
        int ox = (i % cols) * tile, oy = (i / cols) * tile;
        for (int y = 0; y < tile; y++) for (int x = 0; x < tile; x++) {
            int sx = x * im.w / tile, sy = y * im.h / tile;
            memcpy(&px[(size_t(oy + y) * W + ox + x) * 4], &im.px[(size_t(sy) * im.w + sx) * 4], 4);
        }
        for (int x = 0; x < tile; x++) { unsigned char* q = &px[(size_t(oy) * W + ox + x) * 4]; q[0] = q[1] = q[2] = 90; }   // tile border
        for (int y = 0; y < tile; y++) { unsigned char* q = &px[(size_t(oy + y) * W + ox) * 4]; q[0] = q[1] = q[2] = 90; }
    }
    return writePng(out, W, H, px.data());
}

// collider geometry: misbound (colList), small isolated blocking spheres (nothing overlaps them, no capsule partner)
static json colliderItem(App& a, const json& c, bool& fail) {
    json L = a.colList();
    int misAll = L.value("misbound_count", 0), mis = 0;
    json misHead = json::array();
    for (auto& m : L.value("misbound", json::array())) {
        bool headWant = m.value("suggested", std::string()) == "head", headIs = m.value("bone", std::string()) == "head";
        if (headWant != headIs) { mis++; misHead.push_back(m); }
    }
    float smallMm = c.value("small_mm", 12.0f);
    auto& sp = a.pack.spheres;
    std::vector<int> capN(sp.size(), 0);
    for (auto& q : a.pack.capsules) { capN[q[0]]++; if (q[1] != q[0]) capN[q[1]]++; }
    std::vector<V3> cen(sp.size());
    std::vector<float> rad(sp.size());
    for (size_t i = 0; i < sp.size(); i++) {
        cen[i] = sp[i].hasC ? sp[i].c : a.colLocalToWorld(sp[i].bone, sp[i].local);
        rad[i] = a.colR.count((int)i) ? a.colR[(int)i] : sp[i].r;
    }
    json smallL = json::array(), isolated = json::array();
    for (size_t i = 0; i < sp.size(); i++) {
        if (rad[i] < 0.0005f) continue;   // switched off
        bool touch = capN[i] > 0;
        for (size_t j = 0; j < sp.size() && !touch; j++) if (j != i && rad[j] >= 0.0005f && len(cen[i] - cen[j]) < rad[i] + rad[j]) touch = true;
        json e = {{"i", (int)i}, {"bone", a.pack.bones[sp[i].bone].name}, {"r_mm", std::round(rad[i] * 1e4f) / 10}, {"c_mm", v3j(cen[i] * 1000.0f)}, {"tag", sp[i].tag}};
        if (!touch) isolated.push_back(e);
        if (rad[i] * 1000 < smallMm && !touch) smallL.push_back(e);
    }
    fail = mis > 0;
    return {{"misbound", mis}, {"misbound_list", misHead}, {"misbound_any_bone", misAll}, {"misbound_any_bone_list", L["misbound"]}, {"spheres", sp.size()}, {"capsules", a.pack.capsules.size()},
            {"isolated", isolated.size()}, {"isolated_list", isolated}, {"small_isolated", smallL.size()}, {"small_isolated_list", smallL},
            {"small_mm", smallMm}, {"note", "misbound = head-height spheres not bound to the head (they do not turn with the head in game: Geralt headband 2026-10-16); misbound_any_bone also counts shoulder / torso differences (information); isolated / small spheres are reported (small spaced spheres leak: make blocking spheres big and overlapping)"}};
}

// export item: the Studio's own dev export, then the exported config's look.extra_set against the project's look keys
static json exportItem(App& a, const json& c, bool& fail) {
    json r;
    std::string od;
    bool ok = a.exportAll(od, false);
    r["ok"] = ok; r["dir"] = od; r["status"] = a.status;
    if (!ok) { fail = true; r["error"] = "Studio export failed: " + a.status; return r; }
    std::string base = od + "\\" + a.pack.character + (a.styleId != "default" ? "_" + a.styleId : std::string()) + "_" + a.tag;
    r["base"] = base;
    json look, cfg;
    try { std::ifstream f(base + "_look.json"); look = json::parse(f); } catch (...) {}
    try { std::ifstream f(base + "_config.json"); cfg = json::parse(f); } catch (...) {}
    if (look.is_null() || cfg.is_null()) { fail = true; r["error"] = "export wrote no _look.json / _config.json next to " + base; return r; }
    json xs = cfg.value("look", json::object()).value("extra_set", json::object());
    json missing = json::array(), diff = json::array(), studioOnly = json::array();
    auto same = [](const json& x, const json& y) {
        if (x.is_number() && y.is_number()) return std::fabs(x.get<double>() - y.get<double>()) < 1e-4;
        if (x.is_array() && y.is_array()) {
            size_t n = std::min(x.size(), y.size());
            for (size_t i = 0; i < n; i++) if (!(x[i].is_number() && y[i].is_number() ? std::fabs(x[i].get<double>() - y[i].get<double>()) < 1e-4 : x[i] == y[i])) return false;
            for (size_t i = n; i < x.size(); i++) if (!(x[i].is_number() && x[i].get<double>() == 255)) return false;   // Color alpha padding
            for (size_t i = n; i < y.size(); i++) if (!(y[i].is_number() && y[i].get<double>() == 255)) return false;
            return true;
        }
        return x == y;
    };
    json lookSet = look.value("set", json::object());
    for (auto& [path, tv] : lookSet.items()) {
        if (!xs.contains(path)) { missing.push_back(path); continue; }
        json want = tv.is_array() && tv.size() == 2 ? tv[1] : tv, got = xs[path].is_array() && xs[path].size() == 2 ? xs[path][1] : xs[path];
        if (!same(want, got)) diff.push_back({{"path", path}, {"project", want}, {"config", got}});
        // Studio-only colour scaling exported as 0 made Junior black in game (HAIRWORK "L495 / L497")
        if ((path == "graphicalMaterials.color.textureBrightness" || path == "graphicalMaterials.color.strandBlendScale") && want.is_number() && want.get<double>() == 0)
            studioOnly.push_back(path);
    }
    r["look_keys"] = lookSet.size(); r["extra_set_keys"] = xs.size();
    r["missing"] = missing; r["different"] = diff; r["studio_only_zero"] = studioOnly;
    // beards: the Coarse short preset (user rule 2026-10-15) for every HairWorks beard with edits
    // (the preset name lives in the project file: top level "material" for the edited fur, "beard": {"material"} for the
    // parked beard fur in a hair project)
    if (a.beardOn && a.beardGroom.ops.size() + a.beardLook.over.size() > 0) {
        std::string bm;
        if (a.fur == 1) bm = a.presetSel < (int)a.presets.size() ? a.presets[a.presetSel].name : "";
        else if (!a.projectPath.empty()) { try { std::ifstream f(a.projectPath); json pj = json::parse(f); bm = pj.value("beard", json::object()).value("material", ""); } catch (...) {} }
        r["beard_material"] = bm;
        if (!bm.empty() && bm.rfind("Coarse short", 0) != 0) r["beard_not_coarse_short"] = true;
    }
    fail = !missing.empty() || !diff.empty() || !studioOnly.empty() || r.value("beard_not_coarse_short", false);
    return r;
}

}  // namespace hgate


namespace hgate {
// the geometry measurements of one groom state (run on the current groom and on the reference): every number an agent
// can act on, no verdict yet
struct Geo { json roots, loops, direction, faces, colliders; };
static Geo measure(App& a, const json& c, const std::string& out, const std::set<std::string>& skip) {
    Geo g;
    bool quit = false;
    auto run = [&](const json& cmd) { return a.serveCommand(cmd, out, quit); };
    if (!skip.count("roots")) {
        json h = run({{"do", "hairline_check"}});
        g.roots = {{"roots_off_scalp", h.value("roots_off_scalp_count", 0)}, {"guides_over_face", h.value("guides_over_face", 0)},
                   {"lifted", h.value("lifted_count", 0)}, {"flow_outliers", h.value("flow_outliers_count", 0)},
                   {"roots_off_scalp_list", h.value("roots_off_scalp", json::array())}, {"over_face_list", h.value("over_face", json::array())}};
        if (h.contains("error")) g.roots["error"] = h["error"];
    }
    if (!skip.count("loops")) {
        json l = run({{"do", "check_loops"}});
        g.loops = {{"count", l.value("count", 0)}, {"list", l.value("flagged", json::array())}};
    }
    if (!skip.count("direction")) {
        // 2026-10-16 (no-penetration task): 8 headings by default; the old 4 (15 / 105 / 195 / 285) can be passed in
        json yaws = c.value("wind_yaw_deg", json({0, 45, 90, 135, 180, 225, 270, 315})), strengths = c.value("wind_strengths", json({1, 5}));
        json rest = a.checkDirection({{"wind", false}});
        int nWind = 0;
        json winds = json::array();
        for (auto& s : strengths) for (auto& y : yaws) {
            json d = a.checkDirection({{"wind", true}, {"wind_yaw_deg", y}, {"wind_strength", s}, {"wind_s", c.value("wind_s", 3.0f)}});
            int n = d.value("wind_offenders", 0);
            nWind += n;
            winds.push_back({{"yaw", y}, {"strength", s}, {"offenders", n}, {"samples", d.value("wind_samples", 0)}, {"list", d.value("wind_list", json::array())}});
        }
        g.direction = {{"rest_offenders", rest.value("rest_offenders", 0)}, {"rest_up", rest.value("rest_up", 0)}, {"rest_against", rest.value("rest_against", 0)},
                       {"rest_list", rest.value("rest_list", json::array())}, {"wind_offenders_total", nWind}, {"wind", winds}};
    }
    if (!skip.count("faces")) {
        json fc = {{"wind_strength", c.value("faces_wind", 1.0f)}, {"list_only", true}, {"wind_yaw_deg", c.value("wind_yaw_deg", json({0, 45, 90, 135, 180, 225, 270, 315}))}};
        json f = checkFaces(a, fc, a.grpId);
        json pen = f.value("penetration", json::object());
        json skinList = json::array();
        for (auto& e : pen.value("list", json::array())) if (e.value("skin", false) && skinList.size() < 100) skinList.push_back(e);
        g.faces = {{"through_skin_rest", pen.value("skin_rest", 0)}, {"through_skin_wind", pen.value("skin_wind", 0)}, {"through_skin_list", skinList},
                   {"penetration_rest", pen.value("rest", 0)}, {"penetration_wind", pen.value("wind", 0)}, {"interp_only", pen.value("interp_only", 0)},
                   {"guides_inside", f.value("guides_inside", json::object()).value("count", 0)}, {"divergent", f.value("divergent", json::object()).value("count", 0)},
                   {"sliver", f.value("sliver", json::object()).value("count", 0)}, {"big", f.value("big", json::object()).value("count", 0)},
                   {"faces_checked", f.value("faces_checked", 0)}, {"wind", f.value("wind", json())}};
        if (f.contains("error")) g.faces["error"] = f["error"];
    }
    if (!skip.count("colliders")) { bool fail = false; g.colliders = colliderItem(a, c, fail); }
    return g;
}
}  // namespace hgate

// The verdict (2026-10-16, calibrated on marlene_user_1811 / regis_mcp_6 / yennefer_user_1112 / keira_mcp_4: the user's own
// approved saves show a few rest offenders, loops, roots off a wrong scalp mask and tens of faces through the skin under
// wind): every item must be NO WORSE than the reference (the user's snapshot the agent started from, or the build as
// opened), and the absolute targets of the checklist are reported next to it. strict: true also fails on the absolute
// targets (0 roots off the scalp, 0 loops, 0 offenders, 0 faces through the skin, 0 misbound). Wind-based counts carry
// the SDK's simulation noise (10-30 % run to run), so they get a tolerance of x1.35 + 5; rest geometry counts + 2.
json handbackGateCmd(App& a, const json& c, const std::string& out) {
    using namespace hgate;
    json r = {{"do", "handback_gate"}};
    if (!a.loaded) { r["error"] = "no build open"; return r; }
    bool quit = false;
    auto run = [&](const json& cmd) { return a.serveCommand(cmd, out, quit); };
    std::string tag = c.value("tag", std::string("gate"));
    std::string dir = out + "\\gate";
    fs::create_directories(dir);
    int size = c.value("size", 1000);
    bool strict = c.value("strict", false);
    json items = json::object();
    json failed = json::array(), warnings = json::array();
    auto item = [&](const std::string& name, json v, bool fail, const std::string& why) {
        v["pass"] = !fail;
        if (fail) { v["why"] = why; failed.push_back(name + ": " + why); }
        items[name] = v;
    };
    std::set<std::string> skip;
    if (c.contains("skip")) for (auto& s : c["skip"]) skip.insert(s.get<std::string>());
    auto want = [&](const std::string& n) { return !skip.count(n); };
    auto t0 = std::chrono::steady_clock::now();
    auto secs = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    json timing = json::object();

    // 1. the current groom: numbers
    Geo cur = measure(a, c, out, skip);
    timing["measure"] = secs();
    // 2. the current groom: renders per camera (plain, scalp overlay, hair mask)
    auto cams = cameras(a, c);
    struct PerCam { long scalp = -1, hair = -1, refScalp = -1, refHair = -1, flare = -1, loss = -1; std::string png, scalpPng, maskPng; };
    std::vector<PerCam> pc(cams.size());
    std::vector<std::string> plain;
    bool wantViews = want("views"), wantScalp = want("scalp"), wantSil = want("silhouette");
    auto renderSet = [&](const std::string& pre, bool doPlain, bool doScalp, bool doMask, std::vector<PerCam>& P, bool ref) {
        for (size_t i = 0; i < cams.size(); i++) {
            const json& cam = cams[i].cam;
            json base = {{"do", "render"}, {"eye", cam["eye"]}, {"target", cam["target"]}, {"fovy", cam.value("fovy", 26.0f)}, {"size", size}};
            if (doPlain) {
                json q = base; q["path"] = dir + "\\" + pre + "_" + cams[i].name + ".png";
                run(q);
                if (!ref) { P[i].png = q["path"]; plain.push_back(q["path"]); }
            }
            if (doScalp) {
                json q = base; q["overlay"] = "scalp"; q["path"] = dir + "\\" + pre + "_" + cams[i].name + "_scalp.png";
                run(q);
                Img im = loadImg(q["path"]);
                long n = im.ok() ? countPx(im, isGreen) : -1;
                if (ref) P[i].refScalp = n; else { P[i].scalp = n; P[i].scalpPng = q["path"]; }
            }
            if (doMask) {
                json q = base; q["overlay"] = "mask"; q["path"] = dir + "\\" + pre + "_" + cams[i].name + "_mask.png";
                run(q);
                if (!ref) P[i].maskPng = q["path"];
                Img im = loadImg(q["path"]);
                long n = im.ok() ? countPx(im, isHair) : -1;
                if (ref) P[i].refHair = n; else P[i].hair = n;
            }
        }
    };
    renderSet(tag, wantViews, wantScalp, wantSil, pc, false);
    timing["renders"] = secs();
    // 3. the reference: a project path ("reference"), else the build as opened ("baseline"): numbers + scalp / mask renders
    std::string refPath = c.value("reference", std::string());
    bool refIsBaseline = refPath.empty() || refPath == "baseline";
    std::string refNote, refLabel = refIsBaseline ? "baseline (build as opened)" : refPath;
    if (!refIsBaseline && !fs::exists(refPath)) { refNote = "reference project not found, using the build as opened: " + refPath; refIsBaseline = true; refLabel = "baseline (reference not found)"; }
    Geo ref;
    bool refOk = false;
    {
        auto doRef = [&]() { ref = measure(a, c, out, skip); renderSet(tag + "_ref", false, wantScalp, wantSil, pc, true); refOk = true; };
        if (refIsBaseline) {
            BaselineScope bs(a, true);
            doRef();
        } else {
            // swap the reference project in (the current state goes to a temp project first) and back
            std::string tmp = dir + "\\" + tag + "_current_tmp.groomproj.json";
            std::string keepPath = a.projectPath;
            int keepSaved = a.savedEdits;
            a.saveProject(tmp);
            if (a.loadProject(refPath)) { settleRest(a, 0.5); doRef(); }
            else refNote = "cannot open the reference project: " + a.status;
            a.loadProject(tmp);
            settleRest(a, 0.5);
            a.projectPath = keepPath; a.savedEdits = keepSaved;
            std::error_code ec; fs::remove(tmp, ec);
        }
    }
    timing["reference"] = secs();
    // 4. verdicts: current vs reference (+ tolerance), absolute targets when strict
    auto cnt = [](const json& j, const char* k) { return j.is_object() ? j.value(k, 0) : 0; };
    auto worse = [&](int curV, int refV, bool windy) {
        int tol = windy ? int(std::lround(refV * 0.35)) + 5 : 2;
        return refOk ? curV > refV + tol : false;
    };
    if (want("roots")) {
        json v = cur.roots; v["reference"] = refOk ? json{{"roots_off_scalp", cnt(ref.roots, "roots_off_scalp")}, {"guides_over_face", cnt(ref.roots, "guides_over_face")}} : json();
        int off = cnt(cur.roots, "roots_off_scalp"), face = cnt(cur.roots, "guides_over_face");
        int rOff = cnt(ref.roots, "roots_off_scalp"), rFace = cnt(ref.roots, "guides_over_face");
        bool fail = cur.roots.contains("error") || off > rOff || face > rFace || (strict && (off > c.value("max_roots_off", 0) || face > c.value("max_over_face", 0)));
        v["target"] = "0 roots off the painted scalp, 0 guides over the face (user rules 2026-10-15)";
        if (off > 0 && !fail) warnings.push_back("roots: " + std::to_string(off) + " roots off the painted scalp (same as the reference; the scalp mask may be wrong for this head)");
        item("roots", v, fail, cur.roots.contains("error") ? cur.roots["error"].get<std::string>() : std::to_string(off) + " roots off the painted scalp (reference " + std::to_string(rOff) + "), " + std::to_string(face) + " guides over the face (reference " + std::to_string(rFace) + ")");
    }
    if (want("loops")) {
        json v = cur.loops; int n = cnt(cur.loops, "count"), rn = cnt(ref.loops, "count");
        v["reference"] = rn;
        bool fail = n > rn || (strict && n > c.value("max_loops", 0));
        if (n > 0 && !fail) warnings.push_back("loops: " + std::to_string(n) + " looping guides (reference " + std::to_string(rn) + ")");
        item("loops", v, fail, std::to_string(n) + " looping guides (reference " + std::to_string(rn) + ")");
    }
    if (want("direction")) {
        json v = cur.direction;
        int nRest = cnt(cur.direction, "rest_offenders"), nWind = cnt(cur.direction, "wind_offenders_total");
        int rRest = cnt(ref.direction, "rest_offenders"), rWind = cnt(ref.direction, "wind_offenders_total");
        v["reference"] = {{"rest_offenders", rRest}, {"wind_offenders_total", rWind}};
        bool fail = worse(nRest, rRest, false) || worse(nWind, rWind, true) || (strict && (nRest > c.value("max_rest", 0) || nWind > c.value("max_wind", 0)));
        if ((nRest > 0 || nWind > 0) && !fail) warnings.push_back("direction: " + std::to_string(nRest) + " rest / " + std::to_string(nWind) + " wind offenders (reference " + std::to_string(rRest) + " / " + std::to_string(rWind) + "): run studio_check_direction fix");
        item("direction", v, fail, std::to_string(nRest) + " rest offenders (reference " + std::to_string(rRest) + "), " + std::to_string(nWind) + " wind offenders over 8 runs (reference " + std::to_string(rWind) + ")");
    }
    if (want("faces")) {
        json v = cur.faces;
        int sr = cnt(cur.faces, "through_skin_rest"), sw = cnt(cur.faces, "through_skin_wind"), gi = cnt(cur.faces, "guides_inside");
        int rsr = cnt(ref.faces, "through_skin_rest"), rsw = cnt(ref.faces, "through_skin_wind"), rgi = cnt(ref.faces, "guides_inside");
        v["reference"] = {{"through_skin_rest", rsr}, {"through_skin_wind", rsw}, {"guides_inside", rgi}, {"interp_only", cnt(ref.faces, "interp_only")}, {"divergent", cnt(ref.faces, "divergent")}};
        bool fail = cur.faces.contains("error") || worse(sr, rsr, true) || worse(sw, rsw, true) || worse(gi, rgi, true) || (strict && sr + sw > c.value("max_through_skin", 0));
        if ((sr > 0 || sw > 0) && !fail) warnings.push_back("faces: " + std::to_string(sr) + " rest / " + std::to_string(sw) + " wind faces through the skin (reference " + std::to_string(rsr) + " / " + std::to_string(rsw) + "): check the low front / nape renders");
        if (cnt(cur.faces, "interp_only") > 0) warnings.push_back("faces: " + std::to_string(cnt(cur.faces, "interp_only")) + " interp-only penetrations (reference " + std::to_string(cnt(ref.faces, "interp_only")) + "; under the scalp = invisible hair, the bald-patch mechanism)");
        item("faces", v, fail, cur.faces.contains("error") ? cur.faces["error"].get<std::string>() : std::to_string(sr) + " faces through the skin at rest (reference " + std::to_string(rsr) + "), " + std::to_string(sw) + " under wind (reference " + std::to_string(rsw) + "), " + std::to_string(gi) + " guides inside (reference " + std::to_string(rgi) + ")");
    }
    if (want("colliders")) {
        json v = cur.colliders; int mis = cnt(cur.colliders, "misbound"), rmis = cnt(ref.colliders, "misbound");
        v["reference"] = {{"misbound", rmis}};
        bool fail = mis > rmis || (strict && mis > 0);
        if (mis > 0 && !fail) warnings.push_back("colliders: " + std::to_string(mis) + " head-height spheres not bound to the head (same as the reference): studio_rebind_colliders");
        if (cnt(cur.colliders, "small_isolated") > 0) warnings.push_back("colliders: " + std::to_string(cnt(cur.colliders, "small_isolated")) + " small isolated spheres (< small_mm)");
        item("colliders", v, fail, std::to_string(mis) + " head-height spheres bound to another bone (reference " + std::to_string(rmis) + ")");
    }
    // silhouette: current hair outside the reference's dilated mask (flare) and reference hair outside ours (loss)
    if (wantSil && refOk) {
        int fl = std::max(1, int(std::lround(c.value("flare_px", 10.0f) * size / 1000.0f)));
        for (size_t i = 0; i < cams.size(); i++) {
            Img curI = loadImg(pc[i].maskPng), refI = loadImg(dir + "\\" + tag + "_ref_" + cams[i].name + "_mask.png");
            if (!curI.ok() || !refI.ok() || curI.w != refI.w || curI.h != refI.h) continue;
            auto mc = maskOf(curI, isHair), mr = maskOf(refI, isHair);
            auto dc = dilate(mc, curI.w, curI.h, fl), dr = dilate(mr, refI.w, refI.h, fl);
            long flare = 0, loss = 0;
            for (size_t k = 0; k < mc.size(); k++) { if (mc[k] && !dr[k]) flare++; if (mr[k] && !dc[k]) loss++; }
            pc[i].flare = flare; pc[i].loss = loss;
        }
    }
    if (wantScalp) {
        float tol = c.value("scalp_tol", 0.10f); long abs_ = c.value("scalp_abs", 400L);
        json per = json::array(); std::vector<std::string> bad;
        for (size_t i = 0; i < cams.size(); i++) {
            json e = {{"camera", cams[i].name}, {"scalp_px", pc[i].scalp}, {"reference_px", pc[i].refScalp}, {"png", pc[i].scalpPng}};
            bool b = pc[i].scalp >= 0 && pc[i].refScalp >= 0 && pc[i].scalp > pc[i].refScalp * (1 + tol) + abs_;
            e["worse"] = b;
            if (b) bad.push_back(cams[i].name + " " + std::to_string(pc[i].scalp) + " vs " + std::to_string(pc[i].refScalp));
            per.push_back(e);
        }
        json v = {{"cameras", per}, {"reference", refLabel}, {"tol", tol}, {"abs_px", abs_}, {"note", "green scalp pixels per gate camera (size px), current vs reference; studio_pick the spot when a camera is worse"}};
        if (!refNote.empty()) v["note_reference"] = refNote;
        std::string why; for (auto& w : bad) why += (why.empty() ? "" : "; ") + w;
        item("scalp", v, !bad.empty(), "more scalp visible than the reference: " + why);
    }
    if (wantSil) {
        float frac = c.value("flare_frac", 0.03f); long minPx = c.value("flare_min_px", 300L);
        json per = json::array(); std::vector<std::string> bad;
        for (size_t i = 0; i < cams.size(); i++) {
            json e = {{"camera", cams[i].name}, {"hair_px", pc[i].hair}, {"reference_hair_px", pc[i].refHair}, {"flare_px", pc[i].flare}, {"loss_px", pc[i].loss}};
            bool fb = pc[i].flare >= 0 && pc[i].refHair > 0 && pc[i].flare > std::max<long>(minPx, long(frac * pc[i].refHair));
            bool lb = pc[i].loss >= 0 && pc[i].refHair > 0 && pc[i].loss > std::max<long>(minPx, long(c.value("loss_frac", 0.06f) * pc[i].refHair));
            e["flare_bad"] = fb; e["loss_bad"] = lb;
            if (fb) bad.push_back(cams[i].name + " flare " + std::to_string(pc[i].flare) + " px");
            if (lb) bad.push_back(cams[i].name + " loss " + std::to_string(pc[i].loss) + " px");
            per.push_back(e);
        }
        json v = {{"cameras", per}, {"reference", refLabel}, {"flare_frac", frac}, {"min_px", minPx}, {"dilate_px", c.value("flare_px", 10.0f)},
                  {"note", "flare = our hair outside the reference silhouette (wider / fuller than the reference: ear flare, standing tufts); loss = reference hair we no longer cover. Look at the camera's _mask.png pair"}};
        if (!refNote.empty()) v["note_reference"] = refNote;
        std::string why; for (auto& w : bad) why += (why.empty() ? "" : "; ") + w;
        item("silhouette", v, !bad.empty() && !c.value("silhouette_info_only", false), "silhouette differs from the reference: " + why);
    }
    std::string sheetPath;
    if (wantViews) {
        sheetPath = dir + "\\" + tag + "_sheet.png";
        bool ok = sheet(plain, sheetPath, c.value("tile", 500), 5);
        json names = json::array(); for (auto& cm : cams) names.push_back(cm.name);
        item("views", {{"sheet", ok ? sheetPath : ""}, {"pngs", plain}, {"order", names}, {"note", "look at every tile at full size against the reference; the sheet is 5 tiles per row in `order`"}}, !ok, "contact sheet not written");
    }
    timing["views"] = secs();
    // 5. export
    if (want("export")) {
        bool fail = false;
        json v = exportItem(a, c, fail);
        std::string why = v.value("error", std::string());
        if (why.empty()) {
            if (!v.value("missing", json::array()).empty()) why += std::to_string(v["missing"].size()) + " look keys missing from look.extra_set; ";
            if (!v.value("different", json::array()).empty()) why += std::to_string(v["different"].size()) + " look keys differ; ";
            if (!v.value("studio_only_zero", json::array()).empty()) why += "Studio-only colour keys at 0 (" + v["studio_only_zero"].dump() + "); ";
            if (v.value("beard_not_coarse_short", false)) why += "beard not on Coarse short; ";
        }
        item("export", v, fail, why);
        timing["export"] = secs();
    }
    bool ok = failed.empty();
    r["pass"] = ok; r["failed"] = failed; r["warnings"] = warnings; r["items"] = items; r["sheet"] = sheetPath; r["dir"] = dir;
    r["reference"] = refLabel; r["strict"] = strict;
    r["timing_s"] = timing; r["project"] = a.projectPath; r["config"] = a.configName;
    r["guides"] = a.groom.numAlive(); r["faces"] = a.groom.faces.size(); r["ops"] = a.groom.ops.size();
    json summary = {{"failed", failed}, {"warnings", warnings}};
    gate::record(a, ok, summary, sheetPath);
    std::ofstream(dir + "\\" + tag + "_gate.json") << r.dump(1);
    r["report"] = dir + "\\" + tag + "_gate.json";
    logf("handback_gate: %s (%d failed items, %d warnings) in %.0f s", ok ? "PASS" : "FAIL", (int)failed.size(), (int)warnings.size(), secs());
    return r;
}
