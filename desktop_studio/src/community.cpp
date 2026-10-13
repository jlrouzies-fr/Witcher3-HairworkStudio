// Desktop Groom Studio - Community window (milestone 1: browse + read-only install + working copy). Our own code (MIT).
// Sources: a local folder (checkout of the community repo, or its community/ folder) or a GitHub repo "owner/name" + channel
// fetched over WinHTTP from raw.githubusercontent.com (browse / install need no login). Working copies, sync (3-way merge),
// export, apply and GitHub publishing through the gh CLI (community_gh.cpp). See COMMUNITY_PLAN.md.
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include "../third_party/imgui/imgui.h"
#include "app.h"
#include "community_gh.h"
#include "ui_widgets.h"
using w::S;
#include "community_merge.h"

#pragma comment(lib, "winhttp.lib")
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <cmath>
namespace fs = std::filesystem;

namespace {
struct CPack {
    json meta;
    std::string path;                 // relative folder in the repo: characters/<char>/<slot>/<author>__<name>
    std::string key() const { return meta.value("author", "") + "__" + meta.value("name", ""); }
    std::string ver() const { return meta.value("version", "0.0.0"); }
};

std::mutex g_mx;
std::vector<CPack> g_packs;
std::string g_msg = "Not loaded yet.";
std::atomic<bool> g_busy{false};
bool g_loadedOnce = false;
char g_filter[96] = "";
std::map<std::string, ID3D11ShaderResourceView*> g_tex;
std::map<std::string, ImVec2> g_texSz;

fs::path P(const std::string& u8) { return fs::u8path(u8); }
std::string dataDir(const App& a) { return P(a.set.file).parent_path().string(); }
std::string installRoot(const App& a) { return dataDir(a) + "\\community"; }
std::string workRoot(const App& a) { return a.set.repo + "\\build\\desktop_studio\\projects\\community"; }

void setMsg(const std::string& s) { std::lock_guard<std::mutex> l(g_mx); g_msg = s; }
std::string getMsg() { std::lock_guard<std::mutex> l(g_mx); return g_msg; }

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// HTTPS GET; returns the HTTP status (0 = transport error). Only raw.githubusercontent.com is allowed.
int httpGet(const std::string& url, std::string& body, size_t maxBytes = 4u * 1024 * 1024) {
    body.clear();
    if (url.rfind("https://raw.githubusercontent.com/", 0) != 0) return 0;
    std::wstring wu = wide(url);
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wu.c_str(), 0, 0, &uc)) return 0;
    HINTERNET s = WinHttpOpen(L"HairworkStudio-Community/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!s) return 0;
    int code = 0;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD sc = 0, sz = sizeof sc;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &sc, &sz, nullptr);
        code = (int)sc;
        DWORD av = 0;
        while (WinHttpQueryDataAvailable(r, &av) && av) {
            size_t o = body.size();
            body.resize(o + av);
            DWORD got = 0;
            if (!WinHttpReadData(r, &body[o], av, &got)) break;
            body.resize(o + got);
            if (body.size() > maxBytes) { code = -2; break; }   // over the cap: stop reading, refuse
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return code;
}

std::string rawUrl(const std::string& repo, const std::string& channel, const std::string& rel) {
    return "https://raw.githubusercontent.com/" + repo + "/" + (channel.empty() ? "main" : channel) + "/" + rel;
}

// ---- download safety: a pack is data only. Fixed file list with size caps, hashes from the index, pinned commit.
struct AllowedFile { const char* name; size_t cap; bool required; };
const AllowedFile kPackFiles[] = {
    {"pack.json", 64u * 1024, true},
    {"edits.hwgroom", 2u * 1024 * 1024, true},
    {"ribbons.json", 512u * 1024, false},
    {"preview.jpg", 300u * 1024, false},
    {"preview.png", 300u * 1024, false},
};
const AllowedFile* allowedFile(const std::string& n) {
    for (auto& f : kPackFiles) if (n == f.name) return &f;
    return nullptr;
}
std::string sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char out[32];
    std::string r;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
            BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 && BCryptFinishHash(h, out, 32, 0) == 0) {
            static const char* hx = "0123456789abcdef";
            for (unsigned char c : out) { r += hx[c >> 4]; r += hx[c & 15]; }
        }
        if (h) BCryptDestroyHash(h);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return r;
}
// image header check before decoding: real PNG / JPEG signature and a sane pixel size
bool imageHeaderOk(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::string b(64 * 1024, 0);
    f.read(b.data(), b.size());
    b.resize((size_t)f.gcount());
    auto u8 = [&](size_t i) { return i < b.size() ? (unsigned)(unsigned char)b[i] : 0u; };
    unsigned w = 0, h = 0;
    static const unsigned char sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() > 24 && memcmp(b.data(), sig, 8) == 0) {
        w = u8(16) << 24 | u8(17) << 16 | u8(18) << 8 | u8(19);
        h = u8(20) << 24 | u8(21) << 16 | u8(22) << 8 | u8(23);
    } else if (u8(0) == 0xFF && u8(1) == 0xD8) {
        size_t i = 2;
        while (i + 9 < b.size()) {
            if (u8(i) != 0xFF) return false;
            unsigned mk = u8(i + 1), len = u8(i + 2) << 8 | u8(i + 3);
            if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) { h = u8(i + 5) << 8 | u8(i + 6); w = u8(i + 7) << 8 | u8(i + 8); break; }
            i += 2 + len;
        }
    } else return false;
    return w > 0 && h > 0 && w <= 4096 && h <= 4096;
}

// the local source may be the repo root or its community/ folder
std::string localRoot(std::string d) {
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    std::error_code ec;
    if (!fs::is_directory(P(d) / "characters", ec) && fs::is_directory(P(d) / "community" / "characters", ec)) d += "\\community";
    return d;
}

bool readJson(const fs::path& f, json& j) {
    try { std::ifstream i(f, std::ios::binary); if (!i) return false; j = json::parse(i); return true; } catch (...) { return false; }
}

void clearRO(const fs::path& d) {
    std::error_code ec;
    if (!fs::exists(d, ec)) return;
    for (auto& e : fs::recursive_directory_iterator(d, ec)) fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add, ec);
}
void makeRO(const fs::path& d) {
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(d, ec))
        if (e.is_regular_file()) fs::permissions(e.path(), fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove, ec);
}
void removeAll(const fs::path& d) { clearRO(d); std::error_code ec; fs::remove_all(d, ec); }

int cmpVer(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s) { int v[3] = {0, 0, 0}; sscanf(s.c_str(), "%d.%d.%d", &v[0], &v[1], &v[2]); return std::array<int, 3>{v[0], v[1], v[2]}; };
    auto x = parse(a), y = parse(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

// installed copies: <root>/<author>__<name>@<ver>/pack.json
struct Inst { std::string dir, ver; };
std::vector<Inst> installedOf(const App& a, const std::string& key) {
    std::vector<Inst> r;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(P(installRoot(a)), ec)) {
        std::string n = e.path().filename().string();
        if (!e.is_directory() || n.rfind(key + "@", 0) != 0 || n.size() > 4 && n.substr(n.size() - 4) == ".tmp") continue;
        json m;
        if (readJson(e.path() / "pack.json", m)) r.push_back({e.path().string(), m.value("version", n.substr(key.size() + 1))});
    }
    return r;
}
const Inst* newestInst(const std::vector<Inst>& v) {
    const Inst* b = nullptr;
    for (auto& i : v) if (!b || cmpVer(i.ver, b->ver) > 0) b = &i;
    return b;
}

void doRefresh(Settings s) {
    std::vector<CPack> out;
    std::string err;
    if (s.commSource == 0 && !s.commLocal.empty()) {
        std::string root = localRoot(s.commLocal);
        json idx;
        if (readJson(P(root) / "index.json", idx) && idx.contains("packs")) {
            for (auto& m : idx["packs"]) out.push_back({m, m.value("path", "")});
        } else {
            std::error_code ec;
            for (auto& e : fs::recursive_directory_iterator(P(root) / "characters", ec)) {
                if (!e.is_regular_file() || e.path().filename() != "pack.json") continue;
                json m;
                if (!readJson(e.path(), m)) continue;
                m["path"] = fs::relative(e.path().parent_path(), P(root), ec).generic_string();
                out.push_back({m, m["path"]});
            }
        }
        if (out.empty()) err = "No packs found in " + root;
    } else if (s.commSource == 1) {
        if (s.commRepo.find('/') == std::string::npos) err = "Set the GitHub repo as owner/name.";
        else {
            std::string body;
            int c = httpGet(rawUrl(s.commRepo, "index", (s.commChannel == "testing" ? "testing" : "main") + std::string(".json")), body);
            if (c != 200) err = c ? "index.json: HTTP " + std::to_string(c) : "No network (offline): installed packs stay usable.";
            else {
                try { json idx = json::parse(body); for (auto& m : idx["packs"]) out.push_back({m, m.value("path", "")}); }
                catch (std::exception& e) { err = std::string("index.json: ") + e.what(); }
            }
        }
    } else err = "Set a local folder or a GitHub repo.";
    std::sort(out.begin(), out.end(), [](const CPack& a, const CPack& b) {
        return std::make_tuple(a.meta.value("character", ""), a.meta.value("slot", ""), a.key()) < std::make_tuple(b.meta.value("character", ""), b.meta.value("slot", ""), b.key());
    });
    {
        std::lock_guard<std::mutex> l(g_mx);
        g_packs = out;
        g_msg = err.empty() ? std::to_string(out.size()) + " pack(s) loaded." : err;
    }
    g_busy = false;
}

std::vector<std::string> validatePack(const fs::path& d, bool installCheck = false);
bool reOk(const std::string& s, const char* re);
std::string lower(std::string s);

void doInstall(Settings s, std::string instRoot, CPack p) {
    // index values become folder names and URLs: refuse anything that is not a plain author / name / version / path
    if (!reOk(p.meta.value("author", ""), R"(^[A-Za-z0-9-]{1,39}$)") || !reOk(p.meta.value("name", ""), R"(^[a-z0-9][a-z0-9-]{0,39}$)") ||
        !reOk(p.ver(), R"(^\d{1,4}\.\d{1,4}\.\d{1,4}$)") || !reOk(p.path, R"(^characters/[a-z0-9_]{1,40}/[A-Za-z0-9_.-]{1,80}/[A-Za-z0-9-]{1,39}__[a-z0-9-]{1,40}$)")) {
        setMsg("Install refused: the index entry has an invalid author, name, version or path.");
        g_busy = false;
        return;
    }
    std::string name = p.key() + "@" + p.ver();
    fs::path dst = P(instRoot) / name, tmp = P(instRoot) / (name + ".tmp");
    std::error_code ec;
    removeAll(tmp);
    fs::create_directories(tmp, ec);
    bool ok = true;
    std::string err;
    if (s.commSource == 0) {
        fs::path src = P(localRoot(s.commLocal)) / P(p.path);
        for (auto& f : kPackFiles) {   // only the known pack files, never the whole folder
            std::error_code e2;
            if (!fs::exists(src / f.name, e2)) { if (f.required) { ok = false; err = std::string(f.name) + " missing"; } continue; }
            if (fs::file_size(src / f.name, e2) > f.cap) { ok = false; err = std::string(f.name) + " is over its size limit"; continue; }
            fs::copy_file(src / f.name, tmp / f.name, fs::copy_options::overwrite_existing, e2);
            if (e2) { ok = false; err = e2.message(); }
        }
    } else {
        // files come from the fixed list only; the index must give a SHA-256 per file and the commit they were reviewed at
        json hashes = p.meta.value("sha256", json::object());
        std::string commit = p.meta.value("commit", "");
        if (!hashes.is_object() || hashes.empty() || !reOk(commit, "^[0-9a-f]{40}$")) {
            ok = false;
            err = "the index has no file hashes / commit for this pack (not built by the repo's CI)";
        }
        for (auto& f : kPackFiles) {
            if (!ok) break;
            std::string hv = hashes.is_object() ? hashes.value(f.name, "") : "";
            if (hv.empty()) { if (f.required) { ok = false; err = std::string(f.name) + ": no hash in the index"; } continue; }
            std::string body;
            int c = httpGet(rawUrl(s.commRepo, commit, p.path + "/" + f.name), body, f.cap);
            if (c == -2) { ok = false; err = std::string(f.name) + " is over its size limit"; break; }
            if (c != 200) { ok = false; err = std::string(f.name) + ": " + (c ? "HTTP " + std::to_string(c) : std::string("network error")); break; }
            if (sha256Hex(body) != lower(hv)) { ok = false; err = std::string(f.name) + ": content does not match the reviewed version (hash mismatch)"; break; }
            std::ofstream(tmp / f.name, std::ios::binary) << body;
        }
    }
    if (ok) {   // same checks as the repo CI, on what actually landed on disk
        auto ve = validatePack(tmp, true);
        if (!ve.empty()) { ok = false; err = "pack rejected: " + ve[0] + (ve.size() > 1 ? " (+" + std::to_string(ve.size() - 1) + " more)" : ""); }
    }
    if (ok) {
        removeAll(dst);
        fs::rename(tmp, dst, ec);
        if (ec) { ok = false; err = ec.message(); }
        else makeRO(dst);
    }
    if (!ok) removeAll(tmp);
    setMsg(ok ? "Installed " + name + " (read-only)." : "Install failed: " + err);
    g_busy = false;
}

