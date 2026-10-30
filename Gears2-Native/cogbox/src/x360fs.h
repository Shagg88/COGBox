// COGBox - reading the player's own Xbox 360 game copies (x360fs)
//
// Turns what the player has into the two folders the ports read:
//   * the game folder: default.xex and the game partition's files, exactly as on the disc
//     (from an extracted folder, a disc image (.iso) or a Games on Demand copy), and
//   * the title update folder: default.xexp and the update's other files (from the update's STFS package).
//
// Formats (all from public descriptions; nothing here is Microsoft code):
//   XDVDFS  - the disc file system ("MICROSOFT*XBOX*MEDIA" at sector 32 of the game partition, little-endian,
//             2048-byte sectors, each directory a binary tree of entries).
//   STFS    - CON / LIVE / PIRS packages (title updates, DLC): big-endian header, 4096-byte blocks with
//             interleaved SHA-1 hash tables. Block numbering matches tools\stfs_extract.ps1.
//   SVOD    - Games on Demand: the same header file plus <name>.data\Data0000.. files holding an XDVDFS
//             file system with interleaved hash blocks.
//
// Standard C++17 only. Paths are std::filesystem::path (wide on Windows). Names read from the images are
// turned into safe single path components (no "..", no separators, no drive letters, no Windows device names).
// Error strings are plain English meant to be shown to players.
//
// Thread safety: every function opens its own files; calls on different threads are independent.
// Extraction writes straight into out_dir (created if missing); after a failure or a cancel, what was
// written so far is left there - extract into a fresh temporary folder and rename it when the call succeeds.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace x360fs {

namespace fs = std::filesystem;

// done/total in bytes of file data; current_file is a relative path with '/' separators (UTF-8).
using ProgressFn = std::function<void(uint64_t done, uint64_t total, const std::string &current_file)>;
// May be null. Set to true from any thread to stop an extraction (the call then fails with a "cancelled" error).
using CancelFlag = const std::atomic<bool> *;

enum class SourceKind {
    ExtractedFolder,   // a folder with default.xex in it
    DiscImage,         // a disc image with an XDVDFS file system (check disc_has_file(.., "default.xex"))
    GodContainer,      // Games on Demand: the SVOD header file (+ <name>.data\Data0000..)
    StfsPackage,       // a CON / LIVE / PIRS package (title updates: content type 0xB0000)
    Unknown
};
const char *source_kind_name(SourceKind k);

// What 'p' is. 'resolved' (optional) receives what to pass to the matching extract_* / reader:
//   ExtractedFolder - the folder (also when p is the default.xex file itself)
//   DiscImage       - the image file
//   GodContainer    - the SVOD header file (p may be that file, its <name>.data folder or a Data#### file in
//                     it, the 00007000 folder, or the <titleid> folder above it)
//   StfsPackage     - the package file (p may also be a folder holding title updates directly or in a
//                     000B0000 subfolder, e.g. <titleid> or <titleid>\000B0000: the newest update is taken)
SourceKind detect(const fs::path &p, fs::path *resolved = nullptr);

// ---- package headers (STFS and SVOD share them) ---------------------------------------------------------

constexpr uint32_t kContentSavedGame       = 0x00000001;
constexpr uint32_t kContentMarketplace     = 0x00000002;   // DLC
constexpr uint32_t kContentInstalledGame   = 0x00004000;
constexpr uint32_t kContentGamesOnDemand   = 0x00007000;
constexpr uint32_t kContentArcadeTitle     = 0x000D0000;
constexpr uint32_t kContentTitleUpdate     = 0x000B0000;   // "installer" content type: title updates

// Title IDs of the games COGBox knows, read from the players' own discs and title updates by COGBox --x360test (job 472,
// 2026-10-25): Gears of War 2 4D53082D, Gears of War 3 4D5308AB, Judgment 4D530A26. (4D5307D5 is the first Gears of War.)
constexpr uint32_t kTitleGears2        = 0x4D53082D;
constexpr uint32_t kTitleGears3        = 0x4D5308AB;
constexpr uint32_t kTitleGearsJudgment = 0x4D530A26;

