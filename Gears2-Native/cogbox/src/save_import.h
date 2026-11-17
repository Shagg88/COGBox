// COGBox 0.6.1 (Shagg 13 Nov): "Import saves" - a player's own saves from Xenia or from an Xbox 360 (a USB stick or
// hard-disk copy of the console's Content folder) into a game's COGBox user folder. Our code (MIT). No SDL, no Dear ImGui.
//
// What a save is, in all three places:
//   COGBox / Xenia Canary : <root>\<XUID>\<TITLE>\00000001\<save name>\<files>            (loose files)
//   older Xenia            : <root>\<TITLE>\00000001\<save name>\<files>
//   Xbox 360               : Content\<profile>\<TITLE>\00000001\<save name>               (one STFS package per save)
// and the per-profile game settings the Gears games keep their progress in (title-specific profile settings
// 63E83FFF / 63E83FFE / 63E83FFD):
//   COGBox / older Xenia   : <root>\<TITLE>\profile\<user>\63E83FFF                        (the setting's bytes)
//   Xenia Canary, Xbox 360 : <TITLE>.gpd (an XDBF file; on the console inside the profile's own STFS package,
//                            Content\<profile>\FFFE07D1\00010000\<profile>)
// Nothing is deleted: what the import replaces is moved to <user dir>\_saves-before-import-<stamp> first.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace save_import {

struct Result {
    int saves = 0;                      // save folders put in place
    int settings = 0;                   // profile setting files put in place
    int packages = 0;                   // Xbox 360 packages unpacked for them
    std::vector<std::string> names;     // the saves' names
    std::vector<std::string> notes;     // things the player should know (a package that could not be read, ...)
    std::filesystem::path backup_dir;   // where replaced files went ("" = nothing was replaced)
    std::string error;                  // set when import() returns false
};

// source: a folder to search (at most 8 levels deep), or one file (a save package, a profile package, a <TITLE>.gpd).
// title_id: the game's. user_dir: the game's COGBox user folder (game_user_dir). stamp: for the backup / work folder
// names (yyyymmdd-hhmmss). False (r.error says why) when nothing of this game was found or a move failed; then
// the user folder is as it was (moves are undone).
// own_dirs: COGBox's other user folders of this game (other launcher profiles) - never searched as a source.
// Saves go under the SDK's one profile (B13EBABEBABEBABE), whatever profile they came from.
// region_titles: other title IDs of the same game (regional discs) whose saves are imported too, under title_id.
bool import(const std::filesystem::path &source, uint32_t title_id, const std::filesystem::path &user_dir,
            const std::string &stamp, Result &r, const std::vector<std::filesystem::path> &own_dirs = {},
            const std::vector<uint32_t> &region_titles = {});

// 0.6.4 (Shagg 16 Nov: players copy their saves into COGBox's saves folder by hand, as they would for Xenia, and the
// game never sees them): what in a game's COGBox user folder the game cannot use as it is -
//   another profile's saves: <XUID>\<TITLE>\00000001 (XUID not COGBox's B13EBABEBABEBABE, not DLC's 0000000000000000),
//   older Xenia's layout: <TITLE>\00000001,  another user's game progress: <TITLE>\profile\<name other than User>,
//   Xbox 360 save / profile packages (files, not unpacked) at the folder's top or in COGBox's own 00000001 folder,
//   <TITLE>.gpd files (a Xenia Canary / console profile's game data) at the top or one folder down.
// (title_ids: the game's and its regional copies'.) Nothing is read beyond file names and the first 4 bytes.
std::vector<std::filesystem::path> find_misplaced(const std::filesystem::path &user_dir, const std::vector<uint32_t> &title_ids);
// Imports what find_misplaced finds: it is moved (same relative paths) into <user dir's parent>\_saves-found-<user dir
// name>-<stamp>, then imported from there as import() does; that folder stays, holding the originals. If the import
// fails, everything is moved back. False + r.error when nothing was found or the import failed.
bool import_misplaced(const std::filesystem::path &user_dir, uint32_t title_id, const std::string &stamp, Result &r,
                      const std::vector<std::filesystem::path> &own_dirs = {}, const std::vector<uint32_t> &region_titles = {});
// The title-specific settings (id, bytes) of a GPD file's bytes. False if it is not an XDBF file.
bool gpd_title_settings(const uint8_t *d, size_t n, std::vector<std::pair<uint32_t, std::string>> &out);

}  // namespace save_import
