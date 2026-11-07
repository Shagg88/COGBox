// Layer 2 (Unreal Engine 3): --ue3_ini_edits - engine settings changed through the game's own config bundle
// (ue3/config_bundle.h, notes/64). For each <game data>\<--ue3_ini_bundle_dir>\Coalesced_*.bin: read, edit, write to
// <user data>\ini-edits\, and show the game a view of the bundle folder in which those files are the edited copies.
//
// How the redirect works (job 256): the file system opens a file by resolving its *folder* through the symbolic links
// and then looking the name up among that folder's entries (VirtualFileSystem::OpenFile), so a link on a file's own
// path is never consulted - job 255's edits were written but never read. Instead the folder itself is linked to an
// overlay device: the same folder listing as the game's (every other file still the game's own, opened read-only from
// where it is), except that the edited bundles' entries point at our copies. The game's files are never written.
// Our code (MIT).
#include <ue3/config_bundle.h>
#include <ue3/main_menu_exit.h>
#include <ue3/package_exports.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/string/utf8.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <cctype>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <sstream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

REXCVAR_DEFINE_STRING(ue3_ini_edits, "", "Gears2",
                      "Engine config edits, 'file|section|key=value', 'file|section|key*=factor', 'file|section|-key=value' (remove one entry) or 'file|section|~=old>new' (replace text in every value), ';'-separated, e.g. "
                      "'Xe-GearEngine.ini|SystemSettings|MotionBlur=False' (ue3/config_bundle.h)");
REXCVAR_DEFINE_STRING(ue3_mods, "", "Gears2",
                      "Mods (0.2.0): host folders, ';'-separated in load order (a later one wins), each laid out like the game "
                      "data folder; their files replace or add to the game's, read-only, through a folder overlay");
// GitHub #87: the spoken language apart from the text. UE3 loads voices (and other localised assets) from
// <package>_LOC_<LANG>.xxx in the game's language; with --ue3_voice_language=INT (or FRA, ESM, ...) every
// *_LOC_<other>.xxx that has a <voice language> twin in the same folder is served as that twin through the mods'
// folder overlay - which also gives its table-of-contents line the twin's size and MD5 and fixes the start-up hash
// table. Menus and Coalesced text stay in the game's language; subtitles stored in the packages may follow the voices.
REXCVAR_DEFINE_STRING(ue3_voice_language, "", "Gears2",
                      "GitHub #87: the voices' language as the packages name it (INT, FRA, DEU, ITA, ESN, ESM, ...); "
                      "empty = the game's language");
REXCVAR_DEFINE_STRING(ue3_ini_bundle_dir, "GearGame\\CookedXenon", "Gears2",
                      "Folder of the Coalesced_*.bin config bundles, relative to the game data root");
REXCVAR_DEFINE_BOOL(gow_main_menu_exit, true, "ui", "Add Exit Game to the main menu; restart");
REXCVAR_DEFINE_BOOL(gow2_horde_bots, false, "Gears2",
                    "GitHub #237 (francoissubke-blip's tip, as Gears 3's --gow3_horde_bots): Back on the Horde scoreboard "
                    "adds two bots (local Horde)");
REXCVAR_DEFINE_BOOL(gow2_split_vertical, false, "Gears2",
                    "GitHub #237: the two-player split side by side (eSST_2P_VERTICAL) from the start: Default2PSplitType "
                    "and DesiredSplitscreenType in [Engine.GameViewportClient] of the engine ini (in memory)");

namespace {
constexpr const char* kGameDevice = "\\Device\\Harddisk0\\Partition1";
constexpr const char* kOverlayDevice = "\\Device\\Ue3IniOverlay";

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

// A folder entry whose children we fill ourselves (Entry's child list is only reachable from an entry class).
class OverlayFolder : public rex::filesystem::HostPathEntry {
 public:
  OverlayFolder(rex::filesystem::Device* device, rex::filesystem::Entry* parent, std::string_view path,
                const std::filesystem::path& host_path)
      : HostPathEntry(device, parent, path, host_path) {
    attributes_ = rex::filesystem::kFileAttributeDirectory;
  }
  void SetTimes(const rex::filesystem::FileInfo& info) {
    create_timestamp_ = info.create_timestamp;
    access_timestamp_ = info.access_timestamp;
    write_timestamp_ = info.write_timestamp;
  }
  void Add(rex::filesystem::Entry* child) { children_.push_back(std::unique_ptr<rex::filesystem::Entry>(child)); }
};

// Read-only view of `game_dir` in which the files named in `replaced` (lower case) come from `edit_dir`. A
// HostPathDevice so that its file entries open the way the game's own do (HostPathEntry::Open expects one).
class OverlayDevice : public rex::filesystem::HostPathDevice {
 public:
  OverlayDevice(std::string_view mount, std::filesystem::path game_dir, std::filesystem::path edit_dir,
                std::set<std::string> replaced)
      : HostPathDevice(mount, game_dir, true),
        game_dir_(std::move(game_dir)),
        edit_dir_(std::move(edit_dir)),
        replaced_(std::move(replaced)) {}
  bool Initialize() override {
    std::error_code ec;
    if (!std::filesystem::is_directory(game_dir_, ec)) return false;
    root_ = std::make_unique<OverlayFolder>(this, nullptr, "", game_dir_);
    Fill(root_.get(), game_dir_, true);
    return true;
  }
  rex::filesystem::Entry* ResolvePath(const std::string_view path) override { return root_->ResolvePath(path); }
  void Dump(rex::string::StringBuffer* out) override { root_->Dump(out, 0); }
  uint32_t replaced_count() const { return replaced_hits_; }

