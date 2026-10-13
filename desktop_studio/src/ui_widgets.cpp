// Desktop Groom Studio - Ink & Iron widgets (design/ui_direction1/IMPLEMENTATION.md section 4). Our own code (MIT).
#include "ui_widgets.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "../third_party/imgui/imgui_internal.h"

namespace w {

using namespace ImGui;

float anim(ImGuiID id, float target, float speed) {
    static std::unordered_map<ImGuiID, float> s;
    auto it = s.find(id);
    if (it == s.end()) it = s.emplace(id, target).first;
    float dt = GetIO().DeltaTime;
    it->second += (target - it->second) * (1.0f - std::exp(-dt * speed));
    if (std::fabs(it->second - target) < 0.002f) it->second = target;
    return it->second;
}

// ------------------------------------------------------------------------------------------------ drawing
void icon(ImDrawList* d, const char* glyph, ImVec2 c, float px, ImU32 col) {
    if (!glyph || !*glyph) return;
    ImFont* f = g_fontIcon ? g_fontIcon : GetFont();
    ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0, glyph);
    d->AddText(f, px, ImVec2(std::floor(c.x - sz.x * 0.5f + 0.5f), std::floor(c.y - sz.y * 0.5f + 0.5f)), col, glyph);
}

void customIcon(ImDrawList* d, Custom ic, ImVec2 c, float px, ImU32 col) {
    float k = px / 24.0f, t = std::max(1.2f, 1.7f * k);
    auto P = [&](float x, float y) { return ImVec2(c.x + (x - 12) * k, c.y + (y - 12) * k); };
    auto cap = [&](ImVec2 p) { d->AddCircleFilled(p, t * 0.5f, col, 8); };
    switch (ic) {
    case CI_COMB:
        d->AddRect(P(3, 5.5f), P(21, 9), col, 1.0f * k, 0, t);
        for (float x : {5.f, 8.f, 11.f, 14.f, 17.f}) { d->AddLine(P(x, 9), P(x, 18.5f), col, t); cap(P(x, 18.5f)); }
        d->AddLine(P(20, 9), P(20, 15.5f), col, t);
        cap(P(20, 15.5f));
        break;
    case CI_CURL: {
        // spiral: 3 arcs with a growing radius
        ImVec2 o = P(12, 11.6f);
        for (int i = 0; i <= 48; i++) {
            float a = i / 48.0f * 2.6f * 6.2832f, r = (0.8f + i / 48.0f * 7.4f) * k;
            d->PathLineTo(ImVec2(o.x + std::cos(a + 3.1416f) * r, o.y + std::sin(a + 3.1416f) * r));
        }
        d->PathStroke(col, 0, t);
        break;
    }
    case CI_HAIR:
        for (float ox : {0.f, 6.f, 12.f}) {
            d->AddBezierCubic(P(5 + ox, 3), P(6 + ox, 8), P(8.5f + ox, 10), P(8.5f + ox, 14.5f), col, t);
            d->AddBezierCubic(P(8.5f + ox, 14.5f), P(8.5f + ox, 19), P(6.5f + ox, 20), P(6 + ox, 21), col, t);
            cap(P(5 + ox, 3));
            cap(P(6 + ox, 21));
        }
        break;
    case CI_GUIDES:
        d->AddBezierCubic(P(5, 20), P(5, 13), P(8, 8), P(13, 5), col, t);
        d->AddBezierCubic(P(11, 20), P(11, 15), P(13.5f, 11), P(18, 8), col, t);
        for (ImVec2 q : {P(5, 20), P(11, 20), P(13, 5), P(18, 8)}) d->AddCircle(q, 1.6f * k, col, 10, t * 0.9f);
        break;
    case CI_COLLIDER: {
        d->AddRect(P(7, 2.5f), P(17, 21.5f), col, 5 * k, 0, t);
        // dashed equator
        for (int i = 0; i < 5; i += 2) {
            float x0 = 7 + i * 2.0f, x1 = x0 + 2.0f;
            auto y = [](float x) { float u = (x - 12) / 5; return 9.5f + 1.0f * (1 - u * u); };
            d->AddLine(P(x0, y(x0)), P(x1, y(x1)), col, t * 0.8f);
        }
        break;
    }
    default: break;
    }
}

void gradientRect(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding, ImDrawFlags fl, bool horizontal) {
    if (b.x - a.x < 0.5f || b.y - a.y < 0.5f) return;
    int v0 = d->VtxBuffer.Size;
    d->AddRectFilled(a, b, c0, rounding, fl);
    int v1 = d->VtxBuffer.Size;
    ShadeVertsLinearColorGradientKeepAlpha(d, v0, v1, a, horizontal ? ImVec2(b.x, a.y) : ImVec2(a.x, b.y), c0, c1);
}

