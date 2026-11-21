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
#include <rex/filesystem.h>   // rex::path_to_utf8 (#193 log)
#include <rex/filesystem/vfs.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <xenos/dev_tools.h>
#include <xenos/frame_limiter.h>
#include <xenos/display_size.h>

#include "gow3_addresses.h"
#include <ue3/objects.h>
#include <ue3/script_input.h>
#include <ue3/console.h>  // Ue3ConsoleQueue (GitHub #208)
#include <ue3/name_tags.h>
#include <ue3/native_viewport.h>  // 0.6.6 true 1080p menus
#include <iterator>   // std::size  // GitHub #353
#include "mouse_look.h"
#include "main_menu_exit.h"

#include <algorithm>
#include <tuple>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <format>
#include <mutex>   // 0.6.7: the file-handle map lock
#include <string>
#include <cstdio>
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
// The game draws a 1280 x H scene with H = 1280 / aspect (a multiple of 8, 240-720), changed at the same sites as the
// Canary "21:9 Aspect Ratio Ultrawide Patch" (boma) for this XEX, with the view widened exactly (Hor+):
//   0x8261BEF0 `lfs f0,0x630(r27)` (the field-of-view factor) -> after it, f0 = the widened factor;
//   0x8261BE60 `beq cr6,+0x90` -> always taken (ConstrainAspectRatio off);
//   0x82960C78 `li r11,720` (the scene buffer height, stored to 0x8339242C) -> after it, r11 = H;
//   0x827C583C `lwz r30,0x242c(r9)` (that height read back by the scene render targets' sizing, TU6 sub_827D7AA8) ->
//     left at H since 0.6.3 (GitHub #330; the Canary patch and 0.6.2 forced 720 here, see Gow3UltrawideSizeCheck);
//   0x827C6CCC `addi r3,r1,260` (scene shared memory block 2 created next) -> its size kept at the 16:9 size, so the
//     shadow depth texture placed in it still fits (0.6.3, Gow3UltrawideSharedMemory; not in the Canary patch);
//   0x82BC9D84 `stw r4,0x88(r3)` -> skipped (pillarboxed pre-rendered videos);
//   the .rdata float at 0x820CDE2C (16:9 = 1.77778) -> the new aspect, written at load.
// The front buffer becomes 1280 x H (x the resolution scale), shown at its own shape (--gow2_present_frame_aspect):
// it fills a window of that shape, letterboxed in others. Shapes narrower than 16:9 (16:10, 4:3) stay 16:9: a taller
// scene buffer may not fit EDRAM [unchecked].
REXCVAR_DEFINE_BOOL(gow3_ultrawide, false, "Gears3",
                    "21:9 ultrawide (Canary patch by boma): a 1280x552 picture (x the resolution scale) with a wider "
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

// GitHub #330: before 0.6.3 the scene render targets stayed 1280x720 under a 1280xH picture; the Stats & Awards pages
// (Scaleform, whose blend effects copy the screen into those targets and draw it back as if they were the screen's
// size) then drew stacked blue bands. True (the old way) only to compare, or if a very wide shape fails at start-up.
REXCVAR_DEFINE_BOOL(gow3_aspect_targets_720, false, "Gears3",
                    "With --gow3_aspect / --gow3_ultrawide: keep the scene render targets at the 16:9 size 1280x720 as "
                    "the Canary patch does (before 0.6.3). Off = sized for the picture, as the game sizes them at 16:9 "
                    "(GitHub #330: on, the Stats & Awards pages draw stacked blue bands)");

namespace {
// Canary's 21:9 scene height is 550; the game rounds its scene render targets up to 8 rows (sub_827D7AA8), so every
// height here is a multiple of 8 and the targets match the picture exactly (GitHub #330): 550 -> 552.
constexpr uint32_t kCanaryUltrawideHeight = 552;
uint32_t SceneRows(double rows) { return std::clamp(uint32_t(std::lround(rows / 8.0)) * 8u, 240u, 720u); }

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
    // GitHub #293 (0.6.2): down to 240 rows = 48:9 (three 16:9 screens side by side); 320 (4:1) before.
    // GitHub #330 (0.6.3): a multiple of 8 rows (was: even), as the game's scene render targets are.
    const uint32_t h = SceneRows(1280.0 / aspect);
    REXLOG_INFO("gow3_aspect: '{}' = {:.4f} -> scene 1280x{} ({:.4f})", v, aspect, h, 1280.0 / h);
    return h;
  }();
  return height;
}
bool Wide() { return WideHeight() != 0; }
}  // namespace

void Gow3UltrawideFov(PPCRegister& f0, PPCRegister& f31) {  // after `lfs f0,0x630(r27)` at 0x8261BEF0
  CogMouseLookNoteFov(f31.f64);  // GitHub #114: the camera's own FOV, also without ultrawide
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
// GitHub #207 (gow3_manifest_tu6.toml): a voice callback whose object is already gone (no vtable) is not called
bool Gow3VoiceCallbackGone(PPCRegister& r3) {
  const uint8_t* m = rex::Runtime::instance()->virtual_membase();
  const uint32_t obj = r3.u32;
  const uint8_t* p = m + obj;
  const uint32_t vtable = obj ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3] : 0;
  if (obj && vtable) return false;
  static std::atomic<int> told{0};
  if (told.fetch_add(1) < 3) REXLOG_WARN("gow3: voice callback object {:08X} has no vtable - its call skipped (GitHub #207)", obj);
  return true;
}
bool Gow3UltrawideNoConstrain() { return Wide(); }  // jump_address_on_true: the beq at 0x8261BE60 is taken

// ---- Experiment (Shagg, 20 Nov): true 1080p, as Gears 2's --gow2_native_lines ---------------------------------------
// The same function as Gears 2's sub_82816638 (TU6 sub_82971BC0): it makes the screen height 720 (`li r11,720`, the
// Gow3UltrawideSceneHeight site) and the width aspect x 720 clamped to 900..1280 - stored at 0x82971C80
// (`stw r11,...(r10)`, below 1280) or, after `li r11,1280` at 0x82971F3C, at the next instruction. --gow3_native_lines=1080 gives both numbers: 1920x1080. Needs
// --gow2_renderer_edram_extended=true (10 MiB of EDRAM cannot hold 1080p). Not with a wider picture shape
// (--gow3_ultrawide / --gow3_aspect). NOT RUN YET: written from the generated code, to be proven by a gameplay run.
REXCVAR_DEFINE_INT32(gow3_native_lines, 0, "Gears3",
                     "Experiment (true 1080p): the game's own screen height in lines, 0 = 720 as on the console. 1080 "
                     "makes the game draw 1920x1080 itself (width = lines x 16 / 9, a multiple of 8). Needs "
                     "--gow2_renderer_edram_extended=true. With --gow3_ultrawide / --gow3_aspect the wide picture is "
                     "scaled the same way")
    .range(0, 1440);
namespace {
uint32_t Gow3NativeLines() {
  static const uint32_t lines = [] {
    const int v = REXCVAR_GET(gow3_native_lines);
    if (v <= 720) return 0u;   // 0.6.6: also with a wider picture shape (its height is scaled by lines / 720)
    return uint32_t(v + 7) / 8u * 8u;
  }();
  return lines;
}
// The game's own width times lines / 720 (a multiple of 8): the 16:10 and 4:3 video modes keep their shape.
uint32_t Gow3NativeWidth(uint32_t game_width) { return (game_width * Gow3NativeLines() / 720u + 7u) / 8u * 8u; }
}  // namespace
void Gow3NativeSceneWidth(PPCRegister& r11) {  // before the width's store (below 1280)
  if (!Gow3NativeLines()) return;
  const uint32_t width = Gow3NativeWidth(r11.u32);
  GOW3_LOG_ONCE("gow3_native_lines: screen width {} -> {}", r11.u32, width);
  r11.u64 = width;
}
void Gow3NativeSceneWidthMax(PPCRegister& r11) { Gow3NativeSceneWidth(r11); }  // after `li r11,1280` (one name per hook)

