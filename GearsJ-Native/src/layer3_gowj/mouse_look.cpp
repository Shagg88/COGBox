// COGBox 0.2.1: native mouse look for this game - its build's addresses (the logic: layer 2's ue3/mouse_look.h).
// {name, gears2, entry function, player axis / axis accumulate, friction, MouseX name, MouseY name, RightX name,
//  RightY name, check slot, check function}
#include <ue3/mouse_look.h>   // the shared logic (layer 2); this file: the build table
#include "mouse_look.h"

namespace {
// --ue3_mouse_turn_cap: the object tables, as cheats.cpp / adaptive_triggers.cpp open them (globals, layout given)
bool OpenObjects() {
  ue3::obj::Layout layout;
  ue3::obj::ParseLayout("32,40,44,52,64,68,60,68,84,104,8,0,1", &layout);   // ue3::obj::LayoutText order
  return ue3::obj::Ready() || ue3::obj::Init(0x8355A328u, 0x8355A368u, &layout);
}
constexpr ue3_mouse::Build kBuild = {
    .name = "Gears of War: Judgment (Title Update 4)", .gears2 = false, .entry_fn = 0x826390E0,
    .player_axis_fn = 0x827626A0, .friction_fn = 0, .mouse_x_name = 0x835617A8, .mouse_y_name = 0x835617B0,
    .right_x_name = 0, .right_y_name = 0, .check_slot = 0, .check_fn = 0,
    // GitHub #52 / #114 (every camera the same angle per count) were never ported to Judgment: aim / zoom multipliers only
    .scaling = ue3_mouse::Build::Scaling::kAimZoomOnly,
    .open_objects = &OpenObjects};
}  // namespace

void CogMouseLookInstall(rex::ui::Window* window) { ue3_mouse::Install(kBuild, window); }
void CogMouseLookNoteFov(double fov) { ue3_mouse::NoteViewFov(fov); }
