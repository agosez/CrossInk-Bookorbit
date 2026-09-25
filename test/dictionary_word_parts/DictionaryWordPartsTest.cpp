#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "DictionaryWordParts.h"

namespace {

struct Part {
  std::string text;
  size_t sourceOffset;
  DictionaryWordPartSeparator separatorBefore;
};

std::vector<Part> splitWordParts(const char* text, const size_t length) {
  std::vector<Part> parts;
  forEachDictionaryWordPart(text, length, [&parts](const DictionaryWordPart& part) {
    parts.push_back({std::string(part.text, part.length), part.sourceOffset, part.separatorBefore});
  });
  return parts;
}

}  // namespace

TEST(DictionaryWordParts, SplitsAsciiHyphenatedCompounds) {
  const auto parts = splitWordParts("half-loft", 9);

  ASSERT_EQ(parts.size(), 2U);
  EXPECT_EQ(parts[0].text, "half");
  EXPECT_EQ(parts[0].sourceOffset, 0U);
  EXPECT_EQ(parts[1].text, "loft");
  EXPECT_EQ(parts[1].sourceOffset, 5U);
  EXPECT_EQ(parts[1].separatorBefore, DictionaryWordPartSeparator::Hyphen);
  EXPECT_STREQ(dictionaryWordPartSeparatorText(parts[1].separatorBefore), "-");
}

TEST(DictionaryWordParts, PreservesTrailingLayoutHyphenForCrossLineLookup) {
  const auto parts = splitWordParts("exter-", 6);

  ASSERT_EQ(parts.size(), 1U);
  EXPECT_EQ(parts[0].text, "exter-");
  EXPECT_EQ(parts[0].sourceOffset, 0U);
  EXPECT_EQ(parts[0].separatorBefore, DictionaryWordPartSeparator::None);
}

TEST(DictionaryWordParts, PreservesAdjacentAsciiHyphens) {
  const auto compound = splitWordParts("foo--bar", 8);
  const auto trailing = splitWordParts("foo--", 5);

  ASSERT_EQ(compound.size(), 1U);
  EXPECT_EQ(compound[0].text, "foo--bar");
  ASSERT_EQ(trailing.size(), 1U);
  EXPECT_EQ(trailing[0].text, "foo--");
}

TEST(DictionaryWordParts, RetainsExistingUnicodeDashSplitting) {
  constexpr char word[] = "half\xE2\x80\x94loft";  // half—loft
  const auto parts = splitWordParts(word, sizeof(word) - 1);

  ASSERT_EQ(parts.size(), 2U);
  EXPECT_EQ(parts[0].text, "half");
  EXPECT_EQ(parts[0].sourceOffset, 0U);
  EXPECT_EQ(parts[1].text, "loft");
  EXPECT_EQ(parts[1].sourceOffset, 7U);
  EXPECT_EQ(parts[1].separatorBefore, DictionaryWordPartSeparator::EmDash);
  EXPECT_STREQ(dictionaryWordPartSeparatorText(parts[1].separatorBefore), "\xE2\x80\x94");
}

TEST(DictionaryWordParts, PositionsRtlCompoundFragmentsInVisualOrder) {
  constexpr int fullWidth = 23;
  constexpr int firstPartWidth = 10;
  constexpr int logicalSecondPartOffset = 13;  // first part plus the hyphen.

  EXPECT_EQ(dictionaryWordPartVisualOffset(fullWidth, 0, firstPartWidth, true), 13);
  EXPECT_EQ(dictionaryWordPartVisualOffset(fullWidth, logicalSecondPartOffset, 10, true), 0);
  EXPECT_EQ(dictionaryWordPartVisualOffset(fullWidth, logicalSecondPartOffset, 10, false), logicalSecondPartOffset);
}
