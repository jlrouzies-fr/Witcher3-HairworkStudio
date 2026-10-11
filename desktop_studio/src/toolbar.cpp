// Desktop Groom Studio - viewport overlays: floating top bar (view / camera / light), bottom playback bar, top-left HUD
// (edit-pose chip, selection, stats), axis gizmo, tool hint card; and the sun gizmo of the Light window.
// Design: design/ui_direction1 (IMPLEMENTATION.md 5.3). Our own code (MIT).
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>

#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_internal.h"
#include "app.h"
#include "lighting.h"
#include "ui_widgets.h"

using namespace DirectX;
using w::S;
using w::S2;

static const ImVec4 T_MUTED = tv(tok::MUTED), T_GLOW = tv(tok::GLOW);
static void tbTip(const char* t) { w::tip(t); }

void App::cameraShot(int shot) {
    std::string k = std::to_string(shot);
    if (loaded && pack.shots.contains(k)) {
        auto& s = pack.shots[k];
        XMFLOAT3 e{s["eye"][0], s["eye"][1], s["eye"][2]}, t{s["target"][0], s["target"][1], s["target"][2]};
        float dx = e.x - t.x, dy = e.y - t.y, dz = e.z - t.z, dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        (void)t;
        // the shot's direction (HairLab framing), centred on the head at a distance that keeps the whole head and the
        // toolbar clear of each other
        cam.target = {pack.headC.x, pack.headC.y, pack.headC.z - 0.03f};
        cam.pitch = std::asin(dz / std::max(dist, 1e-4f));
        cam.yaw = std::atan2(dx, dy);
        cam.dist = 0.95f;
    } else {
        setView(shot == 4 ? "front" : shot == 3 ? "34" : shot == 2 ? "right" : "back");
    }
    turntable = false;
    camShot = shot;
}

void App::frameHead() {
    if (!loaded) return;
    cam.target = {pack.headC.x, pack.headC.y, pack.headC.z - 0.04f};
    cam.dist = 0.95f;
}

void App::stopReset() {
    paused = true;
    motion.t = 0;
    teleport = true;
    status = "Stopped: rest pose";
}

// sun gizmo: top-down azimuth dial + elevation arc; drag to set
void App::drawSunGizmo(float size) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = size * 0.42f;
    ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    ImGui::InvisibleButton("##sunaz", ImVec2(size, size));
    tbTip("Sun azimuth: drag around the dial (top = the character's face side).");
    if (ImGui::IsItemActive()) {
        ImVec2 m = ImGui::GetIO().MousePos;
        rig.sunAz = std::atan2(m.x - c.x, -(m.y - c.y));
    }
    d->AddCircle(c, r, ImGui::GetColorU32(ImVec4(0.235f, 0.25f, 0.27f, 1)), 40, 1.5f);
    d->AddCircleFilled(ImVec2(c.x, c.y), r * 0.18f, ImGui::GetColorU32(ImVec4(0.5f, 0.52f, 0.55f, 1)), 16);   // the head
    d->AddLine(ImVec2(c.x, c.y - r * 0.18f), ImVec2(c.x, c.y - r * 0.32f), ImGui::GetColorU32(T_MUTED), 1.5f);  // face direction
    ImVec2 sp(c.x + r * std::sin(rig.sunAz) * std::cos(rig.sunEl), c.y - r * std::cos(rig.sunAz) * std::cos(rig.sunEl));
    d->AddLine(c, sp, ImGui::GetColorU32(ImVec4(0.94f, 0.63f, 0.25f, 0.5f)), 1.5f);
    d->AddCircleFilled(sp, size * 0.06f, ImGui::GetColorU32(T_GLOW), 16);
    ImGui::SameLine();
    // elevation arc (side view)
    ImVec2 q = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sunel", ImVec2(size, size));
    tbTip("Sun elevation: drag along the arc (horizon .. zenith).");
    ImVec2 o(q.x + size * 0.1f, q.y + size * 0.85f);
    float R = size * 0.75f;
    if (ImGui::IsItemActive()) {
        ImVec2 m = ImGui::GetIO().MousePos;
        rig.sunEl = std::clamp(std::atan2(o.y - m.y, m.x - o.x), 0.02f, 1.55f);
    }
    d->PathArcTo(o, R, -1.5708f, 0.0f, 24);
    d->PathStroke(ImGui::GetColorU32(ImVec4(0.235f, 0.25f, 0.27f, 1)), 0, 1.5f);
    d->AddLine(o, ImVec2(o.x + R, o.y), ImGui::GetColorU32(ImVec4(0.235f, 0.25f, 0.27f, 1)), 1.5f);
    ImVec2 ep(o.x + R * std::cos(rig.sunEl), o.y - R * std::sin(rig.sunEl));
    d->AddLine(o, ep, ImGui::GetColorU32(ImVec4(0.94f, 0.63f, 0.25f, 0.5f)), 1.5f);
    d->AddCircleFilled(ep, size * 0.06f, ImGui::GetColorU32(T_GLOW), 16);
}

