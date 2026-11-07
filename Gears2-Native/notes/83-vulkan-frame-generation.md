# 83: AMD FSR frame generation on Vulkan

`--gow2_frame_generation=fsr` with `--gow2_renderer_api=vulkan` (and `--gow2_present=ours`): AMD FidelityFX frame
generation through AMD's Vulkan frame-generation swap chain. Experimental. The D3D12 path (patches 0146-0148,
`frame_generation.cpp`) is unchanged. NVIDIA DLSS-G stays DirectX 12 only: `dlss` on Vulkan logs a line and uses AMD's.

## Pieces

| Where | What |
|---|---|
| SDK patch 69 (`patches/sdk-patch69-vulkan-frame-generation.diff`) | `rex/frame_hooks.h`: `vk_all_queues`, `vk_create_swap_chain`, `vk_before_present`, `struct VulkanSwapChainHook`, `REX_FRAME_HOOKS_VK`. `vulkan_device.cpp`: with `vk_all_queues` every graphics / compute / transfer family gets all its queues (at most 4). `vulkan_presenter.cpp`: the swap chain comes from the hook when it accepts; then its images / acquire / present / destroy go through the hook's functions, presenting on the graphics queue (queue 0) without the SDK's lock. |
| `renderer/frame_generation_vk.cpp` | AMD's side: loader, swap chain, frame-generation context, Prepare, pacing. Its own file: AMD's DX12 and VK headers can't share one translation unit (both define the same typedefs / enumerators). |
| `renderer/frame_generation.cpp` | Routes: on Vulkan, `XenosFrameGenerationInstall` calls `XenosFgVkInstall`, and the four queries (`Wanted`, `HudlessFormat`, `HudlessSize`, `SetInputs`) answer from the Vulkan side. |
| `vulkan_backend.cpp` / `gpu.h` | `BeginNativeVulkan` / `NativeTextureVulkan` (VkDevice, our recording VkCommandBuffer, a texture moved to a VkImageLayout). Kept apart from `BeginNative`: the DLSS / FSR / XeSS upscalers cast that one's result to D3D12 objects. |
| `shadow_renderer.cpp` | `FgHandOver` on Vulkan: after `PresentPacked`, the frame's depth (R32F) and motion (RG16F) are copied with `vkCmdCopyImage` into the slot of the presenter image just written, left in `SHADER_READ_ONLY_OPTIMAL`, and handed over keyed by that image's `VkImage`. |
| launcher | "AMD FSR frame generation (experimental on Vulkan)"; DLSS choices greyed "DirectX 12 only"; allowed under Proton / Wine on Vulkan (Steam Deck), still off for DirectX 12 under Wine. Needs AMD's two VK DLLs, the exe's text `Vulkan: AMD frame-generation swap chain made` and the runtime's `SDK patch 69`. No `--gow2_frame_generation_hudless` on Vulkan. |
| CMake (all four games) | builds `frame_generation_vk.cpp`; copies `amd_fidelityfx_loader_vk.dll` + `amd_fidelityfx_framegeneration_vk.dll` beside the exe. The package takes them from there (it notes a build without them). |

## Queues

AMD's swap chain (`FrameInterpolationSwapChainVK`) refuses to start unless the game, async compute, present and image
acquire queues are four different `VkQueue`s, the present family can present to the surface, and the async one can
compute. The SDK device made one queue per family, so `vk_all_queues` (set in `OnPostInitLogging`, before the device)
makes the rest. `PickQueues`: present = a spare queue of a present-capable graphics + compute family (AMD then composes
on the present queue), else any present-capable; async compute = a compute-only family first; image acquire = anything
left. With too few spare queues (some Intel / older drivers expose one graphics queue and nothing else) the log says
`not enough queues for AMD's swap chain` and the usual swap chain is made.

The game queue is shared with our renderer and the presenter (the SDK's recursive mutex per queue). AMD submits to it
from inside its present, and from its own threads, through `gameQueue.submitFunc` = `GameSubmit`, which takes that
lock. The presenter calls AMD's present without holding the lock (holding it while AMD waited for a worker that needs it
would deadlock).

## Per frame

Renderer thread: `FgHandOver` copies depth + motion into the slot for the presenter image (as D3D12) and calls
`XenosFrameGenerationSetInputs(VkImage, frame)`. Presenter thread: after its paint is submitted, `vk_before_present`
-> `BeforePresent`: the newest inputs for the painted image, the same reset / no-scene (30 frames) rules as D3D12,
`ffxConfigureDescFrameGeneration` (swapChain = the VkSwapchainKHR, generation rectangle from the presenter's guest output
rectangle), then `ffxDispatchDescFrameGenerationPrepareV2` recorded into one of three command buffers of ours and
submitted on the game queue (fence-paced, 2 s bounded waits). Then the presenter calls AMD's `vkQueuePresentKHR`.

Cap semantics are the D3D12 ones: the launcher's frame-rate cap is the on-screen rate, the game draws half of it.

## Differences from D3D12 / not yet

- **No HUD-less image** (step 4 of the D3D12 plan): the HUD is interpolated with the scene, as on D3D12 without
  `--gow2_frame_generation_hudless`. The HUD-less chain is still `!presenter_vulkan_` in the renderer.
- **Repaints are presented**: on Vulkan the swap-chain image is already acquired and drawn when the hook runs, so a
  repaint of an image already shown can't be dropped as on D3D12; its inputs are re-sent with reset. Logged as "window
  repaints presented" every 600 presents.
- **Object motion**: whatever the renderer's motion texture holds (`--gow2_upscale_object_motion` works on the shared
  passes); not separately checked on Vulkan yet.
- **Resize**: the presenter destroys AMD's swap chain (our destroy turns generation off, destroys the frame-generation
  context, waits for presents, destroys the swap-chain context, then AMD's destroy) and makes a new one, never passing
  it as `oldSwapchain`.

## Fallbacks (each one log line, usual swap chain, the game runs as before)

AMD's Vulkan loader missing; no `ffx_api` exports; the presenter gave no device / queue; device functions missing; not
enough queues; `ffxCreateContext` for the swap chain refused; no replacement functions; frame-generation context
refused (then frames shown as drawn); a build without the FFX VK headers, the Vulkan headers or a patch-69 runtime header
(`REX_FRAME_HOOKS_VK`) builds a stub that logs "this build has no Vulkan frame generation".

Risk: `FrameHooks` grew at the end. An exe built against patch-69 headers must ship with a patch-69 `rexruntime.dll`
(it writes `vk_all_queues` past an older runtime's struct). Releases ship both; dev builds must not mix.

## Proton / Steam Deck

Allowed (the Deck is the main Vulkan user). RADV exposes graphics (1), compute (4) and transfer queues, and all
families can present, which should satisfy `PickQueues`. Untested; every failure above falls back cleanly.

## Testing (not run yet)

Build: SDK patches 68 + 69 into rexruntime, Gears 2 TU6 (job 1384). Run `Test\builds\VKFG-TEST-gears2-2x.bat`; the log
should show `VulkanPresenter: SDK patch 69: swap chain made by the frame-generation hook`, `making AMD's swap chain
(... queues ...)`, `AMD frame generation <version> on`, and every 600 presents `generation on N`. Check: resize /
fullscreen toggle, exit, the frame counter at about twice the game's rate, `--gow2_fg_debug=4` (debug view).
