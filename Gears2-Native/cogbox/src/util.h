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
};

// Opens a folder or a web page with Windows' own handler.
void shell_open(const std::string &target);
// Windows' folder / file pickers (empty = cancelled). filter is like L"Disc images\0*.iso\0All files\0*.*\0".
std::string pick_folder(const wchar_t *title);
std::string pick_file(const wchar_t *title, const wchar_t *filter);
// Known folders
std::string desktop_dir();
