// Layer 0 (game-free): Xenos shader microcode -> DXBC (Shader Model 5.1) bytecode, directly, with no HLSL compiler
// in between (renderer overhaul option B, 2026-10-23). The DXC path (hlsl.cpp -> DXC) costs 120-230 ms per shader on
// the target PC, which is what freezes the game while shaders compile; Canary and Edge emit DXBC directly and take a
// few milliseconds. This translator keeps our semantics exactly - it is a line-by-line port of hlsl.cpp's generated
// code (same maths rules, same bindings, same metadata) - only the output format changes. Where hlsl.cpp's text is
// post-processed by the shader cache (precise positions, the D24FS8 depth remap), the DXBC is generated in its final
// form: DXBC with no "refactoring allowed" flag is precise everywhere.
//
// Bytecode is written with the SDK's DXBC assembler (rex/graphics/format/dxbc.h, Xenia - Copyright Ben Vanik, BSD,
// header-only); the container checksum is our own MD5-based implementation. This file and dxbc_gen.cpp are our own code
// (MIT).
//
// Binding contract: identical to hlsl.cpp's (notes/39): b0 system constants, b1 float constants, b2 bool + loop
// constants, b3 fetch constants; t0-31 Texture2D, t32-63 TextureCube, t64-95 Texture3D, s0-31 samplers; t96 guest
// memory and t97 the upload ring (raw buffers); u0 the guest memory for memory export. All in space 0. Vertex shader
// outputs TEXCOORD0-15 (o0-o15) and SV_Position (o16); pixel shader inputs TEXCOORD0-15 (v0-v15), SV_Position (v16)
// and SV_IsFrontFace (v17.x); pixel shader outputs SV_Target0-3 and SV_Depth when the shader writes depth.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <xenos/hlsl.h>
#include <xenos/ucode.h>

namespace xenos::dxbc_gen {

struct DxbcTranslation {
  // False when the shader uses something this translator does not handle; `unsupported` says what, and the caller
  // uses the HLSL path for it.
  bool ok = false;
  std::string unsupported;
  std::vector<uint8_t> bytecode;  // the DXBC container (checksummed), ready for a pipeline state
  // The same metadata hlsl::TranslateToHlsl gives for this program and these options (`hlsl` is left empty).
  hlsl::Translation meta;
  bool writes_depth = false;
  // The registers behind the SM 5.1 range IDs (index = ID), for tools and tests.
  std::vector<uint32_t> srv_registers, sampler_registers;
};

DxbcTranslation TranslateToDxbc(const ucode::Program& program, const hlsl::HlslOptions& options = {});

// The renderer's rectangle-list geometry shader (builtin_shaders.h kRectListGs, from Canary) as DXBC, for pipelines
// whose vertex and pixel shaders are DXBC (a pipeline state takes one bytecode kind).
std::vector<uint8_t> RectListGeometryShader();

// Patch 0087: how many shaders (final translations, all threads, since start) each newer Canary rule changed.
struct RuleHits {
  uint64_t tfetch1d_xy = 0, mulsc_round_toward_zero = 0, memexport_f16_nan_zero = 0;
};
RuleHits CanaryRuleHits();

}  // namespace xenos::dxbc_gen
