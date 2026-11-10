// CogBox test of stale_files (0.4.8 old-program-file cleanup), against the SHA256SUMS.txt of every published release.
//   g++ -std=c++17 -O2 -Wall -Wextra -I../src stale_files_test.cpp ../src/stale_files.cpp -o stale_files_test
//   ./stale_files_test [stale_releases_dir]        (default: stale_releases next to this file)
// For each past release R: the install folder = R's files + the new release's files unzipped on top + the player's and
// the games' run-time files + other programs' files (a COGBox folder shared with other things). The new release is
// 0.4.7's list with new hashes (0.4.8 ships the same files). The decision is the launcher's: stale_path_ok, then
// known_old_program on the file's hash. Checked: exactly R's program files that the new release no longer lists move,
// and nothing else - in particular no user / run-time file, even one that is a byte copy of a shipped DLL.
#include "stale_files.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

using namespace cogbox;

static int g_fail = 0, g_pass = 0;
#define CHECK_MSG(c, m) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, #c, std::string(m).c_str()); } } while (0)

static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::string lower(std::string s) { for (char &c : s) { c = (char)tolower((unsigned char)c); if (c == '/') c = '\\'; } return s; }
static std::string upper(std::string s) { for (char &c : s) c = (char)toupper((unsigned char)c); return s; }
static std::string fake_hash(int n) { char b[65]; snprintf(b, sizeof b, "%064x", 0xabc000 + n); return b; }   // never a real one

struct Release { std::vector<std::pair<std::string, std::string>> files; };   // path (as shipped), hash
static Release load_rel(const std::string &p)
{
    Release r;
    std::istringstream in(slurp(p));
    std::string line;
    while (std::getline(in, line)) if (line.size() > 66) r.files.push_back({ line.substr(66), lower(line.substr(0, 64)) });
    return r;
}

