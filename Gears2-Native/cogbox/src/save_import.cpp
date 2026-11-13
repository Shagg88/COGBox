// COGBox 0.6.2: "Import saves" (see save_import.h). Our code (MIT).
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

std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string name_of(const fs::path &p) { return p.filename().u8string().empty() ? std::string() : std::string(reinterpret_cast<const char *>(p.filename().u8string().c_str())); }
bool is_hex(const std::string &s, size_t n) {
    if (s.size() != n) return false;
    for (char c : s) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
bool is_title_setting(uint32_t id) { return id == 0x63E83FFFu || id == 0x63E83FFEu || id == 0x63E83FFDu; }

bool read_file(const fs::path &p, std::string &out, uint64_t max = 64ull << 20) {
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
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return !ec;
}
bool has_files(const fs::path &dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) return true;
    return false;
}

struct Work {
    uint32_t title = 0;
    std::string title_hex;       // lower case
    fs::path stage;              // <user dir>\_import-<stamp>: saves\<name>\..., settings\<ID>, tmp\...
    fs::path user_dir;
    Result *r = nullptr;
    int tmp_n = 0;
    uint64_t seen = 0;           // directory entries looked at (a cap against a whole-disk search)
};

void take_gpd(Work &w, const fs::path &gpd) {
    std::string b;
    std::vector<std::pair<uint32_t, std::string>> set;
    if (!read_file(gpd, b) || !gpd_title_settings(reinterpret_cast<const uint8_t *>(b.data()), b.size(), set)) {
        w.r->notes.push_back("The profile file " + name_of(gpd) + " could not be read.");
        return;
    }
    for (const auto &[id, data] : set) {
        char nm[16];
        std::snprintf(nm, sizeof(nm), "%08X", id);
        write_file(w.stage / "settings" / nm, data);
    }
}
void scan(Work &w, const fs::path &p, int depth);

void take_package(Work &w, const fs::path &file) {
    x360fs::PackageInfo info;
    std::string err;
    if (!x360fs::read_package_info(file, info, err) || info.is_svod()) return;
    const bool save = info.content_type == x360fs::kContentSavedGame && info.title_id == w.title;
    const bool profile = info.content_type == 0x00010000u;  // a profile: its <TITLE>.gpd is what we want
    if (!save && !profile) return;
    if (save) {
        const fs::path out = w.stage / "saves" / file.filename();
        if (x360fs::extract_stfs(file, out, nullptr, nullptr, err, true) && has_files(out)) {
            ++w.r->packages;
        } else {
            std::error_code ec;
            fs::remove_all(out, ec);  // (our own work folder)
            w.r->notes.push_back("The save package " + name_of(file) + " could not be unpacked: " + err);
        }
        return;
    }
    const fs::path out = w.stage / "tmp" / std::to_string(w.tmp_n++);
    if (!x360fs::extract_stfs(file, out, nullptr, nullptr, err, true)) {
        w.r->notes.push_back("The profile package " + name_of(file) + " could not be unpacked: " + err);
        return;
    }
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(out, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && lower(name_of(it->path())) == w.title_hex + ".gpd") {
            take_gpd(w, it->path());
            ++w.r->packages;
        }
}
// <TITLE>: its 00000001 folder's save folders (Xenia, COGBox) and its profile\<user>\<setting id> files.
void take_title_dir(Work &w, const fs::path &dir) {
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir / "00000001", ec)) {
        if (!e.is_directory(ec) || !has_files(e.path())) continue;
        copy_tree(e.path(), w.stage / "saves" / e.path().filename());
    }
    for (const auto &u : fs::directory_iterator(dir / "profile", ec)) {
        if (!u.is_directory(ec)) continue;
        for (const auto &f : fs::directory_iterator(u.path(), ec)) {
            const std::string n = name_of(f.path());
            if (!f.is_regular_file(ec) || !is_hex(n, 8) || !is_title_setting(uint32_t(std::stoul(n, nullptr, 16)))) continue;
            std::string b;
            if (read_file(f.path(), b, 1 << 20)) {
                std::string up = n;
                for (char &c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
                write_file(w.stage / "settings" / up, b);
            }
        }
    }
}
void scan(Work &w, const fs::path &p, int depth) {
    std::error_code ec;
    if (++w.seen > 200000) return;
    if (fs::is_regular_file(p, ec)) {
        if (lower(name_of(p)) == w.title_hex + ".gpd") take_gpd(w, p);
        else if (package_magic(p)) take_package(w, p);
        return;
    }
    if (!fs::is_directory(p, ec)) return;
    if (lower(name_of(p)) == w.title_hex) take_title_dir(w, p);
    if (depth >= 8) return;
    for (const auto &e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
        const std::string n = name_of(e.path());
        if (!n.empty() && n[0] == '_' && (n.rfind("_import-", 0) == 0 || n.rfind("_saves-before-import-", 0) == 0)) continue;
        scan(w, e.path(), depth + 1);
    }
}
// The profile folder the game's saves go under: the one that already holds this game's, else any, else the default.
std::string xuid_dir(const fs::path &user_dir, const std::string &title_hex) {
    std::error_code ec;
    std::string any;
    for (const auto &e : fs::directory_iterator(user_dir, ec)) {
        const std::string n = name_of(e.path());
        if (!e.is_directory(ec) || !is_hex(n, 16) || n == "0000000000000000") continue;
        for (const auto &t : fs::directory_iterator(e.path(), ec))
            if (lower(name_of(t.path())) == title_hex) return n;
        if (any.empty()) any = n;
    }
    return any.empty() ? "B13EBABEBABEBABE" : any;  // the SDK's (and Xenia's) default profile
}
// Moves `from` to `to`; what is at `to` goes to the backup folder first (never deleted).
bool put(const fs::path &from, const fs::path &to, const fs::path &backup, bool &backed_up) {
    std::error_code ec;
    if (fs::exists(to, ec)) {
        fs::create_directories(backup.parent_path(), ec);
        fs::rename(to, backup, ec);
        if (ec) return false;
        backed_up = true;
    }
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec) {  // (another drive is not possible - the work folder is inside the user folder - but be sure)
        ec.clear();
        if (fs::is_directory(from, ec)) return copy_tree(from, to);
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        return !ec;
    }
    return true;
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
        if (ns != 3 || !is_title_setting(uint32_t(id)) || (id >> 32) || base + off + len > n || len < 0x18) continue;
        // A setting: id u32, 4 bytes, type u8 (6 = binary), 7 bytes, then for binary: size u32, 4 bytes, the data.
        const uint8_t *s = d + base + off;
        if (be32(s) != uint32_t(id) || s[8] != 6) continue;
        const uint64_t size = be32(s + 0x10);
        if (0x18 + size > len) continue;
        out.emplace_back(uint32_t(id), std::string(reinterpret_cast<const char *>(s + 0x18), size_t(size)));
    }
    return true;
}

