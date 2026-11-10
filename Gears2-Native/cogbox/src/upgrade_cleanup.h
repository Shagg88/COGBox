// CogBox 0.4.9 (cleanup049): what an update leaves behind that hash matching (stale_files.h) can't see. Runs at the
// launcher's start whenever the COGBox version changed (CogBox.ini [Cleanup] UpgradeDoneVersion), before any game can
// start, on both ways of updating (the in-app updater restarts CogBox.exe; a release unzipped by hand starts it too):
//  - settings.ini: read and written again by this version's Store (gears_settings.cpp load_ini translates the old keys
//    - Fps, WineD3D12, Exclusive, the PS1's flat keys, FrameCap list 1 - and save_ini writes only the keys that still
//    exist). The player's values are kept; the file as it was is copied first to _old-files\settings-from-<prev>\.
//  - <Game>\shadercache: the compiled shaders (shaders.pack, loose .dxil / .spv) and the saved pipelines
//    (pipelines-*.bin) belong to one shader compiler (DXC) and one D3D12 runtime (Agility D3D12Core.dll). When the
//    release's dxcompiler.dll or D3D12Core.dll changed (their SHA-256 in SHA256SUMS.txt, remembered in CogBox.ini
//    [Cleanup] CacheKey) those files move to _old-files\shadercache-from-<prev>\<Game>\ with prepare-progress.txt, so
//    the launcher offers "Prepare all shaders" again. ucode\ (the game's recorded shader microcode, which preparing
//    rebuilds from) stays. Nothing is deleted.
// Program files (.exe / .dll) of older releases are stale_files.cpp's job; the updater's own leftovers (*.old-cogbox,
// *.new-cogbox, CogBox-update.*.part) are update.cpp update_tidy's.
#pragma once
#include <string>

namespace cogbox {

// "dxc=<sha256>;agility=<sha256>[,<sha256>...]" from a release's SHA256SUMS.txt text: the hash of shared\dxcompiler.dll
// (else the first <...>\dxcompiler.dll by name) and the sorted distinct hashes of every D3D12Core.dll. "" when the
// list names neither.
std::string upgrade_cache_key(const std::string &sumsText);

// rel: a path relative to <Game>\shadercache ('\' or '/'). True for the files tied to the compiler / D3D12 runtime
// (moved when the cache key changes): top-level shaders.pack, pipelines-*.bin, *.dxil, *.spv, shaders.bin,
// prepare-progress.txt. Never anything in a sub-folder (ucode\recorded.pack is the game's own recording).
bool upgrade_cache_file(const std::string &rel);

// Shagg 2026-11-08: the NVIDIA Streamline files 0.4.8 put beside each game exe (sl.*.dll, nvngx_dlssg.dll) that 0.4.9
// ships once in shared\. rel: a path relative to the COGBox folder; now: this release's SHA256SUMS.txt. True when rel
// is such a file in <Game>\port*\, this release doesn't list it there and does list shared\<name>. It is moved to
// _old-files only when its bytes are a file COGBox shipped (known_old_program) - a player's own copy stays.
struct ShippedList;
bool upgrade_streamline_leftover(const std::string &rel, const ShippedList &now);

// settings.ini as this version writes it (old keys translated, dead ones dropped, values kept). Returns false when the
// text holds no setting this version reads (then the file is left alone); *changed tells whether the text differs.
bool upgrade_settings_text(const std::string &in, std::string *out, bool *changed);

}  // namespace cogbox

#ifdef _WIN32
struct App;
// Once per COGBox version, right after stale_cleanup_startup. Logs to logs\old-files.txt.
void upgrade_cleanup_startup(App &a, const std::string &prevVersion);
#endif
