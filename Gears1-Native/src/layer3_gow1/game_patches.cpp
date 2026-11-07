// Gears1-Native (Gears of War, 2006) layer 3: this game's own patches. Started empty (2026-10-30): every patch here must
// be measured on Gears 1 first; Judgment's addresses don't apply. The functions gow1_app.h calls are kept so the app
// matches the other three games' shape.
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <set>
#include <string>
#include <chrono>
#include <thread>
#include <filesystem>
#include <system_error>
#include <immintrin.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/ppc/func.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xmemory.h>
#include <xenos/dev_tools.h>
#include <xenos/frame_limiter.h>
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

REXCVAR_DEFINE_BOOL(cogbox_test_skip_movies, false, "COGBox",
                    "TEST ONLY (scripted test runs): no full-screen movies - the start-up film player passes "
                    "every film after the rating screen (EpicLogo, MGSLogo, Startup), plus [FullScreenMovie] bForceNoMovies "
                    "in case this engine reads it (unverified for Gears 1), so the logos and title film are passed as "
                    "if skipped and a run reaches gameplay without button presses. In-engine (real-time) cutscenes still "
                    "play. Not for players: loading screens go black.");
void Gow1AddBuiltInIniEdits() {
  const auto game_default = [](const char* name, const std::string& value) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
  };
  game_default("ue3_ini_bundle_dir", "WarGame\\Config\\Xenon\\Cooked");
  if (REXCVAR_GET(gow1_aniso16)) game_default("gow2_renderer_force_aniso", "16");
  if (REXCVAR_GET(gow1_trilinear)) game_default("gow2_renderer_force_trilinear", "true");
  std::string add;
  if (REXCVAR_GET(cogbox_test_skip_movies)) add += "Xe-WarEngine.ini|FullScreenMovie|bForceNoMovies=True;";
  if (const int32_t mb = REXCVAR_GET(gow1_texture_pool_mb); mb > 0 && mb <= 400)
    add += std::format("Xe-WarEngine.ini|TextureStreaming|PoolSize={};", mb);
  if (!add.empty()) rex::cvar::SetFlagByName("ue3_ini_edits", add + REXCVAR_GET(ue3_ini_edits));   // the player's last
  REXLOG_INFO("gow1: options - aniso16 {}, trilinear {}, pool {} MB, skip logos {}", REXCVAR_GET(gow1_aniso16),
              REXCVAR_GET(gow1_trilinear), REXCVAR_GET(gow1_texture_pool_mb), REXCVAR_GET(gow1_skip_logos));
}

// The start-up check of Coalesced.ini (gow1_manifest.toml): skipped only when the config edits gave the game an edited
// copy, so an unedited game is checked as on the console.
static bool Gow1EditedConfig() { return !REXCVAR_GET(ue3_ini_edits).empty(); }
// GitHub #179: motion blur and depth of field off (the launcher's Graphics options; Gears 1 has no [SystemSettings]).
// From Xenia Canary's game-patches for this TU5 image (hash 34849A59634915F6; the Coalesced-check addresses below are
// that file's too): "Disable Motion Blur" (illusion, boma) and "Upscaling Fix - Disable Gaussian Blur" (boma).
//  - Motion blur: Canary makes 0x82548E34 `lwz r11,4(r31)` read 0, so sub_82548E18 (the motion-blur proxy's render,
//    419 instructions) takes its own first early exit (`beq` to `li r3,0`: nothing drawn, returns 0). The function is
//    reached only through its vtable (no direct `bl` in the generated code), so here it is replaced whole, once at
//    start-up, by a function that returns 0 - the same exit, without even entering it. Nothing when the option is off.
//  - Depth of field: the gaussian filter sub_82998DE8 (f1 = blur radius) has exactly two callers, sub_82538EA0 and
//    sub_8255A840 (the depth-of-field / bloom post passes), each loading the radius just before the call
//    (0x82539568 `lfs f1,12(r26)`, 0x8255AF08 `lfs f1,12(r30)`); Canary reads it from offset 0 instead, i.e. ~0. Direct
//    `bl` calls don't go through the dispatcher, so these two are midasm hooks after the loads (gow1_manifest.toml)
//    that set the radius to 0 - one bool test per pass when off. The same filter softens the bloom: said in the
//    launcher's tooltip.
// Each is applied only when the guest code at those addresses holds the exact instructions the patches expect
// (another build is logged and left alone).
REXCVAR_DEFINE_BOOL(gow1_no_motion_blur, false, "Gears 1",
                    "No motion blur (Canary's TU5 patch by illusion and boma, as the render's own early exit)");
