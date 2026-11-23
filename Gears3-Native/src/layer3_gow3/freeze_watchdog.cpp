// Layer 3 (Gears 3): freeze watchdog - this game's option only. The watchdog itself is ONE shared copy, Gears 2's
// src/layer2_ue3/freeze_watchdog.cpp (ue3/freeze_watchdog.h), built by all four games. Read-only, log-only.
#include "freeze_watchdog.h"

#include <ue3/freeze_watchdog.h>

#include <rex/cvar.h>

#include <cstdint>

REXCVAR_DEFINE_INT32(gow3_freeze_watchdog_seconds, 0, "Gears3",
                     "Developer (23 Nov, the strict-queries freeze): when no frame has been presented for this many "
                     "seconds after the first one, log every guest thread's state and every host thread's stack "
                     "(read-only, log-only; a long load can also trigger it). 0 = off (default)");

void Gow3StartFreezeWatchdog() {
  ue3::FreezeWatchdogConfig c;
  c.game = "gow3";
  c.limit_seconds = REXCVAR_GET(gow3_freeze_watchdog_seconds);
  ue3::StartFreezeWatchdog(c);
}

void Gow3StopFreezeWatchdog() { ue3::StopFreezeWatchdog(); }
