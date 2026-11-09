#include "BookOrbitHomeUi.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitDownloadIndex.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/reader/ReadingDailyMinutes.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/afternoon.h"
#include "components/icons/bookOrbitHomeIcons.h"
#include "components/icons/evening.h"
#include "components/icons/listIcons.h"
#include "components/icons/morning.h"
#include "components/icons/night.h"
#include "components/icons/streak.h"
#include "fontIds.h"
#include "util/CardSpace.h"

namespace {
using BookOrbitHomeLayout::kBookRowCoverInset;
using BookOrbitHomeLayout::kBookRowHeaderHeight;
using BookOrbitHomeLayout::kGap;
using BookOrbitHomeLayout::kMargin;
using BookOrbitHomeLayout::kPad;
using BookOrbitHomeLayout::kRingOutset;
using BookOrbitHomeLayout::kRowCount;
using BookOrbitHomeLayout::kShelfCaptionHeight;
using BookOrbitHomeLayout::kShelfCoverTop;

constexpr int kTileRadius = 10;
constexpr int kBookRadius = 12;
constexpr int kBookPad = 13;
constexpr int kRingWidth = 3;
constexpr int kTopBarHeight = 22;
constexpr int kTitleRowTop = 27;
constexpr int kStatsYearTileHeight = 172;
constexpr int kIconSize = 24;
constexpr int kYearCellPitch = 8;
constexpr int kYearCellSize = 7;
constexpr uint16_t kYearLevelSomeMinutes = 20;
constexpr uint16_t kYearLevelLotMinutes = 60;
constexpr uint32_t kForecastPaceDays = 14;
constexpr uint32_t kForecastMinPaceMinutes = 5;
constexpr int kStatsRows = 4;
// An OpenCatalogBook stop's arg: the snapshot's lists laid end to end.
constexpr int kDiscoverArgBase = BookOrbitHomeSnapshot::CONTINUE_COUNT;
constexpr int kRecentArgBase = kDiscoverArgBase + BookOrbitHomeSnapshot::SHELF_COUNT;

// The big numbers. Boards with scalable reader fonts do not carry the built-in Lexend.
#if CROSSINK_SCALABLE_FONTS
constexpr int kBigFontId = UI_12_FONT_ID;
#else
constexpr int kBigFontId = LEXENDDECA_16_FONT_ID;
#endif
constexpr int kBigValueTop = 28;
constexpr int kBigValueBaseline = 34;  // from kBigValueTop

// Copies the index-th character of a translated initials string: "LMMJVSD" holds one
// character per weekday, and some languages need more than one byte for them.
void copyInitial(const char* initials, const int index, char* out, const size_t outSize) {
  const auto* cursor = reinterpret_cast<const unsigned char*>(initials);
  for (int i = 0; i < index; i++) utf8NextCodepoint(&cursor);
  const auto* start = cursor;
  utf8NextCodepoint(&cursor);
  const size_t length = std::min(static_cast<size_t>(cursor - start), outSize - 1);
  memcpy(out, start, length);
  out[length] = '\0';
}

// Copies the index-th comma-separated token of a translated list.
void copyToken(const char* list, int index, char* out, const size_t outSize) {
  while (index > 0 && *list != '\0') {
    if (*list++ == ',') index--;
  }
  size_t length = 0;
  while (list[length] != '\0' && list[length] != ',' && length + 1 < outSize) {
    out[length] = list[length];
    length++;
  }
  out[length] = '\0';
}

void formatHours(const uint32_t hours, char* out, const size_t outSize) {
  snprintf(out, outSize, "%luh", static_cast<unsigned long>(hours));
}

void formatPercent(const float percent, char* out, const size_t outSize) {
  snprintf(out, outSize, "%d%%", static_cast<int>(percent + 0.5f));
}

const char* displayTitle(const RecentBook& book) { return book.title.empty() ? book.path.c_str() : book.title.c_str(); }

uint8_t onCardMask(const BookOrbitHomeBook* books, const int count) {
  std::string path;
  uint8_t mask = 0;
  for (int i = 0; i < count; i++) {
    if (BookOrbitDownloadIndex::lookup(books[i].id, path)) mask |= static_cast<uint8_t>(1u << i);
  }
  return mask;
}

// Decimal gigabytes, as card capacities are sold: one decimal under 10 GB.
void formatGigabytes(const uint64_t bytes, char* out, const size_t outSize) {
  const auto tenths = static_cast<unsigned long>((bytes + 50000000ull) / 100000000ull);
  if (tenths >= 100) {
    snprintf(out, outSize, "%lu", (tenths + 5) / 10);
  } else {
    snprintf(out, outSize, "%lu.%lu", tenths / 10, tenths % 10);
  }
}

// Counting the used space froze Home at every start on slow FAT32 cards (#110), so only
// exFAT cards get a storage bar (see CardSpace). The card is asked once per boot; every
// download and file transfer ends in a restart, which renews it.
void readCardSpace(uint64_t& total, uint64_t& used) {
  static bool read = false;
  static uint64_t cachedTotal = 0;
  static uint64_t cachedUsed = 0;
  if (!read) {
    read = true;
    const unsigned long start = millis();
#ifndef SIMULATOR
    // The host storage shim does not expose card capacity.
    if (CardSpace::isQuickToCount()) {
      cachedTotal = Storage.totalBytes();
      cachedUsed = Storage.usedBytes();
    }
#endif
    LOG_DBG("HOME", "Card space read in %lu ms (used=%llu total=%llu)", millis() - start,
            static_cast<unsigned long long>(cachedUsed), static_cast<unsigned long long>(cachedTotal));
  }
  total = cachedTotal;
  used = cachedUsed;
}

const char* greeting(const ReadingStatsDateTime& now) {
  if (now.hour >= 5 && now.hour < 12) return tr(STR_HOME_GOOD_MORNING);
  if (now.hour >= 12 && now.hour < 18) return tr(STR_HOME_GOOD_AFTERNOON);
  return tr(STR_HOME_GOOD_EVENING);
}
}  // namespace

BookOrbitHomeUi::BookOrbitHomeUi(GfxRenderer& renderer, const MappedInputManager& mappedInput)
    : renderer(renderer), mappedInput(mappedInput) {}

void BookOrbitHomeUi::begin(const Source& source) {
  data = source;
  showStats = SETTINGS.shouldTrackReadingStats();
  hasAccount = BOOKORBIT_STORE.hasCredentials();
  showRing = !mappedInput.hasTouch();
  hasToday = getCurrentLocalReadingStatsDateTimeAnyClock(today);

  snapshot.reset();
  if (hasAccount) {
    snapshot = makeUniqueNoThrow<BookOrbitHomeSnapshot>();
    if (snapshot && !BookOrbitHomeStore::load(*snapshot)) snapshot.reset();
  }

  // Only the index header is read: the count as of the Library's last scan, no card walk.
  cardBookCount = -1;
  if (snapshot && Storage.exists(library::libraryIndexPath())) {
    library::LibraryIndexFile index;
    if (index.open(library::libraryIndexPath())) cardBookCount = index.bookCount();
  }
  if (snapshot) readCardSpace(cardTotalBytes, cardUsedBytes);

  bookProgress.fill(-1.0f);
  if (data.books != nullptr) {
    const size_t count = std::min<size_t>(data.books->size(), MAX_BOOKS);
    for (size_t i = 0; i < count; i++) {
      const RecentBook& book = (*data.books)[i];
      if (i == 0) {
        bookProgress[i] = data.currentBookProgress;
      } else {
        bookProgress[i] = FsHelpers::hasEpubExtension(book.path) ? RecentBookProgress::loadCachedEpubPercent(book)
                                                                 : RecentBookProgress::loadPercent(book);
      }
    }
  }
  computeRows();

  computeReadingData();
  computeLayout();

  currentPage = std::min(currentPage, availablePages - 1);
  selection = 0;
  repaintRequested = true;
}

