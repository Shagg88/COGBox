// Layer 0 (game-free), notes/83: AMD FSR frame generation on the Vulkan renderer (--gow2_frame_generation=fsr with
// --gow2_renderer_api=vulkan). Its own file: AMD's DirectX 12 and Vulkan headers can't share one. Built for real only
// with the FidelityFX SDK (GN_HAVE_FSR), its Vulkan headers, the Vulkan headers and SDK patch 69 (GN_FG_VK below);
// otherwise XenosFgVkInstall logs why and the game runs without frame generation. frame_generation.cpp calls in here.
//
// The same design as the D3D12 path above, through SDK patch 69 (rex/frame_hooks.h):
//  - vk_all_queues (set at install, before the Vulkan device exists): the device also makes the spare queues AMD's
//    swap chain needs - it wants four different VkQueues: the game queue (the presenter's graphics queue, which our
//    renderer submits on too), an async compute queue, a present queue and an image acquire queue.
//  - vk_create_swap_chain: AMD's frame-generation swap chain (ffxCreateContextDescFrameGenerationSwapChainVK) is made
//    with the presenter's VkSwapchainCreateInfoKHR, and the presenter then uses AMD's replacement functions for the
//    images, acquire, present and destroy (ffxQueryDescSwapchainReplacementFunctionsVK). Our destroy turns frame
//    generation off and destroys the contexts first (AMD: the swap chain outlives its context).
//  - vk_before_present: the frame's Prepare pass (depth + motion, which the renderer copied per presented image in
//    FgHandOver and left in SHADER_READ_ONLY_OPTIMAL) goes on the game queue in a command buffer of ours, then the
//    presenter calls AMD's present.
// AMD submits to the game queue from its own present path; it gets a submit function that takes the SDK's lock for
// that queue, as everything else that submits there does.
//
// HUD-less colour (patch 0148 on Vulkan): the renderer's HUD-less image (the generation rectangle's size, in the
// back buffer's byte layout - red/blue swapped for a B8G8R8A8 back buffer) is copied in our Prepare command buffer into
// a back-buffer-sized image of the back buffer's format (vkCmdCopyImage between size-compatible formats copies the
// bytes), which AMD gets as HUDLessColor.
//
// Not yet (notes/83): a repaint of an image already shown can't be dropped on Vulkan (the swap chain
// image is already acquired when we see it): it is presented, its inputs re-sent with reset.
//
// Run time: amd_fidelityfx_loader_vk.dll + amd_fidelityfx_framegeneration_vk.dll next to the exe (AMD's, MIT).

#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/frame_hooks.h>
#include <rex/logging/macros.h>

#include <string>

// REX_FRAME_HOOKS_VK: rex/frame_hooks.h has SDK patch 69's members.
#if defined(GN_HAVE_FSR) && defined(REX_FRAME_HOOKS_VK) && __has_include(<vk/ffx_api_vk.h>) && \
    __has_include(<vk/ffx_api_framegeneration_vk.h>) && __has_include(<vulkan/vulkan.h>)
#define GN_FG_VK 1
#else
#define GN_FG_VK 0
#endif

#if GN_FG_VK

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vulkan/vulkan.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_api_types.h>
#include <vk/ffx_api_vk.h>
#include <ffx_framegeneration.h>
#include <vk/ffx_api_framegeneration_vk.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

REXCVAR_DECLARE(int32_t, gow2_fg_debug);  // frame_generation.cpp
// Notes/83 (Judgment freezes, jobs 1389-1390): A/B switches for AMD's Vulkan swap chain; the defaults are 0.4.8's.
REXCVAR_DEFINE_STRING(gow2_fg_vk_present_queue, "graphics", "Gears2",
                      "Vulkan frame generation: which spare queue AMD's swap chain presents on - graphics = one of the "
                      "graphics family's (AMD then composes on it), compute = a compute-only family's when it can "
                      "present (else as graphics)");
REXCVAR_DEFINE_STRING(gow2_fg_vk_compose, "auto", "Gears2",
                      "Vulkan frame generation: where AMD composes the frames it presents - auto = AMD's choice from "
                      "the present queue, game = on the game queue, present = on the present queue");

