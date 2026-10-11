// Desktop Groom Studio - native Windows editor for Witcher 3 HairWorks grooms (window, main loop).
// Our own code (MIT). Renders and simulates with the NVIDIA HairWorks runtime (proprietary, GameWorks EULA),
// loaded at run time from a DLL path the user configures (default: the SDK copy in <repo>\sources\).
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_impl_dx11.h"
#include "../third_party/imgui/imgui_impl_win32.h"
#include "app.h"
#include "uitest.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
void openLog(const std::string& path, bool console);
extern void (*g_hairLog)(const char*);
void applyTheme(const std::string& fontDir, float dpi);   // ui.cpp

namespace fs = std::filesystem;

static App g_app;
static const char* g_stage = "start";

static std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void parseArgs(Args& a, int argc, wchar_t** argv) {
    for (int i = 1; i < argc; i++) {
        std::string s = narrow(argv[i]);
        auto next = [&]() { return i + 1 < argc ? narrow(argv[++i]) : std::string(); };
        if (s == "--cfg") a.cfg = next();
        else if (s == "--sdk") a.sdk = next();
        else if (s == "--dll") a.dll = next();
        else if (s == "--agent") a.agent = next();
        else if (s == "--project") a.project = next();
        else if (s == "--ui-test") a.uiTest = next();
        else if (s == "--no-splash") a.noSplash = true;
        else if (s == "--screenshot") a.screenshot = next();
        else if (s == "--frames") a.frames = std::stoi(next());
        else if (s == "--view") a.view = next();
        else if (s == "--motion") a.motion = next();
        else if (s == "--guides") a.guides = true;
        else if (s == "--close-lod") a.closeLod = true;
        else if (s == "--exit") a.exitAfter = true;
    }
}

