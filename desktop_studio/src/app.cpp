// Desktop Groom Studio - application logic (open, edit, simulate, render, export, projects). Our own code (MIT).
#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../third_party/imgui/imgui.h"
#include "lighting.h"

namespace fs = std::filesystem;
using namespace DirectX;

#ifndef HW_DEFAULT_SDK_DIR
#define HW_DEFAULT_SDK_DIR ""
#endif
#ifndef HW_REPO_ROOT
#define HW_REPO_ROOT ""
#endif

bool writePng(const std::string& path, int w, int h, const unsigned char* rgba);
unsigned char* loadImage(const std::string& path, int* w, int* h);
void freeImage(unsigned char* p);

const char* CREDIT = "Uses NVIDIA HairWorks, NVIDIA GameWorks technology, (c) NVIDIA Corporation";

// ================================================================================================ log / process
static std::vector<std::string> g_log;
static std::ofstream g_logFile;
static bool g_console = false;

void openLog(const std::string& path, bool console) {
    g_logFile.open(path);
    g_console = console;
}

void logf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_log.push_back(buf);
    if (g_log.size() > 600) g_log.erase(g_log.begin());
    if (g_logFile) { g_logFile << buf << "\n"; g_logFile.flush(); }
    if (g_console) { fputs(buf, stdout); fputs("\n", stdout); fflush(stdout); }
}

const std::vector<std::string>& logLines() { return g_log; }

int runCapture(const std::string& cmd, const std::string& cwd, std::string& out) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::string c = cmd;
    if (!CreateProcessA(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        out = "cannot start: " + cmd;
        return -1;
    }
    CloseHandle(wr);
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return (int)code;
}

static std::string narrowW(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return narrowW(fs::path(buf).parent_path().wstring());
}

// ================================================================================================ settings
std::string Settings::dllPath() const {
    if (!dll.empty()) return dll;
    std::string p = sdkDir + "\\bin\\win64\\NvHairWorksDx11.win64.dll";
    // release builds bundle the runtime next to the exe (packaging/package_release.py); the SDK copy wins when present
    std::string bundled = exeDir() + "\\NvHairWorksDx11.win64.dll";
    if (!fs::exists(p) && fs::exists(bundled)) return bundled;
    return p;
}

void Settings::load() {
    char* la = nullptr;
    size_t n = 0;
    _dupenv_s(&la, &n, "LOCALAPPDATA");
    file = std::string(la ? la : ".") + "\\HairworkForAll\\desktop_studio.json";
    free(la);
    std::string fileRepo;
    std::ifstream f(file);
    if (f) {
        try {
            json j = json::parse(f);
            sdkDir = j.value("sdk_dir", "");
            dll = j.value("dll", "");
            python = j.value("python", "py");
            fileRepo = j.value("repo", "");
            gameDir = j.value("game_dir", "");
            if (j.contains("recent") && j["recent"].is_array()) for (auto& r : j["recent"]) recent.push_back(r);
        } catch (...) {}
    }
    char* env = nullptr;
    _dupenv_s(&env, &n, "HAIRWORKS_SDK_DIR");
    if (env && *env) sdkDir = env;
    free(env);
    // Python for the pipeline: the release bundles a private embedded CPython (python\python.exe next to the exe) and uses it
    // unless the setting names another command; dev builds keep using the "py" launcher.
    if (python.empty()) python = "py";
    {
        std::string bundledPy = exeDir() + "\\python\\python.exe";
        if ((python.empty() || python == "py") && fs::exists(bundledPy)) python = "\"" + bundledPy + "\"";
    }
    if (repo.empty()) {
        // release folder: the pipeline is bundled next to the exe, so that folder is the repo.
        // dev build: exe in <repo>\desktop_studio\build\<config>\, walk up
        fs::path p = exeDir();
        if (fs::exists(p / "tools" / "hairworks" / "hairpipe.py") && fs::exists(p / "desktop_studio" / "tools" / "desk_prep.py")) {
            repo = narrowW(p.wstring());
            bundled = true;
        } else {
            for (int k = 0; k < 6 && !p.empty(); k++, p = p.parent_path())
                if (fs::exists(p / "tools" / "hairworks")) { repo = narrowW(p.wstring()); break; }
        }
    }
    if (repo.empty()) repo = !fileRepo.empty() ? fileRepo : HW_REPO_ROOT;
    if (sdkDir.empty()) sdkDir = HW_DEFAULT_SDK_DIR;
    if (sdkDir.empty() || !fs::exists(sdkDir)) sdkDir = repo + "\\sources\\HairWorks-r1_2_1-15-distro\\HairWorks";
    // game folder: HWFA_GAME env, the saved setting, the repo's parent (dev layout), then the usual install folders
    auto isGame = [](const fs::path& g) { std::error_code ec; return !g.empty() && fs::is_directory(g / "content" / "content0", ec); };
    char* ge = nullptr;
    _dupenv_s(&ge, &n, "HWFA_GAME");
    if (ge && *ge) gameDir = ge;
    free(ge);
    if (gameDir.empty() || !isGame(gameDir)) {
        std::vector<fs::path> cand = {fs::path(repo).parent_path(),
            "C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Witcher 3", "C:\\Program Files (x86)\\GOG Galaxy\\Games\\The Witcher 3 Wild Hunt GOTY",
            "C:\\GOG Games\\The Witcher 3 Wild Hunt GOTY", "C:\\Program Files\\Epic Games\\TheWitcher3", "D:\\SteamLibrary\\steamapps\\common\\The Witcher 3",
            "D:\\Games\\The Witcher 3 Wild Hunt GOTY", "G:\\Games\\The Witcher 3 Wild Hunt GOTY"};
        for (auto& c : cand) if (isGame(c)) { gameDir = c.string(); break; }
    }
    if (!gameDir.empty()) _putenv_s("HWFA_GAME", gameDir.c_str());     // child processes (the Python tools) inherit it
}

void Settings::addRecent(const std::string& r) {
    recent.erase(std::remove(recent.begin(), recent.end(), r), recent.end());
    recent.insert(recent.begin(), r);
    if (recent.size() > 10) recent.resize(10);
    save();
}

void Settings::save() const {
    fs::create_directories(fs::path(file).parent_path());
    json j = {{"sdk_dir", sdkDir}, {"dll", dll}, {"python", python == "\"" + exeDir() + "\\python\\python.exe\"" ? std::string() : python}, {"repo", bundled ? std::string() : repo}, {"game_dir", gameDir}, {"recent", recent}};
    std::ofstream(file) << j.dump(2);
}

// ================================================================================================ camera
XMMATRIX Camera::view() const {
    XMFLOAT3 e = eye();
    return XMMatrixLookAtRH(XMLoadFloat3(&e), XMLoadFloat3(&target), XMVectorSet(0, 0, 1, 0));
}
XMFLOAT3 Camera::eye() const {
    return {target.x + dist * std::cos(pitch) * std::sin(yaw), target.y + dist * std::cos(pitch) * std::cos(yaw),
            target.z + dist * std::sin(pitch)};
}

// ================================================================================================ init / open
bool App::init(HWND hwnd) {
    std::string err;
    if (!gfx.init(hwnd, err)) { logf("%s", err.c_str()); if (!headless) MessageBoxA(hwnd, err.c_str(), "Groom Studio", MB_ICONERROR); return false; }
    std::string shaderDir = exeDir();
    if (!gfx.loadShaders(shaderDir, err)) { logf("%s", err.c_str()); if (!headless) MessageBoxA(hwnd, err.c_str(), "Groom Studio", MB_ICONERROR); return false; }
    logf("repo: %s", set.repo.c_str());
    logf("HairWorks SDK folder: %s", set.sdkDir.c_str());
    std::string dll = set.dllPath();
    if (!fs::exists(dll)) {
        status = "HairWorks runtime not found: " + dll + ". Set the SDK folder in Settings.";
    } else if (!hw.load(dll, gfx.dev, gfx.ctx)) {
        status = "HairWorks load failed: " + hw.lastError;
    } else {
        auto cso = readFile(shaderDir + "\\hair_ps.cso");
        if (cso.empty() || !hw.loadPixelShader(cso)) status = "hair_ps.cso missing or invalid next to the exe";
        else { sdkOk = true; status = "HairWorks runtime loaded"; }
        // second runtime object for the beard fur (same DLL, its own SDK object)
        if (sdkOk && hwBeard.load(dll, gfx.dev, gfx.ctx) && hwBeard.loadPixelShader(cso)) {
            beardSdkOk = true;
            logf("beard runtime: %s SDK object", hwBeard.sdkPtr() == hw.sdkPtr() ? "shared" : "separate");
        }
        else logf("beard runtime not available: %s", hwBeard.lastError.c_str());
    }
    logf("%s (%s)", status.c_str(), dll.c_str());
    // logos: ours (assets) and the GameWorks one read from the user's SDK copy at run time only
    std::string logo = exeDir() + "\\assets\\hairwork_for_all_logo.jpg";
    if (!fs::exists(logo)) logo = set.repo + "\\desktop_studio\\assets\\hairwork_for_all_logo.jpg";
    if (fs::exists(logo)) logoSrv = gfx.loadTexture(logo, false, &logoW, &logoH);
    // NVIDIA GameWorks logo (EULA 6.B.f): embedded in the exe as RCDATA 201 at build time (CMakeLists.txt), so every
    // build shows it whatever SDK folder the user points to. The bytes go through a temp file for the texture loader.
    if (HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(201), MAKEINTRESOURCEW(10))) {   // 10 = RT_RCDATA
        HGLOBAL g = LoadResource(nullptr, r);
        const void* p = g ? LockResource(g) : nullptr;
        DWORD n = SizeofResource(nullptr, r);
        if (p && n) {
            std::string tmp = (fs::temp_directory_path() / "hairwork_studio_gameworks_logo.png").string();
            if (FILE* f = fopen(tmp.c_str(), "wb")) {
                fwrite(p, 1, n, f);
                fclose(f);
                gwLogoSrv = gfx.loadTexture(tmp, false, &gwW, &gwH);
            }
        }
    }
    if (!gwLogoSrv) logf("NVIDIA GameWorks logo resource could not be loaded: the text attribution is shown instead");
    if (!headless) loadCatalog();
    return true;
}

