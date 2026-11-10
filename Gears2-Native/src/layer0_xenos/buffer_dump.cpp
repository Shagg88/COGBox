// Layer 0 (game-free): in-frame buffer dump (D19 item 5, notes/34 "final frames vs in-frame buffers"). With
// --gow2_buffer_dump_frame=N, every EDRAM copy (resolve) of frame N is saved straight after it runs: HDR and other float
// or 16-bit destinations as OpenEXR (half float, uncompressed), 8-bit colour as PNG, depth as a greyscale EXR of the
// depth value. Needs --readback_resolve=full (the SDK then copies each resolve result back to guest memory before the
// next command; normally it stays on the GPU). Frame numbering as --gow2_draw_stats. Files go to --gow2_buffer_dump_dir
// (default <exe dir>\buffers) as frame-NNNNNN-copy-CC-<dest address>-<W>x<H>-<what>.exr/png.
// Resolve destinations are 2D tiled textures (draw.cpp: GetTiledOffset2D); the whole pitch x height area is untiled
// from the destination base, so a copy into part of a texture shows the rest of that texture too (job 068: the bloom
// copy fills 320 of 352 columns; the rest is stale memory). 8-bit colour honours copy_dest_swap (job 068: the front
// buffer matches the frame capture to 0.7/255 with red and blue swapped). Depth decodes by the source depth format.
// Byte order: the stored words are little-endian with the copy's endian mode applied on top (verified by eye in jobs
// 067-068: 16-bit floats came out as noise with an extra 32-bit byte swap).
#include <xenos/dev_tools.h>
#include <xenos/formats.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <stb_image_write.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

REXCVAR_DEFINE_INT32(gow2_buffer_dump_frame, 0, "Gears2", "Save every resolve of this frame (EXR/PNG); needs --readback_resolve=full");
REXCVAR_DEFINE_STRING(gow2_buffer_dump_dir, "", "Gears2", "Folder for --gow2_buffer_dump_frame (default: <exe dir>\\buffers)");

namespace {

using namespace rex::graphics;

uint16_t FloatToHalf(float f) {
  const uint32_t x = std::bit_cast<uint32_t>(f);
  const uint32_t sign = (x >> 16) & 0x8000;
  int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
  uint32_t m = x & 0x7FFFFF;
  if (((x >> 23) & 0xFF) == 0xFF) return uint16_t(sign | 0x7C00 | (m ? 0x200 : 0));
  if (e >= 31) return uint16_t(sign | 0x7C00);
  if (e <= 0) {
    if (e < -10) return uint16_t(sign);
    m |= 0x800000;
    const uint32_t shift = uint32_t(14 - e);
    uint32_t h = m >> shift;
    if ((m >> (shift - 1)) & 1) ++h;  // round half up
    return uint16_t(sign | h);
  }
  uint32_t h = sign | (uint32_t(e) << 10) | (m >> 13);
  if (m & 0x1000) ++h;
  return uint16_t(h);
}

// Minimal OpenEXR writer: scanline, uncompressed, HALF channels A, B, G, R (alphabetical, as the format requires).
bool WriteExr(const std::filesystem::path& path, uint32_t w, uint32_t h, const std::vector<uint16_t>& rgba) {
  std::vector<uint8_t> out;
  auto u8 = [&](uint8_t v) { out.push_back(v); };
  auto i32 = [&](int32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(uint32_t(v) >> (8 * i))); };
  auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i))); };
  auto str = [&](const char* s) { while (*s) out.push_back(uint8_t(*s++)); out.push_back(0); };
  auto f32 = [&](float f) { i32(std::bit_cast<int32_t>(f)); };
  i32(20000630); i32(2);
  str("channels"); str("chlist"); i32(4 * (2 + 16) + 1);
  for (const char* c : {"A", "B", "G", "R"}) { str(c); i32(1); u8(0); u8(0); u8(0); u8(0); i32(1); i32(1); }
  u8(0);
  str("compression"); str("compression"); i32(1); u8(0);
  str("dataWindow"); str("box2i"); i32(16); i32(0); i32(0); i32(int32_t(w) - 1); i32(int32_t(h) - 1);
  str("displayWindow"); str("box2i"); i32(16); i32(0); i32(0); i32(int32_t(w) - 1); i32(int32_t(h) - 1);
  str("lineOrder"); str("lineOrder"); i32(1); u8(0);
  str("pixelAspectRatio"); str("float"); i32(4); f32(1.0f);
  str("screenWindowCenter"); str("v2f"); i32(8); f32(0.0f); f32(0.0f);
  str("screenWindowWidth"); str("float"); i32(4); f32(1.0f);
  u8(0);
  const uint64_t line_bytes = 8 + uint64_t(w) * 4 * 2;
  const uint64_t table_end = out.size() + uint64_t(h) * 8;
  for (uint32_t y = 0; y < h; ++y) u64(table_end + y * line_bytes);
  for (uint32_t y = 0; y < h; ++y) {
    i32(int32_t(y)); i32(int32_t(w * 4 * 2));
    for (int c : {3, 2, 1, 0}) {  // A B G R
      for (uint32_t x = 0; x < w; ++x) {
        const uint16_t v = rgba[(size_t(y) * w + x) * 4 + c];
        out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8));
      }
    }
  }
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

