# Achievements Part 4: Launcher Achievements Tab Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The COGBox launcher gets an ACHIEVEMENTS tab on each game's page (the popup settings + the achievements
browser) and an ACHIEVEMENTS button on the front screen, drawn entirely with the launcher's existing design.

**Architecture:** The popup options move to a new `gs::Tab::Achievements`, so the page's existing option-row loop draws
them. A new `achievements_tab.cpp` draws the rest from part 3's tested model (`achievements_browser_model`) with the
launcher's theme functions; `page.cpp` and `front.cpp` get a tab name and a button. The launcher is built on this PC
with llvm-mingw + SDL3 (test build, no FFmpeg).

**Tech Stack:** C++17 / MinGW (`cogbox/build.sh`), SDL3 + Dear ImGui (launcher's copy), `stb_image`, the shared
`xenos::ach` model; `settings_test` built natively with clang.

**Spec:** `docs/achievements/2026-11-04-achievements-launcher-design.md`

## Global Constraints

- The launcher's look is Shagg's: only `th_heading`, `th_button`, `th_button_red`, `th_lamp`, the launcher fonts
  (`F_BODY`, `F_SMALL`, `F_BOLD`, `F_HEAD`) and `col::` colours; no new styles.
- Tab order: DISPLAY, GRAPHICS, CONTROLS, GAMEPLAY, ACHIEVEMENTS, ABOUT (`k_tabCount` 6; ABOUT stays last).
- Option ids, scope (Shared), choices, defaults, ini keys and `--cogbox_ach_*` arguments unchanged. New labels:
  "Popup style", "Popup colour (Xbox style)", "Popup position", "Popup sound", "Hide achievement popups".
- Headings: "ACHIEVEMENT POPUPS", "ACHIEVEMENTS". Game buttons: "Gears of War", "Gears of War 2", "Gears of War 3",
  "Judgment", "All" (this page's game first, then series order, then All).
- Status line: `"<u> / <t> achievements | <e> / <g> G | <p>%"`; lamp green (1) if any unlocked, else grey (0).
- Reveal: `ask()` title "Reveal this secret achievement?", text "Its name, description and icon will be shown in every
  game and in the launcher.", buttons { "No, keep it secret", "Yes, reveal it" } (index 0 = default). Failure:
  `set_message(a, "Couldn't save - is the COGBox folder read-only?", true)`.
- Paths: the launcher's strings are UTF-8; build `std::filesystem::path` from `widen(...)` (never from a narrow
  string - MinGW would read it as the ANSI code page).
- Downloads: each one confirmed by the player first (file, source, size). Tools live outside the repo
  (`C:\Users\flipp\GearsRecomps\work\toolchains\`); nothing of them is committed.
- Test build: `COGBOX_NO_FFMPEG=1` (no menu music). Never shipped.

## Review Focus

- A COGBox folder path with non-ASCII characters (a user name with accents): catalogs, icons and `revealed.txt` still
  load and save - test in Task 3 (a path helper unit-tested natively).
- A game's exe without the popup capability (older port): the five rows show greyed with the launcher's reason, as on
  Display before - check in Task 2's `settings_test` cases (no `--cogbox_ach_*` args).
- Switching tabs or games quickly, or a game closing while the tab is shown: data reloads once, icons are not
  re-created every frame - Task 3 reload rule.
- No game added at all / only one game: the tab and the front button still draw (front button only under a box) -
  player check in Task 5.
- Translations: new labels with no translation fall back to English (the launcher's `tr()`), never empty - Task 2.

---

### Task 1: A launcher build on this PC

**Files:** none in the repo. Tools under `C:\Users\flipp\GearsRecomps\work\toolchains\`.

- [ ] **Step 1: Ask the player** for each download, naming file, source and size: llvm-mingw (UCRT, x86_64) from its
  GitHub release (or `winget install MartinStorsjo.LLVM-MinGW.UCRT`), and `SDL3-devel-<ver>-mingw.zip` from
  libsdl-org/SDL's GitHub releases (the 3.x version `build.sh`'s default path names, or the latest 3.x).
- [ ] **Step 2: Unpack** into `work\toolchains\` (each into its own new folder).
- [ ] **Step 3: Build the unchanged launcher** from Git Bash: `PATH=<llvm-mingw>/bin:$PATH SDL3_DIR=<sdl>/x86_64-w64-mingw32
  COGBOX_NO_FFMPEG=1 ./build.sh` in `Gears2-Native/cogbox`. Expected: `build/CogBox.exe` listed. If MinGW-specific
  fixes are needed, they go in the ledger as rulings and do not change `build.sh` for Shagg's Linux build.
- [ ] **Step 4: Build `settings_test` natively** (clang, as in part 2) and run `--summary`. Expected: the known
  pre-existing "FAIL ini values" only.

### Task 2: Popup settings on their own tab

**Files:**
- Modify: `Gears2-Native/cogbox/src/gears_settings.h` (`enum class Tab`), `gears_settings.cpp` (the five `add(...)`
  calls, `tab_name`)
- Test: `Gears2-Native/cogbox/test/settings_test.cpp` (`--summary` self-checks)

**Interfaces:**
- Produces: `enum class Tab { Display, Graphics, Controls, Gameplay, Achievements };`, `tab_name(Tab::Achievements) ==
  "Achievements"`.

- [ ] **Step 1: Write the failing self-check** in `--summary`: for every game, each of `AchievementStyle`,
  `AchievementColour`, `AchievementPosition`, `AchievementSound`, `HideAchievementPopups` exists, has
  `std::string(tab_name(o.tab)) == "Achievements"` and its label is the Global Constraints label; print
  `FAIL achievements tab <id>` otherwise.
- [ ] **Step 2: Build and run** `settings_test --summary`. Expected: `FAIL achievements tab ...` lines (they are on
  Display) - written via `tab_name` so it compiles before `Tab::Achievements` exists.
- [ ] **Step 3: Implement**: add `Tab::Achievements`, its `tab_name`, move the five options, set the labels.
- [ ] **Step 4: Run** `--summary` (no `FAIL achievements tab`) and the part 2 cases file (`--cases`
  `scratchpad\ach-cases.txt`): the args are byte-identical to part 2's run (ps5 / teal / top_right / sound false /
  hide true; defaults; an exe without the capability: none).
- [ ] **Step 5: Commit** `Achievements launcher: popup settings on their own tab`.

### Task 3: The ACHIEVEMENTS tab

**Files:**
- Create: `Gears2-Native/cogbox/src/achievements_tab.cpp`
- Modify: `Gears2-Native/cogbox/src/app.h` (declarations), `page.cpp` (`k_tabs`, `k_tabCount`, the Achievements
  branch), `play.cpp` (`play_poll`: reload after a game closes), `build.sh` (two shared sources)
- Test: `Gears2-Native/cogbox/test/achievements_tab_test.cpp` (native clang, like `settings_test`)

**Interfaces:**
- Consumes: part 3's `BuildSidebar`, `HeaderFor`, `BuildRows`, `EmptyMessage`, `SortName`, `ShowName`, `NextSort`
  order, `GameLabel`; part 1's `LoadAllGames`, `AddRevealed`, `ReadFile`; part 2's `FitText`.
- Produces (`app.h`):
  ```cpp
  void achievements_tab(App &a, int g);       // page.cpp: below the option rows on the Achievements tab
  void achievements_tab_reload();             // play.cpp: a game closed
  void achievements_tab_open_all();           // front.cpp: the next opening chooses All
  void achievements_reveal_answer(App &a, int answer);   // main.cpp: the "achreveal" ask() answered
  ```
  and, for the test, in `achievements_tab.cpp` behind a small header `achievements_tab_logic.h`:
  ```cpp
  std::filesystem::path cogbox_path(const std::string &utf8);   // via widen()
  std::string status_line(const xenos::ach::Totals &t);        // "<u> / <t> achievements | <e> / <g> G | <p>%"
  const char *button_label(const xenos::ach::BrowserEntry &e); // "Gears of War".."Judgment", "All"
  ```

- [ ] **Step 1: Write the failing tests** (`achievements_tab_test.cpp`, plain `main` with checks like
  `settings_test`): `status_line({2,5,20,90}) == "2 / 5 achievements | 20 / 90 G | 22%"`; `button_label` for the
  Judgment entry is "Judgment", for All "All", for Gears 2 "Gears of War 2"; `cogbox_path("C:\\Us\xC3\xA9r\\COGBox")`
  round-trips to the same UTF-8 (`path.u8string()` equals the input) and `LoadAllGames` on a scratch folder with a
  non-ASCII name containing one catalog loads it.
- [ ] **Step 2: Build natively and run.** Expected: build FAILS (`achievements_tab_logic.h` missing).
- [ ] **Step 3: Implement** the three helpers, then the tab:
  - `k_tabs` = { "DISPLAY", "GRAPHICS", "CONTROLS", "GAMEPLAY", "ACHIEVEMENTS", "ABOUT" }, `k_tabCount` 6; in the
    option-tab branch, after the rows, `if (want == gs::Tab::Achievements) achievements_tab(a, g);`.
  - State (file statics): the loaded games, selection by game index / All, sort, show, a `stale` flag, the frame the
    tab was last drawn (a gap of more than one frame = the tab was just shown → reload), the pending reveal
    (title, id), an icon map `(title,id) -> SDL_Texture*` (null = failed), freed at exit (`atexit` or the app's
    shutdown).
  - Layout: `th_heading("ACHIEVEMENT POPUPS", g_rowX1)` is drawn before the option rows of this tab (page.cpp, in the
    branch, before the loop); then `achievements_tab` draws `th_heading("ACHIEVEMENTS", g_rowX1)`, the game buttons
    (`th_button_red` for the chosen one, `th_button` for the rest, small-button size), the status line with
    `th_lamp`, Sort / Show rows drawn like `option_row` (label at `g_labelW`, combo `g_ctrlW` wide), then the rows:
    icon `T(40)` square (tinted grey when locked; "?" box), name `F_BODY` `col::text`, text `F_SMALL` `col::dim`,
    GP and status right-aligned to `g_rowX1`, tags as `F_SMALL` in `col::faint` boxes; each row a `Selectable` (nav
    and mouse), A / click on a hidden secret → `ask(a, "achreveal", g, ...)`.
  - `main.cpp`: `else if (m.tag == "achreveal") achievements_reveal_answer(a, m.answer);` (answer 1 = Yes).
  - `play_poll`: call `achievements_tab_reload()` after the process handle is closed.
  - `build.sh`: add `../src/layer0_xenos/achievements_browser_model.cpp ../src/layer0_xenos/achievements_popup_logic.cpp`
    to the source loop.
- [ ] **Step 4: Run** the test (PASS) and Task 1's launcher build (links).
- [ ] **Step 5: Commit** `Achievements launcher: the ACHIEVEMENTS tab`.

### Task 4: Front-screen button

**Files:** Modify `Gears2-Native/cogbox/src/front.cpp`

- [ ] **Step 1:** Three buttons centred (`x = cx - 1.5f * bw - gap`): PLAY / ADD..., SETTINGS, `th_button("ACHIEVEMENTS",
  ..., st.ready != R_NO_PORT, F_HEAD, a.padConnected && a.frontSel == 2)`; pressed (or A with `frontSel == 2`) →
  `a.page = g; a.tab = 4; a.zoomTo = SCR_PAGE; a.zoomStart = now; a.barSel = -1; achievements_tab_open_all();`.
  LB / RB: `frontSel = max(0, frontSel - 1)` / `min(2, frontSel + 1)`.
- [ ] **Step 2: Build** the launcher (links). Commit `Achievements launcher: front-screen button`.

### Task 5: Player check

- [ ] **Step 1:** Back up `Downloads\COGBox\settings.ini` and `CogBox.ini` (`*.pre-ach-launcher`); copy the test build
  to `Downloads\COGBox\CogBox-achievements-test.exe` (and `SDL3.dll` if the folder's is a different version: as
  `SDL3.dll` only when none is there - otherwise ask).
- [ ] **Step 2: Player** checks (spec "Building and testing"): popup settings change and save and are gone from
  Display; every game button and All; every Sort / Show; reveal No then Yes and the same secret in game; front button
  with mouse and LB / RB; a game played from the test launcher shows new unlocks after it closes; one resize; a game
  with no unlocks and Judgment not played.
- [ ] **Step 3:** Fix what the player reports (pure helpers: failing test first), rebuild, commit.
