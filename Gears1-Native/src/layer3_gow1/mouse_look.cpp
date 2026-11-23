// COGBox 0.2.1: native mouse look for Gears of War (2006) - its build's addresses (the logic: layer 2's ue3/mouse_look.h).
// {name, gears2 way, entry function, player axis (unused), friction, MouseX name, MouseY name, RightX name,
//  RightY name, check slot, check function, client offset, viewport argument offset, Viewports offset}
// GitHub #82: Gears 1 still drove the right stick from the mouse (capped / slow turning).
// v2 (job 833 test: hook "on" but the camera never moved): v1 hooked ProcessInput 0x82988948, which Gears 1 only
// reaches by a direct `bl` from the client Tick (0x824B3A10) - direct calls go to the weak sub_ symbol, not the
// dispatcher, so the hook never ran. Now the entry is the client Tick 0x824B39D8, reached only through its vtable
// (slot 0x82112A64), which calls ProcessInput on each of Viewports (data this+92, count this+96).
//  - ProcessInput 0x82988948: viewport client at viewport +56, InputAxis = client vtable +24 with r4 = viewport +4.
//  - Key names from sub_822960F8 (L"MouseX" 0x820D1790 ... L"XboxTypeS_RightY" 0x820D1D78).
//  - Friction: AWarPC::execGetFrictionTarget 0x821DB478 (native table entry 0x82F9DA98).
//  - Pad used last: the client's InputAxis, hooked when first found (logged with its address).
#include <ue3/mouse_look.h>   // the shared logic (layer 2); this file: the build table
#include "mouse_look.h"

namespace {
// --ue3_mouse_turn_cap: the object tables, as cheats.cpp / adaptive_triggers.cpp open them (globals, layout given)
bool OpenObjects() {
  ue3::obj::Layout layout;
  ue3::obj::ParseLayout("4,40,44,52,60,76,64,72,96,124,16,1,1", &layout);   // ue3::obj::LayoutText order
  return ue3::obj::Ready() || ue3::obj::Init(0x82FED4F0u, 0x82FDD1C4u, &layout);
}
constexpr ue3_mouse::Build kBuild = {
    .name = "Gears of War (Title Update 5)", .gears2 = true, .entry_fn = 0x824B39D8, .player_axis_fn = 0,
    .friction_fn = 0x821DB478, .mouse_x_name = 0x82FDCD74, .mouse_y_name = 0x82FDCD7C, .right_x_name = 0x82FDD0D4,
    .right_y_name = 0x82FDD0DC, .check_slot = 0x82112A64, .check_fn = 0x824B39D8,
    .client_off = 56, .viewport_arg_off = 4, .viewports_off = 92,
    // GitHub #52 / #114 (every camera the same angle per count) were never ported to Gears 1: aim / zoom multipliers only
    .scaling = ue3_mouse::Build::Scaling::kAimZoomOnly,
    .diag = true,   // GitHub #82 bring-up: the six "diag" lines
    .open_objects = &OpenObjects};
}  // namespace

void CogMouseLookInstall(rex::ui::Window* window) { ue3_mouse::Install(kBuild, window); }
void CogMouseLookNoteFov(double fov) { ue3_mouse::NoteViewFov(fov); }   // 0.5.0
