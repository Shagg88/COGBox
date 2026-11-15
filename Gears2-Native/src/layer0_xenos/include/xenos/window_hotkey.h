// Layer 0 (game-free): the window-mode hotkey (GitHub #316) - one key (default Alt+Enter) that switches the game window
// between windowed and borderless full screen, in all four games. The key text is the cvar bind_cogbox_fullscreen:
// "Alt+Return", "F11", "Ctrl+Shift+F11" - zero or more modifiers (Alt, Ctrl, Shift) and one key named as the SDK's other
// binds name theirs (rex/ui/keybinds.h: F1, Return, PageUp, Backtick ...); "None" or empty turns the hotkey off.
// The text -> key rules are here, header only (they need nothing but rex::ui::ParseVirtualKey), so
// tests/layer0_window_hotkey_test.cpp checks them. Our code (MIT).
#pragma once
#include <cctype>
#include <string>
#include <string_view>

#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>

namespace rex::ui {
class Window;
}

namespace xenos::window_hotkey {

struct Chord {
  rex::ui::VirtualKey key = rex::ui::VirtualKey::kNone;
  bool alt = false;
  bool ctrl = false;
  bool shift = false;
  bool valid() const { return key != rex::ui::VirtualKey::kNone; }
};

namespace detail {
inline std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}
inline bool EqualsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}
// The key's name as the SDK spells it, or as typed in lower case / upper case ("return", "f11").
inline rex::ui::VirtualKey ParseKeyName(std::string_view name) {
  // "Enter" is what the key says on it (and what the launcher shows), but the SDK's name is Return (r40)
  if (name.size() == 5) {
    std::string low(name);
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low == "enter") return rex::ui::VirtualKey::kReturn;
  }
  rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(name);
  if (vk != rex::ui::VirtualKey::kNone) return vk;
  std::string s(name);
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  vk = rex::ui::ParseVirtualKey(s);
  if (vk != rex::ui::VirtualKey::kNone) return vk;
  for (size_t i = 1; i < s.size(); ++i) s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
  return rex::ui::ParseVirtualKey(s);
}
}  // namespace detail

// "Alt+Return" -> {kReturn, alt}. False (out left invalid) for "", "None", an unknown key, a modifier or a mouse button
// on its own, or a text with no key; those leave the hotkey off.
inline bool ParseChord(std::string_view text, Chord& out) {
  out = Chord{};
  text = detail::Trim(text);
  if (text.empty() || detail::EqualsNoCase(text, "none")) return false;
  Chord c;
  size_t pos = 0;
  while (true) {
    const size_t plus = text.find('+', pos);
    const std::string_view tok = detail::Trim(text.substr(pos, plus == std::string_view::npos ? std::string_view::npos : plus - pos));
    if (tok.empty()) return false;
    if (plus == std::string_view::npos) {   // the last token is the key
      const rex::ui::VirtualKey vk = detail::ParseKeyName(tok);
      using VK = rex::ui::VirtualKey;
      if (vk == VK::kNone || vk == VK::kShift || vk == VK::kControl || vk == VK::kMenu || vk == VK::kLButton ||
          vk == VK::kRButton || vk == VK::kMButton || vk == VK::kXButton1 || vk == VK::kXButton2)
        return false;
      c.key = vk;
      break;
    }
    if (detail::EqualsNoCase(tok, "alt")) c.alt = true;
    else if (detail::EqualsNoCase(tok, "ctrl") || detail::EqualsNoCase(tok, "control")) c.ctrl = true;
    else if (detail::EqualsNoCase(tok, "shift")) c.shift = true;
    else return false;
    pos = plus + 1;
  }
  out = c;
  return true;
}

// The modifiers must match exactly: Alt+Enter is not Enter, and F11 is not Ctrl+F11.
inline bool Matches(const Chord& c, rex::ui::VirtualKey key, bool alt, bool ctrl, bool shift) {
  return c.valid() && key == c.key && alt == c.alt && ctrl == c.ctrl && shift == c.shift;
}

}  // namespace xenos::window_hotkey

// Install from XenosInstallInGameMenu (every game goes through it), shut down from XenosShutdownInGameMenu; UI thread.
void XenosInstallWindowHotkey(rex::ui::Window* window);
void XenosShutdownWindowHotkey();
