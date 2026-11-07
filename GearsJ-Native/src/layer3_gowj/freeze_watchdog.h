// Layer 3 (Judgment): GitHub #219 / #185 - freeze watchdog, see freeze_watchdog.cpp.
#pragma once

// From OnPostSetup. Does nothing when --gowj_freeze_watchdog_seconds=0.
void GowjStartFreezeWatchdog();
// From OnShutdown.
void GowjStopFreezeWatchdog();
