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
    g_logFile.open(path, path.size() > 9 && path.substr(path.size() - 9) == "serve.log" ? std::ios::app : std::ios::out);   // serve: keep the log of a crashed session
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
            commRepo = kCommunityRepo; /* hardcoded: the official community repo only */ commChannel = j.value("community_channel", "main"); commLocal = j.value("community_local", ""); commSource = j.value("community_source", 1);
            commAuthor = j.value("community_author", "");
            if (j.contains("community_pinned") && j["community_pinned"].is_array()) for (auto& r : j["community_pinned"]) if (r.is_string()) commPinned.insert(r.get<std::string>());
            if (j.contains("guide_colours") && j["guide_colours"].is_object())
                for (auto& [k, v] : j["guide_colours"].items()) if (v.is_array() && v.size() == 3) guideCol[k] = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
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
    json j = {{"sdk_dir", sdkDir}, {"dll", dll}, {"python", python == "\"" + exeDir() + "\\python\\python.exe\"" ? std::string() : python}, {"repo", bundled ? std::string() : repo}, {"game_dir", gameDir}, {"community_repo", commRepo}, {"community_channel", commChannel}, {"community_local", commLocal}, {"community_source", commSource}, {"community_author", commAuthor}, {"community_pinned", commPinned}, {"guide_colours", guideCol}, {"recent", recent}};
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
    if (fur == 1) swapFur();   // Beard mode: put the hair back in the live slot before the new build replaces both
    colR.clear();
    colUndoSt.clear(); colRedoSt.clear(); colSel = colSel2 = -1; colEdit = colDrag = false; colTargets = json(); colPick.clear(); colReportJ = json(); colMsg.clear();
    json raw;
    try { std::ifstream f(dir + "\\desk.json"); raw = json::parse(f); } catch (...) {}
    pack = std::move(p);
    pack.shots = raw.value("shots", json::object());
    showHeadgear = pack.headgear.is_object() && pack.headgear.value("all", false);
    headgearApp = pack.headgear.is_object() ? pack.headgear.value("default_app", std::string()) : std::string();
    skel = DeskPack();
    skel.bones = pack.bones;
    groom.init(&pack);
    look.init(&pack);
    look.enforceCaps();
    packMaterials = raw.value("materials", json::object());
    presets = loadPresets(pack, set.repo + "\\desktop_studio\\presets", packMaterials);
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
    for (auto& kv : vanillaTex) safeRelease(kv.second.srv);
    vanillaTex.clear();
    keepCards.clear();   // per-mesh triangle masks of the previous pack
    cardOffsetsBase = json::array(); cardOffsets = json::array(); cardShift.clear(); cardSel = -1; cardPick = false;
    for (auto& m : pack.meshes) {
        if (!m.texFile.empty() && !meshTex.count(m.texFile)) meshTex[m.texFile] = gfx.loadTexture(dir + "\\" + m.texFile, true);
        if (!m.nrmFile.empty() && !meshTex.count(m.nrmFile)) meshTex[m.nrmFile] = gfx.loadTexture(dir + "\\" + m.nrmFile, false);
    }
    computeHeadScalp();
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
    applyGuideColour();
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
    hw.sphereR.assign(colR.empty() ? 0 : pack.spheres.size(), -1.0f);
    for (auto& [i, r] : colR) if (i >= 0 && i < (int)hw.sphereR.size()) hw.sphereR[i] = r;
    if (!hw.build(pack, groom, guideOfAsset, atlasOn ? &atlas.uv : nullptr, hideGuide.empty() ? nullptr : &hideGuide, hideFace.empty() ? nullptr : &hideFace)) { status = "HairWorks: " + hw.lastError; logf("%s", status.c_str()); }
    stiffDirty = true;
    teleport = true;
    assetDirty = false;
    lastBuild = headless ? 0 : ImGui::GetTime();
}

void App::buildBeard() {
    beardOn = false;
    FurSlot& o = beardSlot;
    for (auto* t : {&o.stiffSrv, &o.densitySrv, &o.colorSrv, &o.bakedDensitySrv, &o.bakedColorSrv}) safeRelease(*t);
    safeRelease(o.stiffTex);
    o = FurSlot();
    if (!beardSdkOk || !pack.beardPack(beardPk)) return;
    beardGroom.init(&beardPk);
    beardLook.init(&beardPk);
    // budget reference of the beard = the beard as built (Beard mode budget card / export limit), same formula as
    // desk_prep.budget_of (the Studio's beard preview has no density map: one per face)
    {
        beardPk.budgetRef = json::object();
        Budget b = beardLook.budget((int)beardPk.F.size(), (double)beardPk.F.size(), beardPk.K);
        beardPk.budgetRef = {{"config", configName + " beard (as built)"}, {"hairs", b.hairs}, {"hairs_close", b.hairsClose}, {"cost", b.cost},
                             {"density", beardLook.f("physicalMaterials.volume.density", 1.0f)},
                             {"width", beardLook.f("physicalMaterials.strandWidth.width", 1.0f)}, {"detailLODDensity", 1.0}};
        beardPk.budgetSelf = beardPk.budgetRef;
    }
    o.smap.reset(beardPk.guides.size());
    o.presets = loadPresets(beardPk, set.repo + "\\desktop_studio\\presets", packMaterials);
    // 2026-10-16: the beard's density texture (the vanilla beard atlas alpha on density_tex builds): the game keeps only the
    // hairs the map allows (Olgierd L502: mean alpha .12, a sparse stubble); the preview drew every hair (a dense fuzz)
    if (!beardPk.densityFile.empty()) o.densitySrv = gfx.loadTexture(beardPk.dir + "\\" + beardPk.densityFile, false);
    hwBeard.densityGameOnly = true;   // bound to the SDK only while the game-accurate toggle is on (hairsdk.cpp): the default preview is unchanged
    hwBeard.setDensityTexture(o.densitySrv);
    if (o.densitySrv) logf("beard fur: density texture %s (%s)", beardPk.densityFile.c_str(), beardPk.densityChannel.c_str());
    hwBeard.setColorTexture(nullptr);
    hwBeard.setStiffnessTexture(nullptr);
    hwBeard.sphereR.clear();
    if (!hwBeard.build(beardPk, beardGroom, beardGuideOfAsset)) { logf("beard fur: %s", hwBeard.lastError.c_str()); return; }
    beardOn = true;
    logf("beard fur: %d guides, %d faces (second HairWorks instance)", (int)beardPk.guides.size(), (int)beardPk.F.size());
}

void App::swapFur() {
    using std::swap;
    ::swapFur(pack, beardPk);
    swap(groom, beardGroom);
    swap(look, beardLook);
    swap(hw, hwBeard);
    swap(guideOfAsset, beardGuideOfAsset);
    groom.pack = look.pack = &pack;
    beardGroom.pack = beardLook.pack = &beardPk;
    FurSlot& o = beardSlot;
    swap(smap, o.smap); swap(atlas, o.atlas); swap(atlasOn, o.atlasOn); swap(keepColours, o.keepColours);
    swap(stiffTex, o.stiffTex); swap(stiffSrv, o.stiffSrv); swap(densitySrv, o.densitySrv); swap(colorSrv, o.colorSrv);
    swap(bakedDensitySrv, o.bakedDensitySrv); swap(bakedColorSrv, o.bakedColorSrv);
    swap(srcDensity, o.srcDensity); swap(srcColor, o.srcColor); swap(bakedDensity, o.bakedDensity); swap(bakedColor, o.bakedColor);
    swap(srcDW, o.srcDW); swap(srcDH, o.srcDH); swap(srcCW, o.srcCW); swap(srcCH, o.srcCH);
    swap(colR, o.colR); swap(presets, o.presets); swap(presetSel, o.presetSel);
    colSel = colSel2 = -1; colEdit = colDrag = false; colUndoSt.clear(); colRedoSt.clear();   // collider edits belong to the hair fur
    fur = 1 - fur;
}

bool App::setFur(int f) {
    if (f == fur) return true;
    if (!loaded || !beardOn || windBusy) return false;
    // pending rebuilds belong to the outgoing fur: do them before it is parked
    if (assetDirty && sdkOk) rebuildHair();
    if (stiffDirty && sdkOk) updateStiffnessTexture();
    swapFur();
    // selection and check results are guide / face indices of the other fur
    sel.clear(); combOff.clear(); strokeSet.clear(); hideGuide.clear(); hideFace.clear();
    endFaces.clear(); endGuides.clear(); loopList.clear(); loopWhy.clear(); loopGuides.clear();
    windRows.clear(); windGuides.clear(); windFaces.clear();
    dragging = boxSel = drawing = false;
    smap.ensure(groom.P.size());
    assetDirty = stiffDirty = aoRefreshAll = true;
    computeBudget();
    status = fur ? "Beard mode: editing the beard (" + std::to_string(groom.numAlive()) + " guides); the hair is the preview"
                 : "Hair mode: editing the hair (" + std::to_string(groom.numAlive()) + " guides); the beard is the preview";
    logf("%s", status.c_str());
    return true;
}

int App::editCount() const {
    size_t n = groom.ops.size() + look.over.size() + (pack.colModified ? 1 : 0);
    if (beardOn) n += beardGroom.ops.size() + beardLook.over.size();
    return int(n);
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
    bud = look.budget((int)groom.faces.size(), sumFaceDensity, groom.K);
    double tsum = 0, psum = 0;
    int tn = 0;
    for (size_t i = 0; i < groom.P.size(); i++) {
        if (i < groom.alive.size() && !groom.alive[i]) continue;
        const Guide& g = groom.P[i];
        if (g.size() < 2) continue;
        double len = 0;
        for (size_t k = 1; k < g.size(); k++) { V3 e = g[k] - g[k - 1]; len += std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z); }
        V3 r = g.back() - g.front();
        tsum += len / std::max(std::sqrt(double(r.x * r.x + r.y * r.y + r.z * r.z)), 1e-9);
        psum += (double)g.size();
        tn++;
    }
    look.pathTrace(bud, (int)groom.faces.size(), tn ? tsum / tn : 1.0, tn ? psum / tn : 0.0);
    if (!ptCalibLoaded) { ptCalib = ptLoadCalib(set.repo); ptCalibLoaded = true; }
    PtInput pi;
    pi.guides.resize(groom.P.size());
    for (size_t i = 0; i < groom.P.size(); i++)
        if (!(i < groom.alive.size() && !groom.alive[i])) pi.guides[i] = groom.P[i];
    for (auto& f : groom.faces) pi.faces.push_back(f.g);
    pi.p.clump = look.f("physicalMaterials.clumping.clumpScale", 1.0f);
    pi.p.clumpNoise = look.f("physicalMaterials.clumping.clumpNoise", 0.0f);
    pi.p.width = look.f("physicalMaterials.strandWidth.width", 1.0f);
    pi.p.wRoot = look.f("physicalMaterials.strandWidth.widthRootScale", 1.0f);
    pi.p.wTip = look.f("physicalMaterials.strandWidth.widthTipScale", 1.0f);
    pi.p.waveScale = look.f("physicalMaterials.waveness.waveScale", 0.0f);
    pi.p.waveStrand = look.f("physicalMaterials.waveness.waveStrand", 0.0f);
    pi.p.waveFreq = look.f("physicalMaterials.waveness.waveFreq", 3.0f);
    ptWorker.request(std::move(pi));
}

void App::editsChanged() {
    assetDirty = true;
    syncCardOffsets();
    // 2026-10-15 crash fix (renderPoint, stage input): an undo / delete / replay can shrink or kill guides while the
    // selection still holds their indices; the Move tool then read *sel.begin() directly. Drop stale entries here.
    for (auto it = sel.begin(); it != sel.end();)
        it = (*it < 0 || *it >= (int)groom.alive.size() || !groom.alive[*it]) ? sel.erase(it) : std::next(it);
    computeBudget();
}

