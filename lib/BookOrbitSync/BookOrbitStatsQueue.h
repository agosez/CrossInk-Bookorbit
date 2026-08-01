#pragma once
#include <cstdint>
#include <string>
#include <vector>

/**
 * One per-page reading event queued for upload to BookOrbit's page-stats endpoint,
 * matching what BookOrbit's own KOReader plugin sends (the server clusters raw page
 * events into reading sessions, which power its time/streak/pace/reading-DNA stats).
 *
 * CrossPoint page numbers are per-chapter and layout-dependent, so events store the
 * overall book position in basis points instead: page N of totalPages=10000, so
 * page/totalPages reproduces the exact reading fraction on any device.
 *
 * `era`/`flags` exist because the X4 has no battery-backed RTC: events recorded
 * before the clock was NTP-confirmed carry CLOCK_APPROXIMATE and the WallClock power
 * era they were stamped in. Same-era events are corrected to exact time at upload
 * (see BookOrbitSyncActivity::uploadQueuedStats); older-era events upload with their
 * checkpoint-approximate stamps.
 */
struct BookOrbitStatEvent {
  uint32_t startTime = 0;        // Page-read start, UTC epoch seconds (possibly approximate)
  uint32_t durationSeconds = 0;  // Dwell time on the page in seconds
  uint16_t page = 0;             // Overall book position in basis points (0-10000)
  uint16_t totalPages = 0;       // BookOrbitStatsQueue::PROGRESS_SCALE (self-describing per event)
  uint16_t era = 0;              // WallClock power era the timestamp was taken in (truncated)
  uint8_t flags = 0;             // FLAG_* bitmask
  uint8_t reserved = 0;

  static constexpr uint8_t FLAG_CLOCK_APPROXIMATE = 0x01;
};
static_assert(sizeof(BookOrbitStatEvent) == 16, "queue file records are raw 16-byte structs");

/**
 * On-SD queue of per-page reading events, one file per book in the book's cache
 * directory (see docs/file-formats.md). The reader buffers events in RAM during a
 * session and appends them as one batch when it closes (never once per page turn),
 * and BookOrbitSyncActivity drains the file on the next successful sync.
 */
class BookOrbitStatsQueue {
 public:
  // Fixed denominator reported to the server as `totalPages`: positions are basis
  // points, so page/totalPages reproduces the exact reading fraction.
  static constexpr uint16_t PROGRESS_SCALE = 10000;

  // Bounds the queue file so a never-synced device cannot grow it unbounded
  // (2000 events = 32KB on SD and ~32KB heap when drained for upload). On overflow
  // the OLDEST events are dropped: recent reading is worth more than old backlog.
  static constexpr size_t MAX_QUEUED_EVENTS = 2000;

  // Appends a batch of events to <bookCachePath>/bookorbit_stats.bin, dropping the
  // oldest queued events if the cap would be exceeded. Returns false on storage
  // failure. An empty batch is a no-op success.
  static bool appendBatch(const std::string& bookCachePath, const std::vector<BookOrbitStatEvent>& events);

  // Reads every queued event. Returns false on storage failure or a bad header;
  // an absent file (or a discarded pre-v2 queue) yields true with an empty vector.
  static bool readAll(const std::string& bookCachePath, std::vector<BookOrbitStatEvent>& outEvents);

  // Removes the queue file (call after a fully successful upload).
  static void clear(const std::string& bookCachePath);

  // Number of events currently queued for the book (0 for absent/foreign files).
  // Cheap size probe for diagnostics; does not load the events.
  static size_t queuedCount(const std::string& bookCachePath);

  // Captures the current UTC epoch for stamping a session. Prefers the DS3231 RTC
  // (X3; exact), then the system clock kept by WallClock (X4; may be approximate).
  // Sets `outApproximate` when the value may be offset from real time and should be
  // corrected/qualified at upload. Returns false only when no usable clock exists
  // at all (first-ever boot before any checkpoint or NTP) — the timestamp is still
  // written and remains correctable within the same power era.
  static bool captureNow(uint32_t& outEpochSeconds, bool& outApproximate);
};