REXCVAR_DEFINE_BOOL(gow1_no_gaussian_blur, false, "Gears 1",
                    "No depth of field: the gaussian blur radius 0 (Canary's TU5 patch by boma; the bloom is softer too)");
namespace {
std::atomic<bool> g_no_gaussian{false};
uint32_t GuestWord(const uint8_t* m, uint32_t a) {
  const uint8_t* p = m + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void NoMotionBlurRender(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }   // sub_82548E18's early exit
}  // namespace
void Gow1InstallPostFxSwitches(rex::Runtime* runtime) {
  const uint8_t* m = runtime ? runtime->virtual_membase() : nullptr;
  auto* d = runtime ? runtime->function_dispatcher() : nullptr;
  if (!m || !d) return;
  if (REXCVAR_GET(gow1_no_motion_blur)) {
    if (GuestWord(m, 0x82548E34) != 0x817F0004u) {   // lwz r11,4(r31)
      REXLOG_WARN("gow1_no_motion_blur: the code at 0x82548E34 is not TU5's - left alone");
    } else if (!d->SetFunction(0x82548E18, &NoMotionBlurRender)) {
      REXLOG_WARN("gow1_no_motion_blur: the motion-blur render couldn't be replaced");
    } else {
      REXLOG_INFO("gow1_no_motion_blur: on");
    }
  }
  if (REXCVAR_GET(gow1_no_gaussian_blur)) {
    if (GuestWord(m, 0x82539568) != 0xC03A000Cu || GuestWord(m, 0x8255AF08) != 0xC03E000Cu) {   // lfs f1,12(r26 / r30)
      REXLOG_WARN("gow1_no_gaussian_blur: the code at 0x82539568 / 0x8255AF08 is not TU5's - left alone");
    } else {
      g_no_gaussian.store(true);
      REXLOG_INFO("gow1_no_gaussian_blur: on (depth of field off, softer bloom)");
    }
  }
}
void Gow1NoGaussianBlurA(PPCRegister& f1) {   // after `lfs f1,12(r26)` at 0x82539568
  if (g_no_gaussian.load(std::memory_order_relaxed)) f1.f64 = 0.0;
}
void Gow1NoGaussianBlurB(PPCRegister& f1) {   // after `lfs f1,12(r30)` at 0x8255AF08
  if (g_no_gaussian.load(std::memory_order_relaxed)) f1.f64 = 0.0;
}

void Gow1CoalescedResult(PPCRegister& r3) { if (Gow1EditedConfig()) r3.u64 = 0; }
bool Gow1CoalescedSkip() {
  if (!Gow1EditedConfig()) return false;
  static bool logged = false;
  if (!logged) { logged = true; REXLOG_INFO("gow1: config check skipped for the edited Coalesced.ini"); }
  return true;
}

void Gow1HideLogosIfRequested(rex::Runtime*) {}   // job 694: hiding the films crashed the player - see Gow1SkipLogoStep
void Gow1SkipLogoStep(PPCRegister& r11) {   // gow1_manifest.toml: the start-up film index, after each step
  if (REXCVAR_GET(cogbox_test_skip_movies) && r11.u32 >= 1 && r11.u32 <= 3) {  // test only: past the table's 4 films
    static bool logged_all = false;
    if (!logged_all) { logged_all = true; REXLOG_INFO("cogbox_test_skip_movies: start-up films after the rating screen passed"); }
    r11.u64 = 4;
    return;
  }
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

// GitHub #246: a nickname change hid the campaign checkpoint. Gears 1 names the file inside its checkpoint package
// after the gamertag: sub_821C7350 (TU5) builds "save:\" + XamUserGetName(user, buf + 6, 4), i.e. the first three
// letters ("Use" for the default "User"), so with a new --user_gamertag the game looks for another file in
// default_checkpoint.sav and says "No checkpoint". After that call (midasm hook at 0x821C73A0, gow1_manifest.toml),
// if the package folder already holds a file under another name and none under the new one, the name of the file
// that is there is used instead - old saves keep working under any nickname, nothing is moved or renamed.
// --gow1_keep_checkpoint_name=false = the game's own rule.
REXCVAR_DEFINE_BOOL(gow1_keep_checkpoint_name, true, "Gears 1",
                    "GitHub #246: keep finding the campaign checkpoint after a nickname change (the file name the "
                    "game derives from the gamertag's first letters)");
namespace {
uint8_t* g_save_membase = nullptr;
std::filesystem::path g_save_root;
std::atomic<bool> g_save_keep{false};
}  // namespace
void Gow1InstallCheckpointName(rex::Runtime* runtime) {
  if (!runtime || !REXCVAR_GET(gow1_keep_checkpoint_name)) return;
  uint8_t* m = runtime->virtual_membase();
  if (!m) return;
  // li r5,4 / addi r4,r31,6 / bl XamUserGetName's thunk (TU5)
  if (GuestWord(m, 0x821C7394) != 0x38A00004u || GuestWord(m, 0x821C739C) != 0x389F0006u ||
      GuestWord(m, 0x821C73A0) != 0x4882C4B9u) {
    REXLOG_WARN("gow1_keep_checkpoint_name: the code at 0x821C7394 is not TU5's - left alone");
    return;
  }
  g_save_membase = m;
  g_save_root = runtime->user_data_root();
  g_save_keep.store(true);
  REXLOG_INFO("gow1_keep_checkpoint_name: on");
}
void Gow1CheckpointFileName(PPCRegister& r31) {   // after the bl at 0x821C73A0; r31 = the "save:\" + name buffer
  if (!g_save_keep.load(std::memory_order_relaxed) || !g_save_membase) return;
  char* name = reinterpret_cast<char*>(g_save_membase + r31.u32 + 6);
  const std::string want(name, strnlen(name, 4));
  namespace fs = std::filesystem;
  std::error_code ec;
  std::string other;
  int others = 0;
  for (const auto& user : fs::directory_iterator(g_save_root, ec)) {   // <xuid>\4D5307D5\00000001\default_checkpoint.sav
    if (!user.is_directory(ec) || user.path().filename().native().size() != 16) continue;
    const fs::path pkg = user.path() / "4D5307D5" / "00000001" / "default_checkpoint.sav";
    if (!fs::is_directory(pkg, ec)) continue;
    for (const auto& f : fs::directory_iterator(pkg, ec)) {
      if (!f.is_regular_file(ec)) continue;
      const std::string n = rex::path_to_utf8(f.path().filename());
      if (n == want) return;  // the game's own name is there: nothing to do
      if (n.empty() || n.size() > 3) continue;
      other = n;
      ++others;
    }
  }
  if (others != 1) return;  // nothing saved yet (or more than one choice): the game's own rule
  std::memcpy(name, other.c_str(), other.size() + 1);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 2)
    REXLOG_INFO("gow1_keep_checkpoint_name: the checkpoint saved under an earlier nickname is used ({} letters)",
                other.size());
}


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
  static xenos::FrameLimiter limiter;  // shared precise limiter (high-resolution timer + spin; no catch-up bursts)
  limiter.Wait(double(cap));
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
