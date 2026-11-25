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
constexpr size_t MAX_ENTRIES = 2048;

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

uint32_t serverCrc() {
  const std::string& url = BOOKORBIT_STORE.getServerUrl();
  return uzlib_crc32(url.data(), static_cast<unsigned int>(url.size()), 0);
}

uint32_t fnv1a(const std::string& path) {
  uint32_t hash = 2166136261u;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return hash;
}

std::vector<uint8_t> legacyIndex(const std::vector<std::pair<int64_t, std::string>>& entries) {
  std::vector<uint8_t> out = {'B', 'O', 'D', '1'};
  append(out, serverCrc());
  for (const auto& [bookId, path] : entries) {
    append(out, bookId);
    append(out, static_cast<uint32_t>(Storage.bytes(path)->size()));
    append(out, static_cast<uint16_t>(path.size()));
    out.insert(out.end(), path.begin(), path.end());
  }
  return out;
}

// A BOD2 file as another firmware could have written it; a record's offset can be forced.
struct RawEntry {
  int64_t bookId;
  std::string path;
  uint32_t forcedOffset = 0;
};

std::vector<uint8_t> currentIndex(const std::vector<RawEntry>& entries) {
  std::vector<uint8_t> out = {'B', 'O', 'D', '2'};
  append(out, serverCrc());
  append(out, static_cast<uint32_t>(entries.size()));
  uint32_t offset = static_cast<uint32_t>(12 + entries.size() * 22);
  for (const auto& entry : entries) {
    append(out, entry.bookId);
    const auto* book = Storage.bytes(entry.path);
    append(out, static_cast<uint32_t>(book ? book->size() : 0));
    append(out, fnv1a(entry.path));
    append(out, entry.forcedOffset != 0 ? entry.forcedOffset : offset);
    append(out, static_cast<uint16_t>(entry.path.size()));
    offset += static_cast<uint32_t>(entry.path.size());
  }
  for (const auto& entry : entries) out.insert(out.end(), entry.path.begin(), entry.path.end());
  return out;
}

void recordRange(const int from, const int to) {
  std::vector<std::string> paths;
  std::vector<BookOrbitDownloadIndex::Download> downloads;
  paths.reserve(static_cast<size_t>(to - from));
  for (int i = from; i < to; i++) paths.push_back(bookPath(i));
  for (int i = from; i < to; i++) downloads.push_back({i, &paths[static_cast<size_t>(i - from)]});
  BookOrbitDownloadIndex::recordAll(downloads.data(), downloads.size());
}
}  // namespace

class BookOrbitDownloadIndexTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    BOOKORBIT_STORE.serverUrl = "https://books.example.test";
  }
};

TEST_F(BookOrbitDownloadIndexTest, RecordedPathsAreFoundAgain) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  BookOrbitDownloadIndex::record(1, bookPath(1));
  BookOrbitDownloadIndex::record(2, bookPath(2));

  EXPECT_EQ(lookup(1), bookPath(1));
  EXPECT_EQ(lookup(2), bookPath(2));
  EXPECT_EQ(lookup(3), "<none>");
  EXPECT_TRUE(BookOrbitDownloadIndex::hasPath(bookPath(2)));
  EXPECT_FALSE(BookOrbitDownloadIndex::hasPath("/Books/Other.epub"));
  EXPECT_EQ(BookOrbitDownloadIndex::recordedCount(), 2U);
}

TEST_F(BookOrbitDownloadIndexTest, StaleEntryIsDroppedRightAway) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  BookOrbitDownloadIndex::record(1, bookPath(1));
  BookOrbitDownloadIndex::record(2, bookPath(2));

  Storage.remove(bookPath(1).c_str());
  EXPECT_EQ(lookup(1), "<none>");
  EXPECT_EQ(storedRecordCount(), 1U);
  EXPECT_EQ(lookup(2), bookPath(2));
}

TEST_F(BookOrbitDownloadIndexTest, LookupAllAnswersEveryBookOfAPage) {
  for (int i = 1; i <= 5; i++) putBook(bookPath(i), 100 * i);
  recordRange(1, 6);
  Storage.remove(bookPath(3).c_str());

  const int64_t page[] = {5, 3, 9, 1, 5};
  std::vector<std::pair<size_t, std::string>> found;
  BookOrbitDownloadIndex::lookupAll(
      page, 5,
      [](const size_t i, const std::string& path, void* context) {
        static_cast<std::vector<std::pair<size_t, std::string>>*>(context)->emplace_back(i, path);
      },
      &found);

  const std::vector<std::pair<size_t, std::string>> expected = {{0, bookPath(5)}, {3, bookPath(1)}, {4, bookPath(5)}};
  EXPECT_EQ(found, expected);
  EXPECT_EQ(storedRecordCount(), 4U);  // book 3's entry went with its file, in the same call
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
  EXPECT_EQ(storedRecordCount(), 1U);
}

TEST_F(BookOrbitDownloadIndexTest, RecordAllSupersedesWithinTheBatchToo) {
  for (int i = 1; i <= 3; i++) putBook(bookPath(i), 100 * i);
  BookOrbitDownloadIndex::record(1, bookPath(1));

  const std::string p2 = bookPath(2);
  const std::string p3 = bookPath(3);
  const BookOrbitDownloadIndex::Download batch[] = {{2, &p2}, {1, &p3}, {4, &p2}};
  BookOrbitDownloadIndex::recordAll(batch, 3);

  EXPECT_EQ(lookup(1), p3);        // its old entry, on another file, is gone
  EXPECT_EQ(lookup(2), "<none>");  // book 4 took that file later in the batch
  EXPECT_EQ(lookup(4), p2);
  EXPECT_EQ(storedRecordCount(), 2U);
}

