// Layer 0 (game-free): the renderer cvars, moved unchanged (same names, defaults, help text and order) from
// shadow_renderer.cpp - reorganisation step, no behaviour change. Declarations: shadow_renderer_cvars.h. Our code (MIT).
#include <cstdint>
#include <string>

#include <rex/cvar.h>

#include "shadow_renderer_cvars.h"

REXCVAR_DEFINE_STRING(gow2_renderer_api, "d3d12", "Gears2",
                      "Notes/68: the graphics API our renderer uses - d3d12 (default) | vulkan (stage V2: draws, but "
                      "no resolves or present yet, so no picture - a test mode)");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_direct_spirv, true, "Gears2",
                    "Notes/68 V5a: with --gow2_renderer_api=vulkan and --gow2_renderer_shader_backend=dxbc, our DXBC is "
                    "converted to SPIR-V directly (well under a millisecond) instead of HLSL through DXC (100-300 ms); "
                    "false = the HLSL path. D3D12 is not affected");
REXCVAR_DEFINE_STRING(gow2_renderer_vulkan_compressible_targets, "always_mutable", "Gears2",
                      "Patch 0087 (Vulkan only; job 388/390b: colour targets take ~2x D3D12's GPU time, the depth-only "
                      "target - one format, never mutable - the same): always_mutable (default, as before) | lazy - "
                      "colour render targets start as one-format images the driver can compress, and one is remade "
                      "mutable (contents copied, logged) the first time it needs a signed or bits view | never - "
                      "diagnostic: such views read the image's own format (wrong values; measurements only)");
REXCVAR_DEFINE_STRING(gow2_renderer, "off", "Gears2",
                      "Our own renderer: off (default) | shadow (draws next to the emulator, compares, logs) | "
                      "takeover (our renderer draws, resolves, answers queries and supplies the image; the emulator "
                      "draws nothing - needs SDK patch 19)");
REXCVAR_DEFINE_BOOL(gow2_renderer_debug, false, "Gears2",
                    "Forward D3D12 debug-layer messages of our device to the log (needs the SDK's --d3d12_debug)");
REXCVAR_DEFINE_BOOL(gow2_renderer_pipeline_library, true, "Gears2",
                    "notes/64: saved pipelines - a D3D12 pipeline library (<shader cache dir>\\pipelines-d3d12.bin); "
                    "pipelines seen in an earlier run load in ~1 ms instead of compiling, so their draws are not "
                    "skipped as pending. Default on since job 255 (job 253: ch10 capped run 2 skipped 18 pending draws, "
                    "its shots identical to run 1 and within 2/255 of the run without it)");
REXCVAR_DEFINE_BOOL(gow2_renderer_pipeline_save_background, true, "Gears2",
                    "Patch 0010 (stutter): save the pipeline library (tens of MB) on a background thread instead of "
                    "inside a frame - Shagg's 01:31 session froze 413 ms and 116 ms at its two saves; job 292: a 66 ms frame at a "
                    "save. Default on since job 294");
REXCVAR_DEFINE_BOOL(gow2_renderer_pipeline_load_async, true, "Gears2",
                    "Patch 0010 (stutter): pipelines already in the library load on the pipeline workers (their draws "
                    "wait as pending, see --gow2_renderer_pipeline_wait_ms) instead of ~1 ms each on the frame - a new "
                    "area loaded 271 at once (247 ms frame); job 292 ch10 start: 386 -> 260 ms (recording 152 -> 18 ms), worst "
                    "later frame 47 -> 30 ms. Default on since job 294");
REXCVAR_DEFINE_INT32(gow2_renderer_pipeline_threads, -1, "Gears2",
                     "Patch 0056 (Canary d3d12_pipeline_creation_threads): pipeline worker threads; -1 = 75% of the "
                     "logical processors (Canary's default), 0 = the old count (a quarter, at most 4), N = N");
REXCVAR_DEFINE_INT32(gow2_renderer_pipeline_thread_priority, 0, "Gears2",  // job 518: normal again, as Canary / Edge (below normal: textures took too long)
                     "Patch 0162: Windows priority of the pipeline worker threads: -1 = below normal (default; the game, "
                     "its renderer and the audio keep their CPU time during a burst of first-visit builds - Shagg's "
                     "Gears 2 audio crackled while 12 normal-priority workers filled 16 logical processors), 0 = normal, "
                     "-2 = lowest");
REXCVAR_DEFINE_INT32(gow2_renderer_backlog_priority, -3, "Gears2",
                     "0.6.3 (4-core laptop stuttering while the start-up backlog built): Windows priority of a pipeline "
                     "worker while it builds a start-up backlog pipeline (recorded pipelines, the recipe pack; a pipeline "
                     "a draw asks for is moved to the live queue first and keeps the workers' own priority): -3 = auto "
                     "(below normal; 0.6.5 - was only on 8 logical processors or fewer), -2 = lowest, -1 = below "
                     "normal, 0 = the workers' own (gow2_renderer_pipeline_thread_priority). D3D12");
REXCVAR_DEFINE_INT32(gow2_renderer_backlog_play_workers, -1, "Gears2",
                     "0.6.3: the most pipeline workers on start-up backlog pipelines at once while the game is being "
                     "played (frames of 64+ draws); loading screens, menus and videos keep every worker, and pipelines a "
                     "draw asks for are never limited. -1 = auto (half the workers; 0.6.5 - "
                     "was only on 8 logical processors or fewer), 0 = no limit, N = N. D3D12");
REXCVAR_DEFINE_INT32(gow2_renderer_pipeline_save_seconds, 15, "Gears2",
                     "Patch 0056: seconds between saves of the pipeline library while it grows (was 60; a quit or a "
                     "hard exit lost everything since the last save). Minimum 5");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_slim, true, "Gears2",
                    "Patch 0060 (DXBC back end): smaller DXBC with the same values - ALU operands used in place, no "
                    "one-pass loop where nothing breaks out of it, branch-free pixel prologue. The driver's pipeline "
                    "build time grows with the DXBC size (job 355: 2.4 ms per KB on an RTX 3080 Ti)");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_canary_maths, true, "Gears2",  // job 634: on (was off)
                    "Patch 0060 (DXBC back end): Canary's max / min (SM3 a >= b ? a : b) and result clamps (_sat) "
                    "instead of the exact FTZ / total-order / NaN emulation - ~17% smaller DXBC. Job 634 ran both forms "
                    "of all 3,423 shaders of a Judgment run on the same inputs: the same values everywhere but a "
                    "denormal passed through instead of flushed (0x00000001 for 0) in 7 vertex shaders");
// 0.6.6 (Shagg 20 Nov: "Why would we turn speed up switches off? Ever?"): the speed study's switches are ON by default -
// tex_post_skip, ps_no_alpha_variant, tex_frac16 = false, ps_r21 = false here; front_copy_lean, present_gamma_to_smaa
// and vertex_watch_min_bytes = 256 further down. NOT gow2_renderer_fused_scaled_resolve: Shagg's decision of 14 Nov
// ("Do not change gow2_renderer_fused_scaled_resolve's default") stands - it measured no real gain (5.84 -> 5.79 ms)
// and its depth copies differed slightly at 4K (job 238); Gears 1 keeps its own setting (gow1_app.h). Together: Gears 2 chapter 9 at 1440p about
// 8 ms -> 5 ms a frame (jobs 2146 / 2161). Each keeps its switch only as a way back if one breaks a picture in one
// game. Pictures checked in Gears 2; Gears 1, Gears 3 and Judgment still to be checked in a gameplay run.
REXCVAR_DEFINE_BOOL(gow2_renderer_tex_post_skip, true, "Gears2",
                    "Speed study (with --gow2_renderer_tex_post_affine): a texture fetch whose slot needs no "
                    "post-processing (no sign / gamma / integer step, exponent adjust 1) skips it in one uniform branch. "
                    "Same values. Changes the pixel shaders (pipelines rebuild once)");
REXCVAR_DEFINE_INT32(gow2_renderer_ps_no_alpha_from_swap, 0, "Gears2",
                     "Speed study (picture check): with --gow2_renderer_ps_no_alpha_variant, the variant is used only from "
                     "this swap on - a still scene's screenshots before and after it compare the two paths in one run");
REXCVAR_DEFINE_INT32(gow2_renderer_query_chunk, 0, "Gears2",
                     "Speed study (opt-in, strict queries): submit the closed occlusion queries to the GPU every N "
                     "queries (between two queries) instead of once after the whole query section, so the game's first "
                     "answers land sooner (job 2132: the game waited from before our single submit). 0 = off");
REXCVAR_DEFINE_BOOL(gow2_renderer_ps_no_colour_variant, false, "Gears2",
                    "Speed study (opt-in, job 2127: 43 such draws a frame cost 0.38 ms of GPU at 2x): a draw that needs its "
                    "pixel shader only for the alpha test, kills or depth (no colour target written) uses a variant "
                    "without colour outputs, so the GPU runs only what feeds those. D3D12 / DXBC shaders; the variants "
                    "are compiled at first use (not kept across sessions yet)");
REXCVAR_DEFINE_BOOL(gow2_renderer_ps_no_alpha_variant, true, "Gears2",
                    "Speed study: a draw whose alpha test is off (ALWAYS) uses a variant of its pixel shader without the "
                    "alpha test - no discard instruction, so the GPU keeps early depth rejection (every pixel shader "
                    "that writes colour 0 otherwise carries the discard, whether the test is on or not). Same pictures; "
                    "a second variant of such shaders is translated on first use");
REXCVAR_DEFINE_BOOL(gow2_renderer_ps_r21, false, "Gears2",
                    "Speed study (with --gow2_renderer_canary_r21): false = the 21-bit rounding of exp / log / rcp / rsq "
                    "/ sqrt only in vertex shaders, as Xenia Edge does (b13a0478d: in pixel shaders 'the extra "
                    "instructions cost too much per pixel'). Changes the pixel shaders");
REXCVAR_DEFINE_BOOL(gow2_renderer_tex_frac16, false, "Gears2",
                    "Speed study: false = a fixed-point texture's filtered value is not rounded to 16 fractional bits "
                    "(Canary's emulation of the hardware's filter precision). Differences are below 1/65536 - with "
                    "--gow2_renderer_tex_post_skip most fetches then skip the post-processing");
REXCVAR_DEFINE_BOOL(gow2_renderer_tex_post_affine, true, "Gears2",
                    "Job 634 (DXBC back end, with --gow2_renderer_tex_post_constants): the texture-fetch "
                    "post-processing as multiply-adds and selects on per-slot constants the renderer works out from the "
                    "fetch constant - ~45 instructions a fetch instead of ~100 (the driver's pipeline build time grows "
                    "with the DXBC size). The same values bit for bit (job 634: 367M CPU cases, and all 3,423 shaders "
                    "of a Judgment run interpreted on the same inputs). false = as before");
// Job 636: the second round of smaller DXBC (each checked on all 3,423 shaders of a Judgment run: the same outputs on
// 10 random input sets; the short forms also on every possible input).
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_structured_jumps, true, "Gears2",
                    "Job 636 (DXBC back end): a program whose only flow control is forward jumps as nested if / else "
                    "instead of the pc loop (1,209 of 1,212 looping shaders of a Judgment run). false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_mul_movc, true, "Gears2",
                    "Job 636 (DXBC back end): the legacy multiply in three instructions (movc on min(|x|, |y|)); "
                    "differs from the four-instruction form only for a denormal times Inf / NaN. false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_short_r21, true, "Gears2",
                    "Job 636 (DXBC back end): the 21-bit result rounding in nine instructions instead of eleven - the "
                    "same bits for all 2^32 inputs. false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_short_swap, true, "Gears2",
                    "Job 636 (DXBC back end): the vertex word byte swap in 12 instructions instead of 17 - the same bits "
                    "for every value and mode. false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_short_degamma, true, "Gears2",
                    "Job 636 (DXBC back end): the texture gamma decode as a sum of clamped segments, 12 instructions "
                    "instead of 16 - the same bits for every float in [0, 1]. false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_coalesce_moves, true, "Gears2",
                    "Job 636 (DXBC back end): an instruction into a scratch temp followed by its move to the destination "
                    "writes the destination itself when the temp is not read again (a pass on the finished code). "
                    "false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_dead_stores, true, "Gears2",
                    "Job 637 (DXBC back end, with --gow2_renderer_dxbc_coalesce_moves): instructions whose temp result is "
                    "never read (initial zeros overwritten before use, unused components) are dropped. Same outputs on "
                    "all 3,423 shaders of a Judgment run (10 random input sets each). false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_vertex_words, true, "Gears2",
                    "Job 638 (DXBC back end): a vertex fetch's words addressed, range-tested and byte-swapped together "
                    "(one ring / memory choice per fetch). Same outputs (job 638). false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_fold_modifiers, true, "Gears2",
                    "Job 638 (DXBC back end): an operand's abs / negate applied as it is read (no mov first). The same "
                    "bits. false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_merge_movs, true, "Gears2",
                    "Job 638 (DXBC back end, with --gow2_renderer_dxbc_coalesce_moves): consecutive one-component movs "
                    "between two registers merged (then folded), a temp's mov onto itself dropped, and a copy of a copy "
                    "read from the original. Same outputs (job 638). false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_mul_min_abs, true, "Gears2",
                    "Job 634 (DXBC back end): the legacy multiply's zero rule (0 * anything = 0) in four instructions "
                    "(min(|x|, |y|) != 0) instead of five - the same bits (9M operand pairs, and all 3,423 shaders of "
                    "a Judgment run interpreted). false = as before");
REXCVAR_DEFINE_INT32(gow2_renderer_dxbc_salt, 0, "Gears2",
                    "Patch 0063, TEST ONLY (first-visit measurements): non-zero = every DXBC shader carries one dead "
                    "instruction with this value, so its bytes - and the graphics driver's own cache key - are new and "
                    "the driver compiles every pipeline from scratch, as on a first visit. Same picture. Use only with "
                    "an empty --gow2_shader_cache_dir. 0 (default) = shaders unchanged");
REXCVAR_DEFINE_BOOL(gow2_renderer_pipeline_record, true, "Gears2",
                    "Patch 0058 (Canary pipeline storage): record every game-draw pipeline the first time it is made - "
                    "its shader hashes and fixed-function state, no game data - in <shader cache dir>\\pipelines.list");
REXCVAR_DEFINE_BOOL(gow2_renderer_pipeline_precreate, true, "Gears2",
                    "Patch 0058 (Canary: stored pipelines are created at title start): at start-up every pipeline in "
                    "pipelines.list whose shaders the warm-up has ready is queued to the pipeline workers, behind any "
                    "pipeline a draw is waiting for - built (or loaded from the saved library) during the start-up and "
                    "loading screens instead of when first drawn");
REXCVAR_DEFINE_BOOL(gow2_renderer_prepare_all, false, "Gears2",
                    "Job 641 (the launcher's \"Prepare all shaders\"): with --gow2_renderer_recipe_pack and "
                    "--ue3_shader_prebuild_at_start, every chapter list of the pack starts at once and is built at full "
                    "speed while the game shows its intro; once the package reading is done and nothing more can be "
                    "built, the pipeline library is saved and the game exits. Progress in <shader cache dir>\\"
                    "prepare-progress.txt (\"<done> <total> <state> <seconds>\")");
REXCVAR_DEFINE_BOOL(gow2_renderer_prepare_only, false, "Gears2",
                    "Patch 0061 (\"Preparing shaders\"; a test switch since 0062 removed the launcher button): at "
                    "start-up build every recorded pipeline (pipelines.list) on all pipeline workers, save the pipeline "
                    "library, write progress to <shader cache dir>\\prepare-progress.txt and exit - the game never "
                    "starts");
REXCVAR_DEFINE_BOOL(gow2_renderer_load_hold, false, "Gears2",
                    "Patch 0132 (Shagg 21:46: holds are a hindrance, brief pop-in instead): off by default. Patch 0063 (first visit of an area): within 20 s after a load (a swap more than 1 s after the one "
                    "before), a frame with draws waiting for their pipeline (0064: 1+, see load_hold_draws; mid-play "
                    "play_hold_draws) is not shown and the swap waits until "
                    "those pipelines are built - the loading picture stays a little longer instead of a half-drawn "
                    "world. Also for a frame where most draws wait (200+, a new scene). Other mid-play frames are not "
                    "held (a few pending draws stay pop-in, as before). Our own presenter only (--gow2_present=ours)");
