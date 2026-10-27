// COGBox - mods (0.2.0). Each game has <Game>\mods\<mod>\: a folder laid out like the game's own data folder (for
// example <mod>\GearGame\CookedXenon\SomePackage.xxx), with an optional mod.ini:
//     [Mod]
//     Name = ...      Version = ...      Author = ...      Description = ...
// The enabled mods, in load order (a later one wins), go to the game as --ue3_mods; the game shows their files in
// place of (or beside) its own, without changing its folder. Programs (.xex, .dll, .exe) are never used from a mod.
// Order and on/off are in CogBox.ini [Mods] <game>Order / <game>Enabled ("name|name|...").
#include "app.h"
#include "../third_party/miniz/miniz.h"

#include <windows.h>
#include <algorithm>
#include <set>
#include <filesystem>
#include <cstring>

static std::vector<std::string> split_bar(const std::string &s)
{
    std::vector<std::string> v;
    size_t at = 0;
    while (at <= s.size()) {
        size_t e = s.find('|', at);
        if (e == std::string::npos) e = s.size();
        if (e > at) v.push_back(s.substr(at, e - at));
        at = e + 1;
    }
    return v;
}
static std::string join_bar(const std::vector<std::string> &v)
{
    std::string s;
    for (const std::string &x : v) s += (s.empty() ? "" : "|") + x;
    return s;
}
static std::string lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }

std::string mods_dir(int g) { return join(game_root(g), "mods"); }

static std::string ini_value(const std::string &text, const std::string &key)
{
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(at, e - at);
        at = e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto trim = [](std::string x) { size_t a = x.find_first_not_of(" \t\r"), b = x.find_last_not_of(" \t\r"); return a == std::string::npos ? std::string() : x.substr(a, b - a + 1); };
        if (lower(trim(k)) == lower(key)) return trim(v);
    }
    return "";
}

