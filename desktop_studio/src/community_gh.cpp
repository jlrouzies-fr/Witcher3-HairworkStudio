// Desktop Groom Studio - GitHub CLI layer of the Community window. Our own code (MIT). See community_gh.h.
#include "community_gh.h"

#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include "../third_party/nlohmann/json.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace ghx {
namespace {
std::mutex g_mx;
std::vector<std::string> g_log;
State g_st;
std::string g_dataDir, g_gitPath;
std::map<std::string, std::string> g_prUrl, g_prText;
std::atomic<bool> g_busy{false};

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return s.substr(i);
}
std::string redact(std::string s) {
    static const std::regex tok(R"((gh[pousr]_[A-Za-z0-9_]{8,}|github_pat_[A-Za-z0-9_]{8,}))");
    s = std::regex_replace(s, tok, "***");
    static const std::regex hdr(R"((?:Authorization|Token)\s*[:=]\s*\S+)", std::regex::icase);
    return std::regex_replace(s, hdr, "[redacted]");
}
void setState(const std::function<void(State&)>& f) { std::lock_guard<std::mutex> l(g_mx); f(g_st); }

std::string envExpand(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    _dupenv_s(&v, &n, name);
    std::string r = v ? v : "";
    free(v);
    return r;
}

std::string findExe(const wchar_t* exe, const std::vector<std::string>& extra) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = SearchPathW(nullptr, exe, nullptr, MAX_PATH * 2, buf, nullptr);
    if (n > 0 && n < MAX_PATH * 2) return narrow(buf);
    std::error_code ec;
    for (auto& p : extra) if (!p.empty() && fs::exists(fs::u8path(p), ec)) return p;
    return "";
}

std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; bs = 0; continue; }
        r.append(bs, L'\\'); bs = 0; r += c;
    }
    r.append(bs * 2, L'\\');
    r += L'"';
    return r;
}

// environment of the children: our own + the gh / git folders on PATH (a fresh winget install is not in our PATH) + no prompts
std::vector<wchar_t> makeEnv(bool noPrompt) {
    std::wstring ghDir = g_st.ghPath.empty() ? L"" : fs::u8path(g_st.ghPath).parent_path().wstring();
    std::wstring gitDir = g_gitPath.empty() ? L"" : fs::u8path(g_gitPath).parent_path().wstring();
    std::vector<std::wstring> vars;
    wchar_t* e = GetEnvironmentStringsW();
    for (wchar_t* p = e; *p; p += wcslen(p) + 1) {
        std::wstring v = p;
        std::wstring up = v.substr(0, 5);
        for (auto& c : up) c = (wchar_t)towupper(c);
        if (up == L"PATH=") {
            std::wstring add;
            if (!ghDir.empty()) add += ghDir + L";";
            if (!gitDir.empty() && gitDir != ghDir) add += gitDir + L";";
            v = L"PATH=" + add + v.substr(5);
        }
        vars.push_back(v);
    }
    FreeEnvironmentStringsW(e);
    if (noPrompt) { vars.push_back(L"GH_PROMPT_DISABLED=1"); vars.push_back(L"GIT_TERMINAL_PROMPT=0"); vars.push_back(L"GH_NO_UPDATE_NOTIFIER=1"); }
    std::vector<wchar_t> blk;
    for (auto& v : vars) { blk.insert(blk.end(), v.begin(), v.end()); blk.push_back(0); }
    blk.push_back(0);
    return blk;
}

struct RunOpt { std::string cwd; std::string* out = nullptr; bool quiet = false; bool console = false; };

// argv[0] "gh" / "git" resolve to the detected executables. Returns the exit code (-1 = could not start).
int run(std::vector<std::string> argv, const RunOpt& o = RunOpt()) {
    if (argv[0] == "gh") argv[0] = g_st.ghPath.empty() ? "gh" : g_st.ghPath;
    if (argv[0] == "git") argv[0] = g_gitPath.empty() ? "git" : g_gitPath;
    std::wstring cl;
    for (size_t i = 0; i < argv.size(); i++) cl += (i ? L" " : L"") + quoteArg(wide(argv[i]));
    if (!o.quiet) {
        std::string shown;
        for (auto& a : argv) shown += (shown.empty() ? "" : " ") + a;
        log("> " + (argv[0] == g_st.ghPath ? "gh" + shown.substr(argv[0].size()) : shown));
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    HANDLE rd = nullptr, wr = nullptr, nul = INVALID_HANDLE_VALUE;
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (o.console) {
        flags |= CREATE_NEW_CONSOLE;
    } else {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        CreatePipe(&rd, &wr, &sa, 0);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = si.hStdError = wr;
        si.hStdInput = nul;
        flags |= CREATE_NO_WINDOW;
    }
    auto env = makeEnv(!o.console);
    std::wstring cwd = o.cwd.empty() ? L"" : wide(o.cwd);
    BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, !o.console, flags, env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (wr) CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        if (rd) CloseHandle(rd);
        if (!o.quiet) log("could not start " + argv[0]);
        return -1;
    }
    std::string all, line;
    if (rd) {
        char buf[4096];
        DWORD got;
        while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got) {
            all.append(buf, got);
            for (DWORD i = 0; i < got; i++) {
                if (buf[i] == '\n') { if (!o.quiet) log(trim(line)); line.clear(); }
                else line += buf[i];
            }
        }
        if (!o.quiet && !trim(line).empty()) log(trim(line));
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (o.out) *o.out = all;
    return (int)code;
}

