// Layer 0 (game-free): --gow2_fps_bar=top|bottom - a one-line frame-rate readout drawn by us (an ImGui window on the
// SDK's overlay layer, over the game's picture), with no change to the game: guest frames per second (swaps counted
// by the observer hub, averaged over half a second), the frame time that gives, and the slowest frame in that half
// second. For both games (shared layer 0); the game's own counter (--gow2_patch_show_fps) is a column and a patch.
// Our code (MIT).
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

REXCVAR_DEFINE_STRING(gow2_fps_bar, "off", "Gears2",
                      "A one-line frame-rate bar drawn over the game, no game patch: off, top or bottom");
REXCVAR_DEFINE_BOOL(gow2_shader_note, true, "Gears2",
                    "Patch 0062: a small 'Building shaders N%' note in the bottom-left corner while the renderer's "
                    "pipeline workers have had work queued for more than a second (start-up backlog, new areas)");

namespace {

using PreparationPhase = cogbox::shader_progress::ShaderPreparationPhase;
using PreparationProgress = cogbox::shader_progress::ShaderPreparationProgress;
std::mutex g_startup_mutex;
PreparationProgress g_startup;
std::chrono::steady_clock::time_point g_startup_started{};
PreparationProgress StartupProgress() {
  std::lock_guard<std::mutex> lock(g_startup_mutex);
  return g_startup;
}
bool StartupProgressActive() { return StartupProgress().phase != PreparationPhase::kInactive; }

class FpsBar final : public rex::ui::ImGuiDialog {
 public:
  FpsBar(rex::ui::ImGuiDrawer* drawer, bool top) : ImGuiDialog(drawer), top_(top) {}
  // 0.2.1 (patch 42): the bar is redrawn with each game frame anyway; asking for more made NVIDIA / RTSS count the
  // presenter's extra repaints (monitor rate) instead of the game's frames.
  bool WantsContinuousPaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (StartupProgressActive()) return;
    using clock = std::chrono::steady_clock;
    const clock::time_point now = clock::now();
    const uint64_t swaps = XenosSwapCount();
    if (swaps != last_swaps_) {  // slowest guest frame seen at the overlay's own rate (good to a host frame)
      if (last_swap_time_.time_since_epoch().count() && swaps == last_swaps_ + 1)
        worst_ms_ = std::max(worst_ms_, std::chrono::duration<double, std::milli>(now - last_swap_time_).count());
      last_swaps_ = swaps;
      last_swap_time_ = now;
    }
    if (!window_start_.time_since_epoch().count()) window_start_ = now, window_swaps_ = swaps;
    const double span = std::chrono::duration<double>(now - window_start_).count();
    if (span >= 0.5) {
      fps_ = double(swaps - window_swaps_) / span;
      shown_worst_ = worst_ms_;
      worst_ms_ = 0.0;
      window_start_ = now;
      window_swaps_ = swaps;
    }
    char text[128];
    if (fps_ > 0.5)
      std::snprintf(text, sizeof(text), "%.0f FPS     %.1f ms     slowest %.1f ms", fps_, 1000.0 / fps_, shown_worst_);
    else
      std::snprintf(text, sizeof(text), "-- FPS");
    const float scale = std::max(1.0f, io.DisplaySize.y / 1080.0f);  // readable at 1440p / 4K
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float w = size.x * scale + 20.0f * scale, h = size.y * scale + 8.0f * scale;
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - w) * 0.5f, top_ ? 0.0f : io.DisplaySize.y - h));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    if (ImGui::Begin("##gow2_fps_bar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoBringToFrontOnFocus)) {
      // Drawn at the scaled size through the draw list (the same call in every ImGui version we may meet).
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale, at,
                                          IM_COL32(140, 255, 115, 255), text);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
  }

 private:
  bool top_;
  double fps_ = 0.0, worst_ms_ = 0.0, shown_worst_ = 0.0;
  uint64_t last_swaps_ = 0, window_swaps_ = 0;
  std::chrono::steady_clock::time_point last_swap_time_{}, window_start_{};
};

