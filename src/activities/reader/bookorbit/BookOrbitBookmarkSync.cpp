#include "BookOrbitBookmarkSync.h"

#include <Epub.h>
#include <Logging.h>

#include <algorithm>

#include "BookOrbitBookmarkStore.h"
#include "BookOrbitKeySet.h"
#include "BookOrbitSyncClient.h"
#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "Epub/Section.h"
#include "ProgressMapper.h"
#include "util/BookContentId.h"

namespace {
// Bookmark sync is a newer server capability than annotation sync. The reference plugin only
// calls the route when the server advertises it; the equivalent here is remembering a
// confirmed 404 for the rest of this boot, so an older BookOrbit is asked exactly once
// instead of paying up to three 404s on every sync.
bool s_bookmarkRouteUnsupported = false;
}  // namespace

void BookOrbitBookmarkSync::prepare(const bool serverAcceptsSidecar) {
  if (s_bookmarkRouteUnsupported) return;
  pendingBookmarks.clear();
  pendingBookmarkWatermark = 0;
  pendingBookmarkKeys.clear();
  pendingBookmarkKeysComplete = false;
  pendingBookmarkSidecar = BookOrbitSidecarState::Unstated;
  bookmarkStoreUnmarked = false;

  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  std::vector<BookOrbitBookmarkRecord> records;
  // Same guard as highlights: only a readable store proves this book has synced bookmarks
  // before, so a missing or unreadable one must suppress deletion propagation, not feed it.
  const bool syncedHereBefore = BookOrbitBookmarkStore::readAll(stateDir, records);
  const uint32_t watermark = BookOrbitBookmarkStore::readWatermark(stateDir);

  if (!BOOKMARKS.loadForBook(epubPath, "", "", "epub")) {
    LOG_ERR("BookOrbit", "Could not read bookmarks; they will not sync");
    return;
  }
  const auto& bookmarks = BOOKMARKS.getBookmarks();
  // Read now: the store is unloaded before the sidecar state is worked out.
  const size_t localBookmarkCount = bookmarks.size();

  // Bookmarks from before timestamp stamping carry 0: invisible to sync until the backfill
  // stamps them, and excluded from the completeness rule -- the server cannot hold what was
  // never uploadable.
  std::vector<uint32_t> liveTimestamps;
  size_t syncableCount = 0;
  liveTimestamps.reserve(bookmarks.size());
  for (const Bookmark& bookmark : bookmarks) {
    if (bookmark.timestamp == 0) continue;
    syncableCount++;
    liveTimestamps.push_back(bookmark.timestamp);
  }
  if (BookOrbitBookmarkStore::retain(stateDir, liveTimestamps)) {
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [&](const BookOrbitBookmarkRecord& record) {
                                   return std::find(liveTimestamps.begin(), liveTimestamps.end(), record.timestamp) ==
                                          liveTimestamps.end();
                                 }),
                  records.end());
  }

  for (const BookOrbitBookmarkRecord& record : records) {
    if (pendingBookmarks.size() >= BOOKORBIT_BOOKMARK_BATCH) break;
    if (record.identityEpoch <= watermark) continue;

    const Bookmark* bookmark = nullptr;
    for (const Bookmark& candidate : bookmarks) {
      if (candidate.timestamp == record.timestamp && candidate.spineIndex == record.spineIndex) {
        bookmark = &candidate;
        break;
      }
    }
    if (!bookmark) continue;  // deleted locally; the key set will tell the server

    BookOrbitBookmark entry;
    if (!bookOrbitFormatDatetime(record.identityEpoch, entry.datetime)) continue;
    entry.pos = record.pos;
    entry.chapter = bookmark->chapterTitle;
    pendingBookmarks.push_back(std::move(entry));
    pendingBookmarkWatermark = std::max(pendingBookmarkWatermark, record.identityEpoch);
  }

  BOOKMARKS.unload();

  // An empty set IS complete: deleting the last bookmark must propagate, and it is safe for
  // the same reason as annotations -- the server diffs the key set against this device's own
  // sync states only. The reference plugin reports empty-complete too. And as with
  // annotations, only once a readable store proves this book has synced bookmarks before: a
  // lost store must not read as "every bookmark deleted" (see BookOrbitHighlightSync::prepare).
  const bool everyBookmarkHasAPosition = records.size() >= syncableCount;
  if (!syncedHereBefore) {
    LOG_INF("BookOrbit", "No bookmark sync history for this book; deletions will not propagate this sync");
  } else if (records.size() <= MAX_KEYS_PER_SYNC && everyBookmarkHasAPosition) {
    pendingBookmarkKeysComplete = BookOrbitKeySet::build(records, pendingBookmarkKeys);
  } else if (!everyBookmarkHasAPosition && syncableCount > 0) {
    LOG_INF("BookOrbit", "%u of %u bookmarks carry a position; deletions will not propagate yet",
            (unsigned)records.size(), (unsigned)syncableCount);
  }

  // The server applies the same lost-copy rule to bookmarks (see BookOrbitHighlightSync::prepare).
  pendingBookmarkSidecar = bookOrbitSidecarState(syncedHereBefore, watermark, localBookmarkCount, serverAcceptsSidecar);
  bookmarkStoreUnmarked = syncedHereBefore && watermark == 0;

  LOG_INF("BookOrbit", "Prepared %u bookmark(s) for upload, %u key(s) (watermark=%lu, sidecar=%d)",
          (unsigned)pendingBookmarks.size(), (unsigned)pendingBookmarkKeys.size(),
          static_cast<unsigned long>(watermark), static_cast<int>(pendingBookmarkSidecar));
}

