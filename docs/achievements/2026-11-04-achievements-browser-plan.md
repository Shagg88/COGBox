# Achievements Part 3: In-Game Browser Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An Achievements page in COGBox's in-game menu (button in F1 / Back + Start, or F7 straight to it): the
running game, the other games and "All", with totals, five sort orders, a Show filter and click-to-reveal secrets.

**Architecture:** Everything the page decides is pure C++17 in `achievements_browser_model.{h,cpp}` (unit-tested;
part 4's launcher reuses it), built on part 1's `achievements_data`. `achievements_browser.{h,cpp}` loads the data and
icons and draws the page with ImGui inside the menu window. `ingame_menu.cpp` gets a page switch, the button, F7 and
the page's controller mapping; the menu's existing pad / mouse / input-isolation code is reused unchanged.

**Tech Stack:** C++23 / clang (games), C++17 standard library only in the model, Dear ImGui (SDK, 1.92+ font API
`PushFont(font, size)`), Catch2 (`gn_tests`), SDK `rex::ui::DecodeImageRGBA` + `ImmediateDrawer::CreateTexture`.

**Spec:** `docs/achievements/2026-11-04-achievements-browser-design.md`

## Global Constraints

- Model files: standard library only (C++17), namespace `xenos::ach`, no SDK or ImGui includes.
- Left column order: running game (tagged "This game"), then the other games in `kGames` order (Gears of War, Gears
  of War 2, Gears of War 3, Gears of War: Judgment), then "All".
- Entry line: `"<unlocked> / <total> · <earned> / <total> G"`; not loaded: `"Not played yet"`.
- Header: title = game name, or `"All played games"`; counts `"<u> / <t> achievements · <e> / <g> G · <p>%"` with
  `PercentByGp`; All uses `SumTotals` (loaded games only).
- Sort names and cycle order: Unlock time → Rarity → Game order → Gamerscore → Name (A-Z) → Unlock time. Default
  Unlock time. Show: All → Unlocked → Locked → All; default All.
- Row status: `"Unlocked <d Mon yyyy, HH:MM>"` (local time, English month abbreviations, 24 h, day without leading
  zero), `"Unlocked"` when the time is unknown, `"Locked"`. GP text `"<gp> G"`.
- Hidden secret (secret, not unlocked, not revealed): name `"Secret achievement"`, text `"Select to reveal"`, no icon.
  Secret that is unlocked or revealed: normal row plus the `"Secret"` tag. Locked rows use the `unachieved` text.
- Game labels in All: `"Gears 1"`, `"Gears 2"`, `"Gears 3"`, `"Judgment"`.
- Empty results: not loaded `"Start <game name> once from COGBox to load its achievements."`; Show Unlocked
  `"No unlocked achievements yet"`; Show Locked `"Every achievement is unlocked!"`.
- Rarity text when known: `"Rare · <percent with 1 decimal>%"` only if `IsRare`; otherwise none.
- Reveal question: title `"Reveal this secret achievement?"`, text `"Its name, description and icon will be shown in
  every game and in the launcher."`, buttons `"Yes, reveal it"` / `"No, keep it secret"` (No focused). Save failure
  text: `"Couldn't save - is the COGBox folder read-only?"`.
- Hint line: `"LB / RB: game   A: select   X: sort   Y: show   B: back"`; PS prompts:
  `"L1 / R1: game   Cross: select   Square: sort   Triangle: show   Circle: back"`.
- Page buttons: `"Back to settings"`, `"Resume"` (crimson). Settings page gains `"Achievements"`.
- F7: opens the menu on the Achievements page; F7 while open closes the menu. The SDK's `bind_achievements` (F7) is
  unregistered; apps' `CreateAchievementsOverlay` returns `nullptr`.
- Icons 52 px at 1080p (x the menu's scale / 1080-based factor); only rows on screen load icons (`ImGuiListClipper`).

## Review Focus

- The running title is not one of the four (0 or unknown): the column is the four games in order plus All, nothing
  tagged "This game", no crash - test in Task 2.
- "All" with some games not loaded: their rows are absent, totals exclude them, every row has its game label - test in
  Task 2.
- A secret that is already unlocked, or revealed, is never shown as hidden - test in Task 2.
- Unlock time 0 or before 1970: `"Unlocked"` without a date, never a garbage date - test in Task 2.
- Name sort with mixed case, accents, non-Latin names and equal names: stable, ties in game order - test in Task 1.

---

### Task 1: Three more sort orders

**Files:**
- Modify: `Gears2-Native/src/layer0_xenos/include/xenos/achievements_data.h` (`enum class SortOrder`)
- Modify: `Gears2-Native/src/layer0_xenos/achievements_data.cpp` (`SortAchievements`)
- Test: `Gears2-Native/tests/layer0_achievements_data_test.cpp`

**Interfaces:**
- Produces: `enum class SortOrder { kUnlockTime, kRarity, kGameOrder, kGamerscore, kName };` and
  `std::string NameSortKey(const std::string& utf8)` (exposed for tests).

- [ ] **Step 1: Write the failing tests** (tag `[ach]`), using `Achievement` values built in the test:
  - `"SortAchievements kGameOrder: game index, then catalog order"`: input (game 2, order 1), (game 0, order 5),
    (game 0, order 2) → (0,2), (0,5), (2,1).
  - `"SortAchievements kGamerscore: highest first, ties in game order"`: GP 10 (g0 o3), 50 (g0 o4), 10 (g0 o1) →
    50, 10 (o1), 10 (o3).
  - `"SortAchievements kName: case and accents ignored, ties in game order"`: input labels (game, order)
    "beta" (0,1), "Élite" (0,2), "Alpha" (1,0), "alpha" (0,0), "Alpha" (0,9), "한글" (0,3) → "alpha" (0,0),
    "Alpha" (0,9), "Alpha" (1,0), "beta" (0,1), "Élite" (0,2), "한글" (0,3).
  - `"NameSortKey folds ASCII case and Latin accents"`: `NameSortKey("ÉLITE") == "elite"`, `NameSortKey("Ça") ==
    "ca"`, `NameSortKey("Straße") == "strasse"`, `NameSortKey("Ægis") == "aegis"`; a Korean string comes back
    unchanged (`NameSortKey("한글") == "한글"`); invalid UTF-8 bytes are kept, never dropped.
- [ ] **Step 2: Run** `ninja -C Gears2-Native/build/Release gn_tests && Gears2-Native/build/Release/gn_tests.exe "[ach]"`
  Expected: build FAILS (`kGameOrder` / `NameSortKey` not declared).
- [ ] **Step 3: Implement.** `NameSortKey`: decode UTF-8; ASCII `A-Z` → lower; U+00C0-U+017F → base letter(s) by a
  table (ß → "ss", Æ/æ → "ae", Œ/œ → "oe", Ø/ø → "o", Đ/đ/Ð/ð → "d", Þ/þ → "th", Ł/ł → "l"); everything else copied
  as its original bytes. `kGameOrder` compares `(game_index, list_order)`; `kGamerscore` GP descending then game
  order; `kName` `NameSortKey` ascending then game order. All with `std::stable_sort`.
- [ ] **Step 4: Run** the Step 2 command. Expected: `All tests passed`.
- [ ] **Step 5: Commit** `Achievements browser: game order, gamerscore and name sorting`.

### Task 2: Browser model

**Files:**
- Create: `Gears2-Native/src/layer0_xenos/include/xenos/achievements_browser_model.h`
- Create: `Gears2-Native/src/layer0_xenos/achievements_browser_model.cpp`
- Create: `Gears2-Native/tests/layer0_achievements_browser_test.cpp`
- Modify: `Gears2-Native/CMakeLists.txt` (model `.cpp` in the layer0 source list next to
  `achievements_popup_logic.cpp`; test + model `.cpp` in the `gn_tests` list next to
  `tests/layer0_achievements_popup_test.cpp`)

**Interfaces:**
- Consumes: Task 1's `SortOrder`; part 1's `GameAchievements`, `Totals`, `SumTotals`, `PercentByGp`, `kGames`,
  `SortAchievements`; part 2's `IsRare(const Rarity&)` (`achievements_popup_logic.h`).
- Produces (namespace `xenos::ach`):
  ```cpp
  enum class ShowFilter { kAll, kUnlocked, kLocked };
  SortOrder NextSort(SortOrder);  ShowFilter NextShow(ShowFilter);
  const char* SortName(SortOrder);  const char* ShowName(ShowFilter);   // "Unlock time"... / "All", "Unlocked", "Locked"
  const char* GameLabel(uint32_t title_id);                             // "Gears 1".. "Judgment", "" if unknown
  struct BrowserEntry {
    bool all = false;               // the "All" entry
    int game = -1;                  // index into the games vector (and kGames) when !all
    bool this_game = false, loaded = false;
    std::string name, line;         // "Gears of War 2" / "All"; the entry line
    Totals totals;
  };
  std::vector<BrowserEntry> BuildSidebar(const std::vector<GameAchievements>& games, uint32_t running_title);
  struct BrowserHeader { std::string title, counts; uint32_t percent = 0; };
  BrowserHeader HeaderFor(const BrowserEntry& e);
  struct BrowserRow {
    const Achievement* a = nullptr; // into `games`; valid while `games` is unchanged
    std::string name, text, gp, status, game_label, rarity;
    bool locked = false, hidden_secret = false, secret_tag = false;
  };
  // utc_offset_minutes(unix_seconds) -> the PC's offset from UTC at that moment (the drawing passes localtime's)
  std::vector<BrowserRow> BuildRows(const std::vector<GameAchievements>& games, const BrowserEntry& e, SortOrder s,
                                    ShowFilter f, const std::function<int(int64_t)>& utc_offset_minutes);
  std::string FormatUnlockTime(uint64_t filetime, int utc_offset_minutes);  // "" if 0 or before 1970
  std::string EmptyMessage(const BrowserEntry& e, ShowFilter f);            // "" when rows exist is the caller's call
  const char* HintLine(bool ps_prompts);
  ```
  `games` is `LoadAllGames`' result: index i is `kGames[i]`.

- [ ] **Step 1: Write the failing tests** (file `layer0_achievements_browser_test.cpp`, tag `[ach]`), with a helper
  building a 4-entry `games` vector (Gears 1 loaded: 3 achievements, one a locked secret; Gears 2 loaded: 2; Gears 3
  and Judgment not loaded):
  - `"BuildSidebar: running game first and tagged, others in order, All last"`: running `0x4D53082D` → names
    "Gears of War 2" (this_game), "Gears of War", "Gears of War 3", "Gears of War: Judgment", "All"; Judgment's
    line "Not played yet"; Gears 2's line e.g. `"1 / 2 · 10 / 30 G"` (from the helper's values; `·` is U+00B7).
  - `"BuildSidebar: unknown running title"`: running 0 → the four in `kGames` order, none `this_game`, then All.
  - `"HeaderFor All uses loaded games only"`: All's totals = Gears 1 + Gears 2; title "All played games"; counts
    string exact.
  - `"BuildRows All: only loaded games, each with its label"`: 5 rows, every `game_label` non-empty, labels
    "Gears 1" / "Gears 2".
  - `"BuildRows: hidden secret"`: the locked unrevealed secret → `hidden_secret`, name "Secret achievement", text
    "Select to reveal", gp "<gp> G", status "Locked".
  - `"BuildRows: unlocked or revealed secret is not hidden"`: same achievement with `unlocked=true` and, separately,
    `revealed=true` → `!hidden_secret`, `secret_tag`, real name.
  - `"BuildRows: locked rows use the unachieved text"`.
  - `"BuildRows: Show filter"`: Unlocked → only unlocked rows; Locked → only locked.
  - `"FormatUnlockTime"`: FILETIME for 2026-11-04 20:10:00 UTC, offset 0 → `"4 Nov 2026, 20:10"`; offset +60 →
    `"4 Nov 2026, 21:10"`; offset -1290 crosses the date back to `"3 Nov 2026, ..."`; 0 → `""`; FILETIME for
    1601-06-01 → `""`.
  - `"BuildRows: unknown unlock time"`: unlocked with `unlocked_at = 0` → status `"Unlocked"`.
  - `"BuildRows: rarity text only when rare"`: known 4.25 → `"Rare · 4.3%"` (`%.1f`); known 30 → `""`; unknown → `""`.
  - `"EmptyMessage"`: not loaded Judgment → `"Start Gears of War: Judgment once from COGBox to load its
    achievements."`; Unlocked → `"No unlocked achievements yet"`; Locked → `"Every achievement is unlocked!"`.
  - `"NextSort / NextShow cycle"`: five steps from kUnlockTime return to it via the spec order; three for Show.
  - `"HintLine"`: both exact strings.
- [ ] **Step 2: Run** the Task 1 command. Expected: build FAILS (`achievements_browser_model.h` not found).
- [ ] **Step 3: Implement** the model. FILETIME → Unix: `filetime / 10'000'000 - 11'644'473'600`; date from days
  with the civil-from-days algorithm (no `localtime` in the model). Rows: collect from the entry's game (or every
  loaded game for All), filter, `SortAchievements`, then fill the text fields.
