// Layer 3 (Gears of War 3): game-version data addresses our code writes (notes/24). GOW3_TU6 (CMake
// GN_GAME_VERSION=tu6, tools\build.ps1 -Tu6): Title Update 6 (0.0.6.11); otherwise the disc (0.0.0.11). Hook sites
// are in the manifests (gow3_manifest.toml / gow3_manifest_tu6.toml); these are the data addresses only.
// TU6 values [V]: FPS flags found through the code that reads them (same three / two readers, same instructions, at
// +0x10630 in both images); the aspect float has no direct reader (reached from a base register), so it was matched as
// the 4th of the four aligned 1.77778 constants, in the same order in both images, and agrees with the public TU6
// patch list. WriteGuestFloat checks the old value before writing either way.
#pragma once
#include <cstdint>

namespace gow3::addr {
#if defined(GOW3_TU6) && GOW3_TU6
inline constexpr uint32_t kShowFpsFlag = 0x83439898u;        // disc 0x834290D8 (+0x107C0)
inline constexpr uint32_t kShowFrameTimeFlag = 0x834398ACu;  // disc 0x834290EC
inline constexpr uint32_t kWidescreenAspect = 0x820CDED8u;   // disc 0x820CDE2C (.rdata float 1.77778)
inline constexpr uint32_t kGEngine = 0x8343987Cu;            // disc 0x834290BC (UEngine*, notes/25)
inline constexpr uint32_t kGLog = 0x833AA864u;               // disc 0x8339A498 (FOutputDevice*)
inline constexpr const char* kVersion = "TU6 (0.0.6.11)";
#else
inline constexpr uint32_t kShowFpsFlag = 0x834290D8u;
inline constexpr uint32_t kShowFrameTimeFlag = 0x834290ECu;
inline constexpr uint32_t kWidescreenAspect = 0x820CDE2Cu;
// GEngine / GLog: read in the engine's own "-EXEC=" start-up code (disc 0x829A09DC / TU6 0x829B2024: lwz r3,GEngine;
// GamePlayers at +0x4A0/+0x4A4; FExec at +0x3C, vtable slot 0; r5 = GLog), same instructions in both versions.
inline constexpr uint32_t kGEngine = 0x834290BCu;
inline constexpr uint32_t kGLog = 0x8339A498u;
inline constexpr const char* kVersion = "disc (0.0.0.11)";
#endif
inline constexpr float kWidescreenAspectValue = 16.0f / 9.0f;  // what the game has there (3FE38E4C)
}  // namespace gow3::addr
