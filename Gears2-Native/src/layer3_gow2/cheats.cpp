// Layer 3 (Gears of War 2): --gow2_cheats, the developer cheats on hotkeys (GitHub #264; Gears 3's --gow3_cheats,
// Gears3-Native layer3_gow3/extras.cpp). Off by default.
//
// No package patch: Gears 2's Engine.GameInfo.AllowCheats is the stock `return WorldInfo.NetMode == NM_Standalone`
// (GearGame does not override it), so PlayerController.EnableCheats makes a cheat manager in a standalone game - the
// campaign, local split screen, Horde / versus against bots on one console - and never in a System Link or online game
// (a host is a listen server, a client has no GameInfo at all). The retail EnableCheats binding of our Horde-bots option
// (layer2 config_install.cpp, "Set WorldInfo NetMode NM_Standalone | EnableCheats | AddBot ...") already works that way.
// We never touch NetMode here. Each hotkey runs "EnableCheats" first (ue3/console.h before_each): the cheat manager
// belongs to the player controller, which a new level replaces.
//
// Console commands go through the engine's own path (layer 2 console.cpp): GEngine -> GamePlayers(0) -> its FExec
// subobject -> vtable slot 0, output device GLog - read from the engine's "-EXEC=" start-up code (disc 0x82840BA0..
// 0x82840C04, TU6 0x828527E4..0x82852848: `lwz r3,GEngine`, a cast, GamePlayers count +816 / data +812, `addi r3,r11,60`,
// `lwz r5,GLog`, `bctrl` on vtable slot 0). They run on the game thread from our override of UEngine::GetMaxTickRate
// (disc sub_824A2E78, TU6 sub_824A2F88 - the function whose `lfs f1,1284(r3)` Gow2MaxTickRate hooks; the hook still
// runs inside the original). Our code (MIT).
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <ue3/console.h>

REXCVAR_DEFINE_BOOL(gow2_cheats, false, "Gears2",
                    "GitHub #264: developer cheats on hotkeys, in games on this PC only (never System Link / online): F5 "
                    "god mode, F6 slow motion (cycles 1/2, 1/4, normal), F9 free camera, F10 hide the HUD, F11 all weapons "
                    "and ammo (keys can be changed with --ue3_hotkeys)");

namespace {
#if defined(GOW2_TU6) && GOW2_TU6
constexpr uint32_t kGEngine = 0x831455CCu;  // disc 0x8312327C (+140112, as kShowFpsFlag)
constexpr uint32_t kGLog = 0x830F0E4Cu;     // disc 0x830CF548
#else
constexpr uint32_t kGEngine = 0x8312327Cu;
constexpr uint32_t kGLog = 0x830CF548u;
#endif
// F7 (achievements) and F8 (COGBox social) are the in-game menu's; F1-F4 the SDK's / ours.
constexpr const char* kDefaultHotkeys =
    "F5=God;F6=Slomo 0.5|Slomo 0.25|Slomo 1;F9=ToggleDebugCamera;F10=ToggleHUD;F11=AllWeapons|AllAmmo";
}  // namespace

// OnPostSetup. Without --gow2_cheats only what --ue3_hotkeys / --ue3_exec name runs, and nothing enables cheats.
void Gow2InstallCheats() {
  Ue3ConsoleTarget target;
  target.engine_global = kGEngine;
  target.log_global = kGLog;
  target.players_data_offset = 812;  // UGameEngine::GamePlayers in Gears 2's engine (Gears 3: 0x4A0 / 0x4A4)
  target.players_num_offset = 816;
  target.exec_offset = 60;           // as Gears 3
  target.exec_vtable_slot = 0;
  const bool cheats = REXCVAR_GET(gow2_cheats);
  if (cheats) target.before_each = "EnableCheats";
  Ue3ConsoleSetTarget(target);
  if (cheats) {
    Ue3ConsoleInstallHotkeys(kDefaultHotkeys);
    REXLOG_INFO("gow2_cheats: developer cheats on hotkeys (standalone games only: the game's own AllowCheats)");
  } else {
    Ue3ConsoleInstallHotkeys();  // only what --ue3_hotkeys names
  }
}

// UEngine::GetMaxTickRate, overridden: the original (with the frame-rate hook inside), then queued console commands
// (ctx is left as the original left it, so f1 - its result - is untouched).
#if defined(GOW2_TU6) && GOW2_TU6
#define GOW2_MAX_TICK_RATE sub_824A2F88
#else
#define GOW2_MAX_TICK_RATE sub_824A2E78
#endif
#define GOW2_JOIN2(a, b) a##b
#define GOW2_JOIN(a, b) GOW2_JOIN2(a, b)
extern "C" void GOW2_JOIN(__imp__, GOW2_MAX_TICK_RATE)(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void GOW2_MAX_TICK_RATE(PPCContext& __restrict ctx, uint8_t* base) {
  GOW2_JOIN(__imp__, GOW2_MAX_TICK_RATE)(ctx, base);
  Ue3ConsoleRunQueued(ctx, base);
}
