// GitHub #353: see xenos/player_tags.h. The game's one cache, fed by a background thread that asks the master server
// for the XUIDs the game has wanted since (one POST per batch; nothing while every wanted XUID is answered).
#include <xenos/player_tags.h>

#include <rex/kernel/xam/net/identity.h>
#include <rex/kernel/xam/net/master_client.h>
#include <rex/logging/macros.h>

#include "../../cogbox/third_party/nlohmann/json.hpp"

#include <chrono>
#include <mutex>
#include <thread>

namespace {
namespace net = rex::kernel::xam::net;

std::mutex g_mu;
xenos::tags::Cache g_cache;
std::once_flag g_started;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Online() {
  if (!net::MasterClient::Get().Configured()) return false;
  const net::ClientIdentity id = net::GetClientIdentity();
  return !id.uuid.empty() && !id.token.empty();   // claimed: the server answers signed-in accounts only
}

void Worker() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::vector<uint64_t> batch;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      batch = g_cache.TakeBatch(NowMs());
    }
    if (batch.empty()) continue;
    nlohmann::json body = {{"xuids", nlohmann::json::array()}};
    for (uint64_t x : batch) body["xuids"].push_back(std::to_string(x));
    const net::MasterClient::Result r = net::MasterClient::Get().Call("POST", "/api/v1/players/tags", body.dump());
    const auto j = r.status == 200 ? nlohmann::json::parse(r.body, nullptr, false) : nlohmann::json();
    std::lock_guard<std::mutex> lock(g_mu);
    if (j.is_discarded() || !j.is_object() || !j.contains("tags") || !j["tags"].is_array()) {
      g_cache.Failed(batch, NowMs());
      REXLOG_DEBUG("player tags: {} XUIDs not answered (HTTP {})", batch.size(), r.status);
      continue;
    }
    std::vector<std::pair<uint64_t, xenos::tags::Tag>> found;
    for (const auto& t : j["tags"]) {
      if (!t.is_object() || !t.contains("xuid") || !t["xuid"].is_string()) continue;
      const uint64_t xuid = std::strtoull(t["xuid"].get<std::string>().c_str(), nullptr, 10);
      const std::string name = t.value("display_name", std::string()), disc = t.value("discriminator", std::string());
      if (xuid && !name.empty() && !disc.empty()) found.push_back({xuid, {name, disc}});
    }
    g_cache.Answer(batch, found, NowMs());
    REXLOG_INFO("player tags: {} of {} players in the match have a COGBox tag", found.size(), batch.size());
  }
}
}  // namespace

bool XenosPlayerTagsOnline() { return Online(); }

std::optional<xenos::tags::Tag> XenosPlayerTag(uint64_t xuid) {
  if (!xuid || !Online()) return std::nullopt;
  std::call_once(g_started, [] { std::thread(Worker).detach(); });
  std::lock_guard<std::mutex> lock(g_mu);
  return g_cache.Want(xuid, NowMs());
}