// crash report into the log (headless runs)
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* e) {
    void* addr = e->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod);
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    // symbol from the PDB next to the exe (Release builds carry /Zi symbols)
    char symName[256] = "?";
    alignas(SYMBOL_INFO) char sbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* si = (SYMBOL_INFO*)sbuf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (SymInitialize(proc, nullptr, TRUE) && SymFromAddr(proc, (DWORD64)addr, &disp, si)) snprintf(symName, sizeof symName, "%s+0x%llx", si->Name, (unsigned long long)disp);
    IMAGEHLP_LINE64 line = {sizeof line};
    DWORD ld = 0;
    char lineInfo[300] = "";
    if (SymGetLineFromAddr64(proc, (DWORD64)addr, &ld, &line)) snprintf(lineInfo, sizeof lineInfo, " (%s:%lu)", line.FileName, line.LineNumber);
    logf("CRASH 0x%08lx at %s+0x%llx = %s%s (stage: %s)", e->ExceptionRecord->ExceptionCode, name,
         (unsigned long long)((char*)addr - (char*)mod), symName, lineInfo, g_stage);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void hairLogSink(const char* s) { logf("%s", s); }

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_app.gfx.dev && wp != SIZE_MINIMIZED) g_app.gfx.resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void shutdownAll(HWND hwnd) {
    if (g_app.job.th.joinable()) g_app.job.th.join();
    g_app.hwBeard.shutdown(false);   // second SDK object: releasing two SDK objects of this 2016 DLL crashes at exit; process exit frees it
    g_app.hw.shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_app.gfx.shutdown();
    DestroyWindow(hwnd);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    parseArgs(g_app.args, argc, argv);
    g_app.headless = !g_app.args.agent.empty();
    g_app.set.load();
    if (!g_app.args.sdk.empty()) g_app.set.sdkDir = g_app.args.sdk;
    if (!g_app.args.dll.empty()) g_app.set.dll = g_app.args.dll;
    if (g_app.headless && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
    }
    fs::create_directories(g_app.set.repo + "\\build\\desktop_studio");
    openLog(g_app.set.repo + "\\build\\desktop_studio\\" + (g_app.headless ? "agent.log" : "desktop_studio.log"), g_app.headless);
    SetUnhandledExceptionFilter(crashFilter);
    g_hairLog = hairLogSink;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HICON icoBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON icoSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSEXW wc = {sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, icoBig, LoadCursor(nullptr, IDC_ARROW),
                      nullptr, nullptr, L"HairworkStudio", icoSmall};
    RegisterClassExW(&wc);
    float sysDpi = GetDpiForSystem() / 96.0f;
    int ww = std::min(int(1680 * sysDpi), GetSystemMetrics(SM_CXSCREEN) - 40), wh = std::min(int(1020 * sysDpi), GetSystemMetrics(SM_CYSCREEN) - 60);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Hairwork Studio", WS_OVERLAPPEDWINDOW, 20, 20, ww, wh,
                              nullptr, nullptr, inst, nullptr);
    // test runs never take the user's screen or focus: the window opens without activation, off the visible desktop
    char* na = nullptr;
    size_t nn = 0;
    _dupenv_s(&na, &nn, "GROOM_STUDIO_NOACTIVATE");
    bool noActivate = !g_app.args.uiTest.empty() || (na && *na == '1');
    free(na);
    if (noActivate) {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200;
        SetWindowPos(hwnd, HWND_BOTTOM, vx, 0, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    // a normal launch shows the window only after the first frame (splash) is presented, see the main loop
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    float dpi = std::max(1.0f, GetDpiForWindow(hwnd) / 96.0f);
    ImGui_ImplWin32_Init(hwnd);
    if (!g_app.init(hwnd)) return 1;
    ImGui_ImplDX11_Init(g_app.gfx.dev, g_app.gfx.ctx);
    std::string fonts = exeDir() + "\\assets\\fonts";
    if (!fs::exists(fonts)) fonts = g_app.set.repo + "\\desktop_studio\\assets\\fonts";
    applyTheme(fonts, dpi);
    if (g_app.headless) {
        g_stage = "agent";
        int rc = g_app.runAgent(g_app.args.agent);
        shutdownAll(hwnd);
        return rc;
    }
    g_app.splashOn = !g_app.args.noSplash && g_app.args.uiTest.empty() && g_app.args.screenshot.empty() && !g_app.headless;
    UiTest uitest;
    if (!g_app.args.uiTest.empty() && !uitest.load(g_app.args.uiTest, g_app)) { shutdownAll(hwnd); return 2; }
    g_stage = "open";
    if (!g_app.args.project.empty()) g_app.loadProject(g_app.args.project);
    else if (!g_app.args.cfg.empty()) g_app.openConfig(g_app.args.cfg, false);
    if (!g_app.args.motion.empty())
        for (int i = 0; i < (int)MotionPreset::Count; i++) {
            std::string n = motionName(MotionPreset(i));
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            if (n.find(g_app.args.motion) == 0) g_app.motion.preset = MotionPreset(i);
        }
    g_app.showGuideOverlay = g_app.args.guides;
    g_app.forceCloseLod = g_app.args.closeLod;
    auto t0 = std::chrono::high_resolution_clock::now();
    int frameNo = 0;
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        auto t1 = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(t1 - t0).count();
        t0 = t1;
        if (!g_app.args.screenshot.empty()) dt = 1.0 / 60.0;     // deterministic capture runs
        g_stage = "ui-test";
        uitest.tick(g_app, hwnd);
        if (uitest.active) dt = 1.0 / 60.0;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        g_stage = "input";
        g_app.handleInput();
        g_stage = "step";
        g_app.step(dt);
        g_stage = "draw";
        g_app.drawScene();
        if (frameNo % 15 == 0) g_app.st = g_app.hw.stats();
        g_stage = "ui";
        g_app.ui();
        g_app.drawSplash();
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!g_app.pendingShot.empty()) {
            bool ok = g_app.gfx.saveScreenshot(g_app.pendingShot);
            g_app.status = (ok ? "Saved " : "Screenshot failed: ") + g_app.pendingShot;
            g_app.pendingShot.clear();
        }
        frameNo++;
        if (!g_app.args.screenshot.empty() && frameNo == g_app.args.frames) {
            g_app.st = g_app.hw.stats();
            bool ok = g_app.gfx.saveScreenshot(g_app.args.screenshot);
            logf("screenshot %s: %s; HairWorks hairs %d; estimate %.0f, close-up %.0f; reference %.0f / %.0f; ratio %.2f",
                 g_app.args.screenshot.c_str(), ok ? "ok" : "FAILED", g_app.st.hairs, g_app.bud.hairs, g_app.bud.hairsClose,
                 g_app.bud.refHairs, g_app.bud.refHairsClose, g_app.bud.ratio);
            if (g_app.args.exitAfter) done = true;
        }
        g_app.gfx.present(true);
        if (frameNo == 1 && !noActivate) {
            ShowWindow(hwnd, SW_SHOWDEFAULT);
            UpdateWindow(hwnd);
        }
    }
    shutdownAll(hwnd);
    return 0;
}
