// Covers what BookOrbit Smart sync (and the sleep sync, which follows its rules) decides
// from the two positions and from this device's memory of its last sync of the book.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "BookOrbitSmartSync.h"

namespace {

using BookOrbitSmartSync::decide;
using BookOrbitSmartSync::Decision;
using BookOrbitSmartSync::makeMarker;
using BookOrbitSmartSync::Marker;
using BookOrbitSmartSync::Position;

constexpr int64_t kLastSync = 1'790'000'000;
constexpr int64_t kLater = kLastSync + 600;

// A marker from a release that recorded the date alone.
constexpr Marker datedOnly(const int64_t timestamp) {
  Marker marker;
  marker.timestamp = timestamp;
  return marker;
}

}  // namespace

TEST(BookOrbitSmartSync, PositionsWithinAPageAreAlreadySynced) {
  const Marker marker = makeMarker(kLastSync, {5, 12, 30});
  EXPECT_EQ(decide({5, 12, 30}, {5, 12, 30}, kLastSync, marker), Decision::AlreadySynced);
  EXPECT_EQ(decide({5, 12, 30}, {5, 11, 30}, kLater, marker), Decision::AlreadySynced);
  EXPECT_EQ(decide({5, 12, 30}, {5, 13, 30}, kLater, marker), Decision::AlreadySynced);
  // Decided before any history is looked at.
  EXPECT_EQ(decide({5, 12, 30}, {5, 13, 30}, 0, Marker{}), Decision::AlreadySynced);
}

TEST(BookOrbitSmartSync, AsksWithoutHistoryOrServerDates) {
  EXPECT_EQ(decide({5, 12, 30}, {3, 0, 20}, kLater, Marker{}), Decision::AskNoHistory);
  EXPECT_EQ(decide({5, 12, 30}, {3, 0, 20}, 0, makeMarker(kLastSync, {3, 0, 20})), Decision::AskNoHistory);
}

TEST(BookOrbitSmartSync, UploadsWhenOnlyThisDeviceReadOn) {
  const Marker marker = makeMarker(kLastSync, {5, 12, 30});
  EXPECT_EQ(decide({5, 20, 30}, {5, 12, 30}, kLastSync, marker), Decision::UploadLocal);
  EXPECT_EQ(decide({6, 2, 25}, {5, 12, 28}, kLastSync, marker), Decision::UploadLocal);
}

TEST(BookOrbitSmartSync, AppliesWhenOnlyAnotherReaderReadOn) {
  const Marker marker = makeMarker(kLastSync, {5, 12, 30});
  EXPECT_EQ(decide({5, 12, 30}, {5, 20, 30}, kLater, marker), Decision::ApplyRemote);
  EXPECT_EQ(decide({5, 12, 30}, {7, 0, 28}, kLater, marker), Decision::ApplyRemote);
  // This device read on too, but the other reader went further still.
  EXPECT_EQ(decide({5, 16, 30}, {5, 20, 30}, kLater, marker), Decision::ApplyRemote);
}

// Issue #91: BookOrbit's web reader saves the position it opens a book at. The server's
// date moves, its position does not, and the device's further position must go up.
TEST(BookOrbitSmartSync, UploadsPastAServerThatOnlyChangedItsDate) {
  const Marker marker = makeMarker(kLastSync, {5, 12, 30});
  // The web reader landed exactly where the device left the book.
  EXPECT_EQ(decide({5, 20, 30}, {5, 12, 30}, kLater, marker), Decision::UploadLocal);
  // One page further is still the same place, as far as the mapping can tell.
  EXPECT_EQ(decide({5, 20, 30}, {5, 13, 30}, kLater, marker), Decision::UploadLocal);
  // It saved the start of its own, larger page, or the start of the chapter.
  EXPECT_EQ(decide({5, 20, 30}, {5, 9, 30}, kLater, marker), Decision::UploadLocal);
  EXPECT_EQ(decide({6, 3, 25}, {5, 0, 28}, kLater, marker), Decision::UploadLocal);
}

// A server position in an earlier chapter is not a date change: somebody went back on
// purpose. A progress reset in BookOrbit is served that way, as the start of the book
// dated now, and what a device uploads over it is held back by the server.
TEST(BookOrbitSmartSync, AsksWhenTheServerWentBackToAnEarlierChapter) {
  const Marker marker = makeMarker(kLastSync, {5, 12, 30});
  EXPECT_EQ(decide({5, 20, 30}, {2, 4, 30}, kLater, marker), Decision::AskConflict);
  EXPECT_EQ(decide({5, 20, 30}, {0, 0, 30}, kLater, marker), Decision::AskConflict);
  EXPECT_EQ(decide({5, 12, 30}, {0, 0, 30}, kLater, marker), Decision::AskConflict);
  // As before positions were recorded: a device that is not past it follows the server.
  EXPECT_EQ(decide({1, 0, 30}, {2, 4, 30}, kLater, marker), Decision::ApplyRemote);
}

