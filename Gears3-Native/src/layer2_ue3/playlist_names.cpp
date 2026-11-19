#include "ue3/playlist_names.h"

#include <string>

namespace ue3 {
namespace {

// The GearGame localisation file: "...\Localization\INT\GearGame.int", "geargame.fra" - "geargame." plus a
// three-letter language code, so not GearGame.ini (or a region-suffixed "geargame.ini_us") or GearGameUI.int.
bool IsGearGameText(const ConfigFile& f) {
  std::string n = AsciiLower(f.name.Utf8());
  const size_t slash = n.find_last_of("\\/");
  if (slash != std::string::npos) n = n.substr(slash + 1);
  if (n.size() != 12 || n.rfind("geargame.", 0) != 0 || n == "geargame.ini") return false;
  for (size_t i = 9; i < 12; ++i)
    if (n[i] < 'a' || n[i] > 'z') return false;
  return true;
}

struct Entry {
  const char* section;
  const char* key;
  const char* value;
};
// English for every language: the game never shipped a translation of these.
constexpr Entry kEntries[] = {
    {"DefaultDLC4Guardian GearOnlinePlaylistProvider", "DisplayName", "Guardian"},
    {"DefaultDLC4Guardian GearOnlinePlaylistProvider", "PlaylistDescription",
     "Protect your leader. While the leader lives, your team respawns (up to 10 players)."},
    {"DefaultDLC4GuardianRanked GearOnlinePlaylistProvider", "DisplayName", "Guardian"},
};

}  // namespace

int AddMissingPlaylistNames(ConfigBundle& bundle) {
  int added = 0;
  for (ConfigFile& f : bundle.files) {
    if (!IsGearGameText(f)) continue;
    const bool wide = f.name.wide;
    for (const Entry& e : kEntries) {
      ConfigSection* sec = nullptr;
      for (ConfigSection& s : f.sections)
        if (AsciiLower(s.name.Utf8()) == AsciiLower(e.section)) sec = &s;
      if (!sec) {
        f.sections.push_back({});
        sec = &f.sections.back();
        sec->name = ConfigString::FromUtf8(e.section, wide);
      }
      ConfigKey* empty = nullptr;
      bool has = false;
      for (ConfigKey& k : sec->keys)
        if (AsciiLower(k.key.Utf8()) == AsciiLower(e.key)) {
          if (k.value.Utf8().empty()) empty = &k;
          else has = true;
        }
      if (has) continue;
      if (empty) empty->value = ConfigString::FromUtf8(e.value, wide);  // fill a blank one rather than add a second
      else sec->keys.push_back({ConfigString::FromUtf8(e.key, wide), ConfigString::FromUtf8(e.value, wide)});
      ++added;
    }
  }
  return added;
}

}  // namespace ue3
