// Layer 0 (game-free): COGBox's achievement popups - see xenos/achievement_popup.h. Our code (MIT).
#include <xenos/achievement_popup.h>
#include <xenos/achievement_popup_assets.h>
#include <xenos/achievements_data.h>

#include "popup_styles.h"

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/windowed_app_context.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

// The launcher (cogbox/src/gears_settings.cpp) looks for "COGBox achievement popups" in the exe to know a game takes
// these options: keep that text in cogbox_ach_style's description.
REXCVAR_DEFINE_STRING(cogbox_ach_style, "x360", "COGBox",
                      "COGBox achievement popups: x360, xbox or ps5");
REXCVAR_DEFINE_STRING(cogbox_ach_colour, "dark", "COGBox",
                      "Achievement popup colour (Xbox style): dark, red, green, blue, purple, orange, pink, teal, grey, gold");
REXCVAR_DEFINE_STRING(cogbox_ach_position, "default", "COGBox",
                      "Achievement popup position: default, top_left, top_centre, top_right, bottom_left, bottom_centre, bottom_right");
REXCVAR_DEFINE_BOOL(cogbox_ach_sound, true, "COGBox", "Play the achievement sound (assets\\sounds\\achievement.wav)");
REXCVAR_DEFINE_BOOL(cogbox_ach_hide, false, "COGBox", "Hide achievement popups (no popup, no sound; achievements still unlock)");
REXCVAR_DEFINE_BOOL(cogbox_ach_test_keys, false, "COGBox", "Test: F3 shows a sample achievement popup, F4 a sample Platinum (keys nothing else in the four games uses)");

namespace xenos {

namespace {
const char* StyleName(ach::PopupStyle s) {
  switch (s) {
    case ach::PopupStyle::Xbox: return "xbox";
    case ach::PopupStyle::Ps5: return "ps5";
    default: return "x360";
  }
}
}  // namespace

AchievementPopup::AchievementPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
                                   rex::Runtime* runtime, rex::ui::WindowedAppContext* ui)
    : AchievementNotificationDialog(drawer), runtime_(runtime), ui_(ui), icons_(immediate_drawer, runtime),
      alive_(std::make_shared<bool>(true)) {
  imgui_drawer()->RemoveDialog(this);  // the base registered it; nothing to show yet
  test_keys_ = REXCVAR_GET(cogbox_ach_test_keys);
  if (test_keys_) {
    rex::ui::RegisterBind("bind_cogbox_ach_sample", "F3", "COGBox: sample achievement popup", [this] { PushSample(false); });
    rex::ui::RegisterBind("bind_cogbox_ach_platinum", "F4", "COGBox: sample Platinum popup", [this] { PushSample(true); });
  }
}

AchievementPopup::~AchievementPopup() {
  *alive_ = false;
  if (test_keys_) {
    rex::ui::UnregisterBind("bind_cogbox_ach_sample");
    rex::ui::UnregisterBind("bind_cogbox_ach_platinum");
  }
}

void AchievementPopup::Push(const rex::system::AchievementEvent& event) {
  if (REXCVAR_GET(cogbox_ach_hide)) return;  // nothing queued: turning it off later shows no backlog
  bool completes = false;
  if (runtime_ && runtime_->kernel_state()) {
    auto& mgr = runtime_->kernel_state()->achievements();
    std::vector<std::pair<uint32_t, bool>> states;
    for (const auto& a : mgr.ListAchievements()) states.emplace_back(a.id, mgr.IsUnlocked(a.id));
    completes = ach::CompletesGame(states, event.achievement.id);
  }
  Entry e;
  e.info = event.achievement;
  e.platinum = completes;  // only used to queue the PS5 Platinum entry below
  Queue(std::move(e));
}

void AchievementPopup::Queue(Entry e) {
  const uint32_t title = runtime_ && runtime_->kernel_state() ? runtime_->kernel_state()->title_id() : 0;
  const bool completes = e.platinum;
  e.platinum = false;
  e.style = ach::ParseStyle(REXCVAR_GET(cogbox_ach_style));
  e.colour = ach::ParseColour(REXCVAR_GET(cogbox_ach_colour));
  e.position = ach::ParsePosition(REXCVAR_GET(cogbox_ach_position));
  e.sound = REXCVAR_GET(cogbox_ach_sound);
  e.grade = ach::GradeFor(title, e.info.id, e.info.gamerscore);
  const ach::Rarity rarity = ach::CurrentRarityProvider().Get(title, e.info.id);
  e.rare = ach::IsRare(rarity);
  e.rare_percent = rarity.percent;
  if (const ach::GameDef* g = ach::FindGame(title)) e.game_name = g->name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool ps5 = e.style == ach::PopupStyle::Ps5;
    Entry platinum = e;
    queue_.push_back(std::move(e));
    if (ps5 && completes) {
      platinum.platinum = true;
      platinum.grade = ach::TrophyGrade::Platinum;
      queue_.push_back(std::move(platinum));
    }
  }
  Attach();
}