void App::listConfigs() {
    configs.clear();
    std::string out;
    runCapture(set.python + " \"" + set.repo + "\\desktop_studio\\tools\\desk_prep.py\" --list", set.repo, out);
    std::istringstream s(out);
    std::string line;
    while (std::getline(s, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() > 2 && (line[0] == '*' || line[0] == ' ')) configs.push_back(line);
    }
    logf("%d built configs found (* = approved)", (int)configs.size());
}

bool App::openConfig(const std::string& cfg, bool rebuildPrep) {
    std::string dir = set.repo + "\\build\\desktop_studio\\" + cfg;
    bool needPrep = rebuildPrep || !packUpToDate(cfg);
    if (needPrep) {
        std::string out;
        std::string cmd = set.python + " \"" + set.repo + "\\desktop_studio\\tools\\desk_prep.py\" " + cfg;
        logf("> %s", cmd.c_str());
        int rc = runCapture(cmd, set.repo, out);
        std::istringstream s(out);
        std::string l;
        while (std::getline(s, l)) logf("  %s", l.c_str());
        if (rc != 0) { status = "Could not read the build of " + cfg + " (see Log)"; return false; }
    }
    DeskPack p;
    std::string err;
    if (!p.load(dir, err)) { status = err; logf("%s", err.c_str()); return false; }
    json raw;
    try { std::ifstream f(dir + "\\desk.json"); raw = json::parse(f); } catch (...) {}
    pack = std::move(p);
    pack.shots = raw.value("shots", json::object());
    groom.init(&pack);
    look.init(&pack);
    look.enforceCaps();
    presets = loadPresets(pack, set.repo + "\\desktop_studio\\presets", raw.value("materials", json::object()));
    presetSel = 0;
    smap.reset(pack.guides.size());
    stiffDirty = true;
    sel.clear();
    configName = cfg;
    // hairstyle of the build (optional "style" key of tools/hairworks/chars/<cfg>.json; missing = the default style):
    // shown in the title-bar breadcrumb, and exports of another style are named <char>_<style>_<tag>
    styleId = "default";
    try {
        std::ifstream cf(set.repo + "\\tools\\hairworks\\chars\\" + cfg + ".json");
        if (cf) { json cj = json::parse(cf); if (cj.contains("style") && cj["style"].is_string() && !cj["style"].get<std::string>().empty()) styleId = cj["style"].get<std::string>(); }
    } catch (...) {}
    snprintf(modName, sizeof modName, "%s", pack.mod.value("mod", std::string("modHairworks")).c_str());
    safeRelease(densitySrv);
    if (!pack.densityFile.empty()) densitySrv = gfx.loadTexture(dir + "\\" + pack.densityFile, false);
    hw.setDensityTexture(densitySrv);
    for (auto& kv : meshTex) safeRelease(kv.second);
    meshTex.clear();
    for (auto& m : pack.meshes) {
        if (!m.texFile.empty() && !meshTex.count(m.texFile)) meshTex[m.texFile] = gfx.loadTexture(dir + "\\" + m.texFile, true);
        if (!m.nrmFile.empty() && !meshTex.count(m.nrmFile)) meshTex[m.nrmFile] = gfx.loadTexture(dir + "\\" + m.nrmFile, false);
    }
    safeRelease(colorSrv);
    if (!pack.colorFile.empty()) colorSrv = gfx.loadTexture(dir + "\\" + pack.colorFile, true);
    srcDensity.clear();
    srcColor.clear();
    auto cpuCopy = [&](const std::string& f, std::vector<unsigned char>& v, int& w, int& h) {
        if (f.empty()) return;
        unsigned char* p = loadImage(dir + "\\" + f, &w, &h);
        if (p) { v.assign(p, p + size_t(w) * h * 4); freeImage(p); }
    };
    cpuCopy(pack.densityFile, srcDensity, srcDW, srcDH);
    cpuCopy(pack.colorFile, srcColor, srcCW, srcCH);
    hw.setColorTexture(colorSrv);
    cam.target = {pack.headC.x, pack.headC.y, pack.headC.z - 0.05f};
    setView(args.view);
    camShot = -1;
    loaded = true;
    savedEdits = 0;
    rebuildHair();
    buildBeard();
    status = "Opened " + cfg + " (" + std::to_string(pack.guides.size()) + " guides, " + std::to_string(pack.F.size()) + " faces)";
    pickerOpen = false;
    set.addRecent(cfg);
    logf("%s", status.c_str());
    return true;
}

void App::setView(const std::string& v) {
    // the character faces +Y
    if (v == "front") cam.yaw = 0.0f;
    else if (v == "back") cam.yaw = 3.14159f;
    else if (v == "left") cam.yaw = -1.5708f;
    else if (v == "right") cam.yaw = 1.5708f;
    else if (v == "34") cam.yaw = 0.75f;
    else if (v == "back34") cam.yaw = 2.4f;
    cam.pitch = 0.12f;
    cam.dist = 0.95f;
}


static ID3D11ShaderResourceView* makeSrv(Gfx& g, const std::vector<unsigned char>& px, int size, bool srgb) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = size;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* t = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &t))) {
        g.ctx->UpdateSubresource(t, 0, nullptr, px.data(), size * 4, 0);
        g.dev->CreateShaderResourceView(t, nullptr, &srv);
        if (srv) g.ctx->GenerateMips(srv);
        t->Release();
    }
    return srv;
}

// unique UVs: the atlas layout for the current faces + density / colour re-baked into it (preview == export)
void App::bakeAtlas() {
    atlas.build(groom);
    safeRelease(bakedDensitySrv);
    safeRelease(bakedColorSrv);
    if (!srcDensity.empty()) {
        int ch = pack.densityChannel == "ALPHA" ? 3 : pack.densityChannel == "GREEN" ? 1 : pack.densityChannel == "BLUE" ? 2 : 0;
        atlas.bake(groom, srcDensity.data(), srcDW, srcDH, ch, bakedDensity);
        bakedDensitySrv = makeSrv(gfx, bakedDensity, atlas.size, false);
    }
    if (!srcColor.empty()) {
        atlas.bake(groom, srcColor.data(), srcCW, srcCH, -1, bakedColor);
        bakedColorSrv = makeSrv(gfx, bakedColor, atlas.size, true);
    }
}

void App::rebuildHair() {
    computeBudget();
    if (!sdkOk || !loaded) return;
    atlasOn = smap.painted;
    if (atlasOn) bakeAtlas();
    hw.setDensityTexture(atlasOn && bakedDensitySrv ? bakedDensitySrv : densitySrv);
    hw.setColorTexture(atlasOn && bakedColorSrv ? bakedColorSrv : colorSrv);
    if (!hw.build(pack, groom, guideOfAsset, atlasOn ? &atlas.uv : nullptr)) { status = "HairWorks: " + hw.lastError; logf("%s", status.c_str()); }
    stiffDirty = true;
    teleport = true;
    assetDirty = false;
    lastBuild = headless ? 0 : ImGui::GetTime();
}

void App::buildBeard() {
    beardOn = false;
    if (!beardSdkOk || !pack.beardPack(beardPk)) return;
    beardGroom.init(&beardPk);
    beardLook.init(&beardPk);
    hwBeard.setDensityTexture(nullptr);
    hwBeard.setColorTexture(nullptr);
    if (!hwBeard.build(beardPk, beardGroom, beardGuideOfAsset)) { logf("beard fur: %s", hwBeard.lastError.c_str()); return; }
    beardOn = true;
    logf("beard fur: %d guides, %d faces (second HairWorks instance)", (int)beardPk.guides.size(), (int)beardPk.F.size());
}

void App::updateStiffnessTexture() {
    smap.rasterise(groom);                              // decides painted / not painted
    if (smap.painted != atlasOn) { assetDirty = true; rebuildHair(); }   // switch to / from unique UVs
    smap.size = 1024;
    smap.rasterise(groom, atlasOn ? &atlas.uv : nullptr);
    if (stiffTex) {
        D3D11_TEXTURE2D_DESC cur;
        stiffTex->GetDesc(&cur);
        if ((int)cur.Width != smap.size) { safeRelease(stiffSrv); safeRelease(stiffTex); }
    }
    if (!stiffTex) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = smap.size;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        gfx.dev->CreateTexture2D(&td, nullptr, &stiffTex);
        gfx.dev->CreateShaderResourceView(stiffTex, nullptr, &stiffSrv);
    }
    gfx.ctx->UpdateSubresource(stiffTex, 0, nullptr, smap.rgba.data(), smap.size * 4, 0);
    hw.setStiffnessTexture(smap.painted ? stiffSrv : nullptr);
    stiffDirty = false;
}

