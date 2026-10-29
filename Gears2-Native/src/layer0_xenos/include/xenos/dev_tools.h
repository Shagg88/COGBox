// Layer 0 (Xbox 360 GPU core and game-free tooling). Nothing here may include layer 1-3 headers.
// Developer tools that work for any 360 title: crash report, scripted gamepad, image dump.
#pragma once

#include <cstdint>

namespace rex { class Runtime; }
namespace rex::system { class XThread; }
namespace rex::graphics { class DrawObserver; class RegisterFile; struct ObservedIndexBuffer; }
namespace rex::ui { class ImGuiDrawer; class Window; }

// Crash report (unwound stack + minidump) for unhandled host exceptions; --gow2_crash_dir.
void XenosInstallCrashHandler();
// Patch 0131: a description of the PC (Windows / Wine, CPU, memory, graphics cards and drivers, screens, controllers,
// build, command line) at the top of the log. system_report.cpp.
void XenosLogSystemReport(const char* game);
// Patch 0135 (latency_reflex.cpp): NVIDIA Reflex when --gow2_reflex is on and the build has NVAPI. Install once at
// start-up; the renderer hands over its D3D12 device and reports each submitted frame.
void XenosLatencyInstall();
void XenosLatencySetDevice(void* d3d12_device);
void XenosLatencyFrameSubmitted();
// Patch 0146 (renderer/frame_generation.cpp): frame generation (--gow2_frame_generation). XenosLatencyInstall calls it, so
// every game that installs the latency tools gets it.
void XenosFrameGenerationInstall();
// Patch 0147: frame generation step 3. The renderer asks whether to hand over inputs, then for each image it writes
// into the presenter (the guest-output texture, ID3D12Resource*) gives that frame's inputs, or null when the frame has
// no 3D scene (frame generation pauses). Depth / motion are ID3D12Resource* in NON_PIXEL_SHADER_RESOURCE state, at the
// render size, kept unchanged until the same guest-output texture is written again.
struct XenosFgFrame {
  void* depth = nullptr;   // R32F, guest convention (0 near .. 1 far)
  void* motion = nullptr;  // R16G16F, previous minus current position, render pixels
  uint32_t width = 0, height = 0;
  float jitter_x = 0.0f, jitter_y = 0.0f;
  float vertical_fov = 1.0f, camera_near = 10.0f, camera_far = 100000.0f, frame_ms = 16.7f;
  bool reset = true, infinite = false;
  uint64_t frame = 0;      // the renderer's frame number (a gap = an image that was never shown)
  float camera_position[3] = {}, camera_up[3] = {}, camera_right[3] = {}, camera_forward[3] = {};
};
bool XenosFrameGenerationWanted();
void XenosFrameGenerationSetInputs(void* guest_output_resource, const XenosFgFrame* frame);
// --gow2_input_script: a scripted gamepad for unattended tests.
void XenosAddScriptPadIfRequested(rex::Runtime* runtime);
// --gow2_dump_image: write the loaded game image for tools/scan_vtables.py.
void XenosDumpImageIfRequested(rex::Runtime* runtime);
// --gow2_draw_stats=N: log per-frame draw/copy counts every N frames (uses SDK patch 2, the draw observer).
void XenosInstallDrawStatsIfRequested();
// The one SDK draw observer is a hub in layer 0; tools register with it (before the game starts).
void XenosRegisterDrawObserver(rex::graphics::DrawObserver* observer);
// --gow2_frame_shots=N and --gow2_frame_dir: save every Nth finished frame as PNG, read back inside the process
// (SDK patch 3) - no window capture, no focus stealing.
void XenosInstallFrameCaptureIfRequested();
// The hub without any tool: needed for the swap count alone.
void XenosEnsureObserverHub();
// Swaps seen so far (counted by the hub on the GPU worker thread; 0 until the hub is installed).
uint64_t XenosSwapCount();
// --gow2_fps_bar=top|bottom: a one-line frame-rate bar drawn by us over the game (no game patch). Create from the
// app's OnCreateDialogs; destroy from OnShutdown (before the SDK drops its ImGui drawer).
void XenosCreateFpsBarIfRequested(rex::ui::ImGuiDrawer* drawer);
void XenosDestroyFpsBar();
// Patch 0163: COGBox's in-game menu (ingame_menu.cpp) - Back + Start or F1 opens it; COGBox's settings for this game
// (--cogbox_menu=<file>), Resume, Restart to apply, Quit game. Install from OnCreateDialogs (with the app's window and
// runtime), shut down from OnShutdown.
void XenosInstallInGameMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, rex::Runtime* runtime);
void XenosShutdownInGameMenu();
// Patch 0062: --gow2_shader_note (default on) - a small "Building shaders N%" note in the bottom-left corner while the
// renderer's pipeline workers have work queued for more than a second (created and destroyed with the FPS bar, shown
// whatever the bar's setting). The renderer reports, once a frame: jobs left (queued + being built), jobs ever queued.
void XenosSetShaderJobs(uint64_t left, uint64_t ever_queued);
// Patch 0067: while the renderer holds a frame back (the game paused behind it) an animated "Loading shaders N%"
// indicator is drawn over the last picture; the renderer reports the held frame's pipelines and how many are left.
void XenosSetLoadHold(bool active, uint32_t pipelines_total, uint32_t pipelines_left);
// Patch 0066: a game's shader-creation hook (layer 3) reports each shader the game creates - its microcode's guest
// address and size in bytes - so the renderer can translate it and have the driver build it before the first draw.
// Any thread; no-op unless our renderer runs (--gow2_renderer_shader_precreate, default on).
void XenosGuestShaderCreated(bool is_vertex, uint32_t guest_address, uint32_t size_bytes);
// Patch 0068: a pixel shader found in a package the game just opened (layer 2 reads the file) - its microcode as stored
// (big-endian dwords) - to translate like a draw's and put through the driver before its first draw. Any thread;
// copies the bytes. XenosPrebuildWanted: false unless our renderer runs the DXBC path with
// --gow2_renderer_shader_prebuild (default on), so layer 2 reads nothing otherwise.
// `variant` says which part of the game's shader record this is (layer 2: 0 = the whole microcode region, 1 = the code
// after its literal-constant block); XenosPrebuildVariantHits(v) = new draw pipelines so far whose pixel shader was a
// prebuilt one of that variant, so layer 2 can stop offering a variant a game never draws.
bool XenosPrebuildWanted();
void XenosPrebuildPixelShader(const void* be_ucode, uint32_t size_bytes, uint32_t variant);
// notes/78 C3: a vertex shader found in a package (its record's whole microcode region, big-endian). Used only with
// --gow2_renderer_recipe_pack: the recipe vertex shaders it rebuilds exactly are prepared. Any thread; copies.
void XenosPrebuildVertexShader(const void* be_ucode, uint32_t size_bytes);
uint64_t XenosPrebuildVariantHits(uint32_t variant);
// --gow2_frame_times=N: every N swaps log swap interval (mean/p99/max) and the observer callbacks' own time per frame.
void XenosInstallFrameTimesIfRequested();
// --gow2_profile=S (--gow2_profile_start, --gow2_profile_top): sampling profiler of every busy thread (notes/52).
void XenosInstallProfilerIfRequested();
// --gow2_draw_log_frames="1200,1350" and --gow2_draw_log_dir: per-draw log (state/constant hashes) of chosen frames.
void XenosInstallDrawLogIfRequested();
// --gow2_resolve_log_frames="1500,2400": log every EDRAM copy/clear of those frames (source, destination, formats).
void XenosInstallResolveLogIfRequested();
// --gow2_buffer_dump_frame=N (+ --readback_resolve=full): save every resolve of frame N as EXR/PNG.
void XenosInstallBufferDumpIfRequested(rex::Runtime* runtime);
// --gow2_shader_dump_dir: write every distinct shader (microcode + the SDK disassembly) once (Stage 3 corpus).
void XenosInstallShaderDumpIfRequested();
// --gow2_watch_shaders="ps-<hash>,...": log the loop constants seen by draws using those shaders.
void XenosInstallShaderWatchIfRequested();
// --gow2_hold_guest: keep the game's main thread suspended (for the SDK's --trace_gpu_replay).
void XenosHoldGuestIfRequested(rex::system::XThread* main_thread);
// --gow2_prim_detail=N: tessellation / 0x10+ primitive detail, point-run lengths, quad-list shaders (job 094).
void XenosInstallPrimDetailIfRequested();
// --gow2_renderer=takeover (set by --gow2_gpu=ours, the default): our own D3D12 renderer draws, resolves and presents
// (notes/44, 50). Also --gow2_shader_cache_dir, --gow2_dxc_dir, --gow2_renderer_debug, --gow2_renderer_log_every,
// --gow2_renderer_log_aliases, --gow2_shader_threads.
// The hints name the D3D library's fixed shaders by microcode hash (they belong to the XDK's D3D, not to layer 0):
// the fast-clear pair drawn as a half-pitch 4x MSAA rectangle, and the vertex shader of its no-op single-point draws.
// 0 = not known (no clear detection; point draws of one vertex are then all treated as no-ops).
struct XenosRendererHints {
  uint64_t clear_vs = 0, clear_ps = 0;
  uint64_t dummy_point_vs = 0;
};
void XenosInstallRendererIfRequested(rex::Runtime* runtime, const XenosRendererHints& hints = {});
// --gow2_query_log=N: log every occlusion query of every Nth frame (report addresses, the draws each covers, the
// emulator's answer read back from guest memory) plus per-frame and per-N-frame counts (Stage 4 reference, notes/46).
// The hint is D3D library knowledge (layer 1): the end report's offset from the begin report, in bytes (XDK 7645:
// -32). 0 = not known (addresses only, no answers). Also --gow2_query_log_max_lines, --gow2_query_log_fake.
struct XenosQueryLogHints {
  int32_t end_minus_begin = 0;
};
void XenosInstallQueryLogIfRequested(rex::Runtime* runtime, const XenosQueryLogHints& hints = {});
// --gow2_state_coverage=N (Stage 5, notes/49): every N frames, the GPU registers 0x2000-0x3FFF the game writes that our
// renderer does not read - changing ones with change counts and sample values, then ones set once.
void XenosInstallStateCoverageIfRequested();
// --gow2_pm4_shadow=N (Stage 5B, notes/49): our own PM4 processor runs on every ring range (SDK patch 20) and the GPU
// state it reaches is compared with the SDK's at every draw; summary every N frames.
void XenosInstallPm4ShadowIfRequested(rex::Runtime* runtime);
// Stage 5B phase 3 (notes/49): shader ucode as our PM4 processor holds it - host-order dwords; hash = XXH3-64 of the
// guest's big-endian bytes, i.e. the SDK's Shader::ucode_data_hash, so caches keyed on it stay valid.
struct XenosUcode {
  uint64_t hash = 0;
  const uint32_t* dwords = nullptr;
  uint32_t count = 0;
};
// Patch 0051: the shaders of the draws our PM4 processor has queued for the renderer but the renderer has not run yet
// (the rest of the block it is in, then the queued blocks), up to max_draws - called on the renderer thread, so a draw
// about to wait for its shader can start compiling the next ones on the worker threads too. No-op without the split.
using XenosUcodeVisitor = void (*)(void* ctx, const XenosUcode& vertex, const XenosUcode& pixel);
uint32_t XenosLookaheadUcode(XenosUcodeVisitor visitor, void* ctx, uint32_t max_draws);
// Patch 0053: shaders our PM4 processor met for the first time since the last call (it records them as it parses a
// draw, ahead of the renderer), visited once each with pixel = {} or vertex = {}; renderer thread. No-op without the
// split. Returns how many.
uint32_t XenosTakeNewUcode(XenosUcodeVisitor visitor, void* ctx);
bool XenosNewUcodePending();
// Observers that can take ucode directly (our renderer) implement this next to DrawObserver.
class XenosUcodeSink {
 public:
  virtual ~XenosUcodeSink() = default;
  virtual void OnDrawUcode(const XenosUcode& vertex, const XenosUcode& pixel) = 0;
};
// A draw/copy our processor ran: every hub observer gets OnDrawUcode (sinks) or OnDrawShaders(nullptr, nullptr),
// then OnDraw - the same order the SDK used.
void XenosDispatchOurDraw(const rex::graphics::RegisterFile& regs, uint32_t primitive, uint32_t index_count,
                          const rex::graphics::ObservedIndexBuffer* index_buffer, bool major_mode_explicit,
                          const XenosUcode& vertex, const XenosUcode& pixel);
