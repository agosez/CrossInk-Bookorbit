#include "BookOrbitHomeStops.h"

#include <Logging.h>

#include <algorithm>

#include "components/themes/BaseTheme.h"

void BookOrbitHomeStops::reset(const bool hasLibrary, const bool hasStats) {
  availablePages = 0;
  pages[availablePages++] = Page::Home;
  if (hasLibrary) pages[availablePages++] = Page::Library;
  if (hasStats) pages[availablePages++] = Page::Stats;

  stopCount = 0;
  currentPage = std::min(currentPage, availablePages - 1);
  selection = 0;
}

void BookOrbitHomeStops::startPage(const Page page) {
  pageFirstStop[static_cast<size_t>(page)] = static_cast<uint8_t>(stopCount);
}

void BookOrbitHomeStops::add(const Rect& rect, const int radius, const int ringOutset, const Action action,
                             const int arg) {
  if (stopCount >= MAX_STOPS) {
    LOG_ERR("HOME", "BookOrbit Home is out of stops (%d)", MAX_STOPS);
    return;
  }
  Stop& stop = stops[stopCount++];
  stop.x = static_cast<int16_t>(rect.x);
  stop.y = static_cast<int16_t>(rect.y);
  stop.width = static_cast<int16_t>(rect.width);
  stop.height = static_cast<int16_t>(rect.height);
  stop.radius = static_cast<uint8_t>(radius);
  stop.ringOutset = static_cast<uint8_t>(ringOutset);
  stop.action = action;
  stop.arg = static_cast<uint8_t>(arg);
}

BookOrbitHomeStops::Page BookOrbitHomeStops::pageAt(const int index) const {
  return pages[static_cast<size_t>(std::clamp(index, 0, availablePages - 1))];
}

int BookOrbitHomeStops::pageFirstStopIndex(const int pageIndex) const {
  return pageFirstStop[static_cast<size_t>(pageAt(pageIndex))];
}

int BookOrbitHomeStops::pageStopCount(const int pageIndex) const {
  const size_t id = static_cast<size_t>(pageAt(pageIndex));
  return pageFirstStop[id + 1] - pageFirstStop[id];
}

const BookOrbitHomeStops::Stop* BookOrbitHomeStops::stopAt(const int index) const {
  if (index < 0 || index >= stopCount) return nullptr;
  return &stops[static_cast<size_t>(index)];
}

int BookOrbitHomeStops::selectedStopIndex(const int pageIndex) const {
  return selection < pageStopCount(pageIndex) ? pageFirstStopIndex(pageIndex) + selection : -1;
}

const BookOrbitHomeStops::Stop* BookOrbitHomeStops::selectedStop() const {
  return stopAt(selectedStopIndex(currentPage));
}

void BookOrbitHomeStops::moveSelection(const int delta) {
  // Wraps around the page: changing page is movePage()'s job.
  const int count = pageStopCount(currentPage);
  if (count <= 0) return;
  selection = ((selection + delta) % count + count) % count;
}

void BookOrbitHomeStops::movePage(const int delta) {
  selection = 0;
  currentPage = (currentPage + delta + availablePages) % availablePages;
}
