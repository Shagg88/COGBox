// Unit tests for the S3.5 layer-0 renderer core (GPU-free): EDRAM addressing, resolve rectangles and destinations,
// PWL gamma, primitive conversions and texture layouts. Each function is compared with golden tables produced by
// running the reference code (tools/golden/gn_renderer_core_golden.cpp -> tests/golden/layer0_renderer_core_golden.inc)
// over the shared sweeps in tests/golden/layer0_renderer_core_sweeps.h, plus hand-checked cases. Game-free.
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include "golden/layer0_renderer_core_golden.inc"
#include "golden/layer0_renderer_core_sweeps.h"
#include "xenos/edram.h"
#include "xenos/formats.h"
#include "xenos/gamma.h"
#include "xenos/primitives.h"
#include "xenos/resolve.h"
#include "xenos/texture_layout.h"

namespace gd = gn_golden_data;
namespace tx = xenos::texture;
namespace ed = xenos::edram;
namespace rs = xenos::resolve;

static uint32_t Bits(float f) { return std::bit_cast<uint32_t>(f); }

// ---------------------------------------------------------------------------------------------------------------------
// Texture layout
// ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("texture: format block table matches the reference FormatInfo", "[renderer_core][texture]") {
  for (uint32_t i = 0; i < 64; ++i) {
    const tx::FormatBlock& b = tx::GetFormatBlock(i);
    INFO("format " << i);
    CHECK(b.block_width == gd::kFormats[i].bw);
    CHECK(b.block_height == gd::kFormats[i].bh);
    CHECK(b.bits_per_pixel == gd::kFormats[i].bpp);
    CHECK(uint32_t(b.type) == gd::kFormats[i].type);
  }
}

TEST_CASE("texture: tiled 2D/3D offsets and bounds match the reference", "[renderer_core][texture]") {
  CHECK(gn_golden::SweepTiled2D(tx::TiledOffset2D) == gd::kHashTiled2D);
  CHECK(gn_golden::SweepTiled3D(tx::TiledOffset3D) == gd::kHashTiled3D);
  CHECK(gn_golden::SweepTiledBounds([](uint32_t r, uint32_t b, uint32_t z, uint32_t pitch, uint32_t height,
                                       uint32_t bpp, uint32_t kind) -> uint32_t {
          switch (kind) {
            case 0: return tx::TiledAddressLowerBound2D(r, b, pitch, bpp);
            case 1: return tx::TiledAddressUpperBound2D(r, b, pitch, bpp);
            case 2: return tx::TiledAddressLowerBound3D(r, b, z, pitch, height, bpp);
            default: return tx::TiledAddressUpperBound3D(r, b, z, pitch, height, bpp);
          }
        }) == gd::kHashTiledBounds);
  for (const auto& r : gd::kTiled2D) {
    INFO("x " << r.x << " y " << r.y << " bpp " << r.bpp);
    CHECK(tx::TiledOffset2D(r.x, r.y, r.pitch, r.bpp) == r.off);
  }
}

TEST_CASE("texture: our TiledOffset2D agrees with the existing formats::Tiled2D", "[renderer_core][texture]") {
  for (uint32_t bpp = 0; bpp <= 4; ++bpp)
    for (uint32_t pitch : {32u, 64u, 96u, 352u, 1280u})
      for (int32_t y = 0; y < 100; ++y)
        for (int32_t x = 0; x < 200; x += 3) {
          REQUIRE(tx::TiledOffset2D(x, y, pitch, bpp) == xenos::formats::Tiled2D(x, y, pitch, bpp));
        }
}

