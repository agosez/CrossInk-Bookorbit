#include "BookOrbitDownloadIndex.h"

#include <BookOrbitCredentialStore.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <uzlib.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr char INDEX_PATH[] = "/.crosspoint/bookorbit_downloads.bin";
constexpr char TMP_PATH[] = "/.crosspoint/bookorbit_downloads.tmp";
constexpr char BACKUP_PATH[] = "/.crosspoint/bookorbit_downloads.bak";
// 4-byte magic, u32 CRC32 of the server URL, u32 record count, the fixed-size records,
// then every record's path back to back (see docs/file-formats.md). A file whose magic
// or server CRC does not match is discarded: book ids only mean something on the server
// that issued them. BOD1 files (variable-length records) are migrated on first use.
constexpr char INDEX_MAGIC[4] = {'B', 'O', 'D', '2'};
constexpr char LEGACY_MAGIC[4] = {'B', 'O', 'D', '1'};
constexpr size_t HEADER_SIZE = 12;
constexpr size_t LEGACY_HEADER_SIZE = 8;
constexpr size_t DISK_RECORD_SIZE = 22;  // bookId 8 + fileSize 4 + pathHash 4 + pathOffset 4 + pathLen 2
// FIFO eviction past the cap; evicted books fall back to the catalog's filename heuristic
// and flat directory scan. No RAM grows with it: what does is each pass (22 bytes a record,
// 45 KB at the cap) and the On device listing, which opens every recorded book.
constexpr uint32_t MAX_ENTRIES = 2048;
constexpr uint32_t LEGACY_MAX_ENTRIES = 128;
constexpr uint16_t PATH_MAX_LEN = 256;
// Records read per SD call. SdFat serves small reads from its sector cache, so this only
// saves calls. The 176-byte chunk lives in the Reader, on the stack of whichever call runs:
// with the file handle, a little over 256 bytes, for as long as one call.
constexpr size_t CHUNK_RECORDS = 8;
constexpr uint32_t NONE = UINT32_MAX;

struct Record {
  int64_t bookId;
  uint32_t fileSize;    // size of the downloaded file, for the replacement check
  uint32_t pathHash;    // FNV-1a of the path: a path match reads the SD card only on a hash hit
  uint32_t pathOffset;  // where the path sits in INDEX_PATH
  uint16_t pathLen;
};

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

// Size of the file at path, or 0 when it cannot be opened.
uint32_t fileSizeOf(const std::string& path) {
  FsFile f;
  if (!Storage.openFileForRead("BODI", path, f)) return 0;
  const uint32_t size = static_cast<uint32_t>(f.fileSize());
  f.close();
  return size;
}

template <typename T>
T podAt(const uint8_t* bytes) {
  T value;
  memcpy(&value, bytes, sizeof(T));  // records are packed: no aligned access
  return value;
}

// A save writes TMP_PATH and swaps it in through a backup, so a failure at any step leaves
// the previous index in place (open() restores the backup after a power loss).
bool replaceIndex() {
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
  return true;
}

// One BOD1 entry: false at the end of the file, and at the first entry cut short.
bool readLegacyEntry(FsFile& in, int64_t& bookId, uint32_t& size, std::string& path) {
  uint16_t length = 0;
  if (!serialization::tryReadPod(in, bookId)) return false;  // clean end of file
  if (!serialization::tryReadPod(in, size) || !serialization::tryReadPod(in, length) || length == 0 ||
      length > PATH_MAX_LEN) {
    LOG_ERR("BODI", "Legacy download index truncated");
    return false;
  }
  path.resize(length);
  if (in.read(&path[0], length) != static_cast<int>(length)) {
    LOG_ERR("BODI", "Legacy download index truncated in a path");
    return false;
  }
  return true;
}

