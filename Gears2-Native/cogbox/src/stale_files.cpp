// CogBox 0.4.8: moving old program files out of the way after a release is unzipped over an older one (stale_files.h).
#include "stale_files.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "stale_known.h"

namespace cogbox {

namespace {

std::string lower_norm(std::string s)
{
    for (char &c : s) { if (c == '/') c = '\\'; c = (char)std::tolower((unsigned char)c); }
    while (!s.empty() && (s[0] == '\\' || s[0] == '.') && s.compare(0, 2, ".\\") == 0) s.erase(0, 2);
    while (!s.empty() && s[0] == '\\') s.erase(0, 1);
    return s;
}

std::vector<std::string> split_path(const std::string &p)
{
    std::vector<std::string> v;
    std::string cur;
    for (char c : p) { if (c == '\\') { v.push_back(cur); cur.clear(); } else cur += c; }
    v.push_back(cur);
    return v;
}

bool ends_with(const std::string &s, const char *e) { size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }
bool starts_with(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }
// The release's port folders: "port", "port-tu6", "port-disc" ... - not a copy the player made ("port-tu6 - copy", "port.bak")
bool is_port_dir(const std::string &d)
{
    if (d == "port") return true;
    if (d.size() < 6 || d.compare(0, 5, "port-") != 0) return false;
    for (size_t i = 5; i < d.size(); i++) if (!std::isalnum((unsigned char)d[i])) return false;
    return true;
}

// Folder names that hold the player's things (or things made at run time), wherever they appear in a path.
const char *const kUserDirs[] = { "user", "game", "shadercache", "logs", "achievements", "covers", "mods", "saves",
                                  "extracted", "update", "_old-files", "screenshots", "crash", "crashes", "frames", "dlc",
                                  "backup", "backups", "reshade-shaders", "plugins", "scripts" };
// DLLs players drop beside a game themselves: ReShade / Special K / DXVK / vkd3d / ASI loaders / input wrappers.
const char *const kModDlls[] = { "dxgi.dll", "d3d9.dll", "d3d10.dll", "d3d11.dll", "d3d12.dll", "dinput8.dll", "dinput.dll",
                                 "version.dll", "winmm.dll", "winhttp.dll", "wininet.dll", "opengl32.dll", "xinput1_3.dll",
                                 "xinput1_4.dll", "xinput9_1_0.dll", "dsound.dll", "d3dcompiler_47.dll", "vulkan-1.dll",
                                 "dbghelp.dll", "dxvk_d3d11.dll", "vkd3d-proton.dll" };

}  // namespace

const std::vector<std::string> &stale_tree_tops()
{
    static const std::vector<std::string> v = { "shared", "lang", "recipes", "prompts", "assets", "licenses" };
    return v;
}
const std::vector<std::string> &stale_game_dirs()
{
    static const std::vector<std::string> v = { "gears1", "gears2", "gears3", "judgment" };
    return v;
}

static bool is_hex64(const std::string &s)
{
    if (s.size() != 64) return false;
    for (char c : s) if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

bool parse_shipped(const std::string &text, ShippedList &out)
{
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) line.erase(0, 3);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.size() < 67) continue;
        std::string h = line.substr(0, 64);
        if (!is_hex64(h) || (line[64] != ' ' && line[64] != '\t')) continue;
        size_t p = 64;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '*')) p++;
        std::string rel = lower_norm(line.substr(p));
        if (rel.empty()) continue;
        for (char &c : h) c = (char)std::tolower((unsigned char)c);
        out.files.insert(rel);
        out.hashes.insert(h);
    }
    return !out.files.empty();
}

bool stale_path_ok(const std::string &relIn, const ShippedList &now, std::string *why)
{
    auto no = [&](const char *w) { if (why) *why = w; return false; };
    const std::string rel = lower_norm(relIn);
    if (rel.empty()) return no("empty");
    const std::vector<std::string> p = split_path(rel);
    for (const std::string &c : p) {
        if (c.empty() || c == "." || c == ".." || c.find(':') != std::string::npos) return no("odd path");
    }
    const std::string &name = p.back();
    if (!ends_with(name, ".exe") && !ends_with(name, ".dll")) return no("not a program file");
    // where
    bool where = false;
    if (p.size() == 1) where = true;   // beside CogBox.exe (not recursive)
    else if (std::find(stale_tree_tops().begin(), stale_tree_tops().end(), p[0]) != stale_tree_tops().end()) where = true;
    else if (p.size() >= 3 && std::find(stale_game_dirs().begin(), stale_game_dirs().end(), p[0]) != stale_game_dirs().end() &&
             is_port_dir(p[1])) where = true;
    if (!where) return no("not in a folder the release ships");
    for (size_t k = 0; k + 1 < p.size(); k++)
        for (const char *u : kUserDirs) if (p[k] == u) return no("user-data folder");
    // the player's own DLLs
    for (const char *m : kModDlls) if (name == m) return no("a mod / injector DLL name");
    if (starts_with(name, "sl.") || (starts_with(name, "nvngx_") && name != "nvngx_dlss.dll") || name.find("reshade") != std::string::npos ||
        name.find("specialk") != std::string::npos || starts_with(name, "dlss") || name.find("optiscaler") != std::string::npos)
        return no("a DLL players add themselves");
    if (name == "cogbox.exe" && p.size() == 1) return no("the launcher");
    if (now.files.count(rel)) return no("shipped by this release");
    if (why) why->clear();
    return true;
}

