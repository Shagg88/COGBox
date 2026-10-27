// COGBox 0.2.1: native mouse look for this game - its build's addresses (see ue3_mouse_look.h).
// {name, gears2, entry function, player axis / axis accumulate, friction, MouseX name, MouseY name, RightX name,
//  RightY name, check slot, check function}
#include "ue3_mouse_look.h"
#include "mouse_look.h"

namespace {
#ifdef GOW2_TU6
constexpr ue3_mouse::Build kBuild = { "Gears of War 2 (Title Update 6)", true, 0x8280BCE0, 0x82644C48, 0x828F99A8, 0x83152DD0, 0x83152DD8, 0x83153140, 0x83153148, 0, 0 };
#else
constexpr ue3_mouse::Build kBuild = { "Gears of War 2 (disc)", true, 0x827FC468, 0x82640560, 0x828E3830, 0x8313AA24, 0x8313AA2C, 0x8313AD94, 0x8313AD9C, 0x821763D0, 0x827FC468 };
#endif
}  // namespace

void CogMouseLookInstall(rex::ui::Window* window) { ue3_mouse::Install(kBuild, window); }
