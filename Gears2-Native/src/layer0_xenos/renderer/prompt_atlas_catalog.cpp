// Layer 0 (game-free): catalog of known prompt texture atlases (512x512 HUD action prompts, 256x256 Gears 2 / 3 /
// Judgment button font sheets, and 256x256 Melee & Chainsaw Duel sheets) and their per-button cell coordinates.
#include <xenos/renderer/prompt_atlas_catalog.h>

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

// 2. 256x256 Gears 3 & Judgment Font Sheets.
constexpr PromptCell kFont256G3JCells[] = {
    // Face buttons
    {"a", "keybind_a", {200, 54, 227, 81}, {200, 54, 226, 80}, {212, 66}, true, false},
    {"b", "keybind_b", {227, 54, 254, 81}, {227, 54, 253, 80}, {239, 66}, true, false},
    {"x", "keybind_x", {196, 108, 221, 133}, {196, 108, 220, 132}, {207, 119}, true, false},
    {"y", "keybind_y", {222, 108, 248, 133}, {222, 108, 246, 132}, {233, 119}, true, false},
    // Triggers & Bumpers
    {"lt", "keybind_left_trigger", {31, 134, 56, 162}, {31, 134, 56, 162}, {43, 148}, true, false},
    {"rt", "keybind_right_trigger", {57, 134, 81, 162}, {57, 134, 81, 162}, {68, 148}, true, false},
    {"lb", "keybind_left_shoulder", {81, 139, 109, 157}, {81, 139, 109, 157}, {95, 148}, true, false},
    {"rb", "keybind_right_shoulder", {109, 139, 138, 157}, {109, 139, 138, 157}, {123, 148}, true, false},
    {"lt", "keybind_left_trigger", {203, 188, 229, 218}, {203, 188, 229, 218}, {214, 202}, true, false},
    {"rt", "keybind_right_trigger", {229, 188, 256, 218}, {229, 188, 256, 218}, {241, 202}, true, false},
    {"lb", "keybind_left_shoulder", {0, 219, 29, 238}, {0, 219, 29, 238}, {13, 228}, true, false},
    {"rb", "keybind_right_shoulder", {29, 219, 58, 238}, {29, 219, 58, 238}, {42, 228}, true, false},
    // Back & Start
    {"back", "keybind_back", {44, 55, 71, 78}, {44, 55, 71, 78}, {58, 66}, true, false},
    {"start", "keybind_start", {74, 55, 104, 78}, {74, 55, 104, 78}, {87, 66}, true, false},
    {"back", "keybind_back", {103, 55, 135, 78}, {103, 55, 135, 78}, {118, 66}, true, false},
    {"start", "keybind_start", {147, 55, 179, 78}, {147, 55, 179, 78}, {163, 66}, true, false},
    // D-Pad
    {"dpad_up", "keybind_dpad_up", {218, 28, 238, 51}, {218, 28, 238, 51}, {228, 39}, true, false},
    {"dpad_right", "keybind_dpad_right", {194, 30, 217, 51}, {194, 30, 217, 51}, {205, 40}, true, false},
    {"dpad_down", "keybind_dpad_down", {0, 57, 21, 80}, {0, 57, 21, 80}, {10, 68}, true, false},
    {"dpad_left", "keybind_dpad_left", {21, 57, 44, 78}, {21, 57, 44, 78}, {32, 67}, true, false},
    {"dpad_left", "keybind_dpad_left", {0, 195, 22, 214}, {0, 195, 22, 214}, {10, 204}, true, false},
    {"dpad_right", "keybind_dpad_right", {30, 195, 52, 214}, {30, 195, 52, 214}, {40, 204}, true, false},
    {"dpad_down", "keybind_dpad_down", {80, 195, 99, 217}, {80, 195, 99, 217}, {90, 205}, true, false},
    {"dpad_up_down", "dpad_vertical", {158, 188, 177, 215}, {158, 188, 177, 215}, {168, 202}, true, false},
    {"dpad_left_right", "dpad_horizontal", {177, 191, 202, 210}, {177, 191, 202, 210}, {189, 200}, true, false},
    {"dpad_all", "dpad_vertical", {207, 165, 226, 184}, {207, 165, 226, 184}, {216, 174}, true, false},
    {"dpad_up", "keybind_dpad_up", {232, 160, 251, 184}, {232, 160, 251, 184}, {241, 173}, true, false},
    // Sticks
    {"rs", "keybind_rstick_press", {0, 81, 23, 108}, {0, 81, 23, 108}, {10, 93}, true, false},
    {"ls", "keybind_lstick_press", {200, 83, 223, 106}, {200, 83, 223, 106}, {212, 93}, true, false},
    {"ls", "keybind_lstick_press", {223, 83, 248, 106}, {223, 83, 248, 106}, {234, 93}, true, false},
    {"rspress", "keybind_rstick_press", {80, 161, 109, 191}, {80, 161, 109, 191}, {94, 175}, true, false},
    {"lspress", "keybind_lstick_press", {143, 161, 172, 190}, {143, 161, 172, 190}, {157, 175}, true, false},
    {"stickmove", "move_left", {61, 216, 84, 242}, {61, 216, 84, 242}, {72, 229}, true, false},
};

