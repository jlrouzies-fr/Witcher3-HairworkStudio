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
                ID3D11Buffer* lightCb, bool depthOnly = false);   // depthOnly: no pixel shader (game-accurate depth prepass)
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
    // Game-accurate preview (default on, 2026-10-15). W3 5.0 draws HairWorks path-traced (PTHairQualityMode 3): its strand
    // generation (analysis/hairworks_re/pt_hair_visibility.txt) makes a fixed RTNHairsPerPatch (32) hairs per growth face,
    // never reads the `density` scalar nor any detail / distance LOD, keeps a hair by the density texture, and applies root
    // transparency (PTHairRootAlphaMode / FalloffOverride). The 1.2.1 raster runtime instead scales the count by density and,
    // because its LOD distance is fov-normalised (a 1.3 m HairLab shot at fovy 19.7 reads ~0.35 m), switches to the
    // close-up detail LOD (detailLODDensity, detailLODWidth) in every head view. With gameAccurate the descriptor gets:
    // density = hairsPerFace / 64, detail + distance LOD off, width x ptWidthScale, rootAlphaFalloff from the game's
    // settings. Preview only: look values and exports are untouched. Env HW_GAME_ACCURATE=0 starts with it off.
    bool gameAccurate = true;           // 2026-10-17 (user): "studio lighting should be game lighting 'path tracing' by default": ON, with the path-traced look + game light (FINDINGS 13.6); was OFF since 2026-10-16 evening. Env HW_GAME_ACCURATE=0 still turns it off.
    int ptHairsPerFace = 32;            // [Rendering/HairWorks] RTNHairsPerPatch
    int ptHairsPerFaceGame = 32;        // the settings value (Thin-hair model off restores it)
    float ptTipScale = -1.0f;           // >= 0: game-accurate widthTipScale override (calibration)
    // L480 refit (2026-10-16, with the detail-LOD emulation below): Studio width = 0.24 x w_eff^0.55, root / tip scales ^0.55, 32 hairs
    // per face: mean |crown skin Studio - game| 0.069 over the 32 L480 rows (base -.003, width / root / tip / detailLODWidth within
    // .01-.10, detailLODDensity .05, stiffness .03 / .12). The Studio's raster coverage responds to width about 1.8x more steeply than
    // the game's denoised path tracing (crown skin ~ width^-0.82 vs ^-0.46), hence the exponent.
    float ptWidthExp = 0.55f;           // with ptWidthConst > 0: width = ptWidthConst x width^ptWidthExp (L480 fit; was the L391 0.1)
    float ptScaleExp = 0.55f;          // with ptWidthConst > 0: widthRootScale / widthTipScale -> scale^ptScaleExp (L480 fit)
    float ptVeilWref = 0.0f;           // > 0: veil x clamp(ptVeilWref / effective mid width, .25, 1) (wide strands smear less)
    float ptEffWidth = 0.0f;           // last effective mid strand width sent (reported)
    int ptDensityMip = -1;              // >= 0: game-accurate density map sampled from this mip up (game [GC_PTHair] GCP_PTHairAlphaTestMipLevel, L394/L395)
    int ptDensityMipGame = 4;           // the value read from the game settings (reported only)
    float ptWidthConst = 0.24f;         // > 0: width = ptWidthConst x w_eff^ptWidthExp (L480 fit, w_eff from the detail-LOD emulation); 0 = the fur width as is
    bool ptWidthFloor = true;           // game-accurate width >= 0.221 x width^0.1 (L391 low-width fit; only widths < ~0.187 change); only when ptLodEmu is off
    // Detail-LOD emulation (2026-10-16, L480 parameter report): in game the close-up detail LOD is NOT ignored by the path
    // tracer (contrary to the RE note): detailLODWidth (.1 / .5 / 2 -> crown skin .56 / .36 / .23) and detailLODDensity (.2 / .8 / 3
    // -> .53 / .36 / .25) dominate close-range coverage, enableLOD off makes the hair vanish (the stored density 0 is used), and
    // the fur width itself is weak (.0592 / .592 / 2.96 -> .43 / .36 / .28). One model fits every width / root / tip / LOD row
    // of L480 on one monotone curve (rms .047 in log, 0 order violations): w_eff = lerp(width, detailLODWidth, t) with t = 0.8
    // at the HairLab head shots, hairs kept = lerp(1, detailLODDensity, t) (enableLOD off: the fur's density scalar), and the
    // crown skin share ~ (w_eff x mean(root, tip) scale)^-0.46. t = saturate((detailLODStart - d) / (detailLODStart - detailLODEnd))
    // with d = the SDK's own fov-normalised camera distance (stats, 0.342 m at the shots) x ptLodDistScale (1.64: t = 0.8 on Anna's
    // fur, start 1.2 / end 0.4). The SDK's detail LOD stays off; the blend is applied to the descriptor here.
    bool ptLodEmu = true;               // game-accurate: emulate the game's detail LOD on width / hair count (false = the old model)
    bool densityGameOnly = false;       // the density texture is bound only while gameAccurate is on (the beard runtime: the default preview never had one)
    float ptLodDistScale = 1.64f;       // LOD distance = SDK camera distance x this (fitted on L480)
    float ptLodT = -1.0f;               // >= 0: fixed LOD factor t (calibration); < 0: from the camera distance
    float ptCamDistSdk = 0.342f;        // the SDK's camera distance of the last frame (stats), feeds the LOD factor
    float ptLodTUsed = 0.0f, ptLodWidthEff = 0.0f, ptLodKeep = 1.0f;   // reported: t, lerp(width, dlw, t), hair keep factor
    // textureBrightness (ignored by the Studio shader without a card texture; in game it scales the hair colour: .3 / 1 / 2 ->
    // crown luminance x .53 / 1 / 1.47, the same response as the root / tip colour) and the engine's PTHairRoughness (.15 / auto / .8
    // -> x 1.05 / 1 / .60): both become a colour gain in game-accurate mode (App fills g_pt.w)
    float ptRoughness = -1.0f;          // [Rendering/RT/PathTracer] PTHairRoughness (-1 = auto); a brightness gain here (3-point fit)
    // controls the game ignores (L480: specular primary / secondary, diffuseHairNormalWeight, ambientEnvScale, shadowSigma,
    // shadowDensityScale change nothing in game): in game-accurate mode the descriptor takes the L480 calibration constants
    // instead of the fur's values, so a user cannot tune the preview with a knob that does nothing in game
    bool ptNeutralise = true;
    // W3 bone stiffness: in game stiffness 0 / 0 lets Anna's hair fall (crown skin .50) and 1 / 1 holds it (.22); the emulation
    // below used to pull bendStiffness to 1 for every stiffnessBoneEnable fur, which froze the shape whatever the stiffness. In
    // game-accurate mode the pull is scaled by the fur's stiffness (ptStiffByFur)
    bool ptStiffByFur = true;
    // fewer hairs look thicker in the game's denoised path tracing (L480: RTNHairsPerPatch 8 / 16 / 32 -> crown skin .53 / .46 / .36,
    // detailLODDensity .2 -> .53, while the Studio's raster at the same counts bares far more): width x (hairs / 32)^-ptCountExp
    float ptCountExp = 0.3f;
    // the Studio's tone response to a colour gain is lum ~ gain^0.43 (measured on the tbright rows) while the game's is ~ gain^0.55
    // (colour and textureBrightness rows): the gain is raised to ptTbExp before use
    float ptTbExp = 1.28f;
    static float roughnessGain(float r) {   // PTHairRoughness -> hair brightness gain (L480: .15 x1.05, auto x1, .8 x.60; auto taken as .3)
        if (r < 0.0f) return 1.0f;
        const float x[3] = {0.15f, 0.30f, 0.80f}, y[3] = {1.06f, 1.00f, 0.30f};   // Studio gains giving the game's lum ratios 1.05 / 1 / .60
        if (r <= x[0]) return y[0];
        if (r >= x[2]) return y[2];
        int i = r < x[1] ? 0 : 1;
        return y[i] + (y[i + 1] - y[i]) * (r - x[i]) / (x[i + 1] - x[i]);
    }
    int ptRootAlphaMode = 1;            // [GC_Store] / [Rendering/RT/PathTracer] PTHairRootAlphaMode (0 off, 1 stochastic, 2 alpha test)
    float ptRootAlphaFalloff = 0.06f;   // PTHairRootAlphaFalloffOverride (< 0: the fur's rootAlphaFalloff)
    float ptWidthScale = 1.0f;          // path-traced capsule width / raster ribbon width (calibrated on in-game shots)
    int ptSpline = 0;                   // spline multiplier sent in game mode (0 = the fur's); see hairsdk.cpp
    float ptWaveScale = 1.0f;            // game wave amplitude / 1.2.1 wave amplitude at the same waveScale (calibrated in game)
    float ptHairScatter = 1.0f;          // (L387 calibration: 1.0; was 0.5) share of the light the hair blocks that still reaches the skin under it (meshes only):
                                         // the path tracer scatters light through the fibres onto the scalp, a depth map cannot
    // "Path-traced look" (default on with gameAccurate, 2026-10-16): the hair is composited like the game's denoised path
    // tracer (shaders/pt_look.hlsl): veil = blur sigma in px at 1400 px height, opacity = layers per covered pixel,
    // transmit = scene light passing through the hair, sharp = share of unblurred strands, tint = hair-colour tint of it.
    bool ptLook = true;
    // Defaults fitted on L387 (Anna, Ewald, Triss, Vesemir; behind / side / front; build/desktop_studio/ptlook/fit3.py).
    // ptMsSat / ptMsSatAmb replace the rig's multiple-scattering saturation (msSat / msSatAmb) in game-accurate mode.
    float ptVeil = 2.5f, ptOpacity = 1.0f, ptTransmit = 0.0f, ptSharp = 0.3f, ptTint = 0.0f;   // refit 2026-10-16 (L387 + L389 Voorhis)
    float ptMsSat = 0.5f, ptMsSatAmb = 1.5f;   // 2026-10-17 (FINDINGS 13.6): was 1.6 / .7; measured on Ves + Shani vs the game frames, the lit hair is LESS saturated in game, the shade more
    float ptLayers = 0.0f;             // layered-hair opacity (pt_look.hlsl g_q.z): extra strand layers close the gaps
    float ptBury = 0.0f;               // mm: hair this close to a mesh is hidden (game PT tubes half inside the scalp)
    float ptSatChroma = 1.0f;          // hue-aware saturation: 0 = hue^k, 1 = same saturation with the hue kept (shaders/hair_ps.hlsl MsSat)
    bool ptGameLight = true;            // warm noon "game light" sun / sky colours while the path-traced look is on
    float ptGate = 4.0f;                // transmission x (1 - coverage)^ptGate: only thin hair lets the skin under it through
    std::string ptSettingsSource;       // where the PT values came from (dx12user.settings path, or "defaults")
    void loadGameHairSettings();        // reads the user's dx12user.settings (Documents\The Witcher 3)
    // physical / LOD values of the fur next to what the SDK instance descriptor received (calibration dump)
    std::string paramDump(const Look& lk) const;

private:
    nvidia::HairWorks::Sdk* sdk = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* samp = nullptr;
    ID3D11ShaderResourceView* densitySrv = nullptr, * stiffSrv = nullptr, * colorSrv = nullptr;
    ID3D11ShaderResourceView* densityMipSrv = nullptr, * densityMipFor = nullptr, * densityBound = nullptr;   // ptDensityMip view cache
    int densityMipFor_ = -1;
    int assetId = 0xFFFF, instId = 0xFFFF;
    int numBones = 0, numAssetGuides = 0, assetCvs = 0, splineMul = 1;   // CVs per guide given to the asset, spline multiplier sent
    bool stepped = false;
    ID3D11Buffer* readStage = nullptr;   // staging copy of the guide position buffer (readGuides)
    UINT readStageBytes = 0;
    void freeInstance();
};
