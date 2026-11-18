// Layer 3 (Gears of War 2): --gow2_cheats, the developer cheats on hotkeys (GitHub #264; Gears 3's --gow3_cheats,
// Gears3-Native layer3_gow3/extras.cpp). Off by default.
//
// No package patch: Gears 2's Engine.GameInfo.AllowCheats is the stock `return WorldInfo.NetMode == NM_Standalone`
// (GearGame does not override it), so PlayerController.EnableCheats makes a cheat manager only in a standalone game -
// and never in a System Link or online game (a host is a listen server, a client has no GameInfo at all). (Before the
// 0.6.3 fix our Horde-bots option's Back binding forced NM_Standalone and ran EnableCheats; it is now "GBA_Scoreboard |
// Friends" and touches neither - layer2 config_install.cpp.) GitHub #342: a campaign started from the menus is a listen
// server too (drop-in co-op), so EnableCheats was refused even alone; each hotkey now runs EnableCheats through
// ue3/auto_cheats.h (Ue3ConsoleTarget::enable_cheats), which shows NetMode as NM_Standalone for that one call while
// every player is on this PC, and takes the cheat manager away again if another PC's player joins.
//
// Console commands go through the engine's own path (layer 2 console.cpp): GEngine -> GamePlayers(0) -> its FExec
// subobject -> vtable slot 0, output device GLog - read from the engine's "-EXEC=" start-up code (disc 0x82840BA0..
// 0x82840C04, TU6 0x828527E4..0x82852848: `lwz r3,GEngine`, a cast, GamePlayers count +816 / data +812, `addi r3,r11,60`,
// `lwz r5,GLog`, `bctrl` on vtable slot 0). They run on the game thread from our override of UEngine::GetMaxTickRate
// (disc sub_824A2E78, TU6 sub_824A2F88 - the function whose `lfs f1,1284(r3)` Gow2MaxTickRate hooks; the hook still
// runs inside the original). Our code (MIT).
//
// 0.6.3: no cheat command runs in a Versus match (ue3/match_guard.h, asked from the console before every command): campaign
// (solo / co-op), Horde (Invasion) and the menus keep them, every Versus game type - on this PC, split screen, System Link or
// online, with or without bots - does not. NetMode is no help here (local split-screen Versus against bots is standalone, and
// a hosted Horde is a listen server), so the guard reads the GameInfo class. Gears 2's GearGame
// package (disc GearGame.xxx, class export table): Engine.GameInfo > GearGame.GearGameMP_Base > { GearGameAnnex_Base (Annex,
// King of the Hill), GearGameCTM_Base (Meatflag), GearGameHorde_Base (Horde), GearGameTDM_Base (Warzone, Execution;
// GearGameWingman_Base: Wingman) }, with Assassination = GearGameContent.GearGameKTL (a TDM). GearGameSP_Base (the campaign,
// co-op too), GearMenuGame and its lobby classes are not under GearGameMP_Base. Horde is the one PvE game type below it.
#include <atomic>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <ue3/auto_cheats.h>
#include <ue3/console.h>
#include <ue3/name_tags.h>
#include <iterator>   // std::size  // GitHub #353
#include <ue3/objects.h>   // the kill feed hooks (Read32, Readable)
#include <ue3/match_guard.h>

#include "gow2_addresses.h"

REXCVAR_DEFINE_BOOL(gow2_cheats, false, "Gears2",
                    "GitHub #264 / #342: the developers' cheat keys, in games on this PC only (never System Link / online), and "
                    "never in a Versus match (campaign and Horde only): F9 free camera, F10 freeze the world, F11 hide / show the "
                    "HUD (keys can be changed with --ue3_hotkeys). God mode, weapons, speed and a hidden HUD are --ue3_cheat_*");

