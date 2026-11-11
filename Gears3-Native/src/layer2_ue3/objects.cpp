// Layer 2 (Unreal Engine 3): the engine's objects in guest memory (ue3/objects.h). From KiltedKernel's Gears 3 port.
// Our code (MIT).
#include <ue3/objects.h>

#include <rex/logging/macros.h>
#include <rex/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ue3::obj {
namespace {
constexpr uint32_t kSlot = 32;             // UObject::ObjectInternalInteger
constexpr uint32_t kViewportName = 10043;  // "GameViewportClient" on the disc; only for the memory search (fallback)
std::atomic<bool> g_ready{false};
std::mutex g_mutex;
uint32_t g_names = 0, g_objects = 0, g_objectCount = 0, g_namesVar = 0, g_objectsVar = 0;
std::unordered_map<std::string, uint32_t> g_classCache;
bool NameKey(const std::string& full, uint32_t* index, uint32_t* number);  // below

uint32_t Swap(uint32_t v) { return _byteswap_ulong(v); }

uint8_t* Host(uint32_t address) {
  auto* rt = rex::Runtime::instance();
  return rt ? rt->virtual_membase() + address : nullptr;
}

// only plain committed memory: a guard page (guest stacks) would lose its guard, and the GPU register pages fault on
// purpose for the emulated device; neither may be touched from here
bool PageReadable(const uint8_t* host) {
  // the last readable region, per thread: most reads land in the same few regions (VirtualQuery is a system call)
  static thread_local const uint8_t *lo = nullptr, *hi = nullptr;
  if (host >= lo && host < hi) return true;
  MEMORY_BASIC_INFORMATION mbi;
  if (!host || !VirtualQuery(host, &mbi, sizeof(mbi))) return false;
  const bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mbi.Protect != 0;
  if (ok) {
    lo = static_cast<const uint8_t*>(mbi.BaseAddress);
    hi = lo + mbi.RegionSize;
  }
  return ok;
}

bool CopyRaw(void* out, const void* src, size_t size) {
  __try {
    std::memcpy(out, src, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// guest pages (4 KB) seen readable: a committed page without a guard stays so (one that is freed later is caught by
// CopyRaw's handler), so each is asked about once. A name lookup alternates between the names table and each name's
// own allocation, and with only the last region remembered nearly every read was a VirtualQuery: while the title
// loaded, RaiseActCap's lookups took the game thread 38-63 s.
std::atomic<uint32_t> g_readablePages[0xC0000000ull / 4096 / 32];

bool GuestPageReadable(uint32_t address) {
  std::atomic<uint32_t>& word = g_readablePages[address >> 17];
  const uint32_t bit = 1u << ((address >> 12) & 31);
  if (word.load(std::memory_order_relaxed) & bit) return true;
  if (!PageReadable(Host(address))) return false;
  word.fetch_or(bit, std::memory_order_relaxed);
  return true;
}

bool Copy(uint32_t address, void* out, size_t size) {
  if (address < 0x10000 || uint64_t(address) + size > 0xC0000000ull) return false;
  if (address >= 0x7FC00000 && address < 0x80000000) return false;  // GPU registers
  for (uint32_t page = address & ~4095u; page <= uint32_t(address + size - 1); page += 4096)
    if (!GuestPageReadable(page)) return false;
  return CopyRaw(out, Host(address), size);
}

template <class F>
bool GuardedScan(F& f, uint32_t a, const uint8_t* host, size_t n) {
  bool r = true;
  __try {
    r = f(a, host, n);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    r = true;
  }
  return r;
}

// committed guest regions, for the one-time table search
template <class F>
void ForEachRegion(uint64_t from, uint64_t to, F&& f) {
  for (uint64_t a = from; a < to;) {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* host = Host(uint32_t(a));
    if (!host || !VirtualQuery(host, &mbi, sizeof(mbi))) break;
    const uint64_t end = std::min<uint64_t>(to, a + uint64_t((uint8_t*)mbi.BaseAddress + mbi.RegionSize - host));
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && end > a)
      if (!GuardedScan(f, uint32_t(a), host, size_t(end - a))) return;
    a = end;
  }
}

std::string EntryName(uint32_t entry) {
  char buf[96] = {};
  if (!entry || !Copy(entry + 8, buf, sizeof(buf) - 1)) return {};
  buf[sizeof(buf) - 1] = 0;
  return buf;
}

// the engine's hardcoded names come first (None, then the property types: ByteProperty, IntProperty...)
bool CheckNames(uint32_t table) {
  if (EntryName(Read32(table)) != "None") return false;
  for (uint32_t i = 1; i <= 3; i++) {
    const std::string n = EntryName(Read32(table + 4 * i));
    if (n.size() < 9 || n.compare(n.size() - 8, 8, "Property") != 0) return false;
  }
  return true;
}

// every listed object's ObjectInternalInteger is its own slot
bool CheckObjects(uint32_t table) {
  int good = 0;
  for (uint32_t i : {1u, 2u, 3u, 4u, 10u, 100u, 1000u, 5000u, 20000u}) {
    const uint32_t x = Read32(table + 4 * i);
    if (x && Read32(x + kSlot) != i) return false;
    good += x != 0;
  }
  return good >= 6;
}

bool FindTables() {
  if (const uint32_t n = Read32(g_namesVar); n && CheckNames(n)) g_names = n;
  if (const uint32_t o = Read32(g_objectsVar); g_names && o && CheckObjects(o)) g_objects = o;
  if (!g_names && g_namesVar) return false;   // known globals: wait for the engine to fill them (no memory search)
  if (!g_names) {
    // the name entry {kViewportName * 2, hash next, "GameViewportClient"}, then the pointer table pointing at it
    static const char name[] = "GameViewportClient";
    const uint8_t idx[4] = {0, 0, uint8_t((kViewportName * 2) >> 8), uint8_t(kViewportName * 2)};
    uint32_t entry = 0;
    ForEachRegion(0x10000, 0xC0000000, [&](uint32_t base, const uint8_t* host, size_t size) {
      for (size_t i = 0; i + 8 + sizeof(name) <= size; i++)
        if (host[i + 3] == idx[3] && std::memcmp(host + i, idx, 4) == 0 && std::memcmp(host + i + 8, name, sizeof(name)) == 0) {
          entry = base + uint32_t(i);
          return false;
        }
      return true;
    });
    if (!entry) return false;
    const uint32_t be = Swap(entry);
    ForEachRegion(0x10000, 0xC0000000, [&](uint32_t base, const uint8_t* host, size_t size) {
      const uint32_t* w = (const uint32_t*)host;
      for (size_t i = 0; i < size / 4; i++)
        if (w[i] == be && base + 4 * i >= 4 * kViewportName && CheckNames(uint32_t(base + 4 * i - 4 * kViewportName))) {
          g_names = uint32_t(base + 4 * i - 4 * kViewportName);
          return false;
        }
      return true;
    });
    if (!g_names) return false;
  }
  if (!g_objects) return false;   // (the globals give it once the engine has started)
  if (g_objectsVar) {   // the engine's own count (TArray {data, count, max})
    g_objectCount = Read32(g_objectsVar + 4);
    return g_objectCount > 0;
  }
  uint32_t n = 0, last = 0;
  for (uint32_t bad = 0; n < 3000000 && bad < 4096; n++) {
    if (!Readable(g_objects + 4 * n, 4)) break;
    const uint32_t x = Read32(g_objects + 4 * n);
    if (x && Read32(x + kSlot) == n) {
      last = n;
      bad = 0;
    } else {
      bad++;
    }
  }
  g_objectCount = last + 1;
  return true;
}

// current table addresses and object count from the engine's globals (the arrays are reallocated as they grow)
void Refresh() {
  const uint32_t n = Read32(g_namesVar), o = Read32(g_objectsVar), count = Read32(g_objectsVar + 4);
  if (n && n != g_names && CheckNames(n)) g_names = n;
  if (o && o != g_objects && CheckObjects(o)) g_objects = o;
  if (o == g_objects && count && count < 4000000) g_objectCount = count;
}

void ExtendCount() {
  if (g_objectsVar) return;   // Refresh has the engine's own count
  for (uint32_t n = g_objectCount, bad = 0; n < 3000000 && bad < 4096; n++) {
    const uint32_t x = Read32(g_objects + 4 * n);
    if (x && Read32(x + kSlot) == n) {
      g_objectCount = n + 1;
      bad = 0;
    } else {
      bad++;
    }
  }
}

uint32_t FindProperty(uint32_t cls, const char* property) {
  for (uint32_t c = cls, guard = 0; c && guard < 64; c = Read32(c + 64), guard++)
    for (uint32_t f = Read32(c + 68), n = 0; f && n < 10000; f = Read32(f + 60), n++)
      if (ObjectName(f) == property) return f;
  return 0;
}
}  // namespace

bool Init(uint32_t names_var, uint32_t objects_var) {
  if (g_ready) return true;
  std::lock_guard lock(g_mutex);
  if (g_ready) return true;
  g_namesVar = names_var;
  g_objectsVar = objects_var;
  if (!FindTables()) return false;
  g_ready = true;
  REXLOG_INFO("ue3 objects: names at {:08X}, objects at {:08X} ({} slots)", g_names, g_objects, g_objectCount);
  return true;
}

bool Ready() { return g_ready; }

uint32_t Read32(uint32_t address) {
  uint32_t v;
  return Copy(address, &v, 4) ? Swap(v) : 0;
}

float ReadFloat(uint32_t address) {
  const uint32_t u = Read32(address);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

bool Readable(uint32_t address, uint32_t size) {
  uint8_t probe;
  return Copy(address, &probe, 1) && Copy(address + size - 1, &probe, 1);
}

bool Write32(uint32_t address, uint32_t value) {
  if (!Readable(address, 4)) return false;
  const uint32_t be = Swap(value);
  std::memcpy(Host(address), &be, 4);
  return true;
}

bool Write8(uint32_t address, uint8_t value) {
  if (!Readable(address, 1)) return false;
  *Host(address) = value;
  return true;
}

std::string Name(uint32_t index) {
  static thread_local auto last = std::chrono::steady_clock::time_point{};
  if (std::chrono::steady_clock::now() - last > std::chrono::milliseconds(500)) {
    Refresh();
    last = std::chrono::steady_clock::now();
  }
  if (!g_names || index > 1000000) return {};
  return EntryName(Read32(g_names + 4 * index));
}

std::string ObjectName(uint32_t object) {
  if (!object) return {};
  const uint32_t index = Read32(object + 44), number = Read32(object + 48);
  std::string n = Name(index);
  return number ? n + "_" + std::to_string(number - 1) : n;
}

uint32_t ClassOf(uint32_t object) { return object ? Read32(object + 52) : 0; }

uint32_t FindClass(const char* name) {
  {
    std::lock_guard lock(g_mutex);
    auto it = g_classCache.find(name);
    if (it != g_classCache.end()) return it->second;
  }
  uint32_t found = 0;
  Refresh();
  uint32_t index = 0, number = 0, classIndex = 0, classNumber = 0;
  if (NameKey(name, &index, &number) && NameKey("Class", &classIndex, &classNumber)) {
    std::vector<uint32_t> table(g_objectCount);
    if (g_objectCount && Copy(g_objects, table.data(), table.size() * 4))
      for (uint32_t raw : table) {
        const uint32_t o = Swap(raw);
        if (!o || Read32(o + 44) != index || Read32(o + 48) != number) continue;
        const uint32_t c = ClassOf(o);
        if (c && Read32(c + 44) == classIndex) { found = o; break; }
      }
  }
  if (found) {
    std::lock_guard lock(g_mutex);
    g_classCache[name] = found;
  }
  return found;
}

bool IsA(uint32_t object, uint32_t cls) {
  for (uint32_t c = ClassOf(object), guard = 0; c && guard < 64; c = Read32(c + 64), guard++)
    if (c == cls) return true;
  return false;
}

namespace {
// "Name" or "Name_<n>" -> (name index, number): the names table searched once per string; -1 when not (yet) a name
bool NameKey(const std::string& full, uint32_t* index, uint32_t* number) {
  // every name read so far, by text: the table only grows, so each call reads only the names added since the last
  // (a name looked up before it exists - Default__GearTypes while GearGame.xxx loads - costs little to ask again)
  static std::mutex m;
  static std::unordered_map<std::string, uint32_t> byText;
  static uint32_t scanned = 0;
  std::string base = full;
  uint32_t num = 0;
  if (const size_t u = full.find_last_of('_'); u != std::string::npos && u + 1 < full.size() &&
                                                full.find_first_not_of("0123456789", u + 1) == std::string::npos &&
                                                (full.size() - u - 1 == 1 || full[u + 1] != '0')) {
    base = full.substr(0, u);
    num = uint32_t(std::stoul(full.substr(u + 1))) + 1;
  }
  std::lock_guard lock(m);
  if (!g_names) return false;
  // up to the table's count (TArray {data, count, max} in the engine's global; empty slots are skipped)
  const uint32_t count = g_namesVar ? std::min<uint32_t>(Read32(g_namesVar + 4), 2000000) : 1000000;
  for (; scanned < count; scanned++)
    if (const uint32_t entry = Read32(g_names + 4 * scanned)) byText.emplace(EntryName(entry), scanned);
  const auto it = byText.find(base);
  if (it == byText.end()) return false;
  *index = it->second;
  *number = num;
  return true;
}
}  // namespace

uint32_t FindObject(const char* name) {
  Refresh();
  uint32_t index = 0, number = 0;
  if (!NameKey(name, &index, &number)) return 0;   // (a name nothing has made yet: no such object)
  std::vector<uint32_t> table(g_objectCount);
  if (!g_objectCount || !Copy(g_objects, table.data(), table.size() * 4)) return 0;
  for (uint32_t raw : table) {
    const uint32_t o = Swap(raw);
    if (o && Read32(o + 44) == index && Read32(o + 48) == number) return o;
  }
  return 0;
}

std::vector<uint32_t> AllObjects() {
  std::vector<uint32_t> out;
  Refresh();
  ExtendCount();
  std::vector<uint32_t> table(g_objectCount);
  if (!g_objectCount || !Copy(g_objects, table.data(), table.size() * 4)) return out;
  out.reserve(table.size());
  for (uint32_t raw : table)
    if (raw) out.push_back(Swap(raw));
  return out;
}

std::vector<uint32_t> FindInstancesOf(uint32_t cls) {
  std::vector<uint32_t> out;
  std::unordered_map<uint32_t, bool> isSub;  // class -> derives from cls
  for (uint32_t o : AllObjects()) {
    const uint32_t c = ClassOf(o);
    if (!c) continue;
    auto it = isSub.find(c);
    if (it == isSub.end()) {
      bool sub = false;
      for (uint32_t k = c, guard = 0; k && guard < 64; k = Read32(k + 64), guard++)
        if (k == cls) {
          sub = true;
          break;
        }
      it = isSub.emplace(c, sub).first;
    }
    if (it->second && ObjectName(o).rfind("Default__", 0) != 0) out.push_back(o);
  }
  return out;
}

std::vector<uint32_t> FindInstancesOfRange(uint32_t cls, uint32_t first, uint32_t count, uint32_t* total) {
  std::vector<uint32_t> out;
  Refresh();
  ExtendCount();
  if (total) *total = g_objectCount;
  if (!cls || first >= g_objectCount) return out;
  count = std::min(count, g_objectCount - first);
  std::vector<uint32_t> table(count);
  if (!count || !Copy(g_objects + 4 * first, table.data(), table.size() * 4)) return out;
  static std::unordered_map<uint64_t, bool> isSub;  // (cls, class) -> derives (game thread; classes are not freed)
  if (isSub.size() > 65536) isSub.clear();
  for (uint32_t raw : table) {
    const uint32_t o = Swap(raw);
    if (!o) continue;
    const uint32_t c = ClassOf(o);
    if (!c) continue;
    const uint64_t key = (uint64_t(cls) << 32) | c;
    auto it = isSub.find(key);
    if (it == isSub.end()) {
      bool sub = false;
      for (uint32_t k = c, guard = 0; k && guard < 64; k = Read32(k + 64), guard++)
        if (k == cls) { sub = true; break; }
      it = isSub.emplace(key, sub).first;
    }
    if (it->second && ObjectName(o).rfind("Default__", 0) != 0) out.push_back(o);
  }
  return out;
}

int PropertyOffset(uint32_t cls, const char* property) {
  const uint32_t p = FindProperty(cls, property);
  return p ? int(Read32(p + 84)) : -1;
}

int PropertySize(uint32_t cls, const char* property) {
  const uint32_t p = FindProperty(cls, property);
  return p ? int(Read32(p + 68)) : -1;
}

uint32_t BoolMask(uint32_t cls, const char* property) {
  const uint32_t p = FindProperty(cls, property);
  static const uint32_t boolProperty = FindClass("BoolProperty");
  return p && ClassOf(p) == boolProperty ? Read32(p + 104) : 0;
}

}  // namespace ue3::obj
