// Desktop Groom Studio - hairstyle switcher in the editor (title-bar breadcrumb crumb, inspector character card,
// Ctrl+Shift+H). Our own code (MIT). Design: design/ui_direction1 (Ink & Iron popups, fields and chips).
// Picking a style with builds opens its best / approved build (the style's catalog "default": approved, else pick, else
// newest), asking Save / Discard / Cancel first when there are unsaved edits. A style with no build shows a small panel
// in the inspector card instead ("Start plain conversion" when the hair type supports it and the base config exists,
// else "Not started yet"); the loaded build is untouched, so the editor never ends up in a half-switched state.
#include <windows.h>

#include <algorithm>
#include <filesystem>

#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_internal.h"
#include "app.h"
#include "stylesw.h"
#include "ui_widgets.h"

namespace fs = std::filesystem;
using w::S;
using w::S2;

static const json* curChar(const App& a) { return a.loaded ? a.catalogEntry(a.configName) : nullptr; }
static int nStyles(const json* c) { return c && c->contains("hairstyles") && (*c)["hairstyles"].is_array() ? (int)(*c)["hairstyles"].size() : 0; }
static int styleIdxOf(const json* c, const std::string& id) {
    for (int i = 0; i < nStyles(c); i++) if ((*c)["hairstyles"][i].value("id", "") == id) return i;
    return -1;
}
static std::string sname(const json& s) { return s.value("name", s.value("id", std::string())); }
static std::string projPath(const App& a) {
    return a.projectPath.empty() ? a.set.repo + "\\build\\desktop_studio\\projects\\" + a.configName + ".groomproj.json" : a.projectPath;
}

std::string App::edStyleId() const {
    if (!edStyleNoBuild.empty() && edStyleNoBuildCfg == configName) return edStyleNoBuild;
    return styleId;
}

bool App::dirtyEdits() const { return loaded && int(groom.ops.size() + look.over.size()) != savedEdits; }

void App::requestOpen(const std::string& cfg, bool build) {
    if (cfg.empty() || job.running) return;
    if (dirtyEdits()) {
        switchCfg = cfg;
        switchBuild = build;
        switchConfirmReq = true;
        return;
    }
    edStyleNoBuild.clear();
    openAsync(cfg, build);
}

void App::pickEditorStyle(const std::string& id) {
    const json* ce = curChar(*this);
    int i = styleIdxOf(ce, id);
    if (i < 0) return;
    const json& s = (*ce)["hairstyles"][i];
    if (id == styleId) {   // the loaded build's own hairstyle: just close the not-started panel (no reload, edits kept)
        edStyleNoBuild.clear();
        return;
    }
    if (s.contains("default") && s["default"].is_string()) {
        std::string cfg = s["default"].get<std::string>();
        bool built = false;
        if (s.contains("variants")) for (auto& v : s["variants"]) if (v.value("cfg", "") == cfg) built = v.value("built", false);
        requestOpen(cfg, !built);
        return;
    }
    edStyleNoBuild = id;
    edStyleNoBuildCfg = configName;
    status = sname(s) + ": no HairWorks build of this hairstyle yet (still editing " + configName + ")";
}

