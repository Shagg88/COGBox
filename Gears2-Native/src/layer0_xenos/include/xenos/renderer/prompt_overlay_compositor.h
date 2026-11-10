// Layer 0 (game-free): 4x prompt overlay compositor for Xbox, PlayStation, Switch, Steam Deck, Steam Controller and
// procedural keyboard/mouse key caps. Each style's atlases are built when first needed; only a few stay resident.
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
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

  struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
  };

  // Finds the Kenney CC0 glyph folder (`glyphs_dir`, else fallback paths next to the executable). Cheap: nothing is
  // decoded or composed here; a style's 4x overlay buffers ([width:u32, height:u32, rgba...]) are built by
  // PrepareStyle / SetActiveStyle / GetOverlay the first time that style is needed.
  bool Initialize(const std::filesystem::path& glyphs_dir = "assets/glyphs",
                  const std::map<std::string, std::string>& keybind_overrides = {});

  bool PreloadAllStyles(const std::filesystem::path& glyphs_dir = "assets/glyphs",
                        const std::map<std::string, std::string>& keybind_overrides = {}) {
    return Initialize(glyphs_dir, keybind_overrides);
  }

  bool IsInitialized() const;

  // Builds `style`'s atlases now (if not already resident), so a later SetActiveStyle is instant. Call off the render
  // thread. IsStyleReady: resident (kOriginal is always ready).
  bool PrepareStyle(DeviceStyle style);
  bool IsStyleReady(DeviceStyle style) const;

  // How many styles keep their atlases resident (default 2: keyboard + the pad in use). Older ones are dropped; a
  // buffer the texture cache still holds is taken back rather than rebuilt.
  void SetMaxResidentStyles(size_t count);

  // Returns the 4x overlay buffer ([width:u32, height:u32, rgba...]) for a cataloged or aliased prompt atlas hash,
  // building the style if needed. Returns an empty span / nullptr for `DeviceStyle::kOriginal` or unknown hashes. The
  // span stays valid only while the style is resident; hold GetOverlayShared's pointer to keep it.
  std::span<const uint8_t> GetOverlay(uint64_t hash, DeviceStyle style) const;
  std::span<const uint8_t> GetActiveOverlay(uint64_t hash, DeviceStyle style) const {
    return GetOverlay(hash, style);
  }
  std::shared_ptr<const std::vector<uint8_t>> GetOverlayShared(uint64_t hash, DeviceStyle style) const;

  // Switches the active prompt overlay style in TextureCache across all cataloged (and aliased) prompt atlas hashes.
  // `hide_disk_overlays` (kOriginal only): also hide on-disk prompt overlays (the launcher's key caps), so auto mode's
  // "game's own prompts" really shows the game's own art. Explicit "original" passes false (unchanged behaviour).
  void SetActiveStyle(DeviceStyle style, bool hide_disk_overlays = false);
  DeviceStyle ActiveStyle() const;

  // GitHub #223: the running game (its title ID). From then on a style builds only the atlases that game draws
  // (AtlasLayout::games); styles built before are dropped. Unset or an unknown title: every atlas, as before.
  void SetGameTitle(uint32_t title_id);

  // Re-composes the 4x procedural keyboard/mouse overlays using updated keybind overrides (or current cvars).
  void RebuildKeyboardOverlays(const std::map<std::string, std::string>& keybind_overrides = {});

  // Registers an additional hash (e.g., synthetic guest bytes in unit tests) as an alias of a known catalog hash.
  void RegisterAtlasAlias(uint64_t custom_hash, uint64_t catalog_hash);
  void ClearAtlasAliases();

 private:
  static constexpr int kStyleCount = 7;
  static constexpr int kKindCount = kAtlasKindCount;  // AtlasKind values (GitHub #214: + kHud512G1; #223: + 2)

  const AtlasLayout* ResolveLayoutLocked(uint64_t hash) const;
  bool KindWantedLocked(AtlasKind kind) const;  // a layout of this kind belongs to the running game (or no game set)
  void ComposeControllerStyleLocked(DeviceStyle style, const std::unordered_map<std::string, Image>& glyphs);
  void ComposeKeyboardStyleLocked(const std::map<std::string, std::string>& keybind_overrides);
  void ApplyActiveStyleLocked();
  bool EnsureStyleLocked(DeviceStyle style);
  bool StyleResidentLocked(DeviceStyle style) const;

  mutable std::mutex mutex_;
  bool initialized_ = false;
  DeviceStyle active_style_ = DeviceStyle::kOriginal;
  std::filesystem::path resolved_glyphs_dir_;
  bool hide_disk_overlays_ = false;
  uint8_t game_bits_ = 0;  // GitHub #223: AtlasLayout::games bit of the running game; 0 = build every atlas
  size_t max_resident_ = 2;
  std::vector<DeviceStyle> resident_order_;  // resident styles, least recently used first
  std::map<std::string, std::string> keybind_overrides_;
  std::shared_ptr<const std::vector<uint8_t>> overlays_[kStyleCount][kKindCount];
  std::weak_ptr<const std::vector<uint8_t>> weak_overlays_[kStyleCount][kKindCount];
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
