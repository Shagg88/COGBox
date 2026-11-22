// Layer 0 (game-free): the in-match chat panel (xenos/chat_panel.h). Our code (MIT).
#include <xenos/chat_panel.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/logging/macros.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <xenos/achievement_popup_assets.h>   // COGBox's UI font
#include <xenos/chat_logic.h>
#include <xenos/dev_tools.h>                  // XenosInGameMenuIsOpen

REXCVAR_DEFINE_BOOL(cogbox_match_chat, true, "COGBox",
                    "In-match chat: Y / U open a chat box in a match and other players' messages show on the left");
REXCVAR_DEFINE_STRING(cogbox_match_chat_background, "dark", "COGBox",
                      "In-match chat background: dark (a see-through panel with a red edge) or none (outlined text only)");

namespace {
namespace chat = xenos::chat;

std::atomic<bool (*)()> g_match_running{nullptr};
std::atomic<bool (*)(const std::string&, bool)> g_send{nullptr};

double Now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The approved mock-up's values at 720p (u = 1 there, following the output height).
constexpr float kWidth = 420.f, kTextPx = 13.5f, kRowGap = 3.f, kPadX = 8.f, kPadY = 6.f, kEdge = 3.f;
constexpr int kRowsShown = 5;          // message rows at most (wrapped rows count), above the typing row
constexpr ImU32 kName = IM_COL32(255, 255, 255, 255), kAll = IM_COL32(207, 214, 210, 255),
                kTeam = IM_COL32(127, 196, 255, 255), kPrompt = IM_COL32(255, 211, 107, 255),
                kBack = IM_COL32(10, 14, 12, 158), kRed = IM_COL32(163, 39, 31, 255);

ImU32 WithAlpha(ImU32 c, float a) {
  return (c & ~IM_COL32_A_MASK) | (ImU32(float((c >> IM_COL32_A_SHIFT) & 0xFF) * a) << IM_COL32_A_SHIFT);
}

class ChatPanel;

// While the typing row is open, every key press and typed character goes to it and none reaches the game: this listener
// sits above the keyboard / mouse driver (z 200; the window hotkey's 128 already swallows keys the same way). Key-ups pass
// through, so a key held when the box opened (W...) is let go in the game too. The mouse and pads are never touched.
class KeyListener final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  explicit KeyListener(ChatPanel* panel) : panel_(panel) {}
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyChar(rex::ui::KeyEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;

 private:
  ChatPanel* panel_;
};

class ChatPanel final : public rex::ui::ImGuiDialog {
 public:
  ChatPanel(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window)
      : ImGuiDialog(drawer), window_(window), keys_(this) {
    drawer->RemoveDialog(this);   // attached only while there is something to show
    if (window_) {
      window_->AddInputListener(&keys_, 200);
      window_->AddListener(&keys_);
    }
  }
  ~ChatPanel() override {
    *alive_ = false;
    if (window_) {
      window_->RemoveInputListener(&keys_);
      window_->RemoveListener(&keys_);
    }
  }

  // Whether Y / U may open the typing row now: chat on, no COGBox screen, a match running.
  static bool CanOpen() {
    if (!REXCVAR_GET(cogbox_match_chat) || XenosInGameMenuIsOpen()) return false;
    auto* running = g_match_running.load();
    return running && running();
  }

  // UI thread, the listener (z 200, above the keyboard / mouse driver and the SDK's binds): a press of the chat keys
  // opens the box and is the box's - but only when it can open (in a match). Anywhere else the key goes on to the
  // game as any other key (the SDK bind below then finds it, does nothing, and the game has already had it).
  bool BindKey(rex::ui::VirtualKey vk, bool repeat) {
    if (repeat || vk == rex::ui::VirtualKey::kNone) return false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (typing_) return false;
    }
    const auto key_of = [](const char* bind) { return rex::ui::ParseVirtualKey(rex::cvar::GetFlagByName(bind)); };
    const bool all = vk == key_of("bind_cogbox_chat_all"), team = !all && vk == key_of("bind_cogbox_chat_team");
    if ((!all && !team) || !CanOpen()) return false;
    Open(team);
    return true;
  }

