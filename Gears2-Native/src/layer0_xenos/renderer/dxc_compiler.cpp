// Layer 0 (game-free): DXC in-process (IDxcCompiler3), loaded at run time from dxcompiler.dll. No link-time
// dependency: if the DLL is missing the shadow renderer logs it and stays off, the game runs as before. The build
// copies third_party\dxc-<version>\bin\x64\dxcompiler.dll and dxil.dll (cmake/gn_deps.cmake) next to gow2.exe (CMake POST_BUILD); DXC loads
// dxil.dll itself (normal DLL search order, the exe folder first) to sign the DXIL - without it D3D12 rejects the
// bytecode, which the compile messages then say.
//
// Arguments are the S3.3 pinned set (notes/39): -E main -HV 2021 -Gis -denorm ftz, Shader Model 6.6; SPIR-V adds
// -spirv -fspv-target-env=vulkan1.3 (Vulkan backend, later). Our code (MIT).
#include <xenos/renderer/shader_cache.h>

#ifndef _WIN32

namespace xenos::renderer {
std::unique_ptr<ShaderCompiler> CreateDxcCompiler(const std::filesystem::path&, std::string& error) {
  error = "DXC in-process is only wired up on Windows";
  return nullptr;
}
}  // namespace xenos::renderer

#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// dxcapi.h needs the COM base interfaces, which WIN32_LEAN_AND_MEAN leaves out of windows.h.
#include <objidl.h>
#include <unknwn.h>
#ifndef _Maybenull_
#define _Maybenull_  // SAL annotation used by dxcapi.h; the Windows SDK's sal.h defines it
#endif
#include <dxcapi.h>

#include <cstdio>
#include <vector>

#include "shared_dlls.h"  // shared049: <COGBox>\shared when no --gow2_dxc_dir

namespace xenos::renderer {
namespace {

template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

class DxcCompiler final : public ShaderCompiler {
 public:
  ~DxcCompiler() override {
    SafeRelease(compiler_);
    SafeRelease(utils_);
    // The DLL stays loaded: COM objects from it may still be referenced during process exit.
  }

