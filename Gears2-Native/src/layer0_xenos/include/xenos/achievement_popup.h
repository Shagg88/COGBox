// Layer 0 (game-free): COGBox's achievement popups (docs/achievements/README.md):
// Xbox 360, modern Xbox and PS5 trophy styles, animated from xenos::ach::PopupTimeline, with a replaceable sound.
// Replaces the SDK's toast. Like the old LazyAchievementToast it is attached to the ImGui drawer only while a popup
// shows, and it is drawn with the game's own frames (no repaints of its own). Our code (MIT).
#pragma once

#include <xenos/achievements_popup_logic.h>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <rex/cvar.h>
#include <rex/system/achievement_manager.h>
#include <rex/ui/overlay/achievement_icon_cache.h>
#include <rex/ui/overlay/achievement_notification.h>

REXCVAR_DECLARE(std::string, cogbox_ach_style);
REXCVAR_DECLARE(std::string, cogbox_ach_colour);
REXCVAR_DECLARE(std::string, cogbox_ach_position);
REXCVAR_DECLARE(bool, cogbox_ach_sound);
REXCVAR_DECLARE(bool, cogbox_ach_hide);
REXCVAR_DECLARE(bool, cogbox_ach_test_keys);
// F-8: the in-game menu sets the popup cvars on the UI thread while an unlock on a guest thread reads them (the style /
// colour / position are strings). Both hold this lock: the menu around SetFlagByName, the popup while it reads them.
std::mutex& XenosAchievementSettingsMutex();

namespace rex {
class Runtime;
}
namespace rex::ui {
class ImmediateDrawer;
class WindowedAppContext;
}  // namespace rex::ui

namespace xenos {

class AchievementPopup final : public rex::ui::AchievementNotificationDialog {
 public:
  // Made on the UI thread (ReXApp's CreateAchievementNotificationDialog).
  AchievementPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer, rex::Runtime* runtime,
                   rex::ui::WindowedAppContext* ui);
  ~AchievementPopup() override;
  bool WantsContinuousPaint() const override { return false; }  // drawn with each game frame

  // Any thread (guest threads included).
  void Push(const rex::system::AchievementEvent& event) override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct Entry {
    ach::PopupStyle style = ach::PopupStyle::X360;
    ach::ColourPreset colour{};
    ach::PopupPosition position = ach::PopupPosition::Default;
    rex::system::AchievementInfo info;
    ach::TrophyGrade grade = ach::TrophyGrade::Bronze;
    bool rare = false;
    float rare_percent = 0;
    bool platinum = false;
    bool sound = true;
    std::string game_name;
    std::optional<std::chrono::steady_clock::time_point> started;
  };
  void Queue(Entry entry);
  void PushSample(bool completes_game);
  void Attach();

  rex::Runtime* runtime_ = nullptr;
  rex::ui::WindowedAppContext* ui_ = nullptr;
  rex::ui::AchievementIconCache icons_;
  std::shared_ptr<bool> alive_;  // read and written on the UI thread only
  std::mutex mutex_;
  std::deque<Entry> queue_;
  bool test_keys_ = false;
};

}  // namespace xenos
