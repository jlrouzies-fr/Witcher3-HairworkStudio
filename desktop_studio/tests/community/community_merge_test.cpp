// Reads {base, theirs, mine} (lists of ops) from argv[1] and prints {merged, conflicts:[{theirs,mine,reason}]} as JSON.
// tests/community/crosscheck.py compares this with tools/hairworks/hwgroom.py merge3.
#include <fstream>
#include <iostream>
#include "../../src/community_merge.h"
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream f(argv[1]);
    cm::json j = cm::json::parse(f);
    auto L = [&](const char* k) { std::vector<cm::json> v; for (auto& o : j[k]) v.push_back(o); return v; };
    auto R = cm::merge3(L("base"), L("theirs"), L("mine"));
    cm::json out;
    out["merged"] = R.merged;
    out["conflicts"] = cm::json::array();
    for (auto& c : R.conflicts) out["conflicts"].push_back({{"theirs", c.theirs}, {"mine", c.mine}, {"reason", c.reason}});
    // converter round trip: hwgroom -> Studio -> hwgroom must lose nothing (skip counter 0, same ops)
    int sk1 = 0, sk2 = 0;
    std::vector<cm::json> all = L("theirs"), st = cm::toStudio(all, &sk1), back = cm::toHwgroom(st, &sk2);
    out["roundtrip"] = {{"skipped", sk1 + sk2}, {"equal", back == all}};
    std::cout << out.dump();
    return 0;
}
