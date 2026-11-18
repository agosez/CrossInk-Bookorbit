#include "BookOrbitSyncActivity.h"

#include <GfxRenderer.h>

#ifdef SIMULATOR
#include <cstdlib>
#include <cstring>
#endif
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WallClock.h>
#include <WiFi.h>

#include <algorithm>
#include <cassert>
#include <ctime>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitDeviceScan.h"
#include "BookOrbitDownloadIndex.h"
#include "BookOrbitHomeStore.h"
#include "BookOrbitRemotePosition.h"
#include "BookOrbitStatsUpload.h"
#include "BookOrbitSyncMarkerFile.h"
#include "BookOrbitSyncScreens.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/browser/BookOrbitCatalogLink.h"
#include "activities/browser/BookOrbitCatalogListCache.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReaderUtils.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/themes/bookorbit/BookOrbitHomeLayout.h"
#include "fontIds.h"
#include "network/SavedWifiConnect.h"
#include "util/BookContentId.h"

// This file mirrors src/activities/reader/KOReaderSyncActivity.cpp; see that file's
// comments for the rationale behind the heap/TLS-related steps. Kept as a separate,
// independent activity (rather than parameterizing the KOReader one) so BookOrbit
// support cannot regress the existing generic KOReader sync path.

namespace {
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

#ifdef SIMULATOR
// Integration-test hook (test/integration/): every sync outcome funnels through
// returnToReader(), and every sleep sync through the end of runSleepSync(), once its
// files and uploads are settled. Those are the deterministic places a scripted run can
// end at. The state says how it went.
void endSimulatorScenarioIfRequested(const int state) {
  if (std::getenv("CROSSINK_SIM_BOOKORBIT_QUIT_AFTER_SYNC") != nullptr) {
    LOG_INF("BookOrbit", "Simulator sync scenario finished (state=%d)", state);
    std::_Exit(0);
  }
}
#endif
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

void BookOrbitSyncActivity::setStatus(const char* message) {
  {
    RenderLock lock(*this);
    statusMessage = message;
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::fail(const char* message) {
  {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    statusMessage = message;
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::beginPhase(const char* status, const size_t total) {
  {
    RenderLock lock(*this);
    statusMessage = status;
    progressDone = 0;
    progressTotal = total;
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::reportProgress(const size_t done, const size_t total) {
  {
    RenderLock lock(*this);
    progressDone = done;
    progressTotal = total;
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::endPhase() {
  RenderLock lock(*this);
  progressDone = 0;
  progressTotal = 0;
}

bool BookOrbitSyncActivity::wakeRequested() const { return sleepSync && sleepSyncWakeRequested(); }

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
    fail(tr(STR_SAVE_PROGRESS_FAILED));
    return;
  }
  RecentBookProgress::saveCachedEpubPercent(*epub, position.spineIndex, position.pageNumber, pageCount);
  // Manual applies record the marker too, so the history smart sync reads is
  // already there the day the option gets switched on.
  BookOrbitSyncMarkerFile::write(BookContentId::bookStateDir(epubPath), remoteProgress.timestamp,
                                 {position.spineIndex, position.pageNumber, pageCount});
  returnToReader();
}

void BookOrbitSyncActivity::returnToReader() {
  // The sleep sync has no reader to go back to: runSleepSync() closes the session and
  // puts the device back to sleep once the attempt unwinds. It never offers an update: the
  // next manual sync does.
  if (sleepSync) return;
  if (offerBookUpdate()) return;
#ifdef SIMULATOR
  endSimulatorScenarioIfRequested(static_cast<int>(state));
#endif
  syncSession.reset();
  activityManager.goToReader(epubPath);
}

void BookOrbitSyncActivity::resolveUpdateBook() {
  int64_t bookId = 0;
  if (!BookOrbitDownloadIndex::bookIdForPath(epubPath, bookId)) bookId = 0;
  BookOrbitDownloadIndex::unload();
  if (bookId == 0) {
    // Not a catalog download (copied to the card, or the index lost it): the server still
    // resolves this copy's older hash to its book. A bare hash stays out of Books to Review.
    const BookOrbitSyncClient::MatchQuery query{documentHash.c_str(), nullptr, nullptr};
    char libraryVersion[BookOrbitSyncClient::LIBRARY_VERSION_SIZE] = {};
    BookOrbitSyncClient::matchCheck(
        SETTINGS.getEffectiveDeviceName(), &query, 1,
        [](const char*, const int64_t matchedBookId, int64_t, void* context) {
          *static_cast<int64_t*>(context) = matchedBookId;
        },
        &bookId, libraryVersion);
  }
  updateBookId = bookId;
  LOG_INF("BookOrbit", "This copy differs from the library file (book %lld)", static_cast<long long>(updateBookId));
}

bool BookOrbitSyncActivity::offerBookUpdate() {
  if (updateOffered || !documentIncompatible()) return false;
  updateOffered = true;
  syncSession.reset();  // nothing more goes over it; the update restarts into the catalog
  {
    RenderLock lock(*this);
    state = UPDATE_OFFER;
  }
  requestUpdate(true);
  return true;
}

void BookOrbitSyncActivity::startBookUpdate() {
  BookOrbitCatalogLink link;
  link.bookId = updateBookId;
  link.replacePath = epubPath;
  // Shown on the download screen; the file name is what this device calls the book.
  link.title = epubPath.substr(epubPath.find_last_of('/') + 1);
  const size_t dot = link.title.rfind('.');
  if (dot != std::string::npos) link.title.resize(dot);
  // Through the catalog's network boot rather than a pushed catalog: leaving this activity in
  // place would run onExit()'s restart into the reader. The catalog reads the link back,
  // downloads over this file and reopens it.
  if (!BookOrbitCatalogListCache::saveEntryLink(link)) {
    LOG_ERR("BookOrbit", "Could not save the catalog link to update this copy");
    returnToReader();
    return;
  }
  LOG_INF("BookOrbit", "Restarting into the catalog to update %s (book %lld)", epubPath.c_str(),
          static_cast<long long>(updateBookId));
  SavedWifiConnect::disconnect();
  silentRestartToNetwork(NetworkBootTarget::BOOKORBIT_CATALOG);
  // Only returns when deep sleep is already under way, which ends this session anyway.
  BookOrbitCatalogListCache::discardEntryLink();
}

void BookOrbitSyncActivity::handleUpdateOfferInput(const bool backRequested) {
  const bool canUpdate = updateBookId != 0;
#ifdef SIMULATOR
  // Scripted runs: CROSSINK_SIM_BOOKORBIT_UPDATE takes the update, any other scripted run
  // declines it and ends as usual.
  if (std::getenv("CROSSINK_SIM_BOOKORBIT_UPDATE") != nullptr && canUpdate) {
    startBookUpdate();
    return;
  }
  if (std::getenv("CROSSINK_SIM_BOOKORBIT_QUIT_AFTER_SYNC") != nullptr) {
    returnToReader();
    return;
  }
#endif
  if (canUpdate && mappedInput.hasTouch()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const auto actions = BookOrbitSyncScreens::noRemoteProgressActionLayout(screen, metrics);
    int touchedOption = -1;
    const auto touch = mappedInput.rowTouch(touchedOption, actions.buttons[0].y,
                                            TouchActionButtons::kDefaultHeight + TouchActionButtons::kDefaultGap,
                                            actions.count, actions.buttons[0].x,
                                            actions.buttons[0].x + actions.buttons[0].width, actions.buttons[0].height);
    if (touch == MappedInputManager::RowTouch::Down) return;
    if (touch == MappedInputManager::RowTouch::Tap) {
      if (touchedOption == 0) {
        startBookUpdate();
      } else if (touchedOption == 1) {
        returnToReader();
      }
      return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (canUpdate) {
      startBookUpdate();
    } else {
      returnToReader();
    }
    return;
  }
  if (backRequested) returnToReader();
}

void BookOrbitSyncActivity::runSleepSync() {
  LOG_INF("BookOrbit", "Sleep sync starting (free=%u maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  sdFontSystem.releaseLoadedFont(renderer);
  // The BookOrbit theme's sync also recognises books copied to the card. Its offline half
  // (the Library index, the hashes) runs before Wi-Fi takes its share of the heap.
  const bool bookOrbitHome = UITheme::hasBookOrbitHome();
  if (bookOrbitHome) BookOrbitDeviceScan::prepare(&sleepSyncWakeRequested);
  wifiActivated = true;
  bool wifiConnectedForSleepSync = false;
  if (SavedWifiConnect::connect(&sleepSyncWakeRequested)) {
    wifiConnectedForSleepSync = true;
    // The whole sync runs synchronously from here, decisions included.
    onWifiSelectionComplete(true);
  } else if (!stopIfWoken()) {
    LOG_INF("BookOrbit", "Sleep sync skipped: no saved network answered");
  }
  syncSession.reset();
  // The BookOrbit Home draws from what this boot's network leaves behind. Each step is
  // retried by the next sync, so a wake press can end it anywhere. Its tile dates the
  // snapshot as the last sync: a sync that failed for the book leaves it as it was, or
  // the tile would report as done a sync whose progress never reached the server.
  const bool homeShown = wifiConnectedForSleepSync && bookOrbitHome;
  const bool refreshHome = homeShown && state != SYNC_FAILED;
  if (homeShown && !refreshHome) LOG_INF("BookOrbit", "Sleep sync failed: the Home snapshot stays as it was");
  const auto homeCoverSizes = BookOrbitHomeLayout::coverSizes(renderer);
  if (refreshHome && !sleepSyncWakeRequested()) BookOrbitHomeStore::refresh(&sleepSyncWakeRequested, homeCoverSizes);
  if (refreshHome && !sleepSyncWakeRequested()) BookOrbitDeviceScan::run(&sleepSyncWakeRequested);
  SavedWifiConnect::disconnect();
  if (refreshHome && !sleepSyncWakeRequested()) {
    BookOrbitHomeStore::decodePendingCovers(&sleepSyncWakeRequested, homeCoverSizes);
  }
  LOG_INF("BookOrbit", "Sleep sync over (state=%d)", static_cast<int>(state));
#ifdef SIMULATOR
  endSimulatorScenarioIfRequested(static_cast<int>(state));
#endif
  completeSleepAfterSleepSync();
}

bool BookOrbitSyncActivity::stopIfWoken() {
  if (!wakeRequested()) return false;
  // Checked only between steps, where every file the sync touches is consistent: each
  // step is retried by the next sync, so the attempt can simply end here.
  LOG_INF("BookOrbit", "Sleep sync cancelled: the device was woken");
  wakeFromSleepSync();
  return true;
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
  if (stopIfWoken()) return;

  setStatus(tr(STR_CALC_HASH));

  performSync();
}

void BookOrbitSyncActivity::performSync() {
  // BookOrbit only supports the binary partial-MD5 document hash (no filename option).
  documentHash = KOReaderDocumentId::calculate(epubPath);
  if (documentHash.empty()) {
    fail(tr(STR_HASH_FAILED));
    return;
  }

  LOG_DBG("BookOrbit", "Document hash: %s", documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BookOrbit", "Fetch progress screen could not be rendered synchronously; aborting sync");
    SavedWifiConnect::disconnect();
    fail(tr(STR_SYNC_FAILED_MSG));
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
    if (!exchangeReadingData()) return;
  }

  if (result == BookOrbitSyncClient::NOT_FOUND) {
    if (smartSyncEnabled() || sleepSync) {
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
    fail(BookOrbitSyncClient::errorString(result).c_str());
    return;
  }

  resolveRemoteProgress();
}

bool BookOrbitSyncActivity::exchangeReadingData() {
  const size_t statsAccepted = BookOrbitStatsUpload::uploadQueued(epubPath, documentHash, *this);
  if (stopIfWoken()) return false;

  setStatus(tr(STR_SYNCING_HIGHLIGHTS));

  const bool serverAcceptsSidecar = BookOrbitSyncClient::acceptsSidecarState();
  highlights.prepare(serverAcceptsSidecar);
  bookmarks.prepare(serverAcceptsSidecar);
  // The server's changes come down first, then this device's go up. Applied before the
  // returns in performSync(), not after: they leave early for a book the server holds no
  // progress for, and the highlights it just sent would leave with them.
  if (highlights.exchangeKeys()) {
    if (highlights.hasIncoming()) {
      ensureEpubLoaded();
      if (epub) {
        highlights.drainIncoming(*epub, *this);
        // Released for the uploads, which never ran beside it; ensureEpubLoaded() brings it
        // back for the bookmarks and the progress.
        epub.reset();
      } else {
        LOG_ERR("BookOrbit", "Cannot place server highlights without the epub; they stay pending");
        highlights.discardIncoming();
      }
    }
    // What the server sent is acknowledged only once applied, so stopping before that leaves
    // it to be offered again.
    if (stopIfWoken()) return false;
    highlights.sendBacklog(*this);
  }
  if (stopIfWoken()) return false;
  setStatus(tr(STR_SYNCING_BOOKMARKS));
  bookmarks.exchange();
  if (stopIfWoken()) return false;
  if (bookmarks.hasIncoming()) {
    ensureEpubLoaded();
    if (epub) {
      bookmarks.applyIncoming(epub, renderer);
    } else {
      LOG_ERR("BookOrbit", "Cannot place server bookmarks without the epub; they stay pending");
    }
  }

  showExchangeSummaries();
  if (stopIfWoken()) return false;
  // While the session is open: the offer at the end of the sync needs the book to download.
  if (!sleepSync && documentIncompatible()) resolveUpdateBook();

  recordSweep(statsAccepted);
  return !stopIfWoken();
}

void BookOrbitSyncActivity::showExchangeSummaries() {
  // Held on screen for a moment: the progress decision replaces this view immediately after,
  // and a count that flashes past is no better than no count at all. Silent when nothing
  // happened, so an ordinary sync does not grow an extra pause.
  if (highlights.changedAnything()) {
    char received[64];
    char sent[64];
    snprintf(received, sizeof(received), tr(STR_HIGHLIGHTS_RECEIVED_FORMAT), static_cast<int>(highlights.added()),
             static_cast<int>(highlights.removed()));
    snprintf(sent, sizeof(sent), tr(STR_HIGHLIGHTS_SENT_FORMAT), static_cast<int>(highlights.sent()),
             static_cast<int>(highlights.deletedOnServer()));
    LOG_INF("BookOrbit", "Highlights received +%u -%u, sent +%u -%lu", (unsigned)highlights.added(),
            (unsigned)highlights.removed(), (unsigned)highlights.sent(),
            static_cast<unsigned long>(highlights.deletedOnServer()));
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_HIGHLIGHTS_SYNCED);
      summaryLines[0] = received;
      summaryLines[1] = sent;
    }
    requestUpdateAndWait();
    if (!sleepSync) delay(1500);
    RenderLock lock(*this);
    summaryLines[0].clear();
    summaryLines[1].clear();
  }
  if (bookmarks.changedAnything()) {
    char summary[96];
    snprintf(summary, sizeof(summary), tr(STR_BOOKMARKS_SYNCED_FORMAT), static_cast<int>(bookmarks.sent()),
             static_cast<int>(bookmarks.added()), static_cast<int>(bookmarks.removed()));
    {
      RenderLock lock(*this);
      statusMessage = summary;
    }
    requestUpdateAndWait();
    if (!sleepSync) delay(1500);
  }
}

void BookOrbitSyncActivity::recordSweep(const size_t statsAccepted) {
  // Recorded before any progress push: the push is what triggers the server's session
  // estimation, and a sweep already on file is what suppresses it (and retires the
  // duplicate estimates earlier syncs may have left). A failure never fails the sync —
  // the next sync records another one, and the server's sweep window spans many syncs.
  const auto sweepResult =
      BookOrbitSyncClient::completeSweep(SETTINGS.getEffectiveDeviceName(), highlights.documentUnmatched() ? 0 : 1,
                                         static_cast<uint32_t>(statsAccepted), highlights.sent());
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

void BookOrbitSyncActivity::resolveRemoteProgress() {
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    syncSession.reset();
    fail("");
    return;
  }

  remotePosition =
      BookOrbitRemotePosition::toLocal(epub, renderer, remoteProgress, currentSpineIndex, totalPagesInSpine);
  remoteLocalPercent = BookOrbitRemotePosition::localPercent(*epub, remotePosition);
  const BookOrbitSmartSync::Position local{currentSpineIndex, currentPage, totalPagesInSpine};
  const BookOrbitSmartSync::Position remote{remotePosition.spineIndex, remotePosition.pageNumber,
                                            remotePosition.totalPages};

  if ((smartSyncEnabled() || sleepSync) && decideAutomatically(local, remote)) return;

  if (sleepSync) {
    // Nobody is there to answer the choice screen; the next manual sync asks it.
    LOG_INF("BookOrbit", "Sleep sync: progress left for the next manual sync");
    syncSession.reset();
    return;
  }

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
    selectedOption = BookOrbitSmartSync::isAhead(local, remote) ? 1 : 0;  // 1 = Upload local, 0 = Apply remote
  }
  requestUpdate(true);
}

bool BookOrbitSyncActivity::decideAutomatically(const BookOrbitSmartSync::Position& local,
                                                const BookOrbitSmartSync::Position& remote) {
  const std::string stateDir = BookContentId::bookStateDir(epubPath);
  const BookOrbitSmartSync::Marker lastSync = BookOrbitSyncMarkerFile::read(stateDir);
  LOG_DBG("BookOrbit",
          "Smart decision: local spine=%d page=%d, remote spine=%d page=%d/%d, serverTs=%lld; last sync ts=%lld "
          "spine=%u page=%u/%u",
          local.spineIndex, local.pageNumber, remote.spineIndex, remote.pageNumber, remote.pageCount,
          (long long)remoteProgress.timestamp, (long long)lastSync.timestamp, (unsigned)lastSync.spineIndex,
          (unsigned)lastSync.pageNumber, (unsigned)lastSync.pageCount);
  switch (BookOrbitSmartSync::decide(local, remote, remoteProgress.timestamp, lastSync)) {
    case BookOrbitSmartSync::Decision::AlreadySynced:
      // Both sides agree; refresh the marker so the next visit still knows
      // whether the server moved in the meantime.
      BookOrbitSyncMarkerFile::write(stateDir, remoteProgress.timestamp, BookOrbitSmartSync::furtherOf(local, remote));
      completeAlreadySynced();
      return true;
    case BookOrbitSmartSync::Decision::ApplyRemote:
      saveProgressAndReturn(remotePosition);
      return true;
    case BookOrbitSmartSync::Decision::UploadLocal:
      performUpload();
      return true;
    case BookOrbitSmartSync::Decision::AskNoHistory:
      LOG_DBG("BookOrbit", "Smart sync: no sync history for this book yet, showing the choice screen");
      return false;
    case BookOrbitSmartSync::Decision::AskConflict:
      LOG_DBG("BookOrbit", "Smart sync: progress and sync history disagree, showing the choice screen");
      return false;
  }
  return false;
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
    SavedWifiConnect::disconnect();
    fail(tr(STR_SYNC_FAILED_MSG));
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

  if (stopIfWoken()) return;
  const auto result = BookOrbitSyncClient::updateProgress(progress);

  syncSession.reset();
  // The sleep sync refreshes Home on this connection next; runSleepSync() closes it.
  if (!sleepSync) SavedWifiConnect::disconnect();

  if (result != BookOrbitSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = BookOrbitSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  BookOrbitSyncMarkerFile::write(BookContentId::bookStateDir(epubPath), progress.timestamp,
                                 {currentSpineIndex, currentPage, totalPagesInSpine});
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  if (smartSyncEnabled()) {
    markAutoReturn();
  }
  requestUpdate(true);
}

void BookOrbitSyncActivity::uploadFromNoRemotePrompt() {
  if (documentHash.empty()) {
    documentHash = KOReaderDocumentId::calculate(epubPath);
  }
  performUpload();
}

void BookOrbitSyncActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("BookOrbit", "BookOrbit sync starting");

  // Already in its network boot, with no screen to orient or touch to route. The attempt
  // waits for loop(): at boot this onEnter() runs inside setup(), whose frames would sit
  // under the TLS session on the loop task's 8 KB stack.
  if (sleepSync) return;

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
    SavedWifiConnect::disconnect();
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

void BookOrbitSyncActivity::drawHints(const char* confirm, const char* up, const char* down) {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

void BookOrbitSyncActivity::render(RenderLock&&) {
  // The sleep screen stays on the panel for the whole sleep sync.
  if (sleepSync) return;

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  // On touch devices the title band carries the way back: the result screen's
  // choices are the only other tap targets, and the rest have none at all.
  const Rect header = headerBandRect();
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_BOOKORBIT_SYNC), /*readerContext=*/true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BOOKORBIT_SYNC));
  }

  const int top = screen.y + screen.height / 2 - 40;
  switch (state) {
    case NO_CREDENTIALS:
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                                EpdFontFamily::BOLD);
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_BOOKORBIT_SETUP_HINT), true,
                                EpdFontFamily::BOLD);
      drawHints();
      break;
    case SYNCING:
    case UPLOADING:
      BookOrbitSyncScreens::drawStatus(renderer, screen, metrics, top, statusMessage.c_str(), summaryLines,
                                       progressDone, progressTotal);
      break;
    case SHOWING_RESULT:
      renderResult(screen, metrics, header.y + header.height + metrics.verticalSpacing);
      break;
    case NO_REMOTE_PROGRESS:
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));
      if (mappedInput.hasTouch()) {
        const auto actions = BookOrbitSyncScreens::noRemoteProgressActionLayout(screen, metrics);
        const char* actionLabels[] = {tr(STR_UPLOAD), tr(STR_CANCEL)};
        TouchActionButtons::draw(renderer, actions, actionLabels, 0, -1, UI_10_FONT_ID);
      }
      drawHints(tr(STR_UPLOAD));
      break;
    case SYNC_COMPLETE:
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_ALREADY_SYNCED), true,
                                EpdFontFamily::BOLD);
      drawHints();
      break;
    case UPLOAD_COMPLETE:
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_UPLOAD_SUCCESS), true,
                                EpdFontFamily::BOLD);
      drawHints();
      break;
    case SYNC_FAILED:
      BookOrbitSyncScreens::drawFailure(renderer, screen, metrics, top, statusMessage.c_str());
      drawHints();
      break;
    case UPDATE_OFFER: {
      const bool canUpdate = updateBookId != 0;
      BookOrbitSyncScreens::drawUpdateOffer(renderer, screen, metrics, top, canUpdate && mappedInput.hasTouch());
      const auto labels =
          mappedInput.mapLabels(canUpdate ? tr(STR_NOT_NOW) : tr(STR_BACK), canUpdate ? tr(STR_UPDATE) : "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
      break;
    }
    case WIFI_SELECTION:
    case CONNECTING:
      // Nothing of this activity's own is shown before the sync starts.
      return;
  }
  renderer.displayBuffer();
}

