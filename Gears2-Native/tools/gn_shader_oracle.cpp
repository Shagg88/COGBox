// Gears2-Native S3.4: the shader-translator check (D21 item 4, notes/36, notes/38 point 5, notes/41).
//
// For every shader in a corpus (*.ucode, host-order microcode dwords as Shader::ucode_dwords()):
//  1. decode with our decoder (xenos/ucode.h) and translate with our translator (xenos/hlsl.h, S3.3);
//  2. wrap the generated HLSL in a compute shader (the translated `main` is called once per lane and its outputs are
//     written to a UAV - see MakeComputeWrapper) and compile it with DXC to DXIL (cs_6_6, the S3.3 flags);
//  3. run it on the GPU (D3D12, gn_shader_oracle_d3d12.cpp) for kSets constant sets x kLanes lanes;
//  4. run the SDK's CPU ShaderInterpreter (rexglue, Xenia-derived, BSD-3-Clause; patched by our SDK patches 7 and 8) on
//     exactly the same inputs - patch 8 gives it a texture-fetch callback, kill tracking, a plain guest-memory block
//     and the a0-relative index range;
//  5. compare every output component and report per shader: EXACT / CLOSE (within the tolerance; transcendentals and
//     trig differ between libm and GPU approximations) / FAIL / SKIP / ERROR, plus a CSV (hashes and numbers only).
//
// Inputs are synthetic (seeded, reproducible): float constants per set from ordinary values to the edge set (+-0,
// denormals, +-Inf, NaN, FLT_MAX), bool/loop constants (loop i31 = count 6 start 0 step 0 as measured in job 078),
// vertex data in a guest memory block, pixel interpolators per lane. Textures are the "stand-in": every texel of slot
// k has one exact colour (SlotColour), so the comparison covers the ALU around fetches, not filtering or addressing.
//
// Modes: default = full (needs D3D12 + dxc); --compile-only (translate, wrap, compile); --cpu-only (interpreter only:
// checks for hangs/crashes and reports a0 index ranges). Standard library + the SDK interpreter sources; our code (MIT).
#include "gn_shader_oracle.h"

#include <xenos/hlsl.h>
#include <xenos/ucode.h>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace rg = rex::graphics;
using namespace gn_oracle;

#ifndef GN_ORACLE_VULKAN
std::unique_ptr<GpuRunner> gn_oracle::CreateVulkanRunner() { return nullptr; }
#endif

namespace {

// ---- Small helpers ----------------------------------------------------------------------------------------------

uint32_t FloatBits(float f) { return std::bit_cast<uint32_t>(f); }
float BitsFloat(uint32_t u) { return std::bit_cast<float>(u); }

std::vector<uint8_t> ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void WriteText(const fs::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary);
  f << s;
}

// Runs a command line with a timeout; stdout/stderr go to log_path. Returns the exit code, -1 on timeout or failure.
int RunProcess(const std::string& command_line, const fs::path& log_path, uint32_t timeout_ms) {
#ifdef _WIN32
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE log = CreateFileW(log_path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = log;
  si.hStdError = log;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::string cmd = command_line;  // CreateProcessA may modify the buffer
  const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                 &pi);
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  if (!ok) return -1;
  int rc = -1;
  if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    rc = int(code);
  } else {
    TerminateProcess(pi.hProcess, 1);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return rc;
#else
  const std::string full = "timeout " + std::to_string(timeout_ms / 1000 + 1) + " " + command_line + " > \"" +
                           log_path.string() + "\" 2>&1";
  const int rc = std::system(full.c_str());
  return rc == 0 ? 0 : (rc == -1 ? -1 : 1);
#endif
}

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

// Debug (--temps, vertex shaders): the outputs o0-o15 carry the final temporaries r0-r15 instead of the
// interpolators, on both sides, to localise a mismatch.
bool g_temps_mode = false;
// --no-denormals: no denormal input values (they become zeros of the same sign). For a GPU that cannot flush
// denormals (Mesa lavapipe ignores DenormFlushToZero), so its results are comparable with the flushing interpreter.
bool g_no_denormals = false;
float NoDenormal(float v) {
  uint32_t u = std::bit_cast<uint32_t>(v);
  if (g_no_denormals && (u & 0x7F800000u) == 0) u &= 0x80000000u;
  return std::bit_cast<float>(u);
}

// ---- Inputs -----------------------------------------------------------------------------------------------------

// Reproducible values, by kind. The distributions are written out here from raw mt19937 words because the standard
// library's distributions (and libm) are implementation-defined: MSVC and libstdc++ give different values from the
// same engine, so the PC and the container saw different inputs in job 090. Only exact float operations are used.
class ValueSource {
 public:
  explicit ValueSource(uint32_t seed) : rng_(seed) {}
  // Uniform in [0, 1) with 24 random bits (exact in float).
  float Unit() { return float(rng_() >> 8) * (1.0f / 16777216.0f); }
  double UnitD() { return double(rng_() >> 5) * (1.0 / 134217728.0); }
  float Uniform(float lo, float hi) { return lo + (hi - lo) * Unit(); }
  // Uniform integer in [lo, hi] (modulo bias is irrelevant here).
  uint32_t Int(uint32_t lo, uint32_t hi) { return lo + uint32_t(rng_() % (uint64_t(hi) - lo + 1)); }
  float Ordinary() { return Uniform(-2.0f, 2.0f); }
  float Positive() { return Uniform(1.0f / 1024.0f, 1.0f); }
  float Large() {
    // About 1e20 to 1e35: a mantissa in [1, 2) times 2^66 to 2^116 (exact).
    const float m = 1.0f + Unit();
    return (Chance(0.5) ? -1.0f : 1.0f) * std::ldexp(m, int(Int(66, 116)));
  }
  float SmallInteger() { return float(Int(0, 8)); }
  float Zero() { return Chance(0.5) ? 0.0f : -0.0f; }
  float Denormal() {
    const uint32_t m = Int(1, 0x7FFFFF);
    return NoDenormal(BitsFloat((Chance(0.5) ? 0x80000000u : 0u) | m));
  }
  float Special() {
    switch (Int(0, 5)) {
      case 0: return INFINITY;
      case 1: return -INFINITY;
      case 2: return std::numeric_limits<float>::quiet_NaN();
      case 3: return FLT_MAX;
      case 4: return -FLT_MAX;
      default: return 1.0f;
    }
  }
  // Mode 0 ordinary, 1 positive, 2 +zeros, 3 +denormals, 4 +specials, 5 large, 6 small integers, 7 everything.
  float ByMode(uint32_t mode) {
    switch (mode) {
      case 0: return Ordinary();
      case 1: return Positive();
      case 2: return Chance(0.15) ? Zero() : Ordinary();
      case 3: return Chance(0.15) ? Denormal() : Ordinary();
      case 4: return Chance(0.10) ? Special() : Ordinary();
      case 5: return Large();
      case 6: return SmallInteger();
      default: {
        switch (Int(0, 6)) {
          case 0: return Zero();
          case 1: return Denormal();
          case 2: return Special();
          case 3: return Large();
          case 4: return SmallInteger();
          case 5: return Positive();
          default: return Ordinary();
        }
      }
    }
  }
  uint32_t Bits() { return rng_(); }
  bool Chance(double p) { return UnitD() < p; }

