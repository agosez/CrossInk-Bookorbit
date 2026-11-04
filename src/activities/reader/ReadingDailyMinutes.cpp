#include "ReadingDailyMinutes.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace ReadingDailyMinutes {
namespace {
constexpr char PATH[] = "/.crosspoint/daily_minutes.bin";
constexpr uint8_t MAGIC[4] = {'B', 'O', 'D', 'M'};
constexpr uint8_t VERSION = 1;
constexpr size_t LAST_DAY_OFFSET = 8;
constexpr size_t BUCKETS_OFFSET = 12;
// Magic, version, 3 reserved bytes, lastDay, seconds per time of day.
constexpr size_t HEADER_SIZE = BUCKETS_OFFSET + READING_TIME_BUCKET_COUNT * sizeof(uint32_t);
constexpr size_t SLOT_SIZE = 2;
// The day totals, then the pending times.
constexpr size_t PENDING_OFFSET = HEADER_SIZE + DAYS * SLOT_SIZE;
constexpr size_t FILE_SIZE = PENDING_OFFSET + DAYS * SLOT_SIZE;
// Slots count tens of seconds: a uint16_t still holds more than a day, and a short
// session is not rounded away the way it would be in whole minutes.
constexpr uint32_t SECONDS_PER_UNIT = 10;

enum class Column : uint8_t { Total, Pending };

size_t slotOffset(const uint32_t dayIndex, const Column column = Column::Total) {
  return (column == Column::Total ? HEADER_SIZE : PENDING_OFFSET) + (dayIndex % DAYS) * SLOT_SIZE;
}

bool readHeader(HalFile& file, uint32_t& lastDay, std::array<uint32_t, READING_TIME_BUCKET_COUNT>& buckets) {
  uint8_t header[HEADER_SIZE];
  if (!file.seekSet(0) || file.read(header, HEADER_SIZE) != static_cast<int>(HEADER_SIZE)) return false;
  if (memcmp(header, MAGIC, sizeof(MAGIC)) != 0 || header[4] != VERSION) return false;
  memcpy(&lastDay, header + LAST_DAY_OFFSET, sizeof(lastDay));
  memcpy(buckets.data(), header + BUCKETS_OFFSET, READING_TIME_BUCKET_COUNT * sizeof(uint32_t));
  return true;
}

bool writeBuckets(HalFile& file, const std::array<uint32_t, READING_TIME_BUCKET_COUNT>& buckets) {
  constexpr size_t size = READING_TIME_BUCKET_COUNT * sizeof(uint32_t);
  return file.seekSet(BUCKETS_OFFSET) && file.write(reinterpret_cast<const uint8_t*>(buckets.data()), size) == size;
}

bool writeLastDay(HalFile& file, const uint32_t lastDay) {
  return file.seekSet(LAST_DAY_OFFSET) &&
         file.write(reinterpret_cast<const uint8_t*>(&lastDay), sizeof(lastDay)) == sizeof(lastDay);
}

bool writeSlot(HalFile& file, const uint32_t dayIndex, const uint16_t units, const Column column = Column::Total) {
  return file.seekSet(slotOffset(dayIndex, column)) &&
         file.write(reinterpret_cast<const uint8_t*>(&units), SLOT_SIZE) == SLOT_SIZE;
}

bool readSlot(HalFile& file, const uint32_t dayIndex, uint16_t& units, const Column column = Column::Total) {
  return file.seekSet(slotOffset(dayIndex, column)) &&
         file.read(reinterpret_cast<uint8_t*>(&units), SLOT_SIZE) == static_cast<int>(SLOT_SIZE);
}

bool addToSlot(HalFile& file, const uint32_t dayIndex, const uint32_t seconds, const Column column) {
  uint16_t units = 0;
  if (!readSlot(file, dayIndex, units, column)) return false;
  const uint32_t total = units + seconds / SECONDS_PER_UNIT;
  return writeSlot(file, dayIndex, total > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(total), column);
}

bool inWindow(const uint32_t lastDay, const uint32_t dayIndex) {
  return dayIndex <= lastDay && lastDay - dayIndex < DAYS;
}

bool createFile(HalFile& file, const uint32_t lastDay) {
  file = Storage.open(PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("DMIN", "Cannot create %s", PATH);
    return false;
  }
  uint8_t header[HEADER_SIZE] = {};
  memcpy(header, MAGIC, sizeof(MAGIC));
  header[4] = VERSION;
  memcpy(header + LAST_DAY_OFFSET, &lastDay, sizeof(lastDay));
  if (file.write(header, HEADER_SIZE) != HEADER_SIZE) return false;
  constexpr uint16_t empty = 0;
  for (size_t i = 0; i < 2 * DAYS; i++) {
    if (file.write(reinterpret_cast<const uint8_t*>(&empty), SLOT_SIZE) != SLOT_SIZE) return false;
  }
  return true;
}

// Moves the window so it ends on dayIndex, clearing the days it uncovers. A clock that
// jumped back past the whole window (a bad date corrected) would otherwise leave every
// later day out of it until the bad date came around again.
bool advanceWindow(HalFile& file, uint32_t& lastDay, const uint32_t dayIndex) {
  const bool jumpedBack = lastDay >= dayIndex + DAYS;
  if (dayIndex <= lastDay && !jumpedBack) return true;
  const uint32_t first = !jumpedBack && dayIndex - lastDay < DAYS ? lastDay + 1 : dayIndex - DAYS + 1;
  for (uint32_t day = first; day <= dayIndex; day++) {
    if (!writeSlot(file, day, 0) || !writeSlot(file, day, 0, Column::Pending)) return false;
  }
  lastDay = dayIndex;
  return writeLastDay(file, lastDay);
}

void subtractSeconds(ReadingStatsDateTime& dateTime, const uint32_t seconds) {
  const uint32_t intoDay = dateTime.hour * 3600u + dateTime.minute * 60u + dateTime.second;
  uint32_t remainder = 0;
  if (seconds <= intoDay) {
    remainder = intoDay - seconds;
  } else {
    const uint32_t deficit = seconds - intoDay;
    const uint32_t days = (deficit + 86399u) / 86400u;
    addDaysToReadingStatsDate(dateTime.date, -static_cast<int>(days));
    remainder = days * 86400u - deficit;
  }
  dateTime.hour = static_cast<uint8_t>(remainder / 3600u);
  dateTime.minute = static_cast<uint8_t>(remainder % 3600u / 60u);
  dateTime.second = static_cast<uint8_t>(remainder % 60u);
}

bool addToDay(HalFile& file, uint32_t& lastDay, const uint32_t dayIndex, const uint32_t seconds) {
  return advanceWindow(file, lastDay, dayIndex) && addToSlot(file, dayIndex, seconds, Column::Total) &&
         addToSlot(file, dayIndex, seconds, Column::Pending);
}
}  // namespace

