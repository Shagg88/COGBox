# 05 - First boots (jobs 007-014)

Each boot runs twice: A = stock backend (`--gow2_gpu=sdk`), B = our renderer (layer 0's default, `--gow2_gpu=ours`).
`--gpu_plugin=xenos` alone does NOT select the stock backend (job 007 ran ours by mistake).

| Job | Build | How far | Stopped at | Fix |
|---|---|---|---|---|
| 007 | 2 hints | guest code starts, interrupt callback set | `[FATAL] unregistered function 0x82B21F30` | vtable scan: 172 hints (notes in manifest) |
| 009 | 174 -> 172 hints | audio endpoint, `UPDATE:\` probe, `D:\Binaries\EpicInternal.txt` probe | `[FATAL] unregistered function 0x823D4F78` (callback built with lis/addi at 0x823D4D78) | lis/addi scan: 7 hints |
| 012 | 179 hints, own SDK copy | controller connected, movie probes (`Open_Logos.txt`, `LoadingMovie.xxx` - optional files; the .bik files exist), B: first frame drawn by our renderer | 0xC000001D (illegal instruction) gow3.exe+0x4D7FA29 = `__builtin_trap()` "Switch case out of range" in sub_82FDCE28 (job 013 symbolized it) | jump table 0x82FDCF1C has 11 entries, detection found 4 (index = 3 bits, or 2 bits + 7): `[[entrypoint.switch_tables]]` |
| 014 | + switch table, + Gears 2 job 188 Drain fix | **TITLE SCREEN on both renderers**: Microsoft Studios logo movie, then GEARS OF WAR 3 / PRESS START; ran the full 120 s, closed cleanly. Renderer self-test: all 7 chains bit-exact (job 188 fix). B (ours): title background (smoke/skull) black | - | B: ~6 draws/frame into k_16_16_16_16 "not modelled" - Gears 2's current layer 0 draws k_16_16 / k_16_16_16_16 (SNORM, x1/32); our copy (reuse zip, 2026-10-21 20:53) predates it -> job 015 refresh, job 016 boot + START/A presses |
| 016 | + layer 0 refresh (rebuilt), + D3D built-in shader hints | title screen; START pressed at 50 s; clears now recognised (2,400/300 frames), k_16_16_16_16 now drawn (colour-only skips 1,889 -> 89), 0 fixed16 multiplicative blends - but B's title background is STILL black | both A and B, 0.6 s after START (after an XNetLogonGetTitleID stub): `[FATAL] Dispatch thread: deferred completion threw 'Invalid UTF-8'` (a deferred XAM completion converting UTF-8 -> UTF-16) | job 018: same with --log_verbose to name the call; background: needs a frame comparison (draw log / captures) |
| 017 | same build as 016 | **GAMEPLAY on the stock backend**: chapter 0 (Prologue: cutscene + subtitles, then gameplay with the WALK tutorial) and chapter 3 (Hanover: cutscene, then gameplay in the street) both ran 120 s, no crash; direct chapter loading works (notes/06). Chapter 17 on OUR renderer: ~700-1,000 draws/frame, subtitles drawn, 3D scene BLACK | ours, ch17, 77 s in, no button: the same `'Invalid UTF-8'` FATAL - so not the menu: most likely the checkpoint save (content create) | jobs 018 (trace), 019 (shadow compare), 020 (ch0 on ours) |

Seen, not yet a problem: XamMediaVerificationVerify / ...FailedBlocks are SDK stubs, called ~2x a second from the title on (disc verification). XamVoiceSetMicArrayIdleUsers, XFileFsDeviceInformation, IoDismountVolumeByFileHandle stubs.
Harmless so far: `UPDATE:\` device not found (no title-update content mounted - we build the base disc XEX, D7);
missing optional movie subtitle files.

## SOLVED (job 014): renderer transfer self-test failed in Gears 3 (job 012 run B)
Cause (Gears 2 notes/53, job 188): DestroyTexture freed RTV/DSV slots before the threaded recording thread replayed
the calls that name them. Fix: list_->Drain() at the top of DestroyTexture. With it, job 014: all 7 chains bit-exact.
Original write-up:

At the first swap, layer 0's self-test sends 1,280 known words colour -> depth/7e3/16_16_FLOAT -> colour and reads them
back. Gears 2 (same SDK build, its job 216): all 7 chains bit-exact. Gears 3 job 012: chains 1-6 read back ALL ZERO
(1279 of 1280 "NOT bit-exact", word 0 is 0 anyway), chain 7 (D24FS8 -> 16_16_FLOAT -> D24S8 -> 8888) bit-exact.
GUESS: a timing difference - Gears 3 swaps within ~30 ms of boot - so the first chains are read before their copy
really landed, or the scratch targets (EDRAM base 0) interact with the game's first render targets (an alias warning
at frame 3). Not blocking: the self-test is diagnostic only. To check once boots go further: does it still fail at a
later first swap (e.g. --gow2_renderer_transfer_selftest delayed), and does IsReadbackReady's fence match the list the
copy was recorded in? Asked the Gears 2 chat whether they ever saw it fail.

## Lead (GUESS, desk work 2026-10-22 09:00): job 016's 'Invalid UTF-8' after START
utfcpp throws "Invalid UTF-8" only from UTF-8 -> UTF-16 (to_utf16 / Windows path conversion). Deferred XAM completions
in rexglue: XamShowMessageBoxUI converts UTF-16 -> UTF-8 (would say "Invalid UTF-16"); XamShowDeviceSelectorUI converts
nothing; XamEnumerate (xam_enum.cpp 66) writes already-built items; XamContentCreateEx (xam_content.cpp 231) ->
ContentManager::CreateContent/OpenContent -> package path built from XCONTENT_DATA.file_name via rex::to_path
(content_manager.cpp 114, 202, 304, 385) - a UTF-8 -> UTF-16 conversion on Windows. Most likely: the save/profile
container opened right after START has a file name that is not valid UTF-8 (or file_name_raw is read past its end -
content_manager.h 70-73 uses strlen on a 42-byte array that may not be NUL-terminated). Job 018 (--log_verbose)
should show XamContentCreateEx and its arguments just before the FATAL.
Update 09:22: Canary has the SAME code (content_manager.h file_name() with strlen; filesystem_win.cc to_path ->
to_utf16, which throws on invalid UTF-8), and Gears 3 saves fine on Canary. So if job 018 confirms content create,
the file name our build hands over must contain bytes Canary's does not - i.e. a difference on OUR side (recompiled
code, our profile/user name, or a toolkit difference upstream of the name), not just a missing guard. A guard in the
toolkit (strnlen + tolerant conversion) would still be worth adding, but the root cause needs the trace values first.

## FOUND (desk work 2026-10-22 09:40, VERIFIED by reading code; fix not yet tested): the 'Invalid UTF-8' FATAL
Job 018's trace: after START the game calls XamContentCreateInternal (ordinal 0x271, through its wrapper sub_82CE65A0,
which builds an XCONTENT_AGGREGATE_DATA on its stack: device, type, display name, file name, xuid, title id) twice
with the same overlapped 441DE410: first OPEN_EXISTING (completes 3 = PATH_NOT_FOUND, root name never used), then a
create (completion throws).
Cause, in the toolkit: xam_content.cpp line 144 captures `root_name = root_name.value()` in the deferred lambda.
In rexglue `MappedPtr<char>::value()` returns a std::string_view pointing INTO GUEST MEMORY (types.h 300). The
lambda runs later on the dispatch thread; by then the game has reused that buffer, so CreateContent gets garbage as
the root name and the ContentPackage symlink (root_name + ":") conversion throws 'Invalid UTF-8'.
Canary's same line is safe: its lpstring_t is StringPointerParam<char, std::string>, whose value() returns a COPY
(std::string). Only this one deferred lambda in rexglue captures a guest string view (all others checked).
Fix (one line, toolkit): `root_name = std::string(root_name.value())`. Sent to the Gears 2 chat (D18: SDK patch on
their side, we re-copy). Test after: job 016's START press, and job 017 chapter 17 (checkpoint save) - both should
get past the save without the FATAL.

## Job 019 (09:30): title compare - one skipped resolve (cause of the black background: GUESS until job 021)
A (stock + our renderer in shadow, compared every 150 frames): every title-frame resolve matches the stock backend
(most 100% bit-exact; the 7e3 scene copy 98.3-98.5%, diff <= 3) EXCEPT copy 9: a raw k_2_10_10_10 resolve of the
7e3 scene target (base 720, pitch 1280) to 13370000 - the same destination as copy 7 (7e3 with exp_bias -5). No draw
ever uses that k_2_10_10_10 view, so our renderer had no target for it and SKIPPED the resolve ("we never drew into
that colour target"). In takeover the guest then keeps copy 7's 1/32-scaled data instead of the raw bits.
B (ours, takeover): background pure black (0,0,0); logo, PRESS START and copyright drawn. Stock: smoke/skull shown.
Fix in our layer 0 (shadow_renderer.cpp ResolveColour, --gow2_renderer_resolve_view_targets, default on): create
the view's target and let PrepareColourTarget transfer the tiles in from the 7e3 owner (guest-word transfer).
Job 020 (chapter 0 on ours) builds with it; job 021 compares title on/off. If it works: send to Gears 2 (their layer 0).

## Job 020 (09:41): chapter 0 on ours - still black; the colour-grading table is read stale (GUESS: the cause)
Build with the resolve-view fix: it fired (copy 9's k_2_10_10_10 view created) but the 3D scene stayed black (title
card, subtitles, WALK tutorial drawn). The new CPU-path log named the one texture per frame read from stale CPU
bytes: base 14010000, k_8_8_8_8, 16x16x16, stacked 2D (16 layers) fetched as 3D, tiled = UE3's colour-grading table,
rendered by the game each frame. The GPU load path only took single-layer 2D textures, so this one went through the
CPU path, which in takeover reads memory our resolves never wrote (they live in the GPU mirror). Black table -> every
graded 3D pixel black; UI is drawn after grading. Gears 2 has no such texture (its counter is 0).
Fix (layer 0): GPU load for stacked textures fetched as 3D - the untile shader with an RWTexture3D destination, one
dispatch per layer at the CPU path's layer stride; D3D12 backend creates Texture3D UAVs.
--gow2_renderer_gpu_load_stacked_3d (default on). Job 022: chapter 0 with it on vs off.

## Job 021 (09:56): TITLE BACKGROUND FIXED on our renderer - VERIFIED (both fixes needed)
Build with both layer 0 fixes. A (both on): title matches the stock backend by eye (smoke/skull/light shafts);
"CPU-path loads that read our pages" 0 (was 300 per 300 frames); screenshots ~5 MB (were 0.4 MB, black).
B (resolve-view fix OFF, colour-table GPU load on): background present but very dark - the game reads copy 7's
exp_bias -5 (1/32) data instead of copy 9's raw bits. So: the colour-grading table (stacked 3D load) was why it was
black; the skipped resolve-only view was why it would be dim. Next: job 022 (chapter 0 on/off for the table load).

## Job 022 (09:58): CHAPTER 0 3D WORLD DRAWN on our renderer - VERIFIED (colour-table load is the cause)
A (colour-table GPU load on): the Prologue cutscene renders - Marcus, Anya, the ship interior, lighting and
subtitles; screenshots 3.3-4.6 MB; CPU-path loads of our pages 0. B (off): black 3D (shots ~25 KB), CPU-path loads
300 per 300 frames. Compared by eye with job 017's stock run: same scenes and look; the camera shots differ at the
same second because the two runs' timing differs (not a renderer difference). Not yet checked: a pixel comparison
(shadow mode on a gameplay frame), gameplay past the cutscene, other chapters.

## Job 023 (10:01): SDK patch 30 - the 'Invalid UTF-8' FATAL is GONE (VERIFIED on both renderers)
SDK: Gears 2 install copied (1,198 files, 105 MB, matched) -> third_party\rexglue-install-llvm23-rwdi-20261022-1000;
includes Gears 2 patches 25-30 (30 = our XamContentCreate root-name fix, 29 = UTF-8-tolerant conversion). Codegen
rewrote 227 of 459 files (patch 28 changes the generator); exe 86 -> 79 MB. Layer 0 merged with Gears 2 jobs 225/227.
A (stock) and B (ours), START at 50 s then A at 58/66/74 s, 120 s each: 0 FATAL lines, no crash. Stock: main menu ->
Campaign -> Standard ("New" selected). Ours: main menu -> Campaign -> Standard -> SELECT DIFFICULTY (Act I: Troubled
Past) - one screen further because our run reached the menus sooner. Menus, title background drawn correctly on ours.

## Job 024 (10:19): NEW CAMPAIGN from the menus -> Act I gameplay on BOTH renderers (VERIFIED), first real save
START 50 s + A every 8 s (58-106 s), 200 s each, 0 FATAL. Stock: Select Difficulty at 81 s, cutscene, WALK tutorial
in the cell by 151-201 s; wrote GearsCheckpoint0.sav (26,375 bytes, 10:20:27) = the first real campaign save.
Ours: same route ~20 s sooner (loading screen at 81 s), cutscene, the same WALK-tutorial gameplay frame at 201 s -
by eye ours and stock look alike there. Ours wrote no NEW save file (GUESS: with job A's save now present, the
menu offered continue/load instead of a fresh campaign; not a renderer matter - the save path is the toolkit's,
proven by run A). To check later: run with an empty out\userdata, or read the menu shots between 58 and 81 s.

## Job 025 (10:37): pixel check of gameplay (shadow mode, ch0 + ch3) and the first save from scratch on OURS
Save: out\userdata moved aside (kept as out\userdata-before-025); a new campaign on OUR renderer wrote
GearsCheckpoint0.sav (26,375 bytes), PlayerStorage.dat, profile - VERIFIED, 0 FATAL. (Job 024's "no new save" on ours
was the existing save, as guessed.)
Shadow compare (stock draws, ours draws beside it, every resolve compared every 150 frames):
ch0 748 resolves compared, 339 bit-exact, 16 not compared; ch3 473 / 244 / 8. Per frame (ch0 frame 900, 40 copies):
- NOT COMPARED, once per frame: copy 13, a 4x MSAA k_2_10_10_10_FLOAT (7e3) 640x360 at base 720 pitch 640 ->
  13B00000, exp_bias -6, draws 707-763 (56 drawn by us). Our layer 0 returns before resolving ANY MSAA colour target
  ("build B"; Gears 2 notes/44: "MSAA colour resolves: none in Gears 2"). So in takeover this resolve is SKIPPED every
  frame - a real gap (GUESS: half-resolution translucency/particles; the 328x184 chain after it may read stale data).
  The MSAA targets themselves are real multisample textures and are drawn; only the resolve is missing.
- Shadow-map-like depth targets differ: D24S8 base 1440 pitch 1120, 544x544 (copy 1) 85% pixels equal with full-range
  depth differences; 1120x688 / 544x288 copies differ only in STENCIL (depth max diff 0) - stencil probably unused.
- k_8_8_8_8 base 720 1280x720 (copy 3, draws 223-228) 86% equal, max diff 188: channel-masked writes (mask 1, one
  channel) - looks like UE3's light-attenuation / shadow-projection buffer, which reads the shadow depth above.
  GUESS: the shadow depth difference feeds this one.
- The resolve-only k_2_10_10_10 view (our fix) 58-92% equal, max diff up to 738 (raw 7e3 bits): small float differences
  in the 7e3 scene, exposed bit-for-bit by the raw copy. The 7e3 exp_bias -5 copies differ by <= 4.
Most 328x184 / 640x360 post chains: 97.5-100%. By eye (jobs 022/024) the scenes look right; these are the leads for
"looks slightly different" later. Remember: the stock emulator is a reference, not the truth.

## Job 026 (10:50): Gears 2's sleep-instead-of-spin fixes at Gears 3 addresses - VERIFIED, now default on
GPU-wait step sub_82AF98D8 (hook 0x82AF98EC -> 0x82AF991C, --gow3_patch_gpu_wait) and the query wait loop
(0x82961994, --gow3_patch_query_poll_yield 3). Chapter 0 on our renderer, 75 s each, CPU averaged from 30 s on:
both off 1.69 cores busy, 30.0 fps | gpu_wait 0.87 cores, 30.0 fps | gpu_wait + query wait 0.80 cores, 30.0 fps.
0 FATAL, screenshots alike. Both now default on (query wait default 3 from job 027's build).

## Job 027 (10:58): INVALID - built a stale renderer file
The file copy delivered a cached 09:45 shadow_renderer.cpp (281,487 bytes) instead of the MSAA version (288,520), so
027 tested neither the MSAA resolve nor the depth-bias log (both "absent": NOT COMPARED / 0 lines). Its build also
lacked the Gears 2 225/227 merge for its runs only. Fixed 11:05 (fresh staging folder, read back and checked; notes
02/07 were stale too). Job 028 = 027 again. Useful from 027 anyway: the shadow-depth max difference is 16777215 (full
range), not Gears 2's 0x0FFFFF D24FS8 round-trip signature; and the depth copies of base 1440 that differ in depth
come from draws early in the frame (copies 1, 6, 8, 9 in ch0).

## Job 028 (11:21): MSAA colour resolve WORKS (compared every time now); depth bias is NOT the shadow cause
MSAA resolve: the 4x 7e3 640x360 copy (base 720 pitch 640 -> 13B00000) is resolved and compared on every compare
frame (ch0 0 not compared, was 16; ch3 0, was 8). The game asks sample select 6 = average of all 4 (so the host
sample order does not matter here). Match vs stock: 0-79% pixels equal, max diff 61-510 (10-bit units); some frames
0% equal with max diff ~140 everywhere = a whole-frame difference, not edges. GUESS: the 4x target's starting
contents - tiles another target wrote before are not transferred into an MSAA target ("MSAA transfers", open in
Gears 2 notes/44). Visible effect (ours, same frames, on vs off): the dust/haze in the cell doorway is lighter with
the fix ON (5-6% of pixels differ >8/255). Which is right needs a same-frame stock reference.
Depth bias (--gow2_renderer_log_depth_bias_base=1440, 800 draws logged in ch0 + ch3): polygon offset DISABLED and
all four offsets 0 on every draw - so bias is not the shadow-depth cause. The same EDRAM base 1440 is used by
D24S8 pitch 1120 (shadow maps, depth func 3/7 write 1, rect-list clears), D24S8 pitch 560 and D24FS8 pitch 640 (depth
func 6, write 0: reads). Full-range max diffs (16777215, not 0x0FFFFF) + three formats/pitches sharing tiles ->
GUESS: ownership/transfer between those targets (stale tiles), as Gears 2 suggested.

## Jobs 029-030 (11:38-12:00): the haze is a missing 1x -> 4x MSAA transfer (job 031 tests the fix)
029 (same frame numbers, frame shots every 300): stock vs ours MSAA resolve ON vs OFF, ch0 cell doorway, frames
2400/2700: neither matches. ON draws a thick flat fog that hides the back wall (doorway pixels >8 off: 29% / 32%);
OFF shows stale data (26% / 31%); stock: light haze, wall visible. 030 (fast clear also clears a real 4x twin,
Gears 2's diagnosis): the twin clear NEVER fired - the game never fast-clears that target; the pitch-640 4x fast
clears in the trace are the frame's depth clears (half-pitch trick, correct). Depth-bias log with msaa: every
shadow-area target is 1x (pitch 560 D24S8 is a real third 1x target, Gears 2's question d). MSAA copy numbers unchanged.
Cause (VERIFIED by reading the renderer, GUESS until 031): the 4x 640x360 7e3 target at base 720 pitch 640 occupies
exactly the tiles of the 1x 1280x720 7e3 scene target at base 720 pitch 1280. On the console it starts each frame
holding the scene (each 2x2 block of pixels = the 4 samples of one 4x pixel) and its averaging resolve is a cheap
half-size scene copy. Our renderer draws no colour into MSAA targets and never moved 1x data into a 4x target, so the
resolve read stale data, and the haze built on itself frame after frame. Fix: MsaaUpTransfer (per-sample pass).

## Jobs 031-032 (12:10-12:28): MSAA transfer WORKS; the shadow over-claim is a 9-vertex fill
031 (1x -> 4x MSAA transfer, --gow2_renderer_msaa_transfers): the 4x copy now matches stock in 93-99% of pixels in
ch0 (max diff 1-45; was 0-79%, max 510) and 46-97% in ch3 (max <= 49). Same-frame vs stock (job 029's frames): frame
1200 whole-frame pixels >8 off 9.2% -> 1.7%, doorway 17.1% -> 3.7%; the thick flat fog is gone (door031.jpg). Frames
2400/2700 still ~28-32% off in the doorway (lighting/shadows - see below).
032 (fill-why log): every fill draw that rule (b) did not bound (12 in ch0, 3 in ch3) is the fill shader, not indexed,
xy_div_w 1, correct fetch, index_count 9 = THREE rectangles (e.g. 26x266 strips at depth 1). Rule (b) takes only 3 or 6,
so these fell back to the scissor (1120x8192) and claimed the whole EDRAM wrap - stale tiles around the shadow maps.
Fix (12:31, --gow2_renderer_fill_rect_lists, default on): 9..48 vertices bound ownership by their union. In job 033's
build; offered to Gears 2.
