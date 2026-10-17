// Hand-back gate state + save / export guard (2026-10-16, agent tools). See gate.h. Our own code (Studio License).
#include <ctime>
#include <filesystem>
#include <fstream>
#include "gate.h"

namespace gate {
State& state() { static State s; return s; }

static std::string now() {
    char b[32];
    time_t t = time(nullptr);
    tm tmv;
    localtime_s(&tmv, &t);
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tmv);
    return b;
}

void record(const App& a, bool ok, const json& summary, const std::string& sheet) {
    State& s = state();
    s.ran = true; s.ok = ok;
    s.project = a.projectPath.empty() ? a.configName : a.projectPath;
    s.config = a.configName;
    s.ops = a.groom.ops.size();
    s.when = now();
    s.summary = summary;
    s.sheet = sheet;
}

bool guard(const App& a, const json& call, const std::string& action, const std::string& out, std::string& err, json& note) {
    State& s = state();
    std::string proj = a.projectPath.empty() ? a.configName : a.projectPath;
    bool same = s.ran && s.config == a.configName;   // a verdict belongs to the open build (the project path changes on every save)
    if (!same) { note = {{"gate", "not run on this build"}}; return true; }
    bool stale = s.ops != a.groom.ops.size();
    if (s.ok) { note = {{"gate", stale ? "passed, but edits were made since: run studio_handback_gate again" : "passed"}, {"gate_when", s.when}}; return true; }
    std::string reason = call.value("override", std::string());
    if (call.contains("override") && call["override"].is_boolean() && call["override"].get<bool>()) reason = "(no reason given)";
    if (reason.empty()) {
        err = "hand-back gate: the last studio_handback_gate on this build FAILED (" + s.when + "): " + action +
              " refused. Fix the defects and run the gate again, or pass override: \"<reason>\" (logged). Failed items: " +
              s.summary.value("failed", json::array()).dump();
        return false;
    }
    std::string logPath = out + "\\gate_overrides.log";
    try {
        std::filesystem::create_directories(out);
        std::ofstream(logPath, std::ios::app) << now() << "  " << action << "  " << proj << "  reason: " << reason
                                              << "  failed: " << s.summary.value("failed", json::array()).dump() << "\n";
    } catch (...) {}
    logf("gate override: %s on %s, reason: %s", action.c_str(), proj.c_str(), reason.c_str());
    note = {{"gate", "FAILED verdict overridden"}, {"override_reason", reason}, {"override_logged", logPath}, {"gate_when", s.when},
            {"gate_failed", s.summary.value("failed", json::array())}};
    return true;
}
}  // namespace gate