ID3D11ShaderResourceView* previewTex(App& a, const std::string& dir, ImVec2& sz) {
    for (const char* f : {"preview.png", "preview.jpg"}) {
        std::string path = dir + "\\" + f;
        auto it = g_tex.find(path);
        if (it != g_tex.end()) { sz = g_texSz[path]; if (it->second) return it->second; continue; }
        std::error_code ec;
        if (!fs::exists(P(path), ec)) continue;
        if (fs::file_size(P(path), ec) > 300u * 1024 || !imageHeaderOk(path)) { g_tex[path] = nullptr; g_texSz[path] = ImVec2(0, 0); continue; }
        int w = 0, h = 0;
        ID3D11ShaderResourceView* t = a.gfx.loadTexture(path, true, &w, &h);
        g_tex[path] = t;
        g_texSz[path] = ImVec2((float)w, (float)h);
        sz = g_texSz[path];
        if (t) return t;
    }
    return nullptr;
}

void startRefresh(App& a) {
    if (g_busy) return;
    g_busy = true;
    g_loadedOnce = true;
    setMsg("Loading...");
    std::thread(doRefresh, a.set).detach();
}

void makeWorkingCopy(App& a, const CPack& p, const Inst& in) {
    fs::path dst = P(workRoot(a)) / p.key();
    std::error_code ec;
    if (fs::exists(dst, ec)) { setMsg("Working copy already exists: " + dst.string()); return; }
    fs::create_directories(dst.parent_path(), ec);
    fs::copy(P(in.dir), dst, fs::copy_options::recursive, ec);
    if (ec) { setMsg("Working copy failed: " + ec.message()); return; }
    for (auto& e : fs::recursive_directory_iterator(dst, ec)) fs::permissions(e.path(), fs::perms::owner_write, fs::perm_options::add, ec);
    json m;
    readJson(dst / "pack.json", m);
    std::string sha = p.meta.value("sha", "");
    m["based_on"] = p.key() + (sha.empty() ? "" : "@" + sha);
    m["based_on_version"] = in.ver;       // optional in pack.schema.json: the 3-way merge base of the sync
    m["status"] = "wip";
    std::ofstream(dst / "pack.json") << m.dump(1);
    setMsg("Working copy created: " + dst.string());
}

// ------------------------------------------------------------------------------------------------ milestone 2/3 helpers
// the slot of the loaded project: the hair component (mesh) name of the build
std::string currentSlot(const App& a) { return !a.pack.comp.empty() ? a.pack.comp : a.configName; }
std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }
bool reOk(const std::string& s, const char* re) { try { return std::regex_match(s, std::regex(re)); } catch (...) { return false; } }
const char* kVerRe = R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$)";
const char* kNameRe = R"(^[a-z0-9][a-z0-9-]{0,39}$)";
const char* kAuthorRe = R"(^[A-Za-z0-9-]{1,39}$)";

