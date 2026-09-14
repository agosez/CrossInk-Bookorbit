#include <OtaBootSwitch.h>
#include <gtest/gtest.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {
constexpr size_t sector = SPI_FLASH_SEC_SIZE;
esp_partition_t metadata{ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, 2 * sector, "otadata"};
const esp_partition_t destination0{ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x100000, "ota_0"};
const esp_partition_t destination1{ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0 + 1, 0x100000, "ota_1"};
std::array<uint8_t, 2 * sector> flash;
bool missing;
int failingRead;
bool failingErase;
bool failingWrite;
size_t reads;
std::vector<size_t> erases;
std::vector<size_t> writes;

void seedSlot(int slot, uint32_t sequence, uint32_t state = 2) {
  ota_boot::SelectEntry entry{};
  entry.ota_seq = sequence;
  entry.ota_state = state;
  entry.crc = ota_boot::computeSeqCrc(sequence);
  std::memcpy(flash.data() + slot * sector, &entry, sizeof(entry));
}

ota_boot::SelectEntry readSlot(int slot) {
  ota_boot::SelectEntry entry;
  std::memcpy(&entry, flash.data() + slot * sector, sizeof(entry));
  return entry;
}

class OtaBootSwitch : public testing::Test {
 protected:
  void SetUp() override {
    flash.fill(0xff);
    missing = failingErase = failingWrite = false;
    failingRead = -1;
    reads = 0;
    erases.clear();
    writes.clear();
    metadata.size = flash.size();
  }
  void expectNoMutation() {
    EXPECT_TRUE(erases.empty());
    EXPECT_TRUE(writes.empty());
  }
  void expectEntry(int slot, uint32_t sequence) {
    const auto entry = readSlot(slot);
    EXPECT_EQ(entry.ota_seq, sequence);
    EXPECT_EQ(entry.ota_state, ota_boot::kOtaImgNew);
    EXPECT_EQ(entry.crc, ota_boot::computeSeqCrc(sequence));
    for (auto byte : entry.seq_label) EXPECT_EQ(byte, 0xff);
    EXPECT_EQ(erases, std::vector<size_t>{slot * sector});
    EXPECT_EQ(writes, std::vector<size_t>{slot * sector});
  }
};

TEST_F(OtaBootSwitch, SequenceCrcMatchesFixedBootMetadataVectors) {
  // Fixed CRC32-LE vectors over four little-endian bytes, seeded with UINT32_MAX.
  // Keep expected values independent of computeSeqCrc: using it to build both
  // fixtures and expectations alone would hide a wrong seed or byte count.
  EXPECT_EQ(ota_boot::computeSeqCrc(0), 0xffffffffU);
  EXPECT_EQ(ota_boot::computeSeqCrc(1), 0x4743989aU);
  EXPECT_EQ(ota_boot::computeSeqCrc(2), 0x55f63774U);
  EXPECT_EQ(ota_boot::computeSeqCrc(0x12345678), 0x71d6a731U);
  EXPECT_EQ(ota_boot::computeSeqCrc(0xfffffffe), 0x99f8b879U);
}

