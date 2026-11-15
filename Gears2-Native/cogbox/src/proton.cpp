// COGBox - Linux and Steam Deck through Proton / Wine (proton049). See proton.h.
#include "proton.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace proton {

namespace {

std::string lower(std::string s)
{
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string first_line(const std::string &s)
{
    const size_t n = s.find_first_of("\r\n");
    return trim(n == std::string::npos ? s : s.substr(0, n));
}

// os-release: KEY=value or KEY="value"
std::string os_release_value(const std::string &text, const std::string &key)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.compare(0, key.size() + 1, key + "=") != 0) continue;
        std::string v = line.substr(key.size() + 1);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
        return v;
    }
    return "";
}

std::string basename_of(std::string p)
{
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

// ---- detection --------------------------------------------------------------------------------------------------------
Info detect(const Inputs &in)
{
    Info i;
    auto env = [&](const char *k) { return in.env ? in.env(k) : std::string(); };
    auto read = [&](const std::string &p) { std::string s; return in.readUnix && in.readUnix(p, s) ? s : std::string(); };
    i.wineVersion = in.wineVersion;
    i.wineBuild = in.wineBuild;
    i.hostSystem = in.hostSystem;
    i.hostRelease = in.hostRelease;
    i.zDrive = in.zDrive;
    i.wine = !in.wineVersion.empty();
    if (!i.wine) return i;   // Windows proper: nothing more is looked at (and nothing changes there)

    i.home = env("HOME");
    if (i.home.empty()) {   // Wine's own: WINEHOMEDIR=\??\Z:\home\deck
        std::string w = env("WINEHOMEDIR");
        if (w.rfind("\\??\\", 0) == 0) w = w.substr(4);
        i.home = to_unix(w);
    }
    i.user = env("USER");
    if (i.user.empty()) i.user = env("LOGNAME");
    if (i.user.empty() && !i.home.empty()) i.user = basename_of(i.home);

    // Proton: Steam sets these for every game it starts through a compatibility tool (non-Steam games too)
    const std::string tools = env("STEAM_COMPAT_TOOL_PATHS");
    i.proton = !env("STEAM_COMPAT_DATA_PATH").empty() || !env("STEAM_COMPAT_CLIENT_INSTALL_PATH").empty() || !tools.empty() ||
               lower(in.wineBuild).find("proton") != std::string::npos;
    const std::string pdir = proton_dir(tools);
    if (!pdir.empty()) {
        i.protonVersion = version_from_file(read(pdir + "/version"));
        if (i.protonVersion.empty()) i.protonVersion = basename_of(pdir);   // "Proton - Experimental", "GE-Proton11-12"
    }

    // SteamOS and the distribution
    const std::string osr = read("/etc/os-release");
    i.distro = os_release_value(osr, "PRETTY_NAME");
    if (i.distro.empty()) i.distro = os_release_value(osr, "NAME");
    i.steamOS = env("SteamOS") == "1" || lower(os_release_value(osr, "ID")) == "steamos";

    // 0.6.2 Proton review R4: System Link hosts on port 1000, and Linux lets normal programs use ports only from this one
    // up (default 1024) - the SDK then binds another port (NetDll_bind, patch 38) and joiners can't reach the host
    if (in.hostSystem != "Darwin") {
        const std::string v = first_line(read("/proc/sys/net/ipv4/ip_unprivileged_port_start"));
        if (!v.empty() && v.size() < 7 && std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            i.lowPortStart = std::stoi(v);
    }

    // The Steam Deck: Valve's DMI names, Steam's own variable, or the Deck's APU
    const std::string vendor = lower(first_line(read("/sys/devices/virtual/dmi/id/sys_vendor")));
    const std::string product = lower(first_line(read("/sys/devices/virtual/dmi/id/product_name")));
    const std::string board = lower(first_line(read("/sys/devices/virtual/dmi/id/board_name")));
    const bool valve = vendor.find("valve") != std::string::npos;
    bool lcd = valve && (product == "jupiter" || board == "jupiter");
    bool oled = valve && (product == "galileo" || board == "galileo");
    for (const std::string &g : in.gpus) {
        const std::string n = lower(g);
        if (n.find("custom gpu 0405") != std::string::npos || n.find("vangogh") != std::string::npos) lcd = lcd || !oled;
        if (n.find("custom gpu 0932") != std::string::npos || n.find("sephiroth") != std::string::npos) oled = true;
    }
    if (oled) lcd = false;
    i.steamDeck = lcd || oled || env("SteamDeck") == "1";
    i.deckOled = oled;
    i.deckLcd = lcd;
    i.deckModel = !i.steamDeck ? "" : oled ? "Steam Deck OLED" : lcd ? "Steam Deck LCD" : "Steam Deck";
    i.gamescope = !env("GAMESCOPE_WAYLAND_DISPLAY").empty() || lower(env("XDG_CURRENT_DESKTOP")) == "gamescope";
    return i;
}

std::string summary(const Info &i)
{
    if (!i.wine) return "";
    std::string s;
    std::string pv = i.protonVersion;
    if (lower(pv).rfind("proton-", 0) == 0) pv = pv.substr(7);   // "proton-11.0-2" -> "Proton 11.0-2"
    if (i.proton) s = (lower(pv).find("proton") != std::string::npos ? pv : "Proton" + (pv.empty() ? std::string() : " " + pv)) + " (Wine " + i.wineVersion + ")";
    else s = (i.hostSystem == "Darwin" ? "Wine on a Mac " : "Wine ") + i.wineVersion;
    if (!i.deckModel.empty()) s += ", " + i.deckModel;
    if (!i.distro.empty()) s += ", " + i.distro;
    else if (i.steamOS) s += ", SteamOS";
    if (i.gamescope) s += ", Game Mode (gamescope)";
    return s;
}

std::vector<std::string> report_lines(const Info &i)
{
    std::vector<std::string> v;
    if (!i.wine) return v;
    v.push_back(std::string(i.hostSystem == "Darwin" ? "Mac (CrossOver): " : "Linux / Steam Deck: ") + summary(i));   // 0.6.2 review
    v.push_back("Wine: " + i.wineVersion + (i.wineBuild.empty() ? "" : " (build " + i.wineBuild + ")") +
                (i.hostSystem.empty() ? "" : ", host " + i.hostSystem + " " + i.hostRelease));
    v.push_back(std::string("Proton: ") + (i.proton ? (i.protonVersion.empty() ? "yes (version not visible)" : i.protonVersion) : "no (plain Wine)"));
    if (i.lowPortStart >= 0)
        v.push_back("Ports for normal programs from: " + std::to_string(i.lowPortStart) +
                    (i.lowPortStart > 1000 ? " (System Link hosting needs 1000 or lower)" : ""));
    v.push_back(std::string("Steam Deck: ") + (i.steamDeck ? i.deckModel : "no") + (i.steamOS ? ", SteamOS" : "") +
                (i.gamescope ? ", in gamescope (Game Mode)" : "") + (i.zDrive ? "" : ", no Z: drive"));
    return v;
}

// ---- paths ------------------------------------------------------------------------------------------------------------
std::string clean_pasted(const std::string &s0)
{
    std::string s = first_line(s0);
    while (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = trim(s.substr(1, s.size() - 2));
    if (lower(s.substr(0, 7)) == "file://") {
        s = s.substr(7);
        if (!s.empty() && s[0] != '/') {   // file://host/path: the host is dropped ("localhost" or empty)
            const size_t sl = s.find('/');
            s = sl == std::string::npos ? "" : s.substr(sl);
        }
        std::string d;   // %xx decoding
        for (size_t k = 0; k < s.size(); k++) {
            if (s[k] == '%' && k + 2 < s.size() && std::isxdigit((unsigned char)s[k + 1]) && std::isxdigit((unsigned char)s[k + 2])) {
                d += (char)std::stoi(s.substr(k + 1, 2), nullptr, 16);
                k += 2;
            } else d += s[k];
        }
        s = d;
    }
    return s;
}

bool looks_unix(const std::string &s) { return !s.empty() && (s[0] == '/' || s == "~" || s.rfind("~/", 0) == 0); }

std::string to_wine(const std::string &input, const std::string &home)
{
    std::string s = clean_pasted(input);
    if (s.size() >= 2 && std::isalpha((unsigned char)s[0]) && s[1] == ':') {   // already a Windows path
        for (char &c : s) if (c == '/') c = '\\';
        return s;
    }
    if (!looks_unix(s)) return "";
    if (s[0] == '~') {
        if (home.empty()) return "";
        s = home + s.substr(1);
    }
    std::string out = "Z:";
    char prev = 0;
    for (char c : s) {
        if (c == '/') c = '\\';
        if (c == '\\' && prev == '\\') continue;   // "//" -> one
        out += c;
        prev = c;
    }
    if (out.size() > 3 && out.back() == '\\') out.pop_back();
    if (out == "Z:") out = "Z:\\";
    return out;
}

std::string to_unix(const std::string &p)
{
    std::string rest;
    if (p.size() >= 2 && (p[0] == 'Z' || p[0] == 'z') && p[1] == ':') rest = p.substr(2);
    else if (lower(p.substr(0, 9)) == "\\\\?\\unix\\") rest = p.substr(8);
    else if (lower(p.substr(0, 6)) == "\\\\?\\z:") rest = p.substr(6);
    else return "";
    for (char &c : rest) if (c == '\\') c = '/';
    if (rest.empty() || rest[0] != '/') rest = "/" + rest;
    if (rest.size() > 1 && rest.back() == '/') rest.pop_back();
    return rest;
}

std::string display_path(const std::string &path, bool wine)
{
    if (!wine) return path;
    const std::string u = to_unix(path);
    return u.empty() ? path : u;
}

// ---- where to look ----------------------------------------------------------------------------------------------------
std::vector<std::string> parse_libraryfolders(const std::string &vdf)
{
    // "path"		"/home/deck/.local/share/Steam"  (VDF: quoted key, whitespace, quoted value; \\ escapes)
    std::vector<std::string> out;
    size_t at = 0;
    while ((at = vdf.find("\"path\"", at)) != std::string::npos) {
        at += 6;
        size_t q = vdf.find('"', at);
        const size_t nl = vdf.find('\n', at);
        if (q == std::string::npos || (nl != std::string::npos && q > nl)) continue;
        std::string v;
        for (size_t k = q + 1; k < vdf.size() && vdf[k] != '"' && vdf[k] != '\n'; k++) {
            if (vdf[k] == '\\' && k + 1 < vdf.size()) k++;
            v += vdf[k];
        }
        if (!v.empty() && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
        at = q + 1;
    }
    return out;
}

std::string version_from_file(const std::string &text)
{
    std::istringstream in(first_line(text));
    std::string tok, last;
    while (in >> tok) last = tok;
    return last;
}

std::string proton_dir(const std::string &toolPaths)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : toolPaths) { if (c == ':') { parts.push_back(cur); cur.clear(); } else cur += c; }
    parts.push_back(cur);
    for (const std::string &p : parts) if (lower(basename_of(p)).find("proton") != std::string::npos) return p;
    for (const std::string &p : parts) if (!p.empty() && lower(p).find("runtime") == std::string::npos) return p;
    return "";
}

std::vector<std::string> scan_roots(const std::string &home, const std::string &user, const std::vector<std::string> &steamLibs,
                                    const std::vector<std::string> &mediaDirs)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](std::string p) {
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (!p.empty() && p[0] == '/' && seen.insert(p).second) out.push_back(p);
    };
    if (!home.empty()) {
        for (const char *d : { "/Games", "/games", "/Downloads", "/Desktop", "/Documents" }) add(home + d);
    }
    for (const std::string &l : steamLibs) add(l + "/steamapps/common");
    for (const std::string &m : mediaDirs) add(m);   // SD cards / USB drives first: the Deck's usual place for big files
    if (!home.empty()) {
        add(home + "/.local/share/Steam/steamapps/common");
        add(home + "/.local/share");
    }
    (void)user;
    return out;
}

