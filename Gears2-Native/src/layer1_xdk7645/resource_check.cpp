// Layer 1 (XDK 7645): Stage 1 comparison mode (notes/34 S1.2-S1.3). Answers the deciding Stage 1 unknown: does memory the
// GPU reads change without an event we can hook?
//  - At every --gow2_res_check=N'th frame the GPU-thread observer hashes the memory behind each fetch constant the draw's
//    shaders use (vertex buffers: exact size; textures: base level and first mip, size worked out from the fetch
//    constant) and every index buffer, once per range per frame.
//  - Between checks, two kinds of event mark ranges as "expected to change":
//      D3D events - the resource-call hooks (d3d_watch.cpp: unlock, XGOffset* rebind, mip realloc / defrag) and resolves;
//      page watch - the emulator's own write detection (guest physical pages the GPU has read are write-protected; a CPU
//                   write or a kernel file read into them fires Memory's invalidation callbacks, which we listen to).
//  - A range whose hash changed since the previous check is classed by the first that applies: explained by a D3D event,
//    by the page watch's exact range, by the page watch's neighbourhood (when one watched page is hit, the emulator
//    unwatches and invalidates up to the enclosing 4 MB block - xmemory.cpp kMaxUnwatchExcess - so later writes there
//    raise no callback but the emulator re-reads that memory anyway), or UNEXPLAINED.
//    Job 062: steady play had 0 unexplained; all were textures during streaming bursts, ~40% caught by the exact range.
// Rough on purpose: CPU writes racing the GPU thread can tear a hash, and CPU hooks run 1-2 frames ahead of the GPU, so
// a mark lasts for two checks. Job 061 (first version) used a fixed 64 KB texture window and lost resolves between
// checks; both fixed here.
#include <xenos/dev_tools.h>
#include <xdk7645/stage1.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/hash.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

REXCVAR_DEFINE_INT32(gow2_res_check, 0, "Gears2", "Stage 1 comparison: hash GPU-read resources every N frames (0 = off)");

