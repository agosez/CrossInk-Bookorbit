#include "BookOrbitBookmarkSync.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <WallClock.h>

#include <algorithm>

#include "BookOrbitBookmarkStore.h"
#include "BookOrbitKeySet.h"
#include "BookOrbitSyncClient.h"
#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "ProgressMapper.h"
#include "activities/RenderLock.h"
#include "util/BookContentId.h"

namespace {
// Bookmark sync is a newer server capability than annotation sync. The reference plugin only
// calls the route when the server advertises it; the equivalent here is remembering a
// confirmed 404 for the rest of this boot, so an older BookOrbit is asked exactly once
// instead of paying up to three 404s on every sync.
bool s_bookmarkRouteUnsupported = false;

// See placeReceivedBookmark(): fine enough that the page it gets is the fraction itself.
constexpr int PLACEMENT_PAGES = 10000;

struct ReceivedPlace {
  float progress;
  uint16_t paragraphIndex;
};

/**
 * Where a received bookmark sits in its chapter: the fraction of the chapter's text before it,
 * which is what a Bookmark keeps, and the paragraph the layout numbers it with, which the reader
 * jumps through.
 *
 * Neither comes out of the xpointer itself: its p[N] counts the siblings under the last element,
 * not the chapter's paragraphs (see ProgressMapper::parseXPointerLocation), and nothing here knows
 * the chapter's pages -- a book just downloaded has no layout at all. So the chapter is streamed
 * and the xpointer's ancestry walked, as for a received reading position
 * (BookOrbitRemotePosition). The mapper answers in pages of a count it is given; asked for
 * PLACEMENT_PAGES, its page is the fraction to within 1/PLACEMENT_PAGES.
 *
 * The inflater's state and window (~43 KB) do not fit beside the open TLS session on the X3/X4,
 * so they take the framebuffer, lent as the reader lends it to mint a position. The render task
 * is held off meanwhile: it would draw into the inflater. What the stream leaves there is never
 * shown, since every screen of this sync clears the buffer before drawing and the sleep sync
 * draws the sleep screen again at its end. Should the read still fail, the bookmark keeps the
 * chapter's start and the xpointer's paragraph hint.
 */
ReceivedPlace placeReceivedBookmark(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, const int spineIndex,
                                    const std::string& pos, const uint16_t paragraphHint) {
  CrossPointPosition located{};
  {
    RenderLock lock;
    GfxRenderer::FrameBufferLoan loan(renderer);
    located = ProgressMapper::toCrossPoint(epub, KOReaderPosition{pos, 0.0f}, spineIndex, PLACEMENT_PAGES);
  }
  if (located.spineIndex != spineIndex) return {0.0f, paragraphHint};
  return {static_cast<float>(located.pageNumber) / static_cast<float>(PLACEMENT_PAGES),
          located.hasParagraphIndex ? located.paragraphIndex : paragraphHint};
}

/**
 * A timestamp for a bookmark received now that no bookmark or record of this book holds
 * (BOOKMARKS must be loaded).
 *
 * The timestamp is what joins a bookmark to its position record, and BookOrbitBookmarkStore::put
 * replaces the record of the same timestamp and chapter. Bookmarks received in one sync all took
 * the same second of WallClock, so each record replaced the one before and only the last bookmark
 * kept an identity. So each takes the latest second not already used, counting back from now: a
 * second already past cannot be taken by a bookmark made later.
 */
uint32_t freeBookmarkTimestamp(const uint32_t now, const std::vector<BookOrbitBookmarkRecord>& records) {
  const auto& bookmarks = BOOKMARKS.getBookmarks();
  const auto taken = [&](const uint32_t timestamp) {
    return std::any_of(bookmarks.begin(), bookmarks.end(),
                       [timestamp](const Bookmark& bookmark) { return bookmark.timestamp == timestamp; }) ||
           std::any_of(records.begin(), records.end(),
                       [timestamp](const BookOrbitBookmarkRecord& record) { return record.timestamp == timestamp; });
  };
  uint32_t timestamp = now;
  while (timestamp > 1 && taken(timestamp)) timestamp--;
  return timestamp;
}

/**
 * Places again a bookmark the server offers back that still sits at its chapter's very start,
 * where the builds before #120's fix put every bookmark they received (BOOKMARKS must be loaded).
 * One made here on its chapter's first page sits there too, and placing it again keeps it there.
 */
void replaceIfAtChapterStart(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer,
                             const BookOrbitBookmarkRecord& record) {
  const auto& bookmarks = BOOKMARKS.getBookmarks();
  for (size_t i = 0; i < bookmarks.size(); i++) {
    const Bookmark& bookmark = bookmarks[i];
    if (bookmark.timestamp != record.timestamp || bookmark.spineIndex != record.spineIndex) continue;
    if (bookmark.progress != 0.0f) return;
    const ReceivedPlace place =
        placeReceivedBookmark(epub, renderer, record.spineIndex, record.pos, bookmark.paragraphIndex);
    if (place.progress != bookmark.progress || place.paragraphIndex != bookmark.paragraphIndex) {
      BOOKMARKS.relocateBookmark(i, place.progress, place.paragraphIndex);
    }
    return;
  }
}
}  // namespace

