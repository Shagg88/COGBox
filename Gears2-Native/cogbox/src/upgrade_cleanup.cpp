// CogBox 0.4.9 (cleanup049): settings and shader caches after an update (upgrade_cleanup.h).
#include "upgrade_cleanup.h"

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <set>
#include <sstream>
#include <vector>

#include "gears_settings.h"
#include "stale_files.h"

namespace cogbox {

namespace {

std::string lower_bs(std::string s)
{
    for (char &c : s) { if (c == '/') c = '\\'; c = (char)std::tolower((unsigned char)c); }
    while (!s.empty() && s[0] == '\\') s.erase(0, 1);
    return s;
}
bool ends_with(const std::string &s, const std::string &e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
std::string base_of(const std::string &p) { size_t i = p.find_last_of('\\'); return i == std::string::npos ? p : p.substr(i + 1); }

}  // namespace

std::string upgrade_cache_key(const std::string &text)
{
    std::istringstream in(text);
    std::string line, dxcShared, dxcFirstPath, dxcFirst;
    std::set<std::string> agility;
    while (std::getline(in, line)) {
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 67) continue;
        std::string h = line.substr(0, 64);
        bool hex = true;
        for (char &c : h) { hex &= std::isxdigit((unsigned char)c) != 0; c = (char)std::tolower((unsigned char)c); }
        if (!hex) continue;
        size_t p = 64;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '*')) p++;
        const std::string rel = lower_bs(line.substr(p));
        const std::string b = base_of(rel);
        if (b == "dxcompiler.dll") {
            if (rel == "shared\\dxcompiler.dll") dxcShared = h;
            else if (dxcFirstPath.empty() || rel < dxcFirstPath) { dxcFirstPath = rel; dxcFirst = h; }
        } else if (b == "d3d12core.dll") agility.insert(h);
    }
    const std::string dxc = dxcShared.empty() ? dxcFirst : dxcShared;
    if (dxc.empty() && agility.empty()) return "";
    std::string k = "dxc=" + dxc + ";agility=";
    bool first = true;
    for (const std::string &a : agility) { k += (first ? "" : ",") + a; first = false; }
    return k;
}

bool upgrade_cache_file(const std::string &relIn)
{
    const std::string rel = lower_bs(relIn);
    if (rel.empty() || rel.find('\\') != std::string::npos) return false;   // sub-folders (ucode\) stay
    if (rel == "shaders.pack" || rel == "shaders.bin" || rel == "prepare-progress.txt") return true;
    if (rel.compare(0, 10, "pipelines-") == 0 && ends_with(rel, ".bin")) return true;
    return ends_with(rel, ".dxil") || ends_with(rel, ".spv");
}

bool upgrade_streamline_leftover(const std::string &relIn, const ShippedList &now)
{
    const std::string rel = lower_bs(relIn);
    const std::vector<std::string> games = stale_game_dirs();
    size_t a = rel.find('\\'), b = a == std::string::npos ? a : rel.find('\\', a + 1);
    if (b == std::string::npos || rel.find('\\', b + 1) != std::string::npos) return false;   // <Game>\<port>\<name> only
    const std::string g = rel.substr(0, a), port = rel.substr(a + 1, b - a - 1), name = rel.substr(b + 1);
    if (std::find(games.begin(), games.end(), g) == games.end() || port.compare(0, 4, "port") != 0) return false;
    if (port != "port" && (port.size() < 6 || port[4] != '-')) return false;   // not a copy ("port.bak", "port - copy")
    for (size_t i = 5; port != "port" && i < port.size(); i++) if (!std::isalnum((unsigned char)port[i])) return false;
    if (!(name.compare(0, 3, "sl.") == 0 && ends_with(name, ".dll")) && name != "nvngx_dlssg.dll") return false;
    return !now.files.count(rel) && now.files.count("shared\\" + name) != 0;
}

