#include "BookOrbitHomeReadingData.h"

#include <algorithm>

#include "BookOrbitHomeModel.h"
#include "BookOrbitHomeStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingDailyMinutes.h"

namespace {
constexpr uint16_t kYearLevelSomeMinutes = 20;
constexpr uint16_t kYearLevelLotMinutes = 60;
constexpr uint32_t kForecastPaceDays = 14;
constexpr uint32_t kForecastMinPaceMinutes = 5;
}  // namespace

void BookOrbitHomeReadingData::compute(const BookOrbitHomeSource& data, const BookOrbitHomeSnapshot* snapshot,
                                       const bool showStats, const bool hasToday, const ReadingStatsDateTime& today) {
  streak = 0;
  lastSevenDays = 0;
  weekMinutes.fill(0);
  weekTotal = 0;
  yearLevels.fill(0);
  yearCellCount = 0;
  yearDaysRead = 0;
  yearMinutes = 0;
  yearBestMonthMinutes = 0;
  hasForecast = false;
  longestStreak = data.stats != nullptr ? data.stats->displayLongestReadingStreak() : 0;
  weekdayMinutes.fill(0);
  timeOfDaySeconds.fill(0);
  if (data.stats != nullptr) timeOfDaySeconds = data.stats->timeOfDaySeconds;

  goalCompleted = 0;
  if (snapshot) {
    goalCompleted = snapshot->goalCompleted;
    // Books finished here since the fetch count until the next fetch brings the server's.
    if (hasToday && today.date.year == snapshot->goalYear && data.localFinishedBooks > snapshot->localFinishedAtFetch) {
      goalCompleted += static_cast<int32_t>(data.localFinishedBooks - snapshot->localFinishedAtFetch);
    }
  }

  if (!showStats || data.stats == nullptr || !hasToday) return;
  const GlobalReadingStats& stats = *data.stats;

  // The window starts on a Monday, so the columns are calendar weeks and the last one
  // holds today.
  const uint32_t todayIndex = readingStatsDayIndex(today.date);
  const uint32_t daysIntoWeek = readingStatsDayOfWeekIndex(today.date);
  yearFirstDay = todayIndex - daysIntoWeek - (YEAR_WEEKS - 1) * 7u;
  yearCellCount = static_cast<uint16_t>(todayIndex - yearFirstDay + 1);

  ReadingDailyMinutes::Reader daily;
  BookOrbitHomeStore::YearReader server;
  // Boards with a clock chip also fill these from before the daily file existed.
  for (size_t i = 0; i < timeOfDaySeconds.size(); i++) {
    timeOfDaySeconds[i] = std::max(timeOfDaySeconds[i], daily.timeOfDaySeconds()[i]);
  }

  uint32_t monthKey = 0;
  uint32_t monthMinutes = 0;
  bool monthIsWhole = false;  // the window's first month is cut short: it cannot be the best one
  uint32_t recentMinutes = 0;
  uint16_t run = 0;
  for (uint16_t cell = 0; cell < yearCellCount; cell++) {
    const uint32_t day = yearFirstDay + cell;
    // The server's minutes hold every device's reading up to the fetch; this device adds
    // what the snapshot cannot hold yet. Without a snapshot, local reading is all there is.
    const uint16_t minutes =
        snapshot ? static_cast<uint16_t>(std::min<uint32_t>(
                       UINT16_MAX, static_cast<uint32_t>(server.minutes(day)) + daily.pendingMinutes(day)))
                 : daily.minutes(day);
    const bool read = minutes > 0 || readingHistoryHasDay(stats.readingHistoryAnchorDay, stats.readingHistoryBits, day);

    // Days only known from the history bits were read for an unknown time.
    uint8_t level = 0;
    if (minutes >= kYearLevelLotMinutes) {
      level = 3;
    } else if (minutes >= kYearLevelSomeMinutes) {
      level = 2;
    } else if (read) {
      level = 1;
    }
    yearLevels[cell / 4] |= static_cast<uint8_t>(level << ((cell % 4) * 2));
    if (read) yearDaysRead++;
    yearMinutes += minutes;

    const uint32_t weekday = (daysIntoWeek + 7u - (todayIndex - day) % 7u) % 7u;
    weekdayMinutes[weekday] += minutes;

    // A streak survives today until today is over.
    if (read) {
      run++;
      longestStreak = std::max(longestStreak, run);
    } else if (day != todayIndex) {
      run = 0;
    }

    ReadingStatsDate date;
    if (readingStatsDateFromDayIndex(day, date)) {
      const uint32_t key = date.year * 12u + date.month;
      if (key != monthKey) {
        if (monthIsWhole) yearBestMonthMinutes = std::max(yearBestMonthMinutes, monthMinutes);
        monthIsWhole = monthKey != 0 || date.day == 1;
        monthKey = key;
        monthMinutes = 0;
      }
      monthMinutes += minutes;
    }

    if (day + 7u > todayIndex) {
      const size_t slot = day + 6u - todayIndex;
      weekMinutes[slot] = minutes;
      weekTotal += minutes;
      if (read) lastSevenDays |= static_cast<uint8_t>(1u << slot);
    }
    if (day + kForecastPaceDays > todayIndex) recentMinutes += minutes;
  }
  if (monthIsWhole) yearBestMonthMinutes = std::max(yearBestMonthMinutes, monthMinutes);
  streak = run;
  if (snapshot) {
    longestStreak = std::max(longestStreak, static_cast<uint16_t>(snapshot->longestStreak));
    // What the server counted is only still true on the day it was counted.
    if (snapshot->fetchDay == todayIndex) {
      streak = std::max(streak, static_cast<uint16_t>(snapshot->currentStreak));
      lastSevenDays |= snapshot->lastSevenDays;
    }
  }

  // Same forecast as BookOrbit's plugin: the last two weeks' pace, at least five minutes
  // a day, so a book read in one sitting does not forecast as ending today forever.
  if (data.currentBookStats != nullptr && !data.books->empty() && !data.currentBookStats->isCompleted &&
      data.currentBookStats->estimatedTimeLeft(data.currentBookProgress, estimatedSecondsLeft)) {
    dailyPaceMinutes = std::max<uint32_t>(kForecastMinPaceMinutes, recentMinutes / kForecastPaceDays);
    const uint32_t minutesLeft = (estimatedSecondsLeft + 30u) / 60u;
    finishDate = today.date;
    addDaysToReadingStatsDate(finishDate,
                              static_cast<int>((minutesLeft + dailyPaceMinutes - 1) / dailyPaceMinutes) - 1);
    hasForecast = finishDate.isValid();
  }
}
