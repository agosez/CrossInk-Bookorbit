#include <gtest/gtest.h>

#include "DownloadFileSwap.h"
class DownloadSwap : public ::testing::Test {
  void SetUp() override {
    Storage.reset();
    Storage.put("/book", {1});
    Storage.put("/book.part", {2});
  }
};
TEST_F(DownloadSwap, PublishesAndRemovesBackup) {
  ASSERT_TRUE(DownloadFileSwap::publish("/book"));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{2});
  EXPECT_FALSE(Storage.exists("/book.old"));
}
TEST_F(DownloadSwap, FailurePreservingOldLeavesOriginal) {
  Storage.failRenameFrom = "/book";
  EXPECT_FALSE(DownloadFileSwap::publish("/book"));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{1});
}
TEST_F(DownloadSwap, FailurePublishingRestoresOld) {
  Storage.failRenameFrom = "/book.part";
  EXPECT_FALSE(DownloadFileSwap::publish("/book"));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{1});
}
TEST_F(DownloadSwap, RestartBeforePublishRestoresBackup) {
  ASSERT_TRUE(Storage.rename("/book", "/book.old"));
  ASSERT_TRUE(DownloadFileSwap::recover("/book"));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{1});
}
TEST_F(DownloadSwap, FailedRecoveryKeepsBackupForRetry) {
  ASSERT_TRUE(Storage.rename("/book", "/book.old"));
  Storage.failAllRenames = true;
  EXPECT_FALSE(DownloadFileSwap::recover("/book"));
  EXPECT_EQ(Storage.bytes("/book.old"), std::vector<uint8_t>{1});
  Storage.failAllRenames = false;
  ASSERT_TRUE(DownloadFileSwap::recover("/book"));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{1});
}
TEST_F(DownloadSwap, CleanupFailureKeepsCommittedBookAndRetries) {
  Storage.failRemovePath = "/book.old";
  ASSERT_TRUE(DownloadFileSwap::publish("/book"));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{2});
  EXPECT_FALSE(DownloadFileSwap::recover("/book"));
  Storage.failRemovePath.clear();
  EXPECT_TRUE(DownloadFileSwap::recover("/book"));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{2});
}
TEST_F(DownloadSwap, KeepReplacedLeavesOldCopyForCaller) {
  ASSERT_TRUE(DownloadFileSwap::publish("/book", /*requireBackupCleanup=*/false, /*keepReplaced=*/true));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{2});
  EXPECT_EQ(Storage.bytes("/book.old"), std::vector<uint8_t>{1});
  // Left behind by a crash before the caller removed it, the next staged download drops it.
  EXPECT_TRUE(DownloadFileSwap::recover("/book"));
  EXPECT_FALSE(Storage.exists("/book.old"));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{2});
}
TEST_F(DownloadSwap, KeepReplacedWithNothingReplaced) {
  Storage.remove("/book");
  ASSERT_TRUE(DownloadFileSwap::publish("/book", /*requireBackupCleanup=*/false, /*keepReplaced=*/true));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{2});
  EXPECT_FALSE(Storage.exists("/book.old"));
}
TEST_F(DownloadSwap, NewDownloadNeedsNoBackup) {
  Storage.remove("/book");
  ASSERT_TRUE(DownloadFileSwap::publish("/book"));
  EXPECT_EQ(Storage.bytes("/book"), std::vector<uint8_t>{2});
}