// ------------------------------------------------------------------------------------------------ bar helpers
static float s_rowTop = 0, s_rowH = 0;   // current bar row (items are centred vertically in it)

static void place(float h) {
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, s_rowTop + (s_rowH - h) * 0.5f));
}

static void barSep(float h) {
    place(s_rowH);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(S(13), s_rowH));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + S(6.5f), p.y + (s_rowH - h) * 0.5f), ImVec2(p.x + S(6.5f), p.y + (s_rowH + h) * 0.5f), tc(tok::IRON), 1);
    ImGui::SameLine(0, 0);
}

static void barToggle(const char* id, const char* glyph, w::Custom ci, bool on, const char* tip, bool& clicked) {
    place(S(34));
    clicked = w::iconToggle(id, glyph, ci, on, tip, 34);
    ImGui::SameLine(0, S(2));
}

// dropdown look: icon + text + chevron; the popup is opened by the caller
static bool dropButton(const char* id, const char* glyph, ImU32 glyphCol, const char* text, float h) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    float wdt = S(9) + (glyph ? S(22) : 0) + ts.x + S(8) + S(14) + S(9);
    place(h);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool r = ImGui::InvisibleButton(id, ImVec2(wdt, h));
    bool hov = ImGui::IsItemHovered() || ImGui::IsPopupOpen((std::string(id) + "_pop").c_str());
    ImDrawList* d = ImGui::GetWindowDrawList();
    if (hov) d->AddRectFilled(p, ImVec2(p.x + wdt, p.y + h), tc(tok::BG3), S(7));
    float x = p.x + S(9), cy = p.y + h * 0.5f;
    if (glyph) { w::icon(d, glyph, ImVec2(x + S(8), cy), S(15), glyphCol); x += S(22); }
    ImGui::PushFont(g_fontUiSemi);
    d->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), tc(tok::TEXT), text);
    ImGui::PopFont();
    x += ts.x + S(8);
    w::icon(d, ICON_LC_CHEVRON_DOWN, ImVec2(x + S(7), cy), S(14), tc(tok::DIM));
    return r;
}

static bool miniSlider(const char* label, float* v, float lo, float hi, float trackW, const char* valText, const char* tip) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    float h = S(30);
    place(h);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float cy = p.y + h * 0.5f;
    const char* e = ImGui::FindRenderedTextEnd(label);
    ImVec2 ls = ImGui::CalcTextSize(label, e);
    float x = p.x + S(4);
    if (ls.x > 0) { d->AddText(ImVec2(x, cy - ls.y * 0.5f), tc(tok::MUTED), label, e); x += ls.x + S(8); }
    ImGui::SetCursorScreenPos(ImVec2(x, p.y));
    float kr = S(6);
    ImGui::InvisibleButton(label, ImVec2(trackW + kr * 2, h));
    bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
    tbTip(tip);
    bool changed = false;
    float t0 = x + kr, t1 = x + kr + trackW;
    if (act) {
        float t = std::clamp((ImGui::GetIO().MousePos.x - t0) / trackW, 0.0f, 1.0f);
        float nv = lo + (hi - lo) * t;
        nv = std::round(nv * 100) / 100;
        if (nv != *v) { *v = nv; changed = true; }
    }
    float t = std::clamp((*v - lo) / (hi - lo), 0.0f, 1.0f), kx = t0 + trackW * t;
    d->AddRectFilled(ImVec2(t0, cy - S(2)), ImVec2(t1, cy + S(2)), hov || act ? tc(0x323940) : tc(tok::BG4), S(2));
    d->AddRectFilled(ImVec2(t0, cy - S(2)), ImVec2(kx, cy + S(2)), tc(tok::AMBER), S(2));
    if (hov || act) d->AddCircleFilled(ImVec2(kx, cy), kr + S(5), tc(tok::GLOW, 0.22f), 20);
    d->AddCircleFilled(ImVec2(kx, cy), kr + S(3), IM_COL32(0, 0, 0, 90), 20);
    d->AddCircleFilled(ImVec2(kx, cy), kr, act ? IM_COL32_WHITE : tc(0xe9e2d8), 20);
    x = t1 + kr + S(8);
    ImGui::PushFont(g_fontUiSemi);
    ImVec2 vs = ImGui::CalcTextSize(valText);
    float vw = std::max(vs.x, S(30));
    d->AddText(ImVec2(x, cy - vs.y * 0.5f), !strcmp(valText, "Off") ? tc(tok::DIM) : tc(tok::TEXT), valText);
    ImGui::PopFont();
    x += vw + S(4);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(x - p.x, h));
    ImGui::SameLine(0, S(2));
    return changed;
}

