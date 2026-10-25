// Covers which sidecar state a BookOrbit sync reports for a store. "known" makes the server
// take an empty key set as the deletion of everything, so it must only follow proof that
// the store itself has been through an exchange.

#include <gtest/gtest.h>

#include "BookOrbitSync/BookOrbitSidecarState.h"

namespace {

constexpr uint32_t kUploadedWatermark = 1'790'000'000;

}  // namespace

TEST(BookOrbitSidecarState, ExchangedStoreIsKnown) {
  EXPECT_EQ(bookOrbitSidecarState(true, kUploadedWatermark, 3, true), BookOrbitSidecarState::Known);
  EXPECT_EQ(bookOrbitSidecarState(true, BOOKORBIT_EXCHANGED_WATERMARK, 0, true), BookOrbitSidecarState::Known);
}

TEST(BookOrbitSidecarState, StoreNeverExchangedIsLeftToTheServer) {
  // Readable but started over (by a new highlight after the old store was lost): its key set
  // holds only what came after, so it must not be reported as the truth.
  EXPECT_EQ(bookOrbitSidecarState(true, 0, 1, true), BookOrbitSidecarState::Unstated);
  EXPECT_EQ(bookOrbitSidecarState(true, 0, 0, true), BookOrbitSidecarState::Unstated);
}

TEST(BookOrbitSidecarState, NoStoreAndNothingHeldIsNew) {
  EXPECT_EQ(bookOrbitSidecarState(false, 0, 0, true), BookOrbitSidecarState::New);
}

TEST(BookOrbitSidecarState, NoStoreButHighlightsHeldIsLeftToTheServer) {
  // "new" would have every highlight sent back on top of the ones still here.
  EXPECT_EQ(bookOrbitSidecarState(false, 0, 2, true), BookOrbitSidecarState::Unstated);
}

TEST(BookOrbitSidecarState, OlderServerIsNeverToldAnything) {
  // A server without the capability rejects the whole exchange when the field is present.
  EXPECT_EQ(bookOrbitSidecarState(true, kUploadedWatermark, 3, false), BookOrbitSidecarState::Unstated);
  EXPECT_EQ(bookOrbitSidecarState(false, 0, 0, false), BookOrbitSidecarState::Unstated);
}