 private:
  std::mt19937 rng_;
};

// Vertex buffers live at this guest physical address; every used vertex fetch constant points here.
constexpr uint32_t kVertexDataAddress = 0x10000;
constexpr uint32_t kVertexDataWords = 0x10000;  // 256 KB

// Fills the guest memory block: mostly ordinary floats stored big-endian (the fetch constants use the k8in32 swap, so
// they read back as those floats), some raw bits (packed formats, NaNs, denormals and the like).
std::vector<uint8_t> MakeGuestMemory() {
  std::vector<uint8_t> mem(kGuestMemoryBytes, 0);
  ValueSource v(0x6E5F0001);
  for (uint32_t a = kVertexDataAddress; a < kVertexDataAddress + kVertexDataWords * 4; a += 4) {
    uint32_t word = v.Chance(0.75) ? FloatBits(v.ByMode(v.Chance(0.9) ? 0 : 7)) : FloatBits(NoDenormal(BitsFloat(v.Bits())));
    word = std::byteswap(word);  // stored big-endian; the k8in32 fetch swap turns it back
    std::memcpy(&mem[a], &word, 4);
  }
  return mem;
}

// What a shader uses, from our decoded program.
struct ShaderFacts {
  std::set<uint32_t> vertex_fetch_constants;  // 0-95
  std::set<uint32_t> texture_slots;           // 0-31 with the dimension of their first fetch
  std::map<uint32_t, uint32_t> texture_dimension;
  bool uses_texture_fetch = false;
  bool uses_transcendental = false;  // exp/log/rcp/rsq/sqrt/sin/cos variants (GPU approximations differ from libm)
  bool uses_a0 = false;
  bool uses_kill = false;
};

ShaderFacts ScanProgram(const xenos::ucode::Program& program) {
  namespace u = xenos::ucode;
  ShaderFacts f;
  auto note_operand = [&](const u::Operand& op) {
    if (op.addressing == u::Addressing::kAddressRegisterRelative) f.uses_a0 = true;
  };
  for (const auto& cf : program.control_flow) {
    for (const auto& ins : cf.instructions) {
      if (ins.kind == u::Instruction::Kind::kVertexFetch) {
        if (!ins.vfetch.mini) f.vertex_fetch_constants.insert(ins.vfetch.fetch_constant);
      } else if (ins.kind == u::Instruction::Kind::kTextureFetch) {
        if (ins.tfetch.opcode == u::FetchOpcode::kTextureFetch && ins.tfetch.has_fetch_constant) {
          f.uses_texture_fetch = true;
          f.texture_slots.insert(ins.tfetch.fetch_constant);
          uint32_t dim = 0;
          if (ins.tfetch.dimension == u::FetchDimension::kCube) dim = 1;
          else if (ins.tfetch.dimension == u::FetchDimension::k3DOrStacked) dim = 2;
          f.texture_dimension.emplace(ins.tfetch.fetch_constant, dim);
        }
      } else {
        const u::Alu& a = ins.alu;
        for (uint32_t i = 0; i < a.vector_operand_count; ++i) note_operand(a.vector_operands[i]);
        for (uint32_t i = 0; i < a.scalar_operand_count; ++i) note_operand(a.scalar_operands[i]);
        if (!a.IsScalarOpDefaultNop()) {
          switch (a.scalar_opcode) {
            case u::AluScalarOpcode::kExp: case u::AluScalarOpcode::kLog: case u::AluScalarOpcode::kLogc:
            case u::AluScalarOpcode::kRcp: case u::AluScalarOpcode::kRcpc: case u::AluScalarOpcode::kRcpf:
            case u::AluScalarOpcode::kRsq: case u::AluScalarOpcode::kRsqc: case u::AluScalarOpcode::kRsqf:
            case u::AluScalarOpcode::kSqrt: case u::AluScalarOpcode::kSin: case u::AluScalarOpcode::kCos:
              f.uses_transcendental = true;
              break;
            case u::AluScalarOpcode::kKillsEq: case u::AluScalarOpcode::kKillsGt: case u::AluScalarOpcode::kKillsGe:
            case u::AluScalarOpcode::kKillsNe: case u::AluScalarOpcode::kKillsOne:
              f.uses_kill = true;
              break;
            default:
              break;
          }
        }
        if (!a.IsVectorOpDefaultNop()) {
          switch (a.vector_opcode) {
            case u::AluVectorOpcode::kKillEq: case u::AluVectorOpcode::kKillGt: case u::AluVectorOpcode::kKillGe:
            case u::AluVectorOpcode::kKillNe:
              f.uses_kill = true;
              break;
            default:
              break;
          }
        }
      }
    }
  }
  return f;
}

// One constant set: register values for the CPU side and the matching cbuffers for the GPU side.
struct SetInputs {
  std::vector<float> float_constants;  // 256 x 4 (the stage's range)
  uint32_t bools[8] = {};
  uint32_t loops[32] = {};
  uint32_t fetch[192] = {};
  std::vector<float> pixel_inputs;     // kLanes x 16 x 4
};

SetInputs MakeSet(uint32_t set, bool vertex, const ShaderFacts& facts, std::string& note) {
  SetInputs s;
  ValueSource v(0x6E5F1000u + set * 7919u + (vertex ? 0u : 104729u));
  s.float_constants.resize(256 * 4);
  for (float& c : s.float_constants) c = v.ByMode(set);
  for (uint32_t i = 0; i < 8; ++i) {
    switch (set & 3) {
      case 0: s.bools[i] = 0; break;
      case 1: s.bools[i] = 0xFFFFFFFFu; break;
      case 2: s.bools[i] = 0xAAAAAAAAu; break;
      default: s.bools[i] = v.Bits(); break;
    }
  }
  for (uint32_t i = 0; i < 32; ++i) s.loops[i] = 0x00010004u;  // count 4, start 0, step 1
  s.loops[31] = 0x00000006u;                                    // as measured (job 078): count 6, start 0, step 0
  // Vertex fetch constants: dword 0 = type kVertex (3) | address in dwords << 2; dword 1 = endian k8in32 (2) | size
  // in words << 2.
  std::set<uint32_t> used_dwords;
  for (uint32_t vf : facts.vertex_fetch_constants) {
    s.fetch[vf * 2 + 0] = 3u | ((kVertexDataAddress / 4) << 2);
    s.fetch[vf * 2 + 1] = 2u | (kVertexDataWords << 2);
    used_dwords.insert(vf * 2);
    used_dwords.insert(vf * 2 + 1);
  }
  // Texture fetch constants: type kTexture (2), unsigned components, k_32_32_32_32_FLOAT (38), 4x4, identity swizzle,
  // no exponent adjust, dimension in dword 5. The translator's post-fetch step (signs, gamma, exponent) is then the
  // identity, and the GPU texture holds SlotColour - the value the CPU sink returns.
  for (uint32_t k : facts.texture_slots) {
    bool overlap = false;
    for (uint32_t d = 0; d < 6; ++d) overlap |= used_dwords.count(k * 6 + d) != 0;
    if (overlap) {
      note += "fetch_constant_overlap(tf" + std::to_string(k) + ") ";
      continue;
    }
    const uint32_t dim = facts.texture_dimension.count(k) ? facts.texture_dimension.at(k) : 0;
    const uint32_t data_dimension = dim == 1 ? 3u : (dim == 2 ? 2u : 1u);  // xenos DataDimension: 2D 1, 3D 2, cube 3
    s.fetch[k * 6 + 0] = 2u;
    s.fetch[k * 6 + 1] = 38u;
    s.fetch[k * 6 + 2] = 3u | (3u << 13) | ((dim == 1 ? 5u : 0u) << 26);
    s.fetch[k * 6 + 3] = (0u | (1u << 3) | (2u << 6) | (3u << 9)) << 1;
    s.fetch[k * 6 + 4] = 0;
    s.fetch[k * 6 + 5] = data_dimension << 9;
  }
  if (!vertex) {
    s.pixel_inputs.resize(size_t(kLanes) * 16 * 4);
    for (uint32_t lane = 0; lane < kLanes; ++lane) {
      const uint32_t mode = lane < 40 ? 0 : lane < 48 ? 1 : lane < 52 ? 2 : lane < 56 ? 3 : lane < 60 ? 4 : 7;
      for (uint32_t i = 0; i < 16 * 4; ++i) s.pixel_inputs[size_t(lane) * 64 + i] = v.ByMode(mode);
    }
  }
  return s;
}