void BookOrbitHomeUi::computeRows() {
  rowCount = 0;
  rows.fill(Row{});
  const int localBooks = data.books != nullptr ? static_cast<int>(data.books->size()) : 0;
  for (int i = 1; i < localBooks && rowCount < static_cast<int>(rows.size()); i++) {
    Row& row = rows[static_cast<size_t>(rowCount++)];
    row.local = &(*data.books)[static_cast<size_t>(i)];
    row.progress = bookProgress[static_cast<size_t>(i)];
    row.onCard = true;
  }

  discoverOnCard = 0;
  recentOnCard = 0;
  if (!snapshot) return;

  const auto isRecentBook = [this, localBooks](const char* title, const std::string& path) {
    for (int i = 0; i < localBooks; i++) {
      const RecentBook& book = (*data.books)[static_cast<size_t>(i)];
      if (book.title == title || (!path.empty() && book.path == path)) return true;
    }
    return false;
  };
  std::string path;
  for (int i = 0; i < snapshot->continueCount && rowCount < static_cast<int>(rows.size()); i++) {
    const BookOrbitHomeBook& book = snapshot->continueReading[i];
    const bool onCard = BookOrbitDownloadIndex::lookup(book.id, path);
    if (isRecentBook(book.title, onCard ? path : std::string{})) continue;
    Row& row = rows[static_cast<size_t>(rowCount++)];
    row.remote = &book;
    row.remoteArg = i;
    row.progress = book.progress != BookOrbitHomeBook::NO_PROGRESS ? static_cast<float>(book.progress) : -1.0f;
    row.onCard = onCard;
  }
  discoverOnCard = onCardMask(snapshot->discover, snapshot->discoverCount);
  recentOnCard = onCardMask(snapshot->recent, snapshot->recentCount);
  serverBookCount = static_cast<int32_t>(BookOrbitDownloadIndex::recordedCount());
  BookOrbitDownloadIndex::unload();
}

const BookOrbitHomeBook* BookOrbitHomeUi::catalogBook(const int arg) const {
  if (!snapshot || arg < 0) return nullptr;
  if (arg < kDiscoverArgBase) return &snapshot->continueReading[arg];
  if (arg < kRecentArgBase) return &snapshot->discover[arg - kDiscoverArgBase];
  if (arg < kRecentArgBase + BookOrbitHomeSnapshot::SHELF_COUNT) return &snapshot->recent[arg - kRecentArgBase];
  return nullptr;
}

void BookOrbitHomeUi::computeReadingData() {
  streak = 0;
  lastSevenDays = 0;
  weekMinutes.fill(0);
  weekTotal = 0;
  yearLevels.fill(0);
  yearCellCount = 0;
  yearDaysRead = 0;
  yearMinutes = 0;
  yearBestMonthMinutes = 0;
  hasForecast = false;
  longestStreak = data.stats != nullptr ? data.stats->displayLongestReadingStreak() : 0;
  weekdayMinutes.fill(0);
  timeOfDaySeconds.fill(0);
  if (data.stats != nullptr) timeOfDaySeconds = data.stats->timeOfDaySeconds;

  goalCompleted = 0;
  if (snapshot) {
    goalCompleted = snapshot->goalCompleted;
    // Books finished here since the fetch count until the next fetch brings the server's.
    if (hasToday && today.date.year == snapshot->goalYear && data.localFinishedBooks > snapshot->localFinishedAtFetch) {
      goalCompleted += static_cast<int32_t>(data.localFinishedBooks - snapshot->localFinishedAtFetch);
    }
  }

  if (!showStats || data.stats == nullptr || !hasToday) return;
  const GlobalReadingStats& stats = *data.stats;

  // The window starts on a Monday, so the columns are calendar weeks and the last one
  // holds today.
  const uint32_t todayIndex = readingStatsDayIndex(today.date);
  const uint32_t daysIntoWeek = readingStatsDayOfWeekIndex(today.date);
  yearFirstDay = todayIndex - daysIntoWeek - (YEAR_WEEKS - 1) * 7u;
  yearCellCount = static_cast<uint16_t>(todayIndex - yearFirstDay + 1);

  ReadingDailyMinutes::Reader daily;
  BookOrbitHomeStore::YearReader server;
  // Boards with a clock chip also fill these from before the daily file existed.
  for (size_t i = 0; i < timeOfDaySeconds.size(); i++) {
    timeOfDaySeconds[i] = std::max(timeOfDaySeconds[i], daily.timeOfDaySeconds()[i]);
  }

  uint32_t monthKey = 0;
  uint32_t monthMinutes = 0;
  bool monthIsWhole = false;  // the window's first month is cut short: it cannot be the best one
  uint32_t recentMinutes = 0;
  uint16_t run = 0;
  for (uint16_t cell = 0; cell < yearCellCount; cell++) {
    const uint32_t day = yearFirstDay + cell;
    // The server's minutes hold every device's reading up to the fetch; this device adds
    // what the snapshot cannot hold yet. Without a snapshot, local reading is all there is.
    const uint16_t minutes =
        snapshot ? static_cast<uint16_t>(std::min<uint32_t>(
                       UINT16_MAX, static_cast<uint32_t>(server.minutes(day)) + daily.pendingMinutes(day)))
                 : daily.minutes(day);
    const bool read = minutes > 0 || readingHistoryHasDay(stats.readingHistoryAnchorDay, stats.readingHistoryBits, day);

    // Days only known from the history bits were read for an unknown time.
    uint8_t level = 0;
    if (minutes >= kYearLevelLotMinutes) {
      level = 3;
    } else if (minutes >= kYearLevelSomeMinutes) {
      level = 2;
    } else if (read) {
      level = 1;
    }
    yearLevels[cell / 4] |= static_cast<uint8_t>(level << ((cell % 4) * 2));
    if (read) yearDaysRead++;
    yearMinutes += minutes;

    const uint32_t weekday = (daysIntoWeek + 7u - (todayIndex - day) % 7u) % 7u;
    weekdayMinutes[weekday] += minutes;

    // A streak survives today until today is over.
    if (read) {
      run++;
      longestStreak = std::max(longestStreak, run);
    } else if (day != todayIndex) {
      run = 0;
    }

    ReadingStatsDate date;
    if (readingStatsDateFromDayIndex(day, date)) {
      const uint32_t key = date.year * 12u + date.month;
      if (key != monthKey) {
        if (monthIsWhole) yearBestMonthMinutes = std::max(yearBestMonthMinutes, monthMinutes);
        monthIsWhole = monthKey != 0 || date.day == 1;
        monthKey = key;
        monthMinutes = 0;
      }
      monthMinutes += minutes;
    }

    if (day + 7u > todayIndex) {
      const size_t slot = day + 6u - todayIndex;
      weekMinutes[slot] = minutes;
      weekTotal += minutes;
      if (read) lastSevenDays |= static_cast<uint8_t>(1u << slot);
    }
    if (day + kForecastPaceDays > todayIndex) recentMinutes += minutes;
  }
  if (monthIsWhole) yearBestMonthMinutes = std::max(yearBestMonthMinutes, monthMinutes);
  streak = run;
  if (snapshot) {
    longestStreak = std::max(longestStreak, static_cast<uint16_t>(snapshot->longestStreak));
    // What the server counted is only still true on the day it was counted.
    if (snapshot->fetchDay == todayIndex) {
      streak = std::max(streak, static_cast<uint16_t>(snapshot->currentStreak));
      lastSevenDays |= snapshot->lastSevenDays;
    }
  }

  // Same forecast as BookOrbit's plugin: the last two weeks' pace, at least five minutes
  // a day, so a book read in one sitting does not forecast as ending today forever.
  if (data.currentBookStats != nullptr && !data.books->empty() && !data.currentBookStats->isCompleted &&
      data.currentBookStats->estimatedTimeLeft(data.currentBookProgress, estimatedSecondsLeft)) {
    dailyPaceMinutes = std::max<uint32_t>(kForecastMinPaceMinutes, recentMinutes / kForecastPaceDays);
    const uint32_t minutesLeft = (estimatedSecondsLeft + 30u) / 60u;
    finishDate = today.date;
    addDaysToReadingStatsDate(finishDate,
                              static_cast<int>((minutesLeft + dailyPaceMinutes - 1) / dailyPaceMinutes) - 1);
    hasForecast = finishDate.isValid();
  }
}

