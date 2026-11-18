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
//
// 0.6.3: no cheat command runs in a Versus match (ue3/match_guard.h, asked from the console before every command), including
// the System Link / online games this PC hosts and local games with bots. Judgment's GearGame package (disc GearGame.xxx,
// class export table): Engine.GameInfo > GearGame.GearGameMP_Base, whose subclasses are
//   GearGameTDM_Base (Team Deathmatch) > GearGameTDM_Newb > GearGameFFA_Base (Free-for-All) and GearGameOccupy_Base
//     (Domination); also under TDM_Base: GearGameKTL_Base > GearGameCTL_Base, GearGameWingman_Base, GearGameHunter;
//   GearGameRing_Base > GearGameAnnex_Base, GearGameKOTH_Base; GearGameBombDelivery;
//   GearGameHvB_Base (OverRun, the "hvb" / "overrun" aliases of Coalesced.ini's GameInfoClassAliases: COG vs Locust players);
//   GearGameWaveBased > GearGameHorde_Base, GearGameSiege_Base (Survival's fortifications);
//   GearGameHvB_Base > GearGameHvB_Coop_Base (the "survival" alias: waves of AI Locusts against the players).
// Campaign (GearGameSP_Base > GearGameAID_Base, Declassified too), GearMenuGame and the lobby classes are not under
// GearGameMP_Base. Not Versus, so cheats as before: everything under GearGameWaveBased and GearGameHvB_Coop_Base.
#include <atomic>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <ue3/auto_cheats.h>
#include <ue3/console.h>
#include <ue3/name_tags.h>
#include <iterator>   // std::size
#include <ue3/objects.h>  // GitHub #353
#include <ue3/match_guard.h>

REXCVAR_DEFINE_BOOL(gowj_cheats, false, "Judgment",
                    "GitHub #264 / #342: the developers' cheat keys: F9 free camera, F10 freeze the world, F11 hide / show "
                    "the HUD (keys can be changed with --ue3_hotkeys; god mode, weapons, speed: --ue3_cheat_*). Also "
                    "in a System Link / online game this PC hosts (Judgment's own AllowCheats), but never in a Versus match");

namespace {
constexpr uint32_t kGEngine = 0x83544E04u;
constexpr uint32_t kGLog = 0x834B5684u;
// 0.6.3 (ue3/match_guard.h): the object tables as adaptive_triggers.cpp opens them (TU4: GNames / GObjObjects, Gears 3's layout).
constexpr uint32_t kGNamesVar = 0x8355A328u;
constexpr uint32_t kGObjectsVar = 0x8355A368u;
constexpr const char* kLayout = "32,40,44,52,64,68,60,68,84,104,8,0,1";   // ue3::obj::LayoutText order
// Cheats run in the campaign (and Declassified), Horde, Survival (HvB_Coop), the wave modes and the front end only - every
// other game type is Versus, OverRun included (review r41: an allow-list). The party / pre-game lobbies (and the front
// end, which runs as one): refused.
constexpr const char* kAllowed[] = {"GearGameSP_Base", "GearGameWaveBased", "GearGameHvB_Coop_Base", "GearMenuGame", nullptr};
constexpr const char* kDenied[] = {"GearPartyGame_Base", nullptr};
bool OpenObjects() {
  ue3::obj::Layout layout;
  ue3::obj::ParseLayout(kLayout, &layout);
  return ue3::obj::Init(kGNamesVar, kGObjectsVar, &layout);
}
// F7 / F8 are the in-game menu's (achievements, COGBox social), F1-F4 the SDK's / ours: as Gears 2.
// GitHub #342: god mode, weapons and slow motion are the launcher's own options now (ue3/auto_cheats.h).
constexpr const char* kDefaultHotkeys = "F9=ToggleDebugCamera;F10=PlayersOnly;F11=ToggleHUD";
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
  ue3::match::Configure({"Judgment", kAllowed, kDenied, target.engine_global, target.players_data_offset,
                         target.players_num_offset, &OpenObjects});
  target.gate = &ue3::match::Check;   // 0.6.3: not in a Versus match
  Ue3ConsoleSetTarget(target);
  // GitHub #342: the launcher's cheats - also in a game this PC hosts (Judgment's own AllowCheats returns True)
  ue3::cheats::Configure({"Judgment", true, false, true, target.engine_global, target.players_data_offset,
                          target.players_num_offset, "GearPawn"});
  ue3::cheats::SetKeysOn(REXCVAR_GET(gowj_cheats));   // F11 = ToggleHUD: Hide the HUD leaves it a say
  if (REXCVAR_GET(gowj_cheats)) {
    Ue3ConsoleInstallHotkeys(kDefaultHotkeys);
    REXLOG_INFO("gowj_cheats: developer cheats on hotkeys (never in a Versus match)");
  } else {
    Ue3ConsoleInstallHotkeys();  // only what --ue3_hotkeys names
  }
}