namespace {

using namespace rex::graphics;

enum class Kind : uint8_t { kVertex, kIndex, kTexture, kTextureMip };
constexpr const char* KindName(Kind k) {
  return k == Kind::kVertex ? "vertex" : k == Kind::kIndex ? "index" : k == Kind::kTexture ? "texture" : "texture-mip";
}
constexpr uint32_t kMaxRange = 16u << 20;

// Bytes per block and block width/height for each xenos::TextureFormat (0 = not handled, falls back to 64 KB).
struct Block { uint8_t bytes, dim; };
constexpr Block kBlocks[64] = {
    {0, 1}, {0, 1}, {1, 1}, {2, 1}, {2, 1}, {2, 1}, {4, 1}, {4, 1},    // 0-7
    {1, 1}, {1, 1}, {2, 1}, {2, 1}, {2, 1}, {4, 1}, {4, 1}, {2, 1},    // 8-15
    {4, 1}, {4, 1}, {8, 4}, {16, 4}, {16, 4}, {8, 1}, {4, 1}, {4, 1},  // 16-23 (18-20 DXT1/3/5)
    {2, 1}, {4, 1}, {8, 1}, {2, 1}, {4, 1}, {8, 1}, {2, 1}, {4, 1},    // 24-31
    {8, 1}, {4, 1}, {8, 1}, {16, 1}, {4, 1}, {8, 1}, {16, 1}, {4, 1},  // 32-39
    {4, 1}, {2, 1}, {4, 1}, {1, 1}, {4, 1}, {4, 1}, {2, 1}, {2, 1},    // 40-47
    {4, 1}, {16, 4}, {4, 1}, {8, 4}, {16, 4}, {16, 4}, {4, 1}, {4, 1}, // 48-55 (49 DXN)
    {4, 1}, {12, 1}, {8, 4}, {8, 4}, {8, 4}, {8, 4}, {4, 1}, {4, 1},   // 56-63 (58-61 DXT3A/5A/CTX1)
};

// Base level size in bytes (4 KB aligned), from the fetch constant. Tiled textures are padded to 32x32 texels.
uint32_t TextureBaseSize(const xenos::xe_gpu_texture_fetch_t& f) {
  const Block b = kBlocks[uint32_t(f.format) & 63];
  if (!b.bytes || f.dimension == xenos::DataDimension::k3D || f.dimension == xenos::DataDimension::k1D) return 64u << 10;
  const uint32_t w = f.size_2d.width + 1, h = f.size_2d.height + 1;
  const uint32_t pitch = f.pitch ? uint32_t(f.pitch) << 5 : ((w + 31) & ~31u);
  const uint32_t rows = f.tiled ? ((h + 31) & ~31u) : h;
  uint64_t bytes = uint64_t((pitch + b.dim - 1) / b.dim) * ((rows + b.dim - 1) / b.dim) * b.bytes;
  if (!f.tiled) bytes = uint64_t((((pitch + b.dim - 1) / b.dim) * b.bytes + 255) & ~255u) * ((rows + b.dim - 1) / b.dim);
  uint32_t slices = 1;
  if (f.dimension == xenos::DataDimension::kCube) slices = 6;
  else if (f.stacked) slices = f.size_2d.stack_depth + 1;
  bytes = ((bytes + 4095) & ~uint64_t(4095)) * slices;
  return uint32_t(std::min<uint64_t>(bytes, kMaxRange));
}

struct Entry {
  uint32_t length = 0;
  Kind kind = Kind::kVertex;
  uint64_t hash = 0;
  uint64_t checked_frame = 0;
  int d3d_marks = 0;    // checks left during which a change counts as explained by a D3D event
  int watch_marks = 0;  // same for the page watch (exact range)
  int area_marks = 0;   // same for the page watch's 4 MB neighbourhood
  uint32_t unexplained_checks = 0;
  const char* last_event = nullptr;
};

struct Totals {
  uint64_t checked = 0, unchanged = 0, d3d = 0, watch = 0, area = 0, unexplained = 0, fresh = 0;
  uint64_t unexplained_kind[4] = {};
  uint64_t watch_kind[4] = {};
};

class ResourceCheck final : public DrawObserver {
 public:
  void Install(rex::memory::Memory* m) {
    memory_ = m;
    m->RegisterPhysicalMemoryInvalidationCallback(&ResourceCheck::WatchThunk, this);
  }

  void Expect(uint32_t addr, const char* event, bool all_textures) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (all_textures) {
      for (auto& [start, e] : entries_)
        if (e.kind == Kind::kTexture || e.kind == Kind::kTextureMip) { e.d3d_marks = 2; e.last_event = event; }
      ++all_texture_events_;
      return;
    }
    if (d3d_pending_.size() < 262144) d3d_pending_.push_back({addr & 0x1FFFFFFF, event});
  }

  void OnDrawShaders(const Shader* vs, const Shader* ps) override { vs_ = vs; ps_ = ps; }

