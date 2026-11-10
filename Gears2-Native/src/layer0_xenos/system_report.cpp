// Layer 0 (game-free), patch 0131: a description of the PC at the top of every game log, so a problem report explains
// itself - Windows (and Wine, if any), CPU, memory, graphics cards with their driver versions, screens, controllers, the
// build (x64 / ARM64, compile date) and the command line (every setting the launcher passed). Read-only system queries;
// nothing is changed. dxgi.dll and xinput1_4.dll are loaded on demand, so nothing new is linked.
#include <xenos/dev_tools.h>
#include <rex/logging/macros.h>

#include <cstdint>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#if !defined(_M_ARM64)
#include <intrin.h>
#endif

#if !defined(_M_ARM64)
// cpu_check.cpp (patch 0134). Naming it here also makes the linker keep that file's start-up check out of the library.
extern "C" const char gn_cpu_level_marker[];
#endif

// save_watch.cpp (GitHub #55): save / checkpoint / content files in the user data folder, logged at info level.
void XenosInstallSaveWatch();

namespace {

// GitHub #59: Wine's DXGI can name another vendor than the card (1002 AMD on an NVIDIA RTX 5060 Ti). The first
// hardware adapter's ids, kept so the Vulkan backend can compare them with its own (XenosReportVulkanDevice).
uint32_t g_dxgi_vendor = 0, g_dxgi_device = 0;
uint32_t g_vk_vendor = 0, g_vk_device = 0;
// GitHub #59 (Judgment under Wine, follow-up): the D3D12 backend's own adapter (found by the device's LUID), which is
// not always DXGI's first hardware adapter (laptops with two GPUs). XenosReportD3D12Device.
uint32_t g_d3d_vendor = 0, g_d3d_device = 0;

const char* VendorName(uint32_t v) {
  switch (v) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: case 0x1022: return "AMD";
    case 0x8086: return "Intel";
    case 0x5143: return "Qualcomm";
    case 0x106B: return "Apple";
    case 0x1414: return "Microsoft (software)";
    case 0x10005: return "Mesa (software)";
    default: return "other";
  }
}

// One line naming the vendor id every GPU-specific choice uses (XenosGpuVendorId) and where it came from.
void LogVendorChoice() {
  const uint32_t v = g_vk_vendor ? g_vk_vendor : g_d3d_vendor ? g_d3d_vendor : g_dxgi_vendor;
  const char* from = g_vk_vendor ? "the Vulkan device" : g_d3d_vendor ? "the D3D12 adapter" : "DXGI's first hardware adapter";
  REXLOG_INFO("pc: GPU vendor for vendor-specific choices: {:04X} ({}) from {} | DXGI first adapter {:04X}:{:04X}, "
              "D3D12 adapter {:04X}:{:04X}, Vulkan device {:04X}:{:04X} (0000 = not used)",
              v, VendorName(v), from, g_dxgi_vendor, g_dxgi_device, g_d3d_vendor, g_d3d_device, g_vk_vendor, g_vk_device);
}

