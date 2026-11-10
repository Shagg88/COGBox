// Layer 0 (game-free), fsr049: the Vulkan half of the AMD FSR upscaler (fsr_vulkan.h). AMD FSR SDK 2.4 brought the
// upscaler to Vulkan (amd_fidelityfx_loader_vk.dll + amd_fidelityfx_upscaler_vk.dll: FSR 4.1 / 3.1 as on D3D12). Our
// code (MIT).
#if defined(GN_HAVE_FSR)

#if __has_include(<vk/ffx_api_vk.h>) && __has_include(<vulkan/vulkan.h>)
#define GN_FSR_VK 1
#else
#define GN_FSR_VK 0
#endif

#include "fsr_vulkan.h"

#include <rex/cvar.h>

#if GN_FSR_VK
#include <vulkan/vulkan.h>
#include <vk/ffx_api_vk.h>

// The launcher looks for "AMD FSR SDK 2.4 Vulkan upscaler" in the exe (gears_settings.cpp fsr_vulkan): only in a build
// that has this path.
// Review: opt-in (experimental) - the launcher sends it only with "FSR on Vulkan (experimental)" on.
REXCVAR_DEFINE_BOOL(gow2_fsr_vulkan, false, "Gears2",
                    "AMD FSR upscaling on the Vulkan renderer (AMD FSR SDK 2.4 Vulkan upscaler, fsr049), experimental; "
                    "false = FSR off on Vulkan");
#endif

namespace xenos::renderer::upscale::fsrvk {

#if GN_FSR_VK
namespace {
struct Backend {
  ffxCreateBackendVKDesc desc{};
  VkDeviceContext context{};
};
}  // namespace

bool Built() { return true; }
bool Enabled() { return REXCVAR_GET(gow2_fsr_vulkan); }

void* NewBackend(gpu::Backend& backend) {
  void* device = nullptr;
  void* cb = nullptr;
  void* physical = nullptr;
  void* gdpa = nullptr;
  if (!backend.BeginNativeVulkan(&device, &cb)) return nullptr;
  const bool have = backend.NativeVulkanDevice(&physical, &gdpa);
  backend.EndNativeVulkan();  // review: only the device is needed - close it on the failure path too
  if (!have || !device || !physical || !gdpa) return nullptr;
  auto* b = new Backend();
  b->context.vkDevice = static_cast<VkDevice>(device);
  b->context.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical);
  b->context.vkDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gdpa);
  b->desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  b->desc.deviceContext = b->context;
  return b;
}
ffxApiHeader* BackendHeader(void* b) { return b ? &static_cast<Backend*>(b)->desc.header : nullptr; }
void* DeviceContext(void* b) { return b ? &static_cast<Backend*>(b)->context : nullptr; }
void DeleteBackend(void* b) { delete static_cast<Backend*>(b); }

bool Resource(gpu::Backend& backend, gpu::TextureHandle texture, bool output, FfxApiResource* out) {
  uint32_t format = 0, w = 0, h = 0, usage = 0, flags = 0;
  if (!out || !backend.NativeTextureVulkanInfo(texture, &format, &w, &h, &usage, &flags)) return false;
  const VkImageLayout layout = output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  void* image = backend.NativeTextureVulkan(texture, uint32_t(layout));
  if (!image) return false;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VkFormat(format);
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.usage = VkImageUsageFlags(usage);
  // Without the mutable-format flag: ffxApiGetImageResourceDescriptionVK would then take the colour for sRGB (ours is
  // linear).
  ci.flags = VkImageCreateFlags(flags) & ~VkImageCreateFlags(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
  *out = ffxApiGetResourceVK(image, ffxApiGetImageResourceDescriptionVK(reinterpret_cast<VkImage>(image), ci, 0),
                             output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ);
  return true;
}
#else
bool Built() { return false; }
bool Enabled() { return false; }
void* NewBackend(gpu::Backend&) { return nullptr; }
ffxApiHeader* BackendHeader(void*) { return nullptr; }
void* DeviceContext(void*) { return nullptr; }
void DeleteBackend(void*) {}
bool Resource(gpu::Backend&, gpu::TextureHandle, bool, FfxApiResource*) { return false; }
#endif

}  // namespace xenos::renderer::upscale::fsrvk

#endif  // GN_HAVE_FSR
