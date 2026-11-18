#include "BookOrbitHomeRefreshActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WallClock.h>

#include <algorithm>
#include <cstdio>

#include "BookOrbitHomeStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/themes/bookorbit/BookOrbitHomeLayout.h"
#include "fontIds.h"
#include "network/SavedWifiConnect.h"

namespace {
constexpr unsigned long FAILURE_MESSAGE_MS = 2500;
constexpr unsigned long SUMMARY_MESSAGE_MS = 2500;
// Each repaint is an e-ink refresh the scan waits for: a few seconds apart.
constexpr unsigned long SCAN_REPAINT_MS = 2000;
constexpr int PROGRESS_BAR_HEIGHT = 16;
}  // namespace

void BookOrbitHomeRefreshActivity::onEnter() {
  Activity::onEnter();
  // This boot's onEnter() can run inside setup(): the scan waits for the first loop().
  scanPending = true;
  requestUpdate();
}

void BookOrbitHomeRefreshActivity::connect() {
  // The BookOrbit theme's sync also recognises books copied to the card; its offline half
  // (the Library index, the hashes) runs before Wi-Fi takes its share of the heap.
  const BookOrbitDeviceScan::Progress progress{this, &BookOrbitHomeRefreshActivity::onScanProgress};
  BookOrbitDeviceScan::prepare(nullptr, &progress);
  // Shown again if the Wi-Fi screen hands back before the refresh starts.
  state = State::Connecting;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             silentRestart();
                             return;
                           }
                           // Out of this callback: the session belongs on loop()'s stack.
                           refreshPending = true;
                         });
}

void BookOrbitHomeRefreshActivity::showStep(const State step) {
  state = step;
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("BookOrbit", "Home refresh step %d could not be shown", static_cast<int>(step));
  }
}

void BookOrbitHomeRefreshActivity::onScanProgress(void* context, const BookOrbitDeviceScan::Step step,
                                                  const size_t done, const size_t total) {
  auto* self = static_cast<BookOrbitHomeRefreshActivity*>(context);
  const bool newStep = self->state != State::Scanning || step != self->scanStep;
  if (!newStep && millis() - self->scanPaintedAt < SCAN_REPAINT_MS) return;
  self->scanStep = step;
  self->scanDone = done;
  self->scanTotal = total;
  self->showStep(State::Scanning);
  // Counted from the end of the paint, so a slow panel still leaves the scan most of the time.
  self->scanPaintedAt = millis();
}

void BookOrbitHomeRefreshActivity::setScanSummary(const BookOrbitDeviceScan::Summary& summary) {
  summaryLines[0].clear();
  summaryLines[1].clear();
  if (summary.recognised == 0) return;
  char line[96];
  snprintf(line, sizeof(line), tr(STR_BOOKORBIT_SCAN_RESULT_FORMAT), static_cast<int>(summary.recognised),
           static_cast<int>(summary.moved));
  summaryLines[0] = line;
  if (summary.duplicates > 0) {
    snprintf(line, sizeof(line), tr(STR_BOOKORBIT_SCAN_DUPLICATES_FORMAT), static_cast<int>(summary.duplicates));
    summaryLines[1] = line;
  }
}

