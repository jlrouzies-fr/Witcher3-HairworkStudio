// Desktop Groom Studio - UI ("Refined Ink & Iron": title bar, tool rail, inspector, status bar) and viewport input.
// Design: desktop_studio/design/ui_direction1 (IMPLEMENTATION.md). Our own code (MIT).
#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_internal.h"
#include "app.h"
#include "stylesw.h"
#include "lighting.h"
#include "ui_widgets.h"

namespace fs = std::filesystem;
using namespace DirectX;
using w::S;
using w::S2;

float g_ui = 1.0f, g_titleH = 44, g_railW = 60, g_panelW = 404, g_statusH = 30;   // scaled by the monitor DPI in applyTheme
ImFont* g_fontUi = nullptr, * g_fontUiSemi = nullptr, * g_fontSmall = nullptr, * g_fontSemi14 = nullptr, * g_fontCap = nullptr;
ImFont* g_fontBadge = nullptr, * g_fontKbd = nullptr, * g_fontBig = nullptr, * g_fontSection = nullptr, * g_fontGroup = nullptr;
ImFont* g_fontFor = nullptr, * g_fontName13 = nullptr, * g_fontName15 = nullptr, * g_fontName = nullptr, * g_fontDisplay = nullptr;
ImFont* g_fontHero = nullptr, * g_fontIcon = nullptr, * g_fontTitle = nullptr;

static const ImVec4 C_TEXT = tv(tok::TEXT), C_MUTED = tv(tok::MUTED), C_GLOW = tv(tok::GLOW), C_WARN = tv(tok::WARN);

// text: Latin-1 + the few typographic signs the UI uses (· ¾ × in Latin-1; – — ' ' " " • … → ● here)
static const ImWchar kTextRanges[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2018, 0x201D, 0x2022, 0x2022, 0x2026, 0x2026,
                                      0x2190, 0x2193, 0x25CF, 0x25CF, 0};
static const ImWchar kIconRanges[] = ICON_LC_RANGES;

static ImFont* addFont(const std::string& dir, const char* file, float px, float track, float iconPx) {
    std::string p = dir + "\\" + file;
    if (!fs::exists(p)) return nullptr;
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.GlyphExtraSpacing.x = track;
    ImFont* f = io.Fonts->AddFontFromFileTTF(p.c_str(), px, &cfg, kTextRanges);
    std::string ic = dir + "\\lucide.ttf";
    if (f && iconPx > 0 && fs::exists(ic)) {
        ImFontConfig m;
        m.MergeMode = true;
        m.OversampleH = 2;
        m.GlyphMinAdvanceX = iconPx;
        m.GlyphOffset.y = 0.12f * iconPx;
        io.Fonts->AddFontFromFileTTF(ic.c_str(), iconPx, &m, kIconRanges);
    }
    return f;
}

void applyTheme(const std::string& fontDir, float dpi) {
    ImGuiIO& io = ImGui::GetIO();
    g_ui = dpi;
    g_titleH = 44 * dpi;
    g_railW = 60 * dpi;
    g_panelW = 404 * dpi;
    g_statusH = 30 * dpi;
    // static SemiBold files (ImGui reads only the default instance of the variable Inter / Cinzel)
    const char* inter = "Inter.ttf", * interSemi = "Inter-SemiBold.ttf", * cinzelSemi = "Cinzel-SemiBold.ttf";
    g_fontUi = addFont(fontDir, inter, 13 * dpi, 0, 15 * dpi);
    g_fontUiSemi = addFont(fontDir, interSemi, 13 * dpi, 0, 15 * dpi);
    g_fontSmall = addFont(fontDir, inter, 11.5f * dpi, 0, 13 * dpi);
    g_fontSemi14 = addFont(fontDir, interSemi, 14 * dpi, 0, 0);
    g_fontCap = addFont(fontDir, interSemi, 10.5f * dpi, 1.3f * dpi, 0);
    g_fontBadge = addFont(fontDir, interSemi, 9.5f * dpi, 1.0f * dpi, 0);
    g_fontKbd = addFont(fontDir, inter, 10 * dpi, 0, 0);
    g_fontBig = addFont(fontDir, interSemi, 26 * dpi, -0.3f * dpi, 0);
    g_fontSection = addFont(fontDir, cinzelSemi, 12 * dpi, 1.9f * dpi, 0);
    g_fontGroup = addFont(fontDir, cinzelSemi, 11.5f * dpi, 2.3f * dpi, 0);
    g_fontFor = addFont(fontDir, cinzelSemi, 10.5f * dpi, 1.7f * dpi, 0);
    g_fontName13 = addFont(fontDir, cinzelSemi, 13 * dpi, 2.2f * dpi, 0);
    g_fontName15 = addFont(fontDir, cinzelSemi, 15 * dpi, 2.1f * dpi, 0);
    g_fontName = addFont(fontDir, cinzelSemi, 17 * dpi, 2.4f * dpi, 0);
    g_fontDisplay = addFont(fontDir, cinzelSemi, 22 * dpi, 4.4f * dpi, 0);
    g_fontHero = addFont(fontDir, cinzelSemi, 28 * dpi, 5.0f * dpi, 0);
    {
        std::string ic = fontDir + "\\lucide.ttf";
        if (fs::exists(ic)) {
            ImFontConfig c;
            c.OversampleH = 2;
            g_fontIcon = io.Fonts->AddFontFromFileTTF(ic.c_str(), 20 * dpi, &c, kIconRanges);
        }
    }
    ImFont* def = g_fontUi ? g_fontUi : io.Fonts->AddFontDefault();
    for (ImFont** f : {&g_fontUi, &g_fontUiSemi, &g_fontSmall, &g_fontSemi14, &g_fontCap, &g_fontBadge, &g_fontKbd, &g_fontBig,
                       &g_fontSection, &g_fontGroup, &g_fontFor, &g_fontName13, &g_fontName15, &g_fontName, &g_fontDisplay, &g_fontHero, &g_fontIcon})
        if (!*f) *f = def;
    g_fontTitle = g_fontSection;
    io.FontDefault = g_fontUi;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 8;
    s.FrameRounding = 5;
    s.PopupRounding = 8;
    s.GrabRounding = 7;
    s.TabRounding = 5;
    s.ScrollbarRounding = 4;
    s.ScrollbarSize = 8;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 1;
    s.PopupBorderSize = 1;
    s.ChildBorderSize = 1;
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(6, 4);
    s.IndentSpacing = 14;
    s.GrabMinSize = 13;
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(0, 3);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.HoverDelayShort = 0.25f;
    s.ScaleAllSizes(dpi);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = tv(tok::TEXT);
    c[ImGuiCol_TextDisabled] = tv(tok::DIM);
    c[ImGuiCol_WindowBg] = tv(tok::BG1);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = tv(0x0b0d0f, 0.94f);
    c[ImGuiCol_Border] = tv(tok::IRON);
    c[ImGuiCol_FrameBg] = tv(tok::BG0);
    c[ImGuiCol_FrameBgHovered] = tv(tok::BG3);
    c[ImGuiCol_FrameBgActive] = tv(tok::BG3);
    c[ImGuiCol_TitleBg] = tv(tok::BG0);
    c[ImGuiCol_TitleBgActive] = tv(tok::BG1);
    c[ImGuiCol_MenuBarBg] = tv(tok::BG0);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = tv(tok::IRON2);
    c[ImGuiCol_ScrollbarGrabHovered] = tv(tok::IRON3);
    c[ImGuiCol_ScrollbarGrabActive] = tv(tok::IRON3);
    c[ImGuiCol_CheckMark] = tv(tok::GLOW);
    c[ImGuiCol_SliderGrab] = tv(tok::KNOB);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_Button] = tv(tok::BG2);
    c[ImGuiCol_ButtonHovered] = tv(tok::BG3);
    c[ImGuiCol_ButtonActive] = tv(tok::AMBER, 0.13f);
    c[ImGuiCol_Header] = tv(tok::BG2);
    c[ImGuiCol_HeaderHovered] = tv(tok::BG3);
    c[ImGuiCol_HeaderActive] = tv(tok::AMBER, 0.13f);
    c[ImGuiCol_Separator] = tv(tok::IRON);
    c[ImGuiCol_SeparatorHovered] = tv(tok::AMBER);
    c[ImGuiCol_SeparatorActive] = tv(tok::GLOW);
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = tv(tok::AMBER, 0.5f);
    c[ImGuiCol_ResizeGripActive] = tv(tok::GLOW);
    c[ImGuiCol_Tab] = tv(tok::BG0);
    c[ImGuiCol_PlotHistogram] = tv(tok::AMBER);
    c[ImGuiCol_TextSelectedBg] = tv(tok::AMBER, 0.35f);
    c[ImGuiCol_NavHighlight] = tv(tok::GLOW);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

// ------------------------------------------------------------------------------------------------ small helpers
static void tip(const char* t) { w::tip(t); }

// Opens the HTML user guide in the default browser: docs\guide\guide.html next to the exe (release folder), else the
// repository copy (dev builds).
static void openGuide(App& a) {
    std::string g = exeDir() + "\\docs\\guide\\guide.html";
    if (!std::filesystem::exists(g)) g = a.set.repo + "\\desktop_studio\\docs\\guide\\guide.html";
    if (!std::filesystem::exists(g)) { a.status = "User guide not found: " + g; return; }
    // force the default WEB browser (the https handler), not whatever app owns .html (often an editor)
    std::wstring wg = std::filesystem::path(g).wstring(), url = L"file:///";
    for (wchar_t c : wg) url += c == (wchar_t)92 ? std::wstring(L"/") : c == L' ' ? std::wstring(L"%20") : std::wstring(1, c);
    wchar_t cmd[2048]; DWORD n = 2048;
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, L"https", L"open", cmd, &n))) {
        std::wstring c = cmd;
        size_t k = c.find(L"%1");
        if (k != std::wstring::npos) c.replace(k, 2, url); else c += L" \"" + url + L"\"";
        STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            return;
        }
    }
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);   // fallback
}

static void label(const char* l) {
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::PushTextWrapPos(0.0f);        // wrap at the panel edge (never truncated)
    ImGui::TextUnformatted(l);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void smallNote(const std::string& t, unsigned col = tok::MUTED) {
    ImGui::PushFont(g_fontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, tv(col));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// label on the left (MUTED), widget on the right: combos / text inputs in the inspector
static void rowLabel(const char* l, float widgetW) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::TextUnformatted(l, ImGui::FindRenderedTextEnd(l));
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - widgetW);
    ImGui::SetNextItemWidth(widgetW);
}

static bool comboRow(const char* l, int* v, const char* const* items, int n, const char* tipText) {
    float wdt = ImGui::GetContentRegionAvail().x - S(128);
    rowLabel(l, wdt);
    w::pushInputStyle();
    std::string id = std::string("##") + l;
    bool r = ImGui::Combo(id.c_str(), v, items, n);
    w::popInputStyle();
    tip(tipText);
    return r;
}

static bool comboFull(const char* id, int* v, const char* const* items, int n, const char* tipText) {
    ImGui::SetNextItemWidth(-1);
    w::pushInputStyle();
    bool r = ImGui::Combo(id, v, items, n);
    w::popInputStyle();
    tip(tipText);
    return r;
}

static std::string fileDialog(bool save, const char* filter, const char* defExt, const std::string& dir) {
    char buf[MAX_PATH] = "";
    OPENFILENAMEA o = {sizeof o};
    o.lpstrFilter = filter;
    o.lpstrFile = buf;
    o.nMaxFile = MAX_PATH;
    o.lpstrDefExt = defExt;
    o.lpstrInitialDir = dir.c_str();
    o.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if ((save ? GetSaveFileNameA(&o) : GetOpenFileNameA(&o))) return buf;
    return {};
}

// ------------------------------------------------------------------------------------------------ input

void App::handleInput() {
    ImGuiIO& io = ImGui::GetIO();
    // global keys (also on the start screen)
    if (ImGui::IsKeyPressed(ImGuiKey_F12)) pendingShot = set.repo + "\\build\\desktop_studio\\screenshot.png";
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) pickerOpen = true;
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) helpOpen = true;
    if (!loaded) return;
    if (windBusy) return;   // Check > Max-wind strays running: no edits / camera moves until it is done
    if (pickerOpen) {
        if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_Escape)) pickerOpen = false;
        return;
    }
    if (!io.WantCaptureKeyboard) {
        bool ctrl = io.KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) { groom.undo(); editsChanged(); }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) { groom.redoOp(); editsChanged(); }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O)) pickerOpen = true;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_G)) { guideBody = !guideBody; if (guideBody) showGuideOverlay = true; }   // hide guides behind the body
        if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_H)) {   // hairstyle switcher (opens under the breadcrumb)
            styleMenuReq = true;
            styleMenuX = crumbStyleX;
            styleMenuY = crumbStyleY;
            styleMenuW = S(360);
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
            std::string p = projectPath.empty() ? set.repo + "\\build\\desktop_studio\\projects\\" + configName + ".groomproj.json" : projectPath;
            saveProject(p);
        }
        if (!ctrl) {
            // tools
            if (ImGui::IsKeyPressed(ImGuiKey_Q)) tool = Tool::Select;
            if (ImGui::IsKeyPressed(ImGuiKey_W)) tool = Tool::Move;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) tool = Tool::Comb;
            if (ImGui::IsKeyPressed(ImGuiKey_T)) tool = Tool::Curl;
            if (ImGui::IsKeyPressed(ImGuiKey_P)) tool = Tool::Paint;
            if (ImGui::IsKeyPressed(ImGuiKey_A)) tool = Tool::Draw;
            // edits
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) opDelete();
            if (ImGui::IsKeyPressed(ImGuiKey_D)) opDuplicate();
            if (ImGui::IsKeyPressed(ImGuiKey_S)) opShorten();
            if (ImGui::IsKeyPressed(ImGuiKey_C)) opCurl(selList());
            if (ImGui::IsKeyPressed(ImGuiKey_X)) symmetry = !symmetry;
            // view
            if (ImGui::IsKeyPressed(ImGuiKey_G)) {
                if (io.KeyShift) { guideFront = !guideFront; if (guideFront) showGuideOverlay = true; }   // Shift+G: front layer only
                else showGuideOverlay = !showGuideOverlay;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_H)) showHair = !showHair;
            if (ImGui::IsKeyPressed(ImGuiKey_M)) showMeshes = !showMeshes;
            if (ImGui::IsKeyPressed(ImGuiKey_N)) showCollision = !showCollision;
            if (ImGui::IsKeyPressed(ImGuiKey_B)) showBones = !showBones;
            if (ImGui::IsKeyPressed(ImGuiKey_Z)) wireframe = !wireframe;
            if (ImGui::IsKeyPressed(ImGuiKey_V)) showSymPlane = !showSymPlane;
            // playback
            if (ImGui::IsKeyPressed(ImGuiKey_Space)) paused = !paused;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) teleport = true;
            if (ImGui::IsKeyPressed(ImGuiKey_Period)) { paused = true; stepOnce = true; }
            if (ImGui::IsKeyPressed(ImGuiKey_J)) windOn = !windOn;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { if (!sel.empty()) sel.clear(); else stopReset(); }
            if (ImGui::IsKeyPressed(ImGuiKey_Tab)) { if (editPose) resumeMotion(); else enterEditPose(); }
            // camera
            if (ImGui::IsKeyPressed(ImGuiKey_1)) cameraShot(4);
            if (ImGui::IsKeyPressed(ImGuiKey_2)) cameraShot(3);
            if (ImGui::IsKeyPressed(ImGuiKey_3)) cameraShot(2);
            if (ImGui::IsKeyPressed(ImGuiKey_4)) cameraShot(0);
            if (ImGui::IsKeyPressed(ImGuiKey_F)) frameHead();
            if (ImGui::IsKeyPressed(ImGuiKey_O)) turntable = !turntable;
            // lighting
            if (ImGui::IsKeyPressed(ImGuiKey_K)) rig.shadows = !rig.shadows;
            if (ImGui::IsKeyPressed(ImGuiKey_L)) applyTimeOfDay(rig, (rig.tod + 1) % timeOfDayCount());
            if (ImGui::IsKeyPressed(ImGuiKey_F6)) hairDebug = (hairDebug + 1) % 6;
            if (ImGui::IsKeyPressed(ImGuiKey_Keypad1)) { setView("front"); camShot = -1; }
            if (ImGui::IsKeyPressed(ImGuiKey_Keypad3)) { setView("right"); camShot = -1; }
            if (ImGui::IsKeyPressed(ImGuiKey_Keypad7)) { setView("back"); camShot = -1; }
            if (ImGui::IsKeyPressed(ImGuiKey_Keypad9)) { setView("left"); camShot = -1; }
        }
    }
    if (io.WantCaptureMouse) return;
    float mx = io.MousePos.x, my = io.MousePos.y;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) && io.KeyCtrl) {
        // Ctrl + right drag: rotate the sun live (azimuth with x, elevation with y)
        rig.sunAz += io.MouseDelta.x * 0.008f;
        if (rig.sunAz > 3.14159f) rig.sunAz -= 6.28318f;
        if (rig.sunAz < -3.14159f) rig.sunAz += 6.28318f;
        rig.sunEl = std::clamp(rig.sunEl - io.MouseDelta.y * 0.008f, 0.02f, 1.55f);
    } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        cam.yaw -= io.MouseDelta.x * 0.006f;
        if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) camShot = -1;
        cam.pitch = std::clamp(cam.pitch + io.MouseDelta.y * 0.006f, -1.4f, 1.4f);
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        V3 d = screenDeltaToWorld(-io.MouseDelta.x, -io.MouseDelta.y, {cam.target.x, cam.target.y, cam.target.z});
        cam.target.x += d.x; cam.target.y += d.y; cam.target.z += d.z;
    }
    if (io.MouseWheel != 0) cam.dist = std::clamp(cam.dist * std::pow(0.9f, io.MouseWheel), 0.15f, 30.0f);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        mx0 = mxl = mx;
        my0 = myl = my;
        dragging = true;
        boxSel = false;
        if (tool == Tool::Select) {
            int g = pickGuide(mx, my);
            if (g >= 0) {
                if (io.KeyCtrl) { if (sel.count(g)) sel.erase(g); else sel.insert(g); }
                else if (io.KeyShift) sel.insert(g);
                else { sel.clear(); sel.insert(g); }
            } else {
                if (!io.KeyCtrl && !io.KeyShift) sel.clear();
                boxSel = true;
            }
        } else if (tool == Tool::Move) {
            // a selection is locked while the button is held (user 2026-10-14): the drag moves it, never the hair under the
            // cursor; a plain click re-selects on release (below); with nothing selected the press picks the guide to move
            if (sel.empty()) { int g = pickGuide(mx, my); if (g >= 0) sel.insert(g); }
            if (sel.empty()) dragging = false;
            else {
                // one selected guide and the click on one of its points (2026-10-13): drag that point only
                std::string fo = falloff;
                int fk = fromK;
                dragK = -1;
                if (sel.size() == 1) {
                    int gi = *sel.begin();
                    float bd = 9.0f * 9.0f;
                    for (int k = 1; k < groom.K; k++) {
                        XMFLOAT2 s = project(posedPoint(gi, k));
                        float d = (s.x - mx) * (s.x - mx) + (s.y - my) * (s.y - my);
                        if (d < bd) { bd = d; dragK = k; }
                    }
                    if (dragK > 0) { fo = "point"; fk = dragK; }
                }
                strokeOps0 = groom.ops.size();
                int grp = 3000 + (++grpId);
                groom.ops.push_back({{"type", "translate"}, {"roots", groom.rootsOf(selList())}, {"offset", {0.0f, 0.0f, 0.0f}}, {"falloff", fo}, {"from", fk}, {"grp", grp}});
                auto mir = symmetry ? mirrorOf(selList()) : std::vector<int>();
                if (!mir.empty())
                    groom.ops.push_back({{"type", "translate"}, {"roots", groom.rootsOf(mir)}, {"offset", {0.0f, 0.0f, 0.0f}}, {"falloff", fo}, {"from", fk}, {"grp", grp}, {"mirrored", true}});
                groom.redo.clear();
                groom.replay();
            }
        } else if (tool == Tool::Comb) {
            strokeOps0 = groom.ops.size();
            combOff.clear();
            ++grpId;
        } else if (tool == Tool::Curl) {
            strokeSet.clear();
        } else if (tool == Tool::Paint) {
            paintAt(mx, my);
        } else if (tool == Tool::Draw) {
            drawing = false;
            drawPx.clear();
            drawWorld.clear();
            if (headRay(mx, my, drawRoot)) { drawing = true; drawUpdate(mx, my); }
            else { dragging = false; status = "Draw: click on the head"; }
        }
    }
    if (dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float dx = mx - mxl, dy = my - myl;
        if (tool == Tool::Move && (dx != 0 || dy != 0) && groom.ops.size() > strokeOps0) {
            auto ix = selList();
            V3 c{0, 0, 0};
            for (int i : ix) c = c + posedPoint(i, dragK > 0 ? dragK : groom.K / 2);
            c = c * (1.0f / std::max<size_t>(ix.size(), 1));
            V3 w = restDelta(ix.empty() ? 0 : ix[0], screenDeltaToWorld(mx - mx0, my - my0, c));   // posed -> rest space
            for (size_t k = strokeOps0; k < groom.ops.size(); k++) {
                bool m = groom.ops[k].value("mirrored", false);
                groom.ops[k]["offset"] = {r4(m ? -w.x : w.x), r4(w.y), r4(w.z)};
            }
            groom.replay();
            assetDirty = true;
        } else if (tool == Tool::Comb && (dx != 0 || dy != 0)) {
            for (int i : guidesUnderBrush(mx, my, brushPx)) {
                float best = 1e9f;
                for (int k = groom.K / 3; k < groom.K; k += 2) {
                    XMFLOAT2 s = project(posedPoint(i, k));
                    best = std::min(best, (s.x - mx) * (s.x - mx) + (s.y - my) * (s.y - my));
                }
                float wgt = combStrength * (1.0f - std::sqrt(best) / brushPx);
                V3 w = restDelta(i, screenDeltaToWorld(dx * wgt, dy * wgt, posedPoint(i, groom.K - 1)));
                combOff[i] = combOff[i] + w;
                if (symmetry) for (int j : mirrorOf({i})) combOff[j] = combOff[j] + V3{-w.x, w.y, w.z};
            }
            groom.ops.resize(strokeOps0);
            groom.replay();
            std::vector<json> add;
            for (auto& [i, o] : combOff)
                add.push_back({{"type", "translate"}, {"roots", groom.rootsOf({i})}, {"offset", {r4(o.x), r4(o.y), r4(o.z)}}, {"falloff", "linear"}, {"from", 0}, {"grp", grpId}});
            for (auto& a : add) groom.ops.push_back(a);
            groom.redo.clear();
            groom.replay();
            assetDirty = true;
        } else if (tool == Tool::Curl) {
            for (int i : guidesUnderBrush(mx, my, brushPx)) strokeSet.insert(i);
        } else if (tool == Tool::Paint && (dx != 0 || dy != 0)) {
            paintAt(mx, my);
        } else if (tool == Tool::Draw && (dx != 0 || dy != 0)) {
            drawUpdate(mx, my);
        }
        mxl = mx;
        myl = my;
    }
    if (dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (boxSel) {
            float x0 = std::min(mx0, mx), x1 = std::max(mx0, mx), y0 = std::min(my0, my), y1 = std::max(my0, my);
            if (x1 - x0 > 3 && y1 - y0 > 3)
                for (size_t i = 0; i < groom.P.size(); i++) {
                    if (!groom.alive[i]) continue;
                    if (visFilter()) {   // guide views + "Select visible only": a shown guide with a drawn point in the box
                        if (!pickVisible((int)i)) continue;
                        for (int k = 1; k < groom.K; k++) {
                            if (!pickPointVisible((int)i, k)) continue;
                            XMFLOAT2 s = project(posedPoint((int)i, k));
                            if (s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1) { sel.insert((int)i); break; }
                        }
                        continue;
                    }
                    XMFLOAT2 s = project(posedPoint((int)i, groom.K / 3));
                    if (s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1) sel.insert((int)i);
                }
        }
        if (tool == Tool::Move && groom.ops.size() > strokeOps0) {
            auto& o = groom.ops.back()["offset"];
            if (o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f) { groom.ops.resize(strokeOps0); groom.replay(); }
        }
        // a plain click (no drag) in Move still selects the hair under the cursor (user 2026-10-14): only a held drag is locked
        if (tool == Tool::Move && std::abs(mx - mx0) < 3 && std::abs(my - my0) < 3) {
            if (groom.ops.size() > strokeOps0) { groom.ops.resize(strokeOps0); groom.replay(); }
            int g = pickGuide(mx, my);
            if (g >= 0) {
                if (io.KeyCtrl) { if (sel.count(g)) sel.erase(g); else sel.insert(g); }
                else if (io.KeyShift) sel.insert(g);
                else { sel.clear(); sel.insert(g); }
            }
        }
        if (tool == Tool::Draw && drawing) opDrawGuide();
        if (tool == Tool::Curl && !strokeSet.empty()) opCurl(std::vector<int>(strokeSet.begin(), strokeSet.end()));
        dragging = false;
        boxSel = false;
        editsChanged();
    }
}