void App::computeBudget() {
    if (!loaded) return;
    double mean = 0;
    for (float v : pack.densityFace) mean += v;
    mean = pack.densityFace.empty() ? 1.0 : mean / pack.densityFace.size();
    std::map<std::array<int, 3>, int> orig;
    for (size_t f = 0; f < pack.F.size(); f++) orig[pack.F[f]] = (int)f;
    sumFaceDensity = 0;
    for (auto& f : groom.faces) {
        auto it = orig.find(f.g);
        sumFaceDensity += (it != orig.end() && !pack.densityFace.empty()) ? pack.densityFace[it->second] : mean;
    }
    bud = look.budget((int)groom.faces.size(), sumFaceDensity, pack.K);
}

void App::editsChanged() {
    assetDirty = true;
    computeBudget();
}

// ================================================================================================ simulation step
void App::step(double dt) {
    fps = fps * 0.95f + 0.05f * float(1.0 / std::max(dt, 1e-4));
    if (!loaded) return;
    if (assetDirty && (!dragging || headless || ImGui::GetTime() - lastBuild > 0.12)) rebuildHair();
    if (stiffDirty && sdkOk) updateStiffnessTexture();
    if (turntable && !camOverride) cam.yaw += float(dt) * 0.45f;
    if (!paused) motion.t += dt;
    else if (stepOnce) motion.t += 1.0 / 60.0;
    motion.windOn = windOn;
    hw.visBones = showBones;
    std::vector<XMFLOAT4X4>& M = boneM;
    motion.boneMatrices(pack, M);
    V3 w = motion.wind();
    hw.updateDescriptor(look, {w.x, w.y, w.z}, simulate, sdkGuides, showCollision, showGrowth, forceCloseLod, colorize,
                        smap.painted);
    hw.skin(M, teleport);
    if (beardOn) {
        // beard bones follow the head (face bones), the neck or the body, from the hair pack's motion
        XMMATRIX H = motion.headM(pack), Nk = motion.neckM(pack), Rt = motion.rootM(pack);
        std::vector<XMFLOAT4X4> Mb(beardPk.bones.size());
        for (size_t i = 0; i < beardPk.bones.size(); i++) {
            const std::string& n = beardPk.bones[i].name;
            XMMATRIX B = H;
            if (n.rfind("torso", 0) == 0 || n.find("shoulder") != std::string::npos || n == "pelvis") B = Rt;
            else if (n == "neck" || n == "neck2" || n == "thyroid") B = Nk;
            XMStoreFloat4x4(&Mb[i], B);
        }
        hwBeard.updateDescriptor(beardLook, {w.x, w.y, w.z}, simulate, false, false, false, forceCloseLod, colorize, false);
        hwBeard.skin(Mb, teleport);
    }
    teleport = false;
    if (!paused && simulate) {
        simAcc += dt;
        int n = 0;
        while (simAcc >= 1.0 / 60.0 && n < 4) {
            hw.step(1.0f / 60.0f);
            if (beardOn) hwBeard.step(1.0f / 60.0f);
            simAcc -= 1.0 / 60.0;
            n++;
        }
        if (n == 4) simAcc = 0;
    } else if (stepOnce || !simulate) {
        hw.step(1.0f / 60.0f);
        if (beardOn) hwBeard.step(1.0f / 60.0f);
    }
    stepOnce = false;
}

// ================================================================================================ rendering
static void meshVerts(const App& a, std::vector<GVertex>& out, bool textured, int only = -1) {
    out.clear();
    const DeskPack& pk = a.pack;
    XMMATRIX H = a.motion.headM(pk), Nk = a.motion.neckM(pk), R = a.motion.rootM(pk);
    float neckZ = 1.62f;
    int nb = pk.boneIndex("neck");
    if (nb >= 0) neckZ = pk.bones[nb].bind[14];
    for (size_t mi = 0; mi < pk.meshes.size(); mi++) {
        const Mesh& m = pk.meshes[mi];
        if (only >= 0 && int(mi) != only) continue;
        // alpha-tested beard cards: drawn one by one (own blend / depth shader), never in the merged lists or the mask
        // pass, and hidden when the beard fur replaces them or the beard is switched off
        if (m.alphaTest && (only < 0 || a.maskPass || a.beardOn || !a.showBeard)) continue;
        bool tex = !m.texFile.empty() && m.uv.size() == m.v.size() && !a.maskPass;
        if (tex != textured) continue;
        auto itN = m.nrmFile.empty() ? a.meshTex.end() : a.meshTex.find(m.nrmFile);
        float texMode = (itN != a.meshTex.end() && itN->second) ? 3.0f : 2.0f;   // 3 = with normal map
        bool head = m.kind == "head" || m.kind == "hat";
        // material class for the mesh shader: 0 cloth / leather / metal, 1 skin, 2 eyeball, 3 mouth (teeth / tongue)
        auto has = [&](const char* k) { return m.texFile.find(k) != std::string::npos; };
        float cls = m.alphaTest ? 4.0f : has("teeth") || has("tongue") || has("mouth") ? 3.0f : has("eye") ? 2.0f
                  : (m.kind == "head" || m.name.rfind("g_", 0) == 0 || has("man_average") || has("woman_average")) ? 1.0f : 0.0f;
        // skin / cloth albedo (linear) close to the HairLab captures' average
        XMFLOAT4 col = a.maskPass ? XMFLOAT4(0, 0, 0, 1) : m.kind == "head" ? XMFLOAT4(0.20f, 0.11f, 0.08f, 1) : XMFLOAT4(0.035f, 0.034f, 0.033f, 1);
        std::vector<XMFLOAT3> P(m.v.size());
        for (size_t i = 0; i < m.v.size(); i++) {
            XMVECTOR p = XMVectorSet(m.v[i].x, m.v[i].y, m.v[i].z, 1);
            float hz = head ? 1.0f : std::clamp((m.v[i].z - (neckZ - 0.03f)) / 0.08f, 0.0f, 1.0f);
            XMVECTOR q = hz >= 1.0f ? XMVector3TransformCoord(p, H)
                                    : XMVectorLerp(XMVector3TransformCoord(p, R), XMVector3TransformCoord(p, Nk), hz);
            XMStoreFloat3(&P[i], q);
        }
        // smooth (area-weighted vertex) normals: the decimated meshes look faceted with face normals
        std::vector<XMVECTOR> N(P.size(), XMVectorZero());
        for (auto& t : m.t) {
            XMVECTOR a0 = XMLoadFloat3(&P[t[0]]), a1 = XMLoadFloat3(&P[t[1]]), a2 = XMLoadFloat3(&P[t[2]]);
            XMVECTOR fn = XMVector3Cross(XMVectorSubtract(a1, a0), XMVectorSubtract(a2, a0));
            for (int c = 0; c < 3; c++) N[t[c]] = XMVectorAdd(N[t[c]], fn);
        }
        for (auto& t : m.t)
            for (int c = 0; c < 3; c++) {
                XMFLOAT3 n;
                XMStoreFloat3(&n, XMVector3Normalize(N[t[c]]));
                out.push_back({P[t[c]], n, tex ? XMFLOAT4(m.uv[t[c]][0], m.uv[t[c]][1], cls, texMode) : col});
            }
    }
}