namespace {
#if defined(GOW2_TU6) && GOW2_TU6
constexpr uint32_t kGEngine = 0x831455CCu;  // disc 0x8312327C (+140112, as kShowFpsFlag)
constexpr uint32_t kGLog = 0x830F0E4Cu;     // disc 0x830CF548
#else
constexpr uint32_t kGEngine = 0x8312327Cu;
constexpr uint32_t kGLog = 0x830CF548u;
#endif
// F7 (achievements) and F8 (COGBox social) are the in-game menu's; F1-F4 the SDK's / ours.
// GitHub #342: god mode, weapons and slow motion are the launcher's own options now (ue3/auto_cheats.h, always on when
// picked); the keys keep what only makes sense on a key.
constexpr const char* kDefaultHotkeys = "F9=ToggleDebugCamera;F10=PlayersOnly;F11=ToggleHUD";
// 0.6.3 (ue3/match_guard.h): cheats run in the campaign, Horde and the front end only - every other game type is Versus
// (review r41: an allow-list, so a mode nobody listed is refused). The party / pre-game lobbies: refused.
constexpr const char* kAllowed[] = {"GearGameSP_Base", "GearGameHorde_Base", "GearMenuGame", nullptr};
constexpr const char* kDenied[] = {"GearPartyGame_Base", nullptr};
bool OpenObjects() { return ue3::obj::Init(gow2::addr::kGNamesVar, gow2::addr::kGObjectsVar); }
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
  // GitHub #342: EnableCheats with the solo campaign's listen server seen as standalone for the call (ue3/auto_cheats.h)
  if (cheats) target.enable_cheats = &ue3::cheats::EnableForConsole;
  ue3::match::Configure({"Gears 2", kAllowed, kDenied, target.engine_global, target.players_data_offset,
                         target.players_num_offset, &OpenObjects});
  target.gate = &ue3::cheats::GateLocalOnly;   // 0.6.3: not in a Versus match; #342: and only this PC's players
  Ue3ConsoleSetTarget(target);
  // GitHub #342: the launcher's cheats - only while every player in the game is on this PC (Gears 2's own rule), and
  // EnableCheats on the menus' solo listen server
  ue3::cheats::Configure({"Gears 2", false, true, true, target.engine_global, target.players_data_offset,
                          target.players_num_offset, "GearPawn"});
  ue3::cheats::SetKeysOn(cheats);   // F11 = ToggleHUD: Hide the HUD leaves it a say
  if (cheats) ue3::cheats::Prepare();   // the keys' EnableCheats uses the lookup even with no launcher cheat on
  if (cheats) {
    Ue3ConsoleInstallHotkeys(kDefaultHotkeys);
    REXLOG_INFO("gow2_cheats: developer cheats on hotkeys (standalone games only: the game's own AllowCheats; never in Versus)");
  } else {
    Ue3ConsoleInstallHotkeys();  // only what --ue3_hotkeys names
  }
}

void Gow2AdaptiveTriggersTick();   // adaptive_triggers.cpp (GitHub #245, --pad_adaptive_triggers, --pad_trigger_rumble)
void Gow2FirstPersonTick();        // first_person.cpp (--gow2_first_person)

// UEngine::GetMaxTickRate, overridden: the original (with the frame-rate hook inside), then queued console commands and
// the DualSense trigger effects (ctx is left as the original left it, so f1 - its result - is untouched).
#if defined(GOW2_TU6) && GOW2_TU6
#define GOW2_MAX_TICK_RATE sub_824A2F88
#else
#define GOW2_MAX_TICK_RATE sub_824A2E78
#endif
#define GOW2_JOIN2(a, b) a##b
#define GOW2_JOIN(a, b) GOW2_JOIN2(a, b)
extern "C" void GOW2_JOIN(__imp__, GOW2_MAX_TICK_RATE)(PPCContext& __restrict ctx, uint8_t* base);
void Gow2NoteTickCall(uint32_t caller);  // game_patches.cpp (0.6.3 host fps)
extern std::atomic<uint32_t> g_gow2_tick_caller;  // game_patches.cpp
extern "C" void GOW2_MAX_TICK_RATE(PPCContext& __restrict ctx, uint8_t* base) {
  // 0.6.3: who asks (the guest return address). A host with remote players asks twice per frame - the engine loop and
  // UWorld::ServerTickClients; the limiter hook inside the original waits only for the engine loop's call, and the
  // per-frame work below runs only for it.
  const uint32_t caller = uint32_t(ctx.lr);
  g_gow2_tick_caller.store(caller, std::memory_order_relaxed);
  GOW2_JOIN(__imp__, GOW2_MAX_TICK_RATE)(ctx, base);
  Gow2NoteTickCall(caller);
  if (caller == gow2::addr::kServerTickClientsTickRateRet) return;
  // GitHub #353: COGBox tags after the players' names. Gears 2's set takes a class only: the class-wide form, put right
  // in the same frame (so before Ue3ConsoleRunQueued). Grey through its UI text markup.
  Ue3NameTagsTick([] { return ue3::obj::Ready() || OpenObjects(); }, Ue3NameTagStyle{"<Color:R=0.5,G=0.5,B=0.5,A=1.0>", "<Color:/>", true});
  Ue3ConsoleRunQueued(ctx, base);
  ue3::cheats::Tick(ctx, base);   // GitHub #342 (ue3/auto_cheats.h; off: four flag tests)
  Gow2AdaptiveTriggersTick();   // GitHub #245 (adaptive_triggers.cpp; off: one flag test)
  Gow2FirstPersonTick();        // --gow2_first_person (first_person.cpp; off: one flag test)
}

