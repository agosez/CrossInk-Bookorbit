#pragma once

#include <string>
#include <vector>

#include "EpdFontFamily.h"

class GfxRenderer {
 public:
  int getScreenWidth() const { return 800; }
  int getScreenHeight() const { return 480; }
  int getLineHeight(int) const { return 20; }
  int getTextWidth(int, const char* text, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return static_cast<int>(std::string(text).size()) * 8;
  }
  int getSpaceWidth(int, EpdFontFamily::Style = EpdFontFamily::REGULAR) const { return 8; }
  std::vector<std::string> wrappedText(int, const char* text, int, int maxLines, EpdFontFamily::Style) const {
    if (maxLines <= 0 || text == nullptr || text[0] == '\0') return {};
    return {text};
  }
  void displayBuffer() const {}
};
