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
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <string>

REXCVAR_DEFINE_STRING(gow2_resolve_log_frames, "", "Gears2", "Comma list of frame numbers whose EDRAM copies/clears to log");
// GitHub #185 (Judgment too bright, also in Xenia Canary): the eye adaptation reads the scene's average brightness from
// tiny copies at the end of a downsample chain. This logs every colour copy whose destination is at most N x N pixels
// (any frame): source format, destination address / size / format / number format / exponent bias / endian, the
// draws before it, and the first texel the copy left in guest memory (read at the next draw, as gow2_buffer_dump does;
// decoded for 16_16_FLOAT, 16_16_16_16_FLOAT, 32_FLOAT, 32_32_FLOAT, 32_32_32_32_FLOAT and 8_8_8_8). Logging only.
REXCVAR_DEFINE_INT32(gow2_resolve_log_small, 0, "Gears2",
                     "Diagnostic (GitHub #185): log colour copies (resolves) whose destination is at most N x N pixels, "
                     "with their formats and the first texel they wrote - the eye-adaptation chain. 0 = off; try 16");
REXCVAR_DEFINE_INT32(gow2_resolve_log_small_lines, 600, "Gears2",
                     "GitHub #185: the most lines --gow2_resolve_log_small writes (max 20000)");

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

// GitHub #185: see gow2_resolve_log_small.
uint32_t SwapWord(uint32_t v, rex::graphics::xenos::Endian128 e) {
  switch (e) {
    case rex::graphics::xenos::Endian128::k8in16: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case rex::graphics::xenos::Endian128::k8in32: return std::byteswap(v);
    case rex::graphics::xenos::Endian128::k16in32: return (v << 16) | (v >> 16);
    default: return v;
  }
}
float HalfToFloat(uint32_t h) {
  const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
  float v;
  if (e == 0) v = std::ldexp(float(m), -24);
  else if (e == 31) v = m ? NAN : INFINITY;
  else v = std::ldexp(float(m | 0x400u), int(e) - 25);
  return s ? -v : v;
}

