// COGBox 0.6.1: "Import saves" (see save_import.h). Our code (MIT).
//
// Rules (Shagg: never delete the player's files): nothing outside our own work folder (<user>\_import-<stamp>) is ever
// removed; whatever an import replaces is moved to <user>\_saves-before-import-<stamp>[-N] first; if putting the new
// files in place fails, what was moved away is moved back. The source is only read.
#include "save_import.h"

#include "x360fs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace save_import {
namespace fs = std::filesystem;

namespace {

// The SDK's one profile: every save lives under this XUID (rex user_profile.cpp; patch 79's online XUID is off by default).
constexpr const char *kProfileXuid = "B13EBABEBABEBABE";
constexpr uint64_t kMaxEntries = 200000;  // directory entries looked at, at most (a whole drive is not searched)

std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string upper(std::string s) {
    for (char &c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string name_of(const fs::path &p) {
    const auto u = p.filename().u8string();
    return std::string(reinterpret_cast<const char *>(u.c_str()), u.size());
}
bool is_hex(const std::string &s, size_t n) {
    if (s.size() != n) return false;
    for (char c : s) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
bool is_title_setting(uint32_t id) { return id == 0x63E83FFFu || id == 0x63E83FFEu || id == 0x63E83FFDu; }
// The size the game reserves for a setting (XUSER profile setting IDs: bits 16-27), so a damaged file can't overflow it.
size_t setting_max(uint32_t id) { return size_t((id >> 16) & 0xFFFu); }

// Directory listing without exceptions (a pulled USB stick or a locked folder ends the listing, not the launcher).
template <class F> void each_entry(const fs::path &dir, F &&f) {
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) f(*it);
}
bool is_folder(const fs::path &p) { std::error_code ec; return fs::is_directory(p, ec); }
bool is_plain_file(const fs::path &p) { std::error_code ec; return fs::is_regular_file(p, ec); }
bool there(const fs::path &p) { std::error_code ec; return fs::exists(p, ec); }

bool read_file(const fs::path &p, std::string &out, uint64_t max) {
    std::error_code ec;
    const uint64_t size = fs::file_size(p, ec);
    if (ec || size > max) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.resize(size_t(size));
    return size == 0 || bool(f.read(out.data(), std::streamsize(size)));
}
bool write_file(const fs::path &p, const std::string &data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    return f && (data.empty() || f.write(data.data(), std::streamsize(data.size()))) && bool(f.flush());
}
bool package_magic(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    char m[4] = {};
    return f.read(m, 4) && (!std::memcmp(m, "CON ", 4) || !std::memcmp(m, "LIVE", 4) || !std::memcmp(m, "PIRS", 4));
}
bool copy_tree(const fs::path &from, const fs::path &to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) return false;
    fs::copy(from, to, fs::copy_options::recursive, ec);
    return !ec;
}
bool has_files(const fs::path &dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) return true;
    return false;
}
fs::path canon(const fs::path &p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p : c;
}
std::string path_str(const fs::path &p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char *>(u.c_str()), u.size());
}
// a is b or inside b (case-insensitive, as Windows paths are)
bool within(const fs::path &a, const fs::path &b) {
    const std::string sa = lower(path_str(canon(a)));
    std::string sb = lower(path_str(canon(b)));
    while (!sb.empty() && (sb.back() == '\\' || sb.back() == '/')) sb.pop_back();
    if (sb.empty()) return false;
    return sa == sb || (sa.size() > sb.size() && sa.compare(0, sb.size(), sb) == 0 && (sa[sb.size()] == '\\' || sa[sb.size()] == '/'));
}

struct Work {
    uint32_t title = 0;
    std::string title_hex;          // lower case
    std::vector<uint32_t> from_titles;      // title IDs whose saves are taken: the game's own + its regional copies
    std::vector<std::string> from_hex;      // the same, lower-case hex
    fs::path stage;                 // <user dir>\_import-<stamp>: saves\<name>\..., settings\<ID>, tmp\...
    std::vector<fs::path> skip;     // COGBox's own user folders (never a source)
    Result *r = nullptr;
    int tmp_n = 0;
    uint64_t seen = 0;
    bool capped = false;
    std::vector<std::string> taken_settings;
};