void App::drawScene() {
    auto s2l = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    float clear[4] = {s2l(rig.bg[0]), s2l(rig.bg[1]), s2l(rig.bg[2]), 1};
    gfx.wireframe = wireframe;
    float aspect = float(gfx.w) / float(std::max(gfx.h, 1));
    XMMATRIX V, P;
    float fov = cam.fov;
    if (camOverride) {
        V = XMLoadFloat4x4(&ovView);
        P = XMLoadFloat4x4(&ovProj);
        fov = ovFovDeg;
    } else {
        V = cam.view();
        P = XMMatrixPerspectiveFovRH(XMConvertToRadians(cam.fov), aspect, 0.02f, 200.0f);
    }
    XMStoreFloat4x4(&viewM, V);
    XMStoreFloat4x4(&projM, P);
    // ---- key-light shadow map: meshes + hair (SDK shadow pass), ortho box around the character
    float ca0 = std::cos(rig.sunEl);
    XMVECTOR Ldir = XMVector3Normalize(XMVectorSet(ca0 * std::sin(rig.sunAz), ca0 * std::cos(rig.sunAz), std::sin(rig.sunEl), 0));
    XMVECTOR ctr = loaded ? XMVectorSet(pack.headC.x, pack.headC.y, 1.05f, 1) : XMVectorSet(0, 0, 1.05f, 1);
    XMVECTOR up = std::fabs(std::sin(rig.sunEl)) > 0.97f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);
    XMMATRIX LV = XMMatrixLookAtRH(XMVectorAdd(ctr, XMVectorScale(Ldir, 3.0f)), ctr, up);
    const float lnear = 0.1f, lfar = 6.0f, lbox = 2.3f;
    XMMATRIX LP = XMMatrixOrthographicRH(lbox, lbox, lnear, lfar);
    XMFLOAT4X4 lightVP, lightV, lightP;
    XMStoreFloat4x4(&lightVP, LV * LP);
    XMStoreFloat4x4(&lightV, LV);
    XMStoreFloat4x4(&lightP, LP);
    bool shadowsOn = rig.shadows && loaded && rig.shadowStrength > 0.0f && rig.sun > 0.0f;
    std::vector<GVertex> depthTris[2];
    if (loaded && showMeshes) {
        meshVerts(*this, depthTris[0], false);
        meshVerts(*this, depthTris[1], true);
    }
    // alpha-tested beard cards in a depth pass: only their opaque texels cast shadow / occlude the sky
    auto drawCardsDepth = [&]() {
        if (!loaded || !showMeshes) return;
        std::vector<GVertex> t;
        for (size_t mi = 0; mi < pack.meshes.size(); mi++) {
            const Mesh& m = pack.meshes[mi];
            if (!m.alphaTest) continue;
            auto it = meshTex.find(m.texFile);
            if (it == meshTex.end() || !it->second) continue;
            meshVerts(*this, t, true, int(mi));
            gfx.drawTrisDepthAlpha(t, it->second);
        }
    };
    if (shadowsOn) {
        gfx.bindShadow(false);
        gfx.beginShadowPass();
        FrameCB fs = {};
        fs.viewProj = lightVP;
        fs.color = {1, 1, 1, 1};
        gfx.setFrame(fs);
        gfx.drawTrisDepth(depthTris[0]);
        gfx.drawTrisDepth(depthTris[1]);
        drawCardsDepth();
        if (showHair && sdkOk) hw.renderShadow(lightV, lightP, float(gfx.shadowSize));
        if (showHair && beardOn && showBeard) hwBeard.renderShadow(lightV, lightP, float(gfx.shadowSize));
    }
    // ---- sky occlusion: one orthographic depth map per sky direction (meshes + hair), 1.5 m around the head and
    // shoulders. Lighting quality 1 = 8 directions, 2 = 16 (README "Rendering").
    int nAo = (!loaded || rig.quality <= 0 || maskPass) ? 0 : (rig.quality == 1 ? 8 : 16);
    XMFLOAT4X4 aoVP[kMaxAoDirs];
    const float aoBox = 1.5f, aoNear = 0.1f, aoFar = 3.2f;
    XMVECTOR aoC = loaded ? XMVectorSet(pack.headC.x, pack.headC.y, pack.headC.z - 0.30f, 1) : XMVectorSet(0, 0, 1.45f, 1);
    // the maps are static in the head frame (fixed box, fixed directions), so the viewport refreshes 4 a frame
    // (16 maps = every 4th frame, ~67 ms at 60 fps); shots and a change of map count refresh all of them
    const int kAoPerFrame = 4;
    bool aoAll = aoRefreshAll || nAo != aoCount;
    int aoN = aoAll ? nAo : std::min(kAoPerFrame, nAo);
    for (int k = 0; k < nAo; k++) {
        float d[3];
        aoDirection(k, nAo, d);
        XMVECTOR D0 = XMVectorSet(d[0], d[1], d[2], 0);
        XMVECTOR up0 = std::fabs(d[2]) > 0.95f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);
        XMStoreFloat4x4(&aoVP[k], XMMatrixLookAtRH(XMVectorAdd(aoC, XMVectorScale(D0, 1.6f)), aoC, up0) *
                                      XMMatrixOrthographicRH(aoBox, aoBox, aoNear, aoFar));
    }
    for (int j = 0; j < aoN; j++) {
        int k = aoAll ? j : (aoNext + j) % nAo;
        float d[3];
        aoDirection(k, nAo, d);
        XMVECTOR D = XMVectorSet(d[0], d[1], d[2], 0);
        XMVECTOR upk = std::fabs(d[2]) > 0.95f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);
        XMMATRIX KV = XMMatrixLookAtRH(XMVectorAdd(aoC, XMVectorScale(D, 1.6f)), aoC, upk);
        XMMATRIX KP = XMMatrixOrthographicRH(aoBox, aoBox, aoNear, aoFar);
        gfx.beginAoPass(k);
        FrameCB fs = {};
        fs.viewProj = aoVP[k];
        fs.color = {1, 1, 1, 1};
        gfx.setFrame(fs);
        gfx.drawTrisDepth(depthTris[0]);
        gfx.drawTrisDepth(depthTris[1]);
        drawCardsDepth();
        if (showHair && sdkOk) {
            XMFLOAT4X4 kv, kp;
            XMStoreFloat4x4(&kv, KV);
            XMStoreFloat4x4(&kp, KP);
            hw.renderShadow(kv, kp, float(gfx.aoSize));
            if (beardOn && showBeard) hwBeard.renderShadow(kv, kp, float(gfx.aoSize));
        }
    }
    if (nAo > 0) aoNext = aoAll ? 0 : (aoNext + aoN) % nAo;
    aoCount = nAo;
    aoRefreshAll = false;
    gfx.beginScene(clear);
    gfx.bindShadow(shadowsOn, nAo > 0);
    // backdrop gradient (time-of-day sky), drawn in clip space
    if (rig.backdrop && !maskPass) {
        FrameCB fb = {};
        XMStoreFloat4x4(&fb.viewProj, XMMatrixIdentity());
        fb.color = {1, 1, 1, 1};
        gfx.setFrame(fb);
        XMFLOAT4 ct(s2l(rig.skyTop[0]), s2l(rig.skyTop[1]), s2l(rig.skyTop[2]), 1), cb(s2l(rig.skyBottom[0]), s2l(rig.skyBottom[1]), s2l(rig.skyBottom[2]), 1);
        std::vector<GVertex> q = {{{-1, 1, 0.999f}, {0, 0, 1}, ct}, {{1, 1, 0.999f}, {0, 0, 1}, ct}, {{1, -1, 0.999f}, {0, 0, 1}, cb},
                                  {{-1, 1, 0.999f}, {0, 0, 1}, ct}, {{1, -1, 0.999f}, {0, 0, 1}, cb}, {{-1, -1, 0.999f}, {0, 0, 1}, cb}};
        gfx.drawColor(q);
        // sun (or moon) disc where the key light really is
        float ce = std::cos(rig.sunEl);
        XMVECTOR Ld = XMVectorSet(ce * std::sin(rig.sunAz), ce * std::cos(rig.sunAz), std::sin(rig.sunEl), 0);
        XMMATRIX Vr = V;
        Vr.r[3] = XMVectorSet(0, 0, 0, 1);   // direction only (sky at infinity)
        XMVECTOR cp = XMVector4Transform(XMVectorSetW(Ld, 1), Vr * P);
        float cw = XMVectorGetW(cp);
        if (cw > 1e-4f) {
            float sx = XMVectorGetX(cp) / cw, sy = XMVectorGetY(cp) / cw;
            bool moon = rig.moon;
            float r = moon ? 0.035f : 0.045f, asp = float(gfx.w) / float(std::max(gfx.h, 1));
            float tint = 1.0f / std::max(std::max(rig.sunCol[0], std::max(rig.sunCol[1], rig.sunCol[2])), 1e-3f);
            XMFLOAT4 core(std::min(1.0f, rig.sunCol[0] * tint), std::min(1.0f, rig.sunCol[1] * tint), std::min(1.0f, rig.sunCol[2] * tint), 1);
            if (moon) core = {0.75f, 0.80f, 0.92f, 1};
            XMFLOAT4 halo((core.x + ct.x) * 0.5f, (core.y + ct.y) * 0.5f, (core.z + ct.z) * 0.5f, 1);
            std::vector<GVertex> d;
            auto disc = [&](float rr, XMFLOAT4 c, float z) {
                const int n = 40;
                for (int k = 0; k < n; k++) {
                    float a0 = 6.2832f * k / n, a1 = 6.2832f * (k + 1) / n;
                    d.push_back({{sx, sy, z}, {0, 0, 1}, c});
                    d.push_back({{sx + rr * std::cos(a1) / asp, sy + rr * std::sin(a1), z}, {0, 0, 1}, c});
                    d.push_back({{sx + rr * std::cos(a0) / asp, sy + rr * std::sin(a0), z}, {0, 0, 1}, c});
                }
            };
            disc(r * 1.9f, halo, 0.9985f);
            disc(r, core, 0.998f);
            gfx.drawColor(d);
        }
    }
    XMMATRIX inv = XMMatrixInverse(nullptr, V);
    XMFLOAT3 e;
    XMStoreFloat3(&e, inv.r[3]);
    FrameCB f;
    XMStoreFloat4x4(&f.viewProj, V * P);
    f.camPos = {e.x, e.y, e.z, 1};
    f.color = {1, 1, 1, 1};
    LightCB L;
    float ca = std::cos(rig.sunEl);
    L.sunDir = {ca * std::sin(rig.sunAz), ca * std::cos(rig.sunAz), std::sin(rig.sunEl), rig.sun};
    L.sunCol = {rig.sunCol[0], rig.sunCol[1], rig.sunCol[2], 0};
    L.skyCol = {rig.skyCol[0], rig.skyCol[1], rig.skyCol[2], rig.sky};
    L.groundCol = {rig.groundCol[0], rig.groundCol[1], rig.groundCol[2], rig.ground};
    L.params = {rig.exposure, rig.colourGain, rig.specGain, rig.selfShadow};
    L.head = loaded ? XMFLOAT4(pack.headC.x, pack.headC.y, pack.headC.z, 0.095f) : XMFLOAT4(0, 0, 1.75f, 0.095f);
    L.material = {loaded ? look.f("graphicalMaterials.color.ambientEnvScale", 0.35f) : 0.35f, rig.floor,
                  (loaded ? look.f("graphicalMaterials.color.textureBrightness", 1.0f) : 1.0f) * rig.texGain, colorSrv ? 1.0f : 0.0f};
    L.extra = {loaded ? look.f("graphicalMaterials.diffuse.diffuseNoiseScale", 0.0f) : 0.0f, rig.strandVar, rig.msSat, rig.msSatAmb};
    L.lightVP = lightVP;
    L.shadow = {shadowsOn ? rig.shadowStrength : 0.0f, 0.0004f, 1.0f / gfx.shadowSize, rig.shadowSoft};
    // penumbra: texels per metre of blocker - receiver distance = tan(sun angular radius) / texel size
    L.shadow2 = {lfar - lnear, float(hairDebug), rig.specPower, std::tan(rig.sunSize * 0.0174533f) / (lbox / gfx.shadowSize)};
    fillEnvironment(rig, L, nAo);
    L.skyHor.w = float(rig.toneMode);
    L.skin = {rig.skinSss, rig.skinSpec, rig.skinGain, rig.cavity};
    L.ao = {float(nAo), rig.aoStrength, aoFar - aoNear, 1.0f / gfx.aoSize};
    L.hair2 = {rig.sigmaScale, rig.msSpread, rig.ttGain, rig.envSpec};
    L.hair3 = {rig.hairEncoding, 0, 0, 0};
    for (int k = 0; k < kMaxAoDirs; k++) L.aoVP[k] = k < nAo ? aoVP[k] : lightVP;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(gfx.ctx->Map(gfx.lightCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &L, sizeof L); gfx.ctx->Unmap(gfx.lightCb, 0); }
    gfx.setFrame(f);
    if (!loaded) { if (!camOverride) gfx.endScene(); return; }
    std::vector<GVertex> lines;
    if (!camOverride) {
        for (int i = -10; i <= 10; i++) {
            XMFLOAT4 c(0.16f, 0.17f, 0.18f, 1);
            lines.push_back({{i * 0.1f, -1, 0}, {0, 0, 1}, c});
            lines.push_back({{i * 0.1f, 1, 0}, {0, 0, 1}, c});
            lines.push_back({{-1, i * 0.1f, 0}, {0, 0, 1}, c});
            lines.push_back({{1, i * 0.1f, 0}, {0, 0, 1}, c});
        }
        gfx.drawLines(lines, true);
        // subtle ground disc (receives the key-light shadow)
        std::vector<GVertex> disc;
        XMFLOAT4 gc(0.05f, 0.05f, 0.052f, 1);
        float cx = pack.headC.x, cy = pack.headC.y;
        for (int k = 0; k < 64; k++) {
            float a0 = k * 6.2831853f / 64, a1 = (k + 1) * 6.2831853f / 64;
            disc.push_back({{cx, cy, 0.001f}, {0, 0, 1}, gc});
            disc.push_back({{cx + 0.9f * std::cos(a0), cy + 0.9f * std::sin(a0), 0.001f}, {0, 0, 1}, gc});
            disc.push_back({{cx + 0.9f * std::cos(a1), cy + 0.9f * std::sin(a1), 0.001f}, {0, 0, 1}, gc});
        }
        bool w = gfx.wireframe;
        gfx.wireframe = false;
        gfx.drawTris(disc);
        gfx.wireframe = w;
    }
    if (showMeshes) {
        std::vector<GVertex> tris;
        meshVerts(*this, tris, false);
        gfx.drawTris(tris);
        for (size_t mi = 0; mi < pack.meshes.size(); mi++) {
            const Mesh& m = pack.meshes[mi];
            if (m.texFile.empty()) continue;
            auto itD = meshTex.find(m.texFile);
            if (itD == meshTex.end() || !itD->second) continue;
            auto itN = m.nrmFile.empty() ? meshTex.end() : meshTex.find(m.nrmFile);
            tris.clear();
            meshVerts(*this, tris, true, int(mi));
            gfx.drawTris(tris, itD->second, itN == meshTex.end() ? nullptr : itN->second, m.alphaTest);
        }
    }
    if (showHair && sdkOk) {
        gfx.ctx->OMSetRenderTargets(1, &gfx.msaaRtv, gfx.dsv);
        gfx.ctx->OMSetBlendState(gfx.bsOpaque, nullptr, 0xffffffff);
        gfx.ctx->OMSetDepthStencilState(gfx.dsOn, 0);
        gfx.ctx->RSSetState(gfx.rsSolid);
        hw.render(viewM, projM, fov, float(gfx.w), float(gfx.h), gfx.lightCb);
        if (beardOn && showBeard && !maskPass) {
            // the beard fur's own material values in the shared light constants (constant colours, no card texture)
            LightCB Lb = L;
            Lb.material = {beardLook.f("graphicalMaterials.color.ambientEnvScale", 0.35f), rig.floor, 1.0f, 0.0f};
            Lb.extra.x = beardLook.f("graphicalMaterials.diffuse.diffuseNoiseScale", 0.0f);
            D3D11_MAPPED_SUBRESOURCE mb;
            if (SUCCEEDED(gfx.ctx->Map(gfx.lightCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mb))) { memcpy(mb.pData, &Lb, sizeof Lb); gfx.ctx->Unmap(gfx.lightCb, 0); }
            hwBeard.render(viewM, projM, fov, float(gfx.w), float(gfx.h), gfx.lightCb);
            if (SUCCEEDED(gfx.ctx->Map(gfx.lightCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mb))) { memcpy(mb.pData, &L, sizeof L); gfx.ctx->Unmap(gfx.lightCb, 0); }
        }
        gfx.ctx->OMSetRenderTargets(1, &gfx.msaaRtv, gfx.dsv);
    }
    // guide overlay (rest pose = what you edit), stiffness paint shows the painted value per guide root
    lines.clear();
    bool overlay = showGuideOverlay || (tool != Tool::Select && tool != Tool::Paint);
    for (size_t i = 0; i < groom.P.size() && !camOverride; i++) {
        if (!groom.alive[i]) continue;
        bool s = sel.count((int)i) > 0, ed = groom.edited.count((int)i) > 0;
        if (!overlay && !s && tool != Tool::Paint) continue;
        XMFLOAT4 c = s ? XMFLOAT4(1.0f, 0.62f, 0.2f, 1) : ed ? XMFLOAT4(0.95f, 0.85f, 0.7f, 0.7f) : XMFLOAT4(0.6f, 0.65f, 0.7f, 0.35f);
        int kmax = pack.K;
        if (tool == Tool::Paint) {
            float v = i < smap.guideS.size() ? (paintTarget == 2 ? smap.guideR[i] : smap.guideS[i]) : 1.0f;
            c = XMFLOAT4(0.2f + 0.8f * (1 - v), 0.35f + 0.5f * v, 0.9f * v, 0.9f);   // soft = warm, stiff = cool
            kmax = std::min(pack.K, 6);
        }
        Guide g(pack.K);
        for (int k = 0; k < pack.K; k++) g[k] = posedPoint((int)i, k);    // the overlay sits on the hair you see
        for (int k = 0; k + 1 < kmax; k++) {
            lines.push_back({{g[k].x, g[k].y, g[k].z}, {0, 0, 1}, c});
            lines.push_back({{g[k + 1].x, g[k + 1].y, g[k + 1].z}, {0, 0, 1}, c});
        }
    }
    // symmetry plane (mid-sagittal plane of the head)
    if (showSymPlane && !camOverride) {
        std::vector<GVertex> pl;
        float px = groom.mirrorPlane(), y0 = pack.headC.y - 0.25f, y1 = pack.headC.y + 0.25f, z0 = pack.headC.z - 0.35f, z1 = pack.headC.z + 0.25f;
        XMFLOAT4 c(0.94f, 0.63f, 0.25f, 0.35f);
        for (int k = 0; k <= 10; k++) {
            float y = y0 + (y1 - y0) * k / 10, z = z0 + (z1 - z0) * k / 10;
            pl.push_back({{px, y, z0}, {1, 0, 0}, c}); pl.push_back({{px, y, z1}, {1, 0, 0}, c});
            pl.push_back({{px, y0, z}, {1, 0, 0}, c}); pl.push_back({{px, y1, z}, {1, 0, 0}, c});
        }
        gfx.drawLines(pl, true);
    }
    gfx.drawLines(lines, false);
    gfx.bindShadow(false);
    if (!camOverride) gfx.endScene();
}

