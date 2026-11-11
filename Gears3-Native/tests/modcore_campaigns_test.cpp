// modcore campaigns (campaigns.h): mod.ini [Campaign] / [Chapter.N], slot allocation, the config it writes.
#include <catch2/catch_test_macros.hpp>

#include "modcore/campaigns.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  explicit TempDir(const char* name) : path(fs::temp_directory_path() / name) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
  fs::path Mod(const std::string& folder, const std::string& ini) const {
    const fs::path d = path / folder;
    fs::create_directories(d);
    std::ofstream(d / "mod.ini", std::ios::binary) << ini;
    return d;
  }
};

// a packed configuration (Coalesced_<LANG>.bin): files of sections of keys, ANSI strings
struct Packed {
  std::vector<uint8_t> d;
  void I32(int32_t v) { for (int s = 24; s >= 0; s -= 8) d.push_back(uint8_t(uint32_t(v) >> s)); }
  void Str(const std::string& s) { I32(int32_t(s.size() + 1)); d.insert(d.end(), s.begin(), s.end()); d.push_back(0); }
};
using Keys = std::vector<std::pair<std::string, std::string>>;
using Sections = std::vector<std::pair<std::string, Keys>>;
std::vector<uint8_t> Pack(const std::vector<std::pair<std::string, Sections>>& files) {
  Packed p;
  p.I32(int32_t(files.size()));
  for (const auto& [name, sections] : files) {
    p.Str(name);
    p.I32(int32_t(sections.size()));
    for (const auto& [sec, keys] : sections) {
      p.Str(sec);
      p.I32(int32_t(keys.size()));
      for (const auto& [k, v] : keys) { p.Str(k); p.Str(v); }
    }
  }
  return p.d;
}

// the stock game's shape: acts 0-4 and chapters 0-31 in Xbox360-GearGame.ini, the names in GearGame.<lang>
std::vector<uint8_t> StockConfig(const std::string& lang = "int") {
  Sections ini, loc;
  for (int a = 0; a < 5; a++) ini.push_back({"Act" + std::to_string(a + 1) + " GearCampaignActData", {{"ActNum", std::to_string(a)}}});
  for (int c = 0; c < 32; c++)
    ini.push_back({"Chapter" + std::to_string(c) + " GearCampaignChapterData", {{"ChapterNum", std::to_string(c)}, {"ActNum", "0"}}});
  loc.push_back({"Act1 GearCampaignActData", {{"ListName", "I: Anchored"}}});
  return Pack({{"..\\GearGame\\Config\\Xbox360-GearGame.ini", ini},
               {"..\\GearGame\\Localization\\" + std::string(lang == "int" ? "INT" : "FRA") + "\\GearGame." + lang, loc}});
}

}  // namespace

TEST_CASE("modcore campaigns: mod.ini [Campaign] and [Chapter.N]", "[modcore]") {
  TempDir t("modcore_campaigns_read");
  const fs::path d = t.Mod("Gears1_Chapter1",
                           "[Mod]\nName = Chapter One\nApi = 1\ncontent = MPCH_A\n"
                           "[Campaign]\nId = Gears1\nTitle = Gears of War\nOrder = 10\nColour = red\n"
                           "[Chapter.2]\nMap = Second_P\n"
                           "[Chapter.1]\nMap = GOW1_SP_Prison_P\nTitle = 14 Years After\nLocation = Jacinto\nTime = E-Day + 14\n"
                           "ContentPackages = MPCH_A, MPCH_B\nSpeed = 2\n"
                           "[Chapter.3]\nTitle = no map\n"
                           "[Chapter.x]\nMap = Bad_P\n"
                           "[Other]\npriority = 5\n");
  const modcore::Manifest m = modcore::Read(d);
  CHECK(m.name == "Chapter One");
  CHECK(m.api == 1);
  CHECK(m.priority == 5);  // keys in other sections are the mod's, as before campaigns
  CHECK(m.campaign.id == "gears1");
  CHECK(m.campaign.title == "Gears of War");
  CHECK(m.campaign.order == 10);
  REQUIRE(m.chapters.size() == 2);
  CHECK(m.chapters[0].number == 1);
  CHECK(m.chapters[0].map == "GOW1_SP_Prison_P");
  CHECK(m.chapters[0].location == "Jacinto");
  CHECK(m.chapters[1].number == 2);
  CHECK(m.content == std::vector<std::string>{"MPCH_A", "MPCH_B"});
  CHECK(m.warnings.size() == 4);  // Colour, Speed, chapter 3 without a map, [Chapter.x]
}

