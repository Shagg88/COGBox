// Chapter recipe packs (notes/78, C3): the pipelines a chapter opening needs, built from the player's own game files
// while the chapter loads. A pack (work\tools\make_recipe_packs.ps1) holds pipeline records (shader fingerprints +
// pipeline state) and, per vertex shader, only the dwords the console fills in when the shader is bound (control flow
// and fetch instructions: buffer slot, offset, stride, format) plus 4 probe positions. A vertex shader in a package is
// the code part (the tail) of its record; with the recipe's dwords written over it, it must hash to the recipe's
// fingerprint or it is not used. Pixel shaders are used as the files hold them. No whole shader is in a pack.
// Header-only so tools can run the same matching. Our code (MIT).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Inline xxhash, as rex/hash.h has it (job 612: without this, an include ahead of rex/hash.h left XXH3_64bits
// undefined at link time).
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

namespace xenos::recipes {

inline uint32_t LoadBe32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

struct VertexFix {
  uint64_t hash = 0;  // the draws' fingerprint: XXH3-64 of the big-endian microcode
  uint32_t dwords = 0;
  std::vector<std::pair<uint32_t, uint32_t>> fill;  // (dword index, value) written over the file's code
  std::vector<uint32_t> probes;                     // dword indices not in `fill`
  uint32_t probe_hash = 0;                          // FNV-1a of the probe dwords (big-endian bytes)
  uint32_t id = 0;                                  // index in Pack::vertex
};

inline uint32_t ProbeHash(const uint8_t* be_code, const std::vector<uint32_t>& probes) {
  uint32_t h = 2166136261u;
  for (uint32_t p : probes) {
    for (int b = 0; b < 4; ++b) {
      h ^= be_code[p * 4 + b];
      h *= 16777619u;
    }
  }
  return h;
}

struct Pack {
  static constexpr size_t kRecordBytes = 144;  // the renderer's PipelineRecord (patch 0058's pipelines.list)
  std::vector<std::array<uint8_t, kRecordBytes>> records;
  std::vector<VertexFix> vertex;
  std::unordered_map<uint32_t, std::vector<uint32_t>> by_dwords;  // length -> indices in `vertex`
  std::vector<uint32_t> lengths;                                  // ascending

  // false (and `err`) for a file that is not a whole, valid pack.
  bool Parse(const std::vector<uint8_t>& b, std::string& err) {
    size_t p = 0;
    auto u32 = [&](uint32_t& v) {
      if (p + 4 > b.size()) return false;
      std::memcpy(&v, &b[p], 4);
      p += 4;
      return true;
    };
    if (b.size() < 16 || std::memcmp(b.data(), "GNRCP001", 8) != 0) {
      err = "not a recipe pack (GNRCP001)";
      return false;
    }
    p = 8;
    uint32_t nrec = 0, nvs = 0;
    if (!u32(nrec) || !u32(nvs) || nrec > 1000000 || nvs > 100000 || p + size_t(nrec) * kRecordBytes > b.size()) {
      err = "bad counts";
      return false;
    }
    records.resize(nrec);
    for (auto& r : records) {
      std::memcpy(r.data(), &b[p], kRecordBytes);
      p += kRecordBytes;
    }
    vertex.resize(nvs);
    for (uint32_t i = 0; i < nvs; ++i) {
      VertexFix& v = vertex[i];
      v.id = i;
      uint32_t lo = 0, hi = 0, m = 0, k = 0;
      if (!u32(lo) || !u32(hi) || !u32(v.dwords) || !u32(m) || v.dwords == 0 || v.dwords > 65536 || m > v.dwords) {
        err = "bad vertex entry " + std::to_string(i);
        return false;
      }
      v.hash = uint64_t(hi) << 32 | lo;
      v.fill.resize(m);
      for (auto& [idx, val] : v.fill) {
        if (!u32(idx) || !u32(val) || idx >= v.dwords) {
          err = "bad fill in vertex entry " + std::to_string(i);
          return false;
        }
      }
      if (!u32(k) || k > 4) {
        err = "bad probes in vertex entry " + std::to_string(i);
        return false;
      }
      v.probes.resize(k);
      for (uint32_t& q : v.probes) {
        if (!u32(q) || q >= v.dwords) {
          err = "bad probe in vertex entry " + std::to_string(i);
          return false;
        }
      }
      if (!u32(v.probe_hash)) {
        err = "truncated vertex entry " + std::to_string(i);
        return false;
      }
      by_dwords[v.dwords].push_back(i);
    }
    if (p != b.size()) {
      err = "trailing bytes";
      return false;
    }
    for (auto& [len, list] : by_dwords) lengths.push_back(len);
    std::sort(lengths.begin(), lengths.end());
    return true;
  }

  // A package's vertex-shader region (big-endian, `dwords` long): calls found(fix, rebuilt_be_bytes) for each recipe
  // vertex shader it rebuilds exactly. skip(id) true = already found (another package), not tried again.
  template <typename Skip, typename Found>
  void MatchVertex(const uint8_t* be_region, uint32_t dwords, std::vector<uint8_t>& scratch, Skip&& skip,
                   Found&& found) const {
    for (uint32_t len : lengths) {
      if (len > dwords) break;
      const uint8_t* tail = be_region + size_t(dwords - len) * 4;
      for (uint32_t i : by_dwords.at(len)) {
        const VertexFix& v = vertex[i];
        if (skip(v.id) || ProbeHash(tail, v.probes) != v.probe_hash) continue;
        scratch.assign(tail, tail + size_t(len) * 4);
        for (const auto& [idx, val] : v.fill) StoreBe32(&scratch[size_t(idx) * 4], val);
        if (XXH3_64bits(scratch.data(), scratch.size()) == v.hash) found(v, scratch);
      }
    }
  }
};

}  // namespace xenos::recipes