// ================================================================================================ editing helpers
XMFLOAT2 App::project(V3 p) const {
    XMMATRIX vp = XMLoadFloat4x4(&viewM) * XMLoadFloat4x4(&projM);
    XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), vp);
    float w = XMVectorGetW(c);
    if (w <= 1e-5f) return {-1e6f, -1e6f};
    return {(XMVectorGetX(c) / w * 0.5f + 0.5f) * gfx.w, (1.0f - (XMVectorGetY(c) / w * 0.5f + 0.5f)) * gfx.h};
}

int App::pickGuide(float mx, float my) const {
    int best = -1;
    float bd = 12.0f * 12.0f;
    for (size_t i = 0; i < groom.P.size(); i++) {
        if (!groom.alive[i]) continue;
        for (int k = 0; k < pack.K; k += 2) {
            XMFLOAT2 s = project(posedPoint((int)i, k));
            float d = (s.x - mx) * (s.x - mx) + (s.y - my) * (s.y - my);
            if (d < bd) { bd = d; best = (int)i; }
        }
    }
    return best;
}

std::vector<int> App::guidesUnderBrush(float mx, float my, float r) const {
    std::vector<int> out;
    for (size_t i = 0; i < groom.P.size(); i++) {
        if (!groom.alive[i]) continue;
        int k0 = tool == Tool::Paint ? 0 : pack.K / 3;
        int k1 = tool == Tool::Paint ? std::min(pack.K, 4) : pack.K;
        for (int k = k0; k < k1; k++) {
            XMFLOAT2 s = project(posedPoint((int)i, k));
            if ((s.x - mx) * (s.x - mx) + (s.y - my) * (s.y - my) <= r * r) { out.push_back((int)i); break; }
        }
    }
    return out;
}

