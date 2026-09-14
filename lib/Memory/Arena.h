#pragma once

#include <Logging.h>
#include <Memory.h>
#include <PoolBudget.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

// Chained slab arena allocator.
//
// Allocations are O(1) bump-pointer within a slab. When a slab fills up, a new
// one is appended from the heap rather than failing immediately. clear() frees
// extra slabs and resets the first, so the initial reservation is preserved for
// reuse without causing heap fragmentation.
//
// Best suited to burst-then-discard lifetimes: parse a chapter into the arena,
// write the results out, then call clear(). The heap sees one large allocation
// and one large free instead of hundreds of small interleaved ones.
//
// NOT thread-safe. Each Arena should be owned by a single task/activity.
//
// Objects stored in an Arena must have trivial destructors, or destructors must
// be called manually before clear() / release().
//
// Example:
//   Arena arena;
//   if (!arena.init(16 * 1024)) { LOG_ERR(TAG, "OOM"); return false; }
//
//   auto* node = arenaNew<HtmlNode>(arena, arg1, arg2);
//   if (!node) { LOG_ERR(TAG, "arena OOM"); return false; }
//
//   arena.clear();   // all nodes gone, no per-object free needed
//   arena.release(); // done with the arena entirely (onExit)

enum class ArenaBacking : uint8_t { Default, PsramOnly, PsramPreferred };

struct alignas(std::max_align_t) ArenaSlab {
  ArenaSlab* next;
  size_t capacity;
  size_t offset;
  MemoryPool pool;

  uint8_t* data() { return reinterpret_cast<uint8_t*>(this + 1); }
};

struct ArenaCheckpoint {
  ArenaSlab* slab;
  size_t offset;
};

struct Arena {
  ArenaSlab* head = nullptr;
  ArenaSlab* current = nullptr;
  size_t slabSize = 0;

  explicit Arena(const ArenaBacking backing = ArenaBacking::Default) : backing_(backing) {}
  ~Arena() { release(); }
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Allocate the first slab. Must be called before alloc(). Returns false on OOM.
  bool init(size_t slabBytes) {
    release();
    if (slabBytes == 0) return false;
    slabSize = slabBytes;
    head = current = allocSlab(slabBytes);
    if (!head) {
      LOG_ERR("Arena", "init failed (slab=%u bytes)", (unsigned)slabBytes);
      return false;
    }
    return true;
  }

  // Release all slabs. Arena is unusable until init() is called again.
  void release() {
    ArenaSlab* s = head;
    while (s) {
      ArenaSlab* n = s->next;
      freeSlab(s);
      s = n;
    }
    head = current = nullptr;
    slabSize = 0;
  }

  // Allocate `size` bytes aligned to `align` (must be a power of two).
  // Returns nullptr only when the heap itself is exhausted.
  void* alloc(size_t size, size_t align = alignof(std::max_align_t)) {
    if (!current || align == 0 || (align & (align - 1)) != 0 || align > alignof(std::max_align_t)) return nullptr;
    void* p = tryAlloc(current, size, align);
    if (p) return p;

    // Grow: allocate a new slab large enough for this request.
    const size_t needed = size > slabSize ? size : slabSize;
    ArenaSlab* next = allocSlab(needed);
    if (!next) {
      LOG_ERR("Arena", "OOM growing arena (need %u bytes)", (unsigned)size);
      return nullptr;
    }
    current->next = next;
    current = next;
    return tryAlloc(current, size, align);
  }

  // Free all extra slabs and reset the first slab to empty.
  // Does NOT call destructors on any objects in the arena.
  void clear() {
    ArenaSlab* s = head ? head->next : nullptr;
    while (s) {
      ArenaSlab* n = s->next;
      freeSlab(s);
      s = n;
    }
    if (head) {
      head->next = nullptr;
      head->offset = 0;
    }
    current = head;
  }

  // Save current position for scratch (temporary) use.
  ArenaCheckpoint save() const { return {current, current ? current->offset : 0}; }

  // Restore to a previously saved checkpoint. Frees any slabs allocated
  // after the checkpoint slab. Must be used in LIFO order.
  void restore(const ArenaCheckpoint& cp) {
    if (!cp.slab) return;
    ArenaSlab* s = cp.slab->next;
    while (s) {
      ArenaSlab* n = s->next;
      freeSlab(s);
      s = n;
    }
    cp.slab->next = nullptr;
    cp.slab->offset = cp.offset;
    current = cp.slab;
  }

  // Total bytes allocated across all slabs (useful for high-water logging).
  size_t used() const {
    size_t total = 0;
    for (const ArenaSlab* s = head; s; s = s->next) total += s->offset;
    return total;
  }

  size_t capacityInPool(const MemoryPool pool) const {
    size_t total = 0;
    for (const ArenaSlab* s = head; s; s = s->next) {
      if (s->pool == pool) total += sizeof(ArenaSlab) + s->capacity;
    }
    return total;
  }

 private:
  ArenaBacking backing_ = ArenaBacking::Default;

  ArenaSlab* allocSlab(const size_t dataSize) {
    if (dataSize > SIZE_MAX - sizeof(ArenaSlab)) return nullptr;
    const size_t bytes = sizeof(ArenaSlab) + dataSize;
    HeapByteBuffer storage;
    if (backing_ == ArenaBacking::Default) {
      storage = makeAlignedByteBufferNoThrow(bytes);
    } else {
      if (MemoryBudget::canAllocatePsram(bytes)) storage = makeAlignedByteBufferNoThrow(bytes, MemoryPool::Psram);
      if (!storage && backing_ == ArenaBacking::PsramPreferred &&
          MemoryBudget::canAllocateInternal(bytes, MemoryBudget::EPUB_LAYOUT_INTERNAL_RESERVE, 8U * 1024U)) {
        storage = makeAlignedByteBufferNoThrow(bytes, MemoryPool::Internal);
      }
    }
    if (!storage) return nullptr;
    const auto pool = byteBufferPool(storage.get());
    // Ownership transfers to the slab chain; release/clear/restore use the
    // matching capability deleter, including mixed-pool fallback chains.
    return ::new (storage.release()) ArenaSlab{nullptr, dataSize, 0, pool};
  }

  static void freeSlab(ArenaSlab* slab) { HeapByteBufferDeleter{}(reinterpret_cast<uint8_t*>(slab)); }

  static void* tryAlloc(ArenaSlab* slab, const size_t size, const size_t align) {
    if (!slab || slab->offset > SIZE_MAX - (align - 1)) return nullptr;
    const size_t aligned = (slab->offset + align - 1u) & ~(align - 1u);
    if (aligned > slab->capacity || size > slab->capacity - aligned) return nullptr;
    slab->offset = aligned + size;
    return slab->data() + aligned;
  }
};

// Construct a T in the arena using placement new. Returns nullptr on OOM.
// Objects with nontrivial destructors must be explicitly destroyed by callers.
template <typename T, typename... Args>
T* arenaNew(Arena& a, Args&&... args) {
  void* mem = a.alloc(sizeof(T), alignof(T));
  if (!mem) return nullptr;
  return ::new (mem) T(std::forward<Args>(args)...);
}

template <typename T>
T* arenaNewArray(Arena& a, const size_t count) {
  if (count == 0 || count > SIZE_MAX / sizeof(T)) return nullptr;
  void* mem = a.alloc(sizeof(T) * count, alignof(T));
  if (!mem) return nullptr;
  return ::new (mem) T[count]();
}
