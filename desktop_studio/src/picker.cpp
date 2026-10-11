// Desktop Groom Studio - start screen / character picker (portrait cards, filters, details drawer). Our own code (MIT).
// Data: build\desktop_studio\catalog.json written by `desk_prep.py --catalog` from APPROVED.json, the skill's
// characters.md (status, pick, issues), lab\state\<char>.json (round-loop best, rounds), the built furs and the newest
// in-game PAIR crops / gallery images (thumbnails under build\desktop_studio\thumbs). Refreshed in the background.
// Portraits: painted (build\desktop_studio\portraits_ai\<char>.jpg, tools/make_portraits.py; default, per card it falls
// back to ours when there is no painting yet), ours, vanilla. Design: design/ui_direction1 (IMPLEMENTATION.md 5.6).
// Roster (2026-10-11, tools/hairworks/roster.json): status / hair type / game part filters; NOT STARTED characters have
// no build and are view-only (their own "Not started yet" group; the drawer explains why there is nothing to open).
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_internal.h"
#include "app.h"
#include "stylesw.h"
#include "ui_widgets.h"

namespace fs = std::filesystem;
using w::S;
using w::S2;

static void ptip(const char* t) { w::tip(t); }

static bool hasIssue(const json& c) {
    std::string is = c.value("issues", std::string());
    std::string low = is;
    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
    return !is.empty() && low.rfind("none", 0) != 0 && low != "-" && low != "ok";
}

// roster facets (tools/hairworks/roster.json via desk_prep.py --catalog): hair type and game part.
// Hair type index: 1 cards, 2 tied cards, 3 native HairWorks (Geralt's barber styles), 4 helmet / hood, 5 beard only
// (beard_mesh / scalp_beard), 6 bald. Characters outside the roster (generic haircuts) have no hair_type: card haircuts.
// A hairstyle may carry its own hair_type (catalog hairstyles[i].hair_type, e.g. Geralt's Wild One = cards); the card
// shows the default style's type and the hair-type filter matches a character when any of its styles has that type.
static std::string jstr(const json& c, const char* k) { return c.contains(k) && c[k].is_string() ? c[k].get<std::string>() : std::string(); }
int hairTypeIdx(const std::string& h) {
    if (h == "tied_cards") return 2;
    if (h == "native_hairworks") return 3;
    if (h == "helmet") return 4;
    if (h == "beard_mesh" || h == "scalp_beard") return 5;
    if (h == "bald") return 6;
    return 1;
}
const char* hairTypeName(int k) {
    static const char* n[] = {"", "Card hair", "Tied cards (bun / braid)", "Native HairWorks", "Helmet or hood", "Beard only", "Bald"};
    return n[std::clamp(k, 0, 6)];
}
int styleHairIdx(const json& c, int i) {
    if (c.contains("hairstyles") && c["hairstyles"].is_array() && i >= 0 && i < (int)c["hairstyles"].size()) {
        std::string h = jstr(c["hairstyles"][i], "hair_type");
        if (!h.empty()) return hairTypeIdx(h);
    }
    return hairTypeIdx(jstr(c, "hair_type"));
}
// the hair-type chip (drawer hero, inspector card): native HairWorks gets its own muted steel chip; returns the width
float hairTypeChip(ImDrawList* d, ImVec2 p, int k, const std::string& extra) {
    std::string ht = w::upper(hairTypeName(k)) + extra;
    ImVec2 ts = w::textSize(g_fontBadge, ht.c_str());
    ImVec2 t0 = p, t1(t0.x + ts.x + S(12), t0.y + S(18));
    bool native = k == 3;
    d->AddRectFilled(t0, t1, native ? tc(tok::STEEL, 0.10f) : IM_COL32(10, 11, 13, 160), S(3));
    d->AddRect(t0, t1, native ? tc(tok::STEEL, 0.38f) : tc(tok::IRON2), S(3));
    d->AddText(g_fontBadge, g_fontBadge->FontSize, ImVec2(t0.x + S(6), t0.y + (S(18) - ts.y) * 0.5f), native ? tc(tok::STEEL, 0.85f) : tc(tok::SOFT), ht.c_str());
    return t1.x - t0.x;
}
static int hairIdx(const json& c) { return styleHairIdx(c, 0); }   // the default hairstyle's type (what the card shows)
static bool hairAny(const json& c, int k) {                         // any hairstyle of that type (hair-type filter)
    int n = c.contains("hairstyles") && c["hairstyles"].is_array() ? (int)c["hairstyles"].size() : 0;
    if (n == 0) return hairIdx(c) == k;
    for (int i = 0; i < n; i++) if (styleHairIdx(c, i) == k) return true;
    return false;
}
static bool noHair(const json& c) { return hairIdx(c) >= 4; }
static const char* hairName(int k) { return hairTypeName(k); }
// game part index: 1 base, 2 Hearts of Stone, 3 Blood and Wine, 0 not in the roster
static int partIdx(const json& c) {
    std::string p = jstr(c, "part");
    return p == "base" ? 1 : p == "hos" ? 2 : p == "baw" ? 3 : 0;
}
static const char* partName(int k) {
    static const char* n[] = {"", "Base game", "Hearts of Stone", "Blood and Wine"};
    return n[std::clamp(k, 0, 3)];
}
static bool hasBuild(const json& c) { return c.contains("default") && c["default"].is_string(); }

// picker filters added with the roster (kept here: the picker owns them)
static int s_pickHair = 0, s_pickPart = 0;
static bool s_hideNoHair = true;

// rect with a vertical gradient that also fades alpha (ShadeVerts...KeepAlpha cannot)
static void fadeRect(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding = 0, ImDrawFlags fl = 0) {
    if (b.y - a.y < 0.5f) return;
    int v0 = d->VtxBuffer.Size;
    d->AddRectFilled(a, b, IM_COL32_WHITE, rounding, fl);
    ImVec4 f0 = ImGui::ColorConvertU32ToFloat4(c0), f1 = ImGui::ColorConvertU32ToFloat4(c1);
    for (int i = v0; i < d->VtxBuffer.Size; i++) {
        ImDrawVert& v = d->VtxBuffer[i];
        float t = std::clamp((v.pos.y - a.y) / (b.y - a.y), 0.0f, 1.0f);
        v.col = ImGui::ColorConvertFloat4ToU32(ImVec4(f0.x + (f1.x - f0.x) * t, f0.y + (f1.y - f0.y) * t, f0.z + (f1.z - f0.z) * t, f0.w + (f1.w - f0.w) * t));
    }
}

ID3D11ShaderResourceView* App::portrait(const json& c, int mode) {
    if (mode < 0) mode = pickPortrait;
    // a hairstyle view (styleView) caches and looks for its painting under <char>__<style>
    std::string ch = c.contains("portrait_key") && c["portrait_key"].is_string() ? c["portrait_key"].get<std::string>() : c.value("char", "");
    if (ch.empty()) return nullptr;
    auto load = [&](const std::string& key, const std::string& path) -> ID3D11ShaderResourceView* {
        auto it = thumbs.find(key);
        if (it != thumbs.end()) return it->second;
        std::error_code ec;
        ID3D11ShaderResourceView* s = (!path.empty() && fs::exists(path, ec)) ? gfx.loadTexture(path, false) : nullptr;
        thumbs[key] = s;
        return s;
    };
    auto rel = [&](const char* k) { return c.contains(k) && c[k].is_string() ? set.repo + "\\" + c[k].get<std::string>() : std::string(); };
    if (mode == 0) {
        // painted portraits are being generated while the app runs: a missing one is looked for again every 5 s
        static std::map<std::string, double> lastTry;
        std::string key = ch + "#p";
        auto it = thumbs.find(key);
        if (it != thumbs.end() && it->second) return it->second;
        double now = ImGui::GetTime();
        if (it == thumbs.end() || now - lastTry[ch] > 5.0) {
            lastTry[ch] = now;
            thumbs.erase(key);
            // the catalog's thumb_painted (desk_prep.py --catalog) when it has one; else the conventional
            // portraits_ai\<char>.jpg, for a painting made after the last catalog refresh
            std::string pp = rel("thumb_painted");
            if (pp.empty()) pp = set.repo + "\\build\\desktop_studio\\portraits_ai\\" + ch + ".jpg";
            if (auto* s = load(key, pp)) return s;
        }
    }
    if (mode == 2) {
        if (auto* s = load(ch + "#v", rel("thumb_vanilla"))) return s;
        return load(ch + "#o", rel("thumb_ours"));
    }
    if (auto* s = load(ch + "#o", rel("thumb_ours"))) return s;
    return load(ch + "#v", rel("thumb_vanilla"));
}

const json* App::catalogEntry(const std::string& cfg) const {
    if (cfg.empty() || !catalog.is_object() || !catalog.contains("characters")) return nullptr;
    for (auto& c : catalog["characters"]) {
        if (c["default"].is_string() && c["default"] == cfg) return &c;
        for (auto& v : c["variants"]) if (v.value("cfg", "") == cfg) return &c;
    }
    return nullptr;
}

// ---- hairstyles (desk_prep.py --catalog "hairstyles"; roster.py groups the appearances by hair component)
static int styleCount(const json& c) { return c.contains("hairstyles") && c["hairstyles"].is_array() ? (int)c["hairstyles"].size() : 0; }