TEST_F(OtaBootSwitch, NullDestinationDoesNotTouchFlash) {
  EXPECT_FALSE(ota_boot::switchTo(nullptr));
  EXPECT_EQ(reads, 0U);
  expectNoMutation();
}
TEST_F(OtaBootSwitch, MissingMetadataDoesNotTouchFlash) {
  missing = true;
  EXPECT_FALSE(ota_boot::switchTo(&destination0));
  expectNoMutation();
}
TEST_F(OtaBootSwitch, UndersizedMetadataDoesNotTouchFlash) {
  metadata.size = 2 * sector - 1;
  EXPECT_FALSE(ota_boot::switchTo(&destination0));
  EXPECT_EQ(reads, 0U);
  expectNoMutation();
}
TEST_F(OtaBootSwitch, EitherReadFailurePreventsErase) {
  for (int slot = 0; slot < 2; ++slot) {
    reads = 0;
    failingRead = slot;
    EXPECT_FALSE(ota_boot::switchTo(&destination0));
    expectNoMutation();
  }
}
TEST_F(OtaBootSwitch, FactoryDestinationIsRejectedBeforeErase) {
  auto factory = destination0;
  factory.subtype = 0;
  EXPECT_FALSE(ota_boot::switchTo(&factory));
  expectNoMutation();
}
TEST_F(OtaBootSwitch, ErasedMetadataStartsFirstOtaPartitionAtSequenceOne) {
  ASSERT_TRUE(ota_boot::switchTo(&destination0));
  expectEntry(0, 1);
  EXPECT_TRUE(std::all_of(flash.begin() + sector, flash.end(), [](uint8_t byte) { return byte == 0xff; }));
}
TEST_F(OtaBootSwitch, ErasedMetadataStartsSecondOtaPartitionAtSequenceTwo) {
  ASSERT_TRUE(ota_boot::switchTo(&destination1));
  expectEntry(0, 2);
}
TEST_F(OtaBootSwitch, NewerSecondSlotIsPreservedWhenFirstSlotIsReplaced) {
  seedSlot(0, 3);
  seedSlot(1, 8);
  const auto before = flash;
  ASSERT_TRUE(ota_boot::switchTo(&destination0));
  expectEntry(0, 9);
  EXPECT_TRUE(std::equal(flash.begin() + sector, flash.end(), before.begin() + sector));
}
TEST_F(OtaBootSwitch, SameDestinationStillAdvancesSequenceAndPreservesActiveSlot) {
  seedSlot(0, 7);
  seedSlot(1, 4);
  const auto before = flash;
  ASSERT_TRUE(ota_boot::switchTo(&destination0));
  expectEntry(1, 9);
  EXPECT_TRUE(std::equal(flash.begin(), flash.begin() + sector, before.begin()));
}
TEST_F(OtaBootSwitch, CorruptCrcDoesNotSupersedeValidSlot) {
  seedSlot(0, 3);
  seedSlot(1, 100);
  flash[sector + offsetof(ota_boot::SelectEntry, crc)] ^= 1;
  ASSERT_TRUE(ota_boot::switchTo(&destination1));
  expectEntry(1, 4);
}
TEST_F(OtaBootSwitch, InvalidAndAbortedImagesDoNotSupersedeValidSlot) {
  for (auto state : {ota_boot::kOtaImgInvalid, ota_boot::kOtaImgAborted}) {
    erases.clear();
    writes.clear();
    seedSlot(0, 3);
    seedSlot(1, 100, state);
    ASSERT_TRUE(ota_boot::switchTo(&destination1));
    expectEntry(1, 4);
  }
}
TEST_F(OtaBootSwitch, ErasedSequenceIsIgnoredEvenWithValidCrc) {
  seedSlot(0, 3);
  seedSlot(1, UINT32_MAX);
  ASSERT_TRUE(ota_boot::switchTo(&destination1));
  expectEntry(1, 4);
}
TEST_F(OtaBootSwitch, EraseFailurePreventsWriteAndPreservesActiveSlot) {
  seedSlot(0, 3);
  const auto before = flash;
  failingErase = true;
  EXPECT_FALSE(ota_boot::switchTo(&destination1));
  EXPECT_EQ(erases, std::vector<size_t>{sector});
  EXPECT_TRUE(writes.empty());
  EXPECT_EQ(flash, before);
}
TEST_F(OtaBootSwitch, PartialWriteFailurePreservesActiveSlot) {
  seedSlot(0, 3);
  const auto before = flash;
  failingWrite = true;
  EXPECT_FALSE(ota_boot::switchTo(&destination1));
  EXPECT_EQ(writes, std::vector<size_t>{sector});
  EXPECT_TRUE(std::equal(flash.begin(), flash.begin() + sector, before.begin()));
}
}  // namespace

const esp_partition_t* esp_partition_find_first(uint8_t type, uint8_t subtype, const char* label) {
  EXPECT_EQ(type, ESP_PARTITION_TYPE_DATA);
  EXPECT_EQ(subtype, ESP_PARTITION_SUBTYPE_DATA_OTA);
  EXPECT_EQ(label, nullptr);
  return missing ? nullptr : &metadata;
}

esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset, void* output, size_t length) {
  EXPECT_EQ(partition, &metadata);
  if (static_cast<int>(reads++) == failingRead) return -1;
  if (offset > flash.size() || length > flash.size() - offset) return -1;
  std::memcpy(output, flash.data() + offset, length);
  return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t* partition, size_t offset, size_t length) {
  EXPECT_EQ(partition, &metadata);
  EXPECT_EQ(length, sector);
  EXPECT_EQ(offset % sector, 0U);
  erases.push_back(offset);
  if (failingErase || offset > flash.size() || length > flash.size() - offset) return -1;
  std::fill_n(flash.data() + offset, length, 0xff);
  return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t* partition, size_t offset, const void* input, size_t length) {
  EXPECT_EQ(partition, &metadata);
  EXPECT_EQ(length, sizeof(ota_boot::SelectEntry));
  writes.push_back(offset);
  if (offset > flash.size() || length > flash.size() - offset) return -1;
  // Model a power/storage failure after writing only the sequence bytes.
  std::memcpy(flash.data() + offset, input, failingWrite ? 4 : length);
  return failingWrite ? -1 : ESP_OK;
}
