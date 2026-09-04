#include "BookOrbitBookmarkStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

namespace {
constexpr char STORE_FILENAME[] = "/bookorbit_bookmarks.bin";
// 4-byte magic + 4-byte watermark, then one length-prefixed record per bookmark. A file whose
// magic does not match is discarded on read and rewritten on the next put; the reader
// re-stamps positions as chapters are visited.
constexpr char STORE_MAGIC[4] = {'B', 'O', 'B', '1'};
constexpr size_t POS_MAX = 512;

std::string storePath(const std::string& bookCachePath) { return bookCachePath + STORE_FILENAME; }

bool readFile(const std::string& path, uint32_t& outWatermark, std::vector<BookOrbitBookmarkRecord>& out) {
  outWatermark = 0;
  out.clear();

  FsFile f;
  if (!Storage.openFileForRead("BOB", path, f)) return false;

  char magic[sizeof(STORE_MAGIC)] = {};
  if (f.read(magic, sizeof(magic)) != static_cast<int>(sizeof(magic)) ||
      memcmp(magic, STORE_MAGIC, sizeof(magic)) != 0 || !serialization::tryReadPod(f, outWatermark)) {
    f.close();
    LOG_ERR("BOB", "Discarding unreadable bookmark store: %s", path.c_str());
    outWatermark = 0;
    return false;
  }

  while (out.size() < BookOrbitBookmarkStore::MAX_RECORDS) {
    BookOrbitBookmarkRecord record;
    uint16_t posLength = 0;
    if (!serialization::tryReadPod(f, record.timestamp)) break;  // clean end of file
    if (!serialization::tryReadPod(f, record.identityEpoch) || !serialization::tryReadPod(f, record.spineIndex) ||
        !serialization::tryReadPod(f, posLength) || posLength == 0 || posLength > POS_MAX) {
      LOG_ERR("BOB", "Bookmark store truncated at record %u: %s", (unsigned)out.size(), path.c_str());
      break;
    }
    record.pos.resize(posLength);
    if (f.read(&record.pos[0], posLength) != static_cast<int>(posLength)) {
      LOG_ERR("BOB", "Bookmark store truncated in record %u: %s", (unsigned)out.size(), path.c_str());
      break;
    }
    out.push_back(std::move(record));
  }

  f.close();
  return true;
}

bool writeFile(const std::string& path, const uint32_t watermark, const std::vector<BookOrbitBookmarkRecord>& records) {
  FsFile f;
  if (!Storage.openFileForWrite("BOB", path, f)) {
    LOG_ERR("BOB", "Failed to open bookmark store for write: %s", path.c_str());
    return false;
  }

  bool ok = f.write(STORE_MAGIC, sizeof(STORE_MAGIC)) == static_cast<int>(sizeof(STORE_MAGIC)) &&
            serialization::tryWritePod(f, watermark);
  for (const BookOrbitBookmarkRecord& record : records) {
    if (!ok) break;
    const uint16_t posLength = static_cast<uint16_t>(std::min(record.pos.size(), POS_MAX));
    if (posLength == 0) continue;
    ok = serialization::tryWritePod(f, record.timestamp) && serialization::tryWritePod(f, record.identityEpoch) &&
         serialization::tryWritePod(f, record.spineIndex) && serialization::tryWritePod(f, posLength) &&
         f.write(record.pos.data(), posLength) == static_cast<int>(posLength);
  }

  f.close();
  if (!ok) LOG_ERR("BOB", "Failed to write bookmark store: %s", path.c_str());
  return ok;
}
}  // namespace

bool BookOrbitBookmarkStore::put(const std::string& bookCachePath, const BookOrbitBookmarkRecord& record) {
  if (record.timestamp == 0 || record.identityEpoch == 0 || record.pos.empty()) {
    LOG_ERR("BOB", "Refusing to store a bookmark record with no timestamp or position");
    return false;
  }
  if (bookCachePath.empty()) {
    LOG_ERR("BOB", "No state directory for this book; the record is not stored");
    return false;
  }

  const std::string path = storePath(bookCachePath);
  uint32_t watermark = 0;
  std::vector<BookOrbitBookmarkRecord> records;
  readFile(path, watermark, records);  // a missing or corrupt file simply starts a new one

  const auto existing = std::find_if(records.begin(), records.end(), [&](const BookOrbitBookmarkRecord& r) {
    return r.timestamp == record.timestamp && r.spineIndex == record.spineIndex;
  });
  if (existing != records.end()) {
    *existing = record;
  } else {
    if (records.size() >= MAX_RECORDS) {
      LOG_ERR("BOB", "Bookmark store full (%u records); bookmark will not sync", (unsigned)records.size());
      return false;
    }
    records.push_back(record);
  }

  return writeFile(path, watermark, records);
}

bool BookOrbitBookmarkStore::readAll(const std::string& bookCachePath, std::vector<BookOrbitBookmarkRecord>& out) {
  uint32_t watermark = 0;
  return readFile(storePath(bookCachePath), watermark, out);
}

uint32_t BookOrbitBookmarkStore::readWatermark(const std::string& bookCachePath) {
  uint32_t watermark = 0;
  std::vector<BookOrbitBookmarkRecord> records;
  readFile(storePath(bookCachePath), watermark, records);
  return watermark;
}

bool BookOrbitBookmarkStore::advanceWatermark(const std::string& bookCachePath, const uint32_t timestamp) {
  const std::string path = storePath(bookCachePath);
  uint32_t watermark = 0;
  std::vector<BookOrbitBookmarkRecord> records;
  if (!readFile(path, watermark, records)) return false;
  if (timestamp <= watermark) return true;
  return writeFile(path, timestamp, records);
}

bool BookOrbitBookmarkStore::retain(const std::string& bookCachePath, const std::vector<uint32_t>& keepTimestamps) {
  const std::string path = storePath(bookCachePath);
  uint32_t watermark = 0;
  std::vector<BookOrbitBookmarkRecord> records;
  if (!readFile(path, watermark, records)) return false;

  std::vector<BookOrbitBookmarkRecord> kept;
  kept.reserve(records.size());
  for (BookOrbitBookmarkRecord& record : records) {
    if (std::find(keepTimestamps.begin(), keepTimestamps.end(), record.timestamp) != keepTimestamps.end()) {
      kept.push_back(std::move(record));
    }
  }
  if (kept.size() == records.size()) return true;

  LOG_INF("BOB", "Dropping %u bookmark record(s) whose bookmark is gone", (unsigned)(records.size() - kept.size()));
  return writeFile(path, watermark, kept);
}
