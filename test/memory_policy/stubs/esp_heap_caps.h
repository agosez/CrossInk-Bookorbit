#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

constexpr uint32_t MALLOC_CAP_DEFAULT = 1;
constexpr uint32_t MALLOC_CAP_INTERNAL = 2;
constexpr uint32_t MALLOC_CAP_SPIRAM = 4;
constexpr uint32_t MALLOC_CAP_8BIT = 8;
namespace fakeheap {
struct Heap {
  size_t total;
  size_t free;
  size_t largest;
  int fail = 0;
  size_t attempts = 0;
  size_t failOnAttempt = 0;
};
inline Heap internal{1024 * 1024, 1024 * 1024, 1024 * 1024};
inline Heap external{8 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024};
struct Allocation {
  size_t bytes;
  bool external;
};
inline std::unordered_map<const void*, Allocation> live;
inline bool defaultExternal = false;
inline Heap& heap(uint32_t caps) { return (caps & MALLOC_CAP_SPIRAM) ? external : internal; }
inline void reset(bool psram = true) {
  assert(live.empty());
  internal = {1024 * 1024, 1024 * 1024, 1024 * 1024};
  external = psram ? Heap{8 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024} : Heap{0, 0, 0};
  defaultExternal = false;
}
}  // namespace fakeheap
inline size_t heap_caps_get_total_size(uint32_t caps) { return fakeheap::heap(caps).total; }
inline size_t heap_caps_get_free_size(uint32_t caps) { return fakeheap::heap(caps).free; }
inline size_t heap_caps_get_largest_free_block(uint32_t caps) { return fakeheap::heap(caps).largest; }
inline void* heap_caps_malloc(size_t bytes, uint32_t caps) {
  if (caps == MALLOC_CAP_DEFAULT && fakeheap::defaultExternal) caps = MALLOC_CAP_SPIRAM;
  auto& h = fakeheap::heap(caps);
  ++h.attempts;
  if (h.failOnAttempt == h.attempts) return nullptr;
  if (h.fail) {
    --h.fail;
    return nullptr;
  }
  if (bytes > h.free || bytes > h.largest) return nullptr;
  void* p = std::malloc(bytes);
  if (p) {
    h.free -= bytes;
    fakeheap::live.emplace(p, fakeheap::Allocation{bytes, bool(caps & MALLOC_CAP_SPIRAM)});
  }
  return p;
}
inline void heap_caps_free(void* p) {
  if (!p) return;
  const auto i = fakeheap::live.find(p);
  assert(i != fakeheap::live.end());
  (i->second.external ? fakeheap::external : fakeheap::internal).free += i->second.bytes;
  fakeheap::live.erase(i);
  std::free(p);
}
struct multi_heap_info_t {
  size_t free_blocks = 1;
  size_t allocated_blocks = 0;
};
inline void heap_caps_get_info(multi_heap_info_t* info, uint32_t) { *info = {}; }

inline void* heap_caps_aligned_alloc(size_t alignment, size_t bytes, uint32_t caps) {
  assert(alignment <= alignof(std::max_align_t));
  return heap_caps_malloc(bytes, caps);
}
