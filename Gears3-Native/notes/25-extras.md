# 25. Extras: developer cheats, photo mode, all mutators

Shagg (15:09, 2026-10-24): "Yes start on the extras" - cheats, photo mode, unlock all mutators (from notes/13's list).
Every switch is off by default. Designed so Gears 2 and Judgment can use the same layer 2 code with their own
addresses / offsets.

## How each part works
- **Console commands from the PC side** (layer 2, `ue3/console.h`) [V addresses, G until job 124]: the retail game reads
  no keyboard, but the engine still runs console commands through ULocalPlayer's FExec::Exec. We call it the way the
  engine's own `-EXEC=` start-up code does (disc 0x829A09DC / TU6 0x829B2024, same instructions): GEngine (disc
  0x834290BC, TU6 0x8343987C) -> GamePlayers at +0x4A0 / count +0x4A4 -> player 0 -> FExec at +0x3C -> vtable slot 0,
  r4 = the command (big-endian UTF-16), r5 = GLog (disc 0x8339A498, TU6 0x833AA864). Called on the game thread from our
  override of UEngine::GetMaxTickRate (disc sub_8253D990, TU6 sub_8254E488), after the original; the guest context is
  restored afterwards. Sources: SDK keybinds (`--ue3_hotkeys`, movable in the SDK settings as bind_ue3_N), `--ue3_exec`
  (also from the SDK console), `--ue3_exec_at` (scripted tests, ticks or seconds).
