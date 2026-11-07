// Layer 2 (Unreal Engine 3): native mouse look's carry of the turn the engine would drop (GitHub #115, #175, #228).
// The same file is in each port's src/layer2_ue3/include/ue3 (tested in Gears2-Native/tests/layer2_mouse_carry_test.cpp).
//
// UE3's PlayerInput turns a MouseX / MouseY value into aTurn / aLookUp as value x MouseSensitivity (60.0 in every
// game's [Engine.PlayerInput]) x FOVScale (camera FOV / 90), and the PlayerController keeps only whole rotation units
// of it each frame (65536 = 360 degrees; the float is truncated into the Rotator). A frame's turn under one unit is lost.
//
// So each frame's motion goes out in whole steps of a few units, and what is left stays here for the next frames:
// nothing is lost and nothing waits longer than it takes to add up to one step. The step is in rotation units, so it
// shrinks with the sensitivity and the zoom like the turn itself. (Before #228 the motion was held until it reached
// 2 / sensitivity mouse counts, a step of the same angle at every sensitivity - with "Mouse turn when aiming: Same as
// moving around" about 0.6 degrees, 50+ pixels in the Longshot's 10 degree zoom.)
#pragma once

#include <cmath>

namespace ue3 {

constexpr double kMouseSensitivity = 60.0;   // [Engine.PlayerInput] MouseSensitivity (Gears 2, 3 and Judgment configs)
constexpr double kMouseFovScaleDivisor = 90.0;  // PlayerInput: FOVScale = FOVAngle / 90
constexpr double kMouseNarrowestFov = 10.0;  // the Longshot's ZoomFOV: used when the camera FOV isn't known
constexpr double kMouseStepMargin = 0.01;    // units added to each step against float rounding in the engine

// Rotation units the engine turns per unit of MouseX / MouseY value at this camera FOV (degrees; 0 = not known, so the
// narrowest zoom is assumed: the estimate is then low, never high, and a step can't come out under one real unit).
inline double MouseUnitsPerValue(double fov) {
  if (!(fov > 1.0 && fov < 170.0)) fov = kMouseNarrowestFov;
  return kMouseSensitivity * fov / kMouseFovScaleDivisor;
}

// One axis.
struct MouseCarry {
  double rest = 0;  // value not sent yet (less than one step)

  // Adds this frame's value (already scaled and signed for the game) and returns what to send now: whole steps of
  // step_units rotation units at units_per_value, the rest carried. step_units 0 (or no estimate): everything, now.
  double Take(double value, double units_per_value, double step_units) {
    rest += value;
    if (!(units_per_value > 0.0) || !(step_units > 0.0)) {
      const double all = rest;
      rest = 0;
      return all;
    }
    const double steps = std::trunc(rest * units_per_value / step_units);
    if (steps == 0.0) return 0.0;
    const double units = steps * step_units;
    rest -= units / units_per_value;
    return (units + std::copysign(kMouseStepMargin, units)) / units_per_value;
  }

  void Clear() { rest = 0; }
};

}  // namespace ue3
