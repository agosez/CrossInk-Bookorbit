#pragma once

#include <cstdint>

struct EpdFontFamily {
  enum Style : uint8_t {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32,
    SMALL_CAPS = 64,
  };
};
