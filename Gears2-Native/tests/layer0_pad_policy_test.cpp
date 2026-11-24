// Layer 0 unit tests: when COGBox's own pad assignment (ingame_menu.cpp InstallPadAssignment) may replace the SDK's.
// With --player_devices the SDK's ExplicitAssignment decides, so COGBox must not override it. Our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <xenos/pad_assignment_policy.h>

using xenos::ShouldOverridePadAssignment;

TEST_CASE("pad policy: no player_devices keeps COGBox's own pad assignment", "[pad_policy]") {
  CHECK(ShouldOverridePadAssignment(""));
}

TEST_CASE("pad policy: a player_devices choice is left to the SDK", "[pad_policy]") {
  CHECK_FALSE(ShouldOverridePadAssignment("sdl:A|p;auto"));
  CHECK_FALSE(ShouldOverridePadAssignment("auto;sdl:A|p;auto"));
  CHECK_FALSE(ShouldOverridePadAssignment("sdl:A|p"));
}
