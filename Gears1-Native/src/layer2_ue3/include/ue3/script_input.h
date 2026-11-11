// Layer 2 (Unreal Engine 3): developer mode's scripted controller (--cogbox_input_script=<file>, off by default), as
// Gears3-Native's (the same verbs and script files), for Gears of War (2006): the game's class names come from the
// caller, the engine's object layout from ue3/objects.h (found in memory, or --ue3_object_layout). A
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
//   <t> choose L|R                  a path choice's side: its trigger, pressed ten times a second apart (Gears 1's
//                                   path choice is not watched: time it to when the choice is up)
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
//   <t> fight <s> [range]           aim at the nearest living Locust and fire (melee up close), for s or until none
// walkto / route / face / follow / fight take turns (choose runs beside them): a step whose time comes while the one before still runs waits for it.
// Events overlap freely: their buttons add up, a later stick or trigger event wins while it lasts.
#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}

// The game's classes the movement verbs look for
struct Ue3ScriptInputGame {
  const char* player_controller;   // player 1's controller class (Gears 1: WarPC)
  const char* pawn;                // the pawns' base class, with a Health property (Gears 1: WarPawn)
  const char* enemy_prefix;        // the class name start of the enemies "fight" aims at
  const char* layout;              // the engine's object layout (ue3/objects.h LayoutText), "": found in memory
};

// OnPostSetup: reads the script and adds the scripted controller to the input system. Nothing without the switch.
// names_var / objects_var: the engine's name and object tables (0: searched in memory), for the movement verbs.
void Ue3InstallScriptInputIfRequested(rex::Runtime* runtime, uint32_t names_var, uint32_t objects_var,
                                      const Ue3ScriptInputGame& game);
