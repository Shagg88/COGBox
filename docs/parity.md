# Four-game parity

Shagg, 23 Nov 2026: "if one game has it, all of them should have it if it applies". Every per-game option (layer 3
cvar) and per-game hook (manifest `midasm_hook`) that is not in all four games is listed here, either as **only where it
applies** (with the reason) or as a **gap to close**. `tools/parity_check.py` compares this file with the code and
fails on anything new that is in neither list - so a feature added to one game is reviewed for the other three.

Names: options without their `gow1_` / `gow2_` / `gow2_patch_` / `gow3_` / `gowj_` prefix, hooks without `Gow1` /
`Gow2` / `Gow3` / `Gowj` (and `Patch`). A feature can exist in a game under another mechanism (an ini edit, another
hook): then it is "only where it applies" with that noted - e.g. 16x anisotropy and the texture pool size are a hook in
Gears 2 / 3 and the game's own settings in Gears 1 / Judgment (`gow1_aniso16`, `gowj_aniso16`, `*_texture_pool_mb`).

## Gaps to close

Player-facing or smoothness features one or more games lack. Each needs: does it apply there (the problem or the effect
exists in that game)? If yes, port it (hook found and checked against that game's generated code, as patch 0066 on
23 Nov); if no, move it to the list below with the reason.

