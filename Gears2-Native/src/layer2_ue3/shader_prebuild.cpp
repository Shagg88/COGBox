// Layer 2 (Unreal Engine 3, both games): patch 0068 - shaders built while the game loads an area (Shagg: "if it's able
// to build behind the actual loading screen"). Our code (MIT).
//
// The renderer meets an area's shaders at its first draws: on a first visit ~350 new pipelines at 15-35 ms each in
// the driver (job 358), which the 0063 hold covers with a 2-3 s pause. But the shaders are in the game's own files,
// and the game opens those files seconds before the first draw. So when the game opens a package (SDK patch 32's
// file-open observer), a background thread here reads the same file from disk, unpacks it and finds the shader
// containers, and hands each pixel shader's microcode to the renderer, which translates it exactly as a draw would
// and has the driver compile it in a warm-only pipeline (layer 0, xenos/dev_tools.h XenosPrebuildPixelShaders). The
// driver keeps each compiled stage (job 362c [V]), so the real pipelines later cost ~0.1 ms. Nothing is written to
// disk and nothing ships: every player's own game files are the source.
//
// Package layout (UE3 on the 360; found with the Gears 3 chat, jobs 365 / 366 and its jobs 004 / 006):
// - Compressed chunks: tag 0x9E2A83C1, block size (a power of two, 0x20000 here), compressed and uncompressed totals,
//   then per block [compressed size][uncompressed size], then the blocks. Each block is XMemCompress-framed LZX
//   (frames "FF uu uu cc cc" or "cc cc"; window 128 KB). Found by scanning for the tag with its totals checked, so the
//   per-game package summary layout does not matter.
// - Shader containers: magic 0x102A1100 (pixel) / 0x102A1101 (vertex), [1] offset to the microcode, [2] its size.
//   Gears 2: each container follows a TArray count (offset + size). Gears 3: inside SeekFreeShaderCache, in inner
//   XMemCompress blocks ([u32 uncompressed][u32 compressed] then frames), packed back to back.
// - Hash key: XXH3-64 of the big-endian microcode, the same key as the renderer's.
// Pixel shaders only: the XDK patches vertex shaders at bind, so 0 of 218 drawn vertex shaders match the files (job 367
// [V]); ~70% of drawn pixel shaders do.
// LZX: decoder from PR #43 (legoliamneeson), ue3/prebuild_lzx.h, in place of the SDK's lzx_decompress (libmspack),
// which lacks the "no pad after an odd uncompressed block" fix, so the blocks that needed it were skipped. It tries the
// XMemCompress form first and the CAB-style pad only as a second attempt; the log line counts what each fix added.
// Also from PR #43: the bytes between and around compressed chunks are scanned for containers too, and a fully
// compressed file of which no chunk decoded is logged as unreadable instead of passing as a package without shaders.
//
// 0.6.3 (player report: Gears 3 on a 4-core / 8-thread laptop stuttered for minutes - two readers decompressing 4 GB of
// packages beside the game on every launch, since nothing of the reading was kept):
// - Done-list (<shader cache>\prebuild-done.bin): a package read to the end without a damaged chunk is remembered by
//   path, size and modification time, with exactly what the renderer took from it - with a recipe pack only the pack's
//   pixel shaders and the vertex-shader regions that rebuild one of its vertex shaders (their bytes kept once each, a
//   few MB). At the next start such a package is not read: those shaders are handed over from the list (instant), the
//   same as reading it would. The list is valid for one recipe pack (its file's hash), one build of this reader and of
//   the renderer's matching, and one format; any change starts it again. Without a recipe pack only packages that
//   offered nothing are listed. A changed, damaged or unreadable file is always read again.
//   --ue3_shader_prebuild_done_list=false: as before.
// - On 8 usable logical processors or fewer (the process's affinity mask, XenosUsableProcessors): one reader thread,
//   and while the game is being played (frames of 64+ draws, xenos/dev_tools.h XenosRendererInPlay) the start-up list
//   is paced to a quarter of one thread (--ue3_shader_prebuild_play_pace), all at the lowest priority; packages the
//   game opens itself (the area being loaded) are never paced, and a paced read in progress when the game opens one
//   finishes at below-normal priority. A rest ends when play does: loading screens, menus and videos read at full
//   speed. Bigger CPUs: as before unless the cvars say otherwise.
#include <ue3/shader_prebuild.h>

#include <ue3/prebuild_lzx.h>
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/hash.h>
#include <rex/logging/macros.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <utility>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(ue3_shader_prebuild, false, "Gears2",
                    "Patch 0068: when the game opens a package, read its pixel shaders from the file and have our "
                    "renderer build them before their first draw (needs SDK patch 32 and the DXBC shader path). "
                    "Patch 0069: off by default until it measurably shortens first-visit holds (job 368b: 97% of a "
                    "cold area's new pixel shaders were in the opened packages, but ~10,000 were offered per load, so "
                    "most were not built in time, and building them doubled a cold ch10's frame time)");

REXCVAR_DEFINE_STRING(ue3_shader_prebuild_at_start, "", "Gears2",
                      "Job 630 (Shagg 19:05: build the shipped recipes during the opening videos): folders (';' between "
                      "them - the game's data and update folders) whose packages are all read at start-up, shader caches "
                      "first, instead of waiting for the game to open each one; with a recipe pack only its shaders are "
                      "built. Same low-priority reader thread. Empty = only the packages the game opens");

REXCVAR_DEFINE_INT32(ue3_shader_prebuild_threads, 0, "Gears2",
                     "Job 632: reader threads with --ue3_shader_prebuild_at_start (0 = auto: 1 on 8 logical processors or "
                     "fewer, 2 on up to 12, else a quarter of them, 2 to 4): the videos and menus leave the CPU idle; all "
                     "at the lowest priority, so the game still comes first");
REXCVAR_DEFINE_INT32(ue3_shader_prebuild_play_pace, -1, "Gears2",
                     "0.6.3: while the game is being played, how much of each reader thread's time goes to the start-up "
                     "list (percent; it reads a file, then rests 3x as long at 25). Packages the game opens itself are "
                     "never paced; loading screens, menus and videos read at full speed. -1 = auto (25 on 8 logical "
                     "processors or fewer; else 50 - 0.6.5, was 100)");
REXCVAR_DEFINE_BOOL(ue3_shader_prebuild_done_list, true, "Gears2",
                    "0.6.3: remember packages read to the end (<shader cache>\\prebuild-done.bin: path, size, date, and "
                    "the recipe pack's shaders found in them) so the next start does not read them again; a changed or "
                    "damaged file is read again. false = every package is read at every start, as before");

