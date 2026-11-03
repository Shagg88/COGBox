#include <catch2/catch_test_macros.hpp>
#include <xenos/renderer/preparation_restart.h>

#include <chrono>
#include <fstream>

namespace {
namespace fs = std::filesystem;
struct CacheFixture {
  fs::path base, root, cache;
  CacheFixture() {
    base = fs::weakly_canonical(fs::current_path() / "work" / "cache-restart-tests");
    fs::create_directories(base);
    root = base / ("test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(root)) throw std::runtime_error("Test folder already exists");
    cache = root / "shadercache";
    fs::create_directory(cache);
  }
  ~CacheFixture() {
    std::error_code ec;
    // Cleanup is restricted to this newly-created fixture beneath the test workspace.
    const auto checked = fs::weakly_canonical(root, ec);
    if (!ec && checked.parent_path() == base && checked.filename().string().rfind("test-", 0) == 0)
      fs::remove_all(checked, ec);
  }
  void Write(const fs::path& relative, const char* value) {
    const auto path = cache / relative;
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f << value;
    if (!f) throw std::runtime_error("Could not write test fixture");
  }
  static std::string Read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  }
};
}

TEST_CASE("Fresh preparation backs up the complete cache and preserves its active control files", "[preparation][restart]") {
  CacheFixture f;
  f.Write("pipelines-d3d12.bin", "driver-pipelines");
  f.Write("ucode/recorded.pack", "recorded-shaders");
  f.Write("shaders.pack", "compiled-shaders");
  f.Write("prepare-progress.old-session.txt", "old-progress");
  f.Write("prepare.lock", "lock");
  f.Write("prepare-cancel.current.flag", "early-stop");
  f.Write("prepare-progress.current.txt", "current-progress");
  const auto backup = xenos::preparation::RestartCache(f.cache, "current");
  REQUIRE(backup == f.root / "shadercache-backups/restart-current");
  CHECK(CacheFixture::Read(backup / "pipelines-d3d12.bin") == "driver-pipelines");
  CHECK(CacheFixture::Read(backup / "ucode/recorded.pack") == "recorded-shaders");
  CHECK(CacheFixture::Read(backup / "shaders.pack") == "compiled-shaders");
  CHECK(CacheFixture::Read(backup / "prepare-progress.old-session.txt") == "old-progress");
  CHECK_FALSE(fs::exists(f.cache / "ucode"));
  CHECK_FALSE(fs::exists(f.cache / "pipelines-d3d12.bin"));
  CHECK(CacheFixture::Read(f.cache / "prepare.lock") == "lock");
  CHECK(CacheFixture::Read(f.cache / "prepare-cancel.current.flag") == "early-stop");
  CHECK(CacheFixture::Read(f.cache / "prepare-progress.current.txt") == "current-progress");
}

TEST_CASE("Cache restart refuses broad folders, unexpected contents and unsafe session paths", "[preparation][restart]") {
  CacheFixture f;
  f.Write("pipelines.list", "recipes");
  f.Write("save.sav", "unrelated-data");
  CHECK_THROWS(xenos::preparation::RestartCache(f.cache, "one"));
  CHECK(CacheFixture::Read(f.cache / "pipelines.list") == "recipes");
  CHECK(CacheFixture::Read(f.cache / "save.sav") == "unrelated-data");
  CHECK_FALSE(fs::exists(f.root / "shadercache-backups"));
  CHECK_THROWS(xenos::preparation::RestartCache(f.root, "one"));
  CHECK_THROWS(xenos::preparation::RestartCache(f.cache, "../escape"));
}

TEST_CASE("Restarting an empty cache does not create a redundant backup", "[preparation][restart]") {
  CacheFixture f;
  f.Write("prepare.lock", "locked");
  CHECK(xenos::preparation::RestartCache(f.cache, "empty").empty());
  CHECK_FALSE(fs::exists(f.root / "shadercache-backups"));
  CHECK(CacheFixture::Read(f.cache / "prepare.lock") == "locked");
}

#ifdef _WIN32
TEST_CASE("A locked cache artifact rolls the restart back without losing earlier moves", "[preparation][restart]") {
  CacheFixture f;
  f.Write("a.dxil", "first");
  f.Write("z.spv", "locked-last");
  const auto path = f.cache / "z.spv";
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(file != INVALID_HANDLE_VALUE);
  CHECK_THROWS(xenos::preparation::RestartCache(f.cache, "rollback"));
  CloseHandle(file);
  CHECK(CacheFixture::Read(f.cache / "a.dxil") == "first");
  CHECK(CacheFixture::Read(f.cache / "z.spv") == "locked-last");
  CHECK(fs::is_empty(f.root / "shadercache-backups/restart-rollback"));
}
#endif
