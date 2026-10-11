// Desktop Groom Studio - strand material presets and the painted stiffness map. Our own code (MIT).
#pragma once
#include <string>
#include <vector>

#include "deskpack.h"
#include "groom.h"
#include "params.h"

// A material = every leaf value of physicalMaterials / graphicalMaterials (path -> value as stored in the redfur JSON).
struct MaterialPreset {
    std::string name, kind;   // kind: builtin | current | custom
    json values;              // {path: value}
    json configKeys;          // hairpipe config keys that produce it (look.mat_src, look.width, ...), may be empty
    std::string file;         // custom presets: their JSON file
};

bool isColourPath(const std::string& path);
json flattenMaterial(const json& physical, const json& graphical);
std::vector<MaterialPreset> loadPresets(const DeskPack& pk, const std::string& presetDir, const json& builtins);
// applies a preset as Look overrides; keepColours keeps the character's root / tip / spec colours and texture brightness
int applyPreset(Look& look, const MaterialPreset& p, bool keepColours);
bool saveCustomPreset(const Look& look, const std::string& name, const std::string& presetDir, std::string& err);

// Painted stiffness / root-stiffness map in growth-mesh UV space (the redfur's per-corner uvs).
// R = stiffness multiplier, G = root-stiffness multiplier (HairWorks multiplies the constant by the texel value).
struct StiffnessMap {
    int size = 512;
    std::vector<float> guideS, guideR;     // painted value per guide (index = groom guide index), default 1
    std::vector<unsigned char> rgba;       // rasterised texture
    int overlapTexels = 0;                 // texels written by faces that disagree (shared UVs in the card atlas)
    bool painted = false;
    void reset(size_t nGuides);
    void ensure(size_t nGuides);
    void rasterise(const Groom& g, const std::vector<std::array<float, 6>>* uvs = nullptr);
    bool uniform() const;
};

// Unique UV layout: the redfur's per-corner UVs come from the card atlas and many faces share texels, so a painted map
// cannot be told apart per region. When a stiffness map is painted, every growth face gets its own triangle in a grid
// atlas and the textures the fur samples through those UVs (density, colour) are re-baked into it, so preview and
// export stay identical to the original look while the stiffness map becomes per region.
struct UVAtlas {
    int size = 2048;
    std::vector<std::array<float, 6>> uv;      // per groom face (same order as Groom::faces)
    void build(const Groom& g);
    // bake a source image (RGBA, sampled through the ORIGINAL face uvs) into the atlas (RGBA); channel < 0 = all channels,
    // else that channel copied into R,G,B (density)
    void bake(const Groom& g, const unsigned char* src, int sw, int sh, int channel, std::vector<unsigned char>& out) const;
};