TEST_CASE("texture: packed mip tail offsets match the reference", "[renderer_core][texture]") {
  CHECK(gn_golden::SweepPackedMip(tx::PackedMipOffset) == gd::kHashPackedMip);
  for (const auto& r : gd::kPacked) {
    uint32_t x, y, z;
    const bool p = tx::PackedMipOffset(r.w, r.h, r.d, r.fmt, r.mip, x, y, z);
    INFO(r.w << "x" << r.h << " mip " << r.mip);
    CHECK(p == bool(r.packed));
    CHECK(x == r.x);
    CHECK(y == r.y);
    CHECK(z == r.z);
  }
  // Hand-checked against the reference's "graph paper" comment: a 64x64 texture packs from mip 2; 16x16, 8x8 and 4x4
  // go along the top at x = 16, 8, 4; from the 2x2 on (packed mip >= 3) a square tail goes down the left: 2x2 at y = 8.
  CHECK(tx::PackedMipLevel(64, 64) == 2);
  uint32_t x, y, z;
  REQUIRE(tx::PackedMipOffset(64, 64, 1, 6, 2, x, y, z));
  CHECK((x == 16 && y == 0));
  REQUIRE(tx::PackedMipOffset(64, 64, 1, 6, 4, x, y, z));
  CHECK((x == 4 && y == 0));
  REQUIRE(tx::PackedMipOffset(64, 64, 1, 6, 5, x, y, z));
  CHECK((x == 0 && y == 8));
  CHECK_FALSE(tx::PackedMipOffset(64, 64, 1, 6, 1, x, y, z));
  // Deep 3D tails (outside the golden sweep, whose depth stops at 32): Canary e6cd2e4 texture_util.cc puts the 1x1 Z
  // mips at (log2_ceil(depth) - mip) * 4 using the absolute mip, else 4. Hand-derived from that line.
  REQUIRE(tx::PackedMipOffset(32, 32, 256, 6, 6, x, y, z));  // packed base 1, packed mip 5; 8 > 7 -> (8 - 6) * 4
  CHECK(z == 8);
  REQUIRE(tx::PackedMipOffset(17, 17, 65, 6, 6, x, y, z));  // log2_ceil(65) = 7, not > 7 -> 4
  CHECK(z == 4);
}
// The index order Canary's geometry-shader quad path produces (its normal path on D3D12 and Vulkan).
TEST_CASE("primitives: quad list in geometry-shader order", "[renderer_core][prim]") {
  const uint32_t quads[] = {0, 1, 2, 3, 4, 5, 6, 7};
  uint32_t out[12] = {};
  xenos::prim::QuadListToTriangleListGsOrder(out, quads, 8);
  const uint32_t expect[] = {0, 1, 3, 1, 2, 3, 4, 5, 7, 5, 6, 7};
  for (int i = 0; i < 12; ++i) CHECK(out[i] == expect[i]);
}

TEST_CASE("texture: guest layouts match the reference", "[renderer_core][texture]") {
  const auto cases = gn_golden::LayoutCases();
  REQUIRE(cases.size() == std::size(gd::kLayouts));
  for (size_t i = 0; i < cases.size(); ++i) {
    const auto& c = cases[i];
    const tx::GuestLayout l =
        tx::GetGuestLayout(tx::Dimension(c.dimension), c.pitch_div_32, c.width, c.height, c.depth, c.tiled, c.format,
                           c.packed, c.has_base, c.max_level);
    INFO("case " << i << ": dim " << c.dimension << " " << c.width << "x" << c.height << "x" << c.depth << " fmt "
                 << c.format << " tiled " << c.tiled << " packed " << c.packed << " base " << c.has_base
                 << " max_level " << c.max_level);
    CHECK(l.base.level_data_extent_bytes == gd::kLayouts[i].base_extent);
    CHECK(l.mips_total_extent_bytes == gd::kLayouts[i].mips_total);
    CHECK(l.packed_level == gd::kLayouts[i].packed_level);
    CHECK(l.max_level == gd::kLayouts[i].max_level);
    CHECK(gn_golden::HashLayout(l, c.has_base) == gd::kLayouts[i].hash);
  }
}