std::unique_ptr<FpsBar> g_bar;

// Patch 0062 (Shagg: no Prepare button - building ahead happens by itself, in menus and loading screens): the
// renderer reports its pipeline jobs once a frame; this note shows the share of the current batch already built. A
// batch starts when the queue was last empty, so the percentage covers the start-up backlog at boot and only the new
// ones later. Shown after a second of queued work (single pipelines built in a few frames never show it).
std::atomic<uint64_t> g_jobs_left{0}, g_jobs_ever{0};
// Patch 0067 (Shagg 19:53-19:54: a hold must not look like a frozen game): while the renderer holds a frame back
// (0063 / 0064, the game paused behind it), an animated "Loading shaders N%" indicator - a spinning arc - is drawn
// over the last picture, bottom right; N = the held frame's pipelines already built.
std::atomic<bool> g_hold_active{false};
std::atomic<uint32_t> g_hold_total{0}, g_hold_left{0};
std::atomic<bool> g_hold_compiling{false};  // prebuild v2: the compile step is part of this hold
// Prebuild v2 indicator (XenosSetPrebuildProgress, from the prebuild thread - it runs on while a load presents nothing):
// the package shaders still to compile, how many were ever taken in, and whether the game is loading now.
std::atomic<uint64_t> g_pre_left{0}, g_pre_ever{0};
std::atomic<bool> g_pre_loading{false};
std::atomic<bool> g_scan_active{false};
std::atomic<uint64_t> g_scan_done{0}, g_scan_total{0};

// 0.2.1 (GitHub #18 / #23: NVIDIA and RTSS read about 3x the game's fps): an ImGuiDrawer with any dialog registered
// asks the presenter for a repaint every UI tick, so this note - registered all the time though it draws nothing
// while no shaders are building - kept the presenter repainting at the monitor's rate (180 Hz on Shagg's PC), and the
// overlays counted those repaints as frames (the 0054 achievement-toast problem again). Now it is only on the drawer
// while the renderer has pipeline work or holds a frame: it leaves when idle and is added back on the UI thread.
class ShaderNote final : public rex::ui::ImGuiDialog {
 public:
  ShaderNote(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* ui)
      : ImGuiDialog(drawer), ui_(ui), alive_(std::make_shared<bool>(true)) {
    if (ui_) {
      imgui_drawer()->RemoveDialog(this);
      attached_.store(false);  // a later progress update must be able to attach it again
    }
  }
  ~ShaderNote() override { *alive_ = false; }
  // Any thread: work is queued or a frame is held - be on the drawer.
  void Wake() {
    if (!ui_ || attached_.exchange(true)) return;
    if (!ui_->CallInUIThread([this, alive = alive_] {
      if (*alive) imgui_drawer()->AddDialog(this);  // no-op if already added
    })) attached_.store(false);
  }
  bool WantsContinuousPaint() const override { return true; }  // also before the first guest frame

