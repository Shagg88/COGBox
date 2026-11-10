// Layer 3 (Judgment): GitHub #219 / #185 - freeze watchdog. Read-only, log-only: it never changes what the game does.
//
// A background thread looks at the swap count (layer 0's observer hub, counted for both GPU paths) four times a
// second. When no frame has been presented for --gowj_freeze_watchdog_seconds (default 10, 0 = off) after the first
// frame, it logs ONE "gowj_freeze:" report for that stall, and one more at 4x the limit, so the two can be compared:
// a long load shows moving stacks, a deadlock shows the same ones. When frames come back it says how long the stall was.
// Each report has, for every guest thread (XThread): its id, name, priority, running / suspend state, the PPC context's
// lr, r1 (stack), r3 / r4 (the first arguments: for a thread inside KeWaitForSingleObject / RtlEnterCriticalSection
// etc. that is usually the object it waits on) and a guest call stack from the r1 back-chain (saved LR at
// back-chain - 8). Recompiled code has no guest PC, and its registers may be held in host locals, so lr / r1 are as of
// the thread's last call that went through the context - the host stack below is the precise one.
// On Windows, every thread of the process is then suspended for a moment, its host stack unwound (no allocation while
// suspended, like layer 0's profiler) and resumed, and the frames are printed with PDB names (sub_8XXXXXXX for
// recompiled guest functions, the SDK's kernel exports such as KeWaitForSingleObject for what it is blocked on).
//
// Scope (Shagg, 0.4.8): the #219 freeze - back to the main menu, then loading a checkpoint / chapter, the picture stops
// while the music plays. So the full report (and the brief thread suspension it needs) is only made for a stall that
// begins within kLoadWindow of a loading screen or level change (GowjLastLevelLoadTicks: the loading movie's subtitle
// file or a *_P.xxx being opened - the menu map and every checkpoint load open one). Any other stall gets one log line.
// Nothing here acts on the game: no thread is killed, no state is changed, the process is never ended - a misfire on a
// long load (or during a save) costs one log report and a few milliseconds of every thread being paused in turn.
// Game cost: none per frame beyond the relaxed fetch_add the observer hub already does on each swap; this thread sleeps
// on a condition variable and reads that counter four times a second.
#include "freeze_watchdog.h"

#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <spdlog/fmt/fmt.h>  // fmt::format (the SDK's rex/logging/format.h uses the same header)
#include <rex/logging/macros.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_INT32(gowj_freeze_watchdog_seconds, 10, "Judgment",
                     "GitHub #219: when no frame has been presented for this many seconds after a loading screen / "
                     "level change, log every guest thread's state and every host thread's stack (read-only, log-only; "
                     "may also fire on a long load). 0 = off");

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

int64_t GowjLastLevelLoadTicks();  // game_patches.cpp (0 = none seen)

