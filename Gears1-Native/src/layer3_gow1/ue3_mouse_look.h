// COGBox 0.2.1: native mouse look for the Unreal Engine 3 ports (Gears of War 1, 2, 3 and Judgment).
//
// The console games still carry Unreal Engine 3's PC mouse path: the input config binds MouseX to
// "Count bXAxis | Axis aMouseX" and MouseY to "Count bYAxis | Axis aMouseY" (MouseSensitivity 60), and
// PlayerInput.PlayerInput adds aMouseX / aMouseY to the turn after the right stick's dead zone, response curve and
// acceleration. A window listener banks the captured mouse motion; once per frame a hook sends the banked counts
// into the engine as MouseX / MouseY axis events, so the camera turns the way the PC versions of the engine turn it.
// The keyboard/mouse driver stops turning mouse motion into right-stick deflection (SDK patch 39, mnk_mouse_stick).
// If a check fails, nothing is hooked and the mouse keeps driving the right stick.
//
// Two ways in, by engine version:
//  - Gears 3 / Judgment: hooks UGameViewportClient::InputAxis (this r3 = its FViewportClient interface, Viewport r4,
//    ControllerId r5, Key r6, Delta f1, DeltaTime f2, bGamepad r9) and sends the motion through the original before
//    the first event of the frame for player 1, so the Scaleform menus and PlayerInput both get it.
//  - Gears 2: hooks the viewport's ProcessInput (this r3, DeltaTime f1) and calls the viewport client's InputAxis
//    (vtable +24) before it runs; keeps the "using a gamepad" flag across mouse events (axis accumulate hook) and
//    takes the aim friction off while the mouse was used last.
//  - Gears 1 (UE3 2006): the Gears 2 way with its own layout - viewport client at ProcessInput's this +56, the viewport
//    argument is this +4, InputAxis has no bGamepad. Gears 1's ProcessInput is only reached by a direct `bl`, and a
//    direct call goes to the weak sub_ symbol, not through the dispatcher, so SetFunction never sees it (v1's bug).
//    So entry_fn is the client's Tick (reached through its vtable), which walks Viewports (this +viewports_off) and
//    calls ProcessInput on each: the hook sends the motion to Viewports[0]'s client first. Gears 1 has no "using a
//    gamepad" flag; "pad used last" comes from hooking the viewport client's InputAxis (vtable +24, reached by bctrl)
//    when it is first found, and the friction native is AWarPC::execGetFrictionTarget (its result is cleared).
//
// Method and the Gears 3 base-disc / Gears 2 base-disc addresses from the BSD-3-Clause projects gow3-recomp
// (Copyright (c) 2026, Flippage and the gow3-recomp contributors) and gears-of-war-2-pc-port / gow2-recomp
// (Copyright (c) 2026, Santiago Quiroz Upegui and the gow2-recomp contributors); see THIRD_PARTY_NOTICES.md.
// COGBox's own: the other builds' addresses (found in our generated code by instruction pattern, COGBox jobs
// 572-575), the per-build tables, the launcher settings and the fallback.
// Copyright (c) 2026, Flippage and the gow3-recomp contributors
// Copyright (c) 2026, Santiago Quiroz Upegui and the gow2-recomp contributors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#pragma once

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <xenos/dev_tools.h>   // PR #134: XenosInGameMenuIsOpen
#include <ue3/mouse_carry.h>     // GitHub #228

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

#include <windows.h>

REXCVAR_DEFINE_BOOL(ue3_mouse_look, false, "COGBox",
                    "Native mouse look: the mouse turns the camera through the engine's own PC mouse axes (no stick "
                    "dead zone, cap or acceleration). Off: the mouse drives the right stick");
REXCVAR_DEFINE_DOUBLE(ue3_mouse_scale, 1.0, "COGBox", "Native mouse look: multiplier on the mouse counts")
    .range(0.01, 20.0);
// GitHub #228: replaces --ue3_mouse_min_counts / --ue3_mouse_hold_ms (#115, #175), which held small motion until it
// reached 2 / sensitivity counts - one step of the same angle at every sensitivity, 50+ pixels in the Longshot's zoom.
REXCVAR_DEFINE_DOUBLE(ue3_mouse_step_units, 2.0, "COGBox",
                      "Native mouse look (GitHub #115/#228): motion goes to the engine in whole steps of this many "
                      "rotation units (65536 = 360 degrees) and the rest is carried to the next frames, so slow zoomed "
                      "aiming is neither lost to the engine's whole units nor sent in chunks. 0 = send it as it comes")
    .range(0.0, 50.0);
