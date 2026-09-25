#pragma once

#include <EpdFontFamily.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FocusReadingText {

// Calls drawRun for the two visual runs that make up a Focus Reading word.
// drawRun must consume text synchronously because the bold prefix uses this
// small stack buffer, matching TextBlock::render().
template <typename DrawRun>
bool drawSplitRuns(const char* word, const size_t wordLength, const uint8_t focusBoundary, const int wordX,
                   const uint16_t secondRunOffset, const EpdFontFamily::Style style, const bool isRtl,
                   DrawRun&& drawRun) {
  if (focusBoundary == 0) {
    return false;
  }

  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  char boldText[40];
  const size_t boldLength = std::min<size_t>({static_cast<size_t>(focusBoundary), wordLength, sizeof(boldText) - 1});
  memcpy(boldText, word, boldLength);
  boldText[boldLength] = '\0';
  const int secondRunX = wordX + secondRunOffset;
  if (isRtl) {
    drawRun(wordX, word + boldLength, style);
    drawRun(secondRunX, boldText, boldStyle);
  } else {
    drawRun(wordX, boldText, boldStyle);
    drawRun(secondRunX, word + boldLength, style);
  }
  return true;
}

}  // namespace FocusReadingText