// Rewrites a BOD1 index (magic and server CRC already checked) as BOD2, in three passes over
// it: count, records, paths. False when it could not be converted and must be left alone.
bool migrateLegacy() {
  FsFile in;
  if (!Storage.openFileForRead("BODI", INDEX_PATH, in)) return false;
  int64_t bookId = 0;
  uint32_t size = 0;
  std::string path;
  uint32_t count = 0;
  bool ok = in.seek(LEGACY_HEADER_SIZE);
  while (ok && count < LEGACY_MAX_ENTRIES && readLegacyEntry(in, bookId, size, path)) count++;

  FsFile out;
  if (!ok || !Storage.openFileForWrite("BODI", TMP_PATH, out)) {
    LOG_ERR("BODI", "Failed to open download index for migration");
    in.close();
    return false;
  }
  ok = out.write(INDEX_MAGIC, sizeof(INDEX_MAGIC)) == sizeof(INDEX_MAGIC) &&
       serialization::tryWritePod(out, serverCrc()) && serialization::tryWritePod(out, count) &&
       in.seek(LEGACY_HEADER_SIZE);
  uint32_t offset = static_cast<uint32_t>(HEADER_SIZE + count * DISK_RECORD_SIZE);
  for (uint32_t i = 0; i < count && ok; i++) {
    ok = readLegacyEntry(in, bookId, size, path);
    const uint16_t length = static_cast<uint16_t>(path.size());
    ok = ok && serialization::tryWritePod(out, bookId) && serialization::tryWritePod(out, size) &&
         serialization::tryWritePod(out, pathHashOf(path)) && serialization::tryWritePod(out, offset) &&
         serialization::tryWritePod(out, length);
    offset += length;
  }
  ok = ok && in.seek(LEGACY_HEADER_SIZE);
  for (uint32_t i = 0; i < count && ok; i++) {
    ok = readLegacyEntry(in, bookId, size, path) && out.write(path.data(), path.size()) == path.size();
  }
  ok = ok && out.sync();
  out.close();
  in.close();
  if (!ok) {
    LOG_ERR("BODI", "Failed to migrate download index");
    Storage.remove(TMP_PATH);
    return false;
  }
  if (!replaceIndex()) return false;
  LOG_INF("BODI", "Migrated %u download index entries to v2", static_cast<unsigned>(count));
  return true;
}

// The index, open for one call. Records come in file order, oldest first, from the first one
// within the cap; a path can be read between two of them, as each chunk of records is read
// from its own offset. A record pointing outside the file is skipped, and so dropped by the
// next rewrite.
class Reader {
 public:
  enum class State : uint8_t { Missing, Discarded, Open, Failed };

  Reader() = default;
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  ~Reader() { close(); }

  State open() {
    close();
    readFailed = false;
    fileSize = 0;
    first = end = 0;
    rewind();
    // A save interrupted between its two renames leaves the previous index as the backup.
    if (!Storage.exists(INDEX_PATH) && Storage.exists(BACKUP_PATH)) {
      LOG_INF("BODI", "Restoring download index from backup");
      Storage.rename(BACKUP_PATH, INDEX_PATH);
    }
    for (int attempt = 0; attempt < 2; attempt++) {
      if (!Storage.openFileForRead("BODI", INDEX_PATH, file)) return State::Missing;
      char magic[sizeof(INDEX_MAGIC)] = {};
      uint32_t crc = 0;
      // Only the two known layouts count as readable, so a CRC mismatch on them really is
      // another server.
      const bool readable =
          file.read(magic, sizeof(magic)) == static_cast<int>(sizeof(magic)) &&
          (memcmp(magic, INDEX_MAGIC, sizeof(magic)) == 0 || memcmp(magic, LEGACY_MAGIC, sizeof(magic)) == 0) &&
          serialization::tryReadPod(file, crc);
      if (!readable || crc != serverCrc()) {
        file.close();
        LOG_INF("BODI", "Discarding download index (%s)", readable ? "recorded for another server URL" : "unreadable");
        return State::Discarded;
      }
      if (memcmp(magic, LEGACY_MAGIC, sizeof(magic)) == 0) {
        file.close();
        if (attempt > 0 || !migrateLegacy()) return State::Failed;
        continue;  // reopen the rewritten file
      }
      uint32_t stored = 0;
      if (!serialization::tryReadPod(file, stored)) {
        file.close();
        LOG_INF("BODI", "Discarding download index (unreadable)");
        return State::Discarded;
      }
      fileSize = file.fileSize();
      const size_t present = fileSize > HEADER_SIZE ? (fileSize - HEADER_SIZE) / DISK_RECORD_SIZE : 0;
      if (stored > present) {
        LOG_ERR("BODI", "Download index truncated at entry %u of %u", static_cast<unsigned>(present),
                static_cast<unsigned>(stored));
        stored = static_cast<uint32_t>(present);
      }
      end = stored;
      // A card moved from a device that kept more entries: its oldest ones are past the cap.
      first = stored > MAX_ENTRIES ? stored - MAX_ENTRIES : 0;
      rewind();
      return State::Open;
    }
    return State::Failed;
  }

