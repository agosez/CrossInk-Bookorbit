#include "BookUpdateMigration.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "BookOrbitAnnotationStore.h"
#include "BookOrbitBookmarkStore.h"
#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "util/BookCacheUtils.h"
#include "util/BookContentId.h"

namespace {
constexpr char CACHE_BASE[] = "/.crosspoint";
// DownloadFileSwap's name for the file a download replaced (DownloadOptions::keepReplaced).
constexpr char REPLACED_SUFFIX[] = ".old";
// The old copy's chapters, kept in the new copy's state dir until the reader compares them.
// See docs/file-formats.md.
constexpr char CHAPTERS_FILE[] = "/replaced_chapters.bin";
constexpr char CHAPTERS_MAGIC[4] = {'R', 'C', 'H', '1'};

// One spine slot: which ZIP entry it reads and that entry's CRC-32 from the central directory.
struct Chapter {
  uint64_t pathHash = 0;
  uint32_t crc32 = 0;
  uint16_t pathLength = 0;
  uint8_t found = 0;

  bool sameAs(const Chapter& other) const {
    return found && other.found && pathHash == other.pathHash && pathLength == other.pathLength && crc32 == other.crc32;
  }
};

bool spineHrefs(const Epub& epub, std::vector<std::string>& out) {
  const int count = epub.getSpineItemsCount();
  if (count <= 0 || count > UINT16_MAX) return false;
  out.clear();
  out.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) out.push_back(FsHelpers::normalisePath(epub.getSpineItem(i).href));
  return true;
}

// The spine of the copy at path, from its render cache. With build, a missing cache is built
// for the occasion and dropped again: nothing will open that copy.
bool loadSpine(const std::string& path, const bool build, std::vector<std::string>& out) {
  auto epub = makeUniqueNoThrow<Epub>(path, CACHE_BASE);  // heap: an Epub is too big for the stack
  if (!epub) {
    LOG_ERR("BUM", "No memory to read the spine of %s", path.c_str());
    return false;
  }
  const bool ok = epub->load(build, /*skipLoadingCss=*/true, Epub::XLocationLoadMode::Skip) && spineHrefs(*epub, out);
  if (build) epub->clearCache();
  return ok;
}

// One central-directory scan for every chapter, as Epub does for its stylesheets. Both arrays
// are short-lived and sized by the spine (~16 bytes a chapter each).
bool chapterIdentities(const std::string& zipPath, const std::vector<std::string>& hrefs, std::vector<Chapter>& out) {
  const size_t count = hrefs.size();
  std::unique_ptr<ZipFile::EntryTarget[]> targets = makeUniqueNoThrow<ZipFile::EntryTarget[]>(count);
  std::unique_ptr<ZipFile::EntryIdentity[]> identities = makeUniqueNoThrow<ZipFile::EntryIdentity[]>(count);
  if (!targets || !identities) {
    LOG_ERR("BUM", "No memory to compare %u chapters", static_cast<unsigned>(count));
    return false;
  }
  out.assign(count, Chapter{});
  for (size_t i = 0; i < count; i++) {
    out[i].pathHash = ZipFile::fnvHash64(hrefs[i].data(), hrefs[i].size());
    out[i].pathLength = static_cast<uint16_t>(hrefs[i].size());
    targets[i] = {out[i].pathHash, out[i].pathLength, static_cast<uint16_t>(i), hrefs[i].c_str()};
  }
  ZipFile::EntryTarget* const sorted = targets.get();
  std::sort(sorted, sorted + count, [](const ZipFile::EntryTarget& a, const ZipFile::EntryTarget& b) {
    return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
  });
  ZipFile(zipPath).fillEntryIdentities(targets.get(), count, identities.get(), count);
  for (size_t i = 0; i < count; i++) {
    out[i].crc32 = identities[i].crc32;
    out[i].found = identities[i].found ? 1 : 0;
  }
  return true;
}

bool writeChapters(const std::string& path, const std::vector<Chapter>& chapters) {
  FsFile f;
  if (!Storage.openFileForWrite("BUM", path, f)) return false;
  const uint16_t count = static_cast<uint16_t>(chapters.size());
  bool ok =
      f.write(reinterpret_cast<const uint8_t*>(CHAPTERS_MAGIC), sizeof(CHAPTERS_MAGIC)) == sizeof(CHAPTERS_MAGIC) &&
      serialization::tryWritePod(f, count);
  for (uint16_t i = 0; i < count && ok; i++) {
    const Chapter& c = chapters[i];
    ok = serialization::tryWritePod(f, c.pathHash) && serialization::tryWritePod(f, c.crc32) &&
         serialization::tryWritePod(f, c.pathLength) && serialization::tryWritePod(f, c.found);
  }
  f.close();
  if (!ok) LOG_ERR("BUM", "Failed writing %s", path.c_str());
  return ok;
}

// An unreadable list reads as an empty one, which orphans every highlight.
void readChapters(const std::string& path, std::vector<Chapter>& out) {
  out.clear();
  FsFile f;
  if (!Storage.openFileForRead("BUM", path, f)) return;
  char magic[sizeof(CHAPTERS_MAGIC)] = {};
  uint16_t count = 0;
  bool ok = f.read(magic, sizeof(magic)) == static_cast<int>(sizeof(magic)) &&
            memcmp(magic, CHAPTERS_MAGIC, sizeof(magic)) == 0 && serialization::tryReadPod(f, count);
  if (ok) {
    out.assign(count, Chapter{});
    for (uint16_t i = 0; i < count && ok; i++) {
      Chapter& c = out[i];
      ok = serialization::tryReadPod(f, c.pathHash) && serialization::tryReadPod(f, c.crc32) &&
           serialization::tryReadPod(f, c.pathLength) && serialization::tryReadPod(f, c.found);
    }
  }
  f.close();
  if (!ok) {
    LOG_ERR("BUM", "Unreadable chapter list, every highlight orphaned: %s", path.c_str());
    out.clear();
  }
}

