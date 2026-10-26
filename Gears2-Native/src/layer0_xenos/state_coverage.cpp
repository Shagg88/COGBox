// Layer 0 (game-free) tool, Stage 5 (notes/49): which GPU registers the game writes that our renderer does not read.
// Behind --gow2_state_coverage=N (0 = off). For every draw and copy it diffs the context/control registers
// (0x2000-0x3FFF; the ALU/fetch/bool/loop constants above 0x4000 are consumed through the shader bindings) against the
// previous draw and counts, per register, the draws where the value changed and the distinct values seen. Every N
// frames it logs the registers that changed during the run (and the ones that hold a non-zero value without changing)
// that are NOT in our renderer's read list - the Stage 5 checklist: each must be shown not to affect output, or be
// implemented. Our code (MIT).
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/register_file.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

REXCVAR_DEFINE_INT32(gow2_state_coverage, 0, "Gears2",
                     "Stage 5: every N frames, log the GPU registers (0x2000-0x3FFF) the game writes that our renderer "
                     "does not read (0 = off)");
REXCVAR_DEFINE_INT32(gow2_state_coverage_lines, 80, "Gears2", "Most registers listed per report");

namespace {

using namespace rex::graphics;

constexpr uint32_t kFirst = 0x2000, kEnd = 0x4000, kCount = kEnd - kFirst;

// Registers our renderer reads (collected from layer 0: renderer/pipeline_state.h, resolve.h, queries.h and
// shadow_renderer.cpp's register constants, 2026-10-21). Index buffer and primitive type come through OnDraw itself.
struct Known {
  uint32_t reg;
  const char* name;
};
constexpr Known kConsumed[] = {
    {0x2000, "RB_SURFACE_INFO"},
    {0x2001, "RB_COLOR_INFO"},
    {0x2002, "RB_DEPTH_INFO"},
    {0x2003, "RB_COLOR1_INFO"},
    {0x2004, "RB_COLOR2_INFO"},
    {0x2005, "RB_COLOR3_INFO"},
    {0x200e, "PA_SC_SCREEN_SCISSOR_TL"},
    {0x200f, "PA_SC_SCREEN_SCISSOR_BR"},
    {0x2080, "PA_SC_WINDOW_OFFSET"},
    {0x2081, "PA_SC_WINDOW_SCISSOR_TL"},
    {0x2082, "PA_SC_WINDOW_SCISSOR_BR"},
    {0x2102, "kRegVgtIndxOffset"},
    {0x2103, "kRegVgtMultiPrimIbResetIndx"},
    {0x2104, "RB_COLOR_MASK"},
    {0x2105, "RB_BLEND_RED"},
    {0x2106, "RB_BLEND_GREEN"},
    {0x2107, "RB_BLEND_BLUE"},
    {0x2108, "RB_BLEND_ALPHA"},
    {0x210c, "RB_STENCILREFMASK_BF"},
    {0x210d, "RB_STENCILREFMASK"},
    {0x210e, "RB_ALPHA_REF"},
    {0x210f, "PA_CL_VPORT_XSCALE"},
    {0x2110, "PA_CL_VPORT_XOFFSET"},
    {0x2111, "PA_CL_VPORT_YSCALE"},
    {0x2112, "PA_CL_VPORT_YOFFSET"},
    {0x2113, "PA_CL_VPORT_ZSCALE"},
    {0x2114, "PA_CL_VPORT_ZOFFSET"},
    {0x2180, "SQ_PROGRAM_CNTL"},
    {0x2181, "kRegSqContextMisc"},
    {0x21f9, "kRegVgtEventInitiator"},
    {0x2200, "RB_DEPTHCONTROL"},
    {0x2201, "RB_BLENDCONTROL0"},
    {0x2202, "RB_COLORCONTROL"},
    {0x2204, "PA_CL_CLIP_CNTL"},
    {0x2205, "PA_SU_SC_MODE_CNTL"},
    {0x2206, "PA_CL_VTE_CNTL"},
    {0x2208, "RB_MODECONTROL"},
    {0x2209, "RB_BLENDCONTROL1"},
    {0x220a, "RB_BLENDCONTROL2"},
    {0x220b, "RB_BLENDCONTROL3"},
    {0x2284, "VGT_OUTPUT_PATH_CNTL"},
    {0x2293, "PA_SC_VIZ_QUERY"},
    {0x2302, "PA_SU_VTX_CNTL"},
    {0x2318, "kRegCopyControl"},
    {0x2319, "kRegCopyDestBase"},
    {0x231a, "kRegCopyDestPitch"},
    {0x231b, "kRegCopyDestInfo"},
    {0x231d, "kRegDepthClear"},
    {0x231e, "kRegColorClear"},
    {0x231f, "kRegColorClearLo"},
    {0x2323, "kRegCopySurfaceSlice"},
    {0x2324, "kRegRbSampleCountCtl"},
    {0x2325, "kRegRbSampleCountAddr"},
    {0x2380, "PA_SU_POLY_OFFSET_FRONT_SCALE"},
    {0x2381, "PA_SU_POLY_OFFSET_FRONT_OFFSET"},
    {0x2382, "PA_SU_POLY_OFFSET_BACK_SCALE"},
    {0x2383, "PA_SU_POLY_OFFSET_BACK_OFFSET"}};

struct Stat {
  uint64_t changes = 0;      // draws where the value differed from the previous draw
  uint64_t nonzero_draws = 0;
  uint32_t first = 0, last = 0;
  std::array<uint32_t, 4> values{};
  uint32_t value_count = 0;  // distinct values seen (saturates at 99)
  bool seen_nonzero = false;
};

class StateCoverage final : public DrawObserver {
 public:
  explicit StateCoverage(int every) : every_(every) {
    for (const Known& k : kConsumed) consumed_[k.reg - kFirst] = true;
  }

