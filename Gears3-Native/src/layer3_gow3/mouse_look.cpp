// COGBox 0.2.1: native mouse look for this game - its build's addresses (see ue3_mouse_look.h).
// {name, gears2, entry function, player axis / axis accumulate, friction, MouseX name, MouseY name, RightX name,
//  RightY name, check slot, check function}
#include "ue3_mouse_look.h"
#include "mouse_look.h"

namespace {
#ifdef GOW3_TU6
constexpr ue3_mouse::Build kBuild = { "Gears of War 3 (Title Update 6)", false, 0x82625EC8, 0x8274CC98, 0, 0x83454898, 0x834548A0, 0, 0, 0, 0 };
#else
constexpr ue3_mouse::Build kBuild = { "Gears of War 3 (disc)", false, 0x82614F38, 0x8273B518, 0, 0x834440C8, 0x834440D0, 0, 0, 0x821897A4, 0x8273B518 };
#endif
}  // namespace

void CogMouseLookInstall(rex::ui::Window* window) { ue3_mouse::Install(kBuild, window); }
