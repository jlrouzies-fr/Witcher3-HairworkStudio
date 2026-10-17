// Desktop Groom Studio - HairWorks runtime wrapper. Our own code (MIT), written against the public API header
// (NvHairSdk.h) of the HairWorks SDK the user has locally; no SDK source, sample or shader code is copied here.
#include "hairsdk.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>

#include <Nv/Common/Platform/Dx11/NvCoDx11Handle.h>
#include <Nv/HairWorks/NvHairSdk.h>

namespace NvHair = nvidia::HairWorks;
namespace NvCo = nvidia::Common;
using namespace DirectX;

static NvHair::InstanceDescriptor g_desc;

// HairWorks reports through this (it must not be null: the runtime logs from inside some calls)
void (*g_hairLog)(const char*) = nullptr;
class StudioLogger : public NvCo::Logger {
public:
    void log(NvCo::ELogSeverity sev, const nvidia::Char* text, const nvidia::Char* fn, const nvidia::Char* file, nvidia::Int line) override {
        if (g_hairLog) {
            char buf[1024];
            snprintf(buf, sizeof buf, "HairWorks[%d]: %s (%s)", int(sev), text ? text : "", fn ? fn : "");
            g_hairLog(buf);
        }
    }
};
static StudioLogger g_logger;

// in-memory streams (round-trip the created asset through the SDK's own .apx serialiser, also used to save .apx files)
class MemWrite : public NvCo::WriteStream {
public:
    std::string data;
    bool closed = false;
    nvidia::Int64 write(const nvidia::Void* p, nvidia::Int64 n) override { data.append((const char*)p, size_t(n)); return n; }
    nvidia::Void flush() override {}
    nvidia::Void close() override { closed = true; }
    nvidia::Bool isClosed() override { return closed; }
};
class MemRead : public NvCo::ReadStream {
public:
    const std::string& data;
    nvidia::Int64 pos = 0;
    bool closed = false;
    explicit MemRead(const std::string& d) : data(d) {}
    nvidia::Int64 read(nvidia::Void* p, nvidia::Int64 n) override {
        n = std::min<nvidia::Int64>(n, nvidia::Int64(data.size()) - pos);
        if (n > 0) memcpy(p, data.data() + pos, size_t(n));
        pos += std::max<nvidia::Int64>(n, 0);
        return std::max<nvidia::Int64>(n, 0);
    }
    nvidia::Int64 seek(NvCo::ESeekOrigin o, nvidia::Int64 d) override {
        nvidia::Int64 b = o == NvCo::SeekOrigin::START ? 0 : o == NvCo::SeekOrigin::END ? nvidia::Int64(data.size()) : pos;
        pos = std::clamp<nvidia::Int64>(b + d, 0, nvidia::Int64(data.size()));
        return pos;
    }
    nvidia::Int64 tell() override { return pos; }
    nvidia::Void close() override { closed = true; }
    nvidia::Bool isClosed() override { return closed; }
};

// ------------------------------------------------------------------------------------------------ game hair settings
// The values the game's path-traced hair uses come from the user's dx12user.settings: Graphics Control keeps its own
// copies in [GC_Store] / [GC_PTHair] and pushes them at game start (HAIRWORK.md L249 / L251), so those win over the
// engine sections. Missing file or keys = the exe defaults (mode 1, falloff 0.06, 32 hairs per face).
void HairRuntime::loadGameHairSettings() {
    if (const char* e = getenv("HW_GAME_ACCURATE")) gameAccurate = atoi(e) != 0;
    if (const char* e = getenv("HW_PT_WIDTH_SCALE")) ptWidthScale = std::clamp(float(atof(e)), 0.05f, 10.0f);
    std::vector<std::string> cands;
    if (const char* e = getenv("HW_DX12_SETTINGS")) cands.push_back(e);
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
        char buf[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, docs, -1, buf, sizeof buf, nullptr, nullptr);
        cands.push_back(std::string(buf) + "/The Witcher 3/dx12user.settings");
        CoTaskMemFree(docs);
    }
    if (const char* up = getenv("USERPROFILE")) {
        cands.push_back(std::string(up) + "/Documents/The Witcher 3/dx12user.settings");
        cands.push_back(std::string(up) + "/OneDrive/Documents/The Witcher 3/dx12user.settings");
    }
    ptSettingsSource = "defaults";
    for (auto& path : cands) {
        std::ifstream f(path);
        if (!f) continue;
        std::map<std::string, std::string> kv;   // "section|key" -> value (last wins)
        std::string line, sec;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.size() > 1 && line[0] == '[') { sec = line.substr(1, line.find(']') - 1); continue; }
            size_t eq = line.find('=');
            if (eq != std::string::npos) kv[sec + "|" + line.substr(0, eq)] = line.substr(eq + 1);
        }
        auto get = [&](std::initializer_list<const char*> keys, std::string& v) {
            for (auto k : keys) { auto it = kv.find(k); if (it != kv.end()) { v = it->second; return true; } }
            return false;
        };
        std::string v;
        if (get({"GC_Store|PTHairRootAlphaMode", "Rendering/RT/PathTracer|PTHairRootAlphaMode"}, v)) ptRootAlphaMode = std::clamp(atoi(v.c_str()), 0, 2);
        if (get({"GC_PTHair|GCP_PTHairRootAlphaFalloffOverride", "GC_Store|PTHairRootAlphaFalloffOverride",
                 "Rendering/RT/PathTracer|PTHairRootAlphaFalloffOverride"}, v)) ptRootAlphaFalloff = float(atof(v.c_str()));
        if (get({"GC_Store|RTNHairsPerPatch", "Rendering/HairWorks|RTNHairsPerPatch"}, v)) { ptHairsPerFace = std::clamp(atoi(v.c_str()), 1, 64); ptHairsPerFaceGame = ptHairsPerFace; }
        if (get({"GC_PTHair|GCP_PTHairAlphaTestMipLevel"}, v)) ptDensityMipGame = std::clamp(atoi(v.c_str()), 0, 15);
        ptSettingsSource = path;
        break;
    }
}

