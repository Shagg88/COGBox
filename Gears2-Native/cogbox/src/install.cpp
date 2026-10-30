// COGBox - "Add your copy": whatever the player has (a disc image, a Games on Demand copy, an extracted folder, a title
// update package) is recognised, matched to one of the three games, checked, and turned into the folders the ports
// read. Unpacking runs on a worker thread into a "...partial" folder that is renamed into place only when it is
// complete, so a cancel or a failure never leaves a half copy where the game looks for it.
#include "app.h"
#include "theme.h"
#include "x360fs.h"

#include <windows.h>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace {
struct Plan {
    std::string source;          // what the player chose (UTF-8)
    x360fs::SourceKind kind = x360fs::SourceKind::Unknown;
    std::wstring resolved;       // what x360fs resolved it to
    int game = -1;
    bool update = false;         // a title update (goes to game\update)
    std::string finalDir;        // where it ends up
    std::string partialDir;      // where it is unpacked first
    bool inPlace = false;        // an extracted folder used where it is
    bool dlc = false;            // 0.2.0: downloadable content (goes to user\0000000000000000\<title>\00000002\<name>)
    std::string dlcName;         // its folder name there (the package's own file name, at most 42 characters)
    std::string dlcTitle;        // its display name
    std::string headerFile;      // the runtime's content header: user\0000000000000000\<title>\Headers\00000002\<name>.header
    std::vector<uint8_t> header; // what goes in it (XCONTENT_AGGREGATE_DATA + the licence mask)
    // Shagg 29 Oct: a version COGBox has no title ID for (e.g. a Japanese or other regional release, its update or
    // DLC) is added to the game whose page it was added from, as untested, instead of being refused.
    uint32_t titleId = 0;        // the title ID it carries
    bool untested = false;       // added although COGBox doesn't know that title ID
};
Plan g_plan;
}

static uint64_t free_space(const std::string &dir)
{
    std::string d = dir;
    while (!d.empty() && !dir_exists(d)) { size_t s = d.find_last_of('\\'); if (s == std::string::npos) break; d = d.substr(0, s); }
    ULARGE_INTEGER freeBytes;
    if (!GetDiskFreeSpaceExW(widen(d).c_str(), &freeBytes, nullptr, nullptr)) return ~0ull;
    return freeBytes.QuadPart;
}

