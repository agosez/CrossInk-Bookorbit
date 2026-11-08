#pragma once

#include <HalDisplay.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BookOrbitHomeStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/BookOrbitHomeLayout.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;
class MappedInputManager;
namespace freeink {
struct Icon;
}

// The BookOrbit theme's Home: pages of tiles over the firmware's own status bar.
//
// Everything on screen comes from the SD card, so opening Home never waits on the
// network. The stop table is computed once in begin() and stays immutable while the
// render task reads it, which lets loop() move the selection without taking the render
// lock. Moving the selection only touches the ring in the white gutter around a tile
// (see render()): no redraw, no SD read and no extra buffer.
class BookOrbitHomeUi final {
 public:
  enum class Action : uint8_t {
    None,
    OpenBook,         // arg: index into the recent books
    OpenCatalogBook,  // arg: see catalogBook()
    OpenLibrary,
    OpenStats,
    OpenSavedItems,
    OpenCatalog,
    OpenAuthors,
    OpenSeries,
    Refresh,
  };
  enum class Page : uint8_t { Home, Library, Stats, Count };

  static constexpr int MAX_BOOKS = BookOrbitHomeLayout::kMaxBooks;
  static constexpr int TAP_PREVIOUS_PAGE = 100;
  static constexpr int TAP_NEXT_PAGE = 101;

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

  struct Source {
    const std::vector<RecentBook>* books = nullptr;
    const GlobalReadingStats* stats = nullptr;
    const BookReadingStats* currentBookStats = nullptr;
    float currentBookProgress = -1.0f;
    const std::string* chapterTitle = nullptr;
    size_t recentBookCount = 0;
    // This device's own count, whichever stats are shown: the snapshot's baseline is one.
    uint32_t localFinishedBooks = 0;
  };

  BookOrbitHomeUi(GfxRenderer& renderer, const MappedInputManager& mappedInput);

  // Reads what the pages show and lays them out. Call again when that data changed;
  // the next render() repaints the page.
  void begin(const Source& source);

  // Paints the current page, or only moves the ring when that is all that changed.
  void render(HalDisplay::RefreshMode refreshMode);

  // Moves through the current page's tiles, wrapping at either end.
  void moveSelection(int delta);
  void movePage(int delta);
  // Null when the page has no stop to activate.
  const Stop* selectedStop() const;
  const Stop* stopAt(int index) const;
  // The server book behind an OpenCatalogBook stop.
  const BookOrbitHomeBook* catalogBook(int arg) const;

  // The exact slot a book's cover is drawn in. Rescaling a dithered 1-bit image aliases
  // badly, so thumbnails are generated at this size.
  Rect coverSlot(int bookIndex) const;
  // Empty when the book has no cover file to draw.
  static std::string coverPath(const RecentBook& book, int width, int height);

 private:
  Rect cell(int column, int row, int columns = 1, int rows = 1) const;
  Rect statsCell(int column, int row, int columns = 1) const;
  Rect bookRowRect(const Rect& tile, int rowIndex) const;
  Rect alsoReadingTile() const;
  Rect currentBookTile() const;
  void computeLayout();
  void computeReadingData();
  void computeRows();
  Rect shelfCoverRect(const Rect& tile, int index) const;
  void addStop(const Rect& rect, int radius, int ringOutset, Action action, int arg = 0);
  void markPageStops(Page page);
  Page pageAt(int index) const;
  int pageStopCount(int pageIndex) const;
  int pageFirstStopIndex(int pageIndex) const;

  void paintPage(Page page);
  void drawTopBar() const;
  void drawTitleRow(const char* title) const;
  void drawNavigation(Page page) const;
  void drawRing(int stopIndex, bool state) const;
  void registerTouchTargets() const;
  uint32_t framebufferHash() const;

  void drawHomePage();
  void drawCurrentBook(const Rect& tile) const;
  void drawStreak(const Rect& tile) const;
  void drawWeek(const Rect& tile) const;
  void drawGoal(const Rect& tile) const;
  void drawAccountTile(const Rect& tile) const;
  void drawAlsoReading(const Rect& tile) const;
  void drawSyncAge(const Rect& tile) const;

  void drawStatsPage();
  void drawYear(const Rect& tile) const;
  void drawForecast(const Rect& tile) const;
  void drawTimeOfDay(const Rect& tile) const;
  void drawWeekdays(const Rect& tile) const;
  void drawRecords(const Rect& tile) const;
  void drawActivity(const Rect& tile) const;
  void drawBadge(const Rect& tile) const;
  void drawHighlight(const Rect& tile) const;

