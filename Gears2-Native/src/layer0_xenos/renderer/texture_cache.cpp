// Layer 0 (game-free): see xenos/renderer/texture_cache.h. The host-format table and swizzles below are taken from
// Xenia Canary e6cd2e4 (d3d12/d3d12_texture_cache.h host_formats_), and DecodeCtx1 / DecodeDxt3a follow Canary's
// shaders/texture_load_ctx1.cs.xesl, texture_load_dxt3a.cs.xesl and pixel_formats.xesli: BSD-3-Clause, Copyright (c)
// Ben Vanik and Xenia project contributors. All rights reserved. Redistribution and use in source and binary forms,
// with or without modification, are permitted under the BSD 3-Clause licence (THIRD_PARTY_NOTICES.md). The rest is our
// code (MIT).
#include <xenos/renderer/texture_cache.h>

#include <xenos/formats.h>
#include <xenos/texture_layout.h>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging/macros.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>

REXCVAR_DEFINE_DOUBLE(gow2_texture_phase_log_ms, 0.0, "Gears2",
                      "Diagnostic (job 299): log a frame whose texture loads took longer than this many ms, split into "
                      "phases - page watch arming, page hashing, host texture creation, GPU-path copy + untile, CPU-path "
                      "untile, CPU-path upload. 0 = off");

namespace {
// Job 299 diagnostic: where the recording thread's texture-load time goes (job 295: untiling on worker threads saved
// only 7% of the 214-228 ms ch10 level-start texture time, so the untile loop is not the main cost).
struct TexturePhases {
  double load = 0, watch = 0, hash = 0, create = 0, gpu = 0, cpu = 0, upload = 0;
  uint32_t loads = 0, creates = 0, gpu_loads = 0, cpu_loads = 0;
} g_phase;
double PhaseMs(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
struct PhaseScope {
  double& acc;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  explicit PhaseScope(double& a) : acc(a) {}
  ~PhaseScope() { acc += PhaseMs(t0); }
};
}  // namespace

namespace xenos::renderer {
namespace {

namespace tx = ::xenos::texture;

constexpr uint32_t Bits(uint32_t v, uint32_t shift, uint32_t width) { return (v >> shift) & ((1u << width) - 1u); }

enum class Convert : uint8_t {
  kNone,          // unsupported
  kCopy,          // endian swap only
  kSwapRB565,     // R5G6B5 -> B5G6R5
  kSwapRB1555,    // R5G5B5A1 -> B5G5R5A1
  kSwapRB4444,    // R4G4B4A4 -> B4G4R4A4
  kDxt3aToR8,     // 4-bit alpha blocks -> R8
  kCtx1ToRG8,     // CTX1 blocks -> R8G8
  kDepthUnorm,    // 24:8 unorm depth -> R32_FLOAT
  kDepthFloat,    // 24:8 20e4 depth -> R32_FLOAT
  kR11G11B10,     // k_10_11_11: R 11 bits low, G 11, B 10 -> RGBA16 unorm
  kR10G11B11,     // k_11_11_10: R 10 bits low, G 11, B 11 -> RGBA16 unorm
};

struct HostFormat {
  gpu::Format format;
  Convert convert;
  uint8_t swizzle[4];  // host component for guest component X/Y/Z/W (Canary's XE_GPU_TEXTURE_SWIZZLE_*)
  bool block_compressed;  // host format is BCn: mip 0 must be 4-aligned
  uint8_t host_bytes;     // bytes per guest block on the host
};

constexpr uint8_t kRRRR[4] = {0, 0, 0, 0}, kRGGG[4] = {0, 1, 1, 1}, kRGBB[4] = {0, 1, 2, 2}, kRGBA[4] = {0, 1, 2, 3};

HostFormat Host(uint32_t f) {
  auto h = [](gpu::Format fmt, Convert c, const uint8_t* s, bool bc, uint8_t bytes) {
    HostFormat r{fmt, c, {s[0], s[1], s[2], s[3]}, bc, bytes};
    return r;
  };
  using F = gpu::Format;
  switch (f) {
    case 2: case 8: case 9: return h(F::kR8Unorm, Convert::kCopy, kRRRR, false, 1);          // k_8, k_8_A, k_8_B (0022)
    case 3: return h(F::kB5G5R5A1Unorm, Convert::kSwapRB1555, kRGBA, false, 2);               // k_1_5_5_5
    case 4: return h(F::kB5G6R5Unorm, Convert::kSwapRB565, kRGBB, false, 2);                  // k_5_6_5
    case 6: case 14: case 50: return h(F::kR8G8B8A8Unorm, Convert::kCopy, kRGBA, false, 4);  // k_8_8_8_8 (+_A, 0022)
    case 7: case 54: return h(F::kR10G10B10A2Unorm, Convert::kCopy, kRGBA, false, 4);        // k_2_10_10_10
    case 10: return h(F::kR8G8Unorm, Convert::kCopy, kRGGG, false, 2);                        // k_8_8
    case 15: return h(F::kB4G4R4A4Unorm, Convert::kSwapRB4444, kRGBA, false, 2);              // k_4_4_4_4
    case 16: case 55: return h(F::kR16G16B16A16Unorm, Convert::kR11G11B10, kRGBB, false, 8); // k_10_11_11
    case 17: case 56: return h(F::kR16G16B16A16Unorm, Convert::kR10G11B11, kRGBB, false, 8); // k_11_11_10
    case 18: case 51: return h(F::kBC1, Convert::kCopy, kRGBA, true, 8);                      // k_DXT1
    case 19: case 52: return h(F::kBC2, Convert::kCopy, kRGBA, true, 16);                     // k_DXT2_3
    case 20: case 53: return h(F::kBC3, Convert::kCopy, kRGBA, true, 16);                     // k_DXT4_5
    case 22: return h(F::kR32Float, Convert::kDepthUnorm, kRRRR, false, 4);                   // k_24_8
    case 23: return h(F::kR32Float, Convert::kDepthFloat, kRRRR, false, 4);                   // k_24_8_FLOAT
    case 24: return h(F::kR16Unorm, Convert::kCopy, kRRRR, false, 2);                         // k_16
    case 25: return h(F::kR16G16Unorm, Convert::kCopy, kRGGG, false, 4);                      // k_16_16
    case 26: return h(F::kR16G16B16A16Unorm, Convert::kCopy, kRGBA, false, 8);                // k_16_16_16_16
    case 27: case 30: return h(F::kR16Float, Convert::kCopy, kRRRR, false, 2);                // k_16(_EXPAND|_FLOAT)
    case 28: case 31: return h(F::kR16G16Float, Convert::kCopy, kRGGG, false, 4);             // k_16_16(...)
    case 29: case 32: return h(F::kR16G16B16A16Float, Convert::kCopy, kRGBA, false, 8);       // k_16_16_16_16(...)
    case 36: return h(F::kR32Float, Convert::kCopy, kRRRR, false, 4);                         // k_32_FLOAT
    case 37: return h(F::kR32G32Float, Convert::kCopy, kRGGG, false, 8);                      // k_32_32_FLOAT
    case 38: return h(F::kR32G32B32A32Float, Convert::kCopy, kRGBA, false, 16);               // k_32_32_32_32_FLOAT
    case 49: return h(F::kBC5, Convert::kCopy, kRGGG, true, 16);                              // k_DXN
    case 58: return h(F::kR8Unorm, Convert::kDxt3aToR8, kRRRR, false, 16);                    // k_DXT3A (16 R8 texels)
    case 59: return h(F::kBC4, Convert::kCopy, kRRRR, true, 8);                               // k_DXT5A
    case 60: return h(F::kR8G8Unorm, Convert::kCtx1ToRG8, kRGGG, false, 32);                  // k_CTX1 (16 R8G8)
    default: return h(F::kUnknown, Convert::kNone, kRGBA, false, 0);
  }
}

void SwapUnits(uint8_t* p, uint32_t bytes, uint32_t endian) {
  if (endian == 1) {  // k8in16
    for (uint32_t i = 0; i + 1 < bytes; i += 2) std::swap(p[i], p[i + 1]);
  } else if (endian == 2) {  // k8in32
    for (uint32_t i = 0; i + 3 < bytes; i += 4) {
      std::swap(p[i], p[i + 3]);
      std::swap(p[i + 1], p[i + 2]);
    }
  } else if (endian == 3) {  // k16in32
    for (uint32_t i = 0; i + 3 < bytes; i += 4) {
      std::swap(p[i], p[i + 2]);
      std::swap(p[i + 1], p[i + 3]);
    }
  }
}

uint32_t Weights(uint32_t codes) {  // Canary XeDXTHighColorWeights
  codes = ((codes & 0x55555555u) << 1u) | ((codes & 0xAAAAAAAAu) >> 1u);
  return codes ^ ((codes & 0xAAAAAAAAu) >> 1u);
}

// One 4x4 CTX1 block (8 bytes, already endian-swapped as 32-bit words) -> 16 R8G8 texels (row-major).
void DecodeCtx1(const uint8_t* block, uint8_t* out) {
  uint32_t d0, d1;
  std::memcpy(&d0, block, 4);
  std::memcpy(&d1, block + 4, 4);
  const uint32_t r0 = (d0 >> 8) & 0xFF, g0 = d0 & 0xFF, r1 = d0 >> 24, g1 = (d0 >> 16) & 0xFF;
  const uint32_t wh = Weights(d1), wl = ~wh;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t a = (wl >> (2 * i)) & 3, b = (wh >> (2 * i)) & 3;
    out[2 * i] = uint8_t((a * r0 + b * r1) / 3);
    out[2 * i + 1] = uint8_t((a * g0 + b * g1) / 3);
  }
}

// One 4x4 DXT3A block (8 bytes, endian-swapped as 32-bit words) -> 16 R8 texels.
void DecodeDxt3a(const uint8_t* block, uint8_t* out) {
  uint32_t d[2];
  std::memcpy(d, block, 8);
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) {
      const uint32_t a = (d[y >> 1] >> (16 * (y & 1) + 4 * x)) & 0xF;
      out[4 * y + x] = uint8_t(a | (a << 4));
    }
}

uint16_t Expand(uint32_t v, uint32_t bits) {
  const uint32_t r = v << (16 - bits);
  return uint16_t(r | (r >> bits));  // bit replication to 16 bits (exact for 0 and max)
}

}  // namespace

