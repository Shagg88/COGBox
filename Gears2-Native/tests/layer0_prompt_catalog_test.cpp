// Layer 0 unit tests: prompt atlas catalog and coordinate mapping verification.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <xenos/renderer/prompt_atlas_catalog.h>

namespace prompt = xenos::renderer::prompt;

TEST_CASE("Prompt atlas catalog recognizes all known HUD, Font, and Chainsaw Duel hashes", "[layer0][prompt][catalog]") {
  const uint64_t kHudHashes[] = {
      0xa000624a174e08baULL,  // Gears 3 512x512 HUD action prompts
      0xce11e464b5064714ULL,  // Gears 2 512x512 HUD action prompts
  };
  const uint64_t kFontG3JHashes[] = {
      0xa52ee00be48fe5c5ULL,  // Gears 3 256x256 Font sheet swap_hash
      0x5d9324ddfea00fbfULL,  // Judgment 256x256 Font sheet swap_hash
      0xae5c68a9d7f7398cULL,  // Gears 3 256x256 Font sheet rex_hash
      0x2587228e1f70bb5fULL,  // Gears 3 256x256 Font sheet menu reload rex_hash
  };
  const uint64_t kFontG2Hashes[] = {
      0xa133822cba91c272ULL,  // Gears 2 256x256 Font sheet swap_hash
      0x86bf4f7c0ec59721ULL,  // Gears 2 256x256 Font sheet page rex_hash
  };
  const uint64_t kDuelHash = 0x30d01cc6c7ff5cd3ULL;  // Gears 3 / Judgment 256x256 Chainsaw Duel sheet

  for (uint64_t h : kHudHashes) {
    INFO("Checking HUD hash: 0x" << std::hex << h);
    CHECK(prompt::IsKnownPromptAtlas(h));
    const auto* layout = prompt::GetAtlasLayout(h);
    REQUIRE(layout != nullptr);
    CHECK(layout->hash == h);
    CHECK(layout->kind == prompt::AtlasKind::kHud512);
    CHECK(layout->width == 512);
    CHECK(layout->height == 512);
    CHECK(layout->cells.size() == 9);
  }

  for (uint64_t h : kFontG3JHashes) {
    INFO("Checking G3/J Font hash: 0x" << std::hex << h);
    CHECK(prompt::IsKnownPromptAtlas(h));
    const auto* layout = prompt::GetAtlasLayout(h);
    REQUIRE(layout != nullptr);
    CHECK(layout->hash == h);
    CHECK(layout->kind == prompt::AtlasKind::kFont256G3J);
    CHECK(layout->width == 256);
    CHECK(layout->height == 256);
    CHECK(layout->cells.size() == 33);
  }

  for (uint64_t h : kFontG2Hashes) {
    INFO("Checking G2 Font hash: 0x" << std::hex << h);
    CHECK(prompt::IsKnownPromptAtlas(h));
    const auto* layout = prompt::GetAtlasLayout(h);
    REQUIRE(layout != nullptr);
    CHECK(layout->hash == h);
    CHECK(layout->kind == prompt::AtlasKind::kFont256G2);
    CHECK(layout->width == 256);
    CHECK(layout->height == 256);
    CHECK(layout->cells.size() == 27);
  }

  {
    INFO("Checking Chainsaw Duel hash: 0x" << std::hex << kDuelHash);
    CHECK(prompt::IsKnownPromptAtlas(kDuelHash));
    const auto* layout = prompt::GetAtlasLayout(kDuelHash);
    REQUIRE(layout != nullptr);
    CHECK(layout->hash == kDuelHash);
    CHECK(layout->kind == prompt::AtlasKind::kChainsawDuel);
    CHECK(layout->width == 256);
    CHECK(layout->height == 256);
    CHECK(layout->cells.size() == 8);
  }

  // Unknown hashes must return false / nullptr.
  CHECK_FALSE(prompt::IsKnownPromptAtlas(0ULL));
  CHECK_FALSE(prompt::IsKnownPromptAtlas(0xdeadbeefcafebabeULL));
  CHECK(prompt::GetAtlasLayout(0ULL) == nullptr);
  CHECK(prompt::GetAtlasLayout(0xdeadbeefcafebabeULL) == nullptr);

  CHECK(prompt::GetAllAtlasLayouts().size() == 9);
}

