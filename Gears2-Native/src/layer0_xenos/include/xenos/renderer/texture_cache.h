// Layer 0 (game-free): guest textures for our renderer (S3.5 M2 start, notes/42 "Textures"). For every texture fetch
// constant a draw's shaders use, finds or creates the host texture holding that guest texture: all mips, untiled
// (xenos/texture_layout.h: Tiled2D/3D addressing, packed mip tails, the full guest layout), endian-swapped and converted
// to a host format (BC1/2/3/4/5 kept compressed; DXT3A, CTX1, 24-bit depth and 10:11:11 formats converted on the CPU).
// The fetch constant's swizzle and signedness become the view; its filter/address/LOD fields become the sampler. Gamma,
// biased components and the exponent adjust stay in the translated shader (notes/39), so texel data is uploaded raw.
//
// Invalidation: GuestWatch (CPU writes seen by the guest memory system, plus our resolves). A texture is re-uploaded
// when the version of its guest bytes changed since its last upload, checked at most once per frame per texture unless
// a callback arrived meanwhile.
//
// Host format table and swizzles follow Xenia Canary e6cd2e4 d3d12/d3d12_texture_cache.h host_formats_ (BSD-3-Clause,
// Copyright (c) Ben Vanik and Xenia project contributors); the CPU decoders for DXT3A and CTX1 follow Canary's
// texture_load_dxt3a / texture_load_ctx1 shaders. Our code (MIT) otherwise.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <xenos/renderer/gpu.h>
#include <xenos/renderer/guest_watch.h>