// local validation, mirrors community/tools/validate_pack.py + the two schemas (pack folder = <author>__<name>)
std::vector<std::string> validatePack(const fs::path& d, bool installCheck) {
    std::vector<std::string> e;
    json m, doc;
    if (!readJson(d / "pack.json", m) || !m.is_object()) { e.push_back("pack.json missing or unreadable"); return e; }
    for (const char* k : {"name", "author", "version", "status", "character", "slot", "base_build", "tags", "studio_version"})
        if (!m.contains(k)) e.push_back(std::string("pack.json: missing ") + k);
    static const std::set<std::string> allowed = {"name", "author", "version", "status", "character", "slot", "base_build", "tags", "studio_version", "description", "based_on", "based_on_version", "mesh_fingerprint"};
    for (auto& [k, v] : m.items()) if (!allowed.count(k)) e.push_back("pack.json: unknown field " + k);
    auto str = [&](const char* k) { return m.contains(k) && m[k].is_string() ? m[k].get<std::string>() : std::string(); };
    if (m.contains("name") && !reOk(str("name"), kNameRe)) e.push_back("pack.json: name must be lowercase letters, digits, '-' (max 40)");
    if (m.contains("author") && !reOk(str("author"), kAuthorRe)) e.push_back("pack.json: author must be a GitHub login");
    if (m.contains("version") && !reOk(str("version"), kVerRe)) e.push_back("pack.json: version must look like 1.2.3");
    if (m.contains("based_on_version") && !reOk(str("based_on_version"), kVerRe)) e.push_back("pack.json: based_on_version must look like 1.2.3");
    if (m.contains("status") && str("status") != "wip" && str("status") != "ready") e.push_back("pack.json: status must be wip or ready");
    if (m.contains("character") && !reOk(str("character"), "^[a-z0-9_]+$")) e.push_back("pack.json: character must be lowercase letters, digits, '_'");
    if (m.contains("based_on") && !reOk(str("based_on"), "^[A-Za-z0-9-]+__[a-z0-9-]+(@[0-9a-f]{7,40})?$")) e.push_back("pack.json: based_on is not <user>__<pack>[@sha]");
    if (m.contains("tags") && (!m["tags"].is_array() || m["tags"].size() > 12)) e.push_back("pack.json: tags must be a list of at most 12");
    if (m.contains("description") && str("description").size() > 500) e.push_back("pack.json: description over 500 characters");
    std::string fn = installCheck ? str("author") + "__" + str("name") : d.filename().string();
    auto us = fn.find("__");
    if (us == std::string::npos || lower(fn.substr(0, us)) != lower(str("author")) || fn.substr(us + 2) != str("name")) e.push_back("folder name must be <author>__<name> of pack.json");
    if (!readJson(d / "edits.hwgroom", doc) || !doc.is_object()) { e.push_back("edits.hwgroom missing or unreadable"); return e; }
    if (doc.value("format", "") != "hwgroom" || doc.value("version", 0) != 1) e.push_back("edits.hwgroom: format must be hwgroom, version 1");
    for (auto& [k, v] : doc.items()) if (k != "format" && k != "version" && k != "character" && k != "tol" && k != "params" && k != "ops") e.push_back("edits.hwgroom: unknown field " + k);
    if (doc.contains("tol") && !(doc["tol"].is_number() && doc["tol"].get<double>() > 0 && doc["tol"].get<double>() <= 0.05)) e.push_back("edits.hwgroom: tol out of range");
    if (!doc.contains("ops") || !doc["ops"].is_array()) e.push_back("edits.hwgroom: ops missing");
    else {
        if (doc["ops"].size() > 20000) e.push_back("edits.hwgroom: more than 20000 ops");
        int bad = 0;
        for (auto& o : doc["ops"]) {
            std::string t = cm::opType(o);
            bool ok = cm::opTypes().count(t) > 0;
            auto has = [&](const char* k) { return o.contains(k); };
            if (ok) {
                if (t == "translate") ok = has("roots") && has("offset");
                else if (t == "set_point") ok = has("root") && has("index") && has("pos");
                else if (t == "shorten") ok = has("roots") && has("keep") && o["keep"].is_number() && o["keep"].get<double>() > 0 && o["keep"].get<double>() <= 1;
                else if (t == "delete") ok = has("roots");
                else if (t == "duplicate") ok = has("root") && has("d");
                else if (t == "reshape") ok = has("root") && has("points") && o["points"].is_array() && o["points"].size() >= 2;
                else if (t == "mirror") ok = o.value("source", "") == "+x" || o.value("source", "") == "-x";
                else if (t == "delete_face") ok = has("roots") && o["roots"].is_array() && o["roots"].size() == 3 && o.value("face", -2) >= -1;
                else if (t == "rebuild") ok = (o.value("kind", "") == "from_cards" || o.value("kind", "") == "baked") && has("params") && o["params"].is_object() && o.contains("studio_version");
            }
            if (!ok) bad++;
        }
        if (bad) e.push_back("edits.hwgroom: " + std::to_string(bad) + " op(s) not valid for the schema");
        // every number finite and in a sane range (metres / factors / indices), nesting bounded
        std::function<bool(const json&, int)> sane = [&](const json& v, int depth) {
            if (depth > 8) return false;
            if (v.is_number_float()) { double x = v.get<double>(); return std::isfinite(x) && std::fabs(x) <= 1e6; }
            if (v.is_number_integer()) { long long x = v.get<long long>(); return x >= -1 && x <= 10000000; }
            if (v.is_string()) return v.get<std::string>().size() <= 256;
            if (v.is_array()) { if (v.size() > 100000) return false; for (auto& x : v) if (!sane(x, depth + 1)) return false; return true; }
            if (v.is_object()) { for (auto& [k, x] : v.items()) if (k.size() > 64 || !sane(x, depth + 1)) return false; return true; }
            return true;
        };
        if (!sane(doc, 0)) e.push_back("edits.hwgroom: values out of range (non-finite, huge or too deeply nested)");
        if (doc.contains("params")) {   // look overrides: name -> number / bool / short word / list of numbers / colour-vector object
            if (!doc["params"].is_object()) e.push_back("edits.hwgroom: params must be an object");
            else for (auto& [k, v] : doc["params"].items()) {
                bool okv = v.is_number() || v.is_boolean() || v.is_string();
                if (v.is_array()) { okv = v.size() <= 16; for (auto& x : v) if (!x.is_number()) okv = false; }
                if (v.is_object()) { okv = v.size() <= 4; for (auto& [k2, x] : v.items()) if (!x.is_number()) okv = false; }
                if (!okv) { e.push_back("edits.hwgroom: params value '" + k + "' is not a plain value"); break; }
            }
        }
        // rebuild settings: plain keys with numbers / booleans / short words only
        for (auto& o : doc["ops"]) {
            if (cm::opType(o) != "rebuild" || !o.contains("params") || !o["params"].is_object()) continue;
            if (o.value("kind", "") == "baked") {   // geometry snapshot: lists of numbers
                for (const char* k : {"guides", "uv", "face_uv", "faces"}) if (!o["params"].contains(k) || !o["params"][k].is_array()) e.push_back(std::string("edits.hwgroom: baked rebuild needs a '") + k + "' list");
                continue;
            }
            for (auto& [k, v] : o["params"].items()) {
                bool okv = reOk(k, "^[a-z0-9_]{1,40}$") && (v.is_number() || v.is_boolean() || (v.is_string() && reOk(v.get<std::string>(), "^[A-Za-z0-9_.-]{0,40}$")) ||
                           (v.is_array() && v.size() <= 16));
                if (v.is_array()) for (auto& x : v) if (!x.is_number()) okv = false;
                if (!okv) { e.push_back("edits.hwgroom: rebuild setting '" + k + "' is not a plain value"); break; }
            }
        }
        for (auto& o : doc["ops"])
            if (cm::opType(o) == "delete_face" && o.value("face", -1) >= 0 && !m.contains("mesh_fingerprint")) { e.push_back("pack.json: delete_face ops need mesh_fingerprint"); break; }
    }
    static const std::regex badName(R"(nvidia|gfsdk|gameworks|hairworks[_-]?sdk|nvhair)", std::regex::icase);
    static const std::set<std::string> badExt = {".w2mesh", ".w2ent", ".bundle", ".apb", ".dll", ".lib", ".exe", ".so", ".dylib"};
    size_t total = 0;
    std::error_code ec;
    for (auto& f : fs::directory_iterator(d, ec)) {
        if (!f.is_regular_file()) continue;
        std::string n = f.path().filename().string(), ext = lower(f.path().extension().string());
        size_t sz = (size_t)f.file_size(ec);
        total += sz;
        if (badExt.count(ext) || std::regex_search(n, badName)) e.push_back(n + ": forbidden file");
        if (installCheck && !allowedFile(n)) e.push_back(n + ": not a pack file");
        if (n.rfind("preview", 0) == 0 && sz > 300 * 1024) e.push_back(n + ": preview over 300KB");
        if (n.rfind("preview", 0) == 0 && !imageHeaderOk(f.path().u8string())) e.push_back(n + ": not a valid PNG / JPEG (or over 4096 px)");
        if (ext == ".json" || ext == ".hwgroom" || ext == ".md" || ext == ".txt") {
            std::ifstream i(f.path(), std::ios::binary);
            std::string txt((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
            if (txt.find("GFSDK") != std::string::npos || txt.find("NvHair") != std::string::npos || txt.find("NvFoundation") != std::string::npos) e.push_back(n + ": SDK identifiers found");
        }
    }
    if (total > 2u * 1024 * 1024) e.push_back("pack over 2MB");
    return e;
}

bool readOps(const fs::path& dir, std::vector<json>& ops, json* docOut = nullptr) {
    json doc;
    if (!readJson(dir / "edits.hwgroom", doc) || !doc.contains("ops") || !doc["ops"].is_array()) return false;
    ops.clear();
    for (auto& o : doc["ops"]) ops.push_back(cm::opTypes().count(cm::opType(o)) ? cm::normOp(o) : o);
    if (docOut) *docOut = doc;
    return true;
}

// ---- apply a pack (installed or working copy) onto the loaded project
void applyPack(App& a, const std::string& dir) {
    json meta;
    std::vector<json> hw;
    json hwDoc;
    if (!readJson(P(dir) / "pack.json", meta) || !readOps(P(dir), hw, &hwDoc)) { setMsg("Apply: pack.json / edits.hwgroom unreadable in " + dir); return; }
    if (!a.loaded) { setMsg("Apply: open the character's build first (File > Open)."); return; }
    std::string ch = meta.value("character", ""), sl = meta.value("slot", "");
    if (lower(ch) != lower(a.pack.character) || lower(sl) != lower(currentSlot(a))) {
        setMsg("Not applied: the pack is for " + ch + " / " + sl + ", the loaded project is " + a.pack.character + " / " + currentSlot(a) + ".");
        return;
    }
    int skipped = 0;
    auto ops = cm::toStudio(hw, &skipped);
    if (ops.empty()) { setMsg("Nothing to apply: the pack has no edit ops."); return; }
    // delete_face ops carry base-mesh face indices: refuse when the base groom is not the one the author edited
    bool needFp = false, hasRebuild = false;
    for (auto& o : ops) {
        if (o.value("op", "") == "delete_face" && o.value("face", -1) >= 0) needFp = true;
        if (o.value("op", "") == "rebuild_param") hasRebuild = true;
    }
    if (needFp) {
        json fp = meta.value("mesh_fingerprint", json());
        if (!fp.is_object() || fp.value("guides", -1) != (int)a.pack.guidesEdit.size() || fp.value("faces", -1) != (int)a.pack.F.size()) {
            setMsg("Not applied: the pack's face edits are for a base groom with " + (fp.is_object() ? std::to_string(fp.value("guides", -1)) + " guides / " + std::to_string(fp.value("faces", -1)) : std::string("?")) +
                   " faces, this one has " + std::to_string(a.pack.guidesEdit.size()) + " guides / " + std::to_string(a.pack.F.size()) + " (different base build).");
            return;
        }
        for (auto& o : ops) {
            if (o.value("op", "") != "delete_face" || o.value("face", -1) < 0) continue;
            int fi = o["face"].get<int>();
            bool bad = fi >= (int)a.pack.F.size();
            if (!bad) {   // the indexed face must sit on the stated roots
                for (int c = 0; c < 3 && !bad; c++) {
                    V3 r0 = a.pack.guidesEdit[a.pack.F[fi][c]][0];
                    float best = 1e9f;
                    for (auto& r : o["roots"]) best = std::min(best, len(V3{r[0].get<float>(), r[1].get<float>(), r[2].get<float>()} - r0));
                    bad = best > 0.004f;
                }
            }
            if (bad) { setMsg("Not applied: face index " + std::to_string(fi) + " does not match its guide roots (different base build)."); return; }
        }
    }
    std::string warn;
    if (hasRebuild) {
        bool cards = false;
        for (auto& m : a.pack.meshes) if (m.kind == "hair_cards") cards = true;
        if (!cards) { setMsg("Not applied: the pack rebuilds the hair from the vanilla cards and this project has none (re-run desk_prep)."); return; }
        if (meta.value("studio_version", "") != GSD_VERSION)
            warn = std::string("; WARNING: made with Studio ") + meta.value("studio_version", "?") + ", this is " + GSD_VERSION + " - the rebuild may differ";
    }
    static int grpCounter = 0;
    int grp = 700000 + (++grpCounter);          // one undo step for the whole pack
    int unmatchedBefore = a.groom.unmatched;
    a.groom.redo.clear();
    int nApplied = 0;
    std::string err;
    for (auto& o : ops) {
        if (o.value("op", "") == "rebuild_param") {   // replayed by this Studio's own algorithm on the user's own game files
            a.groom.replay();
            json c = o["params"];
            c["do"] = "rebuild_from_cards";
            bool quit = false;
            size_t before = a.groom.ops.size();
            json res = a.serveCommand(c, "", quit);
            for (size_t i = before; i < a.groom.ops.size(); i++) a.groom.ops[i]["grp"] = grp;
            if (res.contains("error")) { err = res["error"].is_string() ? res["error"].get<std::string>() : "rebuild failed"; break; }
        } else {
            o["grp"] = grp;
            a.groom.ops.push_back(o);
        }
        nApplied++;
    }
    a.groom.replay();
    int nLook = 0;
    if (hwDoc.contains("params") && hwDoc["params"].is_object()) {   // look / material overrides of the pack
        for (auto& [k, v] : hwDoc["params"].items()) {
            if (v.is_number()) a.look.setF(k, v.get<float>()); else a.look.setJson(k, v);
            nLook++;
        }
        a.look.enforceCaps();
        a.computeBudget();
    }
    a.editsChanged();
    if (nLook) warn += "; " + std::to_string(nLook) + " look value(s) set";
    setMsg("Applied " + std::to_string(nApplied) + " of " + std::to_string(ops.size()) + " op(s) from " + meta.value("name", "?") + " (undo: Ctrl+Z)" +
           (skipped ? "; " + std::to_string(skipped) + " unsupported op(s) skipped" : "") + warn +
           (err.empty() ? "" : "; STOPPED at a rebuild: " + err) +
           (a.groom.unmatched > unmatchedBefore ? "; " + std::to_string(a.groom.unmatched - unmatchedBefore) + " guide(s) not found (different base build?)" : "") + ".");
}

// ---- export the current project as a pack
struct ExportForm {
    char name[48] = "", author[48] = "", version[24] = "0.1.0", tags[160] = "", desc[512] = "", slot[96] = "";
    int status = 0;
    std::string dest;      // folder being written (waiting for the viewport preview)
    bool waiting = false, inited = false;
} g_ef;

void exportPack(App& a) {
    if (!a.loaded) { setMsg("Export: open a build first."); return; }
    ExportForm& f = g_ef;
    std::string name = f.name, author = f.author, ver = f.version, slot = f.slot, ch = a.pack.character;
    if (!reOk(name, kNameRe)) { setMsg("Export: name must be lowercase letters, digits and '-'."); return; }
    if (!reOk(author, kAuthorRe)) { setMsg("Export: author must be your GitHub login."); return; }
    if (!reOk(ver, kVerRe)) { setMsg("Export: version must look like 0.1.0."); return; }
    if (slot.empty() || slot.find_first_of("/\\") != std::string::npos) { setMsg("Export: the slot is empty or has a slash."); return; }
    if (!reOk(ch, "^[a-z0-9_]+$")) { setMsg("Export: the character name \"" + ch + "\" is not a valid pack character."); return; }
    json ed = a.groom.editsDoc(true);
    int skipped = 0;
    cm::Ctx ctx;
    ctx.faceIndex = [&](const json& roots) {   // index of the base-mesh face sitting on these 3 roots, -1 = not a base face
        auto nearTo = [&](const json& r, int g) { return len(V3{r[0].get<float>(), r[1].get<float>(), r[2].get<float>()} - a.pack.guidesEdit[g][0]) < 0.0005f; };
        for (size_t f = 0; f < a.pack.F.size(); f++) {
            bool all = true;
            for (int c = 0; c < 3 && all; c++) {
                bool any = false;
                for (auto& r : roots) any = any || nearTo(r, a.pack.F[f][c]);
                all = any;
            }
            if (all) return (int)f;
        }
        return -1;
    };
    auto hw = cm::toHwgroom(ed["ops"].get<std::vector<json>>(), &skipped, &ctx);
    if (hw.empty()) { setMsg("Export: the project has no shareable edit ops."); return; }
    fs::path dest = P(workRoot(a)) / (author + "__" + name);
    std::error_code ec;
    fs::create_directories(dest, ec);
    clearRO(dest);
    json old;
    readJson(dest / "pack.json", old);
    json doc = {{"format", "hwgroom"}, {"version", 1}, {"character", ch}, {"tol", ed.value("tol", 0.004)}, {"params", json::object()}, {"ops", hw}};
    json tags = json::array();
    std::istringstream ts(f.tags);
    for (std::string t; std::getline(ts, t, ',');) {
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (!t.empty() && tags.size() < 12) tags.push_back(t);
    }
    json m = {{"name", name}, {"author", author}, {"version", ver}, {"status", f.status ? "ready" : "wip"}, {"character", ch}, {"slot", slot},
              {"base_build", a.pack.config}, {"tags", tags}, {"studio_version", GSD_VERSION}};
    for (auto& o : hw) if (o.value("type", "") == "delete_face" && o.value("face", -1) >= 0) { m["mesh_fingerprint"] = {{"guides", (int)a.pack.guidesEdit.size()}, {"faces", (int)a.pack.F.size()}}; break; }
    if (strlen(f.desc)) m["description"] = std::string(f.desc).substr(0, 500);
    for (const char* k : {"based_on", "based_on_version"}) if (old.contains(k)) m[k] = old[k];
    std::ofstream(dest / "edits.hwgroom", std::ios::binary) << doc.dump(1);
    std::ofstream(dest / "pack.json", std::ios::binary) << m.dump(1);
    f.dest = dest.string();
    f.waiting = true;
    a.previewOk = -1;
    for (const char* f : {"preview.png", "preview.jpg"}) {   // the cached thumbnail is stale after a re-export
        std::string k = dest.string() + "\\" + f;
        auto it = g_tex.find(k);
        if (it != g_tex.end()) { g_tex.erase(it); g_texSz.erase(k); }   // not released: this frame may still draw it
    }
    a.pendingPreview = (dest / "preview.jpg").string();
    setMsg("Exporting " + std::to_string(hw.size()) + " op(s)" + (skipped ? " (" + std::to_string(skipped) + " unsupported skipped)" : "") + "; capturing the preview...");
}
void finishExport(App& a) {
    if (!g_ef.waiting || a.previewOk < 0) return;
    g_ef.waiting = false;
    std::string d = g_ef.dest;
    if (a.previewOk == 1) { std::error_code ec; fs::remove(P(d) / "preview.png", ec); }
    auto errs = validatePack(P(d));
    if (a.previewOk != 1) errs.insert(errs.begin(), "preview.jpg could not be captured");
    if (errs.empty()) setMsg("Exported to " + d);
    else { std::string s = "Exported to " + d + " but: "; for (auto& e : errs) s += e + "; "; setMsg(s); }
    a.previewOk = -1;
}

// ---- sync a working copy with a newer installed upstream version
struct SyncState {
    bool askOpen = false, popupShown = false;
    std::string workDir, key, theirsDir, theirsVer, theirsSha;
    json workDoc, workMeta;
    cm::MergeResult R;
    std::vector<int> choice;
} g_sync;

std::string stamp() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    tm tmv;
    localtime_s(&tmv, &t);
    char b[32];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tmv);
    return b;
}
std::string basedKey(const json& m) { std::string b = m.value("based_on", ""); return b.substr(0, b.find('@')); }

