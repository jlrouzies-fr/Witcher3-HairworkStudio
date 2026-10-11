// Desktop Groom Studio - meshes (head / outfit) and debug lines. Our own code (MIT).
#include "studio_light.hlsli"

cbuffer Frame : register(b0)
{
    row_major float4x4 g_viewProj;
    float4 g_color;      // tint / line colour multiplier
    float4 g_camPos;
    float4 g_pad0;
    float4 g_pad1;
};

struct VSIn { float3 pos : POSITION; float3 nrm : NORMAL; float4 col : COLOR; };
struct VSOut { float4 pos : SV_Position; float3 nrm : NORMAL; float4 col : COLOR; float3 wpos : TEXCOORD0; };

VSOut vs_main(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0f), g_viewProj);
    o.nrm = i.nrm;
    o.col = i.col;
    o.wpos = i.pos;
    return o;
}

Texture2D g_tex : register(t0);
Texture2D g_nrm : register(t1);
SamplerState g_samp : register(s0);

float4 ps_mesh(VSOut i) : SV_Target
{
    float3 n = normalize(i.nrm);
    float3 v = normalize(g_camPos.xyz - i.wpos);
    if (dot(n, v) < 0) n = -n;
    if (i.col.w > 2.5f) {
        // tangent-space normal map (game _n01: xy in RG), frame from screen-space derivatives (cotangent frame)
        float3 dp1 = ddx(i.wpos), dp2 = ddy(i.wpos);
        float2 du1 = ddx(i.col.xy), du2 = ddy(i.col.xy);
        float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
        float3 Tt = dp2perp * du1.x + dp1perp * du2.x;
        float3 Bt = dp2perp * du1.y + dp1perp * du2.y;
        float invmax = rsqrt(max(max(dot(Tt, Tt), dot(Bt, Bt)), 1e-20f));
        float2 xy = g_nrm.Sample(g_samp, i.col.xy).rg * 2.0f - 1.0f;
        float z = sqrt(saturate(1.0f - dot(xy, xy)));
        float3 nm = normalize(Tt * invmax * xy.x - Bt * invmax * xy.y + n * z);
        if (all(isfinite(nm))) n = nm;
    }
    float3 l = normalize(g_sunDir.xyz);
    // col.w > 1.5: textured (col.xy = uv, col.z = material class: 0 cloth / leather / metal, 1 skin, 2 eye, 3 mouth);
    // the diffuse map is an sRGB texture (linearised by the sampler)
    bool texd = i.col.w > 1.5f;
    int cls = texd ? (int)(i.col.z + 0.5f) : 0;
    float3 albedo = (texd ? g_tex.Sample(g_samp, i.col.xy).rgb * 0.8f : i.col.rgb) * g_color.rgb;
    float nl = dot(n, l);
    float vis = Studio_Shadow(i.wpos + n * 0.002f, i.pos.xy);
    float sky = Studio_SkyVis(i.wpos, n);
    float3 sun = g_sunCol.rgb * g_sunDir.w;
    float3 diffSun, amb = Studio_SH(n);
    float spec = 0.0f, rough = 0.7f, f0 = 0.04f, cav = 1.0f;
    if (cls == 1) {
        // skin: wrapped (subsurface) diffuse, more wrap for red; the shadow edge bleeds red; GGX lobe of oily skin
        albedo *= g_skin.z;
        float3 w = g_skin.x * float3(0.55f, 0.22f, 0.15f);
        float3 wrapNL = saturate((nl + w) / (1.0f + w)) / (1.0f + w * 0.5f);
        float3 visC = pow(saturate(vis), 1.0f / (1.0f + 3.0f * w));
        diffSun = wrapNL * visC;
        spec = Studio_GGX(n, v, l, 0.42f, 0.028f) * g_skin.y * vis;
        rough = 0.42f; f0 = 0.028f;
        // ambient light scattered under the skin is redder (multi-bounce through the dermis)
        amb *= lerp(1.0f, float3(1.18f, 0.90f, 0.84f), saturate(g_skin.x * 2.0f));
    } else if (cls == 2) {
        // eyeball: wet, sharp highlight, darkened by lids / lashes the preview does not draw
        diffSun = saturate(nl) * vis;
        spec = Studio_GGX(n, v, l, 0.12f, 0.03f) * vis;
        rough = 0.12f; f0 = 0.03f; cav = lerp(1.0f, 0.62f, g_skin.w);
    } else if (cls == 4) {
        // vanilla beard / moustache card (alpha-tested, alpha to coverage): strand-like soft diffuse, no normal map;
        // the card diffuse is authored bright (the game's hair shader darkens it), x0.55 matches the in-game grey
        albedo *= 0.55f;
        diffSun = saturate(nl * 0.5f + 0.5f) * saturate(nl + 0.4f) * vis;
        spec = Studio_GGX(n, v, l, 0.5f, 0.04f) * 0.3f * vis;
        cav = 0.9f;
    } else if (cls == 3) {
        diffSun = saturate(nl) * vis;
        cav = lerp(1.0f, 0.25f, g_skin.w);
    } else {
        diffSun = saturate(nl) * vis;
        spec = Studio_GGX(n, v, l, 0.65f, 0.04f) * 0.5f * vis;
    }
    // environment specular (sky reflection), rougher surfaces see a blurrier (dimmer) sky
    float3 r = reflect(-v, n);
    float fr = f0 + (1.0f - f0) * pow(1.0f - saturate(dot(n, v)), 5.0f);
    float3 envSpec = Studio_Env(r) * fr * (1.0f - rough * 0.7f) * g_skyZen.w * (cls == 1 ? g_skin.y : 1.0f);
    float3 c = (albedo * (sun * diffSun + amb * sky) + spec * sun + envSpec * sky) * cav;
    float alpha = 1.0f;
    if (cls == 4) {
        float ta = g_tex.Sample(g_samp, i.col.xy).a;
        clip(ta - 0.1f);
        alpha = smoothstep(0.1f, 0.6f, ta);      // alpha to coverage around the card's 0.3 reference (soft strand ends)
    }
    return float4(Studio_Tonemap(c), alpha);
}

// depth pass of alpha-tested cards (shadow / sky-occlusion maps): only the opaque part of the card casts
void ps_depth_alpha(VSOut i)
{
    clip(g_tex.Sample(g_samp, i.col.xy).a - 0.3f);
}

float4 ps_line(VSOut i) : SV_Target
{
    return float4(i.col.rgb * g_color.rgb, i.col.a);
}