void BookOrbitHomeUi::computeLayout() {
  layout = BookOrbitHomeLayout::grid(renderer);

  availablePages = 0;
  pages[availablePages++] = Page::Home;
  if (snapshot) pages[availablePages++] = Page::Library;
  if (showStats) pages[availablePages++] = Page::Stats;

  stopCount = 0;

  // Home. The current book comes first: it is what Confirm reopens straight away.
  const bool hasBooks = data.books != nullptr && !data.books->empty();
  markPageStops(Page::Home);
  addStop(currentBookTile(), kBookRadius, kRingOutset, hasBooks ? Action::OpenBook : Action::OpenLibrary);
  addStop(showStats ? cell(1, 3) : cell(0, 2, 2), kTileRadius, kRingOutset,
          hasAccount ? Action::Refresh : Action::OpenLibrary);
  const Rect also = alsoReadingTile();
  // The header's "All (N) >" leads to every book in progress, which the rows cannot hold.
  addStop(alsoReadingLinkRect(also), 6, 0, hasAccount ? Action::OpenContinueReading : Action::OpenLibrary);
  for (int i = 0; i < rowCount; i++) {
    const Row& row = rows[static_cast<size_t>(i)];
    if (row.local != nullptr) {
      addStop(bookRowRect(also, i), 6, 0, Action::OpenBook, i + 1);
    } else {
      addStop(bookRowRect(also, i), 6, 0, Action::OpenCatalogBook, row.remoteArg);
    }
  }

  // Library.
  markPageStops(Page::Library);
  if (snapshot) {
    const Rect discover = cell(0, 0, 2, 2);
    for (int i = 0; i < snapshot->discoverCount; i++) {
      addStop(shelfCoverRect(discover, i), 2, kRingOutset, Action::OpenCatalogBook, kDiscoverArgBase + i);
    }
    const Rect recent = cell(0, 2, 2, 2);
    for (int i = 0; i < snapshot->recentCount; i++) {
      addStop(shelfCoverRect(recent, i), 2, kRingOutset, Action::OpenCatalogBook, kRecentArgBase + i);
    }
    addStop(cell(0, 4), kTileRadius, kRingOutset, Action::OpenCatalog);
    addStop(cell(1, 4), kTileRadius, kRingOutset, Action::OpenAuthors);
    addStop(cell(0, 5), kTileRadius, kRingOutset, Action::OpenSeries);
    addStop(cell(1, 5), kTileRadius, kRingOutset, Action::OpenLibrary);
  }

  // Stats.
  markPageStops(Page::Stats);
  if (showStats) {
    const bool hasBadge = snapshot && snapshot->badgeThreshold > 0;
    addStop(statsCell(0, 0, 2), kTileRadius, kRingOutset, Action::OpenStats);
    if (hasBooks) addStop(statsCell(0, 1, hasBadge ? 1 : 2), kTileRadius, kRingOutset, Action::OpenBook);
    if (snapshot && snapshot->highlightText[0] != '\0') {
      addStop(statsCell(0, 4, 2), kTileRadius, kRingOutset, Action::OpenSavedItems);
    }
  }

  markPageStops(Page::Count);
}

// The covers on a shelf, and the room under them for a caption. The ring needs the
// title's last pixels to end above it, and the neighbouring cover to start clear of it.
Rect BookOrbitHomeUi::shelfCoverRect(const Rect& tile, const int index) const {
  const auto sizes = BookOrbitHomeLayout::coverSizes(renderer);
  const int count = BookOrbitHomeSnapshot::SHELF_COUNT;
  const int gap = (tile.width - 2 * kPad - count * sizes.shelfWidth) / (count - 1);
  return Rect{tile.x + kPad + index * (sizes.shelfWidth + gap), tile.y + kShelfCoverTop, sizes.shelfWidth,
              sizes.shelfHeight + kShelfCaptionHeight};
}

void BookOrbitHomeUi::markPageStops(const Page page) {
  pageFirstStop[static_cast<size_t>(page)] = static_cast<uint8_t>(stopCount);
}

int BookOrbitHomeUi::pageFirstStopIndex(const int pageIndex) const {
  return pageFirstStop[static_cast<size_t>(pageAt(pageIndex))];
}

int BookOrbitHomeUi::pageStopCount(const int pageIndex) const {
  const size_t id = static_cast<size_t>(pageAt(pageIndex));
  return pageFirstStop[id + 1] - pageFirstStop[id];
}

void BookOrbitHomeUi::addStop(const Rect& rect, const int radius, const int ringOutset, const Action action,
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

BookOrbitHomeUi::Page BookOrbitHomeUi::pageAt(const int index) const {
  return pages[static_cast<size_t>(std::clamp(index, 0, availablePages - 1))];
}

Rect BookOrbitHomeUi::cell(const int column, const int row, const int columns, const int rows) const {
  const int x = kMargin + column * (layout.columnWidth + kGap);
  const int y = layout.top + row * (layout.rowHeight + kGap);
  const int width = columns == 2 ? layout.contentWidth : layout.columnWidth;
  return Rect{x, y, width, BookOrbitHomeLayout::spanHeight(layout, rows)};
}

// The stats page's rows: the year tile takes the height it needs, the rest shares what
// is left, so no tile holds a band of empty space.
Rect BookOrbitHomeUi::statsCell(const int column, const int row, const int columns) const {
  const int x = kMargin + column * (layout.columnWidth + kGap);
  const int width = columns == 2 ? layout.contentWidth : layout.columnWidth;
  if (row == 0) return Rect{x, layout.top, width, kStatsYearTileHeight};
  const int rowHeight = (layout.height - kStatsYearTileHeight - kStatsRows * kGap) / kStatsRows;
  return Rect{x, layout.top + kStatsYearTileHeight + kGap + (row - 1) * (rowHeight + kGap), width, rowHeight};
}

Rect BookOrbitHomeUi::currentBookTile() const { return cell(0, 0, 2, 2); }

Rect BookOrbitHomeUi::alsoReadingTile() const {
  const int rows = BookOrbitHomeLayout::alsoReadingRows(showStats);
  return cell(0, kRowCount - rows, 2, rows);
}

// The count of what the link opens: the server's books in progress once a sync has
// counted them, the recent books on the card before that or without an account.
void BookOrbitHomeUi::formatAlsoReadingLink(char* out, const size_t outSize) const {
  const auto count = snapshot && snapshot->continueTotal >= 0 ? static_cast<unsigned>(snapshot->continueTotal)
                                                              : static_cast<unsigned>(data.recentBookCount);
  snprintf(out, outSize, tr(STR_HOME_ALL_FORMAT), count);
}

// Button boards ring the header's link alone, clear of its text so that erasing the ring
// leaves the text whole, and right-aligned with the rows' rings. Touch boards draw no
// ring, so the whole header band takes the tap: an easier target than a line of text.
Rect BookOrbitHomeUi::alsoReadingLinkRect(const Rect& tile) const {
  if (!showRing) return Rect{tile.x, tile.y, tile.width, kBookRowHeaderHeight};
  char link[24];
  formatAlsoReadingLink(link, sizeof(link));
  // The text ends kPad from the tile's edge; the ring keeps 4 px from it on either side,
  // and ends on the header band's last line, just above the first row.
  const int right = tile.x + tile.width - 5;
  const int width = renderer.getTextWidth(SMALL_FONT_ID, link) + kPad + 2;
  return Rect{right - width, tile.y + 4, width, kBookRowHeaderHeight - 4};
}

Rect BookOrbitHomeUi::bookRowRect(const Rect& tile, const int rowIndex) const {
  const int rowHeight = BookOrbitHomeLayout::bookRowHeight(tile.height);
  return Rect{tile.x + 5, tile.y + kBookRowHeaderHeight + rowIndex * rowHeight, tile.width - 10, rowHeight};
}

Rect BookOrbitHomeUi::coverSlot(const int bookIndex) const {
  if (bookIndex == 0) {
    const Rect tile = currentBookTile();
    const int height = tile.height - 2 * kBookPad;
    return Rect{tile.x + kBookPad, tile.y + kBookPad, height * 2 / 3, height};
  }
  const Rect row = bookRowRect(alsoReadingTile(), bookIndex - 1);
  const int height = row.height - kBookRowCoverInset;
  return Rect{row.x + 7, row.y + 5, height * 2 / 3, height};
}

std::string BookOrbitHomeUi::coverPath(const RecentBook& book, const int width, const int height) {
  if (book.coverBmpPath.empty()) return {};
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint").getAdaptiveThumbBmpPath(width, height);
  }
  return UITheme::getCoverThumbPath(book.coverBmpPath, width, height);
}

