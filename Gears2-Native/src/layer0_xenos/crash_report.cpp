// Layer 0 (game-free): crash report for unhandled host exceptions (roadmap section 13). Writes, into --gow2_crash_dir
// (default: <exe dir>\crashes), a text file with the exception and an unwound stack (module + offset per frame, for
// llvm-symbolizer --relative-address) and a small minidump. Only exceptions nobody else handled reach this: the
// runtime's own guest-memory fault handling runs first.
#include <xenos/dev_tools.h>
#include <rex/cvar.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <ctime>
#include <filesystem>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

REXCVAR_DEFINE_STRING(gow2_crash_dir, "", "Gears2", "Folder for crash reports (default: <exe dir>\\crashes)");

namespace {

std::wstring g_dir;

// XenosCrashNote: fixed storage, so the crash handler reads it without allocating.
constexpr int kNotes = 16, kNoteLen = 240;
char g_notes[kNotes][kNoteLen];
volatile LONG g_note_count = 0;

void ModuleOf(DWORD64 addr, wchar_t* name, size_t cap, DWORD64* base) {
  HMODULE m = nullptr;
  name[0] = 0; *base = 0;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(addr), &m) && m) {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(m, path, MAX_PATH)) {
      const wchar_t* slash = wcsrchr(path, L'\\');
      wcsncpy_s(name, cap, slash ? slash + 1 : path, _TRUNCATE);
    }
    *base = reinterpret_cast<DWORD64>(m);
  }
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
  static volatile LONG once = 0;
  if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
  CreateDirectoryW(g_dir.c_str(), nullptr);
  wchar_t stamp[64];
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &now);
  wcsftime(stamp, 64, L"%Y%m%d-%H%M%S", &tm);
  std::wstring base = g_dir + L"\\crash-" + stamp;

  FILE* f = nullptr;
  _wfopen_s(&f, (base + L".txt").c_str(), L"w");
  if (f) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    std::fprintf(f, "exception 0x%08lX at %p thread %lu\n", er->ExceptionCode, er->ExceptionAddress, GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
      std::fprintf(f, "access violation: %s address 0x%llX\n",
                   er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
                   static_cast<unsigned long long>(er->ExceptionInformation[1]));
    }
    CONTEXT c = *ep->ContextRecord;
    std::fprintf(f, "rax %llX rbx %llX rcx %llX rdx %llX rsi %llX rdi %llX rsp %llX rbp %llX\n", c.Rax, c.Rbx, c.Rcx,
                 c.Rdx, c.Rsi, c.Rdi, c.Rsp, c.Rbp);
    std::fprintf(f, "r8 %llX r9 %llX r10 %llX r11 %llX r12 %llX r13 %llX r14 %llX r15 %llX\n", c.R8, c.R9, c.R10,
                 c.R11, c.R12, c.R13, c.R14, c.R15);
    const LONG notes = g_note_count < kNotes ? g_note_count : kNotes;
    if (notes > 0) {
      std::fprintf(f, "notes:\n");
      for (LONG i = 0; i < notes; ++i) std::fprintf(f, "  %s\n", g_notes[i]);
    }
    std::fprintf(f, "stack (module offset):\n");
    for (int i = 0; i < 48 && c.Rip; ++i) {
      wchar_t mod[MAX_PATH];
      DWORD64 mbase = 0;
      ModuleOf(c.Rip, mod, MAX_PATH, &mbase);
      std::fprintf(f, "  #%02d %ls 0x%llX\n", i, mod[0] ? mod : L"?", static_cast<unsigned long long>(c.Rip - mbase));
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
      if (fn) {
        PVOID handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &frame, nullptr);
      } else {
        c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
        c.Rsp += 8;
      }
    }
    std::fclose(f);
  }

  HANDLE h = CreateFileW((base + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                      &mei, nullptr, nullptr);
    CloseHandle(h);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// P0-2: the CRT's fail-fast paths (an invalid parameter to a CRT function, a pure virtual call, abort() /
// std::terminate) end the process with 0xC0000409 without an exception reaching OnCrash - no report, no dump. Each of
// them now writes the same report from the current context first. (A real __fastfail is caught by nobody in-process;
// the launcher registers Windows Error Reporting's LocalDumps for the game exes for that.)
constexpr DWORD kFailFast = 0xC0000409;  // STATUS_STACK_BUFFER_OVERRUN, the code these paths end with
void ReportHere(DWORD code, const char* why) {
  CONTEXT ctx{};
  RtlCaptureContext(&ctx);
  EXCEPTION_RECORD er{};
  er.ExceptionCode = code;
  er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
  er.NumberParameters = 1;
  er.ExceptionInformation[0] = reinterpret_cast<ULONG_PTR>(why);
  EXCEPTION_POINTERS ep{&er, &ctx};
  OnCrash(&ep);
  // the reason, next to the report OnCrash wrote (its first line says 0xC0000409)
  const std::wstring note = g_dir + L"\\crash-reason.txt";
  FILE* f = nullptr;
  if (_wfopen_s(&f, note.c_str(), L"a") == 0 && f) {
    std::fprintf(f, "fail-fast: %s\n", why);
    std::fclose(f);
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

}  // namespace

void XenosCrashNote(const std::string& note) {
  const LONG i = InterlockedIncrement(&g_note_count) - 1;
  if (i < 0 || i >= kNotes) return;
  strncpy_s(g_notes[i], kNoteLen, note.c_str(), _TRUNCATE);
}

void XenosInstallCrashHandler() {
  std::string dir = REXCVAR_GET(gow2_crash_dir);
  if (dir.empty()) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = (std::filesystem::path(exe).parent_path() / L"crashes").wstring();
  } else {
    g_dir = std::filesystem::path(dir).wstring();
  }
  SetUnhandledExceptionFilter(OnCrash);
  _set_invalid_parameter_handler(OnInvalidParameter);  // P0-2
  _set_purecall_handler(OnPureCall);
  std::signal(SIGABRT, OnAbort);
  std::set_terminate(OnTerminate);
}
