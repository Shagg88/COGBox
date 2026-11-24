// COGBox 0.2.1: native mouse look for the Unreal Engine 3 ports (Gears of War 1, 2, 3 and Judgment).
// Layer 2 (Unreal Engine 3): ONE copy, here, for all four games (23 Nov 2026; until then each game had its own
// layer3 ue3_mouse_look.h and they had drifted). Each game's layer3 mouse_look.cpp keeps only its build table
// (addresses) and the per-game choices below (Build::scaling, Build::diag), each with its reason.
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

#include <rex/input/device_assignment.h>   // GitHub #420: KeyboardMouseUser
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
#include <ue3/objects.h>         // --ue3_mouse_turn_cap: PlayerInput's scales, read by name (each game's ue3::obj)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>

#include <windows.h>

REXCVAR_DEFINE_BOOL(ue3_mouse_look, false, "COGBox",
                    "Native mouse look: the mouse turns the camera through the engine's own PC mouse axes (no stick "
                    "dead zone, cap or acceleration). Off: the mouse drives the right stick");
REXCVAR_DEFINE_DOUBLE(ue3_mouse_scale, 1.0, "COGBox", "Native mouse look: multiplier on the mouse counts")
    .range(0.01, 20.0);
// GitHub #52 (Gears 2 / 3, Build::scaling kFovEven): UE3's PlayerInput multiplies the mouse axes by FOVScale
// (FOVAngle / 90), so a wider camera turns faster per count - roadie run ~50% faster (jojobear69, 2 Nov). The launcher's
// Field of view (%) widens every camera by that factor (GearCamera.ini FOVAngle*=k); it sends 100 / FOV % here. The
// ultrawide patch widens the projection, not FOVAngle, so it is not in here. 1 = as before. Not read by Gears 1 /
// Judgment (kAimZoomOnly).
REXCVAR_DEFINE_DOUBLE(ue3_mouse_fov_comp, 1.0, "COGBox",
                      "Native mouse look: multiplier that undoes the launcher's FOV widening (GitHub #52; Gears 2 / 3)")
    .range(0.25, 4.0);
// The view's field of view (degrees, before the ultrawide widening) is noted once per view at the projection set-up
// (each game's ultrawide / view FOV hook, which runs whether or not ultrawide is on) - the value
// PlayerController.GetFOVAngle gives PlayerInput. The reference, 80, is GameplayCam_Default's FOVAngle in the Gears 1 /
// Gears 3 coalesced ini (cover 95, roadie run 105, DBNO 110, aiming 50).
//  - Gears 2 / 3 (kFovEven, GitHub #114 / #52): the mouse is scaled by reference / FOV, so every camera turns as the
//    walking camera does (with --ue3_mouse_fov_constant=false only the wider ones). 0 = off.
//  - every game: the aim / zoom thresholds below are measured from it (Judgment and Gears 1 use it only for that; Gears
//    1 had 80 written in its own copy before 23 Nov - the same default, and the launcher never sends this option).
REXCVAR_DEFINE_DOUBLE(ue3_mouse_wide_fov_ref, 80.0, "COGBox",
                      "Native mouse look: the walking camera's FOV (degrees). Gears 2 / 3 (GitHub #52/#114): cameras "
                      "wider than this (x the launcher's FOV %) turn as fast per count as this one; every game: the aim "
                      "/ zoom thresholds are measured from it. 0 = off")
    .range(0.0, 170.0);
// 0.5.0 (Reddit: the Longshot's zoom turns too fast): extra multipliers while aiming (camera narrower than 80% of the
// walking camera) and while zoomed through a scope (narrower than 40%). 1 = as before. The launcher's "Aim sensitivity"
// and "Zoom sensitivity" (%).
REXCVAR_DEFINE_DOUBLE(ue3_mouse_aim_scale, 1.0, "COGBox", "Native mouse look: extra multiplier while aiming (1 = as before)")
    .range(0.1, 2.0);
REXCVAR_DEFINE_DOUBLE(ue3_mouse_zoom_scale, 1.0, "COGBox",
                      "Native mouse look: extra multiplier while zoomed through a scope (1 = as before)")
    .range(0.1, 2.0);
