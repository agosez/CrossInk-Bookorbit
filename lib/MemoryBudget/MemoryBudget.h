#pragma once

#include <Arduino.h>
#include <Logging.h>

#include <cstdint>
#include <cstring>

#include "PoolBudget.h"

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
#include <esp_heap_caps.h>
#endif

namespace MemoryBudget {

struct HeapSnapshot {
  uint32_t freeHeap;
  uint32_t maxAllocHeap;
};

struct PsramSnapshot {
  uint32_t totalHeap;
  uint32_t freeHeap;
  uint32_t maxAllocHeap;
};

struct HeapShapeSnapshot {
  uint32_t freeHeap;
  uint32_t maxAllocHeap;
  uint32_t freeBlocks;
  uint32_t allocatedBlocks;
};

struct HeapRequirement {
  uint32_t minFree;
  uint32_t minMaxAlloc;
};

constexpr uint32_t EPUB_INLINE_IMAGE_MIN_FREE = 72U * 1024U;
constexpr uint32_t EPUB_INLINE_IMAGE_MIN_MAX_ALLOC = 48U * 1024U;
constexpr uint32_t EPUB_TEXT_LAYOUT_MIN_FREE = 44U * 1024U;
constexpr uint32_t EPUB_TEXT_LAYOUT_MIN_MAX_ALLOC = 32U * 1024U;
constexpr uint32_t EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_FREE = 120U * 1024U;
constexpr uint32_t EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_MAX_ALLOC = 80U * 1024U;
constexpr uint32_t OPTIONAL_EPUB_REBUILD_MIN_FREE = 96U * 1024U;
constexpr uint32_t OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC = 48U * 1024U;
constexpr uint32_t OPTIONAL_EPUB_PREFETCH_AFTER_SD_FONT_RELEASE_MIN_FREE = 88U * 1024U;
// Initial C3 guard for switching to a different dictionary .cpfont. Both total
// free heap and contiguous maxAlloc matter because font metadata and prewarm
// arenas are separate allocations. Hardware stress logs should tune these.
constexpr uint32_t DICTIONARY_SD_FONT_MIN_FREE = 64U * 1024U;
constexpr uint32_t DICTIONARY_SD_FONT_MIN_MAX_ALLOC = 32U * 1024U;
constexpr uint32_t IMAGE_DECODER_HEADROOM = 16U * 1024U;
constexpr uint32_t JPEG_DECODER_APPROX_BYTES = 20U * 1024U;
constexpr uint32_t EPUB_INLINE_JPEG_MIN_FREE = JPEG_DECODER_APPROX_BYTES + IMAGE_DECODER_HEADROOM;
constexpr uint32_t EPUB_INLINE_JPEG_MIN_MAX_ALLOC = JPEG_DECODER_APPROX_BYTES;
// Page preflight loans the framebuffer for legacy ZIP inflation. PXC2 uses
// a separate session workspace below 6.5 KB; pagination retains only dimensions.
constexpr uint32_t EPUB_OPTIMIZER_PXC_MIN_FREE = 8U * 1024U;
constexpr uint32_t EPUB_OPTIMIZER_PXC_MIN_MAX_ALLOC = 6500U;

inline HeapSnapshot snapshot() { return {ESP.getFreeHeap(), ESP.getMaxAllocHeap()}; }

inline PsramSnapshot psramSnapshot() {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  return {static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
          static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM))};
#else
  return {0, 0, 0};
#endif
}

inline void logEpubHeapPools(const char* stage) {
  const auto internal = snapshot();
  const auto psram = psramSnapshot();
  LOG_INF("EPS", "%s: internal free=%u max=%u; psram free=%u max=%u total=%u", stage, internal.freeHeap,
          internal.maxAllocHeap, psram.freeHeap, psram.maxAllocHeap, psram.totalHeap);
}

inline HeapShapeSnapshot shapeSnapshot() {
  const auto heap = snapshot();
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
  multi_heap_info_t info{};
  heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
  return {heap.freeHeap, heap.maxAllocHeap, static_cast<uint32_t>(info.free_blocks),
          static_cast<uint32_t>(info.allocated_blocks)};
#else
  return {heap.freeHeap, heap.maxAllocHeap, 0, 0};
#endif
}

inline void logHeapShape(const char* stage) {
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const auto heap = shapeSnapshot();
  const uint32_t largestPct = heap.freeHeap == 0 ? 0 : heap.maxAllocHeap * 100U / heap.freeHeap;
  LOG_DBG("HEAP", "stage=%s free=%u max=%u freeBlocks=%u allocBlocks=%u largestPct=%u", stage, heap.freeHeap,
          heap.maxAllocHeap, heap.freeBlocks, heap.allocatedBlocks, largestPct);
#else
  (void)stage;
#endif
}

