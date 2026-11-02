// COGBox 0.2.1: native mouse look for the Unreal Engine 3 ports (Gears of War 2, 3 and Judgment).
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
// GitHub #52: UE3's PlayerInput multiplies the mouse axes by FOVScale (FOVAngle / 90), so a wider camera turns faster
// per count. The launcher's Field of view (%) widens every camera by that factor (GearCamera.ini FOVAngle*=k), so the
// mouse got faster with it. This divides that factor out again: the launcher sends 100 / FOV %. If the ultrawide
// patch turns out to change FOVAngle too, multiply its factor in here. 1 = as before. Zoom / roadie-run FOV changes by the
// game itself keep the console's scaling (aiming zoomed is meant to be slower).
REXCVAR_DEFINE_DOUBLE(ue3_mouse_fov_comp, 1.0, "COGBox",
                      "Native mouse look: multiplier that undoes the launcher's FOV / ultrawide widening (GitHub #52)")
    .range(0.25, 4.0);
// GitHub #114: the same FOVScale makes a camera the game widens itself turn faster per count: down but not out
// (GameplayCam_DBNO FOVAngle 110 against the default 80 = 1.375x), roadie run. The view's field of view (degrees, before
// the ultrawide widening) is noted once per view at the projection set-up (the ultrawide FOV hook site, which runs
// whether or not ultrawide is on) - the value PlayerController.GetFOVAngle gives PlayerInput. While it is wider than
// this reference (times the launcher's FOV %), the mouse is scaled by reference / FOV, so it turns as with the default
// camera. Narrower views (zoom) keep the console's slower scaling. 0 = off.
REXCVAR_DEFINE_DOUBLE(ue3_mouse_wide_fov_ref, 80.0, "COGBox",
                      "Native mouse look (GitHub #114): cameras wider than this FOV (degrees, x the launcher's FOV %) "
                      "turn as fast per count as this one (DBNO, roadie run). 0 = off")
    .range(0.0, 170.0);
// GitHub #52 (still uneven after 0.3.5): each camera mode has its own FOV in GearCamera.ini (cover 95, targeting 50,
// roadie run 105, DBNO 110 ...), and UE3 scales the mouse by FOV / 90, so the same mouse motion turned the view by a
// different angle in each mode. With this on (default), every camera turns the same angle per count as the reference
// one - narrower ones (aiming, zoom) too, not only the wider ones #114 evened out. Off: only cameras wider than the
// reference are evened out, and aiming / zoom keep the console's slower turn.
REXCVAR_DEFINE_BOOL(ue3_mouse_fov_constant, true, "COGBox",
                    "Native mouse look (GitHub #52): every camera mode turns the same angle per mouse count (aiming and "
                    "zoom included); off = aiming / zoom keep the game's slower turn");
REXCVAR_DEFINE_DOUBLE(ue3_mouse_min_counts, 2.0, "COGBox",
                      "Native mouse look (GitHub #115): smaller motion waits, per axis, until it reaches this many counts "
                      "(or --ue3_mouse_hold_ms), so slow zoomed aiming isn't lost to the engine's whole rotation units. 0 = off")
    .range(0.0, 20.0);
REXCVAR_DEFINE_DOUBLE(ue3_mouse_hold_ms, 25.0, "COGBox",
                      "Native mouse look (GitHub #115): the longest a small motion waits before it is sent anyway")
    .range(0.0, 200.0);
REXCVAR_DEFINE_BOOL(ue3_mouse_invert_y, false, "COGBox", "Native mouse look: invert up / down");
REXCVAR_DEFINE_BOOL(ue3_mouse_aim_assist, false, "COGBox",
                    "Native mouse look (Gears 2): keep the aim friction for the mouse too");
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
inline std::atomic<bool> g_on{false};
inline std::atomic<bool> g_names_bad{false};
inline thread_local bool g_injecting = false;
inline std::mutex g_bank_mutex;
inline double g_bank_dx = 0, g_bank_dy = 0;
inline std::atomic<int64_t> g_last_mouse{0}, g_last_pad{0};
inline std::atomic<float> g_view_fov{0.0f};  // GitHub #114: the latest view's field of view, degrees (0 = none yet)

// GitHub #114: the factor for a camera wider than the reference; 1 otherwise.
inline double WideCameraComp() {
  const double ref = REXCVAR_GET(ue3_mouse_wide_fov_ref);
  const double fov = g_view_fov.load(std::memory_order_relaxed);
  if (ref <= 0 || !(fov > 1.0 && fov < 170.0)) return 1.0;
  const double widened_ref = ref / REXCVAR_GET(ue3_mouse_fov_comp);  // fov_comp = 100 / launcher FOV %
  if (REXCVAR_GET(ue3_mouse_fov_constant)) return widened_ref / fov;  // GitHub #52: the same angle per count everywhere
  return fov > widened_ref ? widened_ref / fov : 1.0;
}

