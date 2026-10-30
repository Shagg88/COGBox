#include <catch2/catch_test_macros.hpp>
#include <xenos/renderer/mapped_file.h>
#include <xenos/renderer/async_file_appender.h>
#include <xenos/renderer/warm_source_cache.h>
#include <xenos/renderer/warm_stage_store.h>
#include <ue3/shader_package_inventory.h>
#include <winioctl.h>
#include <atomic>
#include <set>
namespace {
struct TestFile {
  std::filesystem::path path;
  TestFile() {
    static std::atomic<uint32_t> serial{0};
    path = std::filesystem::temp_directory_path() /
           ("cogbox-shader-io-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++serial) + ".bin");
  }
  ~TestFile() { std::error_code ec; std::filesystem::remove(path, ec); }
};
}
TEST_CASE("Pipeline library mapping retains exact bytes and releases its view", "[shader-io]") {
  TestFile file;
  { std::ofstream out(file.path, std::ios::binary); out << "compiled pipeline bytes"; }
  cogbox::io::MappedFile map;
  REQUIRE(map.Open(file.path));
  REQUIRE(map.Size() == 23);
  REQUIRE(std::string(static_cast<const char*>(map.Data()), map.Size()) == "compiled pipeline bytes");
  map.Reset();
  REQUIRE(map.Data() == nullptr);
  REQUIRE(map.Size() == 0);
}
TEST_CASE("Pipeline mapping rejects missing and empty cache files", "[shader-io]") {
  TestFile file;
  cogbox::io::MappedFile map;
  REQUIRE_FALSE(map.Open(file.path));
  { std::ofstream out(file.path); }
  REQUIRE_FALSE(map.Open(file.path));
  REQUIRE(map.Size() == 0);
}
TEST_CASE("Shader library mappings handle offsets beyond four GiB without copying the file", "[shader-io]") {
  TestFile file;
  HANDLE f = CreateFileW(file.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(f != INVALID_HANDLE_VALUE);
  DWORD actual = 0;
  if (!DeviceIoControl(f, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &actual, nullptr)) {
    CloseHandle(f); SKIP("Filesystem does not support sparse files");
  }
  LARGE_INTEGER position{}; position.QuadPart = (5ll << 30) + 47;
  const bool seek = SetFilePointerEx(f, position, nullptr, FILE_BEGIN);
  const char tail = 'X';
  const bool wrote = seek && WriteFile(f, &tail, 1, &actual, nullptr);
  CloseHandle(f);
  REQUIRE(wrote);
  cogbox::io::MappedFile map;
  REQUIRE(map.Open(file.path));
  REQUIRE(map.Size() == size_t(position.QuadPart) + 1);
  REQUIRE(static_cast<const char*>(map.Data())[size_t(position.QuadPart)] == 'X');
}
TEST_CASE("Asynchronous metadata appends preserve every concurrent record", "[shader-io]") {
  TestFile file;
  {
    cogbox::io::AsyncFileAppender writer(file.path);
    REQUIRE(writer.Good());
    auto append = [&](uint32_t base) { for (uint32_t n = base; n < base + 1000; ++n) writer.Append(&n, sizeof(n)); };
    std::thread a(append, 0), b(append, 1000); a.join(); b.join();
    writer.WaitIdle();
    REQUIRE(std::filesystem::file_size(file.path) == 2000 * sizeof(uint32_t));
  }
  std::set<uint32_t> records;
  std::ifstream in(file.path, std::ios::binary);
  uint32_t value;
  while (in.read(reinterpret_cast<char*>(&value), sizeof(value))) records.insert(value);
  REQUIRE(records.size() == 2000);
  REQUIRE(*records.begin() == 0);
  REQUIRE(*records.rbegin() == 1999);
}
TEST_CASE("Closing an append writer drains queued pipeline metadata", "[shader-io]") {
  TestFile file;
  { cogbox::io::AsyncFileAppender writer(file.path); writer.Append("first", 5); }
  { cogbox::io::AsyncFileAppender writer(file.path); writer.Append("second", 6); }
  std::ifstream in(file.path, std::ios::binary);
  REQUIRE(std::string(std::istreambuf_iterator<char>(in), {}) == "firstsecond");
}
TEST_CASE("A compiled source inventory preserves stage and source identity", "[shader-io]") {
  TestFile file;
  cogbox::shader_cache::WarmSourceCache cache;
  const std::wstring ps = L"gp00000000000000000000000000000001", vs = L"gp00000000000000000000000000000002";
  cache.Remember({17, false}, ps);
  REQUIRE_FALSE(cache.Contains({17, true}));
  cache.Remember({17, true}, vs);
  const cogbox::shader_cache::LibraryStamp stamp{123, 4096, 567};
  REQUIRE(cache.Save(file.path, stamp));
  cogbox::shader_cache::WarmSourceCache restored;
  REQUIRE(restored.Load(file.path, stamp));
  REQUIRE(restored.Size() == 2);
  REQUIRE(restored.Contains({17, false}));
  REQUIRE(restored.Contains({17, true}));
  REQUIRE(restored.HasPipeline(ps));
  REQUIRE(restored.HasPipeline(vs));
}
TEST_CASE("An inventory cannot bypass compilation after options or native cache changes", "[shader-io]") {
  TestFile file;
  cogbox::shader_cache::WarmSourceCache cache;
  cache.Remember({1, false}, L"gp00000000000000000000000000000001");
  const cogbox::shader_cache::LibraryStamp stamp{123, 4096, 567};
  REQUIRE(cache.Save(file.path, stamp));
  for (auto changed : {cogbox::shader_cache::LibraryStamp{124, 4096, 567},
                       cogbox::shader_cache::LibraryStamp{123, 4097, 567},
                       cogbox::shader_cache::LibraryStamp{123, 4096, 568}}) {
    REQUIRE_FALSE(cache.Load(file.path, changed));
    REQUIRE(cache.Size() == 0);
  }
}
TEST_CASE("Truncated or corrupt inventories fall back to normal shader preparation", "[shader-io]") {
  TestFile file;
  cogbox::shader_cache::WarmSourceCache cache;
  cache.Remember({1, false}, L"gp00000000000000000000000000000001");
  const cogbox::shader_cache::LibraryStamp stamp{123, 4096, 567};
  REQUIRE(cache.Save(file.path, stamp));
  {
    std::fstream corrupt(file.path, std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekp(-1, std::ios::end); corrupt.put(char(0x5A));
  }
  REQUIRE_FALSE(cache.Load(file.path, stamp));
  REQUIRE(cache.Size() == 0);
  std::filesystem::resize_file(file.path, 4);
  REQUIRE_FALSE(cache.Load(file.path, stamp));
}
TEST_CASE("An updated inventory atomically replaces its previous snapshot", "[shader-io]") {
  TestFile file;
  cogbox::shader_cache::WarmSourceCache cache;
  const cogbox::shader_cache::LibraryStamp stamp{123, 4096, 567};
  cache.Remember({1, false}, L"gp00000000000000000000000000000001");
  REQUIRE(cache.Save(file.path, stamp));
  cache.Remember({2, false}, L"gp00000000000000000000000000000001");
  REQUIRE(cache.Save(file.path, stamp));
  REQUIRE(cache.Load(file.path, stamp));
  REQUIRE(cache.Size() == 2);
}
TEST_CASE("Different shader configurations retain independent source inventories", "[shader-io]") {
  TestFile a, b;
  cogbox::shader_cache::WarmSourceCache cache;
  const cogbox::shader_cache::LibraryStamp sa{123,896,17}, sb{124,896,17};
  cache.Remember({1,false}, L"gp00000000000000000000000000000001");
  REQUIRE(cache.Save(a.path,sa));
  cache.Clear();
  cache.Remember({2,false}, L"gp00000000000000000000000000000002");
  REQUIRE(cache.Save(b.path,sb));
  REQUIRE(cache.Load(a.path,sa));
  REQUIRE(cache.Contains({1,false}));
  REQUIRE_FALSE(cache.Contains({2,false}));
  REQUIRE_FALSE(cache.Load(a.path,sb));
  REQUIRE(cache.Load(b.path,sb));
  REQUIRE(cache.Contains({2,false}));
}
TEST_CASE("Appending another configuration's stages preserves existing source readiness", "[shader-io]") {
  TestFile stages, inventory;
  cogbox::shader_cache::WarmStageStore store;
  const std::wstring a=L"gp00000000000000000000000000000001", b=L"gp00000000000000000000000000000002";
  REQUIRE(store.Open(stages.path,true));
  REQUIRE(store.Put(a,"first",5));
  cogbox::shader_cache::WarmSourceCache cache;
  const cogbox::shader_cache::LibraryStamp anchor{123,896,17};
  cache.Remember({1,false},a);
  REQUIRE(cache.Save(inventory.path,anchor));
  REQUIRE(store.Put(b,"second",6));
  REQUIRE(store.Open(stages.path,true));
  REQUIRE(cache.Load(inventory.path,anchor));
  REQUIRE(cache.RetainPipelines([&](const auto& name){return store.Contains(name);})==0);
  REQUIRE(cache.Contains({1,false}));
}
TEST_CASE("Truncating stage storage invalidates only sources whose records disappeared", "[shader-io]") {
  TestFile stages, inventory;
  cogbox::shader_cache::WarmStageStore store;
  const std::wstring a=L"gp00000000000000000000000000000001", b=L"gp00000000000000000000000000000002";
  REQUIRE(store.Open(stages.path,true));
  REQUIRE(store.Put(a,"first",5));
  const auto intact=std::filesystem::file_size(stages.path);
  REQUIRE(store.Put(b,"second",6));
  cogbox::shader_cache::WarmSourceCache cache;
  const cogbox::shader_cache::LibraryStamp anchor{123,896,17};
  cache.Remember({1,false},a); cache.Remember({2,false},b); cache.Remember({3,true},b);
  REQUIRE(cache.Save(inventory.path,anchor));
  std::filesystem::resize_file(stages.path,intact+4);  // incomplete second record
  REQUIRE(store.Open(stages.path,true));
  REQUIRE(cache.Load(inventory.path,anchor));
  REQUIRE(cache.RetainPipelines([&](const auto& name){return store.Contains(name);})==2);
  REQUIRE(cache.Size()==1);
  REQUIRE(cache.Contains({1,false}));
  REQUIRE_FALSE(cache.Contains({2,false}));
  REQUIRE_FALSE(cache.Contains({3,true}));
  REQUIRE_FALSE(cache.HasPipeline(b));
}
TEST_CASE("Compiled stages survive restart without loading their blobs into the index", "[shader-io]") {
  TestFile file;
  const std::wstring name = L"gp00000000000000000000000000000001";
  cogbox::shader_cache::WarmStageStore store;
  REQUIRE(store.Open(file.path, true));
  REQUIRE(store.Put(name, "compiled", 8));
  REQUIRE(store.Size() == 1);
  REQUIRE(store.Open(file.path, true));
  REQUIRE(store.Contains(name));
  std::vector<uint8_t> bytes;
  REQUIRE(store.Get(name, bytes));
  REQUIRE(std::string(bytes.begin(), bytes.end()) == "compiled");
  cogbox::shader_cache::LibraryStamp stamp{1, 2, 3}, changed{1, 2, 3};
  REQUIRE(store.Stamp(stamp));
  REQUIRE(store.Put(name, "new", 3));
  REQUIRE(store.Stamp(changed));
  REQUIRE_FALSE(stamp == changed);
}
TEST_CASE("An interrupted stage append preserves all complete earlier records", "[shader-io]") {
  TestFile file;
  const std::wstring name = L"gp00000000000000000000000000000001";
  cogbox::shader_cache::WarmStageStore store;
  REQUIRE(store.Open(file.path, true));
  REQUIRE(store.Put(name, "compiled", 8));
  const auto complete = std::filesystem::file_size(file.path);
  { std::ofstream append(file.path, std::ios::binary | std::ios::app); append << "partial"; }
  REQUIRE(store.Open(file.path, true));
  REQUIRE(std::filesystem::file_size(file.path) == complete);
  std::vector<uint8_t> bytes;
  REQUIRE(store.Get(name, bytes));
}
TEST_CASE("A corrupt driver blob is rejected before it reaches Direct3D", "[shader-io]") {
  TestFile file;
  const std::wstring name = L"gp00000000000000000000000000000001";
  cogbox::shader_cache::WarmStageStore store;
  REQUIRE(store.Open(file.path, true));
  REQUIRE(store.Put(name, "compiled", 8));
  { std::fstream f(file.path, std::ios::binary | std::ios::in | std::ios::out); f.seekp(-1, std::ios::end); f.put('X'); }
  std::vector<uint8_t> bytes;
  REQUIRE_FALSE(store.Get(name, bytes));
  REQUIRE(bytes.empty());
}
TEST_CASE("Driver changes start a new stage store and retain the incompatible cache", "[shader-io]") {
  TestFile file;
  const std::wstring name = L"gp00000000000000000000000000000001";
  cogbox::shader_cache::WarmStageStore store;
  REQUIRE(store.Open(file.path, true));
  REQUIRE(store.Put(name, "compiled", 8));
  REQUIRE(store.Open(file.path, false));
  REQUIRE_FALSE(store.Contains(name));
  REQUIRE(std::filesystem::file_size(file.path) == 0);
  size_t backups = 0;
  for (const auto& candidate : std::filesystem::directory_iterator(file.path.parent_path())) {
    if (!candidate.path().filename().wstring().starts_with(file.path.filename().wstring() + L".incompatible-")) continue;
    REQUIRE(candidate.file_size() > 8);
    ++backups;
    std::filesystem::remove(candidate.path());
  }
  REQUIRE(backups == 1);
}
TEST_CASE("Concurrent stage saves retain every distinct driver blob", "[shader-io]") {
  TestFile file;
  cogbox::shader_cache::WarmStageStore store;
  REQUIRE(store.Open(file.path, true));
  std::atomic<bool> saved{true};
  auto append = [&](int first) {
    for (int i = first; i < first + 50; ++i) {
      wchar_t name[40]; swprintf(name, 40, L"gp%032x", i);
      if (!store.Put(name, &i, sizeof(i))) saved = false;
    }
  };
  std::thread a(append, 0), b(append, 50); a.join(); b.join();
  REQUIRE(saved.load());
  REQUIRE(store.Open(file.path, true));
  REQUIRE(store.Size() == 100);
}
TEST_CASE("Package identities invalidate same-sized edits and replacement files", "[shader-io]") {
  TestFile package, inventory;
  { std::ofstream f(package.path); f << "original"; }
  ue3::shader_prebuild::PackageFileStamp stamp{};
  REQUIRE(ue3::shader_prebuild::PackageStamp(package.path, stamp));
  ue3::shader_prebuild::PackageInventory files;
  files.Remember(package.path, stamp, 123);
  REQUIRE(files.Save(inventory.path));
  REQUIRE(files.Load(inventory.path));
  uint64_t hash = 0;
  REQUIRE(files.Find(package.path, stamp, hash));
  REQUIRE(hash == 123);
  for (int field = 0; field < 6; ++field) {
    auto changed = stamp;
    switch (field) {
      case 0: ++changed.volume; break;
      case 1: ++changed.file_low; break;
      case 2: ++changed.file_high; break;
      case 3: ++changed.size; break;
      case 4: ++changed.written; break;
      case 5: ++changed.changed; break;
    }
    REQUIRE_FALSE(files.Find(package.path, changed, hash));
  }
  REQUIRE_FALSE(files.Find(inventory.path, stamp, hash));
  { std::fstream f(inventory.path, std::ios::binary | std::ios::in | std::ios::out); f.seekp(-1, std::ios::end); f.put('X'); }
  REQUIRE_FALSE(files.Load(inventory.path));
  REQUIRE_FALSE(files.Find(package.path, stamp, hash));
}
