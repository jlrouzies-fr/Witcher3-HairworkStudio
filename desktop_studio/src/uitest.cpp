// Desktop Groom Studio - built-in UI test mode (--ui-test script.json). Our own code (MIT).
// Events are injected straight into Dear ImGui (AddMousePosEvent / AddMouseButtonEvent / AddKeyEvent): the OS cursor,
// keyboard and window focus are never touched. Widgets are found by label through ImGui's test-engine item hooks.
#include <windows.h>

#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_internal.h"
#include "app.h"
#include "uitest.h"

namespace fs = std::filesystem;

// ------------------------------------------------------------------------------------------------ item registry
struct ItemRec { ImRect bb; std::string label; };
static std::map<ImGuiID, ImRect> s_bbNow;
static std::vector<ItemRec> s_itemsNow, s_itemsPrev;

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) { s_bbNow[id] = bb; }
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    auto it = s_bbNow.find(id);
    if (it != s_bbNow.end() && label) s_itemsNow.push_back({it->second, label});
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

static std::string visible(const std::string& l) {
    size_t p = l.find("##");
    return p == std::string::npos ? l : l.substr(0, p);
}

// ------------------------------------------------------------------------------------------------ runner
bool UiTest::load(const std::string& path, App& app) {
    try { std::ifstream f(path); script = json::parse(f); } catch (std::exception& e) { logf("ui-test: cannot read %s: %s", path.c_str(), e.what()); return false; }
    out = script.value("out", app.set.repo + "\\build\\desktop_studio\\uitest");
    fs::create_directories(out);
    ImGui::GetCurrentContext()->TestEngineHookItems = true;
    app.splashOn = script.value("splash", false);   // the splash is off in UI tests unless the script asks for it
    active = true;
    result = json::object();
    result["steps"] = json::array();
    logf("ui-test: %s (%d steps) -> %s", path.c_str(), (int)script["steps"].size(), out.c_str());
    return true;
}

bool UiTest::find(const std::string& label, int nth, ImVec2& c) const {
    int n = 0;
    for (auto it = s_itemsPrev.rbegin(); it != s_itemsPrev.rend(); ++it) { (void)it; }
    for (auto& r : s_itemsPrev) {
        if (r.label == label || visible(r.label) == label) {
            if (n++ == nth) { c = ImVec2((r.bb.Min.x + r.bb.Max.x) * 0.5f, (r.bb.Min.y + r.bb.Max.y) * 0.5f); return true; }
        }
    }
    return false;
}

static ImGuiKey keyOf(const std::string& k) {
    if (k == "F12") return ImGuiKey_F12;
    if (k == "F1") return ImGuiKey_F1;
    if (k == "F6") return ImGuiKey_F6;
    if (k == "Escape") return ImGuiKey_Escape;
    if (k == "Space") return ImGuiKey_Space;
    if (k == "Delete") return ImGuiKey_Delete;
    if (k == "Tab") return ImGuiKey_Tab;
    if (k == "Enter") return ImGuiKey_Enter;
    if (k == "Period" || k == ".") return ImGuiKey_Period;
    if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z') return ImGuiKey(ImGuiKey_A + (k[0] - 'A'));
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') return ImGuiKey(ImGuiKey_0 + (k[0] - '0'));
    return ImGuiKey_None;
}

void UiTest::record(const json& step, bool ok, const std::string& note) {
    json r = step;
    r["ok"] = ok;
    if (!note.empty()) r["note"] = note;
    result["steps"].push_back(r);
    logf("ui-test step %d %s: %s %s", (int)idx, ok ? "ok" : "FAILED", step.dump().c_str(), note.c_str());
    if (!ok) failures++;
}

