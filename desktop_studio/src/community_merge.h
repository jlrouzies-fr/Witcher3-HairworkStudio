// Desktop Groom Studio - community pack logic that only needs JSON: converter between Studio ops and hwgroom ops, and the
// 3-way merge (port of tools/hairworks/hwgroom.py: ops identified by canonical JSON, touched guides by root position in mm).
// Header-only so the test target (tests/community/community_merge_test.cpp) can use it without the app. Our own code (MIT).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "../third_party/nlohmann/json.hpp"

namespace cm {
using nlohmann::json;

inline const std::set<std::string>& opTypes() {
    static const std::set<std::string> s = {"translate", "set_point", "shorten", "delete", "duplicate", "reshape", "mirror", "delete_face", "rebuild"};
    return s;
}
inline std::string opType(const json& op) {
    if (op.contains("type") && op["type"].is_string()) return op["type"].get<std::string>();
    if (op.contains("op") && op["op"].is_string()) return op["op"].get<std::string>();
    return "";
}
// keys are sorted in nlohmann objects, so dump() equals python json.dumps(sort_keys=True, separators=(',', ':'))
inline std::string canon(const json& op) { return op.dump(); }

// "*" for mirror, else rounded-mm root keys ("x,y,z")
inline std::set<std::string> touched(const json& op) {
    std::set<std::string> t;
    if (opType(op) == "mirror" || opType(op) == "rebuild") { t.insert("*"); return t; }   // a rebuild conflicts with everything on the other side
    json roots = json::array();
    if (op.contains("roots") && op["roots"].is_array()) roots = op["roots"];
    else if (op.contains("root")) roots.push_back(op["root"]);
    for (auto& r : roots) {
        if (!r.is_array() || r.size() < 3) continue;
        char b[96];
        snprintf(b, sizeof b, "%lld,%lld,%lld", (long long)std::nearbyint(r[0].get<double>() * 1000), (long long)std::nearbyint(r[1].get<double>() * 1000),
                 (long long)std::nearbyint(r[2].get<double>() * 1000));
        t.insert(b);
    }
    return t;
}
inline bool overlap(const std::set<std::string>& a, const std::set<std::string>& b) {
    if (a.empty() || b.empty()) return false;
    if (a.count("*") || b.count("*")) return true;
    for (auto& x : a) if (b.count(x)) return true;
    return false;
}

// ---- converter -------------------------------------------------------------------------------------------------
// Studio ops -> hwgroom ops. Curls must already be baked into reshape ops (pass Groom::editsDoc(true)["ops"], which also
// resolves a mirror without source into the explicit form). Editor-only "grp" and the legacy "op" key are dropped.
//  - delete_face: {type, roots[3], face}; face = index into the base mesh faces (Ctx::faceIndex, -1 = a face made by earlier
//    ops, found by roots). Indices only; the pack's mesh_fingerprint (guide + face count) is checked on apply.
//  - rebuild: parametric, {type, kind, params, studio_version}; the baked geometry of the Studio op is NEVER exported, the
//    user's Studio replays its own deterministic algorithm on their own game files. Needs the op's "rebuild" meta.
// skipped counts what still cannot be shared (a mirror that is not resolved, a rebuild without parameters, ...).
struct Ctx { std::function<int(const json& roots)> faceIndex; };
inline json normOp(json o) {
    std::string k = opType(o);
    o.erase("grp");
    o.erase("op");
    o["type"] = k;
    return o;
}
inline std::vector<json> toHwgroom(const std::vector<json>& studio, int* skipped = nullptr, const Ctx* ctx = nullptr) {
    std::vector<json> out;
    int sk = 0;
    for (auto& o : studio) {
        std::string k = opType(o);
        if (k == "rebuild_param") k = "rebuild";
        if (!opTypes().count(k)) { sk++; continue; }
        if (k == "mirror" && !(o.contains("source") && o["source"].is_string())) { sk++; continue; }
        if (k == "delete_face") {
            if (!o.contains("roots") || !o["roots"].is_array() || o["roots"].size() != 3) { sk++; continue; }
            int f = o.contains("face") && o["face"].is_number_integer() ? o["face"].get<int>() : (ctx && ctx->faceIndex ? ctx->faceIndex(o["roots"]) : -1);
            out.push_back({{"type", "delete_face"}, {"roots", o["roots"]}, {"face", f}});
            continue;
        }
        if (k == "rebuild") {
            json m = o.contains("rebuild") ? o["rebuild"] : json();   // Studio meta {kind, params, studio_version}
            if (!m.is_object() && o.contains("params")) m = o;        // hwgroom-form op passed through
            if (!m.is_object() || !m.contains("params") || !m.contains("kind")) { sk++; continue; }
            out.push_back({{"type", "rebuild"}, {"kind", m["kind"]}, {"params", m["params"]}, {"studio_version", m.value("studio_version", std::string())}});
            continue;
        }
        out.push_back(normOp(o));
    }
    if (skipped) *skipped = sk;
    return out;
}
// hwgroom ops -> Studio ops (same strand_edits op vocabulary; duplicates use "op" like the editor writes them).
// A rebuild comes back as the unexpanded "rebuild_param" op: the Studio runs its own algorithm for it when applying.
inline std::vector<json> toStudio(const std::vector<json>& hw, int* skipped = nullptr) {
    std::vector<json> out;
    int sk = 0;
    for (auto& o0 : hw) {
        std::string k = opType(o0);
        if (!opTypes().count(k)) { sk++; continue; }
        if (k == "rebuild" && o0.value("kind", std::string()) == "baked" && o0.contains("params") && o0["params"].is_object()) {
            // baked geometry snapshot (guides + faces + uv): replaces every guide of the loaded groom, see Groom::applyRebuild
            json b = o0["params"];
            b["op"] = "rebuild";
            b["delete_all"] = true;
            out.push_back(b);
            continue;
        }
        json o = normOp(o0);
        if (k == "duplicate" || k == "delete_face") { o.erase("type"); o["op"] = k; }
        if (k == "rebuild") { o.erase("type"); o["op"] = "rebuild_param"; }
        out.push_back(o);
    }
    if (skipped) *skipped = sk;
    return out;
}

// ---- 3-way merge -------------------------------------------------------------------------------------------------
struct Conflict {
    std::vector<json> theirs, mine;
    std::vector<char> mineRemoved;   // per mine op: true = mine REMOVED this base op (it is absent from the merge)
    std::string reason;
    bool theirsAdd = true;           // theirs added the op (else theirs removed a base op that mine modified)
};
struct MergeResult { std::vector<json> merged; std::vector<Conflict> conflicts; };

inline MergeResult merge3(const std::vector<json>& base, const std::vector<json>& theirs, const std::vector<json>& mine) {
    std::vector<std::string> bc;
    for (auto& o : base) bc.push_back(canon(o));
    std::set<std::string> bset(bc.begin(), bc.end());
    auto delta = [&](const std::vector<json>& side, std::vector<json>& add, std::set<std::string>& rem) {
        std::set<std::string> sset;
        for (auto& o : side) sset.insert(canon(o));
        for (auto& o : side) if (!bset.count(canon(o))) add.push_back(o);
        for (auto& c : bc) if (!sset.count(c)) rem.insert(c);
    };
    std::vector<json> tAdd, mAdd;
    std::set<std::string> tRem, mRem;
    delta(theirs, tAdd, tRem);
    delta(mine, mAdd, mRem);
    std::set<std::string> tAddC;
    for (auto& o : tAdd) tAddC.insert(canon(o));
    MergeResult R;
    std::set<std::string> dropT;
    for (auto& ta : tAdd) {
        auto tt = touched(ta);
        Conflict c;
        for (auto& ma : mAdd)
            if (canon(ma) != canon(ta) && overlap(tt, touched(ma))) { c.mine.push_back(ma); c.mineRemoved.push_back(0); }
        for (auto& s : mRem) {
            json j = json::parse(s);
            if (overlap(tt, touched(j))) { c.mine.push_back(j); c.mineRemoved.push_back(1); }
        }
        if (!c.mine.empty()) {
            c.theirs = {ta};
            c.reason = "both sides edit the same guides";
            c.theirsAdd = true;
            R.conflicts.push_back(c);
            dropT.insert(canon(ta));
        }
    }
    for (auto& s : tRem) {
        json tr = json::parse(s);
        for (auto& ma : mAdd)
            if (overlap(touched(tr), touched(ma)) && !tAddC.count(canon(ma))) {
                Conflict c;
                c.theirs = {tr};
                c.mine = {ma};
                c.mineRemoved = {0};
                c.reason = "theirs removed an op on guides that mine changed";
                c.theirsAdd = false;
                R.conflicts.push_back(c);
            }
    }
    std::set<std::string> rem = tRem;
    rem.insert(mRem.begin(), mRem.end());
    std::set<std::string> seen;
    for (auto& o : base) if (!rem.count(canon(o))) { R.merged.push_back(o); seen.insert(canon(o)); }
    std::vector<json> tail = mAdd;
    for (auto& o : tAdd) if (!dropT.count(canon(o))) tail.push_back(o);
    for (auto& o : tail) { std::string c = canon(o); if (!seen.count(c)) { R.merged.push_back(o); seen.insert(c); } }
    return R;
}

// choice per conflict: 0 keep mine (the merge as built), 1 take theirs, 2 keep both
inline std::vector<json> resolve(std::vector<json> merged, const std::vector<Conflict>& cs, const std::vector<int>& choice) {
    for (size_t i = 0; i < cs.size(); i++) {
        int ch = i < choice.size() ? choice[i] : 0;
        if (ch == 0) continue;
        const Conflict& c = cs[i];
        if (ch == 1) {
            std::set<std::string> gone;
            for (size_t m = 0; m < c.mine.size(); m++) if (!c.mineRemoved[m]) gone.insert(canon(c.mine[m]));
            merged.erase(std::remove_if(merged.begin(), merged.end(), [&](const json& o) { return gone.count(canon(o)) > 0; }), merged.end());
            for (size_t m = 0; m < c.mine.size(); m++) if (c.mineRemoved[m]) merged.push_back(c.mine[m]);   // theirs never removed them
        }
        if (c.theirsAdd) merged.push_back(c.theirs[0]);
    }
    std::set<std::string> seen;
    std::vector<json> out;
    for (auto& o : merged) if (seen.insert(canon(o)).second) out.push_back(o);
    return out;
}
}  // namespace cm
