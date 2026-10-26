// Golden-table generator for tests/layer0_renderer_core_test.cpp (S3.5 groundwork). Runs the REFERENCE code over the
// shared sweeps in tests/golden/layer0_renderer_core_sweeps.h and writes tests/golden/layer0_renderer_core_golden.inc:
//  - Xenia Canary e6cd2e4's own texture_util / texture_address / FormatInfo / PWL gamma, compiled from the Canary
//    sources (gn_renderer_core_golden_canary.cpp);
//  - the rexglue SDK 0.10.0's own FloatToD3D11Fixed16p8 and GetD3D10IntegerPolygonOffset (text-identical to Canary's),
//    compiled from the SDK sources;
//  - verbatim-logic copies of the Canary pieces that need a RegisterFile or are shader code (GetResolveInfo,
//    SanitizeCopySampleSelect, XeEdramOffsetBytes, the rectangle-list geometry shader), using Canary's texture_util.
// It also cross-checks our headers exhaustively where a unit test would be too slow (gamma and 16.8 fixed point over
// all 2^32 float bit patterns, against both Canary and rexglue), and counts where rexglue's older texture_util and
// resolve destination math differ from Canary's. Prints the counts.
//
// The Canary copies keep Canary's structure and comments trimmed; Canary/Xenia is BSD-3-Clause, Copyright (c) Ben
// Vanik and Xenia project contributors. This tool is not part of the game build. Game-free: no game data is read.
//
// Build (in the container, see the S3.5 groundwork report):
//   clang++ -std=c++23 -O2 -DNDEBUG -ffp-contract=off -I<sdk>/include -I<fmt>/include -I<spdlog>/include -I<simde>
//     -I<xxHash> -DFMT_HEADER_ONLY -I src/layer0_xenos/include -I tests
//     tools/golden/gn_renderer_core_golden.cpp <sdk>/src/graphics/pipeline/texture/{util,info,info_formats,extent}.cpp
//     <sdk>/src/core/math_gcc.cpp <sdk>/src/graphics/xenos.cpp <sdk>/src/ui/graphics_util.cpp
//     <objects of gn_renderer_core_golden_canary.cpp and Canary texture_util.cc, texture_info.cc,
//      texture_info_formats.cc, texture_extent.cc, xenos.cc built with -I <canary>/src> -lpthread
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/graphics_util.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "golden/layer0_renderer_core_sweeps.h"
#include "xenos/gamma.h"
#include "xenos/resolve.h"

namespace rg = rex::graphics;
namespace rx = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;
using gn_golden::Hash;

namespace gn_canary {  // gn_renderer_core_golden_canary.cpp
int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bpp);
int64_t Tiled3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height, uint32_t bpp);
uint32_t Bound(uint32_t r, uint32_t b, uint32_t z, uint32_t pitch, uint32_t height, uint32_t bpp, uint32_t kind);
bool PackedMipOffset(uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t mip, uint32_t& x, uint32_t& y,
                     uint32_t& z);
uint64_t Layout(const gn_golden::LayoutCase& c, uint32_t& base_extent, uint32_t& mips_total, uint32_t& packed_level,
                uint32_t& max_level);
void Format(uint32_t i, uint32_t& bw, uint32_t& bh, uint32_t& bpp, uint32_t& type);
float PWLGammaToLinear(float v);
float LinearToPWLGamma(float v);
}  // namespace gn_canary

// ---------------------------------------------------------------------------------------------------------------------
// Canary e6cd2e4 copies
// ---------------------------------------------------------------------------------------------------------------------

// shaders/edram.xesli XeEdramOffsetBytes, as C++ (uint2 lanes written out).
static uint32_t CanaryEdramOffsetBytes(uint32_t pix_x, uint32_t pix_y, uint32_t base_tiles, bool wrap,
                                       uint32_t pitch_tiles, uint32_t msaa_samples, bool is_depth,
                                       uint32_t format_ints_log2, uint32_t pixel_sample_index, uint32_t scale_x,
                                       uint32_t scale_y) {
  uint32_t pixel[2] = {pix_x / scale_x, pix_y / scale_y};
  uint32_t subpixel[2] = {pix_x - pixel[0] * scale_x, pix_y - pixel[1] * scale_y};
  uint32_t guest_sample[2];
  if (msaa_samples >= 2) {
    uint32_t sample_offset[2] = {(pixel_sample_index >> 0) & 1u, (pixel_sample_index >> 1) & 1u};
    guest_sample[0] = ((pixel[0] & ~1u) << 1u) | (pixel[0] & 1u) | (sample_offset[0] << 1u);
    guest_sample[1] = ((pixel[1] & ~1u) << 1u) | (pixel[1] & 1u) | (sample_offset[1] << 1u);
  } else if (msaa_samples == 1) {
    guest_sample[0] = (pixel[0] & ~2u) | ((pixel_sample_index & 1u) << 1u);
    guest_sample[1] = ((pixel[1] & ~1u) << 1u) | (pixel[1] & 1u) | (pixel[0] & 2u);
  } else {
    guest_sample[0] = pixel[0];
    guest_sample[1] = pixel[1];
  }
  uint32_t rt[2] = {guest_sample[0] * scale_x + subpixel[0], guest_sample[1] * scale_y + subpixel[1]};
  uint32_t t32[2] = {80u * scale_x, 16u * scale_y};
  uint32_t ts[2] = {t32[0] >> format_ints_log2, t32[1] >> 0u};
  uint32_t to[2] = {rt[0] / ts[0], rt[1] / ts[1]};
  base_tiles += to[1] * pitch_tiles + to[0];
  rt[0] -= to[0] * ts[0];
  rt[1] -= to[1] * ts[1];
  if (is_depth) {
    uint32_t tile_width_half = ts[0] >> 1u;
    rt[0] = uint32_t(int(rt[0]) + ((rt[0] >= tile_width_half) ? -int(tile_width_half) : int(tile_width_half)));
  }
  uint32_t address = base_tiles * (t32[0] * t32[1]) + ((rt[1] * ts[0] + rt[0]) << format_ints_log2);
  if (wrap) address %= t32[0] * t32[1] * 2048u;
  return address << 2;
}

