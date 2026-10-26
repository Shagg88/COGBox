// Layer 2 (Unreal Engine 3 on the 360): the cooked config bundle, GearGame\CookedXenon\Coalesced_<lang>.bin - every
// .ini / .int the game reads, in one file - and edits to it, so engine settings ([SystemSettings] MotionBlur,
// DepthOfField, LensFlares, MaxAnisotropy, ...; camera FOVAngle values) can be changed the way the engine itself
// expects, without touching the game's files: the edited bundle is written under our user-data folder and the game's
// bundle folder is shown to it through an overlay in which those files are the edited copies (config_install.cpp,
// Ue3InstallConfigEditsIfRequested). notes/64.
//
// Format (read from the files themselves, job 253 notes): big-endian int32 file count; per file: FString name, int32
// section count; per section: FString name, int32 key count; per key: FString key, FString value. FString = big-endian
// int32 length then the characters: a negative length is UTF-16 (little-endian here) of -length chars, a positive
// one 8-bit chars; both include the terminating zero. Keys repeat for UE3 array entries (+Key in the source ini).
// Our code (MIT).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue3 {

struct ConfigString {
  std::u16string text;   // without the terminator
  bool wide = true;      // stored as UTF-16 (else 8-bit)
  bool present = true;   // false: length 0 in the file (no terminator either)
  std::string Utf8() const;
  static ConfigString FromUtf8(const std::string& s, bool wide);
};
struct ConfigKey {
  ConfigString key, value;
};
struct ConfigSection {
  ConfigString name;
  std::vector<ConfigKey> keys;
};
struct ConfigFile {
  ConfigString name;  // e.g. "..\GearGame\Config\Xe-GearEngine.ini"
  std::vector<ConfigSection> sections;
};
struct ConfigBundle {
  std::vector<ConfigFile> files;
};

bool ParseConfigBundle(const std::vector<uint8_t>& bytes, ConfigBundle& out, std::string* error = nullptr);
std::vector<uint8_t> WriteConfigBundle(const ConfigBundle& bundle);

// One edit: "<file>|<section>|<key>=<value>" sets the key (every occurrence; added at the end of the section, or a new
// section, when missing); "<file>|<section>|<key>*=<factor>" multiplies the number the value starts with (the rest,
// e.g. a trailing comment, is kept). <file> matches the end of the file name, case-insensitive (e.g.
// "Xe-GearEngine.ini"); <section> may be "*" (every section that has the key - nothing is added then).
// "<file>|<section>|-<key>=<value>" removes the entries of that key whose value is <value> (case-insensitive), the rest
// kept - one element of an array key, e.g. "-StartupMovies=EpicLogo" (Gears 3's form, its layer 2 d5bf473).
// "<file>|<section>|~=<old>><new>" (patch 0127) replaces the text <old> with <new> inside every value of the section,
// e.g. "Xe-GearEngine.ini|SystemSettings|~=MipFilter=point>MipFilter=linear" (trilinear for every TEXTUREGROUP) - the
// rest of each value is the game's own, so no game values are needed here. Nothing is added by either form.
// Several edits are separated by ';'.
struct ConfigEdit {
  std::string file, section, key, value;
  bool scale = false;
  double factor = 1.0;
  bool remove = false;                // patch 0127: "-key=value"
  bool replace = false;               // patch 0127: "~=old>new" (key empty)
  std::string replace_old, replace_new;
};
std::vector<ConfigEdit> ParseConfigEdits(const std::string& spec, std::string* error = nullptr);
// Applies the edits; returns how many values changed or were added. `log` (optional) gets one line per change.
int ApplyConfigEdits(ConfigBundle& bundle, const std::vector<ConfigEdit>& edits, std::vector<std::string>* log = nullptr);

// UE3 console builds check the files they load at start-up (the config bundles, startup packages) against a SHA-1
// table compiled into the executable (job 257: Gears 2 has 40 entries, "<lower-case file name>\0" + 20-byte hash; an
// edited bundle failing it brings up the dirty-disc error). The edits keep that check working: the edited file's own
// hash goes into the table in memory.
using Sha1Digest = std::array<uint8_t, 20>;
Sha1Digest Sha1(const uint8_t* data, size_t size);
// Offsets (into `data`) of the 20 hash bytes of each table entry naming `name` (matched exactly, as stored - lower
// case) whose hash is `hash`.
std::vector<size_t> FindFileHash(const uint8_t* data, size_t size, const std::string& name, const Sha1Digest& hash);

}  // namespace ue3

// --ue3_ini_edits: the edits above, applied to every Coalesced_*.bin in <game data root>\<--ue3_ini_bundle_dir>;
// the edited copies go to <user data root>\ini-edits and the game sees them in place of the originals (folder overlay).
// Off (nothing redirected) when no edit is set. Call after the runtime's file system is set up (OnPostSetup).
namespace rex { class Runtime; }
void Ue3InstallConfigEditsIfRequested(rex::Runtime* runtime);