REXCVAR_DEFINE_BOOL(ue3_mouse_invert_y, false, "COGBox", "Native mouse look: invert up / down");
REXCVAR_DEFINE_BOOL(ue3_mouse_aim_assist, false, "COGBox",
                    "Native mouse look (Gears 1 / 2): keep the aim friction for the mouse too");
REXCVAR_DEFINE_BOOL(ue3_mouse_log, false, "COGBox", "Native mouse look: log what was sent, once a second");

namespace ue3_mouse {

// One build's addresses. 0 = not used / not checked.
struct Build {
  const char* name;
  bool gears2;               // Gears 2 way in (ProcessInput); else the Gears 3 / Judgment one (viewport InputAxis)
  uint32_t entry_fn;         // Gears 3/J: UGameViewportClient::InputAxis; Gears 2: the viewport's ProcessInput
  uint32_t player_axis_fn;   // Gears 3/J: UPlayerInput::InputAxis (fallback); Gears 2: the axis accumulate function
  uint32_t friction_fn;      // Gears 2: the aim friction native
  uint32_t mouse_x_name;     // the FName globals of "MouseX" / "MouseY" (8 bytes each)
  uint32_t mouse_y_name;
  uint32_t right_x_name;     // Gears 2: "XboxTypeS_RightX" / "XboxTypeS_RightY"
  uint32_t right_y_name;
  uint32_t check_slot;       // a vtable / native table entry that must point at check_fn (same-disc checks)
  uint32_t check_fn;
  // Gears 1 additions (defaults = the Gears 2 layout, so the other games' tables are unchanged).
  uint32_t client_off = 52;        // ProcessInput this -> viewport client pointer
  uint32_t viewport_arg_off = 0;   // InputAxis r4 = ProcessInput this + this offset
  uint32_t viewports_off = 0;      // Gears 1: entry_fn is the client Tick (this r3, DeltaTime f1); its Viewports TArray
                                   // (data, count) is at this + viewports_off. The pad-used-last signal then comes from hooking
                                   // the viewport client's InputAxis at first use (no player_axis_fn needed).
};

namespace detail {

inline uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
// An FName as the guest loads it with ld: index in the upper half, number in the lower.
inline uint64_t LoadName(const uint8_t* base, uint32_t a) { return (uint64_t(Load32(base, a)) << 32) | Load32(base, a + 4); }
inline int64_t Now() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

inline const Build* g_build = nullptr;
inline rex::runtime::FunctionDispatcher* g_dispatcher = nullptr;
inline PPCFunc* g_entry_orig = nullptr;
inline PPCFunc* g_axis_orig = nullptr;
inline PPCFunc* g_friction_orig = nullptr;
inline PPCFunc* g_client_axis_orig = nullptr;   // Gears 1: the viewport client's InputAxis (unhooked original)
inline uint32_t g_client_axis_addr = 0;
// One-shot diagnostics (logged a few times in the first minute, whatever ue3_mouse_log says).
inline std::atomic<uint64_t> g_d_moves{0}, g_d_no_window{0}, g_d_no_capture{0}, g_d_not_look{0}, g_d_banked{0};
inline std::atomic<uint64_t> g_d_entry_calls{0}, g_d_taken{0}, g_d_names_wait{0}, g_d_sent{0}, g_d_no_fn{0};
inline std::atomic<uint64_t> g_d_pad_axis{0};
inline uint32_t g_d_viewport = 0, g_d_count = 0, g_d_client = 0, g_d_vt = 0, g_d_fn = 0;
inline std::atomic<bool> g_on{false};
inline std::atomic<bool> g_names_bad{false};
inline thread_local bool g_injecting = false;
inline std::mutex g_bank_mutex;
inline double g_bank_dx = 0, g_bank_dy = 0;
inline ue3::MouseCarry g_carry_x, g_carry_y;  // GitHub #228: turn under one step, sent later (bank mutex)
inline std::atomic<int64_t> g_last_mouse{0}, g_last_pad{0};

inline void Drain(double* dx, double* dy) {
  std::lock_guard<std::mutex> l(g_bank_mutex);
  *dx = g_bank_dx; *dy = g_bank_dy;
  g_bank_dx = g_bank_dy = 0;
  g_carry_x.Clear();
  g_carry_y.Clear();
}

class Listener : public rex::ui::WindowInputListener {
 public:
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (XenosInGameMenuIsOpen()) {   // PR #134 (Flippage): the in-game menu is open - its mouse, not the camera's
      double x, y;
      Drain(&x, &y);
      return;
    }
    rex::ui::Window* w = e.target();
    g_d_moves.fetch_add(1, std::memory_order_relaxed);
    const bool no_window = !w;
    const bool no_capture = !no_window && !w->IsMouseCaptureRequested();
    const bool not_look = !rex::input::mnk::IsMouseLookActive();
    if (no_window) g_d_no_window.fetch_add(1, std::memory_order_relaxed);
    if (no_capture) g_d_no_capture.fetch_add(1, std::memory_order_relaxed);
    if (not_look) g_d_not_look.fetch_add(1, std::memory_order_relaxed);
    if (no_window || no_capture || not_look) {
      double x, y;
      Drain(&x, &y);  // menus / the in-game menu: nothing banked turns the camera afterwards
      return;
    }
    if (e.dx() == 0 && e.dy() == 0) return;
    {
      std::lock_guard<std::mutex> l(g_bank_mutex);
      g_bank_dx += e.dx();
      g_bank_dy += e.dy();
    }
    g_d_banked.fetch_add(1, std::memory_order_relaxed);
    g_last_mouse.store(Now(), std::memory_order_relaxed);
  }
};
inline Listener g_listener;

inline bool SafeCall(PPCFunc* fn, PPCContext* c, uint8_t* base) {
  __try {
    fn(*c, base);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The motion of this frame, scaled and signed for the game (y up), in whole steps the engine keeps. False when none.
// GitHub #115: the engine turns the float turn into whole rotation units every frame (UnrealScript's float-to-int when
// PlayerController.UpdateRotation fills its Rotator), so a frame's turn under one unit is lost - zoomed, slow aiming was
// a dead zone. GitHub #228: the fix for that held small motion back until it reached 2 / sensitivity mouse counts
// (#175), so the smallest move was the same angle at every sensitivity and, with "Mouse turn when aiming: Same",
// 50+ pixels in the Longshot's 10 degree zoom. Now the turn goes out in steps of --ue3_mouse_step_units rotation
// units (ue3/mouse_carry.h) and only the part under a step is carried to the next frame.
inline bool Take(double* x, double* y) {
  const double s = REXCVAR_GET(ue3_mouse_scale);
  const double upv = ue3::MouseUnitsPerValue(0.0);  // this port doesn't note the camera FOV: the narrowest is assumed
  const double step = REXCVAR_GET(ue3_mouse_step_units);
  const double sy = REXCVAR_GET(ue3_mouse_invert_y) ? s : -s;
  {
    std::lock_guard<std::mutex> l(g_bank_mutex);
    *x = g_carry_x.Take(g_bank_dx * s, upv, step);
    *y = g_carry_y.Take(g_bank_dy * sy, upv, step);
    g_bank_dx = g_bank_dy = 0;
  }
  return *x != 0 || *y != 0;
}

// The names are made when the game starts its engine, after the hooks went in: checked at first use. Wrong ones
// (this build's table is off) switch the mouse back to the right stick instead of sending nonsense keys.
inline bool NamesOk(const uint8_t* base, uint64_t* mx, uint64_t* my) {
  if (g_names_bad.load(std::memory_order_relaxed)) return false;
  *mx = LoadName(base, g_build->mouse_x_name);
  *my = LoadName(base, g_build->mouse_y_name);
  const uint32_t ix = uint32_t(*mx >> 32), iy = uint32_t(*my >> 32);
  if (ix == 0 || iy == 0) { g_d_names_wait.fetch_add(1, std::memory_order_relaxed); return false; }  // not made yet
  bool ok = ix != iy && ix < 0x400000 && iy < 0x400000;
  if (ok && g_build->right_x_name) {
    const uint32_t rx = Load32(base, g_build->right_x_name), ry = Load32(base, g_build->right_y_name);
    ok = rx != 0 && ry != 0 && ix != rx && ix != ry && iy != rx && iy != ry;
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    REXLOG_INFO("ue3_mouse_look: {}: MouseX name {} / MouseY {}{}", g_build->name, ix, iy, ok ? "" : " - not plausible, the mouse goes back to the right stick");
  if (!ok) {
    g_names_bad.store(true);
    rex::cvar::SetFlagByName("mnk_mouse_stick", "true");
  }
  return ok;
}

inline void MaybeLog(double x, double y, bool sent) {
  if (!REXCVAR_GET(ue3_mouse_log)) return;
  static double ax = 0, ay = 0;
  static uint64_t frames = 0, failed = 0;
  static int64_t start = 0;
  if (x != 0 || y != 0) { ax += x; ay += y; ++frames; if (!sent) ++failed; }
  const int64_t now = Now();
  if (!start) start = now;
  if (now - start < std::chrono::steady_clock::duration(std::chrono::seconds(1)).count()) return;
  if (frames) REXLOG_INFO("ue3_mouse_look: sent x={:.1f} y={:.1f} frames={} failed={}", ax, ay, frames, failed);
  ax = ay = 0; frames = failed = 0; start = now;
}

// ---- Gears 3 / Judgment ----------------------------------------------------------------------------------------------
inline bool SendViewport(const PPCContext& ctx, uint8_t* base, uint64_t key, double v) {
  PPCContext c = ctx;
  c.r6.u64 = key;
  c.r9.u64 = 0;  // bGamepad
  c.f1.f64 = double(float(v));
  return SafeCall(g_entry_orig, &c, base);
}
inline void ViewportAxisHook(PPCContext& ctx, uint8_t* base) {
  if (!g_injecting && ctx.r5.u32 == 0) {  // player 1, once per frame (the first event that finds banked motion)
    double x = 0, y = 0;
    uint64_t mx, my;
    if (Take(&x, &y) && NamesOk(base, &mx, &my)) {
      bool sent = true;
      g_injecting = true;
      if (x != 0) sent &= SendViewport(ctx, base, mx, x);
      if (y != 0) sent &= SendViewport(ctx, base, my, y);
      g_injecting = false;
      MaybeLog(x, y, sent);
    }
  }
  g_entry_orig(ctx, base);
}

// ---- Gears 2 ---------------------------------------------------------------------------------------------------------
constexpr uint32_t kClientInputAxisSlot = 24;
constexpr uint32_t kUsingGamepadBit = 0x80000000;

inline bool SendClient(PPCContext& ctx, uint8_t* base, PPCFunc* fn, uint32_t client, uint32_t viewport, uint64_t key,
                       double v, double dt) {
  PPCContext c = ctx;
  c.r3.u64 = client;
  c.r4.u64 = viewport;
  c.r5.u64 = 0;  // ControllerId
  c.r6.u64 = key;
  c.r9.u64 = 0;  // bGamepad
  c.f1.f64 = double(float(v));
  c.f2.f64 = dt;
  return SafeCall(fn, &c, base);
}
inline void ClientAxisHook(PPCContext& ctx, uint8_t* base);
// Sends this frame's banked motion to one viewport's client (its InputAxis, vtable +24), as ProcessInput does for sticks.
inline void InjectViewport(PPCContext& ctx, uint8_t* base, uint32_t viewport, double dt) {
  const uint32_t client = viewport ? Load32(base, viewport + g_build->client_off) : 0;
  const uint32_t vt = client ? Load32(base, client) : 0;
  const uint32_t fa = vt ? Load32(base, vt + kClientInputAxisSlot) : 0;
  g_d_viewport = viewport; g_d_client = client; g_d_vt = vt; g_d_fn = fa;
  // Gears 1: the first time the client's InputAxis is known, hook it for the pad-used-last signal.
  if (g_build->viewports_off && fa && !g_client_axis_addr) {
    if (PPCFunc* orig = g_dispatcher->GetFunction(fa)) {
      g_client_axis_orig = orig;
      g_client_axis_addr = fa;
      const bool ok = g_dispatcher->SetFunction(fa, &ClientAxisHook);
      REXLOG_INFO("ue3_mouse_look: viewport {:08X} client {:08X} vtable {:08X} InputAxis {:08X} - pad hook {}", viewport,
                  client, vt, fa, ok ? "on" : "FAILED");
    }
  }
  double x = 0, y = 0;
  uint64_t mx, my;
  if (!viewport || !Take(&x, &y)) return;
  g_d_taken.fetch_add(1, std::memory_order_relaxed);
  if (!NamesOk(base, &mx, &my)) return;
  bool sent = false;
  PPCFunc* fn = nullptr;
  if (fa) fn = (fa == g_client_axis_addr && g_client_axis_orig) ? g_client_axis_orig : g_dispatcher->GetFunction(fa);
  const uint32_t vparg = viewport + g_build->viewport_arg_off;
  if (fn) {
    sent = true;
    if (x != 0) sent &= SendClient(ctx, base, fn, client, vparg, mx, x, dt);
    if (y != 0) sent &= SendClient(ctx, base, fn, client, vparg, my, y, dt);
    if (sent) g_d_sent.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_d_no_fn.fetch_add(1, std::memory_order_relaxed);
  }
  MaybeLog(x, y, sent);
}
inline void Diag();
inline void ProcessInputHook(PPCContext& ctx, uint8_t* base) {
  g_d_entry_calls.fetch_add(1, std::memory_order_relaxed);
  InjectViewport(ctx, base, ctx.r3.u32, ctx.f1.f64);
  Diag();
  g_entry_orig(ctx, base);
}
// Gears 1: the client's Tick (this r3, DeltaTime f1); it calls ProcessInput on each of Viewports (data, count at
// this + viewports_off) with the same DeltaTime. The motion goes to Viewports[0] before the original runs.
inline void ClientTickHook(PPCContext& ctx, uint8_t* base) {
  g_d_entry_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t self = ctx.r3.u32;
  const uint32_t data = self ? Load32(base, self + g_build->viewports_off) : 0;
  const uint32_t count = self ? Load32(base, self + g_build->viewports_off + 4) : 0;
  g_d_count = count;
  const uint32_t viewport = (data && count > 0 && count < 64) ? Load32(base, data) : 0;
  if (viewport) InjectViewport(ctx, base, viewport, ctx.f1.f64);
  else { double x, y; Drain(&x, &y); }
  Diag();
  g_entry_orig(ctx, base);
}
inline bool MouseLast() {
  const int64_t m = g_last_mouse.load(std::memory_order_relaxed);
  return m != 0 && m >= g_last_pad.load(std::memory_order_relaxed);
}
inline void FrictionHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t result = ctx.r5.u32;
  g_friction_orig(ctx, base);
  if (REXCVAR_GET(ue3_mouse_aim_assist) || !MouseLast()) return;
  if (result && Load32(base, result)) Store32(base, result, 0);
}
inline void AxisHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32;
  const double v = ctx.f1.f64;
  const uint32_t key = self ? Load32(base, self + 220) : 0;
  const uint32_t before = self ? Load32(base, self + 216) : 0;
  g_axis_orig(ctx, base);
  if (!self || !key) return;
  if (key == Load32(base, g_build->mouse_x_name) || key == Load32(base, g_build->mouse_y_name)) {
    const uint32_t now = Load32(base, self + 216);
    const uint32_t merged = (now & ~kUsingGamepadBit) | (before & kUsingGamepadBit);
    if (merged != now) Store32(base, self + 216, merged);
  } else if (v != 0 && (key == Load32(base, g_build->right_x_name) || key == Load32(base, g_build->right_y_name))) {
    g_last_pad.store(Now(), std::memory_order_relaxed);
  }
}

// Gears 1: the viewport client's InputAxis (client r3, viewport r4, ControllerId r5, Key r6 as an FName, Delta f1 -
// the stick in -1..1, DeltaTime f2). A right stick past the XInput right dead zone marks the pad as used last.
constexpr double kPadDeadZone = 8689.0 / 32767.0;
inline void ClientAxisHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t key = uint32_t(ctx.r6.u64 >> 32);
  const double v = ctx.f1.f64;
  if (key && std::fabs(v) > kPadDeadZone &&
      (key == Load32(base, g_build->right_x_name) || key == Load32(base, g_build->right_y_name))) {
    g_last_pad.store(Now(), std::memory_order_relaxed);
    g_d_pad_axis.fetch_add(1, std::memory_order_relaxed);
  }
  g_client_axis_orig(ctx, base);
}

// One-shot diagnostics: 6 lines at about 5 s intervals after the first entry call, whatever ue3_mouse_log says.
inline void Diag() {
  static int64_t next = 0;
  static int left = 6;
  if (left <= 0) return;
  const int64_t now = Now();
  const int64_t step = std::chrono::steady_clock::duration(std::chrono::seconds(5)).count();
  if (!next) { next = now + step; return; }
  if (now < next) return;
  next = now + step;
  --left;
  REXLOG_INFO("ue3_mouse_look diag: entry calls {} viewports {} viewport {:08X} client {:08X} vtable {:08X} fn {:08X} | "
              "mouse moves {} banked {} no-window {} no-capture {} not-mouse-look {} | taken {} names-not-made {} "
              "sent {} no-fn {} | pad-axis {} names-bad {}",
              g_d_entry_calls.load(), g_d_count, g_d_viewport, g_d_client, g_d_vt, g_d_fn, g_d_moves.load(),
              g_d_banked.load(), g_d_no_window.load(), g_d_no_capture.load(), g_d_not_look.load(), g_d_taken.load(),
              g_d_names_wait.load(), g_d_sent.load(), g_d_no_fn.load(), g_d_pad_axis.load(), g_names_bad.load());
}

}  // namespace detail

