#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// What BookOrbit Smart sync remembers of a book between two syncs, and the decision it
// takes from it. Free of storage and network code so the rules can be tested on their
// own (test/bookorbit_smart_sync); BookOrbitSyncActivity does the file and server work.
namespace BookOrbitSmartSync {

// A reading position in local pages: a chapter, and a page out of the page count that
// chapter had under the layout it was measured in.
struct Position {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
};

// This device's memory of its last successful sync of a book: the server's progress
// timestamp at the end of it, and the position both sides were left at. A timestamp of
// 0 is no history at all. Markers written up to v1.6.1+bookorbit.1 hold the date alone.
struct Marker {
  int64_t timestamp = 0;
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t pageCount = 0;  // 0 = no position recorded

  constexpr bool hasPosition() const { return pageCount > 0; }
  constexpr Position position() const { return {spineIndex, pageNumber, pageCount}; }
};

// On-disk form (docs/file-formats.md): magic, timestamp, then the position, in the
// device's byte order. The position was appended under the same magic, so both ways
// keep working: earlier releases read the 12 bytes they know and ignore the rest, and
// their own markers read here as a date without a position.
constexpr uint32_t MARKER_MAGIC = 0x424F5359;  // "BOSY"
constexpr size_t MARKER_DATED_SIZE = sizeof(uint32_t) + sizeof(int64_t);
constexpr size_t MARKER_SIZE = MARKER_DATED_SIZE + 3 * sizeof(uint16_t);

// A position the marker cannot hold is recorded as a date alone. A page count the
// caller does not know is one page, which leaves the page number to be compared as is.
constexpr Marker makeMarker(const int64_t timestamp, const Position& position) {
  Marker marker;
  marker.timestamp = timestamp;
  if (position.spineIndex < 0 || position.spineIndex > UINT16_MAX || position.pageNumber < 0 ||
      position.pageNumber > UINT16_MAX) {
    return marker;
  }
  marker.spineIndex = static_cast<uint16_t>(position.spineIndex);
  marker.pageNumber = static_cast<uint16_t>(position.pageNumber);
  marker.pageCount = static_cast<uint16_t>(std::clamp(position.pageCount, 1, static_cast<int>(UINT16_MAX)));
  return marker;
}

inline void encodeMarker(const Marker& marker, uint8_t out[MARKER_SIZE]) {
  const uint32_t magic = MARKER_MAGIC;
  memcpy(out, &magic, sizeof(magic));
  memcpy(out + 4, &marker.timestamp, sizeof(marker.timestamp));
  memcpy(out + 12, &marker.spineIndex, sizeof(marker.spineIndex));
  memcpy(out + 14, &marker.pageNumber, sizeof(marker.pageNumber));
  memcpy(out + 16, &marker.pageCount, sizeof(marker.pageCount));
}

// Anything that is not a whole dated marker is no history, never a wrong date. A marker
// cut short inside its position keeps its date and loses the position.
inline Marker decodeMarker(const uint8_t* data, const size_t size) {
  Marker marker;
  uint32_t magic = 0;
  if (size < MARKER_DATED_SIZE) return marker;
  memcpy(&magic, data, sizeof(magic));
  if (magic != MARKER_MAGIC) return marker;
  memcpy(&marker.timestamp, data + 4, sizeof(marker.timestamp));
  if (marker.timestamp <= 0) return Marker{};
  if (size >= MARKER_SIZE) {
    memcpy(&marker.spineIndex, data + 12, sizeof(marker.spineIndex));
    memcpy(&marker.pageNumber, data + 14, sizeof(marker.pageNumber));
    memcpy(&marker.pageCount, data + 16, sizeof(marker.pageCount));
  }
  return marker;
}

// Mapping a server position into local pages is exact to about one page, so two
// positions within a page of each other are the same place.
constexpr int PAGE_TOLERANCE = 1;

constexpr bool samePlace(const Position& a, const Position& b) {
  const int pageDelta = a.pageNumber - b.pageNumber;
  return a.spineIndex == b.spineIndex && pageDelta <= PAGE_TOLERANCE && pageDelta >= -PAGE_TOLERANCE;
}

constexpr bool isAhead(const Position& a, const Position& b) {
  return a.spineIndex > b.spineIndex || (a.spineIndex == b.spineIndex && a.pageNumber - b.pageNumber > PAGE_TOLERANCE);
}

// Where two sides found at the same place are taken to be: the further of the two.
constexpr Position furtherOf(const Position& a, const Position& b) {
  const bool aLeads = a.spineIndex > b.spineIndex || (a.spineIndex == b.spineIndex && a.pageNumber >= b.pageNumber);
  return aLeads ? a : b;
}

// A recorded page is a fraction of the page count it was measured against, and the
// chapter may have been laid out again since (another font, other margins, an estimated
// count that has settled). This is the page it stands for out of pageCount pages.
constexpr int pageOutOf(const Position& recorded, const int pageCount) {
  if (recorded.pageCount == pageCount || recorded.pageCount <= 1) return recorded.pageNumber;
  if (pageCount <= 1) return 0;
  const int64_t span = recorded.pageCount - 1;
  return static_cast<int>((static_cast<int64_t>(recorded.pageNumber) * (pageCount - 1) * 2 + span) / (span * 2));
}

// BookOrbit's web reader saves the position a book opens at, which dates the server's
// position as of now although nobody read a page. What it saves is the start of its own
// page, or the start of the chapter when it sends no finer position: a place in the
// chapter this device last synced in, at or before the page it synced. A server position
// found there has only been dated again.
constexpr bool onlyRedated(const Position& remote, const Marker& marker) {
  return marker.hasPosition() && remote.spineIndex == marker.spineIndex &&
         remote.pageNumber - pageOutOf(marker.position(), remote.pageCount) <= PAGE_TOLERANCE;
}

enum class Decision : uint8_t {
  AlreadySynced,  // both sides are at the same place
  ApplyRemote,    // the server moved since the last sync, and this device is not past it
  UploadLocal,    // this device is further, and the server did not move
  AskNoHistory,   // no earlier sync of this book to compare with, or a server without dates
  AskConflict,    // both sides moved, or this device went back: a person decides
};

// local and remote are both in today's pages, remote being the server position mapped
// into them. Acts only when the order of the positions and what happened on the server
// since the last sync tell the same story.
constexpr Decision decide(const Position& local, const Position& remote, const int64_t remoteTimestamp,
                          const Marker& marker) {
  if (samePlace(local, remote)) return Decision::AlreadySynced;
  if (remoteTimestamp <= 0 || marker.timestamp <= 0) return Decision::AskNoHistory;

  // A newer date alone does not say the server moved: see onlyRedated(). Anywhere else
  // it did. Further on, somebody read elsewhere. In an earlier chapter, somebody went
  // back on purpose: a progress reset is served as the start of the book dated now, and
  // the server holds back what a device uploads over it, so that one must reach a
  // person. A marker from before positions were recorded has the date alone to go by.
  const bool serverMoved = remoteTimestamp > marker.timestamp && !onlyRedated(remote, marker);
  const bool localAhead = isAhead(local, remote);
  if (!localAhead && serverMoved) return Decision::ApplyRemote;
  if (localAhead && !serverMoved) return Decision::UploadLocal;
  return Decision::AskConflict;
}

}  // namespace BookOrbitSmartSync
