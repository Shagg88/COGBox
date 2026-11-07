// CogBox 0.4.8: old program files left behind when a new release is unzipped over an old COGBox folder.
// The release's SHA256SUMS.txt (tools\package_cogbox.ps1) lists every .exe / .dll it ships. On the first start of a new
// version COGBox looks for .exe / .dll files that the list doesn't name and MOVES them (never deletes) to
// <COGBox>\_old-files\<from-version>\ with the same relative paths. A file is moved only when ALL of these hold:
//  - where: beside CogBox.exe (top level only), or under shared\ lang\ recipes\ prompts\ assets\ licenses\, or under
//    <Game>\port*\ for Gears1 Gears2 Gears3 Judgment. Nothing else is even looked at.
//  - not in a user-data folder (user game shadercache logs achievements covers mods saves extracted update _old-files ...)
//  - .exe or .dll, and not a DLL players add themselves (ReShade / Special K / DXVK names, NVIDIA Streamline sl.*.dll and
//    nvngx_*.dll, ...)
//  - not named in SHA256SUMS.txt (compared ignoring case: Windows names)
//  - its SHA-256 is one a past COGBox release shipped (stale_known.h, plus _old-files\shipped\*.txt saved by later
//    versions): a file COGBox never shipped - the player's own - is never moved, whatever its name.
// The rules are plain string logic so test/stale_files_test.cpp checks them on Linux against every past release.
#pragma once
#include <set>
#include <string>
#include <vector>

namespace cogbox {

struct ShippedList {
    std::set<std::string> files;    // lower case, '\' separated, relative to the COGBox folder
    std::set<std::string> hashes;   // lower-case hex of the listed files
};

// Parses SHA256SUMS.txt ("<64 hex>  <relative path>" per line). Returns false if nothing usable was found.
bool parse_shipped(const std::string &text, ShippedList &out);

// rel: a file's path relative to the COGBox folder ('\' or '/'). True when its place and name allow moving it (the first
// four rules above); 'why' says why not otherwise.
bool stale_path_ok(const std::string &rel, const ShippedList &now, std::string *why = nullptr);

// True when 'hex' is the SHA-256 of a program file some COGBox release shipped (built-in list or 'extra').
bool known_old_program(const std::string &hex, const std::set<std::string> &extra);

// The top folders whose trees are looked in (besides the top-level files and <Game>\port*\).
const std::vector<std::string> &stale_tree_tops();
const std::vector<std::string> &stale_game_dirs();

static const int kStaleMaxMoves = 200;   // more than this: something is wrong (wrong folder / list) - move nothing

}  // namespace cogbox

#ifdef _WIN32
struct App;
// Once per COGBox version (CogBox.ini [Cleanup] DoneVersion). Logs to logs\old-files.txt (and _old-files\<dir>\moved.txt);
// shows a one-line message when something was moved. 'prevVersion' names the folder ("" -> the date).
void stale_cleanup_startup(App &a, const std::string &prevVersion);
#endif
