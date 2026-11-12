// Layer 2 (Unreal Engine 3): the engine's objects in guest memory (ue3/objects.h). Gears3-Native's objects.cpp with
// the object layout as data, and the tables and the layout found in memory. From KiltedKernel's Gears 3 port. Our
// code (MIT).
#include <ue3/objects.h>

#include <rex/logging/macros.h>
#include <rex/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ue3::obj {
namespace {
std::atomic<bool> g_ready{false};
std::mutex g_mutex;
uint32_t g_names = 0, g_objects = 0, g_objectCount = 0, g_namesVar = 0, g_objectsVar = 0;
Layout g_L;
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
  MEMORY_BASIC_INFORMATION mbi;
  if (!host || !VirtualQuery(host, &mbi, sizeof(mbi))) return false;
  return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mbi.Protect != 0;
}

bool CopyRaw(void* out, const void* src, size_t size) {
  __try {
    std::memcpy(out, src, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// guest pages (4 KB) seen readable, each asked about once (Gears3-Native's: a VirtualQuery per read stalled the title)
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
  if (CopyRaw(out, Host(address), size)) return true;
  for (uint32_t page = address & ~4095u; page <= uint32_t(address + size - 1); page += 4096)   // freed since: ask again
    g_readablePages[page >> 17].fetch_and(~(1u << ((page >> 12) & 31)), std::memory_order_relaxed);
  return false;
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

// committed guest regions, for the one-time searches
template <class F>
void ForEachRegion(uint64_t from, uint64_t to, F&& f) {
  for (uint64_t a = from; a < to;) {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* host = Host(uint32_t(a));
    if (!host || !VirtualQuery(host, &mbi, sizeof(mbi))) break;
    const uint64_t end = std::min<uint64_t>(to, a + uint64_t((uint8_t*)mbi.BaseAddress + mbi.RegionSize - host));
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && end > a &&
        !(a >= 0x7FC00000 && a < 0x80000000))
      if (!GuardedScan(f, uint32_t(a), host, size_t(end - a))) return;
    a = end;
  }
}

// a name entry's text: 8-bit characters, or 16-bit big-endian ones (Gears of War 2006's TCHAR names), up to the 0
std::string EntryText(uint32_t entry, uint32_t at, bool wide) {
  if (!entry) return {};
  if (!wide) {
    char buf[96] = {};
    if (!Copy(entry + at, buf, sizeof(buf) - 1)) return {};
    buf[sizeof(buf) - 1] = 0;
    return buf;
  }
  uint8_t buf[192] = {};
  if (!Copy(entry + at, buf, sizeof(buf))) return {};
  std::string out;
  for (size_t i = 0; i + 1 < sizeof(buf); i += 2) {
    const uint16_t ch = uint16_t(buf[i] << 8 | buf[i + 1]);
    if (!ch) break;
    out += ch < 0x80 ? char(ch) : '?';
  }
  return out;
}
std::string EntryName(uint32_t entry) { return EntryText(entry, g_L.name_text, g_L.name_wide != 0); }

// the engine's hardcoded names come first (None, then the property types: ByteProperty, IntProperty...)
bool CheckNames(uint32_t table, uint32_t at, bool wide) {
  if (EntryText(Read32(table), at, wide) != "None") return false;
  for (uint32_t i = 1; i <= 3; i++) {
    const std::string n = EntryText(Read32(table + 4 * i), at, wide);
    if (n.size() < 9 || n.compare(n.size() - 8, 8, "Property") != 0) return false;
  }
  return true;
}

// every listed object's ObjectInternalInteger (at slot) is its own slot
bool CheckObjects(uint32_t table, uint32_t slot) {
  int good = 0;
  for (uint32_t i : {1u, 2u, 3u, 4u, 10u, 100u, 1000u, 5000u, 20000u}) {
    const uint32_t x = Read32(table + 4 * i);
    if (x && Read32(x + slot) != i) return false;
    good += x != 0;
  }
  return good >= 6;
}

// GNames by its first entries: "ByteProperty" texts (8- or 16-bit characters), the entries they may start (text at
// +4..+16), the table word pointing at one of them as entry 1
bool SearchNames() {
  static const char kText[] = "ByteProperty";
  uint8_t wide[2 * sizeof(kText)] = {};
  for (size_t k = 0; k < sizeof(kText); k++) wide[2 * k + 1] = uint8_t(kText[k]);   // big-endian, with its 0
  std::vector<std::pair<uint32_t, bool>> texts;   // (address, 16-bit)
  ForEachRegion(0x10000, 0xC0000000, [&](uint32_t base, const uint8_t* host, size_t size) {
    for (size_t i = 1; i + sizeof(wide) <= size; i++) {
      const void* hit = std::memchr(host + i, 'B', size - sizeof(wide) - i + 1);
      if (!hit) break;
      i = size_t(static_cast<const uint8_t*>(hit) - host);
      if (std::memcmp(host + i, kText, sizeof(kText)) == 0) texts.push_back({base + uint32_t(i), false});
      else if (host[i - 1] == 0 && std::memcmp(host + i - 1, wide, sizeof(wide)) == 0)
        texts.push_back({base + uint32_t(i - 1), true});
    }
    return texts.size() < 4096;
  });
  if (static int logged = 0; logged++ < 3) REXLOG_INFO("ue3 objects: names search: {} \"ByteProperty\" texts", texts.size());
  std::unordered_map<uint32_t, std::pair<uint32_t, bool>> entries;   // big-endian entry address -> (text at, 16-bit)
  for (auto [t, w16] : texts)
    for (uint32_t at : {4u, 8u, 12u, 16u}) entries[Swap(t - at)] = {at, w16};
  ForEachRegion(0x10000, 0xC0000000, [&](uint32_t base, const uint8_t* host, size_t size) {
    const uint32_t* w = reinterpret_cast<const uint32_t*>(host);
    for (size_t i = 1; i < size / 4; i++) {
      const auto it = entries.find(w[i]);
      if (it != entries.end() && CheckNames(base + 4 * uint32_t(i - 1), it->second.first, it->second.second)) {
        g_names = base + 4 * uint32_t(i - 1);
        g_L.name_text = it->second.first;
        g_L.name_wide = it->second.second;
        return false;
      }
    }
    return true;
  });
  if (g_names) REXLOG_INFO("ue3 objects: names at {:08X} (text at +{}, {}-bit)", g_names, g_L.name_text, g_L.name_wide ? 16 : 8);
  return g_names != 0;
}

// GObjObjects: three table words in a row whose objects' slot fields count up (the slot offset tried 4..48), the
// table where that count starts
bool SearchObjects() {
  ForEachRegion(0x10000, 0xC0000000, [&](uint32_t base, const uint8_t* host, size_t size) {
    const uint32_t* w = reinterpret_cast<const uint32_t*>(host);
    for (size_t i = 0; i + 2 < size / 4; i++) {
      const uint32_t p = Swap(w[i]), q = Swap(w[i + 1]), r = Swap(w[i + 2]);
      auto ptr = [](uint32_t x) { return x >= 0x30000000 && x < 0xC0000000 && !(x & 3); };
      if (!ptr(p) || !ptr(q) || !ptr(r)) continue;
      if ((q > p ? q - p : p - q) > 0x100000 || (r > q ? r - q : q - r) > 0x100000) continue;
      for (uint32_t s = 4; s <= 48; s += 4) {
        const uint32_t a = Read32(p + s);
        if (a == 0 || a > 4000000 || Read32(q + s) != a + 1 || Read32(r + s) != a + 2) continue;
        const uint32_t at = base + 4 * uint32_t(i);
        if (at < 4 * a || !CheckObjects(at - 4 * a, s)) continue;
        g_objects = at - 4 * a;
        g_L.slot = s;
        return false;
      }
    }
    return true;
  });
  if (static int logged = 0; logged++ < 3)
    REXLOG_INFO("ue3 objects: objects search: {}", g_objects ? "found (slot at +" + std::to_string(g_L.slot) + ")" : std::string("not found"));
  return g_objects != 0;
}

void CountObjects() {
  uint32_t n = 0, last = 0;
  for (uint32_t bad = 0; n < 3000000 && bad < 4096; n++) {
    if (!Readable(g_objects + 4 * n, 4)) break;
    const uint32_t x = Read32(g_objects + 4 * n);
    if (x && Read32(x + g_L.slot) == n) {
      last = n;
      bad = 0;
    } else {
      bad++;
    }
  }
  g_objectCount = last + 1;
}

uint32_t TableAt(uint32_t i) { return i < g_objectCount ? Read32(g_objects + 4 * i) : 0; }
bool IsObject(uint32_t o) {   // in the table at its own slot
  if (!o) return false;
  const uint32_t s = Read32(o + g_L.slot);
  return s < g_objectCount && TableAt(s) == o;
}

// the object layout from the objects themselves (logged; --ue3_object_layout fixes it once known)
bool Discover() {
  Layout& L = g_L;
  std::vector<uint32_t> sample;
  for (uint32_t i = 1; i < g_objectCount && sample.size() < 3000; i += std::max<uint32_t>(1, g_objectCount / 3000))
    if (const uint32_t o = TableAt(i)) sample.push_back(o);
  uint32_t namesCount = 0;
  for (uint32_t bad = 0; namesCount < 2000000 && bad < 4096; namesCount++) bad = Read32(g_names + 4 * namesCount) ? 0 : bad + 1;
  bool withNumber = true;   // the FName being tried: index and number (later UE3), or the index alone (2006)
  auto nameAt = [&](uint32_t o, uint32_t off) -> std::string {
    const uint32_t idx = Read32(o + off);
    if (idx >= namesCount || (withNumber && Read32(o + off + 4) > 100000)) return {};
    return EntryName(Read32(g_names + 4 * idx));
  };
  // name and class: the pair under which most objects' class is an object whose class is named "Class"
  int best = 0;
  for (const bool num : {true, false}) {
    withNumber = num;
    for (uint32_t n = L.slot + 4; n <= L.slot + 40; n += 4)
      for (uint32_t c = L.slot + 4; c <= L.slot + 48; c += 4) {
        if (c == n || (num && c == n + 4)) continue;
        int score = 0;
        for (uint32_t o : sample) {
          const uint32_t k = Read32(o + c);
          if (nameAt(o, n).empty() || !IsObject(k)) continue;
          if (nameAt(Read32(k + c), n) == "Class") score++;
        }
        if (score > best) best = score, L.name = n, L.cls = c, L.name_number = num;
      }
  }
  withNumber = L.name_number != 0;
  if (best < int(sample.size()) / 2) {
    REXLOG_WARN("ue3 objects: layout: no name/class offsets ({} of {})", best, sample.size());
    // (what the objects hold, for working the layout out by hand: each word, as a name index and as a table object)
    for (size_t k = 0; k < sample.size() && k < 400; k += 100) {
      std::string line;
      for (uint32_t off = 0; off < 72; off += 4) {
        const uint32_t v = Read32(sample[k] + off);
        line += " +" + std::to_string(off) + "=" + std::to_string(v);
        if (v < namesCount) line += "'" + EntryName(Read32(g_names + 4 * v)) + "'";
        if (IsObject(v)) line += "[obj]";
      }
      REXLOG_INFO("ue3 objects: object {:08X}:{}", sample[k], line);
    }
    return false;
  }
  // outer: the offset under which most objects point at another object (packages: none)
  best = 0;
  for (uint32_t o2 = L.slot + 4; o2 <= L.slot + 48; o2 += 4) {
    if (o2 == L.name || (L.name_number && o2 == L.name + 4) || o2 == L.cls) continue;
    int score = 0;
    for (uint32_t o : sample) score += IsObject(Read32(o + o2));
    if (score > best) best = score, L.outer = o2;
  }
  // classes by name, for the struct fields
  auto classNamed = [&](const char* want) -> uint32_t {
    for (uint32_t i = 0; i < g_objectCount; i++) {
      const uint32_t o = TableAt(i);
      if (o && nameAt(o, L.name) == want && nameAt(Read32(o + L.cls), L.name) == "Class") return o;
    }
    return 0;
  };
  const uint32_t actor = classNamed("Actor"), object = classNamed("Object"), pawn = classNamed("Pawn");
  if (!actor || !object || !pawn) { REXLOG_WARN("ue3 objects: layout: Actor / Object / Pawn classes not found"); return false; }
  for (uint32_t s = L.cls + 4; s <= L.cls + 80 && !L.super; s += 4)
    if (Read32(actor + s) == object && Read32(pawn + s) == actor) L.super = s;
  // children and next: Actor's field chain (every field's outer is Actor), the longest one found
  size_t longest = 0;
  std::vector<uint32_t> chain;
  for (uint32_t ch = L.cls + 4; ch <= L.cls + 96; ch += 4) {
    if (ch == L.super) continue;
    const uint32_t first = Read32(actor + ch);
    if (!IsObject(first) || Read32(first + L.outer) != actor) continue;
    for (uint32_t nx = L.cls + 4; nx <= L.cls + 48; nx += 4) {
      std::vector<uint32_t> c;
      for (uint32_t f = first; f && IsObject(f) && Read32(f + L.outer) == actor && c.size() < 4000; f = Read32(f + nx)) c.push_back(f);
      if (c.size() > longest) longest = c.size(), L.children = ch, L.next = nx, chain = c;
    }
  }
  if (longest < 20) { REXLOG_WARN("ue3 objects: layout: Actor's fields not found ({})", longest); return false; }
  auto field = [&](const char* want) -> uint32_t {
    for (uint32_t f : chain) if (nameAt(f, L.name) == want) return f;
    return 0;
  };
  const uint32_t loc = field("Location"), rot = field("Rotation"), scale = field("DrawScale");
  if (!loc || !rot || !scale) { REXLOG_WARN("ue3 objects: layout: Actor.Location / Rotation / DrawScale not found"); return false; }
  for (uint32_t s = L.next + 4; s <= L.next + 64 && !L.elem_size; s += 4)
    if (Read32(loc + s) == 12 && Read32(rot + s) == 12 && Read32(scale + s) == 4) L.elem_size = s;
  for (uint32_t s = L.elem_size + 4; s <= L.elem_size + 64 && !L.prop_offset; s += 4) {
    const uint32_t a = Read32(loc + s), b = Read32(rot + s), c = Read32(scale + s);
    if (a >= 16 && a < 0x2000 && b == a + 12 && c >= 16 && c < 0x2000) L.prop_offset = s;
  }
  // bool masks: the offset where Actor's bool properties all hold one bit, at least two different ones
  const uint32_t boolCls = classNamed("BoolProperty");
  std::vector<uint32_t> bools;
  for (uint32_t f : chain) if (Read32(f + L.cls) == boolCls) bools.push_back(f);
  for (uint32_t s = L.prop_offset + 4; s <= L.prop_offset + 96 && !L.bool_mask && bools.size() >= 4; s += 4) {
    std::set<uint32_t> seen;
    bool ok = true;
    for (uint32_t f : bools) {
      const uint32_t m = Read32(f + s);
      if (!m || (m & (m - 1))) { ok = false; break; }
      seen.insert(m);
    }
    if (ok && seen.size() >= 2) L.bool_mask = s;
  }
  REXLOG_INFO("ue3 objects: layout found: {} (Actor fields {}, bool properties {})", LayoutText(L), longest, bools.size());
  return L.super && L.children && L.next && L.elem_size && L.prop_offset;
}

bool FindTables() {
  if (!g_names) {
    if (g_namesVar) {
      const uint32_t n = Read32(g_namesVar);
      if (n && CheckNames(n, g_L.name_text, g_L.name_wide)) g_names = n;
    } else {
      SearchNames();
    }
  }
  if (!g_names) return false;
  if (!g_objects) {
    if (g_objectsVar) {
      const uint32_t o = Read32(g_objectsVar);
      if (o && g_L.slot && CheckObjects(o, g_L.slot)) g_objects = o;
    } else {
      SearchObjects();
    }
  }
  if (!g_objects) return false;
  if (g_objectsVar) g_objectCount = Read32(g_objectsVar + 4);
  else CountObjects();
  return g_objectCount > 0;
}

void Refresh() {
  if (!g_namesVar) return;
  const uint32_t n = Read32(g_namesVar), o = Read32(g_objectsVar), count = Read32(g_objectsVar + 4);
  if (n && n != g_names && CheckNames(n, g_L.name_text, g_L.name_wide)) g_names = n;
  if (o && o != g_objects && CheckObjects(o, g_L.slot)) g_objects = o;
  if (o == g_objects && count && count < 4000000) g_objectCount = count;
}

void ExtendCount() {
  if (g_objectsVar) return;   // Refresh has the engine's own count
  for (uint32_t n = g_objectCount, bad = 0; n < 3000000 && bad < 4096; n++) {
    const uint32_t x = Read32(g_objects + 4 * n);
    if (x && Read32(x + g_L.slot) == n) {
      g_objectCount = n + 1;
      bad = 0;
    } else {
      bad++;
    }
  }
}

uint32_t FindProperty(uint32_t cls, const char* property) {
  for (uint32_t c = cls, guard = 0; c && guard < 64; c = Read32(c + g_L.super), guard++)
    for (uint32_t f = Read32(c + g_L.children), n = 0; f && n < 10000; f = Read32(f + g_L.next), n++)
      if (ObjectName(f) == property) return f;
  return 0;
}
}  // namespace

std::string LayoutText(const Layout& l) {
  std::ostringstream s;
  s << l.slot << ',' << l.outer << ',' << l.name << ',' << l.cls << ',' << l.super << ',' << l.children << ',' << l.next
    << ',' << l.elem_size << ',' << l.prop_offset << ',' << l.bool_mask << ',' << l.name_text << ',' << l.name_wide << ',' << l.name_number;
  return s.str();
}

bool ParseLayout(const std::string& text, Layout* out) {
  uint32_t v[13] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 1};
  const int n = std::sscanf(text.c_str(), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                            &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12]);
  if (n < 11) return false;
  *out = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12]};
  return true;
}

