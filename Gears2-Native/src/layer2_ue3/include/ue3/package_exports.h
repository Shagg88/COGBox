// Layer 2 (Unreal Engine 3 on the 360): the objects a cooked package exports, by path (Outer.Name; a name's number
// as _N, the way the engine prints it), read from its name and export tables. GitHub #145: --ue3_voice_language
// (config_install.cpp, AddVoiceLanguage) gives a package its other language's twin only when the twin exports every
// object the package does - a twin without one (GearGame_LOC_ESM has no MarcusChatter_ImpactWoozyCough_Effort02_Bleep,
// which the English one has) leaves the game a null object, and Gears 2 faulted at start-up.
//
// Format (read from the files themselves; version 575, big-endian): the summary (tag, version, header size, FString
// folder, flags, name / export / import counts and offsets, depends offset, GUID, generations, engine and cooker
// versions, compression flags, chunk table {uncompressed offset, size, compressed offset, size}); the tables at their
// uncompressed offsets. Compression 4 (LZX): each chunk is {tag, block size, compressed total, uncompressed total,
// per block {compressed, uncompressed size}, the blocks}, each block an XMemCompress-framed LZX stream. Only the
// chunks below the depends offset (the end of the export table) are decoded. A name entry is an FString and 8 bytes of
// flags; an export entry is class, super, outer, name, number, archetype, flags (8 bytes), serial size and offset,
// export flags, NetObjectCount array, package GUID, package flags. Our code (MIT).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>

namespace ue3 {

// Reads `size` bytes at `offset` of the package into `dst`; false if they are not there.
using PackageRead = std::function<bool(uint64_t offset, size_t size, uint8_t* dst)>;

// The package's export paths into *out. False (and *out undefined): not a package this reads - a version outside
// 543-834 (Gears 2 575, Gears 3 828), a package compressed as a whole that doesn't decode, a compression other than none or LZX, a table that does not parse or decode.
bool ReadPackageExportPaths(const PackageRead& read, uint64_t file_size, std::set<std::string>* out);
bool ReadPackageExportPaths(const std::filesystem::path& file, std::set<std::string>* out);
// Shagg's to-do 2: "version N licensee L" (or "compressed as a whole, version N ...") of a package's summary, for the
// voice-language log when a table can't be read.
std::string DescribePackageSummary(const std::filesystem::path& file);

}  // namespace ue3
