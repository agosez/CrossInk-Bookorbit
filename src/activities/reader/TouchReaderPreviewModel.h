#pragma once

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

class TouchReaderPreviewModel {
 public:
  static constexpr size_t TEXT_CAPACITY = 8U * 1024U;
  static constexpr size_t WORD_CAPACITY = 256;
  static constexpr size_t LINE_CAPACITY = 128;

  bool capture(const Page& page, const GfxRenderer& renderer, const int fontId, const uint8_t lineHeightPercent) {
    clear();
    sourceLineHeightPixels =
        static_cast<int16_t>(std::max(1, (renderer.getLineHeight(fontId) * lineHeightPercent + 50) / 100));
    bool previousElementWasLine = false;
    for (const auto& element : page.elements) {
      if (!element || element->getTag() != TAG_PageLine) {
        previousElementWasLine = false;
        continue;
      }
      if (lineCount >= lines.size()) {
        clear();
        return false;
      }
      const auto& pageLine = static_cast<const PageLine&>(*element);
      const auto& block = pageLine.getBlock();
      if (!block) continue;
      if (wordCount + block->wordCount() > words.size()) {
        clear();
        return false;
      }

      Line& line = lines[lineCount++];
      line.y = pageLine.yPos;
      line.firstWord = wordCount;
      line.wordCount = block->wordCount();
      line.style = block->getBlockStyle();
      line.startsParagraph =
          !previousElementWasLine || lineCount == 1 || startsNewParagraph(lines[lineCount - 2], line);
      if (!hasBaseline) {
        firstLineY = line.y;
        hasBaseline = true;
      }

      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        const uint16_t textLength = block->wordTextLen(i);
        if (textSize + textLength + 1U > text.size()) {
          clear();
          return false;
        }
        Word& word = words[wordCount++];
        word.textOffset = textSize;
        word.x = block->wordXpos(i);
        word.style = block->wordStyle(i);
        word.bionicBoundary = block->bionicBoundary(i);
        word.hasSpaceBefore = block->wordHasSpaceBefore(i);
        std::memcpy(text.data() + textSize, block->wordText(i), textLength);
        textSize += textLength;
        text[textSize++] = '\0';
        if (!word.hasSpaceBefore && i > 0) {
          const Word& previous = words[wordCount - 2];
          const int attachedX = previous.x + wordAdvance(renderer, fontId, previous, previous.bionicBoundary != 0) +
                                renderer.getKerning(fontId, lastCodepoint(wordText(previous)),
                                                    firstCodepoint(wordText(word)), previous.style);
          // Some blocks do not report every visible word gap. Recover one
          // only when the rendered source positions prove it was present.
          word.hasSpaceBefore = word.x > attachedX || block->guideDotXOffset(i - 1) > 0;
        }
      }
      previousElementWasLine = true;
    }
    return hasBaseline && wordCount > 0;
  }

  void renderText(const GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                  const int contentWidth, const uint8_t lineHeightPercent, const uint8_t wordSpacing,
                  const uint8_t paragraphAlignment, const bool bionicReadingEnabled, const bool guideReadingEnabled,
                  const bool foregroundBlack) const {
    if (!valid()) return;
    const int currentLineHeight = std::max(1, (renderer.getLineHeight(fontId) * lineHeightPercent + 50) / 100);
    int y = firstLineY + yOffset;
    for (size_t paragraphStart = 0; paragraphStart < lineCount;) {
      size_t paragraphEnd = paragraphStart + 1;
      while (paragraphEnd < lineCount && !lines[paragraphEnd].startsParagraph) ++paragraphEnd;

      const Line& line = lines[paragraphStart];
      const uint16_t firstWord = line.firstWord;
      const uint16_t paragraphWordEnd = lines[paragraphEnd - 1].firstWord + lines[paragraphEnd - 1].wordCount;
      const int leftInset = std::max(0, static_cast<int>(line.style.leftInset()));
      const int rightInset = std::max(0, static_cast<int>(line.style.rightInset()));
      const int availableLeft = xOffset + leftInset;
      const int availableWidth = std::max(1, contentWidth - leftInset - rightInset);
      CssTextAlign alignment = paragraphAlignment < static_cast<uint8_t>(CssTextAlign::None)
                                   ? static_cast<CssTextAlign>(paragraphAlignment)
                                   : line.style.alignment;
      if (alignment == CssTextAlign::None) alignment = CssTextAlign::Justify;

      uint16_t wordIndex = firstWord;
      bool firstPreviewLine = true;
      while (wordIndex < paragraphWordEnd) {
        const int firstLineIndent = firstPreviewLine ? previewFirstLineIndent(renderer, fontId, line, alignment) : 0;
        const int lineWidthLimit = std::max(1, availableWidth - firstLineIndent);
        const uint16_t lineEnd = reflowLineEnd(renderer, fontId, wordIndex, paragraphWordEnd, lineWidthLimit,
                                               wordSpacing, bionicReadingEnabled, guideReadingEnabled);
        renderReflowedLine(renderer, fontId, wordIndex, lineEnd, y, availableLeft, availableWidth, firstLineIndent,
                           alignment, lineEnd == paragraphWordEnd, wordSpacing, bionicReadingEnabled,
                           guideReadingEnabled, foregroundBlack);
        wordIndex = lineEnd;
        firstPreviewLine = false;
        y += currentLineHeight;
      }

      if (paragraphEnd < lineCount) {
        const int sourceGap = lines[paragraphEnd].y - lines[paragraphEnd - 1].y - sourceLineHeightPixels;
        if (sourceGap > 0) y += sourceGap * currentLineHeight / std::max(1, static_cast<int>(sourceLineHeightPixels));
      }
      paragraphStart = paragraphEnd;
    }
  }

  bool valid() const { return hasBaseline && lineCount > 0 && wordCount > 0; }

 private:
  struct Word {
    uint16_t textOffset = 0;
    int16_t x = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    uint8_t bionicBoundary = 0;
    bool hasSpaceBefore = false;
  };

  struct Line {
    int16_t y = 0;
    uint16_t firstWord = 0;
    uint16_t wordCount = 0;
    BlockStyle style{};
    bool startsParagraph = true;
  };

  std::array<char, TEXT_CAPACITY> text{};
  std::array<Word, WORD_CAPACITY> words{};
  std::array<Line, LINE_CAPACITY> lines{};
  uint16_t textSize = 0;
  uint16_t wordCount = 0;
  uint16_t lineCount = 0;
  int16_t firstLineY = 0;
  int16_t sourceLineHeightPixels = 1;
  bool hasBaseline = false;

  static constexpr char GUIDE_DOT_UTF8[] = "\xc2\xb7";
  static constexpr uint32_t GUIDE_DOT_CODEPOINT = 0x00B7;

  const char* wordText(const Word& word) const { return text.data() + word.textOffset; }

  bool startsNewParagraph(const Line& previous, const Line& current) const {
    if (current.y <= previous.y ||
        current.y - previous.y > sourceLineHeightPixels + std::max<int>(2, sourceLineHeightPixels / 3)) {
      return true;
    }
    return previous.style.leftInset() != current.style.leftInset() ||
           previous.style.rightInset() != current.style.rightInset() ||
           previous.style.alignment != current.style.alignment ||
           previous.style.textIndent != current.style.textIndent ||
           previous.style.textIndentDefined != current.style.textIndentDefined ||
           previous.style.isRtl != current.style.isRtl;
  }

  int previewFirstLineIndent(const GfxRenderer& renderer, const int fontId, const Line& line,
                             const CssTextAlign alignment) const {
    const bool naturalAlignment = alignment == CssTextAlign::Justify || alignment == CssTextAlign::Left;
    if (!naturalAlignment || line.wordCount == 0 || words[line.firstWord].x <= 0) return 0;
    if (line.style.textIndentDefined) return std::max(0, static_cast<int>(line.style.textIndent));
    return renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR) * 3;
  }

  uint16_t reflowLineEnd(const GfxRenderer& renderer, const int fontId, const uint16_t firstWord,
                         const uint16_t paragraphWordEnd, const int availableWidth, const uint8_t wordSpacing,
                         const bool bionicEnabled, const bool guideReadingEnabled) const {
    int lineWidth = 0;
    uint16_t wordIndex = firstWord;
    while (wordIndex < paragraphWordEnd) {
      const Word& word = words[wordIndex];
      int width = wordAdvance(renderer, fontId, word, bionicEnabled);
      if (wordIndex > firstWord) {
        width += wordGap(renderer, fontId, words[wordIndex - 1], word, wordSpacing, guideReadingEnabled);
      }
      if (wordIndex > firstWord && lineWidth + width > availableWidth) break;
      lineWidth += width;
      ++wordIndex;
    }
    return wordIndex;
  }

  void renderReflowedLine(const GfxRenderer& renderer, const int fontId, const uint16_t firstWord,
                          const uint16_t lineEnd, const int y, const int availableLeft, const int availableWidth,
                          const int firstLineIndent, const CssTextAlign alignment, const bool isLastLine,
                          const uint8_t wordSpacing, const bool bionicEnabled, const bool guideReadingEnabled,
                          const bool foregroundBlack) const {
    int lineWidth = 0;
    int spaceCount = 0;
    for (uint16_t wordIndex = firstWord; wordIndex < lineEnd; ++wordIndex) {
      const Word& word = words[wordIndex];
      if (wordIndex > firstWord) {
        lineWidth += wordGap(renderer, fontId, words[wordIndex - 1], word, wordSpacing, guideReadingEnabled);
        spaceCount += word.hasSpaceBefore;
      }
      lineWidth += wordAdvance(renderer, fontId, word, bionicEnabled);
    }

    int targetLeft = availableLeft;
    if (alignment == CssTextAlign::Center) {
      targetLeft += std::max(0, (availableWidth - lineWidth) / 2);
    } else if (alignment == CssTextAlign::Right) {
      targetLeft += std::max(0, availableWidth - lineWidth);
    } else {
      targetLeft += firstLineIndent;
    }
    const bool justifyLine =
        alignment == CssTextAlign::Justify && !isLastLine && lineEnd > firstWord + 1 && lineWidth < availableWidth;
    const int justifyExtra =
        justifyLine && spaceCount > 0 ? (availableWidth - firstLineIndent - lineWidth) / spaceCount : 0;

    int wordX = targetLeft;
    for (uint16_t wordIndex = firstWord; wordIndex < lineEnd; ++wordIndex) {
      const Word& word = words[wordIndex];
      if (wordIndex > firstWord) {
        const Word& previous = words[wordIndex - 1];
        const int gap = wordGap(renderer, fontId, previous, word, wordSpacing, guideReadingEnabled);
        if (guideReadingEnabled && word.hasSpaceBefore) {
          const int extra = wordSpacingExtra(wordSpacing);
          const int firstGap =
              renderer.getSpaceAdvance(fontId, lastCodepoint(wordText(previous)), GUIDE_DOT_CODEPOINT, previous.style);
          renderer.drawText(fontId, wordX + firstGap + extra / 2, y, GUIDE_DOT_UTF8, foregroundBlack,
                            EpdFontFamily::REGULAR);
        }
        wordX += gap + (word.hasSpaceBefore ? justifyExtra : 0);
      }
      drawWord(renderer, fontId, wordX, y, word, bionicEnabled, foregroundBlack);
      wordX += wordAdvance(renderer, fontId, word, bionicEnabled);
    }
  }

  static uint32_t firstCodepoint(const char* value) {
    const auto* cursor = reinterpret_cast<const unsigned char*>(value);
    while (true) {
      const uint32_t codepoint = utf8NextCodepoint(&cursor);
      if (codepoint == 0 || codepoint != 0x00AD) return codepoint;
    }
  }

  static uint32_t lastCodepoint(const char* value) {
    const size_t length = std::strlen(value);
    if (length == 0) return 0;
    size_t offset = length - 1;
    while (offset > 0 && (static_cast<uint8_t>(value[offset]) & 0xC0) == 0x80) --offset;
    const auto* cursor = reinterpret_cast<const unsigned char*>(value + offset);
    return utf8NextCodepoint(&cursor);
  }

  static bool isBionicWordCharacter(const uint32_t codepoint) {
    if (codepoint < 128) {
      return ((codepoint | 0x20) >= 'a' && (codepoint | 0x20) <= 'z') || codepoint == '\'';
    }
    if (codepoint >= 0x2000 && codepoint <= 0x2BFF) return codepoint == 0x2018 || codepoint == 0x2019;
    if (codepoint >= 0x00A1 && codepoint <= 0x00BF)
      return codepoint == 0x00AA || codepoint == 0x00B5 || codepoint == 0x00BA;
    if (codepoint >= 0x2E00 && codepoint <= 0x2E7F) return false;
    return codepoint != 0x02D7 && codepoint != 0xFE63 && codepoint != 0xFF0D;
  }

  uint8_t resolvedBionicBoundary(const Word& word, const bool enabled) const {
    if (!enabled || (word.style & EpdFontFamily::BOLD) != 0) return 0;
    if (word.bionicBoundary != 0) return word.bionicBoundary;
    const char* value = wordText(word);
    const auto* cursor = reinterpret_cast<const unsigned char*>(value);
    size_t characters = 0;
    while (*cursor != '\0') {
      const auto* const start = cursor;
      if (!isBionicWordCharacter(utf8NextCodepoint(&cursor)) || cursor <= start) break;
      ++characters;
    }
    if (characters == 0) return 0;
    const size_t boldCharacters = std::clamp<size_t>((characters * 43) / 100, 1, 9);
    if (boldCharacters >= characters) return 0;
    cursor = reinterpret_cast<const unsigned char*>(value);
    for (size_t i = 0; i < boldCharacters; ++i) utf8NextCodepoint(&cursor);
    return static_cast<uint8_t>(std::min<size_t>(cursor - reinterpret_cast<const unsigned char*>(value), UINT8_MAX));
  }

  int wordAdvance(const GfxRenderer& renderer, const int fontId, const Word& word, const bool bionicEnabled) const {
    const char* value = wordText(word);
    const uint8_t boundary = resolvedBionicBoundary(word, bionicEnabled);
    if (boundary == 0 || boundary >= std::strlen(value)) return renderer.getTextAdvanceX(fontId, value, word.style);
    char prefix[40];
    const size_t length = std::min<size_t>({static_cast<size_t>(boundary), sizeof(prefix) - 1, std::strlen(value)});
    std::memcpy(prefix, value, length);
    prefix[length] = '\0';
    const auto boldStyle = static_cast<EpdFontFamily::Style>(word.style | EpdFontFamily::BOLD);
    return renderer.getTextAdvanceX(fontId, prefix, boldStyle, firstCodepoint(value + length)) +
           renderer.getTextAdvanceX(fontId, value + length, word.style);
  }

  static int wordSpacingExtra(const uint8_t wordSpacing) { return std::min<uint8_t>(wordSpacing, 4) * 10; }

  int wordGap(const GfxRenderer& renderer, const int fontId, const Word& left, const Word& right,
              const uint8_t wordSpacing, const bool guideReadingEnabled) const {
    const uint32_t leftCodepoint = lastCodepoint(wordText(left));
    const uint32_t rightCodepoint = firstCodepoint(wordText(right));
    if (!right.hasSpaceBefore) return renderer.getKerning(fontId, leftCodepoint, rightCodepoint, left.style);
    const int extra = wordSpacingExtra(wordSpacing);
    if (!guideReadingEnabled) {
      return renderer.getSpaceAdvance(fontId, leftCodepoint, rightCodepoint, left.style) + extra;
    }
    return renderer.getSpaceAdvance(fontId, leftCodepoint, GUIDE_DOT_CODEPOINT, left.style) +
           renderer.getTextAdvanceX(fontId, GUIDE_DOT_UTF8, EpdFontFamily::REGULAR) +
           renderer.getSpaceAdvance(fontId, GUIDE_DOT_CODEPOINT, rightCodepoint, EpdFontFamily::REGULAR) + extra;
  }

  void drawWord(const GfxRenderer& renderer, const int fontId, const int x, const int y, const Word& word,
                const bool bionicEnabled, const bool foregroundBlack) const {
    const char* value = wordText(word);
    const uint8_t boundary = resolvedBionicBoundary(word, bionicEnabled);
    if (boundary == 0 || boundary >= std::strlen(value)) {
      renderer.drawText(fontId, x, y, value, foregroundBlack, word.style);
      return;
    }
    char prefix[40];
    const size_t length = std::min<size_t>({static_cast<size_t>(boundary), sizeof(prefix) - 1, std::strlen(value)});
    std::memcpy(prefix, value, length);
    prefix[length] = '\0';
    const auto boldStyle = static_cast<EpdFontFamily::Style>(word.style | EpdFontFamily::BOLD);
    renderer.drawText(fontId, x, y, prefix, foregroundBlack, boldStyle);
    renderer.drawText(fontId, x + renderer.getTextAdvanceX(fontId, prefix, boldStyle, firstCodepoint(value + length)),
                      y, value + length, foregroundBlack, word.style);
  }

  void clear() {
    textSize = 0;
    wordCount = 0;
    lineCount = 0;
    firstLineY = 0;
    sourceLineHeightPixels = 1;
    hasBaseline = false;
  }
};
