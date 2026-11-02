// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, gow2_main_menu_exit);

namespace rex::glue {

void ExitGame();

}  // namespace rex::glue

void Gow2InstallMainMenuExitIfRequested();
void Gow2NotifyReadyToExit();
