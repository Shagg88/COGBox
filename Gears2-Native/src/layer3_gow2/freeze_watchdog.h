// Layer 3 (Gears 2): freeze watchdog (ported from Judgment's), see freeze_watchdog.cpp.
#pragma once

// From OnPostSetup. Does nothing when --gow2_freeze_watchdog_seconds=0 (the default). Log-only.
void Gow2StartFreezeWatchdog();
// From OnShutdown.
void Gow2StopFreezeWatchdog();
