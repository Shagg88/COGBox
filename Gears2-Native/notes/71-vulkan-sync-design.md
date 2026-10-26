# 71 - Vulkan synchronisation rewrite (design, 2026-10-24)

Why (research 17:50-18:00, Shagg: "stop and do major research"):
- [Doc] NVIDIA: a global barrier or a graphics<->compute switch on one queue makes the GPU drain every warp (WFI);
  prefer image/buffer barriers with narrow stage masks, batched, no redundant read->read barriers.
- [Code] Xenia Canary's Vulkan backend (reference only, nothing copied): no global memory barrier, no ALL_COMMANDS,
  no GENERAL layout anywhere; per-resource state (stage, access, layout) with pushed image/buffer barriers, deduped
  and merged, flushed in one call; all dispatch / copy / barrier work for a draw done before (re)entering the pass;
  a pass ends only when a barrier is really pending, outside work needs it, or the targets change; render target
  images get only attachment + sampled usage.
- [V] ours: 100-220 global ALL_COMMANDS->ALL_COMMANDS barrier calls per ch15 frame, ~100 passes, everything GENERAL,
  render targets with TRANSFER_SRC|DST|SAMPLED|COLOR_ATTACHMENT. Jobs 415-429: one-switch-at-a-time changes gave
  0-0.5 ms each because the remaining global barriers kept forcing drains.

Design (behind --gow2_renderer_vulkan_sync=tracked, default old):
1. Resource state per texture: {layout, stages, access, written?}; per buffer (guest memory, scratch, index buffers):
   {stages, access} plus the last written range for the guest buffer.
2. Use(resource, usage) with usages: colour/depth attachment (RW), fragment / vertex sample, compute read / write,
   transfer src / dst, vertex/index input, present. A hazard (any write before, or a write now, or a layout change)
   pushes one VkImageMemoryBarrier2 / VkBufferMemoryBarrier2 with exact src (the recorded stages/access) and dst
   masks; read after read pushes nothing. Pending barriers merge per resource; one vkCmdPipelineBarrier2 per flush.
3. Layouts: colour targets COLOR_ATTACHMENT_OPTIMAL while drawn, SHADER_READ_ONLY_OPTIMAL while sampled,
   GENERAL only for storage-image use (resolve destinations). (NVIDIA says layouts cost nothing there; kept exact so
   AMD / Intel are right too.)
4. Pass rules: PrepareDraw collects every Use() first (textures, guest buffer for vertex fetch / memexport), then
   flushes; the pass ends only if a pushed barrier touches an attachment of the open pass or something the pass
   already used, or for outside work (dispatch / copy / clear outside / readback / submit), or new targets.
5. Image usage: render targets SAMPLED | COLOR/DEPTH_ATTACHMENT (+ TRANSFER only if a copy path needs it, checked
   per call site); storage only for resolve destinations / scratch.
6. Validation: gn_vktest with synchronization validation (VK_LAYER_ENABLES sync validation) must report no hazards;
   then ch10 picture gate on Vulkan against the old path, ch15 timing both APIs.

Before building it: one GPU profile per API (Nsight Graphics GPU Trace, ngfx CLI, one ch15 frame) to confirm the
idle gaps are WFI at our barriers / pass breaks and not shader time (the other candidate: VK_NV_raw_access_chains for
our byte-address-buffer reads - vkd3d-proton reports large NVIDIA gains for DXBC-translated shaders).

Hazards the tracked path must cover (Gears 3 18:09, Judgment 18:12) - all in the validation set:
- EDRAM aliasing: one host image's contents come from another's by transfers (D24S8 <-> D24FS8 float depth every frame
  in Gears 3 / Judgment; 7e3 colour aliased by D24S8 at the same base; MSAA transfers / clears in Gears 3). Track the
  transfer source and destination, depth + stencil aspects together. Gears 3's Vulkan black blocks may be one of these
  already missing a barrier: their job 127 T1 (pass_barriers=full) tells.
- Resolves read a few draws later in the same frame (light buffers, scene colour for post).
- Memexport: vertex-shader writes to guest memory later read as vertex / index / texture data (Judgment ~26 export
  draws/frame, 200 in menus): guest memory tracked BY RANGE (Xenia does: last written range), not one whole-buffer
  barrier per export draw.
- 7e3 targets with format conversion on transfer / resolve; CPU readbacks (transfer -> host + fence).
- Keep the global-barrier path as the fallback switch. Judgment offers a University run of the tracked mode; Gears 3
  offers ch1 (black-block room).