// splash + About logo: the app's own (HAIRWORK STUDIO, 4x hybrid), else the mod's 4x hybrid, else the 3x rebuild
static void loadAppLogo(App& a) {
    static bool tried = false;
    if (tried || a.splashSrv) return;
    tried = true;
    std::string p;
    for (const char* n : {"logo_studio.png", "logo_hybrid.png", "logo_rebuilt.png"}) {
        p = exeDir() + "\\assets\\" + n;
        if (!fs::exists(p)) p = a.set.repo + "\\desktop_studio\\assets\\" + n;
        if (fs::exists(p)) break;
    }
    if (fs::exists(p)) a.splashSrv = a.gfx.loadTexture(p, false, &a.splashW, &a.splashH);
}

void App::drawSplash() {
    if (!splashOn) return;
    ImGuiIO& io = ImGui::GetIO();
    double t = ImGui::GetTime();
    if (splashT0 < 0) {
        splashT0 = t;
        loadAppLogo(*this);
    }
    double e = t - splashT0;
    bool skip = e > 0.25 && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || io.InputQueueCharacters.Size > 0 ||
                             ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space));
    const double hold = 1.75, fade = 0.35;
    if (skip && e < hold) splashT0 = t - hold;                // jump to the fade-out
    e = t - splashT0;
    if (e > hold + fade || !splashSrv) { splashOn = false; return; }
    float a = e < fade ? float(e / fade) : (e > hold ? float(1.0 - (e - hold) / fade) : 1.0f);
    // the backdrop is opaque from the first frame (only the logo fades in), so the editor never flashes before the splash
    float bgA = e > hold ? a : 1.0f;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    d->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), ImGui::GetColorU32(ImVec4(0.071f, 0.078f, 0.086f, bgA)));
    float w = std::min(vp->Size.x * 0.62f, (float)splashW), h = w * splashH / std::max(splashW, 1);
    ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.46f);
    d->AddImage((ImTextureID)splashSrv, ImVec2(c.x - w / 2, c.y - h / 2), ImVec2(c.x + w / 2, c.y + h / 2), ImVec2(0, 0), ImVec2(1, 1),
                ImGui::GetColorU32(ImVec4(1, 1, 1, a)));
    // soften the picture's edges into the background (no visible rectangle)
    ImU32 bg = ImGui::GetColorU32(ImVec4(0.071f, 0.078f, 0.086f, a)), clear = ImGui::GetColorU32(ImVec4(0.071f, 0.078f, 0.086f, 0));
    float e0 = w * 0.12f;
    ImVec2 p0(c.x - w / 2 - 1, c.y - h / 2 - 1), p1(c.x + w / 2 + 1, c.y + h / 2 + 1);
    d->AddRectFilledMultiColor(p0, ImVec2(p0.x + e0, p1.y), bg, clear, clear, bg);
    d->AddRectFilledMultiColor(ImVec2(p1.x - e0, p0.y), p1, clear, bg, bg, clear);
    d->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + e0), bg, bg, clear, clear);
    d->AddRectFilledMultiColor(ImVec2(p0.x, p1.y - e0), p1, clear, clear, bg, bg);
    std::string ver = std::string("Hairwork Studio ") + GSD_VERSION + "   |   NVIDIA HairWorks runtime" + (sdkOk ? "" : " not loaded");
    ImGui::PushFont(g_fontSmall);
    ImVec2 ts = ImGui::CalcTextSize(ver.c_str());
    d->AddText(ImVec2(c.x - ts.x / 2, c.y + h / 2 + 12 * g_ui), ImGui::GetColorU32(ImVec4(0.55f, 0.57f, 0.60f, a)), ver.c_str());
    // NVIDIA attribution on the opening screen (GameWorks EULA 6.B.c): the embedded logo, else the text
    float ly = c.y + h / 2 + 12 * g_ui + ts.y + 14 * g_ui;
    if (gwLogoSrv) {
        float lw = (float)gwW * g_ui, lh = (float)gwH * g_ui;
        d->AddImage((ImTextureID)gwLogoSrv, ImVec2(c.x - lw / 2, ly), ImVec2(c.x + lw / 2, ly + lh), ImVec2(0, 0), ImVec2(1, 1),
                    ImGui::GetColorU32(ImVec4(1, 1, 1, a)));
    } else {
        ImVec2 gs = ImGui::CalcTextSize(CREDIT);
        d->AddText(ImVec2(c.x - gs.x / 2, ly), ImGui::GetColorU32(ImVec4(0.55f, 0.57f, 0.60f, a)), CREDIT);
    }
    ImGui::PopFont();
}

// ------------------------------------------------------------------------------------------------ UI state (this file)
static bool s_logOpen = false, s_settingsReq = false;
static int s_curveTab = 0;
static ID3D11ShaderResourceView* s_swoosh = nullptr;
static bool s_swooshTried = false;

static std::string shortPreset(const std::string& n, std::string* src = nullptr) {
    size_t p = n.find(" (");
    if (p == std::string::npos) { if (src) *src = ""; return n; }
    if (src) { *src = n.substr(p + 2); if (!src->empty() && src->back() == ')') src->pop_back(); }
    return n.substr(0, p);
}

static int curlOps(const Groom& g) {
    int n = 0;
    for (auto& o : g.ops) if (o.value("type", std::string()) == "curl" && !o.value("mirrored", false)) n++;
    return n;
}

static ImU32 colourOf(const json& c, ImU32 def) {
    if (!c.is_object()) return def;
    return IM_COL32(c.value("Red", 128), c.value("Green", 128), c.value("Blue", 128), 255);
}

// ------------------------------------------------------------------------------------------------ title bar
static bool menuButton(const char* name, ImVec2& popupPos) {
    ImVec2 ts = ImGui::CalcTextSize(name);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz(ts.x + S(20), S(28));
    bool r = ImGui::InvisibleButton(name, sz);
    bool hov = ImGui::IsItemHovered() || ImGui::IsPopupOpen((std::string("##menu_") + name).c_str());
    ImDrawList* d = ImGui::GetWindowDrawList();
    if (hov) d->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), tc(tok::BG3), S(5));
    d->AddText(ImVec2(p.x + S(10), p.y + (sz.y - ts.y) * 0.5f), hov ? tc(tok::TEXT) : tc(tok::SOFT), name);
    popupPos = ImVec2(p.x, p.y + sz.y + S(6));
    ImGui::SameLine(0, S(2));
    return r;
}

