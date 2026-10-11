// Desktop Groom Studio - curl helpers (ringlet / roll / wave) on guide strands. Our own code (MIT).
// Parameters follow hairpipe's opt-in keys (growth.roll_ends: start, radius, turns, shrink, side, band, start_below_top;
// growth.curl: radius, pitch, start, ramp). Every shape keeps the segment lengths of the strand (same CV count).
#pragma once
#include "deskpack.h"

struct CurlParams {
    std::string mode = "ringlet";   // ringlet | roll | wave
    float start = 0.35f;            // fraction of arc length where the shape starts
    float startBelowTop = -1.0f;    // roll only: start where the strand first drops below head top - this (m); < 0 = off
    float radius = 0.012f;          // m (ringlet radius / loop radius / wave amplitude)
    float turns = 2.0f;             // ringlet turns / roll turns (> 1 = loops on itself) / wave periods
    float ramp = 0.15f;             // ringlet: fraction over which the radius grows from 0
    float shrink = 0.6f;            // roll: radius at the end = radius * shrink (spiral inwards)
    float jitter = 0.3f;            // per-strand random phase / radius variation (0..1)
    int hand = 1;                   // ringlet: +1 right-handed, -1 left-handed
    int side = -1;                  // roll: -1 under (towards the head), +1 out; wave: plane flip
    bool band = false;              // roll: every guide rolls in the same vertical front-back plane (nape roll)
    bool zigzag = false;            // wave: triangle instead of sine
    V3 axis{0, 0, 0};               // roll: manual axis (0 = auto from the scalp)

    json toJson() const;
    static CurlParams fromJson(const json& j);
};

// returns the reshaped guide (same point count, same segment lengths)
Guide applyCurl(const Guide& p, const CurlParams& c, V3 headC, float headTop, unsigned seed);
// CVs available after the start point per turn (warning when < 6: the curl is under-sampled)
float curlCvsPerTurn(int K, const CurlParams& c);