std::vector<std::filesystem::path> find_copies(const std::vector<std::filesystem::path> &roots,
                                               const std::function<bool(const std::filesystem::path &, bool)> &isGame,
                                               int maxDepth, int budget, double seconds)
{
    namespace fs = std::filesystem;
    static const char *const kSkip[] = { "compatdata", "shadercache", "steamapps/temp", "downloading", "pfx", "Trash",
                                         "flatpak", "node_modules", "proc", "sys", "dev", "snap", "containers",
                                         "icons", "fonts", "mime", "applications", "recently-used.xbel", nullptr };
    std::vector<fs::path> found;
    std::set<std::string> seen;
    int visited = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::function<void(const fs::path &, int)> walk = [&](const fs::path &dir, int depth) {
        if (visited >= budget) return;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > seconds) { visited = budget; return; }
        visited++;
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        std::vector<fs::path> subdirs;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::path p = it->path();
            const std::string name = p.filename().u8string();
            std::error_code e2;
            const bool isDir = it->is_directory(e2) && !it->is_symlink(e2);
            if (isDir) {
                if (name.empty() || name[0] == '.') continue;   // hidden below a root (the roots themselves may be hidden)
                bool skip = false;
                for (const char *const *k = kSkip; *k; k++) if (name == *k) skip = true;
                if (skip) continue;
                if (isGame(p, true)) { if (seen.insert(p.u8string()).second) found.push_back(p); continue; }
                subdirs.push_back(p);
            } else if (it->is_regular_file(e2)) {
                std::string ext = lower(p.extension().u8string());
                if (isGame(p, false) && seen.insert(p.u8string()).second) found.push_back(p);
            }
            if (found.size() >= 200) return;
        }
        if (depth >= maxDepth) return;
        std::sort(subdirs.begin(), subdirs.end());
        for (const fs::path &s : subdirs) walk(s, depth + 1);
    };
    for (const fs::path &r : roots) {
        std::error_code ec;
        if (!fs::is_directory(r, ec)) continue;
        if (isGame(r, true)) { if (seen.insert(r.u8string()).second) found.push_back(r); continue; }
        walk(r, 1);
    }
    return found;
}

