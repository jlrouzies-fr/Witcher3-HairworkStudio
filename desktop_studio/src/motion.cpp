// Desktop Groom Studio - motion presets. Our own code (MIT).
// World is the game's mesh space: metres, Z up, the character faces +Y (hair roots reach y -0.11 at the back, +0.08 at the hairline).
#include "motion.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace DirectX;

static const float PI = 3.14159265f;

const char* motionName(MotionPreset p) {
    switch (p) {
    case MotionPreset::Rest: return "Rest (no motion)";
    case MotionPreset::Idle: return "Idle (breathing)";
    case MotionPreset::TurnHead: return "Turn head";
    case MotionPreset::Nod: return "Nod / look up-down";
    case MotionPreset::Walk: return "Walk bob";
    case MotionPreset::Run: return "Run";
    case MotionPreset::SpinBody: return "Spin body (fast turn)";
    case MotionPreset::Wind: return "Wind (standing)";
    case MotionPreset::SprintStop: return "Sprint-stop (6.5 m/s, dead stop)";
    case MotionPreset::Dodge: return "Dodge roll (1.5 m sideways)";
    case MotionPreset::SwordSwings: return "Sword swings (110 deg torso)";
    default: return "?";
    }
}

static std::string alnum(const std::string& s) {
    std::string o;
    for (char ch : s) if (std::isalnum((unsigned char)ch)) o += (char)std::tolower((unsigned char)ch);
    return o;
}

MotionPreset motionFromName(const std::string& name, MotionPreset fallback) {
    std::string b = alnum(name);
    if (b.empty()) return fallback;
    for (int i = 0; i < (int)MotionPreset::Count; i++)
        if (alnum(motionName(MotionPreset(i))).rfind(b, 0) == 0) return MotionPreset(i);
    return fallback;
}

static float ease(float x) {           // smooth 0..1..0 cycle with holds (x in [0,1))
    float s = std::sin(2 * PI * x);
    return std::copysign(std::pow(std::fabs(s), 0.6f), s);
}

static float sstep(float u) { u = std::clamp(u, 0.0f, 1.0f); return u * u * (3 - 2 * u); }
static float sstepInt(float u) { u = std::clamp(u, 0.0f, 1.0f); return u * u * u - 0.5f * u * u * u * u; }   // integral of sstep from 0

// Sprint-stop: legs of 2.5 s, each = accelerate (0.5 s) to vmax, run (0.7 s), dead stop (0.25 s), stand (0.45 s), turn
// 180 deg (0.35 s), stand (0.25 s); the next leg sprints back, so the root stays within ~6 m. vmax = 6.5 m/s * amount
// (Geralt's sprint). Peak deceleration ~39 m/s2 (the game's stop animation halts the root in ~0.2 s).
struct SprintState { float dist, vel, dec, yaw; };
static SprintState sprintAt(float T, float A) {
    const float Ta = 0.5f, Tr = 0.7f, Td = 0.25f, Th = 0.45f, Tt = 0.35f, L = 2.5f, v = 6.5f * A;
    const float D = v * (0.5f * Ta + Tr + 0.5f * Td);
    int k = (int)std::floor(T / L);
    float tl = T - k * L, d, vel = 0, dec = 0;
    if (tl < Ta) { d = v * Ta * sstepInt(tl / Ta); vel = sstep(tl / Ta); }
    else if (tl < Ta + Tr) { d = v * (0.5f * Ta + tl - Ta); vel = 1; }
    else if (tl < Ta + Tr + Td) {
        float u = (tl - Ta - Tr) / Td;
        d = v * (0.5f * Ta + Tr) + v * Td * (u - sstepInt(u)); vel = 1 - sstep(u); dec = std::sin(PI * u);
    } else d = D;
    float yaw = (k & 1) * PI;
    if (tl >= Ta + Tr + Td + Th) yaw += PI * sstep((tl - Ta - Tr - Td - Th) / Tt);
    return {(k & 1) ? D - d : d, vel, dec, yaw};
}

// Dodge: legs of 1.1 s: 1.5 m * amount sideways in 0.4 s (peak ~5.6 m/s) with a crouch, a tilt into the move, a forward
// tuck and a body yaw, then 0.7 s standing; the next leg dodges back
struct DodgeState { float x, u, dir; };
static DodgeState dodgeAt(float T, float A) {
    const float Tm = 0.4f, L = 1.1f, W = 1.5f * A;
    int k = (int)std::floor(T / L);
    float tl = T - k * L, u = std::min(tl / Tm, 1.0f), s = sstep(u);
    float dir = (k & 1) ? -1.0f : 1.0f;
    return {(k & 1) ? W * (1 - s) : W * s, u < 1 ? u : 1.0f, dir};
}