TEST_CASE("texture: hand-checked layouts", "[renderer_core][texture]") {
  // 1280x720 k_8_8_8_8 tiled, pitch 1280: 1280 x 736 (height to 32) x 4 bytes = 3,768,320; the last 32x32 tile ends
  // exactly there, so the data extent equals the stride.
  auto l = tx::GetGuestLayout(tx::Dimension::k2DOrStacked, 40, 1280, 720, 1, true, 6, true, true, 0);
  CHECK(l.base.row_pitch_bytes == 5120);
  CHECK(l.base.z_slice_stride_block_rows == 736);
  CHECK(l.base.array_slice_stride_bytes == 3768320);
  CHECK(l.base.level_data_extent_bytes == 3768320);
  // 1024x1024 DXT1 (4x4 blocks, 8 bytes): 256 blocks x 8 = 2048 bytes per row, 256 block rows.
  l = tx::GetGuestLayout(tx::Dimension::k2DOrStacked, 32, 1024, 1024, 1, true, 18, true, true, 10);
  CHECK(l.base.row_pitch_bytes == 2048);
  CHECK(l.base.array_slice_stride_bytes == 2048 * 256);
  CHECK(l.packed_level == 6);  // 16x16 and below are packed
  CHECK(l.mip_offsets_bytes[1] == 0);
  CHECK(l.mips[1].row_pitch_bytes == 1024);
  // Linear 1280x720 with pitch 1408: the extent stops at the last real row (4E4D083E note), not at 736 rows.
  l = tx::GetGuestLayout(tx::Dimension::k2DOrStacked, 44, 1280, 720, 1, false, 6, false, true, 0);
  CHECK(l.base.row_pitch_bytes == 1408 * 4);
  CHECK(l.base.level_data_extent_bytes == 1408 * 4 * 719 + 1280 * 4);
}

// ---------------------------------------------------------------------------------------------------------------------
// EDRAM
// ---------------------------------------------------------------------------------------------------------------------

static uint32_t OurEdram(uint32_t px, uint32_t py, uint32_t s, uint32_t base, uint32_t pitch, uint32_t msaa,
                         bool depth, uint32_t ints, uint32_t scale) {
  ed::AddressParams p;
  p.base_tiles = base;
  p.pitch_tiles = pitch;
  p.msaa = ed::Msaa(msaa);
  p.is_depth = depth;
  p.format_ints_log2 = ints;
  p.scale_x = p.scale_y = scale;
  p.layout = ed::SampleLayout::kCanaryInterleaved;
  return ed::OffsetBytes(px, py, s, p);
}

TEST_CASE("edram: surface pitch in tiles matches the reference", "[renderer_core][edram]") {
  for (const auto& r : gd::kPitch) {
    INFO("pitch " << r.pitch << " msaa " << r.msaa << " 64bpp " << r.is64);
    CHECK(ed::SurfacePitchTiles(r.pitch, ed::Msaa(r.msaa), r.is64 != 0) == r.tiles);
  }
  CHECK(ed::SurfacePitchTiles(1280, ed::Msaa::k1X, false) == 16);
  CHECK(ed::SurfacePitchTiles(1280, ed::Msaa::k4X, false) == 32);
  CHECK(ed::SurfacePitchTiles(1280, ed::Msaa::k4X, true) == 64);
  CHECK(ed::SurfacePitchTiles(800, ed::Msaa::k1X, false) == 10);
}

TEST_CASE("edram: sample addresses match Canary's XeEdramOffsetBytes", "[renderer_core][edram]") {
  CHECK(gn_golden::SweepEdram(OurEdram) == gd::kHashEdram);
  for (const auto& r : gd::kEdram) {
    INFO("px " << r.px << " py " << r.py << " s " << r.s << " msaa " << r.msaa << " depth " << r.depth << " ints "
               << r.ints);
    CHECK(OurEdram(r.px, r.py, r.s, r.base, r.pitch, r.msaa, r.depth != 0, r.ints, 1) == r.addr);
  }
}