// GitHub #52 (still uneven after 0.3.5; Gears 2 / 3 only - kFovEven): each camera mode has its own FOV in
// GearCamera.ini and UE3 scales the mouse by FOV / 90, so the same motion turned a different angle in each mode. On
// (default): every camera turns the same angle per count as the reference one, aiming and zoom too. Off: only cameras
// wider than the reference are evened out, and aiming / zoom keep the console's slower turn.
REXCVAR_DEFINE_BOOL(ue3_mouse_fov_constant, true, "COGBox",
                    "Native mouse look (GitHub #52): every camera mode turns the same angle per mouse count (aiming and "
                    "zoom included); off = aiming / zoom keep the game's slower turn");
// GitHub #228: replaces --ue3_mouse_min_counts / --ue3_mouse_hold_ms (#115, #175), which held small motion until it
// reached 2 / sensitivity counts - one step of the same angle at every sensitivity, 50+ pixels in the Longshot's zoom.
REXCVAR_DEFINE_DOUBLE(ue3_mouse_step_units, 2.0, "COGBox",
                      "Native mouse look (GitHub #115/#228): motion goes to the engine in whole steps of this many "
                      "rotation units (65536 = 360 degrees) and the rest is carried to the next frames, so slow zoomed "
                      "aiming is neither lost to the engine's whole units nor sent in chunks. 0 = send it as it comes")
    .range(0.0, 50.0);
REXCVAR_DEFINE_BOOL(ue3_mouse_confine, true, "COGBox",
                    "Native mouse look (GitHub #430): keep the pointer inside the game window while the camera has the "
                    "mouse, so a click cannot land on a window behind the game or a second monitor (false = as before)");
REXCVAR_DEFINE_BOOL(ue3_mouse_invert_y, false, "COGBox", "Native mouse look: invert up / down");
REXCVAR_DEFINE_BOOL(ue3_mouse_aim_assist, false, "COGBox",
                    "Native mouse look (Gears 1 / 2): keep the aim friction for the mouse too");
REXCVAR_DEFINE_BOOL(ue3_mouse_log, false, "COGBox", "Native mouse look: log what was sent, once a second");
// Discord #suggestions (23 Nov 2026, "for parity with the original 360 version"): on, each frame's mouse turn (yaw and
// pitch separately) is limited to what the game's own full right-stick deflection turns in that frame, so the mouse can
// never turn faster than a controller. Off (default): uncapped, as before. How the stick's turn is worked out: TurnCap.
REXCVAR_DEFINE_BOOL(ue3_mouse_turn_cap, false, "COGBox",
                    "Native mouse look: limit mouse turning to controller speed (as on Xbox 360) - each frame's turn, yaw "
                    "and pitch separately, at most what the game's full right-stick deflection turns in that frame");
REXCVAR_DEFINE_DOUBLE(ue3_mouse_turn_cap_scale, 1.0, "COGBox",
                      "Native mouse look: multiplier on --ue3_mouse_turn_cap's limit (developer: matching a measured pad turn)")
    .range(0.1, 4.0);

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
  // How the mouse counts are scaled per camera (23 Nov 2026 unification: the four copies had two ways):
  //  kFovEven     (Gears 2, 3): x --ue3_mouse_fov_comp x WideCameraComp (GitHub #52 / #114 - every camera turns the
  //               same angle per count, aim / zoom multipliers on top); the step carry estimates units from the FOV.
  //  kAimZoomOnly (Gears 1, Judgment): only the 0.5.0 aim / zoom multipliers; the carry assumes the narrowest FOV.
  //               #52 / #114 were never ported to these two (not tested there), so they keep the console's per-camera
  //               mouse speed; switching them is a separate, tested change.
  enum class Scaling : uint8_t { kFovEven, kAimZoomOnly };
  Scaling scaling = Scaling::kFovEven;
  bool diag = false;               // Gears 1: six "ue3_mouse_look diag" lines in the first half minute (GitHub #82's
                                   // bring-up; kept as it was)
  // --ue3_mouse_turn_cap: opens this game's object tables (ue3::obj::Init with its globals / layout), so the cap can
  // read PlayerInput's live scales. nullptr: the config values below are used.
  bool (*open_objects)() = nullptr;
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

