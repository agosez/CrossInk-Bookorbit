#include <gtest/gtest.h>

#include "network/FirmwareVersion.h"

namespace {

using firmware_version::compare;

// Release tags carry the fork's '+bookorbit.N'; a device names itself with that '+' turned
// into '-' (scripts/git_branch.py), and a non-default device appends its suffix.
TEST(FirmwareVersionTest, OrdersFinalReleasesByTheForkCounter) {
  EXPECT_EQ(compare("v1.6.1+bookorbit.4", "1.6.1-bookorbit.3"), 1);
  EXPECT_EQ(compare("v1.6.1+bookorbit.4", "1.6.1-bookorbit.4"), 0);
  EXPECT_EQ(compare("v1.6.1+bookorbit.4", "1.6.1-bookorbit.4-x4-pro"), 0);
  EXPECT_EQ(compare("v1.6.1+bookorbit.3", "1.6.1-bookorbit.4"), -1);
  EXPECT_EQ(compare("v1.7.0+bookorbit.1", "1.6.1-bookorbit.4"), 1);
}

TEST(FirmwareVersionTest, PutsAFinalReleaseAfterItsCandidates) {
  EXPECT_EQ(compare("v1.6.1+bookorbit.5", "1.6.1-bookorbit.5-rc3"), 1);
  EXPECT_EQ(compare("v1.6.1", "1.6.1-rc"), 1);
}

TEST(FirmwareVersionTest, KeepsADevBuildEqualToItsRelease) {
  EXPECT_EQ(compare("v1.6.1+bookorbit.4", "1.6.1+bookorbit.4-dev+feat-bookorbit-catalog-sort-filter"), 0);
}

TEST(FirmwareVersionTest, DoesNotOrderTagsThatAreNotVersions) {
  EXPECT_EQ(compare("rc-1.6.1-bookorbit.5-a2b44bd", "1.6.1-bookorbit.4"), 0);
  EXPECT_EQ(compare("v1.6.1+bookorbit.5", "rc-1.6.1-bookorbit.5-a2b44bd"), 0);
  EXPECT_EQ(compare("", "1.6.1-bookorbit.4"), 0);
  EXPECT_EQ(compare(nullptr, "1.6.1-bookorbit.4"), 0);
}

}  // namespace
