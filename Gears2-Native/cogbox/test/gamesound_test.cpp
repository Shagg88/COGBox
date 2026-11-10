// CogBox gamesound tests (the library music's reader, src/gamesound.cpp): synthetic UE3 packages - plain, fully
// compressed (LZX stored blocks in XMemCompress frames, and a stored block), Gears 1's LZO kind, a summary followed by
// a chunk - with SoundNodeWave-style XMA records laid out from vgmstream's / UE Viewer's description, and records that
// must NOT be found. Then FFmpeg's XMA decoder (the minimal Linux build of tools/build_ffmpeg_xma.sh's source) must
// refuse packets that are not XMA. No game content: real decoding is checked on the player's PC (--musictest).
//   FF=<FFmpeg 9.0.2 built for Linux with the configure line of tools/build_ffmpeg_xma.sh, minus the cross options>
//   g++ -std=c++17 -O2 -Wall -Wextra -I../src -I../../src/layer2_ue3/include -isystem $FF/include gamesound_test.cpp
//       ../src/gamesound.cpp ../src/x360fs.cpp ../../src/layer2_ue3/prebuild_lzx.cpp -ldl -o gamesound_test
//   cp $FF/lib/libavcodec.so.63 $FF/lib/libavutil.so.61 . && LD_LIBRARY_PATH=. ./gamesound_test
#include "gamesound.h"
#include "util.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>

using Bytes = std::vector<uint8_t>;
using namespace gamesound;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// util.h stand-ins (the launcher's are Windows code)
std::string exe_dir()
{
    char b[4096];
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    std::string s(b, n > 0 ? size_t(n) : 0);
    return s.substr(0, s.find_last_of('/'));
}
std::wstring exe_dir_w() { return std::wstring(); }
std::wstring widen(const std::string &s) { return std::wstring(s.begin(), s.end()); }
std::string join(const std::string &a, const std::string &b) { return a + "/" + b; }
uint64_t file_size(const std::string &p) { std::ifstream f(p, std::ios::binary | std::ios::ate); return f ? uint64_t(f.tellg()) : 0; }
bool read_file(const std::string &p, std::string &out, size_t limit)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return out.size() <= limit;
}

