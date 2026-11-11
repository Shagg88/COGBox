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
//   <t> log <text>                  a "script_input:" line in the log (to find a moment in it)
//   <t> quit                        the game ends (the test is over)
// Events overlap freely: their buttons add up, a later stick or trigger event wins while it lasts.
#pragma once

namespace rex {
class Runtime;
}

// OnPostSetup: reads the script and adds the scripted controller to the input system. Nothing without the switch.
void Ue3InstallScriptInputIfRequested(rex::Runtime* runtime);
