#pragma once

#include <array>
#include <cstdint>

#include "activities/reader/ReadingStatsUtils.h"

struct BookOrbitHomeSnapshot;
struct BookOrbitHomeSource;

// The reading numbers the BookOrbit Home's tiles show: streaks, the week, the year's
// heatmap, when the current book should be finished. Worked out once per begin() from
// this device's minutes per day and, once synced, the server's.
struct BookOrbitHomeReadingData {
  static constexpr int YEAR_WEEKS = 52;

  // snapshot is null without an account or before the first sync; today is only read
  // when hasToday is set.
  void compute(const BookOrbitHomeSource& data, const BookOrbitHomeSnapshot* snapshot, bool showStats, bool hasToday,
               const ReadingStatsDateTime& today);

  int32_t goalCompleted = 0;
  uint16_t streak = 0;
  uint16_t longestStreak = 0;
  uint8_t lastSevenDays = 0;  // bit 0 is six days ago, bit 6 is today
  std::array<uint32_t, 7> weekMinutes{};
  uint32_t weekTotal = 0;
  bool hasForecast = false;
  uint32_t estimatedSecondsLeft = 0;
  ReadingStatsDate finishDate;
  uint32_t dailyPaceMinutes = 0;
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> weekdayMinutes{};
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};

  // Two bits per day over the heatmap's window: unread, a little, some, a lot.
  std::array<uint8_t, (YEAR_WEEKS * 7 + 3) / 4> yearLevels{};
  uint32_t yearFirstDay = 0;
  uint16_t yearCellCount = 0;
  uint16_t yearDaysRead = 0;
  uint32_t yearMinutes = 0;
  uint32_t yearBestMonthMinutes = 0;
};