static void beginBar(const char* name, ImVec2 pos, ImVec2 pivot, float rowH) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(5), S(5)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(2), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(17 / 255.f, 20 / 255.f, 23 / 255.f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, tv(tok::IRON));
    ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImVec2 a = ImGui::GetWindowPos(), b(a.x + ImGui::GetWindowWidth(), a.y + ImGui::GetWindowHeight());
    w::drawShadow(ImGui::GetBackgroundDrawList(), a, b, S(10), 1.0f);
    // inner top highlight
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + S(8), a.y + 1.5f), ImVec2(b.x - S(8), a.y + 1.5f), IM_COL32(255, 255, 255, 8), 1);
    s_rowTop = ImGui::GetCursorScreenPos().y;
    s_rowH = rowH;
}

static float endBar() {
    float wNow = ImGui::GetWindowWidth();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
    return wNow;
}

static const char* todIcon(int i) {
    switch (i) {
    case 0: return ICON_LC_CAMERA;
    case 1: return ICON_LC_SUN;
    case 2: return ICON_LC_SUNRISE;
    case 3: return ICON_LC_SUNRISE;
    case 4: return ICON_LC_SUNSET;
    case 5: return ICON_LC_CLOUD;
    case 6: return ICON_LC_MOON;
    case 8: return ICON_LC_MOON;
    default: return ICON_LC_FLAME;
    }
}

// ------------------------------------------------------------------------------------------------ chips (HUD)
static bool chip(const char* id, const char* glyph, w::Custom ci, const std::string& bold, const std::string& text, bool amber,
                 const char* action, const char* actionId, bool* actionClicked, const char* kbdText = nullptr) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    float h = S(28);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(g_fontUiSemi);
    ImVec2 bs = bold.empty() ? ImVec2(0, 0) : ImGui::CalcTextSize(bold.c_str());
    ImGui::PopFont();
    ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    float actW = action ? ImGui::CalcTextSize(action).x + S(9) + S(16) + S(10) : 0;
    float kw = kbdText ? w::kbdWidth(kbdText) + S(8) : 0;
    float wdt = S(11) + S(16) + S(8) + bs.x + (bold.empty() ? 0 : S(6)) + ts.x + kw + S(11) + actW;
    bool r = ImGui::InvisibleButton(id, ImVec2(wdt - actW, h));
    bool hov = ImGui::IsItemHovered();
    ImVec2 b(p.x + wdt, p.y + h);
    d->AddRectFilled(p, b, amber ? IM_COL32(42, 26, 10, 217) : (hov && !amber && kbdText ? tc(tok::BG3, 0.92f) : IM_COL32(17, 20, 23, 219)), S(7));
    d->AddRect(p, b, amber ? tc(tok::GLOW, 0.5f) : tc(tok::IRON), S(7));
    float x = p.x + S(11), cy = p.y + h * 0.5f;
    ImU32 col = amber ? tc(tok::GLOW) : tc(tok::SOFT);
    if (ci != w::CI_NONE) w::customIcon(d, ci, ImVec2(x + S(7), cy), S(15), amber ? tc(tok::GLOW) : tc(tok::GLOW));
    else w::icon(d, glyph, ImVec2(x + S(7), cy), S(14), amber ? tc(tok::GLOW) : tc(tok::MUTED));
    x += S(16) + S(8);
    if (!bold.empty()) {
        ImGui::PushFont(g_fontUiSemi);
        d->AddText(ImVec2(x, cy - bs.y * 0.5f), tc(tok::TEXT), bold.c_str());
        ImGui::PopFont();
        x += bs.x + S(6);
    }
    d->AddText(ImVec2(x, cy - ts.y * 0.5f), col, text.c_str());
    x += ts.x;
    if (kbdText) { x += S(8); w::kbd(d, ImVec2(x, cy - S(8)), kbdText); x += kw - S(8); }
    if (action) {
        x += S(11);
        d->AddLine(ImVec2(x, p.y + S(6)), ImVec2(x, b.y - S(6)), tc(tok::GLOW, 0.35f), 1);
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        *actionClicked = ImGui::InvisibleButton(actionId, ImVec2(b.x - x, h));
        bool ah = ImGui::IsItemHovered();
        if (ah) d->AddRectFilled(ImVec2(x + 1, p.y + 1), ImVec2(b.x - 1, b.y - 1), tc(tok::GLOW, 0.12f), S(6), ImDrawFlags_RoundCornersRight);
        x += S(9);
        w::icon(d, ICON_LC_PLAY, ImVec2(x + S(6), cy), S(12), tc(tok::TEXT));
        x += S(16);
        ImGui::PushFont(g_fontUiSemi);
        d->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), tc(tok::TEXT), action);
        ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(wdt, h));
    return r;
}

