// COGBox - test of modcore's config-fragment merge as Gears 2 will use it (ue3/mod_extras.cpp): a synthetic
// Coalesced_*.bin (built with Gears 2's config_bundle), fragments applied through modcore::MergeConfig, the result
// parsed back and checked. Portable code only, so it builds and runs on Linux with plain g++ (C++20: config_bundle
// uses std::format) from Gears2-Native/cogbox:
//   g++ -std=c++20 -Wall -Wextra -I../src/layer2_ue3/include -I../../Gears3-Native/src/layer2_ue3 -o mod_fragments_test test/mod_fragments_test.cpp ../src/layer2_ue3/config_bundle.cpp ../../Gears3-Native/src/layer2_ue3/modcore/modcore.cpp
#include "../../src/layer2_ue3/include/ue3/config_bundle.h"
#include "../../../Gears3-Native/src/layer2_ue3/modcore/modcore.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int fails = 0;

void check(bool ok, const char* what) {
    if (!ok) { std::cout << "FAIL " << what << "\n"; ++fails; }
}

// a packed bundle with one file, [SystemSettings] MotionBlur=True, Suppress=Init, Suppress=Dev,
// Entries=(Identifier=X,Name=Y) - UTF-16 FStrings, as the games' Coalesced_*.bin are
std::vector<uint8_t> make_bundle() {
    auto s = [](const std::u16string& t) { ue3::ConfigString x; x.text = t; x.wide = true; x.present = true; return x; };
    ue3::ConfigBundle b;
    ue3::ConfigFile f;
    f.name = s(u"..\\GearGame\\CookedXenon\\Xe-GearEngine.ini");
    ue3::ConfigSection sys;
    sys.name = s(u"SystemSettings");
    sys.keys.push_back({ s(u"MotionBlur"), s(u"True") });
    sys.keys.push_back({ s(u"Suppress"), s(u"Init") });
    sys.keys.push_back({ s(u"Suppress"), s(u"Dev") });
    sys.keys.push_back({ s(u"Entries"), s(u"(Identifier=X,Name=Y)") });
    f.sections.push_back(std::move(sys));
    b.files.push_back(std::move(f));
    return ue3::WriteConfigBundle(b);
}

void write_text(const std::filesystem::path& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << text;
}

// the first value of key in file ending with `file`, "" when not there
std::u16string value(const ue3::ConfigBundle& b, const std::u16string& file, const std::u16string& section,
                     const std::u16string& key) {
    for (const ue3::ConfigFile& f : b.files) {
        ue3::ConfigString low = f.name;   // the game's names are "..\GearGame\CookedXenon\Xe-...ini"
        (void)low;
        if (f.name.text.size() < file.size() ||
            f.name.text.compare(f.name.text.size() - file.size(), file.size(), file) != 0)
            continue;
        for (const ue3::ConfigSection& x : f.sections) {
            if (x.name.text != section) continue;
            for (const ue3::ConfigKey& k : x.keys)
                if (k.key.text == key) return k.value.text;
        }
    }
    return u"<missing>";
}

