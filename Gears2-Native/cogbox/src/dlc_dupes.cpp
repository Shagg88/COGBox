// COGBox - downloadable content that another installed package already covers (GitHub #283). See dlc_dupes.h.
#include "dlc_dupes.h"
#include "x360fs.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace dlcdup {

namespace fs = std::filesystem;

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
        if (pk[a].files.empty()) continue;   // nothing in it: never "covered", and covers nothing
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

bool scan(const fs::path &dir, Package &out)
{
    out.files.clear();
    out.bytes = 0;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_symlink(ec)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }   // links: not ours to judge
        if (!it->is_regular_file(ec)) continue;
        File f;
        f.size = it->file_size(ec);
        if (ec) { ec.clear(); continue; }
        const auto u8 = fs::relative(it->path(), dir, ec).generic_u8string();   // (std::string in C++17, std::u8string in C++20)
        std::string rel(u8.begin(), u8.end());
        if (ec || rel.empty()) { ec.clear(); continue; }
        f.disk = rel;
        for (char &c : rel) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        f.path = rel;
        out.bytes += f.size;
        out.files.push_back(std::move(f));
        if (out.files.size() > 100000) break;   // a folder that isn't DLC; enough to compare
    }
    std::sort(out.files.begin(), out.files.end(), [](const File &a, const File &b) { return a.path < b.path; });
    return !ec;
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

}  // namespace dlcdup
