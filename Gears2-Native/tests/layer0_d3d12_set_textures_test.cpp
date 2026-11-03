// GitHub #146: D3D12Backend::SetTextures read 96 views and 32 samplers whatever counts it was given. XeGTAO's AO pass
// (ShadowRenderer::AoAddPass) binds one of each from one-element stack arrays, so every pass read past them, and a
// render thread whose arrays sat at the top of its stack faulted (Gears 3, Act 1 "Anchored", a cutscene's first frame).
// Here the arrays end at a page with no access after it: a read past them faults.
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <new>

#include <xenos/renderer/gpu.h>

namespace gpu = xenos::renderer::gpu;

namespace {
// A writable page followed by a no-access page; one T placed so that it ends where the first page does.
template <typename T>
struct GuardedOne {
  GuardedOne() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    page = si.dwPageSize;
    base = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD old = 0;
    VirtualProtect(base + page, page, PAGE_NOACCESS, &old);
    value = new (base + page - sizeof(T)) T{};
  }
  ~GuardedOne() { VirtualFree(base, 0, MEM_RELEASE); }
  size_t page = 0;
  uint8_t* base = nullptr;
  T* value = nullptr;
};
}  // namespace

TEST_CASE("D3D12 SetTextures reads only the views and samplers it is given", "[layer0][d3d12]") {
  GuardedOne<gpu::TextureView> view;
  GuardedOne<gpu::SamplerDesc> sampler;
  REQUIRE(view.base);
  REQUIRE(sampler.base);
  auto backend = gpu::CreateD3D12Backend();  // no device needed: SetTextures only records what is bound
  REQUIRE(backend);
  backend->SetTextures(view.value, sampler.value, 1, 1);  // read past either one: an access violation
  backend->SetTextures(view.value, sampler.value, 1, 1);  // the same binding again (the reuse check reads them too)
  SUCCEED("one view and one sampler read");
}
