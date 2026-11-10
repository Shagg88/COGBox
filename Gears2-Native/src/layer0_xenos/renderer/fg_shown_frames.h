// Layer 0 (game-free): frames that reach the screen while frame generation runs (Shagg 2026-11-10 10:40: "I'd like
// our in game FPS counter to actually tell us the correct frame count when Frame Generation is on"). The FPS bar counts
// the game's own swaps (frames drawn); with frame generation the screen gets 2x, 3x ... that. Each frame-generation back
// end (FSR D3D12 / Vulkan, XeSS-FG, DLSS-G) adds, for each frame it hands on, how many images reached the screen for it:
// 1 while generation is paused, the multiplier while it runs (DLSS-G: the count Streamline reports, so the MFG
// Unlocker's own multiplier is counted too). The bar shows that rate next to the drawn rate.
#pragma once

#include <atomic>
#include <cstdint>

inline std::atomic<uint64_t> g_xenos_fg_shown{0};       // images on screen, counted by a frame-generation back end
inline std::atomic<uint64_t> g_xenos_fg_generating{0};  // frames handed on with generation running

inline void XenosFgCountShown(uint64_t images, bool generating) {
  g_xenos_fg_shown.fetch_add(images, std::memory_order_relaxed);
  if (generating) g_xenos_fg_generating.fetch_add(1, std::memory_order_relaxed);
}