bool upstreamNewer(const App& a, const json& m, Inst* out = nullptr) {
    std::string key = basedKey(m), bv = m.value("based_on_version", "");
    if (key.empty() || bv.empty()) return false;
    const Inst* n = newestInst(installedOf(a, key));
    if (!n || cmpVer(n->ver, bv) <= 0) return false;
    if (out) *out = *n;
    return true;
}

void finishSync(App& a) {
    SyncState& S = g_sync;
    auto ops = cm::resolve(S.R.merged, S.R.conflicts, S.choice);
    fs::path work = P(S.workDir);
    std::error_code ec;
    fs::path bak = P(installRoot(a)) / "_backup" / S.key / stamp();
    fs::create_directories(bak, ec);
    fs::copy(work, bak, fs::copy_options::recursive, ec);
    if (ec) { setMsg("Sync stopped: backup failed (" + ec.message() + ")"); S.askOpen = false; return; }
    json doc = S.workDoc;
    doc["ops"] = ops;
    json m = S.workMeta;
    std::string sha = S.theirsSha;
    {   // sha of the version from the loaded index, when known
        std::lock_guard<std::mutex> l(g_mx);
        for (auto& p : g_packs) if (p.key() == S.key && p.ver() == S.theirsVer) sha = p.meta.value("sha", sha);
    }
    m["based_on"] = S.key + (sha.empty() ? "" : "@" + sha);
    m["based_on_version"] = S.theirsVer;
    clearRO(work);
    std::ofstream(work / "edits.hwgroom", std::ios::binary) << doc.dump(1);
    std::ofstream(work / "pack.json", std::ios::binary) << m.dump(1);
    setMsg("Synced " + work.filename().string() + " with v" + S.theirsVer + ": " + std::to_string(ops.size()) + " op(s), " + std::to_string(S.R.conflicts.size()) + " conflict(s) resolved. Backup: " + bak.string());
    S.askOpen = false;
}

void startSync(App& a, const std::string& workDir) {
    SyncState& S = g_sync;
    S = SyncState();
    json m;
    if (!readJson(P(workDir) / "pack.json", m)) { setMsg("Sync: pack.json unreadable."); return; }
    Inst th;
    if (!upstreamNewer(a, m, &th)) { setMsg("Sync: no newer installed upstream version (install the update first)."); return; }
    std::string key = basedKey(m), bv = m.value("based_on_version", "");
    std::vector<json> base, theirs, mine;
    if (!readOps(P(installRoot(a)) / (key + "@" + bv), base)) { setMsg("Sync: the version this copy was based on (" + key + "@" + bv + ") is no longer installed; reinstall it to merge."); return; }
    if (!readOps(P(th.dir), theirs)) { setMsg("Sync: the new upstream version is unreadable."); return; }
    if (!readOps(P(workDir), mine, &S.workDoc)) { setMsg("Sync: the working copy's edits.hwgroom is unreadable."); return; }
    S.workDir = workDir; S.key = key; S.theirsDir = th.dir; S.theirsVer = th.ver; S.workMeta = m;
    S.R = cm::merge3(base, theirs, mine);
    S.choice.assign(S.R.conflicts.size(), 0);
    if (S.R.conflicts.empty()) { finishSync(a); return; }
    S.askOpen = true;
    S.popupShown = false;
    setMsg("Sync: " + std::to_string(S.R.conflicts.size()) + " conflict(s) to resolve.");
}

std::string opSummary(const json& o) {
    std::string t = cm::opType(o);
    size_t n = o.contains("roots") ? o["roots"].size() : 1;
    std::string s = t + " (" + std::to_string(n) + " guide" + (n == 1 ? "" : "s") + ")";
    for (const char* k : {"offset", "d", "keep", "pos", "falloff"}) if (o.contains(k)) s += " " + std::string(k) + "=" + o[k].dump();
    return s.substr(0, 160);
}

