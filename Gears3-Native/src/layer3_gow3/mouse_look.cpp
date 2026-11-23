// COGBox 0.2.1: native mouse look for this game - its build's addresses (the logic: layer 2's ue3/mouse_look.h).
// {name, gears2, entry function, player axis / axis accumulate, friction, MouseX name, MouseY name, RightX name,
//  RightY name, check slot, check function}
#include <ue3/mouse_look.h>   // the shared logic (layer 2); this file: the build table
#include "mouse_look.h"

#include "gow3_addresses.h"

namespace {
// --ue3_mouse_turn_cap: the object tables, as cheats.cpp / fps_probe.cpp open them (this build's globals)
bool OpenObjects() { return ue3::obj::Ready() || ue3::obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar); }
#ifdef GOW3_TU6
constexpr ue3_mouse::Build kBuild = { .name = "Gears of War 3 (Title Update 6)", .gears2 = false, .entry_fn = 0x82625EC8, .player_axis_fn = 0x8274CC98, .friction_fn = 0, .mouse_x_name = 0x83454898, .mouse_y_name = 0x834548A0, .right_x_name = 0, .right_y_name = 0, .check_slot = 0, .check_fn = 0, .open_objects = &OpenObjects };
#else
constexpr ue3_mouse::Build kBuild = { .name = "Gears of War 3 (disc)", .gears2 = false, .entry_fn = 0x82614F38, .player_axis_fn = 0x8273B518, .friction_fn = 0, .mouse_x_name = 0x834440C8, .mouse_y_name = 0x834440D0, .right_x_name = 0, .right_y_name = 0, .check_slot = 0x821897A4, .check_fn = 0x8273B518, .open_objects = &OpenObjects };
#endif
}  // namespace

void CogMouseLookInstall(rex::ui::Window* window) { ue3_mouse::Install(kBuild, window); }
void CogMouseLookNoteFov(double fov) { ue3_mouse::NoteViewFov(fov); }
