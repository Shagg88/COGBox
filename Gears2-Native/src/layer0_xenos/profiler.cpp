// Layer 0 (game-free): a built-in sampling profiler (notes/52 section 1). Our code (MIT).
// --gow2_profile=S: from swap --gow2_profile_start on, for S seconds, a sampler thread wakes every ~1 ms and, for each
// thread of the process that used CPU since its last look, suspends it, reads its registers, unwinds up to 24 frames
// (RtlLookupFunctionEntry / RtlVirtualUnwind; nothing is allocated while the target is suspended) and resumes it.
// At the end: per-thread CPU time over the window (GetThreadTimes) and, per busy thread, the top functions by own
// samples and by inclusive samples, names from the PDB (DbgHelp). Logged as "gow2_profile:" lines.
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_INT32(gow2_profile, 0, "Gears2", "Sampling profiler: profile S seconds of every busy thread (0 = off)");
REXCVAR_DEFINE_INT32(gow2_profile_start, 600, "Gears2", "Sampling profiler: start at this swap");
REXCVAR_DEFINE_INT32(gow2_profile_top, 15, "Gears2", "Sampling profiler: functions listed per thread");

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <timeapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace {

constexpr int kFrames = 24;

struct Range {
  uint64_t lo, hi;
};

struct Stack {
  uint64_t count = 0;
  uint8_t depth = 0;
  uint64_t pc[kFrames];
};

struct Tracked {
  DWORD tid = 0;
  HANDLE handle = nullptr;
  std::string name;
  ULONG64 last_cycles = 0;
  FILETIME k0{}, u0{};
  uint64_t samples = 0;
  std::unordered_map<uint64_t, Stack> stacks;  // by stack hash
};

std::vector<Range> g_modules;

bool InModule(uint64_t pc) {
  for (const Range& r : g_modules)
    if (pc >= r.lo && pc < r.hi) return true;
  return false;
}

void LoadModuleRanges() {
  g_modules.clear();
  HMODULE mods[1024];
  DWORD needed = 0;
  HANDLE process = GetCurrentProcess();
  if (!EnumProcessModules(process, mods, sizeof(mods), &needed)) return;
  const DWORD n = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
  for (DWORD i = 0; i < n; ++i) {
    MODULEINFO mi{};
    if (GetModuleInformation(process, mods[i], &mi, sizeof(mi)))
      g_modules.push_back({uint64_t(uintptr_t(mi.lpBaseOfDll)), uint64_t(uintptr_t(mi.lpBaseOfDll)) + mi.SizeOfImage});
  }
}

// One unwind step, SEH-guarded (no C++ objects in this function): RtlVirtualUnwind reads the target's stack and
// registers without checks, and a thread caught mid-prologue or on a bad frame made it read address 0x8 (job 220,
// both runs of the 218 build). A fault ends this sample's stack instead of the game.
bool UnwindStep(CONTEXT* ctx, uint64_t ip) {
  __try {
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ip, &image_base, nullptr);
    if (!fe) {  // leaf function: the return address is at the stack pointer
      ctx->Rip = *reinterpret_cast<const uint64_t*>(uintptr_t(ctx->Rsp));
      ctx->Rsp += 8;
    } else {
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ip, fe, ctx, &handler_data, &establisher, nullptr);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Unwinds a suspended thread's context. Stops at the first address outside every module known at start (JIT code,
// or a corrupt stack), so the unwinder never looks up dynamic function tables.
int Unwind(CONTEXT ctx, uint64_t* pc) {
  int n = 0;
  while (n < kFrames) {
    const uint64_t ip = ctx.Rip;
    if (!ip) break;
    pc[n++] = ip;
    if (!InModule(ip)) break;
    const uint64_t sp = ctx.Rsp;
    if (!UnwindStep(&ctx, ip)) break;
    if (ctx.Rsp <= sp) break;  // the stack must move up each frame; anything else is a bad unwind
  }
  return n;
}

std::string ThreadName(HANDLE h) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  std::string out;
  PWSTR w = nullptr;
  if (fn && SUCCEEDED(fn(h, &w)) && w) {
    for (const wchar_t* p = w; *p; ++p) out += (*p < 128) ? char(*p) : '?';
    LocalFree(w);
  }
  return out;
}

uint64_t FileTime100ns(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

std::string Symbol(uint64_t addr) {
  alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(storage);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 255;
  DWORD64 disp = 0;
  if (SymFromAddr(GetCurrentProcess(), DWORD64(addr), &disp, sym)) return std::string(sym->Name, sym->NameLen);
  for (const Range& r : g_modules) {
    if (addr >= r.lo && addr < r.hi) {
      char path[MAX_PATH] = {};
      GetModuleFileNameA(reinterpret_cast<HMODULE>(uintptr_t(r.lo)), path, MAX_PATH);
      const char* slash = strrchr(path, '\\');
      return std::string(slash ? slash + 1 : path) + "!?";
    }
  }
  return "(outside modules)";
}

void Refresh(std::vector<Tracked>& threads, DWORD self) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  const DWORD pid = GetCurrentProcessId();
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  std::unordered_set<DWORD> known;
  for (const Tracked& t : threads) known.insert(t.tid);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || known.count(te.th32ThreadID)) continue;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    Tracked t;
    t.tid = te.th32ThreadID;
    t.handle = h;
    t.name = ThreadName(h);
    QueryThreadCycleTime(h, &t.last_cycles);
    FILETIME c, e;
    GetThreadTimes(h, &c, &e, &t.k0, &t.u0);
    threads.push_back(std::move(t));
  }
  CloseHandle(snap);
}

