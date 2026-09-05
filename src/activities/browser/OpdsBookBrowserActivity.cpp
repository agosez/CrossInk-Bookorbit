#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr size_t OPDS_BROWSER_ENTRY_CAPACITY = MAX_OPDS_FEED_ENTRIES + 2;
constexpr size_t OPDS_DOWNLOAD_BUFFER_SIZE = 2048;
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_SEARCH = 2;
constexpr fui::ActionId ACTION_CANCEL = 3;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

std::string buildBookFilenameBase(const OpdsEntry& book, const OpdsFilenameFormat format) {
  if (book.author.empty()) return book.title;
  if (book.title.empty()) return book.author;
  if (format == OpdsFilenameFormat::TITLE_AUTHOR) return book.title + " - " + book.author;
  return book.author + " - " + book.title;
}

}  // namespace

OpdsBookBrowserActivity::OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 OpdsServer server)
    : Activity("OpdsBookBrowser", renderer, mappedInput),
      buttonNavigator(),
      server(std::move(server)),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  sdFontSystem.releaseLoadedFont(renderer);

  state = BrowserState::CHECK_WIFI;
  entryCount = 0;
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

  uiReady = false;
  visibleRows = 1;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &OpdsBookBrowserActivity::onRowEvent, this);
  app.on(ACTION_SEARCH, &OpdsBookBrowserActivity::onSearchEvent, this);
  app.on(ACTION_CANCEL, &OpdsBookBrowserActivity::onCancelEvent, this);
  app.setScreen(&OpdsBookBrowserActivity::rootScreen, this);
  requestUpdate();

  if (!ensureEntryBuffer()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

#ifdef SIMULATOR
  // Use deterministic catalog data so the UI can be exercised without WiFi or an OPDS server.
  fetchFeed(currentPath);
#else
  checkAndConnectWifi();
#endif
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  clearEntries();
  entries.reset();
  navigationHistory.clear();

#ifndef SIMULATOR
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // OPDS launches from minimal network boot, so restore the full app state
  // even if setup failed before WiFi was started.
  silentRestartAfterNetwork();
#endif
}

void OpdsBookBrowserActivity::activateSelected() {
  if (!entries || entryCount == 0 || selectorIndex < 0 || selectorIndex >= static_cast<int>(entryCount)) return;
  const auto& entry = entries[selectorIndex];
  entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
}

void OpdsBookBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->entryCount)) return;
  self->selectorIndex = event.value;
  // The tapped row leaves the screen either way (new feed or download view);
  // a lingering tap flash would gray an unrelated row on the next list.
  self->app.clearTapFlash();
  self->activateSelected();
}

void OpdsBookBrowserActivity::onSearchEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  self->app.clearTapFlash();
  self->launchSearch();
}

void OpdsBookBrowserActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (state == BrowserState::ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        showLoadingBeforeFetch();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
#ifdef SIMULATOR
    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) app.route(snap);
    }
    if (cancelDownload || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelDownload = false;
      state = BrowserState::BROWSING;
      requestUpdate();
    }
#endif
    return;
  }

  if (state == BrowserState::BROWSING) {
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
      navigateBack();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    // Touch goes through the FreeInkApp: render() registered every tap target
    // (rows, header search button); route the snapshot and let the registered
    // handlers dispatch.
    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) {
        const auto event = app.route(snap);
        // No pressed-state repaint: the render it triggers would drop a slow
        // tap's release inside the uiReady window (tap-to-activate needed two
        // taps), and it costs a second e-ink refresh per tap.
        if (app.invalidated()) requestUpdate();
        if (event) return;  // dispatched to onRowEvent/onSearchEvent
        if (state != BrowserState::BROWSING) return;
      }
    }

    if (entryCount > 0) {
      // Swipes scroll the viewport; the selection stays put (it may scroll
      // off-screen) and button navigation pulls the view back to it.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
        const int next = scrollListBy(topIndex, delta, visibleRows, static_cast<int>(entryCount));
        if (next != topIndex) {
          topIndex = next;
          requestUpdate();
        }
        return;
      }

      const auto moveSelection = [this](const int index) {
        selectorIndex = index;
        topIndex = followListSelection(selectorIndex, topIndex, visibleRows, static_cast<int>(entryCount));
        requestUpdate();
      };
      buttonNavigator.onNextRelease(
          [this, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, entryCount)); });
      buttonNavigator.onPreviousRelease(
          [this, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, entryCount)); });
      buttonNavigator.onNextContinuous([this, &moveSelection] {
        moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, entryCount, visibleRows));
      });
      buttonNavigator.onPreviousContinuous([this, &moveSelection] {
        moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, entryCount, visibleRows));
      });
    }
  }
}

