# 30 - Tooling and libraries: what we have, what to adopt, and which stage needs it (plan for review, 2026-10-20)

> **Carried into Gears2-Native on 2026-10-20 as a plan only.** File names such as `src/...`, `tools/...`, `patches/...` and job numbers refer to the deleted Gears2-Recomp project. They record what we learned there; none of that code exists in this project.

Planning only. Nothing here is installed, added or changed yet.
Labels: **[V]** checked in the file or repository named, **[I]** my inference.
Versions are the latest release tags on GitHub as of 2026-10-20 (read with `git ls-remote --tags`), or the vendor's download page
where there is no GitHub release. Stage numbers refer to `28-native-renderer-roadmap.md`.

---

## 1. What we already have (checked first)

**In the rexglue SDK (`third_party/rexglue-src`, versions read from the checked-out headers on your PC) [V]:**

| Library | Bundled | Latest | Used for today | Note |
|---|---|---|---|---|
| SDL3 | 3.4.14 (branch release-3.4.x) | 3.4.18 | Window, input | 4 patch releases behind |
| Dear ImGui | 1.92.5 | 1.93.0 | Debug overlay | |
| Vulkan Memory Allocator | 3.3.0 | 3.4.0 | Vulkan backend allocations | |
| Tracy | 0.13.1, option `REXGLUE_ENABLE_TRACY` ON | 0.14.1 | CPU zones | The viewer must be the same version as the client |
| Catch2 | 3.12.0, option `REXGLUE_BUILD_TESTS` OFF | 3.16.1 | SDK unit tests | |
| RenderDoc in-app API | `renderdoc_app.h`, only API 1.0.x enums; `src/ui/renderdoc_api.cpp` | RenderDoc 1.46 (API 1.6.0) | **Only detects** that RenderDoc is attached, then turns on debug markers (`src/graphics/flags.cpp`) | No capture calls anywhere |
| D3D12 debug layer + DRED | cvars `d3d12_debug`, `d3d12_break_on_error/warning` (`src/ui/d3d12/d3d12_provider.cpp`) | - | DRED breadcrumbs and page faults are switched on **only together with the debug layer**; readback is in Xenia's `command_processor.cpp` (~line 3338), not in our nrhi device | No GPU-based validation option |
| Vulkan validation | cvar `vulkan_validation_enabled` (`src/ui/vulkan/vulkan_provider.cpp`) | Vulkan SDK 1.4.363.0 layers | Off by default | Needs the Vulkan SDK (or its layers) installed |
| GPU debug markers | cvar `gpu_debug_markers`; D3D12 `BeginEvent/SetMarker` in `deferred_command_list.cpp` | - | **Xenia's D3D12 path only.** The Vulkan backend has no `vkCmdBeginDebugUtilsLabelEXT` calls, and our nrhi device has no markers at all | |
| FidelityFX | option `REXGLUE_ENABLE_FIDELITYFX` OFF, "experimental"; presenter has FSR EASU/RCAS and a temporal FSR mode (`src/ui/presenter.cpp`) | FidelityFX SDK v2.3.0 | Not built | Applies at present time to the final image only |
| DXC | headers only (commit 6b6f402) | v1.9.2609 | Xenia's shader path | |

**In our project [V]:**
- Shader pack: `tools/ucode2hlsl/run_all.py` compiles every shader with DXC (DXIL and SPIR-V) and, with `--fxc`, with FXC (DXBC).
  The pack stores SPIR-V and **FXC DXBC**; the D3D12 path loads the DXBC.
- Runtime HLSL on D3D12: our nrhi device compiles the scene uber shader with **`D3DCompile` (FXC)** (`patches/rexglue-sdk.diff`,
  `native_rhi_d3d12.cpp`), with a retry because FXC "sometimes fails with an internal error on large translated shaders".
- DXC itself: `dxc-v1.9.2609`, which is the latest stable release (16 Sep 2026). The v1.10.2605.x tags are an older preview line [V tag dates].
- Unit tests: one custom program, `tests/xenos_texture_test.cpp`, no framework.
- Screenshot comparison: none. We compare by eye and by per-draw logs (job 229 tool).

**Outdated against your "libraries fully updated" rule:** SDL3, ImGui, VMA, Tracy, Catch2 and the RenderDoc header. They come from the SDK's
submodule pins, so updating them means bumping pins in our SDK patch and re-running the build and a play-test. I'd do it once, as
part of Stage 0, not piecemeal.

