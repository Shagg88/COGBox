// Layer 2 (Unreal Engine 3 on the 360). May include layer 0-1 headers only.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <ue3/profile_tools.h>

// The game's own audio menu from the launcher (Shagg 23 Nov), one copy for all four games. Each game's menu has the same
// three sliders - music, effects, dialogue - kept as int profile settings (Gears 1's WarProfileSettings MusicVolume /
// FxVolume / DialogueVolume, the later games' GearProfileSettings). Each game passes its own ids (its table, or
// --<game>_audio_menu_ids) and the launcher's values (--<game>_audio_music / _effects / _dialogue, -1 = untouched), from
// the profile getter hook its unlocks.cpp / profile_log.cpp already has.
//  - Start-up: each value the launcher set is written into the profile settings array (Profile100's array search) the
//    first time the game reads it, and that read gets it too; the game's next profile save keeps it.
//  - Read-back: every read of a slider is logged once per change, as
//        <game>_audio_menu: now <key>=<value> (id <id>)
//    The launcher reads the LAST such line per key from the game's log when the game closes and takes it as its own
//    value - so a change made in the game's menu is kept, not overwritten at the next start (see docs/audio.md).
//  - A value the player changes in the game's menu also replaces the start-up value here, so if the game moves the
//    array later (a profile reload), the player's value is the one written again.
namespace ue3 {

struct AudioMenuSlider {
  const char* key;   // "music", "effects", "dialogue" (the launcher's names)
  int32_t id;        // the game's profile setting id; -1 = not known for this game yet
};
constexpr int kAudioMenuSliders = 3;

class AudioMenu {
 public:
  // option: "<game>_audio_menu". ids_override: "music,effects,dialogue" ids ("" = the table). values: -1 = untouched.
  void Setup(const char* option, const AudioMenuSlider (&table)[kAudioMenuSliders], const std::string& ids_override,
             const int32_t (&values)[kAudioMenuSliders]) {
    option_ = option;
    for (int i = 0; i < kAudioMenuSliders; ++i) slider_[i] = table[i];
    if (!ids_override.empty()) {
      size_t pos = 0;
      for (int i = 0; i < kAudioMenuSliders && pos <= ids_override.size(); ++i) {
        const size_t comma = ids_override.find(',', pos);
        const std::string part = ids_override.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!part.empty()) slider_[i].id = int32_t(std::strtol(part.c_str(), nullptr, 0));
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    }
    std::vector<std::pair<int32_t, uint32_t>> write;
    for (int i = 0; i < kAudioMenuSliders; ++i) {
      known_ |= slider_[i].id >= 0;
      values_[i] = values[i];
      if (values[i] >= 0 && slider_[i].id >= 0) write.emplace_back(slider_[i].id, uint32_t(values[i]));
      else if (values[i] >= 0)
        REXLOG_WARN("{}: {}={} not applied - this game's id for it is not known yet ({}_ids)", option_, slider_[i].key,
                    values[i], option_);
    }
    writer_.Set(option, std::move(write));
    REXLOG_INFO("{}: ids music={} effects={} dialogue={}; launcher values music={} effects={} dialogue={} (-1 = the "
                "game's own)", option_, slider_[0].id, slider_[1].id, slider_[2].id, values[0], values[1], values[2]);
  }
  bool On() const { return known_; }   // something to read back (the getter hook is needed)

  // After the getter returned `found` (its r3) for (self, id, value_addr).
  // Returns true when the caller must report the setting as found (its r3 = 1): job 2378j - Judgment / Gears 3 read 41-43
  // from a profile that has no such entry yet (found = 0, the game's default used), so nothing could be written into it.
  bool OnRead(uint8_t* base, uint32_t self, int32_t id, uint32_t value_addr, uint32_t found) {
    if (!known_ || !value_addr) return false;
    int k = -1;
    for (int i = 0; i < kAudioMenuSliders; ++i)
      if (slider_[i].id >= 0 && slider_[i].id == id) k = i;
    if (k < 0) return false;
    if (!(found & 1)) {   // not in this profile: give the launcher's value (each read), the game's default otherwise
      if (values_[k] < 0) return false;
      ProfileStore32(base, value_addr, uint32_t(values_[k]));
      std::lock_guard<std::mutex> lock(mutex_);
      if (!missing_logged_[k]) {
        missing_logged_[k] = true;
        REXLOG_INFO("{}: {} (id {}) not in this profile - the launcher's {} given to the game", option_, slider_[k].key, id,
                    values_[k]);
      }
      return true;
    }
    bool first;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      first = !seen_[k];
    }
    // The first read also gets the launcher's value; later reads show the array as it is (the player's menu changes)
    writer_.OnRead(base, self, id, value_addr, found, first);
    const uint32_t value = ProfileLoad32(base, value_addr);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (seen_[k] && last_[k] == value) return false;
      if (seen_[k]) writer_.Update(id, value);   // changed in the game's menu (or by the game): that value stays
      seen_[k] = true, last_[k] = value;
    }
    REXLOG_INFO("{}: now {}={} (id {})", option_, slider_[k].key, int32_t(value), id);
    return false;
  }

 private:
  std::string option_ = "audio_menu";
  AudioMenuSlider slider_[kAudioMenuSliders] = {{"music", -1}, {"effects", -1}, {"dialogue", -1}};
  bool known_ = false;
  Profile100 writer_;
  std::mutex mutex_;
  bool seen_[kAudioMenuSliders] = {};
  uint32_t last_[kAudioMenuSliders] = {};
  int32_t values_[kAudioMenuSliders] = {-1, -1, -1};
  bool missing_logged_[kAudioMenuSliders] = {};
};

}  // namespace ue3

// The same four cvars in every game, with its prefix (gow1 / gow2 / gow3 / gowj). Use once, at file scope, in the file
// that has the profile getter hook. The launcher finds "<prefix>_audio_menu_ids" in the exe before offering the sliders.
#define UE3_AUDIO_MENU_CVARS(p, category)                                                                              \
  REXCVAR_DEFINE_INT32(p##_audio_music, -1, category, "The game's own Music volume, set at start (-1 = the game's own)"); \
  REXCVAR_DEFINE_INT32(p##_audio_effects, -1, category,                                                                \
                       "The game's own Effects volume, set at start (-1 = the game's own)");                           \
  REXCVAR_DEFINE_INT32(p##_audio_dialogue, -1, category,                                                               \
                       "The game's own Dialogue volume, set at start (-1 = the game's own)");                          \
  REXCVAR_DEFINE_STRING(p##_audio_menu_ids, "", category,                                                              \
                        "Profile setting ids of the audio menu's music,effects,dialogue sliders (\"\" = this build's own)")

// Setup(menu, from the four cvars above). `table`: the game's AudioMenuSlider[3].
#define UE3_AUDIO_MENU_SETUP(menu, p, table)                                                                           \
  do {                                                                                                                 \
    const int32_t ue3_audio_values_[ue3::kAudioMenuSliders] = {int32_t(REXCVAR_GET(p##_audio_music)),                 \
                                                               int32_t(REXCVAR_GET(p##_audio_effects)),               \
                                                               int32_t(REXCVAR_GET(p##_audio_dialogue))};             \
    (menu).Setup(#p "_audio_menu", table, REXCVAR_GET(p##_audio_menu_ids), ue3_audio_values_);                         \
  } while (0)

