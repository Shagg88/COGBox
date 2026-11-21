// Gears1-Native (Gears of War, 2006) layer 3: this game's own patches. Started empty (2026-10-30): every patch here must
// be measured on Gears 1 first; Judgment's addresses don't apply. The functions gow1_app.h calls are kept so the app
// matches the other three games' shape.
#include "mouse_look.h"   // 0.5.0: CogMouseLookNoteFov
#include <atomic>
#include <cmath>   // 0.6.7 true 1080p menus: std::round, std::fabs
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
#include <ue3/native_viewport.h>  // 0.6.6 true 1080p menus
#include <ue3/objects.h>
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
// ---- Experiment (Shagg, 20 Nov): true 1080p, as Gears 2's --gow2_native_lines ---------------------------------------
// sub_824BC3B0 (TU5; found by job 2177 as the only function loading both 720 and 1280): it asks the console for the
// video mode and makes the screen height 720 (`li r11,720` at 0x824BC444, stored to the height global) and the width
// aspect x 720 clamped to 900..1280 in r31, every path meeting at 0x824BC47C before `stw r31,0(r29)` (the width
// global) at 0x824BC480; a second `li r11,720` at 0x824BC490 goes with that width into the block at 112(r1) / 116(r1)
// that the next calls take (the device's size). --gow1_native_lines=1080 gives all three: 1920x1080. Needs
// --gow2_renderer_edram_extended=true. Gears 1 draws its 720p frame as two tiles with 4x MSAA; how its tile maths takes
// a larger frame is NOT known. NOT RUN YET: written from the generated code, to be proven by a gameplay run.
REXCVAR_DEFINE_INT32(gow1_native_lines, 0, "Gears 1",
                     "Experiment (true 1080p): the game's own screen height in lines, 0 = 720 as on the console. 1080 "
                     "makes the game draw 1920x1080 itself (width = lines x 16 / 9, a multiple of 8). Needs "
                     "--gow2_renderer_edram_extended=true")
    .range(0, 1440);
