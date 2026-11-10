// Part of tools/golden/gn_renderer_core_golden (see that file). This translation unit is compiled against the Xenia
// Canary e6cd2e4 sources (-I <canary>/src) and links Canary's own texture_util.cc / texture_info*.cc / texture_extent.cc
// and xenos.cc, exposing them through plain functions so the generator can run Canary and rexglue side by side
// (their namespaces and headers can't share a translation unit). Canary/Xenia: BSD-3-Clause, Copyright (c) Ben Vanik
// and Xenia project contributors. Not part of the game build. Game-free.
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

#include "golden/layer0_renderer_core_sweeps.h"

namespace xg = xe::gpu;

namespace gn_canary {

// Canary's tiled functions require 32-aligned pitch/height; the sweeps pass raw values, as rexglue aligns internally.
static uint32_t A32(uint32_t v) { return (v + 31) & ~31u; }

int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bpp) {
  return xg::texture_address::Tiled2D(x, y, A32(pitch), bpp);
}
int64_t Tiled3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height, uint32_t bpp) {
  return xg::texture_address::Tiled3D(x, y, z, A32(pitch), A32(height), bpp);
}
uint32_t Bound(uint32_t r, uint32_t b, uint32_t z, uint32_t pitch, uint32_t height, uint32_t bpp, uint32_t kind) {
  switch (kind) {
    case 0: return xg::texture_util::GetTiledAddressLowerBound2D(r, b, A32(pitch), bpp);
    case 1: return xg::texture_util::GetTiledAddressUpperBound2D(r, b, A32(pitch), bpp);
    case 2: return uint32_t(xg::texture_util::GetTiledAddressLowerBound3D(r, b, z, A32(pitch), A32(height), bpp));
    default: return uint32_t(xg::texture_util::GetTiledAddressUpperBound3D(r, b, z, A32(pitch), A32(height), bpp));
  }
}
bool PackedMipOffset(uint32_t w, uint32_t h, uint32_t d, uint32_t format, uint32_t mip, uint32_t& x, uint32_t& y,
                     uint32_t& z) {
  return xg::texture_util::GetPackedMipOffset(w, h, d, xg::xenos::TextureFormat(format), mip, x, y, z);
}
uint64_t Layout(const gn_golden::LayoutCase& c, uint32_t& base_extent, uint32_t& mips_total, uint32_t& packed_level,
                uint32_t& max_level) {
  const auto l = xg::texture_util::GetGuestTextureLayout(
      xg::xenos::DataDimension(c.dimension), c.pitch_div_32, c.width, c.height, c.depth, c.tiled,
      xg::xenos::TextureFormat(c.format), c.packed, c.has_base, c.max_level);
  base_extent = l.base.level_data_extent_bytes;
  mips_total = l.mips_total_extent_bytes;
  packed_level = l.packed_level;
  max_level = l.max_level;
  return gn_golden::HashLayout(l, c.has_base);
}
void Format(uint32_t i, uint32_t& bw, uint32_t& bh, uint32_t& bpp, uint32_t& type) {
  const xg::FormatInfo* fi = xg::FormatInfo::Get(i);
  bw = fi->block_width;
  bh = fi->block_height;
  bpp = fi->bits_per_pixel;
  type = uint32_t(fi->type);
}
float PWLGammaToLinear(float v) { return xg::xenos::PWLGammaToLinear(v); }
float LinearToPWLGamma(float v) { return xg::xenos::LinearToPWLGamma(v); }

}  // namespace gn_canary
