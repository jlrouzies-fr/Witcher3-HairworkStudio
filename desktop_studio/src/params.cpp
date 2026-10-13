// Desktop Groom Studio - Look parameters and budget. Our own code (MIT).
#include "params.h"

#include <cmath>
#include <limits>
#include <sstream>

const std::vector<ParamDef>& paramDefs() {
    static const std::vector<ParamDef> d = {
        {"physicalMaterials.volume.density", "density", "Volume", PKind::Float, 0, 3},
        {"physicalMaterials.volume.lengthNoise", "length noise", "Volume", PKind::Float, 0, 1},
        {"physicalMaterials.volume.lengthScale", "length scale", "Volume", PKind::Float, 0, 1},
        {"physicalMaterials.strandWidth.width", "width (mm)", "Volume", PKind::Float, 0, 3},
        {"physicalMaterials.strandWidth.widthRootScale", "width root", "Volume", PKind::Float, 0, 2},
        {"physicalMaterials.strandWidth.widthTipScale", "width tip", "Volume", PKind::Float, 0, 2},
        {"physicalMaterials.strandWidth.widthNoise", "width noise", "Volume", PKind::Float, 0, 1},
        {"physicalMaterials.clumping.clumpScale", "clump scale", "Clump / wave", PKind::Float, 0, 1},
        {"physicalMaterials.clumping.clumpRoundness", "clump roundness", "Clump / wave", PKind::Float, 0, 3},
        {"physicalMaterials.clumping.clumpNoise", "clump noise", "Clump / wave", PKind::Float, 0, 1},
        {"physicalMaterials.waveness.waveScale", "wave scale (cm)", "Clump / wave", PKind::Float, 0, 2},
        {"physicalMaterials.waveness.waveScaleNoise", "wave scale noise", "Clump / wave", PKind::Float, 0, 1},
        {"physicalMaterials.waveness.waveFreq", "wave freq", "Clump / wave", PKind::Float, 0, 12},
        {"physicalMaterials.waveness.waveFreqNoise", "wave freq noise", "Clump / wave", PKind::Float, 0, 1},
        {"physicalMaterials.waveness.waveRootStraighten", "wave root straighten", "Clump / wave", PKind::Float, 0, 1},
        {"physicalMaterials.waveness.waveStrand", "wave strand", "Clump / wave", PKind::Float, 0, 1},
        {"physicalMaterials.waveness.waveClump", "wave clump", "Clump / wave", PKind::Float, 0, 1},
        {"graphicalMaterials.color.rootColor", "root colour", "Colour", PKind::Color, 0, 0},
        {"graphicalMaterials.color.tipColor", "tip colour", "Colour", PKind::Color, 0, 0},
        {"graphicalMaterials.color.rootTipColorWeight", "root/tip weight", "Colour", PKind::Float, 0, 1},
        {"graphicalMaterials.color.rootTipColorFalloff", "root/tip falloff", "Colour", PKind::Float, 0, 1},
        {"graphicalMaterials.color.rootAlphaFalloff", "root alpha falloff", "Colour", PKind::Float, 0, 1},
        {"graphicalMaterials.color.textureBrightness", "texture brightness", "Colour", PKind::Float, 0, 2},
        {"graphicalMaterials.specular.specularColor", "spec colour", "Colour", PKind::Color, 0, 0},
        {"graphicalMaterials.specular.specularPrimary", "spec primary", "Colour", PKind::Float, 0, 0.5f},
        {"graphicalMaterials.specular.specularPowerPrimary", "spec power primary", "Colour", PKind::Float, 1, 1000},
        {"graphicalMaterials.specular.specularPrimaryBreakup", "spec breakup", "Colour", PKind::Float, 0, 1},
        {"graphicalMaterials.specular.specularSecondary", "spec secondary", "Colour", PKind::Float, 0, 0.5f},
        {"graphicalMaterials.specular.specularPowerSecondary", "spec power secondary", "Colour", PKind::Float, 1, 200},
        {"graphicalMaterials.specular.specularSecondaryOffset", "spec secondary offset", "Colour", PKind::Float, -1, 1},
        {"graphicalMaterials.glint.glintStrength", "glint", "Colour", PKind::Float, 0, 1},
        {"graphicalMaterials.shadow.shadowSigma", "shadow sigma", "Colour", PKind::Float, 0, 5},
        {"graphicalMaterials.shadow.shadowDensityScale", "shadow density", "Colour", PKind::Float, 0, 1},
        {"physicalMaterials.stiffness.stiffness", "stiffness", "Dynamics", PKind::Float, 0, 1},
        {"physicalMaterials.stiffness.rootStiffness", "root stiffness", "Dynamics", PKind::Float, 0, 1},
        {"physicalMaterials.stiffness.tipStiffness", "tip stiffness", "Dynamics", PKind::Float, 0, 1},
        {"physicalMaterials.stiffness.bendStiffness", "bend stiffness", "Dynamics", PKind::Float, 0, 1},
        {"physicalMaterials.stiffness.stiffnessStrength", "stiffness strength", "Dynamics", PKind::Float, 0, 1},
        {"physicalMaterials.stiffness.stiffnessDamping", "stiffness damping", "Dynamics", PKind::Float, 0, 1},
        {"physicalMaterials.stiffness.interactionStiffness", "interaction stiffness", "Dynamics", PKind::Float, 0, 1},
        {"physicalMaterials.stiffness.stiffnessCurve", "stiffness curve (root -> tip)", "Dynamics", PKind::Curve, 0, 1},
        {"physicalMaterials.stiffness.bendStiffnessCurve", "bend curve (root -> tip)", "Dynamics", PKind::Curve, 0, 1},
        {"physicalMaterials.stiffness.stiffnessStrengthCurve", "strength curve", "Dynamics", PKind::Curve, 0, 1},
        {"physicalMaterials.stiffness.stiffnessDampingCurve", "stiffness damping curve", "Dynamics", PKind::Curve, 0, 1},
        {"physicalMaterials.simulation.damping", "damping", "Dynamics", PKind::Float, 0, 1},
        {"physicalMaterials.simulation.massScale", "mass scale", "Dynamics", PKind::Float, 0, 20},
        {"physicalMaterials.simulation.inertiaLimit", "inertia limit", "Dynamics", PKind::Float, 0, 10},
        {"physicalMaterials.simulation.backStopRadius", "back stop radius", "Dynamics", PKind::Float, 0, 1},
        {"physicalMaterials.simulation.windScaler", "wind scaler", "Dynamics", PKind::Float, 0, 50},
        {"physicalMaterials.simulation.windNoise", "wind noise", "Dynamics", PKind::Float, 0, 5},
        {"levelOfDetail.detailLOD.detailLODDensity", "close-up density x", "LOD", PKind::Float, 0, 3},
        {"levelOfDetail.detailLOD.detailLODStart", "close-up start (m)", "LOD", PKind::Float, 0, 5},
        {"levelOfDetail.detailLOD.detailLODEnd", "close-up end (m)", "LOD", PKind::Float, 0, 5},
        {"levelOfDetail.detailLOD.detailLODWidth", "close-up width (mm)", "LOD", PKind::Float, 0, 3},
        {"levelOfDetail.distanceLOD.distanceLODStart", "distance LOD start (m)", "LOD", PKind::Float, 0, 50},
        {"levelOfDetail.distanceLOD.distanceLODEnd", "distance LOD end (m)", "LOD", PKind::Float, 0, 200},
        {"levelOfDetail.distanceLOD.distanceLODWidth", "distance LOD width (mm)", "LOD", PKind::Float, 0, 5},
    };
    return d;
}