// 0.5.0 (Shagg, RC6-6: no mouse with NVIDIA DLSS frame generation): the frame generation swap chain is made after the
// game has started, and the window can be left believing it lost the focus while it is still the foreground window -
// then the keyboard / mouse driver ignores the mouse (no focus) and never asks for the capture again. On a mouse event
// while our window is the foreground window but not "focused", the focus is requested again (on the UI thread, at most
// every 500 ms), which sends the got-focus event the driver waits for.
// GitHub #430 (Proton, also possible on Windows): while mouse look has the mouse, a click could land on the COGBox
// launcher behind the game, and the pointer could drift onto a second monitor - SDL's pointer lock does not always hold
// (under Wine its relative mode can fall back to recentring the cursor, which leaves it free between two moves). So,
// while the camera owns the mouse (capture requested, mouse look active, no in-game menu) and our window is in front,
// the pointer is clipped to the window's client area (ClipCursor, which Wine implements); it is released on focus loss,
// when a menu opens, and when capture ends. --ue3_mouse_confine=false = as before.
inline std::atomic<bool> g_clipped{false};
inline void Confine(rex::ui::Window* w, bool want) {
  if (!REXCVAR_GET(ue3_mouse_confine)) want = false;
  HWND fg = nullptr;
  if (want) {
    fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!w || !w->HasFocus() || !fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId()) want = false;
  }
  if (!want) {
    if (g_clipped.exchange(false)) ClipCursor(nullptr);
    return;
  }
  RECT r{};
  if (!GetClientRect(fg, &r) || r.right <= r.left || r.bottom <= r.top) return;
  POINT p[2] = {{r.left, r.top}, {r.right, r.bottom}};
  ClientToScreen(fg, &p[0]);
  ClientToScreen(fg, &p[1]);
  const RECT want_rect{p[0].x, p[0].y, p[1].x, p[1].y};
  RECT cur{};
  if (g_clipped.load() && GetClipCursor(&cur) && EqualRect(&cur, &want_rect)) return;
  if (!ClipCursor(&want_rect)) return;
  if (!g_clipped.exchange(true)) {
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 3)
      REXLOG_INFO("ue3_mouse_confine: pointer kept inside the game window ({}x{}) while mouse look has it", want_rect.right - want_rect.left,
                  want_rect.bottom - want_rect.top);
  }
}
inline bool CameraOwnsMouse(rex::ui::Window* w) {
  return w && w->IsMouseCaptureRequested() && rex::input::mnk::IsMouseLookActive() && !XenosInGameMenuIsOpen();
}
class FocusListener : public rex::ui::WindowListener {
 public:
  void OnLostFocus(rex::ui::UISetupEvent&) override { Confine(nullptr, false); }
  void OnMinimized(rex::ui::UIEvent&) override { Confine(nullptr, false); }
  void OnClosing(rex::ui::UIEvent&) override { Confine(nullptr, false); }
};
inline FocusListener g_focus_listener;

inline std::atomic<int64_t> g_focus_heal_last{0};
inline std::atomic<int> g_focus_heals{0};
inline void FocusHeal(rex::ui::Window* w) {
  if (!w || w->HasFocus()) return;
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  if (!fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId()) return;
  const int64_t now = Now(), last = g_focus_heal_last.load(std::memory_order_relaxed);
  if (last && now - last < std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(500)).count()) return;
  g_focus_heal_last.store(now, std::memory_order_relaxed);
  if (g_focus_heals.fetch_add(1) < 6)
    REXLOG_INFO("ue3_mouse_look: the window is in front but had no focus - asking for it again (mouse / keyboard input)");
  // the window is already in front, so raising it sends nothing: take the keyboard focus away and give it back, which
  // makes Windows send the focus messages the window and the driver listen to (we are on the UI thread here)
  SetFocus(nullptr);
  SetFocus(fg);
  w->Focus();
}