 private:
  void Fill(OverlayFolder* folder, const std::filesystem::path& dir, bool top) {
    for (const rex::filesystem::FileInfo& info : rex::filesystem::ListFiles(dir)) {
      const std::filesystem::path game_path = dir / info.name;
      if (info.type == rex::filesystem::FileInfo::Type::kDirectory) {
        auto* sub = new OverlayFolder(this, folder,
                                      rex::string::utf8_join_guest_paths(folder->path(), rex::path_to_utf8(info.name)),
                                      game_path);
        sub->SetTimes(info);
        folder->Add(sub);
        Fill(sub, game_path, false);
        continue;
      }
      rex::filesystem::FileInfo use = info;
      std::filesystem::path host = game_path;
      if (top && replaced_.count(Lower(rex::path_to_utf8(info.name)))) {
        rex::filesystem::FileInfo edited;
        if (rex::filesystem::GetInfo(edit_dir_ / info.name, &edited)) {
          use.total_size = edited.total_size;
          use.write_timestamp = edited.write_timestamp;
          host = edit_dir_ / info.name;
          ++replaced_hits_;
        }
      }
      folder->Add(rex::filesystem::HostPathEntry::Create(this, folder, host, use));
    }
  }
  std::filesystem::path game_dir_, edit_dir_;
  std::set<std::string> replaced_;
  std::unique_ptr<OverlayFolder> root_;
  uint32_t replaced_hits_ = 0;
};
// 0.2.1: a title update's loose config files (update:\TU__<name>.ini, read after the bundles) can set the same keys
// again - Gears 2 TU6's TU__GearCamera.ini puts RoadieRunShakyCamDampeningFactor back to 0.6, so camera shake Off did
// nothing there. The edits are applied to those files too, and update:\ shows the edited copies (a view like the bundles').
// GitHub #237: the input bindings our options add, in [Engine.PlayerInput] of a GearInput.ini (UE3 looks bindings up
// from the last one back, so an added one wins over the game's own for that button). Returns how many were added.
int AddInputBindings(ue3::ConfigFile& f, const char* key) {
  std::vector<const char*> binds;
  if (REXCVAR_GET(gow2_horde_bots))
    binds.push_back("(Name=\"XboxTypeS_Back\",Command=\"Set WorldInfo NetMode NM_Standalone | EnableCheats | AddBot 0 3 | AddBot 0 4\")");
  if (binds.empty()) return 0;
  const bool wide = f.name.wide;
  ue3::ConfigSection* sec = nullptr;
  for (ue3::ConfigSection& c : f.sections)
    if (Lower(c.name.Utf8()) == "engine.playerinput") sec = &c;
  if (!sec) {
    f.sections.push_back({});
    sec = &f.sections.back();
    sec->name = ue3::ConfigString::FromUtf8("Engine.PlayerInput", wide);
  }
  for (const char* b : binds)
    sec->keys.push_back({ue3::ConfigString::FromUtf8(key, wide), ue3::ConfigString::FromUtf8(b, wide)});
  return int(binds.size());
}
bool InputBindingsWanted() { return REXCVAR_GET(gow2_horde_bots) || REXCVAR_GET(gow2_split_vertical); }

// GitHub #237: side-by-side split from the start. Sets the viewport client's split defaults in the engine ini
// (GameViewportClient is config(Engine); a GearGame subclass inherits them unless its own section sets them).
int SetVerticalSplit(ue3::ConfigFile& f) {
  if (!REXCVAR_GET(gow2_split_vertical)) return 0;
  const bool wide = f.name.wide;
  for (const ue3::ConfigSection& c : f.sections)   // what the game has, for the next fix if this is not the key
    if (Lower(c.name.Utf8()).find("viewportclient") != std::string::npos)
      for (const ue3::ConfigKey& k : c.keys)
        REXLOG_INFO("gow2_split_vertical:   {} [{}] {}={}", f.name.Utf8(), c.name.Utf8(), k.key.Utf8(), k.value.Utf8());
  int n = 0;
  for (ue3::ConfigSection& c : f.sections) {
    if (Lower(c.name.Utf8()).find("viewportclient") == std::string::npos) continue;
    for (ue3::ConfigKey& k : c.keys) {
      const std::string kl = Lower(k.key.Utf8());
      if (kl == "default2psplittype" || kl == "desiredsplitscreentype") {
        k.value = ue3::ConfigString::FromUtf8("eSST_2P_VERTICAL", wide);
        ++n;
      }
    }
  }
  ue3::ConfigSection* sec = nullptr;
  for (ue3::ConfigSection& c : f.sections)
    if (Lower(c.name.Utf8()) == "engine.gameviewportclient") sec = &c;
  if (!sec) {
    f.sections.push_back({});
    sec = &f.sections.back();
    sec->name = ue3::ConfigString::FromUtf8("Engine.GameViewportClient", wide);
  }
  for (const char* key : {"Default2PSplitType", "DesiredSplitscreenType"}) {
    bool have = false;
    for (const ue3::ConfigKey& k : sec->keys) have |= Lower(k.key.Utf8()) == Lower(key);
    if (!have) {
      sec->keys.push_back({ue3::ConfigString::FromUtf8(key, wide), ue3::ConfigString::FromUtf8("eSST_2P_VERTICAL", wide)});
      ++n;
    }
  }
  return n;
}

void EditTitleUpdateInis(rex::Runtime* runtime, const std::vector<ue3::ConfigEdit>& edits) {
  namespace fs = std::filesystem;
  const fs::path root = runtime->update_data_root();
  std::error_code ec;
  if (root.empty() || !fs::is_directory(root, ec)) return;
  const fs::path out = runtime->user_data_root() / "ini-edits" / "update";
  std::set<std::string> replaced;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    const std::string n = rex::path_to_utf8(e.path().filename());
    const std::string ln = Lower(n);
    if (!e.is_regular_file() || ln.rfind("tu__", 0) != 0 || !ln.ends_with(".ini")) continue;
    std::ifstream in(e.path(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= 2 && (uint8_t(text[0]) == 0xFF || uint8_t(text[0]) == 0xFE)) continue;  // UTF-16: not seen in a TU
    ue3::ConfigBundle b;
    b.files.emplace_back();
    ue3::ConfigFile& f = b.files.back();
    f.name = ue3::ConfigString::FromUtf8(n, false);
    std::istringstream ls(text);
    std::string line;
    while (std::getline(ls, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
      if (line.empty() || line[0] == ';') continue;
      if (line.front() == '[' && line.back() == ']') {
        f.sections.emplace_back();
        f.sections.back().name = ue3::ConfigString::FromUtf8(line.substr(1, line.size() - 2), false);
        continue;
      }
      const size_t eq = line.find('=');
      if (eq == std::string::npos || f.sections.empty()) continue;
      ue3::ConfigKey k;
      k.key = ue3::ConfigString::FromUtf8(line.substr(0, eq), false);
      k.value = ue3::ConfigString::FromUtf8(line.substr(eq + 1), false);
      f.sections.back().keys.push_back(std::move(k));
    }
    std::vector<std::string> log;
    int tu_changed = edits.empty() ? 0 : ue3::ApplyConfigEdits(b, edits, &log);
    if (ln.find("gearinput.ini") != std::string::npos) {   // GitHub #237: TU__Xe-GearInput.ini
      const int added = AddInputBindings(f, "+Bindings");
      if (added) REXLOG_INFO("gow2 input bindings: title update {}: {} binding(s) added", n, added);
      tu_changed += added;
    }
    if (ln.find("gearengine.ini") != std::string::npos) tu_changed += SetVerticalSplit(f);   // GitHub #237
    if (tu_changed == 0) continue;
    for (const std::string& l : log) REXLOG_INFO("ue3_ini_edits: title update: {}", l);
    std::string o;
    for (const ue3::ConfigSection& sec : f.sections) {
      o += "[" + sec.name.Utf8() + "]\r\n";
      for (const ue3::ConfigKey& k : sec.keys) o += k.key.Utf8() + "=" + k.value.Utf8() + "\r\n";
      o += "\r\n";
    }
    fs::create_directories(out, ec);
    std::ofstream w(out / n, std::ios::binary | std::ios::trunc);
    w.write(o.data(), std::streamsize(o.size()));
    if (!w) { REXLOG_WARN("ue3_ini_edits: could not write {}", rex::path_to_utf8(out / n)); continue; }
    replaced.insert(ln);
  }
  if (replaced.empty()) return;
  constexpr const char* kUpdateOverlay = "\\Device\\Ue3IniUpdateOverlay";
  auto dev = std::make_unique<OverlayDevice>(kUpdateOverlay, root, out, replaced);
  OverlayDevice* view = dev.get();
  if (!dev->Initialize() || !runtime->file_system()->RegisterDevice(std::move(dev))) {
    REXLOG_WARN("ue3_ini_edits: could not set up the title update's view; it reads its own config files");
    return;
  }
  runtime->file_system()->UnregisterSymbolicLink("update:");
  runtime->file_system()->RegisterSymbolicLink("update:", kUpdateOverlay);
  REXLOG_INFO("ue3_ini_edits: title update: {} file(s) edited, {} shown through update:", replaced.size(), view->replaced_count());
}

// GitHub #116: the image's address range is not all readable (a 0x83xxxxxx page of the TU6 image faulted on one PC,
// 0xC0000005 at start-up right after the first bundle was edited). Only the pages the guest heap has readable are
// scanned, run by run, and a fault in a run is caught and that run skipped instead of taking the game down.
void ScanRunInto(const uint8_t* data, size_t size, const std::string* name, const ue3::Sha1Digest* hash,
                 std::vector<size_t>* out) {
  *out = ue3::FindFileHash(data, size, *name, *hash);
}
bool ScanRunGuarded(const uint8_t* data, size_t size, const std::string* name, const ue3::Sha1Digest* hash,
                    std::vector<size_t>* out) {
#if defined(_MSC_VER)
  __try {
    ScanRunInto(data, size, name, hash, out);
    return true;
  } __except (1) {
    return false;
  }
#else
  ScanRunInto(data, size, name, hash, out);
  return true;
#endif
}
std::vector<size_t> FindFileHashReadable(rex::Runtime* runtime, uint32_t base, uint32_t size, const std::string& name,
                                         const ue3::Sha1Digest& hash) {
  std::vector<size_t> found;
  const uint8_t* image = runtime->virtual_membase() + base;
  rex::memory::BaseHeap* heap = runtime->memory()->LookupHeap(base);
  const uint32_t page = heap ? heap->page_size() : 0;
  if (!heap || page == 0) return found;
  auto readable = [&](uint32_t a) {
    uint32_t p = 0;
    return heap->QueryProtect(a, &p) && (p & rex::memory::kMemoryProtectRead) != 0;
  };
  uint32_t off = 0, skipped = 0;
  while (off < size) {
    if (!readable(base + off)) { off += page; ++skipped; continue; }
    uint32_t end = off;
    while (end < size && readable(base + end)) end += page;
    end = std::min(end, size);
    std::vector<size_t> run;
    if (ScanRunGuarded(image + off, end - off, &name, &hash, &run)) {
      for (size_t r : run) found.push_back(off + r);
    } else {
      REXLOG_WARN("ue3 hash table: a fault reading image {:08X}-{:08X}; that part is skipped", base + off, base + end);
    }
    off = end;
  }
  static bool logged = false;
  if (skipped && !logged) {
    logged = true;
    REXLOG_INFO("ue3 hash table: {} unreadable page(s) of the image skipped (GitHub #116)", skipped);
  }
  return found;
}
// The game image's start-up file hash table (ue3/config_bundle.h): give each edited bundle its new hash, so the
// game's own check passes instead of showing the dirty-disc error (job 257). Image at 0x82000000, size from its PE
// header (as XenosDumpImageIfRequested). The table sits in read-only data: its page is made writable for the write
// and protected again after.
int UpdateHashTable(rex::Runtime* runtime, const std::string& name, const ue3::Sha1Digest& old_hash,
                    const ue3::Sha1Digest& new_hash) {
  constexpr uint32_t kBase = 0x82000000u;
  const uint8_t* image = runtime->virtual_membase() + kBase;
  uint32_t size = 0, pe = 0;
  if (image[0] != 'M' || image[1] != 'Z') return -1;
  std::memcpy(&pe, image + 0x3C, 4);
  if (pe == 0 || pe >= 0x1000 || std::memcmp(image + pe, "PE\0\0", 4) != 0) return -1;
  std::memcpy(&size, image + pe + 24 + 56, 4);
  if (size == 0 || size > 0x08000000u) return -1;
  int changed = 0;
  for (size_t off : FindFileHashReadable(runtime, kBase, size, Lower(name), old_hash)) {
    const uint32_t addr = kBase + uint32_t(off);
    rex::memory::BaseHeap* heap = runtime->memory()->LookupHeap(addr);
    if (!heap) continue;
    const uint32_t page = heap->page_size();
    const uint32_t first = addr & ~(page - 1), last = (addr + 19) & ~(page - 1);
    const uint32_t span = last - first + page;
    uint32_t old_protect = 0;
    heap->QueryProtect(first, &old_protect);
    if (!heap->Protect(first, span, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) continue;
    std::memcpy(runtime->memory()->TranslateVirtual<uint8_t*>(addr), new_hash.data(), new_hash.size());
    if (old_protect) heap->Protect(first, span, old_protect);
    ++changed;
  }
  return changed;
}

// ---- 0.2.0: mods ------------------------------------------------------------------------------------------------
// --ue3_mods: each mod folder mirrors the game data folder (e.g. <mod>\GearGame\CookedXenon\SomePackage.xxx). Every file
// in it replaces the game's file of the same path, or is added beside the game's; a later mod wins. Nothing is written
// into the game's folder: each top-level game folder a mod touches (e.g. GearGame) is shown to the game through an
// overlay device (the same mechanism as the config edits above, but replacing at any depth). Programs (.xex, .xexp,
// .dll, .exe) are never taken from a mod. Each replaced or added file also gets its line in the game's table of
// contents (<top folder>\*TOC*.txt: size, uncompressed size, sector, path, MD5) - a copy, in <user data>\mod-overlay.

// MD5 (RFC 1321), for the table of contents.
std::string Md5Hex(const std::filesystem::path& file) {
  static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8,
      0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
      0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87,
      0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
      0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039,
      0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
      0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
      0xeb86d391};
  static const uint32_t R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                 14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  auto block = [&](const uint8_t* p) {
    uint32_t w[16];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 | uint32_t(p[i * 4 + 3]) << 24;
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; ++i) {
      uint32_t f, g;
      if (i < 16) f = (b & c) | (~b & d), g = i;
      else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
      else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
      else f = c ^ (b | ~d), g = (7 * i) % 16;
      const uint32_t t = d;
      d = c;
      c = b;
      const uint32_t x = a + f + K[i] + w[g];
      b = b + ((x << R[i]) | (x >> (32 - R[i])));
      a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
  };
  std::ifstream in(file, std::ios::binary);
  std::vector<uint8_t> buf(1 << 20);
  uint64_t total = 0;
  size_t carry = 0;
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data() + carry), std::streamsize(buf.size() - carry));
    const size_t n = carry + size_t(in.gcount());
    total += uint64_t(in.gcount());
    size_t off = 0;
    for (; off + 64 <= n; off += 64) block(buf.data() + off);
    carry = n - off;
    std::memmove(buf.data(), buf.data() + off, carry);
    if (in.gcount() == 0) break;
  }
  uint8_t tail[128] = {};
  std::memcpy(tail, buf.data(), carry);
  tail[carry] = 0x80;
  const size_t len = carry < 56 ? 64 : 128;
  const uint64_t bits = total * 8;
  for (int i = 0; i < 8; ++i) tail[len - 8 + i] = uint8_t(bits >> (8 * i));
  block(tail);
  if (len == 128) block(tail + 64);
  static const char* hx = "0123456789abcdef";
  std::string out;
  for (uint32_t v : h)
    for (int i = 0; i < 4; ++i) out += hx[(v >> (8 * i) >> 4) & 15], out += hx[(v >> (8 * i)) & 15];
  return out;
}

