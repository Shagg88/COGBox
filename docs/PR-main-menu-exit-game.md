# PR: Main Menu "Exit Game" Support for Gears 2 TU6, Gears 3 TU6, and Judgment TU4

## Summary

This pull request introduces native **"EXIT GAME"** menu options and instant process termination across the front-end menus of **Gears of War 2 (TU6)**, **Gears of War 3 (TU6)**, and **Gears of War: Judgment (TU4)** in COGBox.

### Problem & Motivation
In the original Xbox 360 Unreal Engine 3 builds, title shutdown sequences were designed for console lifecycle events (rebooting to dashboard, dashboard blades, or title switching). In recompilation environments, attempting a standard guest engine shutdown loop frequently causes application hangs, freezes, or thread deadlocks as guest threads await synchronization with UI and rendering loops that are tearing down. Furthermore, the PC ports lacked an intuitive way to quit directly from the main menu without resorting to `Alt+F4`, window close buttons, or navigating secondary overlays.

### Solution Overview
1. **Menu Item Repurposing via In-Memory Coalesced Overrides (`layer2_ue3`):**
   - Repurposes obsolete/defunct online menu slots in the coalesced configuration bundles (`Coalesced_*.bin`) at startup.
   - Localized into **12 languages** (`INT`, `FRA`, `DEU`, `ITA`, `ESN`, `ESM`, `PTB`, `POL`, `RUS`, `CZE`, `HUN`, `KOR`, `CHN`, `CHT`) matching native casing and font styling.
2. **Deterministic Guest Trigger Interception (`layer3_*`):**
   - Hooks into the game-specific triggers executed upon selecting the menu slot:
     - **Gears 3 TU6**: Intercepts `Localize("NeedLiveForDownloadableContent")` (`sub_8230CF58`).
     - **Judgment TU4**: Intercepts `Localize("NeedLiveForDownloadableContent")` (`sub_82302DE0`) and marketplace UI thunk (`sub_82DBF838`).
     - **Gears 2 TU6**: Intercepts Credits level map opening (`SP_Credits_P` / `UI_Scenes_Credits`) via a thread-safe VFS `FileOpenObserver`.
3. **Clean Instant Termination:**
   - On activation, invokes `ExitGame()`, which flushes pending logging via `rex::FlushLogging()` and terminates the process directly using `TerminateProcess(GetCurrentProcess(), 0)` (or `_exit(0)` on POSIX platforms), eliminating shutdown deadlocks entirely.

---

## Technical Overview

### 1. Shared Coalesced Bundle Rewriting & Localization (`layer2_ue3`)
- **Header & Source:** `src/layer2_ue3/include/ue3/main_menu_exit.h` and `src/layer2_ue3/main_menu_exit.cpp`.
- **String Lookups & Title Substitution:**
  - `GetExitStringsForLang(lang, game)` provides localized label and tooltip strings with dynamic substitution of `{GameTitle}` (`Gears of War 2`, `Gears of War 3`, or `Gears of War: Judgment`).
  - All uppercase labels are used across languages to conform to the game font textures (e.g., `EXIT GAME`, `QUITTER LE JEU`, `SPIEL BEENDEN`, `ESCI DAL GIOCO`, `SALIR DEL JUEGO`, `SAIR DO JOGO`, `WYJDŹ Z GRY`, `ВЫХОД ИЗ ИГРЫ`, `OPUSTIT HRU`, `KILÉPÉS`, `게임 종료`, `退出游戏` / `退出遊戲`).
- **Coalesced Configuration Targets:**
  - **Gears of War 3:** Rewrites section `[MainMenu]`, keys `DLC` and `TOOLTIP_DLC` in `GearGameUI.<lang>`.
  - **Gears of War: Judgment:** Rewrites section `[MainMenu]`, keys `TITLE_DLC`, `DLC`, and `TOOLTIP_DLC` in `GearGameUI.<lang>`.
  - **Gears of War 2:** Rewrites section `[OptionsMenu]`, keys `Credits` and `Credits_Desc` in `GearGame.<lang>`.
- **Encoding Preservation:**
  - Inspects the existing bundle entry value encoding. If the original string was wide (UTF-16 LE/BE), rewrites the new value as UTF-16; if narrow, preserves 8-bit ASCII/UTF-8 representation.
- **Engine Configuration Pipeline Integration:**
  - In `config_install.cpp`, `Ue3InstallConfigEditsIfRequested` checks cvar `gow_main_menu_exit` (default: `true`), detects the active title (`0x4D53082D`, `0x4D5308AB`, `0x4D530A26`), and calls `ApplyMainMenuExitToBundle()` prior to generating the `ini-edits` overlay device.

