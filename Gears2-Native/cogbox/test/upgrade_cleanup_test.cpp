// CogBox release gate for upgrade cleanup (cleanup049), file level, on Linux:
//   g++ -std=c++17 -O2 -Wall -Wextra -I../src upgrade_cleanup_test.cpp ../src/upgrade_cleanup.cpp ../src/stale_files.cpp
//       ../src/update_logic.cpp ../src/gears_settings.cpp -o upgrade_cleanup_test
//   ./upgrade_cleanup_test [stale_releases_dir] [new SHA256SUMS.txt]
// For each release 0.4.0 ... 0.4.8 (stale_releases/<v>.txt: the SHA256SUMS.txt it shipped): its files, plus a
// player's things (saves, settings.ini with old keys, CogBox.ini, a mod, covers, ReShade / MFG Unlocker dxgi.dll beside
// each game, a custom nvngx_dlss.dll, shader caches), then the new release put over it BOTH ways:
//   by hand  - every file of the new release written over the folder (an unzip);
//   updater  - update.cpp's install: protected paths skipped (update_protected_path), each replaced file renamed to
//              .old-cogbox first; update_tidy then removes the .old-cogbox copies (only ever COGBox's own files).
// Then the launcher's start: stale_files (stale_path_ok + known_old_program), upgrade_cleanup (settings.ini,
// shader caches). Checked: only COGBox's own old files moved; player files untouched; settings migrated with a
// backup; no file left beside a game that the new version would load instead of shared\; caches moved, ucode kept.
// The new release: the given SHA256SUMS.txt, else 0.4.8's list with shared049's layout (vendor DLLs in shared\) and
// new hashes (a new DXC and Agility runtime). Each file's content in the model is its SHA-256 (the hash IS the bytes).
#include "stale_files.h"
#include "upgrade_cleanup.h"
#include "update_logic.h"
#include "gears_settings.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace cogbox;

static int g_fail = 0, g_pass = 0;
#define CHECK_MSG(c, m) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, #c, std::string(m).c_str()); } } while (0)

static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::string lower(std::string s) { for (char &c : s) { c = (char)tolower((unsigned char)c); if (c == '/') c = '\\'; } return s; }
static std::string base(const std::string &l) { size_t i = l.find_last_of('\\'); return i == std::string::npos ? l : l.substr(i + 1); }
static std::string dir_of(const std::string &l) { size_t i = l.find_last_of('\\'); return i == std::string::npos ? "" : l.substr(0, i); }
static std::string fake_hash(int n) { char b[65]; snprintf(b, sizeof b, "%064x", 0xfeed000 + n); return b; }

typedef std::vector<std::pair<std::string, std::string>> Files;   // path as shipped, hash
static Files load_list(const std::string &text)
{
    Files r;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() > 66) r.push_back({ line.substr(66), lower(line.substr(0, 64)) });
    }
    return r;
}
static std::string to_sums(const Files &f) { std::string s; for (auto &x : f) s += x.second + "  " + x.first + "\r\n"; return s; }

struct Disk {
    std::map<std::string, std::string> content;   // lower path -> bytes (program files: their hash)
    std::map<std::string, std::string> shown;     // lower path -> name on disk
    void put(const std::string &p, const std::string &c) { std::string l = lower(p); content[l] = c; if (!shown.count(l)) shown[l] = p; }
    void erase(const std::string &l) { content.erase(l); shown.erase(l); }
};

static const char *kOldSettings =
    "; COGBox - Gears of War settings\n[General]\nGame=1\nFps=2\nWineD3D12=1\nExclusive=1\nFullscreen=1\nAA=1\nOldRemovedKey=7\n"
    "Vsync=true\n\n[Gears2]\nAniso=8\nNoSuchOption=1\n";