std::string ModLower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

std::string RelKey(std::string rel) {
  for (char& c : rel) c = c == '/' ? '\\' : char(std::tolower(uint8_t(c)));
  return rel;
}

// A read-only view of one top-level game folder in which `files` (lower-case path relative to that folder ->
// host file) replace or add to the game's files, at any depth; `dirs` (same keys) are folders that only a mod has.
class ModOverlayDevice : public rex::filesystem::HostPathDevice {
 public:
  ModOverlayDevice(std::string_view mount, std::filesystem::path game_dir, std::map<std::string, std::filesystem::path> files,
                   std::map<std::string, std::filesystem::path> dirs)
      : HostPathDevice(mount, game_dir, true), game_dir_(std::move(game_dir)), files_(std::move(files)), dirs_(std::move(dirs)) {}
  bool Initialize() override {
    std::error_code ec;
    if (!std::filesystem::is_directory(game_dir_, ec)) return false;
    root_ = std::make_unique<OverlayFolder>(this, nullptr, "", game_dir_);
    Fill(root_.get(), game_dir_, "");
    return true;
  }
  rex::filesystem::Entry* ResolvePath(const std::string_view path) override { return root_->ResolvePath(path); }
  void Dump(rex::string::StringBuffer* out) override { root_->Dump(out, 0); }
  uint32_t replaced() const { return replaced_; }
  uint32_t added() const { return added_; }

