// Gears1-Native (Gears of War, 2006) layer 3: this game's own patches. Started empty (2026-10-30): every patch here must
// be measured on Gears 1 first; Judgment's addresses don't apply. The functions gow1_app.h calls are kept so the app
// matches the other three games' shape.
#include <atomic>
#include <cstdlib>
#include <format>
#include <set>
#include <string>
#include <chrono>
#include <thread>
#include <immintrin.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <xenos/dev_tools.h>
#include <ue3/config_bundle.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(gow1_mute_unfocused, false, "Gears 1", "Mute the game's sound while its window isn't in front");
REXCVAR_DEFINE_INT32(gow1_memory_log_seconds, 0, "Gears 1",
                     "Developer: log the guest's free physical memory every N seconds (0 = off)");

// ---- Launcher options (2026-10-30) ----------------------------------------------------------------------------------
// Gears 1's engine (UE3, 2006) has one config bundle, WarGame\Config\Xenon\Cooked\Coalesced.ini (layer 2 reads its flat
// layout), and no [SystemSettings]: no MaxAnisotropy, MipFilter, mip fade or startup-movie list. So:
//  - texture pool: the game's own [TextureStreaming] PoolSize (140 MB in Xe-WarEngine.ini), through --ue3_ini_edits;
//  - 16x anisotropic and trilinear: the shared renderer's own filtering switches (the game's samplers, made better);
//  - skip logos: the start-up film player passes the two logo films (EpicLogo, MGSLogo); the rating screen, the
//    title film and every cutscene stay.
REXCVAR_DEFINE_BOOL(gow1_aniso16, false, "Gears 1", "16x anisotropic filtering on the game's filtered textures");
REXCVAR_DEFINE_BOOL(gow1_trilinear, true, "Gears 1", "Smooth blending between texture detail levels (trilinear)");
REXCVAR_DEFINE_INT32(gow1_texture_pool_mb, 0, "Gears 1",
                     "Texture streaming pool in MB through the game's own [TextureStreaming] PoolSize (140). 0 = the game's own");
REXCVAR_DEFINE_BOOL(gow1_skip_logos, false, "Gears 1", "Skip the Epic and Microsoft logo films at start-up");
REXCVAR_DEFINE_BOOL(gow1_ini_edits, true, "Gears 1",
                    "Marker for the launcher: this build edits Gears 1's Coalesced.ini (--ue3_ini_edits)");
REXCVAR_DECLARE(std::string, ue3_ini_edits);
REXCVAR_DECLARE(std::string, ue3_ini_bundle_dir);

void Gow1AddBuiltInIniEdits() {
  const auto game_default = [](const char* name, const std::string& value) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
  };
  game_default("ue3_ini_bundle_dir", "WarGame\\Config\\Xenon\\Cooked");
  if (REXCVAR_GET(gow1_aniso16)) game_default("gow2_renderer_force_aniso", "16");
  if (REXCVAR_GET(gow1_trilinear)) game_default("gow2_renderer_force_trilinear", "true");
  std::string add;
  if (const int32_t mb = REXCVAR_GET(gow1_texture_pool_mb); mb > 0 && mb <= 400)
    add += std::format("Xe-WarEngine.ini|TextureStreaming|PoolSize={};", mb);
  if (!add.empty()) rex::cvar::SetFlagByName("ue3_ini_edits", add + REXCVAR_GET(ue3_ini_edits));   // the player's last
  REXLOG_INFO("gow1: options - aniso16 {}, trilinear {}, pool {} MB, skip logos {}", REXCVAR_GET(gow1_aniso16),
              REXCVAR_GET(gow1_trilinear), REXCVAR_GET(gow1_texture_pool_mb), REXCVAR_GET(gow1_skip_logos));
}

// The start-up check of Coalesced.ini (gow1_manifest.toml): skipped only when the config edits gave the game an edited
// copy, so an unedited game is checked as on the console.
static bool Gow1EditedConfig() { return !REXCVAR_GET(ue3_ini_edits).empty(); }
void Gow1CoalescedResult(PPCRegister& r3) { if (Gow1EditedConfig()) r3.u64 = 0; }
bool Gow1CoalescedSkip() {
  if (!Gow1EditedConfig()) return false;
  static bool logged = false;
  if (!logged) { logged = true; REXLOG_INFO("gow1: config check skipped for the edited Coalesced.ini"); }
  return true;
}

void Gow1HideLogosIfRequested(rex::Runtime*) {}   // job 694: hiding the films crashed the player - see Gow1SkipLogoStep
void Gow1SkipLogoStep(PPCRegister& r11) {   // gow1_manifest.toml: the start-up film index, after each step
  if (!REXCVAR_GET(gow1_skip_logos) || (r11.u32 != 1 && r11.u32 != 2)) return;
  static bool logged = false;
  if (!logged) { logged = true; REXLOG_INFO("gow1_skip_logos: the Epic and Microsoft logo films passed"); }
  r11.u64 = 3;
}
void Gow1SkipLogoStep2(PPCRegister& r11) { Gow1SkipLogoStep(r11); }

