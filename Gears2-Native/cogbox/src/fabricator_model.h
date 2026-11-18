// COGBox - the Fabricator (the mod manager) model: install folder names, the per-install record and which install it
// is, and the local zip check. Everything here is local: nothing is fetched or sent.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Where an install came from. "file": a zip the player gave COGBox (its identity is the mod.ini Name, without case,
// within the game). "nexus": kept for a record that names a Nexus Mods file (game_domain + mod_id; file_id is the
// file). Other kinds are not COGBox's.
struct FabSource {
    std::string kind = "file";
    std::string game_domain;
    int64_t mod_id = 0, file_id = 0;
};
// <mod folder>\fabricator.json, written only by COGBox (never taken from a zip). name = the mod.ini Name, or the
// zip / folder name if there is none.
struct FabInstallRecord {
    FabSource source;
    std::string name, version, sha256;
};
// What is being installed (the name from fab_check_zip's FabZipInfo).
struct FabInstallItem {
    FabSource source;
    std::string name, version, sha256;
};

// A mod folder name made of letters, digits and " _-.()" (what mods_add_zip keeps), no trailing dots / spaces.
// Empty stays empty.
std::string mod_folder_chars(const std::string &base);

// The folder for an install named name: its characters by mod_folder_chars ("mod" if none are left), a Windows device
// name made safe ("CON" -> "CON mod"), then the first of name, "name (Fabricator)", "name (Fabricator 2)", ... that is
// not in existing_folders (compared without case). A taken one is reused only when is_same_install(folder) says it
// holds this same install (an update); a folder of that name the player made is never reused.
std::string fab_folder_name(const std::string &name, const std::vector<std::string> &existing_folders,
                            const std::function<bool(const std::string &folder)> &is_same_install);

// The record is of this install: "nexus" = the same game_domain and mod_id; "file" = the same kind and the same
// name, without case.
bool fab_same_install(const FabInstallRecord &r, const FabInstallItem &it);

// <folder>\fabricator.json: {"source":{"kind","game_domain","mod_id","file_id"},"name","version","sha256"}.
// nullopt when it is missing, not JSON, or not COGBox's (no source object, an unknown kind, a "file" record without a
// name, a "nexus" one without its game domain and mod id); mistyped fields take defaults.
std::optional<FabInstallRecord> fab_read_record(const std::string &folder);
bool fab_write_record(const std::string &folder, const FabInstallRecord &r);

// What a mod zip says about itself: its mod.ini [Mod] keys (the mod.ini at the top, or in a single top folder). name =
// mod.ini Name, else the zip's file name without its extension; game = mod.ini Game, else "gears3" (modcore's rule).
struct FabZipInfo {
    std::string name, version, author, game, type, description;
};
// Problems with a mod zip for a game ("gears1".."judgment") that refuse the install; empty = fine. Unsafe paths, NUL
// and device names, names Windows would merge (case, trailing dots), links, encryption, the size and file-count limits,
// mod.ini for another game or a newer Api. Program files (mod_file_rules.h) are not a refusal here: the unpack leaves
// them out and reports them (mods_unpack_zip_in's left_out). info (may be null) is filled as far as the zip was read.
std::vector<std::string> fab_check_zip(const std::string &zip_path, const std::string &game, FabZipInfo *info = nullptr);
