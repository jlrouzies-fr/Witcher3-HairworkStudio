// Desktop Groom Studio - head / body motion presets driving the fur bones. Our own code (MIT).
#pragma once
#include <DirectXMath.h>

#include <string>
#include <vector>

#include "deskpack.h"

// gameplay presets (2026-10-18, appended so saved projects keep their index): SprintStop, Dodge, SwordSwings move the
// root through the world like the player does (forward speed, dead stops, sideways dodges, fast torso turns)
enum class MotionPreset { Rest, Idle, TurnHead, Nod, Walk, Run, SpinBody, Wind, SprintStop, Dodge, SwordSwings, Count };
const char* motionName(MotionPreset p);
// preset from a name: case, spaces and punctuation ignored, prefix match ("spin", "SpinBody", "sprint", "dodge", "sword");
// fallback when nothing matches
MotionPreset motionFromName(const std::string& name, MotionPreset fallback);

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
    // world travel of the root (gameplay presets; 0 for the others): the viewport camera follows it
    V3 travel() const;
};
