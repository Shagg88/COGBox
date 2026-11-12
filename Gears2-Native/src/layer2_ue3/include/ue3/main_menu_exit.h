#pragma once

#include <string>
#include <string_view>
#include "ue3/config_bundle.h"

namespace ue3 {

enum class GameId {
  Gears2,
  Gears3,
  Judgment,
};
using Game = GameId;

struct ExitMenuStrings {
  std::string label;
  std::string tooltip;
};

// Returns localized Exit Game label and tooltip for the given language (e.g. "INT", "FRA") and game.
ExitMenuStrings GetExitStringsForLang(std::string_view lang, GameId game);

// Rewrites main menu DLC (Gears 3 / Judgment) or Credits (Gears 2) entries in the config bundle to localized Exit Game.
// Returns the number of keys replaced.
int ApplyMainMenuExitToBundle(GameId game, std::string_view lang, ConfigBundle& bundle);

// Front-end tracking for the per-game exit hooks, so a credits map or a "need Xbox LIVE" message elsewhere in the
// game can't close it. Feed every guest path the game opens (the file-open observer); InFrontEndForExit() is true
// while GearStart*.xxx (the title screen and its menus) is the last level package the game opened.
void NoteFileOpenedForExit(std::string_view guest_path);
bool InFrontEndForExit();
// Gears 2 (Shagg 2026-11-10): back on the main menu after a level, GearStart.xxx is not opened again, so the file
// hook above never re-arms. The game's own lookup of the menu row's label (seen by the Localize hook) arms it instead.
void ArmFrontEndForExit(std::string_view why);

}  // namespace ue3