namespace {

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

// One XMemCompress-framed LZX block into out (out_size bytes). True = decoded; *padded = only with the CAB-style pad.
bool Lzx(const uint8_t* in, size_t n, uint8_t* out, size_t out_size, std::vector<uint8_t>& scratch, bool* padded) {
  *padded = false;
  scratch.clear();
  size_t p = 0;
  while (p + 2 <= n) {
    size_t csz;
    if (in[p] == 0xFF) {
      if (p + 5 > n) break;
      csz = Be16(in + p + 3);
      p += 5;
    } else {
      csz = Be16(in + p);
      p += 2;
    }
    if (!csz || p + csz > n) break;
    scratch.insert(scratch.end(), in + p, in + p + csz);
    p += csz;
  }
  if (scratch.empty()) return false;
  return ue3::prebuild::PrebuildLzxDecode(scratch.data(), scratch.size(), out, out_size, 17, padded);
}

// The frames of an XMemCompress stream of `c` bytes add up exactly to `c` input bytes and `u` output bytes (a frame
// "FF uu uu cc cc" gives its output size, a plain "cc cc" frame is 32 KB). A cheap test before trying to decode.
bool FramesFit(const uint8_t* in, size_t c, size_t u) {
  size_t p = 0, out = 0;
  while (p < c && out < u) {
    size_t csz, usz;
    if (in[p] == 0xFF) {
      if (p + 5 > c) return false;
      usz = Be16(in + p + 1);
      csz = Be16(in + p + 3);
      p += 5;
    } else {
      if (p + 2 > c) return false;
      usz = 0x8000;
      csz = Be16(in + p);
      p += 2;
    }
    if (!csz || !usz || p + csz > c) return false;
    p += csz;
    out += std::min(usz, u - out);
  }
  return out == u && c - p <= 16;  // a few bytes may follow the last frame (9 seen in Gears 3)
}

struct ScanResult {
  uint32_t chunks = 0, failed_chunks = 0, nested = 0, pixel = 0, vertex = 0, new_pixel = 0;
  uint32_t offered[2] = {};  // pixel shaders handed over per variant (see Containers)
  // What PR #43's parts added: blocks / whole chunks / inner blocks decoded only by the padded second attempt, and
  // shader records (pixel + vertex) found in the bytes outside the compressed chunks of a compressed package.
  uint32_t padded_blocks = 0, padded_chunks = 0, padded_nested = 0, gap_records = 0;
  bool unreadable = false;  // starts like a fully compressed file, but no chunk decoded
};

// Gears 1 (2006) packages are LZO1X-compressed per block (package summary CompressionFlags = 2; checked on
// SP_Prison_S02.xxx: 93 of 93 blocks decode to their stated size, 662 pixel shader containers). A bounds-checked LZO1X
// decoder written from the format (literal runs, M1-M4 matches, trailing literals in the low 2 bits); our code (MIT).
bool Lzo1x(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
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
  auto run = [&](size_t base, size_t mask_add, size_t& len) {   // a zero-extended length
    len = base;
    while (need(1) && in[ip] == 0) { len += 255; ++ip; }
    if (!need(1)) return false;
    len += in[ip++] + mask_add;
    return true;
  };
  if (!need(1)) return false;
  int state = 0;
  int t = -1;
  if (in[0] > 17) {
    ip = 1;
    const size_t k = in[0] - 17;
    if (!lit(k)) return false;
    state = k >= 4 ? 4 : int(k);
  }
  for (;;) {
    if (t < 0) { if (!need(1)) return false; t = in[ip++]; }
    if (t < 16) {
      if (state == 0) {   // a literal run
        size_t len = size_t(t);
        if (t == 0 && !run(15, 0, len)) return false;
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
      if (len == 0 && !run(31, 0, len)) return false;
      len += 2;
      if (!need(2)) return false;
      const size_t d = in[ip] | (size_t(in[ip + 1]) << 8);
      ip += 2;
      dist = 1 + (d >> 2);
      trailing = int(d & 3);
    } else {   // 16..31
      len = size_t(t) & 7;
      if (len == 0 && !run(7, 0, len)) return false;
      len += 2;
      if (!need(2)) return false;
      const size_t d = in[ip] | (size_t(in[ip + 1]) << 8);
      ip += 2;
      dist = ((size_t(t) & 8) << 11) + (d >> 2);
      trailing = int(d & 3);
      if (dist == 0) return op == out_len;   // end of stream
      dist += 0x4000;
    }
    if (!copy(dist, len)) return false;
    state = trailing;
    if (!lit(size_t(trailing))) return false;
    t = -1;
  }
}

// 0.6.3 (done-list): what the renderer took from one package - pixel shaders it wants (key = XXH3 of the bytes, as the
// renderer's) and vertex-shader regions that rebuild a recipe vertex shader (key = XXH3 ^ kVertexSalt, as Fresh's).
constexpr uint64_t kVertexSalt = 0x5653ull;
struct Taken {
  uint8_t kind = 0;  // 0 / 1: pixel shader variant (see PackageScanner::Add), 2: vertex-shader region
  uint64_t key = 0;
  std::vector<uint8_t> bytes;  // big-endian microcode, as stored in the package
};

class PackageScanner {
 public:
  // vertex_relevant (0.6.3, shared by the readers): vertex-shader region key -> 0 being looked at by a reader, 1 not a
  // recipe vertex shader, 2 one.
  PackageScanner(std::unordered_set<uint64_t>& seen, std::unordered_map<uint64_t, uint8_t>& vertex_relevant)
      : seen_(seen), vrel_(vertex_relevant) {}
  // 0.6.3: collect what the renderer takes from the files scanned from now on (Taken; per file, see TakeCollected).
  // selective = the renderer wants a fixed list (XenosPrebuildSelectiveKey): the bytes are kept; otherwise only
  // whether anything was wanted.
  void Collect(bool on, bool selective) {
    collect_ = on;
    selective_ = selective;
  }
  // 0.6.3: the file just scanned - what was taken from it; false = it cannot go on the done-list (a wanted pixel shader
  // without a recipe list, or a vertex region another reader was still looking at).
  bool TakeCollected(std::vector<Taken>& out) {
    out.swap(taken_);
    taken_.clear();
    taken_keys_.clear();
    const bool ok = !uncertain_;
    uncertain_ = false;
    return ok;
  }
  // 0.6.3: a done-list package - hand over what the list kept for it, exactly as Add/Offer would have. Returns the new
  // pixel shaders handed over.
  uint32_t Replay(const std::vector<const Taken*>& items) {
    uint32_t fresh = 0;
    std::vector<XenosPrebuildShaderRef> batch;  // r61 #2: one hand-over for the file (the blobs outlive it)
    for (const Taken* t : items) {
      if (t->kind == 2) {
        bool first = false;
        {
          std::lock_guard<std::mutex> lock(SeenMutex());
          first = vrel_.emplace(t->key, uint8_t(2)).second;
        }
        if (first) XenosPrebuildVertexShader(t->bytes.data(), uint32_t(t->bytes.size()));
        continue;
      }
      if (!offer_[t->kind & 1] || !Fresh(t->key)) continue;
      ++fresh;
      batch.push_back({t->bytes.data(), uint32_t(t->bytes.size()), uint32_t(t->kind & 1)});
    }
    if (!batch.empty()) XenosPrebuildPixelShaders(batch.data(), batch.size());
    return fresh;
  }
  // Which variants to hand over (Containers); both by default.
  void SetVariants(bool whole, bool code_only) {
    offer_[0] = whole;
    offer_[1] = code_only;
  }
  // 0.6.2: the keys of every pixel shader offered-or-known in the files scanned from now on are added to *keys.
  void CollectPixelKeys(std::vector<uint64_t>* keys) { keys_ = keys; }
  ScanResult Scan(const std::vector<uint8_t>& b) {
    r_ = {};
    std::vector<uint8_t> all;
    std::vector<std::pair<size_t, size_t>> regions;  // [begin, end) of each compressed chunk in b
    uint32_t decoded_chunks = 0;
    const size_t n = b.size();
    for (size_t i = 0; i + 24 <= n; ++i) {
      if (b[i] != 0x9E || b[i + 1] != 0x2A || b[i + 2] != 0x83 || b[i + 3] != 0xC1) continue;
      uint32_t bsz = Be32(&b[i + 4]);
      // Older UE3 packages (Gears 1, 2006) store the package tag again where the block size goes; the engine then uses
      // its default loading chunk size, 0x20000 (UE3's FCompressedChunkInfo reader does the same).
      const bool legacy = bsz == 0x9E2A83C1u;   // ... and LZO1X blocks (Lzo1x above)
      if (legacy) bsz = 0x20000;
      const uint32_t tc = Be32(&b[i + 8]), tu = Be32(&b[i + 12]);
      if (bsz < 0x8000 || bsz > 0x100000 || (bsz & (bsz - 1)) || !tu || tu > 0x10000000u || !tc || tc > n) continue;
      const uint32_t nb = (tu + bsz - 1) / bsz;
      if (i + 16 + size_t(nb) * 8 > n) continue;
      const uint8_t* blk = &b[i + 16];
      const uint8_t* data = blk + size_t(nb) * 8;
      uint64_t sumc = 0, sumu = 0;
      for (uint32_t k = 0; k < nb; ++k) {
        sumc += Be32(blk + k * 8);
        sumu += Be32(blk + k * 8 + 4);
      }
      if (sumc != tc || sumu != tu || size_t(data - b.data()) + tc > n) continue;
      const size_t base = all.size();
      all.resize(base + tu);
      size_t w = 0;
      bool ok = true, any = false, padded = false;
      for (uint32_t k = 0; k < nb; ++k) {
        const uint32_t cs = Be32(blk + k * 8), us = Be32(blk + k * 8 + 4);
        // Each block is compressed on its own, so a block that fails to decode is zeros and the next ones still count.
        // A block never unpacks to more than the chunk's block size (the decoder's output-size check).
        if (w + us > tu || us > bsz) {
          ok = false;
          break;
        }
        bool pad = false;
        if (legacy ? Lzo1x(data, cs, &all[base + w], us) : Lzx(data, cs, &all[base + w], us, scratch_, &pad)) {
          any = true;
          if (pad) {
            padded = true;
            ++r_.padded_blocks;
          }
        } else {
          ok = false;
          std::memset(&all[base + w], 0, us);
        }
        data += cs;
        w += us;
      }
      ++r_.chunks;
      if (!ok) ++r_.failed_chunks;
      if (ok && padded) ++r_.padded_chunks;  // fully decoded only thanks to the second attempt
      if (any) ++decoded_chunks;
      regions.emplace_back(i, size_t(data - b.data()));
      i = size_t(data - b.data()) - 1;
    }
    if (r_.chunks) {
      Containers(all.data(), all.size(), false);
      Nested(all.data(), all.size());
    }
    // The bytes outside the compressed chunks: the whole file when it has none (the global shader cache), else the
    // package summary and anything stored between or after the chunks (PR #43).
    const uint32_t before_gaps = r_.pixel + r_.vertex;
    size_t raw = 0;
    regions.emplace_back(n, n);
    for (const auto& [begin, end] : regions) {
      if (begin > raw) Containers(b.data() + raw, begin - raw, false);
      raw = end;
    }
    if (r_.chunks) r_.gap_records = r_.pixel + r_.vertex - before_gaps;
    // A fully compressed file (the chunk tag at 0 followed by a power-of-two block size; an uncompressed package has
    // its version there) of which no chunk decoded is unreadable, not a package without shaders (PR #43).
    if (n >= 8 && Be32(b.data()) == 0x9E2A83C1u) {
      const uint32_t block = Be32(b.data() + 4);
      if (block >= 0x8000 && block <= 0x100000 && !(block & (block - 1)) && !decoded_chunks) r_.unreadable = true;
    }
    FlushOffers();  // 0.6.3 (r61 #2)
    return r_;
  }

 private:
  // One pixel shader record. Its microcode region is [literal constants][code]: the header word before the first
  // 0x1000xx00 word is the code size and the one before that the constants' size (they add up to the region's size).
  // Which part a draw shows the GPU differs by game [V, 2026-10-23 against the drawn records]: Gears 2 mostly the
  // whole region (Hospital_03: 187 whole vs 16 code-only), Gears 3 the code only (5 vs 120). Both are handed over
  // until the renderer has seen which one this game draws (Ue3: XenosPrebuildVariantHits).
  void Add(bool vertex, const uint8_t* rec, uint32_t off, uint32_t size) {
    if (vertex) {
      ++r_.vertex;
      // notes/78 C3: the renderer rebuilds recipe vertex shaders from these (a no-op without a recipe pack).
      const uint64_t key = XXH3_64bits(rec + off, size) ^ kVertexSalt;
      if (!collect_) {
        if (Fresh(key)) XenosPrebuildVertexShader(rec + off, size);
        return;
      }
      // 0.6.3: once per region in the session as before, now also remembering whether it rebuilds a recipe vertex
      // shader, so every file holding one keeps it on the done-list (not only the first file read).
      uint8_t state = 0;
      bool first = false;
      {
        std::lock_guard<std::mutex> lock(SeenMutex());
        const auto [it, added] = vrel_.emplace(key, uint8_t(0));
        first = added;
        state = it->second;
      }
      if (first) {
        state = XenosPrebuildVertexShader(rec + off, size, true) ? 2 : 1;
        std::lock_guard<std::mutex> lock(SeenMutex());
        vrel_[key] = state;
      }
      if (state == 0) uncertain_ = true;  // another reader is looking at it right now
      else if (state == 2) Keep(2, key, rec + off, size);
      return;
    }
    ++r_.pixel;
    const uint8_t* region = rec + off;
    Offer(0, region, size);
    for (uint32_t w = 3; w + 1 < off / 4; ++w) {
      if ((Be32(rec + w * 4) & 0xFFFF00FFu) != 0x10000000u) continue;
      const uint32_t consts = Be32(rec + (w - 2) * 4), code = Be32(rec + (w - 1) * 4);
      if (consts && !(consts & 3) && code && consts + code == size) Offer(1, region + consts, code);
      break;
    }
  }
  void Offer(uint32_t variant, const uint8_t* code, uint32_t size) {
    if (!offer_[variant] && !keys_) return;
    const uint64_t key = XXH3_64bits(code, size);
    if (keys_) keys_->push_back(key);  // 0.6.2: every pixel shader of the file, new or not, whichever form is offered
    if (collect_ && XenosPrebuildPixelWanted(key)) {  // 0.6.3: new or not - each file keeps its own on the done-list
      if (selective_) Keep(uint8_t(variant & 1), key, code, size);
      else uncertain_ = true;  // every pixel shader is wanted: the file is read at every start
    }
    if (!offer_[variant] || !Fresh(key)) return;
    ++r_.new_pixel;
    ++r_.offered[variant];
    // 0.6.3 (review r61 #2): queued and handed over in batches (FlushOffers), so the renderer's lock is taken once per
    // batch, not once per shader, by this low-priority thread. The bytes are copied: the scan buffers are reused.
    pending_.push_back({pending_bytes_.size(), size, variant});
    pending_bytes_.insert(pending_bytes_.end(), code, code + size);
    if (pending_.size() >= 256 || pending_bytes_.size() >= (1u << 20)) FlushOffers();
  }
  void FlushOffers() {
    if (pending_.empty()) return;
    std::vector<XenosPrebuildShaderRef> refs;
    refs.reserve(pending_.size());
    for (const PendingOffer& o : pending_) refs.push_back({pending_bytes_.data() + o.offset, o.size, o.variant});
    XenosPrebuildPixelShaders(refs.data(), refs.size());
    pending_.clear();
    pending_bytes_.clear();
  }
  // Shader containers in a buffer. loose = packed back to back (no TArray count before each).
  uint32_t Containers(const uint8_t* a, size_t n, bool loose) {
    uint32_t count = 0;
    for (size_t i = loose ? 0 : 4; i + 12 <= n; ++i) {
      if (a[i] != 0x10 || a[i + 1] != 0x2A || a[i + 2] != 0x11 || a[i + 3] > 1) continue;
      const uint32_t off = Be32(a + i + 4), sz = Be32(a + i + 8);
      if (!sz || sz > 262144 || (sz & 3) || off < 12 || off > 65536 || i + off + sz > n) continue;
      bool ok = false;
      if (i >= 4) {
        const uint32_t cbe = Be32(a + i - 4);
        const uint32_t cle = uint32_t(a[i - 4]) | uint32_t(a[i - 3]) << 8 | uint32_t(a[i - 2]) << 16 |
                             uint32_t(a[i - 1]) << 24;
        ok = off + sz == cbe || off + sz == cle;
      }
      if (!ok && loose) {
        const size_t e = i + off + sz;
        ok = e + 4 > n || (a[e] == 0x10 && a[e + 1] == 0x2A && a[e + 2] == 0x11);
      }
      if (!ok) continue;
      Add(a[i + 3] != 0, a + i, off, sz);
      ++count;
    }
    return count;
  }
  // Gears 3: LZX blocks inside the decompressed package ([u32 U][u32 C] then frames).
  void Nested(const uint8_t* b, size_t n) {
    std::vector<uint8_t> out;
    for (size_t i = 12; i + 5 <= n; ++i) {
      if (b[i] != 0xFF && (b[i - 12] | b[i - 11])) continue;
      const uint32_t U = Be32(b + i - 8), C = Be32(b + i - 4);
      if (U < 64 || U > 0x1000000u || C < 8 || C > U + 1024 || i + C > n) continue;
      const uint32_t first = U < 0x8000 ? U : 0x8000;
      if (b[i] == 0xFF) {
        if (Be16(b + i + 1) != first || Be16(b + i + 3) + 5 > C) continue;
      } else {
        const uint32_t cs = Be16(b + i);
        if (U < 0x8000 || !cs || cs + 2 > C) continue;
      }
      if (!FramesFit(b + i, C, U)) continue;
      out.resize(U);
      bool pad = false;
      if (Lzx(b + i, C, out.data(), U, scratch_, &pad) && Containers(out.data(), U, true)) {
        ++r_.nested;
        if (pad) ++r_.padded_nested;
        i += C - 1;
      }
    }
  }
  void Keep(uint8_t kind, uint64_t key, const uint8_t* bytes, uint32_t size) {  // 0.6.3
    if (!taken_keys_.insert(key).second) return;
    Taken t;
    t.kind = kind;
    t.key = key;
    t.bytes.assign(bytes, bytes + size);
    taken_.push_back(std::move(t));
  }
  std::unordered_set<uint64_t>& seen_;
  std::unordered_map<uint64_t, uint8_t>& vrel_;  // under SeenMutex
  bool collect_ = false, selective_ = false, uncertain_ = false;
  std::vector<Taken> taken_;
  std::unordered_set<uint64_t> taken_keys_;
  struct PendingOffer {  // r61 #2
    size_t offset;
    uint32_t size, variant;
  };
  std::vector<PendingOffer> pending_;
  std::vector<uint8_t> pending_bytes_;
  bool offer_[2] = {true, true};
  // Job 632: several reader threads at start-up share one set of shaders already handed over.
  static std::mutex& SeenMutex() { static std::mutex m; return m; }
  bool Fresh(uint64_t h) { std::lock_guard<std::mutex> lock(SeenMutex()); return seen_.insert(h).second; }
  std::vector<uint64_t>* keys_ = nullptr;
  std::vector<uint8_t> scratch_;
  ScanResult r_;
};

#ifdef REX_FILESYSTEM_FILE_OPEN_OBSERVER
struct Prebuild {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::filesystem::path> queue;
  std::unordered_set<std::string> opened;  // lower-case file names already queued (each package once per session)
  std::unordered_set<std::string> waiting;  // job 666: ... of those, the ones still in `queue` (not taken by a reader)
  std::thread thread;
  std::filesystem::path shader_files;      // job 632: <shader cache>\prebuild-files.txt - packages that held shaders
  std::unordered_set<std::string> known;   // ... their lower-case names (read at start, added to as found)
  std::unordered_set<uint64_t> seen;  // reader thread: pixel shader hashes already handed over
  std::unordered_map<uint64_t, uint8_t> vertex_relevant;  // 0.6.3 (PackageScanner)
  // Job 641 (under mutex): readers busy with a file, files read, the start-up list queued, the start-up reading done.
  int busy = 0;
  uint64_t files_read = 0;
  bool at_start_listed = false, at_start_done = false;
  // 0.6.3 (under mutex): lower-case names of queued files the GAME asked for (never paced), and a reader resting
  // between start-up files wakes when one comes.
  std::unordered_set<std::string> urgent;
  int pace = 100;                // percent of a reader's time on start-up files during play (100 = no pacing)
  // 0.6.3 (review r61 #4): each reader's thread handle and whether it is reading a paced start-up file now; a file the
  // game opens meanwhile raises that reader to normal priority for the rest of the file (under mutex).
  struct ReaderSlot {
    void* thread = nullptr;  // real HANDLE (Windows), else null
    bool paced_read = false;
    bool raised = false;
  };
  std::deque<ReaderSlot> readers;  // deque: a reader's slot stays put while others are added
  std::mutex files_mutex;  // r61 #2: prebuild-files.txt appends, outside `mutex`
  // 0.6.3 done-list (under done_mutex).
  struct DoneFile {
    uint64_t size = 0;
    int64_t mtime = 0;
    std::vector<uint64_t> keys;  // Taken keys (pixel, then vertex)
  };
  std::mutex done_mutex;
  bool done_on = false, selective = false;
  std::filesystem::path done_path;
  std::ofstream done_out;
  std::unordered_map<std::string, DoneFile> done;   // lower-case generic UTF-8 path -> what it gave
  std::unordered_map<uint64_t, Taken> blobs;        // Taken key -> its bytes (once each)
  uint64_t done_skipped = 0, done_skipped_bytes = 0, done_added = 0, done_read = 0, done_read_bytes = 0;
  bool done_write_failed = false;
};
Prebuild* g_prebuild = nullptr;  // never destroyed (the thread runs until the process exits)

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// ---- 0.6.3 done-list file: "GNPBDONE" u32 version u64 context, then records appended as files are read:
// 'B' u8 kind u64 key u32 size bytes  - one shader's bytes (written once, before the first file that lists it)
// 'F' u16 n name u64 size i64 mtime u32 count u64 keys[count]  - one package read to the end (a later one wins)
// each followed by u64 XXH3 of the record (version 2, review r61 #5: a zero-filled or flipped tail after a power loss is
// caught, not read as "a package that gave nothing"). Little-endian (the PC). Every 'B' is also checked against its key;
// a file whose shaders are not all there is read again. The load stops at the first bad record and the file is cut
// there, so the next write goes after the good part. The context is the renderer's list identity
// (XenosPrebuildSelectiveKey: the recipe pack and the renderer build) mixed with this file's build and
// prebuild_lzx.cpp's (r61 #6, r63 C).
constexpr char kDoneMagic[8] = {'G', 'N', 'P', 'B', 'D', 'O', 'N', 'E'};
// Bump when what is taken from a package can change for the same recipe pack: this scanner (containers, variants,
// LZX / LZO) or the renderer's matching (OfferRecipePixel / OnPrebuildVertexShader, recipe_pack.h MatchVertex). The
// build ids in the context (r61 #6, r63 C) change only when shader_prebuild.cpp, prebuild_lzx.cpp or
// shadow_renderer.cpp is compiled again - bump this for a change that recompiles none of them.
constexpr uint32_t kDoneVersion = 2;

std::string PathKey(const std::filesystem::path& p) {  // lower-case UTF-8, forward slashes
  const std::u8string u = p.generic_u8string();
  return Lower(std::string(u.begin(), u.end()));
}
// Size and modification time; false if either is unknown (then the file is never skipped or listed).
bool FileIdentity(const std::filesystem::path& p, uint64_t& size, int64_t& mtime) {
  std::error_code ec;
  const uintmax_t sz = std::filesystem::file_size(p, ec);
  if (ec) return false;
  const auto t = std::filesystem::last_write_time(p, ec);
  if (ec) return false;
  size = uint64_t(sz);
  mtime = int64_t(t.time_since_epoch().count());
  return true;
}
bool TakenValid(const Taken& t) {
  if (t.kind > 2 || t.bytes.empty() || t.bytes.size() > 262144 || (t.bytes.size() & 3)) return false;
  return (XXH3_64bits(t.bytes.data(), t.bytes.size()) ^ (t.kind == 2 ? kVertexSalt : 0)) == t.key;
}
// Load (or start) the done-list; under nothing (install, before the readers).
void LoadDoneList(Prebuild& pb, const std::filesystem::path& path, uint64_t context) {
  pb.done_path = path;
  std::vector<uint8_t> b;
  {
    std::ifstream f(path, std::ios::binary);
    if (f) b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  size_t good = 0;
  bool fresh = true;
  uint64_t superseded = 0;
  if (b.size() >= 20 && std::memcmp(b.data(), kDoneMagic, 8) == 0) {
    uint32_t ver = 0;
    uint64_t ctx = 0;
    std::memcpy(&ver, b.data() + 8, 4);
    std::memcpy(&ctx, b.data() + 12, 8);
    if (ver == kDoneVersion && ctx == context) {
      fresh = false;
      size_t p = 20;
      good = p;
      auto have = [&](size_t n) { return p + n <= b.size(); };
      for (;;) {
        if (!have(1)) break;
        const uint8_t tag = b[p];
        const size_t rec_start = p;
        // r61 #5: the record's checksum (8 bytes after it); false = torn or damaged, the load ends here.
        auto sum_ok = [&](size_t rec_end) {
          if (rec_end + 8 > b.size()) return false;
          uint64_t sum = 0;
          std::memcpy(&sum, &b[rec_end], 8);
          return sum == XXH3_64bits(&b[rec_start], rec_end - rec_start);
        };
        if (tag == 'B') {
          if (!have(1 + 1 + 8 + 4)) break;
          Taken t;
          t.kind = b[p + 1];
          std::memcpy(&t.key, &b[p + 2], 8);
          uint32_t n = 0;
          std::memcpy(&n, &b[p + 10], 4);
          if (n > 262144 || !have(14 + size_t(n))) break;
          if (!sum_ok(p + 14 + n)) break;
          t.bytes.assign(b.begin() + p + 14, b.begin() + p + 14 + n);
          p += 14 + n + 8;
          if (TakenValid(t)) pb.blobs.emplace(t.key, std::move(t));
        } else if (tag == 'F') {
          if (!have(3)) break;
          uint16_t nl = 0;
          std::memcpy(&nl, &b[p + 1], 2);
          if (!have(3 + size_t(nl) + 8 + 8 + 4)) break;
          std::string name(reinterpret_cast<const char*>(&b[p + 3]), nl);
          size_t q = p + 3 + nl;
          Prebuild::DoneFile f;
          uint32_t count = 0;
          std::memcpy(&f.size, &b[q], 8);
          std::memcpy(&f.mtime, &b[q + 8], 8);
          std::memcpy(&count, &b[q + 16], 4);
          q += 20;
          if (count > 1000000 || q + size_t(count) * 8 > b.size()) break;
          if (!sum_ok(q + size_t(count) * 8)) break;
          f.keys.resize(count);
          if (count) std::memcpy(f.keys.data(), &b[q], size_t(count) * 8);
          p = q + size_t(count) * 8 + 8;
          if (!pb.done.insert_or_assign(std::move(name), std::move(f)).second) ++superseded;
        } else {
          break;
        }
        good = p;
      }
    }
  }
  if (fresh) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(kDoneMagic, 8);
    f.write(reinterpret_cast<const char*>(&kDoneVersion), 4);
    f.write(reinterpret_cast<const char*>(&context), 8);
    good = 20;
    if (!f) pb.done_write_failed = true;
  } else if (good < b.size()) {
    std::error_code ec;
    std::filesystem::resize_file(path, good, ec);  // drop a torn tail, so appends follow good records
  }
  pb.done_out.open(path, std::ios::binary | std::ios::app);
  pb.done_on = bool(pb.done_out) && !pb.done_write_failed;
  uint64_t blob_bytes = 0;
  for (const auto& [k, t] : pb.blobs) blob_bytes += t.bytes.size();
  REXLOG_INFO("ue3_shader_prebuild: done-list {}: {} - {} package(s) remembered, {} shader(s) kept ({:.1f} MB){}{}",
              path.filename().string(),
              fresh ? (b.empty() ? "new" : "started again (other recipe pack, other version or not a done-list)")
                    : "loaded",
              pb.done.size(), pb.blobs.size(), double(blob_bytes) / 1048576.0,
              superseded ? fmt::format(", {} older entr(ies) replaced", superseded) : std::string(),
              pb.done_on ? "" : " - CANNOT BE WRITTEN, every package is read as before");
}
// The entry for a file if it is unchanged and every shader it lists is kept; under done_mutex.
const Prebuild::DoneFile* DoneHit(Prebuild& pb, const std::string& key, uint64_t size, int64_t mtime) {
  const auto it = pb.done.find(key);
  if (it == pb.done.end() || it->second.size != size || it->second.mtime != mtime) return nullptr;
  for (uint64_t k : it->second.keys)
    if (!pb.blobs.count(k)) return nullptr;
  return &it->second;
}
// Append a file read to the end (and the shaders not kept yet); under done_mutex.
void AddDone(Prebuild& pb, const std::string& key, uint64_t size, int64_t mtime, std::vector<Taken>& taken) {
  if (!pb.done_on || key.size() > 4096) return;
  std::string rec;
  Prebuild::DoneFile f;
  f.size = size;
  f.mtime = mtime;
  auto put = [&rec](const void* p, size_t n) { rec.append(static_cast<const char*>(p), n); };
  auto seal = [&rec, &put](size_t start) {  // r61 #5: the record's checksum after it
    const uint64_t sum = XXH3_64bits(rec.data() + start, rec.size() - start);
    put(&sum, 8);
  };
  for (Taken& t : taken) {
    f.keys.push_back(t.key);
    if (pb.blobs.count(t.key)) continue;
    const uint32_t n = uint32_t(t.bytes.size());
    const size_t start = rec.size();
    rec.push_back('B');
    rec.push_back(char(t.kind));
    put(&t.key, 8);
    put(&n, 4);
    put(t.bytes.data(), n);
    seal(start);
    pb.blobs.emplace(t.key, std::move(t));
  }
  const uint16_t nl = uint16_t(key.size());
  const uint32_t count = uint32_t(f.keys.size());
  const size_t start = rec.size();
  rec.push_back('F');
  put(&nl, 2);
  put(key.data(), nl);
  put(&size, 8);
  put(&mtime, 8);
  put(&count, 4);
  if (count) put(f.keys.data(), size_t(count) * 8);
  seal(start);
  pb.done_out.write(rec.data(), std::streamsize(rec.size()));
  pb.done_out.flush();
  if (!pb.done_out) {
    REXLOG_WARN("ue3_shader_prebuild: done-list {}: a write failed - no more packages are added this session",
                pb.done_path.filename().string());
    pb.done_on = false;
    return;
  }
  pb.done.insert_or_assign(key, std::move(f));
  ++pb.done_added;
}

// The opening thread (the game's loader): copy the path and return.
// COGBox job 659: file-open listeners chained before this reader. Supports multiple listeners
// (e.g. localization logging and main menu exit interception).
using FileOpenListener = void (*)(std::string_view, const std::filesystem::path*);
std::mutex g_chain_mutex;
std::vector<FileOpenListener> g_chain_listeners;

// 0.6.3 (review r61 #4): the game has asked for a package - a reader in the middle of a paced start-up file finishes it
// at a higher priority, so the game's file (queued in front) is not left behind a starved read. r63 B: below normal,
// not normal - above the lowest (CPU time beside the pipeline workers), but behind the game's own threads, so the rest
// of a large start-up package doesn't compete with level streaming. (Dropping and re-queueing the paced file instead
// would throw away the decoding done, with its shaders already claimed and offered.)
void RaisePacedReaders() {
  std::lock_guard<std::mutex> lock(g_prebuild->mutex);
  for (Prebuild::ReaderSlot& r : g_prebuild->readers) {
    if (!r.paced_read || r.raised) continue;
    r.raised = true;
#ifdef _WIN32
    if (r.thread) SetThreadPriority(static_cast<HANDLE>(r.thread), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  }
}

void OnFileOpened(std::string_view guest_path, const std::filesystem::path* host_path) {
  std::vector<FileOpenListener> listeners;
  {
    std::lock_guard<std::mutex> lock(g_chain_mutex);
    listeners = g_chain_listeners;
  }
  for (auto* listener : listeners) {
    if (listener) listener(guest_path, host_path);
  }
  if (!host_path || !g_prebuild) return;
  const std::string name = Lower(host_path->filename().string());
  const bool package = name.size() > 4 && name.ends_with(".xxx");
  const bool global_cache = name.ends_with(".bin") && name.find("shadercache") != std::string::npos;
  if (!package && !global_cache) return;
  // 0.6.2: the renderer hears of every open by the game (its recipe pipelines for this file's shaders are wanted).
  XenosPrebuildGameOpened(XXH3_64bits(name.data(), name.size()),
                          package && (name.starts_with("sp_") || name.starts_with("spe_") || name.starts_with("mp_")),
                          name.c_str());
  {
    std::lock_guard<std::mutex> lock(g_prebuild->mutex);
    if (!g_prebuild->opened.insert(name).second) {
      // Job 630: queued at start-up and not read yet - the game wants it now, so it goes to the front.
      // Job 666: only looked for when it is still waiting (a package read already costs nothing here).
      if (!g_prebuild->waiting.count(name)) return;
      g_prebuild->urgent.insert(name);  // 0.6.3: never paced
      auto& q = g_prebuild->queue;
      for (auto it = q.begin(); it != q.end(); ++it)
        if (Lower(it->filename().string()) == name) {
          std::filesystem::path moved = std::move(*it);
          q.erase(it);
          q.push_front(std::move(moved));
          break;
        }
    } else {
      g_prebuild->waiting.insert(name);
      g_prebuild->urgent.insert(name);             // 0.6.3: never paced
      g_prebuild->queue.push_front(*host_path);   // job 630: what the game opens now comes before the start-up list
    }
  }
  RaisePacedReaders();          // 0.6.3 (r61 #4)
  g_prebuild->cv.notify_all();  // 0.6.3: also wakes a reader resting between start-up files
}

void ReaderThread() {
#ifdef _WIN32
  // Job 615: the reader decompresses every package the game opens while the game loads it too - at the lowest
  // priority it only takes CPU time the game, the renderer and the pipeline workers leave free (Gears 3 ch11's
  // opening was ~5,000 skipped draws worse with it on).
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
  // 0.6.3 (r61 #4): a real handle, so OnFileOpened can raise this reader (never closed: the thread lives as long as the
  // process).
  void* self = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
#else
  void* self = nullptr;
#endif
  size_t slot = 0;
  {
    std::lock_guard<std::mutex> lock(g_prebuild->mutex);
    slot = g_prebuild->readers.size();
    g_prebuild->readers.push_back({self, false, false});
  }
  PackageScanner scanner(g_prebuild->seen, g_prebuild->vertex_relevant);
  bool collect = false;
  {
    std::lock_guard<std::mutex> lock(g_prebuild->done_mutex);
    collect = g_prebuild->done_on;
    scanner.Collect(collect, g_prebuild->selective);
  }
  uint64_t files = 0, pixel = 0, fresh = 0;
  double rest_ms = 0.0;  // 0.6.3: pacing during play, taken before the next file
  for (;;) {
    std::filesystem::path path;
    bool urgent = false;
    {
      std::unique_lock<std::mutex> lock(g_prebuild->mutex);
      // 0.6.3: a file the game asks for ends the rest at once; r61 #7: so does the end of play (a loading screen, a
      // menu - full speed there), looked at every 250 ms.
      for (auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(rest_ms));
           rest_ms > 0.0 && g_prebuild->urgent.empty() && XenosRendererInPlay();) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= until) break;
        g_prebuild->cv.wait_for(lock, std::min<std::chrono::steady_clock::duration>(until - now, std::chrono::milliseconds(250)),
                                [] { return !g_prebuild->urgent.empty(); });
      }
      rest_ms = 0.0;
      // r61 #4: back to the lowest priority if a file the game opened raised the last one.
      Prebuild::ReaderSlot& me = g_prebuild->readers[slot];
      me.paced_read = false;
      if (me.raised) {
        me.raised = false;
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
      }
      g_prebuild->cv.wait(lock, [] { return !g_prebuild->queue.empty(); });
      path = std::move(g_prebuild->queue.front());
      g_prebuild->queue.pop_front();
      const std::string name = Lower(path.filename().string());
      g_prebuild->waiting.erase(name);   // job 666
      urgent = g_prebuild->urgent.erase(name) != 0;  // 0.6.3
      // r61 #4: marked under the same lock as the pop, so a game open from now on raises this read.
      me.paced_read = !urgent && g_prebuild->pace < 100 && XenosRendererInPlay();
      ++g_prebuild->busy;  // job 641: counted with the pop, so no other reader sees an empty queue and nobody busy
    }
    // 0.6.3: a start-up file during play is paced (rests after it); the game's own are not. All at the lowest priority
    // (r61 #2: not idle - an idle thread preempted inside a lock the render thread or the loader needs can starve them).
    bool paced = false;
    {
      std::lock_guard<std::mutex> lock(g_prebuild->mutex);
      paced = g_prebuild->readers[slot].paced_read;
    }
    const auto t0 = std::chrono::steady_clock::now();
    // 0.6.2: the file's pixel-shader keys, sent to the renderer when the file is done (on every path, also unread).
    std::vector<uint64_t> file_keys;
    const std::string file_name = Lower(path.filename().string());
    scanner.CollectPixelKeys(&file_keys);
    // Job 641: when this was the last queued file and no other reader is busy, the start-up reading is done.
    struct BusyGuard {
      const std::vector<uint64_t>* keys = nullptr;
      const std::string* name = nullptr;
      ~BusyGuard() {
        XenosPrebuildFileShaders(XXH3_64bits(name->data(), name->size()), keys->data(), keys->size());
        bool done = false;
        uint64_t files = 0;
        {
          std::lock_guard<std::mutex> l(g_prebuild->mutex);
          --g_prebuild->busy;
          ++g_prebuild->files_read;
          if (g_prebuild->at_start_listed && !g_prebuild->at_start_done && g_prebuild->queue.empty() && !g_prebuild->busy) {
            g_prebuild->at_start_done = true;
            done = true;
            files = g_prebuild->files_read;
          }
        }
        if (done) {
          XenosPrebuildAtStartDone(files);
          std::lock_guard<std::mutex> l(g_prebuild->done_mutex);  // 0.6.3: what the done-list saved this time
          if (g_prebuild->done_on || g_prebuild->done_skipped || g_prebuild->done_read)
            REXLOG_INFO("ue3_shader_prebuild: done-list: {} package(s) not read ({:.0f} MB; their shaders from the list), "
                        "{} read ({:.0f} MB), {} of them added to the list ({} remembered in all)",
                        g_prebuild->done_skipped, double(g_prebuild->done_skipped_bytes) / 1048576.0,
                        g_prebuild->done_read, double(g_prebuild->done_read_bytes) / 1048576.0,
                        g_prebuild->done_added, g_prebuild->done.size());
        }
      }
    } busy_guard;
    busy_guard.keys = &file_keys;
    busy_guard.name = &file_name;
    // Once enough new draws have used prebuilt shaders, stop offering a variant this game never draws (under 5%).
    const uint64_t h0 = XenosPrebuildVariantHits(0), h1 = XenosPrebuildVariantHits(1), hn = h0 + h1;
    scanner.SetVariants(hn < 32 || h0 * 20 >= hn, hn < 32 || h1 * 20 >= hn);
    // 0.6.3 done-list: an unchanged package read to the end at an earlier start - what it gave is handed over from the
    // list, and the file is not read.
    uint64_t file_size = 0;
    int64_t file_mtime = 0;
    const bool identified = collect && FileIdentity(path, file_size, file_mtime);
    const std::string path_key = identified ? PathKey(path) : std::string();
    if (identified) {
      std::lock_guard<std::mutex> lock(g_prebuild->done_mutex);
      if (const Prebuild::DoneFile* hit = DoneHit(*g_prebuild, path_key, file_size, file_mtime)) {
        std::vector<const Taken*> items;
        items.reserve(hit->keys.size());
        for (uint64_t k : hit->keys) {
          const Taken& t = g_prebuild->blobs.at(k);
          items.push_back(&t);
          if (t.kind != 2) file_keys.push_back(k);
        }
        fresh += scanner.Replay(items);
        ++g_prebuild->done_skipped;
        g_prebuild->done_skipped_bytes += file_size;
        ++files;
        continue;  // (BusyGuard sends the file's pixel keys)
      }
    }
    std::vector<uint8_t> data;
    {
      std::ifstream f(path, std::ios::binary);
      if (!f) continue;
      f.seekg(0, std::ios::end);
      const std::streamoff size = f.tellg();
      if (size <= 0 || size > (std::streamoff(1) << 31)) continue;
      data.resize(size_t(size));
      f.seekg(0, std::ios::beg);
      if (!f.read(reinterpret_cast<char*>(data.data()), size)) continue;
    }
    const ScanResult r = scanner.Scan(data);
    {  // 0.6.3: a file read to the end, nothing damaged, nothing uncertain -> on the done-list
      std::vector<Taken> taken;
      const bool sure = scanner.TakeCollected(taken);
      if (identified) {
        std::lock_guard<std::mutex> lock(g_prebuild->done_mutex);
        ++g_prebuild->done_read;
        g_prebuild->done_read_bytes += data.size();
        if (sure && !r.unreadable && !r.failed_chunks && data.size() == file_size)
          AddDone(*g_prebuild, path_key, file_size, file_mtime, taken);
      }
    }
    if (paced) {  // 0.6.3: rest (100 - pace) / pace times as long as this file took, at most 5 s
      const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      rest_ms = std::min(5000.0, took * double(100 - g_prebuild->pace) / double(std::max(1, g_prebuild->pace)));
    }
    if ((r.pixel || r.vertex) && !g_prebuild->shader_files.empty()) {  // job 632: remembered for the next start-up
      const std::string name = Lower(path.filename().string());
      bool added = false;
      {
        std::lock_guard<std::mutex> lock(g_prebuild->mutex);
        added = g_prebuild->known.insert(name).second;
      }
      if (added) {  // r61 #2: the file write outside `mutex` (the game's file-open thread takes it)
        std::lock_guard<std::mutex> lock(g_prebuild->files_mutex);
        std::ofstream f(g_prebuild->shader_files, std::ios::app);
        f << name << "\n";
      }
    }
    ++files;
    pixel += r.pixel;
    fresh += r.new_pixel;
    if (r.unreadable)
      REXLOG_WARN("ue3_shader_prebuild: {}: {:.1f} MB, a fully compressed file of which none of its {} chunk(s) "
                  "decoded - unreadable, no shaders taken from it",
                  path.filename().string(), double(data.size()) / 1048576.0, r.chunks);
    // Report 20261108-175341 (Judgment, Latin-American disc): SP_E1_W.xxx had 1 of 9 chunks with an LZX block that
    // doesn't decode - the same file decodes cleanly from other players' discs (0 of 9). The game then streamed it
    // during the Episode 1 intro and its own LZX decoder (guest sub_82DD2F90) walked a Huffman table built from the
    // bad block off the end of its stack: "Unhandled guest access violation". A chunk this decoder can't read is a
    // damaged file in the player's dump, so it is said plainly, and the crash report repeats it.
    if (r.failed_chunks && !r.unreadable) {
      const std::string name = path.filename().string();
      REXLOG_WARN("ue3_shader_prebuild: {}: DAMAGED GAME FILE? {} of its {} compressed chunk(s) don't decode - this "
                  "copy of the file is probably damaged; the game is likely to crash when it loads it. Re-dump or "
                  "re-extract the game",
                  name, r.failed_chunks, r.chunks);
      XenosCrashNote("game file that doesn't decode (damaged dump?): " + name + " (" +
                     std::to_string(r.failed_chunks) + " of " + std::to_string(r.chunks) + " chunks)");
    }
    if (r.pixel || r.failed_chunks)
      REXLOG_INFO("ue3_shader_prebuild: {}: {:.1f} MB, {} chunk(s) ({} with an unreadable block, {} decoded only "
                  "with the padding retry: {} block(s)), {} inner block(s) ({} with the padding retry), {} pixel / "
                  "{} vertex shader(s) ({} of them outside the compressed chunks), {} new pixel shader(s) handed to "
                  "the renderer (whole {} / code only {}), {:.0f} ms | {} file(s) read, {} handed over in all | "
                  "draws so far used prebuilt whole {} / code only {}",
                  path.filename().string(), double(data.size()) / 1048576.0, r.chunks, r.failed_chunks,
                  r.padded_chunks, r.padded_blocks, r.nested, r.padded_nested, r.pixel, r.vertex, r.gap_records,
                  r.new_pixel, r.offered[0], r.offered[1],
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), files,
                  fresh, h0, h1);
  }
}
#endif

}  // namespace

