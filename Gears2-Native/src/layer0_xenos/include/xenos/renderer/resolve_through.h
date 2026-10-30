#pragma once
#include <cstdint>

namespace xenos::renderer {
// A scaled mirror has no independent copy of untouched texels. Require complete
// coverage until partial-write ownership is tracked for these mirrors.
constexpr bool ResolveCoversTexture(uint32_t dx, uint32_t dy, uint32_t width, uint32_t height,
                                    uint32_t texture_width, uint32_t texture_height) {
  return texture_width && texture_height && !dx && !dy && width >= texture_width && height >= texture_height;
}

// GuestWatch::MarkGpuWritten increments each overlapping 4 KB page once. Any
// additional version change is an overlapping CPU/GPU write: keep the old cache
// generation so the normal loader will reconstruct the texture.
constexpr bool ResolveVersionMatches(uint64_t before, uint64_t after, uint32_t start, uint32_t length,
                                     uint32_t texture_start, uint32_t texture_length) {
  const uint64_t lo = start > texture_start ? start : texture_start;
  const uint64_t end = uint64_t(start) + length;
  const uint64_t texture_end = uint64_t(texture_start) + texture_length;
  const uint64_t hi = end < texture_end ? end : texture_end;
  return hi > lo && after == before + (((hi - 1) >> 12) - (lo >> 12) + 1);
}

constexpr bool FuseResolveSize(uint32_t width, uint32_t height, uint32_t minimum_pixels) {
  return uint64_t(width) * height >= minimum_pixels;
}
}  // namespace xenos::renderer