TEST_CASE("edram: hand-checked addresses", "[renderer_core][edram]") {
  // Scene depth at base 720 (notes/35), 1x: pixel (0,0) lands in the second half of tile 720 (depth half swap).
  ed::AddressParams p;
  p.base_tiles = 720;
  p.pitch_tiles = 16;
  p.is_depth = true;
  CHECK(ed::OffsetBytes(0, 0, 0, p) == (720u * 1280u + 40u) * 4u);
  CHECK(ed::OffsetBytes(40, 0, 0, p) == (720u * 1280u + 0u) * 4u);
  p.is_depth = false;
  CHECK(ed::OffsetBytes(0, 0, 0, p) == 720u * 5120u);
  CHECK(ed::OffsetBytes(80, 0, 0, p) == 721u * 5120u);   // next tile right
  CHECK(ed::OffsetBytes(0, 16, 0, p) == 736u * 5120u);   // next tile row: +pitch
  CHECK(ed::OffsetBytes(1, 1, 0, p) == 720u * 5120u + (80u + 1u) * 4u);
  // Wrap: base 2047, one tile right -> tile 0.
  p.base_tiles = 2047;
  CHECK(ed::OffsetBytes(80, 0, 0, p) == 0u);
  // 64bpp: 40 samples per tile row, 8 bytes each.
  p.base_tiles = 0;
  p.format_ints_log2 = 1;
  p.pitch_tiles = 32;
  CHECK(ed::OffsetBytes(1, 0, 0, p) == 8u);
  CHECK(ed::OffsetBytes(40, 0, 0, p) == 5120u);
  CHECK(ed::UNorm24To32(0xFFFFFF) == 1.0f);
  CHECK(ed::UNorm24To32(0) == 0.0f);
  // Behaves like n / 16777215 (the reference's own example: 0xC00000 is not exactly 0.75).
  CHECK(ed::UNorm24To32(0xC00000) == float(0xC00001) / 16777216.0f);
  CHECK(ed::UNorm24To32(0x400000) == 0.25f);
  CHECK(ed::StorageFormat(ed::ColorFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16) == ed::ColorFormat::k_2_10_10_10_FLOAT);
  CHECK(ed::StorageFormat(ed::ColorFormat::k_2_10_10_10_AS_10_10_10_10) == ed::ColorFormat::k_2_10_10_10);
  CHECK(ed::Is64bpp(ed::ColorFormat::k_16_16_16_16_FLOAT));
  CHECK_FALSE(ed::Is64bpp(ed::ColorFormat::k_16_16_FLOAT));
}

TEST_CASE("edram: both sample layouts are one-to-one and agree where they must", "[renderer_core][edram]") {
  for (auto layout : {ed::SampleLayout::kCanaryInterleaved, ed::SampleLayout::kAdjacent})
    for (uint32_t msaa = 0; msaa <= 2; ++msaa) {
      // Pixels [0,40)x[0,8) with all samples cover exactly the single-sampled block of the right size.
      const uint32_t sx = msaa >= 2 ? 2 : 1, sy = msaa >= 1 ? 2 : 1;
      std::set<uint64_t> seen;
      for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 40; ++x)
          for (uint32_t s = 0; s < (1u << msaa); ++s) {
            const auto c = ed::GuestSample(x, y, s, ed::Msaa(msaa), layout);
            CHECK(c.u < 40 * sx);
            CHECK(c.v < 8 * sy);
            seen.insert((uint64_t(c.v) << 32) | c.u);
          }
      CHECK(seen.size() == size_t(40 * 8) << msaa);
    }
  // 1x is the identity in both; 4x sample 0 of an even pixel sits at (2x, 2y) in both.
  for (uint32_t y = 0; y < 16; y += 2)
    for (uint32_t x = 0; x < 16; x += 2) {
      const auto a = ed::GuestSample(x, y, 0, ed::Msaa::k4X, ed::SampleLayout::kCanaryInterleaved);
      const auto b = ed::GuestSample(x, y, 0, ed::Msaa::k4X, ed::SampleLayout::kAdjacent);
      CHECK((a.u == b.u && a.v == b.v && a.u == 2 * x && a.v == 2 * y));
    }
  // The documented difference: Canary puts 4x sample 1 two columns right, the adjacent layout one row down.
  const auto c1 = ed::GuestSample(0, 0, 1, ed::Msaa::k4X, ed::SampleLayout::kCanaryInterleaved);
  const auto a1 = ed::GuestSample(0, 0, 1, ed::Msaa::k4X, ed::SampleLayout::kAdjacent);
  CHECK((c1.u == 2 && c1.v == 0));
  CHECK((a1.u == 0 && a1.v == 1));
}

