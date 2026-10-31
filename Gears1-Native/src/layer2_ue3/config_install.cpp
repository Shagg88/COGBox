// Layer 2 (Unreal Engine 3): --ue3_ini_edits - engine settings changed through the game's own config bundle
// (ue3/config_bundle.h, notes/64). For each <game data>\<--ue3_ini_bundle_dir>\Coalesced_*.bin: read, edit, write to
// <user data>\ini-edits\, and show the game a view of the bundle folder in which those files are the edited copies.
//
// How the redirect works (job 256): the file system opens a file by resolving its *folder* through the symbolic links
// and then looking the name up among that folder's entries (VirtualFileSystem::OpenFile), so a link on a file's own
// path is never consulted - job 255's edits were written but never read. Instead the folder itself is linked to an
// overlay device: the same folder listing as the game's (every other file still the game's own, opened read-only from
// where it is), except that the edited bundles' entries point at our copies. The game's files are never written.
// Gears 3 (job 050, from Gears 2's 3af5654): the disc's table of contents (<bundle dir>\..\*TOC*.txt) lists each file's
// size and MD5, so the edited bundles' lines there are rewritten too and the overlay covers the parent folder (the TOC
// files and the bundle folder below it). Gears 3 also has Gears 2's start-up SHA-1 table (job 050: 9 entries updated;
// an earlier search of an incomplete image copy missed it), so both are updated.
// Our code (MIT).
#include <ue3/config_bundle.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/string/utf8.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <sstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(ue3_ini_edits, "", "Gears3",
                      "Engine config edits, 'file|section|key=value' or 'file|section|key*=factor', ';'-separated, e.g. "
                      "'Xe-GearEngine.ini|SystemSettings|MotionBlur=False' (ue3/config_bundle.h)");
REXCVAR_DEFINE_STRING(ue3_mods, "", "Gears3",
                      "Mods (0.2.0): host folders, ';'-separated in load order (a later one wins), each laid out like the game "
                      "data folder; their files replace or add to the game's, read-only, through a folder overlay");
REXCVAR_DEFINE_STRING(ue3_ini_bundle_dir, "GearGame\\CookedXbox360", "Gears3",
                      "Folder of the Coalesced_*.bin config bundles, relative to the game data root");

namespace {
constexpr const char* kGameDevice = "\\Device\\Harddisk0\\Partition1";
constexpr const char* kOverlayDevice = "\\Device\\Ue3IniOverlay";

using ue3::AsciiLower;

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

// Read-only view of `game_dir` in which the files named in `replaced` (lower-case paths relative to `game_dir`, '/'
// separated) come from the same relative path in `edit_dir`. A
// HostPathDevice so that its file entries open the way the game's own do (HostPathEntry::Open expects one).
class OverlayDevice : public rex::filesystem::HostPathDevice {
 public:
  OverlayDevice(std::string_view mount, std::filesystem::path game_dir, std::filesystem::path edit_dir,
                std::set<std::string> replaced, std::set<std::string> hidden = {})
      : HostPathDevice(mount, game_dir, true),
        game_dir_(std::move(game_dir)),
        edit_dir_(std::move(edit_dir)),
        replaced_(std::move(replaced)),
        hidden_(std::move(hidden)) {}
  uint32_t hidden_count() const { return hidden_hits_; }
  bool Initialize() override {
    std::error_code ec;
    if (!std::filesystem::is_directory(game_dir_, ec)) return false;
    root_ = std::make_unique<OverlayFolder>(this, nullptr, "", game_dir_);
    Fill(root_.get(), game_dir_, std::string());
    return true;
  }
  rex::filesystem::Entry* ResolvePath(const std::string_view path) override { return root_->ResolvePath(path); }
  void Dump(rex::string::StringBuffer* out) override { root_->Dump(out, 0); }
  uint32_t replaced_count() const { return replaced_hits_; }