- [ ] **Step 4: Run.** Expected: `All tests passed`.
- [ ] **Step 5: Commit** `Achievements browser: model (columns, header, rows, text)`.

### Task 3: Browser page (data, icons, drawing)

**Files:**
- Create: `Gears2-Native/src/layer0_xenos/include/xenos/achievements_browser.h`
- Create: `Gears2-Native/src/layer0_xenos/achievements_browser.cpp`
- Modify: `Gears2-Native/src/layer0_xenos/include/xenos/achievements_export.h` / `achievements_export.cpp`
  (share the SDK → catalog conversion)
- Modify: `Gears2-Native/CMakeLists.txt` (browser `.cpp` in the layer0 list)

**Interfaces:**
- Consumes: Task 2's model; `XenosCogboxDir()`, `XenosAchievementFont(bool)`, `kAchievementFontPixels`
  (`achievement_popup_assets.h`); `FitText` (`achievements_popup_logic.h`); `LoadAllGames`, `BuildGame`,
  `LoadRevealed`, `AddRevealed`, `CurrentRarityProvider`, `FindGame`.
- Produces:
  ```cpp
  // achievements_export.h
  xenos::ach::Catalog XenosCatalogFromRuntime(rex::Runtime* runtime);   // the running title's list (may be empty)
  xenos::ach::Unlocks XenosUnlocksFromRuntime(rex::Runtime* runtime);   // id -> GetUnlockTime for unlocked ids
  // achievements_browser.h
  namespace xenos {
  class AchievementsBrowser {
   public:
    AchievementsBrowser(rex::ui::ImmediateDrawer* immediate, rex::Runtime* runtime);  // registers an unlock callback
    ~AchievementsBrowser();                                   // unregisters it
    void Reload();                                            // call when the page is shown
    // Draws the page into the current window, `height` tall. Returns the selected row's full name + text (the menu's
    // description line).
    std::string Draw(float scale, float height);
    void Previous(); void Next();                             // LB / RB
    void CycleSort(); void CycleShow();                       // X / Y
    bool RevealOpen() const;                                  // the reveal question is showing (B means No)
  };
  }
  ```
  Selection, sort and Show are members, so they last as long as the object (the menu owns one per game run, see
  Task 4).

