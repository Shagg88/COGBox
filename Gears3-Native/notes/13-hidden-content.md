# 13 - Hidden, unused and cut content (Gears 3 base disc, 2026-10-22)

From Coalesced_INT.bin, the disc file list, GearGame.upk / Engine.upk script and the xex strings (research agent,
work files in the session scratchpad `hidden/`). [V] = seen in the files, [G] = interpretation.

## Cheats and debug commands
- [V] Cheats are off by one script byte: `GearGame.AllowCheats` = `04 28` (return false). `PlayerController.AddCheats`
  only creates the cheat manager when it returns true. Flipping 0x28 -> 0x27 would enable CheatManager (58 commands:
  AllAmmo, AllWeapons, Summon, Teleport, Slomo, ToggleDebugCamera, ...), GearCheatManager (27) and the debug camera.
- [V] Not gated by cheats (on the player controller): God, Fly, Ghost, Walk, UnlimitedHealth / UH, Kill <ai|bots|all|
  enemies|friends...>, PlayersOnly, Revive, Stuck, UnlockAllChapters, UnlockChapter, LoadCheckpoint, SaveCheckpoint,
  RestartChapter, `Cheat <cmd>`, Mofo, ShowProfileSettings, FF (friendly fire), LaughTrack, AddBot, DEBUGMP, ...
- [V] Xbox360-GearInput.ini [Engine.PlayerInput] still binds keyboard debug keys: G god, J/Alt+J/Ctrl+J fly/ghost/walk,
  K / Ctrl+K kill enemy / kill ai, O slomo 5 (Ctrl 1, Shift .5), P PlayersOnly, Shift+P stat fps, Ctrl+C show
  collision, N show dynamicshadows, number pad = weapons. Would work if the port passes keyboard keys to the engine
  under their engine names [G] - filter them if not wanted.
- [V] Engine commands in the xex: FREEZERENDERING, CAPTUREMODE, TOGGLEUI, PROFILEGPU, SHADERCOMPLEXITY, VIEWMODE,
  BENCHMARK, DEMOREC/DEMOPLAY, TILEDSHOT, TRIOVIZ*, RELOADCFG, DUMPINI. Need a command route.

## Maps
- [V] Full campaign maps on disc that start no chapter: SP_Thrashball_P (18.3 MB), SP_Maelstrom2_P, SP_Freeway_P,
  SP_Imulsion_P, SP_Airfield_P (with objectives in level_objectives.int). [G] later sections of existing chapters
  (Breakneck Run, MVP, Watery Grave) streamed mid-chapter, not cut levels - unconfirmed.
- [V] ChapterLevelNames[27]=SP_Azura_Intro_P vs [Chapter27] MapName=sp_azura_p. Chapter enum shows an older act layout
  (no Stadium, Warehouse, Rooftop).
- [V] MP: Azura, Rustlung, Blood Drive on disc but locked as DLC (DLCId=1, no bHasPurchased). Reach: ini edit or
  `MP_Azura?game=tdm` [G]. Seven more MP names in localisation with no package (Aftermath, Artillery, ClocktowerG3,
  Cove, DayOneG3, JacintoG3, RiverG3) - later DLC. MP_Training forced to cook but missing (cut Anya training map [G]).
- [V] URL game aliases: sp, coop, arcade, horde, siege/beast, tdm/warzone, annex, wingman, ctl, koth, newb/noob.

## Cut / unfinished
- [V] Annex mode: full script, `annex` alias, no content package. [V] Cobra burst rifle tuned in GearWeapon.ini,
  package missing. [V] Player titles (Loser, Captain, Cheater, Pirate, ... One..Eleven placeholders). [V] Tai Kaliso
  MP pawn spawned by name in Automation_KillMobs, no character package. [V] "POC" choice prompts (Hawk / Bulldog) and an
  MVP hat prompt (Normal / Backwards) in level_pathsplit.int.

## Config switches worth an ini edit [G]
- [GearGame.GearGame] bSkipAllMatinees=FALSE; [GearGame.GearHUD_Base] bMinimalHUD=False;
  [Engine.GameViewportClient] bDebugNoGFxUI=FALSE (hide the UI layer).

## Not checked
- Whether the five unlisted campaign maps are streamed by their chapters (extract SP_Convoy_P / SP_Stadium_P /
  SP_Maelstrom_P and look for the names). Whether the retail engine creates the ~ console.
