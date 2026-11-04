#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

/**
 * What the BookOrbit Home shows from the server, kept on the SD card so opening Home
 * never touches the network: a sync (behind the sleep screen, or from the Home tile)
 * writes it, and Home only reads. Layout in docs/file-formats.md.
 */
struct BookOrbitHomeBook {
  static constexpr uint8_t NO_PROGRESS = 0xFF;

  int32_t id = 0;
  uint32_t coverVersion = 0;  // hash of the server's updatedAt: a new cover gets a new file
  uint8_t progress = NO_PROGRESS;
  bool hasCover = false;
  char title[46] = {};
};

struct BookOrbitHomeSnapshot {
  static constexpr int CONTINUE_COUNT = 3;
  static constexpr int SHELF_COUNT = 4;

  uint32_t generatedAt = 0;  // Unix seconds, 0 when the clock was not set
  uint32_t fetchDay = 0;     // local day index of the fetch
  // This device's finished-book count at the fetch: the ones finished since then count
  // toward the goal until the server knows of them.
  uint32_t localFinishedAtFetch = 0;
  int32_t totalBooks = -1;
  int32_t authors = -1;
  int32_t series = -1;
  int32_t collections = -1;
  int16_t goalBooks = 0;  // 0: no yearly goal configured on the server
  int16_t goalCompleted = 0;
  int16_t goalYear = 0;
  int16_t currentStreak = 0;
  int16_t longestStreak = 0;
  uint16_t badgeProgress = 0;
  uint16_t badgeThreshold = 0;  // 0: no next badge
  uint32_t yearFirstDay = 0;    // local day index of the first entry after this struct
  uint16_t yearDayCount = 0;
  uint8_t lastSevenDays = 0;  // bit 0 is six days before fetchDay, bit 6 is fetchDay
  uint8_t continueCount = 0;
  uint8_t discoverCount = 0;
  uint8_t recentCount = 0;
  char displayName[24] = {};
  char badgeName[32] = {};
  char highlightText[180] = {};
  char highlightBook[46] = {};
  BookOrbitHomeBook continueReading[CONTINUE_COUNT];
  BookOrbitHomeBook discover[SHELF_COUNT];
  BookOrbitHomeBook recent[SHELF_COUNT];
};

namespace BookOrbitHomeStore {

using StopCheck = bool (*)();

struct CoverSizes {
  int shelfWidth = 0;
  int shelfHeight = 0;
  int rowWidth = 0;
  int rowHeight = 0;
};

// Reads the saved snapshot. False when there is none or it comes from another version.
bool load(BookOrbitHomeSnapshot& out);

// Replaces the snapshot from the server, then fetches the covers it is missing. Needs
// Wi-Fi up. shouldStop, when set, is checked between requests. The local reading the
// new snapshot cannot hold, what still waits in the books' upload queues, becomes the
// pending time of ReadingDailyMinutes.
bool refresh(StopCheck shouldStop, const CoverSizes& sizes);

// True when a fetched cover is waiting to be decoded.
bool hasPendingCovers();

// Turns fetched covers into the BMPs Home draws. The JPEG decoder needs about 53 KB of
// contiguous heap, which never exists beside Wi-Fi on the C3, so this runs with the
// radio off; a cover it has no room for stays pending for the next call.
void decodePendingCovers(StopCheck shouldStop, const CoverSizes& sizes);

// Where a book's cover is drawn from at this size.
void coverPath(const BookOrbitHomeBook& book, int width, int height, char* out, size_t outSize);

// The server's reading minutes per day, read in place.
class YearReader {
 public:
  YearReader();
  ~YearReader();
  YearReader(const YearReader&) = delete;
  YearReader& operator=(const YearReader&) = delete;

  // 0 when the day is out of the snapshot's window.
  uint16_t minutes(uint32_t dayIndex);

 private:
  HalFile file;
  uint32_t firstDay = 0;
  uint16_t dayCount = 0;
};

}  // namespace BookOrbitHomeStore
