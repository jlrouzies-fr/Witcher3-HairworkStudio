// Desktop Groom Studio - application state shared by the UI, the editor and the headless agent runner.
// Our own code (MIT).
#pragma once
// the one community pack repo the Studio reads from and publishes to (not user-configurable)
inline constexpr const char* kCommunityRepo = "jlrouzies-fr/Witcher3-HairworkCommunity";
#include <windows.h>

#define GSD_VERSION "0.2.1"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

#include "curls.h"
#include "deskpack.h"
#include "gfx.h"
#include "groom.h"
#include "hairsdk.h"
#include "materials.h"
#include "motion.h"
#include "params.h"
#include "pt_estimate.h"

struct Settings {
    std::string repo, sdkDir, dll, python = "py", gameDir;   // gameDir: The Witcher 3 folder (content\content0 + mods), exported as HWFA_GAME to the Python tools
    bool bundled = false;   // tools bundled next to the exe (release folder): that folder is the repo
    std::string commRepo = kCommunityRepo, commChannel = "main", commLocal;   // Community window: GitHub owner/name + channel, or a local checkout
    int commSource = 1;   // 0 local folder, 1 GitHub (default)
    std::string commAuthor;            // GitHub login used as pack author (filled from gh api user)
    std::set<std::string> commPinned;  // installed packs (author__name) pinned to their version: no update prompts
    std::string file;   // %LOCALAPPDATA%\HairworkForAll\desktop_studio.json
    std::vector<std::string> recent;   // recently opened builds ("cfg") and projects (".groomproj.json" paths)
    std::map<std::string, std::array<float, 3>> guideCol;   // View > Guide colour remembered per character
    void addRecent(const std::string& r);
    void load();
    void save() const;
    std::string dllPath() const;
};

struct Args {
    std::string cfg, screenshot, view = "back", motion, sdk, dll, agent, project, uiTest, out;
    bool serve = false;
    std::string ptEstimate, ptOpts;   // --pt-estimate <hwpack.json> [--pt-opts k=v,..]: print the path tracing cost metric and exit
    int frames = 240;
    bool exitAfter = false, guides = false, closeLod = false, noSplash = false, guidesFront = false, guidesBody = false;
};

struct Camera {
    DirectX::XMFLOAT3 target{0, 0, 1.75f};
    float yaw = 3.14159f, pitch = 0.12f, dist = 0.95f, fov = 35.0f;
    DirectX::XMMATRIX view() const;
    DirectX::XMFLOAT3 eye() const;
};

