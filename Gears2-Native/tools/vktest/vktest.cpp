// gn_vktest: checks of the Vulkan backend (renderer/vulkan_backend.cpp) without a game - device, textures, clears,
// uploads, the guest-memory buffer, readbacks, submission (stage V1); shaders, pipelines, descriptors and draws (V2);
// compute (V4); on someone else's device + the present blit (V3). Runs on any Vulkan 1.3 device
// (lavapipe for the local checks, the real GPU in a PC job). Exit code 0 = every check passed. Our code (MIT).
// Usage: gn_vktest [--validation]
#include <xenos/renderer/gpu.h>
#include <xenos/renderer/shader_cache.h>  // patch 0114: CreateDxcCompiler

#include "vktest_spirv.h"

#if GN_HAVE_VULKAN_HEADERS  // V3 check: an external device (as the SDK's provider) + PresentPacked
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <mutex>
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
#endif

#include <chrono>
#include <cmath>
#include <thread>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace xenos::renderer::gpu;

namespace xenos::renderer::gpu { std::unique_ptr<Backend> CreateVulkanBackend(); }

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                    \
  do {                                                                      \
    if (cond) { ++g_pass; } else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } \
  } while (0)

static const uint8_t* WaitData(Backend& b, ReadbackHandle h, uint32_t& pitch) {
  b.WaitForFence(b.Submit());
  return b.ReadbackData(h, pitch);
}

#if GN_HAVE_VULKAN_HEADERS
// Notes/68 V3: what the SDK's Vulkan provider does (its own instance, device and queue, the queue under a recursive
// mutex), then our backend on that device draws packed present words into a presenter-style image
// (A2B10G10R10_UNORM_PACK32, colour attachment + sampled + transfer source); we read the image back ourselves.
static void ExternalDeviceCheck() {
#ifdef _WIN32
  void* lib = reinterpret_cast<void*>(LoadLibraryA("vulkan-1.dll"));
  auto gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(reinterpret_cast<HMODULE>(lib), "vkGetInstanceProcAddr")) : nullptr;
#else
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  auto gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")) : nullptr;
#endif
  CHECK(gipa != nullptr, "V3: loader");
  if (!gipa) return;
#define L(i, f) auto f = reinterpret_cast<PFN_##f>(gipa(i, #f))
  L(nullptr, vkCreateInstance);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_4;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance inst = VK_NULL_HANDLE;
  CHECK(vkCreateInstance(&ici, nullptr, &inst) == VK_SUCCESS, "V3: instance");
  if (!inst) return;
  L(inst, vkEnumeratePhysicalDevices); L(inst, vkGetPhysicalDeviceProperties); L(inst, vkGetPhysicalDeviceQueueFamilyProperties);
  L(inst, vkCreateDevice); L(inst, vkGetDeviceProcAddr); L(inst, vkDestroyInstance); L(inst, vkGetPhysicalDeviceMemoryProperties);
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(inst, &n, devs.data());
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  int best = -1;
  for (VkPhysicalDevice d : devs) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(d, &p);
    const int sc = p.apiVersion < VK_API_VERSION_1_3 ? -1 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3 : 1;
    if (sc > best) { best = sc; pd = d; }
  }
  CHECK(pd != VK_NULL_HANDLE, "V3: device");
  if (!pd) { vkDestroyInstance(inst, nullptr); return; }
  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
  uint32_t fam = 0;
  for (uint32_t i = 0; i < qn; ++i) if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { fam = i; break; }
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f12.timelineSemaphore = VK_TRUE;
  f12.pNext = &f13;
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f12;
  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = fam;
  qi.queueCount = 1;
  qi.pQueuePriorities = &prio;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.pNext = &f2;
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  VkDevice dev = VK_NULL_HANDLE;
  CHECK(vkCreateDevice(pd, &di, nullptr, &dev) == VK_SUCCESS, "V3: vkCreateDevice");
  if (!dev) { vkDestroyInstance(inst, nullptr); return; }
#define D(f) auto f = reinterpret_cast<PFN_##f>(vkGetDeviceProcAddr(dev, #f))
  D(vkGetDeviceQueue); D(vkDestroyDevice); D(vkCreateImage); D(vkDestroyImage); D(vkGetImageMemoryRequirements);
  D(vkAllocateMemory); D(vkFreeMemory); D(vkBindImageMemory); D(vkCreateImageView); D(vkDestroyImageView);
  D(vkCreateBuffer); D(vkDestroyBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkMapMemory);
  D(vkCreateCommandPool); D(vkDestroyCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
  D(vkEndCommandBuffer); D(vkCmdPipelineBarrier); D(vkCmdCopyImageToBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle);
  VkQueue q = VK_NULL_HANDLE;
  vkGetDeviceQueue(dev, fam, 0, &q);
  static std::recursive_mutex qmutex;
  VulkanExternal ext;
  ext.instance = inst;
  ext.physical_device = pd;
  ext.device = dev;
  ext.queue_family = fam;
  ext.queue = q;
  ext.lock = [](void*) { qmutex.lock(); };
  ext.unlock = [](void*) { qmutex.unlock(); };
  {
    BackendOptions o;
    o.upload_ring_mb = 16;
    o.guest_memory_bytes = 16ull << 20;
    o.vulkan_external = &ext;
    auto b = CreateVulkanBackend();
    std::string err;
    const bool init = b->Init(o, err);
    CHECK(init, "V3: backend on an external device: %s", err.c_str());
    if (init) {
      // Packed words: R = x * 100, G = y * 200, B = 1023 - x, on an 8x4 source.
      TextureDesc wd;
      wd.width = 8; wd.height = 4; wd.format = Format::kR32Uint; wd.usage = kUsageSampled | kUsageStorage;
      const TextureHandle src = b->CreateTexture(wd);
      uint32_t words[32];
      for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 8; ++x) words[y * 8 + x] = (x * 100) | ((y * 200) << 10) | ((1023 - x) << 20);
      b->UploadTexture(src, 0, 0, words, 32, 4, 1);
      // The presenter-style image and a host buffer for its read-back.
      VkPhysicalDeviceMemoryProperties mp;
      vkGetPhysicalDeviceMemoryProperties(pd, &mp);
      auto mem_type = [&](uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        return 0u;
      };
      VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ii.imageType = VK_IMAGE_TYPE_2D;
      ii.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      ii.extent = {8, 4, 1};
      ii.mipLevels = ii.arrayLayers = 1;
      ii.samples = VK_SAMPLE_COUNT_1_BIT;
      ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      VkImage img = VK_NULL_HANDLE;
      vkCreateImage(dev, &ii, nullptr, &img);
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(dev, img, &mr);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = mr.size;
      ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VkDeviceMemory im = VK_NULL_HANDLE;
      vkAllocateMemory(dev, &ai, nullptr, &im);
      vkBindImageMemory(dev, img, im, 0);
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = img;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = ii.format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view = VK_NULL_HANDLE;
      vkCreateImageView(dev, &vi, nullptr, &view);
      VulkanPresentTarget pt;
      pt.image = img; pt.view = view; pt.width = 8; pt.height = 4; pt.first_write = true;
      CHECK(b->PresentPacked(src, 8, 4, &pt), "V3: PresentPacked (first write)");
      b->WaitForFence(b->Submit());
      pt.first_write = false;  // again from SHADER_READ_ONLY_OPTIMAL, as every later frame
      CHECK(b->PresentPacked(src, 8, 4, &pt), "V3: PresentPacked (again)");
      b->WaitForFence(b->Submit());
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      bi.size = 8 * 4 * 4;
      bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      VkBuffer buf = VK_NULL_HANDLE;
      vkCreateBuffer(dev, &bi, nullptr, &buf);
      vkGetBufferMemoryRequirements(dev, buf, &mr);
      ai.allocationSize = mr.size;
      ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VkDeviceMemory bm = VK_NULL_HANDLE;
      vkAllocateMemory(dev, &ai, nullptr, &bm);
      vkBindBufferMemory(dev, buf, bm, 0);
      VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pci.queueFamilyIndex = fam;
      VkCommandPool pool = VK_NULL_HANDLE;
      vkCreateCommandPool(dev, &pci, nullptr, &pool);
      VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cai.commandPool = pool;
      cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cai.commandBufferCount = 1;
      VkCommandBuffer cb = VK_NULL_HANDLE;
      vkAllocateCommandBuffers(dev, &cai, &cb);
      VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vkBeginCommandBuffer(cb, &cbi);
      VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      ib.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      ib.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // PresentPacked's promise
      ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      ib.image = img;
      ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
      VkBufferImageCopy rg{};
      rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      rg.imageExtent = {8, 4, 1};
      vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rg);
      vkEndCommandBuffer(cb);
      VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cb;
      {
        std::lock_guard<std::recursive_mutex> lock(qmutex);
        vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(q);
      }
      void* m = nullptr;
      vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, &m);
      const auto* got = static_cast<const uint32_t*>(m);
      bool same = got != nullptr;
      uint32_t bad = 0;
      for (uint32_t i = 0; same && i < 32; ++i) {
        // A2B10G10R10: R in bits 0-9 like our words; alpha 3 (1023/1023 -> 3 in 2 bits).
        const uint32_t want = (words[i] & 0x3FFFFFFFu) | (3u << 30);
        if (got[i] != want) { same = false; bad = i; }
      }
      CHECK(same, "V3: presenter image after PresentPacked: word %u is %08X, want %08X", bad, got ? got[bad] : 0u,
            (words[bad] & 0x3FFFFFFFu) | (3u << 30));
      std::string why;
      CHECK(!b->IsLost(&why), "V3: lost: %s", why.c_str());
      b.reset();  // waits for its own work only; the device stays ours
      vkDestroyCommandPool(dev, pool, nullptr);
      vkDestroyBuffer(dev, buf, nullptr);
      vkFreeMemory(dev, bm, nullptr);
      vkDestroyImageView(dev, view, nullptr);
      vkDestroyImage(dev, img, nullptr);
      vkFreeMemory(dev, im, nullptr);
    }
  }
  vkDestroyDevice(dev, nullptr);
  vkDestroyInstance(inst, nullptr);
#undef D
#undef L
}
#endif

