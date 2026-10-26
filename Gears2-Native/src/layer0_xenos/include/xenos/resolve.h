// Layer 0 (game-free): what a Xenos resolve (EDRAM -> memory copy) covers - the pixel rectangle from the vf0 vertices,
// the EDRAM source tiles, the sample selection and the destination address range. Plain CPU code; our renderer's
// resolve path runs these numbers and then dispatches its own copy (S3.5 M1, region-to-region per D29).
//
// Behaviour follows Xenia Canary e6cd2e4 src/xenia/gpu/draw_util.cc GetResolveInfo / SanitizeCopySampleSelect and
// src/xenia/ui/graphics_util.cc FloatToD3D11Fixed16p8 (BSD-3-Clause, Copyright (c) Ben Vanik and Xenia project
// contributors). Re-expressed here; checked against a verbatim copy of the Canary logic in
// tools/golden/gn_renderer_core_golden.cpp. Differences from the rexglue SDK 0.10.0 copy of the same function
// (src/graphics/util/draw.cpp), which is older:
//  - Canary aligns the destination pitch/height to 32, moves the low "macro phase" of an 8/16bpp destination base
//    back into x (534307D5 fix), reads the 3D slice height from RB_COPY_SURFACE_SLICE, and measures the destination
//    extent from the block-aligned destination origin. rexglue does none of these. For 32bpp+ 2D destinations whose
//    pitch is already a multiple of 32 and whose rectangle starts at (0, 0) the two agree (the generator counts this).
//  - The scissor is an input here: the caller passes the result of the guest scissor rules
//    (draw_util GetScissor(regs, scissor, clamp_to_surface_pitch = false)).
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include "xenos/edram.h"
#include "xenos/texture_layout.h"