void drawSyncPopup(App& a) {
    SyncState& SY = g_sync;
    if (!SY.askOpen) return;
    if (!SY.popupShown) { ImGui::OpenPopup("Sync conflicts"); SY.popupShown = true; }
    ImGui::SetNextWindowSize(ImVec2(S(820), S(560)), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal("Sync conflicts", &open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s: your copy and upstream v%s changed the same guides in %d place(s). Non-overlapping changes are merged already. A backup of your copy is written before anything changes.",
                           SY.key.c_str(), SY.theirsVer.c_str(), (int)SY.R.conflicts.size());
        ImGui::BeginChild("##conf", ImVec2(0, -S(40)), true);
        for (size_t i = 0; i < SY.R.conflicts.size(); i++) {
            auto& c = SY.R.conflicts[i];
            ImGui::PushID((int)i);
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Conflict %d: %s", (int)i + 1, c.reason.c_str());
            ImGui::TextDisabled("theirs: %s", (c.theirsAdd ? opSummary(c.theirs[0]) : "removed " + opSummary(c.theirs[0])).c_str());
            for (size_t m = 0; m < c.mine.size(); m++) ImGui::TextDisabled("mine:   %s%s", c.mineRemoved[m] ? "removed " : "", opSummary(c.mine[m]).c_str());
            ImGui::RadioButton("Keep mine", &SY.choice[i], 0); ImGui::SameLine();
            ImGui::RadioButton("Take theirs", &SY.choice[i], 1); ImGui::SameLine();
            ImGui::RadioButton("Keep both", &SY.choice[i], 2);
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (ImGui::Button("Merge")) { finishSync(a); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || !open) { SY.askOpen = false; setMsg("Sync cancelled; nothing changed."); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// working copies and own exports: <workRoot>/<author>__<name>
std::vector<std::string> workDirs(const App& a) {
    std::vector<std::string> r;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(P(workRoot(a)), ec)) if (e.is_directory() && fs::exists(e.path() / "pack.json", ec)) r.push_back(e.path().string());
    std::sort(r.begin(), r.end());
    return r;
}
void openFolder(const std::string& p) { ShellExecuteW(nullptr, L"open", wide(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

bool g_goGithub = false, g_ghChecked = false;
std::string g_pubSel;
size_t g_logCount = 0;
}  // namespace

// ------------------------------------------------------------------------------------------------ Community window UI
// Presentation only ("Refined Ink & Iron"): every action below calls the same functions as before.
namespace {
int g_tab = 0;                       // 0 Browse, 1 My packs, 2 GitHub
bool g_srcOpen = false, g_exportOpen = false, g_logOpen = false;
std::string g_charSel;               // character filter chip ("" = All)
const unsigned kSteel = 0x6f9fd8;

float txtW(ImFont* f, const char* t) { return w::textSize(f, t).x; }
void txt(ImDrawList* d, ImFont* f, ImVec2 p, ImU32 c, const std::string& t) { d->AddText(f, f->FontSize, p, c, t.c_str()); }

std::string fitText(ImFont* f, std::string t, float wd, bool force = false) {
    if (!force && txtW(f, t.c_str()) <= wd) return t;
    while (!t.empty() && txtW(f, (t + "...").c_str()) > wd) {
        t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
        if (!t.empty() && (unsigned char)t.back() >= 0xC0) t.pop_back();
    }
    return t + "...";
}

std::vector<std::string> wrapLines(ImFont* f, const std::string& text, float wd, int maxLines) {
    std::vector<std::string> lines;
    std::istringstream ss(text);
    std::string word, cur;
    while (ss >> word) {
        std::string t = cur.empty() ? word : cur + " " + word;
        if (cur.empty() || txtW(f, t.c_str()) <= wd) cur = t;
        else { lines.push_back(cur); cur = word; }
    }
    if (!cur.empty()) lines.push_back(cur);
    bool cut = (int)lines.size() > maxLines;
    if (cut) lines.resize(maxLines);
    for (size_t i = 0; i < lines.size(); i++) lines[i] = fitText(f, lines[i], wd, cut && i + 1 == lines.size());
    return lines;
}

float chip(ImDrawList* d, ImVec2 p, const std::string& t, unsigned col, ImFont* f = g_fontBadge) {
    ImVec2 ts = w::textSize(f, t.c_str());
    float wd = ts.x + S(14), h = S(18);
    d->AddRectFilled(p, ImVec2(p.x + wd, p.y + h), tc(col, 0.14f), S(9));
    d->AddRect(p, ImVec2(p.x + wd, p.y + h), tc(col, 0.5f), S(9));
    txt(d, f, ImVec2(p.x + S(7), p.y + (h - ts.y) * 0.5f), tc(col), t);
    return wd;
}
unsigned statusCol(const std::string& s) { return s == "ready" ? tok::OK : tok::GLOW; }

bool looksBad(const std::string& m) {
    std::string l = lower(m);
    for (const char* k : {"fail", "not applied", "no packs", "set a ", "set the", "offline", "http", "error", "nothing", "refused", "unreadable", "exists", "apply:", "not loaded", "missing"})
        if (l.find(k) != std::string::npos) return true;
    return false;
}

// status banner with icon (working / ok / warn)
void banner(const std::string& msg, bool busy) {
    bool bad = !busy && looksBad(msg);
    unsigned col = busy ? kSteel : bad ? tok::GLOW : tok::OK;
    const char* gl = busy ? ICON_LC_REFRESH_CW : bad ? ICON_LC_TRIANGLE_ALERT : ICON_LC_CIRCLE_CHECK;
    float W = ImGui::GetContentRegionAvail().x, wrapW = W - S(48);
    ImFont* f = g_fontUi;
    ImGui::PushFont(f);
    ImVec2 ts = ImGui::CalcTextSize(msg.c_str(), nullptr, false, wrapW);
    ImGui::PopFont();
    float h = (std::max)(S(34), ts.y + S(16));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + W, p.y + h), tc(col, 0.09f), S(6));
    d->AddRect(p, ImVec2(p.x + W, p.y + h), tc(col, 0.4f), S(6));
    d->AddRectFilled(p, ImVec2(p.x + S(3), p.y + h), tc(col), S(2));
    w::icon(d, gl, ImVec2(p.x + S(20), p.y + h * 0.5f), S(15), tc(col));
    d->AddText(f, f->FontSize, ImVec2(p.x + S(38), p.y + (h - ts.y) * 0.5f), tc(tok::SOFT), msg.c_str(), nullptr, wrapW);
    ImGui::Dummy(ImVec2(W, h));
}

struct Card {
    ImDrawList* d = nullptr;
    ImVec2 p;
    float W = 0;
    unsigned fill = tok::BG2;
    void begin(unsigned fillCol = tok::BG2) {
        fill = fillCol;
        p = ImGui::GetCursorScreenPos();
        W = ImGui::GetContentRegionAvail().x;
        d = ImGui::GetWindowDrawList();
        d->ChannelsSplit(2);
        d->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::Indent(S(14));
    }
    float inner() const { return W - S(28); }
    void end() {
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::Unindent(S(14));
        ImGui::EndGroup();
        float y = ImGui::GetItemRectMax().y;
        d->ChannelsSetCurrent(0);
        d->AddRectFilled(p, ImVec2(p.x + W, y), tc(fill), S(8));
        d->AddRect(p, ImVec2(p.x + W, y), tc(tok::IRON2), S(8));
        d->ChannelsMerge();
        ImGui::Dummy(ImVec2(0, S(6)));
    }
};

void rowLabel(const char* l, float labelW) {
    float x0 = ImGui::GetCursorScreenPos().x;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, tv(tok::MUTED));
    ImGui::TextUnformatted(l);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(x0 + labelW, ImGui::GetCursorScreenPos().y));
}

float btnW(const char* t, const char* glyph) { return ImGui::CalcTextSize(t).x + (glyph ? S(23) : 0) + S(24); }
float pbtnW(const char* t, const char* glyph) { return txtW(g_fontUiSemi, t) + (glyph ? S(22) : 0) + S(24); }
// put the next button on the same line when it fits, else wrap
void flow(float wdt, float xmax) {
    if (ImGui::GetItemRectMax().x + S(8) + wdt <= xmax) ImGui::SameLine(0, S(8));
}
bool steelButton(const char* label, const char* text, ImVec2 size, bool enabled, const char* glyph) {
    w::g_primaryAlt = kSteel;
    bool r = w::primaryButton(label, text, size, enabled, glyph);
    w::g_primaryAlt = 0;
    return r;
}

bool emptyState(const char* glyph, const char* text, const char* btnLabel, const char* btnText) {
    float W = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0, S(30)));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* d = ImGui::GetWindowDrawList();
    w::icon(d, glyph, ImVec2(p.x + W * 0.5f, p.y + S(22)), S(40), tc(tok::IRON3));
    ImGui::Dummy(ImVec2(W, S(52)));
    auto lines = wrapLines(g_fontUi, text, (std::min)(W - S(40), S(460)), 4);
    for (auto& l : lines) {
        ImVec2 q = ImGui::GetCursorScreenPos();
        txt(d, g_fontUi, ImVec2(q.x + (W - txtW(g_fontUi, l.c_str())) * 0.5f, q.y), tc(tok::MUTED), l);
        ImGui::Dummy(ImVec2(W, S(19)));
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    float bw = btnW(btnText, ICON_LC_REFRESH_CW);
    ImGui::SetCursorScreenPos(ImVec2(p.x + (W - bw) * 0.5f, ImGui::GetCursorScreenPos().y));
    bool r = w::button(btnLabel, btnText, ImVec2(bw, S(30)), ICON_LC_CHEVRON_RIGHT);
    return r;
}

void prStateChip(ImDrawList* d, ImVec2 p, const std::string& t) {
    std::string l = lower(t);
    bool merged = l.find("merged") != std::string::npos, closed = l.find("closed") != std::string::npos, opn = l.find("open") != std::string::npos;
    chip(d, p, merged ? "MERGED" : closed ? "CLOSED" : opn ? "OPEN" : "UNKNOWN", merged ? tok::OK : closed ? tok::WARN : opn ? kSteel : tok::MUTED);
}

// ---- header + source settings
void drawHeader(App& a, Settings& s, bool& changed) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImGui::PushFont(g_fontSection);
    ImGui::TextUnformatted("COMMUNITY");
    ImGui::PopFont();
    ImGui::PushFont(g_fontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, tv(tok::MUTED));
    ImGui::TextUnformatted("Shared hair packs");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    float endY = ImGui::GetCursorScreenPos().y;
    std::string src = s.commSource == 0 ? "Local folder" : (s.commRepo.empty() ? "GitHub repo" : s.commRepo + " / " + (s.commChannel.empty() ? "main" : s.commChannel));
    src = fitText(g_fontUi, src, (std::min)(S(300), W * 0.4f));
    float cw = btnW(src.c_str(), ICON_LC_SETTINGS), rw = S(30);
    float y = p.y + (endY - p.y - S(30)) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(p.x + W - rw, y));
    if (w::iconButton("Refresh##comm", ICON_LC_REFRESH_CW, 30, "Refresh\nReload the pack list from the source.")) startRefresh(a);
    ImGui::SetCursorScreenPos(ImVec2(p.x + W - rw - S(8) - cw, y));
    if (w::button("Source##chip", src.c_str(), ImVec2(cw, S(30)), ICON_LC_SETTINGS, !g_srcOpen)) g_srcOpen = !g_srcOpen;
    w::tip("Source\nWhere packs come from: a local folder or a GitHub repo.");
    ImGui::SetCursorScreenPos(ImVec2(p.x, endY));
    ImGui::Dummy(ImVec2(W, S(4)));
    d->AddLine(ImVec2(p.x, ImGui::GetCursorScreenPos().y), ImVec2(p.x + W, ImGui::GetCursorScreenPos().y), tc(tok::IRON));
    ImGui::Dummy(ImVec2(W, S(8)));

    if (g_srcOpen) {
        Card c;
        c.begin();
        w::pushInputStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(6)));
        const char* srcN[] = {"Local folder", "GitHub repo"};
        float lw = S(84);
        char buf[512];
        rowLabel("Source", lw);
        ImGui::SetNextItemWidth(S(180));
        if (ImGui::Combo("##src", &s.commSource, srcN, 2)) changed = true;
        if (s.commSource == 0) {
            snprintf(buf, sizeof buf, "%s", s.commLocal.c_str());
            rowLabel("Folder", lw);
            float bw = btnW("Use repo community", nullptr);
            ImGui::SetNextItemWidth((std::max)(S(120), c.inner() - lw - bw - S(8)));
            if (ImGui::InputText("##folder", buf, sizeof buf)) { s.commLocal = buf; changed = true; }
            ImGui::SameLine(0, S(8));
            if (w::button("Use repo community", "Use repo community", ImVec2(bw, ImGui::GetFrameHeight()))) { s.commLocal = s.repo + "\\community"; changed = true; }
        } else {
            rowLabel("Repo", lw);
            ImGui::TextUnformatted(kCommunityRepo);   // fixed: the official community repo
            int ch = s.commChannel == "testing" ? 1 : 0;
            const char* chN[] = {"main", "testing"};
            rowLabel("Channel", lw);
            ImGui::SetNextItemWidth(S(180));
            if (ImGui::Combo("##channel", &ch, chN, 2)) { s.commChannel = chN[ch]; changed = true; }
        }
        ImGui::PopStyleVar();
        w::popInputStyle();
        c.end();
    }
}

// ---- tab strip
void drawTabs(const int counts[3]) {
    const char* names[3] = {"Browse", "My packs", "GitHub"};
    const char* ids[3] = {"Browse##ctab", "My packs##ctab", "GitHub##ctab"};
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x, x = p0.x, h = S(32);
    ImDrawList* d = ImGui::GetWindowDrawList();
    for (int i = 0; i < 3; i++) {
        std::string cnt = counts[i] >= 0 ? std::to_string(counts[i]) : "";
        float tw = txtW(g_fontUiSemi, names[i]) + (cnt.empty() ? 0 : S(8) + txtW(g_fontSmall, cnt.c_str()) + S(12)) + S(28);
        ImGui::SetCursorScreenPos(ImVec2(x, p0.y));
        if (ImGui::InvisibleButton(ids[i], ImVec2(tw, h))) g_tab = i;
        bool hov = ImGui::IsItemHovered(), on = g_tab == i;
        ImU32 col = tc(on ? tok::TEXT : hov ? tok::SOFT : tok::MUTED);
        float tx = x + S(14);
        txt(d, g_fontUiSemi, ImVec2(tx, p0.y + (h - S(15)) * 0.5f), col, names[i]);
        if (!cnt.empty()) {
            float cx = tx + txtW(g_fontUiSemi, names[i]) + S(8), cwid = txtW(g_fontSmall, cnt.c_str()) + S(12);
            d->AddRectFilled(ImVec2(cx, p0.y + (h - S(17)) * 0.5f), ImVec2(cx + cwid, p0.y + (h + S(17)) * 0.5f), on ? tc(kSteel, 0.25f) : tc(tok::BG3), S(8.5f));
            txt(d, g_fontSmall, ImVec2(cx + S(6), p0.y + (h - S(13)) * 0.5f), tc(on ? kSteel : tok::MUTED), cnt);
        }
        if (on) d->AddRectFilled(ImVec2(x + S(8), p0.y + h - S(2)), ImVec2(x + tw - S(8), p0.y + h), tc(kSteel), S(1));
        x += tw;
    }
    d->AddLine(ImVec2(p0.x, p0.y + h), ImVec2(p0.x + W, p0.y + h), tc(tok::IRON));
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(W, h + S(10)));
}

