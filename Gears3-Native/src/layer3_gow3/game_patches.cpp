// Layer 3 (Gears of War 3, disc XEX or Title Update 6): game patches as switches. Each one is a hook at a site checked
// in our own disassembly (recompiled code can't be patched in memory, so a hook changes a register or takes a branch
// instead), or a data write when the image is loaded. Community patches from xenia-canary/game-patches "4D5308AB -
// Gears of War 3" were the starting point for several; their authors (boma, Sowa_95) are credited in the option
// descriptions. Gears 2's versions of the same switches (Gears2-Native) are the model. Each hook's site is in its
// comment below and in gow3_manifest.toml (disc addresses below; Title Update 6's are in gow3_manifest_tu6.toml, its
// data addresses in gow3_addresses.h; notes/24).
// Not needed in Gears 3: Gears 2's "Black Shading Fix" (Gears 3 sets its tiling flag 0x83428B98 to 0 itself, at
// 0x82960B94; TU6: 0x83439358 at 0x82971B6C).
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <xenos/dev_tools.h>
#include <xenos/display_size.h>

#include "gow3_addresses.h"

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
#define GOW3_LOG_ONCE(...)                                           \
  do {                                                               \
    static std::atomic<bool> gow3_logged_{false};                    \
    if (!gow3_logged_.exchange(true)) REXLOG_INFO(__VA_ARGS__);      \
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

// 0x82969614 `lwz r5,836(r30)` reads the anisotropy setting; after it, r5 = 16.
REXCVAR_DEFINE_BOOL(gow3_patch_aniso16, false, "Gears3", "Force 16x anisotropic filtering (community patch, boma)");
// Diagnostics for black / flickering characters (Canary "Performance Patch, Option A", Sowa_95):
//  - light environment shadows: `rlwinm r9,r10,0,0,0` at 0x8236C990 tests the flags' top bit and the `beq` at
//    0x8236C998 skips the shadow work; after the rlwinm, r9 = 0.
//  - all dynamic shadows: `lwz r11,0x414(r21)` at 0x827CE810, the `beq` at 0x827CE818 skips when 0; after it, r11 = 0.
REXCVAR_DEFINE_BOOL(gow3_patch_no_light_env_shadows, false, "Gears3",
                    "Diagnostic: no character light-environment shadows (Canary Performance A, Sowa_95). If the black / "
                    "flickering characters go away with this, the cause is their self-shadow rendering");
REXCVAR_DEFINE_BOOL(gow3_patch_no_dynamic_shadows, false, "Gears3",
                    "Diagnostic: no dynamic (projected) shadows at all (Canary Performance A, Sowa_95)");
// The engine's FPS and frame-time display flags (disc 0x834290D8, 0x834290EC; gow3_addresses.h) set to 1 when the image
// is loaded.
REXCVAR_DEFINE_BOOL(gow3_patch_show_fps, false, "Gears3", "Turn on the engine's FPS and frame-time display (community patch, Sowa_95)");

void Gow3PatchAniso16(PPCRegister& r5) {
  if (!REXCVAR_GET(gow3_patch_aniso16)) return;
  GOW3_LOG_ONCE("gow3_patch_aniso16: anisotropy {} -> 16", r5.u32);
  r5.u64 = 16;
}
void Gow3NoLightEnvShadows(PPCRegister& r9) {  // after `rlwinm r9,r10,0,0,0` at 0x8236C990
  if (!REXCVAR_GET(gow3_patch_no_light_env_shadows)) return;
  GOW3_LOG_ONCE("gow3_patch_no_light_env_shadows: light environment shadows skipped");
  r9.u64 = 0;
}
void Gow3NoDynamicShadows(PPCRegister& r11) {  // after `lwz r11,0x414(r21)` at 0x827CE810
  if (!REXCVAR_GET(gow3_patch_no_dynamic_shadows)) return;
  GOW3_LOG_ONCE("gow3_patch_no_dynamic_shadows: dynamic shadows skipped");
  r11.u64 = 0;
}

// ---- Picture shape: 21:9, any wider shape, "auto" -------------------------------------------------------------------
// The game draws a 1280 x H scene with H = 1280 / aspect (even, 320-720), changed at the same sites as the Canary
// "21:9 Aspect Ratio Ultrawide Patch" (boma) for this XEX, with the view widened exactly (Hor+):
//   0x8261BEF0 `lfs f0,0x630(r27)` (the field-of-view factor) -> after it, f0 = the widened factor;
//   0x8261BE60 `beq cr6,+0x90` -> always taken (ConstrainAspectRatio off);
//   0x82960C78 `li r11,720` (the scene buffer height, stored to 0x8339242C) -> after it, r11 = H;
//   0x827C583C `lwz r30,0x242c(r9)` (that height read back for a size check) -> after it, r30 = 720 (as before);
//   0x82BC9D84 `stw r4,0x88(r3)` -> skipped (pillarboxed pre-rendered videos);
//   the .rdata float at 0x820CDE2C (16:9 = 1.77778) -> the new aspect, written at load.
// The front buffer becomes 1280 x H (x the resolution scale), shown at its own shape (--gow2_present_frame_aspect):
// it fills a window of that shape, letterboxed in others. Shapes narrower than 16:9 (16:10, 4:3) stay 16:9: a taller
// scene buffer may not fit EDRAM [unchecked].
REXCVAR_DEFINE_BOOL(gow3_ultrawide, false, "Gears3",
                    "21:9 ultrawide (Canary patch by boma): a 1280x550 picture (x the resolution scale) with a wider "
                    "view; fills a 21:9 window or monitor, letterboxed in others. Same as --gow3_aspect=21:9");
REXCVAR_DEFINE_STRING(gow3_aspect, "", "Gears3",
                      "Picture shape wider than 16:9: '' = the game's 16:9 (or 21:9 with --gow3_ultrawide), 'auto' = "
                      "the monitor's shape, or e.g. '21:9', '32:9', '3440:1440', '2.4'. Narrower shapes stay 16:9");
// The game multiplies pi/360 by each camera's horizontal field of view in degrees (`fmuls f2,f31,f0` at 0x8261BEF8):
// the half angle in radians. Exact Hor+ keeps the vertical view: half' = atan(tan(half) * aspect / (16/9)), and
// f0 = half' / fov gives exactly that after the game's multiply. Canary's fixed 0.0109750 widens every angle by 1.2577.
REXCVAR_DEFINE_BOOL(gow3_ultrawide_fov_exact, true, "Gears3",
                    "With --gow3_ultrawide: the wider view computed exactly from each camera's field of view (default), "
                    "or Canary's fixed factor 0.0109750 (false; 21:9 only)");

namespace {
constexpr uint32_t kCanaryUltrawideHeight = 550;  // Canary's 21:9 scene height

double ParseAspect(const std::string& v) {  // "21:9", "3440:1440" or "2.4"; 0 when unreadable
  char* end = nullptr;
  const size_t colon = v.find(':');
  if (colon == std::string::npos) {
    const double x = std::strtod(v.c_str(), &end);
    return end == v.c_str() ? 0.0 : x;
  }
  const double w = std::strtod(v.c_str(), &end), h = std::strtod(v.c_str() + colon + 1, nullptr);
  return h > 0 ? w / h : 0.0;
}
double MonitorAspect() {  // layer 0 knows the monitor the game window is on (the primary one before the window exists)
  uint32_t w = 0, h = 0;
  if (!xenos::display::MonitorSize(w, h) || !h) {
    REXLOG_INFO("gow3_aspect: monitor size unknown");
    return 0.0;
  }
  REXLOG_INFO("gow3_aspect: monitor is {}x{}", w, h);
  return double(w) / double(h);
}
// The scene buffer height for the chosen shape, or 0 = the game's own 16:9. Worked out once.
uint32_t WideHeight() {
  static const uint32_t height = [] {
    const std::string v = REXCVAR_GET(gow3_aspect);
    if (v.empty()) return REXCVAR_GET(gow3_ultrawide) ? kCanaryUltrawideHeight : 0u;
    const double aspect = v == "auto" ? MonitorAspect() : ParseAspect(v);
    if (!(aspect > 16.0 / 9.0 + 0.01)) {
      REXLOG_INFO("gow3_aspect: '{}' = {:.4f}, not wider than 16:9 - the game's 16:9 picture is kept", v, aspect);
      return 0u;
    }
    const uint32_t h = std::clamp(uint32_t(std::lround(1280.0 / aspect / 2.0)) * 2u, 320u, 720u);
    REXLOG_INFO("gow3_aspect: '{}' = {:.4f} -> scene 1280x{} ({:.4f})", v, aspect, h, 1280.0 / h);
    return h;
  }();
  return height;
}
bool Wide() { return WideHeight() != 0; }
}  // namespace

void Gow3UltrawideFov(PPCRegister& f0, PPCRegister& f31) {  // after `lfs f0,0x630(r27)` at 0x8261BEF0
  if (!Wide()) return;
  const double game = f0.f64, fov = f31.f64;
  double factor = double(0.0109750f);  // Canary's
  const bool exact = REXCVAR_GET(gow3_ultrawide_fov_exact) || WideHeight() != kCanaryUltrawideHeight;
  if (exact && fov > 1.0 && fov < 170.0 && game > 0.0) {
    const double widen = (1280.0 / double(WideHeight())) / (16.0 / 9.0);
    factor = std::atan(std::tan(fov * game) * widen) / fov;  // fov * game = the game's own half angle (radians)
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8)
    REXLOG_INFO("gow3_aspect: field of view {:.2f} deg: factor {} -> {} (half angle {:.2f} -> {:.2f} deg)", fov, game,
                factor, fov * game * 57.29578, fov * factor * 57.29578);
  f0.f64 = factor;
}
bool Gow3UltrawideNoConstrain() { return Wide(); }  // jump_address_on_true: the beq at 0x8261BE60 is taken
void Gow3UltrawideSceneHeight(PPCRegister& r11) {  // after `li r11,720` at 0x82960C78
  if (!Wide()) return;
  GOW3_LOG_ONCE("gow3_aspect: scene buffer height {} -> {}", r11.u32, WideHeight());
  r11.u64 = WideHeight();
}
void Gow3UltrawideSizeCheck(PPCRegister& r30) {  // after `lwz r30,0x242c(r9)` at 0x827C583C
  if (Wide()) r30.u64 = 720;
}
bool Gow3UltrawideVideo(PPCRegister& r4) {  // jump_address_on_true: past `stw r4,0x88(r3)` at 0x82BC9D84
  if (!Wide()) return false;
  GOW3_LOG_ONCE("gow3_aspect: 0x82BC9D84 store of {} skipped", r4.u32);
  r4.u64 = 1;  // as Canary's `li r4,1` in place of the store
  return true;
}

// ---- Frame rate and the engine tick ---------------------------------------------------------------------------------
// GetMaxTickRate 0x8253D9A0: f1 = 0.0 (no limit); with frame-rate smoothing on (flag 0x8343F7D4, bSmoothFrameRate=TRUE
// in Xbox360-GearEngine.ini) `lfs f1,0x4C8(r3)` at 0x8253D9B0 loads MaxSmoothedFrameRate (62 in that ini) and returns
// it. The hook runs after that load and replaces the value: a settings override, not a code change (Canary's "Unlock
// FPS" nops the load instead). Above 30 also needs --gow2_gpu_vsync=false (the game asks for every second vblank).
REXCVAR_DEFINE_INT32(gow3_max_fps, 0, "Gears3",
                     "The engine's frame-rate limit (UE3 MaxSmoothedFrameRate, 62 in the game's own ini): 0 = the game's, "
                     "N = N fps, -1 = none. Above 30 also needs --gow2_gpu_vsync=false");
// The engine tick's rhythm, for small regular stutters: the renderer's frame-time line gives only mean / p99 / max and
// its hitch report starts at 40 ms. GetMaxTickRate runs once per engine loop [G: UE3's FEngineLoop::Tick asks for it
// each frame], so the hook above times it. Every N ticks one line: median / p95 / max, the slow ticks (> 1.5x median
// and > median + 4 ms) and the spacing between them (median gap in ms and ticks, and gap spread / median gap: a stutter
// on a fixed rhythm gives a steady gap with a small spread). Read-only.
REXCVAR_DEFINE_INT32(gow3_tick_log, 600, "Gears3",
                     "Log the engine's tick rhythm every N ticks (median / p95 / max, slow ticks and their spacing). "
                     "0 = off");

namespace {
double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}
void TickTimeline() {  // the game thread is the only caller
#ifdef _WIN32
  const int32_t n = REXCVAR_GET(gow3_tick_log);
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
  REXLOG_INFO("gow3_tick_log: ticks {}-{}: median {:.2f} ms, p95 {:.2f}, max {:.1f} | slow (> {:.1f} ms): {} (mean {:.1f} ms), "
              "{}", ticks - iv.size(), ticks - 1, med, p95, mx, slow_at, slow.size(), slow_mean, rhythm);
  iv.clear();
#endif
}
}  // namespace