class Listener : public rex::ui::WindowInputListener {
 public:
  void OnMouseDown(rex::ui::MouseEvent& e) override { Confine(e.target(), CameraOwnsMouse(e.target())); }   // GitHub #430
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    FocusHeal(e.target());   // 0.5.0: see FocusHeal
    Confine(e.target(), CameraOwnsMouse(e.target()));   // GitHub #430
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
inline std::atomic<float> g_view_fov{0.0f};  // the latest view's field of view, degrees (0 = none yet; NoteViewFov)

// kFovEven (GitHub #52 / #114): reference / FOV (fov_constant) or, off, only for cameras wider than the reference; times
// the player's aim / zoom multipliers (scoped < 40 % of the walking camera, aiming < 80 %); 1 otherwise.
inline double WideCameraComp() {
  const double ref = REXCVAR_GET(ue3_mouse_wide_fov_ref);
  const double fov = g_view_fov.load(std::memory_order_relaxed);
  if (ref <= 0 || !(fov > 1.0 && fov < 170.0)) return 1.0;
  const double widened_ref = ref / REXCVAR_GET(ue3_mouse_fov_comp);  // fov_comp = 100 / launcher FOV %
  const double aimzoom = fov < widened_ref * 0.4 ? REXCVAR_GET(ue3_mouse_zoom_scale)
                         : fov < widened_ref * 0.8 ? REXCVAR_GET(ue3_mouse_aim_scale) : 1.0;
  if (REXCVAR_GET(ue3_mouse_fov_constant)) return aimzoom * widened_ref / fov;  // the same angle per count everywhere
  return aimzoom * (fov > widened_ref ? widened_ref / fov : 1.0);
}
// kAimZoomOnly (0.5.0): only the aim / zoom multipliers - aiming = narrower than 80% of the walking camera, scoped (the
// Longshot, 10 deg) = narrower than 40%. Walking / other cameras are unchanged.
inline double AimZoomComp() {
  const double ref = REXCVAR_GET(ue3_mouse_wide_fov_ref), fov = g_view_fov.load(std::memory_order_relaxed);
  if (!(fov > 1.0 && fov < 170.0) || ref <= 0) return 1.0;
  return fov < ref * 0.4 ? REXCVAR_GET(ue3_mouse_zoom_scale) : fov < ref * 0.8 ? REXCVAR_GET(ue3_mouse_aim_scale) : 1.0;
}

// ---- --ue3_mouse_turn_cap -------------------------------------------------------------------------------------------
// How the console limits turning (UE3 PlayerInput; COGBox notes and the games' own config):
//  - the right stick reaches PlayerInput as aTurn / aLookUp in -1..1 (GBA_LookX / GBA_LookY: "Axis aTurn Speed=1.0",
//    "Axis aLookup Speed=1.0" in Gears 3's Xbox360-GearInput.ini), after the dead zone and the game's response curve;
//  - PlayerInput.PlayerInput scales them by TimeScale = 100 x DeltaTime and LookRightScale / LookUpScale (Gears 3:
//    300 / -250; the UE3 defaults are the same) - rotation units (65536 = 360 deg) for this frame, which
//    PlayerController.UpdateRotation adds whole. Full deflection = 100 x |LookRightScale| units a second yaw (30000 =
//    165 deg/s), 100 x |LookUpScale| pitch (25000 = 137 deg/s);
//  - Gears' own PlayerInput then slows it while targeting (Gears 3: TargetingModeViewScalePct 0.455) and has a view
//    acceleration (bViewAccelEnabled, YawAccelMultiplier 2.5 after the stick is held past YawAccelThreshold ...). The
//    acceleration's exact formula is script we have not read: it is NOT included (the cap is the stick's steady full
//    turn; a pad held over and accelerating can turn faster) - its values are logged so a pad test can settle it, and
//    --ue3_mouse_turn_cap_scale can then match it without a new build.
//  - the mouse: aMouseX / aMouseY x MouseSensitivity (60) x FOVScale (FOV / 90 when bEnableFOVScaling), added to the
//    same aTurn / aLookUp - so a sent value turns value x that many units, and the cap in value units follows.
// The scales are read from the live PlayerInput (by property name, so all four games' layouts work); a game whose
// tables can't be opened, or a property not found, uses the written config values (300 / 250 / 60, targeting 1).
struct TurnScales {
  double look_right = 300.0, look_up = 250.0;  // config values (Gears 3's Xbox360-GearInput.ini; UE3 defaults) if not read
  double mouse_sens = 60.0;                    // [Engine.PlayerInput] MouseSensitivity in every game's config
  double target_pct = 1.0;                     // Gears' TargetingModeViewScalePct (not in every game)
  bool fov_scaling = true;                     // PlayerInput.bEnableFOVScaling
  bool live = false;                           // read from the game, not the written values
};
inline std::atomic<uint32_t> g_pi{0};          // the PlayerInput object (player 1's: the first live one)
inline std::atomic<int> g_off_right{-1}, g_off_up{-1}, g_off_sens{-1}, g_off_target{-1}, g_off_fovs{-1};
inline std::atomic<uint32_t> g_mask_fovs{0};
inline std::atomic<uint64_t> g_capped_x{0}, g_capped_y{0};

inline TurnScales ReadTurnScales() {
  TurnScales t;
  const uint32_t pi = g_pi.load(std::memory_order_acquire);
  if (!pi || !ue3::obj::IsLive(pi)) return t;
  auto rd = [&](const std::atomic<int>& off, double* v, bool absolute) {
    const int o = off.load(std::memory_order_relaxed);
    if (o < 0) return false;
    const double f = ue3::obj::ReadFloat(pi + uint32_t(o));
    if (!(std::fabs(f) > 0.0 && std::fabs(f) < 1e6)) return false;  // unreadable or nonsense: keep the written value
    *v = absolute ? std::fabs(f) : f;
    return true;
  };
  const bool r = rd(g_off_right, &t.look_right, true), u = rd(g_off_up, &t.look_up, true);
  rd(g_off_sens, &t.mouse_sens, true);
  double tp = 1.0;
  if (rd(g_off_target, &tp, true) && tp <= 1.0) t.target_pct = tp;
  const int fo = g_off_fovs.load(std::memory_order_relaxed);
  const uint32_t fm = g_mask_fovs.load(std::memory_order_relaxed);
  if (fo >= 0 && fm) t.fov_scaling = (ue3::obj::Read32(pi + uint32_t(fo)) & fm) != 0;
  t.live = r && u;
  return t;
}

// Worker (only with the cap on): finds the PlayerInput and its fields' offsets by name; checked every 2 s and searched
// again only when it is gone (a new map makes a new PlayerInput). The per-frame reads are a few checked guest loads.
// Split screen: the first live PlayerInput is taken for every player (a guess - player 1's in a one-player game).
inline void TurnCapWorker(bool (*open)()) {
  bool logged_fields = false;
  uint32_t last_cls = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (!open || !open()) continue;
    const uint32_t cur = g_pi.load(std::memory_order_relaxed);
    if (cur && ue3::obj::IsLive(cur) && ue3::obj::ClassOf(cur) == last_cls) continue;   // still the same one: no scan
    const uint32_t base_cls = ue3::obj::FindClass("PlayerInput");
    if (!base_cls) continue;
    uint32_t pi = 0;
    for (uint32_t o : ue3::obj::FindInstancesOf(base_cls))
      if (ue3::obj::IsLive(o)) { pi = o; break; }
    if (!pi) { g_pi.store(0, std::memory_order_release); continue; }
    const uint32_t cls = ue3::obj::ClassOf(pi);
    if (cls != last_cls) {
      last_cls = cls;
      auto fl = [&](const char* n) {
        return ue3::obj::PropertyType(cls, n) == "FloatProperty" ? ue3::obj::PropertyOffset(cls, n) : -1;
      };
      g_off_right.store(fl("LookRightScale"));
      g_off_up.store(fl("LookUpScale"));
      g_off_sens.store(fl("MouseSensitivity"));
      g_off_target.store(fl("TargetingModeViewScalePct"));
      g_mask_fovs.store(ue3::obj::BoolMask(cls, "bEnableFOVScaling"));
      g_off_fovs.store(ue3::obj::PropertyOffset(cls, "bEnableFOVScaling"));
      if (!logged_fields) {
        logged_fields = true;
        // the view acceleration is not in the cap (see above): its values, for a pad-against-mouse test
        const int acc = ue3::obj::PropertyOffset(cls, "bViewAccelEnabled");
        const uint32_t accm = ue3::obj::BoolMask(cls, "bViewAccelEnabled");
        const int ym = fl("YawAccelMultiplier"), pm = fl("PitchAccelMultiplier");
        REXLOG_INFO("ue3_mouse_turn_cap: {} {:08X}: LookRightScale @{} LookUpScale @{} MouseSensitivity @{} "
                    "TargetingModeViewScalePct @{} bEnableFOVScaling @{} | view acceleration (not in the cap): {} yaw x{} "
                    "pitch x{}",
                    ue3::obj::ObjectName(cls), pi, g_off_right.load(), g_off_up.load(), g_off_sens.load(),
                    g_off_target.load(), g_off_fovs.load(),
                    acc >= 0 && accm ? ((ue3::obj::Read32(pi + uint32_t(acc)) & accm) ? "on" : "off") : "none",
                    ym >= 0 ? ue3::obj::ReadFloat(pi + uint32_t(ym)) : 0.0f,
                    pm >= 0 ? ue3::obj::ReadFloat(pi + uint32_t(pm)) : 0.0f);
      }
    }
    g_pi.store(pi, std::memory_order_release);
  }
}

