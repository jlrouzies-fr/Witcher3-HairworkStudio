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
double r4(double v);