std::string Utf8(const wchar_t* w) {
  if (!w || !*w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string RegString(const wchar_t* key, const wchar_t* name) {
  wchar_t buf[256];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return Utf8(buf);
}

DWORD RegDword(const wchar_t* key, const wchar_t* name) {
  DWORD v = 0, size = sizeof(v);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return 0;
  return v;
}

void LogWindows() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof(v);
  if (auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr) fn(&v);
  const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  REXLOG_INFO("pc: Windows {}.{} build {}.{} ({} {})", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
              RegDword(cv, L"UBR"), RegString(cv, L"ProductName"), RegString(cv, L"DisplayVersion"));
  // Wine / Proton / CrossOver: ntdll exports wine_get_version there (Athelas lesson 19 - such reports explain themselves).
  using WineVersionFn = const char*(__cdecl*)();
  using WineHostFn = void(__cdecl*)(const char**, const char**);
  if (auto wv = ntdll ? reinterpret_cast<WineVersionFn>(GetProcAddress(ntdll, "wine_get_version")) : nullptr) {
    const char* sys = nullptr;
    const char* rel = nullptr;
    if (auto wh = reinterpret_cast<WineHostFn>(GetProcAddress(ntdll, "wine_get_host_version"))) wh(&sys, &rel);
    REXLOG_INFO("pc: running under Wine {} on {} {}", wv() ? wv() : "?", sys ? sys : "?", rel ? rel : "");
  }
}

void LogCpuAndMemory() {
  char brand[49] = {};
#if !defined(_M_ARM64)
  int r[4] = {};
  __cpuid(r, static_cast<int>(0x80000000u));
  if (static_cast<unsigned>(r[0]) >= 0x80000004u) {
    for (int i = 0; i < 3; ++i) {
      __cpuid(r, static_cast<int>(0x80000002u) + i);
      std::memcpy(brand + i * 16, r, 16);
    }
  }
#endif
  std::string name = brand;
  while (!name.empty() && name.front() == ' ') name.erase(name.begin());
  DWORD cores = 0, len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  if (len) {
    std::string buf(len, '\0');
    auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, p, &len)) {
      for (DWORD off = 0; off < len;) {
        auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        ++cores;
        off += e->Size;
      }
    }
  }
  REXLOG_INFO("pc: CPU {} - {} cores, {} threads", name.empty() ? "?" : name, cores,
              GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof(m);
  if (GlobalMemoryStatusEx(&m)) {
    REXLOG_INFO("pc: memory {:.1f} GB, {:.1f} GB free at start", m.ullTotalPhys / 1073741824.0, m.ullAvailPhys / 1073741824.0);
  }
}

void LogGraphicsCards() {
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  if (!dxgi) { REXLOG_INFO("pc: graphics cards: dxgi.dll not found"); return; }
  using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
  auto create = reinterpret_cast<CreateFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
  IDXGIFactory1* factory = nullptr;
  if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
    REXLOG_INFO("pc: graphics cards: no DXGI factory");
    return;
  }
  IDXGIAdapter1* a = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    LARGE_INTEGER umd{};
    std::string driver = "?";
    if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
      const uint64_t q = static_cast<uint64_t>(umd.QuadPart);
      driver = std::to_string(q >> 48) + "." + std::to_string((q >> 32) & 0xFFFF) + "." +
               std::to_string((q >> 16) & 0xFFFF) + "." + std::to_string(q & 0xFFFF);
    }
    REXLOG_INFO("pc: graphics card {}: {} (vendor {:04X} device {:04X}{}) - {} MB own memory, driver {}", i,
                Utf8(d.Description), d.VendorId, d.DeviceId, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? ", software" : "",
                d.DedicatedVideoMemory >> 20, driver);
    if (g_dxgi_vendor == 0 && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { g_dxgi_vendor = d.VendorId; g_dxgi_device = d.DeviceId; }
    a->Release();
  }
  factory->Release();
}

BOOL CALLBACK OnMonitor(HMONITOR mon, HDC, LPRECT, LPARAM count) {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi)) return TRUE;
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  const DWORD hz = EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
  int& n = *reinterpret_cast<int*>(count);
  REXLOG_INFO("pc: screen {}: {}x{} at {} Hz{}", n++, mi.rcMonitor.right - mi.rcMonitor.left,
              mi.rcMonitor.bottom - mi.rcMonitor.top, hz, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (main)" : "");
  return TRUE;
}

void LogControllers() {
  struct PadState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
  using GetStateFn = DWORD(WINAPI*)(DWORD, PadState*);
  HMODULE x = LoadLibraryW(L"xinput1_4.dll");
  if (!x) x = LoadLibraryW(L"xinput9_1_0.dll");
  auto get = x ? reinterpret_cast<GetStateFn>(GetProcAddress(x, "XInputGetState")) : nullptr;
  if (!get) { REXLOG_INFO("pc: controllers: XInput not found"); return; }
  std::string slots;
  for (DWORD i = 0; i < 4; ++i) {
    PadState s{};
    if (get(i, &s) == ERROR_SUCCESS) slots += (slots.empty() ? "" : ", ") + std::to_string(i + 1);
  }
  REXLOG_INFO("pc: Xbox-type controllers connected: {}", slots.empty() ? "none" : slots);
}

}  // namespace

