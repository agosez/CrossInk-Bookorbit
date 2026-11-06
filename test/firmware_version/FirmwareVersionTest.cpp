#include <gtest/gtest.h>

#include <vector>

#include "network/FirmwareVersion.h"

namespace {

using firmware_version::compare;
using firmware_version::isComparable;

// Release tags carry the fork's '+bookorbit.N'; a device names itself with that '+' turned
// into '-' (scripts/git_branch.py), and a non-default device appends its suffix.
TEST(FirmwareVersionTest, OrdersFinalReleasesByTheForkCounter) {
  EXPECT_EQ(compare("v1.6.1+bookorbit.4", "1.6.1-bookorbit.3"), 1);
  EXPECT_EQ(compare("v1.6.1+bookorbit.4", "1.6.1-bookorbit.4"), 0);
  EXPECT_EQ(compare("v1.6.1+bookorbit.4", "1.6.1-bookorbit.4-x4-pro"), 0);
  EXPECT_EQ(compare("v1.6.1+bookorbit.3", "1.6.1-bookorbit.4"), -1);
  EXPECT_EQ(compare("v1.7.0+bookorbit.1", "1.6.1-bookorbit.4"), 1);
}

TEST(FirmwareVersionTest, OffersTheNextReleaseCandidateToAFinalRelease) {
  EXPECT_EQ(compare("v1.6.1+bookorbit.5-rc1", "1.6.1-bookorbit.4"), 1);
  EXPECT_EQ(compare("v1.7.0+bookorbit.1-rc1", "1.6.1-bookorbit.4-sticky"), 1);
}

TEST(FirmwareVersionTest, OrdersCandidatesOfOneReleaseByNumber) {
  EXPECT_EQ(compare("v1.6.1+bookorbit.5-rc2", "1.6.1-bookorbit.5-rc1"), 1);
  EXPECT_EQ(compare("v1.6.1+bookorbit.5-rc1", "1.6.1-bookorbit.5-rc2"), -1);
  EXPECT_EQ(compare("v1.6.1+bookorbit.5-rc2", "1.6.1-bookorbit.5-rc2"), 0);
  EXPECT_EQ(compare("v1.6.1+bookorbit.5-rc2", "1.6.1-bookorbit.5-rc2-x4-pro"), 0);
  EXPECT_EQ(compare("v1.6.1+bookorbit.5-rc10", "1.6.1-bookorbit.5-rc9-x4-classic"), 1);
  // Upstream's own candidate tags.
  EXPECT_EQ(compare("v1.5.1-rc-3", "1.5.1-rc-2"), 1);
}

TEST(FirmwareVersionTest, PutsAFinalReleaseAfterItsCandidates) {
  EXPECT_EQ(compare("v1.6.1+bookorbit.5", "1.6.1-bookorbit.5-rc3"), 1);
  EXPECT_EQ(compare("v1.6.1+bookorbit.5-rc3", "1.6.1-bookorbit.5"), -1);
  EXPECT_EQ(compare("v1.6.1", "1.6.1-rc"), 1);
}

TEST(FirmwareVersionTest, PlacesAnUnnumberedWorkflowCandidateBeforeNumberedOnes) {
  // release_candidate.yml builds name themselves <branch>-<hash>-RC.
  EXPECT_EQ(compare("v1.6.1+bookorbit.5-rc1", "1.6.1-bookorbit.5-a2b44bd-RC"), 1);
  EXPECT_EQ(compare("v1.6.1+bookorbit.5", "1.6.1-bookorbit.5-a2b44bd-RC"), 1);
}

TEST(FirmwareVersionTest, KeepsADevBuildEqualToItsRelease) {
  EXPECT_EQ(compare("v1.6.1+bookorbit.4", "1.6.1+bookorbit.4-dev+feat-bookorbit-catalog-sort-filter"), 0);
}

TEST(FirmwareVersionTest, LeavesWorkflowCandidateTagsOutOfTheOrder) {
  EXPECT_FALSE(isComparable("rc-1.6.1-bookorbit.5-a2b44bd"));
  EXPECT_FALSE(isComparable(""));
  EXPECT_FALSE(isComparable(nullptr));
  EXPECT_TRUE(isComparable("v1.6.1+bookorbit.5-rc1"));
  EXPECT_TRUE(isComparable("1.6.1-bookorbit.5-a2b44bd-RC"));
  EXPECT_EQ(compare("rc-1.6.1-bookorbit.5-a2b44bd", "1.6.1-bookorbit.4"), 0);
  EXPECT_EQ(compare("v1.6.1+bookorbit.5", "rc-1.6.1-bookorbit.5-a2b44bd"), 0);
}

// The release-list check keeps whichever listed tag compares highest, so the order has to
// hold between any two of them, not only against the installed firmware.
TEST(FirmwareVersionTest, IsAConsistentOrderAcrossReleaseTags) {
  const std::vector<const char*> ascending = {
      "v1.6.1+bookorbit.4", "v1.6.1+bookorbit.5-rc1", "v1.6.1+bookorbit.5-rc2", "v1.6.1+bookorbit.5-rc10",
      "v1.6.1+bookorbit.5", "v1.7.0+bookorbit.1-rc1", "v1.7.0+bookorbit.1",
  };
  for (size_t i = 0; i < ascending.size(); ++i) {
    for (size_t j = 0; j < ascending.size(); ++j) {
      const int expected = i == j ? 0 : (i > j ? 1 : -1);
      EXPECT_EQ(compare(ascending[i], ascending[j]), expected) << ascending[i] << " vs " << ascending[j];
    }
  }
}

}  // namespace