bool upgrade_settings_text(const std::string &in, std::string *out, bool *changed)
{
    namespace gs = cogbox::gears;
    if (changed) *changed = false;
    gs::Store st;
    if (!st.load_ini(in)) return false;
    // The old "Exclusive fullscreen instead" box (0.2.2) is read by Store::view only and save_ini doesn't write it:
    // fold it into Fullscreen (2 = exclusive) the way view does, or rewriting the file would lose the player's choice.
    const gs::Option *fs = gs::find_option(gs::Game::Gears2, "Fullscreen");
    const double fsDef = fs ? fs->def : 0.0;
    auto fold = [&](std::map<std::string, double> &m, bool useDefault) {
        auto ex = m.find("Exclusive");
        if (ex == m.end()) return;
        const bool on = ex->second != 0.0;
        m.erase(ex);
        auto f = m.find("Fullscreen");
        double x = f != m.end() ? f->second : (useDefault ? fsDef : -1.0);
        if (on && x == 1.0) m["Fullscreen"] = 2.0;
    };
    fold(st.top, true);
    for (int g = 0; g < gs::kGameCount; g++) fold(st.per[g], false);
    const std::string w = st.save_ini();
    if (out) *out = w;
    if (changed) {
        // line endings and trailing blanks don't count as a change
        auto norm = [](const std::string &s) {
            std::string r;
            std::istringstream is(s);
            std::string l;
            while (std::getline(is, l)) {
                while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
                r += l + "\n";
            }
            while (r.size() >= 2 && r[r.size() - 1] == '\n' && r[r.size() - 2] == '\n') r.pop_back();
            return r;
        };
        *changed = norm(in) != norm(w);
    }
    return true;
}

}  // namespace cogbox

// ---- Windows: the moves ----------------------------------------------------------------------------------------------
#ifdef _WIN32
#include "app.h"

#include <bcrypt.h>

namespace {

std::string safe_name(std::string s)
{
    for (char &c : s) if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) c = '_';
    return s.empty() ? std::string("unknown") : s;
}

std::string sha256_of(const std::string &path)
{
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD got = 0;
        while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, NULL) && got) BCryptHashData(h, buf.data(), got, 0);
        unsigned char d[32];
        if (BCryptFinishHash(h, d, 32, 0) == 0) { static const char *hx = "0123456789abcdef"; for (unsigned char c : d) { out += hx[c >> 4]; out += hx[c & 15]; } }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

// a new folder under _old-files (never an existing one)
std::string fresh_dir(const std::string &root, const std::string &name)
{
    std::string d = join(join(root, "_old-files"), name);
    if (dir_exists(d)) d += "-" + now_stamp();
    return d;
}

}  // namespace

