// COGBox - downloadable content that another installed package already covers (GitHub #283).
//
// Packages of the same DLC circulate more than once: the same skin or character in a package of its own and inside a
// bundle, a re-release under a new content ID, the same pack from two regions. Mounted together, the game gets the
// same items twice (Gears 3: the same skin twice in the character / weapon select; also on Xenia), and every extra
// package makes the start-up DLC loop longer (#283: 31 packages installed where 11 are different).
//
// The rule uses only what COGBox unpacked, never the display name: package A is covered by package B when every file
// in A is also in B under the same relative path (compared without case, as the console does) with the same size and
// the same contents. B then gives the game everything A does, so A can go. Identical packages cover each other; one
// of them (the first by name) is kept. A package with no files is never marked. Nothing is decided on titles: two
// packages with the same name but different files are different (e.g. an older and a newer version of a pack).
//
// Standard C++17 only (no Windows calls), so the logic is tested on its own (test/dlc_dupes_test.cpp).
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dlcdup {

struct File {
    std::string path;    // relative to the package folder, '/' separators, ASCII lower case (what is compared)
    uint64_t size = 0;
    std::string disk;    // the same path as it is spelled on disk (what is opened); empty = 'path'
};

struct Package {
    std::string name;    // the folder name (00000002\<name>)
    std::vector<File> files;
    uint64_t bytes = 0;  // the files' sizes added up
};

// True when package a's file 'fa' and package b's file 'fb' (the same path, the same size) hold the same bytes.
using SameFn = std::function<bool(int a, const File &fa, int b, const File &fb)>;

// For each package, the index of the package that covers it (see above), or -1. A package is only ever covered by one
// that is itself not covered, so removing every covered package keeps at least one copy of every file. Packages are
// considered largest first (then most files, then name), so a bundle covers the single packs inside it.
std::vector<int> find_covered(const std::vector<Package> &pk, const SameFn &same);

// The files under 'dir' (recursively; folders themselves are not listed). False if 'dir' can't be read.
bool scan(const std::filesystem::path &dir, Package &out);

// What two files are compared by: SHA-1 of the whole file up to 64 MB; above that, of its size and three 1 MB samples
// (start, middle, end) - cooked UE3 packages of the same name and size that differ only outside those samples are not
// expected. "" when the file can't be read (such a file never matches).
std::string content_key(const std::filesystem::path &file, uint64_t size);

}  // namespace dlcdup
