#pragma once

#include <cstdint>

class GfxRenderer {
 public:
  bool isSdCardFont(int) const { return false; }
  bool releaseSdCardFontForLowMemory(int, bool = false) { return false; }
};
