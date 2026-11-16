#include "BookOrbitDownloadIndex.h"

#include <BookOrbitCredentialStore.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <uzlib.h>

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {

constexpr char INDEX_PATH[] = "/.crosspoint/bookorbit_downloads.bin";
constexpr char TMP_PATH[] = "/.crosspoint/bookorbit_downloads.tmp";
constexpr char BACKUP_PATH[] = "/.crosspoint/bookorbit_downloads.bak";
// 4-byte magic, u32 CRC32 of the server URL, u32 record count, the fixed-size records,
// then every record's path back to back (see docs/file-formats.md). A file whose magic
// or server CRC does not match is discarded: book ids only mean something on the server
// that issued them. BOD1 files (variable-length records) are migrated on first load.
constexpr char INDEX_MAGIC[4] = {'B', 'O', 'D', '2'};
constexpr char LEGACY_MAGIC[4] = {'B', 'O', 'D', '1'};
constexpr size_t HEADER_SIZE = 12;
constexpr size_t DISK_RECORD_SIZE = 22;  // bookId 8 + fileSize 4 + pathHash 4 + pathOffset 4 + pathLen 2
// FIFO eviction past the cap; evicted books fall back to the catalog's filename heuristic
// and flat directory scan. Only the 24-byte records stay in RAM, so the cap costs 12 KB of
// internal RAM on devices without PSRAM, and 48 KB of PSRAM where there is some.
constexpr size_t MAX_ENTRIES_INTERNAL = 512;
constexpr size_t MAX_ENTRIES_PSRAM = 2048;
constexpr size_t LEGACY_MAX_ENTRIES = 128;
constexpr size_t GROWTH_STEP = 32;
constexpr uint16_t PATH_MAX_LEN = 256;

struct Record {
  int64_t bookId;
  uint32_t fileSize;    // size of the downloaded file, for the replacement check
  uint32_t pathHash;    // FNV-1a of the path: a path match reads the SD card only on a hash hit
  uint32_t pathOffset;  // where the path sits in INDEX_PATH
  uint16_t pathLen;
};
static_assert(std::is_trivially_copyable_v<Record>, "records are moved with memcpy/memmove");

// In-RAM records, alive only between the first use and unload(): the catalog activity is
// the sole long-lived user, so this costs nothing outside catalog browsing. A runtime-sized
// array the index grows on demand, so neither stack nor static storage fits.
bool loaded = false;
bool dirty = false;  // records dropped since the last save (stale lookups); saved by unload()
// The file holds entries that could not be loaded (out of memory, failed seek). Saving would
// then rewrite the index with only what is in RAM, so every write is refused until unload().
bool readOnly = false;
HeapByteBuffer storage;
Record* records = nullptr;
size_t count = 0;
size_t capacity = 0;

size_t maxEntries() { return psramHeapAvailable() ? MAX_ENTRIES_PSRAM : MAX_ENTRIES_INTERNAL; }

uint32_t serverCrc() {
  const std::string& url = BOOKORBIT_STORE.getServerUrl();
  return uzlib_crc32(url.data(), static_cast<unsigned int>(url.size()), 0);
}

uint32_t pathHashOf(const std::string& path) {
  uint32_t hash = 2166136261u;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return hash;
}

void releaseRecords() {
  storage.reset();
  records = nullptr;
  count = 0;
  capacity = 0;
}

// Forgets the in-RAM copy without saving; the next use reloads whatever the card holds.
// Used when a save failed and the records' path offsets no longer match the file.
void reset() {
  releaseRecords();
  loaded = false;
  dirty = false;
  readOnly = false;
}

bool reserve(size_t wanted) {
  wanted = std::min(wanted, maxEntries());
  if (wanted <= capacity) return true;
  const size_t bytes = wanted * sizeof(Record);
  HeapByteBuffer grown;
  if (psramHeapAvailable()) grown = makeAlignedByteBufferNoThrow(bytes, MemoryPool::Psram);
  if (!grown) grown = makeAlignedByteBufferNoThrow(bytes);
  if (!grown) {
    LOG_ERR("BODI", "Out of memory for %u download index entries", static_cast<unsigned>(wanted));
    return false;
  }
  if (count > 0) memcpy(grown.get(), records, count * sizeof(Record));
  storage = std::move(grown);
  records = reinterpret_cast<Record*>(storage.get());
  capacity = wanted;
  return true;
}

void eraseAt(const size_t index) {
  memmove(records + index, records + index + 1, (count - index - 1) * sizeof(Record));
  count--;
}

bool readPath(FsFile& file, const Record& record, std::string& out) {
  out.resize(record.pathLen);
  return file.seek(record.pathOffset) && file.read(&out[0], record.pathLen) == static_cast<int>(record.pathLen);
}

// Reads one record's path from the index file.
bool pathAt(const size_t index, std::string& out) {
  FsFile f;
  if (!Storage.openFileForRead("BODI", INDEX_PATH, f)) {
    LOG_ERR("BODI", "Download index missing while reading entry %u", static_cast<unsigned>(index));
    return false;
  }
  const bool ok = readPath(f, records[index], out);
  f.close();
  if (!ok) LOG_ERR("BODI", "Failed to read download index entry %u", static_cast<unsigned>(index));
  return ok;
}

constexpr size_t NOT_FOUND = SIZE_MAX;

size_t findPath(const std::string& path) {
  if (path.size() > PATH_MAX_LEN) return NOT_FOUND;
  const uint32_t hash = pathHashOf(path);
  FsFile f;  // opened on the first hash hit only
  std::string candidate;
  size_t found = NOT_FOUND;
  for (size_t i = 0; i < count && found == NOT_FOUND; i++) {
    if (records[i].pathHash != hash || records[i].pathLen != path.size()) continue;
    if (!f && !Storage.openFileForRead("BODI", INDEX_PATH, f)) {
      LOG_ERR("BODI", "Download index missing while matching a path");
      break;
    }
    if (readPath(f, records[i], candidate) && candidate == path) found = i;
  }
  if (f) f.close();
  return found;
}

// Rewrites the index file from the records. A record's path is copied over from the current
// file, except for pendingIndex's, which comes from pendingPath (a new or moved entry), and
// except when allPaths supplies every path (a BOD1 migration). Writes a temporary file then
// swaps it in through a backup, so a failure leaves the previous index intact; on success
// the records' path offsets point into the new file.
bool save(const std::string* pendingPath = nullptr, const size_t pendingIndex = NOT_FOUND,
          const std::vector<std::string>* allPaths = nullptr) {
  if (readOnly) {
    LOG_ERR("BODI", "Download index not saved: it was not fully loaded");
    return false;
  }
  FsFile out;
  if (!Storage.openFileForWrite("BODI", TMP_PATH, out)) {
    LOG_ERR("BODI", "Failed to open download index for write");
    return false;
  }
  FsFile in;
  const bool copiesPaths = allPaths == nullptr && count > (pendingPath != nullptr ? 1u : 0u);
  if (copiesPaths && !Storage.openFileForRead("BODI", INDEX_PATH, in)) {
    LOG_ERR("BODI", "Download index missing while saving");
    out.close();
    Storage.remove(TMP_PATH);
    return false;
  }

  const uint32_t crc = serverCrc();
  const uint32_t recordCount = static_cast<uint32_t>(count);
  bool ok = out.write(INDEX_MAGIC, sizeof(INDEX_MAGIC)) == sizeof(INDEX_MAGIC) &&
            serialization::tryWritePod(out, crc) && serialization::tryWritePod(out, recordCount);
  const uint32_t blobStart = static_cast<uint32_t>(HEADER_SIZE + count * DISK_RECORD_SIZE);
  uint32_t offset = blobStart;
  for (size_t i = 0; i < count && ok; i++) {
    const Record& r = records[i];
    ok = serialization::tryWritePod(out, r.bookId) && serialization::tryWritePod(out, r.fileSize) &&
         serialization::tryWritePod(out, r.pathHash) && serialization::tryWritePod(out, offset) &&
         serialization::tryWritePod(out, r.pathLen);
    offset += r.pathLen;
  }
  char buffer[PATH_MAX_LEN];  // one path, the longest the format allows
  for (size_t i = 0; i < count && ok; i++) {
    const Record& r = records[i];
    const char* source = buffer;
    if (i == pendingIndex) {
      source = pendingPath->data();
    } else if (allPaths != nullptr) {
      source = (*allPaths)[i].data();
    } else {
      ok = in.seek(r.pathOffset) && in.read(buffer, r.pathLen) == static_cast<int>(r.pathLen);
    }
    ok = ok && out.write(source, r.pathLen) == r.pathLen;
  }
  ok = ok && out.sync();
  out.close();
  if (in) in.close();
  if (!ok) {
    LOG_ERR("BODI", "Failed to write download index");
    Storage.remove(TMP_PATH);
    return false;
  }
  const bool replacing = Storage.exists(INDEX_PATH);
  if (replacing) {
    Storage.remove(BACKUP_PATH);  // leftover from an interrupted save
    if (!Storage.rename(INDEX_PATH, BACKUP_PATH)) {
      LOG_ERR("BODI", "Failed to back up download index");
      Storage.remove(TMP_PATH);
      return false;
    }
  }
  if (!Storage.rename(TMP_PATH, INDEX_PATH)) {
    LOG_ERR("BODI", "Failed to replace download index");
    Storage.remove(TMP_PATH);
    if (replacing) Storage.rename(BACKUP_PATH, INDEX_PATH);
    return false;
  }
  if (replacing) Storage.remove(BACKUP_PATH);

  offset = blobStart;
  for (size_t i = 0; i < count; i++) {
    records[i].pathOffset = offset;
    offset += records[i].pathLen;
  }
  dirty = false;
  return true;
}

// Reads a BOD1 index (magic and server CRC already checked) and rewrites it as BOD2.
// False when the old file could not be converted and must be left alone.
bool migrateLegacy(FsFile& f) {
  std::vector<std::string> paths;
  paths.reserve(LEGACY_MAX_ENTRIES);
  if (!reserve(LEGACY_MAX_ENTRIES)) {
    f.close();
    return false;
  }
  while (count < LEGACY_MAX_ENTRIES) {
    Record r{};
    if (!serialization::tryReadPod(f, r.bookId)) break;  // clean end of file
    if (!serialization::tryReadPod(f, r.fileSize) || !serialization::tryReadPod(f, r.pathLen) || r.pathLen == 0 ||
        r.pathLen > PATH_MAX_LEN) {
      LOG_ERR("BODI", "Legacy download index truncated at entry %u", static_cast<unsigned>(count));
      break;
    }
    std::string path(r.pathLen, '\0');
    if (f.read(&path[0], r.pathLen) != static_cast<int>(r.pathLen)) {
      LOG_ERR("BODI", "Legacy download index truncated in entry %u", static_cast<unsigned>(count));
      break;
    }
    r.pathHash = pathHashOf(path);
    records[count++] = r;
    paths.push_back(std::move(path));
  }
  f.close();
  if (!save(nullptr, NOT_FOUND, &paths)) {
    releaseRecords();  // offsets are meaningless without the rewritten file
    return false;
  }
  LOG_INF("BODI", "Migrated %u download index entries to v2", static_cast<unsigned>(count));
  return true;
}

void ensureLoaded() {
  if (loaded) return;
  loaded = true;
  dirty = false;
  readOnly = false;
  releaseRecords();

  // A save interrupted between its two renames leaves the previous index as the backup.
  if (!Storage.exists(INDEX_PATH) && Storage.exists(BACKUP_PATH)) {
    LOG_INF("BODI", "Restoring download index from backup");
    Storage.rename(BACKUP_PATH, INDEX_PATH);
  }
  FsFile f;
  if (!Storage.openFileForRead("BODI", INDEX_PATH, f)) return;  // no index yet

  char magic[sizeof(INDEX_MAGIC)] = {};
  uint32_t crc = 0;
  // Only the two known layouts count as readable, so a CRC mismatch on them really is
  // another server.
  const bool readable =
      f.read(magic, sizeof(magic)) == static_cast<int>(sizeof(magic)) &&
      (memcmp(magic, INDEX_MAGIC, sizeof(magic)) == 0 || memcmp(magic, LEGACY_MAGIC, sizeof(magic)) == 0) &&
      serialization::tryReadPod(f, crc);
  if (!readable || crc != serverCrc()) {
    f.close();
    LOG_INF("BODI", "Discarding download index (%s)", readable ? "recorded for another server URL" : "unreadable");
    return;
  }
  if (memcmp(magic, LEGACY_MAGIC, sizeof(magic)) == 0) {
    if (!migrateLegacy(f)) readOnly = true;
    return;
  }
  uint32_t stored = 0;
  if (!serialization::tryReadPod(f, stored)) {
    f.close();
    LOG_INF("BODI", "Discarding download index (unreadable)");
    return;
  }

  // A card moved from a PSRAM device can hold more entries than this one keeps: load the
  // newest ones, and let the next save shrink the file.
  const size_t fileSize = f.fileSize();
  const size_t keep = std::min<size_t>(stored, maxEntries());
  const size_t skipped = stored - keep;
  if (keep == 0) {
    f.close();
    return;
  }
  if (!reserve(keep + GROWTH_STEP) || !f.seek(HEADER_SIZE + skipped * DISK_RECORD_SIZE)) {
    LOG_ERR("BODI", "Download index not loaded; leaving it untouched");
    f.close();
    readOnly = true;
    return;
  }
  while (count < keep) {
    Record r{};
    if (!serialization::tryReadPod(f, r.bookId) || !serialization::tryReadPod(f, r.fileSize) ||
        !serialization::tryReadPod(f, r.pathHash) || !serialization::tryReadPod(f, r.pathOffset) ||
        !serialization::tryReadPod(f, r.pathLen) || r.pathLen == 0 || r.pathLen > PATH_MAX_LEN ||
        static_cast<size_t>(r.pathOffset) + r.pathLen > fileSize) {
      LOG_ERR("BODI", "Download index truncated at entry %u", static_cast<unsigned>(count));
      dirty = true;  // rewrite without the unreadable tail
      break;
    }
    records[count++] = r;
  }
  f.close();
  if (skipped > 0) dirty = true;
}

// Size of the file at path, or 0 when it cannot be opened.
uint32_t fileSizeOf(const std::string& path) {
  FsFile f;
  if (!Storage.openFileForRead("BODI", path, f)) return 0;
  const uint32_t size = static_cast<uint32_t>(f.fileSize());
  f.close();
  return size;
}

}  // namespace

