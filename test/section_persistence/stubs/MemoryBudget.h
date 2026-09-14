#pragma once

#include <cstdint>

namespace MemoryBudget {
struct Snapshot {
  uint32_t freeHeap = UINT32_MAX;
  uint32_t maxAllocHeap = UINT32_MAX;
};
inline Snapshot snapshot() { return {}; }
inline void logEpubHeapPools(const char*) {}
}  // namespace MemoryBudget
