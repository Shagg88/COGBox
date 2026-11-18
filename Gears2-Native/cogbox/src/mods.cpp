// COGBox - mods (0.2.0). Each game has <Game>\mods\<mod>\: a folder laid out like the game's own data folder (for
// example <mod>\GearGame\CookedXenon\SomePackage.xxx), with an optional mod.ini:
//     [Mod]
//     Name = ...      Version = ...      Author = ...      Description = ...
// The enabled mods, in load order (a later one wins), go to the game as --ue3_mods; the game shows their files in
// place of (or beside) its own, without changing its folder. Programs (mod_file_rules.h) are never used from a mod.
// Order and on/off are in CogBox.ini [Mods] <game>Order / <game>Enabled ("name|name|...").
#include "app.h"
#include "fabricator_model.h"
#include "mod_file_rules.h"
#include "i18n.h"   // 0.6.4: the Mods tab lines built here (trf)
#include "../third_party/miniz/miniz.h"
#include "modcore/campaigns.h"

#include <windows.h>
#include <algorithm>
#include <set>
#include <filesystem>
#include <cstring>
#include <cwctype>

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

// KiltedKernel (mod API 1): Gears 3's mods through the game's own rules (modcore, MIT: Gears3-Native's
// src/layer2_ue3/modcore, compiled into CogBox by build.sh), so the list says what the game will do: a mod left out
// and why (another game's, a required mod missing or off), the act and chapters the campaign registry gives a
// campaign, and the mod.ini keys the game does not understand. Worked out again only when the mods, their order or
// their files change (the list is read every 2 s).
static void apply_mod_rules(int g, std::vector<ModItem> &out)
{
    if (std::string(game_def(g).key) != "gears3") return;
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string sig = i18n_current() + ";";   // review r1940: its lines are translated here, so a language switch rebuilds them
    for (const ModItem &m : out) {
        sig += m.folder + (m.enabled ? "+" : "-");
        const fs::path dir = fs::path(widen(m.path));
        for (const fs::path &f : { dir / L"mod.ini", dir / L"config" }) {
            auto t = fs::last_write_time(f, ec);
            sig += ec ? "|" : "|" + std::to_string((long long)t.time_since_epoch().count());
        }
    }
    struct Result { std::string problem, campaign; std::vector<std::string> notes; };
    static std::string s_sig;
    static std::vector<Result> s_res;
    if (sig != s_sig || s_res.size() != out.size()) {
        s_sig = sig;
        s_res.assign(out.size(), Result{});
        std::vector<fs::path> enabled;
        for (const ModItem &m : out) if (m.enabled) enabled.push_back(fs::path(widen(m.path)));
        const std::vector<modcore::Manifest> on = modcore::FromFolders(enabled, "gears3");
        // the slots: after the acts and chapters the game's config has once the mods' own fragments are in
        std::string base;
        read_file(join(game_root(g), "game\\extracted\\GearGame\\CookedXbox360\\Coalesced_INT.bin"), base, 64 << 20);
        std::vector<std::pair<std::string, fs::path>> fragments;
        for (const modcore::Manifest &m : on)
            if (m.Active()) for (const fs::path &f : modcore::Fragments(m)) fragments.emplace_back(m.name, f);
        const std::vector<uint8_t> merged = modcore::MergeConfig(std::vector<uint8_t>(base.begin(), base.end()), fragments);
        const modcore::Allocation alloc = merged.empty() ? modcore::Allocation{} : modcore::AllocateCampaigns(on, modcore::UsedCampaignSlots(merged));
        size_t k = 0;
        for (size_t i = 0; i < out.size(); i++) {
            Result &r = s_res[i];
            const modcore::Manifest m = out[i].enabled ? on[k++] : modcore::Read(fs::path(widen(out[i].path)));
            r.notes = m.warnings;
            if (!out[i].enabled) continue;
            r.problem = m.problem;
            if (m.api > modcore::kModApiLevel)
                r.notes.push_back(trf("made for a newer COGBox (mod API {1}; this one has {2})",
                                      { std::to_string(m.api), std::to_string(modcore::kModApiLevel) }));
            for (const std::string &p : alloc.problems)
                if (p.rfind(m.name + ": ", 0) == 0) r.notes.push_back(p.substr(m.name.size() + 2));
            for (const modcore::PlacedCampaign &c : alloc.campaigns) {
                std::string mine;
                for (const modcore::PlacedChapter &ch : c.chapters)
                    if (ch.mod == m.name) mine += (mine.empty() ? "" : ", ") + std::to_string(ch.decl.number);
                if (!mine.empty())
                    r.campaign = trf(mine.find(',') == std::string::npos ? "Select Chapter: Act {1} {2}, chapter {3}"
                                                                         : "Select Chapter: Act {1} {2}, chapters {3}",
                                     { modcore::ActNumeral(c.act), c.title, mine });
            }
            if (!r.campaign.empty() && m.campaign.max_players > 0)   // ([Campaign] MaxPlayers)
                r.campaign += " " + (m.campaign.max_players == 1 ? tr(std::string("(one player)"))
                                                                 : trf("(co-op up to {1})", { std::to_string(m.campaign.max_players) }));
            if (m.Active() && !m.chapters.empty() && r.campaign.empty() && merged.empty())
                r.notes.push_back(tr(std::string("its chapters' place in Select Chapter shows once the game's files are in")));
        }
    }
    for (size_t i = 0; i < out.size(); i++) {
        out[i].problem = s_res[i].problem;
        out[i].campaign = s_res[i].campaign;
        out[i].notes = s_res[i].notes;
    }
}

