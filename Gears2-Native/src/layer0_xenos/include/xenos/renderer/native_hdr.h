// Captured Gears 2 / Gears 3 TU6 HDR profiles; no game shader data.
#pragma once
#include <cstddef>
#include <algorithm>
#include <bit>
#include <cmath>
#include <string>
#include <utility>
#include <xenos/ucode.h>

namespace xenos::renderer::native_hdr {
inline constexpr uint64_t kScene = 0xB6B6DD4E7D91F3D9ull;
inline constexpr uint64_t kCanvas = 0xA9AC71DCCCEAEBE4ull;
inline constexpr uint64_t kSceneUi = 0xECE0CF7069CD4A91ull;
inline constexpr uint64_t kGears3Scene = 0x2F284B68A112ED3Eull;
inline bool IsScene(uint64_t hash) { return hash == kScene || hash == kGears3Scene; }
inline bool IsCanvasVertex(uint64_t hash) {
  return hash == 0x868DFF11A6BDEBAAull || hash == 0xE775468CA654846Dull ||
         hash == 0x6F83A894AB66D831ull || hash == 0x2E88B37100D9D0CEull;
}

struct Gears3Curve {
  float offset, scale, shadow_start, shadow_scale, power, weight_scale, blend;
  bool operator==(const Gears3Curve&) const = default;
  float Linear(float scene) const {
    const double x = std::max(double(scene), 0.0);
    const double power_curve = std::pow(std::max(x * std::abs(double(shadow_scale)), 0.0), double(power));
    const double rational = x * scale / std::max(std::abs(x + offset), 1e-6);
    const double weight = std::clamp((x - shadow_start) * weight_scale, 0.0, 1.0);
    const double mixed = power_curve + weight * (rational - power_curve);
    return float(std::pow(std::clamp(mixed + blend * (rational - mixed), 0.0, 1.0), 2.2));
  }
  std::pair<float, float> Extension() const {
    const float target = float(std::pow(std::clamp(std::abs(double(scale)), 0.01, 1.0) * 0.75, 2.2));
    float low = 0, high = std::max({std::abs(offset), shadow_start, 1e-4f});
    for (unsigned i = 0; i < 20 && Linear(high) < target; ++i) high *= 2;
    for (unsigned i = 0; i < 20; ++i) {
      const float middle = (low + high) * 0.5f;
      if (Linear(middle) < target) low = middle; else high = middle;
    }
    const float pivot = std::max((low + high) * 0.5f, 1e-5f);
    const float step = std::max(pivot * 0.001f, 1e-6f);
    const float slope = std::max((Linear(pivot + step) - Linear(std::max(pivot - step, 0.0f))) / (2 * step), 0.0f);
    return {pivot, std::isfinite(slope) ? slope : 0.0f};
  }
  static Gears3Curve FromConstants(const uint32_t* constants) {
    const auto value = [=](unsigned reg, unsigned component) { return std::bit_cast<float>(constants[4 * reg + component]); };
    return {value(4, 0), value(4, 1), value(4, 2), value(4, 3), value(252, 1), value(253, 2), value(5, 0)};
  }
};

// Curve constants usually remain unchanged for many frames. Avoid repeating
// the tangent's root search until any of its seven source parameters changes.
class Gears3CurveCache {
 public:
  std::pair<float, float> Get(const Gears3Curve& curve) {
    if (!valid_ || !(curve == curve_)) {
      curve_ = curve;
      extension_ = curve.Extension();
      valid_ = true;
    }
    return extension_;
  }
 private:
  Gears3Curve curve_{};
  std::pair<float, float> extension_{};
  bool valid_ = false;
};

// Gears 3 decodes its packed HDR lighting before a filmic curve and a 3D LUT.
// Retain the complete original shader, including DOF/bloom/grain/LUT grading.
// Capture its real scene input to the curve, then add a slope-matched highlight
// extension. A uniform gain retains the LUT's hue instead of stretching white
// pixels inferred from a finished SDR image. Values below the pivot are exact.
inline std::string Gears3SceneSource(std::string source, uint32_t low, uint32_t high) {
  const auto constant = [=](uint32_t index) {
    if (low != high && index >= high) index = low + index - high;
    return "xe_c[" + std::to_string(index) + "]";
  };
  const auto replace = [&](const std::string& from, const std::string& to) {
    const auto at = source.find(from);
    if (at == std::string::npos || source.find(from, at + from.size()) != std::string::npos) return false;
    source.replace(at, from.size(), to);
    return true;
  };
  const std::string function = "XePsOut main(XePsIn xe_in) {";
  std::string curve = R"(
float NativeGears3CurveLinear(float scene) {
  float4 curve = $C4;
  float powerCurve = pow(max(scene * abs(curve.w), 0.0), $C252.y);
  float rationalCurve = scene * $C4.y / max(abs(scene + curve.x), 1e-6);
  float weight = saturate((scene - curve.z) * $C253.z);
  float blended = lerp(powerCurve, rationalCurve, weight);
  return pow(max(saturate(lerp(blended, rationalCurve, $C5.x)), 0.0), 2.2);
}
)";
  for (const auto& item : {std::pair{"$C252", constant(252)}, std::pair{"$C253", constant(253)},
                           std::pair{"$C4", constant(4)}, std::pair{"$C5", constant(5)}}) {
    size_t at = 0;
    while ((at = curve.find(item.first, at)) != std::string::npos) {
      curve.replace(at, std::string(item.first).size(), item.second); at += item.second.size();
    }
  }
  if (!replace(function, curve + function + "\n  float3 nativeSceneForCurve = 0.0;")) return {};
  const std::string capture = "float4 s0 = r[3].xzyy;\n        float4 s1 = " + constant(4) + ".xxxx;";
  if (!replace(capture, "nativeSceneForCurve = max(r[3].xzy, 0.0);\n        " + capture)) return {};
  const std::string shoulder = "  float brightest = max(max(nativeColor.r, nativeColor.g), nativeColor.b);";
  const std::string extension = R"(
  // The CPU finds the 75%-white point of this draw's actual filmic curve and
  // supplies its tangent once per draw. Exposure changes keep their midtones.
  float sceneEnergy = max(max(nativeSceneForCurve.r, nativeSceneForCurve.g), nativeSceneForCurve.b);
  float pivot = max(asfloat(xe_sys2.z), 1e-5);
  float3 nativeGears3Grade = 1.0;
  if (sceneEnergy > pivot) {
    float atPivot = NativeGears3CurveLinear(pivot);
    float slope = max(asfloat(xe_sys2.w), 0.0);
    float extended = atPivot + slope * (sceneEnergy - pivot);
    float referenceCurve = NativeGears3CurveLinear(sceneEnergy);
    float extra = max(extended - referenceCurve, 0.0);
    // Retain the LUT and all game fade/output scaling as a throughput factor.
    // Normalizing by the graded peak would cancel a fade toward black. Apply
    // this factor AFTER the HDR shoulder so highlights fade proportionally too.
    nativeGears3Grade = nativeColor / max(referenceCurve, 1e-5);
    float displayHeadroom = max(asfloat(xe_sys2.x) - 1.0, 0.0);
    nativeColor = referenceCurve + extra * displayHeadroom;
  }
)";
  if (!replace(shoulder, extension + shoulder)) return {};
  const std::string rolloff = "  nativeColor *= shoulder / max(brightest, 1.0);";
  if (!replace(rolloff, rolloff + R"(
  nativeColor *= nativeGears3Grade;
  float displayPeak = max(asfloat(xe_sys2.x), 1e-5);
  float gradedPeak = max(max(nativeColor.r, nativeColor.g), nativeColor.b);
  nativeColor *= displayPeak / max(displayPeak, gradedPeak);
)")) return {};
  return source;
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
struct UiProfile { uint64_t hash; size_t dwords; uint64_t fingerprint; bool modulation = false; };
inline constexpr UiProfile kUiProfiles[] = {
    {kCanvas, 54, 0xD3A4073CB8DAEB0Dull},
    {0x3F3410143BA49FE9ull, 54, 0xBD1EBA583EEF9E4Dull},
    {0xCDA0455F4F4EAFB4ull, 36, 0x2E30DE46E0279C75ull},
    {0x790DEB5680F51A2Dull, 48, 0x890F2A778E81E0BDull},
    {0xCF8D65F2EAD24695ull, 84, 0x51231205354C123Bull},
    {kSceneUi, 222, 0x84FEA527E0089A1Dull},
    {0x56B153DABC9D68B3ull, 57, 0xD923D828C2CD577Aull},
    {0x3A795B52038F5F99ull, 36, 0x27ECDC4E8FE1A683ull},
    {0x7FA8DF4F5A89A67Eull, 195, 0x5B6ECACDD433C080ull},  // Gears 3 damage mask, no scene texture.
    {0xA7AB2D75C169DFDEull, 39, 0x0F571E5129A382D5ull},   // Gears 3 menu color.
    {0x19055C4D86C6C08Cull, 54, 0xA85FD2065640460Cull},   // Gears 3 menu atlas.
    {0xB8A5DDABA29740E5ull, 45, 0x74A8C52597E18F7Eull},   // Gears 3 menu image.
    {0x1DE36FC34E4B7E48ull, 129, 0xA3BC0986E1229B47ull},  // Gears 3 menu mask.
    {0xA8EEB25041FFBA45ull, 66, 0x28D5E5E1C3472B34ull, true}, // Multiplicative darkening, no UI white gain.
    {0x9FCCAF2C4923CA93ull, 129, 0x5CF7251E4670B002ull},  // Gears 3 blood overlay; three BC3 UI textures.
    {0x63C7939AE43787ABull, 54, 0x59ABADA2DC6A676Aull},   // Gears 3 additional damage/reload overlay.
    {0xD2B5DF9B2B04E715ull, 42, 0xFF8DB83E451A0BF5ull},   // Gears 3 solid-color UI; no textures.
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