// ================================================================================================ game-accurate preview
void App::setGameAccurate(const json& c) {
    // "Thin-hair model" (opt-in, PT_LOOK_STUDY.md dg3): thin game strands (width = 0.09 x width^0.1), the density map
    // softened to v^0.27, opacity 1, veil 1.5. Off restores the default preview values.
    if (c.contains("thin_hair")) {
        thinHair = c["thin_hair"].get<bool>();
        // 2026-10-16 v2 (sn1, user: root / tip 1-2 drew "super thick" strands): 64 hairs per face, width 0.075 x width^0.1,
        // root / tip scales compressed to scale^0.5 (Anna L391/L393 root/tip/width sweep MAE 0.031)
        json t = thinHair ? json{{"width_const", 0.075f}, {"width_exp", 0.1f}, {"scale_exp", 0.5f}, {"hairs_per_face", 64}, {"opacity", 1.0f}, {"veil", 1.5f}, {"density_gamma", 1.0f}}
                          : json{{"width_const", 0.24f}, {"width_exp", 0.55f}, {"scale_exp", 0.55f}, {"hairs_per_face", hw.ptHairsPerFaceGame}, {"opacity", 1.0f}, {"veil", 2.5f}, {"density_gamma", 1.0f}};   // L480 defaults
        setGameAccurate(t);
    }
    for (HairRuntime* h : {&hw, &hwBeard}) {
        if (c.contains("on")) h->gameAccurate = c["on"].get<bool>();
        if (c.contains("tip_scale")) h->ptTipScale = std::clamp(c["tip_scale"].get<float>(), -1.0f, 10.0f);
        if (c.contains("width_exp")) h->ptWidthExp = std::clamp(c["width_exp"].get<float>(), 0.0f, 1.0f);
        if (c.contains("density_mip")) h->ptDensityMip = std::clamp(c["density_mip"].get<int>(), -1, 15);
        if (c.contains("width_floor")) h->ptWidthFloor = c["width_floor"].get<bool>();
        if (c.contains("lod_emu")) h->ptLodEmu = c["lod_emu"].get<bool>();
        if (c.contains("lod_dist_scale")) h->ptLodDistScale = std::clamp(c["lod_dist_scale"].get<float>(), 0.0f, 20.0f);
        if (c.contains("lod_t")) h->ptLodT = std::clamp(c["lod_t"].get<float>(), -1.0f, 1.0f);
        if (c.contains("roughness")) h->ptRoughness = std::clamp(c["roughness"].get<float>(), -1.0f, 1.0f);
        if (c.contains("neutralise")) h->ptNeutralise = c["neutralise"].get<bool>();
        if (c.contains("stiff_by_fur")) h->ptStiffByFur = c["stiff_by_fur"].get<bool>();
        if (c.contains("count_exp")) h->ptCountExp = std::clamp(c["count_exp"].get<float>(), 0.0f, 2.0f);
        if (c.contains("tb_exp")) h->ptTbExp = std::clamp(c["tb_exp"].get<float>(), 0.1f, 4.0f);
        if (c.contains("width_const")) h->ptWidthConst = std::clamp(c["width_const"].get<float>(), 0.0f, 10.0f);
        if (c.contains("width_scale")) h->ptWidthScale = std::clamp(c["width_scale"].get<float>(), 0.05f, 10.0f);
        if (c.contains("hairs_per_face")) h->ptHairsPerFace = std::clamp(c["hairs_per_face"].get<int>(), 1, 64);
        if (c.contains("root_alpha_mode")) h->ptRootAlphaMode = std::clamp(c["root_alpha_mode"].get<int>(), 0, 2);
        if (c.contains("root_alpha_falloff")) h->ptRootAlphaFalloff = c["root_alpha_falloff"].get<float>();
        if (c.contains("spline")) h->ptSpline = std::clamp(c["spline"].get<int>(), 0, 64);
        if (c.contains("wave_scale")) h->ptWaveScale = std::clamp(c["wave_scale"].get<float>(), 0.0f, 10.0f);
        if (c.contains("hair_scatter")) h->ptHairScatter = std::clamp(c["hair_scatter"].get<float>(), 0.0f, 1.0f);
        if (c.contains("pt_look")) h->ptLook = c["pt_look"].get<bool>();
        if (c.contains("scale_exp")) h->ptScaleExp = std::clamp(c["scale_exp"].get<float>(), 0.0f, 2.0f);
        if (c.contains("veil_wref")) h->ptVeilWref = std::clamp(c["veil_wref"].get<float>(), 0.0f, 10.0f);
        if (c.contains("veil")) h->ptVeil = std::clamp(c["veil"].get<float>(), 0.0f, 20.0f);
        if (c.contains("opacity")) h->ptOpacity = std::clamp(c["opacity"].get<float>(), 0.01f, 10.0f);
        if (c.contains("transmit")) h->ptTransmit = std::clamp(c["transmit"].get<float>(), 0.0f, 2.0f);
        if (c.contains("sharp")) h->ptSharp = std::clamp(c["sharp"].get<float>(), 0.0f, 1.0f);
        if (c.contains("tint")) h->ptTint = std::clamp(c["tint"].get<float>(), 0.0f, 1.0f);
        if (c.contains("sat_chroma")) h->ptSatChroma = std::clamp(c["sat_chroma"].get<float>(), 0.0f, 1.0f);
        if (c.contains("layers")) h->ptLayers = std::clamp(c["layers"].get<float>(), 0.0f, 50.0f);
        if (c.contains("bury")) h->ptBury = std::clamp(c["bury"].get<float>(), 0.0f, 20.0f);
        if (c.contains("game_light")) h->ptGameLight = c["game_light"].get<bool>();
        if (c.contains("gate")) h->ptGate = std::clamp(c["gate"].get<float>(), 0.0f, 20.0f);
        if (c.contains("ms_sat")) h->ptMsSat = std::clamp(c["ms_sat"].get<float>(), 0.0f, 10.0f);
        if (c.contains("ms_sat_amb")) h->ptMsSatAmb = std::clamp(c["ms_sat_amb"].get<float>(), 0.0f, 10.0f);
    }
    // calibration switch (agent only): "density_tex": false unbinds the density texture (every hair of every face kept)
    if (c.contains("density_tex"))
        hw.setDensityTexture(c["density_tex"].get<bool>() ? (atlasOn && bakedDensitySrv ? bakedDensitySrv : densitySrv) : nullptr);
    // calibration (agent only, L394): "density_gamma": g binds the source density map remapped v -> v^g (1 = the map itself)
    const bool gBaked = atlasOn && bakedDensitySrv && !bakedDensity.empty();
    const std::vector<unsigned char>& gSrc = gBaked ? bakedDensity : srcDensity;
    const int gW = gBaked ? atlas.size : srcDW, gH = gBaked ? atlas.size : srcDH;
    if (c.contains("density_gamma") && !gSrc.empty() && gW > 0 && gH > 0) {
        float g = std::clamp(c["density_gamma"].get<float>(), 0.01f, 10.0f);
        safeRelease(gammaDensitySrv);
        if (std::fabs(g - 1.0f) < 1e-4f) hw.setDensityTexture(gBaked ? bakedDensitySrv : densitySrv);
        else {
            std::vector<unsigned char> px(gSrc.size());
            unsigned char lut[256];
            for (int i = 0; i < 256; ++i) lut[i] = (unsigned char)std::lround(255.0 * std::pow(i / 255.0, double(g)));
            for (size_t i = 0; i < px.size(); ++i) px[i] = lut[gSrc[i]];
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = UINT(gW); td.Height = UINT(gH); td.MipLevels = 0; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ID3D11Texture2D* t = nullptr;
            if (SUCCEEDED(gfx.dev->CreateTexture2D(&td, nullptr, &t))) {
                gfx.ctx->UpdateSubresource(t, 0, nullptr, px.data(), UINT(gW) * 4, 0);
                gfx.dev->CreateShaderResourceView(t, nullptr, &gammaDensitySrv);
                if (gammaDensitySrv) gfx.ctx->GenerateMips(gammaDensitySrv);
                t->Release();
            }
            hw.setDensityTexture(gammaDensitySrv ? gammaDensitySrv : (gBaked ? bakedDensitySrv : densitySrv));
        }
    }
}

json App::gameAccurateJson() const {
    return {{"on", hw.gameAccurate}, {"width_scale", hw.ptWidthScale}, {"width_const", hw.ptWidthConst}, {"width_floor", hw.ptWidthFloor}, {"density_mip", hw.ptDensityMip},
            {"lod_emu", hw.ptLodEmu}, {"lod_dist_scale", hw.ptLodDistScale}, {"lod_t", hw.ptLodT}, {"lod_t_used", hw.ptLodTUsed}, {"lod_width_eff", hw.ptLodWidthEff}, {"lod_keep", hw.ptLodKeep}, {"cam_dist_sdk", hw.ptCamDistSdk},
            {"roughness", hw.ptRoughness}, {"neutralise", hw.ptNeutralise}, {"stiff_by_fur", hw.ptStiffByFur}, {"count_exp", hw.ptCountExp}, {"tb_exp", hw.ptTbExp}, {"thin_hair", thinHair}, {"density_mip_game", hw.ptDensityMipGame}, {"width_exp", hw.ptWidthExp}, {"hairs_per_face", hw.ptHairsPerFace},
            {"root_alpha_mode", hw.ptRootAlphaMode}, {"root_alpha_falloff", hw.ptRootAlphaFalloff}, {"spline", hw.ptSpline}, {"hair_scatter", hw.ptHairScatter}, {"wave_scale", hw.ptWaveScale},
            {"pt_look", hw.ptLook}, {"veil", hw.ptVeil}, {"scale_exp", hw.ptScaleExp}, {"veil_wref", hw.ptVeilWref}, {"eff_width", hw.ptEffWidth}, {"opacity", hw.ptOpacity}, {"transmit", hw.ptTransmit}, {"sharp", hw.ptSharp}, {"tint", hw.ptTint}, {"gate", hw.ptGate}, {"game_light", hw.ptGameLight}, {"bury", hw.ptBury}, {"layers", hw.ptLayers}, {"sat_chroma", hw.ptSatChroma}, {"ms_sat", hw.ptMsSat}, {"ms_sat_amb", hw.ptMsSatAmb},
            {"settings", hw.ptSettingsSource}};
}

