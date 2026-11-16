// COGBox - the Fabricator (the mod store) as the launcher sees it: parsed server answers, install folder names,
// the per-install record and the local zip check (same rules as the server's checkModZip).
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct FabCard {
    int64_t id = 0;
    std::string name, author, game, type, short_desc, screen, version;
    double rating = 0;
    int ratings = 0, downloads = 0;
    int64_t updated_at = 0, size = 0;
};
struct FabVersion {
    int64_t id = 0;
    std::string version, notes, sha256;
    int64_t size = 0, created_at = 0;
};
struct FabPage : FabCard {
    std::string long_desc, side, state;
    std::vector<std::string> tags, screens;
    std::vector<std::pair<int64_t, std::string>> requires;
    std::vector<FabVersion> versions;
    std::vector<FabCard> others;
};
struct FabLibraryItem {
    int64_t id = 0, version_id = 0, size = 0;
    std::string name, game, type, state, version, sha256, screen;
};
struct FabInstallRecord {
    int64_t mod = 0, version = 0;
    std::string version_text, sha256;
};

// nullopt only for a body that isn't JSON or has the wrong top-level shape; missing / mistyped fields take defaults.
std::optional<std::vector<FabCard>> fab_parse_list(const std::string &body, int *total);
std::optional<FabPage> fab_parse_page(const std::string &body);
std::optional<std::vector<FabLibraryItem>> fab_parse_library(const std::string &body);   // {"mods":[...]} or a bare array

// A mod folder name made of letters, digits and " _-.()" (what mods_add_zip keeps), no trailing dots / spaces.
// Empty stays empty.
std::string mod_folder_chars(const std::string &base);

std::string fab_folder_name(const std::string &store_name, const std::vector<std::string> &existing_folders, int64_t mod_id,
                            const std::function<std::optional<FabInstallRecord>(const std::string &)> &record_of);

std::optional<FabInstallRecord> fab_read_record(const std::string &folder);       // <folder>\fabricator.json
bool fab_write_record(const std::string &folder, const FabInstallRecord &r);

// Problems with a mod zip for a game ("gears1".."judgment"); empty = fine.
std::vector<std::string> fab_check_zip(const std::string &zip_path, const std::string &game);
