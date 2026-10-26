// Layer 0 (game-free): --gow2_prim_detail=N looks closer at three things the primitive census (job 093) left open, and
// logs a summary every N frames plus a run summary every 10th report:
//  1. Primitive type 0x12 and anything else from 0x10 up, or any draw with tessellation switched on: 0x12 is
//     k2DCopyRectListV2 normally but kQuadPatch when VGT_OUTPUT_PATH_CNTL.path_select = kTessellationEnable (Canary
//     xenos.h). Logs path_select, VGT_HOS_CNTL.tess_mode, the min/max tessellation levels, index count, whether it is
//     auto-indexed or has an index buffer (adaptive mode reads per-edge factors from it), VGT_INDX_OFFSET, the shaders,
//     and whether the index buffer starts inside a resolve destination written earlier in the same frame (GPU-written)
//     - otherwise the CPU wrote it (the shader corpus has no memexport shaders). The first 40 such draws are logged
//     one by one; after that they are counted by (prim, path, mode, indexed, vs, ps).
//  2. Point-list draws: lengths of back-to-back runs of point draws (the known sync groups are 24) and point draws by
//     vertex shader.
//  3. Quad-list draws: counts by (vs, ps) and indexed/auto-indexed, to see what they draw.
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging/macros.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

REXCVAR_DEFINE_INT32(gow2_prim_detail, 0, "Gears2",
                     "Every N frames log tessellation / 0x10+ primitive detail, point-run lengths and quad-list shaders");

namespace {

using namespace rex::graphics;

std::string Hex(uint64_t h) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(h));
  return b;
}

class PrimDetail final : public DrawObserver {
 public:
  explicit PrimDetail(int every) : every_(every) {}

  void OnDrawShaders(const Shader* vs, const Shader* ps) override {
    vs_ = vs ? vs->ucode_data_hash() : 0;
    ps_ = ps ? ps->ucode_data_hash() : 0;
  }

  void OnDraw(const RegisterFile& regs, xenos::PrimitiveType primitive, uint32_t index_count,
              const ObservedIndexBuffer* ib, bool major_mode_explicit) override {
    if (regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
      EndPointRun();
      copy_dests_.push_back(regs[XE_GPU_REG_RB_COPY_DEST_BASE]);
      return;
    }
    const uint32_t p = uint32_t(primitive);
    const auto path = uint32_t(regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select);
    const auto mode = uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode);

    if (p == uint32_t(xenos::PrimitiveType::kPointList)) {
      ++point_run_;
      ++points_by_vs_[vs_];
    } else {
      EndPointRun();
    }
    if (p == uint32_t(xenos::PrimitiveType::kQuadList)) {
      ++quads_[{vs_, ps_, ib != nullptr}];
      quad_indices_ += index_count;
    }