int count_keys(const ue3::ConfigBundle& b, const std::u16string& section, const std::u16string& key) {
    int n = 0;
    for (const ue3::ConfigFile& f : b.files)
        for (const ue3::ConfigSection& x : f.sections) {
            if (x.name.text != section) continue;
            for (const ue3::ConfigKey& k : x.keys) if (k.key.text == key) ++n;
        }
    return n;
}
}  // namespace

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cogbox-fragments-test";
    std::filesystem::create_directories(dir);

    // one fragment: replace, add, remove, first-field replace
    write_text(dir / "a.ini",
               "##### Xe-GearEngine.ini\n"
               "[SystemSettings]\n"
               "MotionBlur=False\n"
               "+Suppress=New\n"
               "-Suppress=Init\n"
               "*Entries=(Identifier=X,Name=Z)\n");
    // a second mod's fragment, applied after: its add must not duplicate, its replace wins
    write_text(dir / "b.ini",
               "##### Xe-GearEngine.ini\n"
               "[SystemSettings]\n"
               "+Suppress=New\n"
               "MotionBlur=False\n");

    std::vector<uint8_t> bytes = make_bundle();
    std::vector<uint8_t> merged = modcore::MergeConfig(bytes, {{"Mod A", dir / "a.ini"}, {"Mod B", dir / "b.ini"}},
                                                       [](const std::string& line) { std::cout << "log: " << line << "\n"; });
    check(!merged.empty(), "MergeConfig returned a bundle");

    ue3::ConfigBundle out;
    std::string err;
    check(ue3::ParseConfigBundle(merged, out, &err), "the merged bundle parses");
    if (fails) return 1;
    const std::u16string ini = u"Xe-GearEngine.ini", sys = u"SystemSettings";
    check(value(out, ini, sys, u"MotionBlur") == u"False", "replace: MotionBlur=False");
    check(count_keys(out, sys, u"MotionBlur") == 1, "replace removed the old MotionBlur");
    check(value(out, ini, sys, u"Entries") == u"(Identifier=X,Name=Z)", "first-field replace: Entries");
    check(count_keys(out, sys, u"Suppress") == 2, "remove + add: two Suppress left");
    bool has_dev = false, has_new = false, has_init = false;
    for (const ue3::ConfigFile& f : out.files)
        for (const ue3::ConfigSection& x : f.sections) {
            if (x.name.text != sys) continue;
            for (const ue3::ConfigKey& k : x.keys) {
                if (k.key.text != u"Suppress") continue;
                has_dev = has_dev || k.value.text == u"Dev";
                has_new = has_new || k.value.text == u"New";
                has_init = has_init || k.value.text == u"Init";
            }
        }
    check(has_dev && has_new && !has_init, "Suppress: Init removed, Dev kept, New added once");

    // a new file and section are created
    write_text(dir / "c.ini",
               "##### Xe-GearEngine.ini\n"
               "[GearCamera]\n"
               "DefaultFOV=80\n"
               "##### Xe-Content.ini\n"
               "[NewSection]\n"
               "+List=One\n");
    merged = modcore::MergeConfig(merged, {{"Mod C", dir / "c.ini"}}, {});
    check(!merged.empty(), "the second merge parses");
    check(ue3::ParseConfigBundle(merged, out, &err) && !err.empty() == false, "the twice-merged bundle parses");
    check(value(out, ini, u"GearCamera", u"DefaultFOV") == u"80", "a section the game's file did not have is created");
    check(value(out, u"Xe-Content.ini", u"NewSection", u"List") == u"One", "a packed file the game did not have is created");

    // bare Xbox 360 files go to Gears 2's cooked folder
    check(modcore::GamePath("MyPackage.xxx", "cookedxenon") == "geargame\\cookedxenon\\mypackage.xxx",
          "GamePath: a bare package goes to CookedXenon");
    check(modcore::GamePath("Intro.bik", "cookedxenon") == "geargame\\movies\\intro.bik", "GamePath: a bare movie");
    check(modcore::GamePath("mod.ini", "cookedxenon") == "", "GamePath: mod.ini is the mod's own");
    check(modcore::GamePath("config\\x.ini", "cookedxenon") == "", "GamePath: fragments are the mod's own");

    // the game a mod is for (mod_extras.cpp ActiveModFolders reads --ue3_mods with FromFolders(dirs, "gears2")):
    // a folder-only mod and a mod.ini without a game key are for the game reading them; a game key still wins
    {
        const std::filesystem::path m = dir / "mods";
        std::filesystem::create_directories(m / "FolderOnly");
        std::filesystem::create_directories(m / "NoGameKey");
        std::filesystem::create_directories(m / "ForGears3");
        std::filesystem::create_directories(m / "ForGears2");
        write_text(m / "NoGameKey" / "mod.ini", "[Mod]\nName = No game key\n");
        write_text(m / "ForGears3" / "mod.ini", "[Mod]\nName = G3 only\nGame = gears3\n");
        write_text(m / "ForGears2" / "mod.ini", "[Mod]\nName = G2 only\nGame = gears2\n");
        const auto g2 = modcore::FromFolders({m / "FolderOnly", m / "NoGameKey", m / "ForGears3", m / "ForGears2"}, "gears2");
        check(g2.size() == 4, "FromFolders: four mods");
        if (g2.size() == 4) {
            check(g2[0].Active() && g2[0].game == "gears2", "Gears 2: a folder-only mod is active");
            check(g2[1].Active() && g2[1].game == "gears2", "Gears 2: a mod.ini without Game is active");
            check(!g2[2].Active() && g2[2].problem.find("Gears of War 3") != std::string::npos, "Gears 2: a Gears 3 mod is left out");
            check(g2[3].Active(), "Gears 2: a Gears 2 mod is active");
        }
        const auto g3 = modcore::FromFolders({m / "FolderOnly", m / "NoGameKey", m / "ForGears2"}, "gears3");
        check(g3.size() == 3 && g3[0].Active() && g3[1].Active() && !g3[2].Active(), "Gears 3: unchanged (default gears3, Gears 2 mod out)");
        check(modcore::Read(m / "FolderOnly").game == "gears3", "Read without a game: gears3 as before");
    }

    std::cout << (fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