void BookOrbitHomeUi::moveSelection(const int delta) {
  // Wraps around the page: changing page is movePage()'s job.
  const int count = pageStopCount(currentPage);
  if (count <= 0) return;
  selection = ((selection + delta) % count + count) % count;
}

void BookOrbitHomeUi::movePage(const int delta) {
  selection = 0;
  currentPage = (currentPage + delta + availablePages) % availablePages;
}

const BookOrbitHomeUi::Stop* BookOrbitHomeUi::stopAt(const int index) const {
  if (index < 0 || index >= stopCount) return nullptr;
  return &stops[static_cast<size_t>(index)];
}

const BookOrbitHomeUi::Stop* BookOrbitHomeUi::selectedStop() const {
  return selection < pageStopCount(currentPage) ? stopAt(pageFirstStopIndex(currentPage) + selection) : nullptr;
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
  const int pageIndexNow = std::clamp(currentPage, 0, availablePages - 1);
  const Page page = pageAt(pageIndexNow);
  const int stop =
      showRing && selection < pageStopCount(pageIndexNow) ? pageFirstStopIndex(pageIndexNow) + selection : -1;

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
  const int first = pageFirstStopIndex(currentPage);
  const int count = pageStopCount(currentPage);
  for (int i = first; i < first + count; i++) {
    const Stop& stop = stops[static_cast<size_t>(i)];
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
  const Stop* stop = stopAt(stopIndex);
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

void BookOrbitHomeUi::drawTitleRow(const char* title) const {
  const int y = kTitleRowTop + UITheme::getTopStatusBarInset(renderer);
  const int titleWidth = renderer.getScreenWidth() - 2 * kMargin - (availablePages * 15 + 8);
  drawFitted(UI_12_FONT_ID, kMargin, y, title, titleWidth, true);
  constexpr int dotRadius = 4;
  int centerX = renderer.getScreenWidth() - kMargin - dotRadius;
  const int centerY = y + 17;
  for (int i = availablePages - 1; i >= 0; i--) {
    drawDot(centerX - dotRadius, centerY - dotRadius, dotRadius, i == currentPage);
    centerX -= 15;
  }
}

void BookOrbitHomeUi::drawNavigation(const Page page) const {
  if (showRing) {
    const auto labels =
        mappedInput.mapLabels(tr(STR_MENU), tr(STR_OPEN), tr(STR_HOME_PAGE_PREVIOUS), tr(STR_HOME_PAGE_NEXT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const int y = renderer.getScreenHeight() - 40;
  if (currentPage > 0) drawLucideIcon(renderer, icon_home_prev_24, kMargin + 2, y + 8);
  if (currentPage < availablePages - 1) {
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
  switch (page) {
    case Page::Home:
      drawHomePage();
      break;
    case Page::Library:
      drawLibraryPage();
      break;
    case Page::Stats:
      drawStatsPage();
      break;
    case Page::Count:
      break;
  }
  drawNavigation(page);
}

void BookOrbitHomeUi::drawFitted(const int fontId, const int x, const int y, const char* text, const int maxWidth,
                                 const bool bold, const bool black) const {
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  renderer.drawText(fontId, x, y, renderer.truncatedText(fontId, text, maxWidth, style).c_str(), black, style);
}

void BookOrbitHomeUi::drawTileFrame(const Rect& tile) const {
  renderer.drawRoundedRect(tile.x, tile.y, tile.width, tile.height, 1, kTileRadius, true);
}

void BookOrbitHomeUi::drawTileHeader(const Rect& tile, const char* title, const freeink::Icon* icon) const {
  drawTileFrame(tile);
  const int reserved = icon != nullptr ? kIconSize + 2 * kPad : kPad;
  drawFitted(UI_10_FONT_ID, tile.x + kPad, tile.y + 7, title, tile.width - kPad - reserved, true);
  if (icon != nullptr) {
    drawLucideIcon(renderer, *icon, tile.x + tile.width - kPad - kIconSize, tile.y + 7);
  }
}

void BookOrbitHomeUi::drawTileFooter(const Rect& tile, const char* text) const {
  drawFitted(SMALL_FONT_ID, tile.x + kPad, tile.y + tile.height - 26, text, tile.width - 2 * kPad);
}

void BookOrbitHomeUi::drawRightAligned(const int fontId, const int right, const int y, const char* text,
                                       const bool bold) const {
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  renderer.drawText(fontId, right - renderer.getTextWidth(fontId, text, style), y, text, true, style);
}

void BookOrbitHomeUi::drawDot(const int left, const int top, const int radius, const bool filled) const {
  const int size = 2 * radius + 1;
  if (filled) {
    renderer.fillRoundedRect(left, top, size, size, radius, Color::Black);
  } else {
    renderer.drawRoundedRect(left, top, size, size, 1, radius, true);
  }
}

void BookOrbitHomeUi::drawBigValue(const Rect& tile, const char* value, const char* unit) const {
  const int y = tile.y + kBigValueTop;
  const int valueY = y + kBigValueBaseline - renderer.getFontAscenderSize(kBigFontId);
  renderer.drawText(kBigFontId, tile.x + kPad, valueY, value, true, EpdFontFamily::BOLD);
  if (unit == nullptr) return;
  const int x = tile.x + kPad + renderer.getTextWidth(kBigFontId, value, EpdFontFamily::BOLD) + 5;
  drawFitted(UI_10_FONT_ID, x, y + 14, unit, tile.x + tile.width - kPad - x);
}

void BookOrbitHomeUi::drawProgressBar(const int x, const int y, const int width, const int height, const float fraction,
                                      const bool black) const {
  renderer.drawRoundedRect(x, y, width, height, 1, height / 2, black);
  const float clamped = std::clamp(fraction, 0.0f, 1.0f);
  if (clamped <= 0.0f) return;
  const int fill = std::max(height, static_cast<int>(static_cast<float>(width - 2) * clamped + 0.5f));
  renderer.fillRoundedRect(x + 1, y + 1, std::min(fill, width - 2), height - 2, (height - 2) / 2,
                           black ? Color::Black : Color::White);
}

void BookOrbitHomeUi::drawBar(const int x, const int base, const int width, const int chartHeight, const uint32_t value,
                              const uint32_t highest, const bool emphasized) const {
  const int height = std::max(value > 0 ? 3 : 1, static_cast<int>(chartHeight * value / highest));
  if (emphasized) {
    renderer.fillRect(x, base - height, width, height);
  } else {
    renderer.fillRectDither(x, base - height, width, height, Color::DarkGray);
    renderer.drawRect(x, base - height, width, height);
  }
}

void BookOrbitHomeUi::drawColumnChart(const Rect& area, const uint32_t* values, const int count, const int emphasized,
                                      const char* initials) const {
  const int barWidth = std::min(22, (area.width - (count - 1) * 6) / count);
  const int gap = count > 1 ? (area.width - count * barWidth) / (count - 1) : 0;
  const int labelHeight = 21;
  const int base = area.y + area.height - labelHeight;
  const int chartHeight = base - area.y;
  uint32_t highest = 1;
  for (int i = 0; i < count; i++) highest = std::max(highest, values[i]);
  for (int i = 0; i < count; i++) {
    const int x = area.x + i * (barWidth + gap);
    drawBar(x, base, barWidth, chartHeight, values[i], highest, i == emphasized);
    char initial[5];
    copyInitial(initials, i, initial, sizeof(initial));
    renderer.drawText(SMALL_FONT_ID, x + (barWidth - renderer.getTextWidth(SMALL_FONT_ID, initial)) / 2, base, initial);
  }
  renderer.drawLine(area.x, base, area.x + area.width - 1, base);
}

void BookOrbitHomeUi::drawCover(const RecentBook& book, const Rect& slot, const bool onBlack) const {
  drawCoverFile(coverPath(book, slot.width, slot.height).c_str(), slot, onBlack);
}

void BookOrbitHomeUi::drawCoverFile(const char* path, const Rect& slot, const bool onBlack) const {
  if (path[0] != '\0' && Storage.exists(path)) {
    HalFile file;
    if (Storage.openFileForRead("HOME", path, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const Rect fitted = fittedBitmapRect(bitmap, slot);
        renderer.fillRect(fitted.x, fitted.y, fitted.width, fitted.height, false);
        renderer.drawBitmap(bitmap, fitted.x, fitted.y, fitted.width, fitted.height);
        renderer.drawRect(fitted.x, fitted.y, fitted.width, fitted.height, !onBlack);
        file.close();
        return;
      }
      file.close();
    }
  }
  renderer.fillRect(slot.x, slot.y, slot.width, slot.height, false);
  renderer.drawRect(slot.x, slot.y, slot.width, slot.height, !onBlack);
  if (slot.width >= icon_book_open_32.w + 8) {
    drawLucideIcon(renderer, icon_book_open_32, slot.x + (slot.width - icon_book_open_32.w) / 2,
                   slot.y + (slot.height - icon_book_open_32.h) / 2);
  }
}

void BookOrbitHomeUi::drawHomePage() {
  char title[64];
  const char* heading = hasToday ? greeting(today) : tr(STR_HOME);
  if (hasToday && snapshot && snapshot->displayName[0] != '\0') {
    snprintf(title, sizeof(title), "%s, %s", heading, snapshot->displayName);
  } else {
    snprintf(title, sizeof(title), "%s", heading);
  }
  drawTitleRow(title);

  drawCurrentBook(currentBookTile());
  if (showStats) {
    drawStreak(cell(0, 2));
    drawWeek(cell(1, 2));
    drawGoal(cell(0, 3));
    drawAccountTile(cell(1, 3));
  } else {
    drawAccountTile(cell(0, 2, 2));
  }
  drawAlsoReading(alsoReadingTile());
}

void BookOrbitHomeUi::drawCurrentBook(const Rect& tile) const {
  renderer.fillRoundedRect(tile.x, tile.y, tile.width, tile.height, kBookRadius, Color::Black);
  if (data.books == nullptr || data.books->empty()) {
    const char* text = tr(STR_NO_OPEN_BOOK);
    const int width = renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, tile.x + (tile.width - width) / 2, tile.y + tile.height / 2 - 15, text, false,
                      EpdFontFamily::BOLD);
    return;
  }

  const RecentBook& book = data.books->front();
  const Rect slot = coverSlot(0);
  drawCover(book, slot, true);

  const int x = slot.x + slot.width + 14;
  const int width = tile.x + tile.width - 14 - x;
  int y = tile.y + 11;
  renderer.drawText(SMALL_FONT_ID, x, y, tr(STR_HOME_READING), false);
  y += 21;
  for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, displayTitle(book), width, 2, EpdFontFamily::BOLD)) {
    renderer.drawText(UI_12_FONT_ID, x, y, line.c_str(), false, EpdFontFamily::BOLD);
    y += 28;
  }
  y += 1;
  if (!book.author.empty()) {
    drawFitted(UI_10_FONT_ID, x, y, book.author.c_str(), width, false, false);
    y += 25;
  }
  if (data.chapterTitle != nullptr && !data.chapterTitle->empty()) {
    drawFitted(SMALL_FONT_ID, x, y, data.chapterTitle->c_str(), width, false, false);
  }

  constexpr int pillHeight = 36;
  const int pillY = tile.y + tile.height - kBookPad - pillHeight;
  if (data.currentBookProgress >= 0.0f) {
    const int barY = pillY - 22;
    char percent[8];
    formatPercent(data.currentBookProgress, percent, sizeof(percent));
    const int percentWidth = renderer.getTextWidth(UI_10_FONT_ID, percent, EpdFontFamily::BOLD);
    drawProgressBar(x, barY, width - percentWidth - 12, 8, data.currentBookProgress / 100.0f, false);
    renderer.drawText(UI_10_FONT_ID, x + width - percentWidth, barY - 8, percent, false, EpdFontFamily::BOLD);
  }

  const char* resume = tr(STR_HOME_RESUME);
  const int pillWidth = renderer.getTextWidth(UI_10_FONT_ID, resume, EpdFontFamily::BOLD) + 34;
  renderer.fillRoundedRect(x, pillY, pillWidth, pillHeight, pillHeight / 2, Color::White);
  renderer.drawText(UI_10_FONT_ID, x + 17, pillY + 5, resume, true, EpdFontFamily::BOLD);

  if (hasForecast || (data.currentBookStats != nullptr && data.currentBookStats->estimatedTimeLeftSeconds > 0)) {
    const uint32_t seconds = hasForecast ? estimatedSecondsLeft : data.currentBookStats->estimatedTimeLeftSeconds;
    char duration[16];
    formatCompactReadingDuration(seconds, duration, sizeof(duration));
    char left[32];
    snprintf(left, sizeof(left), tr(STR_HOME_LEFT_FORMAT), duration);
    const int leftX = x + pillWidth + 10;
    drawFitted(SMALL_FONT_ID, leftX, pillY + 8, left, tile.x + tile.width - 14 - leftX, false, false);
  }
}

void BookOrbitHomeUi::drawStreak(const Rect& tile) const {
  drawTileHeader(tile, tr(STR_HOME_STREAK));
  renderer.drawIcon(StreakIcon, tile.x + tile.width - kPad - kIconSize, tile.y + 7, kIconSize, kIconSize);
  char value[8];
  snprintf(value, sizeof(value), "%u", static_cast<unsigned>(streak));
  drawBigValue(tile, value, streak == 1 ? tr(STR_HOME_DAY) : tr(STR_HOME_DAYS));

  constexpr int radius = 7;
  const float step = static_cast<float>(tile.width - 2 * kPad - 2 * radius) / 6.0f;
  const int centerY = tile.y + tile.height - 17;
  for (int i = 0; i < 7; i++) {
    const int left = tile.x + kPad + static_cast<int>(static_cast<float>(i) * step + 0.5f);
    drawDot(left, centerY - radius, radius, (lastSevenDays & (1u << i)) != 0);
  }
}

void BookOrbitHomeUi::drawWeek(const Rect& tile) const {
  drawTileHeader(tile, tr(STR_HOME_THIS_WEEK));
  if (weekTotal > 0) {
    char total[16];
    formatCompactReadingDuration(weekTotal * 60u, total, sizeof(total));
    drawRightAligned(UI_10_FONT_ID, tile.x + tile.width - kPad, tile.y + 7, total, true);
  }

  // The last seven days, oldest first, so today's initial depends on the date.
  const char* initials = tr(STR_HOME_WEEKDAY_INITIALS);
  char rotated[32] = {};
  size_t used = 0;
  const int todayOfWeek = hasToday ? readingStatsDayOfWeekIndex(today.date) : 6;
  for (int i = 0; i < 7; i++) {
    char initial[5];
    copyInitial(initials, (todayOfWeek + 1 + i) % 7, initial, sizeof(initial));
    const size_t length = strlen(initial);
    if (used + length >= sizeof(rotated)) break;
    memcpy(rotated + used, initial, length);
    used += length;
  }
  drawColumnChart(Rect{tile.x + kPad, tile.y + 36, tile.width - 2 * kPad, tile.height - 36 - 4}, weekMinutes.data(), 7,
                  6, rotated);
}

void BookOrbitHomeUi::drawGoal(const Rect& tile) const {
  if (snapshot && snapshot->goalBooks > 0) {
    char title[32];
    snprintf(title, sizeof(title), tr(STR_HOME_GOAL_FORMAT), static_cast<unsigned>(snapshot->goalYear));
    drawTileHeader(tile, title, &icon_home_trophy_24);
    char completed[12];
    snprintf(completed, sizeof(completed), "%ld", static_cast<long>(goalCompleted));
    char goal[32];
    snprintf(goal, sizeof(goal), tr(STR_HOME_OF_BOOKS_FORMAT), static_cast<unsigned>(snapshot->goalBooks));
    drawBigValue(tile, completed, goal);
    if (hasToday && today.date.year == snapshot->goalYear) {
      ReadingStatsDate newYear;
      newYear.year = today.date.year;
      newYear.month = 1;
      newYear.day = 1;
      const uint32_t dayOfYear = readingStatsDayIndex(today.date) - readingStatsDayIndex(newYear) + 1;
      const uint32_t daysInYear = isLeapYear(today.date.year) ? 366u : 365u;
      char pace[32];
      snprintf(
          pace, sizeof(pace), tr(STR_HOME_ON_PACE_FORMAT),
          static_cast<unsigned>(static_cast<uint32_t>(std::max<int32_t>(0, goalCompleted)) * daysInYear / dayOfYear));
      drawTileFooter(tile, pace);
    }
    return;
  }
  drawTileHeader(tile, tr(STR_HOME_FINISHED), &icon_home_trophy_24);
  char value[12];
  snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(data.stats ? data.stats->completedBooks : 0));
  drawBigValue(tile, value, tr(STR_HOME_BOOKS));
  drawTileFooter(tile, tr(STR_HOME_ON_THIS_DEVICE));
}

void BookOrbitHomeUi::drawAccountTile(const Rect& tile) const {
  if (!hasAccount) {
    drawTileHeader(tile, tr(STR_LIBRARY), &icon_home_books_24);
    drawTileFooter(tile, tr(STR_BROWSE));
    return;
  }
  drawTileHeader(tile, tr(STR_BOOKORBIT), &icon_home_refresh_24);
  drawSyncAge(tile);
  drawTileFooter(tile, tr(STR_HOME_SYNC_NOW));
}

void BookOrbitHomeUi::drawSyncAge(const Rect& tile) const {
  const int y = tile.y + 37;
  const time_t now = time(nullptr);
  if (!snapshot) {
    drawFitted(UI_10_FONT_ID, tile.x + kPad, y, tr(STR_HOME_NOT_SYNCED), tile.width - 2 * kPad);
    return;
  }
  if (snapshot->generatedAt == 0 || now < static_cast<time_t>(snapshot->generatedAt)) return;
  const uint32_t minutes = static_cast<uint32_t>(now - snapshot->generatedAt) / 60u;
  char age[16];
  if (minutes < 60) {
    snprintf(age, sizeof(age), "%lu min", static_cast<unsigned long>(minutes));
  } else if (minutes < 48u * 60u) {
    formatHours(minutes / 60u, age, sizeof(age));
  } else {
    char dayInitial[5];
    copyInitial(tr(STR_HOME_DAYS), 0, dayInitial, sizeof(dayInitial));
    snprintf(age, sizeof(age), "%lu %s", static_cast<unsigned long>(minutes / (24u * 60u)), dayInitial);
  }
  char text[32];
  snprintf(text, sizeof(text), tr(STR_HOME_SYNCED_AGO_FORMAT), age);
  drawFitted(UI_12_FONT_ID, tile.x + kPad, y - 2, text, tile.width - 2 * kPad, true);
}

void BookOrbitHomeUi::drawAlsoReading(const Rect& tile) const {
  drawTileHeader(tile, tr(STR_HOME_ALSO_READING));
  char link[24];
  formatAlsoReadingLink(link, sizeof(link));
  // At the title's height, which centres it in the ring alsoReadingLinkRect() draws.
  drawRightAligned(SMALL_FONT_ID, tile.x + tile.width - kPad, tile.y + 7, link);

  for (int i = 0; i < rowCount; i++) {
    const Row& book = rows[static_cast<size_t>(i)];
    const Rect row = bookRowRect(tile, i);
    const Rect slot = coverSlot(i + 1);
    const char* title = "";
    if (book.local != nullptr) {
      drawCover(*book.local, slot, false);
      title = displayTitle(*book.local);
    } else {
      char path[64];
      BookOrbitHomeStore::coverPath(*book.remote, slot.width, slot.height, path, sizeof(path));
      drawCoverFile(path, slot, false);
      title = book.remote->title;
    }

    const int x = slot.x + slot.width + 12;
    const int width = tile.x + tile.width - kPad - x;
    drawFitted(UI_10_FONT_ID, x, row.y + 4, title, width, true);
    if (!book.onCard) {
      drawLucideIcon(renderer, icon_home_cloud_down_24, tile.x + tile.width - kPad - 66, row.y + 28);
    }
    if (book.progress >= 0.0f) {
      char percent[8];
      formatPercent(book.progress, percent, sizeof(percent));
      drawProgressBar(x, row.y + 36, width - 76, 7, book.progress / 100.0f);
      drawRightAligned(SMALL_FONT_ID, tile.x + tile.width - kPad, row.y + 29, percent);
    }
  }
}

void BookOrbitHomeUi::drawStatsPage() {
  drawTitleRow(tr(STR_HOME_STATS));
  drawYear(statsCell(0, 0, 2));
  if (snapshot && snapshot->badgeThreshold > 0) {
    drawForecast(statsCell(0, 1));
    drawBadge(statsCell(1, 1));
  } else {
    drawForecast(statsCell(0, 1, 2));
  }
  drawTimeOfDay(statsCell(0, 2));
  drawWeekdays(statsCell(1, 2));
  drawRecords(statsCell(0, 3, 2));
  if (snapshot && snapshot->highlightText[0] != '\0') {
    drawHighlight(statsCell(0, 4, 2));
  } else {
    drawActivity(statsCell(0, 4, 2));
  }
}

void BookOrbitHomeUi::drawYear(const Rect& tile) const {
  drawTileHeader(tile, tr(STR_HOME_YEAR_IN_READING));

  const int gridX = tile.x + tile.width - kPad - YEAR_WEEKS * kYearCellPitch;
  const int gridY = tile.y + 50;

  // Month names every other month: every month would run into its neighbour.
  const char* months = tr(STR_HOME_MONTHS_SHORT);
  int previousMonth = 0;
  int monthsSeen = 0;
  for (int week = 0; week < YEAR_WEEKS && yearCellCount > 0; week++) {
    ReadingStatsDate monday;
    if (!readingStatsDateFromDayIndex(yearFirstDay + static_cast<uint32_t>(week) * 7u, monday)) continue;
    if (monday.month == previousMonth) continue;
    previousMonth = monday.month;
    if (week > 0 || monday.day <= 7) {
      if (monthsSeen++ % 2 == 0) {
        char token[8];
        copyToken(months, monday.month - 1, token, sizeof(token));
        renderer.drawText(SMALL_FONT_ID, gridX + week * kYearCellPitch, tile.y + 29, token);
      }
    }
  }

  for (uint16_t cellIndex = 0; cellIndex < yearCellCount; cellIndex++) {
    const int x = gridX + (cellIndex / 7) * kYearCellPitch;
    const int y = gridY + (cellIndex % 7) * kYearCellPitch;
    switch ((yearLevels[cellIndex / 4] >> ((cellIndex % 4) * 2)) & 0x3) {
      case 0:
        renderer.drawPixel(x + kYearCellSize / 2, y + kYearCellSize / 2);
        break;
      case 1:
        renderer.fillRectDither(x, y, kYearCellSize, kYearCellSize, Color::LightGray);
        break;
      case 2:
        renderer.fillRectDither(x, y, kYearCellSize, kYearCellSize, Color::DarkGray);
        break;
      default:
        renderer.fillRect(x, y, kYearCellSize, kYearCellSize);
        break;
    }
  }

  const char* initials = tr(STR_HOME_WEEKDAY_INITIALS);
  for (const int day : {0, 3, 6}) {
    char initial[5];
    copyInitial(initials, day, initial, sizeof(initial));
    renderer.drawText(SMALL_FONT_ID, tile.x + kPad - 2, gridY + day * kYearCellPitch + kYearCellSize / 2 - 11, initial);
  }

  char hours[16];
  formatHours(yearMinutes / 60u, hours, sizeof(hours));
  char best[16];
  formatHours(yearBestMonthMinutes / 60u, best, sizeof(best));
  char daysRead[8];
  snprintf(daysRead, sizeof(daysRead), "%u", static_cast<unsigned>(yearDaysRead));
  const std::array<std::pair<const char*, const char*>, 3> columns{
      {{daysRead, tr(STR_HOME_DAYS_READ)}, {hours, tr(STR_HOME_OVER_A_YEAR)}, {best, tr(STR_HOME_BEST_MONTH)}}};
  const int statsY = gridY + 7 * kYearCellPitch + 6;
  const int columnWidth = (tile.width - 2 * kPad) / 3;
  int x = tile.x + kPad;
  for (const auto& [value, label] : columns) {
    renderer.drawText(UI_12_FONT_ID, x, statsY, value, true, EpdFontFamily::BOLD);
    drawFitted(SMALL_FONT_ID, x, statsY + 28, label, columnWidth - 6);
    x += columnWidth;
  }
}

void BookOrbitHomeUi::drawForecast(const Rect& tile) const {
  drawTileHeader(tile, tr(STR_HOME_FINISH_BY), &icon_home_calendar_24);
  if (!hasForecast) {
    renderer.drawText(UI_12_FONT_ID, tile.x + kPad, tile.y + 37, "-", true, EpdFontFamily::BOLD);
    return;
  }
  char month[8];
  copyToken(tr(STR_HOME_MONTHS_SHORT), finishDate.month - 1, month, sizeof(month));
  char date[24];
  snprintf(date, sizeof(date), "%u %s", static_cast<unsigned>(finishDate.day), month);
  renderer.drawText(UI_12_FONT_ID, tile.x + kPad, tile.y + 37, date, true, EpdFontFamily::BOLD);
  char duration[16];
  formatCompactReadingDuration(estimatedSecondsLeft, duration, sizeof(duration));
  char pace[48];
  snprintf(pace, sizeof(pace), tr(STR_HOME_PACE_FORMAT), duration, static_cast<unsigned>(dailyPaceMinutes));
  drawTileFooter(tile, pace);
}

void BookOrbitHomeUi::drawTimeOfDay(const Rect& tile) const {
  drawTileHeader(tile, tr(STR_HOME_TIME_OF_DAY));

  const std::array<const uint8_t*, READING_TIME_BUCKET_COUNT> icons{MorningReaderIcon, AfternoonReaderIcon,
                                                                    EveningReaderIcon, NightReaderIcon};
  const auto& seconds = timeOfDaySeconds;
  const uint32_t highest = std::max<uint32_t>(1, *std::max_element(seconds.begin(), seconds.end()));
  const int columnWidth = (tile.width - 2 * kPad) / static_cast<int>(seconds.size());
  const int base = tile.y + tile.height - kIconSize - 8;
  const int chartHeight = base - (tile.y + 36);
  for (size_t i = 0; i < seconds.size(); i++) {
    const int x = tile.x + kPad + static_cast<int>(i) * columnWidth;
    drawBar(x + 8, base, columnWidth - 16, chartHeight, seconds[i], highest, seconds[i] == highest);
    renderer.drawIcon(icons[i], x + (columnWidth - kIconSize) / 2, base + 4, kIconSize, kIconSize);
  }
  renderer.drawLine(tile.x + kPad, base, tile.x + tile.width - kPad - 1, base);
}

void BookOrbitHomeUi::drawWeekdays(const Rect& tile) const {
  drawTileHeader(tile, tr(STR_HOME_WEEKDAYS));
  const auto highest = std::max_element(weekdayMinutes.begin(), weekdayMinutes.end());
  drawColumnChart(Rect{tile.x + kPad, tile.y + 36, tile.width - 2 * kPad, tile.height - 36 - 4}, weekdayMinutes.data(),
                  static_cast<int>(weekdayMinutes.size()), static_cast<int>(highest - weekdayMinutes.begin()),
                  tr(STR_HOME_WEEKDAY_INITIALS));
}

void BookOrbitHomeUi::drawMetrics(const Rect& tile, const char* const* values, const char* const* labels,
                                  const int count) const {
  drawTileFrame(tile);
  drawMetricColumns(tile, tile.y + (tile.height - 50) / 2, tile.y + 24, tile.y + tile.height - 24, values, labels,
                    count);
}

// Values over their labels, in columns split by rules running from ruleTop to ruleBottom.
void BookOrbitHomeUi::drawMetricColumns(const Rect& tile, const int valueY, const int ruleTop, const int ruleBottom,
                                        const char* const* values, const char* const* labels, const int count) const {
  const int columnWidth = (tile.width - 2 * kPad) / count;
  for (int i = 0; i < count; i++) {
    const int x = tile.x + kPad + i * columnWidth;
    if (i > 0) renderer.drawLine(x - 9, ruleTop, x - 9, ruleBottom);
    renderer.drawText(UI_12_FONT_ID, x, valueY, values[i], true, EpdFontFamily::BOLD);
    drawFitted(SMALL_FONT_ID, x, valueY + 30, labels[i], columnWidth - 12);
  }
}

// The Library's books and the ones the catalog downloaded, over a bar of how full the
// card is with its used and total space beside it.
void BookOrbitHomeUi::drawCardTile(const Rect& tile) const {
  drawTileHeader(tile, tr(STR_HOME_ON_CARD), &icon_home_card_24);
  char books[12] = "-";
  if (cardBookCount >= 0) snprintf(books, sizeof(books), "%ld", static_cast<long>(cardBookCount));
  char server[12];
  snprintf(server, sizeof(server), "%ld", static_cast<long>(serverBookCount));
  const char* values[] = {books, server};
  const char* labels[] = {tr(STR_HOME_BOOKS), tr(STR_BOOKORBIT)};
  const int valueY = tile.y + 30;
  drawMetricColumns(tile, valueY, valueY, valueY + 44, values, labels, 2);
  if (cardTotalBytes == 0) return;

  char used[8];
  char total[8];
  formatGigabytes(cardUsedBytes, used, sizeof(used));
  formatGigabytes(cardTotalBytes, total, sizeof(total));
  char space[24];
  snprintf(space, sizeof(space), tr(STR_HOME_STORAGE_FORMAT), used, total);
  constexpr int kBarHeight = 10;
  const int barY = tile.y + tile.height - kPad - kBarHeight;
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, space);
  const int barWidth = tile.width - 2 * kPad - textWidth - 8;
  drawProgressBar(tile.x + kPad, barY, barWidth, kBarHeight,
                  static_cast<float>(static_cast<double>(cardUsedBytes) / static_cast<double>(cardTotalBytes)));
  renderer.drawText(SMALL_FONT_ID, tile.x + tile.width - kPad - textWidth,
                    barY + (kBarHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2, space);
}

void BookOrbitHomeUi::drawRecords(const Rect& tile) const {
  if (data.stats == nullptr) return drawTileFrame(tile);
  const GlobalReadingStats& stats = *data.stats;
  char time[12];
  formatHours(stats.totalReadingSeconds / 3600u, time, sizeof(time));
  char pages[12];
  if (stats.totalPagesTurned >= 1000u) {
    snprintf(pages, sizeof(pages), "%lu k", static_cast<unsigned long>(stats.totalPagesTurned / 1000u));
  } else {
    snprintf(pages, sizeof(pages), "%lu", static_cast<unsigned long>(stats.totalPagesTurned));
  }
  char books[12];
  snprintf(books, sizeof(books), "%lu", static_cast<unsigned long>(stats.completedBooks));
  char best[12];
  snprintf(best, sizeof(best), "%u", static_cast<unsigned>(longestStreak));
  const char* values[] = {time, pages, books, best};
  const char* labels[] = {tr(STR_HOME_TOTAL_TIME), tr(STR_HOME_PAGES), tr(STR_HOME_BOOKS_FINISHED),
                          tr(STR_HOME_BEST_STREAK)};
  drawMetrics(tile, values, labels, 4);
}

// Where the server's highlight of the day goes when there is none to show.
void BookOrbitHomeUi::drawActivity(const Rect& tile) const {
  if (data.stats == nullptr) return drawTileFrame(tile);
  const GlobalReadingStats& stats = *data.stats;
  char sessions[12];
  snprintf(sessions, sizeof(sessions), "%lu", static_cast<unsigned long>(stats.totalSessions));
  char perSession[16] = "-";
  if (stats.totalSessions > 0) {
    formatCompactReadingDuration(stats.totalReadingSeconds / stats.totalSessions, perSession, sizeof(perSession));
  }
  char pace[12];
  const uint32_t pagesPerTenMinutes =
      stats.totalReadingSeconds > 60u
          ? static_cast<uint32_t>(static_cast<uint64_t>(stats.totalPagesTurned) * 600u / stats.totalReadingSeconds)
          : 0;
  snprintf(pace, sizeof(pace), "%lu.%lu", static_cast<unsigned long>(pagesPerTenMinutes / 10u),
           static_cast<unsigned long>(pagesPerTenMinutes % 10u));
  char days[12];
  snprintf(days, sizeof(days), "%u", static_cast<unsigned>(yearDaysRead));
  const char* values[] = {sessions, perSession, pace, days};
  const char* labels[] = {tr(STR_HOME_SESSIONS), tr(STR_HOME_PER_SESSION), tr(STR_HOME_PAGES_PER_MIN),
                          tr(STR_HOME_DAYS_READ)};
  drawMetrics(tile, values, labels, 4);
}

void BookOrbitHomeUi::drawBadge(const Rect& tile) const {
  drawTileHeader(tile, tr(STR_HOME_NEXT_BADGE), &icon_home_award_24);
  drawFitted(UI_10_FONT_ID, tile.x + kPad, tile.y + 38, snapshot->badgeName, tile.width - 2 * kPad, true);
  drawProgressBar(tile.x + kPad, tile.y + 71, tile.width - 2 * kPad, 8,
                  static_cast<float>(snapshot->badgeProgress) / static_cast<float>(snapshot->badgeThreshold));
  char progress[24];
  snprintf(progress, sizeof(progress), tr(STR_HOME_BADGE_PROGRESS_FORMAT),
           static_cast<unsigned>(snapshot->badgeProgress), static_cast<unsigned>(snapshot->badgeThreshold));
  drawTileFooter(tile, progress);
}

void BookOrbitHomeUi::drawHighlight(const Rect& tile) const {
  drawTileFrame(tile);
  drawLucideIcon(renderer, icon_home_quote_24, tile.x + kPad - 2, tile.y + 6);
  renderer.drawText(UI_10_FONT_ID, tile.x + kPad + kIconSize + 4, tile.y + 6, tr(STR_HOME_HIGHLIGHT), true,
                    EpdFontFamily::BOLD);
  const bool hasBook = snapshot->highlightBook[0] != '\0';
  const int textWidth = tile.width - 2 * kPad;
  const int lines = (tile.height - 33 - (hasBook ? 26 : 6)) / renderer.getLineHeight(UI_10_FONT_ID);
  int y = tile.y + 33;
  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, snapshot->highlightText, textWidth, lines)) {
    renderer.drawText(UI_10_FONT_ID, tile.x + kPad, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  if (hasBook) {
    drawTileFooter(tile, snapshot->highlightBook);
  }
}

void BookOrbitHomeUi::drawLibraryPage() {
  drawTitleRow(tr(STR_LIBRARY));
  if (!snapshot) return;
  drawShelf(cell(0, 0, 2, 2), tr(STR_HOME_DISCOVER), icon_home_shuffle_24, snapshot->discover, snapshot->discoverCount,
            discoverOnCard);
  drawShelf(cell(0, 2, 2, 2), tr(STR_HOME_RECENTLY_ADDED), icon_home_clock_24, snapshot->recent, snapshot->recentCount,
            recentOnCard);

  char collections[32];
  snprintf(collections, sizeof(collections), tr(STR_HOME_COLLECTIONS_FORMAT),
           static_cast<unsigned>(std::max<int32_t>(0, snapshot->collections)));
  drawLink(cell(0, 4), tr(STR_HOME_CATALOG), icon_home_books_24, snapshot->totalBooks, tr(STR_HOME_BOOKS), collections);
  drawLink(cell(1, 4), tr(STR_HOME_AUTHORS), icon_home_authors_24, snapshot->authors, nullptr, tr(STR_BROWSE));
  drawLink(cell(0, 5), tr(STR_HOME_SERIES), icon_home_series_24, snapshot->series, nullptr, tr(STR_BROWSE));
  drawCardTile(cell(1, 5));
}

void BookOrbitHomeUi::drawShelf(const Rect& tile, const char* title, const freeink::Icon& icon,
                                const BookOrbitHomeBook* books, const int count, const uint8_t onCardMask) const {
  drawTileHeader(tile, title, &icon);

  for (int i = 0; i < count; i++) {
    const Rect rect = shelfCoverRect(tile, i);
    const Rect slot{rect.x, rect.y, rect.width, rect.height - kShelfCaptionHeight};
    char path[64];
    BookOrbitHomeStore::coverPath(books[i], slot.width, slot.height, path, sizeof(path));
    drawCoverFile(path, slot, false);
    drawFitted(SMALL_FONT_ID, slot.x, slot.y + slot.height + 1, books[i].title, slot.width);
    if ((onCardMask & (1u << i)) != 0) {
      constexpr int markSize = 25;
      const int markX = slot.x + slot.width - markSize - 3;
      const int markY = slot.y + 3;
      renderer.fillRoundedRect(markX, markY, markSize, markSize, markSize / 2, Color::White);
      renderer.drawRoundedRect(markX, markY, markSize, markSize, 1, markSize / 2, true);
      drawLucideIcon(renderer, icon_home_check_24, markX, markY);
    }
  }
}

void BookOrbitHomeUi::drawLink(const Rect& tile, const char* title, const freeink::Icon& icon, const int32_t value,
                               const char* unit, const char* detail) const {
  drawTileHeader(tile, title, &icon);
  if (value >= 0) {
    char number[12];
    snprintf(number, sizeof(number), "%ld", static_cast<long>(value));
    drawBigValue(tile, number, unit);
  }
  drawTileFooter(tile, detail);
}