namespace xenos::renderer {

// Computes the 6-qword metadata + content XXH3 texture hash used by ReXGlue texture packs (rex_hash), where
// `swap_hash` is the XXH3_64bits of the base-level guest bytes.
uint64_t ComputeRexTextureHash(uint64_t swap_hash, uint32_t width, uint32_t height, uint32_t layers,
                               uint32_t pitch_div_32, uint32_t format, uint32_t endian, uint32_t dim, bool tiled,
                               bool packed, uint32_t max_level, uint32_t base_extent_bytes,
                               uint32_t mips_extent_bytes);

// In-memory dynamic texture overlays (auto-switching prompt glyphs and unit tests). `overlay_data` may be either raw
// [width:u32, height:u32, rgba...] or prefixed with the 8-byte "COGBOXOV" file header; empty removes the overlay.
// Calling SetDynamicOverlay, SetDynamicOverlayShared, or ClearDynamicOverlays automatically advances OverlayGeneration().
void SetDynamicOverlay(uint64_t hash, std::vector<uint8_t> overlay_data);
void SetDynamicOverlayShared(uint64_t hash, std::shared_ptr<const std::vector<uint8_t>> overlay_data);
void SetSuppressDiskOverlaysForPrompts(bool suppress);
void ClearDynamicOverlays();
uint64_t BumpOverlayGeneration();
uint64_t OverlayGeneration();

struct TextureCacheStats {
  uint64_t textures = 0, uploads = 0, upload_bytes = 0, bound = 0, stand_ins = 0;
  uint64_t gpu_loads = 0, gpu_load_bytes = 0, gpu_loads_stacked = 0;
  uint64_t gpu_block_loads = 0, gpu_block_subresources = 0, gpu_block_failures = 0, cpu_loads = 0;  // patch 0021
  // Patch 0087: loads whose packed mip tail (level 0 packed, mip address 0) was read from the base address (Canary
  // 344fca7), and loads whose fetch constant has a per-axis gradient exponent bias (dword 4 bits 22-31; not applied).
  uint64_t packed_tail_at_base = 0, grad_exp_bias_loads = 0;
  uint64_t gpu_volume_loads = 0;  // patch 0026  // loads done by the GPU untile (guest bytes copied to the GPU)
  uint64_t host_bytes = 0;                      // all live host textures
  // upl-n108: re-checks whose written pages all hashed the same as before (no reload), pages hashed for them, and
  // textures evicted by the host-memory cap.
  uint64_t content_unchanged = 0, pages_hashed = 0, evictions = 0, evicted_bytes = 0;
  uint64_t pages_skipped = 0, volatile_reloads = 0;  // upl-n109: pages not hashed / reloads of volatile textures
  // Takeover T2: pages our resolves wrote on the GPU - kept (not re-uploaded from CPU bytes), counted as changed - and
  // CPU-path loads that read such pages (their CPU bytes are not ours: shown as a count, should stay 0).
  uint64_t gpu_pages_kept = 0, gpu_pages_changed = 0, cpu_loads_of_gpu_pages = 0;
  // Stage 7: textures loaded at the resolution scale from our scaled resolves (live entries, loads, bound slots).
  uint64_t scaled_textures = 0, scaled_loads = 0, scaled_bound = 0, scaled_bytes = 0;
  std::map<std::string, uint64_t> scaled_misses;  // job 189: resolve-written textures not taken at the scale, by reason
  std::map<std::string, uint64_t> stand_in_reasons;  // why a slot got the white stand-in
  // Job 047 resolve-through: resolves that wrote a cached texture directly (its reload skipped); resolves that matched
  // a texture whose content was already out of date (normal reload); writes whose version moved meanwhile.
  uint64_t through_writes = 0, through_out_of_date = 0, through_raced = 0;
  // Patch 0137 (frame-rate research): where texture binding time goes as the cache grows. Slot-cache hits (no lookup)
  // and misses; lookups that ended at "no write anywhere" (epoch), at "its pages unchanged" (version walk), or went on
  // to re-check bytes; 4 KB pages walked by those version checks; watch epoch changes seen by Bind.
  uint64_t slot_hits = 0, slot_misses = 0, lookups = 0, lookup_epoch_hits = 0, lookup_version_hits = 0;
  uint64_t lookup_rechecks = 0, version_pages = 0, epoch_changes = 0;
};

class TextureCache {
 public:
  TextureCache(gpu::Backend& backend, const uint8_t* guest_physical_base, GuestWatch& watch)
      : backend_(backend), guest_(guest_physical_base), watch_(watch) {}
  // `fetch` = the 192 fetch-constant dwords (register 0x4800 on). Fills views (index = binding register t0-t95) and
  // samplers (s0-s31) for the slots in the three masks. Returns the number of slots that got a stand-in.
  uint32_t Bind(const uint32_t* fetch, uint32_t slots_2d, uint32_t slots_cube, uint32_t slots_3d, uint64_t frame,
                gpu::TextureView views[96], gpu::SamplerDesc samplers[32]);
  const TextureCacheStats& Stats() const { return stats_; }
  uint64_t CreateRetries() const { return create_retries_total_; }  // patch 0149: session totals
  uint64_t ScaledDropped() const { return scaled_evicted_total_; }
  // GPU loads (upl-n106): uncompressed 2D textures are untiled by this compute pipeline (builtin kUntileCs) from the
  // guest-memory buffer instead of on the CPU. 0 = CPU only.
  void SetGpuUntile(gpu::PipelineHandle untile) { untile_ = untile; }
  // Gears 3: the same untile writing one z slice of a 3D host texture (stacked textures fetched as 3D).
  void SetGpuUntile3D(gpu::PipelineHandle untile3d) { untile3d_ = untile3d; }
  // Patch 0021 (Canary texture_cache): BC1-5 textures (2D, arrays, cubes) also load on the GPU from the mirror through
  // this pipeline (builtin kUntileBlocksCs) and the backend's scratch buffer. 0 = CPU path, as before.
  void SetGpuUntileBlocks(gpu::PipelineHandle untile_blocks) { untile_blocks_ = untile_blocks; }
  // Batched texture loads (--gow2_renderer_texture_batch): a GPU-path load's untile dispatches (and BC scratch copies)
  // are recorded at the end of the draw's Bind, after every upload that Bind made, instead of between them - so the
  // guest-memory mirror changes state once per draw instead of twice per texture, and the backend can batch the scratch
  // copies (notes/texture-batching-2026-10-29.md).
  void SetBatchLoads(bool on) { batch_loads_ = on; }
  // Patch 0026: true 3D textures (32/64/128bpp raw-copy formats) untiled on the GPU into a 3D host texture.
  void SetGpuUntileVolume(gpu::PipelineHandle untile_volume) { untile_volume_ = untile_volume; }
  void ResetCounters();
  // Host-memory cap (upl-n108): past it, textures not bound this frame or the previous one are destroyed, least
  // recently used first, down to 90% of the cap. 0 = no cap. Call EndFrame once per frame.
  void SetHostMemoryCap(uint64_t bytes) { cap_bytes_ = bytes; }
  // Job 150 diagnostic (the ch18 title overwritten): a texture unchanged for at least `frames` frames and then reloaded
  // with changed bytes is reported, with whether its pages were last written by our resolves (GPU) or the CPU.
  struct StaleReload {
    uint32_t base_start = 0, base_length = 0, mip_start = 0, mip_length = 0;
    uint32_t guest_format = 0, width = 0, height = 0;
    uint64_t frame = 0, loaded_frame = 0;
    bool gpu_pages = false;  // any of its pages holds our resolve's data (takeover T2)
  };
  // Job 042 (Gears 3 ground flicker): called before a CPU-path load reads guest memory that our GPU wrote last (any
  // GPU-current page in [start, start + length)), so the owner can bring pending GPU results (memexport readbacks)
  // into guest memory first.
  // Job 045 diagnostic: log every texture load (new or reloaded) during this frame (0 = off).
  void SetLoadLogFrame(uint64_t frame) { load_log_frame_ = frame; }
  // Job 284 diagnostic (Gears 2 ch10 rainbow flash: a colour-grading volume texture): for textures whose guest base
  // is in `bases`, during frames [first, last], log every lookup once per frame and texture - what the cache did (hit
  // with no write since, checked unchanged, reloaded, new), the frame of its last load, and the XXH3 of its guest bytes
  // now and at that load - and save the guest bytes as <dir>/probe-<base>-<frame>.bin.
  void SetProbe(std::vector<uint32_t> bases, uint64_t first, uint64_t last, std::string dir) {
    probe_bases_ = std::move(bases);
    probe_first_ = first;
    probe_last_ = last;
    probe_dir_ = std::move(dir);
  }
  void SetBeforeCpuLoadOfGpuPages(std::function<void(uint32_t start, uint32_t length)> hook) {
    before_cpu_load_ = std::move(hook);
  }
  void SetStaleReloadReport(uint32_t frames, std::function<void(const StaleReload&)> report) {
    stale_after_ = frames;
    on_stale_ = std::move(report);
  }
  // Job 144 (slow-sharpening check): reload on every page-version change, skipping the unchanged-bytes shortcut.
  void SetAlwaysReload(bool on) { always_reload_ = on; }
  // A fetch whose dimension doesn't match the texture (cube fetch of a 2D texture, or the reverse) reads zero, as
  // Canary's null view does (default), instead of the white stand-in.
  void SetMismatchZero(bool on) { mismatch_zero_ = on; }
  // Job 189 diagnostic (scaled_misses); off by default since job 227 - it cost 45% of the renderer thread at 2x.
  void SetScaledMissLog(bool on) { scaled_miss_log_ = on; }
  // Patch 0087 (Canary texture_util.cc, 344fca7): a texture whose level 0 is already the packed mip tail keeps its whole
  // tail at the base address - also when D3D left the mip address at 0 (then we used to load level 0 only).
  void SetPackedTailAtBase(bool on) { packed_tail_at_base_ = on; }
  void EndFrame(uint64_t frame);

