// Desktop Groom Studio - "Refined Ink & Iron" design tokens, fonts and layout shared by ui.cpp, toolbar.cpp and
// picker.cpp (design/ui_direction1/IMPLEMENTATION.md section 1). Our own code (MIT).
#pragma once
#include "../third_party/imgui/imgui.h"

// colour tokens (0xRRGGBB)
namespace tok {
enum : unsigned {
    BG0 = 0x0d0f11, BG1 = 0x131619, BG2 = 0x191d21, BG3 = 0x20252a, BG4 = 0x282e34,
    IRON = 0x2b3036, IRON2 = 0x3b424a, IRON3 = 0x545c66,
    TEXT = 0xe4e6e9, SOFT = 0xb9bec5, MUTED = 0x8a9099, DIM = 0x5f656d,
    AMBER = 0xc8761e, GLOW = 0xf0a040, OK = 0x8fbf8a, WARN = 0xe2765a,
    STEEL = 0x8ea9c9, SAND = 0xd8b377, SILVER = 0xc9ced6, HOLD = 0x8b9097, VIEW = 0x121416,
    KNOB = 0xece6dc, TIPBG = 0x0b0d0f,
};
}
inline ImU32 tc(unsigned rgb, float a = 1.0f) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (int)(a * 255.0f + 0.5f));
}
inline ImVec4 tv(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}
#define AMBER_WASH tc(tok::AMBER, 0.13f)

// fonts (applyTheme in ui.cpp). Icons (Lucide) are merged into fUi / fUiSemi / fSmall; fIcon is icon-only.
extern ImFont* g_fontUi;        // Inter 13 (+ icons 15)
extern ImFont* g_fontUiSemi;    // Inter SemiBold 13 (+ icons 15)
extern ImFont* g_fontSmall;     // Inter 11.5 (+ icons 13)
extern ImFont* g_fontSemi14;    // Inter SemiBold 14 (budget numbers)
extern ImFont* g_fontCap;       // Inter SemiBold 10.5, tracked (caps labels)
extern ImFont* g_fontBadge;     // Inter SemiBold 9.5, tracked (badges)
extern ImFont* g_fontKbd;       // Inter 10
extern ImFont* g_fontBig;       // Inter SemiBold 26 (budget %)
extern ImFont* g_fontSection;   // Cinzel SemiBold 12, tracked (section headers)
extern ImFont* g_fontGroup;     // Cinzel SemiBold 11.5 (picker group headers)
extern ImFont* g_fontFor;       // Cinzel SemiBold 10.5 ("FOR" in the brand)
extern ImFont* g_fontName13;    // Cinzel SemiBold 13 (brand, breadcrumb)
extern ImFont* g_fontName15;    // Cinzel SemiBold 15 (card names)
extern ImFont* g_fontName;      // Cinzel SemiBold 17 (inspector name)
extern ImFont* g_fontDisplay;   // Cinzel SemiBold 22 (picker title)
extern ImFont* g_fontHero;      // Cinzel SemiBold 28 (drawer hero name)
extern ImFont* g_fontIcon;      // Lucide only, 20 (scaled for other sizes)
extern ImFont* g_fontTitle;     // = g_fontSection (older code)

// layout (scaled by the monitor DPI)
extern float g_ui;              // DPI scale
extern float g_titleH, g_railW, g_panelW, g_statusH;
