// Layer 0 (game-free): DXBC (the bytecode our own translator writes, dxbc_gen.h) -> SPIR-V for the Vulkan backend.
// Notes/68 stage V5a: the Vulkan path compiled our HLSL through DXC to SPIR-V, 100-300 ms per shader, which is what
// made a first visit on Vulkan take a minute. D3D12 got fast by writing DXBC directly; rather than a second translator
// that repeats all the Xenos maths in SPIR-V, this converts that same DXBC instruction for instruction, so the maths has
// one source (dxbc_gen.cpp, played on D3D12) and Vulkan follows it exactly.
//
// It handles the subset dxbc_gen emits (vertex and pixel shaders, SM 5.1 range IDs, structured control flow, raw
// buffers, 2D / cube / 3D textures with separate samplers) and nothing else: anything outside it fails with a reason
// and the caller uses the HLSL path. The SPIR-V has the HLSL path's interface exactly (DXC -spirv with
// -fvk-*-shift 0/100/200/300 and hlsl.cpp's [[vk::binding]]s), so a converted shader and a DXC one can share a
// pipeline: b0-b3 set 0 bindings 0-3; t / s set 1 / set 2 at their register; t96 / t97 raw buffers set 1;
// u0 set 0 binding 300; TEXCOORD0-15 one float4[16] at location 0; SV_Target0-3 locations 0-3.
//
// Our code (MIT).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xenos::dxbc_spirv {

struct SpirvResult {
  bool ok = false;
  std::string error;            // when !ok: what was not handled
  std::vector<uint32_t> words;  // SPIR-V 1.6 module
  bool pixel = false;
};

// precise = false (patch 0088, a measurement): float arithmetic without NoContraction, so the driver may fuse (FMA).
SpirvResult ConvertDxbcToSpirv(const uint8_t* dxbc, size_t size, bool precise = true);

// Patch 0102 (a measurement switch, Vulkan): DXBC discard as OpTerminateInvocation (the lane stops, as DXC's default
// OpKill) instead of OpDemoteToHelperInvocation (the lane runs on as a helper to the end). Every pixel shader has a
// discard (the alpha test). Counts since start: pixel shaders converted / discards converted / of them terminated.
void SetDiscardTerminates(bool on);
void DiscardCounts(uint64_t& pixel_shaders, uint64_t& discards, uint64_t& terminated);

// Patch 0103 (a measurement switch, Vulkan): false = no ExecutionMode SignedZeroInfNanPreserve (float controls), so the
// driver may use its fast float paths as D3D12's DXBC compiler does. Default true (as before).
void SetFloatControls(bool on);

}  // namespace xenos::dxbc_spirv