static void chamferPath(ImDrawList* d, ImVec2 a, ImVec2 b, float c) {
    d->PathLineTo(ImVec2(a.x + c, a.y));
    d->PathLineTo(ImVec2(b.x, a.y));
    d->PathLineTo(ImVec2(b.x, b.y - c));
    d->PathLineTo(ImVec2(b.x - c, b.y));
    d->PathLineTo(ImVec2(a.x, b.y));
    d->PathLineTo(ImVec2(a.x, a.y + c));
}

void chamferRect(ImDrawList* d, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 border) {
    if (fill) { chamferPath(d, a, b, cut); d->PathFillConvex(fill); }
    if (border) { chamferPath(d, ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f), cut); d->PathStroke(border, ImDrawFlags_Closed, 1.0f); }
}

void drawShadow(ImDrawList* d, ImVec2 a, ImVec2 b, float radius, float strength) {
    // soft drop shadow (offset 6 px down): stacked translucent rounded rects
    float oy = S(6), spread = S(14);
    for (int i = 7; i >= 0; i--) {
        float e = spread * (i + 1) / 8.0f;
        d->AddRectFilled(ImVec2(a.x - e * 0.6f, a.y - e * 0.4f + oy), ImVec2(b.x + e * 0.6f, b.y + e + oy * 0.5f),
                         IM_COL32(0, 0, 0, (int)(255 * strength * 0.045f)), radius + e);
    }
}

ImVec2 textSize(ImFont* f, const char* t) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, t); }

void textTracked(ImDrawList* d, ImFont* f, ImVec2 p, ImU32 col, const char* t) { d->AddText(f, f->FontSize, p, col, t); }

std::string upper(const std::string& s) {
    std::string r = s;
    for (auto& ch : r) ch = (char)std::toupper((unsigned char)ch);
    return r;
}

// ------------------------------------------------------------------------------------------------ badge / kbd
ImU32 statusColour(const std::string& s) {
    if (s == "APPROVED") return tc(tok::GLOW);
    if (s == "FINAL") return tc(tok::SILVER);
    if (s == "PICK") return tc(tok::SAND);
    if (s == "IN PROGRESS") return tc(tok::STEEL);
    if (s == "NOT STARTED") return tc(tok::DIM);   // roster characters with no build yet: muted
    return tc(tok::HOLD);
}

float badgeWidth(const std::string& st) { return S(7) + S(5) + S(5) + textSize(g_fontBadge, upper(st).c_str()).x + S(6); }

float badge(ImDrawList* d, ImVec2 p, const std::string& st, bool backdrop) {
    std::string t = upper(st);
    float wdt = badgeWidth(st), h = S(18);
    ImU32 c = statusColour(st);
    ImVec4 cv = ColorConvertU32ToFloat4(c);
    ImVec2 b(p.x + wdt, p.y + h);
    if (backdrop) chamferRect(d, p, b, S(4), IM_COL32(10, 11, 13, 160), 0);
    chamferRect(d, p, b, S(4), ColorConvertFloat4ToU32(ImVec4(cv.x, cv.y, cv.z, 0.10f)), c);
    d->AddCircleFilled(ImVec2(p.x + S(7) + S(2.5f), p.y + h * 0.5f), S(2.5f), c, 10);
    ImVec2 ts = textSize(g_fontBadge, t.c_str());
    d->AddText(g_fontBadge, g_fontBadge->FontSize, ImVec2(p.x + S(7) + S(5) + S(5), p.y + (h - ts.y) * 0.5f), c, t.c_str());
    return wdt;
}

float kbdWidth(const char* t) { return textSize(g_fontKbd, t).x + S(9); }

float kbd(ImDrawList* d, ImVec2 p, const char* t, bool onAmber) {
    float wdt = kbdWidth(t), h = S(16);
    ImVec2 b(p.x + wdt, p.y + h);
    ImU32 border = onAmber ? IM_COL32(0, 0, 0, 64) : tc(tok::IRON2), fill = onAmber ? IM_COL32(0, 0, 0, 38) : tc(tok::BG2);
    d->AddRectFilled(p, b, border, S(4));
    d->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(b.x - 1, b.y - S(2)), fill, S(3));
    ImVec2 ts = textSize(g_fontKbd, t);
    d->AddText(g_fontKbd, g_fontKbd->FontSize, ImVec2(p.x + (wdt - ts.x) * 0.5f, p.y + (h - S(1) - ts.y) * 0.5f),
               onAmber ? IM_COL32(42, 26, 10, 255) : tc(tok::MUTED), t);
    return wdt;
}

