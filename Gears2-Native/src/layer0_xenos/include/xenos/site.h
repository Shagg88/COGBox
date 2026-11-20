#pragma once
// Speed study (19 Nov): where the renderer thread is, for the sampling profiler - a plain store of (file tag << 16 |
// source line) at points along the draw path (no timers); the profiler reads it while that thread is suspended and
// lists the share of samples per site. Written by the renderer thread only. Tags: 1 shadow_renderer.cpp,
// 2 texture_cache.cpp, 3 pm4_shadow.cpp. No other includes: texture_cache.cpp is also built into the game-free tests.
#include <cstdint>

// 20 Nov: g_xenos_site_flags is ORed into the tag - 8 while an occlusion query is open on the renderer thread, so
// the query box draws' time shows apart (tag 9 = shadow_renderer.cpp inside a query, 10 = texture_cache.cpp, ...).
inline volatile uint32_t g_xenos_site = 0;
inline volatile uint32_t g_xenos_site_flags = 0;
#define XENOS_SITE(tag) (g_xenos_site = ((uint32_t(tag) | g_xenos_site_flags) << 16) | uint32_t(__LINE__))
