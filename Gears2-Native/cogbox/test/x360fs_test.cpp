// CogBox x360fs tests: builds synthetic XDVDFS images, STFS packages and SVOD (Games on Demand) copies and
// checks the readers against them. The builders lay out the formats from their own (independent) description
// of the layout, not from the reader's formulas.
//   g++ -std=c++17 -O2 -Wall -Wextra -I../src x360fs_test.cpp ../src/x360fs.cpp -o x360fs_test
//   ./x360fs_test [work_dir]      (work_dir is emptied first; packages for the PowerShell-port oracle stay there)
#include "x360fs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

using namespace x360fs;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_MSG(c, m) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, #c, std::string(m).c_str()); } } while (0)

// ---- helpers ----------------------------------------------------------------------------------------------

static uint32_t g_seed = 12345;
static uint32_t rnd() { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }
static Bytes random_bytes(size_t n) { Bytes b(n); for (auto &x : b) x = uint8_t(rnd()); return b; }

static void put_le16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
static void put_le24(uint8_t *p, uint32_t v) { put_le16(p, v); p[2] = uint8_t(v >> 16); }
static void put_le32(uint8_t *p, uint32_t v) { put_le24(p, v); p[3] = uint8_t(v >> 24); }
static void put_be16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
static void put_be24(uint8_t *p, uint32_t v) { p[0] = uint8_t(v >> 16); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v); }
static void put_be32(uint8_t *p, uint32_t v) { put_be16(p, v >> 16); put_be16(p + 2, v); }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, uint32_t(v >> 32)); put_be32(p + 4, uint32_t(v)); }
static void put_utf16be(uint8_t *p, const std::string &s) { for (size_t i = 0; i < s.size(); i++) put_be16(p + i * 2, uint8_t(s[i])); }

static void sha1_raw(const uint8_t *d, size_t n, uint8_t out[20])
{
    std::string h = sha1_hex(d, n);
    for (int i = 0; i < 20; i++) out[i] = uint8_t(std::stoi(h.substr(size_t(i) * 2, 2), nullptr, 16));
}

// sparse file: chunks at offsets, final length
static void write_sparse(const fs::path &p, const std::map<uint64_t, Bytes> &chunks, uint64_t length)
{
    fs::create_directories(p.parent_path());
    { std::ofstream o(p, std::ios::binary | std::ios::trunc); }
    std::fstream o(p, std::ios::binary | std::ios::in | std::ios::out);
    for (const auto &c : chunks) {
        o.seekp(std::streamoff(c.first));
        o.write(reinterpret_cast<const char *>(c.second.data()), std::streamsize(c.second.size()));
    }
    o.close();
    fs::resize_file(p, length);
}
static void write_file(const fs::path &p, const Bytes &b) { write_sparse(p, { { 0, b } }, b.size()); }

// all regular files under dir: relative path ('/') -> sha1
static std::map<std::string, std::string> tree_hashes(const fs::path &dir)
{
    std::map<std::string, std::string> m;
    if (!fs::exists(dir)) return m;
    for (auto &e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) m[fs::relative(e.path(), dir).generic_string()] = sha1_file(e.path());
    return m;
}

// a default.xex with an execution info header
static Bytes make_xex(uint32_t title, uint32_t version, size_t total)
{
    Bytes b(std::max<size_t>(total, 0x200), 0);
    std::memcpy(b.data(), "XEX2", 4);
    put_be32(&b[8], 0x1000);
    put_be32(&b[0x14], 2);
    put_be32(&b[0x18], 0x000002FF); put_be32(&b[0x1C], 0x180);      // some other header
    put_be32(&b[0x20], 0x00040006); put_be32(&b[0x24], 0x100);      // execution info at 0x100
    put_be32(&b[0x100], 0xCAFEBABE); put_be32(&b[0x104], version); put_be32(&b[0x108], 0x00000500);
    put_be32(&b[0x10C], title); b[0x112] = 1; b[0x113] = 1;
    for (size_t i = 0x200; i < b.size(); i++) b[i] = uint8_t(rnd());
    return b;
}

// ============================================================================================================
// XDVDFS builder
// ============================================================================================================

struct Node {
    std::string name;
    bool dir = false;
    Bytes data;
    std::vector<Node> kids;
    int64_t pin_sector = -1;        // put this file's data at this sector
    int64_t force_sector = -1;      // write this sector into the entry instead of the real one (damage tests)
    uint32_t force_size = 0;
    // filled by layout
    uint32_t sector = 0, size = 0;
};
static Node D(const std::string &n, std::vector<Node> kids = {}) { Node x; x.name = n; x.dir = true; x.kids = std::move(kids); return x; }
static Node F(const std::string &n, Bytes data) { Node x; x.name = n; x.data = std::move(data); return x; }

static std::string upper(std::string s) { for (auto &c : s) c = char(std::toupper(uint8_t(c))); return s; }

struct DirLayout { std::vector<uint32_t> order_offsets; };

// table bytes for a directory: a balanced tree sorted by upper-case name, entries never cross a sector
static Bytes dir_table(const std::vector<Node> &kids)
{
    if (kids.empty()) return {};
    std::vector<size_t> idx(kids.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return upper(kids[a].name) < upper(kids[b].name); });
    std::vector<uint32_t> off(kids.size());
    uint32_t cur = 0;
    // pass 1: offsets in preorder
    std::function<void(int, int)> place = [&](int lo, int hi) {
        if (lo > hi) return;
        int mid = (lo + hi) / 2;
        const Node &k = kids[idx[size_t(mid)]];
        uint32_t len = uint32_t((14 + k.name.size() + 3) & ~size_t(3));
        if (cur % 2048 + len > 2048) cur = (cur + 2047) & ~2047u;
        off[size_t(mid)] = cur;
        cur += len;
        place(lo, mid - 1);
        place(mid + 1, hi);
    };
    place(0, int(kids.size()) - 1);
    Bytes t((cur + 2047) & ~2047u, 0xFF);
    std::function<uint32_t(int, int)> fill = [&](int lo, int hi) -> uint32_t {
        if (lo > hi) return 0;
        int mid = (lo + hi) / 2;
        const Node &k = kids[idx[size_t(mid)]];
        uint32_t o = off[size_t(mid)];
        uint32_t l = fill(lo, mid - 1), r = fill(mid + 1, hi);
        put_le16(&t[o], l / 4); put_le16(&t[o + 2], r / 4);
        put_le32(&t[o + 4], k.force_sector >= 0 ? uint32_t(k.force_sector) : k.sector);
        put_le32(&t[o + 8], k.force_sector >= 0 ? k.force_size : k.size);
        t[o + 12] = k.dir ? 0x10 : 0x80;
        t[o + 13] = uint8_t(k.name.size());
        std::memcpy(&t[o + 14], k.name.data(), k.name.size());
        size_t len = (14 + k.name.size() + 3) & ~size_t(3);
        for (size_t i = 14 + k.name.size(); i < len; i++) t[o + i] = 0;
        return o;
    };
    fill(0, int(kids.size()) - 1);
    // the root of the tree must be at offset 0
    if (off[size_t((kids.size() - 1) / 2)] != 0) std::abort();
    return t;
}

