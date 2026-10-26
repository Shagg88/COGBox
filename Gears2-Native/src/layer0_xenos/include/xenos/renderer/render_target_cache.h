// Layer 0 (game-free): our EDRAM model at the level the renderer needs (S3.5 step 2, D29): host render targets keyed
// by (EDRAM base, storage class, surface pitch, MSAA), and which host target last wrote each of the 2,048 EDRAM tiles,
// so a draw that reuses tiles last written through a different key is reported as an alias instead of silently
// reading stale data. Byte-level EDRAM addressing (xenos/edram.h) is only needed for transfers and is not used yet:
// D29 says transfers are added only for the reinterpretations Gears 2 is seen doing; everything else is logged.
//
// Storage classes: the colour format with the blending-precision variants folded (xenos::edram::StorageFormat - 7e3
// and its 16-bit blendable view are ONE storage), or one of the two depth formats. Host depth formats: D24FS8 targets
// are D32_FLOAT + S8 (D29: 20e4 only at resolves); D24S8 targets are D24_UNORM_S8_UINT, the format the reference
// (Canary / the SDK's RTV path) uses, so depth quantisation and the integer depth-bias unit match it (job 097 showed
// +-1 differences on 1-2% of shadow-map pixels with D32). The exact-math option keeps D32 for D24S8 too (note: the
// D24S8 constant depth bias from pipeline_state.h is in unorm24 units and is then too small on a D32 target).
//
// Host target size: width = surface pitch, height = every row the EDRAM can hold from the base at that pitch (capped
// at 8192), so a target never has to be recreated when a pass draws lower down. Our code (MIT).
//
// Resolution scale (Stage 7, notes/53): every target is drawn at `scale` host pixels per guest pixel on each axis.
// Rectangles the renderer passes in (PixelRect, gpu::Rect) are in HOST pixels; tile maths converts them to guest
// pixels (rounded outwards) and back, so callers never see guest units. The host size is the guest size x scale,
// with the guest size capped so the host size stays within D3D12's 16384.
#pragma once

#include <cstdint>
#include <bitset>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <xenos/edram.h>
#include <xenos/renderer/gpu.h>

