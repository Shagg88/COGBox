// Shared by tools/golden/gn_renderer_core_golden.cpp (which runs the reference code) and
// tests/layer0_renderer_core_test.cpp (which runs ours): the exact input sweeps, the case generators and the hash, so
// both sides walk identical inputs. Game-free; nothing here comes from game data.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gn_golden {

// FNV-1a 64 over 32-bit words.
struct Hash {
  uint64_t h = 0xCBF29CE484222325ull;
  void Add(uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      h ^= (v >> (i * 8)) & 0xFF;
      h *= 0x100000001B3ull;
    }
  }
  void AddF(float f) { Add(std::bit_cast<uint32_t>(f)); }
  void AddI(int32_t v) { Add(uint32_t(v)); }
};

struct SplitMix {
  uint64_t s;
  explicit SplitMix(uint64_t seed) : s(seed) {}
  uint64_t Next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint32_t Below(uint32_t n) { return uint32_t(Next() % n); }
};

// ---- tiled addressing sweeps: f(x, y, pitch, bpp_log2) -> int32 ----
template <class F>
uint64_t SweepTiled2D(F f) {
  Hash h;
  for (uint32_t bpp = 0; bpp <= 4; ++bpp)
    for (uint32_t pitch = 1; pitch <= 1300; pitch += (pitch < 96 ? 1 : 37))
      for (int32_t y = 0; y < 200; y += 3)
        for (int32_t x = 0; x < 300; x += 5) h.AddI(f(x, y, pitch, bpp));
  return h.h;
}
// f(x, y, z, pitch, height, bpp_log2)
template <class F>
uint64_t SweepTiled3D(F f) {
  Hash h;
  for (uint32_t bpp = 0; bpp <= 4; ++bpp)
    for (uint32_t pitch : {1u, 32u, 33u, 64u, 96u, 128u, 160u, 256u})
      for (uint32_t height : {1u, 32u, 40u, 64u, 128u})
        for (int32_t z = 0; z < 20; ++z)
          for (int32_t y = 0; y < 70; y += 3)
            for (int32_t x = 0; x < 140; x += 7) h.AddI(f(x, y, z, pitch, height, bpp));
  return h.h;
}
// Upper/lower bounds: f(right, bottom, back, pitch, height, bpp_log2, kind) where kind 0 = lower2D, 1 = upper2D,
// 2 = lower3D, 3 = upper3D. Lower bounds take (left, top, front).
template <class F>
uint64_t SweepTiledBounds(F f) {
  Hash h;
  for (uint32_t kind = 0; kind < 4; ++kind)
    for (uint32_t bpp = 0; bpp <= 4; ++bpp)
      for (uint32_t pitch : {32u, 64u, 96u, 160u, 352u, 1280u})
        for (uint32_t height : {32u, 64u, 736u})
          for (uint32_t r = 0; r < 400; r += 13)
            for (uint32_t b = 0; b < 300; b += 11)
              for (uint32_t z : {0u, 1u, 4u, 5u, 9u}) h.Add(f(r, b, z, pitch, height, bpp, kind));
  return h.h;
}

// ---- packed mip offsets: f(w, h, d, format, mip, &x, &y, &z) -> bool ----
inline const std::vector<uint32_t>& PackedSizes() {
  static const std::vector<uint32_t> v = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 48, 63, 64, 65,
                                          80, 100, 127, 128, 129, 180, 256, 260, 320, 512, 720, 800, 1024, 1280, 2048};
  return v;
}
template <class F>
uint64_t SweepPackedMip(F f) {
  Hash h;
  for (uint32_t format : {2u, 4u, 6u, 18u, 19u, 20u, 26u, 38u, 39u, 40u, 49u, 11u})
    for (uint32_t w : PackedSizes())
      for (uint32_t hh : PackedSizes())
        for (uint32_t d : {1u, 2u, 4u, 8u, 16u, 32u})
          for (uint32_t mip = 0; mip <= 12; ++mip) {
            uint32_t x = 0xDEAD, y = 0xDEAD, z = 0xDEAD;
            bool packed = f(w, hh, d, format, mip, x, y, z);
            h.Add(packed);
            h.Add(x);
            h.Add(y);
            h.Add(z);
          }
  return h.h;
}

