// COGBox - the games' own sound for the library's music (see gamesound.h)
#include "gamesound.h"

#include "util.h"
#include "x360fs.h"

#include <ue3/prebuild_lzx.h>   // Gears2-Native/src/layer2_ue3 (MIT, ours): XMemCompress LZX

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>   // (the Linux unit test, test/gamesound_test.cpp)
#endif

#ifndef COGBOX_NO_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}
#endif

namespace gamesound {
namespace {

uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint16_t le16(const uint8_t *p) { return uint16_t(p[1] << 8 | p[0]); }
uint32_t le32(const uint8_t *p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }

const uint32_t kTag = 0x9E2A83C1u;
const size_t kMaxPackage = size_t(384) << 20;   // the largest Gears package with sound is ~20 MB packed

// ---- chunk decoders ----------------------------------------------------------------------------------------------

// One block: XMemCompress frames ("FF uu uu cc cc" or "cc cc"), their payload one LZX stream (128 KB window).
bool lzx_block(const uint8_t *in, size_t n, uint8_t *out, size_t out_size, std::vector<uint8_t> &scratch)
{
    scratch.clear();
    size_t p = 0;
    while (p + 2 <= n) {
        size_t c;
        if (in[p] == 0xFF) {
            if (p + 5 > n) break;
            c = be16(in + p + 3);
            p += 5;
        } else {
            c = be16(in + p);
            p += 2;
        }
        if (!c || p + c > n) break;
        scratch.insert(scratch.end(), in + p, in + p + c);
        p += c;
    }
    return !scratch.empty() && ue3::prebuild::PrebuildLzxDecode(scratch.data(), scratch.size(), out, out_size, 17);
}

// Gears 1's blocks: LZO1X (from Gears2-Native/src/layer2_ue3/shader_prebuild.cpp Lzo1x, ours, MIT; checked there on
// SP_Prison_S02.xxx: 93 of 93 blocks to their stated size).
bool lzo1x(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    size_t ip = 0, op = 0;
    auto need = [&](size_t k) { return ip + k <= in_len; };
    auto lit = [&](size_t k) {
        if (!need(k) || op + k > out_len) return false;
        std::memcpy(out + op, in + ip, k); ip += k; op += k; return true;
    };
    auto copy = [&](size_t dist, size_t len) {
        if (dist == 0 || dist > op || op + len > out_len) return false;
        for (size_t k = 0; k < len; ++k, ++op) out[op] = out[op - dist];
        return true;
    };
    auto run = [&](size_t base, size_t& len) {
        len = base;
        while (need(1) && in[ip] == 0) { len += 255; ++ip; }
        if (!need(1)) return false;
        len += in[ip++];
        return true;
    };
    if (!need(1)) return false;
    int state = 0, t = -1;
    if (in[0] > 17) {
        ip = 1;
        const size_t k = in[0] - 17;
        if (!lit(k)) return false;
        state = k >= 4 ? 4 : int(k);
    }
    for (;;) {
        if (t < 0) { if (!need(1)) return false; t = in[ip++]; }
        if (t < 16) {
            if (state == 0) {
                size_t len = size_t(t);
                if (t == 0 && !run(15, len)) return false;
                if (!lit(len + 3)) return false;
                state = 4;
                if (!need(1)) return false;
                t = in[ip++];
                if (t >= 16) continue;
                if (!need(1)) return false;
                const size_t dist = 1 + 0x0800 + (size_t(t) >> 2) + (size_t(in[ip++]) << 2);
                if (!copy(dist, 3)) return false;
            } else {
                if (!need(1)) return false;
                const size_t dist = (state < 4 ? 1 : 1 + 0x0800) + (size_t(t) >> 2) + (size_t(in[ip++]) << 2);
                if (!copy(dist, state < 4 ? 2 : 3)) return false;
            }
            state = t & 3;
            if (!lit(size_t(state))) return false;
            t = -1;
            continue;
        }
        size_t len, dist;
        int trailing;
        if (t >= 64) {
            if (!need(1)) return false;
            dist = 1 + ((size_t(t) >> 2) & 7) + (size_t(in[ip++]) << 3);
            len = (size_t(t) >> 5) + 1;
            trailing = t & 3;
        } else if (t >= 32) {
            len = size_t(t) & 31;
            if (len == 0 && !run(31, len)) return false;
            len += 2;
            if (!need(2)) return false;
            const size_t d = in[ip] | (size_t(in[ip + 1]) << 8);
            ip += 2;
            dist = 1 + (d >> 2);
            trailing = int(d & 3);
        } else {
            len = size_t(t) & 7;
            if (len == 0 && !run(7, len)) return false;
            len += 2;
            if (!need(2)) return false;
            const size_t d = in[ip] | (size_t(in[ip + 1]) << 8);
            ip += 2;
            dist = ((size_t(t) & 8) << 11) + (d >> 2);
            trailing = int(d & 3);
            if (dist == 0) return op == out_len;
            dist += 0x4000;
        }
        if (!copy(dist, len)) return false;
        state = trailing;
        if (!lit(size_t(trailing))) return false;
        t = -1;
    }
}

struct Chunk { size_t at = 0, len = 0; uint32_t total_u = 0; };

// A chunk at b[i] (header checked: totals add up), or false.
bool chunk_at(const std::vector<uint8_t> &b, size_t i, Chunk &c, bool &legacy, uint32_t &bsz)
{
    const size_t n = b.size();
    if (i + 24 > n || be32(&b[i]) != kTag) return false;
    bsz = be32(&b[i + 4]);
    legacy = bsz == kTag;
    if (legacy) bsz = 0x20000;
    const uint32_t tc = be32(&b[i + 8]), tu = be32(&b[i + 12]);
    if (bsz < 0x8000 || bsz > 0x100000 || (bsz & (bsz - 1)) || !tu || tu > kMaxPackage || !tc || tc > n) return false;
    const uint32_t nb = (tu + bsz - 1) / bsz;
    if (i + 16 + size_t(nb) * 8 + tc > n) return false;
    uint64_t sc = 0, su = 0;
    for (uint32_t k = 0; k < nb; ++k) { sc += be32(&b[i + 16 + k * 8]); su += be32(&b[i + 20 + k * 8]); }
    if (sc != tc || su != tu) return false;
    c.at = i; c.len = 16 + size_t(nb) * 8 + tc; c.total_u = tu;
    return true;
}

bool decode_chunk(const std::vector<uint8_t> &b, const Chunk &c, uint8_t *out, std::vector<uint8_t> &scratch)
{
    bool legacy;
    uint32_t bsz;
    Chunk again;
    if (!chunk_at(b, c.at, again, legacy, bsz)) return false;
    const uint32_t nb = (c.total_u + bsz - 1) / bsz;
    const uint8_t *tab = &b[c.at + 16], *data = tab + size_t(nb) * 8;
    size_t w = 0;
    for (uint32_t k = 0; k < nb; ++k) {
        const uint32_t cs = be32(tab + k * 8), us = be32(tab + k * 8 + 4);
        if (us > bsz || w + us > c.total_u) return false;
        if (cs == us) std::memcpy(out + w, data, us);   // a block stored as it is
        else if (!(legacy ? lzo1x(data, cs, out + w, us) : lzx_block(data, cs, out + w, us, scratch))) return false;
        data += cs;
        w += us;
    }
    return w == c.total_u;
}

// ---- package summary: names and exports, for the waves' names --------------------------------------------------

struct Summary { uint32_t names = 0, name_off = 0, exports = 0, export_off = 0; };

bool read_fstring(const std::vector<uint8_t> &b, size_t &p, std::string *s)
{
    if (p + 4 > b.size()) return false;
    int32_t len = int32_t(be32(&b[p]));
    p += 4;
    if (len > 1024 || len < -1024) return false;
    if (len >= 0) {
        if (p + size_t(len) > b.size()) return false;
        if (s) s->assign(reinterpret_cast<const char *>(&b[p]), len ? size_t(len) - 1 : 0);
        p += size_t(len);
    } else {
        const size_t k = size_t(-len);
        if (p + k * 2 > b.size()) return false;
        if (s) { s->clear(); for (size_t i = 0; i + 1 < k; i++) s->push_back(char(be16(&b[p + i * 2]) < 128 ? be16(&b[p + i * 2]) : '?')); }
        p += k * 2;
    }
    return true;
}

bool read_summary(const std::vector<uint8_t> &b, Summary &s)
{
    if (b.size() < 64 || be32(&b[0]) != kTag) return false;
    size_t p = 12;
    if (!read_fstring(b, p, nullptr) || p + 20 > b.size()) return false;
    s.names = be32(&b[p + 4]); s.name_off = be32(&b[p + 8]);
    s.exports = be32(&b[p + 12]); s.export_off = be32(&b[p + 16]);
    return s.names && s.names < 2000000 && s.name_off < b.size() && s.export_off < b.size() && s.exports < 2000000;
}

std::vector<std::string> read_names(const std::vector<uint8_t> &b, const Summary &s)
{
    std::vector<std::string> out;
    size_t p = s.name_off;
    for (uint32_t i = 0; i < s.names; i++) {
        std::string n;
        if (!read_fstring(b, p, &n) || p + 8 > b.size()) { out.clear(); break; }
        p += 8;   // the name's object flags
        out.push_back(n);
    }
    return out;
}

// ---- FFmpeg (loaded at run time) ---------------------------------------------------------------------------------

#ifndef COGBOX_NO_FFMPEG
struct Ff {
    bool tried = false, ok = false;
    std::string why;
    unsigned (*avcodec_version)(void);
    const AVCodec *(*avcodec_find_decoder)(enum AVCodecID);
    AVCodecContext *(*avcodec_alloc_context3)(const AVCodec *);
    int (*avcodec_open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
    int (*avcodec_send_packet)(AVCodecContext *, const AVPacket *);
    int (*avcodec_receive_frame)(AVCodecContext *, AVFrame *);
    void (*avcodec_free_context)(AVCodecContext **);
    AVPacket *(*av_packet_alloc)(void);
    void (*av_packet_free)(AVPacket **);
    AVFrame *(*av_frame_alloc)(void);
    void (*av_frame_free)(AVFrame **);
    void (*av_frame_unref)(AVFrame *);
    void *(*av_mallocz)(size_t);
    void (*av_channel_layout_default)(AVChannelLayout *, int);
    void (*av_log_set_level)(int);
};
Ff g_ff;
std::mutex g_ffLock;

#ifdef _WIN32
using Lib = HMODULE;
Lib open_lib(const char *stem, int major)
{
    const std::wstring p = exe_dir_w() + L"\\" + widen(stem) + L"-" + std::to_wstring(major) + L".dll";
    return LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}
void *lib_sym(Lib m, const char *name) { return reinterpret_cast<void *>(GetProcAddress(m, name)); }
#else
using Lib = void *;
Lib open_lib(const char *stem, int major)
{
    const std::string p = exe_dir() + "/lib" + stem + ".so." + std::to_string(major);
    return dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
}
void *lib_sym(Lib m, const char *name) { return dlsym(m, name); }
#endif
template <class T> bool sym(Lib m, const char *name, T &f)
{
    f = reinterpret_cast<T>(lib_sym(m, name));
    return f != nullptr;
}

bool ff_load()
{
    std::lock_guard<std::mutex> l(g_ffLock);
    if (g_ff.tried) return g_ff.ok;
    g_ff.tried = true;
    Lib u = open_lib("avutil", LIBAVUTIL_VERSION_MAJOR);
    Lib c = u ? open_lib("avcodec", LIBAVCODEC_VERSION_MAJOR) : nullptr;
    if (!c) { g_ff.why = "avcodec-" + std::to_string(LIBAVCODEC_VERSION_MAJOR) + ".dll / avutil-" + std::to_string(LIBAVUTIL_VERSION_MAJOR) + ".dll are not beside CogBox.exe"; return false; }
    Ff &f = g_ff;
    bool all = sym(c, "avcodec_version", f.avcodec_version) && sym(c, "avcodec_find_decoder", f.avcodec_find_decoder) &&
               sym(c, "avcodec_alloc_context3", f.avcodec_alloc_context3) && sym(c, "avcodec_open2", f.avcodec_open2) &&
               sym(c, "avcodec_send_packet", f.avcodec_send_packet) && sym(c, "avcodec_receive_frame", f.avcodec_receive_frame) &&
               sym(c, "avcodec_free_context", f.avcodec_free_context) && sym(c, "av_packet_alloc", f.av_packet_alloc) &&
               sym(c, "av_packet_free", f.av_packet_free) && sym(u, "av_frame_alloc", f.av_frame_alloc) &&
               sym(u, "av_frame_free", f.av_frame_free) && sym(u, "av_frame_unref", f.av_frame_unref) &&
               sym(u, "av_mallocz", f.av_mallocz) && sym(u, "av_channel_layout_default", f.av_channel_layout_default) &&
               sym(u, "av_log_set_level", f.av_log_set_level);
    if (!all) { g_ff.why = "the FFmpeg DLLs beside CogBox.exe are missing functions"; return false; }
    if ((f.avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR) { g_ff.why = "the FFmpeg DLLs are another version"; return false; }
    if (!f.avcodec_find_decoder(AV_CODEC_ID_XMA2) || !f.avcodec_find_decoder(AV_CODEC_ID_XMA1)) { g_ff.why = "the FFmpeg DLLs have no XMA decoder"; return false; }
    f.av_log_set_level(AV_LOG_QUIET);
    g_ff.ok = true;
    return true;
}
#endif

}  // namespace

// ---- reading -----------------------------------------------------------------------------------------------------

bool read_copy_file(const std::string &copy, const std::string &rel, std::vector<uint8_t> &out, size_t max)
{
    out.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::u8path(copy);
    if (fs::is_directory(base, ec)) {
        std::string r = rel;
        std::replace(r.begin(), r.end(), '/', '\\');
        const std::string p = join(copy, r);
        const uint64_t sz = file_size(p);
        if (!sz || sz > max) return false;
        std::string s;
        if (!read_file(p, s, size_t(sz) + 1) || s.size() != sz) return false;
        out.assign(s.begin(), s.end());
        return true;
    }
    uint64_t sz = 0;
    std::vector<uint8_t> probe;
    if (!x360fs::image_read_file(base, rel, 0, 0, probe, &sz) || !sz || sz > max) return false;
    return x360fs::image_read_file(base, rel, 0, size_t(sz), out, nullptr) && out.size() == sz;
}

bool ue3_unpack(const std::vector<uint8_t> &b, std::vector<uint8_t> &out, std::string &why)
{
    out.clear();
    if (b.size() < 32 || be32(&b[0]) != kTag) { why = "not a UE3 package"; return false; }
    std::vector<uint8_t> scratch;
    Chunk c;
    bool legacy;
    uint32_t bsz;
    // fully compressed: the file is chunks back to back from byte 0
    if (chunk_at(b, 0, c, legacy, bsz)) {
        size_t at = 0;
        while (at < b.size() && chunk_at(b, at, c, legacy, bsz)) {
            if (out.size() + c.total_u > kMaxPackage) { why = "package too large"; out.clear(); return false; }
            const size_t w = out.size();
            out.resize(w + c.total_u);
            if (!decode_chunk(b, c, &out[w], scratch)) { why = "a compressed block didn't decode"; out.clear(); return false; }
            at += c.len;
        }
        if (out.size() < 32 || be32(&out[0]) != kTag) { why = "the decompressed data is not a package"; out.clear(); return false; }
        return true;
    }
    const uint32_t v = be32(&b[4]);   // a package's version there; a block size (or the tag again): chunks that don't add up
    if (v == kTag || (v >= 0x8000 && v <= 0x100000 && !(v & (v - 1)))) { why = "the compressed package is damaged"; return false; }
    // a plain summary, then chunks: each goes where the summary's chunk table (uncompressed offset, uncompressed size,
    // compressed offset, compressed size) puts it; the table is found by the chunk's own file offset and size
    std::vector<Chunk> chunks;
    for (size_t i = 16; i + 24 <= b.size(); i++)
        if (b[i] == 0x9E && chunk_at(b, i, c, legacy, bsz)) { chunks.push_back(c); i += c.len - 1; }
    if (chunks.empty()) { out = b; return true; }   // stored plain
    const size_t head = chunks[0].at;
    out.assign(b.begin(), b.begin() + head);
    size_t next = head;
    for (const Chunk &k : chunks) {
        size_t uoff = next;
        for (size_t j = 8; j + 8 <= head; j += 4)
            if (be32(&b[j]) == k.at && be32(&b[j + 4]) == k.len && be32(&b[j - 4]) == k.total_u) { uoff = be32(&b[j - 8]); break; }
        if (uoff + k.total_u > kMaxPackage) { why = "package too large"; out.clear(); return false; }
        if (out.size() < uoff + k.total_u) out.resize(uoff + k.total_u, 0);
        if (!decode_chunk(b, k, &out[uoff], scratch)) { why = "a compressed block didn't decode"; out.clear(); return false; }
        next = uoff + k.total_u;
    }
    return true;
}

std::vector<Wave> ue3_find_waves(const std::vector<uint8_t> &b)
{
    std::vector<Wave> out;
    const size_t n = b.size();
    for (size_t i = 0; i + 28 + 0x20 <= n; i++) {
        // bulk data record: flags, element count == size on disk, offset; then UE3's XMA layout
        if (b[i + 4] != b[i + 8] || be32(&b[i + 4]) != be32(&b[i + 8])) continue;
        const uint32_t count = be32(&b[i + 4]);
        if (count < 12 + 0x20 + 2048 || count > n - i - 16 || be32(&b[i]) > 0xFFFF) continue;
        const uint8_t *r = &b[i + 16];
        const uint32_t cs = be32(r), ss = be32(r + 4), ds = be32(r + 8);
        if (cs < 0x20 || cs > 0x400 || ds < 2048 || ds % 2048 || ss > count || uint64_t(12) + cs + ss + ds != count) continue;
        const uint8_t *f = r + 12;
        Wave w;
        w.at = i + 16;
        w.size = count;
        if (cs == 0x34 && be16(f) == 0x166 && be16(f + 0x10) == 34) {   // WAVEFORMATEX + XMA2 extension
            w.channels = be16(f + 2); w.rate = int(be32(f + 4)); w.samples = be32(f + 0x12 + 6);
        } else if ((f[0] == 3 || f[0] == 4) && f[1] >= 1 && f[1] <= 8 && cs == 32u + (f[0] == 3 ? 0 : 8) + 4u * f[1]) {   // XMA2WAVEFORMAT
            const size_t so = f[0] == 3 ? 0x20 : 0x28;
            for (int s = 0; s < f[1]; s++) w.channels += f[so + s * 4];
            w.rate = int(be32(f + 0x0c)); w.samples = be32(f + (f[0] == 3 ? 0x14 : 0x1c));
        } else if (be16(f) == 0x165 && be16(f + 8) >= 1 && be16(f + 8) <= 8 && cs == 12u + 20u * be16(f + 8)) {   // XMAWAVEFORMAT
            w.xma = 1;
            for (int s = 0; s < be16(f + 8); s++) w.channels += f[12 + s * 20 + 17];
            w.rate = int(be32(f + 12 + 4));
            const uint32_t bps = be32(f + 12);   // the first stream's bytes per second: the length (XMA1 states none)
            w.samples = bps ? uint32_t(uint64_t(ds) * uint32_t(w.rate) / bps) : 0;
        } else if (le16(f) == 0x165 && le16(f + 8) >= 1 && le16(f + 8) <= 8 && cs == 12u + 20u * le16(f + 8)) {
            // Gears 1 (2006, job 994): the same UE3 sizes (big-endian), but the XMAWAVEFORMAT little-endian, as in a RIFF file
            w.xma = 1;
            w.le = true;
            for (int s = 0; s < le16(f + 8); s++) w.channels += f[12 + s * 20 + 17];
            w.rate = int(le32(f + 12 + 4));
            const uint32_t bps = le32(f + 12);
            w.samples = bps ? uint32_t(uint64_t(ds) * uint32_t(w.rate) / bps) : 0;
        } else continue;
        if (w.channels < 1 || w.channels > 8 || w.rate < 8000 || w.rate > 96000) continue;
        if (!w.samples) w.samples = uint32_t(uint64_t(ds / 2048) * 2048 * 8 / 2 / std::max(1, w.channels) / 3);   // rough (XMA1 gives none)
        out.push_back(w);
        i += 16 + count - 1;
    }
    // names, from the exports whose serial data hold each wave (best effort: the summary layout differs by game)
    Summary s;
    if (out.empty() || !read_summary(b, s)) return out;
    std::vector<std::string> names = read_names(b, s);
    if (names.empty()) return out;
    const size_t end = std::min(n, size_t(s.export_off) + size_t(s.exports) * 200);
    for (Wave &w : out) {   // the smallest export holding the wave (its serial data come after the tables)
        uint32_t best = UINT32_MAX;
        for (size_t j = size_t(s.export_off) + 20; j + 8 <= end; j += 4) {
            const uint32_t sz = be32(&b[j]), off = be32(&b[j + 4]);
            if (off > s.export_off && off <= w.at && uint64_t(off) + sz >= w.at + w.size && sz < best && be32(&b[j - 20]) < names.size()) {
                w.name = names[be32(&b[j - 20])];
                best = sz;
            }
        }
    }
    return out;
}

bool xma_available(std::string *why)
{
#ifdef COGBOX_NO_FFMPEG
    if (why) *why = "this COGBox was built without FFmpeg";
    return false;
#else
    const bool ok = ff_load();
    if (why) *why = g_ff.why;
    return ok;
#endif
}

bool xma_decode(const std::vector<uint8_t> &b, const Wave &w, std::vector<int16_t> &out, std::string &why,
                bool (*cancel)(void *), void *ctx)
{
    out.clear();
#ifdef COGBOX_NO_FFMPEG
    (void)b; (void)w; (void)cancel; (void)ctx;
    why = "this COGBox was built without FFmpeg";
    return false;
#else
    if (!ff_load()) { why = g_ff.why; return false; }
    if (w.at + w.size > b.size()) { why = "wave outside the package"; return false; }
    if (w.rate > 0 && w.samples > uint32_t(w.rate) * 480u) { why = "longer than 8 minutes"; return false; }
    const uint8_t *r = &b[w.at];
    const uint32_t cs = be32(r), ss = be32(r + 4), ds = be32(r + 8);
    const uint8_t *f = r + 12, *data = r + 12 + cs + ss;
    // the decoder's configuration (FFmpeg wmaprodec.c xma_decode_init): XMA2WAVEFORMAT as it is (only bytes are read),
    // the 34-byte XMA2WAVEFORMATEX little-endian, or XMAWAVEFORMAT from its fifth byte with the stream count as a byte
    std::vector<uint8_t> ex;
    AVCodecID id = AV_CODEC_ID_XMA2;
    if (w.xma == 1) {
        id = AV_CODEC_ID_XMA1;
        ex.assign(f + 4, f + cs);
        ex[4] = uint8_t(w.le ? le16(f + 8) : be16(f + 8)); ex[5] = 0;
    } else if (cs == 0x34) {
        ex.assign(34, 0);
        const uint8_t *e = f + 0x12;
        ex[0] = uint8_t((w.channels + 1) / 2);   // streams: 2 channels each, the last 1 or 2
        for (int k = 0; k < 4; k++) ex[6 + k] = e[9 - k];   // samples encoded
        ex[10] = 0; ex[11] = 0; ex[12] = 1; ex[13] = 0;    // bytes per block 0x10000
    } else ex.assign(f, f + cs);

    Ff &F = g_ff;
    const AVCodec *codec = F.avcodec_find_decoder(id);
    AVCodecContext *c = codec ? F.avcodec_alloc_context3(codec) : nullptr;
    if (!c) { why = "no decoder"; return false; }
    c->sample_rate = w.rate;
    F.av_channel_layout_default(&c->ch_layout, w.channels);
    c->extradata = static_cast<uint8_t *>(F.av_mallocz(ex.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (c->extradata) { std::memcpy(c->extradata, ex.data(), ex.size()); c->extradata_size = int(ex.size()); }
    AVPacket *pkt = F.av_packet_alloc();
    AVFrame *fr = F.av_frame_alloc();
    std::vector<uint8_t> pbuf(2048 + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::vector<float> lr;   // decoded, stereo float at the wave's rate
    lr.reserve(size_t(w.samples ? w.samples : 1) * 2 + 8192);
    const size_t packets = ds / 2048;
    size_t bad = 0;
    bool ok = c->extradata && pkt && fr && F.avcodec_open2(c, codec, nullptr) >= 0;
    if (!ok) why = "the decoder refused the wave's format";
    auto drain = [&]() {
        while (F.avcodec_receive_frame(c, fr) >= 0) {
            const int ch = fr->ch_layout.nb_channels;
            if (fr->format == AV_SAMPLE_FMT_FLTP && ch >= 1) {
                const float *L = reinterpret_cast<const float *>(fr->extended_data[0]);
                const float *R = reinterpret_cast<const float *>(fr->extended_data[ch > 1 ? 1 : 0]);
                for (int k = 0; k < fr->nb_samples; k++) { lr.push_back(L[k]); lr.push_back(R[k]); }
            }
            F.av_frame_unref(fr);
        }
    };
    for (size_t k = 0; ok && k <= packets; k++) {
        if (cancel && (k & 63) == 0 && cancel(ctx)) { ok = false; why = "cancelled"; break; }
        if (k < packets) {
            std::memcpy(pbuf.data(), data + k * 2048, 2048);
            pkt->data = pbuf.data();
            pkt->size = 2048;
        } else { pkt->data = nullptr; pkt->size = 0; }   // the end: what the decoder holds back
        if (F.avcodec_send_packet(c, pkt) < 0) bad++;
        drain();
        if (lr.size() > size_t(w.rate) * 2 * 480) { ok = false; why = "longer than 8 minutes"; }   // (memory: review of 0004)
    }
    F.av_frame_free(&fr);
    F.av_packet_free(&pkt);
    F.avcodec_free_context(&c);
    if (!ok) return false;
    // checks: most packets decoded, the length is what the format says, and it is sound, not silence or noise at full scale
    const size_t frames = lr.size() / 2;
    if (bad * 10 > packets) { why = "too many packets failed to decode (" + std::to_string(bad) + " of " + std::to_string(packets) + ")"; return false; }
    if (frames < size_t(w.rate)) { why = "under a second of sound"; return false; }
    if (w.xma == 2 && w.samples && (frames < w.samples * 0.9 || frames > w.samples * 1.1 + 4096)) {
        why = "decoded " + std::to_string(frames) + " samples, the format says " + std::to_string(w.samples);
        return false;
    }
    double sum = 0;
    size_t clip = 0;
    for (float v : lr) { sum += double(v) * v; if (std::fabs(v) >= 0.999f) clip++; }
    const double rms = std::sqrt(sum / double(lr.size()));
    if (rms < 1e-4) { why = "silent"; return false; }
    if (rms > 0.7 || clip * 20 > lr.size()) { why = "not music (full-scale noise)"; return false; }
    // to 16-bit stereo at 44.1 kHz (linear interpolation when the rate differs)
    const size_t m = size_t(double(frames) * 44100.0 / w.rate);
    out.resize(m * 2);
    for (size_t k = 0; k < m; k++) {
        const double src = double(k) * w.rate / 44100.0;
        const size_t a = std::min(frames - 1, size_t(src)), z = std::min(frames - 1, a + 1);
        const float t = float(src - double(a));
        for (int ch = 0; ch < 2; ch++) {
            const float v = lr[a * 2 + ch] * (1 - t) + lr[z * 2 + ch] * t;
            out[k * 2 + ch] = int16_t(std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f));
        }
    }
    return true;
#endif
}

void trim_leading_silence(std::vector<int16_t> &pcm)
{
    size_t first = 0;
    const size_t n = pcm.size() / 2;
    while (first < n && std::abs(pcm[first * 2]) < 64 && std::abs(pcm[first * 2 + 1]) < 64) first++;
    if (first > 0 && first < n) pcm.erase(pcm.begin(), pcm.begin() + first * 2);
}

}  // namespace gamesound