// KiltedKernel: texture packs. A mod's folder may hold DDS files named after the hash of the texture each replaces
// (`<hash16>_....dds`), anywhere in it - a pack made for the ReXGlue Gears 2 port keeps its `<TITLEID>\replace` folder as
// it comes. Folders named dump, .build and replace_<variant> are left aside, as the game's renderer does
// (layer0_xenos/renderer/texture_pack.cpp). Counted again at most every 10 s (the list is read every 2 s).
static bool pack_texture_name(const std::wstring &n)
{
    if (n.size() < 20 || lower(narrow(n.substr(n.size() - 4))) != ".dds") return false;
    for (size_t i = 0; i < 16; i++) if (!iswxdigit(n[i])) return false;
    return n[16] == L'_' || n[16] == L'.';
}
static uint32_t count_pack_textures(const std::wstring &dir, int depth)
{
    uint32_t n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            const std::string l = lower(narrow(name));
            if (depth < 6 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && l != "dump" && l != ".build" &&
                l.rfind("replace_", 0) != 0)
                n += count_pack_textures(dir + L"\\" + name, depth + 1);
        } else if (pack_texture_name(name)) {
            n++;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}
static uint32_t pack_textures(const std::string &path)
{
    struct Counted { Uint64 at; uint32_t n; };
    static std::map<std::string, Counted> s_counted;
    const Uint64 now = SDL_GetTicks();
    auto it = s_counted.find(path);
    if (it != s_counted.end() && now - it->second.at < 10000) return it->second.n;
    const uint32_t n = count_pack_textures(widen(path), 0);
    s_counted[path] = { now, n };
    return n;
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
            // hidden: names starting with '.' (the Fabricator's .fabricator-backup) and unpackings not finished (.partial)
            if (s.find('|') != std::string::npos || s[0] == '.' ||
                (s.size() >= 8 && lower(s.substr(s.size() - 8)) == ".partial")) continue;
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
        if (auto r = fab_read_record(m.path)) { m.fab = true; m.fab_source = r->source.kind; }   // "from the Fabricator"
        out.push_back(m);
    }
    apply_mod_rules(g, out);
    for (ModItem &m : out) m.textures = pack_textures(m.path);
    return out;
}

void mods_store(App &a, int g, const std::vector<ModItem> &v)
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
    mods_store(a, g, v);
}

void mods_move(App &a, int g, const std::string &folder, int dir)
{
    std::vector<ModItem> v = mods_list(a, g);
    int i = -1;
    for (size_t k = 0; k < v.size(); k++) if (v[k].folder == folder) i = (int)k;
    int j = i + dir;
    if (i < 0 || j < 0 || i >= (int)v.size() || j >= (int)v.size()) return;
    std::swap(v[i], v[j]);
    mods_store(a, g, v);
}

// a folder COGBox made itself (the unpacking of a mod, a Fabricator backup), removed with everything in it; a link
// (junction, symlink) inside it is removed, never followed
void mods_remove_tree(const std::wstring &dir)
{
    const DWORD at = GetFileAttributesW(dir.c_str());
    if (at == INVALID_FILE_ATTRIBUTES) return;
    if (at & FILE_ATTRIBUTE_REPARSE_POINT) { RemoveDirectoryW(dir.c_str()); return; }   // a link itself: only the link goes
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) mods_remove_tree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

void mods_turn_on_last(App &a, int g, const std::string &folder)
{
    std::vector<ModItem> v = mods_list(a, g);
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].folder == folder) {
            ModItem m = v[i];
            m.enabled = true;
            v.erase(v.begin() + (long)i);
            v.push_back(m);
            break;
        }
    mods_store(a, g, v);
}

