// Desktop Groom Studio - thin wrapper around the NVIDIA HairWorks runtime (loaded at run time from a DLL the user
// already has; never shipped from this repository). Our own code (MIT); the HairWorks API itself is NVIDIA's
// proprietary SDK under the GameWorks EULA, included at build time from the configurable SDK folder.
#pragma once
#include <d3d11.h>
#include <DirectXMath.h>

#include <string>
#include <vector>

#include "deskpack.h"
#include "groom.h"
#include "params.h"

namespace nvidia { namespace HairWorks { class Sdk; struct InstanceDescriptor; } }

struct HairStats {
    int hairs = 0, faces = 0;
    float avgDensity = 0, detailLod = 0, distanceLod = 0, camDist = 0;
};

class HairRuntime {
public:
    std::string dllPath, includeDir, lastError, buildString;
    std::string lastApx;      // binary .apb of the current asset (SDK serialiser)
    bool ok() const { return sdk != nullptr; }

    bool load(const std::string& dll, ID3D11Device* dev, ID3D11DeviceContext* ctx);
    bool loadPixelShader(const std::vector<char>& cso);
    void shutdown(bool releaseSdk = true);
    const void* sdkPtr() const { return sdk; }

    // (re)creates the HairWorks asset + instance from the current groom geometry
    // hideGuide / hideFace (optional, groom guide / face index -> 1): left out of this asset only (wind check elimination runs)
    bool build(const DeskPack& pk, const Groom& gr, std::vector<int>& guideOfAsset, const std::vector<std::array<float, 6>>* uvs = nullptr,
               const std::vector<char>* hideGuide = nullptr, const std::vector<char>* hideFace = nullptr);
    void setDensityTexture(ID3D11ShaderResourceView* srv);
    void setColorTexture(ID3D11ShaderResourceView* srv);      // root + tip colour texture (card diffuse)
    void setStiffnessTexture(ID3D11ShaderResourceView* srv);   // R = stiffness, G = root stiffness multipliers (null = off)
    void updateDescriptor(const Look& lk, const DirectX::XMFLOAT3& wind, bool simulate, bool showGuides,
                          bool showCollision, bool showGrowth, bool forceCloseLod, int colorize, bool stiffMap);
    void skin(const std::vector<DirectX::XMFLOAT4X4>& M, bool teleport);
    void step(float dt);
    void render(const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj, float fovDeg, float w, float h,
                ID3D11Buffer* lightCb);
    HairStats stats();
    // depth-only hair pass into the bound shadow map (SDK shadow pass: density x shadowDensityScale)
    void renderShadow(const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj, float size);
    bool visBones = false;
    // collision sphere radius overrides (Studio project "collision_r", studio_collision_band): index = pack sphere,
    // value < 0 = the pack's radius. Applied by build().
    std::vector<float> sphereR;
    void settle(int frames, float dt);
    // simulated guide CVs of the instance, copied back from the GPU (the runtime's master-strand position buffer, a
    // public shader resource of the API): asset order (guideOfAsset), xyz per CV, perGuide CVs per guide. The runtime keeps
    // the spline-refined strand: (CVs - 1) x splineMultiplier + 1 points per guide, packed guide after guide (measured
    // 2026-10-14: Ciri K 13, spline 3 -> 37, its CVs at every 3rd point). false when the buffer is missing or too small
    // (info says why). Stalls the GPU: call per step only.
    bool readGuides(std::vector<float>& xyz, int& perGuide, std::string* info = nullptr);
    int numGuides() const { return numAssetGuides; }

    // W3-only bone-distance stiffness (stiffnessBone* block of the redfur) is not in the 1.2.1 runtime. Emulation
    // (2026-10-11, calibrated on L237 in-game shots of guillaume_l124_locks_hug): for furs with stiffnessBoneEnable,
    // weights w_i = amount x stiffnessBoneCurve_i lerp the stiffness, stiffness-strength and bend-stiffness curves
    // towards 1 at the 4 curve points, and bendStiffness towards 1 by amount. Only the SDK descriptor changes; the
    // look values (and so the export) are untouched. 0 = raw 1.2.1 behaviour. Env HW_BONE_STIFF_EMU overrides the start value.
    float boneStiffEmu = 1.0f;
    bool boneStiffInFur = false;     // set by updateDescriptor: the fur has stiffnessBoneEnable
    // physical / LOD values of the fur next to what the SDK instance descriptor received (calibration dump)
    std::string paramDump(const Look& lk) const;

private:
    nvidia::HairWorks::Sdk* sdk = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* samp = nullptr;
    ID3D11ShaderResourceView* densitySrv = nullptr, * stiffSrv = nullptr, * colorSrv = nullptr;
    int assetId = 0xFFFF, instId = 0xFFFF;
    int numBones = 0, numAssetGuides = 0, assetCvs = 0, splineMul = 1;   // CVs per guide given to the asset, spline multiplier sent
    bool stepped = false;
    ID3D11Buffer* readStage = nullptr;   // staging copy of the guide position buffer (readGuides)
    UINT readStageBytes = 0;
    void freeInstance();
};