// ------------------------------------------------------------------------------------------------ popup + modal
void App::drawStyleMenu() {
    const json* ce = curChar(*this);
    int n = nStyles(ce);
    styleMenuOpen = false;
    if (styleMenuReq) {
        styleMenuReq = false;
        if (n > 1 && !pickerOpen) ImGui::OpenPopup("##edstyle_pop");
        else if (loaded && !pickerOpen) status = "This character has one hairstyle (Default)";
    }
    if (n > 1) {
        float W = std::max(styleMenuW, S(340));
        ImGuiViewport* vp = ImGui::GetMainViewport();
        float x = std::clamp(styleMenuX, vp->Pos.x + S(8), vp->Pos.x + vp->Size.x - W - S(8));
        ImGui::SetNextWindowPos(ImVec2(x, styleMenuY));
        ImGui::SetNextWindowSize(ImVec2(W, 0));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, tv(tok::TIPBG));
        ImGui::PushStyleColor(ImGuiCol_Border, tv(tok::IRON2));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(8));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, S2(6, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, S2(0, 2));
        bool open = ImGui::BeginPopup("##edstyle_pop");
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(2);
        styleMenuOpen = open;
        if (open) {
            ImDrawList* d = ImGui::GetWindowDrawList();
            float RW = ImGui::GetContentRegionAvail().x;
            {   // header: HAIRSTYLE · N + the shortcut
                ImVec2 p = ImGui::GetCursorScreenPos();
                std::string h = "HAIRSTYLE \xC2\xB7 " + std::to_string(n);
                d->AddText(g_fontCap, g_fontCap->FontSize, ImVec2(p.x + S(8), p.y + S(7)), tc(tok::MUTED), h.c_str());
                w::kbd(d, ImVec2(p.x + RW - S(8) - w::kbdWidth("Ctrl Shift H"), p.y + S(5)), "Ctrl Shift H");
                ImGui::Dummy(ImVec2(RW, S(26)));
            }
            std::string cur = edStyleId();
            for (int i = 0; i < n; i++) {
                const json& s = (*ce)["hairstyles"][i];
                std::string id = s.value("id", ""), nm = sname(s), st = s.value("status", "");
                bool hasB = s.contains("default") && s["default"].is_string();
                float rh = S(50);
                ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + RW, p.y + rh);
                bool clicked = ImGui::InvisibleButton((nm + "##edstyle").c_str(), ImVec2(RW, rh));
                bool hov = ImGui::IsItemHovered(), on = id == cur;
                {
                    std::string t = nm + "  [" + st + "]\n" +
                                    (hasB ? "Opens " + s["default"].get<std::string>() + (dirtyEdits() && id != styleId ? " (asks about your unsaved edits first)" : "")
                                          : std::string("No HairWorks build of this hairstyle yet: the editor keeps the loaded build."));
                    if (id == styleId) t += "\nThe loaded build's hairstyle.";
                    w::tip(t.c_str());
                }
                if (on) d->AddRectFilled(p, q, tc(tok::BG3), S(6));
                else if (hov) d->AddRectFilled(p, q, tc(tok::BG2), S(6));
                if (on) d->AddRectFilled(ImVec2(p.x, p.y + S(10)), ImVec2(p.x + S(2.5f), q.y - S(10)), tc(tok::GLOW), S(1));
                // portrait thumbnail (the style's painting / capture; the character's when the style has none)
                ImVec2 t0(p.x + S(10), p.y + S(6)), t1(t0.x + S(30), q.y - S(6));
                ID3D11ShaderResourceView* srv = portrait(styleViewAt(*ce, i));
                if (srv) d->AddImageRounded((ImTextureID)srv, t0, t1, ImVec2(0.14f, 0.05f), ImVec2(0.86f, 0.70f), IM_COL32_WHITE, S(4));
                else { d->AddRectFilled(t0, t1, tc(tok::BG3), S(4)); w::icon(d, ICON_LC_USER, ImVec2((t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f), S(16), tc(0x3a4047)); }
                d->AddRect(t0, t1, on ? tc(tok::IRON3) : tc(tok::IRON2), S(4));
                // name, then hair type + what it opens
                float bw = w::badgeWidth(st);
                float tx = t1.x + S(12), rx = q.x - S(10) - bw - S(8);
                ImGui::PushStyleColor(ImGuiCol_Text, on ? tv(tok::TEXT) : hov ? tv(tok::TEXT) : tv(tok::SOFT));
                ImGui::PushFont(g_fontUiSemi);
                ImGui::RenderTextEllipsis(d, ImVec2(tx, p.y + S(9)), ImVec2(rx, p.y + S(9) + ImGui::GetFontSize() + 2), rx, rx, nm.c_str(), nullptr, nullptr);
                ImGui::PopFont();
                ImGui::PopStyleColor();
                std::string sub = std::string(hairTypeName(styleHairIdx(*ce, i))) + " \xC2\xB7 " + (hasB ? s["default"].get<std::string>() : std::string("not started"));
                ImGui::PushFont(g_fontSmall);
                ImGui::PushStyleColor(ImGuiCol_Text, styleHairIdx(*ce, i) == 3 ? tv(tok::STEEL, 0.85f) : tv(tok::MUTED));
                ImGui::RenderTextEllipsis(d, ImVec2(tx, p.y + S(28)), ImVec2(rx, p.y + S(28) + ImGui::GetFontSize() + 2), rx, rx, sub.c_str(), nullptr, nullptr);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                w::badge(d, ImVec2(q.x - S(10) - bw, p.y + (rh - S(18)) * 0.5f), st);
                if (clicked) {
                    ImGui::CloseCurrentPopup();
                    pickEditorStyle(id);
                }
            }
            ImGui::EndPopup();
        }
    }
    // unsaved edits: Save / Discard / Cancel before leaving the loaded build
    if (switchConfirmReq) { ImGui::OpenPopup("Unsaved edits"); switchConfirmReq = false; }
    ImGui::SetNextWindowSize(ImVec2(S(440), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Unsaved edits", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
        int edits = int(groom.ops.size() + look.over.size());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(420));
        ImGui::TextColored(tv(tok::TEXT), "%d unsaved %s on %s.", edits, edits == 1 ? "edit" : "edits", configName.c_str());
        ImGui::TextColored(tv(tok::MUTED), "Opening %s replaces the loaded build. Save the edits as a project first?", switchCfg.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(6)));
        auto go = [&] {
            std::string cfg = switchCfg;
            bool b = switchBuild;
            switchCfg.clear();
            ImGui::CloseCurrentPopup();
            edStyleNoBuild.clear();
            openAsync(cfg, b);
        };
        if (w::primaryButton("Save##stylesw", "Save", ImVec2(S(120), S(32)), true, ICON_LC_SAVE)) { if (saveProject(projPath(*this))) go(); }
        w::tip(("Save\nSaves the edits as a project (" + projPath(*this) + "), then opens " + switchCfg + ".").c_str());
        ImGui::SameLine(0, S(8));
        if (w::button("Discard##stylesw", "Discard", ImVec2(S(120), S(32)))) go();
        w::tip("Discard\nOpens the other build; the unsaved edits are lost.");
        ImGui::SameLine(0, S(8));
        if (w::button("Cancel##stylesw", "Cancel", ImVec2(S(120), S(32)), nullptr, true) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            switchCfg.clear();
            ImGui::CloseCurrentPopup();
        }
        w::tip("Cancel\nStay on the loaded build with your edits.");
        ImGui::EndPopup();
    }
}