  // Stage 7 (notes/53, job 190): textures whose memory our resolves wrote at the resolution scale. The renderer answers
  // whether the base level (guest base address, extent in bytes) is wholly held by its scaled copy of guest memory and
  // still current; if so the texture is loaded from that copy at scale x size (kScaledLoadCs) and its slot is reported
  // in LastScaledMask(), so the shaders use the guest size for texel maths.
  struct ScaledSource {
    gpu::TextureHandle words = 0;  // the R32_UINT chunk texture holding the scaled copy of [chunk_base, + chunk_bytes)
    uint32_t chunk_base = 0, chunk_width = 0, chunk_bytes = 0;
    uint64_t gen = 0;  // changes whenever the extent's memory is written
    uint32_t reason = 0;  // on failure: 1 extent crosses a chunk / no chunk, 4 pages not scaled-current
    // Job 192 diagnostic (reason 4): pages scaled-current / written last by an unscaled GPU resolve / otherwise
    // (CPU or never), and the first page that is not scaled-current (offset from the base, bytes).
    uint32_t pages_scaled = 0, pages_gpu = 0, pages_other = 0, first_bad = 0;
  };
  using ScaledLookup = std::function<bool(uint32_t base, uint32_t extent_bytes, ScaledSource& out)>;
  void SetScaled(uint32_t scale, gpu::PipelineHandle load, ScaledLookup lookup) {
    scale_ = scale;
    scaled_load_ = load;
    scaled_lookup_ = std::move(lookup);
  }
  // Slots (bit k = fetch constant k) whose 2D view the last Bind took from a scaled texture.
  uint32_t LastScaledMask() const { return last_scaled_mask_; }

