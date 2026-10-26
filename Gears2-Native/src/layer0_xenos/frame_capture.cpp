// Layer 0 (game-free): save finished frames as PNG from inside the process, using the per-swap readback of SDK patch 3.
// --gow2_frame_shots=N saves every Nth frame; --gow2_frame_dir=<folder> (default: <exe dir>\frames). PNG encoding runs on
// a background thread so the GPU thread only copies the pixels; --gow2_frame_shots_first / _last limit it to a window.
// Image: the guest output after the game's gamma ramp, before window scaling and overlays (notes/33).
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/logging/macros.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"  // from third_party/flip/src/cpp/tool (public domain / MIT), see CMakeLists.txt

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(gow2_frame_shots, 0, "Gears2", "Save every Nth finished frame as PNG (0 = off)");
REXCVAR_DEFINE_STRING(gow2_frame_dir, "", "Gears2", "Folder for --gow2_frame_shots (default: <exe dir>\\frames)");
REXCVAR_DEFINE_INT32(gow2_frame_shots_first, 0, "Gears2",
                     "With --gow2_frame_shots: first frame saved (job 277: catching a one-frame glitch while a chapter loads)");
REXCVAR_DEFINE_INT32(gow2_frame_shots_last, 0, "Gears2",
                     "With --gow2_frame_shots: last frame saved (0 = no limit). With a window, up to 16 frames may be in "
                     "flight at once instead of 2, so consecutive frames are not dropped");

namespace {

using namespace rex::graphics;

class FrameCapture final : public DrawObserver {
 public:
  FrameCapture(int every, std::filesystem::path dir, uint64_t first, uint64_t last)
      : every_(every), dir_(std::move(dir)), first_(first), last_(last) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
  }
  void OnDraw(const RegisterFile&, xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer*, bool) override {}
  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {}
  bool WantsFrameImages() const override { return true; }
  void OnFrameImage(const uint8_t* src, uint32_t width, uint32_t height, uint32_t row_pitch, uint64_t frame) override {
    if (frame < first_ || (last_ && frame > last_)) return;
    if (frame % uint64_t(every_) != 0 || in_flight_.load() > (last_ ? 16 : 2)) return;
    // R10G10B10A2_UNORM -> RGB8 (top 8 bits of each 10-bit channel).
    std::vector<uint8_t> rgb(size_t(width) * height * 3);
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* row = src + size_t(y) * row_pitch;
      uint8_t* out = rgb.data() + size_t(y) * width * 3;
      for (uint32_t x = 0; x < width; ++x) {
        uint32_t p;
        std::memcpy(&p, row + size_t(x) * 4, 4);
        out[x * 3 + 0] = uint8_t((p >> 2) & 0xFF);
        out[x * 3 + 1] = uint8_t((p >> 12) & 0xFF);
        out[x * 3 + 2] = uint8_t((p >> 22) & 0xFF);
      }
    }
    char name[64];
    std::snprintf(name, sizeof(name), "frame-%06llu.png", static_cast<unsigned long long>(frame));
    std::string path = (dir_ / name).string();
    ++in_flight_;
    std::thread([this, rgb = std::move(rgb), width, height, path]() {
      stbi_write_png(path.c_str(), int(width), int(height), 3, rgb.data(), int(width) * 3);
      --in_flight_;
    }).detach();
  }

 private:
  int every_;
  std::filesystem::path dir_;
  uint64_t first_ = 0, last_ = 0;
  std::atomic<int> in_flight_{0};
};

}  // namespace

void XenosInstallFrameCaptureIfRequested() {
  const int every = REXCVAR_GET(gow2_frame_shots);
  if (every <= 0) return;
  std::filesystem::path dir = REXCVAR_GET(gow2_frame_dir);
  if (dir.empty()) dir = std::filesystem::current_path() / "frames";
  const uint64_t first = uint64_t(std::max(REXCVAR_GET(gow2_frame_shots_first), 0));
  const uint64_t last = uint64_t(std::max(REXCVAR_GET(gow2_frame_shots_last), 0));
  static FrameCapture capture(every, dir, first, last);  // lives until exit, as the observer contract requires
  XenosRegisterDrawObserver(&capture);
  REXLOG_INFO("gow2_frame_shots: every {} frames into {}{}", every, dir.string(),
              last ? " (frames " + std::to_string(first) + "-" + std::to_string(last) + ")" : std::string());
}