// The stick's full-deflection turn this frame, as the most a frame's mouse value may be (value units, yaw / pitch).
// aiming: the camera is in the aim / zoom range (the same thresholds as the aim / zoom multipliers).
inline void TurnCap(double dt, double* cap_x, double* cap_y) {
  static std::atomic<bool> logged{false};
  static double last_r = 0, last_u = 0, last_t = 0;
  static int changes = 0;
  const TurnScales t = ReadTurnScales();
  const double fov = g_view_fov.load(std::memory_order_relaxed);
  double ref = REXCVAR_GET(ue3_mouse_wide_fov_ref);
  if (g_build->scaling == Build::Scaling::kFovEven) ref /= REXCVAR_GET(ue3_mouse_fov_comp);
  const bool known = fov > 1.0 && fov < 170.0;
  const bool aiming = known && ref > 0 && fov < ref * 0.8;
  const double k = 100.0 * REXCVAR_GET(ue3_mouse_turn_cap_scale) * (aiming ? t.target_pct : 1.0);
  const double yaw_ups = k * t.look_right, pitch_ups = k * t.look_up;   // rotation units per second
  // units a sent value turns: MouseSensitivity x FOV / 90 (the walking camera's FOV when none was noted yet)
  const double upv = t.mouse_sens * (t.fov_scaling ? (known ? fov : (ref > 0 ? ref : 90.0)) / 90.0 : 1.0);
  const double d = std::clamp(dt, 0.0, 0.25);
  *cap_x = upv > 0 ? yaw_ups * d / upv : 0.0;
  *cap_y = upv > 0 ? pitch_ups * d / upv : 0.0;
  if (!logged.exchange(true)) {
    REXLOG_INFO("ue3_mouse_turn_cap: on - at most {:.0f} rotation units per second yaw / {:.0f} pitch (the stick's full "
                "deflection)", 100.0 * REXCVAR_GET(ue3_mouse_turn_cap_scale) * t.look_right,
                100.0 * REXCVAR_GET(ue3_mouse_turn_cap_scale) * t.look_up);
    REXLOG_INFO("ue3_mouse_turn_cap: {} - LookRightScale {} LookUpScale {} MouseSensitivity {} targeting x{} FOV scaling {}",
                t.live ? "read from the game's PlayerInput" : "the game's PlayerInput not read yet: the written config values",
                t.look_right, t.look_up, t.mouse_sens, t.target_pct, t.fov_scaling ? "on" : "off");
    last_r = t.look_right; last_u = t.look_up; last_t = t.target_pct;
  } else if ((t.look_right != last_r || t.look_up != last_u || t.target_pct != last_t) && changes < 10) {
    ++changes;   // the game's own sensitivity setting, a new map's PlayerInput, or the first live read
    REXLOG_INFO("ue3_mouse_turn_cap: now at most {:.0f} / {:.0f} rotation units per second yaw / pitch ({}; LookRightScale "
                "{} LookUpScale {} targeting x{})", 100.0 * REXCVAR_GET(ue3_mouse_turn_cap_scale) * t.look_right,
                100.0 * REXCVAR_GET(ue3_mouse_turn_cap_scale) * t.look_up, t.live ? "the game's" : "written",
                t.look_right, t.look_up, t.target_pct);
    last_r = t.look_right; last_u = t.look_up; last_t = t.target_pct;
  }
}
inline double Clamp(double v, double cap, std::atomic<uint64_t>& capped) {
  if (std::fabs(v) <= cap) return v;
  capped.fetch_add(1, std::memory_order_relaxed);
  return std::copysign(cap, v);   // the rest is dropped, as a stick held at full turns no further
}