inline void Drain(double* dx, double* dy) {
  std::lock_guard<std::mutex> l(g_bank_mutex);
  *dx = g_bank_dx; *dy = g_bank_dy;
  g_bank_dx = g_bank_dy = 0;
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
    if (!w || !w->IsMouseCaptureRequested() || !rex::input::mnk::IsMouseLookActive()) {
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

// The banked motion of this frame, scaled and signed for the game (y up). False when there is none.
// GitHub #115: the engine turns the float turn into whole rotation units every frame (UnrealScript's float-to-int when
// PlayerController.UpdateRotation fills its Rotator), so a frame's turn under one unit is lost. Zoomed (Longshot,
// FOVScale = FOV / 90 small) and at a high frame rate, slow aiming moves were all under it: a dead zone. A small
// motion is now kept banked, per axis, until it reaches --ue3_mouse_min_counts or has waited --ue3_mouse_hold_ms, so it
// arrives in steps the engine keeps. Ordinary motion (above the minimum) goes out the same frame, as before.
inline void DrainReady(double* dx, double* dy) {
  static int64_t held_x = 0, held_y = 0;  // when each axis' banked motion started waiting (0 = nothing waiting)
  const double min_counts = REXCVAR_GET(ue3_mouse_min_counts);
  const int64_t hold = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           std::chrono::duration<double, std::milli>(REXCVAR_GET(ue3_mouse_hold_ms))).count();
  const int64_t now = Now();
  std::lock_guard<std::mutex> l(g_bank_mutex);
  auto take = [&](double& bank, int64_t& held, double* out) {
    *out = 0;
    if (bank == 0) { held = 0; return; }
    if (min_counts <= 0 || std::fabs(bank) >= min_counts || (held && now - held >= hold)) {
      *out = bank; bank = 0; held = 0;
    } else if (!held) {
      held = now;
    }
  };
  take(g_bank_dx, held_x, dx);
  take(g_bank_dy, held_y, dy);
}
inline bool Take(double* x, double* y) {
  double dx, dy;
  DrainReady(&dx, &dy);
  if (dx == 0 && dy == 0) return false;
  const double s = REXCVAR_GET(ue3_mouse_scale) * REXCVAR_GET(ue3_mouse_fov_comp)  // GitHub #52
                   * WideCameraComp();                                                  // GitHub #114
  *x = dx * s;
  *y = -dy * s;
  if (REXCVAR_GET(ue3_mouse_invert_y)) *y = -*y;
  return true;
}

// The names are made when the game starts its engine, after the hooks went in: checked at first use. Wrong ones
// (this build's table is off) switch the mouse back to the right stick instead of sending nonsense keys.
inline bool NamesOk(const uint8_t* base, uint64_t* mx, uint64_t* my) {
  if (g_names_bad.load(std::memory_order_relaxed)) return false;
  *mx = LoadName(base, g_build->mouse_x_name);
  *my = LoadName(base, g_build->mouse_y_name);
  const uint32_t ix = uint32_t(*mx >> 32), iy = uint32_t(*my >> 32);
  if (ix == 0 || iy == 0) return false;  // not made yet
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
constexpr uint32_t kViewportClientOff = 52;
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
inline void ProcessInputHook(PPCContext& ctx, uint8_t* base) {
  const double dt = ctx.f1.f64;
  const uint32_t viewport = ctx.r3.u32;
  double x = 0, y = 0;
  uint64_t mx, my;
  if (viewport && Take(&x, &y) && NamesOk(base, &mx, &my)) {
    bool sent = false;
    const uint32_t client = Load32(base, viewport + kViewportClientOff);
    const uint32_t vt = client ? Load32(base, client) : 0;
    const uint32_t fa = vt ? Load32(base, vt + kClientInputAxisSlot) : 0;
    if (PPCFunc* fn = fa ? g_dispatcher->GetFunction(fa) : nullptr) {
      sent = true;
      if (x != 0) sent &= SendClient(ctx, base, fn, client, viewport, mx, x, dt);
      if (y != 0) sent &= SendClient(ctx, base, fn, client, viewport, my, y, dt);
    }
    MaybeLog(x, y, sent);
  }
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

}  // namespace detail

// GitHub #114: each view's field of view in degrees (the projection set-up calls this once per view).
inline void NoteViewFov(double fov) {
  if (fov > 1.0 && fov < 170.0) detail::g_view_fov.store(float(fov), std::memory_order_relaxed);
}

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
    if (!entry || (b.gears2 && (!axis || !friction))) why = "a game function is missing";
  }
  if (!why) {
    g_build = &b;
    g_dispatcher = d;
    g_entry_orig = entry;
    g_axis_orig = axis;
    g_friction_orig = friction;
    bool ok = d->SetFunction(b.entry_fn, b.gears2 ? &ProcessInputHook : &ViewportAxisHook);
    if (ok && b.gears2) ok = d->SetFunction(b.player_axis_fn, &AxisHook) && d->SetFunction(b.friction_fn, &FrictionHook);
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
