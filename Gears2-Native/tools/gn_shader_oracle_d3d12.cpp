// Gears2-Native S3.4 shader oracle: the D3D12 side (notes/41). Runs the compute-wrapped translated shader for every
// constant set and reads the outputs back. Resources follow the S3.3 binding contract (notes/39):
//   b0-b3 root CBVs (XeSystem, float constants, bool/loop, fetch constants), t0-31 Texture2D, t32-63 TextureCube,
//   t64-95 Texture3D (each slot a 4x4(x4) R32G32B32A32_FLOAT texture filled with SlotColour), t96 the guest memory
//   (raw buffer), t97 the pixel inputs (root SRV), u0 the outputs (root UAV), s0-31 static point/clamp samplers.
// Our code (MIT). Windows only; elsewhere CreateGpuRunner returns nullptr.
#include "gn_shader_oracle.h"

#ifndef _WIN32

namespace gn_oracle {
std::unique_ptr<GpuRunner> CreateGpuRunner() { return nullptr; }
}  // namespace gn_oracle

#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gn_oracle {
namespace {

template <typename T>
void SafeRelease(T*& p) {
  if (p) { p->Release(); p = nullptr; }
}

std::string Hr(const char* what, HRESULT hr) {
  char b[128];
  std::snprintf(b, sizeof(b), "%s failed (0x%08lX)%s", what, (unsigned long)hr,
                hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ? " DEVICE_REMOVED" : "");
  return b;
}

constexpr uint32_t kTableSrvs = 97;  // t0-t96
constexpr uint32_t kUploadCbBytes = 8192;        // per set: cb0 256 + cb1 4096 + cb2 256 + cb3 768 (aligned)
constexpr uint32_t kCb0 = 0, kCb1 = 256, kCb2 = 256 + 4096, kCb3 = 256 + 4096 + 256;
constexpr uint32_t kPixelInputBytesPerSet = kLanes * 16 * 16;
constexpr uint32_t kOutBytesPerSet = kLanes * kVsOutputs * 16;

class D3D12Runner final : public GpuRunner {
 public:
  ~D3D12Runner() override {
    if (fence_event_) CloseHandle(fence_event_);
    for (auto*& t : textures_) SafeRelease(t);
    SafeRelease(guest_);
    SafeRelease(upload_cb_);
    SafeRelease(upload_in_);
    SafeRelease(out_);
    SafeRelease(readback_);
    SafeRelease(heap_);
    SafeRelease(root_);
    SafeRelease(list_);
    SafeRelease(allocator_);
    SafeRelease(fence_);
    SafeRelease(queue_);
    SafeRelease(device_);
  }

  std::string Describe() const override { return description_; }