// ================================================================================================ simulation step
void App::step(double dt) {
    fps = fps * 0.95f + 0.05f * float(1.0 / std::max(dt, 1e-4));
    if (!loaded) return;
    if (windBusy && !windInside) { windResume(); return; }   // Check > Max-wind strays owns the simulation while it runs
    if (assetDirty && (!dragging || headless || ImGui::GetTime() - lastBuild > 0.12)) rebuildHair();
    if (stiffDirty && sdkOk) updateStiffnessTexture();
    if (turntable && !camOverride) cam.yaw += float(dt) * 0.45f;
    if (!paused) motion.t += dt;
    else if (stepOnce) motion.t += 1.0 / 60.0;
    motion.windOn = windOn;
    hw.visBones = showBones;
    // skinning matrices of one fur: hair bones (Motion::boneMatrices) or beard bones, which follow the head (face
    // bones), the neck or the body; pivots from the hair pack's skeleton in both modes
    auto furMatrices = [&](const DeskPack& fp, bool beard, std::vector<XMFLOAT4X4>& out) {
        if (!beard) { motion.boneMatrices(fp, out, &skel); return; }
        XMMATRIX H = motion.headM(skel), Nk = motion.neckM(skel), Rt = motion.rootM(skel);
        out.resize(fp.bones.size());
        for (size_t i = 0; i < fp.bones.size(); i++) {
            const std::string& n = fp.bones[i].name;
            XMMATRIX B = H;
            if (n.rfind("torso", 0) == 0 || n.find("shoulder") != std::string::npos || n == "pelvis") B = Rt;
            else if (n == "neck" || n == "neck2" || n == "thyroid") B = Nk;
            XMStoreFloat4x4(&out[i], B);
        }
    };
    std::vector<XMFLOAT4X4>& M = boneM;
    furMatrices(pack, fur == 1, M);
    V3 w = motion.wind();
    if (hw.gameAccurate && hw.ptLodEmu && sdkOk) { HairStats cs = hw.stats(); if (cs.camDist > 0.0f) hw.ptCamDistSdk = cs.camDist; hwBeard.ptCamDistSdk = hw.ptCamDistSdk; }
    hw.updateDescriptor(look, {w.x, w.y, w.z}, simulate, sdkGuides, showCollision, showGrowth, forceCloseLod, colorize,
                        smap.painted);
    hw.skin(M, teleport);
    if (beardOn) {   // the parked fur (preview)
        std::vector<XMFLOAT4X4> Mb;
        furMatrices(beardPk, fur == 0, Mb);
        hwBeard.updateDescriptor(beardLook, {w.x, w.y, w.z}, simulate, false, false, false, forceCloseLod, colorize, beardSlot.smap.painted);
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
// skin classes of the preview shader (head, body; not eyes / teeth / beard cards): what "Scalp highlight" paints green
static bool isSkinMesh(const Mesh& m) {
    auto has = [&](const char* k) { return m.texFile.find(k) != std::string::npos; };
    if (m.alphaTest || has("teeth") || has("tongue") || has("mouth") || has("eye")) return false;
    return m.kind == "head" || m.name.rfind("g_", 0) == 0 || has("man_average") || has("woman_average");
}

static void meshVerts(const App& a, std::vector<GVertex>& out, bool textured, int only = -1) {
    out.clear();
    const DeskPack& pk = a.pack;
    const DeskPack& sk = a.skel;   // the hair pack's bones (in Beard mode pk.bones are the beard fur's)
    XMMATRIX H = a.motion.headM(sk), Nk = a.motion.neckM(sk), R = a.motion.rootM(sk);
    float neckZ = 1.62f;
    int nb = sk.boneIndex("neck");
    if (nb >= 0) neckZ = sk.bones[nb].bind[14];
    for (size_t mi = 0; mi < pk.meshes.size(); mi++) {
        const Mesh& m = pk.meshes[mi];
        if (only >= 0 && int(mi) != only) continue;
        if (m.kind == "headgear" && !a.headgearVisible(m)) continue;
        // alpha-tested beard cards: drawn one by one (own blend / depth shader), never in the merged lists or the mask
        // pass, and hidden when the beard fur replaces them or the beard is switched off
        if (m.alphaTest && (only < 0 || a.maskPass || (m.kind == "hair_cards" ? !a.vanillaHair && !a.keepCards.count(int(mi))
                                                                              : (a.beardOn || !a.showBeard)))) continue;
        // kept vanilla cards (keep_cards) next to the groom: only their triangles; the full set when the cards view is on
        const std::vector<uint8_t>* keepT = nullptr;
        if (m.kind == "hair_cards" && !a.vanillaHair) {
            auto kc = a.keepCards.find(int(mi));
            if (kc != a.keepCards.end()) keepT = &kc->second;
        }
        bool scalp = a.scalpOverlay && !a.maskPass && isSkinMesh(m);
        // scalp highlight on a textured head: drawn textured so the shader follows the painted hairline per pixel
        bool scalpTex = scalp && a.headScalp.count(int(mi)) && !m.texFile.empty() && m.uv.size() == m.v.size();
        bool tex = !m.texFile.empty() && m.uv.size() == m.v.size() && !a.maskPass && (!scalp || scalpTex);
        if (tex != textured) continue;
        auto itN = m.nrmFile.empty() ? a.meshTex.end() : a.meshTex.find(m.nrmFile);
        float texMode = (itN != a.meshTex.end() && itN->second) ? 3.0f : 2.0f;   // 3 = with normal map
        bool head = m.kind == "head" || m.kind == "hat" || m.kind == "headgear";
        // material class for the mesh shader: 0 cloth / leather / metal, 1 skin, 2 eyeball, 3 mouth (teeth / tongue)
        auto has = [&](const char* k) { return m.texFile.find(k) != std::string::npos; };
        float cls = m.alphaTest ? 4.0f : has("teeth") || has("tongue") || has("mouth") ? 3.0f : has("eye") ? 2.0f
                  : (m.kind == "head" || m.name.rfind("g_", 0) == 0 || has("man_average") || has("woman_average")) ? 1.0f : 0.0f;
        // skin / cloth albedo (linear) close to the HairLab captures' average
        XMFLOAT4 col = scalp ? XMFLOAT4(0, 1, 0, 5) : a.maskPass ? XMFLOAT4(0, 0, 0, 1) : m.kind == "head" ? XMFLOAT4(0.20f, 0.11f, 0.08f, 1) : XMFLOAT4(0.035f, 0.034f, 0.033f, 1);
        const std::vector<V3>* cs = nullptr;
        if (m.kind == "hair_cards") {
            auto it = a.cardShift.find(int(mi));
            if (it != a.cardShift.end() && it->second.size() == m.v.size()) cs = &it->second;
        }
        std::vector<XMFLOAT3> P(m.v.size());
        for (size_t i = 0; i < m.v.size(); i++) {
            V3 v0 = m.v[i];
            if (cs) v0 = v0 + (*cs)[i];   // card_offset (moved kept card piece)
            XMVECTOR p = XMVectorSet(v0.x, v0.y, v0.z, 1);
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
        for (size_t ti = 0; ti < m.t.size(); ti++) {
            const auto& t = m.t[ti];
            if (keepT && (ti >= keepT->size() || !(*keepT)[ti])) continue;
            for (int c = 0; c < 3; c++) {
                XMFLOAT3 n;
                XMStoreFloat3(&n, XMVector3Normalize(N[t[c]]));
                XMFLOAT4 cc = col;
                if (scalp) {   // scalp highlight: green only on the head's painted scalp area, other skin grey
                    auto hs = a.headScalp.find(int(mi));
                    float fl = hs != a.headScalp.end() ? hs->second[t[c]] : 0.0f;
                    cc = scalpTex ? XMFLOAT4(m.uv[t[c]][0], m.uv[t[c]][1], fl, 6) : XMFLOAT4(0, 0, fl, 5);
                }
                out.push_back({P[t[c]], n, scalp ? cc : tex ? XMFLOAT4(m.uv[t[c]][0], m.uv[t[c]][1], cls, texMode) : cc});
            }
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
        gfx.snapshotShadowMeshes();
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
        gfx.snapshotAoMeshes(k);
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
    // "game light" (2026-10-17, FINDINGS 13.6, user go), on while the path-traced look is on: the capture rig with the sun
    // x 1.5 and the sky at .6 (the rig's own colours). Measured on Ves + Shani against the L620 / L621 game frames (HLS of
    // the lit half of hair pixels): the game's shaded hair is about half as light and more saturated than the preview's,
    // the lit hair a little lighter and less saturated. With these (and ptMsSat .5 / ptMsSatAmb 1.5) the shade lands
    // within .02 L on both grooms (Ves .29 / .29, Shani .14 / .09), the lit side within .12 (Ves .70 / .82, Shani .45 / .47).
    // The earlier warm noon sun 1 / .8 / .68 + neutral sky .7 / .72 / .8 at 2.6 (L387) kept every shade above L .43 (Ves).
    // The copy also feeds fillEnvironment (the SH sky light): the sky intensity must drop there too, not only in skyCol.w.
    // Two lights while the game light is on: gl for the hair pass (above), gm for the meshes. The game's skin measured on the
    // same frames (face lit / shade, L620 / L621): lit L .29 / .29, shade L .20 / .19; a skin-only sweep of the rig (sun 2.2-3.3,
    // sky .6-1.9) puts sun 2.2 / sky 1.0 closest (Ves lit .27 shade .20, Shani lit .27 shade .22; the hair light's sky .6 gave
    // .22-.23 lit and the old sky 1.9 gave .36: too dark / too bright). Same sun direction, colours and exposure on both.
    Rig gl = rig, gm = rig;
    const bool gameLight = hw.gameAccurate && hw.ptLook && hw.ptGameLight;
    if (gameLight) {
        gl.sun = rig.sun * 1.5f;
        gl.sky = 0.6f;
        gl.sunCol[0] = 1.0f; gl.sunCol[1] = 0.96f; gl.sunCol[2] = 0.90f;
        gl.skyCol[0] = 0.60f; gl.skyCol[1] = 0.72f; gl.skyCol[2] = 0.90f;
        gm.sunCol[0] = 1.0f; gm.sunCol[1] = 0.96f; gm.sunCol[2] = 0.90f;
        gm.skyCol[0] = 0.60f; gm.skyCol[1] = 0.72f; gm.skyCol[2] = 0.90f;
        gm.sky = 1.0f;
    }
    const Rig& lr = gm;
    float ca = std::cos(lr.sunEl);
    L.sunDir = {ca * std::sin(lr.sunAz), ca * std::cos(lr.sunAz), std::sin(lr.sunEl), lr.sun};
    L.sunCol = {lr.sunCol[0], lr.sunCol[1], lr.sunCol[2], 0};
    L.skyCol = {lr.skyCol[0], lr.skyCol[1], lr.skyCol[2], lr.sky};
    L.groundCol = {lr.groundCol[0], lr.groundCol[1], lr.groundCol[2], lr.ground};
    L.params = {rig.exposure, rig.colourGain, rig.specGain, rig.selfShadow};
    L.head = loaded ? XMFLOAT4(pack.headC.x, pack.headC.y, pack.headC.z, 0.095f) : XMFLOAT4(0, 0, 1.75f, 0.095f);
    const bool neut = hw.gameAccurate && hw.ptNeutralise;   // L480: ambientEnvScale does nothing in game
    L.material = {loaded ? (neut ? 0.7f : look.f("graphicalMaterials.color.ambientEnvScale", 0.35f)) : 0.35f, rig.floor,
                  (loaded ? look.f("graphicalMaterials.color.textureBrightness", 1.0f) : 1.0f) * rig.texGain, colorSrv ? 1.0f : 0.0f};
    L.extra = {loaded ? look.f("graphicalMaterials.diffuse.diffuseNoiseScale", 0.0f) : 0.0f, rig.strandVar, hw.gameAccurate && hw.ptLook ? hw.ptMsSat : rig.msSat, hw.gameAccurate && hw.ptLook ? hw.ptMsSatAmb : rig.msSatAmb};
    L.lightVP = lightVP;
    L.shadow = {shadowsOn ? rig.shadowStrength : 0.0f, 0.0004f, 1.0f / gfx.shadowSize, rig.shadowSoft};
    // penumbra: texels per metre of blocker - receiver distance = tan(sun angular radius) / texel size
    L.shadow2 = {lfar - lnear, float(hairDebug), rig.specPower, std::tan(rig.sunSize * 0.0174533f) / (lbox / gfx.shadowSize)};
    fillEnvironment(lr, L, nAo);
    L.skyHor.w = float(rig.toneMode);
    L.skin = {rig.skinSss, rig.skinSpec, rig.skinGain, rig.cavity};
    L.ao = {float(nAo), rig.aoStrength, aoFar - aoNear, 1.0f / gfx.aoSize};
    L.hair2 = {rig.sigmaScale, rig.msSpread, rig.ttGain, rig.envSpec};
    L.hair3 = {rig.hairEncoding, rig.ambRoot, rig.ambVar, rig.ambTv};
    // g_pt.w: game-accurate colour gain = textureBrightness (the Studio shader only applied it to card textures; in game it scales
    // the hair colour, L480) x the PTHairRoughness brightness (hairsdk.h); 0 = off
    auto ptGain = [&](const HairRuntime& h, const Look& lk, bool tex) { return h.gameAccurate ? HairRuntime::roughnessGain(h.ptRoughness) * (tex ? 1.0f : std::pow(std::max(lk.f("graphicalMaterials.color.textureBrightness", 1.0f), 0.0f), h.ptTbExp)) : 0.0f; };
    L.pt = {hw.gameAccurate ? hw.ptHairScatter : 0.0f, hw.gameAccurate && !maskPass ? float(hw.ptRootAlphaMode) : 0.0f, hw.gameAccurate && hw.ptLook ? hw.ptSatChroma : 0.0f, loaded ? ptGain(hw, look, colorSrv != nullptr) : 0.0f};
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
    // the hair's own light (gl) while the game light is on: sun x 1.5, sky .6 and its SH environment; the meshes above were
    // drawn with gm; L is put back after the hair / beard passes (the composite and the overlays use the mesh light)
    LightCB Lh = L;
    if (gameLight) {
        float cah = std::cos(gl.sunEl);
        Lh.sunDir = {cah * std::sin(gl.sunAz), cah * std::cos(gl.sunAz), std::sin(gl.sunEl), gl.sun};
        Lh.sunCol = {gl.sunCol[0], gl.sunCol[1], gl.sunCol[2], 0};
        Lh.skyCol = {gl.skyCol[0], gl.skyCol[1], gl.skyCol[2], gl.sky};
        Lh.groundCol = {gl.groundCol[0], gl.groundCol[1], gl.groundCol[2], gl.ground};
        fillEnvironment(gl, Lh, nAo);
        Lh.skyHor.w = float(rig.toneMode);
        D3D11_MAPPED_SUBRESOURCE mh;
        if (SUCCEEDED(gfx.ctx->Map(gfx.lightCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mh))) { memcpy(mh.pData, &Lh, sizeof Lh); gfx.ctx->Unmap(gfx.lightCb, 0); }
    }
    if (showHair && sdkOk) {
        // "Path-traced look": the hair goes alone into a premultiplied target (meshes' depth), then pt_look.hlsl composites it
        bool ptl = hw.gameAccurate && hw.ptLook && !maskPass && !scalpOverlay && gfx.ptReady() && gfx.ptBegin();
        if (!ptl) gfx.ctx->OMSetRenderTargets(1, &gfx.msaaRtv, gfx.dsv);
        gfx.ctx->OMSetBlendState(ptl ? (gfx.bsPt ? gfx.bsPt : gfx.bsAlpha) : gfx.bsOpaque, nullptr, 0xffffffff);
        gfx.ctx->OMSetDepthStencilState(gfx.dsOn, 0);
        gfx.ctx->RSSetState(gfx.rsSolid);
        // half-buried tubes: the game path-traces each hair as a tube (radius = width) grown on the scalp, so where a hair
        // lies on the skin the closest hit is the skin. The hair pass gets its depth pushed back by ptBury mm (projection
        // z offset, z_ndc ~ 1 - near / d): hair closer to a mesh than that fails the depth test, hanging hair is untouched.
        XMFLOAT4X4 hairProj = projM;
        if (ptl && hw.ptBury > 0.0f) {
            float d = std::max(cam.dist, 0.1f), b = hw.ptBury * 1e-3f * 0.02f / (d * d);
            for (int r = 0; r < 4; r++) hairProj.m[r][2] += b * hairProj.m[r][3];
        }
        if (hw.gameAccurate && hw.ptRootAlphaMode != 0 && !maskPass) {
            // game-accurate root transparency (PTHairRootAlphaMode): the game keeps only the CLOSEST hair hit of a pixel and
            // drops it with probability 1 - saturate(t / falloff) (mode 1, stochastic, averaged over frames by the temporal
            // filter) or when that is under 0.5 (mode 2): a root zone in front shows the skin / scene, not the hair behind
            // it. Depth prepass of the hair, then one shading pass on the closest hair only, blended by that alpha.
            hw.render(viewM, hairProj, fov, float(gfx.w), float(gfx.h), gfx.lightCb, true);
            gfx.ctx->OMSetBlendState(hw.ptRootAlphaMode == 1 ? gfx.bsAlpha : gfx.bsOpaque, nullptr, 0xffffffff);
            gfx.ctx->OMSetDepthStencilState(gfx.dsTestNoWrite, 0);
            hw.render(viewM, hairProj, fov, float(gfx.w), float(gfx.h), gfx.lightCb);
            gfx.ctx->OMSetBlendState(ptl ? (gfx.bsPt ? gfx.bsPt : gfx.bsAlpha) : gfx.bsOpaque, nullptr, 0xffffffff);
            gfx.ctx->OMSetDepthStencilState(gfx.dsOn, 0);
        } else
        hw.render(viewM, hairProj, fov, float(gfx.w), float(gfx.h), gfx.lightCb);
        if (ptLoadSim && ptMs > 0) {   // PT load sim: repeat the hair pass so the GPU load follows the estimate (Vesemir ~ 1 pass per 14 ms)
            int extra = std::min(12, std::max(0, int(std::round(ptMs / ptCalib.vesemir)) - 1));
            for (int i = 0; i < extra; i++) hw.render(viewM, projM, fov, float(gfx.w), float(gfx.h), gfx.lightCb);
        }
        if (beardOn && showBeard && !maskPass) {
            // the parked fur's own material values in the shared light constants (the beard: constant colours, no card
            // texture; the hair in Beard mode: its colour texture when it has one)
            LightCB Lb = Lh;
            Lb.pt.w = ptGain(hwBeard, beardLook, beardSlot.colorSrv != nullptr);
            Lb.material = {neut ? 0.7f : beardLook.f("graphicalMaterials.color.ambientEnvScale", 0.35f), rig.floor,
                           beardSlot.colorSrv ? beardLook.f("graphicalMaterials.color.textureBrightness", 1.0f) * rig.texGain : 1.0f,
                           beardSlot.colorSrv ? 1.0f : 0.0f};
            Lb.extra.x = beardLook.f("graphicalMaterials.diffuse.diffuseNoiseScale", 0.0f);
            D3D11_MAPPED_SUBRESOURCE mb;
            if (SUCCEEDED(gfx.ctx->Map(gfx.lightCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mb))) { memcpy(mb.pData, &Lb, sizeof Lb); gfx.ctx->Unmap(gfx.lightCb, 0); }
            hwBeard.render(viewM, projM, fov, float(gfx.w), float(gfx.h), gfx.lightCb);
            if (SUCCEEDED(gfx.ctx->Map(gfx.lightCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mb))) { memcpy(mb.pData, &L, sizeof L); gfx.ctx->Unmap(gfx.lightCb, 0); }
        } else if (gameLight) {
            D3D11_MAPPED_SUBRESOURCE mh;
            if (SUCCEEDED(gfx.ctx->Map(gfx.lightCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mh))) { memcpy(mh.pData, &L, sizeof L); gfx.ctx->Unmap(gfx.lightCb, 0); }
        }
        if (ptl) {
            const float veilEff = hw.ptVeilWref > 0.0f && hw.ptEffWidth > 0.0f ? hw.ptVeil * std::clamp(hw.ptVeilWref / hw.ptEffWidth, 0.25f, 1.0f) : hw.ptVeil;
            const float pp[8] = {veilEff, hw.ptOpacity, hw.ptTransmit, hw.ptSharp, hw.ptTint, hw.ptGate, hw.ptLayers, 0};
            gfx.ptComposite(pp);
        }
        gfx.ctx->OMSetBlendState(gfx.bsOpaque, nullptr, 0xffffffff);
        gfx.ctx->OMSetRenderTargets(1, &gfx.msaaRtv, gfx.dsv);
    }
    // guide overlay (rest pose = what you edit), stiffness paint shows the painted value per guide root
    lines.clear();
    bool overlay = showGuideOverlay || (tool != Tool::Select && tool != Tool::Paint);
    // guide visibility views (guidevis.cpp): this frame's camera, the meshes as drawn (posed) occlude
    bool visPass = guideVisOn() && loaded && !maskPass && (!camOverride || showGuideOverlay);
    if (visPass)
        computeGuideVis(viewM, projM, gfx.w, gfx.h, showMeshes ? &depthTris[0] : nullptr, showMeshes ? &depthTris[1] : nullptr,
                        !camOverride && !headless);   // viewport: background worker (no frame waits on it)
    else if (!guideVisOn()) gvis.valid = false;
    bool visPts = visPass && gvis.ptsK == groom.K && gvis.pts.size() == groom.P.size() * size_t(groom.K);
    for (size_t i = 0; i < groom.P.size() && (!camOverride || showGuideOverlay); i++) {   // off-screen renders: only on request (overlay "guides")
        if (!groom.alive[i]) continue;
        bool s = sel.count((int)i) > 0, ed = groom.edited.count((int)i) > 0;
        bool fan = endingsOpen && endGuides.count((int)i) > 0;
        bool lp = loopsOpen && loopGuides.count((int)i) > 0;
        bool wd = windOpen && windGuides.count((int)i) > 0;
        if (!overlay && !s && !fan && !lp && !wd && tool != Tool::Paint) continue;
        if (!s && !guideShown((int)i)) continue;   // selected guides always stay drawn
        XMFLOAT4 c = s ? XMFLOAT4(0.15f, 0.55f, 1.0f, 1)
                   : wd ? XMFLOAT4(1.0f, 0.12f, 0.08f, 1)
                   : lp ? XMFLOAT4(0.65f, 0.25f, 1.0f, 1)
                   : fan ? XMFLOAT4(1.0f, 0.5f, 0.05f, 1)
                   : guideCustom ? XMFLOAT4(guideRgb[0], guideRgb[1], guideRgb[2], ed ? 0.95f : 0.8f)
                   : ed ? XMFLOAT4(0.95f, 0.85f, 0.7f, 0.7f) : XMFLOAT4(0.6f, 0.65f, 0.7f, 0.35f);
        int kmax = groom.K;
        if (tool == Tool::Paint) {
            float v = i < smap.guideS.size() ? (paintTarget == 2 ? smap.guideR[i] : smap.guideS[i]) : 1.0f;
            c = XMFLOAT4(0.2f + 0.8f * (1 - v), 0.35f + 0.5f * v, 0.9f * v, 0.9f);   // soft = warm, stiff = cool
            kmax = std::min(groom.K, 6);
        }
        Guide g(groom.K);
        for (int k = 0; k < groom.K; k++) g[k] = visPts ? gvis.pts[i * groom.K + k] : posedPoint((int)i, k);    // the overlay sits on the hair you see
        bool clip = !s && guideBody && visPts;   // "Hide behind the body": only the segments in front of the meshes
        for (int k = 0; k + 1 < kmax; k++) {
            if (clip && !(guidePointShown((int)i, k) && guidePointShown((int)i, k + 1))) continue;
            lines.push_back({{g[k].x, g[k].y, g[k].z}, {0, 0, 1}, c});
            lines.push_back({{g[k + 1].x, g[k + 1].y, g[k + 1].z}, {0, 0, 1}, c});
        }
        // selected guides (2026-10-13, user: bold, like the web editor): thick saturated blue line + big bright dots
        if (s && tool != Tool::Paint) {
            const float t = 0.0004f, r = 0.0022f;
            const float off[7][3] = {{0, 0, 0}, {t, 0, 0}, {-t, 0, 0}, {0, t, 0}, {0, -t, 0}, {0, 0, t}, {0, 0, -t}};
            XMFLOAT4 lc(0.05f, 0.4f, 1.0f, 1), dc(1.0f, 0.95f, 0.3f, 1);
            for (int k = 0; k + 1 < kmax; k++)
                for (auto& o : off) {
                    lines.push_back({{g[k].x + o[0], g[k].y + o[1], g[k].z + o[2]}, {0, 0, 1}, lc});
                    lines.push_back({{g[k + 1].x + o[0], g[k + 1].y + o[1], g[k + 1].z + o[2]}, {0, 0, 1}, lc});
                }
            for (int k = 0; k < kmax; k++)
                for (int ax = 0; ax < 3; ax++)
                    for (auto& o : off) {
                        XMFLOAT3 a = {g[k].x + o[0], g[k].y + o[1], g[k].z + o[2]}, b = a;
                        (&a.x)[ax] -= r; (&b.x)[ax] += r;
                        lines.push_back({a, {0, 0, 1}, dc});
                        lines.push_back({b, {0, 0, 1}, dc});
                    }
        }
    }
    // Draw tool: live preview of the stroke
    if (drawing && tool == Tool::Draw && !camOverride && drawWorld.size() > 1)
        for (size_t k = 0; k + 1 < drawWorld.size(); k++) {
            XMFLOAT4 c(1.0f, 0.8f, 0.2f, 1);
            lines.push_back({{drawWorld[k].x, drawWorld[k].y, drawWorld[k].z}, {0, 0, 1}, c});
            lines.push_back({{drawWorld[k + 1].x, drawWorld[k + 1].y, drawWorld[k + 1].z}, {0, 0, 1}, c});
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
void App::checkEndings() {
    endFaces.clear();
    endGuides.clear();
    for (size_t f = 0; f < groom.faces.size(); f++) {
        auto& F = groom.faces[f];
        float rootSp = std::max({len(groom.P[F.g[0]][0] - groom.P[F.g[1]][0]), len(groom.P[F.g[1]][0] - groom.P[F.g[2]][0]), len(groom.P[F.g[2]][0] - groom.P[F.g[0]][0])});
        if (!faceDivergent(groom, F, rootSp, endDiv, endSpread)) continue;
        endFaces.push_back((int)f);
        for (int q = 0; q < 3; q++) endGuides.insert(F.g[q]);
    }
}

void App::checkLoops() {
    loopList.clear();
    loopWhy.clear();
    loopGuides.clear();
    for (size_t g = 0; g < groom.P.size(); g++) {
        if (!groom.alive[g]) continue;
        LoopInfo li = guideLoop(groom.P[g], loopArc / 1000, loopClose / 1000, loopCurlR / 1000, loopTurn);
        if (!li.flag) continue;
        loopList.push_back((int)g);
        loopGuides.insert((int)g);
        char b[96];
        if (li.radius > 0) snprintf(b, sizeof b, "returns to within %.0f mm after %.0f mm of hair (loop radius %.0f mm)", li.dist * 1000, li.arc * 1000, li.radius * 1000);
        else snprintf(b, sizeof b, "circles %.0f deg without curling", li.turn);
        loopWhy.push_back(b);
    }
}

// one undo group of normal delete ops (opDelete, without the mirrored twins)
void App::removeLoops() {
    checkLoops();
    if (loopList.empty()) return;
    bool symOld = symmetry;
    symmetry = false;
    sel = loopGuides;
    opDelete();
    symmetry = symOld;
    checkLoops();
}

std::string App::charKey() const {
    const json* ce = loaded ? catalogEntry(configName) : nullptr;
    if (ce) { std::string c = ce->value("char", ""); if (!c.empty()) return c; }
    return configName.substr(0, configName.find('_'));
}

void App::rememberGuideColour() {
    std::string k = charKey();
    if (k.empty()) return;
    if (guideCustom) set.guideCol[k] = {guideRgb[0], guideRgb[1], guideRgb[2]};
    else set.guideCol.erase(k);
    set.save();
}

void App::applyGuideColour() {
    auto it = set.guideCol.find(charKey());
    guideCustom = it != set.guideCol.end();
    if (guideCustom) for (int i = 0; i < 3; i++) guideRgb[i] = it->second[i];
    else { guideRgb[0] = 0.6f; guideRgb[1] = 0.65f; guideRgb[2] = 0.7f; }
}

// one undo group of delete_face ops (same ops as the MCP trim_growth)
void App::removeFanned() {
    checkEndings();
    if (endFaces.empty()) return;
    int id = 0, grp = 1000 + (++grpId);
    for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    std::vector<json> ops;
    for (int f : endFaces) {
        json rr = json::array();
        for (int q = 0; q < 3; q++) { V3 p = groom.P[groom.faces[f].g[q]][0]; rr.push_back({r4(p.x), r4(p.y), r4(p.z)}); }
        ops.push_back({{"op", "delete_face"}, {"roots", rr}, {"id", ++id}, {"grp", grp}});
    }
    for (auto& o : ops) groom.ops.push_back(o);
    groom.redo.clear();
    groom.replay();
    editsChanged();
    checkEndings();
}

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
        if (!groom.alive[i] || !pickVisible((int)i)) continue;   // guide views + "Select visible only": shown guides only
        // distance to the projected segments (clicking between two points selects too)
        XMFLOAT2 a = project(posedPoint((int)i, 0));
        for (int k = 1; k < groom.K; k++) {
            XMFLOAT2 b = project(posedPoint((int)i, k));
            if (!pickPointVisible((int)i, k - 1) || !pickPointVisible((int)i, k)) { a = b; continue; }   // segment behind the body
            float ex = b.x - a.x, ey = b.y - a.y, l2 = ex * ex + ey * ey;
            float t = l2 > 1e-6f ? std::clamp(((mx - a.x) * ex + (my - a.y) * ey) / l2, 0.0f, 1.0f) : 0.0f;
            float px = a.x + ex * t - mx, py = a.y + ey * t - my, d = px * px + py * py;
            if (d < bd) { bd = d; best = (int)i; }
            a = b;
        }
    }
    return best;
}

// ---- Draw tool (2026-10-14): draw a new guide by hand
static bool screenRay(const App& a, float mx, float my, V3& o, V3& d) {
    XMMATRIX IVP = XMMatrixInverse(nullptr, XMLoadFloat4x4(&a.viewM) * XMLoadFloat4x4(&a.projM));
    float nx = mx / a.gfx.w * 2 - 1, ny = 1 - my / a.gfx.h * 2;
    XMVECTOR ra = XMVector3TransformCoord(XMVectorSet(nx, ny, 0, 1), IVP), rb = XMVector3TransformCoord(XMVectorSet(nx, ny, 1, 1), IVP);
    XMFLOAT3 p, q;
    XMStoreFloat3(&p, ra);
    XMStoreFloat3(&q, rb);
    o = {p.x, p.y, p.z};
    d = V3{q.x - p.x, q.y - p.y, q.z - p.z};
    float l = len(d);
    if (l < 1e-9f) return false;
    d = d * (1.0f / l);
    return true;
}

bool App::headRay(float mx, float my, V3& hit) const {
    V3 o, d;
    if (!screenRay(*this, mx, my, o, d)) return false;
    std::vector<V3> pos;
    std::vector<float> fl;
    std::vector<std::array<int, 3>> tr;
    if (!scalpMesh(pos, fl, tr)) return false;
    float best = 1e30f;
    for (auto& t : tr) {
        V3 a = pos[t[0]], e1 = pos[t[1]] - a, e2 = pos[t[2]] - a;
        V3 pv = cross(d, e2);
        float det = dot(e1, pv);
        if (std::fabs(det) < 1e-12f) continue;
        float inv = 1.0f / det;
        V3 tv = o - a;
        float u = dot(tv, pv) * inv;
        if (u < 0 || u > 1) continue;
        V3 qv = cross(tv, e1);
        float v = dot(d, qv) * inv;
        if (v < 0 || u + v > 1) continue;
        float s = dot(e2, qv) * inv;
        if (s > 0 && s < best) best = s;
    }
    if (best > 1e29f) return false;
    hit = o + d * best;
    return true;
}

// stroke points: the view plane through the root, then out of the head (>= 4 mm above the skin, same rule as the serve push_out)
static void drawPushOut(const App& a, std::vector<V3>& pts) {
    std::vector<V3> pos;
    std::vector<float> fl;
    std::vector<std::array<int, 3>> tr;
    if (!a.scalpMesh(pos, fl, tr)) return;
    const V3 c = a.pack.headC;
    std::vector<V3> dir;
    std::vector<float> rad;
    for (auto& p : pos) { V3 d = p - c; float r = len(d); if (r > 1e-4f) { dir.push_back(d * (1.0f / r)); rad.push_back(r); } }
    if (dir.empty()) return;
    for (size_t k = 1; k < pts.size(); k++) {
        V3 d = pts[k] - c;
        float r = len(d);
        if (r < 1e-5f || pts[k].z < c.z - 0.01f) continue;
        V3 u = d * (1.0f / r);
        std::vector<float> v;
        for (size_t i = 0; i < dir.size(); i++) if (dot(dir[i], u) > 0.996f) v.push_back(rad[i]);
        if (v.empty()) continue;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        float s = v[v.size() / 2];
        if (r < s + 0.004f) pts[k] = c + u * (s + 0.004f);
    }
}

void App::drawUpdate(float mx, float my) {
    if (!drawing) return;
    if (!drawPx.empty() && std::fabs(drawPx.back().first - mx) + std::fabs(drawPx.back().second - my) < 2.0f) return;
    drawPx.push_back({mx, my});
    V3 o, d;
    drawWorld.assign(1, drawRoot);
    XMMATRIX inv = XMMatrixInverse(nullptr, XMLoadFloat4x4(&viewM));
    V3 fw = V3{-XMVectorGetX(inv.r[2]), -XMVectorGetY(inv.r[2]), -XMVectorGetZ(inv.r[2])};
    fw = fw * (1.0f / std::max(len(fw), 1e-9f));
    for (auto& p : drawPx) {
        if (!screenRay(*this, p.first, p.second, o, d)) continue;
        float den = dot(d, fw);
        if (std::fabs(den) < 1e-6f) continue;
        float t = dot(drawRoot - o, fw) / den;
        if (t <= 0) continue;
        drawWorld.push_back(o + d * t);
    }
    drawPushOut(*this, drawWorld);
}

void App::opDrawGuide() {
    drawing = false;
    std::vector<V3> st = drawWorld;
    drawWorld.clear();
    drawPx.clear();
    if (st.empty() || groom.P.empty()) return;
    const int K = groom.K;
    // nearest live guide to the root: the template (growth face, length)
    auto nearest = [&](V3 r) {
        int b = -1;
        float bd = 1e30f;
        for (size_t i = 0; i < groom.P.size(); i++) {
            if (!groom.alive[i]) continue;
            float dd = len(groom.P[i][0] - r);
            if (dd < bd) { bd = dd; b = (int)i; }
        }
        return b;
    };
    int tpl = nearest(st[0]);
    if (tpl < 0) return;
    auto glen = [&](const Guide& G) { float l = 0; for (size_t k = 1; k < G.size(); k++) l += len(G[k] - G[k - 1]); return l; };
    // stroke -> K points evenly by arc length (a stroke under 1 cm takes the template's length and continues along the stroke)
    std::vector<float> sl(st.size(), 0.0f);
    for (size_t k = 1; k < st.size(); k++) sl[k] = sl[k - 1] + len(st[k] - st[k - 1]);
    float L = sl.back();
    if (L < 0.01f) {
        const Guide& T = groom.P[tpl];
        float tl = std::max(glen(T), 0.02f);
        V3 dir = T[K - 1] - T[0];
        if (st.size() >= 2 && L > 1e-4f) dir = st.back() - st[0];
        float dl = len(dir);
        dir = dl > 1e-6f ? dir * (1.0f / dl) : V3{0, 0, -1};
        st = {st[0], st[0] + dir * tl};
        sl = {0, tl};
        L = tl;
    }
    Guide G(K);
    size_t seg = 1;
    for (int k = 0; k < K; k++) {
        float target = L * k / std::max(K - 1, 1);
        while (seg + 1 < st.size() && sl[seg] < target) seg++;
        float s0 = sl[seg - 1], s1 = sl[seg];
        float f = s1 > s0 ? std::clamp((target - s0) / (s1 - s0), 0.0f, 1.0f) : 0.0f;
        G[k] = st[seg - 1] + (st[seg] - st[seg - 1]) * f;
    }
    int id = 0;
    for (auto& o : groom.ops) if (o.contains("id")) id = std::max(id, o["id"].get<int>());
    int grp = 1000 + (++grpId);
    int n0 = (int)groom.P.size();
    auto plant = [&](const Guide& Q, int t) {
        V3 rs = groom.P[t][0];
        V3 u = Q[0] - pack.headC;
        float ul = len(u);
        V3 root = pack.headC + u * (ul > 1e-6f ? len(rs - pack.headC) / ul : 1.0f);   // on the scalp at the template's root radius
        Guide R = Q;
        V3 sh = root - Q[0];
        for (auto& p : R) p = p + sh;
        V3 dd = root - rs;
        groom.ops.push_back({{"op", "duplicate"}, {"root", {r4(rs.x), r4(rs.y), r4(rs.z)}}, {"d", {r4(dd.x), r4(dd.y), r4(dd.z)}}, {"edge_face", true}, {"id", ++id}, {"grp", grp}});
        json pts = json::array();
        for (auto& p : R) pts.push_back({r4(p.x), r4(p.y), r4(p.z)});
        groom.ops.push_back({{"op", "reshape"}, {"root", {r4(R[0].x), r4(R[0].y), r4(R[0].z)}}, {"points", pts}, {"id", ++id}, {"grp", grp}});
    };
    plant(G, tpl);
    if (symmetry) {
        float px = groom.mirrorPlane();
        if (std::fabs(G[0].x - px) >= 0.004f) {
            Guide M = G;
            for (auto& p : M) p.x = 2 * px - p.x;
            int t2 = nearest(M[0]);
            if (t2 >= 0) plant(M, t2);
        }
    }
    groom.redo.clear();
    groom.replay();
    sel.clear();
    for (int k = n0; k < (int)groom.P.size(); k++) sel.insert(k);
    smap.ensure(groom.P.size());
    assetDirty = true;
    editsChanged();
    status = "Drew " + std::to_string((int)groom.P.size() - n0) + " new guide(s), length " + std::to_string((int)(L * 1000)) + " mm";
}

std::vector<int> App::guidesUnderBrush(float mx, float my, float r) const {
    std::vector<int> out;
    for (size_t i = 0; i < groom.P.size(); i++) {
        if (!groom.alive[i] || !pickVisible((int)i)) continue;
        int k0 = tool == Tool::Paint ? 0 : groom.K / 3;
        int k1 = tool == Tool::Paint ? std::min(groom.K, 4) : groom.K;
        for (int k = k0; k < k1; k++) {
            if (!pickPointVisible((int)i, k)) continue;
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
    if (i < 0 || i >= (int)groom.P.size()) return V3{};   // stale index guard (see editsChanged)
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
    float cpt = curlCvsPerTurn(groom.K, curl);
    status = std::to_string(ix.size()) + " guide(s) " + curl.mode + (cpt < 6 ? "  -  WARNING: only " + std::to_string(int(cpt)) +
             " CVs per turn (guides have " + std::to_string(groom.K) + " CVs): lower the turns or start earlier" : "");
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
// "z_min":m,"z_max":m (root height relative to the head top), "tip_below":m, "near":[x,y,z] + "radius":m (root distance),
// "min_length" / "max_length":m, "roots":[[x,y,z]...], "indices":[...]}
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
        if (s.contains("passes_near") && s["passes_near"].is_array() && s["passes_near"].size() == 3) {
            // any point of the strand within radius (hair over an ear / eye / forehead spot)
            V3 q{s["passes_near"][0].get<float>(), s["passes_near"][1].get<float>(), s["passes_near"][2].get<float>()};
            float rad = s.value("radius", 0.015f);
            bool hit = false;
            for (auto& p : groom.P[i]) if (len(p - q) < rad) { hit = true; break; }
            if (!hit) continue;
        }
        if (s.contains("near") && s["near"].is_array() && s["near"].size() == 3) {
            V3 q{s["near"][0].get<float>(), s["near"][1].get<float>(), s["near"][2].get<float>()};
            if (len(r - q) > s.value("radius", 0.01f)) continue;
        }
        if (s.contains("min_length") || s.contains("max_length")) {
            float L = 0;
            for (size_t k = 1; k < groom.P[i].size(); k++) L += len(groom.P[i][k] - groom.P[i][k - 1]);
            if (s.contains("min_length") && L < s["min_length"].get<float>()) continue;
            if (s.contains("max_length") && L > s["max_length"].get<float>()) continue;
        }
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

// the live fur's export files: <base>_edits.json, _look.json, _geo.json (+ the painted stiffness map and the atlas
// re-bakes when allowPaint) -> the extra desk_export arguments of the stiffness map
static std::string writeFurFiles(App& a, const std::string& base, bool allowPaint) {
    Groom& groom = a.groom;
    DeskPack& pack = a.pack;
    StiffnessMap& smap = a.smap;
    bool painted = smap.painted && allowPaint;
    std::ofstream(base + "_edits.json") << groom.editsText(true);
    json lookJ = a.look.exportLook();
    lookJ["material_preset"] = a.presetSel < (int)a.presets.size() ? a.presets[a.presetSel].name : "";
    lookJ["material_config_keys"] = a.presetSel > 0 && a.presetSel < (int)a.presets.size() ? a.presets[a.presetSel].configKeys : json::object();
    std::ofstream(base + "_look.json") << lookJ.dump(1);
    // geometry exactly as shown (fur frame) + the painted stiffness map
    json geo;
    geo["K"] = groom.K;
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
    if (painted) a.bakeAtlas();
    for (size_t fi = 0; fi < groom.faces.size(); fi++) {
        auto& f = groom.faces[fi];
        if (map[f.g[0]] < 0 || map[f.g[1]] < 0 || map[f.g[2]] < 0) continue;
        F.push_back({map[f.g[0]], map[f.g[1]], map[f.g[2]]});
        U.push_back(painted ? a.atlas.uv[fi] : f.uv);
    }
    geo["unique_uv"] = painted;
    geo["guides"] = G; geo["bone_idx"] = BI; geo["bone_w"] = BW; geo["F"] = F; geo["uv"] = U;
    if (pack.colModified) {   // the fitted / edited colliders (colliders.cpp): the whole set; desk_export.set_colliders writes it into the fur
        json sp = json::array(), cp = json::array();
        for (size_t i = 0; i < pack.spheres.size(); i++) {
            const Sphere& s = pack.spheres[i];
            sp.push_back({{"bone", s.bone}, {"bone_name", pack.bones[s.bone].name}, {"local", {s.local.x, s.local.y, s.local.z}}, {"r", s.r}});
        }
        for (auto& c : pack.capsules) cp.push_back({c[0], c[1]});
        geo["colliders"] = {{"spheres", sp}, {"capsules", cp}};
    }
    if (!a.colR.empty()) {   // collision sphere radius overrides: desk_export.set_geometry checks bone / old radius, then writes them
        json cr = json::array();
        for (auto& [i, r] : a.colR) if (i >= 0 && i < (int)pack.spheres.size()) cr.push_back({{"i", i}, {"bone", pack.spheres[i].bone}, {"r_old", pack.spheres[i].r}, {"r", r}});
        geo["collision_r"] = cr;
    }
    geo["budget"] = {{"hairs", a.bud.hairs}, {"hairs_close", a.bud.hairsClose}, {"ratio", a.bud.ratio}, {"ref", a.bud.refName}};
    std::ofstream(base + "_geo.json") << geo.dump();
    std::string stiffArg;
    if (painted) {
        smap.rasterise(groom, &a.atlas.uv);
        if (!a.bakedDensity.empty()) { writePng(base + "_density_atlas.png", a.atlas.size, a.atlas.size, a.bakedDensity.data()); stiffArg += " --density-png \"" + base + "_density_atlas.png\""; }
        if (!a.bakedColor.empty()) { writePng(base + "_colour_atlas.png", a.atlas.size, a.atlas.size, a.bakedColor.data()); stiffArg += " --colour-png \"" + base + "_colour_atlas.png\""; }
        std::string png = base + "_stiffness.png";
        writePng(png, smap.size, smap.size, smap.rgba.data());
        stiffArg += " --stiffness \"" + png + "\"";
        if (smap.overlapTexels > 0) logf("stiffness map: %d texels shared by faces with different values (card-atlas UVs overlap)", smap.overlapTexels);
    }
    return stiffArg;
}

bool App::exportAll(std::string& outDir, bool release) {
    // the hair fur's files first, then the beard fur's (Beard mode: swapped back for the export, restored after)
    struct Restore { App& a; int f; ~Restore() { if (a.fur != f) a.swapFur(); a.computeBudget(); } } restore{*this, fur};
    if (fur == 1) swapFur();
    computeBudget();
    // budget = information only (user 2026-10-15): never blocks the export; GPU cost is judged from in-game measurements
    logf("export: budget %s (information only)", bud.msg.c_str());
    outDir = set.repo + "\\build\\desktop_studio\\" + configName + "\\export";
    fs::create_directories(outDir);
    std::string base = outDir + "\\" + pack.character + (styleId != "default" ? "_" + styleId : std::string()) + "_" + tag;   // <char>[_<style>]_<tag>
    std::string stiffArg = writeFurFiles(*this, base, true);
    // beard fur edits (Beard mode): <base>_beard_edits / _look / _geo.json, written into the build's beard redfur
    if (beardOn && beardGroom.ops.size() + beardLook.over.size() > 0) {
        swapFur();
        computeBudget();
        logf("export: beard budget %s (information only)", bud.msg.c_str());
        if (smap.painted) logf("beard stiffness map: not exported (the beard fur keeps its own UVs and density map)");
        writeFurFiles(*this, base + "_beard", false);
        swapFur();
        stiffArg += " --beard-edits \"" + base + "_beard_edits.json\" --beard-look \"" + base + "_beard_look.json\" --beard-geo \"" + base + "_beard_geo.json\"";
    }
    std::string cmd = set.python + " \"" + set.repo + "\\desktop_studio\\tools\\desk_export.py\" --cfg " + configName +
                      " --edits \"" + base + "_edits.json\" --look \"" + base + "_look.json\" --geo \"" + base + "_geo.json\" --mod " +
                      modName + " --tag " + tag + " --limit " + std::to_string(look.limit) + stiffArg + (release ? " --release" : "");
    exportAllowPerf = false;
    if (release) {
        checkCore(true);
        logf("release export: requires %s (installed: %s)", core.required.empty() ? "the Core" : ("Hairwork for All Core v" + core.required).c_str(),
             core.summary().c_str());
    }
    logf("> %s", cmd.c_str());
    std::string out;
    int rc = runCapture(cmd, set.repo, out);
    if (rc != 0 && out.find("build the beard config once with the pipeline") != std::string::npos) {
        // the mod folder has no entry for the beard fur yet: build the config once (writes it), then export again
        logf("beard fur path unknown: building %s once with the pipeline, then exporting again", configName.c_str());
        std::string bo;
        runCapture(set.python + " \"" + set.repo + "\\tools\\hairworks\\hairpipe.py\" " + configName + " build --release", set.repo, bo);
        out.clear();
        rc = runCapture(cmd, set.repo, out);
    }
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
static const int kPrepVersion = 9;      // desk_prep.py PREP_VERSION (8: vanilla hair cards, 9: headgear)

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

json App::furState() const {
    json j;
    j["ops"] = groom.ops;
    j["look"] = look.over;
    j["material"] = presetSel < (int)presets.size() ? presets[presetSel].name : "";
    j["keep_colours"] = keepColours;
    j["stiffness"] = {{"s", smap.guideS}, {"r", smap.guideR}};
    if (!colR.empty()) { json cr = json::object(); for (auto& [i, r] : colR) cr[std::to_string(i)] = r; j["collision_r"] = cr; }
    if (pack.colModified) { json cj = colToJson(); cj.erase("collision_r"); j["colliders"] = cj; }   // the fitted / edited set (colliders.cpp)
    return j;
}

void App::applyFurState(const json& j) {
    groom.ops.clear();
    groom.redo.clear();
    for (auto& o : j.value("ops", json::array())) groom.ops.push_back(o);
    groom.replay();
    std::string mat = j.value("material", "");
    for (int i = 0; i < (int)presets.size(); i++) if (presets[i].name == mat) presetSel = i;
    keepColours = j.value("keep_colours", true);
    look.over.clear();
    json lk = j.value("look", json::object());
    for (auto& [k, v] : lk.items()) look.over[k] = v;
    colR.clear();
    colUndoSt.clear(); colRedoSt.clear(); colSel = colSel2 = -1;
    if (j.contains("colliders") && j["colliders"].is_object()) {
        std::string err;
        if (colFromJson(j["colliders"], &err)) pack.colModified = true;
        else logf("project colliders not applied: %s", err.c_str());
    } else if (pack.colModified) {   // another project's set is still in the pack: back to the build's own
        pack.spheres = pack.spheres0; pack.capsules = pack.capsules0; pack.colModified = false;
    }
    if (j.contains("collision_r")) for (auto& [k, v] : j["collision_r"].items()) colR[std::stoi(k)] = v.get<float>();
    look.enforceCaps();
    smap.reset(groom.P.size());
    if (j.contains("stiffness")) {
        auto s = j["stiffness"].value("s", json::array()), r = j["stiffness"].value("r", json::array());
        for (size_t i = 0; i < s.size() && i < smap.guideS.size(); i++) smap.guideS[i] = s[i];
        for (size_t i = 0; i < r.size() && i < smap.guideR.size(); i++) smap.guideR[i] = r[i];
    }
}

bool App::saveProject(const std::string& path) {
    if (!loaded) return false;
    // top-level fur keys = the hair fur (the format older projects have); "beard" = the beard fur's own keys (Beard mode,
    // 2026-10-14; absent for builds without a beard fur), "fur": "beard" = the project was saved in Beard mode
    int f0 = fur;
    if (fur == 1) swapFur();
    json j = furState();
    if (beardOn) { swapFur(); j["beard"] = furState(); swapFur(); }
    if (f0 == 1) { swapFur(); j["fur"] = "beard"; }
    j["format"] = "groomstudio-project";
    j["version"] = 1;
    j["config"] = configName;
    if (guideCustom) j["guide_colour"] = {guideRgb[0], guideRgb[1], guideRgb[2]};
    j["motion"] = {{"preset", (int)motion.preset}, {"speed", motion.speed}, {"amount", motion.amount}, {"wind_yaw", motion.windYawDeg}, {"wind_pitch", motion.windPitchDeg}};
    j["camera"] = {{"yaw", cam.yaw}, {"pitch", cam.pitch}, {"dist", cam.dist}, {"target", {cam.target.x, cam.target.y, cam.target.z}}};
    j["mod"] = modName;
    j["tag"] = tag;
    if (perfAccepted) j["perf_accepted"] = true;
    // 2026-10-15 kept vanilla cards (keep_cards, e.g. Donar's side braids): mesh index -> kept triangle indices, so a
    // project reopens with its cards + strands mix (the game build keeps them with the beard keep_card_* keys)
    if (!keepCards.empty()) {
        json kc = json::object();
        for (auto& kv : keepCards) {
            json l = json::array();
            for (size_t ti = 0; ti < kv.second.size(); ti++) if (kv.second[ti]) l.push_back(int(ti));
            if (!l.empty()) kc[std::to_string(kv.first)] = l;
        }
        if (!kc.empty()) j["keep_cards"] = kc;
    }
    if (!cardOffsets.empty()) j["card_offsets"] = cardOffsets;   // 2026-10-17 moved kept card pieces (studio_card_offset)
    j["saved_by"] = headless ? "agent" : "ui";   // 2026-10-16 agent tools: an agent's MCP layer snapshots a UI save before it edits it
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path) << j.dump(1);
    projectPath = path;
    set.addRecent(path);
    savedEdits = editCount();
    status = "Project saved: " + path;
    return true;
}

bool App::loadProject(const std::string& path) {
    json j;
    try { std::ifstream f(path); j = json::parse(f); } catch (std::exception& e) { status = std::string("Project: ") + e.what(); return false; }
    if (!openConfig(j.value("config", ""), false)) return false;
    applyFurState(j);
    if (beardOn && j.contains("beard") && j["beard"].is_object()) {
        // the beard fur's edits: applied while it is live, its asset rebuilt now (it is parked again afterwards)
        swapFur();
        applyFurState(j["beard"]);
        computeBudget();
        rebuildHair();
        swapFur();
    }
    // the project's colour wins; without one openConfig already restored the character's remembered colour
    if (j.contains("guide_colour") && j["guide_colour"].is_array() && j["guide_colour"].size() == 3) {
        guideCustom = true;
        for (int i = 0; i < 3; i++) guideRgb[i] = j["guide_colour"][i].get<float>();
    }
    if (j.contains("motion")) { motion.preset = MotionPreset(j["motion"].value("preset", 1)); motion.speed = j["motion"].value("speed", 1.0f); motion.amount = j["motion"].value("amount", 1.0f); motion.windYawDeg = j["motion"].value("wind_yaw", 206.565f); motion.windPitchDeg = j["motion"].value("wind_pitch", 2.85f); }
    if (j.contains("camera")) { auto& c = j["camera"]; cam.yaw = c.value("yaw", cam.yaw); cam.pitch = c.value("pitch", cam.pitch); cam.dist = c.value("dist", cam.dist); }
    snprintf(modName, sizeof modName, "%s", j.value("mod", std::string(modName)).c_str());
    snprintf(tag, sizeof tag, "%s", j.value("tag", std::string(tag)).c_str());
    perfAccepted = j.value("perf_accepted", false);
    keepCards.clear();
    if (j.contains("keep_cards") && j["keep_cards"].is_object())
        for (auto& [k, l] : j["keep_cards"].items()) {
            int mi = std::atoi(k.c_str());
            if (mi < 0 || mi >= int(pack.meshes.size()) || pack.meshes[mi].kind != "hair_cards") continue;
            auto& mask = keepCards[mi];
            mask.assign(pack.meshes[mi].t.size(), 0);
            for (auto& t : l) { int ti = t.get<int>(); if (ti >= 0 && ti < int(mask.size())) mask[ti] = 1; }
        }
    cardOffsetsBase = j.contains("card_offsets") && j["card_offsets"].is_array() ? j["card_offsets"] : json::array();
    cardSel = -1;
    syncCardOffsets();
    stiffDirty = true;
    editsChanged();
    if (j.value("fur", std::string()) == "beard") setFur(1);
    projectPath = path;
    set.addRecent(path);
    savedEdits = editCount();
    status = "Project loaded: " + path + (fur == 1 ? " (Beard mode)" : "");
    return true;
}


// ray (world) against the posed skin meshes (same pose / vertex blend as the render); nearest hit
void App::posedMeshTris(std::vector<GVertex>& out) const {
    out.clear();
    std::vector<GVertex> t;
    for (int pass = 0; pass < 2; pass++) { meshVerts(*this, t, pass == 1); out.insert(out.end(), t.begin(), t.end()); }
}

bool App::skinRay(const XMFLOAT3& o, const XMFLOAT3& d, XMFLOAT3& hit, int& mesh) const {
    float best = 1e30f;
    mesh = -1;
    XMVECTOR O = XMLoadFloat3(&o), D = XMLoadFloat3(&d);
    for (size_t mi = 0; mi < pack.meshes.size(); mi++) {
        if (!isSkinMesh(pack.meshes[mi])) continue;
        std::vector<GVertex> tv;
        for (int pass = 0; pass < 2; pass++) {
            tv.clear();
            meshVerts(*this, tv, pass == 1, int(mi));
            for (size_t i = 0; i + 2 < tv.size(); i += 3) {
                XMVECTOR a0 = XMLoadFloat3(&tv[i].pos), e1 = XMVectorSubtract(XMLoadFloat3(&tv[i + 1].pos), a0), e2 = XMVectorSubtract(XMLoadFloat3(&tv[i + 2].pos), a0);
                XMVECTOR pv = XMVector3Cross(D, e2);
                float det = XMVectorGetX(XMVector3Dot(e1, pv));
                if (std::fabs(det) < 1e-12f) continue;
                XMVECTOR tvv = XMVectorSubtract(O, a0);
                float u = XMVectorGetX(XMVector3Dot(tvv, pv)) / det;
                if (u < 0 || u > 1) continue;
                XMVECTOR qv = XMVector3Cross(tvv, e1);
                float v = XMVectorGetX(XMVector3Dot(D, qv)) / det;
                if (v < 0 || u + v > 1) continue;
                float t = XMVectorGetX(XMVector3Dot(e2, qv)) / det;
                if (t > 0 && t < best) { best = t; mesh = int(mi); XMStoreFloat3(&hit, XMVectorAdd(O, XMVectorScale(D, t))); }
            }
        }
    }
    return mesh >= 0;
}

// posed head mesh with its scalp flags (per original vertex): input of the serve hairline tools (serve.cpp)
bool App::scalpMesh(std::vector<V3>& pos, std::vector<float>& flag, std::vector<std::array<int, 3>>& tris) const {
    for (auto& [mi, f] : headScalp) {
        const Mesh& m = pack.meshes[mi];
        std::vector<GVertex> tv;
        meshVerts(*this, tv, true, mi);
        if (tv.size() != m.t.size() * 3) meshVerts(*this, tv, false, mi);
        if (tv.size() != m.t.size() * 3) continue;
        pos.assign(m.v.size(), V3{});
        for (size_t j = 0; j < m.t.size(); j++)
            for (int c = 0; c < 3; c++) pos[m.t[j][c]] = {tv[j * 3 + c].pos.x, tv[j * 3 + c].pos.y, tv[j * 3 + c].pos.z};
        flag = f;
        tris = m.t;
        return true;
    }
    return false;
}

// 2026-10-17 card_offset: per-vertex displacement of the hair-card vertices picked by each cardOffsets entry. Selection
// (same rule as hairpipe.py card_offset): uv box = vertices of triangles whose centroid uv lies in [uv_min, uv_max];
// near + radius_mm = with a uv box, triangles whose centroid is within the radius too, without one, whole connected card
// strips with a vertex within the radius. Picks use the ORIGINAL vertex positions. Move = rotate_deg (X, then Y, then Z,
// about the picked vertices' centroid) then offset.
json App::applyCardOffsets() {
    cardShift.clear();
    json rep = json::array();
    for (auto& e : cardOffsets) {
        bool uvBox = e.contains("uv_min") && e.contains("uv_max");
        bool nearP = e.contains("near");
        V3 p = nearP ? V3{e["near"][0].get<float>(), e["near"][1].get<float>(), e["near"][2].get<float>()} : V3{};
        float rad = e.value("radius_mm", 25.0f) / 1000;
        float u0 = uvBox ? e["uv_min"][0].get<float>() : 0, v0 = uvBox ? e["uv_min"][1].get<float>() : 0;
        float u1 = uvBox ? e["uv_max"][0].get<float>() : 0, v1 = uvBox ? e["uv_max"][1].get<float>() : 0;
        V3 off{};
        if (e.contains("offset")) off = {e["offset"][0].get<float>(), e["offset"][1].get<float>(), e["offset"][2].get<float>()};
        V3 rot{};
        if (e.contains("rotate_deg")) rot = {e["rotate_deg"][0].get<float>(), e["rotate_deg"][1].get<float>(), e["rotate_deg"][2].get<float>()};
        int nv = 0;
        V3 c0{};
        for (size_t mi = 0; mi < pack.meshes.size(); mi++) {
            const Mesh& m = pack.meshes[mi];
            if (m.kind != "hair_cards" || m.t.empty()) continue;
            std::vector<uint8_t> sel(m.v.size(), 0);
            if (uvBox) {
                if (m.uv.size() != m.v.size()) continue;
                for (auto& t : m.t) {
                    float cu = (m.uv[t[0]][0] + m.uv[t[1]][0] + m.uv[t[2]][0]) / 3, cv = (m.uv[t[0]][1] + m.uv[t[1]][1] + m.uv[t[2]][1]) / 3;
                    if (cu < u0 || cu > u1 || cv < v0 || cv > v1) continue;
                    if (nearP && len((m.v[t[0]] + m.v[t[1]] + m.v[t[2]]) * (1.0f / 3) - p) > rad) continue;
                    sel[t[0]] = sel[t[1]] = sel[t[2]] = 1;
                }
            } else if (nearP) {
                std::vector<int> par(m.v.size());
                for (size_t i = 0; i < par.size(); i++) par[i] = int(i);
                std::function<int(int)> fd = [&](int x) { while (par[x] != x) x = par[x] = par[par[x]]; return x; };
                for (auto& t : m.t) { par[fd(t[1])] = fd(t[0]); par[fd(t[2])] = fd(t[0]); }
                std::set<int> hit;
                for (size_t i = 0; i < m.v.size(); i++) if (len(m.v[i] - p) < rad) hit.insert(fd(int(i)));
                for (auto& t : m.t) if (hit.count(fd(t[0]))) sel[t[0]] = sel[t[1]] = sel[t[2]] = 1;
            }
            int n = 0;
            V3 c{};
            for (size_t i = 0; i < sel.size(); i++) if (sel[i]) { c = c + m.v[i]; n++; }
            if (!n) continue;
            c = c * (1.0f / n);
            const float d2r = 3.14159265f / 180;
            float cx = std::cos(rot.x * d2r), sx = std::sin(rot.x * d2r), cy = std::cos(rot.y * d2r), sy = std::sin(rot.y * d2r),
                  cz = std::cos(rot.z * d2r), sz = std::sin(rot.z * d2r);
            auto& sh = cardShift[int(mi)];
            if (sh.size() != m.v.size()) sh.assign(m.v.size(), V3{});
            for (size_t i = 0; i < sel.size(); i++) {
                if (!sel[i]) continue;
                V3 q = m.v[i] + sh[i] - c;
                q = {q.x, cx * q.y - sx * q.z, sx * q.y + cx * q.z};
                q = {cy * q.x + sy * q.z, q.y, -sy * q.x + cy * q.z};
                q = {cz * q.x - sz * q.y, sz * q.x + cz * q.y, q.z};
                sh[i] = q + c + off - m.v[i];
            }
            nv += n; c0 = c;
        }
        rep.push_back({{"moved_vertices", nv}, {"centroid_before", {c0.x, c0.y, c0.z}},
                       {"centroid_after", {c0.x + off.x, c0.y + off.y, c0.z + off.z}}});
    }
    return rep;
}

// 2026-10-17 card pieces: the current moves = the last "card_offsets" snapshot op of the hair fur (undo / redo walk them),
// else the project's saved list. Beard mode keeps the hair fur's moves.
void App::syncCardOffsets() {
    if (fur == 1) return;
    json cur = cardOffsetsBase;
    for (auto it = groom.ops.rbegin(); it != groom.ops.rend(); ++it)
        if (it->value("type", std::string()) == "card_offsets") { cur = it->value("entries", json::array()); break; }
    if (cur != cardOffsets || cardShift.empty() != cur.empty()) { cardOffsets = cur; applyCardOffsets(); }
    if (cardSel >= (int)cardOffsets.size()) cardSel = (int)cardOffsets.size() - 1;
}

// picked vertices (original positions) + their shift under `entries` for entry `e`: centroid, ring axis, inner radius
static bool cardPieceFrame(const App& a, const json& e, const std::map<int, std::vector<V3>>& shift, V3& c, V3& ax, float& rin) {
    bool uvBox = e.contains("uv_min") && e.contains("uv_max");
    bool nearP = e.contains("near");
    V3 p = nearP ? V3{e["near"][0].get<float>(), e["near"][1].get<float>(), e["near"][2].get<float>()} : V3{};
    float rad = e.value("radius_mm", 25.0f) / 1000;
    std::vector<V3> pts;
    for (size_t mi = 0; mi < a.pack.meshes.size(); mi++) {
        const Mesh& m = a.pack.meshes[mi];
        if (m.kind != "hair_cards" || !uvBox || m.uv.size() != m.v.size()) continue;
        auto sh = shift.find(int(mi));
        std::vector<uint8_t> sel(m.v.size(), 0);
        for (auto& t : m.t) {
            float cu = (m.uv[t[0]][0] + m.uv[t[1]][0] + m.uv[t[2]][0]) / 3, cv = (m.uv[t[0]][1] + m.uv[t[1]][1] + m.uv[t[2]][1]) / 3;
            if (cu < e["uv_min"][0].get<float>() || cu > e["uv_max"][0].get<float>() || cv < e["uv_min"][1].get<float>() || cv > e["uv_max"][1].get<float>()) continue;
            if (nearP && len((m.v[t[0]] + m.v[t[1]] + m.v[t[2]]) * (1.0f / 3) - p) > rad) continue;
            sel[t[0]] = sel[t[1]] = sel[t[2]] = 1;
        }
        for (size_t i = 0; i < sel.size(); i++)
            if (sel[i]) pts.push_back(m.v[i] + (sh != shift.end() && sh->second.size() == m.v.size() ? sh->second[i] : V3{}));
    }
    if (pts.size() < 3) return false;
    c = {};
    for (auto& q : pts) c = c + q;
    c = c * (1.0f / pts.size());
    float C[3][3] = {};
    for (auto& q : pts) { float d[3] = {q.x - c.x, q.y - c.y, q.z - c.z}; for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) C[r][k] += d[r] * d[k]; }
    float tr = C[0][0] + C[1][1] + C[2][2];
    V3 v{0.3f, 0.5f, 0.8f};   // power iteration on (tr I - C): the least-spread direction = the ring axis
    for (int it = 0; it < 60; it++) {
        V3 w{tr * v.x - (C[0][0] * v.x + C[0][1] * v.y + C[0][2] * v.z), tr * v.y - (C[1][0] * v.x + C[1][1] * v.y + C[1][2] * v.z),
             tr * v.z - (C[2][0] * v.x + C[2][1] * v.y + C[2][2] * v.z)};
        float l = len(w);
        if (l < 1e-12f) break;
        v = w * (1.0f / l);
    }
    ax = v;
    rin = 1e9f;
    for (auto& q : pts) { V3 d = q - c; d = d - ax * dot(d, ax); rin = std::min(rin, len(d)); }
    return true;
}

// pushes the snapshot op (+ the "guides follow" reshapes) as one undo group
json App::commitCardOffsets(const json& prevEntries, bool follow) {
    json r;
    int grp = 4000 + (++grpId);
    int id = 0;
    for (auto& o : groom.ops) if (o.contains("id") && o["id"].is_number_integer()) id = std::max(id, o["id"].get<int>());
    groom.redo.clear();
    int nf = 0;
    if (follow && cardSel >= 0 && cardSel < (int)cardOffsets.size() && cardSel < (int)prevEntries.size()) {
        // guides passing near the piece's OLD centre (a ponytail gathered by a ring): converge to the NEW centre inside the
        // hole, the part above slides with a smoothstep ramp from the root, the rest is carried (jringgather.py logic)
        json cur = cardOffsets;
        cardOffsets = prevEntries; applyCardOffsets();
        auto shiftOld = cardShift;
        cardOffsets = cur; applyCardOffsets();
        V3 c0, a0, c1, a1;
        float r0, r1;
        if (cardPieceFrame(*this, cardOffsets[cardSel], shiftOld, c0, a0, r0) && cardPieceFrame(*this, cardOffsets[cardSel], cardShift, c1, a1, r1)) {
            V3 dl = c1 - c0;
            float hole = 0.75f * r1;
            for (size_t i = 0; i < groom.P.size(); i++) {
                if (!groom.alive[i]) continue;
                const Guide& G = groom.P[i];
                float L = 0, best = 1e9f;
                int j = 0;
                for (size_t k = 0; k < G.size(); k++) {
                    if (k) L += len(G[k] - G[k - 1]);
                    float d = len(G[k] - c0);
                    if (d < best) { best = d; j = int(k); }
                }
                if (best > cardFollowMm / 1000 || L < cardFollowLen || j == 0) continue;
                std::vector<float> seg(G.size(), 0);
                for (size_t k = 1; k < G.size(); k++) seg[k] = seg[k - 1] + len(G[k] - G[k - 1]);
                Guide Q = G;
                for (size_t k = 0; k < G.size(); k++) {
                    float t = std::clamp(seg[k] / std::max(seg[j], 1e-6f), 0.0f, 1.0f);
                    Q[k] = G[k] + dl * (t * t * (3 - 2 * t));
                }
                V3 rv = (Q[j] - c1) - a1 * dot(Q[j] - c1, a1);
                float rn = len(rv);
                if (rn > hole) {
                    V3 fix = rv * (hole / rn - 1);
                    for (size_t k = 0; k < G.size(); k++) { float z = (seg[k] - seg[j]) / 0.012f; Q[k] = Q[k] + fix * std::exp(-z * z); }
                }
                json pts = json::array();
                for (auto& q : Q) pts.push_back({q.x, q.y, q.z});
                json op = {{"op", "reshape"}, {"root", {G[0].x, G[0].y, G[0].z}}, {"points", pts}, {"id", ++id}, {"grp", grp}};
                int dup = 0;   // stacked copies share one root: address the k-th one
                for (size_t q = 0; q < i; q++) if (groom.alive[q] && len(groom.P[q][0] - G[0]) < 1e-6f) dup++;
                if (dup) op["dup"] = dup;
                groom.ops.push_back(op);
                nf++;
            }
        }
    }
    groom.ops.push_back({{"type", "card_offsets"}, {"entries", cardOffsets}, {"id", ++id}, {"grp", grp}});
    groom.replay();
    editsChanged();
    r["guides_followed"] = nf;
    return r;
}

// click in the viewport (Card pieces > Pick piece): the kept card piece under the cursor becomes an entry (uv box of its
// triangles + near / radius around them, the studio_card_offset rule) or, when an entry already covers it, is selected
bool App::pickCardPiece(float mx, float my) {
    V3 o, d;
    if (!screenRay(*this, mx, my, o, d)) return false;
    float best = 1e30f;
    int bm = -1, bt = -1;
    for (size_t mi = 0; mi < pack.meshes.size(); mi++) {
        const Mesh& m = pack.meshes[mi];
        if (m.kind != "hair_cards" || m.uv.size() != m.v.size()) continue;
        auto kc = keepCards.find(int(mi));
        if (!vanillaHair && kc == keepCards.end()) continue;
        auto sh = cardShift.find(int(mi));
        auto P = [&](int i) { return m.v[i] + (sh != cardShift.end() && sh->second.size() == m.v.size() ? sh->second[i] : V3{}); };
        for (size_t ti = 0; ti < m.t.size(); ti++) {
            if (!vanillaHair && (ti >= kc->second.size() || !kc->second[ti])) continue;
            const auto& t = m.t[ti];
            V3 a0 = P(t[0]), e1 = P(t[1]) - a0, e2 = P(t[2]) - a0;
            V3 pv = cross(d, e2);
            float det = dot(e1, pv);
            if (std::fabs(det) < 1e-12f) continue;
            float inv = 1.0f / det;
            V3 tv = o - a0;
            float u = dot(tv, pv) * inv;
            if (u < 0 || u > 1) continue;
            V3 qv = cross(tv, e1);
            float v = dot(d, qv) * inv;
            if (v < 0 || u + v > 1) continue;
            float s = dot(e2, qv) * inv;
            if (s > 0 && s < best) { best = s; bm = int(mi); bt = int(ti); }
        }
    }
    if (bm < 0) { status = "Card pieces: no kept card under the cursor (keep the cards first: studio_keep_cards / project keep_cards)"; return false; }
    const Mesh& m = pack.meshes[bm];
    std::vector<int> par(m.v.size());
    for (size_t i = 0; i < par.size(); i++) par[i] = int(i);
    std::function<int(int)> fd = [&](int x) { while (par[x] != x) x = par[x] = par[par[x]]; return x; };
    for (auto& t : m.t) { par[fd(t[1])] = fd(t[0]); par[fd(t[2])] = fd(t[0]); }
    int root = fd(m.t[bt][0]);
    float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
    V3 c{};
    int n = 0;
    std::vector<V3> cen;
    for (auto& t : m.t) {
        if (fd(t[0]) != root) continue;
        float cu = (m.uv[t[0]][0] + m.uv[t[1]][0] + m.uv[t[2]][0]) / 3, cv = (m.uv[t[0]][1] + m.uv[t[1]][1] + m.uv[t[2]][1]) / 3;
        u0 = std::min(u0, cu); v0 = std::min(v0, cv); u1 = std::max(u1, cu); v1 = std::max(v1, cv);
        V3 q = (m.v[t[0]] + m.v[t[1]] + m.v[t[2]]) * (1.0f / 3);
        cen.push_back(q); c = c + q; n++;
    }
    c = c * (1.0f / n);
    float rad = 0;
    for (auto& q : cen) rad = std::max(rad, len(q - c));
    for (size_t k = 0; k < cardOffsets.size(); k++) {
        const json& e = cardOffsets[k];
        if (e.contains("near") && len(V3{e["near"][0].get<float>(), e["near"][1].get<float>(), e["near"][2].get<float>()} - c) < 0.006f) {
            cardSel = int(k); status = "Card pieces: piece " + std::to_string(k + 1) + " selected"; return true;
        }
        if (!e.contains("near") && e.contains("uv_min") && e["uv_min"][0].get<float>() <= u0 && e["uv_max"][0].get<float>() >= u1 &&
            e["uv_min"][1].get<float>() <= v0 && e["uv_max"][1].get<float>() >= v1) {
            cardSel = int(k); status = "Card pieces: piece " + std::to_string(k + 1) + " selected"; return true;
        }
    }
    auto r4f = [](float x) { return std::round(x * 1e4f) / 1e4f; };
    json prev = cardOffsets;
    cardOffsets.push_back({{"uv_min", {r4f(u0 - 0.002f), r4f(v0 - 0.002f)}}, {"uv_max", {r4f(u1 + 0.002f), r4f(v1 + 0.002f)}},
                           {"near", {r4f(c.x), r4f(c.y), r4f(c.z)}}, {"radius_mm", std::round(rad * 1000 + 3)},
                           {"offset", {0.0, 0.0, 0.0}}, {"rotate_deg", {0.0, 0.0, 0.0}}});
    cardSel = (int)cardOffsets.size() - 1;
    applyCardOffsets();
    commitCardOffsets(prev, false);
    status = "Card pieces: picked piece " + std::to_string(cardSel + 1) + " (" + std::to_string(n) + " triangles); move it with the fields";
    return true;
}
