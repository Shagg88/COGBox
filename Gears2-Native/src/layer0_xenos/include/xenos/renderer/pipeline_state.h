// Layer 0 (game-free): Xenos GPU registers -> API-independent host pipeline state (depth/stencil, rasteriser, viewport,
// scissor, blending, alpha test, topology after our primitive expansion, and whether the draw should be issued at all).
// Plain CPU code, standard library only; the D3D12 backend (and later the Vulkan one) maps these enums 1:1.
//
// Our code is MIT. Behaviour follows Xenia Canary e6cd2e4 (BSD-3-Clause, Copyright (c) Ben Vanik and Xenia project
// contributors), re-expressed here from:
//   src/xenia/gpu/draw_util.cc  IsRasterizationPotentiallyDone, GetNormalizedDepthControl, GetPreferredFacePolygonOffset,
//                               GetHostViewportInfo, GetScissorTmpl, GetNormalizedColorMask
//   src/xenia/gpu/draw_util.h   IsPrimitivePolygonal, GetD3D10IntegerPolygonOffset, kXenosSamplePositions2x/4x
//   src/xenia/gpu/d3d12/pipeline_cache.cc  GetCurrentStateDescription (cull/fill/bias/depth/stencil/blend tables),
//                               CreateD3D12Pipeline (blend enable inference, MIN/MAX factors)
//   src/xenia/gpu/d3d12/d3d12_command_processor.cc  IssueDraw (skip rules), UpdateFixedFunctionState (blend constant,
//                               stencil reference), UpdateSystemConstantValues_Impl (W0 format flags, alpha test)
// Register numbers: rexglue SDK include/rex/graphics/register_table.inc; bitfields: SDK include/rex/graphics/registers.h
// (identical to Canary's for every field used here).
//
// Deliberately NOT ported (Canary heuristics / cvars; see README): roundToNearestOrderOfMagnitude on the front polygon
// offset scale, depth_bias_shader_offset, force_depth_clamp, window_offset_in_edram (window offset always stays in the
// viewport here), occlusion_query_viz / VIZ survey normalisation, half_pixel_offset=false, the MIN/MAX RT0 shader
// pre-multiply, the float24 depth-range remaps (full_float24_in_0_to_1, convert_z_to_float24), the constant-alpha
// replication for hosts without ALPHA_FACTOR, Canary's tessellation of non-patch primitives.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include "xenos/primitives.h"  // xenos::prim::D3D10IntegerPolygonOffset, kPolygonOffsetScaleSubpixelUnit