static bool package_name(const std::string &n)
{
    if (n.size() < 40) return false;
    for (char c : n) if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

bool quick_candidate(const std::filesystem::path &p, bool isDir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!isDir) {
        const std::string ext = lower(p.extension().u8string());
        return ext == ".iso" || ext == ".god" || (ext.empty() && package_name(p.filename().u8string()));
    }
    auto has = [&](const fs::path &d, const char *name) {
        std::error_code e;
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, e), end; !e && it != end; it.increment(e))
            if (lower(it->path().filename().u8string()) == name) return true;
        return false;
    };
    if (fs::is_regular_file(p / "default.xex", ec) || has(p, "default.xex")) return true;
    const std::string self = lower(p.filename().u8string());
    if (self == "00007000" || self == "000b0000") return true;
    int n = 0;
    for (fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && n < 400; it.increment(ec), n++) {
        std::error_code e2;
        const std::string name = lower(it->path().filename().u8string());
        if (it->is_directory(e2)) {
            if (name == "00007000" || name == "000b0000") return true;
            if (name.size() == 8 && package_name(name + "00000000000000000000000000000000") &&   // a title ID folder (4D5307E6)
                (has(it->path(), "00007000") || has(it->path(), "000b0000"))) return true;
        } else if (it->path().extension().empty() && package_name(it->path().filename().u8string())) return true;
    }
    return false;
}