V3 App::screenDeltaToWorld(float dx, float dy, V3 at) const {
    XMMATRIX inv = XMMatrixInverse(nullptr, XMLoadFloat4x4(&viewM));
    XMFLOAT3 e, right, up;
    XMStoreFloat3(&e, inv.r[3]);
    XMStoreFloat3(&right, inv.r[0]);
    XMStoreFloat3(&up, inv.r[1]);
    float depth = len(at - V3{e.x, e.y, e.z});
    float wpp = 2.0f * depth * std::tan(XMConvertToRadians(cam.fov) * 0.5f) / gfx.h;   // metres per pixel
    return V3{right.x, right.y, right.z} * (dx * wpp) + V3{up.x, up.y, up.z} * (-dy * wpp);
}

// guide point in the current pose: rest point x the bone matrices of the guide's skinning (as HairWorks skins it)
V3 App::posedPoint(int i, int k) const {
    V3 r = groom.renderPoint(i, k);
    if (boneM.empty()) return r;
    int b = groom.baseOf(i);
    const auto& I = pack.boneIdx[b];
    const auto& W = pack.boneW[b];
    XMVECTOR p = XMVectorSet(r.x, r.y, r.z, 1), acc = XMVectorZero();
    float ws = 0;
    for (int c = 0; c < 4; c++) {
        int bi = int(I[c]);
        if (W[c] <= 0 || bi < 0 || bi >= (int)boneM.size()) continue;
        acc = XMVectorAdd(acc, XMVectorScale(XMVector3TransformCoord(p, XMLoadFloat4x4(&boneM[bi])), W[c]));
        ws += W[c];
    }
    if (ws <= 0) return r;
    XMFLOAT3 o;
    XMStoreFloat3(&o, XMVectorScale(acc, 1.0f / ws));
    return {o.x, o.y, o.z};
}

V3 App::restDelta(int i, V3 d) const {
    if (boneM.empty()) return d;
    int b = groom.baseOf(i);
    const auto& I = pack.boneIdx[b];
    const auto& W = pack.boneW[b];
    int best = 0;
    for (int c = 1; c < 4; c++) if (W[c] > W[best]) best = c;   // dominant bone
    int bi = int(I[best]);
    if (bi < 0 || bi >= (int)boneM.size()) return d;
    XMMATRIX inv = XMMatrixInverse(nullptr, XMLoadFloat4x4(&boneM[bi]));
    XMFLOAT3 o;
    XMStoreFloat3(&o, XMVector3TransformNormal(XMVectorSet(d.x, d.y, d.z, 0), inv));
    return {o.x, o.y, o.z};
}

// edit pose: editing tools pause the motion and bring the character back to the bind pose with the hair at rest
void App::enterEditPose() {
    if (editPose || editWhileMoving) return;
    savedPreset = motion.preset;
    savedSimulate = simulate;
    motion.preset = MotionPreset::Rest;
    motion.t = 0;
    simulate = false;
    teleport = true;
    editPose = true;
    status = "Edit pose: motion paused, bind pose, hair at rest (Resume in Physics and motion, or Select tool)";
}

void App::resumeMotion() {
    if (!editPose) return;
    motion.preset = savedPreset;
    simulate = savedSimulate;
    teleport = true;
    editPose = false;
    status = std::string("Motion resumed: ") + motionName(motion.preset);
}

std::vector<int> App::selList() const {
    std::vector<int> v;
    for (int i : sel) if (i < (int)groom.alive.size() && groom.alive[i]) v.push_back(i);
    return v;
}

std::vector<int> App::mirrorOf(const std::vector<int>& ix) const {
    float px = const_cast<Groom&>(groom).mirrorPlane();
    std::vector<int> out;
    for (int i : ix) {
        V3 r = groom.P[i][0];
        if (std::fabs(r.x - px) < 0.004f) continue;                   // on the plane: its own partner
        V3 m{2 * px - r.x, r.y, r.z};
        int best = -1;
        float bd = 0.015f;
        for (size_t j = 0; j < groom.P.size(); j++) {
            if (!groom.alive[j] || (int)j == i) continue;
            float d = len(groom.P[j][0] - m);
            if (d < bd) { bd = d; best = (int)j; }
        }
        if (best >= 0 && std::find(ix.begin(), ix.end(), best) == ix.end() && std::find(out.begin(), out.end(), best) == out.end())
            out.push_back(best);
    }
    return out;
}

void App::opMirror() {
    json op = {{"type", "mirror"}, {"source", mirrorSource == 0 ? "+x" : "-x"}, {"plane_x", "auto"}, {"band", r4(mirrorBandMm / 1000.0f)}};
    if (mirrorYmin > -0.5f) op["ymin"] = mirrorYmin;
    int n0 = groom.numAlive();
    groom.push(op);
    smap.ensure(groom.P.size());
    editsChanged();
    status = "Mirrored the " + std::string(mirrorSource == 0 ? "left (+x)" : "right (-x)") + " side across x = " +
             std::to_string(groom.mirrorPlane()).substr(0, 7) + " (" + std::to_string(n0) + " -> " + std::to_string(groom.numAlive()) + " guides)";
}

void App::opDelete() {
    auto ix = selList();
    if (symmetry) { auto m = mirrorOf(ix); ix.insert(ix.end(), m.begin(), m.end()); }
    if (ix.empty()) { status = "Select guides first"; return; }
    groom.push({{"type", "delete"}, {"roots", groom.rootsOf(ix)}});
    sel.clear();
    editsChanged();
    status = std::to_string(ix.size()) + " guide(s) deleted";
}

void App::opShorten() {
    auto ix = selList();
    if (symmetry) { auto m = mirrorOf(ix); ix.insert(ix.end(), m.begin(), m.end()); }
    if (ix.empty()) { status = "Select guides first"; return; }
    groom.push({{"type", "shorten"}, {"roots", groom.rootsOf(ix)}, {"keep", r4(keepPct / 100.0f)}});
    editsChanged();
    status = std::to_string(ix.size()) + " guide(s) shortened to " + std::to_string(int(keepPct)) + " %";
}

void App::opDuplicate() {
    auto ix = selList();
    if (ix.empty()) { status = "Select guides first"; return; }
    XMMATRIX inv = XMMatrixInverse(nullptr, XMLoadFloat4x4(&viewM));
    XMFLOAT3 r3;
    XMStoreFloat3(&r3, inv.r[0]);
    V3 right{r3.x, r3.y, r3.z}, c = pack.headC;
    float stp = dupStepMm / 1000.0f;
    int n0 = (int)groom.P.size(), id = 0;
    for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    int grp = 1000 + (++grpId);
    for (int i : ix) {
        V3 r = groom.P[i][0], rel = r - c;
        float rl = std::max(len(rel), 1e-6f);
        V3 n = rel * (1.0f / rl);
        V3 t = right - n * dot(right, n);
        t = t * (stp / std::max(len(t), 1e-6f));
        V3 q = rel + t;
        V3 d = q * (rl / std::max(len(q), 1e-6f)) - rel;
        groom.ops.push_back({{"op", "duplicate"}, {"root", {r4(r.x), r4(r.y), r4(r.z)}}, {"d", {r4(d.x), r4(d.y), r4(d.z)}}, {"id", ++id}, {"grp", grp}});
    }
    groom.redo.clear();
    groom.replay();
    sel.clear();
    for (int k = n0; k < (int)groom.P.size(); k++) sel.insert(k);
    smap.ensure(groom.P.size());
    editsChanged();
    status = std::to_string(ix.size()) + " guide(s) duplicated, the copies are selected";
}