// ------------------------------------------------------------------------------------------------ tooltips
static void parseTip(const char* text, std::string& title, std::string& key, std::string& body) {
    std::string s = text ? text : "";
    size_t nl = s.find('\n');
    std::string first = nl == std::string::npos ? s : s.substr(0, nl);
    body = nl == std::string::npos ? "" : s.substr(nl + 1);
    size_t op = first.rfind(" (");
    if (!first.empty() && first.back() == ')' && op != std::string::npos && first.size() - op - 3 <= 14 && op > 0) {
        title = first.substr(0, op);
        key = first.substr(op + 2, first.size() - op - 3);
    } else if (nl != std::string::npos && first.size() < 60) {
        title = first;
    } else {
        body = s;
    }
}

static void tipWindow(const char* glyph, const std::string& title, const std::string& key, const std::string& body) {
    PushStyleVar(ImGuiStyleVar_WindowPadding, S2(11, 9));
    PushStyleVar(ImGuiStyleVar_WindowRounding, S(7));
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    PushStyleColor(ImGuiCol_PopupBg, tv(tok::TIPBG, 0.98f));
    PushStyleColor(ImGuiCol_Border, tv(tok::IRON2));
    BeginTooltip();
    float wrap = S(250);
    if (!title.empty()) {
        ImDrawList* d = GetWindowDrawList();
        ImVec2 p = GetCursorScreenPos();
        float x = 0;
        if (glyph) { icon(d, glyph, ImVec2(p.x + S(7), p.y + GetTextLineHeight() * 0.5f), S(14), tc(tok::GLOW)); x = S(20); }
        PushFont(g_fontUiSemi);
        ImVec2 ts = CalcTextSize(title.c_str());
        d->AddText(ImVec2(p.x + x, p.y), tc(tok::TEXT), title.c_str());
        PopFont();
        float rowW = x + ts.x;
        if (!key.empty()) {
            float kw = kbdWidth(key.c_str());
            float kx = std::max(rowW + S(12), body.empty() ? 0.0f : wrap - kw);
            kbd(d, ImVec2(p.x + kx, p.y + (GetTextLineHeight() - S(16)) * 0.5f), key.c_str());
            rowW = kx + kw;
        }
        Dummy(ImVec2(rowW, GetTextLineHeight() + (body.empty() ? 0 : S(2))));
    }
    if (!body.empty()) {
        PushStyleColor(ImGuiCol_Text, tv(title.empty() ? tok::TEXT : tok::SOFT));
        PushTextWrapPos(GetCursorPosX() + wrap);
        TextUnformatted(body.c_str());
        PopTextWrapPos();
        PopStyleColor();
    }
    EndTooltip();
    PopStyleColor(2);
    PopStyleVar(3);
}

void richTip(const char* glyph, const char* title, const char* key, const char* body) {
    tipWindow(glyph, title ? title : "", key ? key : "", body ? body : "");
}

static void tipNow(const char* text) {
    std::string t, k, b;
    parseTip(text, t, k, b);
    tipWindow(nullptr, t, k, b);
}

