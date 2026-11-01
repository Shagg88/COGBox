// Layer 0 (game-free): catalog of known prompt texture atlases (512x512 HUD action prompts, 256x256 Gears 2 / 3 /
// Judgment button font sheets, and 256x256 Melee & Chainsaw Duel sheets) and their per-button cell coordinates.
#pragma once

#include <cstdint>
#include <span>

namespace xenos::renderer::prompt {

enum class AtlasKind : uint8_t {
  kHud512,        // 512x512 HUD action prompts (A, B, X, Y, RS)
  kFont256G3J,    // 256x256 Gears 3 & Judgment button font sheet
  kFont256G2,     // 256x256 Gears 2 button font sheet
  kChainsawDuel,  // 256x256 Melee & Chainsaw Duel sheet (B mash, LT, RT, Y)
};

struct PromptCell {
  const char* button;   // Canonical button name: "a", "b", "x", "y", "lb", "rb", "lt", "rt", "ls", "rs", "lspress",
                        // "rspress", "stickmove", "back", "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
                        // "dpad_up_down", "dpad_left_right", "dpad_all", "move_left", "move_right"
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

struct AtlasLayout {
  uint64_t hash;
  AtlasKind kind;
  uint32_t width;
  uint32_t height;
  std::span<const PromptCell> cells;
};

bool IsKnownPromptAtlas(uint64_t hash);
const AtlasLayout* GetAtlasLayout(uint64_t hash);
std::span<const AtlasLayout> GetAllAtlasLayouts();

}  // namespace xenos::renderer::prompt
