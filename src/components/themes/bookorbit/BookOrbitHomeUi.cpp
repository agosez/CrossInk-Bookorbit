#include "BookOrbitHomeUi.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>

#include "BookOrbitHomePages.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/bookOrbitHomeIcons.h"
#include "fontIds.h"

namespace {
using BookOrbitHomeLayout::kMargin;

constexpr int kRingWidth = 3;
constexpr int kTopBarHeight = 22;
}  // namespace

BookOrbitHomeUi::BookOrbitHomeUi(GfxRenderer& renderer, const MappedInputManager& mappedInput)
    : renderer(renderer), mappedInput(mappedInput) {}

void BookOrbitHomeUi::begin(const Source& source) {
  showRing = !mappedInput.hasTouch();
  // SETTINGS is read here and in render(), never in the model: GCC inlines a store's
  // getInstance(), static constructor and all, into a file that calls it only once,
  // which costs the C3 about 500 bytes of flash per such file.
  model.load(source, SETTINGS.shouldTrackReadingStats());
  layout = BookOrbitHomeLayout::grid(renderer);

  stops.reset(model.snapshot != nullptr, model.showStats);
  const BookOrbitHomePageContext context = pageContext();
  stops.startPage(Page::Home);
  BookOrbitHomeHomePage::addStops(context, stops);
  stops.startPage(Page::Library);
  BookOrbitHomeLibraryPage::addStops(context, stops);
  stops.startPage(Page::Stats);
  BookOrbitHomeStatsPage::addStops(context, stops);
  stops.startPage(Page::Count);

  repaintRequested = true;
}

BookOrbitHomePageContext BookOrbitHomeUi::pageContext() const {
  return BookOrbitHomePageContext{renderer,          BookOrbitHomeTiles(renderer), model, layout, showRing,
                                  stops.pageCount(), stops.currentPageIndex()};
}

Rect BookOrbitHomeUi::coverSlot(const int bookIndex) const {
  return BookOrbitHomeLayout::coverSlot(layout, model.showStats, bookIndex);
}

std::string BookOrbitHomeUi::coverPath(const RecentBook& book, const int width, const int height) {
  return BookOrbitHomeTiles::coverPath(book, width, height);
}

uint32_t BookOrbitHomeUi::framebufferHash() const {
  // FNV-1a over 32-bit words: well under a millisecond for 48 KB. It lets render() trust
  // the framebuffer without tracking everything else that can draw into it (popups,
  // child activities, the frontlight panel).
  const uint8_t* buffer = renderer.getFrameBuffer();
  if (buffer == nullptr) return 0;
  const size_t size = renderer.getBufferSize();
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i + sizeof(uint32_t) <= size; i += sizeof(uint32_t)) {
    uint32_t word;
    memcpy(&word, buffer + i, sizeof(word));
    hash = (hash ^ word) * 16777619u;
  }
  return hash;
}

void BookOrbitHomeUi::render(const HalDisplay::RefreshMode refreshMode) {
  const int pageIndexNow = std::clamp(stops.currentPageIndex(), 0, stops.pageCount() - 1);
  const Page page = stops.pageAt(pageIndexNow);
  const int stop = showRing ? stops.selectedStopIndex(pageIndexNow) : -1;

  const bool samePage = painted && !repaintRequested && paintedPage == pageIndexNow;
  if (samePage && refreshMode == HalDisplay::FAST_REFRESH && framebufferHash() == paintedHash) {
    registerTouchTargets();
    if (stop == paintedStop) return;
    drawRing(paintedStop, false);
    drawRing(stop, true);
    drawTopBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    HalDisplay::RefreshMode mode = refreshMode;
    if (painted && paintedPage != pageIndexNow && mode == HalDisplay::FAST_REFRESH) {
      // Page turns repaint a large share of the panel, so they pay the user's refresh
      // cadence like the reader's page turns do.
      if (++pageTurnsSinceHalfRefresh >= SETTINGS.getRefreshFrequency()) {
        pageTurnsSinceHalfRefresh = 0;
        mode = HalDisplay::HALF_REFRESH;
      }
    }
    paintPage(page);
    drawRing(stop, true);
    registerTouchTargets();
    renderer.displayBuffer(mode);
    // The big numbers come from a compressed reader font, whose decompressed glyph group
    // takes about 10 KB. Home has no use for it until the next paint.
    if (auto* fontCache = renderer.getFontCacheManager()) fontCache->clearCache();
  }

  painted = true;
  repaintRequested = false;
  paintedPage = pageIndexNow;
  paintedStop = stop;
  paintedHash = framebufferHash();
}