void TextureCache::ResetCounters() {
  const uint64_t textures = stats_.textures, host = stats_.host_bytes;
  const uint64_t scaled = stats_.scaled_textures, scaled_bytes = stats_.scaled_bytes;
  stats_ = TextureCacheStats{};
  stats_.textures = textures;
  stats_.host_bytes = host;
  stats_.scaled_textures = scaled;
  stats_.scaled_bytes = scaled_bytes;
}

void TextureCache::StandIn(const std::string& why) {
  ++stats_.stand_ins;
  ++stats_.stand_in_reasons[why];
}

// Host formats the GPU untile can store (typed UAV stores required on D3D11-class hardware); 0 = CPU path.
uint32_t UntileCode(gpu::Format f) {
  switch (f) {
    case gpu::Format::kR8Unorm: return 1;
    case gpu::Format::kR8G8Unorm: return 2;
    case gpu::Format::kR8G8B8A8Unorm: return 3;
    case gpu::Format::kR16Unorm: return 4;
    case gpu::Format::kR16G16Unorm: return 5;
    case gpu::Format::kR16G16B16A16Unorm: return 6;
    case gpu::Format::kR16Float: return 7;
    case gpu::Format::kR16G16Float: return 8;
    case gpu::Format::kR16G16B16A16Float: return 9;
    case gpu::Format::kR32Float: return 10;
    case gpu::Format::kR32G32Float: return 11;
    case gpu::Format::kR32G32B32A32Float: return 12;
    case gpu::Format::kR10G10B10A2Unorm: return 13;
    default: return 0;
  }
}

uint32_t TextureCache::Bind(const uint32_t* fetch, uint32_t slots_2d, uint32_t slots_cube, uint32_t slots_3d,
                            uint64_t frame, gpu::TextureView views[96], gpu::SamplerDesc samplers[32]) {
  uint32_t stand_ins = 0;
  last_scaled_mask_ = 0;
  const uint64_t epoch = watch_.Epoch();
  if (epoch != last_bind_epoch_) {  // patch 0137
    ++stats_.epoch_changes;
    last_bind_epoch_ = epoch;
  }
  for (uint32_t k = 0; k < 32; ++k) {
    const uint32_t kinds = ((slots_2d >> k) & 1) | (((slots_cube >> k) & 1) << 1) | (((slots_3d >> k) & 1) << 2);
    if (!kinds) continue;
    const uint32_t* tf = fetch + 6 * k;
    SlotCache& sc = slot_cache_[k];
    if (sc.valid && sc.kinds == kinds && sc.epoch == epoch && std::memcmp(sc.tf, tf, sizeof(sc.tf)) == 0) {
      samplers[k] = sc.sampler;
      for (uint32_t kind = 0; kind < 3; ++kind)
        if ((kinds >> kind) & 1) views[kind * 32 + k] = sc.views[kind];
      for (Entry* e : sc.entries)
        if (e) e->last_used = frame;
      if (sc.scaled && (kinds & 1)) {
        last_scaled_mask_ |= 1u << k;
        ++stats_.scaled_bound;
      }
      stand_ins += sc.stand_ins;
      stats_.stand_ins += sc.stand_ins;
      stats_.bound += sc.bound;
      ++stats_.slot_hits;  // patch 0137
      continue;
    }
    ++stats_.slot_misses;  // patch 0137
    const uint32_t stand_ins_before = stand_ins;
    sc.entries[0] = sc.entries[1] = sc.entries[2] = nullptr;
    sc.scaled = false;
    const uint64_t bound_before = stats_.bound;
    // Sampler (shared by all dimensions of the slot).
    gpu::SamplerDesc& sd = samplers[k];
    const uint32_t dw3 = tf[3], dw4 = tf[4], dw5 = tf[5];
    sd.mag_linear = Bits(dw3, 19, 2) == 1;
    sd.min_linear = Bits(dw3, 21, 2) == 1;
    const uint32_t mip = Bits(dw3, 23, 2);
    sd.mip_linear = mip == 1;
    const uint32_t aniso = Bits(dw3, 25, 3);
    sd.max_anisotropy = aniso >= 1 && aniso <= 5 ? uint8_t(1u << (aniso - 1)) : 0;
    auto address = [](uint32_t c) {
      switch (c) {
        case 0: return gpu::SamplerAddress::kWrap;
        case 1: return gpu::SamplerAddress::kMirror;
        case 2: case 4: return gpu::SamplerAddress::kClamp;   // clamp to edge / clamp to halfway
        case 6: return gpu::SamplerAddress::kBorder;
        default: return gpu::SamplerAddress::kMirrorOnce;     // the mirror-clamp modes
      }
    };
    sd.address[0] = address(Bits(tf[0], 10, 3));
    sd.address[1] = address(Bits(tf[0], 13, 3));
    sd.address[2] = address(Bits(tf[0], 16, 3));
    const uint32_t border = Bits(dw5, 0, 2);
    sd.border = border == 0 ? 0 : border == 1 ? 1 : 2;
    sd.lod_bias = float(int32_t(dw4 << 10) >> 22) / 32.0f;  // signed 10 bits at 12, 5 fraction bits
    sd.min_lod = float(Bits(dw4, 2, 4));
    sd.max_lod = mip == 2 ? sd.min_lod : float(Bits(dw4, 6, 4));  // kBaseMap: no mips
    if (sd.max_lod < sd.min_lod) sd.max_lod = sd.min_lod;
    // Views: swizzle (fetch constant, then the host format's channel map) and signedness.
    const HostFormat hf = Host(Bits(tf[1], 0, 6));
    const uint32_t swz = Bits(dw3, 1, 12);
    uint8_t comp[4];
    bool any_signed = false, any_unsigned = false;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t s = Bits(swz, 3 * i, 3);
      comp[i] = s < 4 ? hf.swizzle[s] : (s == 4 ? 4 : 5);
      if (s < 4) {
        const uint32_t sign = Bits(tf[0], 2 + 2 * s, 2);
        (sign == 1 ? any_signed : any_unsigned) = true;
      }
    }
    if (any_signed && any_unsigned) StandIn("mixed signed/unsigned components (signed view used)");
    for (uint32_t kind = 0; kind < 3; ++kind) {
      if (!((kinds >> kind) & 1)) continue;
      const uint32_t reg = kind * 32 + k;  // t(k), t(32+k) cube, t(64+k) 3D
      gpu::TextureView& v = views[reg];
      v.dimension = kind == 0 ? gpu::ViewDimension::k2D : kind == 1 ? gpu::ViewDimension::kCube : gpu::ViewDimension::k3D;
      if (Bits(tf[0], 0, 2) != 2) {  // not a texture fetch constant (kTexture = 2)
        StandIn("fetch constant is not a texture");
        ++stand_ins;
        continue;
      }
      // Stage 7: a 2D texture our scaled resolves hold is taken at the resolution scale.
      if (kind == 0 && scale_ > 1 && scaled_lookup_ && scaled_load_) {
        if (Entry* se = LookupScaled(tf)) {
          se->last_used = frame;
          sc.entries[kind] = se;
          v.texture = se->texture;
          for (uint32_t i = 0; i < 4; ++i) v.components[i] = comp[i];
          v.signed_view = any_signed;
          ++stats_.bound;
          ++stats_.scaled_bound;
          sc.scaled = true;
          last_scaled_mask_ |= 1u << k;
          continue;
        }
      }
      Entry& e = Lookup(tf, kind, frame);
      e.last_used = frame;
      sc.entries[kind] = &e;
      if (!e.failure.empty() || !e.texture) {
        StandIn(e.failure.empty() ? "texture not created" : e.failure);
        ++stand_ins;
        if (mismatch_zero_ && e.dimension_mismatch) v.zero = true;
        continue;
      }
      v.texture = e.texture;
      for (uint32_t i = 0; i < 4; ++i) v.components[i] = comp[i];
      v.signed_view = any_signed;
      ++stats_.bound;
    }
    // Remember the result; a re-upload inside Lookup bumps nothing in the watch, so the epoch read before is valid.
    sc.valid = true;
    std::memcpy(sc.tf, tf, sizeof(sc.tf));
    sc.kinds = kinds;
    sc.epoch = epoch;
    sc.sampler = sd;
    for (uint32_t kind = 0; kind < 3; ++kind)
      if ((kinds >> kind) & 1) sc.views[kind] = views[kind * 32 + k];
    sc.stand_ins = stand_ins - stand_ins_before;
    sc.bound = uint32_t(stats_.bound - bound_before);
  }
  return stand_ins;
}