struct Image {
    std::map<uint64_t, Bytes> sectors;   // first sector -> bytes
    uint32_t root_sector = 0, root_size = 0;
    uint64_t total_sectors = 0;
};

static void alloc(Node &n, uint32_t &next)
{
    if (n.dir) {
        n.size = uint32_t(dir_table(n.kids).size());
        n.sector = n.size ? next : 0;
        next += n.size / 2048;
        for (auto &k : n.kids) alloc(k, next);
    } else {
        n.size = uint32_t(n.data.size());
        if (n.pin_sector >= 0) { n.sector = uint32_t(n.pin_sector); return; }
        n.sector = next;
        next += uint32_t((n.size + 2047) / 2048);
    }
}
static void emit(const Node &n, Image &img)
{
    if (n.dir) {
        if (n.size) img.sectors[n.sector] = dir_table(n.kids);
        for (auto &k : n.kids) emit(k, img);
    } else if (n.size) {
        img.sectors[n.sector] = n.data;
    }
    uint64_t end = uint64_t(n.sector) + (n.size + 2047) / 2048;
    img.total_sectors = std::max(img.total_sectors, end);
}
static Bytes volume_descriptor(uint32_t rs, uint32_t rz)
{
    Bytes vd(2048, 0);
    std::memcpy(vd.data(), "MICROSOFT*XBOX*MEDIA", 20);
    put_le32(&vd[20], rs); put_le32(&vd[24], rz);
    std::memcpy(&vd[0x7EC], "MICROSOFT*XBOX*MEDIA", 20);
    return vd;
}
// root: a directory node (its name unused). first: the first sector for tables/files.
static Image build_image(Node &root, uint32_t first = 33, bool write_vd = true)
{
    Image img;
    uint32_t next = first;
    alloc(root, next);
    emit(root, img);
    img.root_sector = root.sector;
    img.root_size = root.size;
    if (write_vd) img.sectors[32] = volume_descriptor(root.sector, root.size);
    img.total_sectors = std::max<uint64_t>(img.total_sectors, 33);
    return img;
}
static void write_iso(const fs::path &p, const Image &img, uint64_t part, int64_t cut_bytes = 0)
{
    std::map<uint64_t, Bytes> ch;
    for (auto &s : img.sectors) ch[part + s.first * 2048] = s.second;
    write_sparse(p, ch, uint64_t(int64_t(part + img.total_sectors * 2048) - cut_bytes));
}

// expected files of a tree: rel path -> sha1
static void expect_files(const Node &n, const std::string &prefix, std::map<std::string, std::string> &m)
{
    for (auto &k : n.kids) {
        std::string p = prefix.empty() ? k.name : prefix + "/" + k.name;
        if (k.dir) expect_files(k, p, m);
        else m[p] = sha1_hex(k.data.data(), k.data.size());
    }
}

static Node game_tree(const Bytes &xex)
{
    Node cooked = D("CookedXenon");
    for (int i = 0; i < 300; i++) {   // several sectors of folder table
        char nm[64];
        std::snprintf(nm, sizeof nm, "%s_Level_%03d.xxx", i % 3 ? "SP" : "MP", i);
        cooked.kids.push_back(F(nm, random_bytes(rnd() % 5000)));
    }
    Node root = D("", {
        F("default.xex", xex),
        D("GearGame", {
            cooked,
            D("Config", { D("a", { D("b", { D("C", { F("Deep.ini", random_bytes(777)) }) }) }), F("Xe-GearEngine.ini", random_bytes(4096)) }),
            D("Movies", { F("Intro.bik", random_bytes(3 * 1024 * 1024 + 123)) }),
        }),
        D("EmptyFolder"),
        F("zero.bin", {}),
        F("MixedCase.TXT", random_bytes(2048)),
    });
    return root;
}

// ============================================================================================================
// STFS builder
// ============================================================================================================

struct SFile {
    std::string path;   // '/' separated
    Bytes data;
    bool contiguous = true;
};
struct StfsOpts {
    std::string magic = "LIVE";
    uint32_t header_size = 0x971A;
    bool female = true;
    bool lie_about_sex = false;        // descriptor bit says the other kind (the PS1 probes; so do we)
    bool alt_copies = false;           // male: current copy alternates per level-0 table; garbage in the other
    bool top_second = false;           // male: the top-level table's second copy is current
    uint32_t content_type = kContentTitleUpdate;
    uint32_t title_id = kTitleGears2;
    uint32_t version = 0x00000605, base_version = 0x00000005;
    uint32_t data_start = 0;           // first data block after the file table (0 = right after it)
    std::string display = "Title Update 6";
};
struct Stfs {
    std::map<uint64_t, Bytes> chunks;
    uint64_t length = 0;
    std::map<std::string, uint64_t> block_offsets;   // "path#j" -> physical byte offset (damage tests)
};