- [ ] **Step 1: Move** the `ListAchievements` → `Catalog` loop out of `XenosExportAchievements` into
  `XenosCatalogFromRuntime` (title id, game name, language, entries) and use it there; add
  `XenosUnlocksFromRuntime`. Build `gow2` (`ninja -C Gears2-Native/build/Release-tu6`). Expected: builds; `gn_tests
  "[ach]"` still passes.
- [ ] **Step 2: Implement `AchievementsBrowser`.**
  - `Reload`: `LoadAllGames(XenosCogboxDir(), CurrentRarityProvider())`; if the running game (`FindGame(title)`) is
    not loaded, replace its entry with `BuildGame(*game, XenosCatalogFromRuntime(rt), XenosUnlocksFromRuntime(rt),
    LoadRevealed(dir), CurrentRarityProvider(), dir)`; rebuild the sidebar; keep the selected entry by game (default:
    the running game, else the first entry); clear `stale_`.
  - Unlock callback (`kernel_state()->achievements().RegisterUnlockCallback`) sets `std::atomic<bool> stale_`; `Draw`
    calls `Reload` first when it is set.
  - Layout: left child 240 px x s with one selectable per entry (name, "This game" tag, progress bar
    `ImGui::ProgressBar`, line); right child: header (title, counts, bar, Sort and Show combos), then the rows child
    with `ImGuiListClipper`. Row height 64 px x s; icon 52 px x s, greyed (tint 0.45) when locked; "?" box for a
    hidden secret; name / text with `FitText` against the row's free width; GP, status and rarity right-aligned; tags
    as small filled rects. Text uses `XenosAchievementFont` via `ImGui::PushFont(font, size)` when it is non-null.
    Empty rows → `EmptyMessage`, centred, dim.
  - Icons: running game → `rex::ui::AchievementIconCache::GetIcon` (needs an `AchievementInfo` with `id` and
    `image_id`, from `FindAchievement`); others → read `Achievement::icon` with `ach::ReadFile`,
    `rex::ui::DecodeImageRGBA`, `ImmediateDrawer::CreateTexture(w, h, kLinear, false, rgba)`; cached in a
    `std::map<std::pair<uint32_t,uint32_t>, std::unique_ptr<ImmediateTexture>>` (a failed load is cached as null).
  - Selecting a hidden secret (click or A on the row) opens popup `"##reveal"` with the spec texts; "No" has
    `SetItemDefaultFocus`; "Yes" → `AddRevealed`; success → close + `Reload`; failure → show the save-failure text,
    `REXLOG_WARN`.
  - `utc_offset_minutes`: `localtime_s` vs `gmtime_s` of the Unix time, difference in minutes.
