#pragma once

#include <Memory.h>

namespace MemoryBudget {

// Bytes still to allocate, plus the reserve required after allocation. Live
// old buffers are already excluded from a fresh free-byte snapshot.
struct PoolRequirement {
  size_t bytes = 0;
  size_t largest = 0;
  size_t reserve = 0;
};

constexpr size_t EPUB_PSRAM_RESERVE = 128U * 1024U;
constexpr size_t EPUB_INFLATE_INTERNAL_RESERVE = 16U * 1024U;
constexpr size_t EPUB_FONT_INTERNAL_RESERVE = 40U * 1024U;
constexpr size_t EPUB_LAYOUT_INTERNAL_RESERVE = 44U * 1024U;

inline bool admits(const ByteHeapSnapshot& heap, const PoolRequirement& need) {
  return need.bytes <= heap.free && need.reserve <= heap.free - need.bytes && need.largest <= heap.largest;
}

inline bool admitsOperation(const ByteHeapSnapshot& internal, const ByteHeapSnapshot& external,
                            const PoolRequirement& internalNeed, const PoolRequirement& externalNeed) {
  return admits(internal, internalNeed) && admits(external, externalNeed);
}

inline bool canAllocateInternal(const size_t bytes, const size_t reserve, const size_t remainingLargest = 0) {
  return admits(byteHeapSnapshot(MemoryPool::Internal),
                {bytes, bytes > remainingLargest ? bytes : remainingLargest, reserve});
}

inline bool canAllocatePsram(const size_t bytes) {
  return admits(byteHeapSnapshot(MemoryPool::Psram), {bytes, bytes, EPUB_PSRAM_RESERVE});
}

}  // namespace MemoryBudget
