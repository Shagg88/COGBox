// Layer 0 (game-free): when COGBox's own pad assignment (ingame_menu.cpp InstallPadAssignment) may replace the SDK's.
// --player_devices (SDK patch 117, the launcher's Player N dropdowns) makes the SDK use its ExplicitAssignment, which
// gives each player the device chosen for it and the Auto players the rest (keyboard and mouse first, then the pads:
// SDK patch 129); COGBox's SplitScreenPads / SinglePlayerPads would throw that away, so with the cvar set COGBox does
// not apply its own rules. Header only, tested by tests/layer0_pad_policy_test.cpp. Our code (MIT).
#pragma once
#include <string_view>

namespace xenos {

inline bool ShouldOverridePadAssignment(std::string_view player_devices) {
  return player_devices.empty();
}

}  // namespace xenos
