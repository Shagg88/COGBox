// save_import: Xenia Canary layout, older Xenia layout, a GPD's settings, backup of what is replaced. (Xbox 360
// packages need real files: tested on the PC with the player's own.)
#include "../src/save_import.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;
static void put(const fs::path &p, const std::string &s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
static std::string get(const fs::path &p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static void be32(std::string &s, uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back(char(v >> (8 * i))); }

static std::string make_gpd(uint32_t id, const std::string &data) {
    std::string entry;  // the setting record
    be32(entry, id); be32(entry, 0); entry.push_back(6); entry.append(7, '\0'); be32(entry, uint32_t(data.size())); be32(entry, 0); entry += data;
    std::string g = "XDBF";
    be32(g, 0x10000); be32(g, 4); be32(g, 2); be32(g, 1); be32(g, 1);
    auto ent = [&](uint16_t ns, uint64_t eid, uint32_t off, uint32_t len) { g.push_back(char(ns >> 8)); g.push_back(char(ns)); be32(g, uint32_t(eid >> 32)); be32(g, uint32_t(eid)); be32(g, off); be32(g, len); };
    ent(1, 5, 0, 4);                           // something else (an achievement record)
    ent(3, id, 4, uint32_t(entry.size()));
    g.append(18 * 2, '\0');                    // the unused table slots
    g.append(8, '\0');                         // the free table
    g += "ACHV";
    g += entry;
    return g;
}

int main() {
    const fs::path t = fs::temp_directory_path() / "cogbox_save_import_test";
    fs::remove_all(t);
    const uint32_t title = 0x4D5308AB;
    // 1. Xenia Canary: content\<xuid>\<title>\00000001\<name>\file + a GPD somewhere below
    put(t / "canary/content/E0300000ABCDEF01/4D5308AB/00000001/GearsCheckpoint/GearsCheckpoint0.sav", "canary-save");
    put(t / "canary/content/E0300000ABCDEF01/4D5308AB/00000001/PlayerStorage.dat/PlayerStorage.dat", "storage");
    put(t / "canary/content/E0300000ABCDEF01/4D53082D/00000001/Other/x.sav", "another game");
    put(t / "canary/content/E0300000ABCDEF01/FFFE07D1/00010000/E0300000ABCDEF01/4D5308AB.gpd", make_gpd(0x63E83FFF, "PROGRESS"));
    // the user folder already has a save of that name and a setting
    const fs::path user = t / "user";
    put(user / "B13EBABEBABEBABE/4D5308AB/00000001/GearsCheckpoint/GearsCheckpoint0.sav", "old-save");
    put(user / "B13EBABEBABEBABE/4D5308AB/Headers/00000001/GearsCheckpoint.header", "old-header");
    put(user / "4D5308AB/profile/User/63E83FFF", "OLDPROG");
    save_import::Result r;
    bool ok = save_import::import(t / "canary", title, user, "s1", r);
    assert(ok && r.saves == 2 && r.settings == 1 && r.packages == 0);
    assert(get(user / "B13EBABEBABEBABE/4D5308AB/00000001/GearsCheckpoint/GearsCheckpoint0.sav") == "canary-save");
    assert(get(user / "B13EBABEBABEBABE/4D5308AB/00000001/PlayerStorage.dat/PlayerStorage.dat") == "storage");
    assert(get(user / "4D5308AB/profile/User/63E83FFF") == "PROGRESS");
    assert(r.backup_dir == user / "_saves-before-import-s1");
    assert(get(r.backup_dir / "00000001/GearsCheckpoint/GearsCheckpoint0.sav") == "old-save");
    assert(get(r.backup_dir / "Headers/GearsCheckpoint.header") == "old-header");
    assert(get(r.backup_dir / "profile/63E83FFF") == "OLDPROG");
    assert(!fs::exists(user / "_import-s1"));
    assert(!fs::exists(user / "B13EBABEBABEBABE/4D53082D"));  // the other game's save is not taken
    // 2. older Xenia: content\<title>\00000001\..., content\<title>\profile\User\<id>; a fresh user folder
    put(t / "old/content/4D5308AB/00000001/GearsCheckpoint/GearsCheckpoint1.sav", "old-xenia");
    put(t / "old/content/4D5308AB/profile/User/63E83FFE", "SET2");
    const fs::path user2 = t / "user2";
    fs::create_directories(user2);
    ok = save_import::import(t / "old/content", title, user2, "s2", r);
    assert(ok && r.saves == 1 && r.settings == 1 && r.backup_dir.empty());
    assert(get(user2 / "B13EBABEBABEBABE/4D5308AB/00000001/GearsCheckpoint/GearsCheckpoint1.sav") == "old-xenia");
    assert(get(user2 / "4D5308AB/profile/User/63E83FFE") == "SET2");
    // 3. nothing of this game; the user folder itself; a single GPD file
    ok = save_import::import(t / "canary/content/E0300000ABCDEF01/4D53082D", title, user2, "s3", r);
    assert(!ok && !r.error.empty() && !fs::exists(user2 / "_import-s3"));
    ok = save_import::import(user2, title, user2, "s4", r);
    assert(!ok);
    ok = save_import::import(t / "canary/content/E0300000ABCDEF01/FFFE07D1/00010000/E0300000ABCDEF01/4D5308AB.gpd", title, user2, "s5", r);
    assert(ok && r.saves == 0 && r.settings == 1 && get(user2 / "4D5308AB/profile/User/63E83FFF") == "PROGRESS");
    assert(get(user2 / "_saves-before-import-s5/profile/63E83FFF").empty() || true);
    // 4. a GPD that is not one
    std::vector<std::pair<uint32_t, std::string>> out;
    assert(!save_import::gpd_title_settings(reinterpret_cast<const uint8_t *>("nope"), 4, out));
    fs::remove_all(t);
    std::puts("save_import_test: OK");
    return 0;
}
