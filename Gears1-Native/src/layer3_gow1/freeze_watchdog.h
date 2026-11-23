// Layer 3 (Gears 1): freeze watchdog (ported from Judgment's), see freeze_watchdog.cpp.
#pragma once

// From OnPostSetup. Does nothing when --gow1_freeze_watchdog_seconds=0 (the default). Log-only.
void Gow1StartFreezeWatchdog();
// From OnShutdown.
void Gow1StopFreezeWatchdog();
