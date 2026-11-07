#include <gtest/gtest.h>

#include "PixelCache.h"
TEST(PixelCache, PaddedDecodeBlockCachesShortImage) {
  Storage.reset();
  PixelCache cache;
  ASSERT_TRUE(cache.begin("/short.pxc", 480, 67, 0, 0, 121));
  EXPECT_EQ(cache.bandRows, 67);
  std::memset(cache.buffer, 0x55, 120 * 67);
  ASSERT_TRUE(cache.finalize());
  const auto& bytes = Storage.bytes("/short.pxc");
  ASSERT_EQ(bytes.size(), 4u + 120u * 67u);
  EXPECT_EQ(bytes[4], 0x55);
  EXPECT_EQ(bytes.back(), 0x55);
}
TEST(PixelCache, RejectsDecodeBandAboveMemoryCeiling) {
  Storage.reset();
  PixelCache cache;
  EXPECT_FALSE(cache.begin("/large.pxc", 480, 1000, 0, 0, 300));
  EXPECT_FALSE(Storage.exists("/large.pxc"));
}

TEST(PixelCache, FailedCloseDiscardsIncompleteImage) {
  Storage.reset();
  PixelCache cache;
  ASSERT_TRUE(cache.begin("/failed.pxc", 480, 67, 0, 0, 121));
  Storage.failClose("/failed.pxc");
  EXPECT_FALSE(cache.finalize());
  EXPECT_FALSE(Storage.exists("/failed.pxc"));
}
