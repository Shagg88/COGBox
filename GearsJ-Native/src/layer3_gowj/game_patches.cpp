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
#include <xenos/display_size.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <cctype>
#include <filesystem>
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

// ---- Frame rate and the engine tick ---------------------------------------------------------------------------------
// GetMaxTickRate 0x8255E210 (shape as Gears 3's 0x8253D9A0): f1 = 0.0 (no limit); with frame-rate smoothing on (flag read at 0x8255E210, bSmoothFrameRate=TRUE
// in Xbox360-GearEngine.ini) `lfs f1,0x4CC(r3)` at 0x8255E220 loads MaxSmoothedFrameRate (62 in that ini) and returns
// it. The hook runs after that load and replaces the value: a settings override, not a code change (Canary's "Unlock
// FPS" nops the load instead). Above 30 also needs --gow2_gpu_vsync=false (the game asks for every second vblank).
REXCVAR_DEFINE_INT32(gowj_max_fps, 0, "Judgment",
                     "The engine's frame-rate limit through its own settings ([Engine.Engine] MaxSmoothedFrameRate, 62; "
                     "-1 = no limit: MaxSmoothedFrameRate=1000, as GetMaxTickRate ignores bSmoothFrameRate on the 360): 0 = the game's, N = N fps. Above 30 also needs "
                     "--gow2_gpu_vsync=false");
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
REXCVAR_DEFINE_INT32(gowj_loading_fps, 60, "Judgment",
                     "GitHub #38: the engine's frame-rate limit while a loading screen runs (0 = no change). Above it a "
                     "loading screen can stay up for ever (the end of Monroe Commons at 162 fps)");

namespace {
using LoadClock = std::chrono::steady_clock;
std::atomic<int64_t> g_load_start{0};     // steady_clock ticks of the loading movie's open; 0 = no loading screen
std::atomic<int64_t> g_load_last_pkg{0};  // the last .xxx opened since then

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
  if (EndsWithNoCase(guest_path, ".bik") && ContainsNoCase(guest_path, "loadingmovie")) {
    if (g_load_start.exchange(now) == 0) REXLOG_INFO("gowj_loading_fps: loading screen - engine limit {} fps", REXCVAR_GET(gowj_loading_fps));
    g_load_last_pkg.store(now);
  } else if (g_load_start.load() != 0 && EndsWithNoCase(guest_path, ".xxx")) {
    g_load_last_pkg.store(now);
  }
}
bool LoadingClampActive() {
  const int64_t start = g_load_start.load();
  if (start == 0) return false;
  using namespace std::chrono;
  const auto now = LoadClock::now().time_since_epoch();
  const auto since_start = now - LoadClock::duration(start);
  const auto since_pkg = now - LoadClock::duration(g_load_last_pkg.load());
  if (since_start < seconds(10) || since_pkg < seconds(6)) return true;
  int64_t expected = start;
  if (g_load_start.compare_exchange_strong(expected, 0))
    REXLOG_INFO("gowj_loading_fps: loading screen over after {:.1f} s - the player's own limit again",
                duration<double>(since_start).count());
  return false;
}
}  // namespace

void Ue3ShaderPrebuildChainFileOpened(void (*listener)(std::string_view, const std::filesystem::path*));  // shader_prebuild.cpp

void GowjInstallLoadingClamp() {
  if (REXCVAR_GET(gowj_loading_fps) <= 0) return;
#if defined(REX_FILESYSTEM_FILE_OPEN_OBSERVER)
  // Job 666: through the shader reader's observer (it tells ours first), so it doesn't matter which installs first.
  Ue3ShaderPrebuildChainFileOpened(&OnGowjFileOpened);
  REXLOG_INFO("gowj_loading_fps: on - loading screens run at {} fps at most (GitHub #38)", REXCVAR_GET(gowj_loading_fps));
#else
  REXLOG_WARN("gowj_loading_fps: this SDK has no file-open observer (SDK patch 32); loading screens keep the player's limit");
#endif
}

void GowjMaxTickRate(PPCRegister& f1) {  // after `lfs f1,0x4CC(r3)` at 0x8255E220: f1 = the engine's limit (fps)
  TickTimeline();
  const int32_t cap = REXCVAR_GET(gowj_loading_fps);
  if (cap > 0 && LoadingClampActive() && (f1.f64 <= 0.0 || f1.f64 > double(cap))) f1.f64 = double(cap);
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
//   0x827F1DD4 `lwz r30,-10976(r9)` (SceneRenderTargets reads that global) -> 720, so the targets keep their size
//   0x82CBA684 `stw r4,136(r3)` (Scaleform's scale mode) -> replaced by `li r4,1` (the store is skipped)
//   the .rdata floats 0x82082C30 / 0x82083350 (post-process scale, 16:9 = 1.7778) -> 2.3273, written at load.
// The front buffer becomes 1280x550 (x the resolution scale) and is shown at its own shape (--gow2_present_frame_aspect).
REXCVAR_DEFINE_BOOL(gowj_ultrawide, false, "Judgment",
                    "21:9 ultrawide (Canary patch by boma): a 1280x550 picture (x the resolution scale) with a wider view; "
                    "fills a 21:9 window or monitor, letterboxed in others");

namespace {
template <typename T> void LogOnce(bool& done, const char* what, T before, T after) {
  if (done) return;
  done = true;
  REXLOG_INFO("gowj_ultrawide: {} {} -> {}", what, before, after);
}
}  // namespace

bool GowjUltrawideNoConstrain() { return REXCVAR_GET(gowj_ultrawide); }  // jump_address_on_true 0x82640588
void GowjUltrawideFov(PPCRegister& f0) {
  if (!REXCVAR_GET(gowj_ultrawide)) return;
  static bool l = false;
  LogOnce(l, "field-of-view factor", f0.f64, double(0.010975f));
  f0.f64 = double(0.010975f);
}
void GowjUltrawideSceneHeight(PPCRegister& r11) {
  if (!REXCVAR_GET(gowj_ultrawide)) return;
  static bool l = false;
  LogOnce(l, "scene buffer height", uint32_t(r11.u32), 550u);
  r11.u64 = 550;
}
bool GowjUltrawideNoVignette() { return REXCVAR_GET(gowj_ultrawide); }  // jump_address_on_true 0x82369E3C
void GowjUltrawideTargetHeight(PPCRegister& r30) {
  if (!REXCVAR_GET(gowj_ultrawide)) return;
  static bool l = false;
  LogOnce(l, "render target height", uint32_t(r30.u32), 720u);
  r30.u64 = 720;
}
bool GowjUltrawideUiScale(PPCRegister& r4) {  // jump_address_on_true 0x82CBA688: the store is skipped, r4 = 1
  if (!REXCVAR_GET(gowj_ultrawide)) return false;
  static bool l = false;
  LogOnce(l, "Scaleform scale mode", uint32_t(r4.u32), 1u);
  r4.u64 = 1;
  return true;
}

void GowjApplyUltrawideIfRequested(rex::Runtime* runtime) {
  if (!REXCVAR_GET(gowj_ultrawide) || !runtime || !runtime->memory()) return;
  for (uint32_t addr : {0x82082C30u, 0x82083350u}) {
    uint8_t* p = runtime->memory()->TranslateVirtual<uint8_t*>(addr);
    const float value = 2.3273f;
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
