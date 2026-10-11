// Desktop Groom Studio - physically based sun + sky lighting: time-of-day presets, procedural sky / ground environment
// (projected to SH9 irradiance on the CPU), sky-occlusion directions. Our own code (MIT).
#pragma once

struct Rig;
struct LightCB;

struct TimeOfDay {
    const char* name;
    const char* tip;
    float az, el, sun;              // sun azimuth / elevation (radians, character space: +Y = the face), intensity
    float sunCol[3];
    float sky, skyCol[3];           // zenith sky radiance (E/pi units: a white Lambert surface under a full uniform sky = sky)
    float horizon, horizonCol[3];   // horizon band radiance (trees / haze)
    float ground, groundCol[3];     // ground albedo (x colour): lit by the sun + sky, it is the bounce light from below
    float exposure;
    float skyTop[3], skyBottom[3];  // backdrop gradient (sRGB)
    float shadowSoft;               // minimum shadow filter radius in shadow-map texels (overcast = very soft)
    float shadowStrength;           // 0 = no visible shadow (overcast), 1 = full
    float sunSize;                  // light-source angular radius (degrees) for the contact-hardening sun penumbra
    bool moon;                      // draw a moon disc instead of the sun
};

int timeOfDayCount();
const TimeOfDay& timeOfDay(int i);
void applyTimeOfDay(Rig& rig, int i);

// sky radiance towards direction d (unit, z up) for the rig, in the same units as the sun term (no sun disc)
void skyRadiance(const Rig& r, const float d[3], float out[3]);
// fills the environment part of the light constants: SH9 irradiance (E/pi), sky / horizon / ground radiances for the
// reflection lookup, and the sky-occlusion directions + weights (n = number of occlusion maps, 0 = off)
void fillEnvironment(const Rig& r, LightCB& L, int nAoDirs);
// unit direction of sky-occlusion map k of n (12 upper-hemisphere + 4 lower for n = 16; 6 + 2 for n = 8)
void aoDirection(int k, int n, float out[3]);
