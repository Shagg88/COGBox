// COGBox - reading the player's own Xbox 360 game copies (see x360fs.h)
#include "x360fs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>

namespace x360fs {

namespace {

// ============================================================================================================
// small helpers
// ============================================================================================================

uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le24(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); }
uint32_t le32(const uint8_t *p) { return le24(p) | (uint32_t(p[3]) << 24); }
uint16_t be16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t be24(const uint8_t *p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]); }
uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | be24(p + 1); }
uint64_t be64(const uint8_t *p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

std::string lower(std::string s)
{
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
bool ieq(const std::string &a, const std::string &b) { return lower(a) == lower(b); }

std::string num(uint64_t v) { return std::to_string(v); }

// UTF-8 text of a path (for messages and progress)
std::string path_text(const fs::path &p)
{
#if defined(__cpp_char8_t)
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.u8string();
#endif
}
fs::path path_from_utf8(const std::string &s)
{
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

void append_utf8(std::string &out, uint32_t c)
{
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    else {
        out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F));
    }
}

// UTF-16BE (up to 'chars' units, stops at a NUL) -> UTF-8
std::string utf16be_to_utf8(const uint8_t *p, size_t chars)
{
    std::string out;
    for (size_t i = 0; i < chars; i++) {
        uint32_t c = be16(p + i * 2);
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < chars) {
            uint32_t d = be16(p + (i + 1) * 2);
            if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
            else c = 0xFFFD;
        } else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
        append_utf8(out, c);
    }
    return out;
}

// A name read from an image -> one safe path component (UTF-8), or false to skip the entry.
// Disc names are 8-bit; bytes >= 0x80 are taken as Latin-1 (UNSURE what the console uses; Gears has none).
bool safe_name(const std::string &raw, std::string &out)
{
    out.clear();
    if (raw.empty() || raw == "." || raw == "..") return false;
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7F || std::strchr("/\\:*?\"<>|", c)) out += '_';
        else if (c >= 0x80) append_utf8(out, c);
        else out += char(c);
    }
    // Windows drops trailing dots and spaces, which could turn "x." into "x" or "..." into ""
    if (out.back() == '.' || out.back() == ' ') out.back() = '_';
    // Windows device names (CON, NUL, COM1.txt, ...) cannot be files
    std::string stem = lower(out.substr(0, out.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    static const char *const dev[] = { "con", "prn", "aux", "nul", "conin$", "conout$" };
    bool reserved = false;
    for (const char *d : dev) reserved = reserved || stem == d;
    if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && stem[3] >= '0' && stem[3] <= '9')
        reserved = true;
    if (reserved) out = "_" + out;
    return true;
}

std::string join(const std::vector<std::string> &parts)
{
    std::string s;
    for (size_t i = 0; i < parts.size(); i++) { if (i) s += '/'; s += parts[i]; }
    return s;
}
fs::path out_path(const fs::path &root, const std::vector<std::string> &parts)
{
    fs::path p = root;
    for (const auto &c : parts) p /= path_from_utf8(c);
    return p;
}

// ============================================================================================================
// SHA-1 (FIPS 180-4)
// ============================================================================================================

class Sha1 {
public:
    Sha1() { reset(); }
    void reset()
    {
        h_[0] = 0x67452301; h_[1] = 0xEFCDAB89; h_[2] = 0x98BADCFE; h_[3] = 0x10325476; h_[4] = 0xC3D2E1F0;
        len_ = 0; total_ = 0;
    }
    void update(const void *data, size_t n)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        total_ += n;
        if (len_) {
            size_t take = std::min(n, sizeof buf_ - len_);
            std::memcpy(buf_ + len_, p, take);
            len_ += take; p += take; n -= take;
            if (len_ == sizeof buf_) { block(buf_); len_ = 0; }
        }
        while (n >= 64) { block(p); p += 64; n -= 64; }
        if (n) { std::memcpy(buf_, p, n); len_ = n; }
    }
    void final(uint8_t out[20])
    {
        uint64_t bits = total_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (len_ != 56) update(&zero, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
        update(l, 8);
        for (int i = 0; i < 5; i++)
            for (int k = 0; k < 4; k++) out[i * 4 + k] = uint8_t(h_[i] >> (24 - 8 * k));
    }

private:
    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    void block(const uint8_t *p)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = be32(p + i * 4);
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    }
    uint32_t h_[5];
    uint8_t buf_[64];
    size_t len_;
    uint64_t total_;
};