// ------------------------------------------------------------------------------------------------ inspector card row
float styleCardRow(App& a, ImVec2 p, float W) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    const json* ce = curChar(a);
    int n = nStyles(ce);
    std::string cur = a.edStyleId();
    int ci = std::max(0, styleIdxOf(ce, cur));
    const json* s = n > 0 ? &(*ce)["hairstyles"][ci] : nullptr;
    bool multi = n > 1;
    std::string nm = multi && s ? sname(*s) : "Default";
    std::string st = s ? s->value("status", "") : (ce ? ce->value("status", "") : "");
    int hk = ce ? styleHairIdx(*ce, ci) : 1;
    float fh = S(30);
    // caps label + field
    ImVec2 ls = w::textSize(g_fontCap, "HAIRSTYLE");
    d->AddText(g_fontCap, g_fontCap->FontSize, ImVec2(p.x, p.y + (fh - ls.y) * 0.5f), tc(tok::MUTED), "HAIRSTYLE");
    ImVec2 f0(p.x + S(86), p.y), f1(p.x + W, p.y + fh);
    ImGui::SetCursorScreenPos(f0);
    bool clicked = ImGui::InvisibleButton("##card_style", ImVec2(f1.x - f0.x, fh));
    bool hov = ImGui::IsItemHovered() && multi;
    w::tip(multi ? "Hairstyle (Ctrl+Shift+H)\nSwitch hairstyle: a style with builds opens its best / approved build; one not started shows what can be done here."
                 : "Hairstyle\nThis character has one hairstyle (Default).");
    if (clicked && multi) { a.styleMenuReq = true; a.styleMenuX = f0.x; a.styleMenuY = f1.y + S(4); a.styleMenuW = f1.x - f0.x; }
    d->AddRectFilled(f0, f1, hov ? tc(tok::BG2) : tc(tok::BG0), S(6));
    d->AddRect(f0, f1, hov ? tc(tok::IRON3) : tc(tok::IRON2, multi ? 1.0f : 0.6f), S(6));
    float cy = p.y + fh * 0.5f;
    unsigned sc = st == "APPROVED" ? tok::GLOW : st == "FINAL" ? tok::SILVER : st == "PICK" ? tok::SAND : st == "IN PROGRESS" ? tok::STEEL : st == "ON HOLD" ? tok::HOLD : 0;
    if (sc) d->AddCircleFilled(ImVec2(f0.x + S(12), cy), S(3), tc(sc, multi ? 1.0f : 0.5f), 10);
    else d->AddCircle(ImVec2(f0.x + S(12), cy), S(3), tc(tok::MUTED), 10, S(1.2f));
    // hair type chip on the right, before the chevron
    std::string ht = w::upper(hairTypeName(hk));
    float chipW = w::textSize(g_fontBadge, ht.c_str()).x + S(12);
    float cx = f1.x - S(28) - chipW;
    hairTypeChip(d, ImVec2(cx, cy - S(9)), hk);
    ImGui::PushStyleColor(ImGuiCol_Text, multi ? tv(tok::TEXT) : tv(tok::DIM));
    ImGui::RenderTextEllipsis(d, ImVec2(f0.x + S(22), cy - ImGui::GetFontSize() * 0.5f), ImVec2(cx - S(6), cy + ImGui::GetFontSize()), cx - S(6), cx - S(6), nm.c_str(), nullptr, nullptr);
    ImGui::PopStyleColor();
    w::icon(d, ICON_LC_CHEVRON_DOWN, ImVec2(f1.x - S(14), cy), S(14), multi ? tc(tok::SOFT) : tc(tok::DIM));
    float used = fh;
    // not-started panel: the editor keeps the loaded build
    if (multi && !a.edStyleNoBuild.empty() && a.edStyleNoBuildCfg == a.configName && s) {
        float y = p.y + fh + S(8);
        std::string ch = ce->value("char", "");
        std::string plain = ch + "_" + cur;
        bool cardHair = hk == 1;
        bool plainExists = cardHair && fs::exists(a.set.repo + "\\tools\\hairworks\\chars\\" + plain + ".json");
        std::string l1 = nm + " \xC2\xB7 " + hairTypeName(hk);
        std::string l2 = "No HairWorks build of this hairstyle yet. You are still editing " + a.configName + ".";
        ImGui::PushFont(g_fontUi);
        float tw = W - S(12) - S(16) - S(10) - S(12);
        float h2 = ImGui::CalcTextSize(l2.c_str(), nullptr, false, tw).y;
        ImGui::PopFont();
        float ph = S(10) + S(16) + S(18) + h2 + S(10) + S(30) + S(12);
        ImVec2 a0(p.x, y), a1(p.x + W, y + ph);
        d->AddRectFilled(a0, a1, tc(tok::BG2), S(7));
        d->AddRect(a0, a1, tc(tok::IRON2), S(7));
        w::icon(d, ICON_LC_INFO, ImVec2(a0.x + S(12) + S(8), a0.y + S(18)), S(15), tc(tok::MUTED));
        float tx = a0.x + S(12) + S(16) + S(10);
        d->AddText(g_fontCap, g_fontCap->FontSize, ImVec2(tx, a0.y + S(10)), tc(tok::TEXT), "HAIRSTYLE NOT STARTED");
        d->AddText(g_fontUiSemi, g_fontUiSemi->FontSize, ImVec2(tx, a0.y + S(10) + S(16)), tc(tok::SOFT), l1.c_str());
        ImGui::SetCursorScreenPos(ImVec2(tx, a0.y + S(10) + S(16) + S(18)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
        ImGui::PushStyleColor(ImGuiCol_Text, tv(tok::MUTED));
        ImGui::TextUnformatted(l2.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        float by = a1.y - S(12) - S(30), bw = (W - S(24) - S(8)) * 0.62f, kw = W - S(24) - S(8) - bw;
        ImGui::SetCursorScreenPos(ImVec2(a0.x + S(12), by));
        if (plainExists) {
            if (w::button("Start plain conversion##stylenb", "Start plain conversion", ImVec2(bw, S(30)), ICON_LC_LAYERS, false, !a.job.running)) a.requestOpen(plain, true);
            w::tip(("Start plain conversion\nBuilds the base config " + plain + " with the pipeline (vanilla cards converted to strands, 'start plain' rule, nothing installed), then opens it.").c_str());
        } else {
            w::button("Not started yet", "Not started yet", ImVec2(bw, S(30)), ICON_LC_CLOCK, false, false);
            w::tip(cardHair ? ("Not started yet\nNo base config for this hairstyle yet (tools/hairworks/chars/" + plain + ".json): a plain conversion cannot start from the Studio.").c_str()
                   : hk == 3 ? "Not started yet\nNative HairWorks: the game already renders this hairstyle with its own fur, so there is no card conversion to start."
                             : (std::string("Not started yet\n") + hairTypeName(hk) + ": no plain conversion for this hair type.").c_str());
        }
        ImGui::SetCursorScreenPos(ImVec2(a0.x + S(12) + bw + S(8), by));
        if (w::button("Keep editing##stylenb", "Keep editing", ImVec2(kw, S(30)), nullptr, true)) a.edStyleNoBuild.clear();
        w::tip(("Keep editing\nBack to the loaded build's hairstyle (" + a.configName + ").").c_str());
        used += S(8) + ph;
    }
    return used;
}
