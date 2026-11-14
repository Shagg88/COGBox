// CogBox - test of social_net's pure parts (social step 3): the profile's identity.json, the server address, the
// claim answer. Prints FAIL lines; exit code = failures.
//   clang++ -std=c++17 -Isrc -Ithird_party test/social_net_test.cpp src/social_net.cpp src/social_model.cpp src/util.cpp
//           -lwinhttp -lole32 -lshell32 -luuid -o social_net_test.exe
#include "../src/social_net.h"
#include "../src/util.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "cogbox_social_net_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string d = narrow(dir.wstring());

    // no file, then a file without a token, then a full one
    CHECK(!read_account(d).has_value());
    {
        std::ofstream(dir / "identity.json") << R"({"uuid":"u","display_name":"Flapper","discriminator":"4293"})";
        CHECK(!read_account(d).has_value());
        std::ofstream(dir / "identity.json") << R"({"uuid":"u","token":"t","display_name":"Flapper","discriminator":"4293","xuid":"5"})";
        auto a = read_account(d);
        CHECK(a.has_value());
        if (a) {
            CHECK(a->tag() == "Flapper#4293");
            CHECK(a->uuid == "u");
            CHECK(a->token == "t");
        }
        std::ofstream(dir / "identity.json") << "garbage";
        CHECK(!read_account(d).has_value());
    }

    // the server address
    {
        auto u = parse_server_url("http://203.0.113.7:18443");
        CHECK(u.has_value());
        if (u) {
            CHECK(u->host == L"203.0.113.7");
            CHECK(u->port == 18443);
            CHECK(!u->secure);
        }
        auto s = parse_server_url("https://cogbox.example");
        CHECK(s.has_value() && s->port == 443 && s->secure && s->host == L"cogbox.example");
        auto bare = parse_server_url("127.0.0.1:1234");
        CHECK(bare.has_value() && bare->port == 1234 && !bare->secure);
        CHECK(!parse_server_url("").has_value());
        CHECK(!parse_server_url("http://").has_value());
        CHECK(!parse_server_url("http://h:notaport").has_value());
        CHECK(stun_address("http://203.0.113.7:18443") == "203.0.113.7:3478");
        CHECK(stun_address("") == "");
    }

    // the claim answer and the identity file the games read
    {
        auto c = parse_claim(R"({"uuid":"u","display_name":"Guest","discriminator":"0001","tag":"Guest#0001","xuid":"77","token":"tok"})");
        CHECK(c.has_value());
        if (c) {
            CHECK(c->tag() == "Guest#0001");
            CHECK(c->token == "tok");
            CHECK(c->xuid == "77");
        }
        CHECK(!parse_claim(R"({"error":"x"})").has_value());
        CHECK(write_account_exclusive(d + "\\new", *c));
        auto back = read_account(d + "\\new");
        CHECK(back.has_value() && back->tag() == "Guest#0001" && back->xuid == "77");
        CHECK(!write_account_exclusive(d + "\\new", *c));   // a game made it first: never overwritten
        CHECK(update_account_name(d + "\\new", "Guest Two", "0002"));
        auto renamed = read_account(d + "\\new");
        CHECK(renamed.has_value() && renamed->tag() == "Guest Two#0002" && renamed->token == "tok" && renamed->xuid == "77");
        CHECK(!update_account_name(d + "\\missing", "X", "0001"));
    }

    // 0.6.2 GitHub #302: a token-less file (a game's claim never completed) is completed, or set aside, never ignored
    {
        const std::string p = d + "\\partial";
        fs::create_directories(dir / "partial");
        CHECK(!read_account_partial(p).has_value());
        std::ofstream(dir / "partial" / "identity.json") << R"({"uuid":"u2","display_name":"Tavo","discriminator":"","token":""})";
        auto part = read_account_partial(p);
        CHECK(part.has_value() && part->uuid == "u2" && part->token.empty());
        CHECK(!read_account(p).has_value());
        SocialAccount c;
        c.uuid = "u2"; c.token = "t2"; c.name = "Tavo"; c.discriminator = "1234"; c.xuid = "9";
        CHECK(write_account_replace(p, c));
        auto full = read_account(p);
        CHECK(full.has_value() && full->tag() == "Tavo#1234" && full->token == "t2" && full->uuid == "u2");
        CHECK(account_dir_writable(p));
        CHECK(!fs::exists(dir / "partial" / "identity.write-test.tmp"));
        CHECK(account_file_age_seconds(p) >= 0 && account_file_age_seconds(p) < 30);
        CHECK(set_account_aside(p, "no-token"));
        CHECK(!fs::exists(dir / "partial" / "identity.json"));
        int asides = 0;
        for (const auto& e : fs::directory_iterator(dir / "partial")) asides += e.path().filename().wstring().rfind(L"identity.json.no-token-", 0) == 0;
        CHECK(asides == 1);
        CHECK(!read_account_partial(p).has_value());
        CHECK(account_file_age_seconds(p) == -1);
    }
    fs::remove_all(dir);
    if (g_failures == 0) std::printf("social_net_test: all passed\n");
    return g_failures;
}
