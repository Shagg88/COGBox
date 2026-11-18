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

// The path rules fab_check_zip applies, shared with the Creator's folder check (fabricator_pack.h).
// name (a zip entry name or a relative path, '/' or '\'): it would escape the mod folder (absolute, drive letter or
// stream ':', NUL, a ".." component, also as Windows reads it with trailing dots and spaces dropped).
bool fab_path_unsafe(const std::string &name);
// The first segment of name that is a Windows device name (mod_file_rules.h), "" when none.
std::string fab_device_segment(const std::string &name);
// name as Windows compares it (one separator, no empty or "." segments, trailing dots and spaces off, case folded):
// two names with the same key cannot both exist in one folder.
std::string fab_windows_key(const std::string &name);
// "" or the problem text for a mod.ini Api value (-1 = none given) newer than modcore::kModApiLevel (modcore/campaigns.h: the level the game's modcore implements).
std::string fab_api_problem(long api);
// The mod limits shared by the zip check and the Creator's folder check: packed zip bytes, unpacked bytes, files, and
// the size of mod.ini (larger is "mod.ini is too large").
constexpr uint64_t kFabMaxPackedBytes = 4ull << 30, kFabMaxUnpackedBytes = 8ull << 30, kFabMaxFiles = 50000;
constexpr size_t kFabMaxIniBytes = 256 * 1024;
