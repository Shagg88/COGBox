// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstdint>
#include <string_view>

#include <rex/cvar.h>

struct PPCContext;

REXCVAR_DECLARE(bool, gow2_main_menu_exit);

namespace rex::glue {

void ExitGame();
bool GuestUtf16EqualsNoCase(const uint8_t* base, uint32_t addr, std::string_view expected);
bool GuestAsciiEqualsNoCase(const uint8_t* base, uint32_t addr, std::string_view expected);
bool MatchesStringNoCase(const uint8_t* base, uint32_t addr, std::string_view text);
bool MatchesCredits(const uint8_t* base, const PPCContext& ctx);

}  // namespace rex::glue

void Gow2InstallMainMenuExitIfRequested();
void Gow2NotifyReadyToExit();