static void drawTitleBar(App& a) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, g_titleH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##titlebar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 o = vp->Pos;
    float W = vp->Size.x, H = g_titleH, cy = o.y + H * 0.5f;
    w::gradientRect(d, o, ImVec2(o.x + W, o.y + H), tc(0x111417), tc(tok::BG0));
    d->AddLine(ImVec2(o.x, o.y + H - 0.5f), ImVec2(o.x + W, o.y + H - 0.5f), tc(tok::IRON), 1);
    // brand: the swoosh + HAIRWORK STUDIO (the app's name; the mod is Hairwork for All)
    if (!s_swooshTried) {
        s_swooshTried = true;
        std::string p = exeDir() + "\\assets\\brand_swoosh.png";
        if (!fs::exists(p)) p = a.set.repo + "\\desktop_studio\\assets\\brand_swoosh.png";
        if (fs::exists(p)) s_swoosh = a.gfx.loadTexture(p, false);
    }
    float x = o.x + S(16);
    if (s_swoosh) d->AddImage((ImTextureID)s_swoosh, ImVec2(x, cy - S(5.5f)), ImVec2(x + S(46), cy + S(5.5f)), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 242));
    x += S(46) + S(10);
    ImVec2 t1 = w::textSize(g_fontName13, "HAIRWORK");
    d->AddText(g_fontName13, g_fontName13->FontSize, ImVec2(x, cy - t1.y * 0.5f), tc(tok::SILVER), "HAIRWORK");
    x += t1.x + S(7);
    // second word in the amber accent, in caps at the same size and baseline as HAIRWORK
    ImVec2 t2 = w::textSize(g_fontName13, "STUDIO");
    d->AddText(g_fontName13, g_fontName13->FontSize, ImVec2(x, cy - t1.y * 0.5f), tc(tok::GLOW), "STUDIO");
    x += t2.x + S(24);
    // menus (buttons opening popups: BeginMenu would open sideways outside a menu bar)
    ImGui::SetCursorScreenPos(ImVec2(x, cy - S(14)));
    ImVec2 pp;
    const char* menus[] = {"File", "Edit", "View", "Check", "Help"};
    for (const char* m : menus) {
        if (menuButton(m, pp)) ImGui::OpenPopup((std::string("##menu_") + m).c_str());
        ImGui::SetNextWindowPos(pp);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(8, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, S2(10, 8));
        bool open = ImGui::BeginPopup((std::string("##menu_") + m).c_str());
        if (!open) { ImGui::PopStyleVar(2); continue; }
        if (!strcmp(m, "File")) {
            if (ImGui::MenuItem("Characters...", "Ctrl+O")) a.pickerOpen = true;
            if (ImGui::MenuItem("Open project...", "")) {
                auto p = fileDialog(false, "Groom project\0*.groomproj.json\0All\0*.*\0", "json", a.set.repo + "\\build\\desktop_studio\\projects");
                if (!p.empty()) a.loadProject(p);
            }
            if (ImGui::MenuItem("Save project", "Ctrl+S", false, a.loaded)) {
                std::string p = a.projectPath.empty() ? a.set.repo + "\\build\\desktop_studio\\projects\\" + a.configName + ".groomproj.json" : a.projectPath;
                a.saveProject(p);
            }
            if (ImGui::MenuItem("Save project as...", nullptr, false, a.loaded)) {
                fs::create_directories(a.set.repo + "\\build\\desktop_studio\\projects");
                auto p = fileDialog(true, "Groom project\0*.groomproj.json\0", "groomproj.json", a.set.repo + "\\build\\desktop_studio\\projects");
                if (!p.empty()) a.saveProject(p);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Re-read the build", nullptr, false, a.loaded)) a.openConfig(a.configName, true);
            if (ImGui::MenuItem("Refresh character list", nullptr, false, !a.job.running)) a.refreshCatalog();
            if (ImGui::MenuItem("Screenshot", "F12")) a.pendingShot = a.set.repo + "\\build\\desktop_studio\\screenshot.png";
            ImGui::Separator();
            if (ImGui::MenuItem("Settings...")) s_settingsReq = true;
        } else if (!strcmp(m, "Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, a.loaded)) { a.groom.undo(); a.editsChanged(); }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, a.loaded && !a.groom.redo.empty())) { a.groom.redoOp(); a.editsChanged(); }
            ImGui::Separator();
            if (ImGui::MenuItem("Select all", nullptr, false, a.loaded)) {
                a.sel.clear();
                for (size_t i = 0; i < a.groom.alive.size(); i++) if (a.groom.alive[i]) a.sel.insert((int)i);
            }
            if (ImGui::MenuItem("Mirror side", nullptr, false, a.loaded)) a.opMirror();
            if (a.beardOn) {   // Beard mode (builds with a beard fur): same switch as the inspector's "Editing  Hair | Beard"
                ImGui::Separator();
                if (ImGui::MenuItem("Edit the beard fur", nullptr, a.fur == 1, a.loaded && !a.windBusy)) a.setFur(a.fur == 1 ? 0 : 1);
            }
        } else if (!strcmp(m, "View")) {
            const char* vn[] = {"front", "back", "left", "right", "34", "back34"};
            const char* vl[] = {"Front (Num 1)", "Back (Num 7)", "Left side (Num 9)", "Right side (Num 3)", "Three-quarter front", "Three-quarter back"};
            for (int i = 0; i < 6; i++) if (ImGui::MenuItem(vl[i])) { a.setView(vn[i]); a.camShot = -1; }
            ImGui::Separator();
            ImGui::MenuItem("Hair (HairWorks)", "H", &a.showHair);
            ImGui::MenuItem("Head and outfit", "M", &a.showMeshes);
            ImGui::MenuItem("Scalp highlight (green)", nullptr, &a.scalpOverlay);
            ImGui::MenuItem("Vanilla reference...", nullptr, &a.vanillaOpen);
            if (ImGui::MenuItem("Vanilla hair (cards)", nullptr, &a.vanillaHair)) a.showHair = !a.vanillaHair;
            ImGui::MenuItem("Guide overlay", "G", &a.showGuideOverlay);
            // guide visibility views (guidevis.cpp): live with the camera; both can be on
            if (ImGui::MenuItem("Guides: front layer only", "Shift+G", &a.guideFront) && a.guideFront) a.showGuideOverlay = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only the guides on the outside of the hair as seen from the camera (the first layer):\nguides covered by other hair or by the head / body are hidden.");
            if (ImGui::MenuItem("Guides: hide behind the body", "Ctrl+G", &a.guideBody) && a.guideBody) a.showGuideOverlay = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Guide parts hidden by the head, body or outfit from the camera are not drawn.");
            if (ImGui::BeginMenu("Guide view options", a.guideVisOn())) {
                ImGui::MenuItem("Select visible only", nullptr, &a.selectVisibleOnly);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click, box and brush selection (and the comb / curl / paint brushes) only take the guides the view shows.");
                ImGui::SetNextItemWidth(S(240));
                ImGui::SliderFloat("front points %", &a.guideFrontPct, 10, 100, "%.0f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Front layer: share of a guide's points that must be front-most for it to show.");
                ImGui::SetNextItemWidth(S(240));
                ImGui::SliderFloat("depth tolerance mm", &a.guideTolMm, 1, 40, "%.0f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Front layer: how far behind the outermost hair a point may lie and still count as front.");
                if (a.gvis.valid) ImGui::TextDisabled("%d of %d guides shown (%.1f ms in the background, %.1f ms a frame)", a.gvis.nShown, a.groom.numAlive(), a.gvis.ms, a.gvis.msMain);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Guide colour")) {   // white hair (Ciri) hides the default grey guides; saved in the project
                if (ImGui::ColorPicker3("##guidecol", a.guideRgb, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayRGB)) {
                    a.guideCustom = true;
                    a.showGuideOverlay = true;
                    a.rememberGuideColour();
                }
                const float presets[][3] = {{1.0f, 0.2f, 0.6f}, {0.1f, 0.9f, 0.3f}, {1.0f, 0.55f, 0.0f}, {0.1f, 0.1f, 0.1f}};
                const char* names[] = {"Magenta", "Green", "Orange", "Black"};
                for (int p = 0; p < 4; p++) {
                    if (p) ImGui::SameLine();
                    if (ImGui::SmallButton(names[p])) { for (int i = 0; i < 3; i++) a.guideRgb[i] = presets[p][i]; a.guideCustom = true; a.showGuideOverlay = true; a.rememberGuideColour(); }
                }
                if (ImGui::SmallButton("Default")) { a.guideCustom = false; a.guideRgb[0] = 0.6f; a.guideRgb[1] = 0.65f; a.guideRgb[2] = 0.7f; a.rememberGuideColour(); }
                ImGui::EndMenu();
            }
            ImGui::MenuItem("Simulated guides (SDK)", nullptr, &a.sdkGuides);
            ImGui::MenuItem("Collision capsules (SDK)", "N", &a.showCollision);
            ImGui::MenuItem("Growth mesh (SDK)", nullptr, &a.showGrowth);
            ImGui::Separator();
            ImGui::MenuItem("Turntable", "O", &a.turntable);
            ImGui::MenuItem("Sky backdrop", nullptr, &a.rig.backdrop);
            ImGui::MenuItem("Community...", nullptr, &a.communityOpen);
            ImGui::MenuItem("Light settings...", nullptr, &a.lightOpen);
            ImGui::MenuItem("Log", nullptr, &s_logOpen);
        } else if (!strcmp(m, "Check")) {
            if (ImGui::MenuItem("Unnatural endings...", nullptr, a.endingsOpen, a.loaded)) a.endingsOpen = !a.endingsOpen;
            if (ImGui::MenuItem("Unwanted loops...", nullptr, a.loopsOpen, a.loaded)) a.loopsOpen = !a.loopsOpen;
            if (ImGui::MenuItem("Max-wind strays...", nullptr, a.windOpen, a.loaded)) a.windOpen = !a.windOpen;
        } else {
            if (ImGui::MenuItem("Shortcuts", "F1")) a.helpOpen = true;
            if (ImGui::MenuItem("User guide")) openGuide(a);
            if (ImGui::MenuItem("About")) a.aboutOpen = true;
        }
        ImGui::EndPopup();
        ImGui::PopStyleVar(2);
    }
    // breadcrumb (centred): portrait, name, build, badge, unsaved edits; click = the character picker
    {
        const json* ce = a.loaded ? a.catalogEntry(a.configName) : nullptr;
        bool showChar = a.loaded && !a.pickerOpen;
        std::string name = ce ? w::upper(ce->value("name", "")) : (showChar ? w::upper(a.configName.substr(0, a.configName.find('_'))) : "CHARACTERS");
        std::string st = ce ? ce->value("status", "") : "";
        int edits = a.loaded ? a.editCount() : 0;
        bool dirty = a.loaded && edits != a.savedEdits;
        std::string build = a.configName, dirtyT = std::to_string(edits) + (edits == 1 ? " edit" : " edits") + (dirty ? " unsaved" : "");
        // hairstyle (Character > Hairstyle > build): always shown in the editor; a dropdown ("##crumb_style", Ctrl+Shift+H)
        // listing the character's hairstyles, "Default" (disabled) for a character with one
        int nStyles = ce && ce->contains("hairstyles") && (*ce)["hairstyles"].is_array() ? (int)(*ce)["hairstyles"].size() : 0;
        std::string style = showChar ? a.styleName(a.configName) : std::string();
        if (showChar && style.empty()) style = "Default";
        float pw = 0;
        ImVec2 nsz = w::textSize(g_fontName13, name.c_str());
        if (showChar) {
            pw = S(6) + S(26) + S(10) + nsz.x + S(10) + S(14) + S(10) + ImGui::CalcTextSize(build.c_str()).x;
            if (!style.empty()) pw += ImGui::CalcTextSize(style.c_str()).x + S(4) + S(12) + S(10) + S(14) + S(10);
            if (!st.empty()) pw += S(10) + w::badgeWidth(st);
            if (edits > 0) pw += S(10) + S(9) + S(14) + w::textSize(g_fontSmall, dirtyT.c_str()).x;
            pw += S(12);
        } else {
            std::string sub = a.loaded ? ("Back to " + a.configName) : "No character loaded";
            pw = S(12) + S(16) + S(10) + nsz.x + S(10) + S(10) + w::textSize(g_fontSmall, sub.c_str()).x + S(12);
        }
        float ph = S(34);
        ImVec2 p0(o.x + (W - pw) * 0.5f, cy - ph * 0.5f);
        ImGui::SetCursorScreenPos(p0);
        ImGui::SetNextItemAllowOverlap();   // the hairstyle crumb sits on top of it
        bool clicked = ImGui::InvisibleButton("##crumb", ImVec2(pw, ph));
        bool hov = ImGui::IsItemHovered();
        tip(a.pickerOpen ? (a.loaded ? "Back to the editor (Esc)" : "Pick a character to start") : "Characters (Ctrl+O)\nSwitch character or build.");
        // hairstyle crumb: its own button over the breadcrumb (x = after the portrait, the name and the first chevron)
        bool styleHov = false;
        if (showChar && !style.empty()) {
            float sx = p0.x + S(6) + S(26) + S(10) + nsz.x + S(10) + S(14) + S(10);
            float sw = ImGui::CalcTextSize(style.c_str()).x + S(4) + S(12);
            ImVec2 s0(sx - S(6), p0.y + S(4)), s1(sx + sw + S(6), p0.y + ph - S(4));
            ImGui::SetCursorScreenPos(s0);
            bool sc = ImGui::InvisibleButton("##crumb_style", ImVec2(s1.x - s0.x, s1.y - s0.y));
            styleHov = ImGui::IsItemHovered() && nStyles > 1;
            tip(nStyles > 1 ? "Hairstyle (Ctrl+Shift+H)\nSwitch hairstyle: a style with builds opens its best / approved build; one not started shows what can be done in the inspector."
                            : "Hairstyle\nThis character has one hairstyle (Default).");
            a.crumbStyleX = s0.x;
            a.crumbStyleY = p0.y + ph + S(6);
            if (sc && nStyles > 1) { a.styleMenuReq = true; a.styleMenuX = s0.x; a.styleMenuY = p0.y + ph + S(6); a.styleMenuW = S(360); }
            if (sc) clicked = false;
            if (styleHov) hov = false;
        }
        if (clicked) a.pickerOpen = a.loaded ? !a.pickerOpen : true;
        d->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + ph), tc(hov ? tok::BG2 : tok::BG1), S(8));
        d->AddRect(p0, ImVec2(p0.x + pw, p0.y + ph), tc(hov ? tok::IRON2 : tok::IRON), S(8));
        bool styleOpen = a.styleMenuOpen;   // last frame (the popup is drawn by drawStyleMenu)
        float bx = p0.x;
        if (showChar) {
            bx += S(6);
            ImVec2 c(bx + S(13), cy);
            ID3D11ShaderResourceView* srv = ce ? a.portrait(*ce) : nullptr;
            if (srv) d->AddImageRounded((ImTextureID)srv, ImVec2(c.x - S(13), c.y - S(13)), ImVec2(c.x + S(13), c.y + S(13)), ImVec2(0.12f, 0.06f), ImVec2(0.88f, 0.62f), IM_COL32_WHITE, S(13));
            else d->AddCircleFilled(c, S(13), tc(tok::BG3), 24);
            d->AddCircle(c, S(13), tc(tok::IRON2), 24, 1);
            bx += S(26) + S(10);
            d->AddText(g_fontName13, g_fontName13->FontSize, ImVec2(bx, cy - nsz.y * 0.5f), tc(tok::TEXT), name.c_str());
            bx += nsz.x + S(10);
            w::icon(d, ICON_LC_CHEVRON_RIGHT, ImVec2(bx + S(7), cy), S(13), tc(tok::DIM));
            bx += S(14) + S(10);
            if (!style.empty()) {
                ImVec2 ss = ImGui::CalcTextSize(style.c_str());
                bool multi = nStyles > 1;
                if (styleHov || (multi && styleOpen)) {
                    ImVec2 s0(bx - S(6), p0.y + S(4)), s1(bx + ss.x + S(4) + S(12) + S(6), p0.y + ph - S(4));
                    d->AddRectFilled(s0, s1, tc(tok::BG3), S(6));
                    d->AddRect(s0, s1, styleOpen ? tc(tok::IRON3) : tc(tok::IRON2), S(6));
                }
                d->AddText(ImVec2(bx, cy - ss.y * 0.5f), multi ? tc(tok::TEXT) : tc(tok::MUTED), style.c_str());
                bx += ss.x + S(4);
                w::icon(d, multi && styleOpen ? ICON_LC_CHEVRON_UP : ICON_LC_CHEVRON_DOWN, ImVec2(bx + S(6), cy), S(12), multi ? (styleHov ? tc(tok::TEXT) : tc(tok::SOFT)) : tc(tok::DIM));
                bx += S(12) + S(10);
                w::icon(d, ICON_LC_CHEVRON_RIGHT, ImVec2(bx + S(7), cy), S(13), tc(tok::DIM));
                bx += S(14) + S(10);
            }
            ImVec2 bs = ImGui::CalcTextSize(build.c_str());
            d->AddText(ImVec2(bx, cy - bs.y * 0.5f), tc(tok::SOFT), build.c_str());
            bx += bs.x;
            if (!st.empty()) { bx += S(10); bx += w::badge(d, ImVec2(bx, cy - S(9)), st); }
            if (edits > 0) {
                bx += S(10);
                d->AddLine(ImVec2(bx, cy - S(8)), ImVec2(bx, cy + S(8)), tc(tok::IRON), 1);
                bx += S(9);
                ImU32 dc = dirty ? tc(tok::GLOW) : tc(tok::DIM);
                if (dirty) d->AddCircleFilled(ImVec2(bx + S(3.5f), cy), S(6), tc(tok::GLOW, 0.18f), 16);
                d->AddCircleFilled(ImVec2(bx + S(3.5f), cy), S(3.5f), dc, 12);
                bx += S(14);
                ImVec2 ds = w::textSize(g_fontSmall, dirtyT.c_str());
                d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(bx, cy - ds.y * 0.5f), tc(tok::MUTED), dirtyT.c_str());
            }
        } else {
            bx += S(12);
            w::icon(d, ICON_LC_USERS, ImVec2(bx + S(8), cy), S(15), tc(tok::GLOW));
            bx += S(16) + S(10);
            d->AddText(g_fontName13, g_fontName13->FontSize, ImVec2(bx, cy - nsz.y * 0.5f), tc(tok::TEXT), name.c_str());
            bx += nsz.x + S(10);
            d->AddLine(ImVec2(bx, cy - S(8)), ImVec2(bx, cy + S(8)), tc(tok::IRON), 1);
            bx += S(10);
            std::string sub = a.loaded ? ("Back to " + a.configName) : "No character loaded";
            ImVec2 ss = w::textSize(g_fontSmall, sub.c_str());
            d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(bx, cy - ss.y * 0.5f), tc(tok::DIM), sub.c_str());
        }
    }
    // right: Characters (Ctrl O), Save, primary Export mod
    {
        bool canExport = a.loaded && !a.bud.overLimit;
        float ew = w::textSize(g_fontUiSemi, "Export mod").x + S(22) + S(24);
        float rx = o.x + W - S(14) - ew;
        ImGui::SetCursorScreenPos(ImVec2(rx, cy - S(15)));
        if (w::primaryButton("Export mod##title", "Export mod", ImVec2(ew, S(30)), canExport, ICON_LC_PACKAGE)) { a.exportReq = true; a.pickerOpen = false; }
        tip(!a.loaded ? "Export mod\nOpen a character first." : a.bud.overLimit ? "Export mod\nBlocked: over the 120 % performance budget." : "Export mod\nOpens the Export section (mod folder, tag, export).");
        {   // Community, same size class as Export mod
            float cw = w::textSize(g_fontUiSemi, "Community").x + S(22) + S(24);
            rx -= S(8) + cw;
            ImGui::SetCursorScreenPos(ImVec2(rx, cy - S(15)));
            w::g_primaryAlt = 0x6f9fd8;   // steel blue, distinct from the amber Export mod
            if (w::primaryButton("Community##title", "Community", ImVec2(cw, S(30)), true, ICON_LC_USERS)) a.communityOpen = !a.communityOpen;
            w::g_primaryAlt = 0;
            tip("Community\nBrowse, install, sync and publish shared hair packs.");
        }
        {   // user guide, always visible
            float gw = w::textSize(g_fontUiSemi, "Open User Guide").x + S(22) + S(24);
            rx -= S(8) + gw;
            ImGui::SetCursorScreenPos(ImVec2(rx, cy - S(15)));
            w::g_primaryAlt = 0x8fbf8a;   // sage green, distinct from Community (steel) and Export mod (amber)
            bool og = w::primaryButton("Guide##title", "Open User Guide", ImVec2(gw, S(30)), true, ICON_LC_INFO);
            w::g_primaryAlt = 0;
            if (og) openGuide(a);
            tip("User guide\nOpens the illustrated guide in your web browser (also Help > User guide).");
        }
        if (a.pickerOpen) {
            float ow = ImGui::CalcTextSize("Open project...").x + S(23) + S(24);
            rx -= S(8) + ow;
            ImGui::SetCursorScreenPos(ImVec2(rx, cy - S(15)));
            if (w::button("Open project...##title", "Open project...", ImVec2(ow, S(30)), ICON_LC_FOLDER, true)) {
                auto p = fileDialog(false, "Groom project\0*.groomproj.json\0All\0*.*\0", "json", a.set.repo + "\\build\\desktop_studio\\projects");
                if (!p.empty()) a.loadProject(p);
            }
            tip("Open a saved project (*.groomproj.json).");
        } else {
            rx -= S(8) + S(30);
            ImGui::SetCursorScreenPos(ImVec2(rx, cy - S(15)));
            if (w::iconButton("##save", ICON_LC_SAVE, 30, "Save project (Ctrl+S)", a.loaded)) {
                std::string p = a.projectPath.empty() ? a.set.repo + "\\build\\desktop_studio\\projects\\" + a.configName + ".groomproj.json" : a.projectPath;
                a.saveProject(p);
            }
            float cw = ImGui::CalcTextSize("Characters").x + S(23) + w::kbdWidth("Ctrl O") + S(8) + S(24);
            rx -= S(8) + cw;
            ImGui::SetCursorScreenPos(ImVec2(rx, cy - S(15)));
            if (w::button("Characters##chars", "Characters", ImVec2(cw, S(30)), ICON_LC_USERS, true, true, "Ctrl O")) a.pickerOpen = !a.pickerOpen;
            tip("Characters (Ctrl+O)\nThe character picker: approved / final / pick / in-progress characters, their builds, recent projects and exports.");
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

// ------------------------------------------------------------------------------------------------ tool rail
static bool railButton(const char* id, const char* glyph, w::Custom ci, bool active, bool toggled, const char* key, const char* tipText, bool enabled) {
    float sz = S(42);
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.x = ImGui::GetWindowPos().x + (g_railW - sz) * 0.5f;
    ImGui::SetCursorScreenPos(p);
    if (!enabled) ImGui::BeginDisabled();
    bool r = ImGui::InvisibleButton(id, ImVec2(sz, sz));
    bool hov = ImGui::IsItemHovered() && enabled;
    w::tipAt(tipText, ImVec2(ImGui::GetWindowPos().x + g_railW + S(12), p.y), true);
    if (!enabled) ImGui::EndDisabled();
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 b(p.x + sz, p.y + sz), c(p.x + sz * 0.5f, p.y + sz * 0.5f);
    if (active) {
        d->AddRectFilled(p, b, AMBER_WASH, S(8));
        d->AddRect(p, b, tc(tok::GLOW, 0.42f), S(8));
        float bx = ImGui::GetWindowPos().x;
        d->AddRectFilled(ImVec2(bx, p.y + S(8)), ImVec2(bx + S(7), b.y - S(8)), tc(tok::GLOW, 0.2f), S(3));
        d->AddRectFilled(ImVec2(bx, p.y + S(9)), ImVec2(bx + S(3), b.y - S(9)), tc(tok::GLOW), S(2), ImDrawFlags_RoundCornersRight);
    } else if (hov) d->AddRectFilled(p, b, tc(tok::BG3), S(8));
    ImU32 col = (active || toggled) ? tc(tok::GLOW) : hov ? tc(tok::TEXT) : tc(tok::MUTED, enabled ? 1.0f : 0.45f);
    if (ci != w::CI_NONE) w::customIcon(d, ci, c, S(20), col);
    else w::icon(d, glyph, c, S(20), col);
    if (key && *key) d->AddText(g_fontKbd, S(8.5f), ImVec2(b.x - S(3) - g_fontKbd->CalcTextSizeA(S(8.5f), FLT_MAX, 0, key).x, b.y - S(12)), tc(tok::DIM), key);
    if (toggled) d->AddCircleFilled(ImVec2(b.x - S(7.5f), p.y + S(7.5f)), S(2.5f), tc(tok::GLOW), 10);
    return r && enabled;
}

static void railSep() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x = ImGui::GetWindowPos().x + (g_railW - S(28)) * 0.5f;
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p.y + S(4)), ImVec2(x + S(28), p.y + S(4)), tc(tok::IRON), 1);
}

