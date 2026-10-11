// Desktop Groom Studio - physically based sun + sky lighting. Our own code (MIT).
//
// Units: every light value is irradiance / pi, so a white Lambert surface facing the sun shows `sun`, and the same
// surface under a full uniform sky of radiance `sky` shows `sky`. The sky is a procedural model (zenith colour, a
// horizon band for trees / haze, a circumsolar glow) over a ground whose radiance is its albedo x (sun + sky light), so
// the bounce light from below follows the sun. It is projected to SH9 irradiance every frame (cheap: 48 x 96 samples)
// and sampled along 8 / 16 directions for the sky-occlusion maps (app.cpp drawScene). No image assets.
//
// Index 0 is the HairLab capture rig, fitted to the in-game HairLab frames (L244 Guillaume, L232 Vesemir; README
// "Rendering"). The other presets use the same model and units, so they stay consistent with it.
#include "lighting.h"

#include <algorithm>
#include <cmath>

#include "app.h"

static const TimeOfDay kPresets[] = {
    // name, tip, az, el, sun, sunCol, sky, skyCol, horizon, horizonCol, ground, groundCol, exposure, skyTop, skyBottom,
    // shadowSoft, shadowStrength, sunSize, moon
    {"HairLab capture rig", "Matches the in-game HairLab captures (clear day at the capture spot: sun behind the character, open sky, trees and a dirt road around): use it for comparisons (default).",
     -2.8f, 0.80f, 2.2f, {1.0f, 0.96f, 0.90f}, 1.9f, {0.60f, 0.72f, 0.90f}, 1.1f, {0.28f, 0.46f, 0.16f}, 0.7f, {0.56f, 0.47f, 0.34f}, 1.45f,
     {0.11f, 0.12f, 0.13f}, {0.06f, 0.065f, 0.07f}, 1.0f, 1.0f, 0.6f, false},
    {"Noon sun", "High warm-white sun in front, blue sky, crisp shadows.",
     0.4f, 1.20f, 2.6f, {1.0f, 0.96f, 0.90f}, 1.3f, {0.45f, 0.60f, 0.90f}, 0.9f, {0.70f, 0.76f, 0.84f}, 0.5f, {0.55f, 0.50f, 0.42f}, 1.2f,
     {0.42f, 0.58f, 0.80f}, {0.78f, 0.82f, 0.86f}, 1.0f, 1.0f, 0.5f, false},
    {"Morning", "Low-ish soft gold sun from the side, cool sky.",
     1.2f, 0.42f, 2.3f, {1.0f, 0.84f, 0.64f}, 1.1f, {0.46f, 0.58f, 0.86f}, 0.8f, {0.80f, 0.74f, 0.66f}, 0.5f, {0.55f, 0.50f, 0.42f}, 1.25f,
     {0.52f, 0.62f, 0.78f}, {0.92f, 0.82f, 0.68f}, 1.2f, 1.0f, 0.6f, false},
    {"Dawn", "Sun just above the horizon, orange key, deep blue sky, long shadows.",
     1.6f, 0.07f, 1.7f, {1.0f, 0.52f, 0.24f}, 0.6f, {0.30f, 0.36f, 0.66f}, 0.7f, {0.88f, 0.58f, 0.42f}, 0.4f, {0.50f, 0.44f, 0.38f}, 1.6f,
     {0.16f, 0.20f, 0.38f}, {0.86f, 0.52f, 0.34f}, 1.4f, 1.0f, 0.8f, false},
    {"Dusk", "Low golden sun from behind, purple-blue sky.",
     -1.25f, 0.14f, 1.9f, {1.0f, 0.60f, 0.30f}, 0.6f, {0.38f, 0.36f, 0.66f}, 0.7f, {0.86f, 0.52f, 0.40f}, 0.4f, {0.50f, 0.42f, 0.36f}, 1.5f,
     {0.20f, 0.18f, 0.36f}, {0.86f, 0.50f, 0.30f}, 1.4f, 1.0f, 0.8f, false},
    {"Overcast", "Soft white sky light from everywhere, no hard shadow.",
     0.5f, 1.0f, 0.3f, {0.92f, 0.94f, 0.98f}, 2.0f, {0.82f, 0.84f, 0.88f}, 1.5f, {0.74f, 0.76f, 0.78f}, 0.5f, {0.50f, 0.48f, 0.44f}, 1.3f,
     {0.62f, 0.64f, 0.68f}, {0.46f, 0.47f, 0.48f}, 4.0f, 0.35f, 6.0f, false},
    {"Moonlight", "Cool blue moon key, dark blue sky.",
     1.0f, 0.75f, 0.35f, {0.62f, 0.72f, 1.0f}, 0.12f, {0.22f, 0.30f, 0.55f}, 0.06f, {0.20f, 0.24f, 0.34f}, 0.3f, {0.40f, 0.40f, 0.42f}, 4.0f,
     {0.03f, 0.05f, 0.10f}, {0.01f, 0.015f, 0.03f}, 1.6f, 1.0f, 0.6f, true},
    {"Torch / interior", "Warm close light from the side (torch), very low cool ambient.",
     -1.1f, 0.30f, 2.8f, {1.0f, 0.58f, 0.26f}, 0.06f, {0.30f, 0.28f, 0.34f}, 0.05f, {0.26f, 0.20f, 0.16f}, 0.3f, {0.50f, 0.36f, 0.26f}, 1.4f,
     {0.07f, 0.05f, 0.04f}, {0.02f, 0.015f, 0.012f}, 1.2f, 1.0f, 3.0f, false},
    {"Night", "Moonless night: faint blue starlight from the sky only.",
     1.0f, 0.75f, 0.0f, {0.62f, 0.72f, 1.0f}, 0.10f, {0.20f, 0.28f, 0.50f}, 0.05f, {0.16f, 0.18f, 0.26f}, 0.3f, {0.40f, 0.40f, 0.42f}, 5.0f,
     {0.015f, 0.025f, 0.05f}, {0.005f, 0.008f, 0.016f}, 2.0f, 0.0f, 1.0f, false},
};

