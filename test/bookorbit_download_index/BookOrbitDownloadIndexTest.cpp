#include <BookOrbitCredentialStore.h>
#include <BookOrbitDownloadIndex.h>
#include <HalStorage.h>
#include <gtest/gtest.h>
#include <uzlib.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr char INDEX_PATH[] = "/.crosspoint/bookorbit_downloads.bin";
constexpr char TMP_PATH[] = "/.crosspoint/bookorbit_downloads.tmp";
constexpr char BACKUP_PATH[] = "/.crosspoint/bookorbit_downloads.bak";
constexpr size_t MAX_ENTRIES = 512;  // no PSRAM on the host

// A book file whose size identifies it (the index's replacement check compares sizes).
void putBook(const std::string& path, const size_t size) { Storage.put(path, std::vector<uint8_t>(size, 'x')); }

std::string bookPath(const int i) {
  return "/Books/Series " + std::to_string(i) + "/Book " + std::to_string(i) + ".epub";
}

std::string lookup(const int64_t bookId) {
  std::string path;
  return BookOrbitDownloadIndex::lookup(bookId, path) ? path : std::string("<none>");
}

uint32_t storedRecordCount() {
  const auto* bytes = Storage.bytes(INDEX_PATH);
  if (bytes == nullptr || bytes->size() < 12) return UINT32_MAX;
  uint32_t count = 0;
  memcpy(&count, bytes->data() + 8, sizeof(count));
  return count;
}

template <typename T>
void append(std::vector<uint8_t>& out, const T& value) {
  const auto* p = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), p, p + sizeof(T));
}

std::vector<uint8_t> legacyIndex(const std::vector<std::pair<int64_t, std::string>>& entries) {
  std::vector<uint8_t> out = {'B', 'O', 'D', '1'};
  const std::string& url = BOOKORBIT_STORE.getServerUrl();
  append(out, uzlib_crc32(url.data(), static_cast<unsigned int>(url.size()), 0));
  for (const auto& [bookId, path] : entries) {
    append(out, bookId);
    append(out, static_cast<uint32_t>(Storage.bytes(path)->size()));
    append(out, static_cast<uint16_t>(path.size()));
    out.insert(out.end(), path.begin(), path.end());
  }
  return out;
}
}  // namespace

class BookOrbitDownloadIndexTest : public testing::Test {
 protected:
  void SetUp() override {
    BookOrbitDownloadIndex::unload();
    Storage.reset();
    BOOKORBIT_STORE.serverUrl = "https://books.example.test";
  }
  void TearDown() override { BookOrbitDownloadIndex::unload(); }
};

TEST_F(BookOrbitDownloadIndexTest, RecordedPathSurvivesReload) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  BookOrbitDownloadIndex::record(1, bookPath(1));
  BookOrbitDownloadIndex::record(2, bookPath(2));
  BookOrbitDownloadIndex::unload();

  EXPECT_EQ(lookup(1), bookPath(1));
  EXPECT_EQ(lookup(2), bookPath(2));
  EXPECT_EQ(lookup(3), "<none>");
  EXPECT_TRUE(BookOrbitDownloadIndex::hasPath(bookPath(2)));
  EXPECT_FALSE(BookOrbitDownloadIndex::hasPath("/Books/Other.epub"));
}

TEST_F(BookOrbitDownloadIndexTest, StaleEntryIsDroppedAndPersistedOnUnload) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  BookOrbitDownloadIndex::record(1, bookPath(1));
  BookOrbitDownloadIndex::record(2, bookPath(2));
  BookOrbitDownloadIndex::unload();

  Storage.remove(bookPath(1).c_str());
  EXPECT_EQ(lookup(1), "<none>");
  EXPECT_EQ(storedRecordCount(), 2U);  // not rewritten per lookup
  BookOrbitDownloadIndex::unload();
  EXPECT_EQ(storedRecordCount(), 1U);
  EXPECT_EQ(lookup(2), bookPath(2));
}

TEST_F(BookOrbitDownloadIndexTest, ReplacedFileIsNotReportedAsTheBook) {
  putBook(bookPath(1), 100);
  BookOrbitDownloadIndex::record(1, bookPath(1));
  putBook(bookPath(1), 150);  // another file at the same path
  EXPECT_EQ(lookup(1), "<none>");
}

TEST_F(BookOrbitDownloadIndexTest, OneEntryPerBookAndPerPath) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  BookOrbitDownloadIndex::record(1, bookPath(1));
  BookOrbitDownloadIndex::record(1, bookPath(2));  // re-downloaded elsewhere
  EXPECT_EQ(lookup(1), bookPath(2));
  EXPECT_FALSE(BookOrbitDownloadIndex::hasPath(bookPath(1)));

  BookOrbitDownloadIndex::record(7, bookPath(2));  // another book written over that file
  EXPECT_EQ(lookup(1), "<none>");
  EXPECT_EQ(lookup(7), bookPath(2));
  BookOrbitDownloadIndex::unload();
  EXPECT_EQ(storedRecordCount(), 1U);
}

