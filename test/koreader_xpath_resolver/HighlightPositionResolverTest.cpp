#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "HighlightPositionResolver.h"

namespace {
std::shared_ptr<Epub> epubWith(std::string xhtml) {
  std::vector<std::string> spine;
  spine.push_back(std::move(xhtml));
  return std::make_shared<Epub>(std::move(spine));
}

// Chapters from the sample EPUB attached to issue #84.
constexpr char kBlockquoteChapter[] = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head><title>Blockquotes only</title></head>
  <body>
    <h1>Blockquotes only</h1>
    <blockquote id="quote-1">A paper lantern hung beside the harbor window. Mara remembered the color of the evening.</blockquote>
    <blockquote id="quote-2">At dawn, a small ferry crossed the quiet channel. Its wake split the silver water into two paths.</blockquote>
  </body>
</html>)";

constexpr char kParagraphChapter[] = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head><title>Paragraph control</title></head>
  <body>
    <h1>Paragraph control</h1>
    <p id="paragraph-1">A blue kite rose over the field. The string hummed in the afternoon wind.</p>
    <p id="paragraph-2">Later, the kite came down gently beside the old stone wall.</p>
  </body>
</html>)";

struct Resolved {
  bool ok = false;
  std::string pos0;
  std::string pos1;
  std::string sourceText;
};

Resolved resolve(const char* xhtml, const uint16_t paragraphIndex, const std::string& highlightText) {
  Resolved r;
  r.ok = HighlightPositionResolver::findHighlightXPointers(epubWith(xhtml), 0, paragraphIndex, highlightText, r.pos0,
                                                           r.pos1, &r.sourceText);
  return r;
}
}  // namespace

TEST(HighlightPositionResolver, ResolvesHighlightInBlockquoteOnlyChapter) {
  // No <p> precedes the text, so the layout's paragraph hint is 0.
  const auto r = resolve(kBlockquoteChapter, 0, "A paper lantern hung beside the harbor window.");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.pos0, "/body/DocFragment[1]/body/blockquote[1]/text()[1].0");
  EXPECT_EQ(r.pos1, "/body/DocFragment[1]/body/blockquote[1]/text()[1].46");
  EXPECT_EQ(r.sourceText, "A paper lantern hung beside the harbor window.");
}

TEST(HighlightPositionResolver, ResolvesMidBlockquoteOffset) {
  const auto r = resolve(kBlockquoteChapter, 0, "Its wake split the silver water into two paths.");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.pos0, "/body/DocFragment[1]/body/blockquote[2]/text()[1].50");
  EXPECT_EQ(r.pos1, "/body/DocFragment[1]/body/blockquote[2]/text()[1].97");
}

TEST(HighlightPositionResolver, SpansTwoBlockquotesWithABlockBreak) {
  const auto r = resolve(kBlockquoteChapter, 0, "evening. At dawn");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.pos0, "/body/DocFragment[1]/body/blockquote[1]/text()[1].80");
  EXPECT_EQ(r.pos1, "/body/DocFragment[1]/body/blockquote[2]/text()[1].7");
  EXPECT_EQ(r.sourceText, "evening.\nAt dawn");
}

TEST(HighlightPositionResolver, ResolvesHeadingText) {
  const auto r = resolve(kBlockquoteChapter, 0, "Blockquotes only");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.pos0, "/body/DocFragment[1]/body/h1[1]/text()[1].0");
  EXPECT_EQ(r.pos1, "/body/DocFragment[1]/body/h1[1]/text()[1].16");
}

TEST(HighlightPositionResolver, KeepsParagraphPositionsUnchanged) {
  const auto r = resolve(kParagraphChapter, 1, "A blue kite rose over the field.");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.pos0, "/body/DocFragment[1]/body/p[1]/text()[1].0");
  EXPECT_EQ(r.pos1, "/body/DocFragment[1]/body/p[1]/text()[1].32");
}

TEST(HighlightPositionResolver, DropsLeadingWhitespaceOfIndentedBlockText) {
  const auto r =
      resolve("<html><body><div>\n  <blockquote>\n    Indented   words here\n  </blockquote>\n</div></body></html>", 0,
              "words here");
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(r.pos0, "/body/DocFragment[1]/body/div[1]/blockquote[1]/text()[1].9");
  EXPECT_EQ(r.pos1, "/body/DocFragment[1]/body/div[1]/blockquote[1]/text()[1].19");
}

TEST(HighlightPositionResolver, PrefersTheOccurrenceNearestTheParagraphHint) {
  constexpr char kRepeated[] = "<html><body><p>Echo one</p><p>Other</p><p>Echo two</p></body></html>";
  EXPECT_EQ(resolve(kRepeated, 1, "Echo").pos0, "/body/DocFragment[1]/body/p[1]/text()[1].0");
  EXPECT_EQ(resolve(kRepeated, 3, "Echo").pos0, "/body/DocFragment[1]/body/p[3]/text()[1].0");
}

TEST(HighlightPositionResolver, RejectsUnknownParagraphHint) {
  EXPECT_FALSE(resolve(kBlockquoteChapter, UINT16_MAX, "A paper lantern").ok);
}
