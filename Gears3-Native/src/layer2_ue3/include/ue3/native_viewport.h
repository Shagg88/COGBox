// Layer 2 (Unreal Engine 3 on the 360): true 1080p (--gowN_native_lines) - the game's own FViewport reports the larger
// screen. This header has a copy in each game's src/layer2_ue3/include/ue3 - keep the four identical. Our code (MIT).
//
// Why (job 2189): with --gowN_native_lines=1080 the game draws 1920x1080 itself (the size function's 720 / 1280 are
// changed), but its FViewport - the one object every UI system asks for "the screen's size" through its virtual
// GetSizeX / GetSizeY (FRenderTarget vtable slots 2 / 3, +8 / +12) - keeps the console's 1280x720. So the menus (UE3 UI
// scenes: UGameUISceneClient::GetViewportSize reads RenderViewport->GetSizeX/Y; Scaleform movies: the GFx engine sets
// each movie's GViewport from its Viewport->GetSizeX/Y; UGameViewportClient::GetViewportSize) lay themselves out in
// 1280x720 and sit in the top-left of the 1920x1080 picture. Read in the generated code:
//   Gears 2 TU6 sub_826E9B90 (GetViewportSize): lwz r11,64(r31) / lwz r10,8(r11) / bctrl ... lwz r7,12(r8) / bctrl;
//   Gears 3 TU6 sub_826266B8 (UGameViewportClient::GetViewportSize, the script native sub_8252C2A0 calls it):
//     lwz r11,68(r3) / lwz r10,8(r11) / bctrl ... lwz r7,12(r8) / bctrl;
//   Gears 3 TU6 sub_829343E0 / sub_82934188 (the GFx movies' viewport): li r30,1280 / li r9,720 as the default, then
//     lwz r11,72(r3) / lwz r10,8(r11) / bctrl / ... / lwz r7,12(r8) / bctrl, the size passed on to the movie.
// Every one of those calls is a virtual call (bctrl), so it goes through the function dispatcher: the two getters the
// viewport's vtable names are replaced (FunctionDispatcher::SetFunction, as ue3_mouse_look.h does) by wrappers that
// run the game's getter and, only for this one viewport object, return the size times lines / 720. Other objects that
// share the same getter code get the game's value unchanged. Direct (non-virtual) reads of the size are not changed.
//
// Checks before anything is replaced (else nothing changes and the reason is returned): the vtable's slots hold two
// plain getters `lwz r3,N(r3)` / `blr` with N and N + 4 (SizeX, SizeY next to each other), and the viewport's words
// there read as a screen size (320..4096 x 200..4096). Slots 2 / 3 are tried first, then slots 0..11 (the 2006 engine).
#pragma once

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <atomic>
#include <cstdint>

namespace ue3::native_viewport {

inline std::atomic<uint32_t> g_viewport{0};  // the game's FViewport (guest address)
inline std::atomic<uint32_t> g_lines{0};     // the true-1080p screen height (0 = off)
inline std::atomic<bool> g_installed{false};
inline PPCFunc* g_orig_x = nullptr;
inline PPCFunc* g_orig_y = nullptr;
inline uint32_t g_fn_x = 0, g_fn_y = 0, g_slot = 0;

// 1280 -> 1920 (a multiple of 8, as the size function's width hooks); a value already larger is left alone.
inline uint32_t ScaleW(uint32_t w) {
  const uint32_t l = g_lines.load(std::memory_order_relaxed);
  return (l && w && w <= 1280u) ? (w * l / 720u + 7u) / 8u * 8u : w;
}
// 720 -> 1080 (even); also a wide picture's smaller height (ultrawide) by the same factor.
inline uint32_t ScaleH(uint32_t h) {
  const uint32_t l = g_lines.load(std::memory_order_relaxed);
  return (l && h && h <= 720u) ? (h * l / 720u + 1u) & ~1u : h;
}

inline void SizeXHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  g_orig_x(ctx, base);
  if (self && self == g_viewport.load(std::memory_order_relaxed)) ctx.r3.u64 = ScaleW(ctx.r3.u32);
}
inline void SizeYHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  g_orig_y(ctx, base);
  if (self && self == g_viewport.load(std::memory_order_relaxed)) ctx.r3.u64 = ScaleH(ctx.r3.u32);
}