// 3. 256x256 Gears 2 Font Sheet.
constexpr PromptCell kFont256G2Cells[] = {
    {"a", "keybind_a", {202, 55, 227, 80}, {202, 55, 227, 80}, {214, 67}, true, false},
    {"b", "keybind_b", {228, 55, 254, 80}, {228, 55, 254, 80}, {241, 67}, true, false},
    {"x", "keybind_x", {196, 109, 221, 136}, {196, 109, 221, 136}, {208, 122}, true, false},
    {"y", "keybind_y", {222, 109, 248, 136}, {222, 109, 248, 136}, {235, 122}, true, false},
    {"ls", "keybind_lstick_press", {200, 83, 223, 106}, {200, 83, 223, 106}, {211, 94}, true, false},
    {"ls", "keybind_lstick_press", {223, 83, 246, 106}, {223, 83, 246, 106}, {234, 94}, true, false},
    {"rs", "keybind_rstick_press", {0, 84, 21, 105}, {0, 84, 21, 105}, {10, 94}, true, false},
    {"lt", "keybind_left_trigger", {31, 134, 57, 164}, {31, 134, 57, 164}, {44, 149}, true, false},
    {"rt", "keybind_right_trigger", {57, 134, 81, 164}, {57, 134, 81, 164}, {69, 149}, true, false},
    {"lb", "keybind_left_shoulder", {81, 137, 109, 158}, {81, 137, 109, 158}, {95, 147}, true, false},
    {"rb", "keybind_right_shoulder", {109, 137, 138, 158}, {109, 137, 138, 158}, {123, 147}, true, false},
    {"lt", "keybind_left_trigger", {203, 189, 228, 219}, {203, 189, 228, 219}, {215, 204}, true, false},
    {"rt", "keybind_right_trigger", {228, 189, 255, 219}, {228, 189, 255, 219}, {241, 204}, true, false},
    {"lb", "keybind_left_shoulder", {0, 219, 29, 239}, {0, 219, 29, 239}, {14, 229}, true, false},
    {"rb", "keybind_right_shoulder", {29, 219, 57, 239}, {29, 219, 57, 239}, {43, 229}, true, false},
    {"back", "keybind_back", {45, 55, 72, 76}, {45, 55, 72, 76}, {58, 65}, true, false},
    {"start", "keybind_start", {75, 55, 102, 76}, {75, 55, 102, 76}, {88, 65}, true, false},
    {"back", "keybind_back", {106, 55, 136, 76}, {106, 55, 136, 76}, {121, 65}, true, false},
    {"start", "keybind_start", {149, 55, 179, 76}, {149, 55, 179, 76}, {164, 65}, true, false},
    {"dpad_up", "keybind_dpad_up", {233, 161, 251, 184}, {233, 161, 251, 184}, {242, 172}, true, false},
    {"dpad_left", "keybind_dpad_left", {3, 189, 24, 211}, {3, 189, 24, 211}, {13, 200}, true, false},
    {"dpad_right", "keybind_dpad_right", {29, 192, 52, 211}, {29, 192, 52, 211}, {40, 201}, true, false},
    {"dpad_down", "keybind_dpad_down", {81, 192, 99, 216}, {81, 192, 99, 216}, {90, 204}, true, false},
    {"rs", "move_right", {79, 168, 110, 191}, {79, 168, 110, 191}, {94, 179}, true, false},
    {"rspress", "keybind_rstick_press", {109, 164, 140, 191}, {109, 164, 140, 191}, {124, 177}, true, false},
    {"stickmove", "move_left", {141, 168, 172, 192}, {141, 168, 172, 192}, {156, 180}, true, false},
    {"lspress", "keybind_lstick_press", {172, 164, 203, 191}, {172, 164, 203, 191}, {187, 177}, true, false},
};

