#include "BookOrbitHomeModel.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitDownloadIndex.h"
#include "activities/home/RecentBookProgress.h"
#include "util/CardSpace.h"

namespace {
uint8_t onCardMask(const BookOrbitHomeBook* books, const int count) {
  std::string path;
  uint8_t mask = 0;
  for (int i = 0; i < count; i++) {
    if (BookOrbitDownloadIndex::lookup(books[i].id, path)) mask |= static_cast<uint8_t>(1u << i);
  }
  return mask;
}

// Counting the used space froze Home at every start on slow FAT32 cards (#110), so only
// exFAT cards get a storage bar (see CardSpace). The card is asked once per boot; every
// download and file transfer ends in a restart, which renews it.
void readCardSpace(uint64_t& total, uint64_t& used) {
  static bool read = false;
  static uint64_t cachedTotal = 0;
  static uint64_t cachedUsed = 0;
  if (!read) {
    read = true;
    const unsigned long start = millis();
#ifndef SIMULATOR
    // The host storage shim does not expose card capacity.
    if (CardSpace::isQuickToCount()) {
      cachedTotal = Storage.totalBytes();
      cachedUsed = Storage.usedBytes();
    }
#endif
    LOG_DBG("HOME", "Card space read in %lu ms (used=%llu total=%llu)", millis() - start,
            static_cast<unsigned long long>(cachedUsed), static_cast<unsigned long long>(cachedTotal));
  }
  total = cachedTotal;
  used = cachedUsed;
}
}  // namespace

void BookOrbitHomeModel::load(const BookOrbitHomeSource& source, const bool trackedStats) {
  data = source;
  showStats = trackedStats;
  hasAccount = BOOKORBIT_STORE.hasCredentials();
  hasToday = getCurrentLocalReadingStatsDateTimeAnyClock(today);

  snapshot.reset();
  if (hasAccount) {
    snapshot = makeUniqueNoThrow<BookOrbitHomeSnapshot>();
    if (snapshot && !BookOrbitHomeStore::load(*snapshot)) snapshot.reset();
  }

  // Only the index header is read: the count as of the Library's last scan, no card walk.
  cardBookCount = -1;
  if (snapshot && Storage.exists(library::libraryIndexPath())) {
    library::LibraryIndexFile index;
    if (index.open(library::libraryIndexPath())) cardBookCount = index.bookCount();
  }
  if (snapshot) readCardSpace(cardTotalBytes, cardUsedBytes);

  bookProgress.fill(-1.0f);
  if (data.books != nullptr) {
    const size_t count = std::min<size_t>(data.books->size(), bookProgress.size());
    for (size_t i = 0; i < count; i++) {
      const RecentBook& book = (*data.books)[i];
      if (i == 0) {
        bookProgress[i] = data.currentBookProgress;
      } else {
        bookProgress[i] = FsHelpers::hasEpubExtension(book.path) ? RecentBookProgress::loadCachedEpubPercent(book)
                                                                 : RecentBookProgress::loadPercent(book);
      }
    }
  }
  computeRows();

  reading.compute(data, snapshot.get(), showStats, hasToday, today);
}

void BookOrbitHomeModel::computeRows() {
  rowCount = 0;
  rows.fill(Row{});
  const int localBooks = data.books != nullptr ? static_cast<int>(data.books->size()) : 0;
  for (int i = 1; i < localBooks && rowCount < static_cast<int>(rows.size()); i++) {
    Row& row = rows[static_cast<size_t>(rowCount++)];
    row.local = &(*data.books)[static_cast<size_t>(i)];
    row.progress = bookProgress[static_cast<size_t>(i)];
    row.onCard = true;
  }

  discoverOnCard = 0;
  recentOnCard = 0;
  if (!snapshot) return;

  const auto isRecentBook = [this, localBooks](const char* title, const std::string& path) {
    for (int i = 0; i < localBooks; i++) {
      const RecentBook& book = (*data.books)[static_cast<size_t>(i)];
      if (book.title == title || (!path.empty() && book.path == path)) return true;
    }
    return false;
  };
  std::string path;
  for (int i = 0; i < snapshot->continueCount && rowCount < static_cast<int>(rows.size()); i++) {
    const BookOrbitHomeBook& book = snapshot->continueReading[i];
    const bool onCard = BookOrbitDownloadIndex::lookup(book.id, path);
    if (isRecentBook(book.title, onCard ? path : std::string{})) continue;
    Row& row = rows[static_cast<size_t>(rowCount++)];
    row.remote = &book;
    row.remoteArg = i;
    row.progress = book.progress != BookOrbitHomeBook::NO_PROGRESS ? static_cast<float>(book.progress) : -1.0f;
    row.onCard = onCard;
  }
  discoverOnCard = onCardMask(snapshot->discover, snapshot->discoverCount);
  recentOnCard = onCardMask(snapshot->recent, snapshot->recentCount);
  serverBookCount = static_cast<int32_t>(BookOrbitDownloadIndex::recordedCount());
  BookOrbitDownloadIndex::unload();
}

const BookOrbitHomeBook* BookOrbitHomeModel::catalogBook(const int arg) const {
  if (!snapshot || arg < 0) return nullptr;
  if (arg < kDiscoverArgBase) return &snapshot->continueReading[arg];
  if (arg < kRecentArgBase) return &snapshot->discover[arg - kDiscoverArgBase];
  if (arg < kRecentArgBase + BookOrbitHomeSnapshot::SHELF_COUNT) return &snapshot->recent[arg - kRecentArgBase];
  return nullptr;
}
