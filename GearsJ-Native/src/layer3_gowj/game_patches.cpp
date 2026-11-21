// Layer 3 (Gears of War: Judgment, disc 0.0.0.3 + Title Update 4): game patches as switches.
// Shagg's rule (10:11): no hack patches or settings when we can build it better. So engine settings go through the
// game's own config (layer 2 --ue3_ini_edits: MaxAnisotropy, PoolSize, MaxSmoothedFrameRate) instead of overwriting
// registers, and code hooks are kept only to put back what recompiling removed (the GPU-wait / query-poll sleeps) or
// to read timings. No memory pokes (Gears 3's show-FPS flag is not ported; layer 0's --gow2_fps_bar does that job). Copied from Gears 3's
// game_patches.cpp (Shagg's own project, D5) and moved to Judgment's addresses, each one read in disassembly next to
// Gears 3's site (notes/05: same instructions, same registers). Kept from Gears 3: 16x anisotropic, the frame-rate limit
// and tick log, trilinear / fast mip fade ini edits (Judgment's own TEXTUREGROUP values), the texture pool hook and
// memory gauge, the two GPU-wait sleeps. The ultrawide hooks (0.2.0) are at the end of the file. Left out: Gears 3's shadow diagnostics. Community patch authors (boma, Sowa_95, illusion) are credited in the option descriptions.
// Not needed: Gears 2's "Black Shading Fix" - Judgment stores 0 to its tiling flag itself at 0x8299257C, like Gears 3.
#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <xenos/dev_tools.h>
#include <xenos/frame_limiter.h>
#include <xenos/display_size.h>
#include <ue3/native_viewport.h>  // 0.6.6 true 1080p menus
#include <ue3/objects.h>
#include "mouse_look.h"   // 0.5.0: CogMouseLookNoteFov (aim / zoom sensitivity)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <cstdio>
#include <string_view>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include <immintrin.h>
#ifdef _WIN32
#include <windows.h>
#endif

// One log line the first time this place in the code runs (any thread).
#define GOWJ_LOG_ONCE(...)                                           \
  do {                                                               \
    static std::atomic<bool> gowj_logged_{false};                    \
    if (!gowj_logged_.exchange(true)) REXLOG_INFO(__VA_ARGS__);      \
  } while (0)

namespace {
#ifdef _WIN32
uint64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return uint64_t(t.QuadPart);
}
uint64_t QpcFrequency() {
  static const uint64_t f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return uint64_t(x.QuadPart); }();
  return f;
}
#endif
}  // namespace

// ---- Simple switches ------------------------------------------------------------------------------------------------

// 16x anisotropic filtering through the game's own setting: [SystemSettings] MaxAnisotropy=4 -> 16 (the engine reads
// it at 0x8299B564 `lwz r5,0x34C(r30)`, the site of Canary's register patch by boma, which we don't use).
REXCVAR_DEFINE_BOOL(gowj_aniso16, false, "Judgment", "16x anisotropic filtering (the game's own MaxAnisotropy setting, 4 -> 16)");
// Film grain off (gowj_manifest.toml: Xenia Canary's TU4 "Disable Film Grain" by boma, as the blended amount 0).
REXCVAR_DEFINE_BOOL(gowj_no_film_grain, false, "Judgment", "No film grain (Canary's TU4 patch by boma: the frame's film-grain amount 0)");
static std::atomic<bool> g_no_film_grain{false};
void GowjInstallFilmGrainSwitch(rex::Runtime* runtime) {
  if (!REXCVAR_GET(gowj_no_film_grain) || !runtime) return;
  const uint8_t* m = runtime->virtual_membase();
  if (!m) return;
  const uint8_t* p = m + 0x8243CF68u;
  const uint32_t w = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  if (w != 0xD01F013Cu) {   // stfs f0,316(r31)
    REXLOG_WARN("gowj_no_film_grain: the code at 0x8243CF68 is not TU4's ({:08X}) - left alone", w);
    return;
  }
  g_no_film_grain.store(true);
  REXLOG_INFO("gowj_no_film_grain: on");
}
void GowjNoFilmGrain(PPCRegister& f0) {   // before `stfs f0,316(r31)` at 0x8243CF68
  if (g_no_film_grain.load(std::memory_order_relaxed)) f0.f64 = 0.0;
}

// ---- Frame rate and the engine tick ---------------------------------------------------------------------------------
// GetMaxTickRate 0x8255E210 (shape as Gears 3's 0x8253D9A0): f1 = 0.0 (no limit); with frame-rate smoothing on (flag read at 0x8255E210, bSmoothFrameRate=TRUE
// in Xbox360-GearEngine.ini) `lfs f1,0x4CC(r3)` at 0x8255E220 loads MaxSmoothedFrameRate (62 in that ini) and returns
// it. The hook runs after that load and replaces the value: a settings override, not a code change (Canary's "Unlock
// FPS" nops the load instead). Above 30 also needs --gow2_gpu_vsync=false (the game asks for every second vblank).
REXCVAR_DEFINE_INT32(gowj_max_fps, 0, "Judgment",
                     "The engine's frame-rate limit through its own settings ([Engine.Engine] MaxSmoothedFrameRate, 62; "
                     "-1 = no limit: MaxSmoothedFrameRate=1000, as GetMaxTickRate ignores bSmoothFrameRate on the 360): 0 = the game's, N = N fps. Above 30 also needs "
                     "--gow2_gpu_vsync=false");
// Review 049: the in-game menu's live frame cap (an atomic from the UI thread), else the start-up cvar.
static int32_t LiveMaxFps() {
  int32_t v = 0;
  return XenosLiveMaxFps(&v) ? v : REXCVAR_GET(gowj_max_fps);
}
// The engine tick's rhythm, for small regular stutters: the renderer's frame-time line gives only mean / p99 / max and
// its hitch report starts at 40 ms. GetMaxTickRate runs once per engine loop [G: UE3's FEngineLoop::Tick asks for it
// each frame], so the hook above times it. Every N ticks one line: median / p95 / max, the slow ticks (> 1.5x median
// and > median + 4 ms) and the spacing between them (median gap in ms and ticks, and gap spread / median gap: a stutter
// on a fixed rhythm gives a steady gap with a small spread). Read-only.
REXCVAR_DEFINE_INT32(gowj_tick_log, 600, "Judgment",
                     "Log the engine's tick rhythm every N ticks (median / p95 / max, slow ticks and their spacing). "
                     "0 = off");

namespace {
double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}
void TickTimeline() {  // the game thread is the only caller
#ifdef _WIN32
  const int32_t n = REXCVAR_GET(gowj_tick_log);
  if (n <= 0) return;
  static uint64_t last = 0, ticks = 0;
  static std::vector<double> iv;  // tick lengths, ms
  const uint64_t t = QpcNow();
  if (last) iv.push_back(double(t - last) * 1000.0 / double(QpcFrequency()));
  last = t;
  ++ticks;
  if (int32_t(iv.size()) < n) return;
  std::vector<double> sorted = iv;
  std::sort(sorted.begin(), sorted.end());
  const double med = sorted[sorted.size() / 2], p95 = sorted[sorted.size() * 95 / 100], mx = sorted.back();
  const double slow_at = std::max(med * 1.5, med + 4.0);
  std::vector<size_t> slow;
  double slow_mean = 0;
  for (size_t i = 0; i < iv.size(); ++i)
    if (iv[i] > slow_at) slow.push_back(i), slow_mean += iv[i];
  if (!slow.empty()) slow_mean /= double(slow.size());
  std::string rhythm = "no slow ticks";
  if (slow.size() >= 3) {
    std::vector<double> gap_ms, gap_ticks;
    for (size_t k = 1; k < slow.size(); ++k) {
      double ms = 0;
      for (size_t i = slow[k - 1] + 1; i <= slow[k]; ++i) ms += iv[i];
      gap_ms.push_back(ms);
      gap_ticks.push_back(double(slow[k] - slow[k - 1]));
    }
    const double gm = Median(gap_ms), gt = Median(gap_ticks);
    std::vector<double> dev;
    for (double g : gap_ms) dev.push_back(std::fabs(g - gm));
    rhythm = std::format("every {:.0f} ms / {:.0f} ticks (spread {:.2f})", gm, gt, gm > 0 ? Median(dev) / gm : 0.0);
  } else if (!slow.empty()) {
    rhythm = std::format("{} slow tick(s)", slow.size());
  }
  REXLOG_INFO("gowj_tick_log: ticks {}-{}: median {:.2f} ms, p95 {:.2f}, max {:.1f} | slow (> {:.1f} ms): {} (mean {:.1f} ms), "
              "{}", ticks - iv.size(), ticks - 1, med, p95, mx, slow_at, slow.size(), slow_mean, rhythm);
  iv.clear();
#endif
}
}  // namespace

