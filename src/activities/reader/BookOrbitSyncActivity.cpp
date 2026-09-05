#include "BookOrbitSyncActivity.h"

#include <GfxRenderer.h>

#ifdef SIMULATOR
#include <cstdlib>
#include <cstring>
#endif
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WallClock.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <ctime>

#include "BookOrbitAnnotationStore.h"
#include "BookOrbitBookmarkStore.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitStatsQueue.h"
#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookContentId.h"

// This file mirrors src/activities/reader/KOReaderSyncActivity.cpp; see that file's
// comments for the rationale behind the heap/TLS-related steps. Kept as a separate,
// independent activity (rather than parameterizing the KOReader one) so BookOrbit
// support cannot regress the existing generic KOReader sync path.

namespace {
// Result-screen action geometry, mirroring KOReaderSyncActivity's. The two
// screens ask the same question with the same two answers, and this file already
// tracks that one deliberately (see the class comment): a shared helper would
// have to live in KOReaderSyncActivity.cpp, which is upstream's, and would
// conflict on every sync.
constexpr int RESULT_LOCAL_PAGE_Y_OFFSET = 200;
constexpr int RESULT_ACTION_MARGIN_TOP = 20;
constexpr int RESULT_ACTION_HEIGHT = 48;
constexpr int RESULT_ACTION_GAP = 10;
constexpr int RESULT_NON_TOUCH_ACTION_MARGIN_TOP = 8;
constexpr int RESULT_NON_TOUCH_ACTION_HEIGHT = 40;
constexpr int RESULT_NON_TOUCH_ACTION_GAP = 8;

struct ResultActionLayout {
  Rect buttons[2];
  int rowStep;
  int rowHeight;
  TouchActionButtons::Layout touchLayout;
};

// One layout for the draw and for the hit test, so a translated label cannot
// make the two drift apart. Buttons sit under the local-progress line, but ride
// up when the screen is too short to hold both below it.
ResultActionLayout resultActionLayout(const Rect& screen, const ThemeMetrics& metrics, const int contentTop,
                                      const int lineHeight, const bool hasTouch) {
  const int buttonX = screen.x + metrics.contentSidePadding;
  const int buttonWidth = std::max(1, screen.width - metrics.contentSidePadding * 2);
  const int buttonHeight = hasTouch ? RESULT_ACTION_HEIGHT : RESULT_NON_TOUCH_ACTION_HEIGHT;
  const int buttonGap = hasTouch ? RESULT_ACTION_GAP : RESULT_NON_TOUCH_ACTION_GAP;
  const int marginTop = hasTouch ? RESULT_ACTION_MARGIN_TOP : RESULT_NON_TOUCH_ACTION_MARGIN_TOP;
  const int desiredButtonY = contentTop + RESULT_LOCAL_PAGE_Y_OFFSET + lineHeight + marginTop;
  // Touch devices spend the button-hint band on content: nothing reads it there.
  const int reservedBottom = hasTouch ? metrics.verticalSpacing : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int latestButtonY = screen.y + screen.height - reservedBottom - buttonHeight * 2 - buttonGap;
  const int firstButtonY = std::min(desiredButtonY, latestButtonY);
  ResultActionLayout result{{Rect{buttonX, firstButtonY, buttonWidth, buttonHeight},
                             Rect{buttonX, firstButtonY + buttonHeight + buttonGap, buttonWidth, buttonHeight}},
                            buttonHeight + buttonGap,
                            buttonHeight,
                            {}};
  if (hasTouch) {
    constexpr int touchHeight = TouchActionButtons::kDefaultHeight;
    constexpr int touchGap = TouchActionButtons::kDefaultGap;
    constexpr int touchTotal = touchHeight * 2 + touchGap;
    const Rect touchContainer{buttonX, std::min(firstButtonY, screen.y + screen.height - reservedBottom - touchTotal),
                              buttonWidth, touchTotal};
    result.touchLayout = TouchActionButtons::vertical(touchContainer, 2);
    result.buttons[0] = result.touchLayout.buttons[0];
    result.buttons[1] = result.touchLayout.buttons[1];
    result.rowStep = touchHeight + touchGap;
    result.rowHeight = touchHeight;
  }
  return result;
}

TouchActionButtons::Layout noRemoteProgressActionLayout(const Rect& screen, const ThemeMetrics& metrics) {
  constexpr uint8_t buttonCount = 2;
  constexpr int totalHeight =
      TouchActionButtons::kDefaultHeight * buttonCount + TouchActionButtons::kDefaultGap * (buttonCount - 1);
  const Rect container{screen.x + metrics.contentSidePadding,
                       screen.y + screen.height - metrics.verticalSpacing - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, buttonCount);
}

// Smart sync's memory: the server progress timestamp this device saw at the
// end of its last successful sync of this book, one 12-byte file beside the
// book's stats queue. It answers the one question the automatic decision
// needs -- has the server moved since we were last here? -- which progress
// percentages alone cannot
// 0 means "unknown", and unknown always falls back to the choice screen.
constexpr char SYNC_MARKER_FILE[] = "/bookorbit_sync.bin";
constexpr uint32_t SYNC_MARKER_MAGIC = 0x424F5359;  // "BOSY"

int64_t readLastSyncMarker(const std::string& bookCachePath) {
  if (bookCachePath.empty()) return 0;  // book not hashable: no history is the honest answer
  const std::string path = bookCachePath + SYNC_MARKER_FILE;
  FsFile file;
  if (!Storage.openFileForRead("BookOrbit", path.c_str(), file)) return 0;
  uint32_t magic = 0;
  int64_t timestamp = 0;
  const bool ok = file.read(&magic, sizeof(magic)) == sizeof(magic) &&
                  file.read(&timestamp, sizeof(timestamp)) == sizeof(timestamp) && magic == SYNC_MARKER_MAGIC;
  file.close();
  return ok && timestamp > 0 ? timestamp : 0;
}

void writeLastSyncMarker(const std::string& bookCachePath, const int64_t timestamp) {
  if (timestamp <= 0) return;         // nothing learned; keep whatever history exists
  if (bookCachePath.empty()) return;  // book not hashable: nowhere safe to write
  const std::string path = bookCachePath + SYNC_MARKER_FILE;
  FsFile file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("BookOrbit", "Could not write sync marker %s", path.c_str());
    return;
  }
  const bool ok = file.write(&SYNC_MARKER_MAGIC, sizeof(SYNC_MARKER_MAGIC)) == sizeof(SYNC_MARKER_MAGIC) &&
                  file.write(&timestamp, sizeof(timestamp)) == sizeof(timestamp);
  file.close();
  if (!ok) {
    // A short marker fails the magic/size check on read, so a partial write
    // degrades to "unknown", never to a wrong date.
    LOG_ERR("BookOrbit", "Short write on sync marker %s", path.c_str());
    Storage.remove(path.c_str());
  }
}

