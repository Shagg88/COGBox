// Layer 0 (game-free): Xenos shader microcode -> HLSL (Stage 3 step S3.3). See hlsl.h for the scope, the sources the
// semantics come from (Xenia via rexglue, BSD-3-Clause: interpreter.cpp and dxbc_translator*.cpp) and the licence (our
// code, MIT). notes/39 documents the design and the binding contract; this comment block is the short version.
//
// Shape of every generated shader:
//  - A fixed prelude: the constant buffers, resources and helper functions (CommonPrelude). Helpers carry the Xenos
//    maths rules from notes/38 (legacy multiply, D3D9 max/min order, rcp/rsq/log/exp special cases), each marked
//    `precise` so DXC's fast-math cannot fold them back into plain HLSL operations.
//  - main(): Xenos registers are a local `float4 r[N]` (dynamic indexing only where aL/a0-relative addressing needs
//    it), plus the previous scalar `ps`, the predicate `p0`, the address register `a0`, the loop and call stacks, the
//    vfetch address and the register LOD/gradients.
//  - Control flow: shaders without loops, calls or jumps (most) are emitted straight-line inside a one-pass
//    `do { } while (false)`, where an executed exec_end is a `break`. Shaders with any loop, call or jump use a
//    pc-driven loop: each control flow instruction k is `if (xe_pc == k) { ...; xe_pc = k + 1; }` in order, so
//    consecutive instructions run in one pass, a backward jump sets xe_pc and `continue`s, and an ended shader breaks
//    out. This mirrors how Xenia's DXBC translator structures control flow (an if-chain inside a loop). A guard counter
//    stops a malformed shader from hanging the GPU.
//  - Each ALU slot reads all its sources first, computes the vector and scalar results into temporaries, then writes,
//    as the hardware issues both operations together.
//  - Epilogue: vertex shaders complete the position like Xenia's CompleteVertexOrDomainShader (1/W, XY/W and Z/W flags,
//    then the guest-to-host NDC scale and offset); pixel shaders apply the alpha test and the per-target colour
//    exponent bias (RB_COLOR_INFO.color_exp_bias, as a multiplier the device supplies).

#include <xenos/hlsl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <set>
#include <string>
#include <string_view>