bool known_old_program(const std::string &hexIn, const std::set<std::string> &extra)
{
    std::string hex = hexIn;
    for (char &c : hex) c = (char)std::tolower((unsigned char)c);
    if (!is_hex64(hex)) return false;
    for (const char *k : kKnownOldProgramHashes) if (hex == k) return true;
    return extra.count(hex) != 0;
}

bool stale_is_custom_dlss(const std::string &relIn, const std::string &hexIn, const std::string &markerText)
{
    const std::string rel = lower_norm(relIn);
    const std::vector<std::string> p = split_path(rel);
    if (p.back() != "nvngx_dlss.dll") return false;
    std::string hex = hexIn, mark = markerText;
    for (char &c : hex) c = (char)std::tolower((unsigned char)c);
    if (mark.size() >= 3 && (unsigned char)mark[0] == 0xEF && (unsigned char)mark[1] == 0xBB && (unsigned char)mark[2] == 0xBF) mark.erase(0, 3);
    while (!mark.empty() && (mark.back() == '\r' || mark.back() == '\n' || mark.back() == ' ' || mark.back() == '\t')) mark.pop_back();
    while (!mark.empty() && (mark[0] == ' ' || mark[0] == '\t')) mark.erase(0, 1);
    for (char &c : mark) c = (char)std::tolower((unsigned char)c);
    return is_hex64(hex) && mark == hex;
}

std::string stale_custom_dlss_marker(const std::string &relIn)
{
    const std::string rel = lower_norm(relIn);
    const size_t s = rel.find_last_of('\\');
    return s == std::string::npos ? std::string("_old-files\\dlss-backup\\custom-dlss.txt")
                                  : "_old-files\\dlss-backup\\" + rel.substr(0, s) + "\\custom-dlss.txt";
}

}  // namespace cogbox

// ---- Windows: the scan and the moves ----------------------------------------------------------------------------------
#ifdef _WIN32
#include "app.h"

#include <bcrypt.h>

namespace {

std::string sha256_file(const std::wstring &path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        uint64_t total = 0;
        while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, NULL) && got) {
            BCryptHashData(h, buf.data(), got, 0);
            if ((total += got) > (1ull << 30)) { ok = false; break; }   // no program of ours is 1 GB
        }
        unsigned char d[32];
        if (ok && BCryptFinishHash(h, d, 32, 0) == 0) {
            static const char *hx = "0123456789abcdef";
            for (unsigned char b : d) { out += hx[b >> 4]; out += hx[b & 15]; }
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

// Files under root\rel (rel "" = root, files only when !recurse). Never enters a junction / symbolic link and never
// takes a reparse-point file.
void walk(const std::wstring &root, const std::wstring &rel, bool recurse, int depth, std::vector<std::wstring> &out)
{
    if (depth > 8 || out.size() > 20000) return;
    const std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::wstring r = rel.empty() ? n : rel + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (recurse) walk(root, r, true, depth + 1, out); }
        else out.push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool is_dir_real(const std::wstring &p)
{
    DWORD at = GetFileAttributesW(p.c_str());
    return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) && !(at & FILE_ATTRIBUTE_REPARSE_POINT);
}

std::string safe_dir_name(std::string s)
{
    for (char &c : s) if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) c = '_';
    return s;
}

}  // namespace

