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
// Our code (MIT).
#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

struct Ue3ConsoleTarget {
  uint32_t engine_global = 0;            // address of GEngine (UEngine*)
  uint32_t log_global = 0;               // address of GLog (FOutputDevice*)
  uint32_t players_data_offset = 0x4A0;  // UGameEngine::GamePlayers (TArray<ULocalPlayer*>) data pointer
  uint32_t players_num_offset = 0x4A4;   // ... and count
  uint32_t exec_offset = 0x3C;           // FExec subobject inside ULocalPlayer
  uint32_t exec_vtable_slot = 0;         // FExec::Exec
};

// Layer 3: where the game keeps these (per game version). Without a target nothing runs.
void Ue3ConsoleSetTarget(const Ue3ConsoleTarget& target);
// Hotkeys from --ue3_hotkeys (call once the app exists, e.g. OnPostSetup). `defaults` is used when the option is empty.
void Ue3ConsoleInstallHotkeys(const std::string& defaults = std::string());
// Thread-safe; runs on the next Ue3ConsoleRunQueued.
void Ue3ConsoleQueue(const std::string& command);
// Game thread only (layer 3's override of a once-per-frame engine function, after the original): runs what is queued.
// Leaves `ctx` as it found it.
void Ue3ConsoleRunQueued(PPCContext& ctx, uint8_t* base);