// UEngine::GetMaxTickRate, overridden: the original (with the frame-rate hook inside), then queued console commands
// (ctx is left as the original left it, so f1 - its result - is untouched).
// The override runs with --gowj_cheats on or off; it also drives the DualSense trigger feel (GitHub #245) and the
// first-person camera.
void GowjAdaptiveTriggersTick();   // adaptive_triggers.cpp (GitHub #245, --pad_adaptive_triggers, --pad_trigger_rumble)
void GowjFirstPersonTick();        // first_person.cpp (--gowj_first_person)
bool GowjObjectsReady();           // adaptive_triggers.cpp (GitHub #353)
void GowjPollLoadingMovie(PPCContext& ctx, uint8_t* base);  // game_patches.cpp (GitHub #318, --gowj_loading_fps)
extern "C" void __imp__sub_8255E200(PPCContext& __restrict ctx, uint8_t* base);
extern std::atomic<uint32_t> g_gowj_tick_caller;  // game_patches.cpp (0.6.3 host fps)
bool GowjNoteTickCall(uint32_t caller);            // game_patches.cpp: true = the engine loop's call
extern "C" void sub_8255E200(PPCContext& __restrict ctx, uint8_t* base) {
  // 0.6.3: who asks (the guest return address). A host with remote players asks twice per frame - the engine loop and
  // UWorld::ServerTickClients; the limiter hook inside the original waits only for the engine loop's call, and the
  // per-frame work here runs only for it.
  const uint32_t caller = uint32_t(ctx.lr);
  g_gowj_tick_caller.store(caller, std::memory_order_relaxed);
  if (!GowjNoteTickCall(caller)) {
    __imp__sub_8255E200(ctx, base);
    return;
  }
  // GitHub #318: ask the game whether its loading movie is up BEFORE the original runs, so the loading-screen cap
  // (decided by the GowjMaxTickRate hook inside it) follows this tick's answer. ctx is restored afterwards.
  GowjPollLoadingMovie(ctx, base);
  __imp__sub_8255E200(ctx, base);
  Ue3ConsoleRunQueued(ctx, base);
  ue3::cheats::Tick(ctx, base);   // GitHub #342 (ue3/auto_cheats.h; off: four flag tests)
  GowjAdaptiveTriggersTick();   // GitHub #245 (adaptive_triggers.cpp; off: one flag test)
  GowjFirstPersonTick();        // --gowj_first_person (first_person.cpp; off: one flag test)
  // GitHub #353: COGBox tags after the players' names, grey (Judgment's Flash text fields render <font>, as Gears 3's)
  Ue3NameTagsTick(&GowjObjectsReady, Ue3NameTagStyle{"<font color='#808080'>", "</font>"});
}

// GitHub #353, the kill feed: the HUD keeps copies of the names in its DeathData entries and draws them through
// Canvas's text calls, which draw the text as it is - so the tag's grey markup (rendered by the Flash scenes: lobby,
// scoreboard) would show as text. These are the
// engine's text routines under Canvas.DrawText / TextSize / StrLen (exec functions 8275ED00, 8275F540, 8275F2A8 call them with the
// script's copy of the text): the original gets a copy, without the markup, of any argument holding it - the text itself, or an FString whose
// data it is - before the original runs.
namespace {
// Each text argument through Ue3NameTagsTextArg: a scratch copy without the tag's markup in place of text holding it
// (the caller's text is never changed); a register is rewritten only when its value is replaced.
void CleanTextArgs(PPCContext& ctx) {
  for (PPCRegister* r : {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10})
    if (const uint32_t v = Ue3NameTagsTextArg(r->u32); v != r->u32) r->u64 = v;
}
// The hooks act only once these Canvas natives are found at these addresses (this build's; checked at start-up).
constexpr Ue3TextHookCheck kTextHookChecks[] = {{"DrawText", 0x8275ED00}, {"TextSize", 0x8275F540}, {"StrLen", 0x8275F2A8}};
const bool kTextHookChecksSet = (Ue3NameTagsTextHookChecks(kTextHookChecks, std::size(kTextHookChecks)), true);
}  // namespace
#define TEXT_HOOK(addr)                                                             \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);     \
  extern "C" void sub_##addr(PPCContext& __restrict ctx, uint8_t* base) {           \
    CleanTextArgs(ctx);                                                             \
    __imp__sub_##addr(ctx, base);                                                   \
  }
TEXT_HOOK(8275C490)  // DrawText's
TEXT_HOOK(8275DFF8)  // DrawText's
TEXT_HOOK(8275DE88)  // TextSize's
TEXT_HOOK(8275DF28)  // StrLen's
#undef TEXT_HOOK
// The Canvas exec natives themselves: the hooks above act only inside one of these (the text routines are shared by
// much of the engine; only the script's draw calls hold the copy that is cleaned) - review r368b.
#define CANVAS_EXEC(addr)                                                                  \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);    \
  extern "C" void sub_##addr(PPCContext& __restrict ctx, uint8_t* base) {          \
    Ue3CanvasTextCall in_canvas;                                                     \
    __imp__sub_##addr(ctx, base);                                                    \
  }
CANVAS_EXEC(8275ED00)  // Canvas.DrawText
CANVAS_EXEC(8275F540)  // Canvas.TextSize
CANVAS_EXEC(8275F2A8)  // Canvas.StrLen
#undef CANVAS_EXEC
