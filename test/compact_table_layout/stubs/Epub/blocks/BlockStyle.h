#pragma once

#include <cstdint>

enum class CssTextAlign : uint8_t { None, Left, Center, Right, Justify };

struct BlockStyle {
  CssTextAlign alignment = CssTextAlign::Left;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingLeft = 0;
  int16_t paddingRight = 0;
  bool isRtl = false;

  int16_t leftInset() const { return marginLeft + paddingLeft; }
  int16_t totalHorizontalInset() const { return leftInset() + marginRight + paddingRight; }
};