void Report(std::vector<Tracked>& threads, double wall_s) {
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);  // fails harmlessly if the watch report already did it
  struct Row {
    Tracked* t;
    double cpu;  // % of one core
  };
  std::vector<Row> rows;
  double total = 0;
  for (Tracked& t : threads) {
    FILETIME c, e, k, u;
    double cpu = 0;
    if (GetThreadTimes(t.handle, &c, &e, &k, &u))
      cpu = double((FileTime100ns(k) - FileTime100ns(t.k0)) + (FileTime100ns(u) - FileTime100ns(t.u0))) / 1e5 / wall_s;
    total += cpu;
    rows.push_back({&t, cpu});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cpu > b.cpu; });
  int busy = 0;
  for (const Row& r : rows) busy += r.cpu >= 5.0;
  REXLOG_INFO("gow2_profile: {:.1f} s window | process CPU {:.0f}% of one core (= {:.2f} cores busy on average) | "
              "{} threads, {} above 5% | logical cores on this PC: {}",
              wall_s, total, total / 100.0, rows.size(), busy, std::thread::hardware_concurrency());
  for (const Row& r : rows) {
    if (r.cpu < 1.0) continue;
    REXLOG_INFO("gow2_profile: thread {:>6} {:<36} CPU {:5.1f}% | samples {}", r.t->tid,
                r.t->name.empty() ? std::string("(unnamed)") : r.t->name.substr(0, 36), r.cpu, r.t->samples);
  }
  const int top = std::max(1, int(REXCVAR_GET(gow2_profile_top)));
  std::unordered_map<uint64_t, std::string> names;
  auto name_of = [&](uint64_t a) -> const std::string& {
    auto it = names.find(a);
    if (it == names.end()) it = names.emplace(a, Symbol(a)).first;
    return it->second;
  };
  for (const Row& r : rows) {
    if (r.cpu < 5.0 || !r.t->samples) continue;
    std::unordered_map<std::string, uint64_t> self, incl;
    for (const auto& [h, s] : r.t->stacks) {
      if (!s.depth) continue;
      self[name_of(s.pc[0])] += s.count;
      std::unordered_set<std::string> seen;
      for (int i = 0; i < s.depth; ++i) {
        const std::string& n = name_of(i ? s.pc[i] - 1 : s.pc[i]);  // return address - 1 = inside the call
        if (seen.insert(n).second) incl[n] += s.count;
      }
    }
    auto print = [&](const char* what, std::unordered_map<std::string, uint64_t>& m) {
      std::vector<std::pair<uint64_t, std::string>> v;
      for (auto& [n, c] : m) v.push_back({c, n});
      std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      for (int i = 0; i < int(v.size()) && i < top; ++i)
        REXLOG_INFO("gow2_profile:   [{} {}] {:5.1f}% {}", r.t->tid, what, 100.0 * double(v[i].first) / double(r.t->samples),
                    v[i].second.substr(0, 160));
    };
    REXLOG_INFO("gow2_profile: thread {} {} - CPU {:.1f}% of a core, {} samples:", r.t->tid, r.t->name, r.cpu, r.t->samples);
    print("self", self);
    print("incl", incl);
    // Who calls the top few own-time functions (first frame outside them), e.g. which code allocates or copies.
    std::vector<std::pair<uint64_t, std::string>> tops;
    for (auto& [n, c] : self) tops.push_back({c, n});
    std::sort(tops.begin(), tops.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (int k = 0; k < int(tops.size()) && k < 6; ++k) {
      const std::string& fn = tops[k].second;
      std::unordered_map<std::string, uint64_t> callers;
      for (const auto& [h, s] : r.t->stacks) {
        if (!s.depth || name_of(s.pc[0]) != fn) continue;
        std::string caller = "(none)";
        for (int i = 1; i < s.depth; ++i) {
          const std::string& n = name_of(s.pc[i] - 1);
          auto skip = [](const std::string& x) {  // runtime / allocator / standard-library plumbing: look past it
            for (const char* s : {"malloc", "free", "operator new", "operator delete", "Heap", "_base", "std::",
                                  "allocator", "memmove", "memcpy", "memset", "_Allocate", "_Deallocate"})
              if (x.find(s) != std::string::npos) return true;
            return x.rfind("Rtl", 0) == 0 || x.rfind("Nt", 0) == 0 || x.rfind("Zw", 0) == 0;
          };
          if (n != fn && !skip(n)) {
            caller = n;
            break;
          }
        }
        callers[caller] += s.count;
      }
      std::vector<std::pair<uint64_t, std::string>> cv;
      for (auto& [n, c] : callers) cv.push_back({c, n});
      std::sort(cv.begin(), cv.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      for (int i = 0; i < int(cv.size()) && i < 4; ++i)
        REXLOG_INFO("gow2_profile:   [{} callers of {}] {:5.1f}% {}", r.t->tid, fn.substr(0, 60),
                    100.0 * double(cv[i].first) / double(r.t->samples), cv[i].second.substr(0, 150));
    }
  }
  for (Tracked& t : threads) CloseHandle(t.handle);
}

void Run(int seconds, int start_swap) {
  while (XenosSwapCount() < uint64_t(start_swap)) Sleep(50);
  LoadModuleRanges();
  timeBeginPeriod(1);
  const DWORD self = GetCurrentThreadId();
  std::vector<Tracked> threads;
  threads.reserve(512);
  Refresh(threads, self);
  REXLOG_INFO("gow2_profile: sampling {} threads for {} s from swap {}", threads.size(), seconds, XenosSwapCount());
  LARGE_INTEGER f, t0, now;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  LARGE_INTEGER last_refresh = t0;
  uint64_t sweeps = 0;
  for (;;) {
    QueryPerformanceCounter(&now);
    const double el = double(now.QuadPart - t0.QuadPart) / double(f.QuadPart);
    if (el >= seconds) break;
    if (double(now.QuadPart - last_refresh.QuadPart) / double(f.QuadPart) > 0.5) {
      Refresh(threads, self);
      last_refresh = now;
    }
    for (Tracked& t : threads) {
      ULONG64 cyc = 0;
      if (!QueryThreadCycleTime(t.handle, &cyc) || cyc == t.last_cycles) continue;  // idle since last look
      t.last_cycles = cyc;
      if (SuspendThread(t.handle) == DWORD(-1)) continue;
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_FULL;
      uint64_t pc[kFrames];
      int n = 0;
      if (GetThreadContext(t.handle, &ctx)) n = Unwind(ctx, pc);
      ResumeThread(t.handle);
      if (!n) continue;
      uint64_t h = 1469598103934665603ull;
      for (int i = 0; i < n; ++i) h = (h ^ pc[i]) * 1099511628211ull;
      Stack& s = t.stacks[h];
      if (!s.count) {
        s.depth = uint8_t(n);
        std::copy(pc, pc + n, s.pc);
      }
      ++s.count;
      ++t.samples;
    }
    ++sweeps;
    Sleep(1);
  }
  timeEndPeriod(1);
  QueryPerformanceCounter(&now);
  const double wall = double(now.QuadPart - t0.QuadPart) / double(f.QuadPart);
  REXLOG_INFO("gow2_profile: done at swap {} - {} sweeps in {:.1f} s ({:.2f} ms apart)", XenosSwapCount(), sweeps, wall,
              sweeps ? 1000.0 * wall / double(sweeps) : 0.0);
  Report(threads, wall);
}

}  // namespace

void XenosInstallProfilerIfRequested() {
  const int seconds = REXCVAR_GET(gow2_profile);
  if (seconds <= 0) return;
  XenosEnsureObserverHub();  // the swap counter
  const int start = std::max(0, int(REXCVAR_GET(gow2_profile_start)));
  std::thread([seconds, start] { Run(seconds, start); }).detach();
}

#else
void XenosInstallProfilerIfRequested() {}
#endif
