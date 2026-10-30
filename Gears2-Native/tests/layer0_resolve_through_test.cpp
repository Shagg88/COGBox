#include <catch2/catch_test_macros.hpp>
#include <xenos/renderer/resolve_through.h>
using namespace xenos::renderer;

TEST_CASE("Direct scaled mirrors require complete origin-aligned coverage", "[resolve-through]") {
  REQUIRE(ResolveCoversTexture(0,0,1280,720,1280,720));
  REQUIRE(ResolveCoversTexture(0,0,1280,736,1280,720));
  REQUIRE_FALSE(ResolveCoversTexture(0,0,640,720,1280,720));
  REQUIRE_FALSE(ResolveCoversTexture(1,0,1280,720,1280,720));
  REQUIRE_FALSE(ResolveCoversTexture(0,1,1280,720,1280,720));
  REQUIRE_FALSE(ResolveCoversTexture(0,0,1280,720,0,720));
}
TEST_CASE("Resolve mirrors reject overlapping writes including full-coverage races", "[resolve-through]") {
  REQUIRE(ResolveVersionMatches(10,12,0x1800,0x1000,0x1000,0x2000));
  REQUIRE_FALSE(ResolveVersionMatches(10,13,0x1800,0x1000,0x1000,0x2000));
  REQUIRE_FALSE(ResolveVersionMatches(10,11,0x1800,0x1000,0x1000,0x2000));
  REQUIRE(ResolveVersionMatches(10,11,0x2000,0x1000,0x1000,0x2000));
  REQUIRE_FALSE(ResolveVersionMatches(10,10,0x3000,0x1000,0x1000,0x2000));
  REQUIRE_FALSE(ResolveVersionMatches(10,10,0x2000,0,0x1000,0x2000));
}
TEST_CASE("Selective fused resolves use guest coverage and wide area arithmetic", "[resolve-through]") {
  REQUIRE_FALSE(FuseResolveSize(16,16,65536));
  REQUIRE(FuseResolveSize(256,256,65536));
  REQUIRE(FuseResolveSize(1280,720,65536));
  REQUIRE(FuseResolveSize(65536,65536,65536));
}
