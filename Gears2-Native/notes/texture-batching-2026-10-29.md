# Batched texture loads and two small CPU savings

October 29, 2026. The changes below are D3D12 only and haven't been run yet. They passed a syntax check against the
tree at `bbb7cee`. Turn the main change off with `--gow2_renderer_texture_batch=false` to compare.

## The freeze this targets

Gears 2 log `20261029-114138`, frame 8321: the first gameplay frame after a cutscene. It had 4,221 draws, uploaded
98.55 MB, spent 50.5 ms in texture work and created no shaders or pipelines. In the next frames the GPU work took
126, 58, 61 and 27 ms, against about 9.4 ms in steady play. That window loaded 491 BC textures on the GPU path, which
is 4,872 subresources, and made no CPU-path loads.

Each BC subresource was loaded the same way:

1. A compute pass untiles it into the single scratch buffer, which is in UAV state.
2. A barrier moves the scratch buffer to COPY_SOURCE and the texture to COPY_DEST.
3. A copy moves it into the texture.
4. The next subresource needs the scratch buffer back in UAV state, so the GPU waits for that copy before it can
   start the next pass.

That gives about 4,900 strictly serialized pass → barrier → copy → barrier round trips in one frame, with the GPU
mostly idle. Raw (non-BC) GPU loads also put a UAV barrier after every mip's untile pass. Each texture's upload also
switched the guest-memory mirror between COPY_DEST and shader-resource state once per texture.

## What changed

- **Uploads first, then untiles.** Within one draw's texture binding (`TextureCache::Bind`), every texture's
  guest-memory upload is recorded first. The untile work runs at the end of the binding, through `RunGpuJobs`. The
  mirror now changes state once per draw instead of twice per texture.
- **One scratch buffer, many regions (D3D12).**
  - Each BC subresource gets its own 512-byte-aligned region of the scratch buffer, which is now 32 MB.
  - Each pass writes through a raw UAV view that starts at its region, so the shader is unchanged.
  - The copies are queued and recorded together: one scratch barrier, one batch of texture barriers, then all the
    copies.
  - The queue is flushed by:
    - any recording call other than the texture-load ones (`Recording()`);
    - `SetTextures`;
    - a transition of a queued texture;
    - `DestroyTexture`;
    - `Submit`;
    - a full scratch buffer.
- **No barrier after each untile pass.** A raw untile pass no longer adds a UAV barrier after itself
  (`ComputeBindings::defer_uav_barrier`). The texture's next use is a transition, which waits for the writes. Writing
  the same mip as a UAV again gets the barrier first (`Texture::uav_unbarriered`).
- **Pipeline memo.** `GetGraphicsPipeline` remembers the last ready pipeline. A draw with the same description
  skips the hash and map lookup. Pipelines are never destroyed. The "pipelines … (same as the previous draw N)" count
  in the CPU split line shows the hits.
- **Page watch.** `GuestWatch::Bump` adds once per 256 KB block and once per 4 MB block instead of once per page. The
  sums are identical (checked against the old code on 20,000 random ranges). A 4 MB widening now takes 1,041
  atomic adds instead of 3,072.

What was left alone:

- The Vulkan backend is unchanged, and the texture-cache reordering is switched off for Vulkan.
- No shader text changed, so the shader caches stay valid.

## How to test

1. Replay the same Gears 2 transition, once as normal and once with `--gow2_renderer_texture_batch=false`. Compare the
   frame log rows for the first gameplay frames: `our_gpu_ms`, `renderer_top_part_ms` and `frame_ms`.
2. Run once with `--d3d12_debug=true` through a level start. The debug layer reports resource-state mistakes in the
   log. There should be no new errors.
3. Add `--gow2_texture_phase_log_ms=20` to see how the remaining texture time splits between hashing, creation,
   upload and the GPU path.
4. The "patch 0021" log line now ends with "in N batches, untile barriers left out M".

The Gears 2 query wait needs no code change. `--gow2_patch_query_wait=1` already makes Gears 2's query wait sleep
the way Gears 3 and Judgment do when uncapped. The default, 2, spins when uncapped. To compare them, add the switch
and keep everything else the same.
