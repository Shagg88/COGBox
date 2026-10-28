// COGBox 0.2.1: native mouse look for this game - its build's addresses (see ue3_mouse_look.h).
// {name, gears2, entry function, player axis / axis accumulate, friction, MouseX name, MouseY name, RightX name,
//  RightY name, check slot, check function}
#include "ue3_mouse_look.h"
#include "mouse_look.h"

// 0.2.1 (GitHub #18): Gears 2 TU6's MouseX / MouseY / RightX / RightY name slots read from its own code (the name
// set-up function, sub_82455660 = disc sub_82457E68) - 0.2.0 had them 0xA000 too low, so the check sent the mouse back
// to the right stick.
namespace {
#ifdef GOW2_TU6
constexpr ue3_mouse::Build kBuild = { "Gears of War 2 (Title Update 6)", true, 0x8280BCE0, 0x82644C48, 0x828F99A8, 0x8315CDD0, 0x8315CDD8, 0x8315D140, 0x8315D148, 0, 0 };
#else
constexpr ue3_mouse::Build kBuild = { "Gears of War 2 (disc)", true, 0x827FC468, 0x82640560, 0x828E3830, 0x8313AA24, 0x8313AA2C, 0x8313AD94, 0x8313AD9C, 0x821763D0, 0x827FC468 };
#endif
}  // namespace

void CogMouseLookInstall(rex::ui::Window* window) { ue3_mouse::Install(kBuild, window); }