// draw_util.cc SanitizeCopySampleSelect.
static rx::CopySampleSelect CanarySanitizeCopySampleSelect(rx::CopySampleSelect copy_sample_select,
                                                              rx::MsaaSamples msaa_samples, bool is_depth) {
  if (msaa_samples >= rx::MsaaSamples::k4X) {
    if (copy_sample_select > rx::CopySampleSelect::k0123) copy_sample_select = rx::CopySampleSelect::k0123;
    if (is_depth) {
      switch (copy_sample_select) {
        case rx::CopySampleSelect::k01:
        case rx::CopySampleSelect::k0123:
          copy_sample_select = rx::CopySampleSelect::k0;
          break;
        case rx::CopySampleSelect::k23:
          copy_sample_select = rx::CopySampleSelect::k2;
          break;
        default:
          break;
      }
    }
  } else if (msaa_samples >= rx::MsaaSamples::k2X) {
    switch (copy_sample_select) {
      case rx::CopySampleSelect::k2:
        copy_sample_select = rx::CopySampleSelect::k0;
        break;
      case rx::CopySampleSelect::k3:
        copy_sample_select = rx::CopySampleSelect::k1;
        break;
      default:
        if (copy_sample_select > rx::CopySampleSelect::k01) copy_sample_select = rx::CopySampleSelect::k01;
    }
    if (is_depth && copy_sample_select == rx::CopySampleSelect::k01) copy_sample_select = rx::CopySampleSelect::k0;
  } else {
    copy_sample_select = rx::CopySampleSelect::k0;
  }
  return copy_sample_select;
}

struct RefResolveOut {
  uint32_t valid, x0, y0, x1, y1, width_div_8, height_div_8, sample_select, edram_off_x, edram_off_y;
  uint32_t color_used, color_base, color_pitch, color_64, depth_used, depth_base, depth_pitch;
  uint32_t dest_format, dest_pitch_div_32, dest_height_div_32, dest_off_x, dest_off_y, dest_base, ext_start, ext_len;
};