### 2. Gears 3 TU6 Hook (`layer3_gow3`)
- **Module:** `Gears3-Native/src/layer3_gow3/main_menu_exit.h` and `main_menu_exit.cpp`.
- **Cvar:** `gow3_main_menu_exit` (default `true`, category `"ui"`).
- **Function Hook:** `sub_8230CF58` (TU6 `Localize` function called when activating Marketplace).
- **Matching Logic:** `MatchesNeedLive(base, ctx.r5.u32)` checks guest string argument for `"NeedLiveForDownloadableContent"` across both UTF-16 BE and ASCII representations.
- **Action:** If matched and cvar enabled, immediately logs `"MAINMENU: Exit Game selected, ending the process"`, flushes logs, and calls `TerminateProcess(GetCurrentProcess(), 0)`. Non-matching calls forward to `__imp__sub_8230CF58`.

### 3. Gears Judgment TU4 Hook (`layer3_gowj`)
- **Module:** `GearsJ-Native/src/layer3_gowj/main_menu_exit.h` and `main_menu_exit.cpp`.
- **Cvar:** `gowj_main_menu_exit` (default `true`, category `"ui"`).
- **Function Hooks:**
  - `sub_82302DE0` (`Localize` handler): inspects `ctx.r5.u32` for `"NeedLiveForDownloadableContent"` via `MatchesNeedLive`.
  - `sub_82DBF838` (`XamShowMarketplaceDownloadItemsUI` thunk): secondary entry point if Marketplace activation bypasses localization.
- **Action:** If either hook is triggered while `gowj_main_menu_exit` is enabled, triggers `ExitGame()` to flush logs and terminate the process.

### 4. Gears 2 TU6 Hook (`layer3_gow2`)
- **Module:** `Gears2-Native/src/layer3_gow2/main_menu_exit.h` and `main_menu_exit.cpp`.
- **Cvar:** `gow2_main_menu_exit` (default `true`, category `"ui"`).
- **Chained VFS File-Opened Notifications:**
  - Enhanced `Ue3ShaderPrebuildChainFileOpened` in `layer2_ue3/shader_prebuild.cpp` to maintain a thread-safe list of observer callbacks (`g_chain_listeners` protected by `g_chain_mutex`), allowing multiple subsystems (e.g. localization logging and main menu exit) to register concurrently.
- **Interception Mechanism:**
  - In Gears 2, selecting the repurposed Credits entry attempts to load the credits level map `SP_Credits_P.war` / `UI_Scenes_Credits`.
  - Registered file-open observer `Gow2OnFileOpenedExit` detects case-insensitive requests for `sp_credits_p` or `ui_scenes_credits`.
  - Gatekeeper flag `g_ready_to_exit` (set via `OnPreLaunchModule`) prevents accidental triggers during engine startup / initial precaching.
  - Upon user activation from the menu, triggers `ExitGame()`, cleanly terminating the game.

---

## Verification & Testing

### 1. Unit Tests (Catch2)
Executed `Gears2-Native/build/Release/gn_tests.exe`:
- **Main Menu Exit Suite:** 6 test cases (71 assertions) testing:
  - Localized string lookups for all supported languages and title substitutions.
  - Bundle rewriting for Gears 2, Gears 3, and Judgment.
  - Preservation of unrelated sections and keys.
  - Encoding preservation across wide (UTF-16) and narrow entries.
  - Foreign language bundle rewriting.
- **Full Test Suite:** **133 test cases, 1,064,027 assertions passed (100% success, 0 failures).**

### 2. Compilation Verification
All three Title Update targets compiled in `Release` configuration with 0 errors:
- **Gears of War 2 TU6:** `gow2.exe` (mapped guest arena base `0x0000000100000000`, XEX version `0.0.6.7`).
- **Gears of War 3 TU6:** `gow3.exe` (mapped guest arena base `0x0000000100000000`, XEX version `0.0.6.11`).
- **Gears of War: Judgment TU4:** `gowj.exe` (mapped guest arena base `0x0000000100000000`, XEX version `0.0.4.3`).

### 3. Binary Deployment
Compiled executables were deployed directly to local game test installations:
- `COGBox/Gears2-Native/build/Release-tu6/gow2.exe` -> `C:\Users\flipp\Downloads\COGBox\Gears2\port-tu6\gow2.exe`
- `COGBox/Gears3-Native/build/Release-tu6/gow3.exe` -> `C:\Users\flipp\Downloads\COGBox\Gears3\port-tu6\gow3.exe`
- `COGBox/GearsJ-Native/build/Release/gowj.exe` -> `C:\Users\flipp\Downloads\COGBox\Judgment\port\gowj.exe`

---

## Configuration & Console Variables (CVARs)

| Game | CVAR | Default | Description |
|---|---|---|---|
| Global / UE3 | `gow_main_menu_exit` | `true` | Add Exit Game to the main menu coalesced bundle |
| Gears of War 2 | `gow2_main_menu_exit` | `true` | Intercept Credits selection to exit process |
| Gears of War 3 | `gow3_main_menu_exit` | `true` | Intercept Xbox LIVE Marketplace selection to exit process |
| Gears of War: Judgment | `gowj_main_menu_exit` | `true` | Intercept Xbox LIVE Marketplace selection to exit process |