void Gow3MaxTickRate(PPCRegister& f1) {  // after `lfs f1,0x4C8(r3)` at 0x8253D9B0
  TickTimeline();
  const int32_t v = REXCVAR_GET(gow3_max_fps);
  if (!v) return;
  GOW3_LOG_ONCE("gow3_max_fps: engine frame-rate limit {} -> {}", f1.f64, v < 0 ? std::string("none") : std::to_string(v));
  f1.f64 = v < 0 ? 0.0 : double(v);
}

// ---- Engine settings through the game's own config (layer 2 --ue3_ini_edits) ----------------------------------------
// Added to --ue3_ini_edits before the bundle is edited (gow3_app.h), so layer 2 does the work: the edited
// Coalesced_*.bin, its table-of-contents size and MD5, and the start-up SHA-1 table. Ideas from DryreL's multipatch
// (Nexus gearsofwar3/mods/27), checked against our Xbox360-GearEngine.ini.
//  - Trilinear: every TEXTUREGROUP_* with MipFilter=point gets MipFilter=linear, the rest of its value unchanged (the
//    game's own values, its "ansio" typo included). Detail levels blend instead of showing a line where they step down.
//  - Fast mip fade: [Engine.Engine] MipFadeInSpeed0/1 (0.3 / 2.0 s per mip level) and MipFadeOutSpeed0/1 (0.1 / 1.0) set
//    to 0.01 s, so a newly streamed sharper mip shows at once. Not 0: the engine divides by the speed [G].
REXCVAR_DEFINE_BOOL(gow3_trilinear, true, "Gears3",
                    "Smooth blending between texture detail levels (trilinear; the game uses point). Idea from DryreL's "
                    "multipatch");