  void close() {
    if (file) file.close();
  }
  bool isOpen() const { return static_cast<bool>(file); }
  bool failed() const { return readFailed; }
  uint32_t recordCount() const { return end - first; }

  void rewind() {
    chunkBase = first;
    buffered = 0;
    cursor = 0;
  }

  // The next readable record; false at the end, and on a read error (failed() then says so).
  bool next(uint32_t& index, Record& record) {
    if (!file) return false;
    for (;;) {
      if (cursor == buffered) {
        const uint32_t base = chunkBase + static_cast<uint32_t>(buffered);
        if (base >= end) return false;
        const size_t count = std::min<size_t>(CHUNK_RECORDS, end - base);
        const size_t bytes = count * DISK_RECORD_SIZE;
        if (!file.seek(HEADER_SIZE + static_cast<size_t>(base) * DISK_RECORD_SIZE) ||
            file.read(chunk, bytes) != static_cast<int>(bytes)) {
          LOG_ERR("BODI", "Failed to read download index at entry %u", static_cast<unsigned>(base));
          readFailed = true;
          return false;
        }
        chunkBase = base;
        buffered = count;
        cursor = 0;
      }
      const uint8_t* bytes = chunk + cursor * DISK_RECORD_SIZE;
      index = chunkBase + static_cast<uint32_t>(cursor++);
      record.bookId = podAt<int64_t>(bytes);
      record.fileSize = podAt<uint32_t>(bytes + 8);
      record.pathHash = podAt<uint32_t>(bytes + 12);
      record.pathOffset = podAt<uint32_t>(bytes + 16);
      record.pathLen = podAt<uint16_t>(bytes + 20);
      if (record.pathLen > 0 && record.pathLen <= PATH_MAX_LEN &&
          static_cast<size_t>(record.pathOffset) + record.pathLen <= fileSize) {
        return true;
      }
    }
  }

  bool readPath(const Record& record, std::string& out) {
    out.resize(record.pathLen);
    if (file && file.seek(record.pathOffset) &&
        file.read(&out[0], record.pathLen) == static_cast<int>(record.pathLen)) {
      return true;
    }
    LOG_ERR("BODI", "Failed to read a download index path");
    readFailed = true;
    return false;
  }

  // Whether the record's path is exactly path: the hash and length first, the card only
  // on a hit.
  bool pathIs(const Record& record, const std::string& path, const uint32_t hash, std::string& scratch) {
    return record.pathHash == hash && record.pathLen == path.size() && readPath(record, scratch) && scratch == path;
  }

 private:
  FsFile file;
  size_t fileSize = 0;
  uint32_t first = 0;
  uint32_t end = 0;
  uint32_t chunkBase = 0;
  size_t buffered = 0;
  size_t cursor = 0;
  bool readFailed = false;
  uint8_t chunk[CHUNK_RECORDS * DISK_RECORD_SIZE] = {};
};

struct Addition {
  int64_t bookId;
  uint32_t fileSize;
  uint32_t pathHash;
  const std::string* path;
};

// What a rewrite changes: the records it leaves out besides those past the cap, one record's
// new path, and the entries it appends, newest last.
struct Edit {
  bool (*drops)(uint32_t index, const Record& record, Reader& in, const void* context) = nullptr;
  const void* context = nullptr;
  uint32_t renamed = NONE;
  const std::string* renamedPath = nullptr;
  const Addition* additions = nullptr;
  size_t additionCount = 0;
};

