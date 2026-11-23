// Layer 2 (Unreal Engine 3 on the 360). May include layer 0-1 headers only.
#pragma once

#include <cstdint>
#include <string_view>

// GitHub #101 (Gears 3's --gow3_menu_fps, shared with Gears 2 / Judgment since the 23 Nov parity pass): the front end
// (title screen and its menus) ran at the player's limit - with none, hundreds of fps and the graphics card at 100 % on
// a still menu. The signal is the file-open observer (SDK patch 32, chained through the shader reader): GearStart*.xxx
// opened -> front end; a level package (*_P.xxx, or MP_*.xxx without _LOC_) -> a level, the player's limit again.
// Only file opens decide, so a wrong answer can only leave a menu uncapped, never cap gameplay.
namespace ue3 {

// Installs the listener once (call at start-up, before the game opens a file). `option` names the game's cvar for the
// log lines (e.g. "gow2_menu_fps"); `fps` is its value at start-up (<= 0: nothing is installed).
void InstallMenuFpsCap(std::string_view option, int32_t fps);
// True while GearStart*.xxx is the last front-end / level package the game opened.
bool InMenuFront();
// The limit for a front-end frame, as Gears 3's Gow3MaxTickRate: `player` is the player's limit (< 0 none, 0 the
// game's own, N), `engine` the engine's own value (0 = none), `menu` the menu cap (> 0). Never above `menu`.
double MenuCappedLimit(int32_t player, double engine, int32_t menu);
// --<game>_tick_floor_fps (Gears 3's): with no limit the engine still ticks at most `floor` times a second, so a tick's
// delta never drops below 1/floor s (checkpoint saves and streaming ran at 2000+ ticks/s with nothing presented).
// Returns the value the engine should get instead of "none" (0), or `none_value` when the floor is off.
inline double TickFloorLimit(int32_t floor, double none_value) { return floor > 0 ? double(floor) : none_value; }

}  // namespace ue3
