// Layer 0 (game-free): the Vulkan backend of our renderer (notes/68, Shagg 2026-10-23: "one renderer, D3D12 + Vulkan").
// Stage V1: device, textures (create / destroy / upload / clear), the guest-memory mirror, the upload ring, readbacks,
// submission with a timeline semaphore, debug labels and stats. Stage V2: shaders (SPIR-V from DXC, bindings remapped
// to the layout below), graphics pipelines (asynchronous workers like the D3D12 backend), descriptors, render targets
// (dynamic rendering), constants, textures and samplers, draws (plain, indexed, persistent index buffers). Chosen with
// --gow2_renderer_api=vulkan; D3D12 stays the default.
//
// Binding layout (graphics; gpu.h's contract, with the per-stage constant buffers split):
//   set 0: dynamic uniform buffers in the upload ring - binding 0-3 = the vertex shader's b0-b3, 4-7 = the pixel
//          shader's b0-b3 (each stage has its own copy, as the D3D12 root signature gives it).
//   set 1: binding 0-95 sampled images (t0-t31 2D, t32-t63 cube, t64-t95 3D), 96 the guest-memory buffer (t96),
//          97 the upload ring (t97, transient reads), 98 the guest-memory buffer writable (u0, memexport).
//   set 2: binding 0-31 samplers (s0-s31).
// The translator's HLSL carries [[vk::binding]] for set 1 / set 2; everything else (our fixed shaders) is compiled
// with -fvk-{b,t,s,u}-shift 0 / 100 / 200 / 300 and CreateShader moves those bindings into place (RemapBindings).
//
// Built without any Vulkan library to link: the loader (vulkan-1.dll / libvulkan.so.1) is opened at run time and every
// function comes through vkGetInstanceProcAddr / vkGetDeviceProcAddr (VK_NO_PROTOTYPES). Headers: the SDK tree's
// vulkan-headers (CMake sets GN_HAVE_VULKAN_HEADERS) or the system's; without either this file builds the null stub, so
// a project that compiles layer 0 by glob (Gears 3) still links.
//
// Vulkan version (Shagg 21:46: "the most up to date vulkan"): we ask for Vulkan 1.4, the newest core version, and use
// its features where the driver has them (push descriptors, maintenance5 / 6 - core in 1.4); the minimum is 1.3
// (dynamic rendering, synchronization2, timeline semaphores), so GPUs whose drivers stop at 1.3 still run.
// RTX 3080 Ti on current NVIDIA drivers, recent AMD / Intel drivers, the Steam Deck (Mesa) and lavapipe (the local
// tests) are 1.4.
// Our code (MIT).
#include <xenos/renderer/gpu.h>

#if defined(GN_HAVE_VULKAN_HEADERS) || __has_include(<vulkan/vulkan_core.h>)
#define GN_VULKAN_BACKEND 1
#endif

#ifndef GN_VULKAN_BACKEND

namespace xenos::renderer::gpu {
std::unique_ptr<Backend> CreateVulkanBackend() { return nullptr; }
}  // namespace xenos::renderer::gpu

#else

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <rex/logging/macros.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <format>
#include <string>
#include <vector>

#include "vk_present_spirv.h"

namespace xenos::renderer::gpu {
namespace {

// ---- the loader ----
#define GN_VK_INSTANCE_FUNCS(X)                                                                                        \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                                  \
  X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties)       \
  X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice)                     \
  X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define GN_VK_DEVICE_FUNCS(X)                                                                                          \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateCommandPool) X(vkDestroyCommandPool)            \
  X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)                      \
  X(vkQueueSubmit2) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkGetSemaphoreCounterValue) X(vkWaitSemaphores)       \
  X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateBuffer) X(vkDestroyBuffer)              \
  X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage)                           \
  X(vkGetImageMemoryRequirements) X(vkGetImageMemoryRequirements2) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)                      \
  X(vkCmdPipelineBarrier2) X(vkCmdCopyImage) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer)                     \
  X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdBeginRendering) X(vkCmdEndRendering)                   \
  X(vkCmdClearAttachments) X(vkCmdFillBuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule)                     \
  X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
  X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets)           \
  X(vkUpdateDescriptorSets) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreatePipelineCache)                \
  X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) X(vkCreateSampler) X(vkDestroySampler) X(vkCmdBindPipeline)       \
  X(vkCmdBindDescriptorSets) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport)              \
  X(vkCmdSetScissor) X(vkCmdSetStencilReference) X(vkCmdSetBlendConstants) X(vkCreateComputePipelines)             \
  X(vkCmdDispatch) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp2) X(vkCmdBeginQuery) X(vkCmdEndQuery)          \
  X(vkGetQueryPoolResults)
#define GN_VK_DEBUG_FUNCS(X) X(vkCmdBeginDebugUtilsLabelEXT) X(vkCmdEndDebugUtilsLabelEXT)

struct Vk {
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
  PFN_vkCreateInstance vkCreateInstance = nullptr;
  PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;
  PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
#define GN_VK_DECLARE(f) PFN_##f f = nullptr;
  GN_VK_INSTANCE_FUNCS(GN_VK_DECLARE)
  GN_VK_DEVICE_FUNCS(GN_VK_DECLARE)
  GN_VK_DEBUG_FUNCS(GN_VK_DECLARE)
#undef GN_VK_DECLARE
};

void* OpenLoader() {
#ifdef _WIN32
  return reinterpret_cast<void*>(LoadLibraryA("vulkan-1.dll"));
#else
  void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  return h ? h : dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
}
PFN_vkGetInstanceProcAddr LoaderEntry(void* lib) {
#ifdef _WIN32
  return reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(reinterpret_cast<HMODULE>(lib), "vkGetInstanceProcAddr"));
#else
  return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
}

std::string VkErr(const char* what, VkResult r) { return std::format("{} failed (VkResult {})", what, int(r)); }

// ---- formats ----
struct FormatInfo {
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkFormat snorm = VK_FORMAT_UNDEFINED;  // the signed twin (TextureView::signed_view)
  VkFormat bits = VK_FORMAT_UNDEFINED;   // the UINT bits view (TextureView::bits_view, SetRenderTargets bits_view)
  uint32_t block_bytes = 4;              // bytes per texel, or per 4x4 block for BCn
  bool block = false;
  bool depth = false;
};
FormatInfo Info(Format f) {
  switch (f) {
    case Format::kR8G8B8A8Unorm: return {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, 4};
    case Format::kR16G16B16A16Float: return {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_UNDEFINED, VK_FORMAT_R16G16B16A16_UINT, 8};
    case Format::kR16G16Snorm: return {VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, 4};
    case Format::kR16G16B16A16Snorm: return {VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, 8};
    case Format::kR32Uint: return {VK_FORMAT_R32_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_UINT, 4};
    case Format::kR32Float: return {VK_FORMAT_R32_SFLOAT, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_UINT, 4};
    case Format::kR32G32B32A32Float: return {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_UINT, 16};
    case Format::kR8Unorm: return {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, 1};
    case Format::kR8G8Unorm: return {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, 2};
    case Format::kR16Unorm: return {VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, 2};
    case Format::kR16G16Unorm: return {VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, 4};
    case Format::kR16G16B16A16Unorm: return {VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, 8};
    case Format::kR16Float: return {VK_FORMAT_R16_SFLOAT, VK_FORMAT_UNDEFINED, VK_FORMAT_R16_UINT, 2};
    case Format::kR16G16Float: return {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_UNDEFINED, VK_FORMAT_R16G16_UINT, 4};
    case Format::kR32G32Float: return {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_UINT, 8};
    // DXGI lists components from the low bits, Vulkan's PACK formats from the high bits: the same layouts.
    case Format::kR10G10B10A2Unorm: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_UNDEFINED, VK_FORMAT_A2B10G10R10_UINT_PACK32, 4};
    case Format::kB5G6R5Unorm: return {VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 2};
    case Format::kB5G5R5A1Unorm: return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 2};
    case Format::kB4G4R4A4Unorm: return {VK_FORMAT_A4R4G4B4_UNORM_PACK16, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 2};
    case Format::kBC1: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 8, true};
    case Format::kBC2: return {VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 16, true};
    case Format::kBC3: return {VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 16, true};
    case Format::kBC4: return {VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_UNDEFINED, 8, true};
    case Format::kBC5: return {VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_UNDEFINED, 16, true};
    case Format::kD32FloatS8X24: return {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 8, false, true};
    case Format::kD24UnormS8: return {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, 4, false, true};
    default: return {};
  }
}

struct Texture {
  bool live = false;
  TextureDesc desc;
  VkFormat format = VK_FORMAT_UNDEFINED;  // the image's format (may differ from Info(desc.format) on fallback)
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // the whole image's layout (V1: one layout per image)
  VkImageView attachment_view = VK_NULL_HANDLE;      // mip 0 layer 0 (render target / depth attachment)
  VkImageView bits_view = VK_NULL_HANDLE;            // the UINT attachment view (SetRenderTargets bits_view)
  std::vector<std::pair<uint64_t, VkImageView>> views;  // sampled views by key (ViewKey)
  // Patch 0087: a colour render target made without MUTABLE_FORMAT (so the driver can compress it) although its format
  // has other view formats; made mutable (Promote) the first time such a view is asked for.
  bool can_promote = false;
  uint32_t vk_usage = 0, vk_flags = 0, vk_view_formats = 1;  // patch 0101: as created (the draw report)
  // Patch 0089 (stage V5b): the barrier epoch in which this image was last sampled / written as an attachment.
  uint64_t read_epoch = 0, written_epoch = 0;
  uint64_t att_written_epoch = 0;  // patch 0116: att_epoch_ when last drawn to as an attachment
};

struct Shader {
  ShaderStage stage = ShaderStage::kVertex;
  VkShaderModule module = VK_NULL_HANDLE;
  uint64_t bytes = 0;  // patch 0079: SPIR-V size
  // Compute (stage V4): its own one-set layout, from the descriptor types found in the SPIR-V (ComputeBinding below).
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorType types[8] = {};  // by compute binding; VK_DESCRIPTOR_TYPE_MAX_ENUM = unused
};

struct Pipeline {
  VkPipeline pipeline = VK_NULL_HANDLE;
  ShaderHandle compute = 0;  // compute pipelines: their shader (layout and binding types)
  std::atomic<bool> ready{true};
  std::atomic<bool> failed{false};
};

struct PipelineJob {
  PipelineHandle handle = 0;
  GraphicsPipelineDesc desc;
  VkShaderModule modules[3] = {};
  uint64_t bytes = 0;  // patch 0079: SPIR-V size of its stages
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* mapped = nullptr;
  VkDeviceSize size = 0;
};

struct Readback {
  bool live = false;
  Buffer buffer;
  uint64_t fence = 0;  // the submission that copies into it
  uint32_t row_pitch = 0;
};

// Work the GPU must finish before a resource can be freed.
struct Deferred {
  uint64_t fence = 0;
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct CommandSlot {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  uint64_t fence = 0;  // 0 = free
};

class VulkanBackend final : public Backend {
 public:
  ~VulkanBackend() override { Shutdown(); }

  bool Init(const BackendOptions& options, std::string& error) override {
    options_ = options;
    lib_ = OpenLoader();
    if (!lib_) { error = "no Vulkan loader (vulkan-1.dll / libvulkan.so.1)"; return false; }
    vk_.vkGetInstanceProcAddr = LoaderEntry(lib_);
    if (!vk_.vkGetInstanceProcAddr) { error = "the Vulkan loader has no vkGetInstanceProcAddr"; return false; }
    vk_.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vk_.vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    vk_.vkEnumerateInstanceVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vk_.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    vk_.vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        vk_.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));
    uint32_t api = VK_API_VERSION_1_0;
    if (vk_.vkEnumerateInstanceVersion) vk_.vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_3) { error = std::format("the Vulkan loader is {}.{}, 1.3 is needed", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api)); return false; }
    ext_ = options.vulkan_external;
    if (ext_) {  // notes/68 V3: the SDK provider's instance and device (its presenter shows our image)
      if (!ext_->instance || !ext_->physical_device || !ext_->device || !ext_->queue) {
        error = "the external Vulkan device is incomplete";
        return false;
      }
      instance_ = static_cast<VkInstance>(ext_->instance);
    } else if (!CreateInstance(options, error)) {
      return false;
    }
#define GN_VK_LOAD_I(f) vk_.f = reinterpret_cast<PFN_##f>(vk_.vkGetInstanceProcAddr(instance_, #f));
    GN_VK_INSTANCE_FUNCS(GN_VK_LOAD_I)