namespace fgvk {

// The inputs per presenter image (as frame_generation.cpp's): written by the renderer thread, read at present time.
struct Inputs {
  void* resource = nullptr;  // the presenter's guest-output VkImage
  XenosFgFrame frame{};
  bool has = false;          // false: no 3D scene that frame (generation pauses)
  uint64_t serial = 0;
};

std::mutex g_mutex;  // every hook and SetInputs; never held across AMD's present
std::atomic<bool> g_active{false};          // installed: the Vulkan renderer's frame generation answers the queries
std::atomic<bool> g_inputs_wanted{false};   // the renderer hands over inputs (off again when anything fails)
HMODULE g_module = nullptr;
ffxFunctions g_fn{};
bool g_load_failed = false;

// The device and the game queue (from the presenter, SDK patch 69).
VkDevice g_device = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkInstance g_instance = VK_NULL_HANDLE;
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
PFN_vkGetDeviceProcAddr g_gdpa = nullptr;
VkQueue g_game_queue = VK_NULL_HANDLE;
uint32_t g_game_family = 0;
void* g_lock_user = nullptr;
void (*g_lock)(void*) = nullptr;
void (*g_unlock)(void*) = nullptr;
struct Fns {
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkCreateCommandPool CreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkCreateFence CreateFence = nullptr;
  PFN_vkDestroyFence DestroyFence = nullptr;
  PFN_vkWaitForFences WaitForFences = nullptr;
  PFN_vkResetFences ResetFences = nullptr;
  // HUD-less colour
  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkBindImageMemory BindImageMemory = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImage CmdCopyImage = nullptr;
  PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
} g_vk;

// HUD-less colour (patch 0148 on Vulkan).
bool g_hudless_on = false;                   // --gow2_frame_generation_hudless
std::atomic<uint32_t> g_hudless_format{0};   // for the renderer: 0 none, 1 RGBA8, 2 RGB10A2, 3 RGBA8 holding BGRA8 bytes
std::atomic<uint32_t> g_hl_rect_size{0};     // the last generation rectangle, width << 16 | height
VkImage g_hl_image = VK_NULL_HANDLE;         // back-buffer size and format; rests in SHADER_READ_ONLY_OPTIMAL
VkDeviceMemory g_hl_memory = VK_NULL_HANDLE;
uint32_t g_hl_w = 0, g_hl_h = 0;
VkFormat g_hl_format = VK_FORMAT_UNDEFINED;
bool g_hl_fresh = false;                     // just made: contents undefined (cleared on its first use)
uint64_t g_hl_used = 0, g_hl_missing = 0, g_hl_mismatch = 0;

// The swap chain.
ffxContext g_swap_chain_ctx = nullptr;
VkSwapchainKHR g_swap_chain = VK_NULL_HANDLE;
VkSwapchainCreateInfoKHR g_create_info{};
ffxQueryDescSwapchainReplacementFunctionsVK g_replace{};

// Frame generation.
ffxContext g_fg_ctx = nullptr;
bool g_fg_failed = false;
uint32_t g_fg_display_w = 0, g_fg_display_h = 0, g_fg_render_w = 0, g_fg_render_h = 0;
uint32_t g_transfer = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
bool g_gen_on = false;
uint64_t g_frame_id = 0, g_last_shown_frame = 0;
XenosFgFrame g_last_frame{};
bool g_have_last_frame = false;
uint32_t g_no_scene_streak = 0;
constexpr uint32_t kNoSceneFramesBeforeOff = 30;

// Prepare command buffers (one per frame in flight, each with its fence).
VkCommandPool g_pool = VK_NULL_HANDLE;
struct PrepareList {
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
};
std::array<PrepareList, 3> g_lists{};
uint64_t g_list_index = 0;

std::array<Inputs, 4> g_inputs{};
uint64_t g_serial = 0, g_last_presented = 0;
uint64_t g_presents = 0, g_repaints = 0, g_generated = 0, g_paused = 0, g_resets = 0, g_prepare_failed = 0;

// AMD's submits to the game queue: under the SDK's lock for it.
VkResult GameSubmit(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
  if (g_lock) g_lock(g_lock_user);
  const VkResult r = g_vk.QueueSubmit(g_game_queue, count, submits, fence);
  if (g_unlock) g_unlock(g_lock_user);
  return r;
}

bool Load() {
  if (g_fn.CreateContext) return true;
  if (g_load_failed) return false;
  g_module = LoadLibraryW(L"amd_fidelityfx_loader_vk.dll");
  if (!g_module) {
    g_load_failed = true;
    REXLOG_WARN("gow2_frame_generation: Vulkan: amd_fidelityfx_loader_vk.dll not found next to the exe; no frame "
                "generation, usual swap chain");
    return false;
  }
  ffxLoadFunctions(&g_fn, g_module);
  if (!g_fn.CreateContext || !g_fn.DestroyContext || !g_fn.Dispatch || !g_fn.Configure || !g_fn.Query) {
    g_load_failed = true;
    g_fn = {};
    REXLOG_WARN("gow2_frame_generation: Vulkan: the FidelityFX loader has no ffx_api entry points; no frame "
                "generation, usual swap chain");
    return false;
  }
  return true;
}

void WaitForPresents() {
  if (!g_swap_chain_ctx) return;
  ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w{};
  w.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
  g_fn.Dispatch(&g_swap_chain_ctx, &w.header);
}

void WaitForPrepareLists() {
  for (PrepareList& l : g_lists)
    if (l.pending) {
      g_vk.WaitForFences(g_device, 1, &l.fence, VK_TRUE, 2000000000ull);  // bounded: a lost device must not hang us
      l.pending = false;
    }
}

void ReleasePrepareObjects() {
  WaitForPrepareLists();
  for (PrepareList& l : g_lists) {
    if (l.fence) g_vk.DestroyFence(g_device, l.fence, nullptr);
    l = {};
  }
  if (g_pool) g_vk.DestroyCommandPool(g_device, g_pool, nullptr), g_pool = VK_NULL_HANDLE;
  if (g_hl_image) g_vk.DestroyImage(g_device, g_hl_image, nullptr), g_hl_image = VK_NULL_HANDLE;  // HUD-less colour
  if (g_hl_memory) g_vk.FreeMemory(g_device, g_hl_memory, nullptr), g_hl_memory = VK_NULL_HANDLE;
  g_hl_w = g_hl_h = 0;
  g_hl_format = VK_FORMAT_UNDEFINED;
}

// HUD-less colour: the back-buffer-sized image (made again when the swap chain's size or format changes). Callers hold
// g_mutex; the old one is only freed after the Prepare lists and AMD's presents are done with it.
bool EnsureHudless() {
  const VkFormat format = g_create_info.imageFormat;
  const uint32_t w = g_create_info.imageExtent.width, h = g_create_info.imageExtent.height;
  if (g_hl_image && g_hl_w == w && g_hl_h == h && g_hl_format == format) return true;
  if (g_hl_image) {
    WaitForPresents();
    WaitForPrepareLists();
    g_vk.DestroyImage(g_device, g_hl_image, nullptr), g_hl_image = VK_NULL_HANDLE;
    g_vk.FreeMemory(g_device, g_hl_memory, nullptr), g_hl_memory = VK_NULL_HANDLE;
  }
  if (!g_vk.GetPhysicalDeviceMemoryProperties) return false;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (g_vk.CreateImage(g_device, &ci, nullptr, &g_hl_image) != VK_SUCCESS) {
    g_hl_image = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements mr{};
  g_vk.GetImageMemoryRequirements(g_device, g_hl_image, &mr);
  VkPhysicalDeviceMemoryProperties mp{};
  g_vk.GetPhysicalDeviceMemoryProperties(g_physical, &mp);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
    if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
  for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
    if (mr.memoryTypeBits & (1u << i)) type = i;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = type;
  if (type == UINT32_MAX || g_vk.AllocateMemory(g_device, &ai, nullptr, &g_hl_memory) != VK_SUCCESS ||
      g_vk.BindImageMemory(g_device, g_hl_image, g_hl_memory, 0) != VK_SUCCESS) {
    if (g_hl_memory) g_vk.FreeMemory(g_device, g_hl_memory, nullptr), g_hl_memory = VK_NULL_HANDLE;
    g_vk.DestroyImage(g_device, g_hl_image, nullptr), g_hl_image = VK_NULL_HANDLE;
    REXLOG_WARN("gow2_frame_generation_hudless: Vulkan: no memory for the HUD-less image; none this frame");
    return false;
  }
  g_hl_w = w, g_hl_h = h, g_hl_format = format;
  g_hl_fresh = true;
  return true;
}

void ImageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                  VkAccessFlags dst_access) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = src_access;
  b.dstAccessMask = dst_access;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  g_vk.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &b);
}