// 0.3.0 tidy-up: the job 708 chapter-jump experiment (URL starts) is gone with the launcher's "Start at". Its three
// hooks stay in gow1_manifest.toml (the generated code calls them) but do nothing now - no per-tick check or logging.
void Gow1LaunchChapterTick(PPCRegister&) {}
void Gow1LaunchChapterFlag(PPCRegister&) {}
void Gow1LaunchChapterIndexHook(PPCRegister&) {}

namespace {
std::atomic<bool> g_memory_gauge_stop{false};
}  // namespace

void Gow1StartMemoryGauge() {
  const int every = REXCVAR_GET(gow1_memory_log_seconds);
  if (every <= 0) return;
  REXLOG_INFO("gow1_memory_log_seconds: not built yet for Gears 1 (asked for every {} s)", every);
}
void Gow1StopMemoryGauge() { g_memory_gauge_stop.store(true); }
void Gow1ApplyUltrawideIfRequested(rex::Runtime*) {}

// --gow1_tiling_off (job 680, test): Xenia Canary's "Black Shading Fix" for Gears 1 (boma, game-patches
// 4D5307D5): "solves most dynamic lighting artifacts caused by an issue with light environments and MSAA tiling" by
// turning the engine's predicated tiling off - GUseTilingCode = FALSE, GTilingMode = 4. Its addresses are the disc
// build's (0x82BB6BBB / 0x82BB6B53); in the TU5 image the same data block sits 0x3E0BE0 higher (the 128 bytes around
// both match exactly, notes/03). Written once after the image is loaded, only when the old value is the disc's (1).
// On by default since job 699 (30 Oct): the console split each 720p frame into two tiles to fit its 10 MB EDRAM, so
// every scene draw was sent twice; on a PC that only doubled our renderer's work. One pass: Ephyra uncapped 13.8 ->
// 8.5 ms a frame (job 697), and the same frame matched split on / off to under 1/255 on average (job 699, edges aside -
// the game's 2x MSAA goes with the tiles; the player's anti-aliasing setting, SMAA by default here, smooths them).
REXCVAR_DEFINE_BOOL(gow1_tiling_off, true, "Gears 1",
                    "One pass per frame: the engine's predicated tiling (the console's EDRAM split) off. "
                    "false = the console's two tiles with its 2x MSAA");

void Gow1ApplyDataPatchesIfRequested(rex::Runtime* runtime) {
  if (!REXCVAR_GET(gow1_tiling_off) || !runtime || !runtime->memory()) return;
  struct Poke { uint32_t addr; uint8_t expect, value; const char* what; };
  for (const Poke& k : {Poke{0x82F9779Bu, 1, 0, "GUseTilingCode"}, Poke{0x82F97733u, 1, 4, "GTilingMode"}}) {
    uint8_t* p = runtime->memory()->TranslateVirtual<uint8_t*>(k.addr);
    if (!p || *p != k.expect) { REXLOG_WARN("gow1_tiling_off: {} at {:08X} is {}, not {} - not changed", k.what, k.addr, p ? int(*p) : -1, k.expect); continue; }
#ifdef _WIN32
    DWORD old_protect = 0;
    if (!VirtualProtect(p, 1, PAGE_READWRITE, &old_protect)) { REXLOG_WARN("gow1_tiling_off: {} could not be unlocked", k.what); continue; }
    *p = k.value;
    VirtualProtect(p, 1, old_protect, &old_protect);
#else
    *p = k.value;
#endif
    REXLOG_INFO("gow1_tiling_off: {} {} -> {}", k.what, k.expect, k.value);
  }
}

// --gow1_max_fps (job 689): Gears 1's frame-rate limit. The 2006 engine has no MaxSmoothedFrameRate (none in its
// config) and nothing else holds it back once vsync is off (job 688: ~80 fps in Ephyra, uncapped; Canary's notes warn
// of physics and sound timing trouble above ~100 fps). The hook sits in D3D's present-interval choice (TU5 0x82250DEC,
// after `li r10,2` / `li r10,1` / `li r10,3` - the interval in r10; the disc build's 0x8223E7B8 is Canary's 60 fps
// patch site), which the game's render thread reaches once per frame [G: checked by the counter below]. With a limit
// it waits until the next frame's time (sleep, then a short spin), so frames are evenly spaced. 0 = the game's own
// rate (30 with vsync), N = N fps. Needs --gow2_gpu_vsync=false above 30 (the renderer then ignores interval 2).
REXCVAR_DEFINE_INT32(gow1_max_fps, 0, "Gears 1",
                     "Frame-rate limit: 0 = the game's own (30 with vsync), N = at most N fps (above 30 also needs "
                     "--gow2_gpu_vsync=false)");