TextureCache::Entry& TextureCache::Lookup(const uint32_t* tf, uint32_t host_kind, uint64_t frame) {
  // Content key: everything that changes the texel data or layout (not swizzle, signs, filters or clamps).
  const uint64_t a = (uint64_t(tf[0] & 0xFFC00000u) << 32) | (tf[1] & 0xFFFFF4FFu);
  // Patch 0029 (Gears 3 bug hunt E3): the max-level field (dword 4 bits 6-9) moved to bits 2-5 of the key so it no
  // longer overlaps dword 5 bit 9 (the dimension).
  const uint64_t b = (uint64_t(tf[2]) << 32) | (tf[5] & 0xFFFFFE00u) | ((tf[4] & 0x3C0u) >> 4) | host_kind;
  const Key key{a, b};
  // Job 284 diagnostic: hash a probed texture's bytes at every load (any frame), log lookups inside the frame window.
  const bool probe_base = !probe_bases_.empty() && Probed(tf, ~0ull);
  const bool probe = probe_base && Probed(tf, frame);
  auto done = [&](Entry& r, const char* what) -> Entry& {
    if (probe) Probe(r, tf, frame, what);
    return r;
  };
  ++stats_.lookups;  // patch 0137
  auto it = entries_.find(key);
  // Patch 0149 (deep-dive review): a texture whose host creation failed (memory pressure) was a stand-in for the rest of
  // the session; retried every 300 frames now (eviction may have freed memory). Slot caches may point at the entry.
  // Only after something was freed since the failure (review of 0149: a creation that always fails would otherwise
  // retry - and log an error - every 300 frames).
  if (it != entries_.end() && !it->second->texture && it->second->failure == "host texture creation failed" &&
      frame >= it->second->checked_frame + 300 && it->second->freed_at_load != freed_total_) {
    entries_.erase(it);
    it = entries_.end();
    for (SlotCache& sc : slot_cache_) sc.valid = false;
    ++create_retries_total_;
  }
  if (it == entries_.end()) {
    const uint64_t epoch_before = watch_.Epoch();  // a write during the load must trigger the next re-check
    auto e = std::make_unique<Entry>();
    Load(*e, tf, host_kind);
    e->loaded_frame = frame;
    e->freed_at_load = freed_total_;  // patch 0149
    if (probe_base) e->probe_load_hash = GuestHash(*e);
    if (load_log_frame_ && frame == load_log_frame_) LogLoad(*e, tf, frame, "new");
    if (e->texture) {
      ++stats_.textures;
      stats_.host_bytes += e->host_bytes;
    }
    it = entries_.emplace(key, std::move(e)).first;
    it->second->checked_frame = frame;
    it->second->callbacks_at_check = epoch_before;
    return done(*it->second, "new");
  }
  Entry& e = *it->second;
  if (!e.texture) return done(e, "stand-in");
  // Re-check only when something was written since the last check: the watch epoch advances on every Bump, the
  // callbacks' and our own resolves' alike (upl-n107: the callback counter alone missed resolves within a frame, so a
  // texture checked earlier in the frame kept stale content - job 109's green channel).
  const uint64_t epoch = watch_.Epoch();
  if (e.callbacks_at_check == epoch) {
    ++stats_.lookup_epoch_hits;  // patch 0137
    return done(e, "hit: no write anywhere since its last check");
  }
  e.checked_frame = frame;
  e.callbacks_at_check = epoch;
  const uint64_t version = watch_.Version(e.base_start, e.base_length) + watch_.Version(e.mip_start, e.mip_length);
  stats_.version_pages += ((uint64_t(e.base_length) + 4095) >> 12) + ((uint64_t(e.mip_length) + 4095) >> 12);  // 0137
  if (version == e.version) {
    ++stats_.lookup_version_hits;  // patch 0137
    return done(e, "hit: its pages not written since");
  }
  ++stats_.lookup_rechecks;  // patch 0137
  // Something in its pages was written (or a whole 4 MB block was bumped): re-arm the watch, then compare the bytes of
  // just the pages that moved. Unchanged bytes keep the upload (upl-n108: job 110's texture cost grew with the cache
  // because block bumps reloaded every texture in the block).
  // Patch 0029 (Gears 3 bug hunt E2): read the version BEFORE arming the watch - a write faulting in between then shows
  // as a newer version at the next check instead of hiding inside the one we keep.
  const uint64_t rearmed = watch_.Version(e.base_start, e.base_length) + watch_.Version(e.mip_start, e.mip_length);
  watch_.Watch(e.base_start, e.base_length);
  if (e.mip_length) watch_.Watch(e.mip_start, e.mip_length);
  const uint64_t gpu_pages0 = stats_.gpu_pages_changed, volatile0 = stats_.volatile_reloads;
  if (!always_reload_ && !PagesChanged(e)) {
    e.version = rearmed;
    ++stats_.content_unchanged;
    return done(e, "hit: pages written, bytes hashed unchanged");
  }
  if (stale_after_ && on_stale_ && e.loaded_frame != ~0ull && frame >= e.loaded_frame + stale_after_) {
    StaleReload s;
    s.base_start = e.base_start;
    s.base_length = e.base_length;
    s.mip_start = e.mip_start;
    s.mip_length = e.mip_length;
    s.guest_format = tf[1] & 0x3Fu;
    s.width = (tf[2] & 0x1FFFu) + 1;
    s.height = ((tf[2] >> 13) & 0x1FFFu) + 1;
    s.frame = frame;
    s.loaded_frame = e.loaded_frame;
    for (uint32_t p = e.base_start >> GuestWatch::kPageShift;
         e.base_length && p <= (e.base_start + e.base_length - 1) >> GuestWatch::kPageShift && !s.gpu_pages; ++p)
      s.gpu_pages = watch_.GpuCurrent(p);
    on_stale_(s);
  }
  e.loaded_frame = frame;
  Load(e, tf, host_kind);  // re-upload into the same host texture
  if (probe_base) e.probe_load_hash = GuestHash(e);
  const char* why = stats_.gpu_pages_changed != gpu_pages0 ? "reload: our GPU wrote its pages"
                    : stats_.volatile_reloads != volatile0 ? "reload: volatile (pages written, not hashed)"
                                                            : "reload: CPU changed its bytes";
  if (load_log_frame_ && frame == load_log_frame_) LogLoad(e, tf, frame, why);
  return done(e, why);
}

bool TextureCache::Probed(const uint32_t* tf, uint64_t frame) const {  // frame ~0: any frame
  if (frame != ~0ull && (frame < probe_first_ || frame > probe_last_)) return false;
  const uint32_t base = tf[1] & 0xFFFFF000u;
  return std::find(probe_bases_.begin(), probe_bases_.end(), base) != probe_bases_.end();
}

uint64_t TextureCache::GuestHash(const Entry& e) const {
  return e.base_length ? XXH3_64bits(guest_ + e.base_start, e.base_length) : 0;
}

void TextureCache::Probe(const Entry& e, const uint32_t* tf, uint64_t frame, const char* what) {
  const uint32_t base = tf[1] & 0xFFFFF000u;
  for (const auto& d : probe_done_)
    if (d.first == frame && d.second == base) return;  // once per frame and texture (the first lookup of the frame)
  probe_done_.push_back({frame, base});
  const uint64_t now = GuestHash(e);
  REXLOG_INFO("gow2_texture_probe: frame {} base {:08X} (+{:X}) fmt {} dim {} | {} | loaded frame {} | guest bytes {:016X}, "
              "at that load {:016X}{}",
              frame, e.base_start, e.base_length, tf[1] & 0x3Fu, (tf[5] >> 9) & 3u, what,
              e.loaded_frame == ~0ull ? -1ll : (long long)e.loaded_frame, now, e.probe_load_hash,
              now == e.probe_load_hash ? " (same)" : " (DIFFERENT: the cache holds other bytes than guest memory)");
  if (!probe_dir_.empty() && e.base_length) {
    char leaf[48];
    std::snprintf(leaf, sizeof(leaf), "/probe-%08X-%06llu.bin", base, static_cast<unsigned long long>(frame));
    const std::string name = probe_dir_ + leaf;
    if (FILE* f = std::fopen(name.c_str(), "wb")) {
      std::fwrite(guest_ + e.base_start, 1, e.base_length, f);
      std::fclose(f);
    }
  }
}

