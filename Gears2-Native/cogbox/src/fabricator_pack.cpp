// COGBox - the Creator's folder check and pack (see fabricator_pack.h).
#include "fabricator_pack.h"
#include "fabricator_model.h"
#include "mod_file_rules.h"
#include "util.h"

#include "../third_party/miniz/miniz.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace {

const uint64_t kMaxPackedBytes = 4ull << 30, kMaxUnpackedBytes = 8ull << 30;
const size_t kMaxFiles = 50000;

struct PackFile {
    std::string rel;   // '\' separated, under the folder
    uint64_t size = 0;
    bool dir = false;
};

std::string lower_s(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// What COGBox keeps beside a mod and the pack leaves out: fabricator.json, '.'-named entries such as .fabricator-backup
// and .git, unfinished ".partial" unpackings.
bool skipped_name(const std::string &n)
{
    const std::string low = lower_s(n);
    return n.empty() || n[0] == '.' || low == "fabricator.json" || (low.size() >= 8 && low.compare(low.size() - 8, 8, ".partial") == 0);
}

// Everything under root that is not skipped (files and folders, in walk order). Links (junctions / symlinks) are listed
// in links: not followed. Stops once more than maxFiles files are listed.
void list_entries(const std::string &root, const std::string &rel, std::vector<PackFile> &out, std::vector<std::string> &links,
                  size_t maxFiles)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(widen(join(rel.empty() ? root : join(root, rel), "*")).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const std::string n = narrow(fd.cFileName);
        if (n == "." || n == ".." || skipped_name(n)) continue;
        const std::string r = rel.empty() ? n : rel + "\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { links.push_back(r); continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            out.push_back({ r, 0, true });
            list_entries(root, r, out, links, maxFiles);
        } else
            out.push_back({ r, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, false });
    } while (out.size() <= maxFiles && FindNextFileW(f, &fd));
    FindClose(f);
}

std::string slashed(std::string s)
{
    for (char &c : s) if (c == '\\') c = '/';
    return s;
}

// "a, b, c, ..." of at most three names
std::string some(const std::vector<std::string> &v)
{
    std::string s = slashed(v[0]);
    for (size_t i = 1; i < v.size() && i < 3; i++) s += ", " + slashed(v[i]);
    return s + (v.size() > 3 ? ", ..." : "");
}

struct ReadCtx {
    FILE *f = nullptr;
    FabProgress *p = nullptr;
};
size_t read_piece(void *opaque, mz_uint64 ofs, void *buf, size_t n)
{
    ReadCtx *c = (ReadCtx *)opaque;
    if (c->p->cancel) return 0;
    if (_fseeki64(c->f, (__int64)ofs, SEEK_SET) != 0) return 0;
    const size_t got = fread(buf, 1, n, c->f);
    c->p->done += (int64_t)got;
    return got;
}

const char *kLimitPacked = "The zip is over 4 GB - the limit for a mod.";

// "" when written to tmp; "cancelled"; else what went wrong. A failure deletes tmp.
std::string zip_files(const std::string &dir, const std::vector<PackFile> &files, const std::string &tmp, FabProgress &p)
{
    FILE *out = _wfopen(widen(tmp).c_str(), L"wb");
    if (!out) return "The zip couldn't be made (is the disk full, or the folder not writable?).";
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    bool ok = mz_zip_writer_init_cfile(&z, out, 0) != 0;
    std::string err;
    for (size_t i = 0; ok && i < files.size(); i++) {
        if (p.cancel) { err = "cancelled"; ok = false; break; }
        if (z.m_archive_size > kMaxPackedBytes) { err = kLimitPacked; ok = false; break; }
        const std::string name = slashed(files[i].rel);
        FILE *in = _wfopen(widen(join(dir, files[i].rel)).c_str(), L"rb");
        if (!in) { err = "A file in the folder couldn't be read: " + name; ok = false; break; }
        ReadCtx rc{ in, &p };
        ok = mz_zip_writer_add_read_buf_callback(&z, name.c_str(), read_piece, &rc, files[i].size, nullptr, nullptr, 0, MZ_BEST_SPEED,
                                                 nullptr, 0, nullptr, 0) != 0;
        fclose(in);
        if (!ok) err = p.cancel ? "cancelled" : "The zip couldn't be written (is the disk full?).";
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&z) != 0;
    if (ok && z.m_archive_size > kMaxPackedBytes) { err = kLimitPacked; ok = false; }
    if (!ok && err.empty()) err = "The zip couldn't be written (is the disk full?).";
    mz_zip_writer_end(&z);
    const bool closed = fclose(out) == 0;
    if (ok && !closed) { err = "The zip couldn't be written (is the disk full?)."; ok = false; }
    if (!ok) DeleteFileW(widen(tmp).c_str());
    return ok ? "" : err;
}

}  // namespace

