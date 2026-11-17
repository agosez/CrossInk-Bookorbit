#include "BookOrbitDownloadIndex.h"

#include <BookOrbitCredentialStore.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <uzlib.h>

#include <cstring>

#include "network/DownloadFileSwap.h"

namespace {

constexpr char INDEX_PATH[] = "/.crosspoint/bookorbit_downloads.bin";
constexpr char STAGED_PATH[] = "/.crosspoint/bookorbit_downloads.bin.part";
constexpr char INDEX_MAGIC[] = {'B', 'O', 'D', '1'};
constexpr size_t PATH_LIMIT = 256;

struct Entry {
  int64_t bookId = 0;
  uint32_t fileSize = 0;
  std::string path;
};

enum class IndexState { Missing, Ready, Error };

uint32_t serverCrc() {
  const std::string& url = BOOKORBIT_STORE.getServerUrl();
  return uzlib_crc32(url.data(), static_cast<unsigned int>(url.size()), 0);
}

IndexState openIndex(FsFile& file, const uint32_t expectedCrc) {
  if (!DownloadFileSwap::recover(INDEX_PATH)) return IndexState::Error;
  if (!Storage.exists(INDEX_PATH)) return IndexState::Missing;
  if (!Storage.openFileForRead("BODI", INDEX_PATH, file)) return IndexState::Error;

  char magic[sizeof(INDEX_MAGIC)]{};
  uint32_t crc = 0;
  if (file.read(magic, sizeof(magic)) == sizeof(magic) && memcmp(magic, INDEX_MAGIC, sizeof(magic)) == 0 &&
      serialization::tryReadPod(file, crc) && crc == expectedCrc) {
    return IndexState::Ready;
  }
  file.close();
  LOG_INF("BODI", "Ignoring index with an invalid header or another server URL");
  return IndexState::Missing;
}

bool readEntry(FsFile& file, Entry& entry) {
  uint16_t length = 0;
  if (!serialization::tryReadPod(file, entry.bookId) || !serialization::tryReadPod(file, entry.fileSize) ||
      !serialization::tryReadPod(file, length) || length == 0 || length > PATH_LIMIT) {
    LOG_ERR("BODI", "Invalid download index record");
    return false;
  }
  entry.path.resize(length);
  if (file.read(entry.path.data(), length) == length) return true;
  LOG_ERR("BODI", "Truncated download index path");
  return false;
}

bool writeEntry(FsFile& file, const Entry& entry) {
  const auto length = static_cast<uint16_t>(entry.path.size());
  return serialization::tryWritePod(file, entry.bookId) && serialization::tryWritePod(file, entry.fileSize) &&
         serialization::tryWritePod(file, length) && file.write(entry.path.data(), length) == length;
}

uint32_t fileSizeOf(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("BODI", path, file)) return 0;
  const auto size = file.isDirectory() ? 0 : static_cast<uint32_t>(file.fileSize());
  file.close();
  return size;
}

// Reuse one record's path buffer (up to 256 bytes) across the scan. The visitor
// runs only on the main task and may open a different file, but not this index.
template <typename Visit>
void scanIndex(Visit visit) {
  FsFile file;
  if (openIndex(file, serverCrc()) != IndexState::Ready) return;
  const size_t end = file.fileSize();
  Entry entry;
  entry.path.reserve(PATH_LIMIT);
  while (file.position() < end && readEntry(file, entry) && visit(entry)) {
  }
  file.close();
}

// Keep the previous index if writing or publishing the replacement fails. The
// legacy writer could leave a partial last record; keep the readable prefix.
template <typename Keep>
void rewriteIndex(Keep keep, const Entry* replacement) {
  FsFile source;
  const uint32_t crc = serverCrc();
  const auto state = openIndex(source, crc);
  if (state == IndexState::Error || (state == IndexState::Missing && !replacement)) return;
  FsFile staged;
  if (!Storage.openFileForWrite("BODI", STAGED_PATH, staged)) {
    source.close();
    return;
  }
  bool ok =
      staged.write(INDEX_MAGIC, sizeof(INDEX_MAGIC)) == sizeof(INDEX_MAGIC) && serialization::tryWritePod(staged, crc);
  if (state == IndexState::Ready) {
    const size_t end = source.fileSize();
    Entry entry;
    entry.path.reserve(PATH_LIMIT);
    while (ok && source.position() < end && readEntry(source, entry)) {
      if (keep(entry)) ok = writeEntry(staged, entry);
    }
  }
  source.close();
  if (ok && replacement) ok = writeEntry(staged, *replacement);
  ok = staged.close() && ok;
  if (ok && DownloadFileSwap::publish(INDEX_PATH)) return;
  LOG_ERR("BODI", "Failed to replace download index");
  Storage.remove(STAGED_PATH);
}

}  // namespace

bool BookOrbitDownloadIndex::lookup(const int64_t bookId, std::string& outPath) {
  bool found = false;
  bool stale = false;
  scanIndex([&](const Entry& entry) {
    if (entry.bookId != bookId) return true;
    const uint32_t size = fileSizeOf(entry.path);
    found = size != 0 && size == entry.fileSize;
    if (found) outPath = entry.path;
    stale = !found;
    return false;
  });
  if (stale) rewriteIndex([bookId](const Entry& entry) { return entry.bookId != bookId; }, nullptr);
  return found;
}

void BookOrbitDownloadIndex::record(const int64_t bookId, const std::string& path) {
  const uint32_t size = fileSizeOf(path);
  if (size == 0 || path.size() > PATH_LIMIT) {
    LOG_ERR("BODI", "Not indexing download at %s", path.c_str());
    return;
  }
  const Entry replacement{bookId, size, path};
  rewriteIndex([&](const Entry& entry) { return entry.bookId != bookId && entry.path != path; }, &replacement);
}

void BookOrbitDownloadIndex::forEachExisting(void (*visit)(int64_t, const std::string&, void*), void* context,
                                             bool (*include)(int64_t, void*)) {
  scanIndex([&](const Entry& entry) {
    if (include && !include(entry.bookId, context)) return true;
    const uint32_t size = fileSizeOf(entry.path);
    if (size != 0 && size == entry.fileSize) visit(entry.bookId, entry.path, context);
    return true;
  });
}

void BookOrbitDownloadIndex::relocate(const std::string& oldPath, const std::string& newPath) {
  Entry previous;
  scanIndex([&](const Entry& entry) {
    if (entry.path != oldPath) return true;
    previous = entry;
    return false;
  });
  if (previous.path.empty()) return;
  if (newPath.size() > PATH_LIMIT) {
    LOG_ERR("BODI", "Dropping download entry, path too long: %s", newPath.c_str());
    rewriteIndex([&](const Entry& entry) { return entry.path != oldPath; }, nullptr);
    return;
  }
  previous.path = newPath;
  rewriteIndex([&](const Entry& entry) { return entry.path != oldPath && entry.path != newPath; }, &previous);
}

size_t BookOrbitDownloadIndex::recordedCount() {
  size_t count = 0;
  scanIndex([&](const Entry&) {
    ++count;
    return true;
  });
  return count;
}
