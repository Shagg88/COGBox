// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstdint>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, gow_main_menu_exit);  // config_install.cpp: the label and this hook

namespace rex::glue {

void ExitGame();

}  // namespace rex::glue
