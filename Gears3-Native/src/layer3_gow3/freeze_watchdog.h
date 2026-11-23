// Layer 3 (Gears 3): freeze watchdog (ported from Judgment's), see freeze_watchdog.cpp.
#pragma once

// From OnPostSetup. Does nothing when --gow3_freeze_watchdog_seconds=0 (the default). Log-only.
void Gow3StartFreezeWatchdog();
// From OnShutdown.
void Gow3StopFreezeWatchdog();
