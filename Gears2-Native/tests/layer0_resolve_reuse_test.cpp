#include <catch2/catch_test_macros.hpp>
#include <xenos/renderer/resolve_reuse.h>
#include <unordered_map>
#include <utility>
namespace {
struct Resolve { uint32_t texture; uint64_t last_used; uint32_t width, height; };
auto Shape(const Resolve& value) { return std::pair{value.width, value.height}; }
}
TEST_CASE("Resolve recycling preserves recent display and readback sources", "[resolve-reuse]") {
  std::unordered_map<int, Resolve> cache{{1,{1,96,1280,720}},{2,{2,99,1280,720}},{3,{3,100,1280,720}}};
  REQUIRE(cogbox::renderer::ReusableResolve(cache,100,1280,720,Shape) == cache.end());
  REQUIRE(cogbox::renderer::ReusableResolve(cache,4,1280,720,Shape) == cache.end());
  cache.emplace(4,Resolve{4,95,1280,720});
  const auto reuse = cogbox::renderer::ReusableResolve(cache,100,1280,720,Shape);
  REQUIRE(reuse != cache.end());
  REQUIRE(reuse->first == 4);
}
TEST_CASE("Resolve recycling requires a live texture of exactly the same physical shape", "[resolve-reuse]") {
  std::unordered_map<int, Resolve> cache{{1,{1,1,1280,360}},{2,{2,2,640,720}},{3,{0,0,1280,720}},{4,{4,10,1280,720}},{5,{5,15,1280,720}}};
  const auto reuse = cogbox::renderer::ReusableResolve(cache,100,1280,720,Shape);
  REQUIRE(reuse != cache.end());
  REQUIRE(reuse->first == 4);
  REQUIRE(cogbox::renderer::ReusableResolve(cache,100,720,1280,Shape) == cache.end());
}
TEST_CASE("Streaming guest addresses reuse resources without aliasing two live cache keys", "[resolve-reuse]") {
  std::unordered_map<int, Resolve> cache;
  uint32_t created = 0;
  for (int frame = 1; frame <= 300; ++frame) {
    const uint32_t width = (frame % 2) ? 1280 : 640;
    auto reuse = cogbox::renderer::ReusableResolve(cache,frame,width,720,Shape);
    uint32_t handle = 0;
    if (reuse != cache.end()) { handle = reuse->second.texture; cache.erase(reuse); }
    else handle = ++created;
    cache.emplace(frame,Resolve{handle,uint64_t(frame),width,720});
    for (const auto& [key, value] : cache)
      if (key != frame) REQUIRE(value.texture != handle);
  }
  REQUIRE(created == 6);
  REQUIRE(cache.size() == created);
}
TEST_CASE("Persistent presentation sources can be excluded from scratch reuse", "[resolve-reuse]") {
  std::unordered_map<int, Resolve> cache{{1,{1,1,1280,720}},{2,{2,2,1280,720}}};
  const auto reuse = cogbox::renderer::ReusableResolve(cache,100,1280,720,
      [](const Resolve& value) { return value.texture == 1 ? std::pair<uint32_t,uint32_t>{0,0} : Shape(value); });
  REQUIRE(reuse != cache.end());
  REQUIRE(reuse->first == 2);
}