 private:
  static std::string Join(const std::string& rel, const std::string& name) { return rel.empty() ? name : rel + "\\" + name; }
  void Fill(OverlayFolder* folder, const std::filesystem::path& dir, const std::string& rel) {
    std::set<std::string> seen;
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec))
      for (const rex::filesystem::FileInfo& info : rex::filesystem::ListFiles(dir)) {
        const std::string name = rex::path_to_utf8(info.name);
        const std::string key = Join(rel, ModLower(name));
        seen.insert(ModLower(name));
        if (info.type == rex::filesystem::FileInfo::Type::kDirectory) {
          auto* sub = new OverlayFolder(this, folder, rex::string::utf8_join_guest_paths(folder->path(), name), dir / info.name);
          sub->SetTimes(info);
          folder->Add(sub);
          Fill(sub, dir / info.name, key);
          continue;
        }
        rex::filesystem::FileInfo use = info;
        std::filesystem::path host = dir / info.name;
        auto it = files_.find(key);
        rex::filesystem::FileInfo modinfo;
        if (it != files_.end() && rex::filesystem::GetInfo(it->second, &modinfo)) {
          use.total_size = modinfo.total_size;
          use.write_timestamp = modinfo.write_timestamp;
          host = it->second;
          ++replaced_;
        }
        folder->Add(rex::filesystem::HostPathEntry::Create(this, folder, host, use));
      }
    // what only the mods have, directly in this folder
    const std::string prefix = rel.empty() ? "" : rel + "\\";
    for (const auto& [key, host] : dirs_) {
      if (key.compare(0, prefix.size(), prefix) != 0 || key.find('\\', prefix.size()) != std::string::npos) continue;
      const std::string lname = key.substr(prefix.size());
      if (seen.count(lname)) continue;
      seen.insert(lname);
      rex::filesystem::FileInfo info;
      if (!rex::filesystem::GetInfo(host, &info)) continue;
      const std::string name = rex::path_to_utf8(host.filename());
      auto* sub = new OverlayFolder(this, folder, rex::string::utf8_join_guest_paths(folder->path(), name), host);
      sub->SetTimes(info);
      folder->Add(sub);
      Fill(sub, dir / host.filename(), key);
    }
    for (const auto& [key, host] : files_) {
      if (key.compare(0, prefix.size(), prefix) != 0 || key.find('\\', prefix.size()) != std::string::npos) continue;
      if (seen.count(key.substr(prefix.size()))) continue;
      rex::filesystem::FileInfo info;
      if (!rex::filesystem::GetInfo(host, &info)) continue;
      folder->Add(rex::filesystem::HostPathEntry::Create(this, folder, host, info));
      ++added_;
    }
  }
  std::filesystem::path game_dir_;
  std::map<std::string, std::filesystem::path> files_, dirs_;
  std::unique_ptr<OverlayFolder> root_;
  uint32_t replaced_ = 0, added_ = 0;
};

