#include "BookOrbitStatsQueue.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WallClock.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr char QUEUE_FILENAME[] = "/bookorbit_stats.bin";
// 4-byte header: magic + format version. Bump the version if the record layout changes
// and document it in docs/file-formats.md; queues whose header does not match are
// discarded on read and replaced on append, so a bump costs at most the events that
// were still waiting to be uploaded.
constexpr char QUEUE_MAGIC[4] = {'B', 'O', 'Q', '1'};

std::string queuePath(const std::string& bookCachePath) { return bookCachePath + QUEUE_FILENAME; }

// Howard Hinnant's days_from_civil: days since 1970-01-01 for a Gregorian date.
int32_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(year - era * 400);
  const uint32_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

// The DS3231 is synced in UTC (see HalClock); only present on the X3.
bool rtcUtcEpoch(uint32_t& outEpochSeconds) {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute)) {
    return false;
  }
  if (year < 2020 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
    return false;
  }
  const int32_t days = daysFromCivil(year, month, day);
  outEpochSeconds = static_cast<uint32_t>(days) * 86400u + hour * 3600u + minute * 60u;
  return true;
}
}  // namespace

namespace {
// Writes header + records to a freshly truncated queue file.
bool rewriteQueue(const char* path, const std::vector<BookOrbitStatEvent>& events, size_t firstIndex) {
  FsFile file;
  if (!Storage.openFileForWrite("BOQ", path, file)) {
    LOG_ERR("BOQ", "Failed to open stats queue for rewrite: %s", path);
    return false;
  }
  bool ok = file.write(QUEUE_MAGIC, sizeof(QUEUE_MAGIC)) == sizeof(QUEUE_MAGIC);
  for (size_t i = firstIndex; ok && i < events.size(); i++) {
    ok = file.write(&events[i], sizeof(BookOrbitStatEvent)) == sizeof(BookOrbitStatEvent);
  }
  file.close();
  if (!ok) {
    LOG_ERR("BOQ", "Failed to rewrite stats queue");
  }
  return ok;
}
}  // namespace

bool BookOrbitStatsQueue::appendBatch(const std::string& bookCachePath, const std::vector<BookOrbitStatEvent>& events) {
  if (events.empty()) {
    return true;
  }
  const std::string path = queuePath(bookCachePath);

  // Never append onto a queue with an old/foreign header: readAll() would discard
  // the whole file — including the records we are about to add. Recreate it instead.
  size_t existingCount = 0;
  if (Storage.exists(path.c_str())) {
    FsFile existing;
    char magic[4] = {};
    const bool headerOk = Storage.openFileForRead("BOQ", path.c_str(), existing) &&
                          existing.read(magic, sizeof(magic)) == sizeof(magic) &&
                          memcmp(magic, QUEUE_MAGIC, sizeof(QUEUE_MAGIC)) == 0;
    if (headerOk) {
      existingCount = (existing.fileSize() - sizeof(QUEUE_MAGIC)) / sizeof(BookOrbitStatEvent);
    }
    if (existing.isOpen()) existing.close();
    if (!headerOk) {
      LOG_INF("BOQ", "Replacing old-format stats queue: %s", path.c_str());
      Storage.remove(path.c_str());
    }
  }

  // Cap overflow: drop the OLDEST events (recent reading beats stale backlog).
  if (existingCount + events.size() > MAX_QUEUED_EVENTS) {
    std::vector<BookOrbitStatEvent> merged;
    if (!readAll(bookCachePath, merged)) {
      merged.clear();
    }
    merged.reserve(merged.size() + events.size());
    merged.insert(merged.end(), events.begin(), events.end());
    const size_t firstKept = merged.size() > MAX_QUEUED_EVENTS ? merged.size() - MAX_QUEUED_EVENTS : 0;
    LOG_INF("BOQ", "Stats queue over cap; dropping %u oldest events", (unsigned)firstKept);
    return rewriteQueue(path.c_str(), merged, firstKept);
  }

  FsFile file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("BOQ", "Failed to open stats queue for append: %s", path.c_str());
    return false;
  }

  bool ok = true;
  if (file.fileSize() == 0) {
    ok = file.write(QUEUE_MAGIC, sizeof(QUEUE_MAGIC)) == sizeof(QUEUE_MAGIC);
  }
  for (size_t i = 0; ok && i < events.size(); i++) {
    ok = file.write(&events[i], sizeof(BookOrbitStatEvent)) == sizeof(BookOrbitStatEvent);
  }
  const size_t totalBytes = file.fileSize();
  file.close();
  if (!ok) {
    LOG_ERR("BOQ", "Failed to append stats queue batch");
  } else {
    // INF so field logs show whether a session's events reached the SD queue (and
    // how many the file holds) — key evidence when sessions go missing at sync.
    LOG_INF("BOQ", "Queued %u events (file now holds %u)", (unsigned)events.size(),
            (unsigned)((totalBytes - sizeof(QUEUE_MAGIC)) / sizeof(BookOrbitStatEvent)));
  }
  return ok;
}