TEST_CASE("edram: resolve tile span", "[renderer_core][edram]") {
  // 1280x720 1x from offset 0: 160 columns of 8 -> 16 tiles per row, 90 rows of 8 -> 45 tile rows.
  auto s = ed::ResolveTileSpan(0, 16, ed::Msaa::k1X, false, 0, 0, 160, 90);
  CHECK(s.base_tiles == 0);
  CHECK(s.row_length_used == 16);
  CHECK(s.rows == 45);
  // 4x: samples double in both directions.
  s = ed::ResolveTileSpan(720, 32, ed::Msaa::k4X, false, 0, 0, 160, 90);
  CHECK(s.row_length_used == 32);
  CHECK(s.rows == 90);
  // Offset 1 (8 px) into the first tile, width 10 (80 px): touches tiles 0 and 1.
  s = ed::ResolveTileSpan(100, 16, ed::Msaa::k1X, false, 1, 0, 10, 2);
  CHECK(s.base_tiles == 100);
  CHECK(s.row_length_used == 2);
  CHECK(s.rows == 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// Gamma
// ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("gamma: PWL curves match the reference bit for bit", "[renderer_core][gamma]") {
  CHECK(gn_golden::SweepFloatBits([](float v) { return Bits(xenos::gamma::PWLGammaToLinear(v)); }) ==
        gd::kHashGammaToLinear);
  CHECK(gn_golden::SweepFloatBits([](float v) { return Bits(xenos::gamma::LinearToPWLGamma(v)); }) ==
        gd::kHashLinearToGamma);
  for (uint32_t i = 0; i < 256; ++i) {
    INFO("gamma " << i << "/255");
    CHECK(Bits(xenos::gamma::PWLGammaToLinear(float(i) / 255.0f)) == gd::kGammaToLinear256[i]);
  }
  for (uint32_t i = 0; i < 1024; ++i) {
    INFO("linear " << i << "/1023");
    CHECK(Bits(xenos::gamma::LinearToPWLGamma(float(i) / 1023.0f)) == gd::kLinearToGamma1024[i]);
  }
}

TEST_CASE("gamma: hand-checked values", "[renderer_core][gamma]") {
  using namespace xenos::gamma;
  CHECK(PWLGammaToLinear(0.0f) == 0.0f);
  CHECK(PWLGammaToLinear(1.0f) == 1.0f);
  CHECK(LinearToPWLGamma(0.0f) == 0.0f);
  CHECK(LinearToPWLGamma(1.0f) == 1.0f);
  CHECK(Bits(PWLGammaToLinear(std::nanf(""))) == 0u);  // NaN -> +0
  CHECK(Bits(LinearToPWLGamma(-0.0f)) == 0u);
  CHECK(PWLGammaToLinear(2.0f) == 1.0f);
  // First piece is linear with slope 255/1023: 32/255 -> 32/1023.
  CHECK(PWLGammaToLinear(32.0f / 255.0f) == 32.0f / 1023.0f);
  // gamma -> linear -> gamma in float32 is NOT lossless: the truncation in LinearToPWLGamma drops some codes by one
  // (e.g. 17/255 -> 17/1023 -> 16.9999 -> 16). Both directions match the reference bit for bit (above), so this is
  // the reference behaviour; it never gains, and never loses more than one code.
  uint32_t lost = 0;
  for (uint32_t i = 0; i < 256; ++i) {
    INFO(i);
    const long back = std::lround(LinearToPWLGamma(PWLGammaToLinear(float(i) / 255.0f)) * 255.0f);
    CHECK((back == long(i) || back == long(i) - 1));
    lost += back != long(i);
  }
  CHECK(lost == 16);  // measured (codes 17, 21, 25, ... 122); documents the count so a change in either curve shows up
  // Monotonic.
  float prev = -1.0f;
  for (uint32_t i = 0; i <= 4096; ++i) {
    const float g = PWLGammaToLinear(float(i) / 4096.0f);
    CHECK(g >= prev);
    prev = g;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("resolve: 16.8 fixed point matches the reference", "[renderer_core][resolve]") {
  CHECK(gn_golden::SweepFloatBits([](float v) { return uint32_t(rs::FloatToFixed16p8(v)); }) == gd::kHashFixed16p8);
  for (const auto& r : gd::kFixed) {
    INFO("bits 0x" << std::hex << r.bits);
    CHECK(rs::FloatToFixed16p8(std::bit_cast<float>(r.bits)) == r.fixed);
  }
  CHECK(rs::FloatToFixed16p8(0.5f) == 128);
  CHECK(rs::FloatToFixed16p8(-0.5f) == -128);
  CHECK(rs::FloatToFixed16p8(1280.0f) == 1280 * 256);
  CHECK(rs::FloatToFixed16p8(3.0f / 512.0f) == 2);  // 1.5 -> 2 (to even)
  CHECK(rs::FloatToFixed16p8(5.0f / 512.0f) == 2);  // 2.5 -> 2 (to even)
}

TEST_CASE("resolve: sample selection matches the reference", "[renderer_core][resolve]") {
  for (uint32_t s = 0; s < 8; ++s)
    for (uint32_t m = 0; m < 3; ++m)
      for (uint32_t d = 0; d < 2; ++d) {
        INFO("select " << s << " msaa " << m << " depth " << d);
        CHECK(uint32_t(rs::SanitizeSampleSelect(rs::CopySampleSelect(s), ed::Msaa(m), d != 0)) ==
              gd::kSampleSelect[s][m][d]);
      }
}

static rs::Regs ToRegs(const gn_golden::ResolveCase& c) {
  rs::Regs r;
  for (int i = 0; i < 6; ++i) r.vertices[i] = c.v[i];
  r.pix_center_d3d_zero = c.pix_center_zero;
  r.vtx_window_offset_enable = c.window_offset_enable;
  r.window_x_offset = c.wx;
  r.window_y_offset = c.wy;
  r.scissor_x = c.sx;
  r.scissor_y = c.sy;
  r.scissor_w = c.sw;
  r.scissor_h = c.sh;
  r.surface_pitch = c.surface_pitch;
  r.msaa = c.msaa;
  r.copy_src_select = c.src_select;
  r.copy_sample_select = c.sample_select;
  r.copy_command = c.copy_command;
  r.depth_clear_enable = c.depth_clear_enable;
  r.color_base = c.color_base;
  r.color_format = c.color_format;
  r.depth_base = c.depth_base;
  r.depth_format = c.depth_format;
  r.copy_dest_base = c.dest_base;
  r.copy_dest_pitch = c.dest_pitch;
  r.copy_dest_height = c.dest_height;
  r.copy_dest_array = c.dest_array;
  r.copy_dest_slice = c.dest_slice;
  r.copy_dest_format = c.dest_format;
  r.copy_surface_slice = c.surface_slice;
  return r;
}

static std::vector<uint32_t> Words(const rs::Info& o) {
  return {uint32_t(o.valid), uint32_t(o.x0), uint32_t(o.y0), uint32_t(o.x1), uint32_t(o.y1), o.width_div_8,
          o.height_div_8, uint32_t(o.sample_select), o.edram_offset_x_div_8, o.edram_offset_y_div_8,
          uint32_t(o.color.used), o.color.base_tiles, o.color.pitch_tiles, uint32_t(o.color.is_64bpp),
          uint32_t(o.depth.used), o.depth.base_tiles, o.depth.pitch_tiles, o.dest_format,
          o.dest_pitch_aligned_div_32, o.dest_height_aligned_div_32, o.dest_offset_x_div_8, o.dest_offset_y_div_8,
          o.dest_base, o.dest_extent_start, o.dest_extent_length};
}

TEST_CASE("resolve: rectangles, EDRAM source and destination match Canary's GetResolveInfo", "[renderer_core][resolve]") {
  const auto cases = gn_golden::ResolveCases();
  gn_golden::Hash h;
  for (size_t i = 0; i < cases.size(); ++i) {
    const auto w = Words(rs::GetInfo(ToRegs(cases[i])));
    REQUIRE(w.size() == 25);
    for (uint32_t v : w) h.Add(v);
    if (i < std::size(gd::kResolve)) {
      for (int k = 0; k < 25; ++k) {
        INFO("case " << i << " field " << k);
        CHECK(w[k] == gd::kResolve[i][k]);
      }
    }
  }
  CHECK(h.h == gd::kHashResolve);
}

TEST_CASE("resolve: hand-checked Gears-shaped resolves", "[renderer_core][resolve]") {
  const auto cases = gn_golden::ResolveCases();
  // 0: 1280x720 colour, 1x.
  auto o = rs::GetInfo(ToRegs(cases[0]));
  CHECK(o.valid);
  CHECK((o.x0 == 0 && o.y0 == 0 && o.x1 == 1280 && o.y1 == 720));
  CHECK((o.width_div_8 == 160 && o.height_div_8 == 90));
  CHECK((o.color.used && o.color.pitch_tiles == 16 && o.color.base_tiles == 0));
  CHECK(o.dest_extent_start == 0x1A000000u);
  CHECK(o.dest_extent_length == 3768320u);
  // 1: 4x MSAA depth at EDRAM base 720 (notes/35): pitch 32 tiles, depth only, no averaging (k0).
  o = rs::GetInfo(ToRegs(cases[1]));
  CHECK((o.is_depth && o.depth.used && !o.color.used));
  CHECK((o.depth.base_tiles == 720 && o.depth.pitch_tiles == 32));
  CHECK(o.sample_select == rs::CopySampleSelect::k0);
  CHECK(o.dest_format == rs::kTextureFormat_k_24_8_FLOAT);
  // 2: 800x800 shadow map (D24S8): pitch 10 tiles.
  o = rs::GetInfo(ToRegs(cases[2]));
  CHECK((o.width_div_8 == 100 && o.height_div_8 == 100 && o.depth.pitch_tiles == 10));
  CHECK(o.dest_format == rs::kTextureFormat_k_24_8);
  // 3: bloom 320x180 into a 352x182 16_16_FLOAT texture at 0x14550000: height rounds up to 184.
  o = rs::GetInfo(ToRegs(cases[3]));
  CHECK((o.x1 == 320 && o.y1 == 184 && o.height_div_8 == 23));
  CHECK((o.dest_pitch_aligned_div_32 == 11 && o.dest_base == 0x14550000u));
  CHECK(o.dest_extent_start == 0x14550000u);
  // 4: a rectangle starting at (24, 8) after snapping: x0 23.5+0.5 = 24 -> aligned down to 24, y0 8.
  o = rs::GetInfo(ToRegs(cases[4]));
  CHECK((o.x0 == 24 && o.y0 == 8 && o.x1 == 408 && o.y1 == 304));
  CHECK((o.color.is_64bpp && o.color.pitch_tiles == 16 && o.color.base_tiles == 100));
  CHECK((o.edram_offset_x_div_8 == 3 && o.edram_offset_y_div_8 == 1));
  // 5: 16bpp destination at base 0x10000800: macro phase 1 moves 32 columns into x, base back to 0x10000000.
  o = rs::GetInfo(ToRegs(cases[5]));
  CHECK(o.dest_base == 0x10000000u);
  CHECK(o.dest_offset_x_div_8 == 4);
  // 6: convert-to-1_1_1_1 into DXT3A_AS_1_1_1_1: supported, 4x4 blocks.
  o = rs::GetInfo(ToRegs(cases[6]));
  CHECK(o.dest_supported);
  CHECK(o.dest_bpp_log2 == 3);
  // Unsupported copy command and 8x MSAA are rejected.
  rs::Regs r = ToRegs(cases[0]);
  r.copy_command = 3;
  CHECK_FALSE(rs::GetInfo(r).valid);
  r = ToRegs(cases[0]);
  r.msaa = 3;
  CHECK_FALSE(rs::GetInfo(r).valid);
  // Fully outside the scissor: valid but empty.
  r = ToRegs(cases[0]);
  r.scissor_x = 2000;
  r.scissor_w = 10;
  o = rs::GetInfo(r);
  CHECK((o.valid && o.width_div_8 == 0 && o.height_div_8 == 0));
}

// ---------------------------------------------------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("primitives: rectangle-list strip order matches the reference geometry shader", "[renderer_core][prim]") {
  const auto v = gn_golden::RectCases();
  REQUIRE(v.size() / 6 == std::size(gd::kRectOrder));
  for (size_t i = 0; i < v.size() / 6; ++i) {
    const float* p = &v[i * 6];
    const auto s = xenos::prim::RectListStripOrder(p, p + 2, p + 4);
    INFO("case " << i);
    CHECK(s.order[0] == gd::kRectOrder[i]);
    CHECK(s.order[1] == (s.order[0] + 1) % 3);
    CHECK(s.order[2] == (s.order[0] + 2) % 3);
  }
}

TEST_CASE("primitives: the fourth rectangle vertex completes axis-aligned rectangles", "[renderer_core][prim]") {
  const float r[4][2] = {{0, 0}, {100, 0}, {0, 50}, {100, 50}};
  const int orders[][3] = {{0, 1, 2}, {1, 3, 0}, {3, 2, 1}, {2, 0, 3}, {1, 0, 3}, {0, 2, 1}};
  for (auto& o : orders) {
    const float* in[3] = {r[o[0]], r[o[1]], r[o[2]]};
    const auto s = xenos::prim::RectListStripOrder(in[0], in[1], in[2]);
    const float x = xenos::prim::RectFourth(in[s.order[0]][0], in[s.order[1]][0], in[s.order[2]][0]);
    const float y = xenos::prim::RectFourth(in[s.order[0]][1], in[s.order[1]][1], in[s.order[2]][1]);
    // The four corners are the input three plus the generated one.
    std::set<std::pair<float, float>> corners = {{in[0][0], in[0][1]}, {in[1][0], in[1][1]}, {in[2][0], in[2][1]},
                                                 {x, y}};
    CHECK(corners.size() == 4);
    CHECK(corners.count({0.f, 0.f}) + corners.count({100.f, 0.f}) + corners.count({0.f, 50.f}) +
              corners.count({100.f, 50.f}) ==
          4);
  }
}

TEST_CASE("primitives: index conversions", "[renderer_core][prim]") {
  using namespace xenos::prim;
  const uint16_t fan[] = {10, 11, 12, 13, 14};
  uint16_t out[16] = {};
  REQUIRE(TriangleFanListCount(5) == 9);
  TriangleFanToList(out, fan, 5);
  const uint16_t fan_expect[] = {11, 12, 10, 12, 13, 10, 13, 14, 10};
  for (int i = 0; i < 9; ++i) CHECK(out[i] == fan_expect[i]);
  CHECK(TriangleFanListCount(2) == 0);

  const uint32_t loop[] = {7, 8, 9};
  uint32_t lout[8] = {};
  REQUIRE(LineLoopStripCount(3) == 4);
  LineLoopToStrip(lout, loop, 3);
  CHECK((lout[0] == 7 && lout[1] == 8 && lout[2] == 9 && lout[3] == 7));
  CHECK(LineLoopStripCount(1) == 0);
  CHECK(LineLoopStripCount(2) == 3);

  const uint32_t quads[] = {0, 1, 2, 3, 4, 5, 6, 7, 99};  // trailing partial quad is dropped
  uint32_t qout[16] = {};
  REQUIRE(QuadListTriangleListCount(9) == 12);
  QuadListToTriangleList(qout, quads, 9);
  const uint32_t q_expect[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
  for (int i = 0; i < 12; ++i) CHECK(qout[i] == q_expect[i]);
}

TEST_CASE("primitives: depth bias matches the reference", "[renderer_core][prim]") {
  for (const auto& r : gd::kPoly) {
    INFO("offset bits 0x" << std::hex << r.bits << " float24 " << r.float24);
    CHECK(xenos::prim::D3D10IntegerPolygonOffset(r.float24 != 0, std::bit_cast<float>(r.bits)) == r.value);
  }
  CHECK(xenos::prim::D3D10IntegerPolygonOffset(false, 1.0f / 16777215.0f) == 1);
  CHECK(xenos::prim::D3D10IntegerPolygonOffset(true, 1.0f / 2097152.0f) == 8);  // one 20e4 ULP at [0.5,1) -> 8 float32 ULPs
  CHECK(xenos::prim::D3D10IntegerPolygonOffset(false, -2.0f / 16777215.0f) == -2);
  CHECK(xenos::prim::kPolygonOffsetScaleSubpixelUnit == 1.0f / 16.0f);
}
