# Achievements Part 1: Data Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every COGBox game writes its achievement list and icons to a shared cache, and one dependency-free reader
(used by the games now, the launcher in part 4) turns the caches plus each game's unlock file into per-game lists,
totals, sort orders, revealed secrets and rarity.

**Architecture:** `achievements_data.{h,cpp}` (standard C++17 only) holds the formats, the reader, totals, sorting,
the revealed list and the rarity interface. `achievements_export.{h,cpp}` (uses the ReXGlue SDK) runs in each game
at `OnPreLaunchModule`: it writes the catalog from `AchievementManager::ListAchievements()`, the icons from the
title's XDBF, and forwards unlocks to the rarity provider. The launcher passes `--cogbox_dir`.

**Tech Stack:** C++ (games: C++23 via CMake / clang; launcher: C++17 via mingw `cogbox/build.sh`), Catch2 (`gn_tests`).

**Spec:** `docs/achievements/2026-11-04-achievements-data-layer-design.md`

## Global Constraints

- `achievements_data.{h,cpp}` compile as C++17 with only the standard library (no `std::span`, `std::format`,
  `ends_with`, `<rex/...>`, ImGui): the launcher builds them with `-std=c++17` under mingw.
- Namespace `xenos::ach` for both new modules. Licence header line `// Our code (MIT).` as the other layer 0 files.
- Paths: catalog `<cogbox_dir>\achievements\<TTTTTTTT>\catalog.txt`, icons `...\<TTTTTTTT>\icons\<id>.png`, unlocks
  `<cogbox_dir>\<folder>\user\achievements\<TTTTTTTT>.toml`, revealed `<cogbox_dir>\achievements\revealed.txt`.
  `TTTTTTTT` is the title ID as 8 upper-case hex digits; `<id>` is decimal.
- Games, in this order: `Gears1` `0x4D5307D5` "Gears of War"; `Gears2` `0x4D53082D` "Gears of War 2"; `Gears3`
  `0x4D5308AB` "Gears of War 3"; `Judgment` `0x4D530A26` "Gears of War: Judgment".
- Catalog first line `# COGBox achievement catalog 1`; keys `title`, `game`, `language`, then per `[achievement]`:
  `id`, `gamerscore`, `flags`, `image`, `label`, `description`, `unachieved`. In values `\n` = line break, `\\` = backslash.