static void drawRail(App& a) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float H = vp->Size.y - g_titleH - g_statusH;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + g_titleH));
    ImGui::SetNextWindowSize(ImVec2(g_railW, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, S(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("rail", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    d->AddLine(ImVec2(wp.x + g_railW - 0.5f, wp.y), ImVec2(wp.x + g_railW - 0.5f, wp.y + H), tc(tok::IRON), 1);
    bool on = a.loaded && !a.pickerOpen;
    struct T { Tool t; const char* glyph; w::Custom ci; const char* id; const char* key; const char* tipText; };
    T tools[] = {{Tool::Select, ICON_LC_MOUSE_POINTER_2, w::CI_NONE, "##sel", "Q", "Select (Q)\nClick a guide; Ctrl+click toggles, Shift+click adds; drag on empty space for a box."},
                 {Tool::Move, ICON_LC_MOVE, w::CI_NONE, "##mov", "W", "Move (W)\nDrag the selected guides in the view plane. Falloff and 'from CV' are in Edit guides."},
                 {Tool::Comb, nullptr, w::CI_COMB, "##comb", "E", "Comb (E)\nDrag over guides: they follow the stroke, roots stay fixed. Brush size in Edit guides."},
                 {Tool::Curl, nullptr, w::CI_CURL, "##curl", "T", "Curl brush (T)\nBrush over guides to curl them with the Curl settings (ringlet, roll, wave). C curls the selection."},
                 {Tool::Paint, ICON_LC_PAINTBRUSH, w::CI_NONE, "##paint", "P", "Paint stiffness (P)\nPaint a stiffness / root-stiffness value on the scalp (exported as the redfur stiffnessTex)."},
                 {Tool::Draw, ICON_LC_SPLINE, w::CI_NONE, "##draw", "A", "Draw (A)\nClick on the scalp where the hair grows, drag to trace its path; release to add the guide."}};
    for (auto& t : tools) if (railButton(t.id, t.glyph, t.ci, on && a.tool == t.t, false, t.key, t.tipText, on)) a.tool = t.t;
    // editing tools edit the bind pose (unless "edit while moving"); Select keeps the motion running
    if (a.loaded) {
        if (a.tool != Tool::Select && !a.editPose && !a.editWhileMoving) a.enterEditPose();
    }
    railSep();
    if (railButton("##sym", ICON_LC_FLIP_HORIZONTAL_2, w::CI_NONE, false, a.symmetry, "X",
                   "Symmetry (X)\nEdits, combing, curls and paint on one side are mirrored onto the partner guides across the head's mid-plane.", on))
        a.symmetry = !a.symmetry;
    railSep();
    if (railButton("##undo", ICON_LC_UNDO_2, w::CI_NONE, false, false, "", "Undo (Ctrl+Z)", on && !a.groom.ops.empty())) { a.groom.undo(); a.editsChanged(); }
    if (railButton("##redo", ICON_LC_REDO_2, w::CI_NONE, false, false, "", "Redo (Ctrl+Y)", on && !a.groom.redo.empty())) { a.groom.redoOp(); a.editsChanged(); }
    railSep();
    if (railButton("##dup", ICON_LC_COPY, w::CI_NONE, false, false, "D", "Duplicate selected guides (D)\nCopies are placed one 'dup step' to the screen right, on the scalp.", on)) a.opDuplicate();
    if (railButton("##short", ICON_LC_SCISSORS, w::CI_NONE, false, false, "S", "Shorten selected guides (S)\nShortens them to the 'keep %' of their length.", on)) a.opShorten();
    if (railButton("##del", ICON_LC_TRASH_2, w::CI_NONE, false, false, "", "Delete selected guides (Del)\nThe growth mesh is re-triangulated around them.", on)) a.opDelete();
    // bottom: shortcuts, settings
    ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + H - S(10) - S(42) * 2 - S(4)));
    if (railButton("##shortcuts", ICON_LC_KEYBOARD, w::CI_NONE, false, false, "", "Shortcuts (F1)\nEvery key and mouse action.", true)) a.helpOpen = true;
    if (railButton("##settings", ICON_LC_SETTINGS, w::CI_NONE, false, false, "", "Settings\nHairWorks SDK / DLL path and the pipeline's Python.", true)) s_settingsReq = true;
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ------------------------------------------------------------------------------------------------ inspector
static void budgetCard(App& a) {
    const Budget& b = a.bud;
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth();
    ImVec2 p(wp.x + S(14), ImGui::GetCursorScreenPos().y + S(10));
    float cw = W - S(28), ch = S(152);
    ImVec2 q(p.x + cw, p.y + ch);
    w::gradientRect(d, p, q, tc(0x171b1f), tc(0x14171a), S(8));
    d->AddRect(p, q, tc(tok::IRON), S(8));
    float x = p.x + S(14), xr = q.x - S(14), y = p.y + S(11);
    // row 1: BUDGET + state
    d->AddText(g_fontCap, g_fontCap->FontSize, ImVec2(x, y + S(3)), tc(tok::MUTED), "BUDGET");
    {
        const char* st = b.overLimit ? "Over 120 %: export blocked" : "Within budget \xC2\xB7 export OK";
        ImU32 c = b.overLimit ? tc(tok::WARN) : tc(tok::OK);
        ImVec2 ss = ImGui::CalcTextSize(st);
        d->AddText(ImVec2(xr - ss.x, y), c, st);
        w::icon(d, b.overLimit ? ICON_LC_TRIANGLE_ALERT : ICON_LC_CIRCLE_CHECK, ImVec2(xr - ss.x - S(11), y + ss.y * 0.5f), S(13), c);
    }
    // row 2: 100 % + hairs
    y += S(20);
    char pct[16];
    snprintf(pct, sizeof pct, "%.0f", b.ratio * 100);
    ImVec2 ps = w::textSize(g_fontBig, pct);
    d->AddText(g_fontBig, g_fontBig->FontSize, ImVec2(x, y), b.overLimit ? tc(tok::WARN) : tc(tok::TEXT), pct);
    d->AddText(g_fontSemi14, g_fontSemi14->FontSize, ImVec2(x + ps.x + S(2), y + ps.y - w::textSize(g_fontSemi14, "%").y - S(2)), tc(tok::MUTED), "%");
    std::string of = fmtCount(b.hairsClose) + " close-up hairs / " + fmtCount(b.refHairsClose) + " reference";
    ImVec2 os = ImGui::CalcTextSize(of.c_str());
    d->AddText(ImVec2(x + ps.x + S(22), y + ps.y - os.y - S(3)), tc(tok::MUTED), of.c_str());
    // meter: 0..130 %, reference tick at 100 %, limit at 120 %
    y += ps.y + S(12);
    float mx0 = x, mx1 = xr, mh = S(10), full = 1.3f;
    auto X = [&](float r) { return mx0 + (mx1 - mx0) * std::min(r / full, 1.0f); };
    ImVec2 m0(mx0, y), m1(mx1, y + mh);
    d->AddRectFilled(m0, m1, tc(tok::BG0), S(5));
    float lim = a.look.limit;
    // 100..120 % hatched, >120 % warn
    d->PushClipRect(ImVec2(X(1.0f), y), ImVec2(X(lim), y + mh), true);
    for (float hx = X(1.0f) - mh; hx < X(lim) + mh; hx += S(8)) d->AddLine(ImVec2(hx, y + mh), ImVec2(hx + mh, y), tc(tok::WARN, 0.16f), S(4));
    d->PopClipRect();
    d->AddRectFilled(ImVec2(X(lim), y), m1, tc(tok::WARN, 0.22f), S(5), ImDrawFlags_RoundCornersRight);
    float fx = X((float)b.ratio);
    if (fx > mx0 + 1) {
        for (int i = 3; i >= 1; i--) d->AddRectFilled(ImVec2(mx0 - i * S(2), y - i * S(2)), ImVec2(fx + i * S(2), y + mh + i * S(2)), tc(tok::GLOW, i == 1 ? 0.12f : i == 2 ? 0.06f : 0.03f), S(5) + i * S(2));
        int v0 = d->VtxBuffer.Size;
        d->AddRectFilled(m0, ImVec2(fx, y + mh), tc(0x7a4512), S(5), fx > X(full) - 2 ? 0 : ImDrawFlags_RoundCornersLeft);
        // three-stop gradient #7a4512 -> AMBER (70 %) -> GLOW
        int v1 = d->VtxBuffer.Size;
        float mid = mx0 + (fx - mx0) * 0.7f;
        for (int i = v0; i < v1; i++) {
            ImDrawVert& v = d->VtxBuffer[i];
            ImVec4 c0 = tv(0x7a4512), c1 = tv(tok::AMBER), c2 = tv(tok::GLOW);
            ImVec4 c;
            if (v.pos.x <= mid) { float t = (v.pos.x - mx0) / std::max(mid - mx0, 1.0f); c = ImVec4(c0.x + (c1.x - c0.x) * t, c0.y + (c1.y - c0.y) * t, c0.z + (c1.z - c0.z) * t, 1); }
            else { float t = (v.pos.x - mid) / std::max(fx - mid, 1.0f); c = ImVec4(c1.x + (c2.x - c1.x) * t, c1.y + (c2.y - c1.y) * t, c1.z + (c2.z - c1.z) * t, 1); }
            v.col = ImGui::ColorConvertFloat4ToU32(c);
        }
    }
    d->AddRect(m0, m1, tc(tok::IRON), S(5));
    float t100 = X(1.0f), t120 = X(lim);
    d->AddRectFilled(ImVec2(t100 - S(1), y - S(4)), ImVec2(t100 + S(1), y + mh + S(4)), tc(tok::SILVER), S(1));
    d->AddRectFilled(ImVec2(t120 - S(1), y - S(4)), ImVec2(t120 + S(1), y + mh + S(4)), tc(tok::WARN), S(1));
    ImGui::PushFont(g_fontKbd);
    char lt[32];
    snprintf(lt, sizeof lt, "limit %.0f %%", lim * 100);
    ImVec2 rs = ImGui::CalcTextSize("ref 100 %");
    d->AddText(ImVec2(t100 - S(6) - rs.x, y + mh + S(5)), tc(tok::MUTED), "ref 100 %");
    d->AddText(ImVec2(std::max(t120 - S(34), t100 + S(6)), y + mh + S(5)), tc(tok::WARN), lt);
    ImGui::PopFont();
    // row 3: hairs / live / LOD
    y += mh + S(24);
    float colW = (xr - x) / 3;
    const char* lab[3] = {"Hairs", "Live (SDK)", "LOD"};
    std::string val[3] = {fmtCount(b.hairs), fmtCount(a.st.hairs), ""};
    char lod[16];
    snprintf(lod, sizeof lod, "%.2f", a.st.detailLod);
    val[2] = lod;
    const char* sub[3] = {"", "on screen", "forced on"};
    for (int i = 0; i < 3; i++) {
        float cx = x + colW * i;
        d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(cx, y), tc(tok::MUTED), lab[i]);
        ImVec2 vs = w::textSize(g_fontSemi14, val[i].c_str());
        d->AddText(g_fontSemi14, g_fontSemi14->FontSize, ImVec2(cx, y + S(15)), tc(tok::TEXT), val[i].c_str());
        if (*sub[i]) d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(cx + vs.x + S(4), y + S(15) + vs.y - w::textSize(g_fontSmall, sub[i]).y - S(1)), tc(tok::MUTED), sub[i]);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##budget", ImVec2(cw, ch));
    {
        std::string t = "Performance budget\nClose-up hairs " + fmtCount(b.hairsClose) + " vs the reference " + fmtCount(b.refHairsClose) + " (" + b.refName + ", " +
                        a.pack.budgetRef.value("kind", std::string("")) + ").\nHairs HairWorks generates for the current camera: " + fmtCount(a.st.hairs) +
                        ".\nLimits are enforced: density, close-up density and width are capped, LOD stays on, export is blocked above 120 %." +
                        (b.overLimit ? "\n" + b.msg : "");
        tip(t.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(wp.x, q.y + S(10)));
    // path-tracing GPU cost: warnings (never block export) + the estimate
    {
        PtResult pr;
        bool have = a.ptWorker.get(pr);
        if (have) a.ptMs = a.ptCalib.ms(pr);
        ImGui::SetCursorPosX(S(14));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(W - S(14));
        char pb[160];
        // 2026-10-13: estimate hidden (fit r^2 0.28 vs in-game PresentMon, see pt_calibration.json); set kShowPtEstimate to show it again
        constexpr bool kShowPtEstimate = false;
        if (!kShowPtEstimate) {
        } else if (have) {
            double ms = std::max(0.0, a.ptMs);
            snprintf(pb, sizeof pb, "Path tracing est.: ~%.0f ms/frame (Vesemir ~%.0f)%s", ms, a.ptCalib.vesemir, a.ptWorker.busy() ? " ..." : "");
            smallNote(pb, ms >= 20 ? tok::WARN : ms >= 16 ? tok::AMBER : tok::OK);
        } else smallNote("Path tracing est.: computing ...");
        ImGui::PopTextWrapPos();
        if (kShowPtEstimate) ImGui::Checkbox("PT load sim##ptsim", &a.ptLoadSim);
        if (kShowPtEstimate && ImGui::IsItemHovered()) tip("Adds an artificial GPU load to the viewport (repeats the hair pass) proportional to the estimate. Off by default.");
        ImGui::PushTextWrapPos(W - S(14));
        for (auto& wmsg : b.warns) smallNote("Warning: " + wmsg, tok::WARN);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, S(6)));
    }
}