  // UI thread (BindKey, or the Y / U binds). Opens the typing row (or switches ALL / TEAM while open).
  void Open(bool team) {
    if (!CanOpen()) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (typing_) return;
      typing_ = true, team_ = team, text_.clear(), caret_ = 0, skip_char_ = true;
      opened_at_ = Now();
    }
    // (not under mutex_: the window could hand us an event from inside the call)
    if (window_) window_->SetTextInputActive(true);   // SDL sends typed characters only while this is on
    Attach();
  }

  // UI thread: the typing row's keys. Returns true when the key was the box's (it never reaches the game).
  bool Key(rex::ui::VirtualKey vk, bool repeat) {
    using rex::ui::VirtualKey;
    std::string send;
    bool team = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!typing_) {
        // the Enter / Esc that closed the box: its auto-repeats stay ours until it comes up
        if (repeat && vk == swallowing_) return true;
        swallowing_ = VirtualKey::kNone;
        return false;
      }
      switch (vk) {
        case VirtualKey::kReturn:
          send = text_, team = team_;
          Close(vk);
          break;
        case VirtualKey::kEscape: Close(vk); break;
        case VirtualKey::kBack:
          if (caret_ > 0) text_.erase(--caret_, 1);
          break;
        case VirtualKey::kDelete:
          if (caret_ < text_.size()) text_.erase(caret_, 1);
          break;
        case VirtualKey::kLeft: caret_ = caret_ ? caret_ - 1 : 0; break;
        case VirtualKey::kRight: caret_ = std::min(caret_ + 1, text_.size()); break;
        case VirtualKey::kHome: caret_ = 0; break;
        case VirtualKey::kEnd: caret_ = text_.size(); break;
        default: break;
      }
      // A new key press after the box opened: any character from now on is typed, never the opening key's own
      // (Flippage: "Y" then "yes" lost its first 'y').
      skip_char_ = false;
    }
    ApplyTextInput();
    if (!send.empty()) {
      auto* fn = g_send.load();
      if (fn && !fn(send, team) && !chat::CleanText(send).empty()) {
        // there was something to send, but it could not go (5 already waiting): say so instead of losing it silently
        std::lock_guard<std::mutex> lock(mutex_);
        log_.Add({"", "Not sent: too many messages waiting - try again in a few seconds", false, Now()});
      }
    }
    return true;
  }

  // UI thread: a typed character. True when the box took it.
  bool Char(unsigned int c) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!typing_) return false;
    // the bind's own letter (Y / U) may arrive as a character right after it opened the box, before any other key
    // press - only that one is skipped
    if (skip_char_) {
      skip_char_ = false;
      if (Now() - opened_at_ < 0.5 && (c == 'y' || c == 'Y' || c == 'u' || c == 'U')) return true;
    }
    if (c >= 0x20 && c < 0x7F && text_.size() < chat::kMaxChars) text_.insert(caret_++, 1, char(c));
    return true;
  }

  void CloseNow() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (typing_) Close(rex::ui::VirtualKey::kNone);
    }
    ApplyTextInput();
  }
  bool WantsContinuousPaint() const override { return false; }   // drawn with each game frame

  void Receive(const std::string& raw, bool team, size_t name_len) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      chat::Line line = chat::ParseReceived(raw, Now(), name_len);
      line.team = line.team || team;   // the team marker (ParseReceived) or the caller's say
      log_.Add(std::move(line));
    }
    Attach();
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::vector<std::pair<chat::Line, float>> lines;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (typing_) {   // the match ended (or a COGBox screen opened) while typing: the box closes, the text is dropped
        auto* running = g_match_running.load();
        if (!running || !running() || XenosInGameMenuIsOpen()) Close(rex::ui::VirtualKey::kNone);
      }
      lines = log_.Visible(Now(), typing_);
      drawing_typing_ = typing_;   // Draw's copy (read without the lock)
    }
    ApplyTextInput();
    if (lines.empty() && !drawing_typing_) {
      imgui_drawer()->RemoveDialog(this);   // safe during drawing (the SDK defers it)
      return;
    }
    if (!REXCVAR_GET(cogbox_match_chat) || XenosInGameMenuIsOpen()) return;
    Draw(io, lines);
  }

 private:
  struct Row {
    std::string text;
    size_t name_chars = 0;   // the first characters drawn in the name colour (first row of a message)
    ImU32 colour;
    float alpha;
  };

  void Attach() {
    if (!window_) return;
    window_->app_context().CallInUIThread([this, alive = alive_] {
      if (*alive) imgui_drawer()->AddDialog(this);   // no-op if already added
    });
  }

  void Draw(ImGuiIO& io, const std::vector<std::pair<chat::Line, float>>& lines) {
    const float u = std::clamp(io.DisplaySize.y / 720.f, 0.8f, 4.f);
    const float px = kTextPx * u, row_h = px * 1.25f, gap = kRowGap * u, pad_x = kPadX * u, pad_y = kPadY * u;
    const float width = std::min(kWidth * u, io.DisplaySize.x * 0.5f), text_w = width - 2.f * pad_x - kEdge * u;
    ImFont* font = XenosAchievementFontFor(true, px);
    if (!font) font = ImGui::GetFont();
    const auto measure = [&](std::string_view s) {
      return font->CalcTextSizeA(px, FLT_MAX, 0.f, s.data(), s.data() + s.size()).x;
    };

    // Wrapped rows are kept per line (and dropped when the font size or width changes), so each frame only measures
    // lines it has not wrapped yet - not every line on screen again.
    if (wrap_px_ != px || wrap_w_ != text_w || wrap_font_ != font) wrap_cache_.clear(), wrap_px_ = px, wrap_w_ = text_w, wrap_font_ = font;
    std::vector<WrapEntry> keep;
    std::vector<Row> rows;
    for (const auto& [line, alpha] : lines) {
      const std::string prefix = (line.team ? "[TEAM] " : "") + (line.who.empty() ? std::string() : line.who + ": ");
      std::string all = prefix + line.text;
      auto hit = std::find_if(wrap_cache_.begin(), wrap_cache_.end(), [&](const WrapEntry& w) { return w.at == line.at && w.text == all; });
      if (hit == wrap_cache_.end()) {
        wrap_cache_.push_back({line.at, all, chat::Wrap(all, text_w, measure)});
        hit = wrap_cache_.end() - 1;
      }
      keep.push_back(*hit);
      const auto& wrapped = hit->rows;
      for (size_t i = 0; i < wrapped.size(); ++i)
        rows.push_back({wrapped[i], i == 0 ? std::min(prefix.size(), wrapped[i].size()) : 0, line.team ? kTeam : kAll, alpha});
    }
    wrap_cache_ = std::move(keep);   // only the lines still shown
    if (int(rows.size()) > kRowsShown) rows.erase(rows.begin(), rows.end() - kRowsShown);   // oldest rows go first

    const int shown = int(rows.size()) + (drawing_typing_ ? 1 : 0);
    if (!shown) return;
    const float full_h = 2.f * pad_y + float(kRowsShown + 1) * row_h + float(kRowsShown) * gap;
    const float bottom = io.DisplaySize.y * 0.5f + full_h * 0.5f;
    const float height = 2.f * pad_y + float(shown) * row_h + float(shown - 1) * gap;
    float panel_alpha = drawing_typing_ ? 1.f : 0.f;
    for (const Row& r : rows) panel_alpha = std::max(panel_alpha, r.alpha);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 p0(0.f, bottom - height), p1(width, bottom);
    const bool box = REXCVAR_GET(cogbox_match_chat_background) != "none";
    if (box) {
      dl->AddRectFilled(p0, p1, WithAlpha(kBack, panel_alpha));
      dl->AddRectFilled(p0, ImVec2(kEdge * u, bottom), WithAlpha(kRed, panel_alpha));
    }
    // No background: each line outlined in near-black so it reads over any part of the game.
    const auto text = [&](ImVec2 at, ImU32 colour, float alpha, const char* b, const char* e) {
      if (!box) {
        const float o = std::max(1.f, u);
        const ImU32 outline = WithAlpha(IM_COL32(0, 0, 0, 220), alpha);
        for (const ImVec2 d : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o)})
          dl->AddText(font, px, ImVec2(at.x + d.x, at.y + d.y), outline, b, e);
      }
      dl->AddText(font, px, at, WithAlpha(colour, alpha), b, e);
    };

    const float x = kEdge * u + pad_x;
    float y = p0.y + pad_y;
    for (const Row& r : rows) {
      text(ImVec2(x, y), r.colour, r.alpha, r.text.c_str(), nullptr);
      if (r.name_chars && r.colour != kTeam)   // the name in white over the line's own colour
        dl->AddText(font, px, ImVec2(x, y), WithAlpha(kName, r.alpha), r.text.c_str(), r.text.c_str() + r.name_chars);
      y += row_h + gap;
    }
    if (drawing_typing_) {
      if (!box)   // the typing row keeps a dark strip of its own, like a text field
        dl->AddRectFilled(ImVec2(0.f, y - pad_y * 0.5f), ImVec2(width, y + row_h + pad_y * 0.5f), kBack);
      DrawTyping(dl, font, px, x, y, text_w);
    }
  }

  // mutex_ held.
  void Close(rex::ui::VirtualKey key) {
    typing_ = false, text_.clear(), caret_ = 0;
    swallowing_ = key;
    text_input_off_ = true;   // switched off by ApplyTextInput, once mutex_ is let go
  }

  // UI thread, mutex_ NOT held: text input off after a Close (every Close is from a key, a draw or focus loss).
  void ApplyTextInput() {
    if (text_input_off_.exchange(false) && window_) window_->SetTextInputActive(false);
  }

  // The typing row: "ALL:" / "TEAM:", then the text (its end when it is wider than the row) and a blinking caret.
  // mutex_ is not held here: the text is copied first.
  void DrawTyping(ImDrawList* dl, ImFont* font, float px, float x, float y, float width) {
    std::string text;
    size_t caret;
    bool team;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      text = text_, caret = caret_, team = team_;
    }
    const char* label = team ? "TEAM: " : "ALL: ";
    dl->AddText(font, px, ImVec2(x, y), kPrompt, label);
    const float lx = x + font->CalcTextSizeA(px, FLT_MAX, 0.f, label).x, room = width - (lx - x);
    size_t first = 0;   // keep the caret in view
    while (first < caret && font->CalcTextSizeA(px, FLT_MAX, 0.f, text.c_str() + first, text.c_str() + caret).x > room - px)
      ++first;
    size_t last = text.size();
    while (last > caret && font->CalcTextSizeA(px, FLT_MAX, 0.f, text.c_str() + first, text.c_str() + last).x > room) --last;
    dl->AddText(font, px, ImVec2(lx, y), kName, text.c_str() + first, text.c_str() + last);
    if (std::fmod(Now(), 1.0) < 0.5) {
      const float cx = lx + font->CalcTextSizeA(px, FLT_MAX, 0.f, text.c_str() + first, text.c_str() + caret).x;
      dl->AddRectFilled(ImVec2(cx, y + px * 0.1f), ImVec2(cx + std::max(1.f, px * 0.1f), y + px * 1.15f), kName);
    }
  }

  struct WrapEntry {
    double at;
    std::string text;
    std::vector<std::string> rows;
  };
  std::vector<WrapEntry> wrap_cache_;   // UI thread (Draw)
  float wrap_px_ = 0, wrap_w_ = 0;
  const ImFont* wrap_font_ = nullptr;

  rex::ui::Window* window_;
  KeyListener keys_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::mutex mutex_;
  chat::Log log_;
  bool typing_ = false, team_ = false, skip_char_ = false;
  bool drawing_typing_ = false;              // UI thread: typing_ as OnDraw last saw it
  std::atomic<bool> text_input_off_{false};
  std::string text_;
  size_t caret_ = 0;
  double opened_at_ = 0;
  rex::ui::VirtualKey swallowing_ = rex::ui::VirtualKey::kNone;
};