#undef GN_VK_LOAD_I
    if (!PickDevice(error)) return false;
    if (!CreateDevice(error)) return false;
    if (!CreateBuffers(error)) return false;
    if (!CreateLayouts(error)) return false;
    ready_ = true;
    if (!CreateDefaults(error)) { ready_ = false; return false; }
    REXLOG_INFO("gow2_renderer: Vulkan backend: {} (driver {}, Vulkan {}.{}.{}, built with headers 1.4.{}), queue family "
                "{}, {} MB device memory, push descriptors {}{}",
                device_name_, driver_name_, VK_API_VERSION_MAJOR(device_api_), VK_API_VERSION_MINOR(device_api_),
                VK_API_VERSION_PATCH(device_api_), VK_HEADER_VERSION, queue_family_, device_local_mb_,
                push_descriptor_ ? "yes" : "no", ext_ ? " - on the SDK presenter's device (notes/68 V3)" : "");
    return true;
  }
  bool CreateInstance(const BackendOptions& options, std::string& error) {
    // Debug labels when the loader offers them (RenderDoc, validation layers).
    std::vector<const char*> exts;
    {
      uint32_t n = 0;
      vk_.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
      std::vector<VkExtensionProperties> e(n);
      vk_.vkEnumerateInstanceExtensionProperties(nullptr, &n, e.data());
      for (const auto& x : e)
        if (!std::strcmp(x.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) { exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); debug_utils_ = true; }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Gears-Native";
    app.pEngineName = "gn_renderer";
    app.apiVersion = VK_API_VERSION_1_4;  // the highest version we use (the loader / device may report less)
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (options.debug_layer) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &layer; }
    VkResult r = vk_.vkCreateInstance(&ici, nullptr, &instance_);
    if (r != VK_SUCCESS && options.debug_layer) {  // no validation layer installed: carry on without
      ici.enabledLayerCount = 0;
      r = vk_.vkCreateInstance(&ici, nullptr, &instance_);
    }
    if (r != VK_SUCCESS) { error = VkErr("vkCreateInstance", r); return false; }
    return true;
  }

  std::string Describe() const override {
    return std::format("Vulkan {}.{} on {} ({})", VK_API_VERSION_MAJOR(device_api_), VK_API_VERSION_MINOR(device_api_),
                       device_name_, driver_name_);
  }
  ShaderIL GetShaderIL() const override { return ShaderIL::kSpirv; }
  bool IsLost(std::string* reason) override {
    if (lost_ && reason) *reason = lost_reason_;
    return lost_;
  }

  // ---- resources ----
  TextureHandle CreateTexture(const TextureDesc& d) override {
    if (!ready_ || lost_) return 0;
    FormatInfo fi = Info(d.format);
    if (fi.format == VK_FORMAT_UNDEFINED) { NotDone("CreateTexture: format"); return 0; }
    VkFormat fmt = fi.format;
    if (fi.depth && fmt == VK_FORMAT_D24_UNORM_S8_UINT && !d24s8_) fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;  // AMD: no D24S8
    Texture t;
    t.desc = d;
    t.format = fmt;
    t.aspect = fi.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = d.dimension == TextureDimension::k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {std::max(1u, d.width), std::max(1u, d.height), d.dimension == TextureDimension::k3D ? std::max(1u, d.depth_or_layers) : 1u};
    ii.mipLevels = std::max(1u, d.mips);
    ii.arrayLayers = d.dimension == TextureDimension::k3D ? 1u : std::max(1u, d.depth_or_layers);
    ii.samples = VkSampleCountFlagBits(std::max(1u, d.samples));
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (d.usage & kUsageRenderTarget) ii.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (d.usage & kUsageDepthStencil) ii.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (d.usage & kUsageStorage) ii.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (d.dimension == TextureDimension::kCube) ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // Signed and bits views of the same memory (D3D12: a typeless resource). Patch 0086: only images that have such a
    // view are mutable, and with the exact list of their view formats (VkImageFormatListCreateInfo) - job 388: every
    // colour image was mutable with no list, which on NVIDIA turns off colour compression; ch15's colour targets drew
    // in ~2x the GPU time of D3D12's while the depth-only target (never mutable) matched.
    VkFormat view_formats[3] = {fmt};
    uint32_t view_format_count = 1;
    if (!fi.depth && fi.snorm != VK_FORMAT_UNDEFINED && fi.snorm != fmt) view_formats[view_format_count++] = fi.snorm;
    if (!fi.depth && !fi.block && fi.bits != VK_FORMAT_UNDEFINED && fi.bits != fmt) view_formats[view_format_count++] = fi.bits;
    VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    // Patch 0087 (--gow2_renderer_vulkan_compressible_targets): colour render targets start with one format; Promote
    // makes one mutable the first time it really gets a view of another format (counted and logged).
    t.can_promote = options_.vk_mutable_targets != 0 && (d.usage & kUsageRenderTarget) && view_format_count > 1;
    if (view_format_count > 1 && !t.can_promote) {
      ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      fl.viewFormatCount = view_format_count;
      fl.pViewFormats = view_formats;
      ii.pNext = &fl;
    }
    VkResult r = vk_.vkCreateImage(device_, &ii, nullptr, &t.image);
    if (r != VK_SUCCESS) { Fail(VkErr("vkCreateImage", r)); return 0; }
    t.vk_usage = ii.usage;  // patch 0101
    t.vk_flags = ii.flags;
    t.vk_view_formats = ii.pNext ? view_format_count : 1;
    t.memory = AllocateImage(t.image);
    if (!t.memory) { vk_.vkDestroyImage(device_, t.image, nullptr); Fail("texture memory"); return 0; }
    // Outside review: check the bind and the attachment view, so a texture is never marked live without them.
    if ((r = vk_.vkBindImageMemory(device_, t.image, t.memory, 0)) != VK_SUCCESS) {
      vk_.vkDestroyImage(device_, t.image, nullptr);
      vk_.vkFreeMemory(device_, t.memory, nullptr);
      Fail(VkErr("vkBindImageMemory", r));
      return 0;
    }
    if (d.usage & (kUsageRenderTarget | kUsageDepthStencil)) {
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = t.image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = fmt;
      vi.subresourceRange = {t.aspect, 0, 1, 0, 1};
      if ((r = vk_.vkCreateImageView(device_, &vi, nullptr, &t.attachment_view)) != VK_SUCCESS) {
        vk_.vkDestroyImage(device_, t.image, nullptr);
        vk_.vkFreeMemory(device_, t.memory, nullptr);
        Fail(VkErr("vkCreateImageView", r));
        return 0;
      }
    }
    t.live = true;
    TextureHandle h = 0;
    for (TextureHandle i = 1; i < textures_.size(); ++i)
      if (!textures_[i].live) { h = i; break; }
    if (!h) { h = TextureHandle(textures_.size()); textures_.push_back({}); }
    textures_[h] = t;
    // Depth starts at its clear value (TextureDesc::clear_depth), colour at 0, both defined (no garbage).
    if (Record()) {
      EndPass(9);
      if (fi.depth) {
        Transition(textures_[h], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue v{d.clear_depth, 0};
        VkImageSubresourceRange rg{t.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vk_.vkCmdClearDepthStencilImage(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg);
      } else if (!fi.block) {
        Transition(textures_[h], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue v{};
        VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vk_.vkCmdClearColorImage(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg);
      }
    }
    return h;
  }
  void DestroyTexture(TextureHandle h) override {
    if (!Valid(h)) return;
    bool bound = targets_.depth == h;
    for (uint32_t i = 0; i < targets_.count; ++i) bound = bound || targets_.colors[i] == h;
    if (bound) { EndPass(); targets_ = {}; }
    Texture& t = textures_[h];
    Defer(t.image, t.attachment_view, VK_NULL_HANDLE, t.memory);
    if (t.bits_view) Defer(VK_NULL_HANDLE, t.bits_view, VK_NULL_HANDLE, VK_NULL_HANDLE);
    for (auto& [k, v] : t.views) Defer(VK_NULL_HANDLE, v, VK_NULL_HANDLE, VK_NULL_HANDLE);
    t = {};
    view_sets_.clear();  // sets that named this texture's views
  }
  TextureHandle ImportTexture(void*, Format, uint32_t) override { NotDone("ImportTexture (V3: the presenter's image)"); return 0; }
  void ReleaseImport(TextureHandle, uint32_t) override {}

  // Notes/68 V3: our packed present words into the SDK presenter's guest-output image (colour attachment + sampled
  // only, so a draw rather than the D3D12 path's compute unpack). One triangle, the pixel shader loads the word at its
  // pixel; the image ends in SHADER_READ_ONLY_OPTIMAL for the presenter's fragment-shader reads.
  bool PresentPacked(TextureHandle src, uint32_t width, uint32_t height, const void* target) override {
    const auto* t = static_cast<const VulkanPresentTarget*>(target);
    if (!ready_ || lost_ || !Valid(src) || !t || !t->image || !t->view || !width || !height) return false;
    if (!EnsurePresentPipeline() || !Record()) return false;
    EndPass(7);
    Texture& s = textures_[src];
    if (s.layout != VK_IMAGE_LAYOUT_GENERAL) Transition(s, VK_IMAGE_LAYOUT_GENERAL);
    if (present_view_image_ != s.image) {
      if (present_view_) Defer(VK_NULL_HANDLE, present_view_, VK_NULL_HANDLE, VK_NULL_HANDLE);
      present_view_ = VK_NULL_HANDLE;
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = s.image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = VK_FORMAT_R32_UINT;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (vk_.vkCreateImageView(device_, &vi, nullptr, &present_view_) != VK_SUCCESS) { present_view_ = VK_NULL_HANDLE; return false; }
      present_view_image_ = s.image;
    }
    // A descriptor set the GPU is done with (or a new one; at most 16, then wait for the oldest).
    const uint64_t done = CompletedFence();
    VkDescriptorSet set = VK_NULL_HANDLE;
    size_t slot = present_sets_.size();
    for (size_t i = 0; i < present_sets_.size(); ++i)
      if (present_sets_[i].second <= done) { slot = i; break; }
    if (slot == present_sets_.size()) {
      VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      ai.descriptorPool = present_pool_;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &present_set_layout_;
      if (present_sets_.size() < 16 && vk_.vkAllocateDescriptorSets(device_, &ai, &set) == VK_SUCCESS) {
        present_sets_.push_back({set, 0});
      } else {
        slot = 0;
        for (size_t i = 1; i < present_sets_.size(); ++i)
          if (present_sets_[i].second < present_sets_[slot].second) slot = i;
        if (present_sets_.empty() || present_sets_[slot].second > submitted_) return false;  // all in this recording
        WaitForFence(present_sets_[slot].second);
      }
    }
    if (slot < present_sets_.size()) set = present_sets_[slot].first;
    else slot = present_sets_.size() - 1;
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, present_view_, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vk_.vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    present_sets_[slot].second = submitted_ + 1;  // this recording's fence
    // Our source: earlier writes (gamma / SMAA / HDR dispatches) before the fragment reads. The target: from the
    // presenter's state (or nothing, the first time) to a colour attachment.
    VkImageMemoryBarrier2 b[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    b[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b[0].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b[0].oldLayout = b[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[0].image = s.image;
    b[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b[1] = b[0];
    b[1].srcStageMask = t->first_write ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b[1].srcAccessMask = VK_ACCESS_2_NONE;  // a read before: an execution dependency is enough
    b[1].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b[1].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b[1].oldLayout = t->first_write ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[1].image = static_cast<VkImage>(t->image);
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 2;
    di.pImageMemoryBarriers = b;
    vk_.vkCmdPipelineBarrier2(C(), &di);
    const uint32_t w2 = std::min(width, t->width ? t->width : width), h2 = std::min(height, t->height ? t->height : height);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = static_cast<VkImageView>(t->view);
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w2, h2}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vk_.vkCmdBeginRendering(C(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT), &ri);
    vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
    const VkViewport vp{0.0f, 0.0f, float(w2), float(h2), 0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {w2, h2}};
    vk_.vkCmdSetViewport(cmd_, 0, 1, &vp);
    vk_.vkCmdSetScissor(cmd_, 0, 1, &sc);
    vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout_, 0, 1, &set, 0, nullptr);
    vk_.vkCmdDraw(C(), 3, 1, 0, 0);
    vk_.vkCmdEndRendering(cmd_);
    VkImageMemoryBarrier2 a = b[1];
    a.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    a.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    a.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    a.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    a.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &a;
    vk_.vkCmdPipelineBarrier2(C(), &di);
    // The bound render state is the backend's no longer (the next draw rebinds what it needs).
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_index_ = VK_NULL_HANDLE;
    dirty_sets_ = dirty_cb_ = dirty_viewport_ = dirty_scissor_ = dirty_stencil_ = dirty_blend_ = true;
    return true;
  }
  bool EnsurePresentPipeline() {
    if (present_pipeline_) return true;
    if (present_failed_) return false;
    present_failed_ = true;  // until it works
    VkDescriptorSetLayoutBinding lb{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &lb;
    if (vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &present_set_layout_) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &present_set_layout_;
    if (vk_.vkCreatePipelineLayout(device_, &pli, nullptr, &present_layout_) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vk_.vkCreateDescriptorPool(device_, &pi, nullptr, &present_pool_) != VK_SUCCESS) return false;
    VkShaderModule mods[2] = {};
    const std::pair<const uint32_t*, size_t> code[2] = {{kVkPresentVs, sizeof(kVkPresentVs)}, {kVkPresentPs, sizeof(kVkPresentPs)}};
    for (int i = 0; i < 2; ++i) {
      VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      mi.codeSize = code[i].second;
      mi.pCode = code[i].first;
      if (vk_.vkCreateShaderModule(device_, &mi, nullptr, &mods[i]) != VK_SUCCESS) {
        if (mods[0]) vk_.vkDestroyShaderModule(device_, mods[0], nullptr);
        return false;
      }
    }
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = mods[0];
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = mods[1];
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    const VkFormat fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // the SDK VulkanPresenter's kGuestOutputFormat
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &rci;
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vin;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vps;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &ds;
    gi.layout = present_layout_;
    const VkResult r = vk_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gi, nullptr, &present_pipeline_);
    vk_.vkDestroyShaderModule(device_, mods[0], nullptr);
    vk_.vkDestroyShaderModule(device_, mods[1], nullptr);
    if (r != VK_SUCCESS) {
      present_pipeline_ = VK_NULL_HANDLE;
      REXLOG_ERROR("gow2_renderer: Vulkan: the present pipeline failed ({})", VkErr("vkCreateGraphicsPipelines", r));
      return false;
    }
    present_failed_ = false;
    return true;
  }
  ShaderHandle CreateShader(ShaderStage stage, const void* code, size_t size) override {
    if (!ready_ || lost_ || !code || size < 20 || (size & 3)) return 0;
    std::vector<uint32_t> words(size / 4);
    std::memcpy(words.data(), code, size);
    if (words[0] != 0x07230203u) { REXLOG_ERROR("gow2_renderer: Vulkan: CreateShader got no SPIR-V"); return 0; }
    RemapBindings(words, stage);
    Shader csh;
    if (stage == ShaderStage::kCompute && !ComputeLayout(words, csh)) return 0;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = words.size() * 4;
    mi.pCode = words.data();
    Shader sh = csh;
    sh.stage = stage;
    sh.bytes = size;
    const VkResult r = vk_.vkCreateShaderModule(device_, &mi, nullptr, &sh.module);
    if (r != VK_SUCCESS) { REXLOG_ERROR("gow2_renderer: Vulkan: {}", VkErr("vkCreateShaderModule", r)); return 0; }
    shaders_.push_back(sh);
    return ShaderHandle(shaders_.size() - 1);
  }
  PipelineHandle GetGraphicsPipeline(const GraphicsPipelineDesc& d, bool async, bool* pending) override {
    if (pending) *pending = false;
    if (!ready_ || lost_) return 0;
    auto it = pipeline_cache_.find(HashPipelineDesc(d));
    if (it != pipeline_cache_.end()) {
      for (const auto& [desc, handle] : it->second) {
        if (!(desc == d)) continue;
        if (!handle) return 0;
        Pipeline& p = *pipelines_[handle];
        if (p.ready.load(std::memory_order_acquire)) return p.failed.load() ? 0 : handle;
        // Patch 0076 (as D3D12's 0062): a draw that needs a job still queued - behind the start-up backlog or the
        // guessed pipelines - doesn't wait its turn: a waiting draw builds it now, an async one moves it to the front.
        if (!pipeline_backlog_) {
          PipelineJob queued;
          if (TakeQueued(handle, queued)) {
            if (!async) {
              ++stats_.pipelines_promoted;
              Build(queued);
              return pipelines_[handle]->failed.load() ? 0 : handle;
            }
            {
              std::lock_guard<std::mutex> lock(job_mutex_);
              jobs_.push_front(queued);
            }
            ++stats_.pipelines_promoted;
            job_cv_.notify_one();
          }
        }
        if (async) {
          if (pending) { ++stats_.pipelines_pending_draws; *pending = true; }
          return 0;
        }
        while (!p.ready.load(std::memory_order_acquire)) std::this_thread::yield();
        return p.failed.load() ? 0 : handle;
      }
    }
    ++pipeline_misses_;
    PipelineJob job;
    job.desc = d;
    const ShaderHandle hs[3] = {d.vs, d.ps, d.gs};
    for (int i = 0; i < 3; ++i) {
      job.modules[i] = hs[i] && hs[i] < shaders_.size() ? shaders_[hs[i]].module : VK_NULL_HANDLE;
      if (job.modules[i]) job.bytes += shaders_[hs[i]].bytes;
    }
    if (!job.modules[0] || (d.ps && !job.modules[1]) || (d.gs && !job.modules[2])) {
      pipeline_cache_[HashPipelineDesc(d)].push_back({d, 0});
      return 0;
    }
    pipelines_.push_back(std::make_unique<Pipeline>());
    const PipelineHandle handle = PipelineHandle(pipelines_.size() - 1);
    job.handle = handle;
    pipeline_cache_[HashPipelineDesc(d)].push_back({d, handle});
    ++stats_.pipelines;
    if (async && StartWorkers()) {
      pipelines_[handle]->ready.store(false);
      {
        std::lock_guard<std::mutex> lock(job_mutex_);
        (pipeline_backlog_ ? backlog_jobs_ : jobs_).push_back(job);  // patch 0076: live requests first
      }
      job_cv_.notify_one();
      ++stats_.pipelines_async;
      if (pending) { ++stats_.pipelines_pending_draws; *pending = true; }
      return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    Build(job);
    stats_.pipeline_create_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return pipelines_[handle]->failed.load() ? 0 : handle;
  }
  uint64_t PipelineMisses() const override { return pipeline_misses_; }
  uint64_t PipelineJobsLeft() override {
    std::lock_guard<std::mutex> lock(job_mutex_);
    return jobs_.size() + backlog_jobs_.size() + jobs_busy_;
  }
  void SetPipelineBacklog(bool on) override { pipeline_backlog_ = on; }  // patch 0076
  // A queued (not started) job for this pipeline, removed from its queue.
  bool TakeQueued(PipelineHandle handle, PipelineJob& out) {
    std::lock_guard<std::mutex> lock(job_mutex_);
    for (auto* q : {&jobs_, &backlog_jobs_})
      for (auto it = q->begin(); it != q->end(); ++it)
        if (it->handle == handle) {
          out = *it;
          q->erase(it);
          return true;
        }
    return false;
  }
  bool PipelinePending(const GraphicsPipelineDesc& d) override {
    auto it = pipeline_cache_.find(HashPipelineDesc(d));
    if (it == pipeline_cache_.end()) return false;
    for (const auto& [desc, handle] : it->second)
      if (desc == d) return handle && !pipelines_[handle]->ready.load(std::memory_order_acquire);
    return false;
  }
  PipelineHandle CreateComputePipeline(ShaderHandle cs) override {
    if (!ready_ || lost_ || !cs || cs >= shaders_.size() || shaders_[cs].stage != ShaderStage::kCompute) return 0;
    const Shader& sh = shaders_[cs];
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sh.module, "main", nullptr};
    ci.layout = sh.layout;
    auto p = std::make_unique<Pipeline>();
    p->compute = cs;
    const VkResult r = vk_.vkCreateComputePipelines(device_, pipeline_cache_vk_, 1, &ci, nullptr, &p->pipeline);
    if (r != VK_SUCCESS) { REXLOG_ERROR("gow2_renderer: Vulkan: {}", VkErr("vkCreateComputePipelines", r)); return 0; }
    pipelines_.push_back(std::move(p));
    ++stats_.pipelines;
    return PipelineHandle(pipelines_.size() - 1);
  }

  // ---- guest memory mirror ----
  // Patch 0078: data a draw reads straight from the upload ring (t97, bound whole at offset 0), no copy into the
  // guest-memory buffer - the renderer's small per-draw vertex ranges (job 160). Without it every such range became a
  // guest copy, and on Vulkan a copy ends the rendering pass and costs two full barriers: ~4,800 barrier calls a frame
  // in job 378 (D3D12 ~380), draws 60 ms of GPU time against D3D12's 15.
  uint32_t UploadTransient(const void* data, uint32_t size) override {
    if (!ready_ || lost_ || !size || !Record()) return UINT32_MAX;
    uint64_t at = 0;
    if (!RingAlloc(size, 16, at) || at > 0xFFFFFFF0ull) return UINT32_MAX;
    std::memcpy(ring_.mapped + at, data, size);
    stats_.transient_upload_bytes += size;
    return uint32_t(at);
  }
  void UploadGuestMemory(uint32_t guest_physical, const void* data, uint32_t size) override {
    if (!ready_ || lost_ || !size) return;
    const uint64_t end = uint64_t(guest_physical) + size;
    if (end > guest_.size) return;
    if (!Record()) return;
    uint64_t at = 0;
    if (!RingAlloc(size, 16, at)) return;
    std::memcpy(ring_.mapped + at, data, size);
    EndPass(2);
    GuestBarrier(false);
    VkBufferCopy c{at, guest_physical, size};
    vk_.vkCmdCopyBuffer(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), ring_.buffer, guest_.buffer, 1, &c);
    stats_.upload_bytes += size;
    stats_.guest_upload_bytes += size;
    guest_written_ = true;
  }

  // ---- graphics recording (V2) ----
  void SetRenderTargets(uint32_t color_count, const TextureHandle* colors, TextureHandle depth, bool bits_view) override {
    TargetSet t;
    t.count = std::min<uint32_t>(color_count, GraphicsPipelineDesc::kMaxColor);
    for (uint32_t i = 0; i < t.count; ++i) t.colors[i] = colors && Valid(colors[i]) ? colors[i] : 0;
    t.depth = Valid(depth) ? depth : 0;
    t.bits = bits_view;
    if (t == targets_) return;
    EndPass(1);
    targets_ = t;
  }
  void ClearDepthStencil(TextureHandle h, bool clear_depth, float depth_value, bool clear_stencil, uint8_t stencil_value,
                         const Rect* rects, uint32_t rect_count) override {
    if (!Valid(h) || !Record() || !(clear_depth || clear_stencil)) return;
    Texture& t = textures_[h];
    if (!(t.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    VkImageAspectFlags aspect = (clear_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (clear_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    VkClearDepthStencilValue v{depth_value, stencil_value};
    if (options_.vk_clear_in_pass && targets_.depth == h) {  // patch 0092: the bound depth target clears inside the pass
      VkClearAttachment ca{aspect, 0, {}};
      ca.clearValue.depthStencil = v;
      if (ClearInPass(t, ca, rects, rect_count)) return;
    }
    EndPass(5);
    if (!rects || !rect_count) {
      Transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkImageSubresourceRange rg{aspect, 0, 1, 0, 1};
      vk_.vkCmdClearDepthStencilImage(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg);
      return;
    }
    // Rectangles: clear inside a render pass with only that attachment.
    Transition(t, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = t.attachment_view;
    a.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {t.desc.width, t.desc.height}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &a;
    ri.pStencilAttachment = &a;
    vk_.vkCmdBeginRendering(C(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT), &ri);
    VkClearAttachment ca{aspect, 0, {}};
    ca.clearValue.depthStencil = v;
    ClearRects(ca, rects, rect_count, t.desc.width, t.desc.height);
    vk_.vkCmdEndRendering(cmd_);
  }
  // D3D's viewport is y-down with a y-up NDC; a negative-height Vulkan viewport gives the same mapping and winding.
  // Patch 0081: dynamic state is recorded only when it changed (as D3D12's state_calls_skipped; every draw set all
  // four before).
  void SetViewport(const Viewport& v) override {
    const VkViewport n{v.x, v.y + v.height, v.width, -v.height, v.min_depth, v.max_depth};
    if (std::memcmp(&n, &viewport_, sizeof(n)) == 0) { ++stats_.state_calls_skipped; return; }
    viewport_ = n;
    dirty_viewport_ = true;
  }
  void SetScissor(const Rect& r) override {
    const int32_t x0 = std::max(r.x0, 0), y0 = std::max(r.y0, 0);
    const VkRect2D n{{x0, y0}, {uint32_t(std::max(r.x1 - x0, 0)), uint32_t(std::max(r.y1 - y0, 0))}};
    if (std::memcmp(&n, &scissor_, sizeof(n)) == 0) { ++stats_.state_calls_skipped; return; }
    scissor_ = n;
    dirty_scissor_ = true;
  }
  void SetGuestWritable(bool writable) override { guest_writable_ = writable; }  // patch 0089 (V5b bookkeeping)
  void SetStencilRef(uint8_t ref) override {
    if (ref == stencil_ref_) { ++stats_.state_calls_skipped; return; }
    stencil_ref_ = ref;
    dirty_stencil_ = true;
  }
  void SetBlendConstant(const float* rgba) override {
    if (std::memcmp(rgba, blend_, sizeof(blend_)) == 0) { ++stats_.state_calls_skipped; return; }
    std::memcpy(blend_, rgba, sizeof(blend_));
    dirty_blend_ = true;
  }
  void ClearColor(TextureHandle h, const float rgba[4], const Rect* rects, uint32_t rect_count) override {
    if (!Valid(h) || !Record()) return;
    Texture& t = textures_[h];
    if (t.aspect != VK_IMAGE_ASPECT_COLOR_BIT) return;
    VkClearColorValue v{};
    std::memcpy(v.float32, rgba, sizeof(v.float32));
    if (options_.vk_clear_in_pass && !targets_.bits)  // patch 0092: a bound colour target clears inside the pass
      for (uint32_t i = 0; i < targets_.count; ++i)
        if (targets_.colors[i] == h) {
          VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, i, {}};
          ca.clearValue.color = v;
          if (ClearInPass(t, ca, rects, rect_count)) return;
          break;
        }
    EndPass(5);
    if (!rects || !rect_count || !t.attachment_view) {
      Transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkImageSubresourceRange rg{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vk_.vkCmdClearColorImage(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg);
      return;
    }
    Transition(t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = t.attachment_view;
    a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {t.desc.width, t.desc.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &a;
    vk_.vkCmdBeginRendering(C(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT), &ri);
    VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    ca.clearValue.color = v;
    ClearRects(ca, rects, rect_count, t.desc.width, t.desc.height);
    vk_.vkCmdEndRendering(cmd_);
  }
  // Patch 0092 (--gow2_renderer_vulkan_clear_in_pass): clear an attachment of the current targets with
  // vkCmdClearAttachments inside the render pass (beginning it if needed) instead of ending the pass, changing layouts
  // twice and starting a new pass. Only when the clear stays inside the pass's render area (a whole-image clear needs
  // the image to be the area's size). false = not done here (the caller clears the old way).
  bool ClearInPass(const Texture& t, const VkClearAttachment& ca, const Rect* rects, uint32_t rect_count) {
    if (!in_pass_ && !BeginPass()) return false;
    if (!rects || !rect_count) {
      if (t.desc.width != pass_w_ || t.desc.height != pass_h_) return false;
      const Rect all{0, 0, int32_t(pass_w_), int32_t(pass_h_)};
      ClearRects(ca, &all, 1, pass_w_, pass_h_);
    } else {
      ClearRects(ca, rects, rect_count, pass_w_, pass_h_);
    }
    ++stats_.vk_clears_in_pass;
    return true;
  }
  void SetPipeline(PipelineHandle p) override { current_pipeline_ = p < pipelines_.size() ? p : 0; }
  // Patch 0080 (as D3D12's SetConstants): data the slot already holds in this command buffer is not uploaded again,
  // and the descriptor sets are rebound only when an offset changed. Job 380: Vulkan "constants" 2.8 ms a frame against
  // D3D12's 1.1, with every draw uploading all slots and rebinding the sets. The shadows are dropped when a new
  // command buffer begins (ring space of a finished submission is reused).
  void SetConstants(ShaderStage stage, uint32_t slot, const void* data, uint32_t size) override {
    SetConstantsSplit(stage, slot, data, size, nullptr, 0);
  }
  void SetConstantsSplit(ShaderStage stage, uint32_t slot, const void* a, uint32_t a_size, const void* b2,
                         uint32_t b_size) override {
    if (!ready_ || lost_ || slot >= kCbCount || (stage != ShaderStage::kVertex && stage != ShaderStage::kPixel)) return;
    const uint32_t b = (stage == ShaderStage::kPixel ? kCbCount : 0) + slot;
    if (!a) a_size = 0;
    if (!b2) b_size = 0;
    a_size = std::min(a_size, kCbRange);
    b_size = std::min(b_size, kCbRange - a_size);
    const uint32_t size = a_size + b_size;
    CbShadow& sh = cb_shadow_[b];
    if (!size) {
      sh.valid = false;
      SetCbOffset(b, 0);
      return;
    }
    if (!Record()) return;
    if (sh.valid && sh.size == size && (!a_size || std::memcmp(sh.data, a, a_size) == 0) &&
        (!b_size || std::memcmp(sh.data + a_size, b2, b_size) == 0)) {
      ++stats_.constant_uploads_skipped;
      return;
    }
    uint64_t at = 0;
    if (!RingAllocUniform(size, at)) return;
    if (a_size) { std::memcpy(ring_.mapped + at, a, a_size); std::memcpy(sh.data, a, a_size); }
    if (b_size) { std::memcpy(ring_.mapped + at + a_size, b2, b_size); std::memcpy(sh.data + a_size, b2, b_size); }
    sh.size = size;
    sh.valid = true;
    SetCbOffset(b, uint32_t(at));
    stats_.upload_bytes += size;
  }
  void SetConstantsBoth(uint32_t slot, const void* data, uint32_t size) override {
    if (slot >= kCbCount) return;
    SetConstants(ShaderStage::kVertex, slot, data, size);
    cb_shadow_[kCbCount + slot].valid = false;  // the pixel slot shares the vertex copy; its own data must upload
    SetCbOffset(kCbCount + slot, cb_offset_[slot]);
  }
  void SetCbOffset(uint32_t b, uint32_t at) {
    if (cb_offset_[b] != at) { cb_offset_[b] = at; dirty_cb_ = true; }
  }
  void SetTextures(const TextureView* views, const SamplerDesc* samplers, uint32_t view_count, uint32_t sampler_count) override {
    // Patch 0080: only a real change marks the sets for a new lookup (job 380: every draw hashed 96 views and looked
    // up 32 samplers); the slots that hold a texture are listed for PrepareDraw's layout check.
    view_count = std::min(view_count, 96u);
    sampler_count = std::min(sampler_count, 32u);
    bool vchg = false, schg = false;
    for (uint32_t i = 0; i < 96; ++i) {
      const TextureView v = views && i < view_count ? views[i] : TextureView{};
      if (!(views_[i] == v)) { views_[i] = v; vchg = true; }
    }
    for (uint32_t i = 0; i < 32; ++i) {
      const SamplerDesc d = samplers && i < sampler_count ? samplers[i] : SamplerDesc{};
      if (!(samplers_[i] == d)) { samplers_[i] = d; schg = true; }
    }
    if (vchg) {
      dirty_views_ = true;
      bound_view_count_ = 0;
      for (uint32_t i = 0; i < kViewBindings; ++i)
        if (views_[i].texture) bound_views_[bound_view_count_++] = uint8_t(i);
    }
    if (schg) dirty_samplers_ = true;
    if (!vchg && !schg) ++stats_.texture_tables_reused;
  }
  void UploadTexture(TextureHandle h, uint32_t mip, uint32_t layer, const void* data, uint32_t row_bytes, uint32_t rows,
                     uint32_t slices) override {
    if (!Valid(h) || !Record() || !data) return;
    Texture& t = textures_[h];
    const FormatInfo fi = Info(t.desc.format);
    if (mip >= std::max(1u, t.desc.mips)) { ++stats_.texture_uploads_refused; return; }
    const bool is3d = t.desc.dimension == TextureDimension::k3D;
    if (!is3d && layer >= std::max(1u, t.desc.depth_or_layers)) { ++stats_.texture_uploads_refused; return; }
    const uint32_t w = std::max(1u, t.desc.width >> mip), hgt = std::max(1u, t.desc.height >> mip);
    const uint32_t d = is3d ? std::max(1u, std::max(1u, t.desc.depth_or_layers) >> mip) : 1u;
    slices = std::max(1u, std::min(slices, d));
    // The texel rows the copy needs, tightly packed by Vulkan's rule (bufferRowLength in texels, 0 = tight).
    const uint32_t tw = fi.block ? (w + 3) / 4 : w;
    const uint32_t th = fi.block ? (hgt + 3) / 4 : hgt;
    const uint32_t tight = tw * fi.block_bytes;
    rows = std::min(rows, th);
    if (row_bytes < tight || !rows) { ++stats_.texture_uploads_refused; return; }
    const uint64_t size = uint64_t(tight) * rows * slices;
    uint64_t at = 0;
    if (!RingAlloc(size, std::max(16u, fi.block_bytes), at)) return;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (uint32_t s = 0; s < slices; ++s)
      for (uint32_t y = 0; y < rows; ++y)
        std::memcpy(ring_.mapped + at + (uint64_t(s) * rows + y) * tight, src + (uint64_t(s) * rows + y) * row_bytes, tight);
    EndPass(3);
    Transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = at;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, is3d ? 0u : layer, 1};
    c.imageExtent = {fi.block ? std::min(w, rows * 4) : w, fi.block ? std::min(hgt, rows * 4) : rows, slices};
    if (fi.block) {  // whole blocks: the copy extent may round up only at the image's edge
      c.imageExtent.width = w;
      c.imageExtent.height = std::min(hgt, rows * 4);
    }
    vk_.vkCmdCopyBufferToImage(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), ring_.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    stats_.upload_bytes += size;
    stats_.texture_upload_bytes += size;
  }
  void Draw(uint32_t vertex_count) override {
    if (!vertex_count || !PrepareDraw()) return;
    vk_.vkCmdDraw(C(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT), vertex_count, 1, 0, 0);
    ++stats_.draws;
  }
  void DrawIndexed(const uint32_t* indices, uint32_t index_count) override { DrawIndexedKeep(indices, index_count); }
  // Patch 0081 (as D3D12's job 178): the indices' ring offset is returned so repeats of the same index range in this
  // command buffer draw from the same copy (DrawIndexedAt) instead of converting and copying again. The ring is bound
  // as the index buffer once per command buffer, each draw choosing its range with firstIndex.
  uint64_t DrawIndexedKeep(const uint32_t* indices, uint32_t index_count) override {
    if (!indices || !index_count || !Record()) return UINT64_MAX;
    uint64_t at = 0;
    if (!RingAlloc(uint64_t(index_count) * 4, 4, at)) return UINT64_MAX;
    std::memcpy(ring_.mapped + at, indices, size_t(index_count) * 4);
    stats_.upload_bytes += uint64_t(index_count) * 4;
    if (!PrepareDraw()) return UINT64_MAX;
    DrawRingIndices(at, index_count);
    return at;
  }
  bool DrawIndexedAt(uint64_t offset, uint32_t index_count, uint64_t list) override {
    if (!cmd_ || list != stats_.submits || !index_count || offset + uint64_t(index_count) * 4 > ring_.size) return false;
    if (!PrepareDraw()) return false;  // as D3D12: the caller falls back to DrawIndexed
    DrawRingIndices(offset, index_count);
    ++stats_.index_draws_reused;
    return true;
  }
  void DrawRingIndices(uint64_t at, uint32_t index_count) {
    if (bound_index_ != ring_.buffer) {
      vk_.vkCmdBindIndexBuffer(cmd_, ring_.buffer, 0, VK_INDEX_TYPE_UINT32);
      bound_index_ = ring_.buffer;
    }
    vk_.vkCmdDrawIndexed(C(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT), index_count, 1, uint32_t(at / 4), 0, 0);
    ++stats_.draws;
  }
  void NextFrame() override {}
  BufferHandle CreateIndexBuffer(const uint32_t* indices, uint32_t index_count) override {
    if (!ready_ || lost_ || !indices || !index_count || !Record()) return 0;
    const uint64_t size = uint64_t(index_count) * 4;
    IndexBuffer ib;
    if (!MakeBuffer(size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ib.buffer, false, 0))
      return 0;
    uint64_t at = 0;
    if (!RingAlloc(size, 4, at)) { Free(ib.buffer); return 0; }
    std::memcpy(ring_.mapped + at, indices, size);
    EndPass(2);
    VkBufferCopy c{at, 0, size};
    vk_.vkCmdCopyBuffer(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), ring_.buffer, ib.buffer.buffer, 1, &c);
    FullBarrier();
    ib.live = true;
    for (BufferHandle i = 1; i < index_buffers_.size(); ++i)
      if (!index_buffers_[i].live) { index_buffers_[i] = ib; return i; }
    if (index_buffers_.empty()) index_buffers_.push_back({});
    index_buffers_.push_back(ib);
    ++stats_.index_buffers;
    stats_.index_buffer_bytes += size;
    return BufferHandle(index_buffers_.size() - 1);
  }
  void ReleaseIndexBuffer(BufferHandle h) override {
    if (!h || h >= index_buffers_.size() || !index_buffers_[h].live) return;
    Defer(VK_NULL_HANDLE, VK_NULL_HANDLE, index_buffers_[h].buffer.buffer, index_buffers_[h].buffer.memory);
    index_buffers_[h] = {};
  }
  void DrawIndexedBuffer(BufferHandle h, uint32_t index_count) override {
    if (!h || h >= index_buffers_.size() || !index_buffers_[h].live || !index_count || !PrepareDraw()) return;
    if (bound_index_ != index_buffers_[h].buffer.buffer) {
      vk_.vkCmdBindIndexBuffer(cmd_, index_buffers_[h].buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
      bound_index_ = index_buffers_[h].buffer.buffer;
    }
    vk_.vkCmdDrawIndexed(C(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT), index_count, 1, 0, 0, 0);
    ++stats_.draws;
    ++stats_.index_buffer_draws;
  }

  // ---- occlusion queries (V4) ----
  OcclusionHandle BeginOcclusionQuery() override { return 0; }
  void EndOcclusionQuery(OcclusionHandle) override {}
  bool GetOcclusionResult(OcclusionHandle, uint64_t*) override { return false; }
  void ReleaseOcclusionQuery(OcclusionHandle) override {}

  // ---- compute (V4) ----
  void Dispatch(PipelineHandle ph, const ComputeBindings& b, uint32_t gx, uint32_t gy, uint32_t gz) override {
    if (!ready_ || lost_ || !ph || ph >= pipelines_.size() || !pipelines_[ph]->compute || !gx || !gy || !gz || !Record()) return;
    const Shader& sh = shaders_[pipelines_[ph]->compute];
    EndPass(4);
    // Everything the dispatch reads or writes in GENERAL, behind one full barrier (V5 narrows this).
    for (uint32_t i = 0; i < 4; ++i) if (Valid(b.srv[i])) Transition(textures_[b.srv[i]], VK_IMAGE_LAYOUT_GENERAL);
    if (Valid(b.uav_texture)) Transition(textures_[b.uav_texture], VK_IMAGE_LAYOUT_GENERAL);
    if (Valid(b.uav2_texture)) Transition(textures_[b.uav2_texture], VK_IMAGE_LAYOUT_GENERAL);
    FullBarrier();
    VkDescriptorSet set = AllocateComputeSet(sh.set_layout);
    if (!set) return;
    std::vector<VkWriteDescriptorSet> wr;
    VkDescriptorBufferInfo bufs[8];
    VkDescriptorImageInfo imgs[8];
    // Binding 0: the constants (b0, up to 16 dwords) in the ring.
    if (sh.types[0] == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
      uint64_t at = 0;
      const uint32_t n = std::min(b.constant_count, 16u) * 4;
      if (!RingAllocUniform(64, at)) return;
      std::memset(ring_.mapped + at, 0, 64);
      std::memcpy(ring_.mapped + at, b.constants, n);
      bufs[0] = {ring_.buffer, at, 64};
      wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufs[0], nullptr});
    }
    // Bindings 1-4: t0-t3; 5: u0; 6: u1.
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t binding = 1 + i;
      const VkDescriptorType ty = sh.types[binding];
      if (ty == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;
      if (ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {  // t3 = the guest-memory raw buffer (guest_t3)
        bufs[binding] = {guest_.buffer, 0, VK_WHOLE_SIZE};
        wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, ty, nullptr, &bufs[binding], nullptr});
        continue;
      }
      TextureHandle th = Valid(b.srv[i]) ? b.srv[i] : defaults_[1][0];
      TextureView v;
      v.texture = th;
      v.plane = uint8_t(b.srv_plane[i]);
      imgs[binding] = {VK_NULL_HANDLE, SampledView(th, v, ViewDimension::k2D), VK_IMAGE_LAYOUT_GENERAL};
      if (!imgs[binding].imageView) { NotDone("Dispatch: image view creation failed"); return; }  // outside review
      wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &imgs[binding], nullptr, nullptr});
    }
    for (uint32_t u = 0; u < 2; ++u) {
      const uint32_t binding = 5 + u;
      const VkDescriptorType ty = sh.types[binding];
      if (ty == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;
      if (ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {  // u0 = guest memory (guest_u0) or the scratch buffer (scratch_u0)
        if (u == 0 && b.scratch_u0 && scratch_.buffer) bufs[binding] = {scratch_.buffer, 0, VK_WHOLE_SIZE};
        else bufs[binding] = {guest_.buffer, 0, VK_WHOLE_SIZE};
        wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, ty, nullptr, &bufs[binding], nullptr});
        continue;
      }
      const TextureHandle th = u == 0 ? b.uav_texture : b.uav2_texture;
      if (!Valid(th)) { NotDone("Dispatch with a null storage image"); return; }
      imgs[binding] = {VK_NULL_HANDLE, StorageView(th, u == 0 ? b.uav_mip : 0), VK_IMAGE_LAYOUT_GENERAL};
      if (!imgs[binding].imageView) { NotDone("Dispatch: storage view creation failed"); return; }  // outside review
      wr.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &imgs[binding], nullptr, nullptr});
    }
    vk_.vkUpdateDescriptorSets(device_, uint32_t(wr.size()), wr.data(), 0, nullptr);
    vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[ph]->pipeline);
    vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, sh.layout, 0, 1, &set, 0, nullptr);
    vk_.vkCmdDispatch(C(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT), gx, gy, gz);
    FullBarrier();  // its writes visible to whatever comes next
    ++stats_.dispatches;
  }

  // ---- readback ----
  ReadbackHandle ReadbackTexture(TextureHandle h, const Rect& region) override {
    if (!Valid(h) || !Record()) return 0;
    Texture& t = textures_[h];
    if (t.aspect != VK_IMAGE_ASPECT_COLOR_BIT || Info(t.desc.format).block) return 0;
    const int32_t x0 = std::clamp(region.x0, 0, int32_t(t.desc.width)), y0 = std::clamp(region.y0, 0, int32_t(t.desc.height));
    const int32_t x1 = std::clamp(region.x1, x0, int32_t(t.desc.width)), y1 = std::clamp(region.y1, y0, int32_t(t.desc.height));
    if (x1 <= x0 || y1 <= y0) return 0;
    const uint32_t bpp = Info(t.desc.format).block_bytes;
    const uint32_t pitch = uint32_t(x1 - x0) * bpp;
    Readback rb;
    rb.row_pitch = pitch;
    if (!MakeBuffer(uint64_t(pitch) * uint32_t(y1 - y0), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, rb.buffer, true,
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
      return 0;
    EndPass(7);
    Transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {x0, y0, 0};
    c.imageExtent = {uint32_t(x1 - x0), uint32_t(y1 - y0), 1};
    vk_.vkCmdCopyImageToBuffer(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer.buffer, 1, &c);
    return AddReadback(std::move(rb));
  }
  ReadbackHandle ReadbackGuest(uint32_t address, uint32_t size) override {
    if (!ready_ || lost_ || !size || uint64_t(address) + size > guest_.size || !Record()) return 0;
    Readback rb;
    rb.row_pitch = size;
    if (!MakeBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, rb.buffer, true,
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
      return 0;
    EndPass(7);
    GuestBarrier(true);
    VkBufferCopy c{address, 0, size};
    vk_.vkCmdCopyBuffer(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), guest_.buffer, rb.buffer.buffer, 1, &c);
    return AddReadback(std::move(rb));
  }
  // Patch 0021's scratch path (BCn loads written by a compute shader as bytes, then copied into the texture).
  bool PrepareScratchFor(TextureHandle h, uint32_t mip, uint32_t layer, uint32_t& row_pitch, uint32_t& rows) override {
    row_pitch = rows = 0;
    if (!Valid(h)) return false;
    const Texture& t = textures_[h];
    const FormatInfo fi = Info(t.desc.format);
    if (mip >= std::max(1u, t.desc.mips) || (t.desc.dimension != TextureDimension::k3D && layer >= std::max(1u, t.desc.depth_or_layers))) return false;
    const uint32_t w = std::max(1u, t.desc.width >> mip), hh = std::max(1u, t.desc.height >> mip);
    const uint32_t tw = fi.block ? (w + 3) / 4 : w, th = fi.block ? (hh + 3) / 4 : hh;
    const uint32_t d = t.desc.dimension == TextureDimension::k3D ? std::max(1u, std::max(1u, t.desc.depth_or_layers) >> mip) : 1u;
    row_pitch = tw * fi.block_bytes;
    rows = th;
    const uint64_t need = uint64_t(row_pitch) * th * d;
    if (need > scratch_.size) {
      if (scratch_.buffer) Defer(VK_NULL_HANDLE, VK_NULL_HANDLE, scratch_.buffer, scratch_.memory);
      scratch_ = {};
      if (!MakeBuffer(std::max<uint64_t>(need, 4ull << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratch_, false, 0)) { row_pitch = rows = 0; return false; }
    }
    scratch_layout_ = {h, mip, layer, row_pitch, rows, d};
    return true;
  }
  void CopyScratchToTexture(TextureHandle h, uint32_t mip, uint32_t layer) override {
    if (!Valid(h) || !Record() || scratch_layout_.texture != h || scratch_layout_.mip != mip || scratch_layout_.layer != layer) return;
    Texture& t = textures_[h];
    const FormatInfo fi = Info(t.desc.format);
    EndPass(3);
    FullBarrier();
    Transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const bool is3d = t.desc.dimension == TextureDimension::k3D;
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, is3d ? 0u : layer, 1};
    c.imageExtent = {std::max(1u, t.desc.width >> mip), std::max(1u, t.desc.height >> mip), scratch_layout_.slices};
    (void)fi;
    vk_.vkCmdCopyBufferToImage(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), scratch_.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    ++stats_.scratch_texture_copies;
  }
  bool IsReadbackReady(ReadbackHandle h) override {
    if (!h || h >= readbacks_.size() || !readbacks_[h].live) return false;
    return readbacks_[h].fence && CompletedFence() >= readbacks_[h].fence;
  }
  const uint8_t* ReadbackData(ReadbackHandle h, uint32_t& row_pitch) override {
    if (!IsReadbackReady(h)) return nullptr;
    row_pitch = readbacks_[h].row_pitch;
    return readbacks_[h].buffer.mapped;
  }
  void ReleaseReadback(ReadbackHandle h) override {
    if (!h || h >= readbacks_.size() || !readbacks_[h].live) return;
    Readback& rb = readbacks_[h];
    if (rb.buffer.mapped) vk_.vkUnmapMemory(device_, rb.buffer.memory);
    rb.buffer.mapped = nullptr;
    Defer(VK_NULL_HANDLE, VK_NULL_HANDLE, rb.buffer.buffer, rb.buffer.memory);
    rb = {};
  }

  // ---- submission ----
  uint64_t Submit() override {
    if (!ready_ || lost_) return submitted_;
    if (!cmd_) return submitted_;  // nothing recorded: the last value is already the right one to wait for
    EndPass(8);
    if (debug_depth_ && vk_.vkCmdEndDebugUtilsLabelEXT)
      for (; debug_depth_; --debug_depth_) vk_.vkCmdEndDebugUtilsLabelEXT(cmd_);
    if (ts_open_) TsEmit(0xFE);  // patch 0077: the end mark
    FlushBarriers();  // patch 0082: queued layout changes must be in this list (the tracked layouts already say so)
    VkResult r = vk_.vkEndCommandBuffer(cmd_);
    if (r != VK_SUCCESS) { Fail(VkErr("vkEndCommandBuffer", r)); return submitted_; }
    const uint64_t value = submitted_ + 1;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = cmd_;
    VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = timeline_;
    sig.value = value;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    LockQueue();
    r = vk_.vkQueueSubmit2(queue_, 1, &si, VK_NULL_HANDLE);
    UnlockQueue();
    if (r != VK_SUCCESS) { Fail(VkErr("vkQueueSubmit2", r)); return submitted_; }
    submitted_ = value;
    slots_[slot_].fence = value;
    if (ts_open_) {
      ts_cur_.fence = value;
      ts_pending_.push_back(std::move(ts_cur_));
      ts_open_ = false;
    }
    for (auto& dp : pools_) if (dp.fence == UINT64_MAX) dp.fence = value;
    for (auto& dp : compute_pools_) if (dp.fence == UINT64_MAX) dp.fence = value;
    used_pools_.clear();
    for (auto& rb : readbacks_)
      if (rb.live && !rb.fence) rb.fence = value;
    ring_marks_.push_back({value, ring_allocated_});
    cmd_ = VK_NULL_HANDLE;
    // Patch 0079: the workers' build times into the stats (the renderer logs the driver's time per new pipeline), and
    // the pipeline cache to disk every pipeline_save_seconds when new pipelines were built.
    stats_.pipelines_built_async = built_async_.load();
    stats_.pipeline_async_ms = double(async_us_.load()) / 1000.0;
    stats_.pipeline_async_max_ms = double(async_max_us_.load()) / 1000.0;
    stats_.pipeline_async_bytes = async_bytes_.load();
    MaybeSaveCache(false);
    ++stats_.submits;
    Retire();
    return value;
  }
  uint64_t CompletedFence() override {
    if (!ready_) return 0;
    uint64_t v = 0;
    if (vk_.vkGetSemaphoreCounterValue(device_, timeline_, &v) != VK_SUCCESS) return completed_;
    completed_ = std::max(completed_, v);
    return completed_;
  }
  void WaitForFence(uint64_t fence) override {
    if (!ready_ || lost_ || !fence || CompletedFence() >= fence) return;
    if (fence > submitted_) Submit();  // waiting for work still being recorded: send it first
    const auto t0 = std::chrono::steady_clock::now();
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &fence;
    const VkResult r = vk_.vkWaitSemaphores(device_, &wi, 10ull * 1000 * 1000 * 1000);
    if (r == VK_TIMEOUT) Fail("the GPU did not finish within 10 s (hang)");
    else if (r != VK_SUCCESS) Fail(VkErr("vkWaitSemaphores", r));
    ++stats_.fence_waits;
    stats_.fence_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CompletedFence();
  }
  void BeginEvent(const char* name) override {
    if (!debug_utils_ || !vk_.vkCmdBeginDebugUtilsLabelEXT || !Record()) return;
    VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    l.pLabelName = name ? name : "";
    vk_.vkCmdBeginDebugUtilsLabelEXT(cmd_, &l);
    ++debug_depth_;
  }
  void EndEvent() override {
    if (!debug_depth_ || !cmd_ || !vk_.vkCmdEndDebugUtilsLabelEXT) return;
    vk_.vkCmdEndDebugUtilsLabelEXT(cmd_);
    --debug_depth_;
  }
  const BackendStats& Stats() const override { return stats_; }

 private:
  // ---- stage V2: binding layout, shaders, pipelines, descriptors, passes ----
  static constexpr uint32_t kCbCount = 4;        // b0-b3 per stage
  // The window of each dynamic uniform buffer (b1 = 256 float4; job 634: 8 KB - the fetch buffer with the
  // tex_post_affine constants of all 32 slots is 4,864 bytes).
  static constexpr uint32_t kCbRange = 8192;
  static constexpr uint32_t kViewBindings = 96;  // t0-t95
  static constexpr uint64_t kRingReserved = 4096;  // ring bytes [0, 4096): zeros, the unset constant buffers

  // DXC places our fixed shaders' registers in set 0 with these shifts (b +0, t +100, s +200, u +300); the translator's
  // shaders carry [[vk::binding]] for t (set 1) and s (set 2). Both end up in the layout above.
  static void RemapBindings(std::vector<uint32_t>& w, ShaderStage stage) {
    struct Dec { size_t set_at = 0, binding_at = 0; };
    std::unordered_map<uint32_t, Dec> decs;
    for (size_t i = 5; i < w.size();) {
      const uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
      if (!n || i + n > w.size()) break;
      if (op == 71 && n >= 4) {  // OpDecorate %id DescriptorSet / Binding value
        if (w[i + 2] == 34) decs[w[i + 1]].set_at = i + 3;
        if (w[i + 2] == 33) decs[w[i + 1]].binding_at = i + 3;
      }
      i += n;
    }
    const uint32_t cb_base = stage == ShaderStage::kPixel ? kCbCount : 0;
    for (auto& [id, d] : decs) {
      if (!d.set_at || !d.binding_at) continue;
      uint32_t& set = w[d.set_at];
      uint32_t& b = w[d.binding_at];
      if (stage == ShaderStage::kCompute) {  // one set: 0 = b0, 1-4 = t0-t3, 5-6 = u0-u1
        if (set == 0 && b >= 100 && b < 104) b = 1 + (b - 100);
        else if (set == 0 && b >= 300 && b < 302) b = 5 + (b - 300);
        continue;
      }
      if (set != 0) continue;  // explicit set 1 / 2 from the translator
      if (b < kCbCount) b += cb_base;
      else if (b >= 100 && b < 200) { set = 1; b -= 100; }
      else if (b >= 200 && b < 232) { set = 2; b -= 200; }
      else if (b >= 300 && b < 302) { set = 1; b = 98 + (b - 300); }
    }
  }

  bool CreateLayouts(std::string& error) {
    VkDescriptorSetLayoutBinding cb[2 * kCbCount];
    for (uint32_t i = 0; i < 2 * kCbCount; ++i)
      cb[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
               VkShaderStageFlags(i < kCbCount ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT
                                               : VK_SHADER_STAGE_FRAGMENT_BIT), nullptr};
    std::vector<VkDescriptorSetLayoutBinding> tex;
    const VkShaderStageFlags gfx = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_GEOMETRY_BIT;
    for (uint32_t i = 0; i < kViewBindings; ++i) tex.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, gfx, nullptr});
    for (uint32_t i = 96; i < 99; ++i) tex.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, gfx, nullptr});
    VkDescriptorSetLayoutBinding smp[32];
    for (uint32_t i = 0; i < 32; ++i) smp[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, gfx, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2 * kCbCount;
    li.pBindings = cb;
    if (vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layouts_[0]) != VK_SUCCESS) { error = "set layout 0"; return false; }
    li.bindingCount = uint32_t(tex.size());
    li.pBindings = tex.data();
    if (vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layouts_[1]) != VK_SUCCESS) { error = "set layout 1"; return false; }
    li.bindingCount = 32;
    li.pBindings = smp;
    if (vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layouts_[2]) != VK_SUCCESS) { error = "set layout 2"; return false; }
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 3;
    pi.pSetLayouts = set_layouts_;
    if (vk_.vkCreatePipelineLayout(device_, &pi, nullptr, &layout_) != VK_SUCCESS) { error = "pipeline layout"; return false; }
    // Set 0 once (all eight point at the ring; the draw supplies the offsets). Its own small pool.
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * kCbCount};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    if (vk_.vkCreateDescriptorPool(device_, &dp, nullptr, &cb_pool_) != VK_SUCCESS) { error = "constant pool"; return false; }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = cb_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layouts_[0];
    if (vk_.vkAllocateDescriptorSets(device_, &ai, &cb_set_) != VK_SUCCESS) { error = "constant set"; return false; }
    VkDescriptorBufferInfo bi[2 * kCbCount];
    VkWriteDescriptorSet wr[2 * kCbCount];
    for (uint32_t i = 0; i < 2 * kCbCount; ++i) {
      bi[i] = {ring_.buffer, 0, kCbRange};
      wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cb_set_, i, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &bi[i], nullptr};
    }
    vk_.vkUpdateDescriptorSets(device_, 2 * kCbCount, wr, 0, nullptr);
    std::memset(ring_.mapped, 0, kRingReserved);
    ring_head_ = kRingReserved;
    // Patch 0079: the Vulkan pipeline cache on disk next to D3D12's library (the driver checks its own header and
    // ignores data from another driver or GPU).
    if (!options_.pipeline_library_path.empty()) {
      std::filesystem::path pth(options_.pipeline_library_path);
      std::string name = pth.filename().string();
      const size_t at = name.find("d3d12");
      name = at != std::string::npos ? name.replace(at, 5, "vulkan") : name + ".vulkan";
      cache_path_ = (pth.parent_path() / name).string();
    }
    std::vector<char> initial;
    if (!cache_path_.empty()) {
      std::ifstream f(cache_path_, std::ios::binary | std::ios::ate);
      if (f) {
        const std::streamoff n = f.tellg();
        if (n > 0 && n < (std::streamoff(1) << 30)) {
          initial.resize(size_t(n));
          f.seekg(0);
          f.read(initial.data(), n);
          if (!f) initial.clear();
        }
      }
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = initial.size();
    ci.pInitialData = initial.empty() ? nullptr : initial.data();
    if (vk_.vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_vk_) != VK_SUCCESS && !initial.empty()) {
      ci.initialDataSize = 0;  // unusable data: start empty
      ci.pInitialData = nullptr;
      if (vk_.vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_vk_) != VK_SUCCESS)
        pipeline_cache_vk_ = VK_NULL_HANDLE;  // outside review: Vulkan allows no cache - pipelines are just not cached
    }
    if (!pipeline_cache_vk_) REXLOG_WARN("gow2_renderer: Vulkan pipeline cache couldn't be created - running without one");
    if (!cache_path_.empty())
      REXLOG_INFO("gow2_renderer: Vulkan pipeline cache {} ({} KB loaded)", cache_path_, (initial.size() + 1023) / 1024);
    cache_saved_at_ = std::chrono::steady_clock::now();
    return true;
  }
  // White 2D / cube / 3D textures for unbound slots, black ones for TextureView::zero.
  bool CreateDefaults(std::string& error) {
    const TextureDimension dims[3] = {TextureDimension::k2D, TextureDimension::kCube, TextureDimension::k3D};
    for (int z = 0; z < 2; ++z)
      for (int i = 0; i < 3; ++i) {
        TextureDesc d;
        d.dimension = dims[i];
        d.width = d.height = 1;
        d.depth_or_layers = dims[i] == TextureDimension::kCube ? 6 : 1;
        d.format = Format::kR8G8B8A8Unorm;
        TextureHandle h = CreateTexture(d);
        if (!h) { error = "default textures"; return false; }
        const uint32_t px = z ? 0u : 0xFFFFFFFFu;
        for (uint32_t l = 0; l < d.depth_or_layers; ++l) UploadTexture(h, 0, l, &px, 4, 1, 1);
        defaults_[z][i] = h;
      }
    return true;
  }

  bool StartWorkers() {
    if (!workers_.empty()) return true;
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const int want = options_.pipeline_threads;
    const uint32_t n = want < 0 ? std::max(1u, hw * 3 / 4) : want == 0 ? std::max(1u, std::min(4u, hw / 4)) : std::clamp(uint32_t(want), 1u, hw);
    worker_count_ = n;  // read by the workers (workers_ grows while they start)
    workers_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) workers_.emplace_back([this] { Worker(); });
    REXLOG_INFO("gow2_renderer: Vulkan pipeline workers: {} thread(s)", n);
    return true;
  }
  void Worker() {
    for (;;) {
      PipelineJob job;
      bool backlog = false;
      {
        std::unique_lock<std::mutex> lock(job_mutex_);
        // Patch 0076: live requests first; backlog jobs (start-up set, guessed pipelines) on at most half the workers,
        // so the driver time a draw is waiting for is never all taken by guesses.
        const auto backlog_ok = [this] { return !backlog_jobs_.empty() && backlog_busy_ < std::max<size_t>(1, worker_count_ / 2); };
        job_cv_.wait(lock, [&] { return stop_workers_ || !jobs_.empty() || backlog_ok(); });
        if (stop_workers_) return;
        backlog = jobs_.empty();
        auto& q = backlog ? backlog_jobs_ : jobs_;
        job = q.front();
        q.pop_front();
        ++jobs_busy_;
        if (backlog) ++backlog_busy_;
      }
      const auto t0 = std::chrono::steady_clock::now();
      Build(job);
      const uint64_t us = uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
      built_async_.fetch_add(1);
      async_us_.fetch_add(us);
      async_bytes_.fetch_add(job.bytes);
      for (uint64_t m = async_max_us_.load(); us > m && !async_max_us_.compare_exchange_weak(m, us);) {}
      {
        std::lock_guard<std::mutex> lock(job_mutex_);
        --jobs_busy_;
        if (backlog) --backlog_busy_;
      }
      if (backlog) job_cv_.notify_one();  // a backlog slot is free again
    }
  }
  static VkCompareOp Compare(CompareFunc f) { return VkCompareOp(uint32_t(f)); }  // same order as VkCompareOp
  static VkStencilOp StencilOpVk(StencilOp o) {
    switch (o) {
      case StencilOp::kKeep: return VK_STENCIL_OP_KEEP;
      case StencilOp::kZero: return VK_STENCIL_OP_ZERO;
      case StencilOp::kReplace: return VK_STENCIL_OP_REPLACE;
      case StencilOp::kIncrSat: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
      case StencilOp::kDecrSat: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
      case StencilOp::kInvert: return VK_STENCIL_OP_INVERT;
      case StencilOp::kIncrWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
      case StencilOp::kDecrWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
  }
  static VkBlendFactor Factor(BlendFactor f) {
    static const VkBlendFactor k[] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
                                      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
                                      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
                                      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA,
                                      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR,
                                      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    return k[std::min<uint32_t>(uint32_t(f), 12)];
  }
  static VkBlendOp Op(BlendOp o) {
    static const VkBlendOp k[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT};
    return k[std::min<uint32_t>(uint32_t(o), 4)];
  }
  VkFormat AttachmentFormat(Format f, bool bits) const {
    if (f == Format::kUnknown) return VK_FORMAT_UNDEFINED;
    const FormatInfo fi = Info(f);
    if (fi.depth) return f == Format::kD24UnormS8 && !d24s8_ ? VK_FORMAT_D32_SFLOAT_S8_UINT : fi.format;
    return bits && fi.bits != VK_FORMAT_UNDEFINED ? fi.bits : fi.format;
  }
  // Builds one pipeline (any thread: Vulkan pipeline creation is free-threaded; the cache is internally synchronised).
  void Build(const PipelineJob& job) {
    const GraphicsPipelineDesc& d = job.desc;
    VkPipelineShaderStageCreateInfo st[3];
    uint32_t ns = 0;
    const VkShaderStageFlagBits bits[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    for (int i = 0; i < 3; ++i)
      if (job.modules[i]) st[ns++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, bits[i], job.modules[i], "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    static const VkPrimitiveTopology topo[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                               VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    ia.topology = topo[std::min<uint32_t>(uint32_t(d.topology), 4)];
    ia.primitiveRestartEnable = d.primitive_restart && (d.topology == Topology::kLineStrip || d.topology == Topology::kTriangleStrip);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = !d.depth_clip && depth_clamp_;
    rs.polygonMode = d.wireframe && fill_mode_non_solid_ ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cull == CullMode::kFront ? VK_CULL_MODE_FRONT_BIT : d.cull == CullMode::kBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = d.front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = d.depth_bias != 0 || d.depth_bias_slope != 0.0f;
    rs.depthBiasConstantFactor = float(d.depth_bias);
    rs.depthBiasSlopeFactor = d.depth_bias_slope;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(std::max(1u, d.samples));
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth_enable;
    ds.depthWriteEnable = d.depth_write;
    ds.depthCompareOp = Compare(d.depth_func);
    ds.stencilTestEnable = d.stencil_enable;
    auto face = [&](const StencilFace& f) {
      VkStencilOpState o{};
      o.failOp = StencilOpVk(f.fail);
      o.passOp = StencilOpVk(f.pass);
      o.depthFailOp = StencilOpVk(f.depth_fail);
      o.compareOp = Compare(f.func);
      o.compareMask = d.stencil_read_mask;
      o.writeMask = d.stencil_write_mask;
      return o;
    };
    ds.front = face(d.stencil_front);
    ds.back = face(d.stencil_back);
    VkPipelineColorBlendAttachmentState cba[GraphicsPipelineDesc::kMaxColor] = {};
    VkFormat cf[GraphicsPipelineDesc::kMaxColor] = {};
    const uint32_t nc = std::min<uint32_t>(d.color_count, GraphicsPipelineDesc::kMaxColor);
    for (uint32_t i = 0; i < nc; ++i) {
      const BlendDesc& b = d.color_blend[i];
      cba[i].blendEnable = b.enable;
      cba[i].srcColorBlendFactor = b.enable ? Factor(b.src) : VK_BLEND_FACTOR_ONE;
      cba[i].dstColorBlendFactor = b.enable ? Factor(b.dst) : VK_BLEND_FACTOR_ZERO;
      cba[i].colorBlendOp = b.enable ? Op(b.op) : VK_BLEND_OP_ADD;
      cba[i].srcAlphaBlendFactor = b.enable ? Factor(b.src_alpha) : VK_BLEND_FACTOR_ONE;
      cba[i].dstAlphaBlendFactor = b.enable ? Factor(b.dst_alpha) : VK_BLEND_FACTOR_ZERO;
      cba[i].alphaBlendOp = b.enable ? Op(b.op_alpha) : VK_BLEND_OP_ADD;
      cba[i].colorWriteMask = d.color_formats[i] == Format::kUnknown ? 0 : (d.color_write_masks[i] & 0xF);
      cf[i] = AttachmentFormat(d.color_formats[i], d.color_bits);
      if (cf[i] == VK_FORMAT_UNDEFINED) cba[i].colorWriteMask = 0;
      if (d.color_bits || IntegerFormat(cf[i])) cba[i].blendEnable = VK_FALSE;  // no blending on integer views
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = nc;
    cb.pAttachments = cba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                  VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = nc;
    ri.pColorAttachmentFormats = cf;
    const VkFormat df = AttachmentFormat(d.depth_format, false);
    ri.depthAttachmentFormat = df;
    ri.stencilAttachmentFormat = df;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;
    gi.stageCount = ns;
    gi.pStages = st;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = layout_;
    if (pipeline_stats_) gi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;  // patch 0104
    Pipeline& p = *pipelines_[job.handle];
    const VkResult r = vk_.vkCreateGraphicsPipelines(device_, pipeline_cache_vk_, 1, &gi, nullptr, &p.pipeline);
    if (r != VK_SUCCESS) {
      p.failed.store(true);
      REXLOG_ERROR("gow2_renderer: Vulkan: {} (vs {} ps {} gs {} topology {} targets {} depth {})", VkErr("vkCreateGraphicsPipelines", r),
                   d.vs, d.ps, d.gs, uint32_t(d.topology), d.color_count, uint32_t(d.depth_format));
    }
    p.ready.store(true, std::memory_order_release);
  }
  static bool IntegerFormat(VkFormat f) {
    switch (f) {
      case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32_UINT:
      case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R16_UINT:
      case VK_FORMAT_R32G32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: return true;
      default: return false;
    }
  }

  // ---- render passes (dynamic rendering), begun at the first draw on the current targets ----
  struct TargetSet {
    uint32_t count = 0;
    TextureHandle colors[GraphicsPipelineDesc::kMaxColor] = {};
    TextureHandle depth = 0;
    bool bits = false;
    bool operator==(const TargetSet&) const = default;
  };
  VkImageView AttachmentView(Texture& t, bool bits) {
    if (!bits || t.aspect != VK_IMAGE_ASPECT_COLOR_BIT) return t.attachment_view;
    const VkFormat bf = Info(t.desc.format).bits;
    if (bf == VK_FORMAT_UNDEFINED || t.can_promote) return t.attachment_view;  // can_promote here: diagnostic "never"
    if (!t.bits_view) {
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = t.image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = bf;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (vk_.vkCreateImageView(device_, &vi, nullptr, &t.bits_view) != VK_SUCCESS) {
        t.bits_view = VK_NULL_HANDLE;
        return t.attachment_view;  // outside review: no bits view - the plain attachment view, never a null one
      }
    }
    return t.bits_view;
  }
  bool BeginPass() {
    if (in_pass_) return true;
    if (targets_.bits)  // patch 0087: a bits (UINT) attachment view needs a mutable image
      for (uint32_t i = 0; i < targets_.count; ++i)
        if (TextureHandle c = targets_.colors[i]; c && textures_[c].can_promote) Promote(c);
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    // Attachments in GENERAL (they are also sampled elsewhere; V5 narrows this), behind one full barrier. Patch 0100
    // (--gow2_renderer_vulkan_attachment_layouts=optimal): in COLOR_ / DEPTH_STENCIL_ATTACHMENT_OPTIMAL instead, back
    // to GENERAL in the next full barrier that comes when they are not being drawn to (FullBarrier).
    for (uint32_t i = 0; i < targets_.count; ++i)
      if (TextureHandle c = targets_.colors[i]) {
        ToAttachmentLayout(c);
        w = std::min(w, textures_[c].desc.width); h = std::min(h, textures_[c].desc.height);
      }
    if (targets_.depth) {
      ToAttachmentLayout(targets_.depth);
      w = std::min(w, textures_[targets_.depth].desc.width); h = std::min(h, textures_[targets_.depth].desc.height);
    }
    if (w == UINT32_MAX) { w = h = 1; }
    // Patch 0089 (V5b, --gow2_renderer_vulkan_pass_barriers=when_needed): the full barrier only when something since
    // the last one could conflict with this pass - GPU work outside a pass (copies, uploads, dispatches, clears,
    // readbacks, a submit), a memory-export draw, or an attachment of this pass that was sampled or written as an
    // attachment since. Textures this pass samples that were written since are caught per draw (PrepareDraw's split).
    bool need = options_.vk_pass_barriers == 0 || outside_dirty_;
    bool written = false;  // patch 0115: an attachment drawn to since the last full barrier (read_epoch: sampled)
    // Patch 0116: in narrow mode, "since the last barrier of any kind" - one attachment barrier orders every attachment
    // write before it, so a pass that switches back to a target drawn before the last one needs none.
    const bool narrow = options_.vk_pass_barriers == 2;
    for (uint32_t i = 0; i < targets_.count && !need; ++i)
      if (TextureHandle c = targets_.colors[i]) {
        need = textures_[c].read_epoch == barrier_epoch_;
        written |= narrow ? textures_[c].att_written_epoch == att_epoch_ : textures_[c].written_epoch == barrier_epoch_;
      }
    if (!need && targets_.depth) {
      need = textures_[targets_.depth].read_epoch == barrier_epoch_;
      written |= narrow ? textures_[targets_.depth].att_written_epoch == att_epoch_
                        : textures_[targets_.depth].written_epoch == barrier_epoch_;
    }
    if (!need && written && options_.vk_pass_barriers != 2) need = true;
    if (need) FullBarrier();
    else if (written) {  // patch 0115: earlier passes' attachment writes before this pass's attachment reads / writes
      pending_att_ = true;
      ++att_epoch_;  // patch 0116: attachment writes so far are ordered before what follows
      ++stats_.vk_barriers_attachment;
    } else ++stats_.vk_barriers_skipped;
    for (uint32_t i = 0; i < targets_.count; ++i)
      if (TextureHandle c = targets_.colors[i]) {
        textures_[c].written_epoch = barrier_epoch_;
        textures_[c].att_written_epoch = att_epoch_;  // patch 0116
      }
    if (targets_.depth) {
      textures_[targets_.depth].written_epoch = barrier_epoch_;
      textures_[targets_.depth].att_written_epoch = att_epoch_;
    }
    VkRenderingAttachmentInfo ca[GraphicsPipelineDesc::kMaxColor];
    for (uint32_t i = 0; i < targets_.count; ++i) {
      ca[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      if (TextureHandle c = targets_.colors[i]) {
        ca[i].imageView = AttachmentView(textures_[c], targets_.bits);
        ca[i].imageLayout = AttachmentLayout(textures_[c]);
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        pass_written_.push_back(c);
      }
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (targets_.depth) {
      da.imageView = textures_[targets_.depth].attachment_view;
      da.imageLayout = AttachmentLayout(textures_[targets_.depth]);
      da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      pass_written_.push_back(targets_.depth);
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    pass_w_ = w;  // patch 0092
    pass_h_ = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = targets_.count;
    ri.pColorAttachments = ca;
    if (targets_.depth) { ri.pDepthAttachment = &da; ri.pStencilAttachment = &da; }
    vk_.vkCmdBeginRendering(C(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT), &ri);
    in_pass_ = true;
    ++stats_.vk_passes;  // patch 0089
    dirty_viewport_ = dirty_scissor_ = dirty_stencil_ = dirty_blend_ = true;
    bound_pipeline_ = VK_NULL_HANDLE;
    return true;
  }
  // Patch 0089: why each render pass ended (BackendStats::vk_pass_end): 0 other, 1 new targets, 2 guest-memory upload,
  // 3 texture upload, 4 compute dispatch, 5 clear, 6 a draw reads a texture written in the pass, 7 readback / copy,
  // 8 submit, 9 texture created / destroyed.
  void EndPass(uint32_t why = 0) {
    if (why != 1) outside_dirty_ = true;  // patch 0089: GPU work outside a pass follows (or a submit / an unknown)
    if (!in_pass_) return;
    if (occ_active_) OccEnd();  // patch 0098: a query begun in a pass ends in it
    ++stats_.vk_pass_end[std::min<uint32_t>(why, 9)];
    vk_.vkCmdEndRendering(cmd_);
    in_pass_ = false;
  }
  // Patch 0100: the layout a pass draws to an attachment in (GENERAL unless the switch is on).
  VkImageLayout AttachmentLayout(const Texture& t) const {
    if (!options_.vk_attachment_layouts) return VK_IMAGE_LAYOUT_GENERAL;
    return t.aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  }
  void ToAttachmentLayout(TextureHandle h) {
    Texture& t = textures_[h];
    const VkImageLayout to = AttachmentLayout(t);
    if (to != VK_IMAGE_LAYOUT_GENERAL && t.layout != to) {
      ++stats_.vk_attachment_layout_changes;
      att_layout_.push_back(h);
    }
    Transition(t, to);
  }
  bool IsTarget(TextureHandle h) const {
    if (h == targets_.depth) return true;
    for (uint32_t i = 0; i < targets_.count; ++i) if (targets_.colors[i] == h) return true;
    return false;
  }
  void FullBarrier() {
    // Patch 0100: attachments left in an attachment layout go back to GENERAL inside this barrier (one barrier call
    // either way), except the current targets while a pass is about to begin on them; all callers are outside a pass.
    if (!att_layout_.empty()) {
      size_t keep = 0;
      for (TextureHandle h : att_layout_) {
        if (!Valid(h)) continue;
        Texture& t = textures_[h];
        if (t.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && t.layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
          continue;  // moved on already (a copy, a clear, a dispatch)
        if (IsTarget(h)) { att_layout_[keep++] = h; continue; }
        Transition(t, VK_IMAGE_LAYOUT_GENERAL);
      }
      att_layout_.resize(keep);
    }
    ++stats_.vk_full_barriers;  // patch 0089
    ++barrier_epoch_;           // patch 0089: everything before is now ordered and visible
    ++att_epoch_;               // patch 0116
    outside_dirty_ = false;
    GuestBarrier(false);
    pass_written_.clear();
  }

  // ---- descriptors ----
  static uint64_t ViewKey(const TextureView& v) {
    return uint64_t(v.components[0]) | uint64_t(v.components[1]) << 4 | uint64_t(v.components[2]) << 8 |
           uint64_t(v.components[3]) << 12 | uint64_t(v.signed_view) << 16 | uint64_t(v.dimension) << 17 |
           uint64_t(v.plane) << 20 | uint64_t(v.bits_view) << 22;
  }
  // Patch 0087: remakes a compressible colour render target as a mutable image with its view-format list, contents
  // copied (all mips and layers); the old image and its views are freed after the work using them. false = not done
  // (the diagnostic mode "never", or a failure): the caller uses a view of the image's own format.
  bool Promote(TextureHandle h) {
    Texture& t = textures_[h];
    if (!t.can_promote) return true;
    if (options_.vk_mutable_targets == 2) {
      ++stats_.vk_views_refused;
      return false;
    }
    if (!Record()) return false;
    const FormatInfo fi = Info(t.desc.format);
    VkFormat view_formats[3] = {t.format};
    uint32_t view_format_count = 1;
    if (fi.snorm != VK_FORMAT_UNDEFINED && fi.snorm != t.format) view_formats[view_format_count++] = fi.snorm;
    if (!fi.block && fi.bits != VK_FORMAT_UNDEFINED && fi.bits != t.format) view_formats[view_format_count++] = fi.bits;
    VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    fl.viewFormatCount = view_format_count;
    fl.pViewFormats = view_formats;
    const TextureDesc d = t.desc;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.pNext = &fl;
    ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (d.dimension == TextureDimension::kCube) ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ii.imageType = d.dimension == TextureDimension::k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ii.format = t.format;
    ii.extent = {std::max(1u, d.width), std::max(1u, d.height), d.dimension == TextureDimension::k3D ? std::max(1u, d.depth_or_layers) : 1u};
    ii.mipLevels = std::max(1u, d.mips);
    ii.arrayLayers = d.dimension == TextureDimension::k3D ? 1u : std::max(1u, d.depth_or_layers);
    ii.samples = VkSampleCountFlagBits(std::max(1u, d.samples));
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (d.usage & kUsageStorage) ii.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    Texture n;
    n.desc = d;
    n.format = t.format;
    n.aspect = t.aspect;
    if (vk_.vkCreateImage(device_, &ii, nullptr, &n.image) != VK_SUCCESS) { t.can_promote = false; return false; }
    n.vk_usage = ii.usage;  // patch 0101
    n.vk_flags = ii.flags;
    n.vk_view_formats = view_format_count;
    n.memory = AllocateImage(n.image);
    if (!n.memory) { vk_.vkDestroyImage(device_, n.image, nullptr); t.can_promote = false; return false; }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = n.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = n.format;
    vi.subresourceRange = {n.aspect, 0, 1, 0, 1};
    // Outside review: the old image stays live unless the replacement is fully ready.
    if (vk_.vkBindImageMemory(device_, n.image, n.memory, 0) != VK_SUCCESS ||
        vk_.vkCreateImageView(device_, &vi, nullptr, &n.attachment_view) != VK_SUCCESS) {
      vk_.vkDestroyImage(device_, n.image, nullptr);
      vk_.vkFreeMemory(device_, n.memory, nullptr);
      t.can_promote = false;
      return false;
    }
    EndPass(0);
    Transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(n, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    std::vector<VkImageCopy> regions;
    for (uint32_t m = 0; m < ii.mipLevels; ++m) {
      VkImageCopy c{};
      c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, ii.arrayLayers};
      c.extent = {std::max(1u, ii.extent.width >> m), std::max(1u, ii.extent.height >> m), std::max(1u, ii.extent.depth >> m)};
      regions.push_back(c);
    }
    vk_.vkCmdCopyImage(C(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT), t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, n.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       uint32_t(regions.size()), regions.data());
    Transition(n, VK_IMAGE_LAYOUT_GENERAL);
    GuestBarrier(false);
    Defer(t.image, t.attachment_view, VK_NULL_HANDLE, t.memory);
    if (t.bits_view) Defer(VK_NULL_HANDLE, t.bits_view, VK_NULL_HANDLE, VK_NULL_HANDLE);
    for (auto& [k, v] : t.views) Defer(VK_NULL_HANDLE, v, VK_NULL_HANDLE, VK_NULL_HANDLE);
    n.live = true;
    t = std::move(n);
    view_sets_.clear();  // sets that named the old image's views
    ++stats_.vk_targets_made_mutable;
    REXLOG_INFO("gow2_renderer: Vulkan: render target {} ({}x{}, VkFormat {}) made mutable for a view of another "
                "format (patch 0087; {} so far)", h, d.width, d.height, int(t.format), stats_.vk_targets_made_mutable);
    return true;
  }
  VkImageView SampledView(TextureHandle h, const TextureView& v, ViewDimension want) {
    Texture& t = textures_[h];
    const uint64_t key = ViewKey(v) | uint64_t(want) << 24;
    for (auto& [k, view] : t.views) if (k == key) return view;
    const FormatInfo fi = Info(t.desc.format);
    const bool other = (v.signed_view && fi.snorm != VK_FORMAT_UNDEFINED && fi.snorm != t.format) ||
                       (v.bits_view && fi.bits != VK_FORMAT_UNDEFINED && fi.bits != t.format);
    if (other && t.can_promote && !Promote(h)) {  // diagnostic "never": a view of the image's own format instead
      TextureView same = v;
      same.signed_view = same.bits_view = false;
      return SampledView(h, same, want);
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = want == ViewDimension::kCube ? VK_IMAGE_VIEW_TYPE_CUBE : want == ViewDimension::k3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.format;
    if (v.signed_view && fi.snorm != VK_FORMAT_UNDEFINED) vi.format = fi.snorm;
    if (v.bits_view && fi.bits != VK_FORMAT_UNDEFINED) vi.format = fi.bits;
    static const VkComponentSwizzle sw[6] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                             VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
    vi.components = {sw[std::min<uint8_t>(v.components[0], 5)], sw[std::min<uint8_t>(v.components[1], 5)],
                     sw[std::min<uint8_t>(v.components[2], 5)], sw[std::min<uint8_t>(v.components[3], 5)]};
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (fi.depth) {
      aspect = v.plane ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
      // gpu.h: the stencil plane reads as uint in .y (D3D12's X24_TYPELESS_G8_UINT): put the stencil in G.
      if (v.plane) vi.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO};
    }
    const uint32_t layers = t.desc.dimension == TextureDimension::k3D ? 1u : std::max(1u, t.desc.depth_or_layers);
    vi.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, want == ViewDimension::kCube ? 6u : (want == ViewDimension::k2D ? 1u : layers)};
    VkImageView view = VK_NULL_HANDLE;
    if (vk_.vkCreateImageView(device_, &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    t.views.push_back({key, view});
    return view;
  }
  VkSampler SamplerFor(const SamplerDesc& d) {
    uint64_t key = uint64_t(d.mag_linear) | uint64_t(d.min_linear) << 1 | uint64_t(d.mip_linear) << 2 |
                   uint64_t(d.max_anisotropy) << 3 | uint64_t(d.address[0]) << 8 | uint64_t(d.address[1]) << 11 |
                   uint64_t(d.address[2]) << 14 | uint64_t(d.border) << 17;
    uint32_t f[3];
    std::memcpy(&f[0], &d.lod_bias, 4); std::memcpy(&f[1], &d.min_lod, 4); std::memcpy(&f[2], &d.max_lod, 4);
    key ^= (uint64_t(f[0]) << 19) ^ (uint64_t(f[1]) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(f[2]) * 0xC2B2AE3D27D4EB4Full);
    auto it = sampler_objects_.find(key);
    if (it != sampler_objects_.end()) return it->second;
    static const VkSamplerAddressMode am[] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d.mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = d.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = d.mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = am[std::min<uint32_t>(uint32_t(d.address[0]), 4)];
    si.addressModeV = am[std::min<uint32_t>(uint32_t(d.address[1]), 4)];
    si.addressModeW = am[std::min<uint32_t>(uint32_t(d.address[2]), 4)];
    si.mipLodBias = d.lod_bias;
    si.anisotropyEnable = d.max_anisotropy > 1 && anisotropy_;
    si.maxAnisotropy = std::min(float(d.max_anisotropy), limits_.maxSamplerAnisotropy);
    si.minLod = d.min_lod;
    si.maxLod = d.max_lod;
    si.borderColor = d.border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : d.border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                                                         : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler smp = VK_NULL_HANDLE;
    if (vk_.vkCreateSampler(device_, &si, nullptr, &smp) != VK_SUCCESS) {  // outside review: never cache a null
      REXLOG_WARN("gow2_renderer: Vulkan sampler creation failed");
      return VK_NULL_HANDLE;
    }
    sampler_objects_[key] = smp;
    return smp;
  }
  VkDescriptorSet AllocateSet(uint32_t which) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (pool_index_ < pools_.size()) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pools_[pool_index_].pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layouts_[which];
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vk_.vkAllocateDescriptorSets(device_, &ai, &set) == VK_SUCCESS) {
          pools_[pool_index_].fence = UINT64_MAX;  // in use by the submission being recorded
          return set;
        }
      }
      // This pool is full: the next free one (its submissions done), or a new one.
      NextPool();
    }
    return VK_NULL_HANDLE;
  }
  void NextPool() {
    if (pool_index_ < pools_.size()) used_pools_.push_back(pool_index_);
    const uint64_t done = CompletedFence();
    pool_index_ = UINT32_MAX;
    for (uint32_t i = 0; i < pools_.size(); ++i)
      if (pools_[i].fence <= done && std::find(used_pools_.begin(), used_pools_.end(), i) == used_pools_.end()) {
        vk_.vkResetDescriptorPool(device_, pools_[i].pool, 0);
        pools_[i].fence = 0;
        pool_index_ = i;
        break;
      }
    if (pool_index_ == UINT32_MAX) {
      VkDescriptorPoolSize sz[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256 * kViewBindings},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 * 3},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 256 * 32}};
      VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      pi.maxSets = 512;
      pi.poolSizeCount = 3;
      pi.pPoolSizes = sz;
      DescriptorPool dp;
      if (vk_.vkCreateDescriptorPool(device_, &pi, nullptr, &dp.pool) != VK_SUCCESS) { Fail("vkCreateDescriptorPool"); return; }
      pools_.push_back(dp);
      pool_index_ = uint32_t(pools_.size() - 1);
    }
    view_sets_.clear();
    sampler_sets_.clear();
  }
  VkDescriptorSet ViewSet() {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < kViewBindings; ++i) {
      const TextureView& v = views_[i];
      h = (h ^ (uint64_t(v.texture) | ViewKey(v) << 20 | uint64_t(v.zero) << 50)) * 0x100000001B3ull;
    }
    auto it = view_sets_.find(h);
    if (it != view_sets_.end()) return UseCached(it->second);
    VkDescriptorSet set = AllocateSet(1);
    if (!set) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[kViewBindings];
    for (uint32_t i = 0; i < kViewBindings; ++i) {
      const ViewDimension want = i < 32 ? ViewDimension::k2D : i < 64 ? ViewDimension::kCube : ViewDimension::k3D;
      const TextureView& v = views_[i];
      TextureHandle th = v.texture;
      if (!Valid(th) || ViewDimensionOf(textures_[th]) != want) th = defaults_[v.zero || Valid(v.texture) ? 1 : 0][uint32_t(want)];
      VkImageView view = SampledView(th, th == v.texture ? v : TextureView{}, want);
      // Outside review: a view the driver refused - the default texture instead, and no set (draw skipped) if even that fails.
      if (!view) view = SampledView(defaults_[1][uint32_t(want)], TextureView{}, want);
      if (!view) return VK_NULL_HANDLE;
      ii[i] = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    }
    VkDescriptorBufferInfo bi[3] = {{guest_.buffer, 0, VK_WHOLE_SIZE}, {ring_.buffer, 0, VK_WHOLE_SIZE}, {guest_.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet wr[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, kViewBindings, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ii, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 96, 0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, bi, nullptr}};
    vk_.vkUpdateDescriptorSets(device_, 2, wr, 0, nullptr);
    view_sets_[h] = {set, pool_index_};
    ++stats_.texture_table_misses;
    return set;
  }
  static ViewDimension ViewDimensionOf(const Texture& t) {
    return t.desc.dimension == TextureDimension::kCube ? ViewDimension::kCube
           : t.desc.dimension == TextureDimension::k3D ? ViewDimension::k3D : ViewDimension::k2D;
  }
  VkDescriptorSet SamplerSet() {
    uint64_t h = 0xCBF29CE484222325ull;
    VkSampler smp[32];
    for (uint32_t i = 0; i < 32; ++i) {
      // Patch 0080: the slot's sampler object from its last lookup while its description is unchanged.
      if (!sampler_memo_[i] || !(sampler_memo_desc_[i] == samplers_[i])) {
        sampler_memo_[i] = SamplerFor(samplers_[i]);
        sampler_memo_desc_[i] = samplers_[i];
      }
      smp[i] = sampler_memo_[i];
      if (!smp[i]) return VK_NULL_HANDLE;  // outside review: no sampler - skip the draw, never write a null descriptor
      h = (h ^ uint64_t(reinterpret_cast<uintptr_t>(smp[i]))) * 0x100000001B3ull;
    }
    auto it = sampler_sets_.find(h);
    if (it != sampler_sets_.end()) return UseCached(it->second);
    VkDescriptorSet set = AllocateSet(2);
    if (!set) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[32];
    for (uint32_t i = 0; i < 32; ++i) ii[i] = {smp[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 32, VK_DESCRIPTOR_TYPE_SAMPLER, ii, nullptr, nullptr};
    vk_.vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);
    sampler_sets_[h] = {set, pool_index_};
    ++stats_.sampler_tables_written;
    return set;
  }

  // Everything a draw needs recorded before vkCmdDraw*: false = skip the draw.
  bool PrepareDraw() {
    if (!ready_ || lost_ || !current_pipeline_ || !Record()) return false;
    Pipeline& p = *pipelines_[current_pipeline_];
    if (!p.ready.load(std::memory_order_acquire) || p.failed.load() || !p.pipeline) return false;
    if (!targets_.count && !targets_.depth) return false;
    // Textures read by this draw: in GENERAL, and not written in this pass without a barrier since.
    bool split = false;
    for (uint32_t k = 0; k < bound_view_count_ && !split; ++k) {  // patch 0080: only the slots holding a texture
      const TextureHandle th = views_[bound_views_[k]].texture;
      if (!Valid(th)) continue;
      if (textures_[th].layout != VK_IMAGE_LAYOUT_GENERAL) split = true;
      for (TextureHandle w : pass_written_) if (w == th) split = true;
    }
    if (split) {
      EndPass(6);
      for (uint32_t k = 0; k < bound_view_count_; ++k) {
        const TextureHandle th = views_[bound_views_[k]].texture;
        if (Valid(th)) Transition(textures_[th], VK_IMAGE_LAYOUT_GENERAL);
      }
      FullBarrier();
    }
    // Patch 0089 (V5b): what this draw samples, for the next pass's barrier decision; a memory-export draw orders
    // the next pass after it.
    for (uint32_t k = 0; k < bound_view_count_; ++k)
      if (const TextureHandle th = views_[bound_views_[k]].texture; Valid(th)) textures_[th].read_epoch = barrier_epoch_;
    if (guest_writable_) outside_dirty_ = true;
    // Descriptor sets before the pass (allocation may switch pools; no commands involved).
    if (dirty_views_ || !cur_view_set_) { cur_view_set_ = ViewSet(); dirty_views_ = false; dirty_sets_ = true; }
    if (dirty_samplers_ || !cur_sampler_set_) { cur_sampler_set_ = SamplerSet(); dirty_samplers_ = false; dirty_sets_ = true; }
    if (!cur_view_set_ || !cur_sampler_set_) return false;
    if (!BeginPass()) return false;
    if (bound_pipeline_ != p.pipeline) { vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline); bound_pipeline_ = p.pipeline; dirty_sets_ = true; }
    if (dirty_viewport_) { vk_.vkCmdSetViewport(cmd_, 0, 1, &viewport_); dirty_viewport_ = false; }
    if (dirty_scissor_) { vk_.vkCmdSetScissor(cmd_, 0, 1, &scissor_); dirty_scissor_ = false; }
    if (dirty_stencil_) { vk_.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref_); dirty_stencil_ = false; }
    if (dirty_blend_) { vk_.vkCmdSetBlendConstants(cmd_, blend_); dirty_blend_ = false; }
    if (dirty_sets_ || dirty_cb_) {
      const VkDescriptorSet sets[3] = {cb_set_, cur_view_set_, cur_sampler_set_};
      vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, sets, 2 * kCbCount, cb_offset_);
      dirty_sets_ = dirty_cb_ = false;
    }
    OccTrack();  // patch 0098
    return true;
  }
  // Uniform data: aligned for dynamic offsets, and a whole kCbRange window must fit inside the ring.
  bool RingAllocUniform(uint32_t size, uint64_t& at) {
    const uint64_t align = std::max<uint64_t>(16, limits_.minUniformBufferOffsetAlignment);
    if (!RingAlloc(size, align, at)) return false;
    if (at + kCbRange > ring_.size) return RingAlloc(size, align, at);  // too close to the end: take the next lap
    return true;
  }

  // ---- stage V4: compute ----
  // Compute bindings, all in set 0: 0 = b0 (constants), 1-4 = t0-t3, 5 = u0, 6 = u1. The descriptor type of each comes
  // from the SPIR-V (t3 / u0 are images in some of our shaders and raw buffers in others).
  bool ComputeLayout(const std::vector<uint32_t>& w, Shader& out) {
    for (auto& t : out.types) t = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    std::unordered_map<uint32_t, uint32_t> binding_of, set_of, image_sampled, pointer_to, pointer_class;
    std::unordered_map<uint32_t, bool> is_sampler, is_struct;
    struct Var { uint32_t id, type, cls; };
    std::vector<Var> vars;
    for (size_t i = 5; i < w.size();) {
      const uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
      if (!n || i + n > w.size()) break;
      if (op == 71 && n >= 4 && w[i + 2] == 33) binding_of[w[i + 1]] = w[i + 3];
      if (op == 71 && n >= 4 && w[i + 2] == 34) set_of[w[i + 1]] = w[i + 3];
      if (op == 25 && n >= 9) image_sampled[w[i + 1]] = w[i + 7];  // OpTypeImage: Sampled 1 = sampled, 2 = storage
      if (op == 26) is_sampler[w[i + 1]] = true;
      if (op == 30) is_struct[w[i + 1]] = true;
      if (op == 32 && n >= 4) { pointer_class[w[i + 1]] = w[i + 2]; pointer_to[w[i + 1]] = w[i + 3]; }
      if (op == 59 && n >= 4) vars.push_back({w[i + 2], w[i + 1], w[i + 3]});
      i += n;
    }
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (const Var& v : vars) {
      if (!binding_of.count(v.id)) continue;
      const uint32_t b = binding_of[v.id];
      if (b >= 8 || set_of[v.id] != 0) { REXLOG_ERROR("gow2_renderer: Vulkan: compute binding {} set {} outside the layout", b, set_of[v.id]); return false; }
      const uint32_t t = pointer_to[v.type];
      VkDescriptorType ty;
      if (v.cls == 2) ty = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;                // Uniform
      else if (v.cls == 12) ty = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;          // StorageBuffer
      else if (image_sampled.count(t)) ty = image_sampled[t] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      else if (is_sampler.count(t)) ty = VK_DESCRIPTOR_TYPE_SAMPLER;
      else continue;
      out.types[b] = ty;
      lb.push_back({b, ty, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = uint32_t(lb.size());
    li.pBindings = lb.data();
    if (vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &out.set_layout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &out.set_layout;
    return vk_.vkCreatePipelineLayout(device_, &pi, nullptr, &out.layout) == VK_SUCCESS;
  }
  VkDescriptorSet AllocateComputeSet(VkDescriptorSetLayout layout) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (compute_pool_index_ < compute_pools_.size()) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = compute_pools_[compute_pool_index_].pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vk_.vkAllocateDescriptorSets(device_, &ai, &set) == VK_SUCCESS) {
          compute_pools_[compute_pool_index_].fence = UINT64_MAX;
          return set;
        }
      }
      const uint64_t done = CompletedFence();
      compute_pool_index_ = UINT32_MAX;
      for (uint32_t i = 0; i < compute_pools_.size(); ++i)
        if (compute_pools_[i].fence <= done) { vk_.vkResetDescriptorPool(device_, compute_pools_[i].pool, 0); compute_pools_[i].fence = 0; compute_pool_index_ = i; break; }
      if (compute_pool_index_ == UINT32_MAX) {
        VkDescriptorPoolSize sz[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1024;
        pi.poolSizeCount = 4;
        pi.pPoolSizes = sz;
        DescriptorPool dp;
        if (vk_.vkCreateDescriptorPool(device_, &pi, nullptr, &dp.pool) != VK_SUCCESS) { Fail("compute descriptor pool"); return VK_NULL_HANDLE; }
        compute_pools_.push_back(dp);
        compute_pool_index_ = uint32_t(compute_pools_.size() - 1);
      }
    }
    return VK_NULL_HANDLE;
  }
  VkImageView StorageView(TextureHandle h, uint32_t mip) {
    Texture& t = textures_[h];
    const uint64_t key = (1ull << 40) | uint64_t(mip);
    for (auto& [k, view] : t.views) if (k == key) return view;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = t.desc.dimension == TextureDimension::k3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, std::min(mip, std::max(1u, t.desc.mips) - 1), 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vk_.vkCreateImageView(device_, &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    t.views.push_back({key, view});
    return view;
  }

  // ---- device setup ----
  bool PickDevice(std::string& error) {
    if (ext_) {
      physical_ = static_cast<VkPhysicalDevice>(ext_->physical_device);
      VkPhysicalDeviceProperties p;
      vk_.vkGetPhysicalDeviceProperties(physical_, &p);
      if (p.apiVersion < VK_API_VERSION_1_3) { error = "the external Vulkan device is not 1.3"; return false; }
      return DescribeDevice(error);
    }
    uint32_t n = 0;
    vk_.vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vk_.vkEnumeratePhysicalDevices(instance_, &n, devs.data());
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; ++i) {
      VkPhysicalDeviceProperties p;
      vk_.vkGetPhysicalDeviceProperties(devs[i], &p);
      if (p.apiVersion < VK_API_VERSION_1_3) continue;
      const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 4
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 2
                                                                                 : 1;  // CPU (lavapipe) last
      if (score > best_score) { best = int(i); best_score = score; }
    }
    if (best < 0) { error = "no Vulkan 1.3 device"; return false; }
    physical_ = devs[size_t(best)];
    return DescribeDevice(error);
  }
  bool DescribeDevice(std::string& error) {
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &p12;
    vk_.vkGetPhysicalDeviceProperties2(physical_, &p2);
    device_name_ = p2.properties.deviceName;
    driver_name_ = p12.driverName[0] ? std::string(p12.driverName) + " " + p12.driverInfo : std::string("unknown");
    device_api_ = p2.properties.apiVersion;
    limits_ = p2.properties.limits;
    vk_.vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    for (uint32_t i = 0; i < memory_.memoryHeapCount; ++i)
      if (memory_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        device_local_mb_ = std::max<uint64_t>(device_local_mb_, memory_.memoryHeaps[i].size >> 20);
    uint32_t qn = 0;
    vk_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vk_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qf.data());
    queue_family_ = UINT32_MAX;
    for (uint32_t i = 0; i < qn; ++i)
      if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { queue_family_ = i; break; }
    if (ext_) queue_family_ = ext_->queue_family < qn ? ext_->queue_family : UINT32_MAX;
    if (queue_family_ == UINT32_MAX) { error = "no graphics + compute queue"; return false; }
    VkFormatProperties fp;
    vk_.vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    d24s8_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    return true;
  }
  bool CreateDevice(std::string& error) {
    if (ext_) {  // the owner created it (SDK patch 33 enables what we use); we may use only what it enabled
      device_ = static_cast<VkDevice>(ext_->device);
      push_descriptor_ = ext_->push_descriptor;
      depth_clamp_ = ext_->depth_clamp;
      fill_mode_non_solid_ = ext_->fill_mode_non_solid;
      anisotropy_ = ext_->anisotropy;
      if (!ext_->texture_compression_bc) REXLOG_WARN("gow2_renderer: Vulkan: BC texture formats not enabled on the SDK's device");
      return FinishDevice(error);
    }
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f12.pNext = &f13;
    vk_.vkGetPhysicalDeviceFeatures2(physical_, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) {
      error = "the device lacks dynamic rendering / synchronization2 / timeline semaphores";
      return false;
    }
    // Vulkan 1.4 features where the device is 1.4.
    VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    if (device_api_ >= VK_API_VERSION_1_4) {
      f13.pNext = &f14;
      vk_.vkGetPhysicalDeviceFeatures2(physical_, &f2);
    }
    // What we use, where present (the draw stages check the ones they need).
    VkPhysicalDeviceVulkan14Features e14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    e14.pushDescriptor = f14.pushDescriptor;
    e14.maintenance5 = f14.maintenance5;
    e14.maintenance6 = f14.maintenance6;
    e14.dynamicRenderingLocalRead = f14.dynamicRenderingLocalRead;
    push_descriptor_ = f14.pushDescriptor == VK_TRUE;
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    if (device_api_ >= VK_API_VERSION_1_4) e13.pNext = &e14;
    e13.dynamicRendering = VK_TRUE;
    e13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.timelineSemaphore = VK_TRUE;
    e12.pNext = &e13;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    e2.pNext = &e12;
    const VkPhysicalDeviceFeatures& have = f2.features;
    VkPhysicalDeviceFeatures& want = e2.features;
    want.geometryShader = have.geometryShader;
    want.independentBlend = have.independentBlend;
    want.depthClamp = have.depthClamp;
    want.depthBiasClamp = have.depthBiasClamp;
    want.fillModeNonSolid = have.fillModeNonSolid;
    want.samplerAnisotropy = have.samplerAnisotropy;
    want.textureCompressionBC = have.textureCompressionBC;
    want.fragmentStoresAndAtomics = have.fragmentStoresAndAtomics;
    want.vertexPipelineStoresAndAtomics = have.vertexPipelineStoresAndAtomics;
    want.shaderImageGatherExtended = have.shaderImageGatherExtended;
    want.shaderStorageImageWriteWithoutFormat = have.shaderStorageImageWriteWithoutFormat;
    want.occlusionQueryPrecise = have.occlusionQueryPrecise;
    if (options_.vk_robust_buffer_access) want.robustBufferAccess = have.robustBufferAccess;  // patch 0088 (bench)
    depth_clamp_ = have.depthClamp;
    fill_mode_non_solid_ = have.fillModeNonSolid;
    anisotropy_ = have.samplerAnisotropy;
    if (!have.textureCompressionBC) REXLOG_WARN("gow2_renderer: Vulkan: the device has no BC texture formats");
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = queue_family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &e2;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    // Patch 0104: the driver's per-pipeline statistics (a measurement, own device only).
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    const char* dev_exts[1] = {};
    if (options_.vk_pipeline_stats) {
      uint32_t n = 0;
      vk_.vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
      std::vector<VkExtensionProperties> xs(n);
      vk_.vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, xs.data());
      for (const auto& x : xs)
        if (!std::strcmp(x.extensionName, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME)) pipeline_stats_ = true;
      if (pipeline_stats_) {
        pe.pipelineExecutableInfo = VK_TRUE;
        pe.pNext = e2.pNext;
        e2.pNext = &pe;
        dev_exts[0] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = dev_exts;
      } else {
        REXLOG_WARN("gow2_renderer: Vulkan: VK_KHR_pipeline_executable_properties not offered - no pipeline statistics");
      }
    }
    VkResult r = vk_.vkCreateDevice(physical_, &di, nullptr, &device_);
    if (r != VK_SUCCESS) { error = VkErr("vkCreateDevice", r); return false; }
    return FinishDevice(error);
  }
  bool FinishDevice(std::string& error) {
#define GN_VK_LOAD_D(f) vk_.f = reinterpret_cast<PFN_##f>(vk_.vkGetDeviceProcAddr(device_, #f));
    GN_VK_DEVICE_FUNCS(GN_VK_LOAD_D)
    if (pipeline_stats_) {  // patch 0104
      get_exec_props_ = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
          vk_.vkGetDeviceProcAddr(device_, "vkGetPipelineExecutablePropertiesKHR"));
      get_exec_stats_ = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
          vk_.vkGetDeviceProcAddr(device_, "vkGetPipelineExecutableStatisticsKHR"));
      if (!get_exec_props_ || !get_exec_stats_) pipeline_stats_ = false;
    }
    if (debug_utils_) { GN_VK_DEBUG_FUNCS(GN_VK_LOAD_D) }