void BookOrbitHomeUi::registerTouchTargets() const {
  const int currentPage = stops.currentPageIndex();
  const int first = stops.pageFirstStopIndex(currentPage);
  const int count = stops.pageStopCount(currentPage);
  for (int i = first; i < first + count; i++) {
    const Stop& stop = *stops.stopAt(i);
    TouchRegistry::getInstance().add(Rect{stop.x, stop.y, stop.width, stop.height}, i, TouchRegistry::Item);
  }
  if (showRing) return;
  const int footerY = renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - 40;
  const int footerWidth = renderer.getScreenWidth() / 3;
  TouchRegistry::getInstance().add(Rect{0, footerY, footerWidth, 40}, TAP_PREVIOUS_PAGE, TouchRegistry::Item);
  TouchRegistry::getInstance().add(Rect{renderer.getScreenWidth() - footerWidth, footerY, footerWidth, 40},
                                   TAP_NEXT_PAGE, TouchRegistry::Item);
}

void BookOrbitHomeUi::drawRing(const int stopIndex, const bool state) const {
  const Stop* stop = stops.stopAt(stopIndex);
  if (stop == nullptr || !showRing) return;
  const int outset = stop->ringOutset;
  renderer.drawRoundedRect(stop->x - outset, stop->y - outset, stop->width + 2 * outset, stop->height + 2 * outset,
                           kRingWidth, stop->radius + outset, state);
}

void BookOrbitHomeUi::drawTopBar() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(
      renderer,
      Rect{0, metrics.topPadding, renderer.getScreenWidth(), kTopBarHeight + UITheme::getTopStatusBarInset(renderer)},
      nullptr);
}

void BookOrbitHomeUi::drawNavigation(const Page page) const {
  if (showRing) {
    const auto labels =
        mappedInput.mapLabels(tr(STR_MENU), tr(STR_OPEN), tr(STR_HOME_PAGE_PREVIOUS), tr(STR_HOME_PAGE_NEXT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const int currentPage = stops.currentPageIndex();
  const int y = renderer.getScreenHeight() - 40;
  if (currentPage > 0) drawLucideIcon(renderer, icon_home_prev_24, kMargin + 2, y + 8);
  if (currentPage < stops.pageCount() - 1) {
    drawLucideIcon(renderer, icon_home_next_24, renderer.getScreenWidth() - kMargin - 26, y + 8);
  }
  const char* name =
      page == Page::Stats ? tr(STR_HOME_STATS) : (page == Page::Library ? tr(STR_LIBRARY) : tr(STR_HOME));
  const int width = renderer.getTextWidth(UI_10_FONT_ID, name, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, (renderer.getScreenWidth() - width) / 2, y + 7, name, true, EpdFontFamily::BOLD);
}

void BookOrbitHomeUi::paintPage(const Page page) {
  renderer.clearScreen();
  drawTopBar();
  const BookOrbitHomePageContext context = pageContext();
  switch (page) {
    case Page::Home:
      BookOrbitHomeHomePage::draw(context);
      break;
    case Page::Library:
      BookOrbitHomeLibraryPage::draw(context);
      break;
    case Page::Stats:
      BookOrbitHomeStatsPage::draw(context);
      break;
    case Page::Count:
      break;
  }
  drawNavigation(page);
}
