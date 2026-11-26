#pragma once

#include <HalStorage.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "ReadingStatsUtils.h"

// Reading time per calendar day over the last year, in /.crosspoint/daily_minutes.bin
// (layout in docs/file-formats.md). The BookOrbit Home's streak, week, year and moments
// all read from here. Each day also keeps its pending time: the part of it the BookOrbit
// Home snapshot does not hold yet, which Home adds to the server's own count.
//
// global_stats.bin could not carry this: its history and time buckets are only recorded
// on boards with a clock chip, because getCurrentLocalReadingStatsDateTime() ignores the
// system clock, and the file doubles as the nearby stats sync's payload. This store also
// runs on the system clock a board without a chip sets over Wi-Fi: its drift is harmless
// at the scale of a day. Single entries are read and written in place, so no call holds
// the year in RAM.
namespace ReadingDailyMinutes {

constexpr size_t DAYS = 371;  // 53 weeks: the Home heatmap's 52 and a little margin

// Adds a span of reading to the days it covers, splitting it at midnight, and to their
// pending time. Without a known start, the span is taken to have just ended.
void record(const ReadingStatsDateTime* localStart, uint32_t seconds);

// Adds a committed reading session, taken to have just ended, with the threshold
// commitReadingSession() applies to reading time. Call it next to the reader's real commit
// only: saveReadingSessionCheckpoint() commits previews of the same session, which would
// count it again at every checkpoint.
void recordSession(uint32_t seconds);

// Forgets every recorded day. Returns false when the file could not be removed.
bool reset();

// Keeps the file open across lookups, for callers walking many days.
class Reader {
 public:
  Reader();
  ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Minutes read on a day, 0 when the day is unknown or out of the file's window.
  uint16_t minutes(uint32_t dayIndex);
  // The part of minutes() the BookOrbit Home snapshot does not hold yet.
  uint16_t pendingMinutes(uint32_t dayIndex);
  // Seconds read per ReadingTimeBucket since the file was created.
  const std::array<uint32_t, READING_TIME_BUCKET_COUNT>& timeOfDaySeconds() const { return buckets; }

 private:
  HalFile file;
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> buckets{};
  uint32_t lastDay = 0;
  bool valid = false;
};

// Resets every day's pending time when a new BookOrbit Home snapshot arrives; add() then
// puts back the reading that snapshot cannot hold. Days out of the window are skipped.
class PendingRebuild {
 public:
  PendingRebuild();
  ~PendingRebuild();
  PendingRebuild(const PendingRebuild&) = delete;
  PendingRebuild& operator=(const PendingRebuild&) = delete;

  // Pass a day's whole total at once: the file keeps tens of seconds.
  void add(uint32_t dayIndex, uint32_t seconds);

 private:
  HalFile file;
  uint32_t lastDay = 0;
  bool valid = false;
};

}  // namespace ReadingDailyMinutes