REXCVAR_DEFINE_INT32(gow2_renderer_load_hold_max_ms, 20000, "Gears2",
                    "Patch 0063: the longest the frames of one load may be held in all (ms)");
REXCVAR_DEFINE_INT32(gow2_renderer_load_hold_draws, 1, "Gears2",
                    "Patch 0064: within 20 s after a load, a frame is held when at least this many draws wait for "
                    "their pipeline (0063 used 16; Shagg: a longer hold over stuttering frames)");
REXCVAR_DEFINE_BOOL(gow2_renderer_d3d12_sm65, false, "Gears2",
                    "0.6.1 (test): D3D12 runs with Shader Model 6.5 shaders even on a device with 6.6 - the path a card that "
                    "only has 6.5 takes");
REXCVAR_DEFINE_BOOL(gow2_renderer_chapter_hold, false, "Gears2",
                    "0.6.1 (test, off by default). The recipe pack's pipelines whose pixel shader is in a package the "
                    "GAME has opened are 'wanted': they are translated and built before every other list's, also "
                    "during clean play. And where a loading screen ends (30+ frames in a row with fewer than 64 draws, "
                    "or no frame for over a second, then a frame with 64+; a level package opened since the last "
                    "time), the first frames of play are not shown until every wanted pipeline is built - the loading "
                    "picture stays, with the Loading shaders indicator, and the game waits behind it. Nothing is held "
                    "when they are built already (a second visit, a checkpoint), nor mid-play. Needs the recipe pack, "
                    "--ue3_shader_prebuild and our own presenter");
REXCVAR_DEFINE_INT32(gow2_renderer_chapter_hold_max_ms, 120000, "Gears2",
                    "0.6.1: the longest the frames after one loading screen may be held for the wanted pipelines (ms)");
REXCVAR_DEFINE_INT32(gow2_renderer_play_hold_draws, 0, "Gears2",
                    "Patch 0132: 0 by default (no mid-play holds). Patch 0064: outside the load window (mid-play), a frame is held when at least this many draws "
                    "wait for their pipeline; 0 = never hold mid-play (0063's rule)");
REXCVAR_DEFINE_BOOL(gow2_renderer_shader_precreate, true, "Gears2",
                    "Patch 0066 (Shagg: build behind the loading screen): shaders the game creates while loading (a "
                    "hook in each game's own shader-creation code) are translated at once and built by the driver in "
                    "a guessed pipeline in the background, before their first draw. Job 362c [V]: the driver keeps "
                    "each shader stage, so the real pipelines later cost ~0.1 ms instead of 15-35 ms. DXBC path only");
REXCVAR_DEFINE_STRING(gow2_renderer_shader_recipes, "", "Gears2",
                      "Chapter recipes (notes/78): a text file of pixel-shader fingerprints (16 hex digits a line, the "
                      "draws' own XXH3 key - no game data) recorded from chapter openings. Set = patch 0068's package "
                      "reader runs (with --ue3_shader_prebuild) and builds ONLY the listed shaders, from the player's own "
                      "game files, while a chapter loads. Empty (default) = off");
REXCVAR_DEFINE_STRING(gow2_renderer_recipe_pack, "", "Gears2",
                      "Chapter recipe pack (notes/78, C3): a .rcp file (work\\tools\\make_recipe_packs.ps1) of recorded "
                      "chapter-opening pipelines plus the layout dwords of their vertex shaders. Set = patch 0068's "
                      "package reader (with --ue3_shader_prebuild) hands over the shaders of every package the game "
                      "opens; the listed ones come from the player's own files (vertex shaders rebuilt and checked "
                      "against their fingerprint), are translated, and each recorded pipeline is built as soon as both "
                      "its shaders are ready, as the start-up backlog is (patch 0058). Job 612c: a chapter's list "
                      "starts when half of its marker pixel shaders have been found. Empty (default) = off");
REXCVAR_DEFINE_INT32(gow2_renderer_recipe_pack_jobs, 6, "Gears2",
                     "Job 612c: the recipe pack queues more pipelines only while fewer than this many pipeline jobs "
                     "are in flight (612 queued thousands at once and starved the game's own threads). 1-64");
REXCVAR_DEFINE_INT32(gow2_renderer_recipe_pack_jobs_load, 64, "Gears2",
                     "Job 613 (Shagg 11:01: let the loading screen do the work): the recipe pack's limit instead while "
                     "the frame is a loading screen or menu (under 64 draws) or already missing draws (32+ skipped) - "
                     "Gears 2 ch5 had ~200 of ~800 pipelines ready after 10 s at 6. 1-64");
REXCVAR_DEFINE_INT32(gow2_renderer_recipe_pack_jobs_missing, 4, "Gears2",
                     "Job 730 (Shagg 17:40, Gears 3 at 18 fps for minutes after a renderer update): the recipe pack's limit "
                     "during play while frames are missing draws (32+ skipped). Was the loading-screen limit (64), which "
                     "filled every logical processor and left the game and its renderer no CPU. Loading screens, menus "
                     "and Prepare all shaders keep the full limit. 1-64");
REXCVAR_DEFINE_BOOL(gow2_renderer_shader_prebuild, false, "Gears2",
                    "Patch 0132: off by default (its warm-only builds took pipeline workers from the draws' own pipelines). Patch 0068 (Shagg: shaders built behind the loading screen, both games): when the game opens a "
                    "package, its pixel shaders (found by layer 2 in the file itself) are translated like a draw's and "
                    "put through the driver in warm-only pipelines (built and released), so their first draws cost "
                    "~0.1 ms instead of 15-35 ms (job 362c). DXBC path only. The load hold stays as the safety net");
REXCVAR_DEFINE_INT32(gow2_renderer_shader_prebuild_queue, 64, "Gears2",
                    "Patch 0068: the most warm-only pipelines queued for the pipeline workers at once (the rest waits)");
REXCVAR_DEFINE_INT32(gow2_renderer_shader_prebuild_workers, 3, "Gears2",
                    "Patch 0069: the most pipeline workers building warm-only pipelines at the same time (job 368b: "
                    "all 12 at once doubled the frame time of a cold ch10 and slowed the draws' own pipelines)");
REXCVAR_DEFINE_INT32(gow2_renderer_shader_prebuild_priority, -2, "Gears2",
                     "PR #43 (legoliamneeson): Windows priority of a pipeline worker while it builds a warm-only "
                     "pipeline (speculative; nothing waits on it): -2 = lowest (default), -1 = below normal, 0 = the "
                     "workers' own (gow2_renderer_pipeline_thread_priority). Draws' own pipelines keep that priority");
REXCVAR_DEFINE_INT32(gow2_renderer_shader_precreate_ms, 6, "Gears2",
                    "Patch 0066: the most time per frame spent translating shaders the game created (ms); the rest "
                    "waits for the next frame");
REXCVAR_DEFINE_INT32(gow2_renderer_play_hold_max_ms, 1000, "Gears2",
                    "Patch 0064: the longest one mid-play hold may wait (ms); then the frame is shown as it is");
REXCVAR_DEFINE_BOOL(gow2_renderer_texture_batch, false, "Gears2",
                    "Off until tested (job 620, PR #43 a8b8abd - not run by its author yet). Batched texture loads (D3D12; 2026-10-29, the Gears 2 level-start freeze: ~4,900 BC mip loads in one "
                    "frame, each a compute pass, a copy and a GPU wait on one scratch buffer): a draw's texture uploads "
                    "go first, then their untile passes, and the BC copies are recorded together - the GPU waits once "
                    "per draw instead of twice per mip level. false = the old one-at-a-time loads");
REXCVAR_DEFINE_BOOL(gow2_renderer_placed_textures, true, "Gears2",
                    "Patch 0017 (stutter): guest textures (sampled only, up to 16 MB) are placed in shared 64 MB heaps "
                    "instead of one committed allocation each - job 299: creating 376 textures took 157 of the 208 ms "
                    "of the ch10 level-start texture loads");
REXCVAR_DEFINE_DOUBLE(gow2_hitch_report_ms, 0.0, "Gears2",
                    "Patch 0010 diagnostic: log every frame whose renderer time is over this many ms, with where the time "
                    "went (CPU parts) and what happened in it (pipelines loaded/stored/saved, shader compiles, resolves, "
                    "uploads). 0 = off; the first 200 such frames are logged");
REXCVAR_DEFINE_STRING(gow2_shader_cache_dir, "", "Gears2",
                      "Folder for our translated/compiled shaders (default: <current dir>\\out\\shader-cache)");
REXCVAR_DEFINE_STRING(gow2_dxc_dir, "", "Gears2", "Folder holding dxcompiler.dll + dxil.dll (default: the exe folder)");
// Shader pop-in (Gears 3, 2026-10-23; research: UnleashedRecomp precompiles every shader and never skips a draw, Xenia
// stores its shader / pipeline list per title and loads it at start-up, Canary's async mode skips draws as we do).
REXCVAR_DEFINE_BOOL(gow2_renderer_shader_record, true, "Gears2",
                    "Save every guest shader the game uses to <shader cache dir>\\ucode (a few MB; game-derived, stays "
                    "on this PC) so --gow2_renderer_shader_warmup can prepare them at the next start");
REXCVAR_DEFINE_BOOL(gow2_renderer_shader_warmup, true, "Gears2",
                    "At start-up, prepare every shader recorded in earlier sessions (translate, load or queue its "
                    "compile) so areas already seen draw without shader pop-in. Default on since patch 0014 (job 292: "
                    "~820 shaders in 0.5-0.8 s at start-up; without it the first use of each reads its file mid-game)");
REXCVAR_DEFINE_BOOL(gow2_renderer_shader_pack, false, "Gears2",
                    "Keep compiled shaders in one file, <shader cache dir>\\shaders.pack, opened once (patch 0015: each "
                    "loose .dxil file cost ~8 ms to open mid-game, 920 ms in one ch05 frame). Loose files are copied in "
                    "as they are used; they stay on disk");
REXCVAR_DEFINE_DOUBLE(gow2_renderer_shader_wait_ms, 0.0, "Gears2",
                      "Patch 0132: 0 by default (never wait - the draw pops in a frame later). Per-frame time a draw may wait for its still-compiling shader instead of being skipped. Patch 0047 "
                      "(Shagg 13:21: textures must show even with no shader cache, a stutter is fine): -1 = no cap, the "
                      "default - a draw is never skipped for a compiling shader. 0 = never wait (skip, as before 0047); "
                      "e.g. 8 = up to 8 ms a frame");
REXCVAR_DEFINE_INT32(gow2_renderer_log_every, 300, "Gears2", "Our renderer's summary line every N frames");
REXCVAR_DEFINE_BOOL(gow2_renderer_d24s8_exact, false, "Gears2",
                    "Store D24S8 targets as D32_FLOAT and round at resolves (exact maths) instead of D24_UNORM like the "
                    "reference");
REXCVAR_DEFINE_BOOL(gow2_renderer_msaa_transfers, true, "Gears2",
                    "Gears 3: a 4x MSAA colour target whose EDRAM tiles a 1x target of the same format wrote last takes "
                    "that data (each 2x2 block of 1x pixels -> the 4 samples of one 4x pixel, as EDRAM stores them)");
REXCVAR_DEFINE_BOOL(gow2_renderer_fill_rect_lists, true, "Gears2",
                    "Gears 3 (job 032): rule (b) (fill/clear ownership) also takes fill draws of several rectangles "
                    "(9, 12, ... up to 48 vertices) - ownership bounded by their union instead of the scissor");
REXCVAR_DEFINE_STRING(gow2_dxil_vs_twin, "auto", "Gears2",
                      "Notes/80 s.16 (PR #248 follow-up): a draw pairing a DXBC vertex shader with an HLSL-path (DXIL) pixel shader "
                      "- PCSS, the HLSL override folder, a DXBC fallback - uses the vertex shader's DXIL twin. auto = D3D12 only "
                      "(it cannot build a pipeline mixing DXBC and DXIL; Vulkan has SPIR-V for both), on = every back end, "
                      "off = never (0.4.8: such pairs go to the pipeline as they are, and the warm-up builds them)");
REXCVAR_DEFINE_BOOL(gow2_renderer_clear_msaa_twins, true, "Gears2",
                    "Gears 3: a fast clear (redirected from 4x half pitch to the 1x target) also clears a REAL 4x MSAA colour "
                    "target with the same key, which otherwise keeps last frame's contents");
REXCVAR_DEFINE_BOOL(gow2_renderer_msaa_colour_resolves, true, "Gears2",
                    "Resolve MSAA colour targets (Gears 3: one 4x MSAA 7e3 half-resolution resolve per frame): read the "
                    "selected host samples, average after decoding; false = skip them as before (\"build B\")");
REXCVAR_DEFINE_BOOL(gow2_renderer_learn_clear_pair, false, "Gears2",
                    "Developer: when the fast-clear hint pair never matches, learn the pair from 3-vertex 4x MSAA "
                    "rectangle lists (first pair with 30 draws, from frame 90) and treat it as the fast clear (redirected to pitch x2 1x)");
REXCVAR_DEFINE_BOOL(gow2_renderer_msaa2x_colour_draws, true, "Gears2",
                    "Draw into 2x MSAA colour targets (a 7e3 scene at 2x); false = skip them as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_msaa_depth_resolves, true, "Gears2",
                    "Resolve MSAA depth targets (sample 0 / the selected sample), as the reference does; false = "
                    "skip them as before (\"MSAA depth resolves are not in M1\")");
REXCVAR_DEFINE_STRING(gow2_renderer_resolve_rules, "canary", "Gears2",
                      "Patch 0023 (D36): colour resolve conversion rules - canary (default since 0039, map group 2) or sdk "
                      "(the SDK's old-Xenia rules, kept until the switch to Canary is play-tested) - Canary's incl. its hardware-tested Oct 2026 "
                      "fixes: 8_8_8_8_GAMMA sources degammaed and MSAA-averaged as 10-bit codes then PWL'd again "
                      "(534aeb6), k_16_16 / 16_16_16_16 targets with 10 fractional bits, decode i/1024 and host output "
                      "scale 1024/32767 (42c2b88), Canary's fixed-point packing and raw-copy rules)");
REXCVAR_DEFINE_BOOL(gow2_renderer_vertex_index_clamp, true, "Gears2",
                    "Patch 0028 (Canary RemapAndConvertVertexIndices; on by default since 0037, again since 0043): vertex shaders take the offset vertex index's low 24 "
                    "bits, clamped to VGT_MIN/MAX_VTX_INDX, so a bad index reads a clamped vertex instead of another one "
                    "(NPC vertex explosion candidate). Changes the generated vertex shaders (new shader-cache entries)");
REXCVAR_DEFINE_BOOL(gow2_renderer_alpha_test_rt0, true, "Gears2",
                    "Patch 0027 (Canary; on by default since 0037, again since 0043): the alpha test applies only when the pixel shader writes colour 0 (oC0); a "
                    "pixel shader that writes no oC0 never discards by alpha test (black ground / missing depth "
                    "candidate). false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_depth_alias_drop, true, "Gears2",
                    "Patch 0025 (Canary 9da693480; on by default since 0039): a depth target sharing its EDRAM base with a colour target the draw "
                    "writes is not bound (the colour target owns the tiles) unless the depth access is read-only - "
                    "Gears 3 ch2 tone-map candidate. false = depth always bound, as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_tex_sign_swizzle, true, "Gears2",
                    "Patch 0024 (Canary, Gears 3 review 02-a; on by default since 0037, again since 0043): a fetched component's sign / bias / gamma mode is the one "
                    "of the source component the fetch swizzle picked (constant 0/1 components get none), not the one "
                    "of the output position. Changes the generated shaders (new shader-cache entries)");
REXCVAR_DEFINE_BOOL(gow2_renderer_canary_r21, true, "Gears2",
                    "Patch 0037/0041 (Canary 3a44f20): exp / log / rcp / rsq / sqrt rounded to 21 mantissa bits. On by "
                    "default again since 0043 (the Gears 3 black eyes were the emptied shader cache, not 0037). Changes the generated shaders");
REXCVAR_DEFINE_BOOL(gow2_renderer_mulsc_round_toward_zero, false, "Gears2",
                    "Patch 0087 (Canary 3390fc2, its mulsc_round_toward_zero - off by default there too; unconfirmed on "
                    "hardware): mulsc products rounded toward zero instead of to nearest (DXBC back end). Changes the "
                    "shaders that use mulsc");