  bool Init(const uint8_t* guest_memory, std::string& error) override {
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
    if (FAILED(hr)) { error = Hr("D3D12CreateDevice", hr); return false; }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
      error = "device lacks Shader Model 6.6";
      return false;
    }
    {
      IDXGIFactory4* factory = nullptr;
      if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        const LUID luid = device_->GetAdapterLuid();
        if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
          DXGI_ADAPTER_DESC1 d{};
          adapter->GetDesc1(&d);
          char name[128] = {};
          WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
          description_ = std::string("D3D12 ") + name + ", Shader Model 6.6";
          adapter->Release();
        }
        factory->Release();
      }
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (FAILED(hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) { error = Hr("CreateCommandQueue", hr); return false; }
    if (FAILED(hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)))) { error = Hr("CreateCommandAllocator", hr); return false; }
    if (FAILED(hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_, nullptr, IID_PPV_ARGS(&list_)))) { error = Hr("CreateCommandList", hr); return false; }
    if (FAILED(hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) { error = Hr("CreateFence", hr); return false; }
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!CreateRootSignature(error)) return false;

    // Buffers.
    if (!CreateBuffer(kGuestMemoryBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &guest_, error)) return false;
    if (!CreateBuffer(kSets * kUploadCbBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &upload_cb_, error)) return false;
    if (!CreateBuffer(kSets * kPixelInputBytesPerSet, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &upload_in_, error)) return false;
    if (!CreateBuffer(kSets * kOutBytesPerSet, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &out_, error)) return false;
    if (!CreateBuffer(kSets * kOutBytesPerSet, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &readback_, error)) return false;

    // Descriptor heap: t0-t96.
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kTableSrvs, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    if (FAILED(hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) { error = Hr("CreateDescriptorHeap", hr); return false; }
    const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Upload everything (guest memory and the 96 textures) through one staging buffer.
    std::vector<std::pair<ID3D12Resource*, uint32_t>> tex_info;  // resource, dimension
    for (uint32_t dim = 0; dim < 3; ++dim) {
      for (uint32_t k = 0; k < 32; ++k) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = dim == 2 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 4;
        rd.Height = 4;
        rd.DepthOrArraySize = dim == 0 ? 1 : (dim == 1 ? 6 : 4);
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        rd.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        ID3D12Resource* t = nullptr;
        if (FAILED(hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t)))) {
          error = Hr("CreateCommittedResource(texture)", hr);
          return false;
        }
        textures_.push_back(t);
      }
    }
    // Staging layout: guest memory, then each texture's subresources (footprints from the device).
    struct Copy { ID3D12Resource* dst; UINT sub; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 row_bytes; uint32_t dim, k; };
    std::vector<Copy> copies;
    UINT64 offset = (kGuestMemoryBytes + 511) & ~UINT64(511);
    for (uint32_t i = 0; i < textures_.size(); ++i) {
      const uint32_t dim = i / 32, k = i % 32;
      D3D12_RESOURCE_DESC rd = textures_[i]->GetDesc();
      const UINT subs = dim == 1 ? 6 : 1;
      for (UINT s = 0; s < subs; ++s) {
        Copy c{textures_[i], s, {}, 0, 0, dim, k};
        UINT64 total = 0;
        device_->GetCopyableFootprints(&rd, s, 1, offset, &c.fp, &c.rows, &c.row_bytes, &total);
        offset = (c.fp.Offset + total + 511) & ~UINT64(511);
        copies.push_back(c);
      }
    }
    ID3D12Resource* staging = nullptr;
    if (!CreateBuffer(offset, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &staging, error)) return false;
    uint8_t* map = nullptr;
    staging->Map(0, nullptr, reinterpret_cast<void**>(&map));
    std::memcpy(map, guest_memory, kGuestMemoryBytes);
    for (const Copy& c : copies) {
      float colour[4];
      SlotColour(c.dim, c.k, colour);
      for (UINT z = 0; z < c.fp.Footprint.Depth; ++z) {
        for (UINT y = 0; y < c.rows; ++y) {
          float* row = reinterpret_cast<float*>(map + c.fp.Offset + UINT64(z) * c.fp.Footprint.RowPitch * c.rows +
                                                UINT64(y) * c.fp.Footprint.RowPitch);
          for (UINT x = 0; x < c.fp.Footprint.Width; ++x) std::memcpy(row + x * 4, colour, 16);
        }
      }
    }
    staging->Unmap(0, nullptr);
    list_->CopyBufferRegion(guest_, 0, staging, 0, kGuestMemoryBytes);
    for (const Copy& c : copies) {
      D3D12_TEXTURE_COPY_LOCATION dst{c.dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      dst.SubresourceIndex = c.sub;
      D3D12_TEXTURE_COPY_LOCATION src{staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      src.PlacedFootprint = c.fp;
      list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.push_back(Transition(guest_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    for (auto* t : textures_) barriers.push_back(Transition(t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    list_->ResourceBarrier(UINT(barriers.size()), barriers.data());
    const bool ok = Submit(error);
    staging->Release();
    if (!ok) return false;

    // SRVs.
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < 96; ++i) {
      const uint32_t dim = i / 32;
      D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
      sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      if (dim == 0) { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1; }
      else if (dim == 1) { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = 1; }
      else { sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = 1; }
      device_->CreateShaderResourceView(textures_[i], &sd, D3D12_CPU_DESCRIPTOR_HANDLE{h.ptr + SIZE_T(i) * inc});
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC gd{};
    gd.Format = DXGI_FORMAT_R32_TYPELESS;
    gd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    gd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    gd.Buffer.NumElements = kGuestMemoryBytes / 4;
    gd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->CreateShaderResourceView(guest_, &gd, D3D12_CPU_DESCRIPTOR_HANDLE{h.ptr + SIZE_T(96) * inc});
    return true;
  }

  bool Run(const std::vector<uint8_t>& dxil, bool vertex, const std::vector<GpuSet>& sets, std::vector<float>& out,
           std::string& error) override {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_;
    pd.CS = {dxil.data(), dxil.size()};
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) { error = Hr("CreateComputePipelineState", hr); return false; }

    uint8_t* cb = nullptr;
    upload_cb_->Map(0, nullptr, reinterpret_cast<void**>(&cb));
    uint8_t* in = nullptr;
    upload_in_->Map(0, nullptr, reinterpret_cast<void**>(&in));
    for (uint32_t s = 0; s < kSets; ++s) {
      uint8_t* base = cb + size_t(s) * kUploadCbBytes;
      std::memcpy(base + kCb0, sets[s].cb0.data(), std::min<size_t>(sets[s].cb0.size(), 256));
      std::memcpy(base + kCb1, sets[s].cb1.data(), std::min<size_t>(sets[s].cb1.size(), 4096));
      std::memcpy(base + kCb2, sets[s].cb2.data(), std::min<size_t>(sets[s].cb2.size(), 256));
      std::memcpy(base + kCb3, sets[s].cb3.data(), std::min<size_t>(sets[s].cb3.size(), 768));
      if (!vertex) std::memcpy(in + size_t(s) * kPixelInputBytesPerSet, sets[s].pixel_inputs.data(), kPixelInputBytesPerSet);
    }
    upload_in_->Unmap(0, nullptr);
    upload_cb_->Unmap(0, nullptr);

    allocator_->Reset();
    list_->Reset(allocator_, pso);
    list_->SetComputeRootSignature(root_);
    list_->SetDescriptorHeaps(1, &heap_);
    list_->SetComputeRootDescriptorTable(4, heap_->GetGPUDescriptorHandleForHeapStart());
    const D3D12_GPU_VIRTUAL_ADDRESS cbva = upload_cb_->GetGPUVirtualAddress(), inva = upload_in_->GetGPUVirtualAddress(),
                                    outva = out_->GetGPUVirtualAddress();
    for (uint32_t s = 0; s < kSets; ++s) {
      const D3D12_GPU_VIRTUAL_ADDRESS b = cbva + UINT64(s) * kUploadCbBytes;
      list_->SetComputeRootConstantBufferView(0, b + kCb0);
      list_->SetComputeRootConstantBufferView(1, b + kCb1);
      list_->SetComputeRootConstantBufferView(2, b + kCb2);
      list_->SetComputeRootConstantBufferView(3, b + kCb3);
      list_->SetComputeRootShaderResourceView(5, inva + UINT64(s) * kPixelInputBytesPerSet);
      list_->SetComputeRootUnorderedAccessView(6, outva + UINT64(s) * kOutBytesPerSet);
      list_->Dispatch(1, 1, 1);
    }
    D3D12_RESOURCE_BARRIER b1 = Transition(out_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &b1);
    list_->CopyBufferRegion(readback_, 0, out_, 0, UINT64(kSets) * kOutBytesPerSet);
    D3D12_RESOURCE_BARRIER b2 = Transition(out_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list_->ResourceBarrier(1, &b2);
    const bool ok = Submit(error);
    pso->Release();
    if (!ok) return false;

    const uint32_t outputs = vertex ? kVsOutputs : kPsOutputs;
    out.assign(size_t(kSets) * kLanes * outputs * 4, 0.0f);
    float* rb = nullptr;
    D3D12_RANGE range{0, SIZE_T(kSets) * kOutBytesPerSet};
    readback_->Map(0, &range, reinterpret_cast<void**>(&rb));
    // The UAV region per set is laid out with the shader's own stride (17 or 6 float4 per lane).
    for (uint32_t s = 0; s < kSets; ++s)
      std::memcpy(&out[size_t(s) * kLanes * outputs * 4], reinterpret_cast<const uint8_t*>(rb) + size_t(s) * kOutBytesPerSet,
                  size_t(kLanes) * outputs * 16);
    D3D12_RANGE none{0, 0};
    readback_->Unmap(0, &none);
    return true;
  }

 private:
  static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
  }

  bool CreateBuffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                    ID3D12Resource** out, std::string& error) {
    D3D12_HEAP_PROPERTIES hp{type};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    const HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(out));
    if (FAILED(hr)) { error = Hr("CreateCommittedResource(buffer)", hr); return false; }
    return true;
  }

  bool CreateRootSignature(std::string& error) {
    D3D12_ROOT_PARAMETER params[7] = {};
    for (UINT i = 0; i < 4; ++i) {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      params[i].Descriptor.ShaderRegister = i;
      params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTableSrvs, 0, 0, 0};
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &range;
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[5].Descriptor.ShaderRegister = 97;
    params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[6].Descriptor.ShaderRegister = 0;
    D3D12_STATIC_SAMPLER_DESC samplers[32] = {};
    for (UINT i = 0; i < 32; ++i) {
      samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
      samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
      samplers[i].ShaderRegister = i;
      samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rs{7, params, 32, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) {
      error = Hr("D3D12SerializeRootSignature", hr);
      if (err) { error += std::string(": ") + static_cast<const char*>(err->GetBufferPointer()); err->Release(); }
      return false;
    }
    hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
    blob->Release();
    if (FAILED(hr)) { error = Hr("CreateRootSignature", hr); return false; }
    return true;
  }

  bool Submit(std::string& error) {
    HRESULT hr = list_->Close();
    if (FAILED(hr)) { error = Hr("Close", hr); return false; }
    ID3D12CommandList* lists[] = {list_};
    queue_->ExecuteCommandLists(1, lists);
    ++fence_value_;
    queue_->Signal(fence_, fence_value_);
    if (fence_->GetCompletedValue() < fence_value_) {
      fence_->SetEventOnCompletion(fence_value_, fence_event_);
      if (WaitForSingleObject(fence_event_, 10000) != WAIT_OBJECT_0) {
        error = "GPU timeout (10 s)";
        const HRESULT reason = device_->GetDeviceRemovedReason();
        if (FAILED(reason)) error += " " + Hr("device", reason);
        return false;
      }
    }
    const HRESULT reason = device_->GetDeviceRemovedReason();
    if (FAILED(reason)) { error = Hr("execution", reason); return false; }
    return true;
  }

  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* queue_ = nullptr;
  ID3D12CommandAllocator* allocator_ = nullptr;
  ID3D12GraphicsCommandList* list_ = nullptr;
  ID3D12Fence* fence_ = nullptr;
  HANDLE fence_event_ = nullptr;
  UINT64 fence_value_ = 0;
  ID3D12RootSignature* root_ = nullptr;
  ID3D12DescriptorHeap* heap_ = nullptr;
  ID3D12Resource* guest_ = nullptr;
  ID3D12Resource* upload_cb_ = nullptr;
  ID3D12Resource* upload_in_ = nullptr;
  ID3D12Resource* out_ = nullptr;
  ID3D12Resource* readback_ = nullptr;
  std::vector<ID3D12Resource*> textures_;
  std::string description_ = "D3D12";
};

}  // namespace

std::unique_ptr<GpuRunner> CreateGpuRunner() { return std::make_unique<D3D12Runner>(); }

}  // namespace gn_oracle

#endif
