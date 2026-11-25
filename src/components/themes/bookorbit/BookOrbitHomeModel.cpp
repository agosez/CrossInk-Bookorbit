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
// Every server book Home shows, asked about in one pass over the download index.
struct OnCard {
  static constexpr int CONTINUE = BookOrbitHomeSnapshot::CONTINUE_COUNT;
  static constexpr int SHELF_BOOKS = BookOrbitHomeShelf::BOOK_COUNT;
  static constexpr int BOOKS = CONTINUE + BookOrbitHomeSnapshot::SHELVES * SHELF_BOOKS;
  int64_t ids[BOOKS] = {};
  size_t count = 0;
  size_t continueCount = 0;             // the Continue Reading books come first in ids
  uint32_t found = 0;                   // bit i: ids[i] is on the card
  std::string continuePaths[CONTINUE];  // where they are, for the recent-book check

  bool has(const size_t i) const { return ((found >> i) & 1u) != 0; }
};
static_assert(OnCard::BOOKS <= 32, "one bit per book");
static_assert(OnCard::SHELF_BOOKS <= 8, "one bit per shelf entry");

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

  shelfOnCard.fill(0);
  if (!snapshot) return;

  const auto isRecentBook = [this, localBooks](const char* title, const std::string& path) {
    for (int i = 0; i < localBooks; i++) {
      const RecentBook& book = (*data.books)[static_cast<size_t>(i)];
      if (book.title == title || (!path.empty() && book.path == path)) return true;
    }
    return false;
  };
  OnCard onCard;
  const int continueCount = std::min<int>(snapshot->continueCount, OnCard::CONTINUE);
  for (int i = 0; i < continueCount; i++) onCard.ids[onCard.count++] = snapshot->continueReading[i].id;
  onCard.continueCount = onCard.count;
  std::array<size_t, BookOrbitHomeSnapshot::SHELVES> shelfStart{};
  for (int s = 0; s < BookOrbitHomeSnapshot::SHELVES; s++) {
    shelfStart[s] = onCard.count;
    const int books = std::min<int>(snapshot->shelves[s].count, OnCard::SHELF_BOOKS);
    for (int b = 0; b < books; b++) onCard.ids[onCard.count++] = snapshot->shelves[s].books[b].id;
  }
  BookOrbitDownloadIndex::lookupAll(
      onCard.ids, onCard.count,
      [](const size_t i, const std::string& path, void* context) {
        auto& on = *static_cast<OnCard*>(context);
        on.found |= 1u << i;
        if (i < on.continueCount) on.continuePaths[i] = path;
      },
      &onCard);

  for (int i = 0; i < continueCount && rowCount < static_cast<int>(rows.size()); i++) {
    const BookOrbitHomeBook& book = snapshot->continueReading[i];
    if (isRecentBook(book.title, onCard.continuePaths[i])) continue;  // empty when not on the card
    Row& row = rows[static_cast<size_t>(rowCount++)];
    row.remote = &book;
    row.remoteArg = i;
    row.progress = book.progress != BookOrbitHomeBook::NO_PROGRESS ? static_cast<float>(book.progress) : -1.0f;
    row.onCard = onCard.has(static_cast<size_t>(i));
  }
  for (int s = 0; s < BookOrbitHomeSnapshot::SHELVES; s++) {
    uint8_t mask = 0;
    const int books = std::min<int>(snapshot->shelves[s].count, OnCard::SHELF_BOOKS);
    for (int b = 0; b < books; b++) {
      if (onCard.has(shelfStart[s] + static_cast<size_t>(b))) mask |= static_cast<uint8_t>(1u << b);
    }
    shelfOnCard[s] = mask;
  }
  serverBookCount = static_cast<int32_t>(BookOrbitDownloadIndex::recordedCount());
}

const BookOrbitHomeBook* BookOrbitHomeModel::catalogBook(const int arg) const {
  if (!snapshot || arg < 0) return nullptr;
  if (arg < kShelfArgBase) return &snapshot->continueReading[arg];
  const int shelf = (arg - kShelfArgBase) / BookOrbitHomeShelf::BOOK_COUNT;
  if (shelf >= BookOrbitHomeSnapshot::SHELVES) return nullptr;
  return &snapshot->shelves[shelf].books[(arg - kShelfArgBase) % BookOrbitHomeShelf::BOOK_COUNT];
}