TEST_F(BookOrbitDownloadIndexTest, RelocateFollowsAMovedBook) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  BookOrbitDownloadIndex::record(1, bookPath(1));
  BookOrbitDownloadIndex::record(2, bookPath(2));
  BookOrbitDownloadIndex::unload();

  const std::string moved = "/Read/Book 1.epub";
  Storage.rename(bookPath(1).c_str(), moved.c_str());
  BookOrbitDownloadIndex::relocate(bookPath(1), moved);  // unloaded: loads, saves, unloads

  EXPECT_EQ(lookup(1), moved);
  EXPECT_EQ(lookup(2), bookPath(2));
  EXPECT_FALSE(BookOrbitDownloadIndex::hasPath(bookPath(1)));
}

TEST_F(BookOrbitDownloadIndexTest, EvictsOldestPastTheCap) {
  for (size_t i = 0; i <= MAX_ENTRIES; i++) {
    putBook(bookPath(static_cast<int>(i)), 10 + i);
    BookOrbitDownloadIndex::record(static_cast<int64_t>(i), bookPath(static_cast<int>(i)));
  }
  BookOrbitDownloadIndex::unload();

  EXPECT_EQ(storedRecordCount(), MAX_ENTRIES);
  EXPECT_EQ(lookup(0), "<none>");
  EXPECT_EQ(lookup(1), bookPath(1));
  EXPECT_EQ(lookup(static_cast<int64_t>(MAX_ENTRIES)), bookPath(static_cast<int>(MAX_ENTRIES)));
}

TEST_F(BookOrbitDownloadIndexTest, ForEachExistingSkipsMissingBooks) {
  for (int i = 1; i <= 3; i++) {
    putBook(bookPath(i), 100 * i);
    BookOrbitDownloadIndex::record(i, bookPath(i));
  }
  Storage.remove(bookPath(2).c_str());

  std::vector<std::pair<int64_t, std::string>> seen;
  BookOrbitDownloadIndex::forEachExisting(
      [](const int64_t bookId, const std::string& path, void* context) {
        static_cast<std::vector<std::pair<int64_t, std::string>>*>(context)->emplace_back(bookId, path);
      },
      &seen);

  const std::vector<std::pair<int64_t, std::string>> expected = {{1, bookPath(1)}, {3, bookPath(3)}};
  EXPECT_EQ(seen, expected);
}

TEST_F(BookOrbitDownloadIndexTest, MigratesVersion1) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  Storage.put(INDEX_PATH, legacyIndex({{1, bookPath(1)}, {2, bookPath(2)}}));

  EXPECT_EQ(lookup(2), bookPath(2));
  const auto* bytes = Storage.bytes(INDEX_PATH);
  ASSERT_NE(bytes, nullptr);
  EXPECT_EQ(std::string(bytes->begin(), bytes->begin() + 4), "BOD2");
  BookOrbitDownloadIndex::unload();
  EXPECT_EQ(lookup(1), bookPath(1));
}

TEST_F(BookOrbitDownloadIndexTest, DiscardedWhenServerChanges) {
  putBook(bookPath(1), 100);
  BookOrbitDownloadIndex::record(1, bookPath(1));
  BookOrbitDownloadIndex::unload();

  BOOKORBIT_STORE.serverUrl = "https://other.example.test";
  EXPECT_EQ(lookup(1), "<none>");
}

TEST_F(BookOrbitDownloadIndexTest, FailedSaveKeepsThePreviousIndex) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  BookOrbitDownloadIndex::record(1, bookPath(1));

  Storage.failNextRenameFrom(TMP_PATH);
  BookOrbitDownloadIndex::record(2, bookPath(2));

  EXPECT_EQ(lookup(1), bookPath(1));
  EXPECT_EQ(lookup(2), "<none>");
  EXPECT_FALSE(Storage.exists(TMP_PATH));
}

TEST_F(BookOrbitDownloadIndexTest, RestoresTheBackupAfterAnInterruptedSave) {
  putBook(bookPath(1), 100);
  BookOrbitDownloadIndex::record(1, bookPath(1));
  BookOrbitDownloadIndex::unload();

  // Power lost after the index was moved to the backup, before the new file took its place.
  Storage.rename(INDEX_PATH, BACKUP_PATH);

  EXPECT_EQ(lookup(1), bookPath(1));
  EXPECT_TRUE(Storage.exists(INDEX_PATH));
}