#undef GN_VK_LOAD_D
    if (ext_) queue_ = static_cast<VkQueue>(ext_->queue);
    else vk_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    VkSemaphoreTypeCreateInfo st{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &st;
    VkResult r = vk_.vkCreateSemaphore(device_, &sci, nullptr, &timeline_);
    if (r != VK_SUCCESS) { error = VkErr("vkCreateSemaphore", r); return false; }
    return true;
  }
  bool CreateBuffers(std::string& error) {
    const uint64_t ring = uint64_t(std::max(16u, options_.upload_ring_mb)) << 20;
    if (!MakeBuffer(ring, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ring_, true,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      error = "upload ring";
      return false;
    }
    if (!MakeBuffer(options_.guest_memory_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, guest_, false, 0)) {
      error = "guest memory buffer";
      return false;
    }
    if (Record()) vk_.vkCmdFillBuffer(C(), guest_.buffer, 0, VK_WHOLE_SIZE, 0);  // defined contents from the start
    guest_written_ = true;
    return true;
  }
  void Shutdown() {
    if (device_) {
      if (cmd_) Submit();
      {
        std::lock_guard<std::mutex> lock(job_mutex_);
        stop_workers_ = true;
        jobs_.clear();
        backlog_jobs_.clear();
      }
      job_cv_.notify_all();
      for (auto& t : workers_) t.join();
      workers_.clear();
      for (int i = 0; i < 500 && cache_saving_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      MaybeSaveCache(true);  // patch 0079
      if (ext_) {  // the device isn't ours: wait for our own work only (vkDeviceWaitIdle would need every queue locked)
        const VkSemaphore sem = timeline_;
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &sem;
        wi.pValues = &submitted_;
        if (timeline_ && submitted_) vk_.vkWaitSemaphores(device_, &wi, 5'000'000'000ull);
      } else {
        vk_.vkDeviceWaitIdle(device_);
      }
      if (present_pipeline_) vk_.vkDestroyPipeline(device_, present_pipeline_, nullptr);
      if (ts_pool_) vk_.vkDestroyQueryPool(device_, ts_pool_, nullptr);
      if (occ_pool_) vk_.vkDestroyQueryPool(device_, occ_pool_, nullptr);  // patch 0098
      if (present_layout_) vk_.vkDestroyPipelineLayout(device_, present_layout_, nullptr);
      if (present_set_layout_) vk_.vkDestroyDescriptorSetLayout(device_, present_set_layout_, nullptr);
      if (present_pool_) vk_.vkDestroyDescriptorPool(device_, present_pool_, nullptr);
      if (present_view_) vk_.vkDestroyImageView(device_, present_view_, nullptr);
      for (auto& p : pipelines_) if (p && p->pipeline) vk_.vkDestroyPipeline(device_, p->pipeline, nullptr);
      for (auto& sh : shaders_) if (sh.module) vk_.vkDestroyShaderModule(device_, sh.module, nullptr);
      for (auto& [k, smp] : sampler_objects_) vk_.vkDestroySampler(device_, smp, nullptr);
      for (auto& dp : pools_) vk_.vkDestroyDescriptorPool(device_, dp.pool, nullptr);
      for (auto& dp : compute_pools_) vk_.vkDestroyDescriptorPool(device_, dp.pool, nullptr);
      for (auto& sh : shaders_) {
        if (sh.layout) vk_.vkDestroyPipelineLayout(device_, sh.layout, nullptr);
        if (sh.set_layout) vk_.vkDestroyDescriptorSetLayout(device_, sh.set_layout, nullptr);
      }
      Free(scratch_);
      if (cb_pool_) vk_.vkDestroyDescriptorPool(device_, cb_pool_, nullptr);
      if (layout_) vk_.vkDestroyPipelineLayout(device_, layout_, nullptr);
      for (auto& l : set_layouts_) if (l) vk_.vkDestroyDescriptorSetLayout(device_, l, nullptr);
      if (pipeline_cache_vk_) vk_.vkDestroyPipelineCache(device_, pipeline_cache_vk_, nullptr);
      for (auto& ib : index_buffers_) if (ib.live) Free(ib.buffer);
      for (auto& rb : readbacks_) if (rb.live) { if (rb.buffer.mapped) vk_.vkUnmapMemory(device_, rb.buffer.memory); Free(rb.buffer); }
      for (auto& t : textures_)
        if (t.live) {
          if (t.attachment_view) vk_.vkDestroyImageView(device_, t.attachment_view, nullptr);
          if (t.bits_view) vk_.vkDestroyImageView(device_, t.bits_view, nullptr);
          for (auto& [k, v] : t.views) vk_.vkDestroyImageView(device_, v, nullptr);
          vk_.vkDestroyImage(device_, t.image, nullptr);
          vk_.vkFreeMemory(device_, t.memory, nullptr);
        }
      for (auto& d : deferred_) Release(d);
      deferred_.clear();
      if (ring_.mapped) vk_.vkUnmapMemory(device_, ring_.memory);
      Free(ring_);
      Free(guest_);
      for (auto& s : slots_) vk_.vkDestroyCommandPool(device_, s.pool, nullptr);
      slots_.clear();
      if (timeline_) vk_.vkDestroySemaphore(device_, timeline_, nullptr);
      if (!ext_) vk_.vkDestroyDevice(device_, nullptr);
      device_ = VK_NULL_HANDLE;
    }
    if (instance_ && !ext_) vk_.vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    ready_ = false;
  }

  // ---- memory ----
  // Patch 0097: device-local memory for an image; with --gow2_renderer_vulkan_dedicated_images (BackendOptions::
  // vk_dedicated_images) in an allocation of its own when the driver prefers or requires that (D3D12's committed
  // resources; NVIDIA prefers it for render targets).
  VkDeviceMemory AllocateImage(VkImage image) {
    ++stats_.vk_images;
    VkMemoryDedicatedRequirements dr{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 mr2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr};
    VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    ri.image = image;
    if (!options_.vk_dedicated_images || !vk_.vkGetImageMemoryRequirements2) {
      VkMemoryRequirements mr;
      vk_.vkGetImageMemoryRequirements(device_, image, &mr);
      return Allocate(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    }
    vk_.vkGetImageMemoryRequirements2(device_, &ri, &mr2);
    VkMemoryDedicatedAllocateInfo da{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    da.image = image;
    const bool dedicated = dr.prefersDedicatedAllocation || dr.requiresDedicatedAllocation;
    if (dedicated) ++stats_.vk_dedicated_images;
    return Allocate(mr2.memoryRequirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, dedicated ? &da : nullptr);
  }
  VkDeviceMemory Allocate(const VkMemoryRequirements& mr, VkMemoryPropertyFlags need, VkMemoryPropertyFlags prefer,
                          const void* next = nullptr) {
    for (int pass = 0; pass < 2; ++pass) {
      const VkMemoryPropertyFlags want = pass == 0 ? (need | prefer) : need;
      for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
        if (!(mr.memoryTypeBits & (1u << i)) || (memory_.memoryTypes[i].propertyFlags & want) != want) continue;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, next};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = i;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (vk_.vkAllocateMemory(device_, &ai, nullptr, &m) == VK_SUCCESS) return m;
      }
      if (!prefer) break;
    }
    return VK_NULL_HANDLE;
  }
  bool MakeBuffer(uint64_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags need, Buffer& out, bool map,
                  VkMemoryPropertyFlags prefer) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    if (vk_.vkCreateBuffer(device_, &bi, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vk_.vkGetBufferMemoryRequirements(device_, out.buffer, &mr);
    out.memory = Allocate(mr, need, prefer);
    if (!out.memory) { vk_.vkDestroyBuffer(device_, out.buffer, nullptr); out = {}; return false; }
    if (vk_.vkBindBufferMemory(device_, out.buffer, out.memory, 0) != VK_SUCCESS) {  // outside review
      vk_.vkDestroyBuffer(device_, out.buffer, nullptr);
      vk_.vkFreeMemory(device_, out.memory, nullptr);
      out = {};
      return false;
    }
    out.size = size;
    if (map) {
      void* p = nullptr;
      if (vk_.vkMapMemory(device_, out.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) { Free(out); return false; }
      out.mapped = static_cast<uint8_t*>(p);
    }
    return true;
  }
  void Free(Buffer& b) {
    if (b.buffer) vk_.vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vk_.vkFreeMemory(device_, b.memory, nullptr);
    b = {};
  }
  void Defer(VkImage image, VkImageView view, VkBuffer buffer, VkDeviceMemory memory) {
    // Freed once every submission so far (and the one being recorded) has finished.
    deferred_.push_back({submitted_ + (cmd_ ? 1 : 0), image, view, buffer, memory});
  }
  void Release(const Deferred& d) {
    if (d.view) vk_.vkDestroyImageView(device_, d.view, nullptr);
    if (d.image) vk_.vkDestroyImage(device_, d.image, nullptr);
    if (d.buffer) vk_.vkDestroyBuffer(device_, d.buffer, nullptr);
    if (d.memory) vk_.vkFreeMemory(device_, d.memory, nullptr);
  }
  void MaybeSaveCache(bool now) {
    if (cache_path_.empty() || !pipeline_cache_vk_ || cache_saving_.load()) return;
    const uint64_t built = built_async_.load() + stats_.pipelines;
    if (built == cache_saved_built_) return;
    const auto t = std::chrono::steady_clock::now();
    if (!now && options_.pipeline_save_seconds > 0 &&
        t - cache_saved_at_ < std::chrono::seconds(options_.pipeline_save_seconds))
      return;
    if (!now && options_.pipeline_save_seconds <= 0) return;
    size_t n = 0;
    if (vk_.vkGetPipelineCacheData(device_, pipeline_cache_vk_, &n, nullptr) != VK_SUCCESS || !n) return;
    auto data = std::make_shared<std::vector<char>>(n);
    if (vk_.vkGetPipelineCacheData(device_, pipeline_cache_vk_, &n, data->data()) != VK_SUCCESS) return;
    data->resize(n);
    cache_saved_at_ = t;
    cache_saved_built_ = built;
    ++stats_.pipeline_library_saves;
    const std::string path = cache_path_;
    auto write = [path, data]() {
      const std::string tmp = path + ".tmp";
      {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(data->data(), std::streamsize(data->size()));
        if (!f) return;
      }
      std::error_code ec;
      std::filesystem::rename(tmp, path, ec);
    };
    if (now) {
      write();
      return;
    }
    cache_saving_.store(true);
    std::thread([this, write]() {
      write();
      cache_saving_.store(false);
    }).detach();
  }
  void Retire() {
    const uint64_t done = CompletedFence();
    TsRetire(done);
    while (!deferred_.empty() && deferred_.front().fence <= done) { Release(deferred_.front()); deferred_.pop_front(); }
    while (!ring_marks_.empty() && ring_marks_.front().fence <= done) { ring_freed_ = ring_marks_.front().allocated; ring_marks_.pop_front(); }
  }

  // ---- the upload ring: allocations move a head forward and wrap; bytes are counted (padding included) so the ring
  // knows how much is in use: allocated_ - freed_, where a submission's completion frees everything allocated before
  // it was sent (RingMark.allocated). ----
  bool RingAlloc(uint64_t size, uint64_t align, uint64_t& at) {
    if (size > ring_.size / 2) { Fail(std::format("an upload of {} MB does not fit the {} MB ring", size >> 20, ring_.size >> 20)); return false; }
    for (;;) {
      uint64_t start = (ring_head_ + align - 1) / align * align;
      uint64_t need = start - ring_head_ + size;
      if (start + size > ring_.size) {  // wrap: the rest of the ring is padding (and the zero block at its start)
        start = kRingReserved;
        need = ring_.size - ring_head_ + kRingReserved + size;
      }
      if (ring_allocated_ - ring_freed_ + need <= ring_.size) {
        at = start;
        ring_head_ = start + size;
        ring_allocated_ += need;
        return true;
      }
      // Full: wait for the oldest submission holding ring space (send the current one first if only it holds it).
      ++stats_.ring_waits;
      if (ring_marks_.empty()) {
        Submit();
        if (ring_marks_.empty() || lost_) { Fail("upload ring: no space"); return false; }
      }
      WaitForFence(ring_marks_.front().fence);
      Retire();
      if (lost_ || !Record()) return false;
    }
  }

  // ---- recording ----
  bool Record() {
    if (!ready_ && !device_) return false;
    if (lost_) return false;
    if (cmd_) {
      TsCat();  // patch 0077: a timestamp when GPU work of a new category starts
      return true;
    }
    // A free slot (its last submission finished), or a new one.
    const uint64_t done = CompletedFence();
    slot_ = UINT32_MAX;
    for (uint32_t i = 0; i < slots_.size(); ++i)
      if (slots_[i].fence <= done) { slot_ = i; break; }
    if (slot_ == UINT32_MAX) {
      if (slots_.size() >= 8) {  // bounded: wait for the oldest
        uint64_t oldest = UINT64_MAX;
        for (auto& s : slots_) oldest = std::min(oldest, s.fence);
        WaitForFence(oldest);
        return Record();
      }
      CommandSlot s;
      VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      pi.queueFamilyIndex = queue_family_;
      if (vk_.vkCreateCommandPool(device_, &pi, nullptr, &s.pool) != VK_SUCCESS) { Fail("vkCreateCommandPool"); return false; }
      VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ai.commandPool = s.pool;
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      if (vk_.vkAllocateCommandBuffers(device_, &ai, &s.cmd) != VK_SUCCESS) { Fail("vkAllocateCommandBuffers"); return false; }
      slots_.push_back(s);
      slot_ = uint32_t(slots_.size() - 1);
    }
    CommandSlot& s = slots_[slot_];
    vk_.vkResetCommandPool(device_, s.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk_.vkBeginCommandBuffer(s.cmd, &bi) != VK_SUCCESS) { Fail("vkBeginCommandBuffer"); return false; }
    s.fence = UINT64_MAX;  // in use until submitted
    pending_img_.clear();  // patch 0082: nothing queued carries over from a list that was never sent
    pending_mem_ = false;
    pending_att_ = false;  // patch 0115
    work_stages_ = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;  // patch 0107: work of earlier submissions may still run
    cmd_ = s.cmd;
    in_pass_ = false;
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_index_ = VK_NULL_HANDLE;
    dirty_sets_ = dirty_cb_ = dirty_viewport_ = dirty_scissor_ = dirty_stencil_ = dirty_blend_ = true;
    for (auto& sh : cb_shadow_) sh.valid = false;  // patch 0080: the ring copies may belong to a finished submission
    dirty_views_ = dirty_samplers_ = true;         // and the sets are looked up again, which marks their pools in use
    TsBegin();
    TsCat();
    return true;
  }

  // ---- GPU timestamps (patch 0077, as D3D12's jobs 176-177): a timestamp when GPU work of a new category starts, one
  // at the end of the list, summed by category when the list's submission completes (Retire). Each list owns a block
  // of kTsPerList queries, reset when the list begins (outside any rendering). ----
  static constexpr uint32_t kTsPerList = 4096, kTsLists = 32;  // patch 0101: 256 -> 4096 (a timestamp per draw)
  struct TsList {
    uint64_t fence = 0;
    uint32_t block = 0, count = 0;
    std::vector<uint8_t> cats;
    std::vector<uint32_t> tags;     // patch 0101: the SetGpuTag value of each stretch
    std::vector<uint8_t> occ_cats;  // patch 0098: the target slot of each occlusion query in this list's block
    std::vector<uint32_t> occ_tags;  // patch 0102: the SetGpuTag value of each occlusion query (0 = none)
    bool occ_reset = false;          // patch 0102: this list's block of occlusion queries was reset
  };
  // Patch 0098 (--gow2_renderer_vulkan_count_pixels, a measurement): samples that pass the depth / stencil tests per
  // render-target slot, from precise occlusion queries around each run of draws to one target slot inside a pass
  // (the game's own occlusion queries are not used on Vulkan, so nothing else is active).
  bool EnsureOcc() {
    if (occ_pool_) return true;
    if (occ_failed_ || !EnsureTimestamps()) return false;
    occ_failed_ = true;
    VkPhysicalDeviceFeatures f{};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vk_.vkGetPhysicalDeviceFeatures2(physical_, &f2);
    f = f2.features;
    occ_precise_ = f.occlusionQueryPrecise == VK_TRUE;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = kTsPerList * kTsLists;
    if (vk_.vkCreateQueryPool(device_, &qi, nullptr, &occ_pool_) != VK_SUCCESS) { occ_pool_ = VK_NULL_HANDLE; return false; }
    occ_failed_ = false;
    REXLOG_INFO("gow2_renderer: Vulkan: counting passed samples per render target (patch 0098, {} occlusion queries)",
                occ_precise_ ? "precise" : "non-precise - counts may be 0 / 1 only");
    return true;
  }
  void OccTrack() {
    if (!ts_open_ || !in_pass_ || !occ_pool_ || !ts_cur_.occ_reset) return;
    const uint8_t c = gpu_cat_pending_;
    const uint32_t tag = tag_samples_ ? gpu_tag_pending_ : 0;  // patch 0102: one query per tagged draw
    if (occ_active_ && occ_cat_ == c && occ_tag_ == tag) return;
    if (occ_active_) OccEnd();
    if (!tag && (!options_.vk_count_pixels || c < 0x20 || c >= 0x40)) return;
    if (ts_cur_.occ_cats.size() >= kTsPerList) return;
    vk_.vkCmdBeginQuery(cmd_, occ_pool_, ts_cur_.block * kTsPerList + uint32_t(ts_cur_.occ_cats.size()),
                        occ_precise_ ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    ts_cur_.occ_cats.push_back(c);
    ts_cur_.occ_tags.push_back(tag);
    occ_active_ = true;
    occ_cat_ = c;
    occ_tag_ = tag;
  }
  void OccEnd() {
    vk_.vkCmdEndQuery(cmd_, occ_pool_, ts_cur_.block * kTsPerList + uint32_t(ts_cur_.occ_cats.size()) - 1);
    occ_active_ = false;
  }
  void SetGpuCategory(uint8_t category) override { gpu_cat_pending_ = category; }
  void SetGpuTag(uint32_t tag) override { gpu_tag_pending_ = tag; }  // patch 0101
  void SetGpuTagSamples(bool on) override { tag_samples_ = on; }    // patch 0102
  void TakeGpuTagTimes(std::vector<GpuTagTime>& out) override {
    out.swap(tag_times_);
    tag_times_.clear();
  }
  std::string PipelineStatistics(PipelineHandle h) override {  // patch 0104
    if (!pipeline_stats_ || !h || h >= pipelines_.size() || !pipelines_[h]) return {};
    Pipeline& p = *pipelines_[h];
    if (!p.ready.load(std::memory_order_acquire) || !p.pipeline) return {};
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = p.pipeline;
    uint32_t n = 0;
    if (get_exec_props_(device_, &pi, &n, nullptr) != VK_SUCCESS) return "properties failed";
    std::vector<VkPipelineExecutablePropertiesKHR> props(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    get_exec_props_(device_, &pi, &n, props.data());
    std::string out;
    for (uint32_t e = 0; e < n; ++e) {
      VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
      ei.pipeline = p.pipeline;
      ei.executableIndex = e;
      uint32_t m = 0;
      get_exec_stats_(device_, &ei, &m, nullptr);
      std::vector<VkPipelineExecutableStatisticKHR> st(m, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
      get_exec_stats_(device_, &ei, &m, st.data());
      if (!out.empty()) out += " | ";
      out += props[e].name;
      out += ":";
      for (const auto& x : st) {
        char b[160];
        switch (x.format) {
          case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: std::snprintf(b, sizeof b, " %s %u,", x.name, unsigned(x.value.b32)); break;
          case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: std::snprintf(b, sizeof b, " %s %lld,", x.name, (long long)x.value.i64); break;
          case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: std::snprintf(b, sizeof b, " %s %llu,", x.name, (unsigned long long)x.value.u64); break;
          default: std::snprintf(b, sizeof b, " %s %.3f,", x.name, x.value.f64); break;
        }
        out += b;
      }
    }
    return out;
  }
  bool TextureInfo(TextureHandle h, TextureDesc& out) const override {  // patch 0124
    if (!ready_ || !h || h >= textures_.size() || !textures_[h].live) return false;
    out = textures_[h].desc;
    return true;
  }
  std::string DescribeTexture(TextureHandle h) const override {  // patch 0101
    if (!ready_ || !h || h >= textures_.size() || !textures_[h].live) return "none";
    const Texture& t = textures_[h];
    char b[256];
    std::snprintf(b, sizeof b, "%ux%ux%u m%u s%u VkFormat %d usage 0x%X flags 0x%X%s views %u layout %d", t.desc.width,
                  t.desc.height, t.desc.depth_or_layers, t.desc.mips, t.desc.samples, int(t.format), t.vk_usage,
                  t.vk_flags, (t.vk_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) ? " (mutable)" : "", t.vk_view_formats,
                  int(t.layout));
    return b;
  }
  bool EnsureTimestamps() {
    if (ts_pool_) return true;
    if (ts_failed_) return false;
    ts_failed_ = true;  // until it works
    uint32_t qn = 0;
    vk_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vk_.vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qf.data());
    if (queue_family_ >= qn || !qf[queue_family_].timestampValidBits || limits_.timestampPeriod <= 0.0f) {
      REXLOG_WARN("gow2_renderer: Vulkan: GPU timestamps unavailable on this queue");
      return false;
    }
    const uint32_t bits = qf[queue_family_].timestampValidBits;
    ts_mask_ = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = kTsPerList * kTsLists;
    if (vk_.vkCreateQueryPool(device_, &qi, nullptr, &ts_pool_) != VK_SUCCESS) { ts_pool_ = VK_NULL_HANDLE; return false; }
    ts_block_busy_.assign(kTsLists, false);
    ts_failed_ = false;
    return true;
  }
  void TsBegin() {
    ts_open_ = false;
    gpu_cat_emitted_ = 0xFF;
    if (!EnsureTimestamps() || ts_block_busy_[ts_next_block_]) return;  // every block waits for read-back: untimed
    ts_cur_ = TsList{};
    ts_cur_.block = ts_next_block_;
    ts_block_busy_[ts_cur_.block] = true;
    ts_next_block_ = (ts_next_block_ + 1) % kTsLists;
    vk_.vkCmdResetQueryPool(cmd_, ts_pool_, ts_cur_.block * kTsPerList, kTsPerList);
    if ((options_.vk_count_pixels || tag_samples_) && EnsureOcc()) {
      vk_.vkCmdResetQueryPool(cmd_, occ_pool_, ts_cur_.block * kTsPerList, kTsPerList);
      ts_cur_.occ_reset = true;  // patch 0102
    }
    occ_active_ = false;
    ts_open_ = true;
  }
  void TsEmit(uint8_t cat) {
    if (ts_cur_.count >= kTsPerList - 1 && cat != 0xFE) return;  // keep one slot for the end mark
    if (ts_cur_.count >= kTsPerList) return;
    vk_.vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ts_pool_, ts_cur_.block * kTsPerList + ts_cur_.count);
    ++ts_cur_.count;
    ts_cur_.cats.push_back(cat);
    ts_cur_.tags.push_back(gpu_tag_pending_);  // patch 0101
    gpu_cat_emitted_ = cat;
    gpu_tag_emitted_ = gpu_tag_pending_;
  }
  void TsCat() {
    if (!options_.gpu_timestamp_split) {  // patch 0109: one timestamp at the list's first work, one at its end
      if (ts_open_ && ts_cur_.count == 0) TsEmit(0);
      return;
    }
    if (ts_open_ && (gpu_cat_pending_ != gpu_cat_emitted_ || gpu_tag_pending_ != gpu_tag_emitted_)) TsEmit(gpu_cat_pending_);
  }
  void TsRetire(uint64_t done) {
    while (!ts_pending_.empty() && ts_pending_.front().fence <= done) {
      TsList& l = ts_pending_.front();
      std::vector<uint64_t> t(l.count);
      if (l.count >= 2 &&
          vk_.vkGetQueryPoolResults(device_, ts_pool_, l.block * kTsPerList, l.count, l.count * 8, t.data(), 8,
                                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        const double k = double(limits_.timestampPeriod) / 1e6;  // ns per tick -> ms
        uint64_t prev = t[0] & ts_mask_;
        const uint64_t t0 = prev;
        for (uint32_t i = 1; i < l.count; ++i) {
          const uint64_t v = t[i] & ts_mask_;
          const uint8_t c = l.cats[i - 1];
          if (v >= prev && i - 1 < l.tags.size() && l.tags[i - 1])  // patch 0101
            tag_times_.push_back({l.tags[i - 1], c, double(v - prev) * k});
          if (v >= prev && c < 8) stats_.gpu_cat_ms[c] += double(v - prev) * k;
          if (v >= prev && c >= 0x20 && c < 0x40) {
            stats_.gpu_cat_ms[0] += double(v - prev) * k;
            stats_.gpu_target_ms[c - 0x20] += double(v - prev) * k;
          }
          prev = v;
        }
        if (prev > t0) stats_.gpu_ms += double(prev - t0) * k;
        ++stats_.gpu_lists_timed;
      }
      if (!l.occ_cats.empty() && occ_pool_) {  // patch 0098
        std::vector<uint64_t> o(l.occ_cats.size());
        if (vk_.vkGetQueryPoolResults(device_, occ_pool_, l.block * kTsPerList, uint32_t(o.size()), o.size() * 8, o.data(),
                                      8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
          for (size_t i = 0; i < o.size(); ++i) {
            const uint8_t c = l.occ_cats[i];
            if (options_.vk_count_pixels && c >= 0x20 && c < 0x40) stats_.gpu_target_px[c - 0x20] += double(o[i]);
            if (i < l.occ_tags.size() && l.occ_tags[i]) tag_times_.push_back({l.occ_tags[i], c, 0.0, double(o[i])});  // 0102
          }
      }
      ts_block_busy_[l.block] = false;
      ts_pending_.pop_front();
    }
  }
  // V1: whole-image transitions with full barriers (correct, not fast; stage V5 narrows them).
  // Patch 0082: barriers are queued and sent as one vkCmdPipelineBarrier2 just before the next command that does GPU
  // work (C()), so a pass's attachment transitions, a draw's texture transitions, a dispatch's barrier after it and the
  // next one's before it each become a single wait instead of one per call (job 382: ~315 barrier calls a frame, each a
  // full ALL_COMMANDS wait). An image already queued gets its new layout folded into its queued transition (no work
  // in between, so the old layout is still the one the image is in).
  // Patch 0107: st = the pipeline stages the command recorded next runs in (default: unknown, all).
  VkCommandBuffer C(VkPipelineStageFlags2 st = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
    FlushBarriers();
    work_stages_ |= st;
    return cmd_;
  }
  void FlushBarriers() {
    if (pending_att_ && !pending_mem_ && pending_img_.empty()) {  // patch 0115: attachment writes -> attachment access
      pending_att_ = false;
      VkMemoryBarrier2 a{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      a.srcStageMask = a.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
      a.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      a.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      di.memoryBarrierCount = 1;
      di.pMemoryBarriers = &a;
      vk_.vkCmdPipelineBarrier2(cmd_, &di);
      ++stats_.barrier_calls;
      return;
    }
    pending_att_ = false;  // a full barrier below (or none needed) covers it
    if (options_.vk_skip_memory_barriers && pending_img_.empty() && pending_mem_) {  // patch 0108: diagnostic
      pending_mem_ = false;
      ++stats_.vk_barrier_src[4];
      work_stages_ = 0;
      return;
    }
    if (!pending_mem_ && pending_img_.empty()) return;
    // Patch 0107 (--gow2_renderer_vulkan_barrier_stages=work): wait only for the kinds of work recorded since the
    // last barrier (each barrier's destination is ALL_COMMANDS, so earlier work is already ordered before it).
    VkPipelineStageFlags2 src = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    if (options_.vk_barrier_src_stages && work_stages_ && !(work_stages_ & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) src = work_stages_;
    {
      const VkPipelineStageFlags2 g = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, c = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  t = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
      stats_.vk_barrier_src[src == g ? 0 : src == c ? 1 : src == t ? 2 : src == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT ? 4 : 3]++;
    }
    work_stages_ = 0;
    for (auto& q : pending_img_) q.srcStageMask = src;
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = src;
    m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = pending_mem_ ? 1 : 0;
    di.pMemoryBarriers = pending_mem_ ? &m : nullptr;
    di.imageMemoryBarrierCount = uint32_t(pending_img_.size());
    di.pImageMemoryBarriers = pending_img_.empty() ? nullptr : pending_img_.data();
    vk_.vkCmdPipelineBarrier2(cmd_, &di);
    stats_.barriers += pending_img_.size();
    ++stats_.barrier_calls;
    pending_mem_ = false;
    pending_img_.clear();
  }
  void Transition(Texture& t, VkImageLayout to) {
    if (t.layout == to && to != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return;
    for (auto& q : pending_img_)
      if (q.image == t.image) {
        q.newLayout = to;
        t.layout = to;
        return;
      }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = t.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {t.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    pending_img_.push_back(b);
    t.layout = to;
  }
  // Orders guest-buffer copies after earlier GPU use (V1: full memory barrier).
  void GuestBarrier(bool) { pending_mem_ = true; }  // patch 0082: sent with the next GPU work (FlushBarriers)
  void ClearRects(const VkClearAttachment& ca, const Rect* rects, uint32_t n, uint32_t w, uint32_t h) {
    std::vector<VkClearRect> v;
    v.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      const int32_t x0 = std::clamp(rects[i].x0, 0, int32_t(w)), y0 = std::clamp(rects[i].y0, 0, int32_t(h));
      const int32_t x1 = std::clamp(rects[i].x1, x0, int32_t(w)), y1 = std::clamp(rects[i].y1, y0, int32_t(h));
      if (x1 > x0 && y1 > y0) v.push_back({{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1});
    }
    if (!v.empty()) vk_.vkCmdClearAttachments(C(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT), 1, &ca, uint32_t(v.size()), v.data());
  }
  ReadbackHandle AddReadback(Readback&& rb) {
    rb.live = true;
    rb.fence = 0;  // set by the Submit that sends this command buffer
    for (ReadbackHandle i = 1; i < readbacks_.size(); ++i)
      if (!readbacks_[i].live) { readbacks_[i] = std::move(rb); return i; }
    if (readbacks_.empty()) readbacks_.push_back({});
    readbacks_.push_back(std::move(rb));
    return ReadbackHandle(readbacks_.size() - 1);
  }
  bool Valid(TextureHandle h) const { return ready_ && !lost_ && h && h < textures_.size() && textures_[h].live; }
  void NotDone(const char* what) {
    for (const auto& s : not_done_) if (s == what) return;
    not_done_.push_back(what);
    REXLOG_WARN("gow2_renderer: Vulkan backend: {} is not implemented yet", what);
  }
  void Fail(const std::string& why) {
    if (lost_) return;
    lost_ = true;
    lost_reason_ = why;
    REXLOG_ERROR("gow2_renderer: Vulkan backend: {} - the backend stops here", why);
  }

  BackendOptions options_;
  void* lib_ = nullptr;
  Vk vk_;
  VkInstance instance_ = VK_NULL_HANDLE;
  const VulkanExternal* ext_ = nullptr;  // notes/68 V3: someone else's device (null: ours)
  void LockQueue() { if (ext_ && ext_->lock) ext_->lock(ext_->lock_user); }
  void UnlockQueue() { if (ext_ && ext_->unlock) ext_->unlock(ext_->lock_user); }
  // PresentPacked (notes/68 V3): its own small pipeline (embedded SPIR-V) and descriptor sets.
  VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout present_layout_ = VK_NULL_HANDLE;
  VkPipeline present_pipeline_ = VK_NULL_HANDLE;
  bool present_failed_ = false;
  VkDescriptorPool present_pool_ = VK_NULL_HANDLE;
  std::vector<std::pair<VkDescriptorSet, uint64_t>> present_sets_;  // set, fence of its last use
  VkImageView present_view_ = VK_NULL_HANDLE;  // R32_UINT view of the source
  VkImage present_view_image_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkSemaphore timeline_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_{};
  VkPhysicalDeviceLimits limits_{};
  std::string device_name_, driver_name_;
  uint32_t device_api_ = 0;
  uint64_t device_local_mb_ = 0;
  bool d24s8_ = true;
  bool push_descriptor_ = false;  // Vulkan 1.4 push descriptors (stage V2 binds per draw with them when present)
  bool debug_utils_ = false;
  uint32_t debug_depth_ = 0;
  bool ready_ = false, lost_ = false;
  std::string lost_reason_;
  std::vector<CommandSlot> slots_;
  uint32_t slot_ = 0;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  uint64_t submitted_ = 0, completed_ = 0;
  Buffer ring_, guest_;
  uint64_t ring_head_ = 0, ring_allocated_ = 0, ring_freed_ = 0;
  struct RingMark { uint64_t fence, allocated; };
  std::deque<RingMark> ring_marks_;
  bool guest_written_ = false;
  bool outside_dirty_ = true, guest_writable_ = false;  // patch 0089 (V5b)
  VkPipelineStageFlags2 work_stages_ = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;  // patch 0107: work since the last barrier
  uint64_t barrier_epoch_ = 1;
  uint64_t att_epoch_ = 1;  // patch 0116: bumped by every full or attachment barrier
  uint32_t pass_w_ = 0, pass_h_ = 0;  // patch 0092: the open pass's render area
  std::vector<Texture> textures_ = std::vector<Texture>(1);  // [0] = none
  std::vector<Readback> readbacks_;
  std::deque<Deferred> deferred_;
  std::vector<const char*> not_done_;
  BackendStats stats_;
  // Stage V2.
  struct IndexBuffer { bool live = false; Buffer buffer; };
  struct DescriptorPool { VkDescriptorPool pool = VK_NULL_HANDLE; uint64_t fence = 0; };
  // Stage V4.
  std::vector<DescriptorPool> compute_pools_;
  uint32_t compute_pool_index_ = UINT32_MAX;
  Buffer scratch_;
  struct ScratchLayout { TextureHandle texture = 0; uint32_t mip = 0, layer = 0, row_pitch = 0, rows = 0, slices = 1; } scratch_layout_;
  VkDescriptorSetLayout set_layouts_[3] = {};
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkDescriptorPool cb_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet cb_set_ = VK_NULL_HANDLE;
  VkPipelineCache pipeline_cache_vk_ = VK_NULL_HANDLE;
  TextureHandle defaults_[2][3] = {};  // [zero][2D, cube, 3D]
  bool depth_clamp_ = false, fill_mode_non_solid_ = false, anisotropy_ = false;
  std::vector<Shader> shaders_ = std::vector<Shader>(1);
  std::deque<std::unique_ptr<Pipeline>> pipelines_ = [] { std::deque<std::unique_ptr<Pipeline>> d; d.push_back(std::make_unique<Pipeline>()); return d; }();
  std::unordered_map<uint64_t, std::vector<std::pair<GraphicsPipelineDesc, PipelineHandle>>> pipeline_cache_;
  uint64_t pipeline_misses_ = 0;
  std::vector<std::thread> workers_;
  std::deque<PipelineJob> jobs_;
  std::deque<PipelineJob> backlog_jobs_;  // patch 0076: start-up backlog / guessed pipelines, after live requests
  bool pipeline_backlog_ = false;
  // Patch 0077: GPU timestamps.
  VkQueryPool ts_pool_ = VK_NULL_HANDLE;
  VkQueryPool occ_pool_ = VK_NULL_HANDLE;  // patch 0098
  bool occ_failed_ = false, occ_precise_ = false, occ_active_ = false;
  uint8_t occ_cat_ = 0;
  uint32_t occ_tag_ = 0;      // patch 0102
  bool tag_samples_ = false;  // patch 0102
  bool pipeline_stats_ = false;  // patch 0104
  PFN_vkGetPipelineExecutablePropertiesKHR get_exec_props_ = nullptr;
  PFN_vkGetPipelineExecutableStatisticsKHR get_exec_stats_ = nullptr;
  bool ts_failed_ = false, ts_open_ = false;
  uint64_t ts_mask_ = ~0ull;
  std::vector<bool> ts_block_busy_;
  uint32_t ts_next_block_ = 0;
  TsList ts_cur_;
  std::deque<TsList> ts_pending_;
  uint8_t gpu_cat_pending_ = 0, gpu_cat_emitted_ = 0xFF;
  uint32_t gpu_tag_pending_ = 0, gpu_tag_emitted_ = 0;  // patch 0101
  std::vector<GpuTagTime> tag_times_;
  size_t backlog_busy_ = 0;
  size_t worker_count_ = 1;
  uint64_t jobs_busy_ = 0;
  std::mutex job_mutex_;
  std::condition_variable job_cv_;
  bool stop_workers_ = false;
  std::atomic<uint64_t> built_async_{0}, async_us_{0}, async_bytes_{0}, async_max_us_{0};
  // Patch 0079: the pipeline cache file and its periodic save (the harness ends the process without a shutdown).
  std::string cache_path_;
  std::chrono::steady_clock::time_point cache_saved_at_{};
  uint64_t cache_saved_built_ = 0;
  std::atomic<bool> cache_saving_{false};
  std::vector<IndexBuffer> index_buffers_;
  std::vector<DescriptorPool> pools_;
  uint32_t pool_index_ = UINT32_MAX;
  std::vector<uint32_t> used_pools_;
  // A cached set and the pool it came from: a set found again is in use by the command buffer being recorded, so its
  // pool must not be reset before that submission completes (patch 0080; a hit used to leave the pool's fence at its
  // last submission).
  struct CachedSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t pool = UINT32_MAX;
  };
  VkDescriptorSet UseCached(const CachedSet& c) {
    if (c.pool < pools_.size()) pools_[c.pool].fence = UINT64_MAX;
    return c.set;
  }
  std::unordered_map<uint64_t, CachedSet> view_sets_, sampler_sets_;
  std::unordered_map<uint64_t, VkSampler> sampler_objects_;
  TargetSet targets_;
  bool in_pass_ = false;
  std::vector<TextureHandle> pass_written_;
  std::vector<TextureHandle> att_layout_;  // patch 0100: textures put in an attachment layout, not yet back in GENERAL
  std::vector<VkImageMemoryBarrier2> pending_img_;  // patch 0082: queued barriers, sent by FlushBarriers
  bool pending_mem_ = false;
  bool pending_att_ = false;  // patch 0115: an attachment-only barrier queued for the next pass's first command
  PipelineHandle current_pipeline_ = 0;
  VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
  VkBuffer bound_index_ = VK_NULL_HANDLE;  // patch 0081
  TextureView views_[96];
  SamplerDesc samplers_[32];
  VkDescriptorSet cur_view_set_ = VK_NULL_HANDLE, cur_sampler_set_ = VK_NULL_HANDLE;
  uint32_t cb_offset_[2 * kCbCount] = {};
  // Patch 0080: what each constant slot holds in the command buffer being recorded; slots holding a texture; the
  // sampler object of each slot's last description.
  struct CbShadow {
    bool valid = false;
    uint32_t size = 0;
    uint8_t data[kCbRange];
  };
  CbShadow cb_shadow_[2 * kCbCount];
  uint8_t bound_views_[96] = {};
  uint32_t bound_view_count_ = 0;
  VkSampler sampler_memo_[32] = {};
  SamplerDesc sampler_memo_desc_[32];
  VkViewport viewport_{0, 1, 1, -1, 0, 1};
  VkRect2D scissor_{{0, 0}, {1, 1}};
  uint8_t stencil_ref_ = 0;
  float blend_[4] = {};
  bool dirty_views_ = true, dirty_samplers_ = true, dirty_sets_ = true, dirty_cb_ = true;
  bool dirty_viewport_ = true, dirty_scissor_ = true, dirty_stencil_ = true, dirty_blend_ = true;
};

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace xenos::renderer::gpu

#endif  // GN_VULKAN_BACKEND