void Gow3UltrawideSceneHeight(PPCRegister& r11) {  // after `li r11,720` at 0x82960C78
  if (Gow3NativeLines()) {
    const uint32_t rows = Wide() ? (WideHeight() * Gow3NativeLines() / 720u + 1u) & ~1u : Gow3NativeLines();
    GOW3_LOG_ONCE("gow3_native_lines: screen height {} -> {}{}", r11.u32, rows, Wide() ? " (wide shape)" : "");
    r11.u64 = rows;
    return;
  }
  if (!Wide()) return;
  GOW3_LOG_ONCE("gow3_aspect: scene buffer height {} -> {}", r11.u32, WideHeight());
  r11.u64 = WideHeight();
}
// 0.6.6 (job 2189: at true 1080p the menus sat in the top-left 1280x720 of the 1920x1080 picture): the game's FViewport
// still reports the console's 1280x720 (ue3/native_viewport.h has the why and the checks). Gears 3's menus are Scaleform
// movies whose viewport the GFx engine takes from that FViewport: TU6 sub_829343E0 (one movie) and sub_82934188 (every
// movie) start from `li r30,1280` / `li r9,720`, then `lwz r11,72(r3)` (the engine's Viewport), `lwz r10,8(r11)` /
// `bctrl` (GetSizeX) and `lwz r7,12(r8)` / `bctrl` (GetSizeY), and hand the size to the movie (vtable +100,
// SetViewport; sub_82931728 does the same). UGameViewportClient::GetViewportSize (TU6 sub_826266B8: `lwz r11,68(r3)`,
// the same two slots) and its script native (sub_8252C2A0) ask the same object. The object: GEngine -> GameViewport
// (+1196: `lwz r3,1196(r11)` after GEngine in sub_828F4B98 and seven more callers of sub_826266B8) -> Viewport (+68).
// Once per engine tick until on (checked every 30 ticks); then the object is followed in case it is made again.
void Gow3NativeUiViewportTick() {
  namespace nv = ue3::native_viewport;
  if (!Gow3NativeLines()) return;
  static uint32_t ticks = 0;
  static int fails = 0;
  if (++ticks % 30 || fails > 20) return;
  const uint32_t engine = ue3::obj::Read32(gow3::addr::kGEngine);
  const uint32_t client = engine ? ue3::obj::Read32(engine + 1196u) : 0;
  const uint32_t viewport = client ? ue3::obj::Read32(client + 68u) : 0;
  if (!viewport) return;
  if (nv::g_installed.load(std::memory_order_relaxed)) {
    nv::NoteViewport(viewport);
    return;
  }
  const nv::Result r = nv::Install(viewport, Gow3NativeLines());
  if (r.ok) {
    REXLOG_INFO("gow3_native_lines: UI scene viewport {}x{} -> {}x{} (the game's FViewport 0x{:08X}: GetSizeX / GetSizeY "
                "0x{:08X} / 0x{:08X}, vtable slots {} / {}) - menus and Scaleform movies lay out in the larger screen",
                r.w0, r.h0, r.w1, r.h1, viewport, r.fn_x, r.fn_y, r.slot, r.slot + 1);
  } else if (++fails > 20) {
    REXLOG_WARN("gow3_native_lines: the menus stay laid out in 1280x720 - {}", r.why);
  }
}
// After `lwz r30,0x242c(r9)` at 0x827C583C (TU6 `lwz r30,0x2bd8(r9)` at 0x827D7ACC), in the scene render targets'
// sizing (TU6 sub_827D7AA8, UE3 FSceneRenderTargets::Allocate on Xbox): r29 / r30 = the screen width / height globals,
// grown into BufferSizeX/Y (+204 / +208, rounded up to 8) when larger, then the targets are made again. At 16:9 the
// targets are exactly the screen's size, and the game's code relies on that: the Stats & Awards pages (a Scaleform
// movie) copy the screen into a scene target ~40 times a frame (resolve to a 1280 x BufferSizeY texture) and draw it
// back with tri-strip quads as if the texture were the screen's size [E: from the band ratios 720/540 and 720/480; the
// quads' coordinates are not logged]. The Canary patch kept r30 = 720 here, so under a 1280x540 picture each copy
// came back squeezed to 540/720 of its height: the stacked teal bands of GitHub #330 (job 1905: R1
// 64:27 / D3D12, R3 1x, R4 Vulkan 8:3 with bands 720/480 apart; R2 16:9 clean). The game's own value (H, a multiple
// of 8 - WideHeight) keeps targets and picture the same size, as at 16:9. Its post-processing scales by BufferSize
// itself, so the 3D picture is unchanged.
void Gow3UltrawideSizeCheck(PPCRegister& r30) {
  if (!Wide()) return;
  if (REXCVAR_GET(gow3_aspect_targets_720)) {
    GOW3_LOG_ONCE("gow3_aspect: scene render targets kept at 1280x720 under the 1280x{} picture "
                  "(--gow3_aspect_targets_720; GitHub #330)", WideHeight());
    r30.u64 = 720;
    return;
  }
  // Review r88 L1: log the game's own value each time it changes (a later change re-creates the targets), and say so
  // when it is not the picture's height. Cheap: the size check runs about once a frame.
  static std::atomic<uint32_t> last{0};
  if (last.exchange(r30.u32) != r30.u32) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 8)
      REXLOG_INFO("gow3_aspect: scene render targets sized from screen height {} (picture 1280x{}; 720 before 0.6.3, "
                  "GitHub #330){}", r30.u32, WideHeight(), r30.u32 == WideHeight() ? "" : " - NOT the picture height");
  }
}
// After `addi r3,r1,260` at 0x827C6CCC (TU6 0x827D8F5C), just before the `bl` to RHICreateSharedMemory (disc
// sub_82966ED0 / TU6 sub_82977F18) for the scene's shared memory block 2 (GSceneRenderTargets+268) in InitDynamicRHI
// (disc sub_827C6A20 / TU6 sub_827D8CB0): r5 = its size, max(BufferSize fmt 2, BufferSize fmt 19) = 1280 x
// align32(BufferSizeY) x 4. The game also creates the 1120x688 whole-scene shadow depth texture (0x302000 bytes) in
// that block (TU6 call 17), and RHICreateSharedTexture2D (TU6 sub_82977F90) checks that it fits - DbgBreakPoint
// otherwise. At 16:9 the block is 1280x736x4 = 0x398000; with the targets following a picture under ~616 rows
// (every shape wider than ~2.08:1) it is smaller, and job 1907 trapped at the first scene render (~25 s in). Keep the
// 16:9 capacity (the size 0.6.2 allocated) so the targets can follow H (GitHub #330). Block 1 has its own shadow term.
void Gow3UltrawideSharedMemory(PPCRegister& r5) {
  if (!Wide() || REXCVAR_GET(gow3_aspect_targets_720)) return;
  constexpr uint32_t kStock720 = 1280u * 736u * 4u;  // 0x398000
  if (r5.u32 >= kStock720) return;
  GOW3_LOG_ONCE("gow3_aspect: scene shared memory block 2 0x{:X} -> 0x{:X} (the 1120x688 shadow depth texture lives "
                "there; GitHub #330)", r5.u32, kStock720);
  r5.u64 = kStock720;
}
// 0.6.7 (Shagg's 0.6.6 report, 21 Nov: Gears 3 at true 1080p, main menu squeezed into the top ~42% and drawn at 720p
// size; job 2200's A-1080p-Gears3 pause menu had the same 720p-sized text and a 1280x720 backdrop box in the centre,
// while U-1080uw-Gears3 - true 1080p WITH this skip, through Wide() - was scaled right). The store below is the
// Scaleform movies' scale mode (SetViewScaleMode, see GitHub #396 below). The game's own mode draws a movie's
// 1280x720 stage at 1:1 and lets its script place things against the viewport's edges: at 720 lines the viewport IS
// the stage, so any mode looks the same; at 1080 lines (FViewport 1920x1080, logged "UI scene viewport 1920x1080 ->
// 1920x1080") every menu keeps 720p pixel sizes and the scripts' anchoring moves the pieces (the main menu ~180 rows
// up). With the store skipped the movie keeps show-all, as under Ultrawide (the Canary patch): a full-screen 16:9
// viewport shows the stage scaled by exactly lines / 720 - the console's layout, larger. Split-screen / non-16:9
// viewports: show-all as under Ultrawide (not checked at 1080 lines).
REXCVAR_DEFINE_BOOL(gow3_native_ui_show_all, true, "Gears3",
                    "With --gow3_native_lines: Scaleform menus keep the show-all scale mode, so the console's 1280x720 "
                    "layout is scaled to the larger screen (false = the 0.6.6 behaviour: drawn at 720p size)");