void tip(const char* text) {
    if (!text || !*text) return;
    if (!IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    tipNow(text);
}

void tipAt(const char* text, ImVec2 pos, bool arrowLeft) {
    if (!text || !*text) return;
    if (!IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    std::string t, k, b;
    parseTip(text, t, k, b);
    SetNextWindowPos(pos);
    tipWindow(nullptr, t, k, b);
    if (arrowLeft) {
        ImDrawList* d = GetForegroundDrawList();
        ImVec2 a(pos.x, pos.y + S(19));
        float r = S(6);
        d->AddTriangleFilled(ImVec2(a.x - r, a.y), ImVec2(a.x + 1, a.y - r), ImVec2(a.x + 1, a.y + r), tc(tok::TIPBG));
        d->AddLine(ImVec2(a.x, a.y - r), ImVec2(a.x - r, a.y), tc(tok::IRON2), 1);
        d->AddLine(ImVec2(a.x - r, a.y), ImVec2(a.x, a.y + r), tc(tok::IRON2), 1);
    }
}

// ------------------------------------------------------------------------------------------------ widgets
bool iconToggle(const char* id, const char* glyph, Custom ci, bool on, const char* tipText, float size) {
    float sz = S(size);
    ImVec2 p = GetCursorScreenPos();
    bool r = InvisibleButton(id, ImVec2(sz, sz));
    bool hov = IsItemHovered(), dis = (GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImDrawList* d = GetWindowDrawList();
    ImVec2 b(p.x + sz, p.y + sz), c(p.x + sz * 0.5f, p.y + sz * 0.5f);
    if (on) d->AddRectFilled(p, b, AMBER_WASH, S(7));
    else if (hov && !dis) d->AddRectFilled(p, b, tc(tok::BG3), S(7));
    ImU32 col = on ? tc(tok::GLOW) : (hov && !dis) ? tc(tok::TEXT) : tc(tok::MUTED, dis ? 0.5f : 1.0f);
    if (ci != CI_NONE) customIcon(d, ci, c, S(18), col);
    else icon(d, glyph, c, S(18), col);
    tip(tipText);
    return r;
}

bool segmented(const char* id, const char* const* labels, int n, int* v, const char* const* tips) {
    PushID(id);
    float h = S(28), pad = S(2), gap = S(2);
    std::vector<float> ws(n);
    float total = pad * 2 + gap * (n - 1);
    for (int i = 0; i < n; i++) {
        const char* e = FindRenderedTextEnd(labels[i]);
        ws[i] = CalcTextSize(labels[i], e).x + S(22) + (i == *v ? S(9) : 0);
        total += ws[i];
    }
    ImVec2 p = GetCursorScreenPos();
    ImDrawList* d = GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + total, p.y + h + pad * 2), tc(tok::BG0), S(7));
    d->AddRect(p, ImVec2(p.x + total, p.y + h + pad * 2), tc(tok::IRON), S(7));
    bool changed = false;
    float x = p.x + pad;
    for (int i = 0; i < n; i++) {
        SetCursorScreenPos(ImVec2(x, p.y + pad));
        if (InvisibleButton(labels[i], ImVec2(ws[i], h))) { changed = true; *v = i; }   // any click (re-applies the active one)
        bool hov = IsItemHovered();
        if (tips && tips[i]) tip(tips[i]);
        ImVec2 a(x, p.y + pad), b(x + ws[i], p.y + pad + h);
        bool on = *v == i;
        if (on) {
            d->AddRectFilled(a, b, tc(tok::BG3), S(5));
            d->AddRect(a, b, tc(tok::IRON2), S(5));
        }
        const char* e = FindRenderedTextEnd(labels[i]);
        ImVec2 ts = CalcTextSize(labels[i], e);
        d->AddText(ImVec2(a.x + S(11), a.y + (h - ts.y) * 0.5f), on ? tc(tok::TEXT) : hov ? tc(tok::SOFT) : tc(tok::MUTED), labels[i], e);
        if (on) d->AddCircleFilled(ImVec2(a.x + S(11) + ts.x + S(6) + S(2.5f), a.y + h * 0.5f), S(2.5f), tc(tok::GLOW), 10);
        x += ws[i] + gap;
    }
    SetCursorScreenPos(p);
    Dummy(ImVec2(total, h + pad * 2));
    PopID();
    return changed;
}

bool switchRow(const char* label, bool* v, const char* tipText) {
    float W = GetContentRegionAvail().x, h = S(26);
    ImVec2 p = GetCursorScreenPos();
    bool r = InvisibleButton(label, ImVec2(W, h));
    if (r) *v = !*v;
    bool hov = IsItemHovered(), dis = (GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    tip(tipText);
    ImDrawList* d = GetWindowDrawList();
    const char* e = FindRenderedTextEnd(label);
    ImVec2 ts = CalcTextSize(label, e);
    d->AddText(ImVec2(p.x, p.y + (h - ts.y) * 0.5f), hov && !dis ? tc(tok::TEXT) : tc(tok::SOFT, dis ? 0.5f : 1.0f), label, e);
    float t = anim(GetItemID(), *v ? 1.0f : 0.0f, 18.0f);
    ImVec2 a(p.x + W - S(30), p.y + (h - S(17)) * 0.5f), b(a.x + S(30), a.y + S(17));
    ImVec4 off = tv(tok::BG4), on = tv(tok::AMBER);
    ImVec4 m(off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t, 1);
    d->AddRectFilled(a, b, ColorConvertFloat4ToU32(m), S(8.5f));
    if (t < 0.5f) d->AddRect(a, b, tc(tok::IRON2), S(8.5f));
    ImVec4 k0 = tv(tok::MUTED), k1 = tv(0xfff6ea);
    ImVec4 kc(k0.x + (k1.x - k0.x) * t, k0.y + (k1.y - k0.y) * t, k0.z + (k1.z - k0.z) * t, 1);
    d->AddCircleFilled(ImVec2(a.x + S(8.5f) + t * S(13), a.y + S(8.5f)), S(6.5f), ColorConvertFloat4ToU32(kc), 20);
    return r;
}

bool sliderRow(const char* label, const char* display, float* v, float lo, float hi, const char* fmt, float refValue,
               bool modified, bool* reset, const char* tipText) {
    ImGuiWindow* win = GetCurrentWindow();
    if (win->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    if (hi <= lo) hi = lo + 1e-4f;
    PushID(label);
    ImGuiID id = win->GetID("##slider");
    float W = GetContentRegionAvail().x, rowH = S(28), labW = S(118), valW = S(52), gap = S(10);
    ImVec2 p = win->DC.CursorPos;
    ImRect row(p, ImVec2(p.x + W, p.y + rowH));
    ImRect track(ImVec2(p.x + labW + gap, p.y + S(4)), ImVec2(p.x + W - valW - gap, p.y + rowH - S(4)));
    ImRect val(ImVec2(p.x + W - valW, p.y + S(3)), ImVec2(p.x + W, p.y + rowH - S(3)));
    ItemSize(row.GetSize());
    bool changed = false;
    if (!ItemAdd(track, id)) { PopID(); return false; }
    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);
    bool hovered = ItemHoverable(track, id, g.LastItemData.ItemFlags);
    bool rowHov = IsWindowHovered() && IsMouseHoveringRect(row.Min, row.Max);
    bool valHov = IsWindowHovered() && IsMouseHoveringRect(val.Min, val.Max);
    bool tempActive = TempInputIsActive(id);
    if (!tempActive) {
        bool clicked = hovered && IsMouseClicked(0, ImGuiInputFlags_None, id);
        bool dbl = valHov && IsMouseDoubleClicked(0);
        bool ctrlClick = (clicked || (valHov && IsMouseClicked(0))) && g.IO.KeyCtrl;
        if (clicked || dbl || ctrlClick) {
            SetActiveID(id, win);
            SetFocusID(id, win);
            FocusWindow(win);
            g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
            if (dbl || ctrlClick) tempActive = true;
        }
    }
    ImDrawList* d = win->DrawList;
    if (tempActive) {
        PushStyleColor(ImGuiCol_FrameBg, tv(tok::BG0));
        PushStyleColor(ImGuiCol_Border, tv(tok::GLOW));
        PushStyleVar(ImGuiStyleVar_FrameRounding, S(5));
        PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), (val.GetHeight() - GetFontSize()) * 0.5f));
        changed = TempInputScalar(val, id, label, ImGuiDataType_Float, v, fmt, &lo, &hi);
        PopStyleVar(2);
        PopStyleColor(2);
    }
    // drag area (the knob travels inside the track minus its radius)
    float kr = S(6.5f);
    ImRect inner(ImVec2(track.Min.x + kr, track.Min.y), ImVec2(track.Max.x - kr, track.Max.y));
    if (!tempActive) {
        float sv = *v;
        if (g.ActiveId == id && IsMouseDown(0)) {
            float t = std::clamp((g.IO.MousePos.x - inner.Min.x) / std::max(inner.GetWidth(), 1.0f), 0.0f, 1.0f);
            sv = lo + (hi - lo) * t;
            // round to the displayed precision
            char buf[64];
            snprintf(buf, sizeof buf, fmt, sv);
            sv = (float)atof(buf);
            if (sv != *v) { *v = sv; changed = true; }
        } else if (g.ActiveId == id && !IsMouseDown(0) && !TempInputIsActive(id)) {
            ClearActiveID();
        }
    }
    if (changed) MarkItemEdited(id);
    bool active = g.ActiveId == id;
    // label
    const char* disp = display ? display : label;
    float cy = p.y + rowH * 0.5f;
    ImVec2 ls = CalcTextSize(disp);
    d->PushClipRect(p, ImVec2(p.x + labW + gap * 0.5f, p.y + rowH), true);
    d->AddText(ImVec2(p.x, cy - ls.y * 0.5f), rowHov || active ? tc(tok::TEXT) : tc(tok::MUTED), disp);
    d->PopClipRect();
    if (modified) d->AddCircleFilled(ImVec2(std::min(p.x + ls.x + S(9), p.x + labW), cy), S(3), tc(tok::GLOW), 10);
    // track
    float tx0 = inner.Min.x, tx1 = inner.Max.x;
    float t = std::clamp((*v - lo) / (hi - lo), 0.0f, 1.0f), kx = tx0 + (tx1 - tx0) * t;
    d->AddRectFilled(ImVec2(track.Min.x, cy - S(2)), ImVec2(track.Max.x, cy + S(2)), rowHov || active ? tc(0x323940) : tc(tok::BG4), S(2));
    gradientRect(d, ImVec2(track.Min.x, cy - S(2)), ImVec2(kx, cy + S(2)), tc(0x8a4f15), tc(tok::AMBER), S(2), 0, true);
    if (refValue == refValue) {   // not NaN: tick at the build's value
        float rt = std::clamp((refValue - lo) / (hi - lo), 0.0f, 1.0f), rx = tx0 + (tx1 - tx0) * rt;
        d->AddRectFilled(ImVec2(rx - S(1), cy - S(5)), ImVec2(rx + S(1), cy + S(5)), tc(tok::DIM), S(1));
    }
    float halo = anim(id, (rowHov || active) ? 1.0f : 0.0f);
    if (halo > 0.01f) d->AddCircleFilled(ImVec2(kx, cy), kr + S(6), tc(tok::GLOW, 0.25f * halo), 24);
    d->AddCircleFilled(ImVec2(kx, cy), kr + S(3), tc(tok::BG1), 24);
    d->AddCircleFilled(ImVec2(kx, cy), kr, active ? IM_COL32_WHITE : tc(tok::KNOB), 24);
    // value box
    if (!tempActive) {
        d->AddRectFilled(val.Min, val.Max, tc(tok::BG0), S(5));
        d->AddRect(val.Min, val.Max, valHov ? tc(tok::IRON3) : tc(tok::IRON), S(5));
        char buf[64];
        snprintf(buf, sizeof buf, fmt, *v);
        ImVec2 vs = CalcTextSize(buf);
        d->PushClipRect(val.Min, val.Max, true);
        d->AddText(ImVec2(val.Max.x - S(7) - vs.x, cy - vs.y * 0.5f), tc(tok::TEXT), buf);
        d->PopClipRect();
    }
    // tooltip (label / track), right-click menu
    if (rowHov && !active && tipText && *tipText) {
        static float hovT = 0;
        static ImGuiID hovId = 0;
        static int lastFrame = -10;
        if (hovId != id || g.FrameCount - lastFrame > 1) { hovId = id; hovT = 0; }
        lastFrame = g.FrameCount;
        hovT += g.IO.DeltaTime;
        if (hovT > g.Style.HoverDelayShort) tipNow(tipText);
    }
    if (rowHov && IsMouseClicked(1)) OpenPopup("##ctx");
    if (BeginPopup("##ctx")) {
        if (MenuItem("Reset to build value", nullptr, false, modified)) { if (reset) *reset = true; }
        EndPopup();
    }
    PopID();
    return changed;
}