void BookOrbitSyncActivity::renderResult(const Rect& screen, const ThemeMetrics& metrics, const int top) {
  const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
  const std::string remoteChapter =
      (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                            : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
  const std::string localChapter = !localChapterName.empty()
                                       ? localChapterName
                                       : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

  char remotePageStr[48];
  snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_SYNC_PAGE_FORMAT), remotePosition.pageNumber + 1);
  char localPageStr[48];
  snprintf(localPageStr, sizeof(localPageStr), tr(STR_SYNC_PAGE_TOTAL_FORMAT), currentPage + 1, totalPagesInSpine);

  // remoteLocalPercent, not the server's raw percentage: the raw value comes from the
  // sender's engine and is not comparable with the local card drawn beside it.
  const BookOrbitSyncScreens::ProgressCard remote{tr(STR_SYNC_REMOTE_TITLE), remoteProgress.device.c_str(),
                                                  remotePageStr, remoteChapter.c_str(), remoteLocalPercent};
  const BookOrbitSyncScreens::ProgressCard local{tr(STR_SYNC_LOCAL_TITLE), nullptr, localPageStr, localChapter.c_str(),
                                                 localProgress.percentage};
  BookOrbitSyncScreens::drawResult(renderer, screen, metrics, top, remote, local, selectedOption,
                                   mappedInput.hasTouchHardware());
  drawHints(tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
}

