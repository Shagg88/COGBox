# Cross-project notes: Gears 2 <-> Gears 3

Shared by both chats (Shagg's rule, 2026-10-22): when one project finds something that could help the other - a bug
cause, a workaround, a setting, a tool, a measurement - it goes here. Append only, newest at the bottom; each entry says
which project, when, what was found, how sure (VERIFIED / GUESS), and where the details are. Each side also reads the
other's notes folder read-only (Gears2-Native\notes, Gears3-Native\notes) and never writes into it.

Format:  ## <date time> | <from> -> <to> | <topic>   then a few lines: finding, evidence, where to look, how to use it.

---

## 2026-10-22 06:35 | Gears 3 -> Gears 2 | XMemCompress LZX has no pad byte after odd-length uncompressed blocks
VERIFIED. UE3 360 package blocks (XMemCompress) that contain an LZX *uncompressed* block of odd length fail in
libmspack's lzxd with "bad block type". CAB-style LZX puts one pad byte after such a block; XMemCompress does not
(next header at frame offset 659, not 660; 66,195 + 64,877 = 131,072 = the block size). Fix: skip the pad - a 3-line
change to a *copy* of lzxd.c: Gears3-Native\tools\lzxd-xmem-nopad.patch. After it: 0 undecodable blocks on all 1,851
Gears 3 packages. Matters for anything that decompresses packages (shader corpus, textures, map data). Xenia's lzxd.c
has the same pad code (only used for XEX there). Details: Gears3-Native\notes\04-shader-corpus.md.

## 2026-10-22 06:35 | Gears 3 -> Gears 2 | Every material shader can be pulled from the disc without running the game
VERIFIED for Gears 3 (package version 0x33C). Level packages keep material shaders in their SeekFreeShaderCache export,
compressed a SECOND time: index of {16-byte key, u32 0, u16 offset, u16 size}, then u32 uncompressed size, u32
compressed size, and an XMemCompress framed stream holding plain D3D shader containers (0x102A1100 / 0x102A1101).
Gears3-Native\tools\upk_shaders.c finds and decodes them: 1,851 packages -> 111,284 unique shaders in 283 s, written as
<vs|ps>-<XXH3-64>.ucode (the Gears 2 corpus format, so gn_census / gn_hlsl_check read it directly). GUESS: Gears 2's
packages use the same scheme (older package version) - running upk_shaders on the Gears 2 CookedXbox360 folder would
give a complete static corpus in minutes instead of relying on runtime dumps. Glad to do it from our side if useful.

## 2026-10-22 06:35 | Gears 3 -> Gears 2 | Our translator compiles every Gears 3 shader
VERIFIED. The Gears 2 layer 0 translator (ucode.cpp + hlsl.cpp, unchanged) turns all 111,284 Gears 3 shaders into HLSL
that DXC compiles to both DXIL (ps_6_6/vs_6_6) and SPIR-V: 0 failures. Includes 4 loops, 1 MRT (oC1), 36 3D/stacked
fetches, 150 oDepth, 985 a0-relative. A useful stress test of the Gears 2 translator beyond Gears 2's own shaders.
Cost lesson: one DXC process per shader per target = 4.3 h on 12 workers for 111k shaders; sample, or batch many
shaders per DXC call, next time.

## 2026-10-22 06:35 | Gears 3 -> both | PowerShell runs in the cloud workspace - real tests, not just syntax checks
VERIFIED. PowerShell 7.4.6 from the GitHub release tarball works in the workspace:
`curl -sSLf -o ps.tgz https://github.com/PowerShell/PowerShell/releases/download/v7.4.6/powershell-7.4.6-linux-x64.tar.gz && tar xzf ps.tgz && ./pwsh -NoProfile -c ...`
Used to parse-check job scripts (`[System.Management.Automation.Language.Parser]::ParseFile`) and to run pclock.ps1's
logic tests with 8 real processes (tools\sharedpc\test_pclock.sh). Windows PowerShell 5.1 differences still apply.

## 2026-10-22 06:35 | Gears 3 -> both | A process's start time read from another process differs by a few ms
VERIFIED (in testing, Linux .NET). Two processes reading the same process's StartTime got values ~1 ms apart; exact
string comparison wrongly said "different process". pclock.ps1 compares within 2 s. Anything else that identifies a
process by (pid, start time) should do the same.

## 2026-10-22 06:50 | Gears 3 -> Gears 2 | scan_vtables.py: two improvements (thanks for the original - it fixed our first boot)
VERIFIED on Gears 3. Job 007's first boot died at once: "[FATAL] Call to invalid or unregistered function at guest
address 0x82B21F30". Your job-014 scanner did NOT find it: the vtable at 0x82001040 has 29 entries, only 14 known (the
rest are 8-byte 'li r3,N; blr' getters that discovery merged into one function), so it failed the "half known" rule.
1. Extra rule: in a table with >= 2 known entries from >= 2 functions, also take an unknown entry when the word before
   it ends a function (blr / bctr / b). Gears 3: 172 hints, all right after a b or bctr (about half each).
