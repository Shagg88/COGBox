// Captured Gears 2 / Gears 3 TU6 HDR profiles; no game shader data.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <xenos/renderer/psychov35_bridge.h>

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
  // Match the bounded original target's coverage without bounding world RGB.
  // Use identical coverage for both evaluations; FP16 permits alpha > 1 and
  // a negative inverse-source-alpha blend factor unless explicitly bounded.
  output.hdr.a = output.sdr.a = saturate(output.sdr.a);
  return output;
}
)";
  return material;
}
// Additional Canvas materials seen after the main scene. The animated reload
// material samples the same BC3 HUD atlas as kCanvas, plus an 8x128 mask ramp.
// They must be composed in float too; falling back here would clamp the entire
// firing frame, including the scene's unrelated HDR muzzle flash.
struct UiProfile {
  uint64_t hash;
  size_t dwords;
  uint64_t fingerprint;
  bool modulation = false;
  // Optional captured vertex pair. Such profiles also require the captured
  // blend: straight alpha for emission, destination-color/zero for modulation.
  // A material's other pipeline uses must not inherit the UI emission clamp.
  uint64_t vertex = 0;
};
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
    {0x2C1E4E099E31E037ull, 51, 0xB241E3475A37FF9Cull, false,
     0x5BF4E342D00F1593ull},  // Gears 3 two-sample UI blend; captured 160x80 BC3 image in both slots.
    {0x1C0932D0C22ECC2Aull, 21, 0x88FB1A623EDF043Full, true,
     0x2E88B37100D9D0CEull},  // Gears 3 destination-color UI modulation; captured 68x76 BC1 image.
};
inline const UiProfile* FindUi(uint64_t hash) {
  for (const auto& profile : kUiProfiles) if (profile.hash == hash) return &profile;
  return nullptr;
}

}  // namespace xenos::renderer::native_hdr