TEST(BookOrbitSmartSync, UploadsPastADateChangeEvenWhenThisDeviceDidNotMove) {
  const Marker marker = makeMarker(kLastSync, {5, 12, 30});
  EXPECT_EQ(decide({5, 12, 30}, {5, 9, 30}, kLater, marker), Decision::UploadLocal);
}

TEST(BookOrbitSmartSync, OnlyRedatedIsAPlaceInTheSyncedChapterAtOrBeforeTheSyncedPage) {
  using BookOrbitSmartSync::onlyRedated;
  const Marker marker = makeMarker(kLastSync, {5, 12, 30});
  EXPECT_TRUE(onlyRedated({5, 12, 30}, marker));
  EXPECT_TRUE(onlyRedated({5, 13, 30}, marker));
  EXPECT_TRUE(onlyRedated({5, 0, 30}, marker));
  EXPECT_FALSE(onlyRedated({5, 14, 30}, marker));
  EXPECT_FALSE(onlyRedated({6, 0, 30}, marker));
  EXPECT_FALSE(onlyRedated({4, 29, 30}, marker));
  EXPECT_FALSE(onlyRedated({5, 12, 30}, datedOnly(kLastSync)));
}

TEST(BookOrbitSmartSync, AsksWhenBothSidesReadOn) {
  const Marker marker = makeMarker(kLastSync, {5, 12, 30});
  EXPECT_EQ(decide({5, 25, 30}, {5, 20, 30}, kLater, marker), Decision::AskConflict);
  EXPECT_EQ(decide({7, 0, 30}, {6, 3, 28}, kLater, marker), Decision::AskConflict);
}

TEST(BookOrbitSmartSync, AsksWhenThisDeviceWentBack) {
  const Marker marker = makeMarker(kLastSync, {5, 12, 30});
  // The server did not move at all.
  EXPECT_EQ(decide({3, 4, 30}, {5, 12, 28}, kLastSync, marker), Decision::AskConflict);
  // The server only changed its date: its position is no reason to bring the device forward.
  EXPECT_EQ(decide({3, 4, 30}, {5, 12, 28}, kLater, marker), Decision::AskConflict);
  EXPECT_EQ(decide({5, 2, 30}, {5, 9, 30}, kLater, marker), Decision::AskConflict);
}

TEST(BookOrbitSmartSync, AMarkerWithoutAPositionDecidesOnDatesAlone) {
  const Marker marker = datedOnly(kLastSync);
  EXPECT_EQ(decide({5, 20, 30}, {5, 12, 30}, kLastSync, marker), Decision::UploadLocal);
  EXPECT_EQ(decide({5, 12, 30}, {5, 20, 30}, kLater, marker), Decision::ApplyRemote);
  // Without the position, a date change cannot be told from reading elsewhere.
  EXPECT_EQ(decide({5, 20, 30}, {5, 12, 30}, kLater, marker), Decision::AskConflict);
  EXPECT_EQ(decide({5, 12, 30}, {5, 20, 30}, kLastSync, marker), Decision::AskConflict);
}

TEST(BookOrbitSmartSync, TheRecordedPageFollowsALayoutChange) {
  // Synced on page 12 of 30; the chapter now has 59 pages, so that place is page 24.
  const Marker marker = makeMarker(kLastSync, {5, 12, 30});
  EXPECT_EQ(decide({5, 50, 59}, {5, 24, 59}, kLater, marker), Decision::UploadLocal);
  EXPECT_EQ(decide({5, 50, 59}, {5, 25, 59}, kLater, marker), Decision::UploadLocal);
  EXPECT_EQ(decide({5, 50, 59}, {5, 26, 59}, kLater, marker), Decision::AskConflict);
  // And the other way: 15 pages, page 6.
  EXPECT_EQ(decide({5, 12, 15}, {5, 7, 15}, kLater, marker), Decision::UploadLocal);
  EXPECT_EQ(decide({5, 12, 15}, {5, 8, 15}, kLater, marker), Decision::AskConflict);
}

TEST(BookOrbitSmartSync, PageOutOfKeepsThePageWhenThereIsNoFractionToCarry) {
  using BookOrbitSmartSync::pageOutOf;
  EXPECT_EQ(pageOutOf({5, 12, 30}, 30), 12);
  EXPECT_EQ(pageOutOf({5, 12, 30}, 59), 24);
  EXPECT_EQ(pageOutOf({5, 29, 30}, 59), 58);
  EXPECT_EQ(pageOutOf({5, 0, 30}, 59), 0);
  // A count of one page says nothing about where page 4 stands.
  EXPECT_EQ(pageOutOf({5, 4, 1}, 40), 4);
  EXPECT_EQ(pageOutOf({5, 4, 0}, 40), 4);
  // A one-page chapter has a single place.
  EXPECT_EQ(pageOutOf({5, 12, 30}, 1), 0);
  // The widest values a marker holds do not overflow.
  EXPECT_EQ(pageOutOf({5, 65534, 65535}, 65535), 65534);
  EXPECT_EQ(pageOutOf({5, 65534, 65535}, 32768), 32767);
}

