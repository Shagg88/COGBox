// CogBox - the social worker (see social_client.h), used by the launcher's SOCIAL tab and the games' Social page.
//
// Each start makes a new worker generation. Stopping never waits for a worker (a request can take up to 5 s per
// call while the server is down): it bumps the generation and lets the old thread go. An old worker sees the new
// generation the next time it takes the lock and ends without touching the view, the queue or the join.
// The shared state lives on the heap and is never freed, so a detached worker still finishing at exit is safe.
#include "social_client.h"

#include "../third_party/nlohmann/json.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {
struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    uint64_t gen = 0;   // the current worker's generation; 0 = none running
    uint64_t next_gen = 0;
    std::string key;    // what the running worker serves (server + profile)
    SocialBackend backend;
    SocialView view;
    std::deque<std::pair<SocialAction, std::string>> actions;
    std::optional<SocialJoin> join;
};
Shared& sh() {
    static Shared* s = new Shared;   // never freed (see the top)
    return *s;
}

// One poll: the friends list and the invites. Returns false when the server did not answer.
bool poll(const SocialBackend& b, SocialLists* lists, std::vector<SocialInvite>* invites) {
    const HttpResult f = b.call({ "GET", "/api/v1/friends", "" });
    if (f.status == 0) return false;
    if (auto l = parse_friends(f.body)) *lists = *l;
    const HttpResult i = b.call({ "GET", "/api/v1/invites", "" });
    if (i.status == 200) *invites = parse_invites(i.body);
    return true;
}

void run(SocialBackend b, uint64_t gen) {
    Shared& s = sh();
    for (int tick = 0;; ++tick) {
        std::optional<std::pair<SocialAction, std::string>> action;
        {
            std::unique_lock<std::mutex> lk(s.mu);
            if (s.gen != gen) return;
            if (!s.actions.empty()) {
                action = s.actions.front();
                s.actions.pop_front();
            }
        }
        std::string error;
        const std::optional<SocialAccount> acc = b.account();
        const std::string acc_error = acc || !b.account_error ? std::string() : b.account_error();
        SocialLists lists;
        std::vector<SocialInvite> invites;
        bool reachable = false;
        std::optional<SocialJoin> join;
        std::map<std::string, int> regions;
        std::string tag = acc ? acc->tag() : std::string();
        if (acc) {
            if (action) {
                const HttpResult r = b.call(call_for(action->first, action->second));
                if (action->first == SocialAction::Join || action->first == SocialAction::InviteAccept) join = join_from_answer(r.status, r.body);
                if (r.status == 0) error = "Can't reach the online server.";
                else if (r.status >= 400 && !join) error = server_error(r.body);
                if (action->first == SocialAction::Rename && r.status == 200) {
                    // the rename answer has the new name and number: identity.json takes them (the games read it)
                    const auto j = nlohmann::json::parse(r.body, nullptr, false);
                    if (j.is_object() && j["display_name"].is_string() && j["discriminator"].is_string()) {
                        const std::string n = j["display_name"].get<std::string>(), d = j["discriminator"].get<std::string>();
                        if (b.renamed) b.renamed(n, d);
                        tag = n + "#" + d;
                    }
                }
            }
            reachable = poll(b, &lists, &invites);
            if (reachable && tick % 6 == 0) {   // the Region dropdown's numbers: every 30 s is plenty
                const HttpResult p = b.call({ "GET", "/api/v1/population/regions", "" });
                if (p.status == 200) regions = parse_region_counts(p.body);
            }
        }
        {
            std::unique_lock<std::mutex> lk(s.mu);
            if (s.gen != gen) return;
            SocialView& v = s.view;
            v.configured = true;
            v.busy = false;
            v.have_account = acc.has_value();
            v.reachable = acc ? reachable : false;
            if (acc) v.my_tag = tag;
            if (!acc_error.empty()) v.error = acc_error;
            else if (action) v.error = error;
            else if (reachable && v.error == "Can't reach the online server.") v.error.clear();
            if (reachable) {
                v.lists = lists;
                v.invites = invites;
            }
            if (!regions.empty()) v.region_counts = regions;
            if (join) s.join = join;
            // wait 5 s, or less when an action is queued
            s.cv.wait_for(lk, std::chrono::seconds(5), [&] { return s.gen != gen || !s.actions.empty(); });
        }
    }
}

}  // namespace

void social_stop() {
    Shared& s = sh();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.gen = 0;   // the running worker (if any) ends on its own; nobody waits for it
        s.key.clear();
        s.actions.clear();
        s.join.reset();
        s.view = SocialView{};
    }
    s.cv.notify_all();
}

void social_sign_off(int wait_ms) {
    Shared& s = sh();
    SocialBackend b;
    bool signed_in = false;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        signed_in = s.gen != 0 && s.view.have_account;
        b = s.backend;
    }
    social_stop();
    if (!signed_in || !b.call) return;
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    std::thread([b, done] {
        b.call({ "POST", "/api/v1/friends/offline", "" });
        done->set_value();
    }).detach();
    if (wait_ms > 0) finished.wait_for(std::chrono::milliseconds(wait_ms));
}

void social_start_backend(const std::string& key, SocialBackend backend) {
    Shared& s = sh();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.gen != 0 && s.key == key) return;
    }
    social_sign_off(0);   // the previous profile / server (if any) goes offline
    std::lock_guard<std::mutex> lk(s.mu);
    s.gen = ++s.next_gen;
    s.key = key;
    s.backend = backend;
    s.view = SocialView{};
    s.view.configured = true;
    s.view.busy = true;
    std::thread(run, std::move(backend), s.gen).detach();
}

SocialView social_view() {
    std::lock_guard<std::mutex> lk(sh().mu);
    return sh().view;
}

void social_do(SocialAction action, const std::string& arg) {
    Shared& s = sh();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.gen == 0) return;
        s.actions.emplace_back(action, arg);
        s.view.error.clear();
    }
    s.cv.notify_all();
}

std::optional<SocialJoin> social_take_join() {
    std::lock_guard<std::mutex> lk(sh().mu);
    auto j = sh().join;
    sh().join.reset();
    return j;
}