// ---- Loading screens at a safe frame rate (GitHub #38) -------------------------------------------------------------
// A player at a 162 fps limit (165 Hz, vsync) finished Monroe Commons and the loading screen that followed never ended:
// the engine kept ticking at 162 fps for minutes (report 20261028-171446; the session's earlier loading screens ended).
// Xenia Canary shows the same with its "Unlock FPS" patch, and at 60 the game gets through. So while a loading screen
// runs, the engine's limit is held at --gowj_loading_fps (60): from the moment the game opens its loading movie
// (GearGame\Movies\LoadingMovie*.bik) until 6 s after the last level package (.xxx) it opened, at least 10 s. Then
// the player's own limit comes back. Gameplay itself is unchanged; only the loading screen's own rate is lowered.
// GitHub #77 / #54: 30 by default (the launcher already sends 30; #54's 30 fps run loads cleanly). The window used to
// be stretched by every .xxx opened, and Judgment streams level packages during play, so the cap could stay on for
// good ("locked at 30", #77). Now: (1) a real loading flag, when a hook reports it (GowjSetLoadingState), decides
// alone - cap while loading, release on the first tick it is clear; (2) without one, streamed packages extend the
// window by at most --gowj_loading_stream_s past the last level package / loading movie; (3) --gowj_loading_max_s
// ends any loading cap, logged, whatever the signals say.
// GitHub #77 again (0.3.5: after a checkpoint the cap stayed on until the player paused and resumed): a level package
// (*_P.xxx) the game opens AGAIN while the cap is on is not a new level change - the game re-reads a persistent package
// it already has while it plays, and every such re-read used to re-arm the window (since_trigger < 6 s), so the cap
// only ended when play stopped (the pause menu) [G: the re-read is inferred from "only a pause releases it"; these
// lines show it]. Now only the loading movie or a level package not yet seen in this window re-arms it, and each
// trigger is logged (the first 12 per window, then a count), so the next report names what keeps a cap on.
//
// GitHub #318 (= #54 / #38 / #77; 0.6.3): the file-open window is not enough. Report i318 (2026-11-15): at the end of
// Monroe Commons the game read LoadingMovie_ESM.txt / LoadingMovie.txt (07:33:42, cap on), opened NO level package,
// the window ended after its 5 s minimum, and the loading screen then ran at 165 fps for 3 minutes without ending. Two
// earlier loading screens in that session (07:15, 07:18) look exactly the same in the log and ended fine - file opens
// cannot tell them apart. So the cap now follows the engine's own answer to "is the loading movie up?":
//   GowjPollLoadingMovie (below) calls, once per engine tick on the game thread (from our UEngine::GetMaxTickRate
//   override, cheats.cpp), the game's own predicate sub_82B8AF98 and reports the answer through GowjSetLoadingState.
// What sub_82B8AF98 is, read in the TU4 image (SHA-1 BD3FEE4A..., notes/03) and our generated code [V = read there]:
//   [V] it takes no arguments and returns, as 0 / 1:
//         GFullScreenMovie (the global at 0x835462BC) != null
//         && GFullScreenMovie->vtable[8](L"")            (0x82393A50, GameThreadIsMoviePlaying)
//         && _wcsicmp(GFullScreenMovie->vtable[9](), L"LoadingMovie") == 0   (0x82393B68 GameThreadGetLastMovieName,
//                                                         0x82D9DC48 _wcsicmp, L"LoadingMovie" at 0x8209FDEC)
//   [V] GFullScreenMovie is made by sub_829D8110 (436 bytes, constructor 0x823910C8, vtable 0x820A3360; the
//       constructor reads [FullScreenMovie] / Engine.StreamingMovies and starts the Bink async threads). vtable[4] =
//       0x82392EE0 is GameThreadPlayMovie (builds Movies\<name>.bik), vtable[9] returns the FString at +28 (the last
//       movie's name), vtable[8] with an empty name returns "the object at +8 answers 0 to its vtable[5](0)".
//   [G, standard UE3 FFullScreenMovieBink] +8 is the movie-finished event (made by the synch-event factory at
//       0x835443F0, manual reset) and vtable[5](0) is FEvent::Wait(0), so the predicate is "the movie whose name is
//       LoadingMovie is still playing". The loading movie loops until the game stops it.
//   [V] the game itself uses this predicate to drop the player's input while the loading movie is up: both callers are
//       the GearGame viewport client's input handlers (0x82B8B188 InputKey-like, 0x82B8B260; vtable at 0x8222B764 /
//       0x8222B76C, next to its Attract_Opening_Cinematic handling) and both return "handled" when it is true. So when
//       it is true the player has no control: it cannot be normal gameplay (the #77 worry). [G] When the loading
//       screen ends the game stops the movie, so the answer turns false and the cap is released on the next tick.
//   [G] In the #318 stuck case the screen shown is the loading movie (it was started at 07:33:42: the subtitle .txt is
//       read by the movie player when it plays a movie), so the cap now holds through it, as at 30 fps where the
//       report says the game gets through. To be confirmed on the PC: a run through the end of Monroe Commons at 165
//       fps, whose log should show "cap on - engine signal" at that loading screen and "cap off - engine signal: the
//       loading movie stopped" when it ends.
// The file-open heuristic stays as the fallback while there is no engine answer: before GFullScreenMovie exists (the
// first loading screen at start-up), with --cogbox_test_skip_movies (no movie ever plays), or if the TU4 code check in
// GowjPollLoadingMovie fails. Once the engine answers, file opens no longer start or re-arm the cap (they only note
// the time of a level change for the freeze watchdog). --gowj_loading_max_s stays the hard stop for both; with the
// engine answer a cap that hit it is not re-armed until the movie has stopped once.
REXCVAR_DEFINE_INT32(gowj_loading_fps, 30, "Judgment",
                     "GitHub #38: the engine's frame-rate limit while a loading screen runs (0 = no change). Above it a "
                     "loading screen can stay up for ever (the end of Monroe Commons at 162 fps)");
// GitHub #318: 90 -> 300 s. With the engine's own answer the cap ends when the loading movie stops, and while it plays
// the game takes no input (see above), so a long hard stop cannot hold gameplay at 30; a first visit that builds its
// shaders can keep a loading screen up for well over a minute on a slow PC, and releasing the cap there would bring
// the #318 hang back. The file-open fallback still ends seconds after its last trigger, as before.
REXCVAR_DEFINE_INT32(gowj_loading_max_s, 300, "Judgment",
                     "GitHub #77 / #318: a loading-screen cap ends after this many seconds whatever the loading signals "
                     "say (0 = never)");
REXCVAR_DEFINE_INT32(gowj_loading_stream_s, 8, "Judgment",
                     "GitHub #77: streamed level packages (.xxx that are not *_P.xxx) keep the cap on for at most this many "
                     "seconds after the last level package / loading movie");

REXCVAR_DEFINE_INT32(gowj_loading_tail_s, 3, "Judgment",
                     "0.4.6 (Shagg 5 Nov: the 30 fps after a loading screen / cutscene lasted too long): seconds the "
                     "loading cap stays on after the last level package / loading movie (was 6), and the cap's minimum "
                     "length is this + 2 (was 10)");
REXCVAR_DECLARE(bool, cogbox_test_skip_movies);  // defined below (no movie plays: the engine answer is not used)

