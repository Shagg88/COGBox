// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstdint>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, gow_main_menu_exit);  // config_install.cpp: the label and this hook

namespace rex::glue {

bool PatchExitButton();  // true once done with (worker thread)
bool MatchesExitButton(const uint8_t* base, uint32_t addr);
void ExitGame();

}  // namespace rex::glue

// Starts the worker that hooks the menu button; call once in OnPostSetup.
void Gow3InstallMainMenuExitIfRequested();
