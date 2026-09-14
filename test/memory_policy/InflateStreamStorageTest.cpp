#include <BuildScratch.h>
#include <InflateStream.h>
#include <PoolBudget.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <vector>

struct InflateStreamStorageTest : testing::Test {
  void SetUp() override { fakeheap::reset(); }
  void TearDown() override {
    buildscratch::reclaim();
    EXPECT_TRUE(fakeheap::live.empty());
  }
  std::vector<uint8_t> raw() {
    std::vector<uint8_t> bytes(100000);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t((i * 31 + i / 7919) & 255);
    return bytes;
  }
  std::vector<uint8_t> compress(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out(compressBound(input.size()));
    uLongf n = out.size();
    EXPECT_EQ(compress2(out.data(), &n, input.data(), input.size(), 8), Z_OK);
    out.resize(n);
    return out;
  }
  void decode(InflateStream& stream, bool chunks) {
    auto input = raw();
    auto encoded = compress(input);
    struct Source {
      std::vector<uint8_t>& bytes;
      size_t offset = 0;
    } source{encoded};
    if (chunks) {
      stream.setFill(
          [](void* ctx, const uint8_t** data) -> size_t {
            auto& s = *static_cast<Source*>(ctx);
            *data = s.bytes.data() + s.offset;
            size_t n = std::min(size_t{17}, s.bytes.size() - s.offset);
            s.offset += n;
            return n;
          },
          &source);
    } else {
      stream.setSource(encoded.data(), encoded.size());
    }
    stream.setZlibWrapped();
    std::vector<uint8_t> actual(input.size() + 1);
    size_t offset = 0;
    InflateStream::Status status;
    do {
      size_t produced = 0;
      status = stream.readAtMost(actual.data() + offset, std::min(size_t{113}, actual.size() - offset), &produced);
      ASSERT_NE(status, InflateStream::Status::Error);
      ASSERT_TRUE(produced || status == InflateStream::Status::Done);
      offset += produced;
    } while (status != InflateStream::Status::Done && offset < actual.size());
    EXPECT_EQ(status, InflateStream::Status::Done);
    actual.resize(offset);
    EXPECT_EQ(actual, input);
  }
};
TEST_F(InflateStreamStorageTest, StreamingAndOneShotExactOutput) {
  for (bool streaming : {false, true})
    for (bool chunks : {false, true}) {
      InflateStream stream;
      ASSERT_TRUE(stream.init(streaming));
      EXPECT_EQ(stream.windowPool(), streaming ? MemoryPool::Psram : MemoryPool::None);
      decode(stream, chunks);
      ASSERT_TRUE(stream.init(streaming));
      decode(stream, !chunks);
      stream.deinit();
      EXPECT_TRUE(fakeheap::live.empty());
    }
}
TEST_F(InflateStreamStorageTest, LoanAndOccupiedLoan) {
  alignas(std::max_align_t) std::array<uint8_t, 48000> scratch{};
  buildscratch::lend(scratch.data(), scratch.size());
  EXPECT_EQ(InflateStream::requiredInternalStorageSize(true), 0u);
  InflateStream first, second;
  ASSERT_TRUE(first.init(true));
  ASSERT_TRUE(first.usesBuildScratch());
  EXPECT_TRUE(fakeheap::live.empty());
  decode(first, true);
  EXPECT_GT(InflateStream::requiredInternalStorageSize(true), 0u);
  ASSERT_TRUE(second.init(true));
  EXPECT_FALSE(second.usesBuildScratch());
  decode(second, true);
  first.deinit();
  second.deinit();
  EXPECT_TRUE(buildscratch::available(scratch.size()));
}
TEST_F(InflateStreamStorageTest, ExternalFailureAllowsOnlyAdmittedInternalFallback) {
  InflateStream stream;
  fakeheap::external.fail = 1;
  ASSERT_TRUE(stream.init(true));
  EXPECT_EQ(stream.windowPool(), MemoryPool::Internal);
  decode(stream, true);
  stream.deinit();
  fakeheap::external.fail = 1;
  fakeheap::internal.free = InflateStream::requiredStorageSize(true);  // no reserve
  EXPECT_FALSE(stream.init(true));
  EXPECT_TRUE(fakeheap::live.empty());
  size_t n = 10;
  uint8_t out[10];
  EXPECT_EQ(stream.readAtMost(out, 10, &n), InflateStream::Status::Error);
  fakeheap::reset(false);
  ASSERT_TRUE(stream.init(true));
  EXPECT_EQ(stream.windowPool(), MemoryPool::Internal);
  decode(stream, false);
}
TEST_F(InflateStreamStorageTest, StateFailureAndC3WindowFailureCleanUpImmediately) {
  InflateStream stream;
  fakeheap::internal.fail = 1;
  EXPECT_FALSE(stream.init(true));
  EXPECT_TRUE(fakeheap::live.empty());
  fakeheap::reset(false);
  fakeheap::internal.largest = 20000;
  EXPECT_FALSE(stream.init(true));
  EXPECT_TRUE(fakeheap::live.empty());
  fakeheap::reset();
  ASSERT_TRUE(stream.init(false));
  decode(stream, false);
}
TEST_F(InflateStreamStorageTest, CorruptAndTruncatedStreamsFail) {
  auto encoded = compress(raw());
  for (size_t length : {size_t{1}, encoded.size() / 2, encoded.size() - 1, encoded.size()}) {
    InflateStream stream;
    ASSERT_TRUE(stream.init(true));
    auto data = encoded;
    if (length == data.size()) data.back() ^= 0xFF;
    stream.setSource(data.data(), length);
    stream.setZlibWrapped();
    std::array<uint8_t, 277> output{};
    InflateStream::Status status = InflateStream::Status::Ok;
    for (int i = 0; i < 1000 && status == InflateStream::Status::Ok; ++i) {
      size_t n;
      status = stream.readAtMost(output.data(), output.size(), &n);
    }
    EXPECT_EQ(status, InflateStream::Status::Error);
  }
}
