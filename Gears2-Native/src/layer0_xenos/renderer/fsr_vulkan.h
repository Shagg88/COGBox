// Layer 0 (game-free), fsr049: the Vulkan half of the AMD FSR upscaler (fsr_upscaler.cpp). Kept in its own file because
// the SDK's ffx_api_dx12.h and ffx_api_vk.h declare PfnFfxResourceAllocatorFunc differently and cannot share one
// translation unit. Our code (MIT).
#pragma once

#include <ffx_api.h>
#include <ffx_api_types.h>

#include "xenos/renderer/gpu.h"

namespace xenos::renderer::upscale::fsrvk {

bool Built();    // this exe has the AMD FSR SDK 2.4 Vulkan upscaler path (Vulkan + ffx_api_vk.h headers at build time)
bool Enabled();  // --gow2_fsr_vulkan
// An ffxCreateBackendVKDesc (with its VkDeviceContext) for the backend's device; nullptr when the backend gives none.
void* NewBackend(gpu::Backend& backend);
ffxApiHeader* BackendHeader(void* b);   // chain into a create
void* DeviceContext(void* b);           // VkDeviceContext* for ffxQueryDescGetVersions
void DeleteBackend(void* b);
// A backend texture as an FSR resource: moved to SHADER_READ_ONLY_OPTIMAL (input) or GENERAL (output) and described
// from how it was made.
bool Resource(gpu::Backend& backend, gpu::TextureHandle texture, bool output, FfxApiResource* out);

}  // namespace xenos::renderer::upscale::fsrvk