int timeOfDayCount() { return int(sizeof(kPresets) / sizeof(kPresets[0])); }
const TimeOfDay& timeOfDay(int i) { return kPresets[(i < 0 || i >= timeOfDayCount()) ? 0 : i]; }

void applyTimeOfDay(Rig& r, int i) {
    const TimeOfDay& t = timeOfDay(i);
    r.tod = i;
    r.sunAz = t.az;
    r.sunEl = t.el;
    r.sun = t.sun;
    r.sky = t.sky;
    r.horizon = t.horizon;
    r.ground = t.ground;
    r.exposure = t.exposure;
    for (int c = 0; c < 3; c++) {
        r.sunCol[c] = t.sunCol[c];
        r.skyCol[c] = t.skyCol[c];
        r.horizonCol[c] = t.horizonCol[c];
        r.groundCol[c] = t.groundCol[c];
        r.skyTop[c] = t.skyTop[c];
        r.skyBottom[c] = t.skyBottom[c];
    }
    r.shadowSoft = t.shadowSoft;
    r.shadowStrength = t.shadowStrength;
    r.sunSize = t.sunSize;
    r.moon = t.moon;
}

static void sunDir(const Rig& r, float L[3]) {
    float ce = std::cos(r.sunEl);
    L[0] = ce * std::sin(r.sunAz);
    L[1] = ce * std::cos(r.sunAz);
    L[2] = std::sin(r.sunEl);
}

static void upperSky(const Rig& r, const float d[3], const float L[3], float out[3]) {
    float z = std::max(d[2], 0.0f);
    float t = std::sqrt(std::min(z / 0.55f, 1.0f));                     // horizon band -> zenith
    float mu = std::max(0.0f, d[0] * L[0] + d[1] * L[1] + d[2] * L[2]);
    float glow = 0.12f * std::pow(mu, 12.0f) * r.sun * std::max(L[2], 0.0f) * 0.25f;   // circumsolar (small share of the sun)
    for (int c = 0; c < 3; c++)
        out[c] = (r.horizonCol[c] * r.horizon) * (1 - t) + (r.skyCol[c] * r.sky) * t + glow * r.sunCol[c];
}

void skyRadiance(const Rig& r, const float d[3], float out[3]) {
    float L[3];
    sunDir(r, L);
    if (d[2] >= 0.0f) { upperSky(r, d, L, out); return; }
    // ground: albedo x (sun on the ground + sky light) in E/pi units; the near-horizon band fades into the horizon colour
    float skyAvg[3] = {0, 0, 0};
    for (int c = 0; c < 3; c++) skyAvg[c] = 0.5f * (r.horizonCol[c] * r.horizon + r.skyCol[c] * r.sky);
    float sunG = r.sun * std::max(L[2], 0.0f);
    float b = std::min(-d[2] / 0.12f, 1.0f);
    for (int c = 0; c < 3; c++) {
        float g = r.groundCol[c] * r.ground * (sunG * r.sunCol[c] + skyAvg[c]);
        out[c] = g * b + (r.horizonCol[c] * r.horizon) * (1 - b);
    }
}

