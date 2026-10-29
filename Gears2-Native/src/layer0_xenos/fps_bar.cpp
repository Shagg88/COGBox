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
#include <string>

REXCVAR_DEFINE_STRING(gow2_fps_bar, "off", "Gears2",
                      "A one-line frame-rate bar drawn over the game, no game patch: off, top or bottom");
REXCVAR_DEFINE_BOOL(gow2_shader_note, true, "Gears2",
                    "Patch 0062: a small 'Building shaders N%' note in the bottom-left corner while the renderer's "
                    "pipeline workers have had work queued for more than a second (start-up backlog, new areas)");

namespace {

class FpsBar final : public rex::ui::ImGuiDialog {
 public:
  FpsBar(rex::ui::ImGuiDrawer* drawer, bool top) : ImGuiDialog(drawer), top_(top) {}
  // 0.2.1 (patch 42): the bar is redrawn with each game frame anyway; asking for more made NVIDIA / RTSS count the
  // presenter's extra repaints (monitor rate) instead of the game's frames.
  bool WantsContinuousPaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
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

// 0.2.1 (GitHub #18 / #23: NVIDIA and RTSS read about 3x the game's fps): an ImGuiDrawer with any dialog registered
// asks the presenter for a repaint every UI tick, so this note - registered all the time though it draws nothing
// while no shaders are building - kept the presenter repainting at the monitor's rate (180 Hz on Shagg's PC), and the
// overlays counted those repaints as frames (the 0054 achievement-toast problem again). Now it is only on the drawer
// while the renderer has pipeline work or holds a frame: it leaves when idle and is added back on the UI thread.
class ShaderNote final : public rex::ui::ImGuiDialog {
 public:
  ShaderNote(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* ui)
      : ImGuiDialog(drawer), ui_(ui), alive_(std::make_shared<bool>(true)) {
    // PR #43 (legoliamneeson): also mark it off the drawer here - with attached_ still true, Wake() returned at once
    // and the note (and the "Loading shaders" hold indicator) never came back after this removal.
    if (ui_) {
      imgui_drawer()->RemoveDialog(this);  // nothing to show yet (without the UI thread it stays, as before)
      attached_.store(false);
    }
  }
  ~ShaderNote() override { *alive_ = false; }
  // Any thread: work is queued or a frame is held - be on the drawer.
  void Wake() {
    if (!ui_ || attached_.exchange(true)) return;
    ui_->CallInUIThread([this, alive = alive_] {
      if (*alive) imgui_drawer()->AddDialog(this);  // no-op if already added
    });
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    using clock = std::chrono::steady_clock;
    const clock::time_point now = clock::now();
    if (g_hold_active.load(std::memory_order_acquire)) {
      DrawHold(io, now);
      return;
    }
    const uint64_t left = g_jobs_left.load(std::memory_order_relaxed);
    const uint64_t ever = g_jobs_ever.load(std::memory_order_relaxed);
    if (!left) {
      batch_base_ = ever;
      busy_since_ = {};
      if (ui_) {  // idle: off the drawer until there is work again (safe during drawing - the SDK defers it)
        attached_.store(false);
        imgui_drawer()->RemoveDialog(this);
        // PR #43: work (or a hold) that arrived while leaving saw attached_ still true and did not wake us: re-add.
        if (g_jobs_left.load(std::memory_order_relaxed) || g_hold_active.load(std::memory_order_acquire)) Wake();
      }
      return;
    }
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
  // Patch 0067: the hold indicator - a spinning arc and "Loading shaders N%", bottom right, drawn every paint while
  // the renderer holds a frame (the presenter keeps painting its overlay while the game waits).
  void DrawHold(ImGuiIO& io, std::chrono::steady_clock::time_point now) {
    const uint32_t total = g_hold_total.load(std::memory_order_relaxed);
    const uint32_t left = g_hold_left.load(std::memory_order_relaxed);
    const int pct = total ? int(std::min<uint32_t>(99, (total - std::min(left, total)) * 100u / total)) : 0;
    char text[64];
    std::snprintf(text, sizeof(text), "Loading shaders %d%%", pct);
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
  rex::ui::WindowedAppContext* ui_ = nullptr;
  std::shared_ptr<bool> alive_;     // read and written on the UI thread only
  std::atomic<bool> attached_{true};  // as made: registered by the base until the constructor removes it
};

std::unique_ptr<ShaderNote> g_note;
std::atomic<ShaderNote*> g_note_live{nullptr};
rex::ui::WindowedAppContext* g_ui = nullptr;

}  // namespace

void XenosSetLoadHold(bool active, uint32_t pipelines_total, uint32_t pipelines_left) {  // patch 0067
  g_hold_total.store(pipelines_total, std::memory_order_relaxed);
  g_hold_left.store(pipelines_left, std::memory_order_relaxed);
  g_hold_active.store(active, std::memory_order_release);
  if (active)
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
