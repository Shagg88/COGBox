// Layer 0 (game-free): --gow2_fps_bar=top|bottom - a one-line frame-rate readout drawn by us (an ImGui window on the
// SDK's overlay layer, over the game's picture), with no change to the game: guest frames per second (swaps counted
// by the observer hub, averaged over half a second), the frame time that gives, and the slowest frame in that half
// second. For both games (shared layer 0); the game's own counter (--gow2_patch_show_fps) is a column and a patch.
// Our code (MIT).
#include <xenos/dev_tools.h>
#include <xenos/achievement_popup_assets.h>   // the translated notes' font
#include <xenos/ui_lang.h>   // --cogbox_ui_lang
#include "renderer/fg_shown_frames.h"  // frames on screen with frame generation

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/kernel/xam/net/online_startup.h>   // SDK patch 93 (Flippage 90): CurrentPing

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(gow2_fps_bar, "off", "Gears2",
                      "A one-line frame-rate bar drawn over the game, no game patch: off, top or bottom");
REXCVAR_DEFINE_BOOL(cogbox_show_ping, false, "Network",
                    "Show the ping to the host of the online match or party in the FPS bar (or a bar of its own when "
                    "the FPS bar is off); as host, the worst ping among your players (SDK patch 93)");
REXCVAR_DEFINE_BOOL(gow2_shader_note, true, "Gears2",
                    "Patch 0062: a small 'Building shaders N%' note in the bottom-left corner while the renderer's "
                    "pipeline workers have had work queued for more than a second (start-up backlog, new areas)");
// GitHub #121 (Codec95: the frame-rate target is off while the shader note or an achievement toast shows): nothing of
// ours is drawn over the game but the FPS bar the player asked for. Each note on the drawer used to make the presenter
// repaint at the monitor's rate (ImGuiDialog::WantsContinuousPaint), and those extra presents are what the frame cap /
// V-Sync pacing got out of step with; that is fixed below too, this switch is for players who want none of it.
REXCVAR_DEFINE_BOOL(gow2_hide_onscreen_notes, false, "Gears2",
                    "Hide the on-screen notes drawn over the game: the shader-building note, the shader-loading "
                    "indicator and achievement pop-ups (achievements still unlock). The FPS bar is its own option");

