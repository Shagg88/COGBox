// COGBox - the Fabricator model (see fabricator_model.h).
#include "fabricator_model.h"
#include "mod_file_rules.h"
#include "util.h"
#include "modcore/campaigns.h"   // modcore::kModApiLevel: the mod API the game implements
#include "../third_party/miniz/miniz.h"
#include "../third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
static std::optional<json> parse_json(const std::string &body)
{
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}
// Unicode lower case (as Windows compares file names, for the scripts mod names use); ASCII if the call fails
static std::string fold_case(const std::string &s)
{
    std::wstring w = widen(s), low(w.size(), L'\0');
    if (!w.empty() && LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.c_str(), (int)w.size(), &low[0], (int)low.size(), nullptr,
                                    nullptr, 0) == (int)w.size())
        return narrow(low);
    std::string k = s;
    for (char &c : k) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return k;
}
// ---- folder names ----
// An accented Latin letter as plain ASCII (U+00C0-U+017F: Latin-1 and Latin Extended-A), "" for anything else
// (x and / signs, letters of other scripts).
static const char *latin_ascii(unsigned cp)
{
    static const char *const k00C0[64] = {
        "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
        "D", "N", "O", "O", "O", "O", "O", "", "O", "U", "U", "U", "U", "Y", "Th", "ss",
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y" };
    static const char *const k0100[128] = {
        "A", "a", "A", "a", "A", "a", "C", "c", "C", "c", "C", "c", "C", "c", "D", "d",
        "D", "d", "E", "e", "E", "e", "E", "e", "E", "e", "E", "e", "G", "g", "G", "g",
        "G", "g", "G", "g", "H", "h", "H", "h", "I", "i", "I", "i", "I", "i", "I", "i",
        "I", "i", "IJ", "ij", "J", "j", "K", "k", "k", "L", "l", "L", "l", "L", "l", "L",
        "l", "L", "l", "N", "n", "N", "n", "N", "n", "n", "N", "n", "O", "o", "O", "o",
        "O", "o", "OE", "oe", "R", "r", "R", "r", "R", "r", "S", "s", "S", "s", "S", "s",
        "S", "s", "T", "t", "T", "t", "T", "t", "U", "u", "U", "u", "U", "u", "U", "u",
        "U", "u", "U", "u", "W", "w", "Y", "y", "Y", "Z", "z", "Z", "z", "Z", "z", "s" };
    if (cp >= 0xC0 && cp < 0x100) return k00C0[cp - 0xC0];
    if (cp >= 0x100 && cp < 0x180) return k0100[cp - 0x100];
    return "";
}

std::string mod_folder_chars(const std::string &base)
{
    // accented Latin letters folded to ASCII first (U+00DC -> U, U+00DF -> ss, U+0141 -> L); other non-ASCII text is dropped
    std::string folded;
    for (size_t i = 0; i < base.size();) {
        const unsigned char c = (unsigned char)base[i];
        if (c < 0x80) { folded += (char)c; i++; continue; }
        const size_t len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        unsigned cp = 0;
        bool ok = i + len <= base.size() && len > 1;
        if (ok) {
            cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            for (size_t k = 1; k < len; k++) {
                const unsigned char d = (unsigned char)base[i + k];
                if ((d & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (d & 0x3F);
            }
        }
        if (ok) folded += latin_ascii(cp);
        i += ok ? len : 1;
    }
    std::string name;
    for (char c : folded) if (isalnum((unsigned char)c) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')') name += c;
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    return name;
}

std::string fab_folder_name(const std::string &name, const std::vector<std::string> &existing,
                            const std::function<bool(const std::string &folder)> &is_same_install)
{
    std::string base = mod_folder_chars(name);
    if (base.empty()) base = "mod";
    if (mod_file_is_device_name(base)) {   // " mod" goes after the stem: "CON.txt" is still the device to Windows, "CON mod.txt" is not
        size_t dot = base.find('.');
        std::string stem = base.substr(0, dot), rest = dot == std::string::npos ? "" : base.substr(dot);
        while (!stem.empty() && stem.back() == ' ') stem.pop_back();
        base = stem + " mod" + rest;
    }
    for (int i = 0;; i++) {
        std::string cand = i == 0 ? base : i == 1 ? base + " (Fabricator)" : base + " (Fabricator " + std::to_string(i) + ")";
        const std::string *hit = nullptr;
        for (const std::string &e : existing) if (fold_case(e) == fold_case(cand)) { hit = &e; break; }
        if (!hit) return cand;
        if (is_same_install && is_same_install(*hit)) return *hit;   // this install's own folder (an update)
    }
}

bool fab_same_install(const FabInstallRecord &r, const FabInstallItem &it)
{
    if (r.source.kind != it.source.kind) return false;
    if (r.source.kind == "nexus")
        return r.source.mod_id > 0 && r.source.mod_id == it.source.mod_id && !r.source.game_domain.empty() &&
               lower_s(r.source.game_domain) == lower_s(it.source.game_domain);
    return r.source.kind == "file" && !r.name.empty() && fold_case(r.name) == fold_case(it.name);
}

// ---- install record ----
// (A record from a COGBox before this one may have come in a hand-added zip: such zips were not stripped of
// fabricator.json then.)
std::optional<FabInstallRecord> fab_read_record(const std::string &folder)
{
    std::string text;
    if (!read_file(join(folder, "fabricator.json"), text, 1u << 20)) return std::nullopt;
    auto j = parse_json(text);
    if (!j || !j->is_object()) return std::nullopt;
    auto src = j->find("source");
    if (src == j->end() || !src->is_object()) return std::nullopt;   // an older record shape ({"mod":..}) or a stranger's
    FabInstallRecord r;
    r.source.kind = js(*src, "kind"); r.source.game_domain = js(*src, "game_domain");
    r.source.mod_id = ji(*src, "mod_id"); r.source.file_id = ji(*src, "file_id");
    r.name = js(*j, "name"); r.version = js(*j, "version"); r.sha256 = js(*j, "sha256");
    if (r.source.kind == "file") { if (r.name.empty()) return std::nullopt; }
    else if (r.source.kind == "nexus") { if (r.source.game_domain.empty() || r.source.mod_id <= 0) return std::nullopt; }
    else return std::nullopt;
    return r;
}

bool fab_write_record(const std::string &folder, const FabInstallRecord &r)
{
    json src = {{"kind", r.source.kind}, {"game_domain", r.source.game_domain}, {"mod_id", r.source.mod_id}, {"file_id", r.source.file_id}};
    json j = {{"source", src}, {"name", r.name}, {"version", r.version}, {"sha256", r.sha256}};
    return write_file(join(folder, "fabricator.json"), j.dump(2, ' ', false, json::error_handler_t::replace) + "\n");   // write_file: temporary file, then replaced
}

// ---- zip check (the rules mods.cpp's unpacking keeps, checked before anything is written) ----


// no absolute, drive-letter / stream (':') path, no ".." component - also as Windows reads it (trailing dots and spaces dropped)
bool fab_path_unsafe(const std::string &raw)
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
// The first segment that is a Windows device name (mod_file_is_device_name), "" none.
std::string fab_device_segment(const std::string &name)
{
    for (const std::string &seg : segments(name)) if (mod_file_is_device_name(seg)) return seg;
    return "";
}
// The name as Windows compares it: one separator, no empty or "." segments, trailing dots and spaces off, case folded
// (Unicode lower case, for the scripts mod names use)
std::string fab_windows_key(const std::string &name)
{
    std::string key;
    for (const std::string &seg : segments(name)) {
        if (seg.empty() || seg == ".") continue;
        key += (key.empty() ? "" : "/") + trim_dots_spaces(seg);
    }
    return fold_case(key);
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

std::vector<std::string> fab_check_zip(const std::string &zip_path, const std::string &game, FabZipInfo *info)
{
    FabZipInfo own;
    FabZipInfo &zi = info ? *info : own;
    zi = FabZipInfo();
    zi.name = narrow(std::filesystem::path(widen(zip_path)).stem().wstring());   // until mod.ini names it
    zi.game = "gears3";
    std::vector<std::string> problems;
    auto note = [&](const std::string &p) { if (std::find(problems.begin(), problems.end(), p) == problems.end()) problems.push_back(p); };
    FILE *f = _wfopen(widen(zip_path).c_str(), L"rb");   // a wide path: folder and file names can be non-ASCII
    if (!f) { note("the zip file cannot be opened"); return problems; }
    struct Closer { FILE *f; ~Closer() { fclose(f); } } closer{f};
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_cfile(&z, f, 0, 0)) { note("not a valid zip file"); return problems; }
    struct Ender { mz_zip_archive *z; ~Ender() { mz_zip_reader_end(z); } } ender{&z};
    if (file_size(zip_path) > kFabMaxPackedBytes) note("the zip is over the packed size limit of " + std::to_string(kFabMaxPackedBytes) + " bytes");
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    if (n > kFabMaxFiles) { note("too many files: more than " + std::to_string(kFabMaxFiles) + " files in the zip"); return problems; }

    // (program files - mod_file_rules.h - are not looked at here: the unpack leaves them out and says so)
    int iniIdx = -1, iniDepth = 99, devicesShown = 0;
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
        if (fab_path_unsafe(name)) note("unsafe path in the zip (outside the mod folder)");
        if ((st.m_version_made_by >> 8) == 3 && ((st.m_external_attr >> 16) & 0xf000) == 0xa000) note("the zip contains a link (symlink), which is not allowed");
        const bool isDir = !name.empty() && name.back() == '/';
        const std::string dev = fab_device_segment(name);
        if (!dev.empty() && devicesShown++ < 5)
            note("Windows device name not allowed as a file or folder name (CON, PRN, AUX, NUL, COM1-9, LPT1-9): " + dev);
        const std::string key = fab_windows_key(name);
        if (!key.empty()) {
            for (size_t at = key.find('/'); at != std::string::npos; at = key.find('/', at + 1)) dirKeys.insert(key.substr(0, at));
            if (isDir) dirKeys.insert(key);
            else if (!fileKeys.insert(key).second) sameName = true;
        }
        if (isDir) continue;   // a directory: its name ends in '/'
        std::string norm = name;
        for (char &c : norm) if (c == '\\') c = '/';
        std::string low = lower_s(norm);
        int depth = low == "mod.ini" ? 0 : (low.size() > 8 && low.compare(low.size() - 8, 8, "/mod.ini") == 0 && low.find('/') == low.size() - 8) ? 1 : 99;
        if (depth < iniDepth) { iniDepth = depth; iniIdx = (int)i; }
        declared += st.m_uncomp_size;
        if (!st.m_is_encrypted) toInflate.push_back(i);
        if (declared > kFabMaxUnpackedBytes) { note("unpacked size is over the limit of " + std::to_string(kFabMaxUnpackedBytes) + " bytes"); return problems; }
    }
    for (const std::string &k : fileKeys) if (dirKeys.count(k)) sameName = true;   // a file and a folder of one name
    if (sameName) note("two entries in the zip have the same path or paths that differ only in case: Windows cannot keep both");
    if (!problems.empty()) return problems;

    // really inflate every entry and count the true bytes (the declared sizes can lie)
    uint64_t real = 0;
    std::string ini;
    for (mz_uint i : toInflate) {
        CountSink sink;
        sink.total = &real; sink.limit = kFabMaxUnpackedBytes;
        if ((int)i == iniIdx) { sink.keep = &ini; sink.keepMax = kFabMaxIniBytes + 1; }
        if (!mz_zip_reader_extract_to_callback(&z, i, count_cb, &sink, 0)) {
            if (sink.over) note("unpacked size is over the limit of " + std::to_string(kFabMaxUnpackedBytes) + " bytes");
            else note("an entry unpacks to a different size than declared (size mismatch) or is damaged");
            return problems;
        }
    }
    if (iniIdx >= 0) {
        if (ini.size() > kFabMaxIniBytes) { note("mod.ini is too large"); return problems; }
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
            else if (key == "name") { if (!value.empty()) zi.name = value; }   // an empty Name: the zip's name stays
            else if (key == "version") zi.version = value;
            else if (key == "author") zi.author = value;
            else if (key == "type") zi.type = value;
            else if (key == "description") zi.description = value;
        }
        zi.game = iniGame;
        if (iniGame != game) note("mod.ini is for game " + iniGame + ", but this is for " + game + ": wrong game");
        { const std::string ap = fab_api_problem(iniApi); if (!ap.empty()) note(ap); }
    }
    return problems;
}

std::string fab_api_problem(long api)
{
    if (api <= modcore::kModApiLevel) return "";
    return "mod.ini needs Api " + std::to_string(api) + ", which is newer than this COGBox supports (" + std::to_string(modcore::kModApiLevel) + ")";
}