// draw_util.cc GetResolveInfo, the parts that compute coordinates (register reads replaced by the case fields;
// GetScissor replaced by the case's scissor; logging removed).
static RefResolveOut CanaryGetResolveInfo(const gn_golden::ResolveCase& c) {
  RefResolveOut o{};
  bool is_convert_to_1111 = c.copy_command == 2;
  if (c.copy_command != 0 && c.copy_command != 1 && !is_convert_to_1111) return o;
  float half_pixel_offset = c.pix_center_zero ? 0.5f : 0.0f;
  int32_t vertices_fixed[6];
  for (size_t i = 0; i < 6; ++i) vertices_fixed[i] = rex::ui::FloatToD3D11Fixed16p8(c.v[i] + half_pixel_offset);
  int32_t x0 = std::min(std::min(vertices_fixed[0], vertices_fixed[2]), vertices_fixed[4]);
  int32_t y0 = std::min(std::min(vertices_fixed[1], vertices_fixed[3]), vertices_fixed[5]);
  int32_t x1 = std::max(std::max(vertices_fixed[0], vertices_fixed[2]), vertices_fixed[4]);
  int32_t y1 = std::max(std::max(vertices_fixed[1], vertices_fixed[3]), vertices_fixed[5]);
  x0 = (x0 + 127) >> 8;
  y0 = (y0 + 127) >> 8;
  x1 = (x1 + 127) >> 8;
  y1 = (y1 + 127) >> 8;
  if (c.window_offset_enable) {
    x0 += c.wx;
    y0 += c.wy;
    x1 += c.wx;
    y1 += c.wy;
  }
  int32_t scissor_right = c.sx + c.sw;
  int32_t scissor_bottom = c.sy + c.sh;
  x0 = std::clamp(x0, c.sx, scissor_right);
  y0 = std::clamp(y0, c.sy, scissor_bottom);
  x1 = std::clamp(x1, c.sx, scissor_right);
  y1 = std::clamp(y1, c.sy, scissor_bottom);
  x0 &= ~int32_t(rx::kResolveAlignmentPixels - 1);
  y0 &= ~int32_t(rx::kResolveAlignmentPixels - 1);
  x1 = rex::align(x1, int32_t(rx::kResolveAlignmentPixels));
  y1 = rex::align(y1, int32_t(rx::kResolveAlignmentPixels));
  if (c.msaa > 2) return o;
  rx::MsaaSamples msaa = rx::MsaaSamples(c.msaa);
  int32_t surface_pitch_aligned = int32_t(c.surface_pitch & ~uint32_t(rx::kResolveAlignmentPixels - 1));
  if (x1 > surface_pitch_aligned) {
    x0 = std::min(x0, surface_pitch_aligned);
    x1 = std::min(x1, surface_pitch_aligned);
  }
  if (y1 - y0 > int32_t(rx::kMaxResolveSize)) y1 = y0 + int32_t(rx::kMaxResolveSize);
  o.valid = 1;
  o.x0 = uint32_t(x0);
  o.y0 = uint32_t(y0);
  o.x1 = uint32_t(x1);
  o.y1 = uint32_t(y1);
  if (x0 >= x1 || y0 >= y1) return o;
  o.width_div_8 = uint32_t(x1 - x0) >> rx::kResolveAlignmentPixelsLog2;
  o.height_div_8 = uint32_t(y1 - y0) >> rx::kResolveAlignmentPixelsLog2;

  bool is_depth = c.src_select >= rx::kMaxColorRenderTargets;
  rx::CopySampleSelect sample_select =
      CanarySanitizeCopySampleSelect(rx::CopySampleSelect(c.sample_select), msaa, is_depth);
  o.sample_select = uint32_t(sample_select);
  rx::TextureFormat dest_format;
  if (is_depth) {
    dest_format = c.depth_format ? rx::TextureFormat::k_24_8_FLOAT : rx::TextureFormat::k_24_8;
  } else {
    dest_format = rx::TextureFormat(c.dest_format);
  }
  o.dest_format = uint32_t(dest_format);
  bool convert_to_1111_supported = false;
  if (is_convert_to_1111) {
    if (!(is_depth || dest_format != rx::TextureFormat::k_DXT3A_AS_1_1_1_1 || c.dest_array)) {
      auto source_format = rx::ColorRenderTargetFormat(c.color_format);
      if (source_format == rx::ColorRenderTargetFormat::k_8_8_8_8 ||
          source_format == rx::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        if (rx::IsSingleCopySampleSelected(sample_select)) convert_to_1111_supported = true;
      }
    }
  }

  uint32_t rb_copy_dest_base = c.dest_base;
  uint32_t copy_dest_base_adjusted = rb_copy_dest_base;
  uint32_t copy_dest_extent_start, copy_dest_extent_end;
  const uint32_t copy_dest_pitch_aligned = rex::align(c.dest_pitch, uint32_t(32));  // kStoragePitchHeightAlignmentBlocks
  o.dest_pitch_div_32 = copy_dest_pitch_aligned >> 5;
  uint32_t copy_dest_height = c.dest_height;
  if (c.dest_array) {
    uint32_t rb_copy_surface_slice = c.surface_slice;
    if (rb_copy_surface_slice && c.dest_pitch) copy_dest_height = rb_copy_surface_slice / c.dest_pitch;
  }
  const uint32_t copy_dest_height_aligned = rex::align(copy_dest_height, uint32_t(32));
  o.dest_height_div_32 = copy_dest_height_aligned >> 5;
  const rg::FormatInfo& dest_format_info = *rg::FormatInfo::Get(dest_format);
  if (is_convert_to_1111 ? convert_to_1111_supported : (is_depth || dest_format_info.type == rg::FormatType::kResolvable)) {
    uint32_t bpp_log2 = rex::log2_floor(dest_format_info.bytes_per_block());
    uint32_t dest_block_size_log2 = is_convert_to_1111 ? 2 : 0;
    uint32_t dest_base_relative_x_mask =
        (UINT32_C(1) << rx::GetTextureTiledXBaseGranularityLog2(c.dest_array, bpp_log2)) - 1;
    uint32_t dest_base_relative_y_mask =
        (UINT32_C(1) << rx::GetTextureTiledYBaseGranularityLog2(c.dest_array, bpp_log2)) - 1;
    uint32_t dest_addr_base = rb_copy_dest_base;
    uint32_t dest_addr_x0 = uint32_t(x0) >> dest_block_size_log2;
    uint32_t dest_addr_y0 = uint32_t(y0) >> dest_block_size_log2;
    if (!c.dest_array) {
      uint32_t dest_macro_tile_bytes_log2 = 2 * rx::kTextureTileWidthHeightLog2 + bpp_log2;
      uint32_t dest_macro_phase =
          (rb_copy_dest_base & (rx::kTextureSubresourceAlignmentBytes - 1)) >> dest_macro_tile_bytes_log2;
      dest_addr_base -= dest_macro_phase << dest_macro_tile_bytes_log2;
      dest_addr_x0 += dest_macro_phase << rx::kTextureTileWidthHeightLog2;
    }
    uint32_t dest_addr_x1 = dest_addr_x0 + (uint32_t(x1 - x0) >> dest_block_size_log2);
    uint32_t dest_addr_y1 = dest_addr_y0 + (uint32_t(y1 - y0) >> dest_block_size_log2);
    copy_dest_base_adjusted = dest_addr_base;
    o.dest_off_x =
        ((dest_addr_x0 & dest_base_relative_x_mask) << dest_block_size_log2) >> rx::kResolveAlignmentPixelsLog2;
    o.dest_off_y =
        ((dest_addr_y0 & dest_base_relative_y_mask) << dest_block_size_log2) >> rx::kResolveAlignmentPixelsLog2;
    uint32_t dest_base_x = dest_addr_x0 & ~dest_base_relative_x_mask;
    uint32_t dest_base_y = dest_addr_y0 & ~dest_base_relative_y_mask;
    if (c.dest_array) {
      copy_dest_base_adjusted += uint32_t(gn_canary::Tiled3D(int32_t(dest_base_x), int32_t(dest_base_y), 0,
                                                             copy_dest_pitch_aligned, copy_dest_height_aligned,
                                                             bpp_log2));
      copy_dest_extent_start =
          dest_addr_base + gn_canary::Bound(dest_addr_x0, dest_addr_y0, c.dest_slice, copy_dest_pitch_aligned,
                                            copy_dest_height_aligned, bpp_log2, 2);
      copy_dest_extent_end =
          dest_addr_base + gn_canary::Bound(dest_addr_x1, dest_addr_y1, c.dest_slice + 1, copy_dest_pitch_aligned,
                                            copy_dest_height_aligned, bpp_log2, 3);
    } else {
      copy_dest_base_adjusted +=
          gn_canary::Tiled2D(int32_t(dest_base_x), int32_t(dest_base_y), copy_dest_pitch_aligned, bpp_log2);
      copy_dest_extent_start =
          dest_addr_base + gn_canary::Bound(dest_addr_x0, dest_addr_y0, 0, copy_dest_pitch_aligned, 0, bpp_log2, 0);
      copy_dest_extent_end =
          dest_addr_base + gn_canary::Bound(dest_addr_x1, dest_addr_y1, 0, copy_dest_pitch_aligned, 0, bpp_log2, 1);
    }
  } else {
    copy_dest_extent_start = copy_dest_base_adjusted;
    copy_dest_extent_end = copy_dest_base_adjusted;
  }
  o.dest_base = copy_dest_base_adjusted;
  o.ext_start = copy_dest_extent_start;
  o.ext_len = copy_dest_extent_end - copy_dest_extent_start;

  uint32_t sample_count_log2_x = uint32_t(msaa >= rx::MsaaSamples::k4X);
  uint32_t sample_count_log2_y = uint32_t(msaa >= rx::MsaaSamples::k2X);
  uint32_t x0_samples = uint32_t(x0) << sample_count_log2_x;
  uint32_t y0_samples = uint32_t(y0) << sample_count_log2_y;
  uint32_t base_offset_x_tiles = x0_samples / rx::kEdramTileWidthSamples;
  uint32_t base_offset_y_tiles = y0_samples / rx::kEdramTileHeightSamples;
  o.edram_off_x = (x0_samples % rx::kEdramTileWidthSamples) >> (sample_count_log2_x + 3);
  o.edram_off_y = (y0_samples % rx::kEdramTileHeightSamples) >> (sample_count_log2_y + 3);
  uint32_t surface_pitch_tiles = rx::GetSurfacePitchTiles(c.surface_pitch, msaa, false);
  uint32_t edram_base_offset_tiles = base_offset_y_tiles * surface_pitch_tiles + base_offset_x_tiles;
  if (is_depth || c.depth_clear_enable) {
    o.depth_used = 1;
    o.depth_pitch = surface_pitch_tiles;
    o.depth_base = c.depth_base + edram_base_offset_tiles;
  }
  if (!is_depth) {
    uint32_t is_64bpp = uint32_t(rx::IsColorRenderTargetFormat64bpp(rx::ColorRenderTargetFormat(c.color_format)));
    o.color_used = 1;
    o.color_64 = is_64bpp;
    o.color_pitch = surface_pitch_tiles << is_64bpp;
    o.color_base = c.color_base + (edram_base_offset_tiles << is_64bpp);
  }
  return o;
}

