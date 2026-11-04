// Layer 0 (game-free): save finished frames as PNG from inside the process, using the per-swap readback of SDK patch 3.
// --gow2_frame_shots=N saves every Nth frame; --gow2_frame_dir=<folder> (default: <exe dir>\frames). PNG encoding runs on
// a background thread so the GPU thread only copies the pixels; --gow2_frame_shots_first / _last limit it to a window.
// Image: the guest output after the game's gamma ramp, before window scaling and overlays (notes/33).
#include <xenos/dev_tools.h>

#include <rex/filesystem.h>  // rex::to_path (GitHub #177: UTF-8 path cvars)
#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/logging/macros.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"  // from third_party/flip/src/cpp/tool (public domain / MIT), see CMakeLists.txt

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   // GetModuleFileNameA (the screenshot's game name)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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

// Shagg 3 Nov: the official screenshot key, \ (Backslash), in all four games. One press saves the next finished frame
// as Pictures\COGBox\<game>\<game>-YYYYMMDD-HHMMSS.png (the game's output after its gamma ramp, as
// --gow2_frame_shots). Frame images are asked for only while a shot is pending, so the key costs nothing until pressed.
class Screenshot final : public DrawObserver {
 public:
  void OnDraw(const RegisterFile&, xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer*, bool) override {}
  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {}
  bool WantsFrameImages() const override { return pending_.load(); }
  void Request() { pending_.store(true); }
  void OnFrameImage(const uint8_t* src, uint32_t width, uint32_t height, uint32_t row_pitch, uint64_t) override {
    if (!pending_.exchange(false)) return;
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
    std::filesystem::path dir = Folder();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    std::filesystem::path path = dir / (game_ + "-" + stamp + ".png");
    for (int i = 2; std::filesystem::exists(path, ec) && i < 100; ++i)
      path = dir / (game_ + "-" + stamp + "-" + std::to_string(i) + ".png");
    std::thread([rgb = std::move(rgb), width, height, path]() {
      const bool ok = stbi_write_png(path.string().c_str(), int(width), int(height), 3, rgb.data(), int(width) * 3) != 0;
      REXLOG_INFO("cogbox_screenshot: {} {} ({}x{})", ok ? "saved" : "could not save", path.string(), width, height);
    }).detach();
  }
  std::string game_ = "game";

 private:
  std::filesystem::path Folder() const {
    // The real Pictures folder (it may be moved, e.g. to OneDrive - job 1187 saved to %USERPROFILE%\Pictures while
    // Explorer's Pictures was elsewhere): SHGetKnownFolderPath(FOLDERID_Pictures), looked up at run time so no
    // game needs to link shell32 / ole32; %USERPROFILE%\Pictures if that fails.
    std::filesystem::path base;
    using KnownFolderFn = HRESULT(WINAPI*)(const GUID&, DWORD, HANDLE, PWSTR*);
    using FreeFn = void(WINAPI*)(LPVOID);
    static const GUID kPictures = {0x33E28130, 0x4E1E, 0x4676, {0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB}};
    if (HMODULE sh = LoadLibraryA("shell32.dll")) {
      auto known = reinterpret_cast<KnownFolderFn>(GetProcAddress(sh, "SHGetKnownFolderPath"));
      HMODULE ole = LoadLibraryA("ole32.dll");
      auto free_mem = ole ? reinterpret_cast<FreeFn>(GetProcAddress(ole, "CoTaskMemFree")) : nullptr;
      PWSTR w = nullptr;
      if (known && known(kPictures, 0, nullptr, &w) == S_OK && w) base = std::filesystem::path(w);
      if (w && free_mem) free_mem(w);
    }
    if (base.empty()) {
      const char* up = std::getenv("USERPROFILE");
      base = up && *up ? std::filesystem::path(up) / "Pictures" : std::filesystem::current_path();
    }
    return base / "COGBox" / game_;
  }
  std::atomic<bool> pending_{false};
};

Screenshot* g_screenshot = nullptr;
}  // namespace

void XenosRequestScreenshot() { if (g_screenshot) g_screenshot->Request(); }

void XenosInstallFrameCaptureIfRequested() {
  {  // Shagg 3 Nov: the screenshot key, every game, always
    static Screenshot shot;
    char exe[260] = {};
    if (GetModuleFileNameA(nullptr, exe, sizeof(exe))) {
      const std::string stem = std::filesystem::path(exe).stem().string();
      shot.game_ = stem == "gow1" ? "Gears1" : stem == "gow2" ? "Gears2" : stem == "gow3" ? "Gears3" : stem == "gowj" ? "Judgment" : stem;
    }
    XenosRegisterDrawObserver(&shot);
    g_screenshot = &shot;   // the key itself is bound with the window (ingame_menu.cpp, XenosInstallInGameMenu)
    REXLOG_INFO("cogbox_screenshot: \\ saves a screenshot to Pictures\\COGBox\\{}", shot.game_);
  }
  const int every = REXCVAR_GET(gow2_frame_shots);
  if (every <= 0) return;
  std::filesystem::path dir = rex::to_path(REXCVAR_GET(gow2_frame_dir));  // GitHub #177: UTF-8
  if (dir.empty()) dir = std::filesystem::current_path() / "frames";
  const uint64_t first = uint64_t(std::max(REXCVAR_GET(gow2_frame_shots_first), 0));
  const uint64_t last = uint64_t(std::max(REXCVAR_GET(gow2_frame_shots_last), 0));
  static FrameCapture capture(every, dir, first, last);  // lives until exit, as the observer contract requires
  XenosRegisterDrawObserver(&capture);
  REXLOG_INFO("gow2_frame_shots: every {} frames into {}{}", every, dir.string(),
              last ? " (frames " + std::to_string(first) + "-" + std::to_string(last) + ")" : std::string());
}
