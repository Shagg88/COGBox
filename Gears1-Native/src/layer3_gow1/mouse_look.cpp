// COGBox 0.2.1: native mouse look for Gears of War (2006) - its build's addresses (see ue3_mouse_look.h).
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
#include "ue3_mouse_look.h"
#include "mouse_look.h"

namespace {
constexpr ue3_mouse::Build kBuild = {
    "Gears of War (Title Update 5)", true, 0x824B39D8, 0, 0x821DB478,
    0x82FDCD74, 0x82FDCD7C, 0x82FDD0D4, 0x82FDD0DC,
    0x82112A64, 0x824B39D8,
    56, 4, 92};
}  // namespace

void CogMouseLookInstall(rex::ui::Window* window) { ue3_mouse::Install(kBuild, window); }
void CogMouseLookNoteFov(double fov) { ue3_mouse::NoteViewFov(fov); }   // 0.5.0
