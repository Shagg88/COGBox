// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstdint>
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, gow3_main_menu_exit);

namespace rex::glue {

bool MatchesNeedLive(const uint8_t* base, uint32_t addr);
void ExitGame();

}  // namespace rex::glue
