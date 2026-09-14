#include <gtest/gtest.h>

#include "SleepWakePolicy.h"

namespace {
constexpr size_t kExpectedFrameBytes = 52272;
}

TEST(SleepWakePolicy, Uc8279X3OrdinaryWakeWithoutFrameIsNotSeamless) {
  EXPECT_FALSE(SleepWakePolicy::shouldInitializeSeamlessly(SleepWakePolicy::Resume::SplashlessWake,
                                                           /*isUc8279X3=*/true, /*hasValidFrame=*/false));
}

TEST(SleepWakePolicy, Uc8279X3QuickResumeWithValidFrameIsSeamless) {
  const bool validFrame =
      SleepWakePolicy::hasValidSavedFrame(/*exists=*/true, kExpectedFrameBytes, kExpectedFrameBytes);
  EXPECT_TRUE(validFrame);
  EXPECT_TRUE(SleepWakePolicy::shouldInitializeSeamlessly(SleepWakePolicy::Resume::SplashlessWake,
                                                          /*isUc8279X3=*/true, validFrame));
}

TEST(SleepWakePolicy, ColdBootIsNotSeamless) {
  EXPECT_FALSE(SleepWakePolicy::shouldInitializeSeamlessly(SleepWakePolicy::Resume::Splash,
                                                           /*isUc8279X3=*/true, /*hasValidFrame=*/true));
}

TEST(SleepWakePolicy, SilentRestartRetainsExistingSeamlessBehavior) {
  EXPECT_TRUE(SleepWakePolicy::shouldInitializeSeamlessly(SleepWakePolicy::Resume::Silent,
                                                          /*isUc8279X3=*/true, /*hasValidFrame=*/false));
}

TEST(SleepWakePolicy, OtherProfilesRetainExistingSplashlessBehavior) {
  EXPECT_TRUE(SleepWakePolicy::shouldInitializeSeamlessly(SleepWakePolicy::Resume::SplashlessWake,
                                                          /*isUc8279X3=*/false, /*hasValidFrame=*/false));
}

TEST(SleepWakePolicy, MissingOrWrongSizedFrameIsInvalid) {
  EXPECT_FALSE(SleepWakePolicy::hasValidSavedFrame(/*exists=*/false, kExpectedFrameBytes, kExpectedFrameBytes));
  EXPECT_FALSE(SleepWakePolicy::hasValidSavedFrame(/*exists=*/true, kExpectedFrameBytes - 1, kExpectedFrameBytes));
  EXPECT_FALSE(SleepWakePolicy::hasValidSavedFrame(/*exists=*/true, kExpectedFrameBytes + 1, kExpectedFrameBytes));
}
