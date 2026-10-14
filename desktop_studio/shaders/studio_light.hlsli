// Desktop Groom Studio - shared physically based light rig: sun + soft (contact-hardening) sun shadow, procedural sky /
// ground environment as SH9 irradiance, sky-occlusion maps, filmic SDR curve. Our own code (MIT).
// Light values are irradiance / pi (a white Lambert surface facing the sun shows g_sunDir.w x g_sunCol).
#ifndef STUDIO_LIGHT_HLSLI
#define STUDIO_LIGHT_HLSLI

cbuffer StudioLight : register(b1)
{
    float4 g_sunDir;      // xyz towards the sun, w intensity
    float4 g_sunCol;
    float4 g_skyCol;      // legacy (sky colour, w intensity): the SH below is what lights the scene
    float4 g_groundCol;
    float4 g_params;      // x exposure, y colour gain, z spec gain, w self-shadow (fallback when no occlusion maps)
    float4 g_head;        // xyz head centre, w head radius
    float4 g_mat;         // x ambientEnvScale, y light floor, z textureBrightness, w colour texture bound
    float4 g_extra;       // x diffuseNoiseScale (redfur), y per-strand variation
    row_major float4x4 g_lightVP;
    float4 g_shadow;      // x strength (0 = off), y depth bias, z texel size (uv), w minimum filter radius (texels)
    float4 g_shadow2;     // x light depth range (m), y hair debug view, z specular power scale, w sun penumbra (tan of the angular radius / texel size in m)
    float4 g_sh[9];       // SH9 irradiance / pi
    float4 g_skyZen;      // rgb zenith radiance, w environment specular gain (meshes)
    float4 g_skyHor;      // rgb horizon radiance
    float4 g_groundRad;   // rgb ground radiance, w final saturation
    float4 g_skin;        // x subsurface wrap, y skin specular, z skin albedo gain, w mouth / eye cavity occlusion
    float4 g_ao;          // x number of sky-occlusion maps, y strength, z depth range (m), w texel (uv)
    float4 g_hair2;       // x deep-shadow sigma scale, y multiple-scattering spread, z back light (TT) gain, w hair env spec
    float4 g_aoDir[16];   // xyz direction towards the sky, w weight
    row_major float4x4 g_aoVP[16];
    float4 g_hair3;       // x hair colour encoding (1 = redfur values as stored, like the game; 0 = sRGB-decoded),
                          // y sky occlusion at the strand root (fibre detail in shade), z per-strand sky variation,
                          // w strand-direction term (fibre seen across vs end-on)
};

Texture2D g_shadowMap : register(t8);   // R32 depth (float4-typed so the SDK shadow helpers take it)
Texture2DArray<float> g_aoMap : register(t9);
SamplerComparisonState g_shadowCmp : register(s2);
SamplerState g_shadowPoint : register(s3);

static const float2 kPoisson16[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464), float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420), float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)};

float3 Studio_ShadowCoord(float3 wp)
{
    float4 c = mul(float4(wp, 1.0f), g_lightVP);
    float3 p = c.xyz / c.w;
    return float3(p.x * 0.5f + 0.5f, 0.5f - p.y * 0.5f, p.z);
}

float Studio_Rand(float2 p) { return frac(sin(dot(p, float2(12.9898f, 78.233f))) * 43758.5453f); }

// sun visibility for opaque surfaces: contact-hardening soft shadow (blocker search, then a Poisson PCF whose radius
// grows with the blocker - receiver distance x the sun's angular size), rotated per pixel against banding
float Studio_Shadow(float3 wp, float2 pix)
{
    if (g_shadow.x <= 0.0f) return 1.0f;
    float3 sc = Studio_ShadowCoord(wp);
    if (any(sc.xy < 0.0f) || any(sc.xy > 1.0f) || sc.z > 1.0f) return 1.0f;
    float ang = Studio_Rand(pix) * 6.2831853f;
    float2x2 rot = float2x2(cos(ang), -sin(ang), sin(ang), cos(ang));
    float d = sc.z - g_shadow.y;
    // blocker search over ~12 texels
    float bsum = 0.0f, bn = 0.0f;
    [unroll] for (int i = 0; i < 16; i += 2) {
        float o = g_shadowMap.SampleLevel(g_shadowPoint, sc.xy + mul(rot, kPoisson16[i]) * g_shadow.z * 12.0f, 0).r;
        if (o < d) { bsum += o; bn += 1.0f; }
    }
    if (bn < 0.5f) return 1.0f;
    float distM = (d - bsum / bn) * g_shadow2.x;                       // metres between blocker and receiver
    float rad = clamp(distM * g_shadow2.w, g_shadow.w, 24.0f);          // penumbra radius in texels
    float s = 0.0f;
    [unroll] for (int j = 0; j < 16; j++)
        s += g_shadowMap.SampleCmpLevelZero(g_shadowCmp, sc.xy + mul(rot, kPoisson16[j]) * g_shadow.z * rad, d);
    return lerp(1.0f, s / 16.0f, g_shadow.x);
}