#ifdef _WIN32
// ---- this process (Windows / Wine) --------------------------------------------------------------------------------------
static std::wstring wide(const std::string &s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string utf8(const std::wstring &w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::string env_var(const char *k)
{
    wchar_t b[4096];
    const DWORD n = GetEnvironmentVariableW(wide(k).c_str(), b, 4096);
    return n > 0 && n < 4096 ? utf8(std::wstring(b, n)) : std::string();
}
static bool read_unix(const std::string &unixPath, std::string &out)
{
    const std::string w = to_wine(unixPath, "");
    if (w.empty()) return false;
    std::ifstream f(std::filesystem::path(wide(w)), std::ios::binary);
    if (!f) return false;
    char b[8192];
    f.read(b, sizeof b);
    out.assign(b, (size_t)f.gcount());
    return true;
}

Info detect_here(const std::vector<std::string> &gpus)
{
    Inputs in;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    typedef const char *(__cdecl * StrFn)();
    typedef void(__cdecl * HostFn)(const char **, const char **);
    if (StrFn v = nt ? (StrFn)(void *)GetProcAddress(nt, "wine_get_version") : nullptr) in.wineVersion = v() ? v() : "";
    if (in.wineVersion.empty()) return Info();   // Windows proper
    if (StrFn b = (StrFn)(void *)GetProcAddress(nt, "wine_get_build_id")) in.wineBuild = b() ? b() : "";
    if (HostFn h = (HostFn)(void *)GetProcAddress(nt, "wine_get_host_version")) {
        const char *sys = nullptr, *rel = nullptr;
        h(&sys, &rel);
        in.hostSystem = sys ? sys : "";
        in.hostRelease = rel ? rel : "";
    }
    in.zDrive = GetDriveTypeW(L"Z:\\") != DRIVE_NO_ROOT_DIR && GetFileAttributesW(L"Z:\\etc") != INVALID_FILE_ATTRIBUTES;
    in.env = env_var;
    in.readUnix = in.zDrive ? read_unix : std::function<bool(const std::string &, std::string &)>();
    in.gpus = gpus;
    return detect(in);
}

std::vector<std::string> roots_here(const Info &i)
{
    std::vector<std::string> out;
    if (!i.wine || !i.zDrive) return out;
    namespace fs = std::filesystem;
    // Steam libraries (the client's own list), then SD cards / USB drives
    std::vector<std::string> libs;
    std::vector<std::string> vdfs;
    if (!i.home.empty()) {
        vdfs.push_back(i.home + "/.local/share/Steam/steamapps/libraryfolders.vdf");
        vdfs.push_back(i.home + "/.steam/steam/steamapps/libraryfolders.vdf");
    }
    const std::string client = env_var("STEAM_COMPAT_CLIENT_INSTALL_PATH");
    if (!client.empty()) vdfs.push_back(client + "/steamapps/libraryfolders.vdf");
    for (const std::string &v : vdfs) {
        std::string t;
        if (!read_unix(v, t)) continue;
        std::ifstream f(fs::path(wide(to_wine(v, ""))), std::ios::binary);   // read_unix keeps 8 KB; the list can be longer
        std::stringstream ss;
        ss << f.rdbuf();
        for (const std::string &l : parse_libraryfolders(ss.str())) if (std::find(libs.begin(), libs.end(), l) == libs.end()) libs.push_back(l);
    }
    std::vector<std::string> media;
    auto list_dirs = [&](const std::string &unixDir, bool alsoChildren) {
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(wide(to_wine(unixDir, ""))), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            const std::string name = utf8(it->path().filename().wstring());
            const std::string u = unixDir + "/" + name;
            if (unixDir == "/Volumes") {   // the Mac's own disk (= /, any name) is too big to search: it holds macOS itself
                std::error_code e4;
                if (name.rfind("Macintosh HD", 0) == 0 || fs::exists(it->path() / L"System" / L"Library" / L"CoreServices", e4)) continue;
            }
            if (alsoChildren && name == i.user) {   // /run/media/deck/<card label> (SteamOS 3.5+)
                std::error_code e3;
                for (fs::directory_iterator c(it->path(), e3); !e3 && c != end; c.increment(e3))
                    if (c->is_directory(e3)) media.push_back(u + "/" + utf8(c->path().filename().wstring()));
            } else media.push_back(u);   // /run/media/mmcblk0p1 (older SteamOS), other users' drives
        }
    };
    if (i.hostSystem == "Darwin") list_dirs("/Volumes", false);   // 0.6.2 review: a Mac's external drives (CrossOver)
    else {
        list_dirs("/run/media", true);
        if (!i.user.empty()) list_dirs("/media/" + i.user, false);
        list_dirs("/mnt", false);
    }
    std::set<std::string> seen;   // Wine finds names case-insensitively: ~/Games and ~/games can be one folder
    for (const std::string &r : scan_roots(i.home, i.user, libs, media)) {
        const std::string w = to_wine(r, i.home);
        std::error_code ec;
        if (w.empty() || !fs::is_directory(fs::path(wide(w)), ec)) continue;
        std::error_code e2;
        const fs::path canon = fs::canonical(fs::path(wide(w)), e2);
        if (!seen.insert(lower(e2 ? w : utf8(canon.wstring()))).second) continue;
        out.push_back(w);
    }
    return out;
}
#endif

}  // namespace proton