REXCVAR_DEFINE_STRING(gow2_renderer_msaa_sample_positions, "standard", "Gears2",
                      "Patch 0089 (Canary cc4981a; Gears 3 / Gears 1 - Gears 2 draws no MSAA targets): standard (default, "
                      "as before: the API's sample positions, the viewport keeps the Xenos sample offset at resolution "
                      "scale > 1) | xenos - MSAA draws rasterize at the Xenos positions (D3D12 programmable sample "
                      "positions; MSAA depth cleared under them and decompressed before reads), resolves read host sample "
                      "= Xenos sample | canary_off - the API's positions and no viewport sample correction (Canary's own "
                      "default). Vulkan: xenos acts as standard (not built yet)");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_dispatch_runs, false, "Gears2",
                    "vkpass049 (Vulkan): back-to-back compute dispatches (resolve passes, GPU texture loads) share their "
                    "barriers - no full barrier after each one; a dispatch that reads or writes what an earlier one of the "
                    "run wrote (or writes what it read) gets a compute-only barrier, and the first other GPU work (a pass, "
                    "copy, upload, clear, readback, submit) gets the one full barrier. The stats line counts both");
REXCVAR_DEFINE_STRING(gow2_renderer_vulkan_pass_barriers, "when_needed", "Gears2",
                      "Patch 0090 (Vulkan, stage V5b; the default since 0091 - jobs 394-396: ch15 skips ~74 of 110 "
                      "pass-start barriers a frame; the shadow target 0.30 -> 0.19-0.23 ms in every run; the scene "
                      "target's change is within run-to-run noise (2.95-3.29 ms); picture the same): "
                      "when_needed = a render pass starts with a full GPU barrier only when something since the last "
                      "barrier could conflict with it - work outside a pass (copies, uploads, dispatches, clears, "
                      "readbacks, a submit), a memory-export draw, or one of its attachments sampled or written since | "
                      "full = a full barrier at the start of every render pass (as before 0090) | narrow (patch 0115; "
                      "jobs 423-424: 110 pass breaks between draws take 0.35 ms on Vulkan, 0.05 on D3D12) = as "
                      "when_needed, but when the only conflict is that the pass's attachments were drawn to since, a "
                      "barrier between attachment writes and attachment reads / writes only, not a full one");
REXCVAR_DEFINE_BOOL(gow2_renderer_const_dirty_span, true, "Gears2",
                    "Upload049 (D3D12 and Vulkan): a changed constant block of the same size updates only the span "
                    "of its CPU shadow between the first and last changed 16-byte rows (was the whole block). The "
                    "shadow ends byte-identical, so uploads are unchanged; false = the old whole-block copy.");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_clear_in_pass, true, "Gears2",
                    "Patch 0092 (Vulkan, stage V5b; the default since 0093 - job 396: ch10 passes 90 -> 76 and barrier "
                    "calls 186 -> 151 a frame, ch15 110 -> 101 and 201 -> 179, scenery pixel-identical): a clear of a "
                    "currently bound target runs inside the render pass (vkCmdClearAttachments) instead of ending it, "
                    "changing layouts twice and starting a new pass. false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_suballoc_textures, true, "Gears2",
                    "0.5.1 (Vulkan): sampled textures share 64 MB memory blocks instead of one allocation each (fewer, cheaper vkAllocateMemory calls while textures stream in). false = one allocation per texture, as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_dedicated_images, false, "Gears2",
                    "Patch 0097 (Vulkan): images the driver prefers in an allocation of their own (NVIDIA: render "
                    "targets) get one, as D3D12's committed resources are. A measurement switch first (job 403): "
                    "Vulkan colour targets draw in ~2x D3D12's GPU time even with DXC-compiled shaders (job 402)");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_pipeline_libraries, false, "Gears2",
                    "0.6.5 (Vulkan): build each new pipeline from per-stage pipeline libraries "
                    "(VK_EXT_graphics_pipeline_library, SDK patch 116 / runtime 066): each shader is compiled once, and a "
                    "new state combination is a link of a few microseconds instead of a whole build (bench 1755: 87 ms "
                    "mean). Used only where the driver offers fast linking; else pipelines are built whole");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_count_pixels, false, "Gears2",
                    "Patch 0098 (Vulkan, a measurement; with --gow2_renderer_gpu_by_target): the samples that pass depth "
                    "/ stencil per render target, from occlusion queries - whether Vulkan's ~2x colour-target GPU time "
                    "(jobs 402-403) is more pixels or dearer pixels");
REXCVAR_DEFINE_STRING(gow2_renderer_vulkan_barrier_stages, "all", "Gears2",
                      "Patch 0107 (Vulkan): all (default, as before) - every barrier waits for ALL_COMMANDS | work - "
                      "each barrier waits only for the kinds of GPU work recorded since the last one (graphics, compute "
                      "or transfer; ALL_COMMANDS after anything else and at the start of a command buffer). Jobs 415-416: "
                      "with every draw timed apart both APIs take ~6.3 ms for a ch15 frame; normally D3D12 takes 4.4 "
                      "(its work overlaps) and Vulkan 6.6, with ~180 full barriers a frame | skip_memory (patch 0108, a "
                      "MEASUREMENT mode that may give wrong pictures): barriers with no image layout change are not sent "
                      "(counted as 'all' in the 300-frame line)");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_depth_bias_control, true, "Gears2",
                    "22 Nov (Vulkan, devices without D24S8 such as AMD and the Steam Deck): keep the game's depth bias in "
                    "D24 units on the D32F stand-in target with VK_EXT_depth_bias_control where the driver has it "
                    "(as Xenia Canary and vkd3d-proton). false = the old behaviour (bias shrinks on far surfaces)");
REXCVAR_DEFINE_STRING(gow2_renderer_vulkan_attachment_layouts, "general", "Gears2",
                      "Patch 0100 (Vulkan, a measurement switch first): the layout render passes draw to their targets "
                      "in. general = VK_IMAGE_LAYOUT_GENERAL, as before (targets are also sampled) | optimal = "
                      "COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL while drawn to (as Canary does), back "
                      "to GENERAL in the next full barrier outside a pass. Job 404: Vulkan's colour targets cost ~2.5x "
                      "D3D12's per pixel, not more pixels; job 406: spirv-opt shaders gave no gain");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_record_thread, false, "Gears2",
                    "Vulkan (default off): record the command buffers on a recording thread, as D3D12's "
                    "threaded_recording does - our vkCmd* calls, vkBegin/EndCommandBuffer and vkQueueSubmit2 are "
                    "encoded on the renderer thread and replayed in order on another core (job 2204: Vulkan record "
                    "3.3 ms against D3D12's 0.8 ms). false = every call goes to the driver at once, as before");
REXCVAR_DEFINE_INT32(gow2_renderer_vulkan_record_thread_log, 60, "Gears2",
                     "With --gow2_renderer_vulkan_record_thread: one log line per this many frames with the per-frame "
                     "averages and maxima (caller encode ms, recording thread ms, caller wait ms, queue depth, forced "
                     "flushes by reason); 1 = every frame, 0 = none");
REXCVAR_DEFINE_INT32(gow2_renderer_draw_report_frame, 0, "Gears2",
                     "Patch 0101 (a diagnostic, both APIs): non-zero = from this frame, for "
                     "--gow2_renderer_draw_report_frames frames, every draw and every change of GPU work kind gets its "
                     "own GPU timestamp, and the log lists each one in order with its GPU time, render targets, shaders, "
                     "state, bound textures (as the API holds them) and samplers, then the 40 dearest. Same frame on "
                     "D3D12 and Vulkan (fixed seed / fixed step) = a draw-by-draw comparison. Timestamps between "
                     "every draw make that frame slower; only the report frames are affected");
REXCVAR_DEFINE_BOOL(gow2_renderer_draw_report_samples, false, "Gears2",
                    "Patch 0102 (with --gow2_renderer_draw_report_frame; Vulkan only so far): each reported draw also "
                    "counts the samples that pass depth / stencil (precise occlusion queries) - a draw that costs a lot "
                    "but passes few samples ran its pixel shader on pixels the depth test then threw away");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_discard_terminates, false, "Gears2",
                    "Patch 0102 (Vulkan, a measurement switch): DXBC discard becomes OpTerminateInvocation (the pixel's "
                    "lane stops, as DXC's default OpKill) instead of OpDemoteToHelperInvocation (it runs on to the end "
                    "as a helper). Every pixel shader has one (the alpha test). Job 408: Vulkan's textured ch15 scene "
                    "draws cost ~2.4x D3D12's (some 10-20x) while depth-only draws match; job 402: DXC's SPIR-V (OpKill) "
                    "drew ch15 ~1 ms faster than ours");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_float_controls, true, "Gears2",
                    "Patch 0103 (Vulkan, a measurement switch): false = our SPIR-V without ExecutionMode "
                    "SignedZeroInfNanPreserve, so NVIDIA's compiler may use the fast float paths D3D12's DXBC compiler "
                    "uses (D3D12 has no such mode). Job 409: Vulkan's textured draws cost ~2x D3D12's per pixel even "
                    "without a depth test, while depth-only draws match");
REXCVAR_DEFINE_INT32(gow2_renderer_draw_report_frames, 1, "Gears2",
                     "Patch 0101: how many frames --gow2_renderer_draw_report_frame reports (1-4)");
REXCVAR_DEFINE_INT32(gow2_renderer_vulkan_async_play_threads, 2, "Gears2",
                     "0.6.1 (with --gow2_renderer_vulkan_async_shaders): background threads that convert the shader "
                     "backlog while the game draws (64+ draws in the frame); on loading screens, menus under 64 draws "
                     "and in a chapter hold all of them work. A shader a draw waits for is never limited");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_async_shaders, false, "Gears2",
                    "0.6.1 (first playthrough, Vulkan): a shader whose SPIR-V is not in the store is converted on "
                    "background threads; until it is ready its draws are skipped as for any shader still being made, "
                    "and a draw that needs it moves it to the front. Nothing is converted on the render thread or "
                    "before the first frame. Off (default until measured in play) = converted where 0.6.0 does it");
// OFF BY DEFAULT in 0.6.1 (both switches): the conversion still runs before the first frame and, for a shader first
// seen in play, on the render thread - the optimiser there made a first Vulkan start three times longer (Gears 3
// 31 -> 106 s) and would add ~100 ms to every first-use hitch. They go on once Vulkan shaders are made on background
// threads (see CLAUDE-NOTES, 13 Nov).
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_spirv_optimise, false, "Gears2",
                    "0.6.1 (Vulkan): each shader converted to SPIR-V goes through SPIRV-Tools' performance passes before "
                    "the driver builds it - about a third of the size and a third less driver time per new pipeline "
                    "(303 -> 193 ms on an RTX 3080 Ti; D3D12 158 ms). Same values (NoContraction and the float controls "
                    "are kept). false = the modules as converted, as 0.6.0");
// 0.6.5 (Shagg 16 Nov, "5"): on by default. Jobs 1957b / 1958: Gears 3 on Vulkan converted every recorded shader again at
// EVERY start (18 s for 5,698, 48 s for 8,978) - the store had been switched off together with the optimiser after the
// 0.6.1 candidate's slow first start (the optimiser's cost; the optimiser stays off).
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_store, true, "Gears2",
                    "0.6.5: translated shaders (DXBC) are kept in <shader cache dir>\\dxbc-store.bin, so each recorded "
                    "shader is translated once per COGBox version and not at every start (Gears 3: 9,296 shaders, 14 s "
                    "of CPU). false = translate at every start, as 0.6.4");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_spirv_store, true, "Gears2",
                    "0.6.1 (Vulkan): converted shaders are kept in <shader cache dir>\\spirv-store.bin, so each shader is "
                    "converted to SPIR-V (and optimised) once and not at every start (Gears 3: half a minute before the "
                    "first frame). false = convert at every start, as 0.6.0");
REXCVAR_DEFINE_BOOL(gow2_renderer_ps_refactoring, false, "Gears2",
                    "Patch 0095 (DXBC back end, D3D12 and Vulkan): pixel shaders that don't write depth let the driver "
                    "fuse multiply-adds and reorder maths (DXBC refactoring allowed; no NoContraction on Vulkan). Vertex "
                    "shaders and depth-writing pixel shaders stay precise, so positions and depth match between passes "
                    "(notes/42). Colours may differ in the last bit. Changes the pixel shaders (pipelines rebuild once)");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_precise, true, "Gears2",
                    "Patch 0088 (Vulkan, a MEASUREMENT switch): false = our shaders' SPIR-V without NoContraction, so "
                    "the driver may fuse multiply-adds (FMA). Our maths rules want them unfused (notes/38): false may "
                    "change pixels and vertex positions slightly - for timing only");
REXCVAR_DEFINE_BOOL(gow2_renderer_memexport_f16_nan_zero, true, "Gears2",
                    "Patch 0087 (Canary 3ff230d): a NaN written by memory export as float16 becomes 0 (it was -131008). "
                    "DXBC back end; changes only memory-export shaders with float16 formats");
REXCVAR_DEFINE_BOOL(gow2_renderer_canary_decode, true, "Gears2",
                    "Patch 0037/0041 (Canary 92ada8e, fbdb1f2): co-issued scalar operand read as W/Z and two-coordinate "
                    "tfetch1D (the second coordinate used since patch 0087, DXBC back end, only when the fetch constant "
                    "is not 1D - Canary 7cd4794). On by default again since 0043. Changes the generated shaders");
REXCVAR_DEFINE_STRING(gow2_renderer_shader_backend, "dxbc", "Gears2",  // job 520: DXBC for all three (COGBox never passed it; Judgment set it itself)
                      "Overhaul option B: hlsl = guest shaders go HLSL -> DXC as before (120-230 ms each, compile threads, "
                      "shader cache); dxbc = translated straight to DXBC, ready in under a millisecond (D3D12 only; no "
                      "compile threads or shader cache needed). A shader the DXBC translator cannot do takes the HLSL path");
REXCVAR_DEFINE_BOOL(gow2_renderer_shader_prefetch, true, "Gears2",
                    "Patch 0053: start compiling a shader as soon as our PM4 processor meets it for the first time "
                    "(it parses ahead of the renderer), not when its first draw is reached. false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_defer_cosmetic, true, "Gears2",
                    "Patch 0052: a blended draw that writes no depth and is not a full-screen pass (decals, particles, "
                    "glows) is left out while its shader compiles instead of stopping the frame; everything else waits "
                    "(--gow2_renderer_shader_wait_ms). false = every draw waits");
REXCVAR_DEFINE_BOOL(gow2_renderer_shader_lookahead, true, "Gears2",
                    "Patch 0051: when a draw has to wait for a shader still compiling, first look up the shaders of the "
                    "draws already queued behind it, so new ones compile on all compile threads at once instead of one "
                    "after another on the draw thread. false = as before");
REXCVAR_DEFINE_DOUBLE(gow2_present_gamma, 1.0, "Gears2",
                      "GitHub #185: extra gamma on the game's display ramp at present (1 = original; 1.2 = darker shadows, highlights kept)");
REXCVAR_DEFINE_BOOL(gow2_renderer_tex_post_constants, true, "Gears2",
                    "Patch 0105 (with --gow2_renderer_tex_num_format, the default): the texture-fetch post-processing "
                    "values that depend only on the fetch constant (per component sign mode, signed mode and fixed "
                    "width; per fetch fixed / integer / point sampled) are worked out once per draw on the CPU and read "
                    "by the shaders from the fetch-constant buffer, instead of per pixel (a 25-way format switch and "
                    "~40 integer ops per fetch). Same results by construction. Job 412: that per-pixel work is ~1.6 ms of "
                    "ch15's 5.3 ms of Vulkan draws (0.7 ms of D3D12's 3.3). Changes the generated shaders. Job 630: on by default "
                    "(job 629b: the three games' title screens identical off and on)");
