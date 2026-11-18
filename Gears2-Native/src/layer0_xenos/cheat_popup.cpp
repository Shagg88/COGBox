// Layer 0 (game-free): GitHub #359 - the cheat key notice (see xenos/cheat_popup.h). Our code (MIT).
#include <xenos/cheat_popup.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <xenos/achievement_popup_assets.h>    // the UI fonts (no achievement art or sound is used)
#include <xenos/achievements_popup_logic.h>    // the position names, shared with the achievement popups
#include <xenos/dev_tools.h>                   // XenosInGameMenuIsOpen
#include <xenos/ui_lang.h>

REXCVAR_DEFINE_BOOL(cogbox_cheat_popups, true, "COGBox",
                    "GitHub #359: a short notice when a cheat key is pressed (God mode ON / OFF, World Frozen, ...)");
REXCVAR_DEFINE_STRING(cogbox_cheat_popup_position, "top_left", "COGBox",
                      "Cheat key notice position: top_left, top_centre, top_right, bottom_left, bottom_centre, bottom_right "
                      "(default = top_left, away from the achievement popups)");

namespace {
constexpr float kShowSeconds = 2.0f, kInSeconds = 0.18f, kOutSeconds = 0.3f;

struct Entry {
  std::string label, state;
  XenosCheatTone tone = XenosCheatTone::Value;
  XenosCheatIcon icon = XenosCheatIcon::Generic;
  std::optional<std::chrono::steady_clock::time_point> started;
};

struct Rgb {
  int r, g, b;
};
Rgb Accent(XenosCheatTone tone) {   // the mock-up's colours
  switch (tone) {
    case XenosCheatTone::On: return {0x1f, 0x8f, 0x6a};
    case XenosCheatTone::Off: return {0x4b, 0x50, 0x58};
    case XenosCheatTone::Refused: return {0xb5, 0x53, 0x2c};
    default: return {0x2f, 0x6f, 0xd0};
  }
}
ImU32 Col(Rgb c, float a) { return IM_COL32(c.r, c.g, c.b, int(255.f * std::clamp(a, 0.f, 1.f))); }
Rgb Lighter(Rgb c) { return {c.r + (255 - c.r) * 2 / 5, c.g + (255 - c.g) * 2 / 5, c.b + (255 - c.b) * 2 / 5}; }

// A white icon in a 24-unit box (as an SVG viewBox) centred on c, `box` pixels wide. Shapes only, no art.
void DrawIcon(ImDrawList* dl, ImVec2 c, float box, XenosCheatIcon icon, float a) {
  const ImU32 w = IM_COL32(255, 255, 255, int(255.f * a));
  const float k = box / 24.f, line = std::max(1.f, 2.f * k);
  auto P = [&](float x, float y) { return ImVec2(c.x + (x - 12.f) * k, c.y + (y - 12.f) * k); };
  switch (icon) {
    case XenosCheatIcon::Shield: {
      const ImVec2 pts[] = {P(12, 2), P(20, 5), P(20, 11), P(17.5f, 17), P(12, 22), P(6.5f, 17), P(4, 11), P(4, 5)};
      dl->AddConvexPolyFilled(pts, 8, w);
      break;
    }
    case XenosCheatIcon::Stopwatch:
      dl->AddCircle(P(12, 13.5f), 7.5f * k, w, 24, line);
      dl->AddRectFilled(P(10, 2.5f), P(14, 4.5f), w, k);
      dl->AddLine(P(12, 13.5f), P(12, 9), w, line);
      break;
    case XenosCheatIcon::Weapon:
      dl->AddRectFilled(P(3, 10), P(17, 14), w, k);
      dl->AddRectFilled(P(15, 8), P(21, 16), w, k);
      break;
    case XenosCheatIcon::Eye:
      dl->AddBezierQuadratic(P(2, 12), P(12, 2), P(22, 12), w, line, 16);
      dl->AddBezierQuadratic(P(2, 12), P(12, 22), P(22, 12), w, line, 16);
      dl->AddCircleFilled(P(12, 12), 3.2f * k, w, 16);
      break;
    case XenosCheatIcon::Camera:
      dl->AddRectFilled(P(3, 7), P(21, 19), w, 2.f * k);
      dl->AddRectFilled(P(8, 4.5f), P(14, 7.5f), w, k);
      dl->AddCircleFilled(P(12, 13), 4.f * k, IM_COL32(0, 0, 0, int(140.f * a)), 16);
      break;
    case XenosCheatIcon::Freeze:
      for (int i = 0; i < 3; i++) {
        const float an = 3.14159265f / 2.f + float(i) * 3.14159265f / 3.f, dx = std::cos(an) * 9.f, dy = std::sin(an) * 9.f;
        dl->AddLine(P(12 - dx, 12 - dy), P(12 + dx, 12 + dy), w, line);
      }
      break;
    case XenosCheatIcon::Fly: {
      const ImVec2 tri[] = {P(12, 3), P(19, 11), P(5, 11)};
      dl->AddConvexPolyFilled(tri, 3, w);
      dl->AddRectFilled(P(10, 10), P(14, 21), w);
      break;
    }
    case XenosCheatIcon::Refused:
      dl->AddCircle(P(12, 12), 8.f * k, w, 24, line);
      dl->AddLine(P(6.5f, 17.5f), P(17.5f, 6.5f), w, line);
      break;
    default:
      dl->AddCircleFilled(P(12, 12), 4.f * k, w, 16);
      break;
  }
}

class CheatPopup final : public rex::ui::ImGuiDialog {
 public:
  CheatPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) : ImGuiDialog(drawer), window_(window) {
    drawer->RemoveDialog(this);   // attached only while a notice shows
  }
  ~CheatPopup() override { *alive_ = false; }
  bool WantsContinuousPaint() const override { return false; }   // drawn with each game frame