// ------------------------------------------------------------------------------------------------ the overlays
void App::drawToolbar() {
    if (!loaded || pickerOpen) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float vx0 = vp->Pos.x + g_railW, vx1 = vp->Pos.x + vp->Size.x - g_panelW, vy0 = vp->Pos.y + g_titleH, vy1 = vp->Pos.y + vp->Size.y - g_statusH;
    float cx = (vx0 + vx1) * 0.5f;
    bool c = false;
    // ---- top bar: SHOW toggles | camera | time of day + shadows
    beginBar("##viewport_toolbar", ImVec2(cx + tbDx, vy0 + S(14) + tbDy), ImVec2(0.5f, 0.0f), S(34));
    {
        // grip: drag the bar, double-click = home
        place(S(34));
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tbgrip", ImVec2(S(10), S(34)));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) { tbDx += ImGui::GetIO().MouseDelta.x; tbDy += ImGui::GetIO().MouseDelta.y; }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) tbDx = tbDy = 0;
        tbTip("Drag to move the toolbar (double-click: back to the top centre).");
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 2; j++)
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + S(3) + j * S(4), p.y + S(12) + k * S(5)), S(1.1f), tc(tok::DIM));
        ImGui::SameLine(0, S(2));
    }
    if (tbCollapsed) {
        barToggle("##tb_expand", ICON_LC_CHEVRON_DOWN, w::CI_NONE, false, "Show the toolbar", c);
        if (c) tbCollapsed = false;
        place(ImGui::GetTextLineHeight());
        ImGui::TextColored(T_MUTED, "%s  %s  ", paused ? "paused" : "playing", motionName(motion.preset));
    } else {
        place(S(34));
        ImGui::PushFont(g_fontCap);
        ImVec2 ls = ImGui::CalcTextSize("SHOW");
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + S(4), p.y + (S(34) - ls.y) * 0.5f), tc(tok::DIM), "SHOW");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(ls.x + S(10), S(34)));
        ImGui::SameLine(0, 0);
        barToggle("##tb_guides", nullptr, w::CI_GUIDES, showGuideOverlay, "Guides overlay (G)", c); if (c) showGuideOverlay = !showGuideOverlay;
        barToggle("##tb_hair", nullptr, w::CI_HAIR, showHair, "Rendered hair (H)", c); if (c) showHair = !showHair;
        barToggle("##tb_coll", nullptr, w::CI_COLLIDER, showCollision, "Collision capsules (N)", c); if (c) showCollision = !showCollision;
        barToggle("##tb_bones", ICON_LC_BONE, w::CI_NONE, showBones, "Bones (B)", c); if (c) showBones = !showBones;
        barToggle("##tb_wire", ICON_LC_BOX, w::CI_NONE, wireframe, "Wireframe head / body (Z)", c); if (c) wireframe = !wireframe;
        barToggle("##tb_plane", ICON_LC_FLIP_HORIZONTAL_2, w::CI_NONE, showSymPlane, "Symmetry plane (V)", c); if (c) showSymPlane = !showSymPlane;
        barSep(S(24));
        // camera (HairLab shot framing)
        {
            static const char* cams[] = {"Front##tb_cam4", "\xC2\xBE##tb_cam3", "Side##tb_cam2", "Back##tb_cam0"};
            static const char* camTips[] = {"Front, as HairLab shot 4 (1)", "Three-quarter front, as HairLab shot 3 (2)", "Side, as HairLab shot 2 (3)", "Back, as HairLab shot 0 (4)"};
            static const int shots[] = {4, 3, 2, 0};
            int cur = camShot == 4 ? 0 : camShot == 3 ? 1 : camShot == 2 ? 2 : camShot == 0 ? 3 : -1;
            place(S(32));
            if (w::segmented("##tb_cams", cams, 4, &cur, camTips) && cur >= 0) cameraShot(shots[cur]);
            ImGui::SameLine(0, S(2));
        }
        barToggle("##tb_frame", ICON_LC_SCAN, w::CI_NONE, false, "Frame the head (F)", c); if (c) frameHead();
        barSep(S(24));
        // time of day dropdown (L cycles) + shadows
        {
            const TimeOfDay& td = timeOfDay(rig.tod);
            bool open = dropButton("##tb_tod", todIcon(rig.tod), tc(tok::SAND), td.name, S(32));
            tbTip("Time of day (L cycles). The HairLab capture rig is the calibrated one for comparisons.");
            ImVec2 pp(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + S(10));
            if (open) ImGui::OpenPopup("##tb_tod_pop");
            ImGui::SetNextWindowPos(pp);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(6, 6));
            if (ImGui::BeginPopup("##tb_tod_pop")) {
                for (int i = 0; i < timeOfDayCount(); i++) {
                    ImVec2 rp = ImGui::GetCursorScreenPos();
                    ImGui::SetCursorScreenPos(ImVec2(rp.x + S(24), rp.y));
                    if (ImGui::Selectable(timeOfDay(i).name, rig.tod == i, 0, ImVec2(S(206), S(26)))) applyTimeOfDay(rig, i);
                    tbTip(timeOfDay(i).tip);
                    w::icon(ImGui::GetWindowDrawList(), todIcon(i), ImVec2(rp.x + S(10), rp.y + S(13)), S(14), rig.tod == i ? tc(tok::GLOW) : tc(tok::SAND));
                }
                ImGui::Separator();
                w::switchRow("Sky backdrop", &rig.backdrop, "Sky backdrop gradient of the time of day.");
                if (ImGui::Selectable("Advanced...##light")) lightOpen = true;
                tbTip("Light settings: sun direction, intensities, exposure, shadow softness, calibration gains, hair debug views.");
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
            ImGui::SameLine(0, S(2));
        }
        barToggle("##tb_shadow", ICON_LC_CONTRAST, w::CI_NONE, rig.shadows, "Shadows: key-light shadow map on hair, head, body and ground (K)", c); if (c) rig.shadows = !rig.shadows;
        barSep(S(24));
        barToggle("##tb_collapse", ICON_LC_CHEVRON_UP, w::CI_NONE, false, "Collapse the toolbar", c); if (c) tbCollapsed = true;
    }
    endBar();

    // ---- bottom playback bar: play / stop / step | motion | speed, amount | wind
    beginBar("##playback", ImVec2(cx, vy1 - S(16)), ImVec2(0.5f, 1.0f), S(42));
    {
        ImDrawList* d = ImGui::GetWindowDrawList();
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + S(3), ImGui::GetCursorScreenPos().y));
        place(S(38));
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##tb_play", ImVec2(S(38), S(38)))) paused = !paused;
        bool hov = ImGui::IsItemHovered();
        tbTip(paused ? "Play the simulation (Space)" : "Pause (Space)");
        ImVec2 cc(p.x + S(19), p.y + S(19));
        d->AddCircleFilled(cc, S(19) + S(3), tc(tok::AMBER, 0.18f), 32);
        int v0 = d->VtxBuffer.Size;
        d->AddCircleFilled(cc, S(19), tc(hov ? 0xe08e34 : 0xd4832a), 32);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(d, v0, d->VtxBuffer.Size, ImVec2(cc.x, cc.y - S(19)), ImVec2(cc.x, cc.y + S(19)), tc(hov ? 0xe08e34 : 0xd4832a), tc(0xb8691a));
        w::icon(d, paused ? ICON_LC_PLAY : ICON_LC_PAUSE, ImVec2(cc.x + (paused ? S(1) : 0), cc.y), S(16), IM_COL32(27, 16, 6, 255));
        ImGui::SameLine(0, S(4));
        barToggle("##tb_stop", ICON_LC_SQUARE, w::CI_NONE, false, "Stop and reset to rest (Esc)", c); if (c) stopReset();
        barToggle("##tb_step", ICON_LC_STEP_FORWARD, w::CI_NONE, false, "Step one frame (. key)", c); if (c) { paused = true; stepOnce = true; }
        barSep(S(28));
        // motion preset
        {
            static const MotionPreset quick[] = {MotionPreset::Idle, MotionPreset::Walk, MotionPreset::Run, MotionPreset::SpinBody, MotionPreset::Wind,
                                                 MotionPreset::TurnHead, MotionPreset::Nod, MotionPreset::Rest};
            MotionPreset shown = editPose ? savedPreset : motion.preset;
            bool open = dropButton("##tb_motion", ICON_LC_ACTIVITY, tc(tok::GLOW), motionName(shown), S(32));
            tbTip("Motion preset (idle, walk, run, spin, wind, ...)\nHead / body motion driving the fur bones, so sag, swing and inertia show as in game.");
            ImVec2 pp(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y - S(10));
            if (open) ImGui::OpenPopup("##tb_motion_pop");
            ImGui::SetNextWindowPos(pp, ImGuiCond_Always, ImVec2(0, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(6, 6));
            if (ImGui::BeginPopup("##tb_motion_pop")) {
                for (auto m : quick)
                    if (ImGui::Selectable(motionName(m), shown == m, 0, ImVec2(S(200), S(24)))) {
                        if (editPose) resumeMotion();
                        motion.preset = m; motion.t = 0; paused = false;
                    }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
            ImGui::SameLine(0, S(2));
        }
        barSep(S(28));
        char vb[16];
        snprintf(vb, sizeof vb, "%.1f\xC3\x97", motion.speed);
        miniSlider("Speed", &motion.speed, 0.1f, 3.0f, S(76), vb, "Speed\nMotion speed.");
        snprintf(vb, sizeof vb, "%.2f", motion.amount);
        miniSlider("Amount", &motion.amount, 0.0f, 2.0f, S(76), vb, "Amount\nMotion amplitude.");
        barSep(S(28));
        barToggle("##tb_wind", ICON_LC_WIND, w::CI_NONE, windOn || motion.preset == MotionPreset::Wind, "Wind on / off on any motion (J)", c); if (c) windOn = !windOn;
        if (motion.windStrength <= 0.001f) snprintf(vb, sizeof vb, "Off");
        else snprintf(vb, sizeof vb, "%.1f", motion.windStrength);
        miniSlider("##wind", &motion.windStrength, 0.0f, 5.0f, S(56), vb, "Wind strength\nx the fur's windScaler.");
        // loop progress (motion phase) along the bottom edge
        ImVec2 a = ImGui::GetWindowPos();
        float wdt = ImGui::GetWindowWidth(), y = a.y + ImGui::GetWindowHeight() - S(2);
        float ph = (float)std::fmod(std::max(motion.t, 0.0), 2.0) / 2.0f;
        d->AddRectFilled(ImVec2(a.x + S(12), y), ImVec2(a.x + wdt - S(12), y + S(2)), tc(tok::BG4), S(1));
        if (!paused && motion.preset != MotionPreset::Rest)
            w::gradientRect(d, ImVec2(a.x + S(12), y), ImVec2(a.x + S(12) + (wdt - S(24)) * ph, y + S(2)), tc(tok::GLOW, 0.0f), tc(tok::GLOW), 0, 0, true);
    }
    endBar();
    float pbarTop = ImGui::FindWindowByName("##playback") ? ImGui::FindWindowByName("##playback")->Pos.y : vy1 - S(70);

    // ---- top-left HUD: edit pose chip, selection chip, stats
    {
        // below the top bar when the viewport is too narrow for both side by side
        float hy = vy0 + S(16);
        ImGuiWindow* bar = ImGui::FindWindowByName("##viewport_toolbar");
        ImGuiWindow* hud = ImGui::FindWindowByName("##hud");
        if (bar && hud && vx0 + S(16) + hud->Size.x + S(12) > bar->Pos.x && hy < bar->Pos.y + bar->Size.y) hy = bar->Pos.y + bar->Size.y + S(12);
        ImGui::SetNextWindowPos(ImVec2(vx0 + S(16), hy));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##hud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    {
        bool resume = false;
        if (editPose) {
            if (chip("Edit pose##chip", ICON_LC_PERSON_STANDING, w::CI_NONE, "", "Edit pose \xC2\xB7 motion paused", true, "Resume", "Resume##chip", &resume)) { /* chip body: nothing */ }
            tbTip("Edit pose: motion paused at the bind pose, hair at rest (Tab).\nResume restores the motion preset and the simulation (the Select tool also keeps the motion running).");
            if (resume) { resumeMotion(); tool = Tool::Select; }
        } else if (!editWhileMoving) {
            if (chip("Edit pose##chip", ICON_LC_PERSON_STANDING, w::CI_NONE, "", "Edit pose", false, nullptr, nullptr, nullptr, "Tab")) enterEditPose();
            tbTip("Edit pose (Tab)\nPause at the bind pose, hair at rest. Editing tools (Move, Comb, Curl, Paint) enter it too.");
        } else {
            chip("Editing while moving##chip", ICON_LC_ACTIVITY, w::CI_NONE, "", "Editing while moving", false, nullptr, nullptr, nullptr);
            tbTip("Edit while moving is on (Physics and motion): edits are converted back to rest space.");
        }
        auto sl = selList();
        if (!sl.empty()) {
            chip("##selchip", nullptr, w::CI_GUIDES, std::to_string(sl.size()), sl.size() == 1 ? "guide selected" : "guides selected", false, nullptr, nullptr, nullptr);
            tbTip("Selection\nEsc clears it; Del / D / S / C delete, duplicate, shorten, curl.");
        }
        // stats line (moved here from the inspector)
        ImGui::PushFont(g_fontSmall);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* d = ImGui::GetWindowDrawList();
        std::string parts[4][2] = {{std::to_string(groom.numAlive()), " guides"}, {std::to_string(pack.K), " CVs"},
                                   {std::to_string(groom.faces.size()), " faces"}, {std::to_string(pack.spheres.size()), " colliders"}};
        float x = p.x;
        for (int i = 0; i < 4; i++) {
            if (i) { d->AddText(ImVec2(x, p.y), tc(tok::DIM), " \xC2\xB7 "); x += ImGui::CalcTextSize(" \xC2\xB7 ").x; }
            d->AddText(ImVec2(x, p.y), tc(tok::SOFT), parts[i][0].c_str()); x += ImGui::CalcTextSize(parts[i][0].c_str()).x;
            d->AddText(ImVec2(x, p.y), tc(tok::MUTED), parts[i][1].c_str()); x += ImGui::CalcTextSize(parts[i][1].c_str()).x;
        }
        ImGui::Dummy(ImVec2(x - p.x, ImGui::GetFontSize()));
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    // ---- top-right: axis gizmo + fps (background list: over the 3D, under every window)
    {
        ImDrawList* d = ImGui::GetBackgroundDrawList();
        ImVec2 c(vx1 - S(18) - S(32), vy0 + S(16) + S(32));
        d->AddCircleFilled(c, S(32), IM_COL32(17, 20, 23, 178), 40);
        d->AddCircle(c, S(32), tc(tok::IRON), 40, 1);
        struct Ax { float x, y, z; ImU32 col; const char* n; };
        Ax ax[3];
        const ImU32 cols[3] = {tc(0xd66a5a), tc(0x8fbf8a), tc(0x8ea9c9)};
        const char* nm[3] = {"X", "Y", "Z"};
        for (int i = 0; i < 3; i++) ax[i] = {viewM.m[i][0], -viewM.m[i][1], viewM.m[i][2], cols[i], nm[i]};
        int order[3] = {0, 1, 2};
        std::sort(order, order + 3, [&](int a2, int b2) { return ax[a2].z > ax[b2].z; });   // far first (view space: +z away)
        for (int k = 0; k < 3; k++) {
            Ax& A = ax[order[k]];
            ImVec2 e(c.x + A.x * S(21), c.y + A.y * S(21));
            d->AddLine(c, e, A.col, S(1.6f));
            d->AddCircleFilled(e, S(6.5f), A.col, 16);
            ImVec2 ts = w::textSize(g_fontBadge, A.n);
            d->AddText(g_fontBadge, g_fontBadge->FontSize, ImVec2(e.x - ts.x * 0.5f + S(0.5f), e.y - ts.y * 0.5f), IM_COL32(15, 17, 19, 255), A.n);
        }
        d->AddCircleFilled(c, S(2.5f), tc(tok::SOFT), 10);
        char fb[48];
        snprintf(fb, sizeof fb, "%.0f fps \xC2\xB7 %.1f ms", fps, fps > 0 ? 1000.0f / fps : 0.0f);
        ImVec2 fs = w::textSize(g_fontSmall, fb);
        d->AddText(g_fontSmall, S(11), ImVec2(vx1 - S(20) - fs.x * 11 / 11.5f, vy0 + S(86)), tc(tok::DIM), fb);
    }

    // ---- bottom-left hint card: current tool + its mouse / key hints
    {
        struct Hint { const char* key; const char* glyph; const char* text; };
        const char* name = "Select";
        const char* glyph = ICON_LC_MOUSE_POINTER_2;
        w::Custom ci = w::CI_NONE;
        std::vector<Hint> hs;
        char brush[32];
        snprintf(brush, sizeof brush, "Brush %.0f px", brushPx);
        switch (tool) {
        case Tool::Select: hs = {{nullptr, ICON_LC_MOUSE, "Click a guide"}, {"Ctrl", nullptr, "toggle"}, {"Shift", nullptr, "add"}, {nullptr, nullptr, "Drag: box"}}; break;
        case Tool::Move: name = "Move"; glyph = ICON_LC_MOVE; hs = {{nullptr, ICON_LC_MOUSE, "Drag the selection"}, {nullptr, nullptr, "Falloff in Edit settings"}}; break;
        case Tool::Comb: name = "Comb"; ci = w::CI_COMB; hs = {{nullptr, ICON_LC_MOUSE, "Drag to comb"}, {nullptr, nullptr, brush}}; break;
        case Tool::Curl: name = "Curl brush"; ci = w::CI_CURL; hs = {{nullptr, ICON_LC_MOUSE, "Brush over guides"}, {"C", nullptr, "curls the selection"}, {nullptr, nullptr, brush}}; break;
        case Tool::Paint: name = "Paint stiffness"; glyph = ICON_LC_PAINTBRUSH; hs = {{nullptr, ICON_LC_MOUSE, "Paint the scalp"}, {nullptr, nullptr, brush}}; break;
        default: break;
        }
        hs.push_back({"RMB", nullptr, "Orbit"});
        ImDrawList* d = ImGui::GetBackgroundDrawList();
        ImGui::PushFont(g_fontUiSemi);
        float nw = ImGui::CalcTextSize(name).x;
        ImGui::PopFont();
        float wdt = S(11) + S(16) + S(6) + nw;
        for (auto& h : hs) wdt += S(12) + (h.key ? w::kbdWidth(h.key) + S(5) : 0) + (h.glyph ? S(17) : 0) + ImGui::CalcTextSize(h.text).x;
        wdt += S(11);
        float hh = S(32);
        ImVec2 p(vx0 + S(16), pbarTop - S(16) - hh);
        if (p.y > vy0 + S(150)) {
            d->AddRectFilled(p, ImVec2(p.x + wdt, p.y + hh), IM_COL32(17, 20, 23, 204), S(8));
            d->AddRect(p, ImVec2(p.x + wdt, p.y + hh), tc(tok::IRON), S(8));
            float x = p.x + S(11), cy = p.y + hh * 0.5f;
            if (ci != w::CI_NONE) w::customIcon(d, ci, ImVec2(x + S(8), cy), S(15), tc(tok::GLOW));
            else w::icon(d, glyph, ImVec2(x + S(8), cy), S(14), tc(tok::GLOW));
            x += S(16) + S(6);
            d->AddText(g_fontUiSemi, g_fontUiSemi->FontSize, ImVec2(x, cy - g_fontUiSemi->FontSize * 0.5f), tc(tok::TEXT), name);
            x += nw;
            for (auto& h : hs) {
                x += S(12);
                if (h.key) { w::kbd(d, ImVec2(x, cy - S(8)), h.key); x += w::kbdWidth(h.key) + S(5); }
                if (h.glyph) { w::icon(d, h.glyph, ImVec2(x + S(6), cy), S(12), tc(tok::MUTED)); x += S(17); }
                d->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), tc(tok::MUTED), h.text);
                x += ImGui::CalcTextSize(h.text).x;
            }
        }
    }
}