static void colourRow(App& a, const ParamDef& d) {
    json c = a.look.get(d.path);
    float rgb[3] = {c.is_object() ? c.value("Red", 0) / 255.0f : 1, c.is_object() ? c.value("Green", 0) / 255.0f : 1, c.is_object() ? c.value("Blue", 0) / 255.0f : 1};
    bool modified = a.look.over.count(d.path) > 0;
    float W = ImGui::GetContentRegionAvail().x, h = S(28);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(d.label, ImVec2(W, h));
    bool hov = ImGui::IsItemHovered();
    json b = a.look.base(d.path);
    std::string t = std::string(d.path) + "\nbuild value: " + (b.is_null() ? "(absent: HairWorks default)" : b.dump());
    tip(t.c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string disp = d.label;
    if (!disp.empty()) disp[0] = (char)toupper((unsigned char)disp[0]);
    ImVec2 ls = ImGui::CalcTextSize(disp.c_str());
    dl->AddText(ImVec2(p.x, p.y + (h - ls.y) * 0.5f), hov ? tc(tok::TEXT) : tc(tok::MUTED), disp.c_str());
    if (modified) dl->AddCircleFilled(ImVec2(p.x + ls.x + S(9), p.y + h * 0.5f), S(3), tc(tok::GLOW), 10);
    ImVec2 s0(p.x + W - S(52), p.y + (h - S(22)) * 0.5f), s1(s0.x + S(22), s0.y + S(22));
    dl->AddRectFilled(s0, s1, ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], 1)), S(5));
    dl->AddRect(s0, s1, hov ? tc(tok::IRON3) : tc(tok::IRON2), S(5));
    char hex[16];
    snprintf(hex, sizeof hex, "#%02X%02X%02X", int(rgb[0] * 255 + .5f), int(rgb[1] * 255 + .5f), int(rgb[2] * 255 + .5f));
    ImVec2 hs = w::textSize(g_fontSmall, hex);
    dl->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(s0.x - S(8) - hs.x, p.y + (h - hs.y) * 0.5f), tc(tok::SOFT), hex);
    ImGui::PushID(d.path);
    if (clicked) ImGui::OpenPopup("##cp");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && modified) ImGui::OpenPopup("##cpctx");
    if (ImGui::BeginPopup("##cp")) {
        if (ImGui::ColorPicker3(d.label, rgb, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayRGB))
            a.look.setColor(d.path, int(std::round(rgb[0] * 255)), int(std::round(rgb[1] * 255)), int(std::round(rgb[2] * 255)));
        if (modified && ImGui::Button("Reset to build value")) { a.look.reset(d.path); a.computeBudget(); }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##cpctx")) {
        if (ImGui::MenuItem("Reset to build value")) { a.look.reset(d.path); a.computeBudget(); }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

static void paramSlider(App& a, const ParamDef& d, bool capped) {
    float v = a.look.f(d.path, paramDefault(d.path));
    bool modified = a.look.over.count(d.path) > 0;
    json b = a.look.base(d.path);
    float ref = b.is_number() ? b.get<float>() : paramDefault(d.path);
    std::string disp = d.label;
    if (!disp.empty()) disp[0] = (char)toupper((unsigned char)disp[0]);
    std::string t = capped ? std::string(d.path) + "\nbuild value: " + (b.is_null() ? "(absent: HairWorks default)" : b.dump()) : std::string(d.path);
    bool reset = false;
    float hi = capped ? std::min(d.hi, a.look.cap(d.path)) : d.hi;
    if (w::sliderRow(d.label, disp.c_str(), &v, d.lo, hi, "%.3f", ref, modified, &reset, t.c_str())) { a.look.setF(d.path, v); a.computeBudget(); }
    if (reset) { a.look.reset(d.path); a.computeBudget(); }
}

static void curveEditor(App& a) {
    static const char* paths[4] = {"physicalMaterials.stiffness.stiffnessCurve", "physicalMaterials.stiffness.bendStiffnessCurve",
                                   "physicalMaterials.stiffness.stiffnessStrengthCurve", "physicalMaterials.stiffness.stiffnessDampingCurve"};
    static const char* tabs[4] = {"Stiffness##cv", "Bend##cv", "Strength##cv", "Damping##cv"};
    static const char* tabTips[4] = {"Stiffness curve (root -> tip)", "Bend curve (root -> tip)", "Strength curve", "Stiffness damping curve"};
    ImDrawList* d = ImGui::GetWindowDrawList();
    // sub header: CURVE · ROOT -> TIP  + tabs on the right
    ImGui::Dummy(ImVec2(0, S(4)));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;
    d->AddText(g_fontCap, g_fontCap->FontSize, ImVec2(p.x, p.y + S(5)), tc(tok::DIM), "CURVE \xC2\xB7 ROOT \xE2\x86\x92 TIP");
    float tx = p.x + W;
    for (int i = 3; i >= 0; i--) tx -= ImGui::CalcTextSize(tabs[i], ImGui::FindRenderedTextEnd(tabs[i])).x + S(14) + S(4);
    for (int i = 0; i < 4; i++) {
        const char* e = ImGui::FindRenderedTextEnd(tabs[i]);
        ImVec2 ts = ImGui::CalcTextSize(tabs[i], e);
        ImGui::SetCursorScreenPos(ImVec2(tx, p.y));
        if (ImGui::InvisibleButton(tabs[i], ImVec2(ts.x + S(14), S(22)))) s_curveTab = i;
        tip(tabTips[i]);
        bool on = s_curveTab == i, hov = ImGui::IsItemHovered();
        if (on) d->AddRectFilled(ImVec2(tx, p.y), ImVec2(tx + ts.x + S(14), p.y + S(22)), tc(tok::BG3), S(4));
        d->AddText(ImVec2(tx + S(7), p.y + (S(22) - ts.y) * 0.5f), on || hov ? tc(tok::TEXT) : tc(tok::MUTED), tabs[i], e);
        if (a.look.over.count(paths[i])) d->AddCircleFilled(ImVec2(tx + ts.x + S(12), p.y + S(5)), S(2.5f), tc(tok::GLOW), 8);
        tx += ts.x + S(14) + S(4);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(28)));
    // canvas
    const char* path = paths[s_curveTab];
    json c = a.look.get(path);
    float v[4] = {c.is_object() ? c.value("X", 1.0f) : 1, c.is_object() ? c.value("Y", 1.0f) : 1, c.is_object() ? c.value("Z", 1.0f) : 1, c.is_object() ? c.value("W", 1.0f) : 1};
    ImVec2 c0 = ImGui::GetCursorScreenPos(), c1(c0.x + W, c0.y + S(70));
    d->AddRectFilled(c0, c1, tc(tok::BG0), S(7));
    float ix0 = c0.x + S(14), ix1 = c1.x - S(14), iy0 = c0.y + S(10), iy1 = c1.y - S(14);
    for (int k = 1; k < 3; k++) { float gx = ix0 + (ix1 - ix0) * k / 3; d->AddLine(ImVec2(gx, c0.y + 1), ImVec2(gx, c1.y - 1), tc(0x1f2428), 1); }
    d->AddLine(ImVec2(c0.x + 1, (iy0 + iy1) * 0.5f), ImVec2(c1.x - 1, (iy0 + iy1) * 0.5f), tc(0x1f2428), 1);
    auto P = [&](int k) { return ImVec2(ix0 + (ix1 - ix0) * k / 3.0f, iy1 - (iy1 - iy0) * v[k]); };
    // smooth curve (Catmull-Rom -> cubic Beziers), area fill below it
    ImVec2 pts[4] = {P(0), P(1), P(2), P(3)};
    auto bez = [&](int i, float t, ImVec2 cp1, ImVec2 cp2) {
        ImVec2 a0 = pts[i], a3 = pts[i + 1];
        float u = 1 - t;
        return ImVec2(u * u * u * a0.x + 3 * u * u * t * cp1.x + 3 * u * t * t * cp2.x + t * t * t * a3.x,
                      u * u * u * a0.y + 3 * u * u * t * cp1.y + 3 * u * t * t * cp2.y + t * t * t * a3.y);
    };
    std::vector<ImVec2> curve;
    for (int i = 0; i < 3; i++) {
        ImVec2 pm = pts[std::max(i - 1, 0)], p0 = pts[i], p1 = pts[i + 1], p2 = pts[std::min(i + 2, 3)];
        ImVec2 cp1(p0.x + (p1.x - pm.x) / 6, p0.y + (p1.y - pm.y) / 6), cp2(p1.x - (p2.x - p0.x) / 6, p1.y - (p2.y - p0.y) / 6);
        for (int s = 0; s <= 16; s++) { if (i && !s) continue; curve.push_back(bez(i, s / 16.0f, cp1, cp2)); }
    }
    for (size_t i = 0; i + 1 < curve.size(); i++)
        d->AddQuadFilled(curve[i], curve[i + 1], ImVec2(curve[i + 1].x, c1.y - 1), ImVec2(curve[i].x, c1.y - 1), tc(tok::AMBER, 0.12f));
    d->AddPolyline(curve.data(), (int)curve.size(), tc(tok::GLOW), 0, S(2));
    d->AddRect(c0, c1, tc(tok::IRON), S(7));
    // knobs: vertical drag only
    bool changed = false;
    ImGui::PushID(path);
    for (int k = 0; k < 4; k++) {
        ImVec2 q = P(k);
        ImGui::SetCursorScreenPos(ImVec2(q.x - S(7), q.y - S(7)));
        char id[16];
        snprintf(id, sizeof id, "##knob%d", k);
        ImGui::InvisibleButton(id, ImVec2(S(14), S(14)));
        bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
        if (act) {
            float nv = std::clamp((iy1 - ImGui::GetIO().MousePos.y) / (iy1 - iy0), 0.0f, 1.0f);
            nv = std::round(nv * 100) / 100;
            if (nv != v[k]) { v[k] = nv; changed = true; }
        }
        tip("Multiplier along the strand: root, 1/3, 2/3, tip (HairWorks curve). Drag up / down.");
        q = P(k);
        if (hov || act) d->AddCircleFilled(q, S(9), tc(tok::GLOW, 0.25f), 16);
        d->AddCircleFilled(q, S(5), k == 3 ? tc(tok::GLOW) : tc(tok::BG0), 16);
        d->AddCircle(q, S(5), tc(tok::GLOW), 16, S(1.5f));
        char vt[16];
        snprintf(vt, sizeof vt, k == 0 ? "root %.2f" : k == 3 ? "tip %.2f" : "%.2f", v[k]);
        ImVec2 vs = w::textSize(g_fontKbd, vt);
        float lx = k == 0 ? q.x - S(4) : k == 3 ? q.x - vs.x + S(2) : q.x - vs.x * 0.5f;
        float ly = q.y + S(8) + vs.y > c1.y - S(2) ? q.y - S(8) - vs.y : q.y + S(8);
        d->AddText(g_fontKbd, g_fontKbd->FontSize, ImVec2(lx, ly), k == 3 ? tc(tok::GLOW) : tc(tok::MUTED), vt);
    }
    if (changed) a.look.setJson(path, {{"X", v[0]}, {"Y", v[1]}, {"Z", v[2]}, {"W", v[3]}});
    ImGui::SetCursorScreenPos(c0);
    ImGui::Dummy(ImVec2(W, S(70)));
    if (ImGui::IsMouseHoveringRect(c0, c1) && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && a.look.over.count(path)) ImGui::OpenPopup("##curvectx");
    if (ImGui::BeginPopup("##curvectx")) {
        if (ImGui::MenuItem("Reset to build value")) a.look.reset(path);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

static void materialTiles(App& a) {
    float W = ImGui::GetContentRegionAvail().x, gap = S(6), tw = (W - gap) * 0.5f, th = S(40);
    ImDrawList* d = ImGui::GetWindowDrawList();
    int n = (int)a.presets.size(), shown = n <= 6 ? std::min(n, 5) : 5;
    ImVec2 o = ImGui::GetCursorScreenPos();
    json curRoot = a.look.get("graphicalMaterials.color.rootColor"), curTip = a.look.get("graphicalMaterials.color.tipColor");
    auto tile = [&](int k, int i) {
        ImVec2 p(o.x + (k % 2) * (tw + gap), o.y + (k / 2) * (th + gap));
        ImGui::SetCursorScreenPos(p);
        const MaterialPreset& pr = a.presets[i];
        bool clicked = ImGui::InvisibleButton(pr.name.c_str(), ImVec2(tw, th));
        bool hov = ImGui::IsItemHovered(), on = a.presetSel == i;
        std::string t = pr.kind == "current" ? pr.name + "\nThe material of the loaded build." :
                        pr.name + "\nDerived from the pipeline config keys: " + std::to_string(pr.configKeys.size()) + " look keys.";
        tip(t.c_str());
        if (clicked) a.applyMaterial(i);
        ImVec2 q(p.x + tw, p.y + th);
        d->AddRectFilled(p, q, on ? AMBER_WASH : hov ? tc(tok::BG3) : tc(tok::BG2), S(7));
        d->AddRect(p, q, on ? tc(tok::GLOW, 0.6f) : hov ? tc(tok::IRON3) : tc(tok::IRON), S(7));
        // swatch: root -> tip colour of the preset (else the character's)
        ImU32 rc = colourOf(pr.values.is_object() && pr.values.contains("graphicalMaterials.color.rootColor") && !a.keepColours
                                ? pr.values["graphicalMaterials.color.rootColor"] : curRoot, IM_COL32(120, 90, 50, 255));
        ImU32 tcol = colourOf(pr.values.is_object() && pr.values.contains("graphicalMaterials.color.tipColor") && !a.keepColours
                                  ? pr.values["graphicalMaterials.color.tipColor"] : curTip, IM_COL32(200, 150, 80, 255));
        ImVec2 s0(p.x + S(7), p.y + S(5)), s1(s0.x + S(26), s0.y + S(30));
        w::gradientRect(d, s0, s1, rc, tcol, S(4));
        d->PushClipRect(s0, s1, true);
        // strand lines: hue varies per preset (width / clump look)
        float step = S(4) + (i % 3) * S(0.6f);
        for (float x = s0.x - S(30); x < s1.x; x += step) d->AddLine(ImVec2(x, s1.y), ImVec2(x + S(8), s0.y), IM_COL32(255, 255, 255, 46), 1);
        d->PopClipRect();
        d->AddRect(s0, s1, IM_COL32(255, 255, 255, 16), S(4));
        std::string src, nm = shortPreset(pr.name, &src);
        if (src.empty()) src = pr.kind;
        float tx = s1.x + S(9), maxX = q.x - S(6);
        d->PushClipRect(ImVec2(tx, p.y), ImVec2(maxX, q.y), true);
        d->AddText(ImVec2(tx, p.y + S(5)), on ? IM_COL32_WHITE : tc(tok::TEXT), nm.c_str());
        d->AddText(g_fontSmall, S(10.5f), ImVec2(tx, p.y + S(21)), tc(tok::DIM), src.c_str());
        d->PopClipRect();
    };
    for (int i = 0; i < shown; i++) tile(i, i);
    // last tile: N more · Save preset (dashed)
    {
        int k = shown;
        ImVec2 p(o.x + (k % 2) * (tw + gap), o.y + (k / 2) * (th + gap));
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("Save preset##tile", ImVec2(tw, th))) ImGui::OpenPopup("##morepresets");
        bool hov = ImGui::IsItemHovered(), on = a.presetSel >= shown;
        tip("More presets and Save preset\nAll material presets, and saves the current material (all shading / shape / dynamics values, no colours) to desktop_studio\\presets\\<name>.json.");
        ImVec2 q(p.x + tw, p.y + th);
        if (hov) d->AddRectFilled(p, q, tc(tok::BG3), S(7));
        // dashed border
        ImU32 bc = on ? tc(tok::GLOW, 0.6f) : hov ? tc(tok::IRON3) : tc(tok::IRON2);
        for (float x = p.x + S(6); x < q.x - S(6); x += S(8)) { d->AddLine(ImVec2(x, p.y + 0.5f), ImVec2(std::min(x + S(4), q.x - S(6)), p.y + 0.5f), bc); d->AddLine(ImVec2(x, q.y - 0.5f), ImVec2(std::min(x + S(4), q.x - S(6)), q.y - 0.5f), bc); }
        for (float y = p.y + S(6); y < q.y - S(6); y += S(8)) { d->AddLine(ImVec2(p.x + 0.5f, y), ImVec2(p.x + 0.5f, std::min(y + S(4), q.y - S(6))), bc); d->AddLine(ImVec2(q.x - 0.5f, y), ImVec2(q.x - 0.5f, std::min(y + S(4), q.y - S(6))), bc); }
        d->PathArcTo(ImVec2(p.x + S(6), p.y + S(6)), S(6), 3.1416f, 4.712f, 6); d->PathStroke(bc, 0, 1);
        d->PathArcTo(ImVec2(q.x - S(6), p.y + S(6)), S(6), 4.712f, 6.283f, 6); d->PathStroke(bc, 0, 1);
        d->PathArcTo(ImVec2(q.x - S(6), q.y - S(6)), S(6), 0, 1.5708f, 6); d->PathStroke(bc, 0, 1);
        d->PathArcTo(ImVec2(p.x + S(6), q.y - S(6)), S(6), 1.5708f, 3.1416f, 6); d->PathStroke(bc, 0, 1);
        std::string t = n > shown ? std::to_string(n - shown) + " more \xC2\xB7 Save preset" : "Save preset";
        if (on) t = shortPreset(a.presets[a.presetSel].name) + " \xC2\xB7 more";
        ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        float cx = p.x + (tw - ts.x - S(20)) * 0.5f;
        w::icon(d, ICON_LC_PLUS, ImVec2(cx + S(6), p.y + th * 0.5f), S(13), tc(tok::MUTED));
        d->PushClipRect(p, q, true);
        d->AddText(ImVec2(cx + S(20), p.y + (th - ts.y) * 0.5f), on ? tc(tok::TEXT) : tc(tok::MUTED), t.c_str());
        d->PopClipRect();
    }
    int rows = (shown + 2) / 2;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(W, rows * th + (rows - 1) * gap));
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(320), 0), ImVec2(S(420), S(520)));
    if (ImGui::BeginPopup("##morepresets")) {
        ImGui::PushFont(g_fontCap);
        ImGui::TextColored(tv(tok::DIM), "MATERIAL PRESETS");
        ImGui::PopFont();
        for (int i = 0; i < n; i++) {
            if (ImGui::Selectable((a.presets[i].name + "##list").c_str(), a.presetSel == i)) a.applyMaterial(i);
            if (!a.presets[i].configKeys.empty() || a.presets[i].kind == "current") {
                std::string t = a.presets[i].kind == "current" ? "The material of the loaded build." : ("Derived from the pipeline config keys: " + std::to_string(a.presets[i].configKeys.size()) + " look keys.");
                tip(t.c_str());
            }
        }
        ImGui::Separator();
        float bw = ImGui::CalcTextSize("Save preset").x + S(24);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - S(8));
        w::pushInputStyle();
        ImGui::InputTextWithHint("##pn", "preset name", a.presetName, sizeof a.presetName);
        w::popInputStyle();
        ImGui::SameLine(0, S(8));
        if (w::button("Save preset", "Save preset", ImVec2(bw, ImGui::GetFrameHeight()))) {
            std::string err;
            if (saveCustomPreset(a.look, a.presetName, a.set.repo + "\\desktop_studio\\presets", err)) {
                a.status = std::string("Preset saved: ") + a.presetName;
                json raw;
                try { std::ifstream f(a.pack.dir + "\\desk.json"); raw = json::parse(f); } catch (...) {}
                a.presets = loadPresets(a.pack, a.set.repo + "\\desktop_studio\\presets", raw.value("materials", json::object()));
            } else a.status = err;
        }
        tip("Saves the current material (all shading / shape / dynamics values, no colours) to desktop_studio\\presets\\<name>.json.");
        ImGui::EndPopup();
    }
}

static void whoHeader(App& a) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth();
    // top row: the "Hairstyle" dropdown (+ the not-started panel under it), stylesw.cpp
    float top = S(12) + styleCardRow(a, ImVec2(wp.x + S(16), wp.y + S(12)), W - S(32));
    float h = top + S(12) + S(76);
    const json* ce = a.catalogEntry(a.configName);
    ImVec2 p(wp.x + S(16), wp.y + top + S(12));
    ID3D11ShaderResourceView* srv = ce ? a.portrait(*ce) : nullptr;
    ImVec2 i1(p.x + S(48), p.y + S(62));
    if (srv) d->AddImageRounded((ImTextureID)srv, p, i1, ImVec2(0.08f, 0.04f), ImVec2(0.92f, 0.78f), IM_COL32_WHITE, S(5));
    else { d->AddRectFilled(p, i1, tc(tok::BG3), S(5)); w::icon(d, ICON_LC_USER, ImVec2(p.x + S(24), p.y + S(31)), S(26), tc(0x3a4047)); }
    d->AddRect(p, i1, tc(tok::IRON2), S(5));
    float x = i1.x + S(12), y = p.y - S(1);
    std::string name = ce ? w::upper(ce->value("name", "")) : w::upper(a.configName.substr(0, a.configName.find('_')));
    ImVec2 ns = w::textSize(g_fontName, name.c_str());
    d->AddText(g_fontName, g_fontName->FontSize, ImVec2(x, y), tc(tok::TEXT), name.c_str());
    if (ce && !ce->value("status", std::string()).empty()) w::badge(d, ImVec2(x + ns.x + S(10), y + (ns.y - S(18)) * 0.5f), ce->value("status", ""));
    y += ns.y + S(4);
    d->PushClipRect(ImVec2(x, wp.y), ImVec2(wp.x + W - S(40), wp.y + h), true);
    d->AddText(ImVec2(x, y), tc(tok::SOFT), a.configName.c_str());
    y += ImGui::GetFontSize() + S(4);
    std::string sub;
    if (ce) {
        if (ce->value("approved", json()).is_string() && (*ce)["approved"] == a.configName) sub = "From the approved build";
        else if (ce->value("pick", json()).is_string() && (*ce)["pick"] == a.configName) sub = "The user's pick";
        else if (ce->value("best", json()).is_string() && (*ce)["best"] == a.configName) sub = "Round-loop best";
        else sub = "Variant build";
        if ((*ce)["last_round"].is_string()) sub += " \xC2\xB7 round " + (*ce)["last_round"].get<std::string>().substr(0, 10);
    } else sub = "Build " + a.configName;
    if (!a.sdkOk) sub = "HairWorks runtime not loaded: open Settings (gear)";
    d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(x, y), a.sdkOk ? tc(tok::DIM) : tc(tok::WARN), sub.c_str());
    d->PopClipRect();
    // ellipsis menu: Switch build, Open folder, Reload
    ImGui::SetCursorScreenPos(ImVec2(wp.x + W - S(14) - S(26), p.y));
    bool mb = ImGui::InvisibleButton("##whomenu", S2(26, 26));
    bool hov = ImGui::IsItemHovered();
    tip("Build menu\nSwitch build, open the build folder, re-read the build.");
    ImVec2 mp = ImGui::GetItemRectMin();
    if (hov) d->AddRectFilled(mp, ImGui::GetItemRectMax(), tc(tok::BG3), S(5));
    w::icon(d, ICON_LC_ELLIPSIS, ImVec2(mp.x + S(13), mp.y + S(13)), S(16), hov ? tc(tok::TEXT) : tc(tok::MUTED));
    if (mb) ImGui::OpenPopup("##whopop");
    if (ImGui::BeginPopup("##whopop")) {
        if (ImGui::BeginMenu("Switch build")) {
            if (ce) {
                for (auto& v : (*ce)["variants"]) {
                    std::string cfg = v.value("cfg", "");
                    bool built = v.value("built", false);
                    if (ImGui::MenuItem(cfg.c_str(), built ? nullptr : "build", cfg == a.configName, !a.job.running)) a.openAsync(cfg, !built);
                }
            } else {
                // no catalog entry: the old list of built configs
                if (a.configs.empty()) a.listConfigs();
                for (auto& c : a.configs) {
                    std::string n = c.substr(2);
                    if (ImGui::MenuItem(n.c_str(), nullptr, n == a.configName)) a.openConfig(n, false);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Open folder")) ShellExecuteA(nullptr, "open", a.pack.dir.c_str(), nullptr, nullptr, SW_SHOW);
        if (ImGui::MenuItem("Reload")) a.openConfig(a.configName, true);
        ImGui::Separator();
        if (ImGui::MenuItem("Characters...", "Ctrl+O")) a.pickerOpen = true;
        ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + h));
    d->AddLine(ImVec2(wp.x, wp.y + h - 0.5f), ImVec2(wp.x + W, wp.y + h - 0.5f), tc(tok::IRON), 1);
}

// Beard mode switch, under the character card (only for builds with a beard fur): which fur the tools, Look, Physics,
// Stiffness map, checks, budget and export edits act on; the other fur is drawn as a preview
static void furSwitch(App& a) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float y = ImGui::GetCursorScreenPos().y + S(10);
    d->AddText(g_fontCap, g_fontCap->FontSize, ImVec2(wp.x + S(16), y + S(16) - g_fontCap->FontSize * 0.5f), tc(tok::MUTED), "EDITING");
    ImGui::SetCursorScreenPos(ImVec2(wp.x + S(16) + w::textSize(g_fontCap, "EDITING").x + S(12), y));
    const char* lb[] = {"Hair##fur", "Beard##fur"};
    const char* tp[] = {"Hair mode\nThe tools, Look, Physics, Stiffness map, checks and the budget act on the hair; the beard is a preview.",
                        "Beard mode\nThe tools, Look, Physics, Stiffness map, checks and the budget act on the beard fur; the hair is a preview.\n"
                        "Both edit sets are kept apart, saved in the project and exported (the beard edits go into the build's beard fur)."};
    int v = a.fur;
    if (w::segmented("##fursw", lb, 2, &v, tp) && v != a.fur) a.setFur(v);
    ImGui::SetCursorScreenPos(ImVec2(wp.x, ImGui::GetCursorScreenPos().y));
}

