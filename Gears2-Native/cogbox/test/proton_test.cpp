// CogBox - test of proton.cpp (Linux / Steam Deck detection, Linux paths, where to look) and the Proton parts of
// gears_settings (proton049). Runs on Linux:
//   g++ -std=c++17 -O2 -Wall -Wextra -I../src -I../../src/layer2_ue3/include -I../../src/layer0_xenos/include
//       proton_test.cpp ../src/proton.cpp ../src/gears_settings.cpp -o proton_test && ./proton_test
#include "../src/gears_settings.h"
#include "../src/proton.h"

#include <cstdio>
#include <fstream>
#include <map>

namespace gs = cogbox::gears;
namespace fs = std::filesystem;

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQ(a, b) do { g_n++; const std::string x_ = (a), y_ = (b); if (x_ != y_) { g_fail++; std::printf("FAIL %s:%d  %s\n   got  [%s]\n   want [%s]\n", __FILE__, __LINE__, #a, x_.c_str(), y_.c_str()); } } while (0)

static proton::Inputs inputs(std::map<std::string, std::string> env, std::map<std::string, std::string> files, std::string wine = "9.0")
{
    proton::Inputs in;
    in.wineVersion = wine;
    in.env = [env](const char *k) { auto it = env.find(k); return it == env.end() ? std::string() : it->second; };
    in.readUnix = [files](const std::string &p, std::string &o) { auto it = files.find(p); if (it == files.end()) return false; o = it->second; return true; };
    in.zDrive = true;
    return in;
}

int main()
{
    // ---- paths
    EQ(proton::to_wine("/home/deck/Games", ""), "Z:\\home\\deck\\Games");
    EQ(proton::to_wine("/home/deck/Games/", ""), "Z:\\home\\deck\\Games");
    EQ(proton::to_wine("//home//deck///x", ""), "Z:\\home\\deck\\x");
    EQ(proton::to_wine("/", ""), "Z:\\");
    EQ(proton::to_wine("~/Games", "/home/deck"), "Z:\\home\\deck\\Games");
    EQ(proton::to_wine("~", "/home/deck"), "Z:\\home\\deck");
    EQ(proton::to_wine("~/Games", ""), "");
    EQ(proton::to_wine("  \"/run/media/deck/SD card/Gears 2\"\n", ""), "Z:\\run\\media\\deck\\SD card\\Gears 2");
    EQ(proton::to_wine("'/home/deck/My Games'", ""), "Z:\\home\\deck\\My Games");
    EQ(proton::to_wine("file:///home/deck/My%20Games/Gears%202", ""), "Z:\\home\\deck\\My Games\\Gears 2");
    EQ(proton::to_wine("file://localhost/home/deck/x", ""), "Z:\\home\\deck\\x");
    EQ(proton::to_wine("C:\\Games\\Gears", ""), "C:\\Games\\Gears");
    EQ(proton::to_wine("Z:/home/deck", ""), "Z:\\home\\deck");
    EQ(proton::to_wine("Games\\Gears", ""), "");
    EQ(proton::to_wine("", ""), "");
    EQ(proton::to_unix("Z:\\home\\deck\\Games"), "/home/deck/Games");
    EQ(proton::to_unix("z:\\home\\deck\\Games\\"), "/home/deck/Games");
    EQ(proton::to_unix("Z:\\"), "/");
    EQ(proton::to_unix("Z:"), "/");
    EQ(proton::to_unix("\\\\?\\unix\\home\\deck"), "/home/deck");
    EQ(proton::to_unix("C:\\COGBox"), "");
    EQ(proton::display_path("Z:\\home\\deck\\COGBox\\gears2", true), "/home/deck/COGBox/gears2");
    EQ(proton::display_path("C:\\COGBox", true), "C:\\COGBox");
    EQ(proton::display_path("Z:\\home\\deck", false), "Z:\\home\\deck");   // Windows: never changed
    for (const char *p : { "/home/deck/Games/Gears of War 2", "/run/media/mmcblk0p1/x", "/" })   // round trip
        EQ(proton::to_unix(proton::to_wine(p, "")), p);

    // ---- detection
    {   // Windows proper: nothing, even with Steam variables around
        proton::Inputs in = inputs({ { "SteamDeck", "1" }, { "STEAM_COMPAT_DATA_PATH", "/x" } }, {}, "");
        proton::Info i = proton::detect(in);
        CHECK(!i.wine && !i.proton && !i.steamDeck);
        EQ(proton::summary(i), "");
        CHECK(proton::report_lines(i).empty());
    }
    {   // Steam Deck OLED in Game Mode, Proton 11 through STEAM_COMPAT_TOOL_PATHS + its version file
        proton::Inputs in = inputs({ { "HOME", "/home/deck" }, { "USER", "deck" }, { "SteamDeck", "1" }, { "SteamOS", "1" },
                                     { "STEAM_COMPAT_DATA_PATH", "/home/deck/.local/share/Steam/steamapps/compatdata/3141592653" },
                                     { "STEAM_COMPAT_TOOL_PATHS", "/home/deck/.local/share/Steam/steamapps/common/Proton 11.0:/home/deck/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper" },
                                     { "GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0" } },
                                   { { "/home/deck/.local/share/Steam/steamapps/common/Proton 11.0/version", "1752000000 proton-11.0-2\n" },
                                     { "/etc/os-release", "NAME=\"SteamOS\"\nPRETTY_NAME=\"SteamOS\"\nID=steamos\n" },
                                     { "/sys/devices/virtual/dmi/id/sys_vendor", "Valve\n" },
                                     { "/sys/devices/virtual/dmi/id/product_name", "Galileo\n" } }, "11.0");
        in.gpus = { "AMD Custom GPU 0932 (RADV SEPHIROTH)" };
        proton::Info i = proton::detect(in);
        CHECK(i.wine && i.proton && i.steamDeck && i.deckOled && !i.deckLcd && i.steamOS && i.gamescope);
        EQ(i.protonVersion, "proton-11.0-2");
        EQ(i.deckModel, "Steam Deck OLED");
        EQ(i.home, "/home/deck");
        EQ(proton::summary(i), "Proton 11.0-2 (Wine 11.0), Steam Deck OLED, SteamOS, Game Mode (gamescope)");
        CHECK(proton::report_lines(i).size() == 4);
    }
    {   // Steam Deck LCD known only by its DMI (desktop mode, no SteamDeck variable); GE-Proton without a version file
        proton::Inputs in = inputs({ { "HOME", "/home/deck" }, { "STEAM_COMPAT_TOOL_PATHS", "/home/deck/.steam/root/compatibilitytools.d/GE-Proton11-12" },
                                     { "STEAM_COMPAT_CLIENT_INSTALL_PATH", "/home/deck/.local/share/Steam" } },
                                   { { "/sys/devices/virtual/dmi/id/sys_vendor", "Valve" }, { "/sys/devices/virtual/dmi/id/board_name", "Jupiter" } });
        proton::Info i = proton::detect(in);
        CHECK(i.steamDeck && i.deckLcd && !i.deckOled && !i.gamescope && i.proton);
        EQ(i.protonVersion, "GE-Proton11-12");
        EQ(i.deckModel, "Steam Deck LCD");
        EQ(i.user, "deck");   // from HOME
        EQ(proton::summary(i), "GE-Proton11-12 (Wine 9.0), Steam Deck LCD");
    }
    {   // the Deck's GPU alone (LCD: 0405)
        proton::Inputs in = inputs({}, {});
        in.gpus = { "AMD Custom GPU 0405" };
        proton::Info i = proton::detect(in);
        CHECK(i.steamDeck && i.deckLcd && !i.proton);
    }
    {   // plain Wine on a desktop PC: no Proton, no Deck; HOME from WINEHOMEDIR
        proton::Inputs in = inputs({ { "WINEHOMEDIR", "\\??\\Z:\\home\\sam" } }, { { "/etc/os-release", "PRETTY_NAME=\"Ubuntu 24.04 LTS\"\nID=ubuntu\n" } });
        in.hostSystem = "Linux";
        proton::Info i = proton::detect(in);
        CHECK(i.wine && !i.proton && !i.steamDeck && !i.steamOS);
        EQ(i.home, "/home/sam");
        EQ(i.user, "sam");
        EQ(proton::summary(i), "Wine 9.0, Ubuntu 24.04 LTS");
    }
    {   // a Proton Wine build seen only in its build id
        proton::Inputs in = inputs({}, {});
        in.wineBuild = "wine-9.0 (Proton-9.0-4)";
        CHECK(proton::detect(in).proton);
    }
    EQ(proton::proton_dir("/a/SteamLinuxRuntime_sniper:/b/Proton - Experimental"), "/b/Proton - Experimental");
    EQ(proton::proton_dir(""), "");
    EQ(proton::version_from_file("1700000000 proton-9.0-4\n"), "proton-9.0-4");

    // ---- where to look
    {
        const std::string vdf = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"/home/deck/.local/share/Steam\"\n\t\t\"label\"\t\t\"\"\n\t}\n"
                                "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"/run/media/deck/SD\\\\Card\"\n\t}\n\t\"2\"\n\t{\n\t\t\"path\"\t\t\"/home/deck/.local/share/Steam\"\n\t}\n}\n";
        const std::vector<std::string> libs = proton::parse_libraryfolders(vdf);
        CHECK(libs.size() == 2);
        if (libs.size() == 2) { EQ(libs[0], "/home/deck/.local/share/Steam"); EQ(libs[1], "/run/media/deck/SD\\Card"); }
        const std::vector<std::string> r = proton::scan_roots("/home/deck", "deck", { "/home/deck/.local/share/Steam" }, { "/run/media/deck/SD" });
        CHECK(!r.empty() && r[0] == "/home/deck/Games");
        int n = 0;
        for (const std::string &x : r) n += x == "/home/deck/.local/share/Steam/steamapps/common";
        CHECK(n == 1);   // no duplicates
        bool sd = false, share = false;
        for (const std::string &x : r) { sd |= x == "/run/media/deck/SD"; share |= x == "/home/deck/.local/share"; }
        CHECK(sd && share);
    }
    {   // the walk over a real folder tree
        const fs::path t = fs::temp_directory_path() / "cogbox-proton-test";
        std::error_code ec;
        fs::remove_all(t, ec);
        fs::create_directories(t / "Games" / "Gears of War 2");
        std::ofstream(t / "Games" / "Gears of War 2" / "default.xex") << "x";
        fs::create_directories(t / "Games" / "Gears of War 2" / "Content" / "deep");   // inside a copy: not entered
        fs::create_directories(t / "Games" / "a" / "b" / "c" / "d" / "Gears 3");      // too deep
        std::ofstream(t / "Games" / "a" / "b" / "c" / "d" / "Gears 3" / "default.xex") << "x";
        fs::create_directories(t / "Games" / ".hidden" / "Judgment");
        std::ofstream(t / "Games" / ".hidden" / "Judgment" / "default.xex") << "x";
        fs::create_directories(t / "Games" / "compatdata" / "Judgment");
        std::ofstream(t / "Games" / "compatdata" / "Judgment" / "default.xex") << "x";
        fs::create_directories(t / "Games" / "isos");
        std::ofstream(t / "Games" / "isos" / "Gears1.iso") << "x";
        std::ofstream(t / "Games" / "isos" / "notes.txt") << "x";
        int entered = 0;
        auto isGame = [&](const fs::path &p, bool dir) {
            if (dir) { if (p.filename() == "deep") entered++; return fs::exists(p / "default.xex"); }
            return p.extension() == ".iso";
        };
        std::vector<fs::path> f = proton::find_copies({ t / "Games", t / "missing" }, isGame, 3, 1000);
        CHECK(f.size() == 2);
        bool g2 = false, iso = false;
        for (const fs::path &p : f) { g2 |= p.filename() == "Gears of War 2"; iso |= p.filename() == "Gears1.iso"; }
        CHECK(g2 && iso);
        CHECK(entered == 0);
        CHECK(proton::find_copies({ t / "Games" }, isGame, 3, 1).size() <= 1);   // the budget stops it
        fs::remove_all(t, ec);
    }

    {   // the quick first look
        const fs::path t = fs::temp_directory_path() / "cogbox-proton-quick";
        std::error_code ec;
        fs::remove_all(t, ec);
        fs::create_directories(t / "god" / "4D5307E6" / "00007000");
        fs::create_directories(t / "tu" / "000B0000");
        fs::create_directories(t / "extracted");
        std::ofstream(t / "extracted" / "default.xex") << "x";
        fs::create_directories(t / "pkg");
        std::ofstream(t / "pkg" / "A1B2C3D4E5F60718293A4B5C6D7E8F9012345678AB") << "x";
        fs::create_directories(t / "plain" / "sub");
        std::ofstream(t / "plain" / "readme.txt") << "x";
        CHECK(proton::quick_candidate(t / "god", true));
        CHECK(proton::quick_candidate(t / "god" / "4D5307E6", true));
        CHECK(proton::quick_candidate(t / "tu", true));
        CHECK(proton::quick_candidate(t / "extracted", true));
        CHECK(proton::quick_candidate(t / "pkg", true));
        CHECK(!proton::quick_candidate(t / "plain", true));
        CHECK(proton::quick_candidate(t / "x" / "Gears.ISO", false));
        CHECK(!proton::quick_candidate(t / "plain" / "readme.txt", false));
        CHECK(proton::quick_candidate(t / "pkg" / "A1B2C3D4E5F60718293A4B5C6D7E8F9012345678AB", false));
        fs::remove_all(t, ec);
    }

    // ---- gears_settings under Proton (nothing changes on Windows)
    {
        gs::Environment win, deck, nv;
        deck.wine = true; deck.steam_deck = true; deck.deck_lcd = true; deck.gpus = { "AMD Custom GPU 0405" };
        nv.wine = true; nv.gpus = { "NVIDIA GeForce RTX 4070" };
        const gs::Option *fs_ = gs::find_option(gs::Game::Gears2, "Fullscreen");
        const gs::Option *aa = gs::find_option(gs::Game::Gears2, "AAMode");
        const gs::Option *fg = gs::find_option(gs::Game::Gears2, "FrameGen");
        CHECK(fs_ && aa && fg);
        if (fs_ && aa && fg) {
            for (int k = 0; k < 8; k++) {
                CHECK(gs::wine_choice_available(*fs_, k, win).ok);
                CHECK(gs::wine_choice_available(*aa, k, win).ok);
                CHECK(gs::wine_choice_available(*fg, k, win).ok);
            }
            CHECK(!gs::wine_choice_available(*fs_, 2, deck).ok && gs::wine_choice_available(*fs_, 1, deck).ok);
            CHECK(!gs::wine_choice_available(*aa, 3, deck).ok && gs::wine_choice_available(*aa, 3, nv).ok);
            CHECK(gs::wine_choice_available(*aa, 2, deck).ok);
            CHECK(!gs::wine_choice_available(*fg, 2, nv).ok && !gs::wine_choice_available(*fg, gs::k_fgXefg, nv).ok);
            CHECK(gs::wine_choice_available(*fg, 1, deck).ok && gs::wine_choice_available(*fg, 0, deck).ok);
            CHECK(!gs::wine_choice_available(*fs_, 2, deck).why.empty());
        }
        gs::Settings s;
        const gs::Option *hdr = gs::find_option(gs::Game::Gears2, "HDR");
        CHECK(hdr && gs::available(*hdr, gs::Game::Gears2, gs::Version::TitleUpdate, s, win).ok);
        CHECK(hdr && !gs::available(*hdr, gs::Game::Gears2, gs::Version::TitleUpdate, s, deck).ok);
        CHECK(hdr && gs::available(*hdr, gs::Game::Gears2, gs::Version::TitleUpdate, s, nv).ok);
        // the tooltips: unchanged on Windows
        const gs::Availability ok;
        gs::Environment nvWin = nv;
        nvWin.wine = false;
        CHECK(gs::tooltip_for(*aa, gs::Game::Gears2, ok, nvWin) == gs::tooltip_for(*aa, gs::Game::Gears2, ok, nv));
        for (const gs::Option &o : gs::options(gs::Game::Gears2)) CHECK(gs::wine_note(o, win).empty());
        CHECK(gs::wine_note(*aa, nv).find("PROTON_ENABLE_NVAPI=1 %command%") != std::string::npos);
        CHECK(!gs::wine_note(*gs::find_option(gs::Game::Gears2, "FrameCap"), deck).empty() && gs::wine_note(*gs::find_option(gs::Game::Gears2, "FrameCap"), nv).empty());

        CHECK(gs::proton_recommended(gs::Game::Gears2, win).empty());
        for (int g = 0; g < gs::kGameCount; g++) {
            const auto r = gs::proton_recommended((gs::Game)g, deck);
            std::map<std::string, double> m(r.begin(), r.end());
            CHECK(m["Api"] == 1 && m["Fullscreen"] == 1 && m["FrameCap"] == 3 && m["PromptStyle"] == 4 && m.count("Mnk") && m["Mnk"] == 0);
            CHECK(m["AAMode"] == 1);   // no FSR-on-Vulkan files in this environment -> SMAA
            for (const auto &kv : r) {
                const gs::Option *o = gs::find_option((gs::Game)g, kv.first);
                CHECK(o != nullptr);   // every id is a real option in every game
                if (o && o->kind == gs::Kind::Choice) CHECK(kv.second >= 0 && kv.second < (double)o->choices.size());
            }
            const auto rn = gs::proton_recommended((gs::Game)g, nv);   // a Linux PC: no Deck display / controls changes
            std::map<std::string, double> mn(rn.begin(), rn.end());
            CHECK(mn.count("Api") && !mn.count("Fullscreen") && !mn.count("PromptStyle") && !mn.count("FrameCap"));
        }
        // the choices' texts the code relies on
        const gs::Option *ps = gs::find_option(gs::Game::Gears2, "PromptStyle");
        const gs::Option *cap = gs::find_option(gs::Game::Gears2, "FrameCap");
        CHECK(ps && ps->choices.size() > 4 && ps->choices[4] == "Steam Deck");
        CHECK(cap && cap->choices[3] == "60" && cap->choices[1] == "40");
        CHECK(fs_ && fs_->choices[1] == "Borderless fullscreen" && fs_->choices[2] == "Exclusive fullscreen");
    }

    std::printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