static uint32_t g_seed = 99;
static uint8_t rnd() { g_seed = g_seed * 1103515245u + 12345u; return uint8_t(g_seed >> 16); }
static void be32(Bytes &b, uint32_t v) { for (int i = 3; i >= 0; i--) b.push_back(uint8_t(v >> (i * 8))); }
static void be16(Bytes &b, uint32_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
static void put32(Bytes &b, size_t at, uint32_t v) { for (int i = 0; i < 4; i++) b[at + i] = uint8_t(v >> (24 - i * 8)); }
static void fstr(Bytes &b, const std::string &s) { be32(b, uint32_t(s.size() + 1)); b.insert(b.end(), s.begin(), s.end()); b.push_back(0); }

// UE3's XMA layout: format chunk size, seek size, data size, format, seek table, packets
static Bytes xma_record(int kind, int channels, uint32_t rate, uint32_t samples, size_t packets, bool random_packets)
{
    Bytes f;
    if (kind == 0x34) {   // WAVEFORMATEX (BE) + XMA2 extension
        be16(f, 0x166); be16(f, channels); be32(f, rate); be32(f, rate * 4); be16(f, 2048); be16(f, 16); be16(f, 34);
        be16(f, (channels + 1) / 2); be32(f, 3); be32(f, samples); be32(f, 0x10000); be32(f, 0); be32(f, samples);
        be32(f, 0); be32(f, 0); f.push_back(0); f.push_back(4); be16(f, 1);
    } else if (kind == 2) {   // XMA2WAVEFORMAT version 4, one stream
        f.push_back(4); f.push_back(1); f.push_back(0); f.push_back(0);
        be32(f, 0); be32(f, 0); be32(f, rate); be32(f, 0); be32(f, 0); be32(f, 0x10000); be32(f, samples); be32(f, samples); be32(f, 1);
        f.push_back(uint8_t(channels)); f.push_back(0); be16(f, 3);
    } else if (kind == 'L') {   // Gears 1: XMAWAVEFORMAT little-endian (job 994's bytes), one stream, 21156 bytes / s
        const uint8_t g1[32] = { 0x65, 0x01, 0x10, 0x00, 0xD6, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0xA4, 0x52, 0x00, 0x00,
                                 0x44, 0xAC, 0x00, 0x00, 0x72, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00 };
        f.assign(g1, g1 + 32);
        (void)channels; (void)rate; (void)samples;
    } else {   // XMAWAVEFORMAT, one stream
        be16(f, 0x165); be16(f, 16); be16(f, 0); be16(f, 0); be16(f, 1); f.push_back(0); f.push_back(0);
        be32(f, 0); be32(f, rate); be32(f, 0); be32(f, 0); f.push_back(0); f.push_back(uint8_t(channels)); be16(f, 3);
    }
    Bytes r;
    be32(r, uint32_t(f.size())); be32(r, 8); be32(r, uint32_t(packets * 2048));
    r.insert(r.end(), f.begin(), f.end());
    be32(r, 1); be32(r, 2);   // seek table
    for (size_t i = 0; i < packets * 2048; i++) r.push_back(random_packets ? rnd() : 0);
    return r;
}

// A plain package: summary, names, exports, then each wave's bulk record inside its export's serial data
struct Pkg { Bytes b; std::vector<size_t> records; };
static Pkg package(const std::vector<std::pair<std::string, Bytes>> &waves, const Bytes &noise = Bytes())
{
    Pkg p;
    Bytes &b = p.b;
    be32(b, 0x9E2A83C1u); be32(b, 575); be32(b, 0); fstr(b, "None");
    be32(b, 0);   // flags
    const size_t counts = b.size();
    for (int i = 0; i < 6; i++) be32(b, 0);
    const uint32_t name_off = uint32_t(b.size());
    std::vector<std::string> names = { "None", "SoundNodeWave", "Core" };
    for (auto &w : waves) names.push_back(w.first);
    for (auto &n : names) { fstr(b, n); be32(b, 0); be32(b, 0x70010); }
    const uint32_t export_off = uint32_t(b.size());
    std::vector<size_t> serial;
    for (size_t i = 0; i < waves.size(); i++) {   // class, super, outer, name, number, archetype, flags(8), size, offset, + 24 more
        be32(b, 0xFFFFFFFF); be32(b, 0); be32(b, 0); be32(b, uint32_t(3 + i)); be32(b, 0); be32(b, 0); be32(b, 0); be32(b, 0x70004);
        serial.push_back(b.size());
        be32(b, 0); be32(b, 0);
        for (int k = 0; k < 6; k++) be32(b, 0);
    }
    put32(b, counts + 0, uint32_t(names.size())); put32(b, counts + 4, name_off);
    put32(b, counts + 8, uint32_t(waves.size())); put32(b, counts + 12, export_off);
    put32(b, 8, uint32_t(b.size()));
    b.insert(b.end(), noise.begin(), noise.end());
    for (size_t i = 0; i < waves.size(); i++) {
        const size_t start = b.size();
        for (int k = 0; k < 37; k++) b.push_back(rnd());   // properties (unaligned on purpose)
        for (int k = 0; k < 4; k++) { be32(b, 0); be32(b, 0); be32(b, 0); be32(b, uint32_t(b.size() + 4)); }   // empty RawData / PC bulks
        be32(b, 0); be32(b, uint32_t(waves[i].second.size())); be32(b, uint32_t(waves[i].second.size())); be32(b, uint32_t(b.size() + 4));
        p.records.push_back(b.size());
        b.insert(b.end(), waves[i].second.begin(), waves[i].second.end());
        put32(b, serial[i], uint32_t(b.size() - start)); put32(b, serial[i] + 4, uint32_t(start));
    }
    return p;
}

// LZX stored blocks (from Gears2-Native/tests/layer2_prebuild_lzx_test.cpp, PR #43), framed as XMemCompress does
static Bytes lzx_stored(const Bytes &block)
{
    Bytes s;
    std::vector<bool> bits;
    auto add = [&](uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) bits.push_back((v >> i) & 1); };
    add(0, 1); add(3, 3); add(uint32_t(block.size()) >> 8, 16); add(uint32_t(block.size()) & 255, 8);
    while (bits.size() % 16) bits.push_back(false);
    for (size_t i = 0; i < bits.size(); i += 16) {
        uint16_t w = 0;
        for (size_t k = 0; k < 16; ++k) w = uint16_t(w << 1) | uint16_t(bits[i + k]);
        s.push_back(uint8_t(w)); s.push_back(uint8_t(w >> 8));
    }
    for (int i = 0; i < 3; ++i) { s.push_back(1); s.insert(s.end(), 3, 0); }
    s.insert(s.end(), block.begin(), block.end());
    Bytes framed;
    for (size_t at = 0; at < s.size(); at += 0x7000) {
        const size_t n = std::min<size_t>(0x7000, s.size() - at);
        framed.push_back(0xFF); be16(framed, 0x8000); be16(framed, uint32_t(n));
        framed.insert(framed.end(), s.begin() + at, s.begin() + at + n);
    }
    return framed;
}
// LZO1X: one literal run, then the end marker
static Bytes lzo_literal(const Bytes &block)
{
    Bytes o;
    size_t l = block.size() - 3;
    if (l <= 15) o.push_back(uint8_t(l));
    else { o.push_back(0); l -= 15; while (l > 255) { o.push_back(0); l -= 255; } o.push_back(uint8_t(l)); }
    o.insert(o.end(), block.begin(), block.end());
    o.push_back(0x11); o.push_back(0); o.push_back(0);
    return o;
}
// One chunk of 'plain' (block size 0x20000): blocks alternately LZX / stored, or all LZO with the legacy header
static Bytes chunk(const Bytes &plain, bool legacy)
{
    const uint32_t bs = 0x20000;
    std::vector<Bytes> blocks;
    for (size_t at = 0, k = 0; at < plain.size(); at += bs, k++) {
        Bytes blk(plain.begin() + at, plain.begin() + std::min(plain.size(), at + bs));
        blocks.push_back(legacy ? lzo_literal(blk) : k % 2 ? blk : lzx_stored(blk));
    }
    Bytes c;
    uint32_t tc = 0;
    for (auto &b : blocks) tc += uint32_t(b.size());
    be32(c, 0x9E2A83C1u); be32(c, legacy ? 0x9E2A83C1u : bs); be32(c, tc); be32(c, uint32_t(plain.size()));
    for (size_t k = 0; k < blocks.size(); k++) { be32(c, uint32_t(blocks[k].size())); be32(c, uint32_t(std::min<size_t>(bs, plain.size() - k * bs))); }
    for (auto &b : blocks) c.insert(c.end(), b.begin(), b.end());
    return c;
}

