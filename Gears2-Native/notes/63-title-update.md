# 63. Title Update 6 (D34)

User (12:47): start on the title update. Gears 3 is doing their TU1 at the same time (13:05); the address-matching
tool is theirs (shared), the rest is per game.

## What TU6 is [V, from the package, 2026-10-22]
`C:\Emulation\Games\Xbox 360\Updates\Gears of War 2 (USA) (v6)\63AF211E\TU_16L621D_000000K000000.00000000001G5`
(read-only): a LIVE (STFS) package of 2,846,720 bytes, 68 entries:
- `default.xexp` (2,074,624 bytes): the XEX delta patch for our exact base (0.0.0.5 -> 0.0.6.5, D34).
- `GearGame\Patches\ScriptPatch_{Engine,GearGame,IpDrv,OnlineSubsystemLive}.bin` (+ `.bin_us`): UnrealScript patches.
- `TU__*.ini` (camera, pawn, pawn MP, playlist, weapon, weapon MP, Xe-GearEngine, Xe-GearGame, Xe-GearUI) and
  `TU__{Engine,GearGame,GearGameContent,GearGameUI}.*` text in 12 languages.
- `tools/stfs_extract.ps1` (ours, MIT) lists / extracts it; checked against a separate Python reader: all 66 files
  byte-identical.

## What the SDK already does [V, from source]
- `UserModule::LoadFromFile` looks for `<xex>p` (default.xexp) next to the XEX and applies it with
  `XexModule::ApplyPatch`; the recompiler loads through the same path (`runtime.LoadXexImage`), so a default.xexp next
  to default.xex patches both the codegen and the running game.
- `--update_data_root=<folder>` mounts a folder as `update:`; the game's image references `update:` / `UPDATE:`
  (UTF-16 strings in the base image).
- No SDK change needed for either. (Corrected below: SDK patch 31.)

## Plan
1. Job 241: extract TU6 to `game\tu6\package`; load default.xex + default.xexp from a test folder (`game\tu6\patched`,
   not our main build) with `--gow2_dump_image` to confirm the patch applies and to get the patched image; the same
   for the base. Size / pages changed.
2. Re-find every game-specific address (function hints, midasm hooks, layer-3 patch sites) in the patched image -
   Gears 3's address-matching tool (code bytes with branch targets masked), checked by reading each instruction.
3. A TU build: manifest addresses updated, default.xexp in the game root, `--update_data_root` = the extracted
   package (keeping `GearGame\Patches`).
4. Full chapter sweep before it becomes the main build (the one change that cannot skip a sweep).

## Results so far
- Job 241 [V]: the package extracts (stfs_extract.ps1); "XEX patch applied successfully: base version: 0.0.0.5, new
  version: 0.0.6.5"; patched image 21,332,992 bytes (base 21,184,000), and most code moved.
- Address remap [V, by reading the instructions]: `tools/tu_remap.py` (ours, MIT - our own matcher; Gears 3's shared tool
  had not arrived and the user said not to wait). Inventory in `notes/63-tu6-addresses.txt`: 29 hooks, 210 function
  hints, 41 other references. All 29 hooks mapped and checked by disassembly (four needed a hand match: only data
  offsets differ); 133 hints mapped, 77 left out (commented in the manifest) for the build's own hint finders.
  Layer-3 data addresses (FPS flags, widescreen float, tiling flag) are in `src/layer3_gow2/gow2_addresses.h`.
- Correction to "no SDK change needed": the SDK only looks for default.xexp NEXT TO default.xex, which would mean a
  second copy of the game folder. SDK patch 31 also looks on `update:` (the mounted update folder), so the base game
  folder stays as it is and the update is only used when `--update_data_root` is given.

## How the TU6 build is made (job 242 on)
- `tools\build.ps1 -Tu6` -> `gow2_manifest_tu6.toml` (codegen from `game\tu6\patched`: default.xex + default.xexp),
  `generated\tu6`, `build\<Config>-tu6`, `GOW2_TU6=1`, logs `out\codegen-tu6.log` / `out\build-tu6.log`.
