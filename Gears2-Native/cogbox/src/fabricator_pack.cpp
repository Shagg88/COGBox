// COGBox - a mod folder packed for publishing (see fabricator_pack.h).
#include "fabricator_pack.h"
#include "util.h"

#include "../third_party/miniz/miniz.h"
#include "../third_party/nlohmann/json.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <vector>

using json = nlohmann::json;

namespace {

struct PackFile {
    std::string rel;   // '\' separated, under the folder
    uint64_t size = 0;
    uint64_t time = 0;   // last write (FILETIME)
};

std::string sha256_hex(const std::string &data)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char d[32];
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return out;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0);
        if (BCryptFinishHash(h, d, 32, 0) == 0)
            for (unsigned char c : d) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::string manifest_of(const std::string &zip) { return zip.substr(0, zip.size() - 4) + ".json"; }

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
// What goes into the zip: every file under the folder except what COGBox itself keeps beside a mod (fabricator.json,
// '.'-named entries such as .fabricator-backup, unfinished .partial unpackings). Links (junctions / symlinks) are
// listed in links: they aren't followed or packed, and the publish says so.
bool skipped_name(const std::string &n)
{
    std::string low = n;
    for (char &c : low) c = (char)tolower((unsigned char)c);
    return n.empty() || n[0] == '.' || low == "fabricator.json" || (low.size() >= 8 && low.compare(low.size() - 8, 8, ".partial") == 0);
}
void list_files(const std::string &root, const std::string &rel, std::vector<PackFile> &out, std::vector<std::string> &links,
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
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) list_files(root, r, out, links, maxFiles);
        else out.push_back({ r, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                             ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime });
    } while (out.size() <= maxFiles && FindNextFileW(f, &fd));
    FindClose(f);
}

json files_json(const std::vector<PackFile> &files)
{
    json a = json::array();
    for (const PackFile &f : files) a.push_back(json::array({ f.rel, f.size, f.time }));
    return a;
}

// "" when written; "cancelled"; else what went wrong. A failure leaves no zip.
std::string zip_files(const std::string &dir, const std::vector<PackFile> &files, const std::string &zip, FabProgress &p)
{
    const uint64_t kMaxPacked = 4ull << 30;
    FILE *out = _wfopen(widen(zip).c_str(), L"wb");
    if (!out) return "The zip for the upload couldn't be made (is the disk full?).";
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    bool ok = mz_zip_writer_init_cfile(&z, out, 0) != 0;
    std::string err;
    for (size_t i = 0; ok && i < files.size(); i++) {
        if (p.cancel) { err = "cancelled"; ok = false; break; }
        if (z.m_archive_size > kMaxPacked) { err = "The zip is over 4 GB - the store's limit."; ok = false; break; }
        std::string name = files[i].rel;
        for (char &c : name) if (c == '\\') c = '/';
        FILE *in = _wfopen(widen(join(dir, files[i].rel)).c_str(), L"rb");
        if (!in) { err = "A file in the folder couldn't be read: " + name; ok = false; break; }
        ReadCtx rc{ in, &p };
        ok = mz_zip_writer_add_read_buf_callback(&z, name.c_str(), read_piece, &rc, files[i].size, nullptr, nullptr, 0, MZ_BEST_SPEED,
                                                 nullptr, 0, nullptr, 0) != 0;
        fclose(in);
        if (!ok) err = p.cancel ? "cancelled" : "The zip for the upload couldn't be written (is the disk full?).";
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&z) != 0;
    if (ok && z.m_archive_size > kMaxPacked) { err = "The zip is over 4 GB - the store's limit."; ok = false; }
    if (!ok && err.empty()) err = "The zip for the upload couldn't be written (is the disk full?).";
    mz_zip_writer_end(&z);
    fclose(out);
    if (!ok) DeleteFileW(widen(zip).c_str());
    return ok ? "" : err;
}

}  // namespace

