// Desktop Groom Studio - guide edits (ops list replayed on the edit-frame guides).
// Same ops format as tools/hairworks/strand_edits.py and the web Groom Studio. Our own code (MIT).
#pragma once
#include <set>
#include <string>
#include <vector>

#include "deskpack.h"

struct Face { std::array<int, 3> g; std::array<float, 6> uv; };

struct Groom {
    const DeskPack* pack = nullptr;
    int N0 = 0, K = 0;
    float tol = 0.004f;
    std::vector<json> preOps;   // none for now (kept for format parity)
    std::vector<json> ops, redo;
    // replay result
    std::vector<Guide> P;       // edit frame
    std::vector<char> alive;
    std::vector<int> src;       // duplicate source (-1 = original)
    std::set<int> edited;
    std::vector<Face> faces;    // current growth faces (indices into P)
    int unmatched = 0;

    void init(const DeskPack* p);
    void replay();
    int baseOf(int i) const;
    V3 renderPoint(int i, int k) const;      // edit frame + fixed build offset = fur frame
    Guide renderGuide(int i) const;
    int numAlive() const;

    // edits (append op + replay)
    void push(json op);
    void undo();
    void redoOp();
    json rootsOf(const std::vector<int>& ix) const;
    json editsDoc(bool bake = true) const;   // bake: curl ops -> reshape ops (what strand_edits.py applies)
    std::string editsText(bool bake = true) const;
    std::vector<json> baked;    // filled by replay(): the op list with curls baked into reshape ops

private:
    std::vector<int> match(const json& roots);
    void applyOp(const json& op);
    void removeGuides(const std::vector<int>& dead);
    void insertGuide(int newIdx, bool edgeFace = false, bool srcUv = false, const float* fixUv = nullptr);
    void applyMirror(const json& op);
    void applyRebuild(const json& op);
public:
    void buildHead();
    json resolveMirror(const json& op);    // explicit mirror form (source, plane_x, source_roots) for community packs
    float mirrorPlane();                   // x of the head's mid-sagittal plane (same search as strand_edits.mirror_plane)
    float headSigned(V3 p, V3* nrm) const; // signed distance to the head mesh (nearest vertex + its normal)
private:
    float planeCache = 1e9f;
    std::vector<V3> headV, headN;
};

std::vector<V3> shortenGuide(const Guide& p, float keep);
// smooth curve through the CVs (centripetal Catmull-Rom, end tangents mirrored), perSeg samples per CV segment, root / tip exact
std::vector<V3> smoothDense(const Guide& p, int perSeg = 16);
// n CVs evenly spaced along the arc of the smooth curve through p (root and tip exact): the "resample" op / Studio resample
Guide resampleSmooth(const Guide& p, int n);
// sample the smooth curve through p at arc lengths s (m, 0 = root); s past the end extrapolates along the tip direction
std::vector<V3> sampleSmoothAt(const Guide& p, const std::vector<float>& s);
// max / mean distance (m) from the smooth curve of a to the smooth curve of b (shape change of a resample)
void curveDeviation(const Guide& a, const Guide& b, float& maxD, float& meanD);
constexpr int kMaxCvs = 32;   // resample cap (the game has run 25-CV guides; the HairWorks SDK documents no maximum)
double r4(double v);
// growth face whose 3 guides go different ways (direction root -> mid > divDeg AND tips spread > spreadK x rootSp): HairWorks fans hair across the gap
bool faceDivergent(const Groom& g, const Face& F, float rootSp, float divDeg = 50.0f, float spreadK = 4.0f);
// unwanted loop: (1) the strand comes back to within closeMax of an earlier point after more than arcMin of hair (back to its own
// root, over the top and back...), or (2) it circles: net turning of its non-tight corners >= turnDeg. Curls are exempt: a returning
// loop whose radius (farthest point from its centroid) is <= curlR, and corners tighter than 30 mm (repeated small-radius turns) do not count.
struct LoopInfo { bool flag = false; float arc = 0, dist = 0, radius = 0, turn = 0; };
LoopInfo guideLoop(const Guide& p, float arcMin = 0.06f, float closeMax = 0.015f, float curlR = 0.045f, float turnDeg = 270.0f);
