#include "BookOrbitAnnotationStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

namespace {
constexpr char STORE_FILENAME[] = "/bookorbit_annotations.bin";
// 4-byte magic + 4-byte watermark, then one length-prefixed record per highlight. Bump the
// magic's trailing digit if the layout changes and document it in docs/file-formats.md;
// a file whose magic does not match is discarded on read and rewritten on the next put, so
// a bump costs only the minted xpointers, which the reader re-stamps as chapters are opened.
constexpr char STORE_MAGIC[4] = {'B', 'O', 'A', '1'};
constexpr size_t POS_MAX = 512;

std::string storePath(const std::string& bookCachePath) { return bookCachePath + STORE_FILENAME; }

bool readFile(const std::string& path, uint32_t& outWatermark, std::vector<BookOrbitAnnotationRecord>& out) {
  outWatermark = 0;
  out.clear();

  FsFile f;
  if (!Storage.openFileForRead("BOA", path, f)) return false;

  char magic[sizeof(STORE_MAGIC)] = {};
  if (f.read(magic, sizeof(magic)) != static_cast<int>(sizeof(magic)) ||
      memcmp(magic, STORE_MAGIC, sizeof(magic)) != 0 || !serialization::tryReadPod(f, outWatermark)) {
    f.close();
    LOG_ERR("BOA", "Discarding unreadable annotation store: %s", path.c_str());
    outWatermark = 0;
    return false;
  }

  while (out.size() < BookOrbitAnnotationStore::MAX_RECORDS) {
    BookOrbitAnnotationRecord record;
    uint16_t pos0Length = 0;
    uint16_t pos1Length = 0;
    if (!serialization::tryReadPod(f, record.timestamp)) break;  // clean end of file
    if (!serialization::tryReadPod(f, record.identityEpoch) || !serialization::tryReadPod(f, record.spineIndex) ||
        !serialization::tryReadPod(f, record.paragraphIndex) || !serialization::tryReadPod(f, pos0Length) ||
        !serialization::tryReadPod(f, pos1Length) || pos0Length == 0 || pos0Length > POS_MAX || pos1Length == 0 ||
        pos1Length > POS_MAX) {
      LOG_ERR("BOA", "Annotation store truncated at record %u: %s", (unsigned)out.size(), path.c_str());
      break;
    }
    record.pos0.resize(pos0Length);
    record.pos1.resize(pos1Length);
    if (f.read(&record.pos0[0], pos0Length) != static_cast<int>(pos0Length) ||
        f.read(&record.pos1[0], pos1Length) != static_cast<int>(pos1Length)) {
      LOG_ERR("BOA", "Annotation store truncated in record %u: %s", (unsigned)out.size(), path.c_str());
      break;
    }
    out.push_back(std::move(record));
  }

  f.close();
  return true;
}

bool writeFile(const std::string& path, const uint32_t watermark,
               const std::vector<BookOrbitAnnotationRecord>& records) {
  FsFile f;
  if (!Storage.openFileForWrite("BOA", path, f)) {
    LOG_ERR("BOA", "Failed to open annotation store for write: %s", path.c_str());
    return false;
  }

  bool ok = f.write(STORE_MAGIC, sizeof(STORE_MAGIC)) == static_cast<int>(sizeof(STORE_MAGIC)) &&
            serialization::tryWritePod(f, watermark);
  for (const BookOrbitAnnotationRecord& record : records) {
    if (!ok) break;
    const uint16_t pos0Length = static_cast<uint16_t>(std::min(record.pos0.size(), POS_MAX));
    const uint16_t pos1Length = static_cast<uint16_t>(std::min(record.pos1.size(), POS_MAX));
    if (pos0Length == 0 || pos1Length == 0) continue;
    ok = serialization::tryWritePod(f, record.timestamp) && serialization::tryWritePod(f, record.identityEpoch) &&
         serialization::tryWritePod(f, record.spineIndex) && serialization::tryWritePod(f, record.paragraphIndex) &&
         serialization::tryWritePod(f, pos0Length) && serialization::tryWritePod(f, pos1Length) &&
         f.write(record.pos0.data(), pos0Length) == static_cast<int>(pos0Length) &&
         f.write(record.pos1.data(), pos1Length) == static_cast<int>(pos1Length);
  }

  f.close();
  if (!ok) LOG_ERR("BOA", "Failed to write annotation store: %s", path.c_str());
  return ok;
}
}  // namespace

bool BookOrbitAnnotationStore::put(const std::string& bookCachePath, const BookOrbitAnnotationRecord& record) {
  if (record.timestamp == 0 || record.identityEpoch == 0 || record.pos0.empty() || record.pos1.empty()) {
    LOG_ERR("BOA", "Refusing to store an annotation record with no timestamp or position");
    return false;
  }

  const std::string path = storePath(bookCachePath);
  uint32_t watermark = 0;
  std::vector<BookOrbitAnnotationRecord> records;
  readFile(path, watermark, records);  // a missing or corrupt file simply starts a new one

  const BookOrbitClippingRef incomingRef{record.timestamp, record.spineIndex, record.paragraphIndex};
  const auto existing = std::find_if(records.begin(), records.end(), [&](const BookOrbitAnnotationRecord& r) {
    return BookOrbitClippingRef{r.timestamp, r.spineIndex, r.paragraphIndex} == incomingRef;
  });
  if (existing != records.end()) {
    *existing = record;
  } else {
    if (records.size() >= MAX_RECORDS) {
      LOG_ERR("BOA", "Annotation store full (%u records); highlight will not sync", (unsigned)records.size());
      return false;
    }
    records.push_back(record);
  }

  return writeFile(path, watermark, records);
}

bool BookOrbitAnnotationStore::readAll(const std::string& bookCachePath, std::vector<BookOrbitAnnotationRecord>& out) {
  uint32_t watermark = 0;
  return readFile(storePath(bookCachePath), watermark, out);
}

uint32_t BookOrbitAnnotationStore::readWatermark(const std::string& bookCachePath) {
  uint32_t watermark = 0;
  std::vector<BookOrbitAnnotationRecord> records;
  readFile(storePath(bookCachePath), watermark, records);
  return watermark;
}

bool BookOrbitAnnotationStore::advanceWatermark(const std::string& bookCachePath, const uint32_t timestamp) {
  const std::string path = storePath(bookCachePath);
  uint32_t watermark = 0;
  std::vector<BookOrbitAnnotationRecord> records;
  if (!readFile(path, watermark, records)) return false;
  if (timestamp <= watermark) return true;
  return writeFile(path, timestamp, records);
}

bool BookOrbitAnnotationStore::retain(const std::string& bookCachePath, const std::vector<BookOrbitClippingRef>& keep) {
  const std::string path = storePath(bookCachePath);
  uint32_t watermark = 0;
  std::vector<BookOrbitAnnotationRecord> records;
  if (!readFile(path, watermark, records)) return false;

  std::vector<BookOrbitAnnotationRecord> kept;
  kept.reserve(records.size());
  for (BookOrbitAnnotationRecord& record : records) {
    const BookOrbitClippingRef ref{record.timestamp, record.spineIndex, record.paragraphIndex};
    if (std::find(keep.begin(), keep.end(), ref) != keep.end()) {
      kept.push_back(std::move(record));
    }
  }
  if (kept.size() == records.size()) return true;

  LOG_INF("BOA", "Dropping %u annotation record(s) whose highlight is gone", (unsigned)(records.size() - kept.size()));
  return writeFile(path, watermark, kept);
}