// ------------------------------------------------------------------------------------------------ loading
bool HairRuntime::load(const std::string& dll, ID3D11Device* d, ID3D11DeviceContext* c) {
    dev = d;
    ctx = c;
    dllPath = dll;
    if (const char* e = getenv("HW_BONE_STIFF_EMU")) boneStiffEmu = std::clamp(float(atof(e)), 0.0f, 1.0f);
    loadGameHairSettings();
    // LOAD_WITH_ALTERED_SEARCH_PATH: the DLL's own folder is searched for its dependencies (d3dcompiler_47.dll)
    HMODULE h = LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { lastError = "cannot load " + dll + " (error " + std::to_string(GetLastError()) + ")"; return false; }
    typedef NvHair::Sdk*(__cdecl * CreateFn)(nvidia::UInt32, NvCo::MemoryAllocator*, NvCo::Logger*, nvidia::Int);
    auto fn = (CreateFn)GetProcAddress(h, "NvHairWorks_Create");
    if (!fn) { lastError = dll + " has no NvHairWorks_Create export (not a HairWorks 1.2 runtime)"; return false; }
    sdk = fn(NV_HAIR_VERSION, nullptr, &g_logger, 0);
    if (!sdk) { lastError = "NvHairWorks_Create failed (version mismatch? header " NV_HAIR_RELEASE_VERSION_STRING ")"; return false; }
    const NvHair::BuildInfo& bi = sdk->getBuildInfo();
    buildString = bi.m_buildString ? bi.m_buildString : "";
    if (NV_FAILED(sdk->initRenderResources(NvCo::Dx11Type::wrap(dev), NvCo::Dx11Type::wrap(ctx)))) {
        lastError = "initRenderResources failed";
        sdk->release();
        sdk = nullptr;
        return false;
    }
    sdk->setCurrentContext(NvCo::Dx11Type::wrap(ctx));
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &samp);
    return true;
}

bool HairRuntime::loadPixelShader(const std::vector<char>& cso) {
    if (ps) { ps->Release(); ps = nullptr; }
    return SUCCEEDED(dev->CreatePixelShader(cso.data(), cso.size(), nullptr, &ps));
}

void HairRuntime::freeInstance() {
    if (readStage) { readStage->Release(); readStage = nullptr; readStageBytes = 0; }
    if (!sdk) return;
    if (instId != NvHair::INSTANCE_ID_NULL) sdk->freeInstance(NvHair::InstanceId(instId));
    if (assetId != NvHair::ASSET_ID_NULL) sdk->freeAsset(NvHair::AssetId(assetId));
    instId = NvHair::INSTANCE_ID_NULL;
    assetId = NvHair::ASSET_ID_NULL;
}

void HairRuntime::shutdown(bool releaseSdk) {
    freeInstance();
    if (sdk && !releaseSdk) sdk = nullptr;   // second runtime: its SDK object is left to process exit (releasing it crashes)
    if (sdk) {
        sdk->freeRenderResources();
        sdk->release();
        sdk = nullptr;
    }
    if (ps) ps->Release();
    if (samp) samp->Release();
    ps = nullptr;
    samp = nullptr;
}

// ------------------------------------------------------------------------------------------------ asset
static gfsdk_float4x4 toF44(const float* m) {
    gfsdk_float4x4 r;
    std::memcpy(&r, m, sizeof(float) * 16);
    return r;
}