int App::pickStyleIdx(const json& c) const {
    int n = styleCount(c);
    if (n <= 1) return 0;
    auto it = pickStyle.find(c.value("char", ""));
    if (it == pickStyle.end()) return 0;
    for (int i = 0; i < n; i++) if (c["hairstyles"][i].value("id", "") == it->second) return i;
    return 0;
}

json App::styleView(const json& c) const { return styleViewAt(c, pickStyleIdx(c)); }

json App::styleViewAt(const json& c, int i) const {
    json v = c;
    int n = styleCount(c);
    if (n <= 1 || i < 0 || i >= n) return v;
    const json& s = c["hairstyles"][i];
    std::string id = s.value("id", "default");
    v["style_id"] = id;
    v["style_name"] = s.value("name", id);
    if (s.contains("hair_type") && s["hair_type"].is_string()) v["hair_type"] = s["hair_type"];   // per-style hair type
    if (i == 0 && id == "default") return v;        // the character record already describes the default style
    for (const char* k : {"status", "default", "approved", "pick", "best", "last_round", "thumb_painted", "thumb_ours", "thumb_vanilla"})
        v[k] = s.contains(k) ? s[k] : json();
    v["variants"] = s.contains("variants") && s["variants"].is_array() ? s["variants"] : json::array();
    v["issues"] = "";                                // characters.md issues are about the default hairstyle
    v["portrait_key"] = s.contains("portrait_key") && s["portrait_key"].is_string() ? s["portrait_key"].get<std::string>() : c.value("char", "") + "__" + id;
    return v;
}

std::string App::pickStyleId() const {
    if (catalog.is_object() && catalog.contains("characters") && pickSel >= 0 && pickSel < (int)catalog["characters"].size()) {
        const json& c = catalog["characters"][pickSel];
        if (styleCount(c) > 0) return c["hairstyles"][pickStyleIdx(c)].value("id", "default");
        return "default";
    }
    return styleId;
}

std::string App::styleName(const std::string& cfg) const {
    const json* ce = catalogEntry(cfg);
    if (!ce || styleCount(*ce) <= 1) return styleId == "default" ? "" : styleId;
    for (auto& s : (*ce)["hairstyles"]) if (s.value("id", "") == styleId) return s.value("name", styleId);
    return styleId;
}

void App::loadCatalog() {
    std::string p = set.repo + "\\build\\desktop_studio\\catalog.json";
    try {
        std::ifstream f(p);
        if (f) catalog = json::parse(f);
    } catch (...) { catalog = json(); }
    // thumbnails may have changed: drop the cache
    for (auto& [k, v] : thumbs) if (v) v->Release();
    thumbs.clear();
    scanProjects();
}

void App::refreshCatalog() {
    startJob("Refreshing the character list", set.python + " \"" + set.repo + "\\desktop_studio\\tools\\desk_prep.py\" --catalog",
             [this](int rc) { if (rc == 0) loadCatalog(); });
}

void App::scanProjects() {
    projectsFound.clear();
    exportsFound.clear();
    std::string pd = set.repo + "\\build\\desktop_studio\\projects";
    std::error_code ec;
    if (fs::exists(pd, ec))
        for (auto& e : fs::directory_iterator(pd, ec))
            if (e.path().string().size() > 15 && e.path().string().find(".groomproj.json") != std::string::npos)
                projectsFound.push_back(e.path().string());
    std::string bd = set.repo + "\\build\\desktop_studio";
    if (fs::exists(bd, ec))
        for (auto& e : fs::directory_iterator(bd, ec))
            if (e.is_directory() && fs::exists(e.path() / "export")) exportsFound.push_back((e.path() / "export").string());
}

static std::string ago(double t) {
    if (t <= 0) return "";
    time_t tt = (time_t)t;
    char b[32];
    tm lt;
    localtime_s(&lt, &tt);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &lt);
    return b;
}

static std::string shortDate(double t) {
    if (t <= 0) return "";
    time_t tt = (time_t)t;
    char b[16];
    tm lt;
    localtime_s(&lt, &tt);
    strftime(b, sizeof b, "%m-%d", &lt);
    return b;
}

static void groupHeader(const char* name, int count) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;
    ImVec2 ns = w::textSize(g_fontGroup, name);
    std::string cs = std::to_string(count);
    ImVec2 cz = w::textSize(g_fontSmall, cs.c_str());
    float cy = p.y + S(10);
    d->AddText(g_fontGroup, g_fontGroup->FontSize, ImVec2(p.x, cy - ns.y * 0.5f), tc(tok::SOFT), name);
    d->AddLine(ImVec2(p.x + ns.x + S(10), cy), ImVec2(p.x + W - cz.x - S(10), cy), tc(tok::IRON), 1);
    d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(p.x + W - cz.x, cy - cz.y * 0.5f), tc(tok::DIM), cs.c_str());
    ImGui::Dummy(ImVec2(W, S(20) - S(8)));   // + the cards' 18 px item spacing
}