    if (p >= 0x10 || path == uint32_t(xenos::VGTOutputPath::kTessellationEnable)) {
      const float tmax = std::bit_cast<float>(regs[XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL]);
      const float tmin = std::bit_cast<float>(regs[XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL]);
      const uint32_t offset = regs[XE_GPU_REG_VGT_INDX_OFFSET];
      // An index buffer that starts inside something a resolve wrote this frame was written by the GPU. Resolve sizes
      // aren't tracked here, so "inside" means within 4 MB above a copy destination - a hint, not proof.
      std::string writer = "auto-indexed";
      if (ib) {
        writer = "cpu";
        for (uint32_t d : copy_dests_) {
          if (ib->guest_base >= d && ib->guest_base - d < (4u << 20)) { writer = "gpu-resolve?"; break; }
        }
      }
      ++special_[{p, path, mode, ib != nullptr, vs_, ps_}];
      if (logged_ < 40) {
        ++logged_;
        REXLOG_INFO(
            "gow2_prim_detail: draw frame {} prim 0x{:02X} path_select {} tess_mode {} min_tess {} max_tess {} count {} "
            "indx_offset {} explicit_major {} ib {} fmt {} base 0x{:08X} ib_count {} len {} writer {} vs {} ps {}",
            frames_ + 1, p, path, mode, tmin, tmax, index_count, offset, major_mode_explicit, ib ? "yes" : "no",
            ib ? uint32_t(ib->format) : 0u, ib ? ib->guest_base : 0u, ib ? ib->count : 0u,
            ib ? ib->length : 0ull, writer, Hex(vs_), Hex(ps_));
      }
    }
  }

  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    EndPointRun();
    copy_dests_.clear();
    ++frames_;
    if (every_ <= 0 || frames_ % uint64_t(every_) != 0 || ++reports_ % 10 != 0) return;
    std::string runs;
    for (auto& [len, n] : point_runs_) runs += " " + std::to_string(len) + "x" + std::to_string(n);
    REXLOG_INFO("gow2_prim_detail: SUMMARY to frame {} | point runs (length x count):{}", frames_, runs);
    Top("point draws by vs", points_by_vs_, [](uint64_t vs) { return "vs " + Hex(vs); });
    std::map<std::tuple<uint64_t, uint64_t, bool>, uint64_t> q = quads_;
    REXLOG_INFO("gow2_prim_detail: SUMMARY quad-list indices {} in {} draws", quad_indices_, Sum(q));
    Top("quad-list by shaders", q, [](const std::tuple<uint64_t, uint64_t, bool>& k) {
      return "vs " + Hex(std::get<0>(k)) + " ps " + Hex(std::get<1>(k)) + (std::get<2>(k) ? " indexed" : " auto");
    });
    Top("0x10+/tessellated by (prim path mode ib vs ps)", special_, [](const SpecialKey& k) {
      char b[64];
      std::snprintf(b, sizeof b, "prim 0x%02X path %u mode %u %s", std::get<0>(k), std::get<1>(k), std::get<2>(k),
                    std::get<3>(k) ? "indexed" : "auto");
      return std::string(b) + " vs " + Hex(std::get<4>(k)) + " ps " + Hex(std::get<5>(k));
    });
  }

 private:
  using SpecialKey = std::tuple<uint32_t, uint32_t, uint32_t, bool, uint64_t, uint64_t>;

  void EndPointRun() {
    if (point_run_) ++point_runs_[point_run_];
    point_run_ = 0;
  }
  template <typename K>
  static uint64_t Sum(const std::map<K, uint64_t>& m) {
    uint64_t s = 0;
    for (auto& kv : m) s += kv.second;
    return s;
  }
  template <typename K, typename F>
  void Top(const char* what, const std::map<K, uint64_t>& m, F name) {
    std::vector<std::pair<uint64_t, K>> v;
    for (auto& [k, n] : m) v.push_back({n, k});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    REXLOG_INFO("gow2_prim_detail: SUMMARY {}: {} distinct, {} draws", what, v.size(), Sum(m));
    for (size_t i = 0; i < v.size() && i < 12; ++i) REXLOG_INFO("gow2_prim_detail:   {} x {}", v[i].first, name(v[i].second));
  }

  int every_;
  uint64_t frames_ = 0, reports_ = 0, vs_ = 0, ps_ = 0, quad_indices_ = 0;
  uint32_t point_run_ = 0, logged_ = 0;
  std::vector<uint32_t> copy_dests_;
  std::map<uint32_t, uint64_t> point_runs_;
  std::map<uint64_t, uint64_t> points_by_vs_;
  std::map<std::tuple<uint64_t, uint64_t, bool>, uint64_t> quads_;
  std::map<SpecialKey, uint64_t> special_;
};

}  // namespace

void XenosInstallPrimDetailIfRequested() {
  const int every = REXCVAR_GET(gow2_prim_detail);
  if (every <= 0) return;
  static PrimDetail detail(every);  // lives until the process exits, as the observer contract requires
  XenosRegisterDrawObserver(&detail);
  REXLOG_INFO("gow2_prim_detail: summary every {} frames", 10 * every);
}