void App::opCurl(const std::vector<int>& ix) {
    if (ix.empty()) { status = "Select guides first (or brush over them)"; return; }
    int grp = 2000 + (++grpId);
    std::vector<int> mir = symmetry ? mirrorOf(ix) : std::vector<int>();
    groom.ops.push_back({{"type", "curl"}, {"roots", groom.rootsOf(ix)}, {"params", curl.toJson()}, {"grp", grp}});
    if (!mir.empty()) {
        CurlParams m = curl;
        m.hand = -m.hand;                                  // mirror image of a right-handed helix is left-handed
        groom.ops.push_back({{"type", "curl"}, {"roots", groom.rootsOf(mir)}, {"params", m.toJson()}, {"grp", grp}});
    }
    groom.redo.clear();
    groom.replay();
    editsChanged();
    float cpt = curlCvsPerTurn(pack.K, curl);
    status = std::to_string(ix.size()) + " guide(s) " + curl.mode + (cpt < 6 ? "  -  WARNING: only " + std::to_string(int(cpt)) +
             " CVs per turn (guides have " + std::to_string(pack.K) + " CVs): lower the turns or start earlier" : "");
}

void App::paintAt(float mx, float my) {
    auto g = guidesUnderBrush(mx, my, brushPx);
    if (symmetry) { auto m = mirrorOf(g); g.insert(g.end(), m.begin(), m.end()); }
    smap.ensure(groom.P.size());
    for (int i : g) {
        if (paintTarget != 2) smap.guideS[i] = paintS;
        if (paintTarget != 1) smap.guideR[i] = paintR;
    }
    if (!g.empty()) stiffDirty = true;
}

void App::holdCurlsPhysics() {
    // curls only survive the simulation when the strand is held towards its rest shape all the way to the tip
    auto raise = [&](const char* p, float v) { if (look.f(p, 0) < v) look.setF(p, v); };
    raise("physicalMaterials.stiffness.stiffness", 0.85f);
    raise("physicalMaterials.stiffness.rootStiffness", 0.9f);
    raise("physicalMaterials.stiffness.tipStiffness", 0.5f);
    raise("physicalMaterials.stiffness.bendStiffness", 0.9f);
    look.setJson("physicalMaterials.stiffness.stiffnessCurve", {{"X", 1.0}, {"Y", 1.0}, {"Z", 0.95}, {"W", 0.9}});
    look.setJson("physicalMaterials.stiffness.bendStiffnessCurve", {{"X", 1.0}, {"Y", 1.0}, {"Z", 0.9}, {"W", 0.8}});
    status = "Stiffness raised so curls hold their shape (stiffness .85, root .9, tip .5, bend .9, flat curves)";
}

void App::applyMaterial(int idx) {
    if (idx < 0 || idx >= (int)presets.size()) return;
    int n = applyPreset(look, presets[idx], keepColours);
    presetSel = idx;
    computeBudget();
    status = "Material: " + presets[idx].name + " (" + std::to_string(n) + " values" + (keepColours ? ", character colours kept)" : ")");
    logf("%s", status.c_str());
}

// region selector used by the agent runner: {"all":true} | {"back":true,"front":true,"left":true,"right":true,
// "z_min":m,"z_max":m (root height relative to the head top), "tip_below":m, "roots":[[x,y,z]...], "indices":[...]}
std::vector<int> App::selectGuides(const json& s) const {
    std::vector<int> out;
    if (s.is_string() && s == "selection") return selList();
    if (s.contains("indices")) { for (auto& i : s["indices"]) out.push_back(i); return out; }
    for (size_t i = 0; i < groom.P.size(); i++) {
        if (!groom.alive[i]) continue;
        V3 r = groom.P[i][0], t = groom.P[i].back();
        float zr = r.z - pack.headTop;
        if (s.value("back", false) && !(r.y < pack.headC.y)) continue;
        if (s.value("front", false) && !(r.y >= pack.headC.y)) continue;
        if (s.value("left", false) && !(r.x > 0)) continue;       // character's left = +X
        if (s.value("right", false) && !(r.x <= 0)) continue;
        if (s.contains("z_min") && zr < s["z_min"].get<float>()) continue;
        if (s.contains("z_max") && zr > s["z_max"].get<float>()) continue;
        if (s.contains("tip_below") && t.z > pack.headTop - s["tip_below"].get<float>()) continue;
        out.push_back((int)i);
    }
    if (s.contains("roots")) {
        out.clear();
        for (auto& r : s["roots"]) {
            V3 q{r[0], r[1], r[2]};
            int best = -1; float bd = 0.004f;
            for (size_t i = 0; i < groom.P.size(); i++)
                if (groom.alive[i] && len(groom.P[i][0] - q) < bd) { bd = len(groom.P[i][0] - q); best = (int)i; }
            if (best >= 0) out.push_back(best);
        }
    }
    return out;
}

// ================================================================================================ export
// Hairwork for All Core (core/README.md): release exports ship a script that calls the Core's functions, so the game's
// script compiler (and the game) refuses to start without it
static std::vector<int> coreVer(const std::string& v) {
    std::vector<int> r;
    int cur = -1;
    for (char c : v) {
        if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
        else if (cur >= 0) { r.push_back(cur); cur = -1; }
    }
    if (cur >= 0) r.push_back(cur);
    r.resize(3, 0);
    return r;
}

bool App::CoreInfo::ok() const { return installed && !version.empty() && coreVer(version) >= coreVer(required); }

std::string App::CoreInfo::summary() const {
    if (!installed) return "Core missing";
    if (version.empty()) return "Core incomplete (no script / version.json)";
    if (!required.empty() && coreVer(version) < coreVer(required)) return "Core v" + version + " < " + required;
    return "Core v" + version;
}

void App::checkCore(bool force) {
    double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    if (!force && now - core.checkedAt < 5.0) return;
    core.checkedAt = now;
    fs::path repo(set.repo);
    // required version = CORE_VERSION in <repo>\core\hwcore.py; installed one = <game>\mods\<core mod>\version.json, read
    // only when the Core's script content\scripts\local\hairworkforall_core.ws is there too. The early texture Core
    // (core_version.json, no script) has no version here: reported as incomplete.
    std::string mod = "mod0000HairworkForAllCore";
    core.required.clear();
    std::ifstream hf(repo / "core" / "hwcore.py");
    for (std::string l; std::getline(hf, l);) {
        auto p = l.find("CORE_VERSION = '");
        if (p == 0) { auto q = l.find('\'', 16); if (q != std::string::npos) core.required = l.substr(16, q - 16); }
        auto m = l.find("CORE_MOD = '");
        if (m == 0) { auto q = l.find('\'', 12); if (q != std::string::npos) mod = l.substr(12, q - 12); }
    }
    core.modDir = ((set.gameDir.empty() ? repo.parent_path() : fs::path(set.gameDir)) / "mods" / mod).string();
    std::error_code ec;
    core.installed = fs::is_directory(core.modDir, ec);
    core.version.clear();
    if (core.installed) {
        fs::path md(core.modDir);
        bool script = fs::is_regular_file(md / "content" / "scripts" / "local" / "hairworkforall_core.ws", ec);
        for (const char* marker : {"version.json"}) {
            if (!script) continue;
            try {
                std::ifstream vf(md / marker);
                if (!vf) continue;
                json j = json::parse(vf);
                core.version = j.value("version", std::string());
                break;
            } catch (...) {}
        }
    }
}

bool App::exportAll(std::string& outDir, bool release) {
    computeBudget();
    if (bud.overLimit) {
        status = bud.msg;
        logf("EXPORT BLOCKED: %s", bud.msg.c_str());
        return false;
    }
    outDir = set.repo + "\\build\\desktop_studio\\" + configName + "\\export";
    fs::create_directories(outDir);
    std::string base = outDir + "\\" + pack.character + (styleId != "default" ? "_" + styleId : std::string()) + "_" + tag;   // <char>[_<style>]_<tag>
    std::ofstream(base + "_edits.json") << groom.editsText(true);
    json lookJ = look.exportLook();
    lookJ["material_preset"] = presetSel < (int)presets.size() ? presets[presetSel].name : "";
    lookJ["material_config_keys"] = presetSel > 0 && presetSel < (int)presets.size() ? presets[presetSel].configKeys : json::object();
    std::ofstream(base + "_look.json") << lookJ.dump(1);
    // geometry exactly as shown (fur frame) + the painted stiffness map
    json geo;
    geo["K"] = pack.K;
    json G = json::array(), BI = json::array(), BW = json::array(), F = json::array(), U = json::array();
    std::vector<int> map(groom.P.size(), -1);
    int n = 0;
    for (size_t i = 0; i < groom.P.size(); i++) {
        if (!groom.alive[i]) continue;
        map[i] = n++;
        Guide g = groom.renderGuide((int)i);
        json gg = json::array();
        for (auto& p : g) gg.push_back({p.x, p.y, p.z});
        G.push_back(gg);
        int b = groom.baseOf((int)i);
        BI.push_back(pack.boneIdx[b]);
        BW.push_back(pack.boneW[b]);
    }
    if (smap.painted) bakeAtlas();
    for (size_t fi = 0; fi < groom.faces.size(); fi++) {
        auto& f = groom.faces[fi];
        if (map[f.g[0]] < 0 || map[f.g[1]] < 0 || map[f.g[2]] < 0) continue;
        F.push_back({map[f.g[0]], map[f.g[1]], map[f.g[2]]});
        U.push_back(smap.painted ? atlas.uv[fi] : f.uv);
    }
    geo["unique_uv"] = smap.painted;
    geo["guides"] = G; geo["bone_idx"] = BI; geo["bone_w"] = BW; geo["F"] = F; geo["uv"] = U;
    geo["budget"] = {{"hairs", bud.hairs}, {"hairs_close", bud.hairsClose}, {"ratio", bud.ratio}, {"ref", bud.refName}};
    std::ofstream(base + "_geo.json") << geo.dump();
    std::string stiffArg;
    if (smap.painted) {
        smap.rasterise(groom, &atlas.uv);
        if (!bakedDensity.empty()) { writePng(base + "_density_atlas.png", atlas.size, atlas.size, bakedDensity.data()); stiffArg += " --density-png \"" + base + "_density_atlas.png\""; }
        if (!bakedColor.empty()) { writePng(base + "_colour_atlas.png", atlas.size, atlas.size, bakedColor.data()); stiffArg += " --colour-png \"" + base + "_colour_atlas.png\""; }
        std::string png = base + "_stiffness.png";
        writePng(png, smap.size, smap.size, smap.rgba.data());
        stiffArg += " --stiffness \"" + png + "\"";
        if (smap.overlapTexels > 0) logf("stiffness map: %d texels shared by faces with different values (card-atlas UVs overlap)", smap.overlapTexels);
    }
    std::string cmd = set.python + " \"" + set.repo + "\\desktop_studio\\tools\\desk_export.py\" --cfg " + configName +
                      " --edits \"" + base + "_edits.json\" --look \"" + base + "_look.json\" --geo \"" + base + "_geo.json\" --mod " +
                      modName + " --tag " + tag + " --limit " + std::to_string(look.limit) + stiffArg + (release ? " --release" : "");
    if (release) {
        checkCore(true);
        logf("release export: requires %s (installed: %s)", core.required.empty() ? "the Core" : ("Hairwork for All Core v" + core.required).c_str(),
             core.summary().c_str());
    }
    logf("> %s", cmd.c_str());
    std::string out;
    int rc = runCapture(cmd, set.repo, out);
    std::istringstream s(out);
    std::string l;
    while (std::getline(s, l)) logf("  %s", l.c_str());
    status = rc == 0 ? (release ? "Exported for release (requires Core v" + core.required + ") to " : "Exported to ") + outDir : "Export FAILED (see Log)";
    if (rc == 0 && release && !core.ok()) status += "  \xC2\xB7  warning: " + core.summary() + " in your game";
    lastExport = rc == 0 ? outDir : "";
    return rc == 0;
}

