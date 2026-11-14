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

// The launcher's account: the profile's identity.json (a game may have made it, or left it without its token), else
// ONE claim under the profile's name, used only once it is saved there (0.6.2, GitHub #302). Asked again on later
// calls while a game may be writing the file or the server did not answer.
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
        // keep in identity.json is a new one at every start, so the launcher never runs on an account it hasn't saved:
        // 1. A folder that can't be written: no claim at all (each start would leave another account on the server);
        //    the reason is shown on the Social tab.
        // 2. identity.json without its token: a game writes the file before its claim, and a claim that never completed
        //    (server not reached, game closed) leaves it so. The launcher completes it with a claim for its own uuid. A
        //    file changed in the last 30 s is left alone (a game's claim may be on its way; this is asked again later).
        //    If the server already knows that uuid and the file still has no token, it is set aside (kept) and a new
        //    account is made.
        // 3. An unreadable identity.json is set aside (kept) before a new account is made.
        if (!account_dir_writable(st->dir)) {
            st->given_up = true;
            st->error = "COGBox can't save the online account in the profile folder (" + st->dir + "), so friends would be "
                        "lost at every start. Move COGBox to a folder you can write to (not Program Files).";
            return st->acc;
        }
        const int age = account_file_age_seconds(st->dir);
        if (auto part = read_account_partial(st->dir); part && part->token.empty()) {
            if (age >= 0 && age < 30) return st->acc;   // a game may be claiming it right now
            clear_account_read_only(st->dir);   // r6 RC2: a read-only file would take the claim and not its token
            const std::string body = nlohmann::json{ { "uuid", part->uuid }, { "display_name", n } }.dump();
            const HttpResult r = social_http(*st->url, { "POST", "/api/v1/auth/claim", body }, nullptr);
            if (auto f = read_account(st->dir)) { st->acc = f; return st->acc; }   // a game completed it meanwhile: its own
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
            const auto again = read_account_partial(st->dir);
            if (!again || again->uuid != part->uuid || !again->token.empty()) return st->acc;   // changed meanwhile: look again later
            if (!set_account_aside(st->dir, "no-token")) {
                st->given_up = true;
                st->error = "The profile's online account file can't be replaced (" + st->dir + "\\identity.json).";
                return st->acc;
            }
        } else if (!part && age >= 0) {   // there, but unreadable (or not an account)
            // (a file dated in the future - a clock set back - counts as old)
            if (age < 30) return st->acc;   // (a game may be writing it)
            if (!set_account_aside(st->dir, "unreadable")) {
                st->given_up = true;
                st->error = "The profile's online account file can't be read or replaced (" + st->dir + "\\identity.json).";
                return st->acc;
            }
        }
        const std::string body = nlohmann::json{ { "display_name", n } }.dump();
        const HttpResult r = social_http(*st->url, { "POST", "/api/v1/auth/claim", body }, nullptr);
        if (auto c = r.status == 200 ? parse_claim(r.body) : std::nullopt) {
            if (!write_account_exclusive(st->dir, *c)) {
                // a game made one at the same moment: its own wins (a token-less one is completed on a later call)
                if (auto f = read_account(st->dir)) st->acc = f;
                else if (!read_account_partial(st->dir)) {   // r6 RC1: nothing usable there and it can't be made - never claim again
                    st->given_up = true;
                    st->error = "Couldn't save the online account in the profile folder (" + st->dir + "\\identity.json).";
                }
                return st->acc;
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