// Before the input system reads the mouse (OnPreSetup or OnPostSetup start): only when the launcher asked for it.
inline bool Wanted() { return REXCVAR_GET(ue3_mouse_look) && rex::cvar::Query<bool>("mnk_mouse"); }

// After setup (the game image is loaded): checks the build's addresses and hooks. Logs why when it stays off.
inline void Install(const Build& b, rex::ui::Window* window) {
  using namespace detail;
  if (!Wanted() || g_on.load()) return;
  rex::Runtime* rt = rex::Runtime::instance();
  rex::runtime::FunctionDispatcher* d = rt ? rt->function_dispatcher() : nullptr;
  uint8_t* base = rt ? rt->virtual_membase() : nullptr;
  const char* why = nullptr;
  if (!rt || !d || !base || !window) why = "runtime, dispatcher, memory or window missing";
  else if (b.check_slot && Load32(base, b.check_slot) != b.check_fn) why = "table entry check failed (another build?)";
  PPCFunc *entry = nullptr, *axis = nullptr, *friction = nullptr;
  if (!why) {
    entry = d->GetFunction(b.entry_fn);
    axis = b.player_axis_fn ? d->GetFunction(b.player_axis_fn) : nullptr;
    friction = b.friction_fn ? d->GetFunction(b.friction_fn) : nullptr;
    // Gears 2 needs the axis accumulate hook; Gears 1 (viewports_off) hooks the client's InputAxis later instead.
    const bool pad_ok = b.viewports_off ? true : axis != nullptr;
    if (!entry || (b.gears2 && (!pad_ok || !friction))) why = "a game function is missing";
  }
  if (!why) {
    g_build = &b;
    g_dispatcher = d;
    g_entry_orig = entry;
    g_axis_orig = axis;
    g_friction_orig = friction;
    PPCFunc* hook = !b.gears2 ? &ViewportAxisHook : b.viewports_off ? &ClientTickHook : &ProcessInputHook;
    bool ok = d->SetFunction(b.entry_fn, hook);
    if (ok && b.gears2) {
      if (axis) ok = d->SetFunction(b.player_axis_fn, &AxisHook);
      ok = ok && d->SetFunction(b.friction_fn, &FrictionHook);
    }
    // The hook must be what an indirect call to entry_fn now finds (v1's Gears 1 entry was only called directly).
    if (ok && d->GetFunction(b.entry_fn) != hook) REXLOG_WARN("ue3_mouse_look: entry {:08X} does not resolve to the hook", b.entry_fn);
    if (!ok) {
      d->SetFunction(b.entry_fn, entry);
      if (axis) d->SetFunction(b.player_axis_fn, axis);
      if (friction) d->SetFunction(b.friction_fn, friction);
      why = "hook install failed";
    }
  }
  if (why) {
    rex::cvar::SetFlagByName("mnk_mouse_stick", "true");
    REXLOG_WARN("ue3_mouse_look: off for {} ({}) - the mouse drives the right stick", b.name, why);
    return;
  }
  rex::cvar::SetFlagByName("mnk_mouse_stick", "false");
  window->AddInputListener(&g_listener, 1);
  g_on.store(true);
  REXLOG_INFO("ue3_mouse_look: on for {} (entry {:08X})", b.name, b.entry_fn);
}

}  // namespace ue3_mouse
