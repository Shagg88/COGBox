// Gears2-Native S3.4 shader oracle: the edge-value probe (--probe, job 091 triage). One compute shader runs the
// translator's own prelude functions (xe_mul, xe_max, ...) and a few plain HLSL forms on every pair of edge values
// (NaN, +-Inf, +-0, 1.5, -2, FLT_MAX), and each result is checked against the Xenos rule the interpreter follows
// (notes/38). It tells which construct a driver changes - for example a D3D9 max select turned into a NaN-dropping
// fmax - without the noise of whole shaders. Our code (MIT).
#include "gn_shader_oracle.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace gn_oracle {

namespace {

const float kValues[8] = {std::numeric_limits<float>::quiet_NaN(), INFINITY, -INFINITY, 0.0f, -0.0f, 1.5f, -2.0f,
                          FLT_MAX};
const char* kValueNames[8] = {"NaN", "+Inf", "-Inf", "+0", "-0", "1.5", "-2", "FLT_MAX"};

// The probe body: lane L takes a = kValues[L & 7] (xe_c[L & 7].x), b = kValues[L >> 3] (xe_c[L >> 3].y).
// Every op writes one float of the 68 (17 float4) outputs of the lane.
struct Op {
  const char* name;
  const char* hlsl;  // expression in a, b, c (= 1.0), f = xe_c[8] (fma test triple)
  bool approximate;  // transcendental: compare with a tolerance
};

const Op kOps[] = {
    {"xe_mul1", "xe_mul1(a, b)", false},
    {"xe_max1", "xe_max1(a, b)", false},
    {"xe_min1", "xe_min1(a, b)", false},
    {"xe_max.x", "xe_max(a.xxxx, b.xxxx).x", false},
    {"xe_min.x", "xe_min(a.xxxx, b.xxxx).x", false},
    {"mad(xe_mul+c)", "xe_mul(a.xxxx, b.xxxx).x + c", false},
    {"add", "a + b", false},
    {"sub", "a - b", false},
    {"xe_sat", "xe_sat(a)", false},
    {"hlsl_saturate", "saturate(a)", false},
    {"hlsl_max", "max(a, b)", false},
    {"hlsl_min", "min(a, b)", false},
    {"xe_rcp", "xe_rcp(a)", false},
    {"xe_rsq", "xe_rsq(a)", true},
    {"xe_log", "xe_log(a)", true},
    {"xe_exp", "xe_exp(a)", true},
    {"xe_logc", "xe_logc(a)", true},
    {"rcpc", "xe_clamp_inf(xe_rcp(a))", false},
    {"rcpf", "xe_zero_inf(xe_rcp(a))", false},
    {"sqrt", "sqrt(a)", false},
    {"frc", "a - floor(a)", false},
    {"floor", "floor(a)", false},
    {"trunc", "trunc(a)", false},
    {"sge", "float(a >= b)", false},
    {"seq", "float(a == b)", false},
    {"sne", "float(!(a == b))", false},
    {"hlsl_ne", "float(a != b)", false},
    {"sgt", "float(a > b)", false},
    {"cndeq", "(a == 0.0) ? 1.0 : 2.0", false},
    {"a0_round", "float(xe_a0_round(a))", false},
    {"ftoi", "float(xe_ftoi(a))", false},
    {"xe_dp4", "xe_dp4(float4(a, 1.0, b, 1.0), float4(b, a, 1.0, 1.0))", false},
    {"xe_max4", "xe_max4(float4(a, b, a, b))", false},
    {"fma_plain", "f.x * f.y + f.z", false},
    {"fma_precise", "xe_probe_precise_mad(f.x, f.y, f.z)", false},
    {"fma_xe_mul", "xe_mul1(f.x, f.y) + f.z", false},
    {"max_nanfix", "xe_probe_max_nanfix(a, b)", false},
    {"min_nanfix", "xe_probe_min_nanfix(a, b)", false},
    {"isnan", "float(isnan(a))", false},
    {"mul_plain", "a * b", false},
    {"sample_nan_coord", "xe_probe_tex.SampleLevel(xe_probe_smp, float2(a, b), 0.0).x", false},
    {"select_ge", "xe_probe_select_ge(a, b)", false},
};
constexpr uint32_t kOpCount = sizeof(kOps) / sizeof(kOps[0]);
static_assert(kOpCount <= kVsOutputs * 4, "probe ops must fit the vertex output layout");

float Fma3(float x, float y, float z) { return float(double(x * y) + double(z)); }  // unfused: product rounded

float ClampFloat(float v, float lo, float hi) {
  const float c = std::isgreater(v, lo) ? v : lo;
  return std::isless(c, hi) ? c : hi;
}

float Mul(float a, float b) { return (a != 0.0f && b != 0.0f) ? a * b : 0.0f; }

int32_t Ftoi(float v) {
  if (std::isnan(v)) return 0;
  if (v >= 2147483648.0f) return INT32_MAX;
  if (v <= -2147483648.0f) return INT32_MIN;
  return int32_t(v);
}

// The value each op must produce (the interpreter's / notes/38's rule; IEEE where the rule is plain IEEE).
float Expected(uint32_t op, float a, float b, const float f[3]) {
  const std::string n = kOps[op].name;
  const float c = 1.0f;
  if (n == "xe_mul1" || n == "mul(xe_mul)") return Mul(a, b);
  if (n == "xe_max1" || n == "xe_max.x" || n == "max_nanfix" || n == "select_ge")
    return std::isgreaterequal(a, b) ? a : b;
  if (n == "xe_min1" || n == "xe_min.x" || n == "min_nanfix") return std::isless(a, b) ? a : b;
  if (n == "mad(xe_mul+c)") return Mul(a, b) + c;
  if (n == "add") return a + b;
  if (n == "sub") return a - b;
  if (n == "xe_sat") return ClampFloat(a, 0.0f, 1.0f);
  if (n == "hlsl_saturate") return std::isnan(a) ? 0.0f : std::min(std::max(a, 0.0f), 1.0f);  // D3D: NaN -> 0
  if (n == "hlsl_max") return std::fmax(a, b);  // D3D10+ max: the non-NaN operand
  if (n == "hlsl_min") return std::fmin(a, b);
  if (n == "xe_rcp") return 1.0f / a;
  if (n == "xe_rsq") return 1.0f / std::sqrt(a);
  if (n == "xe_log") return std::log2(a);
  if (n == "xe_exp") return std::exp2(a);
  if (n == "xe_logc") {
    const float r = std::log2(a);
    return r == -INFINITY ? -FLT_MAX : r;
  }
  if (n == "rcpc") {
    const float r = 1.0f / a;
    return r == INFINITY ? FLT_MAX : r == -INFINITY ? -FLT_MAX : r;
  }
  if (n == "rcpf") {
    const float r = 1.0f / a;
    return r == INFINITY ? 0.0f : r == -INFINITY ? -0.0f : r;
  }
  if (n == "sqrt") return std::sqrt(a);
  if (n == "frc") return a - std::floor(a);
  if (n == "floor") return std::floor(a);
  if (n == "trunc") return std::trunc(a);
  if (n == "sge") return float(std::isgreaterequal(a, b));
  if (n == "seq") return float(a == b);
  if (n == "sne" || n == "hlsl_ne") return float(a != b);
  if (n == "sgt") return float(std::isgreater(a, b));
  if (n == "cndeq") return a == 0.0f ? 1.0f : 2.0f;
  if (n == "a0_round") return float(int32_t(std::floor(ClampFloat(a, -256.0f, 255.0f) + 0.5f)));
  if (n == "ftoi") return float(Ftoi(a));
  if (n == "xe_dp4") {
    float r = 0.0f;
    r += Mul(a, b);
    r += Mul(1.0f, a);
    r += Mul(b, 1.0f);
    r += Mul(1.0f, 1.0f);
    return r;
  }
  if (n == "xe_max4") {
    const float v[4] = {a, b, a, b};
    if (std::isgreaterequal(v[0], v[1]) && std::isgreaterequal(v[0], v[2]) && std::isgreaterequal(v[0], v[3]))
      return v[0];
    if (std::isgreaterequal(v[1], v[2]) && std::isgreaterequal(v[1], v[3])) return v[1];
    if (std::isgreaterequal(v[2], v[3])) return v[2];
    return v[3];
  }
  if (n == "fma_plain" || n == "fma_precise" || n == "fma_xe_mul") return Fma3(f[0], f[1], f[2]);
  if (n == "isnan") return float(std::isnan(a));
  if (n == "mul_plain") return a * b;
  if (n == "sample_nan_coord") {
    float colour[4];
    SlotColour(0, 0, colour);
    return colour[0];
  }
  return 0.0f;
}

}  // namespace