  bool Init(const std::filesystem::path& dll_dir, std::string& error) {
    std::filesystem::path dir = dll_dir;
    if (dir.empty()) {
      wchar_t exe[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      dir = std::filesystem::path(exe).parent_path();
      // shared049: no --gow2_dxc_dir (a game started without the launcher) and none beside the exe -> <COGBox>\shared
      if (!gn_shared_dlls::FileThere((dir / L"dxcompiler.dll").wstring()) && !gn_shared_dlls::SharedDir().empty())
        dir = std::filesystem::path(gn_shared_dlls::SharedDir());
    }
    const std::filesystem::path dll = dir / L"dxcompiler.dll";
    // LOAD_WITH_ALTERED_SEARCH_PATH: dxil.dll is then found next to dxcompiler.dll too.
    module_ = LoadLibraryExW(dll.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module_) {
      error = "could not load " + dll.string() + " (error " + std::to_string(GetLastError()) + ")";
      return false;
    }
    auto create = reinterpret_cast<DxcCreateInstanceProc>(
        reinterpret_cast<void*>(GetProcAddress(module_, "DxcCreateInstance")));
    if (!create) {
      error = "DxcCreateInstance not found in " + dll.string();
      return false;
    }
    if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils_))) ||
        FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_)))) {
      error = "DxcCreateInstance failed";
      return false;
    }
    IDxcVersionInfo* info = nullptr;
    if (SUCCEEDED(compiler_->QueryInterface(IID_PPV_ARGS(&info)))) {
      UINT32 major = 0, minor = 0;
      info->GetVersion(&major, &minor);
      version_ = std::to_string(major) + "." + std::to_string(minor);
      info->Release();
    }
    IDxcVersionInfo2* info2 = nullptr;
    if (SUCCEEDED(compiler_->QueryInterface(IID_PPV_ARGS(&info2)))) {
      UINT32 commits = 0;
      char* hash = nullptr;
      if (SUCCEEDED(info2->GetCommitInfo(&commits, &hash)) && hash) {
        version_ += std::string(" (") + hash + ")";
        CoTaskMemFree(hash);
      }
      info2->Release();
    }
    path_ = dll.string();
    return true;
  }

  std::string Describe() const override { return "DXC " + version_ + " in-process from " + path_; }

  std::string CacheTag(gpu::ShaderIL il) const override {
    return "dxc " + version_ + (il == gpu::ShaderIL::kDxil ? (gpu::g_dxil_shader_model == 65 ? " -HV 2021 -Gis -denorm ftz sm6.5" : " -HV 2021 -Gis -denorm ftz sm6.6") :
                                                        " -HV 2021 -Gis -spirv vulkan1.3 sm6.6 shifts b0 t100 s200 u300 unkfmt");
  }

  bool Compile(const std::string& hlsl, gpu::ShaderStage stage, gpu::ShaderIL il, std::vector<uint8_t>& out,
               std::string& messages) override {
    // 0.6.1: Shader Model 6.5 on a D3D12 device without 6.6 (gpu::g_dxil_shader_model); SPIR-V keeps 6.6's HLSL rules
    const bool sm65 = il == gpu::ShaderIL::kDxil && gpu::g_dxil_shader_model == 65;
    const wchar_t* profile = stage == gpu::ShaderStage::kVertex   ? (sm65 ? L"vs_6_5" : L"vs_6_6")
                             : stage == gpu::ShaderStage::kPixel  ? (sm65 ? L"ps_6_5" : L"ps_6_6")
                             : stage == gpu::ShaderStage::kGeometry ? (sm65 ? L"gs_6_5" : L"gs_6_6")
                                                             : (sm65 ? L"cs_6_5" : L"cs_6_6");
    std::vector<const wchar_t*> args = {L"-T", profile, L"-E", L"main", L"-HV", L"2021", L"-Gis"};
    if (il == gpu::ShaderIL::kDxil) {
      args.push_back(L"-denorm");
      args.push_back(L"ftz");
    } else {
      args.push_back(L"-spirv");
      args.push_back(L"-fspv-target-env=vulkan1.3");
      args.push_back(L"-fspv-use-unknown-image-format");  // VKA1: RW images take the view format, not Rgba32f
      // Notes/68 (Vulkan backend): registers without [[vk::binding]] (our fixed shaders, the memexport u0) land in set 0 at
      // these offsets; the backend moves them into its layout (vulkan_backend.cpp RemapBindings).
      for (const wchar_t* a : {L"-fvk-b-shift", L"0", L"0", L"-fvk-t-shift", L"100", L"0", L"-fvk-s-shift", L"200", L"0",
                               L"-fvk-u-shift", L"300", L"0"})
        args.push_back(a);
    }
    DxcBuffer source{hlsl.data(), hlsl.size(), DXC_CP_UTF8};
    IDxcResult* result = nullptr;
    HRESULT hr = compiler_->Compile(&source, args.data(), UINT32(args.size()), nullptr, IID_PPV_ARGS(&result));
    if (FAILED(hr) || !result) {
      char b[64];
      std::snprintf(b, sizeof(b), "IDxcCompiler3::Compile failed (0x%08lX)", static_cast<unsigned long>(hr));
      messages = b;
      return false;
    }
    IDxcBlobUtf8* errors = nullptr;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) && errors &&
        errors->GetStringLength()) {
      messages.assign(errors->GetStringPointer(), errors->GetStringLength());
    }
    SafeRelease(errors);
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    bool ok = false;
    if (SUCCEEDED(status)) {
      IDxcBlob* object = nullptr;
      if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) && object &&
          object->GetBufferSize()) {
        const auto* p = static_cast<const uint8_t*>(object->GetBufferPointer());
        out.assign(p, p + object->GetBufferSize());
        ok = true;
      }
      SafeRelease(object);
    }
    SafeRelease(result);
    return ok;
  }

 private:
  HMODULE module_ = nullptr;
  IDxcUtils* utils_ = nullptr;
  IDxcCompiler3* compiler_ = nullptr;
  std::string version_ = "?", path_;
};

}  // namespace

std::unique_ptr<ShaderCompiler> CreateDxcCompiler(const std::filesystem::path& dll_dir, std::string& error) {
  auto c = std::make_unique<DxcCompiler>();
  if (!c->Init(dll_dir, error)) return nullptr;
  return c;
}

}  // namespace xenos::renderer

#endif  // _WIN32
