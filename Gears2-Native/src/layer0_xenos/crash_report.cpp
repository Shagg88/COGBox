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
//   - a helper thread made at start writes the .txt with NtCreateFile / WriteFile through to the disk (no CRT, no heap,
//     no logger, no loader lock), section by section, so whatever it got to is on the disk;
//   - it lists every other thread's state, wait reason, CPU time and stack (suspended one at a time);
//   - only then the minidump - and only when a probe thread can still take the process heap and the loader lock,
//     else the .txt says so and the crash goes on to Windows Error Reporting, whose LocalDumps (set by COGBox for this
//     play's folder) writes its dump from outside the process.
#include <xenos/dev_tools.h>
#include <rex/cvar.h>

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
REXCVAR_DEFINE_STRING(gow2_crash_test, "", "Gears2",
                      "Test the crash report 10 s after start: av (access violation), stack (stack overflow), abort, "
                      "heaplock (crash while holding the process heap lock: the report must still be written, the "
                      "minidump left to Windows). Empty = off");

namespace {

using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                        ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
NtCreateFileFn g_nt_create_file = nullptr;
NtQuerySystemInformationFn g_nt_query_system = nullptr;

constexpr int kPath = 1024;
wchar_t g_nt_dir[kPath];  // \??\<absolute crash folder>
size_t g_nt_dir_len = 0;
char g_exe_name[MAX_PATH];
DWORD64 g_exe_base = 0;

// XenosCrashNote: fixed storage, so the crash handler reads it without allocating.
constexpr int kNotes = 16, kNoteLen = 240;
char g_notes[kNotes][kNoteLen];
volatile LONG g_note_count = 0;

// ReportHere's reason travels in ExceptionInformation[0], marked by this in [1].
constexpr ULONG_PTR kReasonMark = 0x6F77326661696C;
constexpr DWORD kFailFast = 0xC0000409;  // STATUS_STACK_BUFFER_OVERRUN, the code the CRT's fail-fast paths end with
constexpr DWORD kReportWaitMs = 30000;    // the crashing thread waits at most this long for the report
constexpr DWORD kProbeWaitMs = 2000;      // heap + loader lock probe before the minidump

HANDLE g_go = nullptr, g_done = nullptr, g_probe_go = nullptr, g_probe_ok = nullptr, g_helper = nullptr;
DWORD g_helper_tid = 0, g_prober_tid = 0;
EXCEPTION_POINTERS* volatile g_ep = nullptr;
volatile DWORD g_crash_tid = 0;
volatile LONG g_owner = 0;  // the thread whose crash is being reported

// ---- the text, written through to the disk (no CRT, no heap) ----------------------------------------------------
char g_text[32768];
size_t g_len = 0;
HANDLE g_txt = INVALID_HANDLE_VALUE;

void Write() {
  if (g_txt != INVALID_HANDLE_VALUE && g_len) {
    DWORD done = 0;
    WriteFile(g_txt, g_text, static_cast<DWORD>(g_len), &done, nullptr);
  }
  g_len = 0;
}
void Section() {  // what the report has so far is on the disk
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
  if (name) {
    p[n++] = L'\\';
    for (; *name && n < kPath + 62; ++name) p[n++] = *name;
  }
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

// ---- stacks (lock-free: no loader calls, the module name comes from the image's own export directory) ------------
bool Readable(DWORD64 a, size_t n) {
  MEMORY_BASIC_INFORMATION mi;
  if (!VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return false;
  if (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  return a + n <= reinterpret_cast<DWORD64>(mi.BaseAddress) + mi.RegionSize;
}
const char* ImageName(void* base) {
  if (reinterpret_cast<DWORD64>(base) == g_exe_base) return g_exe_name;
  const auto* b = static_cast<const char*>(base);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return "?";
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return "?";
  const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!d.VirtualAddress || !d.Size) return "?";
  const auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(b + d.VirtualAddress);
  return ex->Name ? b + ex->Name : "?";
}
void PutFrame(int i, DWORD64 rip) {
  void* base = nullptr;
  RtlPcToFileHeader(reinterpret_cast<void*>(rip), &base);
  Put("  #");
  if (i < 10) Put("0");
  PutDec(i);
  Put(" ");
  Put(base ? ImageName(base) : "?");
  Put(" ");
  PutHex(rip - reinterpret_cast<DWORD64>(base));
  Put("\n");
}
int Walk(CONTEXT* c, DWORD64* out, int max) {
  int n = 0;
  while (n < max && c->Rip) {
    out[n++] = c->Rip;
    const DWORD64 rsp = c->Rsp;
    if (!Readable(rsp, 64)) break;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c->Rip, &image_base, nullptr);
    if (fn) {
      PVOID handler_data = nullptr;
      DWORD64 frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c->Rip, fn, c, &handler_data, &frame, nullptr);
    } else {
      c->Rip = *reinterpret_cast<DWORD64*>(rsp);
      c->Rsp += 8;
    }
    if (c->Rsp <= rsp) break;  // a damaged stack: stop instead of reading garbage
  }
  return n;
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
  Put("threads (where each one was; suspended one at a time to read its stack):\n");
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
        DWORD64 frames[16];
        int n = 0;
        if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid)) {
          if (SuspendThread(h) != static_cast<DWORD>(-1)) {
            alignas(16) CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &c)) n = Walk(&c, frames, 16);
            ResumeThread(h);
          }
          CloseHandle(h);
        }
        for (int f = 0; f < n; ++f) PutFrame(f, frames[f]);
      }
    }
    if (!next) break;
    p += next;
  }
  VirtualFree(buf, 0, MEM_RELEASE);
}

