// COGBox - the one list of program files a mod may not bring (mod_file_rules.cpp). The Mods tab's "add a zip" and the
// Fabricator's installs leave these files out of the unpacked mod (with a note, not a refusal of the whole mod); the
// Fabricator's Creator check lists them as problems, because a clean package must not contain them.
//   exe dll xex xexp bat cmd ps1 psm1 vbs vbe js jse wsf wsh msi msp mst scr com lnk url pif hta reg asi sys cpl ocx jar
//   appx msix appxbundle msixbundle scf inf chm gadget application settingcontent-ms msc, and the nested archives
//   zip 7z rar cab (a program could hide inside one).
#pragma once
#include <string>
#include <vector>

// The extensions above, lower case, without the dot.
const std::vector<std::string> &mod_file_program_exts();
// name (a file name or a path, '/' or '\' separated): its last extension, lower-cased, is in the list. Trailing dots and
// spaces are ignored first, as Windows drops them when it creates the file ("run.exe. " is run.exe).
bool mod_file_is_program(const std::string &name);