// GitHub #59: called by the Vulkan backend once it has picked its device. The Vulkan ids come from the driver itself
// (under Wine: the host's real driver), so they win over DXGI's when the two disagree; both are logged.
void XenosReportVulkanDevice(uint32_t vendor, uint32_t device, const char* name) {
  g_vk_vendor = vendor;
  g_vk_device = device;
  if (g_dxgi_vendor == 0) {
    REXLOG_INFO("pc: Vulkan device {} (vendor {:04X} device {:04X}); no DXGI ids to compare", name ? name : "?", vendor, device);
  } else if (g_dxgi_vendor != vendor) {
    REXLOG_WARN("pc: graphics vendor ids disagree - DXGI {:04X}:{:04X}, Vulkan {:04X}:{:04X} ({}); using Vulkan's",
                g_dxgi_vendor, g_dxgi_device, vendor, device, name ? name : "?");
  } else {
    REXLOG_INFO("pc: Vulkan device {} matches DXGI (vendor {:04X} device {:04X})", name ? name : "?", vendor, device);
  }
  LogVendorChoice();
}

// GitHub #59 follow-up: called by the D3D12 backend with its own adapter's ids (EnumAdapterByLuid of the device). Under
// Wine (vkd3d-proton) these can be as wrong as DXGI's list; they are still the rendering device's, so they win over the
// list's first adapter, and a Vulkan device (if one is ever reported too) wins over both.
void XenosReportD3D12Device(uint32_t vendor, uint32_t device, const char* name) {
  g_d3d_vendor = vendor;
  g_d3d_device = device;
  if (g_dxgi_vendor && (g_dxgi_vendor != vendor || g_dxgi_device != device)) {
    REXLOG_INFO("pc: D3D12 renders on {} (vendor {:04X} device {:04X}), not DXGI's first adapter ({:04X}:{:04X})",
                name ? name : "?", vendor, device, g_dxgi_vendor, g_dxgi_device);
  }
  LogVendorChoice();
}

// The vendor id any GPU-specific choice should use: the rendering device's - the Vulkan device when on Vulkan (under
// Wine: the host's real driver), else the D3D12 backend's adapter - and DXGI's first hardware adapter only before a
// backend has started. Vulkan's id wins when both are known and disagree (GitHub #59: DXGI said 1002, Vulkan 10DE).
uint32_t XenosGpuVendorId() { return g_vk_vendor ? g_vk_vendor : g_d3d_vendor ? g_d3d_vendor : g_dxgi_vendor; }

void XenosLogSystemReport(const char* game) {
#if defined(_M_ARM64)
  const char* arch = "ARM64";
#else
  const char* arch = "x64";
#endif
  REXLOG_INFO("pc: ---- {} native port, {} build of {} {} (patch 0131 PC description) ----", game, arch, __DATE__, __TIME__);
#if !defined(_M_ARM64)
  const char* level = gn_cpu_level_marker + 13;  // after "gn_cpu_level="
  REXLOG_INFO("pc: built for CPU level {}", *level ? level : "x86-64 baseline");
#endif
  LogWindows();
  LogCpuAndMemory();
  LogGraphicsCards();
  int screens = 0;
  EnumDisplayMonitors(nullptr, nullptr, OnMonitor, reinterpret_cast<LPARAM>(&screens));
  LogControllers();
  REXLOG_INFO("pc: command line: {}", Utf8(GetCommandLineW()));
  REXLOG_INFO("pc: ---- end of PC description ----");
  XenosInstallSaveWatch();  // GitHub #55: every game calls this report once, right after logging starts
}
