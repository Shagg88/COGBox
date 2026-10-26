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
// --ue3_package_patches (notes/25): patched script packages go through the same overlay (ue3/package_patch.h).
// Our code (MIT).
#include <ue3/config_bundle.h>
#include <ue3/package_patch.h>

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
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(ue3_ini_edits, "", "Gears3",
                      "Engine config edits, 'file|section|key=value' or 'file|section|key*=factor', ';'-separated, e.g. "
                      "'Xe-GearEngine.ini|SystemSettings|MotionBlur=False' (ue3/config_bundle.h)");
REXCVAR_DEFINE_STRING(ue3_ini_bundle_dir, "GearGame\\CookedXbox360", "Gears3",
                      "Folder of the Coalesced_*.bin config bundles, relative to the game data root");
REXCVAR_DEFINE_STRING(ue3_package_patches, "", "Gears3",
                      "Script package byte patches, '<file>@<offset>:<old hex>><new hex>', ';'-separated (set by the "
                      "game's own switches; ue3/package_patch.h)");

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
                std::set<std::string> replaced)
      : HostPathDevice(mount, game_dir, true),
        game_dir_(std::move(game_dir)),
        edit_dir_(std::move(edit_dir)),
        replaced_(std::move(replaced)) {}
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
  std::unique_ptr<OverlayFolder> root_;
  uint32_t replaced_hits_ = 0;
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
// Reads `name` from `dir` (exact name first, then a case-insensitive match among the folder's files).
bool ReadFileCi(const std::filesystem::path& dir, const std::string& name, std::string* found, std::vector<uint8_t>* out) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::string real;
  if (fs::is_regular_file(dir / name, ec)) real = name;
  else
    for (const auto& e : fs::directory_iterator(dir, ec))
      if (e.is_regular_file() && AsciiLower(e.path().filename().string()) == AsciiLower(name)) real = e.path().filename().string();
  if (real.empty()) return false;
  std::ifstream in(dir / real, std::ios::binary);
  out->assign(std::istreambuf_iterator<char>(in), {});
  *found = real;
  return bool(in) || in.eof();
}

struct TocChange {
  std::string name;
  uint64_t size;
  std::string md5;
  int64_t uncompressed_size;  // -1: keep the line's own
  ue3::Sha1Digest old_sha1{}, new_sha1{};  // for the start-up hash table (patched packages)
};
std::string DigestHex(const ue3::Sha1Digest& d) {
  std::string s;
  for (uint8_t b : d) s += std::format("{:02x}", b);
  return s;
}
bool ParseDigest(const std::string& hex, ue3::Sha1Digest* d) {
  if (hex.size() != 40) return false;
  for (size_t i = 0; i < 20; ++i) (*d)[i] = uint8_t(std::strtoul(hex.substr(2 * i, 2).c_str(), nullptr, 16));
  return true;
}