bool sectionHeader(const char* label, const char* display, const char* glyph, Custom ci, const char* summary, bool modDot, bool defOpen) {
    ImGuiWindow* win = GetCurrentWindow();
    ImGuiID id = win->GetID(label);
    ImGuiStorage* st = win->DC.StateStorage;
    bool open = st->GetInt(id, defOpen ? 1 : 0) != 0;
    float x0 = win->InnerRect.Min.x, x1 = win->InnerRect.Max.x, h = S(35);
    ImVec2 p(x0, GetCursorScreenPos().y);
    SetCursorScreenPos(p);
    if (InvisibleButton(label, ImVec2(x1 - x0, h))) { open = !open; st->SetInt(id, open ? 1 : 0); }
    bool hov = IsItemHovered();
    ImDrawList* d = GetWindowDrawList();
    if (hov) d->AddRectFilled(p, ImVec2(x1, p.y + h), tc(tok::BG2));
    d->AddLine(ImVec2(x0, p.y), ImVec2(x1, p.y), tc(tok::IRON), 1);
    float cy = p.y + h * 0.5f, x = x0 + S(12);
    icon(d, open ? ICON_LC_CHEVRON_DOWN : ICON_LC_CHEVRON_RIGHT, ImVec2(x + S(7), cy), S(14), tc(tok::DIM));
    x += S(14) + S(10);
    ImU32 ic = open ? tc(tok::GLOW) : tc(tok::MUTED);
    if (ci != CI_NONE) customIcon(d, ci, ImVec2(x + S(8), cy), S(16), ic);
    else icon(d, glyph, ImVec2(x + S(8), cy), S(16), ic);
    x += S(16) + S(10);
    std::string t = upper(display ? display : label);
    ImVec2 ts = textSize(g_fontSection, t.c_str());
    d->AddText(g_fontSection, g_fontSection->FontSize, ImVec2(x, cy - ts.y * 0.5f), tc(tok::TEXT), t.c_str());
    if (summary && *summary) {
        ImVec2 ss = textSize(g_fontSmall, summary);
        float sx = x1 - S(14) - ss.x;
        sx = std::max(sx, x + ts.x + S(20));
        d->PushClipRect(ImVec2(x + ts.x + S(8), p.y), ImVec2(x1 - S(8), p.y + h), true);
        d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(sx, cy - ss.y * 0.5f), tc(tok::MUTED), summary);
        if (modDot) d->AddCircleFilled(ImVec2(sx - S(9), cy), S(3), tc(tok::GLOW), 10);
        d->PopClipRect();
    }
    if (open) Dummy(ImVec2(0, S(2)));
    return open;
}