bool HairRuntime::build(const DeskPack& pk, const Groom& gr, std::vector<int>& guideOfAsset, const std::vector<std::array<float, 6>>* uvs,
                        const std::vector<char>* hideGuide, const std::vector<char>* hideFace) {
    if (!sdk) return false;
    freeInstance();
    const int K = gr.K;
    std::vector<int> assetOf(gr.P.size(), -1);
    guideOfAsset.clear();
    for (size_t i = 0; i < gr.P.size(); i++)
        if (gr.alive[i] && !(hideGuide && i < hideGuide->size() && (*hideGuide)[i])) { assetOf[i] = (int)guideOfAsset.size(); guideOfAsset.push_back((int)i); }
    const int N = (int)guideOfAsset.size();
    if (N < 3) { lastError = "fewer than 3 guides"; return false; }
    numAssetGuides = N;
    assetCvs = K;
    std::vector<gfsdk_float3> verts(size_t(N) * K);
    std::vector<nvidia::UInt32> ends(N);
    std::vector<gfsdk_float4> bi(N), bw(N);
    for (int a = 0; a < N; a++) {
        int i = guideOfAsset[a];
        for (int k = 0; k < K; k++) {
            V3 p = gr.renderPoint(i, k);
            verts[size_t(a) * K + k] = {p.x, p.y, p.z};
        }
        ends[a] = nvidia::UInt32((a + 1) * K - 1);
        int b = gr.baseOf(i);
        auto& I = pk.boneIdx[b];
        auto& W = pk.boneW[b];
        bi[a] = {I[0], I[1], I[2], I[3]};
        bw[a] = {W[0], W[1], W[2], W[3]};
    }
    std::vector<nvidia::UInt32> fi;
    std::vector<gfsdk_float2> fuv;
    for (size_t fx = 0; fx < gr.faces.size(); fx++) {
        const Face& f = gr.faces[fx];
        if (hideFace && fx < hideFace->size() && (*hideFace)[fx]) continue;
        const auto& fu = (uvs && fx < uvs->size()) ? (*uvs)[fx] : f.uv;
        int a0 = assetOf[f.g[0]], a1 = assetOf[f.g[1]], a2 = assetOf[f.g[2]];
        if (a0 < 0 || a1 < 0 || a2 < 0) continue;
        fi.push_back(a0); fi.push_back(a1); fi.push_back(a2);
        for (int c = 0; c < 3; c++) fuv.push_back({fu[2 * c], fu[2 * c + 1]});
    }
    if (hideGuide || hideFace) {
        // elimination builds only (wind check): a kept guide whose faces were all left out would have no root UV, so the
        // runtime would sample its stiffness / root-stiffness texture at a wrong place (measured 2026-10-14: such guides
        // flew 3x farther than in the full build). Give it a zero-area face (its own corner UV, no hair) instead.
        std::vector<char> has(size_t(N), 0);
        for (auto v : fi) has[v] = 1;
        for (size_t fx = 0; fx < gr.faces.size(); fx++) {
            const Face& f = gr.faces[fx];
            const auto& fu = (uvs && fx < uvs->size()) ? (*uvs)[fx] : f.uv;
            for (int c = 0; c < 3; c++) {
                int a = f.g[c] >= 0 && f.g[c] < (int)assetOf.size() ? assetOf[f.g[c]] : -1;
                if (a < 0 || has[a]) continue;
                has[a] = 1;
                fi.push_back(a); fi.push_back(a); fi.push_back(a);
                for (int q = 0; q < 3; q++) fuv.push_back({fu[2 * c], fu[2 * c + 1]});
            }
        }
    }
    if (fi.empty()) { lastError = "no growth faces"; return false; }
    numBones = (int)pk.bones.size();
    std::vector<char> names(size_t(NV_HAIR_MAX_STRING) * numBones, 0);
    std::vector<gfsdk_float4x4> binds(numBones);
    std::vector<nvidia::Int32> parents(numBones);
    for (int b = 0; b < numBones; b++) {
        strncpy_s(&names[size_t(b) * NV_HAIR_MAX_STRING], NV_HAIR_MAX_STRING, pk.bones[b].name.c_str(), _TRUNCATE);
        binds[b] = toF44(pk.bones[b].bind);
        parents[b] = pk.bones[b].parent;
    }
    std::vector<NvHair::BoneSphere> spheres(pk.spheres.size());
    for (size_t s = 0; s < pk.spheres.size(); s++) {
        spheres[s].m_boneSphereIndex = pk.spheres[s].bone;
        spheres[s].m_boneSphereRadius = s < sphereR.size() && sphereR[s] >= 0 ? sphereR[s] : pk.spheres[s].r;
        spheres[s].m_boneSphereLocalPos = {pk.spheres[s].local.x, pk.spheres[s].local.y, pk.spheres[s].local.z};
    }
    std::vector<nvidia::UInt32> caps;
    for (auto& c : pk.capsules) { caps.push_back(c[0]); caps.push_back(c[1]); }

    NvHair::AssetDescriptor ad;
    ad.m_numGuideHairs = N;
    ad.m_numVertices = N * K;
    ad.m_vertices = verts.data();
    ad.m_endIndices = ends.data();
    ad.m_numFaces = (int)fi.size() / 3;
    ad.m_faceIndices = fi.data();
    ad.m_faceUvs = fuv.data();
    ad.m_numBones = numBones;
    ad.m_boneIndices = bi.data();
    ad.m_boneWeights = bw.data();
    ad.m_boneNames = names.data();
    ad.m_bindPoses = binds.data();
    ad.m_boneParents = parents.data();
    ad.m_numBoneSpheres = (int)spheres.size();
    ad.m_boneSpheres = spheres.empty() ? nullptr : spheres.data();
    ad.m_numBoneCapsules = (int)caps.size() / 2;
    ad.m_boneCapsuleIndices = caps.empty() ? nullptr : caps.data();
    ad.m_sceneUnit = 100.0f;                          // the game's mesh space is in metres
    ad.m_handedness = NvHair::HandednessHint::RIGHT;
    ad.m_upAxis = NvHair::AxisHint::Z_UP;
    NvHair::AssetId aid;
    if (NV_FAILED(sdk->createAsset(ad, aid))) { lastError = "createAsset failed"; return false; }
    assetId = aid;
    if (!getenv("HW_DIRECT_ASSET")) {
        // createAsset + collision spheres crashed in updateSkinningMatrices with this runtime (1.2.1 build of 2016);
        // the same data serialised to .apx and loaded back through loadAsset works: use the loader path.
        MemWrite w;
        if (NV_SUCCEEDED(sdk->saveAsset(&w, NvHair::SerializeFormat::BINARY, aid))) {
            lastApx = w.data;
            sdk->freeAsset(aid);
            assetId = NvHair::ASSET_ID_NULL;
            MemRead r(lastApx);
            if (NV_FAILED(sdk->loadAsset(&r, aid))) { lastError = "loadAsset (round trip) failed"; return false; }
            assetId = aid;
        }
    }
    NvHair::InstanceId iid;
    if (NV_FAILED(sdk->createInstance(aid, iid))) { lastError = "createInstance failed"; return false; }
    instId = iid;
    if (densitySrv) sdk->setTexture(iid, NvHair::TextureType::DENSITY, NvCo::Dx11Type::wrap(densitySrv));
    densityBound = densitySrv;
    if (colorSrv) {
        sdk->setTexture(iid, NvHair::TextureType::ROOT_COLOR, NvCo::Dx11Type::wrap(colorSrv));
        sdk->setTexture(iid, NvHair::TextureType::TIP_COLOR, NvCo::Dx11Type::wrap(colorSrv));
    }
    if (stiffSrv) {
        sdk->setTexture(iid, NvHair::TextureType::STIFFNESS, NvCo::Dx11Type::wrap(stiffSrv));
        sdk->setTexture(iid, NvHair::TextureType::ROOT_STIFFNESS, NvCo::Dx11Type::wrap(stiffSrv));
    }
    stepped = false;
    return true;
}

void HairRuntime::setStiffnessTexture(ID3D11ShaderResourceView* srv) {
    stiffSrv = srv;
    if (sdk && instId != NvHair::INSTANCE_ID_NULL && srv) {
        sdk->setTexture(NvHair::InstanceId(instId), NvHair::TextureType::STIFFNESS, NvCo::Dx11Type::wrap(srv));
        sdk->setTexture(NvHair::InstanceId(instId), NvHair::TextureType::ROOT_STIFFNESS, NvCo::Dx11Type::wrap(srv));
    }
}

void HairRuntime::setColorTexture(ID3D11ShaderResourceView* srv) {
    colorSrv = srv;
    if (sdk && instId != NvHair::INSTANCE_ID_NULL && srv) {
        sdk->setTexture(NvHair::InstanceId(instId), NvHair::TextureType::ROOT_COLOR, NvCo::Dx11Type::wrap(srv));
        sdk->setTexture(NvHair::InstanceId(instId), NvHair::TextureType::TIP_COLOR, NvCo::Dx11Type::wrap(srv));
    }
}

void HairRuntime::setDensityTexture(ID3D11ShaderResourceView* srv) {
    densitySrv = srv;
    if (sdk && instId != NvHair::INSTANCE_ID_NULL) {
        sdk->setTexture(NvHair::InstanceId(instId), NvHair::TextureType::DENSITY, NvCo::Dx11Type::wrap(srv));
        densityBound = srv;
    }
}

