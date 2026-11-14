// COGBox - downloadable content that another installed package already covers (GitHub #283). See dlc_dupes.h.
#include "dlc_dupes.h"
#include "x360fs.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dlcdup {

namespace fs = std::filesystem;

namespace {
// UTF-8 <-> path the same way in C++17 and C++20 (generic_u8string is std::u8string in C++20)
std::string to_u8(const fs::path &p)
{
    const auto u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}
fs::path from_u8(const std::string &s)
{
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}
const std::string &disk_of(const File &f) { return f.disk.empty() ? f.path : f.disk; }
}  // namespace

std::vector<int> find_covered(const std::vector<Package> &pk, const SameFn &same)
{
    const int n = (int)pk.size();
    std::vector<int> cover(n, -1), order(n);
    for (int i = 0; i < n; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (pk[a].bytes != pk[b].bytes) return pk[a].bytes > pk[b].bytes;
        if (pk[a].files.size() != pk[b].files.size()) return pk[a].files.size() > pk[b].files.size();
        return pk[a].name < pk[b].name;
    });
    std::vector<std::unordered_map<std::string, const File *>> idx(n);
    for (int i = 0; i < n; i++)
        for (const File &f : pk[i].files) idx[i][f.path] = &f;
    std::vector<int> kept;
    for (int a : order) {
        // nothing in it, or a scan that isn't complete and plain (r22 B1 / B2 / R3): never "covered", and covers nothing
        if (pk[a].files.empty() || !pk[a].eligible) continue;
        for (int b : kept) {
            if (pk[b].files.size() < pk[a].files.size() || pk[b].bytes < pk[a].bytes) continue;
            bool all = true;
            for (const File &f : pk[a].files) {
                auto it = idx[b].find(f.path);
                if (it == idx[b].end() || it->second->size != f.size) { all = false; break; }
            }
            if (!all) continue;
            // same names and sizes: now the contents (only for these candidates, so most packages are never read)
            for (const File &f : pk[a].files)
                if (!same(a, f, b, *idx[b].at(f.path))) { all = false; break; }
            if (all) { cover[a] = b; break; }
        }
        if (cover[a] < 0) kept.push_back(a);
    }
    return cover;
}

bool is_link(const fs::path &p)
{
#ifdef _WIN32
    // std::filesystem (libstdc++ on Windows) doesn't see junctions / symlinks; the attributes do (also Wine's links
    // where Wine reports them as reparse points)
    const DWORD at = GetFileAttributesW(p.c_str());
    return at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    return ec || fs::is_symlink(st);
#endif
}

int same_physical(const fs::path &a, const fs::path &b)
{
#ifdef _WIN32
    auto id = [](const fs::path &p, BY_HANDLE_FILE_INFORMATION &fi) {
        HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);   // follows links: what they point at is what counts
        if (h == INVALID_HANDLE_VALUE) return false;
        const bool ok = GetFileInformationByHandle(h, &fi) != 0;
        CloseHandle(h);
        return ok;
    };
    BY_HANDLE_FILE_INFORMATION x, y;
    if (!id(a, x) || !id(b, y)) return -1;
    return x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
#else
    std::error_code ec;
    const bool same = fs::equivalent(a, b, ec);
    return ec ? -1 : same ? 1 : 0;
#endif
}

bool scan(const fs::path &dir, Package &out, size_t limit)
{
    out.files.clear();
    out.bytes = 0;
    out.eligible = true;
    out.why.clear();
    auto bad = [&](const char *why) { if (out.eligible) { out.eligible = false; out.why = why; } };
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { bad("not a folder"); return false; }
    if (is_link(dir)) { bad("the folder is a link"); return true; }   // r22 B1: never read (or judged) through a link
    try {
        fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end;   // r22 R3: no skipping
        for (; !ec && it != end; it.increment(ec)) {
            const fs::path p = it->path();
            if (is_link(p)) { bad("a link inside"); it.disable_recursion_pending(); continue; }   // B1: links inside
            const fs::file_status st = it->symlink_status(ec);
            if (ec) break;
            if (fs::is_directory(st)) continue;
            if (!fs::is_regular_file(st)) { bad("not a plain file inside"); continue; }
            File f;
            f.size = it->file_size(ec);
            if (ec) break;
            std::string rel = to_u8(p.lexically_relative(dir));   // r22 R3: no weakly_canonical per file
            if (rel.empty() || rel == "." || rel.compare(0, 2, "..") == 0) { bad("a name that can't be read"); continue; }
            if (out.files.size() >= limit) { bad("too many files"); break; }
            f.disk = rel;
            for (char &c : rel) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            f.path = rel;
            out.bytes += f.size;
            out.files.push_back(std::move(f));
        }
        if (ec) bad("a read error");
    } catch (...) {   // e.g. a name that isn't valid UTF-16 (generic_u8string throws)
        bad("a read error");
    }
    std::sort(out.files.begin(), out.files.end(), [](const File &a, const File &b) { return a.path < b.path; });
    for (size_t i = 1; i < out.files.size(); i++)
        if (out.files[i].path == out.files[i - 1].path) { bad("names that differ only in case"); break; }   // r22 B2
    return true;
}