REXCVAR_DEFINE_BOOL(gow2_renderer_tex_num_format, true, "Gears2",
                    "Patch 0040 (Canary d119505 / 2ddc5ef / 6a45452 / 0c843ef): texture fetches of fixed-point formats "
                    "follow Canary - integer num_format restores the guest integer, unsigned-biased decodes as offset "
                    "binary, unsigned normalised rounds to 16 fractional bits. Changes the generated shaders. false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_standin_bounded_wait, true, "Gears2",
                    "0.5.1 (D3D12; gameplay diagnostics 10 Nov: Judgment froze 168 ms and 46 ms building one pipeline inside a frame): a stand-in draw (patch 0045) asks for its pipeline on a worker and waits at most the frame's stand-in budget, instead of building it on the renderer thread with no limit; not ready in time = skipped until its pipeline is ready (memexport draws still build at once). false = as before");
REXCVAR_DEFINE_DOUBLE(gow2_renderer_standin_wait_ms, 10.0, "Gears2",  // job 519: was 1000 - waits of 585 / 810 ms on a frame (Gears 3 hitches)
                      "Patch 0045 (Edge de8e606 / 8a4d405 / Edge HEAD GetPipelineStandInWaitReason): a draw whose skipping "
                      "would leave a hole that stays - a small render target (at most 160 pixels wide: generated data), "
                      "pixel-shader memexport, a new memexport vertex shader, or a new vertex shader drawing to a target "
                      "not drawn in the last 4 frames - waits for its shaders and creates its pipeline synchronously, up "
                      "to this many ms per frame in total. 0 = off (skip while compiling, as before)");
REXCVAR_DEFINE_INT32(gow2_pgo_dump_frame, 0, "Gears2",
                     "Patch 0159: in a profile-guided-optimisation training build (GN_PGO=gen) write the execution counters "
                     "(LLVM_PROFILE_FILE) once, at this swap. 0 = never. Does nothing in a normal build");
REXCVAR_DEFINE_INT32(gow2_renderer_cpu_split, 0, "Gears2",
                     "Patch 0153 (job 458: the part timers took ~10% of the renderer thread): our renderer's own CPU "
                     "timing per kind of work. 0 = off (fastest; the CPU split line shows 'off' and HITCH parts read 0); "
                     "1 = part sums and stretches over 20 ms (the CPU split line and HITCH parts, cheap); 2 = also "
                     "every stretch over 50 us summed per source line (patch 0036 detail, as before 0153). Patch 0154: default "
                     "0 (job 461: 1 cost ~3% fps); test jobs that need the CPU split ask for 1");
REXCVAR_DEFINE_INT32(gow2_renderer_submit_draws, -1, "Gears2",
                     "Patch 0138: submit our recorded work to the GPU after every N draws (0 = once a frame, as before "
                     "0.6.1). -1 (the default since 0.6.1; Judgment in play at 2x, job 1711: 12.5 -> 11.1 ms a frame on "
                     "D3D12, 14.6 -> 11.5 on Vulkan, the same picture) = adaptive, with strict queries only: as soon as the GPU has finished everything "
                     "submitted so far and at least 64 draws are recorded, at the latest every 256 draws, at most 12 "
                     "a frame. Measured in Gears 2 at 2x with strict "
                     "queries (jobs 1679 / 1681): every 100-150 draws = 4.8 ms a frame against 5.95 (the occlusion "
                     "answers the title waits for come back sooner); every 50 is worse again");
REXCVAR_DEFINE_INT32(gow2_renderer_frames_in_flight, 1, "Gears2",
                     "Patch 0133 (input latency, Shagg 22:10): the most frames our renderer lets queue on the graphics "
                     "card before it waits (was fixed at 2). 1 = lower input lag (the card works on one frame while the "
                     "next is recorded), may cost some fps when the card is the bottleneck; 2 = as before. 1-3. Patch 0152: "
                     "default 1 (jobs 456/457b: same fps at 1x and on 4 cores, -1.6% at 2x; Shagg 09:03)");
REXCVAR_DEFINE_DOUBLE(gow2_renderer_depth_pipeline_wait_ms, 3.0, "Gears2",
                      "Patch 0132 (Gears 3 / Judgment: a skipped depth-only or shadow draw leaves wrong lighting or black "
                      "blocks for that frame, worse than a late object): a draw with no colour target that tests or writes "
                      "depth / stencil may wait this many ms per frame in total for its pipeline to come off a worker "
                      "(never built on the frame); past that it is skipped like any other. 0 = off");
REXCVAR_DEFINE_DOUBLE(gow2_renderer_fullscreen_wait_ms, 8.0, "Gears2",
                      "Job 621 (Shagg 15:33: a white frame during a fast 360 turn, cold shader cache): a full-screen pass "
                      "(a rectangle or a quad drawn to a colour target - post effects such as motion blur that only run "
                      "while turning) waits for its shaders and creates its pipeline on the spot, up to this many ms per "
                      "frame in total, instead of being skipped - a skipped one leaves its target holding old or empty data "
                      "that the final picture then shows. 0 = off (skip while compiling, as before). Job 632: 8 (was 50 - job 631b's cold "
                      "run froze for up to 2-3 s while full-screen passes waited)");
REXCVAR_DEFINE_DOUBLE(gow2_renderer_fullscreen_pipeline_wait_ms, 50.0, "Gears2",
                      "GitHub #287 (D3D12): a full-screen pass whose pipeline is still on a worker after the "
                      "--gow2_renderer_fullscreen_wait_ms budget, and was already skipped once, waits for it once more, up to "
                      "this many ms - at most one such wait a frame, 4 a second and one per pipeline: one short hitch instead of several frames without the pass "
                      "(a Gears 2 cutscene's lightning went black for 4 frames). 0 = off (as 0.6.1)");
REXCVAR_DEFINE_BOOL(gow2_renderer_standin_new_vs, false, "Gears2",
                    "Patch 0132: also treat 'new vertex shader drawing to a target not drawn in the last 4 frames' as a "
                    "stand-in wait (0045's broad case). Off by default: Shagg's 21:34 session spent 1.8 s in 34 such "
                    "waits (100-700 ms each, the pipeline built on the frame). The narrow cases stay: small render "
                    "target, pixel memexport, new memexport VS");
REXCVAR_DEFINE_BOOL(gow2_renderer_vertex_any_type, true, "Gears2",
                    "Patch 0049 (Canary b083312, gpu_allow_invalid_fetch_constants default true): upload the vertex data of "
                    "a vertex fetch constant whose type field is not kVertex (3) instead of skipping it. false = skip, as "
                    "before");
REXCVAR_DEFINE_BOOL(gow2_renderer_export_wait, false, "Gears2",
                    "Patch 0024 (Edge c8503efe6, Gears 3 review 03-p): a draw whose shader memexports is never skipped "
                    "for a still-compiling shader or pipeline: it waits (up to 2 s) and creates its pipeline "
                    "synchronously, so a new NPC's first skinning export is not lost");
REXCVAR_DEFINE_BOOL(gow2_renderer_export_prepare_mirror, false, "Gears2",
                    "Patch 0024 (Canary RequestRange before an export draw, Gears 3 review 03-o): the pages a memexport "
                    "writes get the CPU's newest bytes in the mirror first, so the parts of those pages it does not "
                    "write stay right when the whole pages become GPU-current");
REXCVAR_DEFINE_BOOL(gow2_renderer_gpu_block_textures, true, "Gears2",
                    "Patch 0021 (Canary texture_cache, D36; default on since patch 0031, jobs 331/333 and Gears 3 091b/092): BC1-5 (DXT1/3/5, DXT5A, DXN) textures - 2D and cubes - load "
                    "on the GPU from the guest-memory mirror like the other formats, so they see our resolves and "
                    "memexports without a readback (Gears 3 ch22 squad). false = CPU path from guest memory, as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_gpu_load_stacked_3d, true, "Gears2",
                    "Stacked textures fetched as 3D (Gears 3's colour-grading table, rendered every frame) load on the "
                    "GPU from our resolves; false = the CPU path, which reads stale bytes in takeover");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_other_bpp, true, "Gears2",
                    "Patch 0030 (Canary resolve_full_8/16/128bpp): colour resolves into 8, 16 and 128 bpp destinations "
                    "and 64/128-bit endian swaps are written (false = skipped as before, the pages keep old bytes)");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_reuse, true, "Gears2",
                    "PR #43 (legoliamneeson): a new resolve destination (1x or scaled) takes the texture of one of the "
                    "same size unused for four frames instead of creating one - less creation, destruction and "
                    "video-memory pressure while streaming. Never a shown (front) buffer. false = always create");
REXCVAR_DEFINE_INT32(gow2_renderer_dest_evict_frames, 600, "Gears2",
                     "Patch 0032: resolve destination textures (1x and scaled) not used for this many frames are freed "
                     "(0 = never, the old behaviour: they piled up and over-filled video memory in long Gears 3 runs)");
REXCVAR_DEFINE_INT32(gow2_renderer_dest_budget_mb, -1, "Gears2",
                     "Patch 0032/0033: -1 (default, 0033) = free the least recently used resolve destination textures "
                     "(not used in the last 30 frames) only when this process uses over 90% of the OS video memory "
                     "budget, down to 80%; N > 0 = keep their total under N MB (0032's 768 churned at 2x scale: "
                     "~300 freed and remade per 300 frames); 0 = no budget (age only)");
// GitHub #257 (Gears 3 sub-menus: 24 full-screen colour resolves a frame, GPU-bound at 2x): a colour resolve that repeats
// the previous copy exactly - the same copy / destination / surface / scissor registers and rectangle vertices, no draw
// of the guest's in between, neither copy clearing - copies the same EDRAM tiles to the same place again, so the
// destination (texture and guest memory) already holds what it would write. Counted always (the colour line of the
// stats); skipped only with this switch (each repeat at 2x costs its 1x pass, its scaled pass and its guest-memory write).
REXCVAR_DEFINE_BOOL(gow2_renderer_skip_repeat_resolves, false, "Gears2",
                    "GitHub #257 test: a colour resolve identical to the one just before it (no draw between, no clear) "
                    "is not done again; the repeats are counted in the stats either way");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_view_targets, true, "Gears2",
                    "Colour resolves from a format view no draw used (Gears 3: 7e3 scene resolved as 2_10_10_10 raw) "
                    "create that target and transfer the tiles in, instead of skipping the resolve");
REXCVAR_DEFINE_BOOL(gow2_renderer_textures, true, "Gears2",
                    "Our renderer reads guest textures through its texture cache (false = white stand-ins, as M1)");
REXCVAR_DEFINE_BOOL(gow2_renderer_upload_watch, true, "Gears2",
                    "Skip re-uploading vertex ranges of 64 KB+ that the guest has not written since (page watch)");
// Patch 0014 (Gears 3 notes/15, 2026-10-23): our resolves marked their pages GPU-current without arming the write
// watch there (the memexport path arms first). A later CPU write to those pages - the game reusing the memory, e.g.
// streaming a texture in - then raised no callback: the pages stayed GPU-current, GPU-path texture loads kept reading
// our old resolve from the mirror, and CPU-path loads showed up as "CPU-path loads that read our pages (should be 0)"
// (ch22: 74 at load, 10-20 per 300 gameplay frames).
REXCVAR_DEFINE_BOOL(gow2_renderer_watch_resolves, true, "Gears2",
                    "Arm the CPU write watch on the pages each of our resolves writes, so a later CPU write there ends "
                    "their GPU-current state (use with --gow2_renderer_watch_narrow=true; the old 4 MB widening would "
                    "drop GPU-current pages the CPU did not write)");
REXCVAR_DEFINE_BOOL(gow2_renderer_watch_narrow, true, "Gears2",
                    "Patch 0008: a CPU write widens the page watch's invalidation like the reference emulator (within "
                    "256 KB, never across a page our GPU wrote - resolves, memexport) instead of the whole 4 MB block, "
                    "so resolve / export results that live only on the GPU are not replaced by older guest bytes. "
                    "false = as before (the summary still counts the GPU-written pages the 4 MB widening hits)");
REXCVAR_DEFINE_BOOL(gow2_renderer_transfers, true, "Gears2",
                    "EDRAM ownership transfers into depth targets (depth -> depth, colour clears with a known value)");
REXCVAR_DEFINE_STRING(gow2_renderer_ownership, "rect", "Gears2",
                      "EDRAM tile ownership rule our renderer acts on: rect (default, hardware model: the tiles a draw's "
                      "scissor and viewport touch, only when it writes) or canary (the reference's estimate: whole rows "
                      "from the base down to the scissor/viewport bottom, for every bound target). The other rule is "
                      "tracked too, and pixels where they disagree are labelled in the comparison");
REXCVAR_DEFINE_STRING(gow2_renderer_tile_trace, "", "Gears2",
                      "EDRAM tiles (indices 0-2047: list and ranges, e.g. 1440-1441,1450; empty = off, the default) "
                      "whose every event is logged in order, every frame: ownership claims (both rules), transfers, "
                      "draws, resolves, with one sample of our depth target after each. Debugging only - very large "
                      "logs (patch 0038: the old default list, used only on the comparison frames 0035 removed, wrote "
                      "5.5 GB in one Gears 3 session); ch02 copy 2 was 1440-1441,1450-1451,1460-1461,1470-1471");
// Gears 3 notes/15 (black squad): on chosen comparison frames, write each compared colour resolve raw to disk - our
// words, the reference's words (same convention), the last-writer draw ID per pixel - plus the frame's draw list, so a
// region that differs (a dark character) can be traced to the draw that wrote it. Diagnostic only; off by default.
// Gears 3 patch 0015 (notes/15, ch2 rainbow: the tone-map draw is issued but rasterizes nothing; its VS takes the
// position straight from vf0): log the vertex bytes a chosen vertex shader's draws read, as uploaded. Diagnostic only.
REXCVAR_DEFINE_STRING(gow2_renderer_log_vs, "", "Gears2",
                      "Vertex shader hash (16 hex digits, e.g. A9C7FE3382D41904): log each of its draws' vertex fetch "
                      "constants and the first vertices (as floats, after the endian swap) at upload time (empty = off)");
REXCVAR_DEFINE_INT32(gow2_renderer_log_vs_max, 400, "Gears2", "Most draws --gow2_renderer_log_vs logs");
REXCVAR_DEFINE_STRING(gow2_renderer_tile_trace_pixel, "7,0", "Gears2",
                      "Which pixel of a traced tile is sampled (x,y inside the tile, in the target's host pixels)");
REXCVAR_DEFINE_BOOL(gow2_renderer_float24_remap, true, "Gears2",
                    "D24FS8 targets keep guest depth * 0.5 on the host, as the reference does (full_float24_in_0_to_1 "
                    "with host render targets): the whole 20e4 range [0, 2) then survives EDRAM reinterpretation "
                    "round trips (a D24S8 word near 1.0 read as 20e4 is >= 1.0)");
REXCVAR_DEFINE_STRING(gow2_renderer_alt_rule, "compare", "Gears2",
                      "When the other ownership rule is tracked for labels: compare (default: only on comparison frames, "
                      "started from a copy of the acting rule's state), always, or off");
REXCVAR_DEFINE_BOOL(gow2_renderer_colour, true, "Gears2",
                    "M2: draw colour targets (8888, 2_10_10_10, 7e3, 16_16_FLOAT, 16_16_16_16_FLOAT, 32_FLOAT, "
                    "32_32_FLOAT) and compare colour resolves; false = M1 behaviour (depth only)");
REXCVAR_DEFINE_BOOL(gow2_renderer_gpu_texture_load, true, "Gears2",
                    "Untile and endian-swap uncompressed 2D guest textures on the GPU (from the guest-memory mirror) "
                    "instead of on the CPU");
REXCVAR_DEFINE_BOOL(gow2_renderer_async_pipelines, true, "Gears2",
                    "Shadow renderer: create game-draw pipeline states on worker threads; a draw whose pipeline "
                    "is not ready yet is skipped and counted as pending (the resolve then says INCOMPLETE) (upl-n108)");
REXCVAR_DEFINE_DOUBLE(gow2_renderer_pipeline_wait_ms, 0.0, "Gears2",
                      "Patch 0132: 0 by default (never wait on the driver; the draw pops in when its pipeline is built). Gears 3 job 037: with async pipelines, each frame may wait up to this many ms in total for new "
                      "pipeline states instead of skipping their draws (a skipped draw leaves the previous content - "
                      "Gears 3 ch17's random blue/rainbow skin, job 034). Patch 0047: -1 = no cap, the default (never "
                      "skip). 0 = never wait (skip, as before 0047)");