// ------------------------------------------------------------------------------------------------ descriptor
static float s2l(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

static gfsdk_float4 color(const json& c, gfsdk_float4 def) {
    if (c.is_array() && c.size() >= 3 && c[0].is_number()) {
        // 2026-10-17 (L593): a look colour stored as [r, g, b(, a)] (agent look_set, exported look files) rendered as the
        // default (white) while the game used it: accept the list, 0-255 (or 0..1 when every value is <= 1)
        float m = 1.0f;
        for (size_t i = 0; i < 3; i++) if (c[i].get<float>() > 1.0f) m = 255.0f;
        return {s2l(c[0].get<float>() / m), s2l(c[1].get<float>() / m), s2l(c[2].get<float>() / m),
                c.size() > 3 && c[3].is_number() ? c[3].get<float>() / m : 1.0f};
    }
    if (!c.is_object()) return def;
    return {s2l(c.value("Red", 255) / 255.0f), s2l(c.value("Green", 255) / 255.0f), s2l(c.value("Blue", 255) / 255.0f),
            c.value("Alpha", 255) / 255.0f};
}

static gfsdk_float4 curve(const json& c) {
    if (!c.is_object()) return {1, 1, 1, 1};
    return {c.value("X", 1.0f), c.value("Y", 1.0f), c.value("Z", 1.0f), c.value("W", 1.0f)};
}

static NvHair::ETextureChannel channel(const json& v) {
    std::string s = v.is_string() ? v.get<std::string>() : "RED";
    if (s == "GREEN") return NvHair::TextureChannel::GREEN;
    if (s == "BLUE") return NvHair::TextureChannel::BLUE;
    if (s == "ALPHA") return NvHair::TextureChannel::ALPHA;
    return NvHair::TextureChannel::RED;
}

void HairRuntime::updateDescriptor(const Look& L, const XMFLOAT3& wind, bool simulate, bool showGuides,
                                   bool showCollision, bool showGrowth, bool forceCloseLod, int colorize, bool stiffMap) {
    if (!sdk || instId == NvHair::INSTANCE_ID_NULL) return;
    NvHair::InstanceDescriptor d;   // HairWorks defaults, then every value the fur stores
    auto F = [&](const char* p, float def) { return L.f(p, def); };
    const std::string pm = "physicalMaterials.", gm = "graphicalMaterials.", lo = "levelOfDetail.";
    // volume / width
    d.m_density = F("physicalMaterials.volume.density", 1.0f);
    d.m_usePixelDensity = F("physicalMaterials.volume.usePixelDensity", 0) > 0.5f;
    d.m_lengthNoise = F("physicalMaterials.volume.lengthNoise", 1.0f);
    d.m_lengthScale = F("physicalMaterials.volume.lengthScale", 1.0f);
    d.m_width = F("physicalMaterials.strandWidth.width", 1.0f);
    d.m_widthRootScale = F("physicalMaterials.strandWidth.widthRootScale", 1.0f);
    d.m_widthTipScale = F("physicalMaterials.strandWidth.widthTipScale", 0.1f);
    d.m_widthNoise = F("physicalMaterials.strandWidth.widthNoise", 0.0f);
    // clumping / waves
    d.m_clumpScale = F("physicalMaterials.clumping.clumpScale", 0.0f);
    d.m_clumpNoise = F("physicalMaterials.clumping.clumpNoise", 0.0f);
    d.m_clumpRoundness = F("physicalMaterials.clumping.clumpRoundness", 1.0f);
    d.m_waveScale = F("physicalMaterials.waveness.waveScale", 0.0f);
    d.m_waveScaleNoise = F("physicalMaterials.waveness.waveScaleNoise", 0.5f);
    d.m_waveFreq = F("physicalMaterials.waveness.waveFreq", 3.0f);
    d.m_waveFreqNoise = F("physicalMaterials.waveness.waveFreqNoise", 0.5f);
    d.m_waveRootStraighten = F("physicalMaterials.waveness.waveRootStraighten", 0.0f);
    // 2026-10-16 (FINDINGS 10.4, user OK): the engine always sends waveStrand 1 / waveClump 0 whatever the fur says
    // (L530), so the DEFAULT preview draws them that way too; the fur's values are still exported, never read here
    d.m_waveScaleStrand = 1.0f;
    d.m_waveScaleClump = 0.0f;
    // simulation
    d.m_simulate = simulate;
    d.m_massScale = F("physicalMaterials.simulation.massScale", 10.0f);
    d.m_damping = F("physicalMaterials.simulation.damping", 0.0f);
    d.m_backStopRadius = F("physicalMaterials.simulation.backStopRadius", 0.0f);
    d.m_inertiaLimit = F("physicalMaterials.simulation.inertiaLimit", 1000.0f);
    d.m_inertiaScale = F("physicalMaterials.simulation.inertiaScale", 1.0f);
    d.m_friction = F("physicalMaterials.simulation.friction", 0.0f);
    d.m_useCollision = F("physicalMaterials.simulation.useCollision", 0) > 0.5f;
    json g = L.get("physicalMaterials.simulation.gravityDir");
    if (g.is_object()) {
        float gx = g.value("X", 0.0f), gy = g.value("Y", 0.0f), gz = g.value("Z", -1.0f);
        float n = std::sqrt(gx * gx + gy * gy + gz * gz);
        if (n > 1e-6f) d.m_gravityDir = {gx / n, gy / n, gz / n};
    }
    float ws = F("physicalMaterials.simulation.windScaler", 1.0f);
    d.m_wind = {wind.x * ws, wind.y * ws, wind.z * ws};
    d.m_windNoise = F("physicalMaterials.simulation.windNoise", 0.0f);
    // stiffness
    d.m_stiffness = F("physicalMaterials.stiffness.stiffness", 0.5f);
    d.m_stiffnessStrength = F("physicalMaterials.stiffness.stiffnessStrength", 1.0f);
    d.m_stiffnessDamping = F("physicalMaterials.stiffness.stiffnessDamping", 0.0f);
    d.m_rootStiffness = F("physicalMaterials.stiffness.rootStiffness", 0.5f);
    d.m_tipStiffness = F("physicalMaterials.stiffness.tipStiffness", 0.0f);
    d.m_bendStiffness = F("physicalMaterials.stiffness.bendStiffness", 0.0f);
    d.m_interactionStiffness = F("physicalMaterials.stiffness.interactionStiffness", 0.0f);
    d.m_pinStiffness = F("physicalMaterials.stiffness.pinStiffness", 1.0f);
    d.m_stiffnessCurve = curve(L.get("physicalMaterials.stiffness.stiffnessCurve"));
    d.m_stiffnessStrengthCurve = curve(L.get("physicalMaterials.stiffness.stiffnessStrengthCurve"));
    d.m_stiffnessDampingCurve = curve(L.get("physicalMaterials.stiffness.stiffnessDampingCurve"));
    d.m_bendStiffnessCurve = curve(L.get("physicalMaterials.stiffness.bendStiffnessCurve"));
    d.m_interactionStiffnessCurve = curve(L.get("physicalMaterials.stiffness.interactionStiffnessCurve"));
    // W3 bone-distance stiffness emulation (see hairsdk.h): hold the strand shape like the game does
    boneStiffInFur = F("physicalMaterials.stiffness.stiffnessBoneEnable", 0.0f) > 0.5f;
    if (boneStiffInFur && boneStiffEmu > 0.0f) {
        float k = std::clamp(boneStiffEmu, 0.0f, 1.0f);
        // L480: the game still lets low-stiffness hair fall under the bone block (stiffness 0 -> crown bald): scale the pull
        if (gameAccurate && ptStiffByFur) k *= std::clamp(0.5f * (d.m_stiffness + d.m_rootStiffness), 0.0f, 1.0f);
        gfsdk_float4 bc = curve(L.get("physicalMaterials.stiffness.stiffnessBoneCurve"));
        auto up = [&](gfsdk_float4& c) {
            c.x += (1.0f - c.x) * k * bc.x; c.y += (1.0f - c.y) * k * bc.y;
            c.z += (1.0f - c.z) * k * bc.z; c.w += (1.0f - c.w) * k * bc.w;
        };
        up(d.m_stiffnessCurve);
        up(d.m_stiffnessStrengthCurve);
        up(d.m_bendStiffnessCurve);
        d.m_bendStiffness += (1.0f - d.m_bendStiffness) * k;
    }
    // shading
    d.m_rootColor = color(L.get("graphicalMaterials.color.rootColor"), d.m_rootColor);
    d.m_tipColor = color(L.get("graphicalMaterials.color.tipColor"), d.m_tipColor);
    d.m_rootTipColorWeight = F("graphicalMaterials.color.rootTipColorWeight", 0.5f);
    d.m_rootTipColorFalloff = F("graphicalMaterials.color.rootTipColorFalloff", 1.0f);
    d.m_rootAlphaFalloff = F("graphicalMaterials.color.rootAlphaFalloff", 0.0f);
    d.m_strandBlendScale = F("graphicalMaterials.color.strandBlendScale", 1.0f);
    d.m_diffuseBlend = F("graphicalMaterials.diffuse.diffuseBlend", 0.5f);
    d.m_hairNormalWeight = F("graphicalMaterials.diffuse.diffuseHairNormalWeight", 0.0f);
    {
        int nb = (int)F("graphicalMaterials.diffuse.diffuseBoneIndex", -1.0f);
        d.m_hairNormalBoneIndex = (nb >= 0 && nb < numBones) ? nb : -1;
    }
    // specular colour is a tint / intensity, not an albedo: used as stored (0..255 -> 0..1), not linearised
    {
        const json& sc = L.get("graphicalMaterials.specular.specularColor");
        if (sc.is_object()) d.m_specularColor = {sc.value("Red", 255) / 255.0f, sc.value("Green", 255) / 255.0f, sc.value("Blue", 255) / 255.0f, 1.0f};
    }
    d.m_specularPrimary = F("graphicalMaterials.specular.specularPrimary", 0.1f);
    d.m_specularPowerPrimary = F("graphicalMaterials.specular.specularPowerPrimary", 100.0f);
    d.m_specularPrimaryBreakup = F("graphicalMaterials.specular.specularPrimaryBreakup", 0.0f);
    d.m_specularSecondary = F("graphicalMaterials.specular.specularSecondary", 0.05f);
    d.m_specularPowerSecondary = F("graphicalMaterials.specular.specularPowerSecondary", 20.0f);
    d.m_specularSecondaryOffset = F("graphicalMaterials.specular.specularSecondaryOffset", 0.1f);
    d.m_specularNoiseScale = std::clamp(F("graphicalMaterials.specular.specularNoiseScale", 0.0f), 0.0f, 1.0f);
    d.m_specularEnvScale = F("graphicalMaterials.specular.specularEnvScale", 0.25f);
    d.m_glintStrength = F("graphicalMaterials.glint.glintStrength", 0.0f);
    d.m_glintCount = F("graphicalMaterials.glint.glintCount", 256.0f);
    d.m_glintExponent = F("graphicalMaterials.glint.glintExponent", 2.0f);
    d.m_shadowSigma = F("graphicalMaterials.shadow.shadowSigma", 0.2f);
    d.m_shadowDensityScale = F("graphicalMaterials.shadow.shadowDensityScale", 0.5f);
    d.m_castShadows = F("graphicalMaterials.shadow.castShadows", 1.0f) > 0.5f;
    d.m_receiveShadows = F("graphicalMaterials.shadow.receiveShadows", 1.0f) > 0.5f;
    // LOD: always on (performance rule), values from the fur
    d.m_enableLod = true;
    d.m_enableDistanceLod = F("levelOfDetail.distanceLOD.enableDistanceLOD", 1.0f) > 0.5f;
    d.m_distanceLodStart = F("levelOfDetail.distanceLOD.distanceLODStart", 5.0f);
    d.m_distanceLodEnd = F("levelOfDetail.distanceLOD.distanceLODEnd", 10.0f);
    d.m_distanceLodFadeStart = F("levelOfDetail.distanceLOD.distanceLODFadeStart", 1000.0f);
    d.m_distanceLodDensity = F("levelOfDetail.distanceLOD.distanceLODDensity", 0.0f);
    d.m_distanceLodWidth = F("levelOfDetail.distanceLOD.distanceLODWidth", 1.0f);
    d.m_enableDetailLod = F("levelOfDetail.detailLOD.enableDetailLOD", 1.0f) > 0.5f;
    d.m_detailLodStart = F("levelOfDetail.detailLOD.detailLODStart", 2.0f);
    d.m_detailLodEnd = F("levelOfDetail.detailLOD.detailLODEnd", 1.0f);
    d.m_detailLodDensity = F("levelOfDetail.detailLOD.detailLODDensity", 1.0f);
    d.m_detailLodWidth = F("levelOfDetail.detailLOD.detailLODWidth", 1.0f);
    if (forceCloseLod) { d.m_detailLodStart = 1000.0f; d.m_detailLodEnd = 999.0f; }
    if (gameAccurate) {
        // what the game's path-traced strand generation does (see hairsdk.h): fixed hairs per growth face, no LOD,
        // per-hair density texture, width as stored (x calibration), root transparency from the game's settings
        d.m_density = float(ptHairsPerFace) / 64.0f;
        d.m_enableDetailLod = false;
        d.m_enableDistanceLod = false;
        // detail-LOD emulation (hairsdk.h ptLodEmu): the game's close-up LOD blends the width to detailLODWidth and the hair
        // count by detailLODDensity at the head-shot distance (L480); enableLOD off = the fur's density scalar (Anna: 0)
        ptLodTUsed = 0.0f; ptLodWidthEff = d.m_width; ptLodKeep = 1.0f;
        if (ptLodEmu) {
            bool furLod = F("levelOfDetail.enableLOD", 1.0f) > 0.5f;
            bool det = F("levelOfDetail.detailLOD.enableDetailLOD", 1.0f) > 0.5f;
            // a beard pack keeps its `lod` empty for the default preview; its own block (lodBeard) feeds this emulation
            const json& lb = L.pack ? L.pack->lodBeard : json();
            if (lb.is_object() && !lb.empty()) {
                furLod = lb.value("enableLOD", 1) > 0;
                const json dl = lb.value("detailLOD", json::object());
                det = dl.value("enableDetailLOD", 1) > 0;
                d.m_detailLodStart = dl.value("detailLODStart", d.m_detailLodStart); d.m_detailLodEnd = dl.value("detailLODEnd", d.m_detailLodEnd);
                d.m_detailLodWidth = dl.value("detailLODWidth", d.m_detailLodWidth); d.m_detailLodDensity = dl.value("detailLODDensity", d.m_detailLodDensity);
            }
            float t = 0.0f;
            if (furLod && det) {
                float s0 = d.m_detailLodStart, s1 = d.m_detailLodEnd;
                float dl = std::max(ptCamDistSdk, 0.0f) * ptLodDistScale;
                t = s0 > s1 ? std::clamp((s0 - dl) / (s0 - s1), 0.0f, 1.0f) : (dl <= s0 ? 1.0f : 0.0f);
            }
            if (ptLodT >= 0.0f) t = std::min(ptLodT, 1.0f);
            ptLodTUsed = t;
            d.m_width = d.m_width + (d.m_detailLodWidth - d.m_width) * t;
            ptLodWidthEff = d.m_width;
            float keep = furLod ? std::clamp(1.0f + (d.m_detailLodDensity - 1.0f) * t, 0.0f, 1.0f) : std::clamp(F("physicalMaterials.volume.density", 1.0f), 0.0f, 1.0f);
            ptLodKeep = keep;
            d.m_density *= keep;
            if (ptCountExp > 0.0f) d.m_width *= std::pow(std::max(keep * float(ptHairsPerFace) / 32.0f, 0.02f), -ptCountExp);
        }
        if (ptNeutralise) {
            // the game ignores these (L480): the preview takes the calibration constants, not the fur's knobs
            d.m_specularPrimary = 0.0945f; d.m_specularSecondary = 0.05f;
            d.m_hairNormalWeight = 0.65f;
            d.m_shadowSigma = 1.56f; d.m_shadowDensityScale = 0.5f;
        }
        // L390 ablation (Anna, mode 0): the path tracer ignores the fur's width (0.296 / 0.592 / 1.184 look the same in
        // game, as the hook's descriptor width force did in L384) but follows widthRootScale / widthTipScale. ptWidthConst > 0
        // replaces the width by that constant (the root / tip scales still shape it)
        // L391: not ignored but compressed: the in-game thickness follows ~ width^0.1 (ptWidthExp)
        if (ptWidthConst > 0.0f) d.m_width = ptWidthConst * std::pow(std::max(d.m_width, 1e-4f), ptWidthExp);
        // width floor (2026-10-16, Olgierd beard): in game a very small fur width does not thin the strands linearly
        // (L391: Anna at width 0.059 covers like a Studio width ~0.195; fit 0.221 x width^0.1). Below the crossover
        // (~0.187) the floor wins; every fur at or above it is untouched. The old (non game-accurate) preview hid tiny widths
        // behind the 1.2.1 detail LOD, whose detailLODWidth REPLACES the width (absolute, default 1.0)
        else if (ptWidthFloor && !ptLodEmu) d.m_width = std::max(d.m_width, 0.221f * std::pow(std::max(d.m_width, 1e-4f), 0.1f));
        if (ptTipScale >= 0.0f) d.m_widthTipScale = ptTipScale;   // calibration: tip width scale override (L393)
        d.m_width *= ptWidthScale;
        if (ptWidthConst > 0.0f && ptScaleExp != 1.0f) {
            d.m_widthRootScale = std::pow(std::max(d.m_widthRootScale, 0.0f), ptScaleExp);
            d.m_widthTipScale = std::pow(std::max(d.m_widthTipScale, 0.0f), ptScaleExp);
        }
        ptEffWidth = d.m_width * 0.5f * (d.m_widthRootScale + d.m_widthTipScale);
        if (ptRootAlphaMode == 0) d.m_rootAlphaFalloff = 0.0f;
        else if (ptRootAlphaFalloff >= 0.0f) d.m_rootAlphaFalloff = ptRootAlphaFalloff;
        // the game never takes the fur's waveClump / waveStrand: every in-game descriptor dump (hook logs, 2301 distinct
        // descriptors incl. Guillaume's waveClump 1 / waveStrand .1) holds the defaults 0 / 1 (HAIRWORK.md "Studio game-accurate preview")
        d.m_waveScaleClump = 0.0f;
        d.m_waveScaleStrand = 1.0f;
        d.m_waveScale *= ptWaveScale;
    }
    d.m_useViewfrustrumCulling = true;
    d.m_useBackfaceCulling = false;
    d.m_splineMultiplier = std::max(1, L.pack->splineMultiplier);
    if (gameAccurate && ptSpline > 0) d.m_splineMultiplier = ptSpline;
    splineMul = d.m_splineMultiplier;
    d.m_textureChannels[NvHair::TextureType::DENSITY] = channel(L.get("physicalMaterials.volume.densityTexChannel"));
    if (!densitySrv) d.m_usePixelDensity = false;
    {
        // ptDensityMip >= 0: a view of the density map that starts at that mip, so the SDK's per-hair sample reads the
        // averaged map as the game does at GCP_PTHairAlphaTestMipLevel (opt-in, L394/L395)
        ID3D11ShaderResourceView* want = (densityGameOnly && !gameAccurate) ? nullptr : densitySrv;
        if (gameAccurate && ptDensityMip >= 0 && densitySrv) {
            if (densityMipFor != densitySrv || densityMipFor_ != ptDensityMip) {
                if (densityMipSrv) { densityMipSrv->Release(); densityMipSrv = nullptr; }
                ID3D11Resource* res = nullptr; densitySrv->GetResource(&res);
                ID3D11Texture2D* t2 = nullptr;
                if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2))) {
                    D3D11_TEXTURE2D_DESC td; t2->GetDesc(&td);
                    D3D11_SHADER_RESOURCE_VIEW_DESC sd; densitySrv->GetDesc(&sd);
                    if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
                        sd.Texture2D.MostDetailedMip = std::min<UINT>(UINT(ptDensityMip), td.MipLevels - 1);
                        sd.Texture2D.MipLevels = UINT(-1);
                        ID3D11Device* dev = nullptr; t2->GetDevice(&dev);
                        if (dev) { dev->CreateShaderResourceView(t2, &sd, &densityMipSrv); dev->Release(); }
                    }
                    t2->Release();
                }
                if (res) res->Release();
                densityMipFor = densitySrv; densityMipFor_ = ptDensityMip;
            }
            if (densityMipSrv) want = densityMipSrv;
        }
        if (want != densityBound) {
            sdk->setTexture(NvHair::InstanceId(instId), NvHair::TextureType::DENSITY, NvCo::Dx11Type::wrap(want));
            densityBound = want;
        }
    }
    // painted stiffness map: the redfur fields stiffnessTex / rootStiffnessTex (+ ...TexChannel) map 1:1 to these
    d.m_textureChannels[NvHair::TextureType::STIFFNESS] = NvHair::TextureChannel::RED;
    d.m_textureChannels[NvHair::TextureType::ROOT_STIFFNESS] = NvHair::TextureChannel::GREEN;
    (void)stiffMap;
    // debug views
    d.m_visualizeGuideHairs = showGuides;
    d.m_visualizeBones = visBones;
    d.m_visualizeCapsules = showCollision;
    d.m_visualizeGrowthMesh = showGrowth;
    d.m_colorizeMode = colorize;
    g_desc = d;
    sdk->updateInstanceDescriptor(NvHair::InstanceId(instId), d);
}