TEST_F(BookOrbitDownloadIndexTest, FindPathsMarksTheClaimedOnes) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  recordRange(1, 3);

  const std::string paths[] = {bookPath(2), "/Books/Other.epub", bookPath(1)};
  bool claimed[3] = {false, true, false};
  BookOrbitDownloadIndex::findPaths(paths, 3, claimed);
  EXPECT_TRUE(claimed[0]);
  EXPECT_FALSE(claimed[1]);
  EXPECT_TRUE(claimed[2]);

  int64_t bookId = 0;
  EXPECT_TRUE(BookOrbitDownloadIndex::bookIdForPath(bookPath(2), bookId));
  EXPECT_EQ(bookId, 2);
  EXPECT_FALSE(BookOrbitDownloadIndex::bookIdForPath("/Books/Other.epub", bookId));
}

TEST_F(BookOrbitDownloadIndexTest, RelocateFollowsAMovedBook) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  BookOrbitDownloadIndex::record(1, bookPath(1));
  BookOrbitDownloadIndex::record(2, bookPath(2));

  const std::string moved = "/Read/Book 1.epub";
  Storage.rename(bookPath(1).c_str(), moved.c_str());
  BookOrbitDownloadIndex::relocate(bookPath(1), moved);

  EXPECT_EQ(lookup(1), moved);
  EXPECT_EQ(lookup(2), bookPath(2));
  EXPECT_FALSE(BookOrbitDownloadIndex::hasPath(bookPath(1)));
}

TEST_F(BookOrbitDownloadIndexTest, EvictsOldestPastTheCap) {
  for (size_t i = 0; i <= MAX_ENTRIES; i++) putBook(bookPath(static_cast<int>(i)), 10 + i);
  for (int from = 0; from < static_cast<int>(MAX_ENTRIES); from += 64) recordRange(from, from + 64);
  EXPECT_EQ(storedRecordCount(), MAX_ENTRIES);
  BookOrbitDownloadIndex::record(static_cast<int64_t>(MAX_ENTRIES), bookPath(static_cast<int>(MAX_ENTRIES)));

  EXPECT_EQ(storedRecordCount(), MAX_ENTRIES);
  EXPECT_EQ(lookup(0), "<none>");
  EXPECT_EQ(lookup(1), bookPath(1));
  EXPECT_EQ(lookup(static_cast<int64_t>(MAX_ENTRIES)), bookPath(static_cast<int>(MAX_ENTRIES)));
}

TEST_F(BookOrbitDownloadIndexTest, KeepsTheNewestOfACardHoldingMoreThanTheCap) {
  std::vector<RawEntry> entries;
  for (size_t i = 0; i < MAX_ENTRIES + 2; i++) {
    putBook(bookPath(static_cast<int>(i)), 10 + i);
    entries.push_back({static_cast<int64_t>(i), bookPath(static_cast<int>(i))});
  }
  Storage.put(INDEX_PATH, currentIndex(entries));

  EXPECT_EQ(BookOrbitDownloadIndex::recordedCount(), MAX_ENTRIES);
  EXPECT_EQ(lookup(1), "<none>");
  EXPECT_EQ(lookup(2), bookPath(2));
  putBook("/new.epub", 7);
  BookOrbitDownloadIndex::record(99999, "/new.epub");
  EXPECT_EQ(storedRecordCount(), MAX_ENTRIES);
  EXPECT_EQ(lookup(2), "<none>");  // the oldest kept one made room
  EXPECT_EQ(lookup(3), bookPath(3));
}

TEST_F(BookOrbitDownloadIndexTest, SkipsARecordPointingOutsideTheFile) {
  putBook(bookPath(1), 100);
  putBook(bookPath(2), 200);
  Storage.put(INDEX_PATH, currentIndex({{1, bookPath(1), /*forcedOffset=*/60000}, {2, bookPath(2)}}));

  EXPECT_EQ(lookup(1), "<none>");
  EXPECT_EQ(lookup(2), bookPath(2));
  putBook(bookPath(3), 300);
  BookOrbitDownloadIndex::record(3, bookPath(3));
  EXPECT_EQ(storedRecordCount(), 2U);  // the rewrite left the unreadable record out
  EXPECT_EQ(lookup(2), bookPath(2));
  EXPECT_EQ(lookup(3), bookPath(3));
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
  EXPECT_EQ(storedRecordCount(), 2U);
  EXPECT_EQ(lookup(1), bookPath(1));
}

TEST_F(BookOrbitDownloadIndexTest, DiscardedWhenServerChanges) {
  putBook(bookPath(1), 100);
  BookOrbitDownloadIndex::record(1, bookPath(1));

  BOOKORBIT_STORE.serverUrl = "https://other.example.test";
  EXPECT_EQ(lookup(1), "<none>");
  EXPECT_EQ(BookOrbitDownloadIndex::recordedCount(), 0U);
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

  // Power lost after the index was moved to the backup, before the new file took its place.
  Storage.rename(INDEX_PATH, BACKUP_PATH);

  EXPECT_EQ(lookup(1), bookPath(1));
  EXPECT_TRUE(Storage.exists(INDEX_PATH));
}
