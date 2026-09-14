#pragma once

#include "components/themes/BaseTheme.h"

namespace TouchActionButtons {

constexpr int kDefaultHeight = 56;
constexpr int kDefaultGap = 16;

struct Layout {
  Rect buttons[2];
};

inline Layout vertical(const Rect& container, unsigned char, int, int) { return Layout{{container, container}}; }

}  // namespace TouchActionButtons
