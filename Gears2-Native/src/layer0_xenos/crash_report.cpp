// Layer 0 (game-free): crash report for unhandled host exceptions (roadmap section 13). Writes, into --gow2_crash_dir
// (default: <exe dir>\crashes), a text file with the exception and an unwound stack (module + offset per frame, for
// llvm-symbolizer --relative-address), where every other thread was at that moment, and a small minidump. Only
// exceptions nobody else handled reach this: the runtime's own guest-memory fault handling runs first.
//
// GitHub #286: a report came back as an empty .txt and a 32-byte .dmp (the minidump header only). The old handler wrote
// both on the crashing thread with fopen / fprintf (CRT stream lock, a heap buffer, file data only reaching the disk at
// fclose) and then called MiniDumpWriteDump in-process, which can hang for good on a lock (process heap, loader) held
// by a thread it suspended. Now:
//   - everything the crash path needs is made at start: the folder as an NT path, a fixed text buffer, two threads;
//   - the crashing thread only hands its exception over and waits (bounded): no stack used there, so a stack overflow
//     is reported as well (and threads with a normal-sized stack keep --gow2_crash_stack_guarantee_kb for it);
//   - a helper thread made at start copies the exception, then writes the .txt with NtCreateFile / WriteFile through
//     to the disk (no CRT, no heap, no logger), section by section, so whatever it got to is on the disk;
//   - stacks as GearsJ's freeze watchdog reads them (r74): a thread is suspended only to copy its context and stack top
//     (syscalls only), resumed, and the copy unwound afterwards, each step SEH-guarded as in the profiler and stopping
//     at the first address outside every image; under Wine / Proton (where the unwinder takes the loader lock) other
//     threads' stacks are skipped and the crashing thread's only read when the loader lock is free;
//   - the minidump (--gow2_crash_minidump): written by COGBox from outside the game, where no lock of the game's can
//     hang it (xenos/crash_dump_ipc.h; review r75); without COGBox (no answer within 3 s) in-process, only when the
//     process heap lock and the loader lock can be taken. Left to Windows Error Reporting only when LocalDumps
//     (HKLM: Microsoft documents the HKCU key COGBox also writes as unsupported) points at the crash folder.
#include <xenos/dev_tools.h>
#include <xenos/crash_dump_ipc.h>
#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <dbghelp.h>

REXCVAR_DEFINE_STRING(gow2_crash_dir, "", "Gears2", "Folder for crash reports (default: <exe dir>\\crashes)");
REXCVAR_DEFINE_INT32(gow2_crash_stack_guarantee_kb, 32, "Gears2",
                     "Stack kept back in each thread with a stack of 512 KB or more, so a stack overflow can still be "
                     "reported (SetThreadStackGuarantee). 0 = Windows' default");
REXCVAR_DEFINE_STRING(gow2_crash_minidump, "auto", "Gears2",
                      "Crash minidump: auto = written by COGBox from outside the game (or, without COGBox, by the game "
                      "after its lock checks; left to Windows Error Reporting when an HKLM LocalDumps key points at the "
                      "crash folder); game = always written by the game itself; windows = never by the game or COGBox. "
                      "A crashed game can wait up to ~4.5 minutes for its report and dump before it closes (only when "
                      "something hangs)");
REXCVAR_DEFINE_STRING(gow2_crash_test, "", "Gears2",
                      "Test the crash report 10 s after start: av (access violation), stack (stack overflow), abort, "
                      "heaplock (crash while holding the process heap lock: the report must still be written and the "
                      "game must not hang). Empty = off");