TEST_CASE("Every cell in every prompt atlas layout has valid coordinates and metadata", "[layer0][prompt][catalog]") {
  const auto layouts = prompt::GetAllAtlasLayouts();
  REQUIRE_FALSE(layouts.empty());

  for (const auto& layout : layouts) {
    INFO("Atlas hash: 0x" << std::hex << layout.hash);
    REQUIRE_FALSE(layout.cells.empty());
    const int w = static_cast<int>(layout.width);
    const int h = static_cast<int>(layout.height);

    for (size_t i = 0; i < layout.cells.size(); ++i) {
      const auto& cell = layout.cells[i];
      INFO("Cell index: " << i);
      REQUIRE(cell.button != nullptr);
      REQUIRE(cell.cvar != nullptr);
      CHECK(std::strlen(cell.button) > 0);
      CHECK(std::strlen(cell.cvar) > 0);

      // box bounds: 0 <= x0 < x1 <= width, 0 <= y0 < y1 <= height
      CHECK(cell.box[0] >= 0);
      CHECK(cell.box[0] < cell.box[2]);
      CHECK(cell.box[2] <= w);
      CHECK(cell.box[1] >= 0);
      CHECK(cell.box[1] < cell.box[3]);
      CHECK(cell.box[3] <= h);

      // draw_rect bounds: 0 <= dx0 < dx1 <= width, 0 <= dy0 < dy1 <= height
      CHECK(cell.draw_rect[0] >= 0);
      CHECK(cell.draw_rect[0] < cell.draw_rect[2]);
      CHECK(cell.draw_rect[2] <= w);
      CHECK(cell.draw_rect[1] >= 0);
      CHECK(cell.draw_rect[1] < cell.draw_rect[3]);
      CHECK(cell.draw_rect[3] <= h);

      // center inside box
      CHECK(cell.center[0] >= cell.box[0]);
      CHECK(cell.center[0] <= cell.box[2]);
      CHECK(cell.center[1] >= cell.box[1]);
      CHECK(cell.center[1] <= cell.box[3]);
    }
  }
}

TEST_CASE("Chainsaw Duel layout includes B, LT, RT, and Y pulsing cells with clear_box on B and Y",
          "[layer0][prompt][catalog]") {
  const auto* duel = prompt::GetAtlasLayout(0x30d01cc6c7ff5cd3ULL);
  REQUIRE(duel != nullptr);
  CHECK(duel->kind == prompt::AtlasKind::kChainsawDuel);

  int count_b = 0;
  int count_lt = 0;
  int count_rt = 0;
  int count_y = 0;

  for (const auto& cell : duel->cells) {
    const std::string_view btn(cell.button);
    CHECK(cell.in_place_pulse);
    if (btn == "b") {
      ++count_b;
      CHECK(cell.clear_box);
      CHECK(std::string_view(cell.cvar) == "keybind_b");
    } else if (btn == "lt") {
      ++count_lt;
      CHECK_FALSE(cell.clear_box);
      CHECK(std::string_view(cell.cvar) == "keybind_left_trigger");
    } else if (btn == "rt") {
      ++count_rt;
      CHECK_FALSE(cell.clear_box);
      CHECK(std::string_view(cell.cvar) == "keybind_right_trigger");
    } else if (btn == "y") {
      ++count_y;
      CHECK(cell.clear_box);
      CHECK(std::string_view(cell.cvar) == "keybind_y");
    }
  }

  CHECK(count_b == 2);
  CHECK(count_lt == 2);
  CHECK(count_rt == 2);
  CHECK(count_y == 2);
}