#if defined(GOW2_TU6) && GOW2_TU6  // the TU6 build's addresses; other builds draw the kill feed with the markup
// GitHub #353, the kill feed: Gears 2's HUD keeps copies of the names in its DeathData entries and draws them through
// Canvas's text calls, which draw the text as it is - so the tag's grey markup (rendered by the UI scenes: lobby,
// scoreboard) would show as text. These are the
// engine's text routines under Canvas.DrawText / DrawTextClipped / TextSize / StrLen (their exec functions 82640110,
// 82640380, 82640860, 826405D0 call them with the script's copy of the text): the original gets a copy, without the markup, of any argument
// holding it - the text itself, or an FString whose data it is - before the original runs.
namespace {
// Each text argument through Ue3NameTagsTextArg: a scratch copy without the tag's markup in place of text holding it
// (the caller's text is never changed); a register is rewritten only when its value is replaced.
void CleanTextArgs(PPCContext& ctx) {
  for (PPCRegister* r : {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10})
    if (const uint32_t v = Ue3NameTagsTextArg(r->u32); v != r->u32) r->u64 = v;
}
// The hooks act only once these Canvas natives are found at these addresses (this build's; checked at start-up).
constexpr Ue3TextHookCheck kTextHookChecks[] = {{"DrawText", 0x82640110}, {"DrawTextClipped", 0x82640380}, {"TextSize", 0x82640860}, {"StrLen", 0x826405D0}};
const bool kTextHookChecksSet = (Ue3NameTagsTextHookChecks(kTextHookChecks, std::size(kTextHookChecks)), true);
}  // namespace
#define GOW2_TEXT_HOOK(addr)                                                         \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);    \
  extern "C" void sub_##addr(PPCContext& __restrict ctx, uint8_t* base) {          \
    CleanTextArgs(ctx);                                                              \
    __imp__sub_##addr(ctx, base);                                                    \
  }
GOW2_TEXT_HOOK(8263EF40)  // DrawText's
GOW2_TEXT_HOOK(8263DF90)  // DrawTextClipped's
GOW2_TEXT_HOOK(8263EDD8)  // TextSize's
GOW2_TEXT_HOOK(8263EEA8)  // StrLen's
#undef GOW2_TEXT_HOOK
// The Canvas exec natives themselves: the hooks above act only inside one of these (the text routines are shared by
// much of the engine; only the script's draw calls hold the copy that is cleaned) - review r368b.
#define GOW2_CANVAS_EXEC(addr)                                                                  \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);    \
  extern "C" void sub_##addr(PPCContext& __restrict ctx, uint8_t* base) {          \
    Ue3CanvasTextCall in_canvas;                                                     \
    __imp__sub_##addr(ctx, base);                                                    \
  }
GOW2_CANVAS_EXEC(82640110)  // Canvas.DrawText
GOW2_CANVAS_EXEC(82640380)  // Canvas.DrawTextClipped
GOW2_CANVAS_EXEC(82640860)  // Canvas.TextSize
GOW2_CANVAS_EXEC(826405D0)  // Canvas.StrLen
#undef GOW2_CANVAS_EXEC
#endif
