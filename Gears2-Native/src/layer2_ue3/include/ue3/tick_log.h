// Layer 2 (Unreal Engine 3 on the 360). May include layer 0-1 headers only.
#pragma once

#include <cstdint>

// --<game>_tick_log (Gears 3 / Judgment's, one copy since the 23 Nov parity pass; Gears 2 too): the engine tick's
// rhythm, for small regular stutters - the renderer's frame-time line gives only mean / p99 / max and its hitch report
// starts at 40 ms. Call once per engine loop (each game's GetMaxTickRate hook, the engine loop's call only). Every
// `every_n` ticks one line: median / p95 / max, the slow ticks (> 1.5x median and > median + 4 ms) and the spacing
// between them (median gap in ms and ticks, and gap spread / median gap: a stutter on a fixed rhythm gives a steady gap
// with a small spread). Read-only. `option` names the game's cvar in the line. Game thread only; Windows only.
namespace ue3 {
void TickLog(const char* option, int32_t every_n);
}  // namespace ue3