float paramDefault(const std::string& path) {
    static const std::map<std::string, float> m = {
        {"physicalMaterials.volume.density", 1}, {"physicalMaterials.volume.lengthNoise", 1}, {"physicalMaterials.volume.lengthScale", 1},
        {"physicalMaterials.strandWidth.width", 1}, {"physicalMaterials.strandWidth.widthRootScale", 1},
        {"physicalMaterials.strandWidth.widthTipScale", 0.1f}, {"physicalMaterials.clumping.clumpRoundness", 1},
        {"physicalMaterials.waveness.waveScaleNoise", 0.5f}, {"physicalMaterials.waveness.waveFreq", 3},
        {"physicalMaterials.waveness.waveFreqNoise", 0.5f}, {"physicalMaterials.waveness.waveStrand", 1},
        {"graphicalMaterials.color.rootTipColorWeight", 0.5f}, {"graphicalMaterials.color.rootTipColorFalloff", 1},
        {"graphicalMaterials.color.textureBrightness", 1}, {"graphicalMaterials.specular.specularPrimary", 0.1f},
        {"graphicalMaterials.specular.specularPowerPrimary", 100}, {"graphicalMaterials.specular.specularSecondary", 0.05f},
        {"graphicalMaterials.specular.specularPowerSecondary", 20}, {"graphicalMaterials.specular.specularSecondaryOffset", 0.1f},
        {"graphicalMaterials.shadow.shadowSigma", 0.2f}, {"graphicalMaterials.shadow.shadowDensityScale", 0.5f},
        {"physicalMaterials.stiffness.stiffness", 0.5f}, {"physicalMaterials.stiffness.rootStiffness", 0.5f},
        {"physicalMaterials.stiffness.stiffnessStrength", 1}, {"physicalMaterials.simulation.massScale", 10},
        {"physicalMaterials.simulation.inertiaLimit", 1000}, {"physicalMaterials.simulation.windScaler", 1},
        {"levelOfDetail.detailLOD.detailLODDensity", 1}, {"levelOfDetail.detailLOD.detailLODStart", 2},
        {"levelOfDetail.detailLOD.detailLODEnd", 1}, {"levelOfDetail.detailLOD.detailLODWidth", 1},
        {"levelOfDetail.distanceLOD.distanceLODStart", 5}, {"levelOfDetail.distanceLOD.distanceLODEnd", 10},
        {"levelOfDetail.distanceLOD.distanceLODWidth", 1}};
    auto it = m.find(path);
    return it == m.end() ? 0.0f : it->second;
}

