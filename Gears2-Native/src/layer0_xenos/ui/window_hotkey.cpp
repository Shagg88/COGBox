// Layer 0 (game-free): GitHub #316 - a hotkey that switches the game window between windowed and borderless full screen
// (the same switch as the in-game menu's full screen setting: Window::SetFullscreen, which puts the window on the
// monitor it is on, and back to its windowed size and place). See xenos/window_hotkey.h for the key text.
//
// The default is Alt+Enter, not F11: F11 is already taken in all four games (the cheat keys' "all weapons and ammo" with
// --gow*_cheats, and XeGTAO's quality with --gow2_ao_debug_keys), and a key that did two things was no choice.
//
// The window's swap chain follows by itself: SDL reports the new pixel size, the presenters (DirectX 12 and Vulkan)
// resize or recreate their swap chain, and the frame-generation hooks (AMD, DLSS-G; AMD's Vulkan one through its
// swap-chain destroy hook) drop their contexts before it - the same path the in-game menu's setting and a window drag use.
// The renderer's "match my monitor" image is made again at the window's new size (xenos::display::SetSurfaceSize).
//
// Not while the window is not in focus, not for the key's auto-repeat, and not while a text field wants the keys
// (Window::IsTextInputActive: Dear ImGui's text input, e.g. a field in the in-game menu). A press's auto-repeat is
// swallowed until the key comes up, even if a modifier is let go first. Our code (MIT).
#include <xenos/window_hotkey.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#include <atomic>
#include <string>

REXCVAR_DEFINE_STRING(bind_cogbox_fullscreen, "Alt+Return", "Input/Keybinds/System",
                      "GitHub #316: switch the game window between windowed and borderless full screen - the key, with "
                      "optional Alt+ / Ctrl+ / Shift+ in front (Alt+Return, F11, Ctrl+Shift+F11); None turns it off");

REXCVAR_DECLARE(bool, fullscreen_exclusive);   // the SDK's (ui/window.cpp): full screen changes the display mode

namespace {

namespace hk = xenos::window_hotkey;

std::atomic<rex::ui::Window*> g_window{nullptr};   // the window the hotkey is installed on (UI thread; read in a deferred call)

class WindowModeListener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    rex::ui::Window* window = g_window.load();
    if (!window) return;
    const std::string text = REXCVAR_GET(bind_cogbox_fullscreen);
    hk::Chord chord;
    if (!hk::ParseChord(text, chord)) {
      if (text != last_bad_ && !text.empty() && text != "None" && text != "none") {   // said once per text, not per key press
        last_bad_ = text;
        REXLOG_WARN("window hotkey: '{}' is not a key (try Alt+Return, F11 or Ctrl+F11; None turns it off); no hotkey", text);
      }
      return;
    }
    // r40: once a press of the key was ours, its auto-repeat stays ours until the key comes up, even if Alt (or Ctrl,
    // Shift) is let go first - otherwise the repeats would reach the game as plain Enter (its Start button).
    if (swallowing_ != rex::ui::VirtualKey::kNone && e.virtual_key() == swallowing_ && e.prev_state()) {
      e.set_handled(true);
      return;
    }
    if (!hk::Matches(chord, e.virtual_key(), e.is_alt_pressed(), e.is_ctrl_pressed(), e.is_shift_pressed())) return;
    // A text field wants this key press (and a window without focus gets no key events, but is not asked to switch).
    if (!window->HasFocus() || window->IsTextInputActive()) return;
    // From here the key is ours: the game does not get it (Alt+Enter would be its Start button as well).
    e.set_handled(true);
    swallowing_ = e.virtual_key();
    if (e.prev_state()) return;   // the key's auto-repeat (it was already down): one press, one switch
    window->app_context().CallInUIThreadDeferred([] {
      rex::ui::Window* w = g_window.load();   // cleared at shutdown: the window may be gone by now
      if (!w) return;
      const bool full = !w->IsFullscreen();
      w->SetFullscreen(full);
      REXLOG_INFO("window hotkey: {}",
                  !full ? "windowed" : (REXCVAR_GET(fullscreen_exclusive) ? "exclusive full screen" : "borderless full screen"));
    });
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (e.virtual_key() == swallowing_) swallowing_ = rex::ui::VirtualKey::kNone;   // the next press is a new press
  }

 private:
  std::string last_bad_;
  rex::ui::VirtualKey swallowing_ = rex::ui::VirtualKey::kNone;   // UI thread only, like the key events
};

WindowModeListener g_listener;

}  // namespace

void XenosInstallWindowHotkey(rex::ui::Window* window) {
  if (!window || g_window.load()) return;
  g_window.store(window);
  // Above the SDK's own listeners (the game's keyboard driver, Dear ImGui at 64): the key is taken before the game sees it.
  window->AddInputListener(&g_listener, 128);
  REXLOG_INFO("window hotkey: {} switches windowed / borderless full screen (--bind_cogbox_fullscreen)",
              REXCVAR_GET(bind_cogbox_fullscreen));
}

void XenosShutdownWindowHotkey() {
  rex::ui::Window* window = g_window.exchange(nullptr);
  if (window) window->RemoveInputListener(&g_listener);
}
