// Layer 0 (game-free): per-draw log for chosen frames, to find which draws differ between two runs (notes/34 item 2).
// --gow2_draw_log_frames="1200,1350" writes <dir>/drawlog-001200.txt etc.: one line per draw (or EDRAM copy) of that
// frame with primitive, index count, index buffer, and hashes of the render state, ALU constants, fetch constants and
// bool/loop constants. Frame N = the draws before the N-th swap, the same numbering as --gow2_draw_stats. Two runs'
// logs are aligned and diffed offline (draws that appear, vanish, or change constants).
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging/macros.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>

REXCVAR_DEFINE_STRING(gow2_draw_log_frames, "", "Gears2", "Comma list of frame numbers to log draw by draw");
REXCVAR_DEFINE_INT32(gow2_draw_log_full, 0, "Gears2", "Also dump every ALU/fetch/bool constant register for the first N draws of each logged frame");
REXCVAR_DEFINE_STRING(gow2_draw_log_dir, "", "Gears2", "Folder for --gow2_draw_log_frames (default: <exe dir>\\drawlog)");
REXCVAR_DEFINE_BOOL(gow2_draw_log_textures, false, "Gears2",
                    "With --gow2_draw_log_frames: under each draw, every texture fetch constant (slot, format, size, "
                    "tiling, pitch, endian, swizzle, base and mip address, raw words without the addresses) - job 283: "
                    "which texture a one-frame colour garble reads");

namespace {

using namespace rex::graphics;

uint32_t Fnv(const RegisterFile& regs, uint32_t first, uint32_t last) {
  uint32_t h = 2166136261u;
  for (uint32_t r = first; r <= last; ++r) {
    uint32_t v = regs[r];
    for (int i = 0; i < 4; ++i) { h ^= (v & 0xFF); h *= 16777619u; v >>= 8; }
  }
  return h;
}

class DrawLog final : public DrawObserver {
 public:
  DrawLog(std::set<uint64_t> frames, std::filesystem::path dir) : frames_(std::move(frames)), dir_(std::move(dir)) {}
  void OnDraw(const RegisterFile& regs, xenos::PrimitiveType primitive, uint32_t index_count,
              const ObservedIndexBuffer* ib, bool) override {
    if (!logging_) return;
    const bool copy = regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "%5u %s prim %2u count %7u ib %08X/%u state %08X alu %08X fetch %08X bl %08X surf %08X color %08X\n",
                  index_++, copy ? "COPY" : "DRAW", uint32_t(primitive), index_count, ib ? ib->guest_base : 0u,
                  ib ? ib->count : 0u, Fnv(regs, 0x2000, 0x23FF), Fnv(regs, 0x4000, 0x47FF),
                  Fnv(regs, 0x4800, 0x48BF), Fnv(regs, 0x4900, 0x4927), regs[0x2000], regs[0x2001]);
    text_ += line;
    if (!copy && REXCVAR_GET(gow2_draw_log_textures)) {
      // Texture fetch constant: 6 words at 0x4800 + 6 * slot. Word 0: type (bits 0-1, 2 = texture), pitch / 32 (22-30),
      // tiled (31); word 1: format (0-5), endian (6-7), base address / 4 KB (12-31); word 2: size (2D: width-1 0-12,
      // height-1 13-25); word 3: swizzle (1-12); word 5: dimension (9-10), mip address / 4 KB (12-31). The addresses
      // are printed apart and masked out of the raw words, so two runs with a different memory layout still compare.
      for (uint32_t s = 0; s < 32; ++s) {
        const uint32_t r = 0x4800 + s * 6;
        const uint32_t w0 = regs[r], w1 = regs[r + 1], w2 = regs[r + 2], w3 = regs[r + 3], w4 = regs[r + 4],
                       w5 = regs[r + 5];
        if ((w0 & 3) != 2) continue;
        char t[256];
        std::snprintf(t, sizeof(t),
                      "      T%02u fmt %2u %5ux%-5u dim %u tiled %u pitch %4u endian %u swz %03X | base %08X mip %08X | "
                      "raw %08X %03X %08X %08X %08X %03X\n",
                      s, w1 & 63, (w2 & 0x1FFF) + 1, ((w2 >> 13) & 0x1FFF) + 1, (w5 >> 9) & 3, w0 >> 31,
                      ((w0 >> 22) & 0x1FF) << 5, (w1 >> 6) & 3, (w3 >> 1) & 0xFFF, w1 & 0xFFFFF000u, w5 & 0xFFFFF000u,
                      w0, w1 & 0xFFF, w2, w3, w4, w5 & 0xFFF);
        text_ += t;
      }
    }
    if (index_ <= uint32_t(REXCVAR_GET(gow2_draw_log_full))) {
      // Full dump: register index and value for the constant ranges, so two runs can be diffed register by register.
      for (uint32_t r : {0x4000u, 0x4800u, 0x4900u}) {
        const uint32_t end = r == 0x4000u ? 0x47FFu : r == 0x4800u ? 0x48BFu : 0x4927u;
        for (uint32_t i = r; i <= end; ++i) {
          char v[32];
          std::snprintf(v, sizeof(v), "  R%04X %08X\n", i, regs[i]);
          text_ += v;
        }
      }
    }
  }
  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    ++frames_seen_;
    if (logging_) {
      char name[64];
      std::snprintf(name, sizeof(name), "drawlog-%06llu.txt", static_cast<unsigned long long>(frames_seen_));
      std::error_code ec;
      std::filesystem::create_directories(dir_, ec);
      if (FILE* f = std::fopen((dir_ / name).string().c_str(), "wb")) {
        std::fwrite(text_.data(), 1, text_.size(), f);
        std::fclose(f);
      }
      REXLOG_INFO("gow2_draw_log: frame {} -> {} ({} draws/copies)", frames_seen_, name, index_);
    }
    text_.clear();
    index_ = 0;
    logging_ = frames_.count(frames_seen_ + 1) != 0;  // the next frame's draws come before swap frames_seen_+1
  }

 private:
  std::set<uint64_t> frames_;
  std::filesystem::path dir_;
  uint64_t frames_seen_ = 0;
  uint32_t index_ = 0;
  bool logging_ = false;
  std::string text_;
};

}  // namespace

void XenosInstallDrawLogIfRequested() {
  const std::string list = REXCVAR_GET(gow2_draw_log_frames);
  if (list.empty()) return;
  std::set<uint64_t> frames;
  std::stringstream ss(list);
  std::string item;
  while (std::getline(ss, item, ',')) if (!item.empty()) frames.insert(std::stoull(item));
  std::filesystem::path dir = REXCVAR_GET(gow2_draw_log_dir);
  if (dir.empty()) dir = "drawlog";
  static DrawLog log(frames, dir);
  XenosRegisterDrawObserver(&log);
  REXLOG_INFO("gow2_draw_log: {} frame(s) into {}", frames.size(), dir.string());
}