// rexglue SDK draw.cpp destination math (older): only for counting where it agrees with Canary.
static void RexglueDest(const gn_golden::ResolveCase& c, const RefResolveOut& canary, uint32_t& base_out,
                        uint32_t& start_out, uint32_t& len_out) {
  uint32_t dest_format = canary.dest_format;
  const rg::FormatInfo& fi = *rg::FormatInfo::Get(dest_format);
  uint32_t bpp_log2 = rex::log2_floor(fi.bits_per_pixel >> 3);
  uint32_t xm = (1u << rx::GetTextureTiledXBaseGranularityLog2(false, bpp_log2)) - 1;
  uint32_t ym = (1u << rx::GetTextureTiledYBaseGranularityLog2(false, bpp_log2)) - 1;
  uint32_t bx = canary.x0 & ~xm, by = canary.y0 & ~ym;
  base_out = c.dest_base + tu::GetTiledOffset2D(int32_t(bx), int32_t(by), c.dest_pitch, bpp_log2);
  start_out = c.dest_base + tu::GetTiledAddressLowerBound2D(canary.x0, canary.y0, c.dest_pitch, bpp_log2);
  uint32_t end = c.dest_base + tu::GetTiledAddressUpperBound2D(canary.x1, canary.y1, c.dest_pitch, bpp_log2);
  len_out = end - start_out;
}