static std::vector<std::string> split(const std::string& p) {
    std::vector<std::string> out;
    std::stringstream s(p);
    std::string t;
    while (std::getline(s, t, '.')) out.push_back(t);
    return out;
}

json Look::base(const std::string& path) const {
    if (!pack) return nullptr;            // no character loaded yet
    auto parts = split(path);
    const json* j = nullptr;
    if (parts[0] == "physicalMaterials") j = &pack->physical;
    else if (parts[0] == "graphicalMaterials") j = &pack->graphical;
    else if (parts[0] == "levelOfDetail") j = &pack->lod;
    else return nullptr;
    for (size_t i = 1; i < parts.size(); i++) {
        if (!j->is_object() || !j->contains(parts[i])) return nullptr;
        j = &(*j)[parts[i]];
    }
    return *j;
}

json Look::get(const std::string& path) const {
    auto it = over.find(path);
    return it != over.end() ? it->second : base(path);
}

float Look::f(const std::string& path, float def) const {
    json v = get(path);
    if (v.is_number()) return v.get<float>();
    if (v.is_boolean()) return v.get<bool>() ? 1.0f : 0.0f;
    return def;
}

void Look::setF(const std::string& path, float v) {
    json b = base(path);
    if (b.is_number() && std::fabs(b.get<float>() - v) < 1e-6f) over.erase(path);
    else over[path] = v;
    enforceCaps();
}

void Look::setColor(const std::string& path, int r, int g, int b) {
    json c = {{"Red", r}, {"Green", g}, {"Blue", b}, {"Alpha", 255}};
    json bs = base(path);
    if (bs.is_object() && bs.value("Red", -1) == r && bs.value("Green", -1) == g && bs.value("Blue", -1) == b) over.erase(path);
    else over[path] = c;
}

float Look::cap(const std::string& path) const {
    if (!pack) return std::numeric_limits<float>::infinity();
    const json& R = pack->budgetRef;
    const float inf = std::numeric_limits<float>::infinity();
    if (path == "physicalMaterials.volume.density") return float(R.value("density", 1.0) * limit);
    if (path == "levelOfDetail.detailLOD.detailLODDensity") return float(std::min(std::max(R.value("detailLODDensity", 1.0), 1.0) * limit, 2.0));
    if (path == "physicalMaterials.strandWidth.width") return float(R.value("width", 1.0) * 1.5);
    if (path == "levelOfDetail.detailLOD.detailLODWidth") return float(std::max(R.value("width", 1.0), 0.5) * 1.5);
    if (path == "levelOfDetail.distanceLOD.distanceLODEnd") return std::max(base(path).is_number() ? base(path).get<float>() : 100.0f, 100.0f);
    return inf;
}

void Look::enforceCaps() {
    for (auto& d : paramDefs()) {
        if (d.kind != PKind::Float) continue;
        float c = cap(d.path);
        if (std::isinf(c)) continue;
        float v = f(d.path, 0.0f);
        if (v > c) over[d.path] = c;
    }
}

void Look::setJson(const std::string& path, const json& v) {
    if (base(path) == v) over.erase(path);
    else over[path] = v;
}

json Look::exportLook() const {
    json set = json::object();
    for (auto& [path, v] : over) {
        json b = base(path);
        if (v == b) continue;
        // the exporter takes the exact CR2W type from the character's redfur (or the preset's source) when it has one
        if (v.is_object() && v.contains("Red")) set[path] = {"Color", {v["Red"], v["Green"], v["Blue"]}};
        else if (v.is_object() && v.contains("X")) set[path] = {"Vector", {v.value("X", 0.0), v.value("Y", 0.0), v.value("Z", 0.0), v.value("W", 0.0)}};
        else if (v.is_number_float()) set[path] = {"Float", v};
        else set[path] = {"auto", v};
    }
    return {{"set", set}};
}

