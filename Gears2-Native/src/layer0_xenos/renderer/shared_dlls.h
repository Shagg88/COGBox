// 0.4.9 (shared049, docs/plan-049-shared-dlls.md): the vendor DLLs every game uses (NVIDIA DLSS + Streamline, AMD
// FidelityFX, XeSS, DXC) are kept ONCE in the release's <COGBox>\shared\ folder instead of beside each game's exe.
// Release layout: <COGBox>\<Game>\<port folder>\<game>.exe, so shared\ is <exe folder>\..\..\shared\.
// Rule: the exe's own folder FIRST (developer builds have every DLL there; a player's own copy - the launcher's
// "Custom DLSS file" - goes there), then shared\. One exception, Streamline (DLSS-G): shared\ first, because the 0.4.8
// copies COGBox put beside each exe stay there after an update (the launcher never moves sl.*.dll / nvngx_dlssg.dll:
// players add those themselves), and an update must still change Streamline in one place. The signed NVIDIA files
// are loaded by full path and checked by NVIDIA's own code; no System32 / PATH guessing for them.
// Not shared (see the plan): D3D12\D3D12Core.dll (the exe's D3D12SDKPath is relative to the exe) and the SDK runtime
// rexruntime.dll / rexgpu-xenos.dll (load-time imports of the exe: Windows only looks beside the exe, System32 and PATH).
// Header only (the four games glob layer 0's sources; no CMake change). Our code (MIT).
#pragma once

#include <windows.h>

#include <string>

namespace gn_shared_dlls {

// "<exe folder>\" (with the trailing backslash), empty if unknown.
inline std::wstring ExeDir() {
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(MAX_PATH * 2));
  if (!n || n >= MAX_PATH * 2) return {};
  std::wstring s(path, n);
  const size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
}

inline bool FileThere(const std::wstring& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// "<COGBox>\shared\" (full path, trailing backslash) when the exe sits in a release layout, else empty.
inline const std::wstring& SharedDir() {
  static const std::wstring dir = [] {
    const std::wstring exe = ExeDir();
    if (exe.empty()) return std::wstring();
    const std::wstring rel = exe + L"..\\..\\shared";
    wchar_t full[MAX_PATH * 2] = {};
    const DWORD n = GetFullPathNameW(rel.c_str(), DWORD(MAX_PATH * 2), full, nullptr);
    if (!n || n >= MAX_PATH * 2) return std::wstring();
    const DWORD a = GetFileAttributesW(full);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return std::wstring();
    std::wstring s(full, n);
    if (s.back() != L'\\') s.push_back(L'\\');
    return s;
  }();
  return dir;
}

// The folder ("...\") holding ALL of `names`: the exe's folder first (shared_first: shared\ first), then the other;
// empty if neither has them all. (All from one folder: Streamline's plugins must come from its interposer's folder.)
template <size_t N>
std::wstring FolderWithAll(const wchar_t* const (&names)[N], bool shared_first = false) {
  const std::wstring order[2] = {shared_first ? SharedDir() : ExeDir(), shared_first ? ExeDir() : SharedDir()};
  for (const std::wstring& d : order) {
    if (d.empty()) continue;
    bool all = true;
    for (const wchar_t* n : names) all = all && FileThere(d + n);
    if (all) return d;
  }
  return {};
}

// By full path only: <exe folder>\name first (a developer build; a player's own copy), then <COGBox>\shared\name.
// Review 049: no LoadLibraryW(name) - its search (current folder, PATH) could pick up a stray copy of a vendor DLL.
// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | SYSTEM32: the DLL's own imports come from its folder and System32 only.
inline HMODULE LoadVendorDll(const wchar_t* name) {
  constexpr DWORD kFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
  for (const std::wstring& d : {ExeDir(), SharedDir()}) {
    if (d.empty() || !FileThere(d + name)) continue;
    if (HMODULE m = LoadLibraryExW((d + name).c_str(), nullptr, kFlags)) return m;
  }
  return nullptr;
}

inline std::string Narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s += c < 128 ? char(c) : '?';
  return s;
}

}  // namespace gn_shared_dlls