// ---- texture layouts ----
struct LayoutCase {
  uint32_t dimension, pitch_div_32, width, height, depth;
  bool tiled;
  uint32_t format;
  bool packed, has_base;
  uint32_t max_level;
};
inline std::vector<LayoutCase> LayoutCases() {
  std::vector<LayoutCase> v = {
      // Gears-relevant shapes (notes/35): 1280x720 scene copies, 800x800 shadow map, 320x180 bloom, 1024 DXT, cube, 3D.
      {1, 40, 1280, 720, 1, true, 6, true, true, 0},
      {1, 40, 1280, 720, 1, true, 31, true, true, 0},
      {1, 40, 1280, 720, 1, true, 6, true, true, 10},
      {1, 25, 800, 800, 1, true, 6, true, true, 0},
      {1, 25, 800, 800, 1, true, 36, false, true, 0},
      {1, 10, 320, 180, 1, true, 6, true, true, 0},
      {1, 11, 352, 182, 1, true, 6, true, true, 0},
      {1, 32, 1024, 1024, 1, true, 18, true, true, 10},
      {1, 32, 1024, 1024, 1, true, 20, true, true, 10},
      {1, 16, 512, 256, 1, true, 49, true, true, 9},
      {1, 1, 8, 8, 1, false, 18, true, true, 3},
      {1, 1, 2, 2, 1, false, 6, true, true, 1},
      {1, 3, 80, 260, 1, true, 6, true, true, 8},
      {1, 44, 1280, 720, 1, false, 6, false, true, 0},  // pitch 1408 for a 1280 image (584109FF note)
      {3, 4, 128, 128, 1, true, 6, true, true, 7},
      {3, 1, 16, 16, 1, true, 32, true, true, 4},
      {2, 1, 32, 32, 32, true, 6, true, true, 5},
      {2, 2, 64, 32, 16, true, 18, true, true, 6},
      {1, 8, 256, 256, 6, true, 6, true, true, 8},  // stacked array
      {0, 8, 256, 1, 1, false, 6, false, true, 0},  // 1D
      {1, 32, 1024, 1024, 1, true, 6, true, false, 10},  // mips only
      {1, 0, 0, 16, 1, true, 6, true, true, 0},          // empty
  };
  SplitMix rng(0x5335);
  const uint32_t formats[] = {2, 3, 4, 6, 7, 10, 15, 18, 19, 20, 22, 24, 25, 26, 30, 31, 32, 36, 37, 38, 39, 40,
                              49, 50, 57, 58, 59, 60, 11};
  for (int i = 0; i < 600; ++i) {
    LayoutCase c{};
    c.dimension = rng.Below(4);
    c.format = formats[rng.Below(sizeof(formats) / sizeof(formats[0]))];
    c.tiled = c.dimension != 0 && rng.Below(4) != 0;
    c.packed = c.dimension != 0 && rng.Below(3) != 0;
    c.has_base = rng.Below(6) != 0;
    const uint32_t maxwh = c.dimension == 2 ? 300 : 2100;
    c.width = 1 + rng.Below(maxwh);
    c.height = c.dimension == 0 ? 1 : 1 + rng.Below(maxwh);
    if (rng.Below(3) == 0) c.width = 1u << rng.Below(c.dimension == 2 ? 9 : 12);
    if (c.dimension != 0 && rng.Below(3) == 0) c.height = 1u << rng.Below(c.dimension == 2 ? 9 : 12);
    c.depth = c.dimension == 2 ? 1 + rng.Below(40) : c.dimension == 1 ? 1 + rng.Below(8) : 1;
    c.pitch_div_32 = (c.width + 31) / 32 + (rng.Below(5) == 0 ? rng.Below(4) : 0);
    uint32_t big = c.width > c.height ? c.width : c.height;
    if (c.dimension == 2 && c.depth > big) big = c.depth;
    uint32_t ml = 31u - uint32_t(std::countl_zero(big));
    c.max_level = rng.Below(ml + 1);
    v.push_back(c);
  }
  return v;
}
// Serialise only the fields the reference defines (it leaves some mips[]/mip_offsets_bytes[] entries unset).
// L must have the GuestLayout / TextureGuestLayout shape.
template <class L>
uint64_t HashLayout(const L& l, bool has_base) {
  Hash h;
  auto level = [&](const auto& lv) {
    h.Add(lv.row_pitch_bytes);
    h.Add(lv.z_slice_stride_block_rows);
    h.Add(lv.array_slice_stride_bytes);
    h.Add(lv.x_extent_blocks);
    h.Add(lv.y_extent_blocks);
    h.Add(lv.z_extent);
    h.Add(lv.array_slice_data_extent_bytes);
    h.Add(lv.level_data_extent_bytes);
  };
  h.Add(l.max_level);
  h.Add(l.packed_level);
  h.Add(l.array_size);
  h.Add(l.mips_total_extent_bytes);
  if (l.array_size == 0) return h.h;  // empty: everything zero
  (void)has_base;
  level(l.base);  // defined: written, or zeroed when !has_base
  const uint32_t max_stored = l.max_level < l.packed_level ? l.max_level : l.packed_level;
  const bool mip0_defined = l.packed_level != 0 || l.max_level != 0;
  if (mip0_defined) {
    level(l.mips[0]);
    if (l.packed_level == 0) h.Add(l.mip_offsets_bytes[0]);
  }
  for (uint32_t i = 1; i <= max_stored && i < 14; ++i) {
    level(l.mips[i]);
    h.Add(l.mip_offsets_bytes[i]);
  }
  return h.h;
}