bool import(const fs::path &source, uint32_t title_id, const fs::path &user_dir, const std::string &stamp, Result &r) {
    std::error_code ec;
    r = Result{};
    if (!fs::exists(source, ec)) { r.error = "That file or folder does not exist."; return false; }
    Work w;
    w.title = title_id;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08x", title_id);
    w.title_hex = hex;
    w.user_dir = user_dir;
    w.r = &r;
    w.stage = user_dir / ("_import-" + stamp);
    fs::create_directories(w.stage, ec);
    if (ec) { r.error = "The game's saves folder could not be written to."; return false; }
    const fs::path src_abs = fs::weakly_canonical(source, ec), user_abs = fs::weakly_canonical(user_dir, ec);
    if (src_abs == user_abs) {
        fs::remove_all(w.stage, ec);
        r.error = "That is this game's own COGBox saves folder - choose the Xenia or Xbox 360 folder to import from.";
        return false;
    }
    scan(w, source, 0);
    std::vector<fs::path> saves, settings;
    for (const auto &e : fs::directory_iterator(w.stage / "saves", ec)) if (e.is_directory(ec) && has_files(e.path())) saves.push_back(e.path());
    ec.clear();
    for (const auto &e : fs::directory_iterator(w.stage / "settings", ec)) if (e.is_regular_file(ec)) settings.push_back(e.path());
    if (saves.empty() && settings.empty()) {
        fs::remove_all(w.stage, ec);  // (our own work folder)
        r.error = r.notes.empty() ? "No saves of this game were found there." : "No saves of this game could be read there.";
        return false;
    }
    std::string up = w.title_hex;
    for (char &c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
    const fs::path backup = user_dir / ("_saves-before-import-" + stamp);
    const fs::path save_root = user_dir / xuid_dir(user_dir, w.title_hex) / up / "00000001";
    const fs::path set_root = user_dir / up / "profile" / "User";
    bool backed_up = false, ok = true;
    for (const fs::path &s : saves) {
        // The game's header file for a save of this name describes the old one: it goes to the backup with it.
        const fs::path header = save_root.parent_path() / "Headers" / "00000001" / (name_of(s) + ".header");
        if (fs::exists(header, ec)) {
            fs::create_directories(backup / "Headers", ec);
            fs::rename(header, backup / "Headers" / header.filename(), ec);
            backed_up = true;
        }
        if (put(s, save_root / s.filename(), backup / "00000001" / s.filename(), backed_up)) {
            ++r.saves;
            r.names.push_back(name_of(s));
        } else {
            ok = false;
        }
    }
    for (const fs::path &s : settings) {
        if (put(s, set_root / s.filename(), backup / "profile" / s.filename(), backed_up)) ++r.settings;
        else ok = false;
    }
    if (backed_up) r.backup_dir = backup;
    fs::remove_all(w.stage, ec);  // (our own work folder; everything in it was moved out or is a temporary copy)
    if (!ok) { r.error = "Some files could not be put in place - is the game running?"; return false; }
    return true;
}

}  // namespace save_import
