// CogBox - the SOCIAL tab's worker (social design step 3): one background thread talks to the master server, so the
// launcher's window never waits on the network. It polls the friends list and invites every 5 s (which also keeps the
// player "Online - In Launcher"), runs the tab's actions, and creates the profile's account when there is none.
#pragma once
#include "social_model.h"

#include <functional>
#include <map>

#include <optional>
#include <string>
#include <vector>

struct SocialView {
    bool configured = false;   // the multiplayer server is enabled
    bool have_account = false;
    bool reachable = false;    // the last request was answered
    bool busy = false;         // a first poll or a claim is running
    std::string my_tag;
    std::string error;         // the last action's error (the server's text), "" when it went through
    SocialLists lists;
    std::vector<SocialInvite> invites;
    std::map<std::string, int> region_counts;   // players per region; empty until the server has answered
    std::vector<SocialMatch> matches;           // open matches (GET /api/v1/matches, every 10 s)
    bool matches_known = false;                 // the server has answered that list at least once
};

// Where the worker gets its account and sends its requests. The launcher: WinHTTP plus a claim (launcher_backend);
// the games: the SDK's signed-in MasterClient (social step 5).
struct SocialBackend {
    std::function<HttpResult(const SocialCall&)> call;       // authenticated request; status 0 = no answer
    std::function<std::optional<SocialAccount>()> account;   // the signed-in account, nullopt while there is none
    std::function<std::string()> account_error;              // why there is none (may be empty)
    std::function<void(const std::string& name, const std::string& disc)> renamed;   // after a Rename (may be empty)
};
// (Re)starts the worker on a backend; the same key keeps the running worker.
void social_start_backend(const std::string& key, SocialBackend backend);
SocialBackend launcher_backend(const std::string& server_url, const std::string& identity_dir, const std::string& name);

// (Re)starts the worker for a profile: empty server_url = offline (the worker stops). name: the account name a new
// account gets (the profile's name). Called at start, on a profile switch and when the server option changes.
void social_start(const std::string& server_url, const std::string& identity_dir, const std::string& name);
void social_stop();
bool social_stop_wait(int max_ms);   // 0.6.0: social_stop, then wait for the worker to end (false: still busy)
// Tells the server this player left (POST /api/v1/friends/offline), then stops the worker; waits at most wait_ms for
// the answer (0: not at all). Nothing when no account is signed in.
void social_sign_off(int wait_ms);
// Deletes the online account (DELETE /api/v1/auth/account) on the running worker's backend, then stops the worker.
// Starts it on its own thread and returns at once (the launcher keeps drawing); social_delete_account_result gives
// the outcome once: nullopt while the call runs, else the HTTP status, 0 when nothing could be sent or the server
// didn't answer, or kSocialDeleteUnconfirmed when the call was still running after limit_ms - the server may
// still delete the account, so the caller can't say either way.
constexpr int kSocialDeleteUnconfirmed = -1;
void social_delete_account_start(int limit_ms);
std::optional<int> social_delete_account_result();
SocialView social_view();
// Queues an action (arg: see call_for); the worker runs it and polls again.
void social_do(SocialAction action, const std::string& arg);
// A Join / invite Accept that has to start a game here (see join_from_answer); taken once.
std::optional<SocialJoin> social_take_join();