---

## 2. Evaluation

| Tool | License | Version | What it replaces | Stage | Verdict |
|---|---|---|---|---|---|
| **NVIDIA FLIP** | BSD-3-Clause [V LICENSE] | v1.7 (README; no release tags; also `pip install flip-evaluator`) | Eyeballing screenshot pairs, and the ad-hoc PPM diffs | **0** | **Adopt.** C++ single header `FLIP.h` (CPU and CUDA), so the harness can score emulator-vs-ours pairs, write an error map, and flag frames above a threshold. Runs on your 3080 Ti through CUDA [I]. |
| **RenderDoc in-app API** | MIT (RenderDoc) | RenderDoc 1.46, API 1.6.0 | Manual captures, and guessing which frame went wrong | **0** | **Adopt, upgrade the header.** Use `StartFrameCapture`/`EndFrameCapture` and `SetCaptureFilePathTemplate` so the harness captures exactly the frame where FLIP crosses the threshold, on both renderers. Needs RenderDoc installed (decision 8, approved). |
| **WinPixEventRuntime** | MIT (source: microsoft/PixEvents) [V LICENSE.txt] | NuGet 1.0.240308001 (Mar 2024; repo updated Jul 2026) | Nothing that strictly needs it | 0 (optional) | **Not needed at first.** Plain `ID3D12GraphicsCommandList::BeginEvent(1, string)` (what the SDK already does) shows up in PIX and RenderDoc. The runtime only adds colours, formatted names and CPU-side events. Programmatic PIX capture (`PIXBeginCapture`) comes from `WinPixGpuCapturer.dll`, which ships with PIX itself, not this package [I]. |
| **Vulkan debug labels** (`VK_EXT_debug_utils`) | Khronos (part of Vulkan) | - | Unnamed Vulkan work in captures | 0, then 6 | **Adopt in nrhi** together with D3D12 markers: one `Cmd::BeginLabel/EndLabel` in our device, named by pass (`light 00700263`, `tonemap B6B6DD4E`, …). Today neither backend of our device labels anything [V]. |
| **Nsight Aftermath** | NVIDIA proprietary SDK license; the runtime DLL is redistributable under it [I: read the license in the download before shipping it] | 2026.3 (developer.nvidia.com) | Guessing at TDRs / device-removed | 0 (nightly), 6 | **Adopt for developer builds only.** GPU crash dumps with the failing shader and marker; works on D3D12 and Vulkan. NVIDIA-only, so not a replacement for DRED. |
| **DRED** | Part of D3D12 | - | - | 0 | **Already there; fix how it's used.** Turn DRED on independently of the debug layer (it's cheap), and add breadcrumb/page-fault readback to our nrhi device's device-removed path, since the existing readback only runs in Xenia's processor. |
| **Validation layers in a nightly run** | D3D12 debug layer (Windows "Graphics Tools" feature); Vulkan layers Apache-2.0 | Vulkan SDK 1.4.363.0 | - | 0 | **Adopt.** The nightly unattended run (decision 9) plays one scripted route with `--d3d12_debug=true` (and GPU-based validation, which needs a new cvar), collects every message into the report, and fails on new errors. Installing "Graphics Tools" changes Windows features, so you'd click that yourself. |
| **Tracy** | BSD-3-Clause [V] | 0.14.1 (bundled 0.13.1) | Our own `gow2_profiler.cpp` / frame timer | 0 | **Already there.** Add GPU zones for nrhi passes later; FPS isn't a priority, so this is low value for now. Bump with the other pins. |
| **GoogleTest** | BSD-3-Clause | v1.18.0 | - | - | **Don't adopt.** |
| **Catch2** | BSL-1.0 | 3.16.1 (bundled 3.12.0) | Our hand-rolled `tests/xenos_texture_test.cpp` | **0, used from 1 on** | **Adopt (it's already in the SDK).** Unit tests for format conversions (7e3, 20e4/DepthF24, 2_10_10_10), untiling, header and device-struct layouts, resolve rules. These tests are game-free and run in seconds. **Reference implementation:** Xenia Canary / Xenia Edge conversion and untiling functions (BSD-3-Clause), never xenia-project master (its 7e3 decode is buggy) - details, line numbers and expected values in `31-stage0-references.md`. Two test frameworks would be one too many, so Catch2 over GoogleTest. |
| **DXC for DXIL and SPIR-V (drop FXC)** | NCSA / MIT (DXC) | v1.9.2609 (we already have it) | FXC: `d3dcompiler_47` at runtime and `fxc.exe` in the pack | **1** (pack), **3** (runtime) | **Adopt, with a comparison.** DXC is maintained; FXC is frozen and already hits internal errors on our largest shaders [V]. One compiler for both backends. Risk: DXIL and DXBC can round differently (denormals, `precise`, min-precision), which would change pixels. So the switch is a pack flag, and FLIP compares the two on the harness routes before it becomes default. Runtime compile moves to `IDxcCompiler3` (ships `dxcompiler.dll` + `dxil.dll`). **Extra benefit:** SM6 with descriptor heaps removes FXC/SM5's 16-sampler limit, so the vertex-texture slot folding and the skip-on-clash rules are no longer needed. **Before the FLIP comparison against the FXC pack, pin DXC's float behaviour explicitly:** denormal mode (`-denorm preserve` or `ftz`, chosen to match FXC/Xenos), IEEE strictness (`-Gis`), no fast-math reassociation. Record the chosen flags in `00-decisions.md`. |
| **D3D12 Memory Allocator** | MIT [V LICENSE.txt] | v3.2.0 (repo updated 12 Oct 2026) | Committed resources per allocation in nrhi | **1** | **Adopt in Stage 1.** Stage 1 creates our own resources for every game texture/buffer; D3D12MA gives pooling, aliasing and budget queries, and pairs with VMA on Vulkan. |
| **D3D12 Agility SDK** | Microsoft Agility SDK license; the redistributable `D3D12Core.dll` and `d3d12SDKLayers.dll` ship next to the exe | 1.619.6 retail (14 Sep 2026); the features below arrived in 1.608.0 | The Windows-installed D3D12 runtime, whose version varies by Windows build | **0** (ship it), **3** and **5** (use it) | **Adopt.** One runtime version on every PC, and the matching debug layer for the nightly. Features we need: **independent front/back stencil reference and masks** (Xenos has separate front/back stencil state; plain D3D12 has one reference and one mask), used in Stage 5 state and Stage 3 draws; **triangle fans** (Xenos draws fans; today they must be converted to lists), used in Stage 3; **Enhanced Barriers** (explicit layouts and per-subresource sync, a better fit for EDRAM-style resolves and aliasing), used in Stages 1-2. |
| *Later (Stage 7)* | | | | | |
| **Streamline** (DLSS, Reflex) | Core: MIT [V license.txt]. The DLSS (NGX) part is under the **NVIDIA RTX SDKs License** [V `external/ngx-sdk/license.txt`]: binary-only distribution, no reverse engineering, and clause 2(e) forbids using it "in any manner that would cause it to become subject to an open source software license" (licenses that require source disclosure, derivative-work rights or free redistribution) | v2.14.16 | - | 7 | **Later.** DLSS needs motion vectors and jittered rendering; Gears 2 has a velocity buffer for motion blur [I, to check], so it's feasible but not trivial. |
| **FSR (FidelityFX SDK)** | AMD license; redistribution "in binary form only" [V docs/license.md] | v2.3.0 | The SDK's experimental FSR presenter path | 7 | **Later.** Spatial FSR already exists in the SDK presenter; temporal FSR has the same inputs as DLSS. |
| **XeSS** | Intel Simplified Software License: binaries only, no modification or reverse engineering [V LICENSE.txt] | v3.0.2 | - | 7 | **Later, lowest priority.** Same inputs as DLSS/FSR. |
| **SMAA** | MIT-style (Jimenez et al.) [V LICENSE.txt] | v2.8 (2013, unchanged) | No AA / 2x MSAA | 7 | **Adopt when AA is chosen.** Header-only HLSL, no temporal inputs needed, so it's the cheapest real AA for the post-tone-map image. |
| **DirectXTex** | MIT [V LICENSE] | may2026 tag (repo updated 15 Oct 2026) | stb + our PPM screenshots; hand-written BC handling | 0 (screenshots), 7 | **Adopt for tooling** (PNG, BC decode in tests). **EXR needs the OpenEXR library in DirectXTex**, so EXR buffer dumps use **tinyexr v1** (BSD-3, header + miniz as C) instead (`31`). Not needed at runtime. |

---

## 3. Order of adoption (proposal)

- **Stage 0 (harness):**
  - FLIP, the RenderDoc API upgrade, D3D12 markers and Vulkan labels in nrhi, and DRED on by default in developer builds with nrhi readback.
  - Aftermath for developer builds, the nightly validation run, Catch2 tests, DirectXTex for screenshots.
  - The SDK pin bumps (SDL3, ImGui, VMA, Tracy, Catch2), **only after** the baseline screenshots, buffers and timings are recorded (`28` Stage 0).
  - The Agility SDK.
  - **Separate runs:** RenderDoc capture, Aftermath, and GPU-based validation each get their own pass; GBV only on one short route.
- **Stage 1 (resources):** D3D12MA. DXC replaces FXC in the pack, behind a flag and FLIP-checked.
- **Stage 3 (draws):** DXC replaces runtime `D3DCompile`.
- **Stage 7:** SMAA first (cheapest, no temporal data), then temporal upscalers via Streamline (DLSS), FSR and XeSS.

Downloads still needing your OK when work starts: Vulkan SDK (only if we test Vulkan), Nsight Aftermath SDK (NVIDIA developer account),
and the Windows "Graphics Tools" optional feature. FLIP, D3D12MA, SMAA and DirectXTex are source repos that the build fetches like the SDK's
other third-party code.

Sources:
- [NVlabs/flip](https://github.com/NVlabs/flip)
- [baldurk/renderdoc](https://github.com/baldurk/renderdoc)
- [WinPixEventRuntime on NuGet](https://www.nuget.org/packages/WinPixEventRuntime)
- [microsoft/PixEvents](https://github.com/microsoft/PixEvents)
- [Nsight Aftermath SDK](https://developer.nvidia.com/nsight-aftermath/getting-started)
- [KhronosGroup/Vulkan-ValidationLayers](https://github.com/KhronosGroup/Vulkan-ValidationLayers)
- [wolfpld/tracy](https://github.com/wolfpld/tracy)
- [catchorg/Catch2](https://github.com/catchorg/Catch2)
- [google/googletest](https://github.com/google/googletest)
- [microsoft/DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler)
- [GPUOpen D3D12MemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator)
- [NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline)
- [GPUOpen FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
- [intel/xess](https://github.com/intel/xess)
- [iryoku/smaa](https://github.com/iryoku/smaa)
- [microsoft/DirectXTex](https://github.com/microsoft/DirectXTex)

## 4. Open source and DLSS (decision P11)
**Question for you:** will Gears2-Native be open source? Our own code can be (rexglue is BSD-3-Clause; the game files never are).

**What it means for DLSS [V license text, I for interpretation]:**
- **With a permissive licence (MIT or BSD):** the usual practice is to keep the NVIDIA files outside our licensed source tree and
  ship `nvngx_dlss.dll` / the Streamline plugins as separate binaries under NVIDIA's terms, with NVIDIA's notice. That looks
  compatible with clause 2(e), because MIT/BSD don't force NVIDIA's part to become open.
- **With GPL (copyleft):** a GPL licence could be read as requiring the combined work, including NVIDIA's part, to be redistributable
  in source, and clause 2(e) forbids exactly that. So no GPL if we want DLSS.
- **FSR and XeSS:** both are binary-only too (AMD: "in binary form only"; Intel: no modification), with the same permissive-licence
  pattern [V].
- **FFmpeg** (inside the SDK, for audio) is LGPL or GPL depending on how it's built [I, to check in Stage 6]. That matters for
  how we ship it, not for DLSS.
- **NVIDIA's own contact** for licence questions is in the licence text (`nvidia-rtx-license-questions@nvidia.com`).

I'm not a lawyer. Before a public release, someone qualified should read these.

**Recommendation:** open source under MIT or BSD-3-Clause (matching rexglue), with DLSS/FSR/XeSS shipped as separate vendor binaries.

Sources (additions):
- [D3D12 Agility SDK downloads](https://devblogs.microsoft.com/directx/directx12agility)
- [NVIDIA RTX SDKs License](https://developer.download.nvidia.com/gameworks/NVIDIA-RTX-SDKs-License-23Jan2023.pdf)
- [NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline)