void AchievementPopup::PushSample(bool completes_game) {
  if (REXCVAR_GET(cogbox_ach_hide)) return;
  Entry e;
  if (runtime_ && runtime_->kernel_state()) {
    const auto list = runtime_->kernel_state()->achievements().ListAchievements();
    if (!list.empty()) e.info = list.front();
  }
  if (e.info.label.empty()) {
    e.info.label = "Sample achievement";
    e.info.description = "A sample popup (F3)";
    e.info.gamerscore = 50;
  }
  e.platinum = completes_game;
  Queue(std::move(e));
}

void AchievementPopup::Attach() {
  if (!ui_) return;
  ui_->CallInUIThread([this, alive = alive_] {
    if (*alive) imgui_drawer()->AddDialog(this);  // no-op if already added
  });
}

void AchievementPopup::OnDraw(ImGuiIO& io) {
  const auto now = std::chrono::steady_clock::now();
  Entry e;
  float t = 0.f;
  const char* sound = nullptr;  // played after unlocking: file checks / PlaySound must not hold up Push on a game thread
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      Entry& front = queue_.front();
      if (!front.started) {
        front.started = now;
        if (front.sound) sound = StyleName(front.style);
      }
      t = std::chrono::duration<float>(now - *front.started).count();
      if (t >= ach::PopupDuration(front.style)) {
        queue_.pop_front();
        continue;
      }
      e = front;
      break;
    }
    if (queue_.empty()) {
      imgui_drawer()->RemoveDialog(this);  // safe during drawing (the SDK defers it)
      return;
    }
  }
  if (sound) XenosPlayAchievementSound(sound);

  // Part 4 player check: never below 80 % of the 1080p size (text at 720p was too small); the modern Xbox style is drawn
  // 25 % bigger than its 1080p design (its 17 / 16 px text was too small at any size). Margins follow the screen only.
  const float screen = std::clamp(io.DisplaySize.y / 1080.f, 0.8f, 2.f), margin = 48.f * screen;
  const float scale = screen * (e.style == ach::PopupStyle::Xbox ? 1.25f : 1.f);
  ImVec2 size;
  switch (e.style) {
    case ach::PopupStyle::Xbox: size = popup::SizeXbox(scale); break;
    case ach::PopupStyle::Ps5: size = popup::SizePs5(scale); break;
    default: size = popup::SizeX360(scale); break;
  }
  using P = ach::PopupPosition;
  const P pos = ach::ResolvePosition(e.style, e.position);
  const bool top = pos == P::TopLeft || pos == P::TopCentre || pos == P::TopRight;
  const bool left = pos == P::TopLeft || pos == P::BottomLeft, right = pos == P::TopRight || pos == P::BottomRight;
  popup::PopupDraw d;
  d.pos.x = left ? margin : right ? io.DisplaySize.x - margin - size.x : (io.DisplaySize.x - size.x) * 0.5f;
  d.pos.y = top ? margin : io.DisplaySize.y - margin - size.y;
  d.scale = scale;
  d.f = ach::PopupTimeline(e.style, t);
  d.name = e.info.label;
  d.description = e.info.description;
  d.gamerscore = e.info.gamerscore;
  d.grade = e.grade;
  d.rare = e.rare;
  d.colour = e.colour;
  if (rex::ui::ImmediateTexture* tex = icons_.GetIcon(e.info)) d.icon = reinterpret_cast<ImTextureID>(tex);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  switch (e.style) {
    case ach::PopupStyle::Xbox: {
      char rare[96];
      std::snprintf(rare, sizeof(rare), "Rare achievement unlocked - %.2f%%", e.rare_percent);
      d.title = e.rare ? rare : "Achievement unlocked!";
      popup::DrawXbox(dl, d);
      break;
    }
    case ach::PopupStyle::Ps5:
      d.title = e.platinum ? e.game_name : e.info.label;
      d.sub = e.platinum ? "Platinum trophy earned!" : "Trophy earned!";
      popup::DrawPs5(dl, d);
      break;
    default:
      d.title = "Achievement unlocked";
      popup::DrawX360(dl, d);
      break;
  }
}

}  // namespace xenos