// Records the HUD-less copy: the renderer's image (SHADER_READ_ONLY_OPTIMAL, given back so) at (x, y) in ours.
void RecordHudlessCopy(VkCommandBuffer cb, const XenosFgFrame& f, int32_t x, int32_t y) {
  const auto src = reinterpret_cast<VkImage>(f.hudless);
  if (g_hl_fresh) {  // first use: defined contents outside the rectangle (black)
    ImageBarrier(cb, g_hl_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_vk.CmdClearColorImage(cb, g_hl_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
    ImageBarrier(cb, g_hl_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);  // clear before the copy (write-after-write)
    g_hl_fresh = false;
  } else {
    ImageBarrier(cb, g_hl_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  }
  ImageBarrier(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  VkImageCopy c{};
  c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.dstSubresource = c.srcSubresource;
  c.dstOffset = {x, y, 0};
  c.extent = {f.hudless_width, f.hudless_height, 1};
  g_vk.CmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_hl_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
  ImageBarrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
  ImageBarrier(cb, g_hl_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

bool EnsurePrepareObjects() {
  if (g_pool) return true;
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pi.queueFamilyIndex = g_game_family;
  if (g_vk.CreateCommandPool(g_device, &pi, nullptr, &g_pool) != VK_SUCCESS) {
    g_pool = VK_NULL_HANDLE;
    REXLOG_WARN("gow2_frame_generation: Vulkan: no command pool for the Prepare pass; frames shown as drawn");
    return false;
  }
  for (PrepareList& l : g_lists) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (g_vk.AllocateCommandBuffers(g_device, &ai, &l.cb) != VK_SUCCESS ||
        g_vk.CreateFence(g_device, &fi, nullptr, &l.fence) != VK_SUCCESS) {
      ReleasePrepareObjects();
      REXLOG_WARN("gow2_frame_generation: Vulkan: no command buffers for the Prepare pass; frames shown as drawn");
      return false;
    }
  }
  return true;
}

ffxReturnCode_t GenerateCallback(ffxDispatchDescFrameGeneration* params, void*) {
  params->backbufferTransferFunction = g_transfer;
  if (g_transfer == FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ) {
    params->minMaxLuminance[0] = 0.0001f;
    params->minMaxLuminance[1] = 1000.0f;
  }
  return g_fn.Dispatch(&g_fg_ctx, &params->header);
}

void DestroyFrameGeneration() {
  if (!g_fg_ctx) return;
  if (g_swap_chain_ctx && g_swap_chain) {
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = reinterpret_cast<void*>(g_swap_chain);
    c.frameGenerationEnabled = false;
    c.frameID = g_frame_id;
    g_fn.Configure(&g_fg_ctx, &c.header);
  }
  WaitForPresents();
  WaitForPrepareLists();
  g_fn.DestroyContext(&g_fg_ctx, nullptr);
  g_fg_ctx = nullptr;
  g_gen_on = false;
  g_have_last_frame = false;
  REXLOG_INFO("gow2_frame_generation: Vulkan: frame-generation context destroyed");
}

bool EnsureFrameGeneration(const XenosFgFrame& f) {
  if (!g_swap_chain || g_fg_failed) return false;
  const uint32_t dw = g_create_info.imageExtent.width, dh = g_create_info.imageExtent.height;
  if (g_fg_ctx && g_fg_display_w == dw && g_fg_display_h == dh && f.width <= g_fg_render_w &&
      f.height <= g_fg_render_h)
    return true;
  DestroyFrameGeneration();
  ffxCreateBackendVKDesc be{};
  be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
#if defined(FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED)  // FidelityFX 2.x: the device in deviceContext
  be.deviceContext.vkDevice = g_device;
  be.deviceContext.vkPhysicalDevice = g_physical;
  be.deviceContext.vkDeviceProcAddr = g_gdpa;
#else
  be.vkDevice = g_device;
  be.vkPhysicalDevice = g_physical;
  be.vkDeviceProcAddr = g_gdpa;
#endif
  ffxCreateContextDescFrameGenerationVersion ver{};
  ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  ver.version = FFX_FRAMEGENERATION_VERSION;
  ver.header.pNext = &be.header;
  ffxCreateContextDescFrameGeneration c{};
  c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  c.header.pNext = &ver.header;
  const bool hdr = g_create_info.imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
  c.flags = (f.infinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0u) |
            (hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0u);
  c.displaySize = {dw, dh};
  c.maxRenderSize = {std::max(f.width, g_fg_render_w), std::max(f.height, g_fg_render_h)};
  c.backBufferFormat = ffxApiGetSurfaceFormatVK(g_create_info.imageFormat);
  REXLOG_INFO("gow2_frame_generation: Vulkan: making the frame-generation context (back buffer format {})",
              uint32_t(g_create_info.imageFormat));
  const ffxReturnCode_t rc = g_fn.CreateContext(&g_fg_ctx, &c.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !g_fg_ctx) {
    g_fg_ctx = nullptr;
    g_fg_failed = true;
    REXLOG_WARN("gow2_frame_generation: Vulkan: the frame-generation context was not made (code {}); frames are shown "
                "as drawn", uint32_t(rc));
    return false;
  }
  g_fg_display_w = dw, g_fg_display_h = dh;
  g_fg_render_w = c.maxRenderSize.width, g_fg_render_h = c.maxRenderSize.height;
  g_transfer = hdr ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
  std::string version = "?";
  ffxQueryGetProviderVersion v{};
  v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  if (g_fn.Query(&g_fg_ctx, &v.header) == FFX_API_RETURN_OK && v.versionName) version = v.versionName;
  REXLOG_INFO("gow2_frame_generation: Vulkan: AMD frame generation {} on: display {}x{}, scene up to {}x{}{}{}", version,
              dw, dh, g_fg_render_w, g_fg_render_h, hdr ? ", HDR10" : "", f.infinite ? ", infinite depth" : "");
  return true;
}

FfxApiResource InputResource(void* image, VkFormat format, uint32_t width, uint32_t height) {
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {width, height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  return ffxApiGetResourceVK(image, ffxApiGetImageResourceDescriptionVK(reinterpret_cast<VkImage>(image), ci, 0),
                             FFX_API_RESOURCE_STATE_COMPUTE_READ);  // our images rest in SHADER_READ_ONLY_OPTIMAL
}

// Records and submits the Prepare pass on the game queue; false = generation paused for this frame.
bool Prepare(const XenosFgFrame& f, uint32_t flags, int32_t hl_x = -1, int32_t hl_y = -1) {
  PrepareList& l = g_lists[g_list_index % g_lists.size()];
  if (l.pending) {
    if (g_vk.WaitForFences(g_device, 1, &l.fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return false;
    l.pending = false;
  }
  if (g_vk.ResetFences(g_device, 1, &l.fence) != VK_SUCCESS || g_vk.ResetCommandBuffer(l.cb, 0) != VK_SUCCESS)
    return false;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (g_vk.BeginCommandBuffer(l.cb, &bi) != VK_SUCCESS) return false;
  if (hl_x >= 0 && g_hl_image && f.hudless) RecordHudlessCopy(l.cb, f, hl_x, hl_y);  // HUD-less colour
  ffxDispatchDescFrameGenerationPrepareV2 p{};
  p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
  p.frameID = g_frame_id;
  p.flags = flags;
  p.commandList = reinterpret_cast<void*>(l.cb);
  p.renderSize = {f.width, f.height};
  p.jitterOffset = {f.jitter_x, f.jitter_y};
  p.motionVectorScale = {1.0f, 1.0f};
  p.frameTimeDelta = f.frame_ms;
  p.reset = f.reset;
  p.cameraNear = f.camera_near;
  p.cameraFar = f.camera_far;
  p.cameraFovAngleVertical = f.vertical_fov;
  p.viewSpaceToMetersFactor = 0.01f;  // Unreal units are centimetres
  p.depth = InputResource(f.depth, VK_FORMAT_R32_SFLOAT, f.width, f.height);
  p.motionVectors = InputResource(f.motion, VK_FORMAT_R16G16_SFLOAT, f.width, f.height);
  std::memcpy(p.cameraPosition, f.camera_position, sizeof(p.cameraPosition));
  std::memcpy(p.cameraUp, f.camera_up, sizeof(p.cameraUp));
  std::memcpy(p.cameraRight, f.camera_right, sizeof(p.cameraRight));
  std::memcpy(p.cameraForward, f.camera_forward, sizeof(p.cameraForward));
  const ffxReturnCode_t rc = g_fn.Dispatch(&g_fg_ctx, &p.header);
  if (g_vk.EndCommandBuffer(l.cb) != VK_SUCCESS || rc != FFX_API_RETURN_OK) return false;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &l.cb;
  if (GameSubmit(1, &si, l.fence) != VK_SUCCESS) return false;
  l.pending = true;
  ++g_list_index;
  return true;
}

// SDK patch 69's destroy: frame generation and the swap-chain context go first, then AMD destroys its swap chain.
void VKAPI_CALL DestroySwapChain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (swapchain != g_swap_chain || !g_swap_chain_ctx) {  // not ours (never expected): the device's own destroy
    REXLOG_WARN("gow2_frame_generation: Vulkan: asked to destroy a swap chain that is not AMD's; left alone");
    return;
  }
  DestroyFrameGeneration();
  WaitForPresents();
  ReleasePrepareObjects();
  g_fn.DestroyContext(&g_swap_chain_ctx, nullptr);
  g_swap_chain_ctx = nullptr;
  if (g_replace.pOutDestroySwapchainFFXAPI) g_replace.pOutDestroySwapchainFFXAPI(device, swapchain, allocator, nullptr);
  g_swap_chain = VK_NULL_HANDLE;
  g_fg_failed = false;
  g_hudless_format.store(0, std::memory_order_relaxed);
  REXLOG_INFO("gow2_frame_generation: Vulkan: AMD frame-generation swap chain destroyed");
}

// Picks AMD's three extra queues from the device's spare ones: all different VkQueues.
bool PickQueues(const rex::VulkanSwapChainHook& io, VkQueueInfoFFXAPI& compute, VkQueueInfoFFXAPI& present,
                VkQueueInfoFFXAPI& acquire) {
  std::vector<bool> used(io.queue_count, false);
  auto take = [&](VkQueueInfoFFXAPI& out, auto&& fits) {
    for (uint32_t i = 0; i < io.queue_count; ++i)
      if (!used[i] && io.queues[i].queue && fits(io.queues[i])) {
        used[i] = true;
        out = {};
        out.queue = static_cast<VkQueue>(io.queues[i].queue);
        out.familyIndex = io.queues[i].family;
        return true;
      }
    return false;
  };
  const uint32_t gc = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
  // Present: a graphics + compute family first (AMD then composes on the present queue), else any that can present;
  // --gow2_fg_vk_present_queue=compute tries a compute-only family that can present first.
  const bool compute_first = REXCVAR_GET(gow2_fg_vk_present_queue) == "compute";
  if (!(compute_first &&
        take(present, [&](const auto& q) { return q.present && (q.flags & VK_QUEUE_COMPUTE_BIT) && !(q.flags & VK_QUEUE_GRAPHICS_BIT); })) &&
      !take(present, [&](const auto& q) { return q.present && (q.flags & gc) == gc; }) &&
      !take(present, [&](const auto& q) { return q.present; }))
    return false;
  // Async compute: a compute-only family first (the real async queue), else any compute queue.
  if (!take(compute, [&](const auto& q) { return (q.flags & VK_QUEUE_COMPUTE_BIT) && !(q.flags & VK_QUEUE_GRAPHICS_BIT); }) &&
      !take(compute, [&](const auto& q) { return (q.flags & VK_QUEUE_COMPUTE_BIT) != 0; }))
    return false;
  // Image acquire: any queue at all (a transfer queue is fine).
  return take(acquire, [&](const auto&) { return true; });
}

bool MakeSwapChain(rex::VulkanSwapChainHook* io) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto off = [](const char* why) {
    g_inputs_wanted.store(false, std::memory_order_relaxed);
    REXLOG_WARN("gow2_frame_generation: Vulkan: {}; no frame generation, usual swap chain", why);
    return false;
  };
  if (!io || !io->create_info || !io->device || !io->game_queue || !io->get_device_proc_addr)
    return off("the presenter gave no device or queue");
  if (!Load()) return off("AMD's Vulkan loader missing");
  if (g_swap_chain_ctx) return off("a swap chain context is still alive");
  g_device = static_cast<VkDevice>(io->device);
  g_physical = static_cast<VkPhysicalDevice>(io->physical_device);
  g_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(io->get_device_proc_addr);
  g_game_queue = static_cast<VkQueue>(io->game_queue);
  g_game_family = io->game_family;
  g_lock_user = io->queue_lock_user;
  g_lock = io->lock_queue;
  g_unlock = io->unlock_queue;
#define GN_VK_FN(name) g_vk.name = reinterpret_cast<PFN_vk##name>(g_gdpa(g_device, "vk" #name))
  GN_VK_FN(QueueSubmit);
  GN_VK_FN(CreateCommandPool);
  GN_VK_FN(DestroyCommandPool);
  GN_VK_FN(AllocateCommandBuffers);
  GN_VK_FN(ResetCommandBuffer);
  GN_VK_FN(BeginCommandBuffer);
  GN_VK_FN(EndCommandBuffer);
  GN_VK_FN(CreateFence);
  GN_VK_FN(DestroyFence);
  GN_VK_FN(WaitForFences);
  GN_VK_FN(ResetFences);
  GN_VK_FN(CreateImage);
  GN_VK_FN(DestroyImage);
  GN_VK_FN(GetImageMemoryRequirements);
  GN_VK_FN(AllocateMemory);
  GN_VK_FN(FreeMemory);
  GN_VK_FN(BindImageMemory);
  GN_VK_FN(CmdPipelineBarrier);
  GN_VK_FN(CmdCopyImage);
  GN_VK_FN(CmdClearColorImage);
#undef GN_VK_FN
  g_instance = static_cast<VkInstance>(io->instance);
  g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(io->get_instance_proc_addr);
  if (g_gipa && g_instance)
    g_vk.GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties"));
  if (!g_vk.QueueSubmit || !g_vk.CreateCommandPool || !g_vk.DestroyCommandPool || !g_vk.AllocateCommandBuffers ||
      !g_vk.ResetCommandBuffer || !g_vk.BeginCommandBuffer || !g_vk.EndCommandBuffer || !g_vk.CreateFence ||
      !g_vk.DestroyFence || !g_vk.WaitForFences || !g_vk.ResetFences)
    return off("device functions missing");
  VkQueueInfoFFXAPI compute{}, present{}, acquire{};
  if (!PickQueues(*io, compute, present, acquire)) {
    REXLOG_WARN("gow2_frame_generation: Vulkan: {} spare queues on this device (needs 3 more besides the game queue: "
                "compute, present, any)", io->queue_count);
    return off("not enough queues for AMD's swap chain");
  }
  g_create_info = *static_cast<const VkSwapchainCreateInfoKHR*>(io->create_info);
  g_create_info.oldSwapchain = VK_NULL_HANDLE;
  ffxCreateContextDescFrameGenerationSwapChainVK d{};
  d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
  VkSwapchainKHR sc = VK_NULL_HANDLE;
  d.physicalDevice = g_physical;
  d.device = g_device;
  d.swapchain = &sc;
  d.allocator = nullptr;
  d.createInfo = g_create_info;
  d.gameQueue.queue = g_game_queue;
  d.gameQueue.familyIndex = g_game_family;
  d.gameQueue.submitFunc = &GameSubmit;
  d.asyncComputeQueue = compute;
  d.presentQueue = present;
  d.imageAcquireQueue = acquire;
#if defined(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK)
  ffxCreateContextDescFrameGenerationSwapChainVersionVK v{};
  v.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
  v.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
  d.header.pNext = &v.header;
#endif
  const std::string compose = REXCVAR_GET(gow2_fg_vk_compose);
#if defined(FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_MODE_VK)
  ffxCreateContextDescFrameGenerationSwapChainModeVK mode{};
  if (compose == "game" || compose == "present") {
    mode.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_MODE_VK;
    mode.composeOnPresentQueue = compose == "present";
    mode.header.pNext = d.header.pNext;
    d.header.pNext = &mode.header;
  }
#else
  if (compose != "auto") REXLOG_WARN("gow2_fg_vk_compose: this FidelityFX SDK has no composition mode; auto");
#endif
  REXLOG_INFO("gow2_frame_generation: Vulkan: making AMD's swap chain ({}x{}, format {}, present mode {}; queues: game "
              "family {}, compute family {}, present family {}, acquire family {}; compose {})",
              g_create_info.imageExtent.width, g_create_info.imageExtent.height, uint32_t(g_create_info.imageFormat),
              uint32_t(g_create_info.presentMode), g_game_family, compute.familyIndex, present.familyIndex,
              acquire.familyIndex, compose);
  const ffxReturnCode_t rc = g_fn.CreateContext(&g_swap_chain_ctx, &d.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !sc || !g_swap_chain_ctx) {
    if (g_swap_chain_ctx) g_fn.DestroyContext(&g_swap_chain_ctx, nullptr);
    g_swap_chain_ctx = nullptr;
    REXLOG_WARN("gow2_frame_generation: Vulkan: AMD's frame-generation swap chain was not made (code {})", uint32_t(rc));
    return off("AMD's swap chain failed");
  }
  g_replace = {};
  g_replace.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
  if (g_fn.Query(&g_swap_chain_ctx, &g_replace.header) != FFX_API_RETURN_OK || !g_replace.pOutGetSwapchainImagesKHR ||
      !g_replace.pOutAcquireNextImageKHR || !g_replace.pOutQueuePresentKHR || !g_replace.pOutDestroySwapchainFFXAPI) {
    g_fn.DestroyContext(&g_swap_chain_ctx, nullptr);
    g_swap_chain_ctx = nullptr;
    if (g_replace.pOutDestroySwapchainFFXAPI) g_replace.pOutDestroySwapchainFFXAPI(g_device, sc, nullptr, nullptr);
    return off("AMD's swap chain gave no replacement functions");
  }
  g_swap_chain = sc;
  g_fg_failed = false;
  io->swapchain = reinterpret_cast<void*>(sc);
  io->get_swapchain_images = reinterpret_cast<void*>(g_replace.pOutGetSwapchainImagesKHR);
  io->acquire_next_image = reinterpret_cast<void*>(g_replace.pOutAcquireNextImageKHR);
  io->queue_present = reinterpret_cast<void*>(g_replace.pOutQueuePresentKHR);
  io->destroy_swapchain = reinterpret_cast<void*>(&DestroySwapChain);
  // HUD-less colour: the renderer makes its image in the back buffer's byte layout (a straight copy then)
  const bool hl_fns = g_vk.CreateImage && g_vk.DestroyImage && g_vk.GetImageMemoryRequirements && g_vk.AllocateMemory &&
                      g_vk.FreeMemory && g_vk.BindImageMemory && g_vk.CmdPipelineBarrier && g_vk.CmdCopyImage &&
                      g_vk.CmdClearColorImage && g_vk.GetPhysicalDeviceMemoryProperties;
  const VkFormat bf = g_create_info.imageFormat;
  g_hudless_format.store(!g_hudless_on || !hl_fns                    ? 0u
                         : bf == VK_FORMAT_R8G8B8A8_UNORM            ? 1u
                         : bf == VK_FORMAT_A2B10G10R10_UNORM_PACK32  ? 2u
                         : bf == VK_FORMAT_B8G8R8A8_UNORM            ? 3u
                                                                     : 0u,
                         std::memory_order_relaxed);
  if (g_hudless_on && !g_hudless_format.load(std::memory_order_relaxed))
    REXLOG_WARN("gow2_frame_generation_hudless: Vulkan: back buffer format {} not handled{}; no HUD-less image",
                uint32_t(bf), hl_fns ? "" : " (device functions missing)");
  g_inputs_wanted.store(true, std::memory_order_relaxed);
  REXLOG_INFO("gow2_frame_generation: Vulkan: AMD frame-generation swap chain made; frame generation on its first "
              "scene frame");
  return true;
}

// Presenter thread, just before AMD's present (SDK patch 69).
void BeforePresent(void* guest_output, const int32_t* rect) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_swap_chain_ctx) return;
  const Inputs* in = nullptr;
  for (const Inputs& i : g_inputs)
    if (guest_output && i.resource == guest_output && i.serial && (!in || i.serial > in->serial)) in = &i;
  const bool new_image = in && in->serial != g_last_presented;
  if (in && !new_image) ++g_repaints;  // presented anyway on Vulkan (see the top)
  if (in) g_last_presented = in->serial;
  ++g_frame_id;
  ++g_presents;
  XenosFgFrame f{};
  bool have = false;
  if (in && in->has && new_image) {
    f = in->frame;
    if (g_last_shown_frame && f.frame != g_last_shown_frame + 1) f.reset = true;
    g_last_shown_frame = f.frame;
    g_last_frame = f;
    g_have_last_frame = true;
    g_no_scene_streak = 0;
    have = true;
  } else if (g_have_last_frame && ++g_no_scene_streak <= kNoSceneFramesBeforeOff) {
    f = g_last_frame;
    f.reset = true;
    have = true;
    if (in && new_image) g_last_shown_frame = 0;
  }
  const int32_t debug = REXCVAR_GET(gow2_fg_debug);
  const uint32_t flags = uint32_t(debug > 0 ? debug : 0) &
                         (FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS |
                          FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES);
  bool generate = have && f.depth && f.motion && EnsurePrepareObjects() && EnsureFrameGeneration(f);
  if (!g_fg_ctx) {
    ++g_paused;
  } else if (!generate && !g_gen_on) {
    ++g_paused;  // paused and already off: nothing to tell AMD
  } else {
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = reinterpret_cast<void*>(g_swap_chain);
    c.frameGenerationCallback = &GenerateCallback;
    c.frameGenerationEnabled = generate;
    c.allowAsyncWorkloads = false;
    c.flags = flags;
    c.onlyPresentGenerated = false;
    if (rect && rect[2] > 0 && rect[3] > 0) c.generationRect = {rect[0], rect[1], rect[2], rect[3]};
    else c.generationRect = {0, 0, int32_t(g_create_info.imageExtent.width), int32_t(g_create_info.imageExtent.height)};
    c.frameID = g_frame_id;
    // HUD-less colour: when the renderer made one for this frame at exactly the generation rectangle's size
    int32_t hl_x = -1, hl_y = -1;
    const auto& gr = c.generationRect;
    if (g_hudless_on && gr.width > 0 && gr.height > 0 && gr.width < 65536 && gr.height < 65536)
      g_hl_rect_size.store((uint32_t(gr.width) << 16) | uint32_t(gr.height), std::memory_order_relaxed);
    if (g_hudless_on && generate && g_hudless_format.load(std::memory_order_relaxed)) {
      const bool fits = f.hudless && int32_t(f.hudless_width) == gr.width && int32_t(f.hudless_height) == gr.height &&
                        gr.left >= 0 && gr.top >= 0 && uint32_t(gr.left + gr.width) <= g_create_info.imageExtent.width &&
                        uint32_t(gr.top + gr.height) <= g_create_info.imageExtent.height;
      if (!f.hudless) ++g_hl_missing;
      else if (!fits) ++g_hl_mismatch;
      if (fits && EnsureHudless()) {
        hl_x = gr.left, hl_y = gr.top;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = g_hl_format;
        ci.extent = {g_hl_w, g_hl_h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        c.HUDLessColor = ffxApiGetResourceVK(reinterpret_cast<void*>(g_hl_image),
                                             ffxApiGetImageResourceDescriptionVK(g_hl_image, ci, 0),
                                             FFX_API_RESOURCE_STATE_COMPUTE_READ);
        ++g_hl_used;
      }
    }
    if (g_fn.Configure(&g_fg_ctx, &c.header) != FFX_API_RETURN_OK) generate = false;
    if (generate && !Prepare(f, flags, hl_x, hl_y)) {
      ++g_prepare_failed;
      c.frameGenerationEnabled = false;
      g_fn.Configure(&g_fg_ctx, &c.header);
      generate = false;
    }
    if (generate) {
      ++g_generated;
      g_resets += f.reset ? 1 : 0;
    } else {
      ++g_paused;
    }
  }
  g_gen_on = generate;
  if (g_presents >= 600) {
    REXLOG_INFO("gow2_frame_generation: Vulkan: {} frames presented: generation on {} ({} resets), paused {} (no scene "
                "or not ready), Prepare failed {}, window repaints presented {}", g_presents, g_generated, g_resets,
                g_paused, g_prepare_failed, g_repaints);
    if (g_hudless_on)
      REXLOG_INFO("gow2_frame_generation_hudless: Vulkan: HUD-less image given {}, none made {}, size not the "
                  "generation rectangle's {}", g_hl_used, g_hl_missing, g_hl_mismatch);
    g_hl_used = g_hl_missing = g_hl_mismatch = 0;
    g_presents = g_generated = g_resets = g_paused = g_prepare_failed = g_repaints = 0;
  }
}

}  // namespace fgvk

bool XenosFgVkActive() { return fgvk::g_active.load(std::memory_order_relaxed); }
bool XenosFgVkWanted() { return fgvk::g_inputs_wanted.load(std::memory_order_relaxed); }
uint32_t XenosFgVkHudlessFormat() {
  return fgvk::g_inputs_wanted.load(std::memory_order_relaxed) ? fgvk::g_hudless_format.load(std::memory_order_relaxed)
                                                                : 0u;
}
bool XenosFgVkHudlessSize(uint32_t& width, uint32_t& height) {
  const uint32_t v = fgvk::g_hl_rect_size.load(std::memory_order_relaxed);
  width = v >> 16, height = v & 0xFFFFu;
  return v != 0;
}

void XenosFgVkSetInputs(void* guest_output_image, const XenosFgFrame* frame) {
  if (!guest_output_image) return;
  std::lock_guard<std::mutex> lock(fgvk::g_mutex);
  fgvk::Inputs* slot = nullptr;
  for (fgvk::Inputs& i : fgvk::g_inputs)
    if (i.resource == guest_output_image) slot = &i;
  if (!slot) {
    slot = &fgvk::g_inputs[0];
    for (fgvk::Inputs& i : fgvk::g_inputs)
      if (i.serial < slot->serial) slot = &i;
  }
  slot->resource = guest_output_image;
  slot->has = frame != nullptr;
  if (frame) slot->frame = *frame;
  slot->serial = ++fgvk::g_serial;
}

bool XenosFgVkInstall(bool hudless) {
  rex::FrameHooks& h = rex::GetFrameHooks();
  h.vk_all_queues.store(true, std::memory_order_release);  // before the Vulkan device is made (OnPostInitLogging)
  h.vk_create_swap_chain.store(&fgvk::MakeSwapChain, std::memory_order_release);
  h.vk_before_present.store(&fgvk::BeforePresent, std::memory_order_release);
  fgvk::g_inputs_wanted.store(true, std::memory_order_relaxed);
  fgvk::g_active.store(true, std::memory_order_relaxed);
  fgvk::g_hudless_on = hudless;  // the image's format is known once the swap chain is made
  REXLOG_INFO("gow2_frame_generation: Vulkan: AMD FSR frame generation (experimental, notes/83; SDK patch 69 hooks "
              "installed)");
  return true;
}

#else  // !GN_FG_VK

bool XenosFgVkActive() { return false; }
bool XenosFgVkWanted() { return false; }
uint32_t XenosFgVkHudlessFormat() { return 0u; }
bool XenosFgVkHudlessSize(uint32_t&, uint32_t&) { return false; }
void XenosFgVkSetInputs(void*, const XenosFgFrame*) {}
bool XenosFgVkInstall(bool) {
  REXLOG_WARN("gow2_frame_generation: this build has no Vulkan frame generation (the FidelityFX SDK's Vulkan headers, "
              "the Vulkan headers or SDK patch 69 were missing when built); off");
  return false;
}

#endif  // GN_FG_VK