REXCVAR_DEFINE_BOOL(gow3_fast_mip_fade, true, "Gears3",
                    "Sharper textures show at once instead of fading in over 0.3-2 s. Idea from DryreL's multipatch");
REXCVAR_DECLARE(std::string, ue3_ini_edits);  // layer 2 config_install.cpp
constexpr const char* kGow3TrilinearEdits =
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
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_RenderTarget=(MinLODSize=1,MaxLODSize=4096,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_MobileFlattened=(MinLODSize=1,MaxLODSize=4096,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_ProcBuilding_Face=(MinLODSize=1,MaxLODSize=1024,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_ProcBuilding_LightMap=(MinLODSize=1,MaxLODSize=256,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_Terrain_Heightmap=(MinLODSize=1,MaxLODSize=4096,LODBias=0,MinMagFilter=aniso,MipFilter=linear);"
    "Xbox360-GearEngine.ini|SystemSettings|TEXTUREGROUP_Terrain_Weightmap=(MinLODSize=1,MaxLODSize=4096,LODBias=0,MinMagFilter=aniso,MipFilter=linear);";
constexpr const char* kGow3FastMipFadeEdits =
    "Xbox360-GearEngine.ini|Engine.Engine|MipFadeInSpeed0=0.01;Xbox360-GearEngine.ini|Engine.Engine|MipFadeOutSpeed0=0.01;"
    "Xbox360-GearEngine.ini|Engine.Engine|MipFadeInSpeed1=0.01;Xbox360-GearEngine.ini|Engine.Engine|MipFadeOutSpeed1=0.01;";
void Gow3AddBuiltInIniEdits() {
  std::string add;
  if (REXCVAR_GET(gow3_trilinear)) add += kGow3TrilinearEdits;
  if (REXCVAR_GET(gow3_fast_mip_fade)) add += kGow3FastMipFadeEdits;
  if (add.empty()) return;
  const std::string user = REXCVAR_GET(ue3_ini_edits);
  rex::cvar::SetFlagByName("ue3_ini_edits", add + user);  // the player's own edits come last, so they win
  REXLOG_INFO("gow3: built-in ini edits added (trilinear {}, fast mip fade {})", REXCVAR_GET(gow3_trilinear),
              REXCVAR_GET(gow3_fast_mip_fade));
}

// ---- Texture streaming pool and the memory gauge --------------------------------------------------------------------
// The pool setup sub_829677A8 reads [TextureStreaming] PoolSize (145 in Xbox360-GearEngine.ini), then -POOLSIZE= /
// -REDUCEPOOLSIZE= from the command line, into r26 (MB); `slwi r11,r26,20` at 0x829678BC makes the byte size, which
// 0x829678C4/C8 round up to 4 KB and pass to the allocator 0x8295C8F8. The hook sits after that slwi (0x829678C0 is not
// a branch target; 0x829678BC is). Gears 2's equivalent is --gow2_patch_texture_pool_mb=196 (Canary "Texture Streaming
// Fix" by boma).
REXCVAR_DEFINE_INT32(gow3_texture_pool_mb, 200, "Gears3",
                     "Texture streaming pool in MB (game: 145). Bigger = fewer blurry textures waiting to sharpen. "
                     "0 = the game's own size. Like Gears 2's 196 MB fix (Canary patch idea by boma)");
// The SDK gives the title 512 MB of "physical" memory (the 0xA0000000 / 0xC0000000 / 0xE0000000 windows all map it):
// everything the GPU reads (textures, the texture pool, vertex buffers) and whatever the game takes with
// XPhysicalAlloc. Its "virtual" allocations (0x00000000-0x7EFFFFFF) are separate host memory in the SDK and do NOT count
// against those 512 MB (on the console they did). The gauge logs both, plus the lowest free physical memory seen, so the
// pool can be sized from a measurement. It starts when the pool is set up and stops at shutdown.
REXCVAR_DEFINE_INT32(gow3_memory_log_seconds, 0,  // off for players (outside review: the gauge thread could
                     // read the kernel during shutdown); 10 is the old diagnostic value
                      "Gears3",
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
  REXLOG_INFO("gow3_memory: physical {} of {} MB used, {} MB free (lowest {} MB) | virtual 4K {} MB, 64K {} MB",
              phys_used, total_mb, free_mb, lowest_free_mb, used_mb(v4k), used_mb(v64k));
}

void StartMemoryGauge() {
  const int32_t secs = REXCVAR_GET(gow3_memory_log_seconds);
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

void Gow3StopMemoryGauge() { g_memory_gauge_stop.store(true); }

void Gow3TexturePoolSize(PPCRegister& r11, PPCRegister& r26) {  // after `slwi r11,r26,20` at 0x829678BC
  const int32_t mb = REXCVAR_GET(gow3_texture_pool_mb);
  const uint32_t game_mb = r26.u32;
  if (mb > 0 && mb <= 400) r11.u64 = uint64_t(uint32_t(mb) << 20);
  static std::atomic<bool> first{true};
  if (first.exchange(false)) {
    if (mb > 400) REXLOG_WARN("gow3_texture_pool_mb: {} is above 400 MB; the game's size is used", mb);
    REXLOG_INFO("gow3_texture_pool_mb: texture pool {} MB (game's value {} MB)", uint32_t(r11.u32 >> 20), game_mb);
    StartMemoryGauge();
  }
}

// ---- Waiting for the GPU without spinning ---------------------------------------------------------------------------
// - --gow3_patch_query_poll_yield: the render thread waits for a GPU query in a loop (0x82961994-0x829619A8, calling
//   0x82AFA708 until it reports done), like Gears 2's loop at 0x828075BC. Hooked at the loop head 0x82961994.
// - --gow3_patch_gpu_wait: sub_82AF98D8 is one step of the D3D library's wait for the GPU (six loops at 0x82AF4868,
//   0x82AF4EA4, 0x82AF5A74, 0x82AF725C, 0x82AFE1B0, 0x82AFEC10 call it until the GPU's read pointer passes a point). On
//   the console the step lowers the hardware thread's priority (cctpl 0x82AF98EC), idles 4 x 8 `or r31,r31,r31` and
//   raises it (cctpm 0x82AF991C); recompiled those are empty, so the render thread spins at 100%. Hook on the cctpl,
//   jump to the cctpm.
// Both: steps less than 200 us apart belong to the same wait; its first `quick_steps` only pause (short waits stay
// fast), later ones sleep until our GPU side writes guest memory (fence, read pointer) or 1 ms.
REXCVAR_DEFINE_INT32(gow3_patch_query_poll_yield, 3, "Gears3",
                     "The render thread's GPU query wait loop: 0 as the title (spins), 1 pause per poll, 2 yield per "
                     "poll, 3 after 32 quick polls sleep until our GPU side writes guest memory (1 ms max; default: "
                     "CPU 0.87 -> 0.80 cores, same fps)");
REXCVAR_DEFINE_INT32(gow3_patch_gpu_wait, 1, "Gears3",
                     "The D3D library's wait-for-the-GPU step (sub_82AF98D8): 0 as the title (its delay loop is empty "
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

void Gow3QueryPollYield() {
  thread_local XenosWaitClock fl_clock;  // the frame log times this wait in every mode (also the spinning ones)
  XenosFrameWaitStep fl_step(kXenosWaitGameQuery, fl_clock);
  switch (REXCVAR_GET(gow3_patch_query_poll_yield)) {
    case 1:
      Pause32();
      break;
    case 2:
#ifdef _WIN32
      SwitchToThread();
#endif
      break;
    case 3: {
      GOW3_LOG_ONCE("gow3_patch_query_poll_yield 3: the query wait loop sleeps between polls");
      thread_local WaitStreak streak;
      SleepStep(streak, 32);
      break;
    }
    default:
      break;
  }
}

bool Gow3GpuWaitStep() {  // jump_address_on_true: skip the empty delay loop to the cctpm
  thread_local XenosWaitClock fl_clock;  // the frame log times this wait, patched or not
  XenosFrameWaitStep fl_step(kXenosWaitGameGpu, fl_clock);
  if (REXCVAR_GET(gow3_patch_gpu_wait) != 1) return false;
  GOW3_LOG_ONCE("gow3_patch_gpu_wait: on - the GPU wait sleeps instead of spinning");
  thread_local WaitStreak streak;
  SleepStep(streak, 64);
  return true;
}

// ---- Data written when the image is loaded --------------------------------------------------------------------------
namespace {
// Writes `value` over the big-endian .rdata float at `addr` only if it holds `expected` (a wrong address for this game
// version is then logged and left alone instead of corrupting another constant).
void WriteGuestFloat(rex::Runtime* runtime, uint32_t addr, float expected, float value, const char* what) {
  uint8_t* p = runtime->memory()->TranslateVirtual<uint8_t*>(addr);
  uint32_t old_bits = 0, bits = 0;
  std::memcpy(&bits, &value, 4);
  for (int i = 0; i < 4; ++i) old_bits = (old_bits << 8) | p[i];
  float old_value;
  std::memcpy(&old_value, &old_bits, 4);
  if (std::fabs(old_value - expected) > 1e-5f) {
    REXLOG_WARN("{}: {:08X} holds {}, not {} ({}): left unchanged", what, addr, old_value, expected, gow3::addr::kVersion);
    return;
  }
#ifdef _WIN32
  DWORD old_protect = 0;
  const bool lifted = VirtualProtect(p, 4, PAGE_READWRITE, &old_protect) != 0;
#endif
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(bits >> (24 - 8 * i));
#ifdef _WIN32
  if (lifted) VirtualProtect(p, 4, old_protect, &old_protect);
#endif
  REXLOG_INFO("{}: {:08X} {} -> {}", what, addr, old_value, value);
}
}  // namespace

void Gow3ApplyDataPatchesIfRequested(rex::Runtime* runtime) {
  if (!runtime || !runtime->memory()) return;
  REXLOG_INFO("gow3: built for game version {}", gow3::addr::kVersion);
  if (REXCVAR_GET(gow3_patch_show_fps)) {
    for (uint32_t addr : {gow3::addr::kShowFpsFlag, gow3::addr::kShowFrameTimeFlag})
      *runtime->memory()->TranslateVirtual<uint8_t*>(addr) = 1;
    REXLOG_INFO("gow3_patch_show_fps: FPS and frame-time display flags set");
  }
  if (Wide()) {
    const float aspect = WideHeight() == kCanaryUltrawideHeight ? 2.3272f : float(1280.0 / double(WideHeight()));
    WriteGuestFloat(runtime, gow3::addr::kWidescreenAspect, gow3::addr::kWidescreenAspectValue, aspect,
                    "gow3_aspect: widescreen aspect");
    const bool set = rex::cvar::SetFlagByName("gow2_present_frame_aspect", "true");
    REXLOG_INFO("gow3_aspect: present at the front buffer's own shape {}", set);
  }
}
