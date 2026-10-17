// Desktop Groom Studio - motion presets. Our own code (MIT).
// World is the game's mesh space: metres, Z up, the character faces +Y (hair roots reach y -0.11 at the back, +0.08 at the hairline).
#include "motion.h"

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
    default: return "?";
    }
}

static float ease(float x) {           // smooth 0..1..0 cycle with holds (x in [0,1))
    float s = std::sin(2 * PI * x);
    return std::copysign(std::pow(std::fabs(s), 0.6f), s);
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