 protected:
  void OnDraw(ImGuiIO& io) override {
    using clock = std::chrono::steady_clock;
    const clock::time_point now = clock::now();
    const PreparationProgress startup = StartupProgress();
    if (startup.phase != PreparationPhase::kInactive) {
      DrawStartupProgress(io, now, startup);
      return;
    }
    if (g_scan_active.load(std::memory_order_acquire)) {
      char text[128];
      const uint64_t total = g_scan_total.load(std::memory_order_relaxed);
      if (total)
        std::snprintf(text, sizeof(text), "Scanning shader packages %llu / %llu",
                      (unsigned long long)g_scan_done.load(std::memory_order_relaxed), (unsigned long long)total);
      else std::snprintf(text, sizeof(text), "Finding shader packages...");
      DrawSpinnerNote(io, now, text);
      return;
    }
    if (g_hold_active.load(std::memory_order_acquire)) {
      DrawHold(io, now);
      return;
    }
    // Prebuild v2 indicator: a load's package shaders compiling - the spinner, bottom right (the game may present
    // nothing for seconds meanwhile). Shown after 300 ms of work so a handful of shaders never flash it.
    const uint64_t pre_left = g_pre_left.load(std::memory_order_relaxed);
    const uint64_t pre_ever = g_pre_ever.load(std::memory_order_relaxed);
    if (!pre_left) {
      pre_base_ = pre_ever;
      pre_since_ = {};
    } else if (g_pre_loading.load(std::memory_order_relaxed)) {
      if (!pre_since_.time_since_epoch().count()) pre_since_ = now;
      if (now - pre_since_ >= std::chrono::milliseconds(300)) {
        const uint64_t total = std::max(pre_left, pre_ever > pre_base_ ? pre_ever - pre_base_ : pre_left);
        const int pct = int(std::min<uint64_t>(99, (total - pre_left) * 100 / total));
        char text[96];
        std::snprintf(text, sizeof(text), "Compiling shaders %d%%  (%llu left)", pct, (unsigned long long)pre_left);
        DrawSpinnerNote(io, now, text);
        return;
      }
    }
    const uint64_t left = g_jobs_left.load(std::memory_order_relaxed);
    const uint64_t ever = g_jobs_ever.load(std::memory_order_relaxed);
    if (!left && !(pre_left && g_pre_loading.load(std::memory_order_relaxed))) {
      batch_base_ = ever;
      busy_since_ = {};
      if (ui_) {  // idle: off the drawer until there is work again (safe during drawing - the SDK defers it)
        attached_.store(false);
        imgui_drawer()->RemoveDialog(this);
        // A worker may have published progress between the idle snapshot and detachment.
        if (StartupProgressActive() || g_scan_active.load() || g_hold_active.load() || g_jobs_left.load() ||
            (g_pre_left.load() && g_pre_loading.load())) Wake();
      }
      return;
    }
    if (!left) return;  // only prebuild leftovers during play: the spinner above is for loads
    if (!busy_since_.time_since_epoch().count()) busy_since_ = now;
    if (now - busy_since_ < std::chrono::seconds(1)) return;
    const uint64_t total = ever > batch_base_ ? ever - batch_base_ : left;
    const uint64_t done = total > left ? total - left : 0;
    const int pct = int(std::min<uint64_t>(99, total ? done * 100 / total : 0));
    char text[96];
    std::snprintf(text, sizeof(text), "Building shaders %d%%  (%llu left)", pct, (unsigned long long)left);
    const float scale = std::max(1.0f, io.DisplaySize.y / 1080.0f);
    ImGui::SetNextWindowBgAlpha(0.40f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 3.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float w = size.x * scale + 16.0f * scale, h = size.y * scale + 6.0f * scale;
    ImGui::SetNextWindowPos(ImVec2(12.0f * scale, io.DisplaySize.y - h - 12.0f * scale));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    if (ImGui::Begin("##gow2_shader_note", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoBringToFrontOnFocus)) {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale, at,
                                          IM_COL32(235, 235, 235, 255), text);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
  }

 private:
  void DrawStartupProgress(ImGuiIO& io, std::chrono::steady_clock::time_point now, const PreparationProgress& p) {
    if (io.DisplaySize.x < 80 || io.DisplaySize.y < 80) return;
    const float scale = std::clamp(std::min(io.DisplaySize.x / 1280.0f, io.DisplaySize.y / 720.0f), 0.75f, 2.0f);
    const float w = std::min(640.0f * scale, io.DisplaySize.x - 32.0f), h = 238.0f * scale;
    const ImVec2 at((io.DisplaySize.x - w) * 0.5f, (io.DisplaySize.y - h) * 0.5f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(8, 9, 12, 255));
    ImGui::SetNextWindowPos(at);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##cogbox_shader_startup", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(23, 25, 32, 255), 10.0f * scale);
      const float pad = 28.0f * scale, left = at.x + pad, width = w - pad * 2;
      auto text = [&](float y, float size, ImU32 color, const char* value) {
        dl->AddText(ImGui::GetFont(), size * scale, ImVec2(left, at.y + y * scale), color, value);
      };
      text(25, 23, IM_COL32(245, 245, 247, 255), "Preparing game shaders");
      const char* stage = p.phase == PreparationPhase::kFinding ? "Finding shader packages" :
                          p.phase == PreparationPhase::kScanning ? "Scanning game files" :
                          p.phase == PreparationPhase::kSaving ? "Saving compiled shaders" : "Compiling shaders";
      text(65, 17, IM_COL32(225, 225, 230, 255), stage);
      char detail[160];
      const int percent = int(std::clamp(p.Fraction() * 100.0, 0.0, 100.0));
      if (p.phase == PreparationPhase::kScanning)
        std::snprintf(detail, sizeof(detail), "%llu / %llu packages   (%d%%)",
                      (unsigned long long)std::min(p.packages_done, p.packages_total),
                      (unsigned long long)p.packages_total, percent);
      else if (p.phase == PreparationPhase::kCompiling)
        std::snprintf(detail, sizeof(detail), "%llu / %llu shaders   (%d%%)",
                      (unsigned long long)p.ShadersDone(), (unsigned long long)p.shaders_total, percent);
      else if (p.phase == PreparationPhase::kSaving)
        std::snprintf(detail, sizeof(detail), "%llu shaders prepared   (100%%)", (unsigned long long)p.shaders_total);
      else std::snprintf(detail, sizeof(detail), "Checking your installed game files...");
      text(95, 15, IM_COL32(173, 177, 190, 255), detail);
      const ImVec2 bar(left, at.y + 127.0f * scale);
      const float bar_h = 10.0f * scale;
      dl->AddRectFilled(bar, ImVec2(left + width, bar.y + bar_h), IM_COL32(47, 50, 61, 255), bar_h * 0.5f);
      if (p.Determinate()) {
        if (p.Fraction() > 0)
          dl->AddRectFilled(bar, ImVec2(left + width * float(p.Fraction()), bar.y + bar_h),
                            IM_COL32(211, 65, 66, 255), bar_h * 0.5f);
      } else {
        const float sweep = float(std::fmod(std::chrono::duration<double>(now.time_since_epoch()).count() * 0.5, 1.0));
        const float span = width * 0.2f;
        const float x = left + sweep * (width - span);
        dl->AddRectFilled(ImVec2(x, bar.y), ImVec2(x + span, bar.y + bar_h), IM_COL32(211, 65, 66, 255), bar_h * 0.5f);
      }
      if (p.phase == PreparationPhase::kScanning)
        std::snprintf(detail, sizeof(detail), "Shaders prepared so far: %llu / %llu",
                      (unsigned long long)p.ShadersDone(), (unsigned long long)p.shaders_total);
      else if (p.phase == PreparationPhase::kSaving)
        std::snprintf(detail, sizeof(detail), "Saving your cache for faster launches.");
      else std::snprintf(detail, sizeof(detail), "Cached shaders are reused automatically.");
      text(154, 14, IM_COL32(173, 177, 190, 255), detail);
      std::chrono::steady_clock::time_point started;
      { std::lock_guard<std::mutex> lock(g_startup_mutex); started = g_startup_started; }
      const uint64_t elapsed = uint64_t(std::max(0.0, std::chrono::duration<double>(now - started).count()));
      std::snprintf(detail, sizeof(detail), "Elapsed %llum %02llus", (unsigned long long)(elapsed / 60),
                    (unsigned long long)(elapsed % 60));
      text(194, 14, IM_COL32(132, 138, 153, 255), detail);
      const ImVec2 center(at.x + w - pad - 8.0f * scale, at.y + 77.0f * scale);
      const float angle = float(std::fmod(std::chrono::duration<double>(now.time_since_epoch()).count() * 5.0, 6.2831853));
      dl->PathArcTo(center, 8.0f * scale, angle, angle + 4.2f, 24);
      dl->PathStroke(IM_COL32(211, 65, 66, 255), 0, 2.0f * scale);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }
  // Patch 0067: the hold indicator - a spinning arc and "Loading shaders N%", bottom right, drawn every paint while
  // the renderer holds a frame (the presenter keeps painting its overlay while the game waits).
  void DrawHold(ImGuiIO& io, std::chrono::steady_clock::time_point now) {
    const uint32_t total = g_hold_total.load(std::memory_order_relaxed);
    const uint32_t left = g_hold_left.load(std::memory_order_relaxed);
    const int pct = total ? int(std::min<uint32_t>(99, (total - std::min(left, total)) * 100u / total)) : 0;
    char text[80];
    if (g_hold_compiling.load(std::memory_order_relaxed))  // prebuild v2: the opened packages' shaders, all of them
      std::snprintf(text, sizeof(text), "Compiling shaders %d%%  (%u left)", pct, left);
    else
      std::snprintf(text, sizeof(text), "Loading shaders %d%%", pct);
    DrawSpinnerNote(io, now, text);
  }
  // The spinning arc and a line of text, bottom right (the hold and the prebuild's compile indicator).
  void DrawSpinnerNote(ImGuiIO& io, std::chrono::steady_clock::time_point now, const char* text) {
    const float scale = std::max(1.0f, io.DisplaySize.y / 1080.0f);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float r = 9.0f * scale, pad = 10.0f * scale;
    const float w = r * 2.0f + pad * 3.0f + ts.x * scale, h = std::max(r * 2.0f, ts.y * scale) + pad * 2.0f;
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - w - 24.0f * scale, io.DisplaySize.y - h - 24.0f * scale));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    if (ImGui::Begin("##gow2_hold_note", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 at = ImGui::GetWindowPos();
      const ImVec2 c(at.x + pad + r, at.y + h * 0.5f);
      const double t = std::chrono::duration<double>(now.time_since_epoch()).count();
      const float a0 = float(std::fmod(t * 5.0, 6.283185307179586));  // ~0.8 turns a second
      dl->PathArcTo(c, r, a0, a0 + 4.2f, 24);
      dl->PathStroke(IM_COL32(235, 235, 235, 255), 0, 2.5f * scale);
      dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale,
                  ImVec2(at.x + pad * 2.0f + r * 2.0f, at.y + (h - ts.y * scale) * 0.5f), IM_COL32(235, 235, 235, 255),
                  text);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
  }
  uint64_t batch_base_ = 0;
  std::chrono::steady_clock::time_point busy_since_{};
  uint64_t pre_base_ = 0;                             // prebuild v2: the compile indicator's batch start
  std::chrono::steady_clock::time_point pre_since_{};
  rex::ui::WindowedAppContext* ui_ = nullptr;
  std::shared_ptr<bool> alive_;     // read and written on the UI thread only
  std::atomic<bool> attached_{true};  // as made: registered by the base until the constructor removes it
};

