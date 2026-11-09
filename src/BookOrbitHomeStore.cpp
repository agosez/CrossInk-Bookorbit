#include "BookOrbitHomeStore.h"

#include <Arduino.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "BookOrbitStatsQueue.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingDailyMinutes.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "network/BookOrbitCatalogClient.h"

namespace BookOrbitHomeStore {
namespace {
constexpr char PATH[] = "/.crosspoint/bookorbit_home.bin";
constexpr char TMP_PATH[] = "/.crosspoint/bookorbit_home.tmp";
constexpr char COVER_DIR[] = "/.crosspoint/bookorbit_home";
constexpr uint32_t MAGIC = 0x484F4242;  // "BBOH"
constexpr uint16_t VERSION = 2;         // 2: continueTotal
constexpr size_t HEADER_SIZE = 8;       // magic, version, sizeof(BookOrbitHomeSnapshot)
// One boot's worth of covers: the rest comes with the next sync.
constexpr int MAX_COVER_FETCHES = 8;
// Shelves and rows hold 11 covers; the older ones stay until a sync finds more than this.
constexpr size_t MAX_COVER_FILES = 30;
// The sync client's handshake discipline, per cover transfer: wolfSSL churns through
// tens of KB, and the SDK's response headers go into containers that abort on OOM.
constexpr uint32_t MIN_FREE_HEAP_FOR_FETCH = 42 * 1024;
constexpr uint32_t MIN_MAX_ALLOC_FOR_FETCH = 20 * 1024;
constexpr uint32_t MIN_MAX_ALLOC_FOR_DECODE = 54 * 1024;
constexpr uint32_t MIN_PLAUSIBLE_EPOCH = 1700000000;
constexpr char BOOK_STATE_ROOT[] = "/.crosspoint";
constexpr char BOOK_STATE_PREFIX[] = "book_";  // BookContentId::bookStateDirName()
constexpr size_t QUEUE_BATCH = 32;             // 512 bytes of events at a time

bool writeHeader(HalFile& file) {
  uint8_t header[HEADER_SIZE];
  const uint16_t size = sizeof(BookOrbitHomeSnapshot);
  memcpy(header, &MAGIC, sizeof(MAGIC));
  memcpy(header + 4, &VERSION, sizeof(VERSION));
  memcpy(header + 6, &size, sizeof(size));
  return file.write(header, HEADER_SIZE) == HEADER_SIZE;
}

bool readHeader(HalFile& file) {
  uint8_t header[HEADER_SIZE];
  if (file.read(header, HEADER_SIZE) != static_cast<int>(HEADER_SIZE)) return false;
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t size = 0;
  memcpy(&magic, header, sizeof(magic));
  memcpy(&version, header + 4, sizeof(version));
  memcpy(&size, header + 6, sizeof(size));
  return magic == MAGIC && version == VERSION && size == sizeof(BookOrbitHomeSnapshot);
}

void pendingCoverPath(const BookOrbitHomeBook& book, char* out, const size_t outSize) {
  snprintf(out, outSize, "%s/%ld_%08lx.jpg", COVER_DIR, static_cast<long>(book.id),
           static_cast<unsigned long>(book.coverVersion));
}

// Every cover the snapshot shows, with the size it is drawn at.
template <typename Visitor>
void forEachCover(const BookOrbitHomeSnapshot& snapshot, const CoverSizes& sizes, Visitor visit) {
  for (int i = 0; i < snapshot.continueCount; i++) {
    if (snapshot.continueReading[i].hasCover) visit(snapshot.continueReading[i], sizes.rowWidth, sizes.rowHeight);
  }
  for (int i = 0; i < snapshot.discoverCount; i++) {
    if (snapshot.discover[i].hasCover) visit(snapshot.discover[i], sizes.shelfWidth, sizes.shelfHeight);
  }
  for (int i = 0; i < snapshot.recentCount; i++) {
    if (snapshot.recent[i].hasCover) visit(snapshot.recent[i], sizes.shelfWidth, sizes.shelfHeight);
  }
}

void fetchMissingCovers(const BookOrbitHomeSnapshot& snapshot, const StopCheck shouldStop, const CoverSizes& sizes) {
  Storage.mkdir(COVER_DIR);
  int fetched = 0;
  bool stopped = false;
  forEachCover(snapshot, sizes, [&](const BookOrbitHomeBook& book, const int width, const int height) {
    if (stopped || fetched >= MAX_COVER_FETCHES) return;
    if (shouldStop != nullptr && shouldStop()) {
      stopped = true;
      return;
    }
    char path[64];
    coverPath(book, width, height, path, sizeof(path));
    if (Storage.exists(path)) return;
    char pending[64];
    pendingCoverPath(book, pending, sizeof(pending));
    if (Storage.exists(pending)) return;
    if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_FETCH || ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_FOR_FETCH) {
      LOG_INF("BookOrbit", "Leaving covers for the next sync (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      stopped = true;
      return;
    }
    fetched++;
    const auto result = BookOrbitCatalogClient::downloadThumbnail(book.id, pending, shouldStop);
    if (result != HttpDownloader::OK) {
      LOG_ERR("BookOrbit", "Cover of book %ld not fetched (err=%d, http=%d)", static_cast<long>(book.id),
              static_cast<int>(result), HttpDownloader::lastHttpStatus);
      Storage.remove(pending);
    }
  });
}

// Removes covers nothing shows any more, once there are more than a shelf turnover's worth.
void evictCovers(const BookOrbitHomeSnapshot& snapshot, const CoverSizes& sizes) {
  HalFile dir = Storage.open(COVER_DIR);
  if (!dir || !dir.isDirectory()) return;
  std::vector<std::string> names;
  names.reserve(MAX_COVER_FILES + 8);
  char name[48];
  HalFile entry;
  while ((entry = dir.openNextFile())) {
    const size_t length = entry.getName(name, sizeof(name));
    entry.close();
    if (length > 0 && names.size() < MAX_COVER_FILES + 8) names.emplace_back(name);
  }
  dir.close();
  if (names.size() <= MAX_COVER_FILES) return;

  size_t remaining = names.size();
  for (const auto& candidate : names) {
    if (remaining <= MAX_COVER_FILES) break;
    const std::string path = std::string(COVER_DIR) + "/" + candidate;
    bool current = false;
    forEachCover(snapshot, sizes, [&](const BookOrbitHomeBook& book, const int width, const int height) {
      char kept[64];
      coverPath(book, width, height, kept, sizeof(kept));
      char pending[64];
      pendingCoverPath(book, pending, sizeof(pending));
      if (path == kept || path == pending) current = true;
    });
    if (!current && Storage.remove(path.c_str())) remaining--;
  }
}
}  // namespace

void coverPath(const BookOrbitHomeBook& book, const int width, const int height, char* out, const size_t outSize) {
  snprintf(out, outSize, "%s/%ld_%08lx_%dx%d.bmp", COVER_DIR, static_cast<long>(book.id),
           static_cast<unsigned long>(book.coverVersion), width, height);
}

bool load(BookOrbitHomeSnapshot& out) {
  HalFile file;
  if (!Storage.exists(PATH) || !Storage.openFileForRead("BookOrbit", PATH, file)) return false;
  const bool ok =
      readHeader(file) && file.read(reinterpret_cast<uint8_t*>(&out), sizeof(out)) == static_cast<int>(sizeof(out));
  file.close();
  if (!ok) LOG_ERR("BookOrbit", "Ignoring an unreadable Home snapshot");
  return ok;
}

namespace {
// Everything sent before the fetch is in the server's minutes now: what this device read
// and the new snapshot cannot hold is what still waits in a book's upload queue.
void rebuildPendingReading() {
  ReadingDailyMinutes::PendingRebuild pending;
  HalFile root = Storage.open(BOOK_STATE_ROOT);
  if (!root || !root.isDirectory()) return;
  std::vector<BookOrbitStatEvent> batch;
  batch.reserve(QUEUE_BATCH);
  char name[48];
  HalFile entry;
  while ((entry = root.openNextFile())) {
    const bool bookDir = entry.isDirectory() && entry.getName(name, sizeof(name)) > sizeof(BOOK_STATE_PREFIX) - 1 &&
                         strncmp(name, BOOK_STATE_PREFIX, sizeof(BOOK_STATE_PREFIX) - 1) == 0;
    entry.close();
    if (!bookDir) continue;
    const std::string dir = std::string(BOOK_STATE_ROOT) + "/" + name;
    const size_t queued = BookOrbitStatsQueue::queuedCount(dir);
    // Summed per day before adding: the file keeps tens of seconds.
    uint32_t day = 0;
    uint32_t seconds = 0;
    for (size_t first = 0; first < queued; first += QUEUE_BATCH) {
      if (!BookOrbitStatsQueue::readRange(dir, first, QUEUE_BATCH, batch)) break;
      for (const BookOrbitStatEvent& event : batch) {
        ReadingStatsDateTime local;
        if (!readingStatsDateTimeFromUtc(static_cast<time_t>(event.startTime), local)) continue;
        const uint32_t eventDay = readingStatsDayIndex(local.date);
        if (eventDay != day) {
          pending.add(day, seconds);
          day = eventDay;
          seconds = 0;
        }
        seconds += event.durationSeconds;
      }
    }
    pending.add(day, seconds);
  }
  root.close();
}
}  // namespace

bool refresh(const StopCheck shouldStop, const CoverSizes& sizes) {
  // The sleep sync comes here after the book's own exchanges. Everything below is
  // retried by the next sync, so short of heap it can simply wait for one.
  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_FETCH || ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_FOR_FETCH) {
    LOG_INF("BookOrbit", "Leaving the Home snapshot for the next sync (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }

  // Two copies at once: the shelf rule below needs the outgoing one. Both are small and
  // short-lived, and the heap is at its cleanest here, right after a network boot.
  auto next = makeUniqueNoThrow<BookOrbitHomeSnapshot>();
  auto previous = makeUniqueNoThrow<BookOrbitHomeSnapshot>();
  if (!next || !previous) {
    LOG_ERR("BookOrbit", "Cannot allocate the Home snapshot (%u bytes)", static_cast<unsigned>(sizeof(*next) * 2));
    return false;
  }
  const bool hadPrevious = load(*previous);

  HalFile file;
  if (!Storage.openFileForWrite("BookOrbit", TMP_PATH, file)) return false;
  // The year's minutes follow the struct: reserve its place, then come back to it.
  bool ok =
      writeHeader(file) && file.write(reinterpret_cast<const uint8_t*>(next.get()), sizeof(*next)) == sizeof(*next);
  ok = ok && BookOrbitCatalogClient::fetchHomeDashboard(*next, file);
  if (!ok) {
    file.close();
    Storage.remove(TMP_PATH);
    return false;
  }

  const time_t now = time(nullptr);
  next->generatedAt = now >= static_cast<time_t>(MIN_PLAUSIBLE_EPOCH) ? static_cast<uint32_t>(now) : 0;
  ReadingStatsDateTime today;
  next->fetchDay = getCurrentLocalReadingStatsDateTimeAnyClock(today) ? readingStatsDayIndex(today.date) : 0;
  next->localFinishedAtFetch = GlobalReadingStats::load().completedBooks;

  // The server draws Discover anew on every request. Keeping a day's draw stops each sync
  // from costing a new set of covers.
  if (hadPrevious && next->fetchDay != 0 && previous->fetchDay == next->fetchDay && previous->discoverCount > 0) {
    next->discoverCount = previous->discoverCount;
    memcpy(next->discover, previous->discover, sizeof(next->discover));
  }

  // The first page of the recent listing. A failure leaves the shelf as it was.
  BookOrbitBookPage page;
  if (BookOrbitCatalogClient::fetchBooks(BookOrbitCatalogClient::sectionBookQuery("recent"), 1, page)) {
    next->recentCount = 0;
    for (const auto& book : page.books) {
      if (next->recentCount >= BookOrbitHomeSnapshot::SHELF_COUNT) break;
      BookOrbitHomeBook& out = next->recent[next->recentCount++];
      out.id = static_cast<int32_t>(book.id);
      out.hasCover = true;
      snprintf(out.title, sizeof(out.title), "%s", book.title.c_str());
    }
  } else if (hadPrevious) {
    next->recentCount = previous->recentCount;
    memcpy(next->recent, previous->recent, sizeof(next->recent));
  }

  // What Home's "All (N)" opens: the dashboard's own in-progress count takes in every
  // format, the catalog only EPUBs. A failure keeps the last count.
  next->continueTotal =
      BookOrbitCatalogClient::fetchBookTotal(BookOrbitCatalogClient::sectionBookQuery("continue-reading"));
  if (next->continueTotal < 0 && hadPrevious) next->continueTotal = previous->continueTotal;
  previous.reset();

  ok = file.seekSet(HEADER_SIZE) &&
       file.write(reinterpret_cast<const uint8_t*>(next.get()), sizeof(*next)) == sizeof(*next);
  file.close();
  if (!ok) {
    LOG_ERR("BookOrbit", "Cannot write the Home snapshot");
    Storage.remove(TMP_PATH);
    return false;
  }
  Storage.remove(PATH);
  if (!Storage.rename(TMP_PATH, PATH)) {
    LOG_ERR("BookOrbit", "Cannot publish the Home snapshot");
    return false;
  }
  rebuildPendingReading();

  fetchMissingCovers(*next, shouldStop, sizes);
  evictCovers(*next, sizes);
  return true;
}

bool hasPendingCovers() {
  HalFile dir = Storage.open(COVER_DIR);
  if (!dir || !dir.isDirectory()) return false;
  bool pending = false;
  char name[48];
  HalFile entry;
  while (!pending && (entry = dir.openNextFile())) {
    const size_t length = entry.getName(name, sizeof(name));
    entry.close();
    pending = length > 4 && strcmp(name + length - 4, ".jpg") == 0;
  }
  dir.close();
  return pending;
}

void decodePendingCovers(const StopCheck shouldStop, const CoverSizes& sizes) {
  auto snapshot = makeUniqueNoThrow<BookOrbitHomeSnapshot>();
  if (!snapshot) {
    LOG_ERR("BookOrbit", "Cannot allocate the Home snapshot (%u bytes)", static_cast<unsigned>(sizeof(*snapshot)));
    return;
  }
  if (!load(*snapshot)) return;

  forEachCover(*snapshot, sizes, [&](const BookOrbitHomeBook& book, const int width, const int height) {
    if (shouldStop != nullptr && shouldStop()) return;
    char pending[64];
    pendingCoverPath(book, pending, sizeof(pending));
    if (!Storage.exists(pending)) return;
    char path[64];
    coverPath(book, width, height, path, sizeof(path));
    if (ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_FOR_DECODE) {
      LOG_INF("BookOrbit", "Cover decode waits for more heap (maxAlloc=%u)", ESP.getMaxAllocHeap());
      return;
    }
    HalFile jpeg;
    HalFile bmp;
    bool decoded = false;
    if (Storage.openFileForRead("BookOrbit", pending, jpeg)) {
      if (Storage.openFileForWrite("BookOrbit", path, bmp)) {
        decoded = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpeg, bmp, width, height, true);
        bmp.close();
      }
      jpeg.close();
    }
    // With the heap it asked for, a failed decode means a bad file: a retry would fail
    // the same way, and the next sync can fetch it again.
    if (!decoded) {
      LOG_ERR("BookOrbit", "Dropping the cover of book %ld", static_cast<long>(book.id));
      Storage.remove(path);
    }
    Storage.remove(pending);
  });
}

YearReader::YearReader() {
  if (!Storage.exists(PATH) || !Storage.openFileForRead("BookOrbit", PATH, file) || !readHeader(file)) return;
  uint32_t first = 0;
  uint16_t count = 0;
  if (file.seekSet(HEADER_SIZE + offsetof(BookOrbitHomeSnapshot, yearFirstDay)) &&
      file.read(reinterpret_cast<uint8_t*>(&first), sizeof(first)) == static_cast<int>(sizeof(first)) &&
      file.seekSet(HEADER_SIZE + offsetof(BookOrbitHomeSnapshot, yearDayCount)) &&
      file.read(reinterpret_cast<uint8_t*>(&count), sizeof(count)) == static_cast<int>(sizeof(count))) {
    firstDay = first;
    dayCount = count;
  }
}

YearReader::~YearReader() {
  if (file) file.close();
}

uint16_t YearReader::minutes(const uint32_t dayIndex) {
  if (dayCount == 0 || dayIndex < firstDay || dayIndex - firstDay >= dayCount) return 0;
  uint16_t value = 0;
  const size_t offset = HEADER_SIZE + sizeof(BookOrbitHomeSnapshot) + (dayIndex - firstDay) * sizeof(value);
  if (!file.seekSet(offset) ||
      file.read(reinterpret_cast<uint8_t*>(&value), sizeof(value)) != static_cast<int>(sizeof(value))) {
    return 0;
  }
  return value;
}

}  // namespace BookOrbitHomeStore