struct PackageInfo {
    std::string magic;                 // "CON ", "LIVE" or "PIRS"
    uint32_t header_size = 0;          // 0x340
    uint32_t content_type = 0;         // 0x344
    uint32_t metadata_version = 0;     // 0x348
    uint64_t content_size = 0;         // 0x34C
    uint32_t media_id = 0;             // 0x354
    uint32_t version = 0;              // 0x358  (for a title update: the version it brings the game to)
    uint32_t base_version = 0;         // 0x35C  (for a title update: the version it applies to)
    uint32_t title_id = 0;             // 0x360
    uint8_t  platform = 0;             // 0x364
    uint8_t  executable_type = 0;      // 0x365
    uint8_t  disc_number = 0;          // 0x366
    uint8_t  disc_count = 0;           // 0x367
    uint32_t descriptor_type = 0;      // 0x3A9: 0 = STFS, 1 = SVOD
    uint32_t data_file_count = 0;      // 0x39D (SVOD: number of Data#### files)
    uint64_t data_files_size = 0;      // 0x3A1
    std::string display_name;          // 0x411, UTF-16BE in the header, UTF-8 here (first locale, English)
    std::string title_name;            // 0x1691, UTF-16BE in the header, UTF-8 here

    // STFS volume descriptor (0x379)
    uint8_t  block_separation = 0;     // bit 0 set: "female" (read-only) package, one hash table per level
    uint32_t file_table_blocks = 0;
    uint32_t file_table_start = 0;
    uint32_t allocated_blocks = 0;
    uint32_t unallocated_blocks = 0;

    // SVOD volume descriptor (0x379)
    uint8_t  svod_flags = 0;           // 0x40: "enhanced GDF" layout
    uint32_t svod_data_blocks = 0;
    uint32_t svod_data_block_offset = 0;

    bool is_svod() const { return descriptor_type == 1; }
    bool is_title_update() const { return content_type == kContentTitleUpdate; }
    // "major.minor.build.qfe" (4/4/16/8 bits, the XEX version layout) of 'version'
    std::string version_string() const;
};
std::string xbox_version_string(uint32_t v);

bool read_package_info(const fs::path &file, PackageInfo &info, std::string &error);

// ---- XEX (default.xex) execution info ------------------------------------------------------------------

struct XexInfo {
    uint32_t title_id = 0, media_id = 0, version = 0, base_version = 0;
    uint8_t disc_number = 0, disc_count = 0;
};
bool read_xex_info(const fs::path &xex, XexInfo &info);                       // a default.xex on disk
bool parse_xex_info(const uint8_t *data, size_t size, XexInfo &info);         // the first bytes of one

// ---- listings ---------------------------------------------------------------------------------------------

struct Entry {
    std::string path;   // relative, '/' separators, UTF-8, already made safe
    uint64_t size = 0;
    bool is_dir = false;
};

// ---- disc images (XDVDFS) ---------------------------------------------------------------------------------

// Byte offset of the game partition in the image (0, XGD2 0xFD90000, XGD3 0x2080000, XGD1 0x18300000), or -1.
int64_t disc_partition_offset(const fs::path &iso);
// 'name' may contain '/' or '\\' (a path in the image); compared without case, as the console does.
bool disc_has_file(const fs::path &iso, const std::string &name);
bool list_disc(const fs::path &iso, std::vector<Entry> &out, std::string &error);
bool disc_xex_info(const fs::path &iso, XexInfo &info);   // from the image's default.xex
bool extract_disc(const fs::path &iso, const fs::path &out_dir, ProgressFn progress, CancelFlag cancel,
                  std::string &error);

// ---- STFS packages (title updates, DLC) -------------------------------------------------------------------

// verify_hashes: check each block against its SHA-1 in the package's hash table (a block whose stored hash
// is all zero is not checked). The PowerShell extractor does not check; this only adds a damage check.
bool list_stfs(const fs::path &package, std::vector<Entry> &out, std::string &error);
bool extract_stfs(const fs::path &package, const fs::path &out_dir, ProgressFn progress, CancelFlag cancel,
                  std::string &error, bool verify_hashes = true);

// ---- Games on Demand (SVOD) -------------------------------------------------------------------------------

// 'header' is the SVOD header file (detect() finds it). Extracts the XDVDFS file system inside, which is the
// same set of files as the disc's game partition.
bool list_god(const fs::path &header, std::vector<Entry> &out, std::string &error);
bool extract_god(const fs::path &header, const fs::path &out_dir, ProgressFn progress, CancelFlag cancel,
                 std::string &error);

// ---- SHA-1 ------------------------------------------------------------------------------------------------

std::string sha1_hex(const void *data, size_t size);        // 40 lowercase hex digits
std::string sha1_file(const fs::path &file);                 // "" if the file cannot be read
std::string xex_code_sha1(const fs::path &file);             // job 647: SHA-1 from the XEX image offset on; "" if not an XEX

} // namespace x360fs