namespace {

using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                        ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
NtCreateFileFn g_nt_create_file = nullptr;
NtQuerySystemInformationFn g_nt_query_system = nullptr;

constexpr int kPath = 1024;
wchar_t g_nt_dir[kPath];  // \??\<absolute crash folder>, no trailing separator
size_t g_nt_dir_len = 0;
char g_exe_name[MAX_PATH];
DWORD64 g_exe_base = 0;
bool g_wine = false;
bool g_wer_here = false;     // an HKLM LocalDumps key for this exe points at the crash folder
int g_minidump_mode = 0;     // 0 auto, 1 game, 2 windows

// XenosCrashNote: fixed storage, so the crash handler reads it without allocating.
constexpr int kNotes = 16, kNoteLen = 240;
char g_notes[kNotes][kNoteLen];
volatile LONG g_note_count = 0;

// ReportHere's reason travels in ExceptionInformation[0], marked by this in [1].
constexpr ULONG_PTR kReasonMark = 0x6F77326661696C;
constexpr DWORD kFailFast = 0xC0000409;  // STATUS_STACK_BUFFER_OVERRUN, the code the CRT's fail-fast paths end with
constexpr DWORD kReportWaitMs = 30000;    // the crashing thread waits at most this long for the report
constexpr DWORD kDumpWaitMs = 240000;     // ... and this much more while the minidump step runs (COGBox's ack 3 s +
                                          // its dump 120 s + the lock probes 4 s + the game's own dump)
constexpr DWORD kAckWaitMs = 3000;        // COGBox answers a dump request within this, or the game dumps itself
constexpr DWORD kLauncherDumpMs = 120000; // COGBox's minidump

// GitHub #286 / r75: COGBox writes the minidump from outside (xenos/crash_dump_ipc.h), made at start.
xenos_crash_ipc::CrashDumpInfo* g_ipc_info = nullptr;
HANDLE g_ipc_req = nullptr, g_ipc_ack = nullptr, g_ipc_done = nullptr;
HANDLE g_parent = nullptr;  // r76 L2: the process that started the game (COGBox): its end stops the waits for it
constexpr DWORD kProbeWaitMs = 2000;      // heap / loader lock probes

HANDLE g_go = nullptr, g_done = nullptr, g_probe_go = nullptr, g_probe_ok = nullptr, g_helper = nullptr;
DWORD g_helper_tid = 0, g_prober_tid = 0;
EXCEPTION_POINTERS* volatile g_ep = nullptr;
volatile DWORD g_crash_tid = 0;
volatile LONG g_owner = 0;    // the thread whose crash is being reported
volatile LONG g_dumping = 0;  // the helper is in the minidump step (COGBox's request, the probes, the dump)

// The helper's own copy of the exception (r74 L1: the crashing thread's EXCEPTION_POINTERS may live on a stack that is
// reused once it stops waiting).
EXCEPTION_RECORD g_er;
alignas(16) CONTEXT g_ctx;
EXCEPTION_POINTERS g_ep_copy{&g_er, &g_ctx};

// ---- the text, written through to the disk (no CRT, no heap) ----------------------------------------------------
char g_text[32768];
size_t g_len = 0;
HANDLE g_txt = INVALID_HANDLE_VALUE;

void Write() {  // the handle is write-through: on the disk when this returns
  if (g_txt != INVALID_HANDLE_VALUE && g_len) {
    DWORD done = 0;
    WriteFile(g_txt, g_text, static_cast<DWORD>(g_len), &done, nullptr);
  }
  g_len = 0;
}
void Section() {
  Write();
  if (g_txt != INVALID_HANDLE_VALUE) FlushFileBuffers(g_txt);
}
void Put(const char* s) {
  for (; *s; ++s) {
    if (g_len >= sizeof(g_text)) Write();
    g_text[g_len++] = *s;
  }
}
void PutHex(unsigned long long v) {
  char b[24], *p = b + sizeof(b);
  *--p = 0;
  do { *--p = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
  Put("0x");
  Put(p);
}
void PutDec(unsigned long long v) {
  char b[24], *p = b + sizeof(b);
  *--p = 0;
  do { *--p = static_cast<char>('0' + v % 10); v /= 10; } while (v);
  Put(p);
}

// \??\ + an absolute DOS path, for NtCreateFile (no heap at crash time, unlike CreateFileW's path conversion).
bool OpenNt(const wchar_t* name, bool dir, bool append, bool through, HANDLE* out) {
  wchar_t p[kPath + 64];
  size_t n = 0;
  for (size_t i = 0; i < g_nt_dir_len; ++i) p[n++] = g_nt_dir[i];
  if (name || (n && p[n - 1] == L':')) p[n++] = L'\\';  // r74 L3: a drive root is "\??\C:\", not the volume "\??\C:"
  for (; name && *name && n < kPath + 62; ++name) p[n++] = *name;
  p[n] = 0;
  UNICODE_STRING us{static_cast<USHORT>(n * 2), static_cast<USHORT>(n * 2 + 2), p};
  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
  IO_STATUS_BLOCK io{};
  const ACCESS_MASK access = dir ? (FILE_LIST_DIRECTORY | SYNCHRONIZE)
                                 : ((append ? FILE_APPEND_DATA : GENERIC_WRITE) | SYNCHRONIZE);
  const ULONG options = FILE_SYNCHRONOUS_IO_NONALERT | (dir ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE) |
                        (through ? FILE_WRITE_THROUGH : 0);
  HANDLE h = nullptr;
  const NTSTATUS s = g_nt_create_file ? g_nt_create_file(&h, access, &oa, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                         (dir || append) ? FILE_OPEN_IF : FILE_OVERWRITE_IF, options,
                                                         nullptr, 0)
                                      : -1;
  *out = s >= 0 ? h : INVALID_HANDLE_VALUE;
  return s >= 0;
}

// ---- locks -----------------------------------------------------------------------------------------------------
// The loader lock, tried without waiting (PEB->LoaderLock, x64 PEB + 0x110; Wine's PEB has it in the same place).
bool LoaderLockFree() {
  const char* peb = *reinterpret_cast<char* const*>(reinterpret_cast<const char*>(NtCurrentTeb()) + 0x60);
  auto* cs = peb ? *reinterpret_cast<CRITICAL_SECTION* const*>(peb + 0x110) : nullptr;
  if (!cs) return false;
  for (DWORD waited = 0; waited < kProbeWaitMs; waited += 10) {
    if (TryEnterCriticalSection(cs)) {
      LeaveCriticalSection(cs);
      return true;
    }
    Sleep(10);
  }
  return false;
}
// The process heap's lock (the CRT allocates there too): HeapLock waits for exactly that lock - a small HeapAlloc
// would be served by the low-fragmentation front end without it (r74 H2). On the prober, so the wait is bounded.
DWORD WINAPI ProberMain(void*) {
  for (;;) {
    WaitForSingleObject(g_probe_go, INFINITE);
    HANDLE heap = GetProcessHeap();
    if (HeapLock(heap)) HeapUnlock(heap);
    SetEvent(g_probe_ok);
  }
}
bool HeapLockFree() {
  if (!g_probe_go) return false;
  ResetEvent(g_probe_ok);
  SetEvent(g_probe_go);
  return WaitForSingleObject(g_probe_ok, kProbeWaitMs) == WAIT_OBJECT_0;
}

// ---- stacks: copy, then unwind the copy (GearsJ freeze_watchdog.cpp), each step SEH-guarded (profiler.cpp) -------
constexpr size_t kStackCopy = 256 * 1024;
uint8_t g_stack_copy[kStackCopy];  // helper thread only

// Syscalls only (the thread may be suspended): its stack from RSP to the end of that committed region.
size_t CopyStack(const CONTEXT& c, uint8_t* buf) {
  if (!c.Rsp) return 0;
  MEMORY_BASIC_INFORMATION mi{};
  if (!VirtualQuery(reinterpret_cast<const void*>(c.Rsp), &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return 0;
  size_t len = static_cast<size_t>(reinterpret_cast<DWORD64>(mi.BaseAddress) + mi.RegionSize - c.Rsp);
  if (len > kStackCopy) len = kStackCopy;
  SIZE_T got = 0;
  ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(c.Rsp), buf, len, &got);  // partial is fine
  return static_cast<size_t>(got);
}

bool UnwindStep(CONTEXT* c) {  // no C++ objects here (SEH)
#if defined(_MSC_VER)
  __try {
#endif
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c->Rip, &image_base, nullptr);
    if (!fn) {  // leaf function: the return address is at the top of the stack
      c->Rip = *reinterpret_cast<const DWORD64*>(c->Rsp);
      c->Rsp += 8;
    } else {
      PVOID handler_data = nullptr;
      DWORD64 frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c->Rip, fn, c, &handler_data, &frame, nullptr);
    }
    return true;
#if defined(_MSC_VER)
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#endif
}
void* ImageOf(DWORD64 pc) {
  void* base = nullptr;
  RtlPcToFileHeader(reinterpret_cast<void*>(pc), &base);
  return base;
}
void Rebase(CONTEXT* c, DWORD64 lo, DWORD64 hi, DWORD64 base) {  // registers restored from the copy hold live-stack
  DWORD64* regs[] = {&c->Rbp, &c->Rbx, &c->Rsi, &c->Rdi, &c->R12, &c->R13, &c->R14, &c->R15};  // addresses
  for (DWORD64* r : regs)
    if (*r >= lo && *r < hi) *r = *r - lo + base;
}
// Unwinds a copy made by CopyStack; stops when RSP leaves the copy, and at the first address outside every image
// (JIT code or a broken stack), so dynamic function tables are never looked up.
int Unwind(CONTEXT c, const uint8_t* stack, size_t len, DWORD64* pc, int max) {
  int n = 0;
  if (len < 8) {
    if (c.Rip) pc[n++] = c.Rip;
    return n;
  }
  const DWORD64 lo = c.Rsp, hi = c.Rsp + len, base = reinterpret_cast<DWORD64>(stack), end = base + len;
  c.Rsp = base;
  Rebase(&c, lo, hi, base);
  while (n < max && c.Rip && c.Rsp >= base && c.Rsp + 8 <= end) {
    pc[n++] = c.Rip;
    if (!ImageOf(c.Rip)) break;
    const DWORD64 rsp = c.Rsp;
    if (!UnwindStep(&c)) break;
    Rebase(&c, lo, hi, base);
    if (c.Rsp <= rsp) break;  // no progress: a broken frame
  }
  return n;
}

// The module's name from its own export directory (no loader call), copied under SEH (an image being unloaded).
void ImageName(void* base, char* out, size_t cap) {
  out[0] = '?';
  out[1] = 0;
  if (reinterpret_cast<DWORD64>(base) == g_exe_base) {
    strncpy_s(out, cap, g_exe_name, _TRUNCATE);
    return;
  }
#if defined(_MSC_VER)
  __try {
#endif
    const auto* b = static_cast<const char*>(base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress || !d.Size) return;
    const auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(b + d.VirtualAddress);
    if (!ex->Name) return;
    size_t i = 0;
    for (const char* s = b + ex->Name; *s && i + 1 < cap; ++s) out[i++] = *s;
    out[i] = 0;
#if defined(_MSC_VER)
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = '?';
    out[1] = 0;
  }
#endif
}
void PutFrame(int i, DWORD64 rip, bool modules) {
  void* base = modules ? ImageOf(rip) : nullptr;
  char name[MAX_PATH];
  if (base) ImageName(base, name, sizeof(name));
  Put("  #");
  if (i < 10) Put("0");
  PutDec(i);
  Put(" ");
  Put(base ? name : "?");
  Put(" ");
  PutHex(rip - reinterpret_cast<DWORD64>(base));
  Put("\n");
}

// ---- every other thread: state, wait reason, CPU time, stack ----------------------------------------------------
struct SysThread {  // SYSTEM_THREAD_INFORMATION (x64)
  LARGE_INTEGER kernel_time, user_time, create_time;
  ULONG wait_time;
  PVOID start_address;
  HANDLE process_id, thread_id;
  LONG priority, base_priority;
  ULONG context_switches, state, wait_reason;
};
static_assert(sizeof(SysThread) == 0x50, "SYSTEM_THREAD_INFORMATION");
constexpr size_t kProcThreadsAt = 0x100, kProcIdAt = 0x50;  // in SYSTEM_PROCESS_INFORMATION (x64)

const char* WaitReason(ULONG r) {
  static const char* const k[] = {
      "Executive", "FreePage", "PageIn", "PoolAllocation", "DelayExecution", "Suspended", "UserRequest",
      "WrExecutive", "WrFreePage", "WrPageIn", "WrPoolAllocation", "WrDelayExecution", "WrSuspended",
      "WrUserRequest", "WrSpare0", "WrQueue", "WrLpcReceive", "WrLpcReply", "WrVirtualMemory", "WrPageOut",
      "WrRendezvous", "WrKeyedEvent", "WrTerminated", "WrProcessInSwap", "WrCpuRateControl", "WrCalloutStack",
      "WrKernel", "WrResource", "WrPushLock", "WrMutex", "WrQuantumEnd", "WrDispatchInt", "WrPreempted",
      "WrYieldExecution", "WrFastMutex", "WrGuardedMutex", "WrRundown", "WrAlertByThreadId (a lock or WaitOnAddress)"};
  return r < sizeof(k) / sizeof(k[0]) ? k[r] : "other";
}

void PutThreads(DWORD crash_tid) {
  Put("threads (where each one was; suspended one at a time only to copy its stack):\n");
  if (g_wine) Put("  (stacks skipped under Wine / Proton: the unwinder there takes the loader lock)\n");
  if (!g_nt_query_system) return;
  void* buf = nullptr;
  ULONG size = 1 << 20;
  NTSTATUS s = -1;
  for (int tries = 0; tries < 6; ++tries, size *= 2) {
    buf = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return;
    s = g_nt_query_system(5 /*SystemProcessInformation*/, buf, size, nullptr);
    if (s >= 0) break;
    VirtualFree(buf, 0, MEM_RELEASE);
    buf = nullptr;
    if (s != static_cast<NTSTATUS>(0xC0000004L)) return;  // STATUS_INFO_LENGTH_MISMATCH: a bigger buffer
  }
  if (!buf) return;
  const DWORD pid = GetCurrentProcessId();
  int unwritten = 0, batch = 1;
  for (const char* p = static_cast<const char*>(buf);;) {
    const ULONG next = *reinterpret_cast<const ULONG*>(p);
    const ULONG count = *reinterpret_cast<const ULONG*>(p + 4);
    if (static_cast<DWORD>(*reinterpret_cast<const ULONG_PTR*>(p + kProcIdAt)) == pid) {
      const auto* t = reinterpret_cast<const SysThread*>(p + kProcThreadsAt);
      for (ULONG i = 0; i < count; ++i) {
        const DWORD tid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(t[i].thread_id));
        if (tid == g_helper_tid || tid == g_prober_tid) continue;
        Put("thread ");
        PutDec(tid);
        static const char* const kState[] = {"initialized", "ready", "running", "standby",
                                             "terminated", "waiting", "transition", "deferred ready"};
        Put(" ");
        Put(t[i].state < 8 ? kState[t[i].state] : "?");
        if (t[i].state == 5) {
          Put(" (");
          Put(WaitReason(t[i].wait_reason));
          Put(")");
        }
        Put(", cpu user ");
        PutDec(static_cast<unsigned long long>(t[i].user_time.QuadPart) / 10000);
        Put(" ms kernel ");
        PutDec(static_cast<unsigned long long>(t[i].kernel_time.QuadPart) / 10000);
        Put(" ms, ");
        PutDec(t[i].context_switches);
        Put(" switches");
        if (tid == crash_tid) {
          Put(" - the crashing thread, its stack is above\n");
          continue;
        }
        Put("\n");
        // r74 M2: on the disk before this thread is suspended, so a hang there names it; r75: every 8 threads when
        // the disk is slow (a write over 50 ms), to keep within the crashing thread's wait
        if (++unwritten >= batch) {
          const ULONGLONG t0 = GetTickCount64();
          Write();
          unwritten = 0;
          if (GetTickCount64() - t0 > 50) batch = 8;
        }
        if (g_wine) continue;
        DWORD64 frames[16];
        int n = 0;
        if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                  FALSE, tid)) {
          if (GetProcessIdOfThread(h) == pid && SuspendThread(h) != static_cast<DWORD>(-1)) {  // r74 L2: still ours
            alignas(16) CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            size_t len = 0;
            const bool ok = GetThreadContext(h, &c) != 0;
            if (ok) len = CopyStack(c, g_stack_copy);  // syscalls only while suspended
            ResumeThread(h);
            if (ok) n = Unwind(c, g_stack_copy, len, frames, 16);  // after the resume
          }
          CloseHandle(h);
        }
        for (int f = 0; f < n; ++f) PutFrame(f, frames[f], true);
      }
    }
    if (!next) break;
    p += next;
  }
  VirtualFree(buf, 0, MEM_RELEASE);
}

