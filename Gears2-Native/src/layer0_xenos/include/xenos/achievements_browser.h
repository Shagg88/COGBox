// Layer 0 (game-free): the Achievements page of COGBox's in-game menu (docs/achievements/README.md):
// games down the left with their progress, the chosen game's (or All's) achievements on the right, sort,
// Show filter and the secret-reveal question. What it shows comes from achievements_browser_model.h; this loads the data
// and icons and draws it inside the menu's window. UI thread only, except the unlock callback. Our code (MIT).
#pragma once

#include <xenos/achievements_browser_model.h>

#include <rex/ui/overlay/achievement_icon_cache.h>

#include <imgui.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rex {
class Runtime;
}
namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace xenos {

class AchievementsBrowser {
 public:
  AchievementsBrowser(rex::ui::ImmediateDrawer* immediate, rex::Runtime* runtime);  // registers an unlock callback
  ~AchievementsBrowser();                                                            // unregisters it
  AchievementsBrowser(const AchievementsBrowser&) = delete;
  AchievementsBrowser& operator=(const AchievementsBrowser&) = delete;

  // Reads every game's files again (call when the page is shown).
  void Reload();
  // Draws the page into the current window, `height` tall, at the menu's scale. Returns the selected row's full name
  // and text, for the menu's description line.
  std::string Draw(float scale, float height);
  void Previous();  // LB
  void Next();      // RB
  void CycleSort();  // X
  void CycleShow();  // Y
  bool RevealOpen() const { return reveal_open_; }  // the reveal question is showing (B means No there)

 private:
  // The menu is set up before the runtime exists (OnCreateDialogs comes before ConstructRuntime): the runtime is
  // looked up when needed, and the running title, its icon cache and the unlock callback are attached once the game's
  // module is loaded. Retried on every Reload until then.
  rex::Runtime* Rt() const;
  void Attach();
  void RebuildRows();
  void Select(size_t index);
  ImTextureID Icon(const ach::Achievement& a);  // 0 = none
  void DrawSidebar(float s, float height);
  void DrawMain(float s, float height, std::string* tip);
  void DrawReveal(float s);

  rex::ui::ImmediateDrawer* immediate_ = nullptr;
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<rex::ui::AchievementIconCache> running_icons_;  // made by Attach
  std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<rex::ui::ImmediateTexture>> icons_;  // null = failed
  uint64_t unlock_listener_ = 0;
  std::shared_ptr<std::atomic<bool>> stale_;

  std::filesystem::path dir_;
  uint32_t running_title_ = 0;
  std::vector<ach::GameAchievements> games_;
  std::vector<ach::BrowserEntry> sidebar_;
  std::vector<ach::BrowserRow> rows_;
  size_t sel_ = 0;
  bool sel_all_ = false;
  int sel_game_ = -2;  // -2: not chosen yet (the running game)
  ach::SortOrder sort_ = ach::SortOrder::kUnlockTime;
  ach::ShowFilter show_ = ach::ShowFilter::kAll;
  bool scroll_top_ = false;

  std::pair<uint32_t, uint32_t> reveal_{0, 0};
  bool want_reveal_ = false, reveal_open_ = false, reveal_failed_ = false;
};

}  // namespace xenos
