// Layer 3 (Gears of War: Judgment, disc 0.0.0.3 + Title Update 4): game patches as switches.
// Shagg's rule (10:11): no hack patches or settings when we can build it better. So engine settings go through the
// game's own config (layer 2 --ue3_ini_edits: MaxAnisotropy, PoolSize, MaxSmoothedFrameRate) instead of overwriting
// registers, and code hooks are kept only to put back what recompiling removed (the GPU-wait / query-poll sleeps) or
// to read timings. No memory pokes (Gears 3's show-FPS flag is not ported; layer 0's --gow2_fps_bar does that job). Copied from Gears 3's
// game_patches.cpp (Shagg's own project, D5) and moved to Judgment's addresses, each one read in disassembly next to
// Gears 3's site (notes/05: same instructions, same registers). Kept from Gears 3: 16x anisotropic, the frame-rate limit
// and tick log, trilinear / fast mip fade ini edits (Judgment's own TEXTUREGROUP values), the texture pool hook and
// memory gauge, the two GPU-wait sleeps. Left out until Judgment's sites are found: the ultrawide hooks and Gears 3's
// shadow diagnostics. Community patch authors (boma, Sowa_95, illusion) are credited in the option descriptions.
// Not needed: Gears 2's "Black Shading Fix" - Judgment stores 0 to its tiling flag itself at 0x8299257C, like Gears 3.
#include <rex/cvar.h>
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

void GowjMaxTickRate(PPCRegister& f1) {  // after `lfs f1,0x4CC(r3)` at 0x8255E220: read-only (tick timing)
  (void)f1;
  TickTimeline();
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
};
void SleepStep(WaitStreak& w, uint32_t quick_steps) {
#ifdef _WIN32
  if (QpcNow() - w.last_qpc > QpcFrequency() / 5000) w.count = 0;
  if (++w.count <= quick_steps) Pause32();
  else XenosWaitGuestWrite(XenosGuestWriteSeq(), 1);
  w.last_qpc = QpcNow();
#else
  (void)w;
  (void)quick_steps;
  Pause32();
#endif
}
}  // namespace

void GowjQueryPollYield() {
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
  if (REXCVAR_GET(gowj_patch_gpu_wait) != 1) return false;
  GOWJ_LOG_ONCE("gowj_patch_gpu_wait: on - the GPU wait sleeps instead of spinning");
  thread_local WaitStreak streak;
  SleepStep(streak, 64);
  return true;
}
