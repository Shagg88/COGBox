// Startup progress presentation, independent of the renderer/UI. Our code (MIT).
#pragma once
#include <algorithm>
#include <cstdint>

namespace cogbox::shader_progress {
enum class ShaderPreparationPhase { kInactive, kFinding, kScanning, kCompiling, kSaving };
struct ShaderPreparationProgress {
  ShaderPreparationPhase phase = ShaderPreparationPhase::kInactive;
  uint64_t packages_done = 0, packages_total = 0;
  uint64_t shaders_total = 0, shaders_left = 0;
  uint64_t ShadersDone() const { return shaders_total - std::min(shaders_left, shaders_total); }
  bool Determinate() const {
    return phase == ShaderPreparationPhase::kSaving ||
           (phase == ShaderPreparationPhase::kScanning && packages_total) ||
           (phase == ShaderPreparationPhase::kCompiling && shaders_total);
  }
  double Fraction() const {
    if (phase == ShaderPreparationPhase::kSaving) return 1.0;
    if (phase == ShaderPreparationPhase::kScanning && packages_total)
      return double(std::min(packages_done, packages_total)) / double(packages_total);
    if (phase == ShaderPreparationPhase::kCompiling && shaders_total)
      return double(ShadersDone()) / double(shaders_total);
    return 0.0;
  }
};
}  // namespace cogbox::shader_progress