static void primaryFill(ImDrawList* d, ImVec2 a, ImVec2 b, bool chamfer, bool hov, bool held, float alpha) {
    ImU32 c0 = tc(hov ? 0xe08e34 : 0xd4832a, alpha), c1 = tc(held ? 0xa65d15 : 0xb8691a, alpha);
    // soft amber glow under the button
    for (int i = 1; i <= 3; i++) d->AddRectFilled(ImVec2(a.x - i, a.y - i + S(2)), ImVec2(b.x + i, b.y + i + S(2)), tc(tok::AMBER, 0.07f * alpha), S(7) + i);
    int v0 = d->VtxBuffer.Size;
    if (chamfer) { chamferPath(d, a, b, S(6)); d->PathFillConvex(c0); }
    else d->AddRectFilled(a, b, c0, S(6));
    ShadeVertsLinearColorGradientKeepAlpha(d, v0, d->VtxBuffer.Size, a, ImVec2(a.x, b.y), c0, c1);
    if (chamfer) chamferRect(d, a, b, S(6), 0, tc(0xe09440, alpha));
    else d->AddRect(a, b, tc(0xe09440, alpha), S(6));
    d->AddLine(ImVec2(a.x + S(6), a.y + 1.5f), ImVec2(b.x - S(4), a.y + 1.5f), IM_COL32(255, 220, 170, (int)(90 * alpha)), 1);
}

