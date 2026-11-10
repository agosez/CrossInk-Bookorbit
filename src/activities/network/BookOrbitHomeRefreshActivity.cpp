#include "BookOrbitHomeRefreshActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WallClock.h>

#include "BookOrbitHomeStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/themes/bookorbit/BookOrbitHomeLayout.h"
#include "network/SavedWifiConnect.h"

namespace {
constexpr unsigned long FAILURE_MESSAGE_MS = 2500;
}

void BookOrbitHomeRefreshActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
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

  SavedWifiConnect::disconnect();

  if (refreshed && BookOrbitHomeStore::hasPendingCovers()) {
    showStep(State::DecodingCovers);
    BookOrbitHomeStore::decodePendingCovers(nullptr, sizes);
  }
  LOG_INF("BookOrbit", "Home refresh over (ok=%d, free=%u maxAlloc=%u)", refreshed, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  if (refreshed) {
    silentRestart();
    return;
  }
  state = State::Failed;
  failedAt = millis();
  requestUpdate();
}

void BookOrbitHomeRefreshActivity::loop() {
  if (refreshPending) {
    refreshPending = false;
    runRefresh();
    return;
  }
  if (state == State::Failed && millis() - failedAt >= FAILURE_MESSAGE_MS) silentRestart();
}

void BookOrbitHomeRefreshActivity::render(RenderLock&&) {
  const char* message = tr(STR_LOADING_POPUP);
  switch (state) {
    case State::Connecting:
      break;
    case State::Refreshing:
      message = tr(STR_HOME_FETCHING_STATS);
      break;
    case State::DecodingCovers:
      message = tr(STR_HOME_PREPARING_COVERS);
      break;
    case State::Failed:
      message = tr(STR_BOOKORBIT_CATALOG_ERROR);
      break;
  }
  renderer.clearScreen();
  GUI.drawPopup(renderer, message);
  renderer.displayBuffer();
}
