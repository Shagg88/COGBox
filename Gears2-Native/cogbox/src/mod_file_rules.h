// COGBox - the one list of program files a mod may not bring (mod_file_rules.cpp). The Mods tab's "add a zip" and the
// Fabricator's installs leave these files out of the unpacked mod (with a note, not a refusal of the whole mod); the
// Fabricator's Creator check lists them as problems, because a clean package must not contain them.
//   exe dll xex xexp bat cmd ps1 psm1 vbs vbe js jse wsf wsh msi msp mst scr com lnk url pif hta reg asi sys cpl ocx jar
//   appx msix appxbundle msixbundle scf inf chm gadget application settingcontent-ms msc, and the nested archives
//   zip 7z rar cab (a program could hide inside one); since review r369 also ax drv efi mui ps1xml psd1 psc1 psc2
//   msh msh1 msh2 mshxml msh1xml msh2xml ws wsc sct vb appref-ms library-ms search-ms searchconnector-ms msu diagcab
//   appinstaller jnlp xll xbap website, and the archives / disk images tar gz tgz bz2 tbz2 xz txz lz lzma zst arj lzh
//   ace iso img vhd vhdx wim dmg; and (the security audit) Office files (macro and legacy forms), one, themes, rdp, Python,
//   htm html xhtml mht mhtml svg, Access databases, certificates, iqy slk, and the Linux launchers desktop sh.
#pragma once
#include <string>
#include <vector>

// The extensions above, lower case, without the dot.
const std::vector<std::string> &mod_file_program_exts();
// name (a file name or a path, '/' or '\' separated): its last extension, lower-cased, is in the list. Trailing dots and
// spaces are ignored first, as Windows drops them when it creates the file ("run.exe. " is run.exe).
bool mod_file_is_program(const std::string &name);
// component (one file or folder name, no separators) is a Windows device, not a file: its stem (trailing dots and
// spaces off, the part before the first dot, trailing spaces off; any case) is CON, PRN, AUX, NUL, CONIN$, CONOUT$,
// COM1-9 / LPT1-9 or COM / LPT with a superscript 1, 2 or 3. "con.txt" and "CON ." are devices; "console", "com10" not.
bool mod_file_is_device_name(const std::string &component);