namespace xenos::hlsl {

using namespace ::xenos::ucode;

namespace {

// ---- The fixed prelude --------------------------------------------------------------------------------------------

// Binding contract (also in notes/39). D3D12: everything in space0. Vulkan: [[vk::binding(binding, set)]] - set 0 the
// constant buffers, set 1 the textures and the guest memory buffer, set 2 the samplers; binding numbers equal the D3D12
// register numbers so one table describes both.
constexpr const char* kPreludeCommon = R"(// ---- Xenos HLSL prelude (Gears2-Native S3.3) ----
// Vulkan bindings only when compiling to SPIR-V (DXIL would warn that the attribute is ignored).
#ifdef __spirv__
#define XE_VK(b_, s_) [[vk::binding(b_, s_)]]
#else
#define XE_VK(b_, s_)
#endif
// Exact FLT_MAX (a decimal literal was compiled to 0x7F7FFFFE by DXC -spirv; job 091 probe).
#define XE_FLT_MAX asfloat(0x7F7FFFFFu)

// System constants the device fills per draw (b0 / set 0 binding 0).
XE_VK(0, 0) cbuffer XeSystem : register(b0, space0) {
  // x: flags (bit 0 W not reciprocal, 1 XY divided by W, 2 Z divided by W, 3 primitive is polygonal, 4 primitive is a
  //    line, 5 alpha test on); y: vertex index offset (int); z: pixel shader interpolator mask (bit per register);
  // w: pixel parameter generation (bit 31 on, bits 0-3 the register).
  uint4 xe_sys0;
  float4 xe_ndc_scale;
  float4 xe_ndc_offset;
  // Per render target 2^RB_COLOR_INFO.color_exp_bias.
  float4 xe_color_exp_scale;
  // x: alpha test function (Xenos CompareFunction 0-7), y: alpha reference (float bits).
  uint4 xe_sys1;
#ifdef XE_VTX_CLAMP
  // Patch 0028: x VGT_MIN_VTX_INDX, y VGT_MAX_VTX_INDX (Canary RemapAndConvertVertexIndices).
  uint4 xe_sys2;
#endif
};
// The stage's 256 float constants (the device copies the SQ_VS_CONST / SQ_PS_CONST range) (b1 / set 0 binding 1).
XE_VK(1, 0) cbuffer XeFloatConstants : register(b1, space0) { float4 xe_c[256]; };
// 256 bool constants (8 dwords) and 32 loop constants (b2 / set 0 binding 2).
XE_VK(2, 0) cbuffer XeBoolLoopConstants : register(b2, space0) { uint4 xe_bool[2]; uint4 xe_loop[8]; };
// The fetch constant register file, 192 dwords: texture fetch constant k at dwords 6k-6k+5, vertex fetch constant v at
// dwords 2v-2v+1 (b3 / set 0 binding 3).
XE_VK(3, 0) cbuffer XeFetchConstants : register(b3, space0) { uint4 xe_fetch[48]; };

float4 xe_c_read(int i) { return (i >= 0 && i < 256) ? xe_c[i] : float4(0.0, 0.0, 0.0, 0.0); }
bool xe_bool_const(uint k) { return ((xe_bool[k >> 7][(k >> 5) & 3] >> (k & 31)) & 1) != 0; }
uint xe_loop_const(uint k) { return xe_loop[(k >> 2) & 7][k & 3]; }
uint xe_fetch_dw(uint j) { return xe_fetch[(j >> 2) % 48][j & 3]; }

// ---- Xenos maths rules (notes/38) ----
// Legacy multiply: if either factor is +-0 (or a denormal, flushed to 0 by -denorm ftz) the product is +0, even with
// Inf or NaN on the other side. Tested with == 0.0 on each factor (NaN compares unequal), not with min(abs(a), abs(b)):
// min/max with a NaN operand is defined in DXIL but undefined in SPIR-V (GLSL.std.450 FMin), job 091 triage.
float xe_mul1(float a, float b) { precise float p = a * b; precise float r = (a == 0.0 || b == 0.0) ? 0.0 : p; return r; }
float4 xe_mul(float4 a, float4 b) {
  precise float4 p = a * b;
  precise float4 r = select(or(a == 0.0, b == 0.0), float4(0.0, 0.0, 0.0, 0.0), p);
  return r;
}
// Direct3D 9 max/min order: max = a >= b ? a : b, min = a < b ? a : b (a NaN on either side returns `b`; -0 and +0
// compare equal, so max(-0, +0) = -0 and min(-0, +0) = +0). Done on the bits, in integers: the job 091 probe showed
// NVIDIA's DXIL compiler turning the float select `a < b ? a : b` into a NaN-dropping, zero-ordering fmin (min(x, NaN)
// gave x; with an isnan fix-up, min(-0, +0) still gave -0). Operands and result are flushed to a signed zero first,
// as the interpreter flushes every ALU operand (Xenos has no denormals, notes/38).
uint xe_ftz_bits(uint u) { return (u & 0x7F800000u) != 0u ? u : (u & 0x80000000u); }
bool xe_nan_bits(uint u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }
int xe_order_key(uint u) { int m = int(u & 0x7FFFFFFFu); return (u >> 31) != 0u ? -m : m; }  // -0 and +0 -> 0
uint4 xe_ftz_bits4(uint4 u) { return select((u & 0x7F800000u) != 0u, u, u & 0x80000000u); }
bool4 xe_nan_bits4(uint4 u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }
int4 xe_order_key4(uint4 u) { int4 m = int4(u & 0x7FFFFFFFu); return select((u >> 31) != 0u, -m, m); }
float xe_max1(float af, float bf) {
  uint a = xe_ftz_bits(asuint(af)), b = xe_ftz_bits(asuint(bf));
  bool ge = !xe_nan_bits(a) && !xe_nan_bits(b) && xe_order_key(a) >= xe_order_key(b);
  return asfloat(ge ? a : b);
}
float xe_min1(float af, float bf) {
  uint a = xe_ftz_bits(asuint(af)), b = xe_ftz_bits(asuint(bf));
  bool lt = !xe_nan_bits(a) && !xe_nan_bits(b) && xe_order_key(a) < xe_order_key(b);
  return asfloat(lt ? a : b);
}
float4 xe_max(float4 af, float4 bf) {
  uint4 a = xe_ftz_bits4(asuint(af)), b = xe_ftz_bits4(asuint(bf));
  bool4 ge = and(and(!xe_nan_bits4(a), !xe_nan_bits4(b)), xe_order_key4(a) >= xe_order_key4(b));
  return asfloat(select(ge, a, b));
}
float4 xe_min(float4 af, float4 bf) {
  uint4 a = xe_ftz_bits4(asuint(af)), b = xe_ftz_bits4(asuint(bf));
  bool4 lt = and(and(!xe_nan_bits4(a), !xe_nan_bits4(b)), xe_order_key4(a) < xe_order_key4(b));
  return asfloat(select(lt, a, b));
}
// Dot products: each product with the legacy rule, summed from +0 so that +0 + -0 stays +0.
float xe_dp4(float4 a, float4 b) {
  precise float4 m = xe_mul(a, b);
  precise float r = 0.0;
  r += m.x; r += m.y; r += m.z; r += m.w;
  return r;
}
float xe_dp3(float4 a, float4 b) {
  precise float4 m = xe_mul(a, b);
  precise float r = 0.0;
  r += m.x; r += m.y; r += m.z;
  return r;
}
float xe_dp2add(float4 a, float4 b, float4 c) {
  precise float4 m = xe_mul(a, b);
  precise float r = 0.0;
  r += m.x; r += m.y; r += c.x;
  return r;
}
// cube: T coordinate, S coordinate, 2 x major axis, face ID (D3DCUBEMAP_FACES); operand is .zzxy as the assembler
// emits it (z = a.x, x = a.z, y = a.w), as rexglue's interpreter reads it.
float4 xe_cube(float4 a) {
  float x = a.z, y = a.w, z = a.x;
  float ax = abs(x), ay = abs(y), az = abs(z);
  float4 r;
  if (az >= ax && az >= ay) {
    bool n = z < 0.0;
    r = float4(-y, n ? -x : x, z, n ? 5.0 : 4.0);
  } else if (ay >= ax) {
    bool n = y < 0.0;
    r = float4(n ? -z : z, x, y, n ? 3.0 : 2.0);
  } else {
    bool n = x < 0.0;
    r = float4(-y, n ? z : -z, x, n ? 1.0 : 0.0);
  }
  r.z *= 2.0;
  return r;
}
// max4 with rexglue's >= priority X, Y, Z, W (Canary's hardware notes suggest >; unresolved, notes/38).
// Integer compares as xe_max (a NaN compares false; result flushed like an interpreter operand).
bool xe_ge_bits(uint a, uint b) { return !xe_nan_bits(a) && !xe_nan_bits(b) && xe_order_key(a) >= xe_order_key(b); }
float xe_max4(float4 af) {
  uint4 a = xe_ftz_bits4(asuint(af));
  if (xe_ge_bits(a.x, a.y) && xe_ge_bits(a.x, a.z) && xe_ge_bits(a.x, a.w)) return asfloat(a.x);
  if (xe_ge_bits(a.y, a.z) && xe_ge_bits(a.y, a.w)) return asfloat(a.y);
  if (xe_ge_bits(a.z, a.w)) return asfloat(a.z);
  return asfloat(a.w);
}
// Scalar transcendentals per variant: plain keeps Inf, c clamps Inf to +-FLT_MAX, f sends Inf to +-0.
// The replacement keeps the sign bit of the Inf and is built from bits: the job 091 probe showed NVIDIA returning -0
// for rcpf(+0) from `v < 0.0 ? -0.0 : 0.0` (both constants treated as the same zero).
float xe_clamp_inf(float v) { return isinf(v) ? asfloat((asuint(v) & 0x80000000u) | 0x7F7FFFFFu) : v; }
float xe_zero_inf(float v) { return isinf(v) ? asfloat(asuint(v) & 0x80000000u) : v; }
// Patch 0037 (Canary 3a44f20, ShaderInterpreter::ReduceFloatPrecision): the scalar approximations (exp, log, logc,
// rcp*, rsq*, sqrt) give 21 mantissa bits - round to nearest at bit 2 (ties up), Inf / NaN kept, and a rounding that
// would make a finite result infinite truncates instead. Applied before the c / f Inf replacements, as Canary.
float xe_r21(float v) {
  uint b = asuint(v);
  if ((b & 0x7F800000u) == 0x7F800000u) return v;
  uint t = b & ~3u, u = t + 4u;
  if ((u & 0x7F800000u) == 0x7F800000u) u = t;
  return asfloat((b & 3u) >= 2u ? u : t);
}
float xe_rcp(float a) { precise float r = 1.0 / a; return xe_r21(r); }
float xe_rsq(float a) { precise float r = 1.0 / sqrt(a); return xe_r21(r); }
float xe_log(float a) { precise float r = log2(a); return xe_r21(r); }
float xe_logc(float a) { precise float r = xe_r21(log2(a)); return isinf(r) && r < 0.0 ? -XE_FLT_MAX : r; }
float xe_exp(float a) { precise float r = exp2(a); return xe_r21(r); }
float xe_sqrt(float a) { precise float r = sqrt(a); return xe_r21(r); }
float xe_muls_prev2(float a, float b, float ps) {
  if (ps == -XE_FLT_MAX || isinf(ps) || isnan(ps) || isinf(b) || isnan(b) || !(b > 0.0)) return -XE_FLT_MAX;
  return xe_mul1(a, ps);
}
// Defined float-to-int conversion (HLSL int() of NaN or of an out-of-range value is undefined in SPIR-V and differs
// between drivers): NaN -> 0, saturated to the int32 range (D3D ftoi); the interpreter does the same (SDK patch 10).
int xe_ftoi(float v) {
  if (isnan(v)) return 0;
  if (v >= 2147483648.0) return 2147483647;
  if (v <= -2147483648.0) return int(0x80000000u);
  return int(v);
}
// Result clamp (_sat): [0, 1] with NaN -> +0 and -0 -> +0, as the interpreter's rex::saturate; spelled out because
// saturate() of NaN is defined in DXIL but undefined in SPIR-V (FClamp).
// Integer compares, so no driver can turn it into a NaN-keeping min/max (job 091: NVIDIA did that to max and min).
float xe_sat(float v) {
  uint u = xe_ftz_bits(asuint(v));
  if (xe_nan_bits(u) || xe_order_key(u) <= 0) return 0.0;
  return xe_order_key(u) < 0x3F800000 ? asfloat(u) : 1.0;
}
float4 xe_sat4(float4 v) { return float4(xe_sat(v.x), xe_sat(v.y), xe_sat(v.z), xe_sat(v.w)); }
// a0 = the value clamped to [-256, 255] (NaN -> -256, as the interpreter's clamp_float), rounded or floored. The clamp
// is spelled out with comparisons: clamp()/min()/max() with NaN are undefined in SPIR-V.
float xe_a0_clamp(float v) { return v > -256.0 ? (v < 255.0 ? v : 255.0) : -256.0; }
int xe_a0_round(float v) { return int(floor(xe_a0_clamp(v) + 0.5)); }
int xe_a0_floor(float v) { return int(floor(xe_a0_clamp(v))); }

// ---- Fetch helpers ----
uint xe_swap(uint v, uint e) {
  if (e == 1 || e == 2) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  if (e == 2 || e == 3) v = (v << 16) | (v >> 16);
  return v;
}
// Normalisation multiplies by the float reciprocal 1/(2^n-1) (and adds it, unfused, for the no-zero mode), as Canary's
// GPU translators do. A division is not used: GPU division is not correctly rounded (NVIDIA's x / 255 gave the result
// of x * (1/255), 1 ulp off the C++ division, which cancellation in skinning then showed; job 091). The interpreter
// does the same since SDK patch 12. The constants are bit patterns, so no compiler rounds them differently.
float xe_unorm_recip(uint m) {
  switch (m) {
    case 1u: return 1.0;
    case 3u: return asfloat(0x3EAAAAABu);
    case 127u: return asfloat(0x3C010204u);
    case 255u: return asfloat(0x3B808081u);
    case 511u: return asfloat(0x3B004020u);
    case 1023u: return asfloat(0x3A802008u);
    case 2047u: return asfloat(0x3A001002u);
    case 32767u: return asfloat(0x38000100u);
    case 65535u: return asfloat(0x37800080u);
    default: { precise float r = 1.0 / float(m); return r; }  // no packed vertex format uses another width
  }
}
float xe_unpack_packed(uint w, uint width, uint offset, bool sgn, bool norm, bool nozero) {
  precise float v;
  if (sgn) {
    v = float(int(w << (32 - (width + offset))) >> (32 - width));
    if (norm) {
      if (nozero) {
        float z = xe_unorm_recip((1u << width) - 1);
        precise float p = v * (2.0 * z);
        v = p + z;
      } else {
        precise float p = v * xe_unorm_recip((1u << (width - 1)) - 1);
        v = p > -1.0 ? p : -1.0;  // -2^(n-1) and -(2^(n-1)-1) both -1 (p is never NaN)
      }
    }
  } else {
    v = float((w >> offset) & ((1u << width) - 1));
    if (norm) v = v * xe_unorm_recip((1u << width) - 1);
  }
  return v;
}
float xe_unpack_32(uint w, bool sgn, bool norm, bool nozero) {
  if (sgn) {
    precise float v = float(int(w));
    if (!norm) return v;
    if (nozero) {
      precise float p = v * asfloat(0x30000000u);  // 1 / 2147483647.5 as a float = 2^-31
      precise float r = p + asfloat(0x2F800000u);  // 0.5 / 2147483647.5 = 2^-32
      return r;
    }
    precise float r = v * asfloat(0x30000000u);  // 1 / 2147483647 as a float = 2^-31
    return r;
  }
  precise float v = float(w);
  if (!norm) return v;
  precise float r = v * asfloat(0x2F800000u);  // 1 / 4294967295 as a float = 2^-32
  return r;
}
// The 360's 16-bit float ("extended range" half): IEEE half layout, but exponent 31 is an ordinary exponent (no Inf or
// NaN, maximum 131,008) and denormals are flushed to a signed zero - exactly rexglue's xenos_half_to_float (math.h),
// which Canary's GPU translators do not follow for vertex fetch (they use IEEE halves; notes/38).
float xe_half_ext(uint h) {
  uint e = (h >> 10) & 31u;
  uint bits = (h & 0x8000u) << 16;
  if (e != 0u) bits |= ((e + 112u) << 23) | ((h & 1023u) << 13);
  return asfloat(bits);
}
// Vertex fetch data formats (xenos::VertexFormat). Components a format does not have read as 0 (as Xenia).
float4 xe_vf_unpack(uint fmt, uint4 w, bool sgn, bool norm, bool nozero) {
  switch (fmt) {
    case 6:  return float4(xe_unpack_packed(w.x, 8, 0, sgn, norm, nozero), xe_unpack_packed(w.x, 8, 8, sgn, norm, nozero),
                           xe_unpack_packed(w.x, 8, 16, sgn, norm, nozero), xe_unpack_packed(w.x, 8, 24, sgn, norm, nozero));
    case 7:  return float4(xe_unpack_packed(w.x, 10, 0, sgn, norm, nozero), xe_unpack_packed(w.x, 10, 10, sgn, norm, nozero),
                           xe_unpack_packed(w.x, 10, 20, sgn, norm, nozero), xe_unpack_packed(w.x, 2, 30, sgn, norm, nozero));
    case 16: return float4(xe_unpack_packed(w.x, 11, 0, sgn, norm, nozero), xe_unpack_packed(w.x, 11, 11, sgn, norm, nozero),
                           xe_unpack_packed(w.x, 10, 22, sgn, norm, nozero), 0.0);
    case 17: return float4(xe_unpack_packed(w.x, 10, 0, sgn, norm, nozero), xe_unpack_packed(w.x, 11, 10, sgn, norm, nozero),
                           xe_unpack_packed(w.x, 11, 21, sgn, norm, nozero), 0.0);
    case 25: return float4(xe_unpack_packed(w.x, 16, 0, sgn, norm, nozero), xe_unpack_packed(w.x, 16, 16, sgn, norm, nozero), 0.0, 0.0);
    case 26: return float4(xe_unpack_packed(w.x, 16, 0, sgn, norm, nozero), xe_unpack_packed(w.x, 16, 16, sgn, norm, nozero),
                           xe_unpack_packed(w.y, 16, 0, sgn, norm, nozero), xe_unpack_packed(w.y, 16, 16, sgn, norm, nozero));
    // 16-bit floats in the 360's extended-range half (xe_half_ext), as rexglue's interpreter decodes them.
    case 31: return float4(xe_half_ext(w.x & 0xFFFFu), xe_half_ext(w.x >> 16), 0.0, 0.0);
    case 32: return float4(xe_half_ext(w.x & 0xFFFFu), xe_half_ext(w.x >> 16), xe_half_ext(w.y & 0xFFFFu), xe_half_ext(w.y >> 16));
    case 33: return float4(xe_unpack_32(w.x, sgn, norm, nozero), 0.0, 0.0, 0.0);
    case 34: return float4(xe_unpack_32(w.x, sgn, norm, nozero), xe_unpack_32(w.y, sgn, norm, nozero), 0.0, 0.0);
    case 35: return float4(xe_unpack_32(w.x, sgn, norm, nozero), xe_unpack_32(w.y, sgn, norm, nozero),
                           xe_unpack_32(w.z, sgn, norm, nozero), xe_unpack_32(w.w, sgn, norm, nozero));
    case 36: return float4(asfloat(w.x), 0.0, 0.0, 0.0);
    case 37: return float4(asfloat(w.x), asfloat(w.y), 0.0, 0.0);
    case 38: return asfloat(w);
    case 57: return float4(asfloat(w.x), asfloat(w.y), asfloat(w.z), 0.0);
    default: return float4(0.0, 0.0, 0.0, 0.0);
  }
}
// Piecewise-linear 360 gamma (8-bit, 255 units) to linear (1023 units), Canary xenos.cc segments without its truncation
// steps (a simplification, notes/39).
float xe_degamma1(float g) {
  float x = xe_sat(g) * 255.0;
  float l = x < 64.0 ? x : (x < 96.0 ? 64.0 + (x - 64.0) * 2.0 : (x < 192.0 ? 128.0 + (x - 96.0) * 4.0 : 512.0 + (x - 192.0) * 8.0));
  return l / 1023.0;
}
// After a texture fetch: the fetch constant's per-component sign mode (0 unsigned, 1 signed - the device binds a signed
// view, 2 unsigned biased 2x-1, 3 gamma) and its exponent adjust (dword 3 bits 13-18).
#ifdef XE_TEX_NUM_FORMAT
// Patch 0040 (Canary d119505 / 2ddc5ef / 6a45452 / 0c843ef and later, as of 534aeb6): the component widths of the
// fixed-point formats (texture_info_formats.inl); zero for the rest (compressed colour, float, 32-bit, YUV, 1-bit).
uint4 xe_fixed_bits(uint fmt) {
  switch (fmt) {
    case 2: case 8: case 9: case 58: case 59: return uint4(8, 0, 0, 0);
    case 3: return uint4(5, 5, 5, 1);
    case 4: return uint4(5, 6, 5, 0);
    case 5: return uint4(5, 5, 6, 0);
    case 6: case 14: case 50: case 62: return uint4(8, 8, 8, 8);
    case 7: case 54: return uint4(10, 10, 10, 2);
    case 10: case 49: case 60: return uint4(8, 8, 0, 0);
    case 13: case 25: return uint4(16, 16, 0, 0);
    case 15: return uint4(4, 4, 4, 4);
    case 16: case 55: return uint4(11, 11, 10, 0);
    case 17: case 56: return uint4(10, 11, 11, 0);
    case 21: case 26: return uint4(16, 16, 16, 16);
    case 24: return uint4(16, 0, 0, 0);
    default: return uint4(0, 0, 0, 0);
  }
}
#endif
float4 xe_tex_post(float4 v, uint slot) {
  uint dw0 = xe_fetch_dw(slot * 6), dw3 = xe_fetch_dw(slot * 6 + 3);
#ifdef XE_TEX_NUM_FORMAT
  // Canary TextureCache::GetIntegerScaleBits, worked out here from the fetch constant: per component the sign mode and,
  // when it carries one, the width (0 = none). Point sampled = point mag / min, point or base-map mip, no anisotropy
  // (dword 3 bits 19-27; the renderer samples with the fetch constant's filters, so this is the host's filtering too).
  uint4 fb = xe_fixed_bits(xe_fetch_dw(slot * 6 + 1) & 63u);
  bool fixed = any(fb != 0u), integer = (dw3 & 1u) != 0u;
  uint mipf = (dw3 >> 23) & 3u;
  bool pt = ((dw3 >> 19) & 15u) == 0u && (mipf == 0u || mipf == 2u) && ((dw3 >> 25) & 7u) == 0u;
  uint last = fb.w != 0u ? 3u : (fb.z != 0u ? 2u : (fb.y != 0u ? 1u : 0u));
  uint4 sg = uint4(0, 0, 0, 0), wd = uint4(0, 0, 0, 0);
#endif
  [unroll] for (uint i = 0; i < 4; ++i) {
#ifdef XE_SIGN_BY_SWIZZLE
    // Patch 0024 (Canary): the sign mode of the source component the fetch swizzle (dword 3 bits 1-12) picked; constant
    // 0 / 1 components (swizzle 4 / 5) get none.
    uint sw = (dw3 >> (1 + 3 * i)) & 7;
    uint s = sw < 4 ? (dw0 >> (2 + 2 * sw)) & 3 : 0;
#else
    uint sw = i;
    uint s = (dw0 >> (2 + 2 * i)) & 3;
#endif
#ifdef XE_TEX_NUM_FORMAT
    uint w = sw < 4 && fixed ? fb[min(sw, last)] : 0u;
    bool carries = w != 0u && w <= 16u;
    if (s == 3) carries = false;
    else if (!integer && s == 0) carries = carries && pt && w >= 4u && w <= 6u;
    sg[i] = sw < 4 && fixed && w != 0u && w <= 16u && !(s == 3 && integer) ? s : 0u;
    wd[i] = carries ? w : 0u;
    if (s == 2) {
      // Signed offset binary, (n - 2^(w-1)) / (2^(w-1) - 1): 128 is zero for 8 bits, not 2u-1's 1/255 off; 2u-1 when the
      // width is unknown or 1.
      if (wd[i] >= 2u) {
        float h = float(1u << (wd[i] - 1u));
        v[i] = (v[i] * (2.0 * h - 1.0) - h) / (h - 1.0);
      } else {
        v[i] = v[i] * 2.0 - 1.0;
      }
    }
#else
    if (s == 2) v[i] = v[i] * 2.0 - 1.0;
#endif
    else if (s == 3) v[i] = xe_degamma1(v[i]);
  }
#ifdef XE_TEX_NUM_FORMAT
  if (fixed) {
    if (!integer) {
      [unroll] for (uint i = 0; i < 4; ++i) {
        if (sg[i] != 0u) {
          // Unsigned-biased: clamped to -1 (filtering after the host's per-texel clamp can land a code below).
          if (sg[i] == 2u) v[i] = max(v[i], -1.0);
          continue;
        }
        // Point sampled 4-6 bit unsigned: the guest's n * (2^w + 1) / 2^(2w), not the host's n / (2^w - 1).
        if (wd[i] != 0u) {
          float e = float(1u << wd[i]);
          float n = round(v[i] * (e - 1.0));
          v[i] = n * (e + 1.0) / (e * e);
        }
        // Unsigned normalised: 16 fractional bits (0c843ef; 4D5309C9 / 4D530AA4 compare filtered samples to Q16).
        v[i] = round(v[i] * 65536.0) / 65536.0;
      }
    } else {
      // Integer num_format: the guest integer, 2^w - 1 for unsigned and 2^(w-1) - 1 for signed and unsigned-biased;
      // a 1-bit biased component maps to -1 / 0 (scale 0.5, offset -0.5). Point sampled: rounded to the integer.
      [unroll] for (uint i = 0; i < 4; ++i) {
        uint wf = wd[i] != 0u ? wd[i] : 1u;
        uint shift = (sg[i] == 1u || sg[i] == 2u) ? wf - 1u : wf;
        float sc = float((1u << shift) - 1u);
        v[i] = v[i] * max(sc, 0.5) + (min(sc, 0.5) - 0.5);
        if (pt) v[i] = round(v[i]);
      }
    }
  }
#endif
  int ea = int(dw3 << 13) >> 26;
  return v * exp2(float(ea));
}
// tfetchCube coordinates (S/TC in 1..2 plus the face) back to a direction, as Xenia's DXBC translator does.
float3 xe_cube_dir(float3 c) {
  float sc = c.x * 2.0 - 3.0, tc = c.y * 2.0 - 3.0;
  uint face = min(uint(max(c.z, 0.0)), 5u);
  bool n = (face & 1) != 0;
  uint axis = face >> 1;
  if (axis == 0) return float3(n ? -1.0 : 1.0, -tc, n ? sc : -sc);
  if (axis == 1) return float3(sc, n ? -1.0 : 1.0, n ? -tc : tc);
  return float3(n ? -sc : sc, -tc, n ? -1.0 : 1.0);
}
// Xenos aL: iterator * step + start, clamped to [-256, 256] (IPR2015-00325, as rexglue's interpreter).
int xe_al(uint iter, uint lc) { return clamp(int(iter) * (int(lc << 8) >> 24) + int((lc >> 8) & 0xFF), -256, 256); }
)";

// Memory export (eA / eM#), Gears 3 2026-10-22 (notes/11). Ported from the SDK's DXBC translator
// (dxbc_translator_memexport.cpp, Xenia - Copyright Ben Vanik, BSD; memexport by Triang3l), same rules: eA is
// index * (0,1,0,0) + the stream constant (dword 0: base address >> 2 | 1 << 30; 1: 0x4B000000 (2^23, the index lands
// in the mantissa); 2: endian [2:0], format [13:8], signed [16], integer [17], red/blue swap [19], 0x4B0 << 20;
// 3: index count | 0x96 << 23). Up to five eM# per eA write elements index + #. Out-of-range elements are dropped.
// The guest-memory buffer is written through u0 (bound for exporting draws only).
constexpr const char* kPreludeMemexport = R"(
static float4 xe_eA = float4(0.0, 0.0, 0.0, 0.0);
static float4 xe_eM[5] = {float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0),
                          float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0)};