static Stfs build_stfs(std::vector<SFile> files, const StfsOpts &o)
{
    const uint32_t T = o.female ? 1 : 2;
    // entries: directories first (from the paths), then files
    struct Ent { std::string name; bool dir; int parent; uint32_t size = 0, start = 0, blocks = 0; bool contig = true; int file = -1; };
    std::vector<Ent> ents;
    std::map<std::string, int> dirs;
    auto dir_index = [&](const std::string &dpath) {
        int parent = -1;
        std::string cur;
        size_t pos = 0;
        while (pos < dpath.size()) {
            size_t s = dpath.find('/', pos);
            std::string part = dpath.substr(pos, s == std::string::npos ? std::string::npos : s - pos);
            cur = cur.empty() ? part : cur + "/" + part;
            if (!dirs.count(cur)) { dirs[cur] = int(ents.size()); ents.push_back({ part, true, parent }); }
            parent = dirs[cur];
            if (s == std::string::npos) break;
            pos = s + 1;
        }
        return parent;
    };
    for (size_t i = 0; i < files.size(); i++) {
        size_t s = files[i].path.rfind('/');
        int parent = s == std::string::npos ? -1 : dir_index(files[i].path.substr(0, s));
        Ent e{ s == std::string::npos ? files[i].path : files[i].path.substr(s + 1), false, parent };
        e.size = uint32_t(files[i].data.size());
        e.contig = files[i].contiguous;
        e.file = int(i);
        ents.push_back(e);
    }
    uint32_t ft_blocks = uint32_t((ents.size() + 63) / 64);
    // logical block contents and chains
    std::map<uint32_t, Bytes> data;        // block -> 4096 bytes
    std::map<uint32_t, uint32_t> next;     // block -> next (0xFFFFFF = end)
    uint32_t cursor = ft_blocks;
    if (o.data_start) cursor = o.data_start;
    std::vector<uint32_t> ft;
    for (uint32_t i = 0; i < ft_blocks; i++) ft.push_back(i);
    std::vector<std::vector<uint32_t>> fblocks(ents.size());
    // non-contiguous files: take their blocks from a shuffled pool after the contiguous ones
    uint32_t need_noncontig = 0;
    for (auto &e : ents) if (!e.dir && !e.contig) need_noncontig += (e.size + 4095) / 4096;
    for (auto &e : ents) {
        if (e.dir || !e.contig) continue;
        uint32_t n = (e.size + 4095) / 4096;
        for (uint32_t j = 0; j < n; j++) fblocks[size_t(&e - &ents[0])].push_back(cursor++);
    }
    std::vector<uint32_t> pool;
    for (uint32_t j = 0; j < need_noncontig; j++) pool.push_back(cursor++);
    for (size_t i = pool.size(); i > 1; i--) std::swap(pool[i - 1], pool[rnd() % i]);
    size_t pi = 0;
    for (auto &e : ents) {
        if (e.dir || e.contig) continue;
        uint32_t n = (e.size + 4095) / 4096;
        for (uint32_t j = 0; j < n; j++) fblocks[size_t(&e - &ents[0])].push_back(pool[pi++]);
    }
    const uint32_t allocated = cursor;
    // file data into blocks
    for (size_t i = 0; i < ents.size(); i++) {
        auto &e = ents[i];
        if (e.dir) continue;
        auto &bl = fblocks[i];
        e.start = bl.empty() ? 0 : bl[0];
        e.blocks = uint32_t(bl.size());
        const Bytes &src = files[size_t(e.file)].data;
        for (size_t j = 0; j < bl.size(); j++) {
            Bytes b(4096, 0);
            size_t n = std::min<size_t>(4096, src.size() - j * 4096);
            std::memcpy(b.data(), src.data() + j * 4096, n);
            data[bl[j]] = b;
            next[bl[j]] = j + 1 < bl.size() ? bl[j + 1] : 0xFFFFFF;
        }
    }
    // file table
    for (uint32_t i = 0; i < ft_blocks; i++) {
        Bytes b(4096, 0);
        for (size_t k = 0; k < 64 && i * 64 + k < ents.size(); k++) {
            const Ent &e = ents[i * 64 + k];
            uint8_t *p = &b[k * 64];
            std::memcpy(p, e.name.data(), e.name.size());
            p[0x28] = uint8_t(e.name.size() | (e.dir ? 0x80 : 0) | (e.contig || e.dir ? 0x40 : 0));
            put_le24(p + 0x29, e.blocks); put_le24(p + 0x2C, e.blocks); put_le24(p + 0x2F, e.start);
            put_be16(p + 0x32, uint32_t(e.parent) & 0xFFFF);
            put_be32(p + 0x34, e.size);
        }
        data[i] = b;
        next[i] = i + 1 < ft_blocks ? i + 1 : 0xFFFFFF;
    }

    // physical layout by walking the package from the front:
    //   [L0 #0][data 0-169][L1 #0][L0 #1][data 170-339][L0 #2]...  and before data 28900: [L2][L1 #1][L0 #170]
    // (each table takes T blocks)
    std::map<uint32_t, uint64_t> phys;            // data block -> physical block
    std::map<uint32_t, uint64_t> l0p, l1p;        // table index -> physical block
    uint64_t l2p = 0;
    uint64_t p = 0;
    int level = allocated <= 170 ? 0 : allocated <= 28900 ? 1 : 2;
    for (uint32_t b = 0; b < allocated; b++) {
        if (b % 170 == 0) {
            if (b == 28900) { l2p = p; p += T; }
            if (b == 170) { l1p[0] = p; p += T; }
            else if (b && b % 28900 == 0) { l1p[b / 28900] = p; p += T; }
            l0p[b / 170] = p; p += T;
        }
        phys[b] = p++;
    }
    const uint64_t base = (uint64_t(o.header_size) + 0xFFF) & ~uint64_t(0xFFF);
    Stfs out;
    auto at = [&](uint64_t blk) { return base + blk * 4096; };
    for (auto &d : data) out.chunks[at(phys[d.first])] = d.second;
    for (size_t i = 0; i < ents.size(); i++)
        for (size_t j = 0; j < fblocks[i].size(); j++) {
            std::string path;
            for (int c = int(i); c >= 0; c = ents[size_t(c)].parent) path = ents[size_t(c)].name + (path.empty() ? "" : "/" + path);
            out.block_offsets[path + "#" + std::to_string(j)] = at(phys[fblocks[i][j]]);
        }

    // hash tables. cur0[k]: which copy of level-0 table k is current
    auto copy_of = [&](uint32_t k) -> int {
        if (T == 1) return 0;
        if (level == 0) return o.top_second ? 1 : 0;
        return o.alt_copies ? int(k % 2) : 0;
    };
    std::map<uint32_t, Bytes> l0tab;
    for (uint32_t k = 0; k <= (allocated - 1) / 170; k++) {
        Bytes t(4096, 0);
        for (uint32_t i = 0; i < 170; i++) {
            uint32_t b = k * 170 + i;
            if (!data.count(b)) continue;
            sha1_raw(data[b].data(), 4096, &t[i * 24]);
            t[i * 24 + 0x14] = 0x80;
            put_be24(&t[i * 24 + 0x15], next[b]);
        }
        l0tab[k] = t;
        int c = copy_of(k);
        out.chunks[at(l0p[k]) + uint64_t(c) * 4096] = t;
        if (T == 2) out.chunks[at(l0p[k]) + uint64_t(1 - c) * 4096] = random_bytes(4096);   // the stale copy
    }
    Bytes top;
    if (level >= 1) {
        std::map<uint32_t, Bytes> l1tab;
        for (uint32_t m = 0; m <= (allocated - 1) / 28900; m++) {
            Bytes t(4096, 0);
            for (uint32_t i = 0; i < 170; i++) {
                uint32_t k = m * 170 + i;
                if (!l0tab.count(k)) continue;
                sha1_raw(l0tab[k].data(), 4096, &t[i * 24]);
                t[i * 24 + 0x14] = uint8_t(0x80 | (copy_of(k) ? 0x40 : 0));
            }
            l1tab[m] = t;
            int c = (level == 1 && o.top_second) ? 1 : 0;
            out.chunks[at(l1p[m]) + uint64_t(c) * 4096] = t;
            if (T == 2) out.chunks[at(l1p[m]) + uint64_t(1 - c) * 4096] = Bytes(4096, 0);
        }
        top = l1tab[0];
        if (level == 2) {
            Bytes t(4096, 0);
            for (auto &l1 : l1tab) { sha1_raw(l1.second.data(), 4096, &t[l1.first * 24]); t[l1.first * 24 + 0x14] = 0x80; }
            int c = o.top_second ? 1 : 0;
            out.chunks[at(l2p) + uint64_t(c) * 4096] = t;
            if (T == 2) out.chunks[at(l2p) + uint64_t(1 - c) * 4096] = Bytes(4096, 0);
            top = t;
        }
    } else {
        top = l0tab[0];
    }

    // header
    Bytes h(o.header_size, 0);
    std::memcpy(h.data(), o.magic.data(), 4);
    put_be32(&h[0x340], o.header_size);
    put_be32(&h[0x344], o.content_type);
    put_be32(&h[0x348], 1);
    put_be64(&h[0x34C], uint64_t(allocated) * 4096);
    put_be32(&h[0x354], 0x12345678);
    put_be32(&h[0x358], o.version);
    put_be32(&h[0x35C], o.base_version);
    put_be32(&h[0x360], o.title_id);
    h[0x364] = 2; h[0x366] = 1; h[0x367] = 1;
    uint8_t *vd = &h[0x379];
    vd[0] = 0x24;
    bool female_bit = o.female != o.lie_about_sex;
    vd[2] = uint8_t((female_bit ? 1 : 0) | (o.top_second ? 2 : 0));
    put_le16(vd + 3, ft_blocks);
    put_le24(vd + 5, 0);
    sha1_raw(top.data(), 4096, vd + 8);
    put_be32(vd + 0x1C, allocated);
    put_be32(vd + 0x20, 0);
    put_be32(&h[0x3A9], 0);
    put_utf16be(&h[0x411], o.display);
    put_utf16be(&h[0x1691], "Gears of War 2");
    out.chunks[0] = h;
    uint64_t maxp = 0;
    for (auto &c : out.chunks) maxp = std::max<uint64_t>(maxp, c.first + c.second.size());
    out.length = maxp;
    return out;
}