// Physically based sun + sky rig (lighting.cpp; preset 0 = the HairLab capture rig fitted to the in-game frames, README
// "Rendering"). Light values are irradiance / pi; the sky / ground are a procedural environment (SH9 + sky occlusion).
struct Rig {
    float sunAz = -2.8f, sunEl = 0.8f, sun = 2.2f, sky = 1.9f, horizon = 1.1f, ground = 0.7f, exposure = 1.45f;
    float sunCol[3] = {1.0f, 0.96f, 0.90f}, skyCol[3] = {0.60f, 0.72f, 0.90f}, horizonCol[3] = {0.28f, 0.46f, 0.16f},
          groundCol[3] = {0.56f, 0.47f, 0.34f};
    // hair calibration (fitted 2026-10-11 on L244 Guillaume + L232 Vesemir): colour gain on the redfur colours, spec
    // gain / power scale, texture gain (card colour textures), per-strand variation
    float colourGain = 0.95f, specGain = 3.0f, selfShadow = 0.6f, floor = 0.0f, specPower = 0.06f, texGain = 1.1f, strandVar = 0.35f;
    // deep shadow (shadowSigma x sigmaScale), multiple-scattering spread (light hair stays lit inside) and saturation
    // (sun light / sky light), back light (TT), hair sky sheen, mesh sky reflections
    float sigmaScale = 0.4f, msSpread = 0.0f, msSat = 1.0f, msSatAmb = 0.4f, ttGain = 0.3f, envSpec = 0.0f, meshEnvSpec = 0.3f;
    // skin: subsurface wrap, specular, albedo gain; eyes / mouth cavity occlusion; sky occlusion strength
    float skinSss = 0.35f, skinSpec = 0.35f, skinGain = 0.78f, cavity = 0.55f, aoStrength = 1.0f;
    float bg[3] = {0.071f, 0.078f, 0.086f};
    // time of day (lighting.cpp presets; 0 = the calibrated HairLab rig), shadows, backdrop
    int tod = 0;
    bool shadows = true, backdrop = true, moon = false;
    float skyTop[3] = {0.11f, 0.12f, 0.13f}, skyBottom[3] = {0.06f, 0.065f, 0.07f};
    float shadowSoft = 1.0f, shadowStrength = 1.0f, sunSize = 0.6f;
    // lighting quality: 0 fast (sun shadow only, no sky occlusion), 1 balanced (8 sky-occlusion maps), 2 high (16)
    int quality = 2;
    // shaded-hair fibre detail (2026-10-14, Zoltan moustache read as flat felt in shade): sky light occluded towards the
    // root of each strand (neighbour fibres) + per-strand sky variation; 0 / 0 = the old smooth sky term
    float ambRoot = 0.2f, ambVar = 0.4f, ambTv = 0.6f;     // 0.2 / 0.4: Vesemir r31a stays at its calibrated look (shot 3)
    float hairEncoding = 0.7f;   // colour decode: 0 = sRGB-decoded, 1 = stored values; 0.7 fits L244 / L232 / L246beard
    int toneMode = 0;   // 0 ACES fitted (RRT + ODT), 1 the same curve on luminance (hue preserving)
};

enum class Tool { Select, Move, Comb, Curl, Paint, Draw, Count };

// one pipeline job at a time (desk_prep / hairpipe build / catalog), run on a thread so the UI stays responsive
struct Job {
    std::thread th;
    std::atomic<bool> running{false}, done{false};
    std::mutex m;
    std::string title, out, lastLine;
    int rc = 0;
    std::function<void(int)> onDone;
};