REXCVAR_DEFINE_BOOL(gow2_renderer_transfer_mixed_bpp, false, "Gears2",
                    "Patch 0005 (Gears 3 2026-10-23): EDRAM ownership transfers between 32bpp and 64bpp targets - a "
                    "64bpp colour target's tiles into a depth target, and colour words between 32bpp and 64bpp colour "
                    "targets (Canary's sample mapping). Off = those tiles stay as the host texture last held them "
                    "('unknown ... colour' in the summary; Gears 3 ch22: ~476 tiles a frame, incl. the per-object "
                    "shadow depth)");
REXCVAR_DEFINE_BOOL(gow2_renderer_memexport, false, "Gears2",
                    "Gears 3 (notes/11): run vertex shaders that memexport - their eA/eM writes go to the guest-memory "
                    "mirror (u0), export-only draws (no rasterization) run as point lists with no targets, the written "
                    "ranges become GPU-current and later vertex fetches read them from the mirror. false: such draws "
                    "are skipped, as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_export_pending_mirror, true, "Gears2",   // #315 (review r44 R1): on by default
                    "Patch 0007 (Gears 3 2026-10-23, NPC vertex explosions; race found by both chats): while an "
                    "exported range's readback has not landed in guest memory yet, its pages keep feeding vertex "
                    "fetches from the mirror (the export's bytes) even when a CPU write elsewhere in the same 4 MB block "
                    "ended their GPU-current state - instead of re-uploading the pre-export guest bytes over them. On by "
                    "default since 0.6.3 (GitHub #315: a texture moved twice copies its source through this path). "
                    "false = as 0.6.2");
REXCVAR_DEFINE_BOOL(gow2_renderer_export_settle, false, "Gears2",   // parked (Shagg 11-14 15:32): off until proven
                    "Green-vine review (job 1844): when a memexport read-back has landed in guest memory, its pages that "
                    "nothing wrote since stop being GPU-current (guest memory now holds the same bytes), so a texture "
                    "later loaded there takes guest memory - which also has any CPU / file write the page watch could not "
                    "see (read-only pages are never watched). false = they stay GPU-current until a seen write, as before");
// GitHub #315 (Judgment, AMD RX 7900 XT at 165 fps: blue / purple blocks that build up chapter after chapter): hole H2 of
// the green-vine review. Judgment / Gears 3 move a streamed texture's small mips
// with memexport copies; the export is read back into guest memory at the end of the frame. An exact-range callback
// (a file read next to it, protect-to-writable, decommit, release) can end the pages' GPU-current state BEFORE that
// read-back lands; a texture loaded then takes guest memory - the pre-move bytes - and the read-back, landing later
// through the physical mapping, bumps nothing, so the texture keeps the wrong small mips for good. With this on, a texture
// load over pages our GPU ever wrote first lands any pending read-back over them (FlushExportReadbacksFor: no-op and no
// wait when none overlaps). It was tied to --gow2_renderer_export_settle (review r26 R1, for a clean A/B), which stays
// parked off; r26 / r44: correct, waits only when a load overlaps an export still in flight (to be measured on a streaming run; inert in
// Gears 1 / 2, which move no textures with memexport). The #315 log (0.6.2): 1922 GPU-current pages ended by exact-range
// callbacks in one 28-minute session (how many of them before their read-back landed is not logged).
REXCVAR_DEFINE_BOOL(gow2_renderer_texture_land_exports, true, "Gears2",
                    "GitHub #315 (blue / purple blocks, Judgment): before a texture loads from guest memory pages our GPU "
                    "once wrote, land any memexport read-back still pending over them (else the texture can keep the "
                    "bytes from before the game moved it). false = only while the pages are still GPU-current (0.6.2), unless "
                    "--gow2_renderer_export_settle is on");
REXCVAR_DEFINE_BOOL(gow2_renderer_memexport_readback, true, "Gears2",
                    "With --gow2_renderer_memexport: copy every exported range back into guest memory once the GPU has "
                    "written it (job 041: Gears 3 moves DXT textures with memexport copies, and DXT textures load from "
                    "guest memory, so without this they keep the old bytes - pale blue skin). The reference emulators' "
                    "readback_memexport");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_through, true, "Gears2",
                    "0.4.6 on by default (job 1219, Gears 2 in play: ~12 reloads a frame skipped, texture time ~1.5 -> ~0.7 ms, its texel-by-texel check 0 differences). Job 047: a resolve into memory that a cached texture is loaded from (same base, pitch and bytes "
                    "per texel, single level) also writes that texture, the same way the GPU untile would load it, so "
                    "the texture is not reloaded (Gears 3 ch8: the shadow map is resolved and reloaded ~15 times a "
                    "frame, bloom buffers ~20)");
REXCVAR_DEFINE_INT32(gow2_renderer_resolve_through_check, 0, "Gears2",
                     "Job 047 check: at this frame, the first 12 resolve-through writes are also loaded the old way "
                     "(GPU untile from guest memory) and compared texel by texel on the GPU (0 = off)");
REXCVAR_DEFINE_BOOL(gow2_renderer_transfer_bits, true, "Gears2",
                    "Shadow renderer: EDRAM transfers into and out of 16/32-bit float and SNORM16 colour targets go "
                    "through UINT views, so NaN payloads and -32768 survive (as Canary); off = through float as "
                    "before n108 (job 111: 01407C0F -> 01407FFF in a 16_16_FLOAT target) (upl-n108)");
REXCVAR_DEFINE_BOOL(gow2_renderer_transfer_selftest, true, "Gears2",
                    "Shadow renderer: at the first frame, send 1280 known EDRAM words colour -> depth -> colour through "
                    "our transfer passes and log any word that doesn't come back bit-exact (upl-n108)");
REXCVAR_DEFINE_BOOL(gow2_renderer_tessellation, true, "Gears2",
                    "Draw Xenos quad-patch tessellated draws (the chapter 18 water) as grids through the game's vertex "
                    "shader, as the tessellator feeds it (false: skipped and counted, as before job 148)");
REXCVAR_DEFINE_INT32(gow2_renderer_resolution_scale, 1, "Gears2",
                     "Stage 7: render at N x the console's resolution on each axis (1-4; 2 = 2560x1440 from 1280x720, "
                     "3 = 3840x2160). Render targets, viewports, scissors and EDRAM transfers are scaled (notes/53). "
                     "0 = auto (\"Match my monitor\"): the smallest N whose image covers the monitor the game is on");
REXCVAR_DEFINE_BOOL(gow2_renderer_ownership_targeted, false, "Gears2",
                    "Experiment (job 2155): a change of EDRAM tile ownership re-checks only the targets whose tiles it "
                    "can have touched, not every target - fewer walks over a target's tiles (the 'ownership' CPU part, "
                    "which grows with the picture size at true 1080p). Off: as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_edram_extended, false, "Gears2",
                    "Experiment (true 1080p, Shagg 20 Nov): model 40 MiB of EDRAM in 8192 tiles instead of the console's "
                    "10 MiB in 2048, as Xenia Canary's edram_tile_count = extended - room for a game patched to draw "
                    "above 1280x720 (1920x1080 colour + depth = 3264 tiles). Surface bases are read with 13 bits. Off: "
                    "the console's size, as before");
REXCVAR_DEFINE_BOOL(gow2_present_native_size, false, "Gears2",
                    "\"Match my monitor\" (Shagg 23 Oct): the presented image is resampled to the window's exact size "
                    "(box filter shrinking, bilinear enlarging), so it is shown 1:1 with no stretch by the presenter; "
                    "the game's shape is kept (bars only where the window's shape differs). Needs --gow2_present=ours");
REXCVAR_DEFINE_STRING(gow2_present_upscale_filter, "bilinear", "Gears2",
                      "0.2.0: how \"Match my monitor\" enlarges the image when the window is bigger than the game's: "
                      "bilinear (as before) or lanczos (sharper; clamped to the nearest source pixels, so no ringing)");
REXCVAR_DEFINE_DOUBLE(gow2_present_sharpen, 0.0, "Gears2",
                      "0.2.0: contrast-adaptive sharpening (RCAS, kSharpenCs) of the image as shown, after it is resized "
                      "to the window - 0 = off, 1 = full");
REXCVAR_DEFINE_INT32(gow2_renderer_resolution_scale_threshold, 1120, "Gears2",
                     "Gears 3 (notes/53b): with a resolution scale, render targets whose surface pitch is at or below "
                     "this many pixels stay at 1x - the downscaled post-process chains (bloom, blur, depth of field), "
                     "which gain nothing from the scale and break with it, as the reference emulators' "
                     "draw_resolution_scale_threshold (364 or 512 suggested there for Gears 3). 0 = scale every target. "
                     "Default 1120 since job 294 (job 292, ch10 at 2x: the white line along the left/top edges, +60/+22 "
                     "brightness with 0, gone with 1120; Gears 3's green tint): the 1280-wide scene targets still scale");
REXCVAR_DEFINE_STRING(gow2_renderer_resolution_scale_pitches, "", "Gears2",
                      "GitHub #105: surface pitches (pixels, comma-separated, e.g. 640) drawn at the resolution scale even "
                      "though they are at or below --gow2_renderer_resolution_scale_threshold - Gears 3's depth of field "
                      "is otherwise drawn at the console's size and enlarged. Their viewports, scissors, resolves and the "
                      "textures read from them follow the target's own scale; the game's texel offsets stay in "
                      "normalised source-size units, so kernels keep their footprint. Empty = off (as 0.4.5). No effect at 1x");
REXCVAR_DEFINE_STRING(gow2_renderer_resolution_unscaled_pitches, "", "Gears2",
                      "GitHub #212: surface pitches (pixels, comma-separated) kept at 1x with a resolution scale although "
                      "above --gow2_renderer_resolution_scale_threshold - Gears 3 sets 1360, its 1344-wide outdoor sun "
                      "shadow map (pixelated shadows at 2x). Empty = off. No effect at 1x");
REXCVAR_DEFINE_INT32(gow2_renderer_camera_log, 0, "Gears2",
                     "Diagnostic (GitHub #132): every N frames, the scene camera taken from the view-projection in c0-c3 "
                     "of the frame's first scene-depth draw - world position, yaw / pitch, vertical and horizontal field "
                     "of view, viewport - so a 16:9 and an ultrawide run can be compared. 0 = off; at most 2000 lines");
REXCVAR_DEFINE_BOOL(gow2_screenshot_presented, false, "Gears2",
                    "Shagg's to-do 8: the screenshot key (\\) saves the image as shown in the window (its full size, after "
                    "resizing and sharpening); false (default) = the game's output size, as 0.4.5. Frame shots keep the game's size");
REXCVAR_DEFINE_BOOL(gow2_renderer_fill_ownership, true, "Gears2",
                    "Job 188: the rect-list fill/clear shader (VS 1E6883FCCDE1F688) takes EDRAM ownership only of the "
                    "tiles its rectangle covers (bounds from its 3 vertices), not of its whole scissor rectangle - fewer "
                    "depth transfers (job 185: its unbounded scissor made most transfers)");
REXCVAR_DEFINE_BOOL(gow2_renderer_scaled_partial, true, "Gears2",
                    "Stage 7: a texture whose memory our resolves wrote only in part (the rest never resolved into) is "
                    "still taken at the scale when at least half its pages are scaled-current (job 193)");
REXCVAR_DEFINE_BOOL(gow2_renderer_scaled_textures, true, "Gears2",
                    "Stage 7 phase B: with a resolution scale, resolves also write a scaled copy, textures wholly held by "
                    "one are sampled at the scale, and the front buffer is presented at the scale (false: phase A - "
                    "1x resolves, 720p output)");
REXCVAR_DEFINE_INT32(gow2_renderer_tess_scale, 1, "Gears2",
                     "Tessellation detail multiplier (1 = the console's level; 2 = twice as fine, an option)");
REXCVAR_DEFINE_INT32(gow2_renderer_texture_stale_log, 0, "Gears2",
                     "Job 150 diagnostic: log textures that stayed unchanged for N frames and then got new bytes, with "
                     "our recent resolves that overlap them (0 = off; first 400 events)");
REXCVAR_DEFINE_BOOL(gow2_renderer_front_copy_lean, true, "Gears2",
                    "Speed study (on by default since 0.6.6; picture-checked in Gears 2): the front buffer's resolve skips its copy into the "
                    "scaled-memory window and into guest memory when our presenter shows the frame (it reads the scaled "
                    "destination). Wrong if the game samples its own front buffer as a texture");
REXCVAR_DEFINE_BOOL(gow2_present_gamma_to_smaa, true, "Gears2",
                    "Speed study (D3D12): with SMAA, the swap's gamma pass also writes SMAA's colour image, so the "
                    "full-screen pass that unpacked it from the gamma pass's words is left out (same values)");
REXCVAR_DEFINE_INT32(gow2_present_gamma_to_smaa_from_swap, 0, "Gears2",
                     "Speed study (picture check): --gow2_present_gamma_to_smaa only from this swap on");
REXCVAR_DEFINE_INT32(gow2_renderer_vertex_watch_min_bytes, 256, "Gears2",
                     "Speed study: vertex ranges of at least this many bytes are watched for writes and copied again "
                     "only when written (16384 = as before: smaller ranges are copied at every use). Below 16384, a "
                     "smaller range that changes at two checks in a row is taken as a dynamic buffer and copied at "
                     "every use for the next 600 frames");
REXCVAR_DEFINE_BOOL(gow2_renderer_vertex_reuse, true, "Gears2",
                    "A direct vertex range drawn again before the next release point (fence, read pointer, interrupt) "
                    "and in the same command list reuses its ring copy without reading the guest bytes again (job "
                    "174; the title cannot legally rewrite memory an unfinished draw uses before it sees a fence)");
REXCVAR_DEFINE_INT32(gow2_renderer_stencil_transfer_bits, 0, "Gears2",
                     "Depth transfers without SV_StencilRef (NVIDIA): 0 all 8 stencil bit passes, 1 only the bits the "
                     "title ever sets or tests (job 179)");
REXCVAR_DEFINE_INT32(gow2_renderer_overlay_depth_nudge, 0, "Gears2",
                     "Job 712: draws with no depth write and a less/greater-or-equal test on a float24 depth target (decals: "
                     "Gears 1 blood) are moved this many 20e4 steps toward passing, so they stop flickering against the "
                     "surface under them (0 = off)");

REXCVAR_DEFINE_DOUBLE(gow2_renderer_decal_min_bias, 0.0, "Gears2",
                      "Job 713 (Gears 1 blood flicker; Canary game-compatibility #44, goldislead's Decal Fix for UE3): a draw "
                      "on a float24 depth target with polygon offset on but smaller than this (guest depth units, ~0.01 "
                      "works) gets this much, same sign - UE3 decals ship ~0.00005, which z-fights on PC (0 = off)");

REXCVAR_DEFINE_STRING(gow2_renderer_fill_vs_extra, "0A6D1DD7767FDF27", "Gears2",
                      "Job 709: more vertex-shader hashes (hex, comma-separated) treated as the rect-list fill/clear shader, "
                      "for the fill skips below. Gears 2/3/Judgment use 1E6883FCCDE1F688; Gears 1 (older XDK) its own. Empty = off");
REXCVAR_DEFINE_BOOL(gow2_renderer_fill_transfer_trim, true, "Gears2",
                    "Depth transfers asked for by the rect-list fill/clear shader skip the depth pass and/or the stencil "
                    "passes it overwrites itself, inside its exact rectangle (job 185)");
REXCVAR_DEFINE_BOOL(gow2_renderer_readonly_transfer_memo, true, "Gears2",
                    "A depth transfer for a read-only (test-only) access is skipped when neither target was written "
                    "since the same transfer last ran over that area (job 182)");
REXCVAR_DEFINE_BOOL(gow2_renderer_texture_memo, true, "Gears2",
                    "A draw with the same texture fetch constants as the draw before (same frame and release epoch, no "
                    "resolve or other binding since) keeps the bound textures without the cache lookups (job 181)");
REXCVAR_DEFINE_BOOL(gow2_renderer_index_reuse, true, "Gears2",
                    "Small index ranges drawn again in the same command list and release epoch reuse their converted "
                    "copy in the upload ring (job 178; same rule as vertex reuse)");
REXCVAR_DEFINE_BOOL(gow2_renderer_threaded_recording, true, "Gears2",
                    "Replay our D3D12 command-list calls on a recording thread instead of the GPU worker thread "
                    "(notes/52 section 10)");