std::unique_ptr<ShaderNote> g_note;
std::atomic<ShaderNote*> g_note_live{nullptr};
rex::ui::WindowedAppContext* g_ui = nullptr;

}  // namespace

void XenosSetLoadHold(bool active, uint32_t pipelines_total, uint32_t pipelines_left, bool compiling) {  // patch 0067
  g_hold_compiling.store(compiling, std::memory_order_relaxed);
  g_hold_total.store(pipelines_total, std::memory_order_relaxed);
  g_hold_left.store(pipelines_left, std::memory_order_relaxed);
  g_hold_active.store(active, std::memory_order_release);
  if (active)
    if (ShaderNote* n = g_note_live.load(std::memory_order_acquire)) n->Wake();
}

void XenosSetShaderScanProgress(bool active, uint64_t scanned, uint64_t total) {
  {
    std::lock_guard<std::mutex> lock(g_startup_mutex);
    if (g_startup.phase != PreparationPhase::kInactive) {
      if (active) {
        g_startup.packages_done = scanned;
        g_startup.packages_total = std::max(total, scanned);
        g_startup.phase = total ? PreparationPhase::kScanning : PreparationPhase::kFinding;
      } else if (g_startup.phase == PreparationPhase::kScanning || g_startup.phase == PreparationPhase::kFinding) {
        g_startup.packages_done = g_startup.packages_total;
        g_startup.phase = PreparationPhase::kCompiling;
      }
    }
  }
  g_scan_done.store(scanned, std::memory_order_relaxed);
  g_scan_total.store(total, std::memory_order_relaxed);
  g_scan_active.store(active, std::memory_order_release);
  if (active)
    if (ShaderNote* n = g_note_live.load(std::memory_order_acquire)) n->Wake();
}