REXCVAR_DEFINE_INT32(ue3_voice_language_log, 40, "Gears2",
                     "GitHub #90: with --ue3_voice_language, log why a localised package keeps its own language (a table "
                     "unreadable, or the twin lacks an object it exports - the first missing "
                     "one named), up to N lines. Logging only. 0 = only the summary");
// rel (lower case, '\\', from the game data root) -> host file; later mods win. Programs are refused.
// GitHub #87 (see ue3_voice_language): adds "<rel of *_LOC_<lang>.xxx>" -> its <voice> twin to the mods' files (a real
// mod's own copy of a file wins). Returns how many were added.
int AddVoiceLanguage(const std::filesystem::path& game, std::map<std::string, std::filesystem::path>* files) {
  namespace fs = std::filesystem;
  std::string voice = REXCVAR_GET(ue3_voice_language);
  for (char& c : voice) c = char(std::toupper(uint8_t(c)));
  if (voice.size() != 3) return 0;
  // GitHub #216 / #217: with the text language set to the same language (--user_language), the game already loads that
  // language's packages and voices itself; swapping the other languages' packages as well crashed it at start-up
  // (guest write of 0x00000003 right after GearGame_LOC_<voice> opened). Nothing to do then.
  {
    const std::string ul = rex::cvar::GetFlagByName("user_language");
    const char* text = ul == "3" ? "DE" : ul == "4" ? "FR" : ul == "5" ? "ES" : ul == "6" ? "IT" : nullptr;
    if (text && voice.compare(0, 2, text) == 0) {
      REXLOG_INFO("ue3_voice_language: {} - the text language (--user_language={}) already gives these voices; no packages "
                  "swapped (GitHub #216)", voice, ul);
      return 0;
    }
  }
  std::error_code ec;
  if (!fs::is_directory(game, ec)) return 0;
  // folder -> lower-case name -> real path, for the twin lookup
  std::map<fs::path, std::map<std::string, fs::path>> byDir;
  for (auto it = fs::recursive_directory_iterator(game, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::string n = rex::path_to_utf8(it->path().filename());
    for (char& c : n) c = char(std::tolower(uint8_t(c)));
    if (n.size() > 12 && n.find("_loc_") != std::string::npos && n.ends_with(".xxx")) byDir[it->path().parent_path()][n] = it->path();
  }
  std::string lv = voice;
  for (char& c : lv) c = char(std::tolower(uint8_t(c)));
  int added = 0, langs = 0, kept = 0, no_twin = 0;
  int kept_no_twin_table = 0, kept_no_own_table = 0, kept_missing = 0;
  const int log_max = std::max(REXCVAR_GET(ue3_voice_language_log), 0);
  int logged = 0;
  auto why = [&](const fs::path& pkg, const std::string& reason) {   // GitHub #90: per-package reason, bounded
    if (logged >= log_max) return;
    ++logged;
    const fs::path rel = fs::relative(pkg, game, ec);
    REXLOG_INFO("ue3_voice_language: {} keeps its own language: {}{}", rex::path_to_utf8(ec ? pkg.filename() : rel), reason,
                logged == log_max ? " (last line of this log - ue3_voice_language_log)" : "");
  };
  std::set<std::string> seenLangs;
  std::map<fs::path, std::pair<bool, std::set<std::string>>> twinExports;
  for (const auto& [dir, names] : byDir)
    for (const auto& [n, p] : names) {
      const size_t at = n.rfind("_loc_");
      if (at == std::string::npos || n.size() != at + 12) continue;   // <base>_loc_<xxx>.xxx
      const std::string lang = n.substr(at + 5, 3);
      if (lang == lv) continue;
      const auto twin = names.find(n.substr(0, at + 5) + lv + ".xxx");
      if (twin == names.end()) {
        ++no_twin;   // counted only: every other language's copy of a package would flood the log
        continue;
      }
      const fs::path relp = fs::relative(p, game, ec);
      if (ec || relp.empty()) continue;
      const std::string key = RelKey(rex::path_to_utf8(relp));
      if (files->count(key)) continue;
      // GitHub #145: only a twin with every object this package exports. GearGame_LOC_ESM lacks one sound the other
      // languages' GearGame_LOC have; given it, the game read a null object at start-up. When either table cannot be
      // read, the package keeps its own language.
      std::set<std::string> mine;
      auto theirs = twinExports.find(twin->second);   // one twin serves every other language: read it once
      if (theirs == twinExports.end()) {
        std::set<std::string> paths;
        const bool ok = ue3::ReadPackageExportPaths(twin->second, &paths);
        theirs = twinExports.emplace(twin->second, std::make_pair(ok, std::move(paths))).first;
      }
      if (!theirs->second.first) {
        ++kept, ++kept_no_twin_table;
        why(p, "the " + voice + " twin's export table could not be read (" + ue3::DescribePackageSummary(twin->second) + ")");
        continue;
      }
      if (!ue3::ReadPackageExportPaths(p, &mine)) {
        ++kept, ++kept_no_own_table;
        why(p, "its own export table could not be read (" + ue3::DescribePackageSummary(p) + ")");
        continue;
      }
      if (!std::includes(theirs->second.second.begin(), theirs->second.second.end(), mine.begin(), mine.end())) {
        ++kept, ++kept_missing;
        size_t missing = 0;
        std::string first;
        for (const std::string& e : mine)
          if (!theirs->second.second.count(e)) {
            if (first.empty()) first = e;
            ++missing;
          }
        why(p, std::format("the {} twin lacks {} object(s) it exports (first: {})", voice, missing, first));
        continue;
      }
      (*files)[key] = twin->second;
      ++added;
      if (seenLangs.insert(lang).second) ++langs;
    }
  REXLOG_INFO("ue3_voice_language: {} - {} localised package(s) in {} other language(s) now give the {} voices (GitHub #87); "
              "{} kept their own: their {} twin lacks objects they export, or a table could not be read (GitHub #145)",
              voice, added, langs, voice, kept, voice);
  REXLOG_INFO("ue3_voice_language: kept own language (GitHub #90): {} twin lacks objects, {} twin table unreadable, {} own "
              "table unreadable; {} other-language package(s) have no {} twin at all",
              kept_missing, kept_no_twin_table, kept_no_own_table, no_twin, voice);
  return added;
}
std::map<std::string, std::filesystem::path> CollectMods(std::map<std::string, std::filesystem::path>* dirs) {
  namespace fs = std::filesystem;
  std::map<std::string, fs::path> out;
  std::string spec = REXCVAR_GET(ue3_mods);
  size_t at = 0;
  while (at <= spec.size()) {
    size_t end = spec.find(';', at);
    if (end == std::string::npos) end = spec.size();
    std::string one = spec.substr(at, end - at);
    at = end + 1;
    while (!one.empty() && (one.back() == ' ' || one.back() == '"')) one.pop_back();
    while (!one.empty() && (one.front() == ' ' || one.front() == '"')) one.erase(0, 1);
    if (one.empty()) continue;
    const fs::path root = rex::to_path(one);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
      REXLOG_WARN("ue3_mods: {} is not a folder - skipped", one);
      continue;
    }
    int n = 0;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_symlink(ec)) { it.disable_recursion_pending(); continue; }   // never follow links out of the mod
      const fs::path relp = fs::relative(it->path(), root, ec);
      if (ec || relp.empty() || relp.is_absolute() || *relp.begin() == "..") continue;
      const std::string key = RelKey(rex::path_to_utf8(relp));
      if (it->is_directory(ec)) {
        (*dirs)[key] = it->path();
        continue;
      }
      if (key.find('\\') == std::string::npos) continue;   // files at the mod's top (mod.ini, readme) are its own
      const std::string ext = ModLower(it->path().extension().string());
      if (ext == ".xex" || ext == ".xexp" || ext == ".dll" || ext == ".exe") {
        REXLOG_WARN("ue3_mods: {}: programs are not taken from mods - skipped", rex::path_to_utf8(it->path()));
        continue;
      }
      out[key] = it->path();
      ++n;
    }
    REXLOG_INFO("ue3_mods: {}: {} file(s)", one, n);
  }
  return out;
}

