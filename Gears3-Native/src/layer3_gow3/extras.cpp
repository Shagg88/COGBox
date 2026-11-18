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
//
// 0.6.3: no cheat command runs in a Versus match (ue3/match_guard.h, asked from the console before every command): the
// campaign (solo and co-op), Horde and Beast keep the cheats, every Versus game type - local split screen, System Link, online,
// private matches, with or without bots - does not, although the patched AllowCheats would let the engine allow them. Gears 3's
// GearGame package (disc and TU6 GearGame.xxx, class export table): Engine.GameInfo > GearGame.GearGameMP_Base, whose
// subclasses are GearGameTDM_Base (Team Deathmatch, Warzone, Execution - the "tdm" / "warzone" aliases of Coalesced.ini's
// GameInfoClassAliases - and GearGameHunter, GearGameKTL_Base > GearGameCTL_Base, GearGameTDM_Newb, GearGameWingman_Base),
// GearGameRing_Base (GearGameAnnex_Base, GearGameKOTH_Base), GearGameBombDelivery and GearGameWaveBased (GearGameHorde_Base,
// GearGameSiege_Base: the "horde" and "siege" / "beast" aliases; GearGameContent.GearGameSuperHorde is in the content package,
// not in this one - not checked, taken as a Horde variant by its name). Campaign
// (GearGameSP_Base, GearGameSP_Arcade), GearMenuGame and the lobby classes are not under GearGameMP_Base. Not Versus, so
// cheats as before: everything under GearGameWaveBased.
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <ue3/auto_cheats.h>
#include <ue3/console.h>
#include <ue3/match_guard.h>
#include <ue3/settings_dump.h>

#include "gow3_addresses.h"

#include <atomic>
#include <string>

REXCVAR_DEFINE_BOOL(ue3_dump_settings, false, "Gears3",
                    "Diagnostic (DLC light research): log the names of the online session properties and contexts "
                    "(every Settings class's PropertyMappings / LocalizedSettingsMappings) once, 45 s after start");
REXCVAR_DEFINE_BOOL(gow3_cheats, false, "Gears3",
                    "Developer cheats on (never in a Versus match), with the keys F9 free camera (photo mode), F10 freeze "
                    "the world, F11 hide / show the HUD (keys can be changed with --ue3_hotkeys). Needs the patched script "
                    "package. GitHub #342: god mode, weapons, speed and a hidden HUD are --ue3_cheat_*");
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
// GitHub #342: god mode, weapons and slow motion are the launcher's own options now (ue3/auto_cheats.h); the keys keep
// what only makes sense on a key (the same keys in all four games).
constexpr const char* kDefaultHotkeys = "F9=ToggleDebugCamera;F10=PlayersOnly;F11=ToggleHUD";

// 0.6.3 (ue3/match_guard.h): cheats run in the campaign (and Arcade), Horde, Beast and the front end only - every other game
// type is Versus (review r41: an allow-list, so a DLC mode such as Guardian is refused). The party / pre-game lobbies (and
// the front end, which runs as one): refused.
constexpr const char* kAllowed[] = {"GearGameSP_Base", "GearGameWaveBased", "GearMenuGame", nullptr};
constexpr const char* kDenied[] = {"GearPartyGame_Base", nullptr};
bool OpenObjects() { return ue3::obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar); }

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
  ue3::match::Configure({"Gears 3", kAllowed, kDenied, target.engine_global, target.players_data_offset,
                         target.players_num_offset, &OpenObjects});
  target.gate = &ue3::match::Check;   // 0.6.3: not in a Versus match
  Ue3ConsoleSetTarget(target);
  // GitHub #342: the launcher's cheats. God mode, speed and a hidden HUD need no cheat manager (no package patch, so they
  // work with Title Update 6); all weapons does, which only --gow3_cheats' patched AllowCheats gives. Also in a game this
  // PC hosts, as Gears 3's cheats always were.
  ue3::cheats::Configure({"Gears 3", true, false, REXCVAR_GET(gow3_cheats), target.engine_global,
                          target.players_data_offset, target.players_num_offset, "GearPawn"});
  ue3::cheats::SetKeysOn(REXCVAR_GET(gow3_cheats));   // F11 = ToggleHUD: Hide the HUD leaves it a say
#if defined(GOW3_TU6) && GOW3_TU6
  // GitHub #359: Title Update 6 runs the unpatched AllowCheats (the launcher never sends --gow3_cheats with it), so a key's
  // command that needs the cheat manager (AllWeapons, Slomo, ToggleDebugCamera, ...) is "not recognised": say why.
  if (!REXCVAR_GET(gow3_cheats)) ue3::cheats::SetUnavailableReason("not available with Title Update 6");
#endif
  if (REXCVAR_GET(gow3_cheats)) {
    AddPackagePatch(kCheatsPatch);
    Ue3ConsoleInstallHotkeys(kDefaultHotkeys);
    REXLOG_INFO("gow3_cheats: developer cheats on (never in a Versus match)");
  } else {
    Ue3ConsoleInstallHotkeys();  // only what --ue3_hotkeys names
  }
  if (REXCVAR_GET(ue3_dump_settings)) ue3::settings_dump::Start("Gears 3", &OpenObjects, 45);   // reads only
  if (REXCVAR_GET(gow3_unlock_mutators)) {
    AddPackagePatch(kMutatorsPatch);
    REXLOG_INFO("gow3_unlock_mutators: every mutator unlocked");
  }
}

// TU6 only (gow3_manifest_tu6.toml): the 20-byte SHA-1 compare before the dirty-disc error. r9 = the first differing
// byte (0 = same). Treated as same only while a package patch of ours is on, so the game's own files are always checked.
void Gow3Tu6PackageHashOk(PPCRegister& r9) {
  if (r9.u32 == 0 || !(REXCVAR_GET(gow3_cheats) || REXCVAR_GET(gow3_unlock_mutators))) return;
  static int n = 0;
  if (n++ < 8) REXLOG_INFO("gow3 TU6: package hash differs (patched package) - accepted for cheats / mutators");
  r9.u64 = 0;
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
extern std::atomic<uint32_t> g_tick_hook_hits;                           // game_patches.cpp (GitHub #313)
extern std::atomic<uint32_t> g_tick_caller;                              // game_patches.cpp (0.6.3 host fps)
void Gow3NoteTickCall(uint32_t hits_before, double result, uint32_t caller);  // game_patches.cpp
extern "C" void GOW3_MAX_TICK_RATE(PPCContext& __restrict ctx, uint8_t* base) {
  // 0.6.3: who asks (the guest return address). A host with remote players asks twice per frame - the engine loop and
  // UWorld::ServerTickClients; the limiter hook inside the original waits only for the engine loop's call.
  const uint32_t caller = uint32_t(ctx.lr);
  g_tick_caller.store(caller, std::memory_order_relaxed);
  const uint32_t hits_before = g_tick_hook_hits.load(std::memory_order_relaxed);
  GOW3_JOIN(__imp__, GOW3_MAX_TICK_RATE)(ctx, base);
  Gow3NoteTickCall(hits_before, ctx.f1.f64, caller);  // read-only: counts the ticks that did not reach the limiter hook
  if (caller == gow3::addr::kServerTickClientsTickRateRet) return;  // once per engine frame (the engine loop's call)
  Ue3ConsoleRunQueued(ctx, base);
  ue3::cheats::Tick(ctx, base);   // GitHub #342 (ue3/auto_cheats.h; off: four flag tests)
}