void BookOrbitBookmarkSync::exchange() {
  if (s_bookmarkRouteUnsupported) return;

  // Never skipped otherwise: this request is what brings the server's bookmark changes down.
  bool unmatched = false;
  bool morePending = false;
  const BookOrbitAnnotationKeys keys{pendingBookmarkKeys.empty() ? nullptr : pendingBookmarkKeys.data(),
                                     pendingBookmarkKeys.size(), pendingBookmarkKeysComplete, pendingBookmarkSidecar};
  const BookOrbitSyncClient::Error result = BookOrbitSyncClient::exchangeBookmarks(
      documentHash, SETTINGS.getEffectiveDeviceName(), keys, pendingBookmarks.data(), pendingBookmarks.size(),
      unmatched, &incomingBookmarks, &morePending);
  LOG_INF("BookOrbit", "Bookmark upload result=%d (http=%d, unmatched=%d)", static_cast<int>(result),
          BookOrbitSyncClient::lastHttpCode, unmatched ? 1 : 0);
  if (result == BookOrbitSyncClient::SERVER_ERROR && BookOrbitSyncClient::lastHttpCode == 404) {
    LOG_INF("BookOrbit", "Server predates bookmark sync; not asking again until reboot");
    s_bookmarkRouteUnsupported = true;
    return;
  }
  if (result != BookOrbitSyncClient::OK || unmatched) return;  // retried on the next sync

  // Same lazy-conversion story as annotations: a web bookmark converted during this request
  // only ships on the next one, with no signal, so the first extra round is unconditional.
  const BookOrbitAnnotationKeys noKeys{nullptr, 0, false};
  for (int round = 0; round < 2 && incomingBookmarks.size() < BOOKORBIT_BOOKMARK_BATCH && (round == 0 || morePending);
       round++) {
    morePending = false;
    bool roundUnmatched = false;
    if (BookOrbitSyncClient::exchangeBookmarks(documentHash, SETTINGS.getEffectiveDeviceName(), noKeys, nullptr, 0,
                                               roundUnmatched, &incomingBookmarks,
                                               &morePending) != BookOrbitSyncClient::OK ||
        roundUnmatched) {
      break;
    }
  }

  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  if (pendingBookmarkWatermark > 0) {
    if (!BookOrbitBookmarkStore::advanceWatermark(stateDir, pendingBookmarkWatermark)) {
      LOG_ERR("BookOrbit", "Bookmarks uploaded but the watermark did not advance; they will be re-sent");
    }
  } else if (bookmarkStoreUnmarked) {
    BookOrbitBookmarkStore::markExchanged(stateDir);
  }
  bookmarksSent = static_cast<uint16_t>(pendingBookmarks.size());
  pendingBookmarks.clear();
}