namespace {

// A stall that starts this long after the last loading screen / level change still counts as #219's case (the reported
// freeze comes as the checkpoint starts loading, or on the menu right after it was loaded).
constexpr std::chrono::seconds kLoadWindow{120};

std::thread g_thread;
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;
std::atomic<bool> g_stopping{false};  // read inside Dump so a shutdown mid-report does not wait for it
std::mutex g_sym_mutex;               // DbgHelp is single-threaded

uint32_t Load32Raw(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Guest stack read; 0 (ends the walk) if the page is not readable. Checked with VirtualQuery first (1388 log: ~20
// "Unhandled guest access violation: read of guest 0x7003..." from this thread - the runtime's vectored exception
// handler sees a fault before __try does, and the top of an idle thread's guest stack is not committed).
bool Readable(const void* p, size_t n) {
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION mi{};
  if (!VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return false;
  const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                   PAGE_EXECUTE_WRITECOPY;
  if (!(mi.Protect & ok) || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
  const auto* end = static_cast<const uint8_t*>(mi.BaseAddress) + mi.RegionSize;
  return static_cast<const uint8_t*>(p) + n <= end;
#else
  (void)p, (void)n;
  return true;
#endif
}

uint32_t Load32(const uint8_t* base, uint32_t a) {
  if (!Readable(base + a, 4)) return 0;
#if defined(_MSC_VER)
  __try {
    return Load32Raw(base, a);
  } __except (1) {
    return 0;
  }
#else
  return Load32Raw(base, a);
#endif
}

#if defined(_WIN32)
constexpr int kHostFrames = 24;

constexpr size_t kStackCopy = 256 * 1024;

// Snapshot of a suspended thread: its CONTEXT and the top of its stack. Taken while the thread is suspended with
// only syscalls (GetThreadContext, VirtualQuery, ReadProcessMemory): no loader / function-table / heap lock, so a
// thread suspended while holding one of those cannot deadlock the watchdog. Unwinding happens after ResumeThread.
size_t SnapshotStack(HANDLE h, CONTEXT* ctx, uint8_t* buf) {
  ctx->ContextFlags = CONTEXT_FULL;
  if (!GetThreadContext(h, ctx) || !ctx->Rsp) return 0;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<const void*>(ctx->Rsp), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return 0;
  const uint64_t end = uint64_t(mbi.BaseAddress) + mbi.RegionSize;
  size_t len = size_t(end - ctx->Rsp);
  if (len > kStackCopy) len = kStackCopy;
  SIZE_T got = 0;
  ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ctx->Rsp), buf, len, &got);  // partial is fine
  return size_t(got);
}

// Unwinds a snapshot (thread already resumed): stack registers are rebased into the copy, and the walk stops when
// RSP leaves it. Return addresses are code addresses and need no rebasing.
int Unwind(CONTEXT ctx, const uint8_t* stack, size_t len, uint64_t* pc) {
  int n = 0;
  if (len < 8) {
    if (ctx.Rip) pc[n++] = ctx.Rip;
    return n;
  }
  const DWORD64 lo = ctx.Rsp, hi = ctx.Rsp + len, base = DWORD64(stack);
  auto rebase = [&] {  // registers restored from the copy hold live-stack addresses: map them into the copy again
    for (DWORD64* r : {&ctx.Rbp, &ctx.Rbx, &ctx.Rsi, &ctx.Rdi, &ctx.R12, &ctx.R13, &ctx.R14, &ctx.R15})
      if (*r >= lo && *r < hi) *r = *r - lo + base;
  };
  ctx.Rsp = base;
  rebase();
  const DWORD64 copy_end = base + len;
#if defined(_MSC_VER)
  __try {
#endif
  while (n < kHostFrames && ctx.Rip && ctx.Rsp >= base && ctx.Rsp + 8 <= copy_end) {
    pc[n++] = ctx.Rip;
    const DWORD64 old_rsp = ctx.Rsp;
    DWORD64 image = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
    if (!fn) {  // leaf function: the return address is at the top of the stack
      ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    PVOID handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
    rebase();
    if (ctx.Rsp <= old_rsp) break;  // no progress: a broken frame
  }
#if defined(_MSC_VER)
  } __except (1) {  // bad stack memory: keep the frames we have
  }
#endif
  return n;
}

std::string Symbol(uint64_t a) {
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
  auto* s = reinterpret_cast<SYMBOL_INFO*>(buf);
  s->SizeOfStruct = sizeof(SYMBOL_INFO);
  s->MaxNameLen = 255;
  DWORD64 disp = 0;
  std::lock_guard<std::mutex> lock(g_sym_mutex);
  if (SymFromAddr(GetCurrentProcess(), a, &disp, s)) return std::string(s->Name, s->NameLen) + "+0x" + fmt::format("{:X}", disp);
  return fmt::format("0x{:X}", a);
}

std::string HostThreadName(HANDLE h) {
  std::string out;
  PWSTR w = nullptr;
  if (SUCCEEDED(GetThreadDescription(h, &w)) && w) {
    for (const wchar_t* p = w; *p; ++p) out.push_back(*p < 128 ? char(*p) : '?');
    LocalFree(w);
  }
  return out;
}

void DumpHostStacks(const std::unordered_map<uint32_t, uint32_t>& guest_of_host) {
  if (g_stopping.load()) return;
  static bool sym_ready = false;
  if (!sym_ready) {
    std::lock_guard<std::mutex> lock(g_sym_mutex);
    SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);  // may already be initialised (crash handler / profiler): fine
    sym_ready = true;
  }
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  std::vector<DWORD> tids;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
  CloseHandle(snap);
  REXLOG_INFO("gowj_freeze: {} host threads:", tids.size());
  for (DWORD tid : tids) {
    if (g_stopping.load()) return;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h) continue;
    uint64_t pc[kHostFrames];
    int n = 0;
    static uint8_t stack_copy[kStackCopy];  // watchdog thread only
    if (SuspendThread(h) != DWORD(-1)) {
      CONTEXT ctx{};
      const size_t len = SnapshotStack(h, &ctx, stack_copy);  // syscalls only while suspended
      ResumeThread(h);
      if (ctx.Rip) n = Unwind(ctx, stack_copy, len, pc);  // RtlLookupFunctionEntry etc. after resume
    }
    auto g = guest_of_host.find(uint32_t(tid));
    std::string head = fmt::format("gowj_freeze: host {} '{}'", tid, HostThreadName(h));
    if (g != guest_of_host.end()) head += fmt::format(" (guest thread {:X})", g->second);
    CloseHandle(h);
    std::string frames;
    for (int i = 0; i < n; ++i) frames += fmt::format("\n    #{} {}", i, Symbol(i ? pc[i] - 1 : pc[i]));
    REXLOG_INFO("{}:{}", head, n ? frames : std::string(" (no stack)"));
  }
}
#endif

void Dump(double seconds, uint64_t swaps, bool second) {
  REXLOG_WARN("gowj_freeze: no frame presented for {:.1f} s (swap {}) - may be a long load, or a freeze; {}", seconds,
              swaps,
              second ? "second report: threads whose stacks did not change since the first report are the stuck ones"
                     : "thread report follows (read-only; a second one follows if it lasts 4x longer)");
  std::unordered_map<uint32_t, uint32_t> guest_of_host;
  auto* ks = rex::system::kernel_state();
  if (ks && ks->memory()) {
    const uint8_t* base = ks->memory()->virtual_membase();
    auto threads = ks->object_table()->GetObjectsByType<rex::system::XThread>();
    REXLOG_INFO("gowj_freeze: {} guest threads:", threads.size());
    for (auto& t : threads) {
      if (g_stopping.load()) return;
      if (!t) continue;
      uint32_t host_id = 0;
      if (auto* ht = t->thread()) host_id = ht->system_id();
      if (host_id) guest_of_host[host_id] = t->thread_id();
      std::string line = fmt::format("gowj_freeze: guest {:X} '{}' host {} prio {} {}{} suspend {}", t->thread_id(),
                                     t->name(), host_id, t->priority(), t->is_running() ? "running" : "not running",
                                     t->main_thread() ? " main" : "", t->suspend_count());
      auto* ts = t->thread_state();
      const PPCContext* c = ts ? ts->context() : nullptr;
      if (c) {
        line += fmt::format(" | lr {:08X} r1 {:08X} r3 {:08X} r4 {:08X} | guest stack:", uint32_t(c->lr), c->r1.u32,
                            c->r3.u32, c->r4.u32);
        uint32_t sp = c->r1.u32;
        for (int i = 0; i < 16; ++i) {
          if (sp < rex::system::XThread::kStackAddressRangeBegin || sp >= rex::system::XThread::kStackAddressRangeEnd - 4)
            break;
          const uint32_t next = Load32(base, sp);
          if (next <= sp || next >= rex::system::XThread::kStackAddressRangeEnd) break;
          line += fmt::format(" {:08X}", Load32(base, next - 8));
          sp = next;
        }
      }
      REXLOG_INFO("{}", line);
    }
  }
#if defined(_WIN32)
  // 0.4.8 AMD / SteamOS review: under Wine / Proton, SuspendThread works through signals and RtlLookupFunctionEntry
  // takes the loader lock, so a thread suspended while holding it (or inside the Unix side) would deadlock this thread
  // and leave the game thread suspended for good. The guest report above is enough there; skip the host stacks.
  static const bool wine = [] {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != nullptr;
  }();
  if (wine)
    REXLOG_INFO("gowj_freeze: host thread stacks skipped under Wine / Proton");
  else
    DumpHostStacks(guest_of_host);
#endif
  REXLOG_INFO("gowj_freeze: end of report");
}

void Run(int limit_s) {
  using clock = std::chrono::steady_clock;
  uint64_t last = XenosSwapCount();
  auto last_change = clock::now();
  int reported = 0;  // 0 none, 1 first, 2 second (for this stall)
  std::unique_lock<std::mutex> lock(g_mutex);
  while (!g_cv.wait_for(lock, std::chrono::milliseconds(250), [] { return g_stop; })) {
    const uint64_t now_swaps = XenosSwapCount();
    const auto now = clock::now();
    const double stalled = std::chrono::duration<double>(now - last_change).count();
    if (now_swaps != last) {
      if (reported) REXLOG_INFO("gowj_freeze: frames resumed after {:.1f} s (swap {} -> {})", stalled, last, now_swaps);
      last = now_swaps;
      last_change = now;
      reported = 0;
      continue;
    }
    if (!last) continue;  // not armed before the first frame
    if ((reported == 0 && stalled >= limit_s) || (reported == 1 && stalled >= 4.0 * limit_s)) {
      // #219 only: a loading screen / level change shortly before (or during) this stall
      const int64_t load = GowjLastLevelLoadTicks();
      const bool near_load =
          load != 0 && clock::time_point(clock::duration(load)) + kLoadWindow >= last_change;
      if (!near_load) {
        if (reported == 0)
          REXLOG_WARN("gowj_freeze: no frame presented for {:.1f} s (swap {}), not after a loading screen - not the "
                      "#219 case, no thread report", stalled, now_swaps);
        reported = 2;  // nothing more for this stall
        continue;
      }
      lock.unlock();
      Dump(stalled, now_swaps, reported == 1);
      lock.lock();
      ++reported;
    }
  }
}

}  // namespace

void GowjStartFreezeWatchdog() {
  const int limit = REXCVAR_GET(gowj_freeze_watchdog_seconds);
  if (limit <= 0 || g_thread.joinable()) return;
  XenosEnsureObserverHub();  // the swap count
  g_thread = std::thread(Run, limit);
  REXLOG_INFO("gowj_freeze: watchdog on ({} s without a frame; --gowj_freeze_watchdog_seconds=0 turns it off)", limit);
}

void GowjStopFreezeWatchdog() {
  if (!g_thread.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stop = true;
  }
  g_stopping.store(true);
  g_cv.notify_all();
  g_thread.join();
}