// one portrait card; returns true on double-click / quick "Open"
static bool card(App& a, const json& c, int i, float cw, float ph, float mh) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImGui::PushID(i);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();
    bool clicked = ImGui::InvisibleButton("card", ImVec2(cw, ph + mh));
    bool dbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + cw, p0.y + ph + mh));
    std::string st = c.value("status", ""), name = c.value("name", "");
    bool built = hasBuild(c);
    std::string def = built ? c["default"].get<std::string>() : "No HairWorks build yet";
    if (ImGui::IsItemHovered()) {
        std::string t = built ? name + "  [" + st + "]\nOpens: " + def + "\n" + c.value("issues", "") + "\n(click: details and variants, double-click: open)"
                              : name + "  [" + st + "]\nNo HairWorks build yet (" + hairName(hairIdx(c)) + "): view only.\n" + c.value("issues", "") +
                                    "\n(click or double-click: details)";
        if (styleCount(c) > 1) {
            t += "\n" + std::to_string(styleCount(c)) + " hairstyles:";
            for (auto& s : c["hairstyles"]) t += " " + s.value("name", s.value("id", "")) + (s.value("status", "") == "NOT STARTED" ? " (not started)" : "") + ",";
            t.pop_back();
            t += "\nThe card shows the default hairstyle; pick another in the details.";
        }
        ptip(t.c_str());
    }
    ImGuiID aid = ImGui::GetID("lift");
    float t = w::anim(aid, hov ? 1.0f : 0.0f);
    ImVec2 p(p0.x, p0.y - S(3) * t);
    ImVec2 q(p.x + cw, p.y + ph + mh);
    bool selC = a.pickSel == i;
    if (t > 0.01f) w::drawShadow(d, p, q, S(8), t);
    if (selC) for (int k = 1; k <= 4; k++) d->AddRect(ImVec2(p.x - k, p.y - k), ImVec2(q.x + k, q.y + k), tc(tok::GLOW, 0.2f * (1.0f - (k - 1) / 4.0f)), S(8) + k, 0, 1.0f);
    d->AddRectFilled(p, q, tc(tok::BG1), S(8));
    // image (3:4 portrait, cropped to the card, zoomed 2 % on hover)
    ImVec2 i0(p.x + 1, p.y + 1), i1(q.x - 1, p.y + ph);
    ID3D11ShaderResourceView* srv = a.portrait(c);
    if (srv) {
        float A = (i1.x - i0.x) / (i1.y - i0.y), span = std::min(0.75f / A, 1.0f), v0 = 0.3f * (1 - span);
        float z = 0.02f * t;
        ImVec2 uv0(z, v0 + span * z), uv1(1 - z, v0 + span * (1 - z));
        d->AddImageRounded((ImTextureID)srv, i0, i1, uv0, uv1, IM_COL32_WHITE, S(7), ImDrawFlags_RoundCornersTop);
        fadeRect(d, i0, ImVec2(i1.x, i0.y + ph * 0.22f), IM_COL32(0, 0, 0, 89), IM_COL32(0, 0, 0, 0), S(7), ImDrawFlags_RoundCornersTop);
        fadeRect(d, ImVec2(i0.x, i1.y - ph * 0.42f), i1, IM_COL32(9, 10, 12, 0), IM_COL32(9, 10, 12, 235));
    } else {
        d->AddRectFilled(i0, i1, tc(0x141619), S(7), ImDrawFlags_RoundCornersTop);
        ImVec2 cc((i0.x + i1.x) * 0.5f, i0.y + ph * 0.4f);
        for (int k = 8; k >= 1; k--) d->AddCircleFilled(cc, (i1.x - i0.x) * 0.62f * k / 8, tc(0x1d2125, 0.16f), 40);
        w::icon(d, ICON_LC_USER, ImVec2(cc.x, cc.y - S(8)), S(56), tc(0x3a4047));
        ImVec2 ns = w::textSize(g_fontSmall, "No portrait yet");
        d->AddText(g_fontSmall, S(11), ImVec2(cc.x - ns.x * 0.48f, cc.y + S(30)), tc(tok::DIM), "No portrait yet");
        fadeRect(d, ImVec2(i0.x, i1.y - ph * 0.4f), i1, IM_COL32(9, 10, 12, 0), IM_COL32(9, 10, 12, 178));
    }
    // badge, issue / selected mark, name
    w::badge(d, ImVec2(i0.x + S(8), i0.y + S(8)), st, true);
    ImVec2 mc(i1.x - S(8) - S(11), i0.y + S(8) + S(11));
    if (selC) {
        d->AddCircleFilled(mc, S(11), tc(tok::GLOW), 20);
        w::icon(d, ICON_LC_CHECK, mc, S(13), IM_COL32(27, 16, 6, 255));
    } else if (hasIssue(c)) {
        d->AddCircleFilled(mc, S(11), IM_COL32(10, 11, 13, 166), 20);
        w::icon(d, ICON_LC_TRIANGLE_ALERT, mc, S(12), tc(tok::SAND));
    }
    std::string nm = w::upper(name);
    ImVec2 ns = w::textSize(g_fontName15, nm.c_str());
    ImVec2 np(p.x + S(12), p.y + ph - S(9) - ns.y);
    d->AddText(g_fontName15, g_fontName15->FontSize, ImVec2(np.x + 1, np.y + 1.5f), IM_COL32(0, 0, 0, 170), nm.c_str());
    d->AddText(g_fontName15, g_fontName15->FontSize, np, tc(0xf3f1ee), nm.c_str());
    // meta: build (ellipsis) + last round
    ImGui::PushFont(g_fontSmall);
    float my = p.y + ph + S(8);
    const bool nativeNb = !built && hairIdx(c) == 3;   // native HairWorks, no build: its muted steel chip on the first line
    if (nativeNb) {
        const char* nt = "NATIVE HAIRWORKS";
        ImVec2 ts = w::textSize(g_fontBadge, nt);
        float cy = my + ImGui::GetFontSize() * 0.5f + S(1), x0 = p.x + S(12);
        ImVec2 c0(x0, cy - S(8)), c1(x0 + ts.x + S(10), cy + S(8));
        d->AddRectFilled(c0, c1, tc(tok::STEEL, 0.10f), S(3));
        d->AddRect(c0, c1, tc(tok::STEEL, 0.38f), S(3));
        d->AddText(g_fontBadge, g_fontBadge->FontSize, ImVec2(c0.x + S(5), cy - ts.y * 0.5f), tc(tok::STEEL, 0.85f), nt);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, built ? tv(tok::SOFT) : tv(tok::MUTED));
        ImGui::RenderTextEllipsis(d, ImVec2(p.x + S(12), my), ImVec2(q.x - S(10), my + ImGui::GetFontSize() + 2), q.x - S(10), q.x - S(10), def.c_str(), nullptr, nullptr);
        ImGui::PopStyleColor();
    }
    // not started: the hair type (and the game part) instead of the last round
    std::string lr = nativeNb ? std::string("No build yet") + (partIdx(c) > 1 ? std::string(" \xC2\xB7 ") + partName(partIdx(c)) : std::string())
                   : !built ? std::string(hairName(hairIdx(c))) + (partIdx(c) > 1 ? std::string(" \xC2\xB7 ") + partName(partIdx(c)) : std::string())
                   : c["last_round"].is_string() ? (styleCount(c) > 1 ? "Last round " + c["last_round"].get<std::string>().substr(5, 5)    // MM-DD: room for the styles chip
                                                 : "Last round " + c["last_round"].get<std::string>().substr(0, 10)) : "No round yet";
    float ly = my + ImGui::GetFontSize() + S(5);
    w::icon(d, built ? ICON_LC_CLOCK : ICON_LC_LAYERS, ImVec2(p.x + S(12) + S(5.5f), ly + ImGui::GetFontSize() * 0.5f), S(11), tc(tok::DIM));
    // "N styles" chip (right end of the meta line) when the character has more than one hairstyle
    float chipW = 0;
    if (styleCount(c) > 1) {
        std::string cs = std::to_string(styleCount(c)) + " STYLES";
        ImVec2 ts = w::textSize(g_fontBadge, cs.c_str());
        chipW = ts.x + S(10);
        float cy = ly + ImGui::GetFontSize() * 0.5f;
        ImVec2 c0(q.x - S(10) - chipW, cy - S(8)), c1(q.x - S(10), cy + S(8));
        d->AddRectFilled(c0, c1, tc(tok::BG2), S(3));
        d->AddRect(c0, c1, tc(tok::IRON2), S(3));
        d->AddText(g_fontBadge, g_fontBadge->FontSize, ImVec2(c0.x + S(5), cy - ts.y * 0.5f), tc(tok::SOFT), cs.c_str());
        chipW += S(8);
    }
    d->PushClipRect(p, ImVec2(q.x - S(6) - chipW, q.y), true);
    d->AddText(ImVec2(p.x + S(12) + S(17), ly), tc(tok::DIM), lr.c_str());
    d->PopClipRect();
    ImGui::PopFont();
    // border
    ImU32 border = selC ? tc(tok::GLOW) : hov ? tc(tok::IRON3) : tc(tok::IRON);
    d->AddRect(p, q, border, S(8), 0, 1.0f);
    // quick open (hover)
    bool open = false;
    if (hov && c["default"].is_string()) {
        float bw = S(82), bh = S(30);
        ImGui::SetCursorScreenPos(ImVec2(p.x + (cw - bw) * 0.5f, p.y + S(116) * ph / S(232) - bh * 0.5f));
        if (w::primaryButton("Open##quick", "Open", ImVec2(bw, bh), !a.job.running, ICON_LC_ARROW_RIGHT)) open = true;
        ptip(("Open " + def).c_str());
    }
    if (clicked || (dbl && !built)) a.pickSel = i;   // no build: double-click shows the details (view only)
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(cw, ph + mh));
    ImGui::PopID();
    return (dbl || open) && built && !a.job.running;
}

static bool drawerRow(const char* id, const char* glyph, const std::string& text, const char* date, bool current, bool dim, const std::vector<std::pair<std::string, unsigned>>& tags, bool* dbl) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    float W = ImGui::GetContentRegionAvail().x, h = S(30);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool r = ImGui::InvisibleButton(id, ImVec2(W, h));
    bool hov = ImGui::IsItemHovered();
    if (dbl) *dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (current) d->AddRectFilled(p, ImVec2(p.x + W, p.y + h), tc(tok::BG3), S(6));
    else if (hov) d->AddRectFilled(p, ImVec2(p.x + W, p.y + h), tc(tok::BG2), S(6));
    float cy = p.y + h * 0.5f, x = p.x + S(8);
    w::icon(d, glyph, ImVec2(x + S(6), cy), S(13), tc(tok::DIM));
    x += S(12) + S(8);
    float rx = p.x + W - S(8);
    if (date && *date) {
        ImVec2 ds = w::textSize(g_fontSmall, date);
        rx -= ds.x;
        d->AddText(g_fontSmall, S(11), ImVec2(rx, cy - ds.y * 0.5f), tc(tok::DIM), date);
        rx -= S(10);
    }
    for (auto it = tags.rbegin(); it != tags.rend(); ++it) {
        ImVec2 ts = w::textSize(g_fontBadge, it->first.c_str());
        float tw = ts.x + S(10);
        rx -= tw;
        d->AddRectFilled(ImVec2(rx, cy - S(8)), ImVec2(rx + tw, cy + S(8)), tc(it->second, 0.12f), S(3));
        d->AddText(g_fontBadge, g_fontBadge->FontSize, ImVec2(rx + S(5), cy - ts.y * 0.5f), tc(it->second), it->first.c_str());
        rx -= S(6);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, current ? tv(0xffffff) : dim ? tv(tok::DIM) : tv(tok::SOFT));
    ImGui::RenderTextEllipsis(d, ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), ImVec2(rx - S(6), cy + ImGui::GetFontSize()), rx - S(6), rx - S(6), text.c_str(), nullptr, nullptr);
    ImGui::PopStyleColor();
    return r;
}

