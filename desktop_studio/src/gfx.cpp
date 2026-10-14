// Desktop Groom Studio - D3D11 helpers. Our own code (MIT).
#include "gfx.h"

#include <algorithm>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

using namespace DirectX;

bool writePng(const std::string& path, int w, int h, const unsigned char* rgba) {
    return stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) != 0;
}

unsigned char* loadImage(const std::string& path, int* w, int* h) { int n; return stbi_load(path.c_str(), w, h, &n, 4); }
void freeImage(unsigned char* p) { stbi_image_free(p); }

std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool Gfx::init(HWND hw, std::string& err) {
    hwnd = hw;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &fl, 1, D3D11_SDK_VERSION, &dev, &got, &ctx))) {
        err = "D3D11CreateDevice failed (a DirectX 11 GPU is required)";
        return false;
    }
    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* ad = nullptr;
    IDXGIFactory2* fac = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    dxgiDev->GetAdapter(&ad);
    ad->GetParent(__uuidof(IDXGIFactory2), (void**)&fac);
    RECT rc;
    GetClientRect(hwnd, &rc);
    w = std::max<int>(1, rc.right - rc.left);
    h = std::max<int>(1, rc.bottom - rc.top);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = fac->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &swap);
    fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    safeRelease(fac);
    safeRelease(ad);
    safeRelease(dxgiDev);
    if (FAILED(hr)) { err = "CreateSwapChainForHwnd failed"; return false; }
    UINT q = 0;
    for (int s : {8, 4, 2}) {
        if (SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, s, &q)) && q > 0) { samples = s; break; }
    }
    createTargets();
    // states
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    dev->CreateRasterizerState(&rd, &rsSolid);
    rd.AntialiasedLineEnable = TRUE;
    rd.MultisampleEnable = FALSE;
    dev->CreateRasterizerState(&rd, &rsLine);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dev->CreateDepthStencilState(&dd, &dsOn);
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&dd, &dsOff);
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &bsOpaque);
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    dev->CreateBlendState(&bd, &bsAlpha);
    D3D11_BLEND_DESC ac = {};
    ac.AlphaToCoverageEnable = TRUE;
    ac.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&ac, &bsA2C);
    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = (sizeof(FrameCB) + 15) & ~15u;
    dev->CreateBuffer(&cb, nullptr, &frameCb);
    cb.ByteWidth = (sizeof(LightCB) + 15) & ~15u;
    dev->CreateBuffer(&cb, nullptr, &lightCb);
    // shadow map (R32 depth, comparison + point samplers)
    {
        D3D11_TEXTURE2D_DESC st = {};
        st.Width = st.Height = shadowSize;
        st.MipLevels = 1;
        st.ArraySize = 1;
        st.Format = DXGI_FORMAT_R32_TYPELESS;
        st.SampleDesc.Count = 1;
        st.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&st, nullptr, &shadowTex);
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(shadowTex, &dv, &shadowDsv);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(shadowTex, &sv, &shadowSrv);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
        sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &shadowCmp);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        dev->CreateSamplerState(&sd, &shadowPoint);
        D3D11_RASTERIZER_DESC r = {};
        r.FillMode = D3D11_FILL_SOLID;
        r.CullMode = D3D11_CULL_NONE;
        r.DepthClipEnable = TRUE;
        r.DepthBias = 200;
        r.SlopeScaledDepthBias = 1.5f;
        dev->CreateRasterizerState(&r, &rsShadow);
        r.FillMode = D3D11_FILL_WIREFRAME;
        r.DepthBias = 0;
        r.SlopeScaledDepthBias = 0;
        r.AntialiasedLineEnable = TRUE;
        dev->CreateRasterizerState(&r, &rsWire);
        // sky-occlusion maps: R32 depth array, one slice per sky direction
        st.Width = st.Height = aoSize;
        st.ArraySize = kMaxAoDirs;
        if (SUCCEEDED(dev->CreateTexture2D(&st, nullptr, &aoTex))) {
            for (int k = 0; k < kMaxAoDirs; k++) {
                D3D11_DEPTH_STENCIL_VIEW_DESC av = {};
                av.Format = DXGI_FORMAT_D32_FLOAT;
                av.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                av.Texture2DArray.FirstArraySlice = k;
                av.Texture2DArray.ArraySize = 1;
                dev->CreateDepthStencilView(aoTex, &av, &aoDsv[k]);
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC asv = {};
            asv.Format = DXGI_FORMAT_R32_FLOAT;
            asv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            asv.Texture2DArray.MipLevels = 1;
            asv.Texture2DArray.ArraySize = kMaxAoDirs;
            dev->CreateShaderResourceView(aoTex, &asv, &aoSrv);
        }
    }
    return true;
}