struct App {
    Settings set;
    Args args;
    Gfx gfx;
    HairRuntime hw;
    // second fur of a beard config (desk.json "beard_fur"): a second HairWorks instance. When it is present the
    // vanilla beard cards are hidden, as in game. Beard mode (fur = 1, inspector "Editing  Hair | Beard"): swapFur()
    // exchanges the two furs, so the beard is the live, editable groom (pack / groom / look / hw / smap ... below:
    // every tool, check, budget and MCP command acts on it) and these "beard*" members hold the PARKED fur, shown as
    // a preview: the beard in Hair mode, the hair in Beard mode. Both edit sets go into the project and the export.
    HairRuntime hwBeard;
    DeskPack beardPk;
    Groom beardGroom;
    Look beardLook;
    std::vector<int> beardGuideOfAsset;
    bool beardSdkOk = false, beardOn = false, showBeard = true;
    void buildBeard();
    // the rest of the parked fur's own state (swapFur exchanges it with the live members of the same name)
    struct FurSlot {
        StiffnessMap smap;
        UVAtlas atlas;
        bool atlasOn = false, keepColours = true;
        ID3D11Texture2D* stiffTex = nullptr;
        ID3D11ShaderResourceView* stiffSrv = nullptr, * densitySrv = nullptr, * colorSrv = nullptr, * bakedDensitySrv = nullptr, * bakedColorSrv = nullptr;
        std::vector<unsigned char> srcDensity, srcColor, bakedDensity, bakedColor;
        int srcDW = 0, srcDH = 0, srcCW = 0, srcCH = 0;
        std::map<int, float> colR;
        std::vector<MaterialPreset> presets;
        int presetSel = 0;
    } beardSlot;
    DeskPack skel;          // the hair pack's bones: motion pivots (neck, torso3) whichever fur is live
    json packMaterials;     // desk.json "materials" (material presets of either fur)
    int fur = 0;            // 0 = Hair mode, 1 = Beard mode (the beard fur is the live groom)
    void swapFur();         // exchange live and parked fur (no rebuild: each runtime keeps its own asset)
    bool setFur(int f);     // the mode switch: swapFur + clears selection / check results; false without a beard fur
    int editCount() const;  // ops + look changes of both furs ("N edits unsaved")
    json furState() const;              // the live fur's edits as project keys (ops, look, material, stiffness, ...)
    void applyFurState(const json& j);  // ... and back (project load)
    DeskPack pack;
    Groom groom;
    Look look;
    Motion motion;
    Camera cam;
    Rig rig;
    bool loaded = false, sdkOk = false, headless = false;
    std::string status, configName;
    std::vector<std::string> configs;
    int cfgSel = -1;
    ID3D11ShaderResourceView* headTexSrv = nullptr;   // unused (kept for layout); mesh textures are in meshTex
    std::map<std::string, ID3D11ShaderResourceView*> meshTex;   // pack PNG -> SRV (diffuse sRGB, normal linear)
    ID3D11ShaderResourceView* densitySrv = nullptr, * colorSrv = nullptr, * logoSrv = nullptr, * gwLogoSrv = nullptr;
    ID3D11Texture2D* stiffTex = nullptr;
    ID3D11ShaderResourceView* stiffSrv = nullptr;
    int logoW = 0, logoH = 0, gwW = 0, gwH = 0;
    std::vector<int> guideOfAsset;
    bool assetDirty = false, stiffDirty = true, teleport = true;
    double lastBuild = 0, simAcc = 0;
    // view toggles
    bool simulate = true, paused = false, showHair = true, showGuideOverlay = false, showMeshes = true;
    bool guideCustom = false;                      // View > Guide colour: own colour for the overlay (white hair hides the default grey)
    float guideRgb[3] = {0.6f, 0.65f, 0.7f};
    bool showCollision = false, showGrowth = false, sdkGuides = false, forceCloseLod = false;
    int colorize = 0;
    // guide visibility views (guidevis.cpp, View menu / toolbar): "Front layer only" (Shift+G) keeps the guides that are
    // the outermost hair seen from the camera, "Hide behind the body" (Ctrl+G) clips guide parts hidden by the meshes.
    // Live with the camera / pose (CPU depth buffers on a background worker in the viewport, inline for off-screen
    // renders); "Select visible only" makes click / box / brushes ignore the guides the view hides.
    bool guideFront = false, guideBody = false, selectVisibleOnly = true;
    float guideFrontPct = 50, guideTolMm = 12;
    struct GuideVis {
        bool valid = false, cached = false;   // cached: inputs unchanged, the last result kept
        uint64_t key = 0;                     // hash of the inputs of the result
        int N = 0, K = 0, nShown = 0, nTri = 0, res = 0, ptsK = 0;
        float ms = 0, msBody = 0, msHair = 0, msMain = 0;   // pass / body raster / hair splats (per thread) / main-thread share
        std::vector<uint8_t> pt, shown;   // per point (drawn after the body clip) / per guide
        std::vector<V3> pts;              // posed points of the current frame (the overlay draws them)
    } gvis;
    bool guideVisOn() const { return guideFront || guideBody; }
    bool visFilter() const { return guideVisOn() && selectVisibleOnly && gvis.valid; }
    // bodyA / bodyB: the frame's posed mesh triangles (async: moved into the job), both null = posedMeshTris
    void computeGuideVis(const DirectX::XMFLOAT4X4& V, const DirectX::XMFLOAT4X4& P, int w, int h,
                         std::vector<GVertex>* bodyA, std::vector<GVertex>* bodyB, bool async);
    bool guideShown(int i) const;               // drawn by the current guide view
    bool guidePointShown(int i, int k) const;
    bool pickVisible(int i) const;              // ... and pickable ("Select visible only")
    bool pickPointVisible(int i, int k) const;
    // editing
    Tool tool = Tool::Select;
    std::set<int> sel;
    std::string falloff = "carry";
    int fromK = 0;
    float keepPct = 80, dupStepMm = 6, brushPx = 40, combStrength = 1.0f;
    bool dragging = false, boxSel = false;
    float mx0 = 0, my0 = 0, mxl = 0, myl = 0;
    size_t strokeOps0 = 0;
    int dragK = -1;   // Move tool: CV dragged alone (-1 = whole guide)
    std::map<int, V3> combOff;
    std::set<int> strokeSet;
    // Draw tool: the stroke (screen px), its root on the head and the live 3D preview
    std::vector<std::pair<float, float>> drawPx;
    V3 drawRoot{};
    bool drawing = false;
    std::vector<V3> drawWorld;
    int grpId = 0;
    CurlParams curl;
    // symmetry
    bool symmetry = false;
    int mirrorSource = 0;          // 0 = +x (character's left), 1 = -x
    float mirrorBandMm = 15, mirrorYmin = -1.0f;
    std::vector<int> mirrorOf(const std::vector<int>& ix) const;   // partner guides across the mid-plane
    void opMirror();
    // materials / stiffness
    std::vector<MaterialPreset> presets;
    int presetSel = 0;
    bool keepColours = true;
    char presetName[64] = "";
    StiffnessMap smap;
    UVAtlas atlas;
    bool atlasOn = false;                       // unique UVs in use (a stiffness map is painted)
    std::vector<unsigned char> srcDensity, srcColor, bakedDensity, bakedColor;
    int srcDW = 0, srcDH = 0, srcCW = 0, srcCH = 0;
    ID3D11ShaderResourceView* bakedDensitySrv = nullptr, * bakedColorSrv = nullptr;
    void bakeAtlas();
    float paintS = 0.6f, paintR = 1.0f;
    int paintTarget = 0;   // 0 both, 1 stiffness, 2 root stiffness
    // export / project
    char modName[128] = "", tag[64] = "desk1";
    std::string lastExport, projectPath;
    // stats
    HairStats st;
    Budget bud;
    PtWorker ptWorker;
    PtCalib ptCalib;
    bool ptCalibLoaded = false;
    bool ptLoadSim = false;   // artificial GPU load proportional to the estimate (off by default)
    double ptMs = 0;          // current estimate (ms/frame), 0 = none yet
    double sumFaceDensity = 0;
    float fps = 0;
    bool aboutOpen = false, helpOpen = false;
    std::string pendingShot;   // saved after the UI is drawn (F12)
    std::string pendingPreview;   // community export: viewport (no UI) saved as <=300KB jpg right after drawScene; previewOk 1/0 when done
    int previewOk = -1;
    // splash (startup): the rebuilt logo, ~1.75 s with a fade; click / key skips; off in agent / ui-test / --no-splash
    bool splashOn = false;
    double splashT0 = -1;
    ID3D11ShaderResourceView* splashSrv = nullptr;
    int splashW = 0, splashH = 0;
    void drawSplash();
    DirectX::XMFLOAT4X4 viewM, projM;
    bool camOverride = false, maskPass = false;
    // View > Vanilla reference: the in-game vanilla stills of the pack's HairLab shots (pack.shots[k]["ref"])
    bool vanillaOpen = false;
    bool communityOpen = false;   // View > Community
    bool vanillaHair = false;   // View > Vanilla hair (cards): the game's hair cards instead of the HairWorks groom
    // 2026-10-12 opt-in (serve "keep_cards"): hair-card mesh index -> per-triangle 1 = this vanilla card stays drawn WITH
    // the groom (preview of the pipeline's split_cards: cards for one element, strands for the rest). Empty = off.
    std::map<int, std::vector<uint8_t>> keepCards;
    std::string vanillaShot;
    struct RefTex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };
    std::map<std::string, RefTex> vanillaTex;   // shot id -> still, loaded on first view, freed when another build opens
    void drawVanillaRef();
    bool scalpOverlay = false;   // View > Scalp highlight / agent "overlay":"scalp": head scalp green, other skin grey
    // scalp area of the head mesh from its painted diffuse (per vertex 0/1): the scalp overlay paints only it green,
    // other skin grey (agent.cpp computeHeadScalp)
    std::map<int, std::vector<float>> headScalp;
    void computeHeadScalp();
    bool scalpMesh(std::vector<V3>& pos, std::vector<float>& flag, std::vector<std::array<int, 3>>& tris) const;   // posed head mesh + scalp flags (hairline tools)
    bool skinRay(const DirectX::XMFLOAT3& o, const DirectX::XMFLOAT3& d, DirectX::XMFLOAT3& hit, int& mesh) const;   // nearest posed skin triangle hit
    // viewport toolbar (toolbar.cpp)
    // Check > Unnatural endings: faces whose guides fan apart (hair between them ends in a hard line), shown orange
    bool endingsOpen = false;
    float endDiv = 50, endSpread = 4;
    std::vector<int> endFaces;
    std::set<int> endGuides;
    void checkEndings();
    void removeFanned();
    // Check > Unwanted loops: guides that come back to themselves (not curls), shown purple
    bool loopsOpen = false;
    float loopArc = 60, loopClose = 15, loopCurlR = 45, loopTurn = 270;   // mm, mm, mm, deg
    std::vector<int> loopList;
    std::vector<std::string> loopWhy;
    std::set<int> loopGuides;
    void checkLoops();
    void removeLoops();
    // Check > Max-wind strays (windcheck.cpp): measured run of the SDK simulation at maximum wind. Rest hair mask vs
    // wind frames per camera (distance from the rest silhouette), simulated guide CVs read back from the GPU, culprits
    // confirmed by hiding candidates and re-running the same wind. Culprit guides are red in the viewport.
    bool windOpen = false, windBusy = false, windInside = false, windCancel = false;
    float windThr = 12;                 // px at 600 px
    float windProgress = 0;
    std::string windPhase;
    struct WindRow { std::string kind; int id = -1; std::vector<int> guides; std::string text; bool confirmed = false; };
    std::vector<WindRow> windRows;
    std::set<int> windGuides;           // culprit guides (overlay red, Select / Delete)
    std::set<int> windFaces;            // culprit faces
    json windResult, windParams;
    std::string windOut;
    void* windFiber = nullptr, * mainFiber = nullptr;
    std::vector<char> hideGuide, hideFace;   // left out of the HairWorks asset (elimination runs), empty = none
    // collision sphere radius overrides (sphere index -> radius, m), saved in the project ("collision_r"), exported in
    // the geo json (desk_export.set_geometry writes them into the fur): studio_collision_band
    std::map<int, float> colR;
    json windCheck(const json& params, const std::string& out);   // the whole check (serve: blocking; UI: inside windFiber)
    void windStart(const json& params);  // UI: start the check on a fiber, resumed a slice per frame from step()
    void windResume();
    void windYield(float progress, const char* phase);
    void windDeleteCulprits();           // one undo group: delete_face ops for face culprits + delete of the guide culprits
    void posedMeshTris(std::vector<GVertex>& out) const;   // posed non-alpha meshes as drawn (occlusion for the wind check)
    std::string charKey() const;          // character the open build belongs to (guide colour memory)
    void rememberGuideColour();           // store the current guide colour for that character
    void applyGuideColour();              // restore the remembered one (or the default)
    bool lightOpen = false;   // Light section open: the viewport shows the light-direction gizmo
    // sky-occlusion maps: refreshed round-robin (kAoPerFrame a frame) in the viewport, all at once for shots / changes
    int aoCount = 0, aoNext = 0;
    bool aoRefreshAll = true;
    int hairDebug = 0;   // 0 lit, 1 N.L, 2 specular, 3 shadow term, 4 albedo, 5 tangent (F6 cycles)
    bool tbCollapsed = false, turntable = false, wireframe = false, showBones = false, showSymPlane = false, windOn = false;
    bool stepOnce = false;
    float tbDx = 0, tbDy = 0;   // drag offset of the toolbar from its home (top centre of the viewport)
    int camShot = -1;           // last HairLab camera preset (4 front, 3 3/4, 2 side, 0 back); -1 after an orbit
    int savedEdits = 0;         // ops + look overrides when the project was last saved / opened ("N edits unsaved")
    bool exportReq = false;     // title-bar "Export mod": open the Export section
    void drawToolbar();
    void cameraShot(int shot);   // HairLab camera presets (0 back, 2 side, 3 front34, 4 front)
    void frameHead();
    void stopReset();
    void drawSunGizmo(float size);   // maskPass: meshes black (they still hide hair behind them)
    DirectX::XMFLOAT4X4 ovView, ovProj;
    float ovFovDeg = 30;

    // start screen / character picker (picker.cpp)
    json catalog;
    bool pickerOpen = true;
    int pickSel = -1, pickStatus = 0, pickGender = 0;
    int pickPortrait = 0;    // card portraits: 0 painted (portraits_ai; per card: ours when no painting yet), 1 ours, 2 vanilla
    ID3D11ShaderResourceView* portrait(const json& c, int mode = -1);   // cached; falls back painted -> ours -> vanilla
    const json* catalogEntry(const std::string& cfg) const;             // the catalog character owning a build
    // hairstyles (Character -> Hairstyle -> iterations; catalog "hairstyles", default first). The card shows the default
    // style; the drawer follows the style picked per character. styleView = the character record with the selected
    // style's default / approved / pick / best / status / variants / portraits (+ style_id, style_name, portrait_key).
    std::map<std::string, std::string> pickStyle;                       // character -> selected style id
    int pickStyleIdx(const json& c) const;
    json styleView(const json& c) const;
    json styleViewAt(const json& c, int i) const;                       // ... for hairstyle index i
    // editor hairstyle switcher (stylesw.cpp): title-bar crumb "##crumb_style" + inspector card "##card_style" open one
    // popup (rows "<name>##edstyle"), Ctrl+Shift+H too. A style with builds opens its best / approved build (Save /
    // Discard / Cancel first when there are unsaved edits); a style with none shows an inline panel in the inspector
    // card ("Start plain conversion" when supported, else "Not started yet") and the loaded build stays as it is.
    std::string edStyleNoBuild, edStyleNoBuildCfg;   // style with no build picked in the editor, and the cfg loaded then
    std::string edStyleId() const;                   // the style the editor switcher shows as selected
    bool styleMenuReq = false, styleMenuOpen = false;
    float styleMenuX = 0, styleMenuY = 0, styleMenuW = 0, crumbStyleX = 0, crumbStyleY = 0;
    std::string switchCfg;                           // build waiting for the unsaved-edits answer
    bool switchBuild = false, switchConfirmReq = false;
    bool dirtyEdits() const;
    void requestOpen(const std::string& cfg, bool build);   // openAsync, asking Save / Discard / Cancel first when dirty
    void pickEditorStyle(const std::string& id);
    void drawStyleMenu();                            // the popup + the unsaved-edits modal (called from ui())
    std::string pickStyleId() const;                                    // style of the selected card (UI tests)
    std::string styleId = "default";                                    // "style" key of the loaded config
    std::string styleName(const std::string& cfg) const;                // catalog name of the loaded config's style
    char pickFilter[64] = "";
    std::map<std::string, ID3D11ShaderResourceView*> thumbs;
    std::vector<std::string> projectsFound, exportsFound;
    Job job;
    void loadCatalog();
    void refreshCatalog();
    void scanProjects();
    void drawPicker();
    bool startJob(const std::string& title, const std::string& cmd, std::function<void(int)> onDone);
    void pollJob();
    void openAsync(const std::string& cfg, bool build);
    bool packUpToDate(const std::string& cfg) const;

    bool init(HWND hwnd);
    void listConfigs();
    bool openConfig(const std::string& cfg, bool rebuildPrep);
    void rebuildHair();
    void updateStiffnessTexture();
    void step(double dt);
    void drawScene();
    void computeBudget();
    bool exportAll(std::string& outDir, bool release = false);   // release: requires the Hairwork for All Core (core/README.md)
    // Hairwork for All Core (dependency mod of release exports): installed in <game>\mods? version vs the repo's CORE_VERSION
    struct CoreInfo {
        bool installed = false;
        std::string version, required, modDir;   // version "" = installed without a readable core_version.json
        double checkedAt = -1e9;
        bool ok() const;                         // installed and version >= required
        std::string summary() const;             // "Core v1.0.0" / "Core missing" / "Core v0.9.0 < 1.0.0"
    } core;
    void checkCore(bool force = false);          // re-reads at most every 5 s
    static constexpr const char* CORE_NEXUS = "https://www.nexusmods.com/witcher3/mods/13996";
    void setView(const std::string& v);
    bool saveProject(const std::string& path);
    bool loadProject(const std::string& path);
    void applyMaterial(int idx);
    void holdCurlsPhysics();
    // posed editing: the overlay / picking / brushes use the CURRENT skinned pose; edits are stored in rest space
    std::vector<DirectX::XMFLOAT4X4> boneM;     // current bone matrices (rest -> posed), updated every step
    V3 posedPoint(int i, int k) const;
    V3 restDelta(int i, V3 posedDelta) const;    // a posed-space offset expressed in the guide's rest space
    bool editPose = false, editWhileMoving = false;
    MotionPreset savedPreset = MotionPreset::Idle;
    bool savedSimulate = true;
    void enterEditPose();
    void resumeMotion();
    // editing helpers
    DirectX::XMFLOAT2 project(V3 p) const;
    int pickGuide(float mx, float my) const;
    V3 screenDeltaToWorld(float dx, float dy, V3 at) const;
    std::vector<int> selList() const;
    std::vector<int> guidesUnderBrush(float mx, float my, float r) const;
    void opDelete();
    void opDuplicate();
    bool headRay(float mx, float my, V3& hit) const;   // screen pixel -> first hit on the head mesh
    void drawUpdate(float mx, float my);               // Draw tool: add a stroke point + refresh the preview
    void opDrawGuide();                                // Draw tool: add the stroke as a new guide (duplicate + reshape ops)
    void opShorten();
    void opCurl(const std::vector<int>& ix);
    void paintAt(float mx, float my);
    void editsChanged();
    std::vector<int> selectGuides(const json& sel) const;
    // UI / input (ui.cpp)
    void handleInput();
    void ui();
    // headless (agent.cpp)
    int runAgent(const std::string& scriptPath);
    json agentStep(const json& stp, const std::string& out, int& rc);
    int finishAgent(json& report, const std::string& out, int rc);
    int runServe(const std::string& out);   // serve.cpp: live headless session, JSON lines on stdin / stdout (MCP bridge)
    json serveCommand(const json& c, const std::string& out, bool& quit);
    bool renderShot(const json& shot, const std::string& outPng, int size);
};

void drawCommunity(App& a, bool& open);   // community.cpp
void logf(const char* fmt, ...);
const std::vector<std::string>& logLines();
int runCapture(const std::string& cmd, const std::string& cwd, std::string& out);
std::string exeDir();
extern const char* CREDIT;
