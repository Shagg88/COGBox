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

struct Listing {
    std::vector<PackFile> entries;     // files and folders, in walk order
    std::vector<std::string> links;    // symlinks / junctions: not followed
    std::vector<std::string> unreadable;   // folders that couldn't be listed
    size_t files = 0;
    bool truncated = false;            // more than maxFiles files: the listing stopped
};

// Everything under root that is not skipped. Only symlinks and junctions are links (other reparse points, such as
// OneDrive placeholders, are ordinary files and folders).
void list_entries(const std::string &root, const std::string &rel, Listing &L, size_t maxFiles)
{
    const std::string here = rel.empty() ? root : join(root, rel);
    // review r369 F1: an entry whose real path isn't <this folder>\<name> is a link to somewhere else - also a Wine Unix
    // symlink, which has no reparse-point attribute - and is listed as a link (never packed or copied)
    const std::wstring realHere = real_path(widen(here));
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(widen(join(here, "*")).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE || realHere.empty()) { if (f != INVALID_HANDLE_VALUE) FindClose(f); L.unreadable.push_back(rel.empty() ? "(the folder)" : rel); return; }
    do {
        const std::string n = narrow(fd.cFileName);
        if (n == "." || n == ".." || skipped_name(n)) continue;
        const std::string r = rel.empty() ? n : rel + "\\" + n;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) { L.links.push_back(r); continue; }
        if (!is_plain_child(realHere, widen(join(here, n)), fd.cFileName)) { L.links.push_back(r); continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            L.entries.push_back({ r, 0, true });
            list_entries(root, r, L, maxFiles);
        } else {
            L.entries.push_back({ r, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, false });
            L.files++;
        }
        if (L.files > maxFiles) { L.truncated = true; break; }
    } while (FindNextFileW(f, &fd));
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
    FILE *out = nullptr;   // the zip being written: a big file stops the pack once the zip is over the limit
    bool over = false;
};
size_t read_piece(void *opaque, mz_uint64 ofs, void *buf, size_t n)
{
    ReadCtx *c = (ReadCtx *)opaque;
    if (c->p->cancel) return 0;
    if (c->out && (uint64_t)_ftelli64(c->out) > kFabMaxPackedBytes) { c->over = true; return 0; }
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
        if (z.m_archive_size > kFabMaxPackedBytes) { err = kLimitPacked; ok = false; break; }
        const std::string name = slashed(files[i].rel);
        FILE *in = _wfopen(widen(join(dir, files[i].rel)).c_str(), L"rb");
        if (!in) { err = "A file in the folder couldn't be read: " + name; ok = false; break; }
        ReadCtx rc{ in, &p, out, false };
        ok = mz_zip_writer_add_read_buf_callback(&z, name.c_str(), read_piece, &rc, files[i].size, nullptr, nullptr, 0, MZ_BEST_SPEED,
                                                 nullptr, 0, nullptr, 0) != 0;
        fclose(in);
        if (!ok) err = rc.over ? kLimitPacked : p.cancel ? "cancelled" : "The zip couldn't be written (is the disk full?).";
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&z) != 0;
    if (ok && z.m_archive_size > kFabMaxPackedBytes) { err = kLimitPacked; ok = false; }
    if (!ok && err.empty()) err = "The zip couldn't be written (is the disk full?).";
    mz_zip_writer_end(&z);
    const bool closed = fclose(out) == 0;
    if (ok && !closed) { err = "The zip couldn't be written (is the disk full?)."; ok = false; }
    if (!ok) DeleteFileW(widen(tmp).c_str());
    return ok ? "" : err;
}

}  // namespace

std::string fab_pack_folder(const std::string &folder, const std::string &zip_path, FabProgress &p, std::vector<std::string> *left_out)
{
    return fab_pack_folder_limit(folder, zip_path, p, kFabMaxFiles, left_out);
}

