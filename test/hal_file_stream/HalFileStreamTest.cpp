#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>

namespace {
size_t bulkCalls, byteCalls, accepted;
const void* lastBuffer;
size_t lastSize;
}  // namespace

// Exercise the real HalFile header/virtual dispatch. Replace only its storage
// backend, so this fails if Print& silently falls back to byte-wise writes.
HalFile::HalFile() = default;
HalFile::~HalFile() = default;
void HalFile::ImplDeleter::operator()(Impl*) const {}
size_t HalFile::write(const void* data, size_t size) {
  ++bulkCalls;
  lastBuffer = data;
  lastSize = size;
  return std::min(size, accepted);
}
size_t HalFile::write(uint8_t) {
  ++byteCalls;
  return accepted != 0;
}

class HalFileStreamTest : public testing::Test {
  void SetUp() override {
    bulkCalls = byteCalls = lastSize = 0;
    lastBuffer = nullptr;
    accepted = std::numeric_limits<size_t>::max();
  }
};
TEST_F(HalFileStreamTest, PrintReferenceForwardsOneChunkToStorage) {
  HalFile file;
  Print& stream = file;
  std::array<uint8_t, 4096> bytes{};
  EXPECT_EQ(stream.write(bytes.data(), bytes.size()), bytes.size());
  EXPECT_EQ(bulkCalls, 1u);
  EXPECT_EQ(byteCalls, 0u);
  EXPECT_EQ(lastBuffer, bytes.data());
  EXPECT_EQ(lastSize, bytes.size());
}
TEST_F(HalFileStreamTest, ShortAndFailedWritesReachCallerWithoutByteRetry) {
  HalFile file;
  Print& stream = file;
  uint8_t bytes[32]{};
  accepted = 7;
  EXPECT_EQ(stream.write(bytes, sizeof(bytes)), 7u);
  accepted = 0;
  EXPECT_EQ(stream.write(bytes, sizeof(bytes)), 0u);
  EXPECT_EQ(bulkCalls, 2u);
  EXPECT_EQ(byteCalls, 0u);
}
TEST_F(HalFileStreamTest, DirectVoidAndByteWritesRemainAvailable) {
  HalFile file;
  const char bytes[] = "data";
  EXPECT_EQ(file.write(static_cast<const void*>(bytes), 4), 4u);
  EXPECT_EQ(file.write(uint8_t{1}), 1u);
  EXPECT_EQ(bulkCalls, 1u);
  EXPECT_EQ(byteCalls, 1u);
}