unsigned g_primaryAlt = 0;   // non-zero: primaryButton filled with this colour instead of amber (Community)
bool primaryButton(const char* label, const char* text, ImVec2 size, bool enabled, const char* glyph, bool chamfer, const char* kbdText, const char* sub) {
    ImFont* f = g_fontUiSemi;
    ImVec2 ts = textSize(f, text);
    float gw = glyph ? S(22) : 0;
    if (size.x <= 0) size.x = size.x < 0 ? GetContentRegionAvail().x : ts.x + gw + S(24);
    if (size.y <= 0) size.y = S(30);
    ImVec2 p = GetCursorScreenPos();
    if (!enabled) BeginDisabled();
    bool r = InvisibleButton(label, size);
    bool hov = IsItemHovered(), held = IsItemActive();
    if (!enabled) EndDisabled();
    float alpha = enabled ? 1.0f : 0.45f;
    ImDrawList* d = GetWindowDrawList();
    ImVec2 b(p.x + size.x, p.y + size.y);
    ImU32 ink = IM_COL32(27, 16, 6, (int)(255 * alpha));
    if (g_primaryAlt) {
        float k = held ? 0.8f : hov && enabled ? 1.12f : 1.0f;
        auto ch = [&](int sh) { return (int)(std::min)(255.0f, ((g_primaryAlt >> sh) & 255) * k); };
        ImU32 top = IM_COL32(ch(16), ch(8), ch(0), (int)(255 * alpha));
        ImU32 bot = IM_COL32((int)(ch(16) * 0.78f), (int)(ch(8) * 0.78f), (int)(ch(0) * 0.78f), (int)(255 * alpha));
        (void)bot;
        d->AddRectFilled(p, b, top, S(4));
        d->AddRect(p, b, IM_COL32(220, 235, 255, (int)(90 * alpha)), S(4));
        ink = IM_COL32(8, 16, 28, (int)(255 * alpha));
    } else primaryFill(d, p, b, chamfer, hov && enabled, held, alpha);
    float cy = p.y + size.y * 0.5f;
    bool left = chamfer || kbdText || sub;
    float x = left ? p.x + S(14) : p.x + (size.x - ts.x - gw) * 0.5f;
    if (glyph) { icon(d, glyph, ImVec2(x + S(8), cy), S(15), ink); x += gw; }
    d->AddText(f, f->FontSize, ImVec2(x, cy - ts.y * 0.5f), ink, text);
    x += ts.x + S(10);
    if (sub && *sub) {
        ImVec2 ss = textSize(g_fontSmall, sub);
        float maxX = b.x - (kbdText ? kbdWidth(kbdText) + S(20) : S(10));
        d->PushClipRect(ImVec2(x, p.y), ImVec2(maxX, b.y), true);
        d->AddText(g_fontSmall, g_fontSmall->FontSize, ImVec2(x, cy - ss.y * 0.5f), IM_COL32(27, 16, 6, (int)(190 * alpha)), sub);
        d->PopClipRect();
    }
    if (kbdText) kbd(d, ImVec2(b.x - S(14) - kbdWidth(kbdText), cy - S(8)), kbdText, true);
    return r && enabled;
}

