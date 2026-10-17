// Desktop Groom Studio - head / body motion presets driving the fur bones. Our own code (MIT).
#pragma once
#include <DirectXMath.h>

#include <vector>

#include "deskpack.h"

enum class MotionPreset { Rest, Idle, TurnHead, Nod, Walk, Run, SpinBody, Wind, Count };
const char* motionName(MotionPreset p);

struct Motion {
    MotionPreset preset = MotionPreset::Idle;
    float speed = 1.0f;     // time scale
    float amount = 1.0f;    // amplitude scale
    float windStrength = 1.0f;
    float windYawDeg = 206.565f;   // wind heading, degrees clockwise from +y (default = front-left towards the back)
    float windPitchDeg = 2.85f;    // -45..45
    bool windOn = false;      // wind on top of any preset (toolbar)
    double t = 0;

    // per-bone world-space transforms (row-vector, world_now = world_bind * M); = HairWorks skinning matrices
    // body (optional): the pack whose neck / torso bones give the pivots (Beard mode: the hair pack's skeleton, the fur
    // bones are the live fur's)
    void boneMatrices(const DeskPack& pk, std::vector<DirectX::XMFLOAT4X4>& out, const DeskPack* body = nullptr) const;
    // transform of a mesh point given a "head weight" (0 = body, 1 = head)
    DirectX::XMMATRIX headM(const DeskPack& pk) const;
    DirectX::XMMATRIX neckM(const DeskPack& pk) const;
    DirectX::XMMATRIX rootM(const DeskPack& pk) const;
    V3 wind() const;        // wind vector (m/s-ish), before the fur's windScaler
};
