# 00 - Plan (prep, 2026-10-23 23:45)

Marks: [V] verified here, [S] read in source/notes, [G] guess.

## What we have

- Disc: our default.xex is media ID 3528321A, title 4D530A26, version 0.0.0.3, entry 0x82DBD420 [V, XEX header].
  That is the same base release OverkillLabs3 built for (their BUILDING.md names media ID 3528321A) [S].
  So every address in their project fits our base disc as-is.
- Title Update 4 package [V, file table read 23:40]: default.xexp (1,888,256 bytes, a code patch),
  GearGame\Patches\ScriptPatch_GearGame.bin (203 KB of UnrealScript fixes) + ScriptPatch_Engine.bin,
  ini changes (xbox360-geargame, xbox360-gearui, gearplaylist, gearpawnmp/ffa, gearweaponffa, GearAward)
  and text in 18 languages. Mostly multiplayer balance; whether the script patch touches the campaign is [G].
  250B2C78 is the update package's own media ID, not the disc's.
- SDK side needs nothing new: default.xexp next to default.xex patches both codegen and runtime, and
  --update_data_root mounts the package as update: (Gears 2 notes/63, SDK patch 31) [S].

## Decision for Shagg: base disc or TU4 (D3)

| | Base disc | TU4 |
| --- | --- | --- |
| OverkillLabs3's 433 hints, Bink tables, switch table, patch sites, save fix | apply directly | need remapping first (Gears 2's tools/tu_remap.py mapped 29/29 hooks and 133/210 hints for TU6; the rest are re-found by the hint loop) |
| Canary community patches | base file | TU4 file |
| Matches Gears 2 / Gears 3 | no (both ended on their title updates) | yes |
| Game content | as shipped | final patched game |

Shagg chose TU4 (23:34). The remap is done once at the very start, before any hook work (the guidebook's
"pick the release first"). Cost [G]: a few hours, mostly one remap job. Base disc is the faster first boot.

## Order of work once Shagg says go (guidebook section 2)

1. TU4 only: unpack the update package (Gears 2's stfs_extract.ps1), apply default.xexp, record the patched SHA-1,
   remap the reference hints with tu_remap.py.
2. SDK snapshot of Gears 2's install into third_party (Gears 3 job 114 as template); toolchain from Gears2-Native.
3. Copy Gears 3's layer 2 (UE3) and tools: queue_driver.ps1, sharedpc\pclock.ps1, env.ps1 (own sccache folder,
   port 4238 [G: check it is free in Gears 2's env.ps1]), scan_vtables.py, build.ps1. Write our own stale-object check (their check_objects_fresh.py shows the idea).
4. Manifest: our own vtable scan, Bink jump-table check and lis/addi callback scan (Gears 2 notes/62), cross-checked against their 433 hints,
   cr_as_local + non_volatile_as_local on.
5. Boot loop to the title screen on the shared renderer; then the must-have patches (SDK patch 1 is already in the
   snapshot; untiled flag; GPU-wait and query-poll sleeps), chapter boot, logs.
6. Mail Gears 2 for the launcher entry and per-game renderer defaults once it boots.

## Shagg's rule on outside code (04:22, all Gears games; clarified 08:27 = D5)

Never take code unless absolutely necessary. Other projects are a reference point: read, understand, write our own,
better version. Copy only when there is no other fix, tell Shagg first, keep the licence notice. Fix only problems we
see in our own game. It applies to other people's projects only: Gears 2 and Gears 3 are Shagg's own, so their code
is copied freely (D5). So for OverkillLabs3: our own vtable / callback / jump-table scans find the hints, and their list
is the cross-check (anything they have that we miss is a lead, not a paste). Their files below are reading material.

## Worth studying in OverkillLabs3 (reference only)

| What | Their file | Note |
| --- | --- | --- |
| 433 function hints, Bink jump-table heads + dispatch sites, 1 switch table | gowj_manifest.toml | base addresses |
| Save fix: XamContentCreateEx results not copied back on a first run, so saves read as corrupted | src/gowj_content_probe.cpp | check whether our SDK snapshot already handles it before porting |
| Input race guard | src/gowj_xam_input_guard.cpp | same as Gears 2 SDK patch 1, already ours |
| Stale-object check after codegen | tools/check_objects_fresh.py | cheap safety |
| Keyboard and mouse: raw look, smoothing and pitch re-centre patched out | src/gowj_kbm_input.cpp, tools/patch_raw_look.py, patch_mouse_smoothing.py | later; open for all Gears games |
| Canary patch sites verified against the original instruction | tools/patch_game_patches.py | we port as manifest hooks, not code edits |
| Coalesced bundle writer, language choice | src/gowj_coalesced.cpp | Gears 3's layer 2 already has a superset; compare |

Their known limits (CPU-bound at 90-110 fps, first-visit pipeline freezes, 720p default) are emulated-renderer
limits; the shared native renderer is how we get past them [S, their README].

## Not doing
- No game data, generated code or shader lists in git. No writes to other projects' folders.
- No builds, codegen or PC jobs until Shagg says go (D2).
