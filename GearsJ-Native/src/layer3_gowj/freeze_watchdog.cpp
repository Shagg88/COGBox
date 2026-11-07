// Layer 3 (Judgment): GitHub #219 / #185 - freeze watchdog. Read-only, log-only: it never changes what the game does.
//
// A background thread looks at the swap count (layer 0's observer hub, counted for both GPU paths) four times a
// second. When no frame has been presented for --gowj_freeze_watchdog_seconds (default 0 = off; test with =8) after the first
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
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_INT32(gowj_freeze_watchdog_seconds, 0, "Judgment",
                     "GitHub #219: when no frame has been presented for this many seconds, log every guest thread's "
                     "state and every host thread's stack once per stall (read-only; may also fire on a long load). "
                     "0 = off (default; test .bat: =8)");

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

namespace {

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

// Guest stack read; 0 (ends the walk) if the page is not readable.
uint32_t Load32(const uint8_t* base, uint32_t a) {
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

int Unwind(CONTEXT ctx, uint64_t* pc) {  // nothing allocated: the target thread may be suspended holding the heap lock
  int n = 0;
#if defined(_MSC_VER)
  __try {
#endif
  while (n < kHostFrames && ctx.Rip && ctx.Rsp) {
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
    if (SuspendThread(h) != DWORD(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(h, &ctx)) n = Unwind(ctx, pc);
      ResumeThread(h);
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