std::string ProbeHlsl(const std::string& vertex_prelude) {
  std::string s = vertex_prelude;
  s += "\n// ---- Gears2-Native S3.4 edge-value probe ----\n"
       "XE_VK(0, 1) Texture2D<float4> xe_probe_tex : register(t0, space0);\n"
       "XE_VK(0, 2) SamplerState xe_probe_smp : register(s0, space0);\n"
       "float xe_probe_precise_mad(float x, float y, float z) { precise float p = x * y; precise float r = p + z; "
       "return r; }\n"
       "float xe_probe_max_nanfix(float a, float b) { precise float r = (a >= b) ? a : b; return isnan(b) ? b : r; }\n"
       "float xe_probe_min_nanfix(float a, float b) { precise float r = (a < b) ? a : b; return isnan(b) ? b : r; }\n"
       "float xe_probe_select_ge(float a, float b) { return (a >= b) ? a : b; }\n"
       "XE_VK(0, 3) RWStructuredBuffer<float4> xe_oracle_out : register(u0, space0);\n"
       "[numthreads(64, 1, 1)] void xe_oracle_main(uint3 xe_tid : SV_DispatchThreadID) {\n"
       "  float a = xe_c[xe_tid.x & 7u].x, b = xe_c[xe_tid.x >> 3].y, c = xe_c[9].x;\n"
       "  float4 f = xe_c[8];\n"
       "  float v[68];\n"
       "  [unroll] for (uint i = 0; i < 68u; ++i) v[i] = 0.0;\n";
  for (uint32_t i = 0; i < kOpCount; ++i) s += "  v[" + std::to_string(i) + "] = " + kOps[i].hlsl + ";\n";
  s += "  [unroll] for (uint k = 0; k < 17u; ++k) xe_oracle_out[xe_tid.x * 17u + k] = "
       "float4(v[k * 4], v[k * 4 + 1], v[k * 4 + 2], v[k * 4 + 3]);\n"
       "}\n";
  return s;
}

