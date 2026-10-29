// Progress counts must stay honest at startup phase boundaries. Our code (MIT).
#include <catch2/catch_test_macros.hpp>
#include <xenos/renderer/shader_progress.h>
#include <limits>
using namespace cogbox::shader_progress;

TEST_CASE("Shader discovery has no percentage before its total is known", "[shader-progress]") {
  ShaderPreparationProgress p;
  p.phase = ShaderPreparationPhase::kFinding;
  REQUIRE_FALSE(p.Determinate());
  REQUIRE(p.Fraction() == 0.0);
  p.phase = ShaderPreparationPhase::kCompiling;
  REQUIRE_FALSE(p.Determinate());
  REQUIRE(p.Fraction() == 0.0);
}
TEST_CASE("Package scanning progress measures packages while shader totals grow", "[shader-progress]") {
  ShaderPreparationProgress p{ShaderPreparationPhase::kScanning, 25, 100, 500, 250};
  REQUIRE(p.Determinate());
  REQUIRE(p.Fraction() == 0.25);
  p.shaders_total = 2000;
  REQUIRE(p.Fraction() == 0.25);
  p.packages_done = 101;
  REQUIRE(p.Fraction() == 1.0);
}
TEST_CASE("Shader progress clamps work handoff snapshots and reaches one hundred percent", "[shader-progress]") {
  ShaderPreparationProgress p{ShaderPreparationPhase::kCompiling, 100, 100, 200, 50};
  REQUIRE(p.ShadersDone() == 150);
  REQUIRE(p.Fraction() == 0.75);
  p.shaders_left = 201;
  REQUIRE(p.ShadersDone() == 0);
  REQUIRE(p.Fraction() == 0.0);
  p.shaders_left = 0;
  REQUIRE(p.ShadersDone() == 200);
  REQUIRE(p.Fraction() == 1.0);
}
TEST_CASE("Saving the compiled cache has its own completed progress phase", "[shader-progress]") {
  ShaderPreparationProgress p;
  p.phase = ShaderPreparationPhase::kSaving;
  REQUIRE(p.Determinate());
  REQUIRE(p.Fraction() == 1.0);
}
TEST_CASE("Shader percentage does not overflow large work counts", "[shader-progress]") {
  ShaderPreparationProgress p;
  p.phase = ShaderPreparationPhase::kCompiling;
  p.shaders_total = std::numeric_limits<uint64_t>::max();
  p.shaders_left = p.shaders_total / 2;
  REQUIRE(p.Fraction() >= 0.49);
  REQUIRE(p.Fraction() <= 0.51);
}
