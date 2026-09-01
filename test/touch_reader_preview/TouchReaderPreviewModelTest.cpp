#include <gtest/gtest.h>

#include <memory>

#include "TouchReaderPreviewModel.h"

namespace {

std::shared_ptr<TextBlock> makeLine(std::initializer_list<const char*> words, const int16_t wordStep = 4,
                                    const bool firstHasSpaceBefore = false) {
  std::vector<TextBlock::Word> line;
  int16_t x = 0;
  for (const char* word : words) {
    TextBlock::Word entry;
    entry.text = word;
    entry.x = x;
    entry.hasSpaceBefore = firstHasSpaceBefore || !line.empty();
    line.push_back(std::move(entry));
    x = static_cast<int16_t>(x + wordStep);
  }
  return std::make_shared<TextBlock>(std::move(line));
}

}  // namespace

TEST(TouchReaderPreviewModel, ReflowsAcrossCapturedLineBreaksBeforeJustifying) {
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "bb"}), 0, 0));
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"cc", "dd"}, 4, true), 0, 10));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 16, 100, 0, static_cast<uint8_t>(CssTextAlign::Justify), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 4U);
  EXPECT_EQ(renderer.drawCalls[0].text, "aa");
  EXPECT_EQ(renderer.drawCalls[0].x, 0);
  EXPECT_EQ(renderer.drawCalls[0].y, 0);
  EXPECT_EQ(renderer.drawCalls[1].text, "bb");
  EXPECT_EQ(renderer.drawCalls[1].x, 6);
  EXPECT_EQ(renderer.drawCalls[1].y, 0);
  EXPECT_EQ(renderer.drawCalls[2].text, "cc");
  EXPECT_EQ(renderer.drawCalls[2].x, 12);
  EXPECT_EQ(renderer.drawCalls[2].y, 0);
  EXPECT_EQ(renderer.drawCalls[3].text, "dd");
  EXPECT_EQ(renderer.drawCalls[3].y, 10);
}

TEST(TouchReaderPreviewModel, WordSpacingReflowsPreviewText) {
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "bb", "cc", "dd"}), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 20, 100, 1, static_cast<uint8_t>(CssTextAlign::Justify), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 4U);
  EXPECT_EQ(renderer.drawCalls[0].y, 0);
  EXPECT_EQ(renderer.drawCalls[1].y, 0);
  EXPECT_EQ(renderer.drawCalls[2].y, 10);
  EXPECT_EQ(renderer.drawCalls[3].y, 10);
}

TEST(TouchReaderPreviewModel, NormalWordSpacingRemovesTheAdditionalPreviewGap) {
  Page page;
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "bb"}), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 40, 100, 2, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);
  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  const int spacedX = renderer.drawCalls[1].x;
  EXPECT_EQ(spacedX, 25);

  renderer.drawCalls.clear();
  model.renderText(renderer, 2, 0, 0, 40, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);
  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  EXPECT_EQ(renderer.drawCalls[1].x, 5);
}

TEST(TouchReaderPreviewModel, WordSpacingUsesSourceWhitespaceInsteadOfPixelGaps) {
  Page page;
  // The source's Bionic/SD-font metrics can put this word at the same x
  // position a plain whole-word advance would predict. The whitespace bit is
  // still authoritative and must survive into the preview.
  page.elements.push_back(std::make_unique<PageLine>(makeLine({"aa", "bb"}, 2), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 30, 100, 1, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  EXPECT_EQ(renderer.drawCalls[1].text, "bb");
  EXPECT_EQ(renderer.drawCalls[1].x, 15);
}

TEST(TouchReaderPreviewModel, NormalWordSpacingRecoversVisibleSourceSpacesWhenMetadataIsMissing) {
  Page page;
  std::vector<TextBlock::Word> source = {{"aa", 0}, {"bb", 23}};
  source[1].hasSpaceBefore = false;
  page.elements.push_back(std::make_unique<PageLine>(std::make_shared<TextBlock>(std::move(source)), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 40, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  EXPECT_EQ(renderer.drawCalls[1].text, "bb");
  EXPECT_EQ(renderer.drawCalls[1].x, 5);
}

TEST(TouchReaderPreviewModel, PreservesAttachedSourceTokens) {
  Page page;
  std::vector<TextBlock::Word> attached = {{"can", 0}, {"not", 3}};
  attached[1].hasSpaceBefore = false;
  page.elements.push_back(std::make_unique<PageLine>(std::make_shared<TextBlock>(std::move(attached)), 0, 0));

  GfxRenderer renderer;
  TouchReaderPreviewModel model;
  ASSERT_TRUE(model.capture(page, renderer, 1, 100));

  model.renderText(renderer, 2, 0, 0, 30, 100, 1, static_cast<uint8_t>(CssTextAlign::Left), false, false, true);

  ASSERT_EQ(renderer.drawCalls.size(), 2U);
  EXPECT_EQ(renderer.drawCalls[1].text, "not");
  EXPECT_EQ(renderer.drawCalls[1].x, 6);
}