- [ ] **Step 3: Build** all four games (commands as in part 2: `ninja -C` each build dir). Expected: all link.
  (The page is not reachable until Task 4.)
- [ ] **Step 4: Commit** `Achievements browser: page drawing, icons and live data`.

### Task 4: Menu page, F7 and the four apps

**Files:**
- Modify: `Gears2-Native/src/layer0_xenos/ingame_menu.cpp`
- Modify: `Gears2-Native/src/layer0_xenos/include/xenos/dev_tools.h` (`XenosInstallInGameMenu` signature)
- Modify: `Gears2-Native/src/layer3_gow2/gow2_app.h`, `Gears3-Native/src/layer3_gow3/gow3_app.h`,
  `GearsJ-Native/src/layer3_gowj/gowj_app.h`, `Gears1-Native/src/layer3_gow1/gow1_app.h`

**Interfaces:**
- Consumes: Task 3's `AchievementsBrowser`.
- Produces: `void XenosInstallInGameMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
  rex::ui::Window* window, rex::Runtime* runtime);`

- [ ] **Step 1: Menu.**
  - `XenosInstallInGameMenu` takes `immediate`; keeps it and the runtime in globals; creates the browser lazily on
    first use (`std::unique_ptr<xenos::AchievementsBrowser> g_browser`, UI thread), reset in
    `XenosShutdownInGameMenu`.
  - `InGameMenu` gets `enum class Page { kSettings, kAchievements } page_`; `OpenNow(Page)` (default kSettings).
    Opening or switching to the Achievements page calls `g_browser->Reload()`.
  - Settings page: an "Achievements" button joins the bar (4 equal buttons: Resume, Achievements, Restart to apply,
    Quit game; in the no-file case the same row with Restart disabled as today).
  - Achievements page: title line hint = `HintLine(cogbox_menu_ps)`; `g_browser->Draw(scale, available height minus
    the bar)`; the description line shows its return value; bar = "Back to settings", "Resume" (crimson).
  - `FeedPad` on the Achievements page: LB / RB → `Previous` / `Next`; X → `CycleSort`; Y → `CycleShow`; B / Esc →
    back to the settings page unless a popup is open (popup: ImGui's own B closes it = No); Start still resumes.
    Settings page: unchanged.
  - F7: in `XenosInstallInGameMenu`, `rex::ui::UnregisterBind("bind_achievements")`, then
    `RegisterBind("bind_cogbox_achievements", "F7", "COGBox achievements", ...)`: closed → `OpenNow(kAchievements)`;
    open → close as Resume does. Unregister it in `XenosShutdownInGameMenu`.
- [ ] **Step 2: Apps.** Each `OnCreateDialogs` passes `immediate_drawer()`; each app overrides
  `std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override { return nullptr; }`.
- [ ] **Step 3: Build** all four games and `gn_tests "[ach]"`. Expected: all link; tests pass.
- [ ] **Step 4: Commit** `Achievements browser: menu page, F7 and the four games`.

### Task 5: In-game check (player)

- [ ] **Step 1:** Deploy the four exes (keep `*.pre-browser` backups) and give the player a `.bat` per game (the
  launcher's last arguments + `--cogbox_dir`).
- [ ] **Step 2: Player** checks, in two games: F1 → Achievements, F7 open / close; controller (LB / RB, X, Y, A, B)
  and mouse; each sort and Show; All; Judgment's "Not played yet"; a reveal of Gears 3 "Respect for the Dead"
  (id 19) with No, then Yes (back up `revealed.txt` first if wanted); the window at a second size; a real unlock
  while open, if one comes.
- [ ] **Step 3:** Fix what the player reports (pure logic: failing test first), rebuild, commit.