// Moving a folder aside (when the player replaces a copy COGBox made, the old one is removed after the new one is
// in place - the player asked for the replacement).
static bool remove_tree(const std::wstring &dir)
{
    DWORD at = GetFileAttributesW(dir.c_str());
    if (at == INVALID_FILE_ATTRIBUTES) return false;
    if (at & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(dir.c_str()) != 0;   // a link: only the link goes, never what it points at
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) remove_tree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

static int game_of_xex(const x360fs::XexInfo &xi, const std::string &xexPath)
{
    int g = game_by_title_id(xi.title_id);
    if (g >= 0 || xexPath.empty()) return g;
    std::string h = x360fs::sha1_file(widen(xexPath));
    const std::string c = x360fs::xex_code_sha1(widen(xexPath));   // job 647: region discs
    for (int i = 0; i < G_COUNT; i++) if (xex_is_tested(game_def(i), h, c)) return i;
    return -1;
}

static const char *kind_text(x360fs::SourceKind k)
{
    switch (k) {
    case x360fs::SourceKind::DiscImage: return "Unpacking the disc image";
    case x360fs::SourceKind::GodContainer: return "Unpacking the Games on Demand copy";
    case x360fs::SourceKind::StfsPackage: return g_plan.dlc ? "Unpacking the downloadable content" : "Unpacking the title update";
    case x360fs::SourceKind::ExtractedFolder: return "Checking the folder";
    default: return "Reading";
    }
}

static bool dir_has_files(const std::string &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((widen(dir) + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) any = dir_has_files(narrow(widen(dir) + L"\\" + n));
        else any = true;
    } while (!any && FindNextFileW(f, &fd));
    FindClose(f);
    return any;
}

static void worker(App *ap)
{
    App &a = *ap;
    InstallJob &j = a.job;
    Plan p = g_plan;
    auto progress = [&](uint64_t done, uint64_t total, const std::string &cur) {
        j.done = done; j.total = total;
        std::lock_guard<std::mutex> l(j.m);
        j.current = cur;
    };
    std::string err;
    bool ok = true;
    if (!p.inPlace) {
        std::wstring part = widen(p.partialDir);
        if (dir_exists(p.partialDir)) remove_tree(part);   // a partial copy from an earlier, interrupted try
        make_dirs(p.partialDir);
        switch (p.kind) {
        case x360fs::SourceKind::DiscImage: ok = x360fs::extract_disc(p.resolved, part, progress, &j.cancel, err); break;
        case x360fs::SourceKind::GodContainer: ok = x360fs::extract_god(p.resolved, part, progress, &j.cancel, err); break;
        case x360fs::SourceKind::StfsPackage: ok = x360fs::extract_stfs(p.resolved, part, progress, &j.cancel, err, true); break;
        default: ok = false; err = "This isn't something COGBox can unpack.";
        }
        if (ok && p.dlc && !dir_has_files(p.partialDir)) { ok = false; err = "The package was empty."; }
        if (ok && !p.dlc && !file_exists(join(p.partialDir, p.update ? "default.xexp" : "default.xex"))) {
            ok = false;
            err = p.update ? "The package didn't hold a default.xexp - is it the title update?" : "The copy didn't hold a default.xex.";
        }
        if (!ok) {
            remove_tree(part);   // only COGBox's own half copy
            std::lock_guard<std::mutex> l(j.m);
            j.error = err;
            j.state = j.cancel ? 4 : 3;
            return;
        }
        // into place: the old copy (COGBox's own) out of the way first, removed once the new one is there
        std::wstring fin = widen(p.finalDir), old = fin + L".old-" + widen(now_stamp());
        bool hadOld = dir_exists(p.finalDir);
        if (hadOld && !MoveFileExW(fin.c_str(), old.c_str(), 0)) {
            std::lock_guard<std::mutex> l(j.m);
            j.error = "The old copy couldn't be moved aside - is the game running, or a file from it open?";
            j.state = 3;
            return;
        }
        if (!MoveFileExW(part.c_str(), fin.c_str(), 0)) {
            // outside review: say so if the old copy can't go back, and where it is
            bool restored = !hadOld || MoveFileExW(old.c_str(), fin.c_str(), 0);
            std::lock_guard<std::mutex> l(j.m);
            j.error = restored ? "The unpacked copy couldn't be moved into place." :
                                 "The unpacked copy couldn't be moved into place, and the old copy couldn't be put back - it is safe in " + narrow(old) + " (rename it to " + p.finalDir + " to use it again).";
            j.state = 3;
            return;
        }
        if (hadOld) remove_tree(old);
    }
    if (p.dlc) {
        // the content header the runtime reads (ContentManager::ReadContentHeaderFile / LoadPackageLicenseMask)
        std::string hd = p.headerFile.substr(0, p.headerFile.find_last_of('\\'));
        make_dirs(hd);
        FILE *f = _wfopen(widen(p.headerFile).c_str(), L"wb");
        bool wrote = f && fwrite(p.header.data(), 1, p.header.size(), f) == p.header.size();
        if (f) fclose(f);
        std::lock_guard<std::mutex> l(j.m);
        if (!wrote) { j.error = "The content was unpacked, but its header file couldn't be written to " + p.headerFile + "."; j.state = 3; return; }
        j.result = (p.dlcTitle.empty() ? std::string("The downloadable content") : "\"" + p.dlcTitle + "\"") + " is added to " + game_def(p.game).name +
                   ". The game finds it the next time it starts." +
                   (p.untested ? " It is for a version COGBox hasn't been tested with, so you may experience problems." : "");
        j.state = 2;
        return;
    }
    // the fingerprint, against what the ports are made for
    const GameDef &d = game_def(p.game);
    std::string file = join(p.finalDir, p.update ? "default.xexp" : "default.xex");
    std::string h = x360fs::sha1_file(widen(file));
    const std::string code = x360fs::xex_code_sha1(widen(file));   // jobs 647 / 648: region discs and their updates
    bool match = p.update ? xexp_is_tested(d, h, code) : xex_is_tested(d, h, code);
    std::lock_guard<std::mutex> l(j.m);
    // Shagg 29 Oct: a version or update COGBox wasn't made for is added with a warning, never refused.
    if (p.update)
        j.result = match ? std::string(d.updateName) + " for " + d.name + " is added and checked." :
                           std::string("This title update for ") + d.name + " hasn't been tested with COGBox (the port is made for " + d.updateName +
                           "). It was added - it may work, but you may experience problems. If the game won't start, choose the disc version on the Gameplay tab.";
    else
        j.result = match ? std::string(d.name) + " is added and checked - ready to play." :
                           std::string("This version of ") + d.name + " hasn't been tested with COGBox (the ports are made for the US release). "
                           "It was added - it may work, but you may experience problems.";
    j.state = 2;
}

static void begin(App &a)
{
    InstallJob &j = a.job;
    if (j.state.load() == 1) return;   // one at a time
    if (j.th.joinable()) j.th.join();
    j.cancel = false; j.done = 0; j.total = 0;
    {
        std::lock_guard<std::mutex> l(j.m);
        j.what = std::string(kind_text(g_plan.kind)) + " - " + game_def(g_plan.game).name;
        j.current.clear(); j.error.clear(); j.result.clear();
    }
    j.game = g_plan.game;
    j.state = 1;
    j.th = std::thread(worker, &a);
}

static void put_be(std::vector<uint8_t> &b, size_t at, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) b[at + i] = uint8_t(v >> (8 * (bytes - 1 - i)));
}
static void write_title_id(std::vector<uint8_t> &h, uint32_t tid) { if (h.size() >= 0x144) put_be(h, 0x140, tid, 4); }

