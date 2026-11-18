// COGBox - test of the Creator's packaging helper: mod.ini editing, the folder check, the clean zip, the package name.
// Prints FAIL lines; exit code = failures. MinGW, from cogbox\:
//   gcc -w -c third_party/miniz/miniz.c -o miniz.o
//   g++ -std=c++17 -Wall -Wextra -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Isrc -Ithird_party
//       -I../../Gears3-Native/src/layer2_ue3   (modcore/campaigns.h: the mod API level)
//       test/fabricator_pack_test.cpp src/fabricator_pack.cpp src/mod_ini_edit.cpp src/fabricator_model.cpp
//       src/mod_file_rules.cpp src/util.cpp miniz.o
//       -static-libgcc -static-libstdc++ -lole32 -lshell32 -luuid -lbcrypt -ladvapi32 -o fabricator_pack_test.exe
#include "../src/fabricator_model.h"
#include "../src/fabricator_pack.h"
#include "../src/mod_ini_edit.h"
#include "../src/util.h"
#include "../third_party/miniz/miniz.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#include <windows.h>

static int g_failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

static bool has(const std::vector<std::string> &v, const char *part)
{
    for (const std::string &s : v) if (s.find(part) != std::string::npos) return true;
    return false;
}
static void put(const std::string &path, const std::string &data)
{
    make_dirs(path.substr(0, path.find_last_of("\\/")));
    FILE *f = _wfopen(widen(path).c_str(), L"wb");
    if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
}
static std::string slurp(const std::string &path)
{
    std::string s;
    read_file(path, s);
    return s;
}
// the entry names of a zip
static std::set<std::string> zip_names(const std::string &path)
{
    std::set<std::string> out;
    FILE *f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) return out;
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (mz_zip_reader_init_cfile(&z, f, 0, 0)) {
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); i++) {
            mz_zip_archive_file_stat st;
            if (mz_zip_reader_file_stat(&z, i, &st)) out.insert(st.m_filename);
        }
        mz_zip_reader_end(&z);
    }
    fclose(f);
    return out;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string tmp = join(std::filesystem::temp_directory_path().string(), "fabpack-test-\xC3\xA9");   // a non-ASCII folder
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(widen(tmp)), ec);
    make_dirs(tmp);

    // ---- mod.ini edit keeps the rest ----
    {
        const std::string dir = join(tmp, "ini");
        const std::string before = "; my mod\r\n[Mod]\r\nName = Old\r\nFoo=1\r\nApi = 1\r\n\r\n[Campaign]\r\nId = x\r\nName = Not the mod's\r\n";
        put(join(dir, "mod.ini"), before);
        ModIniFields w;
        w.name = "New \xC3\xA9 Name"; w.version = "2.0"; w.side = "host"; w.needs = { "A", "B" };
        CHECK(mod_ini_write(dir, w));
        auto r = mod_ini_read(dir);
        CHECK(r && r->name == w.name && r->version == "2.0" && r->side == "host" && r->needs == w.needs);
        CHECK(r && r->game.empty() && r->author.empty());
        const std::string after = slurp(join(dir, "mod.ini"));
        CHECK(after.find("Foo=1\r\n") != std::string::npos);
        CHECK(after.find("; my mod\r\n") == 0);
        CHECK(after.find("Api = 1\r\n") != std::string::npos);
        CHECK(after.find("[Campaign]\r\nId = x\r\nName = Not the mod's\r\n") != std::string::npos);   // unchanged and in order
        CHECK(after.find("[Mod]") < after.find("Foo=1") && after.find("Foo=1") < after.find("[Campaign]"));
        // every line ending is CRLF
        bool lone = false;
        for (size_t i = 0; i < after.size(); i++) if (after[i] == '\n' && (i == 0 || after[i - 1] != '\r')) lone = true;
        CHECK(!lone);
        // a second write with the same fields changes nothing; emptying a field removes its line only
        CHECK(mod_ini_write(dir, w) && slurp(join(dir, "mod.ini")) == after);
        ModIniFields w2 = w;
        w2.needs.clear();
        CHECK(mod_ini_write(dir, w2));
        auto r2 = mod_ini_read(dir);
        CHECK(r2 && r2->needs.empty() && slurp(join(dir, "mod.ini")).find("Requires") == std::string::npos);
        CHECK(slurp(join(dir, "mod.ini")).find("Foo=1\r\n") != std::string::npos);
    }
    // LF files stay LF, no [Mod] section gets one at the top, a BOM stays
    {
        const std::string dir = join(tmp, "ini2");
        put(join(dir, "mod.ini"), "\xEF\xBB\xBF[Other]\nA = 1\n");
        ModIniFields w;
        w.name = "N"; w.game = "gears2";
        CHECK(mod_ini_write(dir, w));
        const std::string s = slurp(join(dir, "mod.ini"));
        CHECK(s == "\xEF\xBB\xBF[Mod]\nName = N\nGame = gears2\n[Other]\nA = 1\n");
        auto r = mod_ini_read(dir);
        CHECK(r && r->name == "N" && r->game == "gears2");
        // no mod.ini at all: made
        const std::string dir3 = join(tmp, "ini3");
        make_dirs(dir3);
        CHECK(mod_ini_write(dir3, w));
        auto r3 = mod_ini_read(dir3);
        CHECK(r3 && r3->name == "N");
        CHECK(!mod_ini_read(join(tmp, "nothing-here")));
    }

    // ---- pack is clean ----
    {
        const std::string dir = join(tmp, "pack");
        put(join(dir, "mod.ini"), "[Mod]\nName = P\nGame = gears2\n");
        put(join(dir, "GearGame\\a.xxx"), "data");
        put(join(dir, "fabricator.json"), "{}");
        put(join(dir, ".git\\x"), "x");
        put(join(dir, "old.partial\\y"), "y");
        put(join(dir, "run.exe"), "MZ");
        put(join(dir, "inner.zip"), "PK");
        const std::string zip = join(tmp, "out\\P.zip");
        FabProgress p;
        std::vector<std::string> left = { "stale" };
        CHECK(fab_pack_folder(dir, zip, p, &left).empty());
        // only the program files and nested archives are named (not fabricator.json, .git or the .partial)
        std::sort(left.begin(), left.end());
        CHECK((left == std::vector<std::string>{ "inner.zip", "run.exe" }));
        const std::set<std::string> names = zip_names(zip);
        CHECK((names == std::set<std::string>{ "mod.ini", "GearGame/a.xxx" }));
        CHECK(p.total == 4 + (int64_t)slurp(join(dir, "mod.ini")).size() && p.done == p.total);
        CHECK(!file_exists(zip + ".tmp-cogbox"));
        const auto probs = fab_check_folder(dir, "gears2");
        CHECK(has(probs, "run.exe") && has(probs, "inner.zip"));
        CHECK(!has(probs, "fabricator.json") && !has(probs, ".git") && !has(probs, "partial"));
        CHECK(!has(probs, "mod.ini"));
        // a pack of the same folder replaces the zip; cancel leaves the old one alone
        FabProgress c;
        c.cancel = true;
        CHECK(fab_pack_folder(dir, zip, c) == "cancelled");
        CHECK(zip_names(zip).size() == 2);
        CHECK(!file_exists(zip + ".tmp-cogbox"));
        // an empty folder, and a folder of only skipped things
        make_dirs(join(tmp, "empty"));
        FabProgress e;
        CHECK(!fab_pack_folder(join(tmp, "empty"), join(tmp, "e.zip"), e).empty() && !file_exists(join(tmp, "e.zip")));
        // the result names no path outside the mod
        CHECK(fab_pack_folder(join(tmp, "empty"), join(tmp, "e.zip"), e).find(tmp) == std::string::npos);
    }

    // ---- the folder check's mod.ini rules ----
    {
        const std::string d1 = join(tmp, "c1");
        make_dirs(d1);
        CHECK(has(fab_check_folder(d1, "gears2"), "no mod.ini"));
        put(join(d1, "mod.ini"), "[Mod]\nVersion = 1\n");
        auto p1 = fab_check_folder(d1, "gears2");
        CHECK(has(p1, "no Name") && has(p1, "no Game"));
        put(join(d1, "mod.ini"), "[Mod]\nName = X\nGame = gears3\nApi = 99\n");
        auto p2 = fab_check_folder(d1, "gears3");
        CHECK(has(p2, "Api 99") && !has(p2, "no Name") && !has(p2, "wrong game"));
        CHECK(has(fab_check_folder(d1, "gears2"), "wrong game"));
        put(join(d1, "mod.ini"), "[Mod]\nName = X\nGame = gears3\nApi = 1\n");
        CHECK(fab_check_folder(d1, "gears3").empty());
        put(join(d1, "con.txt"), "x");
        CHECK(has(fab_check_folder(d1, "gears3"), "device name"));
    }

    // ---- package name ----
    {
        ModIniFields f;
        f.name = "Horde: Bank?"; f.version = "1.2";
        CHECK(fab_package_name(f) == "Horde Bank-1.2.zip");
        ModIniFields g;
        CHECK(fab_package_name(g) == "mod.zip");
        g.name = "??"; g.version = "::";
        CHECK(fab_package_name(g) == "mod.zip");
    }

    // ---- non-ASCII: a folder, files and mod.ini text pack and read back ----
    {
        const std::string dir = join(tmp, "m\xC3\xB6" "d \xE2\x80\x93 \xE6\x97\xA5");
        put(join(dir, "mod.ini"), "[Mod]\nName = \xE6\x97\xA5\xE6\x9C\xAC\nGame = gears3\n");
        put(join(dir, "Maps\\\xC3\xA9t\xC3\xA9.txt"), "z");
        const std::string zip = join(tmp, "\xE6\x97\xA5.zip");
        FabProgress p;
        CHECK(fab_pack_folder(dir, zip, p).empty());
        CHECK((zip_names(zip) == std::set<std::string>{ "mod.ini", "Maps/\xC3\xA9t\xC3\xA9.txt" }));
        FabZipInfo zi;
        CHECK(fab_check_zip(zip, "gears3", &zi).empty() && zi.name == "\xE6\x97\xA5\xE6\x9C\xAC");
        CHECK(fab_check_folder(dir, "gears3").empty());
        auto r = mod_ini_read(dir);
        CHECK(r && r->name == "\xE6\x97\xA5\xE6\x9C\xAC");
    }

    // ---- review fixes ----
    {
        // spacing and BOM + CRLF kept; Requires lines add up on read and fold into the first on write
        const std::string dir = join(tmp, "ini4");
        put(join(dir, "mod.ini"), "\xEF\xBB\xBF[Mod]\r\nName  =  Old\r\nRequires = A\r\nRequires = B, A\r\nApi=1\r\n");
        auto r = mod_ini_read(dir);
        CHECK(r && r->needs == (std::vector<std::string>{ "A", "B" }) && r->api == 1);
        ModIniFields w = *r;
        w.name = "New"; w.needs = { "C", "D" };
        CHECK(mod_ini_write(dir, w));
        CHECK(slurp(join(dir, "mod.ini")) == "\xEF\xBB\xBF[Mod]\r\nName  =  New\r\nRequires = C, D\r\nApi=1\r\n");
        // two [Mod] sections: every occurrence is edited, a new key goes into the last one
        const std::string d2 = join(tmp, "ini5");
        put(join(d2, "mod.ini"), "[Mod]\nName = One\n[Other]\nX=1\n[mod]\nName = Two\n[Tail]\nY=2\n");
        ModIniFields w2;
        w2.name = "Final"; w2.author = "me";
        CHECK(mod_ini_write(d2, w2));
        CHECK(slurp(join(d2, "mod.ini")) == "[Mod]\nName = Final\n[Other]\nX=1\n[mod]\nName = Final\nAuthor = me\n[Tail]\nY=2\n");
        auto r2 = mod_ini_read(d2);
        CHECK(r2 && r2->name == "Final" && r2->author == "me");
    }
    {
        // mod.ini over 256 KB: "too large", not "no mod.ini"
        const std::string dir = join(tmp, "big");
        put(join(dir, "mod.ini"), "[Mod]\nName = X\nGame = gears2\n; " + std::string(300 * 1024, 'x') + "\n");
        const auto pr = fab_check_folder(dir, "gears2");
        CHECK(has(pr, "mod.ini is too large") && !has(pr, "no mod.ini"));
        // the file-count cap counts files (not folders) and a cut-off folder is a problem, never a short zip
        const std::string d2 = join(tmp, "many");
        put(join(d2, "mod.ini"), "[Mod]\nName = X\nGame = gears2\n");
        for (int i = 0; i < 3; i++) put(join(d2, "sub" + std::to_string(i) + "\\f.txt"), "x");
        CHECK(fab_check_folder_limit(d2, "gears2", 4).empty());   // 4 files, 3 folders: at the limit
        CHECK(has(fab_check_folder_limit(d2, "gears2", 3), "too many files"));
        FabProgress p;
        const std::string zip = join(tmp, "many.zip");
        CHECK(fab_pack_folder_limit(d2, zip, p, 4).empty() && zip_names(zip).size() == 4);
        DeleteFileW(widen(zip).c_str());
        CHECK(fab_pack_folder_limit(d2, zip, p, 3).find("over 3 files") != std::string::npos && !file_exists(zip));
        // a folder that is not there is an error, not a clean empty one
        CHECK(has(fab_check_folder(join(tmp, "nope"), "gears2"), "couldn't read"));
    }

    std::filesystem::remove_all(std::filesystem::path(widen(tmp)), ec);
    printf(g_failures ? "%d FAILED\n" : "all ok\n", g_failures);
    return g_failures;
}
