#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

struct Rect;

// The BookOrbit Home's pages and the stops on them: what the buttons walk through and a
// tap lands on. The table is built in begin() and stays immutable while the render task
// reads it, which lets loop() move the selection without taking the render lock.
class BookOrbitHomeStops {
 public:
  enum class Action : uint8_t {
    None,
    OpenBook,         // arg: index into the recent books
    OpenCatalogBook,  // arg: see BookOrbitHomeModel::catalogBook()
    OpenLibrary,
    OpenStats,
    OpenSavedItems,
    OpenCatalog,
    OpenAuthors,
    OpenSeries,
    OpenContinueReading,  // the catalog's books in progress
    Refresh,
  };
  enum class Page : uint8_t { Home, Library, Stats, Count };

  struct Stop {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
    uint8_t radius = 0;
    uint8_t ringOutset = 0;  // 0 keeps the ring inside the rect, for rows inside a tile
    Action action = Action::None;
    uint8_t arg = 0;
  };

  // Empties the table and lists the pages shown, Home always first. The current page
  // stays where it was when it still exists, on its first stop.
  void reset(bool hasLibrary, bool hasStats);
  // The stops added from here on belong to page. Every page is started in order, shown
  // or not, then Page::Count closes the table.
  void startPage(Page page);
  void add(const Rect& rect, int radius, int ringOutset, Action action, int arg = 0);

  int pageCount() const { return availablePages; }
  Page pageAt(int index) const;
  int pageFirstStopIndex(int pageIndex) const;
  int pageStopCount(int pageIndex) const;
  const Stop* stopAt(int index) const;

  int currentPageIndex() const { return currentPage; }
  // The table index of the selected stop on that page; -1 when the page has none there.
  int selectedStopIndex(int pageIndex) const;
  // Null when the page has no stop to activate.
  const Stop* selectedStop() const;
  // Moves through the current page's stops, wrapping at either end.
  void moveSelection(int delta);
  void movePage(int delta);

 private:
  // The most stops the fullest page set can hold: Home 6, Library 12, Stats 3.
  static constexpr int MAX_STOPS = 21;

  std::array<Stop, MAX_STOPS> stops{};
  std::array<uint8_t, static_cast<size_t>(Page::Count) + 1> pageFirstStop{};
  std::array<Page, static_cast<size_t>(Page::Count)> pages{};
  int stopCount = 0;
  int availablePages = 1;

  // Written by loop(), read by the render task, like HomeActivity::selectorIndex.
  int currentPage = 0;
  int selection = 0;
};
