#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "WordRef.h"

namespace ClipSelectionPaging {

constexpr int TOUCH_DRAG_MOVEMENT_PX = 4;
constexpr uint32_t TOUCH_PAGE_ADVANCE_HOLD_MS = 1000;
constexpr int TOUCH_PAGE_END_DWELL_SLOP_PX = 8;

inline bool hasDraggedFrom(const int startX, const int startY, const int x, const int y) {
  const int deltaX = x - startX;
  const int deltaY = y - startY;
  return deltaX >= TOUCH_DRAG_MOVEMENT_PX || deltaX <= -TOUCH_DRAG_MOVEMENT_PX || deltaY >= TOUCH_DRAG_MOVEMENT_PX ||
         deltaY <= -TOUCH_DRAG_MOVEMENT_PX;
}

inline bool hasHeldPageEndLongEnough(const uint32_t now, const uint32_t heldSince) {
  return static_cast<uint32_t>(now - heldSince) >= TOUCH_PAGE_ADVANCE_HOLD_MS;
}

inline bool isWithinPageEndDwellSlop(const WordRef& word, const int x, const int y) {
  return x >= word.x - TOUCH_PAGE_END_DWELL_SLOP_PX && x < word.x + word.w + TOUCH_PAGE_END_DWELL_SLOP_PX &&
         y >= word.y - TOUCH_PAGE_END_DWELL_SLOP_PX && y < word.y + word.h + TOUCH_PAGE_END_DWELL_SLOP_PX;
}

// Returns the first selectable word on the next loaded page only when the
// cursor is already on the final word of its current page.
inline int nextPageStartIndex(const std::vector<WordRef>& words, const uint16_t* readingOrder,
                              const size_t readingOrderSize, const int cursorIdx) {
  if (!readingOrder || cursorIdx < 0 || cursorIdx >= static_cast<int>(readingOrderSize)) return -1;

  const uint16_t currentWordIndex = readingOrder[cursorIdx];
  if (currentWordIndex >= words.size()) return -1;

  const int currentPage = words[currentWordIndex].pageIdx;
  for (size_t orderIdx = static_cast<size_t>(cursorIdx) + 1; orderIdx < readingOrderSize; ++orderIdx) {
    const uint16_t wordIndex = readingOrder[orderIdx];
    if (wordIndex >= words.size()) return -1;

    const int pageIdx = words[wordIndex].pageIdx;
    if (pageIdx == currentPage) return -1;
    return pageIdx > currentPage ? static_cast<int>(orderIdx) : -1;
  }
  return -1;
}

// A moved touch contact may advance as soon as it reaches the final word; do
// not require a second held sample at that word before changing pages.
inline int nextPageStartIndexForTouchDrag(const bool hasDragged, const int startMarkIdx,
                                          const std::vector<WordRef>& words, const uint16_t* readingOrder,
                                          const size_t readingOrderSize, const int cursorIdx) {
  if (!hasDragged || cursorIdx < startMarkIdx) return -1;
  return nextPageStartIndex(words, readingOrder, readingOrderSize, cursorIdx);
}

}  // namespace ClipSelectionPaging
