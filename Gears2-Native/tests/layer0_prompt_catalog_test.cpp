// Layer 0 unit tests: prompt atlas catalog and coordinate mapping verification.
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
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
    CHECK(layout->cells.size() == 29);  // GitHub #223: - wrong d-pad left, + plain d-pad / sticks
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

  CHECK(prompt::GetAllAtlasLayouts().size() == 26);  // GitHub #214: + Gears 1's font and HUD sheets; #223: + 15
}

TEST_CASE("GitHub #223: Gears 2's chainsaw sheet and weapon icon sheet are cataloged", "[layer0][prompt][catalog]") {
  const auto* saw = prompt::GetAtlasLayout(0x64cd4013fef17653ULL);  // Warfare_HUD.HUD.HUD_ActionIcons2
  REQUIRE(saw != nullptr);
  CHECK(saw->kind == prompt::AtlasKind::kChainsawG2);
  CHECK(saw->width == 128);
  CHECK(saw->height == 256);
  REQUIRE(saw->cells.size() == 6);
  // Alternating pulse frames, as on the game's sheet.
  auto area = [](const prompt::PromptCell& c) {
    return (c.draw_rect[2] - c.draw_rect[0]) * (c.draw_rect[3] - c.draw_rect[1]);
  };
  CHECK(area(saw->cells[2]) > area(saw->cells[3]));
  CHECK(area(saw->cells[4]) < area(saw->cells[5]));
  int b = 0, lt = 0, rt = 0;
  for (const auto& cell : saw->cells) {
    const std::string_view btn = cell.button;
    b += btn == "b";
    lt += btn == "lt";
    rt += btn == "rt";
    CHECK(cell.clear_box);  // the whole pulse frame goes, not just the draw rect
  }
  CHECK(b == 2);
  CHECK(lt == 2);
  CHECK(rt == 2);

  const auto* icons = prompt::GetAtlasLayout(0xb9176ddf16f0cf4bULL);  // Warfare_HUD.HUD.Gears_WeaponIcons
  REQUIRE(icons != nullptr);
  CHECK(icons->kind == prompt::AtlasKind::kWeaponIcons);
  CHECK(icons->width == 512);
  CHECK(icons->height == 512);
  REQUIRE(icons->cells.size() == 5);
  CHECK(std::string_view(icons->cells[0].button) == "lspress");
  CHECK(std::string_view(icons->cells[2].button) == "stickmove");
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

TEST_CASE("Chainsaw Duel layout includes B, LT, RT, and Y pulsing cells, every box cleared whole",
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
      CHECK(cell.clear_box);  // GitHub #223: the bigger pulse frame's rim went otherwise
      CHECK(std::string_view(cell.cvar) == "keybind_left_trigger");
    } else if (btn == "rt") {
      ++count_rt;
      CHECK(cell.clear_box);
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
  // GitHub #223: the pulse frames alternate like the game's - big LT with small RT, then small LT with big RT.
  auto area = [](const prompt::PromptCell& c) {
    return (c.draw_rect[2] - c.draw_rect[0]) * (c.draw_rect[3] - c.draw_rect[1]);
  };
  REQUIRE(duel->cells.size() == 8);
  CHECK(area(duel->cells[2]) > area(duel->cells[3]));  // top row: LT big, RT small
  CHECK(area(duel->cells[4]) < area(duel->cells[5]));  // bottom row: LT small, RT big
}

TEST_CASE("GitHub #223: the other games' extra prompt textures are cataloged", "[layer0][prompt][catalog]") {
  struct Want {
    uint64_t hash;
    prompt::AtlasKind kind;
    uint32_t w, h;
    size_t cells;
  };
  const Want wants[] = {
      {0xd70fe15ad0904b0eULL, prompt::AtlasKind::kWeaponIcons, 512, 512, 5},    // Gears 3 Gears_WeaponIcons
      {0x900480dd0bd87a5fULL, prompt::AtlasKind::kWeaponIcons, 512, 512, 5},    // Judgment Gears_WeaponIcons
      {0x656065574df33027ULL, prompt::AtlasKind::kHudAllG1, 512, 512, 5},       // Gears 1 HUD-all-texture
      {0xb82bdd577a969fa2ULL, prompt::AtlasKind::kWeaponIconsG3, 512, 512, 1},  // Gears 3 Gears3_WeaponIcons (RB)
      {0x57a224f414d97aa5ULL, prompt::AtlasKind::kWeaponIconsJ, 512, 512, 2},   // Judgment Gears3_WeaponIcons
      {0xe44da4591e95fcd3ULL, prompt::AtlasKind::kDpadJ, 104, 104, 4},          // Judgment dpad_buttons
      {0x522e6fd2f52ee997ULL, prompt::AtlasKind::kDirButtonA, 32, 32, 1},
      {0x472bc22b620a3671ULL, prompt::AtlasKind::kDirButtonB, 32, 32, 1},
      {0xa89c2617206c928cULL, prompt::AtlasKind::kDirButtonX, 32, 32, 1},
      {0x13d687fed02db98cULL, prompt::AtlasKind::kDirButtonY, 32, 32, 1},
      {0x1d2e87d9580cdf06ULL, prompt::AtlasKind::kDirButtonRS, 32, 32, 1},
      {0x6973365a68ba6ee2ULL, prompt::AtlasKind::kTriggerRT32, 32, 32, 1},
      {0xf9a7d5b28b365f11ULL, prompt::AtlasKind::kTriggerLT32, 32, 32, 1},
  };
  for (const Want& w : wants) {
    INFO("hash 0x" << std::hex << w.hash);
    const auto* layout = prompt::GetAtlasLayout(w.hash);
    REQUIRE(layout != nullptr);
    CHECK(layout->kind == w.kind);
    CHECK(layout->width == w.w);
    CHECK(layout->height == w.h);
    CHECK(layout->cells.size() == w.cells);
  }
  // Gears 1's font: Gears 2's cells plus its seven straight-direction stick glyphs.
  const auto* g1font = prompt::GetAtlasLayout(0x8c33362ad83f2f11ULL);
  REQUIRE(g1font != nullptr);
  CHECK(g1font->kind == prompt::AtlasKind::kFont256G1);
  CHECK(g1font->cells.size() == prompt::GetAtlasLayout(0xa133822cba91c272ULL)->cells.size() + 8);
  // Layouts of one kind share one overlay, so they must agree on how big the glyph is drawn.
  for (const auto& a : prompt::GetAllAtlasLayouts()) {
    for (const auto& b : prompt::GetAllAtlasLayouts()) {
      if (a.kind == b.kind) CHECK(a.glyph_scale == b.glyph_scale);
    }
  }
  // Every kind has a layout, so the compositor builds an overlay for each.
  for (int k = 0; k < prompt::kAtlasKindCount; ++k) {
    bool found = false;
    for (const auto& layout : prompt::GetAllAtlasLayouts()) found |= static_cast<int>(layout.kind) == k;
    INFO("kind " << k);
    CHECK(found);
  }
}

TEST_CASE("GitHub #223: every prompt layout names the games that draw it", "[layer0][prompt][catalog]") {
  for (const auto& layout : prompt::GetAllAtlasLayouts()) {
    INFO("hash 0x" << std::hex << layout.hash);
    CHECK(layout.games != 0);
    CHECK((layout.games & ~(prompt::kGears1 | prompt::kGears2 | prompt::kGears3 | prompt::kJudgment)) == 0);
  }
  CHECK(prompt::GameBitForTitle(0x4D5307D5) == prompt::kGears1);
  CHECK(prompt::GameBitForTitle(0x4D53082D) == prompt::kGears2);
  CHECK(prompt::GameBitForTitle(0x4D5308AB) == prompt::kGears3);
  CHECK(prompt::GameBitForTitle(0x4D530A26) == prompt::kJudgment);
  CHECK(prompt::GameBitForTitle(0) == 0);
}

TEST_CASE("rc6: Gears 2's 128x256 chainsaw sheet has B, LT and RT cells inside the sheet", "[layer0][prompt][catalog]") {
  const auto* g2 = prompt::GetAtlasLayout(0x64cd4013fef17653ULL);
  REQUIRE(g2 != nullptr);
  CHECK(g2->kind == prompt::AtlasKind::kChainsawDuelG2);
  CHECK(g2->width == 128);
  CHECK(g2->height == 256);
  REQUIRE(g2->cells.size() == 6);
  int b = 0;
  for (const auto& c : g2->cells) {
    if (std::string(c.button) == "b") ++b;
    CHECK(std::string(c.button) != "y");
    CHECK(c.box[0] >= 0);
    CHECK(c.box[2] <= 128);
    CHECK(c.box[3] <= 256);
    CHECK(c.draw_rect[0] >= c.box[0]);
    CHECK(c.draw_rect[2] <= c.box[2]);
  }
  CHECK(b == 2);
}
