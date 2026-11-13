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
bool import(const std::filesystem::path &source, uint32_t title_id, const std::filesystem::path &user_dir,
            const std::string &stamp, Result &r, const std::vector<std::filesystem::path> &own_dirs = {});

// The title-specific settings (id, bytes) of a GPD file's bytes. False if it is not an XDBF file.
bool gpd_title_settings(const uint8_t *d, size_t n, std::vector<std::pair<uint32_t, std::string>> &out);

}  // namespace save_import