namespace xenos::renderer {

// ---------------------------------------------------------------------------------------------------------------------
// Register numbers (rexglue SDK include/rex/graphics/register_table.inc, line numbers of that file in brackets).
// ---------------------------------------------------------------------------------------------------------------------
namespace reg {
constexpr uint32_t RB_SURFACE_INFO = 0x2000;                 // [486]
constexpr uint32_t RB_COLOR_INFO = 0x2001;                   // [487]
constexpr uint32_t RB_DEPTH_INFO = 0x2002;                   // [488]
constexpr uint32_t RB_COLOR1_INFO = 0x2003;                  // [489]
constexpr uint32_t RB_COLOR2_INFO = 0x2004;                  // [490]
constexpr uint32_t RB_COLOR3_INFO = 0x2005;                  // [491]
constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0x200E;         // [500]
constexpr uint32_t PA_SC_SCREEN_SCISSOR_BR = 0x200F;         // [501]
constexpr uint32_t PA_SC_WINDOW_OFFSET = 0x2080;             // [503]
constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL = 0x2081;         // [504]
constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR = 0x2082;         // [505]
constexpr uint32_t RB_COLOR_MASK = 0x2104;                   // [511]
constexpr uint32_t RB_BLEND_RED = 0x2105;                    // [512] float
constexpr uint32_t RB_BLEND_GREEN = 0x2106;                  // [513] float
constexpr uint32_t RB_BLEND_BLUE = 0x2107;                   // [514] float
constexpr uint32_t RB_BLEND_ALPHA = 0x2108;                  // [515] float
constexpr uint32_t RB_STENCILREFMASK_BF = 0x210C;            // [519]
constexpr uint32_t RB_STENCILREFMASK = 0x210D;               // [520]
constexpr uint32_t RB_ALPHA_REF = 0x210E;                    // [521] float
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F;              // [522] float
constexpr uint32_t PA_CL_VPORT_XOFFSET = 0x2110;             // [523] float
constexpr uint32_t PA_CL_VPORT_YSCALE = 0x2111;              // [524] float
constexpr uint32_t PA_CL_VPORT_YOFFSET = 0x2112;             // [525] float
constexpr uint32_t PA_CL_VPORT_ZSCALE = 0x2113;              // [526] float
constexpr uint32_t PA_CL_VPORT_ZOFFSET = 0x2114;             // [527] float
constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180;                 // [529]
constexpr uint32_t RB_DEPTHCONTROL = 0x2200;                 // [542]
constexpr uint32_t RB_BLENDCONTROL0 = 0x2201;                // [543]
constexpr uint32_t RB_COLORCONTROL = 0x2202;                 // [544]
constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204;                 // [546]
constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;              // [547]
constexpr uint32_t PA_CL_VTE_CNTL = 0x2206;                  // [548]
constexpr uint32_t RB_MODECONTROL = 0x2208;                  // [550]
constexpr uint32_t RB_BLENDCONTROL1 = 0x2209;                // [551]
constexpr uint32_t RB_BLENDCONTROL2 = 0x220A;                // [552]
constexpr uint32_t RB_BLENDCONTROL3 = 0x220B;                // [553]
constexpr uint32_t VGT_OUTPUT_PATH_CNTL = 0x2284;            // [559]
constexpr uint32_t PA_SC_VIZ_QUERY = 0x2293;                 // [574]
constexpr uint32_t PA_SU_VTX_CNTL = 0x2302;                  // [579]
constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380;   // [621] float
constexpr uint32_t PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2381;  // [622] float
constexpr uint32_t PA_SU_POLY_OFFSET_BACK_SCALE = 0x2382;    // [623] float
constexpr uint32_t PA_SU_POLY_OFFSET_BACK_OFFSET = 0x2383;   // [624] float
// Highest register index read here; `regs` must hold at least this many + 1 entries (the SDK's RegisterFile has
// 0x5003).
constexpr uint32_t kHighestUsed = PA_SU_POLY_OFFSET_BACK_OFFSET;
// RB_COLOR_INFO::rt_register_indices / RB_BLENDCONTROL::rt_register_indices (registers.cc).
constexpr uint32_t kColorInfo[4] = {RB_COLOR_INFO, RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO};
constexpr uint32_t kBlendControl[4] = {RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3};
}  // namespace reg

// ---------------------------------------------------------------------------------------------------------------------
// Host-facing enums. Numeric values chosen so the D3D12 mapping is "+1" where Canary does that.
// ---------------------------------------------------------------------------------------------------------------------
// Same values as xenos::CompareFunction (bit 0 less, bit 1 equal, bit 2 greater). D3D12_COMPARISON_FUNC = value + 1,
// VkCompareOp = value.
enum class CompareFunc : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
// Same values as xenos::StencilOp. D3D12_STENCIL_OP = value + 1. VkStencilOp differs (IncrClamp=3, DecrClamp=4,
// Invert=5, IncrWrap=6, DecrWrap=7 - identical too).
enum class StencilOp : uint8_t { Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap };
enum class CullMode : uint8_t { None, Front, Back };
enum class FillMode : uint8_t { Solid, Wireframe };
// Canary's PipelineBlendFactor (d3d12/pipeline_cache.h), in its order. D3D12: AlphaFactor/InvAlphaFactor need
// D3D12_BLEND_ALPHA_FACTOR support, else Canary uses BLEND_FACTOR/INV_BLEND_FACTOR.
enum class BlendFactor : uint8_t {
  Zero,
  One,
  SrcColor,
  InvSrcColor,
  SrcAlpha,
  InvSrcAlpha,
  DestColor,
  InvDestColor,
  DestAlpha,
  InvDestAlpha,
  BlendFactor,     // constant colour (RB_BLEND_RED..ALPHA)
  InvBlendFactor,
  SrcAlphaSat,
  AlphaFactor,     // constant alpha (RB_BLEND_ALPHA) - only ever produced for colour factors
  InvAlphaFactor,
};
// Same values as xenos::BlendOp. 5-7 are undefined on the guest; Canary maps them to Add (kBlendOpMap), so do we.
enum class BlendOp : uint8_t { Add, Subtract, Min, Max, RevSubtract };

// Host topology after our expansion (xenos/primitives.h): fans are converted to lists, rectangle lists and quad lists
// are expanded to triangles. Lines (0x02 line list, 0x03 strip, 0x0C loop, 0x15 2D strip) have no entry here yet and
// come out Unsupported, as do 0x07 tri-with-W-flags, 0x0E quad strip, 0x0F polygon (Canary drops those too).
enum class Topology : uint8_t {
  PointList,
  TriangleList,
  TriangleStrip,
  RectListAsTriangles,
  QuadListAsTriangles,
  Patch,
  Unsupported,
};

// Only rules Canary actually has (d3d12_command_processor.cc IssueDraw + draw_util.cc IsRasterizationPotentiallyDone).
enum class SkipReason : uint8_t {
  None,
  // RB_MODECONTROL.edram_mode == kCopy: not a draw, it is a resolve (Canary calls IssueCopy). Handle it as a resolve.
  EdramCopy,
  // RB_SURFACE_INFO.surface_pitch == 0: "Doesn't actually draw" (Canary skips even memexport).
  SurfacePitchZero,
  // The following four are !IsRasterizationPotentiallyDone. Canary still issues the draw (rasterisation off) if the
  // VERTEX shader memexports; otherwise "This draw has no effect". The caller checks memexport.
  EdramModeNoRasterization,  // edram_mode is not kColorDepth (4) or kDepthOnly (5), e.g. kNoOperation
  VertexShaderMultipass,     // SQ_PROGRAM_CNTL.vs_export_mode == kMultipass (7)
  KillPixPostHiZ,            // PA_SC_VIZ_QUERY.kill_pix_post_hi_z (with occlusion_query_viz off, Canary's default)
  BothFacesCulled,           // polygonal primitive with cull_front && cull_back
  // PrimitiveProcessor::Process fails (Canary returns false; nothing drawn).
  UnsupportedPrimitive,
};
constexpr const char* SkipReasonName(SkipReason r) {
  switch (r) {
    case SkipReason::None: return "none";
    case SkipReason::EdramCopy: return "edram_mode is copy (resolve, not a draw)";
    case SkipReason::SurfacePitchZero: return "RB_SURFACE_INFO.surface_pitch is 0";
    case SkipReason::EdramModeNoRasterization: return "edram_mode is neither color+depth nor depth-only";
    case SkipReason::VertexShaderMultipass: return "vertex shader export mode is multipass";
    case SkipReason::KillPixPostHiZ: return "PA_SC_VIZ_QUERY.kill_pix_post_hi_z";
    case SkipReason::BothFacesCulled: return "both faces culled";
    case SkipReason::UnsupportedPrimitive: return "unsupported primitive type";
  }
  return "?";
}
// True for the reasons where Canary still issues the draw with rasterisation disabled when the vertex shader
// memexports.
constexpr bool SkipUnlessVertexMemexport(SkipReason r) {
  return r == SkipReason::EdramModeNoRasterization || r == SkipReason::VertexShaderMultipass ||
         r == SkipReason::KillPixPostHiZ || r == SkipReason::BothFacesCulled;
}

// ---------------------------------------------------------------------------------------------------------------------
// Inputs and outputs
// ---------------------------------------------------------------------------------------------------------------------
struct PipelineInputs {
  uint32_t primitive;        // VGT_DRAW_INITIATOR.prim_type (xenos::PrimitiveType value)
  bool major_mode_explicit;  // VGT_DRAW_INITIATOR.major_mode == kExplicit (as the draw observer reports it)
  uint32_t msaa_samples;     // host sample count the draw renders with (1, 2 or 4); 0 = take RB_SURFACE_INFO's
  float resolution_scale;    // host pixels per guest pixel on each axis (1 = native); <= 0 or NaN treated as 1
  // Patch 0089: false = Canary's host-render-target default (xenos_sample_positions off): no Xenos sample-offset
  // correction of the viewport at resolution scale > 1. true = the host rasterizes (or is assumed to) at the Xenos
  // sample positions, as before.
  bool xenos_sample_positions = true;
};

struct StencilFace {
  StencilOp fail_op = StencilOp::Keep;
  StencilOp depth_fail_op = StencilOp::Keep;
  StencilOp pass_op = StencilOp::Keep;
  CompareFunc func = CompareFunc::Always;
  uint8_t ref = 0;
  uint8_t read_mask = 0;
  uint8_t write_mask = 0;
};

struct DepthStencilState {
  // Host depth test enable = depth_func != Always || depth_write (Canary CreateD3D12Pipeline). depth_enable is that.
  bool depth_enable = false;
  bool depth_write = false;
  CompareFunc depth_func = CompareFunc::Always;
  bool stencil_enable = false;
  // Per-face state for APIs with per-face reference/masks (Vulkan). back == front unless two-sided stencil
  // (RB_DEPTHCONTROL.backface_enable on a polygonal primitive): then back uses the _bf ops and RB_STENCILREFMASK_BF.
  StencilFace front;
  StencilFace back;
  // D3D12 has one reference and one pair of masks: Canary picks RB_STENCILREFMASK_BF only if two-sided and front faces
  // are culled, else RB_STENCILREFMASK. Use these with OMSetStencilRef / StencilRead/WriteMask.
  uint8_t single_ref = 0;
  uint8_t single_read_mask = 0;
  uint8_t single_write_mask = 0;
  bool depth_float24 = false;  // RB_DEPTH_INFO.depth_format == kD24FS8 (20e4)
};

struct RasterState {
  CullMode cull_mode = CullMode::None;
  bool front_counter_clockwise = true;  // PA_SU_SC_MODE_CNTL.face == 0
  FillMode fill_mode = FillMode::Solid;
  int32_t depth_bias = 0;                // D3D10+ integer DepthBias (constant)
  float depth_bias_slope_scaled = 0.0f;  // SlopeScaledDepthBias = scale/16 * resolution_scale
  bool depth_clip = true;                // !PA_CL_CLIP_CNTL.clip_disable
  bool primitive_polygonal = false;      // has faces (cull/fill/two-sided stencil apply); rect lists are not (Canary)
  bool provoking_vertex_last = false;    // PA_SU_SC_MODE_CNTL.provoking_vtx_last
};

// Canary ViewportInfo, with the canonical top-left (Vulkan-like) NDC convention: +Y NDC is down the render target.
// D3D12 (origin bottom-left) must negate ndc_scale[1] and ndc_offset[1] - see NdcForBottomLeftOrigin.
// The vertex shader applies, after its own W handling: pos.xyz = pos.xyz * ndc_scale + ndc_offset * pos.w.
struct ViewportState {
  float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;  // host pixels (already multiplied by resolution_scale)
  float z_min = 0.0f, z_max = 1.0f;
  float ndc_scale[3] = {1.0f, 1.0f, 1.0f};
  float ndc_offset[3] = {0.0f, 0.0f, 0.0f};
  // PA_CL_VTE_CNTL W0 formats - system-constant flags for the vertex shader (Canary kSysFlag_*).
  bool xy_divided_by_w = false;   // vtx_xy_fmt: XY are already pre-divided by W (kSysFlag_XYDividedByW)
  bool z_divided_by_w = false;    // vtx_z_fmt: Z already pre-divided (kSysFlag_ZDividedByW)
  bool w_not_reciprocal = false;  // vtx_w0_fmt: incoming W is W, not 1/W (kSysFlag_WNotReciprocal)
  bool clip_disabled = false;     // PA_CL_CLIP_CNTL.clip_disable: huge viewport, offsets done in ndc_*.
};
inline void NdcForBottomLeftOrigin(ViewportState& v) {  // Canary GetHostViewportInfo origin_bottom_left
  v.ndc_scale[1] = -v.ndc_scale[1];
  v.ndc_offset[1] = -v.ndc_offset[1];
}

struct ScissorRect {
  uint32_t x = 0, y = 0, width = 0, height = 0;  // host pixels (multiplied by resolution_scale)
};

struct RenderTargetBlend {
  bool blend_enable = false;
  BlendFactor src_color = BlendFactor::One;
  BlendFactor dst_color = BlendFactor::Zero;
  BlendOp color_op = BlendOp::Add;
  BlendFactor src_alpha = BlendFactor::One;
  BlendFactor dst_alpha = BlendFactor::Zero;
  BlendOp alpha_op = BlendOp::Add;
  // RGBA bits 0-3. RB_COLOR_MASK limited to the format's components, the missing components then set (Canary
  // GetNormalizedColorMask). NOT yet ANDed with the pixel shader's written targets: the caller zeroes RTs the shader
  // doesn't write (Canary does that before blending too; a zero mask means One/Zero/Add, blend off).
  uint8_t write_mask = 0;
  uint8_t color_format = 0;  // RB_COLOR_INFO.color_format (xenos::ColorRenderTargetFormat)
};

struct BlendState {
  RenderTargetBlend rt[4];
  float constant[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // RB_BLEND_RED, GREEN, BLUE, ALPHA
};

// The host has no alpha test: pass func + ref to the pixel shader as system constants (Canary
// kSysFlag_AlphaPassIfLess_Shift = func bits, alpha_test_reference = ref).
struct AlphaTestState {
  CompareFunc func = CompareFunc::Always;  // Always when RB_COLORCONTROL.alpha_test_enable is clear
  float ref = 0.0f;                        // RB_ALPHA_REF
  bool alpha_to_mask_enable = false;       // RB_COLORCONTROL.alpha_to_mask_enable (alpha to coverage)
};

struct PipelineState {
  DepthStencilState depth_stencil;
  RasterState raster;
  ViewportState viewport;
  ScissorRect scissor;
  BlendState blend;
  AlphaTestState alpha_test;
  Topology topology = Topology::Unsupported;
  bool skip = false;
  SkipReason skip_reason = SkipReason::None;
  const char* skip_reason_text = "none";
};

// ---------------------------------------------------------------------------------------------------------------------
// Implementation details
// ---------------------------------------------------------------------------------------------------------------------
namespace detail {
constexpr uint32_t Bits(uint32_t v, uint32_t shift, uint32_t width) { return (v >> shift) & ((1u << width) - 1u); }
inline float F(const uint32_t* regs, uint32_t index) { return std::bit_cast<float>(regs[index]); }
constexpr int32_t SignExtend(uint32_t v, uint32_t width) {
  const uint32_t m = 1u << (width - 1);
  return int32_t((v ^ m) - m);
}
// xe::clamp_float / xe::saturate. NaN -> lo (Canary's SSE min/max also drop the NaN operand here).
inline float ClampF(float v, float lo, float hi) { return v > lo ? (v < hi ? v : hi) : lo; }
inline float Saturate(float v) { return ClampF(v, 0.0f, 1.0f); }
// float -> int32 for floor() results, saturating so no UB on huge values (Canary relies on the clamp before it).
inline int32_t ToInt(float v) {
  if (!(v > -2147483648.0f)) return INT32_MIN;
  if (!(v < 2147483648.0f)) return INT32_MAX;
  return int32_t(v);
}

constexpr uint32_t kEdramModeColorDepth = 4, kEdramModeDepthOnly = 5, kEdramModeCopy = 6;
constexpr uint32_t kVsExportModeMultipass = 7;
constexpr uint32_t kVgtOutputPathTessellationEnable = 1;
constexpr uint32_t kPolygonTypeTriangles = 2;
constexpr uint32_t kPolygonModeDual = 1;
constexpr uint32_t kDepthFormatD24FS8 = 1;
// D3D12_VIEWPORT_BOUNDS_MAX (Canary IssueDraw passes it as x_max / y_max).
constexpr uint32_t kViewportBoundsMax = 32767;
// xenos::kTexture2DCubeMaxWidthHeight (clip-disabled viewport extent cap).
constexpr uint32_t kMaxRenderTargetDimension = 8192;
// draw_util.h kXenosSamplePositions2x/4x (1/16 pixel).
constexpr int8_t kXenosSamplePositions2x[2][2] = {{-4, 4}, {4, -4}};
constexpr int8_t kXenosSamplePositions4x[4][2] = {{-2, -2}, {2, 2}, {-6, 6}, {6, -6}};

// draw_util.h EncodeIsPrimitivePolygonalTable: tri list/fan/strip/with-W-flags, quad list/strip, polygon. Rect lists are
// deliberately not polygonal in Canary (culling them breaks 4D53082D).
constexpr bool IsPolygonalNonPatch(uint32_t prim) {
  switch (prim) {
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x0D: case 0x0E: case 0x0F: return true;
    default: return false;
  }
}

// pipeline_cache.cc kBlendFactorMap / kBlendFactorAlphaMap (xenos::BlendFactor 5-bit value -> PipelineBlendFactor;
// undefined values -> Zero).
constexpr BlendFactor MapColorBlendFactor(uint32_t f) {
  switch (f) {
    case 1: return BlendFactor::One;
    case 4: return BlendFactor::SrcColor;
    case 5: return BlendFactor::InvSrcColor;
    case 6: return BlendFactor::SrcAlpha;
    case 7: return BlendFactor::InvSrcAlpha;
    case 8: return BlendFactor::DestColor;
    case 9: return BlendFactor::InvDestColor;
    case 10: return BlendFactor::DestAlpha;
    case 11: return BlendFactor::InvDestAlpha;
    case 12: return BlendFactor::BlendFactor;
    case 13: return BlendFactor::InvBlendFactor;
    case 14: return BlendFactor::AlphaFactor;
    case 15: return BlendFactor::InvAlphaFactor;
    case 16: return BlendFactor::SrcAlphaSat;
    default: return BlendFactor::Zero;
  }
}
constexpr BlendFactor MapAlphaBlendFactor(uint32_t f) {
  switch (f) {
    case 1: return BlendFactor::One;
    case 4: case 6: return BlendFactor::SrcAlpha;
    case 5: case 7: return BlendFactor::InvSrcAlpha;
    case 8: case 10: return BlendFactor::DestAlpha;
    case 9: case 11: return BlendFactor::InvDestAlpha;
    case 12: case 14: return BlendFactor::BlendFactor;
    case 13: case 15: return BlendFactor::InvBlendFactor;
    case 16: return BlendFactor::SrcAlphaSat;
    default: return BlendFactor::Zero;
  }
}
constexpr BlendOp MapBlendOp(uint32_t op) { return op <= 4 ? BlendOp(op) : BlendOp::Add; }  // kBlendOpMap

// xenos.h GetColorRenderTargetFormatComponentCount.
constexpr uint32_t ColorFormatComponentCount(uint32_t fmt) {
  switch (fmt) {
    case 4: case 6: case 15: return 2;   // 16_16, 16_16_FLOAT, 32_32_FLOAT
    case 14: return 1;                   // 32_FLOAT
    default: return 4;                   // 8888, 8888_GAMMA, 2_10_10_10(_FLOAT)(_AS_...), 16_16_16_16(_FLOAT); others
  }
}
}  // namespace detail

// Topology decision. Gears 2 leaves VGT_OUTPUT_PATH_CNTL.path_select = tessellation for hundreds of thousands of ordinary
// draws, so path_select alone never makes a patch: only 0x10-0x12 (line/triangle/quad patch) in explicit major mode with
// tessellation selected. 0x10-0x12 without that are the 2D copy rect lists -> Unsupported.
inline Topology TranslateTopology(const uint32_t* regs, const PipelineInputs& in) {
  const bool path_tess =
      detail::Bits(regs[reg::VGT_OUTPUT_PATH_CNTL], 0, 2) == detail::kVgtOutputPathTessellationEnable;
  switch (in.primitive) {
    case 0x01: return Topology::PointList;
    case 0x04: return Topology::TriangleList;
    case 0x05: return Topology::TriangleList;  // fan -> list (prim::TriangleFanToList)
    case 0x06: return Topology::TriangleStrip;
    case 0x08: return Topology::RectListAsTriangles;
    case 0x0D: return Topology::QuadListAsTriangles;
    case 0x10: case 0x11: case 0x12:
      return (in.major_mode_explicit && path_tess) ? Topology::Patch : Topology::Unsupported;
    default: return Topology::Unsupported;
  }
}

// Canary IsPrimitivePolygonal, but with our patch rule instead of path_select alone.
inline bool IsPrimitivePolygonal(Topology topology, uint32_t primitive) {
  if (topology == Topology::Patch) return primitive == 0x11 || primitive == 0x12;
  return detail::IsPolygonalNonPatch(primitive);
}

// Canary GetHostViewportInfo (draw_util.cc:308-729) with: origin top-left, allow_reverse_z = false (D3D12),
// pixel_shader_writes_depth = false (if it does, the caller sets z_min = 0, z_max = 1), convert_z_to_float24 and
// full_float24_in_0_to_1 off, window offset kept in the viewport, x_max = y_max = D3D12_VIEWPORT_BOUNDS_MAX, and
// xenos_sample_positions = true (the host is assumed to program the Xenos sample positions when it uses MSAA).
// ArchReciprocalRefined is replaced by an exact division (differs from rcpss+Newton by at most 1 ULP when not exact).
inline ViewportState TranslateViewport(const uint32_t* regs, const PipelineInputs& in) {
  using namespace detail;
  ViewportState out;
  const float s = (in.resolution_scale > 0.0f) ? in.resolution_scale : 1.0f;
  const uint32_t clip = regs[reg::PA_CL_CLIP_CNTL];
  const uint32_t vte = regs[reg::PA_CL_VTE_CNTL];
  const uint32_t mode = regs[reg::PA_SU_SC_MODE_CNTL];
  const uint32_t vtx = regs[reg::PA_SU_VTX_CNTL];
  out.xy_divided_by_w = Bits(vte, 8, 1);
  out.z_divided_by_w = Bits(vte, 9, 1);
  out.w_not_reciprocal = Bits(vte, 10, 1);
  out.clip_disabled = Bits(clip, 16, 1);

  const float scale_xy[2] = {Bits(vte, 0, 1) ? F(regs, reg::PA_CL_VPORT_XSCALE) : 1.0f,
                             Bits(vte, 2, 1) ? F(regs, reg::PA_CL_VPORT_YSCALE) : 1.0f};
  const float scale_z = Bits(vte, 4, 1) ? F(regs, reg::PA_CL_VPORT_ZSCALE) : 1.0f;
  const float offset_base_xy[2] = {Bits(vte, 1, 1) ? F(regs, reg::PA_CL_VPORT_XOFFSET) : 0.0f,
                                   Bits(vte, 3, 1) ? F(regs, reg::PA_CL_VPORT_YOFFSET) : 0.0f};
  const float offset_z = Bits(vte, 5, 1) ? F(regs, reg::PA_CL_VPORT_ZOFFSET) : 0.0f;

  float offset_add_xy[2] = {0.0f, 0.0f};
  if (Bits(mode, 16, 1)) {  // vtx_window_offset_enable
    const uint32_t wo = regs[reg::PA_SC_WINDOW_OFFSET];
    offset_add_xy[0] += float(SignExtend(Bits(wo, 0, 15), 15));
    offset_add_xy[1] += float(SignExtend(Bits(wo, 16, 15), 15));
  }
  if (Bits(vtx, 0, 1) == 0) {  // pix_center == kD3DZero
    offset_add_xy[0] += 0.5f;
    offset_add_xy[1] += 0.5f;
  }
  // Keep the Xenos sample offset in host pixels with resolution scaling (UE3 adds sample 0's position to the viewport
  // offset under predicated tiling, draw_util.h:139-143).
  uint32_t msaa = in.msaa_samples;
  if (!msaa) msaa = 1u << Bits(regs[reg::RB_SURFACE_INFO], 16, 2);
  if (msaa > 1 && s > 1.0f && in.xenos_sample_positions) {
    float frac[2];
    for (uint32_t i = 0; i < 2; ++i) {
      const float axis = offset_base_xy[i] + offset_add_xy[i];
      frac[i] = axis - std::floor(axis) - 0.5f;
    }
    const int8_t(*pos)[2] = msaa >= 4 ? kXenosSamplePositions4x : kXenosSamplePositions2x;
    const uint32_t count = msaa >= 4 ? 4 : 2;
    for (uint32_t i = 0; i < count; ++i) {
      if (frac[0] == float(pos[i][0]) * (1.0f / 16.0f) && frac[1] == float(pos[i][1]) * (1.0f / 16.0f)) {
        for (uint32_t j = 0; j < 2; ++j) offset_add_xy[j] -= frac[j] * (s - 1.0f) / s;
        break;
      }
    }
  }

  // draw_resolution_scale_*_divisor.Apply(x_max): floor(max / scale).
  const uint32_t xy_max_unscaled = std::max<uint32_t>(1u, uint32_t(float(kViewportBoundsMax) / s));

  float ndc_scale[3], ndc_offset[3];
  if (out.clip_disabled) {
    const uint32_t extent_unscaled = std::min(kMaxRenderTargetDimension, xy_max_unscaled);
    const float extent_f = float(extent_unscaled);
    const float pixels_to_ndc = 2.0f / extent_f;  // ViewportRecip2_0
    for (uint32_t i = 0; i < 2; ++i) {
      ndc_scale[i] = scale_xy[i] * pixels_to_ndc;
      ndc_offset[i] = (offset_base_xy[i] - extent_f * 0.5f + offset_add_xy[i]) * pixels_to_ndc;
    }
    out.x = 0.0f;
    out.y = 0.0f;
    out.width = out.height = extent_f * s;
    out.z_min = 0.0f;
    out.z_max = 1.0f;
    ndc_scale[2] = scale_z;
    ndc_offset[2] = offset_z;
  } else {
    float xy_offset[2], xy_extent[2];
    for (uint32_t i = 0; i < 2; ++i) {
      const float offset_axis = offset_base_xy[i] + offset_add_xy[i];
      const float scale_axis = scale_xy[i];
      const float scale_abs = std::abs(scale_axis);
      const float axis_max = float(xy_max_unscaled);
      const int32_t axis_1 = ToInt(std::floor(ClampF(offset_axis + scale_abs, 0.0f, axis_max)));
      const int32_t axis_0 = ToInt(std::floor(ClampF(offset_axis - scale_abs, float(axis_1) - axis_max, axis_max)));
      const uint32_t extent = axis_1 > 0 ? uint32_t(axis_1 - axis_0) : 0u;
      xy_offset[i] = float(axis_0) * s;
      xy_extent[i] = float(extent) * s;
      if (extent) {
        const float extent_f = float(extent);
        const float inv = 1.0f / extent_f;
        ndc_scale[i] = scale_axis * 2.0f * inv;
        ndc_offset[i] = (offset_axis - (float(axis_0) + extent_f * 0.5f)) * 2.0f * inv;
      } else {
        ndc_scale[i] = 1.0f;
        ndc_offset[i] = 0.0f;
      }
    }
    out.x = xy_offset[0];
    out.y = xy_offset[1];
    out.width = xy_extent[0];
    out.height = xy_extent[1];

    float host_clip_offset_z, host_clip_scale_z;
    if (Bits(clip, 19, 1)) {  // dx_clip_space_def: 0...W
      host_clip_offset_z = offset_z;
      host_clip_scale_z = scale_z;
      ndc_scale[2] = 1.0f;
      ndc_offset[2] = 0.0f;
    } else {  // -W...W remapped to 0...W
      host_clip_offset_z = offset_z - scale_z;
      host_clip_scale_z = scale_z * 2.0f;
      ndc_scale[2] = 0.5f;
      ndc_offset[2] = 0.5f;
    }
    out.z_min = Saturate(host_clip_offset_z);
    out.z_max = Saturate(host_clip_offset_z + host_clip_scale_z);
    if (out.z_min > out.z_max) {  // D3D12 forbids reversed depth ranges
      std::swap(out.z_min, out.z_max);
      ndc_scale[2] = -ndc_scale[2];
      ndc_offset[2] = 1.0f - ndc_offset[2];
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    out.ndc_scale[i] = ndc_scale[i];
    out.ndc_offset[i] = ndc_offset[i];
  }
  return out;
}

// Canary GetScissorTmpl<clamp_to_surface_pitch = true> (draw_util.cc:826-912), window_offset_in_edram = false, then
// scaled by the resolution scale as IssueDraw does.
inline ScissorRect TranslateScissor(const uint32_t* regs, const PipelineInputs& in) {
  using namespace detail;
  const float s = (in.resolution_scale > 0.0f) ? in.resolution_scale : 1.0f;
  const uint32_t wtl = regs[reg::PA_SC_WINDOW_SCISSOR_TL], wbr = regs[reg::PA_SC_WINDOW_SCISSOR_BR];
  const uint32_t wo = regs[reg::PA_SC_WINDOW_OFFSET];
  const uint32_t stl = regs[reg::PA_SC_SCREEN_SCISSOR_TL], sbr = regs[reg::PA_SC_SCREEN_SCISSOR_BR];
  const int32_t pitch = int32_t(Bits(regs[reg::RB_SURFACE_INFO], 0, 14));
  int32_t tl_x = int32_t(Bits(wtl, 0, 14)), tl_y = int32_t(Bits(wtl, 16, 14));
  int32_t br_x = int32_t(Bits(wbr, 0, 14)), br_y = int32_t(Bits(wbr, 16, 14));
  if (!Bits(wtl, 31, 1)) {  // !window_offset_disable
    const int32_t ox = SignExtend(Bits(wo, 0, 15), 15), oy = SignExtend(Bits(wo, 16, 15), 15);
    tl_x += ox;
    tl_y += oy;
    br_x += ox;
    br_y += oy;
  }
  tl_x = std::max(tl_x, SignExtend(Bits(stl, 0, 15), 15));
  tl_y = std::max(tl_y, SignExtend(Bits(stl, 16, 15), 15));
  br_x = std::min(br_x, SignExtend(Bits(sbr, 0, 15), 15));
  br_y = std::min(br_y, SignExtend(Bits(sbr, 16, 15), 15));
  tl_x = std::min(tl_x, pitch);
  br_x = std::min(br_x, pitch);
  tl_x = std::max(tl_x, 0);
  tl_y = std::max(tl_y, 0);
  br_x = std::max(br_x, tl_x);
  br_y = std::max(br_y, tl_y);
  ScissorRect r;
  r.x = uint32_t(float(tl_x) * s);
  r.y = uint32_t(float(tl_y) * s);
  r.width = uint32_t(float(br_x - tl_x) * s);
  r.height = uint32_t(float(br_y - tl_y) * s);
  return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// The whole translation
// ---------------------------------------------------------------------------------------------------------------------
inline PipelineState TranslatePipelineState(const uint32_t* regs, const PipelineInputs& in) {
  using namespace detail;
  PipelineState ps;
  const float res_scale = (in.resolution_scale > 0.0f) ? in.resolution_scale : 1.0f;

  ps.topology = TranslateTopology(regs, in);
  const bool polygonal = IsPrimitivePolygonal(ps.topology, in.primitive);
  ps.raster.primitive_polygonal = polygonal;

  const uint32_t edram_mode = Bits(regs[reg::RB_MODECONTROL], 0, 3);
  const uint32_t surface_info = regs[reg::RB_SURFACE_INFO];
  const uint32_t mode = regs[reg::PA_SU_SC_MODE_CNTL];
  const bool cull_front = Bits(mode, 0, 1), cull_back = Bits(mode, 1, 1);

  // ---- skip rules: Canary IssueDraw order ----
  auto set_skip = [&ps](SkipReason r) {
    if (!ps.skip) {
      ps.skip = true;
      ps.skip_reason = r;
      ps.skip_reason_text = SkipReasonName(r);
    }
  };
  if (edram_mode == kEdramModeCopy) set_skip(SkipReason::EdramCopy);
  if (!Bits(surface_info, 0, 14)) set_skip(SkipReason::SurfacePitchZero);
  // IsRasterizationPotentiallyDone.
  if (edram_mode != kEdramModeColorDepth && edram_mode != kEdramModeDepthOnly)
    set_skip(SkipReason::EdramModeNoRasterization);
  if (Bits(regs[reg::SQ_PROGRAM_CNTL], 24, 3) == kVsExportModeMultipass) set_skip(SkipReason::VertexShaderMultipass);
  if (Bits(regs[reg::PA_SC_VIZ_QUERY], 7, 1)) set_skip(SkipReason::KillPixPostHiZ);
  if (polygonal && cull_front && cull_back) set_skip(SkipReason::BothFacesCulled);
  if (ps.topology == Topology::Unsupported) set_skip(SkipReason::UnsupportedPrimitive);

  // ---- depth / stencil (GetNormalizedDepthControl + GetCurrentStateDescription) ----
  uint32_t dc = regs[reg::RB_DEPTHCONTROL];
  if (edram_mode != kEdramModeColorDepth && edram_mode != kEdramModeDepthOnly) dc = 0;
  bool z_enable = Bits(dc, 1, 1);
  const bool z_write = Bits(dc, 2, 1);
  const CompareFunc zfunc = CompareFunc(Bits(dc, 4, 3));
  if (z_enable && !z_write && zfunc == CompareFunc::Always) z_enable = false;
  DepthStencilState& ds = ps.depth_stencil;
  ds.depth_float24 = Bits(regs[reg::RB_DEPTH_INFO], 16, 1) == kDepthFormatD24FS8;
  if (z_enable) {
    ds.depth_func = zfunc;
    ds.depth_write = z_write;
  }
  ds.depth_enable = ds.depth_func != CompareFunc::Always || ds.depth_write;
  if (Bits(dc, 0, 1)) {
    ds.stencil_enable = true;
    const bool two_sided = polygonal && Bits(dc, 7, 1);
    const uint32_t srm = regs[reg::RB_STENCILREFMASK], srm_bf = regs[reg::RB_STENCILREFMASK_BF];
    auto fill_masks = [](StencilFace& f, uint32_t v) {
      f.ref = uint8_t(Bits(v, 0, 8));
      f.read_mask = uint8_t(Bits(v, 8, 8));
      f.write_mask = uint8_t(Bits(v, 16, 8));
    };
    ds.front.func = CompareFunc(Bits(dc, 8, 3));
    ds.front.fail_op = StencilOp(Bits(dc, 11, 3));
    ds.front.pass_op = StencilOp(Bits(dc, 14, 3));
    ds.front.depth_fail_op = StencilOp(Bits(dc, 17, 3));
    fill_masks(ds.front, srm);
    if (two_sided) {
      ds.back.func = CompareFunc(Bits(dc, 20, 3));
      ds.back.fail_op = StencilOp(Bits(dc, 23, 3));
      ds.back.pass_op = StencilOp(Bits(dc, 26, 3));
      ds.back.depth_fail_op = StencilOp(Bits(dc, 29, 3));
      fill_masks(ds.back, srm_bf);
    } else {
      ds.back = ds.front;
    }
    const uint32_t single = (two_sided && cull_front && !cull_back) ? srm_bf : srm;
    ds.single_ref = uint8_t(Bits(single, 0, 8));
    ds.single_read_mask = uint8_t(Bits(single, 8, 8));
    ds.single_write_mask = uint8_t(Bits(single, 16, 8));
  }

  // ---- rasteriser (GetCurrentStateDescription) ----
  RasterState& rs = ps.raster;
  rs.provoking_vertex_last = Bits(mode, 19, 1);
  rs.depth_clip = !Bits(regs[reg::PA_CL_CLIP_CNTL], 16, 1);
  if (polygonal) {
    rs.front_counter_clockwise = Bits(mode, 2, 1) == 0;
    rs.cull_mode = cull_front ? CullMode::Front : (cull_back ? CullMode::Back : CullMode::None);
    bool wire = false;
    if (!cull_front && Bits(mode, 5, 3) != kPolygonTypeTriangles) wire = true;
    if (!cull_back && Bits(mode, 8, 3) != kPolygonTypeTriangles) wire = true;
    if (Bits(mode, 3, 2) != kPolygonModeDual) wire = false;
    rs.fill_mode = wire ? FillMode::Wireframe : FillMode::Solid;
  }
  // GetPreferredFacePolygonOffset, without roundToNearestOrderOfMagnitude on the front scale.
  float bias_scale = 0.0f, bias_offset = 0.0f;
  if (polygonal) {
    if (Bits(mode, 11, 1) && !cull_front) {
      bias_scale = F(regs, reg::PA_SU_POLY_OFFSET_FRONT_SCALE);
      bias_offset = F(regs, reg::PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    if (Bits(mode, 12, 1) && !cull_back && bias_scale == 0.0f && bias_offset == 0.0f) {
      bias_scale = F(regs, reg::PA_SU_POLY_OFFSET_BACK_SCALE);
      bias_offset = F(regs, reg::PA_SU_POLY_OFFSET_BACK_OFFSET);
    }
  } else if (Bits(mode, 13, 1)) {  // poly_offset_para_enable
    bias_scale = F(regs, reg::PA_SU_POLY_OFFSET_FRONT_SCALE);
    bias_offset = F(regs, reg::PA_SU_POLY_OFFSET_FRONT_OFFSET);
  }
  rs.depth_bias = xenos::prim::D3D10IntegerPolygonOffset(ds.depth_float24, bias_offset);
  rs.depth_bias_slope_scaled = bias_scale * xenos::prim::kPolygonOffsetScaleSubpixelUnit * res_scale;

  // ---- viewport / scissor ----
  ps.viewport = TranslateViewport(regs, in);
  ps.scissor = TranslateScissor(regs, in);

  // ---- blend (GetNormalizedColorMask + GetCurrentStateDescription + CreateD3D12Pipeline) ----
  const uint32_t color_mask = regs[reg::RB_COLOR_MASK];
  for (uint32_t i = 0; i < 4; ++i) {
    RenderTargetBlend& rt = ps.blend.rt[i];
    const uint32_t fmt = Bits(regs[reg::kColorInfo[i]], 16, 4);
    rt.color_format = uint8_t(fmt);
    uint32_t wm = 0;
    if (edram_mode == kEdramModeColorDepth) {
      const uint32_t comp = (1u << ColorFormatComponentCount(fmt)) - 1u;
      wm = (color_mask >> (4 * i)) & comp;
      if (wm) wm |= 0xFu & ~comp;
    }
    rt.write_mask = uint8_t(wm);
    if (!wm) continue;  // One/Zero/Add defaults, blend off
    const uint32_t bc = regs[reg::kBlendControl[i]];
    rt.src_color = MapColorBlendFactor(Bits(bc, 0, 5));
    rt.color_op = MapBlendOp(Bits(bc, 5, 3));
    rt.dst_color = MapColorBlendFactor(Bits(bc, 8, 5));
    rt.src_alpha = MapAlphaBlendFactor(Bits(bc, 16, 5));
    rt.alpha_op = MapBlendOp(Bits(bc, 21, 3));
    rt.dst_alpha = MapAlphaBlendFactor(Bits(bc, 24, 5));
    rt.blend_enable = rt.src_color != BlendFactor::One || rt.dst_color != BlendFactor::Zero ||
                      rt.color_op != BlendOp::Add || rt.src_alpha != BlendFactor::One ||
                      rt.dst_alpha != BlendFactor::Zero || rt.alpha_op != BlendOp::Add;
    if (!rt.blend_enable) continue;
    // Host MIN/MAX ignore the factors, so they are normalised to One (CreateD3D12Pipeline). Canary keys all four
    // factors on the COLOUR op only, which also forces One/One on an Add alpha op; we normalise per op (README).
    if (rt.color_op == BlendOp::Min || rt.color_op == BlendOp::Max) rt.src_color = rt.dst_color = BlendFactor::One;
    if (rt.alpha_op == BlendOp::Min || rt.alpha_op == BlendOp::Max) rt.src_alpha = rt.dst_alpha = BlendFactor::One;
  }
  ps.blend.constant[0] = F(regs, reg::RB_BLEND_RED);
  ps.blend.constant[1] = F(regs, reg::RB_BLEND_GREEN);
  ps.blend.constant[2] = F(regs, reg::RB_BLEND_BLUE);
  ps.blend.constant[3] = F(regs, reg::RB_BLEND_ALPHA);

  // ---- alpha test (UpdateSystemConstantValues_Impl) ----
  const uint32_t cc = regs[reg::RB_COLORCONTROL];
  ps.alpha_test.func = Bits(cc, 3, 1) ? CompareFunc(Bits(cc, 0, 3)) : CompareFunc::Always;
  ps.alpha_test.ref = F(regs, reg::RB_ALPHA_REF);
  ps.alpha_test.alpha_to_mask_enable = Bits(cc, 4, 1);
  return ps;
}

}  // namespace xenos::renderer
