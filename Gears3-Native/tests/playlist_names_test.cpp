// ue3/playlist_names.h: the Forces of Nature Guardian playlist gets the name no shipped .int gives it.
#include <catch2/catch_test_macros.hpp>

#include "ue3/playlist_names.h"

#include <string>

using namespace ue3;

namespace {

ConfigFile File(const std::string& name, bool wide = false) {
  ConfigFile f;
  f.name = ConfigString::FromUtf8(name, wide);
  return f;
}
ConfigSection Section(const std::string& name, bool wide = false) {
  ConfigSection s;
  s.name = ConfigString::FromUtf8(name, wide);
  return s;
}
const ConfigSection* Find(const ConfigFile& f, const std::string& name) {
  for (const ConfigSection& s : f.sections)
    if (s.name.Utf8() == name) return &s;
  return nullptr;
}
std::string Value(const ConfigSection* s, const std::string& key) {
  if (!s) return "<no section>";
  for (const ConfigKey& k : s->keys)
    if (k.key.Utf8() == key) return k.value.Utf8();
  return "<no key>";
}

const char kQuick[] = "DefaultDLC4Guardian GearOnlinePlaylistProvider";
const char kRanked[] = "DefaultDLC4GuardianRanked GearOnlinePlaylistProvider";

}  // namespace

TEST_CASE("playlist names: the Guardian playlists are named in GearGame.int", "[playlist_names]") {
  ConfigBundle b;
  b.files.push_back(File("..\\GearGame\\Localization\\INT\\GearGame.int", true));
  ConfigSection war = Section("DefaultWar GearOnlinePlaylistProvider", true);
  war.keys.push_back({ConfigString::FromUtf8("DisplayName", true), ConfigString::FromUtf8("Warzone", true)});
  b.files[0].sections.push_back(war);

  CHECK(AddMissingPlaylistNames(b) == 3);
  const ConfigFile& f = b.files[0];
  CHECK(Value(Find(f, kQuick), "DisplayName") == "Guardian");
  CHECK(Value(Find(f, kQuick), "PlaylistDescription") != "<no key>");
  CHECK(Value(Find(f, kRanked), "DisplayName") == "Guardian");
  CHECK(Find(f, kQuick)->name.wide);                                   // the file's own encoding
  CHECK(Value(Find(f, "DefaultWar GearOnlinePlaylistProvider"), "DisplayName") == "Warzone");  // others untouched

  CHECK(AddMissingPlaylistNames(b) == 0);  // a second pass adds nothing
  CHECK(Find(f, kQuick)->keys.size() == 2);
}

TEST_CASE("playlist names: other languages, and only GearGame's text file", "[playlist_names]") {
  ConfigBundle b;
  b.files.push_back(File("..\\GearGame\\Localization\\FRA\\geargame.fra"));
  b.files.push_back(File("..\\GearGame\\Localization\\FRA\\GearGameUI.fra"));
  b.files.push_back(File("..\\GearGame\\Config\\Xbox360-GearGame.ini"));
  b.files.push_back(File("UPDATE:\\TU__geargame.ini_us"));
  b.files.push_back(File("..\\GearGame\\Config\\GearGame.ini"));
  CHECK(AddMissingPlaylistNames(b) == 3);
  CHECK(Value(Find(b.files[0], kRanked), "DisplayName") == "Guardian");
  for (size_t i = 1; i < b.files.size(); ++i) CHECK(b.files[i].sections.empty());
}

TEST_CASE("playlist names: a blank value is filled, not duplicated", "[playlist_names]") {
  ConfigBundle b;
  b.files.push_back(File("GearGame.int"));
  ConfigSection s = Section(kRanked);
  s.keys.push_back({ConfigString::FromUtf8("DisplayName", false), ConfigString::FromUtf8("", false)});
  b.files[0].sections.push_back(s);
  CHECK(AddMissingPlaylistNames(b) == 3);
  CHECK(Find(b.files[0], kRanked)->keys.size() == 1);
  CHECK(Value(Find(b.files[0], kRanked), "DisplayName") == "Guardian");
}

TEST_CASE("playlist names: the added strings survive writing and reading the bundle", "[playlist_names]") {
  for (const bool wide : {false, true}) {
    ConfigBundle b;
    b.files.push_back(File("..\\GearGame\\Localization\\INT\\GearGame.int", wide));
    ConfigSection war = Section("DefaultWar GearOnlinePlaylistProvider", wide);
    war.keys.push_back({ConfigString::FromUtf8("DisplayName", wide), ConfigString::FromUtf8("Warzone", wide)});
    b.files[0].sections.push_back(war);
    REQUIRE(AddMissingPlaylistNames(b) == 3);
    ConfigBundle back;
    std::string err;
    REQUIRE(ParseConfigBundle(WriteConfigBundle(b), back, &err));
    REQUIRE(back.files.size() == 1);
    CHECK(Value(Find(back.files[0], kQuick), "DisplayName") == "Guardian");
    CHECK(Value(Find(back.files[0], kRanked), "DisplayName") == "Guardian");
    CHECK(Value(Find(back.files[0], "DefaultWar GearOnlinePlaylistProvider"), "DisplayName") == "Warzone");
  }
}

TEST_CASE("playlist names: a name already there (a mod's) is kept", "[playlist_names]") {
  ConfigBundle b;
  b.files.push_back(File("GearGame.int"));
  ConfigSection s = Section(kQuick);
  s.keys.push_back({ConfigString::FromUtf8("DisplayName", false), ConfigString::FromUtf8("Leader Hunt", false)});
  b.files[0].sections.push_back(s);
  CHECK(AddMissingPlaylistNames(b) == 2);  // the description and the ranked name
  CHECK(Value(Find(b.files[0], kQuick), "DisplayName") == "Leader Hunt");
}
