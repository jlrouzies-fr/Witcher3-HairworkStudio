// Desktop Groom Studio - HairWorks runtime wrapper. Our own code (MIT), written against the public API header
// (NvHairSdk.h) of the HairWorks SDK the user has locally; no SDK source, sample or shader code is copied here.
#include "hairsdk.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

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

// ------------------------------------------------------------------------------------------------ loading
bool HairRuntime::load(const std::string& dll, ID3D11Device* d, ID3D11DeviceContext* c) {
    dev = d;
    ctx = c;
    dllPath = dll;
    if (const char* e = getenv("HW_BONE_STIFF_EMU")) boneStiffEmu = std::clamp(float(atof(e)), 0.0f, 1.0f);
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

bool HairRuntime::build(const DeskPack& pk, const Groom& gr, std::vector<int>& guideOfAsset, const std::vector<std::array<float, 6>>* uvs) {
    if (!sdk) return false;
    freeInstance();
    const int K = pk.K;
    std::vector<int> assetOf(gr.P.size(), -1);
    guideOfAsset.clear();
    for (size_t i = 0; i < gr.P.size(); i++)
        if (gr.alive[i]) { assetOf[i] = (int)guideOfAsset.size(); guideOfAsset.push_back((int)i); }
    const int N = (int)guideOfAsset.size();
    if (N < 3) { lastError = "fewer than 3 guides"; return false; }
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
        const auto& fu = (uvs && fx < uvs->size()) ? (*uvs)[fx] : f.uv;
        int a0 = assetOf[f.g[0]], a1 = assetOf[f.g[1]], a2 = assetOf[f.g[2]];
        if (a0 < 0 || a1 < 0 || a2 < 0) continue;
        fi.push_back(a0); fi.push_back(a1); fi.push_back(a2);
        for (int c = 0; c < 3; c++) fuv.push_back({fu[2 * c], fu[2 * c + 1]});
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
        spheres[s].m_boneSphereRadius = pk.spheres[s].r;
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
    if (sdk && instId != NvHair::INSTANCE_ID_NULL)
        sdk->setTexture(NvHair::InstanceId(instId), NvHair::TextureType::DENSITY, NvCo::Dx11Type::wrap(srv));
}

// ------------------------------------------------------------------------------------------------ descriptor
static float s2l(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

static gfsdk_float4 color(const json& c, gfsdk_float4 def) {
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
    d.m_waveScaleStrand = F("physicalMaterials.waveness.waveStrand", 1.0f);
    d.m_waveScaleClump = F("physicalMaterials.waveness.waveClump", 0.0f);
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
    d.m_useViewfrustrumCulling = true;
    d.m_useBackfaceCulling = false;
    d.m_splineMultiplier = std::max(1, L.pack->splineMultiplier);
    d.m_textureChannels[NvHair::TextureType::DENSITY] = channel(L.get("physicalMaterials.volume.densityTexChannel"));
    if (!densitySrv) d.m_usePixelDensity = false;
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

void HairRuntime::render(const XMFLOAT4X4& view, const XMFLOAT4X4& proj, float fovDeg, float w, float h, ID3D11Buffer* lightCb) {
    if (!sdk || instId == NvHair::INSTANCE_ID_NULL || !ps) return;
    NvHair::InstanceId iid = NvHair::InstanceId(instId);
    sdk->setCurrentContext(NvCo::Dx11Type::wrap(ctx));
    NvHair::Viewport vp;
    vp.init(0, 0, w, h);
    sdk->setViewProjection(vp, (const gfsdk_float4x4&)view, (const gfsdk_float4x4&)proj, NvHair::HandednessHint::RIGHT, fovDeg);
    if (!stepped) step(1.0f / 60.0f);
    sdk->preRenderInstance(iid, 1.0f);
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