// Stage 5C-2: an XE_SWAP our processor ran: every hub observer gets OnSwap, as from the SDK.
void XenosDispatchOurSwap(const rex::graphics::RegisterFile& regs, uint32_t frontbuffer, uint32_t width,
                          uint32_t height);
// Stage 5C-2: whether any hub observer wants frame images (frame shots), and our presented image for them (in place
// of the SDK's patch-3 readback, which needs the SDK's swap).
bool XenosFrameImagesWanted();
void XenosDispatchFrameImage(const uint8_t* rgb10a2, uint32_t width, uint32_t height, uint32_t row_pitch,
                             uint64_t frame);
// Stage 5C-2: true once our renderer presents its own image (--gow2_present=ours on the presenter's device); our PM4
// processor then runs XE_SWAP itself.
bool XenosRendererPresents();
// Job 171: our renderer finishes every guest-memory copy it deferred (to its copy threads) - called before the title's
// CPU can learn the GPU got past them. No-op without our renderer or without copy threads.
void XenosRendererRelease();
// Job 173: with --gow2_pm4_split, queue a guest-memory write (at most 16 bytes, guest byte order) for the renderer
// thread to make once it has run everything queued before it; false (write it yourself) without the split. GPU
// thread only.
bool XenosPm4DeferWrite(uint32_t physical, const void* bytes, uint32_t size);
// Job 225 (notes/60): every guest-memory write made for the title from our GPU side (a fence, the read pointer) bumps
// a counter. A title thread waiting on the GPU can sleep until it changes (XenosWaitGuestWrite, at most timeout_ms)
// instead of spinning. Called by the writer: XenosNotifyGuestWrite (the renderer thread's deferred writes, the GPU
// thread's own read-pointer write).
uint32_t XenosGuestWriteSeq();
void XenosNotifyGuestWrite();
void XenosWaitGuestWrite(uint32_t seen, uint32_t timeout_ms);
// Job 240 (notes/60): generations of the shader constant ranges as our render pipe applies register changes (VS floats
// 0x4000-0x43FF, PS floats 0x4400-0x47FF, bool + loop 0x4900-0x4927): a counter moves whenever a write lands in its
// range or the whole file is reloaded. valid only while the pipe feeds the renderer (otherwise compare as before).
struct XenosConstGenerations {
  uint64_t vs = 0, ps = 0, bool_loop = 0;
  bool valid = false;
};
XenosConstGenerations XenosConstGens();
// An EVENT_WRITE_ZPD our processor ran (report already written): every hub observer gets OnZpd, as from the SDK.
void XenosDispatchOurZpd(const rex::graphics::RegisterFile& regs, uint32_t report_address, bool is_end,
                         uint8_t answer, uint64_t samples);

