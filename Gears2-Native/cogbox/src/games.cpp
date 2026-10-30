// COGBox - the three games (see games.h)
#include "games.h"
#include "x360fs.h"

#include <windows.h>
#include <cstdio>

// The fingerprints are the ones Gears2-Native's launcher checks (tools\launcher.ps1, patch 0131): the US discs and the
// last title updates the ports are made for. Title IDs: x360fs.h (checked on real discs and updates, job 472).
static const GameDef k_games[G_COUNT] = {
    { G_GEARS2, "Gears of War 2", "Gears 2", "gears2", "Gears2", x360fs::kTitleGears2, "gow2.exe", "gow2.log",
      "Title Update 6", true, "port-tu6", "port-disc",
      { 18595840, "b6860bf31e55013467b53eca963d12314c0d78da", "eb8aedd4567c220176ae4e7d95c89f667a361387" }, { 2074624, "8689bf52b2a47908eb9281d05c5e86653db0cf11", "1659a0b7a43256e01ae86960ca11df9b44692a48" },
      "2008", "Add your copy of the disc to play" },
    { G_GEARS3, "Gears of War 3", "Gears 3", "gears3", "Gears3", x360fs::kTitleGears3, "gow3.exe", "gow3.log",
      "Title Update 6", true, "port-tu6", "port-disc",
      { 21938176, "2450fd8e4c3cf4d772cd471785d1e38cad3888ad", "35bf62c8d47fe948fea0b994a984706c5f6690a6" }, { 1908736, "5c335a50116064d2dc91dce1ec922ec9e1975b62", "3d0a51599c1c63ea929c8a91c7faada8153628f2" },
      "2011", "Add your copy of the disc to play" },
    { G_JUDGMENT, "Gears of War: Judgment", "Judgment", "judgment", "Judgment", x360fs::kTitleGearsJudgment, "gowj.exe", "gowj.log",
      "Title Update 4", false, "", "port",
      { 22855680, "426c5d27ff73d3ff2af9d404adb49e9a3db18bf1", "8fb96d4149721dc94fc0024b3001d2b734c814df" }, { 1888256, "88e876b10276535379d5524f2bef73ed9195870e", "269c0ae816e077ad37ec945c98abae6f0cafd2a0" },
      "2013", "Add your copy of the disc to play" },
};

const GameDef &game_def(int g) { return k_games[g < 0 || g >= G_COUNT ? 0 : g]; }

int game_by_title_id(uint32_t tid)
{
    for (int g = 0; g < G_COUNT; g++)
        if (k_games[g].titleId == tid) return g;
    return -1;
}

std::string game_root(int g) { return join(exe_dir(), game_def(g).dir); }
std::string game_user_dir(int g) { return join(game_root(g), "user"); }
std::string game_logs_dir(int g) { return join(game_root(g), "logs"); }
std::string game_shader_dir(int g) { return join(game_root(g), "shadercache"); }
std::string game_default_data_dir(int g) { return join(game_root(g), "game\\extracted"); }
std::string game_default_update_dir(int g) { return join(game_root(g), "game\\update"); }

std::string GameState::versionText() const
{
    if (ready != R_READY) return "";
    return useUpdate ? "Title Update" : "Disc version";
}

