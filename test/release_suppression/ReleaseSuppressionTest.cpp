#include <gtest/gtest.h>

#include "util/ReleaseSuppression.h"

namespace {

TEST(ReleaseSuppressionTest, SuppressesOnlyTheMatchingBackReleaseFrame) {
  ReleaseSuppression suppression;
  suppression.suppressBack();

  suppression.expireAfterReleaseFrame({.backReleased = true});
  EXPECT_TRUE(suppression.consumeBackRelease());
  EXPECT_FALSE(suppression.consumeBackRelease());
}

TEST(ReleaseSuppressionTest, StaleBackSuppressionExpiresWithoutAConsumer) {
  ReleaseSuppression suppression;
  suppression.suppressBack();

  suppression.expireAfterReleaseFrame({.backReleased = true});
  suppression.expireAfterReleaseFrame({});

  EXPECT_FALSE(suppression.consumeBackRelease());
}

TEST(ReleaseSuppressionTest, ConfirmSuppressionCoversPhysicalConfirmAndPowerFallback) {
  ReleaseSuppression suppression;
  suppression.suppressConfirm();
  suppression.suppressPowerConfirm();

  suppression.expireAfterReleaseFrame({.powerReleased = true});
  EXPECT_TRUE(suppression.consumeConfirmRelease());
  EXPECT_FALSE(suppression.consumePowerConfirmRelease());
}

TEST(ReleaseSuppressionTest, StaleConfirmSuppressionExpiresAfterEitherSourceIsIdle) {
  ReleaseSuppression suppression;
  suppression.suppressConfirm();

  suppression.expireAfterReleaseFrame({.confirmReleased = true});
  suppression.expireAfterReleaseFrame({});

  EXPECT_FALSE(suppression.consumeConfirmRelease());
}

TEST(ReleaseSuppressionTest, PowerSuppressionRemainsDuringReleaseAndExpiresAfterward) {
  ReleaseSuppression suppression;
  suppression.suppressPower();

  suppression.expireAfterReleaseFrame({.powerReleased = true});
  EXPECT_TRUE(suppression.isPowerReleaseSuppressed());
  EXPECT_TRUE(suppression.consumePowerRelease());

  suppression.suppressPower();
  suppression.expireAfterReleaseFrame({});
  EXPECT_FALSE(suppression.isPowerReleaseSuppressed());
}

TEST(ReleaseSuppressionTest, PowerConfirmSuppressionExpiresWithoutAConsumer) {
  ReleaseSuppression suppression;
  suppression.suppressPowerConfirm();

  suppression.expireAfterReleaseFrame({.powerReleased = true});
  suppression.expireAfterReleaseFrame({});

  EXPECT_FALSE(suppression.consumePowerConfirmRelease());
}

}  // namespace
