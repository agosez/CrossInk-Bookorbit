#include "EpubReaderBookmarkListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cmath>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

EpubReaderBookmarkListActivity::EpubReaderBookmarkListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                               const std::vector<Bookmark>& bookmarks)
    : Activity("EpubReaderBookmarkList", renderer, mappedInput),
      bookmarks(bookmarks),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void EpubReaderBookmarkListActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &EpubReaderBookmarkListActivity::onRowEvent, this);
  app.setScreen(&EpubReaderBookmarkListActivity::listScreen, this);
  requestUpdate();
}

void EpubReaderBookmarkListActivity::onExit() { Activity::onExit(); }

void EpubReaderBookmarkListActivity::deleteSelectedBookmark() {
  if (bookmarks.empty() || selectedIndex < 0 || selectedIndex >= static_cast<int>(bookmarks.size())) return;
  if (!BOOKMARKS.removeBookmarkAt(static_cast<size_t>(selectedIndex))) return;
  bookmarks = BOOKMARKS.getBookmarks();
  if (bookmarks.empty())
    selectedIndex = 0;
  else if (selectedIndex >= static_cast<int>(bookmarks.size()))
    selectedIndex = bookmarks.size() - 1;
  topIndex = followListSelection(selectedIndex, topIndex, visibleRows, static_cast<int>(bookmarks.size()));
  requestUpdate();
}

void EpubReaderBookmarkListActivity::showBookmarkDeletePopup() {
  if (bookmarks.empty() || selectedIndex < 0 || selectedIndex >= static_cast<int>(bookmarks.size())) return;
  confirmingDelete = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  confirmPopup.show(tr(STR_CONFIRM_DELETE_BOOKMARK), options, 2, 0, [this](const int optionIndex) {
    confirmingDelete = false;
    if (optionIndex == 1) deleteSelectedBookmark();
    requestUpdate();
  });
  confirmPopup.setPrimaryOptionIndex(1);
  requestUpdate();
}

void EpubReaderBookmarkListActivity::selectBookmark() {
  if (bookmarks.empty() || selectedIndex < 0 || selectedIndex >= static_cast<int>(bookmarks.size())) return;
  setResult(BookmarkResult{bookmarks[selectedIndex].spineIndex, bookmarks[selectedIndex].progress,
                           bookmarks[selectedIndex].paragraphIndex});
  finish();
}

void EpubReaderBookmarkListActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderBookmarkListActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->bookmarks.size())) return;
  self->selectedIndex = event.value;
  if (event.longPress) {
    self->app.clearTapFlash();
    self->showBookmarkDeletePopup();
    return;
  }
  self->app.clearTapFlash();
  self->selectBookmark();
}

void EpubReaderBookmarkListActivity::loop() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (confirmingDelete) {
    confirmingDelete = false;
    requestUpdate();
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (TouchHeaderBackButton::wasTapped(mappedInput, header) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (!bookmarks.empty() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= ReaderUtils::DELETE_HOLD_MS) {
    showBookmarkDeletePopup();
    return;
  }
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    selectBookmark();
    return;
  }
  const int total = static_cast<int>(bookmarks.size());
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int next = scrollListBy(topIndex, swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows,
                                  visibleRows, total);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }
  const auto move = [this, total](const int next) {
    selectedIndex = next;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, total);
    requestUpdate();
  };
  buttonNavigator.onNextRelease([this, total, &move] { move(ButtonNavigator::nextIndex(selectedIndex, total)); });
  buttonNavigator.onPreviousRelease(
      [this, total, &move] { move(ButtonNavigator::previousIndex(selectedIndex, total)); });
  buttonNavigator.onNextContinuous(
      [this, total, &move] { move(ButtonNavigator::nextPageIndex(selectedIndex, total, visibleRows)); });
  buttonNavigator.onPreviousContinuous(
      [this, total, &move] { move(ButtonNavigator::previousPageIndex(selectedIndex, total, visibleRows)); });
}

void EpubReaderBookmarkListActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderBookmarkListActivity*>(user)->buildListScreen(screen);
}

void EpubReaderBookmarkListActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)),
      static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (bookmarks.empty()) {
    screen.centeredText(tr(STR_NO_BOOKMARKS), screen.theme().bodyText);
    return;
  }
  std::vector<std::string> values(bookmarks.size());
  std::vector<fui::ListItem> items;
  items.reserve(bookmarks.size());
  for (size_t i = 0; i < bookmarks.size(); ++i) {
    const Bookmark& bookmark = bookmarks[i];
    values[i] = std::to_string(static_cast<int>(std::lround(bookmark.progress * 100.0))) + "%";
    fui::ListItem item;
    item.label = bookmark.snippet[0] != '\0'
                     ? bookmark.snippet
                     : (bookmark.chapterTitle[0] != '\0' ? bookmark.chapterTitle : tr(STR_UNKNOWN_CHAPTER));
    if (bookmark.snippet[0] != '\0') {
      item.subtitle = bookmark.chapterTitle[0] != '\0' ? bookmark.chapterTitle : tr(STR_UNKNOWN_CHAPTER);
    }
    item.value = values[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);
  props.valueInset = 8;
  const fui::Rect bounds = screen.body();
  const auto rows = configureUiList(props, screen.theme(), bounds, UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(bookmarks.size()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void EpubReaderBookmarkListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_BOOKMARKS), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BOOKMARKS), nullptr, true);
  }
  uiReady = false;
  app.render();
  uiReady = true;
  if (confirmPopup.processRender(renderer, mappedInput)) return;
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)),
                                            bookmarks.empty() ? "" : tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
