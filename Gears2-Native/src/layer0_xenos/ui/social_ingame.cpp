// COGBox social step 5: see xenos/social_ingame.h.
#include <xenos/social_ingame.h>

#include <chrono>
#include <mutex>

#include <rex/kernel/xam/net/identity.h>
#include <rex/kernel/xam/net/master_client.h>
#include <rex/system/kernel_state.h>

namespace {
namespace net = rex::kernel::xam::net;

std::mutex g_mu;
bool g_started = false;
SocialLists g_prev_lists;
std::vector<SocialInvite> g_prev_invites;
SocialEventState g_events;
bool g_have_prev = false;

SocialBackend GameBackend() {
  SocialBackend b;
  b.call = [](const SocialCall& c) -> HttpResult {
    const net::MasterClient::Result r = net::MasterClient::Get().Call(c.method, c.path, c.body);
    return {r.status, r.body};
  };
  b.account = []() -> std::optional<SocialAccount> {
    const net::ClientIdentity id = net::GetClientIdentity();
    if (id.uuid.empty() || id.token.empty()) return std::nullopt;   // not claimed yet (the SDK claims at start)
    return SocialAccount{id.uuid, id.token, id.display_name, id.discriminator, std::to_string(id.authoritative_xuid)};
  };
  return b;
}
}  // namespace

bool XenosSocialAvailable() { return net::MasterClient::Get().Configured(); }

void XenosSocialStart() {
  if (!XenosSocialAvailable()) return;
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_started) return;
  g_started = true;
  social_start_backend("game", GameBackend());
}

SocialView XenosSocialView() {
  XenosSocialStart();   // online start-up may finish after the menu is installed
  return social_view();
}

std::vector<SocialEvent> XenosSocialTakeEvents() {
  XenosSocialStart();
  const SocialView v = social_view();
  if (!v.reachable) return {};
  std::lock_guard<std::mutex> lock(g_mu);
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  const SocialLists& before = g_have_prev ? g_prev_lists : v.lists;
  const std::vector<SocialInvite>& inv_before = g_have_prev ? g_prev_invites : v.invites;
  std::vector<SocialEvent> out = social_events(before, inv_before, v.lists, v.invites, now, g_events);
  // The game reads its friends once and keeps them until told otherwise (XN_FRIENDS_PRESENCE_CHANGED): also on the
  // first answer, since its own read at start may be older than that.
  if (!g_have_prev || friends_changed(g_prev_lists, v.lists)) {
    if (auto* ks = rex::system::kernel_state()) ks->BroadcastNotification(rex::system::XNotificationID(0x04000001), 0);
  }
  g_prev_lists = v.lists;
  g_prev_invites = v.invites;
  g_have_prev = true;
  return out;
}