// 0.2.0: downloadable content. Builds the runtime's content header (rexglue's XCONTENT_AGGREGATE_DATA, 0x148 bytes,
// big-endian: device id 1 = HDD, content type 2, display name (128 UTF-16BE), file name (42 + 2), xuid 0, title id),
// then the licence mask as the SDK's own InstallContent computes it (the bits of every licence entry with flags set).
static bool dlc_plan(Plan &p, const x360fs::PackageInfo &pi, std::string &err)
{
    p.dlc = true;
    p.dlcTitle = pi.display_name;
    std::string base = narrow(std::filesystem::path(p.resolved).filename().wstring()), name;
    for (char c : base) if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') name += c;
    if (name.size() > 42) name = name.substr(0, 42);
    if (name.empty() || name == "." || name == "..") { err = "This package's file name can't be used - rename it (letters and digits) and add it again."; return false; }
    p.dlcName = name;
    uint8_t h[0x611] = {};
    FILE *f = _wfopen(p.resolved.c_str(), L"rb");
    size_t n = f ? fread(h, 1, sizeof h, f) : 0;
    if (f) fclose(f);
    if (n < sizeof h) { err = "This package couldn't be read."; return false; }
    std::vector<uint8_t> &o = p.header;
    o.assign(0x148 + 4, 0);
    put_be(o, 0x0, 1, 4);                                  // DummyDeviceId::HDD
    put_be(o, 0x4, x360fs::kContentMarketplace, 4);
    std::memcpy(&o[0x8], h + 0x411, 0x7E);                 // display name: the first locale's 0x80 bytes (UTF-16BE), last char kept 0
    std::memcpy(&o[0x108], name.data(), name.size());
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++) {
        const uint8_t *e = h + 0x22C + i * 0x10;           // licence id (8), bits (4), flags (4)
        uint32_t bits = (uint32_t(e[8]) << 24) | (uint32_t(e[9]) << 16) | (uint32_t(e[10]) << 8) | e[11];
        uint32_t flags = (uint32_t(e[12]) << 24) | (uint32_t(e[13]) << 16) | (uint32_t(e[14]) << 8) | e[15];
        if (flags) mask |= bits;
    }
    std::memcpy(&o[0x148], &mask, 4);                      // host order, as the SDK writes it
    return true;
}