  void OnDraw(const RegisterFile& regs, xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer*, bool) override {
    const uint32_t* v = &regs.values[kFirst];
    ++draws_;
    if (!have_prev_) {
      for (uint32_t i = 0; i < kCount; ++i) {
        stats_[i].first = stats_[i].last = v[i];
        Note(i, v[i]);
      }
      std::memcpy(prev_.data(), v, sizeof(uint32_t) * kCount);
      have_prev_ = true;
      return;
    }
    // Compare in blocks of 64 registers; most blocks are unchanged between draws.
    for (uint32_t b = 0; b < kCount; b += 64) {
      if (std::memcmp(&prev_[b], &v[b], 64 * sizeof(uint32_t)) == 0) continue;
      for (uint32_t i = b; i < b + 64; ++i) {
        if (prev_[i] == v[i]) continue;
        ++stats_[i].changes;
        stats_[i].last = v[i];
        Note(i, v[i]);
        prev_[i] = v[i];
      }
    }
  }

  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    ++frames_;
    if (frames_ % uint64_t(every_) == 0) Report();
  }

 private:
  void Note(uint32_t i, uint32_t value) {
    Stat& s = stats_[i];
    if (value) s.seen_nonzero = true;
    for (uint32_t k = 0; k < std::min<uint32_t>(s.value_count, 4); ++k)
      if (s.values[k] == value) return;
    if (s.value_count < 4) s.values[s.value_count] = value;
    if (s.value_count < 99) ++s.value_count;
  }

  static std::string Name(uint32_t reg) {
    // The SDK's register table, read at compile time (RegisterFile::GetRegisterInfo is not exported to us - job 129).
    struct Entry {
      uint32_t index;
      const char* name;
    };
    static constexpr Entry kTable[] = {
#define XE_GPU_REGISTER(index, type, name) {index, #name},
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
    };
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04X", reg);
    for (const Entry& e : kTable)
      if (e.index == reg) return std::string(e.name) + " (" + buf + ")";
    return std::string("unnamed ") + buf;
  }

  void Report() {
    std::vector<uint32_t> changing, fixed;
    uint32_t written = 0, written_consumed = 0;
    for (uint32_t i = 0; i < kCount; ++i) {
      const Stat& s = stats_[i];
      if (!s.changes && !s.seen_nonzero) continue;  // never written to a non-zero value, never changed
      ++written;
      if (consumed_[i]) {
        ++written_consumed;
        continue;
      }
      (s.changes ? changing : fixed).push_back(i);
    }
    std::sort(changing.begin(), changing.end(), [&](uint32_t a, uint32_t b) { return stats_[a].changes > stats_[b].changes; });
    REXLOG_INFO("gow2_state_coverage: frames 1-{}, draws {}: registers 0x2000-0x3FFF in use {} - read by our renderer "
                "{}, NOT read {} ({} changing, {} set once)",
                frames_, draws_, written, written_consumed, changing.size() + fixed.size(), changing.size(), fixed.size());
    const int max_lines = std::max(1, REXCVAR_GET(gow2_state_coverage_lines));
    int lines = 0;
    auto line = [&](uint32_t i, const char* kind) {
      const Stat& s = stats_[i];
      std::string vals;
      for (uint32_t k = 0; k < std::min<uint32_t>(s.value_count, 4); ++k) {
        char b[16];
        std::snprintf(b, sizeof(b), "%s%08X", k ? " " : "", s.values[k]);
        vals += b;
      }
      REXLOG_INFO("gow2_state_coverage:   {} {}: changes {} ({:.2f}/draw), distinct values {}{}: {}", kind,
                  Name(kFirst + i), s.changes, draws_ ? double(s.changes) / double(draws_) : 0.0, s.value_count,
                  s.value_count > 4 ? " (first 4)" : "", vals);
    };
    for (uint32_t i : changing) {
      if (lines++ >= max_lines) break;
      line(i, "CHANGING");
    }
    for (uint32_t i : fixed) {
      if (lines++ >= max_lines) break;
      line(i, "SET ONCE");
    }
  }

  int every_;
  bool have_prev_ = false;
  uint64_t draws_ = 0, frames_ = 0;
  std::array<uint32_t, kCount> prev_{};
  std::array<Stat, kCount> stats_{};
  std::array<bool, kCount> consumed_{};
};

}  // namespace

void XenosInstallStateCoverageIfRequested() {
  const int every = REXCVAR_GET(gow2_state_coverage);
  if (every <= 0) return;
  XenosRegisterDrawObserver(new StateCoverage(every));  // never destroyed (the hub keeps the pointer)
  REXLOG_INFO("gow2_state_coverage: on, report every {} frames ({} registers on our read list)", every,
              sizeof(kConsumed) / sizeof(kConsumed[0]));
}