bool Gow3UltrawideVideo(PPCRegister& r4) {  // jump_address_on_true: past `stw r4,0x88(r3)` at 0x82BC9D84
  if (!Wide()) {
    if (!Gow3NativeLines() || !REXCVAR_GET(gow3_native_ui_show_all)) return false;
    GOW3_LOG_ONCE("gow3_native_lines: Scaleform scale mode {} not stored - menus keep show-all (scaled x{:.2f})",
                  r4.u32, double(Gow3NativeLines()) / 720.0);
    r4.u64 = 1;
    return true;
  }
  GOW3_LOG_ONCE("gow3_aspect: 0x82BC9D84 store of {} skipped", r4.u32);
  r4.u64 = 1;  // as Canary's `li r4,1` in place of the store
  return true;
}

// GitHub #396 (Gears 3 part, 3440x1440): with a wider picture the menus showed other menus' pieces and stray art in the
// strips left and right of the 16:9 centre (the lobby's party list beside the Stats & Awards and Options panels, the
// title's background art ending short of the edges). The store skipped above is the Scaleform movie's scale mode
// (TU6 sub_82BDD3B8 = SetViewScaleMode: `stw r4,136(r3)`, then the movie's SetViewport, vtable +100, with the viewport
// kept at +64 - Judgment's 0x82CBA684 is the same function, "Scaleform's scale mode" there); Canary's 1 = show all:
// each movie's 16:9 stage sits in the centre of a full-width viewport, and whatever a movie places off its stage
// (art made wider than 16:9, a panel under a modal one) is drawn into the strips too, where at 16:9 it falls off the
// screen. Every movie's viewport is set through TU6 sub_82929C38 (GViewport on the stack: BufferWidth/Height, Left,
// Top, Width, Height = r4..r9, the scissor zeroed, flags r10; then vtable +100): from sub_829343E0 (one movie;
// sub_82934188 runs it for every movie, also after sub_82931728 sets a new FViewport) and sub_82932C28 [read in
// gow3_recomp.129 / .74 / .181 / .52]. A full-screen viewport wider than 16:9 becomes the 16:9 box in its centre:
// the layout is the same (show all in a 16:9 box = the stage exactly) and D3D's viewport clips the rest, so nothing
// off-stage reaches the strips [the clip: D3D9 / Xenos viewport behaviour, not seen in the game's renderer]. The
// strips keep what is under the menus (the 3D scene in game, black or the clear colour in the front end).
// --gow3_ultrawide_ui_full_width brings the old full-width viewport back.
REXCVAR_DEFINE_BOOL(gow3_ultrawide_ui_full_width, false, "Gears3",
                    "With --gow3_ultrawide / --gow3_aspect: Scaleform menus get the whole wide picture as their viewport, "
                    "as before 0.6.6 (GitHub #396: other menus' pieces show in the strips beside the 16:9 centre). "
                    "Off = each full-screen menu is drawn in a 16:9 box in the centre");
#if defined(GOW3_TU6) && GOW3_TU6
extern "C" void __imp__sub_82929C38(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82929C38(PPCContext& __restrict ctx, uint8_t* base) {
  if (Wide() && !REXCVAR_GET(gow3_ultrawide_ui_full_width)) {
    const uint32_t bw = ctx.r4.u32, bh = ctx.r5.u32, l = ctx.r6.u32, t = ctx.r7.u32, w = ctx.r8.u32, h = ctx.r9.u32;
    // only a viewport covering the whole buffer (split-screen and render-to-texture movies are left alone), and only
    // when it is wider than 16:9 by more than a rounding step
    if (l == 0 && t == 0 && w == bw && h == bh && h > 0 && uint64_t(w) * 9u > uint64_t(h) * 16u + 9u) {
      const uint32_t w169 = (h * 16u / 9u + 1u) & ~1u;
      const uint32_t left = (w - w169) / 2u;
      static std::atomic<int> told{0};
      if (told.fetch_add(1) < 4)
        REXLOG_INFO("gow3_aspect: Scaleform movie viewport {}x{} -> {}x{} from x {} (GitHub #396; flags 0x{:X})", w, h,
                    w169, h, left, ctx.r10.u32);
      ctx.r6.u64 = left;
      ctx.r8.u64 = w169;
    }
  }
  __imp__sub_82929C38(ctx, base);
}
#endif

// ---- Start-up crash "read of guest 0x4A9E7324" (jobs 1925 / 2253b: 1 in 7 to 1 in 44 Gears 3 starts) ----------------
// The async file system (UE3 FAsyncIOSystemBase, TU6) keeps a name -> file-handle map (NameToHandleMap, a
// TMap<FString, handle> at this+12, 32-byte pairs). Read in gow3_recomp (TU6):
//   - sub_822C86D8 (GetCachedFileHandle): looks a name up (sub_822C8810 -> sub_822BD110, the map's Find) and, when it
//     is not there, opens the file (vtable +52) and ADDS it (sub_822CB058, the map's Set - its only caller). Called
//     only from the file thread's loop sub_822C8E50, outside the request lock (that lock, [this+8]+4, guards the
//     request arrays only).
//   - sub_822CB1A8: the map's Remove; its only caller is the same loop (the "done with this file" request).
//   - sub_8296C3D8 (a virtual of the Xbox file system): looks a name up through the same sub_822C8810 with NO lock and
//     reads the handle - and this one runs on the GAME thread (crash stack: sub_822BD110+0x259 <- sub_822C8810 <-
//     sub_8296C3D8 <- sub_822FF6C0 <- sub_829B03C0 ..., while the file thread was in sub_822C8E50 -> sub_822C8088,
//     the step right after sub_822C86D8).
// So during start-up the game thread can walk the map while the file thread's Set moves the pairs (a grown array is
// reallocated, the hash rebuilt): the walk reads a pair that is no longer one, takes its "string" pointer and the
// compare (sub_82CDE308) reads unmapped memory. A race in the game's own code; the console's timing hid it.
// The three functions now run under one host lock, so a lookup from the game thread never overlaps an add / remove.
// The file thread's own lookups elsewhere need none (it is the only writer). Nothing inside waits on the game
// thread: sub_822C86D8 only opens a file and allocates, sub_8296C3D8 only compares and stores three words.
REXCVAR_DEFINE_BOOL(gow3_file_handle_map_lock, true, "Gears3",
                    "The async file system's name -> handle map is locked between the file thread (add / remove) and "
                    "the game thread's lookup (the rare start-up crash 'read of guest 0x4A9E7324'). false = as before");
#if defined(GOW3_TU6) && GOW3_TU6
namespace {
std::recursive_mutex g_file_handle_map_mutex;
void FileHandleMapLocked(void (*original)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base, const char* what,
                         std::atomic<bool>& told) {
  if (!REXCVAR_GET(gow3_file_handle_map_lock)) {
    original(ctx, base);
    return;
  }
  if (!told.exchange(true)) REXLOG_INFO("gow3_file_handle_map_lock: {} runs under the map lock", what);
  std::lock_guard lock(g_file_handle_map_mutex);
  original(ctx, base);
}
}  // namespace
extern "C" void __imp__sub_822C86D8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_822C86D8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<bool> told{false};
  FileHandleMapLocked(__imp__sub_822C86D8, ctx, base, "the file thread's find-or-open (sub_822C86D8)", told);
}
extern "C" void __imp__sub_822CB1A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_822CB1A8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<bool> told{false};
  FileHandleMapLocked(__imp__sub_822CB1A8, ctx, base, "the file thread's remove (sub_822CB1A8)", told);
}
extern "C" void __imp__sub_8296C3D8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8296C3D8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<bool> told{false};
  FileHandleMapLocked(__imp__sub_8296C3D8, ctx, base, "the game thread's lookup (sub_8296C3D8)", told);
}
#endif

