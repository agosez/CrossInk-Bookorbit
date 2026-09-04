#pragma once
#include <cstdint>
#include <string>
#include <vector>

/**
 * Identifies one local highlight across the two stores.
 *
 * `Clipping::timestamp` alone will not do: it is millis()/1000, seconds since boot, so two
 * highlights made at the same offset in different sessions share it. Adding the chapter and
 * paragraph makes a collision mean the same highlight in practice.
 */
struct BookOrbitClippingRef {
  uint32_t timestamp = 0;
  uint16_t spineIndex = 0;
  uint16_t paragraphIndex = 0;

  bool operator==(const BookOrbitClippingRef& other) const {
    return timestamp == other.timestamp && spineIndex == other.spineIndex && paragraphIndex == other.paragraphIndex;
  }
};

/**
 * The BookOrbit-facing identity of one local highlight.
 *
 * The server keys annotations by md5(datetime | pos0), so pos0 has to be byte-identical
 * on every sync or the same highlight returns as a new one. pos0 is minted when the
 * highlight is created, not when it is synced: building it walks the chapter's HTML
 * structure (ChapterXPathResolver), which is nearly free while the reader already has the
 * chapter open and would mean parsing every affected chapter with 55 KB already committed
 * to TLS if it were left to sync time.
 *
 * `timestamp` joins this record back to its Clipping, and is NOT a date: Clipping::timestamp is
 * millis()/1000, seconds since boot. That is why the join goes through BookOrbitClippingRef and
 * why the datetime the server keys on lives in its own field.
 */
struct BookOrbitAnnotationRecord {
  uint32_t timestamp = 0;  // Clipping::timestamp, seconds since boot; joins to the Clipping
  /**
   * The datetime the server keys this annotation by, as UTC epoch seconds.
   *
   * Taken from WallClock when the highlight is made here, and from the server's own datetime for
   * one received from it. Never from `timestamp`: seconds-since-boot dated every highlight 1970
   * and made the upload watermark non-monotonic, so a highlight created early in a session looked
   * older than one from a previous session and was skipped as already sent.
   */
  uint32_t identityEpoch = 0;
  uint16_t spineIndex = 0;  // Kept for diagnostics and for grouping work by chapter
  uint16_t paragraphIndex = 0;
  std::string pos0;  // KOReader xpointer at the highlight's first codepoint
  std::string pos1;  // Exclusive end, so the pair delimits the highlighted text
};

/**
 * On-SD companion to a book's clipping file, one per book in the book's cache directory
 * (see docs/file-formats.md). Deliberately separate from the clipping store's own format,
 * which several screens read: highlight sync can gain fields without those screens caring.
 *
 * Also holds the upload watermark, the newest annotation datetime the server has taken, so
 * a sync sends only what changed instead of re-offering every highlight.
 */
class BookOrbitAnnotationStore {
 public:
  // Matches CLIPPING_MAX_PER_BOOK: this file never needs to describe more highlights than
  // a book can hold, and the bound keeps a corrupt header from driving a huge allocation.
  static constexpr size_t MAX_RECORDS = 256;

  // Records the xpointers minted for a newly created highlight. Replaces the record of the same
  // highlight (see BookOrbitClippingRef) so re-stamping one does not accumulate duplicates.
  static bool put(const std::string& bookCachePath, const BookOrbitAnnotationRecord& record);

  /**
   * Reads every record. Returns false on a missing or unreadable file, leaving out empty.
   *
   * That false return doubles as a data-safety signal: put() is the sole creator of the
   * file, and it runs when a highlight position is minted, local or applied from the
   * server. A store that cannot be read therefore means "this book never synced
   * highlights" or "history lost", never "the user deleted everything" -- and a sync must
   * not report an empty-complete key set in that state, or the server reads the gap as
   * deletions and erases every highlight this device ever acked.
   */
  static bool readAll(const std::string& bookCachePath, std::vector<BookOrbitAnnotationRecord>& out);

  // The newest identityEpoch the server has accepted, 0 when nothing has been sent.
  static uint32_t readWatermark(const std::string& bookCachePath);

  // Advances the watermark. Never moves it backwards: a clock that lost time must not make
  // the next sync re-offer highlights the server already holds.
  static bool advanceWatermark(const std::string& bookCachePath, uint32_t timestamp);

  /**
   * Drop records whose highlight no longer exists locally.
   *
   * Deleting a clipping does not touch this file, so its records outlive the highlights they
   * describe. Orphans matter twice over: they would eventually fill the file and block new
   * highlights, and a key set built from them describes annotations the device no longer holds.
   *
   * @param keep The clippings that still exist
   * @return true when the file already matched or was rewritten successfully
   */
  static bool retain(const std::string& bookCachePath, const std::vector<BookOrbitClippingRef>& keep);
};