// drawer hairstyle selector: equal-width segmented tabs for 2-4 styles, a dropdown above 4, nothing for one style.
// Labels "<name>##style" (tabs) / "##style_combo" + "<name>##stylei" (dropdown) are findable by the UI tests.
static unsigned statusTok(const std::string& st) {
    return st == "APPROVED" ? tok::GLOW : st == "FINAL" ? tok::SILVER : st == "PICK" ? tok::SAND : st == "IN PROGRESS" ? tok::STEEL : st == "ON HOLD" ? tok::HOLD : 0;
}
static std::string styleTip(const json& s) {
    std::string st = s.value("status", ""), t = s.value("name", "") + "  [" + st + "]";
    if (s.contains("appearance_or_component") && s["appearance_or_component"].is_string()) t += "\nAppearance: " + s["appearance_or_component"].get<std::string>();
    if (s.contains("component") && s["component"].is_string()) t += "\nHair: " + s["component"].get<std::string>();
    if (s.contains("hair_type") && s["hair_type"].is_string()) t += std::string("\nHair type: ") + hairTypeName(hairTypeIdx(s["hair_type"].get<std::string>()));
    int nv = s.contains("variants") && s["variants"].is_array() ? (int)s["variants"].size() : 0;
    t += st == "NOT STARTED" ? "\nNo build of this hairstyle yet: view only. A config opts in with \"style\": \"" + s.value("id", "") + "\"."
                             : "\n" + std::to_string(nv) + (nv == 1 ? " variant" : " variants") + (s.contains("default") && s["default"].is_string() ? ", opens " + s["default"].get<std::string>() : std::string());
    return t;
}
static void styleSelector(App& a, const json& c, float W) {
    int n = styleCount(c);
    if (n <= 1) return;
    int cur = a.pickStyleIdx(c);
    std::string ch = c.value("char", "");
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    std::string hdr = "HAIRSTYLE \xC2\xB7 " + std::to_string(n);
    d->AddText(g_fontCap, g_fontCap->FontSize, ImVec2(p.x, p.y + S(4)), tc(tok::MUTED), hdr.c_str());
    float y = p.y + S(22), h = S(30);
    auto dot = [&](ImVec2 cc, const std::string& st) {
        unsigned t = statusTok(st);
        if (t) d->AddCircleFilled(cc, S(3), tc(t), 10);
        else d->AddCircle(cc, S(3), tc(tok::MUTED), 10, S(1.2f));   // not started: hollow dot (as the status chip)
    };
    if (n <= 4) {
        float pad = S(2), gap = S(2), segW = (W - pad * 2 - gap * (n - 1)) / n;
        d->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + W, y + h + pad * 2), tc(tok::BG0), S(7));
        d->AddRect(ImVec2(p.x, y), ImVec2(p.x + W, y + h + pad * 2), tc(tok::IRON), S(7));
        for (int i = 0; i < n; i++) {
            const json& s = c["hairstyles"][i];
            std::string nm = s.value("name", s.value("id", "")), st = s.value("status", "");
            ImVec2 a0(p.x + pad + i * (segW + gap), y + pad), a1(a0.x + segW, a0.y + h);
            ImGui::SetCursorScreenPos(a0);
            if (ImGui::InvisibleButton((nm + "##style").c_str(), ImVec2(segW, h))) a.pickStyle[ch] = s.value("id", "default");
            bool hov = ImGui::IsItemHovered(), on = i == cur;
            w::tip(styleTip(s).c_str());
            if (on) {
                d->AddRectFilled(a0, a1, tc(tok::BG3), S(5));
                d->AddRect(a0, a1, tc(tok::IRON2), S(5));
            } else if (hov) d->AddRectFilled(a0, a1, tc(tok::BG2), S(5));
            float cy = a0.y + h * 0.5f;
            dot(ImVec2(a0.x + S(12), cy), st);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? tv(tok::TEXT) : hov ? tv(tok::SOFT) : tv(tok::MUTED));
            ImGui::RenderTextEllipsis(d, ImVec2(a0.x + S(21), cy - ImGui::GetFontSize() * 0.5f), ImVec2(a1.x - S(6), cy + ImGui::GetFontSize()), a1.x - S(6), a1.x - S(6), nm.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, y + h + pad * 2 + S(8)));
    } else {
        const json& s0 = c["hairstyles"][cur];
        std::string prev = s0.value("name", s0.value("id", ""));
        // dropdown field drawn by hand (an InvisibleButton: findable as "##style_combo" by the UI tests) + a popup
        ImGui::SetCursorScreenPos(ImVec2(p.x, y));
        ImVec2 f0(p.x, y), f1(p.x + W, y + h + S(4));
        if (ImGui::InvisibleButton("##style_combo", ImVec2(W, h + S(4)))) ImGui::OpenPopup("##style_pop");
        bool hov = ImGui::IsItemHovered();
        bool open = ImGui::IsPopupOpen("##style_pop");
        if (!open) w::tip(styleTip(s0).c_str());
        d->AddRectFilled(f0, f1, hov || open ? tc(tok::BG2) : tc(tok::BG0), S(7));
        d->AddRect(f0, f1, open ? tc(tok::IRON3) : tc(tok::IRON2), S(7));
        float cy = (f0.y + f1.y) * 0.5f;
        dot(ImVec2(p.x + S(12), cy), s0.value("status", ""));
        d->AddText(ImVec2(p.x + S(24), cy - ImGui::GetFontSize() * 0.5f), tc(tok::TEXT), prev.c_str());
        w::icon(d, open ? ICON_LC_CHEVRON_UP : ICON_LC_CHEVRON_DOWN, ImVec2(f1.x - S(16), cy), S(14), tc(tok::SOFT));
        ImGui::SetNextWindowPos(ImVec2(p.x, f1.y + S(2)));
        ImGui::SetNextWindowSize(ImVec2(W, 0));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, tv(tok::BG2));
        ImGui::PushStyleColor(ImGuiCol_Border, tv(tok::IRON2));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(7));
        open = ImGui::BeginPopup("##style_pop");
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        if (open) {
            for (int i = 0; i < n; i++) {
                const json& s = c["hairstyles"][i];
                std::string nm = s.value("name", s.value("id", ""));
                if (ImGui::Selectable((nm + "##stylei").c_str(), i == cur)) a.pickStyle[ch] = s.value("id", "default");
                w::tip(styleTip(s).c_str());
                std::string st = s.value("status", "");   // status, right-aligned in the row
                ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                ImVec2 ts = w::textSize(g_fontBadge, st.c_str());
                ImDrawList* pd = ImGui::GetWindowDrawList();
                pd->AddText(g_fontBadge, g_fontBadge->FontSize, ImVec2(r1.x - ts.x - S(6), (r0.y + r1.y - ts.y) * 0.5f), statusTok(st) ? tc(statusTok(st)) : tc(tok::DIM), st.c_str());
            }
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, y + h + S(14)));
    }
}

static void capHeader(const char* t, const char* right = nullptr) {
    ImGui::Dummy(ImVec2(0, S(12)));
    ImGui::PushFont(g_fontCap);
    ImGui::TextColored(tv(tok::MUTED), "%s", t);
    ImGui::PopFont();
    (void)right;
}

// segmented control with a count after each label (w::segmented's look); labels stay findable ("Cards##ht")
static float segCountsWidth(const char* const* labels, const int* counts, int n) {
    float t = S(4) + S(2) * (n - 1);
    for (int i = 0; i < n; i++)
        t += ImGui::CalcTextSize(labels[i], ImGui::FindRenderedTextEnd(labels[i])).x + S(6) + w::textSize(g_fontSmall, std::to_string(counts[i]).c_str()).x + S(22);
    return t;
}
static bool segCounts(const char* id, const char* const* labels, const int* counts, int n, int* v, const char* const* tips) {
    ImGui::PushID(id);
    float h = S(28), pad = S(2), gap = S(2);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float total = segCountsWidth(labels, counts, n);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + total, p.y + h + pad * 2), tc(tok::BG0), S(7));
    d->AddRect(p, ImVec2(p.x + total, p.y + h + pad * 2), tc(tok::IRON), S(7));
    bool changed = false;
    float x = p.x + pad;
    for (int i = 0; i < n; i++) {
        const char* e = ImGui::FindRenderedTextEnd(labels[i]);
        ImVec2 ts = ImGui::CalcTextSize(labels[i], e);
        std::string cs = std::to_string(counts[i]);
        ImVec2 cz = w::textSize(g_fontSmall, cs.c_str());
        float wi = ts.x + S(6) + cz.x + S(22);
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + pad));
        if (ImGui::InvisibleButton(labels[i], ImVec2(wi, h))) { changed = true; *v = i; }
        bool hov = ImGui::IsItemHovered(), on = *v == i;
        if (tips && tips[i]) w::tip(tips[i]);
        ImVec2 a(x, p.y + pad), b(x + wi, p.y + pad + h);
        if (on) {
            d->AddRectFilled(a, b, tc(tok::BG3), S(5));
            d->AddRect(a, b, tc(tok::IRON2), S(5));
        }
        float cy = a.y + h * 0.5f;
        d->AddText(ImVec2(a.x + S(11), cy - ts.y * 0.5f), on ? tc(tok::TEXT) : hov ? tc(tok::SOFT) : tc(tok::MUTED), labels[i], e);
        d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(a.x + S(11) + ts.x + S(6), cy - cz.y * 0.5f), on ? tc(tok::GLOW) : tc(tok::DIM), cs.c_str());
        x += wi + gap;
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(total, h + pad * 2));
    ImGui::PopID();
    return changed;
}

// compact switch: label then a small track (w::switchRow's look, without the full-width row)
static float miniSwitchWidth(const char* label) { return ImGui::CalcTextSize(label, ImGui::FindRenderedTextEnd(label)).x + S(10) + S(30) + S(8); }
static bool miniSwitch(const char* label, bool* v, const char* tipText) {
    float W = miniSwitchWidth(label), h = S(32);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool r = ImGui::InvisibleButton(label, ImVec2(W, h));
    if (r) *v = !*v;
    bool hov = ImGui::IsItemHovered();
    w::tip(tipText);
    ImDrawList* d = ImGui::GetWindowDrawList();
    const char* e = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, e);
    float t = w::anim(ImGui::GetItemID(), *v ? 1.0f : 0.0f, 18.0f);
    ImVec2 a(p.x + S(4), p.y + (h - S(17)) * 0.5f), b(a.x + S(30), a.y + S(17));
    ImVec4 off = tv(tok::BG4), on = tv(tok::AMBER);
    d->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(ImVec4(off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t, 1)), S(8.5f));
    if (t < 0.5f) d->AddRect(a, b, tc(tok::IRON2), S(8.5f));
    ImVec4 k0 = tv(tok::MUTED), k1 = tv(0xfff6ea);
    d->AddCircleFilled(ImVec2(a.x + S(8.5f) + t * S(13), a.y + S(8.5f)), S(6.5f),
                       ImGui::ColorConvertFloat4ToU32(ImVec4(k0.x + (k1.x - k0.x) * t, k0.y + (k1.y - k0.y) * t, k0.z + (k1.z - k0.z) * t, 1)), 20);
    d->AddText(ImVec2(b.x + S(10), p.y + (h - ts.y) * 0.5f), hov ? tc(tok::TEXT) : tc(tok::SOFT), label, e);
    return r;
}

