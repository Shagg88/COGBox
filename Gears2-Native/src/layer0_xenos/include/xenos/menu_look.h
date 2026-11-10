// Layer 0 (game-free): the in-game menu's look (0.4.9, Shagg: "Noto Sans, the whole screen, like the launcher, not like
// something from Windows ME"). Only Dear ImGui and the C++ library: no runtime, no game - so the same code also draws the
// offline mock-ups (tools/menu_mock). The menu's behaviour (input, live settings, files) stays in ingame_menu.cpp.
//
// Layout: the whole screen is the menu - a dimmed game behind it and the content laid out over the full height. The
// content keeps a 16:9 width at most: 16:9 and 16:10 use the whole screen, 21:9 / 32:9 get a centred 16:9 area with
// the game dimmed at the sides. Everything is sized from the screen height (1 unit = height / 1080), so 720p to 4K
// look the same.
// Style: the launcher's - gunmetal plates with cut corners, crimson accent, headings with a crimson bar, tabs with cut
// top corners, a crimson main button, button / key pictures in a legend at the bottom. Our code (MIT).
#pragma once

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

namespace xenos::menu {

// ---- fonts -----------------------------------------------------------------------------------------------------------
// Noto Sans (SIL Open Font License; <COGBox>\assets\fonts) baked at the sizes the menu draws at for `bake_height`
// (the monitor's height): the SDK's font atlas is made once, so text is crisp when it is drawn at the baked size and
// only scaled when the window is smaller. Characters Noto Sans has no glyph for (Korean, Chinese, Japanese) are merged
// from the PC's fonts, as the launcher does.
struct Fonts {
  ImFont* regular = nullptr;        // rows, descriptions
  ImFont* regular_small = nullptr;  // notes, the legend
  ImFont* semibold = nullptr;       // labels in focus, buttons, tabs
  ImFont* semibold_small = nullptr; // tags, key caps
  ImFont* bold = nullptr;           // the title
  float bake_height = 1080.f;       // the screen height the sizes were made for
  bool ok() const { return regular && semibold; }
};
struct FontFiles {
  std::string regular, semibold, bold;  // Noto Sans TTFs
  std::string hangul;                   // e.g. C:\Windows\Fonts\malgun.ttf ("" = none)
  std::string han;                      // Japanese / Chinese font file (YuGothM.ttc or msyh.ttc; "" = none)
};
// `text`: every string the menu can show (UTF-8) - only their characters are baked (plus all of ASCII / Latin-1).
Fonts ConfigureFonts(ImFontAtlas* atlas, const FontFiles& files, const std::string& text, float bake_height);

// ---- the frame ----------------------------------------------------------------------------------------------------
struct Look {
  ImVec2 display;          // the whole screen
  ImVec2 c0, c1;           // the content area (16:9 at most, centred)
  float u = 1.f;           // one unit = display height / 1080
  float fade = 1.f;        // 0..1 open fade
  bool ps = false;         // PlayStation button names / shapes
  bool pad = false;        // the legend shows the controller (else keys)
  Fonts fonts;
  float px(float base) const { return base * u; }
};
Look MakeLook(ImVec2 display, float fade, const Fonts& fonts, bool ps, bool pad);

// The size every text role is drawn at, in units (px at 1080p).
constexpr float kTitle = 40.f, kBody = 26.f, kSmall = 21.f, kTag = 17.f;

// Push / pop the launcher's ImGui colours and shapes for widgets (combo boxes, sliders, scroll bars, popups).
void PushStyle(const Look& L);
void PopStyle();

// The dimmed game behind the menu (darker at the sides on ultrawide screens).
void Backdrop(ImDrawList* dl, const Look& L);
// A gunmetal plate with cut corners (lit: a crimson edge).
void Plate(ImDrawList* dl, ImVec2 a, ImVec2 b, float chamfer, const Look& L, bool lit = false, float alpha = 1.f);
// "COGBOX  <game>" with the crimson bar; returns the y under the header rule.
float Header(ImDrawList* dl, const Look& L, const char* game, const char* right);
// A heading: crimson bar, text, a thin rule to x1 (launcher th_heading).
void Heading(ImDrawList* dl, const Look& L, ImVec2 p, const char* text, float x1);
// The tabs across the content; returns the tab clicked (or -1). `prev` / `next`: the button names shown at the ends.
int Tabs(const Look& L, ImVec2 a, ImVec2 b, const std::vector<std::string>& names, int current, const char* prev,
         const char* next);
// A launcher button: cut corners, crimson when `red`; drawn as focused when ImGui's nav is on it.
bool Button(const Look& L, const char* label, ImVec2 size, bool red, bool enabled = true);
// An On / Off switch (A / Enter / click flips it). Returns true when flipped.
bool Toggle(const Look& L, const char* id, bool* v, ImVec2 size, const char* on_text, const char* off_text);
// A small tag with cut corners ("RESTART REQUIRED"); returns its width.
float Tag(ImDrawList* dl, const Look& L, ImVec2 p, const char* text, ImVec4 colour);
// A status lamp (launcher th_lamp): 1 green, 2 amber, 3 red.
void Lamp(ImDrawList* dl, const Look& L, ImVec2 c, float r, int state);
// Text with a given font role and size (units); returns its width.
float Text(ImDrawList* dl, const Look& L, ImFont* f, float size_units, ImVec2 p, ImU32 col, const char* text,
           float wrap_w = 0.f);
ImVec2 TextSize(const Look& L, ImFont* f, float size_units, const char* text, float wrap_w = 0.f);

// The legend along the bottom: button / key pictures and what they do (launcher front.cpp's hint line).
struct LegendItem {
  bool key;                // true: a keyboard key cap; false: a controller button
  const char* name;        // "A", "B", "X", "Y", "LB", "RB", "D-pad", "Start" / a key's name
  std::string label;       // what it does
};
void Legend(ImDrawList* dl, const Look& L, const std::vector<LegendItem>& items);

// ---- the settings page --------------------------------------------------------------------------------------------
struct Row {
  std::string id, label, kind, tip, why;   // kind: bool / choice / int / float
  const std::vector<std::string>* choices = nullptr;
  double* value = nullptr;                  // edited in place
  double min = 0, max = 1, step = 1;
  int decimals = 0;
  bool enabled = true;
  bool restart = false;    // a change waiting for "Restart to apply"
  bool live = false;       // the current value applies at once
};
struct SettingsPage {
  const char* game = "";
  std::vector<std::string> tabs;       // translated
  int* tab = nullptr;
  std::vector<Row> rows;               // this tab's rows only
  std::string status;                  // the line above the buttons
  int restart_changes = 0;
  bool can_restart = false;
  bool has_file = true;                // false: started without COGBox (only Resume / Quit)
  bool focus_resume = false;           // first frame: keyboard / pad focus on Resume (only without `focus`)
  // Review 049 (on-PC: the rows never took pad focus; Dear ImGui's own pad navigation stayed in the button column).
  // With `focus` set the menu moves the focus itself: an index over the rows (0..rows-1), then the buttons
  // (Resume, Achievements, Restart, Quit). nav_move -1 / +1 = up / down this frame, nav_adjust -1 / +1 = left / right
  // (right: from a row to the buttons, left: back to that row; never a value). focus_reset: focus on the first row (Resume if there are none).
  int* focus = nullptr;
  bool focus_reset = false;
  int nav_move = 0, nav_adjust = 0;   // menu2: nav_adjust +1 = row -> buttons, -1 = buttons -> the last row
  int* last_row = nullptr;            // the row the buttons go back to (menu-owned; a static if null)
  // Quit game asks first: *confirm_quit is the open "Quit? Yes / No" box, *confirm_sel 0 = Yes (the default, 0.5.0), 1 = No.
  bool* confirm_quit = nullptr;
  int* confirm_sel = nullptr;
  // translated words
  const char *t_resume = "Resume", *t_achievements = "Achievements", *t_restart = "Restart to apply",
             *t_quit = "Quit game", *t_restart_required = "RESTART REQUIRED", *t_on = "On", *t_off = "Off",
             *t_applies_now = "Applies at once", *t_needs_restart = "Needs a restart to change",
             *t_waiting = "Restart required - waits for Restart to apply", *t_no_file = "",
             *t_right = "", *t_quit_confirm = "Quit the game? Changes that wait for a restart are kept for the next start.",
             *t_yes = "Yes", *t_no = "No";
  const char* t_social = nullptr;   // social step 5: the Social button, when the game runs online
  std::vector<LegendItem> legend;
};
enum class Action { kNone, kResume, kAchievements, kRestart, kQuit, kBack, kSocial };
// Draws the whole page (call inside a window that covers the display, no padding). Returns what was clicked.
Action DrawSettings(const Look& L, SettingsPage& page);

// The achievements page frame: header, a heading, the body area for the browser (returned), the two buttons and the
// legend. Call FrameBody's area with the browser between BeginAchievements and EndAchievements.
struct Rect { ImVec2 a, b; };
Rect BeginAchievements(const Look& L, const char* game, const char* heading, const char* right);
Action EndAchievements(const Look& L, const char* t_back, const char* t_resume, const std::vector<LegendItem>& legend);

}  // namespace xenos::menu