// Rewrites the index from `in` (or from nothing when it is not open) with the edit applied,
// in three passes over its records: which stay, their records, their paths. The edit's drops
// answers the same on each, so they agree. Closes `in`. False, with the previous index left
// in place, when it could not be written.
bool rewrite(Reader& in, const Edit& edit) {
  const bool fromFile = in.isOpen();
  const auto dropped = [&](const uint32_t index, const Record& record) {
    return edit.drops != nullptr && edit.drops(index, record, in, edit.context);
  };
  uint32_t index = 0;
  Record record{};
  uint32_t kept = 0;
  if (fromFile) {
    in.rewind();
    while (in.next(index, record)) {
      if (!dropped(index, record)) kept++;
    }
  }
  const size_t added = std::min<size_t>(edit.additionCount, MAX_ENTRIES);
  const Addition* additions = edit.additions + (edit.additionCount - added);
  const uint32_t evicted = kept + added > MAX_ENTRIES ? static_cast<uint32_t>(kept + added - MAX_ENTRIES) : 0;
  const uint32_t total = kept - evicted + static_cast<uint32_t>(added);

  if (in.failed()) {
    in.close();
    return false;  // logged by the Reader
  }
  FsFile out;
  if (!Storage.openFileForWrite("BODI", TMP_PATH, out)) {
    LOG_ERR("BODI", "Failed to open download index for write");
    in.close();
    return false;
  }
  bool ok = out.write(INDEX_MAGIC, sizeof(INDEX_MAGIC)) == sizeof(INDEX_MAGIC) &&
            serialization::tryWritePod(out, serverCrc()) && serialization::tryWritePod(out, total);
  uint32_t offset = static_cast<uint32_t>(HEADER_SIZE + static_cast<size_t>(total) * DISK_RECORD_SIZE);
  const auto writeRecord = [&](const int64_t bookId, const uint32_t size, const uint32_t hash, const size_t length) {
    const uint16_t pathLen = static_cast<uint16_t>(length);
    ok = ok && serialization::tryWritePod(out, bookId) && serialization::tryWritePod(out, size) &&
         serialization::tryWritePod(out, hash) && serialization::tryWritePod(out, offset) &&
         serialization::tryWritePod(out, pathLen);
    offset += pathLen;
  };
  uint32_t ordinal = 0;
  if (fromFile) {
    in.rewind();
    while (ok && in.next(index, record)) {
      if (dropped(index, record) || ordinal++ < evicted) continue;
      if (index == edit.renamed) {
        writeRecord(record.bookId, record.fileSize, pathHashOf(*edit.renamedPath), edit.renamedPath->size());
      } else {
        writeRecord(record.bookId, record.fileSize, record.pathHash, record.pathLen);
      }
    }
  }
  for (size_t i = 0; i < added; i++) {
    writeRecord(additions[i].bookId, additions[i].fileSize, additions[i].pathHash, additions[i].path->size());
  }
  std::string path;  // reused: one allocation for every copied path
  ordinal = 0;
  if (fromFile) {
    in.rewind();
    while (ok && in.next(index, record)) {
      if (dropped(index, record) || ordinal++ < evicted) continue;
      const std::string* source = &path;
      if (index == edit.renamed) {
        source = edit.renamedPath;
      } else {
        ok = in.readPath(record, path);
      }
      ok = ok && out.write(source->data(), source->size()) == source->size();
    }
  }
  for (size_t i = 0; i < added && ok; i++) {
    ok = out.write(additions[i].path->data(), additions[i].path->size()) == additions[i].path->size();
  }
  ok = ok && !in.failed() && out.sync();
  out.close();
  in.close();
  if (!ok) {
    LOG_ERR("BODI", "Failed to write download index");
    Storage.remove(TMP_PATH);
    return false;
  }
  return replaceIndex();
}

// lookupAll()'s answer for one book asked about: its newest record (a book has one, unless
// the file was edited by hand), and whether that record's file is gone.
struct Match {
  uint32_t index = NONE;
  Record record{};
  bool stale = false;
};

struct Matches {
  const Match* matches;
  size_t count;
};

bool dropsStale(const uint32_t index, const Record&, Reader&, const void* context) {
  const auto& set = *static_cast<const Matches*>(context);
  for (size_t i = 0; i < set.count; i++) {
    if (set.matches[i].stale && set.matches[i].index == index) return true;
  }
  return false;
}