  // Job 047 (resolve-through): a resolve into tiled guest memory at `base` (pitch in texels, 4 or 8 bytes per texel)
  // can write the cached texture that the GPU untile would load from those bytes, if one exists (single level, same
  // base/pitch/size per texel) and is current - its content equals the bytes before the resolve - or the resolve
  // rectangle covers it wholly. Then the resolve shader writes it too (kWordsToGuestThroughCs) and ResolveThroughWritten
  // records it as current, so the next fetch needs no reload.
  struct ResolveThrough {
    gpu::TextureHandle texture = 0;
    uint32_t code = 0, endian = 0, width = 0, height = 0;
    void* entry = nullptr;
    uint64_t version_before = 0;
    bool covers = false;
  };
  bool FindResolveThrough(uint32_t base, uint32_t pitch, uint32_t bpb_log2, uint32_t dx, uint32_t dy, uint32_t w,
                          uint32_t h, ResolveThrough& out);
  // After the resolve's dispatch and its GuestWatch::MarkGpuWritten(extent_start, extent_length).
  void ResolveThroughWritten(const ResolveThrough& t, uint32_t extent_start, uint32_t extent_length);
  // Job 047 check: a new texture holding what the GPU untile loads from the same memory now (in queue order), to
  // compare with the texture the resolve wrote. Its texel size in bytes in `texel_bytes`. The caller destroys it.
  gpu::TextureHandle UntileCopyForCheck(const ResolveThrough& t, uint32_t& texel_bytes);

  static void SetDynamicOverlay(uint64_t hash, std::vector<uint8_t> overlay_data) {
    ::xenos::renderer::SetDynamicOverlay(hash, std::move(overlay_data));
  }
  static void SetDynamicOverlayShared(uint64_t hash, std::shared_ptr<const std::vector<uint8_t>> overlay_data) {
    ::xenos::renderer::SetDynamicOverlayShared(hash, std::move(overlay_data));
  }
  static void SetSuppressDiskOverlaysForPrompts(bool suppress) {
    ::xenos::renderer::SetSuppressDiskOverlaysForPrompts(suppress);
  }
  static void SetOverlayForTest(uint64_t hash, std::vector<uint8_t> overlay_data) {
    ::xenos::renderer::SetDynamicOverlay(hash, std::move(overlay_data));
  }
  static void ClearDynamicOverlays() { ::xenos::renderer::ClearDynamicOverlays(); }
  static uint64_t BumpOverlayGeneration() { return ::xenos::renderer::BumpOverlayGeneration(); }
  static uint64_t OverlayGeneration() { return ::xenos::renderer::OverlayGeneration(); }