// A store under the old hash moves to the new one. One already under the new hash wins: it is
// the state of the same content, which another copy of it on this card already uses.
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

void carryState(const std::string& bookPath, const std::string& oldCopy, const bool renderCacheIsOld,
                const std::string& oldHash, const std::string& newHash) {
  LOG_INF("BUM", "%s replaced by a different copy (%s -> %s)", bookPath.c_str(), oldHash.c_str(), newHash.c_str());
  const std::string oldClippings = ClippingStore::storePathForHash(oldHash);
  const std::string newState = BookContentId::stateDirNameForHash(newHash);

  // Noted before anything moves. Only highlights can be orphaned, so a book without any skips it.
  const bool hasHighlights = Storage.exists(oldClippings.c_str());
  std::vector<Chapter> oldChapters;
  if (hasHighlights) {
    std::vector<std::string> hrefs;
    // Right after the download, the render cache still describes the old copy: its spine costs
    // no parse. Otherwise (resumed, or never opened here) the old copy's own is built aside.
    const bool read = ((renderCacheIsOld && loadSpine(bookPath, false, hrefs)) || loadSpine(oldCopy, true, hrefs)) &&
                      chapterIdentities(oldCopy, hrefs, oldChapters);
    if (!read) {
      LOG_ERR("BUM", "Could not read the old copy's chapters; every highlight of %s will be orphaned",
              bookPath.c_str());
      oldChapters.clear();
    }
  }

  moveStore(oldClippings, ClippingStore::storePathForHash(newHash));
  moveStore(BookmarkStore::storePathForHash(oldHash), BookmarkStore::storePathForHash(newHash));
  moveStore(BookContentId::stateDirNameForHash(oldHash), newState);

  if (hasHighlights && (Storage.exists(newState.c_str()) || Storage.mkdir(newState.c_str()))) {
    writeChapters(newState + CHAPTERS_FILE, oldChapters);
  }
  // Every highlight and bookmark goes up again: the server kept none of an outdated copy's, and
  // v1.6.1+bookorbit.7 counted some as sent anyway. Its merge keeps one of each, and what it
  // sent down itself stays here (sendable only when made here).
  BookOrbitAnnotationStore::rewindWatermark(newState);
  BookOrbitBookmarkStore::rewindWatermark(newState);
}
}  // namespace

bool BookUpdateMigration::pending(const std::string& bookPath) {
  return Storage.exists((bookPath + REPLACED_SUFFIX).c_str());
}

void BookUpdateMigration::begin(const std::string& bookPath, const bool renderCacheIsOld) {
  const std::string oldCopy = bookPath + REPLACED_SUFFIX;
  const std::string oldHash = KOReaderDocumentId::calculate(oldCopy);
  // The memo's size check would miss a replacement of the same size.
  BookContentId::forget(bookPath);
  const std::string newHash = BookContentId::contentHash(bookPath);
  if (oldHash.empty() || newHash.empty()) {
    LOG_ERR("BUM", "Cannot hash both copies of %s; its local state stays with the old copy", bookPath.c_str());
  } else if (oldHash != newHash) {
    carryState(bookPath, oldCopy, renderCacheIsOld, oldHash, newHash);
  }
  // Last: a reset before this point finds the old copy and runs it all again.
  clearBookCache(bookPath);
  if (!Storage.remove(oldCopy.c_str())) LOG_ERR("BUM", "Could not remove %s", oldCopy.c_str());
}

void BookUpdateMigration::finish(const Epub& epub) {
  const std::string stateDir = BookContentId::bookStateDirName(epub.getPath());
  if (stateDir.empty()) return;
  const std::string path = stateDir + CHAPTERS_FILE;
  if (!Storage.exists(path.c_str())) return;

  std::vector<Chapter> before;
  readChapters(path, before);
  std::vector<Chapter> after;
  if (!before.empty()) {
    std::vector<std::string> hrefs;
    if (!spineHrefs(epub, hrefs) || !chapterIdentities(epub.getPath(), hrefs, after)) {
      LOG_ERR("BUM", "Could not read the chapters of %s; compared at its next open", epub.getPath().c_str());
      return;
    }
  }
  // An empty list stands for "not compared": every chapter counts as changed.
  std::vector<uint8_t> changed(before.size(), 1);
  size_t changedCount = before.empty() ? 1 : 0;
  for (size_t i = 0; i < before.size(); i++) {
    changed[i] = i < after.size() && before[i].sameAs(after[i]) ? 0 : 1;
    changedCount += changed[i];
  }
  LOG_INF("BUM", "%u of %u chapters changed (new copy has %u)", static_cast<unsigned>(changedCount),
          static_cast<unsigned>(before.size()), static_cast<unsigned>(after.size()));
  if (changedCount > 0 && !ClippingStore::markOrphanedChapters(epub.getPath(), changed.data(), changed.size())) {
    return;  // compared again at the next open
  }
  Storage.remove(path.c_str());
}