bool Init(uint32_t names_var, uint32_t objects_var, const Layout* known) {
  if (g_ready) return true;
  std::lock_guard lock(g_mutex);
  if (g_ready) return true;
  g_namesVar = names_var;
  g_objectsVar = objects_var;
  if (known) g_L = *known;
  if (!FindTables()) return false;
  if (!known && !Discover()) {
    g_names = g_objects = g_objectCount = 0;   // (the next call searches again, later in the boot)
    g_L = Layout{};
    return false;
  }
  g_ready = true;
  REXLOG_INFO("ue3 objects: names at {:08X}, objects at {:08X} ({} slots), layout {}", g_names, g_objects, g_objectCount,
              LayoutText(g_L));
  return true;
}

bool Ready() { return g_ready; }
const Layout& CurrentLayout() { return g_L; }

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

// 0.6.0 review (as Gears3-Native's): the readable-page bitmap never forgets a page, so a page the game freed since it
// was read still passes Readable; the write itself is caught like CopyRaw's read, and that page is asked about again.
bool WriteRaw(void* dst, const void* src, size_t size) {
  __try {
    std::memcpy(dst, src, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void ForgetPage(uint32_t address) {
  if (address < 0xC0000000u) g_readablePages[address >> 17].fetch_and(~(1u << ((address >> 12) & 31)), std::memory_order_relaxed);
}

bool Write32(uint32_t address, uint32_t value) {
  if (!Readable(address, 4)) return false;
  const uint32_t be = Swap(value);
  if (WriteRaw(Host(address), &be, 4)) return true;
  ForgetPage(address);
  ForgetPage(address + 3);
  return false;
}

bool Write8(uint32_t address, uint8_t value) {
  if (!Readable(address, 1)) return false;
  if (WriteRaw(Host(address), &value, 1)) return true;
  ForgetPage(address);
  return false;
}

std::string Name(uint32_t index) {
  if (!g_names || index > 2000000) return {};
  return EntryName(Read32(g_names + 4 * index));
}

namespace {
// the object's FName is (index, number) (a 2006 FName: the index alone, the number 0)
bool NameIs(uint32_t o, uint32_t index, uint32_t number) {
  return Read32(o + g_L.name) == index && (!g_L.name_number || Read32(o + g_L.name + 4) == number);
}
}  // namespace

std::string ObjectName(uint32_t object) {
  if (!object) return {};
  const uint32_t index = Read32(object + g_L.name), number = g_L.name_number ? Read32(object + g_L.name + 4) : 0;
  std::string n = Name(index);
  return number ? n + "_" + std::to_string(number - 1) : n;
}

uint32_t ClassOf(uint32_t object) { return object ? Read32(object + g_L.cls) : 0; }

uint32_t FindClass(const char* name) {
  {
    std::lock_guard lock(g_mutex);
    auto it = g_classCache.find(name);
    if (it != g_classCache.end()) return it->second;
  }
  uint32_t found = 0;
  Refresh();
  ExtendCount();
  uint32_t index = 0, number = 0, classIndex = 0, classNumber = 0;
  if (NameKey(name, &index, &number) && NameKey("Class", &classIndex, &classNumber)) {
    std::vector<uint32_t> table(g_objectCount);
    if (g_objectCount && Copy(g_objects, table.data(), table.size() * 4))
      for (uint32_t raw : table) {
        const uint32_t o = Swap(raw);
        if (!o || !NameIs(o, index, number)) continue;
        const uint32_t c = ClassOf(o);
        if (c && Read32(c + g_L.name) == classIndex) { found = o; break; }
      }
  }
  if (found) {
    std::lock_guard lock(g_mutex);
    g_classCache[name] = found;
  }
  return found;
}

bool IsA(uint32_t object, uint32_t cls) {
  for (uint32_t c = ClassOf(object), guard = 0; c && guard < 64; c = Read32(c + g_L.super), guard++)
    if (c == cls) return true;
  return false;
}

namespace {
// "Name" or "Name_<n>" -> (name index, number); the names read once each (the table only grows)
bool NameKey(const std::string& full, uint32_t* index, uint32_t* number) {
  static std::mutex m;
  static std::unordered_map<std::string, uint32_t> byText;
  static uint32_t scanned = 0;
  std::string base = full;
  uint32_t num = 0;
  if (const size_t u = full.find_last_of('_'); g_L.name_number && u != std::string::npos && u + 1 < full.size() &&
                                                full.find_first_not_of("0123456789", u + 1) == std::string::npos &&
                                                (full.size() - u - 1 == 1 || full[u + 1] != '0')) {
    base = full.substr(0, u);
    num = uint32_t(std::stoul(full.substr(u + 1))) + 1;
  }
  std::lock_guard lock(m);
  if (!g_names) return false;
  for (uint32_t bad = 0; scanned < 2000000 && bad < 4096; scanned++) {
    const uint32_t entry = Read32(g_names + 4 * scanned);
    if (!entry) { bad++; continue; }
    bad = 0;
    byText.emplace(EntryName(entry), scanned);
  }
  scanned = scanned > 4096 ? scanned - 4096 : 0;   // (the tail may fill later: read again from before the gap)
  const auto it = byText.find(base);
  if (it == byText.end()) return false;
  *index = it->second;
  *number = num;
  return true;
}
}  // namespace

uint32_t FindObject(const char* name) {
  Refresh();
  ExtendCount();
  uint32_t index = 0, number = 0;
  if (!NameKey(name, &index, &number)) return 0;
  std::vector<uint32_t> table(g_objectCount);
  if (!g_objectCount || !Copy(g_objects, table.data(), table.size() * 4)) return 0;
  for (uint32_t raw : table) {
    const uint32_t o = Swap(raw);
    if (o && NameIs(o, index, number)) return o;
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
      for (uint32_t k = c, guard = 0; k && guard < 64; k = Read32(k + g_L.super), guard++)
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

int PropertyOffset(uint32_t cls, const char* property) {
  const uint32_t p = FindProperty(cls, property);
  return p ? int(Read32(p + g_L.prop_offset)) : -1;
}

int PropertySize(uint32_t cls, const char* property) {
  const uint32_t p = FindProperty(cls, property);
  return p ? int(Read32(p + g_L.elem_size)) : -1;
}

uint32_t BoolMask(uint32_t cls, const char* property) {
  const uint32_t p = FindProperty(cls, property);
  static const uint32_t boolProperty = FindClass("BoolProperty");
  return p && g_L.bool_mask && ClassOf(p) == boolProperty ? Read32(p + g_L.bool_mask) : 0;
}

}  // namespace ue3::obj