static std::map<std::string, std::string> expect_stfs(const std::vector<SFile> &files)
{
    std::map<std::string, std::string> m;
    for (auto &f : files) m[f.path] = sha1_hex(f.data.data(), f.data.size());
    return m;
}

// ============================================================================================================
// SVOD builder (Games on Demand)
// ============================================================================================================
// true block t (file-system sector after the shift) -> Data file t / 82824, byte
// 0x1000 (master hash) + g * 0x1000 (level-0 hash blocks so far, g = (t % 82824) / 408) + 0x1000 + (t % 82824) * 0x800

struct GodOpts {
    bool egdf = false;
    uint32_t block_offset = 0;
    uint32_t content_type = kContentGamesOnDemand;
};
static void build_god(const fs::path &dir, const std::string &name, const Image &img, const GodOpts &o)
{
    std::map<uint32_t, std::map<uint64_t, Bytes>> files;
    auto place = [&](uint64_t sector, const uint8_t *p, size_t n) {
        int64_t t = int64_t(sector) - 2 * int64_t(o.block_offset) + (o.egdf ? 2 : 0);
        if (t < 0) std::abort();
        uint32_t fi = uint32_t(t / 82824);
        uint64_t fb = uint64_t(t) % 82824;
        uint64_t addr = 0x1000 + (fb / 408) * 0x1000 + 0x1000 + fb * 0x800;
        files[fi][addr] = Bytes(p, p + n);
    };
    for (auto &s : img.sectors) {
        if (s.first == 32 && o.egdf) continue;   // the descriptor is written directly below
        for (size_t off = 0; off < s.second.size(); off += 2048) {
            size_t n = std::min<size_t>(2048, s.second.size() - off);
            place(s.first + off / 2048, s.second.data() + off, n);
        }
    }
    Bytes vd = volume_descriptor(img.root_sector, img.root_size);
    if (o.egdf) files[0][0x2000] = vd;
    else {
        Bytes xsf(2048, 0);
        std::memcpy(xsf.data(), "XSF", 3);
        place(0, xsf.data(), xsf.size());
    }
    uint32_t count = files.rbegin()->first + 1;
    fs::path dd = dir / (name + ".data");
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "Data%04u", i);
        uint64_t len = 0x2000;
        for (auto &c : files[i]) len = std::max<uint64_t>(len, c.first + c.second.size());
        if (i + 1 < count) len = 0xA290000;
        len = (len + 0xFFF) & ~uint64_t(0xFFF);
        write_sparse(dd / nm, files[i], len);
        total += len;
    }
    Bytes h(0xA000, 0);
    std::memcpy(h.data(), "LIVE", 4);
    put_be32(&h[0x340], 0xAD0E);
    put_be32(&h[0x344], o.content_type);
    put_be32(&h[0x348], 2);
    put_be32(&h[0x360], kTitleGears2);
    uint8_t *vdp = &h[0x379];
    vdp[0] = 0x24;
    vdp[0x18] = o.egdf ? 0x40 : 0;
    put_le24(vdp + 0x19, uint32_t(img.total_sectors / 2));
    put_le24(vdp + 0x1C, o.block_offset);
    put_be32(&h[0x39D], count);
    put_be64(&h[0x3A1], total);
    put_be32(&h[0x3A9], 1);
    put_utf16be(&h[0x411], "Gears of War 2");
    write_file(dir / name, h);
}