REXCVAR_DEFINE_INT32(gow2_renderer_copy_threads, 0, "Gears2",
                     "Guest-memory copies into the upload ring run on this many worker threads, finished before each "
                     "submission and before the title can see the GPU got further (job 171; 0 = inline)");
REXCVAR_DEFINE_BOOL(gow2_renderer_float_constants_trim, true, "Gears2",
                    "Upload only the float constants each shader can read (false: all 256 per stage, as before)");
REXCVAR_DEFINE_STRING(gow2_upscaler, "off", "Gears2",
                      "Temporal anti-aliasing / upscaling of the 3D scene (notes/64): off, dlss (NVIDIA DLSS / DLAA, RTX "
                      "GPUs) or fsr (AMD FSR, any GPU). Runs on the "
                      "lit scene before the game's post-processing, with "
                      "camera jitter and motion vectors from depth; native-resolution AA at the render scale for now");
REXCVAR_DEFINE_DOUBLE(gow2_upscale_sharpness, 0.0, "Gears2", "FSR's own sharpening, 0..1 (DLSS ignores it)");
REXCVAR_DEFINE_STRING(gow2_upscale_mip_bias, "auto", "Gears2",
                      "Notes/82: while an upscaling mode draws the scene below the output, sharper mips for it: off, auto (log2 of "
                      "scene / output, 1 lower with DLSS / FSR as NVIDIA's and AMD's guides say: Quality -1.58, Ultra Performance -2.58) or a number. F2 toggles it with --gow2_ao_debug_keys");
REXCVAR_DEFINE_STRING(gow2_upscale_mode, "native", "Gears2",
                      "Patch 0141 (frame-rate item 5): with --gow2_upscaler, draw the 3D scene below the output size and "
                      "let DLSS / FSR fill in the detail. native = the scene at the render scale (anti-aliasing "
                      "only, as before); performance = half (2x -> 1x, 4x -> 2x); quality = 3x -> 2x; ultra = 3x -> 1x. "
                      "The output size is --gow2_renderer_resolution_scale; a mode with no integer pair there stays native");
REXCVAR_DEFINE_BOOL(gow2_fg_inputs, false, "Gears2",
                    "Patch 0145 (frame-rate item 6, frame generation step 1): make the scene's depth and motion vectors "
                    "every frame even with no upscaler selected (no jitter: the picture is unchanged). Frame generation "
                    "needs them; on its own this adds two small GPU passes and a log line");
REXCVAR_DEFINE_STRING(gow2_ao, "game", "Gears2",
                      "Ambient occlusion (notes/80): game = the game's own (as before), gtao = XeGTAO in its place, off "
                      "= none. Work in progress: gtao only tracks the camera every frame so far (no jitter, no extra "
                      "passes, the picture is unchanged)");
REXCVAR_DEFINE_BOOL(gow2_ao_pass_times, false, "Gears2",
                    "Developer: with --gow2_ao_log, also logs the GPU time of each AO pass (not with the draw report, which reads the same timers)");
REXCVAR_DEFINE_INT32(gow2_ao_log, 0, "Gears2",
                     "Notes/80: every N frames, log the XeGTAO camera constants of the frame's scene (0 = off)");
REXCVAR_DEFINE_STRING(gow2_ao_show, "gtao", "Gears2",
                      "With --gow2_ao=gtao (notes/80): the AO the game's composite gets - gtao = XeGTAO, none = no AO (white), "
                      "game = the game's own (XeGTAO still runs, for the debug views). F9 cycles it with --gow2_ao_debug_keys");
REXCVAR_DEFINE_INT32(gow2_ao_skip_game, 3, "Gears2",
                     "Notes/80: while XeGTAO (or no AO) is shown, skip the game's own AO chain, since its result is "
                     "replaced anyway: 1 = its draws, 2 = its copies, 3 = both, 0 = off");
REXCVAR_DEFINE_STRING(gow2_ao_trace_frames, "", "Gears2",
                      "Notes/80 diagnostic: comma list of frames whose full-screen passes (quads / rectangles) are logged with "
                      "their targets, shader hashes, blending and 2D texture addresses");
REXCVAR_DEFINE_BOOL(gow2_ao_everywhere, true, "Gears2",
                    "Notes/80: with XeGTAO, also where the game turns its own AO off (some outdoor areas): the scene is "
                    "multiplied by XeGTAO's AO just before the game's fog pass, as the game's own AO apply does");
REXCVAR_DEFINE_STRING(gow2_ao_fog_ps, "D58B286B84EE971D", "Gears2",
                      "Notes/80: pixel shader hash(es), comma-separated, of the game's fog pass in frames without its AO "
                      "(the anchor for --gow2_ao_everywhere)");
REXCVAR_DEFINE_STRING(gow2_ao_skip_ps, "", "Gears2",
                      "Notes/81: pixel shader hashes, comma-separated, of a game's own AO passes that the chain detection does not "
                      "find (Judgment). Skipped while our AO or no AO is shown, drawn again for the game's own, so F5 and F9 work live");
REXCVAR_DEFINE_STRING(gow2_pcss_trace_frames, "", "Gears2",
                      "Notes/80 s.16 (developer): frames whose draws that sample a depth-format texture (k_24_8 / k_24_8_FLOAT: shadow "
                      "maps, the scene depth) are logged with their targets, state, textures and first pixel constants");
REXCVAR_DEFINE_INT32(gow2_ao_trace_class, -1, "Gears2",
                     "Notes/80 diagnostic: with --gow2_ao_trace_frames, also log every draw whose first colour target has "
                     "this storage class (e.g. 4 = k_16_16, the velocity buffer), not only full-screen passes; -1 = off");
REXCVAR_DEFINE_STRING(gow2_ao_trace_vs, "", "Gears2",
                      "Notes/80 diagnostic: with --gow2_ao_trace_frames, also log these vertex shader float constants of "
                      "each traced draw, a register range 'first-last' (e.g. 0-12: UE3's camera c0-c3 and object rows); "
                      "empty = off");
REXCVAR_DEFINE_INT32(gow2_upscale_capture_raw_class, -1, "Gears2",
                     "Notes/80 diagnostic: during upscaler capture sets (--gow2_upscale_capture_frames), also write exact "
                     "float dumps (.raw: int32 w, h, format, then 2 floats a texel) of the motion vectors and of every "
                     "colour resolve whose source has this storage class (e.g. 4 = k_16_16, Gears 2's velocity buffer); "
                     "-1 = off. Large: ~20 MB a resolve");
REXCVAR_DEFINE_BOOL(gow2_upscale_depth_at_resolve, false, "Gears2",
                    "Notes/80 s.6: the upscalers' depth is copied at the game's scene depth resolve (Gears 2 clears that "
                    "depth before the scene colour resolve - on there by gow2_app.h); false: read at the scene colour "
                    "resolve, as before (Gears 3 measured the same either way, s.9)");
REXCVAR_DEFINE_BOOL(gow2_upscale_character_motion, false, "Gears2",
                    "Notes/80 s.6: DLSS / FSR motion vectors on characters from their own movement (the "
                    "transforms of the game's velocity pass), not the camera's - the camera follows the player, so "
                    "camera-only vectors are wrong on him by the whole camera move. Gears 2's layout (velocity pass "
                    "c5-c12); on there by gow2_app.h");
REXCVAR_DEFINE_BOOL(gow2_upscale_object_motion_address_key, false, "Gears2",
                    "Object motion: also key draws by their vertex / index addresses (the first take; Gears 1 characters never matched)");
REXCVAR_DEFINE_DOUBLE(gow2_upscale_object_motion_match, -1.0, "Gears2",
                      "Notes/80 s.15: how object motion pairs a draw with last frame's. Below 0: the nearest origin of the same "
                      "draw identity (0.4.7). 0 or more: by the object rows - same mesh and vertex shader, rotation within ~14 "
                      "degrees, translation at most this many world units (256 tested in Gears 3, with real depth); Gears 1 keeps "
                      "the origin rule");
REXCVAR_DEFINE_BOOL(gow2_fg_motion_depth_check, false, "Gears2",
                    "0.4.9 (FG character ghosting, Gears 1 / 3 / Judgment, off by default until tested): the camera motion "
                    "of each pixel is checked against last frame's depth; where it lands on another surface but the pixel "
                    "itself held this depth last frame (a character the camera follows), its motion is 0 instead of the "
                    "camera's whole move. Object / character motion passes still overwrite their own pixels. Logged in "
                    "the upscaler log window ('depth check')");
REXCVAR_DEFINE_BOOL(gow2_upscale_object_motion, false, "Gears2",
                    "#147 / #150 (off by default until tested): DLSS / FSR motion vectors per object. Every scene "
                    "draw's own local-to-clip matrix (the camera times its object-to-world rows; Gears 1: its combined "
                    "matrix) is kept by draw identity (vertex / index data, shaders, index count) and matched to last "
                    "frame's; a draw whose identity was seen moving by itself (not as the camera) is drawn again into an "
                    "object-ID target, and its pixels get its own reprojection (last frame's matrix x this frame's "
                    "inverse) where its depth is still the scene's. Rigid only: limbs inside a skinned mesh move with "
                    "the mesh. Logged every log window ('object motion')");
REXCVAR_DEFINE_STRING(gow2_ao_quality, "high", "Gears2",
                      "XeGTAO quality (notes/80): low (1 slice x 2 steps), medium (2 x 2), high (3 x 3), ultra (9 x 3)");
REXCVAR_DEFINE_DOUBLE(gow2_ao_max_screen_radius, 0.325, "Gears2",
                      "XeGTAO: how far its samples reach on screen, as a fraction of the screen height. Close to the "
                      "camera the effect radius covers much of the screen and its samples leave it - the shadows near "
                      "the bottom flickered. 0.325: tuned live in Gears 3 (Flippage, 16 Nov) with --gow2_ao_mip_offset 6.5");
REXCVAR_DEFINE_DOUBLE(gow2_ao_mip_offset, 6.5, "Gears2",
                      "XeGTAO: depth MIP sampling offset (XE_GTAO_DEFAULT_DEPTH_MIP_SAMPLING_OFFSET 3.30). Higher = far "
                      "samples read finer depth levels: steadier under camera jitter, a little slower. 6.5 (tuned live in "
                      "Gears 3, 16 Nov): coarse levels made shadows near the camera flicker as DLSS jitters the view");
REXCVAR_DEFINE_DOUBLE(gow2_ao_radius, 60.0, "Gears2",
                      "XeGTAO effect radius in the game's world units (notes/80; UE3: about 2 cm each)");
REXCVAR_DEFINE_INT32(gow2_ao_denoise, 0, "Gears2",
                     "XeGTAO denoise passes (notes/80 s.4): 1 = sharp, 2 = medium, 3 = soft; 0 = automatic: under DLSS / FSR "
                     "by the scene\'s scale (ultra: 3x or more 1, 2x 2, 1x 3), else 2 at ultra; below ultra one more, up "
                     "to 3. F8 changes it live");
REXCVAR_DEFINE_BOOL(gow2_ao_half, false, "Gears2",
                    "XeGTAO at half resolution (test, Shagg 20 Nov): the main and denoise passes run on a quarter of the "
                    "pixels (the depth pyramid's level 1), then a depth-aware pass brings the result to the scene's size. "
                    "Job 2137b: AO high cost 0.8 ms and ultra 1.5 ms of GPU time at 1440p. Softer; thin things may lose "
                    "contact shadow - compare before using");
REXCVAR_DEFINE_BOOL(gow2_ao_prefilter_single, false, "Gears2",
                    "XeGTAO: build the depth pyramid's 5 levels in one dispatch (XeGTAO's PrefilterDepths16x16) instead of "
                    "five, each reading the whole scene depth again. Needs a backend that binds 5 UAVs (D3D12, Vulkan); "
                    "otherwise the five dispatches are used. false (default) = five dispatches, as 0.4.5");
REXCVAR_DEFINE_STRING(gow2_ao_test_script, "", "Gears2",
                      "Tests (Shagg 20 Nov: one launch, not one per setting): timed XeGTAO changes while the game runs - "
                      "\"<swap>:<key>=<value>,...\" applied at that finished-frame number, each logged as 'gow2_ao_test:'. "
                      "Keys: quality=low|medium|high|ultra, denoise=0..3 (0 = automatic), "
                      "show=ours|none|game, half=0|1, single=0|1 (depth pyramid in one dispatch), mip=<offset>, view=0..5. Needs "
                      "--gow2_ao=gtao; with a script the half-size and one-dispatch shaders are built at start too");
REXCVAR_DEFINE_INT32(gow2_ao_gpu_log, 0, "Gears2",
                     "XeGTAO cost: every N frames, the average GPU time of each XeGTAO pass (depth pyramid levels 0-4, main, "
                     "each denoise pass) from the backend's GPU timestamps. 0 = off. Not while --gow2_renderer_draw_report_frame "
                     "runs (it owns the GPU tags); needs --gow2_renderer_gpu_timestamp_split (the default)");
REXCVAR_DEFINE_DOUBLE(gow2_ao_power, 2.2, "Gears2", "XeGTAO final value power - higher is darker (XeGTAO's default 2.2)");
// Screen-space indirect lighting (ssil_shaders.h, a prototype): one diffuse bounce of the frame's own scene colour,
// added at the scene colour resolve. Needs --gow2_ao=gtao (XeGTAO's view-depth pyramid). The scene copy is the one
// the upscaler takes (--gow2_upscale_scene_formats): Gears 2's 16-bit float HDR or Gears 3's 10:10:10:2 UNORM.
REXCVAR_DEFINE_BOOL(gow2_ssil, false, "Gears2",
                    "Prototype: screen-space indirect lighting (SSILVB, one bounce) added to the scene before bloom / tone "
                    "map / the upscaler. Needs --gow2_ao=gtao. With --gow2_ao_debug_keys: Insert on / off, Shift+Insert view, "
                    "Ctrl+Insert strength, Alt+Insert albedo estimate");
REXCVAR_DEFINE_DOUBLE(gow2_ssil_strength, 1.5, "Gears2", "SSIL: how much of the indirect light is added (Ctrl+Insert cycles it)");
REXCVAR_DEFINE_DOUBLE(gow2_ssil_radius, 200.0, "Gears2", "SSIL: how far light bounces from, in world units (UE3: about 2 cm each)");
REXCVAR_DEFINE_DOUBLE(gow2_ssil_thickness, 30.0, "Gears2",
                      "SSIL: how thick each depth sample is assumed to be (world units) - light passes behind thinner things");
REXCVAR_DEFINE_DOUBLE(gow2_ssil_fade, 6000.0, "Gears2", "SSIL: view distance (world units) where the effect has faded out");
REXCVAR_DEFINE_INT32(gow2_ssil_slices, 3, "Gears2", "SSIL: directions per pixel (1-8)");
REXCVAR_DEFINE_INT32(gow2_ssil_steps, 8, "Gears2", "SSIL: samples per direction and side (2-16)");
REXCVAR_DEFINE_INT32(gow2_ssil_albedo, 3, "Gears2",
                     "SSIL: the albedo estimate of what the light lands on: 0 = the scene colour's hue at 50%, 1 = grey 50%, "
                     "2 = the scene colour itself, 3 = local: the scene colour over the brightness around it (Alt+Insert switches "
                     "between 2 and 3)");
REXCVAR_DEFINE_INT32(gow2_ssil_log, 600, "Gears2", "SSIL: every N frames, how many frames it ran on (0 = off)");
REXCVAR_DEFINE_BOOL(gow2_ssil_backface, true, "Gears2",
                    "SSIL: a surface seen from behind blocks light but sends none (normals from depth; Home toggles it)");
REXCVAR_DEFINE_BOOL(gow2_ssil_difference, false, "Gears2",
                    "SSIL: add only the local part - the indirect light minus 80% of its wide blur, so the even bounce the "
                    "game's baked lighting already has is not added again (Shift+Home toggles it)");
REXCVAR_DEFINE_BOOL(gow2_ssil_temporal, true, "Gears2",
                    "SSIL: blend each frame into the last (reprojected with the upscaler's motion vectors) - only while "
                    "DLSS / FSR / SMAA T2x runs (Ctrl+Home toggles it)");
