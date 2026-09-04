#include "BookOrbitDownloadIndex.h"

#include <BookOrbitCredentialStore.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <uzlib.h>

#include <algorithm>
#include <vector>

namespace {

constexpr char INDEX_PATH[] = "/.crosspoint/bookorbit_downloads.bin";
// 4-byte magic, u32 CRC32 of the server URL, then one length-prefixed record per
// download. A file whose magic or server CRC does not match is discarded: book ids
// only mean something on the server that issued them.
constexpr char INDEX_MAGIC[4] = {'B', 'O', 'D', '1'};
constexpr size_t MAX_ENTRIES = 128;  // FIFO eviction; evicted books fall back to the filename heuristic
constexpr uint16_t PATH_MAX_LEN = 256;

struct Entry {
  int64_t bookId = 0;
  uint32_t fileSize = 0;
  std::string path;
};

// In-RAM copy, alive only between the first lookup/record and unload(): the catalog
// activity is the sole user, so this costs nothing outside catalog browsing.
bool loaded = false;
std::vector<Entry> entries;

uint32_t serverCrc() {
  const std::string& url = BOOKORBIT_STORE.getServerUrl();
  return uzlib_crc32(url.data(), static_cast<unsigned int>(url.size()), 0);
}

void ensureLoaded() {
  if (loaded) return;
  loaded = true;
  entries.clear();

  FsFile f;
  if (!Storage.openFileForRead("BODI", INDEX_PATH, f)) return;  // no index yet

  char magic[sizeof(INDEX_MAGIC)] = {};
  uint32_t crc = 0;
  if (f.read(magic, sizeof(magic)) != static_cast<int>(sizeof(magic)) ||
      memcmp(magic, INDEX_MAGIC, sizeof(magic)) != 0 || !serialization::tryReadPod(f, crc) || crc != serverCrc()) {
    f.close();
    LOG_INF("BODI", "Discarding download index (unreadable or from another server)");
    return;
  }

  entries.reserve(MAX_ENTRIES);
  while (entries.size() < MAX_ENTRIES) {
    Entry entry;
    uint16_t pathLen = 0;
    if (!serialization::tryReadPod(f, entry.bookId)) break;  // clean end of file
    if (!serialization::tryReadPod(f, entry.fileSize) || !serialization::tryReadPod(f, pathLen) || pathLen == 0 ||
        pathLen > PATH_MAX_LEN) {
      LOG_ERR("BODI", "Download index truncated at entry %u", (unsigned)entries.size());
      break;
    }
    entry.path.resize(pathLen);
    if (f.read(&entry.path[0], pathLen) != static_cast<int>(pathLen)) {
      LOG_ERR("BODI", "Download index truncated in entry %u", (unsigned)entries.size());
      break;
    }
    entries.push_back(std::move(entry));
  }
  f.close();
}

void save() {
  FsFile f;
  if (!Storage.openFileForWrite("BODI", INDEX_PATH, f)) {
    LOG_ERR("BODI", "Failed to open download index for write");
    return;
  }
  const uint32_t crc = serverCrc();
  bool ok = f.write(INDEX_MAGIC, sizeof(INDEX_MAGIC)) == static_cast<int>(sizeof(INDEX_MAGIC)) &&
            serialization::tryWritePod(f, crc);
  for (const Entry& entry : entries) {
    if (!ok) break;
    const uint16_t pathLen = static_cast<uint16_t>(std::min<size_t>(entry.path.size(), PATH_MAX_LEN));
    if (pathLen == 0) continue;
    ok = serialization::tryWritePod(f, entry.bookId) && serialization::tryWritePod(f, entry.fileSize) &&
         serialization::tryWritePod(f, pathLen) &&
         f.write(entry.path.data(), pathLen) == static_cast<int>(pathLen);
  }
  f.close();
  // A torn write is caught by ensureLoaded()'s per-record checks and just shortens
  // the index, so failure here only costs future markers.
  if (!ok) LOG_ERR("BODI", "Failed to write download index");
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
  const auto it = std::find_if(entries.begin(), entries.end(),
                               [bookId](const Entry& entry) { return entry.bookId == bookId; });
  if (it == entries.end()) return false;

  // The recorded size doubles as an identity check: a different file put at the
  // same path (same-name transfer, re-used filename) usually differs in size.
  const uint32_t size = fileSizeOf(it->path);
  if (size != 0 && size == it->fileSize) {
    outPath = it->path;
    return true;
  }
  entries.erase(it);
  save();
  return false;
}

void BookOrbitDownloadIndex::record(const int64_t bookId, const std::string& path) {
  const uint32_t size = fileSizeOf(path);
  if (size == 0 || path.size() > PATH_MAX_LEN) {
    LOG_ERR("BODI", "Not indexing download at %s", path.c_str());
    return;
  }

  ensureLoaded();
  // One entry per book, and one per path: a re-download replaces the book's old
  // entry, and a book downloaded onto another book's file supersedes that entry.
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [bookId, &path](const Entry& e) { return e.bookId == bookId || e.path == path; }),
                entries.end());
  if (entries.size() >= MAX_ENTRIES) {
    entries.erase(entries.begin());
  }
  Entry entry;
  entry.bookId = bookId;
  entry.fileSize = size;
  entry.path = path;
  entries.push_back(std::move(entry));
  save();
}

void BookOrbitDownloadIndex::unload() {
  std::vector<Entry>().swap(entries);
  loaded = false;
}