int main(int argc, char **argv)
{
    std::string dir = argc > 1 ? argv[1] : std::string(__FILE__).substr(0, std::string(__FILE__).find_last_of("/\\") + 1) + "stale_releases";
    // ---- the new release ----
    Files r048 = load_list(slurp(dir + "/0.4.8.txt"));
    CHECK_MSG(r048.size() > 50, "0.4.8 list");
    Files newRel;
    if (argc > 2) newRel = load_list(slurp(argv[2]));
    else {
        const std::set<std::string> toShared = { "nvngx_dlss.dll", "dxil.dll", "amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_framegeneration_vk.dll",
                                                 "sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll" };
        int k = 0;
        for (auto &f : r048) {
            const std::string l = lower(f.first);
            if (l.find("\\port") != std::string::npos && toShared.count(base(l))) continue;
            newRel.push_back({ f.first, fake_hash(k++) });
        }
        for (const std::string &n : toShared) newRel.push_back({ "shared\\" + n, fake_hash(k++) });
    }
    ShippedList now;
    CHECK_MSG(parse_shipped(to_sums(newRel), now), "new list");
    const std::string newSums = to_sums(newRel), newKey = upgrade_cache_key(newSums);
    CHECK_MSG(!newKey.empty(), "new cache key");
    std::set<std::string> newHashes;
    for (auto &f : newRel) newHashes.insert(f.second);

    // ---- pure rules ----
    CHECK_MSG(upgrade_streamline_leftover("Gears2\\port-tu6\\sl.interposer.dll", now) && upgrade_streamline_leftover("Judgment/port/nvngx_dlssg.dll", now), "streamline leftover");
    CHECK_MSG(!upgrade_streamline_leftover("Gears2\\port-tu6 - Copy\\sl.common.dll", now) && !upgrade_streamline_leftover("Gears2\\mods\\sl.common.dll", now) &&
              !upgrade_streamline_leftover("shared\\sl.common.dll", now) && !upgrade_streamline_leftover("Gears2\\port-tu6\\nvngx_dlss.dll", now) &&
              !upgrade_streamline_leftover("Gears2\\port-tu6\\x\\sl.common.dll", now), "not streamline leftovers");
    CHECK_MSG(upgrade_cache_file("shaders.pack") && upgrade_cache_file("pipelines-d3d12.bin") && upgrade_cache_file("PREPARE-PROGRESS.TXT") &&
              upgrade_cache_file("vs-0123.dxil") && upgrade_cache_file("ps-1.spv"), "cache files");
    CHECK_MSG(!upgrade_cache_file("ucode\\recorded.pack") && !upgrade_cache_file("ucode/recorded.pack") && !upgrade_cache_file("notes.txt") &&
              !upgrade_cache_file("my\\shaders.pack"), "not cache files");
    CHECK_MSG(upgrade_cache_key("") == "", "empty key");
    CHECK_MSG(upgrade_cache_key(to_sums(r048)) != newKey, "0.4.8 -> new: compiler changed");
    CHECK_MSG(upgrade_cache_key(newSums) == newKey, "stable key");
    for (const char *p : { "CogBox.ini", "settings.ini", "extra-args.txt", "extra-args-gears2.txt", "covers\\gears2.png", "_old-files\\x.dll",
                           "achievements\\gears2.json", "Gears2\\user\\a.sav", "Gears2\\mods\\m.zip", "Gears2\\shadercache\\shaders.pack",
                           "Gears2/game/default.xex", "Gears2\\saves\\1.sav", "logs\\x.txt" })
        CHECK_MSG(cogbox_update::update_protected_path(p), std::string("protected: ") + p);
    for (const char *p : { "CogBox.exe", "shared\\nvngx_dlss.dll", "Gears2\\port-tu6\\gow2.exe", "lang\\de.txt", "SHA256SUMS.txt" })
        CHECK_MSG(!cogbox_update::update_protected_path(p), std::string("not protected: ") + p);
    {   // settings migration alone
        std::string out;
        bool changed = false;
        CHECK_MSG(upgrade_settings_text(kOldSettings, &out, &changed) && changed, "settings migrate");
        CHECK_MSG(out.find("Exclusive") == std::string::npos && out.find("OldRemovedKey") == std::string::npos &&
                  out.find("WineD3D12") == std::string::npos && out.find("NoSuchOption") == std::string::npos && out.find("\nFps=") == std::string::npos, "dead keys dropped:\n" + out);
        cogbox::gears::Store st;
        CHECK_MSG(st.load_ini(out), "migrated loads");
        CHECK_MSG(st.top["Fullscreen"] == 2.0, "exclusive kept as Fullscreen=2");
        CHECK_MSG(st.top["WineApi"] == 2.0, "WineD3D12 -> WineApi 2");
        CHECK_MSG(st.top["FrameCap"] == 5.0, "Fps 2 (120) -> FrameCap 5 (list 2)");
        CHECK_MSG(st.last_game == 1, "Game kept");
        std::string again;
        bool changed2 = true;
        CHECK_MSG(upgrade_settings_text(out, &again, &changed2) && !changed2 && again == out, "migration is stable");
        CHECK_MSG(!upgrade_settings_text("garbage\n", &again, &changed2), "not a settings file: left alone");
    }

    // ---- the release gate ----
    const char *tags[] = { "0.4.0", "0.4.1", "0.4.2", "0.4.3", "0.4.4", "0.4.5", "0.4.6", "0.4.7", "0.4.8" };
    const std::string playerDll = fake_hash(900), customDlss = fake_hash(901), modDll = fake_hash(902);
    std::printf("%-7s %-7s %5s %6s %8s  %s\n", "from", "path", "moved", "caches", "settings", "of them Streamline");
    for (const char *t : tags) {
        const Files old = load_list(slurp(dir + "/" + t + ".txt"));
        CHECK_MSG(old.size() > 10, t);
        for (int way = 0; way < 2; way++) {
            const char *wayName = way ? "updater" : "by-hand";
            Disk d;
            for (auto &f : old) d.put(f.first, f.second);
            d.put("SHA256SUMS.txt", to_sums(old));
            // the player's things
            std::map<std::string, std::string> player = {
                { "settings.ini", kOldSettings }, { "CogBox.ini", "[Notices]\nWhatsNewSeen=" + std::string(t) + "\n" },
                { "extra-args.txt", "--fullscreen\n" }, { "covers\\gears2.png", "png" }, { "achievements\\gears2.json", "{}" },
                { "Gears2\\user\\0000000000000000\\4D5307D5\\00000001\\save.sav", "save" }, { "Gears3\\user\\save2.sav", "save" },
                { "Gears2\\mods\\mymod.zip", "mod" }, { "Gears2\\mods\\test\\rexruntime.dll", old[2].second },   // a byte copy of ours, in the player's folder
                { "Gears2\\port-tu6\\dxgi.dll", playerDll }, { "Gears3\\port-tu6\\dxgi.dll", playerDll }, { "Judgment\\port\\reshade.ini", "x" },
                { "Gears1\\port\\dxgi.dll", playerDll }, { "Judgment\\port\\nvngx_dlss.dll", customDlss },   // MFG Unlocker / ReShade, the custom DLSS file
                { "Gears2\\shadercache\\ucode\\recorded.pack", "ucode" }, { "Gears2\\port-tu6\\my-plugin.dll", modDll },
                { "Gears3\\port-tu6\\sl.interposer.dll", fake_hash(903) },   // a player's own Streamline file (other bytes)
            };
            for (auto &p : player) d.put(p.first, p.second);
            const std::map<std::string, std::string> caches = { { "Gears2\\shadercache\\shaders.pack", "pack" },
                                                                { "Gears2\\shadercache\\pipelines-d3d12.bin", "pso" },
                                                                { "Gears2\\shadercache\\prepare-progress.txt", "1 1 done 9" },
                                                                { "Judgment\\shadercache\\shaders.pack", "pack" } };
            for (auto &c : caches) d.put(c.first, c.second);
            // the new release over it
            std::set<std::string> tidied;
            for (auto &f : newRel) {
                const std::string l = lower(f.first);
                if (way == 1) {
                    if (cogbox_update::update_protected_path(f.first)) continue;
                    if (d.content.count(l)) d.put(f.first + ".old-cogbox", d.content[l]);
                }
                d.put(f.first, f.second);
            }
            d.put("SHA256SUMS.txt", newSums);
            if (way == 1) {   // update_tidy at the next start: the .old-cogbox copies go
                std::vector<std::string> gone;
                for (auto &c : d.content) if (c.first.size() > 11 && c.first.compare(c.first.size() - 11, 11, ".old-cogbox") == 0) gone.push_back(c.first);
                for (auto &g : gone) {
                    const std::string orig = g.substr(0, g.size() - 11);
                    CHECK_MSG(!player.count(d.shown[orig]) && !cogbox_update::update_protected_path(orig), std::string(t) + " updater replaced a player file: " + orig);
                    tidied.insert(orig);
                    d.erase(g);
                }
            }
            // the launcher's start: stale_files
            std::set<std::string> moved;
            for (auto &c : d.content) {
                if (!stale_path_ok(d.shown[c.first], now)) continue;
                if (known_old_program(c.second, {})) moved.insert(c.first);
            }
            CHECK_MSG((int)moved.size() <= kStaleMaxMoves, "cap");
            // upgrade_cleanup step 3 (Shagg 2026-11-08): 0.4.8's Streamline copies beside each game, now in shared
            int slMoves = 0;
            for (auto &c : d.content)
                if (!moved.count(c.first) && upgrade_streamline_leftover(d.shown[c.first], now) && known_old_program(c.second, {})) { moved.insert(c.first); slMoves++; }
            // expected: the old release's program files the new one doesn't list, still with our bytes (the Streamline
            // ones too, moved by upgrade_cleanup below; a player's own copy - other bytes - stays)
            auto isStreamline = [](const std::string &l) { const std::string b = base(l); return b.compare(0, 3, "sl.") == 0 || b == "nvngx_dlssg.dll"; };
            std::set<std::string> expect;
            for (auto &f : old) {
                const std::string l = lower(f.first), b = base(l);
                if (now.files.count(l) || d.content[l] != f.second) continue;
                (void)b;
                expect.insert(l);
            }
            for (auto &e : expect) CHECK_MSG(moved.count(e), std::string(t) + " " + wayName + " should move " + e);
            for (auto &m : moved) CHECK_MSG(expect.count(m), std::string(t) + " " + wayName + " moved " + m);
            for (auto &m : moved) d.erase(m);
            // upgrade_cleanup: settings
            std::string migrated;
            bool changed = false;
            CHECK_MSG(upgrade_settings_text(d.content["settings.ini"], &migrated, &changed) && changed, std::string(t) + " settings");
            const std::string backup = d.content["settings.ini"];
            d.put("settings.ini", migrated);
            CHECK_MSG(backup == kOldSettings, std::string(t) + " " + wayName + ": the updater wrote settings.ini");
            // upgrade_cleanup: caches (the old key from the old release's list, as _old-files\shipped keeps it)
            int cacheMoves = 0;
            if (upgrade_cache_key(to_sums(old)) != newKey) {
                std::vector<std::string> mv;
                for (auto &c : d.content) {
                    const std::string l = c.first;
                    size_t p = l.find("\\shadercache\\");
                    if (p != std::string::npos && upgrade_cache_file(l.substr(p + 13))) mv.push_back(l);
                }
                for (auto &m : mv) { d.erase(m); cacheMoves++; }
            }
            CHECK_MSG(cacheMoves == (int)caches.size(), std::string(t) + " caches moved " + std::to_string(cacheMoves));
            // checks: player files untouched
            for (auto &p : player) {
                if (p.first == "settings.ini") continue;
                const std::string l = lower(p.first);
                CHECK_MSG(d.content.count(l) && d.content[l] == p.second, std::string(t) + " " + wayName + " player file changed: " + p.first);
            }
            CHECK_MSG(d.content.count("gears2\\shadercache\\ucode\\recorded.pack"), "ucode kept");
            // nothing beside a game exe that the new version would load instead of its own / shared\ copy
            std::set<std::string> portDirs;
            for (auto &f : newRel) { const std::string l = lower(f.first); if (base(l).size() > 4 && l.compare(l.size() - 4, 4, ".exe") == 0 && l.find('\\') != std::string::npos) portDirs.insert(dir_of(l)); }
            for (auto &c : d.content) {
                const std::string &l = c.first;
                if (!portDirs.count(dir_of(l)) || l.size() < 4 || l.compare(l.size() - 4, 4, ".dll") != 0 || now.files.count(l)) continue;
                bool isPlayer = false;
                for (auto &p : player) isPlayer |= lower(p.first) == l;
                if (isPlayer) continue;
                const std::string b = base(l);
                CHECK_MSG(false, std::string(t) + " " + wayName + ": an old DLL the new version could load: " + l);
            }
            // no old program file anywhere the release ships to, except the Streamline ones above
            for (auto &c : d.content)
                if (stale_path_ok(d.shown[c.first], now) && known_old_program(c.second, {}))
                    CHECK_MSG(false, std::string(t) + " left: " + c.first);
            for (auto &c : d.content)
                if (isStreamline(c.first) && known_old_program(c.second, {}) && !now.files.count(c.first))
                    CHECK_MSG(false, std::string(t) + " Streamline left: " + c.first);
            std::printf("%-7s %-7s %5zu %6d %8s  %d\n", t, wayName, moved.size(), cacheMoves, "migrated", slMoves);
        }
    }
    // menu5 (0.5.0): Judgment's Brightness default 65 -> 75. A file from before keeps every chosen value; its old
    // default 65 (always written in [General], and in a game's section once that game's settings were saved) becomes 75.
    {
        namespace gs = cogbox::gears;
        const gs::Option *bo = gs::find_option(gs::Game::Judgment, "Brightness");
        CHECK_MSG(bo && bo->def == 75, "Brightness default 75");
        gs::Store old;
        old.load_ini("[General]\nGame=2\nFrameCapList=2\nBrightness=65\n\n[Judgment]\nFrameCapList=2\nBrightness=65\n\n[Gears2]\nBrightness=90\n");
        gs::Environment env;
        CHECK_MSG(old.view(gs::Game::Judgment, env).i("Brightness") == 75, "old default in [Judgment] -> 75");
        CHECK_MSG(old.top["Brightness"] == 75, "old default in [General] -> 75");
        CHECK_MSG(old.per[0]["Brightness"] == 90, "a chosen value stays");
        const std::string w = old.save_ini();
        CHECK_MSG(w.find("BrightnessDefault=75") != std::string::npos, "marker written");
        gs::Store again;   // a file written with the new default: 65 is now the player's choice and stays
        again.load_ini("[General]\nGame=2\nFrameCapList=2\nBrightnessDefault=75\nBrightness=75\n\n[Judgment]\nFrameCapList=2\nBrightness=65\n");
        CHECK_MSG(again.view(gs::Game::Judgment, env).i("Brightness") == 65, "65 chosen after 0.5.0 stays");
        gs::Store fresh;   // no file at all: the default
        fresh.load_ini("");
        CHECK_MSG(fresh.view(gs::Game::Judgment, env).i("Brightness") == 75, "no saved setting -> 75");
        gs::Store other;   // an old file where the player had picked 80: kept
        other.load_ini("[General]\nBrightness=65\n\n[Judgment]\nBrightness=80\n");
        CHECK_MSG(other.view(gs::Game::Judgment, env).i("Brightness") == 80, "chosen 80 stays");
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