void Gow1PresentInterval(PPCRegister& r10) {
  static std::atomic<uint64_t> calls{0};
  const uint64_t n = calls.fetch_add(1) + 1;
  if (n == 1 || n == 600) REXLOG_INFO("gow1_max_fps: present-interval site reached {} time(s) (interval {})", n, uint32_t(r10.u32));
  const int32_t cap = REXCVAR_GET(gow1_max_fps);
  if (cap <= 0) return;
  using clock = std::chrono::steady_clock;
  static clock::time_point next{};
  const auto period = std::chrono::nanoseconds(1000000000LL / cap);
  auto now = clock::now();
  if (next.time_since_epoch().count() == 0 || now - next > period * 4) next = now;  // first frame or a long stall
  if (now < next) {
    if (next - now > std::chrono::milliseconds(2)) std::this_thread::sleep_for(next - now - std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
  }
  next += period;
  static bool logged = false;
  if (!logged) { logged = true; REXLOG_INFO("gow1_max_fps: on - at most {} fps", cap); }
}

// Job 709: Gears 2's CPU wait fixes (jobs 225/231), found in Gears 1 by their shape.
//  - 0x82241A80 is the XDK's GPU-wait step: a delay loop of 4 x 8 db16cyc (`mr r31,r31`, empty when recompiled - the
//    render thread spins at 100%) from 0x82241A98 (its counter store), then the GPU-hang / read-pointer checks at 0x82241AD4. On: the delay
//    loop is skipped after a short pause or a sleep until our GPU side writes guest memory (as Gow2GpuWaitStep).
//  - 0x825BA700 is UE3's wait for one query's answer: GetData(q, out, 4, FLUSH) in a loop from 0x825BA71C. On: each
//    retry pauses (32 quick polls) and then sleeps until our GPU side writes guest memory (as Gow2QueryWaitStep).
REXCVAR_DEFINE_INT32(gow1_gpu_wait, 1, "Gears 1",
                     "The XDK's wait for the GPU: 1 sleeps until the GPU side writes (default, as Gears 2), 0 spins as the title");
// 13:13: default 0 - capped (vsync, the launcher) mode 2 slept ~1 ms per visibility check: 95-120 ms a frame in gameplay.
REXCVAR_DEFINE_INT32(gow1_query_wait, 0, "Gears 1",
                     "UE3's query wait: 0 (default) spins as the title; 1 sleeps between polls, 2 as 1 when capped (both slow - see 13:13)");
namespace {
struct Gow1WaitStreak { uint64_t last_qpc = 0; uint32_t count = 0; uint32_t write_seq = 0; };
void Gow1SleepStep(Gow1WaitStreak& w, uint32_t quick_steps) {
  static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (uint64_t(now.QuadPart) - w.last_qpc > freq / 5000) w.count = 0;
  if (++w.count <= quick_steps) {
    for (int i = 0; i < 32; ++i) _mm_pause();
  } else {
    XenosWaitGuestWrite(w.write_seq, 1);
  }
  w.write_seq = XenosGuestWriteSeq();
  QueryPerformanceCounter(&now);
  w.last_qpc = uint64_t(now.QuadPart);
}
}  // namespace
bool Gow1GpuWaitStep() {   // gow1_manifest.toml: 0x82241A98 (after r29 is loaded), jump to 0x82241AD4 on true
  thread_local XenosWaitClock fl_clock;
  XenosFrameWaitStep fl_step(kXenosWaitGameGpu, fl_clock);
  if (REXCVAR_GET(gow1_gpu_wait) != 1) return false;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("gow1_gpu_wait: on - the GPU wait sleeps instead of spinning");
  thread_local Gow1WaitStreak streak;
  Gow1SleepStep(streak, 64);
  return true;
}
void Gow1QueryWaitStep() {   // gow1_manifest.toml: 0x825BA71C, the retry loop's head
  thread_local XenosWaitClock fl_clock;
  XenosFrameWaitStep fl_step(kXenosWaitGameQuery, fl_clock);
  const int32_t mode = REXCVAR_GET(gow1_query_wait);
  if (mode != 1 && mode != 2) return;
  thread_local uint32_t vsync_calls = 0;
  thread_local bool vsync_on = true;
  if ((vsync_calls++ & 1023) == 0) vsync_on = rex::cvar::Query<bool>("gow2_gpu_vsync");
  if (mode == 2 && !vsync_on && REXCVAR_GET(gow1_max_fps) <= 0) return;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("gow1_query_wait: on - the query wait loop sleeps between polls");
  thread_local Gow1WaitStreak streak;
  Gow1SleepStep(streak, 32);
}