void PutWerDump() {
  if (g_wer_here) {
    Put("Windows Error Reporting (LocalDumps, HKLM, for this folder) writes ");
    Put(g_exe_name);
    Put(".");
    PutDec(GetCurrentProcessId());
    Put(".dmp here from outside the game\n");
  } else if (g_wine) {
    Put("Wine / Proton has no Windows Error Reporting: no minidump\n");
  } else {
    Put("Windows Error Reporting's LocalDumps (HKLM) isn't set for this folder: no minidump\n");
  }
}

// COGBox writes the minidump from outside the game. kNoLauncher: nobody answered (run without COGBox, or an older one).
enum { kNoLauncher, kLauncherWritten, kLauncherFailed, kLauncherStuck };
DWORD WaitOrParent(HANDLE h, DWORD ms) {  // WAIT_OBJECT_0: h; WAIT_OBJECT_0 + 1: COGBox ended
  HANDLE w[2] = {h, g_parent};
  return WaitForMultipleObjects(g_parent ? 2 : 1, w, FALSE, ms);
}
int AskLauncher(const wchar_t* name, DWORD tid) {
  namespace ipc = xenos_crash_ipc;
  if (!g_ipc_info || !g_ipc_req || !g_ipc_ack || !g_ipc_done) return kNoLauncher;
  ipc::CrashDumpInfo* in = g_ipc_info;
  size_t n = 0;
  for (const wchar_t* s = L"\\\\?\\"; *s; ++s) in->path[n++] = *s;  // \\?\ + the folder after "\??\"
  for (size_t i = 4; i < g_nt_dir_len && n < ipc::kPathChars - 40; ++i) in->path[n++] = g_nt_dir[i];
  in->path[n++] = L'\\';
  for (; *name && n < ipc::kPathChars - 1; ++name) in->path[n++] = *name;
  in->path[n] = 0;
  in->thread_id = tid;
  in->exception_pointers = reinterpret_cast<uint64_t>(&g_ep_copy);
  in->dump_type = MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory;
  in->result = ipc::kResultNone;
  in->error = 0;
  MemoryBarrier();
  in->magic = ipc::kMagic;
  ResetEvent(g_ipc_ack);
  ResetEvent(g_ipc_done);
  SetEvent(g_ipc_req);
  if (WaitOrParent(g_ipc_ack, kAckWaitMs) != WAIT_OBJECT_0) {
    // r76 L3: take the request back atomically - unless COGBox claimed it just now (then it writes, we wait)
    const LONG was = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&in->magic), 0,
                                                static_cast<LONG>(ipc::kMagic));
    if (was != static_cast<LONG>(ipc::kTaken)) {
      ResetEvent(g_ipc_req);
      return kNoLauncher;
    }
  }
  Put("COGBox (the launcher) is writing it from outside the game ... ");
  Section();
  const DWORD w = WaitOrParent(g_ipc_done, kLauncherDumpMs);
  if (w == WAIT_OBJECT_0 + 1) {
    Put("COGBox ended during it; ");
    return kLauncherFailed;
  }
  if (w != WAIT_OBJECT_0) return kLauncherStuck;
  if (in->result == ipc::kResultWritten) return kLauncherWritten;
  Put("failed, error ");
  PutHex(in->error);
  Put("; ");
  return kLauncherFailed;
}

