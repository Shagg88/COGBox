// Resolve-texture reuse (PR #43, legoliamneeson): a resolve destination is scratch - the rectangle a resolve writes is
// copied to guest memory (and to any presentation / readback snapshot) in command order - so when a guest address
// changes, a destination of the same size nobody used for a few frames can take the new key instead of destroying one
// texture and creating another (less creation / destruction and video-memory churn while streaming). Header-only.
// Our code (MIT).
#pragma once
#include <cstdint>

namespace cogbox::renderer {

// The least recently used entry of `cache` (an unordered_map of entries with .texture and .last_used) whose
// shape(entry) is (width, height), with a texture, and unused for more than kResolveReuseKeepFrames frames; end() if
// none. shape returns {0, 0} for an entry that must never be taken (the caller's presentation buffers).
inline constexpr uint64_t kResolveReuseKeepFrames = 4;  // the renderer's most frames in flight (3) plus the current one
template <class Map, class Shape>
auto ReusableResolve(Map& cache, uint64_t frame, uint32_t width, uint32_t height, Shape shape) {
  auto best = cache.end();
  if (frame <= kResolveReuseKeepFrames) return best;
  for (auto it = cache.begin(); it != cache.end(); ++it) {
    const auto& entry = it->second;
    if (!entry.texture || entry.last_used >= frame - kResolveReuseKeepFrames) continue;
    const auto dimensions = shape(entry);
    if (dimensions.first != width || dimensions.second != height) continue;
    if (best == cache.end() || entry.last_used < best->second.last_used) best = it;
  }
  return best;
}

}  // namespace cogbox::renderer