// 4. 256x256 Melee & Chainsaw Duel Sheet (Gears 3 / Judgment).
constexpr PromptCell kChainsawDuelCells[] = {
    {"b", "keybind_b", {80, 0, 127, 31}, {98, 1, 126, 29}, {112, 15}, true, true},
    {"b", "keybind_b", {80, 32, 127, 63}, {101, 35, 123, 57}, {112, 46}, true, true},
    {"lt", "keybind_left_trigger", {0, 128, 34, 166}, {3, 133, 31, 161}, {17, 147}, false, true},
    {"rt", "keybind_right_trigger", {50, 131, 80, 163}, {51, 133, 79, 161}, {65, 147}, false, true},
    {"lt", "keybind_left_trigger", {2, 168, 33, 201}, {6, 174, 28, 196}, {17, 185}, false, true},
    {"rt", "keybind_right_trigger", {47, 166, 83, 204}, {54, 174, 76, 196}, {65, 185}, false, true},
    {"y", "keybind_y", {2, 203, 38, 250}, {4, 207, 32, 235}, {18, 221}, true, true},
    {"y", "keybind_y", {42, 203, 75, 250}, {47, 211, 69, 233}, {58, 222}, true, true},
};

constexpr AtlasLayout kAtlasLayouts[] = {
    // 512x512 HUD Action Prompts
    {0xa000624a174e08baULL, AtlasKind::kHud512, 512, 512, kHud512Cells},
    {0xce11e464b5064714ULL, AtlasKind::kHud512, 512, 512, kHud512Cells},
    // 256x256 Gears 3 & Judgment Font Sheets
    {0xa52ee00be48fe5c5ULL, AtlasKind::kFont256G3J, 256, 256, kFont256G3JCells},
    {0x5d9324ddfea00fbfULL, AtlasKind::kFont256G3J, 256, 256, kFont256G3JCells},
    {0xae5c68a9d7f7398cULL, AtlasKind::kFont256G3J, 256, 256, kFont256G3JCells},
    {0x2587228e1f70bb5fULL, AtlasKind::kFont256G3J, 256, 256, kFont256G3JCells},
    // 256x256 Gears 2 Font Sheets
    {0xa133822cba91c272ULL, AtlasKind::kFont256G2, 256, 256, kFont256G2Cells},
    {0x86bf4f7c0ec59721ULL, AtlasKind::kFont256G2, 256, 256, kFont256G2Cells},
    // 256x256 Melee & Chainsaw Duel Sheet
    {0x30d01cc6c7ff5cd3ULL, AtlasKind::kChainsawDuel, 256, 256, kChainsawDuelCells},
};

}  // namespace

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
