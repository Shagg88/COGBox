// Layer 0 (game-free): our renderer in SHADOW mode (notes/42 section 1; steps 1-2 of notes/42 section 4; milestone M1
// of notes/36: depth prepass + shadow maps). Behind --gow2_renderer=shadow, off by default. The draw observer feeds it
// on the GPU worker thread; it draws every draw that writes depth or stencil into its own targets on its own D3D12
// device, resolves depth region-to-region into host textures in the guest format, and on chosen frames compares each
// depth resolve byte for byte with what the emulator wrote to guest memory (needs --readback_resolve=full). The
// emulator keeps rendering and presenting: nothing the player sees changes. notes/44 describes the log lines.
//
// M1 scope: colour is stubbed (colour targets are tracked for EDRAM tile ownership but not drawn; draws that write only
// colour are counted and skipped), textures are stubbed (every slot reads opaque white), tessellated patches are
// skipped and counted. Clears (the D3D library's fixed shader pair on a half-pitch 4x MSAA rectangle, notes/35) are
// drawn at 2x resolution scale onto the 1x target that owns the same tiles - the tile-range clear of D29 - and repeated
// on every other existing target of the same storage class and tile pitch that overlaps those tiles. The 24-point no-op
// draws are skipped and counted. Our code (MIT).
#include <xenos/dev_tools.h>
#include <xenos/tracy_profile.h>
#include <xenos/renderer/async_file_appender.h>
#include <xenos/renderer/resolve_reuse.h>
#include <xenos/renderer/resolve_through.h>
#include <xenos/dxbc_gen.h>
#include <xenos/dxbc_spirv.h>
#include <xenos/display_size.h>
#include <xenos/formats.h>
#include <xenos/hlsl.h>
#include <xenos/display_gamma.h>
#include <xenos/gpu_system.h>
#include <xenos/primitives.h>
#include <xenos/resolve.h>
#include <xenos/texture_layout.h>
#include <xenos/renderer/gpu.h>
#include <xenos/renderer/pipeline_state.h>
#include <xenos/renderer/render_target_cache.h>
#include <xenos/renderer/guest_watch.h>
#include <xenos/renderer/shader_cache.h>
#include <xenos/renderer/texture_cache.h>
#include <xenos/renderer/queries.h>
#include <xenos/colour_formats.h>
#include <xenos/renderer/colour_hlsl.h>

#ifdef _WIN32
#include <windows.h>
void XenosRendererFatal(const std::string& why, bool vulkan);   // d3d12_backend.cpp (0.2.0, #9)
#endif
#include "builtin_shaders.h"
#include "smaa/smaa_area_tex.h"
#include "xenos/renderer/upscaler.h"
#include "smaa/smaa_passes.h"
#include "smaa/smaa_search_tex.h"

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/hash.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xvideo.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
// SDK Vulkan classes: the SDK built with REXGLUE_USE_VULKAN, and renderdoc_app.h reachable (rex/ui/vulkan/instance.h
// includes it; job 375: rex::runtime doesn't pass that include folder on, CMakeLists links rex::renderdoc for it).
#if defined(REX_HAS_VULKAN) && REX_HAS_VULKAN && __has_include(<renderdoc_app.h>)
#define GN_SDK_VULKAN 1
#else
#define GN_SDK_VULKAN 0
#endif
#if GN_SDK_VULKAN  // notes/68 stage V3
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#endif
#include <rex/ui/flags.h>  // present_hdr (SDK patch 24)

#include <algorithm>
#include <source_location>
#include <tuple>
#include <array>
#include <bit>
#include <bitset>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <intrin.h>
#include <span>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <fstream>
#include <unordered_set>
#include <vector>
#if defined(GN_PGO_GEN)
// Patch 0159: the LLVM profile runtime (linked only into a GN_PGO=gen build) - writes the counters to LLVM_PROFILE_FILE.
extern "C" int __llvm_profile_write_file(void);
#endif

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
REXCVAR_DEFINE_INT32(gow2_renderer_pipeline_save_seconds, 15, "Gears2",
                     "Patch 0056: seconds between saves of the pipeline library while it grows (was 60; a quit or a "
                     "hard exit lost everything since the last save). Minimum 5");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_slim, true, "Gears2",
                    "Patch 0060 (DXBC back end): smaller DXBC with the same values - ALU operands used in place, no "
                    "one-pass loop where nothing breaks out of it, branch-free pixel prologue. The driver's pipeline "
                    "build time grows with the DXBC size (job 355: 2.4 ms per KB on an RTX 3080 Ti)");
REXCVAR_DEFINE_BOOL(gow2_renderer_dxbc_canary_maths, false, "Gears2",
                    "Patch 0060 (DXBC back end): Canary's max / min (SM3 a >= b ? a : b) and result clamps (_sat) "
                    "instead of the exact FTZ / total-order / NaN emulation - ~20% smaller DXBC; tiny value "
                    "differences at -0 / NaN / denormals, as in Canary");
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
REXCVAR_DEFINE_INT32(gow2_renderer_play_hold_draws, 0, "Gears2",
                    "Patch 0132: 0 by default (no mid-play holds). Patch 0064: outside the load window (mid-play), a frame is held when at least this many draws "
                    "wait for their pipeline; 0 = never hold mid-play (0063's rule)");
REXCVAR_DEFINE_BOOL(gow2_renderer_shader_precreate, true, "Gears2",
                    "Patch 0066 (Shagg: build behind the loading screen): shaders the game creates while loading (a "
                    "hook in each game's own shader-creation code) are translated at once and built by the driver in "
                    "a guessed pipeline in the background, before their first draw. Job 362c [V]: the driver keeps "
                    "each shader stage, so the real pipelines later cost ~0.1 ms instead of 15-35 ms. DXBC path only");
REXCVAR_DEFINE_BOOL(gow2_renderer_stage_prefetch, true, "Gears2",
                    "Restore compiled stage-cache entries on background workers when UE3 creates its shaders, "
                    "before the first draw using a new pipeline combination; live draws retain priority");
REXCVAR_DEFINE_BOOL(gow2_renderer_shader_prebuild, true, "Gears2",
                    "Prepare all package vertex/pixel shaders and both stored variants at startup (D3D12 DXBC). "
                    "Stage bytecode and driver pipeline keys match the normal renderer; dynamic variants retain live fallback");
REXCVAR_DEFINE_INT32(gow2_renderer_shader_prebuild_queue, 64, "Gears2",
                    "Patch 0068: the most warm-only pipelines queued for the pipeline workers at once (the rest waits)");
REXCVAR_DEFINE_INT32(gow2_renderer_shader_prebuild_workers, -1, "Gears2",
                    "Prebuild v2: pipeline workers building warm-only pipelines at the same time while the game loads "
                    "(no swap for 250 ms, or the 20 s after a load / new scene as for the load hold). -1 (default) = a "
                    "quarter of the logical processors, at least 2. Patch 0069 used 3 at all times (job 368b: all 12 at "
                    "once doubled the frame time of a cold ch10 and slowed the draws' own pipelines)");
REXCVAR_DEFINE_INT32(gow2_renderer_shader_prebuild_play_workers, 1, "Gears2",
                    "Prebuild v2: the same during play (outside a load): 1 (default) keeps building areas the game "
                    "streams in while playing at a trickle; 0 = warm-only builds only while loading");
// Retained for compatibility with old launcher configurations. Speculative work never holds a live frame.
REXCVAR_DEFINE_BOOL(gow2_renderer_shader_prebuild_hold, false, "Gears2", "Deprecated: use ue3_shader_prebuild_startup");
REXCVAR_DEFINE_INT32(gow2_renderer_shader_prebuild_hold_max_ms, 120000, "Gears2", "Deprecated; no gameplay prebuild holds");
REXCVAR_DEFINE_STRING(gow2_renderer_shader_prebuild_variant, "auto", "Gears2", "Deprecated; all stored variants are prepared");
REXCVAR_DEFINE_INT32(gow2_renderer_shader_prebuild_priority, -2, "Gears2",
                    "Prebuild v2: Windows priority of a pipeline worker while it builds a warm-only pipeline: -2 = "
                    "lowest (default), -1 = below normal, 0 = the workers' own (gow2_renderer_pipeline_thread_priority)");
REXCVAR_DEFINE_INT32(gow2_renderer_shader_precreate_ms, 6, "Gears2",
                    "Patch 0066: the most time per frame spent translating shaders the game created (ms); the rest "
                    "waits for the next frame");
REXCVAR_DEFINE_INT32(gow2_renderer_play_hold_max_ms, 1000, "Gears2",
                    "Patch 0064: the longest one mid-play hold may wait (ms); then the frame is shown as it is");
REXCVAR_DEFINE_BOOL(gow2_renderer_texture_batch, true, "Gears2",
                    "Batched texture loads (D3D12; 2026-10-29, the Gears 2 level-start freeze: ~4,900 BC mip loads in one "
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
REXCVAR_DEFINE_INT32(gow2_renderer_log_depth_bias_base, -1, "Gears2",
                     "Diagnostic (Gears 3 job 025): on comparison frames, log cull / polygon-offset registers of up to 400 "
                     "depth draws into the depth target at this EDRAM base (tiles); -1 = off");
REXCVAR_DEFINE_BOOL(gow2_renderer_msaa_transfers, true, "Gears2",
                    "Gears 3: a 4x MSAA colour target whose EDRAM tiles a 1x target of the same format wrote last takes "
                    "that data (each 2x2 block of 1x pixels -> the 4 samples of one 4x pixel, as EDRAM stores them)");
REXCVAR_DEFINE_BOOL(gow2_renderer_fill_rect_lists, true, "Gears2",
                    "Gears 3 (job 032): rule (b) (fill/clear ownership) also takes fill draws of several rectangles "
                    "(9, 12, ... up to 48 vertices) - ownership bounded by their union instead of the scissor");
REXCVAR_DEFINE_BOOL(gow2_renderer_clear_msaa_twins, true, "Gears2",
                    "Gears 3: a fast clear (redirected from 4x half pitch to the 1x target) also clears a REAL 4x MSAA colour "
                    "target with the same key, which otherwise keeps last frame's contents");
REXCVAR_DEFINE_BOOL(gow2_renderer_msaa_colour_resolves, true, "Gears2",
                    "Resolve MSAA colour targets (Gears 3: one 4x MSAA 7e3 half-resolution resolve per frame): read the "
                    "selected host samples, average after decoding; false = skip them as before (\"build B\")");
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
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_clear_in_pass, true, "Gears2",
                    "Patch 0092 (Vulkan, stage V5b; the default since 0093 - job 396: ch10 passes 90 -> 76 and barrier "
                    "calls 186 -> 151 a frame, ch15 110 -> 101 and 201 -> 179, scenery pixel-identical): a clear of a "
                    "currently bound target runs inside the render pass (vkCmdClearAttachments) instead of ending it, "
                    "changing layouts twice and starting a new pass. false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_vulkan_dedicated_images, false, "Gears2",
                    "Patch 0097 (Vulkan): images the driver prefers in an allocation of their own (NVIDIA: render "
                    "targets) get one, as D3D12's committed resources are. A measurement switch first (job 403): "
                    "Vulkan colour targets draw in ~2x D3D12's GPU time even with DXC-compiled shaders (job 402)");
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
REXCVAR_DEFINE_STRING(gow2_renderer_vulkan_attachment_layouts, "general", "Gears2",
                      "Patch 0100 (Vulkan, a measurement switch first): the layout render passes draw to their targets "
                      "in. general = VK_IMAGE_LAYOUT_GENERAL, as before (targets are also sampled) | optimal = "
                      "COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL while drawn to (as Canary does), back "
                      "to GENERAL in the next full barrier outside a pass. Job 404: Vulkan's colour targets cost ~2.5x "
                      "D3D12's per pixel, not more pixels; job 406: spirv-opt shaders gave no gain");
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
REXCVAR_DEFINE_STRING(gow2_renderer_vulkan_spirv_dump_dir, "", "Gears2",
                      "Patch 0099 (Vulkan, a measurement): write each shader's SPIR-V from our converter to this folder "
                      "as <hash of its words>.spv");
REXCVAR_DEFINE_STRING(gow2_renderer_vulkan_spirv_override_dir, "", "Gears2",
                      "Patch 0099 (Vulkan, a measurement): where this folder has <hash>.opt.spv for a shader, use it in "
                      "place of our converter's SPIR-V (an offline-optimised copy, to time whether optimising helps)");
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
REXCVAR_DEFINE_BOOL(gow2_renderer_tex_post_constants, true, "Gears2",
                    "Patch 0105 (with --gow2_renderer_tex_num_format, the default): the texture-fetch post-processing "
                    "values that depend only on the fetch constant (per component sign mode, signed mode and fixed "
                    "width; per fetch fixed / integer / point sampled) are worked out once per draw on the CPU and read "
                    "by the shaders from the fetch-constant buffer, instead of per pixel (a 25-way format switch and "
                    "~40 integer ops per fetch). Same results by construction. Job 412: that per-pixel work is ~1.6 ms of "
                    "ch15's 5.3 ms of Vulkan draws (0.7 ms of D3D12's 3.3). Changes the generated shaders");
REXCVAR_DEFINE_BOOL(gow2_renderer_tex_num_format, true, "Gears2",
                    "Patch 0040 (Canary d119505 / 2ddc5ef / 6a45452 / 0c843ef): texture fetches of fixed-point formats "
                    "follow Canary - integer num_format restores the guest integer, unsigned-biased decodes as offset "
                    "binary, unsigned normalised rounds to 16 fractional bits. Changes the generated shaders. false = as before");
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
REXCVAR_DEFINE_INT32(gow2_renderer_idle_submit, -1, "Gears2",
                     "2026-10-29 (Judgment judder): when our renderer thread runs out of work mid-frame, hand the GPU "
                     "what it has recorded so far once at least this many draws are waiting (at most 4 times a frame), "
                     "so the GPU starts the next frame while the game is still building it. -1 = auto (32 with strict "
                     "occlusion queries, where the game waits for the GPU every frame; off otherwise), 0 = off");
REXCVAR_DEFINE_INT32(gow2_renderer_submit_draws, 0, "Gears2",
                     "Patch 0138: submit our recorded work to the GPU after every N draws (0 = once a frame, as before).");
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
                    "Reuse matching resolve scratch textures unused for four frames when guest addresses change; "
                    "reduces resource creation, destruction and video-memory pressure while streaming");
REXCVAR_DEFINE_INT32(gow2_renderer_dest_evict_frames, 600, "Gears2",
                     "Patch 0032: resolve destination textures (1x and scaled) not used for this many frames are freed "
                     "(0 = never, the old behaviour: they piled up and over-filled video memory in long Gears 3 runs)");
REXCVAR_DEFINE_INT32(gow2_renderer_dest_budget_mb, -1, "Gears2",
                     "Patch 0032/0033: -1 (default, 0033) = free the least recently used resolve destination textures "
                     "(not used in the last 30 frames) only when this process uses over 90% of the OS video memory "
                     "budget, down to 80%; N > 0 = keep their total under N MB (0032's 768 churned at 2x scale: "
                     "~300 freed and remade per 300 frames); 0 = no budget (age only)");
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
REXCVAR_DEFINE_STRING(gow2_renderer_vertex_log_frames, "", "Gears2",
                      "Rainbow-flash diagnostic (job 304): frames first-last; every small draw (rectangle list or at most "
                      "6 vertices) logs its vertex fetch ranges as our renderer reads them (first floats + a hash), and "
                      "at that frame's swap the ranges are hashed again - 'CHANGED after the draw' = the CPU rewrote the "
                      "bytes after we read them (empty = off)");
REXCVAR_DEFINE_DOUBLE(gow2_renderer_pipeline_wait_ms, 3.0, "Gears2",
                      "With async pipelines, wait up to this many ms in total per frame for queued driver work, "
                      "reducing skipped draws and pop-in. Default 3; 0 skips pending draws immediately; -1 removes the cap");
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
REXCVAR_DEFINE_BOOL(gow2_renderer_export_pending_mirror, false, "Gears2",
                    "Patch 0007 (Gears 3 2026-10-23, NPC vertex explosions; race found by both chats): while an "
                    "exported range's readback has not landed in guest memory yet, its pages keep feeding vertex "
                    "fetches from the mirror (the export's bytes) even when a CPU write elsewhere in the same 4 MB block "
                    "ended their GPU-current state - instead of re-uploading the pre-export guest bytes over them. "
                    "false = as before");
REXCVAR_DEFINE_BOOL(gow2_renderer_memexport_readback, true, "Gears2",
                    "With --gow2_renderer_memexport: copy every exported range back into guest memory once the GPU has "
                    "written it (job 041: Gears 3 moves DXT textures with memexport copies, and DXT textures load from "
                    "guest memory, so without this they keep the old bytes - pale blue skin). The reference emulators' "
                    "readback_memexport");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_through, true, "Gears2",
                    "Job 047: a resolve into memory that a cached texture is loaded from (same base, pitch and bytes "
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
REXCVAR_DEFINE_STRING(gow2_renderer_watch_report, "", "Gears2",
                      "Job 151 diagnostic: physical ranges \"start:length;...\" (hex) - every memory-system write callback "
                      "overlapping one is logged (range or page, swap, guest thread)");
REXCVAR_DEFINE_INT32(gow2_renderer_texture_stale_log, 0, "Gears2",
                     "Job 150 diagnostic: log textures that stayed unchanged for N frames and then got new bytes, with "
                     "our recent resolves that overlap them (0 = off; first 400 events)");
REXCVAR_DEFINE_BOOL(gow2_renderer_vertex_reuse, true, "Gears2",
                    "A direct vertex range drawn again before the next release point (fence, read pointer, interrupt) "
                    "and in the same command list reuses its ring copy without reading the guest bytes again (job "
                    "174; the title cannot legally rewrite memory an unfinished draw uses before it sees a fence)");
REXCVAR_DEFINE_INT32(gow2_renderer_stencil_transfer_bits, 0, "Gears2",
                     "Depth transfers without SV_StencilRef (NVIDIA): 0 all 8 stencil bit passes, 1 only the bits the "
                     "title ever sets or tests (job 179)");
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
REXCVAR_DEFINE_BOOL(gow2_renderer_separate_queue, false, "Gears2",
                    "Experimental D3D12 render queue separate from SDK presentation, with GPU-fenced image ownership. "
                    "Off by default; unavailable with frame generation. Restart required");
REXCVAR_DEFINE_BOOL(gow2_renderer_gpu_upload, false, "Gears2",
                    "Use CPU-writable video memory for the D3D12 upload ring when supported; avoids repeated GPU "
                    "reads from system memory. Falls back to ordinary upload memory if unavailable");
REXCVAR_DEFINE_BOOL(gow2_renderer_guest_uav_defer, false, "Gears2",
                    "Order guest-memory UAV writes at the next dependent access; transitions already order writes");
REXCVAR_DEFINE_BOOL(gow2_renderer_defer_import_release, true, "Gears2",
                    "Retain imported presentation views until their GPU fence completes, avoiding a recording "
                    "drain during every release; bounded queue falls back to synchronous release");
REXCVAR_DEFINE_INT32(gow2_renderer_copy_threads, 0, "Gears2",
                     "Guest-memory copies into the upload ring run on this many worker threads, finished before each "
                     "submission and before the title can see the GPU got further (job 171; 0 = inline)");
REXCVAR_DEFINE_BOOL(gow2_renderer_float_constants_trim, true, "Gears2",
                    "Upload only the float constants each shader can read (false: all 256 per stage, as before)");
REXCVAR_DEFINE_STRING(gow2_upscaler, "off", "Gears2",
                      "Temporal anti-aliasing / upscaling of the 3D scene (notes/64): off, dlss (NVIDIA DLSS / DLAA, RTX "
                      "GPUs), fsr (AMD FSR, any GPU) or xess (Intel XeSS, any GPU with Shader Model 6.4). Runs on the "
                      "lit scene before the game's post-processing, with "
                      "camera jitter and motion vectors from depth; native-resolution AA at the render scale for now");
REXCVAR_DEFINE_DOUBLE(gow2_upscale_sharpness, 0.0, "Gears2", "FSR's own sharpening, 0..1 (DLSS ignores it)");
REXCVAR_DEFINE_STRING(gow2_upscale_mode, "native", "Gears2",
                      "Patch 0141 (frame-rate item 5): with --gow2_upscaler, draw the 3D scene below the output size and "
                      "let DLSS / FSR / XeSS fill in the detail. native = the scene at the render scale (anti-aliasing "
                      "only, as before); performance = half (2x -> 1x, 4x -> 2x); quality = 3x -> 2x; ultra = 3x -> 1x. "
                      "The output size is --gow2_renderer_resolution_scale; a mode with no integer pair there stays native");
REXCVAR_DEFINE_BOOL(gow2_fg_inputs, false, "Gears2",
                    "Patch 0145 (frame-rate item 6, frame generation step 1): make the scene's depth and motion vectors "
                    "every frame even with no upscaler selected (no jitter: the picture is unchanged). Frame generation "
                    "needs them; on its own this adds two small GPU passes and a log line");
REXCVAR_DEFINE_INT32(gow2_upscale_jitter_sign, 1, "Gears2",
                     "Diagnostic: 1 or -1 - the sign of the jitter handed to the upscaler (the image shift itself is "
                     "unchanged), for checking against the DLSS SDK's jitter overlay");
REXCVAR_DEFINE_DOUBLE(gow2_upscale_jitter_scale, 1.0, "Gears2",
                      "Diagnostic (Gears 3 chat, job 058): multiplies the sub-pixel camera jitter, both the image shift and "
                      "what the upscaler is told (0 = no jitter; 1 = normal)");
REXCVAR_DEFINE_BOOL(gow2_upscale_camera_move, true, "Gears2",
                    "Motion vectors include the camera's movement, read off the static objects drawn (UE3 renders "
                    "camera-relative, so its camera matrix only turns; notes/64). false: turning only, for comparison");
// From the Gears 3 chat (its commit 80d9b9e, job 056): which vertex constants carry the object rows / the camera
// position. 0 = Gears 2's behaviour (objects c5-c8, no camera position); gow3.exe sets 7 and 5.
REXCVAR_DEFINE_INT32(gow2_upscale_object_constants, 0, "Gears2",
                     "Camera move for DLSS / FSR / XeSS: the first of the four vertex constants holding an object's "
                     "object-to-world rows (translation in the fourth). 0 = the game's (Gears 2: c5; Gears 3 sets c7)");
REXCVAR_DEFINE_INT32(gow2_upscale_camera_constant, 0, "Gears2",
                     "Camera move for DLSS / FSR / XeSS: a vertex constant holding the camera's world position on every "
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
                     "Which colour target formats count as the scene for DLSS / FSR / XeSS: 1 = 16-bit float (Gears 2's lit "
                     "scene), 2 = 10:10:10:2 UNORM (Gears 3's), 3 = both. Gears 2 keeps 1: in a ch05 cutscene a "
                     "full-screen 10:10:10:2 target came first and was taken instead of the scene (job 276); gow3.exe sets 2");
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
REXCVAR_DEFINE_BOOL(gow2_renderer_scaled_resolve_through, true, "Gears2",
                    "Update a cached scaled texture during the scaled-memory write, avoiding its later reload. "
                    "Requires a single tiled level, matching layout, complete coverage and unchanged page versions");
REXCVAR_DEFINE_INT32(gow2_renderer_fused_scaled_min_pixels, 65536, "Gears2",
                     "Minimum guest-pixel resolve area for the optional fused path; smaller resolves keep the reference "
                     "path. 0 restores the unrestricted experiment. Size filtering alone does not establish equivalence");
REXCVAR_DEFINE_BOOL(gow2_renderer_fused_scaled_resolve, false, "Gears2",
                    "Job 238: the resolve shaders' scaled pass writes straight into the scaled-memory windows (one pass "
                    "per window), skipping the scaled destination texture and the copy from it. Front-buffer copies keep "
                    "the old path (the presenter reads the scaled destination)");
REXCVAR_DEFINE_BOOL(gow2_renderer_gpu_timestamp_split, true, "Gears2",
                    "Patch 0109 (both APIs, a measurement switch): false = GPU timestamps only at the start and end of "
                    "each command list - the whole-frame GPU time stays, the split by kind of work / target / draw goes "
                    "(the GPU line shows it all as 'draws'). Each Vulkan timestamp waits for all earlier work "
                    "(ALL_COMMANDS); this shows whether the ~100+ a frame change Vulkan's or D3D12's GPU time");
REXCVAR_DEFINE_BOOL(gow2_renderer_skip_resolve_dispatches, false, "Gears2",
                    "Patch 0117 (both APIs, a MEASUREMENT switch - wrong pictures): the renderer's compute steps for "
                    "resolves and EDRAM copies to guest memory are not sent (present and upscaler compute stay). Shows "
                    "how much of each API's frame those steps and the render-pass breaks / barriers around them cost");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_draws, false, "Gears2",
                    "Patch 0119 (both APIs, test): the colour resolve step for single-sample sources into 32 bpp "
                    "destinations runs as a full-screen draw into the destination (a render target) instead of a "
                    "compute dispatch (jobs 427 / 428: on Vulkan a dispatch that reads the target just drawn costs "
                    "~16-23 us more than the same step as a draw). 64 / 8 / 16 / 128 bpp, MSAA sources, fused scaled "
                    "resolves and the copy into guest memory stay dispatches");
REXCVAR_DEFINE_INT32(gow2_renderer_target_stats_frame, 0, "Gears2",
                     "Patch 0121 (a diagnostic, both APIs): non-zero = in this frame, every time the draws move to other "
                     "colour targets, read the targets just drawn back (the frame waits for it) and log per channel "
                     "mean / min / max / NaN count with the target id and the draw count so far. The same frame on "
                     "D3D12 and Vulkan then shows the first target whose contents differ (Gears 3's Vulkan black blocks)");
REXCVAR_DEFINE_INT32(gow2_renderer_target_stats_each_draw, 0, "Gears2",
                     "Patch 0122 (with --gow2_renderer_target_stats_frame): N > 0 = also read the current colour targets "
                     "back after each of the frame's first N draws (not only when the targets change), so the first "
                     "draw whose output differs between D3D12 and Vulkan shows (Gears 3 job 129: the HDR scene target is "
                     "already ~20% darker on Vulkan after its first 105 opaque draws)");
REXCVAR_DEFINE_INT32(gow2_renderer_target_stats_each_from, 0, "Gears2",
                     "Patch 0123 (with --gow2_renderer_target_stats_each_draw=N): start the each-draw readouts after this "
                     "many draws of the stats frame instead of from the first draw (readouts after draw from+step, "
                     "from+2*step, ... from+N*step)");
REXCVAR_DEFINE_INT32(gow2_renderer_target_stats_each_step, 1, "Gears2",
                     "Patch 0123 (with --gow2_renderer_target_stats_each_draw=N): draws between each-draw readouts "
                     "(1 = every draw; 100 = every 100th draw, so N=11 from 1100 covers draws 1200..2200)");
REXCVAR_DEFINE_BOOL(gow2_renderer_target_stats_textures, false, "Gears2",
                    "Patch 0124 (with --gow2_renderer_target_stats_frame): at each target-stats readout also read back "
                    "the 2D textures the previous draw sampled (decodable colour formats, e.g. an R32F depth copy) and "
                    "log the same per-channel mean / min / max / NaN line with the slot, so a draw whose output differs "
                    "between D3D12 and Vulkan shows whether its inputs already differ (Gears 3 job 130 draw 303)");
REXCVAR_DEFINE_BOOL(gow2_renderer_gpu_by_target, false, "Gears2",
                    "Job 234 diagnostic: GPU draw time per render target, logged with the CPU split (one more GPU "
                    "timestamp whenever the draws move to another target)");
REXCVAR_DEFINE_BOOL(gow2_renderer_scaled_miss_log, false, "Gears2",
                    "Job 189 diagnostic: count why a texture our resolves wrote was not taken at the resolution scale. "
                    "It walks every page of each missed texture - 45% of the renderer thread at 2x (job 225) - so off");
REXCVAR_DEFINE_BOOL(gow2_renderer_packed_tail_at_base, true, "Gears2",
                    "Patch 0087 (Canary 344fca7): a small texture whose level 0 is already the packed mip tail (short side "
                    "16 or less) has its whole tail at the base address - also when the fetch constant's mip address is 0 "
                    "(we used to load level 0 only, so distant mips read level 0). false = the old way");
REXCVAR_DEFINE_INT32(gow2_renderer_texture_load_log, 0, "Gears2",
                     "Job 045 diagnostic: log every texture load (new or reloaded, with the reason) in this frame (0 = off)");
REXCVAR_DEFINE_STRING(gow2_renderer_texture_probe, "", "Gears2",
                      "Job 284 diagnostic: comma list of guest texture base addresses (hex). Each lookup of those textures "
                      "in --gow2_renderer_texture_probe_frames is logged once per frame (what the cache did, last load, "
                      "hash of the guest bytes now and at that load) and the bytes saved to --gow2_renderer_texture_probe_dir");
REXCVAR_DEFINE_STRING(gow2_renderer_texture_probe_frames, "", "Gears2", "Job 284: frame window first-last for the probe");
REXCVAR_DEFINE_STRING(gow2_renderer_texture_probe_dir, "", "Gears2", "Job 284: folder for the probe's byte dumps (empty = none)");
REXCVAR_DEFINE_BOOL(gow2_renderer_texture_always_reload, false, "Gears2",
                    "Job 144 check: re-upload a texture on every write to its pages (no unchanged-bytes shortcut)");
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
                      "Occlusion queries: strict (defer completion until this query's GPU result; own split GPU) | "
                      "off | shadow (run our host queries and log our counts, write nothing) | "
                      "answer (also write our answers into the game's query reports: the last real count for that "
                      "report, 1 if none; corrected when our count lands)");
REXCVAR_DEFINE_INT32(gow2_renderer_queries_min, 0, "Gears2",
                     "Smallest immediate occlusion answer (0 keeps real zeros; 1 = nothing is culled until our count for "
                     "that query has landed, as Canary's fast mode)");
REXCVAR_DEFINE_INT32(gow2_renderer_queries_hidden_after, 3, "Gears2",
                     "Answer an occlusion query 'hidden' only after this many real zero counts in a row for it (1 = the "
                     "first zero, as before job 139; a wrong 'hidden' makes an object vanish for a frame)");
REXCVAR_DEFINE_BOOL(gow2_renderer_queries_correct, true, "Gears2",
                    "Rewrite a query report with our real count when it lands, if the report still holds our estimate");
REXCVAR_DEFINE_INT32(gow2_renderer_queries_log, 0, "Gears2",
                     "Per-query lines (estimate vs our count) for the first N queries of each summary frame (0 = none)");
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
REXCVAR_DEFINE_DOUBLE(gow2_hdr_peak, 1000.0, "Gears2", "Stage 7.3: the display's peak nits, for highlight expansion");
REXCVAR_DEFINE_DOUBLE(gow2_hdr_knee, 0.75, "Gears2",
                      "Stage 7.3: linear luminance (0-1 of paper white) above which highlights are expanded");
REXCVAR_DEFINE_DOUBLE(gow2_hdr_expand, 1.0, "Gears2",
                      "Stage 7.3: highlight expansion strength 0-1 (0 = SDR image shown at paper white in HDR10)");
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
REXCVAR_DEFINE_INT32(gow2_present_check_every, 0, "Gears2",
                     "With --gow2_present=ours: every N frames read back what the presenter holds (the image the window "
                     "paints, Presenter::CaptureGuestOutput) and compare it with our image of that frame (stalls that "
                     "frame; 0 = never)");
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
REXCVAR_DEFINE_STRING(gow2_renderer_resolve_readback, "none", "Gears2",
                      "Patch 0020 (Canary readback_resolve, T2 only): none (default, as before), fast (Canary's default: "
                      "each resolve lands the previous copy of the same destination in guest memory, no stall; the "
                      "first one waits) or full (wait for the GPU at every resolve: exact, slow). Turns on "
                      "--gow2_renderer_resolve_prepare_mirror");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_prepare_mirror, false, "Gears2",
                    "Patch 0020 (Canary SharedMemory::RequestRange before a resolve writes): before our resolve writes "
                    "the GPU mirror, its destination pages that the CPU wrote since the mirror last had them get the "
                    "CPU bytes first, and the range is watched (Canary MakeRangeValid), so the bytes of those pages the "
                    "resolve does not cover are current when the pages become GPU-current");
REXCVAR_DEFINE_INT32(gow2_renderer_log_aliases, 20, "Gears2",
                     "Log at most this many distinct EDRAM alias pairs (a depth target reusing tiles another target "
                     "wrote last)");

namespace {

using rex::graphics::DrawObserver;
using rex::graphics::ObservedIndexBuffer;
using rex::graphics::RegisterFile;
using rex::graphics::Shader;
namespace gx = rex::graphics::xenos;
namespace rr = ::xenos::renderer;
namespace gpu = ::xenos::renderer::gpu;

// Register numbers (rexglue SDK include/rex/graphics/register_table.inc) not already in pipeline_state.h.
constexpr uint32_t kRegVgtIndxOffset = 0x2102;
constexpr uint32_t kRegVgtMaxVtxIndx = 0x2100, kRegVgtMinVtxIndx = 0x2101;  // patch 0028
constexpr uint32_t kRegVgtHosCntl = 0x2285, kRegVgtHosMaxTessLevel = 0x2286;  // MIN is 0x2287
constexpr uint32_t kRegVgtMultiPrimIbResetIndx = 0x2103;
constexpr uint32_t kRegSqContextMisc = 0x2181;
constexpr uint32_t kRegCopyControl = 0x2318;
constexpr uint32_t kRegCopyDestBase = 0x2319;
constexpr uint32_t kRegCopyDestPitch = 0x231A;
constexpr uint32_t kRegCopyDestInfo = 0x231B;
constexpr uint32_t kRegDepthClear = 0x231D;
constexpr uint32_t kRegColorClear = 0x231E;
constexpr uint32_t kRegColorClearLo = 0x231F;
constexpr uint32_t kRegCopySurfaceSlice = 0x2323;
constexpr uint32_t kRegFloatConstVs = 0x4000;  // SHADER_CONSTANT_000_X
constexpr uint32_t kRegFloatConstPs = 0x4400;  // SHADER_CONSTANT_256_X
constexpr uint32_t kRegFetchConst = 0x4800;    // SHADER_CONSTANT_FETCH_00_0
constexpr uint32_t kRegBoolConst = 0x4900;     // SHADER_CONSTANT_BOOL_000_031, then 32 loop constants at 0x4908

constexpr uint32_t Bits(uint32_t v, uint32_t shift, uint32_t width) { return (v >> shift) & ((1u << width) - 1u); }
constexpr int32_t SignExtend(uint32_t v, uint32_t width) {
  const uint32_t m = 1u << (width - 1);
  return int32_t((v ^ m) - m);
}

uint32_t Swap32(uint32_t v, uint32_t endian) {  // xenos::Endian / Endian128 for 32-bit words
  switch (endian) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);  // k8in16
    case 2: return std::byteswap(v);                                     // k8in32
    case 3: return (v << 16) | (v >> 16);                                // k16in32
    default: return v;
  }
}

// Converts guest indices to host uint32 (byte order, 16->32, primitive restart -> 0xFFFFFFFF). Branches hoisted out
// of the loops so they vectorise (job 156: the per-index switch was ~11% of our GPU thread).
static void ConvertIndices(const uint8_t* src, bool is32, uint32_t endian, uint32_t count, bool use_restart,
                           uint32_t reset_index, uint32_t* out) {
  if (is32) {
    switch (endian) {
      case 1:
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t v; std::memcpy(&v, src + 4 * i, 4);
          out[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
        }
        break;
      case 2:
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t v; std::memcpy(&v, src + 4 * i, 4);
          out[i] = std::byteswap(v);
        }
        break;
      case 3:
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t v; std::memcpy(&v, src + 4 * i, 4);
          out[i] = (v << 16) | (v >> 16);
        }
        break;
      default:
        std::memcpy(out, src, size_t(count) * 4);
        break;
    }
  } else if (endian == 2 || endian == 3) {
    // k8in32 / k16in32 swap whole 32-bit units, i.e. pairs of 16-bit indices (the guest reads whole units too).
    const uint32_t units = (count + 1) / 2;
    for (uint32_t u = 0; u < units; ++u) {
      uint32_t pair; std::memcpy(&pair, src + 4 * u, 4);
      pair = endian == 2 ? std::byteswap(pair) : ((pair << 16) | (pair >> 16));
      out[2 * u] = pair & 0xFFFFu;
      if (2 * u + 1 < count) out[2 * u + 1] = pair >> 16;
    }
  } else if (endian == 1) {
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t h; std::memcpy(&h, src + 2 * i, 2);
      out[i] = uint16_t((h << 8) | (h >> 8));
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t h; std::memcpy(&h, src + 2 * i, 2);
      out[i] = h;
    }
  }
  if (use_restart) {
    const uint32_t reset = is32 ? reset_index : (reset_index & 0xFFFFu);
    for (uint32_t i = 0; i < count; ++i) out[i] = out[i] == reset ? 0xFFFFFFFFu : out[i];
  }
}

// XeSystem constant buffer (notes/39 binding contract; hlsl.cpp prelude).
struct SystemConstants {
  uint32_t sys0[4];
  float ndc_scale[4];
  float ndc_offset[4];
  float color_exp_scale[4];
  uint32_t sys1[4];
  uint32_t sys2[4];  // patch 0028: VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX (read only by shaders built with XE_VTX_CLAMP)
};
static_assert(sizeof(SystemConstants) == 96);

const char* DepthFormatName(uint32_t f) { return f ? "D24FS8" : "D24S8"; }



struct FrameStats {
  double pipeline_wait_ms = 0.0;  // job 037: total time spent waiting for new pipelines under the per-frame budget
  double shader_wait_ms = 0.0;    // shader pop-in: time draws waited for their shader (--gow2_renderer_shader_wait_ms)
  uint64_t shader_waits = 0, shader_waits_ready = 0;
  uint64_t resolve_dispatches_skipped = 0;  // patch 0117
  uint64_t resolve_draws = 0;               // patch 0119
  uint64_t standin_waits = 0, standin_over_budget = 0;  // patch 0045
  uint64_t depth_pipeline_waits = 0, depth_pipeline_waits_won = 0;  // patch 0132
  double depth_pipeline_wait_ms = 0.0;
  double standin_wait_ms = 0.0;
  uint64_t draws = 0, drawn = 0, clears = 0, clear_copies = 0, colour_clears = 0, dummy_points = 0, patches = 0,
           tessellated = 0, patches_not_drawn = 0,
           colour_only = 0, pipelines_prewarmed = 0, pipeline_waits = 0, memexport_draws = 0, memexport_only_draws = 0,
           memexport_ps_draws = 0, memexport_ranges = 0, memexport_bytes = 0, vertex_from_exports = 0,
           memexport_readbacks = 0, memexport_readback_bytes = 0, memexport_readback_failures = 0,
           memexport_ranges_gathered = 0, memexport_readbacks_issued = 0,  // patch 0042
           lookaheads = 0, lookahead_draws = 0, lookahead_queued = 0,  // patch 0051
           pipelines_recorded = 0,  // patch 0058
           load_holds = 0,  // patch 0063: frames held back until their pipelines were built
           created_seen = 0, created_known = 0, created_translated = 0, created_guessed = 0, created_prefetched = 0,
           new_draw_pipelines = 0, new_vs_from_load = 0, new_ps_from_load = 0,                // patch 0066
           prebuild_offered = 0, prebuild_known = 0, prebuild_queued = 0,                     // patch 0068
           new_ps_pipelines = 0, new_ps_prebuilt = 0,                                         // patch 0068
           cosmetic_deferred = 0,  // patch 0052
           prefetched = 0,  // patch 0053
           dxbc_mixed = 0,  // DXBC back end: draws pairing a DXBC shader with a DXIL one
           vertex_other_type = 0,  // patch 0049
           memexport_readback_waits = 0, export_waits = 0, depth_alias_dropped = 0, vertex_pages_kept_pending = 0, texture_dependent = 0, alias_tiles = 0, alias_events = 0, pending_shaders = 0,
           shader_errors = 0, pipeline_failures = 0, other_failures = 0, copies = 0, depth_resolves = 0, colour_resolves = 0,
           vertex_bytes = 0, index_bytes = 0, empty = 0, vertex_skipped_bytes = 0, texture_bytes = 0,
           vertex_direct_bytes = 0, vertex_reused_bytes = 0, vertex_memo_hits = 0, xfer_px[4] = {},
           stencil_passes_skipped = 0, texture_memo_hits = 0, readonly_transfers_skipped = 0, readonly_px_skipped = 0,
           fill_transfers_skipped = 0, fill_px_skipped = 0, fill_passes_skipped = 0, fill_ownership_draws = 0, fill_multi_rect_draws = 0, const_gen_skips = 0, compacted_uploads = 0, compacted_bytes_saved = 0,
           transferred_tiles = 0, cleared_tiles = 0, unknown_tiles = 0, index_skipped_bytes = 0, watch_checked = 0,
           watch_stale = 0, watch_stale_bytes = 0, unknown_colour_tiles = 0, disagree_tiles = 0, tainted_tiles = 0, colour_to_depth_tiles = 0, transfer_passes = 0,
           unclipped_depth_draws = 0, mrt_duplicates = 0, colour_drawn = 0, colour_transferred_tiles = 0,
           colour_unknown_tiles = 0, resolve_view_targets = 0, msaa_colour_resolves = 0, msaa_twin_clears = 0, msaa_up_transfers = 0, colour_resolves_compared = 0, fixed16_multiplicative_blends = 0,
           colour_word_transferred_tiles = 0, colour_clears_resolve = 0;
  uint64_t colour_not_modelled[16] = {};  // per colour storage class: draws into it we don't render yet
  double cpu_ms = 0.0;
  std::map<std::string, uint64_t> skipped;
};

// CPU cost split (upl-n101): every microsecond our observer spends on the GPU thread is charged to exactly one part
// (exclusive time: a nested part pauses the one around it).
enum Part : uint32_t {
  kPartState,      // register decode, clear detection, shader lookups, pipeline description
  kPartOwnership,  // tile ownership books (both rules) and alias bookkeeping
  kPartTransfer,   // recording EDRAM transfers and known-clear fills
  kPartTextures,   // texture cache Bind + backend SetTextures
  kPartConstants,  // system/float/bool/fetch constants
  kPartVertex,     // vertex range watch / hash / copy into the guest mirror
  kPartIndex,      // index read / conversion / cache
  kPartRecord,     // pipeline lookup and backend draw recording (descriptor tables, constant upload, barriers)
  kPartResolve,    // depth resolves (dispatch, readbacks)
  kPartCompare,    // comparison frames: snapshots and pixel comparisons
  kPartPoll,       // shader cache poll (finished compiles)
  kPartFrame,      // OnSwap: submit, frame pacing (waits are also counted in "GPU waits")
  kPartOther,
  kPartCount
};
constexpr const char* kPartNames[kPartCount] = {"state", "ownership", "transfers", "textures", "constants", "vertex",
                                                "index", "record", "resolve", "compare", "poll", "frame", "other"};

class ShadowRenderer final : public DrawObserver, public XenosUcodeSink {
 public:
  explicit ShadowRenderer(const XenosRendererHints& hints) : hints_(hints) {}

  bool Init(rex::Runtime* runtime) {
    ::xenos::profiling::Initialize();
    COGBOX_TRACY_SCOPE("Renderer initialization");
    memory_ = runtime ? runtime->memory() : nullptr;
    if (!memory_) {
      REXLOG_ERROR("gow2_renderer: no guest memory; staying off");
      return false;
    }
    const bool vulkan = REXCVAR_GET(gow2_renderer_api) == "vulkan";  // notes/68
    api_vulkan_ = vulkan;  // prebuild v2: warm-only pipelines are D3D12 only
    if (!vulkan && REXCVAR_GET(gow2_renderer_api) != "d3d12")
      REXLOG_WARN("gow2_renderer: unknown --gow2_renderer_api '{}' (d3d12 | vulkan); using d3d12", REXCVAR_GET(gow2_renderer_api));
    backend_ = vulkan ? gpu::CreateVulkanBackend() : gpu::CreateD3D12Backend();
    if (!backend_) {
      REXLOG_ERROR("gow2_renderer: no {} backend in this build; staying off{}", vulkan ? "Vulkan" : "D3D12",
                   vulkan ? " - --gow2_renderer_api=vulkan was asked for, but vulkan_backend.cpp was built without the "
                            "Vulkan headers (the game's CMake needs the third_party/vulkan-headers lines; patch 0120)"
                          : "");
      return false;
    }
    gpu::BackendOptions options;
    options.threaded_recording = REXCVAR_GET(gow2_renderer_threaded_recording);
    options.gpu_timestamp_split = REXCVAR_GET(gow2_renderer_gpu_timestamp_split);  // patch 0109
    options.copy_threads = uint32_t(std::clamp(REXCVAR_GET(gow2_renderer_copy_threads), 0, 16));
    if (REXCVAR_GET(gow2_renderer_pipeline_library)) {  // notes/64: next to our compiled shaders
      std::filesystem::path dir = REXCVAR_GET(gow2_shader_cache_dir);
      if (dir.empty()) dir = std::filesystem::current_path() / "out" / "shader-cache";
      options.pipeline_library_path = (dir / "pipelines-d3d12.bin").string();
      options.pipeline_save_background = REXCVAR_GET(gow2_renderer_pipeline_save_background);  // patch 0010
      options.pipeline_load_async = REXCVAR_GET(gow2_renderer_pipeline_load_async);
      options.pipeline_threads = REXCVAR_GET(gow2_renderer_pipeline_threads);            // patch 0056
      options.pipeline_thread_priority = REXCVAR_GET(gow2_renderer_pipeline_thread_priority);  // patch 0162
      options.pipeline_save_seconds = REXCVAR_GET(gow2_renderer_pipeline_save_seconds);  // patch 0056
    }
    options.warm_workers = PrebuildLoadWorkers();  // patch 0069 (v2: the loading count; StartPrebuild governs it)
    options.warm_thread_priority = std::clamp(REXCVAR_GET(gow2_renderer_shader_prebuild_priority), -2, 0);  // v2
    options.debug_layer = REXCVAR_GET(gow2_renderer_debug) || (!vulkan && rex::cvar::Query<bool>("d3d12_debug"));
    options.gpu_upload_ring = REXCVAR_GET(gow2_renderer_gpu_upload) && !vulkan;
    options.defer_guest_uav_barrier = REXCVAR_GET(gow2_renderer_guest_uav_defer) && !vulkan;
    options.defer_import_release = REXCVAR_GET(gow2_renderer_defer_import_release) && !vulkan;
    options.placed_textures = REXCVAR_GET(gow2_renderer_placed_textures);  // patch 0017
    options.batch_texture_loads = REXCVAR_GET(gow2_renderer_texture_batch) && !vulkan;  // 2026-10-29
    options.index_buffers_in_vram = REXCVAR_GET(gow2_renderer_index_buffers_vram);  // patch 0150
    {  // patch 0087
      const std::string m = REXCVAR_GET(gow2_renderer_vulkan_compressible_targets);
      options.vk_mutable_targets = m == "lazy" ? 1u : m == "never" ? 2u : 0u;
      options.vk_pass_barriers = REXCVAR_GET(gow2_renderer_vulkan_pass_barriers) == "when_needed" ? 1u  // 0089
                                 : REXCVAR_GET(gow2_renderer_vulkan_pass_barriers) == "narrow"    ? 2u  // 0115
                                                                                                    : 0u;
      options.vk_clear_in_pass = REXCVAR_GET(gow2_renderer_vulkan_clear_in_pass);                          // 0092
      options.vk_dedicated_images = REXCVAR_GET(gow2_renderer_vulkan_dedicated_images);                    // 0097
      options.vk_count_pixels = REXCVAR_GET(gow2_renderer_vulkan_count_pixels);                            // 0098
      options.vk_attachment_layouts = REXCVAR_GET(gow2_renderer_vulkan_attachment_layouts) == "optimal";   // 0100
      options.vk_barrier_src_stages = REXCVAR_GET(gow2_renderer_vulkan_barrier_stages) == "work";          // 0107
      options.vk_skip_memory_barriers = REXCVAR_GET(gow2_renderer_vulkan_barrier_stages) == "skip_memory";  // 0108
    }
    // Notes/68: on Vulkan, --gow2_present=ours runs our whole present (gamma, SMAA, HDR) into our own image without a
    // presenter yet (stage V3 connects the SDK's Vulkan presenter): the frame shots read that image, so a Vulkan run
    // can be compared picture for picture with a D3D12 one; the window stays black.
    vk_headless_present_ = vulkan && REXCVAR_GET(gow2_present) == "ours";
#if GN_SDK_VULKAN
    // Stage V3: our backend on the SDK Vulkan provider's device and queue (our GPU system made that provider for
    // --gow2_renderer_api=vulkan), so our image goes into its presenter's guest output on the same queue.
    if (vk_headless_present_) {
      auto* gs = runtime->graphics_system();
      auto* provider = gs ? dynamic_cast<rex::ui::vulkan::VulkanProvider*>(gs->provider()) : nullptr;
      auto* presenter = gs ? dynamic_cast<rex::ui::vulkan::VulkanPresenter*>(gs->presenter()) : nullptr;
      const rex::ui::vulkan::VulkanDevice* dev = provider ? provider->vulkan_device() : nullptr;
      if (dev && presenter && dev->queue_family_graphics_compute() != UINT32_MAX) {
        const uint32_t fam = dev->queue_family_graphics_compute();
        auto* queue = dev->queue_families()[fam].queues[0].get();  // the presenter's contract: graphics + compute queue 0
        const auto& pr = dev->properties();
        if (!pr.dynamicRendering || !pr.synchronization2 || !pr.timelineSemaphore) {
          REXLOG_ERROR("gow2_renderer: Vulkan: the SDK's device lacks dynamic rendering / synchronization2 / timeline "
                       "semaphores ({} {} {}) - SDK patch 33 missing? Frame shots only", pr.dynamicRendering,
                       pr.synchronization2, pr.timelineSemaphore);
        } else {
          vk_external_.instance = dev->vulkan_instance()->instance();
          vk_external_.physical_device = dev->physical_device();
          vk_external_.device = dev->device();
          vk_external_.queue_family = fam;
          vk_external_.queue = queue->queue;
          vk_external_.lock_user = queue;
          vk_external_.lock = [](void* q) { static_cast<rex::ui::vulkan::VulkanDevice::Queue*>(q)->mutex.lock(); };
          vk_external_.unlock = [](void* q) { static_cast<rex::ui::vulkan::VulkanDevice::Queue*>(q)->mutex.unlock(); };
          vk_external_.push_descriptor = pr.pushDescriptor;
          vk_external_.depth_clamp = pr.depthClamp;
          vk_external_.fill_mode_non_solid = pr.fillModeNonSolid;
          vk_external_.anisotropy = pr.samplerAnisotropy;
          vk_external_.texture_compression_bc = pr.textureCompressionBC;
          options.vulkan_external = &vk_external_;
          presenter_ = presenter;
          presenter_vulkan_ = true;
          vk_headless_present_ = false;
          REXLOG_INFO("gow2_present: Vulkan - our image goes into the SDK's Vulkan presenter (stage V3)");
        }
      }
    }
#endif
    if (vk_headless_present_)
      REXLOG_WARN("gow2_renderer: Vulkan: our present runs without a window image (the SDK has no Vulkan presenter here) "
                  "- frame shots only");
    if (!vulkan && REXCVAR_GET(gow2_present) == "ours") {  // Stage 5C-2: the presenter's device and direct queue
      auto* gs = runtime->graphics_system();
      auto* provider = gs ? dynamic_cast<rex::ui::d3d12::D3D12Provider*>(gs->provider()) : nullptr;
      auto* presenter = gs ? dynamic_cast<rex::ui::d3d12::D3D12Presenter*>(gs->presenter()) : nullptr;
      if (!provider || !presenter) {
        REXLOG_ERROR("gow2_present: ours needs the SDK's D3D12 provider and presenter ({} {}); our image is not "
                     "presented by us", provider ? "provider ok" : "NO provider", presenter ? "presenter ok" : "NO presenter");
      } else {
        options.external_device = provider->GetDevice();
        options.external_queue = provider->GetDirectQueue();
        options.separate_present_queue = REXCVAR_GET(gow2_renderer_separate_queue) && !XenosFrameGenerationWanted();
        presenter_ = presenter;
        XenosLatencySetDevice(options.external_device);  // patch 0135: Reflex works on the presenting device
      }
    }
    std::string error;
    if (!backend_->Init(options, error)) {
      REXLOG_ERROR("gow2_renderer: device init failed: {}; staying off", error);
      backend_.reset();
#ifdef _WIN32
      XenosRendererFatal(error, REXCVAR_GET(gow2_renderer_api) == "vulkan");   // 0.2.0 (#9): never a blind game
#endif
      return false;
    }
    const std::filesystem::path dxc_dir(REXCVAR_GET(gow2_dxc_dir));
    {
      std::string dxc_error;
      if (!rr::CreateDxcCompiler(dxc_dir, dxc_error))
        REXLOG_ERROR("gow2_renderer: {} - shaders can't be compiled (only disk-cache hits work)", dxc_error);
    }
    rr::ShaderCompilerFactory factory = [dxc_dir]() {
      std::string e;
      return rr::CreateDxcCompiler(dxc_dir, e);
    };
    std::filesystem::path dir = REXCVAR_GET(gow2_shader_cache_dir);
    if (dir.empty()) dir = std::filesystem::current_path() / "out" / "shader-cache";
    cache_ = std::make_unique<rr::ShaderCache>(*backend_, factory, dir, uint32_t(std::max(0, REXCVAR_GET(gow2_shader_threads))));
    cache_->SetCompactFloatConstants(REXCVAR_GET(gow2_renderer_compact_constants));
    cache_->SetTexSignBySwizzle(REXCVAR_GET(gow2_renderer_tex_sign_swizzle));  // patch 0024
    cache_->SetCanaryR21(REXCVAR_GET(gow2_renderer_canary_r21));  // patch 0041
    cache_->SetCanaryDecode(REXCVAR_GET(gow2_renderer_canary_decode));  // patch 0041
    cache_->SetMulscTowardZero(REXCVAR_GET(gow2_renderer_mulsc_round_toward_zero));      // patch 0087
    cache_->SetMemexportF16NanZero(REXCVAR_GET(gow2_renderer_memexport_f16_nan_zero));  // patch 0087
    cache_->SetSpirvPrecise(REXCVAR_GET(gow2_renderer_vulkan_precise));                 // patch 0088
    cache_->SetPsRefactoring(REXCVAR_GET(gow2_renderer_ps_refactoring));                // patch 0095
    xenos::renderer::ShaderCache::SetSpirvFiles(REXCVAR_GET(gow2_renderer_vulkan_spirv_dump_dir),
                               REXCVAR_GET(gow2_renderer_vulkan_spirv_override_dir));  // patch 0099
    xenos::dxbc_spirv::SetDiscardTerminates(REXCVAR_GET(gow2_renderer_vulkan_discard_terminates));  // patch 0102
    xenos::dxbc_spirv::SetFloatControls(REXCVAR_GET(gow2_renderer_vulkan_float_controls));        // patch 0103
    {  // patch 0089
      const std::string m = REXCVAR_GET(gow2_renderer_msaa_sample_positions);
      msaa_positions_mode_ = m == "xenos" ? 1u : m == "canary_off" ? 2u : 0u;
      msaa_positions_ok_ = backend_->SamplePositionsSupported();
      if (msaa_positions_mode_ == 1)
        REXLOG_INFO("gow2_renderer: MSAA sample positions: xenos - {}", msaa_positions_ok_
                        ? "programmable sample positions supported; MSAA draws use the Xenos pattern (patch 0089)"
                        : "NOT supported by this device / backend; standard positions kept (patch 0089)");
      else if (msaa_positions_mode_ == 2)
        REXLOG_INFO("gow2_renderer: MSAA sample positions: canary_off - standard positions, no viewport sample correction");
    }
    cache_->SetTexNumFormat(REXCVAR_GET(gow2_renderer_tex_num_format));  // patch 0040
    tex_post_constants_ = REXCVAR_GET(gow2_renderer_tex_num_format) && REXCVAR_GET(gow2_renderer_tex_post_constants);
    tex_sign_swizzle_ = REXCVAR_GET(gow2_renderer_tex_sign_swizzle);
    cache_->SetTexPostConstants(tex_post_constants_);  // patch 0105
    cache_->SetVertexIndexClamp(REXCVAR_GET(gow2_renderer_vertex_index_clamp));  // patch 0028
    {
      const std::string be = REXCVAR_GET(gow2_renderer_shader_backend);
      if (be != "hlsl" && be != "dxbc")
        REXLOG_WARN("gow2_renderer: unknown --gow2_renderer_shader_backend '{}' (hlsl or dxbc); using hlsl", be);
      // Notes/68 V5a: on Vulkan, dxbc = our DXBC converted to SPIR-V (dxbc_spirv.cpp) instead of HLSL through DXC;
      // --gow2_renderer_vulkan_direct_spirv=false keeps Vulkan on the HLSL path (the A/B and the fallback).
      const bool vk_direct = backend_->GetShaderIL() == gpu::ShaderIL::kSpirv && REXCVAR_GET(gow2_renderer_vulkan_direct_spirv);
      if (be == "dxbc" && backend_->GetShaderIL() == gpu::ShaderIL::kSpirv && !vk_direct)
        REXLOG_WARN("gow2_renderer: Vulkan: --gow2_renderer_vulkan_direct_spirv=false - shaders through HLSL / DXC");
      cache_->SetDxbc(be == "dxbc" && (backend_->GetShaderIL() == gpu::ShaderIL::kDxil || vk_direct));
      cache_->SetDxbcSlim(REXCVAR_GET(gow2_renderer_dxbc_slim), REXCVAR_GET(gow2_renderer_dxbc_canary_maths));  // 0060
      cache_->SetDxbcSalt(uint32_t(REXCVAR_GET(gow2_renderer_dxbc_salt)));  // 0063 (test only)
    }
    cache_->SetUcodeRecord(REXCVAR_GET(gow2_renderer_shader_record));
    cache_->SetPack(REXCVAR_GET(gow2_renderer_shader_pack));  // patch 0015, before the first shader
    // Patch 0047: a negative budget is no cap (1e9 ms = 11 days, past any session).
    const double sw = REXCVAR_GET(gow2_renderer_shader_wait_ms);
    shader_wait_budget_ms_ = sw < 0.0 ? 1e9 : sw;
    shader_wait_left_ms_ = shader_wait_budget_ms_;  // the first frame too
    standin_wait_budget_ms_ = std::max(0.0, REXCVAR_GET(gow2_renderer_standin_wait_ms));  // patch 0045
    standin_wait_left_ms_ = standin_wait_budget_ms_;
    if (REXCVAR_GET(gow2_renderer_shader_warmup)) {
      const rr::ShaderCache::WarmResult w = cache_->WarmFromDisk();
      REXLOG_INFO("gow2_renderer: shader warm-up: {} recorded shaders, {} already known, {} loaded from disk, {} queued "
                  "to compile, {} failed ({:.0f} ms: listing {:.0f}, reading {:.0f}, translating {:.0f}, making {:.0f}; {} from the "
                  "recorded pack, {} files added to it)",
                  w.files, w.known, w.disk_hits, w.queued, w.failed, w.ms, w.list_ms, w.read_ms, w.translate_ms, w.make_ms, w.from_pack, w.packed_now);
    }
    StartPrebuild();  // patch 0068
    res_scale_ = uint32_t(std::clamp(REXCVAR_GET(gow2_renderer_resolution_scale), 1, 4));
    if (REXCVAR_GET(gow2_renderer_resolution_scale) == 0) {
      // "Match my monitor": the smallest scale whose image covers the monitor - fitted by the 16:9 shape, or by width
      // for a wider game image (ultrawide, gow2_present_frame_aspect), whose height follows the game's own shape.
      uint32_t mw = 0, mh = 0;
      if (xenos::display::MonitorSize(mw, mh)) {
        const bool wide = REXCVAR_GET(gow2_present_frame_aspect);
        const double need = wide ? double(mw) / 1280.0 : std::min(double(mw) / 1280.0, double(mh) / 720.0);
        res_scale_ = uint32_t(std::clamp(int(std::ceil(need - 1e-9)), 1, 4));
        REXLOG_INFO("gow2_renderer: resolution scale auto: monitor {}x{} -> {}x ({}x{} game image{})", mw, mh,
                    res_scale_, 1280 * res_scale_, 720 * res_scale_, wide ? ", fitted by width" : "");
      } else {
        res_scale_ = 2;
        REXLOG_WARN("gow2_renderer: resolution scale auto: monitor size unknown; using 2x");
      }
    }
    const uint32_t scale_threshold = uint32_t(std::max(REXCVAR_GET(gow2_renderer_resolution_scale_threshold), 0));
    rts_ = std::make_unique<rr::RenderTargetCache>(*backend_, REXCVAR_GET(gow2_renderer_d24s8_exact), res_scale_,
                                                   scale_threshold);
    if (res_scale_ > 1)
      REXLOG_INFO("gow2_renderer: resolution scale {}x (notes/53){}", res_scale_,
                  scale_threshold ? std::format("; targets with pitch <= {} stay 1x (notes/53b)", scale_threshold)
                                  : std::string());
    watch_ = std::make_unique<rr::GuestWatch>(memory_);
    watch_->SetNarrowWidening(REXCVAR_GET(gow2_renderer_watch_narrow));
    if (REXCVAR_GET(gow2_renderer_watch_narrow))
      REXLOG_INFO("gow2_renderer: page watch: narrow widening on (patch 0008: 256 KB, stops at GPU-written pages)");
    if (const std::string spec = REXCVAR_GET(gow2_renderer_watch_report); !spec.empty()) {
      std::vector<std::pair<uint32_t, uint32_t>> ranges;
      size_t pos = 0;
      while (pos < spec.size()) {
        const size_t end = std::min(spec.find(';', pos), spec.size());
        const std::string item = spec.substr(pos, end - pos);
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
          try {
            ranges.push_back({uint32_t(std::stoul(item.substr(0, colon), nullptr, 16)) & 0x1FFFFFFFu,
                              uint32_t(std::stoul(item.substr(colon + 1), nullptr, 16))});
          } catch (...) {}
        }
        pos = end + 1;
      }
      REXLOG_INFO("gow2_renderer: watch report on {} range(s)", ranges.size());
      watch_->SetReportRanges(std::move(ranges));
    }
    use_textures_ = REXCVAR_GET(gow2_renderer_textures);
    upload_watch_ = REXCVAR_GET(gow2_renderer_upload_watch);
    vertex_direct_ = REXCVAR_GET(gow2_renderer_vertex_direct);
    log_vs_hash_ = std::strtoull(REXCVAR_GET(gow2_renderer_log_vs).c_str(), nullptr, 16);  // patch 0015
    log_vs_max_ = REXCVAR_GET(gow2_renderer_log_vs_max);
    float_constants_trim_ = REXCVAR_GET(gow2_renderer_float_constants_trim);
    gpu_by_target_ = REXCVAR_GET(gow2_renderer_gpu_by_target);
    report_first_ = uint64_t(std::max(0, REXCVAR_GET(gow2_renderer_draw_report_frame)));  // patch 0101
    report_count_ = uint64_t(std::clamp(REXCVAR_GET(gow2_renderer_draw_report_frames), 1, 4));
    report_samples_ = REXCVAR_GET(gow2_renderer_draw_report_samples);  // patch 0102
    fused_scaled_ = REXCVAR_GET(gow2_renderer_fused_scaled_resolve);
    fused_kinds_ = uint32_t(REXCVAR_GET(gow2_renderer_fused_scaled_kinds)) & 3u;
    memexport_ = REXCVAR_GET(gow2_renderer_memexport);
    memexport_readback_ = memexport_ && REXCVAR_GET(gow2_renderer_memexport_readback);
    pending_mirror_ = memexport_readback_ && REXCVAR_GET(gow2_renderer_export_pending_mirror);
    if (pending_mirror_) REXLOG_INFO("gow2_renderer: export pending mirror on (patch 0007)");
    if (memexport_)
      REXLOG_INFO("gow2_renderer: memexport on (notes/11), readback to guest memory {}", memexport_readback_ ? "on" : "off");
    vertex_reuse_ = REXCVAR_GET(gow2_renderer_vertex_reuse);
    lookahead_shaders_ = REXCVAR_GET(gow2_renderer_shader_lookahead);  // patch 0051
    defer_cosmetic_ = REXCVAR_GET(gow2_renderer_defer_cosmetic);  // patch 0052
    shader_prefetch_ = REXCVAR_GET(gow2_renderer_shader_prefetch);  // patch 0053
    skip_resolve_dispatches_ = REXCVAR_GET(gow2_renderer_skip_resolve_dispatches);  // patch 0117
    resolve_draws_ = REXCVAR_GET(gow2_renderer_resolve_draws);                      // patch 0119
    stats_frame_ = REXCVAR_GET(gow2_renderer_target_stats_frame);                   // patch 0121
    stats_each_ = REXCVAR_GET(gow2_renderer_target_stats_each_draw);                // patch 0122
    stats_from_ = std::max(REXCVAR_GET(gow2_renderer_target_stats_each_from), 0);   // patch 0123
    stats_step_ = std::max(REXCVAR_GET(gow2_renderer_target_stats_each_step), 1);   // patch 0123
    stats_textures_ = REXCVAR_GET(gow2_renderer_target_stats_textures);             // patch 0124
    vertex_any_type_ = REXCVAR_GET(gow2_renderer_vertex_any_type);  // patch 0049
    index_reuse_ = REXCVAR_GET(gow2_renderer_index_reuse);
    texture_memo_ = REXCVAR_GET(gow2_renderer_texture_memo);
    readonly_memo_ = REXCVAR_GET(gow2_renderer_readonly_transfer_memo);
    fill_trim_ = REXCVAR_GET(gow2_renderer_fill_transfer_trim);
    fill_ownership_ = REXCVAR_GET(gow2_renderer_fill_ownership);
    fill_rect_lists_ = REXCVAR_GET(gow2_renderer_fill_rect_lists);
    const_gen_skip_ = REXCVAR_GET(gow2_renderer_const_gen_skip);
    upscale_probe_ = uint32_t(std::max(0, REXCVAR_GET(gow2_upscale_probe)));
    stencil_transfer_bits_ = REXCVAR_GET(gow2_renderer_stencil_transfer_bits) != 0;
    static_index_buffers_ = REXCVAR_GET(gow2_renderer_static_index_buffers);
    textures_ = std::make_unique<rr::TextureCache>(*backend_, memory_->TranslatePhysical<const uint8_t*>(0), *watch_);
    textures_->SetHostMemoryCap(uint64_t(std::max(0, REXCVAR_GET(gow2_renderer_texture_cache_mb))) << 20);
    textures_->SetAlwaysReload(REXCVAR_GET(gow2_renderer_texture_always_reload));
    textures_->SetMismatchZero(REXCVAR_GET(gow2_renderer_texture_mismatch_zero));
    textures_->SetScaledMissLog(REXCVAR_GET(gow2_renderer_scaled_miss_log));
    textures_->SetPackedTailAtBase(REXCVAR_GET(gow2_renderer_packed_tail_at_base));  // patch 0087
    textures_->SetBatchLoads(REXCVAR_GET(gow2_renderer_texture_batch) && !api_vulkan_);  // 2026-10-29 (D3D12)
    textures_->SetLoadLogFrame(uint64_t(std::max(REXCVAR_GET(gow2_renderer_texture_load_log), 0)));
    if (const std::string list = REXCVAR_GET(gow2_renderer_texture_probe); !list.empty()) {  // job 284
      std::vector<uint32_t> bases;
      size_t at = 0;
      while (at < list.size()) {
        const size_t comma = std::min(list.find(',', at), list.size());
        if (comma > at) bases.push_back(uint32_t(std::stoul(list.substr(at, comma - at), nullptr, 16)) & 0xFFFFF000u);
        at = comma + 1;
      }
      const std::string win = REXCVAR_GET(gow2_renderer_texture_probe_frames);
      const size_t dash = win.find('-');
      const uint64_t first = win.empty() ? 0 : std::stoull(win.substr(0, dash));
      const uint64_t last = win.empty() ? ~0ull : dash == std::string::npos ? first : std::stoull(win.substr(dash + 1));
      REXLOG_INFO("gow2_texture_probe: {} base(s), frames {}-{}", bases.size(), first, last);
      textures_->SetProbe(std::move(bases), first, last, REXCVAR_GET(gow2_renderer_texture_probe_dir));
    }
    if (memexport_readback_)  // job 042: moved textures load their new bytes, not the old ones for a frame
      textures_->SetBeforeCpuLoadOfGpuPages([this](uint32_t start, uint32_t length) { FlushExportReadbacksFor(start, length); });
    if (const int stale = REXCVAR_GET(gow2_renderer_texture_stale_log); stale > 0) {
      textures_->SetStaleReloadReport(uint32_t(stale), [this](const rr::TextureCache::StaleReload& s) { ReportStale(s); });
      REXLOG_INFO("gow2_renderer: texture stale-reload log on: textures unchanged for {} frames that get new bytes",
                  stale);
    }
    tessellation_ = REXCVAR_GET(gow2_renderer_tessellation);
    tess_scale_ = REXCVAR_GET(gow2_renderer_tess_scale);
    if (REXCVAR_GET(gow2_renderer_texture_always_reload) || REXCVAR_GET(gow2_renderer_texture_cache_mb) != 512)
      REXLOG_INFO("gow2_renderer: textures: always reload {}, cache cap {} MB (job 144 sharpening check)",
                  REXCVAR_GET(gow2_renderer_texture_always_reload), REXCVAR_GET(gow2_renderer_texture_cache_mb));
    log_every_ = std::max(1, REXCVAR_GET(gow2_renderer_log_every));
    {  // occlusion queries (upl-s4c)
      const std::string q = REXCVAR_GET(gow2_renderer_queries);
      queries_mode_ = q == "strict" ? QueriesMode::kStrict : q == "answer" ? QueriesMode::kAnswer :
                      q == "shadow" ? QueriesMode::kShadow : QueriesMode::kOff;
      if (queries_mode_ == QueriesMode::kStrict &&
          (!XenosOurGpuSystem() || rex::cvar::GetFlagByName("gow2_pm4_split") != "true")) {
        REXLOG_WARN("gow2_renderer: strict queries require our split GPU; keeping fake visible answers");
        queries_mode_ = QueriesMode::kOff;
      }
      if (!q.empty() && q != "off" && queries_mode_ == QueriesMode::kOff)
        REXLOG_WARN("gow2_renderer: unavailable --gow2_renderer_queries '{}' (off | shadow | answer | strict); queries off", q);
      queries_min_ = uint64_t(std::max(0, REXCVAR_GET(gow2_renderer_queries_min)));
      queries_correct_ = REXCVAR_GET(gow2_renderer_queries_correct);
      occ_answers_.SetHiddenAfter(uint32_t(std::max(1, REXCVAR_GET(gow2_renderer_queries_hidden_after))));
      queries_log_ = std::max(0, REXCVAR_GET(gow2_renderer_queries_log));
      if (queries_mode_ == QueriesMode::kStrict)
        REXLOG_INFO("gow2_renderer: occlusion queries STRICT (this issuance's actual GPU result; batched submission, "
                    "idle polling; incomplete draws fall back to visible)");
      else if (queries_mode_ != QueriesMode::kOff)
        REXLOG_INFO("gow2_renderer: occlusion queries {} (immediate answer = last count for the report, else 1; "
                    "minimum {}; correction {})",
                    queries_mode_ == QueriesMode::kAnswer ? "ANSWERED BY US" : "shadow (counted, not written)",
                    queries_min_, queries_correct_ ? "on" : "off");
    }
    {  // takeover step 1: display (notes/50)
      const std::string d = REXCVAR_GET(gow2_renderer_display);
      display_ours_ = d == "ours";
      if (!d.empty() && d != "emulator" && !display_ours_)
        REXLOG_WARN("gow2_renderer: unknown --gow2_renderer_display '{}' (emulator | ours); showing the emulator's", d);
      display_wait_ = REXCVAR_GET(gow2_renderer_display_wait);
      display_mark_ = REXCVAR_GET(gow2_renderer_display_mark);
      resolves_to_guest_ = REXCVAR_GET(gow2_renderer_resolves_to_guest);
      if (resolves_to_guest_) REXLOG_INFO("gow2_renderer: resolves to guest memory ON: our textures read our resolves (T2)");
      canary_resolve_rules_ = REXCVAR_GET(gow2_renderer_resolve_rules) == "canary";  // patch 0023
      depth_alias_drop_ = REXCVAR_GET(gow2_renderer_depth_alias_drop);              // patch 0025
      alpha_test_rt0_ = REXCVAR_GET(gow2_renderer_alpha_test_rt0);                  // patch 0027
      if (canary_resolve_rules_) REXLOG_INFO("gow2_renderer: patch 0023: colour resolves use Canary's rules");
      {  // patch 0020
        const std::string m = REXCVAR_GET(gow2_renderer_resolve_readback);
        resolve_readback_ = m == "full" ? 2u : m == "fast" ? 1u : 0u;
        if (!m.empty() && m != "none" && !resolve_readback_)
          REXLOG_WARN("gow2_renderer: unknown --gow2_renderer_resolve_readback '{}' (none | fast | full); none", m);
        prepare_mirror_ = REXCVAR_GET(gow2_renderer_resolve_prepare_mirror) || resolve_readback_ != 0;
        if (resolves_to_guest_ && (prepare_mirror_ || resolve_readback_))
          REXLOG_INFO("gow2_renderer: patch 0020 (Canary): resolve destinations prepared in the mirror {}, readback to "
                      "guest memory {}",
                      prepare_mirror_ ? "ON" : "off", resolve_readback_ == 2 ? "FULL (waits)" : resolve_readback_ ? "FAST" : "off");
      }
      if (display_ours_)
        REXLOG_INFO("gow2_renderer: display = OUR renderer's front buffer ({})",
                    display_wait_ ? "waiting for each frame, no lag" : "newest finished frame, no stall");
    }
    alias_log_limit_ = std::max(0, REXCVAR_GET(gow2_renderer_log_aliases));
    float24_remap_ = REXCVAR_GET(gow2_renderer_float24_remap);
    transfer_mixed_bpp_ = REXCVAR_GET(gow2_renderer_transfer_mixed_bpp);
    if (transfer_mixed_bpp_) REXLOG_INFO("gow2_renderer: EDRAM transfers between 32bpp and 64bpp targets on (patch 0005)");
    draw_colour_ = REXCVAR_GET(gow2_renderer_colour);
    clamp_next_base_ = REXCVAR_GET(gow2_renderer_clamp_next_base);
    {
      const std::string alt = REXCVAR_GET(gow2_renderer_alt_rule);
      alt_mode_ = alt == "always" ? 2 : 0;
    }
    ParseTileTrace(REXCVAR_GET(gow2_renderer_tile_trace), REXCVAR_GET(gow2_renderer_tile_trace_pixel));
    {
      const std::string own = REXCVAR_GET(gow2_renderer_ownership);
      canary_ownership_ = own == "canary";
      if (own != "rect" && own != "canary")
        REXLOG_WARN("gow2_renderer: unknown --gow2_renderer_ownership '{}' (rect or canary); using rect", own);
    }
    rect_gs_ = cache_->GetBuiltin("rect_list_gs", rr::builtin::kRectListGs, gpu::ShaderStage::kGeometry);
    if (cache_->Dxbc() && backend_->GetShaderIL() == gpu::ShaderIL::kDxil) {  // Vulkan: the SPIR-V rect_gs_ serves all
      const std::vector<uint8_t> gs = xenos::dxbc_gen::RectListGeometryShader();
      rect_gs_dxbc_ = backend_->CreateShader(gpu::ShaderStage::kGeometry, gs.data(), gs.size());
    }
    const gpu::ShaderHandle resolve_cs =
        cache_->GetBuiltin("depth_resolve_cs", std::string(rr::builtin::kHlslScaledChunk) + rr::builtin::kDepthResolveCs,
                           gpu::ShaderStage::kCompute);
    depth_resolve_ = resolve_cs ? backend_->CreateComputePipeline(resolve_cs) : 0;
    const gpu::ShaderHandle colour_resolve_cs =
        cache_->GetBuiltin("colour_resolve_cs", std::string(rr::builtin::kHlslScaledChunk) +
                                                    ColourShaderSource(rr::builtin::kColourResolveCs),
                           gpu::ShaderStage::kCompute);
    colour_resolve_ = colour_resolve_cs ? backend_->CreateComputePipeline(colour_resolve_cs) : 0;
    if (resolve_draws_) {  // patch 0119: the same step as a draw into the R32_UINT destination
      const gpu::ShaderHandle vs = cache_->GetBuiltin("transfer_vs", rr::builtin::kTransferVs, gpu::ShaderStage::kVertex);
      const gpu::ShaderHandle ps = cache_->GetBuiltin("colour_resolve_ps", ColourShaderSource(rr::builtin::kColourResolvePs),
                                                      gpu::ShaderStage::kPixel);
      if (vs && ps) {
        gpu::GraphicsPipelineDesc d;
        d.vs = vs;
        d.ps = ps;
        d.topology = gpu::Topology::kTriangleList;
        d.depth_clip = false;
        d.color_count = 1;
        d.color_formats[0] = gpu::Format::kR32Uint;
        d.color_write_masks[0] = 0xF;
        colour_resolve_draw_ = backend_->GetGraphicsPipeline(d);
      }
    }
    if (resolve_draws_)
      REXLOG_INFO("gow2_renderer: resolve draws (patch 0119) {}", colour_resolve_draw_ ? "on" : "OFF (shader or pipeline failed)");
    InitUpscaler();  // --gow2_upscaler (notes/64)
    if (REXCVAR_GET(gow2_renderer_msaa_colour_resolves)) {
      // Gears 3 (job 025): the same resolve reading a multisample source - c2.z samples (1, 2 or 4), host sample index
      // per selected guest sample in c2.w (4 bits each) - all decoded, then combined by XeColourResolvePixel as for
      // the reference (decode, average, 2^bias, swap, encode).
      std::string ms = rr::builtin::kColourResolveCs;
      const std::string a = "Texture2D<float4> xe_src : register(t0);";
      const std::string b =
          "  float4 v = xe_src.Load(int3(int2(full ? (c0.xy * rs + id.xy) * ss / rs : (c0.xy + id.xy) * rs + rs / 2u), 0));\n"  // 0143
          "  uint2 w = XeHostToGuestWords(v, c1.z);\n"
          "  uint4 xe_lo = uint4(w.x, 0u, 0u, 0u), xe_hi = uint4(w.y, 0u, 0u, 0u);\n"
          "  uint xe_n = 1u;\n"
          "  uint2 o = c2.x != 0u ? w : XeColourResolvePixel(c1.z, xe_lo, xe_hi, xe_n, c1.w, c2.y != 0u, false);\n";
      const size_t pa = ms.find(a), pb = ms.find(b);
      if (pa != std::string::npos && pb != std::string::npos) {
        ms.replace(pb, b.size(),
                   "  int2 sp = int2(full ? (c0.xy * rs + id.xy) * ss / rs : (c0.xy + id.xy) * rs + rs / 2u);\n"
                   "  uint n = clamp(c2.z, 1u, 4u);\n"
                   "  uint4 lo = uint4(0u, 0u, 0u, 0u), hi = uint4(0u, 0u, 0u, 0u);\n"
                   "  [unroll] for (uint k = 0u; k < 4u; ++k) {\n"
                   "    if (k < n) {\n"
                   "      uint2 ws = XeHostToGuestWords(xe_src.Load(sp, int((c2.w >> (4u * k)) & 15u)), c1.z);\n"
                   "      lo[k] = ws.x;\n"
                   "      hi[k] = ws.y;\n"
                   "    }\n"
                   "  }\n"
                   "  uint4 xe_lo = lo, xe_hi = hi;\n"
                   "  uint xe_n = n;\n"
                   "  uint2 o = c2.x != 0u ? uint2(lo.x, hi.x) : XeColourResolvePixel(c1.z, lo, hi, n, c1.w, c2.y != 0u, false);\n");
        ms.replace(ms.find(a), a.size(), "Texture2DMS<float4> xe_src : register(t0);");
        const gpu::ShaderHandle msh =
            cache_->GetBuiltin("colour_resolve_ms_cs", std::string(rr::builtin::kHlslScaledChunk) + ColourShaderSource(ms.c_str()),
                               gpu::ShaderStage::kCompute);
        colour_resolve_ms_ = msh ? backend_->CreateComputePipeline(msh) : 0;
      }
      if (!colour_resolve_ms_) REXLOG_ERROR("gow2_renderer: MSAA colour resolve shader missing; MSAA resolves skipped");
    }
    if (resolves_to_guest_) {
      const gpu::ShaderHandle wtg =
          cache_->GetBuiltin("words_to_guest_cs", rr::builtin::kWordsToGuestCs, gpu::ShaderStage::kCompute);
      words_to_guest_ = wtg ? backend_->CreateComputePipeline(wtg) : 0;
      if (!words_to_guest_) {
        REXLOG_ERROR("gow2_renderer: words-to-guest shader missing; resolves to guest memory OFF");
        resolves_to_guest_ = false;
      }
    }
    if (resolves_to_guest_ && textures_ &&
        (REXCVAR_GET(gow2_renderer_resolve_through) || REXCVAR_GET(gow2_renderer_scaled_resolve_through)) &&
        REXCVAR_GET(gow2_renderer_gpu_texture_load)) {
      const gpu::ShaderHandle wtt = cache_->GetBuiltin("words_to_guest_through_cs", rr::builtin::kWordsToGuestThroughCs,
                                                       gpu::ShaderStage::kCompute);
      words_to_guest_through_ = wtt ? backend_->CreateComputePipeline(wtt) : 0;
      REXLOG_INFO("gow2_renderer: resolve-through (job 047) {}", words_to_guest_through_ ? "ON" : "shader missing - OFF");
      through_check_frame_ = uint64_t(std::max(REXCVAR_GET(gow2_renderer_resolve_through_check), 0));
      if (through_check_frame_) {
        const gpu::ShaderHandle tc = cache_->GetBuiltin("through_compare_cs", rr::builtin::kThroughCompareCs,
                                                        gpu::ShaderStage::kCompute);
        through_compare_ = tc ? backend_->CreateComputePipeline(tc) : 0;
      }
    }
    if (REXCVAR_GET(gow2_renderer_gpu_texture_load)) {
      const gpu::ShaderHandle untile_cs = cache_->GetBuiltin("untile_cs", rr::builtin::kUntileCs, gpu::ShaderStage::kCompute);
      if (untile_cs) textures_->SetGpuUntile(backend_->CreateComputePipeline(untile_cs));
      if (untile_cs && REXCVAR_GET(gow2_renderer_gpu_load_stacked_3d)) {
        // Gears 3 job 020: the same shader writing z slice c2.z of a 3D host texture.
        std::string h3 = rr::builtin::kUntileCs;
        const std::string a = "RWTexture2D<float4> xe_dst", b = "xe_dst[id.xy] = o;";
        const size_t pa = h3.find(a), pb = h3.find(b);
        if (pa != std::string::npos && pb != std::string::npos) {
          h3.replace(h3.find(b), b.size(), "xe_dst[uint3(id.xy, c2.z)] = o;");
          h3.replace(h3.find(a), a.size(), "RWTexture3D<float4> xe_dst");
          const gpu::ShaderHandle u3 = cache_->GetBuiltin("untile3d_cs", h3, gpu::ShaderStage::kCompute);
          if (u3) textures_->SetGpuUntile3D(backend_->CreateComputePipeline(u3));
        }
      }
      if (untile_cs && REXCVAR_GET(gow2_renderer_gpu_block_textures)) {  // patch 0026: true 3D textures on the GPU
        // Same conversions as kUntileCs, one thread per texel of a 3D level, Xenos 3D tiling (texture_layout.h
        // TiledOffset3D) - the colour-grading / tone-map volumes the games resolve into every frame.
        std::string t3 = rr::builtin::kUntileCs;
        const std::string a = "RWTexture2D<float4> xe_dst", b = "xe_dst[id.xy] = o;", sw = "uint XeSwapWord(uint v, uint e) {",
                          of = "int off = c2.x != 0u ? XeTiledOffset2D(gx, gy, c0.y, bpb_log2) : (gy * int(c0.y) + gx) << int(bpb_log2);";
        if (t3.find(a) != std::string::npos && t3.find(b) != std::string::npos && t3.find(sw) != std::string::npos &&
            t3.find(of) != std::string::npos) {
          t3.replace(t3.find(of), of.size(),
                     "if (id.z >= c2.w) return;\n  int gz = int(c2.z + id.z);\n"
                     "  int off = c2.x != 0u ? XeTiledOffset3D(gx, gy, gz, c0.y, c3.x, bpb_log2)\n"
                     "                       : ((gz * int(c3.x) + gy) * int(c0.y) + gx) << int(bpb_log2);");
          t3.replace(t3.find(b), b.size(), "xe_dst[id.xyz] = o;");
          t3.replace(t3.find(sw), 0,
                     "int XeTiledOffset3D(int x, int y, int z, uint pitch, uint height, uint bpb_log2) {\n"
                     "  pitch = (pitch + 31u) & ~31u;\n  height = (height + 31u) & ~31u;\n"
                     "  int outer = ((((z >> 2) * int(height >> 4) + (y >> 4)) * int(pitch >> 5)) + (x >> 5)) << 7;\n"
                     "  int inner = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);\n"
                     "  int oib = (outer | inner) << int(bpb_log2);\n"
                     "  uint bank = uint((y >> 3) ^ (z >> 2)) & 1u;\n"
                     "  uint pipe = (uint(x >> 3) & 3u) ^ (bank << 1u);\n"
                     "  return XeTiledCombine(oib, bank, pipe, uint(y) & 1u);\n}\n");
          t3.replace(t3.find(a), a.size(), "RWTexture3D<float4> xe_dst");
          t3.replace(t3.find("[numthreads(8, 8, 1)]"), std::string("[numthreads(8, 8, 1)]").size(), "[numthreads(4, 4, 4)]");
          const gpu::ShaderHandle v3 = cache_->GetBuiltin("untile_volume_cs", t3, gpu::ShaderStage::kCompute);
          if (v3) textures_->SetGpuUntileVolume(backend_->CreateComputePipeline(v3));
        }
      }
      if (untile_cs && REXCVAR_GET(gow2_renderer_gpu_block_textures)) {  // patch 0021
        const gpu::ShaderHandle ub =
            cache_->GetBuiltin("untile_blocks_cs", rr::builtin::kUntileBlocksCs, gpu::ShaderStage::kCompute);
        const gpu::PipelineHandle up = ub ? backend_->CreateComputePipeline(ub) : 0;
        textures_->SetGpuUntileBlocks(up);
        REXLOG_INFO("gow2_renderer: patch 0021: BC textures load on the GPU from the mirror {}", up ? "ON" : "- shader missing, OFF");
      }
    }
    scaled_partial_ = REXCVAR_GET(gow2_renderer_scaled_partial);
    if (res_scale_ > 1 && REXCVAR_GET(gow2_renderer_scaled_textures) && resolves_to_guest_) {
      // Stage 7 phase B (notes/53): scaled resolve copies, textures loaded from them at the scale.
      const gpu::ShaderHandle sl =
          cache_->GetBuiltin("scaled_load_cs", rr::builtin::kScaledLoadCs, gpu::ShaderStage::kCompute);
      const gpu::PipelineHandle slp = sl ? backend_->CreateComputePipeline(sl) : 0;
      const gpu::ShaderHandle stm =
          cache_->GetBuiltin("scaled_to_mem_cs", rr::builtin::kScaledWordsToMemCs, gpu::ShaderStage::kCompute);
      scaled_to_mem_ = stm ? backend_->CreateComputePipeline(stm) : 0;
      if (REXCVAR_GET(gow2_renderer_scaled_resolve_through)) {
        const gpu::ShaderHandle st = cache_->GetBuiltin("scaled_to_mem_through_cs",
            std::string("#define XE_SCALED_THROUGH\n") + rr::builtin::kScaledWordsToMemCs, gpu::ShaderStage::kCompute);
        scaled_to_mem_through_ = st ? backend_->CreateComputePipeline(st) : 0;
        REXLOG_INFO("gow2_renderer: direct scaled texture refresh {}",
                    scaled_to_mem_through_ ? "ON (complete matching tiled levels only)" : "shader missing - OFF");
      }
      const gpu::ShaderHandle scp =
          cache_->GetBuiltin("scaled_copy_cs", rr::builtin::kScaledCopyWordsCs, gpu::ShaderStage::kCompute);
      scaled_copy_ = scp ? backend_->CreateComputePipeline(scp) : 0;
      if (slp && scaled_to_mem_ && scaled_copy_) {
        scaled_resolves_ = true;
        textures_->SetScaled(res_scale_, slp,
                             [this](uint32_t base, uint32_t extent, rr::TextureCache::ScaledSource& out) {
                               return FindScaledSource(base, extent, out);
                             });
      } else {
        REXLOG_ERROR("gow2_renderer: scaled texture load shader missing; scaled textures OFF");
      }
      REXLOG_INFO("gow2_renderer: scaled resolves and textures {}", scaled_resolves_ ? "on" : "OFF");
    }
    const gpu::ShaderHandle trace_colour_cs = cache_->GetBuiltin(
        "trace_colour_cs", ColourShaderSource(rr::builtin::kTraceColourCs), gpu::ShaderStage::kCompute);
    trace_colour_ = trace_colour_cs ? backend_->CreateComputePipeline(trace_colour_cs) : 0;
    {  // Stage 5C-1: present shadow
      const std::string p = REXCVAR_GET(gow2_present);
      present_shadow_ = p == "shadow";
      present_ours_ = p == "ours" && (presenter_ || vk_headless_present_);
      hdr_ = REXCVAR_GET(present_hdr);
      if (hdr_) {
        const gpu::ShaderHandle he =
            cache_->GetBuiltin("hdr_encode_cs", rr::builtin::kHdrEncodeCs, gpu::ShaderStage::kCompute);
        hdr_encode_ = he ? backend_->CreateComputePipeline(he) : 0;
        REXLOG_INFO("gow2_present: HDR10 output on (paper white {} nits, peak {}, knee {}, expansion {}){}",
                    REXCVAR_GET(gow2_hdr_paper_white), REXCVAR_GET(gow2_hdr_peak), REXCVAR_GET(gow2_hdr_knee),
                    REXCVAR_GET(gow2_hdr_expand), hdr_encode_ ? "" : " - ENCODE SHADER MISSING");
      }
      const std::string aa = REXCVAR_GET(gow2_present_aa);
      smaa_ = aa == "smaa";
      if (!aa.empty() && aa != "off" && aa != "smaa") REXLOG_WARN("gow2_present: unknown aa '{}' (off | smaa); off", aa);
      if (!p.empty() && p != "off" && p != "shadow" && p != "ours")
        REXLOG_WARN("gow2_present: unknown mode '{}' (off | shadow | ours); off", p);
      if ((present_shadow_ || present_ours_) && !display_ours_) {
        REXLOG_WARN("gow2_present: needs --gow2_renderer_display=ours (our front buffer); off");
        present_shadow_ = present_ours_ = false;
      }
      if (present_ours_) {
        const gpu::ShaderHandle pu =
            cache_->GetBuiltin("present_unpack_cs", rr::builtin::kPresentUnpackCs, gpu::ShaderStage::kCompute);
        present_unpack_ = pu ? backend_->CreateComputePipeline(pu) : 0;
        if (!present_unpack_) {
          REXLOG_ERROR("gow2_present: unpack shader missing; ours OFF");
          present_ours_ = false;
        }
        if (present_ours_ && REXCVAR_GET(gow2_present_native_size)) {  // "Match my monitor"
          const gpu::ShaderHandle rs =
              cache_->GetBuiltin("present_resample_cs", rr::builtin::kPresentResampleCs, gpu::ShaderStage::kCompute);
          present_resample_ = rs ? backend_->CreateComputePipeline(rs) : 0;
          REXLOG_INFO("gow2_present: native size on - the image is resampled to the window's size{}",
                      present_resample_ ? "" : " - RESAMPLE SHADER MISSING (shown as before)");
          present_lanczos_ = REXCVAR_GET(gow2_present_upscale_filter) == "lanczos";
        }
        if (present_ours_ && REXCVAR_GET(gow2_present_sharpen) > 0.0) {   // 0.2.0
          const gpu::ShaderHandle sh = cache_->GetBuiltin("sharpen_cs", rr::builtin::kSharpenCs, gpu::ShaderStage::kCompute);
          present_sharpen_ = sh ? backend_->CreateComputePipeline(sh) : 0;
          REXLOG_INFO("gow2_present: sharpening {:.2f}{}{}", REXCVAR_GET(gow2_present_sharpen),
                      present_lanczos_ ? ", lanczos enlarging" : "", present_sharpen_ ? "" : " - SHADER MISSING (off)");
        }
      }
      if (present_shadow_ || present_ours_) {
        const gpu::ShaderHandle pg =
            cache_->GetBuiltin("present_gamma_cs", rr::builtin::kPresentGammaCs, gpu::ShaderStage::kCompute);
        present_gamma_ = pg ? backend_->CreateComputePipeline(pg) : 0;
        gpu::TextureDesc rd;
        rd.width = 256 + 128 * 3;
        rd.format = gpu::Format::kR32Uint;
        rd.debug_name = "gamma ramps";
        ramp_texture_ = backend_->CreateTexture(rd);
        if (!present_gamma_ || !ramp_texture_) {
          REXLOG_ERROR("gow2_present: gamma shader or ramp texture missing; present OFF");
          present_shadow_ = present_ours_ = false;
        } else if (present_ours_) {
          REXLOG_INFO("gow2_present: OURS - our gamma image goes straight into the presenter ({}); the front buffer is "
                      "not written to guest memory", backend_->Describe());
        } else {
          REXLOG_INFO("gow2_present: SHADOW - our gamma pass on our front buffer, compared with the SDK's image");
        }
      }
    }
    if (REXCVAR_GET(gow2_renderer_transfers)) {
      transfer_vs_ = cache_->GetBuiltin("transfer_vs", rr::builtin::kTransferVs, gpu::ShaderStage::kVertex);
      transfer_ps_ = cache_->GetBuiltin("transfer_ps", ColourShaderSource(rr::builtin::kTransferPs),
                                        gpu::ShaderStage::kPixel);
      colour_transfer_ps_ =
          cache_->GetBuiltin("colour_transfer_ps", rr::builtin::kColourTransferPs, gpu::ShaderStage::kPixel);
      if (REXCVAR_GET(gow2_renderer_msaa_transfers)) {
        // Gears 3 (job 030): 1x -> 4x MSAA colour transfer, run per sample. Destination 4x pixel p, host sample s ->
        // its EDRAM sample 2p + (s & 1, s >> 1) (4x stores 2x2 samples per pixel; a 32bpp tile is 80 x 16 samples) ->
        // the source 1x pixel at that EDRAM position. Host sample s is taken as quadrant (s & 1, s >> 1): with
        // D3D12's standard 4x positions {-2,-6},{6,-2},{-6,2},{2,6} that is each sample's own quadrant (GUESS for the
        // EDRAM sample order; a resolve that averages all 4 does not depend on it).
        msaa_up_transfer_ps_ = cache_->GetBuiltin("msaa_up_transfer_ps", R"(
cbuffer XeTransfer : register(b0, space0) { uint4 t0; uint4 t1; uint4 t2; uint4 t3; };
Texture2D<float4> xe_src : register(t0, space0);
float4 main(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target0 {
  uint rs = max(t3.x, 1u);
  uint2 hp = uint2(pos.xy);
  uint2 p = hp / rs, sub = hp % rs;
  uint rss = t3.y != 0u ? t3.y : rs;  // the source target's scale (per-target scale threshold)
  uint2 smp = p * 2u + uint2(s & 1u, s >> 1u);
  uint tile = (t0.x + (smp.y / 16u) * t0.y + smp.x / 80u) % 2048u;
  uint rel = (tile + 2048u - t0.z) % 2048u;
  uint2 q = uint2((rel % t0.w) * 80u + smp.x % 80u, (rel / t0.w) * 16u + smp.y % 16u) * rss + (sub * rss) / rs;
  return xe_src.Load(int3(q, 0));
}
)", gpu::ShaderStage::kPixel);
      }
      word_transfer_ps_ = cache_->GetBuiltin("word_transfer_ps", ColourShaderSource(rr::builtin::kWordTransferPs),
                                             gpu::ShaderStage::kPixel);
      transfer_bits_ = REXCVAR_GET(gow2_renderer_transfer_bits);
      if (transfer_bits_) {
        word_transfer_bits_ps_ = cache_->GetBuiltin(
            "word_transfer_bits_ps", "#define XE_BITS_OUT 1\n" + ColourShaderSource(rr::builtin::kWordTransferPs),
            gpu::ShaderStage::kPixel);
        colour_transfer_bits_ps_ = cache_->GetBuiltin(
            "colour_transfer_bits_ps", std::string("#define XE_BITS_OUT 1\n") + rr::builtin::kColourTransferPs,
            gpu::ShaderStage::kPixel);
        if (!word_transfer_bits_ps_ || !colour_transfer_bits_ps_) transfer_bits_ = false;
      }
      if (backend_->SupportsStencilRefExport())
        transfer_ps_ref_ = cache_->GetBuiltin(
            "transfer_ps_ref", "#define XE_STENCIL_REF 1\n" + ColourShaderSource(rr::builtin::kTransferPs),
            gpu::ShaderStage::kPixel);
    }
    BeginTraceFrame();
    OpenPipelineList();  // patch 0058: read the recorded pipelines, queue them to the workers, then record new ones
    REXLOG_INFO("gow2_renderer: our renderer on: {} | {} | shader back end {} | cache {} | rect-list GS {} | depth resolve {} | clear pair "
                "{:016X}/{:016X}, no-op point VS {:016X}",
                backend_->Describe(), cache_->CompilerDescription(), cache_->Dxbc() ? (rect_gs_dxbc_ ? "dxbc" : "dxbc (DXBC GS MISSING)") : "hlsl",
                dir.string(), rect_gs_ ? "ok" : "MISSING", depth_resolve_ ? "ok" : "MISSING", hints_.clear_vs,
                hints_.clear_ps, hints_.dummy_point_vs);
    return true;
  }

  void OnDrawShaders(const Shader* vs, const Shader* ps) override {
    vs_ = vs ? XenosUcode{vs->ucode_data_hash(), vs->ucode_dwords(), uint32_t(vs->ucode_dword_count())} : XenosUcode{};
    ps_ = ps ? XenosUcode{ps->ucode_data_hash(), ps->ucode_dwords(), uint32_t(ps->ucode_dword_count())} : XenosUcode{};
  }
  // Stage 5B phase 3: the ucode straight from our PM4 processor (same hash as the SDK's Shader objects).
  void OnDrawUcode(const XenosUcode& vs, const XenosUcode& ps) override {
    // Patch 0053: shaders the PM4 processor has just met for the first time start compiling now, while this thread is
    // still on earlier draws (the processor parses ahead of us).
    if (shader_prefetch_ && cache_ && XenosNewUcodePending()) {
      stats_.prefetched += XenosTakeNewUcode(
          [](void* ctx, const XenosUcode& v, const XenosUcode& p) {
            auto* self = static_cast<ShadowRenderer*>(ctx);
            if (v.dwords && v.count) self->cache_->Get(v.hash, v.dwords, v.count, true, 0);
            if (p.dwords && p.count) self->cache_->Get(p.hash, p.dwords, p.count, false);
          },
          this);
    }
    vs_ = vs;
    ps_ = ps;
  }

  void OnDraw(const RegisterFile& regs, gx::PrimitiveType primitive, uint32_t index_count,
              const ObservedIndexBuffer* index_buffer, bool major_mode_explicit) override {
    if (!backend_ || backend_->IsLost()) return;
    const uint64_t t0 = __rdtsc();
    fill_.valid = false;  // job 185
    if (queries_mode_ == QueriesMode::kStrict) {
      // Query data is useful before the post-processing/present work after it.
      // Give a completed query batch its own fence instead of making UE3 wait
      // for every remaining GPU command in the frame. Bound extra submissions.
      if (occ_need_submit_ && !occ_open_.open && occ_closed_in_batch_ && occ_batches_this_frame_ < 4) {
        backend_->SubmitEarly();
        occ_need_submit_ = false;
        occ_closed_in_batch_ = 0;
        ++occ_batches_this_frame_;
      }
      if (!occ_pending_.empty() && (draw_index_ & 31u) == 0) PollOcclusion();
    }
    BeginParts();
    {
      PartScope ps(*this, kPartPoll);
      cache_->Poll();
    }
    const uint32_t* r = regs.values;
    if (Bits(r[rr::reg::RB_MODECONTROL], 0, 3) == 6) {  // kCopy
      PartScope ps(*this, kPartResolve);
      Resolve(r);
    } else {
      PartScope ps(*this, kPartState);
      Draw(r, uint32_t(primitive), index_count, index_buffer, major_mode_explicit);
      ++draw_index_;
      // Patch 0138: submit part of the frame early so the GPU starts while we record the rest (NVIDIA: 5-10 submits
      // per frame). Off (0) by default until measured; N = after every N draws.
      if (submit_draws_ && ++draws_since_submit_ >= submit_draws_) {
        draws_since_submit_ = 0;
        backend_->SubmitEarly();
      }
    }
    EndParts();
    const double ms = double(__rdtsc() - t0) * MsPerTick();
    cpu_ms_ += ms;
    stats_.cpu_ms += ms;
  }

  // Stage 4 (upl-s4c): every EVENT_WRITE_ZPD, after the SDK wrote its own (fake) answer (SDK patch 15).
  void OnZpd(const RegisterFile&, const rex::graphics::ObservedZpd& zpd) override {
    if (queries_mode_ == QueriesMode::kOff || !backend_ || backend_->IsLost()) return;
    const uint64_t t0 = __rdtsc();
    const bool strict = queries_mode_ == QueriesMode::kStrict;
    const bool answer = queries_mode_ == QueriesMode::kAnswer || strict;
    if (strict) PollOcclusion();
    uint8_t* report = zpd.report_address ? memory_->TranslatePhysical<uint8_t*>(zpd.report_address) : nullptr;
    if (!zpd.is_end) {
      if (occ_open_.open) {  // the previous query never got its end
        ++occ_stats_.unended;
        if (occ_open_.handle) backend_->ReleaseOcclusionQuery(occ_open_.handle);
      }
      occ_open_ = OpenOcclusion{zpd.report_address, backend_->BeginOcclusionQuery(), stats_.draws, stats_.drawn, true,
                                rts_ ? rts_->PhaseScale() : res_scale_};  // patch 0141: the scale at its begin
      if (!occ_open_.handle) ++occ_stats_.refused;
      // Our answer is this query's own count, so the begin snapshot the game subtracts must be 0 (the SDK leaves the
      // begin report as it was: job 113 saw stale memory there).
      if (answer && report) std::memset(report, 0, rr::queries::kReportBytes);
      cpu_ms_ += double(__rdtsc() - t0) * MsPerTick();
      return;
    }
    ++occ_stats_.ends;
    if (zpd.answer == rex::graphics::ZpdAnswer::kFake) ++occ_stats_.sdk_fake;
    if (zpd.answer == rex::graphics::ZpdAnswer::kHost) ++occ_stats_.sdk_host;
    const OpenOcclusion q = occ_open_;
    occ_open_ = OpenOcclusion{};
    // Complete = every draw inside the query was really drawn by us (a draw skipped for a pending shader/pipeline
    // would make our count too small: such a count is never learned, never written).
    const bool counted = q.open && stats_.draws >= q.draws && stats_.drawn >= q.drawn;
    const uint64_t attempted = counted ? stats_.draws - q.draws : 0, drawn = counted ? stats_.drawn - q.drawn : 0;
    const bool complete = q.open && q.handle && counted && attempted == drawn;
    if (q.handle) {
      backend_->EndOcclusionQuery(q.handle);
      ++occ_stats_.ours;
      if (!complete) ++occ_stats_.incomplete;
    }
    if (strict) {
      const uint64_t sequence = occ_deferred_.Issue(zpd.report_address);
      if (complete) {
        PendingOcclusion pending;
        pending.end_address = zpd.report_address;
        pending.handle = q.handle;
        pending.frame = frame_;
        pending.sequence = sequence;
        pending.complete = true;
        pending.draws = uint32_t(attempted);
        pending.scale = q.scale ? q.scale : res_scale_;
        pending.queued_at = std::chrono::steady_clock::now();
        occ_pending_.push_back(pending);
        occ_need_submit_ = true;
        ++occ_closed_in_batch_;
      } else {
        // A missing draw or failed query must not incorrectly hide an object.
        if (occ_deferred_.Complete(zpd.report_address, sequence, report, 1)) {
          ++occ_stats_.answered;
          ++occ_stats_.fallback;
          XenosNotifyGuestWrite();
        }
        if (q.handle) backend_->ReleaseOcclusionQuery(q.handle);
      }
      cpu_ms_ += double(__rdtsc() - t0) * MsPerTick();
      return;
    }
    bool from_cache = false;
    const uint64_t estimate =
        occ_answers_.Estimate(rr::queries::AnswerCache::Key(zpd.report_address, uint32_t(attempted)), 1, queries_min_,
                              &from_cache);
    uint64_t sequence = 0;
    if (answer && report) {
      rr::queries::SampleCounts::WriteAnswer(report, estimate, false);  // SDK layout, ZPass/StencilFail lanes last
      sequence = occ_answers_.NoteWritten(zpd.report_address, estimate);
      if (estimate == 0) ++occ_stats_.answered_hidden;
      ++occ_stats_.answered;
      ++(from_cache ? occ_stats_.from_cache : occ_stats_.fallback);
    }
    if (q.handle) {
      const bool log = queries_log_ > 0 && (frame_ + 1) % uint64_t(log_every_) == 0 && occ_lines_ < uint32_t(queries_log_);
      if (log) ++occ_lines_;
      occ_pending_.push_back(PendingOcclusion{zpd.report_address, q.handle, frame_, sequence, estimate, complete,
                                              uint32_t(attempted), log, from_cache, q.scale ? q.scale : res_scale_});  // 0149: q, not the reset occ_open_
    }
    cpu_ms_ += double(__rdtsc() - t0) * MsPerTick();
  }

  // Our counts that have landed (never waits): learn them and, when answering, correct reports still holding our
  // estimate.
  void PollOcclusion() {
    if (queries_mode_ == QueriesMode::kStrict) {
      bool completed_reports = false;
      while (!occ_pending_.empty()) {
        const PendingOcclusion& p = occ_pending_.front();
        uint64_t samples = 0;
        const bool ready = !backend_->IsLost() && backend_->GetOcclusionResult(p.handle, &samples);
        if (!ready && !backend_->IsLost() &&
            std::chrono::steady_clock::now() - p.queued_at < std::chrono::seconds(2)) break;
        if (ready) {
          samples = rr::queries::AnswerCache::ToGuestSamples(samples, p.scale * p.scale);
          ++occ_stats_.results;
          if (!samples) ++occ_stats_.zero;
          occ_stats_.samples += samples;
          occ_stats_.latency_frames += frame_ - p.frame;
        } else {
          samples = 1;  // Device/query failure: preserve visibility and release the guest wait.
          ++occ_stats_.dropped;
          ++occ_stats_.fallback;
        }
        auto* report = memory_->TranslatePhysical<uint8_t*>(p.end_address);
        if (occ_deferred_.Complete(p.end_address, p.sequence, report, samples)) {
          ++occ_stats_.answered;
          if (!samples) ++occ_stats_.answered_hidden;
          completed_reports = true;
        } else ++occ_stats_.too_late;
        backend_->ReleaseOcclusionQuery(p.handle);
        occ_pending_.pop_front();
      }
      if (completed_reports) {
        // Every completed report is already visible. Publish one sequence change
        // and wake after the batch, instead of waking again for each report.
        XenosNotifyGuestWrite();
        ++occ_stats_.completion_notifications;
      }
      return;
    }
    const bool answer = queries_mode_ == QueriesMode::kAnswer;
    while (!occ_pending_.empty()) {
      PendingOcclusion& p = occ_pending_.front();
      uint64_t n = 0;
      if (!backend_->GetOcclusionResult(p.handle, &n)) {
        // Results land in order, so when the next one has landed this one never will (broken / lost): drop it now
        // rather than holding every later query for 8 frames (patch 0149, deep-dive review).
        uint64_t next = 0;
        const bool later_landed = occ_pending_.size() > 1 && backend_->GetOcclusionResult(occ_pending_[1].handle, &next);
        if (!later_landed && frame_ < p.frame + 8) break;  // give a lost one 8 frames
        if (!later_landed || !backend_->GetOcclusionResult(p.handle, &n)) {  // (review: it may have landed meanwhile)
          ++occ_stats_.dropped;
          backend_->ReleaseOcclusionQuery(p.handle);
          occ_pending_.pop_front();
          continue;
        }
      }
      n = rr::queries::AnswerCache::ToGuestSamples(n, p.scale * p.scale);  // host samples -> guest samples (0141: its draws' scale)
      ++occ_stats_.results;
      if (!n) ++occ_stats_.zero;
      occ_stats_.samples += n;
      occ_stats_.latency_frames += frame_ - p.frame;
      bool corrected = false;
      if (p.complete) {
        const uint64_t key = rr::queries::AnswerCache::Key(p.end_address, p.draws);
        occ_answers_.Learn(key, n);
        if (n == 0 && !occ_answers_.TrustZero(key)) ++occ_stats_.zero_not_trusted;
        if (answer && queries_correct_ && p.sequence && n != p.estimate && (n != 0 || occ_answers_.TrustZero(key))) {
          uint8_t* report = memory_->TranslatePhysical<uint8_t*>(p.end_address);
          if (occ_answers_.StillOurs(p.end_address, p.sequence, rr::queries::SampleCounts::Read(report))) {
            rr::queries::SampleCounts::WriteAnswer(report, n, false);
            occ_answers_.NoteWritten(p.end_address, n);
            ++occ_stats_.corrected;
            corrected = true;
          } else {
            ++occ_stats_.too_late;
          }
        }
        if ((p.estimate == 0) != (n == 0)) ++occ_stats_.visibility_changed;
        if (p.estimate == 0 && n != 0) ++occ_stats_.wrong_hidden;  // answered hidden, was visible: a vanished object
      }
      if (p.log)
        REXLOG_INFO("gow2_renderer: query frame {} end {:08X} draws {}{} | answered {} ({}) | our count {} (after {} "
                    "frames){}",
                    p.frame + 1, p.end_address, p.draws, p.complete ? "" : " INCOMPLETE (a draw was not drawn)",
                    p.estimate, p.from_cache ? "last count" : "no count yet", n, frame_ - p.frame,
                    corrected ? " -> corrected" : "");
      backend_->ReleaseOcclusionQuery(p.handle);
      occ_pending_.pop_front();
    }
  }

  bool StrictQueries() const { return queries_mode_ == QueriesMode::kStrict; }
  void* IdleGpuEvent() {
    return StrictQueries() && !occ_pending_.empty()
        ? backend_->OcclusionCompletionEvent(occ_pending_.front().handle) : nullptr;
  }
  bool PollGpuIdle() {
    COGBOX_TRACY_SCOPE("Poll idle GPU results");
    IdleSubmit();
    if (!StrictQueries() || occ_pending_.empty()) return false;
    const uint64_t t0 = __rdtsc();
    if (occ_need_submit_ && !backend_->IsLost()) {
      backend_->SubmitEarly();  // Batch all queued query resolves; never one GPU wait per query.
      occ_need_submit_ = false;
      occ_closed_in_batch_ = 0;
    }
    PollOcclusion();
    cpu_ms_ += double(__rdtsc() - t0) * MsPerTick();
    return !occ_pending_.empty();
  }

  // 2026-10-29 (Judgment Tracy capture): with strict queries the game waits for its GPU results every frame, so after
  // each result the GPU had nothing of ours until this frame's first submit, 2-3 ms later. The renderer thread is out
  // of work here (its queue is empty): give the GPU what this frame has recorded so far.
  void IdleSubmit() {
    if (!backend_ || backend_->IsLost()) return;
    const int32_t v = REXCVAR_GET(gow2_renderer_idle_submit);
    const uint32_t min_draws = v < 0 ? (StrictQueries() ? 32u : 0u) : uint32_t(v);
    if (!min_draws || idle_submits_frame_ >= kIdleSubmitsPerFrame || draw_index_ < idle_submit_draw_ + min_draws) return;
    if (occ_open_.open) return;  // never split an open occlusion query across lists (as the strict batches don't)
    backend_->SubmitEarly();
    idle_submit_draw_ = draw_index_;
    ++idle_submits_frame_;
  }
  static constexpr uint32_t kIdleSubmitsPerFrame = 4;
  uint32_t idle_submit_draw_ = 0, idle_submits_frame_ = 0, idle_submits_last_frame_ = 0;

  template<class F> void FrameStep(const char* site, F&& work) {
    COGBOX_TRACY_SCOPE("Renderer frame step");
    COGBOX_TRACY_NAME(site);
    const uint64_t start = __rdtsc();
    work();
    const uint64_t ticks = __rdtsc() - start;
    const double ms = double(ticks) * MsPerTick();
    if (XenosFrameLogOn()) {
      for (auto& step : frame_steps_) {
        if (!step.site) step.site = site;
        if (step.site == site) { step.ticks += ticks; break; }
      }
    }
    if (ms >= 20.0 && XenosFrameLogOn())
      REXLOG_INFO("gow2_renderer: frame {} slow step {}: {:.1f} ms", frame_, site, ms);
  }
  static uint64_t ThreadCpuTicks() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
      return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
             (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
#endif
    return 0;
  }
  void OnSwap(const RegisterFile& regs, uint32_t frontbuffer_ptr, uint32_t fb_width, uint32_t fb_height) override {
    COGBOX_TRACY_SCOPE("Renderer swap");
    if (!backend_) return;
    if (!thread_cpu_base_) thread_cpu_base_ = ThreadCpuTicks();
#if defined(GN_PGO_GEN)
    {  // patch 0159: the training run is ended from outside (no normal exit), so the counters are written here
      static uint32_t pgo_swaps = 0;
      const int32_t at = REXCVAR_GET(gow2_pgo_dump_frame);
      if (at > 0 && ++pgo_swaps == uint32_t(at)) {
        const int r = __llvm_profile_write_file();
        REXLOG_INFO("gow2_pgo: execution counters written at swap {} (result {})", pgo_swaps, r);
      }
    }
#endif
    part_level_ = std::clamp(REXCVAR_GET(gow2_renderer_cpu_split), 0, 2);  // patch 0153
    if (part_level_ == 2 && part_level_prev_ != 2 && backend_) part_upload_mark_ = backend_->Stats().upload_bytes;
    part_level_prev_ = part_level_;
    const uint64_t t0 = __rdtsc();
    BeginParts();
    PartScope frame_scope(*this, kPartFrame);
    if (!vertex_log_.empty()) FrameStep("vertex recheck", [&] { RecheckDrawVertices(); });
    FrameStep("export readback results", [&] { PollExportReadbacks(); });
    if (!through_checks_.empty()) FrameStep("through checks", [&] { PollThroughChecks(); });
    if (!resolve_readback_retire_.empty()) FrameStep("resolve readback retirement", [&] { RetireResolveReadbacks(); });
    {
      PartScope ps(*this, kPartPoll);
      cache_->Poll();
    }
    if (stats_frame_ && frame_ == uint64_t(stats_frame_)) {  // patch 0121: the last targets of the frame
      TargetStats(stats_prev_);
      stats_prev_.clear();
      REXLOG_INFO("gow2_renderer: target stats (patch 0121) frame {} done: {} draws ({} with no colour target, patch 0123)",
                  frame_, stats_draws_, stats_no_colour_);
    }
    ++frame_;
    if (report_first_) ReportFrame();  // patch 0101
    FrameStep("created shaders", [&] { DrainCreatedShaders(); });  // patch 0066
    // Patch 0068's DrainPrebuild (the whole incoming batch, every frame, here) is gone in prebuild v2: package shaders go
    // straight to the prebuild thread. This thread only keeps the warm-only worker count in step with load / play.
    if (prebuild_on_) GovernWarmWorkers();
    // Patch 0062: the on-screen "Building shaders" note (fps_bar.cpp) - pipelines still queued or being built, and how
    // many were ever queued (both games; no game data).
    // Prebuild v2: and the shaders the prebuild still has to build, so a background compile shows too.
    XenosSetShaderJobs(backend_->PipelineJobsLeft() + (prebuild_on_ ? PrebuildLeft() : 0),
                       backend_->Stats().pipelines_async + (prebuild_on_ ? prebuild_accepted_.load() : 0));
    if (upscale_probe_ && frame_ % upscale_probe_ == 0) LogProbeFrame();
    FrameStep("upscaler end frame", [&] { UpscaleEndFrame(); });
    if (textures_) FrameStep("texture end frame", [&] { textures_->EndFrame(frame_); });
    FrameStep("destination eviction", [&] { EvictDestinations(); });  // patch 0032
    std::string lost;
    if (backend_->IsLost(&lost)) {
      if (!lost_logged_) REXLOG_ERROR("gow2_renderer: our device is lost ({}); shadow renderer stopped", lost);
      lost_logged_ = true;
      return;
    }
    if (!selftest_started_) StartTransferSelfTest();
    DecideLoadHold();  // patch 0063
    if (display_ours_) FrameStep("present image", [&] { RequestDisplay(regs, frontbuffer_ptr & 0x1FFFFFFFu, fb_width, fb_height); });
    if (!export_ranges_.empty()) FrameStep("export readback copies", [&] { IssueExportReadbacks(); });
    FrameStep("submit", [&] { frame_fences_.push_back(backend_->Submit()); });
    occ_need_submit_ = false;
    occ_closed_in_batch_ = 0;
    occ_batches_this_frame_ = 0;
    XenosLatencyFrameSubmitted();  // patch 0135: Reflex RENDERSUBMIT_END
    FrameStep("next frame", [&] { backend_->NextFrame(); });
    FrameStep("frame throttle", [&] {
      while (frame_fences_.size() > frames_in_flight_) {
        backend_->WaitForFence(frame_fences_.front());
        frame_fences_.pop_front();
      }
    });
    if (queries_mode_ != QueriesMode::kOff) FrameStep("occlusion results", [&] { PollOcclusion(); });  // upl-s4c
    double fl_hold_ms = 0.0;  // frame log
    if (hold_this_frame_) {
      const auto th = std::chrono::steady_clock::now();
      HoldForPipelines();  // patch 0063
      fl_hold_ms = MsSince(th);
    }
    hold_descs_.clear();
    hold_keys_.clear();
    if (display_ours_ && !present_ours_) ShowDisplay(frontbuffer_ptr & 0x1FFFFFFFu);
    if (present_ours_) FrameStep("deliver image", [&] { DeliverFrameImages(); });
    {
      PartScope ps(*this, kPartCompare);
      EndTraceFrame();
      FlushTraces(false);
      CheckTransferSelfTest();
    }
    const double fl_pipe_wait_ms = pipeline_wait_ms_frame_;  // frame log: this frame's, before the reset below
    const uint32_t fl_draws = uint32_t(draw_index_), fl_skipped = uint32_t(pending_this_frame_);
    draw_index_ = 0;
    idle_submits_last_frame_ = idle_submits_frame_;  // for the frame log (recorded after this reset)
    idle_submit_draw_ = idle_submits_frame_ = 0;
    copy_index_ = 0;
    occ_lines_ = 0;
    pending_this_frame_ = shader_errors_this_frame_ = pipeline_failures_this_frame_ = 0;
    pipeline_wait_ms_frame_ = 0.0;
    frames_in_flight_ = size_t(std::clamp(REXCVAR_GET(gow2_renderer_frames_in_flight), 1, 3));  // patch 0133
    submit_draws_ = uint32_t(std::max(0, REXCVAR_GET(gow2_renderer_submit_draws)));  // patch 0138
    draws_since_submit_ = 0;
    depth_wait_left_ms_ = std::max(0.0, REXCVAR_GET(gow2_renderer_depth_pipeline_wait_ms));  // patch 0132
    shader_wait_left_ms_ = shader_wait_budget_ms_;
    standin_wait_left_ms_ = standin_wait_budget_ms_;  // patch 0045
    last_copy_draw_ = 0;
    drawn_since_copy_ = 0;
    EndParts();
    const double ms = cpu_ms_ + double(__rdtsc() - t0) * MsPerTick();
    cpu_ms_ = 0;
    cpu_sum_ += ms;
    cpu_max_ = std::max(cpu_max_, ms);
    if (XenosFrameLogOn()) FrameLogRecord(ms, fl_hold_ms, fl_pipe_wait_ms, fl_draws, fl_skipped);  // before 0010
    ::xenos::profiling::Frame(ms, backend_->Stats().gpu_ms);
    ReportHitch(ms);  // patch 0010
    if (frame_ % uint64_t(log_every_) == 0) {
      if (depth_alias_drop_ || memexport_)  // patches 0024 / 0025
        REXLOG_INFO("gow2_renderer: patch 0024/0025 frames {}-{}: depth bindings dropped (shared colour base) {} | "
                    "exporting draws that waited for their shaders {}",
                    frame_ + 1 - uint64_t(log_every_), frame_, stats_.depth_alias_dropped, stats_.export_waits);
      if (textures_ && REXCVAR_GET(gow2_renderer_gpu_block_textures)) {  // patch 0021
        const rr::TextureCacheStats& ts = textures_->Stats();
        REXLOG_INFO("gow2_renderer: patch 0021 frames {}-{}: BC textures loaded on the GPU {} ({} subresources, {} "
                    "failed) | 3D textures on the GPU {} (0026) | CPU-path loads {} | scratch copies so far {} (batched loads: in "
                    "{} batches, untile barriers left out {})",
                    frame_ + 1 - uint64_t(log_every_), frame_, ts.gpu_block_loads, ts.gpu_block_subresources,
                    ts.gpu_block_failures, ts.gpu_volume_loads, ts.cpu_loads, backend_->Stats().scratch_texture_copies,
                    backend_->Stats().scratch_batches, backend_->Stats().uav_barriers_deferred);
      }
      if (submit_draws_) {  // patch 0138
        const uint64_t es = backend_->Stats().early_submits;
        REXLOG_INFO("gow2_renderer: patch 0138 frames {}-{}: early submits {:.1f} per frame (every {} draws)",
                    frame_ + 1 - uint64_t(log_every_), frame_, double(es - early_prev_) / double(log_every_), submit_draws_);
        early_prev_ = es;
      }
      if (textures_) {  // patch 0137: texture binding work per frame over this window (frame-rate research)
        const rr::TextureCacheStats& ts = textures_->Stats();
        const double n = double(log_every_);
        // Patch 0143 fix: the cache's counters can be reset (ResetCounters) - then the window counts from 0.
        auto d = [](uint64_t cur, uint64_t prev) { return double(cur >= prev ? cur - prev : cur); };
        REXLOG_INFO("gow2_renderer: patch 0137 frames {}-{}: texture binding per frame: slots from the slot cache {:.0f}, "
                    "looked up {:.0f} | lookups {:.0f}: no write since {:.0f}, pages unchanged {:.0f} (pages walked {:.0f}), "
                    "bytes re-checked {:.0f} | watch changes seen {:.1f} | cached textures {}",
                    frame_ + 1 - uint64_t(log_every_), frame_, d(ts.slot_hits, tex_prev_.slot_hits) / n,
                    d(ts.slot_misses, tex_prev_.slot_misses) / n, d(ts.lookups, tex_prev_.lookups) / n,
                    d(ts.lookup_epoch_hits, tex_prev_.lookup_epoch_hits) / n,
                    d(ts.lookup_version_hits, tex_prev_.lookup_version_hits) / n,
                    d(ts.version_pages, tex_prev_.version_pages) / n, d(ts.lookup_rechecks, tex_prev_.lookup_rechecks) / n,
                    d(ts.epoch_changes, tex_prev_.epoch_changes) / n, ts.textures);
        tex_prev_.slot_hits = ts.slot_hits; tex_prev_.slot_misses = ts.slot_misses; tex_prev_.lookups = ts.lookups;
        tex_prev_.lookup_epoch_hits = ts.lookup_epoch_hits; tex_prev_.lookup_version_hits = ts.lookup_version_hits;
        tex_prev_.version_pages = ts.version_pages; tex_prev_.lookup_rechecks = ts.lookup_rechecks;
        tex_prev_.epoch_changes = ts.epoch_changes;
      }
      if (resolves_to_guest_ && textures_) {
        const rr::TextureCacheStats& ts = textures_->Stats();
        REXLOG_INFO("gow2_renderer: resolves to guest (T2) frames {}-{}: our resolves written {} ({:.1f} MB), skipped "
                    "{} (format {}, endian {}) | resolves seen {} (colour {}, depth {}) | texture pages from our resolves kept {} / "
                    "counted changed {} (this range) | CPU-path loads that read our pages {} (this range, should be 0)",
                    frame_ + 1 - uint64_t(log_every_), frame_, t2_stats_.written, double(t2_stats_.bytes) / 1048576.0,
                    t2_stats_.skipped, t2_stats_.skipped_format, t2_stats_.skipped_endian,
                    t2_stats_.seen_colour + t2_stats_.seen_depth, t2_stats_.seen_colour,
                    t2_stats_.seen_depth, ts.gpu_pages_kept, ts.gpu_pages_changed,
                    ts.cpu_loads_of_gpu_pages);
        {  // patch 0032: resolve destination textures and video memory
          uint64_t one_bytes = 0, scaled_bytes = 0;
          for (const auto& [k, d] : destinations_) one_bytes += d.bytes;
          for (const auto& [k, d] : scaled_dests_) scaled_bytes += d.bytes;
          uint64_t vram_used = 0, vram_budget = 0;
          const bool vram = backend_->VideoMemory(&vram_used, &vram_budget);
          REXLOG_INFO("gow2_renderer: patch 0032 frames {}-{}: resolve destinations {} ({:.1f} MB), scaled {} "
                      "({:.1f} MB); freed {} ({:.1f} MB) by age {}, by budget {} (video memory pressure {}x); reused {} "
                      "unscaled / {} scaled (this range){}",
                      frame_ + 1 - uint64_t(log_every_), frame_, destinations_.size(), double(one_bytes) / 1048576.0,
                      scaled_dests_.size(), double(scaled_bytes) / 1048576.0, dest_evict_.count,
                      double(dest_evict_.bytes) / 1048576.0, dest_evict_.by_age, dest_evict_.by_budget, dest_evict_.pressure_events,
                      dest_evict_.reused_one, dest_evict_.reused_scaled,
                      vram ? fmt::format(" | video memory used {:.0f} MB of budget {:.0f} MB{}",
                                         double(vram_used) / 1048576.0, double(vram_budget) / 1048576.0,
                                         vram_used > vram_budget ? " - OVER BUDGET (the driver pages: slow)" : "")
                           : std::string());
          dest_evict_ = {};
        }
        if (prepare_mirror_ || resolve_readback_) {  // patch 0020
          REXLOG_INFO("gow2_renderer: patch 0020 frames {}-{}: mirror prepared {} runs ({:.1f} MB) | readbacks {}, "
                      "landed {} ({:.1f} MB), waits {}, previous copy not ready {}, failed {}",
                      frame_ + 1 - uint64_t(log_every_), frame_, rb_stats_.prepared_runs,
                      double(rb_stats_.prepared_bytes) / 1048576.0, rb_stats_.readbacks, rb_stats_.landed,
                      double(rb_stats_.landed_bytes) / 1048576.0, rb_stats_.waits, rb_stats_.not_ready, rb_stats_.failed);
          rb_stats_ = {};
        }
        {  // patch 0008
          const uint64_t lost = watch_->GpuPagesLostToWidening();
          if (lost != watch_lost_logged_ || REXCVAR_GET(gow2_renderer_watch_narrow))
            REXLOG_INFO("gow2_renderer: page watch frames {}-{}: GPU-written pages marked CPU-written by the 4 MB "
                        "widening {} (this range; {} so far){}",
                        frame_ + 1 - uint64_t(log_every_), frame_, lost - watch_lost_logged_, lost,
                        REXCVAR_GET(gow2_renderer_watch_narrow) ? " - narrow widening on" : "");
          watch_lost_logged_ = lost;
        }
        if (scaled_resolves_)
          REXLOG_INFO("gow2_renderer: scaled (Stage 7) frames {}-{}: scaled resolves {} (not into scaled memory {}), "
                      "destinations {}, memory chunks {}, textures {} ({:.1f} MB), loads {}, slots bound {} (this range), "
                      "partial views taken {}, window writes {} (fused {}) | session: dropped unused {}, failed creations retried {}",
                      frame_ + 1 - uint64_t(log_every_), frame_, scaled_resolve_count_, scaled_mem_skipped_,
                      scaled_dests_.size(),
                      scaled_chunks_.size(),
                      ts.scaled_textures, double(ts.scaled_bytes) / 1048576.0, ts.scaled_loads, ts.scaled_bound,
                      scaled_partial_hits_, scaled_window_writes_, scaled_fused_, textures_->ScaledDropped(), textures_->CreateRetries());
        scaled_partial_hits_ = 0;
        scaled_window_writes_ = 0;
        scaled_fused_ = 0;
        if (scaled_resolves_) {  // job 189: resolve-written textures not taken at the scale (top 10 this range)
          std::vector<std::pair<uint64_t, std::string>> top;
          for (const auto& [why, n] : ts.scaled_misses) top.push_back({n, why});
          std::sort(top.rbegin(), top.rend());
          for (size_t i = 0; i < std::min<size_t>(top.size(), 10); ++i)
            REXLOG_INFO("gow2_renderer: scaled miss #{} frames {}-{}: {}x {}", i + 1, frame_ + 1 - uint64_t(log_every_),
                        frame_, top[i].first, top[i].second);

        }
        scaled_resolve_count_ = 0;
        scaled_mem_skipped_ = 0;
        t2_stats_ = {};
      }
      LogSummary();
      LogDisplay();
      LogPresent();
    }
    if ((frame_ & 511u) == 0) EvictStaleUploads();  // patch 0149 (the stats copy that stood here was never read)
    std::copy(std::begin(part_ticks_), std::end(part_ticks_), std::begin(hitch_ticks_));  // patch 0010 (after the
    // summary, which zeroes part_ticks_): the next frame's parts are measured from here
    // The next frame's draw records, if it is a comparison frame (shared with its pending resolves).
    BeginTraceFrame();
    last_swap_end_ = std::chrono::steady_clock::now();  // patch 0063 (after any hold: a hold is not a load)
    last_swap_ns_.store(last_swap_end_.time_since_epoch().count(), std::memory_order_relaxed);  // prebuild v2
  }

  // ---- Patch 0063: load hold (Shagg 18:24, job 358 [V]: a first visit's 340-470 new pipelines cost 80-150 s of
  // driver time over 12 workers, and the seconds right after the load drew a half-blank world - 198k draws skipped in
  // ch10's frames 301-600; a second visit skipped 0-41). A frame that would come out half-drawn right after a load is
  // not presented (the previous picture - the loading screen - stays) and the swap waits on this thread until the
  // pipelines its skipped draws need are built: the guest waits for its GPU, so the game pauses with it. ----
  void DecideLoadHold() {
    hold_this_frame_ = false;
    const auto now = std::chrono::steady_clock::now();
    if (!last_swap_end_.time_since_epoch().count() || now - last_swap_end_ > std::chrono::seconds(1)) {
      load_window_until_ = now + std::chrono::seconds(20);  // a load (or the first frame)
      hold_left_ms_ = double(REXCVAR_GET(gow2_renderer_load_hold_max_ms));
    }
    const bool scene = pending_this_frame_ >= 200 && uint64_t(pending_this_frame_) * 2 >= draw_index_;
    // Prebuild v2: a new scene opens the load window even with the hold off (the prebuild uses the window too).
    if (scene && now >= load_window_until_ && (prebuild_on_ || REXCVAR_GET(gow2_renderer_load_hold))) {
      load_window_until_ = now + std::chrono::seconds(20);  // a new scene mid-play: a window and budget of its own
      hold_left_ms_ = double(REXCVAR_GET(gow2_renderer_load_hold_max_ms));
    }
    load_until_ns_.store(load_window_until_.time_since_epoch().count(), std::memory_order_relaxed);
    if (!REXCVAR_GET(gow2_renderer_load_hold) || !present_ours_ || hold_descs_.empty()) return;
    // Patch 0064 (Shagg 18:33: "I'll take a longer hold over stuttering frames everyday"; job 360: ch05 still showed
    // 1391 skipped draws after its 20 s window, in bursts of 70-450 per 300 frames): any pending draw holds a frame in
    // the load window; mid-play, 8+ pending draws hold it for up to 1 s (the load budget is not used).
    const uint32_t load_n = uint32_t(std::max(1, REXCVAR_GET(gow2_renderer_load_hold_draws)));
    const int32_t play_n = REXCVAR_GET(gow2_renderer_play_hold_draws);
    hold_in_window_ = now < load_window_until_;
    if (hold_in_window_) {
      hold_this_frame_ = (hold_left_ms_ > 0.0 && (pending_this_frame_ >= load_n || scene));
      hold_cap_ms_ = hold_left_ms_;
    } else {
      hold_this_frame_ = play_n > 0 && pending_this_frame_ >= uint32_t(play_n);
      hold_cap_ms_ = double(std::max(0, REXCVAR_GET(gow2_renderer_play_hold_max_ms)));
    }
  }
  void HoldForPipelines() {
    const auto t0 = std::chrono::steady_clock::now();
    size_t left = 0;
    for (bool shown = false;;) {
      left = 0;
      for (const auto& d : hold_descs_) left += backend_->PipelinePending(d) ? 1 : 0;
      const double ms = MsSince(t0);
      if (!left || ms >= hold_cap_ms_) break;
      if (shown || ms >= 100.0) {
        XenosSetLoadHold(true, uint32_t(hold_descs_.size()), uint32_t(left));
        shown = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    XenosSetLoadHold(false, 0, 0);
    const double ms = MsSince(t0);
    if (hold_in_window_) hold_left_ms_ -= ms;
    ++stats_.load_holds;
    REXLOG_INFO("gow2_renderer: {} hold: frame {} not shown ({} of {} draws pending, {} pipelines) - "
                "waited {:.0f} ms, {} still pending; hold time left {:.0f} ms",
                hold_in_window_ ? "load" : "play", frame_, pending_this_frame_, draw_index_, hold_descs_.size(),
                ms, left, std::max(0.0, hold_left_ms_));
  }

  // ---- Patch 0066: shaders the game creates while loading (Shagg 19:09-19:23: "build behind the loading screen").
  // Our GPU side only meets a chapter's shaders at its first draws - the burst the 0063 hold covers. Each game's own
  // shader-creation code (UE3's pixel / vertex shader constructors, found per game: layer 3 hooks) hands us the
  // microcode as it is created during the load. Job 362c [V, RTX 3080 Ti]: the driver keeps every shader STAGE it has
  // compiled - a pipeline whose VS and PS were each built before, in any pairing and any state, costs ~0.1 ms (vs
  // 15-35 ms new). So each new shader is translated (renderer thread, a few ms per frame) and built once in a guessed
  // pipeline (paired with any earlier shader of the other stage; opaque state) on the workers as start-up backlog. ----
 public:
  void OnGuestShaderCreated(bool is_vertex, uint32_t address, uint32_t size) {  // the game's loading thread
    if (!memory_ || !size || size > 256u * 1024u || (size & 3) || !address) return;
    const uint8_t* src = memory_->TranslateVirtual<const uint8_t*>(address);
    if (!src) return;
    CreatedShader c;
    c.is_vertex = is_vertex;
    c.bytes.assign(src, src + size);
    std::lock_guard<std::mutex> lock(created_mutex_);
    if (created_queue_.size() < 20000) created_queue_.push_back(std::move(c));
  }

 private:
  struct CreatedShader {
    bool is_vertex = false;
    std::vector<uint8_t> bytes;  // the microcode as the game created it (big-endian dwords)
  };
  void DrainCreatedShaders() {
    {
      std::lock_guard<std::mutex> lock(created_mutex_);
      if (created_queue_.empty() && guess_waiting_.empty()) return;
    }
    if (!cache_ || !cache_->Dxbc() || !REXCVAR_GET(gow2_renderer_shader_precreate)) {
      std::lock_guard<std::mutex> lock(created_mutex_);
      created_queue_.clear();
      return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const double budget = double(std::max(1, REXCVAR_GET(gow2_renderer_shader_precreate_ms)));
    std::vector<uint32_t> dwords;
    for (;;) {
      CreatedShader c;
      {
        std::lock_guard<std::mutex> lock(created_mutex_);
        if (created_queue_.empty()) break;
        c = std::move(created_queue_.front());
        created_queue_.pop_front();
      }
      ++stats_.created_seen;
      const uint64_t h = XXH3_64bits(c.bytes.data(), c.bytes.size());  // the same key as the draws' shaders
      created_hashes_.insert(h);
      if (prebuild_on_ && !c.is_vertex) PrebuildMarkKnown(h);  // prebuild v2: built here, not again from a package
      if (const auto* known = cache_->Find(h, c.is_vertex)) {
        ++stats_.created_known;
        if (known->ready() && known->handle && REXCVAR_GET(gow2_renderer_stage_prefetch) &&
            backend_->PrefetchShaderStage(known->handle)) ++stats_.created_prefetched;
      } else {
        dwords.resize(c.bytes.size() / 4);
        for (size_t i = 0; i < dwords.size(); ++i) {
          uint32_t v;
          std::memcpy(&v, c.bytes.data() + i * 4, 4);
          dwords[i] = std::byteswap(v);
        }
        const rr::GuestShader& g = cache_->Get(h, dwords.data(), dwords.size(), c.is_vertex);
        if (g.ready() && g.handle) {
          ++stats_.created_translated;
          if (REXCVAR_GET(gow2_renderer_stage_prefetch) && backend_->PrefetchShaderStage(g.handle)) {
            ++stats_.created_prefetched;
            if (MsSince(t0) >= budget) break;
            continue;
          }
          gpu::ShaderHandle& partner_slot = c.is_vertex ? guess_vs_ : guess_ps_;  // the first of a stage pairs others
          if (!partner_slot) partner_slot = g.handle;
          guess_waiting_.push_back({c.is_vertex, g.handle});
        }
      }
      if (MsSince(t0) >= budget) break;
    }
    // One guessed pipeline per new shader, once a shader of the other stage exists to pair it with.
    if (!guess_waiting_.empty() && guess_vs_ && guess_ps_) {
      backend_->SetPipelineBacklog(true);
      for (const auto& [is_vertex, handle] : guess_waiting_) {
        gpu::GraphicsPipelineDesc d;
        d.vs = is_vertex ? handle : guess_vs_;
        d.ps = is_vertex ? guess_ps_ : handle;
        d.topology = gpu::Topology::kTriangleList;
        d.cull = gpu::CullMode::kBack;
        d.depth_enable = d.depth_write = true;
        d.depth_func = gpu::CompareFunc::kLessEqual;
        d.depth_format = gpu::Format::kD24UnormS8;
        d.color_count = 1;
        d.color_formats[0] = gpu::Format::kR8G8B8A8Unorm;
        d.color_write_masks[0] = 0xF;
        backend_->GetGraphicsPipeline(d, true, nullptr);
        ++stats_.created_guessed;
      }
      backend_->SetPipelineBacklog(false);
      guess_waiting_.clear();
    }
  }
  std::mutex created_mutex_;
  std::deque<CreatedShader> created_queue_;                  // filled by the game's loading thread
  std::vector<std::pair<bool, gpu::ShaderHandle>> guess_waiting_;
  std::unordered_set<uint64_t> created_hashes_;
  gpu::ShaderHandle guess_vs_ = 0, guess_ps_ = 0;

  // ---- patch 0068: shaders built while the game loads an area (Shagg: "build behind the actual loading screen") ----
  // Layer 2 (UE3, all three games) reads each package as the game opens it and hands over its pixel shaders' microcode
  // Package records arrive off the render thread. Every distinct stage/microcode pair and both
  // stored variants are retained. Backpressure bounds the queue without dropping records.
  // At startup all pipeline workers prepare the game before its main thread runs; afterward live
  // draws retain priority and package work never holds a gameplay frame. The backend also deduplicates
  // equivalent final bytecode/state and persists its warm pipelines separately from live pipelines.
 public:
  bool PrebuildWanted() const { return prebuild_on_; }
  void BeginStartupPrebuild() {
    startup_pipeline_total_.store(backend_->PipelineJobsLeft(), std::memory_order_relaxed);
    startup_prebuild_.store(true, std::memory_order_relaxed);
    XenosSetShaderStartupPhase(::cogbox::shader_progress::ShaderPreparationPhase::kFinding);
    GovernWarmWorkers();
  }
  bool FinishStartupPrebuild() {
    const uint64_t left = PrebuildLeft() + backend_->PipelineJobsLeft();
    XenosSetPrebuildProgress(left, prebuild_accepted_.load() + startup_pipeline_total_.load(), true);
    if (left) return false;
    XenosSetShaderStartupPhase(::cogbox::shader_progress::ShaderPreparationPhase::kSaving);
    backend_->SavePipelinesNow();
    backend_->ReleaseWarmupCache();
    REXLOG_INFO("gow2_renderer: startup source cache reused {} compiled shaders", backend_->WarmSourcesReused());
    startup_prebuild_.store(false, std::memory_order_relaxed);
    GovernWarmWorkers();
    const auto& s = backend_->Stats();
    REXLOG_INFO("gow2_renderer: startup shader preparation finished: {} unique shaders, {} translated/queued, "
                "{} distinct driver pipelines ({} cache hits), {} translation failures, {} driver failures; live fallback enabled",
                prebuild_accepted_.load(), prebuild_sent_.load(), s.pipelines_warmed, s.pipelines_warm_loaded,
                prebuild_untranslated_.load(), s.pipelines_warm_failed);
    return true;
  }
  void OnPrebuildShader(bool vertex, const void* be_ucode, uint32_t size, uint32_t variant, uint64_t hash) {  // any thread
    if (!prebuild_on_ || !be_ucode || !size || size > 256u * 1024u || (size & 3)) return;
    if (!hash) hash = XXH3_64bits(be_ucode, size);  // the draws' key; layer 2 passes it (this is the reader's thread)
    PrebuildItem it;
    it.hash = hash;
    it.vertex = vertex;
    it.variant = uint8_t(variant & 1);
    prebuild_offered_.fetch_add(1, std::memory_order_relaxed);
    last_offer_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    {
      std::unique_lock<std::mutex> lock(prebuild_mutex_);
      if (!prebuild_seen_[vertex ? 1 : 0].insert(hash).second) return;
      if (backend_->WarmSourceCached(vertex, hash)) {
        if (!vertex) prebuild_queued_hashes_.emplace(hash, it.variant);
        prebuild_accepted_.fetch_add(1, std::memory_order_relaxed);
        prebuild_sent_.fetch_add(1, std::memory_order_relaxed);
        prebuild_queued_.fetch_add(1, std::memory_order_relaxed);
        return;  // already compiled: no microcode copy, translation, queue or driver recreation
      }
      prebuild_cv_.wait(lock, [this] { return prebuild_queue_.size() < kPrebuildQueueMax; });
      it.bytes.assign(static_cast<const uint8_t*>(be_ucode), static_cast<const uint8_t*>(be_ucode) + size);
      prebuild_queue_.push_back(std::move(it));
      prebuild_accepted_.fetch_add(1, std::memory_order_relaxed);
    }
    prebuild_cv_.notify_one();
  }
  // Layer 2 asks this to choose which variants to offer (0069); a forced variant (compile step) answers for itself.
  uint64_t PrebuildVariantHits(uint32_t v) const {
    return prebuild_variant_hits_[v & 1].load(std::memory_order_relaxed);
  }

 private:
  static constexpr size_t kPrebuildQueueMax = 60000;  // ~2-3 KB each: well under 200 MB even when full
  struct PrebuildItem {
    uint64_t hash = 0;
    uint8_t variant = 0;
    bool vertex = false;
    std::vector<uint8_t> bytes;  // big-endian microcode
  };
  static uint32_t PrebuildLoadWorkers() {  // gow2_renderer_shader_prebuild_workers, -1 = a quarter, at least 2
    const int v = REXCVAR_GET(gow2_renderer_shader_prebuild_workers);
    if (v >= 0) return uint32_t(v);
    return std::max(2u, std::max(1u, std::thread::hardware_concurrency()) / 4u);
  }
  void StartPrebuild() {  // Init, after the shader cache is set up (before any draw)
    prebuild_on_ = REXCVAR_GET(gow2_renderer_shader_prebuild) && cache_ && cache_->Dxbc() && !api_vulkan_;
    if (!prebuild_on_) return;
    gpu::GraphicsPipelineDesc warm;
    xenos::ucode::Program stub;
    stub.is_vertex_shader = true;
    const auto translated = xenos::dxbc_gen::TranslateToDxbc(stub);
    if (translated.ok) warm.vs = backend_->CreateShader(gpu::ShaderStage::kVertex,
                                                       translated.bytecode.data(), translated.bytecode.size());
    warm.color_count = 4;
    for (uint32_t i = 0; i < warm.color_count; ++i) {
      warm.color_formats[i] = gpu::Format::kR8G8B8A8Unorm;
      warm.color_write_masks[i] = 15;
    }
    warm.depth_format = gpu::Format::kD24UnormS8;
    warm_template_set_ = backend_->SetWarmTemplate(warm);
    if (!warm_template_set_) {
      REXLOG_ERROR("gow2_renderer: startup shader template could not be created; prebuild unavailable");
      prebuild_on_ = false;
      return;
    }
    backend_->ConfigureWarmSourceCache(cache_->PrebuildIdentity());
    warm_load_workers_ = PrebuildLoadWorkers();
    warm_play_workers_ = uint32_t(std::max(0, REXCVAR_GET(gow2_renderer_shader_prebuild_play_workers)));
    LoadVariantCounts();  // diagnostic counts only; never suppress either variant
    {  // Track recorded shaders for diagnostics; they still receive driver preparation.
      std::lock_guard<std::mutex> lock(prebuild_mutex_);
      cache_->ForEachKnown(false, [this](uint64_t h) { prebuild_known_ps_.insert(h); });
    }
    GovernWarmWorkers();
    const uint32_t translators = std::clamp(std::max(1u, std::thread::hardware_concurrency()) / 4u, 1u, 4u);
    for (uint32_t i = 0; i < translators; ++i)
      prebuild_threads_.emplace_back([this, i] { PrebuildThread(i); });  // lives as long as the renderer (never destroyed)
    REXLOG_INFO("gow2_renderer: shader prebuild enabled: both stages, all stored variants, {} pipeline workers "
                "while loading / {} during play; startup uses the full worker pool, no speculative gameplay holds",
                warm_load_workers_, warm_play_workers_);
  }
  // v2: loading = no swap for 250 ms (the game is loading without presenting) or within the load window (DecideLoadHold:
  // the 20 s after a load or a new scene). Any thread; tells the backend only when the count changes.
  void GovernWarmWorkers() {
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    const int64_t last = last_swap_ns_.load(std::memory_order_relaxed);
    const int64_t stall = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(250)).count();
    const int64_t recent = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(5)).count();
    // v2 fix (Gears 3 log, 16:39: its loading screen keeps presenting, so neither test above saw the map load and the
    // prebuild ran on 1 worker): a package opened in the last 5 s is a load too.
    const bool loading = startup_prebuild_.load(std::memory_order_relaxed) || !last || now - last > stall || now < load_until_ns_.load(std::memory_order_relaxed) ||
                         now - last_offer_ns_.load(std::memory_order_relaxed) < recent;
    // Compile step: the game waits behind the hold, so every pipeline worker may build (the backend caps it).
    const uint32_t want = startup_prebuild_.load(std::memory_order_relaxed) ? 1024u
                          : loading                                         ? warm_load_workers_
                                                                            : warm_play_workers_;
    // The on-screen compile indicator (fps_bar.cpp), at most every 100 ms: from here, because the prebuild thread
    // calls this while it works and while it idles - also when the game presents nothing (no swap to report it).
    const int64_t every = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(100)).count();
    int64_t shown = progress_shown_ns_.load(std::memory_order_relaxed);
    if (now - shown >= every && progress_shown_ns_.compare_exchange_strong(shown, now, std::memory_order_relaxed)) {
      const bool startup = startup_prebuild_.load(std::memory_order_relaxed);
      XenosSetPrebuildProgress(PrebuildLeft() + (startup ? backend_->PipelineJobsLeft() : 0),
                               prebuild_accepted_.load(std::memory_order_relaxed) +
                                   (startup ? startup_pipeline_total_.load(std::memory_order_relaxed) : 0), loading);
    }
    std::lock_guard<std::mutex> lock(warm_govern_mutex_);
    if (want == warm_workers_now_) return;
    warm_workers_now_ = want;
    backend_->SetWarmWorkers(want);
    ++warm_govern_changes_;
  }
  void PrebuildMarkKnown(uint64_t ps_hash) {  // recording thread: a pixel shader now in the cache (drawn / created)
    std::lock_guard<std::mutex> lock(prebuild_mutex_);
    prebuild_known_ps_.insert(ps_hash);
  }
  void PrebuildThread(uint32_t worker_index) {
    const uint64_t cap = uint64_t(std::max(1, REXCVAR_GET(gow2_renderer_shader_prebuild_queue)));
    std::vector<uint32_t> dwords;
    for (;;) {
      PrebuildItem item;
      for (;;) {
        {
          std::unique_lock<std::mutex> lock(prebuild_mutex_);
          // A timed wait: the worker count follows load / play even while nothing new arrives (no swaps in a load).
          prebuild_cv_.wait_for(lock, std::chrono::milliseconds(50), [this, worker_index] { return !prebuild_queue_.empty() && (worker_index == 0 || startup_prebuild_.load()); });
          if (!prebuild_queue_.empty() && (worker_index == 0 || startup_prebuild_.load())) {
            // Patch 0069: newest first - the package the game opened last is the area about to be drawn.
            item = std::move(prebuild_queue_.back());
            prebuild_queue_.pop_back();
            prebuild_cv_.notify_all();
            break;
          }
        }
        GovernWarmWorkers();
      }
      GovernWarmWorkers();  // also while busy: the worker count and the on-screen progress (rate-limited inside)
      // The workers' warm queue is bounded; until the first DXBC draw there is no template, so wait for it.
      while (!warm_template_set_.load(std::memory_order_acquire) || backend_->WarmJobsLeft() >= cap) {
        GovernWarmWorkers();
        backend_->WaitWarmCapacity(cap);
      }
      if (backend_->WarmSourceCached(item.vertex, item.hash)) {
        if (!item.vertex) { std::lock_guard<std::mutex> lock(prebuild_mutex_); prebuild_queued_hashes_.emplace(item.hash, item.variant); }
        prebuild_sent_.fetch_add(1, std::memory_order_relaxed);
        prebuild_queued_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      dwords.resize(item.bytes.size() / 4);
      for (size_t i = 0; i < dwords.size(); ++i) {
        uint32_t v;
        std::memcpy(&v, item.bytes.data() + i * 4, 4);
        dwords[i] = std::byteswap(v);
      }
      std::vector<uint8_t> dxbc;
      if (!cache_->TranslateDxbcOnly(dwords.data(), dwords.size(), item.vertex, dxbc)) {
        prebuild_untranslated_.fetch_add(1, std::memory_order_relaxed);
        REXLOG_WARN("gow2_renderer: prebuild could not translate {} {:016X}; live shader fallback remains enabled",
                    item.vertex ? "VS" : "PS", item.hash);
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(prebuild_mutex_);
        if (!item.vertex) prebuild_queued_hashes_.emplace(item.hash, item.variant);
      }
      const bool sent = item.vertex ? backend_->WarmVertexShader(std::move(dxbc), item.hash)
                                    : backend_->WarmPixelShader(std::move(dxbc), item.hash);
      if (sent) prebuild_sent_.fetch_add(1, std::memory_order_relaxed);
      else REXLOG_ERROR("gow2_renderer: prebuild could not queue {} {:016X}", item.vertex ? "VS" : "PS", item.hash);
      // Publish completion only after the backend owns the job (no false zero at the startup barrier).
      prebuild_queued_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  std::atomic<bool> startup_prebuild_{false};
  std::atomic<uint64_t> startup_pipeline_total_{0};
  bool prebuild_on_ = false;
  bool api_vulkan_ = false;  // v2 (Init)
  std::atomic<bool> warm_template_set_{false};
  std::atomic<uint64_t> prebuild_variant_hits_[2] = {};
  std::mutex prebuild_mutex_;  // guards the queue and the three sets / maps below
  std::condition_variable prebuild_cv_;
  std::deque<PrebuildItem> prebuild_queue_;  // from layer 2, to the prebuild thread
  std::unordered_set<uint64_t> prebuild_seen_[2];                    // every hash offered
  std::unordered_set<uint64_t> prebuild_known_ps_;                // v2: pixel shaders the cache has (nothing to build)
  std::unordered_map<uint64_t, uint8_t> prebuild_queued_hashes_;  // sent to be built -> variant
  std::vector<std::thread> prebuild_threads_;
  std::atomic<uint64_t> prebuild_offered_{0}, prebuild_known_{0}, prebuild_queued_{0}, prebuild_overflow_{0};
  std::atomic<uint64_t> prebuild_sent_{0}, prebuild_untranslated_{0}, prebuild_dropped_{0};
  // v2: the load / play worker count (GovernWarmWorkers) and the last swap / load window for any thread.
  uint32_t warm_load_workers_ = 3, warm_play_workers_ = 1;
  std::mutex warm_govern_mutex_;
  uint32_t warm_workers_now_ = UINT32_MAX;  // under warm_govern_mutex_
  uint64_t warm_govern_changes_ = 0;        // under warm_govern_mutex_
  std::atomic<int64_t> last_swap_ns_{0}, load_until_ns_{0};  // steady_clock ticks
  std::atomic<int64_t> progress_shown_ns_{0};  // the compile indicator's last report (GovernWarmWorkers)
  // Startup and background progress counts.
  std::atomic<int64_t> last_offer_ns_{0};
  std::atomic<uint64_t> prebuild_accepted_{0};
  // Shaders still to do: taken in but not yet through the prebuild thread, plus the warm-only jobs at the workers.
  uint64_t PrebuildLeft() {
    const uint64_t acc = prebuild_accepted_.load(std::memory_order_relaxed);
    const uint64_t done = prebuild_known_.load(std::memory_order_relaxed) + prebuild_dropped_.load(std::memory_order_relaxed) +
                          prebuild_untranslated_.load(std::memory_order_relaxed) +
                          prebuild_queued_.load(std::memory_order_relaxed) + prebuild_overflow_.load(std::memory_order_relaxed);
    return (acc > done ? acc - done : 0) + backend_->WarmJobsLeft();
  }
  uint64_t prebuild_logged_offered_ = 0, prebuild_logged_new_ps_ = 0;
  // Patch 0069: totals for the whole session (stats_ is cleared every log window, so 0068's line showed one window).
  // v2: only the recording thread's two counts are here; the rest are the atomics above.
  struct PrebuildTotals {
    uint64_t new_ps_pipelines = 0, new_ps_prebuilt = 0;
  } pb_;
  // Patch 0069: which variant this game draws, remembered in the shader cache folder (prebuild-variants.txt: two
  // counts), for diagnostics; it never filters the next session.
  std::filesystem::path variant_file_;
  uint64_t variant_saved_total_ = 0;
  void LoadVariantCounts() {
    variant_file_ = cache_->Dir() / "prebuild-variants.txt";
    std::ifstream f(variant_file_);
    uint64_t a = 0, b = 0;
    if (f >> a >> b) {
      prebuild_variant_hits_[0].store(a);
      prebuild_variant_hits_[1].store(b);
      variant_saved_total_ = a + b;
      REXLOG_INFO("gow2_renderer: shader prebuild: this game drew prebuilt shaders whole {} / code only {} before "
                  "(prebuild-variants.txt)", a, b);
    }
  }
  void SaveVariantCounts() {  // log time, when 32+ more were counted
    const uint64_t a = prebuild_variant_hits_[0].load(), b = prebuild_variant_hits_[1].load();
    if (a + b < variant_saved_total_ + 32) return;
    std::ofstream f(variant_file_, std::ios::trunc);
    if (f << a << ' ' << b << '\n') variant_saved_total_ = a + b;
  }

 public:

 private:
  // ---- takeover step 1: our image on screen (notes/50) ----
  struct DisplaySource {
    gpu::TextureHandle texture = 0;
    gpu::Rect rect{};
    ::xenos::resolve::Info info{};
    uint32_t endian = 0;
    gpu::TextureHandle scaled = 0;  // Stage 7: the same copy at the resolution scale (rect x scale), if made
  };
  struct DisplayPending {
    gpu::ReadbackHandle readback = 0;
    DisplaySource src;
    uint64_t frame = 0;
  };
  struct DisplayStats {
    uint64_t swaps = 0, shown = 0, no_source = 0, not_ready = 0, dropped = 0, lag_sum = 0, bytes = 0, bad = 0;
  };
  void RequestDisplay(const RegisterFile& regs, uint32_t fb_phys, uint32_t fb_width, uint32_t fb_height) {
    ++display_stats_.swaps;
    auto it = display_sources_.find(fb_phys);
    if (!display_first_logged_) {
      display_first_logged_ = true;
      std::string list;
      for (const auto& [base, d] : display_sources_) list += fmt::format(" {:08X}", base);
      REXLOG_INFO("gow2_renderer: display: first swap front buffer {:08X} {}x{}; this frame's colour copies to:{}{}",
                  fb_phys, fb_width, fb_height, list.empty() ? " none" : list,
                  it == display_sources_.end() ? " - NO MATCH (the emulator's image stays)" : " - match");
    }
    if (it == display_sources_.end()) {
      ++display_stats_.no_source;
    } else {
      front_bases_.insert(it->second.info.dest_base);  // job 238: keep its scaled destination (non-fused path)
      DisplayPending p;
      p.src = it->second;
      p.frame = frame_;
      if (present_ours_ && hold_this_frame_) {  // patch 0063: held back - the previous picture stays on screen
        ++display_stats_.dropped;
      } else if (present_ours_ && it->second.scaled) {  // Stage 7: the front buffer at the resolution scale
        DisplaySource s = it->second;
        s.texture = s.scaled;
        s.rect = {s.rect.x0 * int32_t(res_scale_), s.rect.y0 * int32_t(res_scale_), s.rect.x1 * int32_t(res_scale_),
                  s.rect.y1 * int32_t(res_scale_)};
        PresentOurs(regs, s, fb_width * res_scale_, fb_height * res_scale_);
      } else if (present_ours_) {  // 5C-2: straight into the presenter; no readback, no guest-memory front buffer
        PresentOurs(regs, it->second, fb_width, fb_height);
      } else {
        p.readback = backend_->ReadbackTexture(p.src.texture, p.src.rect);
        if (p.readback) display_pending_.push_back(p); else ++display_stats_.bad;
        if (present_shadow_) PresentShadow(regs, it->second, fb_width, fb_height);
      }
    }
    display_sources_.clear();
  }

  // ---- Stage 5C-1: the swap's gamma pass on our front buffer, compared with the SDK's presented image ----
  struct PresentImage {
    gpu::ReadbackHandle readback = 0;
    uint64_t frame = 0;
    uint32_t width = 0, height = 0;
  };
  struct PresentStats {
    uint64_t made = 0, no_gamma = 0, unsupported = 0, sdk_images = 0, compared = 0, none_ready = 0, size_mismatch = 0,
             good = 0, exact = 0, identical_sum = 0, pixel_sum = 0, max_diff = 0;
    double worst = 100.0;
    uint64_t lag[8] = {};
  };
  // The gamma pass into present_out_ (packed RGB10A2 words, w x h). False when it could not run.
  bool GammaPass(const RegisterFile& regs, const DisplaySource& src, uint32_t fb_width, uint32_t fb_height, uint32_t& w,
                 uint32_t& h, bool& is_table) {
    const ::xenos::display_gamma::Ramps* ramps = XenosGammaRamps();
    if (!ramps) {
      ++present_stats_.no_gamma;
      return false;
    }
    // Fetch constant 0 (the SDK's swap texture): format dword_1 bits 0:5, endian 6:7; swizzle dword_3 bits 1:12.
    const uint32_t d1 = regs.values[0x4801], d3 = regs.values[0x4803];
    const uint32_t format = d1 & 0x3F, fetch_endian = (d1 >> 6) & 3, swizzle = (d3 >> 1) & 0xFFF;
    const bool table = format == 6 || format == 50, pwl = format == 7 || format == 54;
    if (!present_first_logged_) {
      present_first_logged_ = true;
      REXLOG_INFO("gow2_present: first swap: fetch 0 format {} ({}), endian {}, swizzle {:03X}; our copy endian {}, "
                  "rect {}x{}; swap {}x{}",
                  format, table ? "8_8_8_8, 256-entry table" : pwl ? "2_10_10_10, PWL" : "NOT HANDLED", fetch_endian,
                  swizzle, src.endian, src.rect.x1 - src.rect.x0, src.rect.y1 - src.rect.y0, fb_width, fb_height);
    }
    is_table = table;
    if (!table && !pwl) {
      ++present_stats_.unsupported;
      return false;
    }
    w = std::min<uint32_t>(fb_width ? fb_width : uint32_t(src.rect.x1), uint32_t(std::max(src.rect.x1, 0)));
    h = std::min<uint32_t>(fb_height ? fb_height : uint32_t(src.rect.y1), uint32_t(std::max(src.rect.y1, 0)));
    if (!w || !h) {
      ++present_stats_.unsupported;
      return false;
    }
    if (ramps->table_version != ramp_table_version_ || ramps->pwl_version != ramp_pwl_version_) {
      uint32_t words[256 + 128 * 3];
      std::memcpy(words, ramps->table, sizeof(ramps->table));
      std::memcpy(words + 256, ramps->pwl, sizeof(ramps->pwl));
      backend_->UploadTexture(ramp_texture_, 0, 0, words, uint32_t(sizeof(words)), 1, 1);
      ramp_table_version_ = ramps->table_version;
      ramp_pwl_version_ = ramps->pwl_version;
    }
    if (!present_out_ || present_out_w_ != w || present_out_h_ != h) {
      if (present_out_) backend_->DestroyTexture(present_out_);  // patch 0149: the old size was leaked
      gpu::TextureDesc od;
      od.width = w;
      od.height = h;
      od.format = gpu::Format::kR32Uint;
      od.usage = gpu::kUsageSampled | gpu::kUsageStorage | (smaa_ ? gpu::kUsageRenderTarget : 0u);
      od.debug_name = "present shadow (RGB10A2 words)";
      present_out_ = backend_->CreateTexture(od);
      present_out_w_ = w;
      present_out_h_ = h;
      if (!present_out_) {
        REXLOG_ERROR("gow2_present: could not create the {}x{} output; present shadow OFF", w, h);
        present_shadow_ = false;
        return false;
      }
    }
    gpu::ComputeBindings b;
    b.constants[0] = w;
    b.constants[1] = h;
    b.constants[2] = src.endian;
    b.constants[3] = fetch_endian;
    b.constants[4] = swizzle;
    b.constants[5] = pwl ? 1u : 0u;
    b.constant_count = 8;
    b.srv[0] = src.texture;
    b.srv[1] = ramp_texture_;
    b.uav_texture = present_out_;
    backend_->Dispatch(present_gamma_, b, (w + 7) / 8, (h + 7) / 8, 1);
    return true;
  }
  void PresentShadow(const RegisterFile& regs, const DisplaySource& src, uint32_t fb_width, uint32_t fb_height) {
    uint32_t w = 0, h = 0;
    bool table = false;
    if (!GammaPass(regs, src, fb_width, fb_height, w, h, table)) return;
    PresentImage img;
    img.readback = backend_->ReadbackTexture(present_out_, gpu::Rect{0, 0, int32_t(w), int32_t(h)});
    img.frame = frame_;
    img.width = w;
    img.height = h;
    if (!img.readback) {
      ++present_stats_.unsupported;
      return;
    }
    ++present_stats_.made;
    present_images_.push_back(img);
    while (present_images_.size() > 6) {
      backend_->ReleaseReadback(present_images_.front().readback);
      present_images_.pop_front();
    }
  }

  // 5C-2: our gamma image into the presenter's guest-output texture, on the presenter's queue, and submitted before the
  // refresher returns (Presenter::RefreshGuestOutput's contract). The display size is the SDK's (VdQueryVideoMode).
  void PresentOurs(const RegisterFile& regs, const DisplaySource& src, uint32_t fb_width, uint32_t fb_height) {
    uint32_t w = 0, h = 0;
    bool table = false;
    if (!GammaPass(regs, src, fb_width, fb_height, w, h, table)) {
      ++ours_stats_.not_presented;
      return;
    }
    if (smaa_ && !SmaaPass(w, h) && !smaa_failed_logged_) {
      smaa_failed_logged_ = true;
      REXLOG_ERROR("gow2_present: SMAA could not run (shaders or textures missing); presenting without it");
    }
    // "Match my monitor": the image at the window's exact size (the game's shape kept), shown 1:1 by the presenter.
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const bool own_shape = REXCVAR_GET(gow2_present_frame_aspect);
    uint32_t dw = own_shape ? w : std::max<uint32_t>(1, uint32_t(mode.display_width)),
             dh = own_shape ? h : std::max<uint32_t>(1, uint32_t(mode.display_height));
    gpu::TextureHandle out_tex = present_out_;
    uint32_t ow = w, oh = h;
    if (present_resample_) {
      uint32_t sw = 0, sh = 0;
      if (xenos::display::SurfaceSize(sw, sh)) {
        // The largest dw:dh rectangle inside the window (the presenter would letterbox to the same one).
        if (uint64_t(sw) * dh > uint64_t(sh) * dw) {
          oh = sh;
          ow = std::max<uint32_t>(1, uint32_t((uint64_t(sh) * dw + dh / 2) / dh));
        } else {
          ow = sw;
          oh = std::max<uint32_t>(1, uint32_t((uint64_t(sw) * dh + dw / 2) / dw));
        }
        ow = std::min<uint32_t>(ow, 16384);
        oh = std::min<uint32_t>(oh, 16384);
      }
      if (ow != w || oh != h) {
        if (!present_scaled_ || present_scaled_w_ != ow || present_scaled_h_ != oh) {
          if (present_scaled_) backend_->DestroyTexture(present_scaled_);
          gpu::TextureDesc sd;
          sd.width = ow;
          sd.height = oh;
          sd.format = gpu::Format::kR32Uint;
          sd.usage = gpu::kUsageSampled | gpu::kUsageStorage;
          sd.debug_name = "present at the window's size (RGB10A2 words)";
          present_scaled_ = backend_->CreateTexture(sd);
          present_scaled_w_ = ow;
          present_scaled_h_ = oh;
          if (present_scaled_ && !present_native_logged_) {
            present_native_logged_ = true;
            REXLOG_INFO("gow2_present: native size: game image {}x{} -> window {}x{} (shown {}x{})", w, h, sw, sh, ow, oh);
          }
        }
        if (present_scaled_) {
          gpu::ComputeBindings b;
          b.constants[0] = w;
          b.constants[1] = h;
          b.constants[2] = ow;
          b.constants[3] = oh;
          b.constants[4] = present_lanczos_ ? 1u : 0u;   // 0.2.0: c1.x = filter for enlarging
          b.constant_count = 8;
          b.srv[0] = present_out_;
          b.uav_texture = present_scaled_;
          backend_->Dispatch(present_resample_, b, (ow + 7) / 8, (oh + 7) / 8, 1);
          out_tex = present_scaled_;
          dw = ow;
          dh = oh;
        } else {
          ow = w;
          oh = h;
        }
      }
    }
    if (present_sharpen_) {   // 0.2.0: RCAS on the image as shown (after the resize to the window)
      if (!present_sharp_ || present_sharp_w_ != ow || present_sharp_h_ != oh) {
        if (present_sharp_) backend_->DestroyTexture(present_sharp_);
        gpu::TextureDesc sd;
        sd.width = ow;
        sd.height = oh;
        sd.format = gpu::Format::kR32Uint;
        sd.usage = gpu::kUsageSampled | gpu::kUsageStorage;
        sd.debug_name = "present sharpened (RGB10A2 words)";
        present_sharp_ = backend_->CreateTexture(sd);
        present_sharp_w_ = ow;
        present_sharp_h_ = oh;
      }
      if (present_sharp_) {
        gpu::ComputeBindings b;
        b.constants[0] = ow;
        b.constants[1] = oh;
        b.constants[4] = std::bit_cast<uint32_t>(float(std::clamp(REXCVAR_GET(gow2_present_sharpen), 0.0, 1.0)));
        b.constant_count = 8;
        b.srv[0] = out_tex;
        b.uav_texture = present_sharp_;
        backend_->Dispatch(present_sharpen_, b, (ow + 7) / 8, (oh + 7) / 8, 1);
        out_tex = present_sharp_;
      }
    }
    if (hdr_ && hdr_encode_) {  // Stage 7.3: PQ / BT.2020 for the HDR10 swap chain (SDK patch 24)
      gpu::ComputeBindings b;
      b.constants[0] = ow;
      b.constants[1] = oh;
      b.constants[4] = std::bit_cast<uint32_t>(float(REXCVAR_GET(gow2_hdr_paper_white)));
      b.constants[5] = std::bit_cast<uint32_t>(float(REXCVAR_GET(gow2_hdr_peak)));
      b.constants[6] = std::bit_cast<uint32_t>(float(REXCVAR_GET(gow2_hdr_knee)));
      b.constants[7] = std::bit_cast<uint32_t>(float(REXCVAR_GET(gow2_hdr_expand)));
      b.constant_count = 8;
      b.uav_texture = out_tex;
      backend_->Dispatch(hdr_encode_, b, (ow + 7) / 8, (oh + 7) / 8, 1);
      table = false;  // 10 bits per channel to the window
    }
    bool wrote = false;
    if (!presenter_) {  // notes/68: Vulkan without a presenter yet - the image is made, nothing is shown
      backend_->Submit();
      ++ours_stats_.presented;
      if (XenosFrameImagesWanted()) {
        PresentImage img;
        img.readback = backend_->ReadbackTexture(present_out_, gpu::Rect{0, 0, int32_t(w), int32_t(h)});
        img.frame = XenosSwapCount();  // align captures with frame-timed controller input, including skipped presents
        img.width = w;
        img.height = h;
        if (img.readback) ours_images_.push_back(img);
      }
      return;
    }
    const bool refreshed = presenter_->RefreshGuestOutput(
        ow, oh, dw, dh, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
#if GN_SDK_VULKAN
          if (presenter_vulkan_) {  // stage V3: a draw into the presenter's image (no storage use on Vulkan)
            auto& v = static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
            context.SetIs8bpc(table);
            gpu::VulkanPresentTarget t;
            t.image = v.image();
            t.view = v.image_view();
            t.width = ow;
            t.height = oh;
            t.first_write = !v.image_ever_written_previously();
            if (!backend_->PresentPacked(out_tex, ow, oh, &t)) return false;
            backend_->Submit();  // on the presenter's queue, before the refresher returns
            wrote = true;
            return true;
          }
#endif
          auto& c = static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
          context.SetIs8bpc(table);  // as the SDK: 8bpc for the 256-entry table path without FXAA
          if (!backend_->AcquirePresenterImage()) return false;
          const gpu::TextureHandle out = backend_->ImportTexture(
              c.resource_uav_capable(), gpu::Format::kR10G10B10A2Unorm,
              uint32_t(rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState));
          if (!out) return false;
          gpu::ComputeBindings b;
          b.constants[0] = ow;
          b.constants[1] = oh;
          b.constant_count = 4;
          b.srv[0] = out_tex;
          b.uav_texture = out;
          backend_->Dispatch(present_unpack_, b, (ow + 7) / 8, (oh + 7) / 8, 1);
          backend_->ReleaseImport(out, uint32_t(rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState));
          if (fg_wanted_) FgHandOver(c.resource_uav_capable());  // patch 0147: before the Submit that carries the copies
          wrote = backend_->SubmitPresenterImage();
          return wrote;
        });
    if (refreshed && wrote) ++ours_stats_.presented; else ++ours_stats_.refresh_failed;
    const int check_every = REXCVAR_GET(gow2_present_check_every);
    if (refreshed && wrote && check_every > 0 && frame_ % uint64_t(check_every) == 0) ScreenCheck(out_tex, ow, oh);
    if (XenosFrameImagesWanted()) {  // frame shots: our presented image, read back (the SDK makes none now)
      PresentImage img;
      img.readback = backend_->ReadbackTexture(present_out_, gpu::Rect{0, 0, int32_t(w), int32_t(h)});
      img.frame = XenosSwapCount();
      img.width = w;
      img.height = h;
      if (img.readback) ours_images_.push_back(img);
    }
  }
  // What the window paints is the presenter's guest output: read it back (CaptureGuestOutput, RGBX8) and compare it
  // with our packed image of the same frame (10-bit channels -> 8-bit by rounding; the presenter's own conversion is
  // not known exactly, so within 1 level counts as the same).
  void ScreenCheck(gpu::TextureHandle tex, uint32_t w, uint32_t h) {
    const gpu::ReadbackHandle rb = backend_->ReadbackTexture(tex, gpu::Rect{0, 0, int32_t(w), int32_t(h)});
    backend_->WaitForFence(backend_->Submit());
    rex::ui::RawImage img;
    const bool got = presenter_->CaptureGuestOutput(img);
    uint32_t pitch = 0;
    const uint8_t* ours = rb ? backend_->ReadbackData(rb, pitch) : nullptr;
    if (!got || !ours || img.width != w || img.height != h) {
      REXLOG_INFO("gow2_present: screen check frame {}: NOT COMPARED (captured {} {}x{}, ours {} {}x{})", frame_, got,
                  img.width, img.height, ours != nullptr, w, h);
      if (rb) backend_->ReleaseReadback(rb);
      return;
    }
    uint64_t exact = 0, within1 = 0, maxd = 0;
    for (uint32_t y = 0; y < h; ++y) {
      const auto* o = reinterpret_cast<const uint32_t*>(ours + size_t(y) * pitch);
      const uint8_t* c = img.data.data() + size_t(y) * img.stride;
      for (uint32_t x = 0; x < w; ++x) {
        uint64_t d = 0;
        for (uint32_t k = 0; k < 3; ++k) {
          const uint32_t ten = (o[x] >> (10 * k)) & 0x3FF;
          const int32_t ours8 = int32_t((ten * 255 + 511) / 1023), cap8 = c[x * 4 + k];
          d = std::max<uint64_t>(d, uint64_t(ours8 > cap8 ? ours8 - cap8 : cap8 - ours8));
        }
        if (d == 0) ++exact;
        if (d <= 1) ++within1;
        maxd = std::max(maxd, d);
      }
    }
    const double n = double(w) * h;
    REXLOG_INFO("gow2_present: screen check frame {}: presenter image vs ours: {:.4f}% of pixels within 1 level "
                "({:.4f}% exact), max difference {} (of 255)",
                frame_, 100.0 * double(within1) / n, 100.0 * double(exact) / n, maxd);
    backend_->ReleaseReadback(rb);
  }
  void DeliverFrameImages() {
    while (!ours_images_.empty() && backend_->IsReadbackReady(ours_images_.front().readback)) {
      const PresentImage img = ours_images_.front();
      ours_images_.pop_front();
      uint32_t pitch = 0;
      if (const uint8_t* data = backend_->ReadbackData(img.readback, pitch)) {
        XenosDispatchFrameImage(data, img.width, img.height, pitch, img.frame);
        ++ours_stats_.images;
      }
      backend_->ReleaseReadback(img.readback);
    }
    while (ours_images_.size() > 8) {  // never ready: give up on the oldest
      backend_->ReleaseReadback(ours_images_.front().readback);
      ours_images_.pop_front();
    }
  }
  struct OursStats {
    uint64_t presented = 0, refresh_failed = 0, not_presented = 0, images = 0;
  };
  rex::ui::Presenter* presenter_ = nullptr;  // D3D12Presenter, or VulkanPresenter when presenter_vulkan_ (notes/68 V3)
  bool presenter_vulkan_ = false;
  gpu::VulkanExternal vk_external_;  // notes/68 V3: the SDK device our Vulkan backend runs on (outlives backend_)
  bool vk_headless_present_ = false;  // notes/68: Vulkan, our present without a presenter (frame shots only)
  bool present_ours_ = false;
  gpu::PipelineHandle present_unpack_ = 0;
  // "Match my monitor": the resample pass and the image at the window's size.
  gpu::PipelineHandle present_resample_ = 0;
  gpu::TextureHandle present_scaled_ = 0;
  gpu::PipelineHandle present_sharpen_ = 0;   // 0.2.0: RCAS after the resize
  gpu::TextureHandle present_sharp_ = 0;
  uint32_t present_sharp_w_ = 0, present_sharp_h_ = 0;
  bool present_lanczos_ = false;
  uint32_t present_scaled_w_ = 0, present_scaled_h_ = 0;
  bool present_native_logged_ = false;
  // ---- Stage 7.2: SMAA on the presented image (smaa/smaa_passes.h, notes/54) ----
  bool smaa_ = false, smaa_failed_logged_ = false, smaa_tables_uploaded_ = false, smaa_ready_ = false;
  bool hdr_ = false;  // Stage 7.3: --present_hdr (SDK patch 24)
  gpu::PipelineHandle hdr_encode_ = 0;
  gpu::PipelineHandle smaa_edge_ = 0, smaa_weight_ = 0, smaa_blend_ = 0;
  gpu::TextureHandle smaa_color_ = 0, smaa_edges_ = 0, smaa_weights_ = 0, smaa_area_ = 0, smaa_search_ = 0;
  uint32_t smaa_w_ = 0, smaa_h_ = 0;
  uint64_t smaa_frames_ = 0;
  bool EnsureSmaa(uint32_t w, uint32_t h) {
    if (!smaa_ready_) {
      smaa_ready_ = true;  // once: pipelines and tables
      auto pipe = [&](const char* name, const char* vs, const char* ps, gpu::Format fmt) -> gpu::PipelineHandle {
        const gpu::ShaderHandle v =
            cache_->GetBuiltin(std::string(name) + "_vs", rr::smaa::Source(vs), gpu::ShaderStage::kVertex);
        const gpu::ShaderHandle p =
            cache_->GetBuiltin(std::string(name) + "_ps", rr::smaa::Source(ps), gpu::ShaderStage::kPixel);
        if (!v || !p) return 0;
        gpu::GraphicsPipelineDesc d;
        d.vs = v;
        d.ps = p;
        d.topology = gpu::Topology::kTriangleList;
        d.depth_clip = false;
        d.color_count = 1;
        d.color_formats[0] = fmt;
        d.color_write_masks[0] = 0xF;
        return backend_->GetGraphicsPipeline(d);
      };
      smaa_edge_ = pipe("smaa_edge", rr::smaa::kEdgeVs, rr::smaa::kEdgePs, gpu::Format::kR8G8B8A8Unorm);
      smaa_weight_ = pipe("smaa_weight", rr::smaa::kWeightVs, rr::smaa::kWeightPs, gpu::Format::kR8G8B8A8Unorm);
      smaa_blend_ = pipe("smaa_blend", rr::smaa::kBlendVs, rr::smaa::kBlendPs, gpu::Format::kR32Uint);
      gpu::TextureDesc ad;
      ad.width = AREATEX_WIDTH;
      ad.height = AREATEX_HEIGHT;
      ad.format = gpu::Format::kR8G8Unorm;
      ad.usage = gpu::kUsageSampled;
      ad.debug_name = "SMAA area table";
      smaa_area_ = backend_->CreateTexture(ad);
      gpu::TextureDesc sd;
      sd.width = SEARCHTEX_WIDTH;
      sd.height = SEARCHTEX_HEIGHT;
      sd.format = gpu::Format::kR8Unorm;
      sd.usage = gpu::kUsageSampled;
      sd.debug_name = "SMAA search table";
      smaa_search_ = backend_->CreateTexture(sd);
      REXLOG_INFO("gow2_present: SMAA pipelines {} {} {}, tables {} {}", smaa_edge_ != 0, smaa_weight_ != 0,
                  smaa_blend_ != 0, smaa_area_ != 0, smaa_search_ != 0);
    }
    if (!smaa_edge_ || !smaa_weight_ || !smaa_blend_ || !smaa_area_ || !smaa_search_ || !present_unpack_) return false;
    if (!smaa_tables_uploaded_) {
      backend_->UploadTexture(smaa_area_, 0, 0, areaTexBytes, AREATEX_PITCH, AREATEX_HEIGHT, 1);
      backend_->UploadTexture(smaa_search_, 0, 0, searchTexBytes, SEARCHTEX_PITCH, SEARCHTEX_HEIGHT, 1);
      smaa_tables_uploaded_ = true;
    }
    if (smaa_w_ != w || smaa_h_ != h) {
      for (gpu::TextureHandle* t : {&smaa_color_, &smaa_edges_, &smaa_weights_})
        if (*t) backend_->DestroyTexture(*t);
      gpu::TextureDesc d;
      d.width = w;
      d.height = h;
      d.format = gpu::Format::kR10G10B10A2Unorm;
      d.usage = gpu::kUsageSampled | gpu::kUsageStorage;
      d.debug_name = "SMAA colour";
      smaa_color_ = backend_->CreateTexture(d);
      d.format = gpu::Format::kR8G8B8A8Unorm;
      d.usage = gpu::kUsageSampled | gpu::kUsageRenderTarget;
      d.debug_name = "SMAA edges";
      smaa_edges_ = backend_->CreateTexture(d);
      d.debug_name = "SMAA weights";
      smaa_weights_ = backend_->CreateTexture(d);
      smaa_w_ = w;
      smaa_h_ = h;
    }
    return smaa_color_ && smaa_edges_ && smaa_weights_;
  }
  // The present image (present_out_, w x h packed words) anti-aliased in place.
  bool SmaaPass(uint32_t w, uint32_t h) {
    if (!EnsureSmaa(w, h)) return false;
    {
      gpu::ComputeBindings b;
      b.constants[0] = w;
      b.constants[1] = h;
      b.constant_count = 4;
      b.srv[0] = present_out_;
      b.uav_texture = smaa_color_;
      backend_->Dispatch(present_unpack_, b, (w + 7) / 8, (h + 7) / 8, 1);
    }
    const float rt[4] = {1.0f / float(w), 1.0f / float(h), float(w), float(h)};
    gpu::SamplerDesc s[32];
    for (uint32_t i = 0; i < 2; ++i) {
      s[i].mag_linear = s[i].min_linear = i == 0 ? 1 : 0;
      s[i].mip_linear = 0;
      for (gpu::SamplerAddress& a : s[i].address) a = gpu::SamplerAddress::kClamp;
      s[i].max_lod = 0.0f;
    }
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    auto pass = [&](gpu::TextureHandle target, gpu::PipelineHandle pipeline, std::initializer_list<gpu::TextureHandle> inputs) {
      gpu::TextureView views[96];
      uint32_t n = 0;
      for (gpu::TextureHandle t : inputs) views[n++].texture = t;
      backend_->SetRenderTargets(1, &target, 0);
      backend_->SetViewport({0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f});
      backend_->SetScissor({0, 0, int32_t(w), int32_t(h)});
      backend_->SetPipeline(pipeline);
      backend_->SetConstantsBoth(gpu::kCbSystem, rt, sizeof(rt));
      backend_->SetTextures(views, s, n, 2);
      backend_->Draw(3);
    };
    backend_->ClearColor(smaa_edges_, zero);
    pass(smaa_edges_, smaa_edge_, {smaa_color_});
    backend_->ClearColor(smaa_weights_, zero);
    pass(smaa_weights_, smaa_weight_, {smaa_edges_, smaa_area_, smaa_search_});
    pass(present_out_, smaa_blend_, {smaa_color_, smaa_weights_});
    tex_memo_valid_ = false;  // the bound views and constants changed
    backend_->SetTextures(nullptr, nullptr);
    ++smaa_frames_;
    return true;
  }
  std::deque<PresentImage> ours_images_;
  OursStats ours_stats_;

 public:
  bool PresentsOurs() const { return present_ours_; }
  // Job 171: every deferred guest-memory copy finishes (PM4 release points; same thread as our draws).
  void ReleaseToGuest() {
    if (backend_) backend_->FlushCopies();
    ++release_epoch_;  // job 174: the title may rewrite anything it saw finished from here on
  }
  bool WantsFrameImages() const override { return present_shadow_; }
  // The SDK's presented image (after its gamma pass), 1-2 swaps after its frame: compared with each of our last
  // finished images; the best match counts (our display lags the SDK's swap by about a frame).
  void OnFrameImage(const uint8_t* sdk, uint32_t width, uint32_t height, uint32_t row_pitch, uint64_t) override {
    if (!present_shadow_ || !backend_) return;
    ++present_stats_.sdk_images;
    uint64_t best = 0, best_total = 0, best_diff = 0, best_frame = 0;
    bool any = false;
    for (auto it = present_images_.rbegin(); it != present_images_.rend(); ++it) {
      if (!backend_->IsReadbackReady(it->readback)) continue;
      if (it->width != width || it->height != height) {
        ++present_stats_.size_mismatch;
        continue;
      }
      uint32_t pitch = 0;
      const uint8_t* ours = backend_->ReadbackData(it->readback, pitch);
      if (!ours) continue;
      any = true;
      uint64_t same = 0, maxd = 0;
      for (uint32_t y = 0; y < height; ++y) {
        const auto* a = reinterpret_cast<const uint32_t*>(sdk + size_t(y) * row_pitch);
        const auto* o = reinterpret_cast<const uint32_t*>(ours + size_t(y) * pitch);
        if (std::memcmp(a, o, size_t(width) * 4) == 0) {
          same += width;
          continue;
        }
        for (uint32_t x = 0; x < width; ++x) {
          const uint32_t va = a[x] & 0x3FFFFFFFu, vo = o[x] & 0x3FFFFFFFu;
          if (va == vo) {
            ++same;
            continue;
          }
          for (uint32_t c = 0; c < 3; ++c) {
            const int32_t ca = int32_t((va >> (10 * c)) & 0x3FF), co = int32_t((vo >> (10 * c)) & 0x3FF);
            maxd = std::max<uint64_t>(maxd, uint64_t(ca > co ? ca - co : co - ca));
          }
        }
      }
      if (same > best || !best_total) {
        best = same;
        best_total = uint64_t(width) * height;
        best_diff = maxd;
        best_frame = it->frame;
      }
      if (same == uint64_t(width) * height) break;  // exact: older ones cannot beat it
    }
    if (!any) {
      ++present_stats_.none_ready;
      return;
    }
    PresentStats& s = present_stats_;
    ++s.compared;
    s.identical_sum += best;
    s.pixel_sum += best_total;
    if (best * 1000 >= best_total * 999) ++s.good;
    if (best == best_total) ++s.exact;
    s.max_diff = std::max(s.max_diff, best_diff);
    s.worst = std::min(s.worst, 100.0 * double(best) / double(best_total));
    ++s.lag[std::min<uint64_t>(frame_ - best_frame, 7)];
  }

 private:
  void LogPresent() {
    COGBOX_TRACY_SCOPE("Present diagnostics");
    if (present_ours_) {
      const OursStats& o = ours_stats_;
      REXLOG_INFO("gow2_present: ours frames {}-{}: swaps {} | presented {} | refresh failed {} | gamma pass not run {} "
                  "| frame images delivered {} | no matching front-buffer copy {}",
                  frame_ + 1 - uint64_t(log_every_), frame_, display_stats_.swaps, o.presented, o.refresh_failed,
                  o.not_presented, o.images, display_stats_.no_source);
      ours_stats_ = {};
      display_stats_ = {};
      present_stats_ = {};
      return;
    }
    if (!present_shadow_) return;
    const PresentStats& s = present_stats_;
    REXLOG_INFO("gow2_present: shadow frames {}-{}: our images {} | SDK images {}, compared {}: >= 99.9% identical {}, "
                "exact {}, mean identical {:.4f}%, worst {:.4f}%, max channel difference {} (of 1023) | best match "
                "lag 0:{} 1:{} 2:{} 3:{} 4+:{} | none of ours ready {}, size mismatch {}, no gamma ramps {}, "
                "unsupported {}",
                frame_ + 1 - uint64_t(log_every_), frame_, s.made, s.sdk_images, s.compared, s.good, s.exact,
                s.pixel_sum ? 100.0 * double(s.identical_sum) / double(s.pixel_sum) : 0.0, s.compared ? s.worst : 0.0,
                s.max_diff, s.lag[0], s.lag[1], s.lag[2], s.lag[3], s.lag[4] + s.lag[5] + s.lag[6] + s.lag[7],
                s.none_ready, s.size_mismatch, s.no_gamma, s.unsupported);
    present_stats_ = {};
  }
  bool present_shadow_ = false;
  bool present_first_logged_ = false;
  bool tessellation_ = true, tess_logged_ = false;
  std::unordered_set<uint64_t> patch_kinds_logged_;
  std::unordered_set<uint64_t> resolve_view_logged_;  // Gears 3 job 019: resolve-only format views, logged once
  int32_t tess_scale_ = 1;
  uint32_t res_scale_ = 1;  // Stage 7: host pixels per guest pixel on each axis
  // Stage 7 phase B: resolve destinations at the resolution scale (same key as destinations_; the front buffer is
  // presented from them), and the scaled copy of guest memory (job 190): one R32_UINT chunk texture per 16 MB of guest
  // memory our resolves write, used as a flat word array (builtin_shaders.h kScaledWordsToMemCs).
  struct ScaledDest {
    gpu::TextureHandle texture = 0;
    uint32_t base = 0, pitch = 0, height = 0, words = 1;
    uint64_t last_used = 0, bytes = 0;  // patch 0032
  };
  bool scaled_resolves_ = false;
  gpu::PipelineHandle scaled_to_mem_through_ = 0;
  std::unordered_map<std::string, ScaledDest> scaled_dests_;
  uint32_t tex_memo_scaled_ = 0;
  bool scaled_partial_ = true;
  mutable uint64_t scaled_partial_hits_ = 0;
  uint64_t scaled_resolve_count_ = 0, scaled_mem_skipped_ = 0, scaled_window_writes_ = 0, scaled_fused_ = 0;
  bool fused_scaled_ = false;                  // job 238
  bool const_gen_skip_ = false;                // job 240
  uint64_t vs_gen_seen_ = ~0ull, ps_gen_seen_ = ~0ull;
  uint32_t vs_size_seen_ = 0, ps_size_seen_ = 0;
  uint32_t fused_kinds_ = 3;                   // job 239: 1 colour, 2 depth
  std::unordered_set<uint32_t> front_bases_;   // job 238: resolve destinations the presenter has shown
  // Chunks are 32 MB windows starting on a 1 MB boundary where a resolve first needed one (they may overlap; a write
  // goes to every window it touches, a read uses one window that holds the whole extent).
  static constexpr uint32_t kChunkBytes = 32u << 20, kChunkWidth = 16384;
  struct ScaledChunkInfo {
    uint32_t base = 0;
    gpu::TextureHandle texture = 0;
  };
  std::vector<ScaledChunkInfo> scaled_chunks_;
  gpu::PipelineHandle scaled_to_mem_ = 0, scaled_copy_ = 0;
  ScaledDest* ScaledDestination(const ::xenos::resolve::Info& info, uint32_t words) {
    const uint32_t w = std::max<uint32_t>(info.dest_pitch_aligned_div_32 * 32, 32),
                   h = std::max<uint32_t>(info.dest_height_aligned_div_32 * 32, 32);
    const std::string key = std::to_string(info.dest_base) + "/" + std::to_string(info.dest_format) + "/" +
                            std::to_string(w) + "x" + std::to_string(h);
    auto it = scaled_dests_.find(key);
    if (it != scaled_dests_.end()) {
      it->second.last_used = frame_;
      return it->second.texture ? &it->second : nullptr;
    }
    if (uint64_t(w) * res_scale_ * words > 16384 || uint64_t(h) * res_scale_ > 16384) return nullptr;
    gpu::TextureDesc td;
    td.width = w * res_scale_ * words;
    td.height = h * res_scale_;
    td.format = gpu::Format::kR32Uint;
    td.usage = gpu::kUsageStorage | gpu::kUsageSampled | (resolve_draws_ ? gpu::kUsageRenderTarget : 0u);  // 0119
    td.debug_name = "gow2 scaled resolve destination";
    gpu::TextureHandle texture = 0;
    if (REXCVAR_GET(gow2_renderer_resolve_reuse)) {
      auto reuse = cogbox::renderer::ReusableResolve(scaled_dests_, frame_, td.width, td.height,
          [&](const ScaledDest& value) {
            if (front_bases_.count(value.base)) return std::pair<uint32_t,uint32_t>{0,0};
            return std::pair{value.pitch * res_scale_ * value.words, value.height * res_scale_};
          });
      if (reuse != scaled_dests_.end()) {
        texture = reuse->second.texture;
        scaled_dests_.erase(reuse);
        ++dest_evict_.reused_scaled;
      }
    }
    if (!texture) texture = backend_->CreateTexture(td);
    if (!texture) return nullptr;
    ScaledDest& d = scaled_dests_[key];
    d.texture = texture;
    d.last_used = frame_;
    d.base = info.dest_base;
    d.pitch = w;
    d.height = h;
    d.words = words;
    d.bytes = uint64_t(td.width) * td.height * 4;
    return &d;
  }
  // A window holding all of [start, start + length), created if none does.
  const ScaledChunkInfo* ScaledChunkFor(uint32_t start, uint32_t length) {
    for (const ScaledChunkInfo& c : scaled_chunks_)
      if (start >= c.base && uint64_t(start) + length <= uint64_t(c.base) + kChunkBytes) return &c;
    if (length > kChunkBytes || scaled_chunks_.size() >= 24) return nullptr;
    ScaledChunkInfo c;
    c.base = std::min<uint32_t>(start & ~((1u << 20) - 1), (512u << 20) - kChunkBytes);
    // Job 235: 16 MB steps - any extent of up to 16 MB fits the window starting at its 16 MB boundary, and no address
    // is in more than two windows. Job 236: 32 tries a 32 MB boundary first (windows that do not overlap at all), then
    // 16, 8, ... - an in-between window only for an extent that crosses a 32 MB boundary. Otherwise the 1 MB rule.
    const uint32_t align_mb = uint32_t(std::clamp<int32_t>(REXCVAR_GET(gow2_renderer_scaled_window_align_mb), 1, 32));
    for (uint32_t mb = align_mb; mb > 1; mb >>= 1) {
      const uint32_t aligned = std::min<uint32_t>(start & ~((mb << 20) - 1), (512u << 20) - kChunkBytes);
      if (uint64_t(start) + length <= uint64_t(aligned) + kChunkBytes && start >= aligned) {
        c.base = aligned;
        break;
      }
    }
    gpu::TextureDesc td;
    td.width = kChunkWidth;
    td.height = uint32_t((uint64_t(kChunkBytes) * res_scale_ * res_scale_ / 4 + kChunkWidth - 1) / kChunkWidth);
    td.format = gpu::Format::kR32Uint;
    td.usage = gpu::kUsageStorage | gpu::kUsageSampled;
    td.debug_name = "gow2 scaled guest memory chunk";
    c.texture = backend_->CreateTexture(td);
    REXLOG_INFO("gow2_renderer: scaled memory chunk {:08X}-{:08X}: {}x{} words{}", c.base, c.base + kChunkBytes,
                td.width, td.height, c.texture ? "" : " - CREATION FAILED");
    if (!c.texture) return nullptr;
    // Words already held by overlapping windows come over first, so every window holding an address agrees.
    for (const ScaledChunkInfo& o : scaled_chunks_) {
      const uint32_t a = std::max(c.base, o.base);
      const uint64_t e = std::min(uint64_t(c.base) + kChunkBytes, uint64_t(o.base) + kChunkBytes);
      if (!scaled_copy_ || e <= a) continue;
      const uint64_t s2 = uint64_t(res_scale_) * res_scale_;
      const uint32_t count = uint32_t((e - a) * s2 / 4);
      const uint32_t per_row = 256u * 4096u, rows = (count + per_row - 1) / per_row;
      gpu::ComputeBindings b;
      b.constants[0] = uint32_t((a - o.base) * s2 / 4);
      b.constants[1] = uint32_t((a - c.base) * s2 / 4);
      b.constants[2] = count;
      b.constants[3] = kChunkWidth;
      b.constants[4] = per_row;
      b.constant_count = 8;
      b.srv[0] = o.texture;
      b.uav_texture = c.texture;
      ResolveDispatch(scaled_copy_, b, 4096, rows, 1);
    }
    scaled_chunks_.push_back(c);
    return &scaled_chunks_.back();
  }
  // After a scaled resolve into d: its rectangle (guest dx, dy, w, h) into the scaled copy of guest memory, in every
  // window it touches. False if no window could hold it (the pages then stay 1x only).
  bool ScaledToMemory(const ScaledDest& d, const ::xenos::resolve::Info& info, uint32_t dx, uint32_t dy, uint32_t w,
                      uint32_t h, uint32_t endian, rr::TextureCache::ResolveThrough& through) {
    ++scaled_resolve_count_;
    if (!scaled_to_mem_ || !info.dest_extent_length) return false;
    if (!ScaledChunkFor(info.dest_extent_start, info.dest_extent_length)) {
      ++scaled_mem_skipped_;
      return false;
    }
    const uint64_t s0 = info.dest_extent_start, s1 = s0 + info.dest_extent_length;
    const bool direct = scaled_to_mem_through_ && endian <= 3 &&
        textures_->FindScaledResolveThrough(info.dest_base, d.pitch, d.words == 2 ? 3u : 2u, dx, dy, w, h, through);
    for (const ScaledChunkInfo& c : scaled_chunks_) {
      if (s1 <= c.base || s0 >= uint64_t(c.base) + kChunkBytes) continue;
      gpu::ComputeBindings b;
      b.constants[0] = info.dest_base;
      b.constants[1] = d.pitch;
      b.constants[2] = dx;
      b.constants[3] = dy;
      b.constants[4] = w;
      b.constants[5] = h;
      b.constants[6] = d.words == 2 ? 3u : 2u;
      b.constants[7] = endian;
      b.constants[8] = res_scale_;
      b.constants[9] = c.base;
      b.constants[10] = kChunkWidth;
      b.constants[11] = kChunkBytes;
      b.constant_count = 12;
      b.srv[0] = d.texture;
      b.uav_texture = c.texture;
      if (direct) {
        b.constants[12] = through.code;
        b.constants[13] = through.endian;
        b.constants[14] = through.width;
        b.constants[15] = through.height;
        b.constant_count = 16;
        b.uav2_texture = through.texture;
      }
      ResolveDispatch(direct ? scaled_to_mem_through_ : scaled_to_mem_, b,
                      (w * res_scale_ + 7) / 8, (h * res_scale_ + 7) / 8, 1);
      ++scaled_window_writes_;  // job 235: windows written per scaled resolve
    }
    return true;
  }
  // Job 238: the fused scaled resolve - the resolve shader itself (b set up for its 1x pass) writes every host pixel of
  // the rectangle into each window touching the destination. False if no window could hold it.
  bool FusedScaledResolve(gpu::PipelineHandle pipe, gpu::ComputeBindings b, const ::xenos::resolve::Info& info,
                          uint32_t w, uint32_t h, uint32_t words, uint32_t endian, uint32_t src_scale = 0) {
    static_assert(kChunkWidth == 16384 && kChunkBytes == (32u << 20), "builtin_shaders.h kHlslScaledChunk");
    ++scaled_resolve_count_;
    if (!scaled_to_mem_ || !info.dest_extent_length) return false;
    if (!ScaledChunkFor(info.dest_extent_start, info.dest_extent_length)) {
      ++scaled_mem_skipped_;
      return false;
    }
    const uint32_t pitch = std::max<uint32_t>(info.dest_pitch_aligned_div_32 * 32, 32);
    const uint64_t s0 = info.dest_extent_start, s1 = s0 + info.dest_extent_length;
    b.constants[13] = 1u | (src_scale << 8);  // patch 0143: 0 = the source is at the destination's scale
    b.constants[10] = info.dest_base;
    b.constants[14] = (pitch & 0xFFFFu) | 0x80000000u;
    b.constants[15] = (endian & 3u) | ((words == 2 ? 3u : 2u) << 4);
    for (const ScaledChunkInfo& c : scaled_chunks_) {
      if (s1 <= c.base || s0 >= uint64_t(c.base) + kChunkBytes) continue;
      b.constants[11] = c.base;
      b.uav_texture = c.texture;
      ResolveDispatch(pipe, b, (w * res_scale_ + 7) / 8, (h * res_scale_ + 7) / 8, 1);
      ++scaled_window_writes_;
      ++scaled_fused_;
    }
    return true;
  }
  bool FindScaledSource(uint32_t base, uint32_t extent, rr::TextureCache::ScaledSource& out) const {
    const ScaledChunkInfo* chunk = nullptr;
    for (const ScaledChunkInfo& c : scaled_chunks_)
      if (!chunk && base >= c.base && uint64_t(base) + extent <= uint64_t(c.base) + kChunkBytes) chunk = &c;
    if (!chunk) {
      out.reason = 1;
      return false;
    }
    if (!watch_->ScaledCurrent(base, extent)) {
      out.reason = 4;
      bool first = true, partial_ok = scaled_partial_;
      const uint32_t p0 = base >> rr::GuestWatch::kPageShift, p1 = (base + extent - 1) >> rr::GuestWatch::kPageShift;
      for (uint32_t p = p0; p <= p1; ++p) {
        if (watch_->ScaledCurrent(p)) {
          ++out.pages_scaled;
          continue;
        }
        if (watch_->GpuCurrent(p)) ++out.pages_gpu; else ++out.pages_other;
        if (!watch_->NeverResolved(p) || p == p0) partial_ok = false;
        if (first) out.first_bad = (p << rr::GuestWatch::kPageShift) - (base & ~0xFFFu);
        first = false;
      }
      // Job 193: a view reaching past what our resolves wrote (ch10's 1280x720 8888 at 0FE00000 over 640x360 resolves)
      // - the rest of its pages were never resolved into, so neither path has the game's data there; take it at the
      // scale when at least half its pages are scaled-current.
      if (!partial_ok || out.pages_scaled < out.pages_other) return false;
      ++scaled_partial_hits_;
    }
    out.words = chunk->texture;
    out.chunk_base = chunk->base;
    out.chunk_width = kChunkWidth;
    out.chunk_bytes = kChunkBytes;
    out.gen = watch_->Version(base, extent);
    return true;
  }
  // Guest-pixel rectangle (resolve registers) -> host pixels.
  // Per-target scale (notes/53b): a target below the scale threshold is at 1x, so rectangles use the target's scale.
  rr::PixelRect HostRect(const rr::RenderTarget& t, int32_t x0, int32_t y0, int32_t x1, int32_t y1) const {
    return rr::GuestToHostRect({uint32_t(std::max(x0, 0)), uint32_t(std::max(y0, 0)), uint32_t(std::max(x1, 0)),
                                uint32_t(std::max(y1, 0))}, std::max(t.scale, 1u));
  }
  static gpu::Rect ToGpuRect(const rr::PixelRect& r) {
    return gpu::Rect{int32_t(r.x0), int32_t(r.y0), int32_t(r.x1), int32_t(r.y1)};
  }
  gpu::PipelineHandle present_gamma_ = 0;
  gpu::TextureHandle ramp_texture_ = 0, present_out_ = 0;
  uint32_t present_out_w_ = 0, present_out_h_ = 0;
  uint64_t ramp_table_version_ = ~0ull, ramp_pwl_version_ = ~0ull;
  std::deque<PresentImage> present_images_;
  PresentStats present_stats_;
  void ShowDisplay(uint32_t fb_phys) {
    if (backend_) backend_->FlushCopies();  // job 171: we may write guest memory below
    if (display_wait_ && !frame_fences_.empty()) backend_->WaitForFence(frame_fences_.back());
    // The newest finished image wins; older finished ones are dropped.
    int newest = -1;
    for (size_t i = 0; i < display_pending_.size(); ++i)
      if (backend_->IsReadbackReady(display_pending_[i].readback)) newest = int(i);
    if (newest < 0) {
      if (!display_pending_.empty()) ++display_stats_.not_ready;
      return;
    }
    for (int i = 0; i < newest; ++i) {
      backend_->ReleaseReadback(display_pending_[size_t(i)].readback);
      ++display_stats_.dropped;
    }
    DisplayPending p = display_pending_[size_t(newest)];
    display_pending_.erase(display_pending_.begin(), display_pending_.begin() + newest + 1);
    uint32_t row_pitch = 0;
    const uint8_t* data = backend_->ReadbackData(p.readback, row_pitch);
    if (data) {
      // Same addressing as CompareColour: guest word = Swap32(our word, endian) at dest_base + TiledOffset2D. Written
      // at the CURRENT swap's front buffer (the game may alternate buffers; the layout is the same).
      const auto& in = p.src.info;
      const uint32_t w = uint32_t(p.src.rect.x1 - p.src.rect.x0), h = uint32_t(p.src.rect.y1 - p.src.rect.y0);
      const uint32_t pitch_blocks = in.dest_pitch_aligned_div_32 * 32;
      const uint32_t dx0 = uint32_t(p.src.rect.x0), dy0 = uint32_t(p.src.rect.y0);
      uint8_t* base = memory_->TranslatePhysical<uint8_t*>(fb_phys);
      int64_t lo = INT64_MAX, hi = 0;
      for (uint32_t y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(data + size_t(y) * row_pitch);
        for (uint32_t x = 0; x < w; ++x) {
          const int64_t off = ::xenos::texture::TiledOffset2D(int32_t(dx0 + x), int32_t(dy0 + y), pitch_blocks,
                                                              in.dest_bpp_log2);
          if (off < 0) continue;
          // Mark: magenta, alpha 255, in 8888 any channel order (R=B=FF, G=00) - only for 8888 destinations.
          const bool mark = display_mark_ && x < 32 && y < 32 && (in.dest_format == 6 || in.dest_format == 50);
          const uint32_t word = Swap32(mark ? 0xFFFF00FFu : row[x], p.src.endian);
          std::memcpy(base + off, &word, 4);
          lo = std::min(lo, off);
          hi = std::max(hi, off + 4);
        }
      }
      if (hi > lo) {
        // Tell the SDK this range changed (SDK patch 18): its shared memory and texture cache reload it for the swap.
        // (Job 122: Memory::TriggerPhysicalMemoryCallbacks did nothing here - after a resolve those pages are not
        // watched, so the screenshot stayed the emulator's, byte for byte.)
        rex::graphics::NotifyGuestRangeWrittenByObserver(fb_phys + uint32_t(lo), uint32_t(hi - lo));
        ++display_stats_.shown;
        display_stats_.lag_sum += frame_ - p.frame;
        display_stats_.bytes += uint64_t(hi - lo);
      }
    } else {
      ++display_stats_.bad;
    }
    backend_->ReleaseReadback(p.readback);
  }
  void LogDisplay() {
    COGBOX_TRACY_SCOPE("Display diagnostics");
    if (!display_ours_ || present_ours_) return;  // 5C-2: LogPresent reports the swaps
    const DisplayStats& d = display_stats_;
    REXLOG_INFO("gow2_renderer: display (ours) frames {}-{}: swaps {} shown {} (lag {:.2f} frames, {:.1f} MB written) | "
                "no matching copy {} | not ready {} | dropped older {} | failed {}",
                frame_ + 1 - uint64_t(log_every_), frame_, d.swaps, d.shown,
                d.shown ? double(d.lag_sum) / double(d.shown) : 0.0, double(d.bytes) / 1048576.0, d.no_source,
                d.not_ready, d.dropped, d.bad);
    display_stats_ = {};
  }
  // ---- patch 0020 (Canary): RequestRange before our resolve writes the mirror; readback_resolve after ----
  // Canary SharedMemory::RequestRange: the pages a resolve writes hold the CPU's newest bytes in the GPU copy before the
  // resolve shader writes its part, so the bytes of those pages it does not cover (partial tiles, the page before the
  // extent's start, the pitch padding) are right once the whole extent counts as GPU-written. Pages already GPU-current
  // hold newer bytes than the CPU copy and are left alone. Watch first, then compare versions: a CPU write after the
  // watch bumps the version again.
  void PrepareMirrorForResolve(uint32_t start, uint32_t length) {
    start &= 0x1FFFFFFFu;
    if (!length || !memory_) return;
    if (uint64_t(start) + length > (512ull << 20)) length = uint32_t((512ull << 20) - start);
    constexpr uint32_t kShift = rr::GuestWatch::kPageShift;
    if (mirror_versions_.empty()) mirror_versions_.assign(rr::GuestWatch::kPages, ~0u);
    watch_->Watch(start, length);
    const uint8_t* base = memory_->TranslatePhysical<const uint8_t*>(0);
    if (!base) return;
    const uint32_t first = start >> kShift, last = (start + length - 1) >> kShift;
    uint32_t run = UINT32_MAX;
    auto flush = [&](uint32_t end_page) {
      if (run == UINT32_MAX) return;
      const uint32_t a = run << kShift, b = end_page << kShift;
      backend_->UploadGuestMemory(a, base + a, b - a);
      rb_stats_.prepared_bytes += b - a;
      ++rb_stats_.prepared_runs;
      run = UINT32_MAX;
    };
    for (uint32_t p = first; p <= last; ++p) {
      const uint32_t v = watch_->PageVersion(p);
      if (!watch_->GpuCurrent(p) && mirror_versions_[p] != v) {
        mirror_versions_[p] = v;
        if (run == UINT32_MAX) run = p;
      } else {
        flush(p);
      }
    }
    flush(last + 1);
  }
  // Canary IssueCopy_ReadbackResolvePath: the written extent is copied from the mirror into a readback buffer and then
  // into guest memory through the physical mapping (no watch callback, so the pages stay GPU-current; both copies hold
  // the same bytes). full: wait for the GPU now. fast: land the PREVIOUS copy of the same destination (base, length)
  // now - one frame old, no stall; the first copy of a destination waits, as Canary's cache miss does.
  void LandResolveReadback(gpu::ReadbackHandle h, uint32_t start, uint32_t length) {
    uint32_t pitch = 0;
    const uint8_t* data = backend_->ReadbackData(h, pitch);
    uint8_t* dst = data ? memory_->TranslatePhysical<uint8_t*>(start) : nullptr;
    if (!dst) {
      ++rb_stats_.failed;
      return;
    }
    std::memcpy(dst, data, length);
    rex::graphics::NotifyGuestRangeWrittenByObserver(start, length);
    ++rb_stats_.landed;
    rb_stats_.landed_bytes += length;
  }
  void ReadbackResolve(uint32_t start, uint32_t length) {
    start &= 0x1FFFFFFFu;
    if (!length || !memory_) return;
    if (uint64_t(start) + length > (512ull << 20)) length = uint32_t((512ull << 20) - start);
    const gpu::ReadbackHandle h = backend_->ReadbackGuest(start, length);
    if (!h) {
      ++rb_stats_.failed;
      return;
    }
    ++rb_stats_.readbacks;
    if (resolve_readback_ == 2) {
      ++rb_stats_.waits;
      backend_->WaitForFence(backend_->Submit());
      LandResolveReadback(h, start, length);
      backend_->ReleaseReadback(h);
      return;
    }
    const uint64_t key = (uint64_t(start) << 32) | length;
    auto it = resolve_readback_prev_.find(key);
    if (it == resolve_readback_prev_.end()) {
      ++rb_stats_.waits;  // first copy of this destination: wait, as Canary does when the previous buffer is missing
      backend_->WaitForFence(backend_->Submit());
      LandResolveReadback(h, start, length);
      resolve_readback_prev_.emplace(key, h);
      return;
    }
    if (backend_->IsReadbackReady(it->second)) {
      LandResolveReadback(it->second, start, length);
      backend_->ReleaseReadback(it->second);
    } else {
      // The previous copy is still in flight (the same destination twice in a frame): it is not landed (Canary would
      // land that buffer's older contents) and is released at a swap once done, so nothing waits here.
      ++rb_stats_.not_ready;
      resolve_readback_retire_.push_back(it->second);
    }
    it->second = h;
  }
  void RetireResolveReadbacks() {
    for (size_t i = 0; i < resolve_readback_retire_.size();) {
      if (backend_->IsReadbackReady(resolve_readback_retire_[i])) {
        backend_->ReleaseReadback(resolve_readback_retire_[i]);
        resolve_readback_retire_[i] = resolve_readback_retire_.back();
        resolve_readback_retire_.pop_back();
      } else {
        ++i;
      }
    }
  }

  // ---- takeover T2: our resolves into the GPU guest-memory mirror ----
  void WriteResolveToGuest(gpu::TextureHandle dest, const ::xenos::resolve::Info& info, uint32_t endian, uint32_t words,
                           uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, bool scaled = false,
                           const rr::TextureCache::ResolveThrough* scaled_through = nullptr) {
    if (scaled_resolves_ && frame_ + 1 == 901 && REXCVAR_GET(gow2_renderer_scaled_miss_log))  // job 192 (0029: diag only): every resolve of one frame, to place the scaled-memory misses
      REXLOG_INFO("gow2_renderer: frame 901 resolve: dest {:08X} fmt {} pitch {} extent {:08X}+{:X} rect {},{} {}x{} "
                  "words {} endian {} bpp_log2 {} scaled {}", info.dest_base, info.dest_format,
                  info.dest_pitch_aligned_div_32 * 32, info.dest_extent_start, info.dest_extent_length, dx, dy, w, h,
                  words, endian, info.dest_bpp_log2, scaled ? 1 : 0);
    if (!resolves_to_guest_ || !words_to_guest_ || !dest || !info.dest_extent_length) return;
    // Patch 0030: any bpp the resolve shader wrote words for (1 word per pixel for 8 / 16 / 32 bpp, 2 for 64, 4 for 128);
    // 8in64 for 64 / 128 bpp and 8in128 for 128 bpp handled by kWordsToGuestCs.
    const uint32_t expect_words = info.dest_bpp_log2 == 4 ? 4u : info.dest_bpp_log2 == 3 ? 2u : 1u;
    const bool bpp_ok = words == expect_words &&
                        (info.dest_bpp_log2 >= 2 || REXCVAR_GET(gow2_renderer_resolve_other_bpp)) &&
                        (info.dest_bpp_log2 != 4 || REXCVAR_GET(gow2_renderer_resolve_other_bpp));
    const bool endian_ok = endian <= 3 || (REXCVAR_GET(gow2_renderer_resolve_other_bpp) &&
                                           ((endian == 4 && info.dest_bpp_log2 >= 3) || (endian == 5 && info.dest_bpp_log2 == 4)));
    if (!bpp_ok || !endian_ok) {
      ++t2_stats_.skipped;
      return;
    }
    if (prepare_mirror_) PrepareMirrorForResolve(info.dest_extent_start, info.dest_extent_length);  // patch 0020
    gpu::ComputeBindings b;
    b.constants[0] = info.dest_base;
    b.constants[1] = info.dest_pitch_aligned_div_32 * 32;
    b.constants[2] = dx;
    b.constants[3] = dy;
    b.constants[4] = w;
    b.constants[5] = h;
    b.constants[6] = info.dest_bpp_log2;  // patch 0030: 0..4 (was 2 or 3)
    b.constants[7] = endian;
    b.constant_count = 8;
    b.srv[0] = dest;
    b.guest_u0 = true;
    // Job 047: the cached texture loaded from this memory, written by the same dispatch (not for scaled resolves, whose
    // textures load from the scaled copy).
    rr::TextureCache::ResolveThrough through;
    const bool through_on = words_to_guest_through_ && REXCVAR_GET(gow2_renderer_resolve_through) && !scaled &&
                            (info.dest_bpp_log2 == 2 || info.dest_bpp_log2 == 3) &&
                            endian <= 3 &&
                            textures_->FindResolveThrough(info.dest_base, info.dest_pitch_aligned_div_32 * 32,
                                                          words == 2 ? 3u : 2u, dx, dy, w, h, through);
    if (through_on) {
      b.constants[8] = through.code;
      b.constants[9] = through.endian;
      b.constants[10] = through.width;
      b.constants[11] = through.height;
      b.constant_count = 12;
      b.uav2_texture = through.texture;
      ResolveDispatch(words_to_guest_through_, b, (w + 7) / 8, (h + 7) / 8, 1);
    } else {
      ResolveDispatch(words_to_guest_, b, (w + 7) / 8, (h + 7) / 8, 1);
    }
    if (REXCVAR_GET(gow2_renderer_watch_resolves) || prepare_mirror_)  // patch 0014 / 0020: arm before marking
      watch_->Watch(info.dest_extent_start, info.dest_extent_length);
    watch_->MarkGpuWritten(info.dest_extent_start, info.dest_extent_length, scaled);
    if (resolve_readback_) ReadbackResolve(info.dest_extent_start, info.dest_extent_length);  // patch 0020
    if (through_on || (scaled && scaled_through && scaled_through->texture)) {
      if (!through_on) through = *scaled_through;
      textures_->ResolveThroughWritten(through, info.dest_extent_start, info.dest_extent_length);
      if (through_check_frame_ && frame_ + 1 == through_check_frame_ && through_checks_started_ < 12) {
        ++through_checks_started_;
        // Job 048: the old way's texture, compared with ours on the GPU (job 047's read-back of both textures made the
        // command list fail to close); only the 4x1 result is read back.
        ThroughCheck c;
        c.copy = textures_->UntileCopyForCheck(through, c.texel_bytes);
        if (c.copy && through_compare_) {
          gpu::TextureDesc rd;
          rd.width = 4;
          rd.height = 1;
          rd.format = gpu::Format::kR32Uint;
          rd.usage = gpu::kUsageSampled | gpu::kUsageStorage;
          rd.debug_name = "gow2 resolve-through check result";
          c.result = backend_->CreateTexture(rd);
        }
        if (c.copy && c.result) {
          gpu::ComputeBindings cb;
          cb.constants[0] = through.width;
          cb.constants[1] = through.height;
          cb.constant_count = 4;
          cb.srv[0] = through.texture;
          cb.srv[1] = c.copy;
          cb.uav_texture = c.result;
          cb.constants[2] = 1;
          backend_->Dispatch(through_compare_, cb, 1, 1, 1);
          cb.constants[2] = 0;
          backend_->Dispatch(through_compare_, cb, (through.width + 7) / 8, (through.height + 7) / 8, 1);
          cb.constants[2] = 2;
          backend_->Dispatch(through_compare_, cb, (through.width + 7) / 8, (through.height + 7) / 8, 1);
          c.ours = backend_->ReadbackTexture(c.result, gpu::Rect{0, 0, 4, 1});
          c.w = through.width;
          c.h = through.height;
          c.code = through.code;
          c.base = info.dest_base;
          c.rect = fmt::format("{},{} {}x{}{}", dx, dy, w, h, through.covers ? " covers" : "");
          through_checks_.push_back(c);
        } else {
          if (c.copy) backend_->DestroyTexture(c.copy);
          if (c.result) backend_->DestroyTexture(c.result);
        }
      }
    }
    recent_resolves_[recent_resolve_next_++ % recent_resolves_.size()] =
        RecentResolve{frame_ + 1, info.dest_extent_start, info.dest_extent_length, info.dest_base, info.dest_format, w, h,
                      words};
    ++t2_stats_.written;
    t2_stats_.bytes += uint64_t(w) * h * 4 * words;
  }
  // Job 150 diagnostic: our last 256 resolves, to name the one that overwrote a stale texture.
  struct RecentResolve {
    uint64_t frame = 0;
    uint32_t start = 0, length = 0, base = 0, format = 0, w = 0, h = 0, words = 0;
  };
  std::array<RecentResolve, 256> recent_resolves_{};
  uint64_t recent_resolve_next_ = 0;
  uint32_t stale_lines_ = 0;
  void ReportStale(const rr::TextureCache::StaleReload& s) {
    if (stale_lines_ >= 400) return;
    ++stale_lines_;
    std::string hits;
    uint32_t n = 0;
    const uint64_t s0 = s.base_start, s1 = uint64_t(s.base_start) + s.base_length;
    const uint64_t m0 = s.mip_start, m1 = uint64_t(s.mip_start) + s.mip_length;
    for (size_t k = 0; k < recent_resolves_.size() && n < 4; ++k) {
      const RecentResolve& rr_ = recent_resolves_[(recent_resolve_next_ + recent_resolves_.size() - 1 - k) % recent_resolves_.size()];
      if (!rr_.length) continue;
      const uint64_t a = rr_.start, b = uint64_t(rr_.start) + rr_.length;
      if ((a < s1 && b > s0) || (s.mip_length && a < m1 && b > m0)) {
        hits += fmt::format(" | resolve frame {} to {:08X}-{:08X} (base {:08X}) fmt {} {}x{} words {}", rr_.frame, rr_.start,
                            rr_.start + rr_.length, rr_.base, rr_.format, rr_.w, rr_.h, rr_.words);
        ++n;
      }
    }
    REXLOG_INFO("gow2_renderer: STALE texture reloaded at frame {} (unchanged since {}): base {:08X}+{:X} mip {:08X}+{:X} "
                "fmt {} {}x{} | pages by our resolves {}{}",
                s.frame, s.loaded_frame, s.base_start, s.base_length, s.mip_start, s.mip_length, s.guest_format, s.width,
                s.height, s.gpu_pages, hits.empty() ? " | no overlapping resolve in our last 256 (CPU write?)" : hits);
  }
  struct T2Stats {
    uint64_t written = 0, skipped = 0, bytes = 0, seen_colour = 0, seen_depth = 0;
    uint64_t skipped_format = 0, skipped_endian = 0;  // patch 0030: colour resolves ResolveColour still skips
  };
  bool resolves_to_guest_ = false;
  bool canary_resolve_rules_ = false;  // patch 0023: --gow2_renderer_resolve_rules=canary
  bool depth_alias_drop_ = false;      // patch 0025
  bool alpha_test_rt0_ = false;        // patch 0027
  // ---- patch 0020: Canary readback_resolve + RequestRange for our T2 resolves ----
  uint32_t resolve_readback_ = 0;  // 0 none, 1 fast, 2 full
  bool prepare_mirror_ = false;
  std::vector<uint32_t> mirror_versions_;  // per 4 KB page: the watch version the mirror last got CPU bytes at (~0 never)
  std::unordered_map<uint64_t, gpu::ReadbackHandle> resolve_readback_prev_;  // fast: (base << 32 | length) -> last copy
  std::vector<gpu::ReadbackHandle> resolve_readback_retire_;  // fast: replaced copies still in flight
  struct ReadbackStats {
    uint64_t prepared_bytes = 0, prepared_runs = 0, readbacks = 0, landed = 0, landed_bytes = 0, waits = 0, failed = 0,
             not_ready = 0;
  };
  ReadbackStats rb_stats_;
  gpu::PipelineHandle words_to_guest_ = 0;
  gpu::PipelineHandle words_to_guest_through_ = 0;  // job 047 resolve-through (0 = off)
  // ---- Upscaler probe (--gow2_upscale_probe, notes/64): which draws carry the camera matrix ----
  struct ProbeGroup {
    uint64_t hash = 0;
    float c[16] = {};
    uint32_t depth_base = 0, depth_pitch = 0, depth_scale = 0;
    uint32_t draws = 0, depth_writes = 0, perspective = 0, reading = 0, first_draw = 0, last_draw = 0;
  };
  uint32_t upscale_probe_ = 0;
  std::vector<ProbeGroup> probe_groups_;
  std::vector<std::string> probe_resolves_;
  std::vector<std::string> probe_draws_;  // the first depth-writing camera draws: vertex data address + c4-c15
  bool ProbeFrame() const { return upscale_probe_ && (frame_ + 1) % upscale_probe_ == 0; }
  void NoteProbeDraw(const uint32_t* r, const rr::GuestShader& vsg, const rr::PipelineState& ps,
                     const rr::RenderTarget* depth, bool depth_writes) {
    float c[16];
    std::memcpy(c, &r[kRegFloatConstVs], sizeof(c));
    const uint64_t h = XXH3_64bits(c, sizeof(c)) ^ (depth ? (uint64_t(depth->key.base_tiles) << 52) : 0);
    ProbeGroup* g = nullptr;
    for (ProbeGroup& x : probe_groups_)
      if (x.hash == h) g = &x;
    if (!g) {
      if (probe_groups_.size() >= 512) return;
      g = &probe_groups_.emplace_back();
      g->hash = h;
      std::memcpy(g->c, c, sizeof(c));
      if (depth) g->depth_base = depth->key.base_tiles, g->depth_pitch = depth->key.pitch_pixels, g->depth_scale = depth->scale;
      g->first_draw = draw_index_;
    }
    ++g->draws;
    g->last_draw = draw_index_;
    // The camera matrix has no translation (UE3 renders camera-relative): log what else the first few depth-writing
    // draws into a full-size target carry (c4-c15 and their vertex data), to find the camera position / per-object
    // transforms across frames of a moving-camera run.
    if (depth_writes && depth && depth->guest_width >= 1024 && probe_draws_.size() < 6 && vsg.Reads(0)) {
      std::string v;
      for (int k = 16; k < 64; ++k) v += std::format("{}{:.6g}", k % 4 ? " " : (k > 16 ? " | " : ""), std::bit_cast<float>(r[kRegFloatConstVs + k]));
      uint32_t vb = 0;
      for (uint32_t i = 0; i < 96 && !vb; ++i)
        if ((vsg.vfetch_constants[i >> 5] >> (i & 31)) & 1) vb = r[kRegFetchConst + 2 * i] & 0x1FFFFFFCu;
      probe_draws_.push_back(std::format("draw {} vb {:08X} vs {:016X} c4-c15 = {}", draw_index_, vb, vsg.ucode_hash, v));
    }
    if (depth_writes) ++g->depth_writes;
    if (!ps.viewport.xy_divided_by_w && !ps.viewport.clip_disabled) ++g->perspective;  // XY not pre-divided by W
    if (vsg.Reads(0) && vsg.Reads(1) && vsg.Reads(2) && vsg.Reads(3)) ++g->reading;
  }
  void LogProbeFrame() {
    std::sort(probe_groups_.begin(), probe_groups_.end(), [](const ProbeGroup& a, const ProbeGroup& b) { return a.draws > b.draws; });
    REXLOG_INFO("gow2_upscale_probe: frame {}: {} draws in {} c0-c3 groups, {} resolves", frame_, draw_index_,
                probe_groups_.size(), probe_resolves_.size());
    for (size_t i = 0; i < probe_groups_.size() && i < 8; ++i) {
      const ProbeGroup& g = probe_groups_[i];
      std::string v;
      for (int k = 0; k < 16; ++k) v += std::format("{}{:.6g}", k % 4 ? " " : (k ? " | " : ""), g.c[k]);
      REXLOG_INFO("gow2_upscale_probe:   group {}: draws {} ({}-{}), depth writes {}, perspective {}, VS reads c0-c3 {}; "
                  "depth target base {} pitch {} scale {}; c0-c3 = {}", i, g.draws, g.first_draw, g.last_draw,
                  g.depth_writes, g.perspective, g.reading, g.depth_base, g.depth_pitch, g.depth_scale, v);
    }
    for (const std::string& s : probe_resolves_) REXLOG_INFO("gow2_upscale_probe:   {}", s);
    for (const std::string& s : probe_draws_) REXLOG_INFO("gow2_upscale_probe:   {}", s);
    probe_draws_.clear();
    probe_groups_.clear();
    probe_resolves_.clear();
  }

  // ---- Upscalers: DLSS / FSR on the 3D scene (--gow2_upscaler, notes/64) ----
  // Per frame: the camera matrix is c0-c3 of the frame's first depth-writing, perspective draw (XY not pre-divided by
  // W) into a full-size depth target whose vertex shader reads c0-c3 (UE3's view-projection; the depth prepass). Draws
  // into that depth target with the same c0-c3 get the frame's sub-pixel jitter on their viewport, until the scene
  // colour is resolved (the frame's first full-screen resolve of an fp16 / 7e3 colour target); screen-space passes
  // (pre-divided XY, other c0-c3) are left alone so their per-pixel depth lookups stay aligned. At that resolve: the
  // depth target is copied to R32 float (guest convention), camera motion is computed from it and last frame's camera,
  // the upscaler runs on the colour target, and the resolve reads its output instead - the post passes, HUD and
  // presentation then see the anti-aliased scene. Jitter only starts once the previous frame's upscale succeeded, so a
  // frame without a scene (menus, loading) never shows a shifted image.
  // Guest pixels. 512 high, not 576: ultrawide (--gow2_ultrawide) renders 1280x550 at 1x, and DLSS / FSR never started
  // there with 576 (job 261).
  static constexpr uint32_t kSceneMinWidth = 1024, kSceneMinHeight = 512;
  std::unique_ptr<rr::upscale::Upscaler> upscaler_;
  // Patch 0145: the camera tracking and the depth / motion passes run when an upscaler is selected or with
  // --gow2_fg_inputs; inputs only = no upscaler, no jitter, the resolve reads the scene as it is.
  bool up_active_ = false, up_inputs_only_ = false;
  // Patch 0145: the newest frame's frame-generation inputs, at the render size. The textures are rewritten by the next
  // frame's scene resolve, so a consumer copies them before then (the presenter's mailbox slot, plan step 3).
  struct FgInputs {
    gpu::TextureHandle depth = 0, motion = 0;  // R32F depth (guest convention), R16G16F motion (previous - current, px)
    uint32_t width = 0, height = 0;
    float jitter_x = 0.0f, jitter_y = 0.0f;
    float vertical_fov = 1.0f, camera_near = 10.0f, camera_far = 100000.0f, frame_ms = 16.7f;
    bool reset = true, hdr = false, infinite = false;
    uint64_t frame = 0;  // the frame (frame_ + 1) they belong to; 0 = none yet
    // Patch 0147: camera vectors from the camera matrix's rows (x, y and w rows: right, up, forward) and the position
    // summed from the frames' camera moves (UE3 renders camera-relative; only the moves are known).
    float right[3] = {}, up[3] = {}, forward[3] = {}, position[3] = {};
  };
  float fg_position_[3] = {};  // patch 0147: the summed camera moves
  // Patch 0147: per guest-output texture (the presenter's mailbox images), copies of the inputs of the frame written
  // into it - frame generation reads them when that image is presented, after our next frame may have run.
  struct FgSlot {
    void* resource = nullptr;  // the presenter's guest-output ID3D12Resource
    gpu::TextureHandle depth = 0, motion = 0;
    uint32_t width = 0, height = 0;
    uint64_t used = 0;
  };
  std::array<FgSlot, 4> fg_slots_{};
  uint64_t fg_slot_clock_ = 0;
  bool fg_wanted_ = false;  // patch 0147: --gow2_frame_generation=fsr and the build has it
  FgInputs fg_{};
  uint64_t fg_made_ = 0, fg_resets_ = 0;  // log window
  gpu::PipelineHandle up_depth_pipe_ = 0, up_motion_pipe_ = 0;
  gpu::TextureHandle up_depth_ = 0, up_motion_ = 0, up_output_ = 0;
  uint32_t up_w_ = 0, up_h_ = 0;
  // Patch 0141 (upscaling modes): scene scale (0 = native), output size, whether this frame's output is upscaled, the
  // phase flip pending after the scene resolve packet, frames whose scene ran below the output, frames that never flipped.
  uint32_t up_scale_in_ = 0, up_ow_ = 0, up_oh_ = 0;
  bool up_output_upscaled_ = false, up_flip_after_resolve_ = false, up_scene_seen_ = false;
  uint32_t up_fail_streak_ = 0;
  uint64_t up_scene_phase_frames_ = 0, up_no_flip_ = 0;
  gpu::Format up_out_format_ = gpu::Format::kR16G16B16A16Float;
  bool up_camera_ = false, up_prev_camera_ = false, up_scene_done_ = false, up_last_ok_ = false, up_frame_ok_ = false;
  rr::RenderTargetKey up_depth_key_{};
  float up_vp_[16] = {}, up_prev_vp_[16] = {};
  rr::upscale::Jitter up_jitter_{};      // applied to this frame's scene draws
  bool up_dejitter_depth_ = false;        // --gow2_upscale_dejitter_depth (Gears 3 job 058)
  float up_depth_a_ = 1.0f;               // depth at infinity of this frame's camera (DepthAtInfinity)
  uint64_t up_dejittered_ = 0;            // scene depth resolves written de-jittered (log window)
  uint64_t up_frames_ = 0, up_ok_ = 0, up_fail_ = 0, up_jittered_ = 0, up_camera_frames_ = 0;
  // Gears 3 job 056: which constants carry the object rows / the camera position; the camera position this frame and
  // last, and the sign that maps its change onto the move (learned from frames where the object vote also found one).
  uint32_t up_object_base_ = 5, up_camera_reg_ = 0;
  float up_cam_pos_[3] = {}, up_prev_cam_pos_[3] = {};
  bool up_cam_pos_ok_ = false, up_prev_cam_pos_ok_ = false;
  int up_cam_sign_ = 0, up_cam_sign_votes_ = 0;
  uint64_t up_move_from_pos_ = 0;
  // Camera movement (UE3 renders camera-relative: c0-c3 have no translation). Each scene draw whose vertex shader reads
  // c5-c8 (UE3's LocalToWorld rows, translation in c8 relative to the camera - job 253 probe) gives one sample keyed by
  // its vertex data, shader and c5-c7; this frame's samples against last frame's give the camera's move (static
  // objects all shift by minus it; a vote sets them apart from the moving ones).
  static constexpr size_t kMaxObjectSamples = 2048;
  bool up_camera_move_ = true;
  std::vector<rr::upscale::ObjectSample> up_objects_, up_prev_objects_;
  uint64_t up_move_found_ = 0, up_move_missing_ = 0, up_cuts_ = 0;
  double up_move_sum_ = 0.0;
  uint32_t up_last_votes_ = 0, up_last_matched_ = 0;
  std::chrono::steady_clock::time_point up_last_time_{};

  void InitUpscaler() {
    const rr::upscale::Kind kind = rr::upscale::ParseKind(REXCVAR_GET(gow2_upscaler));
    const std::string name = REXCVAR_GET(gow2_upscaler);
    fg_wanted_ = XenosFrameGenerationWanted();  // patch 0147
    const bool fg_inputs = REXCVAR_GET(gow2_fg_inputs) || fg_wanted_;  // patch 0145
    if (kind == rr::upscale::Kind::kNone) {
      if (!name.empty() && name != "off") REXLOG_WARN("gow2_upscaler: unknown '{}' (off | dlss | fsr | xess); off", name);
      if (!fg_inputs) return;
    } else {
      upscaler_ = rr::upscale::CreateUpscaler(kind);
      if (!upscaler_) {
        REXLOG_WARN("gow2_upscaler: {} is not built into this exe (its SDK was not in third_party at build time); off",
                    rr::upscale::KindName(kind));
        if (!fg_inputs) return;
      }
    }
    const gpu::ShaderHandle d = cache_->GetBuiltin("upscale_depth_cs", rr::builtin::kDepthToFloatCs, gpu::ShaderStage::kCompute);
    const gpu::ShaderHandle m = cache_->GetBuiltin("upscale_motion_cs", rr::builtin::kMotionVectorsCs, gpu::ShaderStage::kCompute);
    up_depth_pipe_ = d ? backend_->CreateComputePipeline(d) : 0;
    up_motion_pipe_ = m ? backend_->CreateComputePipeline(m) : 0;
    if (!up_depth_pipe_ || !up_motion_pipe_) {
      REXLOG_WARN("gow2_upscaler: helper shaders failed to compile; off");
      upscaler_.reset();
      return;
    }
    up_active_ = true;  // patch 0145
    up_inputs_only_ = !upscaler_;
    up_camera_move_ = REXCVAR_GET(gow2_upscale_camera_move);
    up_dejitter_depth_ = REXCVAR_GET(gow2_upscale_dejitter_depth);
    up_jitter_late_ = REXCVAR_GET(gow2_upscale_jitter_late);
    {
      const int32_t ob = REXCVAR_GET(gow2_upscale_object_constants), cr = REXCVAR_GET(gow2_upscale_camera_constant);
      up_object_base_ = ob > 0 && ob <= 252 ? uint32_t(ob) : 5u;
      up_camera_reg_ = cr > 0 && cr <= 255 ? uint32_t(cr) : 0u;
      up_scene_formats_ = uint32_t(std::clamp(REXCVAR_GET(gow2_upscale_scene_formats), 1, 3));
      up_jitter_scope_ = uint32_t(std::clamp(REXCVAR_GET(gow2_upscale_jitter_scope), 0, 3));
      REXLOG_INFO("gow2_upscaler: object rows c{}-c{}, camera position {}, scene formats {}", up_object_base_,
                  up_object_base_ + 3, up_camera_reg_ ? "c" + std::to_string(up_camera_reg_) : std::string("not used"),
                  up_scene_formats_ == 1 ? "16-bit float" : up_scene_formats_ == 2 ? "10:10:10:2" : "both");
    }
    up_objects_.reserve(kMaxObjectSamples);
    up_prev_objects_.reserve(kMaxObjectSamples);
    if (upscaler_) {  // patch 0141: the scene scale for an upscaling mode (integer pairs only; 0145: not inputs only)
      const std::string mode = REXCVAR_GET(gow2_upscale_mode);
      const uint32_t out = res_scale_;
      uint32_t in = out;
      if (mode == "performance" && (out == 2 || out == 4)) in = out / 2;
      else if (mode == "quality" && out == 3) in = 2;
      else if (mode == "ultra" && out == 3) in = 1;  // patch 0143: DLSS maps 3:1 to Ultra Performance now
      else if (mode != "native" && !mode.empty())
        REXLOG_WARN("gow2_upscaler: mode '{}' has no integer scene scale at output {}x (performance: 2x or 4x; quality / "
                    "ultra: 3x); native", mode, out);
      up_scale_in_ = in < out ? in : 0;
      if (up_scale_in_)
        REXLOG_INFO("gow2_upscaler: mode {}: scene {}x ({}x{}) -> output {}x ({}x{}) (patch 0141)", mode, up_scale_in_,
                    1280 * up_scale_in_, 720 * up_scale_in_, out, 1280 * out, 720 * out);
    }
    if (upscaler_)
      REXLOG_INFO("gow2_upscaler: {} selected", rr::upscale::KindName(kind));
    else
      REXLOG_INFO("gow2_fg_inputs: patch 0145: scene depth and motion vectors every frame, no upscaler (no jitter)");
  }

  // Called for each draw before its viewport is set (v in host pixels). c: the draw's VS float constants c0-c3.
  void UpscaleJitter(const uint32_t* r, const rr::GuestShader& vsg, const rr::PipelineState& ps,
                     const rr::RenderTarget* depth, bool depth_writes, rr::ViewportState& v) {
    if (!depth || ps.viewport.xy_divided_by_w || ps.viewport.clip_disabled) return;
    // --gow2_upscale_jitter_scope (Gears 2 job 279 / Gears 3 job 059): which perspective draws into the frame's scene
    // depth get the jitter besides the camera draws - 1: also those whose vertex shader does not read c0-c3, 2: also
    // those with other c0-c3. A depth prepass drawn jittered and a base pass drawn unjittered fail UE3's depth-equal
    // test in patches (black / missing surfaces).
    const bool reads_camera = vsg.Reads(0) && vsg.Reads(1) && vsg.Reads(2) && vsg.Reads(3);
    if (!reads_camera) {
      if (up_jitter_scope_ >= 1 && up_camera_ && up_last_ok_ && IsSceneDepth(*depth)) {
        v.x += up_jitter_.x;
        v.y += up_jitter_.y;
        ++up_jittered_;
        ++up_jittered_scope_;
      }
      return;
    }
    float c[16];
    std::memcpy(c, &r[kRegFloatConstVs], sizeof(c));
    if (up_camera_ && up_jitter_scope_ >= 2 && IsSceneDepth(*depth) && std::memcmp(c, up_vp_, sizeof(c)) != 0) {
      if (up_last_ok_) {
        v.x += up_jitter_.x;
        v.y += up_jitter_.y;
        ++up_jittered_;
        ++up_jittered_scope_;
      }
      return;
    }
    if (!up_camera_) {
      if (!depth_writes || depth->guest_width < kSceneMinWidth || depth->guest_height < kSceneMinHeight) return;
      up_camera_ = true;
      ++up_camera_frames_;  // Gears 3 job 054
      if (up_camera_reg_ && vsg.Reads(up_camera_reg_)) {  // Gears 3 job 056: the camera's world position
        std::memcpy(up_cam_pos_, &r[kRegFloatConstVs + up_camera_reg_ * 4], sizeof(float) * 3);
        up_cam_pos_ok_ = std::isfinite(up_cam_pos_[0]) && std::isfinite(up_cam_pos_[1]) && std::isfinite(up_cam_pos_[2]);
      }
      up_depth_key_ = depth->key;
      up_depth_scale_ = depth->scale;
      std::memcpy(up_vp_, c, sizeof(c));
      up_depth_a_ = rr::upscale::DepthAtInfinity(rr::upscale::FromConstantsTransposed(up_vp_, 0));
    } else if (!IsSceneDepth(*depth) || std::memcmp(c, up_vp_, sizeof(c)) != 0) {
      return;
    }
    const uint32_t ob = up_object_base_;  // Gears 2: c5-c8, Gears 3: c7-c10 (its job 055 probe)
    if (up_camera_move_ && up_objects_.size() < kMaxObjectSamples && vsg.Reads(ob) && vsg.Reads(ob + 1) &&
        vsg.Reads(ob + 2) && vsg.Reads(ob + 3)) {
      float o[16];  // the object-to-world rows
      std::memcpy(o, &r[kRegFloatConstVs + ob * 4], sizeof(o));
      if (o[15] == 1.0f && o[3] == 0.0f && o[7] == 0.0f && o[11] == 0.0f) {  // an affine object-to-world matrix
        uint32_t vb = 0;
        for (uint32_t i = 0; i < 96 && !vb; ++i)
          if ((vsg.vfetch_constants[i >> 5] >> (i & 31)) & 1) vb = r[kRegFetchConst + 2 * i] & 0x1FFFFFFCu;
        struct { uint32_t vb; uint32_t pad; uint64_t vs; float rot[12]; } k{vb, 0, vsg.ucode_hash, {}};
        std::memcpy(k.rot, o, sizeof(k.rot));
        rr::upscale::ObjectSample& smp = up_objects_.emplace_back();
        smp.key = XXH3_64bits(&k, sizeof(k));
        smp.t[0] = o[12], smp.t[1] = o[13], smp.t[2] = o[14];
      }
    }
    if (!up_last_ok_) return;  // no jitter until the upscaler has run on a frame
    v.x += up_jitter_.x;
    v.y += up_jitter_.y;
    ++up_jittered_;
    if (!(depth->key == up_depth_key_)) ++up_jittered_scope_;  // scope 3: a camera draw into the aliasing depth target
  }

  // The scene colour: 16-bit float (Gears 2, linear HDR) and/or 10:10:10:2 UNORM (Gears 3, low dynamic range - from the
  // Gears 3 chat, 21:15), as --gow2_upscale_scene_formats says.
  uint32_t up_scene_formats_ = 1;
  uint32_t up_jitter_scope_ = 0;         // --gow2_upscale_jitter_scope
  uint64_t up_jittered_scope_ = 0;       // draws jittered only because of the scope (log window)
  bool up_jitter_late_ = false;          // --gow2_upscale_jitter_late (Gears 3 job 120)
  uint64_t up_late_draws_ = 0;           // camera draws into the scene depth after the upscale (log window)
  // Gears 3 job 120: a camera draw (same c0-c3 as the frame's first scene draw) into the scene depth after the scene
  // colour resolve that ran the upscaler - drawn unjittered, it can fail UE3's depth-equal test against the jittered
  // depth. Counted always; jittered with --gow2_upscale_jitter_late.
  void UpscaleLateDraw(const uint32_t* r, const rr::PipelineState& ps, const rr::RenderTarget* depth,
                       rr::ViewportState& v) {
    if (!up_camera_ || !depth || !(depth->key == up_depth_key_) || ps.viewport.xy_divided_by_w ||
        ps.viewport.clip_disabled)
      return;
    if (std::memcmp(&r[kRegFloatConstVs], up_vp_, sizeof(up_vp_)) != 0) return;
    ++up_late_draws_;
    if (up_jitter_late_ && up_last_ok_) {
      const float k = up_depth_scale_ ? float(depth->scale) / float(up_depth_scale_) : 1.0f;  // 0141: output twin
      v.x += up_jitter_.x * k;
      v.y += up_jitter_.y * k;
    }
  }
  uint32_t up_depth_scale_ = 1;          // the scene depth target's resolution scale (scope 3)
  // The frame's scene depth: the first camera draw's depth target, or with --gow2_upscale_jitter_scope=3 any depth target
  // at the same EDRAM base, scale and MSAA (another pitch / depth format aliasing the same EDRAM: Gears 3 job 060).
  bool IsSceneDepth(const rr::RenderTarget& t) const {
    if (t.key == up_depth_key_) return true;
    return up_jitter_scope_ >= 3 && t.key.base_tiles == up_depth_key_.base_tiles && t.key.msaa == up_depth_key_.msaa &&
           t.scale == up_depth_scale_;
  }
  bool IsSceneColourResolve(const ::xenos::resolve::Info& info, const rr::RenderTarget& rt) const {
    const bool f16 = (up_scene_formats_ & 1u) && rt.format == gpu::Format::kR16G16B16A16Float;
    const bool u1010102 = (up_scene_formats_ & 2u) && rt.format == gpu::Format::kR10G10B10A2Unorm;
    return (f16 || u1010102) &&
           info.x0 == 0 && info.y0 == 0 &&
           info.width_div_8 * 8 >= kSceneMinWidth && info.height_div_8 * 8 >= kSceneMinHeight;
  }

  uint32_t up_diag_lines_ = 0;  // Gears 3 job 054
  // Runs the upscaler for the scene colour target; returns the texture the resolve should read (0: keep the target).
  gpu::TextureHandle RunUpscaler(const rr::RenderTarget& rt, uint32_t w, uint32_t h) {
    rr::RenderTarget* depth = rts_->Find(up_depth_key_);
    if (!depth || !depth->texture || depth->scale != rt.scale) {
      if (up_diag_lines_ < 12) {  // Gears 3 job 054
        ++up_diag_lines_;
        REXLOG_INFO("gow2_upscaler: frame {}: scene resolve found, but its depth target {} (scale {} vs colour {})",
                    frame_ + 1, !depth ? "is gone" : !depth->texture ? "has no texture" : "differs in scale",
                    depth ? depth->scale : 0, rt.scale);
      }
      return 0;
    }
    if (up_scale_in_ && rt.scale != up_scale_in_) {  // 0141 review: a frame that started at the output scale -
      up_scene_seen_ = true;                         // no evaluate (no reconfigure); the next frame starts at the scene scale
      return 0;
    }
    const uint32_t rw = w * rt.scale, rh = h * rt.scale;
    // Patch 0141: the output is at the output scale (larger than the render size in an upscaling mode).
    const uint32_t ow = up_scale_in_ && rt.scale == up_scale_in_ ? w * res_scale_ : rw;
    const uint32_t oh = up_scale_in_ && rt.scale == up_scale_in_ ? h * res_scale_ : rh;
    // The output has the scene colour's format (Gears 2 job 271: XeSS requires it; DLSS / FSR accept it). Gears 3's
    // scene is 10:10:10:2, so this output used to be 16-bit float while the input was not.
    if (up_w_ != rw || up_h_ != rh || up_ow_ != ow || up_oh_ != oh || up_out_format_ != rt.format) {
      for (gpu::TextureHandle* t : {&up_depth_, &up_motion_, &up_output_})
        if (*t) backend_->DestroyTexture(*t), *t = 0;
      gpu::TextureDesc td;
      td.width = rw;
      td.height = rh;
      td.usage = gpu::kUsageSampled | gpu::kUsageStorage;
      td.format = gpu::Format::kR32Float;
      up_depth_ = backend_->CreateTexture(td);
      td.format = gpu::Format::kR16G16Float;
      up_motion_ = backend_->CreateTexture(td);
      td.format = rt.format;
      td.width = ow;  // patch 0141
      td.height = oh;
      if (upscaler_) up_output_ = backend_->CreateTexture(td);  // patch 0145: none for inputs only
      up_w_ = rw, up_h_ = rh, up_ow_ = ow, up_oh_ = oh, up_out_format_ = rt.format;
      if (!up_depth_ || !up_motion_ || (upscaler_ && !up_output_)) return 0;
    }
    // UE3 computes clip = x*c0 + y*c1 + z*c2 + c3 (job 252): the constants hold the transpose of our row-major matrix.
    const rr::upscale::Mat4 cur = rr::upscale::FromConstantsTransposed(up_vp_, 0);
    const rr::upscale::Mat4 prev = rr::upscale::FromConstantsTransposed(up_prev_camera_ ? up_prev_vp_ : up_vp_, 0);
    float fov = 1.0f, znear = 10.0f;
    bool infinite = false;
    const bool persp = rr::upscale::PerspectiveParams(cur, fov, znear, infinite);
    const bool hdr = rt.format == gpu::Format::kR16G16B16A16Float;
    if (upscaler_ && !upscaler_->Configure(*backend_, rw, rh, ow, oh, hdr, persp && infinite)) {
      REXLOG_WARN("gow2_upscaler: {}; off", upscaler_->Status());
      upscaler_.reset();
      up_last_ok_ = false;
      if (up_output_) backend_->DestroyTexture(up_output_), up_output_ = 0;  // 0147 review: only the upscaler used it
      if (up_scale_in_) {  // patch 0145: an upscaling mode must not leave the scene below the output with no upscaler
        up_scale_in_ = 0;
        rts_->SetPhaseScale(res_scale_);
      }
      up_inputs_only_ = REXCVAR_GET(gow2_fg_inputs) || fg_wanted_;  // patch 0145: frame generation keeps its inputs
      up_active_ = up_inputs_only_;
      if (!up_inputs_only_) return 0;
    }
    // Depth in guest convention (a float24 target is stored halved, see remap24 in Draw).
    gpu::ComputeBindings b;
    b.constants[0] = std::bit_cast<uint32_t>((float24_remap_ && (depth->key.storage_class & 1)) ? 2.0f : 1.0f);
    b.constant_count = 4;
    b.srv[0] = depth->texture;
    b.srv_plane[0] = 0;
    b.uav_texture = up_depth_;
    backend_->Dispatch(up_depth_pipe_, b, (rw + 7) / 8, (rh + 7) / 8, 1);
    rr::upscale::Mat4 rep;
    bool have_rep = false, cut = false;
    float move[3] = {};
    // Gears 3 job 056: the camera position's change, once its sign is known from frames where the object vote agrees.
    float pos_move[3] = {};
    const bool have_pos = up_camera_reg_ && up_cam_pos_ok_ && up_prev_cam_pos_ok_;
    if (have_pos)
      for (int i = 0; i < 3; ++i) pos_move[i] = up_cam_pos_[i] - up_prev_cam_pos_[i];
    if (up_camera_move_ && up_prev_camera_) {
      bool voted = rr::upscale::CameraMoveFromObjects(up_prev_objects_, up_objects_, move, &up_last_votes_, &up_last_matched_);
      if (voted && have_pos) {  // learn / check the sign
        double dp = 0, dm = 0, len = 0;
        for (int i = 0; i < 3; ++i) {
          dp += (move[i] - pos_move[i]) * (move[i] - pos_move[i]);
          dm += (move[i] + pos_move[i]) * (move[i] + pos_move[i]);
          len += double(move[i]) * move[i];
        }
        if (len > 1.0 && std::min(dp, dm) < 0.01 * len) {
          const int sign = dp <= dm ? 1 : -1;
          if (sign == up_cam_sign_) up_cam_sign_votes_ = std::min(up_cam_sign_votes_ + 1, 100);
          else if (--up_cam_sign_votes_ <= 0) up_cam_sign_ = sign, up_cam_sign_votes_ = 1;
          if (up_diag_lines_ < 24) {
            ++up_diag_lines_;
            REXLOG_INFO("gow2_upscaler: frame {}: object vote move ({:.2f} {:.2f} {:.2f}), camera position change ({:.2f} "
                        "{:.2f} {:.2f}): sign {}", frame_ + 1, move[0], move[1], move[2], pos_move[0], pos_move[1],
                        pos_move[2], sign);
          }
        }
      }
      if (!voted && have_pos && up_cam_sign_ && up_cam_sign_votes_ >= 3) {
        for (int i = 0; i < 3; ++i) move[i] = float(up_cam_sign_) * pos_move[i];
        voted = true;
        ++up_move_from_pos_;
      }
      if (voted) {
        const double len = std::sqrt(double(move[0]) * move[0] + double(move[1]) * move[1] + double(move[2]) * move[2]);
        if (len > 2000.0) {  // 20 m in one frame: a camera cut, not movement
          cut = true;
          ++up_cuts_;
        } else {
          ++up_move_found_;
          up_move_sum_ += len;
          have_rep = rr::upscale::ReprojectionMatrix(cur, prev, move, 1.0f, rep);
        }
      } else {
        ++up_move_missing_;
      }
    }
    if (!have_rep && !cut) have_rep = rr::upscale::ReprojectionMatrix(cur, prev, 1.0f, rep);
    gpu::ComputeBindings mb;
    for (int i = 0; i < 16; ++i) mb.constants[i] = std::bit_cast<uint32_t>(have_rep ? rep.m[i] : (i % 5 ? 0.0f : 1.0f));
    mb.constant_count = 16;
    mb.srv[0] = up_depth_;
    mb.uav_texture = up_motion_;
    backend_->Dispatch(up_motion_pipe_, mb, (rw + 7) / 8, (rh + 7) / 8, 1);
    const auto now = std::chrono::steady_clock::now();
    rr::upscale::FrameInputs in;
    in.color = rt.texture;
    in.depth = up_depth_;
    in.motion = up_motion_;
    in.output = up_output_;
    in.render_width = rw;
    in.render_height = rh;
    in.output_width = ow;  // patch 0141
    in.output_height = oh;
    const float sign = REXCVAR_GET(gow2_upscale_jitter_sign) < 0 ? -1.0f : 1.0f;
    in.jitter = up_last_ok_ ? rr::upscale::Jitter{up_jitter_.x * sign, up_jitter_.y * sign} : rr::upscale::Jitter{};
    in.reset = !up_prev_camera_ || !up_last_ok_ || !have_rep || cut;
    in.hdr = hdr;
    in.frame_ms = up_last_time_.time_since_epoch().count()
                      ? float(std::chrono::duration<double, std::milli>(now - up_last_time_).count())
                      : 16.7f;
    in.sharpness = float(std::clamp(REXCVAR_GET(gow2_upscale_sharpness), 0.0, 1.0));
    in.vertical_fov = fov;
    in.camera_near = znear;
    in.camera_far = infinite ? 1.0e7f : 100000.0f;
    up_last_time_ = now;
    // Patch 0145: the frame-generation inputs (made whether or not an upscaler runs).
    fg_.depth = up_depth_;
    fg_.motion = up_motion_;
    fg_.width = rw;
    fg_.height = rh;
    fg_.jitter_x = in.jitter.x;
    fg_.jitter_y = in.jitter.y;
    fg_.vertical_fov = fov;
    fg_.camera_near = znear;
    fg_.camera_far = in.camera_far;
    fg_.frame_ms = in.frame_ms;
    fg_.reset = in.reset;
    fg_.hdr = hdr;
    fg_.infinite = persp && infinite;
    fg_.frame = frame_ + 1;
    {  // patch 0147
      auto unit = [&](int row, float* out) {
        const double x = cur(row, 0), y = cur(row, 1), z = cur(row, 2), l = std::sqrt(x * x + y * y + z * z);
        out[0] = l > 1e-9 ? float(x / l) : 0.0f, out[1] = l > 1e-9 ? float(y / l) : 0.0f, out[2] = l > 1e-9 ? float(z / l) : 0.0f;
      };
      unit(0, fg_.right);
      unit(1, fg_.up);
      unit(3, fg_.forward);
      if (have_rep && !cut)
        for (int i = 0; i < 3; ++i) fg_position_[i] += move[i];
      std::memcpy(fg_.position, fg_position_, sizeof(fg_position_));
    }
    ++fg_made_;
    fg_resets_ += in.reset ? 1 : 0;
    if (!upscaler_) {      // inputs only: the resolve reads the scene as it is
      up_frame_ok_ = true; // the next frame's motion is not a reset
      return 0;
    }
    if (REXCVAR_GET(gow2_upscale_passthrough)) {  // diagnostic: the jittered scene itself, not the reconstruction
      ++up_ok_;
      up_frame_ok_ = true;  // keeps the jitter going
      return 0;             // 0 = the resolve reads the scene target as it is
    }
    if (!upscaler_->Evaluate(*backend_, in)) {
      ++up_fail_;
      if (up_scale_in_ && ++up_fail_streak_ >= 30) {  // patch 0141 review: keeps failing below the output -> native
        REXLOG_WARN("gow2_upscaler: 30 failures in a row with the scene at {}x; upscaling mode off (native)", up_scale_in_);
        up_scale_in_ = 0;
        rts_->SetPhaseScale(res_scale_);
      }
      return 0;
    }
    up_fail_streak_ = 0;
    ++up_ok_;
    up_frame_ok_ = true;
    up_output_upscaled_ = ow != rw;  // patch 0141: the resolve reads it at the output scale
    return up_output_;
  }

  // Patch 0147: copies this frame's depth / motion into the slot of the presenter image just written and gives frame
  // generation their native resources; null when this frame made none (no 3D scene: frame generation pauses).
  void FgHandOver(void* guest_output) {
    if (!XenosFrameGenerationWanted()) return;  // AMD's files or swap chain missing: no copies for nothing
    const bool fresh = fg_.frame == frame_ && fg_.depth && fg_.motion && fg_.width && fg_.height;
    if (!fresh) {
      XenosFrameGenerationSetInputs(guest_output, nullptr);
      return;
    }
    FgSlot* slot = nullptr;
    for (FgSlot& s : fg_slots_)
      if (s.resource == guest_output) slot = &s;
    if (!slot) {  // the least recently used slot
      slot = &fg_slots_[0];
      for (FgSlot& s : fg_slots_)
        if (s.used < slot->used) slot = &s;
      slot->resource = guest_output;
    }
    slot->used = ++fg_slot_clock_;
    if (slot->width != fg_.width || slot->height != fg_.height || !slot->depth || !slot->motion) {
      for (gpu::TextureHandle* t : {&slot->depth, &slot->motion})
        if (*t) backend_->DestroyTexture(*t), *t = 0;
      gpu::TextureDesc td;
      td.width = fg_.width;
      td.height = fg_.height;
      td.usage = gpu::kUsageSampled;
      td.format = gpu::Format::kR32Float;
      slot->depth = backend_->CreateTexture(td);
      td.format = gpu::Format::kR16G16Float;
      slot->motion = backend_->CreateTexture(td);
      slot->width = fg_.width, slot->height = fg_.height;
      if (!slot->depth || !slot->motion) {
        XenosFrameGenerationSetInputs(guest_output, nullptr);
        return;
      }
    }
    void* dev = nullptr;
    void* list = nullptr;
    if (!backend_->BeginNative(&dev, &list)) {
      XenosFrameGenerationSetInputs(guest_output, nullptr);
      return;
    }
    auto* cl = static_cast<ID3D12GraphicsCommandList*>(list);
    void* out_depth = nullptr;
    void* out_motion = nullptr;
    for (int i = 0; i < 2; ++i) {
      const gpu::TextureHandle src = i ? fg_.motion : fg_.depth, dst = i ? slot->motion : slot->depth;
      auto* s = static_cast<ID3D12Resource*>(backend_->NativeTexture(src, D3D12_RESOURCE_STATE_COPY_SOURCE));
      auto* d = static_cast<ID3D12Resource*>(backend_->NativeTexture(dst, D3D12_RESOURCE_STATE_COPY_DEST));
      if (s && d) cl->CopyResource(d, s);
      void* ready = s && d ? backend_->NativeTexture(dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) : nullptr;
      (i ? out_motion : out_depth) = ready;
    }
    backend_->EndNative();
    if (!out_depth || !out_motion) {
      XenosFrameGenerationSetInputs(guest_output, nullptr);
      return;
    }
    XenosFgFrame f;
    f.depth = out_depth;
    f.motion = out_motion;
    f.width = fg_.width;
    f.height = fg_.height;
    f.jitter_x = fg_.jitter_x;
    f.jitter_y = fg_.jitter_y;
    f.vertical_fov = fg_.vertical_fov;
    f.camera_near = fg_.camera_near;
    f.camera_far = fg_.camera_far;
    f.frame_ms = fg_.frame_ms;
    f.reset = fg_.reset;
    f.infinite = fg_.infinite;
    f.frame = frame_;
    std::memcpy(f.camera_position, fg_.position, sizeof(f.camera_position));
    std::memcpy(f.camera_up, fg_.up, sizeof(f.camera_up));
    std::memcpy(f.camera_right, fg_.right, sizeof(f.camera_right));
    std::memcpy(f.camera_forward, fg_.forward, sizeof(f.camera_forward));
    XenosFrameGenerationSetInputs(guest_output, &f);
  }

  // Frame boundary (OnSwap): this frame's camera becomes the previous one; next frame's jitter.
  void UpscaleEndFrame() {
    if (!up_active_) return;  // patch 0145
    up_last_ok_ = up_frame_ok_;
    up_frame_ok_ = false;
    up_prev_camera_ = up_camera_;
    up_prev_cam_pos_ok_ = up_camera_ && up_cam_pos_ok_;  // Gears 3 job 056
    if (up_prev_cam_pos_ok_) std::memcpy(up_prev_cam_pos_, up_cam_pos_, sizeof(up_cam_pos_));
    up_cam_pos_ok_ = false;
    if (up_camera_) std::memcpy(up_prev_vp_, up_vp_, sizeof(up_vp_));
    if (up_camera_) up_prev_objects_.swap(up_objects_);
    up_objects_.clear();
    up_camera_ = false;
    up_scene_done_ = false;
    ++up_frames_;
    up_jitter_ = rr::upscale::JitterOffset(  // patch 0141: more phases when upscaling (render < output)
        up_frames_, rr::upscale::JitterPhaseCount(up_scale_in_ ? up_scale_in_ : 1, up_scale_in_ ? res_scale_ : 1));
    if (up_scale_in_) {  // patch 0141: the next frame's scene at the scene scale only if this frame upscaled
      if (rts_->PhaseScale() != res_scale_) ++up_no_flip_;  // scene scale all frame: no scene resolve upscaled
      const bool scene_next = up_last_ok_ || up_scene_seen_;  // 0141 review: also after an output-scale scene frame
      up_scene_seen_ = false;
      rts_->SetPhaseScale(scene_next ? up_scale_in_ : res_scale_);
      up_scene_phase_frames_ += scene_next ? 1 : 0;
    }
    {
      const float js = float(std::clamp(REXCVAR_GET(gow2_upscale_jitter_scale), 0.0, 1.0));
      up_jitter_.x *= js;
      up_jitter_.y *= js;
    }
    if (up_inputs_only_) up_jitter_ = rr::upscale::Jitter{};  // patch 0145: no upscaler, no jitter
    if (fg_made_ + fg_resets_ && frame_ % uint64_t(log_every_) == 0 && (REXCVAR_GET(gow2_fg_inputs) || fg_wanted_)) {
      REXLOG_INFO("gow2_fg_inputs: frames {}-{}: depth + motion made on {} frames ({} resets), {}x{}, {}", 
                  frame_ - uint64_t(log_every_) + 1, frame_, fg_made_, fg_resets_, fg_.width, fg_.height,
                  up_inputs_only_ ? "no upscaler" : "with the upscaler");
      fg_made_ = fg_resets_ = 0;
    }
    if (frame_ % uint64_t(log_every_) == 0) {
      REXLOG_INFO("gow2_upscaler: frames {}-{}: {}; upscaled {} (failed {}), jittered draws {:.0f}/frame, camera found "
                  "on {} frames{}", frame_ - uint64_t(log_every_) + 1, frame_,
                  upscaler_ ? upscaler_->Status() : std::string("inputs only (patch 0145)"), up_ok_, up_fail_,
                  double(up_jittered_) / log_every_, up_camera_frames_,
                  (up_dejitter_depth_ ? std::format(", scene depth resolves de-jittered {} (depth at infinity {:.6g})",
                                                    up_dejittered_, up_depth_a_)
                                      : std::string()) +
                      (up_jitter_scope_ ? std::format(", jitter scope {}: {:.0f} more draws/frame", up_jitter_scope_,
                                                      double(up_jittered_scope_) / log_every_)
                                        : std::string()) +
                      std::format(", camera draws after the upscale {:.1f}/frame ({})", double(up_late_draws_) / log_every_,
                                  up_jitter_late_ ? "jittered" : "not jittered") +
                      (up_scale_in_ ? std::format(" | patch 0141 mode: scene at {}x on {} frames, output {}x {}x{}, "
                                                  "frames without a scene resolve {}, targets ~{} MB",
                                                  up_scale_in_, up_scene_phase_frames_, res_scale_, up_ow_, up_oh_,
                                                  up_no_flip_, rts_->HostBytes() >> 20)
                                    : std::string()));
      up_scene_phase_frames_ = 0;
      up_no_flip_ = 0;
      up_late_draws_ = 0;
      up_jittered_scope_ = 0;
      up_camera_frames_ = 0;
      up_dejittered_ = 0;
      if (up_camera_move_)
        REXLOG_INFO("gow2_upscaler: camera move found on {} frames (mean {:.2f} units/frame; {} from the camera "
                    "position, sign {} x{}), not found {}, cuts {}; last frame {} of {} matched objects agreed",
                    up_move_found_, up_move_found_ ? up_move_sum_ / double(up_move_found_) : 0.0, up_move_from_pos_,
                    up_cam_sign_, up_cam_sign_votes_, up_move_missing_, up_cuts_, up_last_votes_, up_last_matched_);
      up_move_from_pos_ = 0;
      up_jittered_ = 0;
      up_move_found_ = up_move_missing_ = up_cuts_ = 0;
      up_move_sum_ = 0.0;
    }
  }
  struct ThroughCheck {  // job 047 check (--gow2_renderer_resolve_through_check)
    gpu::TextureHandle copy = 0, result = 0;
    gpu::ReadbackHandle ours = 0;
    uint32_t texel_bytes = 0, w = 0, h = 0, code = 0, base = 0;
    std::string rect;
  };
  std::vector<ThroughCheck> through_checks_;
  uint64_t through_check_frame_ = 0;
  gpu::PipelineHandle through_compare_ = 0;
  uint32_t through_checks_started_ = 0;
  void PollThroughChecks() {
    for (size_t i = 0; i < through_checks_.size();) {
      ThroughCheck& c = through_checks_[i];
      if (c.ours && !backend_->IsReadbackReady(c.ours)) {
        ++i;
        continue;
      }
      uint32_t pitch = 0;
      const uint8_t* d = c.ours ? backend_->ReadbackData(c.ours, pitch) : nullptr;
      if (!d) {
        REXLOG_INFO("gow2_renderer: resolve-through check {:08X}: readback failed", c.base);
      } else {
        uint32_t v[4];
        std::memcpy(v, d, sizeof(v));
        // The clear pass sets [1] to FFFFFFFF; still there with 0 differences = the comparison ran and found none.
        if (v[0] == 0 && v[1] != 0xFFFFFFFFu)
          REXLOG_INFO("gow2_renderer: resolve-through check: base {:08X}: comparison did NOT run (result {:08X} {:08X})",
                      c.base, v[0], v[1]);
        else REXLOG_INFO("gow2_renderer: resolve-through check: base {:08X} {}x{} code {} rect {}: {} of {} texels differ{}",
                    c.base, c.w, c.h, c.code, c.rect, v[0], uint64_t(c.w) * c.h,
                    v[0] ? fmt::format(" (first at {},{}: written {:08X} untiled {:08X})", v[1] % std::max(c.w, 1u),
                                       v[1] / std::max(c.w, 1u), v[2], v[3])
                         : std::string());
      }
      if (c.ours) backend_->ReleaseReadback(c.ours);
      backend_->DestroyTexture(c.copy);
      backend_->DestroyTexture(c.result);
      through_checks_.erase(through_checks_.begin() + i);
    }
  }

  T2Stats t2_stats_;
  std::map<std::string, uint64_t> resolve_early_;  // patch 0030: resolves that ended early, by reason (per window)

  bool display_ours_ = false;
  bool display_wait_ = false;
  bool display_first_logged_ = false;
  bool display_mark_ = false;
  std::unordered_map<uint32_t, DisplaySource> display_sources_;
  std::vector<DisplayPending> display_pending_;
  DisplayStats display_stats_;


  // ---------------------------------------------------------------------------------------------------------------
  // Draws
  // ---------------------------------------------------------------------------------------------------------------
  // ---- CPU cost split ----
  // Part timers read the time-stamp counter (job 156: steady_clock = QueryPerformanceCounter cost ~5% of this thread at
  // several calls per draw), converted with a ratio measured once against steady_clock.
  static double MsPerTick() {
    static const double ratio = [] {
      const auto c0 = std::chrono::steady_clock::now();
      const uint64_t t0 = __rdtsc();
      while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(20)) {
      }
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
      const uint64_t ticks = __rdtsc() - t0;
      return ticks ? ms / double(ticks) : 0.0;
    }();
    return ratio;
  }
  // Frame log (frame_log.cpp): this frame's renderer side - busy time and its waits, the biggest CPU part (this frame's
  // part ticks, read before ReportHitch moves the mark and the summary clears them), new pipelines / shaders, uploads.
  void FrameLogRecord(double ms, double hold_ms, double pipe_wait_ms, uint32_t draws, uint32_t skipped) {
    const gpu::BackendStats& bs = backend_->Stats();
    const rr::ShaderCacheStats& cs = cache_->Stats();
    XenosRenderedFrame f;
    f.cpu_ms = ms;
    f.gpu_wait_ms = std::max(0.0, bs.fence_wait_ms - fl_prev_.fence_wait_ms);
    f.hold_ms = hold_ms;
    f.pipeline_wait_ms = pipe_wait_ms;
    f.gpu_ms = std::max(0.0, bs.gpu_ms - fl_prev_.gpu_ms);
    if (part_level_ > 0) {
      uint32_t top = kPartCount;
      double top_ms = 0.0;
      for (uint32_t i = 0; i < kPartCount; ++i) {
        const double pm = double(part_ticks_[i] - hitch_ticks_[i]) * MsPerTick();
        if (pm > top_ms) {
          top_ms = pm;
          top = i;
        }
      }
      if (top < kPartCount) {
        f.top_part = kPartNames[top];
        f.top_part_ms = top_ms;
      }
    }
    f.draws = draws;
    f.skipped_draws = skipped;
    f.pipelines_new = uint32_t(bs.pipelines - fl_prev_.pipelines);
    f.shaders_new = uint32_t(cs.translated - fl_prev_.translated);
    f.upload_mb = double(bs.upload_bytes - fl_prev_.upload_bytes) / 1048576.0;
    auto cat = [&](std::initializer_list<int> c) {
      double v = 0.0;
      for (int i : c) v += bs.gpu_cat_ms[i] - fl_prev_.cat[i];
      return std::max(0.0, v);
    };
    f.gpu_draws_ms = cat({0});
    f.gpu_transfers_ms = cat({1, 5, 6, 7});
    f.gpu_resolves_ms = cat({2});
    f.gpu_textures_ms = cat({3});
    f.gpu_idle_ms = std::max(0.0, bs.gpu_idle_ms - fl_prev_.gpu_idle_ms);
    f.idle_submits = idle_submits_last_frame_;
    fl_prev_ = FrameLogPrev{bs.fence_wait_ms, bs.gpu_ms, bs.pipelines, cs.translated, bs.upload_bytes, {}, bs.gpu_idle_ms};
    std::copy(std::begin(bs.gpu_cat_ms), std::end(bs.gpu_cat_ms), std::begin(fl_prev_.cat));
    XenosFrameRendered(f);
  }
  struct FrameLogPrev {
    double fence_wait_ms = 0.0, gpu_ms = 0.0;
    uint64_t pipelines = 0, translated = 0, upload_bytes = 0;
    double cat[8] = {};
    double gpu_idle_ms = 0.0;
  };
  FrameLogPrev fl_prev_;
  // Patch 0010: one line for a frame over --gow2_hitch_report_ms - its biggest CPU parts and what happened in it.
  void ReportHitch(double ms) {
    const double limit = REXCVAR_GET(gow2_hitch_report_ms);
    const gpu::BackendStats& bs = backend_->Stats();
    const rr::ShaderCacheStats& cs = cache_->Stats();
    HitchCounters now{bs.pipelines_loaded, bs.pipelines_loaded_async, bs.pipelines_stored, bs.pipeline_library_saves,
                      bs.pipelines_async, bs.pipelines_pending_draws, cs.compiled, cs.disk_hits, cs.translated,
                      stats_.copies, bs.texture_upload_bytes, bs.upload_bytes, bs.fence_wait_ms, bs.pipeline_create_ms,
                      cs.translate_ms, cs.disk_ms, cs.pack_hits};
    if (limit > 0.0 && ms > limit && hitches_logged_ < 200) {
      ++hitches_logged_;
      std::pair<double, uint32_t> parts[kPartCount];
      for (uint32_t i = 0; i < kPartCount; ++i)
        parts[i] = {double(part_ticks_[i] - hitch_ticks_[i]) * MsPerTick(), i};
      std::sort(std::begin(parts), std::end(parts), [](const auto& a, const auto& b) { return a.first > b.first; });
      std::string top;
      for (uint32_t i = 0; i < 4; ++i) top += std::format("{}{} {:.1f}", i ? ", " : "", kPartNames[parts[i].second], parts[i].first);
      // Patch 0033: where the long stretches were (shadow_renderer.cpp lines; "end of" = the scope opened there ends).
      for (const SlowStretch& st : slow_stretches_)
        top += std::format(" | {} {:.1f} ms from line {} to {}{}", kPartNames[st.part], st.ms, st.from & 0x7FFFFFFFu,
                           (st.to & 0x80000000u) ? "end of " : "", st.to & 0x7FFFFFFFu);
      top += StretchAggText();  // patch 0036
      const HitchCounters& p = hitch_prev_;
      REXLOG_INFO("gow2_renderer: HITCH frame {}: {:.1f} ms in the renderer | parts (ms): {} | pipelines: loaded {} "
                  "(async {}), stored {}, library saves {}, new async {}, pending draws {}, sync create {:.1f} ms | "
                  "shaders: compiled {}, from disk {} ({:.1f} ms, pack {}), translated {} ({:.1f} ms) | resolves {} | uploads {:.1f} MB "
                  "(textures {:.1f}) | GPU waits {:.1f} ms",
                  frame_, ms, top, now.loaded - p.loaded, now.loaded_async - p.loaded_async, now.stored - p.stored,
                  now.saves - p.saves, now.async - p.async, now.pending - p.pending, now.create_ms - p.create_ms,
                  now.compiled - p.compiled, now.disk_hits - p.disk_hits, now.disk_ms - p.disk_ms, now.pack_hits - p.pack_hits, now.translated - p.translated,
                  now.translate_ms - p.translate_ms, now.copies - p.copies,
                  double(now.upload - p.upload) / 1048576.0, double(now.tex_upload - p.tex_upload) / 1048576.0,
                  now.wait_ms - p.wait_ms);
    }
    hitch_prev_ = now;
    slow_stretches_.clear();  // patch 0033: per frame
    ClearStretchAgg();        // patch 0036
  }
  struct HitchCounters {
    uint64_t loaded = 0, loaded_async = 0, stored = 0, saves = 0, async = 0, pending = 0, compiled = 0, disk_hits = 0,
             translated = 0, copies = 0, tex_upload = 0, upload = 0;
    double wait_ms = 0, create_ms = 0, translate_ms = 0, disk_ms = 0;
    uint64_t pack_hits = 0;
  };
  HitchCounters hitch_prev_;
  uint64_t hitch_ticks_[kPartCount] = {};
  uint32_t hitches_logged_ = 0;

  void BeginParts() {
    if (part_level_ == 0) {  // patch 0153: no part timers at all
      parts_on_ = false;
      return;
    }
    part_mark_ = __rdtsc();
    part_line_ = 0;
    part_cur_ = kPartOther;
    parts_on_ = true;
  }
  // Job 177: the part also names the GPU category of the work it records (record = draws, transfers, resolves, frame);
  // other parts keep the current one. The backend writes a timestamp only when work of a new category starts.
  void GpuCat(uint8_t c) {  // job 179: a finer GPU category inside the current part (transfer kinds)
    if (c != gpu_cat_ && backend_) {
      gpu_cat_ = c;
      backend_->SetGpuCategory(c);
      if (report_on_) RepTag(false, "work: category " + std::to_string(c));  // patch 0101
    }
  }
  void SetPart(Part p) {
    part_cur_ = p;
    static constexpr uint8_t kCat[kPartCount] = {0xFF, 0xFF, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0, 2, 0xFF, 0xFF, 4, 0xFF};
    uint8_t c = kCat[p];
    if (c == 0 && gpu_by_target_ && draw_cat_) c = draw_cat_;  // job 234: draws carry their target's slot
    if (c != 0xFF && c != gpu_cat_ && backend_) {
      gpu_cat_ = c;
      backend_->SetGpuCategory(c);
      if (report_on_ && (c < 0x20 || c >= 0x40)) RepTag(false, "work: category " + std::to_string(c));  // patch 0101
    }
  }
  // ---- Patch 0101: the draw report ----
  void RepTag(bool draw, std::string text) {
    RepRecord r;
    r.tag = ++report_seq_;
    r.frame = frame_;
    r.draw = draw;
    r.text = std::move(text);
    rep_records_.push_back(std::move(r));
    backend_->SetGpuTag(report_seq_);
  }
  void RepNoteTextures(const gpu::TextureView* views, const gpu::SamplerDesc* samplers, uint32_t n, uint32_t ns) {
    rep_view_count_ = views ? std::min<uint32_t>(n, 96) : 0;
    rep_sampler_count_ = samplers ? std::min<uint32_t>(ns, 32) : 0;
    for (uint32_t i = 0; i < rep_view_count_; ++i) rep_views_[i] = views[i];
    for (uint32_t i = 0; i < rep_sampler_count_; ++i) rep_samplers_[i] = samplers[i];
  }
  std::string RepDrawText(const gpu::GraphicsPipelineDesc& d, rr::RenderTarget* const* colour, rr::RenderTarget* depth,
                          uint64_t vs_hash, uint64_t ps_hash, uint32_t count, float vw, float vh, uint32_t sw,
                          uint32_t sh) {
    std::string t;
    char b[512];
    std::snprintf(b, sizeof b, "vs %016llX ps %016llX topo %u n %u vp %.0fx%.0f sc %ux%u msaa %u", (unsigned long long)vs_hash,
                  (unsigned long long)(d.ps ? ps_hash : 0), unsigned(d.topology), count, vw, vh, sw, sh, d.samples);
    t += b;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!colour[i]) continue;
      const gpu::BlendDesc& bl = d.color_blend[i];
      std::snprintf(b, sizeof b, " | rt%u id %u fmt %u mask %X blend %s%u/%u/%u %u/%u/%u [%s]", i, colour[i]->id,
                    unsigned(colour[i]->format), i < gpu::GraphicsPipelineDesc::kMaxColor ? d.color_write_masks[i] : 0,
                    bl.enable ? "on " : "off ", unsigned(bl.src), unsigned(bl.dst), unsigned(bl.op), unsigned(bl.src_alpha),
                    unsigned(bl.dst_alpha), unsigned(bl.op_alpha), backend_->DescribeTexture(colour[i]->texture).c_str());
      t += b;
    }
    if (depth) {
      std::snprintf(b, sizeof b, " | ds id %u fmt %u z %u/%u f%u st %u [%s]", depth->id, unsigned(depth->format),
                    unsigned(d.depth_enable), unsigned(d.depth_write), unsigned(d.depth_func), unsigned(d.stencil_enable),
                    backend_->DescribeTexture(depth->texture).c_str());
      t += b;
    }
    for (uint32_t i = 0; i < rep_view_count_; ++i) {
      const gpu::TextureView& v = rep_views_[i];
      if (!v.texture && !v.zero) continue;
      std::snprintf(b, sizeof b, " | t%u dim %u sw %u%u%u%u%s%s%s [%s]", i, unsigned(v.dimension), v.components[0],
                    v.components[1], v.components[2], v.components[3], v.signed_view ? " signed" : "",
                    v.bits_view ? " bits" : "", v.zero ? " zero" : "", backend_->DescribeTexture(v.texture).c_str());
      t += b;
      const uint32_t si = i & 31u;  // fetch slot = sampler
      if (si < rep_sampler_count_) {
        const gpu::SamplerDesc& sd = rep_samplers_[si];
        std::snprintf(b, sizeof b, " s mag%u min%u mip%u aniso %u addr %u%u%u lod %.2f %.2f-%.2f", sd.mag_linear,
                      sd.min_linear, sd.mip_linear, sd.max_anisotropy, unsigned(sd.address[0]), unsigned(sd.address[1]),
                      unsigned(sd.address[2]), sd.lod_bias, sd.min_lod, sd.max_lod);
        t += b;
      }
    }
    return t;
  }
  void ReportFrame() {
    const uint64_t end = report_first_ + report_count_;
    if (report_done_) return;
    if (report_samples_) backend_->SetGpuTagSamples(frame_ + 3 >= report_first_ && frame_ < end + 1);  // patch 0102
    if (frame_ == report_first_) {
      rep_records_.clear();
      rep_times_.clear();
      report_seq_ = 0;
      REXLOG_INFO("gow2_renderer: draw report (patch 0101): frames {}-{} get a GPU timestamp per draw on {}",
                  report_first_, end - 1, backend_->Describe());
    }
    report_on_ = frame_ >= report_first_ && frame_ < end;
    if (report_on_) RepTag(false, "frame start");
    else if (frame_ == end) backend_->SetGpuTag(0);
    if (frame_ < report_first_) return;
    std::vector<gpu::GpuTagTime> got;
    backend_->TakeGpuTagTimes(got);
    rep_times_.insert(rep_times_.end(), got.begin(), got.end());
    if (frame_ < end + 6) return;
    report_done_ = true;
    std::vector<double> ms(report_seq_ + 1, 0.0), px(report_seq_ + 1, -1.0);
    std::vector<std::string> cats(report_seq_ + 1);
    for (const gpu::GpuTagTime& g : rep_times_) {
      if (g.tag > report_seq_) continue;
      if (g.samples >= 0.0) {  // patch 0102
        px[g.tag] = std::max(px[g.tag], 0.0) + g.samples;
        continue;
      }
      ms[g.tag] += g.ms;
      char b[48];
      std::snprintf(b, sizeof b, " c%u %.4f", unsigned(g.category), g.ms);
      if (cats[g.tag].size() < 200) cats[g.tag] += b;
    }
    double total = 0.0, draws = 0.0;
    uint32_t timed = 0, draw_count = 0;
    for (const RepRecord& r : rep_records_) {
      total += ms[r.tag];
      if (r.draw) { draws += ms[r.tag]; ++draw_count; }
      if (ms[r.tag] > 0.0) ++timed;
    }
    uint64_t eft_ps = 0, eft_dis = 0, eft_on = 0;
    xenos::dxbc_spirv::DiscardCounts(eft_ps, eft_dis, eft_on);
    REXLOG_INFO("gow2_renderer: draw report (patch 0101) on {}: {} records ({} draws), {} with GPU time; {:.3f} ms in all, "
                "{:.3f} ms in draws ({} frames; the timestamps between draws slow these frames) | SPIR-V pixel shaders "
                "converted {}, discards {}, as terminate {} (patch 0102)",
                backend_->Describe(), rep_records_.size(), draw_count, timed, total, draws, report_count_, eft_ps, eft_dis,
                eft_on);
    uint32_t k = 0;
    for (const RepRecord& r : rep_records_) {
      if (!r.draw && ms[r.tag] <= 0.0) continue;
      char pxs[40] = "";
      if (px[r.tag] >= 0.0) std::snprintf(pxs, sizeof pxs, "px %.0f ", px[r.tag]);  // patch 0102
      REXLOG_INFO("gow2_renderer: draw report f{} #{} {}{:.4f} ms [{} ] {}{}", r.frame, k++, r.draw ? "draw " : "", ms[r.tag],
                  cats[r.tag], pxs, r.text);
    }
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < rep_records_.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(),
              [&](uint32_t a, uint32_t b) { return ms[rep_records_[a].tag] > ms[rep_records_[b].tag]; });
    for (uint32_t i = 0; i < order.size() && i < 40; ++i) {
      const RepRecord& r = rep_records_[order[i]];
      REXLOG_INFO("gow2_renderer: draw report dearest {}: f{} tag {} {}{:.4f} ms px {:.0f} {}", i + 1, r.frame, r.tag,
                  r.draw ? "draw " : "", ms[r.tag], px[r.tag], r.text.substr(0, 600));
    }
    rep_records_.clear();
    rep_records_.shrink_to_fit();
    rep_times_.clear();
  }
  // Job 234: the GPU time of draws per render target (slot = target id mod 32; a slot taken by a new id is reset).
  void NoteDrawTarget(const rr::RenderTarget* t) {
    if (!t) return;
    const uint32_t slot = t->id & 31u;
    TargetSlot& ts = target_slots_[slot];
    if (ts.id != t->id) {
      const bool had = ts.id != 0;
      ts = TargetSlot{t->id, t->key, t->width, t->height, 0};
      ts.reused = had;
    }
    ++ts.draws;
    draw_cat_ = uint8_t(0x20u + slot);
    GpuCat(draw_cat_);
  }
  // Patch 0033: `line` = the source line where this stretch of the current part ends (a scope starts or ends, or a
  // Switch); a stretch over 20 ms is kept (start and end line) for the HITCH line, so "state 2505 ms" names the code.
  void ChargePart(uint32_t line = 0) {
    if (!parts_on_) return;
    const uint64_t now = __rdtsc();
    const uint64_t dt = now - part_mark_;
    part_ticks_[part_cur_] += dt;
    // Patch 0153: level 1 keeps only this sum and the rare long stretch (an integer compare); level 2 adds the
    // per-line sums below (a backend stats read and a hash per call - most of the timers' cost in job 458).
    if (!slow_min_ticks_) slow_min_ticks_ = uint64_t(20.0 / MsPerTick());
    if (dt > slow_min_ticks_ && slow_stretches_.size() < 8)
      slow_stretches_.push_back({double(dt) * MsPerTick(), part_line_, line, part_cur_});
    if (part_level_ < 2) {
      part_mark_ = now;
      part_line_ = line;
      return;
    }
    // Patch 0036: stretches over 50 us (or that uploaded over 1 MB) are also summed per (part, from line, to line), so
    // a freeze made of many short stretches - the 2-2.7 s 'state' frames of Shagg's sessions - still names its code.
    const uint64_t up = backend_ ? backend_->Stats().upload_bytes : 0;
    const uint64_t dup = up - part_upload_mark_;
    part_upload_mark_ = up;
    if (!agg_min_ticks_) agg_min_ticks_ = uint64_t(0.05 / MsPerTick()) + 1;
    if (dt >= agg_min_ticks_ || dup > (1u << 20)) {
      auto l29 = [](uint32_t l) { return (l & 0x0FFFFFFFu) | ((l & 0x80000000u) ? 0x10000000u : 0u); };
      const uint64_t key = (uint64_t(part_cur_) << 58) | (uint64_t(l29(part_line_)) << 29) | uint64_t(l29(line));
      uint32_t h = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 57);  // 0-127
      for (uint32_t probe = 0; probe < kAggSlots; ++probe, h = (h + 1) & (kAggSlots - 1)) {
        StretchAgg& a = agg_[h];
        if (a.n && a.key != key) continue;
        if (!a.n) {
          a.key = key;
          agg_used_.push_back(uint16_t(h));
        }
        a.ticks += dt;
        a.bytes += dup;
        ++a.n;
        break;
      }
    } else {
      agg_small_ticks_[part_cur_] += dt;
    }
    part_mark_ = now;
    part_line_ = line;
  }
  static constexpr uint32_t kAggSlots = 128;
  struct StretchAgg {
    uint64_t key = 0, ticks = 0, bytes = 0;
    uint32_t n = 0;
  };
  std::array<StretchAgg, kAggSlots> agg_{};
  std::vector<uint16_t> agg_used_;
  uint64_t agg_small_ticks_[kPartCount] = {};
  uint64_t agg_min_ticks_ = 0, part_upload_mark_ = 0;
  uint64_t slow_min_ticks_ = 0;  // patch 0153: 20 ms in time-stamp ticks
  void ClearStretchAgg() {
    for (uint16_t h : agg_used_) agg_[h] = StretchAgg{};
    agg_used_.clear();
    std::fill(std::begin(agg_small_ticks_), std::end(agg_small_ticks_), 0);
  }
  std::string StretchAggText() const {  // the 5 biggest summed stretches over 5 ms, and short stretches per part
    std::vector<const StretchAgg*> v;
    for (uint16_t h : agg_used_) v.push_back(&agg_[h]);
    std::sort(v.begin(), v.end(), [](const StretchAgg* a, const StretchAgg* b) { return a->ticks > b->ticks; });
    std::string t;
    for (size_t i = 0; i < v.size() && i < 5; ++i) {
      const double ms = double(v[i]->ticks) * MsPerTick();
      if (ms < 5.0) break;
      const uint32_t to = uint32_t(v[i]->key & 0x1FFFFFFFu), from = uint32_t((v[i]->key >> 29) & 0x1FFFFFFFu);
      // Lines are shadow_renderer.cpp lines; "end of N" = where the scope opened at line N ends.
      t += std::format(" | {} {:.1f} ms summed over {} stretch(es) from line {}{} to {}{}, {:.1f} MB uploaded",
                       kPartNames[v[i]->key >> 58], ms, v[i]->n, (from & 0x10000000u) ? "end of " : "",
                       from & 0x0FFFFFFFu, (to & 0x10000000u) ? "end of " : "", to & 0x0FFFFFFFu,
                       double(v[i]->bytes) / 1048576.0);
    }
    for (uint32_t p = 0; p < kPartCount; ++p) {
      const double ms = double(agg_small_ticks_[p]) * MsPerTick();
      if (ms >= 20.0) t += std::format(" | {} {:.1f} ms in stretches under 50 us", kPartNames[p], ms);
    }
    return t;
  }
  struct SlowStretch {
    double ms = 0;
    uint32_t from = 0, to = 0;  // source lines; bit 31 of `to` = the end of the scope opened at that line
    Part part = kPartOther;
  };
  std::vector<SlowStretch> slow_stretches_;
  uint32_t part_line_ = 0;
  void EndParts() {
    ChargePart();
    parts_on_ = false;
  }
  struct PartScope {
    ShadowRenderer& s;
    Part prev;
    uint32_t line = 0;  // patch 0033
    PartScope(ShadowRenderer& r, Part p, std::source_location l = std::source_location::current())
        : s(r), prev(r.part_cur_), line(uint32_t(l.line())) {
      s.ChargePart(line);
      s.SetPart(p);
    }
    ~PartScope() {
      s.ChargePart(line | 0x80000000u);
      s.SetPart(prev);
    }
    // Charge what passed so far, continue as another part (restored to `prev` at the end).
    void Switch(Part p, std::source_location l = std::source_location::current()) {
      s.ChargePart(uint32_t(l.line()));
      s.SetPart(p);
    }
    PartScope(const PartScope&) = delete;
    PartScope& operator=(const PartScope&) = delete;
  };

  // ---- Tile trace (upl-n101) ----
  void ParseTileTrace(const std::string& spec, const std::string& pixel) {
    trace_tiles_.reset();
    trace_list_.clear();
    size_t i = 0;
    while (i < spec.size()) {
      size_t j = spec.find(',', i);
      if (j == std::string::npos) j = spec.size();
      const std::string part = spec.substr(i, j - i);
      i = j + 1;
      if (part.empty()) continue;
      unsigned a = 0, b = 0;
      const size_t dash = part.find('-');
      try {
        a = unsigned(std::stoul(part.substr(0, dash)));
        b = dash == std::string::npos ? a : unsigned(std::stoul(part.substr(dash + 1)));
      } catch (...) {
        REXLOG_WARN("gow2_renderer: --gow2_renderer_tile_trace: can't read '{}'", part);
        continue;
      }
      for (unsigned t = a; t <= b && t < ::xenos::edram::kTileCount; ++t) trace_tiles_.set(t);
    }
    for (uint32_t t = 0; t < ::xenos::edram::kTileCount; ++t)
      if (trace_tiles_.test(t)) trace_list_.push_back(t);
    unsigned px = 7, py = 0;
    if (std::sscanf(pixel.c_str(), "%u,%u", &px, &py) == 2) {
      trace_px_ = px;
      trace_py_ = py;
    }
    if (!trace_list_.empty())
      REXLOG_INFO("gow2_renderer: tile trace on {} tile(s) ({}), sample pixel ({},{}) of each, on comparison frames",
                  trace_list_.size(), spec, trace_px_, trace_py_);
  }

  static std::string KeyText(const rr::RenderTarget* t) {
    if (!t) return "none";
    char b[128];
    std::snprintf(b, sizeof(b), "#%u %s base %u pitch %u msaa %ux", t->id, rr::StorageClassName(t->key.storage_class).c_str(),
                  t->key.base_tiles, t->key.pitch_pixels, 1u << t->key.msaa);
    return b;
  }

  void BeginTraceFrame() {
    // The alternate ownership book (--gow2_renderer_alt_rule=always only; the comparison frames it served are gone).
    const bool alt_was = alt_active_;
    alt_active_ = alt_mode_ == 2;
    trace_on_ = !trace_list_.empty();  // cleanup: every frame (was comparison frames only)
    trace_samples_ = 0;
    if (!rts_) return;
    if (!trace_on_) {
      rts_->SetTrace(nullptr, nullptr);
      return;
    }
    rts_->SetTrace(&trace_tiles_, [this](uint32_t tile, uint32_t offset, rr::OwnerBook book, const rr::RenderTarget* prev,
                                         const rr::RenderTarget& target, bool take) {
      if (prev == &target) return;  // no change of hands: not worth a line (upl-n107)
      const bool primary = book == rr::OwnerBook::kPrimary;
      const char* rule = (primary != canary_ownership_) ? "rect" : "canary";
      char b[400];
      std::snprintf(b, sizeof(b), "tile %u (+%u in %s): %s rule %s: %s -> %s%s", tile, offset, KeyText(&target).c_str(),
                    primary ? "ACTED" : "tracked", rule, KeyText(prev).c_str(), take ? KeyText(&target).c_str() : "(read only)",
                    prev == &target ? " (already owner)" : "");
      TraceLine(b, -1);
    });
  }

  void TraceLine(std::string text, int32_t sample) {
    if (trace_lines_.size() < 20000) trace_lines_.push_back({trace_ctx_ + ": " + std::move(text), sample});
  }

  // Calls f(tile, offset) for each traced tile that `rect` (target pixels) touches in target t.
  template <class F>
  void ForTracedTiles(const rr::RenderTarget& t, const rr::PixelRect& rect, F&& f) const {
    if (!trace_on_ || !t.pitch_tiles) return;
    uint32_t c0, c1, r0, r1;
    rr::RenderTargetCache::TileBounds(t, rect, c0, c1, r0, r1);
    for (uint32_t tile : trace_list_) {
      for (uint32_t rel = (tile + ::xenos::edram::kTileCount - t.key.base_tiles) % ::xenos::edram::kTileCount;;
           rel += ::xenos::edram::kTileCount) {
        const uint32_t row = rel / t.pitch_tiles, col = rel % t.pitch_tiles;
        if (row >= r1) break;
        if (row >= r0 && col >= c0 && col < c1) f(tile, rel);
      }
    }
  }

  // One sample of a depth target's guest word (24-bit depth << 8 | stencil) in a tile, as the resolve would write it;
  // returns its index in the frame's trace texture (-1 if none).
  int32_t TraceSample(const rr::RenderTarget& t, uint32_t offset) {
    if (!t.texture || t.key.msaa) return -1;
    const bool colour = !rr::IsDepthClass(t.key.storage_class);
    if (colour ? !trace_colour_ : !depth_resolve_) return -1;
    if (trace_samples_ + 2 > kTraceSamples) return -1;
    if (!trace_tex_) {
      gpu::TextureDesc d;
      d.width = kTraceSamples;
      d.height = 1;
      d.format = gpu::Format::kR32Uint;
      d.usage = gpu::kUsageStorage | gpu::kUsageSampled;
      d.debug_name = "gow2 tile-trace samples";
      trace_tex_ = backend_->CreateTexture(d);
      if (!trace_tex_) return -1;
    }
    const rr::PixelRect pr = rr::RenderTargetCache::TilePixels(t, offset);
    if (pr.empty()) return -1;
    gpu::ComputeBindings b;
    if (colour) {
      b.constants[0] = std::min(pr.x0 + trace_px_, pr.x1 - 1);
      b.constants[1] = std::min(pr.y0 + trace_py_, pr.y1 - 1);
      b.constants[2] = trace_samples_;
      b.constants[3] = t.key.storage_class;
      b.constant_count = 4;
      b.srv[0] = t.texture;
      b.uav_texture = trace_tex_;
      backend_->Dispatch(trace_colour_, b, 1, 1, 1);
      const int32_t index = int32_t(trace_samples_);
      trace_samples_ += 2;
      return index | (rr::RenderTargetCache::Is64bpp(t) ? 0x40000000 : 0x20000000);  // colour: 1 or 2 words
    }
    // The same EDRAM sample as colour pixel trace_px_: depth tiles have their 40-sample halves swapped (upl-n108; the
    // n101-n107 trace read depth at the unswapped column, i.e. a different sample than the colour lines).
    const uint32_t depth_px = (trace_px_ % 80u + 40u) % 80u;
    b.constants[0] = std::min(pr.x0 + depth_px, pr.x1 - 1);
    b.constants[1] = std::min(pr.y0 + trace_py_, pr.y1 - 1);
    b.constants[2] = 1;
    b.constants[3] = 1;
    b.constants[4] = trace_samples_;
    b.constants[5] = 0;
    b.constants[6] = DepthFlags(t.key.storage_class);
    b.constant_count = 8;
    b.srv[0] = t.texture;
    b.srv_plane[0] = 0;
    b.srv[1] = t.texture;
    b.srv_plane[1] = 1;
    b.uav_texture = trace_tex_;
    ResolveDispatch(depth_resolve_, b, 1, 1, 1);
    return int32_t(trace_samples_++);
  }

  // A depth access event on traced tiles: one line per tile, with our value there after the event.
  void TraceEvent(const rr::RenderTarget& t, const rr::PixelRect& rect, const std::string& what) {
    ForTracedTiles(t, rect, [&](uint32_t tile, uint32_t offset) {
      char b[200];
      std::snprintf(b, sizeof(b), "tile %u (+%u in %s): %s", tile, offset, KeyText(&t).c_str(), what.c_str());
      TraceLine(b, TraceSample(t, offset));
    });
  }

  void EndTraceFrame() {
    if (trace_lines_.empty()) return;
    PendingTrace pt;
    pt.frame = frame_;
    pt.lines = std::move(trace_lines_);
    trace_lines_.clear();
    if (trace_samples_ && trace_tex_)
      pt.readback = backend_->ReadbackTexture(trace_tex_, gpu::Rect{0, 0, int32_t(trace_samples_), 1});
    pending_traces_.push_back(std::move(pt));
    trace_samples_ = 0;
  }

  void FlushTraces(bool wait) {
    while (!pending_traces_.empty()) {
      PendingTrace& pt = pending_traces_.front();
      if (pt.readback && !backend_->IsReadbackReady(pt.readback)) {
        if (!wait) return;
        backend_->WaitForFence(backend_->Submit());
        if (!backend_->IsReadbackReady(pt.readback)) return;
      }
      uint32_t pitch = 0;
      const uint8_t* data = pt.readback ? backend_->ReadbackData(pt.readback, pitch) : nullptr;
      REXLOG_INFO("gow2_renderer: tile trace frame {}: {} event(s) (values: our guest word 24-bit depth << 8 | "
                  "stencil at the sample pixel, after the event)", pt.frame, pt.lines.size());
      for (const auto& l : pt.lines) {
        if (l.sample >= 0 && data) {
          const uint32_t index = uint32_t(l.sample) & 0xFFFFFFu;
          uint32_t v[2] = {};
          std::memcpy(&v[0], data + size_t(index) * 4, 4);
          if (l.sample & 0x60000000) std::memcpy(&v[1], data + size_t(index + 1) * 4, 4);
          if (l.sample & 0x40000000) {
            REXLOG_INFO("gow2_renderer:   trace {} -> ours {:08X} {:08X} (guest words)", l.text, v[0], v[1]);
          } else if (l.sample & 0x20000000) {
            REXLOG_INFO("gow2_renderer:   trace {} -> ours {:08X} (guest word)", l.text, v[0]);
          } else {
            REXLOG_INFO("gow2_renderer:   trace {} -> ours {:08X}", l.text, v[0]);
          }
        } else {
          REXLOG_INFO("gow2_renderer:   trace {}", l.text);
        }
      }
      if (pt.readback) backend_->ReleaseReadback(pt.readback);
      pending_traces_.pop_front();
    }
  }

  void Skip(const char* reason) {
    ++stats_.skipped[reason];
    TraceDraw(reason);  // patch 0016
  }

  // Patch 0016: where a --gow2_renderer_log_vs draw ends (drawn, or the skip / return that stopped it).
  void TraceDraw(const char* outcome) {
    if (!trace_draw_) return;
    trace_draw_ = false;  // one outcome per draw
    REXLOG_INFO("gow2_renderer: vs-trace frame {} draw {}: {}", frame_ + 1, draw_index_, outcome);
  }

  void Draw(const uint32_t* r, uint32_t prim, uint32_t index_count, const ObservedIndexBuffer* ib, bool explicit_major) {
    ++stats_.draws;
    const uint64_t vs_hash = vs_.dwords ? vs_.hash : 0, ps_hash = ps_.dwords ? ps_.hash : 0;
    trace_draw_ = log_vs_hash_ && vs_hash == log_vs_hash_ && trace_logged_ < log_vs_max_;  // patch 0016
    if (trace_draw_) ++trace_logged_;
    const uint32_t surface_info = r[rr::reg::RB_SURFACE_INFO];
    const uint32_t msaa = Bits(surface_info, 16, 2);
    const uint32_t pitch = Bits(surface_info, 0, 14);
    // The D3D library's no-op single-point draws (notes/35): nothing to draw, state is in the register file anyway.
    if (prim == 0x01 && index_count == 1 && (!hints_.dummy_point_vs || vs_hash == hints_.dummy_point_vs)) {
      ++stats_.dummy_points;
      return;
    }
    // The D3D library's fast clear: fixed shader pair, a 3-vertex rectangle list at half pitch with 4x MSAA. Same
    // tiles as the 1x surface at full pitch (notes/35, Brief 8), so draw it at 2x scale onto that surface.
    const bool is_clear = hints_.clear_vs && vs_hash == hints_.clear_vs && ps_hash == hints_.clear_ps && prim == 0x08 &&
                          msaa == 2;
    fill_.valid = false;  // job 185: set below for this draw only
    fill_geom_.valid = false;  // job 188
    if (trace_on_) {
      char b[200];
      std::snprintf(b, sizeof(b), "frame %llu draw %u (prim %u vs %016llX ps %016llX%s, surface pitch %u msaa %ux)",
                    static_cast<unsigned long long>(frame_ + 1), draw_index_, prim,
                    static_cast<unsigned long long>(vs_hash), static_cast<unsigned long long>(ps_hash),
                    is_clear ? " FAST CLEAR, redirected to pitch x2 1x" : "", pitch, 1u << msaa);
      trace_ctx_ = b;
    }
    // Per-target scale (notes/53b): the scale of the targets this draw renders into (colour and depth share the
    // surface pitch and MSAA, so one scale per draw).
    const uint32_t draw_scale = rts_->ScaleFor(is_clear ? pitch * 2 : pitch, is_clear ? 0u : msaa);
    rr::PipelineInputs in{prim, explicit_major, is_clear ? 1u : 0u, float(draw_scale) * (is_clear ? 2.0f : 1.0f)};
    in.xenos_sample_positions = msaa_positions_mode_ != 2;  // patch 0089: canary_off - no viewport sample correction
    if (msaa_positions_mode_ == 1 && msaa_positions_ok_) {  // patch 0089: rasterize MSAA at the Xenos positions
      // Canary draw_util.cc GetHostSampleXenosPositions: 4x host i = Xenos i; native 2x host 0 = Xenos 1, 1 = Xenos 0.
      static constexpr int8_t kX4[4][2] = {{-2, -2}, {2, 2}, {-6, 6}, {6, -6}};
      static constexpr int8_t kX2[2][2] = {{4, -4}, {-4, 4}};
      const uint32_t samples = is_clear ? 1u : (1u << msaa);
      if (samples >= 4) backend_->SetSamplePositions(4, kX4);
      else if (samples == 2) backend_->SetSamplePositions(2, kX2);
      else backend_->SetSamplePositions(0, nullptr);
    }
    rr::PipelineState ps = rr::TranslatePipelineState(r, in);
    uint32_t tess_n = 0;
    // Xenia's primitive processor also tessellates triangle and quad LISTS when the draw is in explicit major mode with
    // the tessellation path selected (control-point-indexed domain: the VS gets the domain location in r0 and the
    // control point indices in r1). We draw those as plain lists today: log each kind once (notes/51 section 14).
    // Patch 0087: strips and fans too (Canary 9c30b16 converts indexed tessellated triangle strips to lists - faces
    // missing in a cinematic); we only log them so far.
    if (ps.topology != rr::Topology::Patch && explicit_major &&
        (prim == 0x04 || prim == 0x05 || prim == 0x06 || prim == 0x0D) &&
        (r[0x2284] & 3u) == 1u && patch_kinds_logged_.size() < 32) {
      const uint32_t mode = r[kRegVgtHosCntl] & 3u;
      const uint64_t kind = uint64_t(prim) | uint64_t(mode) << 8 | uint64_t(1) << 11 | uint64_t(vs_hash) << 16;
      if (patch_kinds_logged_.insert(kind).second)
        REXLOG_INFO("gow2_renderer: tessellation: tessellated LIST draw drawn as a plain list - prim 0x{:02X} ({}), mode "
                    "{}, {}, {} indices, max level {:.3f}, min level {:.3f}, VS {:016X} PS {:016X} | frame {}",
                    prim, prim == 0x04 ? "triangle list" : prim == 0x05 ? "triangle fan" : prim == 0x06 ? "triangle strip" : "quad list", mode, ib ? "indexed" : "auto-indexed",
                    index_count, std::bit_cast<float>(r[kRegVgtHosMaxTessLevel]),
                    std::bit_cast<float>(r[kRegVgtHosMaxTessLevel + 1]), vs_hash, ps_hash, frame_ + 1);
    }
    if (ps.topology == rr::Topology::Patch) {
      ++stats_.patches;
      // Quad-patch tessellation (notes/51 section 10; the chapter 18 water): auto-indexed patches drawn as a uniform
      // N x N grid per patch through the guest vertex shader's tessellation variant (hlsl.h tess_quad_segments). Every
      // edge factor is VGT_HOS_MAX_TESS_LEVEL + 1 (Xenia d3d12_command_processor.cc); continuous mode is fractional_even,
      // so the grid uses the next even count (exact for the game's level 3 -> 4); discrete is integer. Adaptive mode
      // (per-patch factors from memory) and indexed patch lists are not handled yet - counted, not drawn.
      const uint32_t mode = r[kRegVgtHosCntl] & 3u;
      if (!tessellation_ || prim != 0x12 || ib || mode == 2u || !index_count) {
        ++stats_.patches_not_drawn;
        // Once per kind: what this patch draw is, so the next tessellation case can be built (notes/51 section 14).
        const uint64_t kind = uint64_t(prim) | uint64_t(mode) << 8 | uint64_t(ib ? 1 : 0) << 10 | uint64_t(vs_hash) << 16;
        if (tessellation_ && patch_kinds_logged_.size() < 32 && patch_kinds_logged_.insert(kind).second)
          REXLOG_INFO("gow2_renderer: tessellation: patch draw NOT drawn - prim 0x{:02X} ({}), mode {} ({}), {}, {} "
                      "indices/patches, first {}, max level {:.3f}, min level {:.3f}, VS {:016X} PS {:016X} | frame {}",
                      prim, prim == 0x12 ? "quad patch" : prim == 0x11 ? "triangle patch" : prim == 0x10 ? "line patch" : "?",
                      mode, mode == 0u ? "discrete" : mode == 1u ? "continuous" : mode == 2u ? "adaptive" : "?",
                      ib ? "indexed" : "auto-indexed", index_count, r[kRegVgtIndxOffset],
                      std::bit_cast<float>(r[kRegVgtHosMaxTessLevel]), std::bit_cast<float>(r[kRegVgtHosMaxTessLevel + 1]),
                      vs_hash, ps_hash, frame_ + 1);
        return;
      }
      const float f = std::clamp(std::bit_cast<float>(r[kRegVgtHosMaxTessLevel]) + 1.0f, 1.0f, 64.0f);
      uint32_t n = mode == 1u ? 2u * uint32_t(std::ceil(f * 0.5f)) : uint32_t(std::ceil(f));
      n = std::clamp(n * uint32_t(std::clamp(tess_scale_, 1, 8)), 1u, 64u);
      const uint64_t verts = uint64_t(index_count) * 6u * n * n;
      if (verts > (1ull << 26)) {
        ++stats_.patches_not_drawn;
        return;
      }
      if (!tess_logged_) {
        tess_logged_ = true;
        REXLOG_INFO("gow2_renderer: tessellation: first quad-patch draw - {} patches from {}, mode {} ({}), max level "
                    "{:.3f}, min level {:.3f} -> {} segments per edge ({} vertices), VS {:016X}",
                    index_count, r[kRegVgtIndxOffset], mode, mode == 1u ? "continuous" : "discrete",
                    std::bit_cast<float>(r[kRegVgtHosMaxTessLevel]), std::bit_cast<float>(r[kRegVgtHosMaxTessLevel + 1]),
                    n, verts, vs_hash);
      }
      tess_n = n;
      index_count = uint32_t(verts);
      ps.topology = rr::Topology::TriangleList;
      ++stats_.tessellated;
    }
    if (!vs_.dwords) {
      Skip("no vertex shader");
      return;
    }
    const rr::GuestShader& vsg = cache_->Get(vs_hash, vs_.dwords, vs_.count, true, tess_n);
    if (ps.skip) {
      if (rr::SkipUnlessVertexMemexport(ps.skip_reason) && vsg.memexport) {
        if (memexport_) {
          MemexportOnlyDraw(r, vsg, prim, index_count, ib);
          return;
        }
        Skip("vertex shader memexport (not supported in M1)");
      } else {
        Skip(ps.skip_reason_text);
      }
      return;
    }
    const uint32_t edram_mode = Bits(r[rr::reg::RB_MODECONTROL], 0, 3);
    const rr::GuestShader* psg =
        ps_.dwords ? &cache_->Get(ps_hash, ps_.dwords, ps_.count, false) : nullptr;
    // Shared by all titles, including shader construction paths without a UE3
    // notification hook. Hide stage-cache I/O behind vertex/texture preparation.
    // The backend checks each handle once; live pipeline work retains priority.
    if (REXCVAR_GET(gow2_renderer_stage_prefetch)) {
      if (vsg.ready() && vsg.handle) backend_->PrefetchShaderStage(vsg.handle);
      if (psg && psg->ready() && psg->handle) backend_->PrefetchShaderStage(psg->handle);
    }

    // Patch 0025 (Canary render_target_cache.cc Update, 9da693480; Gears 3 review 01-g): a depth target at the same
    // guest EDRAM base as a colour target this draw writes is not bound - the colour target owns those tiles - unless
    // the depth access is read-only (test, no depth or stencil writes: Canary's keep_aliased_depth).
    if (depth_alias_drop_ && (ps.depth_stencil.depth_enable || ps.depth_stencil.stencil_enable)) {
      const uint32_t dbase = Bits(r[rr::reg::RB_DEPTH_INFO], 0, 12);
      const uint32_t psc = (edram_mode == 4 && psg) ? psg->color_targets_written : 0;
      bool alias = false;
      for (uint32_t i = 0; i < 4; ++i)
        alias |= ps.blend.rt[i].write_mask && ((psc >> i) & 1) && Bits(r[rr::reg::kColorInfo[i]], 0, 12) == dbase;
      rr::DepthStencilState& dsm = ps.depth_stencil;
      const bool read_only = dsm.depth_enable && !dsm.depth_write && !dsm.stencil_enable;
      if (alias && !read_only) {
        dsm.depth_enable = false;
        dsm.depth_write = false;
        dsm.stencil_enable = false;
        ++stats_.depth_alias_dropped;
        TraceDraw("depth binding dropped: shares a colour target's EDRAM base (patch 0025)");
      }
    }
    // Depth / stencil state (needed first: the Canary ownership rule binds depth whenever the test is on).
    const rr::DepthStencilState& ds = ps.depth_stencil;
    auto op_writes = [](const rr::StencilFace& f) {
      return f.fail_op != rr::StencilOp::Keep || f.depth_fail_op != rr::StencilOp::Keep ||
             f.pass_op != rr::StencilOp::Keep;
    };
    const bool stencil_writes = ds.stencil_enable && ds.single_write_mask && (op_writes(ds.front) || op_writes(ds.back));
    if (ds.stencil_enable) {  // job 179: which stencil bits the title can ever set to 1, and which it ever reads
      auto ones = [&](const rr::StencilFace& f) {
        uint8_t m = 0;
        for (rr::StencilOp op : {f.fail_op, f.depth_fail_op, f.pass_op}) {
          if (op == rr::StencilOp::Replace) m |= ds.single_ref;
          else if (op != rr::StencilOp::Keep && op != rr::StencilOp::Zero) m |= 0xFF;
        }
        return uint8_t(m & ds.single_write_mask);
      };
      stencil_ones_draws_ |= uint8_t(ones(ds.front) | ones(ds.back));
      if (ds.front.func != rr::CompareFunc::Always || ds.back.func != rr::CompareFunc::Always)
        stencil_reads_ |= ds.single_read_mask;
    }
    const bool depth_bound = ds.depth_enable || ds.stencil_enable;
    xfer_draw_ = {vs_hash, ps_hash, prim, uint32_t(ds.depth_func), ds.depth_write ? 1u : 0u, ds.stencil_enable ? 1u : 0u,
                  ds.single_write_mask, 0u};
    const uint32_t depth_info = r[rr::reg::RB_DEPTH_INFO];
    const uint32_t depth_format = Bits(depth_info, 16, 1);
    const rr::RenderTargetKey key{Bits(depth_info, 0, 12) & 2047u, rr::DepthStorageClass(depth_format),
                                  is_clear ? pitch * 2 : pitch, is_clear ? 0u : msaa};
    // Gears 3 job 025 (asked by the Gears 2 chat): the shadow-map-like D24S8 target at this EDRAM base differs from the
    // reference. On comparison frames, log each depth draw into it: cull / polygon-offset enables and the four offset
    // registers (D3D12 allows one bias per draw: with culling off pipeline_state.h takes front if set, else back).
    if (depth_bound && int32_t(key.base_tiles) == REXCVAR_GET(gow2_renderer_log_depth_bias_base) &&
        depth_bias_logged_ < 400) {
      ++depth_bias_logged_;
      const uint32_t m = r[rr::reg::PA_SU_SC_MODE_CNTL];
      auto fl = [&](uint32_t reg) { return std::bit_cast<float>(r[reg]); };
      REXLOG_INFO("gow2_renderer: depth-bias frame {} draw {} (prim {} vs {:016X} ps {:016X}) target D{} base {} pitch {} msaa {}x: "
                  "PA_SU_SC_MODE_CNTL {:08X} cull front {} back {} | offset enable front {} back {} para {} | front "
                  "scale {} offset {} | back scale {} offset {} | depth func {} write {}",
                  frame_ + 1, draw_index_, prim, vs_hash, ps_hash, depth_format ? "24FS8" : "24S8", key.base_tiles,
                  key.pitch_pixels, 1u << key.msaa, m, Bits(m, 0, 1), Bits(m, 1, 1), Bits(m, 11, 1), Bits(m, 12, 1), Bits(m, 13, 1),
                  fl(rr::reg::PA_SU_POLY_OFFSET_FRONT_SCALE), fl(rr::reg::PA_SU_POLY_OFFSET_FRONT_OFFSET),
                  fl(rr::reg::PA_SU_POLY_OFFSET_BACK_SCALE), fl(rr::reg::PA_SU_POLY_OFFSET_BACK_OFFSET),
                  uint32_t(ds.depth_func), ds.depth_write ? 1 : 0);
    }

    // Job 185: the rect-list fill/clear shader's exact rectangle (job 184: 7-float vertices, x y z first, in pixels).
    // Job 188: its bounds also limit the draw's ownership rectangle (the tiles it really writes), when switched on.
    fill_.valid = false;
    fill_geom_.valid = false;
    // Gears 3 (job 032): the shadow-map fills draw 3 rectangles at once (9 vertices). With
    // --gow2_renderer_fill_rect_lists (default on) any whole number of rectangles (up to 16) bounds ownership by the
    // union of its vertices instead of falling back to the scissor; the exact fill_trim rectangle stays 3-vertex only.
    const bool fill_count_ok = index_count == 3 || index_count == 6 ||
                               (fill_rect_lists_ && index_count % 3 == 0 && index_count >= 9 && index_count <= 48);
    if ((fill_trim_ || fill_ownership_) && vs_hash == 0x1E6883FCCDE1F688ull && prim == 0x08 && !ib && fill_count_ok &&
        ps.viewport.xy_divided_by_w) {
      const uint32_t dw0 = r[kRegFetchConst], dw1 = r[kRegFetchConst + 1];
      const uint32_t addr = dw0 & 0x1FFFFFFCu, size = ((dw1 >> 2) & 0xFFFFFFu) * 4;
      if ((dw0 & 3) == 3 && size == index_count * 28u && addr + size <= (512u << 20)) {
        const uint8_t* mem = memory_->TranslatePhysical<const uint8_t*>(addr);
        auto f = [&](uint32_t v, uint32_t c) {
          uint32_t w;
          std::memcpy(&w, mem + 28 * v + 4 * c, 4);
          return std::bit_cast<float>(std::byteswap(w));
        };
        auto hx = [&](float x) {
          return (x * ps.viewport.ndc_scale[0] + ps.viewport.ndc_offset[0] + 1.0f) * 0.5f * ps.viewport.width + ps.viewport.x;
        };
        auto hy = [&](float y) {
          return (y * ps.viewport.ndc_scale[1] + ps.viewport.ndc_offset[1] + 1.0f) * 0.5f * ps.viewport.height + ps.viewport.y;
        };
        // One rectangle only (3 vertices); 6 = two rectangles, used only if they form one.
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (uint32_t v = 0; v < index_count; ++v) {
          x0 = std::min(x0, hx(f(v, 0)));
          x1 = std::max(x1, hx(f(v, 0)));
          y0 = std::min(y0, hy(f(v, 1)));
          y1 = std::max(y1, hy(f(v, 1)));
        }
        // Conservative bounds (whole pixels outwards), clipped to the scissor: the tiles the draw can write.
        if (fill_ownership_ && std::isfinite(x0) && std::isfinite(x1) && std::isfinite(y0) && std::isfinite(y1)) {
          const float sx0 = float(ps.scissor.x), sy0 = float(ps.scissor.y);
          const float sx1 = float(ps.scissor.x + ps.scissor.width), sy1 = float(ps.scissor.y + ps.scissor.height);
          const float gx0 = std::clamp(std::floor(x0 + 0.01f), sx0, sx1), gx1 = std::clamp(std::ceil(x1 - 0.01f), sx0, sx1);
          const float gy0 = std::clamp(std::floor(y0 + 0.01f), sy0, sy1), gy1 = std::clamp(std::ceil(y1 - 0.01f), sy0, sy1);
          fill_geom_.valid = true;
          fill_geom_.rect = {uint32_t(gx0), uint32_t(gy0), uint32_t(std::max(gx0, gx1)), uint32_t(std::max(gy0, gy1))};
          ++stats_.fill_ownership_draws;
          if (index_count > 6) ++stats_.fill_multi_rect_draws;
        }
        const gpu::Rect rr{int32_t(std::lround(x0)), int32_t(std::lround(y0)), int32_t(std::lround(x1)),
                           int32_t(std::lround(y1))};
        const bool aligned = std::fabs(x0 - std::round(x0)) < 0.01f && std::fabs(y0 - std::round(y0)) < 0.01f &&
                             std::fabs(x1 - std::round(x1)) < 0.01f && std::fabs(y1 - std::round(y1)) < 0.01f;
        if (fill_trim_ && index_count == 3 && aligned && rr.x1 > rr.x0 && rr.y1 > rr.y0) {
          // Clipped to the scissor (the draw writes nothing outside it).
          fill_.rect = {std::max(rr.x0, int32_t(ps.scissor.x)), std::max(rr.y0, int32_t(ps.scissor.y)),
                        std::min(rr.x1, int32_t(ps.scissor.x + ps.scissor.width)),
                        std::min(rr.y1, int32_t(ps.scissor.y + ps.scissor.height))};
          auto always = [](const rr::StencilFace& sf) {
            return sf.func == rr::CompareFunc::Always && sf.pass_op == rr::StencilOp::Replace;
          };
          const bool depth_always = !ds.depth_enable || ds.depth_func == rr::CompareFunc::Always;
          fill_.depth = ds.depth_write && ds.depth_func == rr::CompareFunc::Always;
          fill_.stencil = ds.stencil_enable && ds.single_write_mask == 0xFF && depth_always && always(ds.front) &&
                          always(ds.back);
          fill_.valid = fill_.rect.x1 > fill_.rect.x0 && fill_.rect.y1 > fill_.rect.y0 && (fill_.depth || fill_.stencil);
          if (fill_rects_logged_ < 12) {
            ++fill_rects_logged_;
            REXLOG_INFO("gow2_renderer: fill rect {}: host {},{}-{},{} (scissor-clipped {},{}-{},{}) is_clear {} depth {} "
                        "stencil {} valid {}", fill_rects_logged_, rr.x0, rr.y0, rr.x1, rr.y1, fill_.rect.x0,
                        fill_.rect.y0, fill_.rect.x1, fill_.rect.y1, is_clear ? 1 : 0, fill_.depth ? 1 : 0,
                        fill_.stencil ? 1 : 0, fill_.valid ? 1 : 0);
          }
        }
      }
    }
    // Colour targets (M2): every target the pixel shader writes with a non-zero mask (Canary/SDK: the others are
    // not bound and take no ownership). A slot with the same EDRAM base as a lower one is dropped (Canary
    // render_target_cache.cc 866-905). Formats not modelled yet keep tile ownership only.
    const uint32_t ps_colour = (edram_mode == 4 && psg) ? psg->color_targets_written : 0;
    bool writes_colour = false;
    rr::RenderTargetKey colour_keys[4];
    uint32_t colour_count = 0, colour_slot[4] = {};
    uint32_t bound_bases[5], bound_count = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!ps.blend.rt[i].write_mask || !((ps_colour >> i) & 1)) continue;
      const uint32_t ci = r[rr::reg::kColorInfo[i]];
      const rr::RenderTargetKey ck{Bits(ci, 0, 12) & 2047u, rr::ColorStorageClass(Bits(ci, 16, 4)),
                                   is_clear ? pitch * 2 : pitch, is_clear ? 0u : msaa};
      bool duplicate = false;
      for (uint32_t k = 0; k < colour_count; ++k) duplicate |= colour_keys[k].base_tiles == ck.base_tiles;
      if (duplicate) {
        ++stats_.mrt_duplicates;
        continue;
      }
      colour_keys[colour_count] = ck;
      colour_slot[colour_count] = i;
      bound_bases[bound_count++] = ck.base_tiles;
      ++colour_count;
    }
    if (depth_bound) bound_bases[bound_count++] = key.base_tiles;
    rr::RenderTarget* colour_rt[4] = {};  // per guest slot: the targets this draw renders into
    bool draw_colour = false;
    for (uint32_t k = 0; k < colour_count; ++k) {
      PartScope part(*this, kPartOwnership);
      writes_colour = true;
      const uint32_t i = colour_slot[k];
      const uint32_t ci = r[rr::reg::kColorInfo[i]];
      // A clear with a constant colour leaves known EDRAM bits that a later depth target on these tiles can take.
      bool known = false;
      uint32_t bits = 0;
      if (is_clear && i == 0 && psg && psg->constant_color0 >= 0) {
        float c[4];
        std::memcpy(c, &r[kRegFloatConstPs + 4 * uint32_t(psg->constant_color0)], 16);
        known = ColorClearBits(Bits(ci, 16, 4), c, bits);
      }
      const bool drawable = draw_colour_ && colour_keys[k].msaa == 0 &&
                            rr::ColorHostFormat(colour_keys[k].storage_class) != gpu::Format::kUnknown;
      if (!drawable && draw_colour_) ++stats_.colour_not_modelled[colour_keys[k].storage_class & 15];
      if (rr::RenderTarget* t = rts_->Get(colour_keys[k], drawable)) {
        const Ownership cown =
            OwnershipFor(ps, true, true, CanaryLimit(colour_keys[k].base_tiles, bound_bases, bound_count));
        if (drawable && t->texture) {
          PrepareColourTarget(*t, cown, known, bits);
          colour_rt[i] = t;
          draw_colour = true;
        } else {
          for (uint32_t book = 0; book < 2; ++book)
            if (cown.active[book])
              rts_->MarkWritten(*t, cown.rect[book], nullptr, nullptr, known, bits, rr::OwnerBook(book), true,
                                cown.limit[book]);
        }
      }
    }
    if (is_clear && writes_colour) ++stats_.colour_clears;

    const bool depth_writes = ds.depth_write || stencil_writes;
    const Ownership own = OwnershipFor(ps, depth_writes, depth_bound, CanaryLimit(key.base_tiles, bound_bases, bound_count));
    // Stage 4 (upl-s4d, job 118): a draw inside an open occlusion query is drawn even when it writes nothing - UE3's
    // bounding-box proxies test depth with colour and depth writes off, and their passing samples ARE the query's
    // answer. It is drawn with the game's state (depth test, no writes), never into the comparison draw-ID target.
    if (trace_draw_) {  // patch 0016
      const rr::RenderTargetKey k0 = colour_count ? colour_keys[0] : rr::RenderTargetKey{};
      REXLOG_INFO("gow2_renderer: vs-trace frame {} draw {}: ps {:016X} edram mode {} psg {} (state {}) targets "
                  "written {:X} mask0 {:X} | colour targets {} key0 base {} class {} pitch {} msaa {} | draw colour {} | "
                  "depth bound {} writes {} | drawn rect {},{}-{},{} | surface msaa {} pitch {}",
                  frame_ + 1, draw_index_, ps_hash, edram_mode, psg ? 1 : 0, psg ? int(psg->state) : -1, ps_colour,
                  ps.blend.rt[0].write_mask, colour_count, k0.base_tiles, k0.storage_class, k0.pitch_pixels, k0.msaa,
                  draw_colour ? 1 : 0, depth_bound ? 1 : 0, depth_writes ? 1 : 0, own.drawn.x0, own.drawn.y0,
                  own.drawn.x1, own.drawn.y1, msaa, pitch);
    }
    const bool query_proxy = !depth_writes && !draw_colour && occ_open_.open && occ_open_.handle != 0;
    if (query_proxy) ++occ_stats_.proxies;
    if (!depth_writes && !draw_colour && !query_proxy) {
      // Nothing we draw. Under the Canary rule a depth-tested draw still takes ownership of the depth tiles (and
      // transfers into them when that is the rule we act on).
      if (depth_bound && (own.active[0] || own.active[1]))
        if (rr::RenderTarget* t = canary_ownership_ ? rts_->Get(key) : rts_->Get(key, false))
          PrepareDepthTarget(*t, own, false, false);
      TraceDraw("skipped: nothing we draw (no colour target taken, no depth write)");  // patch 0016
      ++stats_.colour_only;  // colour into formats not modelled yet (or colour switched off)
      return;
    }
    if (own.drawn.empty()) {
      TraceDraw("skipped: empty drawn rectangle");  // patch 0016
      ++stats_.empty;
      return;
    }
    // Job 184: what the rect-list fill/clear shader (the draw that asks for most remaining depth transfers) draws -
    // its vertices, so its exact coverage can be computed instead of assuming the scissor rectangle.
    if (vs_hash == 0x1E6883FCCDE1F688ull && fill_logged_ < 16) {
      ++fill_logged_;
      std::string fetches;
      const uint8_t* mem = memory_->TranslatePhysical<const uint8_t*>(0);
      for (uint32_t i = 0; i < 96; ++i) {
        if (!((vsg.vfetch_constants[i >> 5] >> (i & 31)) & 1)) continue;
        const uint32_t dw0 = r[kRegFetchConst + 2 * i], dw1 = r[kRegFetchConst + 2 * i + 1];
        const uint32_t addr = dw0 & 0x1FFFFFFCu, size = ((dw1 >> 2) & 0xFFFFFFu) * 4;
        char b[96];
        std::snprintf(b, sizeof(b), " slot %u dw0 %08X dw1 %08X size %u:", i, dw0, dw1, size);
        fetches += b;
        for (uint32_t k = 0; k < std::min<uint32_t>(size / 4, 18); ++k) {
          uint32_t w;
          std::memcpy(&w, mem + addr + 4 * k, 4);
          w = std::byteswap(w);
          std::snprintf(b, sizeof(b), " %g", std::bit_cast<float>(w));
          fetches += b;
        }
      }
      fetches += " | vs c0-c3:";
      for (uint32_t k = 0; k < 16; ++k) {
        char b[32];
        std::snprintf(b, sizeof(b), " %g", std::bit_cast<float>(r[kRegFloatConstVs + k]));
        fetches += b;
      }
      fetches += " | vs float constants used " + std::to_string(vsg.float_constants);
      REXLOG_INFO("gow2_renderer: fill draw {}: prim {} count {} is_clear {} | viewport {} {} {} {} z {} {} scale {} {} {} "
                  "offset {} {} {} xy_div_w {} | scissor {},{} {}x{} | drawn {},{}-{},{} | vfetch formats {:X} |{}",
                  fill_logged_, prim, index_count, is_clear ? 1 : 0, ps.viewport.x, ps.viewport.y, ps.viewport.width,
                  ps.viewport.height, ps.viewport.z_min, ps.viewport.z_max, ps.viewport.ndc_scale[0],
                  ps.viewport.ndc_scale[1], ps.viewport.ndc_scale[2], ps.viewport.ndc_offset[0],
                  ps.viewport.ndc_offset[1], ps.viewport.ndc_offset[2], ps.viewport.xy_divided_by_w ? 1 : 0,
                  ps.scissor.x, ps.scissor.y, ps.scissor.width, ps.scissor.height, own.drawn.x0, own.drawn.y0,
                  own.drawn.x1, own.drawn.y1, vsg.vfetch_formats, fetches);
    }
    if (VertexLogFrame() && (prim == 0x08 || index_count <= 6)) LogDrawVertices(r, vsg, prim, index_count, vs_hash, ps_hash, ps);
    if (ps.viewport.clip_disabled && depth_writes) ++stats_.unclipped_depth_draws;  // ownership from the scissor alone
    rr::RenderTarget* target = nullptr;  // depth/stencil target, if the draw tests or writes depth/stencil
    if (depth_bound) {
      target = rts_->Get(key);
      if (!target) {
        TraceDraw("skipped: no depth target");  // patch 0016
        ++stats_.other_failures;
        return;
      }
      if (depth_writes) {
        // A draw never skips its transfer: its real coverage is its geometry, which can be far smaller than the
        // scissor/viewport rectangle (job 105: fast clears with an unbounded scissor). Transferring first is always
        // right: what the draw covers it then overwrites.
        PrepareDepthTarget(*target, own, false, is_clear);
      } else if (canary_ownership_) {
        PrepareDepthTarget(*target, own, false, false);  // the Canary rule claims depth for a test-only draw
      } else {
        // Test-only (colour draw with the depth/stencil test on): our texture needs the tiles' current content, but
        // on the hardware the draw changes no depth ownership (rect rule).
        Ownership ro = OwnershipForRect(own.drawn);
        ro.active[1] = false;
        PrepareDepthTarget(*target, ro, false, false, true);
        if (own.active[1]) {
          Ownership alt = own;
          alt.active[0] = false;
          PrepareDepthTarget(*target, alt, false, false);
        }
      }
    }

    // Patch 0045 (Edge): would skipping this draw while its shaders / pipeline compile leave a hole that stays?
    const char* standin_wait = nullptr;
    if (standin_wait_budget_ms_ > 0.0) {
      const bool has_target = colour_count != 0 || depth_bound;
      standin_wait = StandInWaitReason(colour_count ? colour_keys[0] : key, has_target, vsg, psg);
      if (standin_wait && standin_wait_left_ms_ > 0.0) {
        const auto t0 = std::chrono::steady_clock::now();
        if (vsg.state == rr::ShaderState::kPending) cache_->WaitReady(vsg, standin_wait_left_ms_);
        const double left = standin_wait_left_ms_ - MsSince(t0);
        if (psg && psg->state == rr::ShaderState::kPending && left > 0.0) cache_->WaitReady(*psg, left);
        const double ms = MsSince(t0);
        standin_wait_left_ms_ -= ms;
        ++stats_.standin_waits;
        stats_.standin_wait_ms += ms;
        ++standin_reasons_[standin_wait];
      } else if (standin_wait) {
        ++stats_.standin_over_budget;
        standin_wait = nullptr;  // this frame's budget is spent: skip as before
      }
    }
    // Shaders. Compiles run on worker threads: a draw whose shaders are not ready yet is skipped (by default never
    // waited for; --gow2_renderer_shader_wait_ms gives it a per-frame budget), and the resolves after it are reported
    // as incomplete rather than as differences.
    // Patch 0024 (Edge c8503efe6): an exporting draw waits for its shaders instead of losing its export.
    const bool export_wait = memexport_ && REXCVAR_GET(gow2_renderer_export_wait) &&
                             (vsg.memexport || (psg && psg->memexport));
    if (export_wait && vsg.state == rr::ShaderState::kPending && cache_->WaitReady(vsg, 2000.0)) ++stats_.export_waits;
    if (export_wait && psg && psg->state == rr::ShaderState::kPending && cache_->WaitReady(*psg, 2000.0))
      ++stats_.export_waits;
    // Patch 0052 (Shagg 13:38: what the player notices first gets its shaders first, the rest fills in after): a
    // blended draw that writes no depth and is not a full-screen pass (decals, particles, glows, fog cards) does not
    // stop the frame for a compiling shader - it is left out until its shader is built (the compile threads already
    // have it, plus the shaders of the draws queued after it), a frame or a few later. Everything else - opaque or
    // depth-writing geometry (characters, faces, walls, ground), full-screen and post passes, exports - waits.
    if (defer_cosmetic_ && !export_wait &&
        (vsg.state == rr::ShaderState::kPending || (psg && psg->state == rr::ShaderState::kPending))) {
      bool blends = false;
      for (uint32_t i = 0; i < 4; ++i)
        if (colour_rt[i] && ps.blend.rt[i].blend_enable) blends = true;
      const bool writes_depth = depth_bound && ps.depth_stencil.depth_enable && ps.depth_stencil.depth_write;
      const bool full_screen = prim == 0x08 || index_count <= 6;  // rectangle list / a quad
      if (blends && !writes_depth && !full_screen && !(psg && psg->memexport) && !vsg.memexport) {
        QueueUpcomingShaders();
        ++stats_.cosmetic_deferred;
        SkipForShaders(true);
        return;
      }
    }
    WaitForShader(vsg);
    if (vsg.state == rr::ShaderState::kPending) {
      SkipForShaders(true);
      return;
    }
    if (vsg.state == rr::ShaderState::kFailed) {
      SkipForShaders(false);
      return;
    }
    const bool needs_ps = edram_mode == 4 && psg &&
                          (draw_colour || psg->kills || psg->writes_depth || ps.alpha_test.func != rr::CompareFunc::Always ||
                           ps.alpha_test.alpha_to_mask_enable);
    if (needs_ps) WaitForShader(*psg);
    if (needs_ps && psg->state != rr::ShaderState::kReady) {
      SkipForShaders(psg->state == rr::ShaderState::kPending);
      return;
    }
    if (needs_ps && psg->writes_depth && !psg->depth_remap_ready && float24_remap_ && target &&
        (target->key.storage_class & 1)) {
      Skip("depth-writing pixel shader without the D24FS8 remap hook");
      return;
    }
    // Textures (M2 texture cache) for the shaders that run: the vertex shader always, the pixel shader if bound.
    uint32_t t2d = vsg.texture_slots_2d, tcube = vsg.texture_slots_cube, t3d = vsg.texture_slots_3d;
    if (needs_ps) {
      t2d |= psg->texture_slots_2d;
      tcube |= psg->texture_slots_cube;
      t3d |= psg->texture_slots_3d;
    }
    uint32_t stand_ins = 0;
    PartScope textures_part(*this, kPartTextures);
    // Job 181: the same texture fetch constants as the draw before, in the same frame and release epoch, with no resolve
    // or other texture binding since: the backend still holds exactly these views - skip the lookups and the bind.
    bool tex_memo_hit = false;
    if (texture_memo_ && (t2d | tcube | t3d) && use_textures_ && tex_memo_valid_ && tex_memo_frame_ == frame_ &&
        tex_memo_epoch_ == release_epoch_ && tex_memo_gen_ == tex_gen_ && tex_memo_masks_[0] == t2d &&
        tex_memo_masks_[1] == tcube && tex_memo_masks_[2] == t3d) {
      tex_memo_hit = true;
      for (uint32_t m = t2d | tcube | t3d; m && tex_memo_hit; m &= m - 1) {
        const uint32_t i = uint32_t(std::countr_zero(m));
        tex_memo_hit = std::memcmp(&tex_memo_fetch_[6 * i], &r[kRegFetchConst + 6 * i], 24) == 0;
      }
    }
    uint32_t scaled_mask = 0;  // Stage 7: slots bound to textures at the resolution scale
    if (tex_memo_hit) {
      stand_ins = tex_memo_stand_ins_;
      scaled_mask = tex_memo_scaled_;
      ++stats_.texture_memo_hits;
    } else if ((t2d | tcube | t3d) && use_textures_) {
      gpu::TextureView views[96];
      gpu::SamplerDesc samplers[32];
      const uint64_t before = textures_->Stats().upload_bytes;
      // Gears 3 job 048: everything Bind records on the GPU (uploads, untiles) is "texture loads"; afterwards the
      // renderer's own category again (before, the texture cache's category 3 stayed on and the following draws were
      // counted as texture loads until another part changed the category).
      backend_->SetGpuCategory(3);
      stand_ins = textures_->Bind(&r[kRegFetchConst], t2d, tcube, t3d, frame_, views, samplers);
      backend_->SetGpuCategory(gpu_cat_ != 0xFF ? gpu_cat_ : 0);
      scaled_mask = textures_->LastScaledMask();
      stats_.texture_bytes += textures_->Stats().upload_bytes - before;
      // The table only needs to reach the highest slot the shaders read (t0-31 2D, t32-63 cube, t64-95 3D).
      const uint32_t count = t3d ? 64u + 32u - uint32_t(std::countl_zero(t3d))
                             : tcube ? 32u + 32u - uint32_t(std::countl_zero(tcube))
                                     : 32u - uint32_t(std::countl_zero(t2d));
      const uint32_t sampler_count = 32u - uint32_t(std::countl_zero(t2d | tcube | t3d));  // fetch slot = sampler
      backend_->SetTextures(views, samplers, count, sampler_count);
      if (report_on_ || StatsTexturesNow()) RepNoteTextures(views, samplers, count, sampler_count);  // 0101, 0124
      if (texture_memo_) {
        tex_memo_valid_ = true;
        tex_memo_frame_ = frame_;
        tex_memo_epoch_ = release_epoch_;
        tex_memo_gen_ = tex_gen_;
        tex_memo_masks_[0] = t2d;
        tex_memo_masks_[1] = tcube;
        tex_memo_masks_[2] = t3d;
        tex_memo_stand_ins_ = stand_ins;
        tex_memo_scaled_ = scaled_mask;
        for (uint32_t m = t2d | tcube | t3d; m; m &= m - 1) {
          const uint32_t i = uint32_t(std::countr_zero(m));
          std::memcpy(&tex_memo_fetch_[6 * i], &r[kRegFetchConst + 6 * i], 24);
        }
      }
    } else {
      tex_memo_valid_ = false;
      backend_->SetTextures(nullptr, nullptr);
      if (report_on_ || StatsTexturesNow()) RepNoteTextures(nullptr, nullptr, 0, 0);  // patch 0101, 0124
      if (t2d | tcube | t3d) stand_ins = uint32_t(std::popcount(t2d | tcube | t3d));  // texture cache switched off
    }
    if (stand_ins) ++stats_.texture_dependent;  // some slot read the white stand-in
    textures_part.Switch(kPartState);  // the pipeline description

    // Pipeline.
    gpu::GraphicsPipelineDesc d;
    d.vs = vsg.handle;
    d.ps = needs_ps ? psg->handle : 0;
    if (d.ps && vsg.dxbc != psg->dxbc) ++stats_.dxbc_mixed;
    bool restart = false;
    switch (ps.topology) {
      case rr::Topology::PointList: d.topology = gpu::Topology::kPointList; break;
      case rr::Topology::TriangleStrip: d.topology = gpu::Topology::kTriangleStrip; restart = true; break;
      case rr::Topology::RectListAsTriangles:
        d.topology = gpu::Topology::kTriangleList;
        d.gs = vsg.dxbc ? rect_gs_dxbc_ : rect_gs_;
        if (!d.gs) {
          SkipForShaders(false);
          return;
        }
        break;
      default: d.topology = gpu::Topology::kTriangleList; break;
    }
    const uint32_t mode_cntl = r[rr::reg::PA_SU_SC_MODE_CNTL];
    const bool reset_enabled = Bits(mode_cntl, 21, 1) != 0;  // multi_prim_ib_ena
    d.primitive_restart = restart && ib && reset_enabled;
    d.cull = ps.raster.cull_mode == rr::CullMode::Front  ? gpu::CullMode::kFront
             : ps.raster.cull_mode == rr::CullMode::Back ? gpu::CullMode::kBack
                                                         : gpu::CullMode::kNone;
    if (tess_n) d.cull = gpu::CullMode::kNone;  // our grid's winding is ours, not the tessellator's
    d.front_ccw = ps.raster.front_counter_clockwise;
    d.wireframe = ps.raster.fill_mode == rr::FillMode::Wireframe;
    d.depth_bias = ps.raster.depth_bias;
    d.depth_bias_slope = ps.raster.depth_bias_slope_scaled;
    d.depth_clip = ps.raster.depth_clip;
    d.depth_enable = ds.depth_enable;
    d.depth_write = ds.depth_write;
    d.depth_func = gpu::CompareFunc(uint8_t(ds.depth_func));
    d.stencil_enable = ds.stencil_enable;
    d.stencil_read_mask = ds.single_read_mask;
    d.stencil_write_mask = ds.single_write_mask;
    auto face = [](const rr::StencilFace& f) {
      gpu::StencilFace g;
      g.fail = gpu::StencilOp(uint8_t(f.fail_op));
      g.depth_fail = gpu::StencilOp(uint8_t(f.depth_fail_op));
      g.pass = gpu::StencilOp(uint8_t(f.pass_op));
      g.func = gpu::CompareFunc(uint8_t(f.func));
      return g;
    };
    d.stencil_front = face(ds.front);
    d.stencil_back = face(ds.back);
    // Colour outputs: slot i = the guest's colour target i (SV_Target i), with its blending and write mask.
    d.color_count = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!colour_rt[i] || !d.ps) continue;
      const rr::RenderTargetBlend& rb = ps.blend.rt[i];
      d.color_formats[i] = colour_rt[i]->format;
      d.color_write_masks[i] = rb.write_mask;
      d.color_blend[i] = HostBlend(rb);
      d.color_count = i + 1;
    }
    d.depth_format = target ? target->format : gpu::Format::kUnknown;
    // The diagnostic target: the first colour target drawn, else the depth target.
    d.samples = 1u << (target ? key.msaa : 0u);
    textures_part.Switch(kPartRecord);
    bool async_pso = REXCVAR_GET(gow2_renderer_async_pipelines);
    // Job 037: a per-frame budget for waiting on new pipelines (--gow2_renderer_pipeline_wait_ms). While this frame has
    // waited less than the budget, pipelines are created (or waited for) synchronously; past it, as before.
    const double pw = REXCVAR_GET(gow2_renderer_pipeline_wait_ms);  // patch 0047: negative = no cap
    const double wait_budget = async_pso ? (pw < 0.0 ? 1e9 : pw) : 0.0;
    const bool budget_sync = wait_budget > 0.0 && pipeline_wait_ms_frame_ < wait_budget;
    // A live budget waits for the asynchronous worker instead of compiling here.
    if (export_wait) async_pso = false;  // patch 0024: exporting draws create their pipeline synchronously
    const bool standin_sync = standin_wait && async_pso;  // patch 0045
    // Texture stand-ins also use a bounded asynchronous wait; never compile a speculative PSO here.
    const auto pso_t0 = budget_sync || standin_sync ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
    bool pipeline_pending = false;
    const uint64_t misses_before = backend_->PipelineMisses();  // patch 0058
    // Patch 0068: the first real DXBC scene draw gives the warm-only pipelines their vertex shader and state.
    if (prebuild_on_ && !warm_template_set_ && vsg.dxbc && d.ps && psg->dxbc && !d.gs && d.color_count &&
        d.color_formats[0] != gpu::Format::kUnknown && d.topology == gpu::Topology::kTriangleList)
      warm_template_set_ = backend_->SetWarmTemplate(d);
    gpu::PipelineHandle pipeline = backend_->GetGraphicsPipeline(d, async_pso, &pipeline_pending);
    if (pipeline_pending && budget_sync && backend_->WaitGraphicsPipeline(d, wait_budget - pipeline_wait_ms_frame_))
      pipeline = backend_->GetGraphicsPipeline(d, true, &pipeline_pending);
    if (pipeline_pending && standin_sync && !budget_sync &&
        backend_->WaitGraphicsPipeline(d, std::max(0.0, standin_wait_left_ms_)))
      pipeline = backend_->GetGraphicsPipeline(d, true, &pipeline_pending);
    // Patch 0132: a depth-only / shadow draw may wait briefly for its pipeline to come off a worker.
    if (pipeline_pending && colour_count == 0 && depth_bound && depth_wait_left_ms_ > 0.0) {
      const auto t0 = std::chrono::steady_clock::now();
      const bool got = backend_->WaitGraphicsPipeline(d, depth_wait_left_ms_);
      const double ms = MsSince(t0);
      depth_wait_left_ms_ -= ms;
      ++stats_.depth_pipeline_waits;
      stats_.depth_pipeline_wait_ms += ms;
      if (got) {
        pipeline = backend_->GetGraphicsPipeline(d, true, &pipeline_pending);
        if (!pipeline_pending) ++stats_.depth_pipeline_waits_won;
      }
    }
    if (backend_->PipelineMisses() != misses_before) {
      if (pipeline_list_) RecordPipeline(d, vs_hash, d.ps ? ps_hash : 0, tess_n);
      if (prebuild_on_ && d.ps) {  // patch 0068: was this new pipeline's pixel shader built ahead from the package?
        ++pb_.new_ps_pipelines;
        std::lock_guard<std::mutex> lock(prebuild_mutex_);  // v2: shared with the prebuild thread (new pipelines only)
        if (const auto q = prebuild_queued_hashes_.find(ps_hash); q != prebuild_queued_hashes_.end()) {
          ++pb_.new_ps_prebuilt;
          prebuild_variant_hits_[q->second & 1].fetch_add(1, std::memory_order_relaxed);
        }
        prebuild_known_ps_.insert(ps_hash);  // v2: drawn now - a later offer of it needs no warm-only build
      }
      if (!created_hashes_.empty()) {  // patch 0066: did this new pipeline's shaders come from the game's loading?
        ++stats_.new_draw_pipelines;
        if (created_hashes_.count(vs_hash)) ++stats_.new_vs_from_load;
        if (d.ps && created_hashes_.count(ps_hash)) ++stats_.new_ps_from_load;
      }
    }
    if (budget_sync) {
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pso_t0).count();
      pipeline_wait_ms_frame_ += ms;
      if (ms > 0.05) { ++stats_.pipeline_waits; stats_.pipeline_wait_ms += ms; }
    }
    if (standin_sync) {  // patch 0045
      const double ms = MsSince(pso_t0);
      standin_wait_left_ms_ -= ms;
      stats_.standin_wait_ms += ms;
    }
    if (pipeline_pending) {
      if (hold_descs_.size() < 4096 && hold_keys_.insert(gpu::HashPipelineDesc(d)).second)  // patch 0063
        hold_descs_.push_back(d);
      SkipForShaders(true);
      return;
    }
    if (!pipeline) {
      TraceDraw("skipped: pipeline failed");  // patch 0016
      ++stats_.pipeline_failures;
      ++pipeline_failures_this_frame_;
      return;
    }

    if (ProbeFrame()) NoteProbeDraw(r, vsg, ps, target, depth_writes);
    // Constants.
    textures_part.Switch(kPartConstants);
    rr::ViewportState v = ps.viewport;
    rr::NdcForBottomLeftOrigin(v);  // D3D12: +Y up in NDC
    if (up_active_ && !up_scene_done_) UpscaleJitter(r, vsg, ps, target, depth_writes, v);  // notes/64 (0145: or inputs)
    else if (up_active_) UpscaleLateDraw(r, ps, target, v);  // Gears 3 job 120
    const bool remap24 = float24_remap_ && target && (target->key.storage_class & 1);
    if (needs_ps && psg->writes_depth) {
      v.z_min = 0.0f;  // the shader's depth output is halved instead (system flag 64)
      v.z_max = 1.0f;
    } else if (remap24) {
      v.z_min *= 0.5f;  // exact: guest [0, 1] -> host [0, 0.5]
      v.z_max *= 0.5f;
    }
    SystemConstants sc{};
    sc.sys0[0] = (v.w_not_reciprocal ? 1u : 0u) | (v.xy_divided_by_w ? 2u : 0u) | (v.z_divided_by_w ? 4u : 0u) |
                 (ps.raster.primitive_polygonal ? 8u : 0u) |
                 (ps.alpha_test.func != rr::CompareFunc::Always ? 32u : 0u) | (remap24 ? 64u : 0u) |
                 (memexport_ ? 128u : 0u);  // notes/11: exports allowed
    sc.sys0[1] = r[kRegVgtIndxOffset];
    sc.sys2[0] = r[kRegVgtMinVtxIndx] & 0xFFFFFFu;  // patch 0028
    sc.sys2[1] = r[kRegVgtMaxVtxIndx] & 0xFFFFFFu;
    if (!sc.sys2[1]) sc.sys2[1] = 0xFFFFFFu;  // deviation: a never-written max (0) would clamp every index to 0
    if (psg) {
      // Xenia Shader::GetInterpolatorInputMask: registers below max(static bound, dynamic count), capped at 16, minus
      // the parameter-generation register.
      const uint32_t program_cntl = r[rr::reg::SQ_PROGRAM_CNTL];
      uint32_t count = psg->register_bound;
      if (psg->dynamic_addressing) count = std::max(count, Bits(program_cntl, 8, 6) + 1);
      count = std::min(count, 16u);
      uint32_t mask = count >= 32 ? ~0u : (1u << count) - 1u;
      const uint32_t gen_pos = Bits(r[kRegSqContextMisc], 8, 8);
      if (Bits(program_cntl, 18, 1) && gen_pos < count) {
        mask &= ~(1u << gen_pos);
        sc.sys0[3] = 0x80000000u | gen_pos;
      }
      sc.sys0[2] = mask;
    }
    for (uint32_t i = 0; i < 3; ++i) {
      sc.ndc_scale[i] = v.ndc_scale[i];
      sc.ndc_offset[i] = v.ndc_offset[i];
    }
    // Stage 7: the scale of scaled textures (texel maths of the slots in ndc_offset.w); the draw's own pixel scale
    // (param gen's guest pixel position) is in system flags w bits 8-11 - they differ for a 1x target (notes/53b).
    sc.ndc_scale[3] = float(res_scale_);
    sc.ndc_offset[3] = std::bit_cast<float>(scaled_mask);  // Stage 7: slots whose texture is at the scale
    sc.sys0[3] |= std::min(draw_scale, 15u) << 8;
    // 2^RB_COLOR_INFO.color_exp_bias per target (exact powers of two, as the reference builds them from the bits).
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t ci = r[rr::reg::kColorInfo[i]];
      int32_t bias = SignExtend(Bits(ci, 20, 6), 6);
      // 16-bit fixed point targets: -32...32 mapped to the host SNORM -1...1 (SDK command_processor.cpp 4013-4021).
      float fixed16_scale = 1.0f;
      if (IsFixed16Class(rr::ColorStorageClass(Bits(ci, 16, 4)))) {
        // Patch 0023 (Canary 42c2b88): with Canary's resolve rules the host SNORM holds guest x 1024/32767 (10
        // fractional bits, as the hardware); else x 1/32 as the SDK.
        if (canary_resolve_rules_) fixed16_scale = 1024.0f / 32767.0f;
        else bias -= 5;
        if (colour_rt[i] && ps.blend.rt[i].blend_enable && MultipliesByColour(ps.blend.rt[i]))
          ++stats_.fixed16_multiplicative_blends;  // the only blends the 1/32 scaling makes wrong (notes/45 2)
      }
      sc.color_exp_scale[i] = std::ldexp(1.0f, bias) * fixed16_scale;
    }
    sc.sys1[0] = uint32_t(ps.alpha_test.func);
    // Patch 0027 (Canary dxbc_shader_translator_om.cc, Gears 3 review 04-x): the alpha test only applies when the pixel
    // shader writes oC0; a depth-only shader with leftover alpha-test state would otherwise discard every pixel.
    if (alpha_test_rt0_ && psg && !(psg->color_targets_written & 1u)) sc.sys1[0] = uint32_t(rr::CompareFunc::Always);
    sc.sys1[1] = std::bit_cast<uint32_t>(ps.alpha_test.ref);
    // Job 177: system and bool/loop constants are the same for both stages - one compare and upload for both.
    if (d.ps) {
      backend_->SetConstantsBoth(gpu::kCbSystem, &sc, sizeof(sc));
      backend_->SetConstantsBoth(gpu::kCbBoolLoop, &r[kRegBoolConst], 40 * 4);
    } else {
      backend_->SetConstants(gpu::ShaderStage::kVertex, gpu::kCbSystem, &sc, sizeof(sc));
      backend_->SetConstants(gpu::ShaderStage::kVertex, gpu::kCbBoolLoop, &r[kRegBoolConst], 40 * 4);
    }
    // Only the float constants the shader can read (job 166: the full 4 KB per stage per draw was ~6% of the GPU thread
    // in compares and copies); a relative read anywhere means all 256.
    // Job 250+: a compacted shader (gow2_renderer_compact_constants) reads c[0, low) then c[high, n) back to back; its
    // layout is fixed by its HLSL, so it is uploaded that way whatever the trim switch says.
    // Job 240: skip the compare + copy when no register write reached that range since this slot's last upload (the
    // render pipe's generation counters) and the layout is the same - the backend slot still holds exactly that data.
    const XenosConstGenerations gens = const_gen_skip_ ? XenosConstGens() : XenosConstGenerations{};
    const auto upload_floats = [&](gpu::ShaderStage stage, uint32_t base, const rr::GuestShader* sh, bool trim,
                                   uint64_t gen, uint64_t& gen_seen, uint32_t& layout_seen) {
      uint32_t lo = 256, hi = 256, n = 256;
      if (sh && sh->compacted()) {
        lo = sh->float_constants_low, hi = sh->float_constants_high, n = sh->float_constants;
      } else if (sh && trim) {
        lo = hi = n = sh->float_constants;
      }
      const uint32_t layout = lo | (hi << 9) | (n << 18);  // each <= 256
      if (gens.valid && gen == gen_seen && layout == layout_seen) {
        ++stats_.const_gen_skips;
        return;
      }
      if (lo == hi) {
        backend_->SetConstants(stage, gpu::kCbFloat, &r[base], n * 16);
      } else {
        backend_->SetConstantsSplit(stage, gpu::kCbFloat, &r[base], lo * 16, &r[base + 4 * hi], (n - hi) * 16);
        ++stats_.compacted_uploads;
        stats_.compacted_bytes_saved += uint64_t(hi - lo) * 16;
      }
      gen_seen = gens.valid ? gen : ~0ull;
      layout_seen = layout;
    };
    upload_floats(gpu::ShaderStage::kVertex, kRegFloatConstVs, &vsg, float_constants_trim_, gens.vs, vs_gen_seen_,
                  vs_size_seen_);
    if (d.ps) {
      upload_floats(gpu::ShaderStage::kPixel, kRegFloatConstPs, needs_ps ? psg : nullptr, float_constants_trim_,
                    gens.ps, ps_gen_seen_, ps_size_seen_);
    }

    // Vertex data the shader fetches (guest memory mirror t96, or the upload ring t97 for small ranges), then the
    // fetch constants as the shaders must see them.
    textures_part.Switch(kPartVertex);
    std::memcpy(fetch_, &r[kRegFetchConst], 192 * 4);
    UploadVertexData(r, vsg);
    if (log_vs_hash_ && vs_.hash == log_vs_hash_ && log_vs_logged_ <= log_vs_max_)  // patch 0015 v2: the draw's state
      REXLOG_INFO("gow2_renderer: vs-log frame {} draw {}: prim {} count {} | viewport {} {} {} {} z {} {} scale {} {} "
                  "offset {} {} xy_div_w {} clip_disabled {} | scissor {},{} {}x{} | alpha test func {}",
                  frame_ + 1, draw_index_, prim, index_count, ps.viewport.x, ps.viewport.y, ps.viewport.width,
                  ps.viewport.height, ps.viewport.z_min, ps.viewport.z_max, ps.viewport.ndc_scale[0],
                  ps.viewport.ndc_scale[1], ps.viewport.ndc_offset[0], ps.viewport.ndc_offset[1],
                  ps.viewport.xy_divided_by_w ? 1 : 0, ps.viewport.clip_disabled ? 1 : 0, ps.scissor.x, ps.scissor.y,
                  ps.scissor.width, ps.scissor.height, int(ps.alpha_test.func));
    if (needs_ps && (psg->vfetch_constants[0] | psg->vfetch_constants[1] | psg->vfetch_constants[2]))
      UploadVertexData(r, *psg);
    textures_part.Switch(kPartConstants);
    if (d.ps)
      backend_->SetConstantsBoth(gpu::kCbFetch, fetch_, FetchUploadBytes());
    else
      backend_->SetConstants(gpu::ShaderStage::kVertex, gpu::kCbFetch, fetch_, FetchUploadBytes());

    // Indices.
    textures_part.Switch(kPartIndex);
    indices_.clear();
    index_ptr_ = &indices_;
    cached_ib_ = nullptr;
    bool indexed = false;
    // Job 178: small index ranges drawn again in the same command list and release epoch reuse their converted ring
    // copy (same rule as vertex reuse; converted fans / quad lists excluded).
    uint64_t idx_key = 0;
    bool idx_hit = false, idx_keep = false;
    uint64_t idx_off = 0, idx_list = 0;
    const uint32_t reset_index = reset_enabled ? r[kRegVgtMultiPrimIbResetIndx] : ~0u;
    const bool use_restart = reset_enabled && restart;
    if (ib && index_reuse_ && prim != 0x05 && prim != 0x0D) {
      const bool is32 = ib->format == gx::IndexFormat::kInt32;
      const uint64_t bytes = uint64_t(index_count) * (is32 ? 4 : 2);
      if (!(upload_watch_ && bytes >= kWatchMinBytes)) {
        const uint64_t list = backend_->Stats().submits;
        if (list != idx_reuse_list_ || release_epoch_ != idx_reuse_epoch_) {
          idx_reuse_.clear();
          idx_reuse_list_ = list;
          idx_reuse_epoch_ = release_epoch_;
        }
        idx_key = (uint64_t(ib->guest_base & 0x1FFFFFFFu) << 32) ^ index_count ^
                  ((uint64_t(is32) | uint64_t(ib->endianness) << 1 | uint64_t(use_restart) << 3) << 24) ^
                  uint64_t(use_restart ? reset_index : 0) * 0x9E3779B97F4A7C15ull;
        idx_keep = true;
        if (auto it = idx_reuse_.find(idx_key); it != idx_reuse_.end()) {
          idx_hit = true;
          idx_off = it->second;
          idx_list = list;
        }
      }
    }
    if (ib) {
      if (!idx_hit && !ReadIndices(*ib, index_count, reset_index, use_restart)) {
        TraceDraw("skipped: index read failed");  // patch 0016
        ++stats_.other_failures;
        return;
      }
      if (idx_hit) stats_.index_bytes += uint64_t(index_count) * (ib->format == gx::IndexFormat::kInt32 ? 4 : 2);
      indexed = true;
    }
    if (prim == 0x05 || prim == 0x0D) {  // fan -> list, quad list -> triangles (Canary's GS order, review of n092)
      if (!indexed) {
        indices_.resize(index_count);
        for (uint32_t i = 0; i < index_count; ++i) indices_[i] = i;
        index_ptr_ = &indices_;
        indexed = true;
      }
      converted_.clear();
      const std::vector<uint32_t>& in_idx = *index_ptr_;
      const uint32_t n = uint32_t(in_idx.size());
      if (prim == 0x05) {
        converted_.resize(::xenos::prim::TriangleFanListCount(n));
        ::xenos::prim::TriangleFanToList(converted_.data(), in_idx.data(), n);
      } else {
        converted_.resize(::xenos::prim::QuadListTriangleListCount(n));
        ::xenos::prim::QuadListToTriangleListGsOrder(converted_.data(), in_idx.data(), n);
      }
      indices_.swap(converted_);
      index_ptr_ = &indices_;
      cached_ib_ = nullptr;  // converted: not the cached buffer
    }

    // Draw, then repeat a clear on the other targets that share its tiles.
    textures_part.Switch(kPartRecord);
    backend_->SetPipeline(pipeline);
    backend_->SetStencilRef(ds.single_ref);
    if (draw_colour) backend_->SetBlendConstant(ps.blend.constant);
    // depth: the depth target to bind (null: none); repeats of a clear onto aliases pass their own target.
    auto issue = [&](rr::RenderTarget* depth, int32_t dy) {
      gpu::TextureHandle rts[gpu::GraphicsPipelineDesc::kMaxColor] = {};
      for (uint32_t i = 0; i < 4; ++i) rts[i] = colour_rt[i] ? colour_rt[i]->texture : 0;
      if (stats_frame_ && frame_ == uint64_t(stats_frame_)) {  // patch 0121
        std::vector<const rr::RenderTarget*> now;
        for (uint32_t i = 0; i < 4; ++i) if (colour_rt[i]) now.push_back(colour_rt[i]);
        // Patch 0122: after each of the first N draws too (the previous draw's targets, before this one is issued).
        // Patch 0123: the readouts can start later (each_from) and be spaced out (each_step): after draw
        // from + k*step for k = 1..N. Draws with no colour target are counted too (0123 logs how many).
        const uint32_t d = stats_draws_, from = uint32_t(stats_from_), step = uint32_t(stats_step_);
        const bool each = stats_each_ > 0 && d > from && (d - from) % step == 0 && (d - from) / step <= uint32_t(stats_each_);
        if (now != stats_prev_ || each) {
          TargetStats(stats_prev_);
          if (stats_textures_) SampledStats();  // patch 0124: the previous draw's sampled textures
        }
        stats_prev_ = now;
        if (stats_textures_) {  // patch 0124: this draw's textures, read at the next readout
          // Patch 0125: only the 2D slots (t0-31). 0124 copied all of rep_view_count_ (up to 96, with the cube and 3D
          // slots) into this 32-entry array and wrote past its end (Gears 2 job 434: crash in the stats frame).
          stats_view_count_ = std::min<uint32_t>(rep_view_count_, 32);
          for (uint32_t i = 0; i < stats_view_count_; ++i) stats_views_[i] = rep_views_[i];
        }
        if (now.empty()) ++stats_no_colour_;
        ++stats_draws_;
      }
      backend_->SetRenderTargets(d.color_count, rts, depth ? depth->texture : 0);
      if (gpu_by_target_) NoteDrawTarget(colour_rt[0] ? colour_rt[0] : depth);
      if (report_on_)  // patch 0101: this draw's own GPU timestamp and its description
        RepTag(true, RepDrawText(d, colour_rt, depth, vs_hash, ps_hash, index_count, v.width, v.height,
                                 uint32_t(ps.scissor.width), uint32_t(ps.scissor.height)));
      backend_->SetViewport({v.x, v.y + float(dy), v.width, v.height, v.z_min, v.z_max});
      backend_->SetScissor({int32_t(ps.scissor.x), int32_t(ps.scissor.y) + dy,
                            int32_t(ps.scissor.x + ps.scissor.width), int32_t(ps.scissor.y + ps.scissor.height) + dy});
      if (indexed && cached_ib_ && static_index_buffers_) {
        if (!cached_ib_->gpu) cached_ib_->gpu = backend_->CreateIndexBuffer(cached_ib_->indices.data(),
                                                                          uint32_t(cached_ib_->indices.size()));
        if (cached_ib_->gpu) {
          backend_->DrawIndexedBuffer(cached_ib_->gpu, uint32_t(cached_ib_->indices.size()));
        } else {
          backend_->DrawIndexed(index_ptr_->data(), uint32_t(index_ptr_->size()));
        }
      } else if (indexed && idx_hit) {
        if (!backend_->DrawIndexedAt(idx_off, index_count, idx_list)) {  // a new list began: convert after all
          idx_hit = false;
          if (ReadIndices(*ib, index_count, reset_index, use_restart))
            backend_->DrawIndexed(index_ptr_->data(), uint32_t(index_ptr_->size()));
        }
      } else if (indexed) {
        const uint64_t off = backend_->DrawIndexedKeep(index_ptr_->data(), uint32_t(index_ptr_->size()));
        if (idx_keep && off != UINT64_MAX && !cached_ib_) {
          if (backend_->Stats().submits != idx_reuse_list_) {
            idx_reuse_.clear();
            idx_reuse_list_ = backend_->Stats().submits;
          }
          idx_reuse_[idx_key] = off;
          idx_hit = true;  // repeats of this draw (aliases) use it too
          idx_off = off;
          idx_list = idx_reuse_list_;
        }
      } else {
        backend_->Draw(index_count);
      }
      if (depth && depth_writes) Wrote(*depth);  // job 182
      for (rr::RenderTarget* c : colour_rt)
        if (c) Wrote(*c);
    };
    // An exporting shader reads and writes the guest-memory buffer through u0, so it needs the UAV state even with
    // the switch off (then system flag 128 is clear and it writes nothing).
    const bool export_shader = vsg.memexport || (needs_ps && psg->memexport);
    const bool exports = memexport_ && export_shader;
    if (exports && REXCVAR_GET(gow2_renderer_export_prepare_mirror)) MarkExports(r, vsg, true);  // patch 0024
    if (export_shader) backend_->SetGuestWritable(true);
    issue(target, 0);
    if (export_shader) backend_->SetGuestWritable(false);
    if (exports) {
      ++stats_.memexport_draws;
      MarkExports(r, vsg);
      if (needs_ps && psg->memexport) ++stats_.memexport_ps_draws;
    }
    // Gears 3 (job 028, Gears 2 chat's diagnosis): the fast clear's half-pitch 4x MSAA surface is redirected to the 1x
    // target at pitch x2 - right for Gears 2, where that 4x surface is only a stand-in. Gears 3 also has REAL 4x targets
    // at that key (the 640x360 7e3 translucency target, base 720 pitch 640), which then never got cleared and kept
    // last frame's contents. Clear such a real 4x twin too, with the clear's constant colour (colour only; a real 4x
    // depth twin is only logged for now).
    if (is_clear && msaa && REXCVAR_GET(gow2_renderer_clear_msaa_twins)) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (!ps.blend.rt[i].write_mask || !((ps_colour >> i) & 1)) continue;
        const uint32_t ci = r[rr::reg::kColorInfo[i]];
        const rr::RenderTargetKey k4{Bits(ci, 0, 12) & 2047u, rr::ColorStorageClass(Bits(ci, 16, 4)), pitch, msaa};
        rr::RenderTarget* twin = rts_->FindAny(k4);  // patch 0141 review: either phase's twin
        if (!twin || !twin->texture || !twin->key.msaa) continue;
        if (i != 0 || !psg || psg->constant_color0 < 0) continue;
        float c[4];
        std::memcpy(c, &r[kRegFloatConstPs + 4 * uint32_t(psg->constant_color0)], 16);
        backend_->ClearColor(twin->texture, c);
        Wrote(*twin);
        ++stats_.msaa_twin_clears;
        if (msaa_twin_logged_.insert(uint64_t(k4.base_tiles) << 32 | uint64_t(k4.storage_class) << 16 | k4.pitch_pixels).second)
          REXLOG_INFO("gow2_renderer: fast clear frame {} draw {}: also cleared the real 4x MSAA target {} base {} pitch {} "
                      "to ({}, {}, {}, {})", frame_ + 1, draw_index_, rr::StorageClassName(k4.storage_class), k4.base_tiles,
                      k4.pitch_pixels, c[0], c[1], c[2], c[3]);
      }
      const rr::RenderTargetKey d4{key.base_tiles, key.storage_class, pitch, msaa};
      if (depth_bound && depth_writes)
        if (rr::RenderTarget* dt = rts_->Find(d4); dt && dt->texture && !msaa_twin_logged_.count(~uint64_t(d4.base_tiles))) {
          msaa_twin_logged_.insert(~uint64_t(d4.base_tiles));
          REXLOG_INFO("gow2_renderer: fast clear frame {}: a real 4x MSAA DEPTH target base {} pitch {} exists - not "
                      "cleared (not done yet)", frame_ + 1, d4.base_tiles, d4.pitch_pixels);
        }
    }
    if (trace_on_ && target) {
      char b[200];
      std::snprintf(b, sizeof(b), "drawn into it (rect %u,%u-%u,%u; depth write %d func %u; stencil write %d mask %02X ref "
                    "%02X func %u pass op %u)", own.drawn.x0, own.drawn.y0, own.drawn.x1, own.drawn.y1,
                    ds.depth_write ? 1 : 0, uint32_t(ds.depth_func), stencil_writes ? 1 : 0, ds.single_write_mask,
                    ds.single_ref, uint32_t(ds.front.func), uint32_t(ds.front.pass_op));
      TraceEvent(*target, own.drawn, b);
    }
    TraceDraw("DRAWN");  // patch 0016
    ++stats_.drawn;
    ++drawn_since_copy_;
    if (trace_on_)
      for (uint32_t i = 0; i < 4; ++i)
        if (colour_rt[i])
          TraceEvent(*colour_rt[i], own.drawn,
                     "colour drawn into it (slot " + std::to_string(i) + ", mask " +
                         std::to_string(ps.blend.rt[i].write_mask) + (ps.blend.rt[i].blend_enable ? ", blended)" : ")"));
    if (target) ++target->draws;
    for (rr::RenderTarget* c : colour_rt)
      if (c) ++c->draws;
    if (draw_colour) ++stats_.colour_drawn;
    if (is_clear && target && !draw_colour) {
      ++stats_.clears;
      uint32_t c0, c1, r0, r1;
      rr::RenderTargetCache::TileBounds(*target, own.drawn, c0, c1, r0, r1);
      const uint32_t tiles = std::min<uint32_t>(r1 * target->pitch_tiles, ::xenos::edram::kTileCount);
      rts_->ForEachOverlap(target->key.base_tiles, tiles, target, [&](rr::RenderTarget& other) {
        if (other.key.storage_class != target->key.storage_class || other.pitch_tiles != target->pitch_tiles ||
            other.key.msaa != 0 || !other.texture || other.format != target->format ||
            (other.key == target->key && other.scale != target->scale))  // patch 0141: not the other-scale twin
          return;
        // Same layout, different base: whole tile rows apart -> a vertical offset in pixels.
        const int32_t delta = int32_t(target->key.base_tiles) - int32_t(other.key.base_tiles);
        if (delta % int32_t(target->pitch_tiles)) return;
        issue(&other, delta / int32_t(target->pitch_tiles) * int32_t(::xenos::edram::kTileHeightSamples) *
                          int32_t(target->scale));
        ++stats_.clear_copies;
      });
    }
  }


  // What was not drawn since the previous copy, from the difference of two stats snapshots.
  static std::string NotDrawn(const FrameStats& now, const FrameStats& base) {
    std::string out;
    auto add = [&](const char* what, uint64_t a, uint64_t b) {
      if (a > b) out += (out.empty() ? "" : ", ") + std::string(what) + " " + std::to_string(a - b);
    };
    add("colour-only", now.colour_only, base.colour_only);
    add("dummy points", now.dummy_points, base.dummy_points);
    add("patches", now.patches, base.patches);
    add("pending shaders", now.pending_shaders, base.pending_shaders);
    add("shader errors", now.shader_errors, base.shader_errors);
    add("pipeline errors", now.pipeline_failures, base.pipeline_failures);
    add("other", now.other_failures, base.other_failures);
    add("empty scissor", now.empty, base.empty);
    for (const auto& [why, n] : now.skipped) {
      auto it = base.skipped.find(why);
      add(why.c_str(), n, it == base.skipped.end() ? 0 : it->second);
    }
    if (out.empty()) out = "nothing";
    char cost[200];
    std::snprintf(cost, sizeof(cost), " | this pass: uploads vertex %.2f MB (%.2f MB unchanged, skipped) index %.2f MB "
                  "textures %.2f MB, our CPU %.2f ms",
                  double(now.vertex_bytes - base.vertex_bytes) / 1048576.0,
                  double(now.vertex_skipped_bytes - base.vertex_skipped_bytes) / 1048576.0,
                  double(now.index_bytes - base.index_bytes) / 1048576.0,
                  double(now.texture_bytes - base.texture_bytes) / 1048576.0, now.cpu_ms - base.cpu_ms);
    char tiles[320];
    std::snprintf(tiles, sizeof(tiles),
                  " | alias tiles: %llu transferred (%llu carrying colour-derived content, %llu transfer runs), %llu "
                  "from colour clears, %llu unknown source (%llu of them colour), %llu where the ownership rules disagree",
                  static_cast<unsigned long long>(now.transferred_tiles - base.transferred_tiles),
                  static_cast<unsigned long long>(now.tainted_tiles - base.tainted_tiles),
                  static_cast<unsigned long long>(now.transfer_passes - base.transfer_passes),
                  static_cast<unsigned long long>(now.cleared_tiles - base.cleared_tiles),
                  static_cast<unsigned long long>(now.unknown_tiles - base.unknown_tiles),
                  static_cast<unsigned long long>(now.unknown_colour_tiles - base.unknown_colour_tiles),
                  static_cast<unsigned long long>(now.disagree_tiles - base.disagree_tiles));
    return out + cost + tiles;
  }

  // EDRAM bits of a constant colour in a 32bpp colour format (for colour clears seen later through a depth target).
  static bool ColorClearBits(uint32_t format, const float c[4], uint32_t& bits) {
    auto unorm = [](float v, uint32_t max) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * float(max))); };
    auto half = [](float f) {
      const uint32_t x = std::bit_cast<uint32_t>(f);
      const uint32_t sign = (x >> 16) & 0x8000;
      const int32_t e = int32_t((x >> 23) & 0xFF) - 112;
      if (((x >> 23) & 0xFF) == 0 || e <= 0) return sign;  // zero / denormal -> signed zero (good enough for clears)
      if (e >= 31) return sign | 0x7C00u;
      return sign | (uint32_t(e) << 10) | ((x >> 13) & 0x3FF);
    };
    if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f) {
      bits = 0;  // zero is zero bits in every format
      return true;
    }
    switch (format) {
      case 0: case 1:  // k_8_8_8_8 (and _GAMMA: the clear value is written as given)
        bits = unorm(c[0], 255) | unorm(c[1], 255) << 8 | unorm(c[2], 255) << 16 | unorm(c[3], 255) << 24;
        return true;
      case 2: case 10:  // k_2_10_10_10
        bits = unorm(c[0], 1023) | unorm(c[1], 1023) << 10 | unorm(c[2], 1023) << 20 | unorm(c[3], 3) << 30;
        return true;
      case 6:  // k_16_16_FLOAT
        bits = half(c[0]) | half(c[1]) << 16;
        return true;
      case 14:  // k_32_FLOAT
        bits = std::bit_cast<uint32_t>(c[0]);
        return true;
      default:
        return false;  // 7e3, 16-bit fixed point and 64bpp: not modelled
    }
  }

  // Which tiles one access to a target touches under each ownership book (upl-n100). Book 0 is the rule we act on
  // (--gow2_renderer_ownership), book 1 the other one, kept only to label pixels where the two disagree.
  struct Ownership {
    rr::PixelRect rect[2];
    uint32_t limit[2] = {::xenos::edram::kTileCount, ::xenos::edram::kTileCount};
    bool active[2] = {true, true};  // this book changes ownership for the access
    rr::PixelRect drawn;            // what the access can really write: scissor ∩ viewport
  };

  // Scissor ∩ viewport in host pixels (a clip-disabled draw has no viewport bound: scissor only). Primitive bounds
  // would narrow it further but need the vertex shader's output (not known before the draw).
  static rr::PixelRect DrawnRect(const rr::PipelineState& ps) {
    rr::PixelRect r{ps.scissor.x, ps.scissor.y, ps.scissor.x + ps.scissor.width, ps.scissor.y + ps.scissor.height};
    if (!ps.viewport.clip_disabled) {
      const auto lo = [](float v) { return uint32_t(std::clamp(std::floor(v), 0.0f, 16384.0f)); };
      const auto hi = [](float v) { return uint32_t(std::clamp(std::ceil(v), 0.0f, 16384.0f)); };
      r.x0 = std::max(r.x0, lo(ps.viewport.x));
      r.y0 = std::max(r.y0, lo(ps.viewport.y));
      r.x1 = std::min(r.x1, hi(ps.viewport.x + ps.viewport.width));
      r.y1 = std::min(r.y1, hi(ps.viewport.y + ps.viewport.height));
    }
    return r;
  }

  // Canary (render_target_cache.cc Update, draw_extent_estimator.cc): whole tile rows from the base down to the
  // scissor bottom, limited by the viewport bottom for clipped draws. Canary runs the vertex shader on the CPU for
  // clip-disabled draws with an unlimited scissor; we use the scissor there (noted in notes/44 section 11).
  static rr::PixelRect CanaryRect(const rr::PipelineState& ps) {
    uint32_t y1 = ps.scissor.y + ps.scissor.height;
    if (!ps.viewport.clip_disabled)
      y1 = std::min<uint32_t>(y1, uint32_t(std::clamp(ps.viewport.y + ps.viewport.height, 0.0f, 16384.0f)));
    return {0, 0, 0xFFFFFFFFu, y1};
  }

  // Canary clamps a bound target's range so that it ends at the next bound target's base (modulo 2048).
  static uint32_t CanaryLimit(uint32_t base, const uint32_t* bases, uint32_t n) {
    uint32_t limit = ::xenos::edram::kTileCount;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t d = (bases[i] + ::xenos::edram::kTileCount - base) % ::xenos::edram::kTileCount;
      if (d) limit = std::min(limit, d);
    }
    return limit;
  }

  Ownership OwnershipFor(const rr::PipelineState& ps, bool writes, bool canary_bound, uint32_t canary_limit) const {
    Ownership o;
    o.drawn = DrawnRect(ps);
    if (fill_geom_.valid) {  // job 188: the fill shader's real bounds
      o.drawn.x0 = std::max(o.drawn.x0, fill_geom_.rect.x0);
      o.drawn.y0 = std::max(o.drawn.y0, fill_geom_.rect.y0);
      o.drawn.x1 = std::min(o.drawn.x1, fill_geom_.rect.x1);
      o.drawn.y1 = std::min(o.drawn.y1, fill_geom_.rect.y1);
    }
    const uint32_t rect_book = canary_ownership_ ? 1u : 0u, canary_book = rect_book ^ 1u;
    o.rect[rect_book] = o.drawn;
    o.active[rect_book] = writes && !o.drawn.empty();
    // A draw cannot sensibly write into the EDRAM of another target bound with it, so its range also ends at the next
    // bound target's base (upl-n107; the reference does this, and without primitive bounds an unclipped draw with the
    // 8192 scissor otherwise takes the whole EDRAM - job 109 copy 16).
    if (clamp_next_base_) o.limit[rect_book] = canary_limit;
    o.rect[canary_book] = CanaryRect(ps);
    o.active[canary_book] = canary_bound && !o.rect[canary_book].empty();
    o.limit[canary_book] = canary_limit;
    if (!alt_active_) o.active[1] = false;  // book 1 is the alternate one
    return o;
  }

  // A resolve (read, take_ownership false) or resolve clear: the same rectangle under both rules.
  Ownership OwnershipForRect(const rr::PixelRect& rect) const {
    Ownership o;
    o.rect[0] = o.rect[1] = o.drawn = rect;
    o.active[1] = alt_active_;
    return o;
  }

  // Runs of tiles with the same source, merged along tile rows and then down columns of equal width (a full claim
  // becomes one rectangle per source instead of one per tile row: upl-n102).
  struct Member {
    uint32_t offset;
    uint8_t taint;
  };
  struct Run {
    const rr::RenderTarget* owner;
    bool known;
    uint32_t bits;
    gpu::Rect rect;
    std::vector<Member> members;
  };
  // Scratch for BuildRuns, kept between draws so the runs and their member lists reuse their memory (job 164: the
  // per-draw vectors' allocations were ~25% of the GPU thread in ch02 uncapped).
  struct RunScratch {
    std::vector<Run> rows, runs;
  };
  std::span<const Run> BuildRuns(const rr::RenderTarget& t, const std::vector<rr::TileSource>& sources,
                                 bool overwrites_all, const rr::PixelRect& drawn, RunScratch& sc) {
    // Runs of tiles with the same source, merged along tile rows and then down columns of equal width (a full claim
    // becomes one rectangle per source instead of one per tile row: upl-n102).
    size_t nrows = 0;
    for (const rr::TileSource& ts : sources) {
      const rr::PixelRect pr = rr::RenderTargetCache::TilePixels(t, ts.offset);
      if (pr.empty()) continue;
      // Tiles the access replaces completely (depth and all of stencil, exact rectangle: resolve clears) keep nothing.
      if (overwrites_all && pr.x0 >= drawn.x0 && pr.x1 <= drawn.x1 && pr.y0 >= drawn.y0 && pr.y1 <= drawn.y1) {
        if (trace_on_)
          TraceEvent(t, pr, "no transfer from " + KeyText(ts.owner) + ": the access replaces depth and stencil there");
        continue;
      }
      const gpu::Rect rect{int32_t(pr.x0), int32_t(pr.y0), int32_t(pr.x1), int32_t(pr.y1)};
      if (nrows) {
        Run& last = sc.rows[nrows - 1];
        if (last.owner == ts.owner && last.known == ts.clear_known && last.bits == ts.clear_bits &&
            last.rect.y0 == rect.y0 && last.rect.y1 == rect.y1 && last.rect.x1 == rect.x0) {
          last.rect.x1 = rect.x1;  // extend along the tile row
          last.members.push_back({ts.offset, ts.taint});
          continue;
        }
      }
      if (nrows == sc.rows.size()) sc.rows.emplace_back();
      Run& row = sc.rows[nrows++];
      row.owner = ts.owner;
      row.known = ts.clear_known;
      row.bits = ts.clear_bits;
      row.rect = rect;
      row.members.clear();
      row.members.push_back({ts.offset, ts.taint});
    }
    size_t nruns = 0;
    for (size_t k = 0; k < nrows; ++k) {
      const Run& r = sc.rows[k];
      bool merged = false;
      for (size_t i = nruns; i-- > 0 && !merged;) {
        Run& m = sc.runs[i];
        if (m.owner == r.owner && m.known == r.known && m.bits == r.bits && m.rect.x0 == r.rect.x0 &&
            m.rect.x1 == r.rect.x1 && m.rect.y1 == r.rect.y0) {
          m.rect.y1 = r.rect.y1;
          m.members.insert(m.members.end(), r.members.begin(), r.members.end());
          merged = true;
        }
        if (nruns - i > 8) break;  // only look back a few runs
      }
      if (merged) continue;
      if (nruns == sc.runs.size()) sc.runs.emplace_back();
      Run& n = sc.runs[nruns++];
      n.owner = r.owner;
      n.known = r.known;
      n.bits = r.bits;
      n.rect = r.rect;
      n.members.assign(r.members.begin(), r.members.end());
    }
    return {sc.runs.data(), nruns};
  }

  // upl-n103's colour maths (all of xenos/renderer/colour_hlsl.h) + our host<->guest word helpers + a shader body.
  static std::string ColourShaderSource(const char* body) {
    std::string s;
    for (const char* part : rr::hlsl::kHlslColourAll) s += part;
    s += rr::builtin::kHlslHostWords;
    s += body;
    return s;
  }
  static bool MultipliesByColour(const rr::RenderTargetBlend& b) {
    auto mul = [](rr::BlendFactor f) { return f != rr::BlendFactor::Zero && f != rr::BlendFactor::One; };
    return mul(b.src_color) || mul(b.dst_color) || mul(b.src_alpha) || mul(b.dst_alpha);
  }
  static bool IsFixed16Class(uint32_t cls) {
    return cls == uint32_t(::xenos::edram::ColorFormat::k_16_16) || cls == uint32_t(::xenos::edram::ColorFormat::k_16_16_16_16);
  }

  static gpu::BlendDesc HostBlend(const rr::RenderTargetBlend& rb) {
    auto factor = [](rr::BlendFactor f) {
      switch (f) {
        case rr::BlendFactor::AlphaFactor: return gpu::BlendFactor::kBlendFactor;      // as the reference (no
        case rr::BlendFactor::InvAlphaFactor: return gpu::BlendFactor::kInvBlendFactor;  // ALPHA_FACTOR there)
        default: return gpu::BlendFactor(uint8_t(f));                                   // same order otherwise
      }
    };
    gpu::BlendDesc b;
    b.enable = rb.blend_enable;
    b.src = factor(rb.src_color);
    b.dst = factor(rb.dst_color);
    b.op = gpu::BlendOp(uint8_t(rb.color_op));
    b.src_alpha = factor(rb.src_alpha);
    b.dst_alpha = factor(rb.dst_alpha);
    b.op_alpha = gpu::BlendOp(uint8_t(rb.alpha_op));
    return b;
  }

  // Before a colour target is drawn: ownership (both rules), then tiles another colour target of the same storage
  // class wrote last are copied in (M2 build A). Other sources (depth, other colour formats) are build B: counted,
  // labelled "other unmodelled source" and tainted.
  void PrepareColourTarget(rr::RenderTarget& t, const Ownership& own, bool known, uint32_t bits, bool read_only = false) {
    PartScope part(*this, kPartOwnership);
    colour_sources_.clear();
    if (own.active[0])
      rts_->MarkWritten(t, own.rect[0], nullptr, &colour_sources_, known, bits, rr::OwnerBook::kPrimary, !read_only,
                        own.limit[0]);
    if (own.active[1] && !read_only)
      rts_->MarkWritten(t, own.rect[1], nullptr, nullptr, known, bits, rr::OwnerBook::kAlternate, true, own.limit[1]);
    if (!t.texture || colour_sources_.empty()) return;
    const std::span<const Run> runs = BuildRuns(t, colour_sources_, false, own.drawn, colour_runs_);
    for (const Run& run : runs) {
      PartScope transfer_part(*this, kPartTransfer);
      const uint32_t tiles = uint32_t(run.members.size());
      const rr::RenderTarget* o = run.owner;
      const rr::PixelRect run_px{uint32_t(run.rect.x0), uint32_t(run.rect.y0), uint32_t(run.rect.x1),
                                 uint32_t(run.rect.y1)};
      // Gears 3 (job 030): 1x -> 4x of the same 32bpp format (the half-resolution 4x target on the scene's tiles).
      if (o && t.key.msaa == 2 && o->key.msaa == 0 && o->texture && t.texture && !rr::IsDepthClass(o->key.storage_class) &&
          o->key.storage_class == t.key.storage_class && !rr::RenderTargetCache::Is64bpp(t) &&
          !BitsClass(t.key.storage_class) && msaa_up_transfer_ps_ && MsaaUpTransfer(*o, t, run.rect)) {
        stats_.colour_transferred_tiles += tiles;
        if (!read_only)
          for (const Member& m : run.members) rts_->SetTaint(t, m.offset, m.taint);
        if (trace_on_) TraceEvent(t, run_px, "TRANSFERRED (1x -> 4x MSAA, per sample) from " + KeyText(o));
        continue;
      }
      const bool same = o && !rr::IsDepthClass(o->key.storage_class) && o->key.storage_class == t.key.storage_class &&
                        o->texture && o->key.msaa == 0 && t.key.msaa == 0 && colour_transfer_ps_ && transfer_vs_;
      const uint64_t run_area = uint64_t(run.rect.x1 - run.rect.x0) * uint64_t(run.rect.y1 - run.rect.y0);
      if (same) GpuCat(1);
      if (same && ColourTransfer(*o, t, run.rect)) {
        stats_.xfer_px[0] += run_area;
        stats_.colour_transferred_tiles += tiles;
        if (!read_only)
          for (const Member& m : run.members) rts_->SetTaint(t, m.offset, m.taint);
        if (trace_on_) TraceEvent(t, run_px, "TRANSFERRED (colour, same storage) from " + KeyText(o));
        continue;
      }
      // Another colour format with the same bits per sample, or a depth target (32bpp): through the guest words.
      const bool word_ok =
          o && o->texture && o->key.msaa == 0 && t.key.msaa == 0 && word_transfer_ps_ && transfer_vs_ &&
          (rr::IsDepthClass(o->key.storage_class)
               ? (transfer_mixed_bpp_ || !rr::RenderTargetCache::Is64bpp(t))
               : rr::ColorHostFormat(o->key.storage_class) != gpu::Format::kUnknown &&
                     (transfer_mixed_bpp_ ||
                      rr::RenderTargetCache::Is64bpp(*o) == rr::RenderTargetCache::Is64bpp(t)));
      if (word_ok) GpuCat(5);
      if (word_ok && WordTransfer(*o, t, run.rect)) {
        stats_.xfer_px[1] += run_area;
        stats_.colour_word_transferred_tiles += tiles;
        if (!read_only)
          for (const Member& m : run.members) rts_->SetTaint(t, m.offset, m.taint);
        if (trace_on_)
          TraceEvent(t, run_px, "TRANSFERRED (guest words, " +
                                    std::string(rr::IsDepthClass(o->key.storage_class) ? "depth -> colour" : "colour format change") +
                                    ") from " + KeyText(o));
        continue;
      }
      stats_.colour_unknown_tiles += tiles;
      if (!read_only)
        for (const Member& m : run.members) rts_->SetTaint(t, m.offset, 2);
      if (trace_on_) TraceEvent(t, run_px, "NOT transferred (colour: source not modelled until build B) from " + KeyText(o));
    }
  }

  // Colour formats whose host bits are the EDRAM bits but which float/SNORM I/O would change (upl-n108).
  bool BitsClass(uint32_t storage_class) const {
    if (!transfer_bits_ || rr::IsDepthClass(storage_class)) return false;
    switch (storage_class) {
      case 4: case 5: case 6: case 7: case 14: case 15: return true;
      default: return false;
    }
  }

  bool WordTransfer(const rr::RenderTarget& src, rr::RenderTarget& dst, const gpu::Rect& rect) {
    const bool dst_bits = BitsClass(dst.key.storage_class);
    auto& cache = dst_bits ? word_transfer_bits_pipelines_ : word_transfer_pipelines_;
    auto it = cache.find(dst.format);
    if (it == cache.end()) {
      gpu::GraphicsPipelineDesc d;
      d.vs = transfer_vs_;
      d.ps = dst_bits ? word_transfer_bits_ps_ : word_transfer_ps_;
      d.topology = gpu::Topology::kTriangleList;
      d.depth_clip = false;
      d.color_count = 1;
      d.color_formats[0] = dst.format;
      d.color_write_masks[0] = 0xF;
      d.color_bits = dst_bits;
      it = cache.emplace(dst.format, backend_->GetGraphicsPipeline(d)).first;
    }
    if (!it->second) return false;
    const bool from_depth = rr::IsDepthClass(src.key.storage_class);
    const bool src_bits = !from_depth && BitsClass(src.key.storage_class);
    gpu::TextureView views[96];
    if (from_depth) {
      views[0].texture = src.texture;
      views[0].plane = 0;
      views[1].texture = src.texture;
      views[1].plane = 1;
    } else if (src_bits) {
      views[3].texture = src.texture;
      views[3].bits_view = true;
    } else {
      views[2].texture = src.texture;
    }
    gpu::SamplerDesc samplers[32];
    tex_memo_valid_ = false;  // job 181: the bound views change
    backend_->SetTextures(views, samplers, 4, 4);
    gpu::TextureHandle rt = dst.texture;
    backend_->SetRenderTargets(1, &rt, 0, dst_bits);
    backend_->SetViewport({0.0f, 0.0f, float(dst.width), float(dst.height), 0.0f, 1.0f});
    backend_->SetScissor(rect);
    const uint32_t tile_w = rr::RenderTargetCache::Is64bpp(dst) ? 40u : 80u;
    uint32_t c[16] = {dst.key.base_tiles, dst.pitch_tiles, src.key.base_tiles, src.pitch_tiles,
                      tile_w, from_depth ? 1u : 0u, from_depth ? DepthFlags(src.key.storage_class) : src.key.storage_class,
                      dst.key.storage_class, src_bits ? 1u : 0u,
                      (!from_depth && rr::RenderTargetCache::Is64bpp(src)) ? 1u : 0u,  // patch 0005: t2.y
                      0u, 0u, dst.scale, src.scale};
    backend_->SetPipeline(it->second);
    backend_->SetConstants(gpu::ShaderStage::kPixel, gpu::kCbSystem, c, sizeof(c));
    backend_->Draw(3);
    Wrote(dst);  // job 182
    ++stats_.transfer_passes;
    return true;
  }

  // Gears 3 (job 030): 1x colour target -> a 4x MSAA colour target of the same storage class (32bpp, float/unorm
  // classes): every sample of the destination gets the 1x pixel at its EDRAM position (msaa_up_transfer_ps_).
  bool MsaaUpTransfer(const rr::RenderTarget& src, rr::RenderTarget& dst, const gpu::Rect& rect) {
    if (!msaa_up_transfer_ps_ || !transfer_vs_) return false;
    const uint32_t samples = 1u << dst.key.msaa;
    const uint64_t pk = uint64_t(dst.format) | uint64_t(samples) << 32;
    auto it = msaa_up_pipelines_.find(pk);
    if (it == msaa_up_pipelines_.end()) {
      gpu::GraphicsPipelineDesc d;
      d.vs = transfer_vs_;
      d.ps = msaa_up_transfer_ps_;
      d.topology = gpu::Topology::kTriangleList;
      d.depth_clip = false;
      d.color_count = 1;
      d.color_formats[0] = dst.format;
      d.color_write_masks[0] = 0xF;
      d.samples = samples;
      it = msaa_up_pipelines_.emplace(pk, backend_->GetGraphicsPipeline(d)).first;
    }
    if (!it->second) return false;
    gpu::TextureView views[96];
    views[0].texture = src.texture;
    gpu::SamplerDesc samplers[32];
    tex_memo_valid_ = false;
    backend_->SetTextures(views, samplers, 3, 4);
    gpu::TextureHandle rt = dst.texture;
    backend_->SetRenderTargets(1, &rt, 0, false);
    backend_->SetViewport({0.0f, 0.0f, float(dst.width), float(dst.height), 0.0f, 1.0f});
    backend_->SetScissor(rect);
    uint32_t c[16] = {dst.key.base_tiles, dst.pitch_tiles, src.key.base_tiles, src.pitch_tiles, 0u, 0u, 0u, 0u,
                      0u, 0u, 0u, 0u, dst.scale, src.scale};
    backend_->SetPipeline(it->second);
    backend_->SetConstants(gpu::ShaderStage::kPixel, gpu::kCbSystem, c, sizeof(c));
    backend_->Draw(3);
    Wrote(dst);
    ++stats_.transfer_passes;
    ++stats_.msaa_up_transfers;
    if (!msaa_up_logged_) {
      msaa_up_logged_ = true;
      REXLOG_INFO("gow2_renderer: MSAA transfer (first) frame {}: {} -> {} rect {},{}-{},{}", frame_ + 1, KeyText(&src),
                  KeyText(&dst), rect.x0, rect.y0, rect.x1, rect.y1);
    }
    return true;
  }

  bool ColourTransfer(const rr::RenderTarget& src, rr::RenderTarget& dst, const gpu::Rect& rect) {
    const bool bits = BitsClass(dst.key.storage_class);  // same storage class on both sides
    auto& cache = bits ? colour_transfer_bits_pipelines_ : colour_transfer_pipelines_;
    auto it = cache.find(dst.format);
    if (it == cache.end()) {
      gpu::GraphicsPipelineDesc d;
      d.vs = transfer_vs_;
      d.ps = bits ? colour_transfer_bits_ps_ : colour_transfer_ps_;
      d.topology = gpu::Topology::kTriangleList;
      d.depth_clip = false;
      d.color_count = 1;
      d.color_formats[0] = dst.format;
      d.color_write_masks[0] = 0xF;
      d.color_bits = bits;
      it = cache.emplace(dst.format, backend_->GetGraphicsPipeline(d)).first;
    }
    if (!it->second) return false;
    gpu::TextureView views[96];
    views[0].texture = src.texture;
    views[0].bits_view = bits;
    gpu::SamplerDesc samplers[32];
    tex_memo_valid_ = false;  // job 181: the bound views change
    backend_->SetTextures(views, samplers, 3, 4);
    gpu::TextureHandle rt = dst.texture;
    backend_->SetRenderTargets(1, &rt, 0, bits);
    backend_->SetViewport({0.0f, 0.0f, float(dst.width), float(dst.height), 0.0f, 1.0f});
    backend_->SetScissor(rect);
    const uint32_t tile_w = rr::RenderTargetCache::Is64bpp(dst) ? 40u : 80u;
    uint32_t c[16] = {dst.key.base_tiles, dst.pitch_tiles, src.key.base_tiles, src.pitch_tiles, tile_w,
                      0u, 0u, 0u, 0u, 0u, 0u, 0u, dst.scale, src.scale};
    backend_->SetPipeline(it->second);
    backend_->SetConstants(gpu::ShaderStage::kPixel, gpu::kCbSystem, c, sizeof(c));
    backend_->Draw(3);
    Wrote(dst);  // job 182
    ++stats_.transfer_passes;
    return true;
  }

  // Before a depth target is drawn or resolved: tiles another target wrote last get its data (D29 transfers, the
  // ones Gears 2 shows: depth -> depth, and colour tiles whose last write was a clear with a known value). Anything
  // else stays as our target last had it and is counted as unknown. `read_only` (resolves): ownership does not change
  // (on the hardware a resolve reads EDRAM; the owner keeps its tiles), but our texture still receives the data.
  // overwrites_all: the access replaces every bit (depth and stencil) of the tiles it covers completely.
  void PrepareDepthTarget(rr::RenderTarget& t, const Ownership& own, bool overwrites_all, bool is_clear = false,
                          bool read_only = false) {
    PartScope part(*this, kPartOwnership);
    tile_sources_.clear();
    alt_sources_.clear();
    aliases_.clear();
    if (own.active[0])
      rts_->MarkWritten(t, own.rect[0], &aliases_, &tile_sources_, false, 0, rr::OwnerBook::kPrimary, !read_only,
                        own.limit[0]);
    if (own.active[1])
      rts_->MarkWritten(t, own.rect[1], nullptr, &alt_sources_, false, 0, rr::OwnerBook::kAlternate, !read_only,
                        own.limit[1]);
    if (!t.texture) return;
    if (!is_clear && !read_only) ReportAliases();  // reusing tiles before a clear is expected
    const std::span<const Run> runs = BuildRuns(t, tile_sources_, overwrites_all, own.drawn, depth_runs_);
    const bool dest_float = (t.key.storage_class & 1) != 0;
    auto tile_rect = [&](uint32_t offset) {
      const rr::PixelRect pr = rr::RenderTargetCache::TilePixels(t, offset);
      return gpu::Rect{int32_t(pr.x0), int32_t(pr.y0), int32_t(pr.x1), int32_t(pr.y1)};
    };
    for (const Run& run : runs) {
      const uint32_t tiles = uint32_t(run.members.size());
      const rr::RenderTarget* o = run.owner;
      bool done = false;
      float origin = 0.0f;
      PartScope transfer_part(*this, kPartTransfer);
      const bool colour_source = o && !rr::IsDepthClass(o->key.storage_class) && o->texture &&
                                 (!rr::RenderTargetCache::Is64bpp(*o) || transfer_mixed_bpp_) &&
                                 rr::ColorHostFormat(o->key.storage_class) != gpu::Format::kUnknown;
      if (o && (rr::IsDepthClass(o->key.storage_class) || colour_source) && o->texture && o->key.msaa == 0 &&
          t.key.msaa == 0 && transfer_vs_ && transfer_ps_) {
        // Depth -> depth, or colour -> depth through the colour's guest word (M2 build B).
        GpuCat(colour_source ? 7 : 6);
        // Job 182: a read-only access takes no ownership, so the same tiles come from the same source at every such
        // draw (ch02: ~77 a frame). If neither target was written since our last refresh and the run is inside what
        // we refreshed then, the tiles already hold it.
        ReadOnlyMemo* memo = nullptr;
        if (read_only && readonly_memo_) {
          memo = &readonly_memo_map_[(uint64_t(t.id) << 32) | o->id];
          if (memo->t_gen == t.write_gen && memo->s_gen == o->write_gen && memo->t_gen &&
              run.rect.x0 >= memo->rect.x0 && run.rect.y0 >= memo->rect.y0 && run.rect.x1 <= memo->rect.x1 &&
              run.rect.y1 <= memo->rect.y1) {
            ++stats_.readonly_transfers_skipped;
            stats_.readonly_px_skipped += uint64_t(run.rect.x1 - run.rect.x0) * uint64_t(run.rect.y1 - run.rect.y0);
            continue;
          }
        }
        const uint64_t t_gen_before = t.write_gen;
        // Job 185: the asking draw is a rectangle fill that covers this run exactly and overwrites depth and/or
        // stencil with ALWAYS tests - only what it keeps needs transferring.
        uint32_t parts = 3;
        if (fill_.valid && !read_only && run.rect.x0 >= fill_.rect.x0 && run.rect.y0 >= fill_.rect.y0 &&
            run.rect.x1 <= fill_.rect.x1 && run.rect.y1 <= fill_.rect.y1) {
          if (fill_.depth) parts &= ~1u;
          if (fill_.stencil) parts &= ~2u;
        }
        if (!parts) {
          ++stats_.fill_transfers_skipped;
          stats_.fill_px_skipped += uint64_t(run.rect.x1 - run.rect.x0) * uint64_t(run.rect.y1 - run.rect.y0);
          Wrote(t);
          done = true;
        } else {
          done = TransferDepth(*o, t, run.rect, dest_float, parts);
        }
        if (done && memo) {
          const bool extend = memo->t_gen == t_gen_before && memo->s_gen == o->write_gen && memo->t_gen;
          memo->rect = extend ? gpu::Rect{std::min(memo->rect.x0, run.rect.x0), std::min(memo->rect.y0, run.rect.y0),
                                          std::max(memo->rect.x1, run.rect.x1), std::max(memo->rect.y1, run.rect.y1)}
                              : run.rect;
          memo->t_gen = t.write_gen;
          memo->s_gen = o->write_gen;
        }
        if (done) {
          const uint64_t px = uint64_t(run.rect.x1 - run.rect.x0) * uint64_t(run.rect.y1 - run.rect.y0);
          stats_.xfer_px[colour_source ? 3 : 2] += px;
          // Job 180: which (source -> destination) target pairs the depth transfers are, and the draw that asked.
          XferPair& p = xfer_pairs_[{o->id, t.id}];
          if (!p.px) {
            p.text = KeyText(o) + " -> " + KeyText(&t);
          }
          p.px += px;
          ++p.runs;
          if (is_clear) p.clear_px += px;
          if (read_only) p.read_px += px;
          if (overwrites_all) p.overwrite_px += px;
          p.vs = xfer_draw_.vs;
          p.ps = xfer_draw_.ps;
          p.prim = xfer_draw_.prim;
          p.depth_func = xfer_draw_.depth_func;
          p.depth_write = xfer_draw_.depth_write;
          p.stencil = xfer_draw_.stencil;
          p.stencil_wmask = xfer_draw_.stencil_wmask;
          p.colour = xfer_draw_.colour;
        }
        if (done) stats_.transferred_tiles += tiles;
        if (done && colour_source) stats_.colour_to_depth_tiles += tiles;
        origin = 1.0f;
      } else if (o && run.known) {
        // Raw EDRAM word: depth24 = bits >> 8 decoded in our format, stencil = low byte (Canary kColorToDepth).
        const uint32_t d24 = run.bits >> 8;
        const float depth =
            dest_float ? HostDepth(t, ::xenos::formats::Float20e4To32(d24)) : ::xenos::edram::UNorm24To32(d24);
        backend_->ClearDepthStencil(t.texture, true, depth, true, uint8_t(run.bits & 0xFF), &run.rect, 1);
        Wrote(t);
        stencil_ones_clears_ |= uint8_t(run.bits & 0xFF);
        stats_.cleared_tiles += tiles;
        done = true;
        origin = 2.0f;
      }
      const rr::PixelRect run_px{uint32_t(run.rect.x0), uint32_t(run.rect.y0), uint32_t(run.rect.x1),
                                 uint32_t(run.rect.y1)};
      if (!done) {
        const bool colour = o && !rr::IsDepthClass(o->key.storage_class);
        stats_.unknown_tiles += tiles;
        if (colour) stats_.unknown_colour_tiles += tiles;
        if (!read_only)
          for (const Member& m : run.members) rts_->SetTaint(t, m.offset, colour ? 1 : 2);
        if (trace_on_)
          TraceEvent(t, run_px, std::string("NOT transferred (") + (colour ? "colour source" : "unmodelled source") +
                                    ") from " + KeyText(o) + (read_only ? " [resolve read]" : ""));
        continue;
      }
      // The transferred content carries the source's taint: label those tiles by where their data really came from.
      uint32_t tainted = 0;
      for (const Member& m : run.members) {
        const uint8_t taint = origin == 1.0f ? m.taint : 0;
        if (!read_only) rts_->SetTaint(t, m.offset, taint);
        if (taint) {
          ++tainted;
        }
      }
      if (tainted) stats_.tainted_tiles += tainted;
      if (trace_on_)
        TraceEvent(t, run_px, std::string(origin == 1.0f ? "TRANSFERRED" : "FILLED with known colour-clear bits") +
                                  " from " + KeyText(o) + " (run " + std::to_string(run.rect.x0) + "," +
                                  std::to_string(run.rect.y0) + "-" + std::to_string(run.rect.x1) + "," +
                                  std::to_string(run.rect.y1) + ", " + std::to_string(tainted) +
                                  " tile(s) carrying colour-derived/unmodelled content)" +
                                  (read_only ? " [resolve read]" : ""));
    }
  }

  // Pipelines of the transfer into one destination format, created once.
  const std::array<gpu::PipelineHandle, 9>* TransferPipelines(gpu::Format format) {
    auto it = transfer_pipelines_.find(format);
    if (it != transfer_pipelines_.end()) return &it->second;
    std::array<gpu::PipelineHandle, 9> p{};
    const bool one_pass = transfer_ps_ref_ != 0;
    for (uint32_t pass = 0; pass < (one_pass ? 1u : 9u); ++pass) {
      gpu::GraphicsPipelineDesc d;
      d.vs = transfer_vs_;
      d.ps = one_pass ? transfer_ps_ref_ : transfer_ps_;
      d.topology = gpu::Topology::kTriangleList;
      d.depth_clip = false;
      d.depth_format = format;
      if (pass == 0) {
        d.depth_enable = true;
        d.depth_write = true;
        d.depth_func = gpu::CompareFunc::kAlways;
      }
      if (pass != 0 || one_pass) {
        d.stencil_enable = true;
        d.stencil_read_mask = 0xFF;
        d.stencil_write_mask = one_pass ? uint8_t(0xFF) : uint8_t(1u << (pass - 1));
        d.stencil_front.func = d.stencil_back.func = gpu::CompareFunc::kAlways;
        d.stencil_front.pass = d.stencil_back.pass = gpu::StencilOp::kReplace;
      }
      p[pass] = backend_->GetGraphicsPipeline(d);
      if (!p[pass]) return nullptr;
    }
    return &transfer_pipelines_.emplace(format, p).first->second;
  }

  // parts: 1 depth, 2 stencil, 3 both (job 185: what the asking fill draw does not overwrite itself).
  bool TransferDepth(const rr::RenderTarget& src, rr::RenderTarget& dst, const gpu::Rect& rect, bool dest_float,
                     uint32_t parts = 3) {
    const std::array<gpu::PipelineHandle, 9>* pipelines = TransferPipelines(dst.format);
    if (!pipelines) return false;
    const bool one_pass = transfer_ps_ref_ != 0;
    const bool from_colour = !rr::IsDepthClass(src.key.storage_class);
    gpu::TextureView views[96];
    const bool src_bits = from_colour && BitsClass(src.key.storage_class);
    if (src_bits) {
      views[3].texture = src.texture;
      views[3].bits_view = true;
    } else if (from_colour) {
      views[2].texture = src.texture;
    } else {
      views[0].texture = src.texture;
      views[0].plane = 0;
      views[1].texture = src.texture;
      views[1].plane = 1;
    }
    gpu::SamplerDesc samplers[32];
    tex_memo_valid_ = false;  // job 181: the bound views change
    backend_->SetTextures(views, samplers, 4, 4);
    backend_->SetRenderTargets(0, nullptr, dst.texture);
    backend_->SetViewport({0.0f, 0.0f, float(dst.width), float(dst.height), 0.0f, 1.0f});
    backend_->SetScissor(rect);
    // Bit passes: stencil starts at 0 in the rectangle and the passes set the 1 bits. One pass: SV_StencilRef.
    if (!one_pass && (parts & 2)) backend_->ClearDepthStencil(dst.texture, false, 0.0f, true, 0, &rect, 1);
    uint32_t c[16] = {dst.key.base_tiles, dst.pitch_tiles, src.key.base_tiles, src.pitch_tiles,
                      from_colour ? 0u : DepthFlags(src.key.storage_class),
                      dest_float ? DepthFlags(dst.key.storage_class) : 0u, 0u, 0u,
                      from_colour ? 1u : 0u, from_colour ? src.key.storage_class : 0u, src_bits ? 1u : 0u,
                      (from_colour && rr::RenderTargetCache::Is64bpp(src)) ? 1u : 0u,  // patch 0005: t2.w
                      dst.scale, src.scale};
    // Job 179: --gow2_renderer_stencil_transfer_bits=1 runs bit passes only for the stencil bits the title ever sets
    // or tests (draws, clears, read masks so far); the others stay 0 from the clear above. Without SV_StencilRef
    // (NVIDIA) each bit is a full pass over the rectangle.
    const uint8_t bit_mask = stencil_transfer_bits_ && !in_selftest_ ? uint8_t(stencil_ones_draws_ | stencil_ones_clears_ | stencil_reads_) : 0xFF;
    for (uint32_t pass = 0; pass < (one_pass ? 1u : 9u); ++pass) {
      if (pass && !((bit_mask >> (pass - 1)) & 1)) {
        ++stats_.stencil_passes_skipped;
        continue;
      }
      if ((pass == 0 && !one_pass && !(parts & 1)) || (pass > 0 && !(parts & 2))) {
        ++stats_.fill_passes_skipped;
        continue;
      }
      c[6] = pass ? 1u : 0u;
      c[7] = pass ? pass - 1 : 0u;
      backend_->SetPipeline((*pipelines)[pass]);
      backend_->SetStencilRef(0xFF);
      backend_->SetConstants(gpu::ShaderStage::kPixel, gpu::kCbSystem, c, sizeof(c));
      backend_->Draw(3);
    }
    Wrote(dst);  // job 182
    ++stats_.transfer_passes;
    return true;
  }

  // ---- Transfer self-test (upl-n108) ----
  // At the first swap: 1280 known 32-bit EDRAM words go colour -> depth -> colour (and through 7e3 and 16_16_FLOAT)
  // with the same transfer passes the game's ownership changes use, on 80x16 scratch targets (one tile). The result
  // is read back a frame or two later and every word that doesn't come back bit-exact is logged. Job 111 traced
  // 01407C0F -> 01407FFF across colour -> D24FS8 -> colour; this proves or clears the transfer passes.
  struct SelfTestChain {
    const char* name;
    std::vector<uint32_t> classes;  // storage classes after the 8888 source; the last is copied back into 8888
    gpu::ReadbackHandle readback = 0;
    bool started = false;
  };
  rr::RenderTarget MakeScratchTarget(uint32_t storage_class) {
    rr::RenderTarget t;
    t.key = {0, storage_class, 80, 0};
    t.pitch_tiles = 1;
    t.width = t.guest_width = 80;
    t.height = t.guest_height = 16;
    gpu::TextureDesc d;
    d.width = 80;
    d.height = 16;
    d.debug_name = "gow2 transfer self-test";
    if (rr::IsDepthClass(storage_class)) {
      d.format = (storage_class & 1) ? gpu::Format::kD32FloatS8X24 : gpu::Format::kD24UnormS8;
      d.usage = gpu::kUsageDepthStencil | gpu::kUsageSampled;
    } else {
      d.format = rr::ColorHostFormat(storage_class);
      d.usage = gpu::kUsageRenderTarget | gpu::kUsageSampled;
    }
    t.format = d.format;
    if (d.format != gpu::Format::kUnknown) t.texture = backend_->CreateTexture(d);
    return t;
  }
  static std::vector<uint32_t> SelfTestWords() {
    std::vector<uint32_t> w(1280);
    static constexpr uint32_t kDepths[16] = {0x000000, 0x01407C, 0xFFFFFF, 0x7FFFFF, 0x800000, 0xF00000, 0x0FFFFF,
                                             0x000001, 0x100000, 0x123456, 0xEFFFFF, 0x00702A, 0xC00000, 0x3F8000,
                                             0x000004, 0x5A5A5A};
    for (uint32_t i = 0; i < 256; ++i) w[i] = (kDepths[i & 15] << 8) | i;  // every stencil value
    static constexpr uint32_t kGame[8] = {0x01407C0Fu, 0x01407FFFu, 0x1FC01B80u, 0x1FC043F8u,
                                          0x00702404u, 0xC0000000u, 0x00040404u, 0x00100C01u};
    for (uint32_t i = 0; i < 8; ++i) w[256 + i] = kGame[i];
    for (uint32_t i = 0; i < 32; ++i) w[264 + i] = 1u << i;
    uint32_t x = 0x9E3779B9u;
    for (uint32_t i = 296; i < 1280; ++i) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      w[i] = x;
    }
    return w;
  }
  void StartTransferSelfTest() {
    in_selftest_ = true;  // job 179: all stencil bits (the test checks every bit)
    struct Done {
      bool& f;
      ~Done() { f = false; }
    } done{in_selftest_};
    selftest_started_ = true;
    if (!REXCVAR_GET(gow2_renderer_transfer_selftest) || !transfer_vs_ || !transfer_ps_ || !word_transfer_ps_) return;
    const uint32_t c8888 = rr::ColorStorageClass(0), c7e3 = rr::ColorStorageClass(3),
                   c16f = rr::ColorStorageClass(6), c2101010 = rr::ColorStorageClass(2);
    const uint32_t d24 = rr::DepthStorageClass(0), d24f = rr::DepthStorageClass(1);
    selftest_ = {{"8888 -> D24S8 -> 8888", {d24}},
                 {"8888 -> D24FS8 -> 8888", {d24f}},
                 {"8888 -> 7e3 -> 8888", {c7e3}},
                 {"8888 -> 7e3 -> D24FS8 -> 7e3 -> 8888 (job 111's leg)", {c7e3, d24f, c7e3}},
                 {"8888 -> 7e3 -> D24S8 -> 7e3 -> 8888", {c7e3, d24, c7e3}},
                 {"8888 -> 16_16_FLOAT -> 8888", {c16f}},
                 {"8888 -> D24FS8 -> 16_16_FLOAT -> D24S8 -> 8888", {d24f, c16f, d24}},
                 // Gears 3 job 034: ch1's scene copy 4 has 587324 px "transferred but different" in a k_2_10_10_10
                 // (unorm) view of the 7e3 scene - a storage class the test never covered.
                 {"8888 -> 2_10_10_10 -> 8888", {c2101010}},
                 {"8888 -> 7e3 -> 2_10_10_10 -> 7e3 -> 8888", {c7e3, c2101010, c7e3}}};
    if (transfer_mixed_bpp_) {  // patch 0005: 32bpp <-> 64bpp through the EDRAM words (64bpp scratch: tile 0 = x < 40)
      const uint32_t c16x4 = rr::ColorStorageClass(5), c16x4f = rr::ColorStorageClass(7);
      selftest_.push_back({"8888 -> 16_16_16_16 -> 8888 (patch 0005)", {c16x4}});
      selftest_.push_back({"8888 -> 16_16_16_16_FLOAT -> 8888 (patch 0005)", {c16x4f}});
      selftest_.push_back({"8888 -> 16_16_16_16 -> D24S8 -> 8888 (patch 0005, ch22 shadow)", {c16x4, d24}});
      selftest_.push_back({"8888 -> D24S8 -> 16_16_16_16_FLOAT -> 8888 (patch 0005)", {d24, c16x4f}});
    }
    const std::vector<uint32_t> words = SelfTestWords();
    rr::RenderTarget source = MakeScratchTarget(c8888);
    if (!source.texture) return;
    backend_->UploadTexture(source.texture, 0, 0, words.data(), 80 * 4, 16, 1);
    const gpu::Rect all{0, 0, 80, 16};
    for (SelfTestChain& chain : selftest_) {
      rr::RenderTarget prev = source;
      std::vector<rr::RenderTarget> made;
      bool ok = true;
      std::vector<uint32_t> classes = chain.classes;
      classes.push_back(c8888);
      for (uint32_t cls : classes) {
        made.push_back(MakeScratchTarget(cls));
        rr::RenderTarget& next = made.back();
        if (!next.texture) {
          ok = false;
          break;
        }
        const bool to_depth = rr::IsDepthClass(cls);
        ok = to_depth ? TransferDepth(prev, next, all, (cls & 1) != 0) : WordTransfer(prev, next, all);
        if (!ok) break;
        prev = next;
      }
      if (ok) {
        chain.readback = backend_->ReadbackTexture(prev.texture, all);
        chain.started = chain.readback != 0;
      }
      for (rr::RenderTarget& t : made)
        if (!chain.started || t.texture != prev.texture) backend_->DestroyTexture(t.texture);  // deferred release
      if (chain.started) selftest_textures_.push_back(prev.texture);
      else REXLOG_WARN("gow2_renderer: transfer self-test {}: not run (a format has no host storage or a pass failed)",
                       chain.name);
    }
    selftest_textures_.push_back(source.texture);
    selftest_words_ = words;
    tex_memo_valid_ = false;  // job 181
    backend_->SetTextures(nullptr, nullptr);
  }
  void CheckTransferSelfTest() {
    bool pending = false;
    for (SelfTestChain& chain : selftest_) {
      if (!chain.readback) continue;
      if (!backend_->IsReadbackReady(chain.readback)) {
        pending = true;
        continue;
      }
      uint32_t pitch = 0;
      const uint8_t* data = backend_->ReadbackData(chain.readback, pitch);
      uint32_t bad = 0;
      std::string first;
      for (uint32_t y = 0; data && y < 16; ++y)
        for (uint32_t x = 0; x < 80; ++x) {
          uint32_t got;
          std::memcpy(&got, data + size_t(y) * pitch + 4 * x, 4);
          const uint32_t want = selftest_words_[y * 80 + x];
          if (got == want) continue;
          if (++bad <= 8) {
            char b[64];
            std::snprintf(b, sizeof(b), "%s%08X -> %08X", first.empty() ? "" : ", ", want, got);
            first += b;
          }
        }
      if (!data) {
        REXLOG_WARN("gow2_renderer: transfer self-test {}: no readback data", chain.name);
      } else if (bad) {
        REXLOG_WARN("gow2_renderer: transfer self-test {}: {} of 1280 words NOT bit-exact (first: {})", chain.name, bad,
                    first);
      } else {
        REXLOG_INFO("gow2_renderer: transfer self-test {}: all 1280 words bit-exact", chain.name);
      }
      backend_->ReleaseReadback(chain.readback);
      chain.readback = 0;
    }
    if (!pending && !selftest_textures_.empty()) {
      for (gpu::TextureHandle t : selftest_textures_) backend_->DestroyTexture(t);
      selftest_textures_.clear();
    }
  }

  // Gears 3 (notes/11): a draw that rasterizes nothing (edram mode no-op / multipass export / both faces culled / viz
  // kill) whose vertex shader memexports. Canary issues it with rasterization off; here the vertex shader runs once per
  // index as a point list with no targets and no pixel shader, the guest-memory mirror writable through u0.
  void MemexportOnlyDraw(const uint32_t* r, const rr::GuestShader& vsg, uint32_t prim, uint32_t index_count,
                         const ObservedIndexBuffer* ib) {
    if (REXCVAR_GET(gow2_renderer_export_wait) && vsg.state == rr::ShaderState::kPending &&
        cache_->WaitReady(vsg, 2000.0))
      ++stats_.export_waits;  // patch 0024
    // Patch 0045 (Edge "new memexport VS"): a skipped export is lost for good, so a new exporting vertex shader waits.
    if (standin_wait_budget_ms_ > 0.0 && !vsg.drawn && vsg.state == rr::ShaderState::kPending) {
      if (standin_wait_left_ms_ > 0.0) {
        const auto t0 = std::chrono::steady_clock::now();
        cache_->WaitReady(vsg, standin_wait_left_ms_);
        const double ms = MsSince(t0);
        standin_wait_left_ms_ -= ms;
        ++stats_.standin_waits;
        stats_.standin_wait_ms += ms;
        ++standin_reasons_["new memexport VS (export-only draw)"];
      } else {
        ++stats_.standin_over_budget;
      }
    }
    vsg.drawn = true;
    WaitForShader(vsg);
    if (vsg.state != rr::ShaderState::kReady || !vsg.handle) {
      SkipForShaders(vsg.state == rr::ShaderState::kPending);
      Skip("memexport-only draw: vertex shader not ready");
      return;
    }
    if (!index_count) return;
    gpu::GraphicsPipelineDesc d;
    d.vs = vsg.handle;
    d.ps = 0;
    d.topology = gpu::Topology::kPointList;
    d.depth_clip = false;
    // Synchronous: an export must not be lost to a pipeline still being created.
    bool pending = false;
    const uint64_t misses_before = backend_->PipelineMisses();  // patch 0151
    const gpu::PipelineHandle pipeline = backend_->GetGraphicsPipeline(d, false, &pending);
    if (!pipeline) {
      ++stats_.pipeline_failures;
      Skip("memexport-only draw: no pipeline");
      return;
    }
    // Patch 0151 (deep-dive review): recorded in pipelines.list like the main path's, so the next start builds it ahead
    // (it was made synchronously on this thread in every session - a hitch per new exporting shader). Only the
    // untessellated variant: the list's record names the shader by hash + tessellation count 0.
    // Only when this draw made it (as the main path; review: thousands of export-only draws a frame in Gears 3).
    if (pipeline_list_ && backend_->PipelineMisses() != misses_before && cache_->Find(vs_.hash, true, 0) == &vsg)
      RecordPipeline(d, vs_.hash, 0, 0);
    SystemConstants sc{};
    sc.sys0[0] = 1u | 128u;  // w not reciprocal (the position is never used) | exports allowed
    sc.sys0[1] = r[kRegVgtIndxOffset];
    sc.sys2[0] = r[kRegVgtMinVtxIndx] & 0xFFFFFFu;  // patch 0028
    sc.sys2[1] = r[kRegVgtMaxVtxIndx] & 0xFFFFFFu;
    if (!sc.sys2[1]) sc.sys2[1] = 0xFFFFFFu;  // deviation: a never-written max (0) would clamp every index to 0
    sc.ndc_scale[0] = sc.ndc_scale[1] = sc.ndc_scale[2] = 1.0f;
    sc.ndc_scale[3] = float(res_scale_);
    backend_->SetConstants(gpu::ShaderStage::kVertex, gpu::kCbSystem, &sc, sizeof(sc));
    backend_->SetConstants(gpu::ShaderStage::kVertex, gpu::kCbBoolLoop, &r[kRegBoolConst], 40 * 4);
    // Float constants: the same layout as the main path (compacted / trimmed), and the main path's generation
    // skip is reset, because this upload changed the slot behind its back.
    if (vsg.compacted()) {
      backend_->SetConstantsSplit(gpu::ShaderStage::kVertex, gpu::kCbFloat, &r[kRegFloatConstVs], vsg.float_constants_low * 16,
                                  &r[kRegFloatConstVs + 4 * vsg.float_constants_high],
                                  (vsg.float_constants - vsg.float_constants_high) * 16);
    } else {
      const uint32_t vs_floats = float_constants_trim_ ? vsg.float_constants : 256u;
      backend_->SetConstants(gpu::ShaderStage::kVertex, gpu::kCbFloat, &r[kRegFloatConstVs], vs_floats * 16);
    }
    vs_gen_seen_ = ~0ull;
    // Textures the vertex shader fetches (bone matrices and the like), as the main path binds them.
    if ((vsg.texture_slots_2d | vsg.texture_slots_cube | vsg.texture_slots_3d) && use_textures_) {
      gpu::TextureView views[96];
      gpu::SamplerDesc samplers[32];
      const uint32_t t2d = vsg.texture_slots_2d, tcube = vsg.texture_slots_cube, t3d = vsg.texture_slots_3d;
      backend_->SetGpuCategory(3);  // job 048, as the main path
      textures_->Bind(&r[kRegFetchConst], t2d, tcube, t3d, frame_, views, samplers);
      backend_->SetGpuCategory(gpu_cat_ != 0xFF ? gpu_cat_ : 0);
      const uint32_t count = t3d ? 64u + 32u - uint32_t(std::countl_zero(t3d))
                             : tcube ? 32u + 32u - uint32_t(std::countl_zero(tcube))
                                     : 32u - uint32_t(std::countl_zero(t2d));
      backend_->SetTextures(views, samplers, count, 32u - uint32_t(std::countl_zero(t2d | tcube | t3d)));
      tex_memo_valid_ = false;
    }
    std::memcpy(fetch_, &r[kRegFetchConst], 192 * 4);
    UploadVertexData(r, vsg);
    backend_->SetConstants(gpu::ShaderStage::kVertex, gpu::kCbFetch, fetch_, FetchUploadBytes());
    bool indexed = false;
    indices_.clear();
    index_ptr_ = &indices_;
    if (ib) {
      if (!ReadIndices(*ib, index_count, ~0u, false)) {
        ++stats_.other_failures;
        return;
      }
      indexed = true;
    }
    backend_->SetPipeline(pipeline);
    backend_->SetRenderTargets(0, nullptr, 0);
    backend_->SetViewport({0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f});
    backend_->SetScissor({0, 0, 1, 1});
    if (REXCVAR_GET(gow2_renderer_export_prepare_mirror)) MarkExports(r, vsg, true);  // patch 0024
    backend_->SetGuestWritable(true);
    if (indexed) backend_->DrawIndexed(index_ptr_->data(), uint32_t(index_ptr_->size()));
    else backend_->Draw(index_count);
    backend_->SetGuestWritable(false);
    ++stats_.memexport_only_draws;
    MarkExports(r, vsg);
    // Patch 0012: besides the first 8, the first export draw of every vertex shader (up to 96 shaders) is logged, so a
    // gameplay log shows what the title exports (formats, counts) and not only the load-time texture moves.
    const bool new_vs = memexport_vs_seen_.size() < 96 && memexport_vs_seen_.insert(vs_.hash).second;
    if (memexport_logged_ < 8 || new_vs) {
      ++memexport_logged_;
      std::string sc_text;
      for (uint32_t c : vsg.memexport_stream_constants) {
        if (c >= 256) continue;
        const uint32_t* w = &r[kRegFloatConstVs + 4 * c];
        if ((w[0] >> 30) != 1u || (w[2] >> 20) != 0x4B0u || (w[3] >> 23) != 0x96u) continue;
        sc_text += std::format(" | c{}: base {:08X} count {} format {} endian {} signed {} integer {} swap {}", c,
                               (w[0] & 0x3FFFFFFFu) << 2, w[3] & 0x7FFFFFu, (w[2] >> 8) & 63u, w[2] & 7u,
                               (w[2] >> 16) & 1u, (w[2] >> 17) & 1u, (w[2] >> 19) & 1u);
      }
      for (uint32_t i = 0; i < 96; ++i)
        if (((vsg.vfetch_constants[i >> 5] >> (i & 31)) & 1) && (r[kRegFetchConst + 2 * i] & 3) == 3)
          sc_text += std::format(" | vf{}: {:08X} size {} endian {}", i, r[kRegFetchConst + 2 * i] & ~3u,
                                 ((r[kRegFetchConst + 2 * i + 1] >> 2) & 0xFFFFFFu) * 4, r[kRegFetchConst + 2 * i + 1] & 3u);
      REXLOG_INFO("gow2_renderer: memexport-only draw frame {} draw {}: prim {} {} {} vs {:016X}{}", frame_ + 1,
                  draw_index_, prim, ib ? "indexed" : "auto-indexed", index_count, vs_.hash, sc_text);
    }
  }

  // Job 041: completed export readbacks into guest memory (bytes as the GPU stored them = guest byte order), and the SDK
  // is told the range changed. Patch 0007 note [V, rexglue xmemory.h]: TranslatePhysical is the physical mapping, which
  // the memory system never write-protects, so this copy raises NO watch callback: pages still GPU-current stay so (both
  // copies now hold the same bytes). Known gap: if the game's CPU rewrote part of the range after the export, this
  // late copy overwrites those newer bytes (the race xenia-edge removed by dropping readback, a84f4d1ff).
  // Job 042 (ground flicker): a CPU-path texture load is about to read [start, start + length). If an export readback
  // over it is still pending, finish the GPU work and write the results into guest memory first - otherwise the load
  // takes the pre-move bytes for the frame or two until the readback lands.
  void FlushExportReadbacksFor(uint32_t start, uint32_t length) {
    for (const auto& [a, e] : export_ranges_)  // patch 0042: gathered, not issued yet
      if (start < e && a < start + uint64_t(length)) {
        IssueExportReadbacks();
        break;
      }
    bool overlap = false;
    for (const ExportReadback& e : export_readbacks_)
      overlap |= start < e.base + e.size && e.base < start + length;
    if (!overlap) return;
    ++stats_.memexport_readback_waits;
    backend_->WaitForFence(backend_->Submit());
    PollExportReadbacks();
  }

  // Patch 0042: an exported range [base, base + size) for the end-of-frame readback, merged with any gathered range it
  // overlaps or touches (only those: copying a gap would put the mirror's bytes over guest memory the GPU never wrote).
  void AddExportRange(uint32_t base, uint32_t size) {
    uint64_t a = base, e = uint64_t(base) + size;
    ++stats_.memexport_ranges_gathered;
    // The map holds disjoint, non-touching [start, end) ranges keyed by start. Fold in the one before (if it reaches a)
    // and every one starting at or before e.
    auto it = export_ranges_.upper_bound(a);
    if (it != export_ranges_.begin()) {
      auto prev = std::prev(it);
      if (prev->second >= a) it = prev;
    }
    while (it != export_ranges_.end() && it->first <= e) {
      a = std::min(a, it->first);
      e = std::max(e, it->second);
      it = export_ranges_.erase(it);
    }
    export_ranges_.emplace(a, e);
  }

  // Patch 0042: one readback per merged range, recorded after every export draw so far (they land via
  // PollExportReadbacks, in order, once the submission has completed).
  void IssueExportReadbacks() {
    for (const auto& [a, e] : export_ranges_) {  // in address order
      const uint32_t base = uint32_t(a), size = uint32_t(e - a);
      if (const gpu::ReadbackHandle h = backend_->ReadbackGuest(base, size)) {
        export_readbacks_.push_back({h, base, size});
        ++stats_.memexport_readbacks_issued;
      } else {
        ++stats_.memexport_readback_failures;
      }
    }
    export_ranges_.clear();
  }

  void PollExportReadbacks() {
    while (!export_readbacks_.empty()) {
      const ExportReadback& e = export_readbacks_.front();
      if (!backend_->IsReadbackReady(e.handle)) return;
      uint32_t pitch = 0;
      const uint8_t* data = backend_->ReadbackData(e.handle, pitch);
      if (uint8_t* dst = data ? memory_->TranslatePhysical<uint8_t*>(e.base) : nullptr) {
        std::memcpy(dst, data, e.size);
        rex::graphics::NotifyGuestRangeWrittenByObserver(e.base, e.size);
        ++stats_.memexport_readbacks;
        stats_.memexport_readback_bytes += e.size;
      }
      backend_->ReleaseReadback(e.handle);
      export_readbacks_.pop_front();
    }
  }

  // The guest memory a memexporting draw writes, from its stream constant(s) (the SDK's draw_util::AddMemExportRanges):
  // marked GPU-current (the mirror holds the newer bytes) and watched, so a later CPU write ends that.
  // prepare_only (patch 0024): before the draw, only make the export ranges' pages current in the mirror.
  void MarkExports(const uint32_t* r, const rr::GuestShader& s, bool prepare_only = false) {
    for (uint32_t c : s.memexport_stream_constants) {
      if (c >= 256) continue;
      const uint32_t* w = &r[kRegFloatConstVs + 4 * c];
      if ((w[0] >> 30) != 1u || (w[2] >> 20) != 0x4B0u || (w[3] >> 23) != 0x96u) continue;
      const uint32_t count = w[3] & 0x7FFFFFu;
      if (!count) continue;
      const uint32_t fmt = (w[2] >> 8) & 63u;
      uint32_t bytes_per = 0;
      switch (fmt) {
        case 2: case 8: case 9: bytes_per = 1; break;
        case 3: case 4: case 5: case 10: case 15: case 24: case 30: bytes_per = 2; break;
        case 6: case 7: case 14: case 16: case 17: case 25: case 31: case 36: case 50: case 54: case 55: case 56:
          bytes_per = 4;
          break;
        case 26: case 32: case 37: bytes_per = 8; break;
        case 38: bytes_per = 16; break;
        default: break;
      }
      if (!bytes_per) continue;
      const uint32_t base = (w[0] & 0x3FFFFFFFu) << 2;
      uint64_t size = uint64_t(count) * bytes_per;
      if ((base & 0x1FFFFFFFu) + size > (512ull << 20)) size = (512ull << 20) - (base & 0x1FFFFFFFu);
      if (!size) continue;
      if (prepare_only) {  // patch 0024
        PrepareMirrorForResolve(base, uint32_t(size));
        continue;
      }
      watch_->Watch(base, uint32_t(size));
      watch_->MarkGpuWritten(base, uint32_t(size));
      exports_marked_ = true;
      if (memexport_readback_) {
        // Job 041: the exported bytes also go back to guest memory (CPU-path texture loads read it), written by
        // PollExportReadbacks once this submission has completed. Patch 0042: not one readback per draw - Gears 3
        // (session 12:47, frame 29894) issued 6173 export-only draws in one frame whose stream ranges overlap (2.5 GB
        // marked in 300 frames), and each readback made a new READBACK-heap buffer (none free until the fence): a
        // 2.4 s freeze in 'state'. The ranges are gathered and merged where they overlap or touch, then read back
        // once, at the end of the frame (or sooner, for a CPU-path texture load over them).
        AddExportRange(base, uint32_t(size));
      }
      ++stats_.memexport_ranges;
      stats_.memexport_bytes += size;
    }
    if (prepare_only) return;
    ++release_epoch_;  // vertex / index reuse memos keyed on the epoch must not hand out pre-export ring copies
  }

  // ---- Patch 0058: recorded pipelines (Canary pipeline_cache.cc storage: every pipeline description is written when
  // first made and all stored ones are created at title start, on all cores). Ours: <shader cache dir>\pipelines.list =
  // a header and fixed-size records - the guest shaders' ucode hashes (+ tessellation variant), whether the pixel
  // shader and the rect-list GS are used, and the description with its shader handles zeroed. No game data. At start
  // (after the shader warm-up) each record whose shaders are ready is queued as start-up backlog: the workers build it
  // (or load it from the saved library in ~1 ms) after anything a live draw needs. A record from another build that no
  // longer matches a draw costs one wasted build, nothing else. ----
  struct PipelineRecord {
    uint64_t vs_hash = 0, ps_hash = 0;
    uint32_t tess_n = 0, flags = 0;  // flags: 1 = pixel shader, 2 = rect-list geometry shader
    gpu::GraphicsPipelineDesc desc;  // vs / ps / gs = 0
  };
  static constexpr uint32_t kPipelineListMagic = 0x4C50474Eu;  // "NGPL"
  static constexpr uint32_t kPipelineListVersion = 1;          // bump when GraphicsPipelineDesc's meaning changes
  static uint64_t PipelineRecordKey(const PipelineRecord& r) {
    uint64_t h = gpu::HashPipelineDesc(r.desc);
    for (uint64_t v : {r.vs_hash, r.ps_hash, uint64_t(r.tess_n) << 32 | r.flags}) {
      h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return h;
  }
  void OpenPipelineList() {
    if (!REXCVAR_GET(gow2_renderer_pipeline_record) && !REXCVAR_GET(gow2_renderer_pipeline_precreate)) return;
    const auto t0 = std::chrono::steady_clock::now();
    const std::filesystem::path path = cache_->Dir() / "pipelines.list";
    std::vector<PipelineRecord> records;
    bool header_ok = false;
    {
      std::ifstream f(path, std::ios::binary);
      uint32_t hdr[3] = {};
      if (f && f.read(reinterpret_cast<char*>(hdr), sizeof(hdr)) && hdr[0] == kPipelineListMagic &&
          hdr[1] == kPipelineListVersion && hdr[2] == sizeof(PipelineRecord)) {
        header_ok = true;
        PipelineRecord r;
        while (f.read(reinterpret_cast<char*>(&r), sizeof(r))) {  // a torn last record (a hard exit) is ignored
          if (recorded_.insert(PipelineRecordKey(r)).second) records.push_back(r);
        }
      }
    }
    std::error_code ec;
    if (!header_ok && std::filesystem::exists(path, ec)) {  // another layout: moved aside, never deleted
      std::filesystem::path old = path;
      old += ".old";
      std::filesystem::rename(path, old, ec);
      REXLOG_INFO("gow2_renderer: recorded pipelines: {} has another layout - moved to {}", path.string(), old.string());
    }
    // Queue the recorded set (start-up backlog).
    uint64_t queued = 0, known = 0, no_shader = 0, no_gs = 0;
    if (REXCVAR_GET(gow2_renderer_pipeline_precreate) && !records.empty()) {
      backend_->SetPipelineBacklog(true);
      for (const PipelineRecord& r : records) {
        const rr::GuestShader* vsg = cache_->Find(r.vs_hash, true, r.tess_n);
        const rr::GuestShader* psg = (r.flags & 1) ? cache_->Find(r.ps_hash, false) : nullptr;
        if (!vsg || !vsg->ready() || ((r.flags & 1) && (!psg || !psg->ready()))) {
          ++no_shader;
          continue;
        }
        gpu::GraphicsPipelineDesc d = r.desc;
        d.vs = vsg->handle;
        d.ps = psg ? psg->handle : 0;
        d.gs = (r.flags & 2) ? (vsg->dxbc ? rect_gs_dxbc_ : rect_gs_) : 0;
        if ((r.flags & 2) && !d.gs) {
          ++no_gs;
          continue;
        }
        const uint64_t m = backend_->PipelineMisses();
        backend_->GetGraphicsPipeline(d, true, nullptr);
        if (backend_->PipelineMisses() != m) ++queued;
        else ++known;
      }
      backend_->SetPipelineBacklog(false);
    }
    if (REXCVAR_GET(gow2_renderer_pipeline_record)) {
      if (!header_ok) {
        std::ofstream header(path, std::ios::binary | std::ios::trunc);
        const uint32_t hdr[3] = {kPipelineListMagic, kPipelineListVersion, uint32_t(sizeof(PipelineRecord))};
        header.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
      }
      pipeline_list_ = std::make_unique<cogbox::io::AsyncFileAppender>(path);
      if (!pipeline_list_->Good()) pipeline_list_.reset();
    }

    REXLOG_INFO("gow2_renderer: recorded pipelines: {} in {} - {} queued to the workers as start-up backlog, {} "
                "already made, {} skipped (shader not ready{}), {} skipped (no GS) ({:.0f} ms){}",
                records.size(), path.string(), queued, known, no_shader, cache_->Dxbc() ? "" : " - HLSL path: still compiling",
                no_gs, MsSince(t0), pipeline_list_ ? "; recording new ones" : "");
    if (REXCVAR_GET(gow2_renderer_prepare_only)) PrepareAndExit(queued, records.size());
  }
  // Patch 0061 ("Preparing shaders"): wait for the start-up backlog, save the library, report, exit. The progress file
  // (one line: "<done> <total> <state> <seconds>") is what the launcher's progress window reads.
  [[noreturn]] void PrepareAndExit(uint64_t queued, size_t recorded) {
    const std::filesystem::path prog = cache_->Dir() / "prepare-progress.txt";
    const auto t0 = std::chrono::steady_clock::now();
    auto write = [&](uint64_t done, const char* state) {
      std::ofstream f(prog, std::ios::trunc);
      f << done << ' ' << queued << ' ' << state << ' ' << uint64_t(MsSince(t0) / 1000.0) << '\n';
    };
    REXLOG_INFO("gow2_renderer: preparing shaders: {} pipelines to build or load ({} recorded)", queued, recorded);
    for (uint64_t last = UINT64_MAX;;) {
      const uint64_t left = backend_->PipelineJobsLeft();
      const uint64_t done = queued > left ? queued - left : 0;
      if (done != last) write(done, "building");
      last = done;
      if (!left) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    write(queued, "saving");
    backend_->SavePipelinesNow();
    const gpu::BackendStats& bs = backend_->Stats();
    REXLOG_INFO("gow2_renderer: preparing shaders: done - {} pipelines in {:.1f} s ({} built by the driver, {:.0f} ms "
                "driver time in all; {} loaded from the saved library); library saved",
                queued, MsSince(t0) / 1000.0, bs.pipelines_built_async, bs.pipeline_async_ms, bs.pipelines_loaded_async);
    write(queued, "done");
    std::fflush(nullptr);
    std::_Exit(0);  // the title never starts; nothing else of the process needs a clean shutdown
  }
  void RecordPipeline(const gpu::GraphicsPipelineDesc& d, uint64_t vs_hash, uint64_t ps_hash, uint32_t tess_n) {
    PipelineRecord r;
    r.vs_hash = vs_hash;
    r.ps_hash = ps_hash;
    r.tess_n = tess_n;
    r.flags = (d.ps ? 1u : 0u) | (d.gs ? 2u : 0u);
    r.desc = d;
    r.desc.vs = r.desc.ps = r.desc.gs = 0;
    if (!recorded_.insert(PipelineRecordKey(r)).second) return;
    pipeline_list_->Append(&r, sizeof(r));
    ++stats_.pipelines_recorded;
  }

  // Patch 0051 (Gears 3 13:31: a no-cache start froze 22 s - 192 shaders compiled one after another on the draw
  // thread, ~170 ms each, while the compile threads sat idle): before a draw waits for its shader, the shaders of the
  // draws already queued behind it (the rest of this frame, as far as the PM4 processor has got) are looked up, so
  // the ones not compiled yet go to the compile threads at once and build in parallel with this wait. At most once
  // per 10 ms; already-known shaders cost a map lookup.
  void QueueUpcomingShaders() {
    if (!lookahead_shaders_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_lookahead_ < std::chrono::milliseconds(10)) return;
    last_lookahead_ = now;
    const uint64_t queued_before = cache_->Stats().queued;
    const uint32_t draws = XenosLookaheadUcode(
        [](void* ctx, const XenosUcode& v, const XenosUcode& p) {
          auto* self = static_cast<ShadowRenderer*>(ctx);
          if (v.dwords && v.count) self->cache_->Get(v.hash, v.dwords, v.count, true, 0);
          if (p.dwords && p.count) self->cache_->Get(p.hash, p.dwords, p.count, false);
        },
        this, 16384);
    ++stats_.lookaheads;
    stats_.lookahead_draws += draws;
    stats_.lookahead_queued += cache_->Stats().queued - queued_before;
  }

  static double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }

  // Patch 0045 (Edge RenderTargetCache::GetPipelineStandInWaitReason, Edge HEAD): why a draw must not be skipped while
  // its shaders / pipeline compile, or nullptr if skipping is harmless (a pass redrawn every frame). Called once per
  // draw; it records the draw target's frame and marks the vertex shader drawn.
  const char* StandInWaitReason(const rr::RenderTargetKey& t, bool has_target, const rr::GuestShader& vsg,
                                const rr::GuestShader* psg) {
    bool recurring = true;  // no target: nothing is kept
    if (has_target) {
      const uint64_t k = uint64_t(t.base_tiles) | (uint64_t(t.storage_class) << 12) | (uint64_t(t.pitch_pixels) << 24) |
                         (uint64_t(t.msaa) << 40);
      auto& [last, before] = standin_target_frames_[k];
      const uint64_t f = frame_ + 1;  // frame 0 is "never"
      if (last != f) {
        before = last;
        last = f;
      }
      recurring = before != 0 && f - before <= 4;  // a pass drawn every frame may still miss one (Edge)
    }
    const bool vs_drawn = vsg.drawn;
    vsg.drawn = true;
    // Small target: at most 2 tiles (160 pixels) wide at 32 bpp - impostors, lookup tables, generated data.
    const uint32_t width_samples = t.pitch_pixels << (t.msaa >= 2 ? 1u : 0u);
    if (has_target && width_samples <= 160) return "small render target";
    if (psg && psg->memexport) return "pixel memexport";
    if (vsg.memexport && !vs_drawn) return "new memexport VS";  // a recurring target says nothing about exports
    if (!recurring && !vs_drawn && REXCVAR_GET(gow2_renderer_standin_new_vs)) return "new VS, not drawn recently";  // 0132
    return nullptr;
  }

  // Shader pop-in: let a draw wait for its still-compiling shader, within this frame's budget.
  void WaitForShader(const rr::GuestShader& s) {
    if (s.state != rr::ShaderState::kPending || shader_wait_left_ms_ <= 0.0) return;
    QueueUpcomingShaders();  // patch 0051
    const auto t0 = std::chrono::steady_clock::now();
    const bool ready = cache_->WaitReady(s, shader_wait_left_ms_);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    shader_wait_left_ms_ -= ms;
    ++stats_.shader_waits;
    if (ready) ++stats_.shader_waits_ready;
    stats_.shader_wait_ms += ms;
  }

  void SkipForShaders(bool pending) {
    TraceDraw(pending ? "skipped: shader / pipeline pending" : "skipped: shader / pipeline failed");  // patch 0016
    if (pending) {
      ++stats_.pending_shaders;
      ++pending_this_frame_;
    } else {
      ++stats_.shader_errors;
      ++shader_errors_this_frame_;
    }
  }

  void ReportAliases() {
    for (const rr::AliasEvent& e : aliases_) {
      stats_.alias_tiles += e.tiles;
      ++stats_.alias_events;
      const uint64_t pair = (uint64_t(e.previous->id) << 32) | e.current->id;
      if (logged_alias_pairs_.size() < size_t(alias_log_limit_) && logged_alias_pairs_.insert(pair).second) {
        REXLOG_WARN("gow2_renderer: alias: frame {} draw {}: {} reuses {} tile(s) last written by {} - no transfer "
                    "(D29: logged, not converted)",
                    frame_ + 1, draw_index_, e.current->Describe(), e.tiles, e.previous->Describe());
      }
    }
  }

  void UploadVertexData(const uint32_t* r, const rr::GuestShader& s) {
    const uint8_t* base = memory_->TranslatePhysical<const uint8_t*>(0);
    // Job 175: with vertex reuse, a slot whose fetch constant is the same as at its last use in this command list and
    // release epoch gets the same result again - no map or watch lookups (same rule as the ring-copy reuse).
    if (vertex_reuse_) {
      const uint64_t list = backend_->Stats().submits;
      if (list != memo_list_ || release_epoch_ != memo_epoch_) {
        memo_list_ = list;
        memo_epoch_ = release_epoch_;
        ++memo_gen_;
      }
    }
    for (uint32_t i = 0; i < 96; ++i) {
      if (!((s.vfetch_constants[i >> 5] >> (i & 31)) & 1)) continue;
      const uint32_t dw0 = r[kRegFetchConst + 2 * i], dw1 = r[kRegFetchConst + 2 * i + 1];
      // Patch 0049 (Canary b083312 + gpu_allow_invalid_fetch_constants, default true): a vertex fetch constant with
      // another type is still uploaded (its draw's shader reads it either way; before, its bytes were never made
      // current - missing character models in 5454086C / 425607FE on Canary).
      if ((dw0 & 3) != 3) {
        if (!vertex_any_type_) continue;  // not a vertex fetch constant (kVertex = 3), skipped as before
        ++stats_.vertex_other_type;
      }
      VertexMemo& m = vertex_memo_[i];
      if (vertex_reuse_ && m.gen == memo_gen_ && m.dw0 == dw0 && m.dw1 == dw1) {
        fetch_[2 * i] = m.out0;
        fetch_[2 * i + 1] = m.out1;
        ++stats_.vertex_memo_hits;
        continue;
      }
      UploadVertexSlot(base, i, dw0, dw1);
      if (vertex_reuse_) {
        if (backend_->Stats().submits != memo_list_) {  // the ring filled and a new list began inside this upload
          memo_list_ = backend_->Stats().submits;
          ++memo_gen_;
        }
        m = {memo_gen_, dw0, dw1, fetch_[2 * i], fetch_[2 * i + 1]};
      }
    }
  }

  void UploadVertexSlot(const uint8_t* base, uint32_t i, uint32_t dw0, uint32_t dw1) {
    {
      const uint32_t address = dw0 & 0x1FFFFFFCu;
      uint64_t size = uint64_t((dw1 >> 2) & 0xFFFFFFu) * 4;
      fetch_[2 * i + 1] &= 0x7FFFFFFFu;  // patch 0029 (B1): cleared before any early return
      if (!size) return;
      if (log_vs_hash_ && vs_.hash == log_vs_hash_) LogVertexBytes(base, i, address, size, dw1 & 3u);
      if (address + size > (512ull << 20)) size = (512ull << 20) - address;
      size = std::min<uint64_t>(size, 64ull << 20);
      fetch_[2 * i + 1] &= 0x7FFFFFFFu;  // bit 31 is ours (t97); the hardware ignores it
      // notes/11: a range holding memexport results (GPU-current pages) is read from the mirror, never from the ring or
      // a CPU copy over them: only its CPU-current pages are copied in.
      if (exports_marked_) {
        constexpr uint32_t kShift = rr::GuestWatch::kPageShift;
        const uint32_t first = address >> kShift, last = uint32_t((address + size - 1) >> kShift);
        // Patch 0007: the page spans of readbacks still in flight that overlap this range. Their pages hold export
        // results only in the mirror until PollExportReadbacks lands them; a CPU store anywhere in the same 4 MB block
        // ends GpuCurrent() for them (guest_watch.cpp: the whole block is bumped), and uploading guest memory then
        // would put the PRE-export bytes over the export's results.
        pending_spans_.clear();
        if (pending_mirror_) {
          for (const ExportReadback& e : export_readbacks_) {
            const uint32_t a = e.base >> kShift, b = (e.base + e.size - 1) >> kShift;
            if (a <= last && b >= first) pending_spans_.push_back({std::max(a, first), std::min(b, last)});
          }
          for (const auto& [ra, re] : export_ranges_) {  // patch 0042: gathered, readback not issued yet
            const uint32_t a = uint32_t(ra >> kShift), b = uint32_t((re - 1) >> kShift);
            if (a <= last && b >= first) pending_spans_.push_back({std::max(a, first), std::min(b, last)});
          }
        }
        auto keep = [&](uint32_t p) {
          if (watch_->GpuCurrent(p)) return true;
          for (const auto& [a, b] : pending_spans_)
            if (p >= a && p <= b) {
              ++stats_.vertex_pages_kept_pending;
              return true;
            }
          return false;
        };
        bool any_gpu = false;
        for (uint32_t p = first; p <= last && !any_gpu; ++p) any_gpu = keep(p);
        if (any_gpu) {
          uint32_t run = UINT32_MAX;
          for (uint32_t p = first; p <= last + 1; ++p) {
            const bool cpu = p <= last && !keep(p);
            if (cpu && run == UINT32_MAX) run = p;
            if (!cpu && run != UINT32_MAX) {
              const uint32_t a = std::max(run << kShift, address);
              const uint32_t b = uint32_t(std::min<uint64_t>(uint64_t(p) << kShift, address + size));
              if (b > a) backend_->UploadGuestMemory(a, base + a, b - a);
              run = UINT32_MAX;
            }
          }
          ++stats_.vertex_from_exports;
          if (export_reader_seen_.size() < 96 && export_reader_seen_.insert(vs_.hash ^ (uint64_t(i) << 56)).second) {
            uint32_t gpu_pages = 0, pages = 0;
            for (uint32_t p = first; p <= last; ++p, ++pages) gpu_pages += watch_->GpuCurrent(p) ? 1u : 0u;
            REXLOG_INFO("gow2_renderer: export reader frame {} draw {}: vs {:016X} vf{} {:08X} size {} endian {} | {} of "
                        "{} pages GPU-current (patch 0012)",
                        frame_ + 1, draw_index_, vs_.hash, i, address, size, dw1 & 3u, gpu_pages, pages);
          }
          return;
        }
      }
      // Small ranges (rewritten every frame): straight from the upload ring, no copy into the mirror (job 160).
      if (vertex_direct_ && !(size >= kWatchMinBytes && upload_watch_)) {
        // Job 174 reuse: the same range again in the same command list and release epoch reuses its ring copy (valid for
        // every draw in that list). Cleanup 2 (patch 0050): job 170's hash-checked dedupe, never made the default, is gone.
        uint32_t off = UINT32_MAX;
        uint64_t key = 0, hash = 0;
        if (vertex_reuse_) {
          // Job 174: same range, same release epoch, same command list -> the ring copy is still what the guest holds.
          const uint64_t list = backend_->Stats().submits;
          if (list != transient_list_ || release_epoch_ != transient_epoch_) {
            transient_reuse_.clear();
            transient_list_ = list;
            transient_epoch_ = release_epoch_;
          }
          key = (uint64_t(address) << 32) | uint32_t(size);
          if (auto it = transient_reuse_.find(key); it != transient_reuse_.end()) {
            off = it->second.second;
            stats_.vertex_reused_bytes += size;
          }
        }
        if (off == UINT32_MAX) {
          off = backend_->UploadTransient(base + address, uint32_t(size));
          if (vertex_reuse_ && off != UINT32_MAX) transient_reuse_[key] = {hash, off};
        }
        if (off != UINT32_MAX) {
          fetch_[2 * i] = off | 3u;  // 16-byte aligned offset in the address field, type kVertex
          fetch_[2 * i + 1] |= 0x80000000u;
          stats_.vertex_bytes += size;
          stats_.vertex_direct_bytes += size;
          return;
        }
      }
      // Ranges of 64 KB and more are watched (GuestWatch: CPU writes and kernel reads reported by the guest memory
      // system) and copied again only when written since; smaller ones (dynamic buffers rewritten every frame) are
      // copied on every draw, where watching would cost more than the copy.
      if (size >= kWatchMinBytes && upload_watch_) {
        const uint64_t key = (uint64_t(address) << 32) | uint32_t(size);
        auto it = vertex_versions_.find(key);
        if (it != vertex_versions_.end() && it->second.version == watch_->Version(address, uint32_t(size))) {
          it->second.last_frame = frame_;  // patch 0149
          stats_.vertex_skipped_bytes += size;
          return;
        }
        watch_->Watch(address, uint32_t(size));  // before the copy: a write during it bumps the version
        WatchedRange& w = vertex_versions_[key];
        w.version = watch_->Version(address, uint32_t(size));
        w.last_frame = frame_;  // patch 0149 (the XXH3 of the range here was never read: removed)
      }
      backend_->UploadGuestMemory(address, base + address, uint32_t(size));
      stats_.vertex_bytes += size;
    }
  }

  // Patch 0015: the bytes this draw's vertex fetch reads, now (what the upload copies), as up to 24 floats.
  void LogVertexBytes(const uint8_t* base, uint32_t slot, uint32_t address, uint64_t size, uint32_t endian) {
    if (log_vs_logged_ >= log_vs_max_ || address + size > (512ull << 20)) return;
    ++log_vs_logged_;
    const uint32_t words = uint32_t(std::min<uint64_t>(size / 4, 24));
    std::string text;
    for (uint32_t k = 0; k < words; ++k) {
      uint32_t w;
      std::memcpy(&w, base + address + 4 * k, 4);
      w = Swap32(w, endian);
      text += fmt::format("{}{:.6g}", k % 8 ? " " : (k ? " | " : ""), std::bit_cast<float>(w));
    }
    uint32_t gpu_pages = 0;
    for (uint32_t p = address >> rr::GuestWatch::kPageShift;
         p <= uint32_t((address + size - 1) >> rr::GuestWatch::kPageShift); ++p)
      gpu_pages += watch_->GpuCurrent(p) ? 1u : 0u;
    REXLOG_INFO("gow2_renderer: vs-log frame {} draw {}: vs {:016X} vf{} {:08X} size {} endian {} gpu-current pages {} "
                "| vertex words as floats: {}",
                frame_ + 1, draw_index_, vs_.hash, slot, address, size, endian, gpu_pages, text);
  }


  bool ReadIndices(const ObservedIndexBuffer& ib, uint32_t count, uint32_t reset_index, bool use_restart) {
    const bool is32 = ib.format == gx::IndexFormat::kInt32;
    const uint32_t endian = uint32_t(ib.endianness);
    const uint64_t bytes = uint64_t(count) * (is32 ? 4 : 2);
    const uint32_t address = ib.guest_base & 0x1FFFFFFFu;
    if (!count || address + bytes + 4 > (512ull << 20)) return false;
    const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(address);
    stats_.index_bytes += bytes;
    // Big index buffers: converted once and kept while the guest bytes are unchanged (GuestWatch).
    CachedIndices* cached = nullptr;
    if (upload_watch_ && bytes >= kWatchMinBytes) {
      uint64_t key = (uint64_t(address) << 32) | count;
      key ^= (uint64_t(is32) << 1 | uint64_t(endian) << 2 | uint64_t(use_restart) << 4) * 0x9E3779B97F4A7C15ull;
      key ^= uint64_t(use_restart ? reset_index : 0) * 0xC2B2AE3D27D4EB4Full;
      cached = &index_cache_[key];
      cached->last_frame = frame_;  // patch 0149: aged out by EvictStaleUploads
      if (!cached->indices.empty() && cached->version == watch_->Version(address, uint32_t(bytes))) {
        stats_.index_skipped_bytes += bytes;
        index_ptr_ = &cached->indices;  // no copy (upl-n102)
        cached_ib_ = cached;
        return true;
      }
      if (cached->gpu) {  // the guest bytes changed: the persistent copy is stale
        backend_->ReleaseIndexBuffer(cached->gpu);
        cached->gpu = 0;
      }
      watch_->Watch(address, uint32_t(bytes));
      cached->version = watch_->Version(address, uint32_t(bytes));  // patch 0149: its unread XXH3 removed
    }
    indices_.resize(count);
    ConvertIndices(src, is32, endian, count, use_restart, reset_index, indices_.data());
    if (cached) {
      cached->indices.swap(indices_);
      index_ptr_ = &cached->indices;
      cached_ib_ = cached;
    } else {
      index_ptr_ = &indices_;
    }
    return true;
  }

  // ---------------------------------------------------------------------------------------------------------------
  // Resolves (EDRAM copies)
  // ---------------------------------------------------------------------------------------------------------------
  // GetScissor(regs, clamp_to_surface_pitch = false) of the SDK's draw_util (BSD, Xenia) - what GetResolveInfo uses.
  static void ResolveScissor(const uint32_t* r, ::xenos::resolve::Regs& out) {
    const uint32_t tl = r[rr::reg::PA_SC_WINDOW_SCISSOR_TL], br = r[rr::reg::PA_SC_WINDOW_SCISSOR_BR];
    int32_t tl_x = int32_t(Bits(tl, 0, 14)), tl_y = int32_t(Bits(tl, 16, 14));
    int32_t br_x = int32_t(Bits(br, 0, 14)), br_y = int32_t(Bits(br, 16, 14));
    if (!Bits(tl, 31, 1)) {
      const uint32_t wo = r[rr::reg::PA_SC_WINDOW_OFFSET];
      const int32_t ox = SignExtend(Bits(wo, 0, 15), 15), oy = SignExtend(Bits(wo, 16, 15), 15);
      tl_x += ox;
      tl_y += oy;
      br_x += ox;
      br_y += oy;
    }
    const uint32_t stl = r[rr::reg::PA_SC_SCREEN_SCISSOR_TL], sbr = r[rr::reg::PA_SC_SCREEN_SCISSOR_BR];
    tl_x = std::max(tl_x, SignExtend(Bits(stl, 0, 15), 15));
    tl_y = std::max(tl_y, SignExtend(Bits(stl, 16, 15), 15));
    br_x = std::min(br_x, SignExtend(Bits(sbr, 0, 15), 15));
    br_y = std::min(br_y, SignExtend(Bits(sbr, 16, 15), 15));
    tl_x = std::max(tl_x, 0);
    tl_y = std::max(tl_y, 0);
    br_x = std::max(br_x, tl_x);
    br_y = std::max(br_y, tl_y);
    out.scissor_x = tl_x;
    out.scissor_y = tl_y;
    out.scissor_w = br_x - tl_x;
    out.scissor_h = br_y - tl_y;
  }

  void Resolve(const uint32_t* r) {
    ++tex_gen_;  // job 181: a resolve can change what a texture holds
    ++stats_.copies;
    const uint32_t copy = copy_index_++;
    if (trace_on_) trace_ctx_ = "frame " + std::to_string(frame_ + 1) + " copy " + std::to_string(copy) + " (resolve)";
    const uint32_t first_draw = last_copy_draw_, last_draw = draw_index_;
    const uint32_t drawn = drawn_since_copy_;
    last_copy_draw_ = draw_index_;
    drawn_since_copy_ = 0;
    // Patch 0149: the "not drawn since the previous copy" text built here on every resolve (plus a full stats copy)
    // was never read: removed.

    ::xenos::resolve::Regs rg;
    // The rectangle: three vertices at vertex fetch constant 0 (draw_util GetResolveInfo).
    const uint32_t f0 = r[kRegFetchConst], f1 = r[kRegFetchConst + 1];
    if ((f0 & 3) != 3 || ((f1 >> 2) & 0xFFFFFFu) != 6) {
      Skip("resolve: vertex fetch constant 0 is not 3 vertices");
      ++resolve_early_["resolve: vertex fetch constant 0 is not 3 vertices"];
      return;
    }
    const uint8_t* vtx = memory_->TranslatePhysical<const uint8_t*>(f0 & 0x1FFFFFFCu);
    for (int i = 0; i < 6; ++i) {
      uint32_t u;
      std::memcpy(&u, vtx + 4 * i, 4);
      rg.vertices[i] = std::bit_cast<float>(Swap32(u, f1 & 3));
    }
    rg.pix_center_d3d_zero = Bits(r[rr::reg::PA_SU_VTX_CNTL], 0, 1) == 0;
    rg.vtx_window_offset_enable = Bits(r[rr::reg::PA_SU_SC_MODE_CNTL], 16, 1) != 0;
    const uint32_t wo = r[rr::reg::PA_SC_WINDOW_OFFSET];
    rg.window_x_offset = SignExtend(Bits(wo, 0, 15), 15);
    rg.window_y_offset = SignExtend(Bits(wo, 16, 15), 15);
    ResolveScissor(r, rg);
    const uint32_t si = r[rr::reg::RB_SURFACE_INFO];
    rg.surface_pitch = Bits(si, 0, 14);
    rg.msaa = Bits(si, 16, 2);
    const uint32_t cc = r[kRegCopyControl];
    rg.copy_src_select = Bits(cc, 0, 3);
    rg.copy_sample_select = Bits(cc, 4, 3);
    rg.copy_command = Bits(cc, 20, 2);
    rg.depth_clear_enable = Bits(cc, 9, 1) != 0;
    const uint32_t ci = r[rr::reg::kColorInfo[rg.copy_src_select & 3]];
    rg.color_base = Bits(ci, 0, 12) & 2047u;
    rg.color_format = Bits(ci, 16, 4);
    const uint32_t di = r[rr::reg::RB_DEPTH_INFO];
    rg.depth_base = Bits(di, 0, 12) & 2047u;
    rg.depth_format = Bits(di, 16, 1);
    rg.copy_dest_base = r[kRegCopyDestBase];
    const uint32_t dp = r[kRegCopyDestPitch], dinfo = r[kRegCopyDestInfo];
    rg.copy_dest_pitch = Bits(dp, 0, 14);
    rg.copy_dest_height = Bits(dp, 16, 14);
    rg.copy_dest_array = Bits(dinfo, 3, 1) != 0;
    rg.copy_dest_slice = Bits(dinfo, 4, 3);
    rg.copy_dest_format = Bits(dinfo, 7, 6);
    rg.copy_surface_slice = r[kRegCopySurfaceSlice];
    const ::xenos::resolve::Info info = ::xenos::resolve::GetInfo(rg);
    if (!info.valid) {
      Skip("resolve: not supported by GetResolveInfo rules");
      ++resolve_early_["resolve: not supported by GetResolveInfo rules"];
      return;
    }
    // The emulator writes the destination (to guest memory with --readback_resolve): textures and vertex ranges there
    // are stale for us from now on.
    if (info.dest_extent_length) watch_->Bump(info.dest_extent_start, info.dest_extent_length);
    if (ProbeFrame() && probe_resolves_.size() < 64)
      probe_resolves_.push_back(std::format("resolve {} after draw {}: {} {}x{} colour fmt {} base {} -> dest {:08X} fmt {} "
                                            "exp {}", copy, draw_index_, info.is_depth ? "depth" : "colour",
                                            info.width_div_8 * 8, info.height_div_8 * 8, rg.color_format, rg.color_base,
                                            info.dest_base, info.dest_format, SignExtend(Bits(r[kRegCopyDestInfo], 16, 6), 6)));
    if (scaled_resolves_ && frame_ + 1 == 901 && REXCVAR_GET(gow2_renderer_scaled_miss_log))  // job 192 (0029: diag only)
      REXLOG_INFO("gow2_renderer: frame 901 copy {}: dest {:08X} fmt {} extent {:08X}+{:X} depth {} msaa {} colour fmt {}",
                  copy, info.dest_base, info.dest_format, info.dest_extent_start, info.dest_extent_length,
                  info.is_depth ? 1 : 0, rg.msaa, rg.color_format);
    if (!info.width_div_8 || !info.height_div_8) {
      Skip("resolve: empty rectangle");
      ++resolve_early_["resolve: empty rectangle"];
      return;
    }
    const rr::RenderTargetKey depth_key{rg.depth_base, rr::DepthStorageClass(rg.depth_format), rg.surface_pitch,
                                        rg.msaa};
    rr::RenderTarget* depth_rt = rts_->FindAny(depth_key);  // patch 0141 review: either phase's twin
    if (info.is_depth) {
      ++stats_.depth_resolves;
      ++t2_stats_.seen_depth;
      ResolveDepth(info, rg, depth_rt, copy, first_draw, last_draw, drawn, Bits(dinfo, 0, 3));
    } else {
      ++stats_.colour_resolves;
      ++t2_stats_.seen_colour;
      if (draw_colour_) ResolveColour(info, rg, copy, first_draw, last_draw, drawn, dinfo);
      else ++resolve_early_["colour drawing off"];
    }
    // Clears after the copy (same 8-aligned rectangle).
    if (Bits(r[kRegCopyControl], 8, 1) && draw_colour_ && !info.is_depth) ColourResolveClear(r, rg, info);
    const bool flip_after = up_flip_after_resolve_;  // patch 0141: flipped after this packet's own clears
    if (rg.depth_clear_enable && depth_rt && depth_rt->texture) {
      const uint32_t v = r[kRegDepthClear];
      const float depth = rg.depth_format ? HostDepth(*depth_rt, ::xenos::formats::Float20e4To32(v >> 8))
                                          : ::xenos::edram::UNorm24To32(v >> 8);
      const rr::PixelRect host = HostRect(*depth_rt, info.x0, info.y0, info.x1, info.y1);
      const gpu::Rect rect = ToGpuRect(host);
      // The clear writes depth and stencil of the rectangle and takes those tiles (both rules); tiles it covers only
      // in part first get their old content, nothing is transferred under the cleared part (Canary ~5361-5381).
      PrepareDepthTarget(*depth_rt, OwnershipForRect(host), true, true);
      backend_->ClearDepthStencil(depth_rt->texture, true, depth, true, uint8_t(v & 0xFF), &rect, 1);
      Wrote(*depth_rt);
      stencil_ones_clears_ |= uint8_t(v & 0xFF);
      if (trace_on_) {
        char b[96];
        std::snprintf(b, sizeof(b), "resolve clear to %08X (rect %d,%d-%d,%d)", v, info.x0, info.y0, info.x1, info.y1);
        TraceEvent(*depth_rt, {uint32_t(std::max(info.x0, 0)), uint32_t(std::max(info.y0, 0)),
                               uint32_t(std::max(info.x1, 0)), uint32_t(std::max(info.y1, 0))}, b);
      }
    }
    if (flip_after) {  // patch 0141: post-processing, HUD and the front buffer at the output scale
      up_flip_after_resolve_ = false;
      rts_->SetPhaseScale(res_scale_);
    }
  }

  gpu::TextureHandle DestinationTexture(const ::xenos::resolve::Info& info, uint32_t words = 1) {
    // Host textures for resolve destinations, keyed by (address, format, size) - one guest address is reused for
    // several formats and sizes in one frame (notes/35).
    const uint32_t w = info.dest_pitch_aligned_div_32 * 32, h = info.dest_height_aligned_div_32 * 32;
    const std::string key = std::to_string(info.dest_base) + "/" + std::to_string(info.dest_format) + "/" +
                            std::to_string(w) + "x" + std::to_string(h);
    auto it = destinations_.find(key);
    if (it != destinations_.end()) {
      it->second.last_used = frame_;
      return it->second.texture;
    }
    gpu::TextureDesc d;
    d.width = std::max<uint32_t>(w, 32) * words;  // 64bpp destinations: two R32 words per pixel
    d.height = std::max<uint32_t>(h, 32);
    d.format = gpu::Format::kR32Uint;
    d.usage = gpu::kUsageStorage | gpu::kUsageSampled | (resolve_draws_ ? gpu::kUsageRenderTarget : 0u);  // 0119
    d.debug_name = "gow2 resolve destination";
    gpu::TextureHandle t = 0;
    if (REXCVAR_GET(gow2_renderer_resolve_reuse)) {
      auto reuse = cogbox::renderer::ReusableResolve(destinations_, frame_, d.width, d.height,
          [&](const DestEntry& value) {
            if (front_bases_.count(value.base)) return std::pair<uint32_t,uint32_t>{0,0};
            return std::pair{value.width, value.height};
          });
      if (reuse != destinations_.end()) {
        t = reuse->second.texture;
        destinations_.erase(reuse);
        ++dest_evict_.reused_one;
      }
    }
    if (!t) t = backend_->CreateTexture(d);
    if (t) destinations_.emplace(key, DestEntry{t, frame_, uint64_t(d.width) * d.height * 4, d.width, d.height, info.dest_base});
    return t;
  }

  // Patch 0032: resolve destination textures were never freed. Gears 3 keeps resolving to new (address, format, size)
  // keys, so they piled up (424 -> 548 in one window) until video memory was over-committed and every GPU job got
  // ~10x slower about 5 minutes in. A destination only has to hold the rectangle of the resolve that just wrote it
  // (the guest-memory copy and the presenter read it in the same frame), so one not used for a while can be freed
  // and made again. DestroyTexture releases after the GPU is done with it (fence).
  struct DestEntry {
    gpu::TextureHandle texture = 0;
    uint64_t last_used = 0, bytes = 0;
    uint32_t width = 0, height = 0, base = 0;
  };
  struct DestEvictStats {
    uint64_t count = 0, bytes = 0, by_age = 0, by_budget = 0, pressure_events = 0;  // pressure: patch 0033
    uint64_t reused_one = 0, reused_scaled = 0;
  };
  DestEvictStats dest_evict_;
  // Patch 0149 (deep-dive review): the watched vertex ranges and converted index buffers (with their persistent GPU
  // copies) were never freed - every level's buffers stayed for the whole session. Entries unused for
  // kStaleUploadFrames are dropped; one used again is simply converted / copied again.
  static constexpr uint64_t kStaleUploadFrames = 3600;
  void EvictStaleUploads() {
    if (frame_ < kStaleUploadFrames) return;
    const uint64_t cut = frame_ - kStaleUploadFrames;
    uint64_t indices = 0, ranges = 0, bytes = 0;
    for (auto it = index_cache_.begin(); it != index_cache_.end();) {
      if (it->second.last_frame < cut) {
        if (it->second.gpu) backend_->ReleaseIndexBuffer(it->second.gpu);  // deferred to its fence by the backend
        bytes += it->second.indices.size() * 4;
        it = index_cache_.erase(it);
        ++indices;
      } else {
        ++it;
      }
    }
    for (auto it = vertex_versions_.begin(); it != vertex_versions_.end();) {
      if (it->second.last_frame < cut) {
        it = vertex_versions_.erase(it);
        ++ranges;
      } else {
        ++it;
      }
    }
    cached_ib_ = nullptr;
    index_ptr_ = &indices_;  // it may point into an erased entry (review of 0149)
    if (indices || ranges)
      REXLOG_INFO("gow2_renderer: stale uploads dropped at frame {}: {} index buffer(s) ({:.1f} MB), {} watched vertex "
                  "range(s); kept {} / {}", frame_, indices, double(bytes) / 1048576.0, ranges, index_cache_.size(),
                  vertex_versions_.size());
  }
  void EvictDestinations() {
    const int32_t age = REXCVAR_GET(gow2_renderer_dest_evict_frames);
    const int32_t budget_mb = REXCVAR_GET(gow2_renderer_dest_budget_mb);
    if (age <= 0 && budget_mb == 0) return;
    if (age > 0 && frame_ % 30 != 0 && budget_mb <= 0) return;
    auto free_tex = [&](gpu::TextureHandle t, uint64_t bytes) {
      if (t) backend_->DestroyTexture(t);
      ++dest_evict_.count;
      dest_evict_.bytes += bytes;
    };
    const uint64_t keep_after = frame_ > 30 ? frame_ - 30 : 0;  // used in the last 30 frames: kept (no churn)
    if (age > 0 && frame_ % 30 == 0) {  // by age, checked twice a second
      for (auto it = destinations_.begin(); it != destinations_.end();) {
        if (frame_ - it->second.last_used > uint64_t(age)) {
          free_tex(it->second.texture, it->second.bytes);
          ++dest_evict_.by_age;
          it = destinations_.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = scaled_dests_.begin(); it != scaled_dests_.end();) {
        if (frame_ - it->second.last_used > uint64_t(age)) {
          free_tex(it->second.texture, it->second.bytes);
          ++dest_evict_.by_age;
          it = scaled_dests_.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (budget_mb == 0) return;
    uint64_t total = 0;
    for (const auto& [k, d] : destinations_) total += d.bytes;
    for (const auto& [k, d] : scaled_dests_) total += d.bytes;
    uint64_t budget = 0;
    if (budget_mb > 0) {
      budget = uint64_t(budget_mb) << 20;
    } else {  // patch 0033: by video-memory pressure, checked twice a second
      if (frame_ % 30 != 0) return;
      uint64_t used = 0, os_budget = 0;
      if (!backend_->VideoMemory(&used, &os_budget) || os_budget == 0) return;
      if (used * 10 <= os_budget * 9) return;
      const uint64_t over = used - os_budget * 8 / 10;
      budget = total > over ? total - over : 0;
      ++dest_evict_.pressure_events;
    }
    if (total <= budget) return;
    // Least recently used first: (last_used, scaled?, key).
    std::vector<std::tuple<uint64_t, int, std::string>> order;
    for (const auto& [k, d] : destinations_)
      if (d.last_used < keep_after) order.emplace_back(d.last_used, 0, k);
    for (const auto& [k, d] : scaled_dests_)
      if (d.last_used < keep_after) order.emplace_back(d.last_used, 1, k);
    std::sort(order.begin(), order.end());
    for (const auto& [used, scaled, k] : order) {
      if (total <= budget) break;
      uint64_t bytes = 0;
      if (scaled) {
        auto it = scaled_dests_.find(k);
        bytes = it->second.bytes;
        free_tex(it->second.texture, bytes);
        scaled_dests_.erase(it);
      } else {
        auto it = destinations_.find(k);
        bytes = it->second.bytes;
        free_tex(it->second.texture, bytes);
        destinations_.erase(it);
      }
      ++dest_evict_.by_budget;
      total -= bytes;
    }
  }

  void ResolveDepth(const ::xenos::resolve::Info& info, const ::xenos::resolve::Regs& rg, rr::RenderTarget* rt,
                    uint32_t copy, uint32_t first_draw, uint32_t last_draw, uint32_t drawn, uint32_t endian) {
    const uint32_t w = info.width_div_8 * 8, h = info.height_div_8 * 8;
    char source[160];
    std::snprintf(source, sizeof(source), "depth %s base %u pitch %u msaa %ux %ux%u at (%d,%d) -> %08X fmt %u",
                  DepthFormatName(rg.depth_format), rg.depth_base, rg.surface_pitch, 1u << rg.msaa, w, h, info.x0,
                  info.y0, info.dest_base, info.dest_format);
    auto log_not_compared = [&](const char* why) {
      ++resolve_early_[why];  // patch 0030: per-window count of resolves that end here (Gears 3's squad lead)
    };
    if (!rt || !rt->texture) return log_not_compared("we never drew into that depth target");
    if (rt->key.msaa) return log_not_compared("MSAA depth resolves are not in M1");
    if (!depth_resolve_) return log_not_compared("depth resolve shader missing");
    if (!info.dest_supported || info.dest_bpp_log2 != 2) return log_not_compared("destination format");
    if (endian > 3) return log_not_compared("64/128-bit endian swap on a 32-bit destination");
    // Tiles the resolve reads that another target wrote last: their data comes in, ownership stays (a read).
    PrepareDepthTarget(*rt, OwnershipForRect(HostRect(*rt, info.x0, info.y0, info.x1, info.y1)), false, false, true);
    const gpu::TextureHandle dest = DestinationTexture(info);
    if (!dest) return log_not_compared("destination texture");
    const uint32_t dx = info.dest_offset_x_div_8 * 8, dy = info.dest_offset_y_div_8 * 8;
    if (dx + w > std::max<uint32_t>(info.dest_pitch_aligned_div_32 * 32, 32) ||
        dy + h > std::max<uint32_t>(info.dest_height_aligned_div_32 * 32, 32))
      return log_not_compared("rectangle extends past the destination texture");
    gpu::ComputeBindings b;
    b.constants[0] = uint32_t(info.x0);
    b.constants[1] = uint32_t(info.y0);
    b.constants[2] = w;
    b.constants[3] = h;
    b.constants[4] = dx;
    b.constants[5] = dy;
    b.constants[6] = DepthFlags(rt->key.storage_class);
    b.constants[12] = rt->scale;  // Stage 7: source pixel = guest pixel x scale (one host sample per guest pixel)
    // Upscalers (Gears 3 job 058): the frame's scene depth, drawn with this frame's jitter, is resolved at the
    // unjittered centres - the same shift the scene draws got (UpscaleJitter: the viewport moves by up_jitter_ host
    // pixels, after --gow2_upscale_jitter_scale). Other depth targets (shadow maps) and frames without jitter: as before.
    if (upscaler_ && up_dejitter_depth_ && up_last_ok_ && up_camera_ && rt->key == up_depth_key_ &&
        (up_jitter_.x != 0.0f || up_jitter_.y != 0.0f)) {
      // c1.w and c2.xy: the only constants no depth-resolve path uses (the fused path sets c2.zw / c3.zw).
      b.constants[7] = std::bit_cast<uint32_t>(up_depth_a_);
      const float jk = up_depth_scale_ ? float(rt->scale) / float(up_depth_scale_) : 1.0f;  // 0141: output twin
      b.constants[8] = std::bit_cast<uint32_t>(up_jitter_.x * jk);
      b.constants[9] = std::bit_cast<uint32_t>(up_jitter_.y * jk);
      ++up_dejittered_;
    }
    b.constant_count = 16;
    b.srv[0] = rt->texture;
    b.srv_plane[0] = 0;
    b.srv[1] = rt->texture;
    b.srv_plane[1] = 1;
    b.uav_texture = dest;
    ResolveDispatch(depth_resolve_, b, (w + 7) / 8, (h + 7) / 8, 1);
    bool scaled = false;
    rr::TextureCache::ResolveThrough through;
    // Per-target scale: a 1x target (below the threshold) writes only the 1x copy, so its pages stop being
    // scaled-current and textures load them at 1x (the reference's 1x1 resolves do the same).
    // Patch 0143 (upscaling modes): a scene-scale depth target above 1x also fills the output-scale copy (nearest).
    const uint32_t cross = up_scale_in_ > 1 && rt->scale == up_scale_in_ ? rt->scale : 0u;
    if (cross) b.constants[12] = res_scale_;
    if (scaled_resolves_ && (rt->scale == res_scale_ || cross) && fused_scaled_ && (fused_kinds_ & 2u) &&
        rr::FuseResolveSize(w, h, uint32_t(std::max(0, REXCVAR_GET(gow2_renderer_fused_scaled_min_pixels))))) {
      scaled = FusedScaledResolve(depth_resolve_, b, info, w, h, 1, endian, cross);
    }
    if (!scaled && scaled_resolves_ && (rt->scale == res_scale_ || cross)) {
      if (ScaledDest* sd = ScaledDestination(info, 1)) {
        b.constants[13] = 1u | (cross << 8);  // patch 0143
        b.uav_texture = sd->texture;
        ResolveDispatch(depth_resolve_, b, (w * res_scale_ + 7) / 8, (h * res_scale_ + 7) / 8, 1);
        scaled = ScaledToMemory(*sd, info, dx, dy, w, h, endian, through);
      }
    }
    WriteResolveToGuest(dest, info, endian, 1, dx, dy, w, h, scaled, &through);
    if (trace_on_)
      TraceEvent(*rt, {uint32_t(std::max(info.x0, 0)), uint32_t(std::max(info.y0, 0)), uint32_t(std::max(info.x1, 0)),
                       uint32_t(std::max(info.y1, 0))},
                 "resolved (read by the copy)");
  }

  // Colour resolve-clear (M2 build B, notes/45 6, SDK value order CLEAR | CLEAR_LO << 32): the rectangle's tiles are
  // taken (partly covered ones first get their old content), then cleared to the host value of the guest bits.
  void ColourResolveClear(const uint32_t* r, const ::xenos::resolve::Regs& rg, const ::xenos::resolve::Info& info) {
    const uint32_t cls = rr::ColorStorageClass(rg.color_format);
    rr::RenderTarget* rt = rts_->FindAny({rg.color_base, cls, rg.surface_pitch, rg.msaa});  // patch 0141 review
    if (!rt || !rt->texture || rg.msaa || rr::ColorHostFormat(cls) == gpu::Format::kUnknown) return;
    namespace col = ::xenos::colour;
    const bool is64 = rr::RenderTargetCache::Is64bpp(*rt);
    const uint64_t v = col::ColourClearValue(is64, r[kRegColorClear], r[kRegColorClearLo], col::Reference::kCanary);  // patch 0039 (map group 2): Canary's 64bpp order, CLEAR_LO | CLEAR << 32
    const col::HostColourClear hc = col::HostColourClearValue(col::ColorFormat(cls), v);
    float rgba[4];
    for (uint32_t i = 0; i < 4; ++i) rgba[i] = hc.rgba[i];
    if (hc.through_uint_view) {
      // Raw guest bits; our views are typed, so give the value that stores exactly these bits.
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t bits = uint32_t(hc.rgba[i]);
        if (cls == uint32_t(::xenos::edram::ColorFormat::k_16_16_FLOAT) ||
            cls == uint32_t(::xenos::edram::ColorFormat::k_16_16_16_16_FLOAT)) {
          rgba[i] = col::HalfToFloat(uint16_t(bits));
        } else if (IsFixed16Class(cls)) {
          rgba[i] = std::max(-1.0f, float(int16_t(uint16_t(bits))) / 32767.0f);
        } else {
          rgba[i] = std::bit_cast<float>(i == 0 ? uint32_t(v) : uint32_t(v >> 32));  // 32_FLOAT / 32_32_FLOAT
        }
      }
    }
    const rr::PixelRect pr = HostRect(*rt, info.x0, info.y0, info.x1, info.y1);
    PrepareColourTarget(*rt, OwnershipForRect(pr), false, 0);
    const gpu::Rect rect = ToGpuRect(pr);
    backend_->ClearColor(rt->texture, rgba, &rect, 1);
    Wrote(*rt);
    ++stats_.colour_clears_resolve;
    if (trace_on_) TraceEvent(*rt, pr, "resolve clear (colour)");
  }

  // Colour resolve (M2 build A): 1x colour target -> destination words by the reference's order, then compared with
  // the emulator's bytes like the depth resolves.
  void ResolveColour(const ::xenos::resolve::Info& info, const ::xenos::resolve::Regs& rg, uint32_t copy,
                     uint32_t first_draw, uint32_t last_draw, uint32_t drawn, uint32_t dinfo) {
    const uint32_t w = info.width_div_8 * 8, h = info.height_div_8 * 8;
    const uint32_t cls = rr::ColorStorageClass(rg.color_format);
    const int32_t exp_bias = SignExtend(Bits(dinfo, 16, 6), 6);
    const bool swap = Bits(dinfo, 24, 1) != 0;
    const uint32_t endian = Bits(dinfo, 0, 3);
    char source[240];
    // Number format (RB_COPY_DEST_INFO bits 13-15) logged for D23: the SDK ignores it for fixed-point destinations.
    std::snprintf(source, sizeof(source),
                  "colour %s base %u pitch %u msaa %ux %ux%u at (%d,%d) -> %08X fmt %u number fmt %u exp_bias %d%s%s",
                  rr::StorageClassName(cls).c_str(), rg.color_base, rg.surface_pitch, 1u << rg.msaa, w, h, info.x0,
                  info.y0, info.dest_base, info.dest_format, Bits(dinfo, 13, 3), exp_bias, swap ? " swap" : "",
                  ::xenos::colour::ColourResolveIsRaw(::xenos::colour::ColorFormat(rg.color_format), dinfo, true,
                                                      ::xenos::colour::Reference::kSdk)
                      ? " raw"
                      : "");
    auto log_not_compared = [&](const char* why) {
      ++resolve_early_[why];  // patch 0030: per-window count of resolves that end here (Gears 3's squad lead)
    };
    const rr::RenderTargetKey ck{rg.color_base, cls, rg.surface_pitch, rg.msaa};
    rr::RenderTarget* rt = rts_->FindAny(ck);  // patch 0141 review: either phase's twin
    // Gears 3 (job 025): MSAA colour sources are resolved with colour_resolve_ms_ (without it: skipped, "build B").
    const bool ms_src = rt_msaa_(rg.msaa);
    if (ms_src && !colour_resolve_ms_) return log_not_compared("MSAA colour resolves are build B");
    if (rr::ColorHostFormat(cls) == gpu::Format::kUnknown) return log_not_compared("source format not modelled (8888_GAMMA)");
    // Gears 3 (job 019): the title/scene HDR target (base 720, 7e3) is resolved a second time through a
    // k_2_10_10_10 view of the same tiles (raw bits, copy 9 of each frame) that no draw ever used. Without a host
    // target for that view the resolve was skipped, so the guest kept the earlier exp_bias -5 copy and the scene
    // came out black. Create the view's target here; PrepareColourTarget below brings the tiles in from their
    // owner (guest-word transfer, bits kept). --gow2_renderer_resolve_view_targets=false restores the old skip.
    if ((!rt || !rt->texture) && REXCVAR_GET(gow2_renderer_resolve_view_targets)) {
      rt = rts_->Get(ck);
      if (rt && rt->texture && resolve_view_logged_.insert(uint64_t(rg.color_base) << 32 | cls << 16 | rg.surface_pitch).second)
        REXLOG_INFO("gow2_renderer: resolve frame {} copy {}: created a target for a resolve-only view: {}", frame_ + 1,
                    copy, source);
      if (rt && rt->texture) ++stats_.resolve_view_targets;
    }
    if (!rt || !rt->texture) return log_not_compared("we never drew into that colour target");
    if (!colour_resolve_) return log_not_compared("colour resolve shader missing");
    const uint32_t f = info.dest_format;
    uint32_t dest_bytes = ::xenos::colour::DestBytesPerPixel(::xenos::colour::DestFormat(f));
    // Patch 0030: 8 / 16 / 128 bpp destinations (Canary resolve_full_8bpp / 16bpp / 128bpp) and the 8in64 / 8in128 swaps.
    const bool other_bpp = REXCVAR_GET(gow2_renderer_resolve_other_bpp);
    if (!dest_bytes && other_bpp && info.dest_supported) {
      if (f == 2 || f == 8 || f == 9) dest_bytes = 1;
      else if (f == 3 || f == 4 || f == 5 || f == 10 || f == 15 || f == 24 || f == 30) dest_bytes = 2;
      else if (f == 38) dest_bytes = 16;
    }
    if (!info.dest_supported || !dest_bytes) {
      ++t2_stats_.skipped_format;
      return log_not_compared("destination format not handled (8/16/128bpp)");
    }
    if (endian > 5 || (endian > 3 && (!other_bpp || dest_bytes < 8)) || (endian == 5 && dest_bytes != 16)) {
      ++t2_stats_.skipped_endian;
      return log_not_compared("8in64/128-bit endian swap");
    }
    const bool narrow_or_wide = (dest_bytes != 4 && dest_bytes != 8) || endian > 3;  // the patch 0030 cases
    const uint32_t words = dest_bytes == 16 ? 4u : dest_bytes == 8 ? 2u : 1u;
    // Tiles the resolve reads that another target wrote last: their data comes in, ownership stays (a read).
    PrepareColourTarget(*rt, OwnershipForRect(HostRect(*rt, info.x0, info.y0, info.x1, info.y1)), false, 0, true);
    // Upscalers (notes/64): the frame's scene colour goes through DLSS / FSR, and the resolve reads their output.
    gpu::TextureHandle resolve_source = rt->texture;
    uint32_t source_scale = rt->scale;  // patch 0141: the upscaler's output is at the output scale
    if (up_active_ && !up_scene_done_ && up_camera_ && !ms_src && IsSceneColourResolve(info, *rt)) {
      up_scene_done_ = true;
      up_output_upscaled_ = false;
      if (const gpu::TextureHandle o = RunUpscaler(*rt, w, h)) {
        resolve_source = o;
        if (up_output_upscaled_) source_scale = res_scale_;
      }
      if (up_scale_in_) up_flip_after_resolve_ = true;  // patch 0141: the rest of the frame at the output scale
    } else if (up_active_ && !up_scene_done_ && info.width_div_8 * 8 >= kSceneMinWidth && up_diag_lines_ < 12) {
      ++up_diag_lines_;  // Gears 3 job 054: a full-width colour resolve that did not start the upscaler, and why
      REXLOG_INFO("gow2_upscaler: frame {} copy {}: full-width colour resolve not used: camera {} msaa {} format {} "
                  "(scene needs {}) at {},{} {}x{} scale {}", frame_ + 1, copy, up_camera_ ? 1 : 0, ms_src ? 1 : 0,
                  uint32_t(rt->format), uint32_t(gpu::Format::kR16G16B16A16Float), info.x0, info.y0,
                  info.width_div_8 * 8, info.height_div_8 * 8, rt->scale);
    }
    const gpu::TextureHandle dest = DestinationTexture(info, words);
    if (!dest) return log_not_compared("destination texture");
    const uint32_t dx = info.dest_offset_x_div_8 * 8, dy = info.dest_offset_y_div_8 * 8;
    if ((dx + w) * words > std::max<uint32_t>(info.dest_pitch_aligned_div_32 * 32, 32) * words ||
        dy + h > std::max<uint32_t>(info.dest_height_aligned_div_32 * 32, 32))
      return log_not_compared("rectangle extends past the destination texture");
    gpu::ComputeBindings b;
    b.constants[0] = uint32_t(info.x0);
    b.constants[1] = uint32_t(info.y0);
    b.constants[2] = w;
    b.constants[3] = h;
    b.constants[4] = dx;
    b.constants[5] = dy;
    b.constants[6] = rg.color_format;
    b.constants[7] = dinfo & ~7u;  // endian is applied by the comparison
    // MSAA (Gears 3): the guest samples the sanitised select names, as host sample indices. Our draws rasterise at
    // D3D12's standard positions (no SetSamplePositions), so each Xenos sample reads the nearest host sample: 4x Xenos
    // {-2,-2},{2,2},{-6,6},{6,-6} vs D3D12 {-2,-6},{6,-2},{-6,2},{2,6} (1/16 px) -> host 0,3,2,1 (each 4/16 px away).
    // 2x Xenos {-4,4},{4,-4} vs D3D12 {4,4},{-4,-4} tie - identity (GUESS). Averages don't depend on the order.
    uint32_t ms_count = 1, ms_map = 0;
    if (ms_src) {
      using S = ::xenos::resolve::CopySampleSelect;
      const bool four = rg.msaa >= 2;
      static constexpr uint32_t kHost4x[4] = {0, 3, 2, 1};
      // Patch 0089: with the Xenos positions programmed, host sample = Xenos sample (4x), swapped for native 2x.
      const bool xp = msaa_positions_mode_ == 1 && msaa_positions_ok_;
      auto h = [&](uint32_t g) { return four ? (xp ? (g & 3) : kHost4x[g & 3]) : (xp ? ((g & 1) ^ 1) : (g & 1)); };
      switch (info.sample_select) {
        case S::k0: case S::k1: case S::k2: case S::k3: ms_map = h(uint32_t(info.sample_select)); break;
        case S::k01: ms_count = 2; ms_map = h(0) | h(1) << 4; break;
        case S::k23: ms_count = 2; ms_map = h(2) | h(3) << 4; break;
        default: ms_count = four ? 4u : 2u; ms_map = four ? (h(0) | h(1) << 4 | h(2) << 8 | h(3) << 12) : (0u | 1u << 4); break;
      }
      if (msaa_resolve_logged_.insert(uint64_t(rg.color_base) << 32 | uint64_t(cls) << 16 | uint64_t(rg.msaa) << 8 |
                                      uint64_t(info.sample_select)).second)
        REXLOG_INFO("gow2_renderer: MSAA colour resolve (first of its kind) frame {} copy {}: {} | sample select {} -> {} "
                    "host sample(s) {:04X}", frame_ + 1, copy, source, uint32_t(info.sample_select), ms_count, ms_map);
      ++stats_.msaa_colour_resolves;
    }
    const bool single_sample = ms_count == 1;
    const bool canary_rules = REXCVAR_GET(gow2_renderer_resolve_rules) == "canary";  // patch 0023
    b.constants[8] = ::xenos::colour::ColourResolveIsRaw(::xenos::colour::ColorFormat(rg.color_format), dinfo,
                                                         single_sample,
                                                         canary_rules ? ::xenos::colour::Reference::kCanary
                                                                      : ::xenos::colour::Reference::kSdk)
                         ? 1u
                         : 0u;
    b.constants[10] = ms_count;
    b.constants[11] = ms_map;
    b.constants[9] = canary_rules ? 1u : 0u;  // the SDK reference (0) or Canary's rules (1, patch 0023)
    b.constants[12] = source_scale;  // Stage 7: source pixel = guest pixel x scale (patch 0141: of the source read)
    b.constant_count = 16;
    b.srv[0] = resolve_source;
    b.srv_plane[0] = 0;
    b.uav_texture = dest;
    const gpu::PipelineHandle resolve_pipe = ms_src ? colour_resolve_ms_ : colour_resolve_;
    // Patch 0119: 32 bpp, single-sample source -> the same step as a draw (the destination is a render target then).
    const bool as_draw = colour_resolve_draw_ && !ms_src && !narrow_or_wide && words == 1;
    if (as_draw)
      ResolveDraw(b, dx, dy, dx + w, dy + h);
    else
      ResolveDispatch(resolve_pipe, b, (w + 7) / 8, (h + 7) / 8, 1);
    ++stats_.colour_resolves_compared;
    ScaledDest* sd = nullptr;
    bool scaled_mem = false;
    rr::TextureCache::ResolveThrough through;
    // Patch 0143 (upscaling modes): a scene-scale source above 1x (not MSAA) also fills the output-scale copy (nearest);
    // the passes below then run at the output scale with the source's scale in c3.y bits 8-15.
    const uint32_t cross = up_scale_in_ > 1 && !ms_src && source_scale == up_scale_in_ ? source_scale : 0u;
    if (cross) b.constants[12] = res_scale_;
    // Job 238: fused unless the presenter has shown this destination (it reads the scaled destination texture). Not
    // for MSAA sources: their shader takes the sample count / map in c2.z / c2.w, which the fused pass uses itself.
    if (narrow_or_wide) {
      // Patch 0030: unscaled only (the scaled-memory paths are 32/64 bpp).
    } else if (scaled_resolves_ && (source_scale == res_scale_ || cross) && fused_scaled_ && (fused_kinds_ & 1u) && !ms_src &&
               !front_bases_.count(info.dest_base) &&
               rr::FuseResolveSize(w, h, uint32_t(std::max(0, REXCVAR_GET(gow2_renderer_fused_scaled_min_pixels))))) {
      scaled_mem = FusedScaledResolve(colour_resolve_, b, info, w, h, words, endian, cross);
    }
    if (!narrow_or_wide && !scaled_mem && scaled_resolves_ && (source_scale == res_scale_ || cross)) {
      sd = ScaledDestination(info, words);
      if (sd) {
        b.constants[13] = 1u | (cross << 8);  // patch 0143
        b.uav_texture = sd->texture;
        if (as_draw)  // patch 0119
          ResolveDraw(b, dx * res_scale_, dy * res_scale_, (dx + w) * res_scale_, (dy + h) * res_scale_);
        else
          ResolveDispatch(resolve_pipe, b, (w * res_scale_ + 7) / 8, (h * res_scale_ + 7) / 8, 1);
        scaled_mem = ScaledToMemory(*sd, info, dx, dy, w, h, endian, through);
      }
    }
    WriteResolveToGuest(dest, info, endian, words, dx, dy, w, h, scaled_mem, &through);
    if (display_ours_ && dest_bytes == 4) {  // takeover step 1: remember this frame's colour copies by destination
      DisplaySource& d = display_sources_[info.dest_base];
      d.texture = dest;
      d.rect = gpu::Rect{int32_t(dx), int32_t(dy), int32_t(dx + w), int32_t(dy + h)};
      d.info = info;
      d.endian = endian;
      d.scaled = sd ? sd->texture : 0;
    }
  }
  static bool rt_msaa_(uint32_t msaa) { return msaa != 0; }


  // Largest absolute difference between two words of the destination format, in that format's channel units
  // (8888/2_10_10_10: integer steps; fp16/fp32: float value).
  static double WordDiff(uint32_t a, uint32_t b, uint32_t format) {
    double m = 0;
    auto half = [](uint32_t h) {
      const uint32_t e = (h >> 10) & 0x1F, f = h & 0x3FF;
      const double v = e == 0 ? std::ldexp(double(f), -24) : e == 31 ? 65536.0 : std::ldexp(double(f | 0x400), int(e) - 25);
      return (h & 0x8000) ? -v : v;
    };
    switch (format) {
      case 6:
        for (int k = 0; k < 4; ++k) m = std::max(m, std::abs(double((a >> (8 * k)) & 0xFF) - double((b >> (8 * k)) & 0xFF)));
        break;
      case 7:
        for (int k = 0; k < 3; ++k)
          m = std::max(m, std::abs(double((a >> (10 * k)) & 0x3FF) - double((b >> (10 * k)) & 0x3FF)));
        m = std::max(m, std::abs(double(a >> 30) - double(b >> 30)));
        break;
      case 31: case 32:
        for (int k = 0; k < 2; ++k)
          m = std::max(m, std::abs(half((a >> (16 * k)) & 0xFFFF) - half((b >> (16 * k)) & 0xFFFF)));
        break;
      default:
        m = std::abs(double(std::bit_cast<float>(a)) - double(std::bit_cast<float>(b)));
        break;
    }
    return m;
  }







  void LogSummary() {
    COGBOX_TRACY_SCOPE("Renderer summary diagnostics");
    const rr::ShaderCacheStats& s = cache_->Stats();
    std::string skipped;
    for (const auto& [why, n] : stats_.skipped) skipped += " | skipped " + std::to_string(n) + ": " + why;
    REXLOG_INFO("gow2_renderer: frames {}-{}: draws {} drawn {} (clears {} + {} repeated on aliases, colour clears {}) "
                "| dummy points {} | patches {} (tessellated {}, not drawn {}) | colour-only (stubbed) {} | texture-dependent kills/alpha {} | aliases "
                "{} ({} tiles) | pending shaders {} | not drawn: shader {} pipeline {} other {} | copies {} (depth {}, "
                "colour {}) | resolves compared {} matched {} explained diffs {} incomplete {} | upload watch: index {:.1f} MB "
                "unchanged, self-check {} ranges, STALE {} ({:.2f} MB) | alias tiles {} transferred {} from colour "
                "clears {} unknown ({} colour), rules disagree {} | ownership {} (unclipped depth draws bounded by scissor only "
                "{}) | GPU texture loads {} ({:.1f} MB guest bytes) | textures {} ({:.0f} MB host) uploads {:.1f} MB, stand-ins {} | vertex {:.1f} MB (direct {:.1f}, reused {:.1f}) index {:.1f} MB | "
                "copy threads: deferred {:.1f} MB in {} copies, flushes {} | "
                "shaders translated {} disk hits {} compiled {} queued {} in flight {} rejected {} (compile {:.0f} "
                "ms on {} threads) | targets {} | our CPU {:.3f} ms/frame (max {:.3f}) | GPU submits {}{}",
                frame_ - uint64_t(log_every_) + 1, frame_, stats_.draws, stats_.drawn, stats_.clears,
                stats_.clear_copies, stats_.colour_clears, stats_.dummy_points, stats_.patches, stats_.tessellated,
                stats_.patches_not_drawn, stats_.colour_only,
                stats_.texture_dependent, stats_.alias_events, stats_.alias_tiles, stats_.pending_shaders,
                stats_.shader_errors, stats_.pipeline_failures, stats_.other_failures, stats_.copies,
                stats_.depth_resolves, stats_.colour_resolves, compared_, matched_, explained_, incomplete_,
                double(stats_.index_skipped_bytes) / 1048576.0, stats_.watch_checked, stats_.watch_stale,
                double(stats_.watch_stale_bytes) / 1048576.0, stats_.transferred_tiles, stats_.cleared_tiles,
                stats_.unknown_tiles, stats_.unknown_colour_tiles, stats_.disagree_tiles,
                canary_ownership_ ? "canary" : "rect", stats_.unclipped_depth_draws, textures_->Stats().gpu_loads,
                double(textures_->Stats().gpu_load_bytes) / 1048576.0, textures_->Stats().textures, double(textures_->Stats().host_bytes) / 1048576.0,
                double(textures_->Stats().upload_bytes) / 1048576.0, textures_->Stats().stand_ins,
                double(stats_.vertex_bytes) / 1048576.0, double(stats_.vertex_direct_bytes) / 1048576.0,
                double(stats_.vertex_reused_bytes) / 1048576.0, double(stats_.index_bytes) / 1048576.0,
                double(backend_->Stats().copy_deferred_bytes) / 1048576.0, backend_->Stats().copies_deferred,
                backend_->Stats().copy_flushes, s.translated,
                s.disk_hits, s.compiled, s.queued, cache_->PendingCount(), s.errors, s.compile_ms, s.threads,
                rts_->size(),
                cpu_sum_ / log_every_, cpu_max_, backend_->Stats().submits, skipped);
    if (!resolve_early_.empty()) {  // patch 0030
      std::string reasons;
      for (const auto& [why, n] : resolve_early_) reasons += (reasons.empty() ? "" : " | ") + std::to_string(n) + ": " + why;
      REXLOG_INFO("gow2_renderer: resolves ended early by reason (frames {}-{}): {}", frame_ + 1 - uint64_t(log_every_),
                  frame_, reasons);
      resolve_early_.clear();
    }
    if (!textures_->Stats().stand_in_reasons.empty()) {
      std::string reasons;
      for (const auto& [why, n] : textures_->Stats().stand_in_reasons)
        reasons += (reasons.empty() ? "" : " | ") + std::to_string(n) + ": " + why;
      REXLOG_INFO("gow2_renderer: texture stand-ins by reason: {}", reasons);
    }
    {  // job 045: GPU pipeline stalls come from barriers; count them per frame
      const gpu::BackendStats& bs = backend_->Stats();
      const double frames = double(std::max(log_every_, 1));
      REXLOG_INFO("gow2_renderer: barriers: {:.1f} calls/frame ({:.1f} barriers/frame) sent as {:.1f} batches/frame ({:.1f} "
                  "merged away/frame), {:.1f} dispatches/frame, {:.1f} draws/frame, {:.1f} submits/frame",
                  double(bs.barrier_calls - summary_barrier_calls_) / frames, double(bs.barriers - summary_barriers_) / frames,
                  double(bs.barrier_batches - summary_barrier_batches_) / frames,
                  double(bs.barriers_merged - summary_barriers_merged_) / frames,
                  double(bs.dispatches - summary_dispatches_) / frames, double(bs.draws - summary_backend_draws_) / frames,
                  double(bs.submits - summary_submits_) / frames);
      summary_barrier_calls_ = bs.barrier_calls;
      summary_barrier_batches_ = bs.barrier_batches;
      summary_barriers_merged_ = bs.barriers_merged;
      summary_barriers_ = bs.barriers;
      summary_dispatches_ = bs.dispatches;
      summary_backend_draws_ = bs.draws;
      summary_submits_ = bs.submits;
    }
    if (pending_mirror_ && (stats_.vertex_pages_kept_pending || stats_.vertex_from_exports))  // patch 0007
      REXLOG_INFO("gow2_renderer: export pending mirror: vertex ranges read from exports {}, page checks that kept the "
                  "mirror because a readback was still in flight {}",
                  stats_.vertex_from_exports, stats_.vertex_pages_kept_pending);
    if (stats_.memexport_draws || stats_.memexport_only_draws) {  // notes/11 (this summary's frames)
      REXLOG_INFO("gow2_renderer: memexport: draws {} (export-only {}, pixel-shader exports {}), ranges marked {} ({:.2f} "
                  "MB), vertex ranges read from exports {}, readbacks into guest memory {} ({:.2f} MB, {} failed, {} "
                  "waited for by texture loads; 0042: {} ranges merged into {} readbacks)",
                  stats_.memexport_draws + stats_.memexport_only_draws, stats_.memexport_only_draws,
                  stats_.memexport_ps_draws, stats_.memexport_ranges, double(stats_.memexport_bytes) / 1048576.0,
                  stats_.vertex_from_exports, stats_.memexport_readbacks,
                  double(stats_.memexport_readback_bytes) / 1048576.0, stats_.memexport_readback_failures,
                  stats_.memexport_readback_waits, stats_.memexport_ranges_gathered, stats_.memexport_readbacks_issued);
    }
    if ((words_to_guest_through_ || scaled_to_mem_through_) && textures_) {
      const rr::TextureCacheStats& ts = textures_->Stats();
      REXLOG_INFO("gow2_renderer: resolve-through: textures written by their resolve {} (reload skipped), matched but "
                  "already out of date {}, version moved meanwhile {} | GPU texture loads {} | scaled mirrors updated {}",
                  ts.through_writes, ts.through_out_of_date, ts.through_raced, ts.gpu_loads, ts.scaled_through_writes);
    }
    if (stats_.depth_pipeline_waits)  // patch 0132 (this summary's frames)
      REXLOG_INFO("gow2_renderer: patch 0132 depth-only / shadow pipeline waits: {} draws waited {:.1f} ms in all, {} got "
                  "their pipeline in time (the rest skipped)",
                  stats_.depth_pipeline_waits, stats_.depth_pipeline_wait_ms, stats_.depth_pipeline_waits_won);
    if (stats_.vertex_other_type)  // patch 0049 (this summary's frames)
      REXLOG_INFO("gow2_renderer: patch 0049: vertex fetch constants uploaded although their type is not kVertex: {}",
                  stats_.vertex_other_type);
    if (stats_.standin_waits || stats_.standin_over_budget) {  // patch 0045 (this summary's frames)
      std::string why;
      for (const auto& [reason, n] : standin_reasons_) why += std::format("{}{} {}", why.empty() ? "" : ", ", reason, n);
      REXLOG_INFO("gow2_renderer: patch 0045 stand-in waits (budget {} ms/frame): {} draws waited ({:.1f} ms in total; "
                  "{}), {} skipped as before because the frame's budget was spent",
                  standin_wait_budget_ms_, stats_.standin_waits, stats_.standin_wait_ms, why, stats_.standin_over_budget);
      standin_reasons_.clear();
    }
    if (stats_.shader_waits) {  // shader pop-in (this summary's frames)
      REXLOG_INFO("gow2_renderer: shader waits (budget {} ms/frame): {} draws waited for their shader, {} got it in "
                  "time, {:.1f} ms in total | 0051 lookahead: {} times over {} queued draws, {} shaders sent to the "
                  "compile threads early | 0052: {} blended no-depth draws left out while compiling | 0053: {} new "
                  "shaders started at first sight",
                  shader_wait_budget_ms_, stats_.shader_waits, stats_.shader_waits_ready, stats_.shader_wait_ms,
                  stats_.lookaheads, stats_.lookahead_draws, stats_.lookahead_queued, stats_.cosmetic_deferred,
                  stats_.prefetched);
    }
    if (cache_->Dxbc()) {  // overhaul option B
      const rr::ShaderCacheStats& cs = cache_->Stats();
      const xenos::dxbc_gen::RuleHits rh = xenos::dxbc_gen::CanaryRuleHits();  // patch 0087
      REXLOG_INFO("gow2_renderer: DXBC back end: {} shaders made as DXBC so far ({:.1f} ms translating in all), {} left "
                  "to the HLSL path | {} draws in these frames paired DXBC with DXIL | newer Canary rules (patch 0087), "
                  "shaders changed: tfetch1D XY {}, mulsc toward zero {}, float16 export NaN {}",
                  cs.dxbc, cs.translate_ms, cs.dxbc_fallbacks, stats_.dxbc_mixed, rh.tfetch1d_xy,
                  rh.mulsc_round_toward_zero, rh.memexport_f16_nan_zero);
    }
    if (stats_.pipeline_waits) {  // job 037 (this summary's frames)
      REXLOG_INFO("gow2_renderer: pipeline waits (budget {} ms/frame): {} in these frames, {:.1f} ms in total",
                  REXCVAR_GET(gow2_renderer_pipeline_wait_ms), stats_.pipeline_waits, stats_.pipeline_wait_ms);
    }
    {
      // CPU split: exclusive ms per frame of each part, then how much of all that was blocked on our own GPU.
      std::string split;
      double total = 0;
      for (uint32_t i = 0; i < kPartCount; ++i) {
        part_ms_[i] = double(part_ticks_[i]) * MsPerTick();
        part_ticks_[i] = 0;
      }
      for (uint32_t i = 0; i < kPartCount; ++i) total += part_ms_[i];
      const double record_ms = part_ms_[kPartRecord];
      if (part_level_ == 0) split = "off (--gow2_renderer_cpu_split=0)";  // patch 0153
      for (uint32_t i = 0; i < kPartCount && part_level_ != 0; ++i) {
        char b[64];
        std::snprintf(b, sizeof(b), "%s%s %.3f", i ? ", " : "", kPartNames[i], part_ms_[i] / log_every_);
        split += b;
        part_ms_[i] = 0;
      }
      // Patch 0120: how much of the console's 512 MB the game holds (physical memory: textures, GPU buffers; the pool
      // can only grow into what is left) and its virtual heaps, now and the most so far - before a bigger texture pool.
      if (memory_) {
        auto mb = [](const rex::memory::BaseHeap* h) {
          return h ? double(uint64_t(h->reserved_page_count()) * h->page_size()) / 1048576.0 : 0.0;
        };
        auto total = [](const rex::memory::BaseHeap* h) { return h ? double(h->heap_size()) / 1048576.0 : 0.0; };
        const rex::memory::BaseHeap* phys = memory_->GetPhysicalHeap();
        const rex::memory::BaseHeap* v0 = memory_->LookupHeap(0x00010000u);
        const rex::memory::BaseHeap* v4 = memory_->LookupHeap(0x40000000u);
        const double p = mb(phys), a0 = mb(v0), a4 = mb(v4);
        mem_peak_phys_mb_ = std::max(mem_peak_phys_mb_, p);
        mem_peak_virt_mb_ = std::max(mem_peak_virt_mb_, a0 + a4);
        REXLOG_INFO("gow2_renderer: frames {}-{}: guest memory (patch 0120): physical {:.1f} of {:.0f} MB (most so far {:.1f}, "
                    "free now {:.1f}) | virtual 00000000 {:.1f} of {:.0f} MB, 40000000 {:.1f} of {:.0f} MB (most so far "
                    "{:.1f} together)", frame_ - uint64_t(log_every_) + 1, frame_, p, total(phys), mem_peak_phys_mb_,
                    total(phys) - p, a0, total(v0), a4, total(v4), mem_peak_virt_mb_);
      }
      const gpu::BackendStats& bs = backend_->Stats();
      REXLOG_INFO("gow2_renderer: frames {}-{}: CPU split (ms/frame, exclusive): {} | total {:.3f} | GPU {:.3f} ms/frame "
                  "({} lists timed; draws {:.3f}, EDRAM transfers {:.3f} [colour same {:.3f}, colour word {:.3f}, depth {:.3f}, "
                  "colour->depth {:.3f}; Mpx/frame {:.2f} {:.2f} {:.2f} {:.2f}], resolves {:.3f}, texture loads {:.3f}, frame "
                  "{:.3f}) | stencil bits set by draws {:02X} clears {:02X} read {:02X}, bit passes skipped {} | read-only transfers "
                  "skipped {:.1f}/frame ({:.2f} Mpx/frame), texture memo hits {:.0f}/frame, fill transfers skipped {:.1f}/frame "
                  "({:.2f} Mpx/frame) + passes {:.1f}/frame, fill-bounded ownership draws {:.1f}/frame (multi-rect {:.1f}), float-constant uploads skipped by generation {:.0f}/frame, compacted {:.0f}/frame ({:.2f} KB/frame not uploaded) | of which blocked "
                  "on our GPU {:.3f} ({} waits) | texture tables reused {} | state calls skipped {} | constant uploads "
                  "skipped {} | ownership fast path {} | transfer runs {} | alternate rule {} | descriptors {} (ring waits {}) | "
                  "pipelines {} (same as the previous draw {}) | upload-ring waits {}",
                  frame_ - uint64_t(log_every_) + 1, frame_, split, total / log_every_,
                  (bs.gpu_ms - gpu_ms_base_) / log_every_, bs.gpu_lists_timed - gpu_lists_base_,
                  (bs.gpu_cat_ms[0] - gpu_cat_base_[0]) / log_every_,
                  (bs.gpu_cat_ms[1] + bs.gpu_cat_ms[5] + bs.gpu_cat_ms[6] + bs.gpu_cat_ms[7] - gpu_cat_base_[1] -
                   gpu_cat_base_[5] - gpu_cat_base_[6] - gpu_cat_base_[7]) / log_every_,
                  (bs.gpu_cat_ms[1] - gpu_cat_base_[1]) / log_every_, (bs.gpu_cat_ms[5] - gpu_cat_base_[5]) / log_every_,
                  (bs.gpu_cat_ms[6] - gpu_cat_base_[6]) / log_every_, (bs.gpu_cat_ms[7] - gpu_cat_base_[7]) / log_every_,
                  double(stats_.xfer_px[0]) / 1e6 / log_every_,
                  double(stats_.xfer_px[1]) / 1e6 / log_every_,
                  double(stats_.xfer_px[2]) / 1e6 / log_every_,
                  double(stats_.xfer_px[3]) / 1e6 / log_every_,
                  (bs.gpu_cat_ms[2] - gpu_cat_base_[2]) / log_every_, (bs.gpu_cat_ms[3] - gpu_cat_base_[3]) / log_every_,
                  (bs.gpu_cat_ms[4] - gpu_cat_base_[4]) / log_every_, stencil_ones_draws_, stencil_ones_clears_,
                  stencil_reads_, stats_.stencil_passes_skipped, double(stats_.readonly_transfers_skipped) / log_every_,
                  double(stats_.readonly_px_skipped) / 1e6 / log_every_, double(stats_.texture_memo_hits) / log_every_,
                  double(stats_.fill_transfers_skipped) / log_every_, double(stats_.fill_px_skipped) / 1e6 / log_every_,
                  double(stats_.fill_passes_skipped) / log_every_, double(stats_.fill_ownership_draws) / log_every_, double(stats_.fill_multi_rect_draws) / log_every_, double(stats_.const_gen_skips) / log_every_, double(stats_.compacted_uploads) / log_every_, double(stats_.compacted_bytes_saved) / 1024.0 / log_every_,
                  (bs.fence_wait_ms - gpu_wait_base_ms_) / log_every_, bs.fence_waits, bs.texture_tables_reused,
                  bs.state_calls_skipped, bs.constant_uploads_skipped, rts_->FastHits(), stats_.transfer_passes,
                  alt_mode_ == 2 ? "always" : "off", bs.descriptors_allocated,
                  bs.descriptor_ring_waits, bs.pipelines, bs.pipeline_memo_hits, bs.ring_waits);
      gpu_wait_base_ms_ = bs.fence_wait_ms;
      if (XenosFrameLogOn()) {
        std::string steps;
        for (auto& step : frame_steps_) {
          if (step.site) steps += std::format("{}{} {:.3f}", steps.empty() ? "" : ", ", step.site,
                                              double(step.ticks) * MsPerTick() / log_every_);
          step.ticks = 0;
        }
        const uint64_t cpu = ThreadCpuTicks();
        REXLOG_INFO("gow2_renderer: frames {}-{}: frame steps (elapsed ms/frame, includes waits): {} | render thread "
                    "CPU {:.3f} ms/frame (OS thread time, includes idle polling; excludes recording/copy workers) | "
                    "waiting for command recorder {:.3f} ms/frame (included in elapsed CPU split) | recorder wakes "
                    "{:.1f}/frame, redundant notifications avoided {:.1f}/frame | guest-memory UAV barriers "
                    "before writes {:.1f}/frame, after writes {:.1f}/frame",
                    frame_ - uint64_t(log_every_) + 1, frame_, steps,
                    cpu >= thread_cpu_base_ ? double(cpu - thread_cpu_base_) / 10000.0 / log_every_ : 0.0,
                    (bs.recording_wait_ms - detail_base_.recording_wait_ms) / log_every_,
                    double(bs.recording_wakeups - detail_base_.recording_wakeups) / log_every_,
                    double(bs.recording_wakeups_avoided - detail_base_.recording_wakeups_avoided) / log_every_,
                    double(bs.guest_uav_barriers_before - detail_base_.guest_uav_barriers_before) / log_every_,
                    double(bs.guest_uav_barriers_after - detail_base_.guest_uav_barriers_after) / log_every_);
        thread_cpu_base_ = cpu;
        REXLOG_INFO("gow2_renderer: frames {}-{}: presentation imports: deferred {}, collected {}, synchronous {} | "
                    "pending {} (peak {})",
                    frame_ - uint64_t(log_every_) + 1, frame_,
                    bs.imports_deferred - detail_base_.imports_deferred,
                    bs.imports_collected - detail_base_.imports_collected,
                    bs.imports_synchronous - detail_base_.imports_synchronous,
                    bs.imports_pending, bs.imports_pending_peak);
      }
      gpu_ms_base_ = bs.gpu_ms;
      std::copy(std::begin(bs.gpu_cat_ms), std::end(bs.gpu_cat_ms), gpu_cat_base_);
      if (gpu_by_target_) {  // job 234: the targets whose draws took the most GPU time this window
        std::vector<std::pair<double, uint32_t>> v;
        for (uint32_t i = 0; i < 32; ++i) {
          const double ms = (bs.gpu_target_ms[i] - gpu_target_base_[i]) / log_every_;
          if (ms > 0.0 && target_slots_[i].id) v.push_back({ms, i});
        }
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string list;
        for (size_t k = 0; k < v.size() && k < 12; ++k) {
          const TargetSlot& t = target_slots_[v[k].second];
          const double mpx = (bs.gpu_target_px[v[k].second] - gpu_target_px_base_[v[k].second]) / log_every_ / 1e6;
          char px[48] = "";
          if (mpx > 0.0) std::snprintf(px, sizeof(px), ", %.2f Msamples passed", mpx);  // patch 0098
          char b[240];
          std::snprintf(b, sizeof(b), "%s#%u %s base %u pitch %u %ux%u: %.3f ms (%.0f draws%s)%s", k ? " | " : "", t.id,
                        rr::StorageClassName(t.key.storage_class).c_str(), t.key.base_tiles, t.key.pitch_pixels,
                        t.width, t.height, v[k].first, double(t.draws) / log_every_, px, t.reused ? " [slot reused]" : "");
          list += b;
        }
        REXLOG_INFO("gow2_renderer: frames {}-{}: GPU draws by target (ms/frame): {}", frame_ - uint64_t(log_every_) + 1,
                    frame_, list.empty() ? "none" : list);
        std::copy(std::begin(bs.gpu_target_ms), std::end(bs.gpu_target_ms), gpu_target_base_);
        std::copy(std::begin(bs.gpu_target_px), std::end(bs.gpu_target_px), gpu_target_px_base_);
        for (TargetSlot& t : target_slots_) {
          t.draws = 0;
          t.reused = false;
        }
      }
      if (!xfer_pairs_.empty()) {  // job 180: the biggest depth-transfer pairs this window
        std::vector<const XferPair*> v;
        for (const auto& [k, p] : xfer_pairs_) v.push_back(&p);
        std::sort(v.begin(), v.end(), [](const XferPair* a, const XferPair* b) { return a->px > b->px; });
        for (size_t i = 0; i < v.size() && i < 6; ++i) {
          const XferPair& p = *v[i];
          REXLOG_INFO("gow2_renderer: frames {}-{}: depth transfer pair #{}: {} | {:.2f} Mpx/frame in {} runs/frame "
                      "(clear {:.2f}, read-only {:.2f}) | last draw: prim {} vs {:016X} ps {:016X} depth func {} write {} "
                      "stencil {} wmask {:02X}",
                      frame_ - uint64_t(log_every_) + 1, frame_, i + 1, p.text, double(p.px) / 1e6 / log_every_,
                      double(p.runs) / log_every_, double(p.clear_px) / 1e6 / log_every_,
                      double(p.read_px) / 1e6 / log_every_, p.prim, p.vs, p.ps, p.depth_func, p.depth_write, p.stencil,
                      p.stencil_wmask);
        }
        xfer_pairs_.clear();
      }
      gpu_lists_base_ = bs.gpu_lists_timed;
      // upl-n108: where the waits are, what record spends its time on, and the texture validation work.
      const gpu::BackendStats& b0 = detail_base_;
      const rr::TextureCacheStats& ts = textures_->Stats();
      // Cleanup: stats_ can be reset between two of these lines (drawn_base_ then above it) - no unsigned wrap.
      const uint64_t drawn = stats_.drawn >= drawn_base_ ? stats_.drawn - drawn_base_ : stats_.drawn;
      const double n = double(log_every_);
      static constexpr const char* kSites[6] = {"renderer", "readback", "sampler table", "upload ring",
                                                "descriptor ring", "other"};
      std::string waits;
      for (uint32_t i = 0; i < 6; ++i) {
        char b[96];
        std::snprintf(b, sizeof(b), "%s%s %llu (%.3f ms/frame)", i ? ", " : "", kSites[i],
                      (unsigned long long)(bs.wait_count[i] - b0.wait_count[i]), (bs.wait_ms[i] - b0.wait_ms[i]) / n);
        waits += b;
      }
      REXLOG_INFO("gow2_renderer: frames {}-{}: CPU detail: GPU waits by site: {} | record {:.2f} us/drawn draw ({} "
                  "drawn/frame): backend state {:.3f} ms/frame of which texture tables {:.3f}, constants {:.3f}; "
                  "pipeline creation on this thread {:.3f} ms/frame, async {} (draws skipped pending {}; saved "
                  "pipelines loaded {} stored {} saves {}) | sampler tables written {} (found {}, heap switches {}) | texture tables reused {} written {} "
                  "| persistent index buffers {} ({:.1f} MB), draws from them {} | textures: re-checks with unchanged "
                  "bytes {}, pages hashed {}, volatile reloads {} ({} pages not hashed), evicted {} ({:.1f} MB), cap {} "
                  "MB",
                  frame_ - uint64_t(log_every_) + 1, frame_, waits, drawn ? record_ms * 1000.0 / double(drawn) : 0.0,
                  drawn / uint64_t(log_every_), (bs.prepare_draw_ms - b0.prepare_draw_ms) / n,
                  (bs.apply_textures_ms - b0.apply_textures_ms) / n,
                  (bs.upload_constants_ms - b0.upload_constants_ms) / n,
                  (bs.pipeline_create_ms - b0.pipeline_create_ms) / n, bs.pipelines_async - b0.pipelines_async,
                  bs.pipelines_pending_draws - b0.pipelines_pending_draws,
                  bs.pipelines_loaded - b0.pipelines_loaded, bs.pipelines_stored - b0.pipelines_stored,
                  bs.pipeline_library_saves - b0.pipeline_library_saves,
                  bs.sampler_tables_written - b0.sampler_tables_written, bs.sampler_table_hits - b0.sampler_table_hits,
                  bs.sampler_heap_switches - b0.sampler_heap_switches,
                  bs.texture_table_hits - b0.texture_table_hits, bs.texture_table_misses - b0.texture_table_misses,
                  bs.index_buffers,
                  double(bs.index_buffer_bytes) / 1048576.0, bs.index_buffer_draws - b0.index_buffer_draws,
                  ts.content_unchanged, ts.pages_hashed, ts.volatile_reloads, ts.pages_skipped, ts.evictions, double(ts.evicted_bytes) / 1048576.0,
                  REXCVAR_GET(gow2_renderer_texture_cache_mb));
      {  // patch 0087: texture rules - totals when they change
        static uint64_t last_tail = 0, last_grad = 0;
        if (ts.packed_tail_at_base != last_tail || ts.grad_exp_bias_loads != last_grad) {
          last_tail = ts.packed_tail_at_base;
          last_grad = ts.grad_exp_bias_loads;
          REXLOG_INFO("gow2_renderer: texture rules (patch 0087): {} loads read a packed mip tail at the base address "
                      "(mip address 0), {} loads had a gradient exponent bias in the fetch constant (totals)",
                      last_tail, last_grad);
        }
      }
      if (bs.vk_passes != b0.vk_passes)  // patch 0089: Vulkan render passes and full barriers per frame (stage V5b)
      {
        auto pf = [&](uint32_t i) { return double(bs.vk_pass_end[i] - b0.vk_pass_end[i]) / double(log_every_); };
        REXLOG_INFO("gow2_renderer: frames {}-{}: Vulkan {:.1f} render passes and {:.1f} full barriers per frame "
                    "({:.1f} pass starts without one, {:.1f} with an attachment-only one, {:.1f} clears inside a pass), {:.1f} barrier calls per frame | "
                    "passes ended per frame by: new targets {:.1f}, guest upload {:.1f}, "
                    "texture upload {:.1f}, dispatch {:.1f}, clear {:.1f}, read of a written texture {:.1f}, readback / "
                    "copy {:.1f}, submit {:.1f}, texture create / destroy {:.1f}, other {:.1f} | images in a dedicated "
                    "allocation {} of {} (patch 0097) | attachments moved into an attachment layout {:.1f} per frame (patch 0100) | "
                    "barrier calls by source stages (patch 0107): graphics {:.1f}, compute {:.1f}, transfer {:.1f}, "
                    "mixed {:.1f}, all {:.1f}",
                    frame_ - uint64_t(log_every_) + 1, frame_, double(bs.vk_passes - b0.vk_passes) / double(log_every_),
                    double(bs.vk_full_barriers - b0.vk_full_barriers) / double(log_every_),
                    double(bs.vk_barriers_skipped - b0.vk_barriers_skipped) / double(log_every_),
                    double(bs.vk_barriers_attachment - b0.vk_barriers_attachment) / double(log_every_),
                    double(bs.vk_clears_in_pass - b0.vk_clears_in_pass) / double(log_every_),
                    double(bs.barrier_calls - b0.barrier_calls) / double(log_every_), pf(1), pf(2), pf(3), pf(4), pf(5),
                    pf(6), pf(7), pf(8), pf(9), pf(0), bs.vk_dedicated_images, bs.vk_images,
                    double(bs.vk_attachment_layout_changes - b0.vk_attachment_layout_changes) / double(log_every_),
                    double(bs.vk_barrier_src[0] - b0.vk_barrier_src[0]) / double(log_every_),
                    double(bs.vk_barrier_src[1] - b0.vk_barrier_src[1]) / double(log_every_),
                    double(bs.vk_barrier_src[2] - b0.vk_barrier_src[2]) / double(log_every_),
                    double(bs.vk_barrier_src[3] - b0.vk_barrier_src[3]) / double(log_every_),
                    double(bs.vk_barrier_src[4] - b0.vk_barrier_src[4]) / double(log_every_));
      }
      if (bs.depth_decompressions != b0.depth_decompressions)  // patch 0089
        REXLOG_INFO("gow2_renderer: frames {}-{}: MSAA depth decompressed before reads (Xenos sample positions, patch "
                    "0089): {} ({} in all)", frame_ - uint64_t(log_every_) + 1, frame_,
                    bs.depth_decompressions - b0.depth_decompressions, bs.depth_decompressions);
      if (bs.vk_targets_made_mutable != b0.vk_targets_made_mutable || bs.vk_views_refused != b0.vk_views_refused)
        REXLOG_INFO("gow2_renderer: frames {}-{}: Vulkan compressible targets (patch 0087): {} made mutable ({} in all), "
                    "{} other-format views refused (diagnostic 'never'; {} in all)", frame_ - uint64_t(log_every_) + 1,
                    frame_, bs.vk_targets_made_mutable - b0.vk_targets_made_mutable, bs.vk_targets_made_mutable,
                    bs.vk_views_refused - b0.vk_views_refused, bs.vk_views_refused);
      // Patch 0057: what the pipeline workers built in these frames (the driver's time per new pipeline).
      if (const uint64_t built = bs.pipelines_built_async - b0.pipelines_built_async) {
        REXLOG_INFO("gow2_renderer: frames {}-{}: pipeline workers built {} new pipeline(s): {:.0f} ms in the driver in "
                    "all, {:.1f} ms each on average (slowest so far {:.1f} ms), {:.1f} KB bytecode each | by priority: "
                    "opaque {} / colour 0 {} / other colour {} / depth only {} / rest {} | still queued {} | recorded to "
                    "pipelines.list this session {} | start-up backlog jobs moved to the front for a draw {}",
                    frame_ - uint64_t(log_every_) + 1, frame_, built, bs.pipeline_async_ms - b0.pipeline_async_ms,
                    (bs.pipeline_async_ms - b0.pipeline_async_ms) / double(built), bs.pipeline_async_max_ms,
                    double(bs.pipeline_async_bytes - b0.pipeline_async_bytes) / 1024.0 / double(built),
                    bs.pipelines_built_by_priority[4] - b0.pipelines_built_by_priority[4],
                    bs.pipelines_built_by_priority[3] - b0.pipelines_built_by_priority[3],
                    bs.pipelines_built_by_priority[2] - b0.pipelines_built_by_priority[2],
                    bs.pipelines_built_by_priority[1] - b0.pipelines_built_by_priority[1],
                    bs.pipelines_built_by_priority[0] - b0.pipelines_built_by_priority[0], bs.pipeline_jobs_queued,
                    stats_.pipelines_recorded, bs.pipelines_promoted - b0.pipelines_promoted);
      }
      if (prebuild_on_ && (prebuild_offered_.load() != prebuild_logged_offered_ ||
                           pb_.new_ps_pipelines != prebuild_logged_new_ps_)) {  // patch 0068
        uint32_t workers_now = 0;
        uint64_t govern_changes = 0;
        {
          std::lock_guard<std::mutex> lock(warm_govern_mutex_);
          workers_now = warm_workers_now_;
          govern_changes = warm_govern_changes_;
        }
        REXLOG_INFO("gow2_renderer: frames {}-{}: shader prebuild (patch 0068 v2): {} pixel shaders from opened packages, "
                    "{} already known, {} queued, {} sent to the driver, {} not translatable, {} dropped (variant), {} "
                    "waiting, {} dropped (queue full) | warm-only pipelines built {} ({:.0f} ms in the driver), failed "
                    "{}, workers now {} ({} load/play switches) | new draw pipelines with a pixel shader {}, its shader "
                    "prebuilt {} | variants drawn: whole {} / code only {} (totals so far)",
                    frame_ - uint64_t(log_every_) + 1, frame_, prebuild_offered_.load(), prebuild_known_.load(),
                    prebuild_queued_.load(), prebuild_sent_.load(), prebuild_untranslated_.load(),
                    prebuild_dropped_.load(), PrebuildWaiting(), prebuild_overflow_.load(), bs.pipelines_warmed,
                    bs.pipeline_warm_ms, bs.pipelines_warm_failed, workers_now, govern_changes, pb_.new_ps_pipelines,
                    pb_.new_ps_prebuilt, prebuild_variant_hits_[0].load(), prebuild_variant_hits_[1].load());
        SaveVariantCounts();  // patch 0069
        prebuild_logged_offered_ = prebuild_offered_.load();
        prebuild_logged_new_ps_ = pb_.new_ps_pipelines;
      }
      if (stats_.created_seen != created_logged_.created_seen ||
          stats_.new_draw_pipelines != created_logged_.new_draw_pipelines) {
        REXLOG_INFO("gow2_renderer: frames {}-{}: shaders the game created (patch 0066): {} seen, {} already known, {} "
                    "translated, {} guessed pipelines queued, {} cached stage prefetch requests, {} waiting | new draw pipelines {}: VS made at load {}, "
                    "PS made at load {} (totals so far)",
                    frame_ - uint64_t(log_every_) + 1, frame_, stats_.created_seen, stats_.created_known,
                    stats_.created_translated, stats_.created_guessed, stats_.created_prefetched, CreatedQueued(), stats_.new_draw_pipelines,
                    stats_.new_vs_from_load, stats_.new_ps_from_load);
        created_logged_ = stats_;
      }
      if (bs.cached_stage_prefetches != detail_base_.cached_stage_prefetches ||
          bs.cached_stage_restores != detail_base_.cached_stage_restores)
        REXLOG_INFO("gow2_renderer: frames {}-{}: cached stages: {} prefetched, {} restored off-thread in {:.1f} ms (totals)",
                    frame_ - uint64_t(log_every_) + 1, frame_, bs.cached_stage_prefetches,
                    bs.cached_stage_restores, bs.cached_stage_restore_ms);
      detail_base_ = bs;
      drawn_base_ = stats_.drawn;
    }
    if (draw_colour_) {
      std::string not_modelled;
      for (uint32_t c = 0; c < 16; ++c)
        if (stats_.colour_not_modelled[c])
          not_modelled += (not_modelled.empty() ? "" : ", ") + rr::StorageClassName(c) + " " +
                          std::to_string(stats_.colour_not_modelled[c]);
      REXLOG_INFO("gow2_renderer: frames {}-{}: colour (M2): draws with colour {} | targets not modelled: {} | MRT "
                  "duplicate slots dropped {} | colour tiles transferred {} (same format) {} (guest words: other "
                  "format / from depth) unknown source {} | colour -> depth tiles {} | colour resolve-clears {} | 16-bit "
                  "fixed draws blending multiplicatively (1/32 scale wrong there) {} | colour resolves done {}, "
                  "compared {} matched {} (all time) | resolve-only view targets created {} | as draws (0119) {}",
                  frame_ - uint64_t(log_every_) + 1, frame_, stats_.colour_drawn,
                  not_modelled.empty() ? std::string("none") : not_modelled, stats_.mrt_duplicates,
                  stats_.colour_transferred_tiles, stats_.colour_word_transferred_tiles, stats_.colour_unknown_tiles,
                  stats_.colour_to_depth_tiles, stats_.colour_clears_resolve, stats_.fixed16_multiplicative_blends,
                  stats_.colour_resolves_compared, colour_compared_, colour_matched_, stats_.resolve_view_targets,
                  stats_.resolve_draws);
    }
    if (queries_mode_ != QueriesMode::kOff) {  // upl-s4c
      const OcclusionStats& o = occ_stats_;
      const gpu::BackendStats& b = backend_->Stats();
      REXLOG_INFO("gow2_renderer: queries ({}) frames {}-{}: {} ({:.1f}/frame) | ours {} (incomplete {}, refused {}, "
                  "unended {}; write-nothing proxy draws kept {}) | answered {}: last count {}, no count yet (1) {} | our counts {}: "
                  "0 -> {} ({:.1f}%), mean "
                  "{:.1f}, after {:.2f} frames; corrected {}, too late {}, estimate visibility wrong {}, dropped {} | "
                  "SDK answers: fake {}, host {} | draws {:.1f}/frame | backend: segments {} refused {} | reports known {} | "
                  "answered hidden {}, WRONGLY hidden {} (visible objects skipped), zeros not yet trusted {} | "
                  "completion notifications {:.1f}/frame",
                  queries_mode_ == QueriesMode::kStrict ? "strict" : queries_mode_ == QueriesMode::kAnswer ? "answer" : "shadow", frame_ - uint64_t(log_every_) + 1, frame_,
                  o.ends, double(o.ends) / log_every_, o.ours, o.incomplete, o.refused, o.unended, o.proxies, o.answered,
                  o.from_cache, o.fallback, o.results, o.zero, o.results ? 100.0 * double(o.zero) / double(o.results) : 0.0,
                  o.results ? double(o.samples) / double(o.results) : 0.0,
                  o.results ? double(o.latency_frames) / double(o.results) : 0.0, o.corrected, o.too_late,
                  o.visibility_changed, o.dropped, o.sdk_fake, o.sdk_host, double(stats_.draws) / log_every_,
                  b.occlusion_segments - occ_backend_segments_base_, b.occlusion_refused - occ_backend_refused_base_,
                  occ_answers_.size(), o.answered_hidden, o.wrong_hidden, o.zero_not_trusted,
                  double(o.completion_notifications) / log_every_);
      occ_backend_segments_base_ = b.occlusion_segments;
      occ_backend_refused_base_ = b.occlusion_refused;
      occ_stats_ = OcclusionStats{};
    }
    textures_->ResetCounters();
    stats_ = FrameStats{};
    drawn_base_ = 0;  // cleanup: drawn_base_ counts from this reset
    cpu_sum_ = cpu_max_ = 0;
  }

  XenosRendererHints hints_;
  rex::memory::Memory* memory_ = nullptr;
  double mem_peak_phys_mb_ = 0.0, mem_peak_virt_mb_ = 0.0;  // patch 0120
  std::unique_ptr<gpu::Backend> backend_;
  std::unique_ptr<rr::ShaderCache> cache_;
  std::unique_ptr<rr::RenderTargetCache> rts_;
  gpu::ShaderHandle rect_gs_ = 0;
  gpu::ShaderHandle rect_gs_dxbc_ = 0;  // the DXBC one, for pipelines of DXBC guest shaders
  std::unique_ptr<cogbox::io::AsyncFileAppender> pipeline_list_;  // patch 0058: pipelines.list, appended to (null = not recording)
  std::unordered_set<uint64_t> recorded_;         // patch 0058: PipelineRecordKey of every record in the list
  gpu::PipelineHandle depth_resolve_ = 0, colour_resolve_ = 0, trace_colour_ = 0;
  gpu::PipelineHandle colour_resolve_ms_ = 0;  // Gears 3: MSAA colour sources (Texture2DMS)
  gpu::ShaderHandle msaa_up_transfer_ps_ = 0;  // Gears 3: 1x -> 4x MSAA colour transfer
  std::unordered_map<uint64_t, gpu::PipelineHandle> msaa_up_pipelines_;
  uint32_t depth_bias_logged_ = 0;  // gow2_renderer_log_depth_bias_base lines so far
  std::unordered_set<uint64_t> msaa_twin_logged_;  // gow2_renderer_clear_msaa_twins: logged once per target
  bool msaa_up_logged_ = false;
  std::unordered_set<uint64_t> msaa_resolve_logged_;
  uint32_t msaa_positions_mode_ = 0;  // patch 0089: 0 standard, 1 xenos, 2 canary_off
  bool msaa_positions_ok_ = false;
  gpu::ShaderHandle transfer_vs_ = 0, transfer_ps_ = 0, transfer_ps_ref_ = 0, colour_transfer_ps_ = 0,
                    word_transfer_ps_ = 0;
  std::map<gpu::Format, gpu::PipelineHandle> word_transfer_pipelines_, word_transfer_bits_pipelines_;
  std::map<gpu::Format, gpu::PipelineHandle> colour_transfer_bits_pipelines_;
  gpu::ShaderHandle word_transfer_bits_ps_ = 0, colour_transfer_bits_ps_ = 0;
  bool transfer_bits_ = true;
  std::map<gpu::Format, gpu::PipelineHandle> colour_transfer_pipelines_;
  std::vector<rr::TileSource> colour_sources_;
  // Transfer pipelines per destination format: [0] depth / one-pass, [1..8] stencil bit passes.
  std::map<gpu::Format, std::array<gpu::PipelineHandle, 9>> transfer_pipelines_;
  std::vector<rr::TileSource> tile_sources_;
  std::vector<rr::TileSource> alt_sources_;
  bool canary_ownership_ = false;
  bool transfer_mixed_bpp_ = false;  // --gow2_renderer_transfer_mixed_bpp (patch 0005)
  bool float24_remap_ = true;  // --gow2_renderer_float24_remap
  double shader_wait_budget_ms_ = 0.0;  // --gow2_renderer_shader_wait_ms
  // Patch 0045 (Edge stand-in waits): the budget, what is left of it this frame, why draws waited (this summary), and
  // per render target the frame it was last drawn in and the one before (Edge draw_target_last_frames_).
  double standin_wait_budget_ms_ = 0.0, standin_wait_left_ms_ = 0.0;
  std::map<std::string, uint64_t> standin_reasons_;
  std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> standin_target_frames_;
  double shader_wait_left_ms_ = 0.0;    // what is left of it this frame
  bool draw_colour_ = true;    // --gow2_renderer_colour
  bool clamp_next_base_ = true;  // --gow2_renderer_clamp_next_base
  int alt_mode_ = 0;           // --gow2_renderer_alt_rule: 0 off, 2 always
  bool alt_active_ = false;
  // Host value of guest depth in a target (D24FS8 with the remap: guest * 0.5).
  float HostDepth(const rr::RenderTarget& t, float guest) const {
    return ((t.key.storage_class & 1) && float24_remap_) ? guest * 0.5f : guest;
  }
  // Depth flags for the resolve/transfer shaders: bit 0 = 20e4, bit 1 = remapped by 0.5.
  uint32_t DepthFlags(uint32_t storage_class) const {
    return (storage_class & 1) ? (float24_remap_ ? 3u : 1u) : 0u;
  }
  // Tile trace state.
  static constexpr uint32_t kTraceSamples = 4096;
  struct TraceEntry {
    std::string text;
    int32_t sample = -1;
  };
  struct PendingTrace {
    uint64_t frame = 0;
    std::vector<TraceEntry> lines;
    gpu::ReadbackHandle readback = 0;
  };
  std::bitset<::xenos::edram::kTileCount> trace_tiles_;
  uint64_t log_vs_hash_ = 0;                         // patch 0015 (set with the other switches)
  int32_t log_vs_max_ = 0, log_vs_logged_ = 0;
  bool trace_draw_ = false;                          // patch 0016: this draw is traced
  int32_t trace_logged_ = 0;
  std::vector<uint32_t> trace_list_;
  uint32_t trace_px_ = 7, trace_py_ = 0;
  bool trace_on_ = false;
  uint32_t trace_samples_ = 0;
  gpu::TextureHandle trace_tex_ = 0;
  std::string trace_ctx_;
  std::vector<TraceEntry> trace_lines_;
  std::deque<PendingTrace> pending_traces_;  // --gow2_renderer_ownership=canary: act on the Canary rule, track ours as alternate
  FrameStats range_base_;  // stats_ at the previous copy of this frame (what was not drawn between two copies)
  XenosUcode vs_, ps_;  // the active shaders' ucode (from the SDK's Shader objects or from our PM4 processor)
  std::deque<uint64_t> frame_fences_;
  std::unordered_map<std::string, DestEntry> destinations_;  // patch 0032: with last use and size
  // Ranges of kWatchMinBytes+ copied under the watch: version at the copy and the bytes' hash (for the self-check).
  struct WatchedRange {
    uint64_t version = 0, last_frame = 0;  // patch 0149: last_frame for EvictStaleUploads
  };
  static constexpr uint64_t kWatchMinBytes = 16u << 10;
  std::unordered_map<uint64_t, WatchedRange> vertex_versions_;  // (address << 32 | size)
  // Converted index buffers of kWatchMinBytes+ (guest bytes), keyed by everything the conversion depends on.
  struct CachedIndices {
    uint64_t version = 0, last_frame = 0;  // patch 0149: last_frame for EvictStaleUploads
    std::vector<uint32_t> indices;
    gpu::BufferHandle gpu = 0;  // persistent GPU copy of `indices` (upl-n108), made on first draw
  };
  CachedIndices* cached_ib_ = nullptr;
  std::vector<SelfTestChain> selftest_;
  std::vector<uint32_t> selftest_words_;
  std::vector<gpu::TextureHandle> selftest_textures_;
  bool selftest_started_ = false;
  gpu::BackendStats detail_base_{};  // backend counters at the last CPU-detail line
  uint64_t drawn_base_ = 0, prewarmed_base_ = 0;  // the draw's indices are this cached buffer's, unconverted
  bool static_index_buffers_ = true;
  std::unordered_map<uint64_t, CachedIndices> index_cache_;
  std::unique_ptr<rr::GuestWatch> watch_;
  std::unique_ptr<rr::TextureCache> textures_;
  rr::TextureCacheStats tex_prev_;  // patch 0137: the counters at the previous log line
  uint32_t submit_draws_ = 0, draws_since_submit_ = 0;  // patch 0138
  uint64_t early_prev_ = 0;  // patch 0138
  bool use_textures_ = true, upload_watch_ = true, vertex_direct_ = true, float_constants_trim_ = true;
  uint32_t fetch_[256] = {};  // this draw's fetch constants as the shaders see them (vertex_direct_ rewrites some)
  // Patch 0105: with --gow2_renderer_tex_post_constants, fetch_[192 + 2k] / [193 + 2k] = TexPostConstants of slot k.
  bool tex_post_constants_ = false, tex_sign_swizzle_ = false;
  uint32_t FetchUploadBytes() {
    if (!tex_post_constants_) return 192 * 4;
    for (uint32_t k = 0; k < 32; ++k) xenos::hlsl::TexPostConstants(&fetch_[6 * k], tex_sign_swizzle_, &fetch_[192 + 2 * k]);
    return 256 * 4;
  }
  RunScratch depth_runs_, colour_runs_;  // BuildRuns scratch (PrepareDepthTarget / PrepareColourTarget)
  bool lookahead_shaders_ = true;  // patch 0051
  bool defer_cosmetic_ = true;     // patch 0052
  bool shader_prefetch_ = true;    // patch 0053
  bool skip_resolve_dispatches_ = false;  // patch 0117: --gow2_renderer_skip_resolve_dispatches (measurement)
  void ResolveDispatch(gpu::PipelineHandle p, const gpu::ComputeBindings& b, uint32_t x, uint32_t y, uint32_t z) {
    if (skip_resolve_dispatches_) { ++stats_.resolve_dispatches_skipped; return; }
    backend_->Dispatch(p, b, x, y, z);
  }
  // Patch 0119: kColourResolvePs into b.uav_texture (an R32_UINT render target: every resolve destination is one while
  // the switch is on) over [x0, x1) x [y0, y1) - the dispatch's threads, by the scissor (the callers have checked the
  // rectangle is inside the texture). Viewport origin 0, so SV_Position is the destination pixel. Same constants as
  // the dispatch; textures and targets are set again by the next guest draw (tex_memo_valid_ cleared, as transfers do).
  bool resolve_draws_ = false;
  gpu::PipelineHandle colour_resolve_draw_ = 0;
  // Patch 0121: per-target contents statistics in one frame (--gow2_renderer_target_stats_frame).
  int32_t stats_frame_ = 0, stats_each_ = 0, stats_from_ = 0, stats_step_ = 1;
  bool stats_textures_ = false;              // patch 0124
  uint32_t stats_view_count_ = 0;
  gpu::TextureView stats_views_[32];         // the 2D slots of the last draw of the stats frame
  bool StatsTexturesNow() const { return stats_textures_ && stats_frame_ && frame_ == uint64_t(stats_frame_); }
  uint32_t stats_draws_ = 0, stats_no_colour_ = 0;
  std::vector<const rr::RenderTarget*> stats_prev_;
  static float StatsHalf(uint16_t h) {
    const uint32_t s = uint32_t(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023;
    uint32_t f;
    if (e == 0) {
      if (!m) f = s;
      else { float v = std::ldexp(float(m), -24); return s ? -v : v; }
    } else if (e == 31) f = s | 0x7F800000u | (m << 13);
    else f = s | ((e + 112) << 23) | (m << 13);
    float out;
    std::memcpy(&out, &f, 4);
    return out;
  }
  // Up to 4 channels of texel x as floats; false for formats the statistics don't decode.
  static bool StatsTexel(gpu::Format f, const uint8_t* p, float* c, uint32_t& n) {
    auto sn16 = [](int16_t v) { return std::max(-1.0f, float(v) / 32767.0f); };
    switch (f) {
      case gpu::Format::kR8G8B8A8Unorm: n = 4; for (int i = 0; i < 4; ++i) c[i] = p[i] / 255.0f; return true;
      case gpu::Format::kR10G10B10A2Unorm: {
        uint32_t w; std::memcpy(&w, p, 4); n = 4;
        c[0] = (w & 1023) / 1023.0f; c[1] = ((w >> 10) & 1023) / 1023.0f; c[2] = ((w >> 20) & 1023) / 1023.0f; c[3] = (w >> 30) / 3.0f;
        return true;
      }
      case gpu::Format::kR16G16B16A16Float: case gpu::Format::kR16G16Float: {
        n = f == gpu::Format::kR16G16Float ? 2 : 4;
        for (uint32_t i = 0; i < n; ++i) { uint16_t h; std::memcpy(&h, p + 2 * i, 2); c[i] = StatsHalf(h); }
        return true;
      }
      case gpu::Format::kR16G16Snorm: case gpu::Format::kR16G16B16A16Snorm: {
        n = f == gpu::Format::kR16G16Snorm ? 2 : 4;
        for (uint32_t i = 0; i < n; ++i) { int16_t v; std::memcpy(&v, p + 2 * i, 2); c[i] = sn16(v); }
        return true;
      }
      case gpu::Format::kR32Float: case gpu::Format::kR32G32Float:
        n = f == gpu::Format::kR32Float ? 1 : 2;
        std::memcpy(c, p, 4 * n);
        return true;
      default: return false;
    }
  }
  // Patch 0124: the 2D textures the previous draw sampled (slots 0-31), whole mip 0, same line format as the targets.
  void SampledStats() {
    const uint32_t n = std::min<uint32_t>(stats_view_count_, 32);
    for (uint32_t i = 0; i < n; ++i) {
      const gpu::TextureView& v = stats_views_[i];
      if (!v.texture) continue;
      gpu::TextureDesc td;
      if (!backend_->TextureInfo(v.texture, td) || td.samples > 1) continue;
      bool seen = false;  // the same texture in two slots: once
      for (uint32_t j = 0; j < i; ++j) seen = seen || stats_views_[j].texture == v.texture;
      if (seen) continue;
      StatsTexture(v.texture, td.format, td.width, td.height, "t" + std::to_string(i));
    }
  }
  void TargetStats(const std::vector<const rr::RenderTarget*>& targets) {
    for (const rr::RenderTarget* t : targets) {
      if (!t || !t->texture) continue;
      const uint32_t s = std::max(t->scale, 1u);
      StatsTexture(t->texture, t->format, std::min(t->width, t->guest_width * s), std::min(t->height, t->guest_height * s),
                   "id " + std::to_string(t->id) + " fmt " + std::to_string(uint32_t(t->format)));
    }
  }
  // One texture's per-channel statistics (patch 0121; split out in 0124 for sampled textures). `what` names it.
  void StatsTexture(gpu::TextureHandle tex, gpu::Format fmt, uint32_t w, uint32_t h, const std::string& what) {
    {
      uint32_t bpp = 0;
      switch (fmt) {
        case gpu::Format::kR8G8B8A8Unorm: case gpu::Format::kR10G10B10A2Unorm: case gpu::Format::kR16G16Float:
        case gpu::Format::kR16G16Snorm: case gpu::Format::kR32Float: bpp = 4; break;
        case gpu::Format::kR16G16B16A16Float: case gpu::Format::kR16G16B16A16Snorm: case gpu::Format::kR32G32Float: bpp = 8; break;
        default: break;
      }
      if (!w || !h || !bpp) {
        REXLOG_INFO("gow2_renderer: target stats f{} after {} draws: {} {}x{} - not decoded", frame_, stats_draws_,
                    what, w, h);
        return;
      }
      const gpu::ReadbackHandle rb = backend_->ReadbackTexture(tex, gpu::Rect{0, 0, int32_t(w), int32_t(h)});
      if (!rb) return;
      backend_->WaitForFence(backend_->Submit());
      uint32_t pitch = 0;
      const uint8_t* data = backend_->ReadbackData(rb, pitch);
      if (!data) { backend_->ReleaseReadback(rb); return; }
      double sum[4] = {}, mn[4], mx[4];
      uint64_t nan = 0, black = 0;
      for (int i = 0; i < 4; ++i) { mn[i] = 1e30; mx[i] = -1e30; }
      uint32_t n = 0;
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          float c[4] = {};
          if (!StatsTexel(fmt, data + size_t(y) * pitch + size_t(x) * bpp, c, n)) break;
          bool all0 = true;
          for (uint32_t i = 0; i < n; ++i) {
            if (std::isnan(c[i])) { ++nan; continue; }
            sum[i] += c[i]; mn[i] = std::min(mn[i], double(c[i])); mx[i] = std::max(mx[i], double(c[i]));
            if (i < 3 && c[i] != 0.0f) all0 = false;
          }
          if (all0) ++black;
        }
      const double px = double(w) * h;
      std::string ch;
      for (uint32_t i = 0; i < n; ++i)
        ch += std::format(" c{} {:.4f} [{:.3g},{:.3g}]", i, sum[i] / px, mn[i], mx[i]);
      REXLOG_INFO("gow2_renderer: target stats f{} after {} draws: {} {}x{} |{} | NaN {} | rgb all 0: {:.2f}%", frame_,
                  stats_draws_, what, w, h, ch, nan, 100.0 * double(black) / px);
      backend_->ReleaseReadback(rb);
    }
  }
  void ResolveDraw(const gpu::ComputeBindings& b, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    if (skip_resolve_dispatches_) { ++stats_.resolve_dispatches_skipped; return; }
    gpu::TextureView views[96];
    views[0].texture = b.srv[0];
    gpu::SamplerDesc samplers[32];
    tex_memo_valid_ = false;
    backend_->SetTextures(views, samplers, 1, 1);
    gpu::TextureHandle t = b.uav_texture;
    backend_->SetRenderTargets(1, &t, 0, false);
    backend_->SetViewport({0.0f, 0.0f, 16384.0f, 16384.0f, 0.0f, 1.0f});
    backend_->SetScissor({int32_t(x0), int32_t(y0), int32_t(x1), int32_t(y1)});
    backend_->SetPipeline(colour_resolve_draw_);
    backend_->SetConstants(gpu::ShaderStage::kPixel, gpu::kCbSystem, b.constants, 64);
    backend_->Draw(3);
    ++stats_.resolve_draws;
  }
  std::chrono::steady_clock::time_point last_lookahead_{};
  bool vertex_any_type_ = true;  // patch 0049
  bool vertex_reuse_ = false, index_reuse_ = false, stencil_transfer_bits_ = false,
       in_selftest_ = false, texture_memo_ = false, tex_memo_valid_ = false;
  uint64_t tex_memo_frame_ = 0, tex_memo_epoch_ = 0, tex_memo_gen_ = 0, tex_gen_ = 0;
  uint32_t tex_memo_masks_[3] = {}, tex_memo_stand_ins_ = 0;
  uint32_t tex_memo_fetch_[32 * 6] = {};
  std::unordered_map<uint64_t, uint64_t> idx_reuse_;  // job 178: index range key -> ring offset (this list + epoch)
  uint64_t idx_reuse_list_ = ~0ull, idx_reuse_epoch_ = ~0ull;
  struct VertexMemo {
    uint64_t gen = 0;
    uint32_t dw0 = 0, dw1 = 0, out0 = 0, out1 = 0;
  };
  VertexMemo vertex_memo_[96];
  uint64_t memo_list_ = ~0ull, memo_epoch_ = ~0ull, memo_gen_ = 1;
  uint64_t transient_list_ = ~0ull, release_epoch_ = 0, transient_epoch_ = ~0ull;
  std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> transient_reuse_;  // (address, size) -> (hash, offset)
  std::set<uint64_t> logged_alias_pairs_;
  std::vector<rr::AliasEvent> aliases_;
  std::vector<uint32_t> indices_, converted_;
  const std::vector<uint32_t>* index_ptr_ = &indices_;  // the draw's indices (indices_ or a cached buffer)
  FrameStats stats_;
  uint64_t frame_ = 0, compared_ = 0, matched_ = 0, incomplete_ = 0, explained_ = 0;
  uint64_t colour_compared_ = 0, colour_matched_ = 0;
  uint32_t draw_index_ = 0, copy_index_ = 0, last_copy_draw_ = 0, drawn_since_copy_ = 0;
  uint32_t pending_this_frame_ = 0, shader_errors_this_frame_ = 0, pipeline_failures_this_frame_ = 0;
  double pipeline_wait_ms_frame_ = 0.0;  // job 037: time waited for new pipelines in this frame
  double depth_wait_left_ms_ = 0.0;      // patch 0132: this frame's wait left for depth-only / shadow pipelines
  size_t frames_in_flight_ = 2;          // patch 0133: --gow2_renderer_frames_in_flight
  // Patch 0063 (load hold): this frame's pending pipelines (one per description), the load window, the hold budget.
  std::vector<gpu::GraphicsPipelineDesc> hold_descs_;
  std::unordered_set<uint64_t> hold_keys_;
  std::chrono::steady_clock::time_point last_swap_end_{}, load_window_until_{};
  double hold_left_ms_ = 0.0;
  double hold_cap_ms_ = 0.0;     // patch 0064: this hold's longest wait
  bool hold_this_frame_ = false;
  bool hold_in_window_ = false;  // patch 0064: within the load window (uses the load budget)
  FrameStats created_logged_;    // patch 0066: the counters at the last log line
  size_t PrebuildWaiting() {  // patch 0068
    std::lock_guard<std::mutex> lock(prebuild_mutex_);
    return prebuild_queue_.size();
  }
  size_t CreatedQueued() {
    std::lock_guard<std::mutex> lock(created_mutex_);
    return created_queue_.size();
  }
  bool memexport_ = false;               // --gow2_renderer_memexport (notes/11)
  bool exports_marked_ = false;          // some memexport range was marked GPU-current (vertex fetch checks pages)
  bool memexport_readback_ = false;      // --gow2_renderer_memexport_readback (job 041)
  bool pending_mirror_ = false;          // --gow2_renderer_export_pending_mirror (patch 0007)
  uint64_t watch_lost_logged_ = 0;       // patch 0008: GuestWatch::GpuPagesLostToWidening at the last summary
  std::vector<std::pair<uint32_t, uint32_t>> pending_spans_;  // patch 0007: scratch, page spans of in-flight readbacks
  struct ExportReadback {
    gpu::ReadbackHandle handle = 0;
    uint32_t base = 0, size = 0;
  };
  std::deque<ExportReadback> export_readbacks_;
  std::map<uint64_t, uint64_t> export_ranges_;  // patch 0042: start -> end, gathered this frame, merged
  uint32_t memexport_logged_ = 0;
  std::unordered_set<uint64_t> memexport_vs_seen_;   // patch 0012: export vertex shaders logged once each
  std::unordered_set<uint64_t> export_reader_seen_;  // patch 0012: (vs hash, slot) pairs reading export ranges
  uint64_t summary_barrier_calls_ = 0, summary_barriers_ = 0, summary_dispatches_ = 0, summary_backend_draws_ = 0,
           summary_submits_ = 0, summary_barrier_batches_ = 0, summary_barriers_merged_ = 0;  // jobs 045/046
  std::string not_drawn_;
  int log_every_ = 300, alias_log_limit_ = 20;
  double cpu_ms_ = 0, cpu_sum_ = 0, cpu_max_ = 0;
  double part_ms_[kPartCount] = {};  // since the last summary (filled from part_ticks_ when it is logged)
  uint64_t part_ticks_[kPartCount] = {};
  struct FrameStepCounter { const char* site = nullptr; uint64_t ticks = 0; };
  std::array<FrameStepCounter, 24> frame_steps_{};
  uint64_t thread_cpu_base_ = 0;
  Part part_cur_ = kPartOther;
  uint64_t part_mark_ = 0;
  bool parts_on_ = false;
  int part_level_ = 0;  // patch 0153: --gow2_renderer_cpu_split, read at each swap (default 0 since 0154)
  int part_level_prev_ = 0;
  uint8_t gpu_cat_ = 0xFF;  // job 177: the GPU category last given to the backend
  bool gpu_by_target_ = false;  // job 234
  // Patch 0101: the draw report (--gow2_renderer_draw_report_frame).
  struct RepRecord {
    uint32_t tag = 0;
    uint64_t frame = 0;
    bool draw = false;
    std::string text;
  };
  uint64_t report_first_ = 0, report_count_ = 1;
  bool report_on_ = false, report_done_ = false, report_samples_ = false;
  uint32_t report_seq_ = 0;
  std::vector<RepRecord> rep_records_;
  std::vector<gpu::GpuTagTime> rep_times_;
  gpu::TextureView rep_views_[96];
  gpu::SamplerDesc rep_samplers_[32];
  uint32_t rep_view_count_ = 0, rep_sampler_count_ = 0;
  uint8_t draw_cat_ = 0;        // job 234: 0x20 + the slot of the target the draws go to
  struct TargetSlot {
    uint32_t id = 0;
    rr::RenderTargetKey key;
    uint32_t width = 0, height = 0;
    uint64_t draws = 0;
    bool reused = false;  // the slot changed hands this window
  };
  TargetSlot target_slots_[32];
  double gpu_target_base_[32] = {};
  double gpu_target_px_base_[32] = {};  // patch 0098
  uint8_t stencil_ones_draws_ = 0, stencil_ones_clears_ = 0, stencil_reads_ = 0;  // job 179
  uint64_t rt_write_gen_ = 0;  // job 182
  uint32_t fill_logged_ = 0;   // job 184
  // Job 304 (rainbow diagnostic): --gow2_renderer_vertex_log_frames.
  struct VertexLogEntry {
    uint64_t frame;
    uint32_t draw, slot, addr, size;
    uint64_t hash;
  };
  std::vector<VertexLogEntry> vertex_log_;
  bool vertex_log_parsed_ = false;
  uint64_t vertex_log_first_ = 0, vertex_log_last_ = 0;
  bool VertexLogFrame() {
    if (!vertex_log_parsed_) {
      vertex_log_parsed_ = true;
      const std::string spec = REXCVAR_GET(gow2_renderer_vertex_log_frames);
      const size_t dash = spec.find('-');
      if (!spec.empty()) {
        vertex_log_first_ = std::strtoull(spec.c_str(), nullptr, 10);
        vertex_log_last_ = dash == std::string::npos ? vertex_log_first_ : std::strtoull(spec.c_str() + dash + 1, nullptr, 10);
      }
    }
    const uint64_t f = frame_ + 1;
    return vertex_log_first_ && f >= vertex_log_first_ && f <= vertex_log_last_;
  }
  static uint64_t HashRange(const uint8_t* p, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 1099511628211ull;
    }
    return h;
  }
  void LogDrawVertices(const uint32_t* r, const rr::GuestShader& vsg, uint32_t prim, uint32_t index_count,
                       uint64_t vs_hash, uint64_t ps_hash, const rr::PipelineState& ps) {
    const uint8_t* mem = memory_->TranslatePhysical<const uint8_t*>(0);
    std::string text;
    for (uint32_t i = 0; i < 96; ++i) {
      if (!((vsg.vfetch_constants[i >> 5] >> (i & 31)) & 1)) continue;
      const uint32_t dw0 = r[kRegFetchConst + 2 * i], dw1 = r[kRegFetchConst + 2 * i + 1];
      if ((dw0 & 3) != 3) continue;
      const uint32_t addr = dw0 & 0x1FFFFFFCu, size = std::min<uint32_t>(((dw1 >> 2) & 0xFFFFFFu) * 4, 4096u);
      if (!size || uint64_t(addr) + size > (512ull << 20)) continue;
      const uint64_t h = HashRange(mem + addr, size);
      vertex_log_.push_back({frame_ + 1, draw_index_, i, addr, size, h});
      char b[96];
      std::snprintf(b, sizeof(b), " | vf%u %08X size %u endian %u hash %016llX:", i, addr, size, dw1 & 3u,
                    static_cast<unsigned long long>(h));
      text += b;
      for (uint32_t k = 0; k < std::min<uint32_t>(size / 4, 16); ++k) {
        uint32_t w;
        std::memcpy(&w, mem + addr + 4 * k, 4);
        w = std::byteswap(w);
        std::snprintf(b, sizeof(b), " %g", std::bit_cast<float>(w));
        text += b;
      }
    }
    REXLOG_INFO("gow2_vertex_log: frame {} draw {} prim {} count {} vs {:016X} ps {:016X} | viewport {} {} {} {} scale {} {} "
                "offset {} {} xy_div_w {} | scissor {},{} {}x{}{}",
                frame_ + 1, draw_index_, prim, index_count, vs_hash, ps_hash, ps.viewport.x, ps.viewport.y,
                ps.viewport.width, ps.viewport.height, ps.viewport.ndc_scale[0], ps.viewport.ndc_scale[1],
                ps.viewport.ndc_offset[0], ps.viewport.ndc_offset[1], ps.viewport.xy_divided_by_w ? 1 : 0, ps.scissor.x,
                ps.scissor.y, ps.scissor.width, ps.scissor.height, text);
  }
  void RecheckDrawVertices() {
    const uint8_t* mem = memory_->TranslatePhysical<const uint8_t*>(0);
    uint32_t changed = 0;
    for (const VertexLogEntry& e : vertex_log_) {
      const uint64_t h = HashRange(mem + e.addr, e.size);
      if (h == e.hash) continue;
      ++changed;
      if (changed <= 64)
        REXLOG_INFO("gow2_vertex_log: frame {} draw {} vf{} {:08X} size {} CHANGED after the draw (by the swap): hash {:016X} "
                    "-> {:016X}", e.frame, e.draw, e.slot, e.addr, e.size, e.hash, h);
    }
    REXLOG_INFO("gow2_vertex_log: frame {} swap: {} logged ranges, {} changed after their draw", frame_ + 1,
                vertex_log_.size(), changed);
    vertex_log_.clear();
  }
  struct FillRect {
    bool valid = false, depth = false, stencil = false;
    gpu::Rect rect{};
  } fill_;                       // job 185: this draw's exact fill rectangle (host pixels) and what it overwrites
  uint32_t fill_rects_logged_ = 0;
  bool fill_trim_ = false;
  struct FillGeom {
    bool valid = false;
    rr::PixelRect rect{};
  } fill_geom_;  // job 188: the fill shader's conservative bounds (host pixels, scissor-clipped)
  bool fill_ownership_ = false;
  bool fill_rect_lists_ = true;  // Gears 3 job 032
  void Wrote(rr::RenderTarget& t) { t.write_gen = ++rt_write_gen_; }
  struct ReadOnlyMemo {
    uint64_t t_gen = 0, s_gen = 0;
    gpu::Rect rect{};
  };
  std::unordered_map<uint64_t, ReadOnlyMemo> readonly_memo_map_;
  bool readonly_memo_ = false;
  struct XferPair {
    std::string text;
    uint64_t px = 0, runs = 0, clear_px = 0, read_px = 0, overwrite_px = 0;
    uint64_t vs = 0, ps = 0;  // the last draw that asked for one
    uint32_t prim = 0, depth_func = 0, depth_write = 0, stencil = 0, stencil_wmask = 0, colour = 0;
  };
  struct XferDraw {
    uint64_t vs = 0, ps = 0;
    uint32_t prim = 0, depth_func = 0, depth_write = 0, stencil = 0, stencil_wmask = 0, colour = 0;
  };
  XferDraw xfer_draw_;
  std::map<std::pair<uint32_t, uint32_t>, XferPair> xfer_pairs_;  // job 180: depth transfers by target pair (window)

  uint64_t xfer_px_base_[4] = {};
  double gpu_wait_base_ms_ = 0;  // backend wait total at the last summary
  double gpu_ms_base_ = 0;  // job 176: backend GPU-time total at the last summary
  uint64_t gpu_lists_base_ = 0;
  double gpu_cat_base_[8] = {};
  bool lost_logged_ = false;
  // ---- occlusion queries (Stage 4, upl-s4c) ----
  enum class QueriesMode { kOff, kShadow, kAnswer, kStrict };
  QueriesMode queries_mode_ = QueriesMode::kOff;
  uint64_t queries_min_ = 0;
  bool queries_correct_ = true;
  int queries_log_ = 0;
  struct OpenOcclusion {
    uint32_t begin_address = 0;
    gpu::OcclusionHandle handle = 0;
    uint64_t draws = 0, drawn = 0;  // stats_ at the begin ZPD
    bool open = false;
    uint32_t scale = 0;  // patch 0141: phase scale at the begin (0 = none)
  };
  OpenOcclusion occ_open_;
  struct PendingOcclusion {
    uint32_t end_address = 0;
    gpu::OcclusionHandle handle = 0;
    uint64_t frame = 0, sequence = 0, estimate = 0;
    bool complete = false;
    uint32_t draws = 0;
    bool log = false, from_cache = false;
    uint32_t scale = 1;  // patch 0141: host pixels per guest pixel of the draws it counted
    std::chrono::steady_clock::time_point queued_at{};
  };
  std::deque<PendingOcclusion> occ_pending_;
  rr::queries::AnswerCache occ_answers_;
  rr::queries::DeferredReports occ_deferred_;
  bool occ_need_submit_ = false;
  uint32_t occ_closed_in_batch_ = 0, occ_batches_this_frame_ = 0;
  struct OcclusionStats {
    uint64_t ends = 0, ours = 0, incomplete = 0, refused = 0, unended = 0, answered = 0, from_cache = 0, fallback = 0,
             results = 0, zero = 0, samples = 0, latency_frames = 0, corrected = 0, too_late = 0,
             visibility_changed = 0, dropped = 0, sdk_fake = 0, sdk_host = 0, proxies = 0, zero_not_trusted = 0,
             answered_hidden = 0, wrong_hidden = 0, completion_notifications = 0;
  };
  OcclusionStats occ_stats_;
  uint64_t occ_backend_segments_base_ = 0, occ_backend_refused_base_ = 0;
  uint32_t occ_lines_ = 0;
};

}  // namespace

namespace {
bool g_renderer_presents = false;  // 5C-2: set once at install (before the GPU thread runs the ring)
ShadowRenderer* g_release_target = nullptr;  // job 171: set once at install, like g_renderer_presents
}  // namespace

void XenosPrebuildShader(bool vertex, const void* bytes, uint32_t size, uint32_t variant, uint64_t hash) {
  if (g_release_target) g_release_target->OnPrebuildShader(vertex, bytes, size, variant, hash);
}
void XenosBeginStartupPrebuild() { if (g_release_target) g_release_target->BeginStartupPrebuild(); }
bool XenosFinishStartupPrebuild() { return !g_release_target || g_release_target->FinishStartupPrebuild(); }
bool XenosPrebuildWanted() { return g_release_target && g_release_target->PrebuildWanted(); }  // patch 0068
void XenosPrebuildPixelShader(const void* be_ucode, uint32_t size_bytes, uint32_t variant, uint64_t ucode_hash) {  // 0068
  if (g_release_target) g_release_target->OnPrebuildShader(false, be_ucode, size_bytes, variant, ucode_hash);
}
uint64_t XenosPrebuildVariantHits(uint32_t variant) {                                           // patch 0068
  return g_release_target ? g_release_target->PrebuildVariantHits(variant) : 0;
}

void XenosGuestShaderCreated(bool is_vertex, uint32_t guest_address, uint32_t size_bytes) {  // patch 0066
  if (g_release_target && REXCVAR_GET(gow2_renderer_shader_precreate))
    g_release_target->OnGuestShaderCreated(is_vertex, guest_address, size_bytes);
}

bool XenosRendererPresents() { return g_renderer_presents; }
bool XenosRendererStrictQueries() { return g_release_target && g_release_target->StrictQueries(); }
bool XenosPollRendererIdle() { return g_release_target && g_release_target->PollGpuIdle(); }
void* XenosRendererIdleEvent() { return g_release_target ? g_release_target->IdleGpuEvent() : nullptr; }
void XenosRendererRelease() {
  if (g_release_target) g_release_target->ReleaseToGuest();
}

void XenosInstallRendererIfRequested(rex::Runtime* runtime, const XenosRendererHints& hints) {
  const std::string mode = REXCVAR_GET(gow2_renderer);
  if (mode.empty() || mode == "off") return;
  bool takeover = mode == "takeover";
  if (mode != "shadow" && !takeover) {
    REXLOG_WARN("gow2_renderer: unknown mode '{}' (off | shadow | takeover); staying off", mode);
    return;
  }
  if (takeover && REXCVAR_GET(gow2_renderer_api) == "vulkan")  // notes/68 (0120: the V2 "no picture" text was stale)
    REXLOG_INFO("gow2_renderer: Vulkan requested - our renderer draws and presents through Vulkan when the build has "
                "the Vulkan backend (the next lines say which API is on; D3D12 if Vulkan is missing)");
  if (takeover) {
    // Takeover preset (notes/50, jobs 123-127): our image on screen, our resolves feed our textures, our occlusion
    // answers. The emulator's drawing is switched off below, only once our renderer is up.
    REXCVAR_SET(gow2_renderer_display, std::string("ours"));
    REXCVAR_SET(gow2_renderer_resolves_to_guest, true);
    // Occlusion queries: no answers of ours by default (jobs 139-140: our answers arrive ~3 frames late, so objects
    // coming into view vanish for a few frames - Shagg saw distant surfaces flicker; with nothing written the game sees
    // the fake "visible" count for every query, as on the emulator, and nothing flickers). --gow2_renderer_queries=answer
    // brings them back for testing until our results land within about a frame (a speed item).
  }
  // Never destroyed: the device lives until the process exits (static destruction order vs the D3D12 runtime is not
  // worth the risk).
  auto* renderer = new ShadowRenderer(hints);
  if (!renderer->Init(runtime)) {
    delete renderer;
    return;
  }
  XenosRegisterDrawObserver(renderer);
  g_renderer_presents = renderer->PresentsOurs();
  g_release_target = renderer;
  if (takeover && XenosOurGpuSystem()) {
    REXLOG_INFO("gow2_renderer: TAKEOVER on our graphics system - no SDK GPU emulation is loaded");
  } else if (takeover) {
    if (rex::cvar::SetFlagByName("gpu_skip_draws", "true"))  // SDK patch 19
      REXLOG_INFO("gow2_renderer: TAKEOVER - the emulator draws nothing; our renderer draws, resolves and presents");
    else
      REXLOG_ERROR("gow2_renderer: takeover asked but the SDK has no gpu_skip_draws (patch 19); the emulator still draws");
  }
}
