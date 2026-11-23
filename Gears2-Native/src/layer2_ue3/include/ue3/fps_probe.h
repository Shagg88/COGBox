// Layer 2 (all four games): frame-rate probe and Kismet minimum delay (fps_probe.cpp; GitHub #405 / #413 / #414).
// ONE copy; each game's layer 3 fps_probe.cpp keeps its options (--fps_probe, --kismet_min_delay, in its own group)
// and says how its object tables are opened. Our code (MIT).
#pragma once

#include <cstdint>

namespace ue3 {

struct FpsProbeConfig {
  bool probe = false;                       // --fps_probe
  float min_delay = 0.0f;                   // --kismet_min_delay (0 = off)
  bool (*open_objects)() = nullptr;         // ue3::obj::Init with the game's tables / layout; true once ready
  uint32_t (*outer_offset)() = nullptr;     // UObject.Outer's offset (0 or nullptr: +40)
};

// OnPostSetup: starts the worker thread; nothing without one of the two switches.
void StartFpsProbe(const FpsProbeConfig& config);

}  // namespace ue3

// A dump of the active Kismet ops and streaming levels now (any thread; returns at once). Only with --fps_probe.
void FpsProbeDump(const char* why);