// dt: the game's DeltaTime for this frame (seconds), for --ue3_mouse_turn_cap.
inline bool Take(double* x, double* y, double dt) {
  const bool even = g_build->scaling == Build::Scaling::kFovEven;
  const double s = even ? REXCVAR_GET(ue3_mouse_scale) * REXCVAR_GET(ue3_mouse_fov_comp)  // GitHub #52
                              * WideCameraComp()                                              // GitHub #114
                        : REXCVAR_GET(ue3_mouse_scale) * AimZoomComp();                     // 0.5.0
  // kAimZoomOnly: the narrowest FOV is assumed for the step estimate (as these two games' copies always did)
  const double upv = ue3::MouseUnitsPerValue(even ? g_view_fov.load(std::memory_order_relaxed) : 0.0);
  const double step = REXCVAR_GET(ue3_mouse_step_units);
  const double sy = REXCVAR_GET(ue3_mouse_invert_y) ? s : -s;
  {
    std::lock_guard<std::mutex> l(g_bank_mutex);
    double vx = g_bank_dx * s, vy = g_bank_dy * sy;
    if (REXCVAR_GET(ue3_mouse_turn_cap)) {   // off (default): nothing here runs
      double cx, cy;
      TurnCap(dt, &cx, &cy);
      vx = Clamp(vx, cx, g_capped_x);
      vy = Clamp(vy, cy, g_capped_y);
    }
    *x = g_carry_x.Take(vx, upv, step);
    *y = g_carry_y.Take(vy, upv, step);
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
  if (frames) REXLOG_INFO("ue3_mouse_look: sent x={:.1f} y={:.1f} frames={} failed={} capped x={} y={}", ax, ay, frames,
                          failed, g_capped_x.exchange(0), g_capped_y.exchange(0));
  ax = ay = 0; frames = failed = 0; start = now;
}

// GitHub #420: the player the keyboard / mouse drives (the SDK's input assignment: player 1 unless split screen gives it
// to another player); the mouse's motion goes in as that player's ControllerId. No keyboard / mouse: player 1.
inline uint32_t MouseController() {
  const int u = rex::input::KeyboardMouseUser();
  return u > 0 ? uint32_t(u) : 0u;
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
  if (!g_injecting && ctx.r5.u32 == MouseController()) {  // the mouse's player, once per frame (the first event that finds banked motion)
    double x = 0, y = 0;
    uint64_t mx, my;
    if (NamesOk(base, &mx, &my) && Take(&x, &y, ctx.f2.f64)) {   // f2: the event's DeltaTime
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
  c.r5.u64 = MouseController();  // ControllerId (GitHub #420: the keyboard / mouse player)
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
  // 0.6.6 (review #30): the names first - movement banked before UE3 has made MouseX / MouseY is kept, not dropped
  if (!viewport || !NamesOk(base, &mx, &my) || !Take(&x, &y, dt)) return;
  g_d_taken.fetch_add(1, std::memory_order_relaxed);
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

// One-shot diagnostics (Build::diag - Gears 1): 6 lines at about 5 s intervals after the first entry call, whatever ue3_mouse_log says.
inline void Diag() {
  if (!g_build->diag) return;
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

// GitHub #52 / 0.5.0: each view's field of view in degrees (the projection set-up calls this once per view).
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
  window->AddListener(&g_focus_listener);   // GitHub #430: release the pointer clip on focus loss
  g_on.store(true);
  REXLOG_INFO("ue3_mouse_look: on for {} (entry {:08X})", b.name, b.entry_fn);
  if (REXCVAR_GET(ue3_mouse_turn_cap)) {   // the limit itself is logged at the first mouse motion (TurnCap)
    if (b.open_objects) std::thread(TurnCapWorker, b.open_objects).detach();
    else REXLOG_INFO("ue3_mouse_turn_cap: {} has no object tables here - the written config values are used", b.name);
  }
}

}  // namespace ue3_mouse
