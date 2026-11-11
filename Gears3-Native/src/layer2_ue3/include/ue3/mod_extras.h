// Layer 2 (Unreal Engine 3): what --ue3_mods does beyond replacing and adding files (from KiltedKernel's Gears 3 port,
// its modcore rules; mod_extras.cpp). Our code (MIT).
//
// - mod.ini rules: a mod's optional mod.ini ([Mod] / [mod]; keys are case-blind) may also say
//     requires = Other Mod, Another     mods (by name or folder) that must be on and loaded, else this one is left out
//     game = gears3                      the game it is made for (gears3 if not given); a mod for another game is left out
//     enabled = false                    off without removing it from the list
//     content = MPCH_COGMarcusG2MP_SF    the game's own content packages its levels need (ModContentPackages)
//     side = everyone | host | client    who needs it in co-op / System Link (ModFingerprint compares "everyone" mods)
// - bare files: mods made for the Xbox 360 are often shared as the bare files; a package (.xxx/.upk), texture cache
//   (.tfc) or Coalesced_*.bin at a mod's top goes to the bundle folder (GearGame\CookedXbox360), a movie (.bik) to
//   GearGame\Movies.
// - config fragments: <mod>\config\*.ini are merged, in load order, into every Coalesced_*.bin (the game's, or a mod's
//   whole copy), so several mods can change the game's settings; the merged copies join the mods' files (their table of
//   contents lines and start-up hashes are then updated as for any replaced file). Fragment syntax: '##### <ini file>'
//   picks the packed file whose name ends with it, then [Section] and Key=v (replace), +Key=v (add), -Key=v (remove),
//   *Key=v (replace the value with the same first field, e.g. a menu entry "(Identifier=X,...)").
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ue3 {

// The --ue3_mods folders (';'-separated, load order), with their mod.ini rules applied: the folders to use, in order.
// Mods left out are logged with the reason.
std::vector<std::filesystem::path> ActiveModFolders(const std::string& spec);

// Where a file of a mod goes in the game, from its path inside the mod (lower case, '\\'): the same path for files in
// folders, the bundle or movie folder for bare Xbox 360 files at the top, "" for what the game never sees (mod.ini,
// read-me files, config\ fragments).
std::string ModGameKey(const std::string& rel_in_mod, const std::string& bundle_rel);

// Merges the active mods' config fragments into each Coalesced_*.bin of `bundle_rel` (the game's own, or the one a mod
// replaces it with, from `files`), writes the results to `out_dir` and points `files` at them. Returns how many
// bundles were written.
int MergeModConfigFragments(const std::filesystem::path& game_root, const std::string& bundle_rel,
                            const std::filesystem::path& out_dir, std::map<std::string, std::filesystem::path>& files);

// The largest SquadCatchUp an active mod's campaign asks for ([Campaign] SquadCatchUp = <distance>; 0: none).
int ModSquadCatchUp();

// The co-op players a chapter map takes: its campaign's [Campaign] MaxPlayers (any of the campaign's mods may give it;
// the smallest wins), 0 for a map no active campaign has, or a campaign that does not say.
int ModMaxPlayersForMap(const std::string& map);

// The game's own content packages the active mods name (mod.ini content = ...), in load order, without repeats.
const std::vector<std::string>& ModContentPackages();

// FNV-1a over the active "everyone" mods (name, version, files and sizes): two players with the same mods have the same
// value whatever their folders are called; 0 with none. For co-op / System Link, to refuse a player whose mods differ.
uint64_t ModFingerprint();

}  // namespace ue3
