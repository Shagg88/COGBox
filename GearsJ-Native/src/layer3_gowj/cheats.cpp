// Layer 3 (Gears of War: Judgment): --gowj_cheats, the developer cheats on hotkeys (GitHub #264; Gears 2's --gow2_cheats,
// Gears 3's --gow3_cheats). Off by default.
//
// No package patch: Judgment's GearGame.AllowCheats already returns True in the disc's GearGame.xxx (export at decompressed
// offset 0x1086A33, `04 27 04 3a <ReturnValue> 53`; Gears 3 has `04 28` there and --gow3_cheats patches it). So, as with
// Gears 3's cheats, the engine makes the cheat manager in every game this PC hosts - campaign, local games and also a
// System Link / online game this PC hosts (a client has no GameInfo: never there). The launcher's option says so.
//
// Console commands go through the engine's own path (layer 2 console.cpp, shared with Gears 2 / 3): GEngine ->
// GamePlayers(0) -> its FExec subobject (+60) -> vtable slot 0, output device GLog - read from the engine's "-EXEC="
// start-up code in sub_829D9E78 (0x829DA2A0..0x829DA2FC: `lwz r3,19972(lis -31916)` = GEngine 0x83544E04, Cast<UGameEngine>,
// GamePlayers count +1192 / data +1188 (4 more than Gears 3), `addi r3,r11,60`, `lwz r5,22148(lis -31925)` = GLog
// 0x834B5684, `bctrl` on slot 0); the same sequence in sub_8255CD90 and the DeferredCommands loop sub_82515070 agree.
// They run on the game thread from our override of UEngine::GetMaxTickRate (sub_8255E200, the function whose
// `lfs f1,0x4CC(r3)` GowjMaxTickRate hooks; the hook still runs inside the original). Research: r7 (2026-11-14). Our code (MIT).
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <ue3/console.h>

REXCVAR_DEFINE_BOOL(gowj_cheats, false, "Judgment",
                    "GitHub #264: developer cheats on hotkeys: F5 god mode, F6 slow motion (cycles 1/2, 1/4, normal), F9 "
                    "free camera, F10 hide the HUD, F11 all weapons and ammo (keys can be changed with --ue3_hotkeys). Also "
                    "in a System Link / online game this PC hosts (Judgment's own AllowCheats)");

namespace {
constexpr uint32_t kGEngine = 0x83544E04u;
constexpr uint32_t kGLog = 0x834B5684u;
// F7 / F8 are the in-game menu's (achievements, COGBox social), F1-F4 the SDK's / ours: as Gears 2.
constexpr const char* kDefaultHotkeys =
    "F5=God;F6=Slomo 0.5|Slomo 0.25|Slomo 1;F9=ToggleDebugCamera;F10=ToggleHUD;F11=AllWeapons|AllAmmo";
}  // namespace

// OnPostSetup. Without --gowj_cheats only what --ue3_hotkeys / --ue3_exec name runs.
void GowjInstallCheats() {
  Ue3ConsoleTarget target;
  target.engine_global = kGEngine;
  target.log_global = kGLog;
  target.players_data_offset = 1188;  // UGameEngine::GamePlayers in Judgment's engine (Gears 3: 0x4A0 / 0x4A4)
  target.players_num_offset = 1192;
  target.exec_offset = 60;
  target.exec_vtable_slot = 0;
  Ue3ConsoleSetTarget(target);
  if (REXCVAR_GET(gowj_cheats)) {
    Ue3ConsoleInstallHotkeys(kDefaultHotkeys);
    REXLOG_INFO("gowj_cheats: developer cheats on hotkeys");
  } else {
    Ue3ConsoleInstallHotkeys();  // only what --ue3_hotkeys names
  }
}

// UEngine::GetMaxTickRate, overridden: the original (with the frame-rate hook inside), then queued console commands
// (ctx is left as the original left it, so f1 - its result - is untouched).
extern "C" void __imp__sub_8255E200(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8255E200(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_8255E200(ctx, base);
  Ue3ConsoleRunQueued(ctx, base);
}
