// Layer 2 (all four games): freeze watchdog (freeze_watchdog.cpp). Read-only, log-only. Our code (MIT).
#pragma once

#include <chrono>
#include <cstdint>

namespace ue3 {

struct FreezeWatchdogConfig {
  const char* game = "ue3";       // log tag "<game>_freeze:" and the option named in the log (--<game>_freeze_watchdog_seconds)
  int limit_seconds = 0;          // the game's option; <= 0: nothing is started
  // Judgment (#219): steady_clock ticks of the last loading screen / level change (0 = none seen). When given, the
  // thread report is made only for a stall that begins within load_window of one; nullptr: every stall is reported.
  int64_t (*last_level_load_ticks)() = nullptr;
  std::chrono::seconds load_window{120};
};

// From OnPostSetup. Does nothing when config.limit_seconds <= 0 or it is already running.
void StartFreezeWatchdog(const FreezeWatchdogConfig& config);
// From OnShutdown: stops and joins the thread (a report in progress ends early).
void StopFreezeWatchdog();

}  // namespace ue3