  void OnDraw(const RegisterFile& regs, xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer* ib, bool) override {
    if (!memory_) return;
    if (regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
      // Resolve: the GPU rewrites the destination. Recorded on every frame, not only check frames.
      std::lock_guard<std::mutex> lock(mutex_);
      if (d3d_pending_.size() < 262144) d3d_pending_.push_back({regs[0x2319] & 0x1FFFFFFF, "resolve"});  // RB_COPY_DEST_BASE
      return;
    }
    if (!checking_) return;
    if (ib && ib->length) Visit(ib->guest_base, uint32_t(std::min<uint64_t>(ib->length, kMaxRange)), Kind::kIndex);
    if (vs_) {
      for (const auto& b : vs_->vertex_bindings()) {
        auto f = regs.GetVertexFetch(b.fetch_constant);
        if (f.type == xenos::FetchConstantType::kVertex && f.size)
          Visit(f.address << 2, std::min<uint32_t>(f.size << 2, kMaxRange), Kind::kVertex);
      }
    }
    for (const Shader* s : {vs_, ps_}) {
      if (!s) continue;
      for (const auto& b : s->texture_bindings()) {
        auto f = regs.GetTextureFetch(b.fetch_constant);
        if (f.type != xenos::FetchConstantType::kTexture || !f.base_address) continue;
        const uint32_t size = TextureBaseSize(f);
        Visit(f.base_address << 12, size, Kind::kTexture);
        if (f.mip_address && f.mip_max_level > 0) Visit(f.mip_address << 12, std::max(4096u, size / 4), Kind::kTextureMip);
      }
    }
  }

  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    ++frame_;
    const int every = REXCVAR_GET(gow2_res_check);
    if (checking_) Report();
    checking_ = every > 0 && (frame_ % uint64_t(every)) == 0;
    if (!checking_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    // Apply the events gathered since the last check to the ranges that contain them.
    for (const auto& [a, ev] : d3d_pending_) {
      auto it = entries_.upper_bound(a);
      if (it == entries_.begin()) continue;
      --it;
      if (a < it->first + it->second.length) { it->second.d3d_marks = 2; it->second.last_event = ev; }
    }
    d3d_pending_.clear();
    for (const auto& [start, length] : watch_pending_) {
      // Every tracked range overlapping [start, start + length).
      auto it = entries_.lower_bound(start + length);
      while (it != entries_.begin()) {
        --it;
        if (it->first + kMaxRange <= start) break;
        if (it->first + it->second.length > start) it->second.watch_marks = 2;
      }
      // The neighbourhood the emulator may unwatch (and invalidate) along with it.
      constexpr uint32_t kExcess = 4u << 20;
      const uint32_t a0 = start & ~(kExcess - 1), a1 = ((start + length - 1) | (kExcess - 1)) + 1;
      auto jt = entries_.lower_bound(a1);
      while (jt != entries_.begin()) {
        --jt;
        if (jt->first + kMaxRange <= a0) break;
        if (jt->first + jt->second.length > a0) jt->second.area_marks = 2;
      }
    }
    watch_pending_.clear();
    frame_totals_ = Totals{};
    examples_.clear();
  }

 private:
  static std::pair<uint32_t, uint32_t> WatchThunk(void* ctx, uint32_t start, uint32_t length, bool) {
    auto* self = static_cast<ResourceCheck*>(ctx);
    std::lock_guard<std::mutex> lock(self->mutex_);
    ++self->watch_events_;
    if (self->watch_pending_.size() < 262144) self->watch_pending_.push_back({start & 0x1FFFFFFF, length});
    return {0, UINT32_MAX};  // we don't care what gets unwatched
  }

