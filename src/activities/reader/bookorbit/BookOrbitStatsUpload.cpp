#include "BookOrbitStatsUpload.h"

#include <I18n.h>
#include <Logging.h>
#include <WallClock.h>

#include <algorithm>
#include <ctime>
#include <vector>

#include "BookOrbitStatsQueue.h"
#include "BookOrbitSyncClient.h"
#include "BookOrbitSyncReporter.h"
#include "CrossPointSettings.h"
#include "util/BookContentId.h"

namespace BookOrbitStatsUpload {

size_t uploadQueued(const std::string& epubPath, const std::string& documentHash, BookOrbitSyncReporter& reporter) {
  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  // The queue is drained one upload batch at a time. Reading it whole cost 16 bytes
  // per event -- 32KB for a full queue -- and held them for the length of the upload,
  // beside the TLS session and every body built for it; a long backlog left too
  // little for the batch that was supposed to clear it.
  const size_t total = std::min(BookOrbitStatsQueue::queuedCount(stateDir), BookOrbitStatsQueue::MAX_QUEUED_EVENTS);
  if (total == 0) {
    LOG_INF("BookOrbit", "No queued reading stats for this book");
    return 0;
  }
  LOG_INF("BookOrbit", "Draining %u queued events (era now %u)", (unsigned)total, (unsigned)WallClock::era());

  // The bar belongs to this phase: clear it on every exit path, or the next
  // phase's status message is drawn over a stale one.
  struct PhaseScope {
    BookOrbitSyncReporter& reporter;
    ~PhaseScope() { reporter.endPhase(); }
  } phaseScope{reporter};
  reporter.beginPhase(tr(STR_SYNCING_READING_SESSIONS), total);

  // Events stamped by the system clock (all of them on RTC-less devices) are
  // re-resolved against NTP: each event gets the correction WallClock measured for ITS
  // era, interpolated along the era's drift ramp when the clock ran continuously since
  // the previous sync, or applied as a flat per-era shift when the era opened on a clock
  // loss (see WallClock::correctionForEvent). The current era's anchors were refreshed
  // by the NTP sync moments ago. Events from eras whose error was never measured keep
  // their checkpoint-approximate stamp unless outright implausible.
  uint32_t syncInstant = 0;                           // real time (NTP set the clock moments ago), 0 if it failed
  if (!WallClock::now(syncInstant)) syncInstant = 0;  // implausible: no upper bound to enforce

  // One batch per request. 25 events is ~2KB of JSON: small enough that building the
  // body cannot exhaust what the kept-session heap floor admits, which a 100-event
  // batch could (measured: abort() while serializing, with a 967-event backlog).
  constexpr size_t BATCH_SIZE = 25;
  // Upload manifest: the corrected, as-sent timestamps, so "which session is
  // missing" is answerable from serial logs. Capped to keep log volume sane.
  constexpr size_t MAX_MANIFEST_LINES = 16;

  std::vector<BookOrbitStatEvent> batch;
  uint32_t previousStart = 0;  // carried across batches: the queue is chronological
  size_t dropped = 0;
  size_t uploaded = 0;
  size_t manifestLines = 0;
  size_t shownTenth = 0;

  for (size_t offset = 0; offset < total; offset += BATCH_SIZE) {
    // A long backlog takes a minute; the queue is only cleared once all of it is
    // accepted, so the sleep sync can stop between batches.
    if (reporter.wakeRequested()) return uploaded;
    if (!BookOrbitStatsQueue::readRange(stateDir, offset, BATCH_SIZE, batch)) {
      LOG_ERR("BookOrbit", "Failed to read queued stats at event %u; keeping the queue", (unsigned)offset);
      return uploaded;
    }
    if (batch.empty()) {
      // The file is shorter than its size implied: a truncated queue. Clearing it
      // below is the self-heal; leaving it would stall every future sync here.
      LOG_ERR("BookOrbit", "Stats queue ended early at event %u of %u", (unsigned)offset, (unsigned)total);
      break;
    }

    size_t droppedInBatch = 0;
    for (auto& event : batch) {
      if (event.flags & BookOrbitStatEvent::FLAG_CLOCK_APPROXIMATE) {
        int64_t delta = 0;
        if (WallClock::correctionForEvent(event.era, event.startTime, delta)) {
          int64_t corrected = static_cast<int64_t>(event.startTime) + delta;
          // Nothing queued can have happened after the sync that is uploading it, and the
          // queue is chronological: keep both invariants whatever the measured error was.
          if (syncInstant != 0 && corrected > static_cast<int64_t>(syncInstant)) corrected = syncInstant;
          if (corrected < static_cast<int64_t>(previousStart)) corrected = previousStart;
          if (corrected > 0 && corrected < static_cast<int64_t>(WallClock::MAX_PLAUSIBLE_EPOCH)) {
            event.startTime = static_cast<uint32_t>(corrected);
          }
        } else if (event.startTime < WallClock::MIN_PLAUSIBLE_EPOCH) {
          event.durationSeconds = 0;  // unresolvable (old era, never had a checkpoint): drop below
          droppedInBatch++;
        }
      }
      previousStart = event.startTime;
    }
    if (droppedInBatch > 0) {
      batch.erase(std::remove_if(batch.begin(), batch.end(),
                                 [](const BookOrbitStatEvent& e) { return e.durationSeconds == 0; }),
                  batch.end());
      dropped += droppedInBatch;
    }
    if (batch.empty()) continue;

    for (size_t i = 0; i < batch.size() && manifestLines < MAX_MANIFEST_LINES; i++, manifestLines++) {
      char isoTime[24];
      const time_t start = static_cast<time_t>(batch[i].startTime);
      struct tm startUtc = {};
      gmtime_r(&start, &startUtc);
      strftime(isoTime, sizeof(isoTime), "%Y-%m-%d %H:%M:%SZ", &startUtc);
      LOG_INF("BookOrbit", "  event %u/%u: start=%s dur=%us pos=%u.%02u%%", (unsigned)(manifestLines + 1),
              (unsigned)total, isoTime, (unsigned)batch[i].durationSeconds, (unsigned)(batch[i].page / 100),
              (unsigned)(batch[i].page % 100));
    }

    const auto result = BookOrbitSyncClient::uploadPageStats(documentHash, SETTINGS.getEffectiveDeviceName(),
                                                             batch.data(), batch.size());
    if (result != BookOrbitSyncClient::OK) {
      const int httpCode = BookOrbitSyncClient::lastHttpCode;
      if (httpCode == 404 || httpCode == 405 || httpCode == 501) {
        // Self-heal: this BookOrbit server predates the page-stats endpoint. Drop
        // the queue instead of re-firing a doomed upload on every future sync;
        // progress sync is unaffected. Updating the server starts buffering fresh.
        LOG_INF("BookOrbit", "Server has no page-stats endpoint (http=%d); discarding queued stats", httpCode);
        BookOrbitStatsQueue::clear(stateDir);
        return uploaded;
      }
      // Transient failure: keep the whole queue for a later attempt; re-sending an
      // already-accepted batch next time is harmless compared to losing sessions.
      LOG_ERR("BookOrbit", "Stats upload failed after %u/%u events (http=%d)", (unsigned)uploaded, (unsigned)total,
              httpCode);
      return uploaded;
    }
    uploaded += batch.size();

    // A batch is a ~half-second round trip and a panel refresh costs about as
    // much, so repaint on each tenth of the queue rather than on each batch.
    const size_t tenth = uploaded * 10 / total;
    if (tenth != shownTenth || uploaded >= total) {
      shownTenth = tenth;
      reporter.reportProgress(uploaded, total);
    }
  }

  if (dropped > 0) {
    LOG_ERR("BookOrbit", "Dropped %u stat events with unresolvable timestamps", (unsigned)dropped);
  }
  LOG_INF("BookOrbit", "Uploaded %u reading-session events", (unsigned)uploaded);
  BookOrbitStatsQueue::clear(stateDir);
  return uploaded;
}

}  // namespace BookOrbitStatsUpload