static uint xe_eM_written = 0u;
// One fixed-point component: integer formats clamp to the range, normalised ones scale (NaN -> 0), then round half
// away from zero (Direct3D's conversion rule) and truncate.
uint xe_mx_fixed(float v, uint width, bool is_signed, bool is_integer) {
  if (width == 0u) return 0u;
  float r;
  if (is_signed) {
    float hi = float((1u << (width - 1u)) - 1u);
    if (is_integer) {
      r = clamp(v, -hi - 1.0, hi);
    } else {
      r = (v != v) ? 0.0 : clamp(v, -1.0, 1.0) * hi;
    }
    r += r < 0.0 ? -0.5 : 0.5;
    return uint(int(r)) & ((width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u));
  }
  float mx = width >= 32u ? 4294967295.0 : float((1u << width) - 1u);
  r = is_integer ? clamp(v, 0.0, mx) : ((v != v) ? 0.0 : saturate(v) * mx);
  return uint(r + 0.5);
}
// Patch 0044 (Canary 02c6bc6e6): the 360's float16 is extended-range - exponent 31 is a normal number (up to +-131008),
// not Inf / NaN. Where the host conversion gives exponent 31, halve the value (clamped to +-131008), convert, and add
// one to the exponent instead.
uint xe_f16x(float v) {
  uint h = f32tof16(v);
  if ((h & 0x7C00u) != 0x7C00u) return h;
  return f32tof16(clamp(v, -131008.0, 131008.0) * 0.5) + 0x0400u;
}
uint xe_mx_pack(float4 v, uint4 w, bool sg, bool it) {
  uint o = xe_mx_fixed(v.x, w.x, sg, it);
  o |= xe_mx_fixed(v.y, w.y, sg, it) << w.x;
  o |= xe_mx_fixed(v.z, w.z, sg, it) << (w.x + w.y);
  if (w.w != 0u) o |= xe_mx_fixed(v.w, w.w, sg, it) << (w.x + w.y + w.z);
  return o;
}
void xe_memexport_flush() {
  uint written = xe_eM_written;
  xe_eM_written = 0u;
  // System flag 128: the renderer allows exports for this draw (--gow2_renderer_memexport); otherwise nothing is written.
  if (written == 0u || (xe_sys0.x & 128u) == 0u) return;
  uint4 a = asuint(xe_eA);
  if (any((a >> uint4(30u, 23u, 23u, 23u)) != uint4(1u, 0x96u, 0x96u, 0x96u))) return;
  uint fmt = (a.z >> 8u) & 63u, endian = a.z & 7u;
  bool sg = ((a.z >> 16u) & 1u) != 0u, it = ((a.z >> 17u) & 1u) != 0u, rb = ((a.z >> 19u) & 1u) != 0u;
  uint base = ((a.x & 0x3FFFFFFFu) << 2u) & 0x1FFFFFFFu, index = a.y & 0x7FFFFFu, count = a.w & 0x7FFFFFu;  // 0024
  [unroll] for (uint i = 0u; i < 5u; ++i) {
    if (((written >> i) & 1u) == 0u) continue;
    uint idx = index + i;
    if (idx >= count) continue;
    float4 v = xe_eM[i];
    if (rb) v = v.zyxw;
    uint4 d = uint4(0u, 0u, 0u, 0u);
    uint size_log2 = 0xFFu;
    switch (fmt) {
      case 2u: case 8u: case 9u: d.x = xe_mx_pack(v, uint4(8u, 0u, 0u, 0u), sg, it); size_log2 = 0u; break;
      case 3u: d.x = xe_mx_pack(v, uint4(5u, 5u, 5u, 1u), sg, it); size_log2 = 1u; break;
      case 4u: d.x = xe_mx_pack(v, uint4(5u, 6u, 5u, 0u), sg, it); size_log2 = 1u; break;
      case 5u: d.x = xe_mx_pack(v, uint4(5u, 5u, 6u, 0u), sg, it); size_log2 = 1u; break;
      case 6u: case 14u: case 50u: d.x = xe_mx_pack(v, uint4(8u, 8u, 8u, 8u), sg, it); size_log2 = 2u; break;
      case 7u: case 54u: d.x = xe_mx_pack(v, uint4(10u, 10u, 10u, 2u), sg, it); size_log2 = 2u; break;
      case 10u: d.x = xe_mx_pack(v, uint4(8u, 8u, 0u, 0u), sg, it); size_log2 = 1u; break;
      case 15u: d.x = xe_mx_pack(v, uint4(4u, 4u, 4u, 4u), sg, it); size_log2 = 1u; break;
      case 16u: case 55u: d.x = xe_mx_pack(v, uint4(11u, 11u, 10u, 0u), sg, it); size_log2 = 2u; break;
      case 17u: case 56u: d.x = xe_mx_pack(v, uint4(10u, 11u, 11u, 0u), sg, it); size_log2 = 2u; break;
      case 24u: d.x = xe_mx_pack(v, uint4(16u, 0u, 0u, 0u), sg, it); size_log2 = 1u; break;
      case 25u: d.x = xe_mx_pack(v, uint4(16u, 16u, 0u, 0u), sg, it); size_log2 = 2u; break;
      case 26u:
        d.x = xe_mx_pack(v.xyxy, uint4(16u, 16u, 0u, 0u), sg, it);
        d.y = xe_mx_pack(v.zwzw, uint4(16u, 16u, 0u, 0u), sg, it);
        size_log2 = 3u;
        break;
      case 30u: d.x = xe_f16x(v.x); size_log2 = 1u; break;
      case 31u: d.x = xe_f16x(v.x) | (xe_f16x(v.y) << 16u); size_log2 = 2u; break;
      case 32u:
        d.x = xe_f16x(v.x) | (xe_f16x(v.y) << 16u);
        d.y = xe_f16x(v.z) | (xe_f16x(v.w) << 16u);
        size_log2 = 3u;
        break;
      case 36u: d.x = asuint(v.x); size_log2 = 2u; break;
      case 37u: d.xy = asuint(v.xy); size_log2 = 3u; break;
      case 38u: d = asuint(v); size_log2 = 4u; break;
      default: break;
    }
    if (size_log2 == 0xFFu) continue;
    // 8-in-64 / 8-in-128: reorder the dwords, then swap bytes within each dword.
    uint e = endian;
    if (e == 4u) { d = d.yxwz; e = 2u; } else if (e == 5u) { d = d.wzyx; e = 2u; }
    d = uint4(xe_swap(d.x, e), xe_swap(d.y, e), xe_swap(d.z, e), xe_swap(d.w, e));
    uint addr = base + (idx << size_log2);
    if (addr + (1u << size_log2) > 0x20000000u) continue;  // patch 0024: never past the 512 MB buffer
    if (size_log2 <= 1u) {
      uint shift = (addr & 3u) * 8u, width = 8u << size_log2;
      uint mask = ((1u << width) - 1u) << shift, dword_addr = addr & ~3u;
      xe_memexport_buffer.InterlockedAnd(dword_addr, ~mask);
      xe_memexport_buffer.InterlockedOr(dword_addr, (d.x << shift) & mask);
    } else if (size_log2 == 2u) {
      xe_memexport_buffer.Store(addr, d.x);
    } else if (size_log2 == 3u) {
      xe_memexport_buffer.Store2(addr, d.xy);
    } else {
      xe_memexport_buffer.Store4(addr, d);
    }
  }
}
)";

