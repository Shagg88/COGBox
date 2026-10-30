#include <catch2/catch_test_macros.hpp>
#include <xenos/renderer/fenced_retirement.h>
#include <limits>
#include <memory>
#include <vector>

TEST_CASE("Imported views cannot be recycled before their GPU submission completes", "[retirement]") {
  xenos::renderer::FencedRetirement<std::unique_ptr<int>, 4> queue;
  std::vector<int> released;
  auto release = [&](auto& value) { released.push_back(*value); value.reset(); };
  auto first = std::make_unique<int>(10), second = std::make_unique<int>(20);
  REQUIRE(queue.Push(7, std::move(first)));
  REQUIRE(queue.Push(8, std::move(second)));
  queue.Collect(6, release);
  REQUIRE(released.empty());
  REQUIRE(queue.Pending() == 2);
  queue.Collect(7, release);
  REQUIRE(released == std::vector<int>{10});
  REQUIRE(queue.Pending() == 1);
  queue.Collect(7, release);
  REQUIRE(released.size() == 1);
  queue.Collect(8, release);
  REQUIRE((released == std::vector<int>{10,20}));
  REQUIRE(queue.Pending() == 0);
}

TEST_CASE("A bounded retirement queue preserves ownership for synchronous fallback", "[retirement]") {
  xenos::renderer::FencedRetirement<std::unique_ptr<int>, 1> queue;
  auto first = std::make_unique<int>(1), fallback = std::make_unique<int>(2);
  REQUIRE(queue.Push(1, std::move(first)));
  REQUIRE_FALSE(first);
  REQUIRE_FALSE(queue.Push(2, std::move(fallback)));
  REQUIRE(fallback);
  REQUIRE(*fallback == 2);
  REQUIRE(queue.Pending() == 1);
  queue.Collect(1, [](auto& value) { value.reset(); });
  REQUIRE(queue.Push(2, std::move(fallback)));
  REQUIRE_FALSE(fallback);
  queue.Clear([](auto& value) { value.reset(); });
}

TEST_CASE("Retirement does not depend on resource release order", "[retirement]") {
  xenos::renderer::FencedRetirement<std::unique_ptr<int>, 4> queue;
  REQUIRE(queue.Push(9, std::make_unique<int>(9)));
  REQUIRE(queue.Push(3, std::make_unique<int>(3)));
  int released = 0;
  queue.Collect(3, [&](auto& value) { released += *value; value.reset(); });
  REQUIRE(released == 3);
  REQUIRE(queue.Pending() == 1);
  queue.Collect(9, [&](auto& value) { released += *value; value.reset(); });
  REQUIRE(released == 12);
  REQUIRE(queue.Pending() == 0);
}

TEST_CASE("Device removal retains views until recording has stopped and teardown clears them", "[retirement]") {
  xenos::renderer::FencedRetirement<std::unique_ptr<int>, 4> queue;
  REQUIRE(queue.Push(2, std::make_unique<int>(1)));
  REQUIRE(queue.Push(3, std::make_unique<int>(2)));
  int released = 0;
  auto release = [&](auto& value) { released += *value; value.reset(); };
  queue.Collect(std::numeric_limits<uint64_t>::max(), release);
  REQUIRE(released == 0);
  REQUIRE(queue.Pending() == 2);
  queue.Clear(release);
  REQUIRE(released == 3);
  REQUIRE(queue.Pending() == 0);
  queue.Clear(release);
  REQUIRE(released == 3);
}
