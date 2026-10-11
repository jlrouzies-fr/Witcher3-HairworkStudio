// Desktop Groom Studio - built-in UI test mode (events injected into Dear ImGui, no OS input). Our own code (MIT).
#pragma once
#include <windows.h>

#include <string>

#include "../third_party/imgui/imgui.h"
#include "deskpack.h"

struct App;

struct UiTest {
    bool active = false, finished = false;
    json script, result;
    std::string out;
    size_t idx = 0;
    int phase = 0, phaseFrames = 0, wait = 0, failures = 0;
    ImVec2 clickPos;
    bool load(const std::string& path, App& app);
    bool tick(App& app, HWND hwnd);
    bool find(const std::string& label, int nth, ImVec2& c) const;
    void record(const json& step, bool ok, const std::string& note);
};