REXCVAR_DEFINE_BOOL(gow2_ssil_bounces, true, "Gears2",
                    "SSIL: last frame's indirect light bounces again (several bounces over frames; needs --gow2_ssil_temporal "
                    "running; Alt+Home toggles it)");
REXCVAR_DEFINE_BOOL(gow2_ssil_adaptive, true, "Gears2",
                    "SSIL: the radius, thickness, fade and steps follow the size of the space in view - the median view "
                    "distance, smoothed over about half a second - from the --gow2_ssil_radius / _thickness / _fade / "
                    "_steps values (a room) to the --gow2_ssil_far_* ones (open ground). End toggles it");
REXCVAR_DEFINE_DOUBLE(gow2_ssil_far_radius, 600.0, "Gears2", "SSIL adaptive: the radius in open spaces (world units)");
REXCVAR_DEFINE_DOUBLE(gow2_ssil_far_thickness, 60.0, "Gears2", "SSIL adaptive: the thickness in open spaces (world units)");
REXCVAR_DEFINE_DOUBLE(gow2_ssil_far_fade, 15000.0, "Gears2", "SSIL adaptive: the fade-out distance in open spaces");
REXCVAR_DEFINE_INT32(gow2_ssil_far_steps, 12, "Gears2", "SSIL adaptive: samples per direction and side in open spaces");
REXCVAR_DEFINE_DOUBLE(gow2_ssil_adapt_near, 800.0, "Gears2",
                      "SSIL adaptive: the median view distance (world units) at and below which the room values apply "
                      "(measured: Gears 2 ch1 hospital 470-760, ch3 open ground 2600-5300, in cover outdoors 800-1000)");
REXCVAR_DEFINE_DOUBLE(gow2_ssil_adapt_far, 2500.0, "Gears2",
                      "SSIL adaptive: the median view distance at and above which the open-space values apply (between: "
                      "in proportion, on a log scale)");
REXCVAR_DEFINE_INT32(gow2_ssil_view, 0, "Gears2", "SSIL: the starting view - 0 normal, 1 indirect light only, 2 split (left without, right with); Shift+Insert cycles it");
REXCVAR_DEFINE_BOOL(gow2_ao_add_at_scene_copy, false, "Gears2",
                    "Where the game draws no AO and has no fog pass to anchor ours (Gears 1): XeGTAO runs on the "
                    "scene depth and is multiplied into the scene at the frame's first full-screen scene copy (Gears 1: "
                    "before its per-light passes). With --gow2_ao=gtao and --gow2_ao_everywhere; gow1.exe sets true");
REXCVAR_DEFINE_DOUBLE(gow2_ao_add_radius, -1.0, "Gears2",
                      "Notes/80 s.9: XeGTAO's radius where the game draws no AO of its own and ours is added "
                      "(--gow2_ao_everywhere; Gears 3 bakes its AO there - a gentler complement); below 0 = --gow2_ao_radius");
REXCVAR_DEFINE_DOUBLE(gow2_ao_add_power, -1.0, "Gears2",
                      "Notes/80 s.9: XeGTAO's power where ours is added (see --gow2_ao_add_radius); below 0 = --gow2_ao_power");
REXCVAR_DEFINE_INT32(gow2_ao_debug, 0, "Gears2",
                     "Notes/80 developer view: 1 = XeGTAO's result only, 2 = split (the game's picture left, XeGTAO "
                     "right), drawn over the finished picture; 3 = view depth (/ 2000), 4 = the raw AO term, 5 = edges; "
                     "0 = off");
REXCVAR_DEFINE_INT32(gow2_upscale_jitter_sign, 1, "Gears2",
                     "Diagnostic: 1 or -1 - the sign of the jitter handed to the upscaler (the image shift itself is "
                     "unchanged), for checking against the DLSS SDK's jitter overlay");
REXCVAR_DEFINE_STRING(gow2_upscale_scene_guest_formats, "", "Gears2",
                      "Preferred guest colour formats of the scene copy (comma-separated numbers; empty = off). Gears 3 "
                      "copies its float scene (format 10) for translucency mid-frame - a different number of times each "
                      "frame - and its 10:10:10:2 UNORM scene (format 2) once at the end; on the host both are R10G10B10A2. "
                      "While frames have a preferred copy after the camera, the first one is taken (decided within the "
                      "frame, nothing counted); a frame after one without takes the frame's last scene copy of any format "
                      "(chapter 12's later part ends on the float scene). Gears 3 sets 2");
REXCVAR_DEFINE_INT32(gow2_upscale_camera_handedness, 0, "Gears2",
                     "The sign of the real camera's c0-c3 determinant (+1 / -1; 0 = not checked). A camera of the other sign "
                     "is a mirror - a planar reflection drawn into the scene depth before the view - and is never taken as "
                     "the frame's camera. Gears 3 chapter 1 (the submarine): its reflection passes the near-plane test on "
                     "some frames, so the upscaler counted the reflection's copy as a scene copy on those frames only");
REXCVAR_DEFINE_BOOL(gow2_upscale_depth_last_copy, false, "Gears2",
                    "With --gow2_upscale_depth_at_resolve: the upscaler keeps the frame's LAST scene-depth copy, not the "
                    "first. Gears 3 and Judgment copy their scene depth out twice in some frames, and the first copy holds "
                    "only part of the scene (as XeGTAO found, PR #336). Off: the first copy, as before (Gears 2); Judgment and "
                    "Gears 3 turn it on");
REXCVAR_DEFINE_BOOL(gow2_upscale_capture_key, false, "Gears2",
                    "Developer (job 724): F9 saves 6 frames of upscaler input / output / motion / depth next to the log. "
                    "Off in releases (Shagg: developer tools never ship)");
REXCVAR_DEFINE_STRING(gow2_upscale_capture_frames, "", "Gears2",
                      "Job 727 (DLSS / FSR comparison runs): frames at which to save 6 frames of upscaler input / output / "
                      "motion / depth next to the log, as F9 does, e.g. '2000,2600'");
REXCVAR_DEFINE_INT32(gow2_upscale_jitter_sign_y, 1, "Gears2",
                     "Job 723 (Gears 1 flicker): 1 or -1 - the sign of only the vertical jitter handed to the upscaler, on "
                     "top of --gow2_upscale_jitter_sign (the image shift itself is unchanged)");
REXCVAR_DEFINE_DOUBLE(gow2_upscale_jitter_scale, 1.0, "Gears2",
                      "Diagnostic (Gears 3 chat, job 058): multiplies the sub-pixel camera jitter, both the image shift and "
                      "what the upscaler is told (0 = no jitter; 1 = normal)");
REXCVAR_DEFINE_BOOL(gow2_upscale_camera_move, true, "Gears2",
                    "Motion vectors include the camera's movement, read off the static objects drawn (UE3 renders "
                    "camera-relative, so its camera matrix only turns; notes/64). false: turning only, for comparison");
// From the Gears 3 chat (its commit 80d9b9e, job 056): which vertex constants carry the object rows / the camera
// position. 0 = Gears 2's behaviour (objects c5-c8, no camera position); gow3.exe sets 7 and 5.
REXCVAR_DEFINE_INT32(gow2_upscale_object_constants, -1, "Gears2",
                     "Camera move for DLSS / FSR: the first of the four vertex constants holding an object's "
                     "object-to-world rows (translation in the fourth). -1 = the game's (Gears 2: c5; Gears 3 sets c7; "
                     "Gears 1 sets c0)");
REXCVAR_DEFINE_INT32(gow2_upscale_camera_matrix, 0, "Gears2",
                     "DLSS / FSR (Gears 1 job 715): the first of the four vertex constants holding the camera's "
                     "view-projection. 0 = c0 (Gears 2, 3, Judgment); -1 = found on the frame's first scene draw (the "
                     "first four constants it reads that make a perspective matrix, outside the object rows) - Gears 1, "
                     "whose c0-c3 are each object's own object-to-world rows");
REXCVAR_DEFINE_INT32(gow2_upscale_camera_constant, 0, "Gears2",
                     "Camera move for DLSS / FSR: a vertex constant holding the camera's world position on every "
                     "scene draw (its change = the move; the sign is learned from the object vote). 0 = none (Gears 3 sets c5)");
REXCVAR_DEFINE_BOOL(gow2_upscale_dejitter_depth, false, "Gears2",
                    "Upscalers (Gears 3 job 058): the frame's scene depth resolve is written at the unjittered pixel "
                    "centres (bilinear, nearest across edges), so the game's passes that rebuild positions from the "
                    "resolved depth (UE3 shadow projection, fog, AO, DOF) match. Without it, close-up character "
                    "self-shadows flip whole triangles dark. The upscaler's own depth input stays jittered");
REXCVAR_DEFINE_BOOL(gow2_upscale_passthrough, false, "Gears2",
                    "Diagnostic (Gears 3 job 062): everything as with the upscaler (jitter, motion vectors, depth), but its "
                    "Evaluate is skipped and the jittered scene goes on unchanged - shows whether an artefact is already in "
                    "the scene the game draws under jitter, or comes from the upscaler's reconstruction");
REXCVAR_DEFINE_BOOL(gow2_upscale_resize_recreate, false, "Gears2",
                    "Diagnostic (GitHub #323): the old way - DLSS / FSR re-created for every scene size (Gears 2 "
                    "narrows its scene in heavy views, many times a second; ~20 ms each). Off: created once for the "
                    "largest size, the frame handed over within it, history kept");
REXCVAR_DEFINE_BOOL(gow2_upscale_resize_reset, false, "Gears2",
                    "Diagnostic (GitHub #323): throw the upscaler's history away on a frame whose scene size differs from "
                    "the last one (as the old re-creation did). Off: the history is kept, motion re-expressed for the "
                    "last frame's size (DLSS / FSR; FSR without the padding shader resets on such a frame either "
                    "way)");
REXCVAR_DEFINE_INT32(gow2_upscale_jitter_scope, 0, "Gears2",
                     "Diagnostic (Gears 3 job 059): which perspective draws into the scene depth get the upscalers' jitter "
                     "besides the camera draws (same c0-c3 as the frame's first depth-writing scene draw): 0 = none, 1 = also "
                     "draws whose vertex shader does not read c0-c3, 2 = also draws with other c0-c3, 3 = as 2, and the scene "
                     "depth is any depth target at the same EDRAM base, scale and MSAA (Gears 3 job 060: ch8 draws into "
                     "D24FS8 pitch 1280 and D24S8 pitch 1360 at base 0)");
REXCVAR_DEFINE_BOOL(gow2_upscale_jitter_late, false, "Gears2",
                    "Diagnostic (Gears 3 job 120): also jitter the camera draws into the scene depth that come AFTER the "
                    "scene colour resolve that ran the upscaler. Gears 3 ch8 cutscenes: whole character triangles lose "
                    "their lighting under DLSS / FSR; [G] later lighting passes drawn unjittered fail the depth-equal test "
                    "against the jittered depth. The count is always logged ('camera draws after the upscale')");
REXCVAR_DEFINE_INT32(gow2_upscale_scene_formats, 1, "Gears2",
                     "Which colour target formats count as the scene for DLSS / FSR: 1 = 16-bit float (Gears 2's lit "
                     "scene), 2 = 10:10:10:2 UNORM (Gears 3's), 3 = both. Gears 2 keeps 1: in a ch05 cutscene a "
                     "full-screen 10:10:10:2 target came first and was taken instead of the scene (job 276); gow3.exe sets 2");
REXCVAR_DEFINE_BOOL(gow2_upscale_scene_last_copy, false, "Gears2",
                    "Notes/80 s.11: DLSS / FSR / T2x take the frame's last full-screen scene copy, at the position "
                    "learned from the frame before, not the first. Judgment copies its scene mid-frame for translucency "
                    "before the finished copy: the first was upscaled ~480 draws early (gowj.exe sets true)");
REXCVAR_DEFINE_INT32(gow2_upscale_probe, 0, "Gears2",
                     "Upscaler groundwork (notes/64): every N frames, log the vertex shaders' c0-c3 (the camera matrix "
                     "candidate) grouped by value for the draws of that frame, with their depth targets and whether they "
                     "are perspective / write depth / read c0-c3, and every resolve of the frame (0 = off)");
REXCVAR_DEFINE_BOOL(gow2_renderer_compact_constants, true, "Gears2",
                    "Default on since job 252 (job 251: ch10 capped with it on byte-identical to the run without it, all 8 "
                    "shared shots; uncapped ch10 13.18 -> 12.51 ms, constants 0.78 -> 0.33 ms/frame, job 250). "
                    "Shaders that read a few low float constants plus the literal block at c252-c255 (and nothing "
                    "relatively) read a compacted constant buffer, and each draw uploads only those two pieces instead "
                    "of all 256 registers (ch10 profile, job 249). Changes the generated HLSL of those shaders, so they "
                    "get new shader-cache files");
REXCVAR_DEFINE_BOOL(gow2_renderer_vertex_direct, true, "Gears2",
                    "Small vertex ranges are read by the shaders straight from the upload ring (no copy into the guest "
                    "mirror and no barrier per draw; job 160). false: copied into the mirror as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_texture_mismatch_zero, true, "Gears2",
                    "A fetch whose dimension doesn't match the texture reads zero, like Canary's null view (false: white)");
REXCVAR_DEFINE_INT32(gow2_renderer_scaled_window_align_mb, 16, "Gears2",
                     "Jobs 235-239: alignment (MB) of a new 32 MB scaled-memory window's base (2K / 4K only), default 16 "
                     "(user 12:14; shots identical in ch02/05/10/18 - job 237's one ch05 difference did not repeat, job 238; "
                     "ch18 4K 18.55 -> 15.11 ms). 1 = job 191's rule (where a "
                     "resolve first needs one); 16 = windows only at 16 MB steps, so an address sits in at most 2 windows "
                     "and each scaled resolve is written at most twice (ch18 at 4K: 4 windows, resolves 9.4 ms); 32 = a 32 MB "
                     "step first (no overlap), then 16, 8, ... for an extent crossing a 32 MB boundary");
REXCVAR_DEFINE_INT32(gow2_renderer_fused_scaled_kinds, 3, "Gears2",
                     "Job 239 diagnostic: which resolves the fused scaled resolve takes - 1 colour, 2 depth, 3 both "
                     "(ch05 at 4K differed slightly everywhere with both, job 238)");
REXCVAR_DEFINE_BOOL(gow2_renderer_const_gen_skip, true, "Gears2",
                    "Job 240 (default on since job 249; shots identical in ch10 / ch02): skip the float-constant compare and copy for a draw when no register write reached the "
                    "VS / PS float constants since that slot's last upload (render-pipe generation counters)");
REXCVAR_DEFINE_BOOL(gow2_renderer_fused_scaled_resolve, false, "Gears2",
                    "Job 238: the resolve shaders' scaled pass writes straight into the scaled-memory windows (one pass "
                    "per window), skipping the scaled destination texture and the copy from it. Front-buffer copies keep "
                    "the old path (the presenter reads the scaled destination)");
REXCVAR_DEFINE_INT32(gow2_renderer_fused_scaled_check, 0, "Gears2",
                     "fuse049 diagnostic (with --gow2_renderer_fused_scaled_resolve): every Nth frame, each fused scaled "
                     "resolve also runs the old path into a scaled destination and the GPU compares its words with the "
                     "window's (log: 'fused check'). 0 = off. Up to 400 lines a session");
REXCVAR_DEFINE_BOOL(gow2_renderer_gpu_timestamp_split, true, "Gears2",
                    "Patch 0109 (both APIs, a measurement switch): false = GPU timestamps only at the start and end of "
                    "each command list - the whole-frame GPU time stays, the split by kind of work / target / draw goes "
                    "(the GPU line shows it all as 'draws'). Each Vulkan timestamp waits for all earlier work "
                    "(ALL_COMMANDS); this shows whether the ~100+ a frame change Vulkan's or D3D12's GPU time");
REXCVAR_DEFINE_BOOL(gow2_renderer_skip_resolve_dispatches, false, "Gears2",
                    "Patch 0117 (both APIs, a MEASUREMENT switch - wrong pictures): the renderer's compute steps for "
                    "resolves and EDRAM copies to guest memory are not sent (present and upscaler compute stay). Shows "
                    "how much of each API's frame those steps and the render-pass breaks / barriers around them cost");
