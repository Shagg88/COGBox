// Layer 0 (game-free): catalog of known prompt texture atlases (512x512 HUD action prompts, 256x256 Gears 2 / 3 /
// Judgment button font sheets, and 256x256 Melee & Chainsaw Duel sheets) and their per-button cell coordinates.
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
#include <xenos/renderer/prompt_atlas_catalog.h>

#include <array>

namespace xenos::renderer::prompt {

namespace {

// 1. 512x512 HUD Action Prompts (Gears 2 & Gears 3).
// `draw_rect` is {cx - 14, cy - 14, cx + 14, cy + 14} (28x28 centered at `center`).
constexpr PromptCell kHud512Cells[] = {
    {"y", "keybind_y", {176, 314, 208, 376}, {177, 336, 205, 364}, {191, 350}, true, false},
    {"a", "keybind_a", {214, 314, 245, 356}, {215, 328, 243, 356}, {229, 342}, true, false},
    {"a", "keybind_a", {250, 314, 281, 357}, {251, 330, 279, 358}, {265, 344}, true, false},
    {"b", "keybind_b", {285, 314, 327, 345}, {298, 315, 326, 343}, {312, 329}, true, false},
    {"x", "keybind_x", {331, 314, 374, 345}, {331, 315, 359, 343}, {345, 329}, true, false},
    {"x", "keybind_x", {378, 314, 419, 345}, {377, 316, 405, 344}, {391, 330}, true, false},
    {"y", "keybind_y", {390, 423, 422, 462}, {391, 422, 419, 450}, {405, 436}, true, false},
    {"rs", "keybind_rstick_press", {426, 423, 459, 476}, {428, 445, 456, 473}, {442, 459}, true, false},
    {"rs", "keybind_rstick_press", {462, 423, 493, 476}, {463, 446, 491, 474}, {477, 460}, true, false},
};

// Font sheets (sections 2, 3, 3b): GitHub #223 - every cell's box and draw rect is its font character's own UV rect
// (StartU / StartV / USize / VSize from the game's Warfare_HUD.Xbox360_18pt Font object, texture 0): exactly the area
// the game shows for that button, so the glyph or key cap fills it the way the game's own art did, and no other
// character's texels are touched (the characters don't overlap). Gears 3 and Judgment have the same characters here.

// 2. 256x256 Gears 3 & Judgment Font Sheets.
constexpr PromptCell kFont256G3JCells[] = {
    // Face buttons
    {"a", "keybind_a", {200, 54, 226, 82}, {201, 55, 225, 81}, {213, 68}, true, false},
    {"b", "keybind_b", {226, 54, 254, 82}, {227, 55, 253, 81}, {240, 68}, true, false},
    {"x", "keybind_x", {195, 108, 221, 136}, {196, 109, 220, 135}, {208, 122}, true, false},
    {"y", "keybind_y", {221, 108, 247, 136}, {222, 109, 246, 135}, {234, 122}, true, false},
    // Triggers & Bumpers
    {"lt", "keybind_left_trigger", {32, 135, 56, 163}, {33, 136, 55, 162}, {44, 149}, true, false},
    {"rt", "keybind_right_trigger", {56, 135, 80, 163}, {57, 136, 79, 162}, {68, 149}, true, false},
    {"lb", "keybind_left_shoulder", {80, 135, 109, 163}, {81, 136, 108, 162}, {94, 149}, true, false},
    {"rb", "keybind_right_shoulder", {109, 135, 138, 163}, {110, 136, 137, 162}, {123, 149}, true, false},
    {"lt", "keybind_left_trigger", {203, 189, 227, 217}, {204, 190, 226, 216}, {215, 203}, true, false},
    {"rt", "keybind_right_trigger", {227, 189, 251, 217}, {228, 190, 250, 216}, {239, 203}, true, false},
    {"lb", "keybind_left_shoulder", {0, 216, 28, 244}, {0, 217, 27, 243}, {13, 230}, true, false},
    {"rb", "keybind_right_shoulder", {28, 216, 57, 244}, {29, 217, 56, 243}, {42, 230}, true, false},
    // Back & Start
    {"back", "keybind_back", {44, 54, 74, 82}, {45, 55, 73, 81}, {59, 68}, true, false},
    {"start", "keybind_start", {74, 54, 104, 82}, {75, 55, 103, 81}, {89, 68}, true, false},
    {"back", "keybind_back", {104, 55, 134, 82}, {105, 55, 133, 81}, {119, 68}, true, false},
    {"start", "keybind_start", {148, 54, 178, 82}, {149, 55, 177, 81}, {163, 68}, true, false},
    // D-Pad
    {"dpad_up", "keybind_dpad_up", {215, 27, 237, 55}, {216, 28, 236, 54}, {226, 41}, true, false},
    {"dpad_right", "keybind_dpad_right", {195, 27, 215, 55}, {196, 28, 214, 54}, {205, 41}, true, false},
    {"dpad_down", "keybind_dpad_down", {0, 54, 19, 82}, {0, 55, 18, 81}, {9, 68}, true, false},
    {"dpad_left", "keybind_dpad_left", {19, 54, 44, 82}, {20, 55, 43, 81}, {31, 68}, true, false},
    {"dpad_left", "keybind_dpad_left", {0, 189, 25, 217}, {0, 190, 24, 216}, {12, 203}, true, false},
    {"dpad_right", "keybind_dpad_right", {25, 189, 51, 217}, {26, 190, 50, 216}, {38, 203}, true, false},
    {"dpad_down", "keybind_dpad_down", {77, 189, 103, 217}, {78, 190, 102, 216}, {90, 203}, true, false},
    {"dpad_up_down", "dpad_vertical", {155, 189, 180, 217}, {156, 190, 180, 216}, {168, 203}, true, false},
    {"dpad_left_right", "dpad_horizontal", {180, 189, 204, 217}, {180, 190, 203, 216}, {191, 203}, true, false},
    {"dpad_all", "dpad_vertical", {203, 162, 229, 190}, {204, 163, 228, 189}, {216, 176}, true, false},
    {"dpad_up", "keybind_dpad_up", {229, 162, 255, 190}, {230, 163, 254, 189}, {242, 176}, true, false},
    // Sticks
    {"rs", "keybind_rstick_press", {0, 81, 19, 109}, {0, 82, 18, 108}, {9, 95}, true, false},
    {"ls", "keybind_lstick_press", {202, 81, 222, 109}, {203, 82, 221, 108}, {212, 95}, true, false},
    {"ls", "keybind_lstick_press", {222, 81, 242, 109}, {223, 82, 241, 108}, {232, 95}, true, false},
    {"rspress", "keybind_rstick_press", {79, 162, 110, 190}, {80, 163, 109, 189}, {94, 176}, true, false},
    {"lspress", "keybind_lstick_press", {141, 162, 172, 190}, {142, 163, 171, 189}, {156, 176}, true, false},
    {"stickmove", "move_left", {57, 216, 88, 244}, {58, 217, 87, 243}, {72, 230}, true, false},
};

// 3. 256x256 Gears 2 Font Sheet.
constexpr PromptCell kFont256G2Cells[] = {
    {"a", "keybind_a", {200, 54, 226, 82}, {201, 55, 225, 81}, {213, 68}, true, false},
    {"b", "keybind_b", {226, 54, 254, 82}, {227, 55, 253, 81}, {240, 68}, true, false},
    {"x", "keybind_x", {195, 108, 221, 136}, {196, 109, 220, 135}, {208, 122}, true, false},
    {"y", "keybind_y", {221, 108, 247, 136}, {222, 109, 246, 135}, {234, 122}, true, false},
    {"ls", "keybind_lstick_press", {202, 81, 222, 109}, {203, 82, 221, 108}, {212, 95}, true, false},
    {"ls", "keybind_lstick_press", {222, 81, 242, 109}, {223, 82, 241, 108}, {232, 95}, true, false},
    {"rs", "keybind_rstick_press", {0, 81, 19, 109}, {0, 82, 18, 108}, {9, 95}, true, false},
    {"lt", "keybind_left_trigger", {32, 135, 56, 163}, {33, 136, 55, 162}, {44, 149}, true, false},
    {"rt", "keybind_right_trigger", {56, 135, 80, 163}, {57, 136, 79, 162}, {68, 149}, true, false},
    {"lb", "keybind_left_shoulder", {80, 135, 109, 163}, {81, 136, 108, 162}, {94, 149}, true, false},
    {"rb", "keybind_right_shoulder", {109, 135, 138, 163}, {110, 136, 137, 162}, {123, 149}, true, false},
    {"lt", "keybind_left_trigger", {203, 189, 227, 217}, {204, 190, 226, 216}, {215, 203}, true, false},
    {"rt", "keybind_right_trigger", {227, 189, 251, 217}, {228, 190, 250, 216}, {239, 203}, true, false},
    {"lb", "keybind_left_shoulder", {0, 216, 28, 244}, {0, 217, 27, 243}, {13, 230}, true, false},
    {"rb", "keybind_right_shoulder", {28, 216, 57, 244}, {29, 217, 56, 243}, {42, 230}, true, false},
    {"back", "keybind_back", {44, 54, 74, 82}, {45, 55, 73, 81}, {59, 68}, true, false},
    {"start", "keybind_start", {74, 54, 104, 82}, {75, 55, 103, 81}, {89, 68}, true, false},
    {"back", "keybind_back", {104, 54, 134, 82}, {105, 55, 133, 81}, {119, 68}, true, false},
    {"start", "keybind_start", {148, 54, 178, 82}, {149, 55, 177, 81}, {163, 68}, true, false},
    {"dpad_up", "keybind_dpad_up", {229, 162, 255, 190}, {230, 163, 254, 189}, {242, 176}, true, false},
    {"dpad_right", "keybind_dpad_right", {25, 189, 51, 217}, {26, 190, 50, 216}, {38, 203}, true, false},
    {"dpad_down", "keybind_dpad_down", {77, 189, 103, 217}, {78, 190, 102, 216}, {90, 203}, true, false},
    {"rs", "move_right", {79, 162, 110, 190}, {80, 163, 109, 189}, {94, 176}, true, false},
    {"rspress", "keybind_rstick_press", {110, 162, 141, 190}, {111, 163, 140, 189}, {125, 176}, true, false},
    {"stickmove", "move_left", {141, 162, 172, 190}, {142, 163, 171, 189}, {156, 176}, true, false},
    {"lspress", "keybind_lstick_press", {172, 162, 203, 190}, {173, 163, 202, 189}, {187, 176}, true, false},
    // GitHub #223: the plain d-pad ring (nothing lit), and the two plain thumbsticks of the bottom row. The ring with
    // its arc at the top-right (3, 189) is a diagonal and stays the game's (it was mapped as d-pad left).
    {"dpad_plain", "dpad_any", {203, 162, 229, 190}, {204, 163, 228, 189}, {216, 176}, true, false},
    {"stick_plain", "move_left", {57, 216, 88, 244}, {58, 217, 87, 243}, {72, 230}, true, false},
    {"stick_plain", "move_left", {99, 216, 130, 244}, {100, 217, 129, 243}, {114, 230}, true, false},
};

// 3b. GitHub #223: Gears 1's button font sheet: Gears 2's cells, plus a row of RS and a row of LS glyphs with a
// direction arrow that Gears 2's sheet has rank chevrons in place of. The straight directions are mapped (Kenney has
// no diagonal stick glyphs).
constexpr PromptCell kFont256G1ExtraCells[] = {
    {"rs_up", "move_right", {19, 81, 39, 109}, {20, 82, 38, 108}, {29, 95}, true, false},
    {"rs_right", "move_right", {62, 81, 87, 109}, {63, 82, 86, 108}, {74, 95}, true, false},
    {"rs_down", "move_right", {110, 81, 130, 109}, {111, 82, 129, 108}, {120, 95}, true, false},
    {"rs_left", "move_right", {153, 81, 179, 109}, {154, 82, 178, 108}, {166, 95}, true, false},
    {"ls_right", "strafe_right", {22, 108, 47, 136}, {23, 109, 46, 135}, {34, 122}, true, false},
    {"ls_down", "move_back", {70, 108, 90, 136}, {71, 109, 89, 135}, {80, 122}, true, false},
    {"ls_left", "strafe_left", {113, 108, 139, 136}, {114, 109, 138, 135}, {126, 122}, true, false},
    // Gears 1's d-pad left ring (arc on the left); Gears 2's sheet has a rotate icon there.
    {"dpad_left", "keybind_dpad_left", {129, 189, 155, 217}, {130, 190, 154, 216}, {142, 203}, true, false},
};

template <size_t N, size_t M>
constexpr std::array<PromptCell, N + M> JoinCells(const PromptCell (&a)[N], const PromptCell (&b)[M]) {
  std::array<PromptCell, N + M> out{};
  for (size_t i = 0; i < N; ++i) out[i] = a[i];
  for (size_t i = 0; i < M; ++i) out[N + i] = b[i];
  return out;
}
// Gears 1's own copy of Gears 2's cells, fitted to Gears 1's font characters (its B is 2 texels narrower).
constexpr PromptCell kFont256G1BaseCells[] = {
    {"a", "keybind_a", {200, 54, 226, 82}, {201, 55, 225, 81}, {213, 68}, true, false},
    {"b", "keybind_b", {226, 54, 252, 82}, {227, 55, 251, 81}, {239, 68}, true, false},
    {"x", "keybind_x", {195, 108, 221, 136}, {196, 109, 220, 135}, {208, 122}, true, false},
    {"y", "keybind_y", {221, 108, 247, 136}, {222, 109, 246, 135}, {234, 122}, true, false},
    {"ls", "keybind_lstick_press", {202, 81, 222, 109}, {203, 82, 221, 108}, {212, 95}, true, false},
    {"ls", "keybind_lstick_press", {222, 81, 242, 109}, {223, 82, 241, 108}, {232, 95}, true, false},
    {"rs", "keybind_rstick_press", {0, 81, 19, 109}, {0, 82, 18, 108}, {9, 95}, true, false},
    {"lt", "keybind_left_trigger", {32, 135, 56, 163}, {33, 136, 55, 162}, {44, 149}, true, false},
    {"rt", "keybind_right_trigger", {56, 135, 80, 163}, {57, 136, 79, 162}, {68, 149}, true, false},
    {"lb", "keybind_left_shoulder", {80, 135, 109, 163}, {81, 136, 108, 162}, {94, 149}, true, false},
    {"rb", "keybind_right_shoulder", {109, 135, 138, 163}, {110, 136, 137, 162}, {123, 149}, true, false},
    {"lt", "keybind_left_trigger", {203, 189, 227, 217}, {204, 190, 226, 216}, {215, 203}, true, false},
    {"rt", "keybind_right_trigger", {227, 189, 251, 217}, {228, 190, 250, 216}, {239, 203}, true, false},
    {"lb", "keybind_left_shoulder", {0, 216, 28, 244}, {0, 217, 27, 243}, {13, 230}, true, false},
    {"rb", "keybind_right_shoulder", {28, 216, 57, 244}, {29, 217, 56, 243}, {42, 230}, true, false},
    {"back", "keybind_back", {44, 54, 74, 82}, {45, 55, 73, 81}, {59, 68}, true, false},
    {"start", "keybind_start", {74, 54, 104, 82}, {75, 55, 103, 81}, {89, 68}, true, false},
    {"back", "keybind_back", {104, 54, 134, 82}, {105, 55, 133, 81}, {119, 68}, true, false},
    {"start", "keybind_start", {148, 54, 178, 82}, {149, 55, 177, 81}, {163, 68}, true, false},
    {"dpad_up", "keybind_dpad_up", {229, 162, 255, 190}, {230, 163, 254, 189}, {242, 176}, true, false},
    {"dpad_right", "keybind_dpad_right", {25, 189, 51, 217}, {26, 190, 50, 216}, {38, 203}, true, false},
    {"dpad_down", "keybind_dpad_down", {77, 189, 103, 217}, {78, 190, 102, 216}, {90, 203}, true, false},
    {"rs", "move_right", {79, 162, 110, 190}, {80, 163, 109, 189}, {94, 176}, true, false},
    {"rspress", "keybind_rstick_press", {110, 162, 141, 190}, {111, 163, 140, 189}, {125, 176}, true, false},
    {"stickmove", "move_left", {141, 162, 172, 190}, {142, 163, 171, 189}, {156, 176}, true, false},
    {"lspress", "keybind_lstick_press", {172, 162, 203, 190}, {173, 163, 202, 189}, {187, 176}, true, false},
    {"dpad_plain", "dpad_any", {203, 162, 229, 190}, {204, 163, 228, 189}, {216, 176}, true, false},
    {"stick_plain", "move_left", {57, 216, 88, 244}, {58, 217, 87, 243}, {72, 230}, true, false},
    {"stick_plain", "move_left", {99, 216, 130, 244}, {100, 217, 129, 243}, {114, 230}, true, false},
};
constexpr auto kFont256G1Cells = JoinCells(kFont256G1BaseCells, kFont256G1ExtraCells);

// 4. 256x256 Melee & Chainsaw Duel Sheet (Gears 3 / Judgment).
// GitHub #223: draw rects are the buttons' own size (alpha > 60) on the texture decoded from the game's package, so
// the pulse frames alternate as the game's do - big LT with small RT on top, small LT with big RT below; B and Y
// shrink between their two frames. LT / RT boxes are now cleared whole (the old ones only cleared the draw rect,
// leaving the bigger frames' rims); the top LT box starts below the chainsaw icon (its art reaches y = 130).
constexpr PromptCell kChainsawDuelCells[] = {
    {"b", "keybind_b", {80, 0, 128, 32}, {99, 1, 127, 30}, {112, 15}, true, true},
    {"b", "keybind_b", {80, 32, 128, 64}, {99, 34, 126, 60}, {112, 46}, true, true},
    {"lt", "keybind_left_trigger", {0, 131, 35, 167}, {3, 131, 32, 164}, {17, 147}, true, true},
    {"rt", "keybind_right_trigger", {50, 131, 81, 164}, {53, 134, 78, 161}, {65, 147}, true, true},
    {"lt", "keybind_left_trigger", {2, 168, 34, 202}, {5, 172, 30, 199}, {17, 185}, true, true},
    {"rt", "keybind_right_trigger", {47, 166, 82, 205}, {51, 169, 80, 202}, {65, 185}, true, true},  // figure at x 83
    {"y", "keybind_y", {2, 203, 38, 250}, {5, 210, 34, 240}, {18, 221}, true, true},
    {"y", "keybind_y", {42, 203, 75, 250}, {45, 211, 70, 240}, {58, 222}, true, true},
};

// 5. GitHub #214: Gears 1's 512x512 HUD action sheet (cover / roadie-run icons). Its Y, A, A, B, X, X buttons sit
// exactly where Gears 2's do (decoded from the 0.4.6 texture dump); the lower Y / RS cells of the Gears 2 sheet are a
// camera and a weapon icon in Gears 1, so they are left out.
constexpr PromptCell kHud512G1Cells[] = {
    {"y", "keybind_y", {176, 314, 208, 376}, {177, 336, 205, 364}, {191, 350}, true, false},
    {"a", "keybind_a", {214, 314, 245, 356}, {215, 328, 243, 356}, {229, 342}, true, false},
    {"a", "keybind_a", {250, 314, 281, 357}, {251, 330, 279, 358}, {265, 344}, true, false},
    {"b", "keybind_b", {285, 314, 327, 345}, {298, 315, 326, 343}, {312, 329}, true, false},
    {"x", "keybind_x", {331, 314, 374, 345}, {331, 315, 359, 343}, {345, 329}, true, false},
    {"x", "keybind_x", {378, 314, 419, 345}, {377, 316, 405, 344}, {391, 330}, true, false},
};

// 6. GitHub #223: Gears 2's 128x256 chainsaw sheet (Warfare_HUD.HUD.HUD_ActionIcons2: chainsaw an object, chainsaw
// duels). Its B and LT / RT cells sit where the Gears 3 duel sheet's do (no Y cells). Draw rects are each button's
// own size, so the pulse frames alternate like the game's (big LT + small RT, then small LT + big RT); boxes take
// all of a button's art (alpha > 4) and are cleared whole. Measured on the texture decoded from the game's package.
constexpr PromptCell kChainsawG2Cells[] = {
    {"b", "keybind_b", {80, 0, 128, 32}, {99, 2, 126, 30}, {112, 15}, true, true},
    {"b", "keybind_b", {80, 32, 128, 64}, {102, 36, 123, 57}, {112, 46}, true, true},
    {"lt", "keybind_left_trigger", {0, 131, 35, 167}, {3, 131, 32, 164}, {17, 147}, true, true},
    {"rt", "keybind_right_trigger", {50, 131, 81, 164}, {53, 134, 78, 161}, {65, 147}, true, true},
    {"lt", "keybind_left_trigger", {2, 168, 34, 202}, {6, 172, 30, 199}, {17, 185}, true, true},
    {"rt", "keybind_right_trigger", {47, 166, 84, 205}, {51, 169, 80, 202}, {65, 185}, true, true},
};

// 7. GitHub #223: the 512x512 weapon icon sheet (Warfare_HUD.HUD.Gears_WeaponIcons; Gears 2, 3 and Judgment each
// have their own copy, button art at the same place). Its button art: two LS press frames (stick with a press arrow),
// a left stick between left / right arrows, and LT / RT. The boxes take the game's arrows too: the lspress /
// stickmove glyphs bring their own.
constexpr PromptCell kWeaponIconsCells[] = {
    {"lspress", "keybind_lstick_press", {223, 267, 259, 325}, {225, 270, 257, 322}, {241, 306}, true, false},
    {"lspress", "keybind_lstick_press", {259, 267, 294, 325}, {261, 270, 291, 322}, {276, 307}, true, false},
    {"stickmove", "move_left", {144, 356, 220, 389}, {146, 357, 217, 387}, {182, 372}, true, false},
    {"lt", "keybind_left_trigger", {62, 378, 94, 413}, {64, 379, 93, 412}, {78, 395}, true, false},
    {"rt", "keybind_right_trigger", {99, 378, 132, 413}, {101, 379, 130, 412}, {115, 395}, true, false},
};

// 8. GitHub #223: Gears 1's 512x512 HUD-all-texture: Y (with its press arrow), A, A, X, B as flattened ovals with
// their dot rings. Boxes take the dots and arrow; the glyph goes on the button body (the second A is the smaller
// pulse frame).
constexpr PromptCell kHudAllG1Cells[] = {
    {"y", "keybind_y", {144, 146, 192, 196}, {155, 166, 178, 181}, {167, 173}, true, false},
    {"a", "keybind_a", {8, 235, 43, 265}, {10, 239, 40, 262}, {26, 251}, true, false},
    {"a", "keybind_a", {10, 265, 41, 294}, {13, 275, 37, 291}, {25, 283}, true, false},
    {"x", "keybind_x", {2, 297, 57, 319}, {3, 299, 34, 317}, {19, 308}, true, false},
    {"b", "keybind_b", {3, 321, 58, 346}, {26, 324, 56, 343}, {41, 333}, true, false},
};

// 9. GitHub #223: Warfare_HUD.Gears3_WeaponIcons: RB in Gears 3; RB and LB in Judgment (the two copies differ by a
// pixel, and Gears 3's has nothing where Judgment's LB is). LB is drawn the size of RB, as the game's two are.
constexpr PromptCell kWeaponIconsG3Cells[] = {
    {"rb", "keybind_right_shoulder", {288, 240, 324, 270}, {291, 244, 321, 266}, {306, 255}, true, false},
};
constexpr PromptCell kWeaponIconsJCells[] = {
    {"rb", "keybind_right_shoulder", {287, 241, 324, 271}, {290, 245, 320, 267}, {305, 256}, true, false},
    {"lb", "keybind_left_shoulder", {336, 242, 370, 268}, {338, 245, 368, 267}, {353, 256}, true, false},
};

// 10. GitHub #223: Judgment's 104x104 Warfare_HUD.dpad_buttons: four d-pads, each with one direction lit and an
// arrow; the boxes take the arrows (the dpad_* glyphs show the direction).
constexpr PromptCell kDpadJCells[] = {
    {"dpad_right", "keybind_dpad_right", {1, 3, 53, 45}, {3, 5, 42, 43}, {22, 24}, true, false},
    {"dpad_up", "keybind_dpad_up", {58, 0, 100, 51}, {60, 11, 98, 49}, {79, 30}, true, false},
    {"dpad_left", "keybind_dpad_left", {1, 56, 53, 99}, {13, 58, 51, 97}, {32, 77}, true, false},
    {"dpad_down", "keybind_dpad_down", {58, 52, 100, 104}, {60, 54, 98, 92}, {79, 73}, true, false},
};

// 11. GitHub #223: single-button 32x32 textures (Gears 3 / Judgment HUD_Dir_Button*, Judgment's Valour store
// trigger icons): the whole texture is the button.
constexpr PromptCell kDirButtonACells[] = {{"a", "keybind_a", {0, 0, 32, 32}, {3, 4, 29, 29}, {16, 16}, true, false}};
constexpr PromptCell kDirButtonBCells[] = {{"b", "keybind_b", {0, 0, 32, 32}, {3, 4, 29, 29}, {16, 16}, true, false}};
constexpr PromptCell kDirButtonXCells[] = {{"x", "keybind_x", {0, 0, 32, 32}, {3, 4, 29, 29}, {16, 16}, true, false}};
constexpr PromptCell kDirButtonYCells[] = {{"y", "keybind_y", {0, 0, 32, 32}, {3, 4, 29, 29}, {16, 16}, true, false}};
constexpr PromptCell kDirButtonRSCells[] = {
    {"rs", "keybind_rstick_press", {0, 0, 32, 32}, {3, 4, 29, 29}, {16, 16}, true, false}};
constexpr PromptCell kTriggerRT32Cells[] = {
    {"rt", "keybind_right_trigger", {0, 0, 32, 32}, {2, 1, 30, 32}, {16, 16}, true, false}};
constexpr PromptCell kTriggerLT32Cells[] = {
    {"lt", "keybind_left_trigger", {0, 0, 32, 32}, {1, 1, 28, 31}, {16, 16}, true, false}};

constexpr AtlasLayout kAtlasLayouts[] = {
    // 512x512 HUD Action Prompts
    {0xa000624a174e08baULL, AtlasKind::kHud512, 512, 512, kHud512Cells, kGears3 | kJudgment},
    {0xce11e464b5064714ULL, AtlasKind::kHud512, 512, 512, kHud512Cells, kGears2},
    // 256x256 Gears 3 & Judgment Font Sheets
    {0xa52ee00be48fe5c5ULL, AtlasKind::kFont256G3J, 256, 256, kFont256G3JCells, kGears3},
    {0x5d9324ddfea00fbfULL, AtlasKind::kFont256G3J, 256, 256, kFont256G3JCells, kJudgment},
    {0xae5c68a9d7f7398cULL, AtlasKind::kFont256G3J, 256, 256, kFont256G3JCells, kGears3},
    {0x2587228e1f70bb5fULL, AtlasKind::kFont256G3J, 256, 256, kFont256G3JCells, kGears3},
    // 256x256 Gears 2 Font Sheets
    {0xa133822cba91c272ULL, AtlasKind::kFont256G2, 256, 256, kFont256G2Cells, kGears2},
    {0x86bf4f7c0ec59721ULL, AtlasKind::kFont256G2, 256, 256, kFont256G2Cells, kGears2},
    // GitHub #214: Gears 1's button font sheet (DXT5) - Gears 2's cells; #223: + its directional stick glyphs
    {0x8c33362ad83f2f11ULL, AtlasKind::kFont256G1, 256, 256, kFont256G1Cells, kGears1},
    // 256x256 Melee & Chainsaw Duel Sheet
    {0x30d01cc6c7ff5cd3ULL, AtlasKind::kChainsawDuel, 256, 256, kChainsawDuelCells, kGears3 | kJudgment, 1.0f},
    // GitHub #214: Gears 1's 512x512 HUD action sheet (DXT5)
    {0x49e274ee709ac41dULL, AtlasKind::kHud512G1, 512, 512, kHud512G1Cells, kGears1},
    // GitHub #223: Gears 2's chainsaw sheet and weapon icon sheet (DXT5; swap hashes = XXH3 of the base level)
    {0x64cd4013fef17653ULL, AtlasKind::kChainsawG2, 128, 256, kChainsawG2Cells, kGears2, 1.0f},
    {0xb9176ddf16f0cf4bULL, AtlasKind::kWeaponIcons, 512, 512, kWeaponIconsCells, kGears2, 1.0f},
    // GitHub #223: the other games' sheets (DXT5; swap hashes). The 32x32 and 104x104 textures are stored padded to a
    // whole 32x32-block tile, the same 16 KB the cache hashes.
    {0xd70fe15ad0904b0eULL, AtlasKind::kWeaponIcons, 512, 512, kWeaponIconsCells, kGears3, 1.0f},
    {0x900480dd0bd87a5fULL, AtlasKind::kWeaponIcons, 512, 512, kWeaponIconsCells, kJudgment, 1.0f},
    {0x656065574df33027ULL, AtlasKind::kHudAllG1, 512, 512, kHudAllG1Cells, kGears1, 1.0f},
    {0xb82bdd577a969fa2ULL, AtlasKind::kWeaponIconsG3, 512, 512, kWeaponIconsG3Cells, kGears3, 1.0f},
    {0x57a224f414d97aa5ULL, AtlasKind::kWeaponIconsJ, 512, 512, kWeaponIconsJCells, kJudgment, 1.0f},
    {0xe44da4591e95fcd3ULL, AtlasKind::kDpadJ, 104, 104, kDpadJCells, kJudgment, 1.0f},
    {0x522e6fd2f52ee997ULL, AtlasKind::kDirButtonA, 32, 32, kDirButtonACells, kGears3 | kJudgment, 1.0f},
    {0x472bc22b620a3671ULL, AtlasKind::kDirButtonB, 32, 32, kDirButtonBCells, kGears3 | kJudgment, 1.0f},
    {0xa89c2617206c928cULL, AtlasKind::kDirButtonX, 32, 32, kDirButtonXCells, kGears3 | kJudgment, 1.0f},
    {0x13d687fed02db98cULL, AtlasKind::kDirButtonY, 32, 32, kDirButtonYCells, kGears3 | kJudgment, 1.0f},
    {0x1d2e87d9580cdf06ULL, AtlasKind::kDirButtonRS, 32, 32, kDirButtonRSCells, kJudgment, 1.0f},
    {0x6973365a68ba6ee2ULL, AtlasKind::kTriggerRT32, 32, 32, kTriggerRT32Cells, kJudgment, 1.0f},
    {0xf9a7d5b28b365f11ULL, AtlasKind::kTriggerLT32, 32, 32, kTriggerLT32Cells, kJudgment, 1.0f},
};

}  // namespace

uint8_t GameBitForTitle(uint32_t title_id) {
  switch (title_id) {
    case 0x4D5307D5: return kGears1;
    case 0x4D53082D: return kGears2;
    case 0x4D5308AB: return kGears3;
    case 0x4D530A26: return kJudgment;
    default: return 0;
  }
}

std::span<const AtlasLayout> GetAllAtlasLayouts() {
  return kAtlasLayouts;
}

const AtlasLayout* GetAtlasLayout(uint64_t hash) {
  for (const auto& layout : kAtlasLayouts) {
    if (layout.hash == hash) {
      return &layout;
    }
  }
  return nullptr;
}

bool IsKnownPromptAtlas(uint64_t hash) {
  return GetAtlasLayout(hash) != nullptr;
}

}  // namespace xenos::renderer::prompt
