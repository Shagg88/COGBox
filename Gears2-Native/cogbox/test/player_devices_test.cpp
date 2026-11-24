// CogBox - test of the per-player input devices (--player_devices): the pure logic of input_devices.cpp and how
// gears_settings keeps the PlayerDevice1..4 settings and builds the argument. Prints FAIL lines; exit code = failures.
//   clang++ -std=c++17 -Isrc test/player_devices_test.cpp src/input_devices.cpp src/gears_settings.cpp -o player_devices_test.exe
#include "../src/gears_settings.h"
#include "../src/input_devices.h"

#include <algorithm>
#include <cstdio>

using namespace cogbox;
namespace gs = cogbox::gears;
typedef std::vector<std::string> V;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static std::string device_arg(const gs::Store& st) {
    gs::Environment e; e.update_ready = true;
    gs::Settings s = st.view(gs::Game::Gears3, e);
    std::vector<std::string> notes;
    for (const std::string& a : gs::build_args(gs::Game::Gears3, gs::effective_version(gs::Game::Gears3, s, e), s, e, &notes))
        if (a.compare(0, 17, "--player_devices=") == 0) return a;
    return std::string();
}

int main() {
    CHECK(ResolvePlayerDevices({ "x", "" }, 2) == (V{ "x", "" }));
    CHECK(ResolvePlayerDevices({ "x" }, 3) == (V{ "x", "", "" }));                           // padded with Auto
    CHECK(ResolvePlayerDevices({ "x", "a", "b" }, 2) == (V{ "x", "a" }));                    // cut to the players
    CHECK(ResolvePlayerDevices({ "x", "x" }, 2) == (V{ "", "x" }));                          // the later pick wins
    CHECK(ResolvePlayerDevices({ "kbm", "a" }, 2) == (V{ "kbm", "a" }));                     // keyboard and mouse is a choice (#420)
    CHECK(ResolvePlayerDevices({ "kbm", "kbm" }, 2) == (V{ "", "kbm" }));
    CHECK(ResolvePlayerDevices({ "p", "q", "p", "p" }, 4) == (V{ "", "q", "", "p" }));

    CHECK(PlayerDevicesArg({ "x" }) == "");                                                // 1 player: nothing
    CHECK(PlayerDevicesArg({}) == "");
    CHECK(PlayerDevicesArg({ "", "" }) == "--player_devices=auto;auto");                     // all Auto: sent (Auto's order)
    CHECK(PlayerDevicesArg({ "x", "" }) == "--player_devices=x;auto");
    CHECK(PlayerDevicesArg({ "sdl:g|p", "x", "" }) == "--player_devices=sdl:g|p;x;auto");

    // Auto's order (SDK patch 128): keyboard and mouse first, then the pads in order
    const std::vector<DetectedDevice> two = { { "sdl:a|1", "Pad A", "" }, { "sdl:b|2", "Pad B", "" } };
    CHECK(EffectiveDevices({ "", "" }, two, true) == (V{ "kbm", "sdl:a|1" }));              // all Auto: KBM player 1, pad player 2
    CHECK(EffectiveDevices({ "sdl:b|2", "" }, two, true) == (V{ "sdl:b|2", "kbm" }));       // P1 on a pad: P2 Auto gets KBM
    CHECK(EffectiveDevices({ "", "kbm" }, two, true) == (V{ "sdl:a|1", "kbm" }));           // KBM chosen for P2
    CHECK(EffectiveDevices({ "", "" }, two, false) == (V{ "sdl:a|1", "sdl:b|2" }));         // keyboard and mouse off: pads
    CHECK(EffectiveDevices({ "sdl:z|9", "" }, two, true) == (V{ "kbm", "sdl:a|1" }));       // missing pad: stand-ins in order
    CHECK(EffectiveDevices({ "", "" }, {}, true) == (V{ "kbm", "" }));                      // nothing left for P2

    std::string k, n;
    UnpackPlayerDevice(PackPlayerDevice("sdl:g|HID#x", "Xbox Controller"), k, n);
    CHECK(k == "sdl:g|HID#x" && n == "Xbox Controller");
    UnpackPlayerDevice("sdl:q|1", k, n);
    CHECK(k == "sdl:q|1" && n.empty());

    // the list: Auto, Keyboard & mouse, pads by label (a second of the same label gets " #2"), a missing saved pad last
    const std::vector<DetectedDevice> pads = { { "sdl:a|1", "Pad", "" }, { "sdl:a|2", "Pad", "" }, { "sdl:b|3", "Other", "Other - USB" } };
    auto list = PlayerDeviceChoices(pads, "", "");
    CHECK(list.size() == 5 && list[0].label == "Auto" && list[1].key == "kbm" && list[1].label == "Keyboard & mouse");
    CHECK(list[2].label == "Pad" && list[3].label == "Pad #2" && list[4].label == "Other - USB");
    CHECK(DeviceLabel(list, "sdl:b|3") == "Other - USB" && DeviceLabel(list, "nope").empty());
    list = PlayerDeviceChoices(pads, "sdl:a|2", "Pad");
    CHECK(list.size() == 5);                                                                  // detected: no extra row
    list = PlayerDeviceChoices(pads, "sdl:z|9", "Old Pad");
    CHECK(list.size() == 6 && list[5].key == "sdl:z|9" && list[5].label == "Old Pad (not connected)");
    list = PlayerDeviceChoices(pads, "kbm", "");                                              // keyboard and mouse is always listed
    CHECK(list.size() == 5);

    // settings: 1 player sends nothing; 2 players send the saved keys, a missing pad's too; 2 -> 1 -> 2 keeps them
    gs::Store st;
    st.texts["PlayerDevice1"] = PackPlayerDevice("sdl:y|1", "Pad One");
    st.texts["PlayerDevice2"] = PackPlayerDevice("sdl:z|9", "Old Pad");                       // not detected anywhere
    st.top["Players"] = 0;
    CHECK(device_arg(st).empty());
    st.top["Players"] = 1;
    CHECK(device_arg(st) == "--player_devices=\"sdl:y|1;sdl:z|9\"");
    st.top["Players"] = 0;
    CHECK(device_arg(st).empty());
    st.top["Players"] = 1;
    CHECK(device_arg(st) == "--player_devices=\"sdl:y|1;sdl:z|9\"");
    {   gs::Store au; au.top["Players"] = 1; CHECK(device_arg(au) == "--player_devices=\"auto;auto\""); }   // untouched: Auto's order
    st.texts["PlayerDevice2"] = PackPlayerDevice("sdl:y|1", "");                              // the same key twice: later wins
    CHECK(device_arg(st) == "--player_devices=\"auto;sdl:y|1\"");
    {   gs::Store kb; kb.top["Players"] = 1;                                                  // keyboard and mouse chosen for P2
        kb.texts["PlayerDevice2"] = PackPlayerDevice("kbm", "");
        CHECK(device_arg(kb) == "--player_devices=\"auto;kbm\""); }

    // the values survive the settings file
    gs::Store st2;
    st2.texts["PlayerDevice1"] = PackPlayerDevice("sdl:z|9", "Old Pad");
    gs::Store st3; st3.load_ini(st2.save_ini());
    CHECK(st3.texts["PlayerDevice1"] == PackPlayerDevice("sdl:z|9", "Old Pad"));

    // availability: dropdown k only with 2+ players and k <= players
    gs::Environment e; e.update_ready = true;
    for (int players = 1; players <= 4; ++players) {
        gs::Store s0; s0.top["Players"] = players - 1;
        const gs::Settings s = s0.view(gs::Game::Gears3, e);
        for (int d = 1; d <= 4; ++d) {
            const gs::Option* o = gs::find_option(gs::Game::Gears3, "PlayerDevice" + std::to_string(d));
            // 0.6.8 (#443): every game plays at most 2 local players, so 3 or 4 saved show 2 lists
            CHECK(o && gs::available(*o, gs::Game::Gears3, gs::effective_version(gs::Game::Gears3, s, e), s, e).ok == (players >= 2 && d <= std::min(players, 2)));
        }
    }

    // Task 9: side by side in all four games; the pause over the whole window for 2+ players
    {
        gs::Environment ec; ec.update_ready = true;
        ec.cap.exe_has = [](const std::string&) { return true; };                              // an exe with every cvar
        gs::Environment en; en.update_ready = true;
        en.cap.exe_has = [](const std::string&) { return false; };                             // an older exe
        struct G { gs::Game g; const char* split; const char* pause; } gl[] = {
            { gs::Game::Gears1, "--gow1_split_vertical=true", nullptr },
            { gs::Game::Gears2, "--gow2_split_vertical=true", "--gow2_pause_fullscreen=true" },
            { gs::Game::Gears3, "--gow3_split_vertical=true", "--gow3_pause_fullscreen=true" },
            { gs::Game::Judgment, "--gowj_split_vertical=true", "--gowj_pause_fullscreen=true" } };
        auto has = [](const V& a, const char* x) { for (const auto& y : a) if (y == x) return true; return false; };
        auto args = [](gs::Game g, const gs::Environment& e, int players, int layout, bool old = false) {
            gs::Store st; st.top["Players"] = players - 1; st.top["SplitLayout"] = layout;
            if (old) st.load_ini(st.save_ini());                                                // saved by an earlier build
            gs::Settings s = st.view(g, e); std::vector<std::string> n;
            return gs::build_args(g, gs::effective_version(g, s, e), s, e, &n);
        };
        for (const G& x : gl) {
            CHECK(has(args(x.g, ec, 2, 1), x.split));
            CHECK(has(args(x.g, ec, 2, 1, true), x.split));
            CHECK(!has(args(x.g, ec, 2, 0), x.split));
            CHECK(has(args(x.g, ec, 3, 1), x.split));                                             // 0.6.8 (#443): 3 saved = played as 2
            CHECK(!has(args(x.g, ec, 1, 1), x.split));
            CHECK(!has(args(x.g, en, 2, 1), x.split));                                            // old build: no cvar
            for (int pl = 1; pl <= 4; ++pl) {
                const bool want = x.pause && pl >= 2;
                CHECK(has(args(x.g, ec, pl, 0), x.pause ? x.pause : "") == want);
                CHECK(!x.pause || !has(args(x.g, en, pl, 0), x.pause));
            }
            for (const auto& a : args(x.g, ec, 2, 1)) CHECK(a.find("DesiredSplitscreenType") == std::string::npos);
        }
    }
    return fails;
}