void KeyListener::OnKeyDown(rex::ui::KeyEvent& e) {
  if (panel_->BindKey(e.virtual_key(), e.prev_state())) {
    e.set_handled(true);
    return;
  }
  if (panel_->Key(e.virtual_key(), e.prev_state())) e.set_handled(true);
}
void KeyListener::OnKeyChar(rex::ui::KeyEvent& e) {
  if (panel_->Char(static_cast<unsigned int>(e.virtual_key()))) e.set_handled(true);
}
void KeyListener::OnLostFocus(rex::ui::UISetupEvent&) { panel_->CloseNow(); }

std::mutex g_panel_mutex;   // Receive comes from the game thread, install / shutdown from the UI thread
std::unique_ptr<ChatPanel> g_panel;
}  // namespace

void XenosInstallChatPanel(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) {
  std::lock_guard<std::mutex> lock(g_panel_mutex);
  if (!drawer || !window || g_panel) return;
  g_panel = std::make_unique<ChatPanel>(drawer, window);
  // Shown in the launcher's key list; Shagg approves any key binds before they ship.
  rex::ui::RegisterBind("bind_cogbox_chat_all", "Y", "COGBox: chat to everyone", [] {
    std::lock_guard<std::mutex> lock(g_panel_mutex);
    if (g_panel) g_panel->Open(false);
  });
  rex::ui::RegisterBind("bind_cogbox_chat_team", "U", "COGBox: team chat", [] {
    std::lock_guard<std::mutex> lock(g_panel_mutex);
    if (g_panel) g_panel->Open(true);
  });
}

void XenosShutdownChatPanel() {
  {   // an open typing box gives the window its text input back first
    std::lock_guard<std::mutex> lock(g_panel_mutex);
    if (g_panel) g_panel->CloseNow();
  }
  rex::ui::UnregisterBind("bind_cogbox_chat_all");
  rex::ui::UnregisterBind("bind_cogbox_chat_team");
  std::lock_guard<std::mutex> lock(g_panel_mutex);
  g_panel.reset();
}

void XenosChatReceive(const std::string& raw, bool team, size_t name_len) {
  if (!REXCVAR_GET(cogbox_match_chat)) return;
  std::lock_guard<std::mutex> lock(g_panel_mutex);
  if (g_panel) g_panel->Receive(raw, team, name_len);
}

void XenosChatSetHooks(bool (*match_running)(), bool (*send)(const std::string& text, bool team)) {
  g_match_running.store(match_running);
  g_send.store(send);
}
