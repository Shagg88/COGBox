// COGBox - mod conflicts (KiltedKernel): which enabled mods change the same game file, which one wins, and which files.
// The rules are the game's own, so the list says what the game will do:
//   - files: --ue3_mods (layer2_ue3 config_install.cpp CollectMods, mod_extras.cpp ModGameKey, modcore.h GamePath) puts
//     each mod's files over the game's by their path inside the mod (case-blind: lower case, '\'), mods in load order,
//     out[path] = file - so the LAST mod with a path wins. Not game files, never in a conflict: Gears 2 / 3's
//     config\ folder (fragments, MERGED in load order into every Coalesced_*.bin - several mods can change the settings; Gears 1 /
//     Judgment have no fragments: their config\ is a folder like any, which the game hasn't), files at the mod's top
//     (mod.ini, read-me, fabricator.json) except bare Xbox 360 files (Gears 2 / 3: a .xxx / .upk / .tfc / Coalesced_*.bin
//     goes to the cooked folder, a .bik to GearGame\Movies; Gears 1 / Judgment: none), a mod's own table of contents
//     (<top folder>\*toc*.txt - the game's is rebuilt), and folders the game does not have (the overlay skips those).
//     Programs: the runtime skips only .xex / .xexp / .dll / .exe; here the launcher's whole list (mod_file_rules.h) is
//     left out, as adding a mod already leaves those files out (a hand-added .bat would be in the overlay, unused).
//   - textures: a texture pack's DDS files are matched by the texture hash at the start of the name (`<hash16>_....dds`,
//     layer0_xenos renderer/texture_pack.cpp HashFromName / Walk: not in dump, .build or replace_* folders, at most 6
//     folders deep). COGBox passes --gow2_texture_pack_dirs with the last-loaded pack first and the renderer keeps the
//     first file of a hash, so the LAST pack in load order wins - two differently named files of one hash conflict.
// mod_conflicts() is the pure part (test/mod_conflicts_test.cpp); the listing of the mods' files runs on a worker of its
// own (mod_conflicts_scan.cpp) and the screens read the latest result (mod_conflicts_get).
#pragma once
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct ModConflictRules {
    std::string cooked;               // a bare package's folder: "cookedxenon" (Gears 2), "cookedxbox360" (Gears 3), "" none
    std::set<std::string> game_tops;  // the game's top-level folders, lower case; empty = not known (none left out)
    bool fragments = true;            // config\ is the mod's fragments (Gears 2 / 3); false: a folder like any (Gears 1 / J)
    size_t max_names = 10;            // file names kept per pair
};

// One enabled mod, in load order: its files' paths inside its folder ('\' or '/'), in files or (when set, so a cached
// listing is not copied) *ref. overlay: its files go to the game (false: a mod the game leaves out - its texture pack
// is still used).
struct ModFileList {
    std::string name;
    std::vector<std::string> files;
    const std::vector<std::string> *ref = nullptr;
    bool overlay = true;
    const std::vector<std::string> &list() const { return ref ? *ref : files; }
};

// A pair of mods: `other` (its index and name), how many files / textures, and the first max_names of them (this mod's
// own paths, sorted).
struct ModConflictPair {
    size_t other = 0;
    std::string other_name;
    uint32_t count = 0;
    std::vector<std::string> names;
};
struct ModConflicts {
    std::vector<ModConflictPair> replaces;      // files of earlier mods this one replaces (it wins)
    std::vector<ModConflictPair> replaced_by;   // its files a later mod replaces
    std::vector<ModConflictPair> tex_wins;      // texture hashes it shares with earlier packs (it wins)
    std::vector<ModConflictPair> tex_loses;     // ... with later packs (they win)
    bool any() const { return !replaces.empty() || !replaced_by.empty() || !tex_wins.empty() || !tex_loses.empty(); }
};

// Where rel (a path inside a mod) goes in the game (lower case, '\'), "" when the game never takes it from the mod.
std::string mod_conflict_game_path(const std::string &rel, const ModConflictRules &r);
// rel is a pack texture the renderer reads: its hash.
bool mod_conflict_texture_hash(const std::string &rel, uint64_t &hash);
// Per mod (the same order as mods): what it replaces and loses, files and textures.
std::vector<ModConflicts> mod_conflicts(const std::vector<ModFileList> &mods, const ModConflictRules &r);

// ---- mod_conflicts_scan.cpp: the screens' side (UI thread) ----
struct ModItem;
// From mods_list: game g's mods (load order) - a listing and a new result are made on the worker when the enabled set or
// order changed, and every few seconds a check of the folders (a folder whose listing is older than its signature -
// its own and its subfolders' times, two levels down - is listed again). dataDir: the game's data folder (its top
// folders). Never waits.
void mod_conflicts_request(int g, const std::vector<ModItem> &mods, const std::string &dataDir);
// The latest result for game g's enabled mods as they are now (null: not worked out yet for this order).
struct ModConflictSet {
    std::string key;                    // the enabled set and order it is for
    std::vector<std::string> folders;   // lower-case folder per entry of `of`
    std::vector<ModConflicts> of;
    const ModConflicts *find(const std::string &folder) const;
};
std::shared_ptr<const ModConflictSet> mod_conflicts_get(int g);
// Every enabled folder of game g listed again, signatures or not (the LIBRARY or the Mods tab opened on it, the game
// closed); the result shown meanwhile stays.
void mod_conflicts_refresh(int g);
// At exit: the worker stops between two mods (its state is never freed, so it may still be finishing one).
void mod_conflicts_shutdown();
// A pair's line for the screens (translated). kind: 0 replaced_by, 1 replaces, 2 tex_loses, 3 tex_wins. Its tip: which
// copy the game uses, then the paths and "and N more" (file names: draw with TextUnformatted, never as a format).
std::string mod_conflict_text(const ModConflictPair &p, int kind);
std::vector<std::string> mod_conflict_tip(const ModConflictPair &p, int kind);