void BookOrbitBookmarkSync::applyIncoming(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer) {
  if (incomingBookmarks.empty()) return;

  std::vector<BookOrbitBookmarkAck> appliedAcks;
  std::vector<uint32_t> deletedIds;
  if (!BOOKMARKS.loadForBook(epubPath, "", "", "epub")) {
    LOG_ERR("BookOrbit", "Could not open bookmarks; server-side changes stay pending");
    return;
  }

  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  std::vector<BookOrbitBookmarkRecord> records;
  BookOrbitBookmarkStore::readAll(stateDir, records);

  uint32_t newestMintedIdentity = 0;
  size_t addedCount = 0;
  const auto keyOfRecord = [](const BookOrbitBookmarkRecord& record,
                              char (&outKey)[BookOrbitAnnotationKey::DIGEST_SIZE], char (&outDatetime)[20]) {
    return bookOrbitFormatDatetime(record.identityEpoch, outDatetime) &&
           bookOrbitAnnotationKey(outDatetime, record.pos.c_str(), outKey);
  };

  for (const BookOrbitIncomingBookmark& incoming : incomingBookmarks) {
    if (incoming.deleted) {
      for (const BookOrbitBookmarkRecord& record : records) {
        char key[BookOrbitAnnotationKey::DIGEST_SIZE] = {};
        char datetime[20] = {};
        if (!keyOfRecord(record, key, datetime) || incoming.key != key) continue;
        const auto& bookmarks = BOOKMARKS.getBookmarks();
        for (size_t i = 0; i < bookmarks.size(); i++) {
          if (bookmarks[i].timestamp == record.timestamp && bookmarks[i].spineIndex == record.spineIndex) {
            BOOKMARKS.removeBookmarkAt(i);
            break;
          }
        }
        break;
      }
      // Already gone locally is the same outcome; withholding the ack would re-offer forever.
      deletedIds.push_back(incoming.serverId);
      continue;
    }

    // Dedupe by position, as the reference plugin does: if a record already describes this
    // pos, the bookmark exists here -- acknowledge with its EXISTING identity.
    bool duplicate = false;
    for (const BookOrbitBookmarkRecord& record : records) {
      if (record.pos != incoming.pos) continue;
      BookOrbitBookmarkAck ack;
      ack.serverId = incoming.serverId;
      if (keyOfRecord(record, ack.key, ack.datetime)) {
        ack.pos = record.pos;
        appliedAcks.push_back(std::move(ack));
        duplicate = true;
      }
      break;
    }
    if (duplicate) continue;

    int spineIndex = 0;
    uint16_t paragraphHint = UINT16_MAX;
    if (!ProgressMapper::parseXPointerLocation(incoming.pos, spineIndex, paragraphHint) || spineIndex < 0 ||
        spineIndex >= epub->getSpineItemsCount()) {
      // Permanently unplaceable: park it server-side rather than have it re-offered forever.
      BookOrbitBookmarkAck ack;
      ack.serverId = incoming.serverId;
      ack.failed = true;
      appliedAcks.push_back(std::move(ack));
      LOG_ERR("BookOrbit", "Cannot place server bookmark %lu from %s", static_cast<unsigned long>(incoming.serverId),
              incoming.pos.c_str());
      continue;
    }

    // The paragraph hint gives the page through the section LUT when that chapter's cache
    // exists; without it the bookmark still lands in the right chapter at its start, and the
    // paragraph anchor lets the reader refine the jump later.
    float progress = 0.0f;
    int pageCount = 1;
    if (paragraphHint != UINT16_MAX) {
      Section tempSection(epub, spineIndex, renderer);
      const auto page = tempSection.getPageForParagraphIndex(paragraphHint);
      if (page.has_value() && tempSection.pageCount > 0) {
        pageCount = tempSection.pageCount;
        progress = static_cast<float>(*page) / static_cast<float>(tempSection.pageCount);
      }
    }

    const char* chapterTitle = nullptr;
    std::string titleStr;
    const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
    if (tocIndex != -1) {
      titleStr = epub->getTocItem(tocIndex).title;
      chapterTitle = titleStr.c_str();
    } else if (!incoming.title.empty()) {
      chapterTitle = incoming.title.c_str();
    }

    if (BOOKMARKS.addBookmark(static_cast<uint16_t>(spineIndex), progress, pageCount, chapterTitle, paragraphHint,
                              incoming.title.c_str()) != BookmarkStore::AddResult::Added) {
      LOG_ERR("BookOrbit", "Could not store server bookmark %lu", static_cast<unsigned long>(incoming.serverId));
      continue;  // unacknowledged: retried next sync
    }
    const Bookmark& stored = BOOKMARKS.getBookmarks().back();
    if (stored.timestamp == 0) {
      // No plausible clock: no identity can be minted, so the apply cannot be linked. Roll the
      // bookmark back and let the next sync (which starts with an NTP refresh) retry.
      BOOKMARKS.removeBookmarkAt(BOOKMARKS.getBookmarks().size() - 1);
      continue;
    }

    // Bookmarks invert the annotation convention: the DEVICE mints the identity and reports it.
    BookOrbitBookmarkRecord record;
    record.timestamp = stored.timestamp;
    record.identityEpoch = stored.timestamp;
    record.spineIndex = static_cast<uint16_t>(spineIndex);
    record.pos = incoming.pos;  // the server's pos verbatim, so both sides hash the same string
    BookOrbitBookmarkStore::put(stateDir, record);
    newestMintedIdentity = std::max(newestMintedIdentity, record.identityEpoch);
    addedCount++;

    BookOrbitBookmarkAck ack;
    ack.serverId = incoming.serverId;
    if (keyOfRecord(record, ack.key, ack.datetime)) {
      ack.pos = record.pos;
      appliedAcks.push_back(std::move(ack));
    }
  }

  BOOKMARKS.unload();

  // Identities minted here sit above the watermark and would be re-offered to the server on
  // the next sync; the server just told us about them, so cover them -- unless older local
  // records still wait to upload.
  if (newestMintedIdentity > 0) {
    const uint32_t watermark = BookOrbitBookmarkStore::readWatermark(stateDir);
    bool localStillPending = false;
    for (const BookOrbitBookmarkRecord& record : records) {
      if (record.identityEpoch > watermark) {
        localStillPending = true;
        break;
      }
    }
    if (!localStillPending) {
      BookOrbitBookmarkStore::advanceWatermark(stateDir, newestMintedIdentity);
    } else {
      // Not past the waiting ones, but the store holds what this exchange delivered (see
      // BookOrbitHighlightSync::applyIncomingAnnotations).
      BookOrbitBookmarkStore::markExchanged(stateDir);
    }
  }

  // Only what was added counts: since BookOrbit 3.5.0 a bookmark renamed on the web is offered
  // again, and one already here is acknowledged without being added.
  bookmarksAdded = static_cast<uint16_t>(addedCount);
  bookmarksRemoved = static_cast<uint16_t>(deletedIds.size());
  LOG_INF("BookOrbit", "Applied %u server bookmark(s) (%u new), %u deletion(s)", (unsigned)appliedAcks.size(),
          (unsigned)addedCount, (unsigned)deletedIds.size());
  if (!appliedAcks.empty() || !deletedIds.empty()) {
    // Rides the sync's session: a nested Session would replace the shared client and close the
    // connection the activity is keeping alive for the progress upload.
    BookOrbitSyncClient::ackBookmarks(documentHash, SETTINGS.getEffectiveDeviceName(), appliedAcks, deletedIds);
  }
  incomingBookmarks.clear();
}