// One patched package (ue3/package_patch.h) into `out_dir`. A cache note beside it (<name>.patch.txt: source size and
// time, the patch list, the result's size and MD5) lets later starts skip the decompress + MD5 of a large package.
bool MakePatchedPackage(const std::filesystem::path& src_dir, const std::filesystem::path& out_dir, const std::string& file,
                        const std::vector<ue3::PackagePatch>& patches, const std::string& spec, std::string* real_name,
                        TocChange* toc) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<uint8_t> bytes;
  if (!ReadFileCi(src_dir, file, real_name, &bytes)) {
    REXLOG_WARN("ue3_package_patches: {} not found in {}", file, src_dir.string());
    return false;
  }
  const fs::path src = src_dir / *real_name, out = out_dir / *real_name, note = out_dir / (*real_name + ".patch.txt");
  const auto src_time = fs::last_write_time(src, ec).time_since_epoch().count();
  const std::string key = std::format("{} {} {}", bytes.size(), src_time, spec);
  {  // cache
    std::ifstream in(note);
    std::string k, md5, sha_old, sha_new;
    uint64_t size = 0;
    int64_t usize = -1;
    if (std::getline(in, k) && k == key && in >> size >> usize >> md5 >> sha_old >> sha_new &&
        fs::file_size(out, ec) == size && !ec) {
      *toc = {*real_name, size, md5, usize};
      ParseDigest(sha_old, &toc->old_sha1);
      ParseDigest(sha_new, &toc->new_sha1);
      REXLOG_INFO("ue3_package_patches: {}: patched copy from an earlier start ({} bytes)", *real_name, size);
      return true;
    }
  }
  const ue3::Sha1Digest old_sha1 = ue3::Sha1(bytes.data(), bytes.size());
  const bool compressed = ue3::IsFullyCompressed(bytes);
  if (compressed) {
    std::vector<uint8_t> plain;
    std::string err;
    if (!ue3::DecompressFully(bytes, plain, &err)) {
      REXLOG_WARN("ue3_package_patches: {}: can't decompress ({}) - left as it is", *real_name, err);
      return false;
    }
    bytes = std::move(plain);
  }
  std::vector<std::string> log;
  const int applied = ue3::ApplyPackagePatches(bytes, *real_name, patches, &log);
  for (const std::string& l : log) REXLOG_INFO("ue3_package_patches: {}", l);
  if (applied <= 0) {
    if (applied < 0) REXLOG_WARN("ue3_package_patches: {} left as it is", *real_name);
    return false;
  }
  fs::create_directories(out_dir, ec);
  std::ofstream o(out, std::ios::binary | std::ios::trunc);
  o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  o.close();
  if (!o) {
    REXLOG_WARN("ue3_package_patches: could not write {}", out.string());
    return false;
  }
  *toc = {*real_name, bytes.size(), ue3::Md5Hex(bytes.data(), bytes.size()), compressed ? 0 : -1};
  toc->old_sha1 = old_sha1;
  toc->new_sha1 = ue3::Sha1(bytes.data(), bytes.size());
  std::ofstream n(note, std::ios::trunc);
  n << key << "\n" << toc->size << " " << toc->uncompressed_size << " " << toc->md5 << " " << DigestHex(toc->old_sha1)
    << " " << DigestHex(toc->new_sha1) << "\n";
  REXLOG_INFO("ue3_package_patches: {}: {} patch(es){}, {} bytes", *real_name, applied,
              compressed ? " (decompressed, stored plain)" : "", bytes.size());
  return true;
}
}  // namespace

