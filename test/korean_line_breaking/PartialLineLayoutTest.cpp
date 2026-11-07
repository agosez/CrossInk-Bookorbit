#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {
BlockStyle justifiedStyle() {
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  return style;
}
}  // namespace

TEST(PartialLineLayout, WiderRemainderDoesNotKeepSpeculativeHyphens) {
  Hyphenator::setPreferredLanguage("en");
  for (const bool hyphenation : {false, true}) {
    for (const bool includeLastLine : {false, true}) {
      for (const size_t limit : {0u, 1u, 3u}) {
        SCOPED_TRACE(::testing::Message() << hyphenation << ", " << includeLastLine << ", " << limit);
        GfxRenderer renderer;
        ParsedText text(false, false, hyphenation);
        for (const char* word : {"nd",  "as",    "Jesus", "times",  "it",  "had", "laid",  "hold",  "him", "and",  "he",
                                 "was", "bound", "with",  "chains", "but", "he",  "would", "break", "the", "bonds"}) {
          text.addWord(word, EpdFontFamily::REGULAR);
        }
        const char* lastWord = hyphenation ? "asunder." : "abcdefghijklmnopqrstuvwxyz";
        text.addWord(lastWord, EpdFontFamily::REGULAR);
        const size_t originalSize = text.size();
        size_t narrowLines = 0;
        ASSERT_TRUE(text.layoutAndExtractLines(
            renderer, 0, 203, [&](std::shared_ptr<TextBlock>, uint32_t, uint32_t) { ++narrowLines; }, includeLastLine,
            limit));
        EXPECT_EQ(narrowLines, limit);
        if (limit == 0) {
          EXPECT_EQ(text.size(), originalSize);
          EXPECT_FALSE(text.isContinuation());
        }
        std::vector<std::string> remainingWords;
        ASSERT_TRUE(
            text.layoutAndExtractLines(renderer, 0, 240, [&](std::shared_ptr<TextBlock> block, uint32_t, uint32_t) {
              for (uint16_t i = 0; i < block->wordCount(); ++i) {
                if (block->wordEndsWithInsertedHyphen(i)) EXPECT_EQ(i + 1, block->wordCount());
                remainingWords.emplace_back(block->wordText(i));
              }
            }));
        EXPECT_NE(std::find(remainingWords.begin(), remainingWords.end(), lastWord), remainingWords.end());
        EXPECT_TRUE(text.isEmpty());
      }
    }
  }
}

TEST(PartialLineLayout, HoldsOnlyTheActualFinalLine) {
  for (const bool hyphenation : {false, true}) {
    GfxRenderer renderer;
    ParsedText text(false, false, hyphenation);
    for (const char* word : {"one", "two", "three"}) text.addWord(word, EpdFontFamily::REGULAR);
    size_t lines = 0;
    const auto collect = [&](std::shared_ptr<TextBlock>, uint32_t, uint32_t) { ++lines; };
    ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 60, collect, false, 1));
    EXPECT_EQ(lines, 1u);
    ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 200, collect, false, 3));
    EXPECT_EQ(lines, 1u);
    ASSERT_TRUE(text.layoutAndExtractLines(renderer, 0, 200, collect));
    EXPECT_EQ(lines, 2u);
    EXPECT_TRUE(text.isEmpty());
  }
}

TEST(PartialLineLayout, LastEmittedLineJustifiesWhenMoreTextRemains) {
  for (const bool hyphenation : {false, true}) {
    GfxRenderer renderer;
    ParsedText text(false, false, hyphenation, false, false, 0, justifiedStyle());
    for (const char* word : {"one", "two", "three"}) text.addWord(word, EpdFontFamily::REGULAR);
    ASSERT_TRUE(text.layoutAndExtractLines(
        renderer, 0, 80,
        [&](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) {
          ASSERT_EQ(line->wordCount(), 2);
          EXPECT_EQ(line->wordXpos(1), 56);
        },
        true, 1));
  }
}

TEST(PartialLineLayout, BoundedLinesReserveRubyOverhangAtBothMargins) {
  for (const bool hyphenation : {false, true}) {
    for (const bool rubyFirst : {false, true}) {
      GfxRenderer renderer;
      ParsedText text(false, false, hyphenation);
      text.addWord(rubyFirst ? "a" : "word", EpdFontFamily::REGULAR);
      text.addWord(rubyFirst ? "word" : "a", EpdFontFamily::REGULAR);
      text.addWord("tail", EpdFontFamily::REGULAR);
      text.setRubyForWordAt(rubyFirst ? 0 : 1, "ruby");
      ASSERT_TRUE(text.layoutAndExtractLines(
          renderer, 0, 52,
          [&](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) { EXPECT_EQ(line->wordCount(), 1); }, true, 1));
    }
  }
}

TEST(PartialLineLayout, FollowingTextCanAbsorbRubyOverhang) {
  for (const bool hyphenation : {false, true}) {
    for (const bool attached : {false, true}) {
      for (const size_t limit : {size_t{1}, SIZE_MAX}) {
        GfxRenderer renderer;
        ParsedText text(false, false, hyphenation);
        text.addWord("a", EpdFontFamily::REGULAR);
        text.setRubyForWordAt(0, attached ? "ruby" : "longer");
        text.addWord("b", EpdFontFamily::REGULAR, false, attached);
        ASSERT_TRUE(text.layoutAndExtractLines(
            renderer, 0, attached ? 36 : 56,
            [&](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) { EXPECT_EQ(line->wordCount(), 2); }, true,
            limit));
        EXPECT_TRUE(text.isEmpty());
      }
    }
  }
}

