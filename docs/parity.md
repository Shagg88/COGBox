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
| `option first_person` | G2 G3 J | G1 | first-person mode and its 8 sub-options |
| `option first_person_body` | G2 G3 J | G1 | |
| `option first_person_diag` | G2 G3 J | G1 | |
| `option first_person_fov` | G2 G3 J | G1 | |
| `option first_person_head` | G2 G3 J | G1 | |
| `option first_person_offset` | G2 G3 J | G1 | |
| `option first_person_third_person_moves` | G2 G3 J | G1 | |
| `option first_person_weapon` | G2 G3 J | G1 | |
| `option first_person_head_control` | G2 | G1 G3 J | |
| `option trilinear` | G1 G3 J | G2 | smooth blending between texture detail levels |
| `option fast_mip_fade` | G3 J | G1 G2 | sharper textures at once instead of fading in (pop-in) |
| `option loading_fps` | J | G1 G2 G3 | frame-rate cap while loading (GitHub #38: endless loads at high fps) - frame-rate pattern |
| `option loading_max_s` | J | G1 G2 G3 | (with loading_fps) |
| `option loading_stream_s` | J | G1 G2 G3 | (with loading_fps) |
| `option loading_tail_s` | J | G1 G2 G3 | (with loading_fps) |
| `option menu_fps` | G3 | G1 G2 J | menu frame-rate cap (GitHub #101) - frame-rate pattern |
| `option tick_floor_fps` | G3 | G1 G2 J | engine tick floor with no fps limit - frame-rate pattern |
| `hook MaxTickRate` | G2 G3 J | G1 | the engine's frame-rate limit read; G1 limits through PresentInterval |
| `option checkpoint_stream_wait` | G3 | G1 G2 J | GitHub #226: streamed sections fully loaded after a checkpoint load |
| `option checkpoint_stream_wait_ms` | G3 | G1 G2 J | (with checkpoint_stream_wait) |
| `option shadow_cap` | G2 G3 | G1 J | Smart Shadow Cuts (parked for G1 / J on 23 Nov - reopen) |
| `hook ShadowCap` | G2 G3 | G1 J | |
| `option shadow_value_cap` | G2 | G1 G3 J | |
| `hook ShadowValueCapA` | G2 | G1 G3 J | |
| `hook ShadowValueCapB` | G2 | G1 G3 J | |
| `option query_poll_yield` | G2 G3 J | G1 | render thread's GPU query wait without spinning |
| `hook QueryPollYield` | G2 G3 J | G1 | |
| `option query_wait` | G1 G2 | G3 J | |
| `hook QueryWaitStep` | G1 G2 | G3 J | |
| `option skip_coalesced_check` | G2 | G1 G3 J | faster start-up config load; G1 has the hooks without the option |
| `hook CoalescedResult` | G1 G2 | G3 J | |
| `hook CoalescedSkip` | G1 G2 | G3 J | |
| `option texture_stats_off` | G2 | G1 G3 J | half of Canary's texture streaming fix |
| `hook TextureStatsOff` | G2 | G1 G3 J | |
| `option show_fps` | G2 G3 | G1 J | the engine's own FPS display |
| `option profile_100` | G2 J | G1 G3 | |
| `option profile_100_file` | G2 J | G1 G3 | |
| `option profile_log` | G1 G3 J | G2 | |
| `option tick_log` | G3 J | G1 G2 | timing log |
| `option skip_logos` | G1 J | G2 G3 | G2 / G3 skip them through an ini edit (check) |
| `option ultrawide` | G2 G3 J | G1 | G1 has its own: no_aspect_lock, ViewFov (check what is missing) |
| `option aspect` | G2 G3 J | G1 | |
| `option ultrawide_fov_exact` | G2 G3 | G1 J | |
| `option aspect_targets_720` | G3 J | G1 G2 | |
| `option native_ui_layout720` | G1 G2 | G3 J | |
| `option native_ui_show_all` | G3 J | G1 G2 | |
| `option ultrawide_min_rows` | G2 | G1 G3 J | |
| `option ultrawide_ui_centre` | G2 | G1 G3 J | |
| `option ultrawide_ui_full_width` | G3 | G1 G2 J | |
| `hook UltrawideFov` | G2 G3 J | G1 | |
| `hook UltrawideNoConstrain` | G2 G3 J | G1 | |
| `hook UltrawideSceneHeight` | G2 G3 J | G1 | |
| `hook UltrawideSharedMemory` | G3 J | G1 G2 | |
| `hook NativeSceneWidthMax` | G2 G3 | G1 J | |
| `option log_weapon_state` | G1 | G2 G3 J | co-op weapon desync diagnostics (notes: G2 / G3 / J need it) |

## Only where it applies

| Name | Game(s) | Why only there |
|---|---|---|
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
| `hook WatchXGOffsetBaseTextureAddress` | G2 | |
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
| `hook ShaderCreatedVertex` | - | (all four since 23 Nov: patch 0066) |