void WriteReport(EXCEPTION_POINTERS* ep, DWORD tid) {
  // first of all: our own copy of the exception (the crashing thread's may go away after its wait)
  g_er = *ep->ExceptionRecord;
  g_er.ExceptionRecord = nullptr;
  g_ctx = *ep->ContextRecord;

  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[40] = L"crash-00000000-000000.txt";  // %Y%m%d-%H%M%S, as before
  const unsigned v[6] = {st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond};
  const int at[6] = {6, 10, 12, 15, 17, 19}, digits[6] = {4, 2, 2, 2, 2, 2};
  for (int i = 0; i < 6; ++i)
    for (int d = digits[i] - 1, x = v[i]; d >= 0; --d, x /= 10) name[at[i] + d] = static_cast<wchar_t>(L'0' + x % 10);
  HANDLE dir;
  if (OpenNt(nullptr, true, false, false, &dir)) CloseHandle(dir);  // makes the folder when it's missing
  OpenNt(name, false, false, true, &g_txt);
  g_len = 0;

  const EXCEPTION_RECORD& er = g_er;
  Put("exception ");
  PutHex(er.ExceptionCode);
  Put(" at ");
  PutHex(reinterpret_cast<DWORD64>(er.ExceptionAddress));
  Put(" thread ");
  PutDec(tid);
  Put("\n");
  if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2) {
    const ULONG_PTR k = er.ExceptionInformation[0];
    Put("access violation: ");
    Put(k == 0 ? "read" : k == 1 ? "write" : "execute");
    Put(" address ");
    PutHex(er.ExceptionInformation[1]);
    Put("\n");
  }
  if (er.ExceptionCode == EXCEPTION_STACK_OVERFLOW) Put("stack overflow\n");
  if (er.NumberParameters == 2 && er.ExceptionInformation[1] == kReasonMark) {
    Put("fail-fast: ");
    Put(reinterpret_cast<const char*>(er.ExceptionInformation[0]));  // a string literal
    Put("\n");
  }
  const CONTEXT& r = g_ctx;
  const struct { const char* n; DWORD64 v; } regs[] = {
      {"rax ", r.Rax}, {" rbx ", r.Rbx}, {" rcx ", r.Rcx}, {" rdx ", r.Rdx}, {" rsi ", r.Rsi}, {" rdi ", r.Rdi},
      {" rsp ", r.Rsp}, {" rbp ", r.Rbp}, {"\nr8 ", r.R8}, {" r9 ", r.R9}, {" r10 ", r.R10}, {" r11 ", r.R11},
      {" r12 ", r.R12}, {" r13 ", r.R13}, {" r14 ", r.R14}, {" r15 ", r.R15}};
  for (const auto& x : regs) {
    Put(x.n);
    PutHex(x.v);
  }
  Put("\n");
  const LONG notes = g_note_count < kNotes ? g_note_count : kNotes;
  if (notes > 0) {
    Put("notes:\n");
    for (LONG i = 0; i < notes; ++i) {
      Put("  ");
      Put(g_notes[i]);
      Put("\n");
    }
  }
  Section();  // r74 M1: the exception, registers and notes are on the disk before any stack is read

  Put("stack (module offset):\n");
  {
    // under Wine the unwinder and RtlPcToFileHeader take the loader lock, which the crashing thread may hold
    const bool lookups = !g_wine || LoaderLockFree();
    if (!lookups) Put("  (loader lock taken: only the faulting address)\n");
    DWORD64 frames[48];
    const size_t len = lookups ? CopyStack(r, g_stack_copy) : 0;
    const int n = lookups ? Unwind(r, g_stack_copy, len, frames, 48) : (frames[0] = r.Rip, 1);
    for (int i = 0; i < n; ++i) PutFrame(i, frames[i], lookups);
  }
  Section();

  PutThreads(tid);
  Section();

  // r74 M3 / r75: an in-process minidump can still hang (dbghelp suspends threads; one may take the heap lock after
  // any probe), so COGBox writes it from outside the game when it is there; else the game, after the lock probes.
  Put("minidump: ");
  name[22] = L'd'; name[23] = L'm'; name[24] = L'p';
  InterlockedExchange(&g_dumping, 1);  // r76 L1: the crashing thread's longer wait covers the whole step
  const int launcher = g_minidump_mode == 0 && !g_wer_here ? AskLauncher(name, tid) : kNoLauncher;
  if (g_minidump_mode == 2 || (g_minidump_mode == 0 && g_wer_here)) {
    if (g_minidump_mode == 2) Put("not written by the game (--gow2_crash_minidump=windows); ");
    PutWerDump();
  } else if (launcher == kLauncherWritten) {
    Put("done (written by COGBox)\n");
  } else if (launcher == kLauncherStuck) {
    Put("not finished after 120 s (the .dmp may be cut short)\n");
  } else if (!HeapLockFree() || !LoaderLockFree()) {
    Put("not written - the process heap lock or the loader lock stays taken by a thread (a minidump made inside the "
        "game would hang here); ");
    PutWerDump();
  } else {
    Put(launcher == kLauncherFailed ? "the game writes it itself ... " : "writing (by the game) ... ");
    Section();  // if it hangs, the report says where
    HANDLE h;
    if (OpenNt(name, false, false, false, &h)) {
      MINIDUMP_EXCEPTION_INFORMATION mei{tid, &g_ep_copy, FALSE};
      const BOOL ok = MiniDumpWriteDump(
          GetCurrentProcess(), GetCurrentProcessId(), h,
          static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &mei, nullptr,
          nullptr);
      const DWORD err = GetLastError();
      CloseHandle(h);
      if (ok) {
        Put("done\n");
      } else {
        Put("failed, error ");
        PutHex(err);
        Put("\n");
      }
    } else {
      Put("the file could not be made\n");
    }
  }
  InterlockedExchange(&g_dumping, 0);
  Section();
  if (g_txt != INVALID_HANDLE_VALUE) CloseHandle(g_txt);
  g_txt = INVALID_HANDLE_VALUE;
}