TEST_CASE("modcore campaigns: an old mod.ini reads as before", "[modcore]") {
  TempDir t("modcore_campaigns_old");
  const modcore::Manifest m = modcore::Read(t.Mod("Old", "[mod]\nname = Old\npriority = 2\ncontent = X, Y\n"));
  CHECK(m.api == 0);
  CHECK(m.campaign.id.empty());
  CHECK(m.chapters.empty());
  CHECK(m.warnings.empty());
  CHECK(m.priority == 2);
  CHECK(m.content == std::vector<std::string>{"X", "Y"});
}

TEST_CASE("modcore campaigns: slots after the game's, joined by Id, in order", "[modcore]") {
  TempDir t("modcore_campaigns_alloc");
  std::vector<fs::path> dirs = {
      t.Mod("G1C2", "[Campaign]\nId = gears1\n[Chapter.2]\nMap = B_P\n"),
      t.Mod("Remix", "[Campaign]\nId = remix\nTitle = Remix\nOrder = 50\n[Chapter.1]\nMap = R_P\n"),
      t.Mod("G1C1", "[Campaign]\nId = gears1\nTitle = Gears of War\nOrder = 10\n[Chapter.1]\nMap = A_P\n"),
  };
  const auto mods = modcore::FromFolders(dirs);
  const modcore::UsedSlots used = modcore::UsedCampaignSlots(StockConfig());
  CHECK(used.acts.size() == 5);
  CHECK(used.chapters.size() == 32);
  const modcore::Allocation a = modcore::AllocateCampaigns(mods, used);
  CHECK(a.problems.empty());
  REQUIRE(a.campaigns.size() == 2);
  // gears1 first: G1C1's Order 10 (the first Order its mods give) sorts it before Remix's 50
  CHECK(a.campaigns[0].id == "gears1");
  CHECK(a.campaigns[0].title == "Gears of War");
  CHECK(a.campaigns[0].act == 5);
  REQUIRE(a.campaigns[0].chapters.size() == 2);
  CHECK(a.campaigns[0].chapters[0].decl.map == "A_P");
  CHECK(a.campaigns[0].chapters[0].slot == 32);
  CHECK(a.campaigns[0].chapters[1].slot == 33);
  CHECK(a.campaigns[1].id == "remix");
  CHECK(a.campaigns[1].act == 6);
  CHECK(a.campaigns[1].chapters[0].slot == 34);
  CHECK(a.log[0] == "campaign gears1 'Gears of War': act VI (ActNum 5), chapters 32-33");
  // the same mods give the same slots
  const modcore::Allocation again = modcore::AllocateCampaigns(modcore::FromFolders(dirs), used);
  CHECK(again.log == a.log);
}

TEST_CASE("modcore campaigns: Order sorts campaigns; taken slots are skipped; a twin chapter is left out", "[modcore]") {
  TempDir t("modcore_campaigns_order");
  const auto mods = modcore::FromFolders({
      t.Mod("Late", "[Campaign]\nId = late\nOrder = 90\n[Chapter.1]\nMap = L_P\n"),
      t.Mod("Early", "[Campaign]\nId = early\nOrder = 5\n[Chapter.1]\nMap = E_P\n"),
      t.Mod("EarlyTwin", "[Campaign]\nId = early\n[Chapter.1]\nMap = E2_P\n"),
      t.Mod("Off", "[Mod]\nenabled = false\n[Campaign]\nId = off\n[Chapter.1]\nMap = O_P\n"),
  });
  modcore::UsedSlots used = modcore::UsedCampaignSlots(StockConfig());
  used.acts.insert(5);       // RAAM's Shadow's act
  used.chapters.insert(32);  // and a chapter an old-style mod writes itself
  const modcore::Allocation a = modcore::AllocateCampaigns(mods, used);
  REQUIRE(a.campaigns.size() == 2);
  CHECK(a.campaigns[0].id == "early");
  CHECK(a.campaigns[0].act == 6);
  CHECK(a.campaigns[0].chapters[0].decl.map == "E_P");
  CHECK(a.campaigns[0].chapters[0].slot == 33);
  CHECK(a.campaigns[1].id == "late");
  CHECK(a.campaigns[1].act == 7);
  CHECK(a.campaigns[1].chapters[0].slot == 34);
  REQUIRE(a.problems.size() == 1);
  CHECK(a.problems[0].find("EarlyTwin") == 0);
}