std::string HairRuntime::paramDump(const Look& L) const {
    // fur value | value in the last instance descriptor sent to the SDK (after defaults / emulation)
    const NvHair::InstanceDescriptor& d = g_desc;
    json o = json::object();
    auto c4 = [](const gfsdk_float4& c) { return json::array({c.x, c.y, c.z, c.w}); };
    auto row = [&](const std::string& p, json sdkv) { o[p] = {{"fur", L.get(p)}, {"sdk", sdkv}}; };
    const std::string s = "physicalMaterials.simulation.", t = "physicalMaterials.stiffness.";
    row(s + "massScale", d.m_massScale); row(s + "damping", d.m_damping); row(s + "backStopRadius", d.m_backStopRadius);
    row(s + "inertiaLimit", d.m_inertiaLimit); row(s + "inertiaScale", d.m_inertiaScale); row(s + "friction", d.m_friction);
    row(s + "useCollision", d.m_useCollision); row(s + "windNoise", d.m_windNoise);
    row(s + "gravityDir", json::array({d.m_gravityDir.x, d.m_gravityDir.y, d.m_gravityDir.z}));
    row(s + "windScaler", json::array({d.m_wind.x, d.m_wind.y, d.m_wind.z}));
    row(t + "stiffness", d.m_stiffness); row(t + "stiffnessStrength", d.m_stiffnessStrength);
    row(t + "stiffnessDamping", d.m_stiffnessDamping); row(t + "rootStiffness", d.m_rootStiffness);
    row(t + "tipStiffness", d.m_tipStiffness); row(t + "bendStiffness", d.m_bendStiffness);
    row(t + "interactionStiffness", d.m_interactionStiffness); row(t + "pinStiffness", d.m_pinStiffness);
    row(t + "stiffnessCurve", c4(d.m_stiffnessCurve)); row(t + "stiffnessStrengthCurve", c4(d.m_stiffnessStrengthCurve));
    row(t + "stiffnessDampingCurve", c4(d.m_stiffnessDampingCurve)); row(t + "bendStiffnessCurve", c4(d.m_bendStiffnessCurve));
    row(t + "interactionStiffnessCurve", c4(d.m_interactionStiffnessCurve));
    for (const char* k : {"stiffnessBoneEnable", "stiffnessBoneIndex", "stiffnessBoneAxis", "stiffnessStartDistance",
                          "stiffnessEndDistance", "stiffnessBoneCurve"})
        row(t + k, boneStiffInFur ? json("emulated (boneStiffEmu " + std::to_string(boneStiffEmu) + ")") : json("not in runtime"));
    row("physicalMaterials.volume.density", d.m_density); row("physicalMaterials.volume.lengthNoise", d.m_lengthNoise);
    row("physicalMaterials.strandWidth.width", d.m_width);
    row("physicalMaterials.clumping.clumpScale", d.m_clumpScale);
    row("physicalMaterials.waveness.waveScale", d.m_waveScale); row("physicalMaterials.waveness.waveFreq", d.m_waveFreq);
    row("physicalMaterials.waveness.waveStrand", d.m_waveScaleStrand); row("physicalMaterials.waveness.waveClump", d.m_waveScaleClump);
    row("physicalMaterials.waveness.waveRootStraighten", d.m_waveRootStraighten);
    o["_runtime"] = {{"sceneUnit_cm", 100.0}, {"step_s", 1.0 / 60.0}, {"simulate", d.m_simulate},
                     {"splineMultiplier", d.m_splineMultiplier}, {"boneStiffEmu", boneStiffEmu}, {"boneStiffInFur", boneStiffInFur}};
    return o.dump(1);
}

