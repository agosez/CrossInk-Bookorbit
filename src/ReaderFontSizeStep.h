#pragma once

#include <cstddef>
#include <cstdint>

enum class FontSizeStepMode : uint8_t { Wrap, Clamp };

// Select one adjacent installed point size. The caller owns persistence and
// reflow; this helper stays allocation-free for both built-in and SD fonts.
inline bool changeReaderFontSizeStep(const uint8_t* sizes, const size_t count, uint8_t& pointSize, const bool larger,
                                     const FontSizeStepMode mode = FontSizeStepMode::Wrap) {
  if (!sizes || count < 2) return false;

  size_t current = 0;
  while (current + 1 < count && sizes[current] < pointSize) ++current;
  if (sizes[current] != pointSize && current > 0 && pointSize - sizes[current - 1] <= sizes[current] - pointSize) {
    --current;
  }

  size_t next = current;
  if (larger) {
    if (current + 1 >= count) {
      if (mode == FontSizeStepMode::Clamp) return false;
      next = 0;
    } else {
      ++next;
    }
  } else if (current == 0) {
    if (mode == FontSizeStepMode::Clamp) return false;
    next = count - 1;
  } else {
    --next;
  }

  if (sizes[next] == pointSize) return false;
  pointSize = sizes[next];
  return true;
}