// ---- Frame rate and the engine tick ---------------------------------------------------------------------------------
// GetMaxTickRate 0x8253D9A0: f1 = 0.0 (no limit); with frame-rate smoothing on (flag 0x8343F7D4, bSmoothFrameRate=TRUE
// in Xbox360-GearEngine.ini) `lfs f1,0x4C8(r3)` at 0x8253D9B0 loads MaxSmoothedFrameRate (62 in that ini) and returns
// it. The hook runs after that load and replaces the value: a settings override, not a code change (Canary's "Unlock
// FPS" nops the load instead). Above 30 also needs --gow2_gpu_vsync=false (the game asks for every second vblank).
REXCVAR_DEFINE_INT32(gow3_max_fps, 0, "Gears3",
                     "The engine's frame-rate limit (UE3 MaxSmoothedFrameRate, 62 in the game's own ini): 0 = the game's, "
                     "N = N fps, -1 = none. Above 30 also needs --gow2_gpu_vsync=false");
// Review 049: the in-game menu's live frame cap (an atomic from the UI thread), else the start-up cvar.
static int32_t LiveMaxFps() {
  int32_t v = 0;
  return XenosLiveMaxFps(&v) ? v : REXCVAR_GET(gow3_max_fps);
}
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

// GitHub #101: the front end (title screen, its menus) ran at the player's limit - with none, hundreds of fps and the
// graphics card at 100 % on a still menu. While the front-end package is the last level the game opened, the engine
// limit is held at --gow3_menu_fps. Signal: the file-open observer (SDK patch 32, chained through the shader reader):
// GearStart*.xxx opened -> front end; any other level package (*_P.xxx) -> a level, the player's limit again.
REXCVAR_DEFINE_INT32(gow3_menu_fps, 60, "Gears3",
                     "GitHub #101: the engine's frame-rate limit on the title screen and its menus (0 = the player's)");
REXCVAR_DEFINE_STRING(ue3_exec_each_level, "", "Gears3",
                      "GitHub #208: console commands (';'-separated) run again each time a level opens - e.g. the "
                      "two-player side-by-side split, which the game resets when a level starts");
namespace {
std::atomic<bool> g_in_front_end{false};
std::atomic<uint32_t> g_split_level_serial{0};  // GitHub #269: bumped at every level / front-end open
bool Gow3NameHas(std::string_view s, std::string_view needle) {
  for (size_t i = 0; i + needle.size() <= s.size(); ++i) {
    size_t k = 0;
    while (k < needle.size() && std::tolower(static_cast<unsigned char>(s[i + k])) == needle[k]) ++k;
    if (k == needle.size()) return true;
  }
  return false;
}
// GitHub #157: versus, Horde and Beast maps are MP_<map>.xxx, without the campaign's _P - the cap stayed at 60 there.
bool Gow3IsLevel(std::string_view path) {
  if (Gow3NameHas(path, "_p.xxx")) return true;
  const size_t slash = path.find_last_of("\\/");
  const std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
  return base.size() > 3 && std::tolower(static_cast<unsigned char>(base[0])) == 'm' &&
         std::tolower(static_cast<unsigned char>(base[1])) == 'p' && base[2] == '_' && !Gow3NameHas(base, "_loc_");
}
void OnGow3FileOpened(std::string_view path, const std::filesystem::path*) {
  if (!Gow3NameHas(path, ".xxx")) return;
  const bool front = Gow3NameHas(path, "gearstart");
  if (front) {
    if (!g_in_front_end.exchange(true)) REXLOG_INFO("gow3_menu_fps: front end ({}) - limit {} fps", path, REXCVAR_GET(gow3_menu_fps));
  } else if (Gow3IsLevel(path) && g_in_front_end.exchange(false)) {
    REXLOG_INFO("gow3_menu_fps: level {} - the player's limit again", path);
  }
  // GitHub #208 / #136: commands run again at every level (the split-screen layout: the game sets its own when a
  // level starts, so --ue3_exec's one run at the front end was overwritten). Queued; run once a player exists.
  if (Gow3IsLevel(path) || front) {
    g_split_level_serial.fetch_add(1, std::memory_order_relaxed);  // GitHub #269: Gow3SplitLayoutTick
    const std::string each = REXCVAR_GET(ue3_exec_each_level);
    if (!each.empty()) {
      size_t at = 0;
      while (at <= each.size()) {
        const size_t end = std::min(each.find(';', at), each.size());
        if (end > at) Ue3ConsoleQueue(each.substr(at, end - at));
        at = end + 1;
      }
    }
  }
}
}  // namespace
void Ue3ShaderPrebuildChainFileOpened(void (*listener)(std::string_view, const std::filesystem::path*));  // shader_prebuild.cpp

// GitHub #193 (DLC not found after 0.4.5): every file the game opens, and (SDK patch 52) every open that fails, with
// the host file it came from - so a run shows whether the DLC packages / their config are looked up after the first
// map, and with what result. Content enumeration itself (XamContentCreateEnumerator) is in the SDK and is not seen here.
REXCVAR_DEFINE_INT32(gow3_log_file_opens, 0, "Gears3",
                     "Diagnostic (GitHub #193): log every file open (and failed open) with its host file, up to N lines. "
                     "0 = off");
namespace {
std::atomic<int> g_open_lines{0};
void OnGow3FileOpenLogged(std::string_view path, const std::filesystem::path* host) {
  const int max = REXCVAR_GET(gow3_log_file_opens);
  if (g_open_lines.load(std::memory_order_relaxed) >= max) return;
  const int n = g_open_lines.fetch_add(1);
  if (n >= max) return;
  REXLOG_INFO("gow3_log_file_opens: opened {} -> {}{}", path, host ? rex::path_to_utf8(*host) : std::string("(no host file)"),
              n + 1 == max ? " - last line (gow3_log_file_opens)" : "");
}
#if defined(REX_FILESYSTEM_FILE_OPEN_FAILED_OBSERVER)
void OnGow3FileOpenFailedLogged(std::string_view path) {
  const int max = REXCVAR_GET(gow3_log_file_opens);
  if (g_open_lines.load(std::memory_order_relaxed) >= max) return;
  const int n = g_open_lines.fetch_add(1);
  if (n >= max) return;
  REXLOG_INFO("gow3_log_file_opens: NOT FOUND {}{}", path, n + 1 == max ? " - last line (gow3_log_file_opens)" : "");
}
#endif
void Gow3InstallFileOpenLog() {
  if (REXCVAR_GET(gow3_log_file_opens) <= 0) return;
#if defined(REX_FILESYSTEM_FILE_OPEN_OBSERVER)
  Ue3ShaderPrebuildChainFileOpened(&OnGow3FileOpenLogged);
#if defined(REX_FILESYSTEM_FILE_OPEN_FAILED_OBSERVER)
  rex::filesystem::SetFileOpenFailedObserver(&OnGow3FileOpenFailedLogged);
  REXLOG_INFO("gow3_log_file_opens: on - opens and failed opens, up to {} lines", REXCVAR_GET(gow3_log_file_opens));
#else
  REXLOG_INFO("gow3_log_file_opens: on - opens only (this SDK has no failed-open observer, patch 52), up to {} lines",
              REXCVAR_GET(gow3_log_file_opens));
#endif
#else
  REXLOG_WARN("gow3_log_file_opens: this SDK has no file-open observer (SDK patch 32)");
#endif
}
}  // namespace

void Gow3InstallMenuCap() {
  Gow3InstallFileOpenLog();  // GitHub #193 (off by default)
  if (REXCVAR_GET(gow3_menu_fps) <= 0) return;
#if defined(REX_FILESYSTEM_FILE_OPEN_OBSERVER)
  Ue3ShaderPrebuildChainFileOpened(&OnGow3FileOpened);
  REXLOG_INFO("gow3_menu_fps: on - the front end runs at {} fps at most (GitHub #101)", REXCVAR_GET(gow3_menu_fps));
#else
  REXLOG_WARN("gow3_menu_fps: this SDK has no file-open observer (SDK patch 32); menus keep the player's limit");
#endif
}