 private:
  struct OverlayVariant {
    gpu::TextureHandle texture = 0;
    uint32_t ov_scale = 1;
    uint64_t host_bytes = 0;
    bool has_overlay = false;
    std::shared_ptr<const std::vector<uint8_t>> overlay_ref;
  };
  struct Entry {
    gpu::TextureHandle texture = 0;
    gpu::Format format = gpu::Format::kUnknown;
    uint32_t base_start = 0, base_length = 0, mip_start = 0, mip_length = 0;
    uint64_t version = ~0ull;
    uint64_t checked_frame = ~0ull, callbacks_at_check = 0;
    uint64_t host_bytes = 0;
    uint64_t last_used = 0;  // frame of the last Bind
    uint64_t freed_at_load = 0;  // patch 0149: freed_total_ when it was loaded (a failed creation retries after a free)
    uint64_t loaded_frame = ~0ull;  // frame of the last (re)load (job 150 diagnostic)
    uint64_t probe_load_hash = 0;   // job 284: XXH3 of the guest bytes at the last (re)load (probed textures only)
    std::string failure;  // non-empty: this guest texture can't be represented (stand-in)
    bool dimension_mismatch = false;  // the failure is a fetch-dimension mismatch (zero view, see SetMismatchZero)
    bool gpu_storage = false;  // created with UAV access: loads go through the GPU untile
    uint32_t ov_scale = 1;     // job 661: an overlay N times the guest size made the host texture N times bigger
    uint64_t overlay_gen = 0;  // overlay generation at the last Load
    bool has_overlay = false;  // whether an overlay or replacement was applied at the last Load
    uint64_t variant_swap_hash = 0;  // base-level XXH3 hash for which `overlay_variants` is valid
    std::unordered_map<const std::vector<uint8_t>*, OverlayVariant> overlay_variants;
    uint64_t TotalHostBytes() const {
      if (overlay_variants.empty()) return host_bytes;
      uint64_t sum = 0;
      for (const auto& [_, var] : overlay_variants) sum += var.host_bytes;
      return sum;
    }
    // GPU path: the page versions of [base] and [mip] last copied to the GPU's guest-memory mirror, so a reload copies
    // only the 4 KB pages written since (upl-n107).
    std::vector<uint32_t> copied_pages[2];
    uint32_t copied_start[2] = {}, copied_length[2] = {};
    // Content validation (upl-n108): per 4 KB page of [base] and [mip], the version and XXH3 of its bytes at the last
    // check. A version change re-hashes only the pages whose version moved; when every one hashes the same (a 4 MB
    // block bump, or a rewrite of the same bytes) the texture is not reloaded.
    std::vector<uint32_t> page_versions[2];
    std::vector<uint64_t> page_hashes[2];
    uint32_t hashed_start[2] = {}, hashed_length[2] = {};
    uint32_t changed_streak = 0, checks = 0;  // upl-n109: consecutive changed checks (volatile at 2)
    bool hashes_stale = false;
    // Stage 7: scaled entries - the source generation and texture last loaded from.
    uint64_t scaled_gen = ~0ull;
    gpu::TextureHandle scaled_words = 0;
    // Job 047 resolve-through: a single-level tiled 2D GPU-untile texture (base level not in a packed tail) that a
    // resolve can write directly; its untile parameters.
    bool through_ok = false, through_indexed = false;
    uint32_t through_base = 0, through_pitch = 0, through_bpb_log2 = 0, through_code = 0, through_endian = 0;
    uint32_t width = 0, height = 0;
  };
  // Stage 7: the scaled entry for a 2D fetch constant if its texture is held by a current scaled resolve, else null.
  Entry* LookupScaled(const uint32_t* tf);
  // Re-hashes the pages of `e` whose version changed; true if any page's bytes differ (or the ranges changed).
  bool PagesChanged(Entry& e, bool from_check = true);
  // 0 = 2D, 1 = cube, 2 = 3D host texture.
  Entry& Lookup(const uint32_t* tf, uint32_t host_kind, uint64_t frame);
  void Load(Entry& e, const uint32_t* tf, uint32_t host_kind);
  void StandIn(const std::string& why);