namespace {

class FpsBar final : public rex::ui::ImGuiDialog {
 public:
  FpsBar(rex::ui::ImGuiDrawer* drawer, bool top, bool fps, bool ping)
      : ImGuiDialog(drawer), top_(top), show_fps_(fps), show_ping_(ping) {}
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
    // Shagg 2026-11-10: with frame generation the screen gets more images than the game draws. The frame-generation
    // back ends count what reached the screen (renderer/fg_shown_frames.h); while generation ran in this half second
    // the bar leads with that rate and keeps the drawn rate (and its frame time, the one input follows) beside it.
    const uint64_t shown = g_xenos_fg_shown.load(std::memory_order_relaxed);
    const uint64_t generating = g_xenos_fg_generating.load(std::memory_order_relaxed);
    if (!window_start_.time_since_epoch().count())
      window_start_ = now, window_swaps_ = swaps, window_shown_ = shown, window_generating_ = generating;
    const double span = std::chrono::duration<double>(now - window_start_).count();
    if (span >= 0.5) {
      fps_ = double(swaps - window_swaps_) / span;
      shown_fps_ = generating != window_generating_ ? double(shown - window_shown_) / span : 0.0;
      shown_worst_ = worst_ms_;
      worst_ms_ = 0.0;
      window_start_ = now;
      window_swaps_ = swaps;
      window_shown_ = shown;
      window_generating_ = generating;
      if (show_ping_) ping_ = rex::kernel::xam::net::CurrentPing();   // twice a second is plenty
    }
    char text[160];
    if (!show_fps_)
      text[0] = 0;
    else if (fps_ > 0.5 && shown_fps_ > fps_ + 0.5)
      std::snprintf(text, sizeof(text), "%.0f FPS (frame gen; %.0f drawn)     %.1f ms     slowest %.1f ms", shown_fps_,
                    fps_, 1000.0 / fps_, shown_worst_);
    else if (fps_ > 0.5)
      std::snprintf(text, sizeof(text), "%.0f FPS     %.1f ms     slowest %.1f ms", fps_, 1000.0 / fps_, shown_worst_);
    else
      std::snprintf(text, sizeof(text), "-- FPS");
    if (show_ping_) {   // SDK patch 93: the round trip to the host; as host, the worst among the players
      using PV = rex::kernel::xam::net::PingView;
      char ping[64];
      if (ping_.kind == PV::Hosting && ping_.ms >= 0)
        std::snprintf(ping, sizeof(ping), "Ping %d ms (worst of %d)", ping_.ms, ping_.players);
      else if (ping_.kind == PV::Hosting)
        std::snprintf(ping, sizeof(ping), "Ping -- (hosting)");
      else if (ping_.kind == PV::ToHost && ping_.ms >= 0)
        std::snprintf(ping, sizeof(ping), "Ping %d ms", ping_.ms);
      else
        std::snprintf(ping, sizeof(ping), "Ping --");
      const size_t used = std::strlen(text);
      std::snprintf(text + used, sizeof(text) - used, "%s%s", used ? "     " : "", ping);
    }
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
  bool show_fps_, show_ping_;
  rex::kernel::xam::net::PingView ping_;
  double fps_ = 0.0, shown_fps_ = 0.0, worst_ms_ = 0.0, shown_worst_ = 0.0;
  uint64_t last_swaps_ = 0, window_swaps_ = 0, window_shown_ = 0, window_generating_ = 0;
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
// --cogbox_ui_lang: a translated note is drawn with the font that has its glyphs (Inter / the PC's own, CJK merged)
struct UiLangFont {
  bool pushed = false;
  UiLangFont() {
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
    if (XenosUiLangActive())
      if (ImFont* f = XenosAchievementFont(false)) ImGui::PushFont(f, 0.0f), pushed = true;
#endif
  }
  ~UiLangFont() {
    if (pushed) ImGui::PopFont();
  }
};

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
  // GitHub #121: the note is drawn with each game frame, so it needs no repaints of its own - only the hold indicator
  // does (the game waits behind the held frame and presents nothing, and the arc must still turn). Before, the note
  // kept the presenter repainting at the monitor's rate for as long as shaders were building (minutes in a new area),
  // and the frame cap / V-Sync pacing counted those presents: the frame-rate target was off while it showed.
  bool WantsContinuousPaint() const override { return g_hold_active.load(std::memory_order_acquire); }
  // Any thread: work is queued or a frame is held - be on the drawer.
  void Wake() {
    if (!ui_ || attached_.exchange(true)) return;
    ui_->CallInUIThread([this, alive = alive_] {
      if (*alive) imgui_drawer()->AddDialog(this);  // no-op if already added
    });
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const UiLangFont font;
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
    char text[192];
    if (XenosUiLangActive())  // the count alone: a lone "left" has no safe translation key
      std::snprintf(text, sizeof(text), "%s %d%%  (%llu)", XenosTr("Building shaders"), pct, (unsigned long long)left);
    else
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
    char text[160];
    std::snprintf(text, sizeof(text), "%s %d%%", XenosTr("Loading shaders"), pct);
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
// 0.4.9 review: XenosRefreshOverlays destroys the note on the UI thread while the render / compile threads may be
// inside Wake() through g_note_live. Wake callers hold this lock while they use the pointer, and XenosDestroyFpsBar
// clears the pointer under it before freeing the note, so no thread can still be using it (Wake only posts a task).
std::mutex g_note_mutex;
rex::ui::WindowedAppContext* g_ui = nullptr;

}  // namespace

void XenosSetLoadHold(bool active, uint32_t pipelines_total, uint32_t pipelines_left) {  // patch 0067
  g_hold_total.store(pipelines_total, std::memory_order_relaxed);
  g_hold_left.store(pipelines_left, std::memory_order_relaxed);
  g_hold_active.store(active, std::memory_order_release);
  if (active) {
    std::lock_guard<std::mutex> lock(g_note_mutex);
    if (ShaderNote* n = g_note_live.load(std::memory_order_acquire)) n->Wake();
  }
}

void XenosSetShaderJobs(uint64_t left, uint64_t ever_queued) {
  g_jobs_left.store(left, std::memory_order_relaxed);
  g_jobs_ever.store(ever_queued, std::memory_order_relaxed);
  if (left) {
    std::lock_guard<std::mutex> lock(g_note_mutex);
    if (ShaderNote* n = g_note_live.load(std::memory_order_acquire)) n->Wake();
  }
}

// 0.2.1: the UI thread for the shader note (each game's OnCreateDialogs, before XenosCreateFpsBarIfRequested).
void XenosSetUiContext(rex::ui::WindowedAppContext* ui) { g_ui = ui; }

bool XenosOnScreenNotesHidden() { return REXCVAR_GET(gow2_hide_onscreen_notes); }  // GitHub #121

static rex::ui::ImGuiDrawer* g_overlay_drawer = nullptr;   // 0.4.9: kept for XenosRefreshOverlays

void XenosCreateFpsBarIfRequested(rex::ui::ImGuiDrawer* drawer) {
  g_overlay_drawer = drawer;
  if (drawer && REXCVAR_GET(gow2_shader_note) && !XenosOnScreenNotesHidden()) {  // patch 0062, GitHub #121
    g_note = std::make_unique<ShaderNote>(drawer, g_ui);
    g_note_live.store(g_note.get(), std::memory_order_release);
  }
  const std::string where = REXCVAR_GET(gow2_fps_bar);
  const bool fps = where == "top" || where == "bottom", ping = REXCVAR_GET(cogbox_show_ping);
  if (!drawer || (!fps && !ping)) return;   // ping alone: the same bar at the top, with just the ping
  XenosEnsureObserverHub();  // the swap count (with the SDK's GPU plugin it comes from the hub's observer)
  g_bar = std::make_unique<FpsBar>(drawer, where != "bottom", fps, ping);
}

// Shagg 2026-11-10 10:41 / 10:47: quitting with frame generation crashed (Gears 3, crash-20261110-104037: access
// violation in sl.dlss_g.dll -> NVIDIA NGX during Windows' DLL unload), while Flippage's main-menu Exit Game (which ends
// the process with TerminateProcess) quits cleanly. SDK patch 73 made the SDK's own quit do the same, but it is in
// rex_app.cpp, which every game exe compiles from the INSTALLED SDK: the runtime-only rebuilds (fix050 / fix051) never
// reached the exes, which still call std::_Exit - and ExitProcess then unloads Streamline into an NGX already gone.
// Each game app overrides the window's OnClosing with this: the SDK's own steps (end the title, flush the log), then the
// end of the process without any DLL unload. (The SDK's shutting_down_ flag only stops the guest thread asking for a
// quit; the process is gone by then.)
void XenosQuitWithoutDllUnload(rex::Runtime* runtime) {
  REXLOG_INFO("Window closing, shutting down...");
  if (runtime && runtime->kernel_state()) runtime->kernel_state()->TerminateTitle();
  REXLOG_INFO("Title terminated; ending the process without unloading DLLs (frame generation safe)");
  rex::FlushLogging();
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), 0);
#endif
  std::_Exit(0);
}

void XenosDestroyFpsBar() {
  g_overlay_drawer = nullptr;
  g_bar.reset();
  {
    std::lock_guard<std::mutex> lock(g_note_mutex);  // waits for a Wake() in progress on another thread
    g_note_live.store(nullptr, std::memory_order_release);
  }
  g_note.reset();  // patch 0062
}

// 0.4.9 live settings: the in-game menu changed --gow2_fps_bar or --gow2_hide_onscreen_notes - the bar and the shader
// note are made again from the cvars. Deferred: the menu calls this from its own draw, while the drawer walks its dialogs.
void XenosRefreshOverlays() {
  if (!g_ui) return;
  g_ui->CallInUIThreadDeferred([] {
    rex::ui::ImGuiDrawer* drawer = g_overlay_drawer;
    if (!drawer) return;   // not made yet, or shutting down
    XenosDestroyFpsBar();
    XenosCreateFpsBarIfRequested(drawer);
    REXLOG_INFO("gow2_fps_bar: overlays made again (fps bar '{}', notes {})", REXCVAR_GET(gow2_fps_bar),
                XenosOnScreenNotesHidden() ? "hidden" : "shown");
  });
}