namespace xenos::renderer {

// Colour classes are the xenos::edram::ColorFormat value of the storage format; depth classes are 0x100 | DepthFormat.
constexpr uint32_t kStorageClassDepthFlag = 0x100;
constexpr uint32_t DepthStorageClass(uint32_t depth_format) { return kStorageClassDepthFlag | (depth_format & 1); }
constexpr uint32_t ColorStorageClass(uint32_t color_format) {
  return uint32_t(edram::StorageFormat(edram::ColorFormat(color_format)));
}
constexpr bool IsDepthClass(uint32_t storage_class) { return (storage_class & kStorageClassDepthFlag) != 0; }
std::string StorageClassName(uint32_t storage_class);

struct RenderTargetKey {
  uint32_t base_tiles = 0;     // 0-2047
  uint32_t storage_class = 0;
  uint32_t pitch_pixels = 0;   // RB_SURFACE_INFO.surface_pitch
  uint32_t msaa = 0;           // 0 = 1x, 1 = 2x, 2 = 4x
  bool operator==(const RenderTargetKey&) const = default;
};

struct RenderTarget {
  uint32_t id = 0;  // 1-based, never reused
  RenderTargetKey key;
  gpu::TextureHandle texture = 0;
  gpu::Format format = gpu::Format::kUnknown;
  uint32_t width = 0, height = 0;  // host pixels (guest x scale)
  uint32_t guest_width = 0, guest_height = 0;  // guest pixels
  uint32_t scale = 1;  // host pixels per guest pixel on each axis (Stage 7)
  // Shadow-renderer diagnostics on comparison frames, both R32_FLOAT and cleared at the target's first use in the
  // frame. coverage: draw ID of the last writer (0 none, 1 transfer, 2N + stand-in for draw record N). origin: where the
  // tile content came from at the last ownership change (0 own, 1 transferred, 2 known colour clear, 3 unknown: colour
  // source, 4 unknown: other source, 5 the two ownership rules disagree there).
  gpu::TextureHandle coverage = 0;
  gpu::TextureHandle origin = 0;
  uint64_t coverage_frame = 0;
  uint32_t pitch_tiles = 0;
  uint64_t draws = 0;
  uint64_t write_gen = 0;  // job 182: stamped from a renderer-wide counter whenever our GPU work writes this target
  // Ownership fast path per book (upl-n102): the tile bounds this target last took, valid while the book is unchanged.
  struct OwnedCache {
    bool valid = false;
    uint64_t epoch = 0;
    uint32_t c0 = 0, c1 = 0, r0 = 0, r1 = 0, limit = 0;
  };
  OwnedCache owned[2];
  std::string Describe() const;
};

// A run of tiles a target is about to use that a different target wrote last.
struct AliasEvent {
  const RenderTarget* previous = nullptr;
  const RenderTarget* current = nullptr;
  uint32_t tiles = 0;
};

// One tile a target is about to use whose content is in a different target (for EDRAM transfers, upl-n099).
struct TileSource {
  uint32_t offset = 0;  // tile index relative to the target's base (row = offset / pitch_tiles)
  const RenderTarget* owner = nullptr;
  bool clear_known = false;  // the owner's last write there was a clear with a known 32-bit EDRAM value
  uint32_t clear_bits = 0;
  uint8_t taint = 0;  // the owner's content there came (at some point) from a source we could not reproduce (below)
};

// A rectangle in a target's host pixels, [x0, x1) x [y0, y1). Ownership is tracked per whole tile, so every tile the
// rectangle touches counts (conservative rounding outwards).
struct PixelRect {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// Host pixels -> the guest pixels they touch (rounded outwards; x1/y1 may be 0xFFFFFFFF, so no "+ s - 1" overflow).
constexpr PixelRect HostToGuestRect(const PixelRect& r, uint32_t s) {
  if (s <= 1) return r;
  return {r.x0 / s, r.y0 / s, r.x1 / s + (r.x1 % s ? 1u : 0u), r.y1 / s + (r.y1 % s ? 1u : 0u)};
}
// Guest pixels -> host pixels (saturating).
constexpr PixelRect GuestToHostRect(const PixelRect& r, uint32_t s) {
  if (s <= 1) return r;
  auto m = [s](uint32_t v) { return v > 0xFFFFFFFFu / s ? 0xFFFFFFFFu : v * s; };
  return {m(r.x0), m(r.y0), m(r.x1), m(r.y1)};
}
// Guest size cap so that guest x scale stays within the host's 16384 (and within 8192 guest pixels).
constexpr uint32_t GuestDimensionCap(uint32_t s) { return s <= 2 ? 8192u : 16384u / s; }

// Two independent ownership books (upl-n100). The renderer acts on kPrimary; kAlternate follows the other rule so
// the comparison can label pixels where the two rules disagree (D23: per-rectangle ownership is the hardware model,
// whole rows from the base is the Canary estimate the reference uses).
enum class OwnerBook : uint32_t { kPrimary = 0, kAlternate = 1 };

// Host format a colour storage class is drawn in (kUnknown = not modelled yet: the target keeps bookkeeping and a
// stand-in texture but is not drawn).
gpu::Format ColorHostFormat(uint32_t storage_class);

class RenderTargetCache {
 public:
  // scale_threshold (per-target scale, Gears 3 notes/53b): targets whose surface pitch is at or below it stay at 1x
  // (the downscaled post-process chains, which gain nothing from the scale and break with it - the reference
  // emulators' draw_resolution_scale_threshold); 0 = every target at `scale`.
  explicit RenderTargetCache(gpu::Backend& backend, bool d24s8_as_d32 = false, uint32_t scale = 1,
                             uint32_t scale_threshold = 0)
      : backend_(backend), d24s8_as_d32_(d24s8_as_d32), scale_(scale < 1 ? 1 : scale),
        scale_threshold_(scale_threshold), phase_scale_(scale_) {}
  uint32_t scale() const { return scale_; }
  // Patch 0141 (upscaling modes): the scale targets above the threshold are drawn at in this part of the frame - the
  // scene's (below the output) until the frame's scene-colour resolve, the output's (scale()) after it. A target key
  // then has one host target per scale ("twins"); Find / Get return the one at the current phase scale. 0 = scale().
  void SetPhaseScale(uint32_t s) { phase_scale_ = s ? s : scale_; }
  uint32_t PhaseScale() const { return phase_scale_; }
  // The scale a target with this pitch / MSAA is (or will be) drawn at.
  uint32_t ScaleFor(uint32_t pitch_pixels, uint32_t msaa) const {
    (void)msaa;
    return (scale_ > 1 && scale_threshold_ && pitch_pixels <= scale_threshold_) ? 1u : phase_scale_;
  }
  uint32_t ScaleFor(const RenderTargetKey& key) const { return ScaleFor(key.pitch_pixels, key.msaa); }
  uint64_t HostBytes() const { return host_bytes_; }
  // Finds or creates the host target for a key. With create_storage = false only the bookkeeping exists (texture 0),
  // used in M1 for colour targets, whose tile ownership is tracked but which are not drawn yet. Null if the texture
  // can't be created.
  RenderTarget* Get(const RenderTargetKey& key, bool create_storage = true);
  RenderTarget* Find(const RenderTargetKey& key);
  // Patch 0141 review: the twin at the phase scale if it exists, else the other-scale twin that our GPU wrote last
  // (resolves, clears and fast clears of a key the other phase drew). Mode off: same as Find.
  RenderTarget* FindAny(const RenderTargetKey& key);
  // The target is about to be written (or, with take_ownership = false, read) in the tiles that `pixels` touches.
  // Updates tile ownership in `book` and reports, per previous owner, how many of those tiles another target wrote
  // last (`aliases`, optional). `sources` (optional) receives every such tile. `clear_known`/`clear_bits`: this write
  // is a clear whose EDRAM value is known (colour clears with a constant colour), so later readers of these tiles can
  // take the value. `tile_limit`: tiles at or beyond this offset from the target's base are left alone (Canary clamps
  // a bound target's range at the next bound target's base).
  void MarkWritten(RenderTarget& target, const PixelRect& pixels, std::vector<AliasEvent>* aliases,
                   std::vector<TileSource>* sources = nullptr, bool clear_known = false, uint32_t clear_bits = 0,
                   OwnerBook book = OwnerBook::kPrimary, bool take_ownership = true,
                   uint32_t tile_limit = edram::kTileCount);
  // Tile-space bounds of a pixel rectangle in a target: columns [c0, c1), rows [r0, r1) of tiles (MSAA and 64bpp
  // applied, columns clamped to the pitch).
  static void TileBounds(const RenderTarget& target, const PixelRect& pixels, uint32_t& c0, uint32_t& c1,
                         uint32_t& r0, uint32_t& r1);
  // The host-pixel rectangle of one tile (by offset from the base), clamped to the target.
  static PixelRect TilePixels(const RenderTarget& target, uint32_t offset);
  static bool Is64bpp(const RenderTarget& target);
  // Tile trace (upl-n101, --gow2_renderer_tile_trace): while set, MarkWritten reports every access to a traced tile
  // (tile index, offset from the target's base, book, previous owner, the accessing target, whether it takes it).
  using TraceFn = std::function<void(uint32_t tile, uint32_t offset, OwnerBook book, const RenderTarget* previous,
                                     const RenderTarget& target, bool take_ownership)>;
  void SetTrace(const std::bitset<edram::kTileCount>* tiles, TraceFn fn) {
    trace_tiles_ = tiles;
    trace_fn_ = std::move(fn);
  }
  const RenderTarget* FindById(uint32_t id) const;
  // Every target other than `except` whose tiles overlap [first_tile, first_tile + tile_count) (modulo 2048).
  void ForEachOverlap(uint32_t first_tile, uint32_t tile_count, const RenderTarget* except,
                      const std::function<void(RenderTarget&)>& fn);
  // Tiles covered by host pixel rows [0, rows) of a target (rows rounded up to whole tile rows).
  static uint32_t TileCount(const RenderTarget& target, uint32_t rows);
  size_t size() const { return targets_.size(); }
  const std::vector<std::unique_ptr<RenderTarget>>& targets() const { return targets_; }

