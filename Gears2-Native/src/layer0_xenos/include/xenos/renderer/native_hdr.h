// Gears 2 TU6 HDR profile. Hashes and instruction shapes only; no game shader data.
#pragma once
#include <cstddef>
#include <string>
#include <xenos/ucode.h>

namespace xenos::renderer::native_hdr {
inline constexpr uint64_t kScene = 0xB6B6DD4E7D91F3D9ull;
inline constexpr uint64_t kCanvas = 0xA9AC71DCCCEAEBE4ull;
inline constexpr uint64_t kSceneUi = 0xECE0CF7069CD4A91ull;
inline bool IsCanvasVertex(uint64_t hash) {
  return hash == 0x868DFF11A6BDEBAAull || hash == 0xE775468CA654846Dull;
}

// Evaluate a scene-sampling Canvas material with both the real HDR scene and
// its original SDR resolve. The difference is scene energy, not UI emission.
// The generated game program stays transient; this contains only translator
// interface names and authored glue. Fail closed if that interface changes.
inline std::string SceneUiPairSource(std::string material) {
  const auto replace = [&](const std::string& from, const std::string& to) {
    const auto at = material.find(from);
    if (at == std::string::npos) return false;
    material.replace(at, from.size(), to);
    return true;
  };
  if (!replace("XePsOut xe_scene_ui_material(XePsIn xe_in)",
               "XE_VK(31, 1) Texture2D<float4> xe_hdr_scene : register(t31, space0);\n"
               "XePsOut xe_scene_ui_material(XePsIn xe_in, bool hdrScene)")) return {};
  // Keep the original 360 texture gamma below white. Extend only values that
  // the floating scene proxy can supply; all original SDR samples are <= 1.
  if (!replace("float xe_degamma1(float g) {",
               "float xe_degamma1(float g) {\n"
               "  if (g > 1.0) return (1016.0 / 1023.0) * pow(max(g, 0.0), 2.2);")) return {};
  const std::string sample = "xe_tex_2d_0.Sample(xe_sampler_0, tc)";
  size_t at = 0, samples = 0;
  while ((at = material.find(sample, at)) != std::string::npos) {
    const std::string pair = "(hdrScene ? xe_hdr_scene.Sample(xe_sampler_0, tc) : " + sample + ")";
    material.replace(at, sample.size(), pair);
    at += pair.size();
    ++samples;
  }
  if (samples != 4) return {};
  material += R"(
struct SceneUiPair { float4 hdr : SV_Target0; float4 sdr : SV_Target1; };
SceneUiPair main(XePsIn input) {
  SceneUiPair output;
  output.hdr = xe_scene_ui_material(input, true).color0;
  output.sdr = xe_scene_ui_material(input, false).color0;
  return output;
}
)";
  return material;
}
// Additional Canvas materials seen after the main scene. The animated reload
// material samples the same BC3 HUD atlas as kCanvas, plus an 8x128 mask ramp.
// They must be composed in float too; falling back here would clamp the entire
// firing frame, including the scene's unrelated HDR muzzle flash.
struct UiProfile { uint64_t hash; size_t dwords; uint64_t fingerprint; };
inline constexpr UiProfile kUiProfiles[] = {
    {kCanvas, 54, 0xD3A4073CB8DAEB0Dull},
    {0x3F3410143BA49FE9ull, 54, 0xBD1EBA583EEF9E4Dull},
    {0xCDA0455F4F4EAFB4ull, 36, 0x2E30DE46E0279C75ull},
    {0x790DEB5680F51A2Dull, 48, 0x890F2A778E81E0BDull},
    {0xCF8D65F2EAD24695ull, 84, 0x51231205354C123Bull},
    {kSceneUi, 222, 0x84FEA527E0089A1Dull},
};
inline const UiProfile* FindUi(uint64_t hash) {
  for (const auto& profile : kUiProfiles) if (profile.hash == hash) return &profile;
  return nullptr;
}

// Retain the game's exposure, DOF, bloom and grading. Extend its two SDR
// ceilings before gamma, using the original floating-point scene samples.
// The abs at 72 must become a nonnegative floor: otherwise negative shadow
// values exposed by removing the first ceiling would turn into positive light.
inline bool ExtendScene(ucode::Program& program) {
  ucode::Instruction *first = nullptr, *floor = nullptr, *last = nullptr;
  for (auto& block : program.control_flow) for (auto& instruction : block.instructions) {
    if (instruction.address == 70) first = &instruction;
    if (instruction.address == 72) floor = &instruction;
    if (instruction.address == 83) last = &instruction;
  }
  if (!first || !floor || !last || program.is_vertex_shader ||
      first->kind != ucode::Instruction::Kind::kAlu ||
      floor->kind != ucode::Instruction::Kind::kAlu ||
      last->kind != ucode::Instruction::Kind::kAlu ||
      first->alu.vector_opcode != ucode::AluVectorOpcode::kMad ||
      floor->alu.vector_opcode != ucode::AluVectorOpcode::kMax ||
      last->alu.vector_opcode != ucode::AluVectorOpcode::kMul ||
      !first->alu.vector_result.clamp || !last->alu.vector_result.clamp ||
      !floor->alu.vector_operands[0].absolute) return false;
  first->alu.vector_result.clamp = false;
  last->alu.vector_result.clamp = false;
  floor->alu.vector_operands[0].absolute = false;
  return true;
}
}  // namespace xenos::renderer::native_hdr