void TextureCache::LogLoad(const Entry& e, const uint32_t* tf, uint64_t frame, const char* reason) {
  const uint32_t w = (tf[2] & 0x1FFFu) + 1, h = ((tf[2] >> 13) & 0x1FFFu) + 1;
  REXLOG_INFO("gow2_renderer: texture load frame {}: {} | base {:08X} (+{:X}) mips {:08X} (+{:X}) format {} {}x{} dim {} "
              "tiled {} | host {} bytes{}",
              frame, reason, e.base_start, e.base_length, e.mip_start, e.mip_length, tf[1] & 0x3Fu, w, h,
              (tf[5] >> 9) & 3u, tf[0] >> 31, e.host_bytes, e.gpu_storage ? " | GPU untile" : " | CPU");
}

TextureCache::Entry* TextureCache::LookupScaled(const uint32_t* tf) {
  const uint32_t format = Bits(tf[1], 0, 6), endian = Bits(tf[1], 6, 2);
  const uint32_t dim = Bits(tf[5], 9, 2);
  const bool tiled = Bits(tf[0], 31, 1), stacked = Bits(tf[1], 10, 1), packed = Bits(tf[5], 11, 1);
  const uint32_t base = Bits(tf[1], 12, 20) << 12, mip_base = Bits(tf[5], 12, 20) << 12;
  const uint32_t w = Bits(tf[2], 0, 13) + 1, h = Bits(tf[2], 13, 13) + 1;
  const uint32_t pitch_div_32 = Bits(tf[0], 22, 9);
  // Job 189 diagnostic: why a texture our resolves wrote (any page GPU-current) is not taken at the scale.
  auto miss = [&](const char* why) -> Entry* {
    if (!scaled_miss_log_ || !base) return nullptr;  // job 227: the page walk below is the cost; off by default
    const uint32_t len = std::max<uint32_t>(pitch_div_32 * 32u, w) * h * 4u;
    bool ours = false;
    for (uint32_t p = base >> GuestWatch::kPageShift; p <= (base + std::max(len, 1u) - 1) >> GuestWatch::kPageShift && !ours; ++p)
      ours = watch_.GpuCurrent(p);
    if (ours) {
      char b[300];
      std::snprintf(b, sizeof(b), "%s: base %08X fmt %u %ux%u pitch %u dim %u tiled %u mips %u", why, base, format, w, h,
                    pitch_div_32 * 32u, dim, tiled ? 1u : 0u, mip_base ? Bits(tf[4], 6, 4) : 0u);
      ++stats_.scaled_misses[b];
    }
    return nullptr;
  };
  if (dim != 1 || stacked || !base) return miss("not a single 2D");
  if (mip_base && Bits(tf[4], 6, 4) != 0) return miss("mip chain");  // not a single resolved level
  const HostFormat hf = Host(format);
  const bool depth_convert = hf.convert == Convert::kDepthUnorm || hf.convert == Convert::kDepthFloat;
  if (!(hf.convert == Convert::kCopy || depth_convert) || hf.block_compressed) return miss("format conversion");
  const uint32_t code = hf.convert == Convert::kDepthUnorm ? 14u : hf.convert == Convert::kDepthFloat ? 15u
                                                                                                     : UntileCode(hf.format);
  if (!code || code == 12u) return miss("no GPU load code");
  const tx::FormatBlock& fb = tx::GetFormatBlock(format);
  const uint32_t bpb = fb.bytes_per_block();
  if ((bpb != 4 && bpb != 8) || fb.block_width != 1 || fb.block_height != 1) return miss("not 32/64 bpp");
  const tx::GuestLayout layout =
      tx::GetGuestLayout(tx::Dimension::k2DOrStacked, pitch_div_32, w, h, 1, tiled, format, packed, true, 0);
  const uint32_t bpb_log2 = tx::Log2Floor(bpb);
  const uint32_t pitch = layout.base.row_pitch_bytes / bpb;
  ScaledSource src;
  if (!scaled_lookup_(base, std::max(layout.base.level_data_extent_bytes, 1u), src) || !src.words) {
    static const char* kWhy[] = {"no scaled source", "no scaled-memory window holds the extent", "?", "?",
                                 "pages not scaled-current"};
    if (src.reason == 4) {
      char why[128];
      std::snprintf(why, sizeof(why), "pages not scaled-current (scaled %u, unscaled GPU %u, other %u, first at +%X)",
                    src.pages_scaled, src.pages_gpu, src.pages_other, src.first_bad);
      return miss(why);
    }
    return miss(kWhy[src.reason < 5 ? src.reason : 0]);
  }
  const Key key{(uint64_t(tf[0] & 0xFFC00000u) << 32) | (tf[1] & 0xFFFFF4FFu),
                (uint64_t(tf[2]) << 32) | (tf[5] & 0xFFFFFE00u) | 8u};
  auto it = scaled_entries_.find(key);
  if (it == scaled_entries_.end()) {
    auto e = std::make_unique<Entry>();
    gpu::TextureDesc td;
    td.width = w * scale_;
    td.height = h * scale_;
    td.format = hf.format;
    td.usage = gpu::kUsageSampled | gpu::kUsageStorage;
    td.debug_name = "gow2 scaled resolve texture";
    e->texture = backend_.CreateTexture(td);
    if (!e->texture) return nullptr;
    e->format = hf.format;
    e->gpu_storage = true;
    e->host_bytes = uint64_t(td.width) * td.height * hf.host_bytes;
    ++stats_.scaled_textures;
    stats_.scaled_bytes += e->host_bytes;
    it = scaled_entries_.emplace(key, std::move(e)).first;
  }
  Entry& e = *it->second;
  if (e.scaled_gen != src.gen || e.scaled_words != src.words) {
    gpu::ComputeBindings b;
    b.constants[0] = base;
    b.constants[1] = pitch;
    b.constants[4] = w * scale_;
    b.constants[5] = h * scale_;
    b.constants[6] = bpb_log2;
    b.constants[7] = endian;
    b.constants[8] = tiled ? 1u : 0u;
    b.constants[9] = code;
    b.constants[10] = scale_;
    b.constants[11] = src.chunk_base;
    b.constants[12] = src.chunk_width;
    b.constants[13] = src.chunk_bytes;
    b.constant_count = 16;
    b.srv[0] = src.words;
    b.uav_texture = e.texture;
    backend_.SetGpuCategory(3);
    backend_.Dispatch(scaled_load_, b, (w * scale_ + 7) / 8, (h * scale_ + 7) / 8, 1);
    e.scaled_gen = src.gen;
    e.scaled_words = src.words;
    ++stats_.scaled_loads;
  }
  return &e;
}

bool TextureCache::PagesChanged(Entry& e, bool from_check) {
  bool changed = false;
  // Volatile after 2 changed checks in a row; every 32nd check of a volatile texture hashes again, so one that settles
  // returns to content validation.
  if (from_check) ++e.checks;
  const bool volatile_check = from_check && e.changed_streak >= 2 && (e.checks & 31u) != 0;
  if (from_check && e.changed_streak >= 2 && !volatile_check) e.hashes_stale = true;
  const uint32_t starts[2] = {e.base_start, e.mip_start}, lengths[2] = {e.base_length, e.mip_length};
  constexpr uint32_t kShift = GuestWatch::kPageShift;
  for (uint32_t r = 0; r < 2; ++r) {
    std::vector<uint32_t>& pv = e.page_versions[r];
    std::vector<uint64_t>& ph = e.page_hashes[r];
    if (!lengths[r]) {
      pv.clear();
      ph.clear();
      continue;
    }
    const uint32_t first = starts[r] >> kShift, last = (starts[r] + lengths[r] - 1) >> kShift;
    const bool fresh =
        pv.size() != last - first + 1 || e.hashed_start[r] != starts[r] || e.hashed_length[r] != lengths[r];
    if (fresh) {
      pv.assign(last - first + 1, ~0u);
      ph.assign(last - first + 1, 0);
      e.hashed_start[r] = starts[r];
      e.hashed_length[r] = lengths[r];
      changed = true;
    }
    for (uint32_t p = first; p <= last; ++p) {
      const uint32_t v = watch_.PageVersion(p);  // read before hashing: a write during the hash bumps it again
      if (v == pv[p - first]) continue;
      pv[p - first] = v;
      if (watch_.GpuCurrent(p)) {  // T2: written by our resolve - its CPU bytes don't show the change
        ++stats_.gpu_pages_changed;
        changed = true;
        continue;
      }
      if (volatile_check) {
        // A texture whose bytes changed at its last checks (resolve targets, streamed data): reload without hashing
        // (upl-n109: job 114 hashed 1.4-2.7 M pages per 300 frames, mostly these). Its hashes are stale until the next
        // full pass.
        ++stats_.pages_skipped;
        changed = true;
        continue;
      }
      const uint32_t a = std::max(p << kShift, starts[r]);
      const uint32_t b = std::min((p + 1) << kShift, starts[r] + lengths[r]);
      const uint64_t h = XXH3_64bits(guest_ + a, size_t(b - a));
      ++stats_.pages_hashed;
      if (h != ph[p - first]) {
        ph[p - first] = h;
        changed = true;
      }
    }
  }
  if (!from_check) return changed;
  if (!volatile_check) {
    // A full pass after stale hashes compares against old content: count it as a change only if it repeats.
    if (e.hashes_stale && changed) {
      e.hashes_stale = false;
      e.changed_streak = 1;
      return true;
    }
    e.hashes_stale = false;
  }
  e.changed_streak = changed ? e.changed_streak + 1 : 0;
  if (volatile_check) ++stats_.volatile_reloads;
  return changed;
}

