#include "BookOrbitSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WallClock.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <ctime>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitStatsQueue.h"
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
void syncTimeWithNTP() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  int retry = 0;
  const int maxRetries = 50;  // 5 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry < maxRetries) {
    LOG_DBG("BookOrbit", "NTP time synced");
  } else {
    LOG_DBG("BookOrbit", "NTP sync timeout, using fallback");
  }
}

void wifiOff() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
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

  const auto result = BookOrbitSyncClient::getProgress(documentHash, remoteProgress);
  LOG_INF("BookOrbit", "Progress fetch result=%d (http=%d)", static_cast<int>(result),
          BookOrbitSyncClient::lastHttpCode);

  // Progress fetch reaching the server (even with no stored progress) means auth and
  // connectivity are good: piggyback the queued reading-session stats on this session.
  if (result == BookOrbitSyncClient::OK || result == BookOrbitSyncClient::NOT_FOUND) {
    uploadQueuedStats();
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
