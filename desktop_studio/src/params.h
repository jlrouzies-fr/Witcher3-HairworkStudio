// Desktop Groom Studio - Look panel parameters, redfur -> HairWorks instance mapping, performance budget.
// Our own code (MIT).
#pragma once
#include <map>
#include <string>
#include <vector>

#include "deskpack.h"

enum class PKind { Float, Color, Bool, Curve };

struct ParamDef {
    const char* path;    // redfur typed path, e.g. physicalMaterials.volume.density (same as redfur.py look.set keys)
    const char* label;
    const char* group;
    PKind kind;
    float lo, hi;
};

const std::vector<ParamDef>& paramDefs();
// HairWorks default of a value the fur does not store (what the runtime uses, so the panel shows the real value)
float paramDefault(const std::string& path);

struct Budget {
    double hairs = 0, hairsClose = 0, cost = 0;
    double refHairs = 0, refHairsClose = 0, refCost = 0;
    double ratio = 0;          // max(hairsClose / refHairsClose, cost / refCost)
    bool overLimit = false;
    std::string refName, msg;
};

struct Look {
    const DeskPack* pack = nullptr;
    std::map<std::string, json> over;   // user overrides (only changed values)
    float limit = 1.2f;                 // export limit vs the reference build

    void init(const DeskPack* p) { pack = p; over.clear(); }
    json base(const std::string& path) const;          // value stored in the character's build (null = absent)
    json get(const std::string& path) const;           // override or base
    float f(const std::string& path, float def) const;
    void setF(const std::string& path, float v);
    void setColor(const std::string& path, int r, int g, int b);
    void setJson(const std::string& path, const json& v);
    void reset(const std::string& path) { over.erase(path); }
    // hard caps (performance): returns the max allowed value for the path, or +inf
    float cap(const std::string& path) const;
    void enforceCaps();
    json exportLook() const;      // {"set": {path: [type, value]}} - only values that differ from the build
    Budget budget(int numFaces, double sumFaceDensity, int K) const;
};

std::string fmtCount(double n);
