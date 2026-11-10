// Layer 0 (game-free), GitHub #55 / #79 / #80 (Gears 3 "sudden death" that sticks to a save until the chapter is
// restarted, also seen in Xenia Canary - so most likely game state, not our timing): info-level lines in the game log
// for every save, checkpoint, profile and content (DLC) file in the user data folder, so the next report shows what
// was written, when, and how big, right before a death.
//
// The SDK keeps all of that under --user_data_root (its content manager's root: <xuid>/<title id>/<content type>/
// <package>/..., a host folder per XamContentCreate root such as SG0_0:), but logs XamContentCreate / Close /
// CreateEnumerator only at debug level and file writes not at all. Rather than an SDK patch (a new runtime build), this
// watches the folder from outside the game:
//   - at start: one line per file already there (relative path, size, last write) - what the content enumerator
//     (saves, DLC) will list this run;
//   - while the game runs: one line per change (added / written / removed / renamed) with the size after it.
// Paths are logged relative to the user data folder (no user names). Read-only: nothing is opened for writing,
// nothing is changed. --cogbox_save_log=false turns it off.
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

REXCVAR_DEFINE_BOOL(cogbox_save_log, true, "Gears2",
                    "GitHub #55: log the save / checkpoint / profile / content (DLC) files in --user_data_root at start "
                    "and every change to them while the game runs (info level, relative paths, sizes).");

namespace {

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

// The value of --name=... on our command line (the SDK's own cvars live in its runtime DLL; reading the command line
// keeps this file free of that link). Empty when not given.
std::wstring ArgValue(const wchar_t* name) {
  using SplitFn = LPWSTR*(WINAPI*)(LPCWSTR, int*);
  HMODULE shell = LoadLibraryW(L"shell32.dll");
  auto split = shell ? reinterpret_cast<SplitFn>(GetProcAddress(shell, "CommandLineToArgvW")) : nullptr;
  if (!split) return {};
  int argc = 0;
  LPWSTR* argv = split(GetCommandLineW(), &argc);
  if (!argv) return {};
  const std::wstring prefix = std::wstring(L"--") + name + L"=";
  std::wstring value;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a.compare(0, prefix.size(), prefix) == 0) value = a.substr(prefix.size());  // the last one wins, as for cvars
  }
  LocalFree(argv);
  while (!value.empty() && (value.back() == L'"' || value.back() == L'\\' || value.back() == L'/')) value.pop_back();
  if (!value.empty() && value.front() == L'"') value.erase(value.begin());
  return value;
}

std::string SizeOf(const std::filesystem::path& p) {
  WIN32_FILE_ATTRIBUTE_DATA a{};
  if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return "gone";
  if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return "folder";
  const uint64_t size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
  return std::to_string(size) + " bytes";
}

// Skip what is ours and not the game's: logs, crash dumps, shader caches (in case a launcher puts them in here).
bool Ignored(const std::wstring& rel) {
  std::wstring l = rel;
  for (wchar_t& c : l) c = wchar_t(towlower(c));
  auto ends = [&](const wchar_t* e) { const size_t n = wcslen(e); return l.size() >= n && l.compare(l.size() - n, n, e) == 0; };
  return ends(L".log") || ends(L".dmp") || l.find(L"shadercache") != std::wstring::npos ||
         l.find(L"shader-cache") != std::wstring::npos;
}

constexpr int kStartLinesMax = 300;     // files listed at start
constexpr int kChangeLinesMax = 20000;  // change lines per run (a long session writes a checkpoint every few minutes)

void ListAtStart(const std::filesystem::path& root) {
  std::error_code ec;
  int files = 0, shown = 0;
  uint64_t bytes = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::wstring rel = std::filesystem::relative(it->path(), root, ec).wstring();
    if (Ignored(rel)) continue;
    const uint64_t size = it->file_size(ec);
    ++files;
    bytes += size;
    if (shown >= kStartLinesMax) continue;
    ++shown;
    std::string when = "?";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    SYSTEMTIME st{};
    if (GetFileAttributesExW(it->path().c_str(), GetFileExInfoStandard, &a)) {
      FILETIME local{};
      if (FileTimeToLocalFileTime(&a.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &st)) {
        char b[32];
        snprintf(b, sizeof(b), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond);
        when = b;
      }
    }
    REXLOG_INFO("save: at start: {} ({} bytes, written {})", Utf8(rel), size, when);
  }
  REXLOG_INFO("save: at start: {} file(s), {} bytes in the user data folder{}", files, bytes,
              files > shown ? " (first " + std::to_string(shown) + " listed)" : "");
}