  void drawLibraryPage();
  void drawShelf(const Rect& tile, const char* title, const freeink::Icon& icon, const BookOrbitHomeBook* books,
                 int count, uint8_t onCardMask) const;
  void drawLink(const Rect& tile, const char* title, const freeink::Icon& icon, int32_t value, const char* unit,
                const char* detail) const;
  void drawMetrics(const Rect& tile, const char* const* values, const char* const* labels, int count) const;
  void drawMetricColumns(const Rect& tile, int valueY, int ruleTop, int ruleBottom, const char* const* values,
                         const char* const* labels, int count) const;
  void drawCardTile(const Rect& tile) const;

  void drawFitted(int fontId, int x, int y, const char* text, int maxWidth, bool bold = false, bool black = true) const;
  void drawTileFrame(const Rect& tile) const;
  void drawTileHeader(const Rect& tile, const char* title, const freeink::Icon* icon = nullptr) const;
  void drawTileFooter(const Rect& tile, const char* text) const;
  void drawRightAligned(int fontId, int right, int y, const char* text, bool bold = false) const;
  void drawDot(int left, int top, int radius, bool filled) const;
  void drawBigValue(const Rect& tile, const char* value, const char* unit) const;
  void drawBar(int x, int base, int width, int chartHeight, uint32_t value, uint32_t highest, bool emphasized) const;
  void drawProgressBar(int x, int y, int width, int height, float fraction, bool black = true) const;
  void drawColumnChart(const Rect& area, const uint32_t* values, int count, int emphasized, const char* initials) const;
  void drawCover(const RecentBook& book, const Rect& slot, bool onBlack) const;
  void drawCoverFile(const char* path, const Rect& slot, bool onBlack) const;

  GfxRenderer& renderer;
  const MappedInputManager& mappedInput;
  Source data;
  BookOrbitHomeLayout::Grid layout;
  bool showStats = false;
  bool hasAccount = false;
  bool hasToday = false;
  bool hasForecast = false;
  bool showRing = false;
  ReadingStatsDateTime today;

  int32_t goalCompleted = 0;
  uint16_t streak = 0;
  uint16_t longestStreak = 0;
  uint8_t lastSevenDays = 0;  // bit 0 is six days ago, bit 6 is today
  std::array<uint32_t, 7> weekMinutes{};
  uint32_t weekTotal = 0;
  std::array<float, MAX_BOOKS> bookProgress{};
  uint32_t estimatedSecondsLeft = 0;
  ReadingStatsDate finishDate;
  uint32_t dailyPaceMinutes = 0;
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> weekdayMinutes{};
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};

  // The rows under the current book: other recent books, then what the server says is
  // being read elsewhere. Exactly one of the two is set.
  struct Row {
    const RecentBook* local = nullptr;
    const BookOrbitHomeBook* remote = nullptr;
    int remoteArg = 0;
    float progress = -1.0f;
    bool onCard = false;
  };
  std::array<Row, MAX_BOOKS - 1> rows{};
  int rowCount = 0;

  // Null without an account or before the first sync. About 900 bytes for as long as
  // Home is open: on the heap so the other themes do not pay for it, and fallible because
  // Home has a full local page to fall back on.
  std::unique_ptr<BookOrbitHomeSnapshot> snapshot;
  uint8_t discoverOnCard = 0;  // bit per shelf entry
  uint8_t recentOnCard = 0;
  // Books the Library's index counted on its last scan; -1 before the first one.
  int32_t cardBookCount = -1;
  int32_t serverBookCount = 0;  // catalog downloads recorded in BookOrbitDownloadIndex
  uint64_t cardTotalBytes = 0;  // 0 when the card did not say, or is FAT32 (see readCardSpace)
  uint64_t cardUsedBytes = 0;

  // Two bits per day over the heatmap's window: unread, a little, some, a lot.
  static constexpr int YEAR_WEEKS = 52;
  std::array<uint8_t, (YEAR_WEEKS * 7 + 3) / 4> yearLevels{};
  uint32_t yearFirstDay = 0;
  uint16_t yearCellCount = 0;
  uint16_t yearDaysRead = 0;
  uint32_t yearMinutes = 0;
  uint32_t yearBestMonthMinutes = 0;

  // The most stops the fullest page set can hold.
  static constexpr int MAX_STOPS = 20;
  std::array<Stop, MAX_STOPS> stops{};
  std::array<uint8_t, static_cast<size_t>(Page::Count) + 1> pageFirstStop{};
  std::array<Page, static_cast<size_t>(Page::Count)> pages{};
  int stopCount = 0;
  int availablePages = 1;

  // Written by loop(), read by the render task, like HomeActivity::selectorIndex.
  int currentPage = 0;
  int selection = 0;

  // What the framebuffer holds, owned by the render task.
  bool painted = false;
  bool repaintRequested = true;
  int paintedPage = -1;
  int paintedStop = -1;
  uint32_t paintedHash = 0;
  int pageTurnsSinceHalfRefresh = 0;
};