// Job 659: a game's own file-open listener, called before this reader's (see g_chain_listeners).
void Ue3ShaderPrebuildChainFileOpened(void (*listener)(std::string_view, const std::filesystem::path*)) {
#ifdef REX_FILESYSTEM_FILE_OPEN_OBSERVER
  if (listener) {
    std::lock_guard<std::mutex> lock(g_chain_mutex);
    if (std::find(g_chain_listeners.begin(), g_chain_listeners.end(), listener) == g_chain_listeners.end()) {
      g_chain_listeners.push_back(listener);
    }
  }
  // Job 666: either order works - while this reader isn't on, its observer still passes every open to the game's
  // listeners (OnFileOpened tells the chain first, then returns without g_prebuild); when it goes on it sets the same one.
  rex::filesystem::SetFileOpenObserver(&OnFileOpened);
#else
  (void)listener;
#endif
}

void Ue3InstallShaderPrebuildIfRequested() {
  if (!REXCVAR_GET(ue3_shader_prebuild)) return;
  if (!XenosPrebuildWanted()) {
    REXLOG_INFO("ue3_shader_prebuild: off - our renderer is not running the DXBC shader path");
    return;
  }
#ifdef REX_FILESYSTEM_FILE_OPEN_OBSERVER
  g_prebuild = new Prebuild();
  const std::string at_start_dirs = REXCVAR_GET(ue3_shader_prebuild_at_start);
  int readers = 1;
  // 0.6.3: small CPUs (8 logical processors or fewer - the report's i5-10300H has 4 cores / 8 threads) keep their cores
  // for the game's own threads, its renderer and the pipeline workers.
  const unsigned hw = XenosUsableProcessors();  // the process's affinity mask, else hardware_concurrency
  const bool small_cpu = hw <= 8;
  {
    const int pace = REXCVAR_GET(ue3_shader_prebuild_play_pace);
    // 0.6.5 (job 2021: on 16 logical processors the readers read at full speed during play and kept handing new shaders
    // to translation while frames spiked): auto paces every CPU - 25 on 8 or fewer, 50 above.
    g_prebuild->pace = pace < 0 ? (small_cpu ? 25 : 50) : std::clamp(pace, 1, 100);
  }
  const std::string cache = rex::cvar::GetFlagByName("gow2_shader_cache_dir");
  if (REXCVAR_GET(ue3_shader_prebuild_done_list) && !cache.empty()) {  // 0.6.3
    const uint64_t selective = XenosPrebuildSelectiveKey();
    g_prebuild->selective = selective != 0;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(cache), ec);
    // r61 #6: this file's build too (the scanner), and (r63 C) the LZX decoder's, a file of its own: a new build of
    // either starts the list again.
    const std::string build = std::string(__DATE__ " " __TIME__ "|") + ue3::prebuild::PrebuildLzxBuildId();
    const uint64_t context = XXH3_64bits(build.data(), build.size()) ^ (selective * 0x9E3779B97F4A7C15ull);
    LoadDoneList(*g_prebuild, std::filesystem::u8path(cache) / "prebuild-done.bin", context);
  }
  if (!at_start_dirs.empty()) {   // job 632: more readers at start-up (the videos and menus leave the CPU idle)
    readers = REXCVAR_GET(ue3_shader_prebuild_threads);
    if (readers <= 0)   // each holds a whole package in memory
      readers = small_cpu ? 1 : hw <= 12 ? 2 : std::clamp(int(hw / 4), 2, 4);
    readers = std::min(readers, 16);
    if (!cache.empty()) {
      g_prebuild->shader_files = std::filesystem::u8path(cache) / "prebuild-files.txt";
      std::ifstream f(g_prebuild->shader_files);
      for (std::string l; std::getline(f, l);) if (!l.empty()) g_prebuild->known.insert(Lower(l));
    }
    REXLOG_INFO("ue3_shader_prebuild_at_start: {} reader thread(s) ({} logical processors), {} package(s) known to hold "
                "shaders; during play the start-up list is read {} (0.6.3: ue3_shader_prebuild_threads / "
                "ue3_shader_prebuild_play_pace)",
                readers, hw, g_prebuild->known.size(),
                g_prebuild->pace >= 100 ? std::string("at full speed, lowest priority")
                                        : fmt::format("at {}% of a thread, lowest priority", g_prebuild->pace));
  }
  g_prebuild->thread = std::thread(ReaderThread);   // after the list above is read (the readers add to it)
  for (int i = 1; i < readers; ++i) std::thread(ReaderThread).detach();
  rex::filesystem::SetFileOpenObserver(&OnFileOpened);
  REXLOG_INFO("ue3_shader_prebuild: on - packages the game opens are read for their pixel shaders (patch 0068)");
  // Job 630: every package of the game's folders, queued at start-up (shader caches first, then the largest - the
  // chapter packages); a package the game opens later is not read twice (the `opened` set).
  const std::string at_start = REXCVAR_GET(ue3_shader_prebuild_at_start);
  if (!at_start.empty()) {
    std::unordered_set<std::string> known;
    {
      std::lock_guard<std::mutex> lock(g_prebuild->mutex);
      known = g_prebuild->known;
    }
    std::thread([at_start, known = std::move(known)] {
      std::vector<std::pair<std::filesystem::path, uintmax_t>> listed, caches, packages;
      // 0.6.3: packages on the done-list (unchanged since) first - they cost no reading, so the renderer has all their
      // shaders within a moment of the start.
      auto on_done_list = [](const std::filesystem::path& path) {
        uint64_t size = 0;
        int64_t mtime = 0;
        if (!FileIdentity(path, size, mtime)) return false;
        std::lock_guard<std::mutex> lock(g_prebuild->done_mutex);
        return g_prebuild->done_on && DoneHit(*g_prebuild, PathKey(path), size, mtime) != nullptr;
      };
      size_t p = 0;
      while (p <= at_start.size()) {
        size_t q = at_start.find(';', p);
        if (q == std::string::npos) q = at_start.size();
        const std::filesystem::path dir = std::filesystem::u8path(at_start.substr(p, q - p));
        p = q + 1;
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
          if (!it->is_regular_file(ec)) continue;
          const std::string name = Lower(it->path().filename().string());
          const uintmax_t size = it->file_size(ec);
          const bool cache_file = name.ends_with(".bin") && name.find("shadercache") != std::string::npos;
          if ((cache_file || (name.size() > 4 && name.ends_with(".xxx"))) && on_done_list(it->path()))
            listed.emplace_back(it->path(), size);   // 0.6.3
          else if (cache_file) caches.emplace_back(it->path(), size);
          else if (name.size() > 4 && name.ends_with(".xxx")) {
            if (known.count(name)) caches.emplace_back(it->path(), size);   // job 632: held shaders last time
            else packages.emplace_back(it->path(), size);
          }
        }
      }
      std::sort(packages.begin(), packages.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      uintmax_t bytes = 0, listed_bytes = 0;
      size_t queued = 0;
      for (const auto& [path, size] : listed) listed_bytes += size;
      {
        std::lock_guard<std::mutex> lock(g_prebuild->mutex);
        for (const auto* list : {&listed, &caches, &packages})
          for (const auto& [path, size] : *list)
            if (g_prebuild->opened.insert(Lower(path.filename().string())).second) {
              g_prebuild->waiting.insert(Lower(path.filename().string()));   // job 666
              g_prebuild->queue.push_back(path);
              bytes += size;
              ++queued;
            }
      }
      {
        std::lock_guard<std::mutex> lock(g_prebuild->mutex);
        g_prebuild->at_start_listed = true;  // job 641
        if (g_prebuild->queue.empty() && !g_prebuild->busy && !g_prebuild->at_start_done) {
          g_prebuild->at_start_done = true;
          XenosPrebuildAtStartDone(g_prebuild->files_read);
        }
      }
      g_prebuild->cv.notify_all();
      REXLOG_INFO("ue3_shader_prebuild_at_start: {} file(s) queued ({} on the done-list first - not read, {:.0f} MB; then "
                  "{} shader cache(s) / known shader packages; {:.0f} MB to read) from {}",
                  queued, listed.size(), double(listed_bytes) / 1048576.0, caches.size(),
                  double(bytes > listed_bytes ? bytes - listed_bytes : 0) / 1048576.0, at_start);
    }).detach();
  }
#else
  REXLOG_WARN("ue3_shader_prebuild: this SDK has no file-open observer (SDK patch 32); nothing is prebuilt");
#endif
}