void BookOrbitHomeRefreshActivity::runRefresh() {
  sdFontSystem.releaseForNetwork(renderer);
  showStep(State::Refreshing);

  // The snapshot's age and the reading days depend on the date. WifiSelectionActivity has
  // just set the system clock on a board without a clock chip; a board with one only asks
  // NTP once, so its system clock is set here.
  if (halClock.isAvailable()) halClock.syncSystemTimeFromNTP();
  WallClock::checkpoint();

  const auto sizes = BookOrbitHomeLayout::coverSizes(renderer);
  const bool refreshed = BookOrbitHomeStore::refresh(nullptr, sizes);
  if (refreshed) {
    const BookOrbitDeviceScan::Progress progress{this, &BookOrbitHomeRefreshActivity::onScanProgress};
    setScanSummary(BookOrbitDeviceScan::run(nullptr, &progress));
  }

  SavedWifiConnect::disconnect();

  if (refreshed && BookOrbitHomeStore::hasPendingCovers()) {
    showStep(State::DecodingCovers);
    BookOrbitHomeStore::decodePendingCovers(nullptr, sizes);
  }
  LOG_INF("BookOrbit", "Home refresh over (ok=%d, free=%u maxAlloc=%u)", refreshed, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  if (refreshed && summaryLines[0].empty()) {
    silentRestart();
    return;
  }
  if (refreshed) {
    // Books moved or deleted on the card: say so before Home shows them in their new place.
    LOG_INF("BookOrbit", "Home refresh: showing the card scan's summary");
    state = State::ScanSummary;
    summaryAt = millis();
  } else {
    state = State::Failed;
    failedAt = millis();
  }
  requestUpdate();
}

void BookOrbitHomeRefreshActivity::loop() {
  if (scanPending) {
    scanPending = false;
    connect();
    return;
  }
  if (refreshPending) {
    refreshPending = false;
    runRefresh();
    return;
  }
  if (state == State::Failed && millis() - failedAt >= FAILURE_MESSAGE_MS) silentRestart();
  if (state == State::ScanSummary && millis() - summaryAt >= SUMMARY_MESSAGE_MS) silentRestart();
}

void BookOrbitHomeRefreshActivity::render(RenderLock&&) {
  char line[96];
  const char* status = tr(STR_LOADING_POPUP);
  bool showSummary = false;
  size_t done = 0;
  size_t total = 0;
  switch (state) {
    case State::Connecting:
      break;
    case State::Scanning:
      switch (scanStep) {
        case BookOrbitDeviceScan::Step::ReadingCard:
          status = tr(STR_LIBRARY_SCANNING);
          if (scanDone > 0) {
            snprintf(line, sizeof(line), tr(STR_LIBRARY_SCAN_COUNT), static_cast<unsigned>(scanDone));
            status = line;
          }
          break;
        case BookOrbitDeviceScan::Step::SortingCard:
          status = tr(STR_LIBRARY_ORGANIZING);
          break;
        case BookOrbitDeviceScan::Step::Hashing:
          status = tr(STR_BOOKORBIT_SCAN_FINDING);
          done = scanDone;
          total = scanTotal;
          break;
        case BookOrbitDeviceScan::Step::Filing:
          status = tr(STR_BOOKORBIT_SCAN_FILING);
          done = scanDone;
          total = scanTotal;
          break;
      }
      break;
    case State::Refreshing:
      status = tr(STR_HOME_FETCHING_STATS);
      break;
    case State::DecodingCovers:
      status = tr(STR_HOME_PREPARING_COVERS);
      showSummary = true;
      break;
    case State::ScanSummary:
      status = tr(STR_BOOKORBIT_SCAN_DONE);
      showSummary = true;
      break;
    case State::Failed:
      status = tr(STR_BOOKORBIT_CATALOG_ERROR);
      break;
  }

  // The book sync's layout: title band, then the step around the middle of the screen,
  // wrapped rather than cut at the screen's edges.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect text{screen.x + metrics.contentSidePadding, screen.y,
                  std::max(1, screen.width - metrics.contentSidePadding * 2), screen.height};
  const int gap = metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_BOOKORBIT_SYNC));
  int y = screen.y + screen.height / 2 - 40;
  y += UITheme::drawCenteredWrappedText(renderer, text, UI_10_FONT_ID, y, status, 3, true, EpdFontFamily::BOLD) + gap;
  if (total > 0) {
    snprintf(line, sizeof(line), "%u / %u", static_cast<unsigned>(done), static_cast<unsigned>(total));
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, y, line);
    y += lineHeight + gap;
    const int barWidth = screen.width / 2;
    GUI.drawProgressBar(renderer, Rect{screen.x + (screen.width - barWidth) / 2, y, barWidth, PROGRESS_BAR_HEIGHT},
                        done, total);
    y += PROGRESS_BAR_HEIGHT + gap;
  }
  for (const std::string& summary : summaryLines) {
    if (!showSummary || summary.empty()) continue;
    y += UITheme::drawCenteredWrappedText(renderer, text, UI_10_FONT_ID, y, summary.c_str(), 3) + gap;
  }
  renderer.displayBuffer();
}