bool button(const char* label, const char* text, ImVec2 size, const char* glyph, bool ghost, bool enabled, const char* kbdText) {
    ImVec2 ts = CalcTextSize(text);
    float gw = glyph ? S(23) : 0, kw = kbdText ? kbdWidth(kbdText) + S(8) : 0;
    if (size.x == 0) size.x = ts.x + gw + kw + S(24);
    if (size.x < 0) size.x = GetContentRegionAvail().x;
    if (size.y <= 0) size.y = S(30);
    ImVec2 p = GetCursorScreenPos();
    if (!enabled) BeginDisabled();
    bool r = InvisibleButton(label, size);
    bool hov = IsItemHovered() && enabled, held = IsItemActive();
    if (!enabled) EndDisabled();
    float alpha = enabled ? 1.0f : 0.45f;
    ImDrawList* d = GetWindowDrawList();
    ImVec2 b(p.x + size.x, p.y + size.y);
    if (!ghost || hov) d->AddRectFilled(p, b, held ? AMBER_WASH : hov ? tc(tok::BG3) : tc(tok::BG2, alpha), S(6));
    if (!ghost) d->AddRect(p, b, hov ? tc(tok::IRON3) : tc(tok::IRON2, alpha), S(6));
    float x = p.x + (size.x - ts.x - gw - kw) * 0.5f, cy = p.y + size.y * 0.5f;
    ImU32 col = ghost ? tc(hov ? tok::TEXT : tok::SOFT, alpha) : tc(tok::TEXT, alpha);
    if (glyph) { icon(d, glyph, ImVec2(x + S(8), cy), S(15), col); x += gw; }
    d->AddText(ImVec2(x, cy - ts.y * 0.5f), col, text, FindRenderedTextEnd(text));
    if (kbdText) kbd(d, ImVec2(x + ts.x + S(8), cy - S(8)), kbdText);
    return r && enabled;
}

bool iconButton(const char* id, const char* glyph, float size, const char* tipText, bool enabled) {
    float sz = S(size);
    ImVec2 p = GetCursorScreenPos();
    if (!enabled) BeginDisabled();
    bool r = InvisibleButton(id, ImVec2(sz, sz));
    bool hov = IsItemHovered() && enabled;
    tip(tipText);
    if (!enabled) EndDisabled();
    ImDrawList* d = GetWindowDrawList();
    ImVec2 b(p.x + sz, p.y + sz);
    d->AddRectFilled(p, b, hov ? tc(tok::BG3) : tc(tok::BG2), S(6));
    d->AddRect(p, b, hov ? tc(tok::IRON3) : tc(tok::IRON2), S(6));
    icon(d, glyph, ImVec2(p.x + sz * 0.5f, p.y + sz * 0.5f), S(15), tc(hov ? tok::TEXT : tok::SOFT, enabled ? 1.0f : 0.45f));
    return r && enabled;
}

void subHeader(const char* text) {
    Dummy(ImVec2(0, S(4)));
    PushFont(g_fontCap);
    TextColored(tv(tok::DIM), "%s", upper(text).c_str());
    PopFont();
}

void note(const char* glyph, unsigned glyphCol, const char* text) {
    ImVec2 p = GetCursorScreenPos();
    icon(GetWindowDrawList(), glyph, ImVec2(p.x + S(7), p.y + S(8)), S(13), tc(glyphCol));
    SetCursorScreenPos(ImVec2(p.x + S(20), p.y));
    PushFont(g_fontSmall);
    PushStyleColor(ImGuiCol_Text, tv(tok::MUTED));
    PushTextWrapPos(0.0f);
    TextUnformatted(text);
    PopTextWrapPos();
    PopStyleColor();
    PopFont();
}

void pushInputStyle() {
    PushStyleColor(ImGuiCol_FrameBg, tv(tok::BG0));
    PushStyleColor(ImGuiCol_FrameBgHovered, tv(tok::BG0));
    PushStyleColor(ImGuiCol_Border, tv(tok::IRON2));
    PushStyleVar(ImGuiStyleVar_FrameRounding, S(6));
}
void popInputStyle() {
    PopStyleVar();
    PopStyleColor(3);
}

}  // namespace w
