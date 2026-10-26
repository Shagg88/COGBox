// Unit tests for xenos/renderer/pipeline_state.h (register -> host pipeline state). Every expected value is worked out
// by hand from Xenia Canary e6cd2e4's code; each case names the Canary function (and file:line) it follows.
// Game-free: register values are synthetic.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

#include "xenos/renderer/pipeline_state.h"

namespace xr = xenos::renderer;
namespace rg = xenos::renderer::reg;
using Catch::Approx;

namespace {
struct Regs {
  std::vector<uint32_t> v = std::vector<uint32_t>(0x5003, 0u);
  Regs() {
    // A plain, drawable baseline: colour+depth EDRAM mode, 1280-pixel pitch, scissors open, all RT0 channels written.
    v[rg::RB_MODECONTROL] = 4;                     // kColorDepth
    v[rg::RB_SURFACE_INFO] = 1280;                 // pitch 1280, 1x MSAA
    v[rg::PA_SC_WINDOW_SCISSOR_TL] = 0;
    v[rg::PA_SC_WINDOW_SCISSOR_BR] = 1280 | (720u << 16);
    v[rg::PA_SC_SCREEN_SCISSOR_TL] = 0;
    v[rg::PA_SC_SCREEN_SCISSOR_BR] = 8192 | (8192u << 16);
    v[rg::RB_COLOR_MASK] = 0xF;
    v[rg::PA_CL_CLIP_CNTL] = 1u << 19;             // dx_clip_space_def
    v[rg::RB_BLENDCONTROL0] = Blend(1, 0, 0, 1, 0, 0);  // One/Zero/Add both
    v[rg::RB_BLENDCONTROL1] = v[rg::RB_BLENDCONTROL2] = v[rg::RB_BLENDCONTROL3] = v[rg::RB_BLENDCONTROL0];
  }
  uint32_t& operator[](uint32_t i) { return v[i]; }
  void SetF(uint32_t i, float f) { v[i] = std::bit_cast<uint32_t>(f); }
  // RB_BLENDCONTROL: color_srcblend +0, color_comb_fcn +5, color_destblend +8, alpha_srcblend +16, alpha_comb_fcn +21,
  // alpha_destblend +24 (SDK registers.h).
  static uint32_t Blend(uint32_t cs, uint32_t cop, uint32_t cd, uint32_t as, uint32_t aop, uint32_t ad) {
    return cs | (cop << 5) | (cd << 8) | (as << 16) | (aop << 21) | (ad << 24);
  }
  xr::PipelineState Run(uint32_t prim = 4, bool explicit_mode = false, uint32_t msaa = 1, float scale = 1.0f) const {
    return xr::TranslatePipelineState(v.data(), xr::PipelineInputs{prim, explicit_mode, msaa, scale});
  }
};
// D3D9-style 1280x720 viewport: scale (640, -360, 1), offset (640, 360, 0), all six VTE enables + vtx_w0_fmt.
void Viewport720(Regs& r) {
  r.SetF(rg::PA_CL_VPORT_XSCALE, 640.0f);
  r.SetF(rg::PA_CL_VPORT_XOFFSET, 640.0f);
  r.SetF(rg::PA_CL_VPORT_YSCALE, -360.0f);
  r.SetF(rg::PA_CL_VPORT_YOFFSET, 360.0f);
  r.SetF(rg::PA_CL_VPORT_ZSCALE, 1.0f);
  r.SetF(rg::PA_CL_VPORT_ZOFFSET, 0.0f);
  r[rg::PA_CL_VTE_CNTL] = 0x3F | (1u << 10);
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Viewport (draw_util.cc GetHostViewportInfo, clip-enabled branch :547-695)
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("viewport: 1280x720 with window offset (8,4) and kD3DZero pixel centre", "[pipeline_state]") {
  Regs r;
  Viewport720(r);
  r[rg::PA_SC_WINDOW_OFFSET] = 8 | (4u << 16);
  r[rg::PA_SU_SC_MODE_CNTL] = 1u << 16;  // vtx_window_offset_enable
  r[rg::PA_SU_VTX_CNTL] = 0;             // pix_center = kD3DZero -> +0.5 (:466-470)
  const auto vp = r.Run().viewport;
  // offset_add = (8.5, 4.5). X: offset_axis 648.5, |scale| 640 -> axis_1 = floor(1288.5) = 1288, axis_0 = floor(8.5)
  // = 8, extent 1280; ndc_scale = 640*2/1280 = 1, ndc_offset = (648.5 - (8 + 640)) * 2/1280 = 1/1280 (:571-600).
  CHECK(vp.x == 8.0f);
  CHECK(vp.width == 1280.0f);
  CHECK(vp.ndc_scale[0] == 1.0f);
  CHECK(vp.ndc_offset[0] == Approx(1.0 / 1280.0).epsilon(1e-6));
  // Y: offset_axis 364.5, |scale| 360 -> axis_1 = 724, axis_0 = 4, extent 720; ndc_scale = -360*2/720 = -1,
  // ndc_offset = 0.5 * 2/720 = 1/720.
  CHECK(vp.y == 4.0f);
  CHECK(vp.height == 720.0f);
  CHECK(vp.ndc_scale[1] == -1.0f);
  CHECK(vp.ndc_offset[1] == Approx(1.0 / 720.0).epsilon(1e-6));
  // Z with dx_clip_space_def: host offset 0, scale 1 -> [0, 1], ndc z unchanged (:613-617, 684-685).
  CHECK(vp.z_min == 0.0f);
  CHECK(vp.z_max == 1.0f);
  CHECK(vp.ndc_scale[2] == 1.0f);
  CHECK(vp.ndc_offset[2] == 0.0f);
  // W0 format flags (d3d12_command_processor.cc UpdateSystemConstantValues_Impl :4288-4296).
  CHECK_FALSE(vp.xy_divided_by_w);
  CHECK_FALSE(vp.z_divided_by_w);
  CHECK(vp.w_not_reciprocal);
  CHECK_FALSE(vp.clip_disabled);

  // D3D12 flips Y (origin_bottom_left, :721-724).
  auto flipped = vp;
  xr::NdcForBottomLeftOrigin(flipped);
  CHECK(flipped.ndc_scale[1] == 1.0f);
  CHECK(flipped.ndc_offset[1] == -vp.ndc_offset[1]);
}

TEST_CASE("viewport: window offset ignored without vtx_window_offset_enable; kOGLHalf adds no 0.5", "[pipeline_state]") {
  Regs r;
  Viewport720(r);
  r[rg::PA_SC_WINDOW_OFFSET] = 8 | (4u << 16);
  r[rg::PA_SU_SC_MODE_CNTL] = 0;
  r[rg::PA_SU_VTX_CNTL] = 1;  // kOGLHalf
  const auto vp = r.Run().viewport;
  // offset_axis 640 exactly: axis_1 = 1280, axis_0 = 0, ndc_offset 0.
  CHECK(vp.x == 0.0f);
  CHECK(vp.y == 0.0f);
  CHECK(vp.width == 1280.0f);
  CHECK(vp.height == 720.0f);
  CHECK(vp.ndc_offset[0] == 0.0f);
  CHECK(vp.ndc_offset[1] == 0.0f);
}

TEST_CASE("viewport: resolution scale 2 multiplies the integer rectangle, not the NDC transform", "[pipeline_state]") {
  Regs r;
  Viewport720(r);
  r[rg::PA_SC_WINDOW_OFFSET] = 8 | (4u << 16);
  r[rg::PA_SU_SC_MODE_CNTL] = 1u << 16;
  const auto vp = r.Run(4, false, 1, 2.0f).viewport;
  // xy_offset = axis_0 * scale, xy_extent = extent * scale (:579-581); 1x MSAA so no sample adjustment.
  CHECK(vp.x == 16.0f);
  CHECK(vp.y == 8.0f);
  CHECK(vp.width == 2560.0f);
  CHECK(vp.height == 1440.0f);
  CHECK(vp.ndc_scale[0] == 1.0f);
  CHECK(vp.ndc_offset[0] == Approx(1.0 / 1280.0).epsilon(1e-6));
}

TEST_CASE("viewport: UE3 4x MSAA sample-0 offset is kept in host pixels at 2x scale", "[pipeline_state]") {
  // UE3 adds (-0.125, -0.125) for 4x tiling (draw_util.h:139-143). Offsets 639.875 / 359.875 + 0.5 pixel centre ->
  // fraction 0.375 - 0.5 = -0.125 = sample 0 (-2, -2)/16; offset_add -= -0.125 * (2-1)/2 -> 0.5625 (:474-503).
  Regs r;
  Viewport720(r);
  r.SetF(rg::PA_CL_VPORT_XOFFSET, 639.875f);
  r.SetF(rg::PA_CL_VPORT_YOFFSET, 359.875f);
  r[rg::RB_SURFACE_INFO] = 1280 | (2u << 16);  // 4x
  const auto vp2 = r.Run(4, false, 4, 2.0f).viewport;
  // X: offset_axis 640.4375, axis_1 = floor(1280.4375) = 1280, axis_0 = 0 -> ndc_offset = 0.4375 * 2/1280.
  CHECK(vp2.x == 0.0f);
  CHECK(vp2.width == 2560.0f);
  CHECK(vp2.ndc_offset[0] == Approx(0.875 / 1280.0).epsilon(1e-6));
  CHECK(vp2.ndc_offset[1] == Approx(0.875 / 720.0).epsilon(1e-6));
  // Native scale: no adjustment, ndc_offset = 0.375 * 2/1280.
  const auto vp1 = r.Run(4, false, 4, 1.0f).viewport;
  CHECK(vp1.ndc_offset[0] == Approx(0.75 / 1280.0).epsilon(1e-6));
  // msaa_samples = 0 takes RB_SURFACE_INFO (4x): same as passing 4.
  const auto vp0 = r.Run(4, false, 0, 2.0f).viewport;
  CHECK(vp0.ndc_offset[0] == vp2.ndc_offset[0]);
}

TEST_CASE("viewport: z remap when dx_clip_space_def is clear", "[pipeline_state]") {
  Regs r;
  Viewport720(r);
  r[rg::PA_CL_CLIP_CNTL] = 0;  // OpenGL -W...W clip space
  r.SetF(rg::PA_CL_VPORT_ZSCALE, 0.5f);
  r.SetF(rg::PA_CL_VPORT_ZOFFSET, 0.5f);
  auto vp = r.Run().viewport;
  // host_clip_offset_z = 0.5 - 0.5 = 0, host_clip_scale_z = 1; ndc z * 0.5 + 0.5 (:664-669).
  CHECK(vp.z_min == 0.0f);
  CHECK(vp.z_max == 1.0f);
  CHECK(vp.ndc_scale[2] == 0.5f);
  CHECK(vp.ndc_offset[2] == 0.5f);
  // Reversed (scale -0.5): offset 1, scale -1 -> z_min 1 > z_max 0 -> swapped, ndc z negated, offset 1 - 0.5 (:689-693).
  r.SetF(rg::PA_CL_VPORT_ZSCALE, -0.5f);
  vp = r.Run().viewport;
  CHECK(vp.z_min == 0.0f);
  CHECK(vp.z_max == 1.0f);
  CHECK(vp.ndc_scale[2] == -0.5f);
  CHECK(vp.ndc_offset[2] == 0.5f);
  // D3D space with a partial range: ZOFFSET 0.25, ZSCALE 0.5 -> [0.25, 0.75].
  r[rg::PA_CL_CLIP_CNTL] = 1u << 19;
  r.SetF(rg::PA_CL_VPORT_ZSCALE, 0.5f);
  r.SetF(rg::PA_CL_VPORT_ZOFFSET, 0.25f);
  vp = r.Run().viewport;
  CHECK(vp.z_min == 0.25f);
  CHECK(vp.z_max == 0.75f);
}

TEST_CASE("viewport: clipping disabled -> 8192 host viewport, offsets in ndc", "[pipeline_state]") {
  // Clip-disabled branch (:520-546): extent min(8192, 32767) = 8192, pixels_to_ndc = 2/8192; VTE all off.
  Regs r;
  r[rg::PA_CL_CLIP_CNTL] = 1u << 16;
  r[rg::PA_CL_VTE_CNTL] = (1u << 8) | (1u << 9);  // pre-divided XY and Z
  r[rg::PA_SU_VTX_CNTL] = 0;
  const auto vp = r.Run().viewport;
  CHECK(vp.clip_disabled);
  CHECK(vp.width == 8192.0f);
  CHECK(vp.height == 8192.0f);
  CHECK(vp.ndc_scale[0] == 1.0f / 4096.0f);
  CHECK(vp.ndc_offset[0] == (0.5f - 4096.0f) / 4096.0f);
  CHECK(vp.ndc_scale[2] == 1.0f);
  CHECK(vp.ndc_offset[2] == 0.0f);
  CHECK(vp.z_min == 0.0f);
  CHECK(vp.z_max == 1.0f);
  CHECK(vp.xy_divided_by_w);
  CHECK(vp.z_divided_by_w);
  CHECK_FALSE(vp.w_not_reciprocal);
  CHECK_FALSE(r.Run().raster.depth_clip);  // pipeline_cache.cc :1487
}

// ---------------------------------------------------------------------------------------------------------------------
// Scissor (draw_util.cc GetScissorTmpl non-SSE path :826-912, clamp_to_surface_pitch = true)
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("scissor: window offset, screen scissor and pitch clamp", "[pipeline_state]") {
  Regs r;
  r[rg::PA_SC_WINDOW_OFFSET] = 8 | (4u << 16);
  // (0,0)-(1280,720) + (8,4) = (8,4)-(1288,724); screen (0,0)-(8192,8192) keeps it; pitch 1280 clamps br_x -> 1280.
  auto s = r.Run().scissor;
  CHECK(s.x == 8);
  CHECK(s.y == 4);
  CHECK(s.width == 1272);
  CHECK(s.height == 720);
  // window_offset_disable (TL bit 31) -> no offset.
  r[rg::PA_SC_WINDOW_SCISSOR_TL] = 1u << 31;
  s = r.Run().scissor;
  CHECK(s.x == 0);
  CHECK(s.y == 0);
  CHECK(s.width == 1280);
  CHECK(s.height == 720);
  // Negative window offset (-16, -8): (-16,-8)-(1264,712) -> clamp tl to 0 -> (0,0)-(1264,712). Scale 2 doubles it.
  r[rg::PA_SC_WINDOW_SCISSOR_TL] = 0;
  r[rg::PA_SC_WINDOW_OFFSET] = (uint32_t(-16) & 0x7FFF) | ((uint32_t(-8) & 0x7FFF) << 16);
  s = r.Run(4, false, 1, 2.0f).scissor;
  CHECK(s.x == 0);
  CHECK(s.y == 0);
  CHECK(s.width == 2528);
  CHECK(s.height == 1424);
  // Screen scissor tighter than the window scissor: intersection.
  r[rg::PA_SC_WINDOW_OFFSET] = 0;
  r[rg::PA_SC_SCREEN_SCISSOR_TL] = 100 | (50u << 16);
  r[rg::PA_SC_SCREEN_SCISSOR_BR] = 600 | (400u << 16);
  s = r.Run().scissor;
  CHECK(s.x == 100);
  CHECK(s.y == 50);
  CHECK(s.width == 500);
  CHECK(s.height == 350);
  // Inverted (TL > BR) collapses to an empty rectangle at TL (:897-900).
  r[rg::PA_SC_SCREEN_SCISSOR_TL] = 0;
  r[rg::PA_SC_SCREEN_SCISSOR_BR] = 8192 | (8192u << 16);
  r[rg::PA_SC_WINDOW_SCISSOR_TL] = 700 | (500u << 16);
  r[rg::PA_SC_WINDOW_SCISSOR_BR] = 600 | (400u << 16);
  s = r.Run().scissor;
  CHECK(s.x == 700);
  CHECK(s.y == 500);
  CHECK(s.width == 0);
  CHECK(s.height == 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Depth / stencil (draw_util.cc GetNormalizedDepthControl :96-125, pipeline_cache.cc :1489-1553,
// d3d12_command_processor.cc stencil ref :4170-4183)
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("depth: LessEqual + write; Always without write normalises to off", "[pipeline_state]") {
  Regs r;
  r[rg::RB_DEPTHCONTROL] = (1u << 1) | (1u << 2) | (3u << 4);  // z_enable, z_write, LessEqual
  auto ds = r.Run().depth_stencil;
  CHECK(ds.depth_enable);
  CHECK(ds.depth_write);
  CHECK(ds.depth_func == xr::CompareFunc::LessEqual);
  CHECK_FALSE(ds.stencil_enable);
  r[rg::RB_DEPTHCONTROL] = (1u << 1) | (7u << 4);  // z_enable, no write, Always -> z_enable cleared (:118-121)
  ds = r.Run().depth_stencil;
  CHECK_FALSE(ds.depth_enable);
  CHECK(ds.depth_func == xr::CompareFunc::Always);
  // Always + write stays enabled on the host (CreateD3D12Pipeline :3158-3160).
  r[rg::RB_DEPTHCONTROL] = (1u << 1) | (1u << 2) | (7u << 4);
  ds = r.Run().depth_stencil;
  CHECK(ds.depth_enable);
  CHECK(ds.depth_write);
  // EDRAM mode other than color+depth / depth-only disables depth and stencil entirely (:97-104).
  r[rg::RB_MODECONTROL] = 0;
  ds = r.Run().depth_stencil;
  CHECK_FALSE(ds.depth_enable);
  CHECK_FALSE(ds.depth_write);
}

TEST_CASE("stencil: two-sided z-fail shadow volume", "[pipeline_state]") {
  Regs r;
  // stencil_enable, z_enable, LessEqual, backface_enable; front: func Always, fail Keep, zpass Keep, zfail IncrWrap;
  // back: func Always, fail Keep, zpass Keep, zfail DecrWrap (RB_DEPTHCONTROL fields, SDK registers.h).
  r[rg::RB_DEPTHCONTROL] = 1u | (1u << 1) | (3u << 4) | (1u << 7) | (7u << 8) | (0u << 11) | (0u << 14) | (6u << 17) |
                           (7u << 20) | (0u << 23) | (0u << 26) | (7u << 29);
  r[rg::RB_STENCILREFMASK] = 0x80 | (0xFFu << 8) | (0x7Fu << 16);
  r[rg::RB_STENCILREFMASK_BF] = 0x01 | (0x0Fu << 8) | (0xF0u << 16);
  auto ds = r.Run().depth_stencil;
  CHECK(ds.stencil_enable);
  CHECK(ds.depth_enable);
  CHECK_FALSE(ds.depth_write);
  CHECK(ds.front.func == xr::CompareFunc::Always);
  CHECK(ds.front.depth_fail_op == xr::StencilOp::IncrementWrap);
  CHECK(ds.front.fail_op == xr::StencilOp::Keep);
  CHECK(ds.front.pass_op == xr::StencilOp::Keep);
  CHECK(ds.back.depth_fail_op == xr::StencilOp::DecrementWrap);
  CHECK(ds.front.ref == 0x80);
  CHECK(ds.front.read_mask == 0xFF);
  CHECK(ds.front.write_mask == 0x7F);
  CHECK(ds.back.ref == 0x01);
  CHECK(ds.back.read_mask == 0x0F);
  CHECK(ds.back.write_mask == 0xF0);
  // No culling -> single-set (D3D12) uses the front register (:1506-1511).
  CHECK(ds.single_ref == 0x80);
  CHECK(ds.single_read_mask == 0xFF);
  // Front culled -> back register.
  r[rg::PA_SU_SC_MODE_CNTL] = 1u;  // cull_front
  ds = r.Run().depth_stencil;
  CHECK(ds.single_ref == 0x01);
  CHECK(ds.single_write_mask == 0xF0);
  // Point list is not polygonal: no two-sided stencil, back = front (:1502-1503, 1533-1541).
  r[rg::PA_SU_SC_MODE_CNTL] = 0;
  ds = r.Run(1).depth_stencil;
  CHECK(ds.back.depth_fail_op == xr::StencilOp::IncrementWrap);
  CHECK(ds.back.ref == 0x80);
}

// ---------------------------------------------------------------------------------------------------------------------
// Rasteriser (pipeline_cache.cc :1422-1474, draw_util.cc GetPreferredFacePolygonOffset :217-246,
// draw_util.h GetD3D10IntegerPolygonOffset :256-275)
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("raster: cull, face, wireframe", "[pipeline_state]") {
  Regs r;
  r[rg::PA_SU_SC_MODE_CNTL] = 2u | (1u << 2);  // cull_back, face = 1 (front CW)
  auto rs = r.Run().raster;
  CHECK(rs.cull_mode == xr::CullMode::Back);
  CHECK_FALSE(rs.front_counter_clockwise);
  CHECK(rs.fill_mode == xr::FillMode::Solid);
  // poly_mode dual, front ptype lines, back ptype triangles, back culled -> wireframe from the front.
  r[rg::PA_SU_SC_MODE_CNTL] = 2u | (1u << 3) | (1u << 5) | (2u << 8);
  CHECK(r.Run().raster.fill_mode == xr::FillMode::Wireframe);
  // Same but poly_mode 2 (reserved, 4541096E) -> solid (:1454-1456).
  r[rg::PA_SU_SC_MODE_CNTL] = 2u | (2u << 3) | (1u << 5) | (2u << 8);
  CHECK(r.Run().raster.fill_mode == xr::FillMode::Solid);
  // Rectangle lists are not polygonal: no cull even with cull_back set (draw_util.h :67-71).
  r[rg::PA_SU_SC_MODE_CNTL] = 2u;
  rs = r.Run(8).raster;
  CHECK(rs.cull_mode == xr::CullMode::None);
  CHECK_FALSE(rs.primitive_polygonal);
}

TEST_CASE("raster: depth bias for D24S8 and D24FS8", "[pipeline_state]") {
  Regs r;
  r[rg::PA_SU_SC_MODE_CNTL] = (1u << 11) | (1u << 12);  // front + back offset enable
  r.SetF(rg::PA_SU_POLY_OFFSET_FRONT_SCALE, 4.0f);
  r.SetF(rg::PA_SU_POLY_OFFSET_FRONT_OFFSET, 1e-5f);
  r.SetF(rg::PA_SU_POLY_OFFSET_BACK_SCALE, 8.0f);
  r.SetF(rg::PA_SU_POLY_OFFSET_BACK_OFFSET, 3e-5f);
  // D24S8: ceil(1e-5 * (2^24 - 1)) = ceil(167.77) = 168; slope 4/16 = 0.25.
  auto rs = r.Run().raster;
  CHECK(rs.depth_bias == 168);
  CHECK(rs.depth_bias_slope_scaled == 0.25f);
  // D24FS8: ceil(1e-5 * 2^21) = ceil(20.97) = 21, << 3 = 168.
  r[rg::RB_DEPTH_INFO] = 1u << 16;
  rs = r.Run().raster;
  CHECK(rs.depth_bias == 168);
  // Negative offset: -2e-5 -> D24FS8 ceil(41.94) = 42 << 3 = -336; D24S8 ceil(335.54) = -336 as well.
  r.SetF(rg::PA_SU_POLY_OFFSET_FRONT_OFFSET, -2e-5f);
  CHECK(r.Run().raster.depth_bias == -336);
  r[rg::RB_DEPTH_INFO] = 0;
  CHECK(r.Run().raster.depth_bias == -336);
  // 1e-6: D24S8 ceil(16.78) = 17; D24FS8 ceil(2.097) = 3 << 3 = 24 (different).
  r.SetF(rg::PA_SU_POLY_OFFSET_FRONT_OFFSET, 1e-6f);
  CHECK(r.Run().raster.depth_bias == 17);
  r[rg::RB_DEPTH_INFO] = 1u << 16;
  CHECK(r.Run().raster.depth_bias == 24);
  // Resolution scale multiplies the slope only (CreateD3D12Pipeline :3116-3121): 4/16 * 2 = 0.5.
  rs = r.Run(4, false, 1, 2.0f).raster;
  CHECK(rs.depth_bias_slope_scaled == 0.5f);
  CHECK(rs.depth_bias == 24);
  // Front culled -> back values (scale 8 -> 0.5, offset 3e-5 float24: ceil(62.91) = 63 << 3 = 504).
  r[rg::PA_SU_SC_MODE_CNTL] |= 1u;
  rs = r.Run().raster;
  CHECK(rs.depth_bias_slope_scaled == 0.5f);
  CHECK(rs.depth_bias == 504);
  // Not ported: Canary's roundToNearestOrderOfMagnitude(front scale) would turn 3.7 into 4; we keep 3.7/16.
  r[rg::PA_SU_SC_MODE_CNTL] = 1u << 11;
  r.SetF(rg::PA_SU_POLY_OFFSET_FRONT_SCALE, 3.7f);
  CHECK(r.Run().raster.depth_bias_slope_scaled == 3.7f * (1.0f / 16.0f));
  // Non-polygonal primitives use the front values only with poly_offset_para_enable (:236-243).
  CHECK(r.Run(1).raster.depth_bias == 0);
  r[rg::PA_SU_SC_MODE_CNTL] = 1u << 13;
  CHECK(r.Run(1).raster.depth_bias == 24);
}

// ---------------------------------------------------------------------------------------------------------------------
// Blend (pipeline_cache.cc kBlendFactorMap/kBlendFactorAlphaMap :1557-1647, CreateD3D12Pipeline :3253-3277,
// draw_util.cc GetNormalizedColorMask :1039-1081)
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("blend: default off, alpha blend, additive, min, alpha-map substitution", "[pipeline_state]") {
  Regs r;
  r[rg::RB_COLOR_MASK] = 0xFFFF;
  // RT0 One/Zero/Add -> blend off.
  auto b = r.Run().blend;
  CHECK_FALSE(b.rt[0].blend_enable);
  CHECK(b.rt[0].write_mask == 0xF);
  // RT1: SrcAlpha(6) / InvSrcAlpha(7) Add, alpha One/Zero Add -> enabled.
  r[rg::RB_BLENDCONTROL1] = Regs::Blend(6, 0, 7, 1, 0, 0);
  // RT2: additive One/One.
  r[rg::RB_BLENDCONTROL2] = Regs::Blend(1, 0, 1, 1, 0, 1);
  // RT3: colour Min with SrcAlpha/One -> host factors One/One; alpha Add SrcColor(4)/ConstantAlpha(14) -> alpha map
  // gives SrcAlpha / BlendFactor.
  r[rg::RB_BLENDCONTROL3] = Regs::Blend(6, 2, 1, 4, 0, 14);
  b = r.Run().blend;
  CHECK(b.rt[1].blend_enable);
  CHECK(b.rt[1].src_color == xr::BlendFactor::SrcAlpha);
  CHECK(b.rt[1].dst_color == xr::BlendFactor::InvSrcAlpha);
  CHECK(b.rt[1].color_op == xr::BlendOp::Add);
  CHECK(b.rt[1].src_alpha == xr::BlendFactor::One);
  CHECK(b.rt[1].dst_alpha == xr::BlendFactor::Zero);
  CHECK(b.rt[2].blend_enable);
  CHECK(b.rt[2].dst_color == xr::BlendFactor::One);
  CHECK(b.rt[2].dst_alpha == xr::BlendFactor::One);
  CHECK(b.rt[3].blend_enable);
  CHECK(b.rt[3].color_op == xr::BlendOp::Min);
  CHECK(b.rt[3].src_color == xr::BlendFactor::One);
  CHECK(b.rt[3].dst_color == xr::BlendFactor::One);
  CHECK(b.rt[3].alpha_op == xr::BlendOp::Add);
  CHECK(b.rt[3].src_alpha == xr::BlendFactor::SrcAlpha);   // per-op normalisation (Canary would force One here)
  CHECK(b.rt[3].dst_alpha == xr::BlendFactor::BlendFactor);
  // Colour ConstantAlpha(14) stays AlphaFactor for colour; undefined factor 2 -> Zero.
  r[rg::RB_BLENDCONTROL1] = Regs::Blend(14, 0, 2, 1, 0, 0);
  b = r.Run().blend;
  CHECK(b.rt[1].src_color == xr::BlendFactor::AlphaFactor);
  CHECK(b.rt[1].dst_color == xr::BlendFactor::Zero);
  // Undefined blend op 5 -> Add (kBlendOpMap :3232-3235).
  r[rg::RB_BLENDCONTROL1] = Regs::Blend(6, 5, 7, 1, 0, 0);
  CHECK(r.Run().blend.rt[1].color_op == xr::BlendOp::Add);
}

TEST_CASE("blend: write masks, constant colour", "[pipeline_state]") {
  Regs r;
  // RT0 k_16_16_FLOAT (6, 2 components), mask R only -> 0b0001 | 0b1100 = 0xD. RT1 k_32_FLOAT (14) mask G only (not a
  // component) -> 0, blend forced off even if programmed.
  r[rg::RB_COLOR_INFO] = 6u << 16;
  r[rg::RB_COLOR1_INFO] = 14u << 16;
  r[rg::RB_COLOR_MASK] = 0x1 | (0x2u << 4);
  r[rg::RB_BLENDCONTROL1] = Regs::Blend(6, 0, 7, 1, 0, 0);
  r.SetF(rg::RB_BLEND_RED, 0.25f);
  r.SetF(rg::RB_BLEND_GREEN, 0.5f);
  r.SetF(rg::RB_BLEND_BLUE, 0.75f);
  r.SetF(rg::RB_BLEND_ALPHA, 1.0f);
  auto b = r.Run().blend;
  CHECK(b.rt[0].write_mask == 0xD);
  CHECK(b.rt[0].color_format == 6);
  CHECK(b.rt[1].write_mask == 0);
  CHECK_FALSE(b.rt[1].blend_enable);
  CHECK(b.rt[1].src_color == xr::BlendFactor::One);
  CHECK(b.constant[0] == 0.25f);
  CHECK(b.constant[1] == 0.5f);
  CHECK(b.constant[2] == 0.75f);
  CHECK(b.constant[3] == 1.0f);
  // Depth-only EDRAM mode -> no colour writes at all (:1041-1045).
  r[rg::RB_MODECONTROL] = 5;
  CHECK(r.Run().blend.rt[0].write_mask == 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Alpha test (d3d12_command_processor.cc UpdateSystemConstantValues_Impl :4310-4314, :4548-4549)
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("alpha test: function and reference", "[pipeline_state]") {
  Regs r;
  r[rg::RB_COLORCONTROL] = 4u | (1u << 3);  // Greater, enabled
  r.SetF(rg::RB_ALPHA_REF, 0.5f);
  auto a = r.Run().alpha_test;
  CHECK(a.func == xr::CompareFunc::Greater);
  CHECK(a.ref == 0.5f);
  CHECK_FALSE(a.alpha_to_mask_enable);
  r[rg::RB_COLORCONTROL] = 4u | (1u << 4);  // func set but test disabled -> Always; alpha-to-mask on
  a = r.Run().alpha_test;
  CHECK(a.func == xr::CompareFunc::Always);
  CHECK(a.alpha_to_mask_enable);
}

// ---------------------------------------------------------------------------------------------------------------------
// Topology and the path_select trap (primitive_processor.cc Process :286-370)
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("topology: path_select left on does not make ordinary draws patches", "[pipeline_state]") {
  Regs r;
  r[rg::VGT_OUTPUT_PATH_CNTL] = 1;  // kTessellationEnable, as Gears 2 leaves it
  CHECK(r.Run(0x04, true).topology == xr::Topology::TriangleList);
  CHECK(r.Run(0x04, false).topology == xr::Topology::TriangleList);
  CHECK(r.Run(0x06, true).topology == xr::Topology::TriangleStrip);
  CHECK(r.Run(0x05, false).topology == xr::Topology::TriangleList);  // fan expanded
  CHECK(r.Run(0x08, false).topology == xr::Topology::RectListAsTriangles);
  CHECK(r.Run(0x0D, false).topology == xr::Topology::QuadListAsTriangles);
  CHECK(r.Run(0x01, false).topology == xr::Topology::PointList);
  // Patch types only in explicit major mode with tessellation selected.
  CHECK(r.Run(0x11, true).topology == xr::Topology::Patch);
  CHECK(r.Run(0x12, true).topology == xr::Topology::Patch);
  CHECK(r.Run(0x10, true).topology == xr::Topology::Patch);
  CHECK(r.Run(0x11, false).topology == xr::Topology::Unsupported);
  r[rg::VGT_OUTPUT_PATH_CNTL] = 0;
  CHECK(r.Run(0x11, true).topology == xr::Topology::Unsupported);
  // Lines, polygon, quad strip: Unsupported (no host enum yet / Canary drops them).
  CHECK(r.Run(0x02).topology == xr::Topology::Unsupported);
  CHECK(r.Run(0x0F).topology == xr::Topology::Unsupported);
  CHECK(r.Run(0x0E).topology == xr::Topology::Unsupported);
}

TEST_CASE("topology: polygonal follows our patch rule, not path_select", "[pipeline_state]") {
  Regs r;
  r[rg::VGT_OUTPUT_PATH_CNTL] = 1;
  r[rg::PA_SU_SC_MODE_CNTL] = 2u;  // cull_back
  // Triangle patch, implicit major mode: Canary's IsPrimitivePolygonal (path_select only) says polygonal; ours is
  // Unsupported and not polygonal.
  auto ps = r.Run(0x11, false);
  CHECK_FALSE(ps.raster.primitive_polygonal);
  CHECK(ps.skip_reason == xr::SkipReason::UnsupportedPrimitive);
  ps = r.Run(0x11, true);
  CHECK(ps.raster.primitive_polygonal);
  CHECK(ps.raster.cull_mode == xr::CullMode::Back);
  CHECK_FALSE(r.Run(0x10, true).raster.primitive_polygonal);  // line patch
}

// ---------------------------------------------------------------------------------------------------------------------
// Skip rules (d3d12_command_processor.cc IssueDraw :2635-2686, draw_util.cc IsRasterizationPotentiallyDone :57-88)
// ---------------------------------------------------------------------------------------------------------------------
TEST_CASE("skip: Canary's rules", "[pipeline_state]") {
  Regs r;
  auto ps = r.Run();
  CHECK_FALSE(ps.skip);
  CHECK(ps.skip_reason == xr::SkipReason::None);

  r[rg::RB_MODECONTROL] = 6;  // kCopy
  ps = r.Run();
  CHECK(ps.skip);
  CHECK(ps.skip_reason == xr::SkipReason::EdramCopy);
  r[rg::RB_MODECONTROL] = 0;  // kNoOperation
  ps = r.Run();
  CHECK(ps.skip_reason == xr::SkipReason::EdramModeNoRasterization);
  CHECK(xr::SkipUnlessVertexMemexport(ps.skip_reason));
  r[rg::RB_MODECONTROL] = 5;  // depth only: drawable
  CHECK_FALSE(r.Run().skip);

  r[rg::RB_SURFACE_INFO] = 0;
  ps = r.Run();
  CHECK(ps.skip_reason == xr::SkipReason::SurfacePitchZero);
  CHECK_FALSE(xr::SkipUnlessVertexMemexport(ps.skip_reason));
  r[rg::RB_SURFACE_INFO] = 1280;

  r[rg::SQ_PROGRAM_CNTL] = 7u << 24;  // vs_export_mode multipass
  CHECK(r.Run().skip_reason == xr::SkipReason::VertexShaderMultipass);
  r[rg::SQ_PROGRAM_CNTL] = 0;

  r[rg::PA_SC_VIZ_QUERY] = 1u << 7;  // kill_pix_post_hi_z
  CHECK(r.Run().skip_reason == xr::SkipReason::KillPixPostHiZ);
  r[rg::PA_SC_VIZ_QUERY] = 1u;  // query enabled alone: drawable
  CHECK_FALSE(r.Run().skip);
  r[rg::PA_SC_VIZ_QUERY] = 0;

  r[rg::PA_SU_SC_MODE_CNTL] = 3u;  // both faces culled
  ps = r.Run(0x04);
  CHECK(ps.skip_reason == xr::SkipReason::BothFacesCulled);
  CHECK(std::string_view(ps.skip_reason_text) == "both faces culled");
  CHECK_FALSE(r.Run(0x01).skip);  // points have no faces
  CHECK_FALSE(r.Run(0x08).skip);  // nor do rect lists
  r[rg::PA_SU_SC_MODE_CNTL] = 0;

  CHECK(r.Run(0x0F).skip_reason == xr::SkipReason::UnsupportedPrimitive);
}

// Stage 7 (notes/53): host <-> guest pixel rectangles for the resolution scale (render_target_cache.h helpers).
#include "xenos/renderer/render_target_cache.h"
TEST_CASE("resolution scale rectangles (Stage 7)", "[pipeline_state]") {
  using xr::PixelRect;
  // Scale 1 is the identity both ways.
  constexpr PixelRect a{3, 5, 17, 19};
  STATIC_REQUIRE(xr::HostToGuestRect(a, 1).x1 == 17);
  STATIC_REQUIRE(xr::GuestToHostRect(a, 1).y0 == 5);
  // Host -> guest rounds outwards.
  constexpr PixelRect g = xr::HostToGuestRect({3, 4, 17, 18}, 2);
  STATIC_REQUIRE(g.x0 == 1);
  STATIC_REQUIRE(g.y0 == 2);
  STATIC_REQUIRE(g.x1 == 9);
  STATIC_REQUIRE(g.y1 == 9);
  constexpr PixelRect g3 = xr::HostToGuestRect({0, 0, 3840, 2160}, 3);
  STATIC_REQUIRE(g3.x1 == 1280);
  STATIC_REQUIRE(g3.y1 == 720);
  // The Canary rule's unbounded x1 (0xFFFFFFFF) must not wrap.
  constexpr PixelRect u = xr::HostToGuestRect({0, 0, 0xFFFFFFFFu, 720}, 2);
  STATIC_REQUIRE(u.x1 == 0x80000000u);
  constexpr PixelRect h = xr::GuestToHostRect({0, 0, 0xFFFFFFFFu, 720}, 3);
  STATIC_REQUIRE(h.x1 == 0xFFFFFFFFu);
  STATIC_REQUIRE(h.y1 == 2160);
  // Round trip of a guest rectangle.
  constexpr PixelRect r = xr::HostToGuestRect(xr::GuestToHostRect({8, 16, 640, 360}, 4), 4);
  STATIC_REQUIRE(r.x0 == 8);
  STATIC_REQUIRE(r.y1 == 360);
  // Guest size caps keep the host size within 16384.
  STATIC_REQUIRE(xr::GuestDimensionCap(1) == 8192);
  STATIC_REQUIRE(xr::GuestDimensionCap(2) == 8192);
  STATIC_REQUIRE(xr::GuestDimensionCap(3) * 3 <= 16384);
  STATIC_REQUIRE(xr::GuestDimensionCap(4) == 4096);
}