std::vector<ModItem> mods_list(App &a, int g)
{
    const GameDef &d = game_def(g);
    std::vector<std::string> found;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((widen(mods_dir(g)) + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            std::string s = narrow(n);
            if (s.find('|') != std::string::npos || s.find(".partial") != std::string::npos) continue;
            found.push_back(s);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::vector<std::string> order = split_bar(a.ini.get("Mods", std::string(d.key) + "Order"));
    std::set<std::string> enabled;
    for (const std::string &e : split_bar(a.ini.get("Mods", std::string(d.key) + "Enabled"))) enabled.insert(lower(e));
    std::vector<std::string> sorted;
    for (const std::string &o : order)
        for (const std::string &x : found) if (lower(x) == lower(o) && std::find(sorted.begin(), sorted.end(), x) == sorted.end()) sorted.push_back(x);
    std::vector<std::string> rest;
    for (const std::string &x : found) if (std::find(sorted.begin(), sorted.end(), x) == sorted.end()) rest.push_back(x);
    std::sort(rest.begin(), rest.end());
    sorted.insert(sorted.end(), rest.begin(), rest.end());
    std::vector<ModItem> out;
    for (const std::string &x : sorted) {
        ModItem m;
        m.folder = x;
        m.path = join(mods_dir(g), x);
        m.enabled = enabled.count(lower(x)) > 0;
        std::string ini;
        if (read_file(join(m.path, "mod.ini"), ini, 1 << 16)) {
            m.name = ini_value(ini, "Name");
            m.version = ini_value(ini, "Version");
            m.author = ini_value(ini, "Author");
            m.description = ini_value(ini, "Description");
        }
        if (m.name.empty()) m.name = x;
        out.push_back(m);
    }
    return out;
}

static void store(App &a, int g, const std::vector<ModItem> &v)
{
    const GameDef &d = game_def(g);
    std::vector<std::string> order, en;
    for (const ModItem &m : v) { order.push_back(m.folder); if (m.enabled) en.push_back(m.folder); }
    a.ini.set("Mods", std::string(d.key) + "Order", join_bar(order));
    a.ini.set("Mods", std::string(d.key) + "Enabled", join_bar(en));
    a.ini.save();
}

void mods_set_enabled(App &a, int g, const std::string &folder, bool on)
{
    std::vector<ModItem> v = mods_list(a, g);
    for (ModItem &m : v) if (m.folder == folder) m.enabled = on;
    store(a, g, v);
}

void mods_move(App &a, int g, const std::string &folder, int dir)
{
    std::vector<ModItem> v = mods_list(a, g);
    int i = -1;
    for (size_t k = 0; k < v.size(); k++) if (v[k].folder == folder) i = (int)k;
    int j = i + dir;
    if (i < 0 || j < 0 || i >= (int)v.size() || j >= (int)v.size()) return;
    std::swap(v[i], v[j]);
    store(a, g, v);
}

std::vector<std::string> mods_enabled_paths(App &a, int g)
{
    std::vector<std::string> p;
    for (const ModItem &m : mods_list(a, g)) if (m.enabled) p.push_back(m.path);
    return p;
}

// A mod in a .zip: unpacked into mods\<zip name>\ (a single top folder in the zip that isn't one of the game's own
// folders is taken as the mod's folder). Only plain relative paths are accepted; programs are left out.
// a folder COGBox made itself (the unpacking of a mod), removed with everything in it
static void remove_dir_tree(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) remove_dir_tree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

bool mods_add_zip(App &a, int g, const std::string &zipPath, std::string &err)
{
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    std::wstring tempCopy;   // miniz takes a narrow path: a non-ASCII one is read from a copy in the COGBox folder
    struct TempGone { std::wstring &p; ~TempGone() { if (!p.empty()) DeleteFileW(p.c_str()); } } tempGone{tempCopy};
    if (!mz_zip_reader_init_file(&z, zipPath.c_str(), 0)) {
        tempCopy = widen(join(exe_dir(), "mod-import.tmp"));   // the COGBox folder: ASCII-safe as far as COGBox runs at all
        if (!CopyFileW(widen(zipPath).c_str(), tempCopy.c_str(), FALSE) || !mz_zip_reader_init_file(&z, narrow(tempCopy).c_str(), 0)) {
            err = "This file isn't a zip COGBox can read.";
            return false;
        }
    }
    const int n = (int)mz_zip_reader_get_num_files(&z);
    if (n <= 0 || n > 50000) { mz_zip_reader_end(&z); err = "The zip is empty or too large."; return false; }
    std::vector<std::string> names(n);
    std::string common;
    bool oneTop = true;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) { mz_zip_reader_end(&z); err = "The zip is damaged."; return false; }
        std::string nm = st.m_filename;
        for (char &c : nm) if (c == '\\') c = '/';
        if (nm.empty() || nm[0] == '/' || nm.find(':') != std::string::npos || ("/" + nm + "/").find("/../") != std::string::npos) {
            mz_zip_reader_end(&z); err = "The zip has a file with an unsafe path (" + nm + ") - not added."; return false;
        }
        total += st.m_uncomp_size;
        names[i] = nm;
        std::string top = nm.substr(0, nm.find('/'));
        if (nm.find('/') == std::string::npos && !st.m_is_directory) oneTop = false;
        if (common.empty()) common = top; else if (lower(common) != lower(top)) oneTop = false;
    }
    if (total > (8ull << 30)) { mz_zip_reader_end(&z); err = "The zip unpacks to more than 8 GB - not added."; return false; }
    // is the single top folder the mod's own, or one of the game's (GearGame...)? The game's own: keep it.
    if (oneTop && !a.st[g].dataDir.empty() && dir_exists(join(a.st[g].dataDir, common))) oneTop = false;
    std::string base = narrow(std::filesystem::path(widen(zipPath)).stem().wstring()), name;
    if (oneTop) base = common;
    for (char c : base) if (isalnum((unsigned char)c) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')') name += c;
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty()) name = "mod";
    std::string dest = join(mods_dir(g), name);
    if (dir_exists(dest)) { mz_zip_reader_end(&z); err = "A mod named \"" + name + "\" is already there - remove or rename it first."; return false; }
    std::string part = dest + ".partial";
    remove_dir_tree(widen(part));   // what an earlier, failed try left
    make_dirs(part);
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        std::string nm = names[i];
        if (oneTop) { size_t s = nm.find('/'); nm = s == std::string::npos ? "" : nm.substr(s + 1); }
        if (nm.empty() || nm.back() == '/') continue;
        std::string ext = lower(nm.substr(nm.find_last_of('.') == std::string::npos ? nm.size() : nm.find_last_of('.')));
        if (ext == ".xex" || ext == ".xexp" || ext == ".dll" || ext == ".exe") continue;
        std::string out = part;
        for (char c : nm) out += c == '/' ? '\\' : c;
        make_dirs(out.substr(0, out.find_last_of('\\')));
        size_t sz = 0;
        void *p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
        if (!p) { ok = false; err = "Couldn't unpack " + nm + "."; break; }
        FILE *f = _wfopen(widen(out).c_str(), L"wb");
        ok = f && fwrite(p, 1, sz, f) == sz;
        if (f) fclose(f);
        mz_free(p);
        if (!ok) err = "Couldn't write " + out + " (is the disk full?).";
    }
    mz_zip_reader_end(&z);
    if (ok && !MoveFileExW(widen(part).c_str(), widen(dest).c_str(), 0)) { ok = false; err = "The unpacked mod couldn't be moved into place."; }
    if (!ok) { remove_dir_tree(widen(part)); return false; }
    // new mods start enabled, last in the order
    std::vector<ModItem> v = mods_list(a, g);
    for (ModItem &m : v) if (m.folder == name) m.enabled = true;
    store(a, g, v);
    return true;
}