// ================================================================================================ projects
// ================================================================================================ background jobs
bool App::startJob(const std::string& title, const std::string& cmd, std::function<void(int)> onDone) {
    if (job.running) { status = "Busy: " + job.title; return false; }
    if (job.th.joinable()) job.th.join();
    job.title = title;
    job.out.clear();
    job.lastLine = "starting...";
    job.rc = 0;
    job.done = false;
    job.running = true;
    job.onDone = std::move(onDone);
    logf("> %s", cmd.c_str());
    std::string repo = set.repo;
    job.th = std::thread([this, cmd, repo]() {
        SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
        HANDLE rd, wr;
        CreatePipe(&rd, &wr, &sa, 0);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si = {sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = wr;
        si.hStdError = wr;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::string c = cmd;
        int rc = -1;
        if (CreateProcessA(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, repo.c_str(), &si, &pi)) {
            CloseHandle(wr);
            char buf[2048];
            DWORD n;
            while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) {
                std::lock_guard<std::mutex> lk(job.m);
                job.out.append(buf, n);
                size_t e = job.out.find_last_not_of("\r\n");
                if (e != std::string::npos) {
                    size_t b = job.out.find_last_of('\n', e);
                    job.lastLine = job.out.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1) + 1);
                }
            }
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            rc = (int)code;
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            CloseHandle(wr);
            std::lock_guard<std::mutex> lk(job.m);
            job.out = "cannot start: " + cmd;
        }
        CloseHandle(rd);
        job.rc = rc;
        job.done = true;
    });
    return true;
}

void App::pollJob() {
    if (!job.running || !job.done) return;
    job.th.join();
    job.running = false;
    {
        std::istringstream s(job.out);
        std::string l;
        while (std::getline(s, l)) logf("  %s", l.c_str());
    }
    if (job.rc != 0) status = job.title + " failed (exit " + std::to_string(job.rc) + "): " + job.lastLine;
    if (job.onDone) { auto f = std::move(job.onDone); job.onDone = nullptr; f(job.rc); }
}

// a pack is current when desk_prep's format version matches, none of the build's inputs (the config JSON and its edits
// file: desk.json "src_files") is newer than desk.json and its fur still exists. A stale pack is re-prepped; desk_prep
// then rebuilds the fur with the pipeline when the inputs' hash changed (desk_prep.py canonical_fur).
static const int kPrepVersion = 7;      // desk_prep.py PREP_VERSION (7: material preset names without character names)

bool App::packUpToDate(const std::string& cfg) const {
    std::string d = set.repo + "\\build\\desktop_studio\\" + cfg + "\\desk.json";
    std::error_code ec;
    if (!fs::exists(d, ec)) return false;
    try {
        std::ifstream f(d);
        json j = json::parse(f);
        if (j.value("prep_version", 0) < kPrepVersion) return false;
        auto packT = fs::last_write_time(d, ec);
        auto abs = [&](const std::string& s) { fs::path p = s; return p.is_relative() ? fs::path(set.repo) / p : p; };
        if (j.contains("src_files") && j["src_files"].is_array())
            for (auto& s : j["src_files"]) {
                fs::path p = abs(s.get<std::string>());
                if (!fs::exists(p, ec) || fs::last_write_time(p, ec) > packT) return false;
            }
        std::string fur = j.value("fur_path", std::string());
        if (!fur.empty() && !fs::exists(abs(fur), ec)) return false;
        return true;
    } catch (...) { return false; }
}

// opens a build; reads / builds it through the pipeline on a worker thread when needed (UI stays live)
void App::openAsync(const std::string& cfg, bool build) {
    if (cfg.empty()) return;
    if (!build && packUpToDate(cfg)) { openConfig(cfg, false); return; }
    std::string cmd = set.python + " \"" + set.repo + "\\desktop_studio\\tools\\desk_prep.py\" " + cfg + (build ? " --build" : "");
    startJob(build ? "Building " + cfg + " with the pipeline" : "Reading the build of " + cfg, cmd, [this, cfg](int rc) {
        if (rc == 0) openConfig(cfg, false);
    });
}

bool App::saveProject(const std::string& path) {
    if (!loaded) return false;
    json j;
    j["format"] = "groomstudio-project";
    j["version"] = 1;
    j["config"] = configName;
    j["ops"] = groom.ops;
    j["look"] = look.over;
    j["material"] = presetSel < (int)presets.size() ? presets[presetSel].name : "";
    j["keep_colours"] = keepColours;
    j["stiffness"] = {{"s", smap.guideS}, {"r", smap.guideR}};
    j["motion"] = {{"preset", (int)motion.preset}, {"speed", motion.speed}, {"amount", motion.amount}};
    j["camera"] = {{"yaw", cam.yaw}, {"pitch", cam.pitch}, {"dist", cam.dist}, {"target", {cam.target.x, cam.target.y, cam.target.z}}};
    j["mod"] = modName;
    j["tag"] = tag;
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path) << j.dump(1);
    projectPath = path;
    set.addRecent(path);
    savedEdits = int(groom.ops.size() + look.over.size());
    status = "Project saved: " + path;
    return true;
}

bool App::loadProject(const std::string& path) {
    json j;
    try { std::ifstream f(path); j = json::parse(f); } catch (std::exception& e) { status = std::string("Project: ") + e.what(); return false; }
    if (!openConfig(j.value("config", ""), false)) return false;
    groom.ops.clear();
    for (auto& o : j["ops"]) groom.ops.push_back(o);
    groom.replay();
    std::string mat = j.value("material", "");
    for (int i = 0; i < (int)presets.size(); i++) if (presets[i].name == mat) presetSel = i;
    keepColours = j.value("keep_colours", true);
    look.over.clear();
    for (auto& [k, v] : j["look"].items()) look.over[k] = v;
    look.enforceCaps();
    smap.reset(groom.P.size());
    if (j.contains("stiffness")) {
        auto s = j["stiffness"]["s"], r = j["stiffness"]["r"];
        for (size_t i = 0; i < s.size() && i < smap.guideS.size(); i++) smap.guideS[i] = s[i];
        for (size_t i = 0; i < r.size() && i < smap.guideR.size(); i++) smap.guideR[i] = r[i];
    }
    if (j.contains("motion")) { motion.preset = MotionPreset(j["motion"].value("preset", 1)); motion.speed = j["motion"].value("speed", 1.0f); motion.amount = j["motion"].value("amount", 1.0f); }
    if (j.contains("camera")) { auto& c = j["camera"]; cam.yaw = c.value("yaw", cam.yaw); cam.pitch = c.value("pitch", cam.pitch); cam.dist = c.value("dist", cam.dist); }
    snprintf(modName, sizeof modName, "%s", j.value("mod", std::string(modName)).c_str());
    snprintf(tag, sizeof tag, "%s", j.value("tag", std::string(tag)).c_str());
    stiffDirty = true;
    editsChanged();
    projectPath = path;
    set.addRecent(path);
    savedEdits = int(groom.ops.size() + look.over.size());
    status = "Project loaded: " + path;
    return true;
}
