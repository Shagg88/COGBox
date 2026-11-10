// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstdint>
#include <string_view>

#include <rex/cvar.h>

struct PPCContext;

REXCVAR_DECLARE(bool, gow_main_menu_exit);  // config_install.cpp: the label and these hooks

namespace rex::glue {

void ExitGame();
bool GuestUtf16EqualsNoCase(const uint8_t* base, uint32_t addr, std::string_view expected);
bool GuestAsciiEqualsNoCase(const uint8_t* base, uint32_t addr, std::string_view expected);
bool MatchesStringNoCase(const uint8_t* base, uint32_t addr, std::string_view text);
bool MatchesCredits(const uint8_t* base, const PPCContext& ctx);
// Armed only by the Exit Game row's label (after a level) and that row was last shown over 30 s ago: the credits are
// then the campaign's own, not Exit Game.
bool ExitRowShownLongAgo();

}  // namespace rex::glue

void Gow2InstallMainMenuExitIfRequested();
