# 11 - Memexport (2026-10-22)

Cause of ch17's random blue/rainbow close-up characters (likely also neon guns, ch8 fire blob): draws whose vertex
shader uses memexport are skipped ("vertex shader memexport (not supported in M1)" 97 + "export mode is multipass" 97 per
300 frames in ch17); later draws read stale memory. Ruled out first: pending pipelines (0 pending, still broken),
texture staleness (always_reload), eviction (cache_mb=0) [V, job 038].

Reference: the SDK's (Xenia, BSD) dxbc_translator_memexport.cpp + draw.cpp AddMemExportRanges.

## Gears 2's checklist (16:33, verbatim)
From Gears 2 - memexport answers

1. Does Gears 2 hit memexport?
   - [V] Not in the shader corpus. notes/39 translated all 2,474 Gears 2 shaders, and none raised the "memexport" simplification (the tool writes one for every eA/eM write). notes/47 records "Gears 2 doesn't use memexport", and prim_detail.cpp is written on that assumption.
   - [G] So we should never see the "vertex shader memexport" skip. I couldn't grep our run logs to confirm, because my PC link is down right now.
   - There is no memexport roadmap item. Stages 4/5 dropped it because Gears 2 doesn't need it. Your diagnosis fits the evidence: random colours that change every run, close-ups only, and they persist with 0 pending pipelines, always_reload and cache_mb=0.
   - [G] The 97 "export mode is multipass" skips are very likely the same kind of draw: memexport-only passes, with no rasterization. Both the reference emulators and Canary still run the vertex shader for those draws when it memexports (pipeline_state.h's SkipUnlessVertexMemexport comment says the same). Implementing memexport should therefore run both groups of 97.

2. Do we have a path where vertex fetch reads GPU-current pages from the mirror?
   - Not as a feature, but most of the plumbing exists. Read UploadVertexSlot in shadow_renderer.cpp (~4120):
     a. t96 is the guest-memory mirror. Ranges of 64 KB and more are watched (GuestWatch), and are copied from CPU memory into the mirror only on first sight or after a CPU write. Between those copies the draw reads whatever is in the mirror. So memexport data in a big range survives - unless it's the first time we see that range, in which case the CPU copy overwrites it.
     b. Small ranges (vertex_direct, the default) come from the upload ring t97. That's a copy of the CPU bytes, so it never sees GPU writes.
     c. Index buffers (ReadIndices) always come from CPU memory.
   - What you'd need:
     a. In UploadVertexSlot, before (a) or (b): if any page of the range is watch_->GpuCurrent(p), skip the ring and point the fetch at t96.
     b. On first-sight or dirty copies, copy only the pages that are not GPU-current (or, if a range is mixed, copy just those CPU pages).
     c. Do the same in ReadIndices only if you find exports landing in index buffers.
   - Caveat [V]: GuestWatch::MarkGpuWritten bumps the version and records GPU-current, but it does not call Watch(). A later CPU write to that page is only noticed if the page is already watched. When you mark export pages, call Watch(range) too, so a CPU rewrite ends GPU-current.
   - Caveat [G]: the vertex_reuse memo (job 175) and the transient_reuse map are keyed on fetch constant / range plus command list plus release epoch. An export inside the same command list doesn't bump release_epoch_, so a reused ring copy could go stale. Bump an epoch, or invalidate those memos, after any exporting draw.

3. Any objection to memexport in shared layer 0 behind a switch, default off?
   - No objection - go ahead. It's a no-op for Gears 2 (no exporting shaders), so it's safe for us as long as it stays off by default and everything sits behind the vsg.memexport / psg flag.
   - Points I'd check while building it:
     a. Ordering: the export is a UAV write to the mirror, and later draws read the mirror as t96 SRV or as texture source. You need a UAV barrier, then a UAV->SRV transition, between the exporting draw and any consumer - including the untile and texture paths that read GPU-current pages (T2). The mirror's state tracking in d3d12_backend currently assumes only copies and resolves write it.
     b. Memexport-only draws (multipass / "no effect"): run them with no RTV/DSV, a null PS, and depth/stencil off. The exports must not depend on rasterization. D3D12 has no rasterizer-discard switch, so [G] either use a pipeline that writes nothing, or force the position off-screen.
     c. Every vertex must actually run. Indexed draws can skip unreferenced vertices, and that's fine - real hardware does the same. But don't let our own culling, zero-area or tessellation shortcuts skip an exporting draw.
     d. Formats: follow the SDK's DXBC memexport code (dxbc_shader_translator_memexport.cc, BSD - keep the notice): eA from the stream constant (base, index, format/endian dword), up to 5 eM per eA, the 32/64/128-bit formats, normalisation and endian swap. Out-of-range addresses get dropped, not clamped.
     e. CPU readers: Canary/edge hold fences until export data is back in guest memory (notes/47, edge command_processor.cc:764-850) for games whose CPU reads the results. For GPU-only consumers like skinned LOD0, you can skip readback at first. Do add a diagnostic: if the CPU touches a GPU-current export page before the next export (a GuestWatch write callback on one), log it, so you'll know whether readback matters.
     f. Pixel-shader memexport exists too (psg). Count it separately, even if you only implement the vertex side.
   - Please send the diff for us to merge once your test proves it. Both games share layer 0, so it goes into our patch series too.
## 17:20 - first real exporting shader (job 038 shaders-038, ch1) [V]
- 1 of 139 vertex shaders exports: vs-896985CCFAF3C47A, a GPU buffer copy - 8 x vfetch 16_16_16_16 integer (128 bytes
  per index), then two `mad eA, r0.xxxx, c255.xyxx, c6` groups of eM0-eM3 (stream constant c6; c255.xy = (0, 1) makes
  the index the y component), position = 0 (rasterizes nothing).
- Our translation: flush before each eA write, eM0-3 marked, final flush; stream constants collected = c255 and c6
  (MarkExports drops c255: it fails the 0x4B0 / 0x96 checks). DXIL and SPIR-V compile (local gn_hlsl_check + DXC).
- The eA maths stays bit-exact: c6's words are normal floats for any physical address below 512 MB (bit 30 set,
  exponent never all ones), and 0x4B000000 + index is exact below 2^23.
