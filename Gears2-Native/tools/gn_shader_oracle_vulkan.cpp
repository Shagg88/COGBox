// Gears2-Native S3.4 shader oracle: the Vulkan side (notes/41, job 091 triage). Runs the compute-wrapped translated
// shader, compiled to SPIR-V, for every constant set and reads the outputs back. It exists so the oracle can run in the
// Linux container on Mesa lavapipe (a CPU Vulkan driver) and, later, so SPIR-V can be compared on the PC too.
// Bindings follow the translator's Vulkan contract (notes/39): set 0 bindings 0-3 the cbuffers; set 1 bindings 0-31
// Texture2D, 32-63 TextureCube, 64-95 Texture3D, 96 the guest memory (ByteAddressBuffer); set 2 bindings 0-31 samplers.
// The oracle wrapper adds set 1 binding 97 (pixel inputs) and set 3 binding 0 (outputs).
// Build with -DGN_ORACLE_VULKAN and link libvulkan. Our code (MIT).
#include "gn_shader_oracle.h"

#ifdef GN_ORACLE_VULKAN

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gn_oracle {
namespace {

constexpr uint32_t kCbBytesPerSet = 8192;  // cb0 256 + cb1 4096 + cb2 256 + cb3 768, each at a 256-aligned offset
constexpr uint32_t kCb0 = 0, kCb1 = 256, kCb2 = 256 + 4096, kCb3 = 256 + 4096 + 256;
constexpr uint32_t kPixelInputBytesPerSet = kLanes * 16 * 16;
constexpr uint32_t kOutBytesPerSet = kLanes * kVsOutputs * 16;

std::string Vk(const char* what, VkResult r) {
  char b[128];
  std::snprintf(b, sizeof(b), "%s failed (VkResult %d)%s", what, int(r),
                r == VK_ERROR_DEVICE_LOST ? " DEVICE_REMOVED" : "");
  return b;
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

class VulkanRunner final : public GpuRunner {
 public:
  ~VulkanRunner() override {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (Image& i : images_) {
      vkDestroyImageView(device_, i.view, nullptr);
      vkDestroyImage(device_, i.image, nullptr);
      vkFreeMemory(device_, i.memory, nullptr);
    }
    for (Buffer* b : {&cb_, &pixel_in_, &out_, &guest_, &staging_}) {
      if (b->buffer) vkDestroyBuffer(device_, b->buffer, nullptr);
      if (b->memory) vkFreeMemory(device_, b->memory, nullptr);
    }
    vkDestroySampler(device_, sampler_, nullptr);
    vkDestroyDescriptorPool(device_, pool_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    for (VkDescriptorSetLayout l : set_layouts_) vkDestroyDescriptorSetLayout(device_, l, nullptr);
    vkDestroyFence(device_, fence_, nullptr);
    vkDestroyCommandPool(device_, cmd_pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
  }

  bool Init(const uint8_t* guest_memory, std::string& error) override {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gn_shader_oracle";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
    if (r != VK_SUCCESS) return error = Vk("vkCreateInstance", r), false;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    if (!n) return error = "no Vulkan device", false;
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    physical_ = pds[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    description_ = std::string(props.deviceName) + " (Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) +
                   "." + std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + ")";
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qf.data());
    queue_family_ = UINT32_MAX;
    for (uint32_t i = 0; i < qn; ++i)
      if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { queue_family_ = i; break; }
    if (queue_family_ == UINT32_MAX) return error = "no compute queue", false;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(physical_, &f2);
    VkPhysicalDeviceVulkan12Features want12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    want12.shaderFloat16 = f12.shaderFloat16;
    want12.uniformBufferStandardLayout = f12.uniformBufferStandardLayout;
    want12.scalarBlockLayout = f12.scalarBlockLayout;
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    want.pNext = &want12;
    want.features.shaderInt64 = f2.features.shaderInt64;
    want.features.shaderFloat64 = f2.features.shaderFloat64;
    want.features.shaderInt16 = f2.features.shaderInt16;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &want;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    r = vkCreateDevice(physical_, &dci, nullptr, &device_);
    if (r != VK_SUCCESS) return error = Vk("vkCreateDevice", r), false;
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props_);

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = queue_family_;
    vkCreateCommandPool(device_, &cpci, nullptr, &cmd_pool_);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cmd_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cbai, &cmd_);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device_, &fci, nullptr, &fence_);

    // Buffers (host visible; fine for a test tool).
    if (!MakeBuffer(cb_, size_t(kSets) * kCbBytesPerSet, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, error) ||
        !MakeBuffer(pixel_in_, size_t(kSets) * kPixelInputBytesPerSet, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error) ||
        !MakeBuffer(out_, size_t(kSets) * kOutBytesPerSet, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error) ||
        !MakeBuffer(guest_, kGuestMemoryBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, error))
      return false;
    std::memcpy(guest_.mapped, guest_memory, kGuestMemoryBytes);

    // 96 textures filled with SlotColour.
    const uint32_t texel_count_max = 4 * 4 * 6;
    if (!MakeBuffer(staging_, size_t(96) * texel_count_max * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, error)) return false;
    images_.resize(96);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_, &bi);
    for (uint32_t t = 0; t < 96; ++t) {
      const uint32_t dim = t / 32, k = t % 32;
      VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ii.imageType = dim == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
      ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
      ii.extent = {4, 4, dim == 2 ? 4u : 1u};
      ii.mipLevels = 1;
      ii.arrayLayers = dim == 1 ? 6 : 1;
      ii.flags = dim == 1 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
      ii.samples = VK_SAMPLE_COUNT_1_BIT;
      ii.tiling = VK_IMAGE_TILING_OPTIMAL;
      ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      r = vkCreateImage(device_, &ii, nullptr, &images_[t].image);
      if (r != VK_SUCCESS) return error = Vk("vkCreateImage", r), false;
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(device_, images_[t].image, &mr);
      VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      mai.allocationSize = mr.size;
      mai.memoryTypeIndex = MemoryType(mr.memoryTypeBits, 0);
      vkAllocateMemory(device_, &mai, nullptr, &images_[t].memory);
      vkBindImageMemory(device_, images_[t].image, images_[t].memory, 0);
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = images_[t].image;
      vi.viewType = dim == 0 ? VK_IMAGE_VIEW_TYPE_2D : dim == 1 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_3D;
      vi.format = ii.format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ii.arrayLayers};
      vkCreateImageView(device_, &vi, nullptr, &images_[t].view);

      float colour[4];
      SlotColour(dim, k, colour);
      float* st = reinterpret_cast<float*>(staging_.mapped) + size_t(t) * texel_count_max * 4;
      for (uint32_t i = 0; i < texel_count_max; ++i) std::memcpy(st + i * 4, colour, 16);
      Barrier(images_[t].image, ii.arrayLayers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkBufferImageCopy c{};
      c.bufferOffset = VkDeviceSize(t) * texel_count_max * 16;
      c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, ii.arrayLayers};
      c.imageExtent = ii.extent;
      vkCmdCopyBufferToImage(cmd_, staging_.buffer, images_[t].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
      Barrier(images_[t].image, ii.arrayLayers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    vkEndCommandBuffer(cmd_);
    if (!Submit(error)) return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    vkCreateSampler(device_, &sci, nullptr, &sampler_);

    // Descriptor set layouts: a superset of what any translated shader declares.
    std::vector<VkDescriptorSetLayoutBinding> b0, b1, b2, b3;
    for (uint32_t i = 0; i < 4; ++i) b0.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT});
    for (uint32_t i = 0; i < 96; ++i) b1.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT});
    b1.push_back({96, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT});
    b1.push_back({97, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT});
    for (uint32_t i = 0; i < 32; ++i) b2.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT});
    b3.push_back({0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT});
    for (auto* b : {&b0, &b1, &b2, &b3}) {
      VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      li.bindingCount = uint32_t(b->size());
      li.pBindings = b->data();
      VkDescriptorSetLayout l;
      vkCreateDescriptorSetLayout(device_, &li, nullptr, &l);
      set_layouts_.push_back(l);
    }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 4;
    pli.pSetLayouts = set_layouts_.data();
    vkCreatePipelineLayout(device_, &pli, nullptr, &layout_);

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 * kSets},
                                          {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 96 * kSets},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kSets},
                                          {VK_DESCRIPTOR_TYPE_SAMPLER, 32 * kSets}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 4 * kSets;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = sizes;
    vkCreateDescriptorPool(device_, &dpi, nullptr, &pool_);
    sets_.resize(size_t(kSets) * 4);
    for (uint32_t s = 0; s < kSets; ++s) {
      VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      ai.descriptorPool = pool_;
      ai.descriptorSetCount = 4;
      ai.pSetLayouts = set_layouts_.data();
      r = vkAllocateDescriptorSets(device_, &ai, &sets_[size_t(s) * 4]);
      if (r != VK_SUCCESS) return error = Vk("vkAllocateDescriptorSets", r), false;
      std::vector<VkWriteDescriptorSet> w;
      std::vector<VkDescriptorBufferInfo> bufs;
      std::vector<VkDescriptorImageInfo> imgs;
      bufs.reserve(8);
      imgs.reserve(128);
      const VkDeviceSize cb_base = VkDeviceSize(s) * kCbBytesPerSet;
      const VkDeviceSize cb_off[4] = {kCb0, kCb1, kCb2, kCb3}, cb_size[4] = {256, 4096, 256, 768};
      for (uint32_t i = 0; i < 4; ++i) {
        bufs.push_back({cb_.buffer, cb_base + cb_off[i], cb_size[i]});
        w.push_back(Write(sets_[s * 4 + 0], i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bufs.back(), nullptr));
      }
      for (uint32_t i = 0; i < 96; ++i) {
        imgs.push_back({VK_NULL_HANDLE, images_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        w.push_back(Write(sets_[s * 4 + 1], i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr, &imgs.back()));
      }
      bufs.push_back({guest_.buffer, 0, kGuestMemoryBytes});
      w.push_back(Write(sets_[s * 4 + 1], 96, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bufs.back(), nullptr));
      bufs.push_back({pixel_in_.buffer, VkDeviceSize(s) * kPixelInputBytesPerSet, kPixelInputBytesPerSet});
      w.push_back(Write(sets_[s * 4 + 1], 97, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bufs.back(), nullptr));
      for (uint32_t i = 0; i < 32; ++i) {
        imgs.push_back({sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
        w.push_back(Write(sets_[s * 4 + 2], i, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr, &imgs.back()));
      }
      bufs.push_back({out_.buffer, VkDeviceSize(s) * kOutBytesPerSet, kOutBytesPerSet});
      w.push_back(Write(sets_[s * 4 + 3], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &bufs.back(), nullptr));
      vkUpdateDescriptorSets(device_, uint32_t(w.size()), w.data(), 0, nullptr);
    }
    return true;
  }

  std::string Describe() const override { return description_; }

  bool Run(const std::vector<uint8_t>& spirv, bool vertex, const std::vector<GpuSet>& sets, std::vector<float>& out,
           std::string& error) override {
    for (uint32_t s = 0; s < kSets; ++s) {
      uint8_t* cb = reinterpret_cast<uint8_t*>(cb_.mapped) + size_t(s) * kCbBytesPerSet;
      std::memcpy(cb + kCb0, sets[s].cb0.data(), sets[s].cb0.size());
      std::memcpy(cb + kCb1, sets[s].cb1.data(), sets[s].cb1.size());
      std::memcpy(cb + kCb2, sets[s].cb2.data(), sets[s].cb2.size());
      std::memcpy(cb + kCb3, sets[s].cb3.data(), sets[s].cb3.size());
      if (!vertex)
        std::memcpy(reinterpret_cast<uint8_t*>(pixel_in_.mapped) + size_t(s) * kPixelInputBytesPerSet,
                    sets[s].pixel_inputs.data(), kPixelInputBytesPerSet);
    }
    std::memset(out_.mapped, 0xCD, size_t(kSets) * kOutBytesPerSet);
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spirv.size();
    smi.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule module;
    VkResult r = vkCreateShaderModule(device_, &smi, nullptr, &module);
    if (r != VK_SUCCESS) return error = Vk("vkCreateShaderModule", r), false;
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "xe_oracle_main";
    cpi.layout = layout_;
    VkPipeline pipeline;
    r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline);
    vkDestroyShaderModule(device_, module, nullptr);
    if (r != VK_SUCCESS) return error = Vk("vkCreateComputePipelines", r), false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkResetCommandBuffer(cmd_, 0);
    vkBeginCommandBuffer(cmd_, &bi);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    for (uint32_t s = 0; s < kSets; ++s) {
      vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 4, &sets_[size_t(s) * 4], 0, nullptr);
      vkCmdDispatch(cmd_, 1, 1, 1);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
    vkEndCommandBuffer(cmd_);
    const bool ok = Submit(error);
    vkDestroyPipeline(device_, pipeline, nullptr);
    if (!ok) return false;
    const uint32_t outputs = vertex ? kVsOutputs : kPsOutputs;
    out.assign(size_t(kSets) * kLanes * outputs * 4, 0.0f);
    for (uint32_t s = 0; s < kSets; ++s)
      std::memcpy(&out[size_t(s) * kLanes * outputs * 4],
                  reinterpret_cast<const uint8_t*>(out_.mapped) + size_t(s) * kOutBytesPerSet,
                  size_t(kLanes) * outputs * 16);
    return true;
  }

 private:
  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
  }

  bool MakeBuffer(Buffer& b, size_t size, VkBufferUsageFlags usage, std::string& error) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VkResult r = vkCreateBuffer(device_, &ci, nullptr, &b.buffer);
    if (r != VK_SUCCESS) return error = Vk("vkCreateBuffer", r), false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device_, b.buffer, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex =
        MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    r = vkAllocateMemory(device_, &mai, nullptr, &b.memory);
    if (r != VK_SUCCESS) return error = Vk("vkAllocateMemory", r), false;
    vkBindBufferMemory(device_, b.buffer, b.memory, 0);
    vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    return true;
  }

  void Barrier(VkImage image, uint32_t layers, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  }

  static VkWriteDescriptorSet Write(VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                                    const VkDescriptorBufferInfo* buf, const VkDescriptorImageInfo* img) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pBufferInfo = buf;
    w.pImageInfo = img;
    return w;
  }

  bool Submit(std::string& error) {
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    vkResetFences(device_, 1, &fence_);
    VkResult r = vkQueueSubmit(queue_, 1, &si, fence_);
    if (r != VK_SUCCESS) return error = Vk("vkQueueSubmit", r), false;
    r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, 30ull * 1000 * 1000 * 1000);
    if (r == VK_TIMEOUT) return error = "fence timeout (30 s) DEVICE_REMOVED", false;
    if (r != VK_SUCCESS) return error = Vk("vkWaitForFences", r), false;
    return true;
  }

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkPhysicalDeviceMemoryProperties mem_props_{};
  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  Buffer cb_, pixel_in_, out_, guest_, staging_;
  std::vector<Image> images_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorSetLayout> set_layouts_;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> sets_;
  std::string description_;
};

}  // namespace

std::unique_ptr<GpuRunner> CreateVulkanRunner() { return std::make_unique<VulkanRunner>(); }

}  // namespace gn_oracle

#endif  // GN_ORACLE_VULKAN