constexpr const char* kPreludeVertex = R"(
// Guest physical memory for vertex fetch (t96 / set 1 binding 96).
XE_VK(96, 1) ByteAddressBuffer xe_shared_memory : register(t96, space0);
// The upload ring (t97): fetch constants the renderer pointed there (bit 31 of dword 1, unused by the hardware) read
// their vertex data from it, the address field holding the byte offset in the ring.
XE_VK(97, 1) ByteAddressBuffer xe_upload_memory : register(t97, space0);
// One vertex word at a dword address: 0 outside the fetch constant's buffer [address, address + size) (the hardware
// clamps; Canary and the interpreter return 0), otherwise loaded and endian-swapped.
uint xe_vf_word(uint dw, uint fdw0, uint fdw1) {
  uint base = fdw0 >> 2, size = (fdw1 >> 2) & 0xFFFFFFu;
  if (!(dw >= base && dw - base < size)) return 0u;
  // Patch 0024 (Canary 0x1FFFFFFC masks; Gears 3 review 05-j): the guest-memory buffer is a root view with no bounds
  // check - an address past 512 MB would be a GPU page fault (device removed). The ring path stays as is.
  // Patch 0029 (Gears 3 bug hunt B1): the ring flag (dw1 bit 31) counts only on a vertex-type constant our renderer
  // rewrote (type 3), so a stray bit 31 never sends a guest address into the ring view.
  bool xe_ring = (fdw1 & 0x80000000u) != 0u && (fdw0 & 3u) == 3u;
  if (!xe_ring) dw &= 0x07FFFFFFu;
#ifdef XE_MEMEXPORT
  // An exporting draw has the guest-memory buffer in UAV state: its own vertex data is read through u0 too.
  uint raw = xe_ring ? xe_upload_memory.Load(dw * 4u) : xe_memexport_buffer.Load(dw * 4u);
#else
  uint raw = xe_ring ? xe_upload_memory.Load(dw * 4u) : xe_shared_memory.Load(dw * 4u);
#endif
  return xe_swap(raw, fdw1 & 3u);
}
struct XeVsOut {
  float4 interp[16] : TEXCOORD0;
  float4 position : SV_Position;
};
)";

constexpr const char* kPreludePixel = R"(
struct XePsIn {
  float4 interp[16] : TEXCOORD0;
  float4 position : SV_Position;
  bool front_face : SV_IsFrontFace;
};
struct XePsOut {
  float4 color0 : SV_Target0;
  float4 color1 : SV_Target1;
  float4 color2 : SV_Target2;
  float4 color3 : SV_Target3;
#ifdef XE_WRITES_DEPTH
  float depth : SV_Depth;
#endif
};
bool xe_alpha_test(float a) {
  if ((xe_sys0.x & 32u) == 0) return true;
  float ref = asfloat(xe_sys1.y);
  switch (xe_sys1.x & 7u) {
    case 0: return false;
    case 1: return a < ref;
    case 2: return a == ref;
    case 3: return a <= ref;
    case 4: return a > ref;
    case 5: return !(a == ref);  // NaN: not equal (DXC -spirv compiles != as ordered)
    case 6: return a >= ref;
    default: return true;
  }
}
)";

// ---- Code generation ----------------------------------------------------------------------------------------------

const char* kComp = "xyzw";

struct Gen {
  Gen(const Program& program, const HlslOptions& options, Translation& translation)
      : p(program), opt(options), out(translation) {}
  const Program& p;
  const HlslOptions& opt;
  Translation& out;
  std::string body;
  int indent = 1;
  uint32_t reg_count = 1;
  bool dynamic_regs = false;
  bool has_loops = false;
  mutable uint32_t float_constants = 0;  // 1 + highest constant read; 256 once any read is relative
  mutable uint64_t constants_read[4] = {};  // bit per absolute constant index read (compaction, HlslOptions)
  mutable bool constants_relative = false;  // any a0/aL-relative constant read

  void Line(const std::string& s) {
    body.append(size_t(indent) * 2, ' ');
    body += s;
    body += '\n';
  }
  void Simplify(const char* feature, const std::string& note) {
    for (const auto& s : out.simplifications)
      if (s.feature == feature) return;  // one entry per feature per shader
    out.simplifications.push_back({feature, note});
  }

  void NoteConstant(const Operand& op) const {
    if (op.addressing != Addressing::kAbsolute) {
      float_constants = 256;
      constants_relative = true;
    } else {
      float_constants = std::max(float_constants, std::min<uint32_t>(op.index + 1, 256));
      if (op.index < 256) constants_read[op.index >> 6] |= 1ull << (op.index & 63);
    }
  }
  // Register access (aL-relative when asked).
  std::string Reg(uint32_t index, Addressing addressing) const {
    if (addressing == Addressing::kLoopRelative) return std::format("r[(uint({}u) + uint(xe_aL)) & 63u]", index);
    return std::format("r[{}]", index);
  }
  std::string Operand4(const Operand& op) const {
    std::string base;
    if (op.source == Source::kRegister) {
      base = Reg(op.index, op.addressing);
    } else {
      if (op.addressing == Addressing::kAddressRegisterRelative) base = std::format("xe_c_read({} + a0)", op.index);
      else if (op.addressing == Addressing::kLoopRelative) base = std::format("xe_c_read({} + xe_aL)", op.index);
      else base = std::format("xe_c[{}]", op.index);
      NoteConstant(op);
    }
    std::string sw;
    for (uint32_t i = 0; i < 4; ++i) sw += kComp[uint32_t(op.components[std::min(i, op.component_count - 1)]) & 3];
    std::string e = base + "." + sw;
    if (op.absolute) e = "abs(" + e + ")";
    if (op.negate) e = "-(" + e + ")";
    return e;
  }
  // Component j of a scalar operand.
  std::string Operand1(const Operand& op, uint32_t j) const {
    std::string base;
    if (op.source == Source::kRegister) {
      base = Reg(op.index, op.addressing);
    } else {
      if (op.addressing == Addressing::kAddressRegisterRelative) base = std::format("xe_c_read({} + a0)", op.index);
      else if (op.addressing == Addressing::kLoopRelative) base = std::format("xe_c_read({} + xe_aL)", op.index);
      else base = std::format("xe_c[{}]", op.index);
      NoteConstant(op);
    }
    std::string e = base + "." + kComp[uint32_t(op.components[j]) & 3];
    if (op.absolute) e = "abs(" + e + ")";
    if (op.negate) e = "-(" + e + ")";
    return e;
  }

  void NoteRegister(uint32_t index, Addressing addressing) {
    if (addressing == Addressing::kLoopRelative) dynamic_regs = true;
    reg_count = std::max(reg_count, index + 1);
  }

