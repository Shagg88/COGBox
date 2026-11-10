// Layer 0 unit tests (Catch2, decision D17): Xenos number formats and the tiled address.
// Expected values derived from Xenia (xenia-canary), Copyright (c) Ben Vanik and Xenia project contributors,
// BSD 3-Clause License (notes/31, research brief 3).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include <xenos/formats.h>

using namespace xenos::formats;
using Catch::Approx;

TEST_CASE("7e3 to float32", "[xenos][7e3]") {
  CHECK(Float7e3To32(0x000) == 0.0f);
  CHECK(Float7e3To32(0x001) == 0.001953125f);
  CHECK(Float7e3To32(0x07F) == 0.248046875f);
  CHECK(Float7e3To32(0x080) == 0.25f);
  CHECK(Float7e3To32(0x0C0) == 0.375f);
  CHECK(Float7e3To32(0x200) == 2.0f);
  CHECK(Float7e3To32(0x380) == 16.0f);
  CHECK(Float7e3To32(0x3FF) == 31.875f);
}

TEST_CASE("float32 to 7e3 (clamped, round to nearest even)", "[xenos][7e3]") {
  CHECK(Float32To7e3(0.0f) == 0x000);
  CHECK(Float32To7e3(0.25f) == 0x080);
  CHECK(Float32To7e3(1.0f) == 0x180);
  CHECK(Float32To7e3(1.5f) == 0x1C0);
  CHECK(Float32To7e3(0.1f) == 0x033);
  CHECK(Float7e3To32(0x033) == 0.099609375f);
  CHECK(Float32To7e3(0.0078125f) == 0x004);
  CHECK(Float32To7e3(40.0f) == 0x3FF);
  CHECK(Float32To7e3(std::numeric_limits<float>::infinity()) == 0x3FF);
  CHECK(Float32To7e3(-1.0f) == 0x000);
  CHECK(Float32To7e3(std::numeric_limits<float>::quiet_NaN()) == 0x000);
}

TEST_CASE("7e3 round trip is exact for every encoding", "[xenos][7e3]") {
  for (uint32_t v = 0; v < 0x400; ++v) {
    INFO("value 0x" << std::hex << v);
    CHECK(Float32To7e3(Float7e3To32(v)) == v);
  }
}

TEST_CASE("float32 to 20e4 depth", "[xenos][20e4]") {
  CHECK(Float32To20e4(0.5f, true) == 0xE00000);
  CHECK(Float32To20e4(1.0f, true) == 0xF00000);
  CHECK(Float32To20e4(1.5f, true) == 0xF80000);
  CHECK(Float32To20e4(0.3f, true) == 0xD33333);
  CHECK(Float32To20e4(1.99999f, true) == 0xFFFFF6);
  CHECK(Float32To20e4(1.99999f, false) == 0xFFFFF5);
  CHECK(Float32To20e4(1e-5f, true) == 0x029F17);
  CHECK(Float32To20e4(1e-5f, false) == 0x029F16);
  CHECK(Float32To20e4(2.0f, true) == 0xFFFFFF);
  CHECK(Float32To20e4(-1.0f, true) == 0);
  CHECK(Float32To20e4(std::numeric_limits<float>::quiet_NaN(), true) == 0);
}

TEST_CASE("20e4 to float32", "[xenos][20e4]") {
  CHECK(Float20e4To32(0x000001) == Approx(5.82076609e-11f).epsilon(1e-7));
  CHECK(Float20e4To32(0x0FFFFF) == Approx(6.1035098e-05f).epsilon(1e-7));
  CHECK(Float20e4To32(0x100000) == Approx(6.10351562e-05f).epsilon(1e-7));
  CHECK(Float20e4To32(0x700000) == 0.00390625f);
  CHECK(Float20e4To32(0xF00000) == 1.0f);
  CHECK(Float20e4To32(0xFFFFFF) == Approx(1.99999905f).epsilon(1e-7));
}

TEST_CASE("k_2_10_10_10 unpack", "[xenos][rgb10a2]") {
  auto u = UnpackR10G10B10A2UNorm(0xC00003FF);
  CHECK(u.r == 1.0f); CHECK(u.g == 0.0f); CHECK(u.b == 0.0f); CHECK(u.a == 1.0f);
  auto f = UnpackR10G10B10A2Float(0xC00003FF);
  CHECK(f.r == 31.875f); CHECK(f.a == 1.0f);
  u = UnpackR10G10B10A2UNorm(0x40100401);
  CHECK(u.r == Approx(0.000977517f)); CHECK(u.g == Approx(0.000977517f)); CHECK(u.b == Approx(0.000977517f));
  CHECK(u.a == Approx(0.333333f));
  f = UnpackR10G10B10A2Float(0x40100401);
  CHECK(f.r == 0.001953125f); CHECK(f.g == 0.001953125f); CHECK(f.b == 0.001953125f);
  u = UnpackR10G10B10A2UNorm(0xFFFFFFFF);
  CHECK(u.r == 1.0f); CHECK(u.g == 1.0f); CHECK(u.b == 1.0f); CHECK(u.a == 1.0f);
}

TEST_CASE("Tiled2D addresses", "[xenos][tiling]") {
  // Pitch 1280, 32 bpp.
  CHECK(Tiled2D(0, 0, 1280, 2) == 0);
  CHECK(Tiled2D(1, 0, 1280, 2) == 4);
  CHECK(Tiled2D(7, 0, 1280, 2) == 44);
  CHECK(Tiled2D(8, 0, 1280, 2) == 64);
  CHECK(Tiled2D(0, 1, 1280, 2) == 16);
  CHECK(Tiled2D(0, 2, 1280, 2) == 256);
  CHECK(Tiled2D(0, 8, 1280, 2) == 1152);
  CHECK(Tiled2D(0, 16, 1280, 2) == 2048);
  CHECK(Tiled2D(32, 0, 1280, 2) == 4096);
  CHECK(Tiled2D(0, 32, 1280, 2) == 163840);
  CHECK(Tiled2D(100, 50, 1280, 2) == 178464);
  // Pitch 32, DXT1 (8 bytes per block).
  CHECK(Tiled2D(1, 0, 32, 3) == 8);
  CHECK(Tiled2D(0, 1, 32, 3) == 16);
  CHECK(Tiled2D(31, 31, 32, 3) == 8056);
}
