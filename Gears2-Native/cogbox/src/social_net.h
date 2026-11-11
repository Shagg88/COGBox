// CogBox - the launcher's side of the online account (social design step 3): the profile's identity.json (shared
// with the four games through --net_identity_dir), the master server's address and one HTTP call to it.
// test/social_net_test.cpp checks the parts without a network.
#pragma once

// Minutes east of UTC on this PC, daylight saving included.
int local_utc_minutes();
#include "social_model.h"

#include <optional>
#include <string>


// <identity_dir>\identity.json as the games write it; nullopt when missing, unreadable or without uuid / token.
std::optional<SocialAccount> read_account(const std::string& identity_dir_utf8);
// The claim answer (POST /api/v1/auth/claim for a new account): uuid, token, display_name, discriminator, xuid.
std::optional<SocialAccount> parse_claim(const std::string& body);
// Writes <identity_dir>\identity.json only when there is none (a game starting at the same moment may have made one:
// that one is kept, and false is returned). The folder is created.
bool write_account_exclusive(const std::string& identity_dir_utf8, const SocialAccount& a);
// After a rename: the existing identity.json takes the new name and number (every other field kept). False when
// there is no readable identity.json.
bool update_account_name(const std::string& identity_dir_utf8, const std::string& name, const std::string& discriminator);

// The COGBox multiplayer server (Flippage's Oracle VM; Shagg agreed 2026-11-08). CogBox.ini [Online] Server overrides.
constexpr const char* kCogboxServer = "https://cogbox-mp.duckdns.org";

struct ServerUrl {
    std::wstring host;
    int port = 0;
    bool secure = false;
};
// "http://host:port", "https://host[:port]" or "host:port" (http).
std::optional<ServerUrl> parse_server_url(const std::string& url);
// The STUN address the games get with that server: its host, port 3478. "" when the url is not one.
std::string stun_address(const std::string& url);

// One request (5 s timeouts); the account's Authorization header when one is given. Blocking: the worker calls it.
HttpResult social_http(const ServerUrl& server, const SocialCall& call, const SocialAccount* account);