class SmallResolveLog final : public DrawObserver {
 public:
  SmallResolveLog(rex::memory::Memory* memory, uint32_t max_size, uint32_t max_lines)
      : memory_(memory), max_size_(max_size), max_lines_(max_lines) {}
  void OnDraw(const RegisterFile& regs, rex::graphics::xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer*, bool) override {
    Flush();  // the previous copy has run by now (the renderer handles a copy within its own OnDraw)
    if (lines_ >= max_lines_) return;
    if (regs.Get<reg::RB_MODECONTROL>().edram_mode != rex::graphics::xenos::EdramMode::kCopy) { ++draws_; return; }
    const auto cc = regs.Get<reg::RB_COPY_CONTROL>();
    if (cc.copy_src_select >= 4) return;  // depth
    const auto dp = regs.Get<reg::RB_COPY_DEST_PITCH>();
    if (!dp.copy_dest_pitch || !dp.copy_dest_height || dp.copy_dest_pitch > max_size_ || dp.copy_dest_height > max_size_)
      return;
    const auto di = regs.Get<reg::RB_COPY_DEST_INFO>();
    static constexpr uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    reg::RB_COLOR_INFO c;
    c.value = regs[kColorInfo[cc.copy_src_select & 3]];
    char b[320];
    std::snprintf(b, sizeof(b),
                  "frame %llu after draw %u: color%u fmt %u exp %d pitch %u -> dest %08X %ux%u fmt %u num %u exp %d endian %u",
                  (unsigned long long)(frame_ + 1), draws_, uint32_t(cc.copy_src_select), uint32_t(c.color_format),
                  int(c.color_exp_bias), uint32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch),
                  regs[0x2319] & 0x1FFFFFFFu, uint32_t(dp.copy_dest_pitch), uint32_t(dp.copy_dest_height),
                  uint32_t(di.copy_dest_format), uint32_t(di.copy_dest_number), int(di.copy_dest_exp_bias),
                  uint32_t(di.copy_dest_endian));
    pending_ = b;
    pending_dest_ = regs[0x2319] & 0x1FFFFFFFu;
    pending_format_ = uint32_t(di.copy_dest_format);
    pending_endian_ = di.copy_dest_endian;
  }
  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    Flush();
    ++frame_;
    draws_ = 0;
  }

 private:
  void Flush() {
    if (pending_.empty()) return;
    std::string value = " | first texel at the destination base (the copy's x/y offset is not applied): ";
    uint32_t w[4] = {};
    const uint8_t* p = memory_->TranslatePhysical<const uint8_t*>(pending_dest_);
    uint32_t raw4[4];
    std::memcpy(raw4, p, 16);
    for (int i = 0; i < 4; ++i) {
      // k8in64 / k8in128 move bytes across 32-bit words: word i comes from word i^1 (k8in64) or 3-i (k8in128),
      // byte-reversed.
      const bool in64 = pending_endian_ == rex::graphics::xenos::Endian128::k8in64;
      const bool in128 = pending_endian_ == rex::graphics::xenos::Endian128::k8in128;
      const int from = in64 ? (i ^ 1) : in128 ? (3 - i) : i;
      const bool whole = in64 || in128;
      w[i] = whole ? std::byteswap(raw4[from]) : SwapWord(raw4[from], pending_endian_);
    }
    char v[160];
    switch (pending_format_) {
      case 31: std::snprintf(v, sizeof(v), "%g %g", HalfToFloat(w[0] & 0xFFFF), HalfToFloat(w[0] >> 16)); break;
      case 32: std::snprintf(v, sizeof(v), "%g %g %g %g", HalfToFloat(w[0] & 0xFFFF), HalfToFloat(w[0] >> 16),
                             HalfToFloat(w[1] & 0xFFFF), HalfToFloat(w[1] >> 16)); break;
      case 36: std::snprintf(v, sizeof(v), "%g", std::bit_cast<float>(w[0])); break;
      case 37: std::snprintf(v, sizeof(v), "%g %g", std::bit_cast<float>(w[0]), std::bit_cast<float>(w[1])); break;
      case 38: std::snprintf(v, sizeof(v), "%g %g %g %g", std::bit_cast<float>(w[0]), std::bit_cast<float>(w[1]),
                             std::bit_cast<float>(w[2]), std::bit_cast<float>(w[3])); break;
      case 6: std::snprintf(v, sizeof(v), "%u %u %u %u", w[0] & 0xFF, (w[0] >> 8) & 0xFF, (w[0] >> 16) & 0xFF, w[0] >> 24); break;
      default: v[0] = 0; break;
    }
    char raw[64];
    std::snprintf(raw, sizeof(raw), "raw %08X %08X", w[0], w[1]);
    value += v[0] ? std::string(v) + " (" + raw + ")" : std::string(raw);
    ++lines_;
    REXLOG_INFO("gow2_resolve_log_small: {}{}{}", pending_, value,
                lines_ == max_lines_ ? " - last line (gow2_resolve_log_small_lines)" : "");
    pending_.clear();
  }
  rex::memory::Memory* memory_;
  uint32_t max_size_, max_lines_, lines_ = 0, draws_ = 0;
  uint64_t frame_ = 0;
  std::string pending_;
  uint32_t pending_dest_ = 0, pending_format_ = 0;
  rex::graphics::xenos::Endian128 pending_endian_ = rex::graphics::xenos::Endian128::kNone;
};

}  // namespace

void XenosInstallSmallResolveLogIfRequested(rex::Runtime* runtime) {
  const int n = REXCVAR_GET(gow2_resolve_log_small);
  if (n <= 0 || !runtime || !runtime->memory()) return;
  const uint32_t lines = uint32_t(std::clamp(REXCVAR_GET(gow2_resolve_log_small_lines), 1, 20000));
  static SmallResolveLog log(runtime->memory(), uint32_t(std::min(n, 256)), lines);
  XenosRegisterDrawObserver(&log);
  REXLOG_INFO("gow2_resolve_log_small: logging colour copies up to {}x{} pixels (GitHub #185, {} lines)", std::min(n, 256),
              std::min(n, 256), lines);
}

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
