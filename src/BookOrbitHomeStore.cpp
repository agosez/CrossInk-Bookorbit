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

#include "BookOrbitCredentialStore.h"
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
constexpr uint16_t VERSION = 3;         // 2: continueTotal, 3: shelves chosen in the settings
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
constexpr char OPTIONS_PATH[] = "/.crosspoint/bookorbit_home_shelves.bin";
constexpr char OPTIONS_TMP_PATH[] = "/.crosspoint/bookorbit_home_shelves.tmp";
constexpr uint32_t OPTIONS_MAGIC = 0x53424248;  // "HBBS"
constexpr uint16_t OPTIONS_VERSION = 1;
struct OptionsHeader {
  uint32_t magic = OPTIONS_MAGIC;
  uint16_t version = OPTIONS_VERSION;
  uint16_t count = 0;
};
static_assert(sizeof(OptionsHeader) == 8, "bookorbit_home_shelves.bin starts with 8 bytes");

static_assert(BookOrbitHomeSnapshot::SHELVES == BookOrbitCredentialStore::HOME_SHELF_COUNT,
              "Every shelf the settings choose needs its place in the snapshot");
static_assert(sizeof(BookOrbitHomeShelfSource) == 67, "bookorbit_home_shelves.bin holds 67-byte entries");

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
  for (const BookOrbitHomeShelf& shelf : snapshot.shelves) {
    for (int i = 0; i < shelf.count; i++) {
      if (shelf.books[i].hasCover) visit(shelf.books[i], sizes.shelfWidth, sizes.shelfHeight);
    }
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

namespace {
template <size_t N>
void copyText(char (&out)[N], const char* text) {
  snprintf(out, N, "%s", text != nullptr ? text : "");
}

// A shelf of the outgoing snapshot that listed the same source, wherever it sat.
const BookOrbitHomeShelf* findShelf(const BookOrbitHomeSnapshot& snapshot, const BookOrbitHomeShelfSource& source) {
  for (const BookOrbitHomeShelf& shelf : snapshot.shelves) {
    if (shelf.source.sameAs(source)) return &shelf;
  }
  return nullptr;
}

// A collection or SmartScope lists its newest books first: a shelf is a glimpse of what
// changed, where the catalog lists them by title.
BookOrbitBookQuery shelfQuery(const BookOrbitHomeShelfSource& source) {
  BookOrbitBookQuery query = BookOrbitCatalogClient::sectionBookQuery(
      source.kind == BookOrbitHomeShelfKind::WANT_TO_READ ? BookOrbitCatalogClient::WANT_TO_READ_SHELF : "recent");
  if (source.kind == BookOrbitHomeShelfKind::COLLECTION) query.collectionId = source.id;
  if (source.kind == BookOrbitHomeShelfKind::SMART_SCOPE) query.smartScopeId = source.id;
  return query;
}

// The first books of a shelf's listing. Discover is the dashboard's to fill.
bool fetchShelf(BookOrbitHomeShelf& shelf) {
  BookOrbitBookPage page;
  if (!BookOrbitCatalogClient::fetchBooks(shelfQuery(shelf.source), 1, page)) return false;
  shelf.count = 0;
  for (const auto& book : page.books) {
    if (shelf.count >= BookOrbitHomeShelf::BOOK_COUNT) break;
    BookOrbitHomeBook& out = shelf.books[shelf.count++];
    out.id = static_cast<int32_t>(book.id);
    out.hasCover = true;
    copyText(out.title, book.title.c_str());
  }
  return true;
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

  // Each shelf lists what the settings chose; the dashboard below fills the Discover ones.
  for (int i = 0; i < BookOrbitHomeSnapshot::SHELVES; i++) next->shelves[i].source = BOOKORBIT_STORE.getHomeShelf(i);

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

  for (BookOrbitHomeShelf& shelf : next->shelves) {
    const BookOrbitHomeShelf* kept = hadPrevious ? findShelf(*previous, shelf.source) : nullptr;
    // The server draws Discover anew on every request. Keeping a day's draw stops each
    // sync from costing a new set of covers. A failed listing leaves its shelf as it was.
    const bool keep = shelf.source.kind == BookOrbitHomeShelfKind::DISCOVER
                          ? next->fetchDay != 0 && previous->fetchDay == next->fetchDay
                          : !fetchShelf(shelf);
    if (keep && kept != nullptr && kept->count > 0) {
      shelf.count = kept->count;
      memcpy(shelf.books, kept->books, sizeof(shelf.books));
    }
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

bool shelvesFollowSettings() {
  auto snapshot = makeUniqueNoThrow<BookOrbitHomeSnapshot>();
  if (!snapshot) {
    LOG_ERR("BookOrbit", "Cannot allocate the Home snapshot (%u bytes)", static_cast<unsigned>(sizeof(*snapshot)));
    return true;
  }
  if (!load(*snapshot)) return true;
  for (int i = 0; i < BookOrbitHomeSnapshot::SHELVES; i++) {
    if (!snapshot->shelves[i].source.sameAs(BOOKORBIT_STORE.getHomeShelf(i))) return false;
  }
  return true;
}

bool refreshShelfOptions() {
  // Written as the pages arrive, so no list of them waits in RAM beside the TLS session;
  // the count goes into the header at the end, and the old file stays until then.
  HalFile file;
  if (!Storage.openFileForWrite("BookOrbit", OPTIONS_TMP_PATH, file)) return false;
  OptionsHeader header;
  bool ok = file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header);

  const struct {
    const char* section;
    BookOrbitHomeShelfKind kind;
  } lists[] = {{"collections", BookOrbitHomeShelfKind::COLLECTION},
               {"smart-scopes", BookOrbitHomeShelfKind::SMART_SCOPE}};
  uint16_t& count = header.count;
  BookOrbitFacetPage page;
  for (const auto& list : lists) {
    for (int number = 1; ok && count < MAX_SHELF_OPTIONS; number++) {
      ok = BookOrbitCatalogClient::fetchSectionEntries(list.section, number, page);
      for (size_t i = 0; ok && i < page.entries.size() && count < MAX_SHELF_OPTIONS; i++) {
        const BookOrbitFacetEntry& entry = page.entries[i];
        if (entry.id.empty()) continue;
        BookOrbitHomeShelfSource option;
        option.kind = list.kind;
        copyText(option.id, entry.id.c_str());
        copyText(option.name, entry.title.c_str());
        ok = file.write(reinterpret_cast<const uint8_t*>(&option), sizeof(option)) == sizeof(option);
        count++;
      }
      if (!page.hasNext) break;
    }
  }
  ok = ok && file.seekSet(0) && file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header);
  file.close();
  if (ok) {
    Storage.remove(OPTIONS_PATH);
    ok = Storage.rename(OPTIONS_TMP_PATH, OPTIONS_PATH);
  }
  if (!ok) {
    LOG_ERR("BookOrbit", "Shelf options not refreshed (http=%d)", HttpDownloader::lastHttpStatus);
    Storage.remove(OPTIONS_TMP_PATH);
    return false;
  }
  LOG_INF("BookOrbit", "Saved %u shelf options", static_cast<unsigned>(count));
  return true;
}

bool loadShelfOptions(std::vector<BookOrbitHomeShelfSource>& out) {
  out.clear();
  HalFile file;
  if (!Storage.exists(OPTIONS_PATH) || !Storage.openFileForRead("BookOrbit", OPTIONS_PATH, file)) return false;
  OptionsHeader header;
  bool ok = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == static_cast<int>(sizeof(header)) &&
            header.magic == OPTIONS_MAGIC && header.version == OPTIONS_VERSION && header.count <= MAX_SHELF_OPTIONS;
  if (ok) {
    std::vector<BookOrbitHomeShelfSource> loaded(header.count);
    const size_t bytes = header.count * sizeof(BookOrbitHomeShelfSource);
    ok = file.read(reinterpret_cast<uint8_t*>(loaded.data()), bytes) == static_cast<int>(bytes);
    out.swap(loaded);
  }
  file.close();
  if (!ok) {
    LOG_ERR("BookOrbit", "Ignoring an unreadable list of shelf options");
    out.clear();
  }
  return ok;
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
