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
//
// 0.6.3: no cheat command runs in a Versus match (ue3/match_guard.h, asked from the console before every command): the campaign
// (solo and co-op) and the menus keep the cheats, every Versus game type - local split screen, System Link, online, with or
// without bots - does not. Gears 1's WarfareGame package (TU5 WarfareGame.xxx, class export table and the Coalesced.ini
// sections): Engine.GameInfo > WarfareGame.WarGame > WarfareGame.WarGameSP (DefaultGame / DefaultServerGame = WarGameSP: the
// campaign, co-op and the menus); the Versus game types are WarfareGameContent's WarGameMP, WarGameTDM (Warzone, Execution,
// Assassination are its settings) and WarGameResurrection, which extend WarGame beside WarGameSP. So: Versus = WarGame minus
// WarGameSP. (Only the WarfareGame package is loaded at start-up, so the content classes are not looked up by name; the export
// table's WarGameSP has the export that the table puts at WarGame's place as its super; names found in the package: WarGame,
// WarGameSP, WarGameKillTheLeader, WarGameResurrection. Gears 1 has no Horde / Beast.)
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <ue3/auto_cheats.h>
#include <ue3/console.h>
#include <ue3/name_tags.h>
#include <iterator>   // std::size  // GitHub #353
#include <ue3/match_guard.h>

REXCVAR_DEFINE_BOOL(gow1_cheats, false, "Gears1",
                    "GitHub #264 / #342: the developers' cheat keys (never in a Versus match): F9 fly through walls (on / "
                    "off), F10 freeze the world, F11 hide / show the HUD (keys can be changed with --ue3_hotkeys; god mode, "
                    "weapons, speed: --ue3_cheat_*)");

namespace {
constexpr uint32_t kGEngine = 0x82FCDD50u;
constexpr uint32_t kGLog = 0x82F98D98u;
// 0.6.3 (ue3/match_guard.h): the object tables as adaptive_triggers.cpp opens them (its notes: the globals, the layout given).
constexpr uint32_t kGNamesVar = 0x82FED4F0u;
constexpr uint32_t kGObjectsVar = 0x82FDD1C4u;
constexpr const char* kLayout = "4,40,44,52,60,76,64,72,96,124,16,1,1";   // ue3::obj::LayoutText order
// Cheats run in WarGameSP only (the campaign, co-op and the front end: DefaultGame=WarfareGame.WarGameSP); every other game
// type - WarGameMP / TDM / Resurrection and anything else - is refused (review r41: their parent classes were never read).
constexpr const char* kAllowed[] = {"WarGameSP", nullptr};
constexpr const char* kDenied[] = {nullptr};
bool OpenObjects() {
  ue3::obj::Layout layout;
  ue3::obj::ParseLayout(kLayout, &layout);
  return ue3::obj::Init(kGNamesVar, kGObjectsVar, &layout);
}
// GitHub #342: god mode, weapons and slow motion are the launcher's own options now (ue3/auto_cheats.h). PlayersOnly was
// checked on the PC (jobs 1818, 1819).
constexpr const char* kDefaultHotkeys = "F9=Ghost|Walk;F10=PlayersOnly;F11=ToggleHUD";
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
  ue3::match::Configure({"Gears 1", kAllowed, kDenied, target.engine_global, target.players_data_offset,
                         target.players_num_offset, &OpenObjects});
  target.gate = &ue3::cheats::GateLocalOnly;   // 0.6.3: not in a Versus match; #342: and only this PC's players
  Ue3ConsoleSetTarget(target);
  // GitHub #342: the launcher's cheats - only while every player is on this PC (Gears 1's AllowCheats: standalone games)
  ue3::cheats::Configure({"Gears 1", false, false, true, target.engine_global, target.players_data_offset,
                          target.players_num_offset, "WarPawn"});
  ue3::cheats::SetKeysOn(cheats);   // F11 = ToggleHUD: Hide the HUD leaves it a say
  if (cheats) {
    Ue3ConsoleInstallHotkeys(kDefaultHotkeys);
    REXLOG_INFO("gow1_cheats: developer cheats on hotkeys (never in a Versus match)");
  } else {
    Ue3ConsoleInstallHotkeys();  // only what --ue3_hotkeys names
  }
}

