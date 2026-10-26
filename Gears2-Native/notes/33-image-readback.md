# 33 - Reading images back inside rexglue (2026-10-20)

Research from the second chat (Brief 6), pasted back here. [V] verified in source, [I] inferred, [G] guess.
Used for: per-swap frame readback (replaces desktop screenshots in tools/run.ps1, so test runs stop taking focus) and the
Stage 0 per-buffer comparison (render targets and resolves).

---

The final frame can be read back cheaply after every swap with a small patch. The SDK already has a screenshot function, but it stalls, so it only suits occasional captures. Bound render targets and resolve outputs can both be found, but each needs a little SDK work to reach from outside.

All of this was checked at tag `nightly-20261002-bd833a2a`. **[V]** means verified in source, **[I]** means inferred, **[G]** means a guess. I didn't build the patch.

## 1. The final frame

**Where it lives.** `D3D12CommandProcessor::IssueSwap` is at `src/graphics/d3d12/command_processor.cpp:1894`. **[V]**
- It gets the front buffer from `texture_cache_->RequestSwapTexture(...)` (~1919). The active area is scaled by the resolution scale (~1937–1960).
- It then calls `presenter->RefreshGuestOutput(...)` (1988). Inside that callback, a compute pass applies the game's gamma ramp (256-entry or PWL) and writes into the presenter's guest-output texture, `guest_output_resource`.
- With FXAA on, the gamma pass writes to `fxaa_source_texture_`, and FXAA then writes the guest-output texture (~2105–2237).
- Both paths finish by putting the texture back in `kGuestOutputInternalState`. Then come `SubmitBarriers()` and `EndSubmission(true)` (~2249–2250).

**What's in that texture:**
- Format `DXGI_FORMAT_R10G10B10A2_UNORM` (`kGuestOutputFormat`, `include/rex/ui/d3d12/d3d12_presenter.h:48`). Its resting state is `PIXEL_SHADER_RESOURCE` (49–50). **[V]**
- The game's gamma ramp is already applied, plus FXAA if enabled. **[V]**
- Size is the active front-buffer area at the draw resolution scale (e.g. 1280×720 × scale). **[V]**
- Window scaling, FSR/CAS sharpening, letterboxing and the ImGui overlay are **not** applied yet; they happen later when the presenter paints. **[I]**

**The existing capture function** is `D3D12Presenter::CaptureGuestOutput(RawImage&)` (`src/ui/d3d12/d3d12_presenter.cpp:205–334`). **[V]**
- It takes the latest image from the presenter's mailbox and creates a fresh readback buffer, allocator and command list every call.
- It submits to the direct queue and **blocks the caller on a fence**.
- It converts to RGBX8 with `Packed10bpcRGBTo8bpcBytes` (`RawImage`, `presenter.h:103–109`). It can be called from any thread.
- Nothing in the SDK calls it.
- It's fine for occasional screenshots, but too slow for every swap: it allocates per call, waits each time, and may skip frames because it only sees the latest image.

**Smallest patch for a per-swap readback** (sketch). Insert it in the `IssueSwap` callback after the FXAA/gamma `if/else`, just before `SubmitBarriers(); EndSubmission(true);`:

```cpp
// Ring of 3 persistently mapped READBACK buffers sized with
// GetCopyableFootprints(guest_output_resource->GetDesc()).
if (REXCVAR_GET(guest_output_readback)) {
  auto& s = readback_ring_[readback_next_++ % 3];
  EnsureReadbackSlot(s, guest_output_resource);  // (re)create on size change
  PushTransitionBarrier(guest_output_resource,
      ui::d3d12::D3D12Presenter::kGuestOutputInternalState, D3D12_RESOURCE_STATE_COPY_SOURCE);
  SubmitBarriers();
  D3D12_TEXTURE_COPY_LOCATION dst{s.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dst.PlacedFootprint = s.footprint;
  D3D12_TEXTURE_COPY_LOCATION src{guest_output_resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  src.SubresourceIndex = 0;
  deferred_command_list_.D3DCopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
      ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
  s.submission = submission_current_;
  s.pending = true;
  s.frame = swap_counter_++;
}
```

Then, at the top of each `IssueSwap`, for every slot where `pending && submission <= GetCompletedSubmission()`, hand `s.mapped + footprint.Offset` (with `RowPitch`) and the frame number to a callback or worker thread, and clear `pending`.
- **Why it doesn't stall:** with a 3-deep ring the GPU thread never waits, and frames arrive 1–2 swaps late.
- **Helpers it uses** all exist: `PushTransitionBarrier`, `SubmitBarriers`, `D3DCopyTextureRegion`, `GetCurrentSubmission` / `GetCompletedSubmission` (`command_processor.h:65–89`). **[V]**
- **When frames become visible:** `submission_completed_` is refreshed during submission checks, which happen every frame. **[I]**

## 2. Render targets mid-frame

**Which path is active.** `D3D12RenderTargetCache` has two:
- `kHostRenderTargets` (the default; `path_`, `d3d12/render_target_cache.h:140`) uses real D3D12 render targets.
- `kPixelShaderInterlock` (ROV) keeps EDRAM as one raw buffer, `edram_buffer_` (`:153`), with no per-target textures. **[V]**