inline uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline bool InImage(uint32_t a) { return a >= 0x82000000u && a < 0x84000000u && (a & 3u) == 0; }
inline bool InHeap(uint32_t a) { return a >= 0x00100000u && a < 0xF0000000u && (a & 3u) == 0; }
// `lwz r3,N(r3)` / `blr`: *off = N
inline bool PlainGetter(const uint8_t* base, uint32_t fn, uint32_t* off) {
  if (!InImage(fn)) return false;
  const uint32_t w0 = Load32(base, fn), w1 = Load32(base, fn + 4);
  if ((w0 & 0xFFFF0000u) != 0x80630000u || w1 != 0x4E800020u) return false;
  *off = w0 & 0xFFFFu;
  return true;
}

struct Result {
  bool ok = false;
  const char* why = "";
  uint32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0, fn_x = 0, fn_y = 0, slot = 0;
};

// The viewport may be made again (keeps the hooks; only the object compared changes).
inline void NoteViewport(uint32_t viewport) {
  if (viewport && g_installed.load(std::memory_order_relaxed)) g_viewport.store(viewport, std::memory_order_relaxed);
}

// Game thread. `viewport`: the game's FViewport* (its FRenderTarget part, as the UI code calls it).
inline Result Install(uint32_t viewport, uint32_t lines) {
  Result r;
  if (g_installed.load()) { r.why = "already on"; return r; }
  rex::Runtime* rt = rex::Runtime::instance();
  rex::runtime::FunctionDispatcher* d = rt ? rt->function_dispatcher() : nullptr;
  const uint8_t* base = rt ? rt->virtual_membase() : nullptr;
  if (!d || !base) { r.why = "runtime or dispatcher missing"; return r; }
  if (!lines || !InHeap(viewport)) { r.why = "no viewport yet"; return r; }
  const uint32_t vt = Load32(base, viewport);
  if (!InImage(vt)) { r.why = "the viewport's vtable is not in the image"; return r; }
  auto try_slot = [&](uint32_t i) {
    uint32_t ox = 0, oy = 0;
    const uint32_t fx = Load32(base, vt + 4 * i), fy = Load32(base, vt + 4 * i + 4);
    if (!PlainGetter(base, fx, &ox) || !PlainGetter(base, fy, &oy) || oy != ox + 4) return false;
    const uint32_t w = Load32(base, viewport + ox), h = Load32(base, viewport + oy);
    if (w < 320u || w > 4096u || h < 200u || h > 4096u) return false;
    r.fn_x = fx, r.fn_y = fy, r.slot = i, r.w0 = w, r.h0 = h;
    return true;
  };
  bool found = try_slot(2);
  for (uint32_t i = 0; !found && i < 12; ++i) found = i != 2 && try_slot(i);
  if (!found) { r.why = "no GetSizeX / GetSizeY getter pair in the viewport's vtable"; return r; }
  PPCFunc* ox = d->GetFunction(r.fn_x);
  PPCFunc* oy = d->GetFunction(r.fn_y);
  if (!ox || !oy || ox == &SizeXHook || oy == &SizeYHook) { r.why = "the getters are not in the dispatcher"; return r; }
  g_orig_x = ox;
  g_orig_y = oy;
  g_lines.store(lines);
  g_viewport.store(viewport);
  if (!d->SetFunction(r.fn_x, &SizeXHook) || !d->SetFunction(r.fn_y, &SizeYHook)) {
    d->SetFunction(r.fn_x, ox);
    d->SetFunction(r.fn_y, oy);
    g_viewport.store(0);
    r.why = "hook install failed";
    return r;
  }
  g_fn_x = r.fn_x, g_fn_y = r.fn_y, g_slot = r.slot;
  g_installed.store(true);
  r.ok = true;
  r.w1 = ScaleW(r.w0);
  r.h1 = ScaleH(r.h0);
  return r;
}

}  // namespace ue3::native_viewport