bool TextureCache::FindResolveThrough(uint32_t base, uint32_t pitch, uint32_t bpb_log2, uint32_t dx, uint32_t dy,
                                      uint32_t w, uint32_t h, ResolveThrough& out) {
  auto [a, b] = through_.equal_range(base);
  const uint32_t pitch_aligned = (pitch + 31u) & ~31u;
  for (auto it = a; it != b; ++it) {
    Entry& e = *it->second;
    if (!e.through_ok || !e.texture || e.through_pitch != pitch_aligned || e.through_bpb_log2 != bpb_log2) continue;
    // Wholly covered: the resolve defines every texel, whatever the texture held. Else it must be current now.
    const bool covers = dx == 0 && dy == 0 && w >= e.width && h >= e.height;
    const uint64_t v = watch_.Version(e.base_start, e.base_length);
    if (!covers && v != e.version) {
      ++stats_.through_out_of_date;
      continue;
    }
    out.texture = e.texture;
    out.code = e.through_code;
    out.endian = e.through_endian;
    out.width = e.width;
    out.height = e.height;
    out.entry = &e;
    out.version_before = v;
    out.covers = covers;
    return true;
  }
  return false;
}

void TextureCache::ResolveThroughWritten(const ResolveThrough& t, uint32_t extent_start, uint32_t extent_length) {
  Entry& e = *static_cast<Entry*>(t.entry);
  constexpr uint32_t kShift = GuestWatch::kPageShift;
  extent_start &= 0x1FFFFFFFu;
  const uint64_t s0 = std::max<uint64_t>(extent_start, e.base_start);
  const uint64_t s1 = std::min<uint64_t>(uint64_t(extent_start) + extent_length, uint64_t(e.base_start) + e.base_length);
  if (s1 <= s0) return;  // the resolve wrote none of its bytes (cannot happen for the same base; keep the reload)
  const uint32_t first = uint32_t(s0 >> kShift), last = uint32_t((s1 - 1) >> kShift);
  if (t.covers) {
    watch_.Watch(e.base_start, e.base_length);  // re-arm: an earlier CPU write may have unwatched its pages
    e.version = watch_.Version(e.base_start, e.base_length);
  } else {
    // MarkGpuWritten bumped each page it wrote once; anything else moved = a write we did not account for.
    const uint64_t v = watch_.Version(e.base_start, e.base_length);
    if (v != t.version_before + (last - first + 1)) {
      ++stats_.through_raced;
      return;  // version left behind: the next fetch reloads it
    }
    e.version = v;
  }
  // The pages it wrote: their new versions are accounted for (no "changed" at the next content check, no mirror copy).
  const uint32_t efirst = e.base_start >> kShift;
  for (uint32_t p = first; p <= last; ++p) {
    if (!watch_.GpuCurrent(p)) continue;
    const uint32_t v = watch_.PageVersion(p);
    if (e.page_versions[0].size() > p - efirst && e.hashed_start[0] == e.base_start) e.page_versions[0][p - efirst] = v;
    if (e.copied_pages[0].size() > p - efirst && e.copied_start[0] == e.base_start) e.copied_pages[0][p - efirst] = v;
  }
  ++stats_.through_writes;
}

gpu::TextureHandle TextureCache::UntileCopyForCheck(const ResolveThrough& t, uint32_t& texel_bytes) {
  const Entry& e = *static_cast<const Entry*>(t.entry);
  if (!untile_ || !e.width || !e.height) return 0;
  texel_bytes = uint32_t(e.host_bytes / (uint64_t(e.width) * e.height));
  gpu::TextureDesc td;
  td.width = e.width;
  td.height = e.height;
  td.format = e.format;
  td.usage = gpu::kUsageSampled | gpu::kUsageStorage;
  td.debug_name = "gow2 resolve-through check";
  const gpu::TextureHandle tex = backend_.CreateTexture(td);
  if (!tex) return 0;
  gpu::ComputeBindings b;  // as Load's level 0
  b.constants[0] = e.through_base;
  b.constants[1] = e.through_pitch;
  b.constants[4] = e.width;
  b.constants[5] = e.height;
  b.constants[6] = e.through_bpb_log2;
  b.constants[7] = e.through_endian;
  b.constants[8] = 1u;
  b.constants[9] = e.through_code;
  b.constant_count = 12;
  b.guest_t3 = true;
  b.uav_texture = tex;
  backend_.Dispatch(untile_, b, (e.width + 7) / 8, (e.height + 7) / 8, 1);
  return tex;
}