namespace {
uint32_t Gow1NativeLines() {
  static const uint32_t lines = [] {
    const int v = REXCVAR_GET(gow1_native_lines);
    return v <= 720 ? 0u : uint32_t(v + 7) / 8u * 8u;
  }();
  return lines;
}
// The game's own width times lines / 720 (a multiple of 8): the 16:10 and 4:3 video modes keep their shape.
uint32_t Gow1NativeWidth(uint32_t game_width) { return (game_width * Gow1NativeLines() / 720u + 7u) / 8u * 8u; }
}  // namespace
void Gow1NativeSceneHeight(PPCRegister& r11) {  // after `li r11,720` at 0x824BC444
  if (!Gow1NativeLines()) return;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("gow1_native_lines: screen height {} -> {}", r11.u32, Gow1NativeLines());
  r11.u64 = Gow1NativeLines();
}
void Gow1NativeSceneWidth(PPCRegister& r31) {  // before `li r5,120` at 0x824BC47C: the clamped width
  if (!Gow1NativeLines()) return;
  static std::atomic<bool> logged{false};
  const uint32_t width = Gow1NativeWidth(r31.u32);
  if (!logged.exchange(true)) REXLOG_INFO("gow1_native_lines: screen width {} -> {}", r31.u32, width);
  r31.u64 = width;
}
void Gow1NativeDeviceHeight(PPCRegister& r11) {  // after `li r11,720` at 0x824BC490: the height beside that width
  if (Gow1NativeLines()) r11.u64 = Gow1NativeLines();
}
// 0.6.6 (job 2189: at true 1080p the pause menu sat in the top-left 1280x720 of the 1920x1080 picture): the game's
// FViewport still reports the console's 1280x720 to the UI (ue3/native_viewport.h: why, the checks, and the Gears 2 / 3
// code it was read from: the UI scene client's GetViewportSize asks RenderViewport->GetSizeX / GetSizeY, virtual calls).
// Gears 1's own code was NOT read for this (its generated files were not staged): the object is GEngine (cheats.cpp's
// 0x82FCDD50) -> GameViewport -> Viewport with both offsets from the script classes (GameEngine.GameViewport,
// GameViewportClient.Viewport) through ue3::obj - no guessed fallback - and Install checks the getters at run time
// (slots 2 / 3 first, then any neighbouring pair: the 2006 engine's FViewport may order its virtuals differently).
// Called from cheats.cpp's FEngineLoop::Tick override with its object-table opener; every 30 ticks until on.
void Gow1NativeUiViewportTick(bool (*open_objects)()) {
  namespace nv = ue3::native_viewport;
  namespace obj = ue3::obj;
  if (!Gow1NativeLines()) return;
  static uint32_t ticks = 0;
  static int fails = 0, gv_off = -1, vp_off = -1;
  if (++ticks % 30 || fails > 20) return;
  if (gv_off < 0 || vp_off < 0) {
    if (ticks % 300 || !open_objects || !(obj::Ready() || open_objects())) return;  // FindClass misses: every 5 s at most
    const uint32_t ge = obj::FindClass("GameEngine"), gvc = obj::FindClass("GameViewportClient");
    if (!ge || !gvc) { ++fails; return; }
    gv_off = obj::PropertyOffset(ge, "GameViewport");
    vp_off = obj::PropertyOffset(gvc, "Viewport");
    REXLOG_INFO("gow1_native_lines: GameEngine.GameViewport +{}, GameViewportClient.Viewport +{} (-1 = not found)", gv_off,
                vp_off);
    if (gv_off < 0 || vp_off < 0) {
      fails = 100;
      REXLOG_WARN("gow1_native_lines: the menus stay laid out in 1280x720 - the viewport properties were not found");
      return;
    }
  }
  const uint32_t engine = obj::Read32(0x82FCDD50u);
  const uint32_t client = engine ? obj::Read32(engine + uint32_t(gv_off)) : 0;
  const uint32_t viewport = client ? obj::Read32(client + uint32_t(vp_off)) : 0;
  if (!viewport) return;
  if (nv::g_installed.load(std::memory_order_relaxed)) {
    nv::NoteViewport(viewport);
    return;
  }
  const nv::Result r = nv::Install(viewport, Gow1NativeLines());
  if (r.ok) {
    REXLOG_INFO("gow1_native_lines: UI scene viewport {}x{} -> {}x{} (the game's FViewport 0x{:08X}: GetSizeX / GetSizeY "
                "0x{:08X} / 0x{:08X}, vtable slots {} / {}) - the menus lay out in the larger screen",
                r.w0, r.h0, r.w1, r.h1, viewport, r.fn_x, r.fn_y, r.slot, r.slot + 1);
  } else if (++fails > 20) {
    REXLOG_WARN("gow1_native_lines: the menus stay laid out in 1280x720 - {}", r.why);
  }
}

// 0.6.7 (GitHub #408, Gears 1 part): true 1080p menus, as Gears 2's 1ba61f7 / 27cffac. Gears 1's FViewport is already
// 1920x1080 by itself (the size function above), so the hook above changes nothing ("1920x1080 -> 1920x1080" in #408's
// log) and the UI scenes lay out in 1920x1080 screen pixels: their pixel-authored parts (the 1280-wide backdrop images,
// logo, text) stay 720p-sized - the title screen, main menu and pause menu of #408 and of jobs 2189 / 2200 / 2204 alike.
// So, as in Gears 2: the UI scene client's GetViewportSize reports the console's 1280x720 (a size above 1280 / 720 is
// divided by lines / 720) and the game scales the scenes to the screen (Gears 2, job 2206).
// The function, sub_82304590 (TU5; 0x82304590..0x82304638 in codegen.partition.json): Canary's TU5 21:9 patch by boma
// writes `li r3,550` / `b +8` at 0x823045F0 - the same two words his Gears 2 patch writes at disc 0x826E3720 / TU6
// 0x826E9BF0 ("Set UIScene viewport height", in Gears 2's GetViewportSize sub_826E36C0 / sub_826E9B90), and all three
// sit 0x60 bytes into their function. Its code was NOT read here (gow1_recomp.131.cpp was not staged), so nothing more
// is assumed: the code must hold at least two virtual calls (GetSizeX / GetSizeY, `bctrl`), checked once; and the output
// is recognised, not assumed - after the original, r5 (UT3's GetViewportSize(Scene, out)) or else r4 must point at two
// floats holding the screen size (height = lines, width above 1280); that register is used from then on. Otherwise
// nothing changes (0.6.6) and it is logged once. Gears 2's CanvasToScreen matrix step is not carried over (all zeros in
// Gears 2 TU6, it never acted - notes 20 Nov 19:28).
REXCVAR_DEFINE_BOOL(gow1_native_ui_layout720, true, "Gears 1",
                    "With --gow1_native_lines: menus and other UI scenes lay out in the console's 1280x720 and are "
                    "scaled to the whole screen (false = the 0.6.6 behaviour, laid out in screen pixels)");
