#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BookOrbitHomeLayout.h"
#include "BookOrbitHomeReadingData.h"
#include "BookOrbitHomeStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReadingStatsUtils.h"

struct BookReadingStats;
struct GlobalReadingStats;

// What HomeActivity hands the BookOrbit Home. The pointers must stay valid until the
// next begin().
struct BookOrbitHomeSource {
  const std::vector<RecentBook>* books = nullptr;
  const GlobalReadingStats* stats = nullptr;
  const BookReadingStats* currentBookStats = nullptr;
  float currentBookProgress = -1.0f;
  const std::string* chapterTitle = nullptr;
  size_t recentBookCount = 0;
  // This device's own count, whichever stats are shown: the snapshot's baseline is one.
  uint32_t localFinishedBooks = 0;
};

// What the BookOrbit Home's pages show. load() reads it all from the SD card, so opening
// Home never waits on the network; the pages only read it.
class BookOrbitHomeModel {
 public:
  // An OpenCatalogBook stop's arg: the snapshot's lists laid end to end.
  static constexpr int kDiscoverArgBase = BookOrbitHomeSnapshot::CONTINUE_COUNT;
  static constexpr int kRecentArgBase = kDiscoverArgBase + BookOrbitHomeSnapshot::SHELF_COUNT;

  // The rows under the current book: other recent books, then what the server says is
  // being read elsewhere. Exactly one of the two is set.
  struct Row {
    const RecentBook* local = nullptr;
    const BookOrbitHomeBook* remote = nullptr;
    int remoteArg = 0;
    float progress = -1.0f;
    bool onCard = false;
  };

  // trackedStats is SETTINGS.shouldTrackReadingStats(), read by the caller (see
  // BookOrbitHomeUi::begin()).
  void load(const BookOrbitHomeSource& source, bool trackedStats);

  // The server book behind an OpenCatalogBook stop.
  const BookOrbitHomeBook* catalogBook(int arg) const;

  BookOrbitHomeSource data;
  bool showStats = false;
  bool hasAccount = false;
  bool hasToday = false;
  ReadingStatsDateTime today;
  BookOrbitHomeReadingData reading;

  std::array<Row, BookOrbitHomeLayout::kMaxBooks - 1> rows{};
  int rowCount = 0;

  // Null without an account or before the first sync. About 900 bytes for as long as
  // Home is open: on the heap so the other themes do not pay for it, and fallible because
  // Home has a full local page to fall back on.
  std::unique_ptr<BookOrbitHomeSnapshot> snapshot;
  uint8_t discoverOnCard = 0;  // bit per shelf entry
  uint8_t recentOnCard = 0;
  // Books the Library's index counted on its last scan; -1 before the first one.
  int32_t cardBookCount = -1;
  int32_t serverBookCount = 0;  // catalog downloads recorded in BookOrbitDownloadIndex
  uint64_t cardTotalBytes = 0;  // 0 when the card did not say, or is FAT32 (see readCardSpace)
  uint64_t cardUsedBytes = 0;

 private:
  void computeRows();

  std::array<float, BookOrbitHomeLayout::kMaxBooks> bookProgress{};
};