bool OpdsBookBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
    case BrowserState::SEARCH_INPUT:
      return true;
    case BrowserState::BROWSING:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

void OpdsBookBrowserActivity::rootScreen(UiApp::ScreenType& screen, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  switch (self->state) {
    case BrowserState::BROWSING:
      self->buildBrowsingScreen(screen);
      break;
    case BrowserState::DOWNLOADING:
      self->buildDownloadScreen(screen);
      break;
    default:
      self->buildStatusScreen(screen);
      break;
  }
}

// Shared chrome for every state: reserve the firmware's button-hint band and
// draw the themed header (padding, centering, and rule come from the theme).
void OpdsBookBrowserActivity::screenHeader(UiApp::ScreenType& screen, const bool withSearch) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  const bool useTouchBackHeader = state == BrowserState::BROWSING && mappedInput.hasTouchHardware();
  if (useTouchBackHeader) {
    const Rect headerRect = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    const auto backLayout = TouchHeaderBackButton::layout(headerRect);
    const bool showSearch = withSearch && !searchTemplate.empty();
    TouchHeaderBackButton::draw(renderer, uiTarget, headerRect,
                                server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str(), false,
                                showSearch ? static_cast<int>(backLayout.iconRect.width + 8) : 0);
    screen.takeTop(static_cast<int16_t>(headerRect.height));

    if (showSearch) {
      fui::ButtonProps search;
      search.action = ACTION_SEARCH;
      search.styles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
      search.minTouchSize = screen.theme().minTouchSize;
      search.radius = 8;
      const fui::Rect searchRect{static_cast<int16_t>(headerRect.x + headerRect.width - backLayout.iconRect.width),
                                 static_cast<int16_t>(backLayout.iconRect.y),
                                 static_cast<int16_t>(backLayout.iconRect.width),
                                 static_cast<int16_t>(backLayout.iconRect.height)};
      screen.button(search, searchRect);
      // Keep the touch target clear of the divider, but draw the glyph on the
      // shared Back/title baseline instead of at the top of its action lane.
      const int16_t iconX =
          static_cast<int16_t>(searchRect.x + (searchRect.width - TouchHeaderBackButton::ICON_SIZE) / 2);
      const int16_t iconY = static_cast<int16_t>(backLayout.iconRect.y + TouchHeaderBackButton::TITLE_VERTICAL_OFFSET +
                                                 (backLayout.iconRect.height - TouchHeaderBackButton::ICON_SIZE) / 2);
      screen.target().bitmap(
          fui::Rect{iconX, iconY, TouchHeaderBackButton::ICON_SIZE, TouchHeaderBackButton::ICON_SIZE},
          fui::bitmapFromIcon(icon_search_32), fui::BitmapMode::Center, fui::Paint::solid(fui::Color::Black));
    }
  } else {
    fui::HeaderProps header;
    header.title = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
    header.borderEdges = fui::EdgeBottom;
    if (withSearch && !searchTemplate.empty()) {
      header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
      header.trailingAction = ACTION_SEARCH;
      // Optically align the icon with the title glyphs: text hangs low in its
      // line cell by the font's internal leading; drop the button to match.
      const int titleFontId = uiScaleSpec().titleFontId;
      header.actionOffsetY =
          static_cast<int16_t>((renderer.getLineHeight(titleFontId) - renderer.getTextHeight(titleFontId)) / 2);
    }
    screen.header(header);
  }
  // Same breathing room between header and content as the legacy screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, true);

  if (entryCount == 0) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  // Transient per-render: sized once via reserve, points into `entries`
  // strings, freed on scope exit.
  std::vector<fui::ListItem> items;
  items.reserve(entryCount);
  for (size_t i = 0; i < entryCount; ++i) {
    const auto& entry = entries[i];
    fui::ListItem item;
    item.label = entry.title.c_str();
    if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) item.subtitle = entry.author.c_str();
    if (entry.type == OpdsEntryType::NAVIGATION) item.value = ">";
    item.actionValue = static_cast<int16_t>(items.size());
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(entryCount));  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void OpdsBookBrowserActivity::buildDownloadScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, false);

  // Centered block: status line, book title, progress bar, cancel button.
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;
  const int16_t blockH = static_cast<int16_t>(lh * 2 + barH + btnH + gap * 3);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_DOWNLOADING), centered);
  screen.target().text(screen.takeTop(lh, gap), statusMessage.c_str(), centered);

  const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (downloadTotal > 0) {
    fui::ProgressBarProps progress;
    progress.value = static_cast<int32_t>(downloadProgress);
    progress.max = static_cast<int32_t>(downloadTotal);
    progress.border = fui::Paint::solid(fui::Color::Black);
    progress.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, progress);
  }

  const fui::Rect btnArea = screen.takeTop(btnH);
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
}

