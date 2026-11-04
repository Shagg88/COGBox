// Layer 0 (game-free): COGBox's achievement data (docs/achievements/2026-11-04-achievements-data-layer-design.md).
// Each game writes its achievement list to <COGBox>\achievements\<TitleID>\catalog.txt (+ icons\<id>.png); the SDK
// keeps each game's unlocks in <COGBox>\<Game>\user\achievements\<TitleID>.toml. This reads both for any of the four
// games and gives per-game lists, totals, sort orders, revealed secrets and rarity. Standard C++17 only: the launcher
// (cogbox/build.sh, mingw -std=c++17) builds this file too. Our code (MIT).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace xenos::ach {

// The four games, in COGBox's order.
struct GameDef {
  const char* folder;  // COGBox\<folder>
  uint32_t title_id;
  const char* name;
};
extern const std::array<GameDef, 4> kGames;
const GameDef* FindGame(uint32_t title_id);  // nullptr if not one of the four

// XDBF achievement flag: the description shows before the unlock. Without it the achievement is secret.
constexpr uint32_t kFlagShowUnachieved = 0x8;

// One game's achievement list, as the game wrote it (catalog.txt).
struct CatalogEntry {
  uint32_t id = 0, gamerscore = 0, flags = 0, image_id = 0;
  std::string label, description, unachieved;
};
struct Catalog {
  uint32_t title_id = 0;
  std::string game;
  uint32_t language = 0;
  std::vector<CatalogEntry> achievements;
};
std::string FormatCatalog(const Catalog& c);
std::optional<Catalog> ParseCatalog(const std::string& text);  // nullopt: no header or no title

// Paths under the COGBox folder.
std::filesystem::path TitleDir(const std::filesystem::path& cogbox_dir, uint32_t title_id);
std::filesystem::path CatalogPath(const std::filesystem::path& cogbox_dir, uint32_t title_id);
std::filesystem::path IconPath(const std::filesystem::path& cogbox_dir, uint32_t title_id, uint32_t id);
std::filesystem::path UnlockPath(const std::filesystem::path& cogbox_dir, const GameDef& game);
std::filesystem::path RevealedPath(const std::filesystem::path& cogbox_dir);

// Writes <path>.tmp, then renames it over <path> (a reader never sees half a file). Creates the parent folders.
bool WriteFileAtomic(const std::filesystem::path& path, const std::string& bytes);
bool ReadFile(const std::filesystem::path& path, std::string* out);
std::optional<Catalog> LoadCatalog(const std::filesystem::path& cogbox_dir, uint32_t title_id);

// A game's unlocks, from the SDK's file ([unlocked."<id>"] / filetime = <FILETIME>, or the old unlocked = [ids]).
using Unlocks = std::map<uint32_t, uint64_t>;  // id -> FILETIME (UTC); 0 = time unknown
Unlocks ParseUnlocks(const std::string& toml);  // never throws; lines it doesn't know are skipped
Unlocks LoadUnlocks(const std::filesystem::path& cogbox_dir, const GameDef& game);  // missing file: none

// Secret achievements the player chose to reveal (revealed.txt: <hex title>:<decimal id> per line).
using RevealedSet = std::set<std::pair<uint32_t, uint32_t>>;  // (title_id, achievement id)
RevealedSet ParseRevealed(const std::string& text);
RevealedSet LoadRevealed(const std::filesystem::path& cogbox_dir);
bool AddRevealed(const std::filesystem::path& cogbox_dir, uint32_t title_id, uint32_t id);  // load, add, write

}  // namespace xenos::ach