uint32_t Swap(uint32_t v, rex::graphics::xenos::Endian128 e) {
  switch (e) {
    case rex::graphics::xenos::Endian128::k8in16: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case rex::graphics::xenos::Endian128::k8in32: return std::byteswap(v);
    case rex::graphics::xenos::Endian128::k16in32: return (v << 16) | (v >> 16);
    default: return v;
  }
}

struct Pending {
  bool active = false;
  uint32_t index = 0, dest = 0, pitch = 0, height = 0, format = 0, number = 0;
  int32_t exp = 0;
  rex::graphics::xenos::Endian128 endian = rex::graphics::xenos::Endian128::kNone;
  bool depth = false;
  bool depth_float = false;  // D24FS8 (20e4) vs D24S8 (unorm)
  bool swap_rb = false;      // RB_COPY_DEST_INFO copy_dest_swap
};

class BufferDump final : public DrawObserver {
 public:
  BufferDump(rex::memory::Memory* memory, uint64_t frame, std::filesystem::path dir)
      : memory_(memory), target_(frame), dir_(std::move(dir)) {}

  void OnDraw(const RegisterFile& regs, rex::graphics::xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer*, bool) override {
    if (frame_ + 1 != target_) return;
    Flush();  // the previous copy has run by now
    if (regs.Get<reg::RB_MODECONTROL>().edram_mode != rex::graphics::xenos::EdramMode::kCopy) return;
    const auto cc = regs.Get<reg::RB_COPY_CONTROL>();
    const auto di = regs.Get<reg::RB_COPY_DEST_INFO>();
    const auto dp = regs.Get<reg::RB_COPY_DEST_PITCH>();
    pending_ = {true, copies_++, regs[0x2319] & 0x1FFFFFFF, dp.copy_dest_pitch, dp.copy_dest_height,
                uint32_t(di.copy_dest_format), uint32_t(di.copy_dest_number), di.copy_dest_exp_bias,
                di.copy_dest_endian, cc.copy_src_select >= 4,
                regs.Get<reg::RB_DEPTH_INFO>().depth_format == rex::graphics::xenos::DepthRenderTargetFormat::kD24FS8,
                bool(di.copy_dest_swap)};
  }
  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    if (frame_ + 1 == target_) {
      Flush();
      REXLOG_INFO("gow2_buffer_dump: frame {}: saved {} of {} copies to {}", target_, saved_, copies_, dir_.string());
    }
    ++frame_;
  }

 private:
  void Flush() {
    if (!pending_.active) return;
    const Pending p = pending_;
    pending_.active = false;
    const uint32_t w = p.pitch, h = p.height;
    if (!w || !h || w > 8192 || h > 8192) return;
    const uint32_t pitch_aligned = (w + 31) & ~31u;
    unsigned bpp_log2;
    switch (p.format) {
      case 6: case 25: case 31: bpp_log2 = 2; break;   // k_8_8_8_8, k_16_16, k_16_16_FLOAT
      case 32: bpp_log2 = 3; break;                    // k_16_16_16_16_FLOAT
      default:
        REXLOG_INFO("gow2_buffer_dump: copy {} dest format {} not handled", p.index, p.format);
        return;
    }
    const uint8_t* base = memory_->TranslatePhysical<const uint8_t*>(p.dest);
    auto word = [&](uint32_t x, uint32_t y, uint32_t i) {
      const int32_t off = ::xenos::formats::Tiled2D(int32_t(x), int32_t(y), pitch_aligned, bpp_log2);
      uint32_t v;
      std::memcpy(&v, base + off + 4 * i, 4);
      return Swap(v, p.endian);  // undo the swap the resolve applied (job 067 also byte-swapped first: wrong)
    };
    char name[160];
    const bool exr = p.depth || p.format != 6;
    std::snprintf(name, sizeof(name), "frame-%06llu-copy-%02u-%08X-%ux%u-%s.%s", (unsigned long long)target_, p.index,
                  p.dest, w, h,
                  p.depth ? "depth" : p.format == 6 ? "rgba8" : p.format == 25 ? "rg16" : p.format == 31 ? "rg16f" : "rgba16f",
                  exr ? "exr" : "png");
    const auto path = dir_ / name;
    bool ok;
    if (!exr) {
      std::vector<uint8_t> px(size_t(w) * h * 4);
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          const uint32_t v = word(x, y, 0);
          uint8_t* o = &px[(size_t(y) * w + x) * 4];
          o[0] = uint8_t(p.swap_rb ? v >> 16 : v); o[1] = uint8_t(v >> 8); o[2] = uint8_t(p.swap_rb ? v : v >> 16);
          o[3] = 255;
        }
      ok = stbi_write_png(path.string().c_str(), int(w), int(h), 4, px.data(), int(w * 4)) != 0;
    } else {
      std::vector<uint16_t> px(size_t(w) * h * 4);
      const uint16_t one = FloatToHalf(1.0f);
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          uint16_t* o = &px[(size_t(y) * w + x) * 4];
          const uint32_t v = word(x, y, 0);
          if (p.depth) {
            // Depth resolve: 24-bit depth in the upper bits, stencil in the low byte; D24FS8 is 20e4, D24S8 unorm.
            const float d = p.depth_float ? ::xenos::formats::Float20e4To32(v >> 8) : float(v >> 8) / 16777215.0f;
            o[0] = o[1] = o[2] = FloatToHalf(d); o[3] = one;
          } else if (p.format == 32) {
            const uint32_t v1 = word(x, y, 1);
            o[0] = uint16_t(v); o[1] = uint16_t(v >> 16); o[2] = uint16_t(v1); o[3] = uint16_t(v1 >> 16);
          } else if (p.format == 31) {
            o[0] = uint16_t(v); o[1] = uint16_t(v >> 16); o[2] = 0; o[3] = one;
          } else {  // k_16_16 unorm
            o[0] = FloatToHalf(float(v & 0xFFFF) / 65535.0f); o[1] = FloatToHalf(float(v >> 16) / 65535.0f);
            o[2] = 0; o[3] = one;
          }
        }
      ok = WriteExr(path, w, h, px);
    }
    {
      // Raw copy of the destination (tiled, as stored) so decoding can be fixed offline without another run.
      const uint32_t raw_bytes = (pitch_aligned * ((h + 31) & ~31u)) << bpp_log2;
      FILE* f = std::fopen((path.string() + ".raw").c_str(), "wb");
      if (f) { std::fwrite(base, 1, raw_bytes, f); std::fclose(f); }
    }
    if (ok) ++saved_;
    REXLOG_INFO("gow2_buffer_dump: copy {} -> {}{}", p.index, name, ok ? "" : " (write FAILED)");
  }

  rex::memory::Memory* memory_;
  uint64_t target_;
  std::filesystem::path dir_;
  uint64_t frame_ = 0;
  uint32_t copies_ = 0, saved_ = 0;
  Pending pending_;
};

}  // namespace

void XenosInstallBufferDumpIfRequested(rex::Runtime* runtime) {
  const int frame = REXCVAR_GET(gow2_buffer_dump_frame);
  if (frame <= 0 || !runtime || !runtime->memory()) return;
  std::filesystem::path dir = REXCVAR_GET(gow2_buffer_dump_dir);
  if (dir.empty()) dir = std::filesystem::current_path() / "buffers";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  static BufferDump dump(runtime->memory(), uint64_t(frame), dir);
  XenosRegisterDrawObserver(&dump);
  REXLOG_INFO("gow2_buffer_dump: saving the resolves of frame {} to {}", frame, dir.string());
}