// GitHub #55 #68 #79 #80 #120 (Shagg's 21:04 trace, uncapped): during checkpoint saves and streaming the engine ticked at
// 0.35-0.6 ms (2000+ ticks/s) for seconds at a time - with no engine limit (--gow3_max_fps=-1) only the presents pace the
// loop, and in those windows nothing is presented. Capped runs (1039 c, 30 fps) show no such bursts: the engine's own
// limiter (this function's value) is asked every tick, in those windows too. --gow3_tick_floor_fps puts a ceiling under
// "no limit": the engine never ticks faster than N per second, so a tick's delta never drops below 1/N s.
REXCVAR_DEFINE_INT32(gow3_tick_floor_fps, 0, "Gears3",
                     "With no frame-rate limit: the engine still ticks at most N times a second (no sub-millisecond ticks "
                     "while nothing is presented, e.g. checkpoint saves). 0 = off");
void Gow3CheckpointStreamTick();        // checkpoint_stream.cpp (GitHub #226)
void Gow3InstallCheckpointStreamWait();  // checkpoint_stream.cpp
void Gow3ModChaptersTick();  // mod_chapters.cpp: mods' chapters and content packages, on the game thread
void Gow3AdaptiveTriggersTick(bool in_front_end);  // adaptive_triggers.cpp (GitHub #245, --pad_adaptive_triggers /
                                                   // --pad_trigger_rumble)
void Gow3FirstPersonTick();  // first_person.cpp (--gow3_first_person)

// Gears 3's path choice (UIScene_PathChoice: "left path / right path") reads the triggers - the left trigger picks the
// left path - while the mouse's buttons are bound the other way round for play (keybind_right_trigger = LMB: fire,
// keybind_left_trigger = RMB: aim), so the left button chose the right path, against the on-screen prompts. While the
// scene is open and takes input the two trigger binds are swapped, and put back as they were when it closes (the
// player's own binds, whatever they are). A pad is not affected. From KiltedKernel's Gears 3 port (hid.cpp).
// 0.4.6 (Shagg 11-06): off by default - the fresh 0.4.6 package still froze at the main menu with it on (job 1294);
// it comes back on once its object scan no longer stalls the game thread.
// (KiltedKernel's MR !3 turned it on by default; 0.6.0 review: kept off until a game without mods is seen to reach the
// main menu and a level with it on - the 0.4.6 freeze was this scan. His UIScene_PathChoiceContent watch stays, so it
// works when switched on; developer mode's "choose L|R" works either way.)
// 0.6.0: on again by default - the freeze is gone (the class lookup retries every 5 s and the instance scan is spread
// over calls, 16384 slots each), and with it off the left mouse button picked the right path again in the campaign
REXCVAR_DEFINE_BOOL(gow3_path_choice_mouse, true, "Gears3",
                    "Path choice: the mouse button on a path's side picks that path (the trigger binds swap while it is open)");
// GitHub #269 (was #136 / #208): two-player split side by side. One `set GameViewportClient DesiredSplitscreenType
// eSST_2P_VERTICAL` when a level package OPENS (--ue3_exec_each_level) ran during the load, before the level's own
// start-up - which sets the viewport client's split type again, so the game still split top / bottom (the reporter's
// in-game Back binding, run after the level started, did work). With --gow3_split_vertical the viewport client is
// watched instead: whenever two players are signed in and its DesiredSplitscreenType is not eSST_2P_VERTICAL, the same
// console command is run again (rate-limited, with a longer wait after each quick reset by the game, 30 s at most).
// 3 and 4 players and one player are left alone. Off by default (the launcher's "Two-player split: Side by side").
REXCVAR_DEFINE_BOOL(gow3_split_vertical, false, "Gears3",
                    "GitHub #269: the two-player split side by side (eSST_2P_VERTICAL), set again whenever the game "
                    "resets it (each level start)");
void Gow3SplitLayoutTick() {
  if (!REXCVAR_GET(gow3_split_vertical)) return;
  static uint32_t ticks = 0;
  if (++ticks % 20) return;  // three times a second at 60
  namespace obj = ue3::obj;
  if (!obj::Ready() && !obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar)) return;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next_lookup{}, last_set{};
  static int vp_off = -1, desired_off = -1, active_off = -1, players_off = -1;
  static uint32_t serial_seen = ~0u;
  static int fails = 0;
  static Clock::time_point vertical_since{};
  static bool in_flight = false, warned = false;
  const Clock::time_point now = Clock::now();
  if (vp_off < 0 || desired_off < 0 || players_off < 0) {
    if (now < next_lookup) return;
    next_lookup = now + std::chrono::seconds(5);   // FindClass misses scan every object: not more often than this
    const uint32_t engine_cls = obj::FindClass("Engine"), game_engine_cls = obj::FindClass("GameEngine");
    const uint32_t vpc_cls = obj::FindClass("GameViewportClient");
    if (!engine_cls || !game_engine_cls || !vpc_cls) return;
    vp_off = obj::PropertyOffset(game_engine_cls, "GameViewport");
    players_off = obj::PropertyOffset(engine_cls, "GamePlayers");
    desired_off = obj::PropertyOffset(vpc_cls, "DesiredSplitscreenType");
    active_off = obj::PropertyOffset(vpc_cls, "ActiveSplitscreenType");
    REXLOG_INFO("gow3_split_vertical: GameViewport +0x{:X}, GamePlayers +0x{:X}, DesiredSplitscreenType +0x{:X}, "
                "ActiveSplitscreenType +0x{:X}", vp_off, players_off, desired_off, active_off);
    if (vp_off < 0 || desired_off < 0 || players_off < 0) {
      REXLOG_WARN("gow3_split_vertical: viewport properties not found - the split is left as the game sets it");
      vp_off = desired_off = players_off = -1;
      next_lookup = now + std::chrono::hours(24 * 365);
      return;
    }
  }
  const uint32_t engine = obj::Read32(gow3::addr::kGEngine);
  if (!engine) return;
  const uint32_t viewport = obj::Read32(engine + uint32_t(vp_off));
  const uint32_t players = obj::Read32(engine + uint32_t(players_off) + 4);  // TArray: data, count, max
  if (!viewport || players != 2) {  // not two players: nothing to keep; a later start counts afresh
    vertical_since = {};
    in_flight = false;
    return;
  }
  auto read8 = [](uint32_t a) { return uint8_t(obj::Read32(a & ~3u) >> (8 * (3 - (a & 3u)))); };
  // ESplitScreenType (Engine.GameViewportClient): eSST_NONE, eSST_2P_HORIZONTAL, eSST_2P_VERTICAL, eSST_3P_*, eSST_4P
  constexpr uint8_t kVertical = 2;
  const uint8_t desired = read8(viewport + uint32_t(desired_off));
  const uint8_t active = active_off >= 0 ? read8(viewport + uint32_t(active_off)) : 0xFF;
  static uint8_t logged_desired = 0xFF, logged_active = 0xFF;
  static int split_lines = 0;   // r36: every split log line is capped (a game that keeps resetting it cannot flood the log)
  if ((desired != logged_desired || active != logged_active) && ++split_lines <= 300) {
    REXLOG_INFO("gow3_split_vertical: split type desired {} active {} (2 = side by side)", desired, active);
    logged_desired = desired;
    logged_active = active;
  }
  // 0.6.2 (GitHub #313): the layout is kept, not given up on. The first version of this watch counted every check within 2 s
  // of a set as a "quick revert" while the layout was still not side by side - one set that took longer than that to
  // run (the console queue waits for a player; a slow level start-up), or one reset by the game's own level start-up,
  // gave the level up for good and the split went back to top / bottom ("worked only for a brief moment"). Now:
  //  - a set is "in flight" until the layout is seen side by side; a set that does not show within 3 s was lost or
  //    overwritten and is made again;
  //  - a "quick revert" is only counted when the layout WAS seen side by side and the game turned it back within 10 s;
  //  - after quick reverts (or lost sets) the wait before the next set doubles (2, 4, 8, 16, then 30 s at most), so the
  //    game is never fought every frame, but it is never left alone for the rest of the level either; the count starts
  //    again after 10 s of a layout that held, or at the next level.
  const uint32_t serial = g_split_level_serial.load(std::memory_order_relaxed);
  if (serial != serial_seen) {
    serial_seen = serial;
    fails = 0; in_flight = false; warned = false; vertical_since = {}; last_set = {};
  }
  if (desired == kVertical) {
    if (vertical_since == Clock::time_point{}) vertical_since = now;
    if (in_flight) {
      in_flight = false;
      if (++split_lines <= 300) REXLOG_INFO("gow3_split_vertical: side by side after {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(now - last_set).count());
    }
    if (fails && now - vertical_since >= std::chrono::seconds(10)) fails = 0;  // it holds
    return;
  }
  if (vertical_since != Clock::time_point{}) {  // it was side by side a moment ago: the game turned it back
    const auto held = now - vertical_since;
    vertical_since = {};
    // r36: a reset within 10 s of holding counts (a game that resets the split every few seconds backs this off too;
    // a layout that held longer was changed for a reason of the game's own and is simply set again)
    if (held < std::chrono::seconds(10)) {
      ++fails;
      if (++split_lines <= 300) REXLOG_INFO("gow3_split_vertical: the game turned the split back after {} ms ({} quick reset(s) in a row)",
                  std::chrono::duration_cast<std::chrono::milliseconds>(held).count(), fails);
    }
  }
  if (in_flight && now - last_set < std::chrono::seconds(3)) return;  // the command has not run yet
  if (in_flight) { in_flight = false; ++fails; }                       // it never showed: lost or overwritten
  if (fails >= 5 && !warned) {
    warned = true;
    REXLOG_WARN("gow3_split_vertical: the game keeps resetting the split here - setting it again only every 30 s");
  }
  const auto gap = std::chrono::seconds(fails <= 0 ? 0 : std::min(30, 1 << std::min(fails, 5)));
  if (last_set != Clock::time_point{} && now - last_set < gap) return;
  last_set = now;
  in_flight = true;
  Ue3ConsoleQueue("set GameViewportClient DesiredSplitscreenType eSST_2P_VERTICAL");
}

