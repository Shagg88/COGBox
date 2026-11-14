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
        if (n.empty()) n = "Player";
        // 0.6.2 GitHub #302 (a new #number - a new account - every start, friends gone): an account the launcher can't
        // keep in identity.json is a new one at every start. Two ways that happened:
        // 1. identity.json without its token: a game writes the file before its claim, and a claim that never completed
        //    (server not reached, game closed) leaves it so. The launcher's own claim then failed to write ("wx": the
        //    file exists) and ran on an account kept in memory only. Now the launcher completes that file: a claim for
        //    its uuid (the server makes the account under it, with a token) written over the file. If the server already
        //    knows that uuid (its token is lost), the file is set aside (kept) and a new account is made and saved.
        // 2. A profile folder that can't be written: no claim at all (each start would leave another account on the
        //    server) - the reason is shown on the Social tab instead.
        if (auto part = read_account_partial(st->dir); part && part->token.empty()) {
            const std::string body = nlohmann::json{ { "uuid", part->uuid }, { "display_name", n } }.dump();
            const HttpResult r = social_http(*st->url, { "POST", "/api/v1/auth/claim", body }, nullptr);
            if (auto c = r.status == 200 ? parse_claim(r.body) : std::nullopt) {
                if (!write_account_replace(st->dir, *c)) {
                    st->given_up = true;
                    st->error = "Couldn't save the online account in the profile folder (" + st->dir + ").";
                    return st->acc;
                }
                st->acc = c;
                return st->acc;
            }
            if (r.status != 401) {   // not reached, or refused for another reason: the file stays as it is
                if (!claim_retry(r.status)) {
                    st->given_up = true;
                    st->error = server_error(r.body);
                    if (st->error.empty()) st->error = "The online server refused the profile's account.";
                }
                return st->acc;
            }
            if (!set_account_aside(st->dir, "no-token")) {
                st->given_up = true;
                st->error = "The profile's online account file can't be replaced (" + st->dir + "\\identity.json).";
                return st->acc;
            }
        }
        if (!account_dir_writable(st->dir)) {
            st->given_up = true;
            st->error = "COGBox can't save the online account in the profile folder (" + st->dir + "), so friends would be "
                        "lost at every start. Move COGBox to a folder you can write to (not Program Files).";
            return st->acc;
        }
        const std::string body = nlohmann::json{ { "display_name", n } }.dump();
        const HttpResult r = social_http(*st->url, { "POST", "/api/v1/auth/claim", body }, nullptr);
        if (auto c = r.status == 200 ? parse_claim(r.body) : std::nullopt) {
            if (!write_account_exclusive(st->dir, *c)) {
                if (auto f = read_account(st->dir)) c = f;   // a game made one at the same moment: its own wins
                else if (!write_account_replace(st->dir, *c)) st->error = "Couldn't save the online account in the profile folder.";
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

