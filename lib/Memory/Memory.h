#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#endif

// Nothrow versions of std::make_unique. Return nullptr on allocation failure
// instead of calling abort() (the default when exceptions are disabled on ESP32).
//
// Single object:
//   auto obj = makeUniqueNoThrow<PNG>();
//   if (!obj) { LOG_ERR("TAG", "OOM"); return false; }
//
// Array:
//   auto buf = makeUniqueNoThrow<uint8_t[]>(size);
//   if (!buf) { LOG_ERR("TAG", "OOM"); return false; }
//   buf[0] = 0xFF;
//   someApi(buf.get(), size);
//

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Elem = std::remove_extent_t<T>;
  return std::unique_ptr<T>(new (std::nothrow) Elem[count]());
}

// malloc-backed byte buffers for capability-specific ESP32 heaps. These are
// runtime-sized working buffers, so stack/static storage is not suitable.
// The custom deleter keeps ownership automatic on every early-return path.
struct HeapByteBufferDeleter {
  void operator()(uint8_t* ptr) const {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
    heap_caps_free(ptr);
#else
    std::free(ptr);
#endif
  }
};

using HeapByteBuffer = std::unique_ptr<uint8_t[], HeapByteBufferDeleter>;

inline bool psramHeapAvailable() {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
  return false;
#endif
}

inline HeapByteBuffer makeHeapByteBufferNoThrow(const size_t count) {
  if (count == 0) return {};
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  return HeapByteBuffer(static_cast<uint8_t*>(heap_caps_malloc(count, MALLOC_CAP_DEFAULT)));
#else
  return HeapByteBuffer(static_cast<uint8_t*>(std::malloc(count)));
#endif
}

inline HeapByteBuffer makePsramByteBufferNoThrow(const size_t count) {
  if (count == 0) return {};
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  return HeapByteBuffer(static_cast<uint8_t*>(heap_caps_malloc(count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
#else
  (void)count;
  return {};
#endif
}

// Explicit backing for migrated byte storage. Default callers above retain
// their existing allocator preference. Pool snapshots are hints, not reservations.
enum class MemoryPool : uint8_t { None, Internal, Psram };

struct ByteHeapSnapshot {
  size_t total;
  size_t free;
  size_t largest;
};

inline ByteHeapSnapshot byteHeapSnapshot(const MemoryPool pool) {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  const uint32_t caps = (pool == MemoryPool::Psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
  return {heap_caps_get_total_size(caps), heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps)};
#else
  return pool == MemoryPool::Psram ? ByteHeapSnapshot{0, 0, 0} : ByteHeapSnapshot{SIZE_MAX, SIZE_MAX, SIZE_MAX};
#endif
}

inline MemoryPool byteBufferPool(const void* ptr) {
  if (!ptr) return MemoryPool::None;
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  return esp_ptr_external_ram(ptr) ? MemoryPool::Psram : MemoryPool::Internal;
#else
  return MemoryPool::Internal;
#endif
}

inline const char* memoryPoolName(const MemoryPool pool) {
  return pool == MemoryPool::Psram ? "psram" : pool == MemoryPool::Internal ? "internal" : "none";
}

inline HeapByteBuffer makeInternalByteBufferNoThrow(const size_t count) {
  if (count == 0) return {};
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  return HeapByteBuffer(static_cast<uint8_t*>(heap_caps_malloc(count, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#else
  return HeapByteBuffer(static_cast<uint8_t*>(std::malloc(count)));
#endif
}

// Preserve libc's configured size-based heap preference for migrated owners
// which previously used malloc/new[]. The older makeHeap helper above retains
// its explicit MALLOC_CAP_DEFAULT behavior for its existing callers.
inline HeapByteBuffer makeDefaultByteBufferNoThrow(const size_t count) {
  if (count == 0) return {};
#if defined(CROSSINK_MEMORY_TEST)
  // Native failure-injection seam; no hooks or dispatch in firmware builds.
  return HeapByteBuffer(static_cast<uint8_t*>(heap_caps_malloc(count, MALLOC_CAP_DEFAULT)));
#else
  return HeapByteBuffer(static_cast<uint8_t*>(std::malloc(count)));
#endif
}

// Arena headers and payloads require natural C++ alignment even on heaps whose
// byte-allocation minimum alignment is smaller. None selects the default heap.
inline HeapByteBuffer makeAlignedByteBufferNoThrow(const size_t count, const MemoryPool pool = MemoryPool::None) {
  if (count == 0) return {};
#if !defined(CROSSINK_MEMORY_TEST)
  if (pool == MemoryPool::None) {
    constexpr size_t alignment = alignof(std::max_align_t);
    if (count > SIZE_MAX - (alignment - 1)) return {};
    const size_t alignedCount = (count + alignment - 1) & ~(alignment - 1);
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
    // ESP's newlib exposes posix_memalign to C++; its implementation uses the
    // same size-based default heap preference as malloc.
    void* ptr = nullptr;
    if (::posix_memalign(&ptr, alignment, alignedCount) != 0) return {};
    return HeapByteBuffer(static_cast<uint8_t*>(ptr));
#else
    return HeapByteBuffer(static_cast<uint8_t*>(std::aligned_alloc(alignment, alignedCount)));
#endif
  }
#endif
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  const uint32_t caps = pool == MemoryPool::None
                            ? MALLOC_CAP_DEFAULT
                            : (pool == MemoryPool::Psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
  return HeapByteBuffer(static_cast<uint8_t*>(heap_caps_aligned_alloc(alignof(std::max_align_t), count, caps)));
#else
  if (pool == MemoryPool::Psram) return {};
  return HeapByteBuffer(static_cast<uint8_t*>(std::malloc(count)));
#endif
}

// Helper struct to call a cleanup function on exit from any scope.
// Use with a lambda to avoid unnecessary allocations from std::function/std::bind:
// Example:
//   auto jpeg = makeUniqueNoThrow<JPEGDEC>();
//   ScopedCleanup cleanup{[&jpeg]{ jpeg->close(); }};
//
template <typename F>
struct [[nodiscard]] ScopedCleanup final {
  const F fn;
  explicit ScopedCleanup(F f) : fn{std::move(f)} {}
  ScopedCleanup(const ScopedCleanup&) = delete;
  ScopedCleanup& operator=(const ScopedCleanup&) = delete;
  ScopedCleanup(ScopedCleanup&&) = delete;
  ScopedCleanup& operator=(ScopedCleanup&&) = delete;
  ~ScopedCleanup() { fn(); }
};

template <typename F>
ScopedCleanup(F) -> ScopedCleanup<F>;