void Gow3PathChoiceTick() {
  // (also while developer mode's scripted controller waits for a path choice: "choose L|R", ue3/script_input.h)
  const bool mouse = REXCVAR_GET(gow3_path_choice_mouse);
  if (!mouse && !Ue3ScriptInputWantsPathChoice()) return;
  static uint32_t ticks = 0;
  if (++ticks % 15) return;  // four times a second at 60: the scene stays open for seconds
  namespace obj = ue3::obj;
  if (!obj::Ready() && !obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar)) return;
  static uint32_t scene = 0;
  static int flags = -1;
  static uint32_t open_mask = 0, input_mask = 0;
  // 0.4.6 freeze (Shagg 11-06: Gears 3 stopped at the menu): FindClass caches hits only, so while
  // UIScene_PathChoice is not loaded (the menus) every call scanned all ~168,000 objects through checked reads -
  // four times a second, which stalled the game thread to a standstill (so did the full FindInstancesOf per call).
  // A miss was then retried every 5 s (a full scan, still a hitch); 0.6.2 spreads the class search over calls as well,
  // and the instances are found by a scan spread over calls (below).
  static std::vector<uint32_t> instances;
  // the campaign's scene is UIScene_PathChoiceContent (GearGameContent), which does not derive from GearGame's
  // UIScene_PathChoice: watching only the latter, the swap never came (2026-11-11: left mouse picked the right path)
  static uint32_t content = 0;
  // 0.6.2 (0.6.1 review of #277): a class that is not loaded yet is looked for 16384 table slots per call, like the
  // instances below, not by a full scan of all ~168,000 objects every 5 s (three of them per try: a hitch each time).
  static uint32_t class_cursor = 0, movie = 0;
  const auto class_step = [&](const char* name) {
    uint32_t total = 0;
    const uint32_t c = obj::FindClassRange(name, class_cursor, 16384, &total);
    return std::make_pair(c, total);
  };
  if (!content && scene) {
    const auto [c, total] = class_step("UIScene_PathChoiceContent");
    content = c;
    class_cursor = c || class_cursor + 16384 >= total ? 0 : class_cursor + 16384;
  }
  if (!scene) {
    uint32_t total = 0;
    if (!movie) std::tie(movie, total) = class_step("GFxMoviePlayer");
    if (!scene) { const auto r = class_step("UIScene_PathChoice"); scene = r.first; total = std::max(total, r.second); }
    if (!content) { const auto r = class_step("UIScene_PathChoiceContent"); content = r.first; total = std::max(total, r.second); }
    class_cursor = class_cursor + 16384 >= total ? 0 : class_cursor + 16384;
    if (!movie || !scene) { scene = 0; return; }
    class_cursor = 0;
    flags = obj::PropertyOffset(movie, "bMovieIsOpen");
    open_mask = obj::BoolMask(movie, "bMovieIsOpen");
    input_mask = obj::BoolMask(movie, "bAllowInput");
    if (flags < 0 || !open_mask) { REXLOG_WARN("path choice: GFxMoviePlayer flags not found - mouse buttons left as bound"); return; }
  }
  if (flags < 0) return;
  // review: no full scan at all - 16384 table slots per call (a pass over ~168k slots in ~2.5 s), plus the newest 2048
  // slots every call, where an opening scene's new instance usually lands (so the swap is not seconds late)
  {
    static uint32_t cursor = 0;
    static std::vector<uint32_t> building;
    uint32_t total = 0;
    for (uint32_t cls : {scene, content}) {
      if (!cls) continue;
      for (uint32_t m : obj::FindInstancesOfRange(cls, cursor, 16384, &total)) building.push_back(m);
      if (total > 2048)
        for (uint32_t m : obj::FindInstancesOfRange(cls, total - 2048, 2048, nullptr))
          if (std::find(instances.begin(), instances.end(), m) == instances.end()) instances.push_back(m);
    }
    cursor += 16384;
    if (cursor >= total) { instances.swap(building); building.clear(); cursor = 0; }
  }
  bool open = false;
  for (uint32_t m : instances) {
    if (!obj::IsA(m, scene) && !(content && obj::IsA(m, content))) continue;  // review L1: a freed / reused slot
    const uint32_t f = obj::Read32(m + uint32_t(flags));
    if ((f & open_mask) && (!input_mask || (f & input_mask))) open = true;
  }
  Ue3ScriptInputPathChoiceOpen(open);
  static bool swapped = false;
  static std::string left, right;
  if (!mouse || open == swapped) return;
  if (open) {
    left = rex::cvar::GetFlagByName("keybind_left_trigger");
    right = rex::cvar::GetFlagByName("keybind_right_trigger");
    rex::cvar::SetFlagByName("keybind_left_trigger", right);
    rex::cvar::SetFlagByName("keybind_right_trigger", left);
  } else {
    rex::cvar::SetFlagByName("keybind_left_trigger", left);
    rex::cvar::SetFlagByName("keybind_right_trigger", right);
  }
  swapped = open;
  REXLOG_INFO("path choice: {} (left path: {}, right path: {})", open ? "open, trigger binds swapped" : "closed, binds restored",
              open ? right : left, open ? left : right);
}

// Exact cap (GitHub #225 follow-up: 121-123 at a 120 cap): the engine's own wait sleeps whole milliseconds, so a limit
// of N gave N +- 2. With a player's limit the game thread waits here on the host clock (shared precise limiter) and the
// engine is told "no limit" - it measured the time before this call, so its own wait would come on top.
// 0.6.3: wait = false for UWorld::ServerTickClients' call (host with remote players): it only reads the value, and a
// second wait in the same frame halved the player's limit (60 -> 30, 144 -> 72). It gets 0 ("no limit"), exactly what
// it got in 0.6.2 after the wait (review r62 M1: the limit itself would change its CPU-saturated test).
// GitHub #313 pace check (xenos::PaceCheck): the engine loop's limiter wait in this frame, ms (game thread only; set to 0
// by Gow3MaxTickRate for each engine-loop call, read by Gow3NoteTickCall after the original returned).
static double g_frame_wait_ms = 0.0;
static void Gow3ExactLimit(PPCRegister& f1, bool wait = true) {
  if (!wait) { f1.f64 = 0.0; return; }
  static xenos::FrameLimiter limiter;
  GOW3_LOG_ONCE("gow3_max_fps: exact host-side limiter on ({} fps)", f1.f64);
  g_frame_wait_ms = limiter.Wait(f1.f64);
  f1.f64 = 0.0;
}

