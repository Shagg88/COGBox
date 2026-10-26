// Layer 0 (game-free), patch 0134: a plain-English message instead of a silent crash on a CPU too old for the build.
// When GN_CPU_ARCH is x86-64-v3 the recompiled code uses AVX2 / FMA / BMI2, and an older CPU (or a VM that hides those
// features) dies with 0xC000001D, "illegal instruction", before anything is logged. This file is always compiled for the
// plain x86-64 baseline (CMakeLists sets that per file), and its check runs among the compiler's own start-up work
// (init_seg(compiler)), before our other start-up code. It only reads the CPU's feature bits; nothing is changed.
// GN_CPU_LEVEL is the level the build asked for; with the baseline (empty) the check does nothing.
#if !defined(_M_ARM64)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <cstring>

#ifndef GN_CPU_LEVEL
#define GN_CPU_LEVEL ""
#endif

// The launcher looks for this text in the exe to know which CPU level it was built for (patch 0134).
extern "C" __declspec(dllexport) const char gn_cpu_level_marker[] = "gn_cpu_level=" GN_CPU_LEVEL;

namespace {

// xgetbv is only reached once the CPU says OSXSAVE is on; the target attribute lets the baseline build use it.
#if defined(__clang__)
__attribute__((target("xsave")))
#endif
bool HasV3() {
  int r[4] = {};
  __cpuid(r, 0);
  const int max_leaf = r[0];
  if (max_leaf < 7) return false;
  __cpuid(r, 1);
  const unsigned ecx1 = static_cast<unsigned>(r[2]);
  const bool fma = ecx1 & (1u << 12), movbe = ecx1 & (1u << 22), osxsave = ecx1 & (1u << 27),
             avx = ecx1 & (1u << 28), f16c = ecx1 & (1u << 29);
  if (!(fma && movbe && osxsave && avx && f16c)) return false;
  // The OS must save the AVX registers on a task switch (XCR0 bits 1 and 2), or AVX code faults anyway.
  if ((_xgetbv(0) & 0x6) != 0x6) return false;
  __cpuidex(r, 7, 0);
  const unsigned ebx7 = static_cast<unsigned>(r[1]);
  const bool bmi1 = ebx7 & (1u << 3), avx2 = ebx7 & (1u << 5), bmi2 = ebx7 & (1u << 8);
  __cpuid(r, static_cast<int>(0x80000001u));
  const bool lzcnt = static_cast<unsigned>(r[2]) & (1u << 5);
  return bmi1 && avx2 && bmi2 && lzcnt;
}

struct CpuCheck {
  CpuCheck() {
    if (std::strcmp(GN_CPU_LEVEL, "x86-64-v3") != 0 && std::strcmp(GN_CPU_LEVEL, "x86-64-v4") != 0) return;
    if (HasV3()) return;
    MessageBoxW(nullptr,
                L"This build needs a CPU with AVX2 (Intel Haswell / AMD Excavator or Ryzen, about 2013 and newer).\r\n\r\n"
                L"This CPU does not report AVX2, FMA and BMI2, so the game would crash at once. If this is a virtual "
                L"machine, turn on passing the host CPU's features through to it.",
                L"CPU too old for this build", MB_OK | MB_ICONERROR);
    ExitProcess(0xC000001Du);  // the same code an illegal instruction gives, so the launcher's message matches
  }
};

#pragma warning(push)
#pragma warning(disable : 4073)  // "initializers put in library initialization area" - wanted: it runs first
#pragma init_seg(compiler)
const CpuCheck g_cpu_check;
#pragma warning(pop)

}  // namespace

#endif