// Sword swings: torso yaw between -110 and +110 deg * amount in 0.3 s (peak ~19 rad/s), 0.25 s holds
static float swordYaw(float T, float A, float* swing = nullptr) {
    const float Y = 1.92f * A, Ts = 0.3f, Th = 0.25f, H = Ts + Th;
    float c = std::fmod(T, 2 * H);
    float y, sw = 0;
    if (c < Ts) { y = -Y + 2 * Y * sstep(c / Ts); sw = std::sin(PI * c / Ts); }
    else if (c < H) y = Y;
    else if (c < H + Ts) { y = Y - 2 * Y * sstep((c - H) / Ts); sw = -std::sin(PI * (c - H) / Ts); }
    else y = -Y;
    if (swing) *swing = sw;
    return y;
}

static XMVECTOR bonePos(const DeskPack& pk, const char* name, XMVECTOR def) {
    int b = pk.boneIndex(name);
    if (b < 0) return def;
    const float* m = pk.bones[b].bind;
    return XMVectorSet(m[12], m[13], m[14], 1);
}

static XMMATRIX aboutPivot(XMVECTOR pivot, XMMATRIX R) {
    return XMMatrixTranslationFromVector(XMVectorNegate(pivot)) * R * XMMatrixTranslationFromVector(pivot);
}

XMMATRIX Motion::rootM(const DeskPack& pk) const {
    float T = float(t) * speed, A = amount;
    XMVECTOR hip = bonePos(pk, "torso3", XMVectorSet(0, 0, 1.4f, 1));
    hip = XMVectorSetZ(hip, 1.0f);
    switch (preset) {
    case MotionPreset::Idle: {
        float z = 0.003f * A * std::sin(2 * PI * 0.25f * T);
        return XMMatrixTranslation(0, 0, z);
    }
    case MotionPreset::Walk: {
        float f = 1.8f;  // steps per second
        float z = 0.025f * A * std::fabs(std::sin(PI * f * T)) - 0.0125f * A;
        float x = 0.015f * A * std::sin(PI * f * T);
        float yaw = 0.05f * A * std::sin(PI * f * T);
        return aboutPivot(hip, XMMatrixRotationZ(yaw)) * XMMatrixTranslation(x, 0, z);
    }
    case MotionPreset::Run: {
        float f = 2.8f;
        float z = 0.06f * A * std::fabs(std::sin(PI * f * T)) - 0.03f * A;
        float x = 0.025f * A * std::sin(PI * f * T);
        float lean = 0.17f * A;   // forward lean (towards +Y)
        float yaw = 0.08f * A * std::sin(PI * f * T);
        return aboutPivot(hip, XMMatrixRotationX(-lean) * XMMatrixRotationZ(yaw)) * XMMatrixTranslation(x, 0, z);
    }
    case MotionPreset::SpinBody: {
        // quick 90 degree turns left / right with holds (sword fight / dodge)
        float c = std::fmod(T / 2.4f, 1.0f);
        float yaw = 1.57f * A * ease(c);
        return aboutPivot(hip, XMMatrixRotationZ(yaw));
    }
    case MotionPreset::SprintStop: {
        SprintState s = sprintAt(T, std::max(A, 0.0f));
        float f = 3.2f;   // sprint strides per second
        float z = s.vel * (0.06f * std::fabs(std::sin(PI * f * T)) - 0.03f);
        float lean = 0.25f * s.vel - 0.3f * s.dec;   // forward while running, back while braking
        float roll = 0.05f * s.vel * std::sin(PI * f * T);
        return aboutPivot(hip, XMMatrixRotationY(roll) * XMMatrixRotationX(-lean) * XMMatrixRotationZ(s.yaw)) * XMMatrixTranslation(0, s.dist, z);
    }
    case MotionPreset::Dodge: {
        DodgeState s = dodgeAt(T, std::max(A, 0.0f));
        float b = std::sin(PI * s.u);   // 0 -> 1 -> 0 over the move
        float z = -0.35f * std::min(A, 1.5f) * b;
        return aboutPivot(hip, XMMatrixRotationX(-0.5f * b) * XMMatrixRotationY(0.45f * s.dir * b) * XMMatrixRotationZ(-0.35f * s.dir * b)) *
               XMMatrixTranslation(s.x, 0, z);
    }
    case MotionPreset::SwordSwings: {
        float sw = 0, yaw = swordYaw(T, A, &sw);
        float lunge = 0.12f * std::fabs(sw);
        return aboutPivot(hip, XMMatrixRotationX(-lunge) * XMMatrixRotationZ(yaw)) * XMMatrixTranslation(0, 0, -0.04f * std::fabs(sw));
    }
    default:
        return XMMatrixIdentity();
    }
}