  void Visit(uint32_t addr, uint32_t length, Kind kind) {
    addr &= 0x1FFFFFFF;
    if (length == 0 || addr + uint64_t(length) > 0x20000000ull) return;
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = entries_[addr];
    if (e.checked_frame == frame_ && e.length) return;  // once per range per check frame
    const uint64_t h = XXH3_64bits(memory_->TranslatePhysical<const uint8_t*>(addr), length);
    Totals& t = frame_totals_;
    ++t.checked;
    if (!e.length || e.length != length) {
      // New range (or a different size at the same address): nothing to compare yet.
      ++t.fresh;
    } else if (e.hash == h) {
      ++t.unchanged;
    } else if (e.d3d_marks > 0) {
      ++t.d3d;
    } else if (e.watch_marks > 0) {
      ++t.watch;
      ++t.watch_kind[int(kind)];
    } else if (e.area_marks > 0) {
      ++t.area;
    } else {
      ++t.unexplained;
      ++t.unexplained_kind[int(kind)];
      ++e.unexplained_checks;
      if (examples_.size() < 6) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%s %08X+%X (unexplained at %u checks so far, last D3D event: %s)", KindName(kind),
                      addr, length, e.unexplained_checks, e.last_event ? e.last_event : "none");
        examples_.emplace_back(buf);
      }
    }
    if (e.d3d_marks > 0) --e.d3d_marks;
    if (e.watch_marks > 0) --e.watch_marks;
    if (e.area_marks > 0) --e.area_marks;
    e.hash = h; e.length = length; e.kind = kind; e.checked_frame = frame_;
  }

  void Report() {
    std::lock_guard<std::mutex> lock(mutex_);
    const Totals& t = frame_totals_;
    run_.checked += t.checked; run_.unchanged += t.unchanged; run_.d3d += t.d3d; run_.watch += t.watch; run_.area += t.area;
    run_.unexplained += t.unexplained; run_.fresh += t.fresh;
    for (int i = 0; i < 4; ++i) { run_.unexplained_kind[i] += t.unexplained_kind[i]; run_.watch_kind[i] += t.watch_kind[i]; }
    REXLOG_INFO("gow2_res_check: frame {} hashed {} new {} unchanged {} | changed: D3D event {} page watch {} watch area {} "
                "UNEXPLAINED {} (vertex {} index {} texture {} mip {}) | tracked {}",
                frame_, t.checked, t.fresh, t.unchanged, t.d3d, t.watch, t.area, t.unexplained, t.unexplained_kind[0],
                t.unexplained_kind[1], t.unexplained_kind[2], t.unexplained_kind[3], entries_.size());
    for (const auto& s : examples_) REXLOG_INFO("gow2_res_check:   unexplained {}", s);
    if (++reports_ % 10 == 0) {
      REXLOG_INFO("gow2_res_check: RUN TOTAL to frame {}: hashed {} new {} unchanged {} | changed: D3D event {} page watch "
                  "{} (vertex {} index {} texture {} mip {}) watch area {} UNEXPLAINED {} (vertex {} index {} texture {} mip {}) | "
                  "page-watch callbacks {} all-texture events {}",
                  frame_, run_.checked, run_.fresh, run_.unchanged, run_.d3d, run_.watch, run_.watch_kind[0],
                  run_.watch_kind[1], run_.watch_kind[2], run_.watch_kind[3], run_.area, run_.unexplained, run_.unexplained_kind[0],
                  run_.unexplained_kind[1], run_.unexplained_kind[2], run_.unexplained_kind[3], watch_events_,
                  all_texture_events_);
      // Ranges unexplained most often (the candidates for a new hook).
      std::vector<std::pair<uint32_t, uint32_t>> top;
      for (const auto& [a, e] : entries_) if (e.unexplained_checks) top.push_back({e.unexplained_checks, a});
      std::sort(top.rbegin(), top.rend());
      for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
        const Entry& e = entries_[top[i].second];
        REXLOG_INFO("gow2_res_check:   most often unexplained: {} {:08X}+{:X} at {} checks", KindName(e.kind),
                    top[i].second, e.length, top[i].first);
      }
    }
  }

  rex::memory::Memory* memory_ = nullptr;
  const Shader* vs_ = nullptr;
  const Shader* ps_ = nullptr;
  std::mutex mutex_;
  std::map<uint32_t, Entry> entries_;
  std::vector<std::pair<uint32_t, const char*>> d3d_pending_;
  std::vector<std::pair<uint32_t, uint32_t>> watch_pending_;
  uint64_t frame_ = 0, reports_ = 0, watch_events_ = 0, all_texture_events_ = 0;
  bool checking_ = false;
  Totals frame_totals_, run_;
  std::vector<std::string> examples_;
};

ResourceCheck& Check() {
  static ResourceCheck c;
  return c;
}

}  // namespace

// Used by d3d_watch.cpp's hooks. Cheap when off: one cvar read per hooked call.
void Gow2ResourceExpect(uint32_t addr, const char* event, bool all_textures) {
  if (REXCVAR_GET(gow2_res_check) > 0) Check().Expect(addr, event, all_textures);
}

void Gow2InstallResourceCheckIfRequested(rex::Runtime* runtime) {
  if (REXCVAR_GET(gow2_res_check) <= 0 || !runtime || !runtime->memory()) return;
  Check().Install(runtime->memory());
  XenosRegisterDrawObserver(&Check());
  REXLOG_INFO("gow2_res_check: comparing every {} frames (D3D events, resolves and page-watch callbacks)",
              REXCVAR_GET(gow2_res_check));
}