// <TITLE>.gpd of the game or of one of its regional copies
bool is_from_gpd(const Work &w, const std::string &file_name) {
    const std::string n = lower(file_name);
    for (const std::string &h : w.from_hex) if (n == h + ".gpd") return true;
    return false;
}
void note_once(Work &w, const std::string &n) {
    if (std::find(w.r->notes.begin(), w.r->notes.end(), n) == w.r->notes.end()) w.r->notes.push_back(n);
}
// A save of this name is staged already (two profiles, or two layouts in one folder): the first one found is kept.
bool save_taken(Work &w, const std::string &name) {
    if (!there(w.stage / "saves" / name)) return false;
    note_once(w, "A second save named " + name + " was found and left out (only the first one is imported).");
    return true;
}
void put_setting(Work &w, uint32_t id, const std::string &data, const std::string &from) {
    char nm[16];
    std::snprintf(nm, sizeof(nm), "%08X", id);
    if (data.size() > setting_max(id)) { note_once(w, std::string("Game progress ") + nm + " in " + from + " is too large - left out."); return; }
    if (std::find(w.taken_settings.begin(), w.taken_settings.end(), nm) != w.taken_settings.end()) {
        note_once(w, "Game progress was found in more than one profile; the first one found is imported.");
        return;
    }
    if (write_file(w.stage / "settings" / nm, data)) w.taken_settings.push_back(nm);
}
void take_gpd(Work &w, const fs::path &gpd) {
    std::string b;
    std::vector<std::pair<uint32_t, std::string>> set;
    if (!read_file(gpd, b, 64ull << 20) || !gpd_title_settings(reinterpret_cast<const uint8_t *>(b.data()), b.size(), set)) {
        note_once(w, "The profile file " + name_of(gpd) + " could not be read.");
        return;
    }
    for (const auto &[id, data] : set) put_setting(w, id, data, name_of(gpd));
}
void take_package(Work &w, const fs::path &file) {
    x360fs::PackageInfo info;
    std::string err;
    if (!x360fs::read_package_info(file, info, err) || info.is_svod()) return;
    const bool save = info.content_type == x360fs::kContentSavedGame &&
        std::find(w.from_titles.begin(), w.from_titles.end(), info.title_id) != w.from_titles.end();
    const bool profile = info.content_type == 0x00010000u;  // a profile: its <TITLE>.gpd is what we want
    if (!save && !profile) return;
    if (save) {
        if (save_taken(w, name_of(file))) return;
        const fs::path out = w.stage / "saves" / file.filename();
        if (x360fs::extract_stfs(file, out, nullptr, nullptr, err, true) && has_files(out)) {
            ++w.r->packages;
        } else {
            std::error_code ec;
            fs::remove_all(out, ec);  // (our own work folder)
            note_once(w, "The save package " + name_of(file) + " could not be unpacked: " + err);
        }
        return;
    }
    const fs::path out = w.stage / "tmp" / std::to_string(w.tmp_n++);
    if (!x360fs::extract_stfs(file, out, nullptr, nullptr, err, true)) {
        note_once(w, "The profile package " + name_of(file) + " could not be unpacked: " + err);
        return;
    }
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(out, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && is_from_gpd(w, name_of(it->path()))) {
            take_gpd(w, it->path());
            ++w.r->packages;
        }
}
// <TITLE>: its 00000001 folder's save folders (Xenia, COGBox) and its profile\<user>\<setting id> files.
void take_title_dir(Work &w, const fs::path &dir) {
    each_entry(dir / "00000001", [&](const fs::directory_entry &e) {
        if (!is_folder(e.path()) || !has_files(e.path())) return;
        const std::string n = name_of(e.path());
        if (save_taken(w, n)) return;
        const fs::path to = w.stage / "saves" / n;
        if (!copy_tree(e.path(), to)) {
            std::error_code ec;
            fs::remove_all(to, ec);  // (our own work folder)
            note_once(w, "The save " + n + " could not be read completely - left out.");
        }
    });
    each_entry(dir / "profile", [&](const fs::directory_entry &u) {
        if (!is_folder(u.path())) return;
        each_entry(u.path(), [&](const fs::directory_entry &f) {
            const std::string n = name_of(f.path());
            if (!is_plain_file(f.path()) || !is_hex(n, 8)) return;
            const uint32_t id = uint32_t(std::stoul(n, nullptr, 16));
            std::string b;
            if (is_title_setting(id) && read_file(f.path(), b, 1 << 20)) put_setting(w, id, b, n);
        });
    });
}
void scan(Work &w, const fs::path &p, int depth, bool chosen) {
    if (++w.seen > kMaxEntries) { w.capped = true; return; }
    for (const fs::path &s : w.skip) if (within(p, s)) return;  // COGBox's own saves are never a source
    if (is_plain_file(p)) {
        // Packages are only looked into where the console keeps them (00000001 saves, 00010000 profiles) or when the
        // player chose the file itself - an installed game or DLC folder is not opened file by file.
        const std::string parent = upper(name_of(p.parent_path()));
        if (is_from_gpd(w, name_of(p))) take_gpd(w, p);
        else if ((chosen || parent == "00000001" || parent == "00010000") && package_magic(p)) take_package(w, p);
        return;
    }
    if (!is_folder(p)) return;
    if (std::find(w.from_hex.begin(), w.from_hex.end(), lower(name_of(p))) != w.from_hex.end()) take_title_dir(w, p);
    if (depth >= 8) return;
    each_entry(p, [&](const fs::directory_entry &e) {
        const std::string n = name_of(e.path());
        if (n.rfind("_import-", 0) == 0 || n.rfind("_saves-before-import-", 0) == 0) return;
        scan(w, e.path(), depth + 1, false);
    });
}

