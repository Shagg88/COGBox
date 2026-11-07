#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "ue3/mouse_carry.h"

namespace {

// What the engine does with a sent value: x MouseSensitivity x FOVScale, then the rotator keeps whole units (trunc).
int EngineUnits(double sent, double fov) {
  return static_cast<int>(std::trunc(static_cast<float>(sent) * ue3::MouseUnitsPerValue(fov)));
}

}  // namespace

TEST_CASE("mouse carry: units per value follow MouseSensitivity x FOV / 90", "[mouse]") {
  CHECK(ue3::MouseUnitsPerValue(90.0) == Catch::Approx(60.0));
  CHECK(ue3::MouseUnitsPerValue(10.0) == Catch::Approx(60.0 * 10.0 / 90.0));
  // No FOV noted (Gears 1 / Judgment, or none yet): the narrowest zoom, so a step is never under a unit.
  CHECK(ue3::MouseUnitsPerValue(0.0) == Catch::Approx(ue3::MouseUnitsPerValue(ue3::kMouseNarrowestFov)));
}

TEST_CASE("mouse carry: ordinary motion goes out the same frame", "[mouse]") {
  ue3::MouseCarry c;
  const double upv = ue3::MouseUnitsPerValue(80.0);  // 53.3 units per count
  const double sent = c.Take(1.0, upv, 2.0);
  CHECK(sent > 0.95);  // whole steps of 2 units: 52 of 53.3 now, the rest carried
  CHECK(EngineUnits(sent, 80.0) == 52);
}

TEST_CASE("mouse carry: one count at a low scale moves the Longshot zoom a small step, not a chunk", "[mouse]") {
  // GitHub #228: the old hold sent nothing until 2 / scale counts had added up, so the smallest step was the same
  // angle at every sensitivity. Now each count goes out in 2-unit steps.
  ue3::MouseCarry c;
  const double fov = 10.0;  // Gears 2 / Gears 3 Longshot ZoomFOV
  const double upv = ue3::MouseUnitsPerValue(fov);  // 6.67 units per count
  const double scale = 0.5;  // 3.33 units per count
  int turned = 0;
  int frames_moved = 0;
  for (int frame = 0; frame < 30; ++frame) {
    const int u = EngineUnits(c.Take(1.0 * scale, upv, 2.0), fov);
    CHECK(u <= 4);  // never a multi-count chunk
    turned += u;
    if (u) ++frames_moved;
  }
  CHECK(frames_moved == 30);  // every count arrives the frame it was made
  CHECK(std::abs(turned - 100) <= 2);  // 30 counts x 3.33 units: nothing lost
}

TEST_CASE("mouse carry: very slow zoomed motion adds up instead of being truncated away", "[mouse]") {
  // GitHub #115 / #175: under one unit per frame the engine drops it; the carry keeps it until a step is reached.
  ue3::MouseCarry c;
  const double fov = 10.0;
  const double upv = ue3::MouseUnitsPerValue(fov);
  int turned = 0;
  for (int frame = 0; frame < 300; ++frame) turned += EngineUnits(c.Take(0.05, upv, 2.0), fov);  // 0.33 units a frame
  CHECK(std::abs(turned - 100) <= 2);
}

TEST_CASE("mouse carry: both directions, and turning back cancels the carried rest", "[mouse]") {
  ue3::MouseCarry c;
  const double fov = 10.0;
  const double upv = ue3::MouseUnitsPerValue(fov);
  int turned = 0;
  for (int i = 0; i < 20; ++i) turned += EngineUnits(c.Take(-0.1, upv, 2.0), fov);
  CHECK(std::abs(turned + 13) <= 2);  // -13.3 units
  turned = 0;
  c.Take(0.1, upv, 2.0);  // 0.67 units: held
  CHECK(c.Take(-0.1, upv, 2.0) == 0.0);  // and undone, nothing sent
}

TEST_CASE("mouse carry: step 0 sends everything as it comes; Clear drops the rest", "[mouse]") {
  ue3::MouseCarry c;
  CHECK(c.Take(0.01, 6.0, 0.0) == Catch::Approx(0.01));
  CHECK(c.Take(0.1, 6.0, 2.0) == 0.0);
  c.Clear();
  CHECK(c.rest == 0.0);
}
