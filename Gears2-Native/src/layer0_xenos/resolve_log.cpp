// Layer 0 (game-free): Stage 2 inventory - every EDRAM copy (resolve) and clear of chosen frames, with what it reads
// and where it writes. --gow2_resolve_log_frames="1500,2400" logs, for each copy in those frames: its position (draws
// before it), source (colour target k or depth, EDRAM base in tiles, format, MSAA, surface pitch), destination
// (address, pitch/height, format, number format, exponent bias, endian, array slice) and the clear flags/values. Frame N
// = the work before the N-th swap, the same numbering as --gow2_draw_stats. This is the table Stage 2 works from.
// It also logs the pass structure of those frames: a "pass" is a run of draws with the same render-target setup
// (surface pitch/MSAA, the colour targets the colour mask writes, and the depth target when depth or stencil is on);
// each change logs the finished pass's draw range and targets, plus the first draw's primitive, index count, shader
// hashes and whether a shader memexports (to identify passes that write neither colour nor depth, notes/35).
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging/macros.h>

#include <cstdint>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>

REXCVAR_DEFINE_STRING(gow2_resolve_log_frames, "", "Gears2", "Comma list of frame numbers whose EDRAM copies/clears to log");

namespace {

using namespace rex::graphics;

class ResolveLog final : public DrawObserver {
 public:
  explicit ResolveLog(std::set<uint64_t> frames) : frames_(std::move(frames)) {}
  void OnDrawShaders(const Shader* vs, const Shader* ps) override { vs_ = vs; ps_ = ps; }
  void OnDraw(const RegisterFile& regs, xenos::PrimitiveType prim, uint32_t count, const ObservedIndexBuffer*, bool) override {
    prim_ = uint32_t(prim);
    count_ = count;
    const bool copy = regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy;
    if (!copy) {
      if (frames_.count(frame_ + 1)) Pass(regs);
      ++draws_;
      return;
    }
    if (frames_.count(frame_ + 1)) EndPass();
    if (!frames_.count(frame_ + 1)) { ++copies_; return; }
    const auto cc = regs.Get<reg::RB_COPY_CONTROL>();
    const auto di = regs.Get<reg::RB_COPY_DEST_INFO>();
    const auto dp = regs.Get<reg::RB_COPY_DEST_PITCH>();
    const auto si = regs.Get<reg::RB_SURFACE_INFO>();
    const bool depth = cc.copy_src_select >= 4;
    std::string src;
    if (depth) {
      const auto d = regs.Get<reg::RB_DEPTH_INFO>();
      src = "depth base " + std::to_string(d.depth_base | (d.depth_base_bit_11 << 11)) + " fmt " +
            std::to_string(uint32_t(d.depth_format));
    } else {
      static constexpr uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
      reg::RB_COLOR_INFO c;
      c.value = regs[kColorInfo[cc.copy_src_select & 3]];
      src = "color" + std::to_string(cc.copy_src_select) + " base " + std::to_string(c.color_base | (c.color_base_bit_11 << 11)) +
            " fmt " + std::to_string(uint32_t(c.color_format)) + " exp " + std::to_string(c.color_exp_bias);
    }
    REXLOG_INFO("gow2_resolve_log: frame {} copy {} after draw {} (prim {} count {} vs {:016X}): {} {} msaa {} pitch {} sample {} | cmd {} -> dest {:08X} "
                "pitch {} height {} fmt {} num {} exp {} endian {} array {} slice {} | clear color {} ({:08X}{:08X}) depth {} ({:08X})",
                frame_ + 1, copies_, draws_, prim_, count_, vs_ ? vs_->ucode_data_hash() : 0ull, depth ? "DEPTH" : "COLOR", src, uint32_t(si.msaa_samples), si.surface_pitch,
                uint32_t(cc.copy_sample_select), uint32_t(cc.copy_command), regs[0x2319], dp.copy_dest_pitch,
                dp.copy_dest_height, uint32_t(di.copy_dest_format), uint32_t(di.copy_dest_number), di.copy_dest_exp_bias,
                uint32_t(di.copy_dest_endian), di.copy_dest_array, di.copy_dest_slice, cc.color_clear_enable, regs[0x231E],
                regs[0x231F], cc.depth_clear_enable, regs[0x231D]);
    ++copies_;
  }
  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    if (frames_.count(frame_ + 1)) EndPass();
    ++frame_;
    if (frames_.count(frame_)) REXLOG_INFO("gow2_resolve_log: frame {} had {} draws and {} copies", frame_, draws_, copies_);
    draws_ = copies_ = 0;
  }