bool Gfx::loadShaders(const std::string& dir, std::string& err) {
    auto vsb = readFile(dir + "\\basic_vs.cso"), pm = readFile(dir + "\\basic_ps_mesh.cso"), pl = readFile(dir + "\\basic_ps_line.cso");
    if (vsb.empty() || pm.empty() || pl.empty()) { err = "missing compiled shaders (basic_*.cso) in " + dir; return false; }
    dev->CreateVertexShader(vsb.data(), vsb.size(), nullptr, &vs);
    dev->CreatePixelShader(pm.data(), pm.size(), nullptr, &psMesh);
    dev->CreatePixelShader(pl.data(), pl.size(), nullptr, &psLine);
    auto pda = readFile(dir + "\basic_ps_depth_alpha.cso");   // optional: without it beard cards cast no shadow
    if (!pda.empty()) dev->CreatePixelShader(pda.data(), pda.size(), nullptr, &psDepthAlpha);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(dev->CreateInputLayout(el, 3, vsb.data(), vsb.size(), &layout))) { err = "CreateInputLayout failed"; return false; }
    return true;
}

void Gfx::createTargets() {
    ID3D11Texture2D* bb = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    D3D11_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(bb, &rv, &bbRtv);
    safeRelease(bb);
    createSceneTargets();
}

void Gfx::createSceneTargets() {
    D3D11_RENDER_TARGET_VIEW_DESC rv = {};
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = samples;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    dev->CreateTexture2D(&td, nullptr, &msaaTex);
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    dev->CreateRenderTargetView(msaaTex, &rv, &msaaRtv);
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    dev->CreateTexture2D(&td, nullptr, &depthTex);
    dev->CreateDepthStencilView(depthTex, nullptr, &dsv);
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.BindFlags = 0;
    dev->CreateTexture2D(&td, nullptr, &resolveTex);
}

void Gfx::releaseTargets() {
    safeRelease(bbRtv); safeRelease(msaaRtv); safeRelease(msaaTex); safeRelease(dsv); safeRelease(depthTex); safeRelease(resolveTex);
}

void Gfx::resize(int nw, int nh) {
    if (nw <= 0 || nh <= 0 || (nw == w && nh == h)) return;
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    releaseTargets();
    w = nw;
    h = nh;
    swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    createTargets();
}

void Gfx::beginScene(const float clear[4]) {
    ctx->OMSetRenderTargets(1, &msaaRtv, dsv);
    ctx->ClearRenderTargetView(msaaRtv, clear);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    D3D11_VIEWPORT vp = {0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
}

void Gfx::setFrame(const FrameCB& f) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(frameCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &f, sizeof f);
        ctx->Unmap(frameCb, 0);
    }
}

void Gfx::upload(const std::vector<GVertex>& v) {
    size_t bytes = v.size() * sizeof(GVertex);
    if (bytes > dynCap) {
        safeRelease(dynVb);
        dynCap = std::max<size_t>(bytes * 2, 1 << 20);
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = (UINT)dynCap;
        dev->CreateBuffer(&bd, nullptr, &dynVb);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(dynVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, v.data(), bytes);
        ctx->Unmap(dynVb, 0);
    }
}