**What identifies a target** is `RenderTargetKey` (`include/rex/graphics/pipeline/render_target/cache.h:228–235`): **[V]**
- `base_tiles` (EDRAM base), `pitch_tiles_at_32bpp`, `msaa_samples`, `is_depth`, `resource_format`.
- Width comes from `GetWidth()` (274–279), height from `GetRenderTargetHeight` (474).
- Host resources are multiplied by `draw_resolution_scale_x/y()` (161–162).
- Pixel (0,0) of the resource is the target's EDRAM base. **[I]**

**Which targets are bound at a draw:**
- `D3D12CommandProcessor::IssueDraw` calls `render_target_cache_->Update(...)` (`command_processor.cpp:2367`). **[V]**
- `D3D12RenderTargetCache::Update` (`render_target_cache.cpp` ~1058) runs `PerformTransfersAndResolveClears` and `SetCommandListRenderTargets(depth_and_color_render_targets)` (1068–1072). Index 0 is depth and 1–4 are colour, as `RenderTarget*`. **[V]**
- Each casts to `D3D12RenderTarget`, which has `resource()` (`d3d12/render_target_cache.h:214`) and `SetResourceState()` for state tracking (228). **[V]**
- That list, `last_update_accumulated_render_targets()` (`cache.h:491`), is **protected**. The public `GetLastUpdateBoundRenderTargets()` (181) returns only a bit mask and the formats. **[V]**
- Patch needed: a public accessor, or record the five pointers in `Update` after 1072. Ownership transfers have already run by then, so the contents are current for that draw. **[I]**

**Host formats** (`render_target_cache.cpp` ~1425–1525): **[V]**

| Guest format | Host resource |
|---|---|
| 7e3 (`k_2_10_10_10_FLOAT`, including the `_AS_16_16_16_16` alias) | `R16G16B16A16_FLOAT` |
| `k_2_10_10_10` | `R10G10B10A2_UNORM` |
| `k_16_16_FLOAT` | `R16G16_TYPELESS`, drawn as `R16G16_FLOAT` |
| `k_16_16_16_16_FLOAT` | `R16G16B16A16_TYPELESS`, drawn as FLOAT |
| `k_16_16` / `k_16_16_16_16` | TYPELESS, drawn as SNORM (the range depends on `snorm16_render_target_full_range`) |
| Depth `kD24S8` | `D24_UNORM_S8_UINT` |
| Depth `kD24FS8` (20e4) | `D32_FLOAT_S8X24_UINT`, holding float32 values that are only rounded to 20e4 if the float24-conversion option is on **[I]** |

**Copying one to the CPU:**
- Do it on the GPU thread, right after `Update` in `IssueDraw`: `SetResourceState(COPY_SOURCE)` with a barrier, then `D3DCopyTextureRegion` into a readback buffer, then restore the state.
- **MSAA targets can't be copied straight to a buffer.** Use `ResolveSubresource` into a temporary texture, or a small compute shader, first.
- **Depth/stencil formats are planar.** Copy plane 0 (depth) and plane 1 (stencil) as separate subresources. **[I]**
- Use the same submission-ring approach as the swap readback to avoid stalls.

## 3. Resolves

- `IssueCopy` (`command_processor.cpp:2873`) is reached from `IssueDraw` when `edram_mode == kCopy` (2284). **[V]**
- It calls `D3D12RenderTargetCache::Resolve` (`render_target_cache.cpp:1162`). **[V]**
- `Resolve` writes the result with compute shaders into the **GPU-side guest-memory buffer** (`D3D12SharedMemory::GetBuffer()`, `shared_memory.h:37`), at offset = guest physical address. It calls `RequestRange` (1237), `UseForWriting` (1285) and `MarkRangeAsResolved` (1313), and returns `written_address` / `written_length`. **[V]**
- **Guest RAM is only updated when readback is on.** In `IssueCopy_ReadbackResolvePath` (2889), `D3DCopyBufferRegion` copies from the shared-memory buffer to readback buffers (3058/3068). The `kFast`/`kSome` modes use the previous frame's ring; the full mode waits (3074–3087). The result is then copied into guest memory (3097). With readback disabled (`d3d12_readback_resolve`), only `Resolve` runs (2880–2885). **[V]**
- **Reading a resolved texture yourself:** right after `Resolve`, call `shared_memory_->UseAsCopySource()` (`shared_memory.h:58`) and `D3DCopyBufferRegion` from `[written_address, +written_length)` into your own readback ring.
- The bytes are in the guest texture's layout: tiled, and byte-swapped according to the destination format. Detile with `texture_address::Tiled2D`. **[I]**

## 4. Threads and timing

- **Everything above happens on the command-processor worker thread** (`CommandProcessor::WorkerThreadMain`, `command_processor.cpp:200`). It records into `deferred_command_list_` (`GetDeferredCommandList`, `command_processor.h:65`) and submits in `EndSubmission`. **[V]**
- **Never `Map` and wait on that thread.** `CheckSubmissionFence` (3105) waits by design. Instead use persistently mapped READBACK buffers, record `submission_current_` when you queue the copy, and read once `GetCompletedSubmission()` has passed it. This is the same pattern the `kFast` resolve readback already uses. **[V pattern / I for your use]**
- **The presenter paints on the UI thread.** `CaptureGuestOutput` submits on its own command list and fence, so it doesn't touch the GPU thread's list, but it does block its caller. **[V]**