// ---- EDRAM addressing (Canary layout): f(px, py, sample, base, pitch_tiles, msaa, depth, ints_log2, scale) ----
template <class F>
uint64_t SweepEdram(F f) {
  Hash h;
  for (uint32_t msaa = 0; msaa <= 2; ++msaa)
    for (uint32_t depth = 0; depth <= 1; ++depth)
      for (uint32_t ints = 0; ints <= 1; ++ints)
        for (uint32_t scale : {1u, 2u, 3u})
          for (uint32_t pitch_tiles : {1u, 8u, 16u, 32u})
            for (uint32_t base : {0u, 720u, 2040u})
              for (uint32_t py = 0; py < 70; py += 3)
                for (uint32_t px = 0; px < 260; px += 7)
                  for (uint32_t s = 0; s < (1u << msaa); ++s)
                    h.Add(f(px, py, s, base, pitch_tiles << ints, msaa, depth != 0, ints, scale));
  return h.h;
}

// ---- float sweeps: every 4099th bit pattern plus specials ----
template <class F>
uint64_t SweepFloatBits(F f) {
  Hash h;
  for (uint64_t b = 0; b <= 0xFFFFFFFFull; b += 4099) h.Add(f(std::bit_cast<float>(uint32_t(b))));
  for (uint32_t b : {0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x3F800000u, 0x3F7FFFFFu,
                     0x00000001u})
    h.Add(f(std::bit_cast<float>(b)));
  return h.h;
}