// The minidump only when another thread can still take the process heap and the loader lock: dbghelp needs both,
// and with either held for good (a frozen thread, or the crash itself inside the heap) it would hang the game.
bool LocksFree() {
  if (!g_probe_go) return false;
  ResetEvent(g_probe_ok);
  SetEvent(g_probe_go);
  return WaitForSingleObject(g_probe_ok, kProbeWaitMs) == WAIT_OBJECT_0;
}
DWORD WINAPI ProberMain(void*) {
  for (;;) {
    WaitForSingleObject(g_probe_go, INFINITE);
    HANDLE heap = GetProcessHeap();
    HeapFree(heap, 0, HeapAlloc(heap, 0, 64));
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);  // walks the loader's module list under the loader lock
    SetEvent(g_probe_ok);
  }
}

void WriteReport(EXCEPTION_POINTERS* ep, DWORD tid) {
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

  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  Put("exception ");
  PutHex(er->ExceptionCode);
  Put(" at ");
  PutHex(reinterpret_cast<DWORD64>(er->ExceptionAddress));
  Put(" thread ");
  PutDec(tid);
  Put("\n");
  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
    const ULONG_PTR k = er->ExceptionInformation[0];
    Put("access violation: ");
    Put(k == 0 ? "read" : k == 1 ? "write" : "execute");
    Put(" address ");
    PutHex(er->ExceptionInformation[1]);
    Put("\n");
  }
  if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW) Put("stack overflow\n");
  if (er->NumberParameters == 2 && er->ExceptionInformation[1] == kReasonMark) {
    Put("fail-fast: ");
    Put(reinterpret_cast<const char*>(er->ExceptionInformation[0]));
    Put("\n");
  }
  const CONTEXT& r = *ep->ContextRecord;
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
  Put("stack (module offset):\n");
  {
    alignas(16) CONTEXT c = r;
    DWORD64 frames[48];
    const int n = Walk(&c, frames, 48);
    for (int i = 0; i < n; ++i) PutFrame(i, frames[i]);
  }
  Section();

  PutThreads(tid);
  Section();

  Put("minidump: ");
  if (!LocksFree()) {
    Put("not written - the process heap or the loader lock stays taken by another thread (a minidump made inside "
        "the game would hang here); Windows Error Reporting writes ");
    Put(g_exe_name);
    Put(".");
    PutDec(GetCurrentProcessId());
    Put(".dmp in this folder when it is allowed to\n");
    Section();
  } else {
    name[22] = L'd'; name[23] = L'm'; name[24] = L'p';
    Put("writing ... ");
    Section();  // if it hangs, the report says where
    HANDLE h;
    if (OpenNt(name, false, false, false, &h)) {
      MINIDUMP_EXCEPTION_INFORMATION mei{tid, ep, FALSE};
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
    Section();
  }
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
  if (InterlockedCompareExchange(&g_owner, static_cast<LONG>(me), 0) != 0) {
    // a report is being written (another thread's crash, or this thread's fail-fast after its crash): let it finish
    // before Windows ends the process
    if (g_done) WaitForSingleObject(g_done, kReportWaitMs);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (g_helper) {
    g_ep = ep;
    g_crash_tid = me;
    SetEvent(g_go);
    WaitForSingleObject(g_done, kReportWaitMs);  // bounded: then Windows Error Reporting takes over
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
void NTAPI CrashTlsCallback(PVOID, DWORD reason, PVOID) {
  if (reason == DLL_THREAD_ATTACH) ApplyStackGuarantee();
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
  std::string dir = REXCVAR_GET(gow2_crash_dir);
  std::wstring folder;
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (dir.empty()) {
    folder = (std::filesystem::path(exe).parent_path() / L"crashes").wstring();
  } else {
    folder = std::filesystem::path(dir).wstring();
  }
  // made now, used at crash time without allocating: the folder as an absolute NT path, the exe's name and base
  std::error_code ec;
  std::wstring abs = std::filesystem::absolute(folder, ec).wstring();
  if (ec) abs = folder;
  while (abs.size() > 3 && (abs.back() == L'\\' || abs.back() == L'/')) abs.pop_back();
  std::wstring nt;
  if (abs.rfind(L"\\\\?\\", 0) == 0 || abs.rfind(L"\\??\\", 0) == 0) nt = L"\\??\\" + abs.substr(4);
  else if (abs.rfind(L"\\\\", 0) == 0) nt = L"\\??\\UNC\\" + abs.substr(2);
  else nt = L"\\??\\" + abs;
  for (wchar_t& c : nt) if (c == L'/') c = L'\\';
  g_nt_dir_len = nt.size() < kPath ? nt.size() : kPath - 1;
  wmemcpy(g_nt_dir, nt.c_str(), g_nt_dir_len);
  const wchar_t* slash = wcsrchr(exe, L'\\');
  WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : exe, -1, g_exe_name, sizeof(g_exe_name), nullptr, nullptr);
  g_exe_base = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    g_nt_create_file = reinterpret_cast<NtCreateFileFn>(GetProcAddress(ntdll, "NtCreateFile"));
    g_nt_query_system = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
  }
  g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_probe_go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_probe_ok = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_go && g_done) g_helper = CreateThread(nullptr, 256 * 1024, HelperMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_helper_tid);
  if (g_probe_go && g_probe_ok) {
    if (HANDLE t = CreateThread(nullptr, 64 * 1024, ProberMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g_prober_tid)) CloseHandle(t);
    else g_probe_go = nullptr;  // no probe: no in-process minidump (Windows Error Reporting's instead)
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
  if (!test.empty()) {
    strncpy_s(g_test, sizeof(g_test), test.c_str(), _TRUNCATE);
    if (HANDLE t = CreateThread(nullptr, 0, CrashTestMain, nullptr, 0, nullptr)) CloseHandle(t);
  }
}