// Frame log (frame_log.cpp; --gow2_frame_log, off by default): every frame's time and what it went on. Writes a
// summary line every --gow2_frame_log_every frames
// (mean, p50/p95/p99, max, 1% low, frame-to-frame jitter, spikes by cause), one line per spike and, with
// --gow2_frame_log_csv, <log name>-frames.csv next to the log file (one row per frame). The game frame is
// measured on the GPU thread (swap to swap as the game hands them over), the shown frame on our renderer thread; the
// waits below are charged to the game frame they happened in.
enum XenosWaitKind : uint32_t {
  kXenosWaitGameGpu = 0,  // a game thread in the D3D library's wait for the GPU (read pointer / fence)
  kXenosWaitGameQuery,    // a game thread in UE3's wait for one GPU query's answer
  kXenosWaitPm4,          // the GPU thread at a WAIT_REG_MEM (the command ring stops until it matches)
  kXenosWaitRenderer,     // the GPU thread at a swap, waiting for our renderer thread to catch up
  kXenosWaitCount
};
// One step of a game's wait loop (call XenosFrameWaitBegin at the top of the step, XenosFrameWaitEnd at its end, or
// use XenosFrameWaitStep): the time from one step to the next counts when the next one starts within 200 us of the
// previous one's end; a longer gap starts a new wait.
struct XenosWaitClock {
  uint64_t begin = 0, end = 0;
};
void XenosFrameWaitBegin(uint32_t kind, XenosWaitClock& clock);
void XenosFrameWaitEnd(XenosWaitClock& clock);
struct XenosFrameWaitStep {
  XenosFrameWaitStep(uint32_t kind, XenosWaitClock& clock) : clock_(clock) { XenosFrameWaitBegin(kind, clock); }
  ~XenosFrameWaitStep() { XenosFrameWaitEnd(clock_); }
  XenosWaitClock& clock_;
};
bool XenosFrameLogOn();
void XenosFrameAddWait(uint32_t kind, double ms, uint32_t waits = 1);  // a wait measured by its caller
void XenosFrameGuestSwap();  // GPU thread: the game handed over a frame (its swap)
// Our renderer thread, at the end of each swap: what that frame cost on our side.
struct XenosRenderedFrame {
  double cpu_ms = 0;            // renderer thread busy on the frame (includes the three waits below)
  double gpu_wait_ms = 0;       // of which waiting for our GPU (fences: frames in flight, readbacks)
  double hold_ms = 0;           // of which a shader / pipeline hold (load hold, compile step)
  double pipeline_wait_ms = 0;  // of which draws waiting for their new pipelines
  double gpu_ms = 0;            // GPU time of our command lists that finished since the previous frame
  const char* top_part = "";    // biggest renderer CPU part (--gow2_renderer_cpu_split >= 1), "" when not measured
  double top_part_ms = 0;
  uint32_t draws = 0, skipped_draws = 0, pipelines_new = 0, shaders_new = 0;
  double upload_mb = 0;
};
void XenosFrameRendered(const XenosRenderedFrame& frame);