bool BookOrbitDownloadIndex::lookup(const int64_t bookId, std::string& outPath) {
  ensureLoaded();
  for (size_t i = 0; i < count; i++) {
    if (records[i].bookId != bookId) continue;
    // The recorded size doubles as an identity check: a different file put at the
    // same path (same-name transfer, re-used filename) usually differs in size.
    std::string path;
    if (pathAt(i, path)) {
      const uint32_t size = fileSizeOf(path);
      if (size != 0 && size == records[i].fileSize) {
        outPath = std::move(path);
        return true;
      }
    }
    // Stale: drop it now, persist on unload() rather than rewriting the file per catalog row.
    eraseAt(i);
    dirty = true;
    return false;
  }
  return false;
}

void BookOrbitDownloadIndex::record(const int64_t bookId, const std::string& path) {
  const uint32_t size = fileSizeOf(path);
  if (size == 0 || path.empty() || path.size() > PATH_MAX_LEN) {
    LOG_ERR("BODI", "Not indexing download at %s", path.c_str());
    return;
  }

  ensureLoaded();
  if (readOnly) {
    LOG_ERR("BODI", "Not indexing download at %s: index not loaded", path.c_str());
    return;
  }
  // One entry per book, and one per path: a re-download replaces the book's old
  // entry, and a book downloaded onto another book's file supersedes that entry.
  for (size_t i = count; i-- > 0;) {
    if (records[i].bookId == bookId) eraseAt(i);
  }
  const size_t samePath = findPath(path);
  if (samePath != NOT_FOUND) eraseAt(samePath);
  if (count >= maxEntries()) eraseAt(0);
  if (count == capacity && !reserve(capacity + GROWTH_STEP)) return;

  records[count] = Record{bookId, size, pathHashOf(path), 0, static_cast<uint16_t>(path.size())};
  count++;
  if (!save(&path, count - 1)) reset();
}

