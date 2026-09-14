#pragma once

#include <algorithm>
#include <cstdint>

namespace TwoFingerSwipe {

enum class Direction : uint8_t { None, Up, Down, Left, Right };

struct CompletedSwipe {
  uint8_t contactCount = 0;
  int startX = 0;
  int startY = 0;
  int endX = 0;
  int endY = 0;
  unsigned long durationMs = 0;
};

inline Direction directionFor(const CompletedSwipe& swipe, const int screenWidth, const int screenHeight) {
  if (swipe.contactCount != 2 || screenWidth < 2 || screenHeight < 2 || swipe.durationMs > 2000) {
    return Direction::None;
  }

  const int dx = swipe.endX - swipe.startX;
  const int dy = swipe.endY - swipe.startY;
  const bool vertical = std::abs(dy) >= std::abs(dx);
  const int primary = vertical ? dy : dx;
  const int cross = vertical ? dx : dy;
  const int axisSize = vertical ? screenHeight : screenWidth;
  const int minimumDistance = std::max(60, axisSize * 6 / 100);
  if (std::abs(primary) < minimumDistance || std::abs(primary) * 2 < std::abs(cross) * 3) {
    return Direction::None;
  }

  if (vertical) return primary < 0 ? Direction::Up : Direction::Down;
  return primary < 0 ? Direction::Left : Direction::Right;
}

inline bool clearDuplicateActions(uint8_t actions[4], const uint8_t notSet, const int editedIndex = -1) {
  bool changed = false;
  for (int i = 0; i < 4; i++) {
    if (actions[i] == notSet) continue;
    for (int j = i + 1; j < 4; j++) {
      if (actions[i] != actions[j]) continue;
      if (editedIndex == j) {
        actions[i] = notSet;
      } else {
        actions[j] = notSet;
      }
      changed = true;
    }
  }
  return changed;
}

}  // namespace TwoFingerSwipe
