// Renderer-level rules for late SDR-authored overlays over the native HDR scene.
// No game shader hashes. Authored code, MIT.
#pragma once
#include <algorithm>
#include <cstdint>
#include <xenos/renderer/gpu.h>

namespace xenos::renderer::native_hdr {
enum class OverlayMode : uint8_t { kNone, kUi, kPremultiplied, kModulation, kModulation2x, kFilterLinear, kFilterPremultiplied };
enum class FallbackReason : uint8_t { kNoScene, kGeneration, kTarget, kDepth, kMaterial,
  kSceneSetup, kShader, kResource, kVertex, kCount };
inline constexpr const char* kFallbackNames[] = {
  "no scene", "resolve generation", "target state", "depth/stencil attachment", "material eligibility",
  "scene geometry", "shader/pipeline", "resource allocation", "vertex twin"};

// Packed integer XY and packed colors are used by screen-space UI vertex
// streams. Float3/4 world positions, skinning and texture-fetch vertices are
// deliberately excluded; verified Canvas vertices can supply separate evidence.
inline bool PackedUiVertex(uint64_t formats, uint32_t textures, bool memexport) {
  constexpr uint64_t xy = uint64_t(1) << 25; // Xenos FMT_16_16
  constexpr uint64_t color = uint64_t(1) << 6; // FMT_8_8_8_8
  return !memexport && !textures && (formats & xy) && !(formats & ~(xy | color));
}

struct OverlayInputs {
  bool owned_target = false;
  bool screen_vertex = false;
  bool textures_available = false;
  bool scene_texture = false;
  bool cube_or_volume = false;
  bool shader_side_effects = false;
  uint32_t color_outputs = 0;
};

inline OverlayMode ClassifyOverlay(const OverlayInputs& input, const gpu::BlendDesc& blend) {
  if (!input.owned_target || !input.screen_vertex || !input.textures_available ||
      input.scene_texture || input.cube_or_volume || input.shader_side_effects || input.color_outputs != 1)
    return OverlayMode::kNone;
  using F = gpu::BlendFactor;
  if (!blend.enable) return OverlayMode::kUi;
  if (blend.op != gpu::BlendOp::kAdd) return OverlayMode::kNone;
  if ((blend.src == F::kDestColor && blend.dst == F::kZero) ||
      (blend.src == F::kZero && blend.dst == F::kSrcColor)) return OverlayMode::kModulation;
  if (blend.src == F::kDestColor && blend.dst == F::kSrcColor) return OverlayMode::kModulation2x;
  if (blend.src == F::kOne && blend.dst == F::kInvSrcAlpha) return OverlayMode::kPremultiplied;
  if ((blend.src == F::kSrcAlpha && (blend.dst == F::kInvSrcAlpha || blend.dst == F::kOne)) ||
      (blend.src == F::kOne && (blend.dst == F::kZero || blend.dst == F::kOne))) return OverlayMode::kUi;
  return OverlayMode::kNone;
}

inline gpu::GraphicsPipelineDesc OverlayPipeline(gpu::GraphicsPipelineDesc original, OverlayMode mode) {
  original.color_formats[0] = gpu::Format::kR16G16B16A16Float;
  // Replay before the original draw, with the original tests but no repeated
  // depth/stencil updates. The original draw remains the sole state writer.
  original.depth_write = false;
  original.stencil_write_mask = 0;
  original.stencil_front.fail = original.stencil_front.depth_fail = original.stencil_front.pass = gpu::StencilOp::kKeep;
  original.stencil_back.fail = original.stencil_back.depth_fail = original.stencil_back.pass = gpu::StencilOp::kKeep;
  if (mode == OverlayMode::kModulation2x) original.color_blend[0].dst = gpu::BlendFactor::kZero;
  return original;
}

}  // namespace xenos::renderer::native_hdr
