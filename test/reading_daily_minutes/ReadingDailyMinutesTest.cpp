#include <HalClock.h>
#include <HalStorage.h>
#include <ReadingDailyMinutes.h>
#include <gtest/gtest.h>

namespace {
constexpr char PATH[] = "/.crosspoint/daily_minutes.bin";
constexpr size_t FILE_SIZE = 28 + ReadingDailyMinutes::DAYS * 4;  // day totals, then pending times

ReadingStatsDateTime at(const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t hour,
                        const uint8_t minute = 0) {
  ReadingStatsDateTime dateTime;
  dateTime.date.year = year;
  dateTime.date.month = month;
  dateTime.date.day = day;
  dateTime.hour = hour;
  dateTime.minute = minute;
  return dateTime;
}

uint32_t dayOf(const uint16_t year, const uint8_t month, const uint8_t day) {
  return readingStatsDayIndex(at(year, month, day, 0).date);
}

uint16_t minutesOn(const uint32_t dayIndex) {
  ReadingDailyMinutes::Reader reader;
  return reader.minutes(dayIndex);
}

uint16_t pendingOn(const uint32_t dayIndex) {
  ReadingDailyMinutes::Reader reader;
  return reader.pendingMinutes(dayIndex);
}
}  // namespace

class ReadingDailyMinutesTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    halClock.systemClockSet = true;
  }
};

TEST_F(ReadingDailyMinutesTest, RecordsMinutesOnTheDayTheyWereRead) {
  const auto start = at(2026, 10, 19, 20);
  ReadingDailyMinutes::record(&start, 600);
  ReadingDailyMinutes::record(&start, 300);

  EXPECT_EQ(minutesOn(dayOf(2026, 10, 19)), 15);
  EXPECT_EQ(minutesOn(dayOf(2026, 10, 18)), 0);
  ASSERT_NE(Storage.bytes(PATH), nullptr);
  EXPECT_EQ(Storage.bytes(PATH)->size(), FILE_SIZE);
}

TEST_F(ReadingDailyMinutesTest, ShortSpansAreNotRoundedAway) {
  const auto start = at(2026, 10, 19, 20);
  for (int i = 0; i < 6; i++) ReadingDailyMinutes::record(&start, 30);
  EXPECT_EQ(minutesOn(dayOf(2026, 10, 19)), 3);
}

TEST_F(ReadingDailyMinutesTest, SplitsASpanAtMidnight) {
  const auto start = at(2026, 10, 19, 23, 50);
  ReadingDailyMinutes::record(&start, 30 * 60);
  EXPECT_EQ(minutesOn(dayOf(2026, 10, 19)), 10);
  EXPECT_EQ(minutesOn(dayOf(2026, 10, 20)), 20);
}

TEST_F(ReadingDailyMinutesTest, CountsSecondsPerTimeOfDay) {
  const auto evening = at(2026, 10, 19, 19);
  ReadingDailyMinutes::record(&evening, 1200);
  const auto morning = at(2026, 10, 20, 8);
  ReadingDailyMinutes::record(&morning, 600);

  ReadingDailyMinutes::Reader reader;
  const auto& buckets = reader.timeOfDaySeconds();
  EXPECT_EQ(buckets[static_cast<size_t>(readingTimeBucketForHour(19))], 1200u);
  EXPECT_EQ(buckets[static_cast<size_t>(readingTimeBucketForHour(8))], 600u);
}

TEST_F(ReadingDailyMinutesTest, AWindowThatMovedOnForgetsTheDaysItLeft) {
  const auto first = at(2025, 1, 1, 10);
  ReadingDailyMinutes::record(&first, 600);
  const uint32_t firstDay = dayOf(2025, 1, 1);

  // Exactly one window later: the same slot, which must not still hold the old minutes.
  ReadingStatsDate later = first.date;
  addDaysToReadingStatsDate(later, static_cast<int>(ReadingDailyMinutes::DAYS));
  ReadingStatsDateTime next = first;
  next.date = later;
  ReadingDailyMinutes::record(&next, 60);

  EXPECT_EQ(minutesOn(firstDay), 0);
  EXPECT_EQ(minutesOn(firstDay + ReadingDailyMinutes::DAYS), 1);
}

TEST_F(ReadingDailyMinutesTest, ADayStillInTheWindowCanBeAddedLater) {
  const auto today = at(2026, 10, 19, 10);
  ReadingDailyMinutes::record(&today, 600);
  const auto earlier = at(2026, 10, 10, 10);
  ReadingDailyMinutes::record(&earlier, 300);

  EXPECT_EQ(minutesOn(dayOf(2026, 10, 19)), 10);
  EXPECT_EQ(minutesOn(dayOf(2026, 10, 10)), 5);
}

TEST_F(ReadingDailyMinutesTest, AClockCorrectedBackPastTheWindowStartsOver) {
  const auto badDate = at(2030, 1, 1, 10);
  ReadingDailyMinutes::record(&badDate, 600);

  const auto corrected = at(2026, 10, 19, 10);
  ReadingDailyMinutes::record(&corrected, 300);

  EXPECT_EQ(minutesOn(dayOf(2026, 10, 19)), 5);
  EXPECT_EQ(minutesOn(dayOf(2030, 1, 1)), 0);
}