// ---- unpacking a mod zip (the Mods tab's "add a zip" and the Fabricator) ----
// The same path rules as the Fabricator's zip check (fab_check_zip): no absolute, drive-letter (C:...), stream (a:b),
// UNC or rooted path and no ".." - also as Windows reads a component, dropping its trailing dots and spaces (".. " and
// "..." are ".." / "" to it). The bytes really inflated are counted, not the sizes the zip declares, and the unpacking
// stops at the limits. Program files (mod_file_rules.h) and any fabricator.json are left out.
static const uint64_t kUnpackMaxBytes = 8ull << 30;   // 8 GiB unpacked
static const mz_uint kUnpackMaxFiles = 50000;

struct ZipReader {   // a wide path: folder and file names can be non-ASCII (miniz itself takes a narrow one)
    FILE *f = nullptr;
    mz_zip_archive z;
    bool ok = false;
    explicit ZipReader(const std::string &path)
    {
        memset(&z, 0, sizeof z);
        f = _wfopen(widen(path).c_str(), L"rb");
        ok = f && mz_zip_reader_init_cfile(&z, f, 0, 0);
    }
    ~ZipReader() { if (ok) mz_zip_reader_end(&z); if (f) fclose(f); }
};

// The components Windows will create for a zip entry (trailing dots / spaces dropped, "." and empty ones skipped);
// false = an unsafe path.
static bool entry_parts(const std::string &raw, std::vector<std::string> &parts)
{
    parts.clear();
    if (raw.empty() || raw.find(':') != std::string::npos) return false;
    std::string nm = raw;
    for (char &c : nm) if (c == '\\') c = '/';
    if (nm[0] == '/') return false;
    for (size_t at = 0; at <= nm.size();) {
        size_t end = nm.find('/', at);
        if (end == std::string::npos) end = nm.size();
        const std::string seg = nm.substr(at, end - at);
        std::string t = seg;
        while (!t.empty() && (t.back() == '.' || t.back() == ' ')) t.pop_back();
        at = end + 1;
        if (seg.empty() || seg == ".") continue;
        if (t.empty()) return false;   // "..", "...", ".. "
        parts.push_back(t);
    }
    return true;
}

static bool device_name(const std::string &comp)   // CON, NUL, COM1 ... (also "con.txt"): a device to Windows, not a file
{
    std::string stem = lower(comp.substr(0, comp.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" || stem == "conout$") return true;
    return stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && stem[3] >= '1' && stem[3] <= '9';
}

struct ZipEntry { mz_uint index; std::string rel; };   // rel: under the mod's folder, '\' separated
struct ZipPlan {
    std::vector<ZipEntry> files;
    std::string top;                     // the single top folder taken off ("" = none)
    std::vector<std::string> left_out;   // the program files not unpacked (rel paths)
};

static bool zip_plan(const std::string &dataDir, ZipReader &zr, bool stripTop, ZipPlan &plan, std::string &err)
{
    const mz_uint n = mz_zip_reader_get_num_files(&zr.z);
    if (n == 0 || n > kUnpackMaxFiles) { err = "The zip is empty or too large."; return false; }
    std::vector<std::vector<std::string>> parts(n);
    std::vector<bool> isDir(n);
    std::string common;
    bool oneTop = true;
    uint64_t declared = 0;
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zr.z, i, &st)) { err = "The zip is damaged."; return false; }
        const std::string nm = st.m_filename;
        if (mz_zip_reader_get_filename(&zr.z, i, nullptr, 0) != nm.size() + 1 || !entry_parts(nm, parts[i])) {   // (+ an embedded NUL)
            err = "The zip has a file with an unsafe path (" + nm + ") - not added.";
            return false;
        }
        isDir[i] = st.m_is_directory || nm.back() == '/' || nm.back() == '\\';
        declared += st.m_uncomp_size;
        if (parts[i].empty()) continue;
        if (!isDir[i] && parts[i].size() == 1) oneTop = false;
        if (common.empty()) common = parts[i][0]; else if (lower(common) != lower(parts[i][0])) oneTop = false;
    }
    if (declared > kUnpackMaxBytes) { err = "The zip unpacks to more than 8 GB - not added."; return false; }
    // is the single top folder the mod's own, or one of the game's (GearGame...)? The game's own: keep it.
    if (common.empty() || (!dataDir.empty() && dir_exists(join(dataDir, common)))) oneTop = false;
    plan.top = stripTop && oneTop ? common : "";
    for (mz_uint i = 0; i < n; i++) {
        if (isDir[i]) continue;
        std::vector<std::string> p = parts[i];
        if (!plan.top.empty()) p.erase(p.begin());
        if (p.empty()) continue;
        bool device = false, record = false;
        for (const std::string &c : p) {
            device = device || device_name(c);
            record = record || lower(c) == "fabricator.json";   // COGBox's install record (or a folder of that name)
        }
        if (device || record) continue;   // never from a zip: only COGBox writes a fabricator.json
        std::string rel;
        for (const std::string &c : p) rel += (rel.empty() ? "" : "\\") + c;
        if (mod_file_is_program(p.back())) { plan.left_out.push_back(rel); continue; }   // left out, with a note
        plan.files.push_back({i, rel});
    }
    return true;
}