std::string prsFile() { return g_dataDir + "\\prs.json"; }
void savePrs() {
    json j = json::object();
    for (auto& [k, u] : g_prUrl) j[k] = u;
    std::error_code ec;
    fs::create_directories(fs::u8path(g_dataDir), ec);
    std::ofstream(fs::u8path(prsFile())) << j.dump(1);
}

void doCheck() {
    setState([](State& s) { s.phase = Checking; s.user.clear(); });
    std::string pf = envExpand("ProgramFiles"), pf86 = envExpand("ProgramFiles(x86)"), la = envExpand("LOCALAPPDATA");
    std::string gh = findExe(L"gh.exe", {pf + "\\GitHub CLI\\gh.exe", pf86 + "\\GitHub CLI\\gh.exe", la + "\\Programs\\GitHub CLI\\gh.exe"});
    g_gitPath = findExe(L"git.exe", {pf + "\\Git\\cmd\\git.exe", pf86 + "\\Git\\cmd\\git.exe", la + "\\Programs\\Git\\cmd\\git.exe"});
    setState([&](State& s) { s.ghPath = gh; });
    std::string out;
    if (gh.empty() || run({"gh", "--version"}, {"", &out, true}) != 0) {
        setState([](State& s) { s.phase = Missing; s.ghPath.clear(); });
        log("GitHub CLI (gh) not found.");
        g_busy = false;
        return;
    }
    log("gh: " + trim(out.substr(0, out.find('\n'))) + (g_gitPath.empty() ? "   (git not found: needed to publish)" : ""));
    if (run({"gh", "auth", "status"}, {"", nullptr, true}) != 0) {   // output not shown: it names the account and the token scopes
        setState([](State& s) { s.phase = NotAuth; });
        log("Not signed in to GitHub.");
        g_busy = false;
        return;
    }
    std::string u;
    run({"gh", "api", "user", "-q", ".login"}, {"", &u, true});
    setState([&](State& s) { s.phase = Ready; s.user = trim(u); });
    log("Signed in as " + trim(u) + ".");
    g_busy = false;
}

bool startJob() {
    bool e = false;
    return g_busy.compare_exchange_strong(e, true);
}

struct PrRec { int number = 0; std::string url, head; };