void HairRuntime::skin(const std::vector<XMFLOAT4X4>& M, bool teleport) {
    if (!sdk || instId == NvHair::INSTANCE_ID_NULL || (int)M.size() != numBones) return;
    sdk->setCurrentContext(NvCo::Dx11Type::wrap(ctx));
    if (!stepped) { sdk->stepInstanceSimulation(NvHair::InstanceId(instId), 1.0f / 60.0f); stepped = true; }
    sdk->updateSkinningMatrices(NvHair::InstanceId(instId), numBones, (const gfsdk_float4x4*)M.data(),
                                teleport ? NvHair::TeleportMode::WITH_SKINNED_POSITION : NvHair::TeleportMode::NONE);
}

void HairRuntime::step(float dt) {
    if (!sdk || instId == NvHair::INSTANCE_ID_NULL) return;
    sdk->setCurrentContext(NvCo::Dx11Type::wrap(ctx));
    sdk->stepInstanceSimulation(NvHair::InstanceId(instId), dt);
    stepped = true;
}

void HairRuntime::settle(int frames, float dt) {
    for (int i = 0; i < frames; i++) step(dt);
}

void HairRuntime::render(const XMFLOAT4X4& view, const XMFLOAT4X4& proj, float fovDeg, float w, float h, ID3D11Buffer* lightCb, bool depthOnly) {
    if (!sdk || instId == NvHair::INSTANCE_ID_NULL || !ps) return;
    NvHair::InstanceId iid = NvHair::InstanceId(instId);
    sdk->setCurrentContext(NvCo::Dx11Type::wrap(ctx));
    NvHair::Viewport vp;
    vp.init(0, 0, w, h);
    sdk->setViewProjection(vp, (const gfsdk_float4x4&)view, (const gfsdk_float4x4&)proj, NvHair::HandednessHint::RIGHT, fovDeg);
    if (!stepped) step(1.0f / 60.0f);
    sdk->preRenderInstance(iid, 1.0f);
    if (depthOnly) {
        ctx->PSSetShader(nullptr, nullptr, 0);
        sdk->renderHairs(iid);
        return;
    }
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &samp);
    ctx->PSSetConstantBuffers(1, 1, &lightCb);
    ID3D11ShaderResourceView* srvs[NvHair::ShaderResourceType::COUNT_OF] = {};
    sdk->getShaderResources(iid, nullptr, NvHair::ShaderResourceType::COUNT_OF, NvCo::Dx11Type::wrapPtr(srvs));
    ctx->PSSetShaderResources(0, 3, srvs);
    ID3D11ShaderResourceView* col[2] = {colorSrv, colorSrv};
    ctx->PSSetShaderResources(3, 2, col);
    sdk->renderHairs(iid);
    sdk->renderVisualization(iid);
    ID3D11ShaderResourceView* nulls[5] = {};
    ctx->PSSetShaderResources(0, 5, nulls);
}

