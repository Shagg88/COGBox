// Layer 2 (Unreal Engine 3 on the 360). May include layer 0-1 headers only.
#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/logging/macros.h>

// Profile debugging tools, one copy for all four games since the 23 Nov parity pass (Shagg 10:42: "port profile_100
// and profile_log to all four games"). Each game hooks its own C++ profile-setting getter (GetProfileSettingValueInt:
// self r3, id r4, value pointer r5; returns found in bit 0) - the addresses and how each game finds it stay in its own
// unlocks.cpp / profile_log.cpp; these are what runs after the original returned.
//  - ProfileLogSeen: --<game>_profile_log's "once per change" filter (key = whatever tells the readers apart - the
//    object's vtable or the caller - and the id). Read-only.
//  - Profile100: --<game>_profile_100 / _profile_100_file (test only): the id=value lines of the file are written into
//    the profile settings array of the object the getter reads, so the game's next profile save writes them itself
//    (Gears 2's g2-100 / Judgment's gj100). The array is found on the first read of a listed id: a TArray {ptr, count,
//    max} in the object whose elements (stride 12..32) hold that id and the value the getter returned; every write is
//    logged; values are applied again when the array moves.
namespace ue3 {

inline uint32_t ProfileLoad32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void ProfileStore32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// True the first time (key, id) is seen with this value (at most 4096 keys). Thread-safe.
class ProfileLogSeen {
 public:
  bool First(uint32_t key, int32_t id, uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t k = (uint64_t(key) << 32) | uint32_t(id);
    auto it = seen_.find(k);
    if (it != seen_.end() && it->second == value) return false;
    if (seen_.size() >= 4096 && it == seen_.end()) return false;
    seen_[k] = value;
    return true;
  }

 private:
  std::mutex mutex_;
  std::unordered_map<uint64_t, uint32_t> seen_;
};

class Profile100 {
 public:
  // Reads `path` ("id=value" lines, decimal or 0x hex, '#' comments). False (and logged): off.
  bool Load(const char* option, const std::string& path) {
    option_ = option;
    std::ifstream in{path};
    std::string line;
    while (std::getline(in, line)) {
      line = line.substr(0, line.find('#'));
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      try {
        values_.emplace_back(int32_t(std::stol(line.substr(0, eq), nullptr, 0)),
                             uint32_t(std::stoul(line.substr(eq + 1), nullptr, 0)));
      } catch (...) {
      }
    }
    REXLOG_INFO("{}: {} value(s) from '{}'", option_, values_.size(), path);
    if (values_.empty()) REXLOG_WARN("{}: off - no id=value lines in the file", option_);
    on_ = !values_.empty();
    return on_;
  }
  // The audio menu (ue3/audio_menu.h): the values come from cvars instead of a file. Empty = off.
  bool Set(const char* option, std::vector<std::pair<int32_t, uint32_t>> values) {
    std::lock_guard<std::mutex> lock(mutex_);
    option_ = option;
    values_ = std::move(values);
    on_ = !values_.empty();
    return on_;
  }
  // The player changed a listed value in the game's own menu: keep it, so a later re-apply (the array moved) writes the
  // player's value, not the start-up one.
  void Update(int32_t id, uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [want_id, want] : values_)
      if (want_id == id) want = value;
  }
  bool On() const { return on_; }
  size_t Count() const { return values_.size(); }

  // After the getter returned `found` (its r3) for (self, id, value_addr). `override_read`: also give this read the
  // listed value (Judgment's gj100 did; Gears 2's g2-100 did not).
  void OnRead(uint8_t* base, uint32_t self, int32_t id, uint32_t value_addr, uint32_t found, bool override_read) {
    if (!on_ || !value_addr || !(found & 1)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [want_id, want] : values_) {
      if (want_id != id) continue;
      Apply(base, self, id, ProfileLoad32(base, value_addr));
      if (override_read) ProfileStore32(base, value_addr, want);
      break;
    }
  }

 private:
  static bool GuestPtr(uint32_t p) { return p >= 0x40000000u && p < 0xA0000000u; }
  void Apply(uint8_t* base, uint32_t self, int32_t id, uint32_t value) {
    if (!lay_.ok && !lay_.failed) {
      for (uint32_t f = 0x20; f < 0x400 && !lay_.ok; f += 4) {
        const uint32_t ptr = ProfileLoad32(base, self + f), cnt = ProfileLoad32(base, self + f + 4),
                       max = ProfileLoad32(base, self + f + 8);
        if (!GuestPtr(ptr) || cnt == 0 || cnt > 4096 || max < cnt) continue;
        for (uint32_t st = 12; st <= 32 && !lay_.ok; st += 4)
          for (uint32_t io = 0; io <= 8 && !lay_.ok; io += 4) {
            bool sane = true;
            for (uint32_t i = 0; i < cnt && sane; ++i) sane = ProfileLoad32(base, ptr + i * st + io) < 0x10000u;
            if (!sane) continue;
            for (uint32_t i = 0; i < cnt && !lay_.ok; ++i) {
              if (ProfileLoad32(base, ptr + i * st + io) != uint32_t(id)) continue;
              for (uint32_t vo = io + 4; vo + 4 <= st && !lay_.ok; vo += 4)
                if (ProfileLoad32(base, ptr + i * st + vo) == value) lay_ = {f, st, io, vo, true, false};
            }
          }
      }
      if (!lay_.ok) {
        lay_.failed = true;
        REXLOG_WARN("{}: settings array not found in object {:08X} (id {} value 0x{:X})", option_, self, id, value);
        return;
      }
      REXLOG_INFO("{}: settings array at object+0x{:X}, stride {}, id +{}, value +{}", option_, lay_.field, lay_.stride,
                  lay_.id_off, lay_.val_off);
    }
    if (!lay_.ok) return;
    const uint32_t ptr = ProfileLoad32(base, self + lay_.field), cnt = ProfileLoad32(base, self + lay_.field + 4);
    if (!GuestPtr(ptr) || cnt > 4096 || (ptr == last_ptr_ && cnt == last_count_)) return;
    last_ptr_ = ptr, last_count_ = cnt;
    int written = 0, missing = 0;
    for (auto& [want_id, want] : values_) {
      bool hit = false;
      for (uint32_t i = 0; i < cnt; ++i) {
        const uint32_t e = ptr + i * lay_.stride;
        if (ProfileLoad32(base, e + lay_.id_off) != uint32_t(want_id)) continue;
        const uint32_t old = ProfileLoad32(base, e + lay_.val_off);
        ProfileStore32(base, e + lay_.val_off, want);
        REXLOG_INFO("{}: id {} 0x{:08X} -> 0x{:08X}", option_, want_id, old, want);
        hit = true, ++written;
        break;
      }
      if (!hit) {
        ++missing;
        REXLOG_WARN("{}: id {} not in the settings array", option_, want_id);
      }
    }
    REXLOG_INFO("{}: {} written, {} missing (array {:08X} x{}); saved with the next profile save", option_, written,
                missing, ptr, cnt);
  }

  struct Layout { uint32_t field = 0, stride = 0, id_off = 0, val_off = 0; bool ok = false, failed = false; };
  std::string option_ = "profile_100";
  std::vector<std::pair<int32_t, uint32_t>> values_;
  Layout lay_;
  uint32_t last_ptr_ = 0, last_count_ = 0;
  bool on_ = false;
  std::mutex mutex_;
};

}  // namespace ue3