GpuSet ToGpuSet(const SetInputs& s) {
  GpuSet g;
  // XeSystem: uint4 xe_sys0 (flags 1 = W not reciprocal, so position is passed through; vertex index offset 0;
  // interpolator mask 0xFFFF; no pixel parameter generation), ndc scale 1, ndc offset 0, colour exponent scale 1,
  // xe_sys1 0 (alpha test off).
  g.cb0.assign(256, 0);
  const uint32_t sys0[4] = {1u, 0u, 0xFFFFu, 0u};
  std::memcpy(&g.cb0[0], sys0, 16);
  const float ones[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  std::memcpy(&g.cb0[16], ones, 16);
  std::memcpy(&g.cb0[48], ones, 16);
  g.cb1.assign(4096, 0);
  std::memcpy(g.cb1.data(), s.float_constants.data(), 4096);
  g.cb2.assign(256, 0);
  std::memcpy(&g.cb2[0], s.bools, 32);
  std::memcpy(&g.cb2[32], s.loops, 128);
  g.cb3.assign(768, 0);
  std::memcpy(g.cb3.data(), s.fetch, 768);
  g.pixel_inputs = s.pixel_inputs;
  return g;
}

// ---- CPU side: the SDK interpreter ------------------------------------------------------------------------------

class Sinks final : public rg::ShaderInterpreter::ExportSink, public rg::ShaderInterpreter::TextureFetchSink {
 public:
  float out[kVsOutputs][4] = {};
  uint64_t written = 0;  // bit slot*4+component: what the interpreter actually exported for this test case
  bool vertex = true;
  void Reset() {
    std::memset(out, 0, sizeof(out));
    written = 0;
  }
  void Export(rg::ucode::ExportRegister reg, const float* value, uint32_t mask) override {
    int slot = -1;
    const uint32_t r = uint32_t(reg);
    if (vertex) {
      if (r < 16) slot = int(r);
      else if (r == 62) slot = 16;
    } else {
      if (r < 4) slot = int(r);
      else if (r == 61) slot = 4;
    }
    if (slot < 0) return;
    for (uint32_t i = 0; i < 4; ++i)
      if (mask & (1u << i)) {
        out[slot][i] = value[i];
        written |= uint64_t(1) << (uint32_t(slot) * 4 + i);
      }
  }
  void FetchTexture(const rg::ucode::TextureFetchInstruction& instr, const float*, float* result) override {
    uint32_t dim = 0;
    if (instr.dimension() == rg::xenos::FetchOpDimension::kCube) dim = 1;
    else if (instr.dimension() == rg::xenos::FetchOpDimension::k3DOrStacked) dim = 2;
    SlotColour(dim, instr.fetch_constant_index() & 31, result);
  }
};

struct CpuResult {
  std::vector<float> out;        // kSets x kLanes x outputs x 4
  std::vector<uint8_t> skipped;  // kSets x kLanes: the interpreter hit its step limit
  std::vector<uint64_t> written; // kSets x kLanes: exported components (bit slot*4+component), see Sinks
  uint64_t a0_reads = 0;
  int32_t a0_min = 0, a0_max = 0;
};

CpuResult RunCpu(const std::vector<uint32_t>& dwords, bool vertex, const std::vector<SetInputs>& sets,
                 const uint8_t* guest_memory, int transcendental_bias = 0) {
  static rg::RegisterFile regs;  // large; one per process
  CpuResult res;
  const uint32_t outputs = vertex ? kVsOutputs : kPsOutputs;
  res.out.assign(size_t(kSets) * kLanes * outputs * 4, 0.0f);
  res.skipped.assign(size_t(kSets) * kLanes, 0);
  res.written.assign(size_t(kSets) * kLanes, 0);
  Sinks sinks;
  sinks.vertex = vertex;
  rg::ShaderInterpreter interp(regs, guest_memory);
  interp.SetExportSink(&sinks);
  interp.SetTextureFetchSink(&sinks);
  interp.SetTranscendentalBias(transcendental_bias);  // SDK patch 9
  interp.SetShader(vertex ? rg::xenos::ShaderType::kVertex : rg::xenos::ShaderType::kPixel, dwords.data());
  bool first_a0 = true;
  for (uint32_t s = 0; s < kSets; ++s) {
    const SetInputs& in = sets[s];
    std::memset(regs.values, 0, sizeof(regs.values));
    const uint32_t base = vertex ? 0 : 256;
    for (uint32_t i = 0; i < 256 * 4; ++i)
      regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_000_X + base * 4 + i] = FloatBits(in.float_constants[i]);
    regs.values[rg::XE_GPU_REG_SQ_VS_CONST] = 0u | (255u << 12);
    regs.values[rg::XE_GPU_REG_SQ_PS_CONST] = 256u | (255u << 12);
    for (uint32_t i = 0; i < 8; ++i) regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i] = in.bools[i];
    for (uint32_t i = 0; i < 32; ++i) regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i] = in.loops[i];
    for (uint32_t i = 0; i < 192; ++i) regs.values[rg::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i] = in.fetch[i];
    for (uint32_t lane = 0; lane < kLanes; ++lane) {
      float* temps = interp.temp_registers();
      std::memset(temps, 0, sizeof(float) * 4 * rg::xenos::kMaxShaderTempRegisters);
      if (vertex) {
        temps[0] = float(lane);
      } else {
        std::memcpy(temps, &in.pixel_inputs[size_t(lane) * 64], sizeof(float) * 64);
      }
      sinks.Reset();
      interp.Execute();
      float* dst = &res.out[(size_t(s) * kLanes + lane) * outputs * 4];
      if (vertex) {
        std::memcpy(dst, sinks.out, sizeof(float) * 4 * kVsOutputs);
        if (g_temps_mode) std::memcpy(dst, temps, sizeof(float) * 4 * 16);
        // The translator's position completion with the oracle's system constants (flags 1, scale 1, offset 0):
        // xyz = xyz * 1 + 0 * w. Same IEEE operations as the shader.
        float* pos = dst + 16 * 4;
        for (int c = 0; c < 3; ++c) pos[c] = pos[c] * 1.0f + 0.0f * pos[3];
      } else {
        std::memcpy(dst, sinks.out, sizeof(float) * 4 * 5);
        if (g_temps_mode) {
          std::memcpy(dst, temps, sizeof(float) * 4 * 4);
          sinks.written |= 0xFFFFu;
        }
        dst[5 * 4] = interp.killed() ? 1.0f : 0.0f;
      }
      res.written[size_t(s) * kLanes + lane] = sinks.written;
      if (interp.hit_step_limit()) res.skipped[size_t(s) * kLanes + lane] = 1;
      if (interp.a0_relative_reads()) {
        res.a0_reads += interp.a0_relative_reads();
        if (first_a0) {
          res.a0_min = interp.a0_relative_index_min();
          res.a0_max = interp.a0_relative_index_max();
          first_a0 = false;
        } else {
          res.a0_min = std::min(res.a0_min, interp.a0_relative_index_min());
          res.a0_max = std::max(res.a0_max, interp.a0_relative_index_max());
        }
      }
    }
  }
  return res;
}

