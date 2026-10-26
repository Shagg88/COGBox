# 08 - Title update (2026-10-22)

## What we have [V]
- The user's package `TU_16L625B_000001C000000.000000000008B` = "Title Update #1" (disc). Contents: notes/01.
- `default.xexp` turns disc 0.0.0.11 into **0.0.1.11**. The SDK applies it cleanly when the file sits next to
  default.xex (UserModule::LoadFromFile / runtime.LoadXexImage; codegen uses the same path). Codegen of the patched image
  ran in the workspace (5 min): 4 unresolved calls, all from base-address hints.
- 73% of the image's words differ, so all addresses move.

## It is NOT Canary's "TU6 Disc; TU1 XBL" (7775898137CE1CE3) [V]
Canary's TU file puts 16x AF at 0x8297A65C; in our TU1 image the same instruction (`lwz r5,0x344(r30)`) is at
0x8296A3C4, and 0x827C4F80 (their Occlusion Query site) is `lwz r8,0x30(r30)` here, not `lwz r11`. So disc TU1 is an
earlier update; the last disc update is TU6 [G, from Canary's file name]. Decision for the user: target TU1 (have it)
or TU6 (needs the package, e.g. from his console's hard drive).

## Moving the addresses (tools/xex_addr_map.py, shared with Gears 2) [V]
Self-test on 3,124 base function starts: 2,790 placed on a real function start in the patched build (89%), 328
reported as ambiguous/not found, 4 wrong (0.1%).
Our list (manifest + game_patches.cpp + launch_command_line.cpp, 196 addresses), results in
`08-tu1-moved-addresses.tsv`: 150 exact, 5 shifted (settled by neighbours), 19 close (same instruction, same shift as
neighbours), 10 CHECK, 8 ambiguous, 5 not found.
- All three hooks found: Gow3GpuWaitStep 0x82AF98EC -> 0x82AFB44C (exact), its jump 0x82AF991C -> 0x82AFB47C (exact),
  Gow3QueryPollYield 0x82961994 -> 0x829626DC (exact), Gow3PatchAniso16 0x82969614 -> 0x8296A3C4 (close, same
  instruction).
- Switch table 0x82FDCF18: all 11 labels moved +0x1C00, so the table is 0x82FDEB18 (tool's only candidate).
- Show-FPS data flags 0x834290D8/0x834290EC are zero-filled data: bytes can't place them; find via the code that
  reads them (Gears 2's tu_remap.py does data through lis/addi references).
- Rest of the CHECK / ambiguous / not-found entries: mostly vtable-scan hints (tiny thunks) - the build's own hint
  finders (vtable scan, lis/addi scan, boot "unregistered function" loop) re-find those anyway.

## Next
1. User decision TU1 vs TU6.
2. Switch: manifest with moved addresses + default.xexp (SDK patch 31 from Gears 2 lets it live in the update folder,
   given with --update_data_root, so game/extracted stays the plain disc) + the 50 TU content files in that folder.
3. PC job: codegen + build + boot + chapter sweep, base vs TU.

## 2026-10-22 14:42 - Gears 2: SDK patch 31 works at run time [their V]
Their TU6 build boots with the update ("XEX patch applied successfully: 0.0.0.5 -> 0.0.6.5"), ch10 and ch02 ran clean.
The first failure was an SDK bug: rex::string::utf8_ends_with_case() steps back one character too many (loop starts
at i = 0; utf8_ends_with starts at i = 1), so utf8_ends_with_case(path, ".xex") is ALWAYS false - never use it for
suffix checks. Final patch 31 = the user_module.cpp part at the end of their patches/rexglue-sdk.diff (also
patches/sdk-patch31-update-xexp.diff): looks on update: only for an .xex and only when update: is mounted; one info
line when it does. The game probes UPDATE:\<file>_us for every update file - those NtCreateFile failures are harmless.
=> Our TU1 switch is unblocked: take patch 31 at the next SDK copy, put default.xexp + the 50 TU files in one folder
given with --update_data_root, keep game/extracted the plain disc.