void record(const ReadingStatsDateTime* localStart, const uint32_t seconds) {
  if (seconds == 0) return;

  ReadingStatsDateTime cursor;
  if (localStart != nullptr) {
    cursor = *localStart;
  } else {
    if (!getCurrentLocalReadingStatsDateTimeAnyClock(cursor)) return;
    subtractSeconds(cursor, seconds);  // the span just ended
  }
  if (!cursor.isValid()) return;

  uint32_t lastDay = readingStatsDayIndex(cursor.date);
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> buckets{};
  HalFile file = Storage.open(PATH, O_RDWR);
  const bool usable = file && file.size() == FILE_SIZE && readHeader(file, lastDay, buckets);
  if (!usable) {
    if (file) file.close();
    buckets.fill(0);
    if (!createFile(file, lastDay)) {
      if (file) file.close();
      Storage.remove(PATH);
      return;
    }
  }

  uint32_t remaining = seconds;
  while (remaining > 0) {
    // One hour at a time: a segment stays on one day and in one time bucket.
    const uint32_t secondsUntilNextHour = 3600u - (cursor.minute * 60u + cursor.second);
    const uint32_t segment = remaining < secondsUntilNextHour ? remaining : secondsUntilNextHour;
    if (!addToDay(file, lastDay, readingStatsDayIndex(cursor.date), segment)) {
      LOG_ERR("DMIN", "Cannot update %s", PATH);
      break;
    }
    buckets[static_cast<size_t>(readingTimeBucketForHour(cursor.hour))] += segment;
    remaining -= segment;
    addSecondsToReadingStatsDateTime(cursor, segment);
  }
  if (!writeBuckets(file, buckets)) LOG_ERR("DMIN", "Cannot update %s", PATH);
  file.close();
}

bool reset() { return !Storage.exists(PATH) || Storage.remove(PATH); }

Reader::Reader() : file(Storage.open(PATH, O_RDONLY)) {
  valid = file && file.size() == FILE_SIZE && readHeader(file, lastDay, buckets);
}

Reader::~Reader() {
  if (file) file.close();
}

uint16_t Reader::minutes(const uint32_t dayIndex) {
  uint16_t units = 0;
  if (!valid || !inWindow(lastDay, dayIndex) || !readSlot(file, dayIndex, units)) return 0;
  return static_cast<uint16_t>(units * SECONDS_PER_UNIT / 60);
}

uint16_t Reader::pendingMinutes(const uint32_t dayIndex) {
  uint16_t units = 0;
  if (!valid || !inWindow(lastDay, dayIndex) || !readSlot(file, dayIndex, units, Column::Pending)) return 0;
  return static_cast<uint16_t>(units * SECONDS_PER_UNIT / 60);
}

PendingRebuild::PendingRebuild() : file(Storage.open(PATH, O_RDWR)) {
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> buckets{};
  valid = file && file.size() == FILE_SIZE && readHeader(file, lastDay, buckets);
  static constexpr uint8_t zeros[64] = {};
  for (size_t done = 0; valid && done < DAYS * SLOT_SIZE; done += sizeof(zeros)) {
    const size_t chunk = std::min(sizeof(zeros), DAYS * SLOT_SIZE - done);
    valid = file.seekSet(PENDING_OFFSET + done) && file.write(zeros, chunk) == chunk;
  }
  if (file && !valid) LOG_ERR("DMIN", "Cannot reset the pending time in %s", PATH);
}

PendingRebuild::~PendingRebuild() {
  if (file) file.close();
}

void PendingRebuild::add(const uint32_t dayIndex, const uint32_t seconds) {
  if (!valid || seconds == 0 || !inWindow(lastDay, dayIndex)) return;
  if (!addToSlot(file, dayIndex, seconds, Column::Pending)) LOG_ERR("DMIN", "Cannot update %s", PATH);
}

}  // namespace ReadingDailyMinutes