static XMMATRIX headRot(MotionPreset p, float T, float A) {
    switch (p) {
    case MotionPreset::Idle: return XMMatrixRotationZ(0.06f * A * std::sin(2 * PI * 0.13f * T)) * XMMatrixRotationX(0.03f * A * std::sin(2 * PI * 0.21f * T));
    case MotionPreset::TurnHead: return XMMatrixRotationZ(1.0f * A * ease(std::fmod(T / 3.0f, 1.0f)));
    case MotionPreset::Nod: return XMMatrixRotationX(0.45f * A * ease(std::fmod(T / 2.5f, 1.0f)));
    case MotionPreset::Walk: return XMMatrixRotationX(0.04f * A * std::sin(2 * PI * 1.8f * T));
    case MotionPreset::Run: return XMMatrixRotationX(0.12f * A + 0.06f * A * std::sin(2 * PI * 2.8f * T));
    case MotionPreset::SprintStop: {   // head pitches with the stride, nods forward on the stop
        SprintState s = sprintAt(T, std::max(A, 0.0f));
        return XMMatrixRotationX(A * (0.08f * s.vel * std::sin(2 * PI * 3.2f * T) + 0.25f * s.dec));
    }
    case MotionPreset::Dodge: {        // head tucks with the roll
        DodgeState s = dodgeAt(T, std::max(A, 0.0f));
        return XMMatrixRotationX(0.3f * A * std::sin(PI * s.u));
    }
    case MotionPreset::SwordSwings: {  // head follows the torso, lagging a little during the swing
        float sw = 0;
        swordYaw(T, A, &sw);
        return XMMatrixRotationZ(-0.25f * A * sw);
    }
    default: return XMMatrixIdentity();
    }
}

XMMATRIX Motion::neckM(const DeskPack& pk) const {
    XMVECTOR neck = bonePos(pk, "neck", XMVectorSet(0, -0.05f, 1.6f, 1));
    XMMATRIX R = headRot(preset, float(t) * speed, amount * 0.5f);
    return aboutPivot(neck, R) * rootM(pk);
}

XMMATRIX Motion::headM(const DeskPack& pk) const {
    XMVECTOR neck = bonePos(pk, "neck", XMVectorSet(0, -0.05f, 1.6f, 1));
    XMMATRIX R = headRot(preset, float(t) * speed, amount);
    return aboutPivot(neck, R) * rootM(pk);
}

void Motion::boneMatrices(const DeskPack& pk, std::vector<XMFLOAT4X4>& out, const DeskPack* body) const {
    out.resize(pk.bones.size());
    const DeskPack& bp = body ? *body : pk;
    XMMATRIX H = headM(bp), Nk = neckM(bp), Rt = rootM(bp);
    for (size_t i = 0; i < pk.bones.size(); i++) {
        const std::string& n = pk.bones[i].name;
        XMMATRIX M = Rt;
        if (n == "head" || n == "hroll" || n.rfind("hair", 0) == 0 || n.find("jaw") != std::string::npos ||
            n.find("ear") != std::string::npos || n.find("eye") != std::string::npos)
            M = H;
        else if (n == "neck" || n == "neck2")
            M = Nk;
        XMStoreFloat4x4(&out[i], M);
    }
}

V3 Motion::travel() const {
    float T = float(t) * speed, A = std::max(amount, 0.0f);
    if (preset == MotionPreset::SprintStop) return {0, sprintAt(T, A).dist, 0};
    if (preset == MotionPreset::Dodge) return {dodgeAt(T, A).x, 0, 0};
    return {0, 0, 0};
}

V3 Motion::wind() const {
    if (preset != MotionPreset::Wind && !windOn) return {0, 0, 0};
    float T = float(t) * speed;
    float gust = 0.6f + 0.4f * std::sin(2 * PI * 0.17f * T) * std::sin(2 * PI * 0.41f * T + 1.0f);
    float s = windStrength * amount * gust;
    // default yaw / pitch = the old fixed {-0.45, -0.9, 0.05}: from the character's front-left towards the back
    const float mag = 1.00623f, yaw = windYawDeg * PI / 180.0f, pit = windPitchDeg * PI / 180.0f;
    float h = mag * std::cos(pit);
    return {h * std::sin(yaw) * s, h * std::cos(yaw) * s, mag * std::sin(pit) * s};
}