struct Additions {
  const Addition* additions;
  size_t count;
};

// The entries a batch of downloads supersedes: the same book, or the same file.
bool dropsSuperseded(const uint32_t, const Record& record, Reader& in, const void* context) {
  const auto& set = *static_cast<const Additions*>(context);
  std::string path;  // allocated on a hash hit only
  for (size_t i = 0; i < set.count; i++) {
    const Addition& added = set.additions[i];
    if (added.bookId == record.bookId || in.pathIs(record, *added.path, added.pathHash, path)) return true;
  }
  return false;
}

bool dropsIndex(const uint32_t index, const Record&, Reader&, const void* context) {
  return index == *static_cast<const uint32_t*>(context);
}

}  // namespace

void BookOrbitDownloadIndex::lookupAll(const int64_t* bookIds, const size_t n,
                                       void (*found)(size_t i, const std::string& path, void* context), void* context) {
  if (bookIds == nullptr || n == 0 || found == nullptr) return;
  Reader in;
  if (in.open() != Reader::State::Open) return;
  // n is a page of books at most: runtime-sized, so neither stack nor static storage fits.
  auto matches = makeUniqueNoThrow<Match[]>(n);
  if (!matches) {
    LOG_ERR("BODI", "Out of memory looking up %u books", static_cast<unsigned>(n));
    return;
  }
  uint32_t index = 0;
  Record record{};
  while (in.next(index, record)) {
    for (size_t i = 0; i < n; i++) {
      if (bookIds[i] != record.bookId) continue;
      matches[i].index = index;
      matches[i].record = record;
    }
  }
  if (in.failed()) return;

  bool anyStale = false;
  std::string path;
  for (size_t i = 0; i < n; i++) {
    Match& match = matches[i];
    if (match.index == NONE || !in.readPath(match.record, path)) continue;
    // The recorded size doubles as an identity check: a different file put at the
    // same path (same-name transfer, re-used filename) usually differs in size.
    const uint32_t size = fileSizeOf(path);
    if (size != 0 && size == match.record.fileSize) {
      found(i, path, context);
    } else {
      match.stale = true;
      anyStale = true;
    }
  }
  if (!anyStale || in.failed()) return;
  const Matches stale{matches.get(), n};
  Edit edit;
  edit.drops = dropsStale;
  edit.context = &stale;
  if (!rewrite(in, edit)) LOG_ERR("BODI", "Dropped stale download entries were not saved");
}

bool BookOrbitDownloadIndex::lookup(const int64_t bookId, std::string& outPath) {
  struct Answer {
    std::string* path;
    bool found;
  } answer{&outPath, false};
  lookupAll(
      &bookId, 1,
      [](size_t, const std::string& path, void* context) {
        auto& out = *static_cast<Answer*>(context);
        *out.path = path;
        out.found = true;
      },
      &answer);
  return answer.found;
}

void BookOrbitDownloadIndex::recordAll(const Download* downloads, const size_t n) {
  if (downloads == nullptr || n == 0) return;
  // A batch of the device scan at most: runtime-sized, so neither stack nor static storage fits.
  auto additions = makeUniqueNoThrow<Addition[]>(n);
  if (!additions) {
    LOG_ERR("BODI", "Out of memory recording %u downloads", static_cast<unsigned>(n));
    return;
  }
  size_t count = 0;
  for (size_t i = 0; i < n; i++) {
    const std::string& path = *downloads[i].path;
    const uint32_t size = fileSizeOf(path);
    if (size == 0 || path.empty() || path.size() > PATH_MAX_LEN) {
      LOG_ERR("BODI", "Not indexing download at %s", path.c_str());
      continue;
    }
    // Within the batch too, the last word on a book or a file wins.
    size_t kept = 0;
    for (size_t k = 0; k < count; k++) {
      if (additions[k].bookId == downloads[i].bookId || *additions[k].path == path) continue;
      additions[kept++] = additions[k];
    }
    count = kept;
    additions[count++] = Addition{downloads[i].bookId, size, pathHashOf(path), &path};
  }
  if (count == 0) return;

  Reader in;
  if (in.open() == Reader::State::Failed) {
    LOG_ERR("BODI", "Not indexing %u downloads: the index could not be read", static_cast<unsigned>(count));
    return;
  }
  const Additions superseded{additions.get(), count};
  Edit edit;
  edit.drops = dropsSuperseded;
  edit.context = &superseded;
  edit.additions = additions.get();
  edit.additionCount = count;
  if (!rewrite(in, edit)) LOG_ERR("BODI", "%u downloads were not indexed", static_cast<unsigned>(count));
}

