# 24. Title Update 6 (0.0.6.11)

Shagg (14:16, 2026-10-24): add TU6 - "Yes please and double check your work!". Same design as Gears 2's TU6
(Gears2-Native notes/63): a second build of the same project, the disc build unchanged.

## The package [V]
`game\tu6\TU_16L625B_000001C000000.00000000001GB`: STFS "LIVE", title 4D5308AB, 101 files, extracted to
`game\tu6\package` (tools/stfs_extract.py): `default.xexp` (patches 0.0.0.11 -> 0.0.6.11), `GearGame\Patches\
ScriptPatch_{Engine,GearGame,IpDrv}.bin` (+ `_us`), `TU__*.ini` and text in 12 languages.
- The TU ini files do not touch anything our ini edits change: Xbox360-GearEngine only adds three SkippableMovies;
  the rest is multiplayer, tutorials, awards, DLC menus, and [GearGame.GearPC] Rotation/Targetting/Zoom sensitivity
  Low/Medium/High (the update's own aim-speed change).
- What TU6 brings [S: the package contents]: Epic's script and balance fixes and DLC menu support. Nothing in it is
  aimed at our rendering problems [G].

## What Epic's script patches change (2026-10-24, read from the decompressed ScriptPatch_*.bin) [V names / G meaning]
ScriptPatch_Engine (11 new names, 10 functions), _IpDrv (12 / matchmaking DLC playlists), _GearGame (663 new names,
458 functions; most are multiplayer, Horde/Beast, menus, DLC, achievements, awards). Campaign-relevant ones:
- Checkpoints: Engine InterpActor:ShouldSaveForCheckpoint + new bDisableCheckpointSaving; GearPC:SaveCheckpoint /
  ApplyCheckpointRecord; GearAI:ApplyCheckpointRecord; SeqAct_Checkpoint:Activated.
- Falling / getting stuck: GearPawn:FellOutOfWorld + LastFellOutOfWorldTime, GearPawn:Landed +
  HackLastTimeWeJumpSplatted, new PawnBlockingVolume, mantling (bMantleOverWall, BS_Mantle_HighWall), grappling-hook
  climb, PhysicsVolume:PostBeginPlay.
- Pause / cutscenes: GearPC:SetPause, GearHUD_Base:UnPauseGame, GearPC:SetCinematicMode, GearPawn:OnToggleMatineeMode.
- Enemies / combat: Berserker (damage, collisions, Hammer of Dawn reaction), Brumak, Reaver, Silverback, emergence
  holes (spawning, closing, camera shakes), revive / DBNO / chainsaw / pickup special moves, several weapons.
- Rendering-adjacent, script only: GearPC:OverrideTranslucentHair (exists on the disc, changed in TU6) - a lead for
  the "glisten around the character outline" Shagg saw with SMAA [G]; nothing else touches rendering.
- Chapter progress / achievements: GearCampaignActData:InitializeChapters, CompleteChapterByDifficulty,
  MarkCoopChapterComplete, achievement progress functions.

## How it is built and run
- `tools\build.ps1 -Tu6`: copies the disc's `game\extracted\default.xex` and `game\tu6\package\default.xexp` into
  `game\tu6\patched` (the SDK's codegen applies a .xexp next to the .xex), configures with `GN_GAME_VERSION=tu6`
  (cmake/rexglue.cmake: `gow3_manifest_tu6.toml`, `generated\tu6`; CMakeLists.txt: `GOW3_TU6=1`), builds
  `build\<Config>-tu6\gow3.exe`, logs `out\codegen-tu6.log` / `out\build-tu6.log`.
- `tools\run.ps1 -Tu6`: that exe with `--update_data_root=game\tu6\package`. At load the SDK (Gears 2's SDK patch 31)
  finds `update:\default.xexp` and applies it to the disc's default.xex, so the running image matches the code.
  Saves and the game folder are the same as the disc build's.
- Layer 3 data addresses: `src/layer3_gow3/gow3_addresses.h` (show-FPS flags, widescreen aspect float).

## Address check (2026-10-24) - every entry re-found, not just mapped
Tools: `tools/xex_addr_map.py` (first pass, base image vs TU6 image from the codegen dump), then each kind checked by
the method that found it on the disc:
- **Hooks (13 sites + 2 jump targets) [V]**: the same instructions in both images, read side by side (9 instructions
  around each). Eight agree with the public TU6 patch list (xenia-canary/game-patches "TU6 Disc"): aniso 0x8297A65C,
  frame-rate 0x8254E4A8, dynamic shadows 0x827E0A88, scene height 0x82971C50 (they write the halfword at +2),
  size check 0x827D7ACC, video 0x82BDD3BC, ConstrainAspectRatio 0x8262CDF0, FOV 0x8262CE80; light-environment
  shadows: they patch the lwz just before our rlwinm (0x8237D02C vs our 0x8237D030), as on the disc.
  - The scene-height global moved (disc 0x8339242C -> TU6 0x833A2BD8; the size check now reads `lwz r30,0x2bd8(r9)`).
    Our hooks only set registers, so no code change.
  - Gears 3's own tiling-flag store (why Gears 2's black-shading fix isn't needed) is still there: 0x82971B6C stores 0
    to 0x83439358.
- **Tail-call hints [V]**: a TU6 codegen with no hints reported exactly two unresolved branches, 0x82DCB448 and
  0x82DE11F0 = the disc's two hints mapped.
- **Function-pointer table hints [V]**: the mapper's "close" results for tiny thunks were wrong (it landed on a
  neighbouring thunk that the TU6 codegen already knew), so the list is a fresh tools/scan_vtables.py run on the TU6
  image with a hint-free TU6 codegen's function list: 169 entries (disc: 170). The rules were checked by running the
  same blind scan on a hint-free disc codegen (below).
- **Callbacks built with lis/addi [V]**: the same scan on the TU6 image finds 7, each the disc entry's counterpart.
- **Switch table [V]**: bctr 0x82FF0918, 11 entries read from the TU6 image (the disc's +0x13A00; same code).
- **Data [V]**: show-FPS flags 0x83439898 / 0x834398AC (the same three / two readers with the same instructions,
  +0x10630); aspect float 0x820CDED8 (1.77778, no direct reader; the 4th of the four aligned 1.77778 constants in both
  images, and the public TU6 list's address). WriteGuestFloat now checks the old value and leaves a wrong address
  alone with a warning.

## Results
- The scans were checked blind on the disc first [V]: a hint-free disc codegen reports exactly the disc's two tail-call
  hints; scan_vtables.py on it finds exactly the disc manifest's 170 table hints (none missing, none extra); the
  lis/addi scan finds exactly the disc's 7 callbacks. The same scans on TU6 gave 2 / 169 / 7.
- Local codegen with gow3_manifest_tu6.toml [V]: "XEX patch applied successfully ... 0.0.6.11", no analysis errors,
  no unresolved branches, 461 files; all 12 hook functions emitted; the switch at 0x82FF0918 has its 11 cases.
- game_patches.cpp syntax-checked for both versions (mingw clang, -fsyntax-only) [V].
- PC job 122 (15:02) [V]: TU6 build ok (codegen 100 s, compile 220 s, 0 error lines, "XEX patch applied ...
  0.0.6.11"); build\RelWithDebInfo-tu6\gow3.exe 81 MB. Runs with the update mounted: "update:\default.xexp found,
  applying it", "gow3: built for game version TU6 (0.0.6.11)", ini edits and the 200 MB texture pool in place. Main
  menu 60 s and chapter 1 150 s: 0 FATAL, 0 crash files, no [error] lines (the job's count matched words like
  "Error" in names); ch1 shots show gameplay on the ship at ~210 fps, picture normal. The NtCreateFile failures for
  'UPDATE:\<file>_us' are the game probing optional uncompressed-size files the package doesn't have (Gears 2's TU6
  does the same) [G, harmless]. Windows Firewall asked about the new exe path (first run of build\RelWithDebInfo-tu6).

## Still to do
- Launcher "Game version" choice for Gears 3 (the Gears 2 chat owns the launcher): mail sent.
- A TU6 play test by Shagg before it becomes the default.