void BookOrbitSyncActivity::loop() {
  if (sleepSync) {
    runSleepSync();  // ends in deep sleep or in a wake restart
    return;
  }

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
    handleResultInput(backRequested);
    return;
  }

  if (state == UPDATE_OFFER) {
    handleUpdateOfferInput(backRequested);
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    handleNoRemoteProgressInput(backRequested);
  }
}

void BookOrbitSyncActivity::handleResultInput(const bool backRequested) {
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
    const auto actions = BookOrbitSyncScreens::resultActionLayout(screen, metrics, true);
    const Rect& first = actions.buttons[0];
    int touchedOption = -1;
    const auto touch = mappedInput.rowTouch(touchedOption, first.y, actions.buttons[1].y - first.y, actions.count,
                                            first.x, first.x + first.width, first.height);
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
}

void BookOrbitSyncActivity::handleNoRemoteProgressInput(const bool backRequested) {
#ifdef SIMULATOR
  // Scripted runs answer the "no progress on the server yet" prompt too;
  // uploading is its only affirmative action, whatever the choice value.
  if (std::getenv("CROSSINK_SIM_BOOKORBIT_CHOICE") != nullptr) {
    uploadFromNoRemotePrompt();
    return;
  }
#endif
  if (mappedInput.hasTouch()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const auto actions = BookOrbitSyncScreens::noRemoteProgressActionLayout(screen, metrics);
    int touchedOption = -1;
    const auto touch = mappedInput.rowTouch(touchedOption, actions.buttons[0].y,
                                            TouchActionButtons::kDefaultHeight + TouchActionButtons::kDefaultGap,
                                            actions.count, actions.buttons[0].x,
                                            actions.buttons[0].x + actions.buttons[0].width, actions.buttons[0].height);
    if (touch == MappedInputManager::RowTouch::Down) return;
    if (touch == MappedInputManager::RowTouch::Tap) {
      if (touchedOption == 0) {
        uploadFromNoRemotePrompt();
      } else if (touchedOption == 1) {
        returnToReader();
      }
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    uploadFromNoRemotePrompt();
  }

  if (backRequested) {
    returnToReader();
  }
}