void BookOrbitDownloadIndex::record(const int64_t bookId, const std::string& path) {
  const Download download{bookId, &path};
  recordAll(&download, 1);
}

void BookOrbitDownloadIndex::findPaths(const std::string* paths, const size_t n, bool* outClaimed) {
  if (paths == nullptr || outClaimed == nullptr || n == 0) return;
  std::fill_n(outClaimed, n, false);
  Reader in;
  if (in.open() != Reader::State::Open) return;
  // A batch of the device scan at most: runtime-sized, so neither stack nor static storage fits.
  auto hashes = makeUniqueNoThrow<uint32_t[]>(n);
  if (!hashes) {
    LOG_ERR("BODI", "Out of memory matching %u paths", static_cast<unsigned>(n));
    return;
  }
  for (size_t i = 0; i < n; i++) hashes[i] = pathHashOf(paths[i]);
  uint32_t index = 0;
  Record record{};
  std::string scratch;
  while (in.next(index, record)) {
    for (size_t i = 0; i < n; i++) {
      if (!outClaimed[i] && in.pathIs(record, paths[i], hashes[i], scratch)) outClaimed[i] = true;
    }
  }
}

bool BookOrbitDownloadIndex::hasPath(const std::string& path) {
  bool claimed = false;
  findPaths(&path, 1, &claimed);
  return claimed;
}

void BookOrbitDownloadIndex::forEachExisting(void (*visit)(int64_t, const std::string&, void*), void* context) {
  if (visit == nullptr) return;
  Reader in;
  if (in.open() != Reader::State::Open) return;
  uint32_t index = 0;
  Record record{};
  std::string path;  // reused: one allocation for the whole listing, not per entry
  while (in.next(index, record)) {
    if (!in.readPath(record, path)) break;
    const uint32_t size = fileSizeOf(path);
    if (size == 0 || size != record.fileSize) continue;
    visit(record.bookId, path, context);
  }
}

bool BookOrbitDownloadIndex::bookIdForPath(const std::string& path, int64_t& outBookId) {
  Reader in;
  if (in.open() != Reader::State::Open) return false;
  const uint32_t hash = pathHashOf(path);
  uint32_t index = 0;
  Record record{};
  std::string scratch;
  bool found = false;
  while (in.next(index, record)) {
    if (!in.pathIs(record, path, hash, scratch)) continue;
    outBookId = record.bookId;
    found = true;
  }
  return found;
}

void BookOrbitDownloadIndex::relocate(const std::string& oldPath, const std::string& newPath) {
  Reader in;
  if (in.open() != Reader::State::Open) return;
  const uint32_t hash = pathHashOf(oldPath);
  uint32_t index = 0;
  Record record{};
  std::string scratch;
  uint32_t moved = NONE;
  while (in.next(index, record)) {
    if (in.pathIs(record, oldPath, hash, scratch)) moved = index;
  }
  if (moved == NONE || in.failed()) return;
  Edit edit;
  if (newPath.empty() || newPath.size() > PATH_MAX_LEN) {
    LOG_ERR("BODI", "Dropping download entry, path too long: %s", newPath.c_str());
    edit.drops = dropsIndex;
    edit.context = &moved;
  } else {
    edit.renamed = moved;
    edit.renamedPath = &newPath;
  }
  if (!rewrite(in, edit)) LOG_ERR("BODI", "Moved download not followed: %s", newPath.c_str());
}

uint32_t BookOrbitDownloadIndex::pathHash(const std::string& path) { return pathHashOf(path); }

size_t BookOrbitDownloadIndex::recordedCount() {
  Reader in;
  return in.open() == Reader::State::Open ? in.recordCount() : 0;
}