// The SNTP client lives behind halClock, whose esp-netif implementation routes every lwIP
// interaction through the core-lock-safe execution path -- this file and KOReaderSyncActivity
// used to carry hand-rolled copies of that discipline.
void syncTimeWithNTP() {
#ifndef SIMULATOR
  if (!halClock.syncSystemTimeFromNTP()) {
    LOG_DBG("BookOrbit", "NTP sync unavailable, using fallback");
  }
#endif
}

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

bool BookOrbitSyncActivity::smartSyncEnabled() const {
  return BOOKORBIT_STORE.getSyncBehavior() == BookOrbitSyncBehavior::SMART;
}

void BookOrbitSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void BookOrbitSyncActivity::completeAlreadySynced() {
  syncSession.reset();  // no request follows; free the TLS session before the reader reloads
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void BookOrbitSyncActivity::saveProgressAndReturn(const CrossPointPosition& position) {
  syncSession.reset();  // applying remote progress is local work; free the TLS session first
  assert(epub);
  const int pageCount = std::max(position.totalPages, position.pageNumber + 1);
  if (pageCount != position.totalPages) {
    LOG_DBG("BookOrbit", "Adjusted remote page count before save: page=%d count=%d -> %d", position.pageNumber,
            position.totalPages, pageCount);
  }
  // Persist the content coordinate too, as KOReaderSyncActivity does. The page number alone
  // is a fraction of an *estimated* chapter total; the reader can only rescale it once the
  // chapter is fully laid out, and with incremental indexing it opens the chapter before
  // that, so the raw page was shown as-is and landed behind whenever the estimate ran short.
  // The offset resolves as soon as the build reaches it, whatever the final page count.
  // (The mapper streams raw XHTML and cannot skip CSS-hidden subtrees the way layout does;
  // that skews the offset and the page fraction alike, so it is no reason to prefer one.)
  const std::optional<uint32_t> visibleTextOffset =
      position.hasVisibleTextOffset ? std::optional<uint32_t>(position.visibleTextOffset) : std::nullopt;
  if (!EpubReaderUtils::saveProgress(*epub, position.spineIndex, position.pageNumber, pageCount, visibleTextOffset)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  RecentBookProgress::saveCachedEpubPercent(*epub, position.spineIndex, position.pageNumber, pageCount);
  // Manual applies record the marker too, so the history smart sync reads is
  // already there the day the option gets switched on.
  writeLastSyncMarker(BookContentId::bookStateDir(epubPath), remoteProgress.timestamp);
  returnToReader();
}

void BookOrbitSyncActivity::returnToReader() {
#ifdef SIMULATOR
  // Integration-test hook (test/integration/): every sync outcome funnels
  // through here once its files and uploads are settled, so it is the one
  // deterministic place a scripted run can end at. The state says how it went.
  if (std::getenv("CROSSINK_SIM_BOOKORBIT_QUIT_AFTER_SYNC") != nullptr) {
    LOG_INF("BookOrbit", "Simulator sync scenario finished (state=%d)", static_cast<int>(state));
    std::_Exit(0);
  }
#endif
  syncSession.reset();
  activityManager.goToReader(epubPath);
}

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
  WiFi.setSleep(false);
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

  // One TLS session for the entire sync, paid right after WiFi came up:
  // Everything after rides it under the kept-session floor.
  // The sessions used to be split (fetch+stats, then highlights+bookmarks,
  // then the upload on its own connection), each fresh handshake re-checked
  // against the 55KB floor.
  //
  // The session is a member and deliberately survives the decision screens: a socket held
  // through a user wait can be closed by the server, but a re-handshake on the kept
  // session fails cleanly (NETWORK_ERROR, retry next sync) where a fresh one was refused
  // outright before it could try.
  syncSession = makeUniqueNoThrow<BookOrbitSyncClient::Session>();
  const BookOrbitSyncClient::Error result = BookOrbitSyncClient::getProgress(documentHash, remoteProgress);
  LOG_INF("BookOrbit", "Progress fetch result=%d (http=%d)", static_cast<int>(result),
          BookOrbitSyncClient::lastHttpCode);

  // Progress fetch reaching the server (even with no stored progress) means auth and
  // connectivity are good: the queued reading-session stats and the highlight and
  // bookmark exchanges all follow on the same session.
  if (result == BookOrbitSyncClient::OK || result == BookOrbitSyncClient::NOT_FOUND) {
    const size_t statsAccepted = uploadQueuedStats();

    {
      RenderLock lock(*this);
      statusMessage = tr(STR_SYNCING_HIGHLIGHTS);
    }
    requestUpdate(true);

    prepareAnnotationBatch();
    prepareBookmarkBatch();
    uploadAnnotationBatch();
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_SYNCING_BOOKMARKS);
    }
    requestUpdate(true);
    uploadBookmarkBatch();
    // Applied before the returns below, not after: they leave early for a book the server holds
    // no progress for, and the highlights it just sent would leave with them.
    if (!incomingAnnotations.empty() || !incomingBookmarks.empty()) {
      ensureEpubLoaded();
      if (epub) {
        applyIncomingAnnotations();
        applyIncomingBookmarks();
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
    if (bookmarksSent > 0 || bookmarksAdded > 0 || bookmarksRemoved > 0) {
      char summary[96];
      snprintf(summary, sizeof(summary), tr(STR_BOOKMARKS_SYNCED_FORMAT), static_cast<int>(bookmarksSent),
               static_cast<int>(bookmarksAdded), static_cast<int>(bookmarksRemoved));
      {
        RenderLock lock(*this);
        statusMessage = summary;
      }
      requestUpdateAndWait();
      delay(1500);
    }

    // Recorded before any progress push: the push is what triggers the server's session
    // estimation, and a sweep already on file is what suppresses it (and retires the
    // duplicate estimates earlier syncs may have left). A failure never fails the sync —
    // the next sync records another one, and the server's sweep window spans many syncs.
    const auto sweepResult =
        BookOrbitSyncClient::completeSweep(SETTINGS.getEffectiveDeviceName(), documentUnmatched ? 0 : 1,
                                           static_cast<uint32_t>(statsAccepted), annotationsSent);
    if (sweepResult != BookOrbitSyncClient::OK) {
      const int httpCode = BookOrbitSyncClient::lastHttpCode;
      if (httpCode == 404 || httpCode == 405 || httpCode == 501) {
        // This BookOrbit server predates the sweeps endpoint; it does not estimate
        // sessions from sync pushes either, so there is nothing to suppress.
        LOG_INF("BookOrbit", "Server has no sweeps endpoint (http=%d); skipping sweep record", httpCode);
      } else {
        LOG_ERR("BookOrbit", "Sweep record failed (result=%d, http=%d); server may estimate duplicate sessions",
                static_cast<int>(sweepResult), httpCode);
      }
    }
  }

  if (result == BookOrbitSyncClient::NOT_FOUND) {
    if (smartSyncEnabled()) {
      LOG_DBG("BookOrbit", "Smart sync: no remote progress, uploading local %.6f", localProgress.percentage);
      performUpload();
      return;
    }
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != BookOrbitSyncClient::OK) {
    syncSession.reset();
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
    syncSession.reset();
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

  // Percentages from different engines are not comparable: KOReader's model runs up to
  // half a point ahead of ours on some books, which made a remote position read as
  // "further" even after reading past it here. Everything that orders the two positions
  // — the smart decision, the default selection, the percentage the screen shows for
  // the remote side — therefore uses the remote position mapped into local pages.
  const float remoteIntra = remotePosition.totalPages > 1 ? static_cast<float>(remotePosition.pageNumber) /
                                                                static_cast<float>(remotePosition.totalPages - 1)
                                                          : 0.0f;
  remoteLocalPercent = epub->calculateProgress(remotePosition.spineIndex, remoteIntra);
  const int spineDelta = currentSpineIndex - remotePosition.spineIndex;
  const int pageDelta = (spineDelta == 0) ? (currentPage - remotePosition.pageNumber) : 0;
  // The xpath mapping is exact to about one page, so within a page the sides agree.
  const bool samePosition = spineDelta == 0 && std::abs(pageDelta) <= 1;
  const bool localAhead = spineDelta > 0 || (spineDelta == 0 && pageDelta > 1);

  if (smartSyncEnabled()) {
    const std::string stateDir = BookContentId::bookStateDir(epubPath);
    const int64_t lastSync = readLastSyncMarker(stateDir);
    LOG_DBG("BookOrbit", "Smart decision: local spine=%d page=%d, remote spine=%d page=%d, serverTs=%lld lastSync=%lld",
            currentSpineIndex, currentPage, remotePosition.spineIndex, remotePosition.pageNumber,
            (long long)remoteProgress.timestamp, (long long)lastSync);
    if (samePosition) {
      // Both sides agree; refresh the marker so the next visit still knows
      // whether the server moved in the meantime.
      writeLastSyncMarker(stateDir, remoteProgress.timestamp);
      completeAlreadySynced();
      return;
    }
    if (remoteProgress.timestamp > 0 && lastSync > 0) {
      // Act only when the position order and the time order tell the same story
      const bool serverMoved = remoteProgress.timestamp > lastSync;
      if (!localAhead && serverMoved) {
        saveProgressAndReturn(remotePosition);
        return;
      }
      if (localAhead && !serverMoved) {
        performUpload();
        return;
      }
      LOG_DBG("BookOrbit", "Smart sync: progress and dates disagree, showing the choice screen");
    } else {
      LOG_DBG("BookOrbit", "Smart sync: no sync history for this book yet, showing the choice screen");
    }
  }

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
    selectedOption = localAhead ? 1 : 0;  // 1 = Upload local, 0 = Apply remote
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::prepareAnnotationBatch() {
  pendingAnnotations.clear();
  pendingAnnotationWatermark = 0;

  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  std::vector<BookOrbitAnnotationRecord> records;
  // A readable store is the proof this book has synced highlights before, even one holding
  // zero records (see the completeness rule below).
  const bool syncedHereBefore = BookOrbitAnnotationStore::readAll(stateDir, records);

  const uint32_t watermark = BookOrbitAnnotationStore::readWatermark(stateDir);
  // The store is loaded only for as long as it takes to copy the batch out, and unloaded
  // before returning: only the small batch stays resident beside the open TLS session.
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
  if (BookOrbitAnnotationStore::retain(stateDir, liveClippings)) {
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
  //
  // ...but only once a readable store proves this book has synced highlights before. The
  // store is keyed by the book's content hash, so a moved or renamed file keeps it; a missing
  // or unreadable store means "never synced this book" or "history lost" (a wiped card, a
  // fresh device), never "the user deleted everything" -- deleting a highlight requires
  // holding it, which requires the store. Claiming completeness over a lost store would have
  // the server erase every highlight this device ever acked. The guard only makes the loss
  // non-destructive; it cannot bring highlights back: the server's push-down
  // (findAddCandidates) re-offers an annotation only to a device id it holds no sync state
  // for, so recovery from real loss stays a server-side action. Deletion reporting resumes
  // on its own once highlights sync again.
  const bool everyClippingHasAPosition = records.size() >= localClippingCount;
  if (!syncedHereBefore) {
    LOG_INF("BookOrbit", "No highlight sync history for this book; deletions will not propagate this sync");
  } else if (records.size() <= MAX_KEYS_PER_SYNC && everyClippingHasAPosition) {
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
  documentUnmatched = unmatched;

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

  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  if (pendingAnnotationWatermark > 0 &&
      !BookOrbitAnnotationStore::advanceWatermark(stateDir, pendingAnnotationWatermark)) {
    LOG_ERR("BookOrbit", "Highlights uploaded but the watermark did not advance; they will be re-sent");
  }
  annotationsSent = static_cast<uint16_t>(pendingAnnotations.size());
  pendingAnnotations.clear();
}

namespace {
// Bookmark sync is a newer server capability than annotation sync. The reference plugin only
// calls the route when the server advertises it; the equivalent here is remembering a
// confirmed 404 for the rest of this boot, so an older BookOrbit is asked exactly once
// instead of paying up to three 404s on every sync.
bool s_bookmarkRouteUnsupported = false;
}  // namespace

void BookOrbitSyncActivity::prepareBookmarkBatch() {
  if (s_bookmarkRouteUnsupported) return;
  pendingBookmarks.clear();
  pendingBookmarkWatermark = 0;
  pendingBookmarkKeys.clear();
  pendingBookmarkKeysComplete = false;

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
  // lost store must not read as "every bookmark deleted" (see prepareAnnotationBatch).
  const bool everyBookmarkHasAPosition = records.size() >= syncableCount;
  if (!syncedHereBefore) {
    LOG_INF("BookOrbit", "No bookmark sync history for this book; deletions will not propagate this sync");
  } else if (records.size() <= MAX_KEYS_PER_SYNC && everyBookmarkHasAPosition) {
    pendingBookmarkKeys.reserve(records.size());
    bool allBuilt = true;
    for (const BookOrbitBookmarkRecord& record : records) {
      BookOrbitAnnotationKey key;
      if (!bookOrbitFormatDatetime(record.identityEpoch, key.dt) ||
          !bookOrbitAnnotationKey(key.dt, record.pos.c_str(), key.k)) {
        allBuilt = false;
        break;
      }
      pendingBookmarkKeys.push_back(key);
    }
    pendingBookmarkKeysComplete = allBuilt;
    if (!allBuilt) pendingBookmarkKeys.clear();
  } else if (!everyBookmarkHasAPosition && syncableCount > 0) {
    LOG_INF("BookOrbit", "%u of %u bookmarks carry a position; deletions will not propagate yet",
            (unsigned)records.size(), (unsigned)syncableCount);
  }

  LOG_INF("BookOrbit", "Prepared %u bookmark(s) for upload, %u key(s) (watermark=%lu)",
          (unsigned)pendingBookmarks.size(), (unsigned)pendingBookmarkKeys.size(),
          static_cast<unsigned long>(watermark));
}

void BookOrbitSyncActivity::uploadBookmarkBatch() {
  if (s_bookmarkRouteUnsupported) return;

  // Never skipped otherwise: this request is what brings the server's bookmark changes down.
  bool unmatched = false;
  bool morePending = false;
  const BookOrbitAnnotationKeys keys{pendingBookmarkKeys.empty() ? nullptr : pendingBookmarkKeys.data(),
                                     pendingBookmarkKeys.size(), pendingBookmarkKeysComplete};
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
  if (pendingBookmarkWatermark > 0 && !BookOrbitBookmarkStore::advanceWatermark(stateDir, pendingBookmarkWatermark)) {
    LOG_ERR("BookOrbit", "Bookmarks uploaded but the watermark did not advance; they will be re-sent");
  }
  bookmarksSent = static_cast<uint16_t>(pendingBookmarks.size());
  pendingBookmarks.clear();
}

void BookOrbitSyncActivity::applyIncomingBookmarks() {
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
    }
  }

  bookmarksAdded = static_cast<uint16_t>(appliedAcks.size());
  bookmarksRemoved = static_cast<uint16_t>(deletedIds.size());
  LOG_INF("BookOrbit", "Applied %u server bookmark(s), %u deletion(s)", (unsigned)appliedAcks.size(),
          (unsigned)deletedIds.size());
  if (!appliedAcks.empty() || !deletedIds.empty()) {
    // Rides syncSession: a nested Session would replace the shared client and close the
    // connection performSync() is keeping alive for the progress upload.
    BookOrbitSyncClient::ackBookmarks(documentHash, SETTINGS.getEffectiveDeviceName(), appliedAcks, deletedIds);
  }
  incomingBookmarks.clear();
}

void BookOrbitSyncActivity::applyIncomingAnnotations() {
  if (incomingAnnotations.empty()) return;

  // This loads the clipping store (~20KB) beside the open TLS session; if that load fails on a
  // tight heap the highlights stay pending for the next sync -- a clean retry, where closing the
  // session here would force the progress upload back onto an unaffordable fresh handshake. The
  // acknowledgment that follows rides the kept session, so no handshake needs room beside the store.
  std::vector<BookOrbitAckEntry> appliedIds;
  std::vector<BookOrbitAckEntry> deletedIds;
  if (!CLIPPINGS.loadForBook(epubPath, "", "", "epub")) {
    LOG_ERR("BookOrbit", "Could not open clippings; server-side highlights stay pending");
    return;
  }

  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  std::vector<BookOrbitAnnotationRecord> records;
  BookOrbitAnnotationStore::readAll(stateDir, records);

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
      BookOrbitAnnotationStore::put(stateDir, record);
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
    const uint32_t watermark = BookOrbitAnnotationStore::readWatermark(stateDir);
    bool localStillPending = false;
    for (const BookOrbitAnnotationRecord& record : records) {
      if (record.identityEpoch > watermark) {
        localStillPending = true;
        break;
      }
    }
    if (!localStillPending) {
      BookOrbitAnnotationStore::advanceWatermark(stateDir, newestReceivedIdentity);
    }
  }

  LOG_INF("BookOrbit", "Applied %u server annotation(s), %u deletion(s)", (unsigned)appliedIds.size(),
          (unsigned)deletedIds.size());
  if (!appliedIds.empty() || !deletedIds.empty()) {
    // Rides syncSession: a nested Session would replace the shared client and close the
    // connection performSync() is keeping alive for the progress upload.
    BookOrbitSyncClient::ackAnnotations(documentHash, SETTINGS.getEffectiveDeviceName(), appliedIds, deletedIds);
  }
  incomingAnnotations.clear();
}

size_t BookOrbitSyncActivity::uploadQueuedStats() {
  // Reading-session events queued by the reader (see BookOrbitStatsQueue), pushed
  // while WiFi is already up for the progress sync. Upload-only: BookOrbit has no
  // stats download API, so stats flow CrossInk -> BookOrbit.
  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  // The queue is drained one upload batch at a time. Reading it whole cost 16 bytes
  // per event -- 32KB for a full queue -- and held them for the length of the upload,
  // beside the TLS session and every body built for it; a long backlog left too
  // little for the batch that was supposed to clear it.
  const size_t total = std::min(BookOrbitStatsQueue::queuedCount(stateDir), BookOrbitStatsQueue::MAX_QUEUED_EVENTS);
  if (total == 0) {
    LOG_INF("BookOrbit", "No queued reading stats for this book");
    return 0;
  }
  LOG_INF("BookOrbit", "Draining %u queued events (era now %u)", (unsigned)total, (unsigned)WallClock::era());

  // The bar belongs to this phase: clear it on every exit path, or the next
  // phase's status message is drawn over a stale one.
  struct ProgressScope {
    BookOrbitSyncActivity* self;
    ~ProgressScope() {
      RenderLock lock(*self);
      self->statsUploaded = 0;
      self->statsTotal = 0;
    }
  } progressScope{this};
  {
    RenderLock lock(*this);
    statsUploaded = 0;
    statsTotal = total;
    statusMessage = tr(STR_SYNCING_READING_SESSIONS);
  }
  requestUpdate(true);

  // Events stamped by the system clock (all of them on RTC-less devices) are
  // re-resolved against NTP: each event gets the correction WallClock measured for ITS
  // era, interpolated along the era's drift ramp when the clock ran continuously since
  // the previous sync, or applied as a flat per-era shift when the era opened on a clock
  // loss (see WallClock::correctionForEvent). The current era's anchors were refreshed
  // by the NTP sync moments ago. Events from eras whose error was never measured keep
  // their checkpoint-approximate stamp unless outright implausible.
  uint32_t syncInstant = 0;                           // real time (NTP set the clock moments ago), 0 if it failed
  if (!WallClock::now(syncInstant)) syncInstant = 0;  // implausible: no upper bound to enforce

  // One batch per request. 25 events is ~2KB of JSON: small enough that building the
  // body cannot exhaust what the kept-session heap floor admits, which a 100-event
  // batch could (measured: abort() while serializing, with a 967-event backlog).
  constexpr size_t BATCH_SIZE = 25;
  // Upload manifest: the corrected, as-sent timestamps, so "which session is
  // missing" is answerable from serial logs. Capped to keep log volume sane.
  constexpr size_t MAX_MANIFEST_LINES = 16;

  std::vector<BookOrbitStatEvent> batch;
  uint32_t previousStart = 0;  // carried across batches: the queue is chronological
  size_t dropped = 0;
  size_t uploaded = 0;
  size_t manifestLines = 0;
  size_t shownTenth = 0;

  for (size_t offset = 0; offset < total; offset += BATCH_SIZE) {
    if (!BookOrbitStatsQueue::readRange(stateDir, offset, BATCH_SIZE, batch)) {
      LOG_ERR("BookOrbit", "Failed to read queued stats at event %u; keeping the queue", (unsigned)offset);
      return uploaded;
    }
    if (batch.empty()) {
      // The file is shorter than its size implied: a truncated queue. Clearing it
      // below is the self-heal; leaving it would stall every future sync here.
      LOG_ERR("BookOrbit", "Stats queue ended early at event %u of %u", (unsigned)offset, (unsigned)total);
      break;
    }

    size_t droppedInBatch = 0;
    for (auto& event : batch) {
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
          droppedInBatch++;
        }
      }
      previousStart = event.startTime;
    }
    if (droppedInBatch > 0) {
      batch.erase(std::remove_if(batch.begin(), batch.end(),
                                 [](const BookOrbitStatEvent& e) { return e.durationSeconds == 0; }),
                  batch.end());
      dropped += droppedInBatch;
    }
    if (batch.empty()) continue;

    for (size_t i = 0; i < batch.size() && manifestLines < MAX_MANIFEST_LINES; i++, manifestLines++) {
      char isoTime[24];
      const time_t start = static_cast<time_t>(batch[i].startTime);
      struct tm startUtc = {};
      gmtime_r(&start, &startUtc);
      strftime(isoTime, sizeof(isoTime), "%Y-%m-%d %H:%M:%SZ", &startUtc);
      LOG_INF("BookOrbit", "  event %u/%u: start=%s dur=%us pos=%u.%02u%%", (unsigned)(manifestLines + 1),
              (unsigned)total, isoTime, (unsigned)batch[i].durationSeconds, (unsigned)(batch[i].page / 100),
              (unsigned)(batch[i].page % 100));
    }

    const auto result = BookOrbitSyncClient::uploadPageStats(documentHash, SETTINGS.getEffectiveDeviceName(),
                                                             batch.data(), batch.size());
    if (result != BookOrbitSyncClient::OK) {
      const int httpCode = BookOrbitSyncClient::lastHttpCode;
      if (httpCode == 404 || httpCode == 405 || httpCode == 501) {
        // Self-heal: this BookOrbit server predates the page-stats endpoint. Drop
        // the queue instead of re-firing a doomed upload on every future sync;
        // progress sync is unaffected. Updating the server starts buffering fresh.
        LOG_INF("BookOrbit", "Server has no page-stats endpoint (http=%d); discarding queued stats", httpCode);
        BookOrbitStatsQueue::clear(stateDir);
        return uploaded;
      }
      // Transient failure: keep the whole queue for a later attempt; re-sending an
      // already-accepted batch next time is harmless compared to losing sessions.
      LOG_ERR("BookOrbit", "Stats upload failed after %u/%u events (http=%d)", (unsigned)uploaded, (unsigned)total,
              httpCode);
      return uploaded;
    }
    uploaded += batch.size();

    // A batch is a ~half-second round trip and a panel refresh costs about as
    // much, so repaint on each tenth of the queue rather than on each batch.
    const size_t tenth = uploaded * 10 / total;
    if (tenth != shownTenth || uploaded >= total) {
      shownTenth = tenth;
      {
        RenderLock lock(*this);
        statsUploaded = uploaded;
      }
      requestUpdate(true);
    }
  }

  if (dropped > 0) {
    LOG_ERR("BookOrbit", "Dropped %u stat events with unresolvable timestamps", (unsigned)dropped);
  }
  LOG_INF("BookOrbit", "Uploaded %u reading-session events", (unsigned)uploaded);
  BookOrbitStatsQueue::clear(stateDir);
  return uploaded;
}

void BookOrbitSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BookOrbit", "Upload progress screen could not be rendered synchronously; aborting upload");
    syncSession.reset();
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

  syncSession.reset();
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

  writeLastSyncMarker(BookContentId::bookStateDir(epubPath), progress.timestamp);
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  if (smartSyncEnabled()) {
    markAutoReturn();
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("BookOrbit", "BookOrbit sync starting");

  // Sync is a reader-originated activity, but its decision prompts are not
  // reader content. Keep their touch actions available even when the reader's
  // tap controls are disabled. (Mirrors KOReaderSyncActivity.)
  if (mappedInput.hasTouchHardware()) {
    mappedInput.setReaderTouchscreenOverride(true);
    touchOverrideActive = true;
  }

  bool hasReaderOrientation = readerOrientation < CrossPointSettings::ORIENTATION_COUNT;
  uint8_t syncOrientation = hasReaderOrientation ? readerOrientation : SETTINGS.orientation;
  const PendingOverlayResume& resume = APP_STATE.pendingOverlayResume;
  if (resume.origin == PendingOverlayOrigin::Reader && resume.overlay == PendingOverlayType::FrontlightDrawer &&
      resume.preserveReaderOrientation && resume.readerOrientation < CrossPointSettings::ORIENTATION_COUNT) {
    syncOrientation = resume.readerOrientation;
    hasReaderOrientation = true;
  }
  ReaderUtils::applyOrientation(renderer, syncOrientation);
  lockInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  if (!BOOKORBIT_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Restart into a minimal network boot first exactly like the KOReader sync flow.
  // By the time onEnter() runs, the reader has already exited cleanly:
  // progress is saved and the reading-session stats are queued, so nothing is lost to the restart.
  // The paragraph anchor is the one piece of the position that lives only in RAM
  if (!networkBoot) {
    // Orientation rides one-based so 0 keeps meaning "no reader override".
    static_assert(CrossPointSettings::ORIENTATION_COUNT <=
                      (BOOKORBIT_SYNC_PAYLOAD_ORIENTATION_MASK >> BOOKORBIT_SYNC_PAYLOAD_ORIENTATION_SHIFT),
                  "orientation payload field too small");
    const uint32_t orientationPayload = hasReaderOrientation ? (static_cast<uint32_t>(syncOrientation) + 1)
                                                                   << BOOKORBIT_SYNC_PAYLOAD_ORIENTATION_SHIFT
                                                             : 0;
    const uint32_t payload =
        orientationPayload |
        (currentParagraphIndex ? (BOOKORBIT_SYNC_PAYLOAD_HAS_PARAGRAPH | *currentParagraphIndex) : 0);
    silentRestartToNetwork(NetworkBootTarget::BOOKORBIT_SYNC, payload);
    return;  // only reached when a deep sleep in progress suppressed the restart
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
  if (touchOverrideActive) {
    mappedInput.setReaderTouchscreenOverride(false);
    touchOverrideActive = false;
  }
  Activity::onExit();

  syncSession.reset();
  if (wifiActivated) {
    wifiOff();
    silentRestartToReader();
  }
}

Rect BookOrbitSyncActivity::headerBandRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Touch devices get the compact band the back button is drawn into, which is
  // not metrics.headerHeight: content below it starts lower.
  return Rect{screen.x, screen.y + metrics.topPadding, screen.width,
              TouchHeaderBackButton::height(metrics, mappedInput)};
}

void BookOrbitSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  // On touch devices the title band carries the way back: the result screen's
  // choices are the only other tap targets, and the rest have none at all.
  const Rect header = headerBandRect();
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_BOOKORBIT_SYNC), /*readerContext=*/true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BOOKORBIT_SYNC));
  }

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
    if (statsTotal > 0) {
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      char counts[32];
      snprintf(counts, sizeof(counts), "%u / %u", (unsigned)statsUploaded, (unsigned)statsTotal);
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, counts);
      const int barWidth = screen.width / 2;
      const int barY = top + 2 * (lineHeight + metrics.verticalSpacing);
      GUI.drawProgressBar(renderer, Rect{screen.x + (screen.width - barWidth) / 2, barY, barWidth, 16}, statsUploaded,
                          statsTotal);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    top = header.y + header.height + metrics.verticalSpacing;
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
    // remoteLocalPercent, not the server's raw percentage: the raw value comes from the
    // sender's engine and is not comparable with the local line drawn just below.
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteLocalPercent * 100);
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
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + RESULT_LOCAL_PAGE_Y_OFFSET,
                      localPageStr);

    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto actions = resultActionLayout(screen, metrics, top, lineHeight, mappedInput.hasTouchHardware());
    const char* actionLabels[] = {tr(STR_APPLY_REMOTE), tr(STR_UPLOAD_LOCAL)};
    if (mappedInput.hasTouchHardware()) {
      TouchActionButtons::draw(renderer, actions.touchLayout, actionLabels, selectedOption, selectedOption,
                               UI_10_FONT_ID);
    } else {
      for (int option = 0; option < 2; ++option) {
        const Rect& button = actions.buttons[option];
        const bool selected = selectedOption == option;
        if (selected) {
          renderer.fillRect(button.x, button.y, button.width, button.height);
        }
        renderer.drawRect(button.x, button.y, button.width, button.height, true);
        const int textX = button.x + (button.width - renderer.getTextWidth(UI_10_FONT_ID, actionLabels[option])) / 2;
        const int textY = button.y + (button.height - lineHeight) / 2;
        renderer.drawText(UI_10_FONT_ID, textX, textY, actionLabels[option], !selected);
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));

    if (mappedInput.hasTouch()) {
      const auto actions = noRemoteProgressActionLayout(screen, metrics);
      const char* actionLabels[] = {tr(STR_UPLOAD), tr(STR_CANCEL)};
      TouchActionButtons::draw(renderer, actions, actionLabels, 0, -1, UI_10_FONT_ID);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_ALREADY_SYNCED), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
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

  // The title band's back button, on the devices that have touch.
  const bool backRequested = mappedInput.wasReleased(MappedInputManager::Button::Back) ||
                             TouchHeaderBackButton::wasTapped(mappedInput, headerBandRect());

  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    // Armed only by the smart outcomes: the screen already said everything it
    // has to say, so it walks back to the book on its own.
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToReader();
      return;
    }
    if (backRequested) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
