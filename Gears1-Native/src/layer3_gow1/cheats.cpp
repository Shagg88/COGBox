// Layer 3 (Gears of War 1, TU5): --gow1_cheats, the developer cheats on hotkeys (GitHub #264; Gears 2's --gow2_cheats,
// Gears 3's --gow3_cheats, Judgment's --gowj_cheats). Off by default. Checked on the PC (jobs 1818, 1819): God,
// AllWeapons, AllAmmo, Slomo, ToggleHUD, PlayersOnly, Ghost / Fly / Walk are all taken; this 2006 engine has no
// ToggleDebugCamera and no BehindView, so F9 here is Ghost / Walk (fly through walls, and back).
//
// Console commands go through the engine's own path (layer 2 console.cpp, shared): GEngine -> GamePlayers(0) -> its FExec
// subobject (+60) -> vtable slot 0, output device GLog - read from the "EXEC=" start-up code in FEngineLoop::Init
// (sub_82229970, 0x82229DCC..0x82229E38: `lwz r3,-0x22B0(r27)` = GEngine 0x82FCDD50, Cast<UGameEngine>, GamePlayers
// count +0x2A0 / data +0x29C, `addi r3,r11,0x3C`, `lwz r5,-0x7268(lis -0x7D06)` = GLog 0x82F98D98, `bctrl` slot 0); the
// engine's own DeferredCommands loop in FEngineLoop::Tick agrees. The commands run from our override of FEngineLoop::Tick (sub_8222A6E0, called once per frame from GuardedMain's loop at
// 0x8222B560), after the original. Each hotkey runs "EnableCheats" first (the cheat manager belongs to the player
// controller, which a new level replaces), as in Gears 2. Research: r7 (2026-11-14). Our code (MIT).
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <ue3/console.h>

REXCVAR_DEFINE_BOOL(gow1_cheats, false, "Gears1",
                    "GitHub #264: developer cheats on hotkeys: F5 god mode, F6 slow motion (cycles 1/2, 1/4, normal), F9 "
                    "fly through walls (on / off), F10 hide the HUD, F11 all weapons and ammo (keys can be changed with "
                    "--ue3_hotkeys)");

namespace {
constexpr uint32_t kGEngine = 0x82FCDD50u;
constexpr uint32_t kGLog = 0x82F98D98u;
constexpr const char* kDefaultHotkeys =
    "F5=God;F6=Slomo 0.5|Slomo 0.25|Slomo 1;F9=Ghost|Walk;F10=ToggleHUD;F11=AllWeapons|AllAmmo";
}  // namespace

// OnPostSetup. Without --gow1_cheats only what --ue3_hotkeys / --ue3_exec name runs, and nothing enables cheats.
void Gow1InstallCheats() {
  Ue3ConsoleTarget target;
  target.engine_global = kGEngine;
  target.log_global = kGLog;
  target.players_data_offset = 0x29C;  // UGameEngine::GamePlayers in Gears 1's engine
  target.players_num_offset = 0x2A0;
  target.exec_offset = 60;
  target.exec_vtable_slot = 0;
  const bool cheats = REXCVAR_GET(gow1_cheats);
  if (cheats) target.before_each = "EnableCheats";
  Ue3ConsoleSetTarget(target);
  if (cheats) {
    Ue3ConsoleInstallHotkeys(kDefaultHotkeys);
    REXLOG_INFO("gow1_cheats: developer cheats on hotkeys");
  } else {
    Ue3ConsoleInstallHotkeys();  // only what --ue3_hotkeys names
  }
}

// FEngineLoop::Tick, overridden: the original, then queued console commands (ctx left as the original left it).
extern "C" void __imp__sub_8222A6E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8222A6E0(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_8222A6E0(ctx, base);
  Ue3ConsoleRunQueued(ctx, base);
}