- Secret = `(flags & 0x8) == 0` (Xenia's "show unachieved"; check against real data in Task 4).
- Percentage = earned GP * 100 / total GP, rounded down; 0 when total GP is 0.
- Every file is written to `<name>.tmp` then renamed over `<name>`. COGBox never writes the SDK's unlock TOML.
- No crash on any file content: bad lines are skipped, unreadable files mean "not loaded" / "no unlocks".
- Commits: author `Flippage <10948118+Flippage@users.noreply.github.com>`, trailer
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, branch `feature/achievements`, local only (no push).

## Review Focus

- A catalog with Windows line endings (`\r\n`, e.g. a file opened and saved in Notepad) must read the same as `\n`.
- An unlock TOML whose `filetime` is missing or 0 (the SDK's migrated array form) is still "unlocked", sorted after
  timed unlocks.
- A catalog whose `[achievement]` has no `id` or `id=0` is skipped, not shown as achievement 0.
- `revealed.txt` with blank lines, lower-case hex or junk lines still loads the valid entries.
- Two achievements unlocked at the same FILETIME keep a stable order (game order, then list order) on every sort.

---

### Task 1: Catalog format, paths and the game table

**Files:**
- Create: `Gears2-Native/src/layer0_xenos/include/xenos/achievements_data.h`
- Create: `Gears2-Native/src/layer0_xenos/achievements_data.cpp`
- Create: `Gears2-Native/tests/layer0_achievements_data_test.cpp`
- Modify: `Gears2-Native/CMakeLists.txt` (the explicit `gn_layer0_xenos` source list at line ~18: add
  `src/layer0_xenos/achievements_data.cpp`; the `gn_tests` list at line ~343: add
  `tests/layer0_achievements_data_test.cpp` and `src/layer0_xenos/achievements_data.cpp`). Gears 1, 3 and Judgment
  glob `layer0_xenos/*.cpp`, so they need no change.

**Interfaces:**
- Produces:
  ```cpp
  namespace xenos::ach {
  struct GameDef { const char* folder; uint32_t title_id; const char* name; };
  extern const std::array<GameDef, 4> kGames;          // order as Global Constraints
  const GameDef* FindGame(uint32_t title_id);          // nullptr if not one of the four
  constexpr uint32_t kFlagShowUnachieved = 0x8;
  struct CatalogEntry { uint32_t id = 0, gamerscore = 0, flags = 0, image_id = 0;
                        std::string label, description, unachieved; };
  struct Catalog { uint32_t title_id = 0; std::string game; uint32_t language = 0;
                   std::vector<CatalogEntry> achievements; };
  std::string FormatCatalog(const Catalog& c);
  std::optional<Catalog> ParseCatalog(const std::string& text);   // nullopt: no header or no title
  std::filesystem::path TitleDir(const std::filesystem::path& cogbox_dir, uint32_t title_id);
  std::filesystem::path CatalogPath(const std::filesystem::path& cogbox_dir, uint32_t title_id);
  std::filesystem::path IconPath(const std::filesystem::path& cogbox_dir, uint32_t title_id, uint32_t id);
  std::filesystem::path UnlockPath(const std::filesystem::path& cogbox_dir, const GameDef& game);
  std::filesystem::path RevealedPath(const std::filesystem::path& cogbox_dir);
  bool WriteFileAtomic(const std::filesystem::path& path, const std::string& bytes);  // creates parent dirs
  bool ReadFile(const std::filesystem::path& path, std::string* out);
  std::optional<Catalog> LoadCatalog(const std::filesystem::path& cogbox_dir, uint32_t title_id);
  }
  ```

- [ ] **Step 1: Write the failing tests** (`[ach]` tag)

```cpp
TEST_CASE("ach: catalog round trip keeps every field", "[ach]") {
  xenos::ach::Catalog c{0x4D53082D, "Gears of War 2", 1, {
      {1, 10, 0x8, 3, "Line\nBreak \\ back", "Описание", ""},
      {2, 50, 0, 4, "숨겨진", "d", "Keep going"}}};
  auto back = xenos::ach::ParseCatalog(xenos::ach::FormatCatalog(c));
  REQUIRE(back);
  CHECK(back->title_id == 0x4D53082D); CHECK(back->game == "Gears of War 2"); CHECK(back->language == 1);
  REQUIRE(back->achievements.size() == 2);
  CHECK(back->achievements[0].label == "Line\nBreak \\ back");
  CHECK(back->achievements[0].description == "Описание");
  CHECK(back->achievements[0].unachieved.empty());
  CHECK(back->achievements[1].label == "숨겨진"); CHECK(back->achievements[1].gamerscore == 50);
}
TEST_CASE("ach: catalog reads CRLF, skips unknown keys and id-less entries", "[ach]") {
  auto c = xenos::ach::ParseCatalog("# COGBox achievement catalog 1\r\ntitle=4D53082D\r\nfuture=x\r\n"
                                    "[achievement]\r\nlabel=no id\r\n[achievement]\r\nid=0\r\n"
                                    "[achievement]\r\nid=7\r\nlabel=Seven\r\nnew_key=1\r\n");
  REQUIRE(c); REQUIRE(c->achievements.size() == 1);
  CHECK(c->achievements[0].id == 7); CHECK(c->achievements[0].label == "Seven");
}
TEST_CASE("ach: catalog without header or title is not loaded", "[ach]") {
  CHECK_FALSE(xenos::ach::ParseCatalog("title=4D53082D\n"));
  CHECK_FALSE(xenos::ach::ParseCatalog("# COGBox achievement catalog 1\n[achievement]\nid=1\n"));
  CHECK_FALSE(xenos::ach::ParseCatalog(std::string("\0\xff garbage", 10)));
}
TEST_CASE("ach: paths and game table", "[ach]") {
  const std::filesystem::path root = "C:/COGBox";
  CHECK(xenos::ach::CatalogPath(root, 0x4D530A26) == root / "achievements" / "4D530A26" / "catalog.txt");
  CHECK(xenos::ach::IconPath(root, 0x4D530A26, 12) == root / "achievements" / "4D530A26" / "icons" / "12.png");
  CHECK(xenos::ach::UnlockPath(root, *xenos::ach::FindGame(0x4D5308AB)) ==
        root / "Gears3" / "user" / "achievements" / "4D5308AB.toml");
  CHECK(xenos::ach::RevealedPath(root) == root / "achievements" / "revealed.txt");
  CHECK(xenos::ach::FindGame(0x12345678) == nullptr);
  CHECK(xenos::ach::kGames[0].title_id == 0x4D5307D5);
}
TEST_CASE("ach: atomic write then load", "[ach]") {
  auto dir = std::filesystem::temp_directory_path() / "cogbox_ach_test1";
  std::filesystem::remove_all(dir);
  xenos::ach::Catalog c{0x4D5307D5, "Gears of War", 1, {{5, 20, 0x8, 1, "L", "D", "U"}}};
  REQUIRE(xenos::ach::WriteFileAtomic(xenos::ach::CatalogPath(dir, c.title_id), xenos::ach::FormatCatalog(c)));
  CHECK_FALSE(std::filesystem::exists(xenos::ach::CatalogPath(dir, c.title_id).string() + ".tmp"));
  auto back = xenos::ach::LoadCatalog(dir, 0x4D5307D5);
  REQUIRE(back); CHECK(back->achievements[0].unachieved == "U");
  CHECK_FALSE(xenos::ach::LoadCatalog(dir, 0x4D53082D));
  std::filesystem::remove_all(dir);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `ninja -C Gears2-Native/build/Release gn_tests` (the PATH and ninja path from the COGBox build notes)
Expected: FAIL to compile (`xenos/achievements_data.h` not found).

- [ ] **Step 3: Implement the Task 1 interface in `achievements_data.{h,cpp}`**

Values are escaped on write and unescaped on read (`\\` -> `\`, `\n` -> line break, any other `\x` kept as is); a
`\r` before a line end is dropped. `title` is hex; numbers are decimal. Header must start with
`# COGBox achievement catalog `. `WriteFileAtomic`: write `<path>.tmp` in binary, remove `<path>`, rename.

- [ ] **Step 4: Run the tests**

Run: `ninja -C Gears2-Native/build/Release gn_tests && Gears2-Native/build/Release/gn_tests.exe "[ach]"`
Expected: `All tests passed`.

- [ ] **Step 5: Check the launcher's compiler settings accept the file**

Run: `clang++ -std=c++17 -Wall -Wextra -fsyntax-only -IGears2-Native/src/layer0_xenos/include Gears2-Native/src/layer0_xenos/achievements_data.cpp`
Expected: no output (no errors or warnings).

- [ ] **Step 6: Commit** — `Achievements: catalog format, paths and game table`

---

### Task 2: Unlock file and revealed list

**Files:**
- Modify: `Gears2-Native/src/layer0_xenos/include/xenos/achievements_data.h`, `achievements_data.cpp`
- Test: `Gears2-Native/tests/layer0_achievements_data_test.cpp`

**Interfaces:**
- Consumes: Task 1 paths, `ReadFile`, `WriteFileAtomic`.
- Produces:
  ```cpp
  using Unlocks = std::map<uint32_t, uint64_t>;          // id -> FILETIME (UTC); 0 = time unknown
  Unlocks ParseUnlocks(const std::string& toml);         // never throws; unrecognised lines skipped
  Unlocks LoadUnlocks(const std::filesystem::path& cogbox_dir, const GameDef& game);  // missing file: empty
  using RevealedSet = std::set<std::pair<uint32_t, uint32_t>>;  // (title_id, achievement id)
  RevealedSet ParseRevealed(const std::string& text);
  RevealedSet LoadRevealed(const std::filesystem::path& cogbox_dir);
  bool AddRevealed(const std::filesystem::path& cogbox_dir, uint32_t title_id, uint32_t id);  // load, insert, write
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("ach: unlocks in the SDK's current format", "[ach]") {
  auto u = xenos::ach::ParseUnlocks("# Achievement unlock state - managed by ReXGlue runtime\n\n"
                                    "[unlocked.\"3\"]\nfiletime = 133500000000000000\n\n"
                                    "[unlocked.\"12\"]\r\nfiletime = 133600000000000000\r\n");
  REQUIRE(u.size() == 2);
  CHECK(u.at(3) == 133500000000000000ull); CHECK(u.at(12) == 133600000000000000ull);
}
TEST_CASE("ach: unlocks in the old array format have no time", "[ach]") {
  auto u = xenos::ach::ParseUnlocks("unlocked = [1, 2, 9]\n");
  REQUIRE(u.size() == 3); CHECK(u.at(9) == 0);
}
TEST_CASE("ach: a damaged unlock file gives no unlocks and no crash", "[ach]") {
  CHECK(xenos::ach::ParseUnlocks("[unlocked.\"abc\"]\nfiletime = x\n\xff\xfe").empty());
  auto u = xenos::ach::ParseUnlocks("[unlocked.\"4\"]\n");   // header only: unlocked, time unknown
  REQUIRE(u.size() == 1); CHECK(u.at(4) == 0);
}
TEST_CASE("ach: revealed list round trip and junk lines", "[ach]") {
  auto dir = std::filesystem::temp_directory_path() / "cogbox_ach_test2";
  std::filesystem::remove_all(dir);
  REQUIRE(xenos::ach::AddRevealed(dir, 0x4D53082D, 15));
  REQUIRE(xenos::ach::AddRevealed(dir, 0x4D53082D, 15));     // twice: still one entry
  auto r = xenos::ach::LoadRevealed(dir);
  CHECK(r.size() == 1); CHECK(r.count({0x4D53082D, 15}) == 1);
  auto p = xenos::ach::ParseRevealed("\n4d53082d:7\njunk\n4D5308AB:\n:5\n4D5308AB:2\r\n");
  CHECK(p.size() == 2); CHECK(p.count({0x4D53082D, 7}) == 1); CHECK(p.count({0x4D5308AB, 2}) == 1);
  std::filesystem::remove_all(dir);
}
```

- [ ] **Step 2: Run to verify they fail** — same command as Task 1 Step 4. Expected: FAIL to compile.

- [ ] **Step 3: Implement the Task 2 interface**

`ParseUnlocks` works line by line (trimmed, `\r` dropped): `[unlocked."<decimal>"]` (quotes optional) starts an
entry with time 0; `filetime = <decimal>` sets the current entry's time; `unlocked = [a, b, ...]` adds each with
time 0; anything else is skipped. `revealed.txt` lines are `<hex title>:<decimal id>`, written upper-case, read
case-insensitively, written sorted.

- [ ] **Step 4: Run the tests** — Expected: `All tests passed`.
- [ ] **Step 5: Repeat Task 1 Step 5** (C++17 syntax check). Expected: no output.
- [ ] **Step 6: Commit** — `Achievements: unlock file reader and revealed list`

---

### Task 3: Per-game lists, totals, sorting and rarity

**Files:**
- Modify: `Gears2-Native/src/layer0_xenos/include/xenos/achievements_data.h`, `achievements_data.cpp`
- Test: `Gears2-Native/tests/layer0_achievements_data_test.cpp`

**Interfaces:**
- Consumes: Tasks 1-2.
- Produces:
  ```cpp
  struct Rarity { bool known = false; float percent = 0; };
  class RarityProvider {
   public:
    virtual ~RarityProvider() = default;
    virtual Rarity Get(uint32_t title_id, uint32_t achievement_id) = 0;
    virtual void ReportUnlock(uint32_t title_id, uint32_t achievement_id, uint64_t filetime) {}
  };
  class NoRarity final : public RarityProvider { public: Rarity Get(uint32_t, uint32_t) override { return {}; } };
  RarityProvider& CurrentRarityProvider();               // NoRarity unless set
  void SetRarityProvider(RarityProvider* provider);      // nullptr: back to NoRarity (the future cloud hook)

  struct Achievement {
    uint32_t title_id = 0, id = 0, gamerscore = 0;
    std::string label, description, unachieved;
    std::filesystem::path icon;                          // empty if the PNG is not there
    bool secret = false, revealed = false, unlocked = false;
    uint64_t unlocked_at = 0;                            // FILETIME UTC, 0 = unknown
    Rarity rarity;
    uint32_t game_index = 0, list_order = 0;             // kGames index, position in the catalog
  };
  struct Totals { uint32_t unlocked = 0, total = 0, gp_earned = 0, gp_total = 0; };
  uint32_t PercentByGp(const Totals& t);
  struct GameAchievements { const GameDef* game = nullptr; bool loaded = false;
                            std::vector<Achievement> achievements; Totals totals; };
  GameAchievements BuildGame(const GameDef& game, const std::optional<Catalog>& catalog, const Unlocks& unlocks,
                             const RevealedSet& revealed, RarityProvider& rarity,
                             const std::filesystem::path& cogbox_dir);   // pure join (icon: exists check only)
  GameAchievements LoadGame(const std::filesystem::path& cogbox_dir, const GameDef& game,
                            const RevealedSet& revealed, RarityProvider& rarity);
  std::vector<GameAchievements> LoadAllGames(const std::filesystem::path& cogbox_dir, RarityProvider& rarity);
  Totals SumTotals(const std::vector<GameAchievements>& games);   // loaded games only
  enum class SortOrder { kUnlockTime, kRarity };
  void SortAchievements(std::vector<Achievement>& list, SortOrder order);
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
namespace {
xenos::ach::Catalog TwoGp() {
  return {0x4D53082D, "Gears of War 2", 1, {{1, 10, 0x8, 1, "A", "", ""}, {2, 30, 0, 2, "B", "", ""},
                                            {3, 60, 0x8, 3, "C", "", ""}}};
}
struct FixedRarity : xenos::ach::RarityProvider {
  xenos::ach::Rarity Get(uint32_t, uint32_t id) override {
    if (id == 3) return {true, 2.5f};
    if (id == 1) return {true, 40.f};
    return {};
  }
};
}  // namespace
TEST_CASE("ach: join, secrets and totals", "[ach]") {
  xenos::ach::NoRarity none;
  const auto& g2 = *xenos::ach::FindGame(0x4D53082D);
  auto g = xenos::ach::BuildGame(g2, TwoGp(), {{2, 100}, {99, 5}}, {{0x4D53082D, 3}}, none, "C:/none");
  REQUIRE(g.loaded); REQUIRE(g.achievements.size() == 3);       // the unlock of unknown id 99 is ignored
  CHECK(g.achievements[1].unlocked); CHECK(g.achievements[1].secret);    // flags 0: secret
  CHECK_FALSE(g.achievements[0].secret); CHECK(g.achievements[2].revealed);
  CHECK(g.totals.unlocked == 1); CHECK(g.totals.total == 3);
  CHECK(g.totals.gp_earned == 30); CHECK(g.totals.gp_total == 100);
  CHECK(xenos::ach::PercentByGp(g.totals) == 30);
  CHECK(xenos::ach::PercentByGp({}) == 0);
}
TEST_CASE("ach: all-games totals count loaded games only", "[ach]") {
  xenos::ach::NoRarity none;
  std::vector<xenos::ach::GameAchievements> games{
      xenos::ach::BuildGame(xenos::ach::kGames[1], TwoGp(), {{3, 1}}, {}, none, "C:/none"),
      xenos::ach::BuildGame(xenos::ach::kGames[2], std::nullopt, {{1, 1}}, {}, none, "C:/none")};
  CHECK_FALSE(games[1].loaded);
  auto t = xenos::ach::SumTotals(games);
  CHECK(t.total == 3); CHECK(t.gp_earned == 60); CHECK(t.gp_total == 100);
}
TEST_CASE("ach: sort by unlock time", "[ach]") {
  xenos::ach::NoRarity none;
  auto g = xenos::ach::BuildGame(xenos::ach::kGames[1], TwoGp(), {{1, 0}, {3, 500}, {2, 500}}, {}, none, "C:/x");
  g.achievements.push_back(g.achievements[0]); g.achievements.back().unlocked = false;   // a locked one
  g.achievements.back().id = 4; g.achievements.back().list_order = 3;
  xenos::ach::SortAchievements(g.achievements, xenos::ach::SortOrder::kUnlockTime);
  // same time: list order; unknown time (0) after timed; locked last
  CHECK(g.achievements[0].id == 2); CHECK(g.achievements[1].id == 3);
  CHECK(g.achievements[2].id == 1); CHECK(g.achievements[3].id == 4);
}
TEST_CASE("ach: sort by rarity", "[ach]") {
  xenos::ach::NoRarity none; FixedRarity fixed;
  auto a = xenos::ach::BuildGame(xenos::ach::kGames[1], TwoGp(), {{1, 300}, {2, 200}, {3, 100}}, {}, none, "C:/x");
  auto b = a.achievements;
  xenos::ach::SortAchievements(a.achievements, xenos::ach::SortOrder::kRarity);
  xenos::ach::SortAchievements(b, xenos::ach::SortOrder::kUnlockTime);
  for (size_t i = 0; i < b.size(); ++i) CHECK(a.achievements[i].id == b[i].id);   // NoRarity: same as time
  auto r = xenos::ach::BuildGame(xenos::ach::kGames[1], TwoGp(), {{1, 300}, {2, 200}, {3, 100}}, {}, fixed, "C:/x");
  xenos::ach::SortAchievements(r.achievements, xenos::ach::SortOrder::kRarity);
  CHECK(r.achievements[0].id == 3); CHECK(r.achievements[1].id == 1); CHECK(r.achievements[2].id == 2);
}
TEST_CASE("ach: rarity provider defaults to none", "[ach]") {
  CHECK_FALSE(xenos::ach::CurrentRarityProvider().Get(1, 1).known);
  FixedRarity fixed; xenos::ach::SetRarityProvider(&fixed);
  CHECK(xenos::ach::CurrentRarityProvider().Get(1, 3).known);
  xenos::ach::SetRarityProvider(nullptr);
  CHECK_FALSE(xenos::ach::CurrentRarityProvider().Get(1, 3).known);
}
```

- [ ] **Step 2: Run to verify they fail** — Expected: FAIL to compile.

- [ ] **Step 3: Implement the Task 3 interface**

Sort keys (use `std::stable_sort`, then ties by `game_index`, `list_order`):

```
kUnlockTime: unlocked before locked; among unlocked, unlocked_at descending with 0 last;
             among locked, (game_index, list_order).
kRarity:     known rarity before unknown; known by percent ascending; the unknown ones in kUnlockTime order.
```

`BuildGame` with `std::nullopt` gives `loaded = false`, no achievements, zero totals. `LoadAllGames` returns all four
games in `kGames` order (loaded or not).

- [ ] **Step 4: Run the tests** — Expected: `All tests passed`.
- [ ] **Step 5: Repeat Task 1 Step 5.** Expected: no output.
- [ ] **Step 6: Commit** — `Achievements: per-game lists, totals, sorting and rarity interface`

---

### Task 4: The game writes its catalog and icons

**Files:**
- Create: `Gears2-Native/src/layer0_xenos/include/xenos/achievements_export.h`
- Create: `Gears2-Native/src/layer0_xenos/achievements_export.cpp`
- Modify: `Gears2-Native/CMakeLists.txt` (`gn_layer0_xenos` list: add `src/layer0_xenos/achievements_export.cpp`)
- Modify: `OnPreLaunchModule` in `Gears1-Native/src/layer3_gow1/gow1_app.h`, `Gears2-Native/src/layer3_gow2/gow2_app.h`,
  `Gears3-Native/src/layer3_gow3/gow3_app.h`, `GearsJ-Native/src/layer3_gowj/gowj_app.h`: first line
  `XenosExportAchievements(runtime());`

**Interfaces:**
- Consumes: Tasks 1-3 (`FindGame`, `Catalog`, `FormatCatalog`, `WriteFileAtomic`, `CatalogPath`, `IconPath`,
  `CurrentRarityProvider`).
- SDK used: `runtime->kernel_state()->title_id()`, `->achievements().ListAchievements()` (`AchievementInfo`: id,
  label, description, unachieved_description, image_id, gamerscore, flags), `->title_xdbf()` then
  `GetEntry(rex::system::util::XdbfSection::kImage, image_id)` (a block with `buffer` / `size`: the PNG bytes),
  `->achievements().RegisterUnlockCallback(cb)` (`AchievementEvent`: `achievement.id`, `unlocked_at_filetime`),
  `REXCVAR_GET(user_language)` (declared in `<rex/system/flags.h>`).
- Produces: `void XenosExportAchievements(rex::Runtime* runtime);` (global namespace, as the other `Xenos*` installs)
  and the cvar `REXCVAR_DEFINE_STRING(cogbox_dir, "", "COGBox", "The COGBox folder (set by the launcher): achievements are cached in its achievements folder")`.

- [ ] **Step 1: Implement `XenosExportAchievements`**

Does nothing (no log) when `cogbox_dir` is empty or the runtime / kernel is missing. Logs once and returns when the
title is not in `kGames`. Otherwise: builds the `Catalog` (game name from `kGames`, `language` = `user_language`)
and writes it; writes each `icons\<id>.png` whose file is missing and whose XDBF image exists; registers an unlock
callback that calls `CurrentRarityProvider().ReportUnlock(title, id, unlocked_at_filetime)`. One info line:
`achievements: <n> written to <catalog path> (<m> new icons)`; a write failure is one warning, never a stop.

- [ ] **Step 2: Build all four games**

Run (each folder's existing build dir): `ninja -C Gears2-Native/build/Release-tu6 gow2`,
`ninja -C Gears3-Native/build/Release-tu6 gow3`, `ninja -C GearsJ-Native/build/Release gowj`,
`ninja -C Gears1-Native/build/Release gow1`. Gears 1 has no build folder on this PC yet: configure one the way
the Gears 3 / Judgment ones were (read their `CMakeCache.txt` for the generator, compiler and options), or, if that
fails, say so and leave Gears 1 to be built by Shagg.
Expected: each ends `Linking CXX executable gowN.exe`, no errors.

- [ ] **Step 3: Check in a game (the player runs it; never play or control the game yourself)**

Copy one exe (back up the installed one as `.pre-ach` first) and have the player start it with
`--cogbox_dir="C:\Users\flipp\Downloads\COGBox"` added (a `.bat` next to the exe, or the launcher's extra
arguments). Expected in the log: `achievements: <n> written to ...\achievements\<TitleID>\catalog.txt`; on disk,
the catalog and `icons\*.png`. Check the secret bit against the data: the achievements the player knows are secret
must have `flags` without `0x8`. If they do not, change `kFlagShowUnachieved` and its test, and note it in the spec.

- [ ] **Step 4: Commit** — `Achievements: games write their catalog and icons (--cogbox_dir)`

---

### Task 5: The launcher passes `--cogbox_dir` and builds the reader

**Files:**
- Modify: `Gears2-Native/cogbox/src/play.cpp` (next to the `--cogbox_menu` arguments, line ~159)
- Modify: `Gears2-Native/cogbox/build.sh` (line ~34: compile `../src/layer0_xenos/achievements_data.cpp` with the
  launcher's own files; add `-I../src/layer0_xenos/include` to `FLAGS`)

**Interfaces:**
- Consumes: Task 1-3 header (unused by the launcher's UI until part 4; built now so a C++17 / mingw break shows early).

- [ ] **Step 1: Pass the argument when the game supports it**

```cpp
if (a.env[g].cap.exe_has && a.env[g].cap.exe_has("cogbox_dir"))   // achievements cache (part 1)
    args.push_back("--cogbox_dir=\"" + exe_dir() + "\"");
```

- [ ] **Step 2: Add the reader to `build.sh`** — as described in Files.

- [ ] **Step 3: Verify**

This PC has no mingw / SDL3 for `build.sh`. Run the C++17 check from Task 1 Step 5 plus
`clang++ -std=c++17 -fsyntax-only -IGears2-Native/cogbox/src -IGears2-Native/cogbox/third_party/imgui Gears2-Native/cogbox/src/play.cpp`
(if SDL headers are missing, check only that the new lines compile in context, and say so). Note in the commit
message that `build.sh` itself was not run.

- [ ] **Step 4: Commit** — `Achievements: launcher passes --cogbox_dir and builds the reader`

---

## Done when

- `gn_tests "[ach]"` passes; all four games build; one game, started with `--cogbox_dir`, wrote its catalog and
  icons; the secret bit is confirmed against real data; the launcher passes the argument.