// ---- GPU side: compute wrapper around the translated shader -----------------------------------------------------

// The translated HLSL keeps its stage `main` (vertex: XeVsOut main(uint vertex_id); pixel: XePsOut main(XePsIn)); the
// wrapper calls it from a compute entry point, one lane per test case, and stores the outputs in a UAV. Pixel kills
// (`discard`, which acts as demote-to-helper) become a flag that is set while execution continues - the same
// demote semantics without needing a rasteriser. Texture Sample() is legal in cs_6_6 (derivatives in compute are a
// Shader Model 6.6 feature); with the uniform stand-in textures the derivatives do not change the result.
std::string MakeComputeWrapper(const std::string& hlsl, bool vertex) {
  std::string w;
  if (vertex) {
    w = hlsl;
    if (g_temps_mode) {
      const std::string from = "[unroll] for (uint k = 0; k < 16u; ++k) o.interp[k] = xe_o[k];";
      const size_t p = w.find(from), rp = w.find("float4 r[");
      if (p != std::string::npos && rp != std::string::npos) {
        const uint32_t n = uint32_t(std::strtoul(w.c_str() + rp + 9, nullptr, 10));
        w.replace(p, from.size(), "[unroll] for (uint k = 0; k < 16u; ++k) o.interp[k] = (k < " + std::to_string(n) +
                                      "u) ? r[min(k, " + std::to_string(n ? n - 1 : 0) + "u)] : float4(0.0, 0.0, 0.0, 0.0);");
      }
    }
    w += "\n// ---- Gears2-Native S3.4 oracle wrapper ----\n"
         "XE_VK(0, 3) RWStructuredBuffer<float4> xe_oracle_out : register(u0, space0);\n"
         "[numthreads(64, 1, 1)] void xe_oracle_main(uint3 xe_tid : SV_DispatchThreadID) {\n"
         "  XeVsOut xe_oo = main(xe_tid.x);\n"
         "  [unroll] for (uint xe_k = 0; xe_k < 16u; ++xe_k) xe_oracle_out[xe_tid.x * 17u + xe_k] = xe_oo.interp[xe_k];\n"
         "  xe_oracle_out[xe_tid.x * 17u + 16u] = xe_oo.position;\n"
         "}\n";
  } else {
    std::string body = hlsl;
    if (g_temps_mode) {  // pixel: oC0-oC3 carry r0-r3
      const std::string from = "  XePsOut o;\n";
      const size_t p = body.find(from), rp = body.find("float4 r[");
      if (p != std::string::npos && rp != std::string::npos) {
        const uint32_t n = uint32_t(std::strtoul(body.c_str() + rp + 9, nullptr, 10));
        body.insert(p + from.size(), "  [unroll] for (uint xe_t = 0; xe_t < 4u; ++xe_t) xe_oc[xe_t] = (xe_t < " +
                                         std::to_string(n) + "u) ? r[min(xe_t, " + std::to_string(n ? n - 1 : 0) +
                                         "u)] : float4(0.0, 0.0, 0.0, 0.0);\n");
      }
    }
    for (size_t p = 0; (p = body.find("discard;", p)) != std::string::npos;) {
      body.replace(p, 8, "{ xe_killed = true; }");
      p += 21;
    }
    w = "static bool xe_killed;\n" + body;
    w += "\n// ---- Gears2-Native S3.4 oracle wrapper ----\n"
         "XE_VK(97, 1) StructuredBuffer<float4> xe_oracle_in : register(t97, space0);\n"
         "XE_VK(0, 3) RWStructuredBuffer<float4> xe_oracle_out : register(u0, space0);\n"
         "[numthreads(64, 1, 1)] void xe_oracle_main(uint3 xe_tid : SV_DispatchThreadID) {\n"
         "  XePsIn xe_ii;\n"
         "  [unroll] for (uint xe_k = 0; xe_k < 16u; ++xe_k) xe_ii.interp[xe_k] = xe_oracle_in[xe_tid.x * 16u + xe_k];\n"
         "  xe_ii.position = float4(float(xe_tid.x) + 0.5, 0.5, 0.5, 1.0);\n"
         "  xe_ii.front_face = true;\n"
         "  xe_killed = false;\n"
         "  XePsOut xe_oo = main(xe_ii);\n"
         "  uint xe_b = xe_tid.x * 6u;\n"
         "  xe_oracle_out[xe_b + 0u] = xe_oo.color0;\n"
         "  xe_oracle_out[xe_b + 1u] = xe_oo.color1;\n"
         "  xe_oracle_out[xe_b + 2u] = xe_oo.color2;\n"
         "  xe_oracle_out[xe_b + 3u] = xe_oo.color3;\n"
         "#ifdef XE_WRITES_DEPTH\n"
         "  xe_oracle_out[xe_b + 4u] = float4(xe_oo.depth, 0.0, 0.0, 0.0);\n"
         "#else\n"
         "  xe_oracle_out[xe_b + 4u] = float4(0.0, 0.0, 0.0, 0.0);\n"
         "#endif\n"
         "  xe_oracle_out[xe_b + 5u] = float4(xe_killed ? 1.0 : 0.0, 0.0, 0.0, 0.0);\n"
         "}\n";
  }
  return w;
}

// ---- Comparison -------------------------------------------------------------------------------------------------

struct Tolerance {
  double rel = 1e-4;   // relative difference allowed for CLOSE
  double abs = 1e-6;   // absolute difference allowed for CLOSE (results near zero)
};

struct Compare {
  uint64_t components = 0, exact = 0, zero_sign = 0, close = 0, mismatch = 0, kill_mismatch = 0, lanes = 0,
           skipped_lanes = 0, killed_lanes = 0;
  uint64_t pos_mismatch = 0;     // vertex: oPos components that differ beyond tolerance (depth-equality risk)
  uint64_t interp_mismatch = 0;  // vertex: interpolator (o0-o15) components that differ beyond tolerance
  uint64_t pos_inexact = 0;      // vertex: oPos components that are not bit-identical (CLOSE or zero_sign included)
  uint64_t unwritten_skipped = 0;  // pixel: components the interpreter did not export, not compared
  uint64_t denorm_flushed = 0;     // equal once denormals are flushed to zero (the GPU runs -denorm ftz)
  uint64_t envelope = 0;           // outside the fixed tolerance, inside the CPU's transcendental-bias envelope
  uint64_t nan_divergence = 0;     // exactly one side NaN, in a set with NaN/Inf inputs (4, 7) or the other side Inf
  uint64_t ill_conditioned = 0;    // outside the envelope, but the CPU's own envelope is wider than the tolerance: the
                                   // output amplifies last-bit transcendental differences (cancellation)
  double max_rel = 0.0;
  std::string first_mismatch, first_pos_mismatch, first_nan;
};

