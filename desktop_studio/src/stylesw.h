// Desktop Groom Studio - hairstyle helpers shared by the picker (picker.cpp) and the editor switcher (stylesw.cpp,
// ui.cpp). Our own code (MIT).
#pragma once
#include <string>

#include "../third_party/imgui/imgui.h"
#include "app.h"

// hair type index: 1 cards, 2 tied cards, 3 native HairWorks, 4 helmet / hood, 5 beard only, 6 bald (picker.cpp)
int hairTypeIdx(const std::string& hairType);
const char* hairTypeName(int k);
int styleHairIdx(const json& c, int styleIdx);     // the style's own hair_type, else the character's
float hairTypeChip(ImDrawList* d, ImVec2 p, int k, const std::string& extra = std::string());   // returns the width

// editor switcher pieces (stylesw.cpp)
// inspector character card: the "Hairstyle" row (field "##card_style") + the not-started panel; returns the height used
float styleCardRow(App& a, ImVec2 p, float W);
