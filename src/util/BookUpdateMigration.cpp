#include "BookUpdateMigration.h"

#include <AppCapabilities.h>

#if CROSSINK_APP_CAP_BOOK_UPDATE

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <Memory.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "ClippingStore.h"
#include "util/BookContentId.h"

namespace {
constexpr char CACHE_BASE[] = "/.crosspoint";

// One spine slot: which ZIP entry it reads and that entry's CRC-32 from the central directory.
struct Chapter {
  uint64_t pathHash = 0;
  uint16_t pathLength = 0;
  uint32_t crc32 = 0;
  bool found = false;
};

// The spine of the EPUB at path. load() builds the metadata cache when there is none; the old
// copy's is removed again (temporaryCache), since nothing will open that file.
bool readChapters(const std::string& path, const bool temporaryCache, std::vector<Chapter>& out) {
  out.clear();
  std::vector<std::string> entryPaths;
  {
    Epub epub(path, CACHE_BASE);
    const bool loaded = epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true);
    const int count = loaded ? epub.getSpineItemsCount() : 0;
    if (count > 0 && count <= UINT16_MAX) {
      entryPaths.reserve(static_cast<size_t>(count));
      for (int i = 0; i < count; i++) entryPaths.push_back(FsHelpers::normalisePath(epub.getSpineItem(i).href));
    }
    if (temporaryCache) epub.clearCache();
    if (!loaded || entryPaths.empty()) {
      LOG_ERR("BUM", "Cannot read the spine of %s", path.c_str());
      return false;
    }
  }

  // One central-directory scan for every chapter, as Epub does for its stylesheets. Both arrays
  // are short-lived and sized by the spine (~16 and ~16 bytes a chapter).
  const size_t count = entryPaths.size();
  auto targets = makeUniqueNoThrow<ZipFile::EntryTarget[]>(count);
  auto identities = makeUniqueNoThrow<ZipFile::EntryIdentity[]>(count);
  if (!targets || !identities) {
    LOG_ERR("BUM", "No memory to compare %u chapters", static_cast<unsigned>(count));
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    targets[i] = {ZipFile::fnvHash64(entryPaths[i].data(), entryPaths[i].size()),
                  static_cast<uint16_t>(entryPaths[i].size()), static_cast<uint16_t>(i), entryPaths[i].c_str()};
  }
  std::sort(targets.get(), targets.get() + count, [](const ZipFile::EntryTarget& a, const ZipFile::EntryTarget& b) {
    return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
  });
  ZipFile(path).fillEntryIdentities(targets.get(), count, identities.get(), count);

  out.resize(count);
  for (size_t i = 0; i < count; i++) {
    out[i].pathHash = ZipFile::fnvHash64(entryPaths[i].data(), entryPaths[i].size());
    out[i].pathLength = static_cast<uint16_t>(entryPaths[i].size());
    out[i].crc32 = identities[i].crc32;
    out[i].found = identities[i].found;
  }
  return true;
}

// A store under the old hash moves to the new one. One already under the new hash wins: it is
// state this device kept for that copy before, and overwriting it could lose more than it saves.
void moveStore(const std::string& from, const std::string& to) {
  if (!Storage.exists(from.c_str())) return;
  if (Storage.exists(to.c_str())) {
    LOG_ERR("BUM", "Not moving %s: %s already exists", from.c_str(), to.c_str());
    return;
  }
  if (Storage.rename(from.c_str(), to.c_str())) {
    LOG_INF("BUM", "Moved %s -> %s", from.c_str(), to.c_str());
  } else {
    LOG_ERR("BUM", "Failed to move %s -> %s", from.c_str(), to.c_str());
  }
}
}  // namespace

void BookUpdateMigration::apply(const std::string& newPath, const std::string& oldCopyPath) {
  const std::string oldHash = KOReaderDocumentId::calculate(oldCopyPath);
  BookContentId::forget(newPath);
  const std::string newHash = BookContentId::contentHash(newPath);
  if (oldHash.empty() || newHash.empty()) {
    LOG_ERR("BUM", "Cannot hash both copies of %s; its local state stays with the old copy", newPath.c_str());
    return;
  }
  if (oldHash == newHash) return;  // the same content: every store already names it
  LOG_INF("BUM", "%s replaced by a different copy (%s -> %s)", newPath.c_str(), oldHash.c_str(), newHash.c_str());

  // Compared before anything moves, so a failure leaves the state where the old copy had it.
  std::vector<Chapter> before;
  std::vector<Chapter> after;
  const bool compared = readChapters(oldCopyPath, /*temporaryCache=*/true, before) &&
                        readChapters(newPath, /*temporaryCache=*/false, after);
  // Without a comparison, every chapter counts as changed (an empty list orphans all).
  std::vector<uint8_t> chapterChanged;
  size_t changedCount = 0;
  if (compared) {
    chapterChanged.resize(before.size(), 1);
    for (size_t i = 0; i < before.size(); i++) {
      const bool same = i < after.size() && before[i].found && after[i].found &&
                        before[i].pathHash == after[i].pathHash && before[i].pathLength == after[i].pathLength &&
                        before[i].crc32 == after[i].crc32;
      chapterChanged[i] = same ? 0 : 1;
      if (!same) changedCount++;
    }
    LOG_INF("BUM", "%u of %u chapters changed (new copy has %u)", static_cast<unsigned>(changedCount),
            static_cast<unsigned>(before.size()), static_cast<unsigned>(after.size()));
  } else {
    LOG_ERR("BUM", "Could not compare the copies; every highlight of %s is orphaned", newPath.c_str());
  }

  // The names below are the content-keyed stores' own (ClippingStore, BookmarkStore,
  // BookContentId::bookStateDirName); EPUB is the only book type with all of them.
  moveStore(std::string(CACHE_BASE) + "/clippings/epub_" + oldHash + ".bin",
            std::string(CACHE_BASE) + "/clippings/epub_" + newHash + ".bin");
  moveStore(std::string(CACHE_BASE) + "/bookmarks/epub_" + oldHash + ".bin",
            std::string(CACHE_BASE) + "/bookmarks/epub_" + newHash + ".bin");
  moveStore(std::string(CACHE_BASE) + "/book_" + oldHash, std::string(CACHE_BASE) + "/book_" + newHash);

  if (!compared || changedCount > 0 || after.size() < before.size()) {
    ClippingStore::markOrphanedChapters(newPath, chapterChanged.data(), chapterChanged.size());
  }
}

#endif  // CROSSINK_APP_CAP_BOOK_UPDATE
