// Desktop Groom Studio - GitHub CLI layer of the Community window (detect / install / sign in / publish a pack as a pull
// request / PR status). Every process runs on a worker thread with hidden pipes (no UI freeze); output goes to a log that the
// window shows. The Studio never reads, stores or prints a token: gh keeps the login, git asks gh as credential helper.
// Our own code (MIT). See COMMUNITY_PLAN.md.
#pragma once
#include <string>
#include <vector>

namespace ghx {
enum Phase { Unknown = 0, Checking = 1, Missing = 2, NotAuth = 3, Ready = 4 };
struct State { int phase = Unknown; std::string user, ghPath; bool busy = false; };
State state();
void setDataDir(const std::string& dir);          // prs.json (opened PR per pack) lives here
std::vector<std::string> logLines();
void log(const std::string& s);                   // tokens are redacted before storing

void check();        // detect gh (PATH + %ProgramFiles%\GitHub CLI), then `gh auth status`, then the login name
void installCli();   // winget install --id GitHub.cli -e in a visible console; falls back to opening https://cli.github.com
void signIn();       // gh auth login --web --git-protocol https in a visible console

struct PubJob {
    std::string packDir;      // exported pack folder (pack.json, edits.hwgroom, preview.jpg)
    std::string upstream;     // owner/name of the community repo
    std::string character, slot, key /* author__name */, name, version, author, baseBuild;
    bool wip = true;
};
void publish(const PubJob& j);                    // fork, shallow clone, branch, copy, commit, push, pr create (or push to the open PR)
void prStatus(const std::string& key);            // gh pr view <url> --json state,statusCheckRollup
std::string prUrl(const std::string& key);
std::string prText(const std::string& key);       // last known status line
}  // namespace ghx