// Patch 0079: the Vulkan pipeline cache is saved next to D3D12's library name (pipelines-d3d12.bin ->
// pipelines-vulkan.bin) and loaded again; a damaged file must not stop the backend. Build times reach the stats.
static void PipelineCacheCheck() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("gn_vktest_cache_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  const fs::path file = dir / "pipelines-vulkan.bin";
  auto run = [&](const char* what, bool expect_built) {
    BackendOptions o;
    o.upload_ring_mb = 16;
    o.guest_memory_bytes = 16ull << 20;
    o.pipeline_library_path = (dir / "pipelines-d3d12.bin").string();
    auto b = CreateVulkanBackend();
    std::string err;
    const bool init = b->Init(o, err);
    CHECK(init, "pipeline cache (%s): init %s", what, err.c_str());
    if (!init) return;
    GraphicsPipelineDesc pd;
    pd.vs = b->CreateShader(ShaderStage::kVertex, k_vs_full, sizeof(k_vs_full));
    pd.ps = b->CreateShader(ShaderStage::kPixel, k_ps_const, sizeof(k_ps_const));
    pd.color_count = 1; pd.color_formats[0] = Format::kR8G8B8A8Unorm; pd.color_write_masks[0] = 0xF;
    bool pending = false;
    b->GetGraphicsPipeline(pd, true, &pending);  // as the game asks: built by a worker
    PipelineHandle pl = 0;
    for (int i = 0; i < 400 && !(pl = b->GetGraphicsPipeline(pd, true, &pending)); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(pl != 0, "pipeline cache (%s): pipeline", what);
    for (int i = 0; i < 200 && b->Stats().pipelines_built_async < 1; ++i) {
      b->WaitForFence(b->Submit());
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    b->WaitForFence(b->Submit());
    const BackendStats st = b->Stats();
    if (expect_built)
      CHECK(st.pipelines_built_async >= 1 && st.pipeline_async_ms > 0.0 && st.pipeline_async_max_ms > 0.0 &&
                st.pipeline_async_max_ms <= st.pipeline_async_ms + 1e-9 && st.pipeline_async_bytes > 0,
            "pipeline cache (%s): build stats %llu built, %.3f ms, max %.3f ms, %llu bytes", what,
            (unsigned long long)st.pipelines_built_async, st.pipeline_async_ms, st.pipeline_async_max_ms,
            (unsigned long long)st.pipeline_async_bytes);
    b.reset();
    std::error_code ec;
    CHECK(fs::exists(file) && fs::file_size(file, ec) > 16 && !fs::exists(dir / "pipelines-vulkan.bin.tmp"),
          "pipeline cache (%s): saved %s", what, file.string().c_str());
  };
  run("cold", true);
  run("warm", true);
  {
    std::FILE* f = std::fopen(file.string().c_str(), "wb");
    if (f) { std::fputs("not a pipeline cache at all, just junk bytes for the loader", f); std::fclose(f); }
  }
  run("damaged file", true);
  std::error_code ec;
  fs::remove_all(dir, ec);  // our own temporary folder
}

// Patch 0087 (--gow2_renderer_vulkan_compressible_targets=lazy): a colour render target starts as a one-format image
// (compressible) and is remade mutable, its contents kept, the first time a signed view of it is sampled; after that it
// still reads and renders as before.
static void LazyTargetsCheck() {
  BackendOptions o;
  o.upload_ring_mb = 16;
  o.guest_memory_bytes = 16ull << 20;
  o.vk_mutable_targets = 1;
  auto b = CreateVulkanBackend();
  std::string err;
  const bool init = b->Init(o, err);
  CHECK(init, "lazy targets: init %s", err.c_str());
  if (!init) return;
  TextureDesc d;
  d.width = 4; d.height = 2; d.format = Format::kR8G8B8A8Unorm; d.usage = kUsageSampled | kUsageRenderTarget;
  const TextureHandle src = b->CreateTexture(d), dst = b->CreateTexture(d);
  const float c[4] = {0.25f, 0.0f, 1.0f, 1.0f};  // bytes 64, 0, 255, 255
  b->ClearColor(src, c);
  GraphicsPipelineDesc pd;
  pd.vs = b->CreateShader(ShaderStage::kVertex, k_vs_full, sizeof(k_vs_full));
  pd.ps = b->CreateShader(ShaderStage::kPixel, k_ps_tex, sizeof(k_ps_tex));
  pd.color_count = 1; pd.color_formats[0] = Format::kR8G8B8A8Unorm; pd.color_write_masks[0] = 0xF;
  const PipelineHandle pl = b->GetGraphicsPipeline(pd);
  CHECK(src && dst && pl, "lazy targets: setup %u %u %u", src, dst, pl);
  TextureView views[96] = {};
  SamplerDesc smp[32] = {};
  views[3].texture = src;
  views[3].signed_view = true;
  smp[3].address[0] = smp[3].address[1] = SamplerAddress::kClamp;
  struct { float rect[4]; float depth[4]; } r{{-1, -1, 1, 1}, {0.5f, 0, 0, 0}};
  const float tsize[4] = {4, 2, 0, 0};
  auto draw = [&]() {
    b->SetTextures(views, smp, 96, 32);
    b->SetRenderTargets(1, &dst, 0);
    b->SetViewport(Viewport{0, 0, 4, 2, 0, 1});
    b->SetScissor(Rect{0, 0, 4, 2});
    b->SetPipeline(pl);
    b->SetConstants(ShaderStage::kVertex, 1, &r, sizeof(r));
    b->SetConstants(ShaderStage::kPixel, 0, tsize, 16);
    b->Draw(6);
    const ReadbackHandle h = b->ReadbackTexture(dst, Rect{0, 0, 4, 2});
    b->WaitForFence(b->Submit());
    uint32_t pitch = 0;
    const uint8_t* q = WaitData(*b, h, pitch);
    const uint32_t v = q ? uint32_t(q[0]) | q[1] << 8 | q[2] << 16 | uint32_t(q[3]) << 24 : 0xDEADBEEFu;
    b->ReleaseReadback(h);
    return v;
  };
  // Signed view: 64 -> 64/127 = 0.504 -> 128 or 129; 0xFF -> -1/127 -> 0 in the UNORM target.
  const uint32_t sv = draw();
  CHECK((sv == 0x00000080u || sv == 0x00000081u) && b->Stats().vk_targets_made_mutable == 1,
        "lazy targets: signed view of the remade image %08X, made mutable %llu", sv,
        (unsigned long long)b->Stats().vk_targets_made_mutable);
  views[3].signed_view = false;
  const uint32_t pv = draw();
  CHECK(pv == 0xFFFF0040u && b->Stats().vk_targets_made_mutable == 1, "lazy targets: plain view after %08X", pv);
}

// Patch 0088 (gn_vktest --bench, on the real GPU): what robustBufferAccess (the SDK's device turns it on with GPU
// emulation) and NoContraction (our DXBC -> SPIR-V marks all float maths) cost a constant- and buffer-heavy pixel shader
// - the suspects for Vulkan's ~2x GPU time on ch15's colour targets (job 391: compression ruled out). 2560x1440
// RGBA16F, 40 full-screen draws per measurement, GPU time from the backend's timestamps.
// Patch 0104: the driver's statistics (registers, instructions, ...) of a graphics pipeline for every pixel shader
// SPIR-V file (*.spv) in a folder - e.g. the 0099 dump of the game's shaders - with a full-screen vertex shader and one
// RGBA16F target. Our own device with VK_KHR_pipeline_executable_properties; the game's device (the SDK's) has none.
static int ShaderStats(const char* dir) {
  BackendOptions o;
  o.upload_ring_mb = 16;
  o.guest_memory_bytes = 16ull << 20;
  o.vk_pipeline_stats = true;
  auto b = CreateVulkanBackend();
  std::string err;
  if (!b->Init(o, err)) { std::printf("vkstats: init failed: %s\n", err.c_str()); return 1; }
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir))
    if (e.path().extension() == ".spv") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  const ShaderHandle vs = b->CreateShader(ShaderStage::kVertex, k_vs_full, sizeof(k_vs_full));
  int done = 0, failed = 0;
  for (const auto& f : files) {
    std::ifstream in(f, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    if (bytes.size() < 20 || bytes.size() % 4) { ++failed; continue; }
    uint32_t model = 0;  // OpEntryPoint's execution model: 4 = fragment
    const uint32_t* w = reinterpret_cast<const uint32_t*>(bytes.data());
    for (size_t i = 5; i < bytes.size() / 4;) {
      const uint32_t n = w[i] >> 16, op = w[i] & 0xFFFF;
      if (!n) break;
      if (op == 15 && i + 1 < bytes.size() / 4) { model = w[i + 1]; break; }
      i += n;
    }
    if (model != 4) continue;
    const ShaderHandle ps = b->CreateShader(ShaderStage::kPixel, bytes.data(), bytes.size());
    GraphicsPipelineDesc pd;
    pd.vs = vs; pd.ps = ps;
    pd.color_count = 1; pd.color_formats[0] = Format::kR16G16B16A16Float; pd.color_write_masks[0] = 0xF;
    pd.depth_format = Format::kD24UnormS8; pd.depth_enable = true; pd.depth_write = true; pd.depth_func = CompareFunc::kGreaterEqual;
    const PipelineHandle pl = ps ? b->GetGraphicsPipeline(pd) : 0;
    const std::string st = pl ? b->PipelineStatistics(pl) : std::string();
    if (st.empty()) { ++failed; std::printf("vkstats: %s: no statistics\n", f.filename().string().c_str()); continue; }
    ++done;
    std::printf("vkstats: %s %zu bytes: %s\n", f.filename().string().c_str(), bytes.size(), st.c_str());
  }
  std::printf("vkstats: %d pipelines with statistics, %d without\n", done, failed);
  return done ? 0 : 1;
}
static void Bench() {
  for (int robust = 0; robust < 2; ++robust) {
    BackendOptions o;
    o.upload_ring_mb = 64;
    o.guest_memory_bytes = 64ull << 20;
    o.vk_robust_buffer_access = robust != 0;
    auto b = CreateVulkanBackend();
    std::string err;
    if (!b->Init(o, err)) { std::printf("vkbench: init failed: %s\n", err.c_str()); return; }
    TextureDesc d;
    d.width = 2560; d.height = 1440; d.format = Format::kR16G16B16A16Float; d.usage = kUsageSampled | kUsageRenderTarget;
    const TextureHandle rt = b->CreateTexture(d);
    std::vector<uint8_t> g(65536);
    for (size_t i = 0; i < g.size(); i += 4) { const float f = 0.001f * float(i % 97); std::memcpy(&g[i], &f, 4); }
    b->UploadGuestMemory(0, g.data(), uint32_t(g.size()));
    float cst[256 * 4];
    for (int i = 0; i < 256 * 4; ++i) cst[i] = 0.5f + 0.001f * float(i % 13);
    const float size[4] = {2560, 1440, 1.0f, 0};
    struct { float rect[4]; float depth[4]; } r{{-1, -1, 1, 1}, {0.5f, 0, 0, 0}};
    const ShaderHandle vs = b->CreateShader(ShaderStage::kVertex, k_vs_full, sizeof(k_vs_full));
    const ShaderHandle ps[2] = {b->CreateShader(ShaderStage::kPixel, k_ps_bench, sizeof(k_ps_bench)),
                                b->CreateShader(ShaderStage::kPixel, k_ps_bench_precise, sizeof(k_ps_bench_precise))};
    for (int precise = 0; precise < 2; ++precise) {
      GraphicsPipelineDesc pd;
      pd.vs = vs; pd.ps = ps[precise];
      pd.color_count = 1; pd.color_formats[0] = Format::kR16G16B16A16Float; pd.color_write_masks[0] = 0xF;
      const PipelineHandle pl = b->GetGraphicsPipeline(pd);
      if (!pl) { std::printf("vkbench: no pipeline\n"); return; }
      double best = 1e9;
      for (int round = 0; round < 4; ++round) {  // round 0 warms up; the best of the other three
        b->SetRenderTargets(1, &rt, 0);
        b->SetViewport(Viewport{0, 0, 2560, 1440, 0, 1});
        b->SetScissor(Rect{0, 0, 2560, 1440});
        b->SetTextures(nullptr, nullptr);
        const double before = b->Stats().gpu_ms;
        for (int k = 0; k < 40; ++k) {
          b->SetPipeline(pl);
          b->SetConstants(ShaderStage::kVertex, 1, &r, sizeof(r));
          b->SetConstants(ShaderStage::kPixel, 0, size, 16);
          b->SetConstants(ShaderStage::kPixel, 1, cst, sizeof(cst));
          b->Draw(6);
        }
        b->WaitForFence(b->Submit());
        b->WaitForFence(b->Submit());  // the timestamps of the first are read once it has retired
        const double ms = (b->Stats().gpu_ms - before) / 40.0;
        if (round > 0 && ms > 0.0) best = std::min(best, ms);
      }
      std::printf("vkbench: robustBufferAccess %s, %s maths: %.4f ms per full-screen draw\n", robust ? "ON " : "off",
                  precise ? "precise (NoContraction)" : "fusable               ", best);
    }
  }
}

// Patch 0110 (--overlap-bench): do our backend's draws overlap on this GPU? Jobs 415-419: with a GPU timestamp per
// draw, ch15 takes ~6.3 ms a frame on both APIs; normally D3D12 takes 4.3 ms (its draws overlap) and Vulkan the same
// 6.3. Here: 880 small quads (64x64 px, the screen tiled) in one pass, in several ways - if the per-draw time stays at
// the single-draw time, consecutive draws don't overlap.
// Patch 0114: the bench's shaders as HLSL too, compiled with DXC for the D3D12 backend (--overlap-bench-d3d12), so the
// same cases run on both APIs (copies of tools/vktest/*.hlsl).
static const char* const kHlsl_vs_full = R"HLSL(// A rectangle from 6 vertices (two triangles): x0, y0, x1, y1 in NDC from the vertex shader's b1, depth in b1[1].x.
cbuffer XeVsRect : register(b1) { float4 rect; float4 depth; };
float4 main(uint id : SV_VertexID) : SV_Position {
  const uint corner[6] = {0u, 1u, 2u, 2u, 1u, 3u};
  uint c = corner[id % 6u];
  float2 uv = float2(float(c & 1u), float(c >> 1u));
  return float4(lerp(rect.xy, rect.zw, uv), depth.x, 1.0);
}
)HLSL";
static const char* const kHlsl_ps_const = R"HLSL(cbuffer XePsColour : register(b0) { float4 colour; };
float4 main(float4 pos : SV_Position) : SV_Target0 { return colour; }
)HLSL";
static const char* const kHlsl_ps_bench = R"HLSL(// Patch 0088 benchmark (gn_vktest --bench): a constant- and buffer-heavy pixel shader, like the game's - 64 dynamically
// indexed float4 constants (b1) and 64 guest-memory loads (t96) per pixel; BENCH_PRECISE marks the sum precise (the
// SPIR-V gets NoContraction, as our DXBC -> SPIR-V shaders do everywhere).
cbuffer XePsColour : register(b0) { float4 size; };
cbuffer XeBench : register(b1) { float4 c[256]; };
[[vk::binding(96, 1)]] ByteAddressBuffer xe_shared_memory : register(t96, space0);
float4 main(float4 pos : SV_Position) : SV_Target0 {
  uint i = uint(pos.x) & 255u;
#ifdef BENCH_PRECISE
  precise float4 acc = 0.0;
#else
  float4 acc = 0.0;
#endif
  [loop] for (uint k = 0u; k < 64u; ++k) {
    float4 a = c[(i + k * 7u) & 255u];
    float4 g = asfloat(xe_shared_memory.Load4(((uint(pos.y) * 64u + k) & 4095u) * 16u));
    acc = acc * a + g;
  }
  return acc * size.z;
}
)HLSL";
static const char* const kHlsl_ps_tex = R"HLSL([[vk::binding(3, 1)]] Texture2D<float4> xe_tex_2d_3 : register(t3, space0);
[[vk::binding(3, 2)]] SamplerState xe_sampler_3 : register(s3, space0);
cbuffer XePsColour : register(b0) { float4 size; };
float4 main(float4 pos : SV_Position) : SV_Target0 { return xe_tex_2d_3.Sample(xe_sampler_3, pos.xy / size.xy); }
)HLSL";
static const char* const kHlsl_cs_fill = R"HLSL(// Fills mip c0.y of a storage image (u0, R32_UINT) with c0.x + x + y * 16.
RWTexture2D<uint> xe_dest : register(u0);
cbuffer XeResolveConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { xe_dest[id.xy] = c0.x + id.x + id.y * 16u; }
)HLSL";
// Patch 0118: why is a full-screen storage-image dispatch ~3x slower on Vulkan than D3D12 (job 423: 35.8 vs 11.2 us,
// barriers skipped)? The same writes as a raw buffer, as fragment draws into an R32_UINT target, and the game's
// pattern (draws, then a step that reads the target just drawn) as a dispatch and as a draw. Compiled with DXC on both.
static const char* const kHlsl_cs_buf = R"HLSL(RWByteAddressBuffer xe_guest : register(u0);
cbuffer XeResolveConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  xe_guest.Store((id.y * 2560u + id.x) * 4u, c0.x + id.x + id.y * 16u);
}
)HLSL";
static const char* const kHlsl_cs_copy = R"HLSL(Texture2D<float4> xe_src : register(t0);
RWTexture2D<uint> xe_dest : register(u0);
cbuffer XeResolveConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
  xe_dest[id.xy] = asuint(xe_src.Load(int3(id.xy, 0)).x) + c0.x;
}
)HLSL";
static const char* const kHlsl_ps_words = R"HLSL(cbuffer XePsColour : register(b0) { uint4 c0; };
uint main(float4 pos : SV_Position) : SV_Target0 { return c0.x + uint(pos.x) + uint(pos.y) * 16u; }
)HLSL";
static const char* const kHlsl_ps_copy = R"HLSL([[vk::binding(3, 1)]] Texture2D<float4> xe_tex_2d_3 : register(t3, space0);
cbuffer XePsColour : register(b0) { uint4 c0; };
uint main(float4 pos : SV_Position) : SV_Target0 { return asuint(xe_tex_2d_3.Load(int3(int2(pos.xy), 0)).x) + c0.x; }
)HLSL";
static void OverlapBench(const BackendOptions& base, bool d3d12 = false) {
  BackendOptions o = base;
  o.upload_ring_mb = 64;
  o.guest_memory_bytes = 64ull << 20;
  auto b = d3d12 ? CreateD3D12Backend() : CreateVulkanBackend();
  std::string err;
  if (!b || !b->Init(o, err)) { std::printf("vkoverlap: init failed: %s\n", err.c_str()); return; }
  std::printf("vkoverlap: %s backend - %s\n", d3d12 ? "D3D12" : "Vulkan", b->Describe().c_str());
  std::unique_ptr<xenos::renderer::ShaderCompiler> dxc;
  if (b->GetShaderIL() != ShaderIL::kSpirv) {
    dxc = xenos::renderer::CreateDxcCompiler({}, err);
    if (!dxc) { std::printf("vkoverlap: no DXC: %s\n", err.c_str()); return; }
  }
  // Patch 0118: the new cases' shaders through DXC on both APIs (no checked-in SPIR-V for them).
  std::unique_ptr<xenos::renderer::ShaderCompiler> dxc_any = xenos::renderer::CreateDxcCompiler({}, err);
  auto mkh = [&](ShaderStage st, const char* hlsl) -> ShaderHandle {
    if (!dxc_any) return 0;
    std::vector<uint8_t> il;
    std::string msg;
    if (!dxc_any->Compile(hlsl, st, b->GetShaderIL(), il, msg)) { std::printf("vkoverlap: DXC failed: %s\n", msg.c_str()); return 0; }
    return b->CreateShader(st, il.data(), il.size());
  };
  // A shader from its SPIR-V (Vulkan) or its HLSL through DXC (D3D12).
  auto mk = [&](ShaderStage st, const uint32_t* spv, size_t spv_bytes, const char* hlsl) -> ShaderHandle {
    if (!dxc) return b->CreateShader(st, spv, spv_bytes);
    std::vector<uint8_t> il;
    std::string msg;
    if (!dxc->Compile(hlsl, st, b->GetShaderIL(), il, msg)) { std::printf("vkoverlap: DXC failed: %s\n", msg.c_str()); return 0; }
    return b->CreateShader(st, il.data(), il.size());
  };
  TextureDesc d;
  d.width = 2560; d.height = 1440; d.format = Format::kR16G16B16A16Float; d.usage = kUsageSampled | kUsageRenderTarget;
  const TextureHandle rt = b->CreateTexture(d), rt2 = b->CreateTexture(d);  // 0111: rt2 = a second target / a source
  TextureDesc sd = d;
  sd.usage = kUsageSampled;
  const TextureHandle tex = b->CreateTexture(sd), tex2 = b->CreateTexture(sd);  // 0111 / 0112: plain sampled textures
  {
    std::vector<uint16_t> px(size_t(2560) * 1440 * 4, 0x3C00);  // 1.0 in float16
    b->UploadTexture(tex, 0, 0, px.data(), 2560 * 8, 1440, 1);
    b->UploadTexture(tex2, 0, 0, px.data(), 2560 * 8, 1440, 1);
  }
  TextureDesc dd;  // patch 0112: a depth target
  dd.width = 2560; dd.height = 1440; dd.format = Format::kD24UnormS8; dd.usage = kUsageDepthStencil | kUsageSampled;
  const TextureHandle ds = b->CreateTexture(dd);
  float cst[256 * 4];
  for (int i = 0; i < 256 * 4; ++i) cst[i] = 0.5f + 0.001f * float(i % 13);
  const float size[4] = {2560, 1440, 1.0f, 0};
  const float colour[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  const ShaderHandle ps_tex = mk(ShaderStage::kPixel, k_ps_tex, sizeof(k_ps_tex), kHlsl_ps_tex);
  TextureView views[96] = {};
  SamplerDesc smp[32] = {};
  smp[3].address[0] = smp[3].address[1] = SamplerAddress::kClamp;
  const ShaderHandle vs = mk(ShaderStage::kVertex, k_vs_full, sizeof(k_vs_full), kHlsl_vs_full);
  const ShaderHandle ps_cheap = mk(ShaderStage::kPixel, k_ps_const, sizeof(k_ps_const), kHlsl_ps_const);
  const ShaderHandle ps_heavy = mk(ShaderStage::kPixel, k_ps_bench, sizeof(k_ps_bench), kHlsl_ps_bench);
  auto pipe = [&](ShaderHandle ps, bool blend) {
    GraphicsPipelineDesc pd;
    pd.vs = vs; pd.ps = ps;
    pd.color_count = 1; pd.color_formats[0] = Format::kR16G16B16A16Float; pd.color_write_masks[0] = blend ? 0x7 : 0xF;
    return b->GetGraphicsPipeline(pd);
  };
  const PipelineHandle pc = pipe(ps_cheap, false), pc2 = pipe(ps_cheap, true), ph = pipe(ps_heavy, false),
                       pt = pipe(ps_tex, false);
  GraphicsPipelineDesc dpd;  // patch 0112: the cheap shader with a depth attachment, depth test and write
  dpd.vs = vs; dpd.ps = ps_cheap;
  dpd.color_count = 1; dpd.color_formats[0] = Format::kR16G16B16A16Float; dpd.color_write_masks[0] = 0xF;
  dpd.depth_format = Format::kD24UnormS8; dpd.depth_enable = dpd.depth_write = true; dpd.depth_func = CompareFunc::kAlways;
  const PipelineHandle pd_ = b->GetGraphicsPipeline(dpd);
  // Patch 0113: compute steps between groups of draws (as the game's EDRAM transfers / resolves) - a fill shader
  // writing its own storage image, so it depends on nothing the draws touch.
  const ShaderHandle csf = mk(ShaderStage::kCompute, k_cs_fill, sizeof(k_cs_fill), kHlsl_cs_fill);
  const PipelineHandle cpf = csf ? b->CreateComputePipeline(csf) : 0;
  TextureDesc wd;
  wd.width = 2560; wd.height = 1440; wd.format = Format::kR32Uint; wd.usage = kUsageSampled | kUsageStorage;
  const TextureHandle words = b->CreateTexture(wd);
  if (!cpf || !words) { std::printf("vkoverlap: no compute pipeline / storage image\n"); return; }
  // groups: 0 = the small dispatch (one 8x8 group), 1 = a big one (the whole 2560x1440 image)
  auto dispatch = [&](int k, bool big) {
    ComputeBindings fb;
    fb.constants[0] = uint32_t(k); fb.constant_count = 4;
    fb.uav_texture = words; fb.uav_mip = 0;
    b->Dispatch(cpf, fb, big ? 320 : 1, big ? 180 : 1, 1);
  };
  if (!pc || !pc2 || !ph || !pt) { std::printf("vkoverlap: no pipeline\n"); return; }
  // Patch 0118: an R32_UINT image that is also a render target, and the new cases' pipelines (0 = case skipped).
  TextureDesc wrd = wd;
  wrd.usage = kUsageSampled | kUsageStorage | kUsageRenderTarget;
  const TextureHandle words_rt = b->CreateTexture(wrd);
  const ShaderHandle cs_buf = mkh(ShaderStage::kCompute, kHlsl_cs_buf), cs_copy = mkh(ShaderStage::kCompute, kHlsl_cs_copy);
  const ShaderHandle ps_words = mkh(ShaderStage::kPixel, kHlsl_ps_words), ps_copy = mkh(ShaderStage::kPixel, kHlsl_ps_copy);
  const PipelineHandle cpb = cs_buf ? b->CreateComputePipeline(cs_buf) : 0, cpc = cs_copy ? b->CreateComputePipeline(cs_copy) : 0;
  auto upipe = [&](ShaderHandle ps) -> PipelineHandle {
    if (!ps) return 0;
    GraphicsPipelineDesc pd;
    pd.vs = vs; pd.ps = ps;
    pd.color_count = 1; pd.color_formats[0] = Format::kR32Uint; pd.color_write_masks[0] = 0xF;
    return b->GetGraphicsPipeline(pd);
  };
  const PipelineHandle pw = upipe(ps_words), pcp = upipe(ps_copy);
  std::printf("vkoverlap: 0118 cases: words_rt %u, cs_buf %u, cs_copy %u, ps_words %u, ps_copy %u\n", unsigned(words_rt),
              unsigned(cpb), unsigned(cpc), unsigned(pw), unsigned(pcp));
  const struct { float rect[4]; float depth[4]; } full{{-1, -1, 1, 1}, {0.5f, 0, 0, 0}};
  constexpr int kTx = 40, kTy = 22;  // 64x64 tiles of 2560x1408
  // mode: 0 one tile only (latency of a single draw), 1 all tiles constants per draw, 2 all tiles same constants
  // (every draw the same rect), 3 all tiles + pipeline switch every draw, 4 one full-screen draw (same pixel count);
  // patch 0111: 5 all tiles, the target switching between rt and rt2 every 8 draws (a new render pass each time),
  // 6 all tiles sampling an uploaded texture, 7 all tiles sampling rt2 after rt2 was drawn to in the same list
  auto run = [&](const char* name, PipelineHandle p, int mode, bool heavy) {
    double best = 1e9;
    int n_override = 0;
    for (int round = 0; round < 4; ++round) {
      if (mode == 7) {  // rt2 written first (a full-screen cheap draw), then sampled
        b->SetRenderTargets(1, &rt2, 0);
        b->SetViewport(Viewport{0, 0, 2560, 1440, 0, 1});
        b->SetScissor(Rect{0, 0, 2560, 1440});
        b->SetTextures(nullptr, nullptr);
        b->SetPipeline(pc);
        struct { float rect[4]; float depth[4]; } f{{-1, -1, 1, 1}, {0.5f, 0, 0, 0}};
        b->SetConstants(ShaderStage::kVertex, 1, &f, sizeof(f));
        b->SetConstants(ShaderStage::kPixel, 0, colour, 16);
        b->Draw(6);
        b->WaitForFence(b->Submit());
        b->WaitForFence(b->Submit());
        b->SetPipeline(pc);
        b->SetConstants(ShaderStage::kVertex, 1, &f, sizeof(f));
        b->Draw(6);  // in the timed list: rt2 drawn, then sampled below
      }
      b->SetRenderTargets(1, &rt, mode == 11 ? ds : 0);
      b->SetViewport(Viewport{0, 0, 2560, 1440, 0, 1});
      b->SetScissor(Rect{0, 0, 2560, 1440});
      views[3].texture = mode == 7 ? rt2 : tex;
      if (mode == 6 || mode == 7 || p == pt) b->SetTextures(views, smp, 96, 32);
      else b->SetTextures(nullptr, nullptr);
      const double before = b->Stats().gpu_ms;
      const int n = mode == 0 || mode == 4 ? 1 : kTx * kTy;
      if (mode == 14 || mode == 15) {  // patch 0113: the dispatches alone, 110 of them
        for (int k = 0; k < n; k += 8) dispatch(k, mode == 15);
        n_override = n / 8;
      }
      // Patch 0118: 16 = 110 full-screen buffer writes, 17 = 110 full-screen draws into words_rt (one pass),
      // 18 = 110 full-screen storage-image dispatches into words_rt (also a render target)
      if (mode == 16 || mode == 18) {
        for (int k = 0; k < n; k += 8) {
          ComputeBindings fb;
          fb.constants[0] = uint32_t(k); fb.constant_count = 4;
          if (mode == 16) fb.guest_u0 = true;
          else { fb.uav_texture = words_rt; fb.uav_mip = 0; }
          b->Dispatch(mode == 16 ? cpb : cpf, fb, 320, 180, 1);
        }
        n_override = n / 8;
      }
      if (mode == 17) {
        b->SetRenderTargets(1, &words_rt, 0);
        b->SetViewport(Viewport{0, 0, 2560, 1440, 0, 1});
        b->SetScissor(Rect{0, 0, 2560, 1440});
        b->SetTextures(nullptr, nullptr);
        for (int k = 0; k < n; k += 8) {
          const uint32_t c[4] = {uint32_t(k), 0, 0, 0};
          b->SetPipeline(pw);
          b->SetConstants(ShaderStage::kVertex, 1, &full, sizeof(full));
          b->SetConstants(ShaderStage::kPixel, 0, c, 16);
          b->Draw(6);
        }
        n_override = n / 8;
      }
      const bool new_case = mode >= 16 && mode <= 18;
      for (int k = 0; k < n && mode != 14 && mode != 15 && !new_case; ++k) {
        // Patch 0118: 19 = every 8 draws a full-screen dispatch that reads rt (just drawn) into words; 20 = the same step
        // as a full-screen draw into words_rt sampling rt (a pass on words_rt, then back to rt).
        if (mode == 19 && k && (k % 8) == 0) {
          ComputeBindings fb;
          fb.constants[0] = uint32_t(k); fb.constant_count = 4;
          fb.srv[0] = rt;
          fb.uav_texture = words; fb.uav_mip = 0;
          b->Dispatch(cpc, fb, 320, 180, 1);
        }
        if (mode == 20 && k && (k % 8) == 0) {
          const uint32_t c[4] = {uint32_t(k), 0, 0, 0};
          b->SetRenderTargets(1, &words_rt, 0);
          views[3].texture = rt;
          b->SetTextures(views, smp, 96, 32);
          b->SetPipeline(pcp);
          b->SetConstants(ShaderStage::kVertex, 1, &full, sizeof(full));
          b->SetConstants(ShaderStage::kPixel, 0, c, 16);
          b->Draw(6);
          b->SetRenderTargets(1, &rt, 0);
          b->SetTextures(nullptr, nullptr);
        }
        if ((mode == 12 || mode == 13) && k && (k % 8) == 0) dispatch(k, mode == 13);  // patch 0113
        if (mode == 5 && (k % 8) == 0) { const TextureHandle t = (k / 8) & 1 ? rt2 : rt; b->SetRenderTargets(1, &t, 0); }
        if (mode == 8) { views[3].texture = (k & 1) ? tex2 : tex; b->SetTextures(views, smp, 96, 32); }  // patch 0112
        if (mode == 9) {
          b->SetViewport(Viewport{float(k & 7), 0, 2560.0f - float(k & 7), 1440, 0, 1});
          b->SetScissor(Rect{0, 0, 2560 - (k & 1), 1440});
        }
        b->SetPipeline(mode == 3 && (k & 1) ? pc2 : p);
        struct { float rect[4]; float depth[4]; } r{{-1, -1, 1, 1}, {0.5f, 0, 0, 0}};
        if (mode != 4) {
          const int tx = mode == 2 ? 0 : k % kTx, ty = mode == 2 ? 0 : k / kTx;
          r.rect[0] = -1.0f + 2.0f * float(tx * 64) / 2560.0f; r.rect[2] = -1.0f + 2.0f * float(tx * 64 + 64) / 2560.0f;
          r.rect[1] = -1.0f + 2.0f * float(ty * 64) / 1440.0f; r.rect[3] = -1.0f + 2.0f * float(ty * 64 + 64) / 1440.0f;
        } else {
          r.rect[3] = -1.0f + 2.0f * 1408.0f / 1440.0f;
        }
        b->SetConstants(ShaderStage::kVertex, 1, &r, sizeof(r));
        if (heavy) { b->SetConstants(ShaderStage::kPixel, 0, size, 16); b->SetConstants(ShaderStage::kPixel, 1, cst, sizeof(cst)); }
        else if (p == pt) b->SetConstants(ShaderStage::kPixel, 0, size, 16);
        else b->SetConstants(ShaderStage::kPixel, 0, colour, 16);
        if (mode == 10) {
          static const uint32_t idx[6] = {0, 1, 2, 3, 4, 5};
          b->DrawIndexed(idx, 6);
        } else {
          b->Draw(6);
        }
      }
      b->WaitForFence(b->Submit());
      b->WaitForFence(b->Submit());
      const double ms = b->Stats().gpu_ms - before;
      if (round > 0 && ms > 0.0) best = std::min(best, ms);
      if (round == 3) {
        const int m = n_override ? n_override : n;
        std::printf("vkoverlap: %-44s %4d %s %.4f ms (%.2f us each)\n", name, m, n_override ? "dispatches" : "draws", best,
                    1000.0 * best / m);
      }
    }
  };
  for (int heavy = 0; heavy < 2; ++heavy) {
    const PipelineHandle p = heavy ? ph : pc;
    const char* sh = heavy ? "heavy PS" : "cheap PS";
    char nm[64];
    std::snprintf(nm, sizeof nm, "%s: one 64x64 draw", sh); run(nm, p, 0, heavy);
    std::snprintf(nm, sizeof nm, "%s: 880 tiles, constants per draw", sh); run(nm, p, 1, heavy);
    std::snprintf(nm, sizeof nm, "%s: 880 draws of the same tile", sh); run(nm, p, 2, heavy);
    if (!heavy) { std::snprintf(nm, sizeof nm, "%s: 880 tiles, pipeline switch per draw", sh); run(nm, p, 3, heavy); }
    std::snprintf(nm, sizeof nm, "%s: one draw of all 880 tiles' pixels", sh); run(nm, p, 4, heavy);
  }
  run("cheap PS: 880 tiles, new pass every 8 draws", pc, 5, false);       // patch 0111
  run("tex PS: one 64x64 draw (uploaded texture)", pt, 0, false);
  run("tex PS: 880 tiles, uploaded texture", pt, 6, false);
  run("tex PS: one draw of all tiles' pixels", pt, 4, false);
  {
    views[3].texture = tex;
    b->SetTextures(views, smp, 96, 32);
  }
  run("tex PS: 880 tiles, a target drawn just before", pt, 7, false);
  run("tex PS: 880 tiles, texture changes every draw", pt, 8, false);       // patch 0112
  run("cheap PS: 880 tiles, viewport / scissor every draw", pc, 9, false);
  run("cheap PS: 880 tiles, indexed draws (indices per draw)", pc, 10, false);
  run("cheap PS: 880 tiles, depth target + test + write", pd_, 11, false);
  run("cheap PS: 880 tiles, a small dispatch every 8 draws", pc, 12, false);  // patch 0113
  run("cheap PS: 880 tiles, a full-screen dispatch every 8", pc, 13, false);
  run("dispatches alone: 110 small", pc, 14, false);
  run("dispatches alone: 110 full-screen", pc, 15, false);
  run("heavy PS: 880 tiles, a full-screen dispatch every 8", ph, 13, true);
  run("heavy PS: 880 tiles, new pass every 8 draws", ph, 5, true);  // patch 0116
  if (cpb) run("0118 dispatches alone: 110 full-screen, buffer", pc, 16, false);
  if (pw && words_rt) run("0118 draws alone: 110 full-screen into R32UI", pc, 17, false);
  if (words_rt) run("0118 dispatches alone: 110 full-screen, RT image", pc, 18, false);
  if (cpc) run("0118 cheap PS 880 tiles, dispatch reads rt every 8", pc, 19, false);
  if (pcp && words_rt) run("0118 cheap PS 880 tiles, draw reads rt every 8", pc, 20, false);
  if (cpc) run("0118 heavy PS 880 tiles, dispatch reads rt every 8", ph, 19, true);
  if (pcp && words_rt) run("0118 heavy PS 880 tiles, draw reads rt every 8", ph, 20, true);
}

int main(int argc, char** argv) {
  BackendOptions o;
  o.upload_ring_mb = 16;
  o.guest_memory_bytes = 64ull << 20;
  for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--validation")) o.debug_layer = true;
  for (int i = 1; i < argc; ++i)  // patch 0089: the whole run with pass barriers only when needed (V5b)
    if (!std::strcmp(argv[i], "--when-needed")) o.vk_pass_barriers = 1, o.vk_clear_in_pass = true;  // + 0092
  for (int i = 1; i < argc; ++i)  // patch 0115
    if (!std::strcmp(argv[i], "--narrow")) o.vk_pass_barriers = 2, o.vk_clear_in_pass = true;
  for (int i = 1; i < argc; ++i)  // patch 0097
    if (!std::strcmp(argv[i], "--dedicated")) o.vk_dedicated_images = true;
  for (int i = 1; i < argc; ++i)  // patch 0098
    if (!std::strcmp(argv[i], "--count-pixels")) o.vk_count_pixels = true;
  for (int i = 1; i < argc; ++i)  // patch 0100
    if (!std::strcmp(argv[i], "--attachment-layouts")) o.vk_attachment_layouts = true;
  for (int i = 1; i < argc; ++i)  // patch 0107
    if (!std::strcmp(argv[i], "--barrier-stages")) o.vk_barrier_src_stages = true;  // patch 0107
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--bench")) { Bench(); return 0; }  // patch 0088
  for (int i = 1; i < argc; ++i)  // patch 0113: measurement mode 0108 (memory-only barriers not sent)
    if (!std::strcmp(argv[i], "--skip-memory")) o.vk_skip_memory_barriers = true;
  for (int i = 1; i < argc; ++i)  // patch 0110
    if (!std::strcmp(argv[i], "--overlap-bench")) { OverlapBench(o); return 0; }
  for (int i = 1; i < argc; ++i)  // patch 0114: the same bench on the D3D12 backend
    if (!std::strcmp(argv[i], "--overlap-bench-d3d12")) { OverlapBench(o, true); return 0; }
  for (int i = 1; i + 1 < argc; ++i)  // patch 0104
    if (!std::strcmp(argv[i], "--shader-stats")) return ShaderStats(argv[i + 1]);
  auto b = CreateVulkanBackend();
  if (!b) { std::printf("vktest: built without Vulkan headers\n"); return 2; }
  std::string err;
  if (!b->Init(o, err)) { std::printf("vktest: init failed: %s\n", err.c_str()); return 3; }
  std::printf("vktest: %s\n", b->Describe().c_str());
  CHECK(b->GetShaderIL() == ShaderIL::kSpirv, "IL");

  // 1. Clear a render target fully, then a rectangle, read back both regions.
  TextureDesc td;
  td.width = 64; td.height = 32; td.format = Format::kR8G8B8A8Unorm; td.usage = kUsageSampled | kUsageRenderTarget;
  TextureHandle rt = b->CreateTexture(td);
  CHECK(rt != 0, "CreateTexture rt");
  const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 0.5f};
  b->ClearColor(rt, red);
  Rect r{8, 4, 24, 12};
  b->ClearColor(rt, green, &r, 1);
  ReadbackHandle rh = b->ReadbackTexture(rt, Rect{0, 0, 64, 32});
  CHECK(rh != 0, "ReadbackTexture");
  CHECK(!b->IsReadbackReady(rh), "ready before submit");
  uint32_t pitch = 0;
  const uint8_t* px = WaitData(*b, rh, pitch);
  CHECK(px && pitch == 64 * 4, "readback data pitch %u", pitch);
  if (px) {
    auto at = [&](int x, int y) { const uint8_t* p = px + y * pitch + x * 4; return uint32_t(p[0]) | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; };
    CHECK(at(0, 0) == 0xFF0000FFu, "outside the rect: %08X", at(0, 0));
    CHECK(at(8, 4) == 0x8000FF00u || at(8, 4) == 0x7F00FF00u, "rect corner: %08X", at(8, 4));
    CHECK(at(23, 11) == at(8, 4), "rect far corner: %08X", at(23, 11));
    CHECK(at(24, 11) == 0xFF0000FFu, "just right of the rect: %08X", at(24, 11));
    CHECK(at(63, 31) == 0xFF0000FFu, "last pixel: %08X", at(63, 31));
  }
  b->ReleaseReadback(rh);

  // 2. Upload a pattern into mip 1 of a texture with padded rows, read back... (readback reads mip 0, so upload mip 0).
  TextureDesc ud;
  ud.width = 16; ud.height = 8; ud.mips = 2; ud.format = Format::kR8G8B8A8Unorm;
  TextureHandle up = b->CreateTexture(ud);
  std::vector<uint8_t> pat(80 * 8);  // row_bytes 80 > 64 (padding)
  for (int y = 0; y < 8; ++y) for (int x = 0; x < 16; ++x) { uint8_t* p = &pat[y * 80 + x * 4]; p[0] = uint8_t(x * 16); p[1] = uint8_t(y * 32); p[2] = uint8_t(x ^ y); p[3] = 0xAB; }
  b->UploadTexture(up, 0, 0, pat.data(), 80, 8, 1);
  std::vector<uint8_t> mip1(8 * 4 * 4, 0x5A);
  b->UploadTexture(up, 1, 0, mip1.data(), 32, 4, 1);
  b->UploadTexture(up, 2, 0, mip1.data(), 32, 4, 1);  // no mip 2: refused
  CHECK(b->Stats().texture_uploads_refused == 1, "refused count %llu", (unsigned long long)b->Stats().texture_uploads_refused);
  ReadbackHandle uh = b->ReadbackTexture(up, Rect{2, 1, 10, 7});
  px = WaitData(*b, uh, pitch);
  CHECK(px && pitch == 8 * 4, "upload readback pitch %u", pitch);
  if (px) {
    bool ok = true;
    for (int y = 0; y < 6 && ok; ++y) for (int x = 0; x < 8 && ok; ++x) ok = !std::memcmp(px + y * pitch + x * 4, &pat[(y + 1) * 80 + (x + 2) * 4], 4);
    CHECK(ok, "uploaded pattern differs");
  }
  b->ReleaseReadback(uh);

  // 3. Guest memory: upload bytes at an offset, read them back with the neighbourhood (zeros around).
  std::vector<uint8_t> g(4096);
  for (size_t i = 0; i < g.size(); ++i) g[i] = uint8_t(i * 7 + 3);
  b->UploadGuestMemory(0x123400, g.data(), uint32_t(g.size()));
  ReadbackHandle gh = b->ReadbackGuest(0x123400 - 16, uint32_t(g.size()) + 32);
  px = WaitData(*b, gh, pitch);
  CHECK(px && pitch == g.size() + 32, "guest readback");
  if (px) {
    bool zeros = true;
    for (int i = 0; i < 16; ++i) zeros = zeros && px[i] == 0 && px[16 + g.size() + i] == 0;
    CHECK(zeros, "guest neighbourhood not zero");
    CHECK(!std::memcmp(px + 16, g.data(), g.size()), "guest bytes differ");
  }
  b->ReleaseReadback(gh);

  // 4. The upload ring wraps: 40 uploads of 1 MB through a 16 MB ring, each checked.
  {
    TextureDesc bd;
    bd.width = 512; bd.height = 512; bd.format = Format::kR8G8B8A8Unorm;
    TextureHandle big = b->CreateTexture(bd);
    std::vector<uint8_t> data(512 * 512 * 4);
    bool all = true;
    for (int k = 0; k < 40; ++k) {
      std::memset(data.data(), k * 5 + 1, data.size());
      b->UploadTexture(big, 0, 0, data.data(), 512 * 4, 512, 1);
      if (k % 13 == 12) {
        ReadbackHandle h = b->ReadbackTexture(big, Rect{500, 500, 501, 501});
        const uint8_t* q = WaitData(*b, h, pitch);
        all = all && q && q[0] == uint8_t(k * 5 + 1);
        b->ReleaseReadback(h);
      }
    }
    CHECK(all, "ring wrap uploads");
    CHECK(b->Stats().ring_waits > 0, "the ring never waited (%llu)", (unsigned long long)b->Stats().ring_waits);
    b->DestroyTexture(big);
  }

  // 5. Depth: created at its clear value, cleared fully and in a rectangle (no readback of depth: checked by not failing).
  TextureDesc dd;
  dd.width = 32; dd.height = 32; dd.format = Format::kD24UnormS8; dd.usage = kUsageDepthStencil | kUsageSampled; dd.clear_depth = 0.5f;
  TextureHandle depth = b->CreateTexture(dd);
  CHECK(depth != 0, "depth texture");
  b->ClearDepthStencil(depth, true, 1.0f, true, 0x80, nullptr, 0);
  Rect dr{4, 4, 8, 8};
  b->ClearDepthStencil(depth, true, 0.25f, false, 0, &dr, 1);
  b->WaitForFence(b->Submit());
  std::string why;
  CHECK(!b->IsLost(&why), "lost: %s", why.c_str());

  // 6. Every texture format can be created, uploaded and destroyed.
  const Format formats[] = {Format::kR16G16B16A16Float, Format::kR16G16Snorm, Format::kR16G16B16A16Snorm, Format::kR32Uint,
                            Format::kR32Float, Format::kR32G32B32A32Float, Format::kR8Unorm, Format::kR8G8Unorm,
                            Format::kR16Unorm, Format::kR16G16Unorm, Format::kR16G16B16A16Unorm, Format::kR16Float,
                            Format::kR16G16Float, Format::kR32G32Float, Format::kR10G10B10A2Unorm, Format::kB5G6R5Unorm,
                            Format::kB5G5R5A1Unorm, Format::kB4G4R4A4Unorm, Format::kBC1, Format::kBC2, Format::kBC3,
                            Format::kBC4, Format::kBC5, Format::kD32FloatS8X24};
  int made = 0;
  for (Format f : formats) {
    TextureDesc fd;
    fd.width = 16; fd.height = 16; fd.format = f; fd.mips = 2;
    fd.usage = IsDepthFormat(f) ? kUsageDepthStencil | kUsageSampled : kUsageSampled;
    TextureHandle h = b->CreateTexture(fd);
    if (h) {
      ++made;
      if (!IsDepthFormat(f)) {
        std::vector<uint8_t> z(16 * 16 * 16, 0x11);
        b->UploadTexture(h, 0, 0, z.data(), 16 * 16, 16, 1);
      }
      b->DestroyTexture(h);
    } else {
      std::printf("  format %u: not created\n", unsigned(f));
    }
  }
  CHECK(made == int(sizeof(formats) / sizeof(formats[0])), "formats created %d", made);
  // 7. Cube and 3D textures upload per face / slice.
  TextureDesc cd;
  cd.dimension = TextureDimension::kCube; cd.width = cd.height = 8; cd.depth_or_layers = 6; cd.format = Format::kR8G8B8A8Unorm;
  TextureHandle cube = b->CreateTexture(cd);
  TextureDesc vd;
  vd.dimension = TextureDimension::k3D; vd.width = vd.height = 8; vd.depth_or_layers = 4; vd.format = Format::kR8G8B8A8Unorm;
  TextureHandle vol = b->CreateTexture(vd);
  CHECK(cube && vol, "cube %u 3D %u", cube, vol);
  std::vector<uint8_t> face(8 * 8 * 4 * 4, 0x33);
  for (uint32_t f = 0; f < 6; ++f) b->UploadTexture(cube, 0, f, face.data(), 32, 8, 1);
  b->UploadTexture(vol, 0, 0, face.data(), 32, 8, 4);
  b->WaitForFence(b->Submit());
  CHECK(!b->IsLost(&why), "lost after cube/3D: %s", why.c_str());
  CHECK(b->Stats().submits >= 6, "submits %llu", (unsigned long long)b->Stats().submits);

  // ======== stage V2: draws ========
  auto pixel = [&](ReadbackHandle h, int x, int y) -> uint32_t {
    uint32_t pp = 0;
    const uint8_t* q = b->ReadbackData(h, pp);
    if (!q) return 0xDEADBEEFu;
    const uint8_t* p = q + y * pp + x * 4;
    return uint32_t(p[0]) | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
  };
  const ShaderHandle vs_full = b->CreateShader(ShaderStage::kVertex, k_vs_full, sizeof(k_vs_full));
  const ShaderHandle vs_guest = b->CreateShader(ShaderStage::kVertex, k_vs_guest, sizeof(k_vs_guest));
  const ShaderHandle ps_const = b->CreateShader(ShaderStage::kPixel, k_ps_const, sizeof(k_ps_const));
  const ShaderHandle ps_tex = b->CreateShader(ShaderStage::kPixel, k_ps_tex, sizeof(k_ps_tex));
  CHECK(vs_full && vs_guest && ps_const && ps_tex, "shaders %u %u %u %u", vs_full, vs_guest, ps_const, ps_tex);
  TextureDesc cd2;
  cd2.width = 64; cd2.height = 32; cd2.format = Format::kR8G8B8A8Unorm; cd2.usage = kUsageSampled | kUsageRenderTarget;
  TextureHandle target = b->CreateTexture(cd2);
  TextureDesc zd;
  zd.width = 64; zd.height = 32; zd.format = Format::kD24UnormS8; zd.usage = kUsageDepthStencil;
  TextureHandle zbuf = b->CreateTexture(zd);
  GraphicsPipelineDesc pd;
  pd.vs = vs_full; pd.ps = ps_const;
  pd.color_count = 1; pd.color_formats[0] = Format::kR8G8B8A8Unorm; pd.color_write_masks[0] = 0xF;
  const PipelineHandle pl = b->GetGraphicsPipeline(pd);
  CHECK(pl != 0, "pipeline");
  struct VsRect { float rect[4]; float depth[4]; };
  auto draw_rect = [&](PipelineHandle p, float x0, float y0, float x1, float y1, float z, const float* colour) {
    VsRect r{{x0, y0, x1, y1}, {z, 0, 0, 0}};
    b->SetPipeline(p);
    b->SetConstants(ShaderStage::kVertex, 1, &r, sizeof(r));
    b->SetConstants(ShaderStage::kPixel, 0, colour, 16);
    b->Draw(6);
  };
  const float black[4] = {0, 0, 0, 0}, blue[4] = {0, 0, 1, 1}, white[4] = {1, 1, 1, 1};
  b->ClearColor(target, black);
  b->SetRenderTargets(1, &target, 0);
  b->SetViewport(Viewport{0, 0, 64, 32, 0, 1});
  b->SetScissor(Rect{0, 0, 64, 32});
  b->SetTextures(nullptr, nullptr);
  // Left half (NDC x -1..0) blue; with the y-up NDC, NDC y 0..1 is the TOP half of the image.
  draw_rect(pl, -1, 0, 0, 1, 0.5f, blue);
  ReadbackHandle d1 = b->ReadbackTexture(target, Rect{0, 0, 64, 32});
  b->WaitForFence(b->Submit());
  CHECK(pixel(d1, 5, 5) == 0xFFFF0000u, "draw: top-left %08X", pixel(d1, 5, 5));
  CHECK(pixel(d1, 40, 5) == 0, "draw: top-right %08X", pixel(d1, 40, 5));
  CHECK(pixel(d1, 5, 25) == 0, "draw: bottom-left %08X (y flip?)", pixel(d1, 5, 25));
  if (o.vk_attachment_layouts)  // patch 0100: the draw's target went into COLOR_ATTACHMENT_OPTIMAL (and read back)
    CHECK(b->Stats().vk_attachment_layout_changes > 0, "attachment layouts: %llu changes",
          (unsigned long long)b->Stats().vk_attachment_layout_changes);
  if (o.vk_count_pixels) {  // patch 0098: the same 32x16 rect again, counted as render-target slot 0
    const double before = b->Stats().gpu_target_px[0];
    b->SetGpuCategory(0x20);
    draw_rect(pl, -1, 0, 0, 1, 0.5f, blue);
    b->SetGpuCategory(0);
    b->WaitForFence(b->Submit());
    for (int k = 0; k < 4 && b->Stats().gpu_target_px[0] == before; ++k) {  // read once a later list retires it
      (void)b->ReadbackTexture(target, Rect{0, 0, 1, 1});
      b->WaitForFence(b->Submit());
    }
    const double got = b->Stats().gpu_target_px[0] - before;
    CHECK(got == 512.0, "count pixels: %.0f samples passed for a 32x16 rect, want 512", got);
  }
  {  // patch 0101: a tagged draw comes back with its own GPU time (0102: and its 512 passed samples)
    b->SetGpuTagSamples(true);
    b->WaitForFence(b->Submit());  // the next list resets its occlusion queries
    b->SetRenderTargets(1, &target, 0);
    b->SetGpuTag(77);
    draw_rect(pl, -1, 0, 0, 1, 0.5f, blue);
    b->SetGpuTag(0);
    b->WaitForFence(b->Submit());
    std::vector<GpuTagTime> tt, all;
    for (int k = 0; k < 4; ++k) {
      (void)b->ReadbackTexture(target, Rect{0, 0, 1, 1});
      b->WaitForFence(b->Submit());
      b->TakeGpuTagTimes(tt);
      all.insert(all.end(), tt.begin(), tt.end());
    }
    bool seen = false;
    double px = -1.0;
    for (const GpuTagTime& g : all) {
      if (g.tag == 77 && g.samples < 0.0) seen = true;
      if (g.tag == 77 && g.samples >= 0.0) px = std::max(px, 0.0) + g.samples;
    }
    b->SetGpuTagSamples(false);
    CHECK(seen, "draw report: tag 77 not timed (%zu tagged stretches)", all.size());
    CHECK(px == 512.0, "draw report: tag 77 passed %.0f samples, want 512", px);
    CHECK(b->DescribeTexture(target).find("VkFormat") != std::string::npos, "draw report: describe '%s'",
          b->DescribeTexture(target).c_str());
  }
  b->ReleaseReadback(d1);

  // Depth test: a far white rect over everything, then a near blue one on the left; then white again at mid depth -
  // only where nothing nearer was drawn.
  GraphicsPipelineDesc zp = pd;
  zp.depth_enable = zp.depth_write = true; zp.depth_func = CompareFunc::kLess; zp.depth_format = Format::kD24UnormS8;
  const PipelineHandle plz = b->GetGraphicsPipeline(zp);
  b->ClearColor(target, black);
  b->ClearDepthStencil(zbuf, true, 1.0f, true, 0, nullptr, 0);
  b->SetRenderTargets(1, &target, zbuf);
  draw_rect(plz, -1, -1, 0, 1, 0.2f, blue);
  draw_rect(plz, -1, -1, 1, 1, 0.6f, white);
  ReadbackHandle d2 = b->ReadbackTexture(target, Rect{0, 0, 64, 32});
  b->WaitForFence(b->Submit());
  CHECK(pixel(d2, 10, 16) == 0xFFFF0000u, "depth: near rect kept %08X", pixel(d2, 10, 16));
  CHECK(pixel(d2, 50, 16) == 0xFFFFFFFFu, "depth: far rect where nothing nearer %08X", pixel(d2, 50, 16));
  b->ReleaseReadback(d2);

  // Blending: additive red over blue = magenta.
  GraphicsPipelineDesc bp = pd;
  bp.color_blend[0].enable = true; bp.color_blend[0].src = BlendFactor::kOne; bp.color_blend[0].dst = BlendFactor::kOne;
  bp.color_blend[0].src_alpha = BlendFactor::kOne; bp.color_blend[0].dst_alpha = BlendFactor::kZero;
  const PipelineHandle plb = b->GetGraphicsPipeline(bp);
  const float red2[4] = {1, 0, 0, 1};
  b->ClearColor(target, blue);
  b->SetRenderTargets(1, &target, 0);
  draw_rect(plb, -1, -1, 1, 1, 0.5f, red2);
  ReadbackHandle d3 = b->ReadbackTexture(target, Rect{0, 0, 64, 32});
  b->WaitForFence(b->Submit());
  CHECK(pixel(d3, 32, 16) == 0xFFFF00FFu, "blend: %08X", pixel(d3, 32, 16));
  b->ReleaseReadback(d3);

  // Patch 0080: constants the slot already holds are not uploaded again within a list; a change still lands; a new
  // list uploads again (its ring copies may be reused). Left half: blue then (same) blue; right half: white; then a new
  // list draws green over the left with the same white as the last right-half draw - it must still be white there.
  {
    const uint64_t skipped0 = b->Stats().constant_uploads_skipped;
    b->ClearColor(target, black);
    b->SetRenderTargets(1, &target, 0);
    draw_rect(pl, -1, -1, 0, 1, 0.5f, blue);
    draw_rect(pl, -1, -1, 0, 1, 0.5f, blue);
    draw_rect(pl, 0, -1, 1, 1, 0.5f, white);
    ReadbackHandle d4 = b->ReadbackTexture(target, Rect{0, 0, 64, 32});
    b->WaitForFence(b->Submit());
    const uint64_t skipped1 = b->Stats().constant_uploads_skipped;
    CHECK(skipped1 >= skipped0 + 2, "constants: repeats skipped %llu", (unsigned long long)(skipped1 - skipped0));
    CHECK(pixel(d4, 10, 16) == 0xFFFF0000u && pixel(d4, 50, 16) == 0xFFFFFFFFu, "constants: after skips %08X %08X",
          pixel(d4, 10, 16), pixel(d4, 50, 16));
    b->ReleaseReadback(d4);
    const float green1[4] = {0, 1, 0, 1};
    b->SetRenderTargets(1, &target, 0);
    draw_rect(pl, -1, -1, 0, 1, 0.5f, green1);
    draw_rect(pl, 0, -1, 1, 1, 0.5f, white);
    ReadbackHandle d5 = b->ReadbackTexture(target, Rect{0, 0, 64, 32});
    b->WaitForFence(b->Submit());
    CHECK(pixel(d5, 10, 16) == 0xFF00FF00u && pixel(d5, 50, 16) == 0xFFFFFFFFu, "constants: next list %08X %08X",
          pixel(d5, 10, 16), pixel(d5, 50, 16));
    b->ReleaseReadback(d5);
  }

  // Textures: a 4x2 pattern at t3 / s3 (point sampling), drawn 1:1 into the top-left corner; then a swizzled view.
  TextureDesc pt;
  pt.width = 4; pt.height = 2; pt.format = Format::kR8G8B8A8Unorm;
  TextureHandle ptex = b->CreateTexture(pt);
  const uint32_t pat2[8] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu, 0xFF000000u, 0xFF00FFFFu, 0xFFFF00FFu, 0xFFFFFF00u};
  b->UploadTexture(ptex, 0, 0, pat2, 16, 2, 1);
  GraphicsPipelineDesc tp = pd;
  tp.ps = ps_tex;
  const PipelineHandle plt = b->GetGraphicsPipeline(tp);
  TextureView views[96] = {};
  SamplerDesc smp[32] = {};
  views[3].texture = ptex;
  smp[3].mag_linear = smp[3].min_linear = smp[3].mip_linear = 0;
  smp[3].address[0] = smp[3].address[1] = SamplerAddress::kClamp;
  const float tsize[4] = {4, 2, 0, 0};
  auto draw_tex = [&]() {
    VsRect r{{-1, -1, 1, 1}, {0.5f, 0, 0, 0}};
    b->SetPipeline(plt);
    b->SetConstants(ShaderStage::kVertex, 1, &r, sizeof(r));
    b->SetConstants(ShaderStage::kPixel, 0, tsize, 16);
    b->Draw(6);
  };
  b->SetTextures(views, smp, 96, 32);
  b->SetRenderTargets(1, &target, 0);
  draw_tex();
  ReadbackHandle d4 = b->ReadbackTexture(target, Rect{0, 0, 64, 32});
  b->WaitForFence(b->Submit());
  bool texok = true;
  for (int y = 0; y < 2; ++y) for (int x = 0; x < 4; ++x) texok = texok && pixel(d4, x, y) == pat2[y * 4 + x];
  CHECK(texok, "texture: %08X %08X / %08X", pixel(d4, 0, 0), pixel(d4, 1, 0), pixel(d4, 0, 1));
  CHECK(pixel(d4, 40, 20) == pat2[7], "texture clamp: %08X", pixel(d4, 40, 20));
  b->ReleaseReadback(d4);
  views[3].components[0] = 2; views[3].components[1] = 1; views[3].components[2] = 0; views[3].components[3] = 4;  // BGR, alpha 0
  b->SetTextures(views, smp, 96, 32);
  draw_tex();
  ReadbackHandle d5 = b->ReadbackTexture(target, Rect{0, 0, 4, 1});
  b->WaitForFence(b->Submit());
  CHECK(pixel(d5, 0, 0) == 0x00FF0000u, "swizzled view: %08X", pixel(d5, 0, 0));
  b->ReleaseReadback(d5);
  // Patch 0086: a signed view of an RGBA8 image (mutable with its exact format list): 0x7F reads 1.0 (0xFF), where
  // the plain view reads 0x7F.
  {
    TextureDesc sd;
    sd.width = 4; sd.height = 2; sd.format = Format::kR8G8B8A8Unorm;
    TextureHandle stex = b->CreateTexture(sd);
    uint32_t s7f[8];
    for (auto& w : s7f) w = 0x7F7F7F7Fu;
    b->UploadTexture(stex, 0, 0, s7f, 16, 2, 1);
    TextureView keep = views[3];
    views[3] = TextureView{};
    views[3].texture = stex;
    views[3].signed_view = true;
    b->SetTextures(views, smp, 96, 32);
    draw_tex();
    ReadbackHandle ds = b->ReadbackTexture(target, Rect{0, 0, 4, 1});
    b->WaitForFence(b->Submit());
    CHECK(pixel(ds, 0, 0) == 0xFFFFFFFFu, "signed view: %08X", pixel(ds, 0, 0));
    b->ReleaseReadback(ds);
    views[3] = keep;
  }
  // An unbound slot reads the white default; sampling a target just drawn into needs the pass split + barrier.
  TextureDesc rd = cd2;
  TextureHandle second = b->CreateTexture(rd);
  b->SetRenderTargets(1, &target, 0);
  draw_rect(pl, -1, -1, 1, 1, 0.5f, blue);  // target all blue
  views[3] = TextureView{};
  views[3].texture = target;
  b->SetTextures(views, smp, 96, 32);
  b->SetRenderTargets(1, &second, 0);
  const float tsize2[4] = {64, 32, 0, 0};
  {
    VsRect r{{-1, -1, 1, 1}, {0.5f, 0, 0, 0}};
    b->SetPipeline(plt);
    b->SetConstants(ShaderStage::kVertex, 1, &r, sizeof(r));
    b->SetConstants(ShaderStage::kPixel, 0, tsize2, 16);
    b->Draw(6);
  }
  ReadbackHandle d6 = b->ReadbackTexture(second, Rect{0, 0, 64, 32});
  b->WaitForFence(b->Submit());
  CHECK(pixel(d6, 33, 17) == 0xFFFF0000u, "render-to-texture then sample: %08X", pixel(d6, 33, 17));
  b->ReleaseReadback(d6);
  b->SetTextures(nullptr, nullptr);

  // Vertices from guest memory, plain and indexed (a quad from 4 vertices).
  const float verts[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
  b->UploadGuestMemory(0x1000, verts, sizeof(verts));
  GraphicsPipelineDesc gp = pd;
  gp.vs = vs_guest;
  gp.topology = Topology::kTriangleList;
  const PipelineHandle plg = b->GetGraphicsPipeline(gp);
  b->ClearColor(target, black);
  b->SetRenderTargets(1, &target, 0);
  b->SetPipeline(plg);
  b->SetConstants(ShaderStage::kPixel, 0, white, 16);
  const uint32_t idx[6] = {0, 1, 2, 2, 1, 3};
  b->DrawIndexed(idx, 6);
  ReadbackHandle d7 = b->ReadbackTexture(target, Rect{0, 0, 64, 32});
  b->WaitForFence(b->Submit());
  CHECK(pixel(d7, 1, 1) == 0xFFFFFFFFu && pixel(d7, 62, 30) == 0xFFFFFFFFu, "indexed guest quad: %08X %08X", pixel(d7, 1, 1), pixel(d7, 62, 30));
  b->ReleaseReadback(d7);
  const BufferHandle ib = b->CreateIndexBuffer(idx, 3);
  CHECK(ib != 0, "index buffer");
  b->ClearColor(target, black);
  b->SetRenderTargets(1, &target, 0);
  b->SetPipeline(plg);
  b->SetConstants(ShaderStage::kPixel, 0, blue, 16);
  b->DrawIndexedBuffer(ib, 3);
  ReadbackHandle d8 = b->ReadbackTexture(target, Rect{0, 0, 64, 32});
  b->WaitForFence(b->Submit());
  CHECK(pixel(d8, 2, 29) == 0xFFFF0000u && pixel(d8, 62, 1) == 0, "index buffer draw: %08X %08X", pixel(d8, 2, 29), pixel(d8, 62, 1));
  b->ReleaseReadback(d8);
  b->ReleaseIndexBuffer(ib);
  // Patch 0081: indices kept in the ring and drawn again in the same list (DrawIndexedAt), mixed with an index-buffer
  // draw (the ring binding must come back); a later list refuses the old offset. Triangle {0,1,2} = lower-left half.
  {
    const uint32_t tri_hi[3] = {2, 1, 3};  // upper-right half
    const BufferHandle ib2 = b->CreateIndexBuffer(tri_hi, 3);
    b->ClearColor(target, black);
    b->SetRenderTargets(1, &target, 0);
    b->SetPipeline(plg);
    b->SetConstants(ShaderStage::kPixel, 0, blue, 16);
    const uint32_t two[6] = {7, 7, 7, 0, 1, 2};  // the triangle after 3 dummies (offset not at the ring's start)
    const uint64_t koff = b->DrawIndexedKeep(two + 3, 3);
    const uint64_t klist = b->Stats().submits;
    b->DrawIndexedBuffer(ib2, 3);  // blue upper-right from a buffer
    b->SetConstants(ShaderStage::kPixel, 0, white, 16);
    const uint64_t reused0 = b->Stats().index_draws_reused;
    const bool at_ok = koff != UINT64_MAX && b->DrawIndexedAt(koff, 3, klist);  // white over the lower-left
    ReadbackHandle dk = b->ReadbackTexture(target, Rect{0, 0, 64, 32});
    b->WaitForFence(b->Submit());
    CHECK(at_ok && b->Stats().index_draws_reused == reused0 + 1, "DrawIndexedAt: drawn %d", int(at_ok));
    CHECK(pixel(dk, 2, 29) == 0xFFFFFFFFu && pixel(dk, 62, 1) == 0xFFFF0000u, "kept indices + buffer: %08X %08X",
          pixel(dk, 2, 29), pixel(dk, 62, 1));
    b->ReleaseReadback(dk);
    b->SetRenderTargets(1, &target, 0);
    CHECK(!b->DrawIndexedAt(koff, 3, klist), "DrawIndexedAt: a later list refuses the old offset");
    b->WaitForFence(b->Submit());
    b->ReleaseIndexBuffer(ib2);
  }

  // Patch 0078: the same quad from the upload ring (UploadTransient, read through t97 at the returned offset).
  {
    const ShaderHandle vs_ring = b->CreateShader(ShaderStage::kVertex, k_vs_ring, sizeof(k_vs_ring));
    const float vr[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    b->UploadTransient(vr, 8);  // something before it, so the offset is not 0
    const uint32_t off = b->UploadTransient(vr, sizeof(vr));
    CHECK(vs_ring && off != UINT32_MAX && off != 0, "UploadTransient: shader %u offset %u", vs_ring, off);
    GraphicsPipelineDesc rp = gp;
    rp.vs = vs_ring;
    const PipelineHandle plr = b->GetGraphicsPipeline(rp);
    b->ClearColor(target, black);
    b->SetRenderTargets(1, &target, 0);
    b->SetPipeline(plr);
    const uint32_t offc[4] = {off, 0, 0, 0};
    b->SetConstants(ShaderStage::kVertex, 1, offc, 16);
    b->SetConstants(ShaderStage::kPixel, 0, white, 16);
    b->DrawIndexed(idx, 6);
    ReadbackHandle d9 = b->ReadbackTexture(target, Rect{0, 0, 64, 32});
    b->WaitForFence(b->Submit());
    CHECK(pixel(d9, 1, 1) == 0xFFFFFFFFu && pixel(d9, 62, 30) == 0xFFFFFFFFu, "ring quad: %08X %08X", pixel(d9, 1, 1), pixel(d9, 62, 30));
    b->ReleaseReadback(d9);
  }

  // Asynchronous pipelines: pending first, then ready; a draw with a pending pipeline is skipped.
  GraphicsPipelineDesc ap = pd;
  ap.cull = CullMode::kBack;
  bool pend = false;
  PipelineHandle pa = b->GetGraphicsPipeline(ap, true, &pend);
  CHECK(pa == 0 && pend, "async: first request pending (%u %d)", pa, int(pend));
  CHECK(b->PipelinePending(ap), "async: PipelinePending");
  for (int i = 0; i < 2000 && b->PipelinePending(ap); ++i) b->WaitForFence(b->Submit());
  for (int i = 0; i < 400 && b->PipelinePending(ap); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  pa = b->GetGraphicsPipeline(ap, true, &pend);
  CHECK(pa != 0 && !pend, "async: ready later (%u %d)", pa, int(pend));
  // Patch 0088: the worker counts its job done just after it publishes the pipeline - give it a moment (a validation run
  // on lavapipe once read 1 here).
  for (int i = 0; i < 400 && b->PipelineJobsLeft() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(b->PipelineJobsLeft() == 0, "jobs left %llu", (unsigned long long)b->PipelineJobsLeft());
  CHECK(!b->IsLost(&why), "lost after draws: %s", why.c_str());
  std::printf("vktest: draws %llu, pipelines %llu, view sets %llu, sampler sets %llu, clears in a pass %llu, pass starts "
              "without a barrier %llu\n", (unsigned long long)b->Stats().draws,
              (unsigned long long)b->Stats().pipelines, (unsigned long long)b->Stats().texture_table_misses,
              (unsigned long long)b->Stats().sampler_tables_written, (unsigned long long)b->Stats().vk_clears_in_pass,
              (unsigned long long)b->Stats().vk_barriers_skipped);

  // ======== stage V4: compute ========
  {
    const ShaderHandle cs1 = b->CreateShader(ShaderStage::kCompute, k_cs_to_guest, sizeof(k_cs_to_guest));
    const ShaderHandle cs2 = b->CreateShader(ShaderStage::kCompute, k_cs_fill, sizeof(k_cs_fill));
    const PipelineHandle cp1 = b->CreateComputePipeline(cs1), cp2 = b->CreateComputePipeline(cs2);
    CHECK(cs1 && cs2 && cp1 && cp2, "compute shaders %u %u pipelines %u %u", cs1, cs2, cp1, cp2);
    TextureDesc wd;
    wd.width = 16; wd.height = 16; wd.mips = 2; wd.format = Format::kR32Uint; wd.usage = kUsageSampled | kUsageStorage;
    TextureHandle words = b->CreateTexture(wd);
    ComputeBindings fb;
    fb.constants[0] = 1000; fb.constant_count = 4;
    fb.uav_texture = words; fb.uav_mip = 0;
    const uint64_t bc0 = b->Stats().barrier_calls;
    b->Dispatch(cp2, fb, 2, 2, 1);
    ComputeBindings gb;
    gb.constants[0] = 0x2000; gb.constant_count = 4;
    gb.srv[0] = words;
    gb.guest_u0 = true;
    b->Dispatch(cp1, gb, 1, 1, 1);
    // Patch 0082: the first dispatch's barrier before it, then its barrier after merged with the second's before it.
    CHECK(b->Stats().barrier_calls - bc0 == 2, "barriers batched: %llu calls for two dispatches",
          (unsigned long long)(b->Stats().barrier_calls - bc0));
    ReadbackHandle gr = b->ReadbackGuest(0x2000, 32);
    b->WaitForFence(b->Submit());
    uint32_t gp2 = 0;
    const uint8_t* q = b->ReadbackData(gr, gp2);
    bool ok = q != nullptr;
    for (uint32_t i = 0; ok && i < 8; ++i) {
      const uint32_t want = 1000 + (i % 4) + (i / 4) * 16;  // stored big-endian
      ok = q[i * 4] == uint8_t(want >> 24) && q[i * 4 + 1] == uint8_t(want >> 16) && q[i * 4 + 2] == uint8_t(want >> 8) && q[i * 4 + 3] == uint8_t(want);
    }
    CHECK(ok, "compute: storage image -> sampled -> guest memory");
    b->ReleaseReadback(gr);
    CHECK(b->Stats().dispatches == 2, "dispatches %llu", (unsigned long long)b->Stats().dispatches);
    // "Match my monitor": the present resample (kPresentResampleCs). 4x2 -> 2x1 shrinks: each output pixel is the box
    // average of the 2x2 source pixels it covers. 2x1 -> 4x1 enlarges bilinearly (edges clamp). Words are RGB10A2.
    {
      const ShaderHandle rs = b->CreateShader(ShaderStage::kCompute, k_cs_resample, sizeof(k_cs_resample));
      const PipelineHandle rp = rs ? b->CreateComputePipeline(rs) : 0;
      CHECK(rp != 0, "resample pipeline");
      auto word = [](uint32_t r, uint32_t g, uint32_t bl, uint32_t top) { return r | (g << 10) | (bl << 20) | (top << 30); };
      TextureDesc sd;
      sd.width = 4; sd.height = 2; sd.format = Format::kR32Uint; sd.usage = kUsageSampled | kUsageStorage;
      const TextureHandle src = b->CreateTexture(sd);
      const uint32_t sw[8] = {word(0, 100, 1023, 3), word(100, 100, 1023, 3), word(200, 0, 0, 3), word(600, 0, 0, 3),
                              word(0, 300, 1023, 3), word(100, 300, 1023, 3), word(400, 0, 0, 3), word(800, 0, 0, 3)};
      b->UploadTexture(src, 0, 0, sw, 16, 2, 1);
      TextureDesc dd;
      dd.width = 2; dd.height = 1; dd.format = Format::kR32Uint; dd.usage = kUsageSampled | kUsageStorage;
      const TextureHandle dst = b->CreateTexture(dd);
      ComputeBindings rb;
      rb.constants[0] = 4; rb.constants[1] = 2; rb.constants[2] = 2; rb.constants[3] = 1; rb.constant_count = 4;
      rb.srv[0] = src;
      rb.uav_texture = dst;
      b->Dispatch(rp, rb, 1, 1, 1);
      TextureDesc ud = dd;
      ud.width = 4;
      const TextureHandle up = b->CreateTexture(ud);
      ComputeBindings ub;
      ub.constants[0] = 2; ub.constants[1] = 1; ub.constants[2] = 4; ub.constants[3] = 1; ub.constant_count = 4;
      ub.srv[0] = dst;
      ub.uav_texture = up;
      b->Dispatch(rp, ub, 1, 1, 1);
      ReadbackHandle r1 = b->ReadbackTexture(dst, Rect{0, 0, 2, 1});
      ReadbackHandle r2 = b->ReadbackTexture(up, Rect{0, 0, 4, 1});
      b->WaitForFence(b->Submit());
      uint32_t p1 = 0, p2 = 0;
      const uint32_t* o1 = reinterpret_cast<const uint32_t*>(WaitData(*b, r1, p1));
      const uint32_t* o2 = reinterpret_cast<const uint32_t*>(WaitData(*b, r2, p2));
      // Averaged in linear light (sRGB curve), values from a double-precision reference (+-1 for float rounding):
      // shrunk R (0, 100, 0, 100) -> 61, G (100, 100, 300, 300) -> 225, B 1023; R (200, 600, 400, 800) -> 558; top bits
      // kept. Enlarged 2 -> 4 (source x -0.25, 0.25, 0.75, 1.25, edges clamped): R 61, 296, 490, 558.
      auto close_to = [](uint32_t v, uint32_t want) { return (v > want ? v - want : want - v) <= 1; };
      auto ch = [](uint32_t w, int k) { return (w >> (10 * k)) & 0x3FF; };
      CHECK(o1 && close_to(ch(o1[0], 0), 61) && close_to(ch(o1[0], 1), 225) && ch(o1[0], 2) == 1023 && (o1[0] >> 30) == 3 &&
                close_to(ch(o1[1], 0), 558) && ch(o1[1], 1) == 0 && ch(o1[1], 2) == 0 && (o1[1] >> 30) == 3,
            "resample shrink (linear light): %08X %08X", o1 ? o1[0] : 0, o1 ? o1[1] : 0);
      bool upok = o2 != nullptr;
      const uint32_t want_r[4] = {61, 296, 490, 558};
      for (int i = 0; upok && i < 4; ++i) upok = close_to(o2[i] & 0x3FF, want_r[i]) && (o2[i] >> 30) == 3;
      CHECK(upok, "resample enlarge: %u %u %u %u", o2 ? o2[0] & 0x3FF : 0, o2 ? o2[1] & 0x3FF : 0, o2 ? o2[2] & 0x3FF : 0,
            o2 ? o2[3] & 0x3FF : 0);
      b->ReleaseReadback(r1);
      b->ReleaseReadback(r2);
    }
    CHECK(!b->IsLost(&why), "lost after compute: %s", why.c_str());
  }

  // Patch 0077: GPU timestamps - lists timed and GPU time counted once their submissions completed.
  b->WaitForFence(b->Submit());
  b->WaitForFence(b->Submit());
  CHECK(b->Stats().gpu_lists_timed > 0 && b->Stats().gpu_ms > 0.0, "GPU timestamps: %llu lists, %.3f ms",
        (unsigned long long)b->Stats().gpu_lists_timed, b->Stats().gpu_ms);
#if GN_HAVE_VULKAN_HEADERS
  b.reset();
  ExternalDeviceCheck();  // notes/68 V3
  PipelineCacheCheck();  // patch 0079
  LazyTargetsCheck();    // patch 0087
#endif
  std::printf("vktest: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