// called once per frame BEFORE ImGui::NewFrame(); returns false when the script is over (app should close)
bool UiTest::tick(App& app, HWND hwnd) {
    if (!active) return true;
    ImGuiIO& io = ImGui::GetIO();
    s_itemsPrev.swap(s_itemsNow);
    s_itemsNow.clear();
    s_bbNow.clear();
    io.AddFocusEvent(true);
    auto& steps = script["steps"];
    if (wait > 0) { wait--; return true; }
    if (idx >= steps.size()) {
        if (!finished) {
            finished = true;
            result["failures"] = failures;
            result["crash"] = false;
            std::ofstream(out + "\\ui_test_result.json") << result.dump(1);
            logf("ui-test done: %d failure(s)", failures);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return false;
    }
    const json& s = steps[idx];
    std::string d = s.value("do", "");
    if (phase == 0) phaseFrames = 0;
    phaseFrames++;
    auto next = [&](bool ok, const std::string& note = "") { record(s, ok, note); idx++; phase = 0; wait = s.value("then_wait", 2); };
    if (d == "wait") { wait = s.value("frames", 30); idx++; return true; }
    if (d == "wait_job") {
        if (app.job.running && phaseFrames < s.value("max_frames", 3600)) { phase = 1; return true; }
        next(!app.job.running, app.status);
        return true;
    }
    if (d == "shot") {
        if (phase == 0) { app.pendingShot = out + "\\" + s.value("name", "shot") + ".png"; phase = 1; return true; }
        next(fs::exists(out + "\\" + s.value("name", "shot") + ".png"));
        return true;
    }
    if (d == "click" || d == "dclick" || d == "rclick") {
        ImVec2 c;
        if (phase >= 2) { c = clickPos; goto have; }   // the widget may disappear once clicked (picker card -> editor)
        {
        bool found = s.contains("xy") ? (c = ImVec2(s["xy"][0], s["xy"][1]), true) : find(s.value("label", ""), s.value("nth", 0), c);
        if (found && s.contains("rows")) c.y += s["rows"].get<float>() * ImGui::GetFrameHeightWithSpacing();   // widget below a label
        if (found && s.contains("dx")) c.x += s["dx"].get<float>();
        if (!found) {
            if (phaseFrames < 60) { phase = 1; return true; }       // the item may appear a few frames later
            next(false, "widget not found: " + s.value("label", ""));
            return true;
        }
        clickPos = c;
        }
    have:
        int btn = d == "rclick" ? 1 : 0;
        int clicks = d == "dclick" ? 2 : 1;
        // frames: 1 move, 2 down, 3 up (4 down, 5 up for double)
        if (phase <= 1) { io.AddMousePosEvent(c.x, c.y); phase = 2; return true; }
        int k = phase - 2;
        if (k < clicks * 2) { io.AddMouseButtonEvent(btn, k % 2 == 0); phase++; return true; }
        next(true);
        return true;
    }
    if (d == "drag") {   // {"xy":[x0,y0], "to":[x1,y1], "button":1}
        int btn = s.value("button", 0);
        ImVec2 a(s["xy"][0], s["xy"][1]), b(s["to"][0], s["to"][1]);
        int steps_ = 20;
        if (phase <= 1) { io.AddMousePosEvent(a.x, a.y); phase = 2; return true; }
        if (phase == 2) { io.AddMouseButtonEvent(btn, true); phase = 3; return true; }
        int k = phase - 3;
        if (k < steps_) { float t = (k + 1) / float(steps_); io.AddMousePosEvent(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); phase++; return true; }
        io.AddMouseButtonEvent(btn, false);
        next(true);
        return true;
    }
    if (d == "wheel") {
        io.AddMousePosEvent(s["xy"][0], s["xy"][1]);
        io.AddMouseWheelEvent(0, s.value("dy", 1.0f));
        next(true);
        return true;
    }
    if (d == "key") {
        ImGuiKey k = keyOf(s.value("key", ""));
        bool ctrl = s.value("ctrl", false), shift = s.value("shift", false);
        if (phase == 0) {
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
            io.AddKeyEvent(k, true);
            phase = 1;
            return true;
        }
        io.AddKeyEvent(k, false);
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
        next(k != ImGuiKey_None);
        return true;
    }
    if (d == "hover") {   // move the mouse onto a widget (tooltips, hover states)
        ImVec2 c;
        bool found = s.contains("xy") ? (c = ImVec2(s["xy"][0], s["xy"][1]), true) : find(s.value("label", ""), s.value("nth", 0), c);
        if (!found) {
            if (phaseFrames < 60) { phase = 1; return true; }
            next(false, "widget not found: " + s.value("label", ""));
            return true;
        }
        if (s.contains("dx")) c.x += s["dx"].get<float>();
        io.AddMousePosEvent(c.x, c.y);
        next(true);
        return true;
    }
    if (d == "text") { io.AddInputCharactersUTF8(s.value("text", "").c_str()); next(true); return true; }
    if (d == "move") { io.AddMousePosEvent(s["xy"][0], s["xy"][1]); next(true); return true; }
    if (d == "expect") {
        bool ok = true;
        std::string note;
        if (s.contains("loaded")) { ok &= app.loaded == s["loaded"].get<bool>(); note += " loaded=" + std::to_string(app.loaded); }
        if (s.contains("config")) { ok &= app.configName == s["config"].get<std::string>(); note += " config=" + app.configName; }
        if (s.contains("picker")) { ok &= app.pickerOpen == s["picker"].get<bool>(); note += " picker=" + std::to_string(app.pickerOpen); }
        if (s.contains("simulate")) { ok &= app.simulate == s["simulate"].get<bool>(); note += " simulate=" + std::to_string(app.simulate); }
        if (s.contains("ops")) { ok &= (int)app.groom.ops.size() == s["ops"].get<int>(); note += " ops=" + std::to_string(app.groom.ops.size()); }
        if (s.contains("motion")) { ok &= std::string(motionName(app.motion.preset)).rfind(s["motion"].get<std::string>(), 0) == 0; note += std::string(" motion=") + motionName(app.motion.preset); }
        if (s.contains("material")) { ok &= app.presetSel < (int)app.presets.size() && app.presets[app.presetSel].name.rfind(s["material"].get<std::string>(), 0) == 0; }
        if (s.contains("widget")) { ImVec2 c; bool f = find(s["widget"], 0, c); ok &= f; note += f ? " widget found" : " widget missing"; }
        if (s.contains("tod")) { ok &= app.rig.tod == s["tod"].get<int>(); note += " tod=" + std::to_string(app.rig.tod); }
        if (s.contains("shadows")) { ok &= app.rig.shadows == s["shadows"].get<bool>(); note += " shadows=" + std::to_string(app.rig.shadows); }
        if (s.contains("paused")) { ok &= app.paused == s["paused"].get<bool>(); note += " paused=" + std::to_string(app.paused); }
        if (s.contains("tool")) { ok &= (int)app.tool == s["tool"].get<int>(); note += " tool=" + std::to_string((int)app.tool); }
        if (s.contains("editpose")) { ok &= app.editPose == s["editpose"].get<bool>(); note += " editpose=" + std::to_string(app.editPose); }
        if (s.contains("hairdebug")) { ok &= app.hairDebug == s["hairdebug"].get<int>(); note += " hairdebug=" + std::to_string(app.hairDebug); }
        if (s.contains("light")) { ok &= app.lightOpen == s["light"].get<bool>(); note += " light=" + std::to_string(app.lightOpen); }
        if (s.contains("camshot")) { ok &= app.camShot == s["camshot"].get<int>(); note += " camshot=" + std::to_string(app.camShot); }
        if (s.contains("symmetry")) { ok &= app.symmetry == s["symmetry"].get<bool>(); note += " symmetry=" + std::to_string(app.symmetry); }
        if (s.contains("guides")) { ok &= app.showGuideOverlay == s["guides"].get<bool>(); note += " guides=" + std::to_string(app.showGuideOverlay); }
        if (s.contains("exported")) { ok &= !app.lastExport.empty() == s["exported"].get<bool>(); note += " lastExport=" + app.lastExport; }
        if (s.contains("collapsed")) { ok &= app.tbCollapsed == s["collapsed"].get<bool>(); }
        if (s.contains("style")) { std::string sid = app.pickStyleId(); ok &= sid == s["style"].get<std::string>(); note += " style=" + sid; }   // picker: selected card's hairstyle, else the loaded config's
        if (s.contains("editor_style")) { std::string es = app.edStyleId(); ok &= es == s["editor_style"].get<std::string>(); note += " editor_style=" + es; }   // editor switcher selection
        if (s.contains("loaded_style")) { ok &= app.styleId == s["loaded_style"].get<std::string>(); note += " loaded_style=" + app.styleId; }
        note += " hairs_live=" + std::to_string(app.st.hairs) + " status=" + app.status;
        next(ok, note);
        return true;
    }
    if (d == "list_widgets") {   // debug: dump the labels seen last frame
        json l = json::array();
        for (auto& r : s_itemsPrev) l.push_back(r.label);
        std::ofstream(out + "\\widgets.json") << l.dump(1);
        next(true, std::to_string(l.size()) + " widgets");
        return true;
    }
    next(false, "unknown step");
    return true;
}