// GitHub #313 ("the fps limit stops working in split screen": Gears 2 and 3, a 120 limit, vsync on or off; not
// reproduced). 0.6.3: the smoothing flag GetMaxTickRate tests (disc 0x8343F7D4, TU6 0x8344FFA4) is written once, at
// start-up (disc sub_82960BE8), so the count below should stay 0 in split screen too; xenos::PaceCheck (every 10 s with
// 2+ local players: engine frames, frames shown, limiter waits) tells the other cases apart. The limiter lives in Gow3MaxTickRate, which runs only where UEngine::GetMaxTickRate loads MaxSmoothedFrameRate (frame-rate
// smoothing on); extras.cpp wraps the whole function. If the game takes another way through it while two players are in
// (no smoothing -> no hook -> no limit), the two counts below differ. These lines show it in a report: how many engine
// ticks did not reach the limiter, what the function returned for them, the limit asked, and the number of local players.
std::atomic<uint32_t> g_tick_hook_hits{0};
// 0.6.3 host frame-rate halving: the guest return address of the GetMaxTickRate call in progress (set by extras.cpp's
// wrapper before the original runs; game thread). The ServerTickClients one (gow3_addresses.h) gets no wait / ticks.
std::atomic<uint32_t> g_tick_caller{0};
namespace {
// Local players (Engine.GamePlayers count) or -1. Read-only; class / property lookups are cached and retried 5 s apart.
int Gow3LocalPlayers() {
  namespace obj = ue3::obj;
  if (!obj::Ready()) return -1;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next_lookup{};
  static int players_off = -1;
  if (players_off < 0) {
    const Clock::time_point now = Clock::now();
    if (now < next_lookup) return -1;
    next_lookup = now + std::chrono::seconds(5);
    const uint32_t engine_cls = obj::FindClass("Engine");
    if (!engine_cls) return -1;
    players_off = obj::PropertyOffset(engine_cls, "GamePlayers");
    if (players_off < 0) return -1;
  }
  const uint32_t engine = obj::Read32(gow3::addr::kGEngine);
  return engine ? int(obj::Read32(engine + uint32_t(players_off) + 4)) : -1;  // TArray: data, count, max
}
}  // namespace
// Game thread, from extras.cpp's GetMaxTickRate wrapper once the original has returned: hits_before = g_tick_hook_hits
// when the call began, result = the value it returned (0 = no limit left for the engine's own wait).
void Gow3NoteTickCall(uint32_t hits_before, double result, uint32_t caller) {
  static xenos::TickCallSites sites;
  const bool server = caller == gow3::addr::kServerTickClientsTickRateRet;
  sites.Note("gow3_max_fps", caller, !server);
  if (server) return;  // not an engine tick: the counts below are the engine loop's
  static uint64_t calls = 0, skipped = 0, window_calls = 0, window_skipped = 0, flips = 0;
  static int last_players = -2;
  static bool last_hooked = true;
  const bool hooked = g_tick_hook_hits.load(std::memory_order_relaxed) != hits_before;
  ++calls, ++window_calls;
  if (!hooked) ++skipped, ++window_skipped;
  const int32_t asked = LiveMaxFps();
  static xenos::PaceCheck pace;  // GitHub #313: engine frames / frames shown / limiter waits, every 10 s in split screen
  pace.Tick("gow3_max_fps", asked, hooked ? g_frame_wait_ms : -1.0, XenosSwapCount(), [] { return Gow3LocalPlayers(); });
  // a change between "reached the limiter" and "did not", each way: the first 12 only
  if (hooked != last_hooked && flips < 12) {
    ++flips;
    REXLOG_INFO("gow3_max_fps: engine tick {}: GetMaxTickRate {} the limiter hook (returned {:.1f}; limit asked {}; local players {})",
                calls, hooked ? "reaches" : "did NOT reach", result, asked, Gow3LocalPlayers());
  }
  last_hooked = hooked;
  if (calls % 60 == 0) {  // a change in the number of local players, with the limit that is in force
    const int players = Gow3LocalPlayers();
    if (players != last_players && players >= 0) {
      last_players = players;
      REXLOG_INFO("gow3_max_fps: local players {} (limit asked {}, front end {}, last tick returned {:.1f})", players, asked,
                  g_in_front_end.load(std::memory_order_relaxed), result);
    }
  }
  if (window_calls >= 1800) {  // every 1800 ticks (30 s at 60): only when some did not reach the hook
    static int summaries = 0;   // r36: capped (smoothing off = every window skips)
    if (window_skipped && ++summaries <= 60)
      REXLOG_INFO("gow3_max_fps: {} of the last {} engine ticks did not reach the limiter hook ({} of {} so far)", window_skipped,
                  window_calls, skipped, calls);
    window_calls = window_skipped = 0;
  }
}

void Gow3MaxTickRate(PPCRegister& f1) {  // after `lfs f1,0x4C8(r3)` at 0x8253D9B0
  g_tick_hook_hits.fetch_add(1, std::memory_order_relaxed);
  // 0.6.3: UWorld::ServerTickClients' call (a host with remote players, every frame after the engine loop's): the same
  // value, no wait, and none of the per-frame ticks below (they ran twice per frame on a host before).
  const bool engine_loop = g_tick_caller.load(std::memory_order_relaxed) != gow3::addr::kServerTickClientsTickRateRet;
  if (engine_loop) {
    g_frame_wait_ms = 0.0;  // GitHub #313 pace check: set by Gow3ExactLimit when it waits in this frame
    TickTimeline();
    Gow3ModChaptersTick();
    Gow3PathChoiceTick();
    Gow3SplitLayoutTick();       // GitHub #269
    Gow3NativeUiViewportTick();  // 0.6.6 true 1080p: menus in the larger screen (off: one test)
    Gow3CheckpointStreamTick();  // GitHub #226
    Gow3AdaptiveTriggersTick(g_in_front_end.load(std::memory_order_relaxed));  // GitHub #245 (off: one flag test)
    Gow3FirstPersonTick();       // --gow3_first_person (off: one flag test)
    // GitHub #353: COGBox tags after the players' names.
    // No colour for now (Flippage / Shagg): the grey markup showed as text wherever the game draws a name as plain
    // text (kill feed, names over heads, TacCom), and not every such place is found yet - plain on every screen.
    Ue3NameTagsTick([] { return ue3::obj::Ready() || ue3::obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar); },
                    Ue3NameTagStyle{});
  }
  const int32_t v = LiveMaxFps();
  if (const int32_t m = REXCVAR_GET(gow3_menu_fps); m > 0 && g_in_front_end.load(std::memory_order_relaxed)) {
    const double own = v < 0 ? 0.0 : (v > 0 ? double(v) : f1.f64);  // the limit that would apply (0 = none)
    f1.f64 = (own <= 0.0 || own > double(m)) ? double(m) : own;
    if (v != 0) Gow3ExactLimit(f1, engine_loop);
    return;
  }
  const int32_t floor_fps = REXCVAR_GET(gow3_tick_floor_fps);
  if (!v) {
    if (floor_fps > 0 && !(f1.f64 > 0.0)) f1.f64 = double(floor_fps);  // the game's own "none" (0) gets the floor too
    return;
  }
  GOW3_LOG_ONCE("gow3_max_fps: engine frame-rate limit {} -> {}{}", f1.f64, v < 0 ? std::string("none") : std::to_string(v),
                v < 0 && floor_fps > 0 ? " (tick floor " + std::to_string(floor_fps) + "/s)" : std::string());
  f1.f64 = v < 0 ? (floor_fps > 0 ? double(floor_fps) : 0.0) : double(v);
  if (v > 0) Gow3ExactLimit(f1, engine_loop);
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
REXCVAR_DEFINE_BOOL(cogbox_test_skip_movies, false, "COGBox",
                    "TEST ONLY (scripted test runs): no full-screen movies - the engine's own [FullScreenMovie] "
                    "bForceNoMovies, so the logos, the loading movie and every pre-rendered (.bik) cutscene are passed as "
                    "if skipped and a run reaches gameplay without button presses. In-engine (real-time) cutscenes still "
                    "play. Not for players: loading screens go black.");
