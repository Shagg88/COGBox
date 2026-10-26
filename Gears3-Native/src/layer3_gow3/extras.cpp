// Layer 3 (Gears of War 3): extras switched on from the launcher (notes/25), all off by default.
//  - --gow3_cheats: the developer cheats. GearGame.AllowCheats is `return false;` in the script package (GearGame.xxx,
//    offset 0xD4508E in the decompressed package: 04 28 = return False); our package patch makes it return True
//    (04 27), so the engine creates the cheat manager as on a developer build (AllAmmo, Slomo, ToggleDebugCamera,
//    Teleport, Summon, ...). Commands on the player controller itself (God, Fly, Ghost, PlayersOnly, Kill, Revive,
//    UnlockAllChapters, ...) need no cheat manager. Also turns on the default hotkeys below (photo mode included).
//  - --gow3_unlock_mutators: GearPC.UpdateMutatorUnlocks sets bHasUnlockedMutator = (UnlockAwardName == "") ||
//    AwardManager.HasEarnedAwardByName(...). The patch turns its `==` (native 0x7A, EqualEqual_StrStr) into `>=` (0x79,
//    GreaterEqual_StrStr; checked in Core.xxx): every string is >= "", so every mutator counts as unlocked. Nothing is
//    written to the profile - turn the switch off and they are locked as before.
// Both bytes are in the disc's GearGame.xxx, which Title Update 6 doesn't replace (its ScriptPatch_GearGame.bin
// patches neither function), so the same patch serves both versions.
// Console commands reach the engine through ue3/console.h, run from our override of UEngine::GetMaxTickRate (once per
// engine loop on the game thread; disc sub_8253D990, TU6 sub_8254E488 - the function whose `lfs f1,0x4C8(r3)` the
// frame-rate hook patches). Our code (MIT).
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <ue3/console.h>

#include "gow3_addresses.h"

#include <string>

REXCVAR_DEFINE_BOOL(gow3_cheats, false, "Gears3",
                    "Developer cheats on, with hotkeys: F5 god mode, F6 slow motion (cycles 1/2, 1/4, normal), F8 free "
                    "camera (photo mode), F9 freeze the world, F10 hide the HUD, F11 all weapons and ammo (keys can be "
                    "changed with --ue3_hotkeys)");
REXCVAR_DEFINE_BOOL(gow3_unlock_mutators, false, "Gears3",
                    "All Arcade mutators available without their awards (not saved to the profile)");
REXCVAR_DEFINE_BOOL(gow3_mute_unfocused, false, "Gears3",
                    "Mute the sound while another window is in front (sets audio_mute; a player's own --audio_mute=true is "
                    "left alone; gow3_app.h)");

REXCVAR_DECLARE(std::string, ue3_package_patches);  // layer 2 config_install.cpp

namespace {
// Offsets in the decompressed GearGame.xxx (86,443,270 bytes, MD5 dca12dda3cc43f2c415552054af12677), notes/25.
constexpr const char* kCheatsPatch = "GearGame.xxx@0xD4508E:28>27#GearGame.AllowCheats returns True";
constexpr const char* kMutatorsPatch =
    "GearGame.xxx@0xD084C3:7a>79#GearPC.UpdateMutatorUnlocks: every mutator unlocked";
constexpr const char* kDefaultHotkeys =
    "F5=God;F6=Slomo 0.5|Slomo 0.25|Slomo 1;F8=ToggleDebugCamera;F9=PlayersOnly;F10=ToggleHUD;F11=AllWeapons|AllAmmo";

void AddPackagePatch(const char* patch) {
  const std::string now = REXCVAR_GET(ue3_package_patches);
  rex::cvar::SetFlagByName("ue3_package_patches", now.empty() ? std::string(patch) : now + ";" + patch);
}
}  // namespace

// OnPostSetup, before Ue3InstallConfigEditsIfRequested (which serves the patched package).
void Gow3InstallExtras() {
  Ue3ConsoleTarget target;
  target.engine_global = gow3::addr::kGEngine;
  target.log_global = gow3::addr::kGLog;
  Ue3ConsoleSetTarget(target);
  if (REXCVAR_GET(gow3_cheats)) {
    AddPackagePatch(kCheatsPatch);
    Ue3ConsoleInstallHotkeys(kDefaultHotkeys);
    REXLOG_INFO("gow3_cheats: developer cheats on");
  } else {
    Ue3ConsoleInstallHotkeys();  // only what --ue3_hotkeys names
  }
  if (REXCVAR_GET(gow3_unlock_mutators)) {
    AddPackagePatch(kMutatorsPatch);
    REXLOG_INFO("gow3_unlock_mutators: every mutator unlocked");
  }
}

// UEngine::GetMaxTickRate, overridden: the original, then any queued console commands (ctx left as the original left
// it, so f1 - its result - is untouched).
#if defined(GOW3_TU6) && GOW3_TU6
#define GOW3_MAX_TICK_RATE sub_8254E488
#else
#define GOW3_MAX_TICK_RATE sub_8253D990
#endif
#define GOW3_JOIN2(a, b) a##b
#define GOW3_JOIN(a, b) GOW3_JOIN2(a, b)
extern "C" void GOW3_JOIN(__imp__, GOW3_MAX_TICK_RATE)(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void GOW3_MAX_TICK_RATE(PPCContext& __restrict ctx, uint8_t* base) {
  GOW3_JOIN(__imp__, GOW3_MAX_TICK_RATE)(ctx, base);
  Ue3ConsoleRunQueued(ctx, base);
}
