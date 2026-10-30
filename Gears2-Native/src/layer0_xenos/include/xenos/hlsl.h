// Layer 0 (game-free): Xbox 360 GPU (Xenos) shader microcode -> HLSL (Stage 3 step S3.3, notes/36, 38, 39).
// Input is the decoded program from xenos/ucode.h (S3.2); output is one HLSL source per shader that compiles with DXC
// for both D3D12 (DXIL, Shader Model 6.6) and Vulkan (SPIR-V, vulkan1.3). The binding layout below is the contract the
// S3.5 device must follow; notes/39 has the same table.
//
// Semantics follow Xenia (Copyright Ben Vanik and Xenia project contributors) via rexglue, BSD-3-Clause: the CPU shader
// interpreter (src/graphics/pipeline/shader/interpreter.cpp) for ALU, control flow and vertex fetch, and the DXBC
// translator (dxbc_translator*.cpp) for texture fetch, position completion and pixel parameter generation. Where the
// two differ, the choice is written next to the code. XenosRecomp (MIT, hedge-dev) was read for structure only; nothing
// is copied from it. This file and hlsl.cpp are our own code (MIT).
//
// Standalone on purpose (standard library only), like ucode.h.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <xenos/ucode.h>

namespace xenos::hlsl {

struct HlslOptions {
  // Patch 0059 (DXBC back end only - the HLSL path ignores these; all off = the 0055 output, unchanged):
  // dxbc_loop_only_if_needed: a straight-line shader is wrapped in a one-pass loop only when it has a conditional
  // exec_end (the only thing that `break`s out of it).
  bool dxbc_loop_only_if_needed = false;
  // dxbc_canary_minmax: max / min as Canary (dxbc_shader_translator_alu.cc: SM3 `a >= b ? a : b` / `a < b ? a : b`,
  // 2 instructions) instead of the FTZ + total-order + NaN emulation (~20), and result clamps as DXBC _sat.
  bool dxbc_canary_minmax = false;
  // dxbc_salt != 0: one dead instruction carrying this value, so the bytecode (and the driver's cache key) is unique -
  // for the pipeline benchmark, which must time the driver's real compile.
  uint32_t dxbc_salt = 0;
  // Patch 0060: dxbc_direct_operands - ALU sources without abs / negate and with absolute addressing are used in
  // place (register or constant buffer, swizzled) instead of being copied to a temp first. Same values.
  bool dxbc_direct_operands = false;
  // Patch 0060: dxbc_slim_prologue - the pixel shader's interpolator and pixel-parameter copies as selects (4 tests per
  // instruction) instead of one if / mov / endif per register. Same values.
  bool dxbc_slim_prologue = false;
  // Kills become demote-to-helper (HLSL `discard` keeps the lane as a helper on D3D12; on Vulkan 1.3 DXC lowers it to
  // OpDemoteToHelperInvocation). Kept as an option so a terminate variant can be tried if a game depends on it.
  bool kill_as_demote = true;
  // Name of the entry point.
  std::string entry = "main";
  // Vertex shaders: N > 0 makes the variant for Xenos quad-patch tessellation (prim 0x12, notes/51 section 10): the
  // host draws patch_count * 6 * N * N vertices with no vertex streams, and the prologue gives the guest shader what
  // the tessellator gives it - r0 = (patch index, u, v, 0), r1.x = 0 (Xenia dxbc_shader_translator.cc domain-shader
  // inputs: patch index as float, domain location u/v, r1.x the corner-swizzle selector = identity) - for an N x N
  // grid of two triangles per cell. The first patch comes from xe_sys0.y (VGT_INDX_OFFSET), as the vertex offset does.
  uint32_t tess_quad_segments = 0;
  // Float-constant compaction (speed; off by default until a renderer run proves it): when a shader reads no
  // constant relatively and its reads leave a gap of kCompactMinGap+ registers, absolute reads index a compacted
  // array - c[0, float_constants_low) then c[float_constants_high, float_constants) - see Translation.
  bool compact_float_constants = false;
  // Patch 0024: texture sign / bias / gamma per component from the component the fetch swizzle picked (as Canary).
  bool tex_sign_by_swizzle = false;
  // Patch 0041: Canary 21-bit rounding of the scalar approximations (0037), off by default.
  bool canary_r21 = false;
  // Patch 0040: Canary texture num_format scaling, offset-binary unsigned-biased decode and Q16 rounding.
  bool tex_num_format = false;
  // Patch 0028: vertex index masked to 24 bits and clamped to VGT_MIN/MAX_VTX_INDX (xe_sys2.xy), as Canary.
  bool vertex_index_clamp = false;
  // Patch 0087 (DXBC back end only; the HLSL path ignores these):
  // canary_tfetch1d_xy - a tfetch1D the decoder gave two coordinates (Canary fbdb1f2: swizzle not X-replicated) reads
  // Y too, kept only when the fetch constant is not really 1D (Canary 7cd4794's run-time check); before, Y was 0.
  bool canary_tfetch1d_xy = false;
  // mulsc_round_toward_zero - Canary's experimental mulsc rounding (3390fc2, its switch off by default there too).
  bool mulsc_round_toward_zero = false;
  // memexport_f16_nan_zero - NaN becomes 0 before a float16 memory export (Canary 3ff230d's rule); was -131008.
  bool memexport_f16_nan_zero = false;
  // Patch 0088 (Vulkan, measurement): false = the SPIR-V made from our DXBC has no NoContraction, so the driver may
  // fuse multiply-adds - whether our strict "precise everywhere" maths is the light-scene GPU cost.
  bool spirv_precise = true;
  // Patch 0095 (DXBC back end): pixel shaders that don't write depth declare "refactoring allowed", so the driver may
  // fuse multiply-adds and reorder maths there (colour results can differ in the last bit; positions and depth stay
  // precise - vertex shaders and depth-writing pixel shaders keep the flag off, notes/42).
  bool ps_refactoring_allowed = false;
  // Patch 0105 (DXBC back end, with tex_num_format): the texture fetch post-processing values that depend only on the
  // fetch constant (per component: sign mode, signed mode, fixed width; per fetch: fixed / integer / point sampled) are
  // read from the fetch-constant buffer (2 dwords per fetch slot after the 192 fetch dwords, filled by the renderer -
  // xenos::TexPostConstants) instead of being worked out per pixel (a 25-way format switch and ~40 integer ops per
  // fetch). Same results.
  bool tex_post_constants = false;
  // Job 634 (DXBC back end, with tex_post_constants): the post-processing as multiply-adds and selects on per-slot
  // constants (TexPostAffine below) - ~45 instructions a fetch instead of ~100, the same values bit for bit.
  bool tex_post_affine = false;
  // Job 634 (DXBC back end): the legacy multiply's zero rule as min(|x|, |y|) != 0 (four instructions, same bits).
  bool dxbc_mul_min_abs = false;
};

// Patch 0105: the two dwords per fetch slot that tex_post_constants shaders read (fetch = the slot's 6 fetch-constant
// dwords as the shaders see them; sign_by_swizzle = tex_sign_by_swizzle). dw0: per component c bits 4c..4c+1 sign mode,
// 4c+2..4c+3 signed mode; bit 16 fixed, 17 integer, 18 point sampled. dw1: per component c bits 8c..8c+7 the fixed width
// that carries (0 = none).
void TexPostConstants(const uint32_t* fetch, bool sign_by_swizzle, uint32_t out[2]);
// Job 634 (tex_post_affine): kTexPostAffineVectors float4s per fetch slot, after the 48 fetch-constant vectors (slot k
// at vector 48 + 8k). Vector 0: per component the steps as bits - 1 gamma, 2 unsigned biased, 4 integer, 8 integer
// rounding (point sampled), 16 point-sampled 4-6 bit decode, 32 16 fractional bits, 64 clamp to -1. Vectors 1-3: the
// biased decode's A1, B1, D1 ((v * A1 + B1) / D1); 4-5: the integer A2, B2 (v * A2 + B2); 6: P (= e - 1 of the 4-6
// bit decode); 7: the exponent adjust 2^k in every component.
inline constexpr uint32_t kTexPostAffineVectors = 8;
void TexPostAffine(const uint32_t* fetch, bool sign_by_swizzle, uint32_t out[kTexPostAffineVectors * 4]);

// Smallest unread gap worth compacting (16 registers = 256 bytes, the constant-buffer upload granularity).
inline constexpr uint32_t kCompactMinGap = 16;

// A place where the generated code uses a placeholder, a stub or a simplified path, so S3.4's comparisons know which
// differences are expected. Hashes plus these strings are all that leaves the tool (no shader content).
struct Simplification {
  std::string feature;  // short machine-readable name, e.g. "tex_get_weights"
  std::string note;     // one line of human-readable detail
};

struct Translation {
  std::string hlsl;
  std::vector<Simplification> simplifications;  // per-shader (global caveats are in notes/39)
  bool uses_pc_loop = false;                     // control flow needed the pc-driven loop (jumps, calls or loops)
  uint32_t texture_slots_2d = 0;                 // bit per fetch constant 0-31 bound as Texture2D
  uint32_t texture_slots_3d = 0;                 // ... as Texture3D
  uint32_t texture_slots_cube = 0;               // ... as TextureCube
  bool uses_shared_memory = false;               // vertex fetch reads the guest memory buffer
  uint32_t float_constants = 0;                  // c0..c(n-1) can be read (256 once any read is relative)
  // The constant buffer this shader reads: c[0, float_constants_low) followed by c[float_constants_high,
  // float_constants). Both equal float_constants when nothing was compacted (then it is c[0, float_constants)).
  uint32_t float_constants_low = 0, float_constants_high = 0;
  uint64_t float_constants_read[4] = {};  // bit per constant read with absolute addressing (original numbering)
  bool float_constants_relative = false;  // any a0/aL-relative constant read
  bool memexport = false;                        // eA/eM writes: the shader writes guest memory through u0 (notes/11)
  std::vector<uint32_t> memexport_stream_constants;  // float constants read by eA writes (the stream constants)
};

Translation TranslateToHlsl(const ucode::Program& program, const HlslOptions& options = {});

// The fixed part of every shader (bindings, system constants, helper functions), for documentation and tests.
std::string CommonPrelude(bool is_vertex_shader);

}  // namespace xenos::hlsl
