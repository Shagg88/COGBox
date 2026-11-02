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

}  // namespace ue3
