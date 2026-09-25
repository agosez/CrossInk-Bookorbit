#include <GfxRenderer.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <JpegToFramebufferConverter.h>
#include <MemoryBudget.h>
#include <gtest/gtest.h>

class JpegDecoderPolicyTest : public testing::Test {
 protected:
  void SetUp() override {
    fakeheap::reset();
    Storage.reset();
    Storage.put("image.jpg", {0xff, 0xd8, 0xff, 0xd9});
    jpegdec_test::reset();
  }

  void TearDown() override { EXPECT_TRUE(fakeheap::live.empty()); }
};

TEST_F(JpegDecoderPolicyTest, PsramFailureTakesFreshSafeInternalFallback) {
  fakeheap::internal.free = MemoryBudget::EPUB_INLINE_JPEG_MIN_FREE;
  fakeheap::internal.largest = MemoryBudget::EPUB_INLINE_JPEG_MIN_MAX_ALLOC;
  fakeheap::external.free = MemoryBudget::EPUB_PSRAM_RESERVE + sizeof(JPEGDEC);
  fakeheap::external.largest = sizeof(JPEGDEC);
  fakeheap::external.fail = 1;

  ImageDimensions dimensions{};
  EXPECT_TRUE(JpegToFramebufferConverter::getDimensionsStatic("image.jpg", dimensions));
  EXPECT_EQ(dimensions.width, jpegdec_test::width);
  EXPECT_EQ(dimensions.height, jpegdec_test::height);
  EXPECT_EQ(fakeheap::external.attempts, 1u);
  EXPECT_EQ(fakeheap::internal.attempts, 1u);
  EXPECT_EQ(jpegdec_test::closeCalls, 1);
  EXPECT_EQ(Storage.openHandles("image.jpg"), 0);
}

TEST_F(JpegDecoderPolicyTest, PsramFailureRejectsWhenFreshSnapshotLosesInternalFallback) {
  fakeheap::internal.free = MemoryBudget::EPUB_INLINE_JPEG_MIN_FREE;
  fakeheap::internal.largest = MemoryBudget::EPUB_INLINE_JPEG_MIN_MAX_ALLOC;
  fakeheap::external.free = MemoryBudget::EPUB_PSRAM_RESERVE + sizeof(JPEGDEC);
  fakeheap::external.largest = sizeof(JPEGDEC);
  fakeheap::external.fail = 1;
  fakeheap::reduceInternalFreeOnExternalFailure = MemoryBudget::IMAGE_DECODER_HEADROOM - 1;

  ImageDimensions dimensions{};
  EXPECT_FALSE(JpegToFramebufferConverter::getDimensionsStatic("image.jpg", dimensions));
  EXPECT_EQ(fakeheap::external.attempts, 1u);
  EXPECT_EQ(fakeheap::internal.attempts, 0u);
  EXPECT_EQ(jpegdec_test::closeCalls, 0);
  EXPECT_EQ(Storage.openHandles("image.jpg"), 0);
}

TEST_F(JpegDecoderPolicyTest, MalformedOpenClosesTakenFileExactlyOnce) {
  fakeheap::reset(false);
  jpegdec_test::openResult = 0;

  ImageDimensions dimensions{};
  EXPECT_FALSE(JpegToFramebufferConverter::getDimensionsStatic("image.jpg", dimensions));
  EXPECT_EQ(jpegdec_test::closeCalls, 1);
  EXPECT_EQ(Storage.openHandles("image.jpg"), 0);
}

TEST_F(JpegDecoderPolicyTest, DecodeFailureClosesTakenFileExactlyOnce) {
  fakeheap::reset(false);
  jpegdec_test::decodeResult = 0;
  GfxRenderer renderer;
  RenderConfig config{};
  config.maxWidth = 320;
  config.maxHeight = 240;
  JpegToFramebufferConverter converter;

  EXPECT_FALSE(converter.decodeToFramebuffer("image.jpg", renderer, config));
  EXPECT_EQ(jpegdec_test::closeCalls, 1);
  EXPECT_EQ(Storage.openHandles("image.jpg"), 0);
}

TEST_F(JpegDecoderPolicyTest, SuccessfulDecodeUsesPsramAndClosesTakenFileExactlyOnce) {
  fakeheap::internal.free = MemoryBudget::IMAGE_DECODER_HEADROOM;
  fakeheap::internal.largest = 1;
  fakeheap::external.free = MemoryBudget::EPUB_PSRAM_RESERVE + sizeof(JPEGDEC);
  fakeheap::external.largest = sizeof(JPEGDEC);
  GfxRenderer renderer;
  RenderConfig config{};
  config.maxWidth = 320;
  config.maxHeight = 240;
  JpegToFramebufferConverter converter;

  EXPECT_TRUE(converter.decodeToFramebuffer("image.jpg", renderer, config));
  EXPECT_EQ(fakeheap::external.attempts, 1u);
  EXPECT_EQ(fakeheap::internal.attempts, 0u);
  EXPECT_EQ(jpegdec_test::closeCalls, 1);
  EXPECT_EQ(Storage.openHandles("image.jpg"), 0);
}