REXCVAR_DEFINE_BOOL(gow2_renderer_t2_measure_skip, false, "Gears2",
                    "#257 / #296 step 1 (both APIs, a MEASUREMENT switch - pictures may be wrong): our resolves are not "
                    "written into the GPU guest-memory mirror (T2). A resolve-through texture is still written by the same "
                    "shader without the mirror stores; other T2 dispatches are skipped. Pages stay marked as ours. Shows "
                    "what T2 and the guest-buffer transitions around it cost (log: 'T2 measure-skip')");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_draws, false, "Gears2",
                    "Patch 0119 (both APIs, test): the colour resolve step for single-sample sources into 32 bpp "
                    "destinations runs as a full-screen draw into the destination (a render target) instead of a "
                    "compute dispatch (jobs 427 / 428: on Vulkan a dispatch that reads the target just drawn costs "
                    "~16-23 us more than the same step as a draw). 64 / 8 / 16 / 128 bpp, MSAA sources, fused scaled "
                    "resolves and the copy into guest memory stay dispatches");
REXCVAR_DEFINE_BOOL(gow2_renderer_gpu_by_target, false, "Gears2",
                    "Job 234 diagnostic: GPU draw time per render target, logged with the CPU split (one more GPU "
                    "timestamp whenever the draws move to another target)");
REXCVAR_DEFINE_BOOL(gow2_renderer_packed_tail_at_base, true, "Gears2",
                    "Patch 0087 (Canary 344fca7): a small texture whose level 0 is already the packed mip tail (short side "
                    "16 or less) has its whole tail at the base address - also when the fetch constant's mip address is 0 "
                    "(we used to load level 0 only, so distant mips read level 0). false = the old way");
REXCVAR_DEFINE_STRING(gow2_renderer_texture_probe, "", "Gears2",
                      "Job 284 diagnostic: comma list of guest texture base addresses (hex). Each lookup of those textures "
                      "in --gow2_renderer_texture_probe_frames is logged once per frame (what the cache did, last load, "
                      "hash of the guest bytes now and at that load) and the bytes saved to --gow2_renderer_texture_probe_dir");
REXCVAR_DEFINE_STRING(gow2_renderer_texture_probe_frames, "", "Gears2", "Job 284: frame window first-last for the probe");
REXCVAR_DEFINE_STRING(gow2_renderer_texture_probe_dir, "", "Gears2", "Job 284: folder for the probe's byte dumps (empty = none)");
REXCVAR_DEFINE_INT32(gow2_renderer_texture_cache_mb, 512, "Gears2",
                     "Shadow renderer: host-memory cap for guest textures in MB; past it, textures unused for two "
                     "frames are evicted least recently used first (0 = no cap) (upl-n108)");
REXCVAR_DEFINE_BOOL(gow2_renderer_static_index_buffers, true, "Gears2",
                    "Shadow renderer: keep cached (unchanged) guest index buffers in persistent GPU buffers instead of "
                    "copying them into the upload ring on every draw (upl-n108)");
REXCVAR_DEFINE_BOOL(gow2_renderer_index_buffers_vram, true, "Gears2",
                    "Patch 0150 (D3D12): the persistent index buffers of --gow2_renderer_static_index_buffers are copied "
                    "once into GPU memory instead of being read from an upload heap (over the bus) at every draw. Patch 0152: "
                    "default on (jobs 456/457b: neutral to +3% fps, picture checked; Shagg 09:03)");
REXCVAR_DEFINE_BOOL(gow2_renderer_clamp_next_base, true, "Gears2",
                    "Rect ownership rule: a target's claim also ends at the next bound target's EDRAM base (as the "
                    "reference does)");
REXCVAR_DEFINE_INT32(gow2_shader_threads, 0, "Gears2",
                     "Shader compile threads for our renderer (0 = logical processors minus 2, at least 1)");
// Stage 4 (upl-s4c, notes/46 section 4): our own occlusion counts. Needs SDK patch 15 (DrawObserver::OnZpd).
REXCVAR_DEFINE_STRING(gow2_renderer_queries, "off", "Gears2",
                      "Occlusion queries: off | shadow (run our host queries and log our counts, write nothing) | "
                      "strict (PR #43: each end report waits for that query's own GPU result; needs our GPU system and "
                      "--gow2_pm4_split; the launcher's \"Real occlusion (experimental)\", D3D12) | "
                      "answer (also write our answers into the game's query reports: the last real count for that "
                      "report, 1 if none; corrected when our count lands)");
REXCVAR_DEFINE_BOOL(gow2_upscale_scene_after_fog, false, "Gears2",
                    "With --gow2_upscale_scene_last_copy: the scene copy is one made after the fog pass (--gow2_ao_fog_ps) "
                    "once a frame has drawn it - Judgment adds a mid-frame scene copy before its fog on some frames, which "
                    "moved the taken copy before the fog (XeGTAO's AO left out of that frame: a one-frame flash)");
REXCVAR_DEFINE_DOUBLE(gow2_renderer_occlusion_slope_bias, 1.0, "Gears2",
                      "Occlusion boxes (query draws that write nothing) get this slope-scaled depth bias toward passing. "
                      "Gears 3 tests them against a half-size depth that keeps the nearest of each 2x2: a flat object's box, "
                      "lying on that object at a shallow angle, lost to its own surface by under a pixel row's depth, was "
                      "hidden, then shown again - every other frame (ground patches flickering). 0 = off");
REXCVAR_DEFINE_INT32(gow2_renderer_queries_min, 0, "Gears2",
                     "Smallest immediate occlusion answer (0 keeps real zeros; 1 = nothing is culled until our count for "
                     "that query has landed, as Canary's fast mode)");
REXCVAR_DEFINE_INT32(gow2_renderer_queries_hidden_after, 3, "Gears2",
                     "Answer an occlusion query 'hidden' only after this many real zero counts in a row for it (1 = the "
                     "first zero, as before job 139; a wrong 'hidden' makes an object vanish for a frame)");
REXCVAR_DEFINE_BOOL(gow2_renderer_queries_correct, true, "Gears2",
                    "Rewrite a query report with our real count when it lands, if the report still holds our estimate");
REXCVAR_DEFINE_INT32(gow2_renderer_queries_idle_submits, 0, "Gears2",
                     "Strict queries: the most extra GPU submissions per frame made while the render thread is idle with "
                     "a closed query batch (0 = no limit, as 0.6.0). Every submission ends the open command list, which "
                     "resets the per-list reuse of vertex, index, constant and table data (0.6.1 review P1). "
                     "Diagnostic: once the limit is reached a waiting query is only answered at the frame's own "
                     "submit, or 'visible' after 2 s");
// Takeover step 1 (notes/50): show OUR image. At each swap, our resolve of the front buffer (the colour copy whose
// destination is the swap's front buffer) is written into guest memory in the guest's layout and the SDK is told the
// range changed, so the emulator presents our pixels. The emulator still draws (its result is overwritten).
REXCVAR_DEFINE_STRING(gow2_renderer_display, "emulator", "Gears2",
                      "Which image the window shows: emulator (default) | ours (our renderer's front-buffer resolve)");
REXCVAR_DEFINE_BOOL(gow2_renderer_display_wait, false, "Gears2",
                    "With --gow2_renderer_display=ours: wait at each swap for our GPU to finish that frame (no lag, "
                    "slower). Off: show our newest finished frame (1-2 swaps behind, no stall)");
REXCVAR_DEFINE_BOOL(gow2_renderer_display_mark, false, "Gears2",
                    "With --gow2_renderer_display=ours: paint a 32x32 magenta square in the top-left corner of our image "
                    "(proof on screenshots that the window shows our pixels)");
// Stage 5C-1 (notes/49 section 10): our swap gamma pass, checked against the SDK's presented image. Nothing on screen
// changes.
REXCVAR_DEFINE_DOUBLE(gow2_hdr_paper_white, 203.0, "Gears2",
                      "Stage 7.3 (--present_hdr): nits of the game's full white (BT.2408 reference white 203)");
REXCVAR_DEFINE_DOUBLE(gow2_hdr_ui_white, 203.0, "Gears2", "Native HDR: independent HUD and reload white in nits");
REXCVAR_DEFINE_BOOL(gowj_hdr_native, false, "Judgment",
                    "Opt in to experimental native HDR grading and post-scene FXAA");
REXCVAR_DEFINE_BOOL(gow1_hdr_native, false, "Gears1",
                    "Opt in to experimental native HDR grading and motion blur");
REXCVAR_DEFINE_BOOL(
    gow2_hdr_compact_targets, false, "Gears2",
    "Opt in to visible-height HDR attachments on backends supporting mixed output sizes");
REXCVAR_DEFINE_BOOL(
    gowj_hdr_single_pass, false, "Judgment", "Pair captured HDR/SDR grading and FXAA outputs; false selects replay for comparison");
REXCVAR_DEFINE_BOOL(gow2_hdr_scene_effects, false, "Gears2",
                    "Experimental selective HDR blur: one lazy versioned input, 128 MiB cap; "
                    "requires gameplay validation");
REXCVAR_DEFINE_BOOL(gow2_hdr_ui_automatic, false, "Gears2",
                    "Additional bounded screen-space UI in native HDR; direct cached targets, "
                    "asynchronous variants; false selects upstream behavior");
REXCVAR_DEFINE_BOOL(gow2_hdr_native, true, "Gears2", "Use the captured Gears 2 / Gears 3 TU6 float scene/HUD profiles in HDR10");
REXCVAR_DEFINE_DOUBLE(gow2_hdr_peak, 1000.0, "Gears2", "Native HDR: display peak in nits for highlight roll-off");
// 0.6.2: how much the Gears 2 / Gears 3 native HDR scene (PsychoV35, PR #294) boosts highlights above SDR white. 1 =
// the tuned look (RenoDX's smoothed-power boost); 0 = no extra boost (the mapper alone); up to 2.
REXCVAR_DEFINE_DOUBLE(gow2_hdr_highlights, 1.0, "Gears2",
                      "Native HDR (Gears 2 / Gears 3): highlight boost strength - 0 none, 0.5 subtle, 1 default, 1.5 strong");
REXCVAR_DEFINE_DOUBLE(gow2_hdr_knee, 0.75, "Gears2",
                      "Stage 7.3 HDR (games without native HDR profiles, or --gow2_hdr_native=false); unused by native HDR");
REXCVAR_DEFINE_DOUBLE(gow2_hdr_expand, 1.0, "Gears2",
                      "Stage 7.3 HDR (games without native HDR profiles, or --gow2_hdr_native=false); unused by native HDR");
REXCVAR_DEFINE_BOOL(gow2_present_frame_aspect, false, "Gears2",
                    "Stage 7.5: show the front buffer at its own shape (square pixels) instead of the video mode's 16:9 - "
                    "set by --gow2_ultrawide, whose front buffer is 1280x550");
REXCVAR_DEFINE_STRING(gow2_present_aa, "off", "Gears2",
                      "Stage 7.2: anti-aliasing of the presented image - off, or smaa (SMAA 1x, preset high, luma edges, "
                      "after the swap's gamma, before the window; HUD included)");
REXCVAR_DEFINE_STRING(gow2_present, "off", "Gears2",
                      "Stage 5C: off | shadow - at each swap run the SDK's gamma pass ourselves on our front buffer and "
                      "compare the result with the SDK's presented image | ours - our renderer runs on the presenter's "
                      "D3D12 device and queue and writes its gamma image into the presenter itself; XE_SWAP becomes "
                      "ours and the front buffer is no longer written to guest memory (both need "
                      "--gow2_renderer_display=ours and --gow2_pm4_execute for the gamma ramps; ours needs SDK patch "
                      "23)");
// Takeover T2 (notes/50): our resolves feed the game's later texture reads. Each colour/depth resolve we do is also
// written, tiled and endian-swapped, into our GPU's guest-memory mirror, and those pages are marked as ours: our texture
// loads read them instead of the emulator's bytes until the CPU writes there again. Resolves we can't do yet (MSAA
// colour, unmodelled formats) still come from the emulator (needs --readback_resolve=full).
REXCVAR_DEFINE_BOOL(gow2_renderer_resolves_to_guest, false, "Gears2",
                    "Our resolve results replace the emulator's in the memory our texture loads read (takeover T2)");
// Canary's readback_resolve (patch 0020, Shagg 07:22 "do it the Canary way"; xenia-canary e6cd2e4bc
// d3d12_command_processor.cc IssueCopy_ReadbackResolvePath, BSD-3-Clause, ideas and behaviour ported, our code): each of
// our T2 resolves is also copied from the GPU mirror back into guest CPU memory, so CPU-path texture loads and pages the
// CPU touches later see the resolve's bytes (Gears 3 job 087 Z1 [V]: the ch22 black squad needs resolves in CPU memory;
// our 301 O vs 307 R [V]: the ch10 flash only happens when textures read our resolves).
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_readback_first_wait, false, "Gears2",
                    "Fast resolve read-back (Judgment): the first copy of every destination waits for the GPU, as "
                    "Canary's cache miss does. Off (default): only one-tile ones (32 x 32 pixels or less, such as a value "
                    "the game reads back), at most two a frame, wait; a larger one lands a frame or two later at a swap, like every later copy - moving between "
                    "areas of a map (Blood Drive) made many first copies, each a full GPU wait");
REXCVAR_DEFINE_STRING(gow2_renderer_resolve_readback, "auto", "Gears2",
                      "Patch 0020 (Canary readback_resolve, T2 only): auto (default: fast in a game that reads its "
                      "exposure back - Judgment - and none in the others), none, fast (Canary's default: "
                      "each resolve lands the previous copy of the same destination in guest memory, no stall; the "
                      "first one waits) or full (wait for the GPU at every resolve: exact, slow). Turns on "
                      "--gow2_renderer_resolve_prepare_mirror");
REXCVAR_DEFINE_INT32(gow2_renderer_resolve_readback_max_kb, -1, "Gears2",
                     "0.6.1: read back (and prepare the mirror for) only resolves whose destination extent is at most "
                     "this many KB; 0 = every resolve; -1 (default) = the game's own: 256 where the read-back is on "
                     "for the game's exposure (Judgment reads small luminance tables, 4 - 64 KB; reading every "
                     "full-screen resolve back cost 6 ms of CPU a frame, job 1695), 0 otherwise");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_prepare_mirror, false, "Gears2",
                    "Patch 0020 (Canary SharedMemory::RequestRange before a resolve writes): before our resolve writes "
                    "the GPU mirror, its destination pages that the CPU wrote since the mirror last had them get the "
                    "CPU bytes first, and the range is watched (Canary MakeRangeValid), so the bytes of those pages the "
                    "resolve does not cover are current when the pages become GPU-current");
REXCVAR_DEFINE_INT32(gow2_renderer_log_aliases, 20, "Gears2",
                     "Log at most this many distinct EDRAM alias pairs (a depth target reusing tiles another target "
                     "wrote last)");
REXCVAR_DEFINE_BOOL(gow2_renderer_query_box_fast, false, "Gears2",
                    "Speed study (O6, layer0 sweep 22 Nov): an occlusion-query box (a non-indexed depth-test-only draw "
                    "inside an open query, no colour, no pixel shader, no textures) that directly follows another with "
                    "the same state registers and shaders is issued again with that box's pipeline, depth target and "
                    "viewport, skipping the lookups and ownership work (same GPU commands; the query counts must not "
                    "change). Off until checked on the PC: query counts and screenshots on D3D12 and Vulkan");
REXCVAR_DEFINE_BOOL(gow2_present_gamma_to_smaa_vulkan, true, "Gears2",
                    "Speed study (O7): --gow2_present_gamma_to_smaa on Vulkan too (the same shader variant; same values "
                    "as the unpack pass it replaces)");
REXCVAR_DEFINE_BOOL(gow2_present_fold_unpack, true, "Gears2",
                    "Speed study (O7, D3D12): the shown frame's last present pass (gamma, resample or sharpening) also "
                    "writes the presenter's image, so the full-screen pass that unpacked its words into it is left out "
                    "(bit-identical: the same n / 1023 values). Not with HDR output or frame generation");
REXCVAR_DEFINE_BOOL(gow2_renderer_draw_trims, true, "Gears2",
                    "Speed study (O9, layer0 sweep 22 Nov): per-draw trims with the same results - the fog-anchor shader "
                    "list parsed once (no text search per draw), the index-reuse table as a flat table cleared by "
                    "generation, and a small filter in front of the recipe pack's drawn-shader set");