void Watch(std::filesystem::path root) {
  HANDLE dir = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (dir == INVALID_HANDLE_VALUE) {
    REXLOG_INFO("save: can't watch the user data folder (error {}); changes won't be logged", GetLastError());
    return;
  }
  std::vector<DWORD> buf(16384);  // 64 KB, DWORD-aligned as ReadDirectoryChangesW wants
  int lines = 0;
  std::wstring last_rel;
  DWORD last_action = 0;
  std::string last_size;
  auto last_time = std::chrono::steady_clock::now();
  for (;;) {
    DWORD got = 0;
    if (!ReadDirectoryChangesW(dir, buf.data(), DWORD(buf.size() * sizeof(DWORD)), TRUE,
                               FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                   FILE_NOTIFY_CHANGE_LAST_WRITE,
                               &got, nullptr, nullptr)) {
      REXLOG_INFO("save: watching the user data folder stopped (error {})", GetLastError());
      break;
    }
    if (got == 0) {  // the buffer overflowed: changes were lost; say so instead of guessing
      REXLOG_INFO("save: many changes at once - some were not listed");
      continue;
    }
    for (auto* p = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buf.data());;) {
      const std::wstring rel(p->FileName, p->FileNameLength / sizeof(wchar_t));
      if (!Ignored(rel)) {
        const char* what = p->Action == FILE_ACTION_ADDED              ? "added"
                           : p->Action == FILE_ACTION_REMOVED          ? "removed"
                           : p->Action == FILE_ACTION_MODIFIED         ? "written"
                           : p->Action == FILE_ACTION_RENAMED_OLD_NAME ? "renamed from"
                           : p->Action == FILE_ACTION_RENAMED_NEW_NAME ? "renamed to"
                                                                       : "changed";
        const std::string size = p->Action == FILE_ACTION_REMOVED || p->Action == FILE_ACTION_RENAMED_OLD_NAME
                                     ? std::string("-")
                                     : SizeOf(root / rel);
        // One write often comes as several notifications; repeat lines for the same file, change and size within a
        // second are left out.
        const auto now = std::chrono::steady_clock::now();
        const bool repeat = rel == last_rel && p->Action == last_action && size == last_size &&
                            now - last_time < std::chrono::seconds(1);
        if (!repeat && lines < kChangeLinesMax) {
          REXLOG_INFO("save: {} {} ({})", what, Utf8(rel), size);
          if (++lines == kChangeLinesMax) REXLOG_INFO("save: {} changes logged - no more this run", lines);
        }
        last_rel = rel;
        last_action = p->Action;
        last_size = size;
        last_time = now;
      }
      if (!p->NextEntryOffset) break;
      p = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(reinterpret_cast<const uint8_t*>(p) + p->NextEntryOffset);
    }
  }
  CloseHandle(dir);
}

}  // namespace

// Called once from XenosLogSystemReport (every game's OnPostInitLogging), after the PC description.
void XenosInstallSaveWatch() {
  static bool installed = false;
  if (installed || !REXCVAR_GET(cogbox_save_log)) return;
  installed = true;
  std::wstring root = ArgValue(L"user_data_root");
  const char* which = "--user_data_root";
  if (root.empty()) {  // the SDK then keeps user data in the game data folder
    root = ArgValue(L"game_data_root");
    which = "--game_data_root (no --user_data_root)";
  }
  std::error_code ec;
  if (root.empty() || !std::filesystem::is_directory(root, ec)) {
    REXLOG_INFO("save: no user data folder to watch ({} not given or not there); save files won't be logged", which);
    return;
  }
  REXLOG_INFO("save: logging save / checkpoint / content files in the user data folder ({}); "
              "paths below are relative to it (GitHub #55)", which);
  // The listing and the watch run on their own thread: a big folder must not hold up the game's start.
  std::thread([path = std::filesystem::path(root)] {
    ListAtStart(path);
    Watch(path);
  }).detach();
}