namespace {
using LoadClock = std::chrono::steady_clock;
std::atomic<int64_t> g_load_start{0};     // steady_clock ticks of the loading movie's open; 0 = no loading screen
std::atomic<int64_t> g_load_last_pkg{0};  // the last .xxx opened since then
std::atomic<int64_t> g_load_last_trigger{0};  // the last loading movie / *_P.xxx (a real level change)
std::atomic<int64_t> g_load_watchdog{0};      // the last "still loading" line
// The engine's own loading state (GitHub #77 / #318): -1 = no answer yet (file-open heuristic), 0 = the loading movie
// is not playing, 1 = it is. Set every engine tick by GowjPollLoadingMovie through GowjSetLoadingState.
std::atomic<int> g_load_flag{-1};
std::atomic<bool> g_load_by_engine{false};     // the current cap is held by the engine answer (for the log lines)
std::atomic<bool> g_load_engine_timed_out{false};  // the hard stop ended an engine-held cap: wait for the movie to stop
// GitHub #77: the level packages that already started or re-armed the current window (cleared when it ends).
std::mutex g_load_names_mutex;
std::vector<std::string> g_load_names;
int g_load_reopens = 0;  // re-reads of a package in g_load_names, ignored as triggers (logged at the release)
std::string LowerName(std::string_view path) {
  const size_t slash = path.find_last_of("\\/");
  std::string n(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
  for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return n;
}

bool EndsWithNoCase(std::string_view s, std::string_view tail) {
  if (s.size() < tail.size()) return false;
  for (size_t i = 0; i < tail.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[s.size() - tail.size() + i])) != tail[i]) return false;
  return true;
}
bool ContainsNoCase(std::string_view s, std::string_view needle) {
  for (size_t i = 0; i + needle.size() <= s.size(); ++i) {
    size_t k = 0;
    while (k < needle.size() && std::tolower(static_cast<unsigned char>(s[i + k])) == needle[k]) ++k;
    if (k == needle.size()) return true;
  }
  return false;
}
void OnGowjFileOpened(std::string_view guest_path, const std::filesystem::path*) {
  const int64_t now = LoadClock::now().time_since_epoch().count();
  // GitHub #54 (report 20261030-092812): the loading movie is opened only once a session, so every loading screen after
  // the first ran at the player's limit (162 fps) and the one after Monroe Commons hung again. A level's persistent
  // package (*_P.xxx) is opened by every level change, so it starts the clamp too.
  // GitHub #54 / #77 (reports 20261031-214709, 20261031-105629): the game reads the loading movie's subtitle file at
  // the start of EVERY loading screen - LoadingMovie_<language>.txt, then LoadingMovie.txt - including the one after
  // Monroe Commons and a checkpoint reload, while the .bik is opened once a session. So the .txt starts the cap too.
  const bool movie = ContainsNoCase(guest_path, "loadingmovie") &&
                     (EndsWithNoCase(guest_path, ".bik") || EndsWithNoCase(guest_path, ".txt"));
  // GitHub #318: once the engine answers (GowjPollLoadingMovie), it alone starts and ends the cap. A file open only
  // notes the time of a level change (the freeze watchdog's GowjLastLevelLoadTicks) and, while a cap is on, the
  // package name for the release line.
  if (g_load_flag.load(std::memory_order_relaxed) != -1) {
    if (movie || EndsWithNoCase(guest_path, "_p.xxx")) g_load_last_trigger.store(now);
    if (!movie && g_load_start.load() != 0 && EndsWithNoCase(guest_path, "_p.xxx")) {
      std::lock_guard<std::mutex> lock(g_load_names_mutex);
      const std::string name = LowerName(guest_path);
      if (std::find(g_load_names.begin(), g_load_names.end(), name) == g_load_names.end()) g_load_names.push_back(name);
      else ++g_load_reopens;
    }
    return;
  }
  if (movie || EndsWithNoCase(guest_path, "_p.xxx")) {
    // GitHub #77: a level package already seen in this window is a re-read, not a level change - it doesn't re-arm.
    int seen_count = 0;
    bool reread = false;
    {
      std::lock_guard<std::mutex> lock(g_load_names_mutex);
      const std::string name = LowerName(guest_path);
      if (g_load_start.load() != 0 && !movie &&
          std::find(g_load_names.begin(), g_load_names.end(), name) != g_load_names.end()) {
        reread = true;
        ++g_load_reopens;
      } else {
        if (g_load_start.load() == 0) { g_load_names.clear(); g_load_reopens = 0; }
        if (!movie) g_load_names.push_back(name);
      }
      seen_count = static_cast<int>(g_load_names.size());
    }
    if (reread) {
      g_load_last_pkg.store(now);  // still counts as a streamed package (bounded by gowj_loading_stream_s)
      return;
    }
    if (g_load_start.exchange(now) == 0)
      REXLOG_INFO("gowj_loading_fps: cap on - file-open signal, no engine answer yet ({}), engine limit {} fps", guest_path,
                  REXCVAR_GET(gowj_loading_fps));
    else if (seen_count <= 12)
      REXLOG_INFO("gowj_loading_fps: cap kept on - {} ({})", movie ? "loading movie" : "new level package", guest_path);
    g_load_last_pkg.store(now);
    g_load_last_trigger.store(now);
  } else if (g_load_start.load() != 0 && EndsWithNoCase(guest_path, ".xxx")) {
    g_load_last_pkg.store(now);
  }
}
// GitHub #54 (a later report): at the end of Monroe Commons the only file sign of the loading screen can be a FAILED
// lookup of GearGame\Movies\LoadingMovie_<language>.txt (no movie, no *_P.xxx yet). A failed LoadingMovie*.txt / .bik
// lookup (SDK patch 52) is handled exactly as a successful one: the same trigger as above (it re-arms, is logged, and
// ends as any window does).
void OnGowjFileOpenFailed(std::string_view guest_path) {
  if (ContainsNoCase(guest_path, "loadingmovie")) OnGowjFileOpened(guest_path, nullptr);
}
void ReleaseLoadingClamp(int64_t start, const char* why, std::chrono::steady_clock::duration since_start) {
  int64_t expected = start;
  if (g_load_start.compare_exchange_strong(expected, 0)) {
    size_t levels = 0;
    int reopens = 0;
    {
      std::lock_guard<std::mutex> lock(g_load_names_mutex);
      levels = g_load_names.size();
      reopens = g_load_reopens;
      g_load_names.clear();
      g_load_reopens = 0;
    }
    g_load_by_engine.store(false);
    REXLOG_INFO("gowj_loading_fps: cap off - {} after {:.1f} s, the player's own limit again ({} level package(s), {} "
                "re-read(s) ignored)", why, std::chrono::duration<double>(since_start).count(), levels, reopens);
  }
}
bool LoadingClampActive() {
  using namespace std::chrono;
  const int flag = g_load_flag.load(std::memory_order_relaxed);
  const int64_t now_ticks = LoadClock::now().time_since_epoch().count();
  if (flag == 0) g_load_engine_timed_out.store(false);  // the movie stopped: the next one may start a cap again
  if (flag == 1 && !g_load_engine_timed_out.load()) {  // the loading movie is up: make sure the cap is on
    int64_t zero = 0;
    if (g_load_start.compare_exchange_strong(zero, now_ticks)) {
      g_load_last_trigger.store(now_ticks);
      g_load_last_pkg.store(now_ticks);
      {
        std::lock_guard<std::mutex> lock(g_load_names_mutex);
        g_load_names.clear();
        g_load_reopens = 0;
      }
      REXLOG_INFO("gowj_loading_fps: cap on - engine signal: the loading movie is playing (the game's own check, "
                  "0x82B8AF98), engine limit {} fps", REXCVAR_GET(gowj_loading_fps));
    } else if (!g_load_by_engine.load()) {  // a file-open window from before the engine answered: the engine takes over
      REXLOG_INFO("gowj_loading_fps: cap kept on - engine signal: the loading movie is playing (0x82B8AF98); it now "
                  "decides when the cap ends");
    }
    g_load_by_engine.store(true);
  }
  const int64_t start = g_load_start.load();
  if (start == 0) return false;
  const auto now = LoadClock::duration(now_ticks);
  const auto since_start = now - LoadClock::duration(start);
  const int32_t max_s = REXCVAR_GET(gowj_loading_max_s);
  if (max_s > 0 && since_start >= seconds(max_s)) {  // hard stop: a cap that never ends is #77
    if (flag == 1) g_load_engine_timed_out.store(true);  // not re-armed until the movie has stopped once
    ReleaseLoadingClamp(start, flag == 1 ? "timeout (--gowj_loading_max_s) with the loading movie still playing"
                                         : "timeout (--gowj_loading_max_s)", since_start);
    return false;
  }
  if (flag == 0) {  // the engine answers and its loading movie is not playing: the first such tick ends the cap
    ReleaseLoadingClamp(start, "engine signal: the loading movie stopped", since_start);
    return false;
  }
  bool on = flag == 1;
  if (!on) {  // no engine answer: the file-open window, streamed packages capped at gowj_loading_stream_s past the last trigger
    const auto since_pkg = now - LoadClock::duration(g_load_last_pkg.load());
    const auto since_trigger = now - LoadClock::duration(g_load_last_trigger.load());
    const int32_t tail = std::clamp(REXCVAR_GET(gowj_loading_tail_s), 1, 10);  // 0.4.6: was a fixed 6 s (minimum 10 s)
    const bool streaming = since_pkg < seconds(tail) && since_trigger < seconds(std::max(tail, REXCVAR_GET(gowj_loading_stream_s)));
    on = since_start < seconds(tail + 2) || since_trigger < seconds(tail) || streaming;
  }
  if (!on) {
    ReleaseLoadingClamp(start, "file-open signal: loading screen over", since_start);
    return false;
  }
  // every 10 s: say it is still on, so a report shows a load that does not end (#54) and why the cap stays
  int64_t last = g_load_watchdog.load(std::memory_order_relaxed);
  if (now - LoadClock::duration(std::max(last, start)) >= seconds(10) && g_load_watchdog.compare_exchange_strong(last, now_ticks))
    REXLOG_INFO("gowj_loading_fps: still loading after {:.0f} s ({}, last package {:.1f} s ago, last trigger {:.1f} s ago)",
                duration<double>(since_start).count(),
                flag == 1 ? "engine signal: the loading movie is playing" : "file-open signal, no engine answer",
                duration<double>(now - LoadClock::duration(g_load_last_pkg.load())).count(),
                duration<double>(now - LoadClock::duration(g_load_last_trigger.load())).count());
  return true;
}
uint32_t LoadBe32(const uint8_t* base, uint32_t guest) {
  const uint8_t* p = base + guest;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
}  // namespace

// GitHub #77 / #318: the engine's own loading state (see g_load_flag). Once called, the file-open heuristic no longer
// starts, re-arms or ends the cap.
void GowjSetLoadingState(bool loading) { g_load_flag.store(loading ? 1 : 0, std::memory_order_relaxed); }

