// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstdint>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, gow_main_menu_exit);  // config_install.cpp: the label and this hook

namespace rex::glue {

bool MatchesNeedLive(const uint8_t* base, uint32_t addr);
void ExitGame();

}  // namespace rex::glue

// Tracks the front end (the file-open observer); call once in OnPostSetup, before the game opens a file.
void Gow3InstallMainMenuExitIfRequested();
