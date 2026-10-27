// COGBox 0.2.1: native mouse look for this game - its build's addresses (see ue3_mouse_look.h).
// {name, gears2, entry function, player axis / axis accumulate, friction, MouseX name, MouseY name, RightX name,
//  RightY name, check slot, check function}
#include "ue3_mouse_look.h"
#include "mouse_look.h"

namespace {
constexpr ue3_mouse::Build kBuild = { "Gears of War: Judgment (Title Update 4)", false, 0x826390E0, 0x827626A0, 0, 0x835617A8, 0x835617B0, 0, 0, 0, 0 };
}  // namespace

void CogMouseLookInstall(rex::ui::Window* window) { ue3_mouse::Install(kBuild, window); }
