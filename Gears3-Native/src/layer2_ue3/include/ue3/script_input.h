// Layer 2 (Unreal Engine 3): developer mode's scripted controller (--cogbox_input_script=<file>, off by default). A
// text file of timed controller events is played into player 1's controller, merged with the real devices: tests
// and recordings that need neither the keyboard nor window focus, and repeat exactly. From KiltedKernel's Gears 3
// port (runtime/hid/hid.cpp there: GOW3_INPUT_SCRIPT). Our code (MIT).
//
// One event a line, '#' starts a comment; <t> is seconds since the game started:
//   <t> pad <buttons> [ms]          buttons held for ms (default 150): A B X Y LB RB LS RS BACK START UP DOWN LEFT
//                                   RIGHT joined by '+', or an XInput mask in hex (0x1000)
//   <t> trig L|R [ms] [0-255]       a trigger (default 200 ms, fully)
//   <t> stick L|R <x> <y> [ms]      a stick held at x, y (-1..1; +y is up), default 500 ms
//   <t> look <y> <ms>               the right stick's y alone (gears3-pc's verb)
//   <t> choose L|R                  a path choice's side: waits for the choice (2 min), then its trigger
// (gears3-pc scripts also run: their bare-hex pad buttons are read, "capture" is ignored)
//   <t> log <text>                  a "script_input:" line in the log (to find a moment in it)
//   <t> quit                        the game ends (the test is over)
// Moving player 1 (positions are the map's x y; plan them from "pos" lines):
//   <t> pos                         player 1's position and camera yaw in the log
//   <t> walkto <x> <y> [s]          walk there (the left stick, relative to the camera); ends within 100 units, stuck
//                                   for 3 s, or after s (20)
//   <t> route x,y;x,y;... [s]       the points in order; a point not reached is tried again (a vault, use, a step
//                                   aside), then skipped
//   <t> face <x> <y>                the camera turned toward it
//   <t> follow <class part> [s] [d] walk after the pawn whose class name has the text (COGDom) for s (60), within d
//   <t> teleport <x> <y> <z> [cls]  player 1 (or the first pawn of that class) put there (testing)
// walkto / route / face / follow take turns: a step whose time comes while the one before still runs waits for it.
// Events overlap freely: their buttons add up, a later stick or trigger event wins while it lasts.
#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}

// The game's path choice (layer 3's Gow3PathChoiceTick watches the scene while a "choose" waits for it, and says
// whether it is open).
bool Ue3ScriptInputWantsPathChoice();
void Ue3ScriptInputPathChoiceOpen(bool open);

// OnPostSetup: reads the script and adds the scripted controller to the input system. Nothing without the switch.
// names_var / objects_var: the engine's name and object tables (ue3/objects.h), for the movement verbs.
void Ue3InstallScriptInputIfRequested(rex::Runtime* runtime, uint32_t names_var, uint32_t objects_var);