// GitHub #318: once per engine tick, on the game thread, at the entry of UEngine::GetMaxTickRate (our override in
// cheats.cpp, before the original): ask the game whether its loading movie is playing - its own check sub_82B8AF98,
// described above - and pass the answer to GowjSetLoadingState. The call is made like layer 2's console exec: the
// whole context is saved and put back, so the original sees exactly the registers it was called with. At a function
// entry r1 is the caller's stack pointer with nothing live below it, so the callee's frame there is an ordinary call.
// Cost per tick: one string compare and a zero-timeout event wait, plus one FString copy while a movie plays.
void GowjPollLoadingMovie(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kIsLoadingMoviePlaying = 0x82B8AF98u;  // the game's own check (TU4)
  constexpr uint32_t kGFullScreenMovie = 0x835462BCu;       // the movie player global it reads first
  static PPCFunc* s_fn = nullptr;
  static int s_state = 0;  // 0 = not checked yet, 1 = in use, -1 = not used (file-open heuristic only)
  // Review r43: the game thread only (as layer 2's console exec): the first caller is the game's engine tick; a call
  // from any other thread is ignored.
  static std::thread::id s_thread;
  if (s_thread == std::thread::id()) s_thread = std::this_thread::get_id();
  if (std::this_thread::get_id() != s_thread) return;
  if (s_state == 0) {      // the game thread is the only caller
    s_state = -1;
    if (REXCVAR_GET(gowj_loading_fps) <= 0 || !base) return;
    if (REXCVAR_GET(cogbox_test_skip_movies)) {
      REXLOG_INFO("gowj_loading_fps: --cogbox_test_skip_movies: no movie plays, so the loading-screen cap keeps the "
                  "file-open signal");
      return;
    }
    // The TU4 code this relies on (read from guest memory, as GowjInstallFilmGrainSwitch does): the check's
    // `lis r31,0x8354` / `lwz r3,0x62BC(r31)` (GFullScreenMovie) and `addi r4,r11,-532` (L"LoadingMovie"), and the
    // string itself. Anything else: not this build's image - keep the file-open signal.
    static constexpr uint8_t kName[] = {0, 'L', 0, 'o', 0, 'a', 0, 'd', 0, 'i', 0, 'n', 0, 'g', 0, 'M', 0, 'o',
                                        0, 'v', 0, 'i', 0, 'e', 0, 0};
    const bool code_ok = LoadBe32(base, 0x82B8AFA4u) == 0x3FE08354u && LoadBe32(base, 0x82B8AFACu) == 0x807F62BCu &&
                         LoadBe32(base, 0x82B8B010u) == 0x388BFDECu &&
                         std::memcmp(base + 0x8209FDECu, kName, sizeof(kName)) == 0;
    s_fn = code_ok ? rex::runtime::ResolveIndirectFunction(kIsLoadingMoviePlaying) : nullptr;
    if (!s_fn) {
      REXLOG_WARN("gowj_loading_fps: the game's loading-movie check at 0x{:08X} is not TU4's ({:08X} {:08X} {:08X}) - the "
                  "loading-screen cap keeps the file-open signal",
                  kIsLoadingMoviePlaying, LoadBe32(base, 0x82B8AFA4u), LoadBe32(base, 0x82B8AFACu),
                  LoadBe32(base, 0x82B8B010u));
      return;
    }
    s_state = 1;
    REXLOG_INFO("gowj_loading_fps: engine signal on - the loading-screen cap follows the game's own \"loading movie is "
                "playing\" check (0x{:08X}, GitHub #318)", kIsLoadingMoviePlaying);
  }
  if (s_state != 1) return;
  if (LoadBe32(base, kGFullScreenMovie) == 0) return;  // no movie player yet (start-up): no answer, the flag stays
  const PPCContext saved = ctx;
  s_fn(ctx, base);
  const bool playing = ctx.r3.u32 != 0;  // UBOOL (0 / 1)
  ctx = saved;
  const int was = g_load_flag.load(std::memory_order_relaxed);
  if (was == -1)
    REXLOG_INFO("gowj_loading_fps: first engine answer - the loading movie {}; file opens no longer start "
                "the cap", playing ? "is playing" : "is not playing");
  // Review r43: "not playing" must hold for 250 ms before the cap is released, so a one-tick blip (a movie restart,
  // seamless travel) neither drops the cap for a few uncapped ticks nor logs an off / on pair. "Playing" applies at once.
  static LoadClock::time_point s_false_since{};
  if (playing) {
    s_false_since = {};
    GowjSetLoadingState(true);
  } else if (was != 1) {
    GowjSetLoadingState(false);
  } else {
    const auto now = LoadClock::now();
    if (s_false_since == LoadClock::time_point{}) s_false_since = now;
    if (now - s_false_since >= std::chrono::milliseconds(250)) {
      s_false_since = {};
      GowjSetLoadingState(false);
    }
  }
}
// GitHub #219: the freeze watchdog's thread asks when the last loading screen / level change began (steady_clock ticks,
// 0 = none seen, or --gowj_loading_fps=0 so no file is watched). A relaxed load, off the game's threads.
int64_t GowjLastLevelLoadTicks() { return g_load_last_trigger.load(std::memory_order_relaxed); }

void Ue3ShaderPrebuildChainFileOpened(void (*listener)(std::string_view, const std::filesystem::path*));  // shader_prebuild.cpp

void GowjInstallLoadingClamp() {
  if (REXCVAR_GET(gowj_loading_fps) <= 0) return;
#if defined(REX_FILESYSTEM_FILE_OPEN_OBSERVER)
  // Job 666: through the shader reader's observer (it tells ours first), so it doesn't matter which installs first.
  Ue3ShaderPrebuildChainFileOpened(&OnGowjFileOpened);
  REXLOG_INFO("gowj_loading_fps: on - loading screens run at {} fps at most (GitHub #38)", REXCVAR_GET(gowj_loading_fps));
#if defined(REX_FILESYSTEM_FILE_OPEN_FAILED_OBSERVER)
  rex::filesystem::SetFileOpenFailedObserver(&OnGowjFileOpenFailed);  // GitHub #54: failed LoadingMovie* lookups too
#else
  REXLOG_WARN("gowj_loading_fps: this SDK has no failed-open observer (SDK patch 52); a loading screen that only looks "
              "up a missing LoadingMovie*.txt keeps the player's limit");
#endif
#else
  REXLOG_WARN("gowj_loading_fps: this SDK has no file-open observer (SDK patch 32); until the engine answers (GitHub "
              "#318), loading screens keep the player's limit");
#endif
}

// 0.6.3 host frame-rate halving (as Gears 3): the guest return address of the GetMaxTickRate call in progress, set by
// cheats.cpp's override before the original runs (game thread). UE3 asks twice per frame on a host with remote players:
// the engine loop (returns to 0x829D8C44 / 0x82515478) and UWorld::ServerTickClients (vtable +308 in sub_825A1908, which
// only reads the value: 0 -> DeltaSeconds, else 1/x). That one gets the limit without the wait - a second wait per frame
// halved the player's limit - and without the per-frame work.
constexpr uint32_t kServerTickClientsTickRateRet = 0x825A1CB0u;
std::atomic<uint32_t> g_gowj_tick_caller{0};
bool GowjNoteTickCall(uint32_t caller) {  // read-only (frame_limiter.h): logs the call sites
  static xenos::TickCallSites sites;
  const bool main = caller != kServerTickClientsTickRateRet;
  sites.Note("gowj_max_fps", caller, main);
  return main;
}

// Exact cap: the engine's own wait sleeps whole milliseconds (165 gave 162 fps), so with a player's limit the game
// thread waits here on the host clock (shared precise limiter) and the engine is told "no limit" - it measured the time
// before this call, so its own wait would come on top.
static void GowjExactLimit(PPCRegister& f1) {
  // ServerTickClients (host with remote players): no second wait, and 0 as in 0.6.2 (review r62 M1)
  if (g_gowj_tick_caller.load(std::memory_order_relaxed) == kServerTickClientsTickRateRet) { f1.f64 = 0.0; return; }
  static xenos::FrameLimiter limiter;
  static std::atomic<bool> s_logged{false};
  if (!s_logged.exchange(true)) REXLOG_INFO("gowj_max_fps: exact host-side limiter on ({:.0f} fps)", f1.f64);
  limiter.Wait(f1.f64);
  f1.f64 = 0.0;
}

void GowjMaxTickRate(PPCRegister& f1) {  // after `lfs f1,0x4CC(r3)` at 0x8255E220: f1 = the engine's limit (fps)
  if (g_gowj_tick_caller.load(std::memory_order_relaxed) != kServerTickClientsTickRateRet) TickTimeline();  // once per frame
  const int32_t cap = REXCVAR_GET(gowj_loading_fps);
  // GitHub #77 (report 20261031-105629): every change of the engine's own limit is logged, so a report names it.
  static std::atomic<int32_t> s_last_engine{-1000};
  const int32_t engine = int32_t(f1.f64 + 0.5);
  if (const int32_t was = s_last_engine.exchange(engine); was != engine && was != -1000)
    REXLOG_INFO("gowj_max_fps: engine limit {} -> {} fps", was, engine);
  if (cap > 0 && LoadingClampActive()) {
    if (f1.f64 <= 0.0 || f1.f64 > double(cap)) f1.f64 = double(cap);
    if (LiveMaxFps() != 0) GowjExactLimit(f1);
    return;
  }
  // GitHub #77: outside a loading screen the game may lower its own limit (30) and keep it; the player's limit wins.
  const int32_t want = LiveMaxFps();
  if (want != 0 && f1.f64 > 0.0 && f1.f64 < (want < 0 ? 1000.0 : double(want)) - 0.5) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
      REXLOG_INFO("gowj_max_fps: the game lowered its limit to {:.0f} fps outside a loading screen - held at {} (GitHub #77)",
                  f1.f64, want < 0 ? 1000 : want);
    f1.f64 = want < 0 ? 0.0 : double(want);
  }
  // 0.4.9 live settings: the in-game menu can change gowj_max_fps while playing, but the engine's own limit is the
  // MaxSmoothedFrameRate the ini got at start-up (or none, after Unlimited) - so a player's cap is always the cvar's.
  if (want > 0) {
    f1.f64 = double(want);
    GowjExactLimit(f1);
  }
}

// ---- Engine settings through the game's own config (layer 2 --ue3_ini_edits) ----------------------------------------
// Added to --ue3_ini_edits before the bundle is edited (gowj_app.h), so layer 2 does the work: the edited
// Coalesced_*.bin, its table-of-contents size and MD5, and the start-up SHA-1 table. Ideas from DryreL's multipatch
// (Nexus gearsofwar3/mods/27), checked against our Xbox360-GearEngine.ini.
//  - Trilinear: every TEXTUREGROUP_* with MipFilter=point gets MipFilter=linear, the rest of its value unchanged (the
//    game's own values, its "ansio" typo included). Detail levels blend instead of showing a line where they step down.
//  - Fast mip fade: [Engine.Engine] MipFadeInSpeed0/1 (0.3 / 2.0 s per mip level) and MipFadeOutSpeed0/1 (0.1 / 1.0) set
//    to 0.01 s, so a newly streamed sharper mip shows at once. Not 0: the engine divides by the speed [G].
REXCVAR_DEFINE_BOOL(gowj_trilinear, true, "Judgment",
                    "Smooth blending between texture detail levels (trilinear; the game uses point). Idea from DryreL's "
                    "multipatch");
REXCVAR_DEFINE_BOOL(gowj_fast_mip_fade, true, "Judgment",
                    "Sharper textures show at once instead of fading in over 0.3-2 s. Idea from DryreL's multipatch");
REXCVAR_DEFINE_INT32(gowj_texture_pool_mb, 0, "Judgment",
                     "Texture streaming pool in MB through the game's own [TextureStreaming] PoolSize (136). 0 = the "
                     "game's own; set it only after Judgment's memory gauge has been read (lowest free minus a margin)");