void Gfx::resetPipeline() {
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->CSSetShader(nullptr, nullptr, 0);
    ctx->IASetInputLayout(layout);
    UINT stride = sizeof(GVertex), off = 0;
    ctx->IASetVertexBuffers(0, 1, &dynVb, &stride, &off);
    ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &frameCb);
    ctx->PSSetConstantBuffers(0, 1, &frameCb);
    ctx->PSSetConstantBuffers(1, 1, &lightCb);
    ctx->OMSetRenderTargets(1, &msaaRtv, dsv);
    D3D11_VIEWPORT vp = {0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &vp);
}

void Gfx::drawTris(const std::vector<GVertex>& v, ID3D11ShaderResourceView* tex, ID3D11ShaderResourceView* nrm, bool a2c) {
    if (v.empty()) return;
    upload(v);
    resetPipeline();
    if (!samp) {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = 8;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &samp);
    }
    ctx->PSSetSamplers(0, 1, &samp);
    ID3D11ShaderResourceView* srv2[2] = {tex, nrm};
    ctx->PSSetShaderResources(0, 2, srv2);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->PSSetShader(psMesh, nullptr, 0);
    ctx->RSSetState(wireframe && rsWire ? rsWire : rsSolid);
    ctx->OMSetDepthStencilState(dsOn, 0);
    ctx->OMSetBlendState(a2c && bsA2C ? bsA2C : bsOpaque, nullptr, 0xffffffff);
    ctx->Draw((UINT)v.size(), 0);
    if (a2c) ctx->OMSetBlendState(bsOpaque, nullptr, 0xffffffff);
}

void Gfx::drawTrisDepthAlpha(const std::vector<GVertex>& v, ID3D11ShaderResourceView* tex) {
    if (v.empty() || !psDepthAlpha || !tex) return;
    drawTrisDepthWith(v, psDepthAlpha, tex);
}