std::string content_key(const fs::path &file, uint64_t size)
{
    FILE *f = nullptr;
#ifdef _WIN32
    f = _wfopen(file.c_str(), L"rb");
#else
    f = fopen(file.c_str(), "rb");
#endif
    if (!f) return "";
    constexpr uint64_t kWhole = 64ull << 20, kSample = 1ull << 20;
    std::vector<uint8_t> buf;
    bool ok = true;
    if (size <= kWhole) {
        buf.resize((size_t)size);
        ok = size == 0 || fread(buf.data(), 1, buf.size(), f) == buf.size();
    } else {
        for (int i = 0; i < 8; i++) buf.push_back(uint8_t(size >> (8 * i)));
        const uint64_t at[3] = { 0, size / 2 - kSample / 2, size - kSample };
        for (uint64_t o : at) {
            const size_t was = buf.size();
            buf.resize(was + (size_t)kSample);
#ifdef _WIN32
            ok = ok && _fseeki64(f, (long long)o, SEEK_SET) == 0;
#else
            ok = ok && fseeko(f, (off_t)o, SEEK_SET) == 0;
#endif
            ok = ok && fread(buf.data() + was, 1, (size_t)kSample, f) == kSample;
        }
    }
    fclose(f);
    return ok ? x360fs::sha1_hex(buf.data(), buf.size()) : "";
}

std::string KeyCache::key(const fs::path &file, uint64_t size)
{
    std::string id;
    try {
        std::error_code ec;
        const auto t = fs::last_write_time(file, ec);
        if (!ec) id = to_u8(file) + "|" + std::to_string(size) + "|" + std::to_string((long long)t.time_since_epoch().count());
    } catch (...) {
        id.clear();
    }
    if (!id.empty()) {
        std::lock_guard<std::mutex> l(m_);
        auto k = keys_.find(id);
        if (k != keys_.end()) return k->second;
    }
    reads_++;
    std::string k = content_key(file, size);
    if (!id.empty() && !k.empty()) {
        std::lock_guard<std::mutex> l(m_);
        if (keys_.size() > 200000) keys_.clear();
        keys_[id] = k;
    }
    return k;
}

std::vector<int> mark(const std::vector<std::pair<std::string, fs::path>> &dirs, KeyCache &kc, std::vector<Package> &pk)
{
    pk.assign(dirs.size(), Package());
    for (size_t i = 0; i < dirs.size(); i++) { pk[i].name = dirs[i].first; scan(dirs[i].second, pk[i]); }
    return find_covered(pk, [&](int x, const File &fx, int y, const File &fy) {
        // keyed by the real spelling on disk, never by the lower-cased path (r22 B2)
        const std::string kx = kc.key(dirs[x].second / from_u8(disk_of(fx)), fx.size);
        return !kx.empty() && kx == kc.key(dirs[y].second / from_u8(disk_of(fy)), fy.size);
    });
}

Verified verify(const std::vector<std::pair<std::string, fs::path>> &dirs, const std::vector<Pair> &shown, KeyCache &kc, const ProgressFn &progress)
{
    Verified v;
    std::vector<Package> pk;
    const std::vector<int> cover = mark(dirs, kc, pk);
    std::map<std::string, int> at;
    for (size_t i = 0; i < dirs.size(); i++) at[dirs[i].first] = (int)i;
    // r22 R4: only what the player was shown, and only while it is still marked with the same cover
    std::vector<std::pair<int, int>> todo;
    uint64_t total = 0, done = 0;
    for (const Pair &p : shown) {
        auto a = at.find(p.name), b = at.find(p.cover);
        if (a == at.end() || b == at.end() || cover[a->second] != b->second || cover[b->second] != -1 ||
            same_physical(dirs[a->second].second, dirs[b->second].second) != 0) {
            v.skipped.push_back(p.name);
            continue;
        }
        todo.push_back({ a->second, b->second });
        total += 2 * pk[a->second].bytes;
    }
    // r22 R2: every file, whole, in both packages (the marker's samples are never enough for a removal)
    for (const auto &t : todo) {
        const Package &A = pk[t.first], &B = pk[t.second];
        const fs::path &da = dirs[t.first].second, &db = dirs[t.second].second;
        std::map<std::string, const File *> inB;
        for (const File &f : B.files) inB[f.path] = &f;
        bool good = true;
        for (const File &f : A.files) {
            auto fb = inB.find(f.path);
            if (fb == inB.end() || fb->second->size != f.size) { good = false; break; }
            const fs::path pa = da / from_u8(disk_of(f)), pb = db / from_u8(disk_of(*fb->second));
            if (progress && !progress(done, total, disk_of(f))) { v.cancelled = true; v.ok.clear(); return v; }
            if (same_physical(pa, pb) != 0) { good = false; break; }   // one file under two names: not a second copy
            const std::string ha = x360fs::sha1_file(pa);
            done += f.size;
            if (progress && !progress(done, total, disk_of(f))) { v.cancelled = true; v.ok.clear(); return v; }
            const std::string hb = x360fs::sha1_file(pb);
            done += f.size;
            if (ha.empty() || ha != hb) { good = false; break; }
        }
        // the duplicate exactly as it was hashed, and the cover still holding all of it
        if (good) {
            Package again, cov;
            scan(da, again);
            scan(db, cov);
            good = again.eligible && cov.eligible && again.files.size() == A.files.size();
            for (size_t i = 0; good && i < A.files.size(); i++)
                good = again.files[i].path == A.files[i].path && again.files[i].disk == A.files[i].disk && again.files[i].size == A.files[i].size;
            std::map<std::string, uint64_t> c;
            for (const File &f : cov.files) c[f.path] = f.size;
            for (size_t i = 0; good && i < A.files.size(); i++) {
                auto k = c.find(A.files[i].path);
                good = k != c.end() && k->second == A.files[i].size;
            }
        }
        if (good) v.ok.push_back({ A.name, B.name });
        else v.skipped.push_back(A.name);
    }
    if (progress) progress(total, total, "");
    return v;
}

}  // namespace dlcdup