static int64_t file_time(const std::string &p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(p).c_str(), GetFileExInfoStandard, &d)) return 0;
    return (int64_t)(((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime);
}

std::string cached_sha1(Ini &ini, const std::string &path)
{
    uint64_t size = file_size(path);
    int64_t t = file_time(path);
    char tag[64];
    snprintf(tag, sizeof tag, "%llu:%lld:", (unsigned long long)size, (long long)t);
    std::string have = ini.get("Hashes", path);
    if (have.size() > 40 && have.compare(0, strlen(tag), tag) == 0) return have.substr(strlen(tag));
    std::string h = x360fs::sha1_file(widen(path));
    if (!h.empty()) ini.set("Hashes", path, tag + h);
    return h;
}

// Job 647: the program's own fingerprint (the XEX after its header), cached like cached_sha1
std::string cached_code_sha1(Ini &ini, const std::string &path)
{
    uint64_t size = file_size(path);
    int64_t t = file_time(path);
    char tag[64];
    snprintf(tag, sizeof tag, "%llu:%lld:", (unsigned long long)size, (long long)t);
    std::string have = ini.get("CodeHashes", path);
    if (have.size() > 40 && have.compare(0, strlen(tag), tag) == 0) return have.substr(strlen(tag));
    std::string h = x360fs::xex_code_sha1(widen(path));
    if (!h.empty()) ini.set("CodeHashes", path, tag + h);
    return h;
}

static bool fp_match(const Fingerprint &f, const std::string &full, const std::string &code)
{
    return (!full.empty() && full == f.sha1) || (!code.empty() && *f.code && code == f.code);
}
bool xex_is_tested(const GameDef &d, const std::string &full, const std::string &code) { return fp_match(d.xex, full, code); }
// Job 648 (Shagg 23:23): a region disc's title update - the same patch after its header counts as the tested one
// Title updates players have reported working, beyond the one each port is made for (GitHub #51, LeffetheProgrammer:
// a Gears 2 Title Update 6 of the same size with a different file - probably another region's copy).
struct ReportedXexp { GameId game; const char *sha1; };
static const ReportedXexp k_reportedXexp[] = {
    { G_GEARS2, "acfe0507c5a196eb322b36e91b4ca44233d40da5" },   // #51, 2,074,624 bytes
};
bool xexp_is_tested(const GameDef &d, const std::string &full, const std::string &code)
{
    if (fp_match(d.xexp, full, code)) return true;
    for (const ReportedXexp &r : k_reportedXexp) if (r.game == d.id && !full.empty() && full == r.sha1) return true;
    return false;
}

void game_set_data(Ini &ini, int g, const std::string &dataDir, bool own)
{
    const GameDef &d = game_def(g);
    if (dataDir.empty()) ini.remove(d.key, "GameFolder"); else ini.set(d.key, "GameFolder", dataDir);
    ini.seti(d.key, "OwnFolder", own ? 1 : 0);
}

void game_set_update(Ini &ini, int g, const std::string &updateDir)
{
    const GameDef &d = game_def(g);
    if (updateDir.empty()) ini.remove(d.key, "UpdateFolder"); else ini.set(d.key, "UpdateFolder", updateDir);
}

void game_refresh(int g, Ini &ini, GameState &st, bool hash)
{
    const GameDef &d = game_def(g);
    std::string root = game_root(g);
    st.portExeTu = *d.portTu ? join(join(root, d.portTu), d.exeName) : "";
    st.portExeDisc = *d.portDisc ? join(join(root, d.portDisc), d.exeName) : "";
    // ports chosen by hand (developers: a build folder elsewhere) - [gears2] PortTu = <exe>, PortDisc = <exe>
    std::string ot = ini.get(d.key, "PortTu"), od = ini.get(d.key, "PortDisc");
    if (!ot.empty()) st.portExeTu = ot;
    if (!od.empty()) st.portExeDisc = od;
    if (!st.portExeTu.empty() && !file_exists(st.portExeTu)) st.portExeTu.clear();
    if (!st.portExeDisc.empty() && !file_exists(st.portExeDisc)) st.portExeDisc.clear();

    std::string data = ini.get(d.key, "GameFolder");
    st.dataOwn = ini.geti(d.key, "OwnFolder", 0) != 0;
    bool remembered = !data.empty();
    // 0.2.0: also when the remembered folder holds no game (0.2.0's first DLC fix saved the DLC's folder there) but
    // COGBox's own unpacked copy is in place
    if ((data.empty() || !file_exists(join(data, "default.xex"))) && file_exists(join(game_default_data_dir(g), "default.xex"))) data = game_default_data_dir(g);
    st.dataDir = file_exists(join(data, "default.xex")) ? data : "";
    std::string upd = ini.get(d.key, "UpdateFolder");
    if (upd.empty()) upd = game_default_update_dir(g);
    st.updateDir = file_exists(join(upd, "default.xexp")) ? upd : "";

    if (hash) {
        st.xexKnown = !st.dataDir.empty();
        const std::string hx = st.xexKnown ? cached_sha1(ini, join(st.dataDir, "default.xex")) : std::string();
        st.xexMatch = st.xexKnown && (hx == d.xex.sha1 || xex_is_tested(d, hx, cached_code_sha1(ini, join(st.dataDir, "default.xex"))));
        st.xexConfirmed = st.xexKnown && !st.xexMatch && !hx.empty() && ini.get(d.key, "WorksXex") == hx;
        st.xexpKnown = !st.updateDir.empty();
        const std::string hp = st.xexpKnown ? cached_sha1(ini, join(st.updateDir, "default.xexp")) : std::string();
        st.xexpMatch = st.xexpKnown && (hp == d.xexp.sha1 || xexp_is_tested(d, hp, cached_code_sha1(ini, join(st.updateDir, "default.xexp"))));
        st.xexpConfirmed = st.xexpKnown && !st.xexpMatch && !hp.empty() && ini.get(d.key, "WorksXexp") == hp;
    }
    st.playSeconds = ini.geti64("PlayTime", d.key, 0);
    st.lastPlayed = ini.geti64("LastPlayed", d.key, 0);

    if (st.portExeTu.empty() && st.portExeDisc.empty()) st.ready = R_NO_PORT;
    else if (st.dataDir.empty()) st.ready = remembered ? R_COPY_MISSING : R_NO_COPY;
    else st.ready = R_READY;
    // The title update build when the update is there; which version Play starts is the settings' Version choice
    // (gears_settings effective_version), applied by the caller. Gears 2 / 3 without the update need the disc build.
    st.useUpdate = !st.updateDir.empty() && (d.updateNeeded ? !st.portExeTu.empty() : true);
    if (st.ready == R_READY && d.updateNeeded && !st.useUpdate && st.portExeDisc.empty()) st.ready = R_NEED_UPDATE;
    // 0.2.0 (GitHub #1): Judgment's disc version crashes at start in this release; Title Update 4 is required for now.
    if (st.ready == R_READY && g == G_JUDGMENT && st.updateDir.empty()) st.ready = R_NEED_UPDATE;
}