void XenosSetPrebuildProgress(uint64_t left, uint64_t ever_taken, bool loading) {  // prebuild v2 indicator
  {
    std::lock_guard<std::mutex> lock(g_startup_mutex);
    if (g_startup.phase != PreparationPhase::kInactive) {
      g_startup.shaders_total = std::max(g_startup.shaders_total, ever_taken);
      g_startup.shaders_left = g_startup.phase == PreparationPhase::kSaving ? 0 : left;
    }
  }
  g_pre_left.store(left, std::memory_order_relaxed);
  g_pre_ever.store(ever_taken, std::memory_order_relaxed);
  g_pre_loading.store(loading, std::memory_order_relaxed);
  if (left && loading)
    if (ShaderNote* n = g_note_live.load(std::memory_order_acquire)) n->Wake();
}

void XenosSetShaderStartupPhase(PreparationPhase phase) {
  {
    std::lock_guard<std::mutex> lock(g_startup_mutex);
    if (phase == PreparationPhase::kFinding) { g_startup = {}; g_startup_started = std::chrono::steady_clock::now(); }
    g_startup.phase = phase;
    if (phase == PreparationPhase::kSaving) g_startup.shaders_left = 0;
  }
  if (phase != PreparationPhase::kInactive)
    if (ShaderNote* n = g_note_live.load(std::memory_order_acquire)) n->Wake();
}

