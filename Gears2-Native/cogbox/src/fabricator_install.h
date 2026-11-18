// COGBox - the Fabricator's installs in <Game>\mods: install, update and uninstall a mod from a zip
// (fabricator_install.cpp). Each returns "" on success, or what went wrong (no file paths in it).
#pragma once
#include <string>
#include <vector>

#include "fabricator_model.h"

struct App;

// An install split in three, so the long part can run on a worker thread:
//   fab_install_prepare (UI thread): the game-running guard, the folder chosen (mods_list and CogBox.ini are the UI
//     thread's), the game's data folder read; the plan says what the other two steps need, nothing else.
//   fab_install_unpack (any thread): fab_check_zip and the unpack into <mods>\<folder>.partial with its record. It touches
//     only that .partial folder: never CogBox.ini, App or another mod folder. Program files are left out of it;
//     left_out (may be null) gets their paths in the mod folder.
//   fab_install_commit (UI thread): the game-running guard again, the plan checked against the mods folder as it is now,
//     then the short steps - the installed version to the backup, the .partial moved into place, a new mod turned on
//     last in the order. "Close the game first." leaves the .partial for a later commit; any other failure removes it.
// Which folder is "this mod": the one whose record fab_same_install says is item (a "file" install: the same mod.ini
// Name). A folder without COGBox's record is never this mod, so a hand-made folder of the same name gets a new
// "<name> (Fabricator)" folder beside it and stays as it is.
struct FabInstallPlan {
    int g = -1;
    FabInstallItem item;
    std::string zip;
    std::string folder;     // under mods_dir(g)
    bool update = false;    // folder holds the installed version of item (its record); else a new folder
    std::string data_dir;   // the game's data folder (App::st[g].dataDir), for the unpack's top-folder rule
    bool strip_top = true;  // false: the zip was packed from the mod's own folder (a folder add), so its top folder is
                            // the mod's, never one to take off
};
std::string fab_install_prepare(App &a, int g, const FabInstallItem &item, const std::string &zip_path, FabInstallPlan *plan);
std::string fab_install_unpack(const FabInstallPlan &plan, std::vector<std::string> *left_out = nullptr);
std::string fab_install_commit(App &a, const FabInstallPlan &plan);
void fab_install_abandon(const FabInstallPlan &plan);   // removes the plan's .partial folder (a cancelled install)

// The three steps in a row on the calling thread (tests; the Mods tab's own zips use mods_add_zip). A mod already
// installed is updated: the new version is unpacked beside the installed one, the installed folder moves to
// mods\.fabricator-backup\<folder> (replacing an older backup of it), then the new one into place - same folder, so
// the same on/off state and position. A failure leaves the installed version as it was. fab_install and fab_update are
// the same call (each turns into the other as the mods folder says).
std::string fab_install(App &a, int g, const FabInstallItem &item, const std::string &zip_path, std::vector<std::string> *left_out = nullptr);
std::string fab_update(App &a, int g, const FabInstallItem &item, const std::string &zip_path, std::vector<std::string> *left_out = nullptr);
// <mods>\<name>.partial folders of game g that hold COGBox's record: Fabricator unpackings a closed or crashed COGBox
// never finished (the record is written before the unpack starts). A .partial without a record (the Mods tab's own
// unpacking) is left alone. Run at start, before any install.
void fab_sweep_partials(int g);
// Moves <mods>\<folder> to the backup folder. Only a folder with COGBox's fabricator.json is ever moved.
std::string fab_uninstall(App &a, int g, const std::string &folder);