void HairRuntime::renderShadow(const XMFLOAT4X4& view, const XMFLOAT4X4& proj, float size) {
    if (!sdk || instId == NvHair::INSTANCE_ID_NULL) return;
    NvHair::InstanceId iid = NvHair::InstanceId(instId);
    sdk->setCurrentContext(NvCo::Dx11Type::wrap(ctx));
    NvHair::Viewport vp;
    vp.init(0, 0, size, size);
    sdk->setViewProjection(vp, (const gfsdk_float4x4&)view, (const gfsdk_float4x4&)proj, NvHair::HandednessHint::RIGHT, 30.0f);
    if (!stepped) step(1.0f / 60.0f);
    sdk->preRenderInstance(iid, 1.0f);
    ctx->PSSetShader(nullptr, nullptr, 0);
    NvHair::ShaderSettings ss(false, true);
    sdk->renderHairs(iid, &ss);
}

HairStats HairRuntime::stats() {
    HairStats s;
    if (!sdk || instId == NvHair::INSTANCE_ID_NULL) return s;
    NvHair::Stats st;
    if (NV_SUCCEEDED(sdk->computeStats(nullptr, false, NvHair::InstanceId(instId), st))) {
        s.hairs = st.m_numHairs;
        s.faces = st.m_numFaces;
        s.avgDensity = st.m_averageDensity;
        s.detailLod = st.m_detailLodFactor;
        s.distanceLod = st.m_distanceLodFactor;
        s.camDist = st.m_camDistance;
    }
    return s;
}

