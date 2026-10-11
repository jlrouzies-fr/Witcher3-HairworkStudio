// Desktop Groom Studio - desk pack (desk.json written by desktop_studio/tools/desk_prep.py).
// Our own code (MIT). No NVIDIA code here.
#pragma once
#include <array>
#include <string>
#include <vector>

#include "../third_party/nlohmann/json.hpp"

using json = nlohmann::json;

struct V3 { float x = 0, y = 0, z = 0; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float len(V3 a);

using Guide = std::vector<V3>;

struct Bone {
    std::string name;
    float bind[16];   // bone -> world, row-vector convention (world = local * M), row major
    int parent = -1;
};

struct Sphere { int bone; V3 local; float r; };

struct Mesh {
    std::string name, kind, texFile, nrmFile, path;   // texFile / nrmFile: diffuse / normal PNG (game files)
    std::vector<std::array<float, 2>> uv;
    std::vector<V3> v;
    std::vector<std::array<int, 3>> t;
    bool alphaTest = false;   // vanilla beard / moustache cards: alpha-tested (texFile keeps the alpha)
    float alphaRef = 0.3f;
};

struct DeskPack {
    std::string dir, config, character, comp, furPath, editPath;
    int K = 0, splineMultiplier = 1;
    std::vector<Guide> guides;       // fur frame (what the redfur holds)
    std::vector<Guide> guidesEdit;   // edit frame (ops frame), same order
    std::vector<std::array<int, 3>> F;
    std::vector<std::array<float, 6>> uv;
    std::vector<std::array<float, 4>> boneIdx, boneW;
    std::vector<Bone> bones;
    std::vector<Sphere> spheres;
    std::vector<std::array<int, 2>> capsules;
    json physical, graphical, lod, mod, budgetRef, budgetSelf, textures, lookCfg, shots;
    // density
    std::string densityFile, densityChannel, colorFile;   // colorFile: rootColorTex (card diffuse), when the fur uses it
    bool densityPixel = false;
    std::vector<float> densityFace;
    std::vector<Mesh> meshes;
    V3 headC; float headTop = 0;

    json beardFur;                   // desk.json "beard_fur" of a beard config (second fur), null when absent

    bool load(const std::string& dir, std::string& err);
    int boneIndex(const std::string& n) const;
    // the beard fur as a pack of its own (guides, faces, uv, bones, skinning, materials; no collision), for a second
    // HairWorks instance. false when the pack has no beard fur.
    bool beardPack(DeskPack& out) const;
};
