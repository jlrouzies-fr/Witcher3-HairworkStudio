// Desktop Groom Studio - D3D11 helpers: device, MSAA scene target, meshes and lines. Our own code (MIT).
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>

#include <string>
#include <vector>

struct GVertex { DirectX::XMFLOAT3 pos, nrm; DirectX::XMFLOAT4 col; };

// scene lighting (same rig for meshes and hair): b1 in every pixel shader
struct LightCB {
    DirectX::XMFLOAT4 sunDir;      // xyz towards the sun, w = intensity
    DirectX::XMFLOAT4 sunCol;      // rgb
    DirectX::XMFLOAT4 skyCol;      // rgb, w = sky intensity
    DirectX::XMFLOAT4 groundCol;   // rgb, w = ground intensity
    DirectX::XMFLOAT4 params;      // x exposure, y colour gain, z spec gain, w self-shadow amount
    DirectX::XMFLOAT4 head;        // xyz head centre, w head radius
    DirectX::XMFLOAT4 material;    // x ambientEnvScale, y light floor, z textureBrightness, w colour texture bound
    DirectX::XMFLOAT4 extra;       // x diffuseNoiseScale (redfur), y per-strand variation
    DirectX::XMFLOAT4X4 lightVP;   // key-light shadow map view-projection
    DirectX::XMFLOAT4 shadow;      // x strength (0 = off), y depth bias (NDC), z texel size (uv), w PCF radius (texels)
    DirectX::XMFLOAT4 shadow2;     // x light depth range (m) for hair transmittance, y hair debug view, z spec power scale, w sun penumbra scale
    // ---- physically based environment (2026-10-11): sky / ground light as SH9 irradiance, sky occlusion maps, skin
    DirectX::XMFLOAT4 sh[9];       // SH9 irradiance / pi (rgb), evaluated with the surface normal
    DirectX::XMFLOAT4 skyZen;      // rgb zenith radiance (reflection lookup), w environment specular gain
    DirectX::XMFLOAT4 skyHor;      // rgb horizon radiance, w tone curve (0 ACES fitted)
    DirectX::XMFLOAT4 groundRad;   // rgb ground radiance (bounce), w saturation of the final image
    DirectX::XMFLOAT4 skin;        // x subsurface wrap, y skin specular, z skin albedo gain, w eye / mouth occlusion
    DirectX::XMFLOAT4 ao;          // x number of sky-occlusion maps (0 = off), y strength, z depth range (m), w texel (uv)
    DirectX::XMFLOAT4 hair2;       // x deep-shadow sigma scale, y multiple-scattering spread, z back-light (TT) gain, w env spec gain
    DirectX::XMFLOAT4 aoDir[16];   // xyz towards the sky direction, w weight (radiance luminance x solid angle)
    DirectX::XMFLOAT4X4 aoVP[16];  // orthographic view-projection of each sky-occlusion map
    DirectX::XMFLOAT4 hair3;       // x hair colour encoding (1 = redfur values as stored, like the game)
};
static const int kMaxAoDirs = 16;

struct FrameCB {
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4 color, camPos, pad0, pad1;
};

template <class T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Gfx {
    HWND hwnd = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11RenderTargetView* bbRtv = nullptr;
    ID3D11Texture2D* msaaTex = nullptr;   ID3D11RenderTargetView* msaaRtv = nullptr;
    ID3D11Texture2D* depthTex = nullptr;  ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Texture2D* resolveTex = nullptr;
    int w = 0, h = 0, samples = 4;
    // offscreen target (headless shots): swaps the scene targets for a square one
    bool beginOffscreen(int size, int height = 0);   // height 0 = square
    bool endOffscreen(const std::string& png);   // resolves + writes the scene (no UI) and restores the window targets
    // pipeline objects
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* psMesh = nullptr, * psLine = nullptr, * psDepthAlpha = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* frameCb = nullptr, * lightCb = nullptr;
    ID3D11RasterizerState* rsSolid = nullptr, * rsLine = nullptr;
    ID3D11DepthStencilState* dsOn = nullptr, * dsOff = nullptr;
    ID3D11BlendState* bsOpaque = nullptr, * bsAlpha = nullptr, * bsA2C = nullptr;   // A2C: alpha to coverage (cards)
    ID3D11Buffer* dynVb = nullptr; size_t dynCap = 0;
    // key-light shadow map
    int shadowSize = 2048;
    ID3D11Texture2D* shadowTex = nullptr;
    ID3D11DepthStencilView* shadowDsv = nullptr;
    ID3D11ShaderResourceView* shadowSrv = nullptr;
    ID3D11SamplerState* shadowCmp = nullptr, * shadowPoint = nullptr;
    ID3D11RasterizerState* rsShadow = nullptr, * rsWire = nullptr;
    bool wireframe = false;
    void beginShadowPass();
    // sky-occlusion maps (one orthographic depth map per sky direction, meshes + hair), Texture2DArray at t9
    int aoSize = 1024;
    ID3D11Texture2D* aoTex = nullptr;
    ID3D11DepthStencilView* aoDsv[kMaxAoDirs] = {};
    ID3D11ShaderResourceView* aoSrv = nullptr;
    void beginAoPass(int k);
    void drawTrisDepth(const std::vector<GVertex>& v);
    void drawTrisDepthAlpha(const std::vector<GVertex>& v, ID3D11ShaderResourceView* tex);   // alpha-tested cards
    void drawTrisDepthWith(const std::vector<GVertex>& v, ID3D11PixelShader* ps, ID3D11ShaderResourceView* tex);
    void bindShadow(bool on, bool ao = false);
    void drawColor(const std::vector<GVertex>& v);   // clip-space coloured triangles (backdrop gradient)

    bool init(HWND h, std::string& err);
    bool loadShaders(const std::string& dir, std::string& err);
    void resize(int w, int h);
    void beginScene(const float clear[4]);
    void setFrame(const FrameCB& f);
    void drawLines(const std::vector<GVertex>& v, bool depth);
    void drawTris(const std::vector<GVertex>& v, ID3D11ShaderResourceView* tex = nullptr, ID3D11ShaderResourceView* nrm = nullptr, bool a2c = false);
    ID3D11SamplerState* samp = nullptr;
    void resetPipeline();
    void endScene();        // resolve MSAA -> back buffer, bind back buffer for UI
    void present(bool vsync);
    bool saveScreenshot(const std::string& path);
    bool saveViewportJpg(const std::string& path, int x0, int y0, int x1, int y1, size_t maxBytes);   // back buffer region, scaled to <=1280 wide, jpeg <= maxBytes
    ID3D11ShaderResourceView* loadTexture(const std::string& path, bool srgb, int* outW = nullptr, int* outH = nullptr);
    void shutdown();

private:
    struct Saved { ID3D11Texture2D* m, * d, * r; ID3D11RenderTargetView* mr; ID3D11DepthStencilView* ds; int w, h; } saved{};
    bool offscreen = false;
    void createSceneTargets();
    void createTargets();
    void releaseTargets();
    void upload(const std::vector<GVertex>& v);
};

std::vector<char> readFile(const std::string& path);