// ---- resolves ----
struct ResolveCase {
  float v[6];
  bool pix_center_zero, window_offset_enable;
  int32_t wx, wy;
  int32_t sx, sy, sw, sh;
  uint32_t surface_pitch, msaa, src_select, sample_select, copy_command;
  bool depth_clear_enable;
  uint32_t color_base, color_format, depth_base, depth_format;
  uint32_t dest_base, dest_pitch, dest_height;
  bool dest_array;
  uint32_t dest_slice, dest_format, surface_slice;
};
inline std::vector<ResolveCase> ResolveCases() {
  std::vector<ResolveCase> v;
  // Hand cases (shapes from notes/35; values are synthetic): full 1280x720 scene, 4x MSAA depth at base 720,
  // 800x800 shadow map, 320x180 bloom into a 352x182 texture at 0x14550000.
  auto rect = [](float x0, float y0, float x1, float y1, ResolveCase c) {
    c.v[0] = x0; c.v[1] = y0; c.v[2] = x1; c.v[3] = y0; c.v[4] = x1; c.v[5] = y1;
    return c;
  };
  ResolveCase base{};
  base.pix_center_zero = true;
  base.sw = 8192; base.sh = 8192;
  base.copy_command = 1;
  {
    ResolveCase c = base; c.surface_pitch = 1280; c.msaa = 0; c.color_format = 0; c.dest_format = 6;
    c.dest_base = 0x1A000000; c.dest_pitch = 1280; c.dest_height = 720;
    v.push_back(rect(-0.5f, -0.5f, 1279.5f, 719.5f, c));
  }
  {
    ResolveCase c = base; c.surface_pitch = 1280; c.msaa = 2; c.src_select = 4; c.depth_base = 720;
    c.depth_format = 1; c.dest_format = 6; c.dest_base = 0x1B000000; c.dest_pitch = 1280; c.dest_height = 720;
    v.push_back(rect(-0.5f, -0.5f, 1279.5f, 719.5f, c));
  }
  {
    ResolveCase c = base; c.surface_pitch = 800; c.src_select = 4; c.depth_format = 0; c.dest_format = 6;
    c.dest_base = 0x1C000000; c.dest_pitch = 800; c.dest_height = 800;
    v.push_back(rect(-0.5f, -0.5f, 799.5f, 799.5f, c));
  }
  {
    ResolveCase c = base; c.surface_pitch = 320; c.color_format = 6; c.dest_format = 31; c.dest_base = 0x14550000;
    c.dest_pitch = 352; c.dest_height = 182;
    v.push_back(rect(-0.5f, -0.5f, 319.5f, 179.5f, c));
  }
  {
    ResolveCase c = base; c.surface_pitch = 640; c.color_format = 7; c.color_base = 100; c.dest_format = 32;
    c.dest_base = 0x10001000; c.dest_pitch = 640; c.dest_height = 360;
    v.push_back(rect(23.5f, 7.5f, 400.5f, 300.5f, c));
  }
  {
    ResolveCase c = base; c.surface_pitch = 512; c.color_format = 0; c.dest_format = 4;  // 16bpp, macro phase
    c.dest_base = 0x10000800; c.dest_pitch = 512; c.dest_height = 512;
    v.push_back(rect(-0.5f, -0.5f, 255.5f, 255.5f, c));
  }
  {
    ResolveCase c = base; c.surface_pitch = 256; c.color_format = 0; c.dest_format = 61; c.copy_command = 2;
    c.dest_base = 0x10004000; c.dest_pitch = 64; c.dest_height = 64;
    v.push_back(rect(-0.5f, -0.5f, 255.5f, 255.5f, c));
  }
  SplitMix rng(0xE5D7A3);
  const uint32_t color_formats[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 12, 14, 15};
  const uint32_t dest_formats[] = {2, 3, 4, 6, 7, 10, 14, 15, 16, 17, 18, 22, 24, 25, 26, 30, 31, 32, 36, 37, 38, 61};
  for (int i = 0; i < 4000; ++i) {
    ResolveCase c{};
    float x0 = float(int32_t(rng.Below(1400)) - 40) + (rng.Below(4) ? -0.5f : float(rng.Below(256)) / 256.0f);
    float y0 = float(int32_t(rng.Below(800)) - 40) + (rng.Below(4) ? -0.5f : float(rng.Below(256)) / 256.0f);
    float x1 = x0 + float(rng.Below(1300));
    float y1 = y0 + float(rng.Below(760));
    if (rng.Below(2)) { float t = x0; x0 = x1; x1 = t; }
    c.v[0] = x0; c.v[1] = y0; c.v[2] = x1; c.v[3] = y0; c.v[4] = rng.Below(2) ? x1 : x0; c.v[5] = y1;
    c.pix_center_zero = rng.Below(4) != 0;
    c.window_offset_enable = rng.Below(5) == 0;
    c.wx = int32_t(rng.Below(160)) - 80;
    c.wy = int32_t(rng.Below(160)) - 80;
    c.sx = rng.Below(4) ? 0 : int32_t(rng.Below(200));
    c.sy = rng.Below(4) ? 0 : int32_t(rng.Below(200));
    c.sw = rng.Below(4) ? 8192 : int32_t(rng.Below(1400));
    c.sh = rng.Below(4) ? 8192 : int32_t(rng.Below(800));
    c.surface_pitch = rng.Below(3) ? 1280 : rng.Below(1600);
    c.msaa = rng.Below(16) == 0 ? 3 : rng.Below(3);
    c.src_select = rng.Below(5);
    c.sample_select = rng.Below(8);
    c.copy_command = rng.Below(10) == 0 ? rng.Below(4) : 1;
    c.depth_clear_enable = rng.Below(4) == 0;
    c.color_base = rng.Below(2048);
    c.color_format = color_formats[rng.Below(12)];
    c.depth_base = rng.Below(2048);
    c.depth_format = rng.Below(2);
    c.dest_base = 0x10000000u + (rng.Below(0x10000) << 12) + (rng.Below(4) == 0 ? (rng.Below(4) << 10) : 0);
    c.dest_pitch = rng.Below(3) ? (rng.Below(45) + 1) * 32 : rng.Below(1500);
    c.dest_height = rng.Below(1000);
    c.dest_array = rng.Below(8) == 0;
    c.dest_slice = rng.Below(8);
    c.dest_format = dest_formats[rng.Below(sizeof(dest_formats) / sizeof(dest_formats[0]))];
    c.surface_slice = rng.Below(3) ? c.dest_pitch * c.dest_height : rng.Below(1u << 20);
    v.push_back(c);
  }
  return v;
}

// ---- rectangle-list strip order ----
inline std::vector<float> RectCases() {
  std::vector<float> v;  // 6 floats per case
  // The six vertex orders of an axis-aligned rectangle, a square (all ties) and degenerate inputs.
  const float r[4][2] = {{0, 0}, {100, 0}, {0, 50}, {100, 50}};
  const int orders[][3] = {{0, 1, 2}, {1, 3, 0}, {3, 2, 1}, {2, 0, 3}, {1, 0, 3}, {0, 2, 1}};
  for (auto& o : orders)
    for (int k = 0; k < 3; ++k) {
      v.push_back(r[o[k]][0]);
      v.push_back(r[o[k]][1]);
    }
  for (float f : {0.f, 0.f, 10.f, 0.f, 0.f, 10.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 5.f, 5.f, 5.f, 5.f, 6.f, 6.f})
    v.push_back(f);
  SplitMix rng(0x7EC7);
  for (int i = 0; i < 3000; ++i)
    for (int k = 0; k < 6; ++k) v.push_back((float(rng.Below(20001)) - 10000.0f) / float(1 + rng.Below(64)));
  return v;
}

}  // namespace gn_golden