void OpdsBookBrowserActivity::buildStatusScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, false);

  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  if (state == BrowserState::ERROR) {
    const int16_t lh = screen.target().lineHeight(centered.font);
    const int16_t gap = screen.theme().spaceMd;
    const bool showTapHint = mappedInput.hasTouch();
    const int16_t blockH = static_cast<int16_t>(lh * (showTapHint ? 3 : 2) + gap * (showTapHint ? 2 : 1));
    const fui::Rect body = screen.body();
    if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
    screen.target().text(screen.takeTop(lh, gap), tr(STR_ERROR_MSG), centered);
    screen.target().text(screen.takeTop(lh, gap), errorMessage.c_str(), centered);
    if (showTapHint) screen.target().text(screen.takeTop(lh), tr(STR_TAP_TO_RETRY), centered);
    return;
  }
  // CHECK_WIFI / LOADING (and the brief child-activity handoff states).
  screen.centeredText(statusMessage.c_str(), centered);
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  MappedInputManager::Labels labels;
  switch (state) {
    case BrowserState::BROWSING: {
      const char* confirmLabel =
          (entryCount > 0 && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
      const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
      labels =
          mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
      break;
    }
    case BrowserState::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case BrowserState::ERROR:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  uiReady = false;
  app.render();
  uiReady = true;
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}

void OpdsBookBrowserActivity::showLoadingBeforeFetch() {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("OPDS", "Loading screen could not be rendered before feed fetch");
    requestUpdate(true);
  }
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (!ensureEntryBuffer()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

#ifdef SIMULATOR
  clearEntries();
  searchTemplate = "simulator://search?query={searchTerms}";

  if (path.empty()) {
    appendEntry(OpdsEntry{OpdsEntryType::NAVIGATION, "Browse fiction", "", "/fiction", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "The Left Hand of Darkness", "Ursula K. Le Guin",
                          "/books/the-left-hand-of-darkness.epub", ""});
    appendEntry(
        OpdsEntry{OpdsEntryType::BOOK, "A Room of One's Own", "Virginia Woolf", "/books/a-room-of-ones-own.epub", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "Frankenstein", "Mary Shelley", "/books/frankenstein.epub", ""});
  } else {
    appendEntry(
        OpdsEntry{OpdsEntryType::BOOK, "The Dispossessed", "Ursula K. Le Guin", "/books/the-dispossessed.epub", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "Kindred", "Octavia E. Butler", "/books/kindred.epub", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "The Time Machine", "H. G. Wells", "/books/the-time-machine.epub", ""});
  }

  selectorIndex = 0;
  topIndex = 0;
  state = BrowserState::BROWSING;
  requestUpdate();
  return;
#endif

  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  clearEntries();
  const std::string url = UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  OpdsParser parser(entries.get(), MAX_OPDS_FEED_ENTRIES);
  {
    OpdsParserStream stream{parser};
    HttpDownloader::DownloadOptions downloadOptions;
    downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
    const auto result = HttpDownloader::streamUrl(
        url, [&stream](const uint8_t* data, const size_t len) { return stream.write(data, len) == len; }, nullptr,
        server.username, server.password, std::move(downloadOptions));
    if (result != HttpDownloader::OK) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = parser.getErrorReason() == OpdsParserError::BUFFER_MEMORY ? tr(STR_OPDS_FEED_BUFFER_MEMORY_ERROR)
                                                                             : tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  entryCount = parser.getEntryCount();
  if (parser.wasTruncated()) {
    LOG_DBG("OPDS", "Feed truncated to %zu entries", entryCount);
  }

  if (!prevUrl.empty()) {
    for (size_t i = entryCount; i > 0; --i) {
      entries[i] = std::move(entries[i - 1]);
    }
    entries[0] = OpdsEntry{OpdsEntryType::NAVIGATION,
                           std::string(mappedInput.resolveLabel(mappedInput.withPreviousPageArrow(tr(STR_PREV_PAGE)))),
                           "", prevUrl, ""};
    entryCount++;
  }
  if (!nextUrl.empty() &&
      !appendEntry(OpdsEntry{OpdsEntryType::NAVIGATION,
                             std::string(mappedInput.resolveLabel(mappedInput.withNextPageArrow(tr(STR_NEXT_PAGE)))),
                             "", nextUrl, ""})) {
    LOG_DBG("OPDS", "No room for next-page entry");
  }

  selectorIndex = 0;
  topIndex = 0;
  state = entryCount == 0 ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entryCount == 0) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

bool OpdsBookBrowserActivity::ensureEntryBuffer() {
  if (entries) return true;
  entries = makeUniqueNoThrow<OpdsEntry[]>(OPDS_BROWSER_ENTRY_CAPACITY);
  return entries != nullptr;
}

void OpdsBookBrowserActivity::clearEntries() {
  // The app's interaction table still references the old row indices until
  // the next render, so stop routing touches while clearing the backing data.
  uiReady = false;
  for (size_t i = 0; entries && i < entryCount; ++i) {
    entries[i] = OpdsEntry{};
  }
  entryCount = 0;
}

bool OpdsBookBrowserActivity::appendEntry(OpdsEntry&& entry) {
  if (!entries || entryCount >= OPDS_BROWSER_ENTRY_CAPACITY) return false;
  entries[entryCount++] = std::move(entry);
  return true;
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  clearEntries();
  selectorIndex = 0;
  showLoadingBeforeFetch();
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    clearEntries();
    selectorIndex = 0;
    showLoadingBeforeFetch();
    fetchFeed(currentPath);
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  cancelDownload = false;
  goHomeAfterCancel = false;
  requestUpdate(true);

#ifdef SIMULATOR
  downloadProgress = 1;
  downloadTotal = 2;
  requestUpdate(true);
  return;
#endif

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  const char* downloadFolder = SETTINGS.opdsDownloadFolder;
  bool useDownloadFolder = downloadFolder[0] != '\0';
  if (useDownloadFolder && !Storage.exists(downloadFolder) && !Storage.mkdir(downloadFolder)) {
    LOG_ERR("OPDS", "Could not create download folder %s", downloadFolder);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  std::string filename;
  filename.reserve(96);
  if (useDownloadFolder) filename += downloadFolder;
  filename += '/';
  filename += StringUtils::sanitizeFilename(buildBookFilenameBase(book, server.filenameFormat));
  filename += ".epub";
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  bool cancelRequested = false;
  auto pollCancel = [this, &cancelRequested] {
    if (cancelRequested || cancelDownload) {
      cancelRequested = true;
      return true;
    }
    mappedInput.update();
    if (mappedInput.wasHomeGesture()) {
      goHomeAfterCancel = true;
      cancelRequested = true;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelRequested = true;
    }
    return cancelRequested;
  };
  HttpDownloader::DownloadOptions downloadOptions;
  downloadOptions.shouldCancel = pollCancel;
  downloadOptions.bufferSize = OPDS_DOWNLOAD_BUFFER_SIZE;
  downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &cancelRequested, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded,
                                                                            const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        // The activity loop is blocked for the whole download; pump input here
        // so the Cancel button or a Back press can abort mid-transfer.
        mappedInput.update();
        if (mappedInput.wasHomeGesture()) {
          goHomeAfterCancel = true;
          cancelRequested = true;
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelRequested = true;
        if (uiReady) {
          const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
          if (snap.touchPressed || snap.touchReleased) app.route(snap);
        }
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancelRequested, server.username, server.password, downloadOptions);

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    state = BrowserState::BROWSING;
  } else if (result == HttpDownloader::ABORTED) {
    LOG_INF("OPDS", "Download cancelled");
    if (goHomeAfterCancel) {
      onGoHome();
      return;
    }
    mappedInput.suppressNextBackRelease();
    state = BrowserState::BROWSING;
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back(currentPath);
  currentPath = url;

  clearEntries();
  selectorIndex = 0;
  showLoadingBeforeFetch();
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    showLoadingBeforeFetch();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    showLoadingBeforeFetch();
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
