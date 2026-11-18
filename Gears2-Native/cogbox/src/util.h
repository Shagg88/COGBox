// COGBox - small helpers: UTF-8 <-> wide text, the launcher's folder, a plain ini file, running programs.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

std::wstring widen(const std::string &utf8);
std::string narrow(const std::wstring &w);

// The folder CogBox.exe is in (no trailing backslash), as UTF-8 / wide.
std::string exe_dir();
std::wstring exe_dir_w();

bool file_exists(const std::string &path);
bool dir_exists(const std::string &path);
bool make_dirs(const std::string &path);          // like mkdir -p
bool read_file(const std::string &path, std::string &out, size_t limit = 64u << 20);
bool write_file(const std::string &path, const std::string &data);   // via a temporary file, then replaced
uint64_t file_size(const std::string &path);
std::string join(const std::string &a, const std::string &b);   // a\b
std::string now_stamp();                          // 20261025-163000 (local time)
std::string today_text();                         // 2026-10-25
int64_t unix_now();
std::string human_duration(int64_t seconds);      // "3 h 12 min", "45 min", "under a minute"
std::string human_bytes(uint64_t n);              // "7.2 GB"

// The launcher's own settings: CogBox.ini next to the exe, UTF-8, [section] / key = value. Unknown lines are kept.
struct Ini {
    std::string path;
    std::vector<std::string> lines;
    bool load(const std::string &p);
    bool save() const;
    std::string get(const std::string &sec, const std::string &key, const std::string &def = "") const;
    int geti(const std::string &sec, const std::string &key, int def) const;
    int64_t geti64(const std::string &sec, const std::string &key, int64_t def) const;
    void set(const std::string &sec, const std::string &key, const std::string &value);
    void seti(const std::string &sec, const std::string &key, int64_t v) { set(sec, key, std::to_string(v)); }
    void remove(const std::string &sec, const std::string &key);
    std::vector<std::pair<std::string, std::string>> section(const std::string &sec) const;
    void clear_section(const std::string &sec);
    void remove_section(const std::string &sec);   // 0.6.3: the [sec] line too, and everything under it
};

// Review r369 F1: a file's or folder's real path (links followed; "" if it can't be opened), and whether child_path is
// really inside real_parent (its real path is real_parent\\name) rather than a link to somewhere else - also a Wine Unix
// symlink, which has no reparse-point attribute.
std::wstring real_path(const std::wstring &p);
bool is_plain_child(const std::wstring &real_parent, const std::wstring &child_path, const std::wstring &name);

// Review r369 audit F1: folders COGBox made itself. dir_claim writes a random token into <dir>\.cogbox-owned and keeps it
// in <COGBox>\fabricator\owned.txt; dir_owned is true only for a folder holding a token from that list (a mod or a
// hand-made folder can't know one). mods_remove_tree removes only such folders. The token moves with the folder.
// The registry line also holds the folder's identity (volume, file index), and the marker must be a plain, single-link
// file really inside the folder, so a marker copied, hard-linked or symlinked from another folder counts for nothing.
bool dir_claim(const std::string &dir);
bool dir_owned(const std::string &dir);
std::string dir_owned_token(const std::string &dir);   // "" none
void dir_disown_token(const std::string &tok);         // after the folder is gone
// After an unpack into an owned folder: its files and folders with their identities, kept in the marker; a removal
// deletes only entries whose identity still matches (a link or a swapped-in name is a different file).
struct OwnedEntry { bool folder = false; uint64_t volume = 0, index = 0; std::string rel; };
bool dir_record_contents(const std::string &dir);
bool dir_recorded_contents(const std::string &dir, std::vector<OwnedEntry> &out);
bool file_identity(const std::string &path, uint64_t *volume, uint64_t *index, uint32_t *links);
bool running_under_wine();

// Review r369: a game program (gow1.exe, gow2.exe, gow3.exe, gowj.exe) is running, started by COGBox or not (Steam,
// a shortcut, the exe itself) - what the mods folder's writers check besides the launcher's own a.run.proc. Looked up
// at most once a second (the answer is kept); any thread.
bool any_game_exe_running();

// Opens a folder or a web page with Windows' own handler.
void shell_open(const std::string &target);
void shell_select(const std::string &file);   // Explorer opens the folder with that file selected
// Windows' folder / file pickers (empty = cancelled). filter is like L"Disc images\0*.iso\0All files\0*.*\0".
std::string pick_folder(const wchar_t *title);
std::string pick_file(const wchar_t *title, const wchar_t *filter);
// The save box (the Fabricator's "Make a zip"): default_name filled in, default_ext (L"zip") added when the name has
// none; it asks before replacing a file. Empty = cancelled.
std::string pick_save_file(const wchar_t *title, const wchar_t *filter, const std::string &default_name, const wchar_t *default_ext);
// Known folders
std::string desktop_dir();