void BookOrbitBookmarkSync::prepare(const bool serverAcceptsSidecar) {
  if (s_bookmarkRouteUnsupported) return;
  pendingBookmarks.clear();
  pendingBookmarkWatermark = 0;
  pendingBookmarkKeys.clear();
  pendingBookmarkKeysComplete = false;
  pendingBookmarkSidecar = BookOrbitSidecarState::Unstated;
  bookmarkStoreUnmarked = false;
  firstVersionStore = false;
  recoveringReceived = false;

  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  std::vector<BookOrbitBookmarkRecord> records;
  // Same guard as highlights: only a readable store proves this book has synced bookmarks
  // before, so a missing or unreadable one must suppress deletion propagation, not feed it.
  const bool syncedHereBefore = BookOrbitBookmarkStore::readAll(stateDir, records);
  const uint32_t watermark = BookOrbitBookmarkStore::readWatermark(stateDir);
  firstVersionStore = syncedHereBefore && BookOrbitBookmarkStore::isFirstVersion(stateDir);
  // Only an exchanged store can have received anything. Without the sidecar state there is no
  // way to ask for the lost ones again: the store just moves on.
  recoveringReceived = firstVersionStore && watermark != 0 && serverAcceptsSidecar;

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

    const auto bookmark = std::find_if(bookmarks.begin(), bookmarks.end(), [&record](const Bookmark& candidate) {
      return candidate.timestamp == record.timestamp && candidate.spineIndex == record.spineIndex;
    });
    if (bookmark == bookmarks.end()) continue;  // deleted locally; the key set will tell the server

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
  if (recoveringReceived) {
    // A bookmark deleted here since the last sync comes back with the rest: the price of not
    // deleting, on the server, the received bookmarks this copy lost.
    LOG_INF("BookOrbit", "Bookmark store predates the received-bookmark fix; asking the server for every bookmark");
  } else if (!syncedHereBefore) {
    LOG_INF("BookOrbit", "No bookmark sync history for this book; deletions will not propagate this sync");
  } else if (records.size() <= MAX_KEYS_PER_SYNC && everyBookmarkHasAPosition) {
    pendingBookmarkKeysComplete = BookOrbitKeySet::build(records, pendingBookmarkKeys);
  } else if (!everyBookmarkHasAPosition && syncableCount > 0) {
    LOG_INF("BookOrbit", "%u of %u bookmarks carry a position; deletions will not propagate yet",
            (unsigned)records.size(), (unsigned)syncableCount);
  }

  // The server applies the same lost-copy rule to bookmarks (see BookOrbitHighlightSync::prepare).
  // New also unlinks this device from every bookmark of the book, so one this sync cannot take
  // stays offered to the next ones, and no later key set deletes it.
  pendingBookmarkSidecar =
      recoveringReceived ? BookOrbitSidecarState::New
                         : bookOrbitSidecarState(syncedHereBefore, watermark, localBookmarkCount, serverAcceptsSidecar);
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
  incompatibleDocument = BookOrbitSyncClient::lastPositionIncompatible;
  LOG_INF("BookOrbit", "Bookmark upload result=%d (http=%d, unmatched=%d, incompatible=%d)", static_cast<int>(result),
          BookOrbitSyncClient::lastHttpCode, unmatched ? 1 : 0, incompatibleDocument ? 1 : 0);
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
  // The server holds this request: whatever it re-offered and this sync does not apply stays
  // offered to the next one.
  if (firstVersionStore && !BookOrbitBookmarkStore::moveToCurrentVersion(stateDir)) {
    LOG_ERR("BookOrbit", "Bookmark store not moved to the current version; the next sync asks for everything again");
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
  // Worked out before the records this apply adds join the list (see the watermark at the end).
  const uint32_t watermark = BookOrbitBookmarkStore::readWatermark(stateDir);
  const bool localStillPending =
      std::any_of(records.begin(), records.end(),
                  [watermark](const BookOrbitBookmarkRecord& record) { return record.identityEpoch > watermark; });
  uint32_t now = 0;
  const bool clockPlausible = WallClock::now(now);

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
        replaceIfAtChapterStart(epub, renderer, record);
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

    // No plausible clock: no identity can be minted, so the apply cannot be linked. The next
    // sync, which starts with an NTP refresh, retries it.
    if (!clockPlausible) continue;

    const ReceivedPlace place = placeReceivedBookmark(epub, renderer, spineIndex, incoming.pos, paragraphHint);

    const char* chapterTitle = nullptr;
    std::string titleStr;
    const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
    if (tocIndex != -1) {
      titleStr = epub->getTocItem(tocIndex).title;
      chapterTitle = titleStr.c_str();
    } else if (!incoming.title.empty()) {
      chapterTitle = incoming.title.c_str();
    }

    // A page count of 0: a received bookmark replaces none. The server has already merged its
    // copies of one place (it dedupes on the converted location, the loop above on the
    // xpointer), and a bookmark replaced here would vanish after this device acknowledged it,
    // so the next sync's complete key set would delete it on the server as well.
    const uint32_t timestamp = freeBookmarkTimestamp(now, records);
    if (BOOKMARKS.addBookmark(static_cast<uint16_t>(spineIndex), place.progress, /*pageCount=*/0, chapterTitle,
                              place.paragraphIndex, incoming.title.c_str(),
                              timestamp) != BookmarkStore::AddResult::Added) {
      LOG_ERR("BookOrbit", "Could not store server bookmark %lu", static_cast<unsigned long>(incoming.serverId));
      continue;  // unacknowledged: retried next sync
    }

    // Bookmarks invert the annotation convention: the DEVICE mints the identity and reports it.
    BookOrbitBookmarkRecord record;
    record.timestamp = timestamp;
    record.identityEpoch = timestamp;
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
    // Seen by the entries after this one: the dedupe above and freeBookmarkTimestamp().
    records.push_back(std::move(record));
  }

  BOOKMARKS.unload();

  // Identities minted here sit above the watermark and would be re-offered to the server on
  // the next sync; the server just told us about them, so cover them -- unless older local
  // records still wait to upload.
  if (newestMintedIdentity > 0) {
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
