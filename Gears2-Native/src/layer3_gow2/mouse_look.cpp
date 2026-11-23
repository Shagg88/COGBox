// COGBox 0.2.1: native mouse look for this game - its build's addresses (the logic: layer 2's ue3/mouse_look.h).
// {name, gears2, entry function, player axis / axis accumulate, friction, MouseX name, MouseY name, RightX name,
//  RightY name, check slot, check function}
#include <ue3/mouse_look.h>   // the shared logic (layer 2); this file: the build table
#include "mouse_look.h"

// 0.2.1 (GitHub #18): Gears 2 TU6's MouseX / MouseY / RightX / RightY name slots read from its own code (the name
// set-up function, sub_82455660 = disc sub_82457E68) - 0.2.0 had them 0xA000 too low, so the check sent the mouse back
// to the right stick.
#include "gow2_addresses.h"

namespace {
// --ue3_mouse_turn_cap: the object tables, as cheats.cpp / fps_probe.cpp open them (this build's globals)
bool OpenObjects() { return ue3::obj::Ready() || ue3::obj::Init(gow2::addr::kGNamesVar, gow2::addr::kGObjectsVar); }
#ifdef GOW2_TU6
constexpr ue3_mouse::Build kBuild = { .name = "Gears of War 2 (Title Update 6)", .gears2 = true, .entry_fn = 0x8280BCE0, .player_axis_fn = 0x82644C48, .friction_fn = 0x828F99A8, .mouse_x_name = 0x8315CDD0, .mouse_y_name = 0x8315CDD8, .right_x_name = 0x8315D140, .right_y_name = 0x8315D148, .check_slot = 0, .check_fn = 0, .open_objects = &OpenObjects };
#else
constexpr ue3_mouse::Build kBuild = { .name = "Gears of War 2 (disc)", .gears2 = true, .entry_fn = 0x827FC468, .player_axis_fn = 0x82640560, .friction_fn = 0x828E3830, .mouse_x_name = 0x8313AA24, .mouse_y_name = 0x8313AA2C, .right_x_name = 0x8313AD94, .right_y_name = 0x8313AD9C, .check_slot = 0x821763D0, .check_fn = 0x827FC468, .open_objects = &OpenObjects };
#endif
}  // namespace

void CogMouseLookInstall(rex::ui::Window* window) { ue3_mouse::Install(kBuild, window); }
void CogMouseLookNoteFov(double fov) { ue3_mouse::NoteViewFov(fov); }
