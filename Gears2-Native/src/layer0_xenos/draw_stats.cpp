// Layer 0 (game-free): first user of SDK patch 2 (the draw observer). --gow2_draw_stats=N logs, every N frames, how many
// draws, EDRAM copies (resolves) and primitives of each type the guest issued per frame. Proves the hook sees every draw
// with its register state; the Stage 0 comparison and our renderer build on the same observer.
// Every primitive type outside the usual seven (triangle-with-W-flags, line loop, quad strip, polygon, and the 2D /
// tessellation types from 0x10 up) is listed as "other type:count" - Canary drops kPolygon, so the renderer needs to
// know which of these Gears 2 issues (Brief 10). A run total of every type seen is logged every 10th report.
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging/macros.h>

#include <array>
#include <cstdint>
#include <string>

REXCVAR_DEFINE_INT32(gow2_draw_stats, 0, "Gears2", "Log per-frame draw/copy counts every N frames (0 = off)");

namespace {

using namespace rex::graphics;

class DrawStats final : public DrawObserver {
 public:
  explicit DrawStats(int every) : every_(every) {}
  void OnDraw(const RegisterFile& regs, xenos::PrimitiveType primitive, uint32_t index_count,
              const ObservedIndexBuffer* index_buffer, bool) override {
    if (regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) { ++copies_; return; }
    ++draws_;
    indices_ += index_count;
    if (index_buffer) ++indexed_;
    const uint32_t p = uint32_t(primitive);
    if (p < prims_.size()) ++prims_[p];
  }
  void OnSwap(const RegisterFile&, uint32_t frontbuffer, uint32_t width, uint32_t height) override {
    ++frames_;
    for (size_t i = 0; i < prims_.size(); ++i) total_[i] += prims_[i];
    if (every_ > 0 && frames_ % uint64_t(every_) == 0) {
      std::string other;
      for (uint32_t t : {0x07u, 0x0Cu, 0x0Eu, 0x0Fu}) {
        if (prims_[t]) other += " " + std::to_string(t) + ":" + std::to_string(prims_[t]);
      }
      for (uint32_t t = 0x10; t < prims_.size(); ++t) {
        if (prims_[t]) other += " " + std::to_string(t) + ":" + std::to_string(prims_[t]);
      }
      REXLOG_INFO("gow2_draw_stats: frame {} draws {} (indexed {}) copies {} indices {} | tri-list {} tri-strip {} "
                  "tri-fan {} rect-list {} quad-list {} line {} point {} | other{} | front 0x{:08X} {}x{}",
                  frames_, draws_, indexed_, copies_, indices_, prims_[4], prims_[6], prims_[5], prims_[8], prims_[13],
                  prims_[2] + prims_[3], prims_[1], other.empty() ? " none" : other, frontbuffer, width, height);
      if (++reports_ % 10 == 0) {
        std::string all;
        for (size_t i = 0; i < total_.size(); ++i)
          if (total_[i]) all += " " + std::to_string(i) + ":" + std::to_string(total_[i]);
        REXLOG_INFO("gow2_draw_stats: RUN TOTAL to frame {} by primitive type (decimal type:count):{}", frames_, all);
      }
    }
    draws_ = indexed_ = copies_ = indices_ = 0;
    prims_.fill(0);
  }

 private:
  int every_;
  uint64_t frames_ = 0, draws_ = 0, indexed_ = 0, copies_ = 0, indices_ = 0;
  std::array<uint64_t, 32> prims_{};
  std::array<uint64_t, 32> total_{};  // whole run, for the primitive census
  uint64_t reports_ = 0;
};

}  // namespace

void XenosInstallDrawStatsIfRequested() {
  const int every = REXCVAR_GET(gow2_draw_stats);
  if (every <= 0) return;
  static DrawStats stats(every);  // lives until the process exits, as the observer contract requires
  XenosRegisterDrawObserver(&stats);
  REXLOG_INFO("gow2_draw_stats: logging every {} frames", every);
}