static void test_find(const char *what, const Bytes &file, const Pkg &plain)
{
    Bytes pkg;
    std::string why;
    CHECK(ue3_unpack(file, pkg, why));
    CHECK(pkg == plain.b);
    std::vector<Wave> ws = ue3_find_waves(pkg);
    CHECK(ws.size() == 3);
    if (ws.size() != 3) { std::printf("  %s: %zu waves\n", what, ws.size()); return; }
    CHECK(ws[0].at == plain.records[0] && ws[0].xma == 2 && ws[0].channels == 2 && ws[0].rate == 44100 && ws[0].samples == 44100 * 40);
    CHECK(ws[1].xma == 2 && ws[1].channels == 1 && ws[1].rate == 32000 && ws[1].samples == 32000 * 3);
    CHECK(ws[2].xma == 1 && ws[2].channels == 2 && ws[2].rate == 48000);
    CHECK(ws[0].name == "Menu_Theme_Loop" && ws[1].name == "UI_Click" && ws[2].name == "Old_Format");
    std::printf("  %s: %zu bytes, waves '%s' %.0f s, '%s', '%s'\n", what, file.size(), ws[0].name.c_str(), ws[0].seconds(), ws[1].name.c_str(), ws[2].name.c_str());
}

int main()
{
    // ---- finding waves ------------------------------------------------------------------------------------------
    Bytes noise;
    for (int i = 0; i < 300000; i++) noise.push_back(rnd());
    // records that must not be found: sizes not adding up, data not whole packets, a bad format
    {
        Bytes bad = xma_record(2, 2, 44100, 1000, 2, true);
        bad[11] ^= 1;   // data size off by one
        Bytes rec; be32(rec, 0); be32(rec, uint32_t(bad.size())); be32(rec, uint32_t(bad.size())); be32(rec, 0);
        noise.insert(noise.end(), rec.begin(), rec.end()); noise.insert(noise.end(), bad.begin(), bad.end());
        Bytes bad2 = xma_record(2, 2, 44100, 1000, 2, true);
        bad2[12] = 9;   // XMA2WAVEFORMAT version 9
        rec.clear(); be32(rec, 0); be32(rec, uint32_t(bad2.size())); be32(rec, uint32_t(bad2.size())); be32(rec, 0);
        noise.insert(noise.end(), rec.begin(), rec.end()); noise.insert(noise.end(), bad2.begin(), bad2.end());
    }
    Pkg plain = package({ { "Menu_Theme_Loop", xma_record(0x34, 2, 44100, 44100 * 40, 600, true) },
                          { "UI_Click", xma_record(2, 1, 32000, 32000 * 3, 12, true) },
                          { "Old_Format", xma_record(1, 2, 48000, 0, 40, true) } }, noise);
    test_find("plain", plain.b, plain);
    test_find("fully compressed (LZX + stored blocks)", chunk(plain.b, false), plain);
    test_find("fully compressed (Gears 1 LZO)", chunk(plain.b, true), plain);
    {   // summary, its chunk table (uncompressed offset / size, compressed offset / size), then one chunk
        const uint32_t head = 0x400;
        Bytes body(plain.b.begin() + head, plain.b.end());
        Bytes c = chunk(body, false);
        Bytes file(plain.b.begin(), plain.b.begin() + head);
        file.resize(0x600, 0);
        Bytes tab; be32(tab, head); be32(tab, uint32_t(body.size())); be32(tab, 0x600); be32(tab, uint32_t(c.size()));
        std::copy(tab.begin(), tab.end(), file.begin() + 0x500);
        file.insert(file.end(), c.begin(), c.end());
        Bytes pkg; std::string why;
        CHECK(ue3_unpack(file, pkg, why) && pkg.size() == plain.b.size() &&
              std::equal(pkg.begin() + head, pkg.end(), plain.b.begin() + head));
        CHECK(ue3_find_waves(pkg).size() == 3);
    }
    {   // damaged: a block that doesn't decode; not a package; nothing in noise alone
        Bytes c = chunk(plain.b, false), pkg;
        const size_t nb = (plain.b.size() + 0x1FFFF) / 0x20000;
        c[16 + 8 * nb + 5 + 1] ^= 0xFF;   // the first block's LZX header (after its frame header)
        std::string why;
        CHECK(!ue3_unpack(c, pkg, why) && pkg.empty());
        Bytes c2 = chunk(plain.b, false);
        c2[16 + 3] ^= 0x01;   // a block size in the table: the totals don't add up
        CHECK(!ue3_unpack(c2, pkg, why) && why.find("damaged") != std::string::npos);
        CHECK(!ue3_unpack(noise, pkg, why));
        CHECK(ue3_find_waves(noise).empty());
    }
    {   // Gears 1's little-endian XMA1 format
        Pkg g1 = package({ { "G1_Music", xma_record('L', 0, 0, 0, 463, true) } });
        std::vector<Wave> ws = ue3_find_waves(g1.b);
        CHECK(ws.size() == 1 && ws[0].xma == 1 && ws[0].le && ws[0].channels == 2 && ws[0].rate == 44100 &&
              ws[0].samples == uint32_t(uint64_t(463 * 2048) * 44100 / 21156) && ws[0].name == "G1_Music");
        if (!ws.empty()) std::printf("  Gears 1 format: %d ch %d Hz %.1f s\n", ws[0].channels, ws[0].rate, ws[0].seconds());
        std::vector<int16_t> pcm;
        std::string why;
        CHECK(ws.size() == 1 && !xma_decode(g1.b, ws[0], pcm, why));   // random packets: refused
        std::printf("  ... random packets: refused (%s)\n", why.c_str());
    }
    {   // trimming
        std::vector<int16_t> p = { 0, 0, 10, -10, 63, 0, 500, 0, 1, 1 };
        trim_leading_silence(p);
        CHECK(p.size() == 4 && p[0] == 500);
    }

    // ---- the decoder refuses what isn't XMA ---------------------------------------------------------------------
    std::string why;
    if (!xma_available(&why)) { std::printf("FAIL: no FFmpeg (%s)\n", why.c_str()); g_fail++; }
    else {
        std::vector<Wave> ws = ue3_find_waves(plain.b);
        std::vector<int16_t> pcm;
        for (const Wave &w : ws) {
            const bool ok = xma_decode(plain.b, w, pcm, why);
            CHECK(!ok && pcm.empty());
            std::printf("  random packets as '%s': refused (%s)\n", w.name.c_str(), why.c_str());
        }
        Pkg zero = package({ { "Zeros", xma_record(2, 2, 44100, 44100 * 30, 300, false) } });
        ws = ue3_find_waves(zero.b);
        CHECK(ws.size() == 1 && !xma_decode(zero.b, ws[0], pcm, why));
        std::printf("  zero packets: refused (%s)\n", why.c_str());
        // cancelling
        bool (*always)(void *) = [](void *) { return true; };
        CHECK(!xma_decode(zero.b, ws[0], pcm, why, always, nullptr) && why == "cancelled");
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
