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

TEST(WakePressDetector, LatchesOnceThePressHasBeenHeldLongEnough) {
  SleepWakePolicy::WakePressDetector detector(/*holdMs=*/400, /*heldAtStart=*/false);
  EXPECT_FALSE(detector.update(false, 0));
  EXPECT_FALSE(detector.update(true, 100));
  EXPECT_FALSE(detector.update(true, 499));
  EXPECT_TRUE(detector.update(true, 500));
}

TEST(WakePressDetector, ShortPressDoesNotCountWhenAHoldIsRequired) {
  SleepWakePolicy::WakePressDetector detector(/*holdMs=*/400, /*heldAtStart=*/false);
  EXPECT_FALSE(detector.update(true, 0));
  EXPECT_FALSE(detector.update(true, 300));
  EXPECT_FALSE(detector.update(false, 310));
  // A new press starts a new hold rather than resuming the old one.
  EXPECT_FALSE(detector.update(true, 320));
  EXPECT_FALSE(detector.update(true, 700));
  EXPECT_TRUE(detector.update(true, 720));
}

TEST(WakePressDetector, StaysLatchedAfterRelease) {
  SleepWakePolicy::WakePressDetector detector(/*holdMs=*/20, /*heldAtStart=*/false);
  EXPECT_FALSE(detector.update(true, 0));
  EXPECT_TRUE(detector.update(true, 20));
  EXPECT_TRUE(detector.update(false, 30));
}

TEST(WakePressDetector, IgnoresThePressThatPutTheDeviceToSleep) {
  SleepWakePolicy::WakePressDetector detector(/*holdMs=*/400, /*heldAtStart=*/true);
  EXPECT_FALSE(detector.update(true, 0));
  EXPECT_FALSE(detector.update(true, 5000));
  EXPECT_FALSE(detector.update(false, 5010));
  EXPECT_FALSE(detector.update(true, 5020));
  EXPECT_TRUE(detector.update(true, 5420));
}

TEST(WakePressDetector, ZeroHoldCountsTheFirstPressedSample) {
  SleepWakePolicy::WakePressDetector detector(/*holdMs=*/0, /*heldAtStart=*/false);
  EXPECT_FALSE(detector.update(false, 0));
  EXPECT_TRUE(detector.update(true, 10));
}
