// Layer 2 (Unreal Engine 3 on the 360). May include layer 0-1 headers only.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <rex/logging/macros.h>
#include <xenos/display_size.h>   // layer 0: the monitor the game window is on

// The ultrawide picture-shape maths (23 Nov parity pass; first used by Gears 1's --gow1_ultrawide / --gow1_aspect).
// Gears 2 / 3 / Judgment carry the same few lines in their own game_patches.cpp (ParseAspect / MonitorAspect /
// SceneRows / the exact Hor+ factor) - they can move here without a behaviour change; not done in the same step.
namespace ue3::aspect {

// "21:9", "3440:1440" or "2.4"; 0 when unreadable.
inline double Parse(const std::string& v) {
  char* end = nullptr;
  const size_t colon = v.find(':');
  if (colon == std::string::npos) {
    const double x = std::strtod(v.c_str(), &end);
    return end == v.c_str() ? 0.0 : x;
  }
  const double w = std::strtod(v.c_str(), &end), h = std::strtod(v.c_str() + colon + 1, nullptr);
  return h > 0 ? w / h : 0.0;
}

// The monitor's shape (0 when unknown).
inline double Monitor(const char* tag) {
  uint32_t w = 0, h = 0;
  if (!xenos::display::MonitorSize(w, h) || !h) {
    REXLOG_INFO("{}: monitor size unknown", tag);
    return 0.0;
  }
  REXLOG_INFO("{}: monitor is {}x{}", tag, w, h);
  return double(w) / double(h);
}

// A scene height in whole multiples of 8 rows (the games' render targets are), 240..720 (240 = 48:9).
inline uint32_t SceneRows(double rows) { return std::clamp(uint32_t(std::lround(rows / 8.0)) * 8u, 240u, 720u); }

// 552 = Canary's 21:9 (1280x550) as a multiple of 8 (Gears 3 GitHub #330).
inline constexpr uint32_t kRows21x9 = 552;

// The 1280-wide scene's height for the option pair: `shape` "" = 21:9 when `wide`, 0 (= off) otherwise; "auto" = the
// monitor's; else Parse. A shape not wider than 16:9 is off.
inline uint32_t Rows(const char* tag, bool wide, const std::string& shape) {
  if (shape.empty()) return wide ? kRows21x9 : 0u;
  const double a = shape == "auto" ? Monitor(tag) : Parse(shape);
  if (!(a > 16.0 / 9.0 + 0.01)) {
    REXLOG_INFO("{}: '{}' = {:.4f}, not wider than 16:9 - the game's 16:9 picture is kept", tag, shape, a);
    return 0u;
  }
  const uint32_t h = SceneRows(1280.0 / a);
  REXLOG_INFO("{}: '{}' = {:.4f} -> scene 1280x{} ({:.4f})", tag, shape, a, h, 1280.0 / h);
  return h;
}

// Exact Hor+: the factor the game multiplies a horizontal FOV in degrees by to get the half angle in radians (pi / 360
// for 16:9), widened so the vertical view stays the 16:9 one: half' = atan(tan(half) * (1280 / rows) / (16 / 9)).
// Returns `game` unchanged for an FOV outside 1..170 deg.
inline double HorPlusFactor(double fov_deg, double game, uint32_t rows) {
  if (!(fov_deg > 1.0 && fov_deg < 170.0 && game > 0.0 && rows) || fov_deg * game >= 1.5) return game;
  const double widen = (1280.0 / double(rows)) / (16.0 / 9.0);
  return std::atan(std::tan(fov_deg * game) * widen) / fov_deg;
}

}  // namespace ue3::aspect
