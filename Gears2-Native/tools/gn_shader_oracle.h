// Gears2-Native S3.4 shader oracle (notes/41): shared declarations between the portable part (gn_shader_oracle.cpp:
// inputs, the SDK's CPU interpreter, comparison, report) and the GPU part (gn_shader_oracle_d3d12.cpp). Our code (MIT).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gn_oracle {

// Test layout: every shader runs kSets constant sets (one dispatch each) of kLanes lanes (vertex index / pixel).
constexpr uint32_t kSets = 8;
constexpr uint32_t kLanes = 64;
// Outputs per lane, in float4s. Vertex: interpolators 0-15, position. Pixel: colour 0-3, (depth, 0, 0, 0),
// (killed, 0, 0, 0).
constexpr uint32_t kVsOutputs = 17;
constexpr uint32_t kPsOutputs = 6;
// Guest physical memory block given to both sides (vertex fetch reads it).
constexpr uint32_t kGuestMemoryBytes = 1u << 20;

// The uniform colour of texture slot k (0-31) for a dimension (0 = 2D and 1D, 1 = cube, 2 = 3D). Exact in fp32: every
// texel of that texture has this value, so any coordinate (and any filter) samples it; the ALU after the fetch is what
// gets compared (notes/41: the texture stand-in).
inline void SlotColour(uint32_t dimension, uint32_t k, float out[4]) {
  const float base = float(dimension);
  out[0] = base + float(1 + k) / 128.0f;
  out[1] = base + float(33 + k) / 128.0f;
  out[2] = base + float(65 + k) / 128.0f;
  out[3] = base + float(97 + k) / 128.0f;
}

// One constant set's GPU inputs, laid out exactly as the translator's cbuffers (notes/39).
struct GpuSet {
  std::vector<uint8_t> cb0;           // XeSystem
  std::vector<uint8_t> cb1;           // float4 xe_c[256]
  std::vector<uint8_t> cb2;           // uint4 xe_bool[2]; uint4 xe_loop[8]
  std::vector<uint8_t> cb3;           // uint4 xe_fetch[48]
  std::vector<float> pixel_inputs;    // pixel shaders: kLanes x 16 float4 interpolators
};

class GpuRunner {
 public:
  virtual ~GpuRunner() = default;
  virtual bool Init(const uint8_t* guest_memory, std::string& error) = 0;
  virtual std::string Describe() const = 0;
  // Runs one compute-wrapped shader (DXIL bytes) for every set; out gets kSets x kLanes x outputs x 4 floats.
  virtual bool Run(const std::vector<uint8_t>& dxil, bool vertex, const std::vector<GpuSet>& sets,
                   std::vector<float>& out, std::string& error) = 0;
};

// D3D12 on Windows; on other platforms returns nullptr (the tool then runs --cpu-only / --compile-only).
std::unique_ptr<GpuRunner> CreateGpuRunner();
// Edge-value probe (gn_shader_oracle_probe.cpp, --probe): the HLSL (prelude of a translated vertex shader plus the
// probe entry point), its float constants, and the report (returns the number of ops that differ).
std::string ProbeHlsl(const std::string& vertex_prelude);
void ProbeConstants(std::vector<uint8_t>& cb1);
uint32_t ReportProbe(const std::vector<float>& out);
// Vulkan (SPIR-V) runner, built with -DGN_ORACLE_VULKAN (gn_shader_oracle_vulkan.cpp); nullptr otherwise.
std::unique_ptr<GpuRunner> CreateVulkanRunner();

}  // namespace gn_oracle