struct Moved { fs::path from, to; };  // done moves, to undo
bool move(const fs::path &from, const fs::path &to, std::vector<Moved> &done) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec) return false;
    done.push_back({from, to});
    return true;
}
void undo(std::vector<Moved> &done) {
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
        std::error_code ec;
        fs::rename(it->to, it->from, ec);
    }
    done.clear();
}

}  // namespace

bool gpd_title_settings(const uint8_t *d, size_t n, std::vector<std::pair<uint32_t, std::string>> &out) {
    // XDBF: "XDBF", version, entry table length, entry count, free table length, free count (all big-endian u32);
    // entries of 18 bytes (namespace u16, id u64, offset u32, length u32); the data follows the two tables.
    if (n < 24 || std::memcmp(d, "XDBF", 4) != 0) return false;
    const uint64_t table_len = be32(d + 8), count = be32(d + 12), free_len = be32(d + 16);
    const uint64_t base = 24 + table_len * 18 + free_len * 8;
    if (count > table_len || base > n) return false;
    for (uint64_t i = 0; i < count; ++i) {
        const uint8_t *e = d + 24 + i * 18;
        const uint32_t ns = uint32_t(e[0]) << 8 | e[1];
        const uint64_t id = be64(e + 2), off = be32(e + 10), len = be32(e + 14);
        if (ns != 3 || (id >> 32) || !is_title_setting(uint32_t(id)) || base + off + len > n || len < 0x18) continue;
        // A setting: id u32, 4 bytes, type u8 (6 = binary), 7 bytes, then for binary: size u32, 4 bytes, the data.
        const uint8_t *s = d + base + off;
        if (be32(s) != uint32_t(id) || s[8] != 6) continue;
        const uint64_t size = be32(s + 0x10);
        if (0x18 + size > len) continue;
        out.emplace_back(uint32_t(id), std::string(reinterpret_cast<const char *>(s + 0x18), size_t(size)));
    }
    return true;
}

