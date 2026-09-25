#pragma once

#include <algorithm>
#include <cstdint>

constexpr uint32_t clampStablePage(const uint32_t page, const uint32_t pageCount) {
  if (pageCount == 0) return 0;
  return std::clamp<uint32_t>(page, 1, pageCount);
}

constexpr uint32_t adjustStablePage(const uint32_t page, const int delta, const uint32_t pageCount) {
  if (pageCount == 0) return 0;
  const int64_t adjusted = static_cast<int64_t>(clampStablePage(page, pageCount)) + delta;
  return static_cast<uint32_t>(std::clamp<int64_t>(adjusted, 1, pageCount));
}

constexpr int16_t stablePageToPermille(const uint32_t page, const uint32_t pageCount) {
  if (pageCount <= 1) return 0;
  const uint64_t offset = clampStablePage(page, pageCount) - 1;
  return static_cast<int16_t>((offset * 1000) / (pageCount - 1));
}

constexpr uint32_t stablePageFromPermille(const int16_t permille, const uint32_t pageCount) {
  if (pageCount <= 1) return pageCount;
  const uint32_t clampedPermille = static_cast<uint32_t>(std::clamp<int>(permille, 0, 1000));
  return 1 + static_cast<uint32_t>((static_cast<uint64_t>(clampedPermille) * (pageCount - 1) + 500) / 1000);
}