void BookOrbitDownloadIndex::forEachExisting(void (*visit)(int64_t, const std::string&, void*), void* context) {
  if (visit == nullptr) return;
  ensureLoaded();
  if (count == 0) return;
  FsFile index;
  if (!Storage.openFileForRead("BODI", INDEX_PATH, index)) {
    LOG_ERR("BODI", "Download index missing while listing");
    return;
  }
  std::string path;  // reused: one allocation for the whole listing, not per entry
  for (size_t i = 0; i < count; i++) {
    if (!readPath(index, records[i], path)) {
      LOG_ERR("BODI", "Failed to read download index entry %u", static_cast<unsigned>(i));
      break;
    }
    const uint32_t size = fileSizeOf(path);
    if (size == 0 || size != records[i].fileSize) continue;
    visit(records[i].bookId, path, context);
  }
  index.close();
}

bool BookOrbitDownloadIndex::hasPath(const std::string& path) {
  ensureLoaded();
  return findPath(path) != NOT_FOUND;
}

void BookOrbitDownloadIndex::relocate(const std::string& oldPath, const std::string& newPath) {
  const bool wasLoaded = loaded;
  ensureLoaded();
  const size_t index = findPath(oldPath);
  if (index != NOT_FOUND) {
    if (newPath.empty() || newPath.size() > PATH_MAX_LEN) {
      LOG_ERR("BODI", "Dropping download entry, path too long: %s", newPath.c_str());
      eraseAt(index);
      if (!save()) reset();
    } else {
      records[index].pathHash = pathHashOf(newPath);
      records[index].pathLen = static_cast<uint16_t>(newPath.size());
      if (!save(&newPath, index)) reset();
    }
  }
  if (!wasLoaded) unload();
}

uint32_t BookOrbitDownloadIndex::pathHash(const std::string& path) { return pathHashOf(path); }

size_t BookOrbitDownloadIndex::recordedCount() {
  ensureLoaded();
  return count;
}

void BookOrbitDownloadIndex::unload() {
  if (loaded && dirty && !save()) LOG_ERR("BODI", "Dropped stale download entries were not saved");
  reset();
}