  // Writes value (a float4 expression named `v`) through a fetch-style result (components with 0/1, write mask).
  void StoreFetch(const Result& r, const std::string& v) {
    if (!r.write_mask) return;
    const std::string dst = Reg(r.index, r.addressing);
    std::string comps, src;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(r.write_mask & (1u << i))) continue;
      comps += kComp[i];
      const Swizzle s = r.components[i];
      Line(std::format("{}.{} = {};", dst, kComp[i],
                       s == Swizzle::k0 ? std::string("0.0") : s == Swizzle::k1 ? std::string("1.0") : v + "." + kComp[uint32_t(s)]));
    }
  }

  std::string ExportTarget(const Result& r) {
    switch (r.target) {
      case Target::kInterpolator: return std::format("xe_o[{}]", r.index);
      case Target::kPosition: return "xe_opos";
      case Target::kPointSizeEdgeFlagKillVertex:
        Simplify("point_size_export", "oPts written; point size / edge flag / kill vertex are dropped");
        return "xe_opts";
      case Target::kColor: return std::format("xe_oc[{}]", r.index);
      case Target::kDepth: return "xe_odepth";
      case Target::kExportAddress:
        memexport = true;
        return "xe_eA";
      case Target::kExportData:
        memexport = true;
        return r.index < 5 ? std::format("xe_eM[{}]", r.index) : std::string("xe_oexport");
      default: return "xe_oexport";
    }
  }

  // ---- ALU ----
  static bool VectorChangesState(AluVectorOpcode op) {
    return (op >= AluVectorOpcode::kSetpEqPush && op <= AluVectorOpcode::kKillNe) || op == AluVectorOpcode::kMaxA;
  }

  void EmitAlu(const Alu& a) {
    const bool vec_writes = a.vector_result.write_mask != 0;
    const bool do_vector = !a.IsVectorOpDefaultNop() && (vec_writes || VectorChangesState(a.vector_opcode));
    const bool do_scalar = a.scalar_opcode != AluScalarOpcode::kRetainPrev || a.scalar_result.write_mask;
    if (!do_vector && !do_scalar && !vec_writes) return;
    if (a.predicated) {
      Line(std::format("if (p0 == {}) {{", a.predicate_condition ? "true" : "false"));
      ++indent;
    }
    Line("{");
    ++indent;
    for (uint32_t i = 0; i < a.vector_operand_count; ++i) {
      if (a.vector_operands[i].source == Source::kRegister) NoteRegister(a.vector_operands[i].index, a.vector_operands[i].addressing);
    }
    for (uint32_t i = 0; i < a.scalar_operand_count; ++i) {
      if (a.scalar_operands[i].source == Source::kRegister) NoteRegister(a.scalar_operands[i].index, a.scalar_operands[i].addressing);
    }
    // Read every source first: both operations see the registers as they were before this slot.
    std::string vr = "float4(0.0, 0.0, 0.0, 0.0)";
    if (do_vector) {
      for (uint32_t i = 0; i < a.vector_operand_count; ++i) Line(std::format("float4 s{} = {};", i, Operand4(a.vector_operands[i])));
    }
    std::string sa, sb;
    if (do_scalar && a.scalar_operand_count == 1) {
      sa = Operand1(a.scalar_operands[0], 0);
      if (a.scalar_operands[0].component_count == 2) sb = Operand1(a.scalar_operands[0], 1);
    } else if (do_scalar && a.scalar_operand_count == 2) {
      sa = Operand1(a.scalar_operands[0], 0);
      sb = Operand1(a.scalar_operands[1], 0);
    }
    if (!sa.empty()) Line("float sa = " + sa + ";");
    if (!sb.empty()) Line("float sb = " + sb + ";");

    if (do_vector) {
      std::string e;
      bool replicate = false;
      switch (a.vector_opcode) {
        case AluVectorOpcode::kAdd: e = "s0 + s1"; break;
        case AluVectorOpcode::kMul: e = "xe_mul(s0, s1)"; break;
        case AluVectorOpcode::kMax: e = "xe_max(s0, s1)"; break;
        case AluVectorOpcode::kMin: e = "xe_min(s0, s1)"; break;
        case AluVectorOpcode::kSeq: e = "float4(s0 == s1)"; break;
        case AluVectorOpcode::kSgt: e = "float4(s0 > s1)"; break;
        case AluVectorOpcode::kSge: e = "float4(s0 >= s1)"; break;
        case AluVectorOpcode::kSne: e = "float4(!(s0 == s1))"; break;  // != is ordered in DXC -spirv
        case AluVectorOpcode::kFrc: e = "s0 - floor(s0)"; break;
        case AluVectorOpcode::kTrunc: e = "trunc(s0)"; break;
        case AluVectorOpcode::kFloor: e = "floor(s0)"; break;
        // Not fused: the legacy product, then the add (as Canary and rexglue).
        case AluVectorOpcode::kMad: e = "xe_mul(s0, s1) + s2"; break;
        case AluVectorOpcode::kCndEq: e = "select(s0 == 0.0, s1, s2)"; break;
        case AluVectorOpcode::kCndGe: e = "select(s0 >= 0.0, s1, s2)"; break;
        case AluVectorOpcode::kCndGt: e = "select(s0 > 0.0, s1, s2)"; break;
        case AluVectorOpcode::kDp4: e = "xe_dp4(s0, s1).xxxx"; break;
        case AluVectorOpcode::kDp3: e = "xe_dp3(s0, s1).xxxx"; break;
        case AluVectorOpcode::kDp2Add: e = "xe_dp2add(s0, s1, s2).xxxx"; break;
        case AluVectorOpcode::kCube: e = "xe_cube(s0)"; break;
        case AluVectorOpcode::kMax4:
          Simplify("max4_order", "max4 uses rexglue's >= priority; the hardware order is unresolved (notes/38)");
          e = "xe_max4(s0).xxxx";
          break;
        case AluVectorOpcode::kSetpEqPush:
          Line("p0 = s0.w == 0.0 && s1.w == 0.0;");
          e = "((s0.x == 0.0 && s1.x == 0.0) ? 0.0 : s0.x + 1.0).xxxx";
          break;
        case AluVectorOpcode::kSetpNePush:
          Line("p0 = s0.w == 0.0 && !(s1.w == 0.0);");
          e = "((s0.x == 0.0 && !(s1.x == 0.0)) ? 0.0 : s0.x + 1.0).xxxx";
          break;
        case AluVectorOpcode::kSetpGtPush:
          Line("p0 = s0.w == 0.0 && s1.w > 0.0;");
          e = "((s0.x == 0.0 && s1.x > 0.0) ? 0.0 : s0.x + 1.0).xxxx";
          break;
        case AluVectorOpcode::kSetpGePush:
          Line("p0 = s0.w == 0.0 && s1.w >= 0.0;");
          e = "((s0.x == 0.0 && s1.x >= 0.0) ? 0.0 : s0.x + 1.0).xxxx";
          break;
        case AluVectorOpcode::kKillEq: e = "float(any(s0 == s1)).xxxx"; break;
        case AluVectorOpcode::kKillGt: e = "float(any(s0 > s1)).xxxx"; break;
        case AluVectorOpcode::kKillGe: e = "float(any(s0 >= s1)).xxxx"; break;
        case AluVectorOpcode::kKillNe: e = "float(any(!(s0 == s1))).xxxx"; break;
        case AluVectorOpcode::kDst: e = "float4(1.0, xe_mul1(s0.y, s1.y), s0.z, s1.w)"; break;
        case AluVectorOpcode::kMaxA:
          // a0 from W rounded and clamped to [-256, 255] (rexglue's interpreter; Canary's DXBC clamps to [0, 255]).
          Line("a0 = xe_a0_round(s0.w);");
          e = "xe_max(s0, s1)";
          break;
        default:
          Simplify("unknown_vector_opcode", std::format("vector opcode {} emitted as 0", uint32_t(a.vector_opcode)));
          e = "float4(0.0, 0.0, 0.0, 0.0)";
          break;
      }
      (void)replicate;
      Line("float4 vr = " + e + ";");
      if (a.vector_result.clamp) Line("vr = xe_sat4(vr);");
      vr = "vr";
      if (a.vector_opcode >= AluVectorOpcode::kKillEq && a.vector_opcode <= AluVectorOpcode::kKillNe) EmitKill("vr.x != 0.0");
    }

    if (do_scalar) {
      std::string e;
      bool sets_ps = true;
      switch (a.scalar_opcode) {
        case AluScalarOpcode::kAdds: case AluScalarOpcode::kAddsc0: case AluScalarOpcode::kAddsc1: e = "sa + sb"; break;
        case AluScalarOpcode::kAddsPrev: e = "sa + ps"; break;
        case AluScalarOpcode::kMuls: case AluScalarOpcode::kMulsc0: case AluScalarOpcode::kMulsc1: e = "xe_mul1(sa, sb)"; break;
        case AluScalarOpcode::kMulsPrev: e = "xe_mul1(sa, ps)"; break;
        case AluScalarOpcode::kMulsPrev2: e = "xe_muls_prev2(sa, sb, ps)"; break;
        case AluScalarOpcode::kMaxs: e = "xe_max1(sa, sb)"; break;
        case AluScalarOpcode::kMins: e = "xe_min1(sa, sb)"; break;
        case AluScalarOpcode::kSeqs: e = "float(sa == 0.0)"; break;
        case AluScalarOpcode::kSgts: e = "float(sa > 0.0)"; break;
        case AluScalarOpcode::kSges: e = "float(sa >= 0.0)"; break;
        case AluScalarOpcode::kSnes: e = "float(!(sa == 0.0))"; break;
        case AluScalarOpcode::kFrcs: e = "sa - floor(sa)"; break;
        case AluScalarOpcode::kTruncs: e = "trunc(sa)"; break;
        case AluScalarOpcode::kFloors: e = "floor(sa)"; break;
        case AluScalarOpcode::kExp: e = "xe_exp(sa)"; break;
        case AluScalarOpcode::kLogc: e = "xe_logc(sa)"; break;
        case AluScalarOpcode::kLog: e = "xe_log(sa)"; break;
        case AluScalarOpcode::kRcpc: e = "xe_clamp_inf(xe_rcp(sa))"; break;
        case AluScalarOpcode::kRcpf: e = "xe_zero_inf(xe_rcp(sa))"; break;
        case AluScalarOpcode::kRcp: e = "xe_rcp(sa)"; break;
        case AluScalarOpcode::kRsqc: e = "xe_clamp_inf(xe_rsq(sa))"; break;
        case AluScalarOpcode::kRsqf: e = "xe_zero_inf(xe_rsq(sa))"; break;
        case AluScalarOpcode::kRsq: e = "xe_rsq(sa)"; break;
        case AluScalarOpcode::kMaxAs:
          Line("a0 = xe_a0_round(sa);");
          e = "xe_max1(sa, sb)";
          break;
        case AluScalarOpcode::kMaxAsf:
          Line("a0 = xe_a0_floor(sa);");
          e = "xe_max1(sa, sb)";
          break;
        case AluScalarOpcode::kSubs: case AluScalarOpcode::kSubsc0: case AluScalarOpcode::kSubsc1: e = "sa - sb"; break;
        case AluScalarOpcode::kSubsPrev: e = "sa - ps"; break;
        case AluScalarOpcode::kSetpEq: Line("p0 = sa == 0.0;"); e = "float(!p0)"; break;
        case AluScalarOpcode::kSetpNe: Line("p0 = !(sa == 0.0);"); e = "float(!p0)"; break;
        case AluScalarOpcode::kSetpGt: Line("p0 = sa > 0.0;"); e = "float(!p0)"; break;
        case AluScalarOpcode::kSetpGe: Line("p0 = sa >= 0.0;"); e = "float(!p0)"; break;
        case AluScalarOpcode::kSetpInv: Line("p0 = sa == 1.0;"); e = "p0 ? 0.0 : (sa == 0.0 ? 1.0 : sa)"; break;
        case AluScalarOpcode::kSetpPop: Line("p0 = (sa - 1.0) <= 0.0;"); e = "p0 ? 0.0 : sa - 1.0"; break;
        case AluScalarOpcode::kSetpClr: Line("p0 = false;"); e = "XE_FLT_MAX"; break;
        case AluScalarOpcode::kSetpRstr: Line("p0 = sa == 0.0;"); e = "p0 ? 0.0 : sa"; break;
        case AluScalarOpcode::kKillsEq: e = "float(sa == 0.0)"; break;
        case AluScalarOpcode::kKillsGt: e = "float(sa > 0.0)"; break;
        case AluScalarOpcode::kKillsGe: e = "float(sa >= 0.0)"; break;
        case AluScalarOpcode::kKillsNe: e = "float(!(sa == 0.0))"; break;
        case AluScalarOpcode::kKillsOne: e = "float(sa == 1.0)"; break;
        case AluScalarOpcode::kSqrt: e = "xe_sqrt(sa)"; break;  // patch 0037
        case AluScalarOpcode::kSin: e = "sin(sa)"; break;
        case AluScalarOpcode::kCos: e = "cos(sa)"; break;
        case AluScalarOpcode::kRetainPrev: sets_ps = false; break;
        default:
          Simplify("unknown_scalar_opcode", std::format("scalar opcode {} emitted as 0", uint32_t(a.scalar_opcode)));
          e = "0.0";
          break;
      }
      if (sets_ps) Line("ps = " + e + ";");
      if (a.scalar_opcode >= AluScalarOpcode::kKillsEq && a.scalar_opcode <= AluScalarOpcode::kKillsOne) EmitKill("ps != 0.0");
      // The clamp applies to the written value; ps keeps the unclamped result.
      Line(std::string("float sr = ") + (a.scalar_result.clamp ? "xe_sat(ps)" : "ps") + ";");
    }

    // Writes.
    const Result& v = a.vector_result;
    const Result& s = a.scalar_result;
    // Both operations of an ALU slot write to the same kind of target: registers, or (export) the register named by
    // the vector destination - the decoder gives both results the same target.
    if (v.target == Target::kRegister) {
      if (v.write_mask && v.target == Target::kRegister) {
        NoteRegister(v.index, v.addressing);
        std::string m;
        for (uint32_t i = 0; i < 4; ++i) if (v.write_mask & (1u << i)) m += kComp[i];
        Line(std::format("{}.{} = {}.{};", Reg(v.index, v.addressing), m, vr, m));
      }
      if (s.write_mask && s.target == Target::kRegister) {
        NoteRegister(s.index, s.addressing);
        std::string m;
        for (uint32_t i = 0; i < 4; ++i) if (s.write_mask & (1u << i)) m += kComp[i];
        Line(std::format("{}.{} = sr.{};", Reg(s.index, s.addressing), m, std::string(m.size(), 'x')));
      }
    } else if (v.write_mask || s.write_mask) {
      if (v.target == Target::kExportAddress) {
        // A new export address: the eM# written for the previous one go to memory first (the SDK flushes the eM
        // potentially written before an eA write). Constants read here are the stream constant(s) (the renderer reads
        // their base / count to know which memory the draw writes).
        Line("xe_memexport_flush();");
        for (uint32_t i = 0; i < a.vector_operand_count; ++i) {
          const Operand& op = a.vector_operands[i];
          if (op.source == Source::kConstantFloat && op.addressing == Addressing::kAbsolute) stream_constants.insert(op.index);
        }
        for (uint32_t i = 0; i < a.scalar_operand_count; ++i) {
          const Operand& op = a.scalar_operands[i];
          if (op.source == Source::kConstantFloat && op.addressing == Addressing::kAbsolute) stream_constants.insert(op.index);
        }
      }
      const std::string dst = ExportTarget(v);
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t bit = 1u << i;
        if (v.write_mask & bit) {
          const Swizzle c = v.components[i];
          const std::string val = c == Swizzle::k0 ? "0.0" : c == Swizzle::k1 ? "1.0" : vr + "." + kComp[i];
          Line(std::format("{}.{} = {};", dst, kComp[i], val));
        } else if (s.write_mask & bit) {
          Line(std::format("{}.{} = sr;", dst, kComp[i]));
        }
      }
      if (v.target == Target::kDepth || s.target == Target::kDepth) writes_depth = true;
      if (v.target == Target::kExportData && v.index < 5) Line(std::format("xe_eM_written |= {}u;", 1u << v.index));
    }
    --indent;
    Line("}");
    if (a.predicated) {
      --indent;
      Line("}");
    }
  }

  bool writes_depth = false;
  bool any_kill = false;
  bool memexport = false;
  std::set<uint32_t> stream_constants;

  void EmitKill(const std::string& cond) {
    if (p.is_vertex_shader) {
      Simplify("kill_in_vertex_shader", "kill op in a vertex shader has no effect (value still written)");
      return;
    }
    any_kill = true;
    Line(std::format("if ({}) discard;", cond));
  }

  // ---- Fetch ----
  void EmitVertexFetch(const VertexFetch& f) {
    out.uses_shared_memory = true;
    if (f.predicated) {
      Line(std::format("if (p0 == {}) {{", f.predicate_condition ? "true" : "false"));
      ++indent;
    }
    Line("{");
    ++indent;
    const uint32_t vf = f.fetch_constant;
    Line(std::format("uint fdw0 = xe_fetch_dw({}u), fdw1 = xe_fetch_dw({}u);", vf * 2, vf * 2 + 1));
    if (!f.mini) {
      NoteRegister(f.address.index, f.address.addressing);
      // Index floored, or rounded as floor(i + 0.5) when RoundIndex is set (Xenia's DXBC translator).
      const std::string idx = Operand1(f.address, 0);
      Line(std::format("int vidx = xe_ftoi(floor({}{}));", idx, f.index_rounded ? " + 0.5" : ""));
      // xe_vfa is in dwords, as the interpreter computes it (stride x index + fetch-constant address, 32-bit wrap).
      Line(std::format("xe_vfa = (fdw0 >> 2) + uint(vidx) * {}u;", f.stride));
    }
    if (f.data_format == 0) {
      Line("float4 vv = float4(0.0, 0.0, 0.0, 0.0);");
    } else {
      uint32_t words = 1;
      switch (f.data_format) {
        case 26: case 32: case 34: case 37: words = 2; break;
        case 57: words = 3; break;
        case 35: case 38: words = 4; break;
        default: words = 1; break;
      }
      Line("uint4 vw = uint4(0u, 0u, 0u, 0u);");
      for (uint32_t i = 0; i < words; ++i) {
        // Words outside the fetch constant's buffer [address, address + size) read as 0: the hardware clamps (Canary
        // dxbc_shader_translator_fetch.cc: "Games rely on that"; the interpreter checks both ends, as here).
        Line(std::format("vw.{} = xe_vf_word(xe_vfa + uint({}), fdw0, fdw1);", kComp[i], f.offset + int32_t(i)));
      }
      Line(std::format("float4 vv = xe_vf_unpack({}u, vw, {}, {}, {});", f.data_format, f.is_signed ? "true" : "false",
                       f.is_integer ? "false" : "true", f.signed_rf_mode ? "true" : "false"));
      if (f.exp_adjust) Line(std::format("vv *= {};", std::ldexp(1.0, f.exp_adjust)));
    }
    NoteRegister(f.result.index, f.result.addressing);
    StoreFetch(f.result, "vv");
    --indent;
    Line("}");
    if (f.predicated) {
      --indent;
      Line("}");
    }
  }

  void EmitTextureFetch(const TextureFetch& f) {
    if (f.predicated) {
      Line(std::format("if (p0 == {}) {{", f.predicate_condition ? "true" : "false"));
      ++indent;
    }
    Line("{");
    ++indent;
    NoteRegister(f.coordinates.index, f.coordinates.addressing);
    const uint32_t k = f.fetch_constant;
    switch (f.opcode) {
      case FetchOpcode::kSetTextureLod:
        Line(std::format("xe_lod = {};", Operand1(f.coordinates, 0)));
        break;
      case FetchOpcode::kSetTextureGradientsHorz:
        Line(std::format("xe_grad_h = float3({}, {}, {});", Operand1(f.coordinates, 0), Operand1(f.coordinates, 1),
                         Operand1(f.coordinates, 2)));
        break;
      case FetchOpcode::kSetTextureGradientsVert:
        Line(std::format("xe_grad_v = float3({}, {}, {});", Operand1(f.coordinates, 0), Operand1(f.coordinates, 1),
                         Operand1(f.coordinates, 2)));
        break;
      case FetchOpcode::kTextureFetch:
      case FetchOpcode::kGetTextureComputedLod: {
        const bool cube = f.dimension == FetchDimension::kCube;
        const bool is3d = f.dimension == FetchDimension::k3DOrStacked;
        std::string tex, coord;
        if (cube) {
          out.texture_slots_cube |= 1u << k;
          tex = std::format("xe_tex_cube_{}", k);
        } else if (is3d) {
          out.texture_slots_3d |= 1u << k;
          tex = std::format("xe_tex_3d_{}", k);
          Simplify("tfetch3d_stacked_as_3d", "3D/stacked fetch always samples a Texture3D (stacked 2D arrays not handled)");
        } else {
          out.texture_slots_2d |= 1u << k;
          tex = std::format("xe_tex_2d_{}", k);
        }
        const uint32_t n = cube || is3d ? 3 : 2;
        // 1D fetches read X only; Y is 0 (rexglue's decoding, notes/37).
        if (f.dimension == FetchDimension::k1D) {
          Line(std::format("float2 tc = float2({}, 0.0);", Operand1(f.coordinates, 0)));
        } else if (n == 2) {
          Line(std::format("float2 tc = float2({}, {});", Operand1(f.coordinates, 0), Operand1(f.coordinates, 1)));
        } else {
          Line(std::format("float3 tc = float3({}, {}, {});", Operand1(f.coordinates, 0), Operand1(f.coordinates, 1),
                           Operand1(f.coordinates, 2)));
        }
        // Size for unnormalized coordinates and offsets (Xenia always adds a 1.5/1024 texel epsilon, at mip 0).
        if (cube) {
          Line(std::format("float2 tsz; {}.GetDimensions(tsz.x, tsz.y);", tex));
          Line(std::format("tc.xy += float2({}, {}) / tsz;", f.offset_x + 1.5f / 1024.0f, f.offset_y + 1.5f / 1024.0f));
          Line("float3 tdir = xe_cube_dir(tc);");
          coord = "tdir";
        } else if (is3d) {
          Line(std::format("float3 tsz; {}.GetDimensions(tsz.x, tsz.y, tsz.z);", tex));
          if (f.unnormalized_coordinates) Line("tc /= tsz;");
          Line(std::format("tc += float3({}, {}, {}) / tsz;", f.offset_x + 1.5f / 1024.0f, f.offset_y + 1.5f / 1024.0f,
                           f.offset_z + 1.5f / 1024.0f));
          coord = "tc";
        } else {
          Line(std::format("float2 tsz; {}.GetDimensions(tsz.x, tsz.y);", tex));
          // Stage 7: a texture held at the resolution scale (bit k of xe_ndc_offset.w) - texel maths in guest texels.
          Line(std::format("if ((asuint(xe_ndc_offset.w) >> {}u) & 1u) tsz /= xe_ndc_scale.w;", k));
          // Patch 0040 (Canary 8486e97): an unnormalized 2D fetch whose fetch constant locks one mip (dword 4: MipMinLevel
          // bits 2-5 == MipMaxLevel bits 6-9) addresses that mip's grid - its size, max(size >> mip, 1), is the divisor.
          if (f.unnormalized_coordinates && f.opcode == FetchOpcode::kTextureFetch)
            Line(std::format("{{ uint xe_w4 = xe_fetch_dw({}u); uint xe_mn = (xe_w4 >> 2) & 15u; if (xe_mn == ((xe_w4 >> 6) "
                             "& 15u)) tsz = max(floor(tsz / float(1u << xe_mn)), 1.0); }}", 6 * k + 4));
          if (f.unnormalized_coordinates) Line("tc /= tsz;");
          Line(std::format("tc += float2({}, {}) / tsz;", f.offset_x + 1.5f / 1024.0f, f.offset_y + 1.5f / 1024.0f));
          coord = "tc";
        }
        if (f.offset_x != 0.0f || f.offset_y != 0.0f || f.offset_z != 0.0f) {
          Simplify("tex_offset_mip0", "fetch offset applied in mip-0 texels (Canary's way; hardware formula unknown)");
        }
        if (f.mag_filter != TextureFilter::kUseFetchConst || f.min_filter != TextureFilter::kUseFetchConst ||
            f.mip_filter != TextureFilter::kUseFetchConst || f.aniso_filter != AnisoFilter::kUseFetchConst ||
            f.vol_mag_filter != TextureFilter::kUseFetchConst || f.vol_min_filter != TextureFilter::kUseFetchConst) {
          Simplify("tex_filter_override", "per-instruction filter override ignored (the fetch constant's sampler is used)");
        }
        const std::string smp = std::format("xe_sampler_{}", k);
        if (f.opcode == FetchOpcode::kGetTextureComputedLod) {
          if (p.is_vertex_shader) {
            Line("float4 tv = float4(0.0, 0.0, 0.0, 0.0);");
            Simplify("tex_lod_query_vs", "getCompTexLOD in a vertex shader returns 0");
          } else {
            Line(std::format("float4 tv = float4({}.CalculateLevelOfDetail({}, {}), 0.0, 0.0, 0.0);", tex, smp, coord));
          }
        } else {
          std::string sample;
          const std::string bias = std::format("{}", f.lod_bias);
          if (f.use_register_gradients) {
            sample = std::format("{}.SampleGrad({}, {}, xe_grad_h{}, xe_grad_v{})", tex, smp, coord, n == 2 ? ".xy" : "",
                                 n == 2 ? ".xy" : "");
          } else if (!f.use_computed_lod || p.is_vertex_shader) {
            sample = std::format("{}.SampleLevel({}, {}, {}{})", tex, smp, coord, bias,
                                 f.use_register_lod ? " + xe_lod" : "");
          } else if (f.lod_bias != 0.0f || f.use_register_lod) {
            sample = std::format("{}.SampleBias({}, {}, {}{})", tex, smp, coord, bias, f.use_register_lod ? " + xe_lod" : "");
          } else {
            sample = std::format("{}.Sample({}, {})", tex, smp, coord);
          }
          Line(std::format("float4 tv = xe_tex_post({}, {}u);", sample, k));
        }
        if (f.has_result) {
          NoteRegister(f.result.index, f.result.addressing);
          StoreFetch(f.result, "tv");
        }
        break;
      }
      case FetchOpcode::kGetTextureWeights: {
        // Gears 3 (2026-10-22): its shadow projection (global ps 3C5E875D17B49A36) filters a 3x3 block of shadow-map
        // taps with these weights; returning 0 gave blocky shadow edges. The weights are the bilinear fractions of the
        // fetch's coordinate (after its offsets): frac(coord * size - 0.5) per axis, filter modes not considered, mip
        // lerp factor (W) 0 - as the SDK's / Xenia's DXBC translator (dxbc_translator_fetch.cpp). Cube maps: 0.
        const bool cube = f.dimension == FetchDimension::kCube;
        const bool is3d = f.dimension == FetchDimension::k3DOrStacked;
        if (cube) {
          Simplify("tex_get_weights_cube", "getWeights of a cube map returns 0");
          goto zero_result;
        }
        if (is3d) {
          out.texture_slots_3d |= 1u << k;
          const std::string tex = std::format("xe_tex_3d_{}", k);
          Line(std::format("float3 tc = float3({}, {}, {});", Operand1(f.coordinates, 0), Operand1(f.coordinates, 1),
                           Operand1(f.coordinates, 2)));
          Line(std::format("float3 tsz; {}.GetDimensions(tsz.x, tsz.y, tsz.z);", tex));
          if (f.unnormalized_coordinates) Line("tc /= tsz;");
          Line(std::format("tc += float3({}, {}, {}) / tsz;", f.offset_x + 1.5f / 1024.0f, f.offset_y + 1.5f / 1024.0f,
                           f.offset_z + 1.5f / 1024.0f));
          Line("float4 tv = float4(frac(tc * tsz - 0.5), 0.0);");
        } else {
          out.texture_slots_2d |= 1u << k;
          const std::string tex = std::format("xe_tex_2d_{}", k);
          if (f.dimension == FetchDimension::k1D) {
            Line(std::format("float2 tc = float2({}, 0.0);", Operand1(f.coordinates, 0)));
          } else {
            Line(std::format("float2 tc = float2({}, {});", Operand1(f.coordinates, 0), Operand1(f.coordinates, 1)));
          }
          Line(std::format("float2 tsz; {}.GetDimensions(tsz.x, tsz.y);", tex));
          Line(std::format("if ((asuint(xe_ndc_offset.w) >> {}u) & 1u) tsz /= xe_ndc_scale.w;", k));
          if (f.unnormalized_coordinates) Line("tc /= tsz;");
          Line(std::format("tc += float2({}, {}) / tsz;", f.offset_x + 1.5f / 1024.0f, f.offset_y + 1.5f / 1024.0f));
          Line(std::format("float4 tv = float4(frac(tc * tsz - 0.5), 0.0, 0.0);"));
          if (f.dimension == FetchDimension::k1D) Line("tv.y = 0.0;");
        }
        if (f.has_result) {
          NoteRegister(f.result.index, f.result.addressing);
          StoreFetch(f.result, "tv");
        }
        break;
      }
      case FetchOpcode::kGetTextureGradients:
        Simplify("tex_get_gradients", "getGradients returns 0 (not implemented)");
        goto zero_result;
      case FetchOpcode::kGetTextureBorderColorFrac:
        Simplify("tex_border_color_frac", "getBCF returns 0 (not implemented)");
      zero_result:
        if (f.has_result) {
          NoteRegister(f.result.index, f.result.addressing);
          Line("float4 tv = float4(0.0, 0.0, 0.0, 0.0);");
          StoreFetch(f.result, "tv");
        }
        break;
      default:
        Simplify("unknown_fetch_opcode", std::format("fetch opcode {} ignored", uint32_t(f.opcode)));
        break;
    }
    --indent;
    Line("}");
    if (f.predicated) {
      --indent;
      Line("}");
    }
  }

  void EmitExecBody(const ControlFlow& cf) {
    for (const Instruction& ins : cf.instructions) {
      switch (ins.kind) {
        case Instruction::Kind::kAlu: EmitAlu(ins.alu); break;
        case Instruction::Kind::kVertexFetch: EmitVertexFetch(ins.vfetch); break;
        case Instruction::Kind::kTextureFetch: EmitTextureFetch(ins.tfetch); break;
      }
    }
  }

  static bool IsExec(CfOpcode op) {
    switch (op) {
      case CfOpcode::kExec: case CfOpcode::kExecEnd: case CfOpcode::kCondExec: case CfOpcode::kCondExecEnd:
      case CfOpcode::kCondExecPred: case CfOpcode::kCondExecPredEnd: case CfOpcode::kCondExecPredClean:
      case CfOpcode::kCondExecPredCleanEnd:
        return true;
      default:
        return false;
    }
  }
  std::string ExecCondition(const ControlFlow& cf) const {
    if (cf.exec_type == ControlFlow::ExecType::kConditional)
      return std::format("xe_bool_const({}u) == {}", cf.bool_constant, cf.condition ? "true" : "false");
    if (cf.exec_type == ControlFlow::ExecType::kPredicated) return std::format("p0 == {}", cf.condition ? "true" : "false");
    return "";
  }
  std::string BranchCondition(const ControlFlow& cf) const {
    if (cf.branch_type == ControlFlow::ExecType::kConditional)
      return std::format("xe_bool_const({}u) == {}", cf.bool_constant, cf.condition ? "true" : "false");
    if (cf.branch_type == ControlFlow::ExecType::kPredicated) return std::format("p0 == {}", cf.condition ? "true" : "false");
    return "";
  }

  void EmitStraightLine() {
    Line("[loop] do {");
    ++indent;
    for (const ControlFlow& cf : p.control_flow) {
      if (!IsExec(cf.opcode)) continue;  // nop, alloc, mark_vs_fetch_done: nothing to do
      Line(std::format("// cf {}", cf.index));
      const std::string c = ExecCondition(cf);
      if (!c.empty()) {
        Line("if (" + c + ") {");
        ++indent;
      } else {
        Line("{");
        ++indent;
      }
      EmitExecBody(cf);
      if (cf.exec_end) Line("break;");
      --indent;
      Line("}");
    }
    --indent;
    Line("} while (false);");
  }

  void EmitPcLoop() {
    out.uses_pc_loop = true;
    Line("uint xe_pc = 0u;");
    Line("uint xe_guard = 0u;");
    Line("[loop] while (xe_guard++ < 65536u) {");
    ++indent;
    for (const ControlFlow& cf : p.control_flow) {
      const uint32_t k = cf.index;
      Line(std::format("if (xe_pc == {}u) {{", k));
      ++indent;
      if (IsExec(cf.opcode)) {
        const std::string c = ExecCondition(cf);
        if (!c.empty()) {
          Line("if (" + c + ") {");
          ++indent;
        } else {
          Line("{");
          ++indent;
        }
        EmitExecBody(cf);
        if (cf.exec_end) Line("xe_pc = 0xFFFFFFFFu; break;");
        --indent;
        Line("}");
        Line(std::format("xe_pc = {}u;", k + 1));
      } else {
        switch (cf.opcode) {
          case CfOpcode::kLoopStart:
            Line(std::format("uint lc = xe_loop_const({}u);", cf.loop_constant));
            Line(std::format("uint it = {};", cf.repeat ? "(xe_loop_depth > 0 ? xe_loop_iter[xe_loop_depth - 1] : 0u)" : "0u"));
            Line(std::format("if (xe_loop_depth >= 4 || it >= (lc & 0xFFu)) {{ xe_pc = {}u; }}", cf.target));
            Line("else {");
            Line(std::format("  xe_loop_iter[xe_loop_depth] = it; xe_loop_lc[xe_loop_depth] = lc; ++xe_loop_depth;"));
            Line("  xe_aL = xe_al(it, lc);");
            Line(std::format("  xe_pc = {}u;", k + 1));
            Line("}");
            if (cf.target <= k) Line("continue;");
            break;
          case CfOpcode::kLoopEnd: {
            Line("if (xe_loop_depth > 0) {");
            Line("  uint d = xe_loop_depth - 1;");
            Line("  uint it = ++xe_loop_iter[d];");
            const std::string brk = cf.predicated_break ? std::format(" && !(p0 == {})", cf.condition ? "true" : "false") : "";
            Line(std::format("  if (it < (xe_loop_lc[d] & 0xFFu){}) {{ xe_aL = xe_al(it, xe_loop_lc[d]); xe_pc = {}u; continue; }}", brk,
                             cf.target));
            Line("  xe_loop_depth = d;");
            Line("  xe_aL = d > 0 ? xe_al(xe_loop_iter[d - 1], xe_loop_lc[d - 1]) : 0;");
            Line("}");
            Line(std::format("xe_pc = {}u;", k + 1));
            break;
          }
          case CfOpcode::kCondCall: {
            const std::string c = BranchCondition(cf);
            Line(std::format("if ({}xe_call_depth < 4) {{", c.empty() ? "" : "(" + c + ") && "));
            Line(std::format("  xe_ret[xe_call_depth++] = {}u; xe_pc = {}u;{}", k + 1, cf.target, cf.target <= k ? " continue;" : ""));
            Line(std::format("}} else {{ xe_pc = {}u; }}", k + 1));
            break;
          }
          case CfOpcode::kReturn:
            Line("if (xe_call_depth > 0) { xe_pc = xe_ret[--xe_call_depth]; continue; }");
            Line(std::format("xe_pc = {}u;", k + 1));
            break;
          case CfOpcode::kCondJmp: {
            const std::string c = BranchCondition(cf);
            if (c.empty()) {
              Line(std::format("xe_pc = {}u;{}", cf.target, cf.target <= k ? " continue;" : ""));
            } else {
              Line(std::format("if ({}) {{ xe_pc = {}u;{} }} else {{ xe_pc = {}u; }}", c, cf.target,
                               cf.target <= k ? " continue;" : "", k + 1));
            }
            break;
          }
          default:
            Line(std::format("xe_pc = {}u;", k + 1));
            break;
        }
      }
      --indent;
      Line("}");
    }
    Line("break;");
    --indent;
    Line("}");
  }
};

}  // namespace

