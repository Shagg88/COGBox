// COGBox social step 5: the in-game social notices - an invite, a friend request, a friend coming online or starting a
// game, and the hosting ban - in their own panel at the middle of the right screen edge. They have nothing to do with
// the achievement pop-ups and none of their settings. Events come from XenosSocialTakeEvents; nothing is shown while
// COGBox's menu is open (they wait).
#include <xenos/social_popup.h>

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <imgui.h>

#include <rex/kernel/xam/net/host_ban.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <xenos/achievement_popup_assets.h>   // the UI font
#include <xenos/dev_tools.h>          // XenosInGameMenuIsOpen
#include <xenos/social_ingame.h>
#include <xenos/ui_lang.h>

namespace {
constexpr float kSlideSeconds = 0.3f, kShowSeconds = 5.f, kLongShowSeconds = 8.f;

struct Entry {
  std::string line1, line2;
  float seconds = kShowSeconds;
  std::optional<std::chrono::steady_clock::time_point> started;
};

std::string GameName(const std::string& title_id) {
  const char* n = game_name_for_title(title_id);
  return n && *n ? XenosTr(n) : std::string(XenosTr("a game"));
}

// How far the panel is out, 0 (off the edge) to 1, eased.
float SlideAmount(float t, float seconds) {
  const float in = std::min(t, seconds - t) / kSlideSeconds;
  const float x = std::clamp(in, 0.f, 1.f);
  return 1.f - (1.f - x) * (1.f - x) * (1.f - x);
}

class SocialPopup final : public rex::ui::ImGuiDialog {
 public:
  SocialPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) : ImGuiDialog(drawer), window_(window) {
    drawer->RemoveDialog(this);   // attached only while something is showing
  }
  bool WantsContinuousPaint() const override { return false; }   // drawn with each game frame

  // Any thread.
  void Push(const SocialEvent& ev) {
    Entry e;
    switch (ev.kind) {
      case SocialEventKind::Invite:
        e.line1 = ev.tag + " " + XenosTr("invited you to") + " " + GameName(ev.title_id);
        e.line2 = XenosTr("Press F8 or Guide to accept");
        break;
      case SocialEventKind::Request:
        e.line1 = ev.tag;
        e.line2 = XenosTr("wants to be your friend");
        break;
      case SocialEventKind::CameOnline:
        e.line1 = ev.tag;
        e.line2 = XenosTr("is online");
        break;
      case SocialEventKind::RequestAccepted:
        e.line1 = ev.tag;
        e.line2 = XenosTr("accepted your friend request");
        break;
      case SocialEventKind::StartedPlaying:
        e.line1 = ev.tag;
        e.line2 = std::string(XenosTr("is playing")) + " " + GameName(ev.title_id);
        break;
    }
    Queue(std::move(e));
  }

  void PushHostBan(int minutes) {
    Entry e;
    e.line1 = std::string(XenosTr("Hosting locked for")) + " " + std::to_string(minutes) + " " +
              XenosTr(minutes == 1 ? "minute" : "minutes");
    e.line2 = XenosTr("You left too many games in progress. Searching for a match to join.");
    e.seconds = kLongShowSeconds;
    Queue(std::move(e));
  }

  ~SocialPopup() override { *alive_ = false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (XenosInGameMenuIsOpen()) return;   // they wait for the menu to close
    const auto now = std::chrono::steady_clock::now();
    Entry e;
    float t = 0.f;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      while (!queue_.empty()) {
        Entry& front = queue_.front();
        if (!front.started) front.started = now;
        t = std::chrono::duration<float>(now - *front.started).count();
        if (t >= front.seconds) {
          queue_.pop_front();
          continue;
        }
        e = front;
        break;
      }
      if (queue_.empty()) {
        imgui_drawer()->RemoveDialog(this);   // safe during drawing (the SDK defers it)
        return;
      }
    }
    const float s = std::clamp(io.DisplaySize.y / 1080.f, 0.8f, 2.f);
    const float width = std::min(460.f * s, io.DisplaySize.x - 48.f * s), pad = 20.f * s, gap = 6.f * s;
    const float size1 = 25.f * s, size2 = 20.f * s, wrap = width - 2.f * pad;
    ImFont* font1 = XenosAchievementFontFor(true, size1);
    ImFont* font2 = XenosAchievementFontFor(false, size2);
    if (!font1) font1 = ImGui::GetFont();
    if (!font2) font2 = ImGui::GetFont();
    const float h1 = font1->CalcTextSizeA(size1, FLT_MAX, wrap, e.line1.c_str()).y;
    const float h2 = font2->CalcTextSizeA(size2, FLT_MAX, wrap, e.line2.c_str()).y;
    const float height = 2.f * pad + h1 + gap + h2;
    const ImVec2 p0(io.DisplaySize.x - (width + 24.f * s) * SlideAmount(t, e.seconds), (io.DisplaySize.y - height) * 0.5f);
    const ImVec2 p1(p0.x + width, p0.y + height);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 185), 14.f * s);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 45), 14.f * s, 0, std::max(1.f, s));
    dl->AddText(font1, size1, ImVec2(p0.x + pad, p0.y + pad), IM_COL32(255, 255, 255, 255), e.line1.c_str(), nullptr, wrap);
    dl->AddText(font2, size2, ImVec2(p0.x + pad, p0.y + pad + h1 + gap), IM_COL32(215, 215, 215, 255), e.line2.c_str(),
                nullptr, wrap);
  }

 private:
  void Queue(Entry e) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(e));
    }
    if (window_) {
      window_->app_context().CallInUIThread([this, alive = alive_] {
        if (*alive) imgui_drawer()->AddDialog(this);   // no-op if already added
      });
    }
  }

  rex::ui::Window* window_ = nullptr;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::mutex mutex_;
  std::deque<Entry> queue_;
};

std::unique_ptr<SocialPopup> g_popup;
std::chrono::steady_clock::time_point g_last_pump{};
}  // namespace

void XenosInstallSocialPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) {
  if (!drawer || !window || g_popup) return;
  g_popup = std::make_unique<SocialPopup>(drawer, window);
}

void XenosShutdownSocialPopup() { g_popup.reset(); }

void XenosPumpSocialPopup() {
  if (!g_popup || !XenosSocialAvailable()) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_pump < std::chrono::seconds(1)) return;
  g_last_pump = now;
  for (const SocialEvent& ev : XenosSocialTakeEvents()) g_popup->Push(ev);
  if (const int minutes = rex::kernel::xam::net::TakeHostBanNotice()) g_popup->PushHostBan(minutes);
}
