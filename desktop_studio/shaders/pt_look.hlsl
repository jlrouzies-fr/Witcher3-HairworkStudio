// "Path-traced look" composite of the game-accurate preview (2026-10-16, our own code).
// The game draws HairWorks hair path-traced: one closest hit per pixel and sample, many samples and frames accumulated, then
// denoised. Its hair reads as a soft veil whose opacity is the hair's pixel coverage, the skin under thin hair shows through,
// and light passes through the fibres. Here the hair is drawn alone into a premultiplied (colour x coverage, coverage)
// target with the meshes' depth, then composited over the scene without hair:
//   veil   = gaussian of the hair layer, sigma g_p.x px at 1400 px image height (denoiser footprint)
//   hair   = lerp(veil, sharp, g_p.w)                        (how much single strands still resolve)
//   a      = saturate(1 - pow(1 - hair.a, g_p.y))            (opacity: g_p.y = equivalent layers per covered pixel)
//   out    = lerp(scene, hairColour, a) + scene * a * g_p.z * tint   (transmission of the scene through the hair)
//   tint   = hair colour normalised to its brightest channel, then lerp(1, tint, g_q.x)
Texture2D<float4> g_scene : register(t0);
Texture2D<float4> g_hair : register(t1);
Texture2D<float> g_layers : register(t2);   // strand fragments per sample (mean over the MSAA samples)
cbuffer PtLook : register(b0) { float4 g_p; float4 g_q; };
struct VOut { float4 pos : SV_Position; };
VOut vs_full(uint id : SV_VertexID) {
    VOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_pt_look(VOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    uint W, H;
    g_hair.GetDimensions(W, H);
    float s = max(g_p.x * H / 1400.0f, 0.3f);
    int R = (int)ceil(2.5f * s);
    int st = max(1, R / 6);
    float4 acc = 0; float ws = 0;
    for (int y = -R; y <= R; y += st)
        for (int x = -R; x <= R; x += st) {
            int2 q = clamp(p + int2(x, y), int2(0, 0), int2(W - 1, H - 1));
            float w = exp(-(x * x + y * y) / (2.0f * s * s));
            acc += w * g_hair.Load(int3(q, 0));
            ws += w;
        }
    float4 hb = acc / ws, hs = g_hair.Load(int3(p, 0));
    float4 h = lerp(hb, hs, saturate(g_p.w));
    float cov = saturate(h.a);
    float a = 1.0f - pow(saturate(1.0f - cov), max(g_p.y, 0.01f));
    // layered hair (g_q.z): the path tracer's many samples per pixel see through ONE thin layer between the strands but
    // hardly ever through several stacked layers; the over-blended coverage counts each covered sample once. Every layer
    // beyond the first (mean fragments per sample minus the covered share, blurred like the veil) closes the gaps further.
    if (g_q.z > 0.0f) {
        float lb = 0.0f, lw = 0.0f;
        for (int y2 = -R; y2 <= R; y2 += st)
            for (int x2 = -R; x2 <= R; x2 += st) {
                int2 q = clamp(p + int2(x2, y2), int2(0, 0), int2(W - 1, H - 1));
                float w = exp(-(x2 * x2 + y2 * y2) / (2.0f * s * s));
                lb += w * g_layers.Load(int3(q, 0)); lw += w;
            }
        float extra = max(lb / lw - hb.a, 0.0f);
        a = 1.0f - (1.0f - a) * exp(-g_q.z * extra);
    }
    float3 col = h.rgb / max(h.a, 1e-4f);
    float3 tint = col / max(max(col.r, max(col.g, col.b)), 1e-3f);
    tint = lerp(float3(1, 1, 1), tint, saturate(g_q.x));
    float3 sc = g_scene.Load(int3(p, 0)).rgb;
    // transmission gated by the hair's own coverage: thin hair lets the skin right under it glow through, dense hair
    // (where meshes behind would ghost: ears, caps) hides it
    float gate = pow(saturate(1.0f - cov), max(g_q.y, 0.0f));
    return float4(lerp(sc, col, a) + sc * a * g_p.z * gate * tint, 1.0f);
}
