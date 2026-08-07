#include "BookOrbitSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WallClock.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <ctime>

#include "BookOrbitAnnotationStore.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitStatsQueue.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// This file mirrors src/activities/reader/KOReaderSyncActivity.cpp; see that file's
// comments for the rationale behind the heap/TLS-related steps. Kept as a separate,
// independent activity (rather than parameterizing the KOReader one) so BookOrbit
// support cannot regress the existing generic KOReader sync path.

namespace {
// The SNTP client this used to drive directly lives behind halClock now. Those calls reach
// into lwIP's core, which requires the caller to hold the core lock when it is not the
// TCP/IP thread, and the stack this firmware builds against aborts on an unlocked call
// instead of tolerating it. One implementation, in the HAL, is the only way to be sure the
// locking is right everywhere -- this file and KOReaderSyncActivity had copies of it.
void syncTimeWithNTP() { halClock.syncSystemTimeFromNTP(); }

void wifiOff() {
  // No SNTP stop here: syncSystemTimeFromNTP() releases the client before it returns.
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}
}  // namespace

void BookOrbitSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    LOG_DBG("BookOrbit", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    if (!epub->load(false, true)) {
      LOG_ERR("BookOrbit", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("BookOrbit", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void BookOrbitSyncActivity::saveProgressAndReturn(const CrossPointPosition& position) {
  assert(epub);
  const int pageCount = std::max(position.totalPages, position.pageNumber + 1);
  if (pageCount != position.totalPages) {
    LOG_DBG("BookOrbit", "Adjusted remote page count before save: page=%d count=%d -> %d", position.pageNumber,
            position.totalPages, pageCount);
  }
  if (!EpubReaderUtils::saveProgress(*epub, position.spineIndex, position.pageNumber, pageCount)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  returnToReader();
}

void BookOrbitSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

bool BookOrbitSyncActivity::consumeInitialConfirmRelease() {
  if (!lockInitialConfirmRelease) {
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    lockInitialConfirmRelease = false;
  }
  return true;
}

void BookOrbitSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("BookOrbit", "WiFi connection failed, exiting");
    returnToReader();
    return;
  }

  LOG_DBG("BookOrbit", "WiFi connected, starting sync");
  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // Capture the pre-NTP clock so WallClock can compute this power era's correction
  // delta; queued stats stamped before this moment get fixed to exact time below.
  uint32_t epochBeforeNtp = 0;
  WallClock::now(epochBeforeNtp);
  syncTimeWithNTP();
  WallClock::markNtpSynced(epochBeforeNtp);

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  performSync();
}

void BookOrbitSyncActivity::performSync() {
  // BookOrbit only supports the binary partial-MD5 document hash (no filename option).
  documentHash = KOReaderDocumentId::calculate(epubPath);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  LOG_DBG("BookOrbit", "Document hash: %s", documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BookOrbit", "Fetch progress screen could not be rendered synchronously; aborting sync");
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  // One TLS connection for the progress fetch and the stats upload that follows: they go to the
  // same host, and a handshake costs a second or two here. The session deliberately ends before
  // the screen waits on a user decision, so no socket is held open across it.
  BookOrbitSyncClient::Error result;
  {
    BookOrbitSyncClient::Session session;
    result = BookOrbitSyncClient::getProgress(documentHash, remoteProgress);
    LOG_INF("BookOrbit", "Progress fetch result=%d (http=%d)", static_cast<int>(result),
            BookOrbitSyncClient::lastHttpCode);

    // Progress fetch reaching the server (even with no stored progress) means auth and
    // connectivity are good: piggyback the queued reading-session stats on this session.
    if (result == BookOrbitSyncClient::OK || result == BookOrbitSyncClient::NOT_FOUND) {
      uploadQueuedStats();
    }
  }

  // Highlights get their own connection rather than sharing the one above. Draining the stats
  // queue takes up to ~32KB (see BookOrbitStatsQueue::MAX_QUEUED_EVENTS) and an annotation batch
  // with its key set another ~8KB; held at the same time inside a 55KB TLS floor, on the ~65KB
  // WiFi leaves, the two starved each other and the stats upload was the one that failed. An
  // extra handshake costs a second; losing a feature's payload costs the feature.
  if (result == BookOrbitSyncClient::OK || result == BookOrbitSyncClient::NOT_FOUND) {
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_SYNCING_HIGHLIGHTS);
    }
    requestUpdate(true);

    prepareAnnotationBatch();
    {
      BookOrbitSyncClient::Session session;
      uploadAnnotationBatch();
    }
    // Applied before the returns below, not after: they leave early for a book the server holds
    // no progress for, and the highlights it just sent would leave with them.
    if (!incomingAnnotations.empty()) {
      ensureEpubLoaded();
      if (epub) {
        applyIncomingAnnotations();
      } else {
        LOG_ERR("BookOrbit", "Cannot place server highlights without the epub; they stay pending");
      }
    }

    // Held on screen for a moment: the progress decision replaces this view immediately after,
    // and a count that flashes past is no better than no count at all. Silent when nothing
    // happened, so an ordinary sync does not grow an extra pause.
    if (annotationsSent > 0 || annotationsAdded > 0 || annotationsRemoved > 0) {
      char summary[96];
      snprintf(summary, sizeof(summary), tr(STR_HIGHLIGHTS_SYNCED_FORMAT), static_cast<int>(annotationsSent),
               static_cast<int>(annotationsAdded), static_cast<int>(annotationsRemoved));
      {
        RenderLock lock(*this);
        statusMessage = summary;
      }
      requestUpdateAndWait();
      delay(1500);
    }
  }

  if (result == BookOrbitSyncClient::NOT_FOUND) {
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != BookOrbitSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = BookOrbitSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  KOReaderPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine);

  // Refine page using section cache LUTs: li index, anchor, or paragraph index.
  if (remotePosition.hasLiIndex || remotePosition.xpathAnchorId[0] != '\0' || remotePosition.hasParagraphIndex) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    bool refined = false;
    if (remotePosition.hasLiIndex) {
      const auto liPage = tempSection.getPageForListItemIndex(remotePosition.liIndex);
      if (liPage.has_value()) {
        LOG_DBG("BookOrbit", "Li index %u -> page %d (was %d)", remotePosition.liIndex, *liPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *liPage;
        refined = true;
      } else {
        LOG_DBG("BookOrbit", "Li index %u not found in section LUT", remotePosition.liIndex);
      }
    }
    if (!refined && remotePosition.xpathAnchorId[0] != '\0') {
      const auto anchorPage = tempSection.getPageForAnchor(std::string(remotePosition.xpathAnchorId));
      if (anchorPage.has_value()) {
        LOG_DBG("BookOrbit", "Anchor '%s' -> page %d (was %d)", remotePosition.xpathAnchorId, *anchorPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *anchorPage;
        refined = true;
      } else {
        LOG_DBG("BookOrbit", "Anchor '%s' not found in section cache", remotePosition.xpathAnchorId);
      }
    }
    if (!refined && remotePosition.hasParagraphIndex) {
      const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
      const auto nextParagraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex + 1);
      if (paragraphPage.has_value()) {
        int refinedPage = std::max(remotePosition.pageNumber, static_cast<int>(*paragraphPage));
        if (nextParagraphPage.has_value()) {
          const int lutSpan = static_cast<int>(*nextParagraphPage) - static_cast<int>(*paragraphPage);
          if (lutSpan > 0 && refinedPage >= static_cast<int>(*nextParagraphPage)) {
            refinedPage = static_cast<int>(*nextParagraphPage) - 1;
          }
        }
        LOG_DBG("BookOrbit", "Paragraph %u -> LUT page %d, intra page %d, using %d", remotePosition.paragraphIndex,
                *paragraphPage, remotePosition.pageNumber, refinedPage);
        remotePosition.pageNumber = refinedPage;
      } else {
        LOG_DBG("BookOrbit", "Paragraph %u not found in section LUT", remotePosition.paragraphIndex);
      }
    }
  }

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    if (localProgress.percentage > remoteProgress.percentage) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::prepareAnnotationBatch() {
  pendingAnnotations.clear();
  pendingAnnotationWatermark = 0;

  const std::string cachePath = Epub::cachePathForFilePath(epubPath, "/.crosspoint");
  std::vector<BookOrbitAnnotationRecord> records;
  BookOrbitAnnotationStore::readAll(cachePath, records);

  const uint32_t watermark = BookOrbitAnnotationStore::readWatermark(cachePath);
  // The store is loaded only for as long as it takes to copy the batch out, and unloaded
  // before returning: it stays out of the way of the handshake that follows.
  if (!CLIPPINGS.loadForBook(epubPath, "", "", "epub")) {
    LOG_ERR("BookOrbit", "Could not read clippings; highlights will not sync");
    return;
  }
  const size_t localClippingCount = CLIPPINGS.clippingCount();

  // Deleting a clipping leaves its record behind, so drop the orphans before anything reads the
  // set: they would otherwise fill the file and, worse, describe annotations to the server that
  // this device no longer holds.
  std::vector<BookOrbitClippingRef> liveClippings;
  liveClippings.reserve(localClippingCount);
  for (size_t i = 0; i < localClippingCount; i++) {
    const Clipping* clipping = CLIPPINGS.clippingAt(i);
    if (clipping && clipping->timestamp != 0) {
      liveClippings.push_back({clipping->timestamp, clipping->spineIndex, clipping->paragraphIndex});
    }
  }
  if (BookOrbitAnnotationStore::retain(cachePath, liveClippings)) {
    records.erase(
        std::remove_if(records.begin(), records.end(),
                       [&](const BookOrbitAnnotationRecord& record) {
                         const BookOrbitClippingRef ref{record.timestamp, record.spineIndex, record.paragraphIndex};
                         return std::find(liveClippings.begin(), liveClippings.end(), ref) == liveClippings.end();
                       }),
        records.end());
  }

  size_t batchTextBytes = 0;
  for (const BookOrbitAnnotationRecord& record : records) {
    if (pendingAnnotations.size() >= BOOKORBIT_ANNOTATION_BATCH ||
        batchTextBytes >= BOOKORBIT_ANNOTATION_BATCH_TEXT_BYTES) {
      break;
    }
    if (record.identityEpoch <= watermark) continue;

    const BookOrbitClippingRef wanted{record.timestamp, record.spineIndex, record.paragraphIndex};
    const Clipping* clipping = nullptr;
    size_t clippingIndex = 0;
    for (size_t i = 0; i < CLIPPINGS.clippingCount(); i++) {
      const Clipping* candidate = CLIPPINGS.clippingAt(i);
      if (candidate &&
          BookOrbitClippingRef{candidate->timestamp, candidate->spineIndex, candidate->paragraphIndex} == wanted) {
        clipping = candidate;
        clippingIndex = i;
        break;
      }
    }
    if (!clipping) continue;  // deleted locally; the key list will tell the server

    BookOrbitAnnotation annotation;
    // identityEpoch was minted from WallClock when the highlight was created; the server
    // validates the formatted datetime strictly and drops the entry otherwise.
    if (!bookOrbitFormatDatetime(record.identityEpoch, annotation.datetime)) continue;

    if (!CLIPPINGS.readClippingText(clippingIndex, annotation.text) || annotation.text.empty()) continue;
    annotation.pos0 = record.pos0;
    annotation.pos1 = record.pos1;
    annotation.chapter = clipping->chapterTitle;
    annotation.pageno = static_cast<uint16_t>(clipping->startPage + 1);
    batchTextBytes += annotation.text.size();
    pendingAnnotations.push_back(std::move(annotation));
    pendingAnnotationWatermark = std::max(pendingAnnotationWatermark, record.identityEpoch);
  }

  CLIPPINGS.unload();

  // The key set is what lets the server notice highlights deleted on the device: it compares
  // its own set against this one. Sending it is optional, and deliberately skipped when it
  // would not fit -- a heavily-annotated book's full set plus its JSON does not coexist with a
  // 55KB TLS floor and the ~65KB WiFi leaves. keysComplete=false is exactly the flag for that
  // case: the server then skips deletion processing rather than reading the gap as deletions.
  pendingAnnotationKeys.clear();
  pendingKeysComplete = false;
  // Complete means every local highlight is described here -- not that the two counts happen to
  // match. A highlight still waiting for its position is absent from the store, and reporting the
  // set as complete without it would have the server read the gap as a deletion and erase it,
  // losing data on the server to describe a device that never said anything of the sort.
  //
  // An empty set IS reported as complete: deleting the last local highlight must propagate, and
  // it is safe because the server scopes deletion detection to this device's own sync states
  // (findStatesForDeviceBook) -- an empty set deletes exactly what this device provably held,
  // never a web highlight it was yet to receive. The reference plugin reports empty-complete too.
  const bool everyClippingHasAPosition = records.size() >= localClippingCount;
  if (records.size() <= MAX_KEYS_PER_SYNC && everyClippingHasAPosition) {
    pendingAnnotationKeys.reserve(records.size());
    bool allBuilt = true;
    for (const BookOrbitAnnotationRecord& record : records) {
      BookOrbitAnnotationKey key;
      if (!bookOrbitFormatDatetime(record.identityEpoch, key.dt) ||
          !bookOrbitAnnotationKey(key.dt, record.pos0.c_str(), key.k)) {
        allBuilt = false;
        break;
      }
      pendingAnnotationKeys.push_back(key);
    }
    pendingKeysComplete = allBuilt;
    if (!allBuilt) pendingAnnotationKeys.clear();
  } else if (!everyClippingHasAPosition) {
    LOG_INF("BookOrbit", "%u of %u highlights carry a position; deletions will not propagate yet",
            (unsigned)records.size(), (unsigned)localClippingCount);
  } else {
    LOG_INF("BookOrbit", "%u highlights is too many to report as a key set; deletions will not propagate",
            (unsigned)records.size());
  }

  LOG_INF("BookOrbit", "Prepared %u highlight(s) for upload, %u key(s) (watermark=%lu)",
          (unsigned)pendingAnnotations.size(), (unsigned)pendingAnnotationKeys.size(),
          static_cast<unsigned long>(watermark));
}

void BookOrbitSyncActivity::uploadAnnotationBatch() {
  // Deliberately not skipped when there is nothing to send. This request is the only thing that
  // brings the server's own highlight changes down, so returning early here would mean a device
  // that has said everything it has to say never hears anything back.
  bool unmatched = false;
  bool morePending = false;
  const BookOrbitAnnotationKeys keys{pendingAnnotationKeys.empty() ? nullptr : pendingAnnotationKeys.data(),
                                     pendingAnnotationKeys.size(), pendingKeysComplete};
  const BookOrbitSyncClient::Error result = BookOrbitSyncClient::exchangeAnnotations(
      documentHash, SETTINGS.getEffectiveDeviceName(), keys, pendingAnnotations.data(), pendingAnnotations.size(),
      unmatched, &incomingAnnotations, &morePending);
  LOG_INF("BookOrbit", "Highlight upload result=%d (http=%d, unmatched=%d)", static_cast<int>(result),
          BookOrbitSyncClient::lastHttpCode, unmatched ? 1 : 0);

  if (result != BookOrbitSyncClient::OK || unmatched) return;  // retried on the next sync

  // The server converts a web highlight's position lazily, one budget per request, and a
  // highlight converted during THIS request only ships on the next one -- with no signal in the
  // response (skippedNoPosition counts conversions beyond the budget, not the ones that
  // happened). The first extra round is therefore unconditional: it is the only way a freshly
  // created web highlight arrives on the same sync that converted it, instead of surfacing as
  // "my highlight needed two syncs". The second round runs only when the server says more is
  // pending. Keys and changes went out with the first request and are not repeated.
  const BookOrbitAnnotationKeys noKeys{nullptr, 0, false};
  for (int round = 0;
       round < 2 && incomingAnnotations.size() < BOOKORBIT_ANNOTATION_BATCH && (round == 0 || morePending); round++) {
    morePending = false;
    bool roundUnmatched = false;
    if (BookOrbitSyncClient::exchangeAnnotations(documentHash, SETTINGS.getEffectiveDeviceName(), noKeys, nullptr, 0,
                                                 roundUnmatched, &incomingAnnotations,
                                                 &morePending) != BookOrbitSyncClient::OK ||
        roundUnmatched) {
      break;
    }
    LOG_INF("BookOrbit", "Pulled again: %u change(s) total", (unsigned)incomingAnnotations.size());
  }

  const std::string cachePath = Epub::cachePathForFilePath(epubPath, "/.crosspoint");
  if (!BookOrbitAnnotationStore::advanceWatermark(cachePath, pendingAnnotationWatermark)) {
    LOG_ERR("BookOrbit", "Highlights uploaded but the watermark did not advance; they will be re-sent");
  }
  annotationsSent = static_cast<uint16_t>(pendingAnnotations.size());
  pendingAnnotations.clear();
}

void BookOrbitSyncActivity::applyIncomingAnnotations() {
  if (incomingAnnotations.empty()) return;

  // Outside the TLS session on purpose: this loads the clipping store to write into it, and the
  // handshake needs the heap that would take. The acknowledgment opens its own connection after.
  std::vector<BookOrbitAckEntry> appliedIds;
  std::vector<BookOrbitAckEntry> deletedIds;
  if (!CLIPPINGS.loadForBook(epubPath, "", "", "epub")) {
    LOG_ERR("BookOrbit", "Could not open clippings; server-side highlights stay pending");
    return;
  }

  const std::string cachePath = Epub::cachePathForFilePath(epubPath, "/.crosspoint");
  std::vector<BookOrbitAnnotationRecord> records;
  BookOrbitAnnotationStore::readAll(cachePath, records);

  bool storeChanged = false;
  uint32_t newestReceivedIdentity = 0;
  for (const BookOrbitIncomingAnnotation& incoming : incomingAnnotations) {
    // A deletion carries no position, only the key this device reported for it. Resolving that
    // key back through the annotation store names the exact highlight, which beats guessing from
    // text: two identical quotations in one chapter would otherwise be indistinguishable.
    if (incoming.deleted) {
      BookOrbitClippingRef target;
      bool found = false;
      for (const BookOrbitAnnotationRecord& record : records) {
        char datetime[20] = {};
        char key[BookOrbitAnnotationKey::DIGEST_SIZE] = {};
        if (!bookOrbitFormatDatetime(record.identityEpoch, datetime) ||
            !bookOrbitAnnotationKey(datetime, record.pos0.c_str(), key)) {
          continue;
        }
        if (incoming.key == key) {
          target = {record.timestamp, record.spineIndex, record.paragraphIndex};
          found = true;
          break;
        }
      }

      bool removed = false;
      if (found) {
        for (size_t i = 0; i < CLIPPINGS.clippingCount(); i++) {
          const Clipping* clipping = CLIPPINGS.clippingAt(i);
          if (!clipping ||
              !(BookOrbitClippingRef{clipping->timestamp, clipping->spineIndex, clipping->paragraphIndex} == target)) {
            continue;
          }
          removed = CLIPPINGS.removeClippingAt(i);
          break;
        }
      }
      if (removed) {
        storeChanged = true;
      } else {
        LOG_INF("BookOrbit", "Server deletion %lu matched no local highlight (key=%s)",
                static_cast<unsigned long>(incoming.serverId), incoming.key.c_str());
      }
      // Acknowledged either way: already gone locally is the same outcome, and withholding it
      // would have the server offer the same deletion on every sync forever.
      deletedIds.push_back({incoming.serverId, 0});
      continue;
    }

    // DocFragment[N] gives the chapter and p[M] the paragraph, which is all a clipping needs to
    // be found again: the reader locates a clipping by its text whenever the stored range does
    // not match the current layout (see findClippingTextOnPageLoose), so no page or word
    // coordinates have to be invented here.
    int spineIndex = 0;
    uint16_t paragraphHint = UINT16_MAX;
    if (!ProgressMapper::parseXPointerLocation(incoming.pos0, spineIndex, paragraphHint) || spineIndex < 0 ||
        spineIndex >= epub->getSpineItemsCount()) {
      LOG_ERR("BookOrbit", "Cannot place server annotation %lu from %s", static_cast<unsigned long>(incoming.serverId),
              incoming.pos0.c_str());
      continue;  // left unacknowledged, so the server offers it again
    }
    const uint16_t spine = static_cast<uint16_t>(spineIndex);

    // Matched on chapter and text, never on the paragraph hint. The server normalizes the
    // xpointers it stores (crengine omits the index of a single child), so a highlight of ours
    // comes back with a path that no longer matches the one we sent -- and the paragraph number
    // read out of it is a sibling index, not the count CrossInk assigns. The text is the only
    // identity both sides agree on, and it is also what the reader draws from.
    if (incoming.text.empty()) continue;

    const auto findLocalByText = [&](size_t& outIndex) {
      for (size_t i = 0; i < CLIPPINGS.clippingCount(); i++) {
        const Clipping* clipping = CLIPPINGS.clippingAt(i);
        if (!clipping || clipping->spineIndex != spine) continue;
        std::string localText;
        if (!CLIPPINGS.readClippingText(i, localText) || localText != incoming.text) continue;
        outIndex = i;
        return true;
      }
      return false;
    };

    size_t existingIndex = 0;
    if (findLocalByText(existingIndex)) {
      appliedIds.push_back({incoming.serverId, incoming.version});  // nothing to do, but it did land
      uint32_t identity = 0;
      if (bookOrbitParseDatetime(incoming.datetime, identity)) {
        newestReceivedIdentity = std::max(newestReceivedIdentity, identity);
      }
      continue;
    }

    // Page and word coordinates are deliberately left at zero (the store clamps pageCount to 1).
    // findClippingStoredRangeOnPage rejects wordCount==0, so these coordinates never place the
    // highlight: the reader locates it by its text, rather than trusting coordinates this
    // device never measured. Zero is also the honest layout signature here.
    const size_t newIndex = CLIPPINGS.clippingCount();
    const auto added = CLIPPINGS.addClipping(spine, 0, 0, 0, 0, 0, 0, incoming.chapter.c_str(), paragraphHint,
                                             incoming.text, /*layoutSignature=*/0);
    if (added != ClippingStore::AddResult::Added) {
      LOG_ERR("BookOrbit", "Could not store server annotation %lu (result=%d)",
              static_cast<unsigned long>(incoming.serverId), static_cast<int>(added));
      continue;  // unacknowledged: a full store should not silently swallow it
    }
    storeChanged = true;
    appliedIds.push_back({incoming.serverId, incoming.version});

    // Record its position too, so the next sync reports it in the key set as ours rather than
    // offering it back as a new highlight.
    const Clipping* stored = CLIPPINGS.clippingAt(newIndex);
    if (stored) {
      BookOrbitAnnotationRecord record;
      record.timestamp = stored->timestamp;
      // The server's datetime, not the moment this clipping was written: that is what it hashes
      // its identity from, and keying on the local moment made it re-offer this highlight every
      // sync and never report its deletion.
      if (!bookOrbitParseDatetime(incoming.datetime, record.identityEpoch)) {
        record.identityEpoch = stored->timestamp;
        LOG_ERR("BookOrbit", "Server datetime \"%s\" unreadable; this highlight may be re-offered", incoming.datetime);
      }
      record.spineIndex = spine;
      record.paragraphIndex = paragraphHint;
      // Stored exactly as the server sent it, so the key we report next sync is the one it
      // computed. Re-deriving it here would produce a different hash and have our own highlight
      // offered back to us on every sync.
      record.pos0 = incoming.pos0;
      record.pos1 = incoming.pos0;
      BookOrbitAnnotationStore::put(cachePath, record);
      if (record.identityEpoch != stored->timestamp) {  // a parsed server datetime, not the fallback
        newestReceivedIdentity = std::max(newestReceivedIdentity, record.identityEpoch);
      }
    }
  }

  if (storeChanged && !CLIPPINGS.saveToFile()) {
    LOG_ERR("BookOrbit", "Failed to save received highlights");
    CLIPPINGS.unload();
    return;  // nothing acknowledged: the server keeps them and the next sync retries
  }
  CLIPPINGS.unload();

  annotationsAdded = static_cast<uint16_t>(appliedIds.size());
  annotationsRemoved = static_cast<uint16_t>(deletedIds.size());
  // The server, by sending these, has by definition accepted them: cover them with the watermark
  // or the next sync offers each one right back (a pointless upload per received highlight).
  // Skipped while older local records are still waiting to upload -- moving the watermark past
  // them would silence them forever, and one redundant push is the cheaper failure.
  if (newestReceivedIdentity > 0) {
    const uint32_t watermark = BookOrbitAnnotationStore::readWatermark(cachePath);
    bool localStillPending = false;
    for (const BookOrbitAnnotationRecord& record : records) {
      if (record.identityEpoch > watermark) {
        localStillPending = true;
        break;
      }
    }
    if (!localStillPending) {
      BookOrbitAnnotationStore::advanceWatermark(cachePath, newestReceivedIdentity);
    }
  }

  LOG_INF("BookOrbit", "Applied %u server annotation(s), %u deletion(s)", (unsigned)appliedIds.size(),
          (unsigned)deletedIds.size());
  if (!appliedIds.empty() || !deletedIds.empty()) {
    BookOrbitSyncClient::Session session;
    BookOrbitSyncClient::ackAnnotations(documentHash, SETTINGS.getEffectiveDeviceName(), appliedIds, deletedIds);
  }
  incomingAnnotations.clear();
}

void BookOrbitSyncActivity::uploadQueuedStats() {
  // Reading-session events queued by the reader (see BookOrbitStatsQueue), pushed
  // while WiFi is already up for the progress sync. Upload-only: BookOrbit has no
  // stats download API, so stats flow CrossInk -> BookOrbit.
  std::vector<BookOrbitStatEvent> events;
  const std::string cachePath = Epub::cachePathForFilePath(epubPath, "/.crosspoint");
  if (!BookOrbitStatsQueue::readAll(cachePath, events) || events.empty()) {
    LOG_INF("BookOrbit", "No queued reading stats for this book");
    return;
  }
  LOG_INF("BookOrbit", "Draining %u queued events (era now %u)", (unsigned)events.size(), (unsigned)WallClock::era());
  // Field-verifiable upload manifest: one line per event (post-correction, below)
  // would miss the corrections, so log after the correction pass instead.

  // Events stamped by the system clock (all of them on RTC-less devices) are
  // re-resolved against NTP: each event gets the correction WallClock measured for ITS
  // era, interpolated along the era's drift ramp when the clock ran continuously since
  // the previous sync, or applied as a flat per-era shift when the era opened on a clock
  // loss (see WallClock::correctionForEvent). The current era's anchors were refreshed
  // by the NTP sync moments ago. Events from eras whose error was never measured keep
  // their checkpoint-approximate stamp unless outright implausible.
  uint32_t syncInstant = 0;                           // real time (NTP set the clock moments ago), 0 if it failed
  if (!WallClock::now(syncInstant)) syncInstant = 0;  // implausible: no upper bound to enforce
  size_t dropped = 0;
  uint32_t previousStart = 0;
  for (auto& event : events) {
    if (event.flags & BookOrbitStatEvent::FLAG_CLOCK_APPROXIMATE) {
      int64_t delta = 0;
      if (WallClock::correctionForEvent(event.era, event.startTime, delta)) {
        int64_t corrected = static_cast<int64_t>(event.startTime) + delta;
        // Nothing queued can have happened after the sync that is uploading it, and the
        // queue is chronological: keep both invariants whatever the measured error was.
        if (syncInstant != 0 && corrected > static_cast<int64_t>(syncInstant)) corrected = syncInstant;
        if (corrected < static_cast<int64_t>(previousStart)) corrected = previousStart;
        if (corrected > 0 && corrected < static_cast<int64_t>(WallClock::MAX_PLAUSIBLE_EPOCH)) {
          event.startTime = static_cast<uint32_t>(corrected);
        }
      } else if (event.startTime < WallClock::MIN_PLAUSIBLE_EPOCH) {
        event.durationSeconds = 0;  // unresolvable (old era, never had a checkpoint): drop below
        dropped++;
      }
    }
    previousStart = event.startTime;
  }
  if (dropped > 0) {
    events.erase(std::remove_if(events.begin(), events.end(),
                                [](const BookOrbitStatEvent& e) { return e.durationSeconds == 0; }),
                 events.end());
    LOG_ERR("BookOrbit", "Dropped %u stat events with unresolvable timestamps", (unsigned)dropped);
    if (events.empty()) {
      BookOrbitStatsQueue::clear(cachePath);
      return;
    }
  }

  // Upload manifest: the corrected, as-sent timestamps, so "which session is
  // missing" is answerable from serial logs. Capped to keep log volume sane.
  constexpr size_t MAX_MANIFEST_LINES = 16;
  const size_t manifestCount = std::min(events.size(), MAX_MANIFEST_LINES);
  for (size_t i = 0; i < manifestCount; i++) {
    char isoTime[24];
    const time_t start = static_cast<time_t>(events[i].startTime);
    struct tm startUtc = {};
    gmtime_r(&start, &startUtc);
    strftime(isoTime, sizeof(isoTime), "%Y-%m-%d %H:%M:%SZ", &startUtc);
    LOG_INF("BookOrbit", "  event %u/%u: start=%s dur=%us pos=%u.%02u%%", (unsigned)(i + 1), (unsigned)events.size(),
            isoTime, (unsigned)events[i].durationSeconds, (unsigned)(events[i].page / 100),
            (unsigned)(events[i].page % 100));
  }
  if (events.size() > MAX_MANIFEST_LINES) {
    LOG_INF("BookOrbit", "  ... %u more events", (unsigned)(events.size() - MAX_MANIFEST_LINES));
  }

  // Batch to keep each JSON body small (~100 events = ~7KB) next to the TLS buffers;
  // the JsonDocument itself is freed before each TLS session starts (see client).
  constexpr size_t BATCH_SIZE = 100;
  for (size_t i = 0; i < events.size(); i += BATCH_SIZE) {
    const size_t n = std::min(BATCH_SIZE, events.size() - i);
    const auto result =
        BookOrbitSyncClient::uploadPageStats(documentHash, SETTINGS.getEffectiveDeviceName(), &events[i], n);
    if (result != BookOrbitSyncClient::OK) {
      const int httpCode = BookOrbitSyncClient::lastHttpCode;
      if (httpCode == 404 || httpCode == 405 || httpCode == 501) {
        // Self-heal: this BookOrbit server predates the page-stats endpoint. Drop
        // the queue instead of re-firing a doomed upload on every future sync;
        // progress sync is unaffected. Updating the server starts buffering fresh.
        LOG_INF("BookOrbit", "Server has no page-stats endpoint (http=%d); discarding queued stats", httpCode);
        BookOrbitStatsQueue::clear(cachePath);
        return;
      }
      // Transient failure: keep the whole queue for a later attempt; re-sending an
      // already-accepted batch next time is harmless compared to losing sessions.
      LOG_ERR("BookOrbit", "Stats upload failed after %u/%u events (http=%d)", (unsigned)i, (unsigned)events.size(),
              httpCode);
      return;
    }
  }

  LOG_INF("BookOrbit", "Uploaded %u reading-session events", (unsigned)events.size());
  BookOrbitStatsQueue::clear(cachePath);
}

void BookOrbitSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BookOrbit", "Upload progress screen could not be rendered synchronously; aborting upload");
    wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  if (epub) {
    epub.reset();
    LOG_DBG("BookOrbit", "Released epub before upload (heap: %u)", (unsigned)ESP.getFreeHeap());
  }

  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;
  progress.device = SETTINGS.getEffectiveDeviceName();
  progress.timestamp = time(nullptr);  // NTP was synced in onWifiSelectionComplete(); BookOrbit uses this to break ties

  const auto result = BookOrbitSyncClient::updateProgress(progress);

  wifiOff();

  if (result != BookOrbitSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = BookOrbitSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("BookOrbit", "BookOrbit sync starting");
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  lockInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  if (!BOOKORBIT_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  sdFontSystem.releaseLoadedFont(renderer);
  wifiActivated = true;

  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("BookOrbit", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  LOG_DBG("BookOrbit", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookOrbitSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated) {
    wifiOff();
    silentRestartToReader();
  }
}

void BookOrbitSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_BOOKORBIT_SYNC));

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_BOOKORBIT_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 40, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 65, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 90, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 115, deviceStr);
    }

    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 150, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 175, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 200, localPageStr);

    const int optionY = top + 230;
    const int optionHeight = 30;

    if (selectedOption == 0) {
      renderer.fillRect(screen.x, optionY - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY, tr(STR_APPLY_REMOTE),
                      selectedOption != 0);

    if (selectedOption == 1) {
      renderer.fillRect(screen.x, optionY + optionHeight - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY + optionHeight,
                      tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    const int messageWidth = screen.width - metrics.contentSidePadding * 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto messageLines = renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(), messageWidth, 3);
    int messageY = top + 40;
    for (const auto& line : messageLines) {
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, messageY, line.c_str());
      messageY += lineHeight + 4;
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }
}

void BookOrbitSyncActivity::loop() {
  if (consumeInitialConfirmRelease()) {
    return;
  }

  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition);
      } else if (selectedOption == 1) {
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (documentHash.empty()) {
        documentHash = KOReaderDocumentId::calculate(epubPath);
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
