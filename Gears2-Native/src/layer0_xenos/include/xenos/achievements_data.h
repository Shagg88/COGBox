// Layer 0 (game-free): COGBox's achievement data (docs/achievements/README.md).
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

// A game's unlocks, from the SDK's file ([unlocked.<id>] / filetime = <FILETIME>; quoted ids and the old
// unlocked = [ids] form are read too).
using Unlocks = std::map<uint32_t, uint64_t>;  // id -> FILETIME (UTC); 0 = time unknown
Unlocks ParseUnlocks(const std::string& toml);  // never throws; lines it doesn't know are skipped
Unlocks LoadUnlocks(const std::filesystem::path& cogbox_dir, const GameDef& game);  // missing file: none

// Secret achievements the player chose to reveal (revealed.txt: <hex title>:<decimal id> per line).
using RevealedSet = std::set<std::pair<uint32_t, uint32_t>>;  // (title_id, achievement id)
RevealedSet ParseRevealed(const std::string& text);
RevealedSet LoadRevealed(const std::filesystem::path& cogbox_dir);
bool AddRevealed(const std::filesystem::path& cogbox_dir, uint32_t title_id, uint32_t id);  // load, add, write

// Rarity: the share of players who have an achievement. Only a placeholder for now (NoRarity: always unknown); a
// cloud tracker can replace it with SetRarityProvider without the popups or the browsers changing.
struct Rarity {
  bool known = false;
  float percent = 0;  // of players who have it
};
class RarityProvider {
 public:
  virtual ~RarityProvider() = default;
  virtual Rarity Get(uint32_t title_id, uint32_t achievement_id) = 0;
  virtual void ReportUnlock(uint32_t /*title_id*/, uint32_t /*achievement_id*/, uint64_t /*filetime*/) {}
};
class NoRarity final : public RarityProvider {
 public:
  Rarity Get(uint32_t, uint32_t) override { return {}; }
};
RarityProvider& CurrentRarityProvider();           // NoRarity unless one was set
void SetRarityProvider(RarityProvider* provider);  // nullptr: back to NoRarity

// One achievement as the browsers and popups see it.
struct Achievement {
  uint32_t title_id = 0, id = 0, gamerscore = 0;
  std::string label, description, unachieved;
  std::filesystem::path icon;  // empty if the PNG is not there
  bool secret = false, revealed = false, unlocked = false;
  uint64_t unlocked_at = 0;  // FILETIME UTC, 0 = unknown
  Rarity rarity;
  uint32_t game_index = 0, list_order = 0;  // index in kGames, position in the game's catalog
};
struct Totals {
  uint32_t unlocked = 0, total = 0, gp_earned = 0, gp_total = 0;
};
uint32_t PercentByGp(const Totals& t);  // earned GP * 100 / total GP, rounded down; 0 with no GP
struct GameAchievements {
  const GameDef* game = nullptr;
  bool loaded = false;  // false: no catalog yet ("Play once to load achievements")
  std::vector<Achievement> achievements;
  Totals totals;
};
// Joins one game's catalog and unlocks (no catalog: not loaded). Icons: the PNG's path when the file exists.
GameAchievements BuildGame(const GameDef& game, const std::optional<Catalog>& catalog, const Unlocks& unlocks,
                           const RevealedSet& revealed, RarityProvider& rarity,
                           const std::filesystem::path& cogbox_dir);
GameAchievements LoadGame(const std::filesystem::path& cogbox_dir, const GameDef& game, const RevealedSet& revealed,
                          RarityProvider& rarity);
std::vector<GameAchievements> LoadAllGames(const std::filesystem::path& cogbox_dir, RarityProvider& rarity);
Totals SumTotals(const std::vector<GameAchievements>& games);  // loaded games only

enum class SortOrder { kUnlockTime, kRarity, kGameOrder, kGamerscore, kName };
// kUnlockTime: unlocked first, newest first, unknown time last among them; locked in game then catalog order.
// kRarity: known rarity first, rarest first; the rest in kUnlockTime order.
// kGameOrder: game, then catalog order. kGamerscore: highest GP first. kName: A-Z by NameSortKey. Ties in game order.
void SortAchievements(std::vector<Achievement>& list, SortOrder order);
// The key names sort by: ASCII lower case, Latin letters with accents (U+00C0-U+017F) as their base letter(s)
// ("Élite" -> "elite", "ß" -> "ss"); everything else, bad UTF-8 included, kept as its bytes.
std::string NameSortKey(const std::string& utf8);

}  // namespace xenos::ach