TEST(BookOrbitSmartSync, FurtherOfPicksTheLeadingPosition) {
  using BookOrbitSmartSync::furtherOf;
  EXPECT_EQ(furtherOf({5, 12, 30}, {5, 13, 30}).pageNumber, 13);
  EXPECT_EQ(furtherOf({5, 13, 30}, {5, 12, 30}).pageNumber, 13);
  EXPECT_EQ(furtherOf({6, 0, 30}, {5, 29, 30}).spineIndex, 6);
}

TEST(BookOrbitSmartSyncMarker, RoundTripsItsDateAndPosition) {
  uint8_t raw[BookOrbitSmartSync::MARKER_SIZE];
  BookOrbitSmartSync::encodeMarker(makeMarker(kLastSync, {5, 12, 30}), raw);

  const Marker marker = BookOrbitSmartSync::decodeMarker(raw, sizeof(raw));
  EXPECT_EQ(marker.timestamp, kLastSync);
  ASSERT_TRUE(marker.hasPosition());
  EXPECT_EQ(marker.spineIndex, 5);
  EXPECT_EQ(marker.pageNumber, 12);
  EXPECT_EQ(marker.pageCount, 30);
}

TEST(BookOrbitSmartSyncMarker, ReadsTheDateOfAMarkerFromAnEarlierRelease) {
  // What releases up to v1.6.1+bookorbit.1 wrote: magic and timestamp, 12 bytes.
  uint8_t raw[BookOrbitSmartSync::MARKER_DATED_SIZE];
  const uint32_t magic = 0x424F5359;  // "BOSY"
  const int64_t timestamp = kLastSync;
  memcpy(raw, &magic, 4);
  memcpy(raw + 4, &timestamp, 8);

  const Marker marker = BookOrbitSmartSync::decodeMarker(raw, sizeof(raw));
  EXPECT_EQ(marker.timestamp, kLastSync);
  EXPECT_FALSE(marker.hasPosition());
}

TEST(BookOrbitSmartSyncMarker, AnEarlierReleaseStillReadsItsTwelveBytes) {
  uint8_t raw[BookOrbitSmartSync::MARKER_SIZE];
  BookOrbitSmartSync::encodeMarker(makeMarker(kLastSync, {5, 12, 30}), raw);

  uint32_t magic = 0;
  int64_t timestamp = 0;
  memcpy(&magic, raw, 4);
  memcpy(&timestamp, raw + 4, 8);
  EXPECT_EQ(magic, 0x424F5359u);
  EXPECT_EQ(timestamp, kLastSync);
}

TEST(BookOrbitSmartSyncMarker, AnythingElseIsNoHistory) {
  uint8_t raw[BookOrbitSmartSync::MARKER_SIZE];
  BookOrbitSmartSync::encodeMarker(makeMarker(kLastSync, {5, 12, 30}), raw);

  // Cut short of its date.
  EXPECT_EQ(BookOrbitSmartSync::decodeMarker(raw, 11).timestamp, 0);
  EXPECT_EQ(BookOrbitSmartSync::decodeMarker(raw, 0).timestamp, 0);
  // Cut short inside its position: the date stands, the position does not.
  const Marker cut = BookOrbitSmartSync::decodeMarker(raw, 15);
  EXPECT_EQ(cut.timestamp, kLastSync);
  EXPECT_FALSE(cut.hasPosition());

  uint8_t foreign[BookOrbitSmartSync::MARKER_SIZE];
  memcpy(foreign, raw, sizeof(foreign));
  foreign[0] ^= 0xFF;
  EXPECT_EQ(BookOrbitSmartSync::decodeMarker(foreign, sizeof(foreign)).timestamp, 0);

  // A date that is not one drops the whole marker, position included.
  const int64_t noDate = 0;
  memcpy(raw + 4, &noDate, 8);
  const Marker undated = BookOrbitSmartSync::decodeMarker(raw, sizeof(raw));
  EXPECT_EQ(undated.timestamp, 0);
  EXPECT_FALSE(undated.hasPosition());
}

TEST(BookOrbitSmartSyncMarker, APositionItCannotHoldIsRecordedAsADateAlone) {
  EXPECT_FALSE(makeMarker(kLastSync, {-1, 0, 10}).hasPosition());
  EXPECT_FALSE(makeMarker(kLastSync, {70000, 0, 10}).hasPosition());
  EXPECT_FALSE(makeMarker(kLastSync, {3, -2, 10}).hasPosition());
  EXPECT_EQ(makeMarker(kLastSync, {70000, 0, 10}).timestamp, kLastSync);
  // An unknown page count is one page, so the position still counts.
  const Marker unknownCount = makeMarker(kLastSync, {3, 4, 0});
  ASSERT_TRUE(unknownCount.hasPosition());
  EXPECT_EQ(unknownCount.pageCount, 1);
  EXPECT_EQ(makeMarker(kLastSync, {3, 4, 90000}).pageCount, UINT16_MAX);
}