void doPublish(PubJob j) {
    auto fail = [&](const std::string& m) { log("Publish stopped: " + m); g_busy = false; };
    std::error_code ec;
    if (g_gitPath.empty()) return fail("git was not found (install Git for Windows).");
    std::string out;
    log("[1/9] GitHub user");
    if (run({"gh", "api", "user", "-q", ".login"}, {"", &out, true}) != 0) return fail("gh api user failed (sign in again).");
    std::string user = trim(out);
    if (_stricmp(user.c_str(), j.author.c_str()) != 0) return fail("pack author \"" + j.author + "\" is not your GitHub login \"" + user + "\" (the pack folder must be <your login>__<name>).");
    std::string repoName = j.upstream.substr(j.upstream.find('/') + 1);
    log("[2/9] Fork " + j.upstream + " (an existing fork is reused)");
    if (run({"gh", "repo", "fork", j.upstream, "--clone=false"}) != 0) return fail("gh repo fork failed.");
    std::string tmp = (fs::temp_directory_path() / ("hwstudio_pub_" + std::to_string(GetTickCount64()))).string();
    struct Cleanup { std::string d; ~Cleanup() {
        std::error_code e;
        for (auto& x : fs::recursive_directory_iterator(fs::u8path(d), e)) fs::permissions(x.path(), fs::perms::owner_write, fs::perm_options::add, e);
        fs::remove_all(fs::u8path(d), e);
    } } cleanup{tmp};
    log("[3/9] Shallow clone of your fork");
    if (run({"git", "clone", "--depth", "1", "https://github.com/" + user + "/" + repoName + ".git", tmp}) != 0) return fail("git clone failed.");
    RunOpt in{tmp};
    log("[4/9] Fetch upstream main");
    if (run({"git", "remote", "add", "upstream", "https://github.com/" + j.upstream + ".git"}, in) != 0) return fail("git remote add failed.");
    if (run({"git", "fetch", "--depth", "1", "upstream", "main"}, in) != 0) return fail("git fetch upstream failed.");
    // an open PR of yours for the same pack gets new commits on its branch
    log("[5/9] Look for an open pull request of this pack");
    std::string branch = j.name + "-v" + j.version;
    bool reuse = false;
    PrRec pr;
    if (run({"gh", "pr", "list", "--repo", j.upstream, "--author", user, "--state", "open", "--json", "number,headRefName,url"}, {"", &out, true}) == 0) {
        try {
            for (auto& p : json::parse(out)) {
                std::string h = p.value("headRefName", "");
                if (h.rfind(j.name + "-v", 0) == 0) { pr.number = p.value("number", 0); pr.url = p.value("url", ""); pr.head = h; reuse = true; break; }
            }
        } catch (...) {}
    }
    if (reuse) {
        branch = pr.head;
        log("Open PR #" + std::to_string(pr.number) + " found, its branch " + branch + " gets the new commit.");
        if (run({"git", "fetch", "--depth", "1", "origin", branch + ":refs/remotes/origin/" + branch}, in) != 0) return fail("git fetch of the PR branch failed.");
        if (run({"git", "checkout", "-B", branch, "origin/" + branch}, in) != 0) return fail("git checkout failed.");
    } else {
        if (run({"git", "checkout", "-B", branch, "upstream/main"}, in) != 0) return fail("git checkout failed.");
    }
    log("[6/9] Copy characters/" + j.character + "/" + j.slot + "/" + j.key + "/");
    std::string rel = "characters/" + j.character + "/" + j.slot + "/" + j.key;
    fs::path dst = fs::u8path(tmp) / fs::u8path(rel);
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    for (auto& f : fs::directory_iterator(fs::u8path(j.packDir), ec))
        if (f.is_regular_file()) fs::copy_file(f.path(), dst / f.path().filename(), fs::copy_options::overwrite_existing, ec);
    log("[7/9] Commit");
    if (run({"git", "add", rel}, in) != 0) return fail("git add failed.");
    run({"git", "status", "--porcelain"}, {tmp, &out, true});
    if (trim(out).empty()) {
        if (!reuse) return fail("nothing to publish: the pack is identical to what is in the community repo.");
        log("No change against the PR branch; the open PR stays as it is: " + pr.url);
        { std::lock_guard<std::mutex> l(g_mx); g_prUrl[j.key] = pr.url; g_prText[j.key] = "open (no new commit)"; savePrs(); }
        g_busy = false;
        return;
    }
    std::string msg = (reuse ? "Update " : "Add ") + j.name + " v" + j.version;
    if (run({"git", "-c", "user.name=" + user, "-c", "user.email=" + user + "@users.noreply.github.com", "commit", "-m", msg}, in) != 0) return fail("git commit failed.");
    log("[8/9] Push to your fork (gh is the git credential helper)");
    run({"gh", "auth", "setup-git"});
    if (run({"git", "push", "-u", "origin", branch}, in) != 0) return fail("git push failed.");
    if (reuse) {
        log("[9/9] Pushed to the branch of the open PR: " + pr.url);
    } else {
        log("[9/9] Open the pull request");
        std::string body = "## What changed\n\n" + std::string(reuse ? "Update of" : "New pack") + " `" + j.key + "` v" + j.version + " for `" + j.character + "` / `" + j.slot + "` (status: " + (j.wip ? "wip" : "ready") +
                           ").\n\n## Base build\n\n`" + j.baseBuild + "`\n\n## Preview\n\n`preview.jpg` is in the pack folder.\n\nPublished from Hairwork Groom Studio. Edit ops only: no game or SDK files.\n";
        std::string bf = (fs::path(tmp).parent_path() / ("hwstudio_prbody_" + std::to_string(GetTickCount64()) + ".md")).string();
        { std::ofstream f(fs::u8path(bf), std::ios::binary); f << body; }
        int rc = run({"gh", "pr", "create", "--repo", j.upstream, "--base", "main", "--head", user + ":" + branch, "--title", msg, "--body-file", bf}, {tmp, &out, false});
        fs::remove(fs::u8path(bf), ec);
        if (rc != 0) return fail("gh pr create failed.");
        std::istringstream is(out);
        std::string ln;
        while (std::getline(is, ln)) { ln = trim(ln); if (ln.rfind("https://github.com/", 0) == 0) pr.url = ln; }
        if (pr.url.empty()) return fail("pull request created but its URL was not returned.");
    }
    { std::lock_guard<std::mutex> l(g_mx); g_prUrl[j.key] = pr.url; g_prText[j.key] = "open"; savePrs(); }
    log("Done: " + pr.url);
    g_busy = false;
}