std::string hex(const uint8_t *p, size_t n)
{
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

void sha1(const void *data, size_t n, uint8_t out[20])
{
    Sha1 s;
    s.update(data, n);
    s.final(out);
}

// ============================================================================================================
// files (64-bit offsets, wide paths on Windows)
// ============================================================================================================

FILE *open_file(const fs::path &p, bool write)
{
#ifdef _WIN32
    return _wfopen(p.c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(p.c_str(), write ? "wb" : "rb");
#endif
}

bool seek_file(FILE *f, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(f, static_cast<long long>(off), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
}

class InFile {
public:
    InFile() = default;
    InFile(const InFile &) = delete;
    InFile &operator=(const InFile &) = delete;
    ~InFile() { close(); }
    bool open(const fs::path &p)
    {
        close();
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return false;
        f_ = open_file(p, false);
        if (!f_) return false;
        size_ = fs::file_size(p, ec);
        if (ec) { close(); return false; }
        pos_ = 0;
        return true;
    }
    void close()
    {
        if (f_) std::fclose(f_);
        f_ = nullptr;
        size_ = 0;
    }
    bool is_open() const { return f_ != nullptr; }
    uint64_t size() const { return size_; }
    // exactly n bytes at off, or false (also when the range runs past the end)
    bool read_at(uint64_t off, void *buf, size_t n)
    {
        if (!f_ || off > size_ || n > size_ - off) return false;
        if (!n) return true;
        if (pos_ != off && !seek_file(f_, off)) { pos_ = UINT64_MAX; return false; }
        size_t got = std::fread(buf, 1, n, f_);
        pos_ = off + got;
        if (got != n) { pos_ = UINT64_MAX; return false; }
        return true;
    }

private:
    FILE *f_ = nullptr;
    uint64_t size_ = 0, pos_ = 0;
};

class OutFile {
public:
    OutFile() = default;
    OutFile(const OutFile &) = delete;
    OutFile &operator=(const OutFile &) = delete;
    ~OutFile() { if (f_) std::fclose(f_); }
    bool open(const fs::path &p)
    {
        f_ = open_file(p, true);
        return f_ != nullptr;
    }
    bool write(const void *p, size_t n) { return !n || std::fwrite(p, 1, n, f_) == n; }
    bool close()
    {
        bool ok = std::fclose(f_) == 0;
        f_ = nullptr;
        return ok;
    }

private:
    FILE *f_ = nullptr;
};

// ============================================================================================================
// progress / output folder helpers
// ============================================================================================================

const char *const kCancelled = "The copy was cancelled.";

struct Progress {
    ProgressFn fn;
    CancelFlag cancel = nullptr;
    uint64_t done = 0, total = 0, last = 0;
    std::string cur;
    bool cancelled() const { return cancel && cancel->load(); }
    void file(const std::string &name)
    {
        cur = name;
        if (fn) fn(done, total, cur);
        last = done;
    }
    void add(uint64_t n)
    {
        done += n;
        if (fn && done - last >= (4u << 20)) { fn(done, total, cur); last = done; }
    }
    void finish()
    {
        if (fn) fn(done, total, std::string());
    }
};

std::string cannot_write(const fs::path &p)
{
    return "Could not write " + path_text(p) + ". Check that the folder can be written to and that the drive has enough free space.";
}

bool make_dirs(const fs::path &p, std::string &error)
{
    std::error_code ec;
    fs::create_directories(p, ec);
    if (fs::is_directory(p, ec)) return true;
    error = "Could not create the folder " + path_text(p) + ". Check that the location can be written to.";
    return false;
}

// Finds 'name' in 'dir' (exact first, then without case - Linux; Windows is case-insensitive anyway).
bool find_child(const fs::path &dir, const std::string &name, fs::path &out)
{
    std::error_code ec;
    fs::path p = dir / path_from_utf8(name);
    if (fs::exists(p, ec)) { out = p; return true; }
    fs::directory_iterator it(dir, ec), end;
    for (int n = 0; !ec && it != end && n < 100000; it.increment(ec), n++) {
        if (ieq(path_text(it->path().filename()), name)) { out = it->path(); return true; }
    }
    return false;
}

// ============================================================================================================
// package headers
// ============================================================================================================

bool is_package_magic(const uint8_t *m)
{
    return !std::memcmp(m, "CON ", 4) || !std::memcmp(m, "LIVE", 4) || !std::memcmp(m, "PIRS", 4);
}

const size_t kHeaderRead = 0x1800;   // everything read_package_info needs (title name ends at 0x1711)

bool parse_header(const uint8_t *h, size_t n, PackageInfo &info)
{
    info = PackageInfo();
    if (n < 0x3AD || !is_package_magic(h)) return false;
    info.magic.assign(reinterpret_cast<const char *>(h), 4);
    info.header_size = be32(h + 0x340);
    info.content_type = be32(h + 0x344);
    info.metadata_version = be32(h + 0x348);
    info.content_size = be64(h + 0x34C);
    info.media_id = be32(h + 0x354);
    info.version = be32(h + 0x358);
    info.base_version = be32(h + 0x35C);
    info.title_id = be32(h + 0x360);
    info.platform = h[0x364];
    info.executable_type = h[0x365];
    info.disc_number = h[0x366];
    info.disc_count = h[0x367];
    info.data_file_count = be32(h + 0x39D);
    info.data_files_size = be64(h + 0x3A1);
    info.descriptor_type = be32(h + 0x3A9);
    const uint8_t *vd = h + 0x379;
    if (info.descriptor_type == 1) {
        // SVOD: size, block cache count, worker processor, worker priority, root hash[20], flags,
        // data block count (u24 LE), data block offset (u24 LE), padding[5]
        info.svod_flags = vd[0x18];
        info.svod_data_blocks = le24(vd + 0x19);
        info.svod_data_block_offset = le24(vd + 0x1C);
    } else {
        // STFS: size, reserved, block separation, file table block count (u16 LE), file table block (u24 LE),
        // top hash table hash[20], allocated blocks (u32 BE), unallocated blocks (u32 BE)
        info.block_separation = vd[2];
        info.file_table_blocks = le16(vd + 3);
        info.file_table_start = le24(vd + 5);
        info.allocated_blocks = be32(vd + 0x1C);
        info.unallocated_blocks = be32(vd + 0x20);
    }
    if (n >= 0x411 + 0x80) info.display_name = utf16be_to_utf8(h + 0x411, 0x40);
    if (n >= 0x1691 + 0x80) info.title_name = utf16be_to_utf8(h + 0x1691, 0x40);
    return true;
}

bool load_header(InFile &f, PackageInfo &info)
{
    uint8_t h[kHeaderRead];
    size_t n = size_t(std::min<uint64_t>(f.size(), sizeof h));
    return f.read_at(0, h, n) && parse_header(h, n, info);
}

bool package_header(const fs::path &p, PackageInfo &info)
{
    InFile f;
    return f.open(p) && load_header(f, info);
}

// ============================================================================================================
// XDVDFS (disc images and the inside of Games on Demand copies)
// ============================================================================================================

const char kMediaMagic[] = "MICROSOFT*XBOX*MEDIA";   // 20 bytes
const uint32_t kSector = 2048;
const uint32_t kMaxDirTable = 64u << 20;   // no real folder table comes near this
const int kMaxDepth = 64;

// Where the file system's bytes come from. Offsets are bytes from the file system's sector 0.
class SectorSource {
public:
    virtual ~SectorSource() = default;
    virtual bool read(uint64_t off, void *buf, size_t n, std::string &error) = 0;
    // Is [off, off+n) present? Checked for every file before anything is written.
    virtual bool check(uint64_t off, uint64_t n, const std::string &file, std::string &error) = 0;
};

class IsoSource : public SectorSource {
public:
    IsoSource(InFile &f, uint64_t part) : f_(f), part_(part) {}
    bool read(uint64_t off, void *buf, size_t n, std::string &error) override
    {
        uint64_t at = part_ + off;
        if (at > f_.size() || n > f_.size() - at) { error = incomplete(at + n); return false; }
        if (!f_.read_at(at, buf, n)) {
            error = "Could not read the disc image near byte " + num(at) + ". The drive or file may be damaged.";
            return false;
        }
        return true;
    }
    bool check(uint64_t off, uint64_t n, const std::string &file, std::string &error) override
    {
        uint64_t end = part_ + off + n;
        if (end <= f_.size()) return true;
        error = "The disc image is incomplete: it ends at byte " + num(f_.size()) + ", but the game's file " + file +
                " needs bytes up to " + num(end) + ". It may have been cut short while it was copied or downloaded - copy or dump it again.";
        return false;
    }

private:
    std::string incomplete(uint64_t need) const
    {
        return "The disc image is incomplete: it ends at byte " + num(f_.size()) + ", but the game's data goes on to byte " +
               num(need) + ". It may have been cut short while it was copied or downloaded - copy or dump it again.";
    }
    InFile &f_;
    uint64_t part_;
};

struct Volume { uint32_t root_sector = 0, root_size = 0; };

bool parse_volume(const uint8_t vd[28], Volume &v)
{
    if (std::memcmp(vd, kMediaMagic, 20)) return false;
    v.root_sector = le32(vd + 20);
    v.root_size = le32(vd + 24);
    return true;
}

struct RawEntry {
    std::string name;
    uint32_t sector, size;
    uint8_t attr;
};

// One folder's table: a binary tree of entries (left, right in 4-byte units; sector; size; attributes; name).
bool read_dir_table(SectorSource &src, uint32_t sector, uint32_t size, std::vector<RawEntry> &out, std::string &error)
{
    out.clear();
    if (!size) return true;   // an empty folder
    if (size > kMaxDirTable) {
        error = "The game's file system is damaged near sector " + num(sector) + " (a folder table is far too large).";
        return false;
    }
    std::vector<uint8_t> t(size);
    if (!src.read(uint64_t(sector) * kSector, t.data(), size, error)) return false;
    std::vector<uint8_t> seen(size / 4 + 1, 0);
    std::vector<uint32_t> todo{ 0 };
    while (!todo.empty()) {
        uint32_t o = todo.back();
        todo.pop_back();
        if (uint64_t(o) + 14 > size || seen[o / 4]) continue;
        seen[o / 4] = 1;
        uint16_t l = le16(&t[o]), r = le16(&t[o + 2]);
        if (l == 0xFFFF) continue;   // padding (0xFF) up to the end of a sector
        unsigned n = t[o + 13];
        if (!n || uint64_t(o) + 14 + n > size) continue;
        out.push_back({ std::string(reinterpret_cast<const char *>(&t[o + 14]), n), le32(&t[o + 4]), le32(&t[o + 8]), t[o + 12] });
        if (l) todo.push_back(uint32_t(l) * 4);
        if (r) todo.push_back(uint32_t(r) * 4);
    }
    return true;
}

struct DiscItem {
    std::vector<std::string> parts;   // safe components
    std::string rel;
    uint32_t sector = 0, size = 0;
    bool dir = false;
};

bool walk_dir(SectorSource &src, uint32_t sector, uint32_t size, const std::vector<std::string> &parts, int depth,
              std::set<uint32_t> &seen, std::vector<DiscItem> &out, std::string &error)
{
    if (depth > kMaxDepth || out.size() > 4000000) {
        error = "The game's file system is damaged (its folders go too deep).";
        return false;
    }
    if (size && !seen.insert(sector).second) {
        error = "The game's file system is damaged near sector " + num(sector) + " (a folder refers back to another).";
        return false;
    }
    std::vector<RawEntry> es;
    if (!read_dir_table(src, sector, size, es, error)) return false;
    std::stable_sort(es.begin(), es.end(), [](const RawEntry &a, const RawEntry &b) { return lower(a.name) < lower(b.name); });
    std::set<std::string> names;
    for (const auto &e : es) {
        std::string safe;
        if (!safe_name(e.name, safe) || !names.insert(lower(safe)).second) continue;   // unsafe or duplicate name
        DiscItem it;
        it.parts = parts;
        it.parts.push_back(safe);
        it.rel = join(it.parts);
        it.sector = e.sector;
        it.size = e.size;
        it.dir = (e.attr & 0x10) != 0;
        out.push_back(it);
        if (it.dir && !walk_dir(src, e.sector, e.size, it.parts, depth + 1, seen, out, error)) return false;
    }
    return true;
}

bool walk_volume(SectorSource &src, const Volume &v, std::vector<DiscItem> &out, std::string &error)
{
    out.clear();
    std::set<uint32_t> seen;
    return walk_dir(src, v.root_sector, v.root_size, {}, 0, seen, out, error);
}

// a path in the file system ('/' or '\\'), without case
bool find_item(SectorSource &src, const Volume &v, const std::string &path, RawEntry &hit, std::string &error)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path + "/") {
        if (c == '/' || c == '\\') { if (!cur.empty()) parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (parts.empty()) return false;
    uint32_t sec = v.root_sector, size = v.root_size;
    bool is_dir = true;
    for (const auto &name : parts) {
        if (!is_dir) return false;
        std::vector<RawEntry> es;
        if (!read_dir_table(src, sec, size, es, error)) return false;
        auto it = std::find_if(es.begin(), es.end(), [&](const RawEntry &e) { return ieq(e.name, name); });
        if (it == es.end()) return false;
        hit = *it;
        sec = it->sector;
        size = it->size;
        is_dir = (it->attr & 0x10) != 0;
    }
    return true;
}

// The root must hold default.xex for this to be an Xbox 360 game.
bool check_game_root(SectorSource &src, const Volume &v, bool disc, std::string &error)
{
    RawEntry e;
    std::string err;
    if (find_item(src, v, "default.xex", e, err) && !(e.attr & 0x10)) return true;
    if (!err.empty()) { error = err; return false; }
    const char *what = disc ? "This disc image" : "This Games on Demand copy";
    if (find_item(src, v, "default.xbe", e, err))
        error = std::string(what) + " is an original Xbox game, not an Xbox 360 game.";
    else
        error = std::string(what) + (disc ? " is not an Xbox 360 game disc" : " is not an Xbox 360 game") + " (it has no default.xex).";
    return false;
}

bool read_xdvdfs_file(SectorSource &src, const Volume &v, const std::string &path, size_t max, std::vector<uint8_t> &out)
{
    RawEntry e;
    std::string err;
    if (!find_item(src, v, path, e, err) || (e.attr & 0x10)) return false;
    out.resize(std::min<size_t>(e.size, max));
    return src.read(uint64_t(e.sector) * kSector, out.data(), out.size(), err);
}

bool copy_range(SectorSource &src, uint64_t off, uint64_t size, const fs::path &dst, Progress &pr, std::string &error)
{
    OutFile o;
    if (!o.open(dst)) { error = cannot_write(dst); return false; }
    std::vector<uint8_t> buf(1u << 20);
    while (size) {
        if (pr.cancelled()) { error = kCancelled; return false; }
        size_t n = size_t(std::min<uint64_t>(size, buf.size()));
        if (!src.read(off, buf.data(), n, error)) return false;
        if (!o.write(buf.data(), n)) { error = cannot_write(dst); return false; }
        off += n;
        size -= n;
        pr.add(n);
    }
    if (!o.close()) { error = cannot_write(dst); return false; }
    return true;
}

bool extract_volume(SectorSource &src, const Volume &v, bool disc, const fs::path &out_dir, ProgressFn progress,
                    CancelFlag cancel, std::string &error)
{
    if (!check_game_root(src, v, disc, error)) return false;
    std::vector<DiscItem> items;
    if (!walk_volume(src, v, items, error)) return false;
    Progress pr;
    pr.fn = std::move(progress);
    pr.cancel = cancel;
    // every file must be there before anything is written
    for (const auto &it : items) {
        if (it.dir) continue;
        if (!src.check(uint64_t(it.sector) * kSector, it.size, it.rel, error)) return false;
        pr.total += it.size;
    }
    if (!make_dirs(out_dir, error)) return false;
    for (const auto &it : items) {
        if (pr.cancelled()) { error = kCancelled; return false; }
        fs::path dst = out_path(out_dir, it.parts);
        if (it.dir) { if (!make_dirs(dst, error)) return false; continue; }
        pr.file(it.rel);
        if (!copy_range(src, uint64_t(it.sector) * kSector, it.size, dst, pr, error)) {
            if (error != kCancelled && error.find(it.rel) == std::string::npos) error = "Copying " + it.rel + ": " + error;
            return false;
        }
    }
    pr.finish();
    return true;
}

void items_to_entries(const std::vector<DiscItem> &items, std::vector<Entry> &out)
{
    out.clear();
    for (const auto &it : items) out.push_back({ it.rel, it.dir ? 0 : it.size, it.dir });
}

// ---- disc images -------------------------------------------------------------------------------------------

// game partition offsets: rebuilt image (game partition only), XGD2, XGD3, XGD1
const uint64_t kPartitions[] = { 0, 0xFD90000ull, 0x2080000ull, 0x18300000ull };

int64_t find_partition(InFile &f)
{
    for (uint64_t p : kPartitions) {
        uint8_t vd[28];
        if (!f.read_at(p + 32 * kSector, vd, sizeof vd)) continue;
        Volume v;
        if (parse_volume(vd, v)) return int64_t(p);
    }
    return -1;
}

bool open_disc(const fs::path &iso, InFile &f, uint64_t &part, Volume &v, std::string &error)
{
    if (!f.open(iso)) {
        error = "Could not open the disc image " + path_text(iso) + ". Check that it is still there and that no other program is using it.";
        return false;
    }
    int64_t p = find_partition(f);
    if (p < 0) {
        error = "This disc image is not an Xbox 360 game disc (no Xbox file system was found in it).";
        return false;
    }
    part = uint64_t(p);
    uint8_t vd[28];
    f.read_at(part + 32 * kSector, vd, sizeof vd);
    parse_volume(vd, v);
    return true;
}

// ============================================================================================================
// STFS
// ============================================================================================================
//
// Block numbering (as tools\stfs_extract.ps1): data block b lives at
//   base + 4096 * (b + tables * (1 + b/170 + [b >= 170](1 + b/28900) + [b >= 28900](1 + b/4913000)))
// with base = header size rounded up to 4096 and tables = 1 for read-only ("female", block separation bit 0
// set) packages, 2 otherwise ("male": two copies of every hash table, one of them current).
//
// Hash tables: 170 entries of 24 bytes (SHA-1 of the block, status byte, next block u24 BE). Level 0 covers
// 170 blocks, level 1 covers 170 level-0 tables (28,900 blocks), level 2 covers 170 level-1 tables. Where
// each table sits and which copy is current in a male package follows the public descriptions (Velocity /
// "Free60"); title updates are female, so the PowerShell extractor never needed the "current copy" rule, and
// it is only used here for chained files and the hash check (which accepts either copy).

const uint32_t kBlock = 0x1000;
const uint32_t kPer0 = 170, kPer1 = 28900, kPer2 = 4913000;
const uint32_t kNoBlock = 0xFFFFFF;
const char *const kAgain = "Copy or download it again.";

struct StfsEntry {
    bool used = false, dir = false, contiguous = false;
    std::string name;
    uint32_t blocks = 0, start = 0, size = 0;
    int parent = -1;
};

class Stfs {
public:
    InFile f;
    PackageInfo info;
    uint64_t base = 0;
    uint32_t tables = 1;
    int top_level = 0;
    bool verify = true;
    std::vector<StfsEntry> entries;   // one per file table slot (64 per block), used or not

    bool open(const fs::path &p, std::string &error)
    {
        if (!f.open(p)) {
            error = "Could not open the package " + path_text(p) + ". Check that it is still there and that no other program is using it.";
            return false;
        }
        if (!load_header(f, info)) {
            error = "This file is not an Xbox 360 title update or download package.";
            return false;
        }
        if (info.is_svod()) {
            error = "This is a Games on Demand copy of a game, not a title update. Add it as the game instead.";
            return false;
        }
        base = (uint64_t(info.header_size) + 0xFFF) & ~uint64_t(0xFFF);
        tables = (info.block_separation & 1) ? 1 : 2;
        top_level = info.allocated_blocks <= kPer0 ? 0 : info.allocated_blocks <= kPer1 ? 1 : 2;
        // the file table: as the descriptor says first, then the other table count (the PowerShell tries 1, 2),
        // each by following the block chain, then as consecutive blocks
        uint32_t order[2] = { tables, tables == 1 ? 2u : 1u };
        for (uint32_t t : order) {
            tables = t;
            for (int chained = 1; chained >= 0; chained--) {
                if (read_file_table(chained != 0) && table_looks_right()) return verify_file_table(chained != 0, error);
            }
        }
        error = "The package is damaged: its list of files could not be read.";
        return false;
    }

    // ---- block positions ----
    uint64_t data_block(uint32_t b) const
    {
        uint64_t blk = b + uint64_t((b + kPer0) / kPer0) * tables;
        if (b >= kPer0) {
            blk += uint64_t((uint64_t(b) + kPer1) / kPer1) * tables;
            if (b >= kPer1) blk += uint64_t((uint64_t(b) + kPer2) / kPer2) * tables;
        }
        return blk;
    }
    uint64_t data_offset(uint32_t b) const { return base + (data_block(b) << 12); }
    uint64_t step0() const { return kPer0 + tables; }            // 0xAB / 0xAC
    uint64_t step1() const { return kPer0 * step0() + tables; }  // 0x718F / 0x723A
    uint64_t level0_block(uint32_t b) const
    {
        if (b < kPer0) return 0;
        uint64_t n = uint64_t(b / kPer0) * step0() + uint64_t(b / kPer1 + 1) * tables;
        if (b / kPer1) n += tables;
        return n;
    }
    uint64_t level1_block(uint32_t b) const
    {
        if (b < kPer1) return step0();
        return tables + uint64_t(b / kPer1) * step1();
    }
    uint64_t table_offset(uint64_t blk, int copy) const { return base + (blk << 12) + uint64_t(copy) * kBlock; }

    // which copy of block b's level-0 table is current (male packages only)
    int current_level0_copy(uint32_t b)
    {
        if (tables == 1) return 0;
        int top = (info.block_separation & 2) ? 1 : 0;
        if (top_level == 0) return top;
        uint8_t st = 0;
        if (top_level == 1) {
            f.read_at(table_offset(step0(), top) + uint64_t(b / kPer0) * 24 + 0x14, &st, 1);
            return (st & 0x40) ? 1 : 0;
        }
        f.read_at(table_offset(step1(), top) + uint64_t(b / kPer1) * 24 + 0x14, &st, 1);
        int l1 = (st & 0x40) ? 1 : 0;
        st = 0;
        f.read_at(table_offset(level1_block(b), l1) + uint64_t((b % kPer1) / kPer0) * 24 + 0x14, &st, 1);
        return (st & 0x40) ? 1 : 0;
    }

    // the level-0 table holding block b (both copies), cached
    struct Table {
        uint64_t blk = UINT64_MAX;
        int current = 0;
        uint8_t copy[2][kBlock];
        bool have[2] = { false, false };
    } cache;
    const Table &table_for(uint32_t b)
    {
        uint64_t blk = level0_block(b);
        if (cache.blk != blk) {
            cache.blk = blk;
            cache.current = current_level0_copy(b);
            for (uint32_t c = 0; c < 2; c++) {
                cache.have[c] = c < tables && f.read_at(table_offset(blk, int(c)), cache.copy[c], kBlock);
                if (!cache.have[c]) std::memset(cache.copy[c], 0, kBlock);
            }
        }
        return cache;
    }
    uint32_t next_block(uint32_t b)
    {
        const Table &t = table_for(b);
        if (!t.have[t.current]) return kNoBlock;
        return be24(t.copy[t.current] + (b % kPer0) * 24 + 0x15);
    }

    // ---- reading ----
    std::string incomplete(uint64_t need) const
    {
        return "The package is incomplete: it ends at byte " + num(f.size()) + ", but its data goes on to byte " + num(need);
    }
    bool read_block(uint32_t b, uint8_t *buf, size_t need, std::string &error)
    {
        uint64_t off = data_offset(b);
        if (off + need > f.size()) { error = incomplete(off + need); return false; }
        size_t n = size_t(std::min<uint64_t>(kBlock, f.size() - off));
        if (!f.read_at(off, buf, n)) { error = "Could not read the package near byte " + num(off); return false; }
        if (verify && n == kBlock && !block_hash_ok(b, buf)) {
            error = "The package is damaged near byte " + num(off);
            return false;
        }
        return true;
    }
    bool block_hash_ok(uint32_t b, const uint8_t *data)
    {
        const Table &t = table_for(b);
        uint8_t h[20];
        sha1(data, kBlock, h);
        bool any = false;
        for (uint32_t c = 0; c < tables; c++) {
            if (!t.have[c]) continue;
            const uint8_t *e = t.copy[c] + (b % kPer0) * 24;
            bool zero = std::all_of(e, e + 20, [](uint8_t x) { return x == 0; });
            if (zero) continue;
            any = true;
            if (!std::memcmp(e, h, 20)) return true;
        }
        return !any;   // no stored hash to check against
    }

    // the blocks of a run of 'count' blocks from 'start'
    bool block_list(uint32_t start, uint32_t count, bool contiguous, std::vector<uint32_t> &out)
    {
        // (a chain pointing at garbage is caught by the bounds and hash checks when the blocks are read)
        out.clear();
        uint64_t b = start;
        for (uint32_t i = 0; i < count; i++) {
            if (contiguous) b = uint64_t(start) + i;
            else if (i) b = next_block(uint32_t(b));
            if (b >= kNoBlock) return false;
            out.push_back(uint32_t(b));
        }
        return true;
    }

    bool read_file_table(bool chained)
    {
        entries.clear();
        cache.blk = UINT64_MAX;
        std::vector<uint32_t> blocks;
        if (!block_list(info.file_table_start, info.file_table_blocks, !chained, blocks)) return false;
        uint8_t buf[kBlock];
        for (uint32_t b : blocks) {
            uint64_t off = data_offset(b);
            if (off >= f.size()) break;
            size_t n = size_t(std::min<uint64_t>(kBlock, f.size() - off));
            std::memset(buf, 0, sizeof buf);
            if (!f.read_at(off, buf, n)) return false;
            for (size_t k = 0; k < 64 && (k + 1) * 64 <= n; k++) {
                const uint8_t *e = buf + k * 64;
                StfsEntry s;
                unsigned nl = e[0x28] & 0x3F;
                if (nl) {
                    s.used = true;
                    s.name.assign(reinterpret_cast<const char *>(e), nl);
                    s.dir = (e[0x28] & 0x80) != 0;
                    s.contiguous = (e[0x28] & 0x40) != 0;
                    s.blocks = le24(e + 0x29);
                    s.start = le24(e + 0x2F);
                    s.parent = int16_t(be16(e + 0x32));   // -1 (0xFFFF) = the root
                    s.size = be32(e + 0x34);
                }
                entries.push_back(s);
            }
        }
        return true;
    }
    // the PowerShell's test: at least one entry, every name printable ASCII
    bool table_looks_right() const
    {
        bool any = false;
        for (const auto &e : entries) {
            if (!e.used) continue;
            any = true;
            for (unsigned char c : e.name) if (c < 0x20 || c > 0x7E) return false;
        }
        return any;
    }
    bool verify_file_table(bool chained, std::string &error)
    {
        if (!verify) return true;
        std::vector<uint32_t> blocks;
        block_list(info.file_table_start, info.file_table_blocks, !chained, blocks);
        uint8_t buf[kBlock];
        for (uint32_t b : blocks) {
            if (data_offset(b) >= f.size()) { error = incomplete(data_offset(b) + kBlock) + ". " + kAgain; return false; }
            if (!read_block(b, buf, 0, error)) { error += " (in its list of files). " + std::string(kAgain); return false; }
        }
        return true;
    }

    // safe path components of entry i, or false (unsafe name, or a broken parent chain)
    bool parts_of(size_t i, std::vector<std::string> &parts) const
    {
        parts.clear();
        int cur = int(i);
        for (int depth = 0; cur >= 0 && size_t(cur) < entries.size() && entries[size_t(cur)].used; depth++) {
            if (depth > kMaxDepth) return false;
            std::string safe;
            if (!safe_name(entries[size_t(cur)].name, safe)) return false;
            parts.push_back(safe);
            cur = entries[size_t(cur)].parent;
        }
        std::reverse(parts.begin(), parts.end());
        return !parts.empty();
    }
};

// ============================================================================================================
// SVOD (Games on Demand)
// ============================================================================================================
//
// Each Data#### file: one level-1 ("master") hash block (4 KB), then up to 203 groups of one level-0 hash block
// (4 KB) and 0xCC data blocks of 4 KB (= 408 file-system sectors of 2 KB). A data file is at most 0xA290000
// bytes. The XDVDFS volume descriptor is read straight from Data0000 at 0x2000 ("enhanced GDF" layout, header
// flag 0x40) or 0x12000 (other GoD packages: a dummy "XSF" header fills the sectors before it); sector numbers
// in the file system are then shifted by 2 * the header's data block offset (and by -2 for enhanced GDF).
// This follows Xenia's SVOD reader (UNSURE: not checked against a real Gears GoD copy here). The hash blocks
// are not checked. The single-file layout (data inside the header file) is not supported.

const uint64_t kSvodSectorsPerGroup = 0x198;     // 408
const uint64_t kSvodSectorsPerFile = 0x14388;    // 82824 = 203 * 408
const uint64_t kSvodMaxFile = 0xA290000;

class SvodSource : public SectorSource {
public:
    std::map<uint32_t, fs::path> files;   // Data#### number -> path
    std::map<uint32_t, std::unique_ptr<InFile>> open_files;
    uint32_t block_offset = 0;
    bool egdf = false;

    bool map(uint64_t sector, uint32_t &file, uint64_t &addr, uint64_t &run) const
    {
        uint64_t shift = uint64_t(block_offset) * 2, adj = egdf ? 2 : 0;
        if (sector + adj < shift) return false;
        uint64_t tb = sector + adj - shift;
        uint64_t fb = tb % kSvodSectorsPerFile, fi = tb / kSvodSectorsPerFile;
        uint64_t l0 = fb / kSvodSectorsPerGroup + 1;
        uint64_t l1 = l0 / 0xA1C4 + 1;
        addr = fb * kSector + l0 * kBlock + l1 * kBlock;
        if (addr >= kSvodMaxFile) { fi++; addr %= kSvodMaxFile; addr += 0x2000; }
        run = kSvodSectorsPerGroup - fb % kSvodSectorsPerGroup;
        if (fi > 0xFFFFFFFFull) return false;
        file = uint32_t(fi);
        return true;
    }
    static std::string data_name(uint32_t i)
    {
        char b[16];
        std::snprintf(b, sizeof b, "Data%04u", i);
        return b;
    }
    InFile *get(uint32_t i, std::string &error)
    {
        auto it = open_files.find(i);
        if (it != open_files.end()) return it->second.get();
        auto p = files.find(i);
        if (p == files.end()) {
            error = "The Games on Demand copy is incomplete: " + data_name(i) + " is missing from its .data folder.";
            return nullptr;
        }
        std::unique_ptr<InFile> f(new InFile);
        if (!f->open(p->second)) {
            error = "Could not open " + path_text(p->second) + ". Check that no other program is using it.";
            return nullptr;
        }
        InFile *raw = f.get();
        open_files[i] = std::move(f);
        return raw;
    }
    bool read(uint64_t off, void *buf, size_t n, std::string &error) override
    {
        uint8_t *out = static_cast<uint8_t *>(buf);
        while (n) {
            uint64_t sector = off / kSector, within = off % kSector;
            uint32_t fi;
            uint64_t addr, run;
            if (!map(sector, fi, addr, run)) {
                error = "The game's file system is damaged (it points before the start of the data, sector " + num(sector) + ").";
                return false;
            }
            size_t take = size_t(std::min<uint64_t>(n, run * kSector - within));
            InFile *f = get(fi, error);
            if (!f) return false;
            if (addr + within + take > f->size()) {
                error = "The Games on Demand copy is incomplete or damaged: " + data_name(fi) + " ends at byte " + num(f->size()) +
                        ", but data is needed up to byte " + num(addr + within + take) + ". Copy it again.";
                return false;
            }
            if (!f->read_at(addr + within, out, take)) {
                error = "Could not read " + data_name(fi) + " near byte " + num(addr + within) + ".";
                return false;
            }
            out += take;
            off += take;
            n -= take;
        }
        return true;
    }
    bool check(uint64_t off, uint64_t n, const std::string &file, std::string &error) override
    {
        if (!n) return true;
        // the first and last sector of the range (every data file in between must exist too)
        uint32_t f0, f1;
        uint64_t a0, a1, run;
        uint64_t last = off + n - 1;
        if (!map(off / kSector, f0, a0, run) || !map(last / kSector, f1, a1, run)) {
            error = "The game's file system is damaged (the file " + file + " points outside the data).";
            return false;
        }
        for (uint32_t i = f0; i <= f1; i++)
            if (!get(i, error)) return false;
        InFile *f = get(f1, error);
        uint64_t need = a1 + last % kSector + 1;
        if (need > f->size()) {
            error = "The Games on Demand copy is incomplete or damaged: " + data_name(f1) + " ends at byte " + num(f->size()) +
                    ", but the game's file " + file + " needs bytes up to " + num(need) + ". Copy it again.";
            return false;
        }
        return true;
    }
};

fs::path data_folder_of(const fs::path &header)
{
    fs::path d = header;
    d += ".data";
    std::error_code ec;
    if (fs::is_directory(d, ec)) return d;
    fs::path alt;
    if (find_child(header.parent_path(), path_text(header.filename()) + ".data", alt) && fs::is_directory(alt, ec)) return alt;
    return d;
}

bool open_god(const fs::path &header, SvodSource &src, Volume &v, std::string &error)
{
    PackageInfo info;
    InFile hf;
    if (!hf.open(header)) {
        error = "Could not open the Games on Demand file " + path_text(header) + ".";
        return false;
    }
    if (!load_header(hf, info)) {
        error = "This file is not an Xbox 360 Games on Demand package.";
        return false;
    }
    if (!info.is_svod()) {
        error = "This package is not a Games on Demand copy of a game (it may be a title update or a download).";
        return false;
    }
    fs::path dd = data_folder_of(header);
    std::error_code ec;
    fs::directory_iterator it(dd, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::string n = lower(path_text(it->path().filename()));
        if (n.size() == 8 && n.compare(0, 4, "data") == 0 && std::all_of(n.begin() + 4, n.end(), [](char c) { return c >= '0' && c <= '9'; }))
            src.files[uint32_t(std::stoul(n.substr(4)))] = it->path();
    }
    if (src.files.empty() || !src.files.count(0)) {
        uint8_t m[20];
        if (hf.read_at(0xD000, m, 20) && !std::memcmp(m, kMediaMagic, 20))
            error = "This Games on Demand copy keeps its data inside the header file, which COGBox does not support yet.";
        else
            error = "The Games on Demand copy is incomplete: its data files (" + path_text(dd.filename()) + "\\Data0000 ...) are missing.";
        return false;
    }
    src.block_offset = info.svod_data_block_offset;
    InFile *d0 = src.get(0, error);
    if (!d0) return false;
    bool flag = (info.svod_flags & 0x40) != 0;
    struct Try { uint64_t at; bool egdf; } tries[2] = { { 0x2000, true }, { 0x12000, false } };
    if (!flag) std::swap(tries[0], tries[1]);
    for (const auto &t : tries) {
        uint8_t vd[28];
        if (d0->read_at(t.at, vd, sizeof vd) && parse_volume(vd, v)) {
            src.egdf = t.egdf;
            return true;
        }
    }
    error = "This Games on Demand copy could not be read: the game's file system was not found in Data0000. "
            "The copy may be damaged, or it uses a layout COGBox does not support yet.";
    return false;
}

// ---- detect helpers ----------------------------------------------------------------------------------------

bool looks_like_data_file(const fs::path &p)
{
    std::string n = lower(path_text(p.filename()));
    std::string dir = lower(path_text(p.parent_path().filename()));
    return n.size() == 8 && n.compare(0, 4, "data") == 0 && dir.size() > 5 && dir.compare(dir.size() - 5, 5, ".data") == 0;
}

// the SVOD header for a "<name>.data" folder
bool header_for_data_folder(const fs::path &dataDir, fs::path &header)
{
    std::string n = path_text(dataDir.filename());
    if (n.size() <= 5 || lower(n.substr(n.size() - 5)) != ".data") return false;
    fs::path h;
    PackageInfo info;
    if (find_child(dataDir.parent_path(), n.substr(0, n.size() - 5), h) && package_header(h, info) && info.is_svod()) {
        header = h;
        return true;
    }
    return false;
}

// packages in 'dir' (files only, sorted by name, at most a few hundred looked at)
std::vector<fs::path> files_in(const fs::path &dir)
{
    std::vector<fs::path> out;
    std::error_code ec;
    fs::directory_iterator it(dir, ec), end;
    for (; !ec && it != end && out.size() < 500; it.increment(ec))
        if (it->is_regular_file(ec)) out.push_back(it->path());
    std::sort(out.begin(), out.end());
    return out;
}
std::vector<fs::path> subdirs_of(const fs::path &dir)
{
    std::vector<fs::path> out;
    std::error_code ec;
    fs::directory_iterator it(dir, ec), end;
    for (; !ec && it != end && out.size() < 500; it.increment(ec))
        if (it->is_directory(ec)) out.push_back(it->path());
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

// ============================================================================================================
// public API
// ============================================================================================================

const char *source_kind_name(SourceKind k)
{
    switch (k) {
    case SourceKind::ExtractedFolder: return "game folder";
    case SourceKind::DiscImage:       return "disc image";
    case SourceKind::GodContainer:    return "Games on Demand copy";
    case SourceKind::StfsPackage:     return "Xbox 360 package";
    default:                          return "unknown";
    }
}

std::string xbox_version_string(uint32_t v)
{
    return num((v >> 28) & 0xF) + "." + num((v >> 24) & 0xF) + "." + num((v >> 8) & 0xFFFF) + "." + num(v & 0xFF);
}
std::string PackageInfo::version_string() const { return xbox_version_string(version); }

bool read_package_info(const fs::path &file, PackageInfo &info, std::string &error)
{
    InFile f;
    if (!f.open(file)) {
        error = "Could not open " + path_text(file) + ".";
        return false;
    }
    if (!load_header(f, info)) {
        error = "This file is not an Xbox 360 package.";
        return false;
    }
    return true;
}

bool parse_xex_info(const uint8_t *d, size_t n, XexInfo &info)
{
    info = XexInfo();
    if (n < 0x18 || std::memcmp(d, "XEX2", 4)) return false;
    uint32_t count = be32(d + 0x14);
    for (uint32_t i = 0; i < count && i < 4096; i++) {
        size_t o = 0x18 + size_t(i) * 8;
        if (o + 8 > n) break;
        if (be32(d + o) != 0x00040006) continue;   // execution info: media id, version, base version, title id, ...
        uint32_t at = be32(d + o + 4);
        if (uint64_t(at) + 24 > n) return false;
        const uint8_t *e = d + at;
        info.media_id = be32(e);
        info.version = be32(e + 4);
        info.base_version = be32(e + 8);
        info.title_id = be32(e + 12);
        info.disc_number = e[18];
        info.disc_count = e[19];
        return true;
    }
    return false;
}

bool read_xex_info(const fs::path &xex, XexInfo &info)
{
    InFile f;
    if (!f.open(xex)) return false;
    std::vector<uint8_t> b(size_t(std::min<uint64_t>(f.size(), 1u << 20)));
    return f.read_at(0, b.data(), b.size()) && parse_xex_info(b.data(), b.size(), info);
}

SourceKind detect(const fs::path &p, fs::path *resolved)
{
    auto found = [&](SourceKind k, const fs::path &r) {
        if (resolved) *resolved = r;
        return k;
    };
    std::error_code ec;
    fs::file_status st = fs::status(p, ec);
    if (ec || !fs::exists(st)) return SourceKind::Unknown;
    fs::path x;
    PackageInfo info;

    if (fs::is_directory(st)) {
        if (find_child(p, "default.xex", x) && fs::is_regular_file(x, ec)) return found(SourceKind::ExtractedFolder, p);
        if (header_for_data_folder(p, x)) return found(SourceKind::GodContainer, x);
        // Games on Demand: <titleid>\00007000\<header>, picked at any of those levels
        std::vector<fs::path> dirs{ p };
        if (find_child(p, "00007000", x)) dirs.push_back(x);
        for (const auto &s : subdirs_of(p)) if (find_child(s, "00007000", x)) dirs.push_back(x);
        for (const auto &d : dirs)
            for (const auto &f : files_in(d))
                if (package_header(f, info) && info.is_svod()) return found(SourceKind::GodContainer, f);
        // title updates: <titleid>\000B0000\<package> (or the package next to it): the newest one
        std::vector<fs::path> tu{ p };
        if (find_child(p, "000B0000", x)) tu.push_back(x);
        for (const auto &s : subdirs_of(p)) if (find_child(s, "000B0000", x)) tu.push_back(x);
        fs::path best;
        uint32_t bestVer = 0;
        for (const auto &d : tu)
            for (const auto &f : files_in(d))
                if (package_header(f, info) && !info.is_svod() && info.is_title_update() && (best.empty() || info.version > bestVer)) {
                    best = f;
                    bestVer = info.version;
                }
        if (!best.empty()) return found(SourceKind::StfsPackage, best);
        return SourceKind::Unknown;
    }

    if (ieq(path_text(p.filename()), "default.xex")) return found(SourceKind::ExtractedFolder, p.parent_path());
    if (package_header(p, info)) return found(info.is_svod() ? SourceKind::GodContainer : SourceKind::StfsPackage, p);
    if (looks_like_data_file(p) && header_for_data_folder(p.parent_path(), x)) return found(SourceKind::GodContainer, x);
    if (disc_partition_offset(p) >= 0) return found(SourceKind::DiscImage, p);
    return SourceKind::Unknown;
}

// ---- disc images ----

int64_t disc_partition_offset(const fs::path &iso)
{
    InFile f;
    return f.open(iso) ? find_partition(f) : -1;
}

bool disc_has_file(const fs::path &iso, const std::string &name)
{
    InFile f;
    uint64_t part;
    Volume v;
    std::string err;
    if (!open_disc(iso, f, part, v, err)) return false;
    IsoSource src(f, part);
    RawEntry e;
    return find_item(src, v, name, e, err) && !(e.attr & 0x10);
}

bool list_disc(const fs::path &iso, std::vector<Entry> &out, std::string &error)
{
    out.clear();
    InFile f;
    uint64_t part;
    Volume v;
    if (!open_disc(iso, f, part, v, error)) return false;
    IsoSource src(f, part);
    std::vector<DiscItem> items;
    if (!walk_volume(src, v, items, error)) return false;
    items_to_entries(items, out);
    return true;
}

bool disc_xex_info(const fs::path &iso, XexInfo &info)
{
    InFile f;
    uint64_t part;
    Volume v;
    std::string err;
    if (!open_disc(iso, f, part, v, err)) return false;
    IsoSource src(f, part);
    std::vector<uint8_t> b;
    return read_xdvdfs_file(src, v, "default.xex", 1u << 20, b) && parse_xex_info(b.data(), b.size(), info);
}

bool extract_disc(const fs::path &iso, const fs::path &out_dir, ProgressFn progress, CancelFlag cancel, std::string &error)
{
    error.clear();
    InFile f;
    uint64_t part;
    Volume v;
    if (!open_disc(iso, f, part, v, error)) return false;
    IsoSource src(f, part);
    return extract_volume(src, v, true, out_dir, std::move(progress), cancel, error);
}

// ---- STFS ----

bool list_stfs(const fs::path &package, std::vector<Entry> &out, std::string &error)
{
    out.clear();
    Stfs s;
    s.verify = false;
    if (!s.open(package, error)) return false;
    std::vector<std::string> parts;
    for (size_t i = 0; i < s.entries.size(); i++) {
        const auto &e = s.entries[i];
        if (!e.used || !s.parts_of(i, parts)) continue;
        out.push_back({ join(parts), e.dir ? 0 : e.size, e.dir });
    }
    return true;
}

bool extract_stfs(const fs::path &package, const fs::path &out_dir, ProgressFn progress, CancelFlag cancel, std::string &error,
                  bool verify_hashes)
{
    error.clear();
    Stfs s;
    s.verify = verify_hashes;
    if (!s.open(package, error)) return false;
    Progress pr;
    pr.fn = std::move(progress);
    pr.cancel = cancel;
    struct Job { size_t index; std::vector<std::string> parts; std::vector<uint32_t> blocks; };
    std::vector<Job> jobs;
    for (size_t i = 0; i < s.entries.size(); i++) {
        const auto &e = s.entries[i];
        Job j{ i, {}, {} };
        if (!e.used || !s.parts_of(i, j.parts)) continue;
        if (!e.dir) {
            uint32_t need = uint32_t((uint64_t(e.size) + kBlock - 1) / kBlock);
            if (!s.block_list(e.start, need, e.contiguous, j.blocks)) {
                error = "The package is damaged: the file " + join(j.parts) + " could not be followed through it.";
                return false;
            }
            if (need) {
                uint64_t end = s.data_offset(j.blocks.back()) + (e.size - uint64_t(need - 1) * kBlock);
                if (end > s.f.size()) { error = s.incomplete(end) + " (in " + join(j.parts) + "). " + kAgain; return false; }
            }
            pr.total += e.size;
        }
        jobs.push_back(std::move(j));
    }
    if (!make_dirs(out_dir, error)) return false;
    std::vector<uint8_t> buf(kBlock);
    for (const auto &j : jobs) {
        if (pr.cancelled()) { error = kCancelled; return false; }
        const auto &e = s.entries[j.index];
        fs::path dst = out_path(out_dir, j.parts);
        if (e.dir) { if (!make_dirs(dst, error)) return false; continue; }
        if (!make_dirs(dst.parent_path(), error)) return false;
        std::string rel = join(j.parts);
        pr.file(rel);
        OutFile o;
        if (!o.open(dst)) { error = cannot_write(dst); return false; }
        uint64_t left = e.size;
        for (uint32_t b : j.blocks) {
            if (pr.cancelled()) { error = kCancelled; return false; }
            size_t n = size_t(std::min<uint64_t>(left, kBlock));
            if (!s.read_block(b, buf.data(), n, error)) { error += " (in " + rel + "). " + kAgain; return false; }
            if (!o.write(buf.data(), n)) { error = cannot_write(dst); return false; }
            left -= n;
            pr.add(n);
        }
        if (!o.close()) { error = cannot_write(dst); return false; }
    }
    pr.finish();
    return true;
}

// ---- Games on Demand ----

bool list_god(const fs::path &header, std::vector<Entry> &out, std::string &error)
{
    out.clear();
    SvodSource src;
    Volume v;
    if (!open_god(header, src, v, error)) return false;
    std::vector<DiscItem> items;
    if (!walk_volume(src, v, items, error)) return false;
    items_to_entries(items, out);
    return true;
}

bool extract_god(const fs::path &header, const fs::path &out_dir, ProgressFn progress, CancelFlag cancel, std::string &error)
{
    error.clear();
    SvodSource src;
    Volume v;
    if (!open_god(header, src, v, error)) return false;
    return extract_volume(src, v, false, out_dir, std::move(progress), cancel, error);
}

// ---- SHA-1 ----

std::string sha1_hex(const void *data, size_t size)
{
    uint8_t h[20];
    sha1(data, size, h);
    return hex(h, 20);
}

std::string sha1_file(const fs::path &file)
{
    InFile f;
    if (!f.open(file)) return std::string();
    Sha1 s;
    std::vector<uint8_t> buf(1u << 20);
    uint64_t off = 0, left = f.size();
    while (left) {
        size_t n = size_t(std::min<uint64_t>(left, buf.size()));
        if (!f.read_at(off, buf.data(), n)) return std::string();
        s.update(buf.data(), n);
        off += n;
        left -= n;
    }
    uint8_t h[20];
    s.final(h);
    return hex(h, 20);
}

// Job 647: the XEX2 header's word at offset 8 is where the image (PE data) starts; everything before it is the header
// (region, media ID, keys, signature), which region discs change while the program stays the same.
std::string xex_code_sha1(const fs::path &file)
{
    InFile f;
    if (!f.open(file)) return std::string();
    uint8_t h8[12];
    if (f.size() < 12 || !f.read_at(0, h8, 12) || memcmp(h8, "XEX2", 4) != 0) return std::string();
    const uint64_t pe = (uint64_t(h8[8]) << 24) | (uint64_t(h8[9]) << 16) | (uint64_t(h8[10]) << 8) | h8[11];
    if (pe < 24 || pe >= f.size()) return std::string();
    Sha1 s;
    std::vector<uint8_t> buf(1u << 20);
    uint64_t off = pe, left = f.size() - pe;
    while (left) {
        size_t n = size_t(std::min<uint64_t>(left, buf.size()));
        if (!f.read_at(off, buf.data(), n)) return std::string();
        s.update(buf.data(), n);
        off += n;
        left -= n;
    }
    uint8_t h[20];
    s.final(h);
    return hex(h, 20);
}

} // namespace x360fs
