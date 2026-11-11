// Layer 0 (game-free): catalog of known prompt texture atlases (512x512 HUD action prompts, 256x256 Gears 2 / 3 /
// Judgment button font sheets, and 256x256 Melee & Chainsaw Duel sheets) and their per-button cell coordinates.
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
#pragma once

#include <cstdint>
#include <span>

namespace xenos::renderer::prompt {

enum class AtlasKind : uint8_t {
  kHud512,        // 512x512 HUD action prompts (A, B, X, Y, RS)
  kFont256G3J,    // 256x256 Gears 3 & Judgment button font sheet
  kFont256G2,     // 256x256 Gears 2 button font sheet
  kChainsawDuel,  // 256x256 Melee & Chainsaw Duel sheet (B mash, LT, RT, Y)
  kHud512G1,      // GitHub #214: Gears 1's 512x512 HUD action sheet (Gears 2's A/B/X/Y cells; no RS cells there)
  kChainsawG2,    // GitHub #223: Gears 2's 128x256 chainsaw sheet (B mash, LT, RT: the duel sheet's left half)
  kWeaponIcons,   // GitHub #223: Gears 2 / 3 / Judgment 512x512 weapon icon sheet (LS press, stick move, LT, RT)
  kHudAllG1,      // GitHub #223: Gears 1's 512x512 HUD-all-texture (Y, A, A, X, B)
  kWeaponIconsG3, // GitHub #223: Gears 3's 512x512 Gears3_WeaponIcons (RB)
  kWeaponIconsJ,  // GitHub #223: Judgment's 512x512 Gears3_WeaponIcons (RB, LB)
  kDpadJ,         // GitHub #223: Judgment's 104x104 d-pad sheet (right, up, left, down)
  kDirButtonA,    // GitHub #223: Gears 3 / Judgment 32x32 single buttons (HUD_Dir_Button*)
  kDirButtonB,
  kDirButtonX,
  kDirButtonY,
  kDirButtonRS,   // Judgment only
  kTriggerRT32,   // GitHub #223: Judgment's 32x32 Valour store trigger icons
  kTriggerLT32,
  kFont256G1,     // GitHub #223: Gears 1's button font sheet (Gears 2's cells + its directional RS / LS glyphs)
};
inline constexpr int kAtlasKindCount = 19;

struct PromptCell {
  const char* button;   // Canonical button name: "a", "b", "x", "y", "lb", "rb", "lt", "rt", "ls", "rs", "lspress",
                        // "rspress", "stickmove", "back", "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
                        // "dpad_up_down", "dpad_left_right", "dpad_all", "rs_up", "rs_down", "rs_left", "rs_right",
                        // "ls_down", "ls_left", "ls_right", "dpad_plain", "stick_plain"
  const char* cvar;     // Keybind cvar name (e.g. "keybind_a", "keybind_b", ..., "dpad_vertical", "dpad_horizontal",
                        // "move_left", "move_right")
  int box[4];           // [x0, y0, x1, y1] region in 1x guest coordinates to clear
  int draw_rect[4];     // [dx0, dy0, dx1, dy1] target rectangle in 1x guest coordinates where the new glyph/keycap is drawn
  int center[2];        // [cx, cy] seed point inside the original button body (used for in-place flood-fill or centering)
  bool clear_box;       // True if the entire `box` must be cleared (e.g. HUD 512x512 cells, Font 256x256 cells, and
                        // Chainsaw Duel B & Y cells that have 3 extra context buttons)
  bool in_place_pulse;  // True for Chainsaw Duel pulsing frames where flood-fill around `center` can size the glyph
                        // dynamically when guest bytes are decoded, falling back to `draw_rect` for static 4x overlays
};

// GitHub #223: which game draws a layout's texture (bits), so the compositor builds only the running game's atlases.
inline constexpr uint8_t kGears1 = 1, kGears2 = 2, kGears3 = 4, kJudgment = 8;
// The game bit for a title ID (0x4D5307D5 / 0x4D53082D / 0x4D5308AB / 0x4D530A26); 0 for any other title.
uint8_t GameBitForTitle(uint32_t title_id);

struct AtlasLayout {
  uint64_t hash;
  AtlasKind kind;
  uint32_t width;
  uint32_t height;
  std::span<const PromptCell> cells;
  uint8_t games;  // kGears1 | kGears2 | ...: the games whose packages hold this texture
  // How much of a cell's draw_rect the controller glyph fills. 0.85 leaves the font sheets' margin (UE3 font quads
  // clip tight against the cell); GitHub #223 layouts whose draw_rects are the game buttons' own measured size use
  // 1.0, so the glyph is as big as the button it replaces. Layouts of one AtlasKind must agree.
  float glyph_scale = 0.85f;
};

bool IsKnownPromptAtlas(uint64_t hash);
const AtlasLayout* GetAtlasLayout(uint64_t hash);
std::span<const AtlasLayout> GetAllAtlasLayouts();

}  // namespace xenos::renderer::prompt