 private:
  // Render-target setup of a draw, as text (also the pass key).
  static std::string Targets(const RegisterFile& regs) {
    static constexpr uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    const auto si = regs.Get<reg::RB_SURFACE_INFO>();
    const uint32_t mask = regs[0x2104];       // RB_COLOR_MASK, 4 bits per target
    const uint32_t depthctl = regs[0x2200];   // RB_DEPTHCONTROL: bit 0 stencil, bit 1 z
    std::string t = "pitch " + std::to_string(si.surface_pitch) + " msaa " + std::to_string(uint32_t(si.msaa_samples));
    for (int k = 0; k < 4; ++k) {
      if (!((mask >> (4 * k)) & 0xF)) continue;
      reg::RB_COLOR_INFO c;
      c.value = regs[kColorInfo[k]];
      t += " | rt" + std::to_string(k) + " base " + std::to_string(c.color_base | (c.color_base_bit_11 << 11)) + " fmt " +
           std::to_string(uint32_t(c.color_format));
    }
    if (depthctl & 3) {
      const auto d = regs.Get<reg::RB_DEPTH_INFO>();
      t += " | depth base " + std::to_string(d.depth_base | (d.depth_base_bit_11 << 11)) + " fmt " +
           std::to_string(uint32_t(d.depth_format)) + ((depthctl & 4) ? " write" : " test");
    }
    return t;
  }
  void Pass(const RegisterFile& regs) {
    std::string t = Targets(regs);
    if (pass_draws_ && t == pass_targets_) { ++pass_draws_; return; }
    EndPass();
    pass_targets_ = std::move(t);
    char first[200];
    std::snprintf(first, sizeof(first), "first draw: prim %u count %u vs %016llX ps %016llX memexport vs %u ps %u",
                  prim_, count_, vs_ ? (unsigned long long)vs_->ucode_data_hash() : 0ull,
                  ps_ ? (unsigned long long)ps_->ucode_data_hash() : 0ull, vs_ ? uint32_t(vs_->memexport_eM_written()) : 0u,
                  ps_ ? uint32_t(ps_->memexport_eM_written()) : 0u);
    pass_first_info_ = first;
    pass_first_ = draws_;
    pass_draws_ = 1;
  }
  void EndPass() {
    if (!pass_draws_) return;
    REXLOG_INFO("gow2_resolve_log: frame {} pass draws {}-{} ({}): {} || {}", frame_ + 1, pass_first_,
                pass_first_ + pass_draws_ - 1, pass_draws_, pass_targets_, pass_first_info_);
    pass_draws_ = 0;
  }

  std::string pass_targets_, pass_first_info_;
  const Shader* vs_ = nullptr;
  const Shader* ps_ = nullptr;
  uint32_t prim_ = 0, count_ = 0;
  uint32_t pass_first_ = 0, pass_draws_ = 0;
  std::set<uint64_t> frames_;
  uint64_t frame_ = 0;
  uint32_t draws_ = 0, copies_ = 0;
};

}  // namespace

void XenosInstallResolveLogIfRequested() {
  const std::string list = REXCVAR_GET(gow2_resolve_log_frames);
  if (list.empty()) return;
  std::set<uint64_t> frames;
  std::stringstream ss(list);
  for (std::string item; std::getline(ss, item, ',');)
    if (!item.empty()) frames.insert(std::stoull(item));
  if (frames.empty()) return;
  static ResolveLog log(std::move(frames));
  XenosRegisterDrawObserver(&log);
  REXLOG_INFO("gow2_resolve_log: logging copies/clears of {} frame(s)", list);
}