DWORD WINAPI HelperMain(void*) {
  WaitForSingleObject(g_go, INFINITE);
  WriteReport(g_ep, g_crash_tid);
  SetEvent(g_done);
  return 0;
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
  // Nothing on the crashing thread's stack beyond this: it may have overflowed.
  const DWORD me = GetCurrentThreadId();
  if (me == g_helper_tid || me == g_prober_tid) return EXCEPTION_CONTINUE_SEARCH;  // the report itself failed
  const LONG owner = InterlockedCompareExchange(&g_owner, static_cast<LONG>(me), 0);
  if (owner != 0) {
    // a report is being written: another thread's crash - let it finish before Windows ends the process. This
    // thread's own (a fail-fast after its crash, after its wait ended): no second wait.
    if (owner != static_cast<LONG>(me) && g_done) WaitForSingleObject(g_done, kReportWaitMs);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (g_helper) {
    g_ep = ep;
    g_crash_tid = me;
    SetEvent(g_go);
    // bounded: then Windows Error Reporting takes over (longer while a minidump is still being written)
    const ULONGLONG t0 = GetTickCount64();
    while (WaitForSingleObject(g_done, 1000) == WAIT_TIMEOUT) {
      const ULONGLONG ms = GetTickCount64() - t0;
      if (ms >= kReportWaitMs && (!g_dumping || ms >= ULONGLONG(kReportWaitMs) + kDumpWaitMs)) break;
    }
  } else {
    WriteReport(ep, me);  // the helper couldn't be made at start
    if (g_done) SetEvent(g_done);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// P0-2: the CRT's fail-fast paths (an invalid parameter to a CRT function, a pure virtual call, abort() /
// std::terminate) end the process with 0xC0000409 without an exception reaching OnCrash - no report, no dump. Each of
// them now writes the same report from the current context first. (A real __fastfail is caught by nobody in-process;
// the launcher registers Windows Error Reporting's LocalDumps for the game exes for that.)
void ReportHere(DWORD code, const char* why) {
  alignas(16) CONTEXT ctx{};
  RtlCaptureContext(&ctx);
  EXCEPTION_RECORD er{};
  er.ExceptionCode = code;
  er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
  er.NumberParameters = 2;
  er.ExceptionInformation[0] = reinterpret_cast<ULONG_PTR>(why);
  er.ExceptionInformation[1] = kReasonMark;
  EXCEPTION_POINTERS ep{&er, &ctx};
  OnCrash(&ep);
  // the reason, next to the report (whose first line says 0xC0000409; it now has a "fail-fast:" line too)
  HANDLE h;
  if (OpenNt(L"crash-reason.txt", false, true, true, &h)) {
    DWORD done = 0;
    WriteFile(h, "fail-fast: ", 11, &done, nullptr);
    WriteFile(h, why, static_cast<DWORD>(std::strlen(why)), &done, nullptr);
    WriteFile(h, "\r\n", 2, &done, nullptr);
    CloseHandle(h);
  }
}
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
  ReportHere(kFailFast, "invalid parameter to a CRT function");
  std::abort();
}
void OnPureCall() {
  ReportHere(kFailFast, "pure virtual function call");
  std::abort();
}
void OnAbort(int) { ReportHere(kFailFast, "abort()"); }
void OnTerminate() {
  ReportHere(kFailFast, "std::terminate (an exception nobody caught, or noexcept)");
  std::abort();
}

// Stack overflow: Windows keeps this much of each thread's stack back for the exception's handling. Only for threads
// with a normal-sized stack (the exe's 1 MB default or more): small ones keep all of theirs.
volatile LONG g_guarantee_bytes = 0;
void ApplyStackGuarantee() {
  ULONG want = static_cast<ULONG>(g_guarantee_bytes);
  if (!want) return;
  const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  MEMORY_BASIC_INFORMATION mi;
  if (!VirtualQuery(static_cast<const char*>(tib->StackBase) - 1, &mi, sizeof(mi))) return;
  if (static_cast<const char*>(tib->StackBase) - static_cast<const char*>(mi.AllocationBase) < 512 * 1024) return;
  SetThreadStackGuarantee(&want);
}
// Runs at process attach, BEFORE cpu_check.cpp's AVX2 check (init_seg(compiler)), and this file is built for the game's CPU level
// (x86-64-v3): keep the process-attach path to a plain compare - no work that the compiler could vectorise.
void NTAPI CrashTlsCallback(PVOID, DWORD reason, PVOID) {
  if (reason == DLL_THREAD_ATTACH) ApplyStackGuarantee();
}

// An HKLM LocalDumps key for this exe pointing at the crash folder. Only HKLM: Microsoft documents every LocalDumps
// value as "not supported in the HKEY_CURRENT_USER registry hive" (review r75; COGBox's HKCU key is ignored here).
// Read at start: the registry isn't touched at crash time.
bool WerDumpsHere(const std::wstring& exe_name, const std::wstring& folder) {
  const std::wstring key = L"Software\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\" + exe_name;
  for (HKEY root : {HKEY_LOCAL_MACHINE}) {
    wchar_t v[kPath];
    DWORD bytes = sizeof(v);
    if (RegGetValueW(root, key.c_str(), L"DumpFolder", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, v, &bytes) !=
        ERROR_SUCCESS)
      continue;
    std::wstring d = v;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    for (wchar_t& c : d) if (c == L'/') c = L'\\';
    if (_wcsicmp(d.c_str(), folder.c_str()) == 0) return true;
  }
  return false;
}

// --gow2_crash_test
char g_test[16];
int (*volatile g_recurse)(int);
int Recurse(int depth) {
  volatile char pad[4096];
  pad[0] = static_cast<char>(depth);
  return g_recurse(depth + 1) + pad[0];
}
DWORD WINAPI CrashTestMain(void*) {
  Sleep(10000);
  if (!std::strcmp(g_test, "stack")) {
    g_recurse = Recurse;
    return static_cast<DWORD>(g_recurse(0));
  }
  if (!std::strcmp(g_test, "abort")) std::abort();
  if (!std::strcmp(g_test, "heaplock")) HeapLock(GetProcessHeap());  // held through the crash below
  *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10)) = 1;  // "av" (and "heaplock")
  return 0;
}

}  // namespace