void install_start(App &a, const std::string &path, int forGame)
{
    if (a.job.state.load() == 1 || a.modal.open) return;
    if (a.run.proc) { set_message(a, "Close the game first.", true); return; }
    Plan p;
    p.source = path;
    std::filesystem::path res;
    p.kind = x360fs::detect(widen(path), &res);
    p.resolved = res.wstring();
    std::string resolved = narrow(p.resolved);
    std::string err;
    switch (p.kind) {
    case x360fs::SourceKind::ExtractedFolder: {
        x360fs::XexInfo xi;
        std::string xex = join(resolved, "default.xex");
        x360fs::read_xex_info(widen(xex), xi);
        p.game = game_of_xex(xi, xex);
        p.titleId = xi.title_id;
        p.inPlace = true;
        p.finalDir = resolved;
        break;
    }
    case x360fs::SourceKind::DiscImage: {
        x360fs::XexInfo xi;
        if (!x360fs::disc_xex_info(p.resolved, xi)) { set_message(a, "This disc image has no default.xex - it isn't an Xbox 360 game disc (or is damaged).", true); return; }
        p.game = game_by_title_id(xi.title_id);
        p.titleId = xi.title_id;
        break;
    }
    case x360fs::SourceKind::GodContainer:
    case x360fs::SourceKind::StfsPackage: {
        x360fs::PackageInfo pi;
        if (!x360fs::read_package_info(p.resolved, pi, err)) { set_message(a, err, true); return; }
        p.game = game_by_title_id(pi.title_id);
        p.titleId = pi.title_id;
        if (p.kind == x360fs::SourceKind::StfsPackage) {
            if (pi.content_type == x360fs::kContentMarketplace) {
                if (!dlc_plan(p, pi, err)) { set_message(a, err, true); return; }
            } else if (!pi.is_title_update()) {
                set_message(a, "This package isn't a title update or downloadable content (" + (pi.display_name.empty() ? std::string("content type ") + std::to_string(pi.content_type) : pi.display_name) +
                                   ").", true);
                return;
            } else
                p.update = true;
        }
        break;
    }
    default:
        set_message(a, "COGBox doesn't recognise this. Choose a disc image (.iso), a Games on Demand file or folder, "
                       "a title update package, or a folder holding default.xex.", true);
        return;
    }
    if (p.game < 0) {
        // Shagg 29 Oct: regional releases (Japanese, European ...) carry other title IDs and some players run them
        // fine - from a game's own page it is added to that game, as untested; only a drop on the main window, which
        // names no game, still asks for the game's page.
        char tid[16];
        snprintf(tid, sizeof tid, "%08X", p.titleId);
        if (forGame < 0) {
            set_message(a, std::string("COGBox doesn't know this version (title ID ") + tid + "). If it is a regional release of "
                           "Gears of War 2, 3 or Judgment, add it from that game's page - it will be added as an untested version.", true);
            return;
        }
        p.game = forGame;
        p.untested = true;
    }
    if (forGame >= 0 && forGame != p.game)
        set_message(a, std::string("That's ") + game_def(p.game).name + ", not " + game_def(forGame).name + " - adding it there.", false);
    const GameDef &d = game_def(p.game);
    if (p.inPlace) {
        // nothing to unpack: remember the folder; the check runs on the worker
        g_plan = p;
        begin(a);
        return;
    }
    if (p.dlc) {
        char tid[16];
        snprintf(tid, sizeof tid, "%08X", game_def(p.game).titleId);
        std::string root = join(join(game_user_dir(p.game), "0000000000000000"), tid);
        p.finalDir = join(join(root, "00000002"), p.dlcName);
        p.headerFile = join(join(join(root, "Headers"), "00000002"), p.dlcName + ".header");
        write_title_id(p.header, game_def(p.game).titleId);
    } else
        p.finalDir = p.update ? game_default_update_dir(p.game) : game_default_data_dir(p.game);
    p.partialDir = p.finalDir + ".partial";
    // the file chosen must not live inside the folder it's about to replace (review 6)
    {
        auto lower = [](std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; };
        std::string src = lower(narrow(p.resolved));
        for (const std::string &d : { p.finalDir, p.partialDir })
            if (src.compare(0, d.size() + 1, lower(d) + "\\") == 0) {
                set_message(a, "That file is inside the folder COGBox unpacks the game into - move it somewhere else first, then add it.", true);
                return;
            }
    }
    // room for it (the files plus a little)
    uint64_t need = 0;
    if (!p.update) {
        std::vector<x360fs::Entry> list;
        // 0.2.0: downloadable content is a package (STFS), not a Games on Demand copy - each kind is measured by its own reader
        bool listed = p.kind == x360fs::SourceKind::DiscImage ? x360fs::list_disc(p.resolved, list, err) :
                      p.kind == x360fs::SourceKind::StfsPackage ? x360fs::list_stfs(p.resolved, list, err) : x360fs::list_god(p.resolved, list, err);
        if (!listed) { set_message(a, err, true); return; }
        for (auto &e : list) need += e.size;
        uint64_t have = free_space(game_root(p.game));
        if (have != ~0ull && have < need + (256ull << 20)) {
            set_message(a, "Not enough free space: unpacking " + std::string(d.name) + " needs " + human_bytes(need) + ", and the drive with the COGBox folder has " +
                               human_bytes(have) + ". Free some space, or extract it elsewhere and add that folder.", true);
            return;
        }
    }
    g_plan = p;
    if (dir_exists(p.finalDir)) {
        ask(a, "replace", p.game, p.dlc ? "Replace the downloadable content" : p.update ? "Replace the title update" : "Replace your copy",
            std::string("COGBox already has ") + (p.dlc ? "this downloadable content for " + std::string(d.name) : p.update ? std::string(d.updateName) : std::string("a copy of ") + d.name) +
                ". Replace it with this one? The old one is removed once the new one is unpacked and in place." +
                (need ? "\n\nUnpacking needs " + human_bytes(need) + " of space." : ""),
            { "Replace", "Cancel" });
        return;
    }
    begin(a);
}