 private:
  void Fill(OverlayFolder* folder, const std::filesystem::path& dir, const std::string& rel) {
    for (const rex::filesystem::FileInfo& info : rex::filesystem::ListFiles(dir)) {
      const std::filesystem::path game_path = dir / info.name;
      if (info.type == rex::filesystem::FileInfo::Type::kDirectory) {
        auto* sub = new OverlayFolder(this, folder,
                                      rex::string::utf8_join_guest_paths(folder->path(), rex::path_to_utf8(info.name)),
                                      game_path);
        sub->SetTimes(info);
        folder->Add(sub);
        Fill(sub, game_path, rel + rex::path_to_utf8(info.name) + "/");
        continue;
      }
      rex::filesystem::FileInfo use = info;
      std::filesystem::path host = game_path;
      const std::string rel_name = rel + rex::path_to_utf8(info.name);
      if (hidden_.count(AsciiLower(rel_name))) { ++hidden_hits_; continue; }   // not in this view at all
      if (replaced_.count(AsciiLower(rel_name))) {
        rex::filesystem::FileInfo edited;
        const std::filesystem::path edited_path = edit_dir_ / std::filesystem::path(rel_name);
        if (rex::filesystem::GetInfo(edited_path, &edited)) {
          use.total_size = edited.total_size;
          use.write_timestamp = edited.write_timestamp;
          host = edited_path;
          ++replaced_hits_;
        }
      }
      folder->Add(rex::filesystem::HostPathEntry::Create(this, folder, host, use));
    }
  }
  std::filesystem::path game_dir_, edit_dir_;
  std::set<std::string> replaced_;
  std::set<std::string> hidden_;
  std::unique_ptr<OverlayFolder> root_;
  uint32_t replaced_hits_ = 0;
  uint32_t hidden_hits_ = 0;
};
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
  for (size_t off : ue3::FindFileHash(image, size, AsciiLower(name), old_hash)) {
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

// rel (lower case, '\\', from the game data root) -> host file; later mods win. Programs are refused.
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
    const std::string name = ModLower(rex::path_to_utf8(host.filename()));
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
      REXLOG_INFO("ue3_mods: {}: {} line(s) changed or added", tname, changed);
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
}  // namespace

void Ue3InstallConfigEditsIfRequested(rex::Runtime* runtime) {
  if (!runtime || !runtime->file_system()) return;
  // 0.2.0: mods. With mods on, the config edits' copies join the mods' overlay instead of their own folder view.
  std::map<std::string, std::filesystem::path> modDirs;
  std::map<std::string, std::filesystem::path> mods = CollectMods(&modDirs);
  struct ModsAtEnd {
    rex::Runtime* rt; std::map<std::string, std::filesystem::path>* files; std::map<std::string, std::filesystem::path>* dirs;
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
  if (spec.empty() || !runtime || !runtime->file_system()) return;
  std::string error;
  const std::vector<ue3::ConfigEdit> edits = ue3::ParseConfigEdits(spec, &error);
  if (!error.empty()) REXLOG_WARN("ue3_ini_edits: {}", error);
  if (edits.empty()) return;
  namespace fs = std::filesystem;
  std::string rel = REXCVAR_GET(ue3_ini_bundle_dir);
  const fs::path src_dir = runtime->game_data_root() / fs::path(rel);
  const fs::path out_root = runtime->user_data_root() / "ini-edits";
  std::error_code ec;
  if (!fs::is_directory(src_dir, ec)) {
    REXLOG_WARN("ue3_ini_edits: {} is not a folder; nothing edited", src_dir.string());
    return;
  }
  // Gears 3: table-of-contents files beside the bundle folder -> the overlay covers that parent folder.
  std::vector<std::string> tocs;
  const fs::path parent_dir = src_dir.parent_path();
  for (const auto& e : fs::directory_iterator(parent_dir, ec)) {
    const std::string n = AsciiLower(e.path().filename().string());
    if (e.is_regular_file() && n.find("toc") != std::string::npos && n.ends_with(".txt"))
      tocs.push_back(e.path().filename().string());
  }
  const bool with_toc = !tocs.empty();
  const fs::path overlay_dir = with_toc ? parent_dir : src_dir;
  const std::string bundle_rel = with_toc ? src_dir.filename().string() + "/" : std::string();  // inside the overlay
  const fs::path out_dir = out_root / fs::path(bundle_rel);
  fs::create_directories(out_dir, ec);
  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(src_dir, ec)) {
    const std::string n = e.path().filename().string();
    if (e.is_regular_file() && ((n.size() > 14 && n.rfind("Coalesced_", 0) == 0 && n.ends_with(".bin")) ||
                                AsciiLower(n) == "coalesced.ini"))   // Coalesced.ini: the 2006 layout (Gears 1)
      names.push_back(n);
  }
  if (names.empty()) {
    REXLOG_WARN("ue3_ini_edits: no Coalesced_*.bin in {}", src_dir.string());
    return;
  }
  int written = 0, hashed = 0;
  std::vector<std::pair<std::string, std::pair<uint64_t, std::string>>> new_entries;  // bundle -> size, MD5
  for (const std::string& n : names) {
    // 0.2.0: a mod's own copy of the bundle, if one has it, is the one edited; the start-up hash table still names the
    // game's original
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
    std::vector<ue3::ConfigString> texts;
    const bool flat = AsciiLower(n) == "coalesced.ini";
    if (flat ? !ue3::ParseFlatConfigBundle(bytes, bundle, texts, &perr) : !ue3::ParseConfigBundle(bytes, bundle, &perr)) {
      REXLOG_WARN("ue3_ini_edits: {}: {} - left as it is", n, perr);
      continue;
    }
    auto write = [&] { return flat ? ue3::WriteFlatConfigBundle(bundle, texts) : ue3::WriteConfigBundle(bundle); };
    if (write() != bytes) {  // our writer must reproduce the file exactly before we edit it
      REXLOG_WARN("ue3_ini_edits: {}: re-writing it does not reproduce the original - left as it is", n);
      continue;
    }
    std::vector<std::string> log;
    const int changed = ue3::ApplyConfigEdits(bundle, edits, &log);
    // Log the English bundle's changes (Coalesced_int.bin on Gears 2, Coalesced_INT.bin on Gears 3), or the first one.
    const bool log_this = AsciiLower(n) == "coalesced_int.bin" || flat || written == 0;
    if (log_this)
      for (const std::string& l : log) REXLOG_INFO("ue3_ini_edits: {}: {}", n, l);
    const std::vector<uint8_t> edited = write();
    std::ofstream o(out_dir / n, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(edited.data()), std::streamsize(edited.size()));
    if (!o) {
      REXLOG_WARN("ue3_ini_edits: could not write {}", (out_dir / n).string());
      continue;
    }
    if (changed == 0) REXLOG_WARN("ue3_ini_edits: {}: no edit matched anything", n);
    new_entries.push_back({n, {edited.size(), ue3::Md5Hex(edited.data(), edited.size())}});
    const std::vector<uint8_t>& before = original.empty() ? bytes : original;
    const int table = UpdateHashTable(runtime, n, ue3::Sha1(before.data(), before.size()),
                                      ue3::Sha1(edited.data(), edited.size()));
    if (table > 0) ++hashed;
    else if (table == 0 && !with_toc && log_this)
      REXLOG_WARN("ue3_ini_edits: {}: not in the game's start-up hash table - it may refuse the edited copy", n);
    ++written;
  }
  if (!written) return;
  std::set<std::string> replaced;
  for (const std::string& n : names)
    if (fs::exists(out_dir / n, ec)) replaced.insert(AsciiLower(bundle_rel + n));
  int toc_lines = 0;
  for (const std::string& t : tocs) {
    std::ifstream in(parent_dir / t, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    int changed = 0;
    for (const auto& [n, entry] : new_entries) changed += ue3::UpdateTocEntry(text, n, entry.first, entry.second);
    if (!changed) continue;
    std::ofstream o(out_root / t, std::ios::binary | std::ios::trunc);
    o.write(text.data(), std::streamsize(text.size()));
    if (!o) {
      REXLOG_WARN("ue3_ini_edits: could not write {}", (out_root / t).string());
      continue;
    }
    replaced.insert(AsciiLower(t));
    toc_lines += changed;
    REXLOG_INFO("ue3_ini_edits: {}: {} line(s) given the edited bundle's size and MD5", t, changed);
  }
  if (with_toc && !toc_lines) REXLOG_WARN("ue3_ini_edits: no table-of-contents line names an edited bundle");
  if (!mods.empty()) {   // 0.2.0: the edited copies (and table of contents) go into the mods' overlay instead
    std::string orel = rel;
    for (char& c : orel) if (c == '/') c = '\\';
    while (!orel.empty() && orel.back() == '\\') orel.pop_back();
    if (with_toc) { const size_t cut = orel.find_last_of('\\'); orel = cut == std::string::npos ? std::string() : orel.substr(0, cut); }
    for (const std::string& r : replaced) mods[RelKey(orel.empty() ? r : orel + "\\" + r)] = out_root / std::filesystem::path(r);
    REXLOG_INFO("ue3_ini_edits: {} file(s) edited; shown through the mods' folder view", replaced.size());
    return;
  }
  auto overlay = std::make_unique<OverlayDevice>(kOverlayDevice, overlay_dir, out_root, replaced);
  OverlayDevice* view = overlay.get();
  if (!overlay->Initialize() || !runtime->file_system()->RegisterDevice(std::move(overlay))) {
    REXLOG_WARN("ue3_ini_edits: could not set up the folder view; the game reads its own config");
    return;
  }
  std::string guest_dir = rel;
  for (char& c : guest_dir) if (c == '/') c = '\\';
  while (!guest_dir.empty() && guest_dir.back() == '\\') guest_dir.pop_back();
  if (with_toc) {  // the overlay is the bundle folder's parent
    const size_t cut = guest_dir.find_last_of('\\');
    guest_dir = cut == std::string::npos ? std::string() : guest_dir.substr(0, cut);
  }
  const std::string game_folder = std::string(kGameDevice) + "\\" + guest_dir;
  runtime->file_system()->RegisterSymbolicLink(game_folder, kOverlayDevice);
  // Check it the way the game's open will go: resolve the folder, look the name up there.
  int verified = 0;
  const std::string bundle_folder = with_toc ? game_folder + "\\" + src_dir.filename().string() : game_folder;
  rex::filesystem::Entry* const folder = runtime->file_system()->ResolvePath(bundle_folder);
  rex::filesystem::Entry* const top = with_toc ? runtime->file_system()->ResolvePath(game_folder) : folder;
  for (const std::string& n : names) {
    auto* e = folder ? dynamic_cast<rex::filesystem::HostPathEntry*>(folder->GetChild(n)) : nullptr;
    if (e && e->host_path() == out_dir / n) ++verified;
    else if (replaced.count(AsciiLower(bundle_rel + n))) REXLOG_WARN("ue3_ini_edits: {} still resolves to the game's own copy", n);
  }
  for (const std::string& t : tocs) {
    if (!replaced.count(AsciiLower(t))) continue;
    auto* e = top ? dynamic_cast<rex::filesystem::HostPathEntry*>(top->GetChild(t)) : nullptr;
    if (e && e->host_path() == out_root / t) ++verified;
    else REXLOG_WARN("ue3_ini_edits: {} still resolves to the game's own copy", t);
  }
  REXLOG_INFO("ue3_ini_edits: {} edit(s) applied to {} config bundle(s) in {}; the game's {} now shows them ({} entries "
              "replaced, {} checked through the file system; {} hash table entries updated, {} table-of-contents lines)",
              edits.size(), written, out_dir.string(), game_folder, view->replaced_count(), verified, hashed, toc_lines);
}

// A view of <game data>\<folder> without the named files (lower-case names), e.g. the logo films (Gears 1's skip-logos
// option: a film that is not there is skipped by the engine). The game's files are never touched.
void Ue3HideGameFiles(rex::Runtime* runtime, const std::string& folder, const std::set<std::string>& names) {
  if (!runtime || !runtime->file_system() || names.empty()) return;
  const std::filesystem::path dir = runtime->game_data_root() / std::filesystem::path(folder);
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) { REXLOG_WARN("ue3_hide: {} is not a folder", dir.string()); return; }
  const std::string dev = std::string("\\Device\\Ue3Hide") + std::to_string(std::hash<std::string>{}(folder) & 0xFFFF);
  auto view = std::make_unique<OverlayDevice>(dev, dir, dir, std::set<std::string>{}, names);
  OverlayDevice* v = view.get();
  if (!view->Initialize() || !runtime->file_system()->RegisterDevice(std::move(view))) {
    REXLOG_WARN("ue3_hide: could not set up the view of {}", folder);
    return;
  }
  std::string guest = folder;
  for (char& c : guest) if (c == '/') c = '\\';
  runtime->file_system()->RegisterSymbolicLink(std::string(kGameDevice) + "\\" + guest, dev);
  REXLOG_INFO("ue3_hide: {}: {} file(s) left out", folder, v->hidden_count());
}