void upgrade_cleanup_startup(App &a, const std::string &prevVersionIn)
{
    using namespace cogbox;
    if (getenv("COGBOX_SHOT") || getenv("COGBOX_NO_CLEANUP")) return;
    if (a.ini.get("Cleanup", "UpgradeDoneVersion") == COGBOX_VERSION) return;
    const std::string root = exe_dir();
    const std::string prev = safe_name(prevVersionIn.empty() ? a.ini.get("Cleanup", "DoneVersion") : prevVersionIn);
    std::string log = "COGBox " COGBOX_VERSION " upgrade check " + now_stamp() + " (from " + prev + ")\r\n";
    bool allDone = true;

    // 1. settings.ini: migrated by this version's Store, the old file kept in _old-files
    {
        const std::string sp = join(root, "settings.ini");
        std::string text, migrated;
        bool changed = false;
        if (!read_file(sp, text, 4 << 20)) log += "settings.ini: none\r\n";
        else if (!upgrade_settings_text(text, &migrated, &changed)) log += "settings.ini: no setting this version reads - left as it is\r\n";
        else if (!changed) log += "settings.ini: already current\r\n";
        else {
            const std::string bd = fresh_dir(root, "settings-from-" + prev);
            make_dirs(bd);
            if (write_file(join(bd, "settings.ini"), text) && write_file(sp, migrated)) {
                log += "settings.ini: migrated (old keys translated / dropped, values kept); the old file: " + join(bd, "settings.ini") + "\r\n";
                a.store.load_ini(migrated);   // the launcher works with what is now on disk
                for (int g = 0; g < G_COUNT; g++) a.cur[g] = a.store.view((gs::Game)g, a.env[g]);
            } else { log += "settings.ini: couldn't back up / write - left as it is\r\n"; allDone = false; }
        }
    }

    // 2. shader caches tied to the old shader compiler / D3D12 runtime
    {
        std::string sums;
        const std::string nowKey = read_file(join(root, "SHA256SUMS.txt"), sums, 1 << 20) ? upgrade_cache_key(sums) : "";
        std::string oldKey = a.ini.get("Cleanup", "CacheKey");
        if (oldKey.empty()) {   // first 0.4.9+: the list the previous version saved (stale_files.cpp, 0.4.8 on)
            std::string t;
            if (!prevVersionIn.empty() && read_file(join(root, "_old-files\\shipped\\SHA256SUMS-" + prevVersionIn + ".txt"), t, 1 << 20))
                oldKey = upgrade_cache_key(t);
        }
        if (nowKey.empty()) log += "shader caches: no SHA256SUMS.txt (a developer build) - left as they are\r\n";
        else if (!oldKey.empty() && oldKey == nowKey) log += "shader caches: same shader compiler and D3D12 runtime - kept\r\n";
        else {
            log += std::string("shader caches: ") + (oldKey.empty() ? "the previous compiler is unknown" : "the shader compiler / D3D12 runtime changed") + "\r\n";
            std::string dest;
            int moved = 0;
            for (int g = 0; g < G_COUNT; g++) {
                const std::string sd = game_shader_dir(g);
                if (!dir_exists(sd)) continue;
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileExW(widen(join(sd, "*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
                if (h == INVALID_HANDLE_VALUE) continue;
                std::vector<std::string> names;
                do {
                    if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                    const std::string n = narrow(fd.cFileName);
                    if (upgrade_cache_file(n)) names.push_back(n);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
                for (const std::string &n : names) {
                    if (dest.empty()) dest = fresh_dir(root, "shadercache-from-" + prev);
                    const std::string to = join(join(dest, game_def(g).dir), n);
                    make_dirs(to.substr(0, to.find_last_of('\\')));
                    if (MoveFileExW(widen(join(sd, n)).c_str(), widen(to).c_str(), MOVEFILE_COPY_ALLOWED)) { moved++; log += "  moved " + join(sd, n) + "\r\n"; }
                    else { log += "  couldn't move " + join(sd, n) + " (error " + std::to_string(GetLastError()) + ")\r\n"; allDone = false; }
                }
            }
            if (moved) write_file(join(dest, "moved.txt"), "Shader caches built by an older COGBox's shader compiler (moved, not deleted; safe to delete "
                                                           "this folder). Prepare all shaders again from each game's page.\r\n");
            if (allDone) a.ini.set("Cleanup", "CacheKey", nowKey);
        }
        if (!nowKey.empty() && oldKey == nowKey) a.ini.set("Cleanup", "CacheKey", nowKey);
    }

    // 3. Shagg 2026-11-08: 0.4.8's Streamline copies beside each game, now in shared\ - moved when the bytes are COGBox's
    {
        std::string sums;
        ShippedList now;
        if (read_file(join(root, "SHA256SUMS.txt"), sums, 1 << 20) && parse_shipped(sums, now)) {
            std::string dest;
            for (int g = 0; g < G_COUNT; g++) {
                const std::string gd = game_root(g);
                WIN32_FIND_DATAW pd;
                HANDLE ph = FindFirstFileExW(widen(join(gd, "port*")).c_str(), FindExInfoBasic, &pd, FindExSearchNameMatch, NULL, 0);
                if (ph == INVALID_HANDLE_VALUE) continue;
                do {
                    if (!(pd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (pd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                    const std::string portRel = std::string(game_def(g).dir) + "\\" + narrow(pd.cFileName);
                    WIN32_FIND_DATAW fd;
                    HANDLE fh = FindFirstFileExW(widen(join(join(root, portRel), "*.dll")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
                    if (fh == INVALID_HANDLE_VALUE) continue;
                    std::vector<std::string> names;
                    do {
                        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                        names.push_back(narrow(fd.cFileName));
                    } while (FindNextFileW(fh, &fd));
                    FindClose(fh);
                    for (const std::string &n : names) {
                        const std::string rel = portRel + "\\" + n;
                        if (!upgrade_streamline_leftover(rel, now)) continue;
                        if (!known_old_program(sha256_of(join(root, rel)), {})) { log += "kept (not a file COGBox shipped): " + rel + "\r\n"; continue; }
                        if (dest.empty()) dest = fresh_dir(root, "streamline-from-" + prev);
                        const std::string to = join(dest, rel);
                        make_dirs(to.substr(0, to.find_last_of('\\')));
                        if (MoveFileExW(widen(join(root, rel)).c_str(), widen(to).c_str(), 0)) log += "moved: " + rel + " -> " + to + "\r\n";
                        else { log += "in use / couldn't move: " + rel + "\r\n"; allDone = false; }
                    }
                } while (FindNextFileW(ph, &pd));
                FindClose(ph);
            }
            if (!dest.empty()) write_file(join(dest, "moved.txt"), "NVIDIA Streamline files an older COGBox put beside the games; this version keeps them in "
                                                                     "shared\\ (moved, not deleted; safe to delete this folder).\r\n");
        }
    }

    {
        const std::string ld = join(root, "logs");
        make_dirs(ld);
        std::string have;
        read_file(join(ld, "old-files.txt"), have, 1 << 20);
        write_file(join(ld, "old-files.txt"), have + log + "\r\n");
    }
    if (allDone) a.ini.set("Cleanup", "UpgradeDoneVersion", COGBOX_VERSION);
    a.ini.save();
}
#endif
