// Layer 0 (game-free): see xenos/renderer/render_target_cache.h. Our code (MIT).
#include <xenos/renderer/render_target_cache.h>

#include <rex/logging/macros.h>

#include <algorithm>
#include <cstdio>

namespace xenos::renderer {

std::string StorageClassName(uint32_t c) {
  if (IsDepthClass(c)) return (c & 1) ? "D24FS8" : "D24S8";
  switch (edram::ColorFormat(c)) {
    case edram::ColorFormat::k_8_8_8_8: return "k_8_8_8_8";
    case edram::ColorFormat::k_8_8_8_8_GAMMA: return "k_8_8_8_8_GAMMA";
    case edram::ColorFormat::k_2_10_10_10: return "k_2_10_10_10";
    case edram::ColorFormat::k_2_10_10_10_FLOAT: return "k_2_10_10_10_FLOAT(7e3)";
    case edram::ColorFormat::k_16_16: return "k_16_16";
    case edram::ColorFormat::k_16_16_16_16: return "k_16_16_16_16";
    case edram::ColorFormat::k_16_16_FLOAT: return "k_16_16_FLOAT";
    case edram::ColorFormat::k_16_16_16_16_FLOAT: return "k_16_16_16_16_FLOAT";
    case edram::ColorFormat::k_32_FLOAT: return "k_32_FLOAT";
    case edram::ColorFormat::k_32_32_FLOAT: return "k_32_32_FLOAT";
    default: return "colour format " + std::to_string(c);
  }
}

std::string RenderTarget::Describe() const {
  char b[160];
  std::snprintf(b, sizeof(b), "#%u %s base %u pitch %u msaa %ux (%ux%u host)", id,
                StorageClassName(key.storage_class).c_str(), key.base_tiles, key.pitch_pixels, 1u << key.msaa, width,
                height);
  return b;
}

RenderTarget* RenderTargetCache::Find(const RenderTargetKey& key) {
  const uint32_t scale = ScaleFor(key);  // review 19 Nov #5: once, not per target
  for (auto& t : targets_)
    if (t->scale == scale && t->key == key) return t.get();  // patch 0141: the twin at the phase scale
  return nullptr;
}

RenderTarget* RenderTargetCache::FindAny(const RenderTargetKey& key) {
  if (RenderTarget* t = Find(key)) return t;
  RenderTarget* best = nullptr;
  for (auto& t : targets_)
    if (t->key == key && (!best || t->write_gen > best->write_gen)) best = t.get();
  return best;
}

RenderTarget* RenderTargetCache::FindForResolve(const RenderTargetKey& key) {
  RenderTarget* t = FindAny(key);
  if (t && t->texture && t->draws == 0 && t->scale != ScaleFor(key)) {
    // Only resolve-only views: a key the other phase drew keeps FindAny's answer (its tiles are that twin's work).
    for (auto& o : targets_)
      if (o->key == key && o->draws) return t;
    if (RenderTarget* p = Get(key); p && p->texture) return p;
  }
  return t;
}

RenderTarget* RenderTargetCache::Get(const RenderTargetKey& key, bool create_storage) {
  if (RenderTarget* t = Find(key)) {
    if (create_storage && !t->texture && !CreateStorage(*t)) return nullptr;
    return t;
  }
  if (!key.pitch_pixels || key.msaa > 2) return nullptr;
  const edram::Msaa msaa = edram::Msaa(key.msaa);
  const bool depth = IsDepthClass(key.storage_class);
  const bool is64 = !depth && edram::Is64bpp(edram::ColorFormat(key.storage_class));
  auto t = std::make_unique<RenderTarget>();
  t->id = next_id_++;
  t->key = key;
  t->pitch_tiles = edram::SurfacePitchTiles(key.pitch_pixels, msaa, is64);
  const uint32_t tile_rows = (edram::Tiles() + t->pitch_tiles - 1) / t->pitch_tiles;
  const uint32_t rows_per_tile = edram::kTileHeightSamples >> uint32_t(msaa >= edram::Msaa::k2X);
  const uint32_t scale = ScaleFor(key);
  const uint32_t cap = GuestDimensionCap(scale);
  t->scale = scale;
  t->guest_width = std::min<uint32_t>(key.pitch_pixels, cap);
  // Extended EDRAM (job 2155): four times the tiles made every target four times as tall (1920x5472 for the 1080p
  // scene, 960x8192) - graphics memory for rows no game draws. Keep the console-sized height (what the same pitch gets
  // with 2048 tiles), and at least 2048 lines so a 1920x1080 scene and anything up to the console's largest surface fit.
  uint32_t rows_px = tile_rows * rows_per_tile;
  if (edram::Tiles() > edram::kTileCount) {
    const uint32_t console_rows = (edram::kTileCount + t->pitch_tiles - 1) / t->pitch_tiles * rows_per_tile;
    rows_px = std::min(rows_px, std::max<uint32_t>(console_rows, 2048u));
  }
  t->guest_height = std::min<uint32_t>(rows_px, cap);
  t->width = t->guest_width * scale;
  t->height = t->guest_height * scale;
  if (create_storage && !CreateStorage(*t)) return nullptr;
  REXLOG_INFO("gow2_renderer: new render target {}{} (all targets ~{} MB)", t->Describe(),
              t->texture ? "" : " (bookkeeping only)", host_bytes_ >> 20);
  targets_.push_back(std::move(t));
  return targets_.back().get();
}

bool RenderTargetCache::CreateStorage(RenderTarget& t) {
  const RenderTargetKey& key = t.key;
  const bool depth = IsDepthClass(key.storage_class);
  const bool is64 = !depth && edram::Is64bpp(edram::ColorFormat(key.storage_class));
  gpu::TextureDesc d;
  d.width = t.width;
  d.height = t.height;
  d.samples = 1u << key.msaa;
  char name[96];
  std::snprintf(name, sizeof(name), "gow2 rt %s base %u pitch %u", StorageClassName(key.storage_class).c_str(),
                key.base_tiles, key.pitch_pixels);
  d.debug_name = name;
  if (depth) {
    d.format = ((key.storage_class & 1) == 0 && !d24s8_as_d32_) ? gpu::Format::kD24UnormS8 : gpu::Format::kD32FloatS8X24;
    d.usage = gpu::kUsageDepthStencil | gpu::kUsageSampled;
  } else {
    // M2: the reference's host formats (SDK GetColorDrawDXGIFormat; notes/45). Formats not modelled yet keep a
    // stand-in storage and are not drawn (ColorHostFormat returns kUnknown for them).
    d.format = ColorHostFormat(key.storage_class);
    if (d.format == gpu::Format::kUnknown) d.format = is64 ? gpu::Format::kR16G16B16A16Float : gpu::Format::kR8G8B8A8Unorm;
    d.usage = gpu::kUsageRenderTarget | gpu::kUsageSampled;
  }
  t.format = d.format;
  t.texture = backend_.CreateTexture(d);
  if (t.texture) {
    const uint64_t bpp = depth ? (d.format == gpu::Format::kD24UnormS8 ? 4u : 8u) : (is64 ? 8u : 4u);
    t.host_bytes = uint64_t(t.width) * t.height * d.samples * bpp;
    host_bytes_ += t.host_bytes;
  }
  return t.texture != 0;
}

uint32_t RenderTargetCache::TileCount(const RenderTarget& target, uint32_t rows) {
  const uint32_t rows_per_tile = edram::kTileHeightSamples >> uint32_t(target.key.msaa >= uint32_t(edram::Msaa::k2X));
  const uint32_t s = std::max(target.scale, 1u);
  const uint32_t guest_rows = std::min(rows / s + (rows % s ? 1u : 0u), target.guest_height);
  const uint32_t tile_rows = (guest_rows + rows_per_tile - 1) / rows_per_tile;
  return std::min<uint32_t>(tile_rows * target.pitch_tiles, edram::Tiles());
}

RenderTargetCache::FreeResult RenderTargetCache::FreeUnusedScales(const std::vector<uint32_t>& phases) {
  FreeResult r;
  std::vector<uint32_t> freed_ids;
  for (auto it = targets_.begin(); it != targets_.end();) {
    RenderTarget& t = **it;
    bool live = false;
    for (uint32_t p : phases) live = live || t.scale == ScaleForPhase(t.key.pitch_pixels, t.key.msaa, p);
    if (live) {
      ++it;
      continue;
    }
    if (t.texture) backend_.DestroyTexture(t.texture);  // deferred by the backend until our GPU is past it
    host_bytes_ -= std::min(host_bytes_, t.host_bytes);
    r.bytes += t.host_bytes;
    ++r.count;
    freed_ids.push_back(t.id);
    it = targets_.erase(it);
  }
  if (freed_ids.empty()) return r;
  for (uint32_t b = 0; b < 2; ++b) {
    for (uint32_t tile = 0; tile < edram::Tiles(); ++tile) {
      uint32_t& o = owner_[b][tile];
      if (!o || std::find(freed_ids.begin(), freed_ids.end(), o) == freed_ids.end()) continue;
      o = 0;
      clear_known_[b][tile] = false;
      clear_bits_[b][tile] = 0;
      if (b == 0) taint_[tile] = 0;
    }
    ++book_epoch_[b];  // every remaining target's OwnedCache fast path is re-checked
  }
  return r;
}

const RenderTarget* RenderTargetCache::FindById(uint32_t id) const {
  for (const auto& t : targets_)
    if (t->id == id) return t.get();
  return nullptr;
}

gpu::Format ColorHostFormat(uint32_t storage_class) {
  switch (edram::ColorFormat(storage_class)) {
    case edram::ColorFormat::k_8_8_8_8: return gpu::Format::kR8G8B8A8Unorm;
    case edram::ColorFormat::k_2_10_10_10: return gpu::Format::kR10G10B10A2Unorm;
    case edram::ColorFormat::k_2_10_10_10_FLOAT: return gpu::Format::kR16G16B16A16Float;  // 7e3: fp16, unclamped
    case edram::ColorFormat::k_16_16_FLOAT: return gpu::Format::kR16G16Float;
    case edram::ColorFormat::k_16_16_16_16_FLOAT: return gpu::Format::kR16G16B16A16Float;
    case edram::ColorFormat::k_32_FLOAT: return gpu::Format::kR32Float;
    case edram::ColorFormat::k_32_32_FLOAT: return gpu::Format::kR32G32Float;
    // 16-bit fixed point: SNORM with the shader output x 1/32 (exp bias -5), so the host bits equal the guest bits
    // (snorm16_render_target_full_range, the SDK/Canary default).
    case edram::ColorFormat::k_16_16: return gpu::Format::kR16G16Snorm;
    case edram::ColorFormat::k_16_16_16_16: return gpu::Format::kR16G16B16A16Snorm;
    default: return gpu::Format::kUnknown;  // 8888_GAMMA (Gears 2 has none, notes/35)
  }
}

bool RenderTargetCache::Is64bpp(const RenderTarget& target) {
  return !IsDepthClass(target.key.storage_class) && edram::Is64bpp(edram::ColorFormat(target.key.storage_class));
}

void RenderTargetCache::TileBounds(const RenderTarget& target, const PixelRect& host_px, uint32_t& c0, uint32_t& c1,
                                   uint32_t& r0, uint32_t& r1) {
  const PixelRect px = HostToGuestRect(host_px, target.scale);
  // Samples per pixel along x: 2 at 4x MSAA; a 64bpp sample takes two 32bpp slots (40 samples per tile).
  const uint32_t xs = (target.key.msaa >= uint32_t(edram::Msaa::k4X) ? 2u : 1u) * (Is64bpp(target) ? 2u : 1u);
  const uint32_t ys = target.key.msaa >= uint32_t(edram::Msaa::k2X) ? 2u : 1u;
  const uint32_t x0 = std::min(px.x0, target.guest_width), x1 = std::min(px.x1, target.guest_width);
  const uint32_t y0 = std::min(px.y0, target.guest_height), y1 = std::min(px.y1, target.guest_height);
  c0 = x0 * xs / edram::kTileWidthSamples;
  c1 = std::min((x1 * xs + edram::kTileWidthSamples - 1) / edram::kTileWidthSamples, target.pitch_tiles);
  r0 = y0 * ys / edram::kTileHeightSamples;
  r1 = (y1 * ys + edram::kTileHeightSamples - 1) / edram::kTileHeightSamples;
  if (x1 <= x0 || y1 <= y0 || c1 <= c0) c1 = c0, r1 = r0;
}

PixelRect RenderTargetCache::TilePixels(const RenderTarget& target, uint32_t offset) {
  const uint32_t xs = (target.key.msaa >= uint32_t(edram::Msaa::k4X) ? 2u : 1u) * (Is64bpp(target) ? 2u : 1u);
  const uint32_t ys = target.key.msaa >= uint32_t(edram::Msaa::k2X) ? 2u : 1u;
  const uint32_t pitch = std::max(target.pitch_tiles, 1u);
  const uint32_t row = offset / pitch, col = offset % pitch;
  const uint32_t tw = edram::kTileWidthSamples / xs, th = edram::kTileHeightSamples / ys;
  PixelRect r{col * tw, row * th, (col + 1) * tw, (row + 1) * th};
  r.x0 = std::min(r.x0, target.guest_width);
  r.x1 = std::min(r.x1, target.guest_width);
  r.y0 = std::min(r.y0, target.guest_height);
  r.y1 = std::min(r.y1, target.guest_height);
  return GuestToHostRect(r, target.scale);  // host pixels
}

void RenderTargetCache::MarkWritten(RenderTarget& target, const PixelRect& pixels, std::vector<AliasEvent>* aliases,
                                    std::vector<TileSource>* sources, bool clear_known, uint32_t clear_bits,
                                    OwnerBook book, bool take_ownership, uint32_t tile_limit) {
  if (aliases) aliases->clear();
  if (sources) sources->clear();
  const uint32_t bi = uint32_t(book) & 1u;
  uint32_t c0, c1, r0, r1;
  TileBounds(target, pixels, c0, c1, r0, r1);
  tile_limit = std::min<uint32_t>(tile_limit, edram::Tiles());  // beyond 2048 the range wraps onto itself
  // Fast path: the target already took a superset of these tiles and nothing in the book changed since - no tile
  // changes hands and none has a previous owner to report.
  RenderTarget::OwnedCache& oc = target.owned[bi];
  const bool traced = trace_tiles_ && trace_fn_;
  if (!traced && !clear_known && oc.valid && oc.epoch == book_epoch_[bi] && c0 >= oc.c0 && c1 <= oc.c1 &&
      r0 >= oc.r0 && r1 <= oc.r1 && tile_limit <= oc.limit) {
    ++fast_hits_;
    return;
  }
  RenderTarget::OwnedCache& cc = target.clean[bi];
  if (!traced && !take_ownership && cc.valid && cc.epoch == book_epoch_[bi] && c0 >= cc.c0 && c1 <= cc.c1 &&
      r0 >= cc.r0 && r1 <= cc.r1 && tile_limit <= cc.limit) {
    ++fast_hits_;
    return;
  }
  bool foreign = false;
  ++slow_claims_;
  bool changed = false;
  const uint32_t tile_mask = edram::TileMask();   // read once: the stores below could alias the global
  uint32_t run_owner = 0, run_tiles = 0;
  // FindById searches every target: neighbouring tiles almost always share an owner, so remember the last one.
  uint32_t found_id = 0;
  const RenderTarget* found = nullptr;
  auto find = [&](uint32_t id) {
    if (id != found_id) {
      found_id = id;
      found = FindById(id);
    }
    return found;
  };
  auto flush = [&]() {
    if (aliases && run_owner && run_tiles) {
      if (const RenderTarget* prev = find(run_owner)) {
        bool merged = false;
        for (AliasEvent& e : *aliases) {
          if (e.previous == prev) {
            e.tiles += run_tiles;
            merged = true;
          }
        }
        if (!merged) aliases->push_back({prev, &target, run_tiles});
      }
    }
    run_tiles = 0;
  };
  for (uint32_t row = r0; row < r1; ++row) {
    const uint32_t row_start = row * target.pitch_tiles;
    if (row_start >= tile_limit) break;
    for (uint32_t col = c0; col < c1; ++col) {
      const uint32_t rel = row_start + col;
      if (rel >= tile_limit) break;
      const uint32_t tile = (target.key.base_tiles + rel) & tile_mask;
      uint32_t& o = owner_[bi][tile];
      ++slow_tiles_;
      if (traced && trace_tiles_->test(tile))
        trace_fn_(tile, rel, book, o ? FindById(o) : nullptr, target, take_ownership);
      if (o && o != target.id) {
        foreign = true;
        if (o != run_owner) {
          flush();
          run_owner = o;
        }
        ++run_tiles;
        if (sources)
          sources->push_back({rel, find(o), clear_known_[bi][tile], clear_bits_[bi][tile], bi == 0 ? taint_[tile] : uint8_t(0)});
      }
      if (take_ownership) {
        if (o != target.id || clear_known_[bi][tile] != clear_known || clear_bits_[bi][tile] != clear_bits)
          changed = true;
        if (o != target.id) ++taken_tiles_;
        if (bi == 0 && o != target.id) taint_[tile] = 0;
        o = target.id;
        clear_known_[bi][tile] = clear_known;
        clear_bits_[bi][tile] = clear_bits;
      }
    }
  }
  flush();
  if (changed) {
    ++book_changes_;
    if (!targeted_invalidation_) {
      ++book_epoch_[bi];   // every target's fast path is re-checked (as before)
    } else if (r1 > r0 && c1 > c0) {
      // Only the targets whose owned tiles this claim can have touched lose their fast path. The claim's tiles lie in
      // [first, first + len) on the tile circle (the span from its first tile to its last - wider than the rectangle,
      // never narrower); a target's owned tiles in the same kind of span. A target whose span does not meet it still
      // owns every tile it took, with the clear state it left - exactly what its fast path assumes.
      auto span = [&](const RenderTarget& t, uint32_t tc0, uint32_t tc1, uint32_t tr0, uint32_t tr1, uint32_t limit,
                      uint32_t* first, uint32_t* len) {
        const uint32_t lo = tr0 * t.pitch_tiles + tc0;
        const uint32_t hi = std::min<uint32_t>((tr1 - 1) * t.pitch_tiles + tc1, limit);
        *first = (t.key.base_tiles + lo) & tile_mask;
        *len = hi > lo ? hi - lo : 0;
      };
      auto inside = [&](uint32_t x, uint32_t start, uint32_t len) { return ((x - start) & tile_mask) < len; };
      uint32_t first = 0, len = 0;
      span(target, c0, c1, r0, r1, tile_limit, &first, &len);
      for (auto& other : targets_) {
        if (other.get() == &target) continue;
        // both fast-path caches of the other target: the tiles it owns, and (speed study) the rectangle it found free
        // of other owners
        for (RenderTarget::OwnedCache* o2 : {&other->owned[bi], &other->clean[bi]}) {
          if (!o2->valid) continue;
          uint32_t f2 = 0, l2 = 0;
          span(*other, o2->c0, o2->c1, o2->r0, o2->r1, o2->limit, &f2, &l2);
          if (len && l2 && (inside(first, f2, l2) || inside(f2, first, len))) {
            o2->valid = false;
            ++targeted_invalidations_;
          }
        }
      }
    }
  }
  if (take_ownership && !clear_known) {
    // Now the owner of all of [c0,c1) x [r0,r1) within the limit.
    oc = {true, book_epoch_[bi], c0, c1, r0, r1, tile_limit};
  } else if (take_ownership) {
    oc.valid = false;  // a clear-known write: a later plain write must reset those tiles' clear state
  }
  // A look that found no other owner: true for every range inside this one until a book changes (the epoch).
  if (!take_ownership && !foreign && !traced) cc = {true, book_epoch_[bi], c0, c1, r0, r1, tile_limit};
}

void RenderTargetCache::ForEachOverlap(uint32_t first_tile, uint32_t tile_count, const RenderTarget* except,
                                       const std::function<void(RenderTarget&)>& fn) {
  tile_count = std::min<uint32_t>(tile_count, edram::Tiles());
  for (auto& t : targets_) {
    if (t.get() == except) continue;
    const uint32_t t_count = TileCount(*t, t->height);
    // Two ranges on a circle of 2048 tiles overlap if either start lies inside the other range.
    auto inside = [](uint32_t x, uint32_t start, uint32_t len) {
      return ((x - start) & edram::TileMask()) < len;
    };
    if (inside(first_tile, t->key.base_tiles, t_count) || inside(t->key.base_tiles, first_tile, tile_count)) fn(*t);
  }
}

}  // namespace xenos::renderer