namespace xenos::resolve {

// D3D11 float -> 16.8 fixed point (D3D11.3 functional spec 3.2.4.1), round to nearest even.
inline int32_t FloatToFixed16p8(float f32) {
  if (!(std::abs(f32) >= 1.0f / 512.0f)) return 0;  // also NaN
  if (f32 >= 32768.0f - 1.0f / 256.0f) return (1 << 23) - 1;
  if (f32 <= -32768.0f) return -32768 * 256;
  const uint32_t bits = std::bit_cast<uint32_t>(f32);
  uint32_t r = (bits & ((1u << 23) - 1)) | (1u << 23);
  if (bits >> 31) r = uint32_t(-int32_t(r));
  const int32_t exponent = int32_t((bits >> 23) & 255) - 127;
  const uint32_t extra_bits = uint32_t(15 - exponent);
  if (extra_bits) {
    r += (1u << (extra_bits - 1)) - 1 + ((r >> extra_bits) & 1u);
    return int32_t(r) >> extra_bits;  // arithmetic shift (sign-extending)
  }
  return int32_t(r);
}

enum class CopySampleSelect : uint32_t { k0, k1, k2, k3, k01, k23, k0123 };
enum class CopyCommand : uint32_t { kRaw = 0, kConvert = 1, kConvertTo1111 = 2, kNull = 3 };

// Depth can't be averaged; selections beyond the sample count fold back.
constexpr CopySampleSelect SanitizeSampleSelect(CopySampleSelect s, edram::Msaa msaa, bool is_depth) {
  using S = CopySampleSelect;
  if (msaa >= edram::Msaa::k4X) {
    if (s > S::k0123) s = S::k0123;
    if (is_depth) {
      if (s == S::k01 || s == S::k0123) s = S::k0;
      else if (s == S::k23) s = S::k2;
    }
  } else if (msaa >= edram::Msaa::k2X) {
    if (s == S::k2) s = S::k0;
    else if (s == S::k3) s = S::k1;
    else if (s > S::k01) s = S::k01;
    if (is_depth && s == S::k01) s = S::k0;
  } else {
    s = S::k0;
  }
  return s;
}

constexpr uint32_t kTextureFormat_k_24_8 = 22;
constexpr uint32_t kTextureFormat_k_24_8_FLOAT = 23;
constexpr uint32_t kTextureFormat_k_DXT3A_AS_1_1_1_1 = 61;

// The register state a resolve reads, already decoded. Vertices are the six floats at vf0, already endian-swapped.
struct Regs {
  float vertices[6] = {};
  bool pix_center_d3d_zero = true;  // PA_SU_VTX_CNTL.pix_center == kD3DZero -> +0.5 to undo the half-pixel offset
  bool vtx_window_offset_enable = false;
  int32_t window_x_offset = 0, window_y_offset = 0;  // PA_SC_WINDOW_OFFSET
  int32_t scissor_x = 0, scissor_y = 0;              // GetScissor(..., false) result
  int32_t scissor_w = 8192, scissor_h = 8192;
  uint32_t surface_pitch = 0;  // RB_SURFACE_INFO
  uint32_t msaa = 0;           // RB_SURFACE_INFO.msaa_samples (0..3; 3 is rejected)
  // RB_COPY_CONTROL
  uint32_t copy_src_select = 0;  // 0..3 colour, >= 4 depth
  uint32_t copy_sample_select = 0;
  uint32_t copy_command = 0;
  bool depth_clear_enable = false;
  // Source surfaces
  uint32_t color_base = 0;    // RB_COLOR_INFO[copy_src_select].color_base
  uint32_t color_format = 0;  // RB_COLOR_INFO[copy_src_select].color_format
  uint32_t depth_base = 0;    // RB_DEPTH_INFO
  uint32_t depth_format = 0;
  // Destination
  uint32_t copy_dest_base = 0;  // RB_COPY_DEST_BASE (bytes)
  uint32_t copy_dest_pitch = 0, copy_dest_height = 0;  // RB_COPY_DEST_PITCH
  bool copy_dest_array = false;
  uint32_t copy_dest_slice = 0;
  uint32_t copy_dest_format = 0;  // RB_COPY_DEST_INFO.copy_dest_format (TextureFormat)
  uint32_t copy_surface_slice = 0;  // RB_COPY_SURFACE_SLICE
};

struct EdramSource {
  bool used = false;
  uint32_t base_tiles = 0, pitch_tiles = 0;
  bool is_64bpp = false;
};

struct Info {
  bool valid = false;  // false = the guest asked for something unsupported (Canary returns false)
  // Rectangle in pixels after vertex snapping, window offset, scissor, 8-alignment and pitch clamp.
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  uint32_t width_div_8 = 0, height_div_8 = 0;  // 0 = nothing to do
  CopySampleSelect sample_select = CopySampleSelect::k0;
  bool is_depth = false;
  uint32_t edram_offset_x_div_8 = 0, edram_offset_y_div_8 = 0;  // within the first tile
  EdramSource color, depth;
  // Destination
  uint32_t dest_format = 0;
  bool dest_supported = false;  // resolvable (or depth, or supported 1111); otherwise nothing is written
  uint32_t dest_bpp_log2 = 0;
  uint32_t dest_pitch_aligned_div_32 = 0, dest_height_aligned_div_32 = 0;
  uint32_t dest_offset_x_div_8 = 0, dest_offset_y_div_8 = 0;
  uint32_t dest_base = 0;  // adjusted base the copy writes relative to
  uint32_t dest_extent_start = 0, dest_extent_length = 0;  // bytes touched
};

inline Info GetInfo(const Regs& r) {
  Info o;
  const bool is_1111 = r.copy_command == uint32_t(CopyCommand::kConvertTo1111);
  if (r.copy_command != uint32_t(CopyCommand::kRaw) && r.copy_command != uint32_t(CopyCommand::kConvert) && !is_1111) {
    return o;
  }

  // Rectangle: top-left rule on the three vf0 vertices in 16.8 fixed point.
  const float half = r.pix_center_d3d_zero ? 0.5f : 0.0f;
  int32_t f[6];
  for (int i = 0; i < 6; ++i) f[i] = FloatToFixed16p8(r.vertices[i] + half);
  int32_t x0 = std::min(std::min(f[0], f[2]), f[4]);
  int32_t y0 = std::min(std::min(f[1], f[3]), f[5]);
  int32_t x1 = std::max(std::max(f[0], f[2]), f[4]);
  int32_t y1 = std::max(std::max(f[1], f[3]), f[5]);
  x0 = (x0 + 127) >> 8;
  y0 = (y0 + 127) >> 8;
  x1 = (x1 + 127) >> 8;
  y1 = (y1 + 127) >> 8;
  if (r.vtx_window_offset_enable) {
    x0 += r.window_x_offset;
    y0 += r.window_y_offset;
    x1 += r.window_x_offset;
    y1 += r.window_y_offset;
  }
  const int32_t sr = r.scissor_x + r.scissor_w, sb = r.scissor_y + r.scissor_h;
  x0 = std::clamp(x0, r.scissor_x, sr);
  y0 = std::clamp(y0, r.scissor_y, sb);
  x1 = std::clamp(x1, r.scissor_x, sr);
  y1 = std::clamp(y1, r.scissor_y, sb);
  constexpr int32_t kAlign = int32_t(edram::kResolveAlignmentPixels);
  x0 &= ~(kAlign - 1);
  y0 &= ~(kAlign - 1);
  x1 = (x1 + kAlign - 1) / kAlign * kAlign;  // xe::align on int32 (values here are >= 0 after the scissor clamp)
  y1 = (y1 + kAlign - 1) / kAlign * kAlign;
  if (r.msaa > uint32_t(edram::Msaa::k4X)) return o;
  const edram::Msaa msaa = edram::Msaa(r.msaa);
  const int32_t pitch_aligned = int32_t(r.surface_pitch & ~(edram::kResolveAlignmentPixels - 1));
  if (x1 > pitch_aligned) {
    x0 = std::min(x0, pitch_aligned);
    x1 = std::min(x1, pitch_aligned);
  }
  constexpr int32_t kMaxResolveSize = (1 << 14) - 8;
  if (y1 - y0 > kMaxResolveSize) y1 = y0 + kMaxResolveSize;
  o.valid = true;
  o.x0 = x0;
  o.y0 = y0;
  o.x1 = x1;
  o.y1 = y1;
  if (x0 >= x1 || y0 >= y1) return o;  // empty: a no-op
  o.width_div_8 = uint32_t(x1 - x0) >> 3;
  o.height_div_8 = uint32_t(y1 - y0) >> 3;

  // Source and sample selection.
  o.is_depth = r.copy_src_select >= 4;
  o.sample_select = SanitizeSampleSelect(CopySampleSelect(r.copy_sample_select), msaa, o.is_depth);
  uint32_t dest_format = o.is_depth ? (r.depth_format == uint32_t(edram::DepthFormat::kD24FS8)
                                           ? kTextureFormat_k_24_8_FLOAT
                                           : kTextureFormat_k_24_8)
                                    : r.copy_dest_format;
  o.dest_format = dest_format;
  bool supported_1111 = false;
  if (is_1111 && !o.is_depth && dest_format == kTextureFormat_k_DXT3A_AS_1_1_1_1 && !r.copy_dest_array &&
      (r.color_format == uint32_t(edram::ColorFormat::k_8_8_8_8) ||
       r.color_format == uint32_t(edram::ColorFormat::k_8_8_8_8_GAMMA)) &&
      o.sample_select <= CopySampleSelect::k3) {
    supported_1111 = true;
  }

  // Destination.
  const uint32_t pitch_al = texture::AlignUp(r.copy_dest_pitch, 32);
  o.dest_pitch_aligned_div_32 = pitch_al >> 5;
  uint32_t dest_height = r.copy_dest_height;
  if (r.copy_dest_array && r.copy_surface_slice && r.copy_dest_pitch) {
    dest_height = r.copy_surface_slice / r.copy_dest_pitch;
  }
  const uint32_t height_al = texture::AlignUp(dest_height, 32);
  o.dest_height_aligned_div_32 = height_al >> 5;
  const texture::FormatBlock& fb = texture::GetFormatBlock(dest_format);
  uint32_t base_adjusted = r.copy_dest_base;
  uint32_t ext_start, ext_end;
  o.dest_supported = is_1111 ? supported_1111 : (o.is_depth || fb.type == texture::FormatType::kResolvable);
  if (o.dest_supported) {
    const uint32_t bpp_log2 = texture::Log2Floor(fb.bytes_per_block());
    o.dest_bpp_log2 = bpp_log2;
    const uint32_t block_log2 = is_1111 ? 2 : 0;
    // xenos::GetTextureTiled{X,Y}BaseGranularityLog2
    const uint32_t xg = 7 - std::min(2u, bpp_log2 + uint32_t(r.copy_dest_array));
    const uint32_t yg = r.copy_dest_array ? 5 : 7 - std::min(2u, bpp_log2);
    const uint32_t xmask = (1u << xg) - 1, ymask = (1u << yg) - 1;
    uint32_t addr_base = r.copy_dest_base;
    uint32_t ax0 = uint32_t(x0) >> block_log2, ay0 = uint32_t(y0) >> block_log2;
    if (!r.copy_dest_array) {
      const uint32_t macro_log2 = 2 * texture::kTileWidthHeightLog2 + bpp_log2;
      const uint32_t phase = (r.copy_dest_base & (texture::kSubresourceAlignmentBytes - 1)) >> macro_log2;
      addr_base -= phase << macro_log2;
      ax0 += phase << texture::kTileWidthHeightLog2;
    }
    const uint32_t ax1 = ax0 + (uint32_t(x1 - x0) >> block_log2);
    const uint32_t ay1 = ay0 + (uint32_t(y1 - y0) >> block_log2);
    base_adjusted = addr_base;
    o.dest_offset_x_div_8 = ((ax0 & xmask) << block_log2) >> 3;
    o.dest_offset_y_div_8 = ((ay0 & ymask) << block_log2) >> 3;
    const uint32_t bx = ax0 & ~xmask, by = ay0 & ~ymask;
    if (r.copy_dest_array) {
      base_adjusted += uint32_t(texture::TiledOffset3D(int32_t(bx), int32_t(by), 0, pitch_al, height_al, bpp_log2));
      ext_start = addr_base + texture::TiledAddressLowerBound3D(ax0, ay0, r.copy_dest_slice, pitch_al, height_al,
                                                                bpp_log2);
      ext_end = addr_base + texture::TiledAddressUpperBound3D(ax1, ay1, r.copy_dest_slice + 1, pitch_al, height_al,
                                                              bpp_log2);
    } else {
      base_adjusted += uint32_t(texture::TiledOffset2D(int32_t(bx), int32_t(by), pitch_al, bpp_log2));
      ext_start = addr_base + texture::TiledAddressLowerBound2D(ax0, ay0, pitch_al, bpp_log2);
      ext_end = addr_base + texture::TiledAddressUpperBound2D(ax1, ay1, pitch_al, bpp_log2);
    }
  } else {
    ext_start = ext_end = base_adjusted;
  }
  o.dest_base = base_adjusted;
  o.dest_extent_start = ext_start;
  o.dest_extent_length = ext_end - ext_start;

  // EDRAM source: offset inside the first tile, and the tile the rectangle starts in.
  const uint32_t sxl = uint32_t(msaa >= edram::Msaa::k4X), syl = uint32_t(msaa >= edram::Msaa::k2X);
  const uint32_t x0s = uint32_t(x0) << sxl, y0s = uint32_t(y0) << syl;
  const uint32_t off_tx = x0s / edram::kTileWidthSamples, off_ty = y0s / edram::kTileHeightSamples;
  o.edram_offset_x_div_8 = (x0s % edram::kTileWidthSamples) >> (sxl + 3);
  o.edram_offset_y_div_8 = (y0s % edram::kTileHeightSamples) >> (syl + 3);
  const uint32_t pitch_tiles = edram::SurfacePitchTiles(r.surface_pitch, msaa, false);
  const uint32_t base_off = off_ty * pitch_tiles + off_tx;
  if (o.is_depth || r.depth_clear_enable) {
    o.depth.used = true;
    o.depth.pitch_tiles = pitch_tiles;
    o.depth.base_tiles = r.depth_base + base_off;
  }
  if (!o.is_depth) {
    const bool is64 = edram::Is64bpp(edram::ColorFormat(r.color_format));
    o.color.used = true;
    o.color.is_64bpp = is64;
    o.color.pitch_tiles = pitch_tiles << uint32_t(is64);
    o.color.base_tiles = r.color_base + (base_off << uint32_t(is64));
  }
  return o;
}

}  // namespace xenos::resolve
