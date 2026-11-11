// CogBox - test of net_scrub: problem reports carry no other player's address and no online token. Prints FAIL lines;
// exit code = failures.
//   clang++ -std=c++17 -Isrc test/net_scrub_test.cpp src/net_scrub.cpp -o net_scrub_test.exe
#include "../src/net_scrub.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

int main() {
    // public addresses lose their middle; private, loopback and the virtual 10.254 ones stay
    CHECK(scrub_network("connected through 203.0.113.7:3074") == "connected through 203.x.x.7:3074");
    CHECK(scrub_network("10.254.1.2 and 192.168.1.5 and 127.0.0.1") == "10.254.1.2 and 192.168.1.5 and 127.0.0.1");
    CHECK(scrub_network("a 172.32.0.9 b") == "a 172.x.x.9 b");
    CHECK(scrub_network("172.16.4.1") == "172.16.4.1");
    // not addresses: version numbers, longer dotted runs, numbers out of range
    CHECK(scrub_network("v0.10.0.124-dev") == "v0.10.0.124-dev");
    CHECK(scrub_network("1.2.3.4.5") == "1.2.3.4.5");
    CHECK(scrub_network("300.1.1.1") == "300.1.1.1");
    // a 64-hex token (the online account's) and an Authorization header
    const std::string tok(64, 'a');
    CHECK(scrub_network("token=" + tok + " end") == "token=<token> end");
    CHECK(scrub_network("Authorization: COGBox 2f1c0e9a-1111-2222-3333-444455556666:" + tok) ==
          "Authorization: COGBox 2f1c0e9a-1111-2222-3333-444455556666:<token>");
    CHECK(scrub_network(std::string(63, 'b')) == std::string(63, 'b'));
    if (!g_failures) std::printf("net_scrub_test: all passed\n");
    return g_failures;
}
