// COGBox - the Fabricator's installs (see fabricator_install.h). Only <Game>\mods is touched: a store mod is the folder
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

static std::string installed_folder(int g, int64_t mod_id)   // the Mods tab folder holding this mod's record ("" none)
{
    for (const std::string &n : mods_entries(g)) {
        if (hidden_name(n)) continue;
        auto r = record_in(g, n);
        if (r && r->mod == mod_id) return n;
    }
    return "";
}

static std::string check(int g, const FabLibraryItem &item, const std::string &zip)
{
    const std::string key = game_def(g).key;
    if (item.id <= 0) return "This isn't a Fabricator mod.";
    if (!item.game.empty() && item.game != key) return "This mod is for another game.";
    const std::vector<std::string> p = fab_check_zip(zip, key);
    if (p.empty()) return "";
    std::string s = "The mod can't be installed: " + p[0];
    for (size_t i = 1; i < p.size() && i < 3; i++) s += "; " + p[i];
    return s + ".";
}

// <dest>.partial: the zip unpacked, with its record; "" = ready to be moved into place
static std::string unpack_beside(App &a, int g, const FabLibraryItem &item, const std::string &zip, const std::string &dest)
{
    const std::string part = dest + ".partial";
    mods_remove_tree(widen(part));   // what an earlier, failed try left
    if (present(part)) return "An unfinished install of this mod is still there and couldn't be removed.";
    std::string err;
    FabInstallRecord rec;
    rec.mod = item.id; rec.version = item.version_id; rec.version_text = item.version; rec.sha256 = item.sha256;
    if (!mods_unpack_zip(a, g, zip, part, true, err) || !fab_write_record(part, rec)) {
        mods_remove_tree(widen(part));
        return err.empty() ? "Couldn't write the mod's record (is the disk full?)." : err;
    }
    return "";
}

// <mods>\<folder> (a store mod's, with its record) to the backup folder, replacing an older backup of that folder
static std::string to_backup(int g, const std::string &folder)
{
    const std::string root = join(mods_dir(g), kBackup), bak = join(root, folder);
    const DWORD ra = attrs_of(root);
    if (ra != INVALID_FILE_ATTRIBUTES && !plain_dir(ra)) return "The Fabricator's backup folder isn't a plain folder - nothing changed.";
    if (ra == INVALID_FILE_ATTRIBUTES && !make_dirs(root)) return "Couldn't make the Fabricator's backup folder (is the disk full?).";
    if (present(bak)) {
        // only a backup COGBox made (it has a record) is replaced
        if (!plain_dir(attrs_of(bak)) || !fab_read_record(bak))
            return "The Fabricator's backup folder has a \"" + folder + "\" COGBox didn't make - move it away first.";
        mods_remove_tree(widen(bak));
        if (present(bak)) return "Couldn't remove the older backup of \"" + folder + "\" (is a file in it open?).";
    }
    if (!MoveFileExW(widen(join(mods_dir(g), folder)).c_str(), widen(bak).c_str(), 0))
        return "Couldn't move \"" + folder + "\" to the backup folder (is a file in it open?).";
    return "";
}

std::string fab_install(App &a, int g, const FabLibraryItem &item, const std::string &zip_path)
{
    std::string e = check(g, item, zip_path);
    if (!e.empty()) return e;
    if (!installed_folder(g, item.id).empty()) return fab_update(a, g, item, zip_path);
    std::string base = item.name;
    while (!base.empty() && (base[0] == '.' || base[0] == ' ')) base.erase(0, 1);   // a name starting with '.' is hidden
    if (hidden_name(base) && !base.empty()) base += " mod";                       // ... and so is "<name>.partial"
    const std::string folder = fab_folder_name(base, mods_entries(g), item.id, [&](const std::string &f) { return record_in(g, f); });
    const std::string dest = join(mods_dir(g), folder);
    if (present(dest) || hidden_name(folder)) return "A mod folder named \"" + folder + "\" is already there.";
    if (!make_dirs(mods_dir(g))) return "Couldn't make the mods folder (is the disk full?).";
    e = unpack_beside(a, g, item, zip_path, dest);
    if (!e.empty()) return e;
    if (!MoveFileExW(widen(dest + ".partial").c_str(), widen(dest).c_str(), 0)) {
        mods_remove_tree(widen(dest + ".partial"));
        return "The unpacked mod couldn't be moved into place.";
    }
    mods_turn_on_last(a, g, folder);
    return "";
}

std::string fab_update(App &a, int g, const FabLibraryItem &item, const std::string &zip_path)
{
    std::string e = check(g, item, zip_path);
    if (!e.empty()) return e;
    const std::string folder = installed_folder(g, item.id);
    if (folder.empty()) return fab_install(a, g, item, zip_path);   // not there (any more): a new install
    const std::string dest = join(mods_dir(g), folder), part = dest + ".partial";
    e = unpack_beside(a, g, item, zip_path, dest);
    if (!e.empty()) return e;
    e = to_backup(g, folder);
    if (!e.empty()) { mods_remove_tree(widen(part)); return e; }
    // the same folder name: CogBox.ini keeps its on/off state and its place in the order
    if (!MoveFileExW(widen(part).c_str(), widen(dest).c_str(), 0)) {
        const bool back = MoveFileExW(widen(join(join(mods_dir(g), kBackup), folder)).c_str(), widen(dest).c_str(), 0) != 0;
        mods_remove_tree(widen(part));
        return back ? "The new version couldn't be moved into place - the installed one is kept."
                    : "The new version couldn't be moved into place - the installed one is in mods\\.fabricator-backup.";
    }
    return "";
}

std::string fab_uninstall(App &a, int g, int64_t mod_id)
{
    const std::string folder = installed_folder(g, mod_id);
    if (folder.empty()) return "This mod isn't installed from the Fabricator.";
    const std::string e = to_backup(g, folder);
    if (!e.empty()) return e;
    mods_store(a, g, mods_list(a, g));   // its name leaves the order and the on list
    return "";
}