 private:
  bool CreateStorage(RenderTarget& target);
  gpu::Backend& backend_;
  bool d24s8_as_d32_ = false;
  uint32_t scale_ = 1;
  uint32_t scale_threshold_ = 0;
  uint32_t phase_scale_ = 1;  // patch 0141
  uint64_t host_bytes_ = 0;
  std::vector<std::unique_ptr<RenderTarget>> targets_;
  // Per book: RenderTarget::id that last wrote the tile (0 = none yet) and whether that write was a known clear.
  uint32_t owner_[2][edram::kTileCount] = {};
  bool clear_known_[2][edram::kTileCount] = {};
  uint32_t clear_bits_[2][edram::kTileCount] = {};
  uint64_t book_epoch_[2] = {1, 1};  // bumped on every change of a book
  uint8_t taint_[edram::kTileCount] = {};
 public:
  // Make the alternate book a copy of the primary one (when it is only tracked around comparison frames).
  void ResyncAlternate();
  // Content taint of a tile (acting book; upl-n102): 0 = our data is what the EDRAM holds, 1 = part of it came
  // through transfers from a colour target we can't reinterpret yet (M2), 2 = from another unmodelled source. A new
  // owner starts at 0; the renderer sets it after deciding the transfer.
  void SetTaint(const RenderTarget& target, uint32_t offset, uint8_t taint) {
    taint_[(target.key.base_tiles + offset) % edram::kTileCount] = taint;
  }
  uint64_t FastHits() const { return fast_hits_; }
 private:
  uint64_t fast_hits_ = 0;
  uint32_t next_id_ = 1;
  const std::bitset<edram::kTileCount>* trace_tiles_ = nullptr;
  TraceFn trace_fn_;
};

}  // namespace xenos::renderer