std::string fab_pack_folder(const std::string &folder, const std::string &zip_path, FabProgress &p)
{
    p.done = 0;
    p.total = 0;
    std::vector<PackFile> entries;
    std::vector<std::string> links;
    list_entries(folder, "", entries, links, kMaxFiles);
    if (!links.empty())
        return "The folder has links (junctions or symlinks), which aren't packed - copy the files in instead: " + some(links);
    std::vector<PackFile> files;
    uint64_t total = 0;
    for (const PackFile &e : entries) {
        if (e.dir || mod_file_is_program(e.rel)) continue;   // (programs and nested archives never go in a package)
        files.push_back(e);
        total += e.size;
    }
    if (files.empty()) return "The folder is empty.";
    if (files.size() > kMaxFiles) return "The folder has over 50000 files - the limit for a mod.";
    if (total > kMaxUnpackedBytes) return "The folder is over 8 GB - the limit for a mod unpacked.";
    p.total = (int64_t)total;

    const std::wstring wzip = widen(zip_path);
    const size_t slash = zip_path.find_last_of("\\/");
    if (slash != std::string::npos && slash > 0 && !make_dirs(zip_path.substr(0, slash)))
        return "The zip couldn't be made (is the disk full, or the folder not writable?).";
    const std::string tmp = zip_path + ".tmp-cogbox";
    const std::string e = zip_files(folder, files, tmp, p);
    if (!e.empty()) return e;
    if (!MoveFileExW(widen(tmp).c_str(), wzip.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(widen(tmp).c_str());
        return "The zip couldn't be made (is the disk full, or the folder not writable?).";
    }
    return "";
}

std::vector<std::string> fab_check_folder(const std::string &folder, const std::string &game)
{
    std::vector<std::string> problems;
    auto note = [&](const std::string &s) { if (std::find(problems.begin(), problems.end(), s) == problems.end()) problems.push_back(s); };
    std::vector<PackFile> entries;
    std::vector<std::string> links;
    list_entries(folder, "", entries, links, kMaxFiles);
    for (const std::string &l : links) note("link (junction or symlink), which isn't packed: " + slashed(l));

    int shown = 0;
    uint64_t total = 0;
    size_t nfiles = 0;
    std::set<std::string> keys;
    bool sameName = false;
    for (const PackFile &e : entries) {
        const std::string name = slashed(e.rel);
        if (!e.dir && mod_file_is_program(e.rel)) note("program file or nested archive (not allowed in a mod): " + name);
        if (fab_path_unsafe(e.rel)) note("unsafe name (outside the mod folder): " + name);
        const std::string dev = fab_device_segment(e.rel);
        if (!dev.empty() && shown++ < 5)
            note("Windows device name not allowed as a file or folder name (CON, PRN, AUX, NUL, COM1-9, LPT1-9): " + dev);
        if (!keys.insert(fab_windows_key(e.rel)).second) sameName = true;
        if (!e.dir) { nfiles++; total += e.size; }
    }
    if (sameName) note("two entries have the same name or names that differ only in case: Windows cannot keep both");
    if (nfiles > kMaxFiles) note("too many files: more than " + std::to_string(kMaxFiles) + " files");
    if (total > kMaxUnpackedBytes) note("unpacked size is over the limit of " + std::to_string(kMaxUnpackedBytes) + " bytes");

    std::string iniText;
    if (!read_file(join(folder, "mod.ini"), iniText, 1u << 20)) { note("no mod.ini"); return problems; }
    const std::optional<ModIniFields> f = mod_ini_read(folder);
    if (f) {
        if (f->name.empty()) note("mod.ini has no Name");
        if (f->game.empty()) note("mod.ini has no Game");
        else if (f->game != game) note("mod.ini is for game " + f->game + ", but this is for " + game + ": wrong game");
    }
    // Api: not in ModIniFields (the Creator form doesn't edit it), read here the way modcore does
    if (iniText.compare(0, 3, "\xEF\xBB\xBF") == 0) iniText.erase(0, 3);
    long api = -1;
    bool mine = true;
    for (size_t at = 0; at <= iniText.size();) {
        size_t en = iniText.find('\n', at);
        if (en == std::string::npos) en = iniText.size();
        std::string line = iniText.substr(at, en - at);
        at = en + 1;
        const size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a, b - a + 1);
        if (line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t c = line.find(']');
            std::string sec = lower_s(line.substr(1, c == std::string::npos ? std::string::npos : c - 1));
            const size_t x = sec.find_first_not_of(" \t"), y = sec.find_last_not_of(" \t");
            sec = x == std::string::npos ? "" : sec.substr(x, y - x + 1);
            mine = sec == "mod";
            continue;
        }
        const size_t eq = line.find('=');
        if (!mine || eq == std::string::npos) continue;
        std::string key = lower_s(line.substr(0, eq)), val = line.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (key == "api") {
            char *end = nullptr;
            const long v = strtol(val.c_str(), &end, 10);
            if (end != val.c_str()) api = v;
        }
    }
    if (api > kFabModApiLevel)
        note("mod.ini needs Api " + std::to_string(api) + ", which is newer than this COGBox supports (" + std::to_string(kFabModApiLevel) + ")");
    return problems;
}

std::string fab_package_name(const ModIniFields &f)
{
    // name-version; the dash only between two parts that survive the character filter ("a" + "-" + "b" filters as one)
    const std::string n = mod_folder_chars(f.name), v = mod_folder_chars(f.version);
    const std::string base = n.empty() || v.empty() ? n + v : mod_folder_chars(n + "-" + v);
    return (base.empty() ? std::string("mod") : base) + ".zip";
}
