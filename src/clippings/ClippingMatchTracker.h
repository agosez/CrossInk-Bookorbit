#pragma once

#include <cstdint>

struct ClippingMatchTracker {
  bool record(const uint16_t startWord, const uint16_t endWord) {
    if (!hasMatch) {
      firstStartWord = startWord;
      firstEndWord = endWord;
      hasMatch = true;
      return true;
    }
    if (startWord != firstStartWord || endWord != firstEndWord) {
      ambiguous = true;
    }
    return false;
  }

  bool found() const { return hasMatch; }
  bool unique() const { return hasMatch && !ambiguous; }
  uint16_t startWord() const { return firstStartWord; }
  uint16_t endWord() const { return firstEndWord; }

 private:
  uint16_t firstStartWord = 0;
  uint16_t firstEndWord = 0;
  bool hasMatch = false;
  bool ambiguous = false;
};
