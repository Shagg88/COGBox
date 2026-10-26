# 58. Stage 6 - movies (full-screen video)

The game plays its Bink movies (startup logos, some story scenes) on the CPU in its own recompiled code and draws each
frame with ordinary draws (Y/U/V planes as textures), so on our graphics system they go through our renderer like
everything else. Nothing so far has checked them: every test since job 033 runs with `-nostartupmovies`, and job 200's
title-screen run pressed A at 10 s.

## Job 202 (probe) - pass criteria written first
- Boot with no input, our graphics system and the SDK reference: the startup movies show on both (moving picture, not
  black, not garbage), in the same order and about the same timing; then the title screen.
- Chapter 1 without `-nostartupmovies`: whatever the game shows before play (movie or loading screen) is visible.
- No error lines, no crash. If a movie is black or wrong on ours only, that is the next item.

## Job 202 result [V]
- Startup movies (Epic, Microsoft Game Studios) and the title screen: our graphics system and the SDK reference show
  the same frames at the same frame numbers (e.g. 120, 420, 900 side by side), no error lines, no crash.
- Chapter 1 without `-nostartupmovies`: the Epic logo, then the opening cinematic (SP_Assault_Intro_Cinematic.bik)
  with its subtitles - dark scenes, the marching Gears, the city flyover - all drawn.
- The `NtCreateFile FAILED` lines for `Movies\*.xxx` / `*.txt` are the game probing for files the disc doesn't have
  (same on the SDK); the .bik files are all there.
- Not checked: movie sound (no one listening; the harness has no audio capture).

## Menus (job 203) - pass criteria written first
- A scripted walk from the title screen through the main menu (START, then down / A / B through the items), the same
  script on ours and on the SDK reference: each screen drawn the same (text, highlights, backgrounds), no error
  lines, no crash. Then the same walk at 4K + SMAA + ultrawide: menus drawn in the 1280x550 frame, text readable.

## Job 203 result [V]
- The walk went title -> main menu (START) -> Solo Campaign (A) -> save slots -> launch (the second A), so it covered
  the main menu, the save-slot screen, the loading screen and the campaign's opening cinematic rather than every menu
  item. Ours and the SDK reference show the same screens (text, highlight, backgrounds, subtitles); the SDK reached
  the loading screen a few frames later (frame 1200 black on the SDK, the loading screen on ours) - timing only.
