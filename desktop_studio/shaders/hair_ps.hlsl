// Desktop Groom Studio - hair pixel shader. Our own code (MIT).
// It uses the HairWorks custom-shader interface (NvHair_PixelShaderInput, NvHair_GetShaderAttributes, the material the
// runtime fills from the instance descriptor) from the SDK header in the local SDK folder (include path set by CMake;
// the header is never copied here). The shading below is ours: a Kajiya-Kay style strand model driven by the same
// redfur parameters the game reads (root / tip colour + falloff, diffuseBlend, hairNormalWeight, primary / secondary
// specular scale / power / offset / breakup, glint, shadowSigma, ambientEnvScale), lit by the studio rig and pushed
// through the same SDR curve as the meshes so it can be compared with the HairLab captures.
#include <Nv/HairWorks/Shader/NvHairShaderCommon.h>
#include "studio_light.hlsli"

NV_HAIR_DECLARE_SHADER_RESOURCES(t0, t1, t2);
Texture2D g_rootTex : register(t3);
Texture2D g_tipTex : register(t4);
SamplerState g_samp : register(s0);

cbuffer HairCb : register(b0)          // filled by HairWorks (renderHairs)
{
    NvHair_ConstantBuffer g_hair;
}

// per-strand random in [0, 1). Integer hash of the float's bits: the old frac(sin(x)) form lost all precision for large
// hair ids (sin of 1e6+), so whole regions of strands (e.g. a moustache of added guides) got the same value
float Strand_Hash(float x)
{
    uint u = asuint(x);
    u ^= u >> 16; u *= 0x7feb352du; u ^= u >> 15; u *= 0x846ca68bu; u ^= u >> 16;
    return float(u & 0xffffffu) / 16777216.0f;
}
// signed per-strand variation for the calibrated terms. The looks were calibrated (Vesemir r31a vs the game) while the
// old hash returned ~0 for most strands, i.e. a variation of -0.5: keep that mean, spread +-0.25 around it
float Strand_Var(float x) { return Strand_Hash(x) * 0.5f - 0.75f; }

// Kajiya-Kay: lighting of a thin cylinder with tangent t
float Strand_Diffuse(float3 t, float3 l) { float tl = dot(t, l); return sqrt(saturate(1.0f - tl * tl)); }
float Strand_Spec(float3 t, float3 v, float3 l, float power)
{
    float3 h = normalize(l + v);
    float th = dot(t, h);
    float s = sqrt(saturate(1.0f - th * th));
    return pow(s, max(power, 1.0f)) * saturate(dot(l, cross(t, cross(l, t))) * 0.5f + 0.5f);
}

// multiple-scattering saturation of the fibre hue (max channel 1): hue^k, or (g_pt.z = 1, game-accurate PT look) the same
// saturation with the hue kept: every channel moves away from 1 by the factor that takes the lowest channel to min^k, so
// orange stays orange instead of turning crimson (hue^k darkens green faster than red: Triss L387); grey hair is unchanged
float3 MsSat(float3 hue, float k) {
    float3 h = max(hue, 1e-3f);
    float3 p = pow(h, k);
    float m = min(h.r, min(h.g, h.b));
    float f = m < 0.999f ? (1.0f - pow(m, k)) / (1.0f - m) : k;
    float3 q = saturate(1.0f - (1.0f - h) * f);
    return lerp(p, q, saturate(g_pt.z));
}

