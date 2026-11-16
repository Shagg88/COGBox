// COGBox - the Fabricator's installs in <Game>\mods: install, update and uninstall a store mod (fabricator_install.cpp).
// Each returns "" on success, or what went wrong (no file paths in it).
#pragma once
#include <cstdint>
#include <string>

#include "fabricator_model.h"

struct App;

// Checks the zip (fab_check_zip), picks the folder (fab_folder_name), unpacks it to <folder>.partial, writes the record
// (fabricator.json), moves it into place and turns the mod on, last in the order. A mod already installed from the
// store is updated instead (fab_update).
std::string fab_install(App &a, int g, const FabLibraryItem &item, const std::string &zip_path);
// Unpacks the new version beside the installed one, moves the installed folder to mods\.fabricator-backup\<folder>
// (replacing an older backup of it), then the new one into place: same folder, so the same on/off state and position.
// A failure leaves the installed version as it was.
std::string fab_update(App &a, int g, const FabLibraryItem &item, const std::string &zip_path);
// Moves the store mod's folder to the backup folder. Only a folder with this mod's fabricator.json is ever moved.
std::string fab_uninstall(App &a, int g, int64_t mod_id);