bool import(const fs::path &source, uint32_t title_id, const fs::path &user_dir, const std::string &stamp, Result &r,
            const std::vector<fs::path> &own_dirs, const std::vector<uint32_t> &region_titles) {
    std::error_code ec;
    r = Result{};
    if (!there(source)) { r.error = "That file or folder does not exist."; return false; }
    Work w;
    w.title = title_id;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08x", title_id);
    w.title_hex = hex;
    // 0.6.2: saves of the game's regional copies (the Japanese Gears 2 disc, 4D5308C8) go in under the game's own ID,
    // as the same port plays both discs.
    w.from_titles.push_back(title_id);
    for (uint32_t t : region_titles) if (t != title_id) w.from_titles.push_back(t);
    for (uint32_t t : w.from_titles) { std::snprintf(hex, sizeof(hex), "%08x", t); w.from_hex.push_back(hex); }
    w.r = &r;
    w.skip = own_dirs;
    w.skip.push_back(user_dir);
    for (const fs::path &s : w.skip) {
        if (within(source, s)) {
            r.error = "That is inside COGBox's own saves folder - choose the Xenia or Xbox 360 folder to import from.";
            return false;
        }
    }
    w.stage = user_dir / ("_import-" + stamp);
    fs::create_directories(w.stage, ec);
    if (ec) { r.error = "The game's saves folder could not be written to."; return false; }
    scan(w, source, 0, true);
    if (w.capped) note_once(w, "The search stopped after " + std::to_string(kMaxEntries) + " files and folders - choose the content folder itself, not a whole drive.");
    std::vector<fs::path> saves, settings;
    each_entry(w.stage / "saves", [&](const fs::directory_entry &e) { if (is_folder(e.path()) && has_files(e.path())) saves.push_back(e.path()); });
    each_entry(w.stage / "settings", [&](const fs::directory_entry &e) { if (is_plain_file(e.path())) settings.push_back(e.path()); });
    if (saves.empty() && settings.empty()) {
        fs::remove_all(w.stage, ec);  // (our own work folder)
        r.error = r.notes.empty() ? "No saves of this game were found there."
                                  : "No saves of this game could be read there.";
        return false;
    }
    const std::string up = upper(w.title_hex);
    fs::path backup = user_dir / ("_saves-before-import-" + stamp);
    for (int n = 2; there(backup); ++n) backup = user_dir / ("_saves-before-import-" + stamp + "-" + std::to_string(n));
    const fs::path title_root = user_dir / kProfileXuid / up;
    const fs::path save_root = title_root / "00000001";
    const fs::path header_root = title_root / "Headers" / "00000001";
    const fs::path set_root = user_dir / up / "profile" / "User";
    std::vector<Moved> done;
    bool ok = true;
    for (const fs::path &s : saves) {
        const std::string n = name_of(s);
        const fs::path dest = save_root / n, header = header_root / (n + ".header");
        // the old save and its header go to the backup (the header describes the old one), then the new save goes in
        if (there(dest) && !move(dest, backup / "00000001" / n, done)) { ok = false; break; }
        if (there(header) && !move(header, backup / "Headers" / (n + ".header"), done)) { ok = false; break; }
        if (!move(s, dest, done)) { ok = false; break; }
        r.names.push_back(n);
    }
    for (size_t i = 0; ok && i < settings.size(); ++i) {
        const fs::path &s = settings[i];
        const fs::path dest = set_root / s.filename();
        if (there(dest) && !move(dest, backup / "profile" / s.filename(), done)) { ok = false; break; }
        if (!move(s, dest, done)) { ok = false; break; }
        ++r.settings;
    }
    if (!ok) {
        undo(done);  // everything back where it was
        fs::remove_all(w.stage, ec);
        r.names.clear();
        r.settings = 0;
        r.error = "The saves could not be put in place (is the game or another program using them?). Nothing was changed.";
        return false;
    }
    r.saves = int(r.names.size());
    if (there(backup)) r.backup_dir = backup;
    fs::remove_all(w.stage, ec);  // (our own work folder: what is left are temporary copies)
    return true;
}

}  // namespace save_import