// The TLS callback that gives each new thread its stack guarantee (DLL_THREAD_ATTACH reaches the exe's callbacks).
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:xenos_crash_tls_callback")
#pragma section(".CRT$XLB", read)
extern "C" __declspec(allocate(".CRT$XLB")) const PIMAGE_TLS_CALLBACK xenos_crash_tls_callback = CrashTlsCallback;

void XenosCrashNote(const std::string& note) {
  const LONG i = InterlockedIncrement(&g_note_count) - 1;
  if (i < 0 || i >= kNotes) return;
  strncpy_s(g_notes[i], kNoteLen, note.c_str(), _TRUNCATE);
}

void XenosInstallCrashHandler() {
  const std::string dir = REXCVAR_GET(gow2_crash_dir);
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  // r74 M4: cvars are UTF-8 (a non-ASCII play folder was mangled through the ANSI code page)
  const std::filesystem::path folder =
      dir.empty() ? std::filesystem::path(exe).parent_path() / L"crashes" : std::filesystem::u8path(dir);
  // made now, used at crash time without allocating: the folder as an absolute NT path, the exe's name and base
  std::error_code ec;
  std::wstring abs = std::filesystem::absolute(folder, ec).wstring();
  if (ec) abs = folder.wstring();
  for (wchar_t& c : abs) if (c == L'/') c = L'\\';
  while (!abs.empty() && abs.back() == L'\\') abs.pop_back();  // r74 L3: also a drive root ("C:\" -> "C:")
  std::wstring nt;
  if (abs.rfind(L"\\\\?\\", 0) == 0 || abs.rfind(L"\\??\\", 0) == 0) nt = L"\\??\\" + abs.substr(4);
  else if (abs.rfind(L"\\\\", 0) == 0) nt = L"\\??\\UNC\\" + abs.substr(2);
  else nt = L"\\??\\" + abs;
  g_nt_dir_len = nt.size() < kPath ? nt.size() : kPath - 1;
  wmemcpy(g_nt_dir, nt.c_str(), g_nt_dir_len);
  const wchar_t* slash = wcsrchr(exe, L'\\');
  const std::wstring exe_name = slash ? slash + 1 : exe;
  WideCharToMultiByte(CP_UTF8, 0, exe_name.c_str(), -1, g_exe_name, sizeof(g_exe_name), nullptr, nullptr);
  g_exe_base = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    g_nt_create_file = reinterpret_cast<NtCreateFileFn>(GetProcAddress(ntdll, "NtCreateFile"));
    g_nt_query_system = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    g_wine = GetProcAddress(ntdll, "wine_get_version") != nullptr;
  }
  g_wer_here = !g_wine && WerDumpsHere(exe_name, abs);
  const std::string mode = REXCVAR_GET(gow2_crash_minidump);
  g_minidump_mode = mode == "game" ? 1 : mode == "windows" ? 2 : 0;
  if (mode != "auto" && mode != "game" && mode != "windows")
    REXLOG_WARN("gow2_crash_minidump: '{}' isn't one of auto, game, windows - auto", mode);
  {  // r75: the dump request block and events for COGBox (xenos/crash_dump_ipc.h); CreateEvent opens COGBox's
    namespace ipc = xenos_crash_ipc;
    const DWORD pid = GetCurrentProcessId();
    wchar_t nm[64];
    ipc::Name(nm, 64, pid, L"info");
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ipc::CrashDumpInfo), nm);
    if (map && GetLastError() == ERROR_ALREADY_EXISTS) {
      // r76 M1: only the game makes this block (COGBox makes the events), so someone else's: not used - in-process
      CloseHandle(map);
      map = nullptr;
      REXLOG_WARN("crash report: {} already existed - the minidump is written by the game itself", "the dump request block");
    }
    if (map)
      g_ipc_info = static_cast<ipc::CrashDumpInfo*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::CrashDumpInfo)));
    ipc::Name(nm, 64, pid, L"req");
    g_ipc_req = CreateEventW(nullptr, FALSE, FALSE, nm);
    ipc::Name(nm, 64, pid, L"ack");
    g_ipc_ack = CreateEventW(nullptr, TRUE, FALSE, nm);
    ipc::Name(nm, 64, pid, L"done");
    g_ipc_done = CreateEventW(nullptr, TRUE, FALSE, nm);
    // r76 L2: the parent (COGBox when it started the game), checked to be older than the game (a reused pid isn't)
    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    struct Pbi {
      NTSTATUS exit_status;
      PVOID peb;
      ULONG_PTR affinity;
      LONG base_priority;
      ULONG_PTR pid, parent_pid;
    } pbi{};
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = ntdll ? reinterpret_cast<NtQueryInformationProcessFn>(
                             reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationProcess")))
                       : nullptr;
    if (query && query(GetCurrentProcess(), 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), nullptr) >= 0 &&
        pbi.parent_pid) {
      if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pbi.parent_pid))) {
        FILETIME pc{}, me{}, x{}, y{}, z{}, k{};
        if (GetProcessTimes(h, &pc, &x, &y, &z) && GetProcessTimes(GetCurrentProcess(), &me, &x, &y, &k) &&
            CompareFileTime(&pc, &me) <= 0)
          g_parent = h;
        else
          CloseHandle(h);
      }
    }
  }

  g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_probe_go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_probe_ok = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_go && g_done) g_helper = CreateThread(nullptr, 256 * 1024, HelperMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_helper_tid);
  if (g_probe_go && g_probe_ok) {
    if (HANDLE t = CreateThread(nullptr, 64 * 1024, ProberMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_prober_tid)) CloseHandle(t);
    else g_probe_go = nullptr;  // no probe: no in-process minidump
  }

  const int kb = REXCVAR_GET(gow2_crash_stack_guarantee_kb);
  g_guarantee_bytes = kb > 0 && kb <= 1024 ? kb * 1024 : 0;
  ApplyStackGuarantee();  // this thread; later ones get it from the TLS callback

  SetUnhandledExceptionFilter(OnCrash);
  _set_invalid_parameter_handler(OnInvalidParameter);  // P0-2
  _set_purecall_handler(OnPureCall);
  std::signal(SIGABRT, OnAbort);
  std::set_terminate(OnTerminate);

  const std::string test = REXCVAR_GET(gow2_crash_test);
  if (test == "av" || test == "stack" || test == "abort" || test == "heaplock") {
    strncpy_s(g_test, sizeof(g_test), test.c_str(), _TRUNCATE);
    REXLOG_WARN("gow2_crash_test: a test crash ({}) in 10 s", test);
    if (HANDLE t = CreateThread(nullptr, 0, CrashTestMain, nullptr, 0, nullptr)) CloseHandle(t);
  } else if (!test.empty()) {
    REXLOG_WARN("gow2_crash_test: '{}' isn't one of av, stack, abort, heaplock - no test crash", test);
  }
}