#ifdef SIMULATOR
    // Integration-test hook: synthetic input cannot be scheduled across the
    // silent network reboot (the simulator only promotes after-wake input
    // scripts on deep-sleep wakes), so the harness answers the choice screen
    // through the environment instead.
    if (const char* choice = std::getenv("CROSSINK_SIM_BOOKORBIT_CHOICE")) {
      LOG_INF("BookOrbit", "Simulator scripted choice: %s", choice);
      if (std::strcmp(choice, "upload") == 0) {
        performUpload();
      } else {
        saveProgressAndReturn(remotePosition);
      }
      return;
    }
#endif
    // Touch: pressing a button highlights it, releasing on it takes the choice.
    // The same layout the draw used, so a translated label cannot move one
    // without the other.
    if (mappedInput.hasTouchHardware()) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const Rect header = headerBandRect();
      const auto actions = resultActionLayout(screen, metrics, header.y + header.height + metrics.verticalSpacing,
                                              renderer.getLineHeight(UI_10_FONT_ID), true);
      int touchedOption = -1;
      const auto touch =
          mappedInput.rowTouch(touchedOption, actions.buttons[0].y, actions.rowStep, 2, actions.buttons[0].x,
                               actions.buttons[0].x + actions.buttons[0].width, actions.rowHeight);
      if (touch == MappedInputManager::RowTouch::Down) {
        if (selectedOption != touchedOption) {
          selectedOption = touchedOption;
          requestUpdate();
        }
        return;
      }
      if (touch == MappedInputManager::RowTouch::Tap) {
        selectedOption = touchedOption;
        if (selectedOption == 0) {
          saveProgressAndReturn(remotePosition);
        } else {
          performUpload();
        }
        return;
      }
    }

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

    if (backRequested) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
