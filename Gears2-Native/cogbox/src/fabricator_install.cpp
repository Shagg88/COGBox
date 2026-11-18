// COGBox - the Fabricator's installs (see fabricator_install.h). Only <Game>\mods is touched: an installed mod is the folder
// <mods>\<folder> with its fabricator.json; the version before an update, or an uninstalled mod, is kept as
// <mods>\.fabricator-backup\<folder> (one per folder: a newer backup replaces it). A folder without a record is never
// moved or removed, and a failure part way leaves the old state or the new one, never a half folder: the zip is
// unpacked to <folder>.partial first and only a finished folder is moved into place.
#include "fabricator_install.h"
#include "app.h"

#include <windows.h>

static const char *kBackup = ".fabricator-backup";

static DWORD attrs_of(const std::string &p) { return GetFileAttributesW(widen(p).c_str()); }
static bool present(const std::string &p) { return attrs_of(p) != INVALID_FILE_ATTRIBUTES; }
static bool plain_dir(DWORD at)   // a folder, not a link (junction / symlink) to somewhere else
{
    return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) && !(at & FILE_ATTRIBUTE_REPARSE_POINT);
}
static std::string lower_s(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
static bool hidden_name(const std::string &n)   // what mods_list leaves out
{
    return n.empty() || n[0] == '.' || (n.size() >= 8 && lower_s(n.substr(n.size() - 8)) == ".partial");
}
static std::optional<FabInstallRecord> record_in(int g, const std::string &folder)
{
    const std::string p = join(mods_dir(g), folder);
    if (!plain_dir(attrs_of(p))) return std::nullopt;
    return fab_read_record(p);
}

static std::vector<std::string> mods_entries(int g)   // every name in the mods folder (folders and files)
{
    std::vector<std::string> v;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((widen(mods_dir(g)) + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return v;
    do {
        std::wstring n = fd.cFileName;
        if (n != L"." && n != L"..") v.push_back(narrow(n));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return v;
}

static std::string installed_folder(int g, const FabInstallItem &item)   // the Mods tab folder holding this mod's record ("" none)
{
    for (const std::string &n : mods_entries(g)) {
        if (hidden_name(n)) continue;
        auto r = record_in(g, n);
        if (r && fab_same_install(*r, item)) return n;
    }
    return "";
}

static bool item_ok(const FabInstallItem &it)   // an install COGBox can tell apart from others (fab_same_install)
{
    if (it.source.kind == "file") return !it.name.empty();
    if (it.source.kind == "nexus") return !it.source.game_domain.empty() && it.source.mod_id > 0 && !it.name.empty();
    return false;
}

static std::string check_zip(const FabInstallPlan &p)
{
    const std::vector<std::string> v = fab_check_zip(p.zip, game_def(p.g).key);
    if (v.empty()) return "";
    std::string s = "The mod can't be installed: " + v[0];
    for (size_t i = 1; i < v.size() && i < 3; i++) s += "; " + v[i];
    return s + ".";
}

// The backup name for <folder>: <folder> itself when that backup is free or is an older backup of this same mod, else
// "<folder> (2)", "(3)", ... - a backup of ANOTHER mod whose name came out as the same folder ("Foo!" and "Foo?" are both
// "Foo") is never replaced (review r369 M2). "" = no name left (100 taken).
static std::string backup_name(const std::string &root, const std::string &folder, const std::optional<FabInstallRecord> &cur)
{
    for (int i = 1; i <= 100; i++) {
        const std::string cand = i == 1 ? folder : folder + " (" + std::to_string(i) + ")";
        const std::string at = join(root, cand);
        const std::string old = join(root, ".old-" + cand);
        if (!present(at) && !present(old)) return cand;
        // taken: by this same mod's older backup (or, after a crash, only its .old-<cand>) -> that name, which to_backup
        // replaces or recovers; by another mod's, or by something COGBox didn't make -> the next name
        const std::string &have = present(at) ? at : old;
        if (!cur || !plain_dir(attrs_of(have)) || !dir_owned(have)) continue;   // audit F1: only COGBox's own is replaced
        const auto r = fab_read_record(have);
        if (r && fab_same_install(*r, FabInstallItem{cur->source, cur->name, cur->version, cur->sha256})) return cand;
    }
    return "";
}

// <mods>\<folder> (an installed mod's, with its record) to the backup folder, replacing an older backup of that same mod
static std::string to_backup(int g, const std::string &folder_in)
{
    const std::string root = join(mods_dir(g), kBackup);
    const std::string folder = backup_name(root, folder_in, fab_read_record(join(mods_dir(g), folder_in)));
    if (folder.empty()) return "The Fabricator's backup folder has 100 backups named \"" + folder_in + "\" - clear some first.";
    const std::string bak = join(root, folder);
    const DWORD ra = attrs_of(root);
    if (ra != INVALID_FILE_ATTRIBUTES && !plain_dir(ra)) return "The Fabricator's backup folder isn't a plain folder - nothing changed.";
    if (ra == INVALID_FILE_ATTRIBUTES && !make_dirs(root)) return "Couldn't make the Fabricator's backup folder (is the disk full?).";
    // an older backup (only one COGBox made: it has a record) is put aside as .old-<folder> until the move worked, and
    // put back if it didn't. The name starts with '.', which no mod folder can (fab_install's hidden-name guard), so it
    // is never another mod's backup.
    const std::string old = join(root, ".old-" + folder);
    if (present(old)) {
        if (!plain_dir(attrs_of(old)) || !fab_read_record(old) || !dir_owned(old))
            return "The Fabricator's backup folder has a \".old-" + folder + "\" COGBox didn't make - move it away first.";
        if (!present(bak)) {   // a crash after putting it aside, before the move: it is still the backup
            if (!MoveFileExW(widen(old).c_str(), widen(bak).c_str(), 0))
                return "Couldn't put the backup of \"" + folder + "\" back (is a file in it open?).";
        } else {               // a crash after the move: the superseded backup
            mods_remove_tree(widen(old));
            if (present(old)) return "Couldn't remove an old backup of \"" + folder + "\" (is a file in it open, or a link in it?) - remove the folder \".fabricator-backup\\.old-" + folder + "\" yourself.";
        }
    }
    const bool hadOld = present(bak);
    if (hadOld) {
        if (!plain_dir(attrs_of(bak)) || !fab_read_record(bak) || !dir_owned(bak))
            return "The Fabricator's backup folder has a \"" + folder + "\" COGBox didn't make - move it away first.";
        if (!MoveFileExW(widen(bak).c_str(), widen(old).c_str(), 0))
            return "Couldn't put the older backup of \"" + folder + "\" aside (is a file in it open?).";
    }
    if (!MoveFileExW(widen(join(mods_dir(g), folder_in)).c_str(), widen(bak).c_str(), 0)) {
        if (hadOld) MoveFileExW(widen(old).c_str(), widen(bak).c_str(), 0);
        return "Couldn't move \"" + folder_in + "\" to the backup folder (is a file in it open?).";
    }
    if (hadOld) mods_remove_tree(widen(old));
    return "";
}

std::string fab_install_prepare(App &a, int g, const FabInstallItem &item, const std::string &zip_path, FabInstallPlan *plan)
{
    if (a.run.proc || any_game_exe_running()) return "Close the game first.";   // the game reads the mods folder while it runs
    if (!item_ok(item)) return "This mod has no name COGBox can install it under.";
    FabInstallPlan p;
    p.g = g;
    p.item = item;
    p.zip = zip_path;
    p.data_dir = a.st[g].dataDir;
    p.folder = installed_folder(g, item);
    p.update = !p.folder.empty();
    if (!p.update) {
        std::string base = mod_folder_chars(item.name);   // the character rule first, then the hidden-name guard
        while (!base.empty() && (base[0] == '.' || base[0] == ' ')) base.erase(0, 1);   // a name starting with '.' is hidden
        if (hidden_name(base) && !base.empty()) base += " mod";                       // ... and so is "<name>.partial"
        // a taken name is reused only for this same install's folder (none here: installed_folder found none), so a
        // folder the player made, or another install's, gets " (Fabricator)" beside it
        p.folder = fab_folder_name(base, mods_entries(g), [&](const std::string &f) {
            auto r = record_in(g, f);
            return r && fab_same_install(*r, item);
        });
        if (present(join(mods_dir(g), p.folder)) || hidden_name(p.folder)) return "A mod folder named \"" + p.folder + "\" is already there.";
        if (!make_dirs(mods_dir(g))) return "Couldn't make the mods folder (is the disk full?).";
    }
    *plan = p;
    return "";
}

// <folder>.partial: the zip checked and unpacked, with its record; "" = ready to be moved into place
std::string fab_install_unpack(const FabInstallPlan &p, std::vector<std::string> *left_out)
{
    if (p.g < 0 || p.folder.empty() || hidden_name(p.folder)) return "The install wasn't prepared.";
    std::string e = check_zip(p);
    if (!e.empty()) return e;
    const std::string part = join(mods_dir(p.g), p.folder) + ".partial";
    mods_remove_tree(widen(part));   // what an earlier, failed try left (only a folder COGBox made: audit F1)
    if (present(part)) return "A folder named \"" + p.folder + ".partial\" is in the mods folder and COGBox didn't make it (or couldn't remove it) - move it away first.";
    std::string err;
    FabInstallRecord rec;   // COGBox's own: a fabricator.json in the zip is never unpacked (mods_unpack_zip_in)
    rec.source = p.item.source; rec.name = p.item.name; rec.version = p.item.version; rec.sha256 = p.item.sha256;
    // the record first: a .partial an unpack cut short (COGBox closed or crashed) is then COGBox's, and the next start's
    // sweep (fab_sweep_partials) removes it
    if (!make_dirs(part) || !dir_claim(part) || !fab_write_record(part, rec)) {   // dir_claim: COGBox's own (audit F1)
        mods_remove_tree(widen(part));
        return "Couldn't write the mod's record (is the disk full?).";
    }
    if (!mods_unpack_zip_in(p.data_dir, p.zip, part, p.strip_top, err, left_out)) {
        mods_remove_tree(widen(part));
        return err.empty() ? "The mod couldn't be unpacked." : err;
    }
    // review r369 F4: written again once the zip is unpacked, so no zip entry (another name for the same file) can
    // have replaced it, and checked
    const auto back = fab_write_record(part, rec) ? fab_read_record(part) : std::nullopt;
    if (!back || !fab_same_install(*back, p.item) || back->sha256 != rec.sha256 || back->version != rec.version ||
        !dir_record_contents(part)) {   // audit F1: the files COGBox made, with their identities
        mods_remove_tree(widen(part));
        return "Couldn't write the mod's record (is the disk full?).";
    }
    return "";
}

void fab_sweep_partials(int g)
{
    for (const std::string &n : mods_entries(g)) {
        if (n.size() <= 8 || lower_s(n.substr(n.size() - 8)) != ".partial") continue;
        const std::string p = join(mods_dir(g), n);
        if (plain_dir(attrs_of(p)) && dir_owned(p)) mods_remove_tree(widen(p));   // only COGBox's own (audit F1)
    }
}

void fab_install_abandon(const FabInstallPlan &p)
{
    if (p.g >= 0 && !p.folder.empty()) mods_remove_tree(widen(join(mods_dir(p.g), p.folder) + ".partial"));
}

std::string fab_install_commit(App &a, const FabInstallPlan &p)
{
    if (a.run.proc || any_game_exe_running()) return "Close the game first.";   // the .partial waits for the game to close
    const int g = p.g;
    const std::string dest = join(mods_dir(g), p.folder), part = dest + ".partial";
    auto drop = [&](const std::string &e) { mods_remove_tree(widen(part)); return e; };
    {   // the unpacked folder is this plan's (its record), and the mods folder is still as the plan saw it
        auto r = plain_dir(attrs_of(part)) ? fab_read_record(part) : std::nullopt;
        if (!r || !fab_same_install(*r, p.item)) return drop("The unpacked mod isn't there any more - add it again.");
    }
    const std::string now = installed_folder(g, p.item);
    if (now != (p.update ? p.folder : std::string())) return drop("The mods folder changed during the install - add the mod again.");
    if (!p.update) {
        if (present(dest)) return drop("A mod folder named \"" + p.folder + "\" is already there.");
        if (!MoveFileExW(widen(part).c_str(), widen(dest).c_str(), 0)) return drop("The unpacked mod couldn't be moved into place.");
        mods_turn_on_last(a, g, p.folder);
        return "";
    }
    const std::string e = to_backup(g, p.folder);
    if (!e.empty()) return drop(e);
    // the same folder name: CogBox.ini keeps its on/off state and its place in the order
    if (!MoveFileExW(widen(part).c_str(), widen(dest).c_str(), 0)) {
        const bool back = MoveFileExW(widen(join(join(mods_dir(g), kBackup), p.folder)).c_str(), widen(dest).c_str(), 0) != 0;
        mods_remove_tree(widen(part));
        return back ? "The new version couldn't be moved into place - the installed one is kept."
                    : "The new version couldn't be moved into place - the installed one is in mods\\.fabricator-backup.";
    }
    return "";
}

std::string fab_install(App &a, int g, const FabInstallItem &item, const std::string &zip_path, std::vector<std::string> *left_out)
{
    FabInstallPlan p;
    std::string e = fab_install_prepare(a, g, item, zip_path, &p);
    if (e.empty()) e = fab_install_unpack(p, left_out);
    if (e.empty()) e = fab_install_commit(a, p);
    if (e == "Close the game first.") fab_install_abandon(p);   // (a synchronous install doesn't wait)
    return e;
}

std::string fab_update(App &a, int g, const FabInstallItem &item, const std::string &zip_path, std::vector<std::string> *left_out)
{
    return fab_install(a, g, item, zip_path, left_out);
}

std::string fab_uninstall(App &a, int g, const std::string &folder)
{
    if (a.run.proc || any_game_exe_running()) return "Close the game first.";
    // one plain folder directly in mods (no separators, not hidden), holding COGBox's record
    if (folder.find_first_of("/\\:") != std::string::npos || hidden_name(folder) || !record_in(g, folder))
        return "This mod wasn't installed by the Fabricator - it is left as it is.";
    const std::string e = to_backup(g, folder);
    if (!e.empty()) return e;
    mods_store(a, g, mods_list(a, g));   // its name leaves the order and the on list
    return "";
}
