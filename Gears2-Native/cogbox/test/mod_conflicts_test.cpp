// CogBox - test of the mod conflicts (mod_conflicts.cpp): which enabled mods change the same game file or texture, and
// which one wins, by the game's rules. Prints FAIL lines; exit code = failures. MinGW, from cogbox\:
//   g++ -O2 -std=c++17 -Wall -Wextra -Isrc test/mod_conflicts_test.cpp src/mod_conflicts.cpp src/mod_file_rules.cpp
//       -static-libgcc -static-libstdc++ -o mod_conflicts_test.exe
#include "../src/mod_conflicts.h"

#include <chrono>
#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

static ModFileList mod(const char *name, std::vector<std::string> files, bool overlay = true)
{
    ModFileList m;
    m.name = name;
    m.files = std::move(files);
    m.overlay = overlay;
    return m;
}
static ModConflictRules gears2()
{
    ModConflictRules r;
    r.cooked = "cookedxenon";
    return r;
}

int main()
{
    // two mods with one package (the path's case differs): the later one wins
    {
        const auto c = mod_conflicts({ mod("Lancer Skin", { "GearGame\\CookedXenon\\Lancer.xxx", "GearGame\\CookedXenon\\Only.xxx" }),
                                       mod("Horde Bank", { "geargame/cookedxenon/LANCER.XXX" }) }, gears2());
        CHECK(c.size() == 2);
        CHECK(c[0].replaced_by.size() == 1 && c[0].replaces.empty());
        CHECK(c[0].replaced_by[0].other == 1 && c[0].replaced_by[0].other_name == "Horde Bank" && c[0].replaced_by[0].count == 1);
        CHECK(c[0].replaced_by[0].names.size() == 1 && c[0].replaced_by[0].names[0] == "GearGame\\CookedXenon\\Lancer.xxx");
        CHECK(c[1].replaces.size() == 1 && c[1].replaces[0].other == 0 && c[1].replaces[0].count == 1);
        CHECK(c[1].replaces[0].names[0] == "geargame/cookedxenon/LANCER.XXX");
        CHECK(c[1].replaced_by.empty() && c[0].tex_loses.empty());
    }
    // a bare package at the top is the cooked folder's file (Gears 2 / 3); Gears 1 has no bare files
    {
        const auto c = mod_conflicts({ mod("A", { "Lancer.xxx" }), mod("B", { "GearGame\\CookedXenon\\Lancer.xxx" }) }, gears2());
        CHECK(c[1].replaces.size() == 1 && c[1].replaces[0].count == 1);
        ModConflictRules g1;
        const auto d = mod_conflicts({ mod("A", { "Lancer.xxx" }), mod("B", { "Lancer.xxx" }) }, g1);
        CHECK(!d[0].any() && !d[1].any());
        ModConflictRules g3;
        g3.cooked = "cookedxbox360";
        CHECK(mod_conflict_game_path("Coalesced_INT.bin", g3) == "geargame\\cookedxbox360\\coalesced_int.bin");
        CHECK(mod_conflict_game_path("intro.bik", g3) == "geargame\\movies\\intro.bik");
    }
    // config fragments merge (no conflict); mod.ini, fabricator.json, read-me files, TOCs and programs are not game files
    // (programs: the launcher's whole mod_file_rules list - run.bat too, which the runtime itself would not skip)
    {
        const auto c = mod_conflicts({ mod("A", { "config\\DefaultGame.ini", "Config/weapons.ini", "mod.ini", "fabricator.json", "readme.txt",
                                                  "GearGame\\Xbox360TOC.txt", "GearGame\\Binaries\\tool.exe", "GearGame\\run.bat", "GearGame\\x.dll" }),
                                       mod("B", { "config\\DefaultGame.ini", "CONFIG\\weapons.ini", "MOD.INI", "fabricator.json", "README.txt",
                                                  "geargame\\xbox360toc.txt", "GearGame\\Binaries\\tool.exe", "GearGame\\run.bat", "GearGame\\x.dll" }) },
                                     gears2());
        CHECK(!c[0].any() && !c[1].any());
        CHECK(mod_conflict_game_path("config\\a.ini", gears2()).empty());
        // Gears 1 / Judgment have no fragments: config\ is a folder like any - used only if the game has one
        ModConflictRules g1;
        g1.fragments = false;
        g1.game_tops = { "geargame" };
        CHECK(mod_conflict_game_path("config\\a.ini", g1).empty());
        g1.game_tops = { "geargame", "config" };
        CHECK(mod_conflict_game_path("Config\\a.ini", g1) == "config\\a.ini");
        g1.game_tops.clear();   // (not known: counted)
        const auto d = mod_conflicts({ mod("A", { "config\\a.ini" }), mod("B", { "config\\A.INI" }) }, g1);
        CHECK(d[1].replaces.size() == 1);
        CHECK(mod_conflict_game_path("GearGame\\CookedXenon\\Startup_INT.xxx", gears2()) == "geargame\\cookedxenon\\startup_int.xxx");
    }
    // a folder the game hasn't is not used (the overlay skips it), when the game's folders are known
    {
        ModConflictRules r = gears2();
        r.game_tops = { "geargame", "engine" };
        const auto c = mod_conflicts({ mod("A", { "Docs\\notes.txt", "Engine\\a.xxx" }), mod("B", { "docs\\notes.txt", "engine\\A.xxx" }) }, r);
        CHECK(c[1].replaces.size() == 1 && c[1].replaces[0].count == 1 && c[1].replaces[0].names[0] == "engine\\A.xxx");
    }
    // texture packs: one hash under two names is a conflict; the pack loaded later wins
    {
        const auto c = mod_conflicts({ mod("Pack A", { "4D53082D\\replace\\0123456789ABCDEF_lancer.dds", "4D53082D\\replace\\1111111111111111.dds" }),
                                       mod("Pack B", { "textures\\0123456789abcdef_LancerHD.DDS", "textures\\2222222222222222_x.dds" }) },
                                     gears2());
        CHECK(c[0].tex_loses.size() == 1 && c[0].tex_loses[0].other_name == "Pack B" && c[0].tex_loses[0].count == 1);
        CHECK(c[0].tex_loses[0].names[0] == "4D53082D\\replace\\0123456789ABCDEF_lancer.dds");
        CHECK(c[1].tex_wins.size() == 1 && c[1].tex_wins[0].other == 0 && c[1].tex_wins[0].names[0] == "textures\\0123456789abcdef_LancerHD.DDS");
        CHECK(c[0].replaced_by.empty() && c[1].replaces.empty());   // textures are not counted as files too
        // the renderer's walk: never in dump, .build or replace_<variant>, at most 6 folders deep; the hash, then _ or .
        uint64_t h = 0;
        CHECK(mod_conflict_texture_hash("a\\b\\c\\d\\e\\f\\0123456789abcdef_x.dds", h) && h == 0x0123456789abcdefull);
        CHECK(!mod_conflict_texture_hash("a\\b\\c\\d\\e\\f\\g\\0123456789abcdef_x.dds", h));
        CHECK(!mod_conflict_texture_hash("pack\\dump\\0123456789abcdef_x.dds", h));
        CHECK(!mod_conflict_texture_hash("pack\\Replace_Alt\\0123456789abcdef_x.dds", h));
        CHECK(!mod_conflict_texture_hash(".build\\0123456789abcdef_x.dds", h));
        CHECK(!mod_conflict_texture_hash("0123456789abcdefx.dds", h));
        CHECK(!mod_conflict_texture_hash("0123456789abcdeg_x.dds", h));
        CHECK(mod_conflict_texture_hash("0123456789abcdef.dds", h));
        // a pack the game leaves out (Gears 3: a missing requirement) is still a texture pack, but its files aren't used
        const auto d = mod_conflicts({ mod("A", { "0123456789abcdef_x.dds", "GearGame\\a.xxx" }, false),
                                       mod("B", { "0123456789abcdef_y.dds", "GearGame\\a.xxx" }) }, gears2());
        CHECK(d[1].tex_wins.size() == 1 && d[1].replaces.empty());
    }
    // three mods on one file: the last wins over both; the middle one loses to it (not "wins" over the first)
    {
        const auto c = mod_conflicts({ mod("A", { "GearGame\\x.xxx", "GearGame\\ab.xxx" }), mod("B", { "GearGame\\x.xxx", "GearGame\\ab.xxx" }),
                                       mod("C", { "GearGame\\X.xxx" }) }, gears2());
        CHECK(c[2].replaces.size() == 2 && c[2].replaces[0].other == 0 && c[2].replaces[1].other == 1);
        CHECK(c[2].replaces[0].count == 1 && c[2].replaces[1].count == 1);
        CHECK(c[0].replaced_by.size() == 2);   // x by C, ab by B
        CHECK(c[0].replaced_by[0].other == 1 && c[0].replaced_by[0].names[0] == "GearGame\\ab.xxx");
        CHECK(c[0].replaced_by[1].other == 2 && c[0].replaced_by[1].names[0] == "GearGame\\x.xxx");
        CHECK(c[1].replaces.size() == 1 && c[1].replaces[0].other == 0 && c[1].replaces[0].names[0] == "GearGame\\ab.xxx");
        CHECK(c[1].replaced_by.size() == 1 && c[1].replaced_by[0].other == 2);
    }
    // disabled mods are not in the list given; a mod's own two paths to one file are one file
    {
        const auto c = mod_conflicts({ mod("A", { "Lancer.xxx", "GearGame\\CookedXenon\\Lancer.xxx" }) }, gears2());
        CHECK(!c[0].any());
        CHECK(mod_conflicts({}, gears2()).empty());
    }
    // a listing given by reference (the worker's cache, not copied) counts as its own files
    {
        const std::vector<std::string> cached = { "GearGame\\a.xxx" };
        ModFileList b = mod("B", {});
        b.ref = &cached;
        const auto c = mod_conflicts({ mod("A", { "GearGame\\A.xxx" }), b }, gears2());
        CHECK(c[1].replaces.size() == 1 && c[1].replaces[0].names[0] == "GearGame\\a.xxx");
    }
    // the names kept per pair: the first max_names in name order, with the full count
    {
        std::vector<std::string> a, b;
        for (int i = 0; i < 25; i++) {
            char n[64];
            snprintf(n, sizeof n, "GearGame\\f%02d.xxx", 24 - i);
            a.push_back(n);
            b.push_back(n);
        }
        const auto c = mod_conflicts({ mod("A", a), mod("B", b) }, gears2());
        CHECK(c[1].replaces.size() == 1 && c[1].replaces[0].count == 25 && c[1].replaces[0].names.size() == 10);
        CHECK(c[1].replaces[0].names.front() == "GearGame\\f00.xxx" && c[1].replaces[0].names.back() == "GearGame\\f09.xxx");
    }
    // large lists: two 50 000-file packs and two 50 000-file mods, half of each overlapping (about 50 ms here; the bound
    // is generous so a slow or busy machine doesn't fail it)
    {
        std::vector<ModFileList> v = { mod("Pack 1", {}), mod("Pack 2", {}), mod("Mod 1", {}), mod("Mod 2", {}) };
        for (int i = 0; i < 50000; i++) {
            char n[96];
            snprintf(n, sizeof n, "4D53082D\\replace\\%016llx_tex%d.dds", (unsigned long long)i * 2654435761ull, i);
            v[0].files.push_back(n);
            snprintf(n, sizeof n, "pack\\%016llx_hd%d.dds", (unsigned long long)(i + 25000) * 2654435761ull, i);
            v[1].files.push_back(n);
            snprintf(n, sizeof n, "GearGame\\CookedXenon\\Package%05d.xxx", i);
            v[2].files.push_back(n);
            snprintf(n, sizeof n, "GearGame\\CookedXenon\\PACKAGE%05d.xxx", i + 25000);
            v[3].files.push_back(n);
        }
        const auto t0 = std::chrono::steady_clock::now();
        const auto c = mod_conflicts(v, gears2());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("200 000 files: %.1f ms\n", ms);
        CHECK(ms < 1000);
        CHECK(c[1].tex_wins.size() == 1 && c[1].tex_wins[0].count == 25000);
        CHECK(c[3].replaces.size() == 1 && c[3].replaces[0].count == 25000 && c[3].replaces[0].names.size() == 10);
    }
    if (g_failures == 0) printf("mod_conflicts_test: all passed\n");
    return g_failures;
}