Budget Look::budget(int numFaces, double sumFaceDensity, int K) const {
    Budget b;
    const json& R = pack->budgetRef;
    double dens = f("physicalMaterials.volume.density", 1.0f);
    double close = f("levelOfDetail.detailLOD.detailLODDensity", 1.0f);
    double width = f("physicalMaterials.strandWidth.width", 1.0f);
    int cvs = (K - 1) * pack->splineMultiplier + 1;
    b.hairs = 64.0 * dens * sumFaceDensity;
    b.hairsClose = b.hairs * close;
    b.cost = b.hairsClose * cvs * width;
    b.refHairs = R.value("hairs", 0.0);
    b.refHairsClose = R.value("hairs_close", 0.0);
    b.refCost = R.value("cost", 0.0);
    b.refName = R.value("config", std::string("?"));
    double r1 = b.refHairsClose > 0 ? b.hairsClose / b.refHairsClose : 0;
    double r2 = b.refCost > 0 ? b.cost / b.refCost : 0;
    b.ratio = std::max(r1, r2);
    b.overLimit = b.ratio > limit + 1e-6;
    std::ostringstream m;
    if (b.overLimit)
        m << "Over budget: " << int(std::round(b.ratio * 100)) << "% of " << b.refName << " (limit " << int(limit * 100)
          << "%). Lower density / close-up density / width, or delete guides, before export.";
    else
        m << int(std::round(b.ratio * 100)) << "% of " << b.refName;
    b.msg = m.str();
    return b;
}

// Thresholds mirror tools/hairworks/perf_budget.py. Measured in game (W3 5.0, RTX 5090, path tracing, one NPC; Vesemir 552 faces
// clump 0.8 = 14 ms): path-traced hair renders a fixed 32 hairs per growth face, so faces matter, not density; Lambert clump
// 0.3 -> 0.8 saved ~4 ms; Lambert shipped 1007 faces clump 0.3 ~27 ms; Guillaume 1516 faces curled 32 ms vs 15.5 ms curl off.
void Look::pathTrace(Budget& b, int numFaces, double tort, double pts) const {
    const int facesWarn = 600, facesStrong = 900, clumpFaces = 450;
    const double clumpLow = 0.5, tortWarn = 1.5, tortPts = 20, tortPtsTort = 1.4;
    double clump = f("physicalMaterials.clumping.clumpScale", 1.0f);
    double wScale = f("physicalMaterials.waveness.waveScale", 1.0f), wStrand = f("physicalMaterials.waveness.waveStrand", 0.0f);
    char buf[200];
    b.warns.clear();
    if (numFaces > facesStrong) {
        snprintf(buf, sizeof buf, "Very many growth faces (%d > %d): path-traced hair cost scales with faces, not density; expect a large GPU cost.", numFaces, facesStrong);
        b.warns.push_back(buf);
    } else if (numFaces > facesWarn) {
        snprintf(buf, sizeof buf, "Many growth faces (%d > %d): path-traced hair cost scales with faces, not density; aim for ~550.", numFaces, facesWarn);
        b.warns.push_back(buf);
    }
    if (clump < clumpLow && numFaces > clumpFaces) {
        snprintf(buf, sizeof buf, "Low clumping (%.2f) with many faces (%d) is slow under path tracing; try clump 0.8.", clump, numFaces);
        b.warns.push_back(buf);
    }
    if (tort > tortWarn || (pts > tortPts && tort > tortPtsTort)) {
        snprintf(buf, sizeof buf, "Tight curl is expensive under path tracing (tortuosity %.2f, %.0f points/guide); loosen pitch / radius.", tort, pts);
        b.warns.push_back(buf);
    }
    double score = numFaces / 1000.0 * (1.0 + std::max(0.0, 0.8 - clump) / 0.5) * (1.0 + std::max(0.0, tort - 1.3) * 3.0) *
                   (1.0 + 2.0 * wStrand * std::max(0.0, 0.4 - wScale) / 0.4);
    (void)score;
    b.costNote.clear();   // the ms estimate comes from pt_estimate.cpp (budget card)
}

std::string fmtCount(double n) {
    char buf[32];
    if (n >= 1e6) snprintf(buf, sizeof buf, "%.2fM", n / 1e6);
    else if (n >= 1e3) snprintf(buf, sizeof buf, "%.1fk", n / 1e3);
    else snprintf(buf, sizeof buf, "%.0f", n);
    return buf;
}