// Puts the mod files (and the config edits' copies, when both are on) in front of the game's. `game` is the game data
// root; `files` keys are relative to it.
void InstallModOverlay(rex::Runtime* runtime, std::map<std::string, std::filesystem::path> files,
                       std::map<std::string, std::filesystem::path> dirs) {
  namespace fs = std::filesystem;
  const fs::path game = runtime->game_data_root();
  const fs::path out_dir = runtime->user_data_root() / "mod-overlay";
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  // the start-up hash table: a replaced file named there gets its new hash
  int hashed = 0;
  for (const auto& [key, host] : files) {
    const fs::path orig = game / rex::to_path(key);
    if (!fs::is_regular_file(orig, ec) || rex::path_to_utf8(host).rfind(rex::path_to_utf8(runtime->user_data_root() / "ini-edits"), 0) == 0) continue;
    if (fs::file_size(orig, ec) > (64u << 20) || fs::file_size(host, ec) > (64u << 20)) continue;
    auto read = [](const fs::path& p) {
      std::ifstream in(p, std::ios::binary);
      return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
    };
    const std::vector<uint8_t> a = read(orig), b = read(host);
    const std::string name = ModLower(rex::path_to_utf8(orig.filename()));   // the game's name (GitHub #87: a voice twin is named otherwise)
    const int n = UpdateHashTable(runtime, name, ue3::Sha1(a.data(), a.size()), ue3::Sha1(b.data(), b.size()));
    if (n > 0) hashed += n;
  }
  // by top-level folder
  std::map<std::string, std::map<std::string, fs::path>> byTop, dirsByTop;
  for (const auto& [key, host] : files) {
    const size_t s = key.find('\\');
    byTop[key.substr(0, s)][key.substr(s + 1)] = host;
  }
  for (const auto& [key, host] : dirs) {
    const size_t s = key.find('\\');
    if (s != std::string::npos) dirsByTop[key.substr(0, s)][key.substr(s + 1)] = host;
  }
  int devices = 0;
  for (auto& [top, map] : byTop) {
    fs::path topdir;
    for (const auto& e : fs::directory_iterator(game, ec))
      if (e.is_directory() && ModLower(rex::path_to_utf8(e.path().filename())) == top) topdir = e.path();
    if (topdir.empty()) {
      REXLOG_WARN("ue3_mods: the game has no folder '{}' - {} file(s) there skipped", top, map.size());
      continue;
    }
    // the table of contents: replaced files get their new size and MD5, added files a new line
    for (const auto& e : fs::directory_iterator(topdir, ec)) {
      const std::string tname = rex::path_to_utf8(e.path().filename());
      const std::string tl = ModLower(tname);
      if (!e.is_regular_file() || tl.find("toc") == std::string::npos || tl.size() < 4 || tl.substr(tl.size() - 4) != ".txt") continue;
      const auto edited = map.find(tl);   // the config edits' copy of this table of contents, when they made one
      std::ifstream in(edited != map.end() ? edited->second : e.path(), std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(in)), {}), outText;
      std::set<std::string> listed;
      size_t p = 0;
      int changed = 0;
      while (p < text.size()) {
        size_t nl = text.find('\n', p);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(p, nl - p);
        p = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string size, usize, sector, path, md5;
        if (ls >> size >> usize >> sector >> path) {
          ls >> md5;
          std::string k = RelKey(path);
          while (k.rfind("..\\", 0) == 0) k.erase(0, 3);
          listed.insert(k);
          auto it = files.find(k);
          if (it != files.end()) {
            line = std::to_string(fs::file_size(it->second, ec)) + " 0 " + sector + " " + path + " " + Md5Hex(it->second);
            ++changed;
          }
        }
        outText += line + "\r\n";
      }
      for (const auto& [k, host] : files) {
        if (listed.count(k) || k.rfind(top + "\\", 0) != 0) continue;
        std::string rel = rex::path_to_utf8(host.filename());
        std::string guest = "..\\" + rex::path_to_utf8(topdir.filename()) + "\\" + k.substr(top.size() + 1);
        guest = guest.substr(0, guest.size() - rel.size()) + rel;   // keep the mod file's own name casing
        outText += std::to_string(fs::file_size(host, ec)) + " 0 0 " + guest + " " + Md5Hex(host) + "\r\n";
        ++changed;
      }
      if (!changed) continue;
      const fs::path tocOut = out_dir / tname;
      std::ofstream o(tocOut, std::ios::binary | std::ios::trunc);
      o << outText;
      if (o) map[tl] = tocOut;
      REXLOG_INFO("ue3_mods: {}: {} line(s) changed or added (given the replaced files' size and MD5)", tname, changed);
    }
    const std::string devname = std::string("\\Device\\Ue3ModOverlay") + std::to_string(devices);
    auto dev = std::make_unique<ModOverlayDevice>(devname, topdir, map, dirsByTop[top]);
    ModOverlayDevice* view = dev.get();
    if (!dev->Initialize() || !runtime->file_system()->RegisterDevice(std::move(dev))) {
      REXLOG_WARN("ue3_mods: could not set up the view of {} - the game reads its own files there", top);
      continue;
    }
    runtime->file_system()->RegisterSymbolicLink(std::string(kGameDevice) + "\\" + rex::path_to_utf8(topdir.filename()), devname);
    ++devices;
    REXLOG_INFO("ue3_mods: {}: {} file(s) replaced, {} added", rex::path_to_utf8(topdir.filename()), view->replaced(), view->added());
  }
  REXLOG_INFO("ue3_mods: {} folder view(s); {} hash table entries updated", devices, hashed);
}
ue3::GameId DetermineGameId(rex::Runtime* runtime) {
  if (runtime && runtime->kernel_state()) {
    const uint32_t tid = runtime->kernel_state()->title_id();
    if (tid == 0x4D53082D) return ue3::GameId::Gears2;
    if (tid == 0x4D5308AB) return ue3::GameId::Gears3;
    if (tid == 0x4D530A26) return ue3::GameId::Judgment;
  }
  if (runtime) {
    const std::string root = Lower(rex::path_to_utf8(runtime->game_data_root()));
    if (root.find("gears3") != std::string::npos || root.find("gow3") != std::string::npos) return ue3::GameId::Gears3;
    if (root.find("judgment") != std::string::npos || root.find("gearsj") != std::string::npos || root.find("gowj") != std::string::npos) return ue3::GameId::Judgment;
    if (root.find("gears2") != std::string::npos || root.find("gow2") != std::string::npos) return ue3::GameId::Gears2;
  }
  const std::string bdir = Lower(REXCVAR_GET(ue3_ini_bundle_dir));
  if (bdir.find("gears3") != std::string::npos || bdir.find("gow3") != std::string::npos) return ue3::GameId::Gears3;
  if (bdir.find("judgment") != std::string::npos || bdir.find("gearsj") != std::string::npos || bdir.find("gowj") != std::string::npos) return ue3::GameId::Judgment;
  return ue3::GameId::Gears2;
}
}  // namespace

