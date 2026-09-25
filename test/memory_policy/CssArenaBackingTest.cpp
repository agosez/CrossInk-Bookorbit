#include <Arduino.h>
#include <CssParser.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

struct CssArenaBackingTest : testing::Test {
  void SetUp() override {
    fakeheap::reset();
    Storage.reset();
  }
  void TearDown() override {
    EXPECT_TRUE(fakeheap::live.empty());
    Storage.reset();
  }
  void createCache() {
    const std::string text =
        "p { text-align: center; margin-top: 12px; } .em { font-weight: bold; } div p { font-style: italic; } "
        ".plain { list-style-type: none; }";
    Storage.put("input.css", {text.begin(), text.end()});
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
    CssParser css("book");
    ASSERT_TRUE(css.loadFromStream(file));
    file.close();
    ASSERT_TRUE(css.saveToCache());
  }
  void checkStyle(CssParser& css) {
    EXPECT_FALSE(css.empty());
    EXPECT_EQ(css.ruleCount(), 3u);
    const auto style = css.resolveStyle("p", "em", {{0, "div", ""}});
    EXPECT_TRUE(style.hasFontWeight());
    EXPECT_TRUE(style.hasFontStyle());
    EXPECT_TRUE(style.hasTextAlign());
    EXPECT_TRUE(style.hasMarginTop());
    EXPECT_EQ(style.marginTop.value, 12);
    EXPECT_EQ(style.fontWeight, CssFontWeight::Bold);
    EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
    EXPECT_EQ(style.textAlign, CssTextAlign::Center);
    const auto listStyle = css.resolveStyle("ol", "plain");
    EXPECT_TRUE(listStyle.hasListStyleType());
    EXPECT_EQ(listStyle.listStyleType, CssListStyleType::None);
  }
};
TEST_F(CssArenaBackingTest, IdenticalStylesForDefaultExternalAndDiskFallback) {
  for (int mode = 0; mode < 3; ++mode) {
    fakeheap::reset(mode != 0);
    createCache();
    CssParser css("book");
    if (mode == 2) fakeheap::external.fail = 1;
    ASSERT_TRUE(css.loadFromCache());
    checkStyle(css);
    if (mode == 2)
      EXPECT_TRUE(fakeheap::live.empty());
    else {
      ASSERT_EQ(fakeheap::live.size(), 1u);
      EXPECT_EQ(fakeheap::live.begin()->second.external, mode == 1);
    }
    css.clear();
    EXPECT_TRUE(css.empty());
    EXPECT_EQ(css.ruleCount(), 0u);
  }
}
TEST_F(CssArenaBackingTest, ExternalHydrationPreservesInternalAndExternalReserves) {
  createCache();
  for (bool internalPressure : {false, true}) {
    fakeheap::reset();
    CssParser css("book");
    if (internalPressure)
      fakeheap::internal.free = 79 * 1024;
    else
      fakeheap::external.free = 128 * 1024;
    ASSERT_TRUE(css.loadFromCache());
    checkStyle(css);
    EXPECT_TRUE(fakeheap::live.empty());
  }
}

TEST_F(CssArenaBackingTest, LargerSourceAllowanceRequiresPsram) {
  EXPECT_EQ(CssParser::maxSourceBytes(), 512u * 1024u);
  fakeheap::reset(false);
  EXPECT_EQ(CssParser::maxSourceBytes(), 128u * 1024u);
}

TEST_F(CssArenaBackingTest, LargeStreamPreservesHiddenRulesThroughCache) {
  // Publisher comments can make a stylesheet exceed the old 128 KiB limit
  // without needing a large rule map. The parser must reach the hidden rule.
  const std::string text = "/*" + std::string(191327, ' ') + "*/\n.modal { display: none; }";
  ASSERT_LT(text.size(), CssParser::maxSourceBytes());
  Storage.put("large.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "large.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  EXPECT_EQ(css.resolveStyle("div", "modal").display, CssDisplay::None);
  ASSERT_TRUE(css.saveToCache());
  css.clear();
  ASSERT_TRUE(css.loadFromCache());
  EXPECT_EQ(css.resolveStyle("div", "modal").display, CssDisplay::None);
}

TEST_F(CssArenaBackingTest, PreviousCacheVersionIsInvalidated) {
  CssParser css("book");
  ASSERT_TRUE(css.saveToCache());
  FsFile file;
  // Obtain a valid empty cache, then mark it as the prior cache revision.
  ASSERT_TRUE(Storage.openFileForRead("test", "book/css_rules.cache", file));
  std::vector<uint8_t> bytes(file.size());
  ASSERT_EQ(file.read(bytes.data(), bytes.size()), static_cast<int>(bytes.size()));
  file.close();
  bytes[4] = 16;
  Storage.put("book/css_rules.cache", bytes);
  EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Invalid);
}

TEST_F(CssArenaBackingTest, PsramParsingMergesSelectorsAndReleasesAllSlabs) {
  const std::string text = ".modal { display: none; } .MODAL { font-weight: bold; }";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  EXPECT_EQ(css.ruleCount(), 1u);
  EXPECT_EQ(css.resolveStyle("div", "modal").display, CssDisplay::None);
  EXPECT_EQ(css.resolveStyle("div", "modal").fontWeight, CssFontWeight::Bold);
  ASSERT_FALSE(fakeheap::live.empty());
  for (const auto& allocation : fakeheap::live) EXPECT_TRUE(allocation.second.external);
  css.clear();
  EXPECT_TRUE(fakeheap::live.empty());
}

TEST_F(CssArenaBackingTest, PsramParsingAllocationFailureStopsSafely) {
  const std::string text = ".modal { display: none; }";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  fakeheap::external.fail = 1;
  EXPECT_FALSE(css.loadFromStream(file));
  file.close();
  EXPECT_TRUE(css.empty());
  EXPECT_TRUE(fakeheap::live.empty());
  EXPECT_EQ(fakeheap::internal.attempts, 0u);
}

TEST_F(CssArenaBackingTest, ManyRulesSurviveArenaGrowthAndCacheRoundTrip) {
  std::string text;
  for (int i = 0; i < 1303; ++i) text += ".rule" + std::to_string(i) + " { display: none; }\n";
  Storage.put("input.css", {text.begin(), text.end()});
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "input.css", file));
  CssParser css("book");
  ASSERT_TRUE(css.loadFromStream(file));
  file.close();
  ASSERT_EQ(css.ruleCount(), 1303u);
  for (int i = 0; i < 1303; ++i)
    EXPECT_EQ(css.resolveStyle("div", "rule" + std::to_string(i)).display, CssDisplay::None);
  ASSERT_TRUE(css.saveToCache());
  css.clear();
  ASSERT_TRUE(css.loadFromCache());
  ASSERT_EQ(css.ruleCount(), 1303u);
  for (int i = 0; i < 1303; ++i)
    EXPECT_EQ(css.resolveStyle("div", "rule" + std::to_string(i)).display, CssDisplay::None);
}