REXCVAR_DECLARE(std::string, ue3_ini_edits);  // layer 2 config_install.cpp
// Judgment's own 20 TEXTUREGROUPs with MipFilter=point (Coalesced_INT.bin), only MipFilter changed.
constexpr const char* kGowjTrilinearEdits =
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_World=(MinLODSize=256,MaxLODSize=1024,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_WorldNormalMap=(MinLODSize=256,MaxLODSize=1024,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_WorldSpecular=(MinLODSize=128,MaxLODSize=512,LODBias=1,MinMagFilter=linear,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_Character=(MinLODSize=256,MaxLODSize=1024,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_CharacterNormalMap=(MinLODSize=256,MaxLODSize=1024,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_CharacterSpecular=(MinLODSize=128,MaxLODSize=512,LODBias=0,MinMagFilter=linear,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_Weapon=(MinLODSize=256,MaxLODSize=1024,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_WeaponNormalMap=(MinLODSize=256,MaxLODSize=1024,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_WeaponSpecular=(MinLODSize=128,MaxLODSize=512,LODBias=0,MinMagFilter=linear,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_Vehicle=(MinLODSize=512,MaxLODSize=1024,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_VehicleNormalMap=(MinLODSize=512,MaxLODSize=1024,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_VehicleSpecular=(MinLODSize=128,MaxLODSize=512,LODBias=1,MinMagFilter=linear,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_Cinematic=(MinLODSize=4096,MaxLODSize=4096,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_Effects=(MinLODSize=128,MaxLODSize=512,LODBias=1,MinMagFilter=linear,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_EffectsNotFiltered=(MinLODSize=128,MaxLODSize=512,LODBias=1,MinMagFilter=ansio,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_Skybox=(MinLODSize=512,MaxLODSize=2048,LODBias=0,MinMagFilter=linear,MipFilter=linear, MipGenSettings=TMGS_NoMipmaps);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_UI=(MinLODSize=512,MaxLODSize=1024,LODBias=0,MinMagFilter=linear,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_Lightmap=(MinLODSize=512,MaxLODSize=4096,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_Shadowmap=(MinLODSize=512,MaxLODSize=4096,LODBias=0,MinMagFilter=aniso,MipFilter=linear,NumStreamedMips=3);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_RenderTarget=(MinLODSize=1,MaxLODSize=4096,LODBias=0,MinMagFilter=aniso,MipFilter=linear);";
constexpr const char* kGowjFastMipFadeEdits =
    "Xbox360-GearEngine.ini|Engine.Engine|MipFadeInSpeed0=0.01;Xbox360-GearEngine.ini|Engine.Engine|MipFadeOutSpeed0=0.01;"
    "Xbox360-GearEngine.ini|Engine.Engine|MipFadeInSpeed1=0.01;Xbox360-GearEngine.ini|Engine.Engine|MipFadeOutSpeed1=0.01;";
// Level select (notes/08 item 1): the game's own developer menu ([GearGame.GearMenuHUD] bUseDevMenu, "Jack build"),
// switched off for release, with its 10 entries (test maps not on the disc, a LAN server at Epic) replaced by the
// campaign and Aftermath maps that do exist (all reached gameplay in jobs 013/014). Only the game's ini changes (D7);
// the menu, its list and the URL format (PocLevelName="<map>?game=sp", as its own AID_Arena_P entry) are the game's.
REXCVAR_DEFINE_BOOL(gowj_level_select, false, "Judgment",
                    "The game's hidden developer menu as a level select: every campaign and Aftermath chapter");
// Mute when unfocused (Shagg's launcher item 8; Gears 2's code, launcher patch 0130, D5): the app sets the SDK's
// audio_mute while another window is in front (GowjApp::OnWindowFocusChanged). The launcher looks for this name in the exe.
REXCVAR_DEFINE_BOOL(gowj_mute_unfocused, false, "Judgment",
                    "Mute the sound while another window is in front (sets audio_mute; a player's own --audio_mute=true "
                    "is left alone)");
// Skip intro logos (Shagg's launcher item 1): the game's own [FullScreenMovie] list without its Open_Logos entry; the
// loading movie and every cutscene stay (bForceNoMovies would drop those too). Layer 2's key-=value removes one entry.
REXCVAR_DEFINE_BOOL(gowj_skip_logos, false, "Judgment",
                    "Skip the intro logo movie at start-up (the loading movie and cutscenes still play)");
REXCVAR_DEFINE_BOOL(cogbox_test_skip_movies, false, "COGBox",
                    "TEST ONLY (scripted test runs): no full-screen movies - the engine's own [FullScreenMovie] "
                    "bForceNoMovies, so the logos, the loading movie and every pre-rendered (.bik) cutscene are passed as "
                    "if skipped and a run reaches gameplay without button presses. In-engine (real-time) cutscenes still "
                    "play. Not for players: loading screens go black. With it on, the loading movie no longer starts the "
                    "loading-screen frame cap (--gowj_loading_fps) - only a level package (*_P.xxx) does, so it may start "
                    "late or not at all: don't measure loading with it");
static std::string GowjLevelSelectEdits() {
  struct Entry { const char* map; const char* name; };
  static constexpr Entry kMaps[] = {
      {"SP_01_University_P", "Ch 1 - Sera University"},       {"SP_02_Mansion_P", "Ch 2 - Onyx Mansion"},
      {"SP_02_Mansion_3_P", "Ch 2 - Onyx Mansion (part 3)"},  {"SP_03_Island_P", "Ch 3 - Halvo Bay military"},
      {"SP_03_to_04_P", "Ch 3 to 4 - transition"},            {"SP_04_Rooftops_P", "Ch 4 - Rooftops"},
      {"SP_04_Rooftops2_P", "Ch 4 - Rooftops (part 2)"},      {"SP_05_Courthouse_P", "Ch 5 - Courthouse"},
      {"SP_05_CourthouseTrans_P", "Ch 5 - Courthouse (transition)"},
      {"SP_05_CourthouseBoss_P", "Ch 5 - Courthouse boss"},   {"SPE_E1_P", "Aftermath 1"},
      {"SPE_E2_P", "Aftermath 2"},                             {"SPE_E2oilrig_P", "Aftermath 2 - oil rig"},
      {"SPE_E3_P", "Aftermath 3"},                             {"SPE_E3_Outro_P", "Aftermath 3 - outro"},
      {"SPE_E4_P", "Aftermath 4"},                             {"SPE_E5_P", "Aftermath 5"},
  };
  std::string s = "Xbox360-GearGame.ini|GearGame.GearMenuHUD|bUseDevMenu=TRUE;"
                  "Xbox360-GearUI.ini|GearGame.UIScene_DevMenu|DevMenuOptions-=;";
  for (const Entry& e : kMaps)
    s += std::format("Xbox360-GearUI.ini|GearGame.UIScene_DevMenu|DevMenuOptions+=(PocLevelName=\"{}?game=sp\","
                     "PocName=\"{}\",PocDesc=\"{}\");", e.map, e.name, e.map);
  return s;
}

void GowjAddBuiltInIniEdits() {
  GowjInstallLoadingClamp();  // GitHub #38 (runs once, at start-up, before the game opens any file)
  std::string add;
  if (REXCVAR_GET(gowj_level_select)) add += GowjLevelSelectEdits();
  if (REXCVAR_GET(cogbox_test_skip_movies)) add += "Xbox360-GearEngine.ini|FullScreenMovie|bForceNoMovies=True;";
  if (REXCVAR_GET(gowj_skip_logos)) add += "Xbox360-GearEngine.ini|FullScreenMovie|StartupMovies-=Open_Logos;";
  if (REXCVAR_GET(gowj_aniso16)) add += "Xbox360-GearEngine.ini|SystemSettings|MaxAnisotropy=16;";
  if (const int32_t mb = REXCVAR_GET(gowj_texture_pool_mb); mb > 0 && mb <= 400)
    add += std::format("Xbox360-GearEngine.ini|TextureStreaming|PoolSize={};", mb);
  // -1: job 019 run E stayed at 62 fps with bSmoothFrameRate=FALSE (applied). GetMaxTickRate doesn't read that key: its
  // test at 0x8255E210 is `lwz r11,0x354(r9)` on a global (0x8355CC04), not the engine object, and when set it returns
  // MaxSmoothedFrameRate (`lfs f1,0x4CC(r3)` at 0x8255E220) - the load Canary's "Unlock FPS" patch removes. So "no
  // limit" is the same setting raised out of reach (1000), still through the game's own ini, no code change (D7).
  if (const int32_t fps = REXCVAR_GET(gowj_max_fps); fps < 0)
    add += "Xbox360-GearEngine.ini|Engine.Engine|bSmoothFrameRate=FALSE;Xbox360-GearEngine.ini|Engine.Engine|MaxSmoothedFrameRate=1000;";
  else if (fps > 0)
    add += std::format("Xbox360-GearEngine.ini|Engine.Engine|MaxSmoothedFrameRate={};", fps);
  if (REXCVAR_GET(gowj_trilinear)) add += kGowjTrilinearEdits;
  if (REXCVAR_GET(gowj_fast_mip_fade)) add += kGowjFastMipFadeEdits;
  if (add.empty()) return;
  const std::string user = REXCVAR_GET(ue3_ini_edits);
  rex::cvar::SetFlagByName("ue3_ini_edits", add + user);  // the player's own edits come last, so they win
  REXLOG_INFO("gowj: built-in ini edits added (trilinear {}, fast mip fade {}, aniso16 {}, pool {} MB, max fps {}, "
              "level select {}, skip logos {})",
              REXCVAR_GET(gowj_trilinear), REXCVAR_GET(gowj_fast_mip_fade), REXCVAR_GET(gowj_aniso16),
              REXCVAR_GET(gowj_texture_pool_mb), REXCVAR_GET(gowj_max_fps), REXCVAR_GET(gowj_level_select),
              REXCVAR_GET(gowj_skip_logos));
}

// ---- Texture streaming pool and the memory gauge --------------------------------------------------------------------
// The pool setup sub_82999618 reads [TextureStreaming] PoolSize (136 in Judgment's Xbox360-GearEngine.ini), then -POOLSIZE= /
// -REDUCEPOOLSIZE= from the command line, into r26 (MB); `slwi r11,r26,20` at 0x8299972C makes the byte size, which
// 0x82999734/38 round up to 4 KB and pass to the allocator 0x8298E1F8. The hook sits after that slwi (0x82999730 is not
// a branch target; 0x8299972C is). Gears 2's equivalent is --gow2_patch_texture_pool_mb=196 (Canary "Texture Streaming
// Fix" by boma).
// The SDK gives the title 512 MB of "physical" memory (the 0xA0000000 / 0xC0000000 / 0xE0000000 windows all map it):
// everything the GPU reads (textures, the texture pool, vertex buffers) and whatever the game takes with
// XPhysicalAlloc. Its "virtual" allocations (0x00000000-0x7EFFFFFF) are separate host memory in the SDK and do NOT count
// against those 512 MB (on the console they did). The gauge logs both, plus the lowest free physical memory seen, so the
// pool can be sized from a measurement. It starts when the pool is set up and stops at shutdown.
REXCVAR_DEFINE_INT32(gowj_memory_log_seconds, 0,  // off for players (outside review: the gauge thread could
                     // read the kernel during shutdown); 10 is the old diagnostic value
                      "Judgment",
                     "Log the game's memory use every N seconds (physical = the 512 MB the GPU sees; virtual = "
                     "the game's other memory). 0 = off");

namespace {
std::atomic<bool> g_memory_gauge_stop{false};

void LogMemoryOnce(uint32_t& lowest_free_mb) {
  auto* ks = REX_KERNEL_STATE();
  auto* mem = ks ? ks->memory() : nullptr;
  if (!mem) return;
  const rex::memory::BaseHeap* phys = mem->GetPhysicalHeap();
  const rex::memory::BaseHeap* v4k = mem->LookupHeap(0x10000000u);
  const rex::memory::BaseHeap* v64k = mem->LookupHeap(0x50000000u);
  if (!phys || !v4k || !v64k) return;
  auto used_mb = [](const rex::memory::BaseHeap* h) {
    return uint32_t((uint64_t(h->reserved_page_count()) * h->page_size()) >> 20);
  };
  const uint32_t total_mb = uint32_t((uint64_t(phys->total_page_count()) * phys->page_size()) >> 20);
  const uint32_t phys_used = used_mb(phys);
  const uint32_t free_mb = total_mb > phys_used ? total_mb - phys_used : 0;
  lowest_free_mb = std::min(lowest_free_mb, free_mb);
  REXLOG_INFO("gowj_memory: physical {} of {} MB used, {} MB free (lowest {} MB) | virtual 4K {} MB, 64K {} MB",
              phys_used, total_mb, free_mb, lowest_free_mb, used_mb(v4k), used_mb(v64k));
}

void StartMemoryGauge() {
  const int32_t secs = REXCVAR_GET(gowj_memory_log_seconds);
  if (secs <= 0) return;
  std::thread([secs] {
    uint32_t lowest_free_mb = UINT32_MAX;
    for (;;) {
      // Wake every 100 ms to notice shutdown, so the kernel state is never read after it is gone.
      for (int64_t i = 0; i < int64_t(secs) * 10; ++i) {  // 64-bit: a huge interval can't wrap
        if (g_memory_gauge_stop.load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (g_memory_gauge_stop.load()) return;
      LogMemoryOnce(lowest_free_mb);
    }
  }).detach();
}
}  // namespace

void GowjStopMemoryGauge() { g_memory_gauge_stop.store(true); }
void GowjStartMemoryGauge() { StartMemoryGauge(); }

// ---- Waiting for the GPU without spinning ---------------------------------------------------------------------------
// - --gowj_patch_query_poll_yield: the render thread waits for a GPU query in a loop (0x829933B4-0x829933C8, calling
//   0x82BE44F0 until it reports done), like Gears 2's loop at 0x828075BC. Hooked at the loop head 0x829933B4.
// - --gowj_patch_gpu_wait: sub_82BE36C0 is one step of the D3D library's wait for the GPU (its callers loop until the GPU's read pointer passes a point). On
//   the console the step lowers the hardware thread's priority (cctpl 0x82BE36D4), idles 4 x 8 `or r31,r31,r31` and
//   raises it (cctpm 0x82BE3704); recompiled those are empty, so the render thread spins at 100%. Hook on the cctpl,
//   jump to the cctpm.
// Both: steps less than 200 us apart belong to the same wait; its first `quick_steps` only pause (short waits stay
// fast), later ones sleep until our GPU side writes guest memory (fence, read pointer) or 1 ms.
REXCVAR_DEFINE_INT32(gowj_patch_query_poll_yield, 3, "Judgment",
                     "The render thread's GPU query wait loop: 0 as the title (spins), 1 pause per poll, 2 yield per "
                     "poll, 3 after 32 quick polls sleep until our GPU side writes guest memory (1 ms max; default: "
                     "CPU 0.87 -> 0.80 cores, same fps)");
REXCVAR_DEFINE_INT32(gowj_patch_gpu_wait, 1, "Judgment",
                     "The D3D library's wait-for-the-GPU step (sub_82BE36C0): 0 as the title (its delay loop is empty "
                     "here, so it spins), 1 sleep until our GPU side writes a fence / the read pointer after 64 quick "
                     "steps (1 ms safety timeout)");

namespace {
void Pause32() {
  for (int i = 0; i < 32; ++i) _mm_pause();
}
struct WaitStreak {
  uint64_t last_qpc = 0;
  uint32_t count = 0;
  uint32_t write_seq = 0;
};
void SleepStep(WaitStreak& w, uint32_t quick_steps) {
#ifdef _WIN32
  if (QpcNow() - w.last_qpc > QpcFrequency() / 5000) w.count = 0;
  if (++w.count <= quick_steps) Pause32();
  else XenosWaitGuestWrite(w.write_seq, 1);  // sampled before the preceding guest completion check
  w.write_seq = XenosGuestWriteSeq();
  w.last_qpc = QpcNow();
#else
  (void)w;
  (void)quick_steps;
  Pause32();
#endif
}
}  // namespace

void GowjQueryPollYield() {
  thread_local XenosWaitClock fl_clock;  // the frame log times this wait in every mode (also the spinning ones)
  XenosFrameWaitStep fl_step(kXenosWaitGameQuery, fl_clock);
  switch (REXCVAR_GET(gowj_patch_query_poll_yield)) {
    case 1:
      Pause32();
      break;
    case 2:
#ifdef _WIN32
      SwitchToThread();
#endif
      break;
    case 3: {
      GOWJ_LOG_ONCE("gowj_patch_query_poll_yield 3: the query wait loop sleeps between polls");
      thread_local WaitStreak streak;
      SleepStep(streak, 32);
      break;
    }
    default:
      break;
  }
#ifdef _WIN32
  // GitHub #54 (report e8a6e675): each sleep already ends after 1 ms, but the loop itself can wait on a query whose
  // answer never comes (a strict query the GPU never completes while the loading movie plays). Say so once per wait
  // after 2 s, with the loading state, so a report shows it. Answering such a query "visible" belongs in the
  // renderer's strict path (DeferredReports), not here.
  thread_local uint64_t wait_start = 0, wait_last = 0;
  thread_local bool wait_logged = false;
  const uint64_t q = QpcNow(), f = QpcFrequency();
  if (q - wait_last > f / 5000) { wait_start = q; wait_logged = false; }  // same 200 us rule as WaitStreak
  wait_last = q;
  if (!wait_logged && q - wait_start > 2 * f) {
    wait_logged = true;
    REXLOG_WARN("gowj_patch_query_poll_yield: one GPU query wait has lasted over 2 s (loading cap {})",
                g_load_start.load() != 0 ? "on" : "off");
  }
#endif
}

bool GowjGpuWaitStep() {  // jump_address_on_true: skip the empty delay loop to the cctpm
  thread_local XenosWaitClock fl_clock;  // the frame log times this wait, patched or not
  XenosFrameWaitStep fl_step(kXenosWaitGameGpu, fl_clock);
  if (REXCVAR_GET(gowj_patch_gpu_wait) != 1) return false;
  GOWJ_LOG_ONCE("gowj_patch_gpu_wait: on - the GPU wait sleeps instead of spinning");
  thread_local WaitStreak streak;
  SleepStep(streak, 64);
  return true;
}

// ---- 0.2.0: 21:9 ultrawide (Canary "21:9 Aspect Ratio Ultrawide Patch" for TU4, author boma) ------------------------
// Each site read in our generated code (job 538); the patch's instruction changes reproduced as hooks:
//   0x82640514 `beq cr6,0x82640588` -> always taken (ConstrainAspectRatio off)
//   0x82640588 `lfs f0,1628(r27)` (the field-of-view factor) -> f0 = 0.010975 (the patch's code cave)
//   0x82992664 `li r11,720` (the scene buffer height, stored to the screen-height global) -> 550
//   0x82369E30 `bne cr6,0x82369e3c` -> always taken (the vignette: its texture can't be rescaled)
//   0x827F1DD4 `lwz r30,-10976(r9)` (SceneRenderTargets reads that global) -> left at the picture's height since
//     0.6.6 (GitHub #396; the Canary patch and 0.6.5 forced 720 here, see GowjUltrawideTargetHeight);
//   0x827F33AC `addi r3,r1,264` (scene shared memory block 2 created next) -> its size kept at the 16:9 size (0.6.6,
//     GowjUltrawideSharedMemory; not in the Canary patch);
//   0x82CBA684 `stw r4,136(r3)` (Scaleform's scale mode) -> replaced by `li r4,1` (the store is skipped)
//   the .rdata floats 0x82082C30 / 0x82083350 (post-process scale, 16:9 = 1.7778) -> 2.3273, written at load.
// The front buffer becomes 1280x550 (x the resolution scale) and is shown at its own shape (--gow2_present_frame_aspect).
REXCVAR_DEFINE_BOOL(gowj_ultrawide, false, "Judgment",
                    "21:9 ultrawide (Canary patch by boma): a 1280x550 picture (x the resolution scale) with a wider view; "
                    "fills a 21:9 window or monitor, letterboxed in others");

// ---- 0.3.0: any ultrawide shape (GitHub #60: 21:9 and 32:9) --------------------------------------------------------
// --gowj_aspect picks the shape when gowj_ultrawide is on: "" = the Canary patch's own 21:9 (1280x550, unchanged), "auto"
// = the monitor's shape, or "21:9", "32:9", "3440:1440", "3.56". The scene height is 1280 / aspect (even, 320-720).
// The field-of-view factor is Canary's 21:9 value times how much more the shape widens than 21:9 does, at the half
// angle (34.05 deg) where Canary's 21:9 factor is exact (the hooks don't see the game's own field of view).
namespace uwshape {
inline double ParseAspect(const std::string& v) {
  double a = 0, b = 0;
  if (std::sscanf(v.c_str(), "%lf:%lf", &a, &b) == 2 && a > 0 && b > 0) return a / b;
  if (std::sscanf(v.c_str(), "%lf", &a) == 1 && a > 0) return a;
  return 0.0;
}
inline double MonitorAspect() {
  MONITORINFO mi{sizeof(mi)};
  HMONITOR m = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  if (!m || !GetMonitorInfoW(m, &mi)) return 0.0;
  const double w = double(mi.rcMonitor.right - mi.rcMonitor.left), h = double(mi.rcMonitor.bottom - mi.rcMonitor.top);
  return h > 0 ? w / h : 0.0;
}
// 550 = Canary's shape; another height when --gowj_aspect asks for one
inline uint32_t Height(const std::string& v, const char* tag) {
  if (v.empty()) return 550u;
  const double aspect = v == "auto" ? MonitorAspect() : ParseAspect(v);
  if (!(aspect > 16.0 / 9.0 + 0.01)) {
    REXLOG_INFO("{}: aspect '{}' = {:.4f} is not wider than 16:9 - Canary's 21:9 is used", tag, v, aspect);
    return 550u;
  }
  // GitHub #293 (0.6.2): down to 240 rows = 48:9 (three 16:9 screens side by side); 320 (4:1) before
  const uint32_t h = std::clamp(uint32_t(std::lround(1280.0 / aspect / 2.0)) * 2u, 240u, 720u);
  REXLOG_INFO("{}: aspect '{}' = {:.4f} -> scene 1280x{}", tag, v, aspect, h);
  return h;
}
inline double Widen(uint32_t h) { return (1280.0 / double(h)) / (16.0 / 9.0); }
// how much a shape of height h widens the view, relative to Canary's 550
inline double FovScale(uint32_t h) {
  constexpr double kRef = 34.046711 * 3.14159265358979 / 180.0;
  auto k = [&](double w) { return std::atan(std::tan(kRef) * w) / kRef; };
  return k(Widen(h)) / k(Widen(550));
}
}  // namespace uwshape
// GitHub #396 (as Gears 3's #330, fixed there in 0.6.3): with the scene render targets kept at 1280x720 under a 1280xH
// picture, the Stats & Awards pages (Achievements, Medals...) draw stacked cyan bands over the whole screen.
REXCVAR_DEFINE_BOOL(gowj_aspect_targets_720, false, "Judgment",
                    "With --gowj_ultrawide: keep the scene render targets at the 16:9 size 1280x720 as the Canary patch "
                    "does (before 0.6.6). Off = sized for the picture, as the game sizes them at 16:9 (GitHub #396: on, "
                    "the Stats & Awards pages draw stacked cyan bands)");
REXCVAR_DEFINE_STRING(gowj_aspect, "", "Judgment",
                      "With --gowj_ultrawide: the shape. Empty = 21:9 (1280x550, the Canary patch); auto = the monitor's; "
                      "or e.g. '21:9', '32:9', '3440:1440'");
namespace {
uint32_t GowjWideHeight() {
  // GitHub #396: the game rounds its scene render targets up to 8 rows (sub_827F3088: (h + 7) & ~7), so with the
  // targets following the picture every height is a multiple of 8 and both match exactly: Canary's 550 -> 552.
  static const uint32_t h = [] {
    const uint32_t raw = uwshape::Height(REXCVAR_GET(gowj_aspect), "gowj_ultrawide");
    if (REXCVAR_GET(gowj_aspect_targets_720)) return raw;
    const uint32_t h8 = std::clamp((raw + 4u) / 8u * 8u, 240u, 720u);
    if (h8 != raw && REXCVAR_GET(gowj_ultrawide)) REXLOG_INFO("gowj_ultrawide: scene height {} -> {} (a multiple of 8, GitHub #396)", raw, h8);
    return h8;
  }();
  return h;
}
template <typename T> void LogOnce(bool& done, const char* what, T before, T after) {
  if (done) return;
  done = true;
  REXLOG_INFO("gowj_ultrawide: {} {} -> {}", what, before, after);
}
}  // namespace

bool GowjUltrawideNoConstrain() { return REXCVAR_GET(gowj_ultrawide); }  // jump_address_on_true 0x82640588
void GowjUltrawideFov(PPCRegister& f0, PPCRegister& f31) {
  {  // 0.5.0: the view FOV for aim / zoom sensitivity (logged once per change, the first 12, to check it is degrees)
    CogMouseLookNoteFov(f31.f64);
    static double last = -1.0;
    static int lines = 0;
    if (f31.f64 != last && lines < 12) { last = f31.f64; ++lines; REXLOG_INFO("gowj_view_fov: {:.2f}", f31.f64); }
  }
  if (!REXCVAR_GET(gowj_ultrawide)) return;
  static bool l = false;
  const double f = double(0.010975f) * uwshape::FovScale(GowjWideHeight());
  LogOnce(l, "field-of-view factor", f0.f64, f);
  f0.f64 = f;
}
// ---- Experiment (Shagg, 20 Nov): true 1080p, as Gears 2's --gow2_native_lines ---------------------------------------
// sub_829925D0 (the function of the scene-height site): the height `li r11,720` at 0x82992664; the width = aspect x 720
// clamped to 904..1280 in r5, both paths meeting at 0x82992694 (`lis r11,-31925`), stored at 0x829926A8; `li r6,720` at
// 0x829926A4 and r5 go to a call (0x822FE3E8) as height and width; the stored width is read back at 0x829926B8
// (`lwz r11,0(r30)`) and compared with 1280 - any other width takes the narrow-screen path (two values set to 800), so
// that read gets 1280 when the width is ours. --gowj_native_lines=1080 gives 1920x1080. Needs
// --gow2_renderer_edram_extended=true. Not with --gowj_ultrawide. NOT RUN YET: written from the generated code.
REXCVAR_DEFINE_INT32(gowj_native_lines, 0, "Judgment",
                     "Experiment (true 1080p): the game's own screen height in lines, 0 = 720 as on the console. 1080 "
                     "makes the game draw 1920x1080 itself (width = lines x 16 / 9, a multiple of 8). Needs "
                     "--gow2_renderer_edram_extended=true. Not with --gowj_ultrawide")
    .range(0, 1440);
namespace {
uint32_t GowjNativeLines() {
  static const uint32_t lines = [] {
    const int v = REXCVAR_GET(gowj_native_lines);
    if (v <= 720 || REXCVAR_GET(gowj_ultrawide)) return 0u;
    return uint32_t(v + 7) / 8u * 8u;
  }();
  return lines;
}
// The game's own width times lines / 720 (a multiple of 8): the 16:10 and 4:3 video modes keep their shape.
uint32_t GowjNativeWidth(uint32_t game_width) { return (game_width * GowjNativeLines() / 720u + 7u) / 8u * 8u; }
}  // namespace
void GowjNativeSceneWidth(PPCRegister& r5) {  // before `lis r11,-31925` at 0x82992694: the clamped width
  if (!GowjNativeLines()) return;
  static bool l = false;
  const uint32_t width = GowjNativeWidth(r5.u32);
  LogOnce(l, "native lines: screen width", uint32_t(r5.u32), width);
  r5.u64 = width;
}
void GowjNativeCallHeight(PPCRegister& r6) {  // after `li r6,720` at 0x829926A4: the height passed with the width
  if (GowjNativeLines()) r6.u64 = GowjNativeLines();
}
void GowjNativeWidthCheck(PPCRegister& r11) {  // after `lwz r11,0(r30)` at 0x829926B8: compared with 1280
  if (GowjNativeLines() && r11.u32 == GowjNativeWidth(1280)) r11.u64 = 1280;  // only the full-width (16:9) case
}
// 0.6.6 (job 2189: at true 1080p the menus sat in the top-left 1280x720 of the 1920x1080 picture): the game's FViewport
// still reports the console's 1280x720 to every UI system (ue3/native_viewport.h: why, the checks, and the Gears 3 TU6
// code it was read from - Judgment's engine is Gears 3's: its Scaleform movies take their GViewport from the GFx
// engine's Viewport->GetSizeX / GetSizeY, as UGameViewportClient::GetViewportSize does). The object: GEngine (cheats.cpp's
// 0x83544E04) -> GameViewport -> Viewport, the two offsets read from the script classes (GameEngine.GameViewport,
// GameViewportClient.Viewport); if a name is not found, Gears 3's layout moved by Judgment's GamePlayers (+4: 1188 here,
// 1184 in Gears 3) - GameViewport +1200 [not read in Judgment's code], Viewport +68 - and Install's checks decide.
// Once per engine tick (cheats.cpp's GetMaxTickRate override) until on, every 30 ticks; then the object is followed.
bool GowjObjectsReady();  // adaptive_triggers.cpp
void GowjNativeUiViewportTick() {
  namespace nv = ue3::native_viewport;
  namespace obj = ue3::obj;
  if (!GowjNativeLines()) return;
  static uint32_t ticks = 0;
  static int fails = 0, gv_off = -1, vp_off = -1;
  if (++ticks % 30 || fails > 20) return;
  if (gv_off < 0 || vp_off < 0) {
    if (ticks % 300 || !GowjObjectsReady()) return;   // FindClass misses scan every object: every 5 s at most
    const uint32_t ge = obj::FindClass("GameEngine"), gvc = obj::FindClass("GameViewportClient");
    if (!ge || !gvc) { ++fails; return; }
    const int g = obj::PropertyOffset(ge, "GameViewport"), v = obj::PropertyOffset(gvc, "Viewport");
    gv_off = g >= 0 ? g : 1200;
    vp_off = v >= 0 ? v : 68;
    REXLOG_INFO("gowj_native_lines: GameEngine.GameViewport +{}{}, GameViewportClient.Viewport +{}{}", gv_off,
                g >= 0 ? "" : " (not found: Gears 3's layout)", vp_off, v >= 0 ? "" : " (not found: Gears 3's)");
  }
  const uint32_t engine = obj::Read32(0x83544E04u);
  const uint32_t client = engine ? obj::Read32(engine + uint32_t(gv_off)) : 0;
  const uint32_t viewport = client ? obj::Read32(client + uint32_t(vp_off)) : 0;
  if (!viewport) return;
  if (nv::g_installed.load(std::memory_order_relaxed)) {
    nv::NoteViewport(viewport);
    return;
  }
  const nv::Result r = nv::Install(viewport, GowjNativeLines());
  if (r.ok) {
    REXLOG_INFO("gowj_native_lines: UI scene viewport {}x{} -> {}x{} (the game's FViewport 0x{:08X}: GetSizeX / GetSizeY "
                "0x{:08X} / 0x{:08X}, vtable slots {} / {}) - menus and Scaleform movies lay out in the larger screen",
                r.w0, r.h0, r.w1, r.h1, viewport, r.fn_x, r.fn_y, r.slot, r.slot + 1);
  } else if (++fails > 20) {
    REXLOG_WARN("gowj_native_lines: the menus stay laid out in 1280x720 - {}", r.why);
  }
}
void GowjUltrawideSceneHeight(PPCRegister& r11) {
  if (GowjNativeLines()) {
    static bool n = false;
    LogOnce(n, "native lines: screen height", uint32_t(r11.u32), GowjNativeLines());
    r11.u64 = GowjNativeLines();
    return;
  }
  if (!REXCVAR_GET(gowj_ultrawide)) return;
  static bool l = false;
  LogOnce(l, "scene buffer height", uint32_t(r11.u32), GowjWideHeight());
  r11.u64 = GowjWideHeight();
}
bool GowjUltrawideNoVignette() { return REXCVAR_GET(gowj_ultrawide); }  // jump_address_on_true 0x82369E3C
// After `lwz r30,-10976(r9)` at 0x827F1DD4 in sub_827F1DB0 (UE3 FSceneRenderTargets::Allocate on Xbox, as Gears 3's
// sub_827D7AA8): r29 / r30 = the screen width / height globals, grown into BufferSizeX/Y (+276 / +280, rounded up to 8
// by sub_827F3088) when larger, then the targets are made again. At 16:9 the targets are exactly the screen's size and
// the Stats & Awards pages (a Scaleform movie) rely on it: they copy the screen into a scene target and draw it back
// as if the texture were the screen's size. The Canary patch kept r30 = 720 here, so under a 1280x536 picture each
// copy came back squeezed: the stacked cyan bands of GitHub #396 (3440x1440; Gears 3's #330 was the same). The game's
// own value (the picture's height) keeps targets and picture the same size, as at 16:9.
void GowjUltrawideTargetHeight(PPCRegister& r30) {
  if (!REXCVAR_GET(gowj_ultrawide)) return;
  if (REXCVAR_GET(gowj_aspect_targets_720)) {
    static bool l = false;
    LogOnce(l, "render target height (--gowj_aspect_targets_720)", uint32_t(r30.u32), 720u);
    r30.u64 = 720;
    return;
  }
  static std::atomic<uint32_t> last{0};
  if (last.exchange(r30.u32) != r30.u32) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 8)
      REXLOG_INFO("gowj_ultrawide: scene render targets sized from screen height {} (picture 1280x{}; 720 before 0.6.6, "
                  "GitHub #396){}", r30.u32, GowjWideHeight(), r30.u32 == GowjWideHeight() ? "" : " - NOT the picture height");
  }
}
// After `addi r3,r1,264` at 0x827F33AC, just before the `bl` to sub_82998D30 (RHICreateSharedMemory) for the scene's
// shared memory block 2 (GSceneRenderTargets+388) in sub_827F3100 (InitDynamicRHI): r5 = its size, max(BufferSize
// format 2, BufferSize format 19). The game also creates a format-12 depth texture of a fixed size in that block (the
// call before 0x827F33AC's block is used again near the function's end: `lwz r7,0(r14)`, `li r6,12`), which no longer
// fits when the targets follow a much smaller picture - Gears 3 trapped there (its #330, job 1907). Keep the 16:9
// capacity (1280 x 736 x 4, the size 0.6.5 allocated) so the targets can follow the picture's height.
void GowjUltrawideSharedMemory(PPCRegister& r5) {
  if (!REXCVAR_GET(gowj_ultrawide) || REXCVAR_GET(gowj_aspect_targets_720)) return;
  constexpr uint32_t kStock720 = 1280u * 736u * 4u;  // 0x398000
  static bool l = false;
  if (r5.u32 >= kStock720) {
    if (!l) { l = true; REXLOG_INFO("gowj_ultrawide: scene shared memory block 2 is 0x{:X} (not below the 16:9 size 0x{:X})", r5.u32, kStock720); }
    return;
  }
  if (!l) { l = true; REXLOG_INFO("gowj_ultrawide: scene shared memory block 2 0x{:X} -> 0x{:X} (GitHub #396)", r5.u32, kStock720); }
  r5.u64 = kStock720;
}
// 0.6.7: same as Gears 3 (gow3_native_ui_show_all): at --gowj_native_lines the game's own Scaleform scale mode draws
// the 1280x720 menus at 720p pixel size (job 2200 A-1080p-Judgment: pause text 2/3 size). Skipping the store keeps
// show-all, as under Ultrawide: the console layout scaled by lines / 720.
REXCVAR_DEFINE_BOOL(gowj_native_ui_show_all, true, "Judgment",
                    "With --gowj_native_lines: Scaleform menus keep the show-all scale mode, so the console's 1280x720 "
                    "layout is scaled to the larger screen (false = the 0.6.6 behaviour: drawn at 720p size)");
bool GowjUltrawideUiScale(PPCRegister& r4) {  // jump_address_on_true 0x82CBA688: the store is skipped, r4 = 1
  if (!REXCVAR_GET(gowj_ultrawide)) {
    if (!GowjNativeLines() || !REXCVAR_GET(gowj_native_ui_show_all)) return false;
    static bool n = false;
    LogOnce(n, "native lines: Scaleform scale mode (menus keep show-all)", uint32_t(r4.u32), 1u);
    r4.u64 = 1;
    return true;
  }
  static bool l = false;
  LogOnce(l, "Scaleform scale mode", uint32_t(r4.u32), 1u);
  r4.u64 = 1;
  return true;
}

void GowjApplyUltrawideIfRequested(rex::Runtime* runtime) {
  if (!REXCVAR_GET(gowj_ultrawide) || !runtime || !runtime->memory()) return;
  for (uint32_t addr : {0x82082C30u, 0x82083350u}) {
    uint8_t* p = runtime->memory()->TranslateVirtual<uint8_t*>(addr);
    const float value = GowjWideHeight() == 550 ? 2.3273f : float(1280.0 / GowjWideHeight());
    uint32_t bits = 0, old_bits = 0;
    std::memcpy(&bits, &value, 4);
    for (int i = 0; i < 4; ++i) old_bits = (old_bits << 8) | p[i];
    float old_value;
    std::memcpy(&old_value, &old_bits, 4);
    if (std::fabs(old_value - 16.0f / 9.0f) > 0.001f) {   // not the TU4 image these addresses are from: leave it alone
      REXLOG_WARN("gowj_ultrawide: aspect float {:08X} is {}, not 16:9 - not changed", addr, old_value);
      continue;
    }
    DWORD old_protect = 0;
    if (!VirtualProtect(p, 4, PAGE_READWRITE, &old_protect)) {
      REXLOG_WARN("gowj_ultrawide: aspect float {:08X} could not be unlocked - not changed", addr);
      continue;
    }
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(bits >> (24 - 8 * i));
    VirtualProtect(p, 4, old_protect, &old_protect);
    REXLOG_INFO("gowj_ultrawide: aspect float {:08X} {} -> {}", addr, old_value, value);
  }
  const bool a = rex::cvar::SetFlagByName("gow2_present_frame_aspect", "true");
  REXLOG_INFO("gowj_ultrawide: present at the front buffer's own shape {}", a);
}
