#pragma once

#include <algorithm>
#include <cstdint>

namespace xenos::preparation {

struct CpuPlan {
  unsigned pipeline_workers;
  unsigned package_readers;
};

// Offline preparation can use the whole CPU. Bound large machines and reduce
// package concurrency when memory is tight: each reader retains decompressed data.
// This is a scheduling allowance, not a hard limit on package allocations.
inline CpuPlan SelectCpuPlan(unsigned logical_processors, uint64_t available_memory) {
  constexpr uint64_t gib = uint64_t{1} << 30;
  const unsigned cpu = std::max(1u, logical_processors);
  const uint64_t spare = available_memory > 4 * gib ? available_memory - 4 * gib : 0;
  const unsigned memory_readers = unsigned(std::min<uint64_t>(8, spare / (2 * gib)));
  return {std::min(cpu, 32u),
          std::max(1u, std::min({cpu / 2, 8u, memory_readers}))};
}

}  // namespace xenos::preparation
