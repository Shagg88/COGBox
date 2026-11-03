#include "ue3/main_menu_exit.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ue3 {
namespace {

struct ExitStringTemplate {
  std::string_view label;
  std::string_view tooltip_template;
};

// Localized strings and templates for each supported language.
const std::unordered_map<std::string_view, ExitStringTemplate> kLanguageStrings = {
    {"INT", {"EXIT GAME", "Exit {GameTitle} to desktop."}},
    {"FRA", {"QUITTER LE JEU", "Quitter {GameTitle} et revenir au bureau."}},
    {"DEU", {"SPIEL BEENDEN", "{GameTitle} beenden und zum Desktop zurückkehren."}},
    {"ITA", {"ESCI DAL GIOCO", "Esci da {GameTitle} e torna al desktop."}},
    {"ESN", {"SALIR DEL JUEGO", "Salir de {GameTitle} al escritorio."}},
    {"ESM", {"SALIR DEL JUEGO", "Salir de {GameTitle} al escritorio."}},
    {"PTB", {"SAIR DO JOGO", "Sair de {GameTitle} para a área de trabalho."}},
    {"POL", {"WYJDŹ Z GRY", "Wyjdź z {GameTitle} do pulpitu."}},
    {"RUS", {"ВЫХОД ИЗ ИГРЫ", "Выйти из {GameTitle} на рабочий стол."}},
    {"CZE", {"UKONČIT HRU", "Ukončit {GameTitle} a přejít na plochu."}},
    {"HUN", {"KILÉPÉS A JÁTÉKBÓL", "Kilépés a {GameTitle} játékból az asztalra."}},
    {"KOR", {"게임 종료", "{GameTitle}를 종료하고 바탕 화면으로 돌아갑니다."}},
    {"CHN", {"離開遊戲", "離開 {GameTitle} 並返回桌面。"}},
    {"CHT", {"離開遊戲", "離開 {GameTitle} 並返回桌面。"}},
};

std::string ToUpper(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

std::string ToLower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

std::string NormalizeLanguageCode(std::string_view lang) {
  // Extract language if passed full filename like "Coalesced_FRA.bin" or "Coalesced_int.bin"
  size_t coal_pos = lang.find("Coalesced_");
  if (coal_pos == std::string_view::npos) {
    coal_pos = lang.find("coalesced_");
  }
  if (coal_pos != std::string_view::npos) {
    size_t start = coal_pos + 10;
    size_t end = lang.find('.', start);
    if (end != std::string_view::npos && end > start) {
      lang = lang.substr(start, end - start);
    } else {
      lang = lang.substr(start);
    }
  }

  // Remove any remaining dots or path separators
  size_t last_sep = lang.find_last_of("/\\");
  if (last_sep != std::string_view::npos) {
    lang = lang.substr(last_sep + 1);
  }
  size_t dot_pos = lang.rfind('.');
  if (dot_pos != std::string_view::npos) {
    lang = lang.substr(0, dot_pos);
  }

  return ToUpper(lang);
}

std::string_view GetGameTitle(GameId game) {
  switch (game) {
    case GameId::Gears2:
      return "Gears of War 2";
    case GameId::Gears3:
      return "Gears of War 3";
    case GameId::Judgment:
      return "Gears of War: Judgment";
  }
  return "Gears of War";
}

std::string FormatTooltip(std::string_view tmpl, std::string_view title) {
  std::string result(tmpl);
  const std::string_view placeholder = "{GameTitle}";
  size_t pos = 0;
  while ((pos = result.find(placeholder, pos)) != std::string::npos) {
    result.replace(pos, placeholder.length(), title);
    pos += title.length();
  }
  return result;
}

bool NeedsWide(std::string_view s) {
  for (char c : s) {
    if (static_cast<unsigned char>(c) > 127) {
      return true;
    }
  }
  return false;
}

void SplitPath(std::string_view path, std::string_view& stem, std::string_view& ext) {
  size_t last_sep = path.find_last_of("/\\:");
  std::string_view fname = (last_sep != std::string_view::npos) ? path.substr(last_sep + 1) : path;

  size_t dot_pos = fname.rfind('.');
  ext = (dot_pos != std::string_view::npos) ? fname.substr(dot_pos) : "";
  stem = (dot_pos != std::string_view::npos) ? fname.substr(0, dot_pos) : fname;
}

bool IsMatchingFile(std::string_view path, std::string_view target_stem) {
  std::string_view stem, ext;
  SplitPath(path, stem, ext);

  // Configuration files (.ini) are never localization files
  if (ToLower(ext) == ".ini") {
    return false;
  }

  return ToLower(stem) == ToLower(target_stem);
}

bool IsGearUIConfigFile(std::string_view path) {
  std::string_view stem, ext;
  SplitPath(path, stem, ext);

  if (ToLower(ext) != ".ini") {
    return false;
  }

  return ToLower(stem).find("gearui") != std::string::npos;
}

size_t FindListProperty(std::string_view val) {
  size_t pos = 0;
  while ((pos = val.find("List=", pos)) != std::string_view::npos) {
    if (pos == 0 || val[pos - 1] == ',' || val[pos - 1] == '(' ||
        std::isspace(static_cast<unsigned char>(val[pos - 1]))) {
      return pos;
    }
    pos += 5;
  }
  return std::string_view::npos;
}

bool ClearListProperty(std::string& val) {
  const size_t list_pos = FindListProperty(val);
  if (list_pos == std::string::npos) {
    const size_t last_paren = val.rfind(')');
    if (last_paren != std::string::npos) {
      val.insert(last_paren, ",List=()");
      return true;
    }
    return false;
  }

  size_t open_paren = list_pos + 5;
  while (open_paren < val.size() && std::isspace(static_cast<unsigned char>(val[open_paren]))) {
    ++open_paren;
  }
  if (open_paren >= val.size() || val[open_paren] != '(') {
    return false;
  }

  int depth = 0;
  size_t close_paren = std::string::npos;
  bool in_quotes = false;
  for (size_t p = open_paren; p < val.size(); ++p) {
    if (val[p] == '"' && (p == 0 || val[p - 1] != '\\')) {
      in_quotes = !in_quotes;
    } else if (!in_quotes) {
      if (val[p] == '(') {
        ++depth;
      } else if (val[p] == ')') {
        --depth;
        if (depth == 0) {
          close_paren = p;
          break;
        }
      }
    }
  }

  if (close_paren != std::string::npos) {
    const std::string_view current_list = std::string_view(val).substr(list_pos, (close_paren + 1) - list_pos);
    if (current_list != "List=()") {
      val.replace(list_pos, (close_paren + 1) - list_pos, "List=()");
      return true;
    }
  }

  return false;
}

}  // namespace

ExitMenuStrings GetExitStringsForLang(std::string_view lang, GameId game) {
  const std::string norm = NormalizeLanguageCode(lang);
  auto it = kLanguageStrings.find(norm);
  if (it == kLanguageStrings.end()) {
    it = kLanguageStrings.find("INT");
  }

  const ExitStringTemplate& tmpl = it->second;
  const std::string_view title = GetGameTitle(game);

  return ExitMenuStrings{
      .label = std::string(tmpl.label),
      .tooltip = FormatTooltip(tmpl.tooltip_template, title),
  };
}

int ApplyMainMenuExitToBundle(GameId game, std::string_view lang, ConfigBundle& bundle) {
  const ExitMenuStrings exit_strs = GetExitStringsForLang(lang, game);
  int replaced = 0;

  const std::string_view target_stem = (game == GameId::Gears2) ? "geargame" : "geargameui";
  const std::string target_section = (game == GameId::Gears2) ? "optionsmenu" : "mainmenu";

  for (ConfigFile& f : bundle.files) {
    const std::string fname = f.name.Utf8();

    if (game == GameId::Judgment && IsGearUIConfigFile(fname)) {
      for (ConfigSection& s : f.sections) {
        if (ToLower(s.name.Utf8()) != "geargame.uiwidget_mainmenu") {
          continue;
        }

        for (size_t i = 0; i < s.keys.size(); ++i) {
          ConfigKey& k = s.keys[i];
          std::string kn = ToLower(k.key.Utf8());
          while (!kn.empty() && (kn.front() == '+' || kn.front() == '.' || kn.front() == '!')) {
            kn.erase(kn.begin());
          }
          if (kn != "alloptions") {
            continue;
          }

          std::string val = k.value.Utf8();
          bool val_modified = false;

          const std::string_view kTarget = "Next=MAINMENUOP_DLC_Categories";
          const std::string_view kReplacement = "Next=MAINMENUOP_DLC";
          if (val.find(kTarget) != std::string::npos) {
            size_t pos = 0;
            while ((pos = val.find(kTarget, pos)) != std::string::npos) {
              val.replace(pos, kTarget.length(), kReplacement);
              pos += kReplacement.length();
            }
            val_modified = true;
          }

          if (val.find("Identifier=MAINMENUOP_DLC_Categories") != std::string::npos) {
            if (ClearListProperty(val)) {
              val_modified = true;
            }
          }

          size_t dlc_id_pos = val.find("Identifier=MAINMENUOP_DLC");
          if (dlc_id_pos != std::string::npos) {
            size_t end_pos = dlc_id_pos + std::string_view("Identifier=MAINMENUOP_DLC").length();
            if (end_pos >= val.size() || val[end_pos] == ',' || val[end_pos] == ')') {
              // Identifier=MAINMENUOP_DLC must NEVER be an AllOptions entry; in UE3 Scaleform
              // any identifier in AllOptions is registered as a navigable submenu screen rather than
              // an immediate action. Erase it if present so selecting Exit Game triggers the action.
              s.keys.erase(s.keys.begin() + i);
              --i;
              ++replaced;
              continue;
            }
          }

          if (val_modified) {
            k.value = ConfigString::FromUtf8(val, k.value.wide);
            k.value.present = true;
            ++replaced;
          }
        }
      }
      continue;
    }

    if (!IsMatchingFile(fname, target_stem)) {
      continue;
    }

    for (ConfigSection& s : f.sections) {
      if (ToLower(s.name.Utf8()) != target_section) {
        continue;
      }

      for (ConfigKey& k : s.keys) {
        const std::string key_name = ToLower(k.key.Utf8());

        if (game == GameId::Gears3) {
          if (key_name == "dlc") {
            const bool wide = k.value.wide || NeedsWide(exit_strs.label);
            k.value = ConfigString::FromUtf8(exit_strs.label, wide);
            k.value.present = true;
            ++replaced;
          } else if (key_name == "tooltip_dlc") {
            const bool wide = k.value.wide || NeedsWide(exit_strs.tooltip);
            k.value = ConfigString::FromUtf8(exit_strs.tooltip, wide);
            k.value.present = true;
            ++replaced;
          }
        } else if (game == GameId::Judgment) {
          if (key_name == "title_dlc" || key_name == "dlc") {
            const bool wide = k.value.wide || NeedsWide(exit_strs.label);
            k.value = ConfigString::FromUtf8(exit_strs.label, wide);
            k.value.present = true;
            ++replaced;
          } else if (key_name == "tooltip_dlc") {
            const bool wide = k.value.wide || NeedsWide(exit_strs.tooltip);
            k.value = ConfigString::FromUtf8(exit_strs.tooltip, wide);
            k.value.present = true;
            ++replaced;
          }
        } else if (game == GameId::Gears2) {
          if (key_name == "credits") {
            const bool wide = k.value.wide || NeedsWide(exit_strs.label);
            k.value = ConfigString::FromUtf8(exit_strs.label, wide);
            k.value.present = true;
            ++replaced;
          } else if (key_name == "credits_desc") {
            const bool wide = k.value.wide || NeedsWide(exit_strs.tooltip);
            k.value = ConfigString::FromUtf8(exit_strs.tooltip, wide);
            k.value.present = true;
            ++replaced;
          }
        }
      }
    }
  }

  return replaced;
}

}  // namespace ue3