namespace {
constexpr uint32_t kGow1UiViewportSize = 0x82304590u, kGow1UiViewportSizeEnd = 0x82304638u;
std::atomic<int> g_ui_size_code{-1};  // -1 = not checked yet, 0 = not the expected code (left alone), 1 = ok
std::atomic<int> g_ui_size_out{0};    // the register holding the output: 0 = not found yet, 4 / 5, -1 = given up
float Gow1LoadF(const uint8_t* base, uint32_t a) {
  const uint32_t v = ue3::native_viewport::Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void Gow1StoreF(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
bool Gow1UiSizeCodeOk(const uint8_t* base) {
  int v = g_ui_size_code.load(std::memory_order_relaxed);
  if (v >= 0) return v == 1;
  int calls = 0;
  for (uint32_t a = kGow1UiViewportSize; a < kGow1UiViewportSizeEnd; a += 4)
    if (ue3::native_viewport::Load32(base, a) == 0x4E800421u) ++calls;  // bctrl
  v = calls >= 2 ? 1 : 0;
  g_ui_size_code.store(v);
  if (v)
    REXLOG_INFO("gow1_native_ui_layout720: GetViewportSize 0x{:08X} holds {} virtual calls - on", kGow1UiViewportSize, calls);
  else
    REXLOG_WARN("gow1_native_ui_layout720: 0x{:08X} holds {} virtual calls, not GetSizeX / GetSizeY - the menus stay laid "
                "out in screen pixels (0.6.6)", kGow1UiViewportSize, calls);
  return v == 1;
}
// Two floats at `out` holding the screen size: height = lines (+-0.5), width above 1280 (1920 at 16:9, 1728 at 16:10).
bool Gow1UiOutIsScreen(const uint8_t* base, uint32_t out) {
  if (!ue3::native_viewport::InHeap(out) || !ue3::obj::Readable(out, 8)) return false;
  const float w = Gow1LoadF(base, out), h = Gow1LoadF(base, out + 4);
  return std::fabs(h - float(Gow1NativeLines())) < 0.5f && w > 1280.5f && w < 8192.0f;
}
}  // namespace
extern "C" void __imp__sub_82304590(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82304590(PPCContext& __restrict ctx, uint8_t* base) {
  // the arguments from entry: after the call r3 is the UBOOL result (Gears 2's 27cffac)
  const uint32_t a4 = ctx.r4.u32, a5 = ctx.r5.u32;
  __imp__sub_82304590(ctx, base);
  if (!Gow1NativeLines() || ctx.r3.u32 == 0 || !REXCVAR_GET(gow1_native_ui_layout720) || !Gow1UiSizeCodeOk(base)) return;
  int reg = g_ui_size_out.load(std::memory_order_relaxed);
  if (reg < 0) return;
  if (reg == 0) {
    static std::atomic<int> misses{0};
    if (Gow1UiOutIsScreen(base, a5)) {
      reg = 5;
    } else if (Gow1UiOutIsScreen(base, a4)) {
      reg = 4;
    } else {
      if (misses.fetch_add(1) == 200) {
        g_ui_size_out.store(-1);
        REXLOG_WARN("gow1_native_ui_layout720: GetViewportSize's output not found (r5 0x{:08X}, r4 0x{:08X}) - the menus stay "
                    "laid out in screen pixels (0.6.6)", a5, a4);
      }
      return;
    }
    g_ui_size_out.store(reg);
  }
  const uint32_t out = reg == 5 ? a5 : a4;
  if (!ue3::native_viewport::InHeap(out)) return;
  const float w0 = Gow1LoadF(base, out), h0 = Gow1LoadF(base, out + 4);
  const float k = float(Gow1NativeLines()) / 720.0f;
  const float w1 = w0 > 1280.5f ? std::round(w0 / k) : w0, h1 = h0 > 720.5f ? std::round(h0 / k) : h0;
  Gow1StoreF(base, out, w1);
  Gow1StoreF(base, out + 4, h1);
  static std::atomic<bool> told{false};
  if (!told.exchange(true))
    REXLOG_INFO("gow1_native_lines: UI scene layout size {}x{} -> {}x{} (console pixels; GetViewportSize 0x{:08X}, output in "
                "r{})", w0, h0, w1, h1, kGow1UiViewportSize, reg);
}

// 0.6.7, second half of the Gears 1 menu fix (job 2246: with the layout size alone the title screen and main menu were
// a 1280x720 box in the top-left of the 1920x1080 picture - this engine does not scale a UI layout to the screen).
// Read in the TU5 generated code (job af01):
//   - FCanvas: +4 the render target, +12 / +16 the transform stack (TArray of 64-byte matrices, row vectors: m00 at
//     +0, m11 at +20). Its constructor sub_823399A0 builds the base transform from the RENDER TARGET's size
//     (GetSizeX / GetSizeY, vtable slots 0 / 1: 1 / (W / 2), -1 / (H / 2)) - never from GetViewportSize, which is why
//     the 1280x720 layout stayed 1280x720 screen pixels. (Gears 2 builds its UI transform from GetViewportSize.)
//   - sub_8233A0D8 = FCanvas::PushRelativeTransform(M): pushes M x top.
//   - sub_82306D00 (scene client, r4 = FCanvas*, r5 = the scene / widget): multiplies the canvas opacity by the
//     object's, PushRelativeTransform(the object's own matrix, sub_822E1E60), pre-render (vtable +420), the client's
//     render (+268), post-render (+424), pops the transform. The only PushRelativeTransform in the scene client.
// So while sub_82306D00 runs, the top of the canvas' transform stack is multiplied by lines / 720 in its rows 0 and 1
// (layout pixel -> screen pixel, applied last because every later push is "M x top"), and put back afterwards. Only the
// outermost call scales (a nested call already draws under the scaled top). Only with the layout hook above active
// and after the code check (the `bl sub_8233A0D8` at 0x82306D54); the HUD and everything else drawn on the same canvas
// outside UI scenes is not touched.
namespace {
constexpr uint32_t kGow1UiRenderPushSite = 0x82306D54u, kGow1UiRenderPushWord = 0x48033385u;  // bl 0x8233A0D8
std::atomic<int> g_ui_render_code{-1};
thread_local int t_ui_render_depth = 0;
bool Gow1UiRenderCodeOk(const uint8_t* base) {
  int v = g_ui_render_code.load(std::memory_order_relaxed);
  if (v >= 0) return v == 1;
  const uint32_t w = ue3::native_viewport::Load32(base, kGow1UiRenderPushSite);
  v = w == kGow1UiRenderPushWord ? 1 : 0;
  g_ui_render_code.store(v);
  if (v)
    REXLOG_INFO("gow1_native_ui_layout720: UI scene render 0x82306D00 found (PushRelativeTransform call at 0x{:08X}) - "
                "scenes are scaled to the screen", kGow1UiRenderPushSite);
  else
    REXLOG_WARN("gow1_native_ui_layout720: 0x{:08X} holds 0x{:08X}, not the expected call - UI scenes are not scaled",
                kGow1UiRenderPushSite, w);
  return v == 1;
}
}  // namespace
extern "C" void __imp__sub_82306D00(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82306D00(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t canvas = ctx.r4.u32;
  uint32_t top = 0;
  float saved[8] = {};
  // the layout hook must have recognised its output (g_ui_size_out 4 / 5): otherwise the scenes still lay out in
  // screen pixels and must not be scaled
  if (Gow1NativeLines() > 720u && REXCVAR_GET(gow1_native_ui_layout720) && g_ui_size_out.load(std::memory_order_relaxed) > 0 &&
      t_ui_render_depth == 0 && Gow1UiRenderCodeOk(base) && ue3::native_viewport::InHeap(canvas) &&
      ue3::obj::Readable(canvas, 24)) {
    const uint32_t data = ue3::native_viewport::Load32(base, canvas + 12), count = ue3::native_viewport::Load32(base, canvas + 16);
    if (count >= 1 && count < 256 && ue3::native_viewport::InHeap(data) && ue3::obj::Readable(data + (count - 1) * 64u, 64)) {
      top = data + (count - 1) * 64u;
      const float k = float(Gow1NativeLines()) / 720.0f;
      for (int i = 0; i < 8; ++i) {
        saved[i] = Gow1LoadF(base, top + uint32_t(i) * 4u);
        Gow1StoreF(base, top + uint32_t(i) * 4u, saved[i] * k);
      }
      static std::atomic<int> told{0};
      if (told.fetch_add(1) < 3)
        REXLOG_INFO("gow1_native_ui_layout720: scene 0x{:08X} on canvas 0x{:08X} (transform stack {}): rows 0 / 1 x {} - "
                    "m00 {} -> {}, m11 {} -> {}, translation {} {}", ctx.r5.u32, canvas, count, k, saved[0], saved[0] * k,
                    saved[5], saved[5] * k, Gow1LoadF(base, top + 48), Gow1LoadF(base, top + 52));
    }
  }
  ++t_ui_render_depth;
  __imp__sub_82306D00(ctx, base);
  --t_ui_render_depth;
  if (top) {
    // the stack is back at the same depth (the function pops what it pushed); the array may have been reallocated by
    // its push, so the address is taken again
    const uint32_t data = ue3::native_viewport::Load32(base, canvas + 12), count = ue3::native_viewport::Load32(base, canvas + 16);
    if (count >= 1 && count < 256 && ue3::native_viewport::InHeap(data) && ue3::obj::Readable(data + (count - 1) * 64u, 64)) {
      const uint32_t now = data + (count - 1) * 64u;
      for (int i = 0; i < 8; ++i) Gow1StoreF(base, now + uint32_t(i) * 4u, saved[i]);
    }
  }
}

// 0.6.5 (Shagg 16 Nov: 16:10): with a 1152x720 video mode Gears 2 / 3 / Judgment fill a 16:10 screen, but Gears 1 kept
// 16:9 with bars (job 1969's gameplay shots): its view code (sub_82392008, TU5) letterboxes to the camera's aspect when
// the camera's constrain flag (bit 1 of the word at +432) is set. Canary's TU5 16:10 patch by boma makes the branch at
// 0x82392530 always skip that; here the flag test reads 0 instead (after the rlwinm at 0x82392528). Checked at start-up.
REXCVAR_DEFINE_BOOL(gow1_no_aspect_lock, false, "Gears 1",
                    "The view fills the picture's own shape (16:10) instead of a letterboxed 16:9 (Canary's TU5 patch by boma)");
namespace {
std::atomic<bool> g_no_gaussian{false};
std::atomic<bool> g_no_aspect_lock{false};
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
  if (REXCVAR_GET(gow1_no_aspect_lock)) {
    if (GuestWord(m, 0x82392528) != 0x556B0042u || GuestWord(m, 0x82392530) != 0x419A009Cu) {   // rlwinm r11,r11,0,1,1; beq cr6,+0x9C
      REXLOG_WARN("gow1_no_aspect_lock: the code at 0x82392528 is not TU5's - left alone");
    } else {
      g_no_aspect_lock.store(true);
      REXLOG_INFO("gow1_no_aspect_lock: on (the view fills the picture's shape)");
    }
  }
}
void Gow1NoAspectLock(PPCRegister& r11) {   // after `rlwinm r11,r11,0,1,1` at 0x82392528: the constrain flag
  if (g_no_aspect_lock.load(std::memory_order_relaxed)) r11.u64 = 0;
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
// GitHub #400 (hevesij, 0.6.5): "only the Microsoft logo is skipped; the Epic Games logo still appears". The two step
// hooks above only act when the player steps from one film to the next (sub_8222E220); a logo that was started another
// way - the first film of a copy without a rating film - played in full. The film player's per-tick check
// (sub_8222D9C0) reads the pads and, on a button, calls its skip routine (sub_8222E088: the first call marks the
// start-up films as being skipped, the next one stops the film and steps on). With --gow1_skip_logos, while the film
// index (r31 + 68) is 1 (EpicLogo) or 2 (MGSLogo), the tick's "a button was pressed" flag (r28) is set just before it
// is tested (0x8222DB00) - exactly what a player pressing A at the logo does, however that logo was started.
void Gow1SkipLogoTick(PPCRegister& r28, PPCRegister& r31) {
  if (!REXCVAR_GET(gow1_skip_logos)) return;
  rex::Runtime* runtime = rex::Runtime::instance();
  const uint8_t* m = runtime ? runtime->virtual_membase() : nullptr;
  if (!m || !r31.u32) return;
  const uint32_t index = GuestWord(m, r31.u32 + 68);
  if (index != 1 && index != 2) return;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("gow1_skip_logos: logo film {} playing - skipped as a button press would", index);
  r28.u64 = 1;
}

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
// Review 049: the in-game menu's live frame cap (an atomic from the UI thread), else the start-up cvar.
static int32_t LiveMaxFps() {
  int32_t v = 0;
  return XenosLiveMaxFps(&v) ? v : REXCVAR_GET(gow1_max_fps);
}

void Gow1PresentInterval(PPCRegister& r10) {
  static std::atomic<uint64_t> calls{0};
  const uint64_t n = calls.fetch_add(1) + 1;
  if (n == 1 || n == 600) REXLOG_INFO("gow1_max_fps: present-interval site reached {} time(s) (interval {})", n, uint32_t(r10.u32));
  const int32_t cap = LiveMaxFps();
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
  if (mode == 2 && !vsync_on && LiveMaxFps() <= 0) return;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("gow1_query_wait: on - the query wait loop sleeps between polls");
  thread_local Gow1WaitStreak streak;
  Gow1SleepStep(streak, 32);
}

// 0.5.0 (job 1507, Gears1-x1): a rare boot crash while the ini files load - two threads call sub_8243F3B8, which locks
// the critical section at (this->+8)+4 around its work, before another thread has made that object (this->+8 still 0:
// RtlEnterCriticalSection on address 4). After 'lwz r11,8(r31)' (0x8243F3D8) a 0 is waited out (up to 3 s, 1 ms steps)
// until the object exists; the game then goes on exactly as it would have. Nothing changes when it is already there.
REXCVAR_DEFINE_BOOL(gow1_boot_lock_wait, true, "Gears 1",
                    "Wait for the ini loader's lock object instead of crashing on it at start-up (job 1507)");
namespace {
uint8_t* g_lock_membase = nullptr;
std::atomic<uint32_t> g_lock_waits{0};
}  // namespace
void Gow1InstallBootLockWait(rex::Runtime* runtime) {
  if (!runtime || !REXCVAR_GET(gow1_boot_lock_wait)) return;
  uint8_t* m = runtime->virtual_membase();
  if (!m) return;
  if (GuestWord(m, 0x8243F3D8) != 0x817F0008u) {   // lwz r11,8(r31)
    REXLOG_WARN("gow1_boot_lock_wait: the code at 0x8243F3D8 is not the expected one - left alone");
    return;
  }
  g_lock_membase = m;
  REXLOG_INFO("gow1_boot_lock_wait: on");
}
// 0.5.0: the camera FOV for the mouse's aim / zoom sensitivity (gow1_manifest.toml, two projection paths).
void Gow1ViewFov(PPCRegister& f28) {
  CogMouseLookNoteFov(f28.f64);
  static std::atomic<int> logged{0};
  static double last = -1.0;
  if (f28.f64 != last && logged.load(std::memory_order_relaxed) < 12) {
    last = f28.f64;
    logged.fetch_add(1);
    REXLOG_INFO("gow1_view_fov: camera FOV {:.2f} deg", f28.f64);
  }
}

void Gow1ViewFovB(PPCRegister& f28) { Gow1ViewFov(f28); }   // the second projection path

void Gow1BootLockWait(PPCRegister& r11, PPCRegister& r31) {   // after lwz r11,8(r31) at 0x8243F3D8
  if (r11.u32 != 0 || !g_lock_membase) return;
  const uint32_t at = r31.u32 + 8;
  for (int ms = 0; ms < 3000; ++ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const uint32_t v = GuestWord(g_lock_membase, at);
    if (v) {
      r11.u64 = v;
      if (g_lock_waits.fetch_add(1) < 8) REXLOG_INFO("gow1_boot_lock_wait: waited {} ms for the ini loader's lock object", ms + 1);
      return;
    }
  }
  REXLOG_WARN("gow1_boot_lock_wait: the lock object at {:08X} never appeared (3 s)", at);
}