| Name | Has it | Missing | Note |
|---|---|---|---|
| `option shadow_cap` | G2 G3 J | G1 | Smart Shadow Cuts (J since 23 Nov: G3's gather function, layer 2 `ue3/shadow_cap.h`). G1 has projected shadow maps too (its Engine script: LightComponent.ShadowProjectionTechnique / ModShadowColor, Engine.Min/MaxShadowResolution) but no shadow counter of the G2 or G3/J shape - its gather function needs its own trace. Side find: the launcher says G1 has no shadow-size setting, but Engine.MinShadowResolution exists in its script (an ini edit may give it Shadow detail) |
| `hook ShadowCap` | G2 G3 J | G1 | |
| `hook WatchXGOffsetBaseTextureAddress` | G2 | G1 G3 J | Since 23 Nov it also feeds the texture pre-load (layer 0 `XenosGuestTextureAddressed`, `--gow2_renderer_texture_preload`, off by default): UE3 gives each texture its memory through the XDK's XGOffsetBaseTextureAddress(header r3, base r4, mip r5). Applies to all four (same XDK call in every UE3 360 game). To port: find the function in each game's image the way patch 0066 did (match G2's TU6 829BAD88 / base 829A0DF0 bytes; first instruction `mflr r12`), add a midasm hook at its first instruction with r3 r4 r5 that calls `XenosGuestTextureAddressed`, and check its generated code on the PC. The renderer checks the header layout itself at run time (first 64 headers, logged). Until then G1 / G3 / J keep the texture-load budget only |

## On hold (Shagg)

Reviewed and deliberately not done for now (Shagg's call); they count as reviewed, not as open gaps.

| Name | Has it | Missing | Note |
|---|---|---|---|
| `option first_person` | G2 G3 J | G1 | first-person mode and its 8 sub-options. On hold (Shagg 23 Nov). Groundwork found: G1's WarfareGame script has the same members the other ports drive (WarCameraMode.ViewOffset / ViewOffset_ViewportAdjustments / FOVAngle / strafe and run adjustments / bDoPredictiveAvoidance; WarPlayerCamera.CurrentCamMode; Camera.HiddenActors; PrimitiveComponent.bIgnoreHiddenActorsMembership); modes WarCameraMode_Default, WarCam_Crouch / _Targeting / _TargetingGrenade plus split-screen *SSHoriz / *SSVert classes; no HeadScaler / bIsHiddenByCamera / MyWarPawn (head hiding needs another way); G1's ue3::obj has no range lookups |
| `option first_person_body` | G2 G3 J | G1 | |
| `option first_person_diag` | G2 G3 J | G1 | |
| `option first_person_fov` | G2 G3 J | G1 | |
| `option first_person_head` | G2 G3 J | G1 | |
| `option first_person_offset` | G2 G3 J | G1 | |
| `option first_person_third_person_moves` | G2 G3 J | G1 | |
| `option first_person_weapon` | G2 G3 J | G1 | |
| `option first_person_head_control` | G2 | G1 G3 J | |

## Only where it applies

| Name | Game(s) | Why only there |
|---|---|---|
| `option fast_mip_fade` | G3 J | G2, G1 | G2: tested (job 2378g, 23 Nov) - an extreme MipFadeIn/OutSpeed 0/1 = 0.0005 ini edit left Gears 2's level start pixel-identical to the normal run, so its engine does not read the key [V]. G1: its Coalesced.ini has no mip-fade or MipFilter keys [V] |
| `hook Aniso16` | G2 G3 | G1 / J do it with the game's own setting (`gow1_aniso16`, `gowj_aniso16`) |
| `hook TexturePoolSize` | G2 G3 | G1 / J use the game's own [TextureStreaming] PoolSize (`*_texture_pool_mb`) |
| `hook BootLockWait` | G1 | G1 start-up lock bug |
| `option boot_lock_wait` | G1 | (with the hook) |
| `hook CheckpointFileName` | G1 | G1 checkpoint naming |
| `option keep_checkpoint_name` | G1 | (with the hook) |
| `hook FilmsWidescreen` | G1 | G1 pre-rendered films |
| `option films_widescreen` | G1 | (with the hook) |
| `hook LaunchChapterFlag` | G1 | G1 chapter launch (others take a URL) |
| `hook LaunchChapterIndexHook` | G1 | |
| `hook LaunchChapterTick` | G1 | |
| `hook NativeDeviceHeight` | G1 | each game's native-lines sites differ |
| `hook NativeSceneHeight` | G1 | |
| `hook NativeCallHeight` | J | |
| `hook NativeWidthCheck` | J | |
| `hook NoAspectLock` | G1 | G1's own widescreen |
| `option no_aspect_lock` | G1 | |
| `hook ViewFov` | G1 | |
| `hook ViewFovB` | G1 | |
| `hook PresentInterval` | G1 | G1's frame limit |
| `hook NoGaussianBlurA` | G1 | Canary TU5 G1 patch |
| `hook NoGaussianBlurB` | G1 | |
| `option no_gaussian_blur` | G1 | |
| `option no_motion_blur` | G1 | Canary TU5 G1 patch |
| `option tiling_off` | G1 | G1 renderer diagnostic |
| `option ini_edits` | G1 | G1's ini edits (others: layer 2 ue3_ini_edits) |
| `hook SkipLogoStep` | G1 | G1's logo skip (J has an option, G2 / G3 an ini edit) |
| `hook SkipLogoStep2` | G1 | |
| `hook SkipLogoTick` | G1 | |
| `option unlock_insane_mask` | G1 | G1 content |
| `option unlock_insane_setting` | G1 | |
| `hook HordeGuard` | G2 | G2 Horde crash guard |
| `option horde_guard` | G2 | |
| `hook NoteTextureDefrag` | G2 | diagnostics |
| `option log_texture_defrag` | G2 | |
| `option loc_log` | G2 | GitHub #90 diagnostics |
| `hook QueryKeepLate` | G2 | G2 query bug |
| `option query_buffered` | G2 | |
| `hook RtPoolBase` | G2 | G2 render-target pool |
| `hook RtPoolCheck` | G2 | |
| `option rt_pool_grow` | G2 | |
| `hook UntiledHook` | G2 | G2 renderer |
| `option untiled` | G2 | |
| `hook UltrawideUiHeight` | G2 | each game's ultrawide sites differ |
| `hook UltrawideNoVignette` | J | |
| `hook UltrawideTargetHeight` | J | |
| `hook UltrawideUiScale` | J | |
| `hook UltrawideSizeCheck` | G3 | |
| `hook UltrawideVideo` | G3 | |
| `hook WatchDeferredFree` | G2 | memory diagnostics |
| `hook WatchInlineUnlockA` | G2 | |
| `hook WatchInlineUnlockB` | G2 | |
| `hook WatchLockCore` | G2 | |
| `hook WatchMipRealloc` | G2 | |
| `hook WatchPoolDefrag` | G2 | |
| `hook WatchUnlock` | G2 | |
| `hook WatchXGOffsetResourceAddress` | G2 | |
| `hook WatchXGSetCubeTextureHeaderEx` | G2 | |
| `hook WatchXGSetIndexBufferHeader` | G2 | |
| `hook WatchXGSetTextureHeaderEx` | G2 | |
| `hook WatchXGSetVertexBufferHeader` | G2 | |
| `hook ModFName` | G3 | G3 modding |
| `option mod_chapters` | G3 | |
| `option mod_content_packages` | G3 | |
| `option mod_file_log` | G3 | |
| `option mod_kismet_trace` | G3 | |
| `option mod_log_props` | G3 | |
| `option mod_squad_log` | G3 | |
| `hook NoDynamicShadows` | G3 | G3 option |
| `option no_dynamic_shadows` | G3 | |
| `hook NoLightEnvShadows` | G3 | |
| `option no_light_env_shadows` | G3 | |
| `hook Tu6PackageHashOk` | G3 | G3 TU6 cheats |
| `hook VoiceCallbackGone` | G3 | G3 crash |
| `option file_handle_map_lock` | G3 | G3 crash |
| `option horde_bot_characters` | G3 | G3 Horde |
| `option log_bot_classes` | G3 | |
| `option log_file_opens` | G3 | |
| `option path_choice_mouse` | G3 | G3 path choice |
| `option split_vertical` | G3 | G3 split screen |
| `option squad_slot_log` | G3 | |
| `option ue3_dump_settings` | G3 | |
| `option ue3_exec_each_level` | G3 | |
| `option unlock_all_chapters` | G3 | G3 content |
| `option unlock_chapter_settings` | G3 | |
| `option unlock_mutators` | G3 | |
| `option level_select` | J | J content |
| `option unlock_aftermath` | J | |
| `option unlock_aftermath_mask` | J | |
| `option unlock_aftermath_setting` | J | |
| `hook NoFilmGrain` | J | J's film grain |
| `option no_film_grain` | J | |
| `option log_weapon_state` | - | (all four since 23 Nov: one layer 2 copy, `ue3/weapon_state_log.h`; on by default in G1, off in G2 / G3 / J until a run shows no hitch - Shagg to flip) |
| `option show_fps` | - | (all four since 23 Nov: G2 / G3 a data patch; J / G1 the engine's own `stat fps` / `stat unit` through the console, logged when the engine runs them) |
| `option profile_100` | - | (all four since 23 Nov, Shagg 10:42: profile debugging tools; one layer 2 copy, `ue3/profile_tools.h`, off by default) |
| `option profile_100_file` | - | (with profile_100) |
| `option profile_log` | - | (all four since 23 Nov) |
| `option audio_music` | - | (all four since 23 Nov: the game's own audio menu from the launcher; one layer 2 copy, `ue3/audio_menu.h`, cvars made by its `UE3_AUDIO_MENU_CVARS`; ids per game in each hook file, all still -1 = not known) |
| `option audio_effects` | - | (with audio_music) |
| `option audio_dialogue` | - | (with audio_music) |
| `option audio_menu_ids` | - | (with audio_music) |
| `option ultrawide` | - | (all four since 23 Nov; G1's is experimental and greyed in the launcher until a run - see the hooks below) |
| `option aspect` | - | (with ultrawide) |
| `option menu_fps` | - | (all four since 23 Nov: G1 / G2 / J share layer 2 `ue3/menu_fps.h`; G3 keeps its own listener) |
| `hook MaxTickRate` | G2 G3 J | G1's 2006 engine has no GetMaxTickRate (no frame smoothing; no function of that shape in its code [V], J's matches at 0x8255E200); G1 limits in `PresentInterval` |
| `option tick_floor_fps` | G2 G3 J | G1: no engine tick-rate read to floor (no GetMaxTickRate, see above); its limiter is the render thread's present interval |
| `option trilinear` | G1 G3 J | G2 does it as a launcher ini edit (`~=MipFilter=point>MipFilter=linear`, gears_settings.cpp) |
| `option skip_logos` | G1 J | G2 / G3 skip the logo films through the launcher's ini edit (`-StartupMovies=EpicLogo` / `MGSLogo`, `Open_Logos`; gears_settings.cpp) [V] |
| `option loading_fps` | J | Shagg 23 Nov: only Judgment has the endless-load reports (#38/#318/#405) |
| `option loading_max_s` | J | (with loading_fps) |
| `option loading_stream_s` | J | (with loading_fps) |
| `option loading_tail_s` | J | (with loading_fps) |
| `option checkpoint_stream_wait` | G3 | Shagg 23 Nov: one G3 DLC scene (RAAM's Shadow ch.4) |
| `option checkpoint_stream_wait_ms` | G3 | (with checkpoint_stream_wait) |
| `option shadow_value_cap` | G2 | Canary's G2 sites (a per-light default read after a `lwz 288 / cmpwi / bgt` override test); that code is in no other game (searched G1 / G3 / J, the same search finds exactly G2's two) [V]; G3 / J use the per-object `shadow_cap` |
| `hook ShadowValueCapA` | G2 | (with shadow_value_cap) |
| `hook ShadowValueCapB` | G2 | (with shadow_value_cap) |
| `option query_poll_yield` | G2 G3 J | One wait, two names: UE3's wait for one query's answer (GetData(q, out, 4, FLUSH) in a loop) is hooked at its loop head in all four games - G1 0x825BA71C and G2 0x82817124 as `query_wait`, G3 0x8297297C and J 0x829933B4 as `query_poll_yield` (mode 3 = G2's sleep-until-GPU-write); checked in the PC's generated code 23 Nov. G2's own `query_poll_yield` is a second, opt-in (default 0) pause / yield inside D3D's GetData itself (job 178) - not needed for G1, which has the loop hook |
| `hook QueryPollYield` | G2 G3 J | (with query_poll_yield) |
| `option query_wait` | G1 G2 | (see query_poll_yield: G3 / J have the same loop hook under that name) |
| `hook QueryWaitStep` | G1 G2 | (with query_wait) |
| `hook CoalescedResult` | G1 G2 | G1 / G2 skip the game's start-up hash check of an edited Coalesced; G3 / J don't need to: their layer 2 config_install.cpp gives each edited bundle its new SHA-1 in the image's start-up hash table (and the TOC's size / MD5), so the game's own check passes [V code] |
| `hook CoalescedSkip` | G1 G2 | (with CoalescedResult) |
| `option skip_coalesced_check` | G2 | (with CoalescedResult; G1 turns its hooks on whenever config edits are applied, gow1_app.h, so it has no option) |
| `option tick_log` | G2 G3 J | the engine tick rhythm, timed at the engine loop's GetMaxTickRate call (one layer 2 copy, `ue3/tick_log.h`, since 23 Nov; G2 added). G1: no engine-tick hook (no GetMaxTickRate, see MaxTickRate); its frames are timed by the renderer's frame-time line |
| `hook UltrawideFov` | G2 G3 J | G1 does it in its own FOV hooks `ViewFov` / `ViewFovB` (the pi/360 load before the half-angle multiply; f0 on one path, f13 on the other - PC's generated code), exact Hor+ (`ue3/aspect.h`) |
| `hook UltrawideNoConstrain` | G2 G3 J | G1: its `NoAspectLock` hook (the camera's constrain flag reads 0) |
| `hook UltrawideSceneHeight` | G2 G3 J | G1: its `NativeSceneHeight` / `NativeDeviceHeight` hooks give the 1280 x H height |
| `option ultrawide_fov_exact` | G2 G3 J | the exact Hor+ field of view against a fixed Canary factor (J since 23 Nov: its FOV load is followed by `fmuls f2,f31,f0`, so f31 is the FOV it applies to - PC's generated code). G1: its ultrawide is exact only - there is no fixed factor (no Canary G1 21:9 patch) to switch back to |
| `option aspect_targets_720` | G3 J | a switch back to Canary's 1280x720 render targets after G3 #330 / J #396 sized them for the wider picture; G1's (new) and G2's ultrawide size the scene from the screen height already, with no such earlier behaviour to return to |
| `hook UltrawideSharedMemory` | G3 J | (part of the same G3 / J target sizing: their scene shared-memory block 2 kept at its 16:9 size) |
| `option native_ui_layout720` | G1 G2 | One job, two UI systems: with true 1080p (`native_lines`) the menus lay out in 1280x720 and are scaled up - G1 / G2's UE3 UI scenes through this option, G3 / J's Scaleform menus through `native_ui_show_all` [V code] |
| `option native_ui_show_all` | G3 J | (see native_ui_layout720) |
| `hook NativeSceneWidthMax` | G2 G3 | true 1080p's width clamp (`li r11,1280`); G1 does it in its `NativeSceneWidth` hook (the clamped width in r31), J in `NativeWidthCheck` (the stored width before its 1280 compare) |
| `option ultrawide_min_rows` | G2 | G2's render-target pool trap below ~536 rows (GitHub #94 / #137); the other games have no such trap (their floor is a fixed 240) |
| `option ultrawide_ui_centre` | G2 | GitHub #278 test switch for G2's UE3 UI scenes; G3 / J centre their Scaleform menus by default (G3's `ultrawide_ui_full_width` switches that back). G1 (same UI system as G2): to look at with its experimental ultrawide's first run |
| `option ultrawide_ui_full_width` | G3 | the switch back to G3's pre-0.6.6 full-width Scaleform menus (GitHub #396); J's menus have their own `UltrawideUiScale` hook |
| `option texture_stats_off` | G2 | Canary's G2-only Texture Streaming Fix (RHIGetTextureMemoryStats -> FALSE, "FudgeFactor stays 1.0"). G3 / J's streamers make no such call: searched their code (PC's G3 TU6, J) for G2's caller set-up (two -1 stack slots passed as r3 / r4) and for a three-output form - none (the same search finds exactly G2's three calls). G1's Canary patch is the pool size only. Reopen if G3 / J show over-budget blur |
| `hook TextureStatsOff` | G2 | (with texture_stats_off) |
| `hook ShaderCreatedVertex` | - | (all four since 23 Nov: patch 0066) |

## Test checklist

Options added or changed by the 23 Nov parity pass. One line each: game, what to pass, and the log line that proves it ran
(from the code; `{}` = a value). A job passes the switch, plays the stated part, and checks the log.

| Game | Pass | Do | Log line that proves it |
|---|---|---|---|
| G1 | `--gow1_menu_fps=60 --gow1_max_fps=0 --gow2_gpu_vsync=false` | title screen, then a level | `gow1_menu_fps: front end (...WarStart.xxx) - limit 60 fps`, then `gow1_menu_fps: level ... - the player's limit again` |
| G2 | `--gow2_menu_fps=60 --gow2_max_fps=-1` | title screen, then a level | `gow2_menu_fps: front end (...GearStart.xxx) - limit 60 fps`, then `... - the player's limit again` |
| J | `--gowj_menu_fps=60 --gowj_max_fps=-1` | title screen, then a level | `gowj_menu_fps: front end (...GearStart...) - limit 60 fps`, then `... - the player's limit again` |
| G2 | `--gow2_max_fps=-1 --gow2_tick_floor_fps=240` | a checkpoint save | `gow2_max_fps: engine frame-rate limit {} -> none (tick floor 240/s)` |
| J | `--gowj_max_fps=-1 --gowj_tick_floor_fps=240` | gameplay | `gowj_max_fps: no limit - tick floor 240/s (--gowj_tick_floor_fps)` |
| G1 | `--gow1_show_fps=true` | a level | `gow1_show_fps: the engine ran 'stat fps'` (missing = G1's engine does not know the name) and the counter on screen |
| J | `--gowj_show_fps=true` | a level | `gowj_show_fps: the engine ran 'stat fps'` and the counter on screen |
| G3 | `--gow3_shadow_cap=20` | a shadow-heavy scene | `gow3_shadow_cap: more than 20 shadow depth passes in a frame - further per-object shadows skipped`, and no `... - left alone` |
| J | `--gowj_shadow_cap=20` | a shadow-heavy scene | `gowj_shadow_cap: more than 20 shadow depth passes in a frame - ...`, and no `... - left alone` |
| G2 | (default) `--gow2_tick_log=600` | 20 s of play | `gow2_tick_log: ticks {}-{}: median {} ms, p95 ...` every 600 ticks |
| G2 / G3 / J | `--gow2_log_weapon_state=true` (`gow3_` / `gowj_`) | co-op, switch weapons | `<game> weapon log: InvManager +0x{}...` once, then `<game> weapon state: player 1 | pawn ... | weapon ...` per change |
| G1 / G2 / G3 / J | `--gow2_audio_menu_ids=<m>,<e>,<d> --gow2_audio_music=3` (`gow1_` / `gow3_` / `gowj_`); runtime `--audio_channels=1` | title screen, Options > Audio | `<game>_audio_menu: ids music=...`, `<game>_audio_menu: settings array at object+0x...`, `<game>_audio_menu: id {} 0x... -> 0x00000003`, `<game>_audio_menu: now music=3 (id {})`; `audio_channels: 1 -> mono (...)` |
| G2 | `--gow2_profile_log=true` | campaign menu | `gow2_profile_log: get int id={} value={} ...` |
| G1 / G3 | `--gow1_profile_100=true --gow1_profile_100_file=<file>` (`gow3_`) | menus, then a profile save | `<option>: {} value(s) from '<file>'`, `<option>: settings array at object+0x...`, `<option>: {} written, {} missing ...` |
| G1 | `--gow1_ultrawide=true --gow1_aspect=21:9` | a level, menus, HUD, a film | `gow1_ultrawide: on - 1280x552, aspect lock off, ...`, `gow1_ultrawide: screen height 720 -> 552`, `gow1_ultrawide: field of view {} deg: factor ...`; look: wider view, not stretched; note how menus / HUD / films look |
| J | `--gowj_ultrawide=true` (`--gowj_ultrawide_fov_exact` default true) | a level, aim | `gowj_ultrawide: field-of-view factor {} -> {}` with a value that differs from 0.010975 x scale; aiming keeps its vertical framing |
| G2 / J | `--gow2_aspect=auto` / `--gowj_aspect=auto` with ultrawide | start-up | `<tag>: monitor is {}x{}` (the shared ue3/aspect.h now measures the monitor) |