  // Any thread. The newest key press replaces the notice showing (no backlog when a key is pressed quickly).
  void Push(Entry e) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      current_ = std::move(e);
    }
    if (window_) {
      window_->app_context().CallInUIThread([this, alive = alive_] {
        if (*alive) imgui_drawer()->AddDialog(this);   // no-op if already added
      });
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (XenosInGameMenuIsOpen()) return;   // waits for COGBox's menu to close
    const auto now = std::chrono::steady_clock::now();
    Entry e;
    float t = 0.f;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (current_ && !current_->started) current_->started = now;
      if (current_) t = std::chrono::duration<float>(now - *current_->started).count();
      if (!current_ || t >= kShowSeconds) {
        current_.reset();
        imgui_drawer()->RemoveDialog(this);   // safe during drawing (the SDK defers it)
        return;
      }
      e = *current_;
    }
    const float a = std::clamp(std::min(t / kInSeconds, (kShowSeconds - t) / kOutSeconds), 0.f, 1.f);
    const float ease = 1.f - (1.f - a) * (1.f - a);

    // The mock-up's sizes are for 720p: u = 1 there, following the output height (never below 80 %).
    const float u = std::clamp(io.DisplaySize.y / 720.f, 0.8f, 4.f), margin = 40.f * u;
    const float height = 44.f * u, radius = height * 0.5f, circle = 28.f * u, gap = 12.f * u;
    const float pad_l = 8.f * u, pad_r = 16.f * u, name_px = 16.f * u, chip_px = 12.f * u;
    const float chip_pad_x = 9.f * u, chip_pad_y = 3.f * u;
    ImFont* font1 = XenosAchievementFontFor(true, name_px);
    ImFont* font2 = XenosAchievementFontFor(true, chip_px);
    if (!font1) font1 = ImGui::GetFont();
    if (!font2) font2 = ImGui::GetFont();
    const bool chip = !e.state.empty() && e.tone != XenosCheatTone::Refused;
    const ImVec2 t1 = font1->CalcTextSizeA(name_px, FLT_MAX, 0.f, e.label.c_str());
    const ImVec2 t2 = chip ? font2->CalcTextSizeA(chip_px, FLT_MAX, 0.f, e.state.c_str()) : ImVec2(0, 0);
    const float chip_w = t2.x + 2.f * chip_pad_x;
    float width = pad_l + circle + gap + t1.x + (chip ? gap + chip_w : 0.f) + pad_r;
    width = std::min(width, io.DisplaySize.x - 2.f * margin);

    using P = xenos::ach::PopupPosition;
    P pos = xenos::ach::ParsePosition(REXCVAR_GET(cogbox_cheat_popup_position));
    if (pos == P::Default) pos = P::TopLeft;
    const bool top = pos == P::TopLeft || pos == P::TopCentre || pos == P::TopRight;
    const bool left = pos == P::TopLeft || pos == P::BottomLeft, right = pos == P::TopRight || pos == P::BottomRight;
    // slides in from its side (left / right edge; the centre ones from the top or bottom edge) while it fades in
    const float slide = (1.f - ease) * 24.f * u;
    ImVec2 p0(left ? margin : right ? io.DisplaySize.x - margin - width : (io.DisplaySize.x - width) * 0.5f,
              top ? margin : io.DisplaySize.y - margin - height);
    if (left) p0.x -= slide;
    else if (right) p0.x += slide;
    else p0.y += top ? -slide : slide;
    const ImVec2 p1(p0.x + width, p0.y + height);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // soft shadow: a few widening, fading pills below
    for (int i = 3; i >= 1; i--) {
      const float g = float(i) * 3.f * u;
      dl->AddRectFilled(ImVec2(p0.x - g, p0.y - g + 6.f * u), ImVec2(p1.x + g, p1.y + g + 6.f * u),
                        IM_COL32(0, 0, 0, int(38.f * a / float(i))), radius + g);
    }
    dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 18, int(219.f * a)), radius);   // 86 %
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, int(20.f * a)), radius, 0, std::max(1.f, u * 0.75f));
    const Rgb accent = Accent(e.tone);
    const float cy = p0.y + radius;
    const ImVec2 cc(p0.x + pad_l + circle * 0.5f, cy);
    dl->AddCircleFilled(cc, circle * 0.5f, Col(accent, a), 32);
    DrawIcon(dl, cc, 18.f * u, e.tone == XenosCheatTone::Refused ? XenosCheatIcon::Refused : e.icon, a);
    float x = p0.x + pad_l + circle + gap;
    dl->PushClipRect(p0, p1, true);
    dl->AddText(font1, name_px, ImVec2(x, cy - t1.y * 0.5f), IM_COL32(238, 241, 244, int(255.f * a)), e.label.c_str());
    if (chip) {
      x += t1.x + gap;
      const ImVec2 c0(x, cy - t2.y * 0.5f - chip_pad_y), c1(x + chip_w, cy + t2.y * 0.5f + chip_pad_y);
      const float cr = 10.f * u;
      dl->AddRectFilled(c0, c1, Col(accent, 0.28f * a), cr);   // tinted fill
      dl->AddRect(c0, c1, Col(Lighter(accent), a), cr, 0, std::max(1.f, u));
      dl->AddText(font2, chip_px, ImVec2(x + chip_pad_x, cy - t2.y * 0.5f), Col(Lighter(Lighter(accent)), a), e.state.c_str());
    }
    dl->PopClipRect();
  }

 private:
  rex::ui::Window* window_ = nullptr;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::mutex mutex_;
  std::optional<Entry> current_;
};

std::mutex g_popup_mutex;   // Push comes from the game thread, install / shutdown from the UI thread
std::unique_ptr<CheatPopup> g_popup;
}  // namespace

void XenosInstallCheatPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) {
  std::lock_guard<std::mutex> lock(g_popup_mutex);
  if (!drawer || !window || g_popup) return;
  g_popup = std::make_unique<CheatPopup>(drawer, window);
}

void XenosShutdownCheatPopup() {
  std::lock_guard<std::mutex> lock(g_popup_mutex);
  g_popup.reset();
}

void XenosCheatPopupPush(const std::string& label, const std::string& state, XenosCheatTone tone, XenosCheatIcon icon) {
  if (!REXCVAR_GET(cogbox_cheat_popups)) return;
  Entry e;
  e.label = XenosTr(label);
  e.state = state.empty() ? std::string() : XenosTr(state);
  if (tone == XenosCheatTone::Refused && !e.state.empty()) {   // no chip: one message, "God mode: not recognised by the game"
    e.label += ": " + e.state;
    e.state.clear();
  }
  e.tone = tone;
  e.icon = icon;
  std::lock_guard<std::mutex> lock(g_popup_mutex);
  if (g_popup) g_popup->Push(std::move(e));
}
