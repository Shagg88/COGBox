// Layer 2 (Unreal Engine 3 on the 360): engine console commands from the PC side - the retail game has no console and
// reads no keyboard, but the engine still runs every command through ULocalPlayer's FExec::Exec (God, Fly, Slomo,
// ToggleDebugCamera, ToggleHUD, PlayersOnly, UnlockAllChapters, ...; notes/25). We call that the way the engine's own
// "-EXEC=" start-up option does: GEngine -> GamePlayers(0) -> its FExec subobject -> vtable slot, with GLog as the
// output device, on the game thread (the game's layer 3 calls Ue3ConsoleRunQueued from a function it overrides there).
//
// Commands come from:
//  - hotkeys: --ue3_hotkeys="F5=God;F6=Slomo 0.25|Slomo 1" ('|' = the key cycles through those commands); each key is
//    an SDK keybind (rex::ui::RegisterBind, cvar bind_ue3_<n>), so it can be moved to another key in the SDK settings;
//  - --ue3_exec="cmd;cmd": run once a player exists (also settable at run time from the SDK console, `ue3_exec God`);
//  - --ue3_exec_at="<tick>:<cmd>,...": run at that game tick (scripted tests).
//    "@F9" there presses that cheat key (GitHub #359: tests of the cheat key notices).
// GitHub #359: a command from a cheat key (not --ue3_exec / scripted ones) gets an on-screen notice (xenos/cheat_popup.h)
// with the cheat's state read back after it (ue3/auto_cheats.h DescribeKey) and a "cheat_popup:" log line.
// console.cpp lives in Gears 2's layer 2 (shared: Gears 1, 3 and Judgment build it from GN_LAYER0_HOME); this header
// has a copy in each game's src/layer2_ue3/include/ue3 - keep the four identical.
// Our code (MIT).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct PPCContext;

struct Ue3ConsoleTarget {
  uint32_t engine_global = 0;            // address of GEngine (UEngine*)
  uint32_t log_global = 0;               // address of GLog (FOutputDevice*)
  uint32_t players_data_offset = 0x4A0;  // UGameEngine::GamePlayers (TArray<ULocalPlayer*>) data pointer
  uint32_t players_num_offset = 0x4A4;   // ... and count
  uint32_t exec_offset = 0x3C;           // FExec subobject inside ULocalPlayer
  uint32_t exec_vtable_slot = 0;         // FExec::Exec
  // Run (silently) before each queued command, or empty. Gears 2 (--gow2_cheats): "EnableCheats" - its cheat manager is
  // made per player controller, and only while Engine.GameInfo.AllowCheats says yes (a standalone game: never a System
  // Link / online host or client).
  const char* before_each = "";
  // GitHub #342: when set, called instead of before_each (Gears 2: EnableCheats, with the game's solo listen server seen as
  // standalone for that one call - ue3/auto_cheats.h). false: no player yet (the command stays queued).
  bool (*enable_cheats)(PPCContext& ctx, uint8_t* base) = nullptr;
  // 0.6.3: asked before every queued command that is not one of the launcher's own display settings (see
  // Ue3ConsoleIsDisplaySetting): 0 = run it, 1 = keep it queued (the game state is not readable yet), 2 = drop it (cheats
  // are refused: Versus matches - ue3/match_guard.h). Empty: always run.
  int (*gate)() = nullptr;
};

// Layer 3: where the game keeps these (per game version). Without a target nothing runs.
void Ue3ConsoleSetTarget(const Ue3ConsoleTarget& target);
// Hotkeys from --ue3_hotkeys (call once the app exists, e.g. OnPostSetup). `defaults` is used when the option is empty.
void Ue3ConsoleInstallHotkeys(const std::string& defaults = std::string());
// 0.6.3: true for the `set ...` commands the launcher writes for display / corpse settings (not cheats: the gate lets
// them through in a Versus match).
bool Ue3ConsoleIsDisplaySetting(const std::string& command);
// Thread-safe; runs on the next Ue3ConsoleRunQueued.
void Ue3ConsoleQueue(const std::string& command);
// GitHub #342: runs one command now through GamePlayers(player)'s Exec (game thread only, no gate, no queue); false when that
// player does not exist yet. *handled: the engine knew the command.
bool Ue3ConsoleExecNow(PPCContext& ctx, uint8_t* base, int player, const std::string& command, bool* handled);
// GitHub #342: true on the thread Ue3ConsoleRunQueued runs its commands on (the engine loop's), once it has run.
bool Ue3ConsoleIsGameThread();
// GitHub #353: a command COGBox itself issues (the players' name tags: `set <PRI> PlayerName ...`). Not asked of the gate
// (it is no cheat) and not preceded by before_each; never from user input (hotkeys and --ue3_exec use Ue3ConsoleQueue).
// `after` (optional) runs on the game thread straight after the command, in the same frame.
void Ue3ConsoleQueueTrusted(const std::string& command, std::function<void()> after = {});
// Game thread only (layer 3's override of a once-per-frame engine function, after the original): runs what is queued.
// Leaves `ctx` as it found it.
void Ue3ConsoleRunQueued(PPCContext& ctx, uint8_t* base);