inline bool hasHeap(const HeapSnapshot heap, const uint32_t minFree, const uint32_t minMaxAlloc) {
  return heap.freeHeap >= minFree && heap.maxAllocHeap >= minMaxAlloc;
}

// Text layout starts with small, fallible allocations: a 4 KB scratch arena and
// a ParsedText object. The operations which really need a large contiguous block
// (table buffering, advance prewarming, and image decoding) each have their own
// max-allocation gate. Do not reject a whole chapter merely because fragmentation
// leaves the general 32 KB background-build threshold a few bytes short.
inline bool hasHeapForEpubTextLayoutStart(const HeapSnapshot heap) {
  return heap.freeHeap >= EPUB_TEXT_LAYOUT_MIN_FREE;
}

inline bool hasHeapForDictionarySdFont(const HeapSnapshot heap) {
  return hasHeap(heap, DICTIONARY_SD_FONT_MIN_FREE, DICTIONARY_SD_FONT_MIN_MAX_ALLOC);
}

inline char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline bool endsWithIgnoreCase(const char* value, const char* suffix) {
  if (!value || !suffix) return false;
  const size_t valueLen = strlen(value);
  const size_t suffixLen = strlen(suffix);
  if (suffixLen > valueLen) return false;

  const char* valueTail = value + valueLen - suffixLen;
  for (size_t i = 0; i < suffixLen; ++i) {
    if (asciiLower(valueTail[i]) != asciiLower(suffix[i])) return false;
  }
  return true;
}

inline bool isJpegSource(const char* source) {
  return endsWithIgnoreCase(source, ".jpg") || endsWithIgnoreCase(source, ".jpeg");
}

inline HeapRequirement epubInlineImageRequirementForSource(const char* source) {
  if (isJpegSource(source)) {
    return {EPUB_INLINE_JPEG_MIN_FREE, EPUB_INLINE_JPEG_MIN_MAX_ALLOC};
  }
  return {EPUB_INLINE_IMAGE_MIN_FREE, EPUB_INLINE_IMAGE_MIN_MAX_ALLOC};
}

inline bool shouldReleaseSdFontCachesForEpubInlineImage(const HeapSnapshot heap) {
  return !hasHeap(heap, EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_FREE, EPUB_INLINE_IMAGE_SD_FONT_RELEASE_MIN_MAX_ALLOC);
}

inline bool hasHeapForEpubInlineImage(const char* tag, const char* source) {
  const auto heap = snapshot();
  const auto requirement = epubInlineImageRequirementForSource(source);
  if (hasHeap(heap, requirement.minFree, requirement.minMaxAlloc)) {
    return true;
  }

  LOG_ERR(tag, "Low heap for inline image (%u free, %u max alloc, need %u/%u); suppressing %s", heap.freeHeap,
          heap.maxAllocHeap, requirement.minFree, requirement.minMaxAlloc, source ? source : "");
  return false;
}

inline bool hasHeapForOptimizerPxcImage(const char* tag, const char* source) {
  const auto heap = snapshot();
  if (hasHeap(heap, EPUB_OPTIMIZER_PXC_MIN_FREE, EPUB_OPTIMIZER_PXC_MIN_MAX_ALLOC)) {
    return true;
  }

  LOG_ERR(tag, "Low heap for optimizer image cache (%u free, %u max alloc, need %u/%u); suppressing %s", heap.freeHeap,
          heap.maxAllocHeap, EPUB_OPTIMIZER_PXC_MIN_FREE, EPUB_OPTIMIZER_PXC_MIN_MAX_ALLOC, source ? source : "");
  return false;
}

inline bool hasHeapForOptionalEpubRebuild(const char* tag, const char* action, const int spineIndex,
                                          const uint32_t minFree = OPTIONAL_EPUB_REBUILD_MIN_FREE,
                                          const uint32_t minMaxAlloc = OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC) {
  const auto heap = snapshot();
  if (hasHeap(heap, minFree, minMaxAlloc)) {
    return true;
  }

  LOG_DBG(tag, "Skipping %s for spine %d: low heap (free=%u, maxAlloc=%u, need free>=%u maxAlloc>=%u)", action,
          spineIndex, heap.freeHeap, heap.maxAllocHeap, minFree, minMaxAlloc);
  return false;
}

inline bool hasHeapForImageDecoder(const char* tag, const char* decoderName, const uint32_t decoderApproxBytes) {
  const auto heap = snapshot();
  const uint32_t minFree = decoderApproxBytes + IMAGE_DECODER_HEADROOM;
  if (hasHeap(heap, minFree, decoderApproxBytes)) {
    return true;
  }

  LOG_ERR(tag, "Not enough heap for %s decoder (%u free, %u max alloc, need %u/%u)", decoderName, heap.freeHeap,
          heap.maxAllocHeap, minFree, decoderApproxBytes);
  return false;
}

}  // namespace MemoryBudget