- **Cheats** (`--gow3_cheats`) [V bytes]: GearGame.AllowCheats is `04 28` (return False) at 0xD4508E of the decompressed
  GearGame.xxx; patched to `04 27` (return True). PlayerController.AddCheats then creates the cheat manager (58 engine
  + 27 Gears commands, notes/13). Default hotkeys: F5 God, F6 Slomo 0.5 / 0.25 / 1, F8 ToggleDebugCamera (photo mode),
  F9 PlayersOnly (freeze the world), F10 ToggleHUD, F11 AllWeapons / AllAmmo. (F3, F4, F7 and ` are the SDK's.)
- **All mutators** (`--gow3_unlock_mutators`) [V bytes and native index]: GearPC.UpdateMutatorUnlocks sets
  bHasUnlockedMutator = (UnlockAwardName == "") || AwardManager.HasEarnedAwardByName(...). Its `==` (native 0x7A,
  EqualEqual_StrStr) at 0xD084C3 becomes `>=` (0x79, GreaterEqual_StrStr - both indices read from Core.xxx): always
  true. Nothing is saved to the profile.
- **Package patches** (layer 2, `ue3/package_patch.h`, `--ue3_package_patches`): the package is decompressed (our own
  LZX decoder, `lzx.cpp` - the SDK's libmspack copy can't read XMemCompress's odd uncompressed blocks; ours reproduces
  GearGame.xxx and Core.xxx byte for byte against tools/upk_decompress, MD5 dca12dda... / 65aa8694...), patched (each
  patch checks the bytes it expects), stored plain under <user data>\ini-edits\CookedXbox360\ and served through the
  same folder overlay as the ini edits, with its table-of-contents line given the new size, MD5 and uncompressed size
  0 (how the disc lists the packages it stores plain, e.g. MPCH_*_SF.xxx) [G until job 124: that the game then reads
  it plain]. A note beside the copy caches the result, so later starts skip the ~1 s decompress.
- Title Update 6: neither patched function is in TU6's ScriptPatch_GearGame.bin, and TU6 keeps the disc's GearGame.xxx,
  so the same offsets serve both.

## Tests
- Local: package patch unit test (decompress + both patches) [V]; syntax check of every changed file, disc and TU6 [V].
- Job 124: test builds RelWithDebInfo-x / RelWithDebInfo-tu6x; menu + ch1 with cheats (scripted God, Slomo, debug
  camera, HUD, freeze, weapons; shots every 15 s), ch1 without cheats (Slomo should do nothing), TU6 with cheats.

## Results (jobs 124 / 124b, 2026-10-24 16:13-16:37) [V]
- The patched GearGame.xxx (decompressed, TOC uncompressed size 0) loads on the disc build; cached copy reused on later
  starts. Every scripted command reached the engine: God, Slomo 0.5 / 1, ToggleDebugCamera (shot 121: the engine's
  DebugCameraHUD), ToggleHUD, PlayersOnly, AllWeapons, TOGGLEUI - all "done" (Exec returned TRUE).
- Cheats gate confirmed: without --gow3_cheats, Slomo and ToggleDebugCamera are "not recognised", God works.
- TU6 (124b R3): commands work, but the log shows "XamShowDirtyDiscErrorUI called!" 5 s in - only with the patched
  package (job 122's TU6 runs had none). Guess: TU6's start-up hash table lists GearGame.xxx; patched packages now get
  the same table update as the bundles (job 126 checks).
- Photo mode: neither TOGGLEUI nor ToggleHUD removed the chapter's "OBJECTIVES" banner, and the debug camera draws its
  own CamLoc text - both still on screen. To find: what hides those (GFx scene / DebugCameraHUD).

## Results (job 126, 2026-10-24 17:43-17:53) [V]
- TU6 dirty-disc: NOT fixed. The hash-table update finds no entry for GearGame.xxx on either version ("0 start-up hash
  table entries updated"), and T1-tu6 still logs XamShowDirtyDiscErrorUI 4.5 s after the package lines, with no failed
  file read just before it. So TU6 checks the patched GearGame.xxx some other way [G: TU6's script patch
  (ScriptPatch_GearGame.bin) checking the package it patches, or a TU6-only table]. Cheats and commands still work on
  TU6 (God, Slomo done). The disc build has no dirty-disc message.
- Photo mode: ToggleHUD does not hide the debug camera's text (DebugCameraHUD / CamLoc / "Not trace hit" still on
  screen 5.6 s after ToggleHUD - shot 101 s against the log times) nor the OBJECTIVES banner.
- Leads found in the script packages (names only so far): Gears 3 has its own screenshot mode (GearScreenshotCamera,
  GearPlayerInput_Screenshot, ScreenshotMode, DelayedEnterScreenshotMode, DemoScreenshotMode); the OBJECTIVES banner is
  a tutorial (GearTutorial_Objectives, TUT_OBJECTIVES_TITLE), and tutorials have an on/off option (TutorialOptionIsOn,
  GetTutorialConfigOption, StopTutorialSystem); GearHUD has bMinimalHUD / bHideHUDFromScenes / HideHUDFromScenes.

## Gears 3's own screenshot mode (read from GearGame.xxx bytecode, 2026-10-24 18:10) [S]
Package reader: /root/g3tools/upk.py (names / imports / exports of the decompressed package; a function's flags are its
last 4 bytes, unless it is a net function).
- GearPlayerInput_Screenshot is a complete input mode for taking pictures: A runs the console command "shot";
  Y flips myHUD.bShowHUD (and looks at the pause state); X detaches the camera from the player (bDetachedFromPlayer,
  CameraLoc / CameraRot) for a free camera; B attaches / detaches a point light to the view target (LightComp,
  moved with the sticks: NewLightTranslationZ) - a fill light; Start pauses; the sticks move the camera.
- GearHUD_Base.DrawHUD checks the name ScreenshotMode (likely: no HUD while in that state) [G].
- Who turns it on: only GearDemoRecSpectator.DemoScreenshotMode (exec, flags 0x20202) - the Theater (recorded match)
  spectator, which logs "Entered screenshot mode" / "Left screenshot mode". GearPC has a ScreenshotMode state
  (BeginState saves the previous state and the pawn), and GearPlayerInput.DelayedEnterScreenshotMode runs
  ConsoleCommand("ScreenshotMode"), but nothing found in GearGame.xxx calls it and no exec named ScreenshotMode exists
  there - so in the campaign it is unreachable as shipped [G until the other script packages are checked].
- To use it in the campaign we'd need our own way in (a package patch that gives GearPC an exec, or pushing
  GearPlayerInput_Screenshot from the PC side). Not started.
- OBJECTIVES banner: a tutorial (GearTutorial_Objectives). The game's options menu has "WEAPON TUTORIALS" on/off and
  "TOOLTIP ICONS" on/off [S, Coalesced_INT]; whether either hides this banner is untested.

## Still to do
- Launcher: a "Cheats" box (hotkey list shown) and "Unlock all mutators" for every game (mail to Gears 2 once job
  124 passes); Gears 2 / Judgment addresses for the console (their -EXEC= code) and their AllowCheats bytes.
- Maybe: Unlock all chapters as a one-off button (`UnlockAllChapters` writes the profile - ask Shagg first).
