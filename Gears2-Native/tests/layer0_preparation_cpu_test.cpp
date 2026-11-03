#include <catch2/catch_test_macros.hpp>
#include <xenos/renderer/preparation_cpu.h>

using xenos::preparation::SelectCpuPlan;
constexpr uint64_t GiB = uint64_t{1} << 30;

TEST_CASE("Offline preparation uses a 16-thread CPU without excessive package readers", "[preparation]") {
  const auto plan = SelectCpuPlan(16, 20 * GiB);
  CHECK(plan.pipeline_workers == 16);
  CHECK(plan.package_readers == 8);
}

TEST_CASE("Offline package concurrency shrinks with available memory", "[preparation]") {
  CHECK(SelectCpuPlan(16, 16 * GiB).package_readers == 6);
  CHECK(SelectCpuPlan(16, 6 * GiB).package_readers == 1);
  CHECK(SelectCpuPlan(16, 0).package_readers == 1);
}

TEST_CASE("Offline preparation handles small and very large CPUs", "[preparation]") {
  const auto unknown = SelectCpuPlan(0, 0);
  CHECK(unknown.pipeline_workers == 1);
  CHECK(unknown.package_readers == 1);
  CHECK(SelectCpuPlan(1, 64 * GiB).package_readers == 1);
  const auto large = SelectCpuPlan(256, 512 * GiB);
  CHECK(large.pipeline_workers == 32);
  CHECK(large.package_readers == 8);
}
