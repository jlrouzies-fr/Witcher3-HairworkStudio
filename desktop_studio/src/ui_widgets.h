// Desktop Groom Studio - Ink & Iron widgets (IMPLEMENTATION.md section 4). Every interactive widget registers its
// label with the test-engine hooks so --ui-test scripts find it. Our own code (MIT).
#pragma once
#include <string>

#include "../third_party/imgui/imgui.h"
#include "icons_lucide.h"
#include "theme.h"

namespace w {

// custom draw-list icons (Lucide has no equivalent)
enum Custom { CI_NONE, CI_COMB, CI_CURL, CI_HAIR, CI_GUIDES, CI_COLLIDER };

inline float S(float v) { return v * g_ui; }
inline ImVec2 S2(float x, float y) { return ImVec2(x * g_ui, y * g_ui); }

// animation: t moves towards target (one float per id)
float anim(ImGuiID id, float target, float speed = 14.0f);

// drawing
void icon(ImDrawList* d, const char* glyph, ImVec2 centre, float px, ImU32 col);
void customIcon(ImDrawList* d, Custom ic, ImVec2 centre, float px, ImU32 col);
void gradientRect(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding = 0, ImDrawFlags fl = 0, bool horizontal = false);
void chamferRect(ImDrawList* d, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 border);
void drawShadow(ImDrawList* d, ImVec2 a, ImVec2 b, float radius, float strength = 1.0f);
void textTracked(ImDrawList* d, ImFont* f, ImVec2 p, ImU32 col, const char* t);   // f already carries the tracking
ImVec2 textSize(ImFont* f, const char* t);
std::string upper(const std::string& s);

// small parts (draw only; return the width used)
ImU32 statusColour(const std::string& status);
float badgeWidth(const std::string& status);
float badge(ImDrawList* d, ImVec2 p, const std::string& status, bool backdrop = false);
float kbdWidth(const char* t);
float kbd(ImDrawList* d, ImVec2 p, const char* t, bool onAmber = false);   // p = top-left; height 16 px

// tooltips: "Title (K)\nbody" -> title row + key cap + body; one line without "\n" -> body only (or title + key)
void tip(const char* text);
void tipAt(const char* text, ImVec2 pos, bool arrowLeft);    // rail tooltips: to the right of the rail with an arrow
void richTip(const char* glyph, const char* title, const char* key, const char* body);

// widgets
bool iconToggle(const char* id, const char* glyph, Custom ci, bool on, const char* tipText, float size = 34);
bool segmented(const char* id, const char* const* labels, int n, int* v, const char* const* tips = nullptr);
bool switchRow(const char* label, bool* v, const char* tipText = nullptr);
// returns true when the value changed; *reset set when "Reset to build value" was picked in the right-click menu
bool sliderRow(const char* label, const char* display, float* v, float lo, float hi, const char* fmt, float refValue,
               bool modified, bool* reset, const char* tipText);
bool sectionHeader(const char* label, const char* display, const char* glyph, Custom ci, const char* summary, bool modDot, bool defOpen);
extern unsigned g_primaryAlt;
bool primaryButton(const char* label, const char* text, ImVec2 size, bool enabled, const char* glyph = nullptr, bool chamfer = false,
                   const char* kbdText = nullptr, const char* sub = nullptr);
bool button(const char* label, const char* text, ImVec2 size, const char* glyph = nullptr, bool ghost = false, bool enabled = true,
            const char* kbdText = nullptr);
bool iconButton(const char* id, const char* glyph, float size, const char* tipText, bool enabled = true);
void subHeader(const char* text);   // small caps sub-heading inside a section (DIM)
void note(const char* glyph, unsigned glyphCol, const char* text);
void pushInputStyle();
void popInputStyle();

}  // namespace w
