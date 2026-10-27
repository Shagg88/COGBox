// COGBox - the look: gunmetal plates, rivets, crimson accents, a ruined skyline behind it all. Everything is drawn in
// code with lines and shapes (no image files, no game art, no logos); the only pictures are the box covers.
#pragma once
#include "imgui.h"

extern float g_ui;   // the display's scale (1 = 96 dpi) times g_zoom
// Job 523 (Shagg 20:07): the whole launcher grows with a bigger window. g_zoom = window size / its starting size (1-2);
// g_ui is the starting scale times it, and text sizes go through FS() (the fonts are loaded once, at zoom 1).
extern float g_zoom;
inline float FS(const ImFont *f) { return f->LegacySize * g_zoom; }
inline float S(float v) { return v * g_ui; }
// All text is this much bigger than the layout scale (Shagg 22:09: "text still needs to be bigger. ALL TEXT"); sizes that
// hold text use T() so they grow with it.
constexpr float kText = 1.3f;
inline float T(float v) { return v * g_ui * kText; }

namespace col {
extern const ImVec4 text, dim, faint, red, redHi, redDark, amber, good, bad, steel, steelHi, edge, panel, shadow;
}
ImU32 U(const ImVec4 &c, float alpha = 1.0f);

extern ImFont *F_BODY, *F_BOLD, *F_SMALL, *F_HEAD, *F_TITLE, *F_HUGE;

void theme_fonts(ImGuiIO &io);   // loads the fonts at g_ui
void theme_style();              // ImGui colours and shapes

// A plate of gunmetal with cut corners (chamfer), a lit top edge and four rivets; lit = hovered / chosen.
void th_plate(ImDrawList *dl, ImVec2 a, ImVec2 b, float chamfer, bool lit, float alpha = 1.0f);
// The same shape, filled only (for backgrounds behind text).
void th_plate_fill(ImDrawList *dl, ImVec2 a, ImVec2 b, float chamfer, ImU32 fill);
// A rivet (a small domed bolt head).
void th_rivet(ImDrawList *dl, ImVec2 c, float r, float alpha = 1.0f);
// Yellow-and-black warning stripes in a box.
void th_hazard(ImDrawList *dl, ImVec2 a, ImVec2 b, float alpha = 1.0f);
// A plain machine cog: 'teeth' square teeth around a ring with a hole (a generic gear, not any emblem).
void th_cog(ImDrawList *dl, ImVec2 c, float r, int teeth, float angle, ImU32 colr, ImU32 hole);
// A heading: a short crimson bar, then the text in the heading font, then a thin rule to x1.
void th_heading(const char *text, float x1);
// Buttons: a crimson one (the main action), a steel one; both with cut corners. Return true when clicked.
// selected: drawn as chosen by the controller (the front screen keeps its own choice instead of Dear ImGui's focus).
bool th_button_red(const char *label, ImVec2 size, bool enabled = true, ImFont *font = nullptr, bool selected = false);
bool th_button(const char *label, ImVec2 size, bool enabled = true, ImFont *font = nullptr, bool selected = false);
// A tab: cut top corners; lit when chosen.
bool th_tab(const char *label, bool selected, ImVec2 size);
// A status light: a small round lamp and a label (green ok, amber warn, red bad).
void th_lamp(ImDrawList *dl, ImVec2 c, float r, int state);   // 0 off, 1 ok, 2 warn, 3 bad

// The background behind everything: a dark sky, a ruined skyline in three layers, slow drifting ash and embers.
// 'tint' 0..1 brings in the game's own colour (Gears 2 cold blue, Gears 3 dusty amber, Judgment ash red).
void th_background(ImDrawList *dl, ImVec2 size, double t, ImVec4 tint);
