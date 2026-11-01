// Layer 0 (game-free): 4x prompt overlay compositor and pre-allocated style cache for Xbox, PlayStation, Switch,
// Steam Deck, Steam Controller, and procedural Keyboard/Mouse keycaps.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <xenos/renderer/prompt_atlas_catalog.h>

namespace xenos::renderer::prompt {

enum class DeviceStyle : uint8_t {
  kOriginal = 0,
  kXbox,
  kPlayStation,
  kSwitch,
  kSteamDeck,
  kSteamController,
  kKeyboard,
};

const char* DeviceStyleName(DeviceStyle style);

class PromptOverlayCompositor {
 public:
  PromptOverlayCompositor() = default;

  // Loads Kenney CC0 controller PNGs from `glyphs_dir` (checking fallback relative/executable paths if needed) and
  // pre-composes 4x overlay buffers ([width:u32, height:u32, rgba...]) for all 4 AtlasKinds across all 6 active styles.
  bool Initialize(const std::filesystem::path& glyphs_dir = "assets/glyphs",
                  const std::map<std::string, std::string>& keybind_overrides = {});

  bool PreloadAllStyles(const std::filesystem::path& glyphs_dir = "assets/glyphs",
                        const std::map<std::string, std::string>& keybind_overrides = {}) {
    return Initialize(glyphs_dir, keybind_overrides);
  }

  bool IsInitialized() const;

  // Returns the 4x overlay buffer ([width:u32, height:u32, rgba...]) for a cataloged or aliased prompt atlas hash.
  // Returns an empty span / nullptr for `DeviceStyle::kOriginal` or unknown hashes.
  std::span<const uint8_t> GetOverlay(uint64_t hash, DeviceStyle style) const;
  std::span<const uint8_t> GetActiveOverlay(uint64_t hash, DeviceStyle style) const {
    return GetOverlay(hash, style);
  }
  std::shared_ptr<const std::vector<uint8_t>> GetOverlayShared(uint64_t hash, DeviceStyle style) const;

  // Switches the active prompt overlay style in TextureCache across all cataloged (and aliased) prompt atlas hashes.
  void SetActiveStyle(DeviceStyle style);
  DeviceStyle ActiveStyle() const;

  // Re-composes the 4x procedural keyboard/mouse overlays using updated keybind overrides (or current cvars).
  void RebuildKeyboardOverlays(const std::map<std::string, std::string>& keybind_overrides = {});

  // Registers an additional hash (e.g., synthetic guest bytes in unit tests) as an alias of a known catalog hash.
  void RegisterAtlasAlias(uint64_t custom_hash, uint64_t catalog_hash);
  void ClearAtlasAliases();

 private:
  struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
  };

  static constexpr int kStyleCount = 7;
  static constexpr int kKindCount = 4;

  const AtlasLayout* ResolveLayoutLocked(uint64_t hash) const;
  void ComposeControllerStyleLocked(DeviceStyle style, const std::unordered_map<std::string, Image>& glyphs);
  void ComposeKeyboardStyleLocked(const std::map<std::string, std::string>& keybind_overrides);
  void ApplyActiveStyleLocked();

  mutable std::mutex mutex_;
  bool initialized_ = false;
  DeviceStyle active_style_ = DeviceStyle::kOriginal;
  std::filesystem::path resolved_glyphs_dir_;
  std::shared_ptr<const std::vector<uint8_t>> overlays_[kStyleCount][kKindCount];
  std::unordered_map<uint64_t, uint64_t> hash_aliases_;
};

PromptOverlayCompositor& GlobalPromptCompositor();

bool IsPromptAtlasOrAlias(uint64_t hash);

}  // namespace xenos::renderer::prompt

namespace xenos::renderer {

using DeviceStyle = prompt::DeviceStyle;
using PromptOverlayCompositor = prompt::PromptOverlayCompositor;
using prompt::DeviceStyleName;
using prompt::GlobalPromptCompositor;

}  // namespace xenos::renderer