std::string fab_pack_path(const std::string &folder)
{
    std::wstring w = widen(folder);
    wchar_t full[32768];
    const DWORD n = GetFullPathNameW(w.c_str(), 32768, full, nullptr);
    if (n > 0 && n < 32768) w.assign(full, n);
    while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    for (wchar_t &c : w) if (c == L'/') c = L'\\';
    if (!w.empty()) {   // Windows paths: one folder, any case
        std::wstring low(w.size(), L'\0');
        if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.c_str(), (int)w.size(), &low[0], (int)low.size(), nullptr, nullptr, 0) ==
            (int)w.size())
            w = low;
    }
    return join(join(fab_state_dir(), "uploads"), sha256_hex(narrow(w)).substr(0, 16) + ".zip");
}

std::string fab_pack_folder(const std::string &folder, FabProgress &p, std::string *zip, bool *reused,
                            const std::function<void()> &before_repack)
{
    const size_t kMaxFiles = 50000;
    const uint64_t kMaxUnpacked = 8ull << 30;
    *reused = false;
    const std::string path = fab_pack_path(folder), manifest = manifest_of(path);
    *zip = path;
    std::vector<PackFile> files;
    std::vector<std::string> links;
    list_files(folder, "", files, links, kMaxFiles);
    if (!links.empty()) {
        std::string s = "The folder has links (junctions or symlinks), which aren't packed - copy the files in instead: " + links[0];
        for (size_t i = 1; i < links.size() && i < 3; i++) s += ", " + links[i];
        return s + (links.size() > 3 ? ", ..." : "");
    }
    if (files.empty()) return "The folder is empty.";
    if (files.size() > kMaxFiles) return "The folder has over 50000 files - the store's limit.";
    uint64_t total = 0;
    for (auto &f : files) total += f.size;
    if (total > kMaxUnpacked) return "The folder is over 8 GB - the store's limit for a mod unpacked.";

    // the zip of an earlier publish of this folder, when nothing in the folder changed since
    const json now = files_json(files);
    std::string text;
    const bool hadPack = file_exists(path) || file_exists(manifest);
    if (hadPack && read_file(manifest, text, 64u << 20)) {
        json m = json::parse(text, nullptr, false);
        if (m.is_object() && m.contains("files") && m["files"] == now && m.value("zip_size", (uint64_t)0) == file_size(path) &&
            file_size(path) > 0) {
            *reused = true;
            p.total = (int64_t)total;
            p.done = (int64_t)total;
            return "";
        }
    }
    if (hadPack && before_repack) before_repack();
    fab_pack_drop(path);
    if (!make_dirs(join(fab_state_dir(), "uploads"))) return "The zip for the upload couldn't be made (is the disk full?).";
    p.done = 0;
    p.total = (int64_t)total;
    const std::string e = zip_files(folder, files, path, p);
    if (!e.empty()) return e;
    if (!write_file(manifest, json{ { "zip_size", file_size(path) }, { "files", now } }.dump())) {
        fab_pack_drop(path);   // (without its manifest it could never be reused, only left behind)
        return "The zip for the upload couldn't be made (is the disk full?).";
    }
    return "";
}

void fab_pack_drop(const std::string &zip)
{
    if (zip.size() < 4) return;
    DeleteFileW(widen(zip).c_str());
    DeleteFileW(widen(manifest_of(zip)).c_str());
}

void fab_pack_sweep(int64_t max_age_s, const std::string &keep)
{
    const std::string dir = join(fab_state_dir(), "uploads");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(widen(join(dir, "*.zip")).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t nowT = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    std::vector<std::string> old;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const uint64_t t = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        const std::string p = join(dir, narrow(fd.cFileName));
        if (t + (uint64_t)max_age_s * 10000000ull < nowT && _wcsicmp(widen(p).c_str(), widen(keep).c_str()) != 0) old.push_back(p);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    for (const std::string &p : old) fab_pack_drop(p);
}
