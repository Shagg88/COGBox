// Layer 0 (game-free): an achievement toast that only keeps the overlay drawer registered while a toast is showing.
//
// Patch 0054 (NVIDIA overlay read about twice our fps, [V] SDK source): the SDK creates its AchievementToastDialog at
// LaunchModule, and an ImGuiDrawer with any dialog registered asks the presenter for a UI repaint every UI tick - so
// the presenter repainted at the monitor's rate (180 Hz on Shagg's PC) on top of the game's own presents, all the
// time, for a toast that is almost never on screen. This one removes itself from the drawer right after it is made,
// adds itself back on the UI thread when an achievement arrives, and removes itself again once the toast has faded.
// Same toast, same look (it is the SDK's AchievementToastDialog); only when the drawer is attached changes.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/windowed_app_context.h>

namespace xenos {

class LazyAchievementToast final : public rex::ui::AchievementToastDialog {
 public:
  // Made on the UI thread (ReXApp::LaunchModule's deferred call), like the SDK's.
  LazyAchievementToast(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer, rex::Runtime* runtime,
                       rex::ui::WindowedAppContext* ui)
      : AchievementToastDialog(drawer, immediate_drawer, runtime), ui_(ui), alive_(std::make_shared<bool>(true)) {
    imgui_drawer()->RemoveDialog(this);  // the base registered it; nothing to show yet
  }
  ~LazyAchievementToast() override { *alive_ = false; }

  // Any thread (guest threads included), as the base.
  void Push(const rex::system::AchievementEvent& event) override {
    AchievementToastDialog::Push(event);
    // Toasts show one after another for kShowSeconds each: keep the drawer until the last one has gone.
    const int64_t now = Now(), show = int64_t(kShowSeconds * 1e9);
    int64_t until = visible_until_.load();
    while (!visible_until_.compare_exchange_weak(until, std::max(until, now) + show)) {
    }
    if (ui_) ui_->CallInUIThread([this, alive = alive_] {
      if (*alive) imgui_drawer()->AddDialog(this);  // no-op if already added
    });
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    AchievementToastDialog::OnDraw(io);
    if (Now() > visible_until_.load()) imgui_drawer()->RemoveDialog(this);  // safe during drawing (SDK defers it)
  }

 private:
  static constexpr double kShowSeconds = 5.0;  // the base shows each for 4.5 s; half a second to finish the fade
  static int64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  rex::ui::WindowedAppContext* ui_ = nullptr;
  std::shared_ptr<bool> alive_;  // read and written on the UI thread only
  std::atomic<int64_t> visible_until_{0};
};

}  // namespace xenos