std::string fab_pack_folder_limit(const std::string &folder, const std::string &zip_path, FabProgress &p, size_t max_files,
                                  std::vector<std::string> *left_out)
{
    if (left_out) left_out->clear();
    p.done = 0;
    p.total = 0;
    Listing L;
    list_entries(folder, "", L, max_files);
    if (!L.unreadable.empty()) return "The folder couldn't be read: " + some(L.unreadable);
    if (!L.links.empty())
        return "The folder has links (junctions or symlinks), which aren't packed - copy the files in instead: " + some(L.links);
    if (L.truncated) return "The folder has over " + std::to_string(max_files) + " files - the limit for a mod.";
    std::vector<PackFile> files;
    uint64_t total = 0;
    for (const PackFile &e : L.entries) {
        if (e.dir) continue;
        if (mod_file_is_program(e.rel)) {   // (programs and nested archives never go in a package)
            if (left_out) left_out->push_back(e.rel);
            continue;
        }
        files.push_back(e);
        total += e.size;
    }
    if (files.empty()) return "The folder is empty.";
    if (total > kFabMaxUnpackedBytes) return "The folder is over 8 GB - the limit for a mod unpacked.";
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
    return fab_check_folder_limit(folder, game, kFabMaxFiles);
}

std::vector<std::string> fab_check_folder_limit(const std::string &folder, const std::string &game, size_t max_files)
{
    std::vector<std::string> problems;
    auto note = [&](const std::string &s) { if (std::find(problems.begin(), problems.end(), s) == problems.end()) problems.push_back(s); };
    Listing L;
    list_entries(folder, "", L, max_files);
    for (const std::string &l : L.links) note("link (junction or symlink), which isn't packed: " + slashed(l));
    for (const std::string &u : L.unreadable) note("couldn't read " + slashed(u));
    if (L.truncated) note("too many files: more than " + std::to_string(max_files) + " files");

    int shown = 0;
    uint64_t total = 0;
    std::set<std::string> keys;
    bool sameName = false;
    for (const PackFile &e : L.entries) {
        const std::string name = slashed(e.rel);
        if (!e.dir && mod_file_is_program(e.rel)) note("program file or nested archive (not allowed in a mod): " + name);
        if (fab_path_unsafe(e.rel)) note("unsafe name (outside the mod folder): " + name);
        const std::string dev = fab_device_segment(e.rel);
        if (!dev.empty() && shown++ < 5)
            note("Windows device name not allowed as a file or folder name (CON, PRN, AUX, NUL, COM1-9, LPT1-9): " + dev);
        if (!keys.insert(fab_windows_key(e.rel)).second) sameName = true;
        if (!e.dir) total += e.size;
    }
    if (sameName) note("two entries have the same name or names that differ only in case: Windows cannot keep both");
    if (total > kFabMaxUnpackedBytes) note("unpacked size is over the limit of " + std::to_string(kFabMaxUnpackedBytes) + " bytes");

    const std::string ini = join(folder, "mod.ini");
    if (!file_exists(ini)) { note("no mod.ini"); return problems; }
    if (file_size(ini) > kFabMaxIniBytes) { note("mod.ini is too large"); return problems; }
    const std::optional<ModIniFields> f = mod_ini_read(folder);
    if (!f) { note("mod.ini cannot be read"); return problems; }
    if (f->name.empty()) note("mod.ini has no Name");
    if (f->game.empty()) note("mod.ini has no Game");
    else if (f->game != game) note("mod.ini is for game " + f->game + ", but this is for " + game + ": wrong game");
    const std::string ap = fab_api_problem(f->api);
    if (!ap.empty()) note(ap);
    return problems;
}

std::string fab_package_name(const ModIniFields &f)
{
    // name-version; the dash only between two parts that survive the character filter ("a" + "-" + "b" filters as one)
    const std::string n = mod_folder_chars(f.name), v = mod_folder_chars(f.version);
    const std::string base = n.empty() || v.empty() ? n + v : mod_folder_chars(n + "-" + v);
    return (base.empty() ? std::string("mod") : base) + ".zip";
}