// ---- Browse
void drawBrowse(App& a, Settings& s, const std::vector<CPack>& packs) {
    {   // search
        float W = ImGui::GetContentRegionAvail().x;
        ImVec2 p = ImGui::GetCursorScreenPos();
        w::pushInputStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(36), S(7)));
        ImGui::SetNextItemWidth(W);
        ImGui::InputTextWithHint("##cfilter", "Search name, author, tag or character", g_filter, sizeof g_filter);
        ImGui::PopStyleVar();
        w::popInputStyle();
        w::icon(ImGui::GetWindowDrawList(), ICON_LC_SEARCH, ImVec2(p.x + S(18), p.y + ImGui::GetFrameHeight() * 0.5f), S(15), tc(tok::MUTED));
    }
    std::string f = g_filter;
    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    std::vector<std::string> chars;
    for (auto& p : packs) {
        std::string c = p.meta.value("character", "?");
        if (std::find(chars.begin(), chars.end(), c) == chars.end()) chars.push_back(c);
    }
    if (!g_charSel.empty() && std::find(chars.begin(), chars.end(), g_charSel) == chars.end()) g_charSel.clear();
    if (chars.size() > 1) {   // character filter chips
        ImGui::Dummy(ImVec2(0, S(2)));
        float xmax = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        std::vector<std::string> all = {""};
        for (auto& c : chars) all.push_back(c);
        for (size_t i = 0; i < all.size(); i++) {
            std::string t = all[i].empty() ? "All" : all[i];
            float cw = txtW(g_fontUiSemi, t.c_str()) + S(24);
            if (i && ImGui::GetItemRectMax().x + S(6) + cw <= xmax) ImGui::SameLine(0, S(6));
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton((t + "##chfilter").c_str(), ImVec2(cw, S(26)))) g_charSel = all[i];
            bool on = g_charSel == all[i], hov = ImGui::IsItemHovered();
            ImDrawList* d = ImGui::GetWindowDrawList();
            d->AddRectFilled(p, ImVec2(p.x + cw, p.y + S(26)), on ? tc(kSteel, 0.2f) : tc(hov ? tok::BG3 : tok::BG2), S(13));
            d->AddRect(p, ImVec2(p.x + cw, p.y + S(26)), on ? tc(kSteel, 0.7f) : tc(tok::IRON2), S(13));
            txt(d, g_fontUiSemi, ImVec2(p.x + S(12), p.y + (S(26) - S(15)) * 0.5f), tc(on ? kSteel : tok::SOFT), t);
        }
    }
    ImGui::Dummy(ImVec2(0, S(4)));

    std::vector<const CPack*> list;
    for (auto& p : packs) {
        std::string hay = p.meta.value("name", "") + " " + p.meta.value("author", "") + " " + p.meta.value("character", "") + " " + p.meta.value("slot", "") + " " + p.meta.value("description", "");
        for (auto& t : p.meta.value("tags", json::array())) hay += " " + t.get<std::string>();
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        if (!f.empty() && hay.find(f) == std::string::npos) continue;
        if (!g_charSel.empty() && p.meta.value("character", "?") != g_charSel) continue;
        list.push_back(&p);
    }
    ImGui::BeginChild("##clist", ImVec2(0, 0), false);
    if (list.empty()) {
        if (packs.empty()) {
            if (emptyState(ICON_LC_PACKAGE, "No packs yet. Pick a source with the gear button above, then refresh.", "Refresh##empty", "Refresh")) startRefresh(a);
        } else if (emptyState(ICON_LC_SEARCH, "No pack matches this search.", "Clear filters##empty", "Clear filters")) { g_filter[0] = 0; g_charSel.clear(); }
        ImGui::EndChild();
        return;
    }
    const float gap = S(14), minW = S(290), ch = S(346), pv = S(140);
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = (std::max)(1, (int)((avail + gap) / (minW + gap)));
    float cw = (avail - (cols - 1) * gap) / cols;
    ImDrawList* d = ImGui::GetWindowDrawList();
    std::string lastChar;
    int col = 0, id = 0;
    ImVec2 rowO;
    auto endRow = [&]() {
        if (col == 0) return;
        ImGui::SetCursorScreenPos(rowO);
        ImGui::Dummy(ImVec2(avail, ch + gap));
        col = 0;
    };
    for (auto* pp : list) {
        const CPack& p = *pp;
        std::string chn = p.meta.value("character", "?");
        if (chn != lastChar) {
            endRow();
            int n = 0;
            for (auto* q : list) if (q->meta.value("character", "?") == chn) n++;
            ImGui::Dummy(ImVec2(0, S(6)));
            ImVec2 hp = ImGui::GetCursorScreenPos();
            std::string head = w::upper(chn);
            txt(d, g_fontGroup, hp, tc(tok::SOFT), head);
            float hw = txtW(g_fontGroup, head.c_str());
            std::string cnt = std::to_string(n) + (n == 1 ? " pack" : " packs");
            txt(d, g_fontSmall, ImVec2(hp.x + hw + S(10), hp.y + S(2)), tc(tok::DIM), cnt);
            float lx = hp.x + hw + S(10) + txtW(g_fontSmall, cnt.c_str()) + S(12);
            d->AddLine(ImVec2(lx, hp.y + S(8)), ImVec2(hp.x + avail, hp.y + S(8)), tc(tok::IRON));
            ImGui::Dummy(ImVec2(avail, S(22)));
            lastChar = chn;
        }
        if (col == 0) rowO = ImGui::GetCursorScreenPos();
        ImVec2 cp(rowO.x + col * (cw + gap), rowO.y);
        ImGui::PushID(id++);
        auto insts = installedOf(a, p.key());
        const Inst* ni = newestInst(insts);
        bool pinned = s.commPinned.count(p.key()) > 0;
        bool upd = ni && !pinned && cmpVer(p.ver(), ni->ver) > 0;
        ImVec2 sz(0, 0);
        ID3D11ShaderResourceView* tx = nullptr;
        if (ni) tx = previewTex(a, ni->dir, sz);
        else if (s.commSource == 0) tx = previewTex(a, localRoot(s.commLocal) + "\\" + p.path, sz);
        ImVec2 cb(cp.x + cw, cp.y + ch);
        bool hot = ImGui::IsMouseHoveringRect(cp, cb) && ImGui::IsWindowHovered();
        d->AddRectFilled(cp, cb, tc(hot ? tok::BG3 : tok::BG2), S(8));
        d->AddRectFilled(cp, ImVec2(cb.x, cp.y + pv), tc(tok::BG0), S(8), ImDrawFlags_RoundCornersTop);
        if (tx && sz.x > 0 && sz.y > 0) {
            float k = (std::min)(cw / sz.x, pv / sz.y), iw = sz.x * k, ih = sz.y * k;
            ImVec2 ip(cp.x + (cw - iw) * 0.5f, cp.y + (pv - ih) * 0.5f);
            d->AddImage((ImTextureID)tx, ip, ImVec2(ip.x + iw, ip.y + ih));
        } else {
            w::icon(d, ICON_LC_IMAGE, ImVec2(cp.x + cw * 0.5f, cp.y + pv * 0.5f - S(8)), S(34), tc(tok::IRON3));
            std::string np = "No preview";
            txt(d, g_fontSmall, ImVec2(cp.x + (cw - txtW(g_fontSmall, np.c_str())) * 0.5f, cp.y + pv * 0.5f + S(16)), tc(tok::DIM), np);
        }
        d->AddRect(cp, cb, hot ? tc(tok::IRON3) : tc(tok::IRON2), S(8));
        float bx = cp.x + S(12), bw = cw - S(24), y = cp.y + pv + S(10);
        txt(d, g_fontName15, ImVec2(bx, y), tc(tok::TEXT), fitText(g_fontName15, p.meta.value("name", "?"), bw));
        y += S(24);
        std::string st = p.meta.value("status", "wip");
        float vx = bx;
        std::string ver = "v" + p.ver();
        txt(d, g_fontSmall, ImVec2(vx, y + S(2)), tc(tok::SOFT), ver);
        vx += txtW(g_fontSmall, ver.c_str()) + S(10);
        vx += chip(d, ImVec2(vx, y), w::upper(st), statusCol(st)) + S(6);
        if (pinned) chip(d, ImVec2(vx, y), "PINNED", kSteel);
        y += S(22);
        std::string by = "by " + p.meta.value("author", "?") + "  -  " + p.meta.value("slot", "?");
        txt(d, g_fontSmall, ImVec2(bx, y), tc(tok::MUTED), fitText(g_fontSmall, by, bw));
        y += S(21);
        {   // tags
            float tx2 = bx;
            auto tags = p.meta.value("tags", json::array());
            int shown = 0;
            for (auto& t : tags) {
                std::string ts = t.get<std::string>();
                float wd = txtW(g_fontSmall, ts.c_str()) + S(14);
                if (tx2 + wd > bx + bw - (shown + 1 < (int)tags.size() ? S(28) : 0)) break;
                tx2 += chip(d, ImVec2(tx2, y), ts, tok::MUTED, g_fontSmall) + S(5);
                shown++;
            }
            if (shown < (int)tags.size()) txt(d, g_fontSmall, ImVec2(tx2 + S(2), y + S(2)), tc(tok::DIM), "+" + std::to_string((int)tags.size() - shown));
        }
        y += S(26);
        for (auto& l : wrapLines(g_fontSmall, p.meta.value("description", ""), bw, 2)) {
            txt(d, g_fontSmall, ImVec2(bx, y), tc(tok::SOFT), l);
            y += S(16);
        }
        float by2 = cp.y + ch - S(12) - S(30);
        if (ni) {   // installed state
            std::string t = upd ? "Installed v" + ni->ver + " - update available" : "Installed v" + ni->ver;
            w::icon(d, upd ? ICON_LC_TRIANGLE_ALERT : ICON_LC_CIRCLE_CHECK, ImVec2(bx + S(7), by2 - S(15)), S(13), tc(upd ? tok::GLOW : tok::OK));
            txt(d, g_fontSmall, ImVec2(bx + S(20), by2 - S(22)), tc(upd ? tok::GLOW : tok::OK), fitText(g_fontSmall, t, bw - S(20)));
        }
        // actions
        float mw = S(30), pw = bw - mw - S(8);
        ImGui::SetCursorScreenPos(ImVec2(bx, by2));
        bool doInst = false;
        if (!ni) {
            doInst = steelButton("Install", "Install", ImVec2(pw, S(30)), !g_busy, ICON_LC_PACKAGE);
        } else if (upd) {
            doInst = w::primaryButton("Update", ("Update to v" + p.ver()).c_str(), ImVec2(pw, S(30)), !g_busy, ICON_LC_REFRESH_CW);
        } else {
            if (steelButton("Apply to groom", "Apply to groom", ImVec2(pw, S(30)), true, ICON_LC_CHECK)) applyPack(a, ni->dir);
            w::tip("Apply to groom\nAdds this pack's edits to the loaded project (same character and slot only). One undo step.");
        }
        if (doInst) {
            g_busy = true;
            setMsg("Installing " + p.key() + "...");
            std::thread(doInstall, a.set, installRoot(a), p).detach();
        }
        ImGui::SameLine(0, S(8));
        if (ni) {
            if (w::iconButton("More##card", ICON_LC_ELLIPSIS, 30, "More actions")) ImGui::OpenPopup("##morep");
            if (ImGui::BeginPopup("##morep")) {
                if (upd && ImGui::MenuItem("Apply to groom")) applyPack(a, ni->dir);
                if (ImGui::MenuItem(upd ? "Install again" : "Reinstall", nullptr, false, !g_busy)) {
                    g_busy = true;
                    setMsg("Installing " + p.key() + "...");
                    std::thread(doInstall, a.set, installRoot(a), p).detach();
                }
                if (ImGui::MenuItem("Create working copy")) makeWorkingCopy(a, p, *ni);
                bool pin = pinned;
                if (ImGui::MenuItem("Pin version", nullptr, &pin)) { if (pin) s.commPinned.insert(p.key()); else s.commPinned.erase(p.key()); s.save(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pinned packs are never offered an update or a sync.");
                ImGui::EndPopup();
            }
        } else {
            w::iconButton("More##card", ICON_LC_ELLIPSIS, 30, "Install the pack first to see more actions", false);
        }
        ImGui::PopID();
        col++;
        if (col >= cols) endRow();
    }
    endRow();
    ImGui::EndChild();
}

// ---- My packs
void drawMine(App& a, Settings& s, const std::string& ghUser) {
    finishExport(a);
    if (!g_ef.inited) {
        g_ef.inited = true;
        snprintf(g_ef.author, sizeof g_ef.author, "%s", s.commAuthor.c_str());
    }
    if (g_ef.slot[0] == 0 && a.loaded) snprintf(g_ef.slot, sizeof g_ef.slot, "%s", currentSlot(a).c_str());
    if (g_ef.author[0] == 0 && !ghUser.empty()) snprintf(g_ef.author, sizeof g_ef.author, "%s", ghUser.c_str());
    ImGui::BeginChild("##mine", ImVec2(0, 0), false);
    {   // export card
        Card c;
        c.begin(tok::BG2);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float W = c.inner();
        ImDrawList* d = c.d;
        const char* bt = g_exportOpen ? "Hide form" : "New pack";
        float bw = pbtnW(bt, ICON_LC_PLUS);
        w::icon(d, ICON_LC_PACKAGE, ImVec2(p.x + S(14), p.y + S(26)), S(28), tc(kSteel));
        txt(d, g_fontName15, ImVec2(p.x + S(40), p.y + S(8)), tc(tok::TEXT), fitText(g_fontName15, "Export current project as pack", W - bw - S(56)));
        auto lines = wrapLines(g_fontSmall, a.loaded ? "Turn your edits on the loaded project into a shareable pack you can publish." : "Open a character build first (File > Open), then export your edits as a pack.", W - bw - S(56), 2);
        float ly = p.y + S(32);
        for (auto& l : lines) { txt(d, g_fontSmall, ImVec2(p.x + S(40), ly), tc(tok::MUTED), l); ly += S(16); }
        ImGui::SetCursorScreenPos(ImVec2(p.x + W - bw, p.y + S(10)));
        if (steelButton("Export current project as pack", bt, ImVec2(bw, S(34)), true, ICON_LC_PLUS)) g_exportOpen = !g_exportOpen;
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(W, S(56)));
        if (g_exportOpen) {
            ImVec2 q = ImGui::GetCursorScreenPos();
            d->AddLine(ImVec2(q.x - S(14), q.y), ImVec2(q.x + W + S(14), q.y), tc(tok::IRON));
            ImGui::Dummy(ImVec2(W, S(12)));
            w::pushInputStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(6)));
            float lw = S(100), fw = (std::min)(W - lw, S(460));
            rowLabel("Name", lw); ImGui::SetNextItemWidth(fw); ImGui::InputText("##ename", g_ef.name, sizeof g_ef.name);
            rowLabel("Author", lw); ImGui::SetNextItemWidth(fw); ImGui::InputText("##eauthor", g_ef.author, sizeof g_ef.author);
            rowLabel("Version", lw); ImGui::SetNextItemWidth(fw); ImGui::InputText("##eversion", g_ef.version, sizeof g_ef.version);
            rowLabel("Slot", lw); ImGui::SetNextItemWidth(fw); ImGui::InputText("##eslot", g_ef.slot, sizeof g_ef.slot);
            rowLabel("Status", lw); ImGui::SetNextItemWidth(fw); ImGui::Combo("##estatus", &g_ef.status, "wip\0ready\0");
            rowLabel("Tags", lw); ImGui::SetNextItemWidth(fw); ImGui::InputTextWithHint("##tags", "comma separated", g_ef.tags, sizeof g_ef.tags);
            rowLabel("Description", lw); ImGui::InputTextMultiline("##desc", g_ef.desc, sizeof g_ef.desc, ImVec2(fw, S(76)));
            ImGui::PopStyleVar();
            w::popInputStyle();
            ImGui::Dummy(ImVec2(0, S(4)));
            ImGui::SetCursorScreenPos(ImVec2(p.x + lw, ImGui::GetCursorScreenPos().y));
            bool can = a.loaded && !g_ef.waiting;
            if (steelButton("Export as pack", "Export as pack", ImVec2(S(160), S(34)), can, ICON_LC_SAVE)) {
                if (g_ef.author[0] && s.commAuthor != g_ef.author) { s.commAuthor = g_ef.author; s.save(); }
                exportPack(a);
            }
            ImGui::SameLine(0, S(12));
            ImGui::AlignTextToFramePadding();
            ImGui::PushFont(g_fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, tv(tok::MUTED));
            ImGui::TextUnformatted(!a.loaded ? "Open a build first." : g_ef.waiting ? "Writing the preview..." : "Only edit ops are exported.");
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, S(2)));
            ImGui::PushTextWrapPos(p.x + W);
            ImGui::PushFont(g_fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, tv(tok::DIM));
            ImGui::TextWrapped("Writes %s\\<author>__<name>\\ (pack.json, edits.hwgroom, preview.jpg of the 3D view).", workRoot(a).c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::PopTextWrapPos();
        }
        c.end();
    }
    ImGui::Dummy(ImVec2(0, S(4)));
    auto dirs = workDirs(a);
    if (dirs.empty()) {
        if (emptyState(ICON_LC_FOLDER_OPEN, "No working copies or exports yet. Create a working copy from an installed pack, or export your current project.", "New pack##empty", "New pack")) g_exportOpen = true;
        ImGui::EndChild();
        return;
    }
    w::subHeader("Your packs");
    int wi = 0;
    for (auto& dpath : dirs) {
        json m;
        readJson(P(dpath) / "pack.json", m);
        ImGui::PushID(wi++);
        Card c;
        c.begin();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float W = c.inner();
        ImDrawList* d = c.d;
        // preview thumbnail on the left, like the Browse cards
        const float thW = S(150), thH = S(96), thGap = S(14);
        {
            ImVec2 tsz(0, 0);
            ID3D11ShaderResourceView* tx = previewTex(a, dpath, tsz);
            ImVec2 tb(p0.x + thW, p0.y + thH);
            d->AddRectFilled(p0, tb, tc(tok::BG0), S(6));
            if (tx && tsz.x > 0 && tsz.y > 0) {
                float k = (std::min)(thW / tsz.x, thH / tsz.y), iw = tsz.x * k, ih = tsz.y * k;
                ImVec2 ip(p0.x + (thW - iw) * 0.5f, p0.y + (thH - ih) * 0.5f);
                d->AddImage((ImTextureID)tx, ip, ImVec2(ip.x + iw, ip.y + ih));
            } else {
                txt(d, g_fontSmall, ImVec2(p0.x + S(10), p0.y + thH * 0.5f - S(7)), tc(tok::DIM), "no preview");
            }
        }
        ImVec2 p(p0.x + thW + thGap, p0.y);
        W -= thW + thGap;
        std::string nm = fs::path(dpath).filename().string(), st = m.value("status", "wip");
        std::string ver = "v" + m.value("version", "?");
        float rest = txtW(g_fontSmall, ver.c_str()) + S(10) + S(70) + S(10);
        std::string fn = fitText(g_fontName15, nm, W - rest);
        txt(d, g_fontName15, ImVec2(p.x, p.y + S(2)), tc(tok::TEXT), fn);
        float x = p.x + txtW(g_fontName15, fn.c_str()) + S(10);
        txt(d, g_fontSmall, ImVec2(x, p.y + S(5)), tc(tok::SOFT), ver);
        x += txtW(g_fontSmall, ver.c_str()) + S(10);
        chip(d, ImVec2(x, p.y + S(3)), w::upper(st), statusCol(st));
        std::string meta = m.value("character", "?") + " / " + m.value("slot", "?");
        if (m.contains("based_on")) meta += "   -   based on " + m.value("based_on", "") + " v" + m.value("based_on_version", "?");
        txt(d, g_fontSmall, ImVec2(p.x, p.y + S(26)), tc(tok::MUTED), fitText(g_fontSmall, meta, W));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(48)));
        ImGui::Indent(thW + thGap);
        float xmax = p.x + W;
        if (steelButton("Apply to groom", "Apply to groom", ImVec2(pbtnW("Apply to groom", ICON_LC_CHECK), S(30)), true, ICON_LC_CHECK)) applyPack(a, dpath);
        w::tip("Apply to groom\nAdds this pack's edits to the loaded project (same character and slot only). One undo step.");
        Inst th;
        bool pinned = s.commPinned.count(basedKey(m)) > 0;
        if (upstreamNewer(a, m, &th)) {
            if (pinned) {
                std::string t = "Upstream v" + th.ver + " (pinned)";
                flow(btnW(t.c_str(), nullptr), xmax);
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, tv(tok::MUTED));
                ImGui::TextUnformatted(t.c_str());
                ImGui::PopStyleColor();
            } else {
                std::string t = "Sync with v" + th.ver;
                float sw = pbtnW(t.c_str(), ICON_LC_REFRESH_CW);
                flow(sw, xmax);
                if (w::primaryButton("Sync", t.c_str(), ImVec2(sw, S(30)), true, ICON_LC_REFRESH_CW)) startSync(a, dpath);
            }
        }
        flow(btnW("Update from project", ICON_LC_SAVE), xmax);
        if (w::button("Update from project", "Update from project", ImVec2(btnW("Update from project", ICON_LC_SAVE), S(30)), ICON_LC_SAVE)) {
            snprintf(g_ef.name, sizeof g_ef.name, "%s", m.value("name", "").c_str());
            snprintf(g_ef.author, sizeof g_ef.author, "%s", m.value("author", "").c_str());
            snprintf(g_ef.version, sizeof g_ef.version, "%s", m.value("version", "0.1.0").c_str());
            snprintf(g_ef.slot, sizeof g_ef.slot, "%s", m.value("slot", "").c_str());
            snprintf(g_ef.desc, sizeof g_ef.desc, "%s", m.value("description", "").c_str());
            g_ef.status = m.value("status", "wip") == "ready";
            std::string tg;
            for (auto& t : m.value("tags", json::array())) tg += (tg.empty() ? "" : ", ") + t.get<std::string>();
            snprintf(g_ef.tags, sizeof g_ef.tags, "%s", tg.c_str());
            exportPack(a);
        }
        w::tip("Update from project\nRe-exports the loaded project's edits into this pack (keeps name, author, version; bump the version in the export form first if you changed it).");
        flow(btnW("Open folder", ICON_LC_FOLDER_OPEN), xmax);
        if (w::button("Open folder", "Open folder", ImVec2(btnW("Open folder", ICON_LC_FOLDER_OPEN), S(30)), ICON_LC_FOLDER_OPEN)) openFolder(dpath);
        flow(btnW("Publish...", ICON_LC_GIT_BRANCH), xmax);
        if (w::button("Publish...", "Publish...", ImVec2(btnW("Publish...", ICON_LC_GIT_BRANCH), S(30)), ICON_LC_GIT_BRANCH)) { g_pubSel = dpath; g_goGithub = true; }
        ImGui::Unindent(thW + thGap);
        { float yb = p0.y + thH; if (ImGui::GetCursorScreenPos().y < yb) ImGui::Dummy(ImVec2(0, yb - ImGui::GetCursorScreenPos().y)); }
        c.end();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---- GitHub