void Gfx::beginShadowPass() {
    ID3D11ShaderResourceView* nul[2] = {};
    ctx->PSSetShaderResources(8, 2, nul);
    ctx->OMSetRenderTargets(0, nullptr, shadowDsv);
    ctx->ClearDepthStencilView(shadowDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp = {0, 0, float(shadowSize), float(shadowSize), 0, 1};
    ctx->RSSetViewports(1, &vp);
}

void Gfx::beginAoPass(int k) {
    ID3D11ShaderResourceView* nul[2] = {};
    ctx->PSSetShaderResources(8, 2, nul);
    ctx->OMSetRenderTargets(0, nullptr, aoDsv[k]);
    ctx->ClearDepthStencilView(aoDsv[k], D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp = {0, 0, float(aoSize), float(aoSize), 0, 1};
    ctx->RSSetViewports(1, &vp);
}

void Gfx::drawTrisDepth(const std::vector<GVertex>& v) { drawTrisDepthWith(v, nullptr, nullptr); }

void Gfx::drawTrisDepthWith(const std::vector<GVertex>& v, ID3D11PixelShader* ps, ID3D11ShaderResourceView* tex) {
    if (v.empty()) return;
    upload(v);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->IASetInputLayout(layout);
    UINT stride = sizeof(GVertex), off = 0;
    ctx->IASetVertexBuffers(0, 1, &dynVb, &stride, &off);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &frameCb);
    ctx->PSSetShader(ps, nullptr, 0);
    if (ps) {
        ctx->PSSetShaderResources(0, 1, &tex);
        if (samp) ctx->PSSetSamplers(0, 1, &samp);
    }
    ctx->RSSetState(rsShadow);
    ctx->OMSetDepthStencilState(dsOn, 0);
    ctx->OMSetBlendState(bsOpaque, nullptr, 0xffffffff);
    ctx->Draw((UINT)v.size(), 0);
}

void Gfx::bindShadow(bool on, bool ao) {
    ID3D11ShaderResourceView* s[2] = {on ? shadowSrv : nullptr, ao ? aoSrv : nullptr};
    ctx->PSSetShaderResources(8, 2, s);
    ID3D11SamplerState* ss[2] = {shadowCmp, shadowPoint};
    ctx->PSSetSamplers(2, 2, ss);
}

void Gfx::drawColor(const std::vector<GVertex>& v) {
    if (v.empty()) return;
    upload(v);
    resetPipeline();
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->PSSetShader(psLine, nullptr, 0);
    ctx->RSSetState(rsSolid);
    ctx->OMSetDepthStencilState(dsOff, 0);
    ctx->OMSetBlendState(bsOpaque, nullptr, 0xffffffff);
    ctx->Draw((UINT)v.size(), 0);
}

void Gfx::drawLines(const std::vector<GVertex>& v, bool depth) {
    if (v.empty()) return;
    upload(v);
    resetPipeline();
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx->PSSetShader(psLine, nullptr, 0);
    ctx->RSSetState(rsLine);
    ctx->OMSetDepthStencilState(depth ? dsOn : dsOff, 0);
    ctx->OMSetBlendState(bsAlpha, nullptr, 0xffffffff);
    ctx->Draw((UINT)v.size(), 0);
}

bool Gfx::beginOffscreen(int size, int height) {
    if (offscreen) return false;
    saved = {msaaTex, depthTex, resolveTex, msaaRtv, dsv, w, h};
    msaaTex = depthTex = resolveTex = nullptr;
    msaaRtv = nullptr;
    dsv = nullptr;
    w = size;
    h = height > 0 ? height : size;
    createSceneTargets();
    offscreen = true;
    return msaaRtv != nullptr;
}

bool Gfx::endOffscreen(const std::string& png, std::vector<unsigned char>* rgba) {
    if (!offscreen) return false;
    ctx->ResolveSubresource(resolveTex, 0, msaaTex, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    D3D11_TEXTURE2D_DESC td;
    resolveTex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    dev->CreateTexture2D(&td, nullptr, &st);
    ctx->CopyResource(st, resolveTex);
    bool ok = false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (st && SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        std::vector<unsigned char> px(size_t(td.Width) * td.Height * 4);
        for (UINT y = 0; y < td.Height; y++) memcpy(&px[size_t(y) * td.Width * 4], (char*)m.pData + size_t(y) * m.RowPitch, td.Width * 4);
        ctx->Unmap(st, 0);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        ok = png.empty() || stbi_write_png(png.c_str(), td.Width, td.Height, 4, px.data(), td.Width * 4) != 0;
        if (rgba) rgba->swap(px);
    }
    safeRelease(st);
    safeRelease(msaaRtv); safeRelease(msaaTex); safeRelease(dsv); safeRelease(depthTex); safeRelease(resolveTex);
    msaaTex = saved.m; depthTex = saved.d; resolveTex = saved.r; msaaRtv = saved.mr; dsv = saved.ds; w = saved.w; h = saved.h;
    offscreen = false;
    return ok;
}

void Gfx::endScene() {
    ctx->ResolveSubresource(resolveTex, 0, msaaTex, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    ID3D11Texture2D* bb = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    ctx->CopyResource(bb, resolveTex);
    safeRelease(bb);
    ctx->OMSetRenderTargets(1, &bbRtv, nullptr);
}

void Gfx::present(bool vsync) { swap->Present(vsync ? 1 : 0, 0); }

bool Gfx::saveScreenshot(const std::string& path) {
    // the back buffer (scene + UI): copy to a staging texture
    ID3D11Texture2D* bb = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    dev->CreateTexture2D(&td, nullptr, &st);
    ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = false;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        std::vector<unsigned char> px(size_t(td.Width) * td.Height * 4);
        for (UINT y = 0; y < td.Height; y++) memcpy(&px[size_t(y) * td.Width * 4], (char*)m.pData + size_t(y) * m.RowPitch, td.Width * 4);
        ctx->Unmap(st, 0);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        ok = stbi_write_png(path.c_str(), td.Width, td.Height, 4, px.data(), td.Width * 4) != 0;
    }
    safeRelease(st);
    safeRelease(bb);
    return ok;
}

bool Gfx::saveViewportJpg(const std::string& path, int x0, int y0, int x1, int y1, size_t maxBytes) {
    ID3D11Texture2D* bb = nullptr;
    swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    dev->CreateTexture2D(&td, nullptr, &st);
    ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = false;
    x0 = std::clamp(x0, 0, (int)td.Width - 1); x1 = std::clamp(x1, x0 + 1, (int)td.Width);
    y0 = std::clamp(y0, 0, (int)td.Height - 1); y1 = std::clamp(y1, y0 + 1, (int)td.Height);
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        int w = x1 - x0, h = y1 - y0;
        int ow = std::min(w, 1280), oh = std::max(1, int(h * (double)ow / w));
        std::vector<unsigned char> px(size_t(ow) * oh * 3);
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) {       // box filter over the source footprint of the output pixel
                int sx0 = x0 + int(x * (double)w / ow), sx1 = std::max(sx0 + 1, x0 + int((x + 1) * (double)w / ow));
                int sy0 = y0 + int(y * (double)h / oh), sy1 = std::max(sy0 + 1, y0 + int((y + 1) * (double)h / oh));
                int acc[3] = {0, 0, 0}, n = 0;
                for (int sy = sy0; sy < sy1 && sy < y1; sy++)
                    for (int sx = sx0; sx < sx1 && sx < x1; sx++) {
                        const unsigned char* s = (const unsigned char*)m.pData + size_t(sy) * m.RowPitch + size_t(sx) * 4;
                        acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2]; n++;
                    }
                for (int c = 0; c < 3; c++) px[(size_t(y) * ow + x) * 3 + c] = (unsigned char)(acc[c] / std::max(n, 1));
            }
        ctx->Unmap(st, 0);
        for (int q = 88; q >= 30 && !ok; q -= 8) {
            std::vector<unsigned char> buf;
            stbi_write_jpg_to_func([](void* c, void* d, int n) { auto* v = (std::vector<unsigned char>*)c; v->insert(v->end(), (unsigned char*)d, (unsigned char*)d + n); }, &buf, ow, oh, 3, px.data(), q);
            if (buf.size() <= maxBytes) {
                FILE* f = nullptr;
                if (fopen_s(&f, path.c_str(), "wb") == 0 && f) { ok = fwrite(buf.data(), 1, buf.size(), f) == buf.size(); fclose(f); }
                break;
            }
        }
    }
    safeRelease(st);
    safeRelease(bb);
    return ok;
}

