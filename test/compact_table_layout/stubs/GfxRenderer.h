#pragma once

#include <cstdint>

#include "EpdFontFamily.h"

class GfxRenderer {
 public:
  int codepointWidth = 1;

  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    int width = 0;
    const auto* cursor = reinterpret_cast<const unsigned char*>(text);
    while (*cursor != '\0') {
      const unsigned char first = *cursor++;
      if ((first & 0x80) == 0) {
        width += 1;
      } else if ((first & 0xE0) == 0xC0) {
        cursor += 1;
        width += codepointWidth;
      } else if ((first & 0xF0) == 0xE0) {
        cursor += 2;
        width += codepointWidth;
      } else {
        cursor += 3;
        width += codepointWidth;
      }
    }
    return width;
  }

  int getSpaceWidth(int, EpdFontFamily::Style) const { return 1; }
};