void drawGithub(App& a, Settings& s) {
    if (!g_ghChecked) { g_ghChecked = true; ghx::check(); }
    auto gs = ghx::state();
    if (!gs.user.empty() && s.commAuthor != gs.user) { s.commAuthor = gs.user; s.save(); }
    ImGui::BeginChild("##gh", ImVec2(0, 0), false);
    {   // status card
        Card c;
        c.begin();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float W = c.inner();
        ImDrawList* d = c.d;
        const char* ph[] = {"Not checked yet", "Checking...", "GitHub CLI is not installed", "Not signed in", "Signed in"};
        unsigned col = gs.phase == ghx::Ready ? tok::OK : (gs.phase == ghx::Missing || gs.phase == ghx::NotAuth) ? tok::GLOW : tok::DIM;
        d->AddCircleFilled(ImVec2(p.x + S(8), p.y + S(20)), S(5), tc(col));
        d->AddCircle(ImVec2(p.x + S(8), p.y + S(20)), S(8), tc(col, 0.35f));
        txt(d, g_fontName15, ImVec2(p.x + S(26), p.y + S(6)), tc(tok::TEXT), "GitHub CLI");
        std::string status = ph[gs.phase];
        if (!gs.user.empty()) status += " as " + gs.user;
        float rb = btnW("Re-check", ICON_LC_REFRESH_CW);
        txt(d, g_fontSmall, ImVec2(p.x + S(26), p.y + S(28)), tc(col), fitText(g_fontSmall, status, W - rb - S(40)));
        ImGui::SetCursorScreenPos(ImVec2(p.x + W - rb, p.y + S(8)));
        if (w::button("Re-check", "Re-check", ImVec2(rb, S(30)), ICON_LC_REFRESH_CW, true) && !gs.busy) ghx::check();
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(W, S(48)));
        if (gs.phase == ghx::Missing || gs.phase == ghx::NotAuth) {
            ImGui::PushTextWrapPos(p.x + W);
            ImGui::PushFont(g_fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, tv(tok::MUTED));
            ImGui::TextWrapped("Publishing needs a GitHub account and the GitHub CLI (gh). Browsing and installing packs work without them.");
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, S(8)));
            struct Step { const char* title; const char* desc; const char* lab; const char* glyph; bool done, enabled; } steps[2] = {
                {"Install GitHub CLI", "Runs: winget install --id GitHub.cli -e (falls back to opening cli.github.com).", "Install GitHub CLI", ICON_LC_PACKAGE, gs.phase != ghx::Missing, gs.phase == ghx::Missing && !gs.busy},
                {"Sign in to GitHub", "The login opens in a console and your browser; gh keeps the token, the Studio never sees it.", "Sign in to GitHub", ICON_LC_USER, false, gs.phase == ghx::NotAuth && !gs.busy}};
            for (int i = 0; i < 2; i++) {
                ImVec2 q = ImGui::GetCursorScreenPos();
                float bw = pbtnW(steps[i].lab, steps[i].glyph);
                d->AddCircleFilled(ImVec2(q.x + S(12), q.y + S(15)), S(11), steps[i].done ? tc(tok::OK, 0.25f) : tc(tok::BG4));
                if (steps[i].done) w::icon(d, ICON_LC_CHECK, ImVec2(q.x + S(12), q.y + S(15)), S(13), tc(tok::OK));
                else txt(d, g_fontUiSemi, ImVec2(q.x + S(8), q.y + S(7)), tc(tok::SOFT), std::to_string(i + 1));
                float tw = W - S(34) - bw - S(14);
                txt(d, g_fontUiSemi, ImVec2(q.x + S(34), q.y), tc(tok::TEXT), steps[i].title);
                float ly = q.y + S(20);
                for (auto& l : wrapLines(g_fontSmall, steps[i].desc, tw, 3)) { txt(d, g_fontSmall, ImVec2(q.x + S(34), ly), tc(tok::MUTED), l); ly += S(15); }
                ImGui::SetCursorScreenPos(ImVec2(q.x + W - bw, q.y));
                bool go = steps[i].done ? false : steelButton(steps[i].lab, steps[i].lab, ImVec2(bw, S(30)), steps[i].enabled, steps[i].glyph);
                if (go) { if (i == 0) ghx::installCli(); else ghx::signIn(); }
                ImGui::SetCursorScreenPos(q);
                ImGui::Dummy(ImVec2(W, (std::max)(S(44), ly - q.y + S(10))));
            }
        }
        c.end();
    }
    auto dirs = workDirs(a);
    std::string cur = g_pubSel.empty() && !dirs.empty() ? dirs[0] : g_pubSel;
    bool haveRepo = s.commRepo.find('/') != std::string::npos;
    w::subHeader("Publish");
    {
        Card c;
        c.begin();
        if (dirs.empty()) {
            if (emptyState(ICON_LC_FOLDER_OPEN, "Nothing to publish yet. Export your project as a pack first.", "Go to My packs##empty", "Go to My packs")) g_tab = 1;
        } else {
            ImGui::Dummy(ImVec2(0, S(4)));
            w::pushInputStyle();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(6)));
            rowLabel("Pack to publish", S(120));
            ImGui::SetNextItemWidth(c.inner() - S(120));
            if (ImGui::BeginCombo("##pubpack", cur.empty() ? "(none)" : fs::path(cur).filename().string().c_str())) {
                for (auto& dd : dirs) if (ImGui::Selectable(fs::path(dd).filename().string().c_str(), dd == cur)) g_pubSel = dd;
                ImGui::EndCombo();
            }
            ImGui::PopStyleVar();
            w::popInputStyle();
            ImGui::Dummy(ImVec2(0, S(4)));
            bool canPub = gs.phase == ghx::Ready && !gs.busy && !cur.empty() && haveRepo;
            if (w::primaryButton("Publish (open a pull request)", "Publish (open a pull request)", ImVec2(c.inner(), S(40)), canPub, ICON_LC_GIT_BRANCH)) {
                auto errs = validatePack(P(cur));
                json m;
                readJson(P(cur) / "pack.json", m);
                std::error_code ec;
                if (!fs::exists(P(cur) / "preview.jpg", ec) && !fs::exists(P(cur) / "preview.png", ec)) errs.push_back("no preview image: export the pack from the project first");
                if (!errs.empty()) { ghx::log("Publish refused:"); for (auto& e : errs) ghx::log("  - " + e); g_logOpen = true; }
                else {
                    ghx::PubJob j;
                    j.packDir = cur; j.upstream = s.commRepo; j.character = m.value("character", ""); j.slot = m.value("slot", "");
                    j.name = m.value("name", ""); j.version = m.value("version", ""); j.author = m.value("author", ""); j.key = j.author + "__" + j.name;
                    j.baseBuild = m.value("base_build", ""); j.wip = m.value("status", "wip") != "ready";
                    ghx::publish(j);
                }
            }
            ImGui::Dummy(ImVec2(0, S(6)));
            if (!haveRepo) w::note(ICON_LC_TRIANGLE_ALERT, tok::GLOW, "Set the GitHub repo (owner/name) in the Source settings (gear button above).");
            else if (gs.phase != ghx::Ready) w::note(ICON_LC_INFO, tok::MUTED, "Sign in with the GitHub CLI to enable publishing.");
            std::string tgt = "Target: " + s.commRepo + " (PR to main). Needs git for Windows. Nothing is sent until you press Publish.";
            if (haveRepo) w::note(ICON_LC_INFO, tok::MUTED, tgt.c_str());
            else w::note(ICON_LC_INFO, tok::MUTED, "Needs git for Windows. Nothing is sent until you press Publish.");
        }
        c.end();
    }
    w::subHeader("Pull requests");
    bool anyPr = false;
    for (auto& dd : dirs) {
        std::string key = fs::path(dd).filename().string();
        std::string url = ghx::prUrl(key);
        if (url.empty()) continue;
        anyPr = true;
        ImGui::PushID(key.c_str());
        Card c;
        c.begin();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float W = c.inner();
        std::string prT = ghx::prText(key);
        float b1 = btnW("Refresh status", ICON_LC_REFRESH_CW), b2 = btnW("Open", ICON_LC_FOLDER_OPEN);
        float tw = (std::max)(S(80), W - b1 - b2 - S(130));
        std::string fk = fitText(g_fontUiSemi, key, tw);
        txt(c.d, g_fontUiSemi, ImVec2(p.x, p.y + S(8)), tc(tok::TEXT), fk);
        prStateChip(c.d, ImVec2(p.x + txtW(g_fontUiSemi, fk.c_str()) + S(10), p.y + S(7)), prT);
        ImGui::SetCursorScreenPos(ImVec2(p.x + W - b1 - b2 - S(8), p.y));
        if (w::button("Refresh status", "Refresh status", ImVec2(b1, S(30)), ICON_LC_REFRESH_CW, true) && !gs.busy) ghx::prStatus(key);
        ImGui::SameLine(0, S(8));
        if (w::button("Open", "Open", ImVec2(b2, S(30)), ICON_LC_FOLDER_OPEN) && url.rfind("https://github.com/", 0) == 0) ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(36)));
        ImGui::PushTextWrapPos(p.x + W);
        ImGui::PushFont(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, tv(tok::MUTED));
        ImGui::TextWrapped("%s", prT.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
        c.end();
        ImGui::PopID();
    }
    if (!anyPr) w::note(ICON_LC_INFO, tok::MUTED, "No pull requests yet. Published packs appear here.");
    ImGui::Dummy(ImVec2(0, S(6)));
    {   // collapsible log
        ImVec2 p = ImGui::GetCursorScreenPos();
        float W = ImGui::GetContentRegionAvail().x;
        if (ImGui::InvisibleButton("Log##toggle", ImVec2(W, S(28)))) g_logOpen = !g_logOpen;
        ImDrawList* d = ImGui::GetWindowDrawList();
        w::icon(d, g_logOpen ? ICON_LC_CHEVRON_DOWN : ICON_LC_CHEVRON_RIGHT, ImVec2(p.x + S(10), p.y + S(14)), S(14), tc(tok::MUTED));
        txt(d, g_fontUiSemi, ImVec2(p.x + S(26), p.y + S(6)), tc(tok::SOFT), gs.busy ? "Log (working...)" : "Log");
        if (g_logOpen) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, tv(tok::BG0));
            ImGui::PushStyleColor(ImGuiCol_Border, tv(tok::IRON2));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(6));
            ImGui::BeginChild("##log", ImVec2(0, S(170)), true);
            auto lines = ghx::logLines();
            ImGui::PushFont(g_fontSmall);
            ImGui::PushTextWrapPos(0.0f);
            for (auto& l : lines) ImGui::TextUnformatted(l.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
            if (lines.size() != g_logCount) { ImGui::SetScrollHereY(1.0f); g_logCount = lines.size(); }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }
    }
    ImGui::EndChild();
}
}  // namespace