  gpu::Backend& backend_;
  const uint8_t* guest_;
  uint64_t last_bind_epoch_ = 0;  // patch 0137
  uint64_t last_bind_overlay_gen_ = 0;
  GuestWatch& watch_;
  struct Key {
    uint64_t a, b;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    // Patch 0144 (frame-rate item 1): a full 64-bit mix. The old hash (a * c ^ (b + (a >> 29))) left the texture's
    // base address (fetch dword 1 bits 12-31) out of the low bits, and the standard library picks the bucket from the
    // low bits - so textures that differ only by address shared buckets and lookups got slower as the cache grew.
    static uint64_t Mix(uint64_t x) {  // splitmix64 finaliser
      x ^= x >> 30;
      x *= 0xBF58476D1CE4E5B9ull;
      x ^= x >> 27;
      x *= 0x94D049BB133111EBull;
      return x ^ (x >> 31);
    }
    size_t operator()(const Key& k) const { return size_t(Mix(k.a ^ Mix(k.b + 0x9E3779B97F4A7C15ull))); }
  };
  std::unordered_map<Key, std::unique_ptr<Entry>, KeyHash> entries_;
  std::unordered_map<Key, std::unique_ptr<Entry>, KeyHash> scaled_entries_;  // Stage 7
  std::unordered_multimap<uint32_t, Entry*> through_;  // job 047: resolve-through entries by base address
  uint32_t scale_ = 1;
  gpu::PipelineHandle scaled_load_ = 0;
  ScaledLookup scaled_lookup_;
  uint32_t last_scaled_mask_ = 0;
  // Per fetch slot: the last result, reused while the slot's six fetch-constant dwords, the dimensions the shaders use
  // and the guest-watch epoch are unchanged (upl-n102: Bind was 8-10 ms/frame).
  struct SlotCache {
    bool valid = false;
    uint32_t tf[6] = {};
    uint32_t kinds = 0;
    uint64_t epoch = 0;
    gpu::SamplerDesc sampler;
    gpu::TextureView views[3];
    Entry* entries[3] = {};  // for the LRU stamp on a hit
    uint32_t stand_ins = 0, bound = 0;
    bool scaled = false;  // Stage 7: the 2D view is a scaled texture
  };
  SlotCache slot_cache_[32];
  uint64_t freed_total_ = 0;  // patch 0149: host textures freed this session (evicted, scaled dropped)
  uint64_t create_retries_total_ = 0, scaled_evicted_total_ = 0;
  TextureCacheStats stats_;
  std::vector<uint8_t> scratch_;
  gpu::PipelineHandle untile_ = 0;
  gpu::PipelineHandle untile3d_ = 0;
  gpu::PipelineHandle untile_blocks_ = 0;  // patch 0021
  bool batch_loads_ = false;
  std::vector<std::function<void()>> gpu_jobs_;  // batched loads: this Bind's untile work, run at its end
  void RunGpuJobs();
  gpu::PipelineHandle untile_volume_ = 0;  // patch 0026
  uint64_t cap_bytes_ = 0;
  bool always_reload_ = false;
  bool mismatch_zero_ = true;
  bool scaled_miss_log_ = false;
  bool packed_tail_at_base_ = true;  // patch 0087
  uint32_t stale_after_ = 0;
  std::function<void(const StaleReload&)> on_stale_;
  std::function<void(uint32_t start, uint32_t length)> before_cpu_load_;
  uint64_t load_log_frame_ = 0;
  void LogLoad(const Entry& e, const uint32_t* tf, uint64_t frame, const char* reason);
  std::vector<uint32_t> probe_bases_;
  uint64_t probe_first_ = 0, probe_last_ = 0;
  std::string probe_dir_;
  std::vector<std::pair<uint64_t, uint32_t>> probe_done_;  // (frame, base) already logged
  bool Probed(const uint32_t* tf, uint64_t frame) const;
  uint64_t GuestHash(const Entry& e) const;
  void Probe(const Entry& e, const uint32_t* tf, uint64_t frame, const char* what);
};

}  // namespace xenos::renderer
