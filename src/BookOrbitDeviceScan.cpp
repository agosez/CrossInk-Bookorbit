#include "BookOrbitDeviceScan.h"

#include <Arduino.h>
#include <BookOrbitCredentialStore.h>
#include <BookOrbitSyncClient.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <strings.h>
#include <uzlib.h>

#include <cstring>
#include <string>

#include "BookOrbitDownloadIndex.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "network/BookOrbitCatalogClient.h"
#include "util/BookMoveUtils.h"
#include "util/BookOrbitDevicePath.h"

namespace {

// prepare() -> run(): the books to ask about (see docs/file-formats.md).
constexpr char WORK_PATH[] = "/.crosspoint/bookorbit_scan.bin";
// Between passes: the last Library arrival handled, the server's library token, and the
// files the server did not recognise.
constexpr char STATE_PATH[] = "/.crosspoint/bookorbit_recognition.bin";
constexpr char STATE_TMP_PATH[] = "/.crosspoint/bookorbit_recognition.tmp";
constexpr char WORK_MAGIC[4] = {'B', 'O', 'S', '2'};
constexpr char STATE_MAGIC[4] = {'B', 'O', 'R', '1'};
constexpr char READ_FOLDER[] = "/Read";
constexpr uint16_t PATH_MAX_LEN = 256;
constexpr size_t HASH_LEN = 32;
// Books per match check: the batch and its request stay within a few KB beside the TLS
// session on an X3/X4, and a wake press waits for at most one batch.
constexpr size_t BATCH = 12;
// Library books looking for new ones are checked against the download index this many at a
// time, one pass over it each (see BookOrbitDownloadIndex).
constexpr size_t CANDIDATES = 32;
constexpr uint16_t FIRST_SEEN_SATURATED = 0xFFFE;  // the Library's arrival counter stops there

// Books hashed per pass, which bounds the time a sync spends on a card full of new books;
// the next pass carries on. Files remembered as unrecognised, at 8 bytes of RAM each while
// prepare() runs.
size_t maxNewPerPass() { return psramHeapAvailable() ? 1000 : 200; }
size_t maxRemembered() { return psramHeapAvailable() ? 8192 : 1024; }

#pragma pack(push, 1)
struct WorkHeader {
  char magic[4];
  uint16_t watermark;  // the state's watermark once every record has been handled
  uint16_t reserved;
  uint32_t count;
};
struct StateHeader {
  char magic[4];
  uint32_t serverCrc;
  uint16_t watermark;  // Library arrival number below which every book has been handled
  uint16_t reserved;
  char libraryVersion[HASH_LEN];  // the server's token when the remembered files were last asked about
  uint32_t count;
};
// One file, in both files, followed by its path, title and author bytes (the state keeps no
// title or author).
struct Record {
  char hash[HASH_LEN];  // KOReader partial MD5, lowercase hex
  uint32_t size;
  uint32_t pathHash;  // BookOrbitDownloadIndex::pathHash()
  uint16_t pathLen;
  uint8_t titleLen;
  uint8_t authorLen;
};
#pragma pack(pop)

uint32_t serverCrc() {
  const std::string& url = BOOKORBIT_STORE.getServerUrl();
  return uzlib_crc32(url.data(), static_cast<unsigned int>(url.size()), 0);
}

// Opens the state and leaves it positioned on its first record. A missing file, or one
// written for another server, reads as a fresh state.
bool openState(FsFile& file, StateHeader& header) {
  header = StateHeader{};
  if (!Storage.openFileForRead("BookOrbit", STATE_PATH, file)) return false;
  if (file.read(&header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
      memcmp(header.magic, STATE_MAGIC, sizeof(STATE_MAGIC)) == 0 && header.serverCrc == serverCrc()) {
    return true;
  }
  file.close();
  header = StateHeader{};
  return false;
}

bool readRecord(FsFile& file, Record& record, std::string& path, std::string* title, std::string* author) {
  if (file.read(&record, sizeof(record)) != static_cast<int>(sizeof(record)) || record.pathLen == 0 ||
      record.pathLen > PATH_MAX_LEN) {
    return false;
  }
  path.resize(record.pathLen);
  if (file.read(&path[0], record.pathLen) != static_cast<int>(record.pathLen)) return false;
  std::string ignored;
  std::string& t = title != nullptr ? *title : ignored;
  std::string& a = author != nullptr ? *author : ignored;
  t.resize(record.titleLen);
  a.resize(record.authorLen);
  return (record.titleLen == 0 || file.read(&t[0], record.titleLen) == static_cast<int>(record.titleLen)) &&
         (record.authorLen == 0 || file.read(&a[0], record.authorLen) == static_cast<int>(record.authorLen));
}

// Up to 255 bytes, cut on a UTF-8 character boundary.
uint8_t fieldLength(const std::string& text) {
  size_t len = std::min<size_t>(text.size(), 255);
  if (len < text.size()) {
    while (len > 0 && (static_cast<uint8_t>(text[len]) & 0xC0) == 0x80) len--;
  }
  return static_cast<uint8_t>(len);
}

bool writeRecord(FsFile& file, Record record, const std::string& path, const std::string& title,
                 const std::string& author) {
  record.pathLen = static_cast<uint16_t>(path.size());
  record.titleLen = fieldLength(title);
  record.authorLen = fieldLength(author);
  return file.write(&record, sizeof(record)) == sizeof(record) &&
         file.write(path.data(), record.pathLen) == record.pathLen &&
         file.write(title.data(), record.titleLen) == record.titleLen &&
         file.write(author.data(), record.authorLen) == record.authorLen;
}

uint32_t fileSizeOf(const std::string& path) {
  FsFile f;
  if (!Storage.openFileForRead("BookOrbit", path, f)) return 0;
  const uint32_t size = static_cast<uint32_t>(f.fileSize());
  f.close();
  return size;
}

bool (*buildCancelledBy)() = nullptr;
bool buildCancelled(void*) { return buildCancelledBy != nullptr && buildCancelledBy(); }

void report(const BookOrbitDeviceScan::Progress* progress, const BookOrbitDeviceScan::Step step, const size_t done,
            const size_t total) {
  if (progress != nullptr && progress->report != nullptr) progress->report(progress->context, step, done, total);
}

void reportBuild(void* context, const library::BuildProgress& build) {
  report(static_cast<const BookOrbitDeviceScan::Progress*>(context),
         build.phase == library::BuildPhase::Organizing ? BookOrbitDeviceScan::Step::SortingCard
                                                        : BookOrbitDeviceScan::Step::ReadingCard,
         build.books, 0);
}

// --- prepare() ---------------------------------------------------------------------------

struct Known {
  uint32_t pathHash;
  uint32_t size;
};

// Rebuilds the Library index when the card may have changed since it was built, as the
// Library itself would on its next visit; a cancelled or failed build leaves the previous one.
bool openLibraryIndex(library::LibraryIndexFile& index, bool (*cancelled)(),
                      const BookOrbitDeviceScan::Progress* progress) {
  const bool metadata = SETTINGS.libraryUseMetadata != 0;
  if (!library::libraryIndexNeedsRefresh() && index.open(library::libraryIndexPath()) &&
      index.header().metadataEnabled == static_cast<uint8_t>(metadata)) {
    return true;
  }
  index.close();
  const unsigned long startMs = millis();
  library::BuildStats stats;
  library::BuildCallbacks callbacks;
  callbacks.context = const_cast<BookOrbitDeviceScan::Progress*>(progress);
  callbacks.cancelRequested = buildCancelled;
  callbacks.progress = reportBuild;
  report(progress, BookOrbitDeviceScan::Step::ReadingCard, 0, 0);
  buildCancelledBy = cancelled;
  const bool built = library::buildLibraryIndex("/", stats, metadata, &callbacks);
  buildCancelledBy = nullptr;
  LOG_INF("BookOrbit", "Device scan: Library index rebuilt (ok=%d, %u books, %lu ms)", built,
          static_cast<unsigned>(stats.books), millis() - startMs);
  return built && index.open(library::libraryIndexPath());
}

}  // namespace

void BookOrbitDeviceScan::prepare(bool (*cancelled)(), const Progress* progress) {
  Storage.remove(WORK_PATH);
  if (!BOOKORBIT_STORE.hasCredentials()) return;
  const unsigned long startMs = millis();
  library::LibraryIndexFile index;
  if (!openLibraryIndex(index, cancelled, progress)) return;

  FsFile stateFile;
  StateHeader state;
  const bool haveState = openState(stateFile, state);
  const uint16_t next = index.header().nextFirstSeen;
  if (state.watermark == next && next < FIRST_SEEN_SATURATED) {
    if (haveState) stateFile.close();
    return;  // nothing arrived since the last pass
  }
  if (state.watermark > next) state.watermark = 0;  // the Library index started over

  // The files the server did not recognise, so that a pass cut short does not ask again.
  const size_t knownCap = std::min<size_t>(state.count, maxRemembered());
  auto known = makeUniqueNoThrow<Known[]>(knownCap > 0 ? knownCap : 1);
  size_t knownCount = 0;
  if (haveState) {
    Record record;
    std::string path;
    while (known && knownCount < knownCap && readRecord(stateFile, record, path, nullptr, nullptr)) {
      known[knownCount++] = Known{record.pathHash, record.size};
    }
    stateFile.close();
  }

  FsFile work;
  WorkHeader header{};
  memcpy(header.magic, WORK_MAGIC, sizeof(WORK_MAGIC));
  if (!Storage.openFileForWrite("BookOrbit", WORK_PATH, work) ||
      work.write(&header, sizeof(header)) != sizeof(header)) {
    LOG_ERR("BookOrbit", "Device scan cannot write %s", WORK_PATH);
    work.close();
    return;
  }
  // Bounds each path lookup's walk through the folder list; without it, lookups start at
  // the first folder.
  auto checkpoints = makeUniqueNoThrow<uint32_t[]>(library::LibraryIndexFile::FOLDER_CHECKPOINT_COUNT);
  uint16_t stride = 0;
  if (checkpoints) index.buildFolderCheckpoints(checkpoints.get(), stride);

  // A Library book that may be new: arrived since the last pass, an EPUB, not one the server
  // already did not recognise. Its record is read again for the title once it is hashed.
  struct Candidate {
    uint16_t ordinal;
    uint16_t firstSeen;
    uint32_t fileSize;
    uint32_t pathHash;
  };
  struct Candidates {
    Candidate items[CANDIDATES];
    std::string paths[CANDIDATES];
    bool claimed[CANDIDATES];
    size_t count;
  };
  // A batch of paths: heap, not the loop task's 8 KB stack.
  auto batch = makeUniqueNoThrow<Candidates>();
  bool ok = static_cast<bool>(batch);
  if (!ok) LOG_ERR("BookOrbit", "Device scan out of memory");

  uint16_t watermark = next;  // lowered to the first arrival left for the next pass
  std::string title;
  std::string author;
  library::ClixRecord entry;
  const uint16_t books = index.bookCount();
  report(progress, Step::Hashing, 0, books);
  for (uint16_t i = 0; i < books && ok;) {
    batch->count = 0;
    for (; i < books && batch->count < CANDIDATES; i++) {
      if (cancelled != nullptr && cancelled()) {
        ok = false;
        break;
      }
      std::string& path = batch->paths[batch->count];
      if (!index.readRecord(i, entry) || entry.firstSeen < state.watermark) continue;
      if (!index.readPath(entry, path, checkpoints.get(), stride) || path.size() > PATH_MAX_LEN ||
          !FsHelpers::hasEpubExtension(path)) {
        continue;
      }
      const uint32_t pathHash = BookOrbitDownloadIndex::pathHash(path);
      bool asked = false;
      for (size_t k = 0; k < knownCount && !asked; k++) {
        asked = known[k].pathHash == pathHash && known[k].size == entry.fileSize;
      }
      if (asked) continue;
      batch->items[batch->count++] = Candidate{i, entry.firstSeen, entry.fileSize, pathHash};
    }
    if (!ok) break;
    // A file the download index holds is a catalog book already in place: not hashed.
    BookOrbitDownloadIndex::findPaths(batch->paths, batch->count, batch->claimed);
    for (size_t k = 0; k < batch->count && ok; k++) {
      const Candidate& candidate = batch->items[k];
      if (batch->claimed[k]) continue;
      if (header.count >= maxNewPerPass()) {
        if (candidate.firstSeen < watermark) watermark = candidate.firstSeen;
        continue;
      }
      if (cancelled != nullptr && cancelled()) {
        ok = false;
        break;
      }
      const std::string& path = batch->paths[k];
      const std::string hash = KOReaderDocumentId::calculate(path);
      if (hash.size() != HASH_LEN || !index.readRecord(candidate.ordinal, entry) ||
          !index.readDisplayText(entry, title, author)) {
        LOG_ERR("BookOrbit", "Device scan could not read %s", path.c_str());
        continue;
      }
      Record record{};
      memcpy(record.hash, hash.data(), HASH_LEN);
      record.size = candidate.fileSize;
      record.pathHash = candidate.pathHash;
      ok = writeRecord(work, record, path, title, author);
      header.count++;
      report(progress, Step::Hashing, candidate.ordinal + 1u, books);
    }
  }
  header.watermark = watermark;
  ok = ok && work.seek(0) && work.write(&header, sizeof(header)) == sizeof(header);
  work.close();
  if (!ok) {
    Storage.remove(WORK_PATH);  // cancelled or failed: the next pass starts over
    return;
  }
  LOG_INF("BookOrbit", "Device scan: %u new books of %u in the Library (arrivals %u..%u, %lu ms)",
          static_cast<unsigned>(header.count), static_cast<unsigned>(books), static_cast<unsigned>(state.watermark),
          static_cast<unsigned>(next), millis() - startMs);
}

// --- run() -------------------------------------------------------------------------------

namespace {

struct Item {
  Record record;
  char hash[HASH_LEN + 1];
  std::string path;
  std::string title;  // empty for a remembered file, asked about with its bare hash
  std::string author;
  int64_t bookId;
  int64_t fileId;
  // Where the download index holds the book, asked for the whole batch before filing it.
  bool indexed;
  std::string indexedPath;
};

struct Run {
  bool (*cancelled)();
  const BookOrbitDeviceScan::Progress* progress = nullptr;
  size_t progressDone = 0;
  size_t progressTotal = 0;
  Item items[BATCH];
  size_t count = 0;
  // The batch's books the download index gains, recorded together once it is filed.
  int64_t filedIds[BATCH] = {};
  std::string filedPaths[BATCH];
  size_t filedCount = 0;
  FsFile state;  // STATE_TMP_PATH
  uint32_t remembered = 0;
  char version[BookOrbitSyncClient::LIBRARY_VERSION_SIZE] = {};
  bool failed = false;  // cancelled, the server could not be asked, or a file could not be written
  size_t asked = 0;
  size_t matched = 0;
  size_t moved = 0;
  size_t duplicates = 0;

