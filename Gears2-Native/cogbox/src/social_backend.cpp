// CogBox - the launcher's social backend (see social_client.h): WinHTTP to the multiplayer server and the profile's
// identity.json, claimed once. The games use the SDK's MasterClient instead (layer0_xenos/social_ingame.cpp).
#include "social_client.h"
#include "social_net.h"

#include "../third_party/nlohmann/json.hpp"

#include <memory>
#include <mutex>

namespace {
std::mutex g_name_mu;
std::string g_claim_name;   // the profile's name for a claim (SAVE NAME changes it before the claim is made)

// The launcher's account: the profile's identity.json (a game may have made it), else ONE claim under the profile's
// name. Kept in memory even when the file can't be written: never a second account. Retried only when the server did
// not answer (no account was made then).
struct LauncherAccount {
    std::mutex mu;
    std::optional<ServerUrl> url;
    std::string dir;
    std::shared_ptr<std::string> name;
    std::optional<SocialAccount> acc;
    bool given_up = false;
    std::string error;
};
}  // namespace

SocialBackend launcher_backend(const std::string& server_url, const std::string& identity_dir, const std::string& name) {
    auto st = std::make_shared<LauncherAccount>();
    st->url = parse_server_url(server_url);
    st->dir = identity_dir;
    st->name = std::make_shared<std::string>(name);
    SocialBackend b;
    b.account = [st]() -> std::optional<SocialAccount> {
        std::lock_guard<std::mutex> lk(st->mu);   // the worker is the only caller; the lock guards `renamed`
        if (st->acc || !st->url) return st->acc;
        st->acc = read_account(st->dir);
        if (st->acc || st->given_up) return st->acc;
        std::string n;
        {
            std::lock_guard<std::mutex> g(g_name_mu);
            n = !g_claim_name.empty() ? g_claim_name : *st->name;
        }
        const std::string body = nlohmann::json{ { "display_name", n.empty() ? std::string("Player") : n } }.dump();
        const HttpResult r = social_http(*st->url, { "POST", "/api/v1/auth/claim", body }, nullptr);
        if (auto c = r.status == 200 ? parse_claim(r.body) : std::nullopt) {
            if (!write_account_exclusive(st->dir, *c)) {
                if (auto f = read_account(st->dir)) c = f;   // a game made one at the same moment: its own wins
                else st->error = "Couldn't save the online account in the profile folder.";
            }
            st->acc = c;
        } else if (!claim_retry(r.status)) {
            st->given_up = true;
            st->error = server_error(r.body);
            if (st->error.empty()) st->error = "The online server refused a new account.";
        }
        return st->acc;
    };
    b.account_error = [st] {
        std::lock_guard<std::mutex> lk(st->mu);
        return st->error;
    };
    b.call = [st](const SocialCall& c) -> HttpResult {
        std::optional<SocialAccount> a;
        {
            std::lock_guard<std::mutex> lk(st->mu);
            a = st->acc;
        }
        if (!st->url || !a) return {};
        return social_http(*st->url, c, &*a);
    };
    b.renamed = [st](const std::string& n, const std::string& d) {
        std::lock_guard<std::mutex> lk(st->mu);
        update_account_name(st->dir, n, d);
        if (st->acc) {
            st->acc->name = n;
            st->acc->discriminator = d;
        }
    };
    return b;
}

void social_start(const std::string& server_url, const std::string& identity_dir, const std::string& name) {
    {
        std::lock_guard<std::mutex> lk(g_name_mu);
        g_claim_name = name;   // a claim not made yet uses the newest name
    }
    if (server_url.empty()) {
        social_sign_off(0);
        return;
    }
    social_start_backend(server_url + "|" + identity_dir, launcher_backend(server_url, identity_dir, name));
}