// d3d12/pipeline_cache.cc rectangle-list geometry shader, the vertex selection (DXBC ops written out in float32).
static void CanaryRectOrder(const float* p, uint32_t order[3]) {
  // p = v0.xy, v1.xy, v2.xy
  float r0[4];
  float t[2];
  t[0] = p[4] - p[2];  // r0.xy = v2.xy - v1.xy
  t[1] = p[5] - p[3];
  {
    float a = t[0] * t[0], b = t[1] * t[1];
    r0[0] = a + b;  // dp2
  }
  t[0] = p[0] - p[4];  // v0 - v2
  t[1] = p[1] - p[5];
  {
    float a = t[0] * t[0], b = t[1] * t[1];
    r0[1] = a + b;
  }
  t[0] = p[2] - p[0];  // v1 - v0
  t[1] = p[3] - p[1];
  {
    float a = t[0] * t[0], b = t[1] * t[1];
    r0[2] = a + b;
  }
  bool w = r0[1] < r0[0];  // r0.w = 12 > 20
  bool x = r0[2] < r0[0];  // r0.x = 12 > 01
  if (w && x) {
    order[0] = 0, order[1] = 1, order[2] = 2;
  } else {
    bool x2 = r0[2] < r0[1];  // 20 > 01
    if (x2) order[0] = 1, order[1] = 2, order[2] = 0;
    else order[0] = 2, order[1] = 0, order[2] = 1;
  }
}

// ---------------------------------------------------------------------------------------------------------------------

template <class F>
static uint64_t ParallelCountAll32(F mismatch) {
  const unsigned n = std::max(1u, std::thread::hardware_concurrency());
  std::atomic<uint64_t> total{0};
  std::vector<std::thread> th;
  for (unsigned t = 0; t < n; ++t)
    th.emplace_back([&, t] {
      uint64_t bad = 0;
      for (uint64_t b = t; b <= 0xFFFFFFFFull; b += n) bad += mismatch(uint32_t(b)) ? 1 : 0;
      total += bad;
    });
  for (auto& x : th) x.join();
  return total.load();
}

