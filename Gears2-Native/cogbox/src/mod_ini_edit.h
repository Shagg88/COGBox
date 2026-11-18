// COGBox - reading and editing a mod folder's mod.ini for the Fabricator's Creator form. The keys are the ones modcore
// reads (Gears3-Native/src/layer2_ue3/modcore/modcore.h): Name Game Version Author Description Side and Requires
// (the mods that must be on too, comma-separated; ModIniFields::needs). Nothing else in the file is touched.
#pragma once
#include <optional>
#include <string>
#include <vector>

struct ModIniFields {
    std::string name, game, version, author, description, side;
    std::vector<std::string> needs;   // the mod.ini key is "Requires" (modcore's spelling)
};

// <folder>\mod.ini's [Mod] keys (the keys before any section count too, as modcore reads them; keys case-insensitive,
// ';' / '#' comment lines, a UTF-8 BOM allowed). nullopt when the file can't be read.
std::optional<ModIniFields> mod_ini_read(const std::string &folder);

// Updates only Name Game Version Author Description Side and Requires in the [Mod] section of <folder>\mod.ini (a field
// left empty removes its line). Every other line, section, comment, the BOM and the line endings are kept; a new key is
// added after the section's last key; a missing [Mod] section is added at the top; a missing mod.ini is created. The
// write is atomic (a temporary file, then a rename). false when it could not be written.
bool mod_ini_write(const std::string &folder, const ModIniFields &f);
