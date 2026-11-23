// Layer 2 (Unreal Engine 3 on the 360). May include layer 0-1 headers only.
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/logging/macros.h>

// Per-object dynamic shadow cap (Shagg 22 Nov; idea from boma's Canary "Smart Shadow Cuts", made our own way), shared by
// Gears 3 (--gow3_shadow_cap) and Judgment (--gowj_shadow_cap) since the 23 Nov parity pass: both games' projected-
// shadow gather is the same function (G3 TU6 sub_827E2848, J TU4 sub_827FD2F8, 461 instructions, the same code but for
// addresses). At its `addi r11,r16,1` (r16 = shadow depth passes so far this frame, r30 = the shadow's info, its flags
// at +0x4E0 already loaded as r10 by `lwz r10,1248(r30)`), a per-object shadow (not whole-scene, not a preshadow: flag
// bits 0x06000000 clear) past the cap is skipped to the game's own skip path, as the game skips one it doesn't draw.
// Whole-scene shadows and preshadows are never capped. Canary's value: 20.
namespace ue3 {

// The instruction at the hook site, read from guest memory once: `addi r11,r16,1` (0x39700001; the 22 Nov Gears 3 code
// checked 0x396B0001, `addi r11,r11,1`, which is not what is there, so its cap never ran).
inline constexpr uint32_t kShadowCapSiteWord = 0x39700001u;

inline uint32_t ShadowCapLoadBe32(const uint8_t* membase, uint32_t guest) {
  const uint8_t* q = membase + guest;
  return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3];
}

// True = skip this shadow. `code_ok` is the caller's once-only check of the site (ShadowCapSiteOk), so the per-shadow
// cost is two compares and one guest read.
inline bool ShadowCapSkip(const char* option, int32_t cap, uint32_t passes, uint32_t info, const uint8_t* membase,
                          bool code_ok) {
  if (cap <= 0 || int32_t(passes) < cap || !code_ok || !info || !membase) return false;
  if (ShadowCapLoadBe32(membase, info + 0x4E0u) & 0x06000000u) return false;  // whole-scene or preshadow: always drawn
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    REXLOG_INFO("{}: more than {} shadow depth passes in a frame - further per-object shadows skipped", option, cap);
  return true;
}

// Once per game: the site holds the expected instruction (else logged and left alone).
inline bool ShadowCapSiteOk(const char* option, const uint8_t* membase, uint32_t site) {
  const uint32_t w = membase ? ShadowCapLoadBe32(membase, site) : 0;
  if (w != kShadowCapSiteWord)
    REXLOG_WARN("{}: the code at 0x{:08X} is {:08X}, not `addi r11,r16,1` - left alone", option, site, w);
  return w == kShadowCapSiteWord;
}

}  // namespace ue3