static void drawInspector(App& a) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float top = vp->Pos.y + g_titleH, H = vp->Size.y - g_titleH - g_statusH;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - g_panelW, top));
    ImGui::SetNextWindowSize(ImVec2(g_panelW, H));
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(340), H), ImVec2(std::max(S(360), vp->Size.x * 0.6f), H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(4)));
    ImGui::Begin("Inspector", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
    g_panelW = ImGui::GetWindowWidth();
    whoHeader(a);
    if (a.beardOn) furSwitch(a);
    budgetCard(a);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), 0));
    ImGui::BeginChild("##sections", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    bool exportNow = a.exportReq;
    if (exportNow) {
        ImGuiID eid = ImGui::GetID("Export");
        ImGui::GetStateStorage()->SetInt(eid, 1);
    }
    auto bodyEnd = [] { ImGui::Dummy(ImVec2(0, S(10))); };
    // ---- Material
    {
        std::string sum = a.presets.empty() ? "-" : shortPreset(a.presets[a.presetSel].name);
        if (w::sectionHeader("Material", "Material", ICON_LC_PALETTE, w::CI_NONE, sum.c_str(), false, true)) {
            materialTiles(a);
            w::switchRow("Keep the character's colours", &a.keepColours, "On: switching presets keeps root / tip / specular colours and texture brightness.");
            ImGui::PushFont(g_fontSmall);
            ImGui::TextColored(tv(tok::DIM), "Shading + strand shape. Density and the density map stay the character's.");
            ImGui::PopFont();
            tip("Strand material = shading + strand shape (width, clumping, waves, specular, glint, diffuse). Density and the density map stay the character's.");
            bodyEnd();
        }
    }
    // ---- Look
    {
        int nLook = 0;
        for (auto& d : paramDefs())
            if (strcmp(d.group, "Dynamics") && strcmp(d.group, "LOD") && a.look.over.count(d.path)) nLook++;
        std::string sum = nLook ? std::to_string(nLook) + " changed" : "Root / tip colour, specular";
        if (w::sectionHeader("Look", "Look", ICON_LC_SPARKLES, w::CI_NONE, sum.c_str(), nLook > 0, false)) {
            const char* grp = "";
            for (auto& d : paramDefs()) {
                if (strcmp(d.group, "Dynamics") == 0 || strcmp(d.group, "LOD") == 0) continue;
                if (strcmp(grp, d.group) != 0) { grp = d.group; w::subHeader(grp); }
                ImGui::PushID(d.path);
                if (d.kind == PKind::Color) colourRow(a, d);
                else paramSlider(a, d, true);
                ImGui::PopID();
            }
            bodyEnd();
        }
    }
    // ---- Physics and motion (motion controls live in the viewport playback bar)
    {
        int nDyn = 0;
        for (auto& d : paramDefs()) if (!strcmp(d.group, "Dynamics") && a.look.over.count(d.path)) nDyn++;
        std::string sum = nDyn ? std::to_string(nDyn) + " changed" : "No changes";
        if (w::sectionHeader("Physics and motion", "Physics & motion", ICON_LC_ACTIVITY, w::CI_NONE, sum.c_str(), nDyn > 0, false)) {
            for (auto& d : paramDefs()) {
                if (strcmp(d.group, "Dynamics") != 0 || d.kind == PKind::Curve) continue;
                ImGui::PushID(d.path);
                paramSlider(a, d, false);
                ImGui::PopID();
            }
            curveEditor(a);
            ImGui::Dummy(ImVec2(0, S(4)));
            w::switchRow("Simulate", &a.simulate, "Simulate\nOff = rest pose (what you edit).");
            w::switchRow("Close-up LOD", &a.forceCloseLod, "Close-up LOD\nShow the close-up (detail LOD) hair count at any distance: the dialogue worst case.");
            bool ewm = a.editWhileMoving;
            if (w::switchRow("Edit while moving", &ewm,
                             "Edit while moving\nOff (default): an editing tool pauses the motion and edits the bind pose. On: edit the posed hair while it moves; "
                             "the overlay follows the pose and every edit is converted back to rest space before it is stored.")) {
                a.editWhileMoving = ewm;
                if (ewm && a.editPose) a.resumeMotion();
            }
            ImGui::Dummy(ImVec2(0, S(4)));
            if (a.hw.boneStiffInFur) {
                bool rr = false;
                w::sliderRow("bone stiffness emulation", "W3 bone stiffness", &a.hw.boneStiffEmu, 0, 1, "%.2f", 1.0f, a.hw.boneStiffEmu != 1.0f, &rr,
                             "W3 bone stiffness emulation\nThis fur uses the game-only stiffnessBone block, which the HairWorks 1.2.1 runtime does not have. "
                             "1 (default) raises the stiffness / strength / bend curves by the fur's stiffnessBone curve so curls hold like in game "
                             "(calibrated on L237 in-game shots); 0 = raw runtime (curls sag ~2 cm more). Preview only: not exported.");
                if (rr) a.hw.boneStiffEmu = 1.0f;
            } else {
                w::note(ICON_LC_INFO, tok::SAND, "This fur has no stiffnessBone block: the runtime simulates it as in game.");
            }
            bodyEnd();
        }
    }
    // ---- Stiffness map
    {
        int painted = 0;
        for (size_t i = 0; i < a.smap.guideS.size(); i++)
            if (a.smap.guideS[i] != 1.0f || (i < a.smap.guideR.size() && a.smap.guideR[i] != 1.0f)) painted++;
        std::string sum = a.smap.painted ? "Painted \xC2\xB7 " + std::to_string(painted) + " guides" : "Not painted";
        if (w::sectionHeader("Stiffness map", "Stiffness map", ICON_LC_PAINTBRUSH, w::CI_NONE, sum.c_str(), a.smap.painted, false)) {
            const char* tg[] = {"stiffness + root stiffness", "stiffness only", "root stiffness only"};
            comboFull("##pt", &a.paintTarget, tg, 3, "What the Paint tool writes.");
            bool r = false;
            w::sliderRow("stiffness x", "Stiffness x", &a.paintS, 0, 1, "%.2f", NAN, false, &r, "Value painted for stiffness (multiplies the stiffness constant).");
            w::sliderRow("root stiffness x", "Root stiffness x", &a.paintR, 0, 1, "%.2f", NAN, false, &r, "Value painted for root stiffness.");
            w::sliderRow("brush px", "Brush px", &a.brushPx, 8, 200, "%.0f", NAN, false, &r, "Brush radius in pixels (comb, curl and paint).");
            float hw = (ImGui::GetContentRegionAvail().x - S(8)) * 0.5f;
            if (w::button("Fill all with value", "Fill all with value", ImVec2(hw, 0))) { a.smap.ensure(a.groom.P.size()); for (auto& v : a.smap.guideS) v = a.paintS; for (auto& v : a.smap.guideR) v = a.paintR; a.stiffDirty = true; }
            ImGui::SameLine(0, S(8));
            if (w::button("Clear map", "Clear map", ImVec2(hw, 0))) { a.smap.reset(a.groom.P.size()); a.stiffDirty = true; }
            smallNote(a.smap.painted ? "Painted: exported as an xbm + physicalMaterials.stiffness.stiffnessTex / rootStiffnessTex (opt-in)." : "Not painted: the fur keeps its constant stiffness.");
            if (a.smap.overlapTexels) smallNote("Warning: " + std::to_string(a.smap.overlapTexels) + " texels shared by faces with different values (the card atlas reuses UVs).", tok::WARN);
            bodyEnd();
        }
    }
    // ---- Curl tools
    {
        int nc = curlOps(a.groom);
        std::string mode = a.curl.mode.empty() ? "ringlet" : a.curl.mode;
        mode[0] = (char)toupper((unsigned char)mode[0]);
        std::string sum = nc ? std::to_string(nc) + (nc == 1 ? " curl" : " curls") : mode + " \xC2\xB7 no curls yet";
        if (w::sectionHeader("Curl tools", "Curl tools", nullptr, w::CI_CURL, sum.c_str(), nc > 0, false)) {
            const char* modes[] = {"ringlet", "roll", "wave"};
            int m = a.curl.mode == "roll" ? 1 : a.curl.mode == "wave" ? 2 : 0;
            if (comboFull("##cm", &m, modes, 3, "ringlet = helix around the strand (vertical curls); roll = the end rolls up / under around a horizontal axis; wave = planar sine / zigzag.")) a.curl.mode = modes[m];
            bool r = false;
            w::sliderRow("start", "Start", &a.curl.start, 0, 0.95f, "%.2f", NAN, false, &r, "Fraction of the strand length where the shape starts.");
            w::sliderRow("radius mm", "Radius", &a.curl.radius, 0.002f, 0.05f, "%.3f", NAN, false, &r, "Ringlet radius / loop radius / wave amplitude (m).");
            w::sliderRow("turns", "Turns", &a.curl.turns, 0.25f, 6.0f, "%.2f", NAN, false, &r, "Turns (roll: >1 loops on itself) or wave periods.");
            if (a.curl.mode == "ringlet") {
                w::sliderRow("ramp", "Ramp", &a.curl.ramp, 0, 1, "%.2f", NAN, false, &r, "Fraction over which the ringlet radius grows from 0.");
                bool left = a.curl.hand < 0;
                if (w::switchRow("left-handed", &left, "Helix handedness.")) a.curl.hand = left ? -1 : 1;
            }
            if (a.curl.mode == "roll") {
                w::sliderRow("shrink", "Shrink", &a.curl.shrink, 0.2f, 1.0f, "%.2f", NAN, false, &r, "Radius at the end = radius x shrink (spirals inwards).");
                bool out = a.curl.side > 0;
                if (w::switchRow("roll outward", &out, "Off = rolls under (towards the head).")) a.curl.side = out ? 1 : -1;
                w::switchRow("band (nape roll)", &a.curl.band, "Every guide rolls in the same vertical front-back plane (Iris nape roll).");
            }
            if (a.curl.mode == "wave") w::switchRow("zigzag", &a.curl.zigzag, "Triangle wave instead of sine.");
            w::sliderRow("jitter", "Jitter", &a.curl.jitter, 0, 1, "%.2f", NAN, false, &r, "Per-strand random phase and radius.");
            float cpt = curlCvsPerTurn(a.groom.K, a.curl);
            if (cpt < 6) { char b[96]; snprintf(b, sizeof b, "Only %.1f CVs per turn: lower the turns or start earlier.", cpt); smallNote(b, tok::WARN); }
            if (cpt < 6 && a.groom.K < kMaxCvs) {
                // more CVs on every guide (same shape: old CVs kept, new ones on the smooth curve): the Studio resample op
                int want = std::clamp(int(std::ceil((a.groom.K - 1) * 6.0f / std::max(cpt, 0.1f))) + 1, 16, kMaxCvs);   // 6 CVs per turn
                std::string lbl = "Resample all guides to " + std::to_string(want) + " CVs";
                if (w::button(lbl.c_str(), lbl.c_str(), ImVec2(-1, 0))) {
                    a.groom.push({{"type", "resample"}, {"n", want}});
                    a.editsChanged();
                    a.status = "All guides resampled to " + std::to_string(a.groom.K) + " CVs (shape kept)";
                }
                tip("Every guide gets more control points along its own smooth curve (old points kept), so curls and lengthening keep their shape. Render cost grows with the CVs.");
            }
            if (w::button("Curl selection (C)", "Curl selection (C)", ImVec2(-1, 0))) a.opCurl(a.selList());
            if (w::button("Hold curls in the simulation", "Hold curls in the simulation", ImVec2(-1, 0))) a.holdCurlsPhysics();
            tip("Raises stiffness / root / tip / bend stiffness and flattens the curves so HairWorks keeps the curled rest shape.");
            bodyEnd();
        }
    }
    // ---- Symmetry
    if (w::sectionHeader("Symmetry", "Symmetry", ICON_LC_FLIP_HORIZONTAL_2, w::CI_NONE, a.symmetry ? "Live mirror on" : "Live mirror off", a.symmetry, false)) {
        w::switchRow("Live symmetry (X)", &a.symmetry, "Every edit on one side is applied to the partner guides on the other side (nearest mirrored root within 15 mm).");
        w::subHeader("Mirror side");
        const char* srcs[] = {"copy the character's left (+x) onto the right", "copy the character's right (-x) onto the left"};
        comboFull("##msrc", &a.mirrorSource, srcs, 2, nullptr);
        bool r = false;
        w::sliderRow("centre band mm", "Centre band mm", &a.mirrorBandMm, 0, 40, "%.0f", NAN, false, &r, "Guides whose root is within this distance of the mid-plane stay as they are.");
        bool lim = a.mirrorYmin > -0.5f;
        if (w::switchRow("keep the back", &lim, "Only mirror guides whose root is in front of y = ymin (keeps a tie / bun at the back as it is).")) a.mirrorYmin = lim ? a.pack.headC.y - 0.06f : -1.0f;
        if (lim) w::sliderRow("ymin", "ymin (m)", &a.mirrorYmin, a.pack.headC.y - 0.15f, a.pack.headC.y + 0.1f, "%.3f", NAN, false, &r, "Front limit of the mirrored guides.");
        if (w::button("Mirror side", "Mirror side", ImVec2(-1, 0))) a.opMirror();
        tip("Same op as strand_edits.py 'mirror' (source, plane_x auto, band, ymin): the target side is rebuilt from the source side, "
            "roots snapped to the scalp, segment lengths kept, centre band untouched.");
        smallNote("mid-plane x = " + std::to_string(a.groom.mirrorPlane()).substr(0, 7) + " m (auto, from the head mesh)");
        bodyEnd();
    }
    // ---- Edit settings
    {
        std::string sum = std::to_string(a.selList().size()) + " selected \xC2\xB7 " + std::to_string(a.groom.ops.size()) + " ops";
        if (w::sectionHeader("Edit settings", "Edit settings", ICON_LC_MOVE, w::CI_NONE, sum.c_str(), false, false)) {
            const char* fo[] = {"carry", "linear", "tip"};
            int fi = a.falloff == "linear" ? 1 : a.falloff == "tip" ? 2 : 0;
            if (comboRow("move falloff", &fi, fo, 3, "carry: the whole strand moves; linear: root fixed, tip moves fully; tip: mostly the lower part.")) a.falloff = fo[fi];
            bool r = false;
            float fk = (float)a.fromK;
            if (w::sliderRow("from CV", "From CV", &fk, 0, (float)std::max(a.groom.K - 1, 1), "%.0f", NAN, false, &r, "CVs before this one stay fixed.")) a.fromK = (int)std::lround(fk);
            w::sliderRow("comb strength", "Comb strength", &a.combStrength, 0.1f, 2.0f, "%.2f", NAN, false, &r, "How strongly the comb drags guides.");
            w::sliderRow("keep %", "Keep %", &a.keepPct, 10, 100, "%.0f", NAN, false, &r, "Shorten keeps this percentage of the length.");
            w::sliderRow("dup step mm", "Dup step mm", &a.dupStepMm, 1, 20, "%.0f", NAN, false, &r, "Distance of a duplicate from its source.");
            smallNote(std::to_string(a.selList().size()) + " selected, " + std::to_string(a.groom.edited.size()) + " edited, " + std::to_string(a.groom.ops.size()) + " ops" +
                      (a.groom.unmatched ? "   (some roots did not match!)" : ""));
            bodyEnd();
        }
    }
    // ---- Export
    {
        std::string sum = std::string(a.modName[0] ? a.modName : "mod?") + " \xC2\xB7 " + a.tag;
        if (exportNow) ImGui::SetScrollHereY(0.0f);
        if (w::sectionHeader("Export", "Export", ICON_LC_PACKAGE, w::CI_NONE, sum.c_str(), false, true)) {
            float iw = ImGui::GetContentRegionAvail().x - S(128);
            rowLabel("mod folder", iw);
            w::pushInputStyle();
            ImGui::InputText("##mod folder", a.modName, sizeof a.modName);
            w::popInputStyle();
            tip("The mod folder name (starts with 'mod').");
            rowLabel("tag", iw);
            w::pushInputStyle();
            ImGui::InputText("##tag", a.tag, sizeof a.tag);
            w::popInputStyle();
            tip("Name of this edit set, used in the file names.");
            ImGui::Dummy(ImVec2(0, S(4)));
            if (w::primaryButton("Export edits + mod", "Export edits + mod", ImVec2(-1, S(36)), !a.bud.overLimit, ICON_LC_PACKAGE)) { std::string o; a.exportAll(o); }
            tip("Writes build\\desktop_studio\\<build>\\export\\: the edits JSON (same format as the web Studio), the look JSON "
                "(only changed values), the config keys, and a ready mod folder + zip built by the Python pipeline from this "
                "character's OWN redfur. Nothing is installed into the game.");
            if (a.bud.overLimit) { char b[64]; snprintf(b, sizeof b, "Blocked: over the %.0f %% budget.", a.look.limit * 100); smallNote(b, tok::WARN); }
            // release export (opt-in): the mod ships a script that calls the Hairwork for All Core, which players install too
            ImGui::Dummy(ImVec2(0, S(2)));
            if (w::button("Export for release (requires Core)", "Export for release (requires Core)", ImVec2(-1, S(30)), ICON_LC_PACKAGE, false, !a.bud.overLimit)) { std::string o; a.exportAll(o, true); }
            tip("Same export, for publishing: the same hair and textures, plus a small script that calls the Hairwork for All "
                "\xE2\x80\x93 Core mod, and REQUIREMENTS.txt + hairworkforall_mod.json (requires_core). "
                "Players must install the Core: the game will refuse to start without the Core (its script compiler reports "
                "the missing Core function, which names the Nexus page).");
            a.checkCore();
            smallNote("Release mods require Hairwork for All \xE2\x80\x93 Core v" + (a.core.required.empty() ? std::string("?") : a.core.required) +
                      " (Nexus): the game will refuse to start without the Core. The dev export above has no such check.");
            smallNote("Also required (listed in REQUIREMENTS.txt): Graphics Control (Nexus) \xE2\x80\x94 set PT hair > 'Hair root transparency mode' "
                      "to 0; otherwise path-traced hair shows see-through gaps.");
            smallNote(a.core.ok() ? "This PC: " + a.core.summary() + " installed." : "This PC: " + a.core.summary() + ". Install it to test release mods in game.",
                      a.core.ok() ? tok::MUTED : tok::WARN);
            if (w::button("Core on Nexus", "Core on Nexus", ImVec2(-1, 0), ICON_LC_ARROW_RIGHT, true))
                ShellExecuteA(nullptr, "open", App::CORE_NEXUS, nullptr, nullptr, SW_SHOW);
            tip(App::CORE_NEXUS);
            if (!a.lastExport.empty() && w::button("Open export folder", "Open export folder", ImVec2(-1, 0), ICON_LC_FOLDER_OPEN))
                ShellExecuteA(nullptr, "open", a.lastExport.c_str(), nullptr, nullptr, SW_SHOW);
            bodyEnd();
        }
    }
    a.exportReq = false;
    ImGui::Dummy(ImVec2(0, S(20)));
    ImGui::EndChild();
    ImGui::PopStyleVar();
    // left border
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x + 0.5f, wp.y), ImVec2(wp.x + 0.5f, wp.y + H), tc(tok::IRON), 1);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ------------------------------------------------------------------------------------------------ status bar
static void drawStatusBar(App& a) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float W = vp->Size.x, H = g_statusH;
    ImVec2 o(vp->Pos.x, vp->Pos.y + vp->Size.y - H);
    ImGui::SetNextWindowPos(o);
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tv(tok::BG0));
    ImGui::Begin("status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddLine(ImVec2(o.x, o.y + 0.5f), ImVec2(o.x + W, o.y + 0.5f), tc(tok::IRON), 1);
    ImGui::PushFont(g_fontSmall);
    float cy = o.y + H * 0.5f, x = o.x + S(14);
    float lh = ImGui::GetFontSize();
    // left: status message
    std::string msg;
    ImU32 ic = tc(tok::OK);
    const char* glyph = ICON_LC_CIRCLE_CHECK;
    if (a.job.running) {
        std::string ll;
        { std::lock_guard<std::mutex> lk(a.job.m); ll = a.job.lastLine; }
        msg = a.job.title + ": " + ll;
        glyph = nullptr;
        float t = (float)ImGui::GetTime() * 6;
        d->PathArcTo(ImVec2(x + S(6), cy), S(5), t, t + 4.2f, 12);
        d->PathStroke(tc(tok::GLOW), 0, S(1.5f));
    } else {
        msg = a.status;
        std::string low = msg;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low.find("fail") != std::string::npos || low.find("error") != std::string::npos || low.find("not found") != std::string::npos ||
            low.find("missing") != std::string::npos || low.find("blocked") != std::string::npos || low.find("invalid") != std::string::npos) {
            glyph = ICON_LC_TRIANGLE_ALERT;
            ic = tc(tok::WARN);
        }
    }
    if (glyph) w::icon(d, glyph, ImVec2(x + S(6), cy), S(13), ic);
    float right = o.x + W - S(14);
    // right cells, drawn right to left
    std::vector<float> seps;
    auto cellText = [&](const char* t, ImU32 c) { ImVec2 ts = ImGui::CalcTextSize(t); right -= ts.x; d->AddText(ImVec2(right, cy - lh * 0.5f), c, t); };
    auto cellEnd = [&] { right -= S(16); d->AddLine(ImVec2(right, cy - S(8)), ImVec2(right, cy + S(8)), tc(tok::IRON), 1); right -= S(16); };
    // credit (required): cpu icon + NVIDIA HairWorks 1.2.1 · GameWorks
    {
        const char* cr = "NVIDIA HairWorks 1.2.1 \xC2\xB7 GameWorks";
        float cw = ImGui::CalcTextSize(cr).x + S(20);
        ImGui::SetCursorScreenPos(ImVec2(right - cw, o.y));
        ImGui::InvisibleButton("##credit", ImVec2(cw, H));
        tip(CREDIT);
        cellText(cr, tc(tok::MUTED));
        right -= S(7);
        w::icon(d, ICON_LC_CPU, ImVec2(right - S(6), cy), S(13), tc(tok::MUTED));
        right -= S(13);
    }
    // log toggle
    {
        cellEnd();
        float lw = ImGui::CalcTextSize("Log").x + S(20);
        ImGui::SetCursorScreenPos(ImVec2(right - lw, o.y));
        if (ImGui::InvisibleButton("Log##status", ImVec2(lw, H))) s_logOpen = !s_logOpen;
        tip("Log\nShow / hide the log drawer.");
        bool hv = ImGui::IsItemHovered();
        cellText("Log", s_logOpen ? tc(tok::GLOW) : hv ? tc(tok::TEXT) : tc(tok::MUTED));
        right -= S(7);
        w::icon(d, ICON_LC_TERMINAL, ImVec2(right - S(6), cy), S(13), s_logOpen ? tc(tok::GLOW) : tc(tok::MUTED));
        right -= S(13);
    }
    // Hairwork for All Core (release exports need it): installed version vs the repo's CORE_VERSION
    {
        a.checkCore();
        cellEnd();
        std::string cs = a.core.summary();
        float cw = ImGui::CalcTextSize(cs.c_str()).x + S(20);
        ImGui::SetCursorScreenPos(ImVec2(right - cw, o.y));
        ImGui::InvisibleButton("##corestatus", ImVec2(cw, H));
        tip(("Hairwork for All \xE2\x80\x93 Core\nDependency mod of release exports (script check: detected by its "
             "hairworkforall_core.ws and version.json). " +
             std::string(a.core.ok() ? "Installed: v" + a.core.version + " (required v" + a.core.required + ")."
                                      : !a.core.installed ? "Not installed in " + a.core.modDir + ": the game will refuse to start with a release mod installed."
                                      : "Installed but older than v" + a.core.required + " (or no script / version.json): update it.") +
             "\n" + App::CORE_NEXUS).c_str());
        cellText(cs.c_str(), a.core.ok() ? tc(tok::MUTED) : tc(tok::WARN));
        right -= S(7);
        w::icon(d, a.core.ok() ? ICON_LC_CIRCLE_CHECK : ICON_LC_TRIANGLE_ALERT, ImVec2(right - S(6), cy), S(13), a.core.ok() ? tc(tok::OK) : tc(tok::WARN));
        right -= S(13);
    }
    if (a.pickerOpen || !a.loaded) {
        cellEnd();
        cellText("opens", tc(tok::MUTED));
        right -= S(6);
        float kw = w::kbdWidth("Enter");
        right -= kw;
        w::kbd(d, ImVec2(right, cy - S(8)), "Enter");
        right -= S(6);
        cellText("Click for details \xC2\xB7 double-click or", tc(tok::MUTED));
    } else {
        // fps
        cellEnd();
        cellText("fps", tc(tok::MUTED));
        right -= S(6);
        char fb[16];
        snprintf(fb, sizeof fb, "%.0f", a.fps);
        cellText(fb, tc(tok::TEXT));
        // edits
        cellEnd();
        int edits = int(a.groom.ops.size() + a.look.over.size());
        std::string et = std::to_string(edits) + (edits == 1 ? " edit" : " edits");
        cellText(et.c_str(), tc(tok::SOFT));
        // state: dot + motion
        cellEnd();
        std::string sm = std::string(motionName(a.editPose ? a.savedPreset : a.motion.preset));
        std::string stt = a.editPose ? "Edit pose \xC2\xB7 " + sm + " paused" : a.paused ? "Paused \xC2\xB7 " + sm : a.simulate ? "Simulating \xC2\xB7 " + sm : "Rest pose \xC2\xB7 " + sm;
        cellText(stt.c_str(), tc(tok::SOFT));
        right -= S(12);
        ImU32 dc = a.editPose ? tc(tok::GLOW) : (a.paused || !a.simulate) ? tc(tok::DIM) : tc(tok::OK);
        d->AddCircleFilled(ImVec2(right + S(3.5f), cy), S(6), (dc & 0x00ffffff) | 0x40000000, 12);
        d->AddCircleFilled(ImVec2(right + S(3.5f), cy), S(3.5f), dc, 12);
        right -= S(4);
        // hairs + meter + %
        cellEnd();
        char pb[16];
        snprintf(pb, sizeof pb, "%.0f %%", a.bud.ratio * 100);
        cellText(pb, a.bud.overLimit ? tc(tok::WARN) : tc(tok::TEXT));
        right -= S(8) + S(90);
        ImVec2 m0(right, cy - S(2.5f)), m1(right + S(90), cy + S(2.5f));
        d->AddRectFilled(m0, m1, tc(tok::BG3), S(3));
        float fr = std::min((float)a.bud.ratio / 1.3f, 1.0f);
        d->AddRectFilled(m0, ImVec2(m0.x + S(90) * fr, m1.y), a.bud.overLimit ? tc(tok::WARN) : tc(tok::AMBER), S(3));
        d->AddLine(ImVec2(m0.x + S(90) / 1.3f, m0.y - S(2)), ImVec2(m0.x + S(90) / 1.3f, m1.y + S(2)), tc(tok::SILVER, 0.7f), 1);
        right -= S(8);
        std::string hs = fmtCount(a.bud.hairsClose);
        cellText(hs.c_str(), tc(tok::TEXT));
        right -= S(6);
        cellText("Hairs", tc(tok::MUTED));
    }
    // message (clipped before the cells)
    d->PushClipRect(ImVec2(x, o.y), ImVec2(right - S(16), o.y + H), true);
    d->AddText(ImVec2(x + S(20), cy - lh * 0.5f), a.job.running ? tc(tok::GLOW) : tc(tok::SOFT), msg.c_str());
    d->PopClipRect();
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// ------------------------------------------------------------------------------------------------ frame
void App::ui() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiIO& io = ImGui::GetIO();
    pollJob();
    bool windLock = windBusy;   // the wind check owns the groom and the simulation: everything but its panel is disabled
    if (windLock) ImGui::BeginDisabled();
    drawTitleBar(*this);
    drawRail(*this);
    bool inspector = loaded && !pickerOpen;
    if (inspector) drawInspector(*this);
    drawStatusBar(*this);
    drawToolbar();   // viewport overlays: top / playback bars, HUD, gizmo, hint card (toolbar.cpp)
    if (pickerOpen || !loaded) drawPicker();
    drawStyleMenu();   // editor hairstyle switcher popup + the unsaved-edits modal (stylesw.cpp)

    // log drawer (status bar "Log")
    if (s_logOpen) {
        float x0 = vp->Pos.x + g_railW, x1 = vp->Pos.x + vp->Size.x - (inspector ? g_panelW : 0), hh = S(220);
        ImGui::SetNextWindowPos(ImVec2(x0, vp->Pos.y + vp->Size.y - g_statusH - hh));
        ImGui::SetNextWindowSize(ImVec2(x1 - x0, hh));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, tv(tok::BG0, 0.97f));
        ImGui::Begin("##logdrawer", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushFont(g_fontCap);
        ImGui::TextColored(tv(tok::DIM), "LOG");
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - S(20));
        if (ImGui::SmallButton("x##logclose")) s_logOpen = false;
        ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(g_fontSmall);
        for (auto& l : logLines()) ImGui::TextUnformatted(l.c_str());
        ImGui::PopFont();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleColor();
    }

    if (vanillaOpen) drawVanillaRef();
    if (communityOpen) drawCommunity(*this, communityOpen);

    // Check > Unnatural endings
    if (endingsOpen && loaded) {
        checkEndings();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + S(16), vp->Pos.y + g_titleH + S(110)), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(S(360), 0), ImGuiCond_Appearing);
        if (ImGui::Begin("Unnatural endings", &endingsOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Faces whose 3 guides go different ways: the hair between them fans across the gap and ends in a hard line. Flagged guides are orange in the viewport.");
            ImGui::Text("%d flagged faces, %d guides", (int)endFaces.size(), (int)endGuides.size());
            ImGui::SliderFloat("direction angle (deg)##enddiv", &endDiv, 10, 120, "%.0f");
            ImGui::SliderFloat("tip spread (x root spacing)##endspread", &endSpread, 1, 12, "%.1f");
            if (ImGui::Button("Select flagged guides##endsel")) { sel = endGuides; }
            ImGui::SameLine();
            if (ImGui::Button("Remove fanned hair##endrm")) removeFanned();
            ImGui::TextWrapped("Removing faces deletes the hair between the guides (one undo step) and can open scalp: check the coverage afterwards.");
        }
        ImGui::End();
    }

    // Check > Unwanted loops
    if (loopsOpen && loaded) {
        checkLoops();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + S(16), vp->Pos.y + g_titleH + S(110)), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(S(360), 0), ImGuiCond_Appearing);
        if (ImGui::Begin("Unwanted loops", &loopsOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Guides that come back to themselves: the strand returns close to a point it passed (or its root) after a long run of hair. Tight repeated turns (curls) are not flagged. Flagged guides are purple in the viewport.");
            ImGui::Text("%d flagged guides", (int)loopList.size());
            ImGui::SliderFloat("min hair between (mm)##looparc", &loopArc, 20, 300, "%.0f");
            ImGui::SliderFloat("return distance (mm)##loopclose", &loopClose, 3, 40, "%.0f");
            ImGui::SliderFloat("curl size, not a loop (mm)##loopcurl", &loopCurlR, 10, 100, "%.0f");
            ImGui::SliderFloat("circling (deg)##loopturn", &loopTurn, 180, 720, "%.0f");
            if (ImGui::Button("Select flagged guides##loopsel")) { sel = loopGuides; }
            ImGui::SameLine();
            if (ImGui::Button("Delete flagged guides##looprm")) removeLoops();
            ImGui::TextWrapped("Deleting is one undo step.");
        }
        ImGui::End();
    }

    // light settings (time-of-day popup "Advanced...", View > Light settings)
    if (lightOpen) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - (inspector ? g_panelW : 0) - S(16), vp->Pos.y + g_titleH + S(110)), ImGuiCond_Appearing, ImVec2(1, 0));
        ImGui::SetNextWindowSize(ImVec2(S(380), 0), ImGuiCond_Appearing);
        // the light settings are taller than small viewports: cap the height, the window scrolls
        ImGui::SetNextWindowSizeConstraints(ImVec2(S(300), 0), ImVec2(S(600), std::max(S(300), vp->Size.y - g_titleH - S(150))));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(10));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(17 / 255.f, 20 / 255.f, 23 / 255.f, 0.96f));
        if (ImGui::Begin("Light", &lightOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            static const char* dbgNames[] = {"Lit (normal)", "Debug: N.L diffuse", "Debug: specular only", "Debug: shadow term", "Debug: albedo", "Debug: tangents"};
            static const char* qNames[] = {"Fast (sun shadow only)", "Balanced (8 sky maps)", "High (16 sky maps)"};
            comboRow("lighting quality", &rig.quality, qNames, 3, "Sky occlusion: depth maps of the head, body and hair seen from 8 or 16 sky directions give soft ambient occlusion, hair self-occlusion and hair shadowing the skin. Fast = sun shadow only (cheapest).");
            comboRow("hair view", &hairDebug, dbgNames, 6, "Hair debug views (F6 cycles): the diffuse light term, the specular term alone, the sun visibility (shadow map + self shadow), the albedo, the strand tangents.");
            {
                float wdt = ImGui::GetContentRegionAvail().x - S(128);
                rowLabel("time of day", wdt);
                w::pushInputStyle();
                if (ImGui::BeginCombo("##time of day", timeOfDay(rig.tod).name)) {
                    for (int i = 0; i < timeOfDayCount(); i++) {
                        if (ImGui::Selectable(timeOfDay(i).name, rig.tod == i)) applyTimeOfDay(rig, i);
                        tip(timeOfDay(i).tip);
                    }
                    ImGui::EndCombo();
                }
                w::popInputStyle();
                tip("Lighting preset (L cycles). The HairLab capture rig is calibrated against the in-game SDR captures: use it for comparisons.");
            }
            drawSunGizmo(86 * g_ui);
            w::switchRow("shadows", &rig.shadows, "Sun shadow map: soft contact-hardening shadows of the head, body and hair, hair self-shadow through the groom (SDK deep-shadow filter, shadowSigma) (K).");
            w::switchRow("sky backdrop", &rig.backdrop, "Sky backdrop gradient of the time of day.");
            bool r = false;
            w::sliderRow("shadow softness", "Shadow softness", &rig.shadowSoft, 0.5f, 5, "%.2f", NAN, false, &r, "PCF filter radius in shadow-map texels.");
            w::sliderRow("shadow strength", "Shadow strength", &rig.shadowStrength, 0, 1, "%.2f", NAN, false, &r, nullptr);
            w::sliderRow("sun", "Sun", &rig.sun, 0, 8, "%.2f", NAN, false, &r, "Sun intensity (HairLab capture scene: afternoon sun).");
            float az = rig.sunAz * 57.2958f, el = rig.sunEl * 57.2958f;
            if (w::sliderRow("sun azimuth", "Sun azimuth", &az, -180, 180, "%.0f", NAN, false, &r, "Degrees; top of the dial = the character's face side.")) rig.sunAz = az / 57.2958f;
            if (w::sliderRow("sun elevation", "Sun elevation", &el, 0, 90, "%.0f", NAN, false, &r, "Degrees above the horizon.")) rig.sunEl = el / 57.2958f;
            w::sliderRow("sky", "Sky", &rig.sky, 0, 3, "%.2f", NAN, false, &r, "Sky (ambient) intensity.");
            w::sliderRow("exposure", "Exposure", &rig.exposure, 0.2f, 3, "%.2f", NAN, false, &r, "Exposure before the SDR tone curve.");
            w::sliderRow("colour gain", "Colour gain", &rig.colourGain, 0.2f, 4, "%.2f", NAN, false, &r, "Calibration: rendered hair colour vs the in-game captures.");
            w::sliderRow("spec gain", "Spec gain", &rig.specGain, 0, 4, "%.2f", NAN, false, &r, nullptr);
            w::sliderRow("self shadow", "Self shadow", &rig.selfShadow, 0, 1, "%.2f", NAN, false, &r, nullptr);
            w::sliderRow("floor", "Floor", &rig.floor, 0, 0.1f, "%.3f", NAN, false, &r, "Additive light floor (0 with the sky-occlusion lighting; kept for comparisons).");
            w::sliderRow("horizon", "Horizon", &rig.horizon, 0, 3, "%.2f", NAN, false, &r, "Horizon band light (trees / haze around the character).");
            w::sliderRow("ground", "Ground", &rig.ground, 0, 1, "%.2f", NAN, false, &r, "Ground albedo: bounce light from below (lit by the sun and the sky).");
            w::sliderRow("sun size", "Sun size", &rig.sunSize, 0.1f, 8, "%.2f", NAN, false, &r, "Angular radius of the light (degrees): shadow penumbra grows with the distance to the occluder.");
            w::sliderRow("sky occlusion", "Sky occlusion", &rig.aoStrength, 0, 1, "%.2f", NAN, false, &r, "Strength of the sky-occlusion maps (ambient occlusion, hair self-occlusion).");
            w::sliderRow("hair depth", "Hair depth", &rig.sigmaScale, 0.1f, 3, "%.2f", NAN, false, &r, "Scales the fur's shadowSigma: how deep light gets into the groom before it is shadowed.");
            w::sliderRow("hair scattering", "Hair scattering", &rig.msSat, 0, 6, "%.2f", NAN, false, &r, "Multiple scattering inside the groom: sun-lit light hair gets a deeper, more saturated colour.");
            w::sliderRow("back light", "Back light", &rig.ttGain, 0, 3, "%.2f", NAN, false, &r, "Light transmitted through back-lit strands (bright rim on light hair).");
            w::sliderRow("skin scattering", "Skin scattering", &rig.skinSss, 0, 1, "%.2f", NAN, false, &r, "Skin subsurface wrap: softer, warmer light falloff and shadow edges on faces.");
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // viewport overlays: box select, light gizmo, brush cursor
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (dragging && boxSel) {
        fg->AddRectFilled(ImVec2(std::min(mx0, io.MousePos.x), std::min(my0, io.MousePos.y)), ImVec2(std::max(mx0, io.MousePos.x), std::max(my0, io.MousePos.y)), tc(tok::GLOW, 0.06f));
        fg->AddRect(ImVec2(mx0, my0), io.MousePos, ImGui::GetColorU32(C_GLOW), 0, 0, 1.0f);
    }
    bool inView = !io.WantCaptureMouse;
    // light-direction gizmo (Light window open): sun marker, arrow from the sun to the head, ground shadow direction
    if (loaded && !pickerOpen && (lightOpen || (io.KeyCtrl && ImGui::IsMouseDown(ImGuiMouseButton_Right)))) {
        float ce = std::cos(rig.sunEl);
        V3 L{ce * std::sin(rig.sunAz), ce * std::cos(rig.sunAz), std::sin(rig.sunEl)};
        V3 h{pack.headC.x, pack.headC.y, pack.headC.z};
        V3 sp{h.x + L.x * 0.42f, h.y + L.y * 0.42f, h.z + L.z * 0.42f};
        V3 tipP{h.x + L.x * 0.14f, h.y + L.y * 0.14f, h.z + L.z * 0.14f};
        XMFLOAT2 a2 = project(sp), b2 = project(tipP), g0 = project({h.x, h.y, 0.0f}), g1 = project({h.x - L.x * 0.4f / std::max(ce, 0.2f) * ce, h.y - L.y * 0.4f / std::max(ce, 0.2f) * ce, 0.0f});
        ImU32 amber = ImGui::GetColorU32(C_GLOW), dim = ImGui::GetColorU32(ImVec4(0.94f, 0.63f, 0.25f, 0.45f));
        ImVec2 A(a2.x, a2.y), B(b2.x, b2.y);
        fg->AddLine(A, B, amber, 2.5f);
        ImVec2 dd(B.x - A.x, B.y - A.y);
        float len = std::sqrt(dd.x * dd.x + dd.y * dd.y);
        if (len > 1) {
            dd.x /= len; dd.y /= len;
            ImVec2 nrm(-dd.y, dd.x);
            fg->AddTriangleFilled(B, ImVec2(B.x - dd.x * 14 + nrm.x * 7, B.y - dd.y * 14 + nrm.y * 7), ImVec2(B.x - dd.x * 14 - nrm.x * 7, B.y - dd.y * 14 - nrm.y * 7), amber);
        }
        fg->AddCircleFilled(A, 11, amber, 24);
        for (int k = 0; k < 8; k++) {
            float an = k * 0.785f;
            fg->AddLine(ImVec2(A.x + std::cos(an) * 14, A.y + std::sin(an) * 14), ImVec2(A.x + std::cos(an) * 20, A.y + std::sin(an) * 20), amber, 2);
        }
        fg->AddLine(ImVec2(g0.x, g0.y), ImVec2(g1.x, g1.y), dim, 2);   // where the shadow falls on the ground
        char buf[64];
        snprintf(buf, sizeof buf, "sun  az %.0f  el %.0f  (Ctrl + right drag)", rig.sunAz * 57.2958f, rig.sunEl * 57.2958f);
        fg->AddText(ImVec2(A.x + 24, A.y - 8), amber, buf);
    }
    // brush cursor: amber ring + dark outer ring, dashed inner ring at 30 %, centre dot
    if (loaded && !pickerOpen && inView && (tool == Tool::Comb || tool == Tool::Curl || tool == Tool::Paint)) {
        ImVec2 m = io.MousePos;
        fg->AddCircle(m, brushPx + 1.5f, IM_COL32(0, 0, 0, 100), 64, 1.0f);
        fg->AddCircle(m, brushPx, tc(tok::GLOW, 0.9f), 64, 1.5f);
        float ri = brushPx * 0.3f;
        for (int k = 0; k < 30; k += 2) {
            float a0 = k * 12 * 0.017453f, a1 = (k + 1) * 12 * 0.017453f;
            fg->PathArcTo(m, ri, a0, a1, 4);
            fg->PathStroke(tc(tok::GLOW, 0.55f), 0, 1.0f);
        }
        fg->AddCircleFilled(m, 2.0f, tc(tok::GLOW), 8);
    }
    // ---- popups
    if (s_settingsReq) { ImGui::OpenPopup("Settings"); s_settingsReq = false; }
    ImGui::SetNextWindowSize(ImVec2(S(560), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        static char sdk[512] = "", dll[512] = "", py[128] = "", game[512] = "";
        static bool init = false;
        if (!init) { snprintf(sdk, sizeof sdk, "%s", set.sdkDir.c_str()); snprintf(dll, sizeof dll, "%s", set.dll.c_str()); snprintf(py, sizeof py, "%s", set.python.c_str()); snprintf(game, sizeof game, "%s", set.gameDir.c_str()); init = true; }
        w::pushInputStyle();
        float iw = ImGui::GetContentRegionAvail().x - S(110);
        rowLabel("SDK folder", iw); ImGui::InputText("##SDK folder", sdk, sizeof sdk); tip("Folder of the NVIDIA HairWorks 1.2.1 SDK (contains bin\\win64\\NvHairWorksDx11.win64.dll). Default: <repo>\\sources\\HairWorks-r1_2_1-15-distro\\HairWorks.");
        rowLabel("DLL", iw); ImGui::InputText("##DLL", dll, sizeof dll); tip("Optional: full path of NvHairWorksDx11.win64.dll (overrides the SDK folder).");
        rowLabel("Game folder", iw); ImGui::InputText("##Game folder", game, sizeof game); tip("The Witcher 3 install folder (contains content\\content0 and mods). The pipeline reads the game's own files from it to build hair; the Core check looks in its mods folder.");
        rowLabel("Python", iw); ImGui::InputText("##Python", py, sizeof py); tip("Command used for the pipeline scripts. Leave it as it is: the release uses its own bundled Python (the python folder); a dev build uses py.");
        w::popInputStyle();
        smallNote("runtime: " + set.dllPath());
        ImGui::Dummy(ImVec2(0, S(6)));
        if (w::primaryButton("Save settings (restart to apply)", "Save settings (restart to apply)", ImVec2(0, 0), true, ICON_LC_SAVE)) {
            set.sdkDir = sdk; set.dll = dll; set.python = py; set.gameDir = game; set.save(); status = "Saved " + set.file;
        }
        ImGui::SameLine(0, S(8));
        if (w::button("Close##settings", "Close", ImVec2(S(100), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (aboutOpen) { ImGui::OpenPopup("About"); aboutOpen = false; }
    if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        loadAppLogo(*this);   // also when the splash was skipped
        if (splashSrv || logoSrv) {
            ID3D11ShaderResourceView* l = splashSrv ? splashSrv : logoSrv;
            float lw = 520 * g_ui, lh = lw * (splashSrv ? (float)splashH / std::max(splashW, 1) : (float)logoH / std::max(logoW, 1));
            ImGui::Image((ImTextureID)l, ImVec2(lw, lh));
        }
        ImGui::TextColored(C_MUTED, "Version %s", GSD_VERSION);
        ImGui::TextUnformatted("Hairwork Studio, part of Hairwork for All (The Witcher 3).");
        ImGui::TextUnformatted("Created by WinterElfeas");
        // project links: amber, underlined on hover, open the default browser
        auto link = [](const char* id, const char* label, const char* url) {
            ImGui::TextColored(C_MUTED, "%s", label);
            ImGui::SameLine();
            ImVec2 r0 = ImGui::GetCursorScreenPos(), ts = ImGui::CalcTextSize(url);
            bool clicked = ImGui::InvisibleButton(id, ts);   // a real item: hover / click (and UI tests) find it
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(r0, tc(tok::GLOW), url);
            if (ImGui::IsItemHovered()) {
                dl->AddLine(ImVec2(r0.x, r0.y + ts.y), ImVec2(r0.x + ts.x, r0.y + ts.y), tc(tok::GLOW), 1.0f);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", url);
            }
            if (clicked) ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
        };
        link("##repo_link", "GitHub", "https://github.com/jlrouzies-fr/Witcher3-HairworkStudio");
        link("##nexus_link", "Nexus Mods", "https://www.nexusmods.com/witcher3/mods/13996");
        ImGui::TextColored(C_MUTED, "Source-available under the Hairwork Studio License (LICENSE.md): shared hair mods must require the Core.");
        ImGui::Separator();
        if (gwLogoSrv) ImGui::Image((ImTextureID)gwLogoSrv, ImVec2((float)gwW * g_ui, (float)gwH * g_ui));
        else ImGui::TextUnformatted("NVIDIA GameWorks");
        ImGui::TextUnformatted(CREDIT);
        ImGui::PushTextWrapPos(520 * g_ui);
        ImGui::TextColored(C_MUTED, "NVIDIA HairWorks is a separate proprietary component licensed under the NVIDIA GameWorks EULA. It is not covered by the Hairwork Studio License.");
        ImGui::PopTextWrapPos();
        if (sdkOk) ImGui::TextColored(C_MUTED, "runtime: %s", hw.buildString.c_str());
        ImGui::PushFont(g_fontSmall);
        ImGui::TextColored(C_MUTED, "Fonts: Cinzel and Inter, SIL Open Font License 1.1. Icons: Lucide (ISC). UI: Dear ImGui (MIT).");
        ImGui::PopFont();
        if (w::button("Close##about", "Close", ImVec2(S(120), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (helpOpen) { ImGui::OpenPopup("Shortcuts"); helpOpen = false; }
    if (ImGui::BeginPopupModal("Shortcuts", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* rows[][2] = {{"Q W E T P A", "Select, Move, Comb, Curl brush, Paint stiffness, Draw a new guide"}, {"Right drag / middle drag / wheel", "Orbit / pan / zoom"},
                                 {"Click, Ctrl+click, Shift+click, drag", "Select, toggle, add, box select"}, {"Del / D / S / C", "Delete / duplicate / shorten / curl the selection"},
                                 {"Ctrl+Z / Ctrl+Y", "Undo / redo"}, {"Ctrl+S", "Save the project"}, {"Ctrl+O", "Characters"},
                                 {"Ctrl+Shift+H", "Hairstyle switcher (breadcrumb / inspector \"Hairstyle\")"}, {"X", "Live symmetry"}, {"G / H / M / N / B / Z / V", "Guides, hair, head+body mesh, colliders, bones, wireframe, symmetry plane"},
                                 {"Shift+G / Ctrl+G", "Guides: front layer only / hide guides behind the body (View menu, toolbar)"},
                                 {"Space / Esc / .", "Play-pause / stop and reset to rest (Esc clears the selection first) / step one frame"},
                                 {"R / J", "Re-teleport the simulation / wind on-off"}, {"Tab", "Edit pose on / off"},
                                 {"1 2 3 4", "Front, 3/4, side, back cameras (HairLab shots)"}, {"F / O", "Frame the head / turntable"},
                                 {"L / K", "Next time of day / shadows on-off"}, {"Ctrl + right drag", "Rotate the sun"}, {"F6", "Hair debug views"},
                                 {"Numpad 1 3 7 9", "Front, right, back, left views"}, {"F12", "Screenshot"}};
        for (auto& r : rows) { ImGui::TextColored(C_GLOW, "%s", r[0]); ImGui::SameLine(300 * g_ui); ImGui::TextUnformatted(r[1]); }
        if (w::button("Close##shortcuts", "Close", ImVec2(S(120), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (windLock) ImGui::EndDisabled();

    // Check > Max-wind strays (windcheck.cpp)
    if ((windOpen || windBusy) && loaded) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + S(16), vp->Pos.y + g_titleH + S(110)), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(S(460), 0), ImGuiCond_Appearing);
        bool open = true;
        if (ImGui::Begin("Max-wind strays", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(S(440));
            ImGui::TextWrapped("Runs the simulation at maximum wind (5) for 6 s, measures how far hair leaves the rest silhouette in 6 cameras "
                               "and reads the simulated guides back from the GPU. The wind is not repeatable, so every measure is the envelope of several runs. "
                               "Candidates are confirmed by hiding them and re-running the same wind. Confirmed culprits are red in the viewport. "
                               "Takes about 2 minutes.");
            if (windBusy) {
                ImGui::ProgressBar(windProgress, ImVec2(S(440), 0), windPhase.c_str());
                if (ImGui::Button("Cancel##windcancel")) windCancel = true;
            } else {
                ImGui::SliderFloat("threshold (px at 600)##windthr", &windThr, 4, 40, "%.0f");
                if (ImGui::Button("Run##windrun")) windStart({{"threshold_px", windThr}});
                if (windResult.contains("error")) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", windResult["error"].get<std::string>().c_str());
                else if (windResult.contains("culprits")) {
                    int fl = 0;
                    for (auto& cm : windResult["cameras"]) fl += cm.value("flagged_px", 0);
                    int nc = 0;
                    for (auto& w : windRows) nc += w.confirmed;
                    ImGui::Text("%d culprit(s), %d confirmed, %d px out of place in all cameras, %d runs, %.0f s", (int)windRows.size(), nc, fl,
                                windResult.value("runs", 0), windResult.value("elapsed_s", 0.0));
                    if (windResult.contains("repeatability")) {
                        auto& rp = windResult["repeatability"];
                        if (rp.value("identical", false)) ImGui::TextWrapped("Repeatability: identical runs (deterministic)");
                        else ImGui::TextWrapped("Repeatability: runs differ (CV up to %.0f mm); measured as the envelope of %d runs",
                                                rp.value("cv_max_diff_mm", 0.0), rp.value("runs", 0));
                    }
                }
                ImGui::BeginChild("##windlist", ImVec2(S(440), S(220)), ImGuiChildFlags_Borders);
                for (size_t i = 0; i < windRows.size(); i++) {
                    auto& w = windRows[i];
                    bool on = true;
                    for (int g : w.guides) on &= sel.count(g) > 0;
                    if (ImGui::Selectable((w.text + "##windrow" + std::to_string(i)).c_str(), on && !w.guides.empty()))
                        sel = std::set<int>(w.guides.begin(), w.guides.end());
                }
                ImGui::EndChild();
                if (ImGui::Button("Select all culprits##windsel")) sel = windGuides;
                ImGui::SameLine();
                if (ImGui::Button("Delete culprits##windrm")) windDeleteCulprits();
                ImGui::TextWrapped("Deleting is one undo step (guides + culprit faces).");
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
        if (!open && !windBusy) windOpen = false;
    }
}

// ================================================================================================ vanilla reference
// floating window with the in-game vanilla stills of this character (one tab per HairLab camera) and "Match camera",
// which puts the viewport on the same eye / target / field of view, so the groom can be compared with vanilla
void App::drawVanillaRef() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + S(90), vp->Pos.y + g_titleH + S(110)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(S(420), S(500)), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(10));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(17 / 255.f, 20 / 255.f, 23 / 255.f, 0.96f));
    if (ImGui::Begin("Vanilla reference", &vanillaOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        std::vector<std::string> ids;
        if (loaded)
            for (auto& [k, sh] : pack.shots.items())
                if (sh.contains("ref") && sh["ref"].is_string()) ids.push_back(k);
        if (ids.empty()) {
            ImGui::TextWrapped("No vanilla capture for this build yet (the reference stills come from the HairLab vanilla rounds).");
        } else {
            if (std::find(ids.begin(), ids.end(), vanillaShot) == ids.end()) vanillaShot = ids[0];
            auto viewName = [&](const std::string& k) {
                std::string v = pack.shots[k].value("view", k);
                if (v == "front34") return std::string("1/4 front R");
                if (v == "front34_left") return std::string("1/4 front L");
                if (v == "side_left") return std::string("side L");
                if (v == "side") return std::string("side R");
                return v;
            };
            for (size_t i = 0; i < ids.size(); i++) {
                if (i) ImGui::SameLine();
                bool on = ids[i] == vanillaShot;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button((viewName(ids[i]) + "##vref" + ids[i]).c_str())) vanillaShot = ids[i];
                if (on) ImGui::PopStyleColor();
            }
            auto& sh = pack.shots[vanillaShot];
            if (ImGui::Button("Match camera##vref")) {
                XMFLOAT3 e{sh["eye"][0], sh["eye"][1], sh["eye"][2]}, t{sh["target"][0], sh["target"][1], sh["target"][2]};
                float dx = e.x - t.x, dy = e.y - t.y, dz = e.z - t.z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
                cam.target = t;
                cam.dist = d;
                cam.yaw = std::atan2(dx, dy);
                cam.pitch = std::asin(std::clamp(dz / std::max(d, 1e-6f), -1.0f, 1.0f));
                cam.fov = sh.value("fovy", cam.fov);
                turntable = false;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put the 3D view on the camera of this vanilla still");
            ImGui::SameLine();
            bool hasCards = false;
            for (auto& m : pack.meshes) if (m.kind == "hair_cards") hasCards = true;
            if (hasCards) {
                if (ImGui::Checkbox("Vanilla hair in 3D##vref", &vanillaHair)) showHair = !vanillaHair;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the game's own hair cards instead of the HairWorks groom (same camera, same light)");
            }
            auto it = vanillaTex.find(vanillaShot);
            if (it == vanillaTex.end()) {
                RefTex rt;
                rt.srv = gfx.loadTexture(set.repo + "\\" + sh["ref"].get<std::string>(), false, &rt.w, &rt.h);
                it = vanillaTex.emplace(vanillaShot, rt).first;
            }
            if (it->second.srv && it->second.w > 0) {
                ImVec2 av = ImGui::GetContentRegionAvail();
                float sc = std::min(av.x / it->second.w, av.y / it->second.h);
                ImGui::Image((ImTextureID)it->second.srv, ImVec2(it->second.w * sc, it->second.h * sc));
            } else {
                ImGui::TextDisabled("Still not found: %s", sh["ref"].get<std::string>().c_str());
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}
