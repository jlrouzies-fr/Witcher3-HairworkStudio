// Hand-back gate state + save / export guard (2026-10-16, agent tools). Our own code (Studio License).
// The gate itself (serve_gate.inc, command "handback_gate") runs the whole hand-back checklist of
// tools/hairworks/AGENT_DIFFICULTIES.md on the open project and records its verdict here. The guard refuses an MCP /
// --agent save or export while the LAST gate on this project failed, unless the call carries an explicit "override"
// (a reason string), which is logged. The user's own saves through the UI never pass through here.
#pragma once
#include <string>
#include "app.h"

namespace gate {
struct State {
    bool ran = false, ok = false;
    std::string project;        // project path (or config) the verdict belongs to
    std::string config;
    size_t ops = 0;             // op count when the gate ran (a later edit makes the verdict stale)
    std::string when;           // local time
    json summary;               // failed items + counts (what the guard reports back)
    std::string sheet;
};
State& state();
void record(const App& a, bool ok, const json& summary, const std::string& sheet);
// The guard for one save / export call. Returns false and fills `err` when the call must be refused; a passing call
// with a stale or missing gate gets a "gate_note" in `note`. action = "save_project" | "export" | "save_edits".
bool guard(const App& a, const json& call, const std::string& action, const std::string& out, std::string& err, json& note);
}

// serve_gate.inc (compiled into serve.cpp): the gate command, also used by the --agent step {"do": "handback_gate"}
json handbackGateCmd(App& a, const json& c, const std::string& out);