void App::drawPicker() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiIO& io = ImGui::GetIO();
    pollJob();
    if (!catalog.is_object() || !catalog.contains("characters")) {
        loadCatalog();
        if ((!catalog.is_object() || !catalog.contains("characters")) && !job.running) refreshCatalog();
    }
    const json& chars = catalog.contains("characters") ? catalog["characters"] : json::array();
    // the picker covers everything between the tool rail, the title bar and the status bar; the drawer is on the right
    ImVec2 pos(vp->Pos.x + g_railW, vp->Pos.y + g_titleH), size(vp->Size.x - g_railW, vp->Size.y - g_titleH - g_statusH);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tv(tok::BG0));
    ImGui::Begin("##picker", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    float drawerW = std::min(S(430), size.x * 0.4f);
    float mainW = size.x - drawerW;
    // Enter opens the selected character
    if (pickSel >= 0 && pickSel < (int)chars.size() && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter) && !job.running) {
        json sv = styleView(chars[pickSel]);         // the hairstyle selected in the drawer
        if (sv["default"].is_string()) {
            std::string def = sv["default"].get<std::string>();
            bool built = false;
            for (auto& v : sv["variants"]) if (v.value("cfg", "") == def) built = v.value("built", false);
            openAsync(def, !built);
        }
    }

    // ================================================================ main column
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(22)));
    ImGui::BeginChild("##pk_main", ImVec2(mainW, size.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* d = ImGui::GetWindowDrawList();
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 ts = w::textSize(g_fontDisplay, "CHOOSE A CHARACTER");
        d->AddText(g_fontDisplay, g_fontDisplay->FontSize, p, tc(tok::TEXT), "CHOOSE A CHARACTER");
        std::string sub = std::to_string(chars.size()) + " characters" + (catalog.contains("time") ? " \xC2\xB7 catalog " + catalog["time"].get<std::string>() : "");
        ImVec2 ss = ImGui::CalcTextSize(sub.c_str());
        d->AddText(ImVec2(p.x + ts.x + S(16), p.y + ts.y - ss.y - S(3)), tc(tok::MUTED), sub.c_str());
        if (loaded) {
            float bw = ImGui::CalcTextSize("Back to the editor").x + w::kbdWidth("Esc") + S(8) + S(23) + S(24);
            ImGui::SetCursorScreenPos(ImVec2(p.x + ImGui::GetContentRegionAvail().x - bw, p.y - S(2)));
            if (w::button("Back to the editor (Esc)", "Back to the editor", ImVec2(bw, S(30)), ICON_LC_ARROW_RIGHT, false, true, "Esc")) pickerOpen = false;
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ts.y + S(16)));
    }
    // ---- toolbar: search, status chips, gender, portraits, refresh
    static const char* stKey[] = {"", "APPROVED", "FINAL", "PICK", "IN PROGRESS", "ON HOLD", "NOT STARTED"};
    // every filter; skip = the facet whose own counts are being computed (1 status, 2 hair type + hide, 3 game part)
    auto pass = [&](const json& c, int skip) -> bool {
        if (skip != 1 && pickStatus > 0 && c.value("status", "") != stKey[pickStatus]) return false;
        bool fem = c.value("female", false);
        if ((pickGender == 1 && fem) || (pickGender == 2 && !fem)) return false;
        if (skip != 2) {
            if (s_pickHair > 0 && !hairAny(c, s_pickHair)) return false;   // any of its hairstyles
            if (s_pickHair == 0 && s_hideNoHair && noHair(c)) return false;   // an explicit no-hair type overrides the toggle
        }
        if (skip != 3 && s_pickPart > 0 && partIdx(c) != s_pickPart) return false;
        if (pickFilter[0]) {
            std::string q = pickFilter;
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            std::string hay = c.value("name", "") + " " + c.value("char", "");
            for (auto& v : c["variants"]) hay += " " + v.value("cfg", "");
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(q) == std::string::npos) return false;
        }
        return true;
    };
    {
        float W = ImGui::GetContentRegionAvail().x;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float rowH = S(34);
        // search
        float sw = W > S(1250) ? S(300) : S(250);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(32), (rowH - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(7));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, tv(tok::BG1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, tv(tok::BG1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, tv(tok::BG1));
        ImGui::PushStyleColor(ImGuiCol_Border, tv(tok::IRON2));
        ImGui::SetNextItemWidth(sw);
        ImGui::InputTextWithHint("##q", "Search a name or a build...", pickFilter, sizeof pickFilter);
        bool act = ImGui::IsItemActive();
        ptip("Filters the characters by name, and by any build name of theirs (e.g. l124).");
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
        if (act) {
            d->AddRect(ImVec2(p.x - S(3), p.y - S(3)), ImVec2(p.x + sw + S(3), p.y + rowH + S(3)), tc(tok::GLOW, 0.12f), S(9), 0, S(3));
            d->AddRect(p, ImVec2(p.x + sw, p.y + rowH), tc(tok::GLOW), S(7));
        }
        w::icon(d, ICON_LC_SEARCH, ImVec2(p.x + S(17), p.y + rowH * 0.5f), S(15), tc(tok::DIM));
        w::kbd(d, ImVec2(p.x + sw - S(10) - w::kbdWidth("Ctrl F"), p.y + rowH * 0.5f - S(8)), "Ctrl F");
        float x = p.x + sw + S(10);
        // status chips with counts
        int counts[7] = {0, 0, 0, 0, 0, 0, 0};
        for (auto& c : chars) {
            if (!pass(c, 1)) continue;
            counts[0]++;
            for (int k = 1; k < 7; k++) if (c.value("status", "") == stKey[k]) counts[k]++;
        }
        static const char* chipL[] = {"All##st", "Approved##st", "Final##st", "Pick##st", "In progress##st", "On hold##st", "Not started##st"};
        static const unsigned chipC[] = {tok::MUTED, tok::GLOW, tok::SILVER, tok::SAND, tok::STEEL, tok::HOLD, tok::DIM};
        for (int k = 0; k < 7; k++) {
            const char* e = ImGui::FindRenderedTextEnd(chipL[k]);
            ImVec2 ls = ImGui::CalcTextSize(chipL[k], e);
            std::string cs = std::to_string(counts[k]);
            ImVec2 cz = w::textSize(g_fontSmall, cs.c_str());
            float cwid = S(11) + (k ? S(13) : 0) + ls.x + S(7) + cz.x + S(11);
            ImVec2 cp(x, p.y + S(2));
            ImGui::SetCursorScreenPos(cp);
            if (ImGui::InvisibleButton(chipL[k], ImVec2(cwid, S(30)))) pickStatus = k;
            bool hov = ImGui::IsItemHovered(), on = pickStatus == k;
            ImVec2 cq(cp.x + cwid, cp.y + S(30));
            d->AddRectFilled(cp, cq, on ? tc(tok::BG3) : hov ? tc(tok::BG2) : tc(tok::BG1), S(15));
            d->AddRect(cp, cq, on ? tc(tok::IRON3) : tc(tok::IRON), S(15));
            float tx = cp.x + S(11), cy = cp.y + S(15);
            if (k == 6) { d->AddCircle(ImVec2(tx + S(3), cy), S(3), tc(tok::MUTED), 10, S(1.2f)); tx += S(13); }   // not started: hollow dot
            else if (k) { d->AddCircleFilled(ImVec2(tx + S(3), cy), S(3), tc(chipC[k]), 10); tx += S(13); }
            d->AddText(ImVec2(tx, cy - ls.y * 0.5f), on ? IM_COL32_WHITE : tc(tok::SOFT), chipL[k], e);
            tx += ls.x + S(7);
            d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(tx, cy - cz.y * 0.5f), on ? tc(tok::SOFT) : tc(tok::DIM), cs.c_str());
            x += cwid + S(6);
        }
        // right group: gender, portraits, refresh (wraps to a second row when the window is narrow)
        static const char* gl[] = {"All##g", "Male##g", "Female##g"};
        static const char* pl[] = {"Painted##pt", "Ours##pt", "Vanilla##pt"};
        static const char* ptips[] = {"Painted portraits (tools/make_portraits.py, build\\desktop_studio\\portraits_ai). A character without one shows ours.",
                                      "Our current best build in game (newest front capture of the character's builds, else the gallery's newest round).",
                                      "The vanilla character (HairLab baseline capture, else the gallery's vanilla column)."};
        auto segW = [&](const char* const* l, int n, int v) {
            float t = S(4) + S(2) * (n - 1);
            for (int k = 0; k < n; k++) t += ImGui::CalcTextSize(l[k], ImGui::FindRenderedTextEnd(l[k])).x + S(22) + (k == v ? S(9) : 0);
            return t;
        };
        float rw = segW(gl, 3, pickGender) + S(10) + segW(pl, 3, pickPortrait) + S(10) + S(34);
        float rx = p.x + W - rw, ry = p.y + S(1);
        bool wrap = rx < x + S(10);
        if (wrap) { rx = p.x; ry = p.y + rowH + S(10); }
        ImGui::SetCursorScreenPos(ImVec2(rx, ry));
        w::segmented("##gender", gl, 3, &pickGender);
        ImGui::SameLine(0, S(10));
        w::segmented("##portraits", pl, 3, &pickPortrait, ptips);
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ry));
        if (w::iconButton("Refresh", ICON_LC_REFRESH_CW, 34, "Refresh\nRe-reads APPROVED.json, characters.md, the round states and the newest captures (desk_prep.py --catalog).", !job.running)) refreshCatalog();
        // second row: hair type (with counts), the no-hair toggle, game part
        float y2 = (wrap ? ry + S(36) : p.y + rowH) + S(10);
        {
            static const char* hl[] = {"All##ht", "Cards##ht", "Tied##ht", "Native##ht", "Helmet-hood##ht", "Beard only##ht", "Bald##ht"};
            static const char* htips[] = {"All hair types\nWith \"Hide no-hair characters\" on, helmet / hood, beard-only and bald characters are left out.",
                                          "Cards\nLoose vanilla hair cards: the HairWorks candidates (generic haircuts count here).",
                                          "Tied\nTied cards (bun, braid, ponytail): these stay cards unless the user asks.",
                                          "Native HairWorks\nThe game already renders this hair with HairWorks (Geralt's barber hairstyles).",
                                          "Helmet-hood\nThe hair is hidden under a helmet or a hood.",
                                          "Beard only\nOnly a beard mesh over the scalp (beards stay vanilla).",
                                          "Bald\nNo scalp hair."};
            static const char* gpl[] = {"All##gp", "Base##gp", "Hearts of Stone##gp", "Blood and Wine##gp"};
            static const char* gptips[] = {"Every game part (generic haircuts are only listed here)", "Base game characters",
                                           "Hearts of Stone characters", "Blood and Wine characters"};
            int hc[7] = {0, 0, 0, 0, 0, 0, 0}, gc[4] = {0, 0, 0, 0}, hiddenNoHair = 0;
            for (auto& c : chars) {
                if (pass(c, 2)) {
                    hc[0]++;
                    for (int k = 1; k < 7; k++) if (hairAny(c, k)) hc[k]++;   // once per type among its hairstyles
                    if (noHair(c)) hiddenNoHair++;
                }
                if (pass(c, 3)) {
                    gc[0]++;
                    if (partIdx(c)) gc[partIdx(c)]++;
                }
            }
            const char* swl = "Hide no-hair characters";
            std::string swTip = std::string("Hide no-hair characters\nLeaves out the bald, helmet / hood and beard-only characters (") + std::to_string(hiddenNoHair) +
                                " now). Picking one of those hair types shows them anyway.";
            float x2 = p.x, maxX = p.x + W;
            ImGui::SetCursorScreenPos(ImVec2(x2, y2));
            segCounts("##hairtype", hl, hc, 7, &s_pickHair, htips);
            x2 += segCountsWidth(hl, hc, 7) + S(12);
            if (x2 + miniSwitchWidth(swl) > maxX) { x2 = p.x; y2 += S(42); }
            ImGui::SetCursorScreenPos(ImVec2(x2, y2));
            miniSwitch(swl, &s_hideNoHair, swTip.c_str());
            x2 += miniSwitchWidth(swl) + S(16);
            float gw = segCountsWidth(gpl, gc, 4);
            if (x2 + gw > maxX) { x2 = p.x; y2 += S(42); }
            else x2 = std::max(x2, maxX - gw);   // right-aligned under the portrait switch
            ImGui::SetCursorScreenPos(ImVec2(x2, y2));
            segCounts("##gamepart", gpl, gc, 4, &s_pickPart, gptips);
        }
        float y1 = y2 + S(32) + S(16);
        if (job.running) {
            std::string jl;
            { std::lock_guard<std::mutex> lk(job.m); jl = job.title + ": " + job.lastLine; }
            float tt = (float)ImGui::GetTime() * 6;
            d->PathArcTo(ImVec2(p.x + S(6), y1 - S(6)), S(5), tt, tt + 4.2f, 12);
            d->PathStroke(tc(tok::GLOW), 0, S(1.5f));
            d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(p.x + S(18), y1 - S(12)), tc(tok::GLOW), jl.c_str());
            y1 += S(10);
        }
        ImVec2 wp = ImGui::GetWindowPos();
        d->AddLine(ImVec2(p.x, y1), ImVec2(wp.x + mainW - S(28), y1), tc(tok::IRON), 1);
        ImGui::SetCursorScreenPos(ImVec2(wp.x, y1 + 1));
    }
    // ---- cards (scrolling)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(16), S(18)));
    ImGui::BeginChild("cards", ImVec2(mainW, 0), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
    {
        std::vector<int> named, generic, notStarted;
        for (int i = 0; i < (int)chars.size(); i++) {
            const json& c = chars[i];
            if (!pass(c, 0)) continue;
            if (c.value("generic", false)) generic.push_back(i);
            else if (c.value("status", "") == "NOT STARTED") notStarted.push_back(i);
            else named.push_back(i);
        }
        float cw = S(180), ph = S(232), mh = S(54), gx = S(16);
        float avail = ImGui::GetContentRegionAvail().x;
        int cols = std::max(1, int((avail + gx) / (cw + gx)));
        auto grid = [&](const std::vector<int>& ix) {
            for (size_t k = 0; k < ix.size(); k++) {
                if (k % cols) ImGui::SameLine(0, gx);
                if (card(*this, chars[ix[k]], ix[k], cw, ph, mh)) {
                    std::string def = chars[ix[k]]["default"].get<std::string>();
                    bool built = false;
                    for (auto& v : chars[ix[k]]["variants"]) if (v.value("cfg", "") == def) built = v.value("built", false);
                    openAsync(def, !built);
                }
            }
        };
        if (!named.empty()) { groupHeader("NAMED CHARACTERS", (int)named.size()); grid(named); }
        if (!generic.empty()) { if (!named.empty()) ImGui::Dummy(ImVec2(0, S(4))); groupHeader("GENERIC HAIRCUTS", (int)generic.size()); grid(generic); }
        if (!notStarted.empty()) {
            if (!named.empty() || !generic.empty()) ImGui::Dummy(ImVec2(0, S(4)));   // no gap when it is the only group
            groupHeader("NOT STARTED YET", (int)notStarted.size());
            grid(notStarted);
        }
        if (chars.empty()) ImGui::TextColored(tv(tok::MUTED), job.running ? "Reading the character list..." : "No character list yet: press Refresh.");
        else if (named.empty() && generic.empty() && notStarted.empty()) ImGui::TextColored(tv(tok::MUTED), "No character matches the filters.");
        ImGui::Dummy(ImVec2(0, S(10)));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::EndChild();

    // ================================================================ details drawer
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tv(tok::BG1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("detail", ImVec2(drawerW, size.y), ImGuiChildFlags_None, 0);
    ImGui::PopStyleVar();
    d = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    d->AddLine(ImVec2(wp.x + 0.5f, wp.y), ImVec2(wp.x + 0.5f, wp.y + size.y), tc(tok::IRON), 1);
    float padX = S(20);
    if (pickSel >= 0 && pickSel < (int)chars.size()) {
        const json& cc = chars[pickSel];          // the character (all hairstyles)
        json cv = styleView(cc);                   // ... with the hairstyle picked below (portrait, Open, variants, status)
        const json& c = cv;
        const bool otherStyle = c.contains("style_id") && c["style_id"] != "default";
        // hero
        float hh = S(268);
        ImVec2 h0 = ImGui::GetCursorScreenPos(), h1(h0.x + drawerW, h0.y + hh);
        ID3D11ShaderResourceView* srv = portrait(c);
        if (srv) {
            float A = drawerW / hh, span = std::min(0.75f / A, 1.0f), v0 = 0.52f * (1 - span) * 0.6f;
            d->AddImage((ImTextureID)srv, h0, h1, ImVec2(0, v0), ImVec2(1, v0 + span));
        } else {
            d->AddRectFilled(h0, h1, tc(0x16191c));
            w::icon(d, ICON_LC_USER, ImVec2((h0.x + h1.x) * 0.5f, h0.y + hh * 0.42f), S(80), tc(0x2a2f35));
        }
        fadeRect(d, h0, ImVec2(h1.x, h0.y + hh * 0.3f), tc(tok::BG1, 0.2f), tc(tok::BG1, 0.0f));
        fadeRect(d, ImVec2(h0.x, h0.y + hh * 0.3f), ImVec2(h1.x, h0.y + hh * 0.6f), tc(tok::BG1, 0.0f), tc(tok::BG1, 0.4f));
        fadeRect(d, ImVec2(h0.x, h0.y + hh * 0.6f), h1, tc(tok::BG1, 0.4f), tc(tok::BG1, 1.0f));
        std::string nm = w::upper(c.value("name", ""));
        ImVec2 ns = w::textSize(g_fontHero, nm.c_str());
        ImVec2 np(h0.x + padX, h1.y - S(14) - S(18) - S(8) - ns.y);
        d->AddText(g_fontHero, g_fontHero->FontSize, ImVec2(np.x + 1, np.y + 2), IM_COL32(0, 0, 0, 150), nm.c_str());
        d->AddText(g_fontHero, g_fontHero->FontSize, np, IM_COL32_WHITE, nm.c_str());
        float bwid = w::badge(d, ImVec2(np.x, np.y + ns.y + S(8)), c.value("status", ""), true);
        if (c.contains("hair_type") && c["hair_type"].is_string()) {   // roster characters: hair type (and game part) tag
            // the selected hairstyle's type (styleView: per-style hair_type, else the character's)
            int hk = hairTypeIdx(c["hair_type"].get<std::string>());
            ImVec2 t0(np.x + bwid + S(6), np.y + ns.y + S(8));
            float tw = hairTypeChip(d, t0, hk, partIdx(c) > 1 ? std::string(" \xC2\xB7 ") + w::upper(partName(partIdx(c))) : std::string());
            // hoverable (tooltip) and findable by label for the UI tests: "<type>##hairtype_chip"
            ImVec2 keep = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(t0);
            ImGui::InvisibleButton((std::string(hairName(hk)) + "##hairtype_chip").c_str(), ImVec2(tw, S(18)));
            ptip((std::string(hairName(hk)) + (hk == 3 ? "\nThe game already renders this hairstyle with HairWorks (its own fur)." : hk == 1 ? "\nVanilla hair cards: a HairWorks candidate." : "") +
                  (styleCount(cc) > 1 ? "\nHair type of the selected hairstyle." : "")).c_str());
            ImGui::SetCursorScreenPos(keep);
        }
        // close
        ImGui::SetCursorScreenPos(ImVec2(h1.x - S(12) - S(28), h0.y + S(12)));
        if (ImGui::InvisibleButton("##drawer_close", S2(28, 28))) pickSel = -1;
        {
            bool hv = ImGui::IsItemHovered();
            ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
            d->AddRectFilled(a0, a1, hv ? tc(tok::BG3, 0.9f) : IM_COL32(10, 11, 13, 170), S(6));
            d->AddRect(a0, a1, tc(tok::IRON2), S(6));
            w::icon(d, ICON_LC_X, ImVec2((a0.x + a1.x) * 0.5f, (a0.y + a1.y) * 0.5f), S(15), hv ? tc(tok::TEXT) : tc(tok::SOFT));
            ptip("Close the details");
        }
        ImGui::SetCursorScreenPos(ImVec2(h0.x, h1.y + S(4)));
        if (pickSel < 0) { ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar(2); return; }
        // body
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(8)));
        ImGui::BeginChild("##drawer_body", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
        d = ImGui::GetWindowDrawList();
        float W = ImGui::GetContentRegionAvail().x;
        styleSelector(*this, cc, W);               // hidden for a single hairstyle
        if (hasIssue(c)) {
            std::string is = c.value("issues", "");
            ImGui::PushFont(g_fontUi);
            float tw = W - S(12) - S(16) - S(10) - S(12);
            ImVec2 isz = ImGui::CalcTextSize(is.c_str(), nullptr, false, tw);
            ImGui::PopFont();
            float chh = S(10) + S(14) + S(2) + isz.y + S(10);
            ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + W, p.y + chh);
            d->AddRectFilled(p, q, tc(tok::SAND, 0.07f), S(7));
            d->AddRect(p, q, tc(tok::SAND, 0.25f), S(7));
            w::icon(d, ICON_LC_TRIANGLE_ALERT, ImVec2(p.x + S(12) + S(8), p.y + S(18)), S(15), tc(tok::SAND));
            float tx = p.x + S(12) + S(16) + S(10);
            d->AddText(g_fontCap, g_fontCap->FontSize, ImVec2(tx, p.y + S(10)), tc(tok::TEXT), "OPEN ISSUE");
            ImGui::SetCursorScreenPos(ImVec2(tx, p.y + S(10) + S(16)));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
            ImGui::PushStyleColor(ImGuiCol_Text, tv(tok::SOFT));
            ImGui::TextUnformatted(is.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + S(6)));
        }
        std::string def = c["default"].is_string() ? c["default"].get<std::string>() : "";
        std::string ap = c["approved"].is_string() ? c["approved"].get<std::string>() : "";
        bool defBuilt = false;
        for (auto& v : c["variants"]) if (v.value("cfg", "") == def) defBuilt = v.value("built", false);
        ImGui::Dummy(ImVec2(0, S(2)));
        int nv = (int)c["variants"].size();
        if (!hasBuild(c)) {
            // ---- view only: no HairWorks build yet
            int hi = hairTypeIdx(jstr(c, "hair_type")), pi = partIdx(c);   // the selected hairstyle's type (styleView)
            std::string l1 = std::string("Hair type: ") + hairName(hi) + (pi ? std::string(" \xC2\xB7 ") + partName(pi) : std::string());
            std::string l2s;
            if (otherStyle) {                      // a hairstyle not started yet (the character has builds of another one)
                l1 = "Hairstyle: " + c.value("style_name", std::string()) + (cc["hairstyles"][pickStyleIdx(cc)].contains("appearance_or_component") &&
                     cc["hairstyles"][pickStyleIdx(cc)]["appearance_or_component"].is_string() ? " \xC2\xB7 " + cc["hairstyles"][pickStyleIdx(cc)]["appearance_or_component"].get<std::string>() : std::string());
                l2s = std::string("No build of this hairstyle yet: the game shows its ") + (hi == 3 ? "native HairWorks fur" : "vanilla cards") + ". A config opts in with \"style\": \"" + c.value("style_id", std::string()) + "\".";
            }
            const char* l2 = !l2s.empty() ? l2s.c_str() : hi == 1 ? "The game shows the vanilla hair cards until a build exists."
                           : hi == 2 ? "Tied cards (bun / braid) stay cards unless the user asks for strands."
                           : hi == 3 ? "The game already renders this hair with HairWorks (its own fur): no HairWorks build of ours yet."
                           : hi == 4 ? "The hair is hidden under a helmet or a hood: nothing to convert."
                           : hi == 5 ? "Only a beard mesh over the scalp: beards stay vanilla."
                                     : "Bald: no scalp hair to convert.";
            ImGui::PushFont(g_fontUi);
            float tw = W - S(12) - S(16) - S(10) - S(12);
            float h2 = ImGui::CalcTextSize(l2, nullptr, false, tw).y;
            ImGui::PopFont();
            float chh = S(10) + S(16) + S(18) + S(2) + h2 + S(12);
            ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + W, p.y + chh);
            ImGui::InvisibleButton("No HairWorks build yet", ImVec2(W, chh));   // findable by label (UI tests)
            w::tip(otherStyle ? "No HairWorks build yet\nThis hairstyle (roster.json hairstyles) has no config yet: the picker shows it for reference only."
                              : "No HairWorks build yet\nThis character is in the roster (tools/hairworks/roster.json) but has no HairWorks config built yet: the picker shows it for reference only.");
            d->AddRectFilled(p, q, tc(tok::BG2), S(7));
            d->AddRect(p, q, tc(tok::IRON2), S(7));
            w::icon(d, ICON_LC_INFO, ImVec2(p.x + S(12) + S(8), p.y + S(18)), S(15), tc(tok::MUTED));
            float tx = p.x + S(12) + S(16) + S(10);
            d->AddText(g_fontCap, g_fontCap->FontSize, ImVec2(tx, p.y + S(10)), tc(tok::TEXT), "NO HAIRWORKS BUILD YET");
            d->AddText(g_fontUiSemi, g_fontUiSemi->FontSize, ImVec2(tx, p.y + S(10) + S(16)), tc(tok::SOFT), l1.c_str());
            ImGui::SetCursorScreenPos(ImVec2(tx, p.y + S(10) + S(16) + S(18) + S(2)));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
            ImGui::PushStyleColor(ImGuiCol_Text, tv(tok::MUTED));
            ImGui::TextUnformatted(l2);
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + S(8)));
            w::primaryButton("Open##drawer", "Open", ImVec2(W, S(44)), false, ICON_LC_ARROW_RIGHT, true, nullptr, "no build yet");
            ptip("Open (Enter)\nNo HairWorks build yet: there is nothing to open for this character.");
            if (hi == 1) {   // card hair only (native HairWorks / tied / no hair: no plain conversion)
                // a plain conversion needs the character's base config tools/hairworks/chars/<char>.json (pipeline defaults,
                // vanilla cards converted to strands); roster.py only writes the vanilla portrait baseline (<char>_baseline).
                // Another hairstyle's base config is <char>_<style>.json
                std::string plain = c.value("char", "") + (otherStyle ? "_" + c.value("style_id", std::string()) : std::string());
                bool plainExists = fs::exists(set.repo + "\\tools\\hairworks\\chars\\" + plain + ".json");
                bool plainBuilt = false;
                for (auto& v : c["variants"]) if (v.value("cfg", "") == plain) plainBuilt = v.value("built", false);
                if (w::button("Start a plain conversion", plainExists ? "Start a plain conversion" : "Start a plain conversion  \xC2\xB7  coming soon", ImVec2(W, S(34)),
                              ICON_LC_LAYERS, false, !job.running && plainExists))
                    openAsync(plain, !plainBuilt);
                ptip(plainExists ? ("Start a plain conversion\nBuilds the base config " + plain + " with the pipeline (vanilla cards converted to strands, 'start plain' rule, nothing installed), then opens it.").c_str()
                                 : ("Start a plain conversion: coming soon\nThe pipeline has no base config for this character yet (tools/hairworks/chars/" + plain +
                                    ".json). The roster only has the vanilla portrait baseline; creating the plain config from the Studio is not supported yet.").c_str());
            }
        } else {
        if (w::primaryButton("Open##drawer", "Open", ImVec2(W, S(44)), !job.running && !def.empty(), ICON_LC_ARROW_RIGHT, true, "Enter", def.c_str())) openAsync(def, !defBuilt);
        ptip(defBuilt ? "Open the best build\nThe approved build when there is one, else the round loop's current best, else the characters.md pick."
                      : "Build and open\nThis build has no fur yet: it is built with the pipeline first (hairpipe, nothing installed), then opened.");
        float bw = (W - S(8)) * 0.5f;
        if (w::button("Start from approved", "Start from approved", ImVec2(bw, S(34)), ICON_LC_CIRCLE_CHECK, false, !job.running && !ap.empty())) openAsync(ap, false);
        ptip(ap.empty() ? "No approved build for this character." : ("Opens " + ap + " (APPROVED.json). Edits never change the approved config.").c_str());
        ImGui::SameLine(0, S(8));
        std::string plain = c.value("char", "") + (otherStyle ? "_" + c.value("style_id", std::string()) : std::string());
        bool plainExists = fs::exists(set.repo + "\\tools\\hairworks\\chars\\" + plain + ".json");
        bool plainBuilt = false;
        for (auto& v : c["variants"]) if (v.value("cfg", "") == plain) plainBuilt = v.value("built", false);
        if (w::button("Plain conversion", "Plain conversion", ImVec2(bw, S(34)), ICON_LC_LAYERS, false, !job.running && plainExists)) openAsync(plain, !plainBuilt);
        ptip(plainExists ? ("Start from the plain conversion\nOpens the character's base config " + plain + " (pipeline defaults: vanilla cards converted to strands, 'start plain' rule). Built first if needed.").c_str()
                         : "This character has no base config.");
        }
        // variants (none for a character with no build: the section is left out)
        static bool builtOnly = false;
        static std::set<std::string> expanded;
        if (nv > 0 || hasBuild(c)) {
        {
            ImGui::Dummy(ImVec2(0, S(8)));
            ImVec2 p = ImGui::GetCursorScreenPos();
            std::string h = "VARIANTS \xC2\xB7 " + std::to_string(nv);
            d->AddText(g_fontCap, g_fontCap->FontSize, ImVec2(p.x, p.y + S(5)), tc(tok::MUTED), h.c_str());
            float bo = ImGui::CalcTextSize("Built only").x + S(22);
            ImGui::SetCursorScreenPos(ImVec2(p.x + W - bo, p.y));
            if (ImGui::InvisibleButton("Built only", ImVec2(bo, S(20)))) builtOnly = !builtOnly;
            bool hv = ImGui::IsItemHovered();
            ptip("Built only\nHide the variants that have no built fur yet.");
            w::icon(d, ICON_LC_FILTER, ImVec2(p.x + W - bo + S(7), p.y + S(10)), S(13), builtOnly ? tc(tok::GLOW) : tc(tok::MUTED));
            d->AddText(ImVec2(p.x + W - bo + S(20), p.y + S(10) - ImGui::GetFontSize() * 0.5f), builtOnly ? tc(tok::GLOW) : hv ? tc(tok::TEXT) : tc(tok::SOFT), "Built only");
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(26)));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), 0));
        const std::string expKey = c.value("char", "") + "#" + c.value("style_id", std::string());
        bool exp = expanded.count(expKey) > 0;
        int shownRows = 0, hidden = 0;
        for (auto& v : c["variants"]) {
            bool built = v.value("built", false);
            if (builtOnly && !built) continue;
            if (!exp && shownRows >= 7) { hidden++; continue; }
            shownRows++;
            std::string cfg = v.value("cfg", "");
            std::vector<std::pair<std::string, unsigned>> tags;
            if (v.value("approved", false)) tags.push_back({"APPROVED", tok::GLOW});
            if (v.value("pick", false)) tags.push_back({"PICK", tok::SAND});
            if (v.value("best", false)) tags.push_back({"BEST", tok::STEEL});
            bool cur = loaded ? cfg == configName : cfg == def;
            bool dbl = false;
            drawerRow(cfg.c_str(), ICON_LC_GIT_BRANCH, cfg, shortDate(v.value("time", 0.0)).c_str(), cur, !built, tags, &dbl);
            std::string t = cfg + "\n" + (built ? "built " + ago(v.value("time", 0.0)) : std::string("not built yet (double-click builds it with the pipeline)")) +
                            (v.value("prepped", false) ? "\nalready read by the Studio" : "") + "\ndouble-click to open";
            ptip(t.c_str());
            if (dbl && !job.running) openAsync(cfg, !built);
        }
        if (hidden > 0 || exp) {
            std::string t = exp ? "Show fewer variants" : "Show " + std::to_string(hidden) + " older variants";
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##morevariants", ImVec2(W, S(26)))) { if (exp) expanded.erase(expKey); else expanded.insert(expKey); }
            bool hv = ImGui::IsItemHovered();
            w::icon(d, exp ? ICON_LC_CHEVRON_UP : ICON_LC_CHEVRON_DOWN, ImVec2(p.x + S(14), p.y + S(13)), S(13), tc(tok::DIM));
            d->AddText(ImVec2(p.x + S(28), p.y + S(13) - ImGui::GetFontSize() * 0.5f), hv ? tc(tok::TEXT) : tc(tok::MUTED), t.c_str());
        }
        ImGui::PopStyleVar();
        }
        // recent / projects / exports
        capHeader("RECENT");
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), 0));
        int ri = 0;
        for (auto& r : set.recent) {
            bool proj = r.find(".groomproj.json") != std::string::npos;
            std::string lbl = proj ? fs::path(r).filename().string() + "  (project)" : r;
            ImGui::PushID(ri++);
            if (drawerRow(lbl.c_str(), proj ? ICON_LC_FOLDER : ICON_LC_HISTORY, lbl, "", false, false, {}, nullptr) && !job.running) {
                if (proj) loadProject(r); else openAsync(r, false);
            }
            ptip(r.c_str());
            ImGui::PopID();
        }
        if (set.recent.empty()) ImGui::TextColored(tv(tok::DIM), "nothing yet");
        if (!projectsFound.empty()) {
            ImGui::PopStyleVar();
            capHeader("SAVED PROJECTS");
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), 0));
            for (auto& r : projectsFound) {
                std::string n = fs::path(r).filename().string();
                if (drawerRow(n.c_str(), ICON_LC_FOLDER, n, "", false, false, {}, nullptr) && !job.running) loadProject(r);
                ptip(r.c_str());
            }
        }
        if (!exportsFound.empty()) {
            ImGui::PopStyleVar();
            capHeader("EXPORTS");
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), 0));
            for (auto& r : exportsFound) {
                std::string n = fs::path(r).parent_path().filename().string();
                if (drawerRow(n.c_str(), ICON_LC_FOLDER_OPEN, "Export \xC2\xB7 " + n, "", false, false, {}, nullptr)) ShellExecuteA(nullptr, "open", r.c_str(), nullptr, nullptr, SW_SHOW);
                ptip(("Open the export folder " + r).c_str());
            }
        }
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0, S(16)));
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    } else {
        // nothing selected: hint + recent
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, S(22)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), 0));
        ImGui::BeginChild("##drawer_body", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
        ImGui::PushFont(g_fontSection);
        ImGui::TextColored(tv(tok::TEXT), "DETAILS");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::PushTextWrapPos();
        ImGui::TextColored(tv(tok::MUTED), "Click a character to see its builds. Double-click a card (or hover it and press Open) to open its best build.");
        ImGui::PopTextWrapPos();
        capHeader("RECENT");
        int ri = 0;
        for (auto& r : set.recent) {
            bool proj = r.find(".groomproj.json") != std::string::npos;
            std::string lbl = proj ? fs::path(r).filename().string() + "  (project)" : r;
            ImGui::PushID(ri++);
            if (drawerRow(lbl.c_str(), proj ? ICON_LC_FOLDER : ICON_LC_HISTORY, lbl, "", false, false, {}, nullptr) && !job.running) {
                if (proj) loadProject(r); else openAsync(r, false);
            }
            ptip(r.c_str());
            ImGui::PopID();
        }
        if (set.recent.empty()) ImGui::TextColored(tv(tok::DIM), "nothing yet");
        if (!projectsFound.empty()) {
            capHeader("SAVED PROJECTS");
            for (auto& r : projectsFound) {
                std::string n = fs::path(r).filename().string();
                if (drawerRow(n.c_str(), ICON_LC_FOLDER, n, "", false, false, {}, nullptr) && !job.running) loadProject(r);
                ptip(r.c_str());
            }
        }
        if (!exportsFound.empty()) {
            capHeader("EXPORTS");
            for (auto& r : exportsFound) {
                std::string n = fs::path(r).parent_path().filename().string();
                if (drawerRow(n.c_str(), ICON_LC_FOLDER_OPEN, "Export \xC2\xB7 " + n, "", false, false, {}, nullptr)) ShellExecuteA(nullptr, "open", r.c_str(), nullptr, nullptr, SW_SHOW);
                ptip(("Open the export folder " + r).c_str());
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}