void Ue3InstallConfigEditsIfRequested(rex::Runtime* runtime) {
  const std::string spec = REXCVAR_GET(ue3_ini_edits), pspec = REXCVAR_GET(ue3_package_patches);
  if ((spec.empty() && pspec.empty()) || !runtime || !runtime->file_system()) return;
  std::string error;
  const std::vector<ue3::ConfigEdit> edits = ue3::ParseConfigEdits(spec, &error);
  if (!error.empty()) REXLOG_WARN("ue3_ini_edits: {}", error);
  error.clear();
  const std::vector<ue3::PackagePatch> patches = ue3::ParsePackagePatches(pspec, &error);
  if (!error.empty()) REXLOG_WARN("ue3_package_patches: {}", error);
  if (edits.empty() && patches.empty()) return;
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
  std::vector<std::string> names;  // files we replace, in the bundle folder
  if (!edits.empty())
    for (const auto& e : fs::directory_iterator(src_dir, ec)) {
      const std::string n = e.path().filename().string();
      if (e.is_regular_file() && n.size() > 14 && n.rfind("Coalesced_", 0) == 0 && n.ends_with(".bin")) names.push_back(n);
    }
  if (!edits.empty() && names.empty()) REXLOG_WARN("ue3_ini_edits: no Coalesced_*.bin in {}", src_dir.string());
  int written = 0, hashed = 0;
  std::vector<TocChange> new_entries;
  for (const std::string& n : names) {
    std::ifstream in(src_dir / n, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
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
    const int changed = ue3::ApplyConfigEdits(bundle, edits, &log);
    // Log the English bundle's changes (Coalesced_int.bin on Gears 2, Coalesced_INT.bin on Gears 3), or the first one.
    const bool log_this = AsciiLower(n) == "coalesced_int.bin" || written == 0;
    if (log_this)
      for (const std::string& l : log) REXLOG_INFO("ue3_ini_edits: {}: {}", n, l);
    const std::vector<uint8_t> edited = ue3::WriteConfigBundle(bundle);
    std::ofstream o(out_dir / n, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(edited.data()), std::streamsize(edited.size()));
    if (!o) {
      REXLOG_WARN("ue3_ini_edits: could not write {}", (out_dir / n).string());
      continue;
    }
    if (changed == 0) REXLOG_WARN("ue3_ini_edits: {}: no edit matched anything", n);
    new_entries.push_back({n, edited.size(), ue3::Md5Hex(edited.data(), edited.size()), -1});
    const int table = UpdateHashTable(runtime, n, ue3::Sha1(bytes.data(), bytes.size()),
                                      ue3::Sha1(edited.data(), edited.size()));
    if (table > 0) ++hashed;
    else if (table == 0 && !with_toc && log_this)
      REXLOG_WARN("ue3_ini_edits: {}: not in the game's start-up hash table - it may refuse the edited copy", n);
    ++written;
  }
  // Script packages (ue3/package_patch.h): one patched copy per named file.
  std::set<std::string> package_files;
  for (const ue3::PackagePatch& p : patches) package_files.insert(AsciiLower(p.file));
  for (const std::string& f : package_files) {
    std::string real;
    TocChange toc;
    if (!MakePatchedPackage(src_dir, out_dir, f, patches, pspec, &real, &toc)) continue;
    // If the start-up hash table lists this package too (job 124b: TU6 showed the dirty-disc error with it patched),
    // give it the patched copy's hash, as for the config bundles.
    const int table = UpdateHashTable(runtime, real, toc.old_sha1, toc.new_sha1);
    REXLOG_INFO("ue3_package_patches: {}: {} start-up hash table entr{} updated", real, table, table == 1 ? "y" : "ies");
    if (table > 0) ++hashed;
    names.push_back(real);
    new_entries.push_back(toc);
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
    for (const TocChange& e : new_entries) changed += ue3::UpdateTocEntry(text, e.name, e.size, e.md5, e.uncompressed_size);
    if (!changed) continue;
    std::ofstream o(out_root / t, std::ios::binary | std::ios::trunc);
    o.write(text.data(), std::streamsize(text.size()));
    if (!o) {
      REXLOG_WARN("ue3_ini_edits: could not write {}", (out_root / t).string());
      continue;
    }
    replaced.insert(AsciiLower(t));
    toc_lines += changed;
    REXLOG_INFO("ue3_ini_edits: {}: {} line(s) given the edited files' size and MD5", t, changed);
  }
  if (with_toc && !toc_lines) REXLOG_WARN("ue3_ini_edits: no table-of-contents line names an edited file");
  auto overlay = std::make_unique<OverlayDevice>(kOverlayDevice, overlay_dir, out_root, replaced);
  OverlayDevice* view = overlay.get();
  if (!overlay->Initialize() || !runtime->file_system()->RegisterDevice(std::move(overlay))) {
    REXLOG_WARN("ue3_ini_edits: could not set up the folder view; the game reads its own files");
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
  REXLOG_INFO("ue3_ini_edits: {} edit(s), {} package patch(es); {} file(s) in {}; the game's {} now shows them ({} "
              "entries replaced, {} checked through the file system; {} hash table entries updated, {} "
              "table-of-contents lines)",
              edits.size(), patches.size(), written, out_dir.string(), game_folder, view->replaced_count(), verified,
              hashed, toc_lines);
}