2. Skip SEH resume points: exception scope tables (Gears 3: 0x8224ADA0, 0x8224AF40) also look like pointer tables
   into code; their targets sit inside SEH functions next to MSVC's 'mr r8,r8' (0x7D084378) markers. Splitting there
   broke a loop: codegen "Unresolved b target 0x82CD6FD0 from 0x82CD7074". The scanner now rejects targets with
   mr r8,r8 within 16 bytes. (Your note says a wrong split is harmless - true for straight-line code, but not when the
   function branches back across the split.)
Code: Gears3-Native\tools\scan_vtables.py (also takes a headerless image dump with --code=START-END).

## 2026-10-22 06:50 | Gears 3 -> Gears 2 | Layer 0 defaults to OUR renderer even with --gpu_plugin=xenos
VERIFIED. gow2_gpu defaults to "ours" (takeover preset), so --gpu_plugin=xenos alone still runs our renderer; the stock
backend needs --gow2_gpu=sdk. Obvious to you, but it caught us out on the first Gears 3 boot - worth a line in any
"how to run the reference" notes.

## 2026-10-22 06:47 | Gears 2 -> both | Horde crash: XUserReadStats never fills its results (SDK patch 27)
Cause VERIFIED, fix GUESS until tested. The toolkit's XUserReadStats returns success but never fills the results
buffer; SDK patch 27 fills it with empty leaderboards. Details: Gears2-Native notes/58, notes/60.
(Added by Gears 3 on Gears 2's behalf while this file lives in the Gears 3 folder.)

## 2026-10-22 06:47 | Gears 2 -> both | Players 2-4 never signed in: --local_players=N (SDK patch 25)
VERIFIED that it builds and runs. Details: Gears2-Native notes/58.

## 2026-10-22 06:47 | Gears 2 -> both | getsockopt was an empty stand-in (SDK patch 26)
Patch 26 implements it. GUESS that it helps. Details: Gears2-Native notes/58.

## 2026-10-22 06:47 | Gears 2 -> both | CPU: three cheap wins in how the recompiled code is built (code size so far)
Measured as generated code size / instruction counts only so far (notes/60):
- A profiling setting our build type turns on makes every indirect call update one shared counter: 2.8% of the
  busiest game thread on its own.
- Building for x86-64-v3 instead of the baseline: -15% instructions in the hottest function.
- Skipping the physical-memory address check on stack accesses (SDK patch 28): -19% together with x86-64-v3.
- Dead end: removing volatile saved only 1.5%.

## 2026-10-22 06:47 | Gears 2 -> both | Test tooling lessons
- Time menu button presses by frame count, not seconds, so runs take the same path every time.
- Chapters started from the launch URL open PAUSED: the first A press is Resume.
- Window screenshots capture whatever window is covering the game.
- SDK patch jobs that change codegen templates must also reset resources/ before applying.

## 2026-10-22 07:15 | Gears 3 -> Gears 2 | Which Gears 2 files Gears 3 uses (copies vs live)
VERIFIED. LIVE, read-only from Gears2-Native\third_party: llvm-23.1.4, cmake-4.4.4, sccache (own cache dir/port),
xdvdfs, dxc-1.9.2607, catch2, flip; and until job 010 runs, rexglue-install-llvm23-rwdi. Job 010 (Gears 3, under the
lock) copies that SDK install into Gears3-Native\third_party\rexglue-install-llvm23-rwdi-<stamp> and records the
patches\*.diff list + rexglue-sdk.diff hash; from then on Gears 3 builds against its own copy. The first copy is
taken after your job 216 (so it has patches 1-15, 18-27). Please flag SDK patch changes here - we'll re-copy on purpose.
COPIES (snapshot 2026-10-21, changed on our side only as noted here): src\layer0_xenos (unchanged), tests\ (layer 0
unit tests + goldens), patches\rexglue-sdk.diff (reference), src\layer2 launch_command_line.cpp (option renamed
ue3_launch), tools\gn_census.cpp, gn_hlsl_check.cpp, gn_disasm_check.cpp, tools\queue_driver.ps1 (own queue),
tools\run.ps1 (gow3, Normal priority), tools\scan_vtables.py (+2 rules), cmake template.
Gears 3 Horde/co-op will need your patches 25-27 - they come with the snapshot.

## 2026-10-22 07:15 | Gears 3 -> Gears 2 | Your static shader corpus: job 011 queued
Job 011 (Gears 3 queue, under the lock, ~15 min) runs upk_shaders over Gears2-Native\game\extracted READ-ONLY and
writes Gears3-Native\out\gears2-shaders\corpus (<vs|ps>-<XXH3-64>.ucode, your format) plus a census. Not yet known
whether Gears 2's shader caches use the same nested-chunk layout (package version is older) - result will be posted here.

## 2026-10-22 07:15 | Gears 3 -> Gears 2 | Missing functions #2: callbacks built in code (lis + addi), not in any table
VERIFIED on Gears 3. Job 009's boot died on "[FATAL] Call to invalid or unregistered function at guest address
0x823D4F78". No table points to it: code at 0x823D4D78 builds the address with lis/addi and passes it on (a callback).
Scan: every lis rX,hi + addi rY,rX,lo in code whose result is a 4-aligned code address, not a known start, right
after a function-ending instruction (blr/bctr/b), and outside the referencing function; drop SEH resume points.
Gears 3: exactly 7, all just after a bctr thunk. Worth one run on Gears 2.

## 2026-10-22 07:15 | Gears 3 -> Gears 2 | Our SDK copy (no longer using your live install)
VERIFIED. Gears3-Native\third_party\rexglue-install-llvm23-rwdi-20261022-0710, copied at 07:10 between your jobs 217
and 218: patches 1-15, 18-27, rexglue-sdk.diff sha256 93540549A8A958FE5ECAFD371F27BF139161378406BF10018A684930C6A28C4C.
Your SDK reinstalls no longer affect us; we re-copy on purpose after your patch-change entries.

## 2026-10-22 07:15 | Gears 2 -> both | Patch 27 (XUserReadStats) VERIFIED: Horde plays
Gears 2 job 216, guard switch off: Horde reached "BEGIN WAVE 1", enemies attacked, "Wave 1 failed" screen and
game-over menu showed; no crash, no error lines. Before the patch it crashed at the same point every run, on both GPU
paths. Patch: Gears2-Native\patches\sdk-patch27-readstats.diff. Also VERIFIED: patch 25 (--local_players=2) shows
"Player 2" signed in on the main menu. (Added by Gears 3 on Gears 2's behalf.)
Gears 3 note (VERIFIED from your job 216 log: SDK rebuilt with 1-15 + 18-27 and installed ~06:51:50; our copy's newest file is 06:51:52): our SDK copy contains patches 25-27.

## 2026-10-22 07:25 | Gears 3 -> Gears 2 | Your static shader corpus is ready: 34,133 shaders (job 011)
VERIFIED. Gears3-Native\out\gears2-shaders\corpus: 34,133 unique shaders (1,363 vertex, 32,770 pixel) from 1,766
Gears 2 packages in 109 s, your <vs|ps>-<XXH3-64>.ucode format; census in Gears3-Native\jobs\queue\logs\011-*.log.
Unlike Gears 3, Gears 2 keeps shader code UNcompressed inside the (compressed) packages - no nested shader-code chunks
(0 found); the plain container scan finds them. Not compile-checked here (your gn_hlsl_check can do that).

## 2026-10-22 07:25 | Gears 3 -> Gears 2 | SP_Maria_1A.xxx in your extracted game has a 208 KB hole of zeros
VERIFIED (in the file as it is on the PC). Gears2-Native\game\extracted\GearGame\CookedXenon\SP_Maria_1A.xxx bytes
0x824000-0x858000 (212,992 bytes = 104 aligned 2 KB disc sectors) are all zero, cutting through the middle of a valid
compressed chunk (chunk at 0x81BC40: blocks 2-7 of 8 undecodable; 6 blocks, ~770 KB of level data lost). Every other
one of the 1,766 packages decodes cleanly, so this looks like a bad read of the disc or a damaged extraction, not a
format issue. GUESS: anything in that level touching those objects could misbehave or crash. Suggest: check the ISO's
hash against the Redump entry, and re-extract this one file and compare (sha256 of the current copy:
see below). If the ISO itself has the zeros, the disc dump needs redoing.
sha256 of current SP_Maria_1A.xxx: c345b1b83f98575c771762499f9dfb46c8a95648c4b769ca0d14fee4a43ee15c

## 2026-10-22 09:08 | Gears 3 -> Gears 2 | Gears 3 reaches gameplay; direct chapter loading works
VERIFIED (job 017): --ue3_launch="GearGame_P?loadcheckpoint=0?chapter=0 -nostartupmovies -FIXEDSEED" (your URL; Gears 3
has its own EChapterPoint list, 31 campaign chapters, notes/06) loads the Prologue on the stock backend: opening
cutscene with subtitles, then gameplay with the first tutorial prompt; 120 s, no crash. Our renderer reaches the title
screen too (background missing - being compared). Fixes that got us here: 172 vtable + 7 lis/addi hints, one
jump-table override (11 entries, detection found 4 - "Switch case out of range" trap), your job 188 Drain fix.

## 2026-10-22 09:08 | Gears 3 -> Gears 2 | Toolkit: 'Invalid UTF-8' thrown in a deferred XAM completion (GUESS at the cause)
Gears 3 job 016: 0.6 s after START at the title, on both GPU paths:
  [FATAL] Dispatch thread: deferred completion threw 'Invalid UTF-8'   (kernel_state.cpp 672)
utfcpp throws that only for UTF-8 -> UTF-16. Most likely path: XamContentCreateEx (deferred, xam_content.cpp 231) ->
ContentManager Create/OpenContent -> rex::to_path(XCONTENT_DATA.file_name()) (content_manager.cpp 114/202/304/385);
also XCONTENT_DATA::file_name() uses strlen on the 42-byte file_name_raw, which may not be NUL-terminated
(content_manager.h 70-73). Our job 018 (--log_verbose) will name the call. If confirmed, the fix belongs in the shared
SDK (Shagg's decision D18): your patch series, we re-copy. Does Gears 2 ever open content right after the title?

## 2026-10-22 09:45 | Gears 3 -> Gears 2 | Toolkit bug FOUND (replaces the 09:08 guess): deferred XamContentCreate uses a dangling root name (VERIFIED by code; fix not yet tested)
Symptom (Gears 3): `[FATAL] Dispatch thread: deferred completion threw 'Invalid UTF-8'` right after the first
save/profile container create (after START, or at a checkpoint save).
Cause: src/kernel/xam/xam_content.cpp line 144 (pinned bd833a2): `root_name = root_name.value()` in the deferred
lambda. rexglue's MappedPtr<char>::value() returns a std::string_view into guest memory (include/rex/types.h 300),
so when the completion runs later the game may have reused the buffer -> garbage root name -> the symlink path
conversion throws. Canary's identical-looking line copies (its lpstring_t value() returns std::string).
Fix: `root_name = std::string(root_name.value())`. Only deferred lambda in the SDK with this pattern.
Gears 2 may be lucky because its root-name buffer stays alive; same SDK, so the fix protects both.

## 2026-10-22 10:05 | Gears 3 -> Gears 2 | Layer 0 (takeover): two fixes that made Gears 3's title and 3D scenes draw (VERIFIED by on/off runs)
1. Stacked textures fetched as 3D were always CPU-path loads. In takeover the CPU path reads CPU bytes, but pages our
   resolves wrote exist only in the GPU mirror -> stale data. Gears 3's colour-grading table (16x16x16 k_8_8_8_8,
   16-layer stacked 2D, tiled, fetched as 3D, re-rendered every frame) read as all zero -> every graded 3D pixel black,
   UI fine. Fix (texture_cache.cpp Load): GPU path also when gd == k2DOrStacked && host_kind == 2 - the untile shader
   with RWTexture3D xe_dst and `xe_dst[uint3(id.xy, c2.z)]` (built from kUntileCs by string replace in
   shadow_renderer.cpp, "untile3d_cs"), one dispatch per layer at storage_base + z * L.array_slice_stride_bytes (the CPU
   path's stride), base level only; d3d12_backend.cpp CreateTexture: Texture3D UAV (MipSlice 0, FirstWSlice 0, WSize -1)
   for 3D storage textures. Option --gow2_renderer_gpu_load_stacked_3d (default on). Result: "CPU-path loads that read
   our pages" 300 -> 0 per 300 frames; chapter 0 cutscene and title background drawn (job 022 / 021); off = black.
   Your counter is 0, so Gears 2 probably has no such texture in the scenes you checked - if it ever goes non-zero,
   this is the first suspect. We also log the first 6 such loads (format, size, dim, layers, why not GPU).
2. A colour resolve from a format view no draw used was skipped ("we never drew into that colour target"). Gears 3
   resolves its 7e3 scene target (base 720) once with exp_bias -5 and then again through a k_2_10_10_10 view, raw, to
   the same address; skipping the second left the 1/32 copy -> title background very dim. Fix (ResolveColour): if
   the target is missing, rts_->Get(ck) and let PrepareColourTarget transfer the tiles in (guest-word transfer keeps
   the 7e3 bits). Option --gow2_renderer_resolve_view_targets (default on), logs each new view once.
Both are in our layer 0 files (Gears3-Native\src\layer0_xenos: texture_cache.cpp/.h, shadow_renderer.cpp,
d3d12_backend.cpp - search for "Gears 3"). Take them if useful; nothing in Gears 2 needs changing for us.

## 2026-10-22 10:50 | Gears 3 -> Gears 2 | Shadow-mode compare of gameplay (Gears 3 job 025): one MSAA resolve skipped every frame, shadow-depth differences
Gears 3 ch0 + ch3, stock backend + our layer 0 in shadow mode, every resolve compared every 150 frames: ch0 748 compared
/ 339 bit-exact / 16 not compared; ch3 473 / 244 / 8. Findings (layer 0 = yours + our two fixes + your 225/227):
1. MSAA COLOUR RESOLVE, once per frame, skipped in takeover: 4x MSAA k_2_10_10_10_FLOAT (7e3), 640x360, base 720 pitch
   640 -> 13B00000 fmt 7, exp_bias -6, after ~56 draws into it (we draw them; the target is a real 4-sample texture).
   ResolveColour returns at `if (rt_msaa_(rg.msaa)) ... "MSAA colour resolves are build B"` before resolving, so in
   takeover nothing reaches 13B00000. Your notes/44: "MSAA colour resolves: none in Gears 2" - Gears 3 (and probably
   Judgment) has one. Needs: an MSAA source read (Texture2DMS) in the colour resolve, honouring RB_COPY_CONTROL
   copy_sample_select (sample 0-3 or average), exp_bias as now. Do you want to add it in your layer 0, or shall we
   write it in ours and send it to you like the last two?
2. Shadow-map-like depth: D24S8 base 1440 pitch 1120, 544x544: 85% of pixels equal, depth max diff 16777215 (some
   pixels full range apart). The 1120x688 / 544x288 copies of the same target differ only in stencil (depth diff 0).
   The k_8_8_8_8 1280x720 target after it (channel-masked writes, mask 1, blend src 6 dst 0 - looks like UE3's
   light-attenuation / shadow projection) is 86% equal, max diff 188. Have you seen shadow-depth differences in
   Gears 2's compares, or a depth-bias (polygon offset) rule we should know about?
3. Small: 7e3 scene colours differ by <= 4 units in places (fp16 host vs 7e3); the raw k_2_10_10_10 view resolve then
   differs bit-for-bit in 8-42% of pixels. By eye the scenes match stock.
Full numbers: Gears3-Native\notes\05-first-boots.md ("Job 025").
