// CogBox - test of the social worker with a fake backend (social step 5: the games reuse the worker through
// SocialBackend). Prints FAIL lines; exit code = failures.
//   clang++ -std=c++17 -Isrc test/social_client_test.cpp src/social_client.cpp
//           src/social_model.cpp -o social_client_test.exe
#include "../src/social_client.h"


#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

static int g_failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

template <class F>
static bool wait_for(F ok, int ms = 2000) {
    for (int i = 0; i < ms / 10; ++i) {
        if (ok()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ok();
}

int main() {
    using namespace std::chrono;
    const SocialAccount acc{ "u", "t", "A", "0001", "5" };
    // a server that answers: one friend
    {
        SocialBackend b;
        b.call = [](const SocialCall& c) -> HttpResult {
            if (c.path == "/api/v1/friends")
                return { 200, R"({"friends":[{"uuid":"f1","tag":"Dom#0002","xuid":"7","presence":{"state":"online"}}],"incoming":[],"outgoing":[]})" };
            if (c.path == "/api/v1/invites") return { 200, R"({"invites":[]})" };
            return { 404, "" };
        };
        b.account = [acc] { return std::optional<SocialAccount>(acc); };
        social_start_backend("fake-ok", b);
        CHECK(wait_for([] { return social_view().lists.friends.size() == 1; }));
        const SocialView v = social_view();
        CHECK(v.have_account);
        CHECK(v.reachable);
        CHECK(v.my_tag == "A#0001");
        social_stop();
    }
    // a server that never answers: not reachable, and stopping never waits for the worker
    {
        SocialBackend b;
        b.call = [](const SocialCall&) -> HttpResult {
            std::this_thread::sleep_for(milliseconds(300));
            return { 0, "" };
        };
        b.account = [acc] { return std::optional<SocialAccount>(acc); };
        social_start_backend("fake-down", b);
        std::this_thread::sleep_for(milliseconds(50));   // the worker is inside a call now
        const auto t0 = steady_clock::now();
        social_stop();
        CHECK(duration_cast<milliseconds>(steady_clock::now() - t0).count() < 100);
        CHECK(!social_view().reachable);
    }
    // going offline: one request to the server, and the worker stops
    {
        static std::atomic<int> offline{0};
        SocialBackend b;
        b.call = [](const SocialCall& c) -> HttpResult {
            if (c.method == "POST" && c.path == "/api/v1/friends/offline") ++offline;
            return { 200, c.path == "/api/v1/friends" ? R"({"friends":[],"incoming":[],"outgoing":[]})" : "{}" };
        };
        b.account = [acc] { return std::optional<SocialAccount>(acc); };
        social_start_backend("fake-off", b);
        CHECK(wait_for([] { return social_view().have_account; }));
        social_sign_off(1000);
        CHECK(offline.load() == 1);
        CHECK(!social_view().configured);
        social_sign_off(100);          // nothing running: nothing sent
        CHECK(offline.load() == 1);
    }
    if (g_failures == 0) std::printf("social_client_test: all passed\n");
    return g_failures;
}