// headless apply (agent step "apply_pack"): the status line applyPack would show
std::string applyPackHeadless(App& a, const std::string& dir) {
    setMsg("");
    applyPack(a, dir);
    std::lock_guard<std::mutex> l(g_mx);
    return g_msg;
}

void drawCommunity(App& a, bool& open) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2((std::max)(S(960), vp->Size.x * 0.62f), (std::max)(S(720), vp->Size.y * 0.78f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(760), S(520)), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
    bool vis = ImGui::Begin("Community", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!vis) { ImGui::End(); return; }
    Settings& s = a.set;
    ghx::setDataDir(installRoot(a));
    finishExport(a);
    std::string ghUser = ghx::state().user;
    if (!g_loadedOnce && (s.commSource == 1 ? s.commRepo.find('/') != std::string::npos : !s.commLocal.empty())) startRefresh(a);

    bool changed = false;
    drawHeader(a, s, changed);
    if (changed) s.save();
    if (g_busy || !getMsg().empty()) { banner(getMsg(), g_busy); ImGui::Dummy(ImVec2(0, S(6))); }

    std::vector<CPack> packs;
    { std::lock_guard<std::mutex> l(g_mx); packs = g_packs; }
    if (g_goGithub) { g_tab = 2; g_goGithub = false; }
    int counts[3] = {(int)packs.size(), (int)workDirs(a).size(), -1};
    drawTabs(counts);
    if (g_tab == 0) drawBrowse(a, s, packs);
    else if (g_tab == 1) drawMine(a, s, ghUser);
    else drawGithub(a, s);
    drawSyncPopup(a);
    ImGui::End();
}
