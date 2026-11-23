// COGBox - GitHub #286 (review r75 / r76): the game's crash minidump, written from outside the game
// (xenos/crash_dump_ipc.h).
#pragma once

#include <windows.h>

#include <memory>
#include <string>

// What the watcher did, for play_poll to log on the main thread (r76 L4).
struct CrashDumpStatus {
    enum : LONG { kWatching = 0, kWritten = 1, kFailed = 2 };   // kWatching at the end: no dump was asked for
    volatile LONG state = kWatching;
    volatile DWORD error = 0;   // kFailed: the Windows error
};

// Started right after the game: a small thread waits for the game's crash request (or its end) and writes the dump
// with MiniDumpWriteDump on the game's process handle, only as <logDir>\crash-YYYYMMDD-HHMMSS.dmp. The handle is
// duplicated; the caller keeps its own. nullptr when the watcher couldn't be started.
// extraType (docs/logging.md, the Everything logging level): more dump flags, only those in kDumpTypeExtraAllowed.
constexpr DWORD kDumpTypeExtraAllowed = 0x00000001 /* DataSegs */ | 0x00000004 /* HandleData */ | 0x00000020 /* UnloadedModules */ |
                                        0x00000100 /* ProcessThreadData */ | 0x00000800 /* FullMemoryInfo */;
std::shared_ptr<CrashDumpStatus> crash_dump_watch(HANDLE proc, DWORD pid, const std::string &logDir, DWORD extraType = 0);

// The writing itself (the thread above calls it; tests call it directly): claims the game's request in its info
// block, sets ack (when given), writes the .dmp and puts the result in the block. 0 = no valid request to claim
// (stray or already given up by the game: ignored), else CrashDumpStatus::kWritten / kFailed (*error set).
int crash_dump_write(HANDLE proc, DWORD pid, const std::wstring &folder, HANDLE ack, DWORD *error, DWORD extraType = 0);

// The only path the launcher writes: "\\?\" + the play's folder (absolute, as GetFullPathNameW gives it; "\\?\UNC\"
// for a share) + "\crash-<8 digits>-<6 digits>.dmp", the folder compared without case.
bool crash_dump_path_ok(const wchar_t *path, const std::wstring &folder);

// The play's folder as crash_dump_path_ok wants it (absolute, no trailing separator), from a UTF-8 path.
std::wstring crash_dump_folder(const std::string &logDir);