void stale_cleanup_startup(App &a, const std::string &prevVersion)
{
    using namespace cogbox;
    if (getenv("COGBOX_SHOT") || getenv("COGBOX_NO_CLEANUP")) return;
    if (a.ini.get("Cleanup", "DoneVersion") == COGBOX_VERSION) return;
    const std::string root = exe_dir();
    const std::wstring rootW = exe_dir_w();
    std::string sumsText;
    if (!read_file(join(root, "SHA256SUMS.txt"), sumsText, 1 << 20)) return;   // a developer build, or not unzipped whole
    std::string log = "COGBox " COGBOX_VERSION " old-files check " + now_stamp() + "\r\n";
    auto finish = [&](bool done) {
        const std::string ld = join(root, "logs");
        make_dirs(ld);
        std::string have;
        read_file(join(ld, "old-files.txt"), have, 1 << 20);
        write_file(join(ld, "old-files.txt"), have + log + "\r\n");
        if (done) { a.ini.set("Cleanup", "DoneVersion", COGBOX_VERSION); a.ini.save(); }
    };
    ShippedList now;
    if (!parse_shipped(sumsText, now) || !now.files.count("cogbox.exe")) { log += "SHA256SUMS.txt unreadable - skipped, tried again next start\r\n"; finish(false); return; }
    // the release must be there whole (a half-finished unzip: try again next start)
    size_t present = 0;
    for (const std::string &f : now.files) present += file_exists(join(root, f));
    if (present * 10 < now.files.size() * 9) {
        log += "only " + std::to_string(present) + " of " + std::to_string(now.files.size()) + " listed files present - skipped\r\n";
        finish(false);
        return;
    }
    // the lists earlier COGBox versions saved, then this one's (so a later version knows these files)
    std::set<std::string> extra;
    const std::string shippedDir = join(root, "_old-files\\shipped");
    {
        std::vector<std::wstring> lists;
        walk(widen(shippedDir), L"", false, 0, lists);
        for (const std::wstring &l : lists) {
            std::string t;
            ShippedList s;
            if (read_file(join(shippedDir, narrow(l)), t, 1 << 20) && parse_shipped(t, s)) extra.insert(s.hashes.begin(), s.hashes.end());
        }
        make_dirs(shippedDir);
        write_file(join(shippedDir, std::string("SHA256SUMS-") + COGBOX_VERSION + ".txt"), sumsText);
    }
    // look
    std::vector<std::wstring> files;
    walk(rootW, L"", false, 0, files);
    for (const std::string &t : stale_tree_tops()) if (is_dir_real(rootW + L"\\" + widen(t))) walk(rootW, widen(t), true, 1, files);
    for (const std::string &g : stale_game_dirs()) {
        const std::wstring gd = rootW + L"\\" + widen(g);
        if (!is_dir_real(gd)) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((gd + L"\\port*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                walk(rootW, widen(g) + L"\\" + fd.cFileName, true, 2, files);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::vector<std::wstring> move;
    for (const std::wstring &f : files) {
        if (!stale_path_ok(narrow(f), now)) continue;
        const std::string h = sha256_file(rootW + L"\\" + f);
        if (h.empty()) { log += "kept (couldn't read): " + narrow(f) + "\r\n"; continue; }
        std::string mark;
        if (read_file(join(root, stale_custom_dlss_marker(narrow(f))), mark, 4096) && stale_is_custom_dlss(narrow(f), h, mark)) {
            log += "kept (the player's Custom DLSS file): " + narrow(f) + "\r\n";
            continue;
        }
        if (known_old_program(h, extra)) move.push_back(f);
        else log += "kept (not a file COGBox shipped): " + narrow(f) + "\r\n";
    }
    if (move.empty()) { log += "nothing to move\r\n"; finish(true); return; }
    if ((int)move.size() > kStaleMaxMoves) {
        log += std::to_string(move.size()) + " files would move - more than " + std::to_string(kStaleMaxMoves) + ", something is wrong: nothing moved (tried again next start)\r\n";
        finish(false);   // cleanup049: not marked done
        return;
    }
    std::string sub = prevVersion.empty() ? "before-" COGBOX_VERSION "-" + today_text() : "from-" + safe_dir_name(prevVersion);
    std::string dest = join(join(root, "_old-files"), sub);
    if (dir_exists(dest)) dest += "-" + now_stamp();
    int moved = 0, busy = 0;
    std::string movedList;
    for (const std::wstring &f : move) {
        const std::wstring src = rootW + L"\\" + f;
        // in use (a running game or a loaded DLL): leave it - a running exe could be renamed, so check explicitly
        HANDLE hf = CreateFileW(src.c_str(), GENERIC_READ | DELETE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) { busy++; log += "in use, left: " + narrow(f) + "\r\n"; continue; }
        CloseHandle(hf);
        const std::string to = join(dest, narrow(f));
        make_dirs(to.substr(0, to.find_last_of('\\')));
        if (MoveFileExW(src.c_str(), widen(to).c_str(), 0)) {
            moved++;
            log += "moved: " + narrow(f) + " -> " + to + "\r\n";
            movedList += narrow(f) + "\r\n";
            // a folder the release no longer has (Gears2\port-disc): remove it once empty (never the top folders)
            std::wstring d = f.substr(0, f.find_last_of(L'\\') == std::wstring::npos ? 0 : f.find_last_of(L'\\'));
            while (!d.empty() && std::count(d.begin(), d.end(), L'\\') >= 1 && RemoveDirectoryW((rootW + L"\\" + d).c_str()))
                d = d.substr(0, d.find_last_of(L'\\') == std::wstring::npos ? 0 : d.find_last_of(L'\\'));
        } else log += "couldn't move: " + narrow(f) + " (error " + std::to_string(GetLastError()) + ")\r\n";
    }
    if (moved) write_file(join(dest, "moved.txt"), "Program files of an older COGBox that " COGBOX_VERSION " no longer uses (moved, not deleted; "
                                                  "safe to delete this folder):\r\n" + movedList);
    finish(busy == 0);   // something in use: try those again next start
    if (moved) set_message(a, "Moved " + std::to_string(moved) + " old program file" + (moved == 1 ? "" : "s") + " of an earlier COGBox to _old-files\\" +
                                  dest.substr(dest.find_last_of('\\') + 1) + ".", false);
}
#endif