int main(int argc, char** argv) {
  const char* out_path = argc > 1 ? argv[1] : "layer0_renderer_core_golden.inc";
  FILE* f = std::fopen(out_path, "w");
  if (!f) return 1;
  std::fprintf(f,
               "// GENERATED by tools/golden/gn_renderer_core_golden.cpp from the reference code (rexglue SDK 0.10.0\n"
               "// bd833a2a texture_util/FormatInfo/gamma/FloatToD3D11Fixed16p8/GetD3D10IntegerPolygonOffset, and Canary\n"
               "// e6cd2e4 logic copies). Do not edit. Game-free.\n#pragma once\n#include <cstdint>\n\n"
               "namespace gn_golden_data {\n\n");

  // Format table.
  std::fprintf(f, "struct FormatRow { uint8_t bw, bh, bpp, type; };\nconstexpr FormatRow kFormats[64] = {\n");
  uint32_t format_table_diffs = 0;
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t bw, bh, bpp, type;
    gn_canary::Format(i, bw, bh, bpp, type);
    const rg::FormatInfo* fi = rg::FormatInfo::Get(i);
    format_table_diffs += bw != fi->block_width || bh != fi->block_height || bpp != fi->bits_per_pixel ||
                          type != uint32_t(fi->type);
    std::fprintf(f, "  {%u, %u, %u, %u},\n", bw, bh, bpp, type);
  }
  std::fprintf(f, "};\n\n");

  // Tiled addressing (Canary), counting where rexglue differs.
  uint64_t rx_diff_2d = 0, rx_diff_3d = 0, rx_diff_b[4] = {}, rx_n_b[4] = {};
  uint64_t h2d = gn_golden::SweepTiled2D([&](int32_t x, int32_t y, uint32_t pitch, uint32_t bpp) {
    const int32_t v = gn_canary::Tiled2D(x, y, pitch, bpp);
    rx_diff_2d += v != tu::GetTiledOffset2D(x, y, pitch, bpp);
    return v;
  });
  uint64_t h3d = gn_golden::SweepTiled3D([&](int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                                             uint32_t bpp) {
    const int64_t v = gn_canary::Tiled3D(x, y, z, pitch, height, bpp);
    rx_diff_3d += v != int64_t(tu::GetTiledOffset3D(x, y, z, pitch, height, bpp));
    return int32_t(v);
  });
  uint64_t hb = gn_golden::SweepTiledBounds([&](uint32_t r, uint32_t b, uint32_t z, uint32_t pitch, uint32_t height,
                                                uint32_t bpp, uint32_t kind) -> uint32_t {
    const uint32_t v = gn_canary::Bound(r, b, z, pitch, height, bpp, kind);
    uint32_t w;
    switch (kind) {
      case 0: w = tu::GetTiledAddressLowerBound2D(r, b, pitch, bpp); break;
      case 1: w = tu::GetTiledAddressUpperBound2D(r, b, pitch, bpp); break;
      case 2: w = tu::GetTiledAddressLowerBound3D(r, b, z, pitch, height, bpp); break;
      default: w = tu::GetTiledAddressUpperBound3D(r, b, z, pitch, height, bpp); break;
    }
    ++rx_n_b[kind];
    rx_diff_b[kind] += v != w;
    return v;
  });
  std::fprintf(f, "constexpr uint64_t kHashTiled2D = 0x%016" PRIX64 "ull;\n", h2d);
  std::fprintf(f, "constexpr uint64_t kHashTiled3D = 0x%016" PRIX64 "ull;\n", h3d);
  std::fprintf(f, "constexpr uint64_t kHashTiledBounds = 0x%016" PRIX64 "ull;\n", hb);
  // A few explicit 2D/3D offsets for readable failures.
  std::fprintf(f, "struct Tiled2DRow { int32_t x, y; uint32_t pitch, bpp; int32_t off; };\nconstexpr Tiled2DRow kTiled2D[] = {\n");
  for (uint32_t bpp = 0; bpp <= 4; ++bpp)
    for (int32_t y : {0, 1, 7, 8, 16, 31, 32, 33, 100})
      for (int32_t x : {0, 1, 7, 8, 31, 32, 100, 639})
        std::fprintf(f, "  {%d, %d, %u, %u, %d},\n", x, y, 1280u, bpp, gn_canary::Tiled2D(x, y, 1280, bpp));
  std::fprintf(f, "};\n\n");

  // Packed mips.
  uint64_t rx_diff_packed = 0;
  uint64_t hp = gn_golden::SweepPackedMip([&](uint32_t w, uint32_t h, uint32_t d, uint32_t fmt, uint32_t mip,
                                              uint32_t& x, uint32_t& y, uint32_t& z) {
    const bool p = gn_canary::PackedMipOffset(w, h, d, fmt, mip, x, y, z);
    uint32_t a, b, c;
    const bool q = tu::GetPackedMipOffset(w, h, d, rx::TextureFormat(fmt), mip, a, b, c);
    rx_diff_packed += p != q || a != x || b != y || c != z;
    return p;
  });
  std::fprintf(f, "constexpr uint64_t kHashPackedMip = 0x%016" PRIX64 "ull;\n", hp);
  std::fprintf(f, "struct PackedRow { uint32_t w, h, d, fmt, mip, packed, x, y, z; };\nconstexpr PackedRow kPacked[] = {\n");
  for (uint32_t w : {256u, 64u, 16u, 1024u})
    for (uint32_t h : {64u, 256u, 16u, 8u})
      for (uint32_t mip = 0; mip <= 10; ++mip) {
        uint32_t x, y, z;
        bool p = gn_canary::PackedMipOffset(w, h, 1, 6, mip, x, y, z);
        std::fprintf(f, "  {%u, %u, 1, 6, %u, %u, %u, %u, %u},\n", w, h, mip, p, x, y, z);
      }
  std::fprintf(f, "};\n\n");

  // Layouts.
  auto cases = gn_golden::LayoutCases();
  std::fprintf(f, "struct LayoutRow { uint64_t hash; uint32_t base_extent, mips_total, packed_level, max_level; };\n"
                  "constexpr LayoutRow kLayouts[] = {\n");
  uint64_t rx_diff_layout = 0;
  for (const auto& c : cases) {
    uint32_t be, mt, pl, ml;
    const uint64_t hash = gn_canary::Layout(c, be, mt, pl, ml);
    auto l = tu::GetGuestTextureLayout(rx::DataDimension(c.dimension), c.pitch_div_32, c.width, c.height, c.depth,
                                       c.tiled, rx::TextureFormat(c.format), c.packed, c.has_base, c.max_level);
    rx_diff_layout += gn_golden::HashLayout(l, c.has_base) != hash;
    std::fprintf(f, "  {0x%016" PRIX64 "ull, %u, %u, %u, %u},\n", hash, be, mt, pl, ml);
  }
  std::fprintf(f, "};\n\n");

  // EDRAM.
  uint64_t he = gn_golden::SweepEdram([](uint32_t px, uint32_t py, uint32_t s, uint32_t base, uint32_t pitch,
                                         uint32_t msaa, bool depth, uint32_t ints, uint32_t scale) {
    return CanaryEdramOffsetBytes(px, py, base, true, pitch, msaa, depth, ints, s, scale, scale);
  });
  std::fprintf(f, "constexpr uint64_t kHashEdram = 0x%016" PRIX64 "ull;\n", he);
  std::fprintf(f, "struct EdramRow { uint32_t px, py, s, base, pitch, msaa, depth, ints, addr; };\nconstexpr EdramRow kEdram[] = {\n");
  for (uint32_t msaa = 0; msaa <= 2; ++msaa)
    for (uint32_t depth = 0; depth <= 1; ++depth)
      for (uint32_t ints = 0; ints <= 1; ++ints)
        for (uint32_t px : {0u, 1u, 2u, 3u, 39u, 40u, 79u, 80u, 641u})
          for (uint32_t py : {0u, 1u, 15u, 16u, 719u})
            for (uint32_t s = 0; s < (1u << msaa); ++s) {
              uint32_t pitch = rx::GetSurfacePitchTiles(1280, rx::MsaaSamples(msaa), ints != 0);
              std::fprintf(f, "  {%u, %u, %u, 720, %u, %u, %u, %u, %u},\n", px, py, s, pitch, msaa, depth, ints,
                           CanaryEdramOffsetBytes(px, py, 720, true, pitch, msaa, depth, ints, s, 1, 1));
            }
  std::fprintf(f, "};\n\n");
  std::fprintf(f, "struct PitchRow { uint32_t pitch, msaa, is64, tiles; };\nconstexpr PitchRow kPitch[] = {\n");
  for (uint32_t pitch : {0u, 1u, 79u, 80u, 81u, 320u, 352u, 640u, 800u, 1280u, 1281u, 2560u, 8192u, 16383u})
    for (uint32_t msaa = 0; msaa <= 2; ++msaa)
      for (uint32_t is64 = 0; is64 <= 1; ++is64)
        std::fprintf(f, "  {%u, %u, %u, %u},\n", pitch, msaa, is64,
                     rx::GetSurfacePitchTiles(pitch, rx::MsaaSamples(msaa), is64 != 0));
  std::fprintf(f, "};\n\n");

  // Gamma and fixed point: hashed stride sweeps + explicit tables.
  uint64_t hg1 = gn_golden::SweepFloatBits([](float v) { return std::bit_cast<uint32_t>(gn_canary::PWLGammaToLinear(v)); });
  uint64_t hg2 = gn_golden::SweepFloatBits([](float v) { return std::bit_cast<uint32_t>(gn_canary::LinearToPWLGamma(v)); });
  uint64_t hfx = gn_golden::SweepFloatBits([](float v) { return uint32_t(rex::ui::FloatToD3D11Fixed16p8(v)); });
  std::fprintf(f, "constexpr uint64_t kHashGammaToLinear = 0x%016" PRIX64 "ull;\n", hg1);
  std::fprintf(f, "constexpr uint64_t kHashLinearToGamma = 0x%016" PRIX64 "ull;\n", hg2);
  std::fprintf(f, "constexpr uint64_t kHashFixed16p8 = 0x%016" PRIX64 "ull;\n", hfx);
  std::fprintf(f, "constexpr uint32_t kGammaToLinear256[256] = {");
  for (uint32_t i = 0; i < 256; ++i)
    std::fprintf(f, "%s0x%08X", i % 8 ? ", " : (i == 0 ? "\n  " : ",\n  "), std::bit_cast<uint32_t>(gn_canary::PWLGammaToLinear(float(i) / 255.0f)));
  std::fprintf(f, "};\nconstexpr uint32_t kLinearToGamma1024[1024] = {");
  for (uint32_t i = 0; i < 1024; ++i)
    std::fprintf(f, "%s0x%08X", i % 8 ? ", " : (i == 0 ? "\n  " : ",\n  "), std::bit_cast<uint32_t>(gn_canary::LinearToPWLGamma(float(i) / 1023.0f)));
  std::fprintf(f, "};\n\n");
  std::fprintf(f, "struct FixedRow { uint32_t bits; int32_t fixed; };\nconstexpr FixedRow kFixed[] = {\n");
  for (float v : {0.0f, -0.0f, 1.0f / 512.0f, 1.0f / 513.0f, 0.5f, -0.5f, 0.001953125f, 1279.5f, 719.5f, -0.5f, 3.0f / 512,
                  5.0f / 512, 7.0f / 512, 32767.99f, 32768.0f, -32768.0f, -32769.0f, 1e30f, -1e30f, 100.12345f,
                  -100.12345f, 0.0029296875f, 0.0048828125f, std::nanf(""), INFINITY, -INFINITY})
    std::fprintf(f, "  {0x%08X, %d},\n", std::bit_cast<uint32_t>(v), rex::ui::FloatToD3D11Fixed16p8(v));
  std::fprintf(f, "};\n\n");

  // Sample select.
  std::fprintf(f, "constexpr uint8_t kSampleSelect[8][3][2] = {\n");
  for (uint32_t s = 0; s < 8; ++s) {
    std::fprintf(f, "  {");
    for (uint32_t m = 0; m < 3; ++m)
      std::fprintf(f, "{%u, %u}%s",
                   uint32_t(CanarySanitizeCopySampleSelect(rx::CopySampleSelect(s), rx::MsaaSamples(m), false)),
                   uint32_t(CanarySanitizeCopySampleSelect(rx::CopySampleSelect(s), rx::MsaaSamples(m), true)),
                   m < 2 ? ", " : "");
    std::fprintf(f, "},\n");
  }
  std::fprintf(f, "};\n\n");

  // Resolves.
  auto rcases = gn_golden::ResolveCases();
  uint64_t agree = 0, comparable = 0, phase0 = 0;
  // Explicit rows for the first kResolveRows cases (readable failures); one hash over all cases.
  constexpr size_t kResolveRows = 407;
  Hash hres;
  std::fprintf(f, "constexpr uint32_t kResolve[][25] = {\n");
  for (size_t ci = 0; ci < rcases.size(); ++ci) {
    const auto& c = rcases[ci];
    RefResolveOut o = CanaryGetResolveInfo(c);
    const uint32_t* w = reinterpret_cast<const uint32_t*>(&o);
    for (int i = 0; i < 25; ++i) hres.Add(w[i]);
    if (ci < kResolveRows) {
      std::fprintf(f, "  {");
      for (int i = 0; i < 25; ++i) std::fprintf(f, "%s%u", i ? ", " : "", w[i]);
      std::fprintf(f, "},\n");
    }
    const rg::FormatInfo& fi = *rg::FormatInfo::Get(o.dest_format);
    if (o.valid && o.width_div_8 && !c.dest_array && c.copy_command != 2 && c.src_select < 4 &&
        fi.type == rg::FormatType::kResolvable && fi.block_width == 1) {
      ++comparable;
      uint32_t bpp_log2 = rex::log2_floor(fi.bytes_per_block());
      bool ph0 = ((c.dest_base & 4095) >> (10 + bpp_log2)) == 0;
      phase0 += ph0;
      uint32_t b, s, l;
      RexglueDest(c, o, b, s, l);
      if (b == o.dest_base && s == o.ext_start && l == o.ext_len) ++agree;
    }
  }
  std::fprintf(f, "};\nconstexpr uint64_t kHashResolve = 0x%016" PRIX64 "ull;\n\n", hres.h);

  // Rectangle lists.
  auto rects = gn_golden::RectCases();
  std::fprintf(f, "constexpr uint8_t kRectOrder[] = {");
  for (size_t i = 0; i < rects.size() / 6; ++i) {
    uint32_t ord[3];
    CanaryRectOrder(&rects[i * 6], ord);
    std::fprintf(f, "%s%u", i ? (i % 32 ? ", " : ",\n  ") : "\n  ", ord[0]);
  }
  std::fprintf(f, "};\n\n");

  // Polygon offset: the SDK's inline draw_util::GetD3D10IntegerPolygonOffset (text-identical to Canary draw_util.h).
  std::fprintf(f, "struct PolyRow { uint32_t bits, float24; int32_t value; };\nconstexpr PolyRow kPoly[] = {\n");
  for (float v : {0.0f, 1e-7f, -1e-7f, 1.0f / 16777215.0f, 2.0f / 16777215.0f, 0.0001f, -0.0001f, 0.001f, 0.5f, 1.0f,
                  -1.0f, 3.0f / 16777216.0f, 1e-9f, 0.0000152587890625f})
    for (bool f24 : {false, true})
      std::fprintf(f, "  {0x%08X, %u, %d},\n", std::bit_cast<uint32_t>(v), uint32_t(f24),
                   rg::draw_util::GetD3D10IntegerPolygonOffset(
                       f24 ? rx::DepthRenderTargetFormat::kD24FS8 : rx::DepthRenderTargetFormat::kD24S8, v));
  std::fprintf(f, "};\n\n}  // namespace gn_golden_data\n");
  std::fclose(f);

  // Exhaustive cross-checks of our headers against both references.
  uint64_t bad_g1 = ParallelCountAll32([](uint32_t b) {
    float v = std::bit_cast<float>(b);
    const uint32_t ours = std::bit_cast<uint32_t>(::xenos::gamma::PWLGammaToLinear(v));
    return ours != std::bit_cast<uint32_t>(gn_canary::PWLGammaToLinear(v)) ||
           ours != std::bit_cast<uint32_t>(rx::PWLGammaToLinear(v));
  });
  uint64_t bad_g2 = ParallelCountAll32([](uint32_t b) {
    float v = std::bit_cast<float>(b);
    const uint32_t ours = std::bit_cast<uint32_t>(::xenos::gamma::LinearToPWLGamma(v));
    return ours != std::bit_cast<uint32_t>(gn_canary::LinearToPWLGamma(v)) ||
           ours != std::bit_cast<uint32_t>(rx::LinearToPWLGamma(v));
  });
  uint64_t bad_fx = ParallelCountAll32([](uint32_t b) {
    float v = std::bit_cast<float>(b);
    return rex::ui::FloatToD3D11Fixed16p8(v) != ::xenos::resolve::FloatToFixed16p8(v);
  });
  std::printf("wrote %s\n", out_path);
  std::printf("layouts %zu, resolves %zu, rects %zu\n", cases.size(), rcases.size(), rects.size() / 6);
  std::printf("exhaustive 2^32: gamma->linear mismatches %" PRIu64 ", linear->gamma %" PRIu64
              ", fixed16.8 %" PRIu64 "\n",
              bad_g1, bad_g2, bad_fx);
  std::printf("rexglue vs Canary: format table rows differing %u/64; tiled2D %" PRIu64 ", tiled3D %" PRIu64
              ", packed mips %" PRIu64 " differing values\n",
              format_table_diffs, rx_diff_2d, rx_diff_3d, rx_diff_packed);
  std::printf("rexglue vs Canary bounds differing: lower2D %" PRIu64 "/%" PRIu64 ", upper2D %" PRIu64 "/%" PRIu64
              ", lower3D %" PRIu64 "/%" PRIu64 ", upper3D %" PRIu64 "/%" PRIu64 "\n",
              rx_diff_b[0], rx_n_b[0], rx_diff_b[1], rx_n_b[1], rx_diff_b[2], rx_n_b[2], rx_diff_b[3], rx_n_b[3]);
  std::printf("rexglue vs Canary layouts differing: %" PRIu64 "/%zu\n", rx_diff_layout, cases.size());
  std::printf("resolve dest, Canary vs rexglue (2D, colour, 1x1-block resolvable): %" PRIu64 "/%" PRIu64
              " agree; %" PRIu64 " of them have macro phase 0\n",
              agree, comparable, phase0);
  return 0;
}