TEST_CASE("modcore campaigns: chapters beyond 49 are left out, with the reason", "[modcore]") {
  TempDir t("modcore_campaigns_full");
  std::string ini = "[Campaign]\nId = big\n";
  for (int n = 1; n <= 20; n++) ini += "[Chapter." + std::to_string(n) + "]\nMap = M" + std::to_string(n) + "_P\n";
  const modcore::Allocation a = modcore::AllocateCampaigns(modcore::FromFolders({t.Mod("Big", ini)}),
                                                           modcore::UsedCampaignSlots(StockConfig()));
  REQUIRE(a.campaigns.size() == 1);
  CHECK(a.campaigns[0].chapters.size() == 18);
  CHECK(a.campaigns[0].chapters.back().slot == 49);
  CHECK(a.problems.size() == 2);
}

TEST_CASE("modcore campaigns: the fragment merges into the config and reads back", "[modcore]") {
  TempDir t("modcore_campaigns_merge");
  const auto mods = modcore::FromFolders({t.Mod("G1C1",
                                                "[Campaign]\nId = gears1\nTitle = Gears of War\n"
                                                "[Chapter.1]\nMap = GOW1_SP_Prison_P\nTitle = 14 Years After\n"
                                                "Location = Jacinto\nScreenshot = UI.Shot\n")});
  for (const std::string lang : {"int", "fra"}) {
    const std::vector<uint8_t> base = StockConfig(lang);
    CHECK(modcore::LocalizedGearGameFile(base) == "GearGame." + lang);
    const modcore::Allocation a = modcore::AllocateCampaigns(mods, modcore::UsedCampaignSlots(base));
    const std::string text = modcore::CampaignFragment(a, modcore::LocalizedGearGameFile(base));
    CHECK(text.find("[Act6 GearCampaignActData]\nActNum=5\n") != std::string::npos);
    CHECK(text.find("[Chapter32 GearCampaignChapterData]\nChapterNum=32\nActNum=5\nMapName=GOW1_SP_Prison_P\n"
                    "ScreenshotPathName=UI.Shot\nIsAlwaysUnlocked=true\n") != std::string::npos);
    CHECK(text.find("##### GearGame." + lang + "\n[Act6 GearCampaignActData]\nListName=VI: Gears of War\n") != std::string::npos);
    CHECK(text.find("ListName=1: 14 Years After\nDisplayName=14 Years After\nChapterNumDesc=Chapter 1\nChapterLocation=Jacinto\n") !=
          std::string::npos);
    const fs::path frag = t.path / "campaigns.ini";
    std::ofstream(frag, std::ios::binary) << text;
    const std::vector<uint8_t> merged = modcore::MergeConfig(base, {{"campaigns", frag}});
    REQUIRE(!merged.empty());
    const modcore::UsedSlots after = modcore::UsedCampaignSlots(merged);
    CHECK(after.acts.count(5) == 1);
    CHECK(after.chapters.count(32) == 1);
  }
  CHECK(modcore::CampaignFragment(modcore::Allocation{}).empty());
}

TEST_CASE("modcore config: sections and keys match whatever their case", "[modcore]") {
  // the trilogy's [Objectives] onto the game's [OBJECTIVES]: one section, the game's keys kept
  TempDir t("modcore_config_case");
  const std::vector<uint8_t> base = Pack({{"..\\..\\GearGame\\Localization\\INT\\level_objectives.int",
                                           {{"OBJECTIVES", {{"NIGHTMARE_OBJ_1", "Follow Anya."}, {"Shared", "old"}}}}}});
  const fs::path frag = t.path / "objectives.ini";
  std::ofstream(frag, std::ios::binary) << "##### level_objectives.int\n[Objectives]\nPRISON_OBJ_DOOR=Open the jammed door.\n"
                                           "shared=new\n";
  const std::vector<uint8_t> merged = modcore::MergeConfig(base, {{"trilogy", frag}});
  REQUIRE(!merged.empty());
  const std::string text(merged.begin(), merged.end());
  auto count = [&](const std::string& ascii) {   // the merged file is UTF-16: look for the text as UTF-16LE
    std::string w;
    for (char c : ascii) { w += c; w += '\0'; }
    size_t n = 0;
    for (size_t at = text.find(w); at != std::string::npos; at = text.find(w, at + 1)) n++;
    return n;
  };
  CHECK(count("OBJECTIVES") == 1);
  CHECK(count("Objectives") == 0);
  CHECK(count("NIGHTMARE_OBJ_1") == 1);
  CHECK(count("PRISON_OBJ_DOOR") == 1);
  CHECK(count("old") == 0);   // shared=new replaced Shared=old
  CHECK(count("new") == 1);
}
