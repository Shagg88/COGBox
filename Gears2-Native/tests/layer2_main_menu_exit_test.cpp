#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "ue3/config_bundle.h"
#include "ue3/main_menu_exit.h"

namespace {

ue3::ConfigBundle MakeMockBundle(const std::string& filename,
                                 const std::string& section,
                                 const std::vector<std::pair<std::string, std::string>>& keys,
                                 bool wide = true) {
  ue3::ConfigBundle b;
  ue3::ConfigFile f;
  f.name = ue3::ConfigString::FromUtf8(filename, wide);
  ue3::ConfigSection s;
  s.name = ue3::ConfigString::FromUtf8(section, wide);
  for (const auto& [k, v] : keys) {
    ue3::ConfigKey key;
    key.key = ue3::ConfigString::FromUtf8(k, wide);
    key.value = ue3::ConfigString::FromUtf8(v, wide);
    s.keys.push_back(key);
  }
  f.sections.push_back(s);
  b.files.push_back(f);
  return b;
}

const ue3::ConfigKey* FindKey(const ue3::ConfigBundle& b, const std::string& filename,
                              const std::string& section, const std::string& key) {
  for (const auto& f : b.files) {
    if (f.name.Utf8() != filename) continue;
    for (const auto& s : f.sections) {
      if (s.name.Utf8() != section) continue;
      for (const auto& k : s.keys) {
        if (k.key.Utf8() == key) return &k;
      }
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("main menu exit: string lookup for languages and games", "[ue3][exit]") {
  using namespace ue3;

  // Test requirements from brief
  auto g3_int = GetExitStringsForLang("INT", Game::Gears3);
  CHECK(g3_int.label == "EXIT GAME");
  CHECK(g3_int.tooltip == "Exit Gears of War 3 to desktop.");

  auto g2_fra = GetExitStringsForLang("FRA", Game::Gears2);
  CHECK(g2_fra.label == "QUITTER LE JEU");
  CHECK(g2_fra.tooltip == "Quitter Gears of War 2 et revenir au bureau.");

  auto gj_deu = GetExitStringsForLang("DEU", GameId::Judgment);
  CHECK(gj_deu.label == "SPIEL BEENDEN");
  CHECK(gj_deu.tooltip == "Gears of War: Judgment beenden und zum Desktop zurückkehren.");

  auto g3_ita = GetExitStringsForLang("ITA", GameId::Gears3);
  CHECK(g3_ita.label == "ESCI DAL GIOCO");
  CHECK(g3_ita.tooltip == "Esci da Gears of War 3 e torna al desktop.");

  auto g2_esn = GetExitStringsForLang("ESN", GameId::Gears2);
  CHECK(g2_esn.label == "SALIR DEL JUEGO");
  CHECK(g2_esn.tooltip == "Salir de Gears of War 2 al escritorio.");

  auto g3_esm = GetExitStringsForLang("ESM", GameId::Gears3);
  CHECK(g3_esm.label == "SALIR DEL JUEGO");
  CHECK(g3_esm.tooltip == "Salir de Gears of War 3 al escritorio.");

  auto gj_ptb = GetExitStringsForLang("PTB", GameId::Judgment);
  CHECK(gj_ptb.label == "SAIR DO JOGO");
  CHECK(gj_ptb.tooltip == "Sair de Gears of War: Judgment para a área de trabalho.");

  auto g2_pol = GetExitStringsForLang("POL", GameId::Gears2);
  CHECK(g2_pol.label == "WYJDŹ Z GRY");
  CHECK(g2_pol.tooltip == "Wyjdź z Gears of War 2 do pulpitu.");

  auto g3_rus = GetExitStringsForLang("RUS", GameId::Gears3);
  CHECK(g3_rus.label == "ВЫХОД ИЗ ИГРЫ");
  CHECK(g3_rus.tooltip == "Выйти из Gears of War 3 на рабочий стол.");

  auto gj_cze = GetExitStringsForLang("CZE", GameId::Judgment);
  CHECK(gj_cze.label == "UKONČIT HRU");
  CHECK(gj_cze.tooltip == "Ukončit Gears of War: Judgment a přejít na plochu.");

  auto g2_hun = GetExitStringsForLang("HUN", GameId::Gears2);
  CHECK(g2_hun.label == "KILÉPÉS A JÁTÉKBÓL");
  CHECK(g2_hun.tooltip == "Kilépés a Gears of War 2 játékból az asztalra.");

  auto g3_kor = GetExitStringsForLang("KOR", GameId::Gears3);
  CHECK(g3_kor.label == "게임 종료");
  CHECK(g3_kor.tooltip == "Gears of War 3를 종료하고 바탕 화면으로 돌아갑니다.");

  auto gj_chn = GetExitStringsForLang("CHN", GameId::Judgment);
  CHECK(gj_chn.label == "離開遊戲");
  CHECK(gj_chn.tooltip == "離開 Gears of War: Judgment 並返回桌面。");

  auto g3_cht = GetExitStringsForLang("CHT", GameId::Gears3);
  CHECK(g3_cht.label == "離開遊戲");
  CHECK(g3_cht.tooltip == "離開 Gears of War 3 並返回桌面。");

  // Language code normalization
  auto g3_int_lower = GetExitStringsForLang("int", Game::Gears3);
  CHECK(g3_int_lower.label == "EXIT GAME");

  auto g3_from_filename = GetExitStringsForLang("Coalesced_FRA.bin", Game::Gears3);
  CHECK(g3_from_filename.label == "QUITTER LE JEU");

  // Unknown language defaults to English
  auto g3_unknown = GetExitStringsForLang("XYZ", Game::Gears3);
  CHECK(g3_unknown.label == "EXIT GAME");
  CHECK(g3_unknown.tooltip == "Exit Gears of War 3 to desktop.");
}

TEST_CASE("main menu exit: Gears 3 bundle rewriting", "[ue3][exit]") {
  using namespace ue3;

  auto bundle = MakeMockBundle(
      "GearGameUI.int", "MainMenu",
      {{"Campaign", "Campaign"},
       {"DLC", "Xbox LIVE Marketplace"},
       {"TOOLTIP_DLC", "See what's new to download for Gears of War 3."},
       {"Options", "Options"}},
      true);

  // Add another unrelated file
  ue3::ConfigFile eng;
  eng.name = ue3::ConfigString::FromUtf8("Xe-GearEngine.ini", true);
  ue3::ConfigSection sec;
  sec.name = ue3::ConfigString::FromUtf8("SystemSettings", true);
  sec.keys.push_back({ue3::ConfigString::FromUtf8("DLC", true), ue3::ConfigString::FromUtf8("KeepMe", true)});
  eng.sections.push_back(sec);
  bundle.files.push_back(eng);

  int replaced = ApplyMainMenuExitToBundle(GameId::Gears3, "INT", bundle);
  CHECK(replaced == 2);

  const auto* dlc = FindKey(bundle, "GearGameUI.int", "MainMenu", "DLC");
  REQUIRE(dlc != nullptr);
  CHECK(dlc->value.Utf8() == "EXIT GAME");
  CHECK(dlc->value.wide == true);

  const auto* tooltip = FindKey(bundle, "GearGameUI.int", "MainMenu", "TOOLTIP_DLC");
  REQUIRE(tooltip != nullptr);
  CHECK(tooltip->value.Utf8() == "Exit Gears of War 3 to desktop.");
  CHECK(tooltip->value.wide == true);

  // Unrelated keys in same section untouched
  const auto* camp = FindKey(bundle, "GearGameUI.int", "MainMenu", "Campaign");
  REQUIRE(camp != nullptr);
  CHECK(camp->value.Utf8() == "Campaign");

  // Unrelated file untouched even if it has a DLC key
  const auto* eng_dlc = FindKey(bundle, "Xe-GearEngine.ini", "SystemSettings", "DLC");
  REQUIRE(eng_dlc != nullptr);
  CHECK(eng_dlc->value.Utf8() == "KeepMe");
}

TEST_CASE("main menu exit: Judgment bundle rewriting", "[ue3][exit]") {
  using namespace ue3;

  auto bundle = MakeMockBundle(
      "..\\..\\GearGame\\Localization\\INT\\GearGameUI.int", "MainMenu",
      {{"TITLE_DLC", "Xbox LIVE Marketplace"},
       {"DLC", "Xbox LIVE Marketplace"},
       {"TOOLTIP_DLC", "Xbox LIVE Marketplace"},
       {"Versus", "Versus"}},
      true);

  int replaced = ApplyMainMenuExitToBundle(GameId::Judgment, "INT", bundle);
  CHECK(replaced == 3);

  const auto* title_dlc = FindKey(bundle, "..\\..\\GearGame\\Localization\\INT\\GearGameUI.int", "MainMenu", "TITLE_DLC");
  REQUIRE(title_dlc != nullptr);
  CHECK(title_dlc->value.Utf8() == "EXIT GAME");
  CHECK(title_dlc->value.wide == true);

  const auto* dlc = FindKey(bundle, "..\\..\\GearGame\\Localization\\INT\\GearGameUI.int", "MainMenu", "DLC");
  REQUIRE(dlc != nullptr);
  CHECK(dlc->value.Utf8() == "EXIT GAME");

  const auto* tooltip = FindKey(bundle, "..\\..\\GearGame\\Localization\\INT\\GearGameUI.int", "MainMenu", "TOOLTIP_DLC");
  REQUIRE(tooltip != nullptr);
  CHECK(tooltip->value.Utf8() == "Exit Gears of War: Judgment to desktop.");

  const auto* versus = FindKey(bundle, "..\\..\\GearGame\\Localization\\INT\\GearGameUI.int", "MainMenu", "Versus");
  REQUIRE(versus != nullptr);
  CHECK(versus->value.Utf8() == "Versus");
}

TEST_CASE("main menu exit: Gears 2 bundle rewriting", "[ue3][exit]") {
  using namespace ue3;

  auto bundle = MakeMockBundle(
      "..\\GearGame\\Localization\\INT\\GearGame.int", "OptionsMenu",
      {{"Audio", "Audio"},
       {"Credits", "Credits"},
       {"Credits_Desc", "See the people responsible for creating Gears of War 2."},
       {"Video", "Video"}},
      true);

  int replaced = ApplyMainMenuExitToBundle(GameId::Gears2, "INT", bundle);
  CHECK(replaced == 2);

  const auto* cred = FindKey(bundle, "..\\GearGame\\Localization\\INT\\GearGame.int", "OptionsMenu", "Credits");
  REQUIRE(cred != nullptr);
  CHECK(cred->value.Utf8() == "EXIT GAME");
  CHECK(cred->value.wide == true);

  const auto* desc = FindKey(bundle, "..\\GearGame\\Localization\\INT\\GearGame.int", "OptionsMenu", "Credits_Desc");
  REQUIRE(desc != nullptr);
  CHECK(desc->value.Utf8() == "Exit Gears of War 2 to desktop.");
  CHECK(desc->value.wide == true);

  const auto* audio = FindKey(bundle, "..\\GearGame\\Localization\\INT\\GearGame.int", "OptionsMenu", "Audio");
  REQUIRE(audio != nullptr);
  CHECK(audio->value.Utf8() == "Audio");
}

TEST_CASE("main menu exit: foreign language with localized filename", "[ue3][exit]") {
  using namespace ue3;

  auto bundle = MakeMockBundle(
      "..\\..\\GearGame\\Localization\\FRA\\geargameui.fra", "MainMenu",
      {{"DLC", "Marché Xbox LIVE"},
       {"TOOLTIP_DLC", "Affichez les nouveautés téléchargeables de Gears of War 3."}},
      true);

  int replaced = ApplyMainMenuExitToBundle(GameId::Gears3, "FRA", bundle);
  CHECK(replaced == 2);

  const auto* dlc = FindKey(bundle, "..\\..\\GearGame\\Localization\\FRA\\geargameui.fra", "MainMenu", "DLC");
  REQUIRE(dlc != nullptr);
  CHECK(dlc->value.Utf8() == "QUITTER LE JEU");

  const auto* tooltip = FindKey(bundle, "..\\..\\GearGame\\Localization\\FRA\\geargameui.fra", "MainMenu", "TOOLTIP_DLC");
  REQUIRE(tooltip != nullptr);
  CHECK(tooltip->value.Utf8() == "Quitter Gears of War 3 et revenir au bureau.");
}

TEST_CASE("main menu exit: narrow string preservation when applicable", "[ue3][exit]") {
  using namespace ue3;

  auto bundle = MakeMockBundle(
      "GearGameUI.int", "MainMenu",
      {{"DLC", "DLC"},
       {"TOOLTIP_DLC", "Tooltip"}},
      false);  // narrow strings

  int replaced = ApplyMainMenuExitToBundle(GameId::Gears3, "INT", bundle);
  CHECK(replaced == 2);

  const auto* dlc = FindKey(bundle, "GearGameUI.int", "MainMenu", "DLC");
  REQUIRE(dlc != nullptr);
  CHECK(dlc->value.Utf8() == "EXIT GAME");
  CHECK_FALSE(dlc->value.wide);
}
