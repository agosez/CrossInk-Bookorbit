#include <gtest/gtest.h>

#include "FrontlightSchedule.h"

namespace {
constexpr uint16_t timeOfDay(const uint8_t hour, const uint8_t minute = 0) {
  return static_cast<uint16_t>(hour * 60 + minute);
}
}  // namespace

TEST(FrontlightSchedule, DisabledOrIncompleteWindowIsInactive) {
  EXPECT_FALSE(FrontlightSchedule::hasCompleteWindow(false, timeOfDay(18), timeOfDay(7)));
  EXPECT_FALSE(FrontlightSchedule::hasCompleteWindow(true, FrontlightSchedule::kUnsetTimeOfDay, timeOfDay(7)));
  EXPECT_FALSE(FrontlightSchedule::hasCompleteWindow(true, timeOfDay(18), FrontlightSchedule::kUnsetTimeOfDay));
}

TEST(FrontlightSchedule, RestoreOnWakeFallsThroughToScheduleOnlyWhenPreviouslyOff) {
  EXPECT_FALSE(FrontlightSchedule::shouldApplyOnWakeSchedule(false, true, true));
  EXPECT_TRUE(FrontlightSchedule::shouldApplyOnWakeSchedule(false, true, false));
  EXPECT_TRUE(FrontlightSchedule::shouldApplyOnWakeSchedule(false, false, true));
  EXPECT_FALSE(FrontlightSchedule::shouldApplyOnWakeSchedule(true, false, false));
}

TEST(FrontlightSchedule, NetworkRestartFollowsWakePolicy) {
  EXPECT_TRUE(FrontlightSchedule::shouldPreserveLightAcrossRestart(/*isSilentReboot=*/true,
                                                                   /*followsWakeLightPolicy=*/false));
  EXPECT_FALSE(FrontlightSchedule::shouldPreserveLightAcrossRestart(/*isSilentReboot=*/true,
                                                                    /*followsWakeLightPolicy=*/true));
  EXPECT_FALSE(FrontlightSchedule::shouldRestoreLightOnStart(
      /*preserveLightAcrossRestart=*/false, /*restoreOnWake=*/false, /*wasLightOnBeforeSleep=*/true));
  EXPECT_TRUE(FrontlightSchedule::shouldRestoreLightOnStart(
      /*preserveLightAcrossRestart=*/false, /*restoreOnWake=*/true, /*wasLightOnBeforeSleep=*/true));
  EXPECT_TRUE(FrontlightSchedule::shouldApplyOnWakeSchedule(
      /*preserveLightAcrossRestart=*/false, /*restoreOnWake=*/false, /*wasLightOnBeforeSleep=*/true));
}

TEST(FrontlightSchedule, SameEndpointIsAnEmptyWindow) {
  EXPECT_FALSE(FrontlightSchedule::hasCompleteWindow(true, timeOfDay(18), timeOfDay(18)));
  EXPECT_FALSE(FrontlightSchedule::containsTimeOfDay(timeOfDay(18), timeOfDay(18), timeOfDay(18)));
}

TEST(FrontlightSchedule, NormalWindowIncludesStartAndExcludesEnd) {
  EXPECT_TRUE(FrontlightSchedule::containsTimeOfDay(timeOfDay(7), timeOfDay(22), timeOfDay(7)));
  EXPECT_TRUE(FrontlightSchedule::containsTimeOfDay(timeOfDay(7), timeOfDay(22), timeOfDay(21, 59)));
  EXPECT_FALSE(FrontlightSchedule::containsTimeOfDay(timeOfDay(7), timeOfDay(22), timeOfDay(22)));
}

TEST(FrontlightSchedule, OvernightWindowWrapsMidnight) {
  EXPECT_TRUE(FrontlightSchedule::containsTimeOfDay(timeOfDay(21), timeOfDay(7), timeOfDay(23)));
  EXPECT_TRUE(FrontlightSchedule::containsTimeOfDay(timeOfDay(21), timeOfDay(7), timeOfDay(6, 59)));
  EXPECT_FALSE(FrontlightSchedule::containsTimeOfDay(timeOfDay(21), timeOfDay(7), timeOfDay(12)));
}

TEST(FrontlightSchedule, LocalTimeOfDayAppliesQuarterHourOffset) {
  EXPECT_EQ(FrontlightSchedule::localTimeOfDay(23, 45, 52), timeOfDay(0, 45));  // UTC+1
  EXPECT_EQ(FrontlightSchedule::localTimeOfDay(0, 15, 44), timeOfDay(23, 15));  // UTC-1
}

TEST(FrontlightSchedule, UnsetTimeStartsAtNoonAndRoundTripsTwelveHourValues) {
  const auto unset = FrontlightSchedule::timeOfDayFromMinutes(FrontlightSchedule::kUnsetTimeOfDay);
  EXPECT_EQ(unset.hour12, 12);
  EXPECT_EQ(unset.minute, 0);
  EXPECT_TRUE(unset.isPm);

  const auto midnight = FrontlightSchedule::timeOfDayFromMinutes(0);
  EXPECT_EQ(midnight.hour12, 12);
  EXPECT_FALSE(midnight.isPm);
  EXPECT_EQ(FrontlightSchedule::minutesFromTimeOfDay(midnight.hour12, midnight.minute, midnight.isPm), 0);

  const auto afternoon = FrontlightSchedule::timeOfDayFromMinutes(timeOfDay(16, 43));
  EXPECT_EQ(afternoon.hour12, 4);
  EXPECT_EQ(afternoon.minute, 43);
  EXPECT_TRUE(afternoon.isPm);
  EXPECT_EQ(FrontlightSchedule::minutesFromTimeOfDay(afternoon.hour12, afternoon.minute, afternoon.isPm),
            timeOfDay(16, 43));
}
