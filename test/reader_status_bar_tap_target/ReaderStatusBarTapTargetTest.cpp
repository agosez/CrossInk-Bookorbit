#include <gtest/gtest.h>

#include "ReaderStatusBarTapTarget.h"

TEST(ReaderStatusBarTapTarget, BottomReservesThirtyTwoPixelsForACompactStatusBar) {
  constexpr int screenHeight = 480;
  constexpr int bottomMargin = 5;

  EXPECT_FALSE(ReaderStatusBarTapTarget::containsBottom(442, screenHeight, bottomMargin, 19));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsBottom(443, screenHeight, bottomMargin, 19));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsBottom(474, screenHeight, bottomMargin, 19));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsBottom(475, screenHeight, bottomMargin, 19));
}

TEST(ReaderStatusBarTapTarget, BottomUsesTheFullTallerStatusBar) {
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsBottom(440, 480, 5, 35));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsBottom(439, 480, 5, 35));
}

TEST(ReaderStatusBarTapTarget, TopUsesTheSameMinimumTouchTarget) {
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsTop(7, 480, 8, 19));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsTop(8, 480, 8, 19));
  EXPECT_TRUE(ReaderStatusBarTapTarget::containsTop(39, 480, 8, 19));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsTop(40, 480, 8, 19));
}

TEST(ReaderStatusBarTapTarget, RejectsMissingOrInvalidStatusBarRegions) {
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsBottom(470, 480, 5, 0));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsTop(8, 480, -1, 19));
  EXPECT_FALSE(ReaderStatusBarTapTarget::containsTop(8, 0, 0, 19));
}