[earlydepthstencil]
// SV_Target1 (path-traced look only, otherwise unbound): +1 per hair fragment and sample, added up (strand layers per pixel)
float4 main(NvHair_PixelShaderInput input, out float4 layers : SV_Target1) : SV_Target
{
    layers = float4(1.0f, 0.0f, 0.0f, 0.0f);
    NvHair_ShaderAttributes a = NvHair_GetShaderAttributes(input, g_hair);
    NvHair_Material m = g_hair.defaultMaterial;

    float3 debugColor;
    if (NvHair_VisualizeColor(g_hair, m, a, debugColor))
        return float4(debugColor, 1.0f);

    float s = a.texcoords.z;                         // 0 root .. 1 tip
    float3 base = NvHair_SampleHairColor(m, a.texcoords);
    // colour encoding (g_hair3.x = 1, default): the game's path-traced hair takes the redfur colours / card texture
    // values as stored (8-bit values / 255, no sRGB decode), so the fibre colour is much less saturated than the
    // decoded value; the saturation it shows comes from multiple scattering (hue^k below). Fitted on L244 / L232:
    // with the stored values one exponent pair explains both the sun-lit gold and the olive shade of Guillaume.
    float enc = g_hair3.x;
    float encExp = lerp(1.0f, 1.0f / 2.2f, enc);    // 0 = sRGB-decoded, 1 = stored value; in between = partial gamma
    base = pow(max(base, 0.0f), encExp);
    if (g_mat.w > 0.5f) {
        // card-texture colour: the texture (x textureBrightness) is the colour, root / tip constants do not tint
        float3 r = g_rootTex.SampleLevel(g_samp, a.texcoords.xy, 0).rgb, t = g_tipTex.SampleLevel(g_samp, a.texcoords.xy, 0).rgb;
        float3 tc = lerp(r, t, NvHair_GetRootTipRatio(s, m));
        base = pow(max(tc, 0.0f), encExp) * g_mat.z;
    }
    // game-accurate colour gain (g_pt.w, App): textureBrightness without a card texture (in game it scales the hair colour,
    // L480) x the PTHairRoughness brightness; the card-texture path above already carries textureBrightness in g_mat.z
    if (g_pt.w > 0.0f) base *= g_pt.w;
    base *= g_params.y;
    // per-strand variation (strand definition): the fur's diffuseNoiseScale + the rig's variation, by hair id
    float hv = Strand_Var(a.hairID * 1.731f + 0.37f);
    base *= max(1.0f + hv * 2.0f * (g_extra.y + 0.5f * g_extra.x), 0.0f);

    float3 T = normalize(a.T);
    float3 V = normalize(a.V);
    float3 L = normalize(g_sunDir.xyz);
    float3 fromHead = a.P - g_head.xyz;
    float dist = length(fromHead);
    float3 Nh = normalize(lerp(normalize(a.N), fromHead / max(dist, 1e-4f), m.diffuseHairNormalWeight));
    float3 hue = base / max(max(base.r, max(base.g, base.b)), 1e-4f);
    // light that reaches the eye from inside a groom has crossed several fibres, each filtering it by the fibre colour:
    // the diffuse colour of a path-traced groom is more saturated than one fibre (multiple scattering). g_extra.z = k in
    // albedo x hue^k (0 = single fibre), fitted on the in-game frames (blond: game lit hair G/R .42, B/R .06)
    float3 baseD = base * MsSat(hue, g_extra.z);
    // sky / ground light enters from every side and leaves after fewer fibre crossings: less saturated (the in-game
    // shadowed side of a blond groom is olive, the sunlit side orange-gold)
    float3 baseA = base * MsSat(hue, g_extra.z * g_extra.w);

    // deep shadow through the hair volume. shadowSigma (cm) = the depth over which hair becomes shadowed; light hair
    // scatters light deeper into the groom (multiple scattering), more in its own colour: per channel the depth scale
    // grows with the fibre albedo, so the inside of a blond groom stays warm instead of going grey / black
    float3 alb = saturate(base * lerp(5.0f, 1.6f, enc));
    float sig = max(m.shadowSigma, 0.05f) * 0.01f * g_hair2.x;                        // cm -> m
    float3 sigC = sig * (1.0f + g_hair2.y * alb);

    float NdotL = dot(Nh, L);
    bool mapped = g_shadow.x > 0.0f;
    // fallback (no shadow map): self shadow from the depth inside the groom, facing away from the sun
    float depthIn = saturate((g_head.w + 0.035f - dist) / 0.035f) * 0.035f;
    float occ = exp(-depthIn / sig);
    float3 sunT;
    if (mapped) {
        // sun: the SDK's deep-shadow filter (NvHair_ShadowFilterDepth: gathered penetration depth behind the first
        // occluder, 16 texels) on our key-light map that holds the meshes and the hair (SDK shadow pass)
        float3 sc = Studio_ShadowCoord(a.P);
        float pen = 0.0f;
        if (all(sc.xy > 0.0f) && all(sc.xy < 1.0f) && sc.z < 1.0f)
            pen = NvHair_ShadowFilterDepth(g_shadowMap, g_shadowPoint, sc.xy, sc.z - g_shadow.y, -1.0f) * g_shadow2.x;
        sunT = lerp(1.0f, exp(-pen / sigC), g_shadow.x);
    } else {
        sunT = lerp(1.0f, occ * saturate(dot(fromHead / max(dist, 1e-4f), L) * 0.7f + 0.5f), g_params.w).xxx;
    }

    // diffuse, as the SDK's reference model: lerp(strand Kajiya term, Lambert on the hair-volume normal, diffuseBlend).
    // With the shadow map the far side of the head is shadowed for real, so only a soft terminator remains.
    float term = mapped ? saturate((NdotL + 0.6f) / 1.6f) : saturate((NdotL + 0.25f) / 1.25f);
    float diff = lerp(Strand_Diffuse(T, L) * term, saturate(NdotL), m.diffuseBlend);

    // sky / ground light: SH irradiance on the hair-volume normal x the sky visibility through the hair and the head,
    // from the sky-occlusion maps (the same deep-shadow transmittance per direction, weighted by sky radiance)
    int cnt = (int)(g_ao.x + 0.5f);
    float3 skyT = 1.0f;
    if (cnt > 0) {
        float3 num = 0.0f;
        float den = 0.0f;
        [loop] for (int k = 0; k < cnt; k++) {
            float3 d = g_aoDir[k].xyz;
            float td = dot(T, d);
            float w = g_aoDir[k].w * (0.4f + 0.6f * sqrt(saturate(1.0f - td * td))) * saturate(dot(Nh, d) * 0.5f + 0.5f);
            float4 c = mul(float4(a.P, 1.0f), g_aoVP[k]);
            float2 uv = float2(c.x * 0.5f + 0.5f, 0.5f - c.y * 0.5f);
            float3 t = 1.0f;
            if (all(uv > 0.0f) && all(uv < 1.0f) && c.z < 1.0f) {
                float4 o = g_aoMap.Gather(g_shadowPoint, float3(uv, k));
                float pen = dot(max(0.0f, c.z - 0.001f - o), 0.25f) * g_ao.z;
                t = exp(-pen / (sigC * 3.0f));   // diffuse sky light penetrates deeper than the collimated sun
            }
            num += w * t;
            den += w;
        }
        skyT = lerp(1.0f, num / max(den, 1e-5f), g_ao.y);
    } else {
        skyT = lerp(1.0f, lerp(0.45f, 1.0f, occ) * lerp(0.6f, 1.0f, s), g_params.w).xxx;
    }
    // fibre detail in shade: the smooth sky term alone renders shaded hair (moustache under the nose) as flat felt.
    // Neighbour fibres hide the sky towards each root, and every strand sees a slightly different sky.
    float hs = Strand_Var(a.hairID * 0.613f + 0.71f);
    float fib = lerp(1.0f - g_hair3.y, 1.0f, sqrt(saturate(s))) * max(1.0f + hs * 2.0f * g_hair3.z, 0.0f);
    // strand direction: a fibre seen across reflects / scatters more sky towards the eye than one seen end-on, so
    // overlapping strands in shade separate (g_hair3.w = strength)
    float tv = dot(T, V);
    fib *= lerp(1.0f, 0.45f + 0.75f * sqrt(saturate(1.0f - tv * tv)), g_hair3.w);
    // only where the sun does not reach (sunlit hair keeps its calibrated look: the sun term already shows the fibres)
    float lit = saturate(dot(sunT, float3(0.2126f, 0.7152f, 0.0722f)) * saturate(NdotL + 0.3f) * 2.5f);
    fib = lerp(fib, 1.0f, lit);
    float3 amb = Studio_SH(Nh) * skyT * (0.5f + g_mat.x) * fib;

    // specular, as the SDK's reference model (Kajiya-Kay on T.H): primary lobe shifted per strand by the breakup noise,
    // secondary lobe shifted by specularSecondaryOffset and tinted by the hair colour
    float n = Strand_Var(a.hairID) + 0.5f;
    float3 H = normalize(V + L);
    float TdotH = clamp(dot(T, H), -1.0f, 1.0f);
    float t1 = clamp(TdotH + m.specularPrimaryBreakup * (n - 0.5f), -1.0f, 1.0f);
    float t2 = clamp(TdotH + m.specularSecondaryOffset, -1.0f, 1.0f);
    float sp1 = m.specularPrimaryScale * pow(max(sqrt(saturate(1.0f - t1 * t1)), 1e-4f), max(m.specularPrimaryPower * g_shadow2.z, 1.0f));
    float sp2 = m.specularSecondaryScale * pow(max(sqrt(saturate(1.0f - t2 * t2)), 1e-4f), max(m.specularSecondaryPower * g_shadow2.z, 1.0f));
    float specVis = lerp(1.0f, saturate(NdotL), m.diffuseBlend) * term;
    float3 spec = (sp1 * m.specularColor.rgb + sp2 * m.specularColor.rgb * hue) * specVis * g_params.z;
    float glint = m.glintStrength > 0.0f ? NvHair_ComputeHairGlint(g_hair, m, a) * m.glintStrength : 0.0f;
    // sky sheen: the primary lobe lit by the sky around the hair-volume normal
    float3 envSpec = m.specularPrimaryScale * m.specularColor.rgb * g_params.z * g_hair2.w * Studio_Env(Nh) * skyT;
    // transmission (TT, Marschner): sun light passing through the fibre towards the camera. Longitudinal lobe: a
    // gaussian in sin(theta_i) + sin(theta_r) = T.(L + V) (same cone as the reflection); azimuthal lobe: light from
    // behind the strand (phi ~ pi), broad for light hair. Coloured by the fibre and scaled by how light
    // the hair is: white / blond hair glows when back-lit, dark hair barely transmits.
    float3 lp = L - T * dot(T, L), vp = V - T * dot(T, V);
    float cphi = dot(lp, vp) * rsqrt(max(dot(lp, lp) * dot(vp, vp), 1e-8f));
    float lon = dot(T, L + V);
    float back = exp(-lon * lon / (2.0f * 0.35f * 0.35f)) * pow(saturate(0.5f - 0.5f * cphi), 2.0f);
    float3 tt = g_hair2.z * back * hue * alb * alb;

    float3 sun = g_sunCol.rgb * g_sunDir.w;
    float3 c = baseD * sun * diff * sunT + baseA * amb + (spec + glint * base + tt) * sun * sunT + envSpec + g_mat.y * hue;
    float sunVis = dot(sunT, float3(0.2126f, 0.7152f, 0.0722f));

    float alpha = NvHair_ComputeAlpha(g_hair, m, a);
    int rootMode = (int)(g_pt.y + 0.5f);
    if (rootMode == 2) { clip(alpha - 0.5f); alpha = 1.0f; }
    // debug views (g_shadow2.y): 1 N.L diffuse term, 2 specular only, 3 shadow / visibility term, 4 albedo, 5 tangent
    int dbg = (int)(g_shadow2.y + 0.5f);
    if (dbg == 1) return float4(diff.xxx, alpha);
    if (dbg == 2) return float4(Studio_Tonemap((spec + glint * base + tt) * sun * sunT + envSpec), alpha);
    if (dbg == 3) return float4(sunVis.xxx, alpha);
    if (dbg == 4) return float4(base, alpha);
    if (dbg == 5) return float4(T * 0.5f + 0.5f, alpha);
    if (dbg == 6) return float4(m.specularPrimaryScale * 5.0f, m.specularPrimaryPower / 1000.0f, m.diffuseBlend, alpha);
    if (dbg == 7) return float4((sp1 / max(m.specularPrimaryScale, 1e-4f)).xxx, alpha);
    if (dbg == 9) {   // cm of occluder in front of this pixel along the light (r), shadow-map uv validity (g)
        float3 sc = Studio_ShadowCoord(a.P);
        float occD = g_shadowMap.SampleLevel(g_shadowPoint, sc.xy, 0).r;
        return float4(saturate((sc.z - occD) * g_shadow2.x * 100.0f / 10.0f), saturate(sc.z), saturate(occD), alpha);
    }
    if (dbg == 10) return float4(sunT, alpha);
    if (dbg == 11) return float4(skyT, alpha);
    if (dbg == 12) return float4(sun * diff * sunT / 4.0f, alpha);
    if (dbg == 13) return float4(amb, alpha);
    if (dbg == 14) return float4(base * 10.0f, alpha);
    if (dbg == 15) return float4(c * 2.0f, alpha);
    if (dbg == 16) return float4(abs(dot(T, V)).xxx, alpha);
    if (dbg == 17) return float4(abs(dot(T, H)).xxx, alpha);
    if (dbg == 18) return float4(normalize(a.N) * 0.5f + 0.5f, alpha);
    if (dbg == 19) return float4(Studio_Tonemap(baseA * amb), alpha);
    if (dbg == 20) return float4(Studio_Tonemap(baseD * sun * diff * sunT), alpha);
    if (dbg == 8) return float4(m.specularSecondaryScale * 5.0f, m.specularSecondaryPower / 100.0f, m.specularSecondaryOffset + 0.5f, alpha);
    return float4(Studio_Tonemap(c), alpha);
}