// ============================================================================================================
// tests
// ============================================================================================================

static fs::path W;

static void test_sha1()
{
    CHECK(sha1_hex("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha1_hex("", 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    std::string q = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha1_hex(q.data(), q.size()) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    write_file(W / "million_a", Bytes(1000000, 'a'));
    CHECK(sha1_file(W / "million_a") == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    CHECK(sha1_file(W / "no_such_file").empty());
}

static void test_disc()
{
    Bytes xex = make_xex(kTitleGears2, 0x00000005, 0x4000);
    Node root = game_tree(xex);
    Image img = build_image(root);
    std::map<std::string, std::string> want;
    expect_files(root, "", want);

    const uint64_t parts[] = { 0, 0xFD90000ull, 0x2080000ull, 0x18300000ull };
    for (uint64_t part : parts) {
        fs::path iso = W / ("game_" + std::to_string(part) + ".iso");
        write_iso(iso, img, part);
        fs::path r;
        CHECK(detect(iso, &r) == SourceKind::DiscImage && r == iso);
        CHECK(disc_partition_offset(iso) == int64_t(part));
        CHECK(disc_has_file(iso, "default.xex"));
        CHECK(disc_has_file(iso, "DEFAULT.XEX"));
        CHECK(disc_has_file(iso, "GearGame\\Config\\a\\b\\c\\deep.ini"));
        CHECK(disc_has_file(iso, "GearGame/CookedXenon/SP_Level_299.xxx"));
        CHECK(!disc_has_file(iso, "GearGame/CookedXenon/SP_Level_300.xxx"));
        CHECK(!disc_has_file(iso, "GearGame"));   // a folder, not a file
        XexInfo xi;
        CHECK(disc_xex_info(iso, xi) && xi.title_id == kTitleGears2 && xi.media_id == 0xCAFEBABE && xi.disc_count == 1);
        if (part != 0xFD90000ull) fs::remove(iso);
    }
    fs::path iso = W / ("game_" + std::to_string(0xFD90000ull) + ".iso");

    {   // one file read in place (the launcher's music): whole, a range, past the end, a folder, not there
        const Node &cooked = root.kids[1].kids[0];
        const Node &lvl = cooked.kids[299];
        Bytes got;
        uint64_t sz = 0;
        CHECK(image_read_file(iso, "GearGame/CookedXenon/" + lvl.name, 0, 1 << 20, got, &sz) && got == lvl.data && sz == lvl.data.size());
        const Node &mov = root.kids[1].kids[2].kids[0];
        CHECK(image_read_file(iso, "gamegame_typo/x", 0, 10, got) == false && got.empty());
        CHECK(image_read_file(iso, "GEARGAME\\MOVIES\\intro.bik", 2047, 5000, got, &sz) && sz == mov.data.size() &&
              got == Bytes(mov.data.begin() + 2047, mov.data.begin() + 2047 + 5000));
        CHECK(image_read_file(iso, "GearGame/Movies/Intro.bik", sz - 10, 100, got) && got == Bytes(mov.data.end() - 10, mov.data.end()));
        CHECK(!image_read_file(iso, "GearGame/Movies/Intro.bik", sz + 1, 1, got));
        CHECK(!image_read_file(iso, "GearGame/Movies", 0, 1, got));
        CHECK(!image_read_file(W / "nothing.iso", "default.xex", 0, 1, got));
    }

    std::vector<Entry> list;
    std::string err;
    CHECK(list_disc(iso, list, err));
    size_t nfiles = 0, ndirs = 0;
    for (auto &e : list) (e.is_dir ? ndirs : nfiles)++;
    CHECK(nfiles == want.size());
    CHECK(ndirs == 8);   // GearGame, CookedXenon, Config, a, b, C, Movies, EmptyFolder
    std::printf("  disc listing: %zu files, %zu folders\n", nfiles, ndirs);

    // extract with progress
    fs::path out = W / "disc_out";
    uint64_t last_done = 0, last_total = 0, calls = 0;
    bool monotonic = true;
    std::set<std::string> seen_files;
    bool ok = extract_disc(iso, out, [&](uint64_t d, uint64_t t, const std::string &f) {
        if (d < last_done) monotonic = false;
        last_done = d; last_total = t; calls++;
        if (!f.empty()) seen_files.insert(f);
    }, nullptr, err);
    CHECK_MSG(ok, err);
    CHECK(tree_hashes(out) == want);
    CHECK(fs::is_directory(out / "EmptyFolder"));
    CHECK(fs::exists(out / "zero.bin") && fs::file_size(out / "zero.bin") == 0);
    CHECK(fs::exists(out / "GearGame" / "Config" / "a" / "b" / "C" / "Deep.ini"));   // case kept
    uint64_t sum = 0;
    for (auto &e : list) sum += e.size;
    CHECK(monotonic && last_done == sum && last_total == sum && calls > nfiles);
    CHECK(seen_files.count("GearGame/Movies/Intro.bik") == 1);
    XexInfo xi;
    CHECK(read_xex_info(out / "default.xex", xi) && xi.title_id == kTitleGears2);
    fs::path r;
    CHECK(detect(out, &r) == SourceKind::ExtractedFolder && r == out);
    CHECK(detect(out / "default.xex", &r) == SourceKind::ExtractedFolder && r == out);

    // cancel after the first progress call
    std::atomic<bool> stop{ false };
    ok = extract_disc(iso, W / "disc_cancel", [&](uint64_t, uint64_t, const std::string &) { stop = true; }, &stop, err);
    CHECK(!ok && err == "The copy was cancelled.");

    // cut short: the last file (Intro.bik is not last; cut 1 MB off the end) -> nothing written, clear message
    fs::path cut = W / "cut.iso";
    write_iso(cut, img, 0, 1 << 20);
    ok = extract_disc(cut, W / "cut_out", nullptr, nullptr, err);
    CHECK_MSG(!ok && err.find("incomplete") != std::string::npos && err.find("byte") != std::string::npos, err);
    CHECK(!fs::exists(W / "cut_out"));
    std::printf("  cut-short image: \"%s\"\n", err.c_str());
    fs::remove(fs::path(cut));
    fs::remove(iso);
}

static void test_disc_hostile()
{
    // unsafe names, a folder pointing back at the root, an original Xbox disc, not a disc at all
    Node root = D("", {
        F("default.xex", make_xex(kTitleGears3, 1, 0x400)),
        F("..", random_bytes(10)),
        F(".", random_bytes(10)),
        F("a\\..\\..\\escape.txt", random_bytes(10)),
        F("C:evil", random_bytes(10)),
        F("CON", random_bytes(10)),
        F("nul.txt", random_bytes(10)),
        F("trail.", random_bytes(10)),
        F("ok.txt", random_bytes(10)),
        F("OK.TXT", random_bytes(11)),   // same name without case: second one skipped
    });
    Image img = build_image(root);
    fs::path iso = W / "hostile.iso";
    write_iso(iso, img, 0);
    fs::path out = W / "hostile_out";
    std::string err;
    CHECK_MSG(extract_disc(iso, out, nullptr, nullptr, err), err);
    auto got = tree_hashes(out);
    std::set<std::string> names;
    for (auto &g : got) names.insert(g.first);
    std::set<std::string> want = { "default.xex", "a_.._.._escape.txt", "C_evil", "_CON", "_nul.txt", "trail_" };
    // "ok.txt"/"OK.TXT": exactly one of them
    bool one_ok = names.count("ok.txt") + names.count("OK.TXT") == 1;
    names.erase("ok.txt"); names.erase("OK.TXT");
    CHECK(one_ok);
    CHECK(names == want);
    bool escaped = false;
    for (auto &e : fs::directory_iterator(W)) escaped = escaped || e.path().filename() == "escape.txt";
    CHECK(!escaped);

    // a folder whose entry points at the root's own table
    Node loop = D("", { F("default.xex", make_xex(kTitleGears2, 1, 0x400)), D("Loop", { F("x", random_bytes(5)) }) });
    Image li = build_image(loop);
    loop.kids[1].force_sector = loop.sector;
    loop.kids[1].force_size = loop.size;
    li = Image();
    emit(loop, li);
    li.sectors[32] = volume_descriptor(loop.sector, loop.size);
    li.total_sectors = std::max<uint64_t>(li.total_sectors, 33);
    write_iso(W / "loop.iso", li, 0);
    CHECK(!extract_disc(W / "loop.iso", W / "loop_out", nullptr, nullptr, err) && err.find("damaged") != std::string::npos);
    std::vector<Entry> l;
    CHECK(!list_disc(W / "loop.iso", l, err));

    // original Xbox disc
    Node ox = D("", { F("default.xbe", random_bytes(4000)), D("Worlds", { F("x.bfa", random_bytes(9)) }) });
    Image oi = build_image(ox);
    write_iso(W / "xbox1.iso", oi, 0x18300000ull);
    CHECK(detect(W / "xbox1.iso") == SourceKind::DiscImage);
    CHECK(!disc_has_file(W / "xbox1.iso", "default.xex"));
    CHECK(!extract_disc(W / "xbox1.iso", W / "xbox1_out", nullptr, nullptr, err) && err.find("original Xbox") != std::string::npos);
    std::printf("  original Xbox: \"%s\"\n", err.c_str());
    fs::remove(W / "xbox1.iso");

    // no default.xex at all
    Node nx = D("", { F("readme.txt", random_bytes(40)) });
    Image ni = build_image(nx);
    write_iso(W / "noxex.iso", ni, 0);
    CHECK(!extract_disc(W / "noxex.iso", W / "noxex_out", nullptr, nullptr, err) &&
          err == "This disc image is not an Xbox 360 game disc (it has no default.xex).");

    // not a disc image
    write_file(W / "random.iso", random_bytes(200000));
    CHECK(detect(W / "random.iso") == SourceKind::Unknown);
    CHECK(!extract_disc(W / "random.iso", W / "random_out", nullptr, nullptr, err) &&
          err.find("is not an Xbox 360 game disc") != std::string::npos);
    std::printf("  random file: \"%s\"\n", err.c_str());
    CHECK(detect(W / "nothing_here") == SourceKind::Unknown);
}

static std::vector<SFile> tu_files()
{
    std::vector<SFile> f;
    f.push_back({ "default.xexp", random_bytes(900000) });   // 220 blocks on its own: a level-1 package
    const char *mods[] = { "Engine", "GearGame", "IpDrv", "OnlineSubsystemLive" };
    for (auto m : mods) {
        f.push_back({ std::string("GearGame/Patches/ScriptPatch_") + m + ".bin", random_bytes(rnd() % 30000 + 1) });
        f.push_back({ std::string("GearGame/Patches/ScriptPatch_") + m + ".bin_us", random_bytes(rnd() % 3000 + 1) });
    }
    const char *langs[] = { "INT", "FRA", "DEU", "ITA", "ESN", "ESM", "JPN", "KOR", "CHT", "POL", "RUS", "PTB" };
    const char *pk[] = { "Engine", "GearGame", "GearGameContent", "GearGameUI" };
    for (auto l : langs)
        for (auto p : pk) f.push_back({ std::string("TU__") + p + "." + l, random_bytes(rnd() % 6000) });
    f.push_back({ "TU__Camera.ini", random_bytes(4096) });   // exactly one block
    f.push_back({ "TU__Weapon.ini", random_bytes(0) });      // empty
    return f;   // 1 + 8 + 48 + 2 = 59 files + 2 folders = 61 entries
}

static void test_stfs()
{
    // (a) a female LIVE title update (the real case), header 0x971A
    std::vector<SFile> files = tu_files();
    files.push_back({ "Extra/More/Deeper/x.bin", random_bytes(70000) });   // 61 + 4 = 65 entries: two file table blocks
    StfsOpts o;
    Stfs pkg = build_stfs(files, o);
    fs::path tu = W / "TU_16L621D_000000K000000.00000000001G5";
    write_sparse(tu, pkg.chunks, pkg.length);
    std::printf("  title update package: %llu bytes\n", (unsigned long long)pkg.length);

    PackageInfo info;
    std::string err;
    CHECK(read_package_info(tu, info, err));
    CHECK(info.magic == "LIVE" && info.title_id == kTitleGears2 && info.content_type == kContentTitleUpdate && info.is_title_update());
    CHECK(info.display_name == "Title Update 6" && info.title_name == "Gears of War 2");
    CHECK(info.version == 0x605 && info.version_string() == "0.0.6.5" && info.base_version == 5);
    CHECK(info.header_size == 0x971A && info.file_table_blocks == 2 && !info.is_svod() && (info.block_separation & 1));
    CHECK(info.allocated_blocks > 170);
    fs::path r;
    CHECK(detect(tu, &r) == SourceKind::StfsPackage && r == tu);

    std::vector<Entry> list;
    CHECK(list_stfs(tu, list, err) && list.size() == files.size() + 5);   // + GearGame, Patches, Extra, More, Deeper
    fs::path out = W / "tu_out";
    uint64_t done = 0, total = 0;
    CHECK_MSG(extract_stfs(tu, out, [&](uint64_t d, uint64_t t, const std::string &) { done = d; total = t; }, nullptr, err), err);
    CHECK(tree_hashes(out) == expect_stfs(files));
    uint64_t sum = 0;
    for (auto &f : files) sum += f.data.size();
    CHECK(done == sum && total == sum);

    // (a2) the descriptor's female bit wrong: the reader falls back to the other table count (as the PS1 probes)
    StfsOpts lie = o;
    lie.lie_about_sex = true;
    g_seed = 777;
    std::vector<SFile> f2 = tu_files();
    Stfs p2 = build_stfs(f2, lie);
    write_sparse(W / "tu_lie", p2.chunks, p2.length);
    CHECK_MSG(extract_stfs(W / "tu_lie", W / "tu_lie_out", nullptr, nullptr, err), err);
    CHECK(tree_hashes(W / "tu_lie_out") == expect_stfs(f2));

    // (b) a male CON package, header 0xAD0E: chained (scattered) files, the current hash table copy
    //     alternating, garbage in the stale copies -> next-block pointers must come from the current copy
    std::vector<SFile> chained;
    for (int i = 0; i < 6; i++) chained.push_back({ "Chained/file" + std::to_string(i) + ".dat", random_bytes(rnd() % 300000 + 1), false });
    chained.push_back({ "contig.dat", random_bytes(500000) });
    StfsOpts m;
    m.magic = "CON ";
    m.header_size = 0xAD0E;
    m.female = false;
    m.alt_copies = true;
    m.content_type = kContentMarketplace;
    Stfs p3 = build_stfs(chained, m);
    write_sparse(W / "male_con", p3.chunks, p3.length);
    CHECK(read_package_info(W / "male_con", info, err) && info.magic == "CON " && !(info.block_separation & 1) && info.allocated_blocks > 340);
    CHECK_MSG(extract_stfs(W / "male_con", W / "male_out", nullptr, nullptr, err), err);
    CHECK(tree_hashes(W / "male_out") == expect_stfs(chained));
    std::printf("  male package: %u blocks\n", info.allocated_blocks);

    // (c) small male package (level 0), second copy of the top table current
    std::vector<SFile> small = { { "a.bin", random_bytes(9000), false }, { "b.bin", random_bytes(20000), false } };
    StfsOpts s = m;
    s.alt_copies = false;
    s.top_second = true;
    s.magic = "PIRS";
    Stfs p4 = build_stfs(small, s);
    write_sparse(W / "male_small", p4.chunks, p4.length);
    CHECK_MSG(extract_stfs(W / "male_small", W / "male_small_out", nullptr, nullptr, err), err);
    CHECK(tree_hashes(W / "male_small_out") == expect_stfs(small));

    // (d) a level-2 package (more than 28,900 blocks; sparse on disk): a file crossing block 28,900
    std::vector<SFile> big = { { "default.xexp", random_bytes(200000) }, { "tail.bin", random_bytes(9000) } };
    StfsOpts l2 = o;
    l2.data_start = 28880;
    Stfs p5 = build_stfs(big, l2);
    write_sparse(W / "level2", p5.chunks, p5.length);
    CHECK(read_package_info(W / "level2", info, err) && info.allocated_blocks > 28900);
    CHECK_MSG(extract_stfs(W / "level2", W / "level2_out", nullptr, nullptr, err), err);
    CHECK(tree_hashes(W / "level2_out") == expect_stfs(big));
    // the same, male with alternating copies (the level-2 copy-selection path)
    StfsOpts l2m = m;
    l2m.data_start = 28880;
    std::vector<SFile> bigc = { { "x.bin", random_bytes(200000), false }, { "y.bin", random_bytes(9000) } };
    Stfs p6 = build_stfs(bigc, l2m);
    write_sparse(W / "level2_male", p6.chunks, p6.length);
    CHECK_MSG(extract_stfs(W / "level2_male", W / "level2_male_out", nullptr, nullptr, err), err);
    CHECK(tree_hashes(W / "level2_male_out") == expect_stfs(bigc));
    fs::remove(W / "level2_male");

    // (e) damage: one byte changed in block 5 of default.xexp
    uint64_t bad = pkg.block_offsets.at("default.xexp#5");
    auto dmg = pkg.chunks;
    std::map<uint64_t, Bytes> patch = dmg;
    for (auto &c : patch)
        if (c.first <= bad + 100 && bad + 100 < c.first + c.second.size()) c.second[bad + 100 - c.first] ^= 0x55;
    write_sparse(W / "tu_damaged", patch, pkg.length);
    CHECK(!extract_stfs(W / "tu_damaged", W / "tu_damaged_out", nullptr, nullptr, err) &&
          err.find("damaged near byte " + std::to_string(bad)) != std::string::npos);
    std::printf("  damaged package: \"%s\"\n", err.c_str());
    CHECK(extract_stfs(W / "tu_damaged", W / "tu_damaged_out2", nullptr, nullptr, err, false));   // without the check

    // (f) cut short
    write_sparse(W / "tu_cut", pkg.chunks, pkg.length - 5000);
    CHECK(!extract_stfs(W / "tu_cut", W / "tu_cut_out", nullptr, nullptr, err) && err.find("incomplete") != std::string::npos);
    std::printf("  cut-short package: \"%s\"\n", err.c_str());

    // (g) not a package / cancel
    CHECK(!extract_stfs(W / "random.iso", W / "x_out", nullptr, nullptr, err) && err.find("not an Xbox 360") != std::string::npos);
    std::atomic<bool> stop{ true };
    CHECK(!extract_stfs(tu, W / "tu_cancel", nullptr, &stop, err) && err == "The copy was cancelled.");

    // (h) detection of an update inside <titleid>\000B0000 (the newest is chosen)
    fs::path tid = W / "Content" / "4D5307D5";
    fs::create_directories(tid / "000B0000");
    fs::copy_file(tu, tid / "000B0000" / "TU_new", fs::copy_options::overwrite_existing);
    StfsOpts old = o;
    old.version = 0x00000305;
    std::vector<SFile> of = { { "default.xexp", random_bytes(5000) } };
    Stfs p7 = build_stfs(of, old);
    write_sparse(tid / "000B0000" / "TU_old", p7.chunks, p7.length);
    CHECK(detect(tid, &r) == SourceKind::StfsPackage && r.filename() == "TU_new");
    CHECK(detect(tid / "000B0000", &r) == SourceKind::StfsPackage && r.filename() == "TU_new");
}

static void test_god()
{
    Bytes xex = make_xex(kTitleGears2, 5, 0x3000);
    // XSF layout, two data files: a file pinned across the Data0000 / Data0001 boundary (sector 82824)
    Node root = game_tree(xex);
    Node span = F("span.bin", random_bytes(300 * 2048 + 77));
    span.pin_sector = 82824 - 100;
    root.kids.push_back(span);
    Image img = build_image(root);
    fs::path god = W / "god" / "4D5307D5" / "00007000";
    build_god(god, "ABCDEF0123456789", img, GodOpts());
    fs::path hdr = god / "ABCDEF0123456789";
    std::map<std::string, std::string> want;
    expect_files(root, "", want);

    fs::path r;
    CHECK(detect(hdr, &r) == SourceKind::GodContainer && r == hdr);
    CHECK(detect(god, &r) == SourceKind::GodContainer && r == hdr);
    CHECK(detect(god.parent_path(), &r) == SourceKind::GodContainer && r == hdr);
    CHECK(detect(god / "ABCDEF0123456789.data", &r) == SourceKind::GodContainer && r == hdr);
    CHECK(detect(god / "ABCDEF0123456789.data" / "Data0001", &r) == SourceKind::GodContainer && r == hdr);
    PackageInfo info;
    std::string err;
    CHECK(read_package_info(hdr, info, err) && info.is_svod() && info.content_type == kContentGamesOnDemand && info.data_file_count == 2);
    std::vector<Entry> list;
    CHECK_MSG(list_god(hdr, list, err), err);
    {   // a file read in place, across the Data0000 / Data0001 boundary
        Bytes got;
        uint64_t sz = 0;
        CHECK(image_read_file(hdr, "span.bin", 0, 1 << 20, got, &sz) && got == span.data && sz == span.data.size());
        CHECK(image_read_file(hdr, "SPAN.BIN", 99 * 2048 + 5, 4000, got) && got == Bytes(span.data.begin() + 99 * 2048 + 5, span.data.begin() + 99 * 2048 + 4005));
        CHECK(image_read_file(hdr, "default.xex", 0, 1 << 20, got) && got == xex);
    }
    fs::path out = W / "god_out";
    CHECK_MSG(extract_god(hdr, out, nullptr, nullptr, err), err);
    CHECK(tree_hashes(out) == want);
    CHECK(!extract_stfs(hdr, W / "god_as_stfs", nullptr, nullptr, err) && err.find("Games on Demand") != std::string::npos);

    // the last data file missing
    fs::rename(god / "ABCDEF0123456789.data" / "Data0001", W / "Data0001.saved");
    CHECK(!extract_god(hdr, W / "god_missing", nullptr, nullptr, err) && err.find("Data0001 is missing") != std::string::npos);
    std::printf("  GoD missing part: \"%s\"\n", err.c_str());
    CHECK(!fs::exists(W / "god_missing" / "default.xex"));   // checked before anything is written
    fs::remove_all(W / "god");

    // enhanced GDF layout with a data block offset (sectors shifted by 2 * offset - 2)
    Node r2 = game_tree(make_xex(kTitleGears3, 5, 0x2000));
    Image i2 = build_image(r2, 64, false);
    GodOpts eg;
    eg.egdf = true;
    eg.block_offset = 20;
    fs::path g2 = W / "god2" / "4D53085B" / "00007000";
    build_god(g2, "0123456789ABCDEF", i2, eg);
    std::map<std::string, std::string> w2;
    expect_files(r2, "", w2);
    CHECK_MSG(extract_god(g2 / "0123456789ABCDEF", W / "god2_out", nullptr, nullptr, err), err);
    CHECK(tree_hashes(W / "god2_out") == w2);
    fs::remove_all(W / "god2");

    // an SVOD package that is not a 360 game (no default.xex)
    Node r3 = D("", { F("default.xbe", random_bytes(100)) });
    Image i3 = build_image(r3);
    build_god(W / "god3", "X", i3, GodOpts());
    CHECK(!extract_god(W / "god3" / "X", W / "god3_out", nullptr, nullptr, err) && err.find("original Xbox") != std::string::npos);
    // a title update given as a GoD
    CHECK(!extract_god(W / "TU_16L621D_000000K000000.00000000001G5", W / "x", nullptr, nullptr, err) &&
          err.find("not a Games on Demand") != std::string::npos);
}

int main(int argc, char **argv)
{
    W = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "x360fs_test";
    fs::remove_all(W);
    fs::create_directories(W);
    std::printf("work folder: %s\n", W.string().c_str());
    test_sha1();
    test_disc();
    test_disc_hostile();
    test_stfs();
    test_god();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