#ifdef SIMULATOR
    // Scripted runs answer the "no progress on the server yet" prompt too;
    // uploading is its only affirmative action, whatever the choice value.
    if (std::getenv("CROSSINK_SIM_BOOKORBIT_CHOICE") != nullptr) {
      if (documentHash.empty()) {
        documentHash = KOReaderDocumentId::calculate(epubPath);
      }
      performUpload();
      return;
    }
#endif
    if (mappedInput.hasTouch()) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const auto actions = noRemoteProgressActionLayout(screen, metrics);
      int touchedOption = -1;
      const auto touch = mappedInput.rowTouch(
          touchedOption, actions.buttons[0].y, TouchActionButtons::kDefaultHeight + TouchActionButtons::kDefaultGap,
          actions.count, actions.buttons[0].x, actions.buttons[0].x + actions.buttons[0].width,
          actions.buttons[0].height);
      if (touch == MappedInputManager::RowTouch::Down) return;
      if (touch == MappedInputManager::RowTouch::Tap) {
        if (touchedOption == 0) {
          if (documentHash.empty()) {
            documentHash = KOReaderDocumentId::calculate(epubPath);
          }
          performUpload();
        } else if (touchedOption == 1) {
          returnToReader();
        }
        return;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (documentHash.empty()) {
        documentHash = KOReaderDocumentId::calculate(epubPath);
      }
      performUpload();
    }

    if (backRequested) {
      returnToReader();
    }
    return;
  }
}