struct UnpackSink { FILE *f; uint64_t *total; bool over = false, writeFailed = false; };
static size_t unpack_cb(void *ud, mz_uint64, const void *buf, size_t len)
{
    UnpackSink *s = (UnpackSink *)ud;
    *s->total += len;   // the bytes really inflated
    if (*s->total > kUnpackMaxBytes) { s->over = true; return 0; }
    if (fwrite(buf, 1, len, s->f) != len) { s->writeFailed = true; return 0; }
    return len;
}

bool mods_unpack_zip(App &a, int g, const std::string &zipPath, const std::string &folder, bool stripTop, std::string &err,
                     std::vector<std::string> *left_out)
{
    return mods_unpack_zip_in(a.st[g].dataDir, zipPath, folder, stripTop, err, left_out);
}

bool mods_unpack_zip_in(const std::string &dataDir, const std::string &zipPath, const std::string &folder, bool stripTop, std::string &err,
                        std::vector<std::string> *left_out)
{
    ZipReader zr(zipPath);
    if (!zr.ok) { err = "This file isn't a zip COGBox can read."; return false; }
    ZipPlan plan;
    if (!zip_plan(dataDir, zr, stripTop, plan, err)) return false;
    if (left_out) left_out->insert(left_out->end(), plan.left_out.begin(), plan.left_out.end());
    if (!dir_exists(folder) && !make_dirs(folder)) { err = "Couldn't make the mod's folder (is the disk full?)."; return false; }
    uint64_t total = 0;
    for (const ZipEntry &e : plan.files) {
        const std::string out = folder + "\\" + e.rel;
        const size_t cut = out.find_last_of('\\');
        if (cut > folder.size()) make_dirs(out.substr(0, cut));
        FILE *f = _wfopen(widen(out).c_str(), L"wb");
        if (!f) { err = "Couldn't write " + e.rel + " (is the disk full?)."; return false; }
        UnpackSink s{f, &total};
        bool ok = mz_zip_reader_extract_to_callback(&zr.z, e.index, unpack_cb, &s, 0) != 0;
        ok = fclose(f) == 0 && ok;
        if (!ok) {
            DeleteFileW(widen(out).c_str());   // no half-written file left
            err = s.over ? "The zip unpacks to more than 8 GB - not added."
                : s.writeFailed ? "Couldn't write " + e.rel + " (is the disk full?)."
                                : "Couldn't unpack " + e.rel + " (the zip is damaged).";
            return false;
        }
    }
    return true;
}

// A mod in a .zip: unpacked into mods\<zip name>\ (a single top folder in the zip that isn't one of the game's own
// folders is taken as the mod's folder), through <name>.partial so a failure leaves no half mod.
bool mods_add_zip(App &a, int g, const std::string &zipPath, std::string &err)
{
    if (a.run.proc) { err = "Close the game first."; return false; }   // the game reads the mods folder while it runs
    std::string top;
    {
        ZipReader zr(zipPath);
        if (!zr.ok) { err = "This file isn't a zip COGBox can read."; return false; }
        ZipPlan plan;
        if (!zip_plan(a.st[g].dataDir, zr, true, plan, err)) return false;
        top = plan.top;
    }
    std::string name = mod_folder_chars(top.empty() ? narrow(std::filesystem::path(widen(zipPath)).stem().wstring()) : top);   // the character rule, shared with the Fabricator's folder names
    while (!name.empty() && (name[0] == '.' || name[0] == ' ')) name.erase(0, 1);   // a name starting with '.' is hidden from the list
    if (name.size() >= 8 && lower(name.substr(name.size() - 8)) == ".partial") name += " mod";   // ... and so is "<name>.partial"
    if (name.empty()) name = "mod";
    std::string dest = join(mods_dir(g), name);
    // 0.6.1: 0.6.0 installed an EMPTY folder for a zip mod (review C1); RemoveDirectoryW only removes an empty one, so
    // that leftover no longer blocks adding the zip again and a real mod of the same name is still refused.
    if (dir_exists(dest) && !RemoveDirectoryW(widen(dest).c_str())) { err = "A mod named \"" + name + "\" is already there - remove or rename it first."; return false; }
    std::string part = dest + ".partial";
    mods_remove_tree(widen(part));   // what an earlier, failed try left
    bool ok = mods_unpack_zip(a, g, zipPath, part, true, err);
    if (ok && !MoveFileExW(widen(part).c_str(), widen(dest).c_str(), 0)) { ok = false; err = "The unpacked mod couldn't be moved into place."; }
    if (!ok) { mods_remove_tree(widen(part)); return false; }
    mods_turn_on_last(a, g, name);   // new mods start enabled, last in the order
    return true;
}
