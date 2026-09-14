#include <gtest/gtest.h>

#include "PageCountEstimator.h"

TEST(PageCountEstimator, NoImagesMatchesByteRatio) { EXPECT_EQ(PageCountEstimator::estimate(10, 0, 1000, 100), 100U); }

TEST(PageCountEstimator, ImageOnlyPrefixIsNotMultipliedByBytes) {
  EXPECT_EQ(PageCountEstimator::estimate(2, 2 * PageCountEstimator::kUnitsPerPage, 100000, 100), 2U);
}

TEST(PageCountEstimator, MixedPageProjectsOnlyItsNonImagePortion) {
  constexpr uint32_t halfPage = PageCountEstimator::kUnitsPerPage / 2;
  EXPECT_EQ(PageCountEstimator::nonImageUnits(1, halfPage), halfPage);
  EXPECT_EQ(PageCountEstimator::estimate(1, halfPage, 1000, 100), 5U);
}

TEST(PageCountEstimator, SmallMixedImageFractionStaysFixedPoint) {
  constexpr uint32_t smallImage = 64;
  EXPECT_EQ(PageCountEstimator::nonImageUnits(1, smallImage), 192U);
  EXPECT_EQ(PageCountEstimator::estimate(1, smallImage, 1000, 100), 7U);
}

TEST(PageCountEstimator, MultipleImageUnitsCannotExceedPhysicalPageBudget) {
  EXPECT_EQ(PageCountEstimator::nonImageUnits(3, 4 * PageCountEstimator::kUnitsPerPage), 0U);
  EXPECT_EQ(PageCountEstimator::estimate(3, 4 * PageCountEstimator::kUnitsPerPage, 100000, 1), 3U);
}

TEST(PageCountEstimator, AllImagePrefixRemainsKnownWithoutByteProjection) {
  constexpr uint32_t imageUnits = 8 * PageCountEstimator::kUnitsPerPage;
  EXPECT_EQ(PageCountEstimator::estimate(8, imageUnits, UINT32_MAX, 1), 8U);
  EXPECT_EQ(PageCountEstimator::estimate(8, imageUnits, 1000, 0), 8U);
}

TEST(PageCountEstimator, LargeProjectionIsCapped) {
  EXPECT_EQ(PageCountEstimator::estimate(1, 0, UINT32_MAX, 1), PageCountEstimator::kMaxPages);
}

TEST(PageCountEstimator, ZeroAndMaxByteInputsAreSafe) {
  EXPECT_EQ(PageCountEstimator::projectNonImageUnits(512, 0, 0), 512U);
  EXPECT_EQ(PageCountEstimator::projectNonImageUnits(512, UINT32_MAX, UINT32_MAX), 512U);
  EXPECT_EQ(PageCountEstimator::estimate(UINT32_MAX, 0, UINT32_MAX, 1), PageCountEstimator::kMaxPages);
  EXPECT_EQ(PageCountEstimator::estimate(UINT16_MAX, 0, UINT32_MAX, 1), PageCountEstimator::kMaxPages);
}

TEST(PageCountEstimator, UnknownGroupedItemsKeepOnePageMinimum) {
  constexpr uint64_t knownNonImageUnits = PageCountEstimator::kUnitsPerPage;
  constexpr uint64_t knownBytes = 100;
  constexpr uint16_t unknownItems = 4;
  const uint64_t projected = (10 * knownNonImageUnits + knownBytes / 2) / knownBytes;
  const uint64_t minimum = static_cast<uint64_t>(unknownItems) * PageCountEstimator::kUnitsPerPage;
  EXPECT_EQ(std::max(projected, minimum), minimum);
}