int main(int argc, char **argv)
{
    std::string dir = argc > 1 ? argv[1] : std::string(__FILE__).substr(0, std::string(__FILE__).find_last_of("/\\") + 1) + "stale_releases";
    const char *tags[] = { "0.1.0", "0.2.0", "0.2.1", "0.2.2-test", "0.2.2", "0.2.5", "0.3.0", "0.3.5", "0.3.6", "0.3.7",
                           "0.4.0", "0.4.1", "0.4.2", "0.4.3", "0.4.4", "0.4.5", "0.4.6", "0.4.7" };
    // the new release (0.4.8): 0.4.7's files, rebuilt (new hashes)
    std::string newSums;
    Release r047 = load_rel(dir + "/0.4.7.txt");
    CHECK_MSG(r047.files.size() > 20, "0.4.7 list");
    int n = 0;
    for (auto &f : r047.files) newSums += fake_hash(9000 + n++) + "  " + f.first + "\r\n";
    ShippedList now;
    CHECK_MSG(parse_shipped("\xEF\xBB\xBF" + newSums, now), "parse");
    CHECK_MSG(now.files.count("gears2\\port-tu6\\gow2.exe") && now.files.count("cogbox.exe"), "parsed paths");
    std::vector<std::string> other;
    for (const char *o : { "/0.4.7-other-files.lst", "/0.1.0-other-files.lst" }) {
        std::istringstream in(slurp(dir + o)); std::string l;
        while (std::getline(in, l)) if (!l.empty()) other.push_back(l);
    }
    const std::string anyOldDll = load_rel(dir + "/0.1.0.txt").files[2].second;   // a real shipped DLL's bytes
    // the player's, the games' and other programs' files: { path, hash } - none may ever move
    std::vector<std::pair<std::string, std::string>> userFiles = {
        { "CogBox.ini", "" }, { "settings.ini", "" }, { "extra-args.txt", "" }, { "extra-args-gears2.txt", "" },
        { "COGBox-0.4.5.zip", "" }, { "CogBox-saves-Gears2-20261101-120000.zip", "" }, { "notes.txt", "" },
        { "Gears2\\user\\0000000000000000\\4D5307D5\\00000001\\save.sav", "" }, { "Gears2\\game\\extracted\\default.xex", "" },
        { "Gears2\\game\\update\\default.xexp", "" }, { "Gears2\\shadercache\\shaders.bin", "" }, { "logs\\gow2.log", "" },
        { "achievements\\gears2.json", "" }, { "covers\\gears2.png", "" }, { "Gears2\\mods\\mymod.zip", "" },
        { "Gears2\\port-tu6\\gow2.log", "" }, { "Gears2\\port-tu6\\ini-edits.txt", "" }, { "Gears2\\port-tu6\\imgui.ini", "" },
        { "Gears2\\port-tu6\\crash-20261101.dmp", "" }, { "Gears2\\port-tu6\\gow2.exe.old-cogbox", anyOldDll },
        { "Gears2\\port-tu6\\gow2_manifest_tu6.toml", "" },
        // DLLs players add beside a game (ReShade, DLSS-G / Streamline, DXVK ...)
        { "Gears2\\port-tu6\\dxgi.dll", fake_hash(1) }, { "Gears2\\port-tu6\\sl.interposer.dll", fake_hash(2) },
        { "Gears2\\port-tu6\\sl.dlss_g.dll", fake_hash(3) }, { "Gears2\\port-tu6\\nvngx_dlssg.dll", fake_hash(4) },
        { "Judgment\\port\\d3d11.dll", fake_hash(5) }, { "Gears3\\port-tu6\\ReShade64.dll", fake_hash(6) },
        { "Gears1\\port\\some-tool.exe", fake_hash(7) }, { "shared\\my-own.dll", fake_hash(8) }, { "MyTool.exe", fake_hash(9) },
        // byte copies of shipped programs the player keeps somewhere of his own
        { "Gears2\\mods\\test\\rexruntime.dll", anyOldDll }, { "Gears2\\user\\backup\\gow2.exe", anyOldDll },
        { "Gears2\\port-tu6 - Copy\\rexruntime.dll", anyOldDll }, { "Gears2\\port.bak\\rexruntime.dll", anyOldDll },
        { "backup\\CogBox.exe", anyOldDll }, { "COGBox-0.4.5\\Gears2\\port-tu6\\gow2.exe", anyOldDll },
        // a COGBox unzipped into a folder with other things (Downloads, a games folder)
        { "SteamSetup.exe", fake_hash(10) }, { "7z.dll", fake_hash(11) }, { "OtherGame\\bin\\game.exe", fake_hash(12) },
        { "_old-files\\from-0.4.5\\Gears2\\port-disc\\gow2.exe", anyOldDll },
        // an old program file the player replaced with a build of his own: not COGBox's bytes -> kept
        { "Gears2\\port-disc\\custom.dll", fake_hash(13) },
    };
    const std::set<std::string> noExtra;
    std::printf("%-11s %5s  %s\n", "from", "moves", "files that would move to _old-files (0.4.8 SHA256SUMS)");
    for (const char *t : tags) {
        Release r = load_rel(dir + "/" + t + ".txt");
        CHECK_MSG(r.files.size() > 10, t);
        std::map<std::string, std::string> disk;   // lower path -> hash; later writes win (unzip over)
        std::map<std::string, std::string> shown;  // lower -> name as on disk
        for (auto &f : r.files) { disk[lower(f.first)] = f.second; shown[lower(f.first)] = f.first; }
        // case-only renames: the old release's names in upper case, the new one's in its own case - one file on Windows
        for (auto &f : r047.files) {
            std::string l = lower(f.first);
            if (!shown.count(l)) shown[l] = f.first; else shown[l] = upper(shown[l]);
            disk[l] = "new";
        }
        for (auto &o : other) { disk[lower(o)] = ""; shown[lower(o)] = o; }
        for (auto &u : userFiles) { disk[lower(u.first)] = u.second; shown[lower(u.first)] = u.first; }
        std::set<std::string> expect;
        for (auto &f : r.files) if (!now.files.count(lower(f.first))) expect.insert(lower(f.first));
        std::set<std::string> moved;
        for (auto &d : disk) {
            if (!stale_path_ok(shown[d.first], now)) continue;
            if (known_old_program(d.second, noExtra)) moved.insert(d.first);
        }
        std::string list;
        for (auto &m : moved) list += (list.empty() ? "" : ", ") + shown[m];
        std::printf("%-11s %5zu  %s\n", t, moved.size(), list.empty() ? "-" : list.c_str());
        for (auto &e : expect) CHECK_MSG(moved.count(e), std::string(t) + " should move " + e);
        for (auto &m : moved) CHECK_MSG(expect.count(m), std::string(t) + " moved a file it shouldn't: " + m);
        for (auto &u : userFiles) CHECK_MSG(!moved.count(lower(u.first)), std::string(t) + " moved user file " + u.first);
        CHECK_MSG((int)moved.size() <= kStaleMaxMoves, std::string(t) + " over the cap");
    }
    // shared049 (0.4.9): the vendor DLLs leave the game folders for shared\. Unzipped over 0.4.8: the old copies of
    // nvngx_dlss.dll, dxil.dll and the two AMD Vulkan DLLs beside each game move; COGBox's 0.4.8 Streamline copies
    // (sl.*.dll, nvngx_dlssg.dll) stay (players add those themselves - the game prefers shared\ for them); a player's
    // own nvngx_dlss.dll (the launcher's "Custom DLSS file": not COGBox's bytes) stays; D3D12Core / runtimes are listed.
    {
        Release r048 = load_rel(dir + "/0.4.8.txt");
        CHECK_MSG(r048.files.size() > 50, "0.4.8 list");
        const std::set<std::string> toShared = { "nvngx_dlss.dll", "dxil.dll", "amd_fidelityfx_loader_vk.dll",
                                                 "amd_fidelityfx_framegeneration_vk.dll", "sl.interposer.dll", "sl.common.dll",
                                                 "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll" };
        auto base = [](const std::string &l) { return l.substr(l.find_last_of('\\') + 1); };
        std::string sums049;
        int k = 0;
        for (auto &f : r048.files) {
            const std::string l = lower(f.first);
            if (l.find("\\port") != std::string::npos && toShared.count(base(l))) continue;   // no longer beside a game
            sums049 += fake_hash(20000 + k++) + "  " + f.first + "\r\n";
        }
        for (const std::string &n2 : toShared) sums049 += fake_hash(20000 + k++) + "  shared\\" + n2 + "\r\n";
        ShippedList now049;
        CHECK_MSG(parse_shipped(sums049, now049), "parse 0.4.9");
        std::map<std::string, std::string> disk;
        for (auto &f : r048.files) disk[lower(f.first)] = f.second;
        disk["judgment\\port\\nvngx_dlss.dll"] = fake_hash(30);   // the player's own DLSS file (Custom DLSS file)
        disk["gears2\\port-tu6\\dxgi.dll"] = fake_hash(31);       // MFG Unlocker / ReShade
        std::set<std::string> moved;
        for (auto &d : disk) if (stale_path_ok(d.first, now049) && known_old_program(d.second, noExtra)) moved.insert(d.first);
        int dlss = 0, dxil = 0, amd = 0;
        for (auto &m : moved) {
            const std::string b = base(m);
            dlss += b == "nvngx_dlss.dll"; dxil += b == "dxil.dll"; amd += b.compare(0, 14, "amd_fidelityfx") == 0;
            CHECK_MSG(b.compare(0, 3, "sl.") != 0 && b != "nvngx_dlssg.dll", "Streamline moved: " + m);
            CHECK_MSG(b != "dxgi.dll", "dxgi.dll moved");
        }
        CHECK_MSG(dlss == 3 && dxil == 4 && amd == 8, "0.4.8 -> 0.4.9 moves: dlss " + std::to_string(dlss) + " dxil " +
                  std::to_string(dxil) + " amd " + std::to_string(amd));
        CHECK_MSG(!moved.count("judgment\\port\\nvngx_dlss.dll"), "player's DLSS file moved");
        CHECK_MSG(moved.size() == 15, "0.4.8 -> 0.4.9 moved " + std::to_string(moved.size()));
        std::printf("0.4.8->0.4.9 shared049: %zu moves (nvngx_dlss %d, dxil %d, AMD Vulkan %d); Streamline + player files kept\n",
                    moved.size(), dlss, dxil, amd);
    }
    // single rules
    std::string why;
    CHECK_MSG(!stale_path_ok("..\\x.dll", now, &why), why);
    CHECK_MSG(!stale_path_ok("Gears2\\port-tu6\\GOW2.EXE", now, &why), "case-only: listed");
    CHECK_MSG(!stale_path_ok("Gears2\\port-tu6\\D3D12\\D3D12Core.dll", now), "listed");
    CHECK_MSG(stale_path_ok("Gears2/port-disc/D3D12/D3D12Core.dll", now), "old port folder");
    CHECK_MSG(stale_path_ok("Gears3\\port-disc\\rexruntime.dll", now), "old port folder");
    CHECK_MSG(!stale_path_ok("Gears2\\user\\x.dll", now), "user");
    CHECK_MSG(!stale_path_ok("Gears2\\port-tu6\\user\\x.dll", now), "user inside port");
    CHECK_MSG(!stale_path_ok("recipes\\recipes-gow2.rcp", now), "not exe/dll");
    CHECK_MSG(!stale_path_ok("CogBox.exe", now), "launcher");
    CHECK_MSG(!known_old_program(fake_hash(1), noExtra) && known_old_program(fake_hash(1), { fake_hash(1) }), "extra lists");
    CHECK_MSG(!known_old_program("xyz", noExtra), "bad hex");
    // 0.4.9 review: a Custom DLSS file whose bytes are a DLSS DLL some COGBox release shipped (0.1.0's, a REAL hash from
    // stale_known.h) is still the player's choice: kept while its custom-dlss.txt marker holds the same hash.
    {
        const std::string realOld = "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e";
        const std::string rel = "Judgment\\port\\nvngx_dlss.dll";
        CHECK_MSG(known_old_program(realOld, noExtra), "0.1.0's DLSS hash is known");
        ShippedList rel049;   // 0.4.9 ships DLSS in shared\, not beside the exe
        CHECK_MSG(parse_shipped(fake_hash(50) + "  CogBox.exe\r\n" + fake_hash(51) + "  shared\\nvngx_dlss.dll\r\n", rel049), "parse");
        CHECK_MSG(stale_path_ok(rel, rel049), "the place alone would allow moving it");
        CHECK_MSG(stale_custom_dlss_marker(rel) == "_old-files\\dlss-backup\\judgment\\port\\custom-dlss.txt", stale_custom_dlss_marker(rel));
        CHECK_MSG(stale_is_custom_dlss(rel, realOld, realOld + "\r\n"), "custom marker -> kept");
        CHECK_MSG(stale_is_custom_dlss("judgment/port/NVNGX_DLSS.DLL", upper(realOld), "\xEF\xBB\xBF" + upper(realOld) + "\n"), "case / BOM");
        CHECK_MSG(!stale_is_custom_dlss(rel, realOld, ""), "no marker -> may move");
        CHECK_MSG(!stale_is_custom_dlss(rel, realOld, fake_hash(9)), "marker of another file -> may move");
        CHECK_MSG(!stale_is_custom_dlss("Judgment\\port\\dxil.dll", realOld, realOld), "only nvngx_dlss.dll");
        // the launcher's decision for that file: kept
        const bool moves = stale_path_ok(rel, rel049) && !stale_is_custom_dlss(rel, realOld, realOld) && known_old_program(realOld, noExtra);
        CHECK_MSG(!moves, "player's DLSS file with an old COGBox hash moved");
    }
    std::printf("stale_files_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