// SH9 irradiance / pi of the sky + ground environment along n
float3 Studio_SH(float3 n)
{
    float x = n.x, y = n.y, z = n.z;
    float3 r = g_sh[0].rgb * 0.282095f
             + g_sh[1].rgb * (0.488603f * y) + g_sh[2].rgb * (0.488603f * z) + g_sh[3].rgb * (0.488603f * x)
             + g_sh[4].rgb * (1.092548f * x * y) + g_sh[5].rgb * (1.092548f * y * z) + g_sh[6].rgb * (0.315392f * (3.0f * z * z - 1.0f))
             + g_sh[7].rgb * (1.092548f * x * z) + g_sh[8].rgb * (0.546274f * (x * x - y * y));
    return max(r, 0.0f);
}

// environment radiance towards d (reflections): zenith / horizon / ground
float3 Studio_Env(float3 d)
{
    if (d.z >= 0.0f) return lerp(g_skyHor.rgb, g_skyZen.rgb, sqrt(saturate(d.z / 0.55f)));
    return lerp(g_skyHor.rgb, g_groundRad.rgb, saturate(-d.z / 0.12f));
}

// fraction of the sky map k that sees the sky from wp (4 texels compared, 1 = open sky)
float Studio_AoOpen(float3 wp, int k)
{
    float4 c = mul(float4(wp, 1.0f), g_aoVP[k]);
    float2 uv = float2(c.x * 0.5f + 0.5f, 0.5f - c.y * 0.5f);
    if (any(uv < 0.0f) || any(uv > 1.0f) || c.z > 1.0f) return 1.0f;
    float4 o = g_aoMap.Gather(g_shadowPoint, float3(uv, k));
    float4 open = step(c.z - 0.0012f, o);
    return dot(open, 0.25f);
}

// directional sky visibility of an opaque surface: the occlusion maps weighted by sky radiance x the cosine lobe
float Studio_SkyVis(float3 wp, float3 n)
{
    int cnt = (int)(g_ao.x + 0.5f);
    if (cnt <= 0) return 1.0f;
    float num = 0.0f, den = 0.0f;
    float3 p = wp + n * 0.004f;
    [loop] for (int k = 0; k < cnt; k++) {
        float3 d = g_aoDir[k].xyz;
        float w = g_aoDir[k].w * saturate(dot(n, d) * 0.85f + 0.15f);
        num += w * Studio_AoOpen(p + d * 0.002f, k);
        den += w;
    }
    return lerp(1.0f, num / max(den, 1e-5f), g_ao.y);
}

// GGX specular (normalised, x N.L), Schlick fresnel
float Studio_GGX(float3 n, float3 v, float3 l, float rough, float f0)
{
    float3 h = normalize(v + l);
    float nl = saturate(dot(n, l)), nv = saturate(dot(n, v)) + 1e-4f, nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    float a = rough * rough, a2 = a * a;
    float dd = nh * nh * (a2 - 1.0f) + 1.0f;
    float D = a2 / (3.14159265f * dd * dd);
    float k = a * 0.5f;
    float G = (nl / (nl * (1 - k) + k)) * (nv / (nv * (1 - k) + k));
    float F = f0 + (1.0f - f0) * pow(1.0f - vh, 5.0f);
    return D * G * F / (4.0f * nv + 1e-4f) * 3.14159265f;   // x pi: our light values are irradiance / pi
}

// filmic SDR curve: ACES RRT + ODT fit (sRGB primaries), what an SDR game capture looks like after tonemapping
float3 Studio_Tonemap(float3 x)
{
    x *= g_params.x;
    const float3x3 inM = {0.59719f, 0.35458f, 0.04823f, 0.07600f, 0.90834f, 0.01566f, 0.02840f, 0.13383f, 0.83777f};
    const float3x3 outM = {1.60475f, -0.53108f, -0.07367f, -0.10208f, 1.10813f, -0.00605f, -0.00327f, -0.07276f, 1.07602f};
    float3 v;
    if (g_skyHor.w > 0.5f) {
        // hue-preserving variant: the same curve on luminance, colour ratios kept (highlights desaturate towards white)
        float lx = dot(mul(inM, x), float3(0.2126f, 0.7152f, 0.0722f));
        float lt = (lx * (lx + 0.0245786f) - 0.000090537f) / (lx * (0.983729f * lx + 0.4329510f) + 0.238081f);
        lt = saturate(dot(mul(outM, lt.xxx), float3(0.2126f, 0.7152f, 0.0722f)));
        float lin = max(dot(x, float3(0.2126f, 0.7152f, 0.0722f)), 1e-5f);
        v = x * (lt / lin);
        float mx = max(v.r, max(v.g, v.b));
        if (mx > 1.0f) v = lerp(v / mx, 1.0f, saturate((mx - 1.0f) * 0.5f));
        v = saturate(v);
    } else {
        v = mul(inM, x);
        v = (v * (v + 0.0245786f) - 0.000090537f) / (v * (0.983729f * v + 0.4329510f) + 0.238081f);
        v = saturate(mul(outM, v));
    }
    float l = dot(v, float3(0.2126f, 0.7152f, 0.0722f));
    return saturate(lerp(l.xxx, v, g_groundRad.w));
}

// legacy hemisphere (kept for the debug views)
float3 Studio_Ambient(float3 n) { return Studio_SH(n); }
#endif
