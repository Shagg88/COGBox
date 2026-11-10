// Layer 0 unit tests: Kenney CC0 controller & mouse prompt glyph asset verification.
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

namespace {

std::filesystem::path FindGlyphsRoot() {
  const std::filesystem::path rel("assets/glyphs");
  if (std::filesystem::exists(rel)) {
    return rel;
  }
#ifdef _WIN32
  char exe_path[MAX_PATH] = {};
  if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH) > 0) {
    const auto exe_glyphs = std::filesystem::path(exe_path).parent_path() / "assets" / "glyphs";
    if (std::filesystem::exists(exe_glyphs)) {
      return exe_glyphs;
    }
  }
#endif
  return rel;
}

void VerifyPngFile(const std::filesystem::path& path) {
  INFO("Checking glyph PNG: " << path.string());
  REQUIRE(std::filesystem::exists(path));

  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  REQUIRE(bytes.size() > 24);

  // Verify PNG signature (89 50 4E 47 0D 0A 1A 0A).
  const uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  for (size_t i = 0; i < 8; ++i) {
    CHECK(bytes[i] == kPngSig[i]);
  }

  int width = 0;
  int height = 0;
  int channels_in_file = 0;
  stbi_uc* rgba = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels_in_file, 4);
  REQUIRE(rgba != nullptr);
  CHECK(width > 0);
  CHECK(height > 0);

  // Ensure at least one non-transparent pixel exists in the decoded RGBA buffer.
  bool has_visible_pixel = false;
  const size_t total_pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  for (size_t i = 0; i < total_pixels; ++i) {
    if (rgba[i * 4 + 3] > 0) {
      has_visible_pixel = true;
      break;
    }
  }
  CHECK(has_visible_pixel);
  stbi_image_free(rgba);
}

}  // namespace

TEST_CASE("Verify required Kenney glyph assets exist and load as valid PNGs", "[layer0][glyphs][assets]") {
  const std::filesystem::path root = FindGlyphsRoot();
  INFO("Glyphs root: " << root.string());

  CHECK(std::filesystem::exists(root / "LICENSE.txt"));

  const char* kProfiles[] = {"xbox", "playstation", "switch", "steamdeck", "steamcontroller"};
  const char* kCanonicalButtons[] = {
      "a",          "b",         "x",               "y",          "lb",        "rb",           "lt",       "rt",
      "ls",         "rs",        "lspress",         "rspress",    "stickmove", "back",         "start",    "dpad_up",
      "dpad_down",  "dpad_left", "dpad_right",      "dpad_up_down", "dpad_left_right", "dpad_all"};

  for (const char* profile : kProfiles) {
    for (const char* button : kCanonicalButtons) {
      const auto png_path = root / profile / (std::string(button) + ".png");
      VerifyPngFile(png_path);
    }
  }

  const char* kKeyboardMouseIcons[] = {
      "mouse_left", "mouse_right", "mouse_scroll", "mouse_side", "mouse_scroll_up", "mouse_scroll_down"};
  for (const char* icon : kKeyboardMouseIcons) {
    const auto png_path = root / "keyboard" / (std::string(icon) + ".png");
    VerifyPngFile(png_path);
  }
}
