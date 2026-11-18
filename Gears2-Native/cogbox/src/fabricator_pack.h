// COGBox - the Fabricator's Creator: a mod folder checked and packed into a clean zip (fabricator_pack.cpp), and the
// zip's name. Everything is local; the zip is made where the caller says.
#pragma once
#include "mod_ini_edit.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Progress of a pack, read by the UI thread: bytes read of the total, and a cancel flag the pack watches.
struct FabProgress {
    std::atomic<int64_t> done{ 0 }, total{ 0 };
    std::atomic<bool> cancel{ false };
};

// Packs the folder into zip_path (through a temporary file beside it, then a rename): every file except fabricator.json,
// '.'-named entries (.git, .fabricator-backup, ...), unfinished ".partial" folders, program files and nested archives
// (mod_file_rules.h). Links (junctions / symlinks) are refused. Limits: 50 000 files, 8 GiB unpacked, 4 GiB packed.
// "" when the zip is written; "cancelled"; else what went wrong, naming only paths inside the mod. A failed or cancelled
// pack leaves no zip_path of its own (an older file there stays). left_out (may be null) gets the paths in the folder
// ('\' separated) of the program files and nested archives left out (only those, not fabricator.json or '.'-names).
std::string fab_pack_folder(const std::string &folder, const std::string &zip_path, FabProgress &p,
                            std::vector<std::string> *left_out = nullptr);

// What a clean package must not have, for a mod made for game ("gears1".."judgment"); empty = clean. Program files and
// nested archives (by name), unsafe or Windows-device names, names that differ only in case, links, no mod.ini or one
// without Name / Game or for another game or with an Api newer than this COGBox, and over the pack's limits.
std::vector<std::string> fab_check_folder(const std::string &folder, const std::string &game);

// The package's file name: mod_folder_chars(name + "-" + version) + ".zip", or "mod.zip" if nothing is left.
std::string fab_package_name(const ModIniFields &f);

// The same with the file-count limit given (a test hook: the real limit is kFabMaxFiles).
std::string fab_pack_folder_limit(const std::string &folder, const std::string &zip_path, FabProgress &p, size_t max_files,
                                  std::vector<std::string> *left_out = nullptr);
std::vector<std::string> fab_check_folder_limit(const std::string &folder, const std::string &game, size_t max_files);