void aoDirection(int k, int n, float out[3]) {
    int nUp = n >= 16 ? 12 : (n * 3) / 4, nDn = n - nUp;
    const float ga = 2.39996323f;   // golden angle
    if (k < nUp) {
        float z = (k + 0.5f) / nUp;                    // equal-area cells over the upper hemisphere
        float rr = std::sqrt(std::max(0.0f, 1 - z * z)), ph = k * ga;
        out[0] = rr * std::cos(ph); out[1] = rr * std::sin(ph); out[2] = z;
    } else {
        int j = k - nUp;
        float z = -0.5f, rr = std::sqrt(1 - z * z), ph = 6.2831853f * (j + 0.5f) / std::max(nDn, 1) + 0.3f;
        out[0] = rr * std::cos(ph); out[1] = rr * std::sin(ph); out[2] = z;
    }
}

void fillEnvironment(const Rig& r, LightCB& Lc, int nAo) {
    // SH9 projection of the sky + ground radiance, convolved with the cosine lobe (irradiance / pi)
    double c[9][3] = {};
    const int NT = 48, NP = 96;
    const double PI = 3.14159265358979;
    for (int i = 0; i < NT; i++) {
        double th = (i + 0.5) * PI / NT, st = std::sin(th), ct = std::cos(th);
        double dw = (PI / NT) * (2 * PI / NP) * st;
        for (int j = 0; j < NP; j++) {
            double ph = (j + 0.5) * 2 * PI / NP;
            float d[3] = {float(st * std::cos(ph)), float(st * std::sin(ph)), float(ct)}, Lr[3];
            skyRadiance(r, d, Lr);
            double x = d[0], y = d[1], z = d[2];
            double Y[9] = {0.282095, 0.488603 * y, 0.488603 * z, 0.488603 * x, 1.092548 * x * y, 1.092548 * y * z,
                           0.315392 * (3 * z * z - 1), 1.092548 * x * z, 0.546274 * (x * x - y * y)};
            for (int k = 0; k < 9; k++)
                for (int ch = 0; ch < 3; ch++) c[k][ch] += Lr[ch] * Y[k] * dw;
        }
    }
    const double A[9] = {1.0, 2.0 / 3, 2.0 / 3, 2.0 / 3, 0.25, 0.25, 0.25, 0.25, 0.25};   // A_l / pi
    for (int k = 0; k < 9; k++) Lc.sh[k] = {float(c[k][0] * A[k]), float(c[k][1] * A[k]), float(c[k][2] * A[k]), 0};
    float up[3] = {0, 0, 1}, hz[3] = {0.7071f, 0, 0.05f}, dn[3] = {0, 0, -1}, z[3], h[3], g[3];
    skyRadiance(r, up, z);
    skyRadiance(r, hz, h);
    skyRadiance(r, dn, g);
    Lc.skyZen = {z[0], z[1], z[2], r.meshEnvSpec};
    Lc.skyHor = {h[0], h[1], h[2], 0};
    Lc.groundRad = {g[0], g[1], g[2], 1.0f};
    // sky-occlusion directions: weight = radiance luminance x the cell's solid angle
    nAo = std::min(nAo, kMaxAoDirs);
    int nUp = nAo >= 16 ? 12 : (nAo * 3) / 4, nDn = nAo - nUp;
    for (int k = 0; k < kMaxAoDirs; k++) {
        if (k >= nAo) { Lc.aoDir[k] = {0, 0, 1, 0}; continue; }
        float d[3], Lr[3];
        aoDirection(k, nAo, d);
        skyRadiance(r, d, Lr);
        float lum = 0.2126f * Lr[0] + 0.7152f * Lr[1] + 0.0722f * Lr[2];
        float dw = k < nUp ? 6.2832f / nUp : 6.2832f / std::max(nDn, 1);
        Lc.aoDir[k] = {d[0], d[1], d[2], std::max(lum * dw, 1e-4f)};
    }
}