std::string CommonPrelude(bool is_vertex_shader) {
  return std::string(kPreludeCommon) + (is_vertex_shader ? kPreludeVertex : kPreludePixel);
}

Translation TranslateToHlsl(const Program& program, const HlslOptions& options) {
  Translation out;
  Gen g(program, options, out);
  bool needs_pc = false;
  for (const ControlFlow& cf : program.control_flow) {
    if (cf.opcode == CfOpcode::kLoopStart || cf.opcode == CfOpcode::kLoopEnd || cf.opcode == CfOpcode::kCondCall ||
        cf.opcode == CfOpcode::kReturn || cf.opcode == CfOpcode::kCondJmp) {
      needs_pc = true;
    }
  }
  g.indent = 1;
  if (needs_pc) g.EmitPcLoop();
  else g.EmitStraightLine();
  if (!program.issues.empty()) {
    for (const auto& is : program.issues) g.Simplify("decoder_issue", IssueKindName(is.kind));
  }

  out.float_constants = g.float_constants;
  out.float_constants_low = out.float_constants_high = g.float_constants;
  for (int i = 0; i < 4; ++i) out.float_constants_read[i] = g.constants_read[i];
  out.float_constants_relative = g.constants_relative;
  // Not for memexport shaders (Gears 3): their export-only draws upload constants on their own path.
  if (options.compact_float_constants && !g.constants_relative && !g.memexport) {
    // The widest run of constants this shader never reads (between two it reads, or below the first). Shaders often
    // read a few low constants plus the literal block the compiler puts at c252-c255, which made every upload the
    // full 4 KB (job 166 / job 249 profile). With a gap of 16+ registers the shader reads a compacted array instead:
    // c[0, L) at 0.., then c[H, float_constants) right after it - the renderer uploads exactly those two pieces.
    const auto read = [&](uint32_t i) { return (g.constants_read[i >> 6] >> (i & 63)) & 1; };
    uint32_t best_l = 0, best_h = 0;
    int prev = -1;
    for (uint32_t i = 0; i < g.float_constants; ++i) {
      if (!read(i)) continue;
      if (int(i) - prev - 1 > int(best_h - best_l)) {
        best_l = uint32_t(prev + 1);
        best_h = i;
      }
      prev = int(i);
    }
    if (best_h - best_l >= kCompactMinGap) {
      out.float_constants_low = best_l;
      out.float_constants_high = best_h;
      // Rewrite every absolute read `xe_c[N]` (relative reads go through xe_c_read, absent here by the check above).
      std::string body;
      body.reserve(g.body.size());
      size_t pos = 0;
      static constexpr char kTag[] = "xe_c[";
      for (size_t at; (at = g.body.find(kTag, pos)) != std::string::npos;) {
        const size_t num = at + sizeof(kTag) - 1;
        size_t end = num;
        while (end < g.body.size() && g.body[end] >= '0' && g.body[end] <= '9') ++end;
        body.append(g.body, pos, num - pos);
        if (end == num || end >= g.body.size() || g.body[end] != ']') {  // not ours: keep as written
          pos = num;
          continue;
        }
        const uint32_t idx = uint32_t(std::stoul(g.body.substr(num, end - num)));
        body += std::to_string(idx < best_l ? idx : best_l + (idx - best_h));
        pos = end;
      }
      body.append(g.body, pos, std::string::npos);
      g.body = std::move(body);
    }
  }
  const uint32_t regs = g.dynamic_regs ? 64u
                                       : std::max(g.reg_count, program.is_vertex_shader
                                                                   ? (options.tess_quad_segments ? 2u : 1u)
                                                                   : 16u);
  std::string s = "// Generated by Gears2-Native xenos::hlsl (S3.3). Do not edit.\n";
  if (options.tex_sign_by_swizzle) s += "#define XE_SIGN_BY_SWIZZLE 1\n";  // patch 0024
  if (options.tex_num_format) s += "#define XE_TEX_NUM_FORMAT 1\n";  // patch 0040
  if (options.vertex_index_clamp && program.is_vertex_shader) s += "#define XE_VTX_CLAMP 1\n";  // patch 0028
  if (!program.is_vertex_shader && g.writes_depth) s += "#define XE_WRITES_DEPTH 1\n";
  if (g.memexport) {
    s += "#define XE_MEMEXPORT 1\n";
    s += "RWByteAddressBuffer xe_memexport_buffer : register(u0, space0);\n";
  }
  s += CommonPrelude(program.is_vertex_shader);
  if (g.memexport) s += kPreludeMemexport;
  out.memexport = g.memexport;
  out.memexport_stream_constants.assign(g.stream_constants.begin(), g.stream_constants.end());
  // Resources this shader uses (binding contract in notes/39).
  for (uint32_t k = 0; k < 32; ++k) {
    const bool any = ((out.texture_slots_2d | out.texture_slots_3d | out.texture_slots_cube) >> k) & 1;
    if (out.texture_slots_2d >> k & 1)
      s += std::format("XE_VK({0}, 1) Texture2D<float4> xe_tex_2d_{0} : register(t{0}, space0);\n", k);
    if (out.texture_slots_cube >> k & 1)
      s += std::format("XE_VK({1}, 1) TextureCube<float4> xe_tex_cube_{0} : register(t{1}, space0);\n", k, 32 + k);
    if (out.texture_slots_3d >> k & 1)
      s += std::format("XE_VK({1}, 1) Texture3D<float4> xe_tex_3d_{0} : register(t{1}, space0);\n", k, 64 + k);
    if (any) s += std::format("XE_VK({0}, 2) SamplerState xe_sampler_{0} : register(s{0}, space0);\n", k);
  }
  s += "\n";
  if (program.is_vertex_shader) {
    s += std::format("XeVsOut {}(uint xe_vertex_id : SV_VertexID) {{\n", options.entry);
  } else {
    s += std::format("XePsOut {}(XePsIn xe_in) {{\n", options.entry);
  }
  s += std::format("  float4 r[{}];\n", regs);
  s += std::format("  [unroll] for (uint i = 0; i < {}u; ++i) r[i] = float4(0.0, 0.0, 0.0, 0.0);\n", regs);
  s += "  float ps = 0.0;\n  bool p0 = false;\n  int a0 = 0;\n  int xe_aL = 0;\n";
  s += "  uint xe_loop_depth = 0u;\n  uint xe_loop_iter[4] = {0u, 0u, 0u, 0u};\n  uint xe_loop_lc[4] = {0u, 0u, 0u, 0u};\n";
  s += "  uint xe_call_depth = 0u;\n  uint xe_ret[4] = {0u, 0u, 0u, 0u};\n";
  s += "  float xe_lod = 0.0;\n  float3 xe_grad_h = float3(0.0, 0.0, 0.0), xe_grad_v = float3(0.0, 0.0, 0.0);\n";
  s += "  float4 xe_oexport = float4(0.0, 0.0, 0.0, 0.0);\n";
  if (program.is_vertex_shader) {
    s += "  uint xe_vfa = 0u;\n";
    s += "  float4 xe_o[16];\n  [unroll] for (uint j = 0; j < 16u; ++j) xe_o[j] = float4(0.0, 0.0, 0.0, 0.0);\n";
    s += "  float4 xe_opos = float4(0.0, 0.0, 0.0, 0.0);\n  float4 xe_opts = float4(0.0, 0.0, 0.0, 0.0);\n";
    if (options.tess_quad_segments) {
      // Quad-patch tessellation as a uniform grid (see HlslOptions). Corners of a cell, in this order:
      // (0,0) (1,0) (0,1) | (1,0) (1,1) (0,1). Edge coordinates are exact (i / N with i = N gives 1.0), so neighbouring
      // patches share bit-identical edge vertices (no cracks). Patch index masked to 24 bits as Xenia's
      // tessellation_indexed.vs.hlsl.
      const uint32_t n = options.tess_quad_segments;
      s += std::format("  const uint xe_tn = {}u;\n", n);
      s += "  const uint xe_tper = 6u * xe_tn * xe_tn;\n";
      s += "  const uint xe_tpatch = xe_vertex_id / xe_tper;\n";
      s += "  const uint xe_tcell = (xe_vertex_id % xe_tper) / 6u, xe_tcorner = xe_vertex_id % 6u;\n";
      s += "  const uint xe_tdu = (xe_tcorner == 1u || xe_tcorner == 3u || xe_tcorner == 4u) ? 1u : 0u;\n";
      s += "  const uint xe_tdv = (xe_tcorner == 2u || xe_tcorner == 4u || xe_tcorner == 5u) ? 1u : 0u;\n";
      s += "  r[0] = float4(float((xe_tpatch + xe_sys0.y) & 0xFFFFFFu), float(xe_tcell % xe_tn + xe_tdu) / float(xe_tn), "
           "float(xe_tcell / xe_tn + xe_tdv) / float(xe_tn), 0.0);\n";
    } else {
      // r0.x = the vertex index (plus the device's offset), as Xenia's vertex shader prologue.
      // Patch 0028 (Canary RemapAndConvertVertexIndices, Gears 3 review 04-t): the GPU uses the low 24 bits of the
      // offset index, clamped to [VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX] - a bad index reads a clamped vertex, not another.
      if (options.vertex_index_clamp)
        s += "  r[0].x = float(min(max((xe_vertex_id + xe_sys0.y) & 0xFFFFFFu, xe_sys2.x), xe_sys2.y));\n";
      else
        s += "  r[0].x = float(int(xe_vertex_id) + int(xe_sys0.y));\n";
    }
  } else {
    s += "  float4 xe_oc[4] = {float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0), "
         "float4(0.0, 0.0, 0.0, 0.0)};\n";
    s += "  float4 xe_odepth = float4(xe_in.position.z, 0.0, 0.0, 0.0);\n";
    // Interpolators into r0-r15 per the device's mask (SQ_PROGRAM_CNTL), then the pixel parameters (ps_param_gen):
    // X = pixel X with back-facing in the sign bit, Y = pixel Y, Z = is-line in the sign bit (Xenia; points are not
    // generated - no Gears 2 shader writes oPts).
    s += std::format("  [unroll] for (uint i2 = 0; i2 < {}u; ++i2) if ((xe_sys0.z >> i2) & 1u) r[i2] = xe_in.interp[i2];\n",
                     std::min(regs, 16u));
    s += "  if (xe_sys0.w & 0x80000000u) {\n";
    // Stage 7: with a resolution scale (xe_ndc_scale.w > 1) the position is in host pixels: the guest position is
    // continuous, host centre (h + 0.5) -> (h + 0.5) / scale - 0.5, so screen-space maths keeps the host's detail.
    s += "    float xe_rs = float(max((xe_sys0.w >> 8u) & 15u, 1u));\n";  // the draw's pixel scale (notes/53b)
    s += "    float4 pg = float4(xe_rs > 1.0 ? xe_in.position.xy / xe_rs - 0.5 : floor(xe_in.position.xy), 0.0, 0.0);\n";
    // Sign bits set on the bits (a -0.0 constant or a negation of +0 may lose its sign under a driver's optimiser).
    s += "    if ((xe_sys0.x & 8u) != 0 && !xe_in.front_face) pg.x = asfloat(asuint(pg.x) | 0x80000000u);\n";
    s += "    if ((xe_sys0.x & 16u) != 0) pg.z = asfloat(0x80000000u);\n";
    s += std::format("    [unroll] for (uint i3 = 0; i3 < {}u; ++i3) if (i3 == (xe_sys0.w & 15u)) r[i3] = pg;\n", std::min(regs, 16u));
    s += "  }\n";
  }
  s += g.body;
  if (g.memexport) s += "  xe_memexport_flush();\n";
  if (program.is_vertex_shader) {
    // Position completion (Xenia's CompleteVertexOrDomainShader).
    s += "  if ((xe_sys0.x & 1u) == 0) xe_opos.w = 1.0 / xe_opos.w;\n";
    s += "  if ((xe_sys0.x & 2u) != 0) xe_opos.xy *= xe_opos.w;\n";
    s += "  if ((xe_sys0.x & 4u) != 0) xe_opos.z *= xe_opos.w;\n";
    s += "  xe_opos.xyz = xe_opos.xyz * xe_ndc_scale.xyz + xe_ndc_offset.xyz * xe_opos.w;\n";
    s += "  XeVsOut o;\n  [unroll] for (uint k = 0; k < 16u; ++k) o.interp[k] = xe_o[k];\n  o.position = xe_opos;\n  return o;\n";
  } else {
    s += "  if (!xe_alpha_test(xe_oc[0].w)) discard;\n";
    s += "  XePsOut o;\n";
    s += "  o.color0 = xe_oc[0] * xe_color_exp_scale.x;\n  o.color1 = xe_oc[1] * xe_color_exp_scale.y;\n";
    s += "  o.color2 = xe_oc[2] * xe_color_exp_scale.z;\n  o.color3 = xe_oc[3] * xe_color_exp_scale.w;\n";
    if (g.writes_depth) s += "  o.depth = xe_odepth.x;\n";
    s += "  return o;\n";
  }
  s += "}\n";
  // Patch 0046: --gow2_renderer_canary_r21=false swaps the rounding for a pass-through here, after generation, so the
  // default (on) text stays byte-identical to 0037's (e8e9d38) and its shader-cache entries keep matching. 0041 had
  // put an #ifdef in the prelude, which changed every shader's text and made every cached shader miss (Gears 3 13:14).
  if (!options.canary_r21) {
    static constexpr std::string_view kR21 =
        "float xe_r21(float v) {\n"
        "  uint b = asuint(v);\n"
        "  if ((b & 0x7F800000u) == 0x7F800000u) return v;\n"
        "  uint t = b & ~3u, u = t + 4u;\n"
        "  if ((u & 0x7F800000u) == 0x7F800000u) u = t;\n"
        "  return asfloat((b & 3u) >= 2u ? u : t);\n"
        "}";
    const size_t at = s.find(kR21);
    if (at != std::string::npos) s.replace(at, kR21.size(), "float xe_r21(float v) { return v; }");
  }
  out.hlsl = std::move(s);
  return out;
}

}  // namespace xenos::hlsl
