// Layer 3 (Judgment): GitHub #219 / #185 - freeze watchdog, see freeze_watchdog.cpp.
#pragma once

// From OnPostSetup. Does nothing when --gowj_freeze_watchdog_seconds=0. Log-only; reports only stalls after a load.
void GowjStartFreezeWatchdog();
// From OnShutdown.
void GowjStopFreezeWatchdog();
