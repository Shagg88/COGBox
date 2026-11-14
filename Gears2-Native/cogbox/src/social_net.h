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
// 0.6.2 (GitHub #302): identity.json as it is, with or without its token (a game writes it before its claim succeeds).
std::optional<SocialAccount> read_account_partial(const std::string& identity_dir_utf8);
// The whole account written over identity.json (new file, swapped in) - for a claim that completed a token-less file.
bool write_account_replace(const std::string& identity_dir_utf8, const SocialAccount& a);
// identity.json set aside as identity.json.<suffix>-<date-time> (kept, never deleted or replaced); true when there is
// no identity.json after.
bool set_account_aside(const std::string& identity_dir_utf8, const std::string& suffix);
// Whether a file can be made in the profile folder (tried with a scratch file, removed again).
bool account_dir_writable(const std::string& identity_dir_utf8);
// identity.json's read-only flag cleared (a game or a copy may have set it).
void clear_account_read_only(const std::string& identity_dir_utf8);
// Seconds since identity.json was last written; -1 when there is none (a date in the future counts as long ago).
int account_file_age_seconds(const std::string& identity_dir_utf8);

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
