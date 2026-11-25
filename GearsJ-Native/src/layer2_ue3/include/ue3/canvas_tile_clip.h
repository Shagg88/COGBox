// Layer 2 (Unreal Engine 3): GitHub #420, Judgment's GIB / EXECUTION band side by side. The star info's event text
// slides in from the left and its band starts left of the view on purpose (GearHUDAID_Base: X = -TextSizeX -
// EventBkMarginX), so full screen it is cut by the window's edge. UCanvas::DrawTile cuts a tile at the canvas's right and
// bottom edges but not its left; side by side, player 2's view starts at the window's centre, so the band's left part
// was drawn over player 1's half (logged: x -37 +216 on a canvas of origin 0). This is the missing left cut: X / XL and
// the texture's U / UL in proportion, as the engine's own right-edge cut does. Our code (MIT).
#pragma once

namespace ue3 {

struct TileX {
  float x, xl, u, ul;
};

// x / xl: the tile's absolute left and width; u / ul: its texture range; view_left: the canvas's origin (OrgX).
inline TileX ClipTileLeft(float x, float xl, float u, float ul, float view_left) {
  TileX t{x, xl, u, ul};
  if (!(xl > 0.0f) || x >= view_left) return t;
  const float cut = view_left - x;
  if (cut >= xl) {
    t.x = view_left;
    t.xl = 0.0f;
    return t;
  }
  const float frac = cut / xl;
  t.x = view_left;
  t.xl = xl - cut;
  t.u = u + frac * ul;
  t.ul = ul - frac * ul;
  return t;
}

// GitHub #420, Judgment's Longshot zoom overlay side by side: GearHUD_Base.DrawWeaponCrosshair draws the overlay
// material over the whole view (DrawMaterialTile(OverlayMIC, ClipX, ClipY)); its art is for a 16:9 view. The material
// itself squeezes its V range by the view's share of the picture's width (with two views side by side, by 2: seen at
// 640x720, 640x552 and 640x360 - the last a 16:9 half of a 32:9 picture, still drawn with tall rings). A tile covering
// the whole view (x = the canvas origin, xl = ClipX, yl = ClipY) samples a centred U span of the view's aspect / 16:9
// (the art at the scale it has full screen, relative to the view's height) and a V span of views_across (undoing the
// material's squeeze). 640x720: U 0.5, V 2 - the values that matched the full-screen look in a live test (2026-11-23).
struct TileUV {
  float u, ul, v, vl;
};
inline TileUV FitFullViewTile16x9(float x, float xl, float yl, float u, float ul, float v, float vl, float view_left,
                                  float clip_x, float clip_y, float views_across) {
  TileUV t{u, ul, v, vl};
  if (!(clip_x > 0.0f) || !(clip_y > 0.0f) || !(views_across > 1.0f)) return t;
  const bool full = x > view_left - 0.5f && x < view_left + 0.5f && xl > clip_x - 0.5f && xl < clip_x + 0.5f &&
                    yl > clip_y - 0.5f && yl < clip_y + 0.5f;
  if (!full) return t;
  const float frac = (clip_x / clip_y) / (16.0f / 9.0f);
  t.u = u + ul * (1.0f - frac) * 0.5f;
  t.ul = ul * frac;
  t.v = v + vl * (1.0f - views_across) * 0.5f;
  t.vl = vl * views_across;
  return t;
}

}  // namespace ue3