- `tools\run.ps1 -Tu6` -> that exe with `--update_data_root=game\tu6\package`.
- Hint tools: `find_hints.py <root> tu6`, `scan_vtables.py <root> <image> --tu6`, `scan_lis_addi.py generated\tu6 <image>`.
- The base build is unchanged and stays the main one until the TU6 sweep passes.
- Job 242 [V]: SDK patch 31 + base unchanged (ch10 shots identical to 240/239). TU6 codegen: "XEX patch applied
  successfully ... 0.0.6.5", then stopped on one unresolved branch to a this-adjusting thunk new in TU6 (0x82979928).
- Job 243 [V]: patch 31 made quiet (only for an .xex, only when update: is mounted; 242's base run logged one harmless
  error and the codegen looked for 'default.xexpp'); base shots identical again, 0 error lines. TU6 codegen completed
  (449 files) with one tail-chunk warning (the disc hint 0x82B3EB88 -> 0x82B9AC20, parent 0x82B9A7C0); the compile
  stopped on 5 'undeclared label' errors = the 5 disc { end } / catch-funclet pairs, each found at +0x48 before the
  label with the same 'addi r31,r12,N' start as on the disc.
- The other left-out hints, re-found on the TU6 build: tools/scan_vtables.py (now takes --init=<gow2_init.cpp>) proposes 72;
  its rules find all 162 disc vtable hints blind and confirm all 106 mapped TU6 ones, so all 72 are taken (as on the
  disc). Callbacks 0x82A155B8-F8 from an image-based lis/addi scan. Disc sweep hint 0x82ADBEF0: both TU6 copies of that
  function are already found by the codegen, so no hint.
- Jobs 244-246 [V]: TU6 compiled (0 errors) but booted at the DISC entry point 0x82AAC8D8 (TU6: 0x82AC6B28, from the
  241 dumps): the xexp was not applied at run time. 246's debug log: update: mounted, sibling lookup fails, patch 31's
  fallback never ran - its check rex::string::utf8_ends_with_case(path, ".xex") is an SDK bug (loop starts at i = 0
  where utf8_ends_with starts at 1, so it compares 5 characters with a 4-character suffix: always false). Upstream
  report material (wait for the user at the PC). Patch 31 now checks the suffix itself (job 247).
  245's lone base shot difference was a one-off (246 = 243 in all 7).
- Job 247 [V]: patch 31's fallback now runs: "update:\default.xexp found, applying it", "XEX patch applied
  successfully ... 0.0.6.5", untiled flag at the TU6 address. TU6 ch10 capped 90 s: ran to the limit, 33.33 ms frames,
  0 error lines, 9 shots; frame 900 matches the disc build almost exactly (2,441 pixels differ, 24 by more than 16).
  The NtCreateFile failures for 'UPDATE:\<file>_us' are the game probing optional US variants (only ScriptPatch_*.bin_us
  exist in the package) [G, harmless: the game carries on]. Base ch10 identical to 246 and 243.
- Job 248: the full chapter sweep on the TU6 build (tools\chapter_sweep.ps1 -Tu6 -Cpu), the step before it can become
  the main build.
- Cross-check [V] against the public patch list (github.com/xenia-canary/game-patches, "4D53082D - Gears of War 2
  (TU6).patch.toml"): every TU6 site we map that it also patches is the same address - aniso 0x8281E3B4, PoolSize
  0x8281D0E4, texture stats 0x8281D978, coalesced check 0x82AC3384 / 0x828089C0, frame-rate 0x824A2FA4, show-FPS flags
  0x831457B8 / C0, widescreen float 0x821D55A0, FOV 0x825324B4, black-shading (our untiled) 0x828165FC, scene height
  0x828166C8 (they write the halfword at +2), UI viewport 0x826E9BF8 (+8 from their 0x826E9BF0, as on the disc).
  ConstrainAspectRatio: we hook the first of three null-check branches (0x82532380, as our disc hook), they patch the
  third (0x8253239C); all three jump to 0x82532414, so the effect is the same.