// Pixel shaders: which colour/depth outputs (oC0-oC3, oDepth) the interpreter exported, per test case. A shader whose
// set varies between test cases writes its outputs dynamically (D3D9 leaves an unwritten output untouched, D3D10+
// does not) - candidates for the translator's merged-output fix (D29).
struct WrittenSets {
  std::map<uint32_t, uint64_t> register_sets;  // register mask (bit n = oC n, bit 4 = oDepth) -> lane count
  std::map<uint64_t, uint64_t> component_sets; // component mask -> lane count
};

WrittenSets CollectWrittenSets(const CpuResult& cpu) {
  WrittenSets w;
  for (size_t i = 0; i < cpu.written.size(); ++i) {
    if (cpu.skipped[i]) continue;
    if (cpu.out[i * kPsOutputs * 4 + 5 * 4] != 0.0f) continue;  // killed: outputs are discarded anyway
    const uint64_t m = cpu.written[i] & 0xFFFFFu;
    uint32_t regs = 0;
    for (uint32_t o = 0; o < 5; ++o)
      if ((m >> (o * 4)) & 0xF) regs |= 1u << o;
    ++w.register_sets[regs];
    ++w.component_sets[m];
  }
  return w;
}

std::string DescribeRegisterMask(uint32_t regs) {
  static const char* kNames[5] = {"oC0", "oC1", "oC2", "oC3", "oDepth"};
  std::string s;
  for (uint32_t o = 0; o < 5; ++o)
    if (regs & (1u << o)) s += (s.empty() ? "" : "+") + std::string(kNames[o]);
  return s.empty() ? "none" : s;
}

float FlushDenormalOut(float v) {
  return (FloatBits(v) & 0x7F800000u) == 0 ? BitsFloat(FloatBits(v) & 0x80000000u) : v;
}

// cpu_lo / cpu_hi: the interpreter run with transcendental bias -1 / +1 (empty when the shader has none).
void CompareOutputs(bool vertex, const std::vector<float>& gpu, const CpuResult& cpu, const CpuResult* cpu_lo,
                    const CpuResult* cpu_hi, const Tolerance& tol, Compare& c) {
  const uint32_t outputs = vertex ? kVsOutputs : kPsOutputs;
  static const char* kVsNames[17] = {"o0", "o1", "o2", "o3", "o4", "o5", "o6", "o7", "o8", "o9", "o10", "o11",
                                     "o12", "o13", "o14", "o15", "oPos"};
  static const char* kPsNames[6] = {"oC0", "oC1", "oC2", "oC3", "oDepth", "kill"};
  for (uint32_t s = 0; s < kSets; ++s) {
    for (uint32_t lane = 0; lane < kLanes; ++lane) {
      if (cpu.skipped[size_t(s) * kLanes + lane]) {
        ++c.skipped_lanes;
        continue;
      }
      ++c.lanes;
      const size_t base = (size_t(s) * kLanes + lane) * outputs * 4;
      if (!vertex) {
        const bool kg = gpu[base + 5 * 4] != 0.0f, kc = cpu.out[base + 5 * 4] != 0.0f;
        if (kg != kc) {
          ++c.kill_mismatch;
          if (c.first_mismatch.empty()) {
            char b[160];
            std::snprintf(b, sizeof(b), "set %u lane %u kill: gpu %d cpu %d", s, lane, int(kg), int(kc));
            c.first_mismatch = b;
          }
          continue;
        }
        if (kc) {  // both killed: the colour outputs are discarded, nothing more to compare
          ++c.killed_lanes;
          continue;
        }
      }
      const uint32_t compared_outputs = vertex ? kVsOutputs : 5;
      // Position with a non-finite W: the completion's 0 * W depends on how the compiler treats it - skip.
      const bool skip_position = vertex && !std::isfinite(cpu.out[base + 16 * 4 + 3]);
      const uint64_t written = cpu.written[size_t(s) * kLanes + lane];
      for (uint32_t o = 0; o < compared_outputs; ++o) {
        if (skip_position && o == 16) continue;
        const bool is_pos = vertex && o == 16;
        for (uint32_t k = 0; k < 4; ++k) {
          // Pixel: compare only what the interpreter exported in this test case (an unwritten output has no defined
          // value on D3D10+; the translator's choice there is checked by the merged-output work, not here).
          if (!vertex && !((written >> (o * 4 + k)) & 1)) {
            ++c.unwritten_skipped;
            continue;
          }
          const float g = gpu[base + o * 4 + k], r = cpu.out[base + o * 4 + k];
          ++c.components;
          if (FloatBits(g) == FloatBits(r) || (std::isnan(g) && std::isnan(r))) {
            ++c.exact;
            continue;
          }
          if (is_pos) ++c.pos_inexact;
          if (g == r) {  // +0 vs -0
            ++c.zero_sign;
            continue;
          }
          {
            const float gf = FlushDenormalOut(g), rf = FlushDenormalOut(r);
            if (FloatBits(gf) == FloatBits(rf) || gf == rf) {
              ++c.denorm_flushed;
              continue;
            }
          }
          if (std::isnan(g) != std::isnan(r) && ((s == 4 || s == 7) || (!std::isfinite(g) && !std::isfinite(r)))) {
            ++c.nan_divergence;
            if (c.first_nan.empty()) {
              char b[200];
              std::snprintf(b, sizeof(b), "set %u lane %u %s.%c: gpu %.9g (%08X) cpu %.9g (%08X)", s, lane,
                            vertex ? kVsNames[o] : kPsNames[o], "xyzw"[k], double(g), FloatBits(g), double(r),
                            FloatBits(r));
              c.first_nan = b;
            }
            continue;
          }
          bool ok = false;
          if (std::isfinite(g) && std::isfinite(r)) {
            const double d = std::fabs(double(g) - double(r));
            const double m = std::max(std::fabs(double(g)), std::fabs(double(r)));
            const double rel = m > 0 ? d / m : 0.0;
            if (rel <= tol.rel || d <= tol.abs) {
              ok = true;
              c.max_rel = std::max(c.max_rel, rel);
            }
          }
          bool in_envelope = false, sensitive = false;
          if (!ok && cpu_lo && cpu_hi && std::isfinite(g) && std::isfinite(r)) {
            const size_t idx = base + o * 4 + k;
            const float a = cpu_lo->out[idx], b = cpu_hi->out[idx];
            if (std::isfinite(a) && std::isfinite(b)) {
              const double lo = std::min({double(a), double(b), double(r)}),
                           hi = std::max({double(a), double(b), double(r)});
              const double slack = std::max(tol.abs, tol.rel * std::max(std::fabs(lo), std::fabs(hi)));
              in_envelope = double(g) >= lo - slack && double(g) <= hi + slack;
              sensitive = (hi - lo) > slack;
            }
          }
          if (ok) {
            ++c.close;
          } else if (in_envelope) {
            ++c.envelope;
          } else if (sensitive) {
            ++c.ill_conditioned;
          } else {
            ++c.mismatch;
            if (vertex) ++(is_pos ? c.pos_mismatch : c.interp_mismatch);
            char b[200];
            std::snprintf(b, sizeof(b), "set %u lane %u %s.%c: gpu %.9g (%08X) cpu %.9g (%08X)", s, lane,
                          vertex ? kVsNames[o] : kPsNames[o], "xyzw"[k], double(g), FloatBits(g), double(r),
                          FloatBits(r));
            if (c.first_mismatch.empty()) c.first_mismatch = b;
            if (is_pos && c.first_pos_mismatch.empty()) c.first_pos_mismatch = b;
          }
        }
      }
    }
  }
}