// ------------------------------------------------------------------------------------------------ guide readback
bool HairRuntime::readGuides(std::vector<float>& xyz, int& perGuide, std::string* info) {
    auto fail = [&](const std::string& why) { if (info) *info = why; return false; };
    if (!sdk || instId == NvHair::INSTANCE_ID_NULL || numAssetGuides <= 0) return fail("no instance");
    sdk->setCurrentContext(NvCo::Dx11Type::wrap(ctx));
    NvHair::EShaderResourceType t = NvHair::ShaderResourceType::MASTER_POSITIONS;
    ID3D11ShaderResourceView* srv = nullptr;
    if (NV_FAILED(sdk->getShaderResources(NvHair::InstanceId(instId), &t, 1, NvCo::Dx11Type::wrapPtr(&srv))) || !srv) return fail("no position resource");
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    srv->GetDesc(&vd);
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    ID3D11Buffer* buf = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Buffer), (void**)&buf);
    if (res) res->Release();
    if (!buf) return fail("position resource is not a buffer");
    D3D11_BUFFER_DESC bd;
    buf->GetDesc(&bd);
    UINT first = 0, num = 0, elem = 0;
    if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) { first = vd.Buffer.FirstElement; num = vd.Buffer.NumElements; }
    else if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) { first = vd.BufferEx.FirstElement; num = vd.BufferEx.NumElements; }
    bool half = false;
    if (bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) elem = bd.StructureByteStride;
    else if (vd.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) elem = 16;
    else if (vd.Format == DXGI_FORMAT_R32G32B32_FLOAT) elem = 12;
    else if (vd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) { elem = 8; half = true; }
    char b[256];
    snprintf(b, sizeof b, "view dim %d format %d first %u num %u, buffer %u bytes misc %u stride %u, %d guides", int(vd.ViewDimension), int(vd.Format),
             first, num, bd.ByteWidth, bd.MiscFlags, bd.StructureByteStride, numAssetGuides);
    if (info) *info = b;
    if (elem != 12 && elem != 16 && elem != 8) { buf->Release(); return fail(std::string(b) + ": unknown element type"); }
    if (num == 0) num = bd.ByteWidth / elem - first;
    perGuide = (assetCvs - 1) * std::max(1, splineMul) + 1;
    if (assetCvs < 2 || num < UINT(perGuide) * UINT(numAssetGuides)) { buf->Release(); return fail(std::string(b) + ": smaller than guides x refined CVs"); }
    num = UINT(perGuide) * UINT(numAssetGuides);
    if (!readStage || readStageBytes != bd.ByteWidth) {
        if (readStage) readStage->Release();
        readStage = nullptr;
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = bd.ByteWidth;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&sd, nullptr, &readStage))) { buf->Release(); return fail("staging buffer failed"); }
        readStageBytes = bd.ByteWidth;
    }
    D3D11_BOX box = {0, 0, 0, bd.ByteWidth, 1, 1};
    ctx->CopySubresourceRegion(readStage, 0, 0, 0, 0, buf, 0, &box);
    buf->Release();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(readStage, 0, D3D11_MAP_READ, 0, &m))) return fail("map failed");
    xyz.resize(size_t(num) * 3);
    const unsigned char* p = (const unsigned char*)m.pData + size_t(first) * elem;
    auto h2f = [](uint16_t h) {
        uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1f, f = h & 0x3ff, o;
        if (e == 0) { float v = std::ldexp(float(f), -24); return s ? -v : v; }
        if (e == 31) o = s | 0x7f800000u | (f << 13);
        else o = s | ((e + 112) << 23) | (f << 13);
        float r;
        memcpy(&r, &o, 4);
        return r;
    };
    for (UINT i = 0; i < num; i++) {
        const unsigned char* q = p + size_t(i) * elem;
        if (half) {
            uint16_t hv[3];
            memcpy(hv, q, 6);
            for (int c = 0; c < 3; c++) xyz[size_t(i) * 3 + c] = h2f(hv[c]);
        } else memcpy(&xyz[size_t(i) * 3], q, 12);
    }
    ctx->Unmap(readStage, 0);
    return true;
}