void install_confirmed(App &a) { begin(a); }

void install_poll(App &a)
{
    InstallJob &j = a.job;
    int s = j.state.load();
    if (s < 2) return;
    if (j.th.joinable()) j.th.join();
    std::lock_guard<std::mutex> l(j.m);
    if (s == 2) {
        // 0.2.0: downloadable content is not the game's copy or update - its folder is never remembered as either
        if (g_plan.dlc) {}
        else if (g_plan.update) game_set_update(a.ini, g_plan.game, g_plan.finalDir);
        else game_set_data(a.ini, g_plan.game, g_plan.finalDir, g_plan.inPlace);
        a.ini.save();
        refresh_games(a, true);
        // the page's values again from the store, so Version follows the newly added update (unless the page has unsaved changes)
        if (!a.dirty[g_plan.game]) { a.cur[g_plan.game] = a.store.view((gs::Game)g_plan.game, a.env[g_plan.game]); env_refresh(a, g_plan.game); }
        a.front = g_plan.game;
        bool good = j.result.find("checked") != std::string::npos;
        set_message(a, j.result, !good);
        // a game without its title update: say where it comes from
        const GameDef &d = game_def(g_plan.game);
        if (!g_plan.update && !g_plan.dlc && a.st[g_plan.game].updateDir.empty() && d.updateNeeded)
            ask(a, "info", g_plan.game, std::string(d.name) + " is added",
                j.result + "\n\nNext, add " + d.updateName + " (the last update, which the main port build is made for). It's on a console that "
                "has played the game online, in Content\\0000000000000000\\" + [&] { char b[12]; snprintf(b, sizeof b, "%08X", d.titleId); return std::string(b); }() +
                "\\000B0000. Without it, the disc version is started.", { "OK" });
    } else if (s == 3) set_message(a, "Couldn't add it: " + j.error, true);
    else set_message(a, "Cancelled - nothing was changed.", false);
    j.state = 0;
}

void install_draw(App &a, ImVec2 size)
{
    InstallJob &j = a.job;
    if (j.state.load() != 1) return;
    ImDrawList *dl = ImGui::GetWindowDrawList();   // the main window's: under the box below, over everything else
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 170));
    float w = std::min(size.x - S(60), S(620)), h = S(200);
    ImVec2 p0((size.x - w) * 0.5f, (size.y - h) * 0.5f), p1(p0.x + w, p0.y + h);
    ImGui::SetNextWindowPos(p0);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##install", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList *wl = ImGui::GetWindowDrawList();
    th_plate(wl, p0, p1, S(14), false);
    std::string what, cur;
    { std::lock_guard<std::mutex> l(j.m); what = j.what; cur = j.current; }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + S(26), p0.y + S(22)));
    th_heading(what.c_str(), p1.x - S(26));
    uint64_t done = j.done, total = j.total;
    float frac = total ? (float)((double)done / total) : 0.0f;
    char ov[64];
    if (total) snprintf(ov, sizeof ov, "%s of %s", human_bytes(done).c_str(), human_bytes(total).c_str());
    else snprintf(ov, sizeof ov, "Starting...");
    ImGui::SetCursorScreenPos(ImVec2(p0.x + S(26), p0.y + S(70)));
    ImGui::ProgressBar(frac, ImVec2(w - S(52), S(24)), ov);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + S(26), p0.y + S(104)));
    ImGui::PushFont(F_SMALL, FS(F_SMALL));
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    if (cur.size() > 70) cur = "..." + cur.substr(cur.size() - 67);
    ImGui::TextUnformatted(cur.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p1.x - S(26) - S(130), p1.y - S(56)));
    if (th_button("CANCEL", ImVec2(S(130), S(36)), !j.cancel, F_HEAD) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) j.cancel = true;
    ImGui::End();
}

