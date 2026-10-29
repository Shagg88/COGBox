// Shader container discovery, independent of the game and renderer. Our code (MIT).
#pragma once
#include <ue3/lzx.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>
namespace ue3::shader_prebuild {
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint32_t Be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

// One XMemCompress-framed LZX block into out (out.size() bytes). True = decoded.
inline bool Lzx(const uint8_t* in, size_t n, uint8_t* out, size_t out_size, std::vector<uint8_t>& scratch) {
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
  return ue3::LzxDecompress(scratch.data(), scratch.size(), out, out_size, 17);
}

// The frames of an XMemCompress stream of `c` bytes add up exactly to `c` input bytes and `u` output bytes (a frame
// "FF uu uu cc cc" gives its output size, a plain "cc cc" frame is 32 KB). A cheap test before trying to decode.
inline bool FramesFit(const uint8_t* in, size_t c, size_t u) {
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
  uint32_t chunks = 0, failed_chunks = 0, rejected_inner_candidates = 0, nested = 0, pixel = 0, vertex = 0, new_pixel = 0;
  uint32_t offered[2] = {};  // pixel shaders handed over per variant (see Containers)
};

class PackageScanner {
 public:
  using Sink = std::function<void(bool, const uint8_t*, uint32_t, uint32_t)>;
  explicit PackageScanner(Sink sink) : sink_(std::move(sink)) {}
  ScanResult Scan(const std::vector<uint8_t>& b) {
    r_ = {};
    std::vector<uint8_t> all;
    std::vector<std::pair<size_t, size_t>> compressed_regions;
    const size_t n = b.size();
    for (size_t i = 0; i + 24 <= n; ++i) {
      if (b[i] != 0x9E || b[i + 1] != 0x2A || b[i + 2] != 0x83 || b[i + 3] != 0xC1) continue;
      const uint32_t bsz = Be32(&b[i + 4]), tc = Be32(&b[i + 8]), tu = Be32(&b[i + 12]);
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
      bool ok = true;
      for (uint32_t k = 0; k < nb; ++k) {
        const uint32_t cs = Be32(blk + k * 8), us = Be32(blk + k * 8 + 4);
        // Each block is compressed on its own, so a block that fails to decode is zeros and the next ones still count.
        if (w + us > tu) {
          ok = false;
          break;
        }
        if (!Lzx(data, cs, &all[base + w], us, scratch_)) {
          ok = false;
          std::memset(&all[base + w], 0, us);
        }
        data += cs;
        w += us;
      }
      ++r_.chunks;
      if (!ok) ++r_.failed_chunks;
      compressed_regions.emplace_back(i, size_t(data - b.data()));
      i = size_t(data - b.data()) - 1;
    }
    if (r_.chunks) {
      Containers(all.data(), all.size(), false);
      Nested(all.data(), all.size());
    }
    // Scan raw ranges outside compressed chunks as well. Looking inside compressed input can
    // mistake partially stored bytes for truncated inner streams, duplicating or losing records.
    size_t raw = 0;
    for (const auto& [begin, end] : compressed_regions) {
      if (begin > raw) { Containers(b.data() + raw, begin - raw, false); Nested(b.data() + raw, begin - raw); }
      raw = end;
    }
    if (raw < n) { Containers(b.data() + raw, n - raw, false); Nested(b.data() + raw, n - raw); }
    // A damaged fully-compressed package must not be cached as an empty, successful scan.
    if (n >= 4 && Be32(b.data()) == 0x9E2A83C1 && !r_.chunks) {
      // Ordinary uncompressed UE3 packages share this tag; their second word is a version,
      // whereas a compressed stream has a power-of-two block size in this position.
      const uint32_t block = n >= 8 ? Be32(b.data() + 4) : 0;
      if (n < 16 || (block >= 0x8000 && block <= 0x100000 && !(block & (block - 1)))) ++r_.failed_chunks;
    }
    return r_;
  }

 private:
  // One pixel shader record. Its microcode region is [literal constants][code]: the header word before the first
  // 0x1000xx00 word is the code size and the one before that the constants' size (they add up to the region's size).
  // Which part a draw shows the GPU differs by game [V, 2026-10-23 against the drawn records]: Gears 2 mostly the
  // whole region (Hospital_03: 187 whole vs 16 code-only), Gears 3 the code only (5 vs 120). Both are handed over
  // until the renderer has seen which one this game draws (Ue3: XenosPrebuildVariantHits).
  void Add(bool vertex, const uint8_t* rec, uint32_t off, uint32_t size) {
    if (vertex) ++r_.vertex;
    else ++r_.pixel;
    const uint8_t* region = rec + off;
    Offer(vertex, 0, region, size);
    for (uint32_t w = 3; w + 1 < off / 4; ++w) {
      if ((Be32(rec + w * 4) & 0xFFFF00FFu) != 0x10000000u) continue;
      const uint32_t consts = Be32(rec + (w - 2) * 4), code = Be32(rec + (w - 1) * 4);
      if (consts && !(consts & 3) && code && !(code & 3) && uint64_t(consts) + code == size) Offer(vertex, 1, region + consts, code);
      break;
    }
  }
  void Offer(bool vertex, uint32_t variant, const uint8_t* code, uint32_t size) {
    // Never infer coverage from previous draws: rare variants still have to be prepared.
    ++r_.new_pixel;
    ++r_.offered[variant];
    sink_(vertex, code, size, variant);
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
      if (U < 16 || U > 0x1000000u || C < 8 || C > U + 1024 || i + C > n) continue;
      const uint32_t first = U < 0x8000 ? U : 0x8000;
      if (b[i] == 0xFF) {
        if (Be16(b + i + 1) != first || Be16(b + i + 3) + 5 > C) continue;
      } else {
        const uint32_t cs = Be16(b + i);
        if (U < 0x8000 || !cs || cs + 2 > C) continue;
      }
      if (!FramesFit(b + i, C, U)) continue;
      out.resize(U);
      if (!Lzx(b + i, C, out.data(), U, scratch_)) {
        // Sizes/frame lengths alone also match some ordinary mesh data. Only a decoded stream
        // with valid containers is confirmed as an inner shader block.
        ++r_.rejected_inner_candidates;
      } else if (Containers(out.data(), U, true)) {
        ++r_.nested;
        i += C - 1;
      }
    }
  }
  Sink sink_;
  std::vector<uint8_t> scratch_;
  ScanResult r_;
};


}  // namespace ue3::shader_prebuild
