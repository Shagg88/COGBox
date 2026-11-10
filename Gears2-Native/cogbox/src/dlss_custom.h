// CogBox 0.4.9: "Custom DLSS file" (Graphics tab, advanced, at the player's own risk). The player picks an official
// nvngx_dlss.dll (DLSS Super Resolution) they have - NVIDIA's DLSS SDK release, or a game they own. CogBox only checks it
// (a 64-bit DLL whose version resource says NVIDIA DLSS, Super Resolution, with a valid Authenticode signature from NVIDIA
// Corporation - the same rule package_cogbox.ps1 applies to the Streamline files) and copies it beside the game's exe.
// COGBox's own file is moved to <COGBox>\_old-files\dlss-backup\<game>\<port folder>\ first (never deleted), and
// "Restore COGBox's DLSS" moves it back. Nothing is downloaded. Per game: the folder of the exe Play starts.
#pragma once
#include <string>

namespace cogbox {

struct DlssFileInfo {
    bool ok = false;          // an official NVIDIA DLSS Super Resolution DLL
    std::string version;      // "310.9.16.0"
    std::string why;          // when !ok: the reason, for the player
};

// Checks a file the player picked (or the one beside a game).
DlssFileInfo dlss_check_file(const std::string &path);

struct DlssState {
    bool present = false;     // an nvngx_dlss.dll beside the exe
    bool custom = false;      // it is the player's own (COGBox's is in the backup folder)
    std::string version;      // the one beside the exe
    std::string backupVersion;
    std::string backupDir;    // where COGBox's own file is kept while a custom one is in use
};

// The DLSS file of the game whose exe is `exe` (cached by size and time, cheap to call every frame).
DlssState dlss_state(const std::string &exe);
// Copy the player's file beside `exe` (backing up COGBox's first). false + why on any problem; nothing half done.
bool dlss_install_custom(const std::string &exe, const std::string &src, std::string &msg);
// Put COGBox's file back (the player's goes to _old-files\dlss-replaced\<time>\, never deleted).
bool dlss_restore(const std::string &exe, std::string &msg);

}  // namespace cogbox