ID3D11ShaderResourceView* Gfx::loadTexture(const std::string& path, bool srgb, int* ow, int* oh) {
    int tw, th, n;
    unsigned char* px = stbi_load(path.c_str(), &tw, &th, &n, 4);
    if (!px) return nullptr;
    // full mip chain (the density map is sampled at hair-root scale)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = tw;
    td.Height = th;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex))) {
        ctx->UpdateSubresource(tex, 0, nullptr, px, tw * 4, 0);
        dev->CreateShaderResourceView(tex, nullptr, &srv);
        if (srv) ctx->GenerateMips(srv);
        safeRelease(tex);
    }
    stbi_image_free(px);
    if (ow) *ow = tw;
    if (oh) *oh = th;
    return srv;
}

void Gfx::shutdown() {
    releaseTargets();
    safeRelease(vs); safeRelease(psMesh); safeRelease(psLine); safeRelease(psDepthAlpha); safeRelease(bsA2C); safeRelease(layout);
    safeRelease(frameCb); safeRelease(lightCb); safeRelease(rsSolid); safeRelease(rsLine);
    safeRelease(samp);
    safeRelease(shadowTex); safeRelease(shadowDsv); safeRelease(shadowSrv); safeRelease(shadowCmp); safeRelease(shadowPoint);
    safeRelease(rsShadow); safeRelease(rsWire);
    for (auto& d : aoDsv) safeRelease(d);
    safeRelease(aoSrv); safeRelease(aoTex);
    safeRelease(dsOn); safeRelease(dsOff); safeRelease(bsOpaque); safeRelease(bsAlpha); safeRelease(dynVb);
    safeRelease(swap); safeRelease(ctx); safeRelease(dev);
}
