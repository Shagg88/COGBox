// Layer 3 (Judgment): freeze watchdog - this game's option only. The watchdog itself is ONE shared copy, Gears 2's
// src/layer2_ue3/freeze_watchdog.cpp (ue3/freeze_watchdog.h), built by all four games. Read-only, log-only.
#include "freeze_watchdog.h"

#include <ue3/freeze_watchdog.h>

#include <rex/cvar.h>

#include <cstdint>

REXCVAR_DEFINE_INT32(gowj_freeze_watchdog_seconds, 10, "Judgment",
                     "GitHub #219: when no frame has been presented for this many seconds after a loading screen / "
                     "level change, log every guest thread's state and every host thread's stack (read-only, log-only; "
                     "may also fire on a long load). 0 = off");

int64_t GowjLastLevelLoadTicks();  // game_patches.cpp (0 = none seen)

void GowjStartFreezeWatchdog() {
  ue3::FreezeWatchdogConfig c;
  c.game = "gowj";
  c.limit_seconds = REXCVAR_GET(gowj_freeze_watchdog_seconds);
  c.last_level_load_ticks = GowjLastLevelLoadTicks;  // #219: report only stalls near a loading screen
  ue3::StartFreezeWatchdog(c);
}

void GowjStopFreezeWatchdog() { ue3::StopFreezeWatchdog(); }
