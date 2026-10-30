// Resolve outputs are scratch: their written rectangles are copied to guest memory
// (and any presentation/readback snapshot) in command order. Our code (MIT).
#pragma once
#include <cstdint>
namespace cogbox::renderer {
template<class Map, class Shape>
auto ReusableResolve(Map& cache, uint64_t frame, uint32_t width, uint32_t height, Shape shape) {
  auto best = cache.end();
  // Keep every current display source and the last three frames (the renderer's
  // maximum frames in flight). Future resolves can reuse the native object:
  // earlier copies/readback snapshots are already recorded in queue order.
  constexpr uint64_t keep_frames = 4;
  if (frame <= keep_frames) return best;
  for (auto it = cache.begin(); it != cache.end(); ++it) {
    const auto& entry = it->second;
    if (!entry.texture || entry.last_used >= frame - keep_frames) continue;
    const auto dimensions = shape(entry);
    if (dimensions.first != width || dimensions.second != height) continue;
    if (best == cache.end() || entry.last_used < best->second.last_used) best = it;
  }
  return best;
}
}  // namespace cogbox::renderer