  bool stopped() {
    if (!failed && cancelled != nullptr && cancelled()) {
      LOG_INF("BookOrbit", "Device scan cancelled");
      failed = true;
    }
    return failed;
  }
};

void remember(Run& run, const Item& item) {
  if (run.remembered >= maxRemembered()) return;
  if (!writeRecord(run.state, item.record, item.path, std::string(), std::string())) {
    run.failed = true;
    return;
  }
  run.remembered++;
}

bool inReadFolder(const std::string& path) {
  const size_t len = sizeof(READ_FOLDER) - 1;
  return path.compare(0, len, READ_FOLDER) == 0 && (path.size() == len || path[len] == '/');
}

// Moves the book along with everything kept under its path (cache, bookmarks, Recent Books
// entry, open-book state), as the file browser's moves do.
bool moveBook(const std::string& from, const std::string& to) {
  const size_t slash = to.find_last_of('/');
  const std::string folder = slash > 0 ? to.substr(0, slash) : std::string();
  if (!folder.empty() && !Storage.exists(folder.c_str()) && !Storage.mkdir(folder.c_str())) {
    LOG_ERR("BookOrbit", "Failed to create the folder of %s", to.c_str());
    return false;
  }
  const auto result = BookMoveUtils::renameFilePreservingBookState(from, to);
  if (result != BookMoveUtils::RenameMigrationResult::Success &&
      result != BookMoveUtils::RenameMigrationResult::KeepRenamed) {
    LOG_ERR("BookOrbit", "Failed to move %s -> %s (%d)", from.c_str(), to.c_str(), static_cast<int>(result));
    return false;
  }
  LOG_INF("BookOrbit", "Moved %s -> %s", from.c_str(), to.c_str());
  return true;
}

// Deletes a misplaced copy of a book already at its catalog location. Only what is keyed by
// this path goes with it (the file, its layout cache, its Recent Books entry): bookmarks,
// highlights and the reading position are kept by content, so they are the other copy's too
// and must stay. The reader takes the position from there when it opens the other copy.
bool removeCopy(const std::string& path, const std::string& kept) {
  if (!Storage.remove(path.c_str())) {
    LOG_ERR("BookOrbit", "Failed to remove duplicate %s", path.c_str());
    return false;
  }
  const std::string cachePath = Epub::cachePathForFilePath(path, "/.crosspoint");
  if (Storage.exists(cachePath.c_str())) Storage.removeDir(cachePath.c_str());
  RECENT_BOOKS.removeByPath(path);
  if (APP_STATE.openEpubPath == path) {
    APP_STATE.openEpubPath = kept;
    APP_STATE.saveToFile();
  }
  LOG_INF("BookOrbit", "Removed duplicate %s (kept %s)", path.c_str(), kept.c_str());
  return true;
}

// Puts a recognised book where the catalog would have downloaded it, and returns where it
// now is. A copy the download index already holds, or one already at that place, is the one
// kept.
std::string place(Run& run, const Item& item, const BookOrbitBookDetail& detail, const BookOrbitCatalogFile& file) {
  if (item.indexed) {
    const std::string& kept = item.indexedPath;
    if (kept == item.path || !removeCopy(item.path, kept)) return item.path;
    run.duplicates++;
    return kept;
  }
  const std::string target = BOOKORBIT_STORE.getDownloadFolder() +
                             BookOrbitDevicePath::catalogFilename(file.devicePath, detail.title, detail.author);
  // Paths differing only by case name the same file on FAT: already in place.
  if (target.size() > PATH_MAX_LEN || strcasecmp(target.c_str(), item.path.c_str()) == 0) return item.path;
  if (Storage.exists(target.c_str())) {
    // Another book under that name: both stay.
    if (KOReaderDocumentId::calculate(target) != item.hash || !removeCopy(item.path, target)) return item.path;
    run.duplicates++;
    return target;
  }
  if (!moveBook(item.path, target)) return item.path;
  run.moved++;
  return target;
}

// Files a recognised book and adds it to the download index where it ends up. The naming
// template is only resolved in the book's catalog detail: a book the catalog hides, or a
// failed request, leaves it indexed where it is, as does /Read.
void file(Run& run, Item& item) {
  std::string where = item.path;
  BookOrbitBookDetail detail;
  if (!inReadFolder(item.path) && BookOrbitCatalogClient::fetchBookDetail(item.bookId, detail)) {
    for (const BookOrbitCatalogFile& candidate : detail.files) {
      if (item.fileId != 0 ? candidate.id != item.fileId : candidate.format != "epub") continue;
      where = place(run, item, detail, candidate);
      break;
    }
  }
  // A copy the index already holds stays the book's entry (the batch's lookup dropped an
  // entry whose file is gone). Another copy of the same book later in the batch now finds
  // this one, as it would in the index.
  if (item.indexed || run.filedCount >= BATCH) return;
  run.filedIds[run.filedCount] = item.bookId;
  run.filedPaths[run.filedCount++] = where;
  for (size_t i = 0; i < run.count; i++) {
    Item& other = run.items[i];
    if (other.bookId != item.bookId || other.indexed) continue;
    other.indexed = true;
    other.indexedPath = where;
  }
}

// Where the download index holds the batch's recognised books, in one pass over it.
void lookUpIndexed(Run& run) {
  int64_t ids[BATCH];
  uint8_t at[BATCH];
  size_t count = 0;
  for (size_t i = 0; i < run.count; i++) {
    Item& item = run.items[i];
    item.indexed = false;
    std::string().swap(item.indexedPath);
    if (item.bookId == 0) continue;
    ids[count] = item.bookId;
    at[count++] = static_cast<uint8_t>(i);
  }
  struct Answer {
    Run* run;
    const uint8_t* at;
  } answer{&run, at};
  BookOrbitDownloadIndex::lookupAll(
      ids, count,
      [](const size_t i, const std::string& path, void* context) {
        const auto& to = *static_cast<Answer*>(context);
        Item& item = to.run->items[to.at[i]];
        item.indexed = true;
        item.indexedPath = path;
      },
      &answer);
}

// Records where the batch's books were filed, in one rewrite of the download index.
void recordFiled(Run& run) {
  BookOrbitDownloadIndex::Download downloads[BATCH];
  for (size_t i = 0; i < run.filedCount; i++) downloads[i] = {run.filedIds[i], &run.filedPaths[i]};
  BookOrbitDownloadIndex::recordAll(downloads, run.filedCount);
  run.filedCount = 0;
}

void onMatch(const char* hash, const int64_t bookId, const int64_t bookFileId, void* context) {
  auto& run = *static_cast<Run*>(context);
  for (size_t i = 0; i < run.count; i++) {
    if (strcasecmp(run.items[i].hash, hash) != 0) continue;
    run.items[i].bookId = bookId;
    run.items[i].fileId = bookFileId;
  }
}

bool ask(Run& run) {
  BookOrbitSyncClient::MatchQuery queries[BATCH];
  for (size_t i = 0; i < run.count; i++) {
    const Item& item = run.items[i];
    queries[i] = {item.hash, item.title.empty() ? nullptr : item.title.c_str(), item.author.c_str()};
  }
  const auto error = BookOrbitSyncClient::matchCheck(SETTINGS.getEffectiveDeviceName(), queries, run.count, onMatch,
                                                     &run, run.version);
  if (error == BookOrbitSyncClient::OK) return true;
  LOG_ERR("BookOrbit", "Match check failed (error=%d, http=%d)", static_cast<int>(error),
          BookOrbitSyncClient::lastHttpCode);
  return false;
}

// Asks the server about the batch, remembers what it does not recognise and files the rest.
// False when the server could not be asked or a file could not be written.
bool checkBatch(Run& run) {
  if (!ask(run)) {
    run.failed = true;
    return false;
  }
  run.asked += run.count;
  lookUpIndexed(run);
  for (size_t i = 0; i < run.count; i++) {
    Item& item = run.items[i];
    if (item.bookId == 0) {
      remember(run, item);
    } else {
      run.matched++;
      file(run, item);
    }
    report(run.progress, BookOrbitDeviceScan::Step::Filing, ++run.progressDone, run.progressTotal);
  }
  recordFiled(run);
  return !run.failed;
}

// Reads the next batch of records. A remembered file about to be asked about again is
// skipped, and so forgotten, when it changed or went away: prepare() finds it again if it
// is still in the Library.
bool readBatch(Run& run, FsFile& file, uint32_t& left, const bool fresh, const bool asking) {
  run.count = 0;
  while (run.count < BATCH && left > 0) {
    Item& item = run.items[run.count];
    if (!readRecord(file, item.record, item.path, &item.title, &item.author)) {
      LOG_ERR("BookOrbit", "Device scan record unreadable");
      return false;
    }
    left--;
    if (!fresh && asking && fileSizeOf(item.path) != item.record.size) continue;
    memcpy(item.hash, item.record.hash, HASH_LEN);
    item.hash[HASH_LEN] = '\0';
    item.bookId = 0;
    item.fileId = 0;
    run.count++;
  }
  return true;
}

// Goes through a file's records in batches, asking the server about each when `asking`.
// A record left unasked (not asking, cancelled, server unreachable) is kept when
// remembered and dropped when fresh, for prepare() to find again. True when every record
// was asked about.
bool checkFile(Run& run, FsFile& file, uint32_t left, const bool fresh, bool asking) {
  bool complete = true;
  while (left > 0) {
    if (!readBatch(run, file, left, fresh, asking)) return false;
    if (asking && run.count > 0 && !run.stopped() && checkBatch(run)) continue;
    if (asking && run.count > 0) complete = asking = false;
    for (size_t i = 0; i < run.count && !fresh; i++) remember(run, run.items[i]);
  }
  return complete;
}

// With no new book to learn the server's token from: asks about the first remembered files
// only, and says whether the rest is worth asking about (the token moved since they were
// last asked about, or one of these now resolves).
bool rememberedMayResolve(Run& run, FsFile& file, const StateHeader& state) {
  uint32_t left = state.count;
  if (!readBatch(run, file, left, false, true) || run.count == 0 || !ask(run)) return false;
  for (size_t i = 0; i < run.count; i++) {
    if (run.items[i].bookId != 0) return true;
  }
  return strncmp(run.version, state.libraryVersion, HASH_LEN) != 0;
}

}  // namespace

BookOrbitDeviceScan::Summary BookOrbitDeviceScan::run(bool (*cancelled)(), const Progress* progress) {
  const unsigned long startMs = millis();
  StateHeader state;
  {
    FsFile header;  // only the header for now: each phase below opens the file it reads
    if (openState(header, state)) header.close();
  }
  FsFile work;
  WorkHeader workHeader{};
  const bool haveWork = Storage.openFileForRead("BookOrbit", WORK_PATH, work) &&
                        work.read(&workHeader, sizeof(workHeader)) == static_cast<int>(sizeof(workHeader)) &&
                        memcmp(workHeader.magic, WORK_MAGIC, sizeof(WORK_MAGIC)) == 0;
  if (!haveWork && work) work.close();
  if (!haveWork && state.count == 0) return {};

  // A dozen paths and titles: heap, not the loop task's 8 KB stack.
  auto run = makeUniqueNoThrow<Run>();
  if (!run) {
    LOG_ERR("BookOrbit", "Device scan out of memory");
    if (haveWork) work.close();
    return {};
  }
  run->cancelled = cancelled;
  run->progress = progress;
  if (!haveWork) {
    // The usual case once the card is known: nothing new, so nothing is written.
    FsFile remembered;
    const bool worthIt = openState(remembered, state) && rememberedMayResolve(*run, remembered, state);
    if (remembered) remembered.close();
    if (!worthIt) return {};
  }

  StateHeader next = state;
  memcpy(next.magic, STATE_MAGIC, sizeof(STATE_MAGIC));
  next.serverCrc = serverCrc();
  next.count = 0;
  if (!Storage.openFileForWrite("BookOrbit", STATE_TMP_PATH, run->state) ||
      run->state.write(&next, sizeof(next)) != sizeof(next)) {
    LOG_ERR("BookOrbit", "Device scan cannot write %s", STATE_TMP_PATH);
    run->state.close();
    if (haveWork) work.close();
    return {};
  }

  // New books first: their answers carry the server's token, which tells whether the
  // remembered files are worth asking about again.
  if (haveWork) {
    run->progressTotal = workHeader.count;
    report(progress, Step::Filing, 0, run->progressTotal);
  }
  const bool workDone = haveWork && checkFile(*run, work, workHeader.count, true, true);
  if (haveWork) work.close();
  const bool reask =
      !haveWork || (run->version[0] != '\0' && strncmp(run->version, state.libraryVersion, HASH_LEN) != 0);
  bool rememberedDone = true;
  if (state.count > 0) {
    FsFile remembered;
    if (openState(remembered, state)) {
      if (reask) {
        run->progressDone = 0;
        run->progressTotal = state.count;
        report(progress, Step::Filing, 0, run->progressTotal);
      }
      rememberedDone = checkFile(*run, remembered, state.count, false, reask);
      remembered.close();
    }
  }

  if (run->moved > 0 || run->duplicates > 0) library::invalidateLibraryIndex();

  if (workDone) next.watermark = workHeader.watermark;
  // A cut-short re-ask keeps the old token, so the next pass asks again.
  if (run->version[0] != '\0' && (!reask || rememberedDone)) memcpy(next.libraryVersion, run->version, HASH_LEN);
  next.count = run->remembered;
  const bool saved = run->state.seek(0) && run->state.write(&next, sizeof(next)) == sizeof(next);
  run->state.close();
  if (saved) {
    Storage.remove(STATE_PATH);
    Storage.rename(STATE_TMP_PATH, STATE_PATH);
  } else {
    LOG_ERR("BookOrbit", "Device scan cannot write %s", STATE_PATH);
    Storage.remove(STATE_TMP_PATH);
  }
  Storage.remove(WORK_PATH);
  LOG_INF("BookOrbit", "Device scan: asked=%u matched=%u moved=%u duplicates=%u remembered=%u%s (%lu ms)",
          static_cast<unsigned>(run->asked), static_cast<unsigned>(run->matched), static_cast<unsigned>(run->moved),
          static_cast<unsigned>(run->duplicates), static_cast<unsigned>(run->remembered),
          workDone || !haveWork ? "" : " incomplete", millis() - startMs);
  return Summary{run->matched, run->moved, run->duplicates};
}
