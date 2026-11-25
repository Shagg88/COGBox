// ue3/gowj_prompt_height_package_patch.h: in split screen player 2's bottom action prompts sit at player 1's height
// (GitHub #420, task 15b). Synthetic buffers only.
#include <catch2/catch_test_macros.hpp>

#include <ue3/gowj_prompt_height_package_patch.h>
#include <ue3/package_patch.h>

namespace {
// GearGame.xxx decompressed (94,780,700 bytes): GearHUD_Base.DrawActionInfo's
// `if (!PC.IsPrimaryPlayer() && PC.IsCoop())` = JumpIfNot 0x0197, Not, context ...
constexpr uint64_t kIf = 0xEAEBF6;
std::vector<uint8_t> Original() {
  std::vector<uint8_t> d(94780700, 0xAA);
  const uint8_t b[6] = {0x07, 0x01, 0x97, 0x82, 0x81, 0x19};
  for (int i = 0; i < 6; ++i) d[kIf + i] = b[i];
  return d;
}
}  // namespace

TEST_CASE("gowj prompt height patch: the non-primary branch is jumped over", "[split]") {
  std::string err;
  const auto patches = ue3::ParsePackagePatches(gowj::kPromptHeightPackagePatch, &err);
  CHECK(err.empty());
  REQUIRE(patches.size() == 1);
  CHECK(patches[0].file == "GearGame.xxx");
  std::vector<uint8_t> d = Original();
  CHECK(ue3::ApplyPackagePatches(d, "GearGame.xxx", patches) == 1);
  CHECK(d[kIf] == 0x06);        // Jump 0x0197: player 2 takes player 1's branch
  CHECK(d[kIf + 1] == 0x01);
  CHECK(d[kIf + 2] == 0x97);
}

TEST_CASE("gowj prompt height patch refuses a different GearGame.xxx", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gowj::kPromptHeightPackagePatch);
  std::vector<uint8_t> d = Original();
  d[kIf + 4] = 0x80;
  const std::vector<uint8_t> before = d;
  CHECK(ue3::ApplyPackagePatches(d, "GearGame.xxx", patches) == -1);
  CHECK(d == before);
}

TEST_CASE("gowj prompt height patch only side by side with two or more local players", "[split]") {
  CHECK_FALSE(gowj::PromptHeightWanted(true, 1));
  CHECK(gowj::PromptHeightWanted(true, 2));
  CHECK(gowj::PromptHeightWanted(true, 4));
  CHECK_FALSE(gowj::PromptHeightWanted(false, 2));   // top / bottom: the game's own heights
  CHECK_FALSE(gowj::PromptHeightWanted(false, 1));
}
