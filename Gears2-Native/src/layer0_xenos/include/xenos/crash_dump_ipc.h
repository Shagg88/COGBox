// GitHub #286 (review r75): the game's crash minidump written by the launcher (COGBox) from outside the game, where
// no lock inside the frozen game can hang it. Shared by the game (layer 0 crash_report.cpp) and COGBox
// (cogbox/src/crash_dump.cpp), so both build the same names and read the same block. Windows-only, header-only.
//
// Per game process (pid): a shared memory block "Local\cogbox-crash-<pid>-info" (CrashDumpInfo, made by the game at
// start) and three events, made by whichever side comes first (CreateEvent opens an existing one):
//   req  (auto-reset)   game -> launcher: the info block is filled, write the dump
//   ack  (manual-reset) launcher -> game: request taken (no ack within a few seconds: no launcher, the game dumps)
//   done (manual-reset) launcher -> game: finished; CrashDumpInfo::result says how
#pragma once

#include <cstddef>
#include <cstdint>

namespace xenos_crash_ipc {

constexpr uint32_t kMagic = 0x31445243;  // "CRD1"
constexpr uint32_t kResultNone = 0, kResultWritten = 1, kResultFailed = 2;
constexpr size_t kPathChars = 1100;

struct CrashDumpInfo {
  uint32_t magic;               // kMagic once the game filled it in
  uint32_t thread_id;           // the crashing thread
  uint64_t exception_pointers;  // EXCEPTION_POINTERS* in the game's memory (MiniDumpWriteDump ClientPointers = TRUE)
  uint32_t dump_type;           // MINIDUMP_TYPE
  uint32_t result;              // launcher: kResultWritten / kResultFailed
  uint32_t error;               // launcher: GetLastError() of the failure
  uint32_t reserved;
  wchar_t path[kPathChars];     // the .dmp to write, "\\?\" form, ends in ".dmp"
};

// "Local\cogbox-crash-<pid>-<what>" into out (no allocation). what: "info", "req", "ack", "done".
inline void Name(wchar_t* out, size_t cap, uint32_t pid, const wchar_t* what) {
  const wchar_t* prefix = L"Local\\cogbox-crash-";
  size_t n = 0;
  for (; *prefix && n + 1 < cap; ++prefix) out[n++] = *prefix;
  wchar_t digits[12];
  int d = 0;
  do { digits[d++] = static_cast<wchar_t>(L'0' + pid % 10); pid /= 10; } while (pid && d < 12);
  while (d && n + 1 < cap) out[n++] = digits[--d];
  if (n + 1 < cap) out[n++] = L'-';
  for (; *what && n + 1 < cap; ++what) out[n++] = *what;
  out[n] = 0;
}

// The launcher writes only a path the game gave that names a .dmp file.
inline bool PathOk(const wchar_t* p) {
  size_t n = 0;
  while (n < kPathChars && p[n]) ++n;
  if (n < 5 || n >= kPathChars) return false;
  const wchar_t* e = p + n - 4;
  return e[0] == L'.' && (e[1] | 0x20) == L'd' && (e[2] | 0x20) == L'm' && (e[3] | 0x20) == L'p';
}

}  // namespace xenos_crash_ipc
