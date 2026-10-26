# 08 - Hidden or switched-off things in the shipped game (Shagg 15:10: "anything hidden we can bring out?")

Marks: [V] read in our files (Coalesced_INT.bin, disc file list, TU4 image), [G] guess. First pass, cloud only.

## Found
1. Developer menu, switched off [V]: Xbox360-GearGame.ini [GearGame.GearMenuHUD] bUseDevMenu=FALSE.
   Its scene (UIScene_DevMenu, title "Jack build", "See release notes" - Jack was Judgment's code name) still has
   10 entries in Xbox360-GearUI.ini, but they start test maps that are NOT on the disc (MP_Nicktest, MP_ThirdTest,
   MP_FourthTest, MP_5, MP_Testmap, MP_Metro, AID_Arena_P "POC for S3 system", POC_MercyCombat) or join a server at
   Epic's office address. Idea [G]: switch it on with our ini edits (the same mechanism as the trilinear setting, no
   code patch) and refill the list with maps that DO exist = a built-in level select / warp menu (every campaign and
   Aftermath map, transition maps, the multiplayer maps alone or with bots).
2. Multiplayer maps on the disc [V]: MP_Estate, MP_Gondola, MP_Island, MP_Junkyard, MP_Library, MP_Rig, MP_Skyline,
   MP_Streets (+ _FFA variants). Offline bot matches are a shipped feature - worth one short test.
3. Cheat manager [V]: GearCheatManager is in the code image; Engine.int has its messages ("Now viewing from", "Console
   commands were used which are disallowed in netplay"). The commands themselves are UnrealScript inside the game's
   packages (not readable without unpacking them). The 360 build has no console to type them. Idea [G]: find the
   script functions in the packages, then a keyboard console or a few hot-keys of our own (god mode, slow motion,
   free camera / photo mode). Research step first.
4. Multiplayer characters on the disc [V]: MPCH_* files incl. Young Marcus, Young Dom, Clayton Carmine, Trishka,
   Grayson, Epic Reaper, Jack-skin variants. Several were paid downloadable content [G] - not touched without Shagg.
5. Leftover Gears 3 dialogue in the localisation [V]: Marcus, Dom, Anya, Sam, Prescott, Jace-era chatter/dialog files
   (Human_Marcus_Dialog.int, human_dom_chatter.INT, ...). Probably engine/shared leftovers; possibly cut lines [G].
6. Engine settings off on the 360 [V]: [SystemSettings] bEnableVSMShadows=False, bEnableBranchingPCFShadows=False -
   PC-only shadow paths in UE3; likely no 360 shaders for them [G], so probably not usable.

## Not found
- No beta/prototype switches; "Beta Tester" is only an award string.

## Graphics settings the 360 build ships turned down (Shagg 15:28) [V Xbox360-GearEngine.ini / GearCamera.ini]
- bUseMaxQualityMode=False (UE3's "everything at its best" switch).
- Shadows: MaxShadowResolution=1120, MaxWholeSceneDominantShadowResolution=1344, ShadowTexelsPerPixel=1.27324.
- Draw distance: MaxDrawDistanceScale=1, DecalCullDistanceScale=1.0. Detail: DetailMode=2 (already the top).
- Field of view: GearCamera.ini FOVAngle per camera mode (gameplay 70, cover 95, roadie run 105, ...).
- MaxAnisotropy=4 (our gowj_aniso16 switch), TextureStreaming PoolSize=136 (our gowj_texture_pool_mb switch).
- Off and PC/DX11-only (no 360 shaders, very likely useless here) [G]: bAllowTemporalAA, bEnableVSMShadows,
  bEnableBranchingPCFShadows, bAllowHardwareShadowFiltering, bAllowBetterModulatedShadows, HighPrecisionGBuffers,
  AllowD3D11, MaxMultiSamples=1 / bAllowD3D9MSAA (MSAA through the engine - risky with EDRAM).
- Job 020 tries the first four groups, one run each, against a baseline.

## Level select - job 025 [V]
--gowj_level_select=true + launch GearStart (the game's front end): after START the "Jack build" dev menu replaces
the main menu (title art, "A SELECT"), but its list draws no text - the entries are there and navigable blind:
DPAD_DOWN x2 + A loaded a campaign level into gameplay (Baird indoors, mansion interior; expected = entry 3,
SP_02_Mansion_3_P; the log doesn't name the map, so not proven which). Next: find why the list text doesn't draw
(the menu's list widget / its strings), then it's a real warp menu. Job 024's try failed because SP_00_Museum_P is the
prologue map, not the front end.

## Level select - job 026 confirms [V]
Control (GearStart, level select off): the normal MAIN MENU draws fine (Campaign / Survival / Versus / ... and the
second START opens Options) - so no missing-text bug in the real front end. With --gowj_level_select the "A SELECT"
screen with no text IS the dev menu, and its order holds: A on entry 1 loaded SP_01_University_P (the street), 025's
DPAD_DOWN x2 + A loaded entry 3 (Mansion part 3, Baird indoors). Works blind; the list text is the remaining gap.

## Launcher checks - job 033 [V]
--gowj_skip_logos: StartupMovies: 1 removed (Open_Logos), LoadingMovie kept; GearStart reaches the menu, no logo movie,
0 errors. --gowj_texture_pool_mb=200: PoolSize 136 -> 200; University lowest free physical 206 MB (was 269 at 136),
0 errors. Layer 2 accepts both "Key-=Value" and Gears 3's "-Key=Value" for removing one entry.
