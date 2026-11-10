// COGBox - Linux and Steam Deck through Proton / Wine (proton049, 0.4.9). COGBox stays a Windows program; this only
// finds out where it runs and makes Linux paths usable:
//   - detect(): Wine (ntdll's wine_get_version), Proton (Steam's STEAM_COMPAT_* variables, the Proton tool's version
//     file), SteamOS (SteamOS=1, /etc/os-release) and the Steam Deck (SteamDeck=1, the Valve "Jupiter" (LCD) / "Galileo"
//     (OLED) DMI names, the Deck's "AMD Custom GPU 0405" / "0932"), read through Wine's Z: drive (= the Linux "/").
//   - Linux paths: "/home/deck/Games" <-> "Z:\home\deck\Games", "~/...", file:// URLs, pasted quotes.
//   - The places worth looking in for game copies (~/Games, ~/.local/share, SD cards under /run/media, Steam libraries)
//     and a bounded folder walk over them.
// Everything except detect_here() / roots_here() is plain C++ (no Windows headers) so test/proton_test.cpp runs it on
// Linux.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace proton {

struct Info {
    bool wine = false;          // running under Wine (any: Proton, plain Wine, CrossOver)
    bool proton = false;        // ... started by Steam's Proton (STEAM_COMPAT_* set, or a Proton Wine build)
    bool steamOS = false;       // SteamOS (SteamOS=1 or os-release ID=steamos)
    bool steamDeck = false;     // a Steam Deck (SteamDeck=1, Valve DMI, or the Deck's GPU)
    bool deckOled = false;      // ... the OLED model (Galileo / GPU 0932)
    bool deckLcd = false;       // ... the LCD model (Jupiter / GPU 0405)
    bool gamescope = false;     // inside gamescope (the Deck's Game Mode)
    bool zDrive = false;        // Z: is the Linux root (Wine's default)
    std::string wineVersion;    // wine_get_version ("9.0", "11.0")
    std::string wineBuild;      // wine_get_build_id ("wine-9.0 (Proton-9.0-4)" on some builds)
    std::string hostSystem, hostRelease;   // wine_get_host_version ("Linux", "6.5.0-valve22-1-neptune")
    std::string protonVersion;  // the Proton tool's version file ("proton-11.0-2", "GE-Proton11-12"), else its folder name
    std::string distro;         // os-release PRETTY_NAME ("SteamOS", "Bazzite", "Ubuntu 24.04 LTS")
    std::string deckModel;      // "Steam Deck OLED" / "Steam Deck LCD" / "Steam Deck" / ""
    std::string home, user;     // the Linux home folder and user name (HOME / USER), for the paths below
};

struct Inputs {
    std::function<std::string(const char *)> env;                             // "" when unset
    std::function<bool(const std::string &unixPath, std::string &out)> readUnix; // a small Linux file (through Z:)
    std::string wineVersion, wineBuild, hostSystem, hostRelease;
    bool zDrive = false;
    std::vector<std::string> gpus;                                            // the graphics adapters' names
};

Info detect(const Inputs &in);
// One line for the launcher and the report: "Proton 11.0-2 (Wine 11.0), Steam Deck OLED, SteamOS" (empty: not Wine)
std::string summary(const Info &i);
// The report's lines (every field that is known)
std::vector<std::string> report_lines(const Info &i);

// ---- paths ----------------------------------------------------------------------------------------------------------
// A pasted path made plain: spaces / newlines and surrounding quotes trimmed, file:// URLs decoded (%20 -> space)
std::string clean_pasted(const std::string &s);
bool looks_unix(const std::string &s);   // "/...", "~", "~/..." (after clean_pasted)
// A Linux path as Wine sees it: "/home/deck/Games" -> "Z:\home\deck\Games", "~/x" -> home + "/x". A Windows path
// ("C:\...", "Z:/...") is returned cleaned (slashes turned). "" when it is neither.
std::string to_wine(const std::string &input, const std::string &home);
// "Z:\home\deck\x" (or "\\?\unix\home\deck\x") -> "/home/deck/x"; "" when it isn't on the Linux side
std::string to_unix(const std::string &winPath);
// What the launcher shows: under Wine a Z: path as its Linux path, otherwise unchanged
std::string display_path(const std::string &path, bool wine);

// ---- where to look ----------------------------------------------------------------------------------------------------
// Steam's libraryfolders.vdf: every library's "path"
std::vector<std::string> parse_libraryfolders(const std::string &vdf);
// The Proton tool's version file ("1752000000 proton-11.0-2") -> "proton-11.0-2"
std::string version_from_file(const std::string &text);
// STEAM_COMPAT_TOOL_PATHS ("/a/Proton - Experimental:/b/runtime") -> the Proton folder ("/a/Proton - Experimental")
std::string proton_dir(const std::string &toolPaths);
// The Linux folders to look in, most likely first, without duplicates: ~/Games, ~/games, ~/Downloads, ~/Desktop,
// each Steam library's steamapps/common, ~/.local/share, SD cards and USB drives (/run/media/<user>/*, /run/media/*),
// /media/<user>/*, /mnt/*. mediaDirs = what is in /run/media (and below <user>) etc., found by the caller.
std::vector<std::string> scan_roots(const std::string &home, const std::string &user, const std::vector<std::string> &steamLibs,
                                    const std::vector<std::string> &mediaDirs);

// A bounded walk: each root to maxDepth levels, skipping caches, Proton prefixes, hidden folders below the root and
// what isGame says is a copy (it is not entered). At most 'budget' folders visited in all, and it stops after 'seconds'
// (a slow network or USB drive). isGame(path, isDir).
std::vector<std::filesystem::path> find_copies(const std::vector<std::filesystem::path> &roots,
                                               const std::function<bool(const std::filesystem::path &, bool)> &isGame,
                                               int maxDepth = 3, int budget = 6000, double seconds = 30);

// A cheap first look before the real check (x360fs::detect reads every file's header): a folder with default.xex, a
// Games on Demand / title update folder (00007000 / 000B0000, here or one level down), or a package file (a name of 40+
// hex digits, no extension); a file: .iso, or such a package. Only these are then checked properly.
bool quick_candidate(const std::filesystem::path &p, bool isDir);

#ifdef _WIN32
// This process: ntdll's wine_* functions, the environment and the Linux files through Z:
Info detect_here(const std::vector<std::string> &gpus);
// scan_roots() for this PC as Windows paths that exist (Z:\...)
std::vector<std::string> roots_here(const Info &i);
#endif

}  // namespace proton
