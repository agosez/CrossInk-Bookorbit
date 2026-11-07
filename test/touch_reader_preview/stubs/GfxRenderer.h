#pragma once
namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

#include <cstdint>
#include <string>
#include <vector>

#include "EpdFontFamily.h"

class GfxRenderer {
 public:
  struct DrawCall {
    std::string text;
    int x = 0;
    int y = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  };

  mutable std::vector<DrawCall> drawCalls;
  int spaceWidth = 1;

  bool isFontCacheScanning() const { return false; }

  int getLineHeight(int) const { return 10; }
  int getTextHeight(int) const { return 10; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return spaceWidth; }
  int getSpaceAdvance(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return spaceWidth; }
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style, int8_t = 0) const { return 0; }

  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style) const {
    return static_cast<int>(std::string(text).size()) * (fontId == 2 ? 2 : 1);
  }

  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style, uint32_t, int8_t = 0) const {
    return getTextAdvanceX(fontId, text, style);
  }

  void drawText(int, int x, int y, const char* text, bool, EpdFontFamily::Style style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t = 0) const {
    drawCalls.push_back({text, x, y, style});
  }
};