// ---- 0.2.0: the downloadable content list (About tab) ------------------------------------------------------
namespace {
std::vector<DlcItem> g_dlc[G_COUNT];
DWORD g_dlcAt[G_COUNT] = {};
int g_dlcJobSeen = -1;
DlcItem g_dlcRemove;
int g_dlcRemoveGame = -1;

std::string dlc_root(int g)
{
    char tid[16];
    snprintf(tid, sizeof tid, "%08X", game_def(g).titleId);
    return join(join(game_user_dir(g), "0000000000000000"), tid);
}
}  // namespace

const std::vector<DlcItem> &dlc_list(int g)
{
    DWORD now = GetTickCount();
    if (g_dlcAt[g] && now - g_dlcAt[g] < 3000) return g_dlc[g];
    g_dlcAt[g] = now ? now : 1;
    std::vector<DlcItem> out;
    std::string root = dlc_root(g), dir = join(root, "00000002");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((widen(dir) + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::string name = narrow(n);
            if (name.find(".partial") != std::string::npos || name.find(".old-") != std::string::npos) continue;
            DlcItem it;
            it.name = name;
            it.dir = join(dir, name);
            it.header = join(join(join(root, "Headers"), "00000002"), name + ".header");
            uint8_t h[0x108] = {};
            if (FILE *hf = _wfopen(widen(it.header).c_str(), L"rb")) {
                if (fread(h, 1, sizeof h, hf) == sizeof h) {
                    std::wstring w;
                    for (int i = 0; i < 127; i++) { wchar_t c = wchar_t((h[8 + 2 * i] << 8) | h[9 + 2 * i]); if (!c) break; w += c; }
                    it.title = narrow(w);
                }
                fclose(hf);
            }
            if (it.title.empty()) it.title = name;
            out.push_back(it);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    g_dlc[g] = out;
    return g_dlc[g];
}

static void dlc_refresh() { for (DWORD &t : g_dlcAt) t = 0; }

void dlc_remove_ask(App &a, int g, const DlcItem &item)
{
    g_dlcRemove = item;
    g_dlcRemoveGame = g;
    ask(a, "dlcremove", g, "Remove downloadable content",
        "Remove \"" + item.title + "\" from " + game_def(g).name + "? Only COGBox's unpacked copy is removed - your own package file is not touched.",
        { "Remove", "Cancel" });
}

void dlc_remove_confirmed(App &a)
{
    if (g_dlcRemoveGame < 0) return;
    if (a.run.proc) { set_message(a, "Close the game first.", true); return; }
    bool ok = remove_tree(widen(g_dlcRemove.dir));
    DeleteFileW(widen(g_dlcRemove.header).c_str());
    set_message(a, ok ? "\"" + g_dlcRemove.title + "\" is removed." : "It couldn't be removed completely - is a file from it open?", !ok);
    g_dlcRemoveGame = -1;
    dlc_refresh();
}

// ---- 0.2.0: folders of downloadable content dropped on the window ---------------------------------------------------
bool drop_is_game_or_update(const std::string &folder)
{
    std::filesystem::path res;
    return x360fs::detect(widen(folder), &res) != x360fs::SourceKind::Unknown;
}

std::vector<std::string> dlc_packages_in(const std::string &folder)
{
    std::vector<std::string> out;
    std::error_code ec;
    namespace fs = std::filesystem;
    for (auto it = fs::recursive_directory_iterator(widen(folder), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec) || it->file_size(ec) < 0xA000) continue;
        x360fs::PackageInfo pi;
        std::string err;
        if (x360fs::read_package_info(it->path(), pi, err) && pi.content_type == x360fs::kContentMarketplace && game_by_title_id(pi.title_id) >= 0)
            out.push_back(narrow(it->path().wstring()));
        if (out.size() >= 200) break;
    }
    std::sort(out.begin(), out.end());
    return out;
}