// FEngineLoop::Tick, overridden: the original, then queued console commands (ctx left as the original left it). The
// override runs with --gow1_cheats on or off; it also drives the DualSense trigger feel (GitHub #245).
void Gow1AdaptiveTriggersTick();   // adaptive_triggers.cpp (GitHub #245, --pad_adaptive_triggers, --pad_trigger_rumble)
extern "C" void __imp__sub_8222A6E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8222A6E0(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_8222A6E0(ctx, base);
  // GitHub #353: COGBox tags after the players' names. Gears 1's set takes a class only: the class-wide form, put right
  // in the same frame (so before Ue3ConsoleRunQueued). Grey: its one grey text style, set to the names' own font.
  Ue3NameTagsTick([] { return ue3::obj::Ready() || OpenObjects(); }, Ue3NameTagStyle{"<Styles:credits_headers>", "<Styles:/>", true, "credits_headers=WarfareFonts_Euro20"});
  Ue3ConsoleRunQueued(ctx, base);
  ue3::cheats::Tick(ctx, base);   // GitHub #342 (ue3/auto_cheats.h; off: four flag tests)
  Gow1AdaptiveTriggersTick();   // GitHub #245 (adaptive_triggers.cpp; off: one flag test)
}

// GitHub #353, the kill feed: Gears 1's HUD draws each kill-feed line from the players' live names (DeathData holds the
// killer and victim, not their text) through Canvas's text calls, which draw the text as it is - so the tag's grey markup
// (rendered by the UI scenes: lobby, scoreboard) would show as text there. These are the engine's text routines under
// Canvas.DrawText / DrawTextClipped / TextSize / StrLen (their exec functions 8233BE60, 8233C058, 8233C4D0, 8233C210 call
// them with the script's copy of the text): the original gets a copy of any text argument without the markup.
namespace {
// Each text argument through Ue3NameTagsTextArg: a scratch copy without the tag's markup in place of text holding it
// (the caller's text is never changed); a register is rewritten only when its value is replaced.
void CleanTextArgs(PPCContext& ctx) {
  for (PPCRegister* r : {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10})
    if (const uint32_t v = Ue3NameTagsTextArg(r->u32); v != r->u32) r->u64 = v;
}
// The hooks act only once these Canvas natives are found at these addresses (this build's; checked at start-up).
constexpr Ue3TextHookCheck kTextHookChecks[] = {{"DrawText", 0x8233BE60}, {"DrawTextClipped", 0x8233C058}, {"TextSize", 0x8233C4D0}, {"StrLen", 0x8233C210}};
const bool kTextHookChecksSet = (Ue3NameTagsTextHookChecks(kTextHookChecks, std::size(kTextHookChecks)), true);
}  // namespace
#define GOW1_TEXT_HOOK(addr)                                                         \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);    \
  extern "C" void sub_##addr(PPCContext& __restrict ctx, uint8_t* base) {          \
    CleanTextArgs(ctx);                                                              \
    __imp__sub_##addr(ctx, base);                                                    \
  }
GOW1_TEXT_HOOK(8233B5D0)  // DrawText's
GOW1_TEXT_HOOK(8233AD88)  // DrawText's and DrawTextClipped's
GOW1_TEXT_HOOK(823A38F0)  // DrawText's and DrawTextClipped's
GOW1_TEXT_HOOK(8233CD68)  // DrawTextClipped's and TextSize's
GOW1_TEXT_HOOK(82398848)  // TextSize's
GOW1_TEXT_HOOK(8233B538)  // StrLen's
#undef GOW1_TEXT_HOOK
// The Canvas exec natives themselves: the hooks above act only inside one of these (the text routines are shared by
// much of the engine; only the script's draw calls hold the copy that is cleaned) - review r368b.
#define GOW1_CANVAS_EXEC(addr)                                                                  \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);    \
  extern "C" void sub_##addr(PPCContext& __restrict ctx, uint8_t* base) {          \
    Ue3CanvasTextCall in_canvas;                                                     \
    __imp__sub_##addr(ctx, base);                                                    \
  }
GOW1_CANVAS_EXEC(8233BE60)  // Canvas.DrawText
GOW1_CANVAS_EXEC(8233C058)  // Canvas.DrawTextClipped
GOW1_CANVAS_EXEC(8233C4D0)  // Canvas.TextSize
GOW1_CANVAS_EXEC(8233C210)  // Canvas.StrLen
#undef GOW1_CANVAS_EXEC
