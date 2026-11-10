// Layer 3 (Gears of War 2): game-version data addresses our code writes (notes/63). GOW2_TU6 (CMake GN_GAME_VERSION=tu6,
// tools\build.ps1 -Tu6): Title Update 6 (0.0.6.5); otherwise the disc (0.0.0.5). Mapped with tools/tu_remap.py from the
// job 241 image dumps (each found through the code that references it; values checked in the patched image).
#pragma once
#include <cstdint>

namespace gow2::addr {
#if defined(GOW2_TU6) && GOW2_TU6
inline constexpr uint32_t kShowFpsFlag = 0x831457B8u;        // disc 0x83123468 (+140112)
inline constexpr uint32_t kShowFrameTimeFlag = 0x831457C0u;  // disc 0x83123470
inline constexpr uint32_t kWidescreenAspect = 0x821D55A0u;   // disc 0x821D0B28 (.rdata float 1.77778)
inline constexpr uint32_t kTilingFlag = 0x831451F4u;         // disc 0x83122EA4 (log text only; the hook writes it)
inline constexpr const char* kVersion = "TU6 (0.0.6.5)";
#else
inline constexpr uint32_t kShowFpsFlag = 0x83123468u;
inline constexpr uint32_t kShowFrameTimeFlag = 0x83123470u;
inline constexpr uint32_t kWidescreenAspect = 0x821D0B28u;
inline constexpr uint32_t kTilingFlag = 0x83122EA4u;
inline constexpr const char* kVersion = "disc (0.0.0.5)";
#endif
}  // namespace gow2::addr