TEST_F(ReadingDailyMinutesTest, WithoutAStartTheSpanEndsNow) {
  ReadingStatsDateTime now;
  ASSERT_TRUE(getCurrentLocalReadingStatsDateTimeAnyClock(now));
  ReadingDailyMinutes::record(nullptr, 120);
  ReadingStatsDate yesterday = now.date;
  addDaysToReadingStatsDate(yesterday, -1);
  const int today = minutesOn(readingStatsDayIndex(now.date));
  const int previous = minutesOn(readingStatsDayIndex(yesterday));
  EXPECT_EQ(today + previous, 2);
}

TEST_F(ReadingDailyMinutesTest, WithoutAnyClockNothingIsRecorded) {
  halClock.systemClockSet = false;
  ReadingDailyMinutes::record(nullptr, 600);
  EXPECT_FALSE(Storage.exists(PATH));
}

TEST_F(ReadingDailyMinutesTest, AFileOfAnotherSizeIsReplaced) {
  HalFile file = Storage.open(PATH, O_RDWR | O_CREAT);
  const uint8_t junk[16] = {};
  file.write(junk, sizeof(junk));
  file.close();

  EXPECT_EQ(minutesOn(dayOf(2026, 10, 19)), 0);
  const auto start = at(2026, 10, 19, 10);
  ReadingDailyMinutes::record(&start, 600);
  EXPECT_EQ(minutesOn(dayOf(2026, 10, 19)), 10);
  EXPECT_EQ(Storage.bytes(PATH)->size(), FILE_SIZE);
}

TEST_F(ReadingDailyMinutesTest, ResetForgetsEverything) {
  const auto start = at(2026, 10, 19, 10);
  ReadingDailyMinutes::record(&start, 600);
  EXPECT_TRUE(ReadingDailyMinutes::reset());
  EXPECT_EQ(minutesOn(dayOf(2026, 10, 19)), 0);
  EXPECT_TRUE(ReadingDailyMinutes::reset());
}

TEST_F(ReadingDailyMinutesTest, RecordedReadingIsPendingUntilARebuild) {
  const auto start = at(2026, 10, 19, 20);
  ReadingDailyMinutes::record(&start, 1800);
  EXPECT_EQ(pendingOn(dayOf(2026, 10, 19)), 30);

  {
    // A new snapshot holds all but 10 minutes of it.
    ReadingDailyMinutes::PendingRebuild rebuild;
    rebuild.add(dayOf(2026, 10, 19), 600);
  }
  EXPECT_EQ(pendingOn(dayOf(2026, 10, 19)), 10);
  EXPECT_EQ(minutesOn(dayOf(2026, 10, 19)), 30);

  ReadingDailyMinutes::record(&start, 300);
  EXPECT_EQ(pendingOn(dayOf(2026, 10, 19)), 15);
  EXPECT_EQ(minutesOn(dayOf(2026, 10, 19)), 35);
}

TEST_F(ReadingDailyMinutesTest, ARebuildClearsEveryDayItIsNotGiven) {
  const auto monday = at(2026, 10, 19, 20);
  ReadingDailyMinutes::record(&monday, 600);
  const auto tuesday = at(2026, 10, 20, 20);
  ReadingDailyMinutes::record(&tuesday, 600);

  {
    ReadingDailyMinutes::PendingRebuild rebuild;
  }

  EXPECT_EQ(pendingOn(dayOf(2026, 10, 19)), 0);
  EXPECT_EQ(pendingOn(dayOf(2026, 10, 20)), 0);
  EXPECT_EQ(minutesOn(dayOf(2026, 10, 20)), 10);
}

TEST_F(ReadingDailyMinutesTest, ARebuildSkipsDaysOutOfTheWindow) {
  const auto today = at(2026, 10, 19, 20);
  ReadingDailyMinutes::record(&today, 600);
  {
    ReadingDailyMinutes::PendingRebuild rebuild;
    rebuild.add(dayOf(2026, 10, 20), 600);                              // after the newest day written
    rebuild.add(dayOf(2026, 10, 19) - ReadingDailyMinutes::DAYS, 600);  // same slot, a window ago
  }
  EXPECT_EQ(pendingOn(dayOf(2026, 10, 19)), 0);
  EXPECT_EQ(pendingOn(dayOf(2026, 10, 20)), 0);
}

TEST_F(ReadingDailyMinutesTest, AWindowThatMovedOnForgetsPendingTimeToo) {
  const auto first = at(2025, 1, 1, 10);
  ReadingDailyMinutes::record(&first, 600);
  ReadingStatsDateTime next = first;
  addDaysToReadingStatsDate(next.date, static_cast<int>(ReadingDailyMinutes::DAYS));
  ReadingDailyMinutes::record(&next, 60);

  EXPECT_EQ(pendingOn(dayOf(2025, 1, 1) + ReadingDailyMinutes::DAYS), 1);
}

TEST_F(ReadingDailyMinutesTest, ARebuildWithoutAFileCreatesNone) {
  {
    ReadingDailyMinutes::PendingRebuild rebuild;
  }
  EXPECT_FALSE(Storage.exists(PATH));
}