TEST(PartialLineLayout, ContinuationRetainsSourceOffsetsAndWordMetadata) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  style.textIndent = 12;
  ParsedText text(false, false, true, false, false, 0, style, true);
  text.addWord("one", EpdFontFamily::REGULAR, false, false, false, 0, 10, 100);
  text.addWord("two", EpdFontFamily::BOLD, true, false, true, 3, 14, 104, 0, false, 20, 7);
  text.addWord("three", EpdFontFamily::ITALIC, false, false, false, 0, 18, 108);
  size_t lines = 0;
  ASSERT_TRUE(text.layoutAndExtractLines(
      renderer, 0, 60,
      [&](std::shared_ptr<TextBlock> line, uint32_t visible, uint32_t reference) {
        ++lines;
        EXPECT_EQ(visible, 10u);
        EXPECT_EQ(reference, 100u);
        EXPECT_EQ(line->wordXpos(0), 12);
      },
      true, 1));
  EXPECT_TRUE(text.isContinuation());
  ASSERT_TRUE(text.layoutAndExtractLines(
      renderer, 0, 240, [&](std::shared_ptr<TextBlock> line, uint32_t visible, uint32_t reference) {
        ++lines;
        ASSERT_EQ(line->wordCount(), 2);
        EXPECT_STREQ(line->wordText(0), "two");
        EXPECT_EQ(line->wordStyle(0), EpdFontFamily::BOLD | EpdFontFamily::UNDERLINE);
        EXPECT_EQ(line->wordFontSize(0), 20);
        EXPECT_EQ(line->wordLinkId(0), 3);
        EXPECT_NE(line->wordFlags(0) & TextBlock::WORD_FLAG_BACKGROUND_BLACK, 0);
        EXPECT_EQ(line->wordStyle(1), EpdFontFamily::ITALIC);
        EXPECT_EQ(line->wordXpos(0), 0);
        EXPECT_EQ(visible, 14u);
        EXPECT_EQ(reference, 104u);
      }));
  EXPECT_EQ(lines, 2u);
  EXPECT_TRUE(text.isEmpty());
}

TEST(PartialLineLayout, EmittedHyphenRemainderRetainsSourceOffsetsAndStyles) {
  for (const bool hyphenation : {false, true}) {
    GfxRenderer renderer;
    ParsedText text(false, false, hyphenation, false, false, 0, BlockStyle{}, true);
    text.addWord("abcdefghij", EpdFontFamily::BOLD, true, false, true, 2, 40, 400, 0, false, 20, 7);
    ASSERT_TRUE(text.layoutAndExtractLines(
        renderer, 0, 48,
        [&](std::shared_ptr<TextBlock> line, uint32_t visible, uint32_t reference) {
          ASSERT_EQ(line->wordCount(), 1);
          EXPECT_STREQ(line->wordText(0), "abcde-");
          EXPECT_TRUE(line->wordEndsWithInsertedHyphen(0));
          EXPECT_EQ(visible, 40u);
          EXPECT_EQ(reference, 400u);
        },
        true, 1));
    ASSERT_TRUE(text.layoutAndExtractLines(
        renderer, 0, 240, [&](std::shared_ptr<TextBlock> line, uint32_t visible, uint32_t reference) {
          ASSERT_EQ(line->wordCount(), 1);
          EXPECT_STREQ(line->wordText(0), "fghij");
          EXPECT_FALSE(line->wordEndsWithInsertedHyphen(0));
          EXPECT_EQ(line->wordStyle(0), EpdFontFamily::BOLD | EpdFontFamily::UNDERLINE);
          EXPECT_EQ(line->wordFontSize(0), 20);
          EXPECT_EQ(line->wordLinkId(0), 2);
          EXPECT_NE(line->wordFlags(0) & TextBlock::WORD_FLAG_BACKGROUND_BLACK, 0);
          EXPECT_EQ(visible, 45u);
          EXPECT_EQ(reference, 405u);
        }));
    EXPECT_TRUE(text.isEmpty());
  }
}

TEST(PartialLineLayout, BoundedLinesKeepRubyAndPunctuationGroupsTogether) {
  for (const bool hyphenation : {false, true}) {
    for (const bool ruby : {false, true}) {
      GfxRenderer renderer;
      ParsedText text(false, false, hyphenation);
      text.addWord("prefix", EpdFontFamily::REGULAR);
      text.addWord("a", EpdFontFamily::REGULAR);
      text.addWord("?", EpdFontFamily::REGULAR, false, true);
      if (ruby) text.setRubyGroupAt(1, 2, "ruby");
      size_t lines = 0;
      ASSERT_TRUE(text.layoutAndExtractLines(
          renderer, 0, 60,
          [&](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) {
            ++lines;
            ASSERT_EQ(line->wordCount(), 1);
            EXPECT_STREQ(line->wordText(0), "prefix");
          },
          true, 1));
      ASSERT_TRUE(
          text.layoutAndExtractLines(renderer, 0, 240, [&](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) {
            ++lines;
            ASSERT_EQ(line->wordCount(), 2);
            EXPECT_STREQ(line->wordText(0), "a");
            EXPECT_STREQ(line->wordText(1), "?");
            EXPECT_EQ(line->wordXpos(1) - line->wordXpos(0), 8);
            if (ruby) {
              ASSERT_EQ(line->getRubyTexts().size(), 2u);
              EXPECT_EQ(line->getRubyTexts()[0], "ruby");
              EXPECT_EQ(line->getRubyTexts()[1], "");
            }
          }));
      EXPECT_EQ(lines, 2u);
      EXPECT_TRUE(text.isEmpty());
    }
  }
}
