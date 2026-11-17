// Layer 0 (game-free): GitHub #353 - the COGBox tag (name#discriminator) of each player in a match, by the XUID the game
// knows them by, so layer 3 can show it after their name (lobby, scoreboard). The master server answers
// POST /api/v1/players/tags in batches; unknown XUIDs are asked once, in the background, and remembered for the session.
// Cache is the pure bookkeeping (no network, no clock of its own: times are milliseconds the caller passes in).
// Our code (MIT).
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xenos::tags {

struct Tag {
  std::string name, discriminator;
};

class Cache {
 public:
  static constexpr size_t kBatch = 32;                    // the server's MAX_TAG_LOOKUP
  static constexpr int64_t kNoAccountRetryMs = 5 * 60 * 1000;  // no COGBox account (bots, guests): ask again much later
  static constexpr int64_t kFailedRetryMs = 30 * 1000;    // the request failed (offline, server busy)

  // The tag if known; otherwise the XUID is noted for the next batch. XUID 0 (no online id) is never asked.
  std::optional<Tag> Want(uint64_t xuid, int64_t now) {
    if (!xuid) return std::nullopt;
    auto [it, added] = entries_.try_emplace(xuid);
    if (added) it->second.retry_at = now;
    if (it->second.state == State::kKnown) return it->second.tag;
    return std::nullopt;
  }
  // Up to kBatch XUIDs due to be asked; they are in flight until Answer / Failed.
  std::vector<uint64_t> TakeBatch(int64_t now) {
    std::vector<uint64_t> out;
    for (auto& [xuid, e] : entries_) {
      if (out.size() == kBatch) break;
      if ((e.state == State::kWanted || e.state == State::kNoAccount || e.state == State::kFailed) && e.retry_at <= now) {
        e.state = State::kInFlight;
        out.push_back(xuid);
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }
  void Answer(const std::vector<uint64_t>& asked, const std::vector<std::pair<uint64_t, Tag>>& found, int64_t now) {
    for (uint64_t x : asked) {
      auto it = entries_.find(x);
      if (it == entries_.end()) continue;
      it->second.state = State::kNoAccount;
      it->second.retry_at = now + kNoAccountRetryMs;
    }
    for (const auto& [x, tag] : found) {
      Entry& e = entries_[x];
      e.state = State::kKnown;
      e.tag = tag;
    }
  }
  void Failed(const std::vector<uint64_t>& asked, int64_t now) {
    for (uint64_t x : asked) {
      auto it = entries_.find(x);
      if (it == entries_.end()) continue;
      it->second.state = State::kFailed;
      it->second.retry_at = now + kFailedRetryMs;
    }
  }

 private:
  enum class State { kWanted, kInFlight, kKnown, kNoAccount, kFailed };
  struct Entry {
    State state = State::kWanted;
    int64_t retry_at = 0;
    Tag tag;
  };
  std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace xenos::tags

// The game's shared cache: the tag for this XUID if known, else asked in the background (never blocks). Empty when the
// game is not online with a claimed COGBox account.
std::optional<xenos::tags::Tag> XenosPlayerTag(uint64_t xuid);
// True when the game is online with a claimed COGBox account (tags can be had at all).
bool XenosPlayerTagsOnline();
