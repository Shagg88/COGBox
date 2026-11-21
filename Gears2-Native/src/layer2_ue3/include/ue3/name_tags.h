// Layer 2 (Unreal Engine 3 on the 360): GitHub #353 - each online player's COGBox tag after their name, everywhere the
// game shows it (party list, lobby, scoreboard). name_tags.cpp lives in Gears 2's layer 2 (shared: Gears 1, 3 and
// Judgment build it from GN_LAYER0_HOME, each with its own ue3/objects.h); this header has a copy in each game's
// src/layer2_ue3/include/ue3 - keep the four identical.
//
// Once a second (a scan of the game's own objects, no network) every PlayerReplicationInfo is read: its UniqueId (the
// XUID) and PlayerName. A player with a COGBox account (xenos/player_tags.h asks the master server once per player)
// gets `set <PRI> PlayerName <name><before>#1234<after>` through Ue3ConsoleQueueTrusted - the engine's own console
// command, so the engine changes the string with its own allocator. Only this PC's copies change: nothing is sent.
// Bots (XUID 0) and offline play are left alone. Our code (MIT).
#pragma once

#include <cstddef>
#include <cstdint>

struct Ue3NameTagStyle {
  const char* before = "";  // markup before "#1234", e.g. "<font color='#808080'>" (empty in every game for now)
  const char* after = "";   // ... and after it
  // Gears 1 / 2 (UE3 2006-08): their `set` takes a class only, not one object. Then, in one game frame (this tick must
  // run on the game thread just before Ue3ConsoleRunQueued): every player's name is read, `set PlayerReplicationInfo
  // PlayerName <160 x>` gives every PlayerName (instances and class defaults) a new engine-owned 160-character buffer,
  // and each gets its own name back in it - with the tag where it has one. The placeholder is never drawn or sent.
  bool class_set = false;
  // For a style that draws in another font (Gears 1's one grey text style, "<Styles:credits_headers>", when its grey
  // was in use; its UI ignores <Fonts:...>). "Style=Font" (object names): that style draws in this font (from when
  // both are loaded). Unused while no game has markup.
  const char* style_font = "";
};

// Once per engine tick (it paces itself); the game thread, before Ue3ConsoleRunQueued, when class_set. `objects_ready`: true once ue3::obj is open (the
// game's layer 3 opens it). Off with --cogbox_name_tags=false.
void Ue3NameTagsTick(bool (*objects_ready)(), const Ue3NameTagStyle& style);

// The kill feed's draw-time half: a game's layer 3 hooks the engine's text routines (under Canvas.DrawText / TextSize /
// StrLen) and passes each text argument through Ue3NameTagsTextArg - the argument itself, or (when it is text, or an
// FString whose text holds the tag's markup) a scratch copy without the markup, in guest memory of our own. The caller's
// text is never changed. Nothing happens until the worker has checked that the game's Canvas natives are the ones the
// hooks were written for (Ue3NameTagsTextHookChecks, from a static initializer of the game file that has the hooks).
// Idle (no scratch memory either) while the game's tags have no markup - every game, for now.
struct Ue3TextHookCheck {
  const char* function;   // a Canvas function, e.g. "DrawText"
  uint32_t exec;          // its native's code address in this build
};
void Ue3NameTagsTextHookChecks(const Ue3TextHookCheck* checks, size_t count);
uint32_t Ue3NameTagsTextArg(uint32_t arg);
// The text routines are shared by much of the engine (Gears 1's 823A38F0 has 327 call sites), so Ue3NameTagsTextArg
// acts only on this thread while one of the Canvas exec natives above runs (they hold the script's own copy of the text):
// a game's layer 3 also wraps each of those natives in an Ue3CanvasTextCall (review r368b).
struct Ue3CanvasTextCall {
  Ue3CanvasTextCall();
  ~Ue3CanvasTextCall();
  Ue3CanvasTextCall(const Ue3CanvasTextCall&) = delete;
  Ue3CanvasTextCall& operator=(const Ue3CanvasTextCall&) = delete;
};