- 4K + SMAA + ultrawide: the menus fill the 1280x550 frame (as Canary's patch lays them out; [G] slightly squashed
  vertically - the patch's UI height), the loading screen and the movie are shown at 16:9 in the middle with bars at
  the sides. No error lines, no crash in any run.

## Job 204 - pass criteria written first
- In-engine cutscenes (no button presses at chapter start) in ch03, ch05 and ch10 on ours and on the SDK reference:
  the same scenes drawn, no error lines, no crash.
- Training Grounds and Horde reached from the main menu (exploratory walks, ours): what screens and maps appear,
  drawn without missing parts; no error lines, no crash.

## Job 204 result [V]
- The cutscene runs had no controller at all (no script pad), so every chapter opened on its pause screen ("controller
  disconnected" pause) - not a cutscene test; job 205 repeats them with an idle pad. Ours and the SDK draw the same
  pause menu; the background behind it differs where the game has not drawn its scene yet (ch03 everywhere, the lower
  half in ch10): black on ours, a purple-blue fill on the SDK [G: memory nothing has written, which each side fills
  differently; ch05, with its scene drawn, matches].
- The menu cursor starts one item lower than my scripts assumed: "two downs" reached the Multiplayer party lobby and
  "four downs" the War Journal. Both drawn cleanly (lobby lists, the "Need More Players" dialog with 0 bots, the
  journal's pages and collectibles). No error lines, no crash.

## Job 205 - pass criteria written first
- Cutscenes with an idle controller (ch03, ch05, ch10, fixed step), ours vs SDK: the same scenes at the same frames.
- Training Grounds (one down), a Multiplayer match with bots (two downs, bots up) and Horde (three downs) reach a map
  and play; drawn without missing parts; no error lines, no crash.

## Job 205 result [V]
- The cutscene runs again showed only the pause screen: every chapter start (launch URL) opens paused, and our sweeps'
  first A at frame 300 has always been "Resume". Job 206 resumes with one A and then leaves the pad alone.
- Training Grounds (one down) loaded a Warzone match on Day One with bots: scoreboard, "Welcome to Warzone" box,
  gameplay with the HUD, a bot running past - all drawn. So multiplayer maps and the MP HUD work on ours.
- The "Multiplayer with bots" walk instead changed the game type to Horde (my down presses landed on GAME TYPE); the
  Horde walk reached the Horde pregame lobby (map selection: Avalanche, "To continue, select a map and press ...")
  and my A presses opened the player card. Lobbies and dialogs drawn cleanly. No error lines, no crash.

## Job 206 result [V]
- Cutscenes (one Resume, then no input): ch03 (the opening speech, "War is all we know", the King Ravens), ch05 (the
  "Tip of the Spear / Roadblocks" title over the scene, the squad talking) and ch10 (the "Denizens" title, the squad
  walking): ours and the SDK show the same shots; pixel differences are animation timing (ch10, where the camera
  holds, differs by under 1 level on average).
- Horde: the lobby -> Avalanche pregame -> "Loading the match" -> Horde on Avalanche drawn (map, HUD, scoreboard), at
  1x and at 4K + ultrawide + SMAA. Then both runs CRASHED about 2 s into the wave countdown (107 s into the run):
  "Unhandled guest access violation: read of guest 0x00000004" on guest thread F8000028 - a game-code read through a
  null pointer, same place both times. [G] Not a drawing problem (it happens in the guest's own code, and the same at
  both settings); job 207 names the function and runs the same walk on the SDK reference.
- [G] Ultrawide's HUD: the scoreboard's right column overlaps the score box - the patch's UI layout.

## Jobs 207-208 result [V]
- The SDK reference crashes the same way at the same moment (guest read of 0x00000004 on thread F8000028), so it is
  not our renderer. ours crashed again identically.
- Stack (llvm-symbolizer, x64): sub_828307A0 <- sub_82826EC8 <- sub_82828F00 <- sub_82828290 <- sub_824D27F8 <-
  sub_824A2EA0 (engine tick) <- ... <- xstart: the game's main thread. sub_828307A0 starts `lwz r11,4(r4)`,
  `lwz r9,4(r11)`: its second argument (sub_82826EC8's field at +1004) holds a null pointer at +4.
- Training Grounds (also a match with bots) ran several minutes without this, so it is Horde-specific.

## Job 209 - pass criteria written first
- A hook at sub_828307A0's first instruction logs the arguments when r4+4 is not a pointer (what object, what's in
  the structure). Expected: one or more log lines just before where the crash was, then the crash as before (switch off).
- `--gow2_patch_horde_guard` (test only): the function returns at once then. Pass for the test: the match carries on
  (wave 1 starts, enemies drawn) or crashes somewhere else - either tells us where to look next.

## Job 209 result [V]
- The hook took (`if (Gow2HordeGuard(ctx.r3, ctx.r4)) { return;`). Guard off: that run's walk ended in the War Journal
  instead of Horde (menu timing), so it says nothing. Guard on: one hit at swap 3069, right where the crash was -
  sub_828307A0(r3 408E9C00 with vtable 82177648, r4 40D60000) and r4's words 00000000 00000000 4213EC40 (36.98f)
  444B2D30 (812.7f) 00000000 8215DE58 00000001 00000004: the +4 pointer is null. Then the match went on: the
  "WAVE 1" HUD came up, the (idle) player died and the game-over menu ("Restart from current wave ...") showed, no
  crash in 240 s. 0.1 s after the hit a new 800-wide depth target was made [G: the function sets up something drawn
  from then on].
- Not known yet: what that structure is and why its pointer is null here (the SDK reference does the same, so the
  cause is below the renderer: game data, the runtime, or the recompiled code). The only runtime warnings around the
  lobby are four `NetDll_getsockopt STUB` calls [G: a lead - Horde runs as a networked match].
- The guard stays a test switch (off by default): skipping a function hides the cause.

## Co-op (job 210) - pass criteria written first
- `--gow2_input_script2`: a second scripted controller. With it, Co-op Campaign is reached from the main menu and a
  two-player split screen is drawn (both halves, both HUDs), no error lines, no crash.

## Job 210 result [V]
- With the second controller connected, Co-op Campaign becomes selectable (one down) and opens the Campaign Menu
  (Host Co-op / Join Co-op / Split-Screen Campaign); player 2 shows as "Not Signed In". My A presses picked Host
  Co-op, which says "No Network Connection" (expected offline) - on ours and on the SDK the same.

## Job 211 result [V]
- Time-based walks drift: both runs on ours landed in Training Grounds matches (Annex and Submission on Day One,
  with bots - scoreboard, objective ring, "Drained Ring" / "Reviving" tip boxes, bot kill feed all drawn), the SDK run
  in Host Co-op again. No error lines, no crash. Job 212 times the presses in frames (--gow2_input_frames).

## Job 212 result [V]
- Frame-timed presses make ours and the SDK take the same path: both went to Training Grounds (Lesson 4: Objective,
  the Annex match on Day One with bots) - same menus, same scoreboard, same kind of gameplay frames (timing apart).
  Co-op was not selectable yet at frame 950 (the cursor skipped it), so job 213 waits longer before moving down.

## Job 213 result [V]
- ours reached the Campaign Menu but my two down presses came during its fade-in, so A picked Host Co-op again
  ("No Network Connection"); the SDK run saw Co-op still greyed out and went into Training Grounds (Annex). Whether
  Co-op is selectable at a given frame depends on when the second pad is noticed - menu walks are a poor route to
  split screen. Job 214 uses drop-in co-op instead: player 2 presses START during a chapter.

## Job 214 result [V]; SDK patch 25 (job 215)
- Drop-in (player 2 presses START in a chapter): nothing happened - no split screen, ours and SDK alike; the frames
  match each other (ch02 hospital, ch10 Reaver) apart from timing. Cause found in the SDK's xam_user.cpp: only user 0
  is ever signed in (every XamUser* call returns "no such user" for users 1-3), and the main menu shows player 2 as
  "Not Signed In". Gears 2 needs a signed-in profile for player 2.
- SDK patch 25 (`patches/sdk-patch25-local-players.diff`, our code): `--local_players=N` (1-4, default 1) signs in
  users 1..N-1 as offline profiles "Player 2".."Player 4" (XUID = user 0's + index); the sign-in notification
  carries the mask of all of them. Their profile settings read as the defaults; their writes are accepted, not kept.

## Job 215 - pass criteria written first
- SDK builds with patch 25; our build, 82 test cases.
- Default (`--local_players=1`): behaviour unchanged (ch10 capped single-player shots like before).
- `--local_players=2` + the second script pad pressing START in ch02 / ch10: split screen (two views, two HUDs) on ours
  and on the SDK reference, no error lines, no crash.

## Job 215 result [V]
- SDK patch 25 builds (Release 58 s, RelWithDebInfo 45 s); our build and the 82 test cases pass; ch10 single-player
  runs as before.
- Drop-in with `--local_players=2` (player 2 presses START in ch02 / ch10): still no split screen, on ours and on the
  SDK alike - the frames are byte-for-byte the size of job 214's. [G] Gears 2 may not offer split-screen drop-in in
  a running chapter at all (it is chosen from Co-op Campaign -> Split-Screen Campaign). Next (job 216): the menu
  route with player 2 signed in - the main menu should now name "Player 2" instead of "Not Signed In".

## The Horde crash, found (desk work while the PC was busy)
- sub_82830A00 builds the object sub_82826EC8 uses (constructor sub_828244F0 zeroes its fields 984-1004), then calls
  sub_82AABFC0 - the game's own XUserReadStats (XDK library): it sizes the results itself (8 + views x (16 + xuids x
  52) + column space x 28; 136-byte stats specs) and returns 122 when the buffer is too small, else sends XAM message
  0x000B0021 with the buffer. The game allocates the size, stores it at +1000 and the buffer at +1004, and calls again.
- The SDK's message handler (xgi_app.cpp, case 0x000B0021) only logs and returns success, leaving the buffer as
  allocated: XUSER_STATS_READ_RESULTS.dwNumViews = 0 and pViews = null - exactly job 209's guard log (words 00000000
  00000000 ...). At the first wave sub_828307A0 reads pViews[0] -> null + 4. It reads the Horde high scores.
- SDK patch 27 (ours): the handler fills one empty view per requested view (the spec's view id, 0 rows). Job 216 runs
  Horde with the guard off: pass = no crash, wave 1 plays.

## Job 216 result [V]
- SDK patches 26 + 27 build; 82 test cases; committed. First job under the shared PC lock (06:49-07:04).
- Horde with the guard OFF: "HORDE - BEGIN WAVE 1", Locust attack the (idle) player, "WAVE 1 FAILED" score screen,
  game-over menu; normal exit, 0 error lines, no crash. Patch 27 fixed the Horde wave-1 crash.
- `--local_players=2`: the main menu now shows "User" and "Player 2" signed in (patch 25 works). My walk's one down
  press landed on Training Grounds ("Too Many Profiles Signed In - only one signed-in profile can play training
  grounds"), so split screen was not reached yet: with player 2 signed in, the cursor seems to start one item lower.
  Next walk: A on the first highlighted item. ours and the SDK reference showed the same screens.

## Job 224 result - split-screen, the menu route [V from the frames]
- `--local_players=2`, walk a (A on the first highlighted item, then down, down, A; player 2 A / START / A / A): the
  game asks "Two controllers are signed into gamer profiles. To play split-screen, start from the campaign lobby.
  Continue to the lobby? (A) Play Split-Screen", opens the Campaign Lobby with User and Player 2 (difficulty
  Hardcore), asks "Checkpoint Overwrite", loads and plays the chapter's opening cutscene ("Gathering Storm:
  Displacement", the old man / "Name's Maria" scene, full screen as cutscenes are). No error lines, no crash.
- The 230 s run ended inside the cutscene, so the split view itself is not seen yet. Next walk (bundled into a later
  job): ~350 s, player 1 presses START during the cutscene to skip it, shots every 90 frames.

## Job 226 / 228 results - split-screen reached [V from the frames]
- Job 226 (the same walk + START presses): this time the Campaign Lobby launched Act 1 "Tip of the Spear" (User +
  Player 2, Hardcore); the START presses paused the intro movie; it stopped at the first "Choose Path" (Skip Training /
  Train). Menu walks drift with the save state - the lobby offers the saved campaign or a new one.
- Job 228 (walk 3, 420 s, SDK patch 30 build): the lobby continued the saved campaign ("Gathering Storm:
  Displacement"); after the cutscene, from frame ~9500 to the end (frame ~12600): a two-player split screen -
  player 1 (Marcus) in the top half, player 2 (Dom) in the bottom half, each its own camera, both drawn correctly by
  our renderer. 0 error lines, no crash. Split-screen co-op works (SDK patch 25 + the menu route).
- "Choose Path" (user, 10:04): left trigger = the left path, right trigger = the right path (Act 1 start: LT = Skip
  Training, RT = Train). Our script pad has LT / RT.

## Jobs 229 / 230 - Gears 3's two layer 0 fixes in Gears 2 [V]
- Ported from Gears3-Native's layer 0 (texture_cache.cpp/.h, shadow_renderer.cpp, d3d12_backend.cpp), both default on.
- (1) GPU load for stacked textures fetched as 3D: never fires in Gears 2 ("CPU-path loads that read our pages" 0).
- (2) Resolve-only format views get a target (tiles transferred in) instead of the resolve being skipped: fires in
  every run at frame 1 (k_8_8_8_8 base 720, 1280x720 -> 1F5F8000, raw) and, as in Gears 3, for k_2_10_10_10 views of
  base 720, raw: ch10 frame 317 (640x128 at 152,376 -> 10354000), ch18 frame 8 (544x600 at 184,120 -> 115E5000) -
  resolves we used to drop.
- Job 229's shot comparisons were void (my script glued the input script onto the launch argument - the chapters
  stayed paused). Job 230 (fixed): ch02 0/7 and ch10 1/7 identical to the old variants; the differences are animation
  poses and one sign texture sharper sooner (texture streaming), scenery and title text the same - a startup-timing
  shift, no wrong output [V by eye]. The 230 shots are new known variants.