void XenosSetShaderJobs(uint64_t left, uint64_t ever_queued) {
  g_jobs_left.store(left, std::memory_order_relaxed);
  g_jobs_ever.store(ever_queued, std::memory_order_relaxed);
  if (left)
    if (ShaderNote* n = g_note_live.load(std::memory_order_acquire)) n->Wake();
}

// 0.2.1: the UI thread for the shader note (each game's OnCreateDialogs, before XenosCreateFpsBarIfRequested).
void XenosSetUiContext(rex::ui::WindowedAppContext* ui) { g_ui = ui; }

void XenosCreateFpsBarIfRequested(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && REXCVAR_GET(gow2_shader_note)) {  // patch 0062
    g_note = std::make_unique<ShaderNote>(drawer, g_ui);
    g_note_live.store(g_note.get(), std::memory_order_release);
    // Preparation may already be running when dialogs become available.
    if (StartupProgressActive() || g_scan_active.load() || g_hold_active.load() || g_jobs_left.load() ||
        (g_pre_left.load() && g_pre_loading.load())) g_note->Wake();
  }
  const std::string where = REXCVAR_GET(gow2_fps_bar);
  if (!drawer || (where != "top" && where != "bottom")) return;
  XenosEnsureObserverHub();  // the swap count (with the SDK's GPU plugin it comes from the hub's observer)
  g_bar = std::make_unique<FpsBar>(drawer, where == "top");
}

void XenosDestroyFpsBar() {
  g_bar.reset();
  g_note_live.store(nullptr, std::memory_order_release);
  g_note.reset();  // patch 0062
}
