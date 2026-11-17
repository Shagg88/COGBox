// COGBox - the Fabricator model (see fabricator_model.h).
#include "fabricator_model.h"
#include "util.h"
#include "../third_party/miniz/miniz.h"
#include "../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using json = nlohmann::json;

static std::string lower_s(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string trim_s(const std::string &s)
{
    size_t x = s.find_first_not_of(" \t\r"), y = s.find_last_not_of(" \t\r");
    return x == std::string::npos ? std::string() : s.substr(x, y - x + 1);
}

// ---- JSON ----
static std::string js(const json &o, const char *k)
{
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}
static int64_t ji(const json &o, const char *k)
{
    auto it = o.find(k);
    if (it == o.end() || !it->is_number()) return 0;
    if (it->is_number_float()) { double d = it->get<double>(); return d > 9e18 || d < -9e18 ? 0 : (int64_t)d; }
    if (it->is_number_unsigned()) { uint64_t u = it->get<uint64_t>(); return u > (uint64_t)INT64_MAX ? 0 : (int64_t)u; }
    return it->get<int64_t>();
}
static double jd(const json &o, const char *k)
{
    auto it = o.find(k);
    return it != o.end() && it->is_number() ? it->get<double>() : 0.0;
}
static int ji32(const json &o, const char *k)
{
    int64_t v = ji(o, k);
    return v > 2147483647 ? 2147483647 : v < -2147483647 ? -2147483647 : (int)v;
}
static std::vector<std::string> jstrs(const json &o, const char *k)
{
    std::vector<std::string> v;
    auto it = o.find(k);
    if (it != o.end() && it->is_array()) for (const json &e : *it) if (e.is_string()) v.push_back(e.get<std::string>());
    return v;
}
static std::optional<json> parse_json(const std::string &body)
{
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}
static void fill_card(FabCard &c, const json &o)
{
    c.id = ji(o, "id"); c.name = js(o, "name"); c.author = js(o, "author"); c.game = js(o, "game"); c.type = js(o, "type");
    c.short_desc = js(o, "short_desc"); c.screen = js(o, "screen"); c.version = js(o, "version");
    c.rating = jd(o, "rating"); c.ratings = ji32(o, "ratings"); c.downloads = ji32(o, "downloads");
    c.updated_at = ji(o, "updated_at"); c.size = ji(o, "size");
}
static std::vector<FabCard> cards_of(const json &o, const char *k)
{
    std::vector<FabCard> v;
    auto it = o.find(k);
    if (it != o.end() && it->is_array())
        for (const json &e : *it) if (e.is_object()) { FabCard c; fill_card(c, e); v.push_back(std::move(c)); }
    return v;
}

std::optional<std::vector<FabCard>> fab_parse_list(const std::string &body, int *total)
{
    auto j = parse_json(body);
    if (!j || !j->is_object()) return std::nullopt;
    std::vector<FabCard> v = cards_of(*j, "mods");
    if (total) *total = j->contains("total") && (*j)["total"].is_number() ? ji32(*j, "total") : (int)v.size();
    return v;
}

std::optional<FabPage> fab_parse_page(const std::string &body)
{
    auto j = parse_json(body);
    if (!j || !j->is_object()) return std::nullopt;
    FabPage p;
    fill_card(p, *j);
    p.long_desc = js(*j, "long_desc"); p.side = js(*j, "side"); p.state = js(*j, "state");
    p.tags = jstrs(*j, "tags"); p.screens = jstrs(*j, "screens");
    auto rq = j->find("requires");
    if (rq != j->end() && rq->is_array())
        for (const json &e : *rq) if (e.is_object()) p.requires.emplace_back(ji(e, "id"), js(e, "name"));
    auto vs = j->find("versions");
    if (vs != j->end() && vs->is_array())
        for (const json &e : *vs) if (e.is_object()) {
            FabVersion v;
            v.id = ji(e, "id"); v.version = js(e, "version"); v.notes = js(e, "notes"); v.sha256 = js(e, "sha256");
            v.size = ji(e, "size"); v.created_at = ji(e, "created_at");
            p.versions.push_back(std::move(v));
        }
    p.others = cards_of(*j, "others");
    return p;
}

std::optional<std::vector<FabLibraryItem>> fab_parse_library(const std::string &body)
{
    auto j = parse_json(body);
    if (!j) return std::nullopt;
    const json *arr = nullptr;
    if (j->is_array()) arr = &*j;
    else if (j->is_object() && j->contains("mods") && (*j)["mods"].is_array()) arr = &(*j)["mods"];
    if (!arr) return std::nullopt;
    std::vector<FabLibraryItem> v;
    for (const json &e : *arr) {
        if (!e.is_object()) continue;
        FabLibraryItem it;
        it.id = ji(e, "id"); it.version_id = ji(e, "version_id"); it.size = ji(e, "size");
        it.name = js(e, "name"); it.game = js(e, "game"); it.type = js(e, "type"); it.state = js(e, "state");
        it.version = js(e, "version"); it.sha256 = js(e, "sha256"); it.screen = js(e, "screen");
        v.push_back(std::move(it));
    }
    return v;
}

// ---- folder names ----
std::string mod_folder_chars(const std::string &base)
{
    std::string name;
    for (char c : base) if (isalnum((unsigned char)c) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')') name += c;
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    return name;
}

static bool reserved_name(const std::string &n)
{
    std::string stem = lower_s(n.substr(0, n.find('.')));   // Windows: "CON.txt" is the device too
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
    return stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && stem[3] >= '1' && stem[3] <= '9';
}

std::string fab_folder_name(const std::string &store_name, const std::vector<std::string> &existing, int64_t mod_id,
                            const std::function<std::optional<FabInstallRecord>(const std::string &)> &record_of)
{
    std::string base = mod_folder_chars(store_name);
    if (base.empty()) base = "mod";
    if (reserved_name(base)) {   // " mod" goes after the stem: "CON.txt" is still the device to Windows, "CON mod.txt" is not
        size_t dot = base.find('.');
        std::string stem = base.substr(0, dot), rest = dot == std::string::npos ? "" : base.substr(dot);
        while (!stem.empty() && stem.back() == ' ') stem.pop_back();
        base = stem + " mod" + rest;
    }
    for (int i = 0;; i++) {
        std::string cand = i == 0 ? base : i == 1 ? base + " (Fabricator)" : base + " (Fabricator " + std::to_string(i) + ")";
        const std::string *hit = nullptr;
        for (const std::string &e : existing) if (lower_s(e) == lower_s(cand)) { hit = &e; break; }
        if (!hit) return cand;
        if (record_of) {
            auto r = record_of(*hit);
            if (r && r->mod == mod_id) return *hit;
        }
    }
}

// ---- install record ----
std::optional<FabInstallRecord> fab_read_record(const std::string &folder)
{
    std::string text;
    if (!read_file(join(folder, "fabricator.json"), text, 1u << 20)) return std::nullopt;
    auto j = parse_json(text);
    if (!j || !j->is_object()) return std::nullopt;
    FabInstallRecord r;
    r.mod = ji(*j, "mod"); r.version = ji(*j, "version"); r.version_text = js(*j, "version_text"); r.sha256 = js(*j, "sha256");
    if (r.mod <= 0) return std::nullopt;
    return r;
}

bool fab_write_record(const std::string &folder, const FabInstallRecord &r)
{
    json j = {{"mod", r.mod}, {"version", r.version}, {"version_text", r.version_text}, {"sha256", r.sha256}};
    return write_file(join(folder, "fabricator.json"), j.dump(2, ' ', false, json::error_handler_t::replace) + "\n");   // write_file: temporary file, then replaced
}

// ---- zip check (mirrors tools/master-server/src/fabricator/checks.ts) ----
static const uint64_t kMaxPackedBytes = 4ull << 30, kMaxUnpackedBytes = 8ull << 30, kMaxFiles = 50000;
static const long kModApiLevel = 1;
static const size_t kMaxIniBytes = 256 * 1024;

// no absolute, drive-letter / stream (':') path, no ".." component - also as Windows reads it (trailing dots and spaces dropped)
static bool bad_path(const std::string &raw)
{
    if (raw.find('\0') != std::string::npos || raw.find(':') != std::string::npos) return true;
    std::string p = raw;
    for (char &c : p) if (c == '\\') c = '/';
    if (!p.empty() && p[0] == '/') return true;
    for (size_t at = 0; at <= p.size();) {
        size_t end = p.find('/', at);
        if (end == std::string::npos) end = p.size();
        std::string seg = p.substr(at, end - at), t = seg;
        while (!t.empty() && (t.back() == '.' || t.back() == ' ')) t.pop_back();
        if (!seg.empty() && seg != "." && t.empty()) return true;
        at = end + 1;
    }
    return false;
}

// The path's segments split on '/' or '\'
static std::vector<std::string> segments(const std::string &name)
{
    std::vector<std::string> v;
    std::string cur;
    for (char c : name) {
        if (c == '/' || c == '\\') { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    v.push_back(cur);
    return v;
}
static std::string trim_dots_spaces(std::string s)
{
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s;
}
// checks.ts deviceName: the first segment whose stem (trailing dots and spaces off, the part before the first dot,
// trailing spaces off) is CON, PRN, AUX, NUL, COM1-9 / LPT1-9 or COM / LPT with a superscript 1, 2 or 3 ("" none).
static std::string device_segment(const std::string &name)
{
    for (const std::string &seg : segments(name)) {
        std::string stem = trim_dots_spaces(seg);
        stem = stem.substr(0, stem.find('.'));
        while (!stem.empty() && stem.back() == ' ') stem.pop_back();
        std::string low = stem;
        for (char &c : low) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (low == "con" || low == "prn" || low == "aux" || low == "nul") return seg;
        if (low.size() >= 4 && (low.compare(0, 3, "com") == 0 || low.compare(0, 3, "lpt") == 0)) {
            const std::string d = low.substr(3);
            if ((d.size() == 1 && d[0] >= '1' && d[0] <= '9') || d == "\xC2\xB9" || d == "\xC2\xB2" || d == "\xC2\xB3") return seg;
        }
    }
    return "";
}
// checks.ts windowsKey: one separator, no empty or "." segments, trailing dots and spaces off, case folded (Unicode
// lower case, as JavaScript's toLowerCase for the scripts mod names use)
static std::string windows_key(const std::string &name)
{
    std::string key;
    for (const std::string &seg : segments(name)) {
        if (seg.empty() || seg == ".") continue;
        key += (key.empty() ? "" : "/") + trim_dots_spaces(seg);
    }
    std::wstring w = widen(key), low(w.size(), L'\0');
    if (!w.empty() && LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.c_str(), (int)w.size(), &low[0], (int)low.size(), nullptr,
                                    nullptr, 0) == (int)w.size())
        return narrow(low);
    for (char &c : key) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return key;
}

// Counts the bytes really inflated (not the declared size) and stops the entry once the total is over the limit.
struct CountSink {
    uint64_t *total;
    uint64_t limit;
    bool over = false;
    std::string *keep = nullptr;
    size_t keepMax = 0;
};
static size_t count_cb(void *ud, mz_uint64, const void *buf, size_t len)
{
    CountSink *s = (CountSink *)ud;
    *s->total += len;
    if (s->keep && s->keep->size() < s->keepMax) s->keep->append((const char *)buf, std::min(len, s->keepMax - s->keep->size()));
    if (*s->total > s->limit) { s->over = true; return 0; }
    return len;
}

std::vector<std::string> fab_check_zip(const std::string &zip_path, const std::string &game)
{
    std::vector<std::string> problems;
    auto note = [&](const std::string &p) { if (std::find(problems.begin(), problems.end(), p) == problems.end()) problems.push_back(p); };
    FILE *f = _wfopen(widen(zip_path).c_str(), L"rb");   // a wide path: folder and file names can be non-ASCII
    if (!f) { note("the zip file cannot be opened"); return problems; }
    struct Closer { FILE *f; ~Closer() { fclose(f); } } closer{f};
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_cfile(&z, f, 0, 0)) { note("not a valid zip file"); return problems; }
    struct Ender { mz_zip_archive *z; ~Ender() { mz_zip_reader_end(z); } } ender{&z};
    if (file_size(zip_path) > kMaxPackedBytes) note("the zip is over the packed size limit of " + std::to_string(kMaxPackedBytes) + " bytes");
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    if (n > kMaxFiles) { note("too many files: more than " + std::to_string(kMaxFiles) + " files in the zip"); return problems; }

    static const std::set<std::string> refused = {".exe", ".dll", ".xex", ".xexp", ".bat", ".cmd", ".ps1", ".vbs", ".js", ".msi", ".scr", ".com", ".lnk"};
    int refusedShown = 0, iniIdx = -1, iniDepth = 99, devicesShown = 0;
    uint64_t declared = 0;
    std::vector<mz_uint> toInflate;
    std::set<std::string> fileKeys, dirKeys;
    bool sameName = false;
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) { note("the zip is damaged: not a valid zip file"); return problems; }
        const std::string name = st.m_filename;
        if (mz_zip_reader_get_filename(&z, i, nullptr, 0) != name.size() + 1) note("unsafe path in the zip (outside the mod folder)");   // an embedded NUL
        if (st.m_is_encrypted) note("encrypted entry: " + name);
        if (bad_path(name)) note("unsafe path in the zip (outside the mod folder)");
        if ((st.m_version_made_by >> 8) == 3 && ((st.m_external_attr >> 16) & 0xf000) == 0xa000) note("the zip contains a link (symlink), which is not allowed");
        const bool isDir = !name.empty() && name.back() == '/';
        const std::string dev = device_segment(name);
        if (!dev.empty() && devicesShown++ < 5)
            note("Windows device name not allowed as a file or folder name (CON, PRN, AUX, NUL, COM1-9, LPT1-9): " + dev);
        const std::string key = windows_key(name);
        if (!key.empty()) {
            for (size_t at = key.find('/'); at != std::string::npos; at = key.find('/', at + 1)) dirKeys.insert(key.substr(0, at));
            if (isDir) dirKeys.insert(key);
            else if (!fileKeys.insert(key).second) sameName = true;
        }
        if (isDir) continue;   // a directory, as the server decides it
        std::string norm = name;
        for (char &c : norm) if (c == '\\') c = '/';
        std::string base = norm.substr(norm.find_last_of('/') == std::string::npos ? 0 : norm.find_last_of('/') + 1);
        while (!base.empty() && (base.back() == '.' || base.back() == ' ')) base.pop_back();
        size_t dot = base.find_last_of('.');
        std::string ext = dot == std::string::npos ? "" : lower_s(base.substr(dot));
        if (refused.count(ext) && refusedShown++ < 5) note("program file not allowed (" + ext + "): " + base);
        std::string low = lower_s(norm);
        int depth = low == "mod.ini" ? 0 : (low.size() > 8 && low.compare(low.size() - 8, 8, "/mod.ini") == 0 && low.find('/') == low.size() - 8) ? 1 : 99;
        if (depth < iniDepth) { iniDepth = depth; iniIdx = (int)i; }
        declared += st.m_uncomp_size;
        if (!st.m_is_encrypted) toInflate.push_back(i);
        if (declared > kMaxUnpackedBytes) { note("unpacked size is over the limit of " + std::to_string(kMaxUnpackedBytes) + " bytes"); return problems; }
    }
    for (const std::string &k : fileKeys) if (dirKeys.count(k)) sameName = true;   // a file and a folder of one name
    if (sameName) note("two entries in the zip have the same path or paths that differ only in case: Windows cannot keep both");
    if (!problems.empty()) return problems;

    // really inflate every entry and count the true bytes (the declared sizes can lie)
    uint64_t real = 0;
    std::string ini;
    for (mz_uint i : toInflate) {
        CountSink sink;
        sink.total = &real; sink.limit = kMaxUnpackedBytes;
        if ((int)i == iniIdx) { sink.keep = &ini; sink.keepMax = kMaxIniBytes + 1; }
        if (!mz_zip_reader_extract_to_callback(&z, i, count_cb, &sink, 0)) {
            if (sink.over) note("unpacked size is over the limit of " + std::to_string(kMaxUnpackedBytes) + " bytes");
            else note("an entry unpacks to a different size than declared (size mismatch) or is damaged");
            return problems;
        }
    }
    if (iniIdx >= 0) {
        if (ini.size() > kMaxIniBytes) { note("mod.ini is too large"); return problems; }
        // modcore's rules: keys case-insensitive, ; / # comment lines, [Mod] or no section, no Game key = gears3
        std::string iniGame = "gears3", section;
        long iniApi = -1;
        if (ini.compare(0, 3, "\xEF\xBB\xBF") == 0) ini.erase(0, 3);
        for (size_t at = 0; at <= ini.size();) {
            size_t e = ini.find('\n', at);
            if (e == std::string::npos) e = ini.size();
            std::string line = trim_s(ini.substr(at, e - at));
            at = e + 1;
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '[' && line.back() == ']') { section = lower_s(trim_s(line.substr(1, line.size() - 2))); continue; }
            if (!section.empty() && section != "mod") continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = lower_s(trim_s(line.substr(0, eq))), value = trim_s(line.substr(eq + 1));
            if (key == "game") { iniGame = lower_s(value); if (iniGame.empty()) iniGame = "gears3"; }
            else if (key == "api") { char *end = nullptr; long v = strtol(value.c_str(), &end, 10); if (end != value.c_str()) iniApi = v; }
        }
        if (iniGame != game) note("mod.ini is for game " + iniGame + ", but this is for " + game + ": wrong game");
        if (iniApi > kModApiLevel) note("mod.ini needs Api " + std::to_string(iniApi) + ", which is newer than this store supports (" + std::to_string(kModApiLevel) + ")");
    }
    return problems;
}
