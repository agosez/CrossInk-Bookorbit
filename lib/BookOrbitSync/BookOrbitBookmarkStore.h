#pragma once
#include <cstdint>
#include <string>
#include <vector>

/**
 * The BookOrbit-facing identity of one local bookmark.
 *
 * The server keys bookmarks by md5(datetime | pos), so both fields have to stay byte-identical
 * between syncs. The position is minted once, when the bookmark is created (the reader has the
 * section open and its page offsets at hand), and never recomputed.
 *
 * `timestamp` joins the record back to its Bookmark: real UTC epoch seconds, stamped by
 * BookmarkStore::addBookmark from WallClock. Bookmarks created before that stamping existed
 * carry 0 and gain a record through the backfill, which also assigns them a timestamp.
 *
 * `identityEpoch` is the datetime the server keys on. Equal to `timestamp` for a bookmark made
 * on this device; for one received from the server it is the datetime this device MINTED at
 * apply time -- bookmarks invert the annotation convention: the device owns the identity and
 * reports it in the acknowledgment, which is what links the server's copy to this device.
 */
struct BookOrbitBookmarkRecord {
  uint32_t timestamp = 0;
  uint32_t identityEpoch = 0;
  uint16_t spineIndex = 0;
  std::string pos;  // KOReader xpointer of the bookmarked page's first codepoint
};

/**
 * On-SD companion to a book's bookmark file, one per book in the book's cache directory (see
 * docs/file-formats.md). Also holds the upload watermark: with up to 1024 bookmarks a book,
 * re-offering the full set every sync (the reference plugin's approach) does not fit beside a
 * TLS session here.
 */
class BookOrbitBookmarkStore {
 public:
  static constexpr size_t MAX_RECORDS = 1024;

  // Records the position minted for a bookmark. Replaces the record with the same timestamp
  // and spine, so re-stamping does not accumulate duplicates.
  static bool put(const std::string& bookCachePath, const BookOrbitBookmarkRecord& record);

  // Returns false on a missing or unreadable file, leaving out empty. As with
  // BookOrbitAnnotationStore::readAll(), that false return means this book has no bookmark
  // sync history, and a sync must not report an empty-complete key set from it.
  static bool readAll(const std::string& bookCachePath, std::vector<BookOrbitBookmarkRecord>& out);

  static uint32_t readWatermark(const std::string& bookCachePath);

  // Never moves backwards: a clock that lost time must not make the next sync re-offer
  // bookmarks the server already holds.
  static bool advanceWatermark(const std::string& bookCachePath, uint32_t timestamp);

  /**
   * Drop records whose bookmark no longer exists locally. Orphans would fill the file and,
   * worse, describe bookmarks to the server that this device no longer holds.
   */
  static bool retain(const std::string& bookCachePath, const std::vector<uint32_t>& keepTimestamps);
};
