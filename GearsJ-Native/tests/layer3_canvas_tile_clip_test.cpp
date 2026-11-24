// ue3/canvas_tile_clip.h: side by side, a HUD tile starting left of its view is cut at the view's left edge (GitHub
// #420, Judgment's GIB / EXECUTION band). Values from the logged tiles (split-20261123-125938.log).
#include <catch2/catch_test_macros.hpp>

#include "ue3/canvas_tile_clip.h"

using ue3::ClipTileLeft;
using ue3::TileX;

TEST_CASE("the band's body is cut at the view edge, its texture shifted to match", "[split]") {
  // x -37 +216 (EXECUTION's band), canvas origin 0: 37 of 216 are left of the view.
  TileX t = ClipTileLeft(-37.0f, 216.0f, 10.0f, 1.0f, 0.0f);
  CHECK(t.x == 0.0f);
  CHECK(t.xl == 179.0f);
  const float frac = 37.0f / 216.0f;
  CHECK(t.u == 10.0f + frac * 1.0f);
  CHECK(t.ul == 1.0f - frac * 1.0f);
}

TEST_CASE("a tile entirely left of the view gets no width", "[split]") {
  TileX t = ClipTileLeft(-40.0f, 3.0f, 156.0f, 10.0f, 0.0f);
  CHECK(t.xl == 0.0f);
}

TEST_CASE("tiles inside the view are left alone", "[split]") {
  TileX t = ClipTileLeft(0.0f, 640.0f, 0.0f, 1.0f, 0.0f);
  CHECK(t.x == 0.0f);
  CHECK(t.xl == 640.0f);
  CHECK(t.u == 0.0f);
  CHECK(t.ul == 1.0f);
  t = ClipTileLeft(700.0f, 50.0f, 3.0f, 4.0f, 640.0f);  // a canvas whose origin is the window's centre
  CHECK(t.x == 700.0f);
  CHECK(t.xl == 50.0f);
}

TEST_CASE("the view's origin is the edge, not 0", "[split]") {
  TileX t = ClipTileLeft(600.0f, 100.0f, 0.0f, 100.0f, 640.0f);
  CHECK(t.x == 640.0f);
  CHECK(t.xl == 60.0f);
  CHECK(t.u == 40.0f);
  CHECK(t.ul == 60.0f);
}

// The Longshot's zoom overlay (GearHUD_Base.DrawWeaponCrosshair: DrawMaterialTile(OverlayMIC, ClipX, ClipY)) is made for
// a 16:9 view; side by side it is drawn over a 640x720 view, its rings squeezed to half width.
using ue3::FitFullViewTile16x9;
using ue3::TileUV;

TEST_CASE("a full-view material tile on a 640x720 view: U span 0.5, V span 2, both centred", "[split]") {
  TileUV t = FitFullViewTile16x9(0.0f, 640.0f, 720.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 640.0f, 720.0f, 2.0f);
  CHECK(t.u == 0.25f);
  CHECK(t.ul == 0.5f);
  CHECK(t.v == -0.5f);
  CHECK(t.vl == 2.0f);
}

TEST_CASE("ultrawide halves: 32:9 (640x360) keeps U whole, 21:9 (640x552) narrows it; V span 2 in both", "[split]") {
  TileUV t = FitFullViewTile16x9(0.0f, 640.0f, 360.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 640.0f, 360.0f, 2.0f);
  CHECK(t.u == 0.0f);
  CHECK(t.ul == 1.0f);
  CHECK(t.vl == 2.0f);
  t = FitFullViewTile16x9(0.0f, 640.0f, 552.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 640.0f, 552.0f, 2.0f);
  CHECK(t.ul > 0.652f);
  CHECK(t.ul < 0.653f);
  CHECK(t.vl == 2.0f);
}

TEST_CASE("one view, partial tiles are left alone", "[split]") {
  TileUV t = FitFullViewTile16x9(0.0f, 1280.0f, 720.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1280.0f, 720.0f, 1.0f);
  CHECK(t.ul == 1.0f);
  CHECK(t.vl == 1.0f);
  t = FitFullViewTile16x9(10.0f, 100.0f, 100.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 640.0f, 720.0f, 2.0f);
  CHECK(t.ul == 1.0f);
  CHECK(t.vl == 1.0f);
}

TEST_CASE("the view's origin counts: player 2's canvas", "[split]") {
  TileUV t = FitFullViewTile16x9(640.0f, 640.0f, 720.0f, 0.0f, 1.0f, 0.0f, 1.0f, 640.0f, 640.0f, 720.0f, 2.0f);
  CHECK(t.u == 0.25f);
  CHECK(t.ul == 0.5f);
  CHECK(t.vl == 2.0f);
}
