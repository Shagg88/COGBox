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
// r22 review: a package takes part only when its scan is complete and plain - not a link / junction / reparse point
// itself, no link inside it, no read error, nothing skipped, under the file limit and no two file names that differ
// only in case (B1, B2, R3). The marker may compare large files by samples; nothing is removed before every file of
// both packages is compared by a SHA-1 of the whole file, and only the pairs the player was shown that are still
// marked the same way (verify(), R2 / R4).
//
// Standard C++17 (plus a few Windows calls under _WIN32 for links and file identity), so the logic is tested on its
// own (test/dlc_dupes_test.cpp).
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
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
    bool eligible = true;   // false: never marked and never a cover (see scan)
    std::string why;        // why not eligible (for the log / tests)
};

// True when package a's file 'fa' and package b's file 'fb' (the same path, the same size) hold the same bytes.
using SameFn = std::function<bool(int a, const File &fa, int b, const File &fb)>;

// For each package, the index of the package that covers it (see above), or -1. A package is only ever covered by one
// that is itself not covered, so removing every covered package keeps at least one copy of every file. Packages are
// considered largest first (then most files, then name), so a bundle covers the single packs inside it. A package
// that is not eligible is never covered and never covers another.
std::vector<int> find_covered(const std::vector<Package> &pk, const SameFn &same);

// The files under 'dir' (recursively; folders themselves are not listed). False if 'dir' can't be read. The package
// is marked not eligible when 'dir' is a link (symlink, junction, any reparse point), when anything inside it is a
// link, on any error while listing, when a file's size or name can't be read, past 'limit' files, or when two files'
// names differ only in case.
bool scan(const std::filesystem::path &dir, Package &out, size_t limit = 100000);

// A symlink, junction or other reparse point (or something whose attributes can't be read: true, to be safe).
bool is_link(const std::filesystem::path &p);

// 1 when both name the same file or folder on disk (the same volume and file index / device and inode), 0 when
// they don't, -1 when that can't be told.
int same_physical(const std::filesystem::path &a, const std::filesystem::path &b);

// What the marker compares two files by: SHA-1 of the whole file up to 64 MB; above that, of its size and three 1 MB
// samples (start, middle, end). "" when the file can't be read (such a file never matches). Never used alone for a
// removal: verify() hashes whole files.
std::string content_key(const std::filesystem::path &file, uint64_t size);

// content_key, remembered by (path as spelled on disk, size, last write time), so a re-check after a list does not
// read the files again. Thread-safe.
class KeyCache {
public:
    std::string key(const std::filesystem::path &file, uint64_t size);
    size_t reads() const { return reads_; }   // content_key calls made (for the test)
private:
    std::mutex m_;
    std::map<std::string, std::string> keys_;
    std::atomic<size_t> reads_{0};
};

// Scans 'dirs' (name -> folder) and marks them with find_covered and cached content keys. pk gets the scans.
std::vector<int> mark(const std::vector<std::pair<std::string, std::filesystem::path>> &dirs, KeyCache &kc, std::vector<Package> &pk);

struct Pair { std::string name, cover; };

// Progress for verify: bytes hashed so far, of the total, and the file; return false to cancel.
using ProgressFn = std::function<bool(uint64_t done, uint64_t total, const std::string &file)>;

struct Verified {
    std::vector<Pair> ok;              // may be removed: the duplicate (name), kept: cover
    std::vector<std::string> skipped;  // shown, but not (or no longer) an exact duplicate of the same package
    bool cancelled = false;
};

// Right before a removal: scans 'dirs' again and marks them, then keeps only the 'shown' pairs that are still marked
// with the same cover, whose two folders are not the same folder on disk, and where every file of the duplicate and
// the file of the same path in the cover give the same SHA-1 over the whole file (and are not the same file on
// disk). Finally the duplicate is listed again and must not have changed.
Verified verify(const std::vector<std::pair<std::string, std::filesystem::path>> &dirs, const std::vector<Pair> &shown, KeyCache &kc,
                const ProgressFn &progress = nullptr);

}  // namespace dlcdup