void Ue3InstallConfigEditsIfRequested(rex::Runtime* runtime) {
  if (!runtime || !runtime->file_system()) return;
  namespace fs = std::filesystem;
  // 0.2.0: mods. With mods on, the config edits' copies join the mods' overlay instead of their own folder view.
  std::map<std::string, fs::path> modDirs;
  std::map<std::string, fs::path> mods = CollectMods(&modDirs);
  AddVoiceLanguage(runtime->game_data_root(), &mods);  // GitHub #87
  struct ModsAtEnd {
    rex::Runtime* rt; std::map<std::string, fs::path>* files; std::map<std::string, fs::path>* dirs;
    ~ModsAtEnd() {   // a destructor must not throw: a file-system error here would end the game at start-up
      if (files->empty()) return;
      try {
        InstallModOverlay(rt, *files, *dirs);
      } catch (const std::exception& e) {
        REXLOG_WARN("ue3_mods: the mods could not be set up ({}) - the game reads its own files", e.what());
      }
    }
  } modsAtEnd{runtime, &mods, &modDirs};
  const std::string spec = REXCVAR_GET(ue3_ini_edits);
  const bool exit_menu = REXCVAR_GET(gow_main_menu_exit);
  const bool binds = InputBindingsWanted();   // GitHub #237
  if (spec.empty() && !exit_menu && !binds) return;
  std::string error;
  const std::vector<ue3::ConfigEdit> edits = spec.empty() ? std::vector<ue3::ConfigEdit>{} : ue3::ParseConfigEdits(spec, &error);
  if (!error.empty()) REXLOG_WARN("ue3_ini_edits: {}", error);
  if (edits.empty() && !exit_menu && !binds) return;
  if (!edits.empty() || binds) EditTitleUpdateInis(runtime, edits);   // 0.2.1
  std::string rel = REXCVAR_GET(ue3_ini_bundle_dir);
  const fs::path src_dir = runtime->game_data_root() / fs::path(rel);
  const fs::path out_dir = runtime->user_data_root() / "ini-edits";
  std::error_code ec;
  if (!fs::is_directory(src_dir, ec)) {
    REXLOG_WARN("ue3_ini_edits: {} is not a folder; nothing edited", src_dir.string());
    return;
  }
  fs::create_directories(out_dir, ec);
  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(src_dir, ec)) {
    const std::string n = e.path().filename().string();
    if (e.is_regular_file() && n.size() > 14 && n.rfind("Coalesced_", 0) == 0 && n.ends_with(".bin")) names.push_back(n);
  }
  if (names.empty()) {
    REXLOG_WARN("ue3_ini_edits: no Coalesced_*.bin in {}", src_dir.string());
    return;
  }
  int written = 0, hashed = 0;
  for (const std::string& n : names) {
    // a mod's own copy of the bundle, if one has it, is the one edited (0.2.0)
    const auto modCopy = mods.find(RelKey(rel + "\\" + n));
    std::ifstream in(modCopy != mods.end() ? modCopy->second : src_dir / n, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    std::vector<uint8_t> original;
    if (modCopy != mods.end()) {
      std::ifstream gin(src_dir / n, std::ios::binary);
      original.assign(std::istreambuf_iterator<char>(gin), {});
    }
    ue3::ConfigBundle bundle;
    std::string perr;
    if (!ue3::ParseConfigBundle(bytes, bundle, &perr)) {
      REXLOG_WARN("ue3_ini_edits: {}: {} - left as it is", n, perr);
      continue;
    }
    if (ue3::WriteConfigBundle(bundle) != bytes) {  // our writer must reproduce the file exactly before we edit it
      REXLOG_WARN("ue3_ini_edits: {}: re-writing it does not reproduce the original - left as it is", n);
      continue;
    }
    std::vector<std::string> log;
    int changed = 0;
    if (!edits.empty()) {
      changed += ue3::ApplyConfigEdits(bundle, edits, &log);
    }
    if (binds) {   // GitHub #237: the disc bundle's GearInput.ini too (no title update, or one without it)
      ue3::ConfigFile* target = nullptr;
      for (ue3::ConfigFile& f : bundle.files) {
        const std::string fn = Lower(f.name.Utf8());
        if (fn.find("xe-gearinput.ini") != std::string::npos) { target = &f; break; }
        if (!target && fn.find("gearinput.ini") != std::string::npos) target = &f;
      }
      for (ue3::ConfigFile& f : bundle.files) {   // GitHub #237: side-by-side split
        const std::string fn = Lower(f.name.Utf8());
        if (fn.find("xe-gearengine.ini") != std::string::npos || fn.find("xbox360-gearengine.ini") != std::string::npos) {
          const int v = SetVerticalSplit(f);
          changed += v;
          if (v && (n == "Coalesced_int.bin" || written == 0))
            REXLOG_INFO("gow2_split_vertical: {}: {} key(s) set in {}", n, v, f.name.Utf8());
        }
      }
      if (target) {
        const int added = AddInputBindings(*target, "Bindings");
        changed += added;
        if (n == "Coalesced_int.bin" || written == 0)
          REXLOG_INFO("gow2 input bindings: {}: {} binding(s) added to {}", n, added, target->name.Utf8());
      } else if (n == "Coalesced_int.bin") {
        REXLOG_WARN("gow2 input bindings: {}: no GearInput.ini in the bundle", n);
      }
    }
    if (exit_menu) {
      const ue3::GameId game = DetermineGameId(runtime);
      const int exit_changed = ue3::ApplyMainMenuExitToBundle(game, n, bundle);
      if (exit_changed > 0) {
        changed += exit_changed;
        if (n == "Coalesced_int.bin" || written == 0) {
          REXLOG_INFO("main_menu_exit: {}: applied Exit Game to {} key(s)", n, exit_changed);
        }
      }
    }
    if (n == "Coalesced_int.bin" || written == 0)
      for (const std::string& l : log) REXLOG_INFO("ue3_ini_edits: {}: {}", n, l);
    const std::vector<uint8_t> edited = ue3::WriteConfigBundle(bundle);
    std::ofstream o(out_dir / n, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(edited.data()), std::streamsize(edited.size()));
    if (!o) {
      REXLOG_WARN("ue3_ini_edits: could not write {}", (out_dir / n).string());
      continue;
    }
    if (changed == 0 && !spec.empty()) REXLOG_WARN("ue3_ini_edits: {}: no edit matched anything", n);
    const std::vector<uint8_t>& before = original.empty() ? bytes : original;
    const int table = UpdateHashTable(runtime, n, ue3::Sha1(before.data(), before.size()),
                                      ue3::Sha1(edited.data(), edited.size()));
    if (table > 0) ++hashed;
    else if (table == 0 && (n == "Coalesced_int.bin" || written == 0))
      REXLOG_WARN("ue3_ini_edits: {}: not in the game's start-up hash table - it may refuse the edited copy", n);
    ++written;
  }
  if (!written) return;
  // #111: the disc's table of contents (<top folder>\*TOC*.txt) lists each bundle's size and MD5; with only the hash
  // table updated, the disc version reports a dirty disc a few seconds in. When there is one, the edited bundles go
  // through the mods' folder view, whose table-of-contents writer gives their lines the edited size and MD5.
  bool with_toc = false;
  {
    std::string top = rel;
    for (char& c : top) if (c == '/') c = '\\';
    while (!top.empty() && top.front() == '\\') top.erase(0, 1);
    top = top.substr(0, top.find('\\'));
    const fs::path topdir = runtime->game_data_root() / rex::to_path(top);
    if (!top.empty() && fs::is_directory(topdir, ec))
      for (const auto& e : fs::directory_iterator(topdir, ec)) {
        const std::string tl = ModLower(rex::path_to_utf8(e.path().filename()));
        if (e.is_regular_file() && tl.find("toc") != std::string::npos && tl.ends_with(".txt")) with_toc = true;
      }
  }
  if (with_toc && mods.empty()) {
    for (const std::string& n : names)
      if (fs::exists(out_dir / n, ec)) mods[RelKey(rel + "\\" + n)] = out_dir / n;
    REXLOG_INFO("ue3_ini_edits: {} edit(s) applied to {} config bundle(s); {} hash table entries updated; table of "
                "contents found - shown through the mods' folder view, which rewrites their table-of-contents lines",
                edits.size(), written, hashed);
    return;
  }
  if (!mods.empty()) {   // 0.2.0: into the mods' overlay (the mods' own copies of these bundles were the ones edited)
    for (const std::string& n : names)
      if (fs::exists(out_dir / n, ec)) mods[RelKey(rel + "\\" + n)] = out_dir / n;
    REXLOG_INFO("ue3_ini_edits: {} edit(s) applied to {} config bundle(s); shown through the mods' folder view", edits.size(), written);
    return;
  }
  std::set<std::string> replaced;
  for (const std::string& n : names)
    if (fs::exists(out_dir / n, ec)) replaced.insert(Lower(n));
  auto overlay = std::make_unique<OverlayDevice>(kOverlayDevice, src_dir, out_dir, replaced);
  OverlayDevice* view = overlay.get();
  if (!overlay->Initialize() || !runtime->file_system()->RegisterDevice(std::move(overlay))) {
    REXLOG_WARN("ue3_ini_edits: could not set up the folder view; the game reads its own config");
    return;
  }
  std::string guest_dir = rel;
  for (char& c : guest_dir) if (c == '/') c = '\\';
  while (!guest_dir.empty() && guest_dir.back() == '\\') guest_dir.pop_back();
  const std::string game_folder = std::string(kGameDevice) + "\\" + guest_dir;
  runtime->file_system()->RegisterSymbolicLink(game_folder, kOverlayDevice);
  // Check it the way the game's open will go: resolve the folder, look the name up there.
  int verified = 0;
  for (const std::string& n : names) {
    rex::filesystem::Entry* folder = runtime->file_system()->ResolvePath(game_folder);
    auto* e = folder ? dynamic_cast<rex::filesystem::HostPathEntry*>(folder->GetChild(n)) : nullptr;
    if (e && e->host_path() == out_dir / n) ++verified;
    else if (replaced.count(Lower(n))) REXLOG_WARN("ue3_ini_edits: {} still resolves to the game's own copy", n);
  }
  REXLOG_INFO("ue3_ini_edits: {} edit(s) applied to {} config bundle(s) in {}; the game's {} now shows them ({} entries "
              "replaced, {} of {} checked through the file system; {} hash table entries updated)", edits.size(), written,
              out_dir.string(), game_folder, view->replaced_count(), verified, names.size(), hashed);
}