bool BookOrbitStatsQueue::readAll(const std::string& bookCachePath, std::vector<BookOrbitStatEvent>& outEvents) {
  outEvents.clear();
  const std::string path = queuePath(bookCachePath);
  if (!Storage.exists(path.c_str())) {
    return true;
  }

  FsFile file;
  if (!Storage.openFileForRead("BOQ", path.c_str(), file)) {
    return false;
  }

  char magic[4];
  if (file.read(magic, sizeof(magic)) != sizeof(magic) || memcmp(magic, QUEUE_MAGIC, sizeof(QUEUE_MAGIC)) != 0) {
    LOG_ERR("BOQ", "Old or bad stats queue header, discarding queue: %s", path.c_str());
    file.close();
    Storage.remove(path.c_str());
    return true;
  }

  const size_t recordBytes = file.fileSize() - sizeof(QUEUE_MAGIC);
  outEvents.reserve(std::min(recordBytes / sizeof(BookOrbitStatEvent), MAX_QUEUED_EVENTS));

  BookOrbitStatEvent event;
  while (outEvents.size() < MAX_QUEUED_EVENTS && file.read(&event, sizeof(event)) == sizeof(event)) {
    outEvents.push_back(event);
  }
  file.close();
  return true;
}

size_t BookOrbitStatsQueue::queuedCount(const std::string& bookCachePath) {
  const std::string path = queuePath(bookCachePath);
  if (!Storage.exists(path.c_str())) return 0;
  FsFile file;
  if (!Storage.openFileForRead("BOQ", path.c_str(), file)) return 0;
  char magic[4];
  size_t count = 0;
  if (file.read(magic, sizeof(magic)) == sizeof(magic) && memcmp(magic, QUEUE_MAGIC, sizeof(QUEUE_MAGIC)) == 0) {
    count = (file.fileSize() - sizeof(QUEUE_MAGIC)) / sizeof(BookOrbitStatEvent);
  }
  file.close();
  return count;
}

void BookOrbitStatsQueue::clear(const std::string& bookCachePath) {
  const std::string path = queuePath(bookCachePath);
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
}

bool BookOrbitStatsQueue::captureNow(uint32_t& outEpochSeconds, bool& outApproximate) {
  if (rtcUtcEpoch(outEpochSeconds)) {
    outApproximate = false;  // battery-backed DS3231: genuinely absolute time
    return true;
  }

  const bool plausible = WallClock::now(outEpochSeconds);
  // The system clock runs on the internal RC oscillator and drifts by minutes across
  // long deep sleeps, so even an NTP-confirmed clock goes stale. Every system-clock
  // stamp is therefore marked correctable and re-resolved against fresh NTP at
  // upload time (see BookOrbitSyncActivity::uploadQueuedStats).
  outApproximate = true;
  return plausible || outEpochSeconds != 0;
}