// Inputs: xe_c[0..7] = (kValues[i], kValues[i], 0, 0); xe_c[8] = the fma triple; xe_c[9].x = 1.0.
void ProbeConstants(std::vector<uint8_t>& cb1) {
  cb1.assign(4096, 0);
  float* c = reinterpret_cast<float*>(cb1.data());
  for (uint32_t i = 0; i < 8; ++i) c[i * 4] = c[i * 4 + 1] = kValues[i];
  // (1 + 2^-12)^2 = 1 + 2^-11 + 2^-24: rounds to 1 + 2^-11, so x * y + z is 0 unfused and 2^-24 fused.
  c[8 * 4 + 0] = 1.0f + 1.0f / 4096.0f;
  c[8 * 4 + 1] = 1.0f + 1.0f / 4096.0f;
  c[8 * 4 + 2] = -(1.0f + 1.0f / 2048.0f);
  c[9 * 4] = 1.0f;
}

// Prints one line per op: how many of the 64 pairs match, and the first few that do not. Returns the failing ops.
uint32_t ReportProbe(const std::vector<float>& out) {
  const float f[3] = {1.0f + 1.0f / 4096.0f, 1.0f + 1.0f / 4096.0f, -(1.0f + 1.0f / 2048.0f)};
  uint32_t failing = 0;
  for (uint32_t op = 0; op < kOpCount; ++op) {
    uint32_t ok = 0;
    std::string bad;
    uint32_t shown = 0;
    for (uint32_t lane = 0; lane < kLanes; ++lane) {
      const float a = kValues[lane & 7], b = kValues[lane >> 3];
      const float g = out[size_t(lane) * kVsOutputs * 4 + op];
      const float e = Expected(op, a, b, f);
      const uint32_t gb = std::bit_cast<uint32_t>(g), eb = std::bit_cast<uint32_t>(e);
      bool match = gb == eb || (std::isnan(g) && std::isnan(e));
      if (!match && kOps[op].approximate && std::isfinite(g) && std::isfinite(e))
        match = std::fabs(double(g) - double(e)) <= 1e-5 * std::max(1.0, std::fabs(double(e)));
      if (!match && g == e) match = true;  // +0 / -0 noted below only for exact ops
      if (match && gb != eb && !std::isnan(g) && !kOps[op].approximate) {
        // signed zero difference: count as a mismatch for the select/mul ops where the sign is part of the rule
        match = false;
      }
      if (match) {
        ++ok;
        continue;
      }
      if (shown++ < 6) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), " [a=%s b=%s gpu %08X want %08X]", kValueNames[lane & 7],
                      kValueNames[lane >> 3], gb, eb);
        bad += buf;
      }
    }
    if (ok != kLanes) ++failing;
    std::printf("PROBE %-18s %2u/64%s%s\n", kOps[op].name, ok, ok == kLanes ? "" : " MISMATCH", bad.c_str());
  }
  std::printf("PROBE SUMMARY: %u of %u ops differ from the Xenos rule\n", failing, kOpCount);
  return failing;
}

}  // namespace gn_oracle