void TextureCache::EndFrame(uint64_t frame) {
  {  // job 299 diagnostic
    const double limit = REXCVAR_GET(gow2_texture_phase_log_ms);
    static uint32_t logged = 0;
    if (limit > 0.0 && g_phase.load > limit && logged < 200) {
      ++logged;
      const TexturePhases& t = g_phase;
      REXLOG_INFO("gow2_textures: frame {}: {} loads {:.1f} ms | watch {:.1f} | hash {:.1f} | create {:.1f} ({}) | GPU path "
                  "{:.1f} ({}) | CPU untile {:.1f} ({}) | CPU upload {:.1f} | other {:.1f}",
                  frame, t.loads, t.load, t.watch, t.hash, t.create, t.creates, t.gpu, t.gpu_loads, t.cpu, t.cpu_loads,
                  t.upload, t.load - t.watch - t.hash - t.create - t.gpu - t.cpu - t.upload);
    }
    g_phase = {};
  }
  // Patch 0149 (deep-dive review): scaled resolve textures (scale > 1) were never freed and are outside the cap; one
  // unused for 3600 frames is dropped now (it is made again from its resolve if a draw needs it).
  if ((frame & 511u) == 0 && frame > 3600 && !scaled_entries_.empty()) {
    uint32_t dropped = 0;
    for (auto it = scaled_entries_.begin(); it != scaled_entries_.end();) {
      if (it->second->last_used + 3600 < frame) {
        if (it->second->texture) backend_.DestroyTexture(it->second->texture);  // deferred past our GPU by the backend
        stats_.scaled_bytes -= std::min(stats_.scaled_bytes, it->second->host_bytes);
        if (stats_.scaled_textures) --stats_.scaled_textures;
        it = scaled_entries_.erase(it);
        ++dropped;
        ++freed_total_;
      } else {
        ++it;
      }
    }
    if (dropped) {
      for (SlotCache& sc : slot_cache_) sc.valid = false;  // they may point at dropped entries
      scaled_evicted_total_ += dropped;
    }
  }
  if (!cap_bytes_ || stats_.host_bytes <= cap_bytes_) return;
  std::vector<std::pair<uint64_t, const Key*>> old;
  for (const auto& [key, e] : entries_)
    if (e->texture && e->last_used + 1 < frame) old.push_back({e->last_used, &key});
  std::sort(old.begin(), old.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  const uint64_t target = cap_bytes_ / 10 * 9;
  std::vector<Key> evict;
  uint64_t bytes = stats_.host_bytes;
  for (const auto& [used, key] : old) {
    if (bytes <= target) break;
    bytes -= entries_.at(*key)->host_bytes;
    evict.push_back(*key);
  }
  if (evict.empty()) return;
  for (const Key& k : evict) {
    auto it = entries_.find(k);
    Entry& e = *it->second;
    backend_.DestroyTexture(e.texture);  // deferred by the backend until our GPU is past it
    stats_.host_bytes -= e.host_bytes;
    stats_.evicted_bytes += e.host_bytes;
    --stats_.textures;
    ++stats_.evictions;
    ++freed_total_;  // patch 0149
    if (e.through_indexed) {
      auto [a, b] = through_.equal_range(e.through_base);
      for (auto t = a; t != b; ++t)
        if (t->second == &e) {
          through_.erase(t);
          break;
        }
    }
    entries_.erase(it);
  }
  for (SlotCache& sc : slot_cache_) sc.valid = false;  // they may point at evicted entries
}

void TextureCache::Load(Entry& e, const uint32_t* tf, uint32_t host_kind) {
  PhaseScope phase_load(g_phase.load);  // job 299
  ++g_phase.loads;
  const uint32_t format = Bits(tf[1], 0, 6), endian = Bits(tf[1], 6, 2);
  const HostFormat hf = Host(format);
  if (hf.convert == Convert::kNone) {
    e.failure = "unsupported format " + std::to_string(format);
    return;
  }
  const uint32_t dim = Bits(tf[5], 9, 2);  // 0 1D, 1 2D/stacked, 2 3D, 3 cube
  const bool tiled = Bits(tf[0], 31, 1), stacked = Bits(tf[1], 10, 1), packed = Bits(tf[5], 11, 1);
  const uint32_t base = Bits(tf[1], 12, 20) << 12, mip_base = Bits(tf[5], 12, 20) << 12;
  const uint32_t pitch_div_32 = Bits(tf[0], 22, 9);
  uint32_t w = 1, h = 1, d = 1;
  tx::Dimension gd = tx::Dimension::k2DOrStacked;
  if (dim == 0) {
    w = Bits(tf[2], 0, 24) + 1;
    gd = tx::Dimension::k1D;
  } else if (dim == 2) {
    w = Bits(tf[2], 0, 11) + 1;
    h = Bits(tf[2], 11, 11) + 1;
    d = Bits(tf[2], 22, 10) + 1;
    gd = tx::Dimension::k3D;
  } else {
    w = Bits(tf[2], 0, 13) + 1;
    h = Bits(tf[2], 13, 13) + 1;
    d = dim == 3 ? 6 : (stacked ? Bits(tf[2], 26, 6) + 1 : 1);
    gd = dim == 3 ? tx::Dimension::kCube : tx::Dimension::k2DOrStacked;
  }
  if (!base) {
    e.failure = "no base address";
    return;
  }
  // Host shape from how the shader fetches it.
  if (host_kind == 1 && dim != 3) {
    e.failure = "cube fetch of a non-cube texture";
    e.dimension_mismatch = true;
    return;
  }
  if (host_kind != 1 && dim == 3) {
    e.failure = "2D/3D fetch of a cube texture";
    e.dimension_mismatch = true;
    return;
  }
  const tx::FormatBlock& fb = tx::GetFormatBlock(format);
  const uint32_t bpb = fb.bytes_per_block();
  if (!bpb) {
    e.failure = "format with no block size";
    return;
  }
  if (hf.block_compressed && ((w & 3) || (h & 3))) {
    e.failure = "BC texture not 4-aligned (decompression not implemented)";
    return;
  }
  // Patch 0087 (Canary 344fca7, texture_util.cc GetSubresourcesFromFetchConstant): level 0 already packed (no 1D) - the
  // whole tail is at the base address, also with no mip address (D3D leaves it at 0 then).
  const bool tail_at_base = packed_tail_at_base_ && packed && dim != 0 && !mip_base && Bits(tf[4], 6, 4) != 0 &&
                            tx::PackedMipLevel(w, h) == 0;
  if (tail_at_base) ++stats_.packed_tail_at_base;
  if (Bits(tf[4], 22, 10) != 0) ++stats_.grad_exp_bias_loads;
  uint32_t max_level = mip_base || tail_at_base ? Bits(tf[4], 6, 4) : 0;
  max_level = std::min(max_level, tx::Log2Floor(std::max(std::max(w, h), dim == 2 ? d : 1u)));
  const tx::GuestLayout layout = tx::GetGuestLayout(gd, pitch_div_32, w, h, d, tiled, format, packed, true, max_level);
  const uint32_t mips = layout.max_level + 1;
  {  // Outside review: a fetch constant whose base or mip range runs past the 512 MB guest memory would be hashed and
    // read out of bounds below - refuse it (the texture is skipped) instead.
    constexpr uint64_t kGuestBytes = 512ull << 20;
    const uint64_t mip_at = layout.packed_level == 0 && (mip_base || tail_at_base) ? base : mip_base;
    if (uint64_t(base) + std::max(layout.base.level_data_extent_bytes, 1u) > kGuestBytes ||
        (mips > 1 && mip_at + std::max(layout.mips_total_extent_bytes, 1u) > kGuestBytes)) {
      e.failure = "texture range past the end of guest memory";
      return;
    }
  }
  const uint32_t layers = gd == tx::Dimension::k3D ? 1 : layout.array_size;
  const bool host_3d = host_kind == 2;
  // GPU path: raw 2D textures (one layer) in a format with typed UAV stores (D3D11-class hardware guarantees these).
  // Takeover T3: 24:8 depth textures (shadow maps, scene depth - resolve destinations) also load on the GPU, from the
  // guest-memory mirror where our resolves put them (codes 14/15: the CPU path's kDepthUnorm / kDepthFloat).
  const uint32_t gpu_code = hf.convert == Convert::kDepthUnorm  ? 14u
                            : hf.convert == Convert::kDepthFloat ? 15u
                                                                 : UntileCode(hf.format);
  const bool depth_convert = hf.convert == Convert::kDepthUnorm || hf.convert == Convert::kDepthFloat;
  // Gears 3 (job 020): a stacked 2D texture fetched as 3D (host_kind 2) - the 16x16x16 colour-grading table the game
  // renders and resolves every frame - also loads on the GPU, one layer per z slice (untile3d_), so it reads our
  // resolves from the mirror instead of stale CPU bytes. Base level only (host_mips is 1 for this case).
  const bool stacked_3d = untile3d_ && gd == tx::Dimension::k2DOrStacked && host_kind == 2 && layers >= 1;
  // Patch 0026: true 3D textures fetched as 3D, in a format with a GPU load code, untiled on the GPU (untile_volume_).
  const bool volume = untile_volume_ && gd == tx::Dimension::k3D && host_kind == 2;
  const bool gpu = untile_ && (hf.convert == Convert::kCopy || depth_convert) && !hf.block_compressed && gpu_code &&
                   ((gd == tx::Dimension::k2DOrStacked && ((layers == 1 && host_kind == 0) || stacked_3d)) || volume) &&
                   fb.block_width == 1 && fb.block_height == 1;
  // Patch 0021 (Canary texture_cache, D36): BC1-5 textures load on the GPU too, from the mirror (where our resolves and
  // memexports are), not from guest CPU memory: blocks into the backend's scratch buffer, then a buffer->texture copy.
  // 2D (one layer), arrays fetched per layer are not host-shaped here, so: one layer, or a cube's 6 faces; 3D stays CPU.
  // Patch 0025: also 32bpp raw-copy cube textures (8888, 2_10_10_10, 16_16, 32_FLOAT ... faces), which the typed-UAV
  // path above never took (one layer only), so their faces no longer load from guest CPU memory either.
  const bool gpu_blocks =
      !gpu && untile_blocks_ && hf.convert == Convert::kCopy && gd != tx::Dimension::k3D && !host_3d &&
      ((hf.block_compressed && (bpb == 8 || bpb == 16) &&
        ((layers == 1 && host_kind == 0) || (host_kind == 1 && layers == 6))) ||
       (!hf.block_compressed && bpb == 4 && fb.block_width == 1 && fb.block_height == 1 && host_kind == 1 &&
        layers == 6 && hf.host_bytes == 4));
  // Host texture (created once; re-loads write into it).
  if (!e.texture) {
    gpu::TextureDesc td;
    td.dimension = host_kind == 1 ? gpu::TextureDimension::kCube
                   : host_3d      ? gpu::TextureDimension::k3D
                                  : gpu::TextureDimension::k2D;
    td.width = w;
    td.height = h;
    td.depth_or_layers = host_3d ? (gd == tx::Dimension::k3D ? d : layers) : (host_kind == 1 ? 6 : 1);
    td.mips = host_3d && gd != tx::Dimension::k3D ? 1 : mips;  // a stacked texture viewed as 3D: base level only
    td.format = hf.format;
    td.usage = gpu::kUsageSampled | (gpu ? gpu::kUsageStorage : 0u);
    td.debug_name = "gow2 guest texture";
    {
      PhaseScope ps(g_phase.create);  // job 299
      ++g_phase.creates;
      e.texture = backend_.CreateTexture(td);
    }
    if (!e.texture) {
      e.failure = "host texture creation failed";
      return;
    }
    e.format = hf.format;
    e.gpu_storage = gpu;
    uint64_t bytes = 0;
    for (uint32_t m = 0; m < td.mips; ++m)
      bytes += uint64_t(std::max(w >> m, 1u)) * std::max(h >> m, 1u) * td.depth_or_layers * hf.host_bytes /
               (uint64_t(fb.block_width) * fb.block_height);
    e.host_bytes = bytes;
  }
  // Watch the guest ranges before reading them, so a write during the copy bumps the version.
  e.base_start = base;
  e.base_length = std::max(layout.base.level_data_extent_bytes, 1u);
  // Patch 0022 (Canary 344fca7): when level 0 is already the packed tail, the whole tail is at the base address even if
  // the fetch constant has a mip address - watch the bytes actually read.
  e.mip_start = layout.packed_level == 0 && (mip_base || tail_at_base) ? base : mip_base;
  e.mip_length = mips > 1 ? std::max(layout.mips_total_extent_bytes, 1u) : 0;
  e.version = watch_.Version(e.base_start, e.base_length) + watch_.Version(e.mip_start, e.mip_length);  // 0029: first
  {
    PhaseScope ps(g_phase.watch);  // job 299
    watch_.Watch(e.base_start, e.base_length);
    if (e.mip_length) watch_.Watch(e.mip_start, e.mip_length);
  }
  {
    PhaseScope ps(g_phase.hash);  // job 299
    (void)PagesChanged(e, false);  // record the page hashes this load reads (a fresh entry hashes every page once)
  }

  const uint32_t bpb_log2 = tx::Log2Floor(bpb);
  const uint32_t host_mips = host_3d && gd != tx::Dimension::k3D ? 1 : mips;
  if ((gpu && e.gpu_storage) || gpu_blocks) {
    PhaseScope phase_gpu(g_phase.gpu);  // job 299
    ++g_phase.gpu_loads;
    // Job 047: may a resolve into this memory write this texture directly?
    e.through_ok = !gpu_blocks && !stacked_3d && host_kind == 0 && layers == 1 && tiled && !e.mip_length && (bpb_log2 == 2 || bpb_log2 == 3) &&
                   layout.packed_level != 0;
    if (e.through_ok) {
      e.through_base = base;
      e.through_pitch = ((layout.base.row_pitch_bytes >> bpb_log2) + 31u) & ~31u;
      e.through_bpb_log2 = bpb_log2;
      e.through_code = gpu_code;
      e.through_endian = endian;
      e.width = w;
      e.height = h;
      if (!e.through_indexed) {
        through_.emplace(base, &e);
        e.through_indexed = true;
      }
    }
    // Copy the guest bytes to the GPU's guest-memory mirror - only the pages written since the last copy - then
    // untile each level there.
    const uint32_t starts[2] = {e.base_start, e.mip_start}, lengths[2] = {e.base_length, e.mip_length};
    for (uint32_t r = 0; r < 2; ++r) {
      if (!lengths[r]) continue;
      constexpr uint32_t kShift = GuestWatch::kPageShift;
      const uint32_t first = starts[r] >> kShift, last = (starts[r] + lengths[r] - 1) >> kShift;
      std::vector<uint32_t>& pv = e.copied_pages[r];
      const bool fresh = pv.size() != last - first + 1 || e.copied_start[r] != starts[r] || e.copied_length[r] != lengths[r];
      if (fresh) pv.assign(last - first + 1, ~0u);
      e.copied_start[r] = starts[r];
      e.copied_length[r] = lengths[r];
      uint32_t run = UINT32_MAX;
      auto flush = [&](uint32_t end_page) {
        if (run == UINT32_MAX) return;
        const uint32_t a = std::max(run << kShift, starts[r]);
        const uint32_t b = std::min(end_page << kShift, starts[r] + lengths[r]);
        if (b > a) {
          backend_.UploadGuestMemory(a, guest_ + a, b - a);
          stats_.gpu_load_bytes += b - a;
        }
        run = UINT32_MAX;
      };
      for (uint32_t p = first; p <= last; ++p) {
        const uint32_t v = watch_.PageVersion(p);
        if (v != pv[p - first]) {
          pv[p - first] = v;
          if (watch_.GpuCurrent(p)) {  // T2: our resolve wrote this page into the mirror; the CPU bytes are older
            flush(p);
            ++stats_.gpu_pages_kept;
            continue;
          }
          if (run == UINT32_MAX) run = p;
        } else {
          flush(p);
        }
      }
      flush(last + 1);
    }
    for (uint32_t level = 0; level < host_mips; ++level) {
      const bool in_tail = layout.packed_level != UINT32_MAX && level >= layout.packed_level;
      const uint32_t storage_level = in_tail ? layout.packed_level : level;
      const tx::GuestLayout::Level& L = storage_level == 0 ? layout.base : layout.mips[storage_level];
      const uint32_t storage_base = storage_level == 0 ? base : mip_base + layout.mip_offsets_bytes[storage_level];
      if (storage_level != 0 && !mip_base) break;
      uint32_t ox = 0, oy = 0, oz = 0;
      if (in_tail) tx::PackedMipOffset(w, h, 1, format, level, ox, oy, oz);
      const uint32_t lw = std::max(w >> level, 1u), lh = std::max(h >> level, 1u);
      if (gpu_blocks) {  // patch 0021: per layer (cube face), blocks -> scratch buffer -> texture
        const uint32_t bw = (lw + fb.block_width - 1) / fb.block_width, bh = (lh + fb.block_height - 1) / fb.block_height;
        for (uint32_t u = 0; u < layers; ++u) {
          uint32_t row_pitch = 0, rows = 0;
          if (!backend_.PrepareScratchFor(e.texture, level, u, row_pitch, rows) || row_pitch < (bw << bpb_log2)) {
            ++stats_.gpu_block_failures;
            continue;
          }
          gpu::ComputeBindings bb;
          bb.constants[0] = storage_base + u * L.array_slice_stride_bytes;
          bb.constants[1] = L.row_pitch_bytes / bpb;
          bb.constants[2] = ox;
          bb.constants[3] = oy;
          bb.constants[4] = bw;
          bb.constants[5] = std::min(bh, rows);
          bb.constants[6] = bpb_log2;
          bb.constants[7] = endian;
          bb.constants[8] = tiled ? 1u : 0u;
          bb.constants[9] = row_pitch;
          bb.constant_count = 12;
          bb.guest_t3 = true;
          bb.scratch_u0 = true;
          backend_.SetGpuCategory(3);
          backend_.Dispatch(untile_blocks_, bb, (bw + 7) / 8, (std::min(bh, rows) + 7) / 8, 1);
          backend_.CopyScratchToTexture(e.texture, level, u);
          ++stats_.gpu_block_subresources;
        }
        continue;
      }
      gpu::ComputeBindings b;
      b.constants[0] = storage_base;
      b.constants[1] = L.row_pitch_bytes / bpb;
      b.constants[2] = ox;
      b.constants[3] = oy;
      b.constants[4] = lw;
      b.constants[5] = lh;
      b.constants[6] = bpb_log2;
      b.constants[7] = endian;
      b.constants[8] = tiled ? 1u : 0u;
      b.constants[9] = gpu_code;
      b.constant_count = 12;
      b.guest_t3 = true;
      b.uav_texture = e.texture;
      b.uav_mip = level;
      backend_.SetGpuCategory(3);  // job 177: texture loads (the next draw part sets draws again)
      if (volume) {  // patch 0026: one dispatch per level, z from the packed-tail offset
        const uint32_t ld = std::max(d >> level, 1u);
        uint32_t vx = 0, vy = 0, vz = 0;
        if (in_tail) tx::PackedMipOffset(w, h, d, format, level, vx, vy, vz);
        b.constants[2] = vx;
        b.constants[3] = vy;
        b.constants[10] = vz;
        b.constants[11] = ld;
        b.constants[12] = L.z_slice_stride_block_rows;
        b.constant_count = 16;
        backend_.Dispatch(untile_volume_, b, (lw + 3) / 4, (lh + 3) / 4, (ld + 3) / 4);
        if (level + 1 == host_mips) ++stats_.gpu_volume_loads;
        continue;
      }
      if (stacked_3d) {  // one dispatch per layer: layer z at storage_base + z * slice stride, into host slice z
        for (uint32_t z = 0; z < layers; ++z) {
          b.constants[0] = storage_base + z * L.array_slice_stride_bytes;
          b.constants[10] = z;
          backend_.Dispatch(untile3d_, b, (lw + 7) / 8, (lh + 7) / 8, 1);
        }
        ++stats_.gpu_loads_stacked;
        break;  // base level only
      }
      backend_.Dispatch(untile_, b, (lw + 7) / 8, (lh + 7) / 8, 1);
    }
    if (gpu_blocks) ++stats_.gpu_block_loads;
    ++stats_.gpu_loads;
    ++stats_.uploads;
    return;
  }
  {  // T2: a CPU-path load reads CPU bytes; pages our resolves wrote only exist in the GPU mirror
    const uint32_t starts[2] = {e.base_start, e.mip_start}, lengths[2] = {e.base_length, e.mip_length};
    bool any = false;
    for (uint32_t r = 0; r < 2 && !any; ++r)
      if (lengths[r])
        for (uint32_t p = starts[r] >> GuestWatch::kPageShift;
             p <= (starts[r] + lengths[r] - 1) >> GuestWatch::kPageShift && !any; ++p)
          any = watch_.GpuCurrent(p);
    if (any && before_cpu_load_) {
      // Job 042: pending memexport readbacks over these pages land in guest memory now, before the bytes are read.
      for (uint32_t r = 0; r < 2; ++r)
        if (lengths[r]) before_cpu_load_(starts[r], lengths[r]);
    }
    if (any) {
      ++stats_.cpu_loads_of_gpu_pages;
      // Gears 3 job 019: one of these per frame (Gears 2: 0). Name the first few, to see which texture and why it
      // is not on the GPU path.
      static int logged = 0;
      if (logged < 6) {
        ++logged;
        REXLOG_INFO("gow2_renderer: CPU-path load reads our resolve pages: base {:08X} (+{:X}) mips {:08X} (+{:X}) "
                    "format {} {}x{}x{} dim {} kind {} tiled {} layers {} convert {} block {}x{} bc {} untile code {}",
                    e.base_start, e.base_length, e.mip_start, e.mip_length, format, w, h, d, dim, host_kind,
                    tiled ? 1 : 0, layers, int(hf.convert), fb.block_width, fb.block_height, hf.block_compressed ? 1 : 0,
                    gpu_code);
      }
    }
  }
  ++g_phase.cpu_loads;  // job 299
  ++stats_.cpu_loads;    // patch 0021
  const auto cpu_t0 = std::chrono::steady_clock::now();
  double cpu_upload = 0;
  for (uint32_t level = 0; level < host_mips; ++level) {
    const bool in_tail = layout.packed_level != UINT32_MAX && level >= layout.packed_level;
    const uint32_t storage_level = in_tail ? layout.packed_level : level;
    const tx::GuestLayout::Level& L = storage_level == 0 ? layout.base : layout.mips[storage_level];
    const uint32_t storage_base = storage_level == 0 ? base : mip_base + layout.mip_offsets_bytes[storage_level];
    if (storage_level != 0 && !mip_base) break;
    uint32_t ox = 0, oy = 0, oz = 0;
    if (in_tail) tx::PackedMipOffset(w, h, gd == tx::Dimension::k3D ? d : 1, format, level, ox, oy, oz);
    const uint32_t lw = std::max(w >> level, 1u), lh = std::max(h >> level, 1u);
    const uint32_t ld = gd == tx::Dimension::k3D ? std::max(d >> level, 1u) : 1u;
    const uint32_t bw = (lw + fb.block_width - 1) / fb.block_width, bh = (lh + fb.block_height - 1) / fb.block_height;
    const uint32_t row_pitch_blocks = L.row_pitch_bytes / bpb;
    // Host rows: for the CPU-decoded block formats a guest block is 4 rows of texels.
    const bool decoded = hf.convert == Convert::kDxt3aToR8 || hf.convert == Convert::kCtx1ToRG8;
    const uint32_t texel_bytes = decoded ? hf.host_bytes / 16 : hf.host_bytes;
    const uint32_t host_w = decoded ? lw : bw, host_h = decoded ? lh : bh;
    const uint32_t row_bytes = host_w * texel_bytes;
    const uint32_t slices = host_3d ? (gd == tx::Dimension::k3D ? ld : layers) : 1;
    // Patch 0024 (Gears 3 review 02-e): the host texture has 6 layers for a cube, else one (2D) - a stacked texture
    // fetched as 2D uploads only layer 0 (the old loop wrote layers 1+ past the texture: invalid copies).
    const uint32_t uploads = host_3d ? 1 : (host_kind == 1 ? layers : 1u);
    for (uint32_t u = 0; u < uploads; ++u) {
      scratch_.assign(size_t(row_bytes) * host_h * slices, 0);
      for (uint32_t z = 0; z < slices; ++z) {
        const uint32_t layer = host_3d ? (gd == tx::Dimension::k3D ? 0 : z) : u;
        const uint32_t zz = gd == tx::Dimension::k3D ? z : 0;
        // Fast path (upl-n103 CPU): tiled 2D raw copies move 16-byte runs, the largest pieces Xenos tiling keeps
        // contiguous (TiledCombine keeps offset bits 0-3), with the endian swap done per run.
        const bool fast = tiled && gd != tx::Dimension::k3D && hf.convert == Convert::kCopy && bpb <= 16 &&
                          texel_bytes == bpb;
        // Contiguous bytes: offset bits 0-3, of which x supplies 3 << bpb_log2 bits (8 bytes at 8bpp).
        const uint32_t run_bytes = std::min(16u, 8u << bpb_log2);
        const uint32_t run_blocks = fast ? (run_bytes >> bpb_log2) : 1u;
        for (uint32_t by = 0; by < bh; ++by) {
          for (uint32_t bx = 0; bx < bw; ++bx) {
            if (fast && ((ox + bx) & (run_blocks - 1)) == 0 && bx + run_blocks <= bw) {
              const int64_t roff = tx::TiledOffset2D(int32_t(ox + bx), int32_t(oy + by), row_pitch_blocks, bpb_log2);
              const uint64_t raddr = uint64_t(storage_base) + uint64_t(layer) * L.array_slice_stride_bytes + uint64_t(roff);
              if (roff >= 0 && raddr + run_bytes <= (512ull << 20)) {
                uint8_t* dst = scratch_.data() + size_t(z) * row_bytes * host_h + size_t(by) * row_bytes +
                               size_t(bx) * texel_bytes;
                std::memcpy(dst, guest_ + raddr, run_bytes);
                SwapUnits(dst, run_bytes, endian);
                bx += run_blocks - 1;
                continue;
              }
            }
            int64_t off;
            if (tiled) {
              off = gd == tx::Dimension::k3D
                        ? tx::TiledOffset3D(int32_t(ox + bx), int32_t(oy + by), int32_t(oz + zz), row_pitch_blocks,
                                            L.z_slice_stride_block_rows, bpb_log2)
                        : tx::TiledOffset2D(int32_t(ox + bx), int32_t(oy + by), row_pitch_blocks, bpb_log2);
            } else {
              off = int64_t(oz + zz) * L.row_pitch_bytes * L.z_slice_stride_block_rows +
                    int64_t(oy + by) * L.row_pitch_bytes + int64_t(ox + bx) * bpb;
            }
            const uint64_t addr = uint64_t(storage_base) + uint64_t(layer) * L.array_slice_stride_bytes + uint64_t(off);
            if (off < 0 || addr + bpb > (512ull << 20)) continue;
            uint8_t block[16];
            std::memcpy(block, guest_ + addr, std::min<uint32_t>(bpb, 16));
            SwapUnits(block, bpb, endian);
            uint8_t* slice_base = scratch_.data() + size_t(z) * row_bytes * host_h;
            switch (hf.convert) {
              case Convert::kCopy:
                std::memcpy(slice_base + size_t(by) * row_bytes + size_t(bx) * texel_bytes, block, texel_bytes);
                break;
              case Convert::kSwapRB565: case Convert::kSwapRB1555: case Convert::kSwapRB4444: {
                uint16_t v;
                std::memcpy(&v, block, 2);
                if (hf.convert == Convert::kSwapRB565) v = uint16_t((v & 0x07E0) | ((v & 0x1F) << 11) | (v >> 11));
                else if (hf.convert == Convert::kSwapRB1555)
                  v = uint16_t((v & 0x83E0) | ((v & 0x1F) << 10) | ((v >> 10) & 0x1F));
                else v = uint16_t((v & 0xF0F0) | ((v & 0xF) << 8) | ((v >> 8) & 0xF));
                std::memcpy(slice_base + size_t(by) * row_bytes + size_t(bx) * 2, &v, 2);
                break;
              }
              case Convert::kDepthUnorm: case Convert::kDepthFloat: {
                uint32_t v;
                std::memcpy(&v, block, 4);
                const float f = hf.convert == Convert::kDepthUnorm ? float(v >> 8) / 16777215.0f
                                                                   : ::xenos::formats::Float20e4To32(v >> 8);
                std::memcpy(slice_base + size_t(by) * row_bytes + size_t(bx) * 4, &f, 4);
                break;
              }
              case Convert::kR11G11B10: case Convert::kR10G11B11: {
                uint32_t v;
                std::memcpy(&v, block, 4);
                uint16_t o[4];
                if (hf.convert == Convert::kR11G11B10) {
                  o[0] = Expand(v & 0x7FF, 11); o[1] = Expand((v >> 11) & 0x7FF, 11); o[2] = Expand(v >> 22, 10);
                } else {
                  o[0] = Expand(v & 0x3FF, 10); o[1] = Expand((v >> 10) & 0x7FF, 11); o[2] = Expand(v >> 21, 11);
                }
                o[3] = 0xFFFF;
                std::memcpy(slice_base + size_t(by) * row_bytes + size_t(bx) * 8, o, 8);
                break;
              }
              case Convert::kDxt3aToR8: case Convert::kCtx1ToRG8: {
                uint8_t texels[32];
                if (hf.convert == Convert::kDxt3aToR8) DecodeDxt3a(block, texels);
                else DecodeCtx1(block, texels);
                for (uint32_t ty = 0; ty < 4 && by * 4 + ty < lh; ++ty)
                  for (uint32_t tx2 = 0; tx2 < 4 && bx * 4 + tx2 < lw; ++tx2)
                    std::memcpy(slice_base + size_t(by * 4 + ty) * row_bytes + size_t(bx * 4 + tx2) * texel_bytes,
                                texels + (ty * 4 + tx2) * texel_bytes, texel_bytes);
                break;
              }
              case Convert::kNone: break;
            }
          }
        }
      }
      {
        const auto up_t0 = std::chrono::steady_clock::now();
        backend_.UploadTexture(e.texture, level, host_3d ? 0 : u, scratch_.data(), row_bytes, host_h, slices);
        cpu_upload += PhaseMs(up_t0);  // job 299
      }
      stats_.upload_bytes += scratch_.size();
    }
  }
  g_phase.upload += cpu_upload;  // job 299
  g_phase.cpu += PhaseMs(cpu_t0) - cpu_upload;
  ++stats_.uploads;
}

}  // namespace xenos::renderer