std::map<std::string, std::string> LoadSimplifications(const fs::path& p) {
  std::map<std::string, std::string> m;
  std::ifstream f(p);
  std::string line;
  while (std::getline(f, line)) {
    const size_t a = line.find(',');
    if (a == std::string::npos) continue;
    const size_t b = line.find(',', a + 1);
    const std::string name = line.substr(0, a), feature = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    if (name == "hash" || name == "name") continue;
    m[name] += (m[name].empty() ? "" : "+") + feature;
  }
  return m;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: gn_shader_oracle <corpus_dir> <out_dir> [--dxc <dxc.exe>] [--shard i/n] [--only <name>]\n"
                 "                        [--simplifications <csv>] [--rel-tol x] [--abs-tol x] [--limit N]\n"
                 "                        [--compile-only | --cpu-only] [--keep-files] [--vulkan] [--no-envelope]\n"
                 "                        [--dump set:lane]   (--only also takes a,b,c or @file)\n");
    return 2;
  }
  const fs::path corpus = argv[1], out_dir = argv[2];
  std::string dxc = "dxc", only;
  uint32_t dump_set = UINT32_MAX, dump_lane = UINT32_MAX;  // --dump set:lane prints both sides' outputs for that case
  fs::path simplifications_path;
  size_t shard = 0, shards = 1, limit = 0;
  bool compile_only = false, cpu_only = false, keep_files = false, vulkan = false, no_envelope = false, probe = false;
  Tolerance tol;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dxc" && i + 1 < argc) dxc = argv[++i];
    else if (a == "--only" && i + 1 < argc) only = argv[++i];
    else if (a == "--dump" && i + 1 < argc) std::sscanf(argv[++i], "%u:%u", &dump_set, &dump_lane);
    else if (a == "--shard" && i + 1 < argc) std::sscanf(argv[++i], "%zu/%zu", &shard, &shards);
    else if (a == "--simplifications" && i + 1 < argc) simplifications_path = argv[++i];
    else if (a == "--rel-tol" && i + 1 < argc) tol.rel = std::atof(argv[++i]);
    else if (a == "--abs-tol" && i + 1 < argc) tol.abs = std::atof(argv[++i]);
    else if (a == "--limit" && i + 1 < argc) limit = size_t(std::atoll(argv[++i]));
    else if (a == "--compile-only") compile_only = true;
    else if (a == "--cpu-only") cpu_only = true;
    else if (a == "--keep-files") keep_files = true;
    else if (a == "--vulkan") vulkan = true;
    else if (a == "--no-envelope") no_envelope = true;
    else if (a == "--temps") g_temps_mode = true;
    else if (a == "--no-denormals") g_no_denormals = true;
    else if (a == "--keep-denormals") rg::ShaderInterpreter::SetFlushDenormals(false);  // CPU side, see SDK patch 11
    else if (a == "--probe") probe = true;
  }
  if (shards == 0) shards = 1;
  fs::create_directories(out_dir);
  const auto simplified = simplifications_path.empty() ? std::map<std::string, std::string>()
                                                        : LoadSimplifications(simplifications_path);

  // --only: one name, a comma-separated list, or @file (one name per line).
  std::set<std::string> only_set;
  if (!only.empty()) {
    std::string list = only;
    if (list[0] == '@') {
      std::ifstream lf(list.substr(1));
      std::stringstream ss;
      ss << lf.rdbuf();
      list = ss.str();
    }
    std::string cur;
    for (char ch : list + ",") {
      if (ch == ',' || ch == '\n' || ch == '\r' || ch == ' ') {
        if (!cur.empty()) only_set.insert(cur);
        cur.clear();
      } else {
        cur += ch;
      }
    }
  }
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(corpus))
    if (e.path().extension() == ".ucode") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  std::vector<fs::path> mine;
  for (size_t i = 0; i < files.size(); ++i) {
    const std::string name = files[i].stem().string();
    if (!only_set.empty() && !only_set.count(name)) continue;
    if (only.empty() && i % shards != shard) continue;
    mine.push_back(files[i]);
  }
  if (limit && mine.size() > limit) mine.resize(limit);

  const std::vector<uint8_t> guest_memory = MakeGuestMemory();
  std::unique_ptr<GpuRunner> gpu;
  if (!compile_only && !cpu_only) {
    gpu = vulkan ? CreateVulkanRunner() : CreateGpuRunner();
    std::string err;
    if (!gpu || !gpu->Init(guest_memory.data(), err)) {
      std::printf("GPU: not available (%s) - stopping; use --cpu-only or --compile-only\n",
                  gpu ? err.c_str() : "no D3D12 on this platform");
      return 3;
    }
    std::printf("GPU: %s\n", gpu->Describe().c_str());
  }
  auto compile_cmd = [&](const fs::path& hlsl_path, const fs::path& out_path) {
    return Quote(dxc) + " " + Quote("-T") + " " + Quote("cs_6_6") + " " + Quote("-E") + " " + Quote("xe_oracle_main") +
           " " + Quote("-HV") + " " + Quote("2021") + " " + Quote(hlsl_path.string()) + " " + Quote("-Gis") + " " +
           (vulkan ? Quote("-spirv") + " " + Quote("-fspv-target-env=vulkan1.3") : Quote("-denorm") + " " + Quote("ftz")) +
           " " + Quote("-Fo") + " " + Quote(out_path.string());
  };
  if (probe) {
    // Edge-value probe: the translator's prelude (from the first vertex shader) plus the probe entry point.
    if (!gpu) {
      std::printf("PROBE: needs a GPU (not with --cpu-only / --compile-only)\n");
      return 3;
    }
    std::string prelude;
    for (const fs::path& path : files) {
      if (path.stem().string().rfind("vs-", 0) != 0) continue;
      const std::vector<uint8_t> bytes = ReadFile(path);
      std::vector<uint32_t> dw(bytes.size() / 4);
      std::memcpy(dw.data(), bytes.data(), dw.size() * 4);
      const xenos::hlsl::Translation tr = xenos::hlsl::TranslateToHlsl(xenos::ucode::Decode(dw.data(), dw.size(), true));
      const size_t cut = tr.hlsl.find("XeVsOut main(");
      if (cut != std::string::npos) prelude = tr.hlsl.substr(0, cut);
      break;
    }
    if (prelude.empty()) {
      std::printf("PROBE: no vertex shader in the corpus to take the prelude from\n");
      return 2;
    }
    const fs::path hp = out_dir / "probe.cs.hlsl", op = out_dir / "probe.cs.bin", lp = out_dir / "probe.cs.log";
    WriteText(hp, ProbeHlsl(prelude));
    const std::string cmd = compile_cmd(hp, op);
    std::printf("CMD: %s\n", cmd.c_str());
    if (RunProcess(cmd, lp, 120000) != 0) {
      std::printf("PROBE: compile failed, see %s\n", lp.string().c_str());
      return 1;
    }
    std::string probe_note;
    SetInputs base = MakeSet(0, true, ShaderFacts{}, probe_note);
    GpuSet g = ToGpuSet(base);
    ProbeConstants(g.cb1);
    std::vector<GpuSet> gs(kSets, g);
    std::vector<float> out;
    std::string err;
    if (!gpu->Run(ReadFile(op), true, gs, out, err)) {
      std::printf("PROBE: GPU run failed: %s\n", err.c_str());
      return 1;
    }
    ReportProbe(out);
    return 0;
  }
  std::printf("oracle: %zu shader(s), shard %zu/%zu, mode %s, %u sets x %u lanes, tolerance rel %g abs %g\n",
              mine.size(), shard, shards, compile_only ? "compile-only" : cpu_only ? "cpu-only" : "full", kSets, kLanes,
              tol.rel, tol.abs);

  std::ofstream csv(out_dir / ("oracle-" + std::to_string(shard) + "-of-" + std::to_string(shards) + ".csv"));
  csv << "name,type,verdict,lanes,skipped_lanes,killed_lanes,components,exact,zero_sign,close,mismatch,"
         "kill_mismatch,max_rel,flags,a0_reads,a0_min,a0_max,pos_mismatch,pos_inexact,interp_mismatch,"
         "unwritten_skipped,ps_written_sets,first_mismatch,first_pos_mismatch,denorm_flushed,envelope,nan_divergence,"
         "first_nan,ill_conditioned\n";
  size_t n_pos_mismatch = 0, n_pos_inexact = 0, n_interp_only = 0, n_written_varies = 0;
  std::map<std::string, size_t> verdicts;
  bool printed_cmd = false;
  const auto t_start = std::chrono::steady_clock::now();
  size_t done = 0;
  for (const fs::path& path : mine) {
    const std::string name = path.stem().string();
    const bool vertex = name.rfind("vs-", 0) == 0;
    const std::vector<uint8_t> bytes = ReadFile(path);
    std::vector<uint32_t> dwords(bytes.size() / 4);
    std::memcpy(dwords.data(), bytes.data(), dwords.size() * 4);
    // The interpreter may read a little past the end in a malformed shader; pad with zeros.
    std::vector<uint32_t> padded = dwords;
    padded.resize(dwords.size() + 3 * 64, 0);

    const xenos::ucode::Program program = xenos::ucode::Decode(dwords.data(), dwords.size(), vertex);
    const xenos::hlsl::Translation tr = xenos::hlsl::TranslateToHlsl(program);
    const ShaderFacts facts = ScanProgram(program);

    std::string flags;
    if (simplified.count(name)) flags += "expected:" + simplified.at(name) + " ";
    if (facts.uses_texture_fetch) flags += "tex_standin ";
    if (facts.uses_transcendental) flags += "transcendental ";
    if (facts.uses_a0) flags += "a0 ";
    if (facts.uses_kill) flags += "kill ";
    if (tr.uses_pc_loop) flags += "pc_loop ";

    std::vector<SetInputs> sets;
    std::string note;
    for (uint32_t s = 0; s < kSets; ++s) sets.push_back(MakeSet(s, vertex, facts, note));
    if (!note.empty()) flags += note;

    std::string verdict, first;
    Compare cmp;
    CpuResult cpu;
    // CPU first (cheap): also proves the interpreter terminates on every shader.
    if (!compile_only) cpu = RunCpu(padded, vertex, sets, guest_memory.data());
    // Transcendental envelope (SDK patch 9): the same inputs with every exp/log/rcp/rsq/sqrt/sin/cos result moved by
    // -2^-21 and +2^-21 relative. A GPU value between the two (or the exact run) is precision, not a translator error.
    // Then 4 runs with pseudo-random directions per result, so errors that cancel under a uniform bias are covered
    // too. cpu_lo / cpu_hi hold the per-component minimum / maximum over all six runs.
    CpuResult cpu_lo, cpu_hi;
    if (!compile_only && !cpu_only && facts.uses_transcendental && !no_envelope) {
      for (int bias : {-1, 1, 2, 3, 4, 5}) {
        CpuResult v = RunCpu(padded, vertex, sets, guest_memory.data(), bias);
        if (cpu_lo.out.empty()) {
          cpu_lo = v;
          cpu_hi = v;
          continue;
        }
        for (size_t i = 0; i < v.out.size(); ++i) {
          const float x = v.out[i];
          if (std::isnan(x) || std::isnan(cpu_lo.out[i])) {
            cpu_lo.out[i] = cpu_hi.out[i] = std::numeric_limits<float>::quiet_NaN();  // no envelope here
            continue;
          }
          cpu_lo.out[i] = std::min(cpu_lo.out[i], x);
          cpu_hi.out[i] = std::max(cpu_hi.out[i], x);
        }
      }
    }

    std::vector<uint8_t> dxil;
    if (!cpu_only) {
      const fs::path hlsl_path = out_dir / (name + ".cs.hlsl"), dxil_path = out_dir / (name + ".cs.dxil"),
                     log_path = out_dir / (name + ".cs.log");
      WriteText(hlsl_path, MakeComputeWrapper(tr.hlsl, vertex));
      const std::string cmd = Quote(dxc) + " " + Quote("-T") + " " + Quote("cs_6_6") + " " + Quote("-E") + " " +
                              Quote("xe_oracle_main") + " " + Quote("-HV") + " " + Quote("2021") + " " +
                              Quote(hlsl_path.string()) + " " + Quote("-Gis") + " " +
                              (vulkan ? Quote("-spirv") + " " + Quote("-fspv-target-env=vulkan1.3")
                                      : Quote("-denorm") + " " + Quote("ftz")) +
                              " " + Quote("-Fo") + " " + Quote(dxil_path.string());
#ifdef _WIN32
      const std::string run = cmd;  // CreateProcess takes the line as is
#else
      const std::string run = cmd;
#endif
      if (!printed_cmd) {
        std::printf("CMD: %s\n", run.c_str());
        printed_cmd = true;
      }
      const int rc = RunProcess(run, log_path, 120000);
      if (rc != 0) {
        verdict = "ERROR";
        std::ifstream lf(log_path);
        std::string line;
        while (std::getline(lf, line))
          if (line.find("error") != std::string::npos) { first = "compile: " + line; break; }
        if (first.empty()) first = "compile failed (rc " + std::to_string(rc) + "), see " + log_path.filename().string();
      } else {
        dxil = ReadFile(dxil_path);
        if (!keep_files) {
          std::error_code ec;
          fs::remove(hlsl_path, ec);
          fs::remove(log_path, ec);
          fs::remove(dxil_path, ec);
        }
      }
    }

    if (verdict.empty()) {
      if (compile_only) {
        verdict = "COMPILED";
      } else if (cpu_only) {
        verdict = "CPU_RAN";
        size_t skipped = 0;
        for (uint8_t sk : cpu.skipped) skipped += sk;
        if (skipped) first = std::to_string(skipped) + " lane(s) hit the interpreter step limit";
      } else {
        std::vector<GpuSet> gsets;
        for (const auto& s : sets) gsets.push_back(ToGpuSet(s));
        std::vector<float> gout;
        std::string err;
        if (!gpu->Run(dxil, vertex, gsets, gout, err)) {
          verdict = "ERROR";
          first = "gpu: " + err;
          if (err.find("DEVICE_REMOVED") != std::string::npos || err.find("device removed") != std::string::npos) {
            std::printf("%s ERROR %s - device lost, stopping\n", name.c_str(), err.c_str());
            csv << name << "," << (vertex ? "vs" : "ps") << ",ERROR,,,,,,,,,,,\"" << flags << "\",,,,,,,,,\"" << first
                << "\",,,,,,\n";
            break;
          }
        } else {
          CompareOutputs(vertex, gout, cpu, cpu_lo.out.empty() ? nullptr : &cpu_lo,
                         cpu_hi.out.empty() ? nullptr : &cpu_hi, tol, cmp);
          if (dump_set < kSets && dump_lane < kLanes) {
            const uint32_t outputs = vertex ? kVsOutputs : kPsOutputs;
            const size_t base = (size_t(dump_set) * kLanes + dump_lane) * outputs * 4;
            std::printf("%s DUMP set %u lane %u written %016" PRIx64 "\n", name.c_str(), dump_set, dump_lane,
                        cpu.written[size_t(dump_set) * kLanes + dump_lane]);
            if (!vertex) {
              for (uint32_t r = 0; r < 16; ++r) {
                const float* in = &sets[dump_set].pixel_inputs[size_t(dump_lane) * 64 + r * 4];
                std::printf("  in%-2u  %08X %08X %08X %08X  (%g %g %g %g)\n", r, FloatBits(in[0]), FloatBits(in[1]),
                            FloatBits(in[2]), FloatBits(in[3]), in[0], in[1], in[2], in[3]);
              }
            }
            for (uint32_t o = 0; o < outputs; ++o) {
              std::printf("  out%-2u", o);
              for (uint32_t k = 0; k < 4; ++k)
                std::printf("  gpu %08X cpu %08X%s", FloatBits(gout[base + o * 4 + k]), FloatBits(cpu.out[base + o * 4 + k]),
                            FloatBits(gout[base + o * 4 + k]) == FloatBits(cpu.out[base + o * 4 + k]) ? " " : "*");
              std::printf("\n");
            }
          }
          if (cmp.lanes == 0) verdict = "SKIP";
          else if (cmp.mismatch || cmp.kill_mismatch) verdict = "FAIL";
          else if (cmp.nan_divergence) verdict = "NAN_ONLY";
          else if (cmp.ill_conditioned) verdict = "PRECISION";
          else if (cmp.close || cmp.zero_sign || cmp.denorm_flushed || cmp.envelope) verdict = "CLOSE";
          else verdict = "EXACT";
          first = cmp.first_mismatch.empty() ? cmp.first_nan : cmp.first_mismatch;
        }
      }
    }
    // Pixel: the set of exported outputs per test case (from the CPU run, so also available with --cpu-only).
    std::string written_desc;
    bool written_varies = false;
    if (!vertex && !compile_only && !cpu.written.empty()) {
      const WrittenSets ws = CollectWrittenSets(cpu);
      for (const auto& [regs, n] : ws.register_sets)
        written_desc += (written_desc.empty() ? "" : " ") + DescribeRegisterMask(regs) + ":" + std::to_string(n);
      written_varies = ws.register_sets.size() > 1;
      if (written_varies) {
        flags += "ps_out_varies ";
        ++n_written_varies;
      } else if (ws.component_sets.size() > 1) {
        flags += "ps_out_components_vary ";
      }
    }
    if (vertex && cmp.pos_mismatch) {
      flags += "opos_mismatch ";
      ++n_pos_mismatch;
    } else if (vertex && cmp.interp_mismatch) {
      flags += "interp_mismatch ";
      ++n_interp_only;
    }
    if (vertex && cmp.pos_inexact) {
      flags += "opos_not_bit_exact ";
      ++n_pos_inexact;
    }
    ++verdicts[verdict];
    char rel[32];
    std::snprintf(rel, sizeof(rel), "%.3g", cmp.max_rel);
    csv << name << "," << (vertex ? "vs" : "ps") << "," << verdict << "," << cmp.lanes << "," << cmp.skipped_lanes
        << "," << cmp.killed_lanes << "," << cmp.components << "," << cmp.exact << "," << cmp.zero_sign << ","
        << cmp.close << "," << cmp.mismatch << "," << cmp.kill_mismatch << "," << rel << ",\"" << flags << "\","
        << cpu.a0_reads << "," << cpu.a0_min << "," << cpu.a0_max << "," << cmp.pos_mismatch << ","
        << cmp.pos_inexact << "," << cmp.interp_mismatch << "," << cmp.unwritten_skipped << ",\"" << written_desc
        << "\",\"" << first << "\",\"" << cmp.first_pos_mismatch << "\"," << cmp.denorm_flushed << ","
        << cmp.envelope << "," << cmp.nan_divergence << ",\"" << cmp.first_nan << "\"," << cmp.ill_conditioned << "\n";
    csv.flush();
    const bool a0_out = cpu.a0_reads && (cpu.a0_min < 0 || cpu.a0_max > 255);
    if (verdict != "EXACT" && verdict != "COMPILED" && verdict != "CPU_RAN") {
      std::printf("%s %s %s| %s\n", name.c_str(), verdict.c_str(), flags.c_str(), first.c_str());
    } else if (!first.empty()) {
      std::printf("%s %s %s| %s\n", name.c_str(), verdict.c_str(), flags.c_str(), first.c_str());
    }
    if (written_varies)
      std::printf("%s PSOUT written outputs vary per test case (synthetic inputs): %s\n", name.c_str(),
                  written_desc.c_str());
    if (vertex && cmp.pos_inexact)
      std::printf("%s OPOS %" PRIu64 " oPos component(s) not bit-identical (%" PRIu64 " beyond tolerance)%s%s\n",
                  name.c_str(), cmp.pos_inexact, cmp.pos_mismatch, cmp.first_pos_mismatch.empty() ? "" : " | ",
                  cmp.first_pos_mismatch.c_str());
    if (a0_out) std::printf("%s A0 (synthetic inputs) relative index range %d..%d (outside 0-255) over %" PRIu64 " reads\n", name.c_str(),
                            cpu.a0_min, cpu.a0_max, cpu.a0_reads);
    if (++done % 100 == 0) {
      const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
      std::printf("progress: %zu/%zu shaders, %.0f s\n", done, mine.size(), secs);
      std::fflush(stdout);
    }
  }
  std::printf("SUMMARY shard %zu/%zu:", shard, shards);
  for (const auto& [v, n] : verdicts) std::printf(" %s %zu", v.c_str(), n);
  std::printf("\n");
  std::printf("SUMMARY-DETAIL shard %zu/%zu: vs_opos_mismatch %zu vs_opos_not_bit_exact %zu vs_interp_only_mismatch %zu "
              "ps_written_outputs_vary %zu\n",
              shard, shards, n_pos_mismatch, n_pos_inexact, n_interp_only, n_written_varies);
  return 0;
}