void Gow3AddBuiltInIniEdits() {
  Gow3InstallMenuCap();  // GitHub #101 (once, at start-up, before the game opens a file)
  Gow3InstallCheckpointStreamWait();  // GitHub #226
  std::string add;
  if (REXCVAR_GET(cogbox_test_skip_movies)) add += "Xbox360-GearEngine.ini|FullScreenMovie|bForceNoMovies=True;";
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
// GitHub #220 (as Gears 2's #132): the camera's view offset counts a viewport as widescreen only within 16:9 +- 0.01
// (.rdata pair 1.7877778 / 1.7677778), so every ultrawide shape got the 4:3 offset - the camera sits higher. With a
// wider picture the upper bound becomes the shape's aspect + 0.05. Found by value in .rdata, written only if found once.
void Gow3UltrawideCameraViewportType(rex::Runtime* runtime, float aspect) {
  const uint8_t* image = runtime->virtual_membase() + 0x82000000u;
  uint32_t lo = 0, hi = 0;
  if (image[0] == 'M' && image[1] == 'Z') {
    uint32_t pe = 0;
    std::memcpy(&pe, image + 0x3C, 4);
    if (pe > 0 && pe < 0x1000 && std::memcmp(image + pe, "PE\0\0", 4) == 0) {
      uint16_t sections = 0, opt = 0;
      std::memcpy(&sections, image + pe + 6, 2);
      std::memcpy(&opt, image + pe + 20, 2);
      for (uint32_t i = 0; i < sections && i < 32; ++i) {
        const uint8_t* sec = image + pe + 24 + opt + 40 * i;
        if (std::memcmp(sec, ".rdata", 7) == 0) {
          uint32_t vsize = 0, va = 0;
          std::memcpy(&vsize, sec + 8, 4);
          std::memcpy(&va, sec + 12, 4);
          lo = va, hi = va + vsize;
        }
      }
    }
  }
  // Gears 3 keeps them the other way round from Gears 2: 1.7677778f then 1.7877778f (TU6 820CE108 / 820CE10C, job 1336 probe).
  static const uint8_t kPair[8] = {0x3F, 0xE2, 0x46, 0x8B, 0x3F, 0xE4, 0xD5, 0xE7};  // 1.7677778f, 1.7877778f (BE)
  uint32_t found = 0, count = 0;
  for (uint32_t o = lo; hi > lo && hi < 0x02000000u && o + 8 <= hi; o += 4)
    if (std::memcmp(image + o, kPair, 8) == 0) found = 0x82000000u + o, ++count;
  if (count != 1) {
    REXLOG_INFO("gow3_aspect: camera viewport bounds not found ({} copies in .rdata {:08X}-{:08X}) - left alone "
                "(GitHub #220)", count, 0x82000000u + lo, 0x82000000u + hi);
    // GitHub #220 (0.4.7: not found in Gears 3): log where 16:9-ish floats sit in the whole image, to find Gears 3's bounds.
    uint32_t pe = 0, size = 0;
    std::memcpy(&pe, image + 0x3C, 4);
    if (pe > 0 && pe < 0x1000) std::memcpy(&size, image + pe + 24 + 56, 4);
    if (size > 0x02000000u) size = 0x02000000u;
    static const float kProbe[] = {1.7677778f, 1.7877778f, 1.7777778f, 1.7f, 1.76f, 1.77f, 1.78f, 1.8f, 1.5f, 1.3333334f};
    for (float v : kProbe) {
      uint32_t bits = 0;
      std::memcpy(&bits, &v, 4);
      const uint8_t be[4] = {uint8_t(bits >> 24), uint8_t(bits >> 16), uint8_t(bits >> 8), uint8_t(bits)};
      std::string hits;
      uint32_t n = 0;
      for (uint32_t o = 0; o + 4 <= size; o += 4)
        if (std::memcmp(image + o, be, 4) == 0 && ++n <= 12) {
          char b[12];
          std::snprintf(b, sizeof b, " %08X", 0x82000000u + o);
          hits += b;
        }
      REXLOG_INFO("gow3_aspect: probe {:.7f}: {} hit(s){}", v, n, hits);
    }
    return;
  }
  WriteGuestFloat(runtime, found + 4, 1.7877778f, aspect + 0.05f,
                  "gow3_aspect: camera widescreen upper bound (GitHub #220: no 4:3 camera offset)");
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
    const float aspect = float(1280.0 / double(WideHeight()));  // 0.6.3: the exact shape (Canary's 2.3272 = 1280 / 550)
    WriteGuestFloat(runtime, gow3::addr::kWidescreenAspect, gow3::addr::kWidescreenAspectValue, aspect,
                    "gow3_aspect: widescreen aspect");
    Gow3UltrawideCameraViewportType(runtime, aspect);
    const bool set = rex::cvar::SetFlagByName("gow2_present_frame_aspect", "true");
    REXLOG_INFO("gow3_aspect: present at the front buffer's own shape {}", set);
  }
}

#if defined(GOW3_TU6) && GOW3_TU6  // the TU6 build's addresses; other builds draw the kill feed with the markup
// GitHub #353, the kill feed: the HUD keeps copies of the names in its DeathData entries and draws them through
// Canvas's text calls, which draw the text as it is - so the tag's grey markup (rendered by the Flash scenes: lobby,
// scoreboard) would show as text. These are the
// engine's text routines under Canvas.DrawText / TextSize / StrLen (exec functions 82749400, 82749AE8, 82749850 call them with the
// script's copy of the text): the original gets a copy, without the markup, of any argument holding it - the text itself, or an FString whose
// data it is - before the original runs.
namespace {
// Each text argument through Ue3NameTagsTextArg: a scratch copy without the tag's markup in place of text holding it
// (the caller's text is never changed); a register is rewritten only when its value is replaced.
void CleanTextArgs(PPCContext& ctx) {
  for (PPCRegister* r : {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10})
    if (const uint32_t v = Ue3NameTagsTextArg(r->u32); v != r->u32) r->u64 = v;
}
// The hooks act only once these Canvas natives are found at these addresses (this build's; checked at start-up).
constexpr Ue3TextHookCheck kTextHookChecks[] = {{"DrawText", 0x82749400}, {"TextSize", 0x82749AE8}, {"StrLen", 0x82749850}};
const bool kTextHookChecksSet = (Ue3NameTagsTextHookChecks(kTextHookChecks, std::size(kTextHookChecks)), true);
}  // namespace
#define TEXT_HOOK(addr)                                                             \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);     \
  extern "C" void sub_##addr(PPCContext& __restrict ctx, uint8_t* base) {           \
    CleanTextArgs(ctx);                                                             \
    __imp__sub_##addr(ctx, base);                                                   \
  }
TEXT_HOOK(82746EE0)  // DrawText's
TEXT_HOOK(82748790)  // DrawText's
TEXT_HOOK(82747930)  // TextSize's
TEXT_HOOK(827486C0)  // StrLen's
#undef TEXT_HOOK
// The Canvas exec natives themselves: the hooks above act only inside one of these (the text routines are shared by
// much of the engine; only the script's draw calls hold the copy that is cleaned) - review r368b.
#define CANVAS_EXEC(addr)                                                                  \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);    \
  extern "C" void sub_##addr(PPCContext& __restrict ctx, uint8_t* base) {          \
    Ue3CanvasTextCall in_canvas;                                                     \
    __imp__sub_##addr(ctx, base);                                                    \
  }
CANVAS_EXEC(82749400)  // Canvas.DrawText
CANVAS_EXEC(82749AE8)  // Canvas.TextSize
CANVAS_EXEC(82749850)  // Canvas.StrLen
#undef CANVAS_EXEC
#endif