void doPrStatus(std::string key, std::string url) {
    std::string out;
    int rc = run({"gh", "pr", "view", url, "--json", "state,statusCheckRollup"}, {"", &out, true});
    std::string t;
    if (rc != 0) t = "status unavailable (gh pr view failed)";
    else {
        try {
            json j = json::parse(out);
            std::string st = j.value("state", "?");
            int pass = 0, failn = 0, pend = 0;
            for (auto& c : j.value("statusCheckRollup", json::array())) {
                std::string concl = c.value("conclusion", ""), status = c.value("status", ""), state = c.value("state", "");
                if (concl == "SUCCESS" || concl == "NEUTRAL" || concl == "SKIPPED" || state == "SUCCESS") pass++;
                else if (concl == "FAILURE" || concl == "TIMED_OUT" || concl == "CANCELLED" || concl == "ACTION_REQUIRED" || state == "FAILURE" || state == "ERROR") failn++;
                else pend++;
                (void)status;
            }
            for (auto& ch : st) ch = (char)tolower(ch);
            t = st;
            if (st == "open") t += ", checks: " + std::to_string(pass) + " passing, " + std::to_string(failn) + " failing, " + std::to_string(pend) + " pending";
        } catch (...) { t = "status unreadable"; }
    }
    { std::lock_guard<std::mutex> l(g_mx); g_prText[key] = t; }
    log(key + ": " + t);
}
}  // namespace

State state() { std::lock_guard<std::mutex> l(g_mx); State s = g_st; s.busy = g_busy; return s; }
std::vector<std::string> logLines() { std::lock_guard<std::mutex> l(g_mx); return g_log; }
void log(const std::string& s) {
    std::lock_guard<std::mutex> l(g_mx);
    g_log.push_back(redact(s));
    if (g_log.size() > 500) g_log.erase(g_log.begin(), g_log.begin() + 100);
}
void setDataDir(const std::string& dir) {
    std::lock_guard<std::mutex> l(g_mx);
    if (g_dataDir == dir) return;
    g_dataDir = dir;
    try {
        std::ifstream f(fs::u8path(prsFile()));
        if (f) for (auto& [k, v] : json::parse(f).items()) if (v.is_string()) { g_prUrl[k] = v.get<std::string>(); g_prText[k] = "not checked yet"; }
    } catch (...) {}
}
std::string prUrl(const std::string& key) { std::lock_guard<std::mutex> l(g_mx); auto it = g_prUrl.find(key); return it == g_prUrl.end() ? "" : it->second; }
std::string prText(const std::string& key) { std::lock_guard<std::mutex> l(g_mx); auto it = g_prText.find(key); return it == g_prText.end() ? "" : it->second; }

void check() { if (startJob()) std::thread(doCheck).detach(); }
void installCli() {
    if (!startJob()) return;
    std::thread([] {
        log("Installing GitHub CLI with winget (a console window shows the progress).");
        int rc = run({"winget", "install", "--id", "GitHub.cli", "-e"}, {"", nullptr, false, true});
        if (rc != 0 && findExe(L"gh.exe", {envExpand("ProgramFiles") + "\\GitHub CLI\\gh.exe"}).empty()) {
            log("winget did not finish (code " + std::to_string(rc) + "); opening https://cli.github.com");
            ShellExecuteW(nullptr, L"open", L"https://cli.github.com", nullptr, nullptr, SW_SHOWNORMAL);
        }
        doCheck();
    }).detach();
}
void signIn() {
    if (!startJob()) return;
    std::thread([] {
        log("Signing in: finish the login in the console window and in your browser.");
        int rc = run({"gh", "auth", "login", "--web", "--git-protocol", "https"}, {"", nullptr, false, true});
        log(rc == 0 ? "Sign-in finished." : "Sign-in ended with code " + std::to_string(rc) + ".");
        doCheck();
    }).detach();
}
void publish(const PubJob& j) { if (startJob()) std::thread(doPublish, j).detach(); }
void prStatus(const std::string& key) {
    std::string u = prUrl(key);
    if (u.empty() || !startJob()) return;
    std::thread([key, u] { doPrStatus(key, u); g_busy = false; }).detach();
}
}  // namespace ghx
