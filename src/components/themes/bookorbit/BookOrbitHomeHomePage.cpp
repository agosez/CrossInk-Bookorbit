// The Home page: the current book, the reading tiles, the BookOrbit tile and the other
// books being read.
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "BookOrbitHomeModel.h"
#include "BookOrbitHomePages.h"
#include "BookOrbitHomeText.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UiAppHelpers.h"
#include "components/icons/bookOrbitHomeIcons.h"
#include "components/icons/streak.h"
#include "fontIds.h"

namespace BookOrbitHomeHomePage {
namespace {
using Action = BookOrbitHomeStops::Action;
using BookOrbitHomeLayout::alsoReadingTile;
using BookOrbitHomeLayout::bookRowRect;
using BookOrbitHomeLayout::cell;
using BookOrbitHomeLayout::coverSlot;
using BookOrbitHomeLayout::currentBookTile;
using BookOrbitHomeLayout::kBookPad;
using BookOrbitHomeLayout::kBookRowHeaderHeight;
using BookOrbitHomeLayout::kIconSize;
using BookOrbitHomeLayout::kPad;
using BookOrbitHomeLayout::kRingOutset;
using BookOrbitHomeLayout::kTileRadius;
using BookOrbitHomeText::copyInitial;

constexpr int kBookRadius = 12;

void formatPercent(const float percent, char* out, const size_t outSize) {
  snprintf(out, outSize, "%d%%", static_cast<int>(percent + 0.5f));
}

const char* displayTitle(const RecentBook& book) { return book.title.empty() ? book.path.c_str() : book.title.c_str(); }

const char* greeting(const ReadingStatsDateTime& now) {
  if (now.hour >= 5 && now.hour < 12) return tr(STR_HOME_GOOD_MORNING);
  if (now.hour >= 12 && now.hour < 18) return tr(STR_HOME_GOOD_AFTERNOON);
  return tr(STR_HOME_GOOD_EVENING);
}

// The count of what the link opens: the server's books in progress once a sync has
// counted them, the recent books on the card before that or without an account.
void formatAlsoReadingLink(const BookOrbitHomeModel& model, char* out, const size_t outSize) {
  const auto count = model.snapshot && model.snapshot->continueTotal >= 0
                         ? static_cast<unsigned>(model.snapshot->continueTotal)
                         : static_cast<unsigned>(model.data.recentBookCount);
  snprintf(out, outSize, tr(STR_HOME_ALL_FORMAT), count);
}

// Button boards ring the header's link alone, clear of its text so that erasing the ring
// leaves the text whole, and right-aligned with the rows' rings. Touch boards draw no
// ring, so the whole header band takes the tap: an easier target than a line of text.
Rect alsoReadingLinkRect(const BookOrbitHomePageContext& context, const Rect& tile) {
  if (!context.showRing) return Rect{tile.x, tile.y, tile.width, kBookRowHeaderHeight};
  char link[24];
  formatAlsoReadingLink(context.model, link, sizeof(link));
  // The text ends kPad from the tile's edge; the ring keeps 4 px from it on either side,
  // and ends on the header band's last line, just above the first row.
  const int right = tile.x + tile.width - 5;
  const int width = context.renderer.getTextWidth(SMALL_FONT_ID, link) + kPad + 2;
  return Rect{right - width, tile.y + 4, width, kBookRowHeaderHeight - 4};
}

void drawCurrentBook(const BookOrbitHomePageContext& context, const Rect& tile) {
  GfxRenderer& renderer = context.renderer;
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeModel& model = context.model;
  const BookOrbitHomeSource& data = model.data;
  renderer.fillRoundedRect(tile.x, tile.y, tile.width, tile.height, kBookRadius, Color::Black);
  if (data.books == nullptr || data.books->empty()) {
    const char* text = tr(STR_NO_OPEN_BOOK);
    const int width = renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, tile.x + (tile.width - width) / 2, tile.y + tile.height / 2 - 15, text, false,
                      EpdFontFamily::BOLD);
    return;
  }

  const RecentBook& book = data.books->front();
  const Rect slot = coverSlot(context.grid, model.showStats, 0);
  tiles.drawCover(book, slot, true);

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
    tiles.drawFitted(UI_10_FONT_ID, x, y, book.author.c_str(), width, false, false);
    y += 25;
  }
  if (data.chapterTitle != nullptr && !data.chapterTitle->empty()) {
    tiles.drawFitted(SMALL_FONT_ID, x, y, data.chapterTitle->c_str(), width, false, false);
  }

  constexpr int pillHeight = 36;
  const int pillY = tile.y + tile.height - kBookPad - pillHeight;
  if (data.currentBookProgress >= 0.0f) {
    const int barY = pillY - 22;
    char percent[8];
    formatPercent(data.currentBookProgress, percent, sizeof(percent));
    const int percentWidth = renderer.getTextWidth(UI_10_FONT_ID, percent, EpdFontFamily::BOLD);
    tiles.drawProgressBar(x, barY, width - percentWidth - 12, 8, data.currentBookProgress / 100.0f, false);
    renderer.drawText(UI_10_FONT_ID, x + width - percentWidth, barY - 8, percent, false, EpdFontFamily::BOLD);
  }

  const char* resume = tr(STR_HOME_RESUME);
  const int pillWidth = renderer.getTextWidth(UI_10_FONT_ID, resume, EpdFontFamily::BOLD) + 34;
  renderer.fillRoundedRect(x, pillY, pillWidth, pillHeight, pillHeight / 2, Color::White);
  renderer.drawText(UI_10_FONT_ID, x + 17, pillY + 5, resume, true, EpdFontFamily::BOLD);

  const BookOrbitHomeReadingData& reading = model.reading;
  if (reading.hasForecast ||
      (data.currentBookStats != nullptr && data.currentBookStats->estimatedTimeLeftSeconds > 0)) {
    const uint32_t seconds =
        reading.hasForecast ? reading.estimatedSecondsLeft : data.currentBookStats->estimatedTimeLeftSeconds;
    char duration[16];
    formatCompactReadingDuration(seconds, duration, sizeof(duration));
    char left[32];
    snprintf(left, sizeof(left), tr(STR_HOME_LEFT_FORMAT), duration);
    const int leftX = x + pillWidth + 10;
    tiles.drawFitted(SMALL_FONT_ID, leftX, pillY + 8, left, tile.x + tile.width - 14 - leftX, false, false);
  }
}

void drawStreak(const BookOrbitHomePageContext& context, const Rect& tile) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeReadingData& reading = context.model.reading;
  tiles.drawTileHeader(tile, tr(STR_HOME_STREAK));
  context.renderer.drawIcon(StreakIcon, tile.x + tile.width - kPad - kIconSize, tile.y + 7, kIconSize, kIconSize);
  char value[8];
  snprintf(value, sizeof(value), "%u", static_cast<unsigned>(reading.streak));
  tiles.drawBigValue(tile, value, reading.streak == 1 ? tr(STR_HOME_DAY) : tr(STR_HOME_DAYS));

  constexpr int radius = 7;
  const float step = static_cast<float>(tile.width - 2 * kPad - 2 * radius) / 6.0f;
  const int centerY = tile.y + tile.height - 17;
  for (int i = 0; i < 7; i++) {
    const int left = tile.x + kPad + static_cast<int>(static_cast<float>(i) * step + 0.5f);
    tiles.drawDot(left, centerY - radius, radius, (reading.lastSevenDays & (1u << i)) != 0);
  }
}

void drawWeek(const BookOrbitHomePageContext& context, const Rect& tile) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeModel& model = context.model;
  tiles.drawTileHeader(tile, tr(STR_HOME_THIS_WEEK));
  if (model.reading.weekTotal > 0) {
    char total[16];
    formatCompactReadingDuration(model.reading.weekTotal * 60u, total, sizeof(total));
    tiles.drawRightAligned(UI_10_FONT_ID, tile.x + tile.width - kPad, tile.y + 7, total, true);
  }

  // The last seven days, oldest first, so today's initial depends on the date.
  const char* initials = tr(STR_HOME_WEEKDAY_INITIALS);
  char rotated[32] = {};
  size_t used = 0;
  const int todayOfWeek = model.hasToday ? readingStatsDayOfWeekIndex(model.today.date) : 6;
  for (int i = 0; i < 7; i++) {
    char initial[5];
    copyInitial(initials, (todayOfWeek + 1 + i) % 7, initial, sizeof(initial));
    const size_t length = strlen(initial);
    if (used + length >= sizeof(rotated)) break;
    memcpy(rotated + used, initial, length);
    used += length;
  }
  tiles.drawColumnChart(Rect{tile.x + kPad, tile.y + 36, tile.width - 2 * kPad, tile.height - 36 - 4},
                        model.reading.weekMinutes.data(), 7, 6, rotated);
}

void drawGoal(const BookOrbitHomePageContext& context, const Rect& tile) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeModel& model = context.model;
  const BookOrbitHomeSnapshot* snapshot = model.snapshot.get();
  if (snapshot && snapshot->goalBooks > 0) {
    const int32_t goalCompleted = model.reading.goalCompleted;
    char title[32];
    snprintf(title, sizeof(title), tr(STR_HOME_GOAL_FORMAT), static_cast<unsigned>(snapshot->goalYear));
    tiles.drawTileHeader(tile, title, &icon_home_trophy_24);
    char completed[12];
    snprintf(completed, sizeof(completed), "%ld", static_cast<long>(goalCompleted));
    char goal[32];
    snprintf(goal, sizeof(goal), tr(STR_HOME_OF_BOOKS_FORMAT), static_cast<unsigned>(snapshot->goalBooks));
    tiles.drawBigValue(tile, completed, goal);
    if (model.hasToday && model.today.date.year == snapshot->goalYear) {
      ReadingStatsDate newYear;
      newYear.year = model.today.date.year;
      newYear.month = 1;
      newYear.day = 1;
      const uint32_t dayOfYear = readingStatsDayIndex(model.today.date) - readingStatsDayIndex(newYear) + 1;
      const uint32_t daysInYear = isLeapYear(model.today.date.year) ? 366u : 365u;
      char pace[32];
      snprintf(
          pace, sizeof(pace), tr(STR_HOME_ON_PACE_FORMAT),
          static_cast<unsigned>(static_cast<uint32_t>(std::max<int32_t>(0, goalCompleted)) * daysInYear / dayOfYear));
      tiles.drawTileFooter(tile, pace);
    }
    return;
  }
  tiles.drawTileHeader(tile, tr(STR_HOME_FINISHED), &icon_home_trophy_24);
  char value[12];
  snprintf(value, sizeof(value), "%lu",
           static_cast<unsigned long>(model.data.stats ? model.data.stats->completedBooks : 0));
  tiles.drawBigValue(tile, value, tr(STR_HOME_BOOKS));
  tiles.drawTileFooter(tile, tr(STR_HOME_ON_THIS_DEVICE));
}

void drawSyncAge(const BookOrbitHomePageContext& context, const Rect& tile) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeSnapshot* snapshot = context.model.snapshot.get();
  const int y = tile.y + 37;
  const time_t now = time(nullptr);
  if (!snapshot) {
    tiles.drawFitted(UI_10_FONT_ID, tile.x + kPad, y, tr(STR_HOME_NOT_SYNCED), tile.width - 2 * kPad);
    return;
  }
  if (snapshot->generatedAt == 0 || now < static_cast<time_t>(snapshot->generatedAt)) return;
  const uint32_t minutes = static_cast<uint32_t>(now - snapshot->generatedAt) / 60u;
  char age[16];
  if (minutes < 60) {
    snprintf(age, sizeof(age), "%lu min", static_cast<unsigned long>(minutes));
  } else if (minutes < 48u * 60u) {
    BookOrbitHomeText::formatHours(minutes / 60u, age, sizeof(age));
  } else {
    char dayInitial[5];
    copyInitial(tr(STR_HOME_DAYS), 0, dayInitial, sizeof(dayInitial));
    snprintf(age, sizeof(age), "%lu %s", static_cast<unsigned long>(minutes / (24u * 60u)), dayInitial);
  }
  char text[32];
  snprintf(text, sizeof(text), tr(STR_HOME_SYNCED_AGO_FORMAT), age);
  tiles.drawFitted(UI_12_FONT_ID, tile.x + kPad, y - 2, text, tile.width - 2 * kPad, true);
}

void drawAccountTile(const BookOrbitHomePageContext& context, const Rect& tile) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  if (!context.model.hasAccount) {
    tiles.drawTileHeader(tile, tr(STR_LIBRARY), &icon_home_books_24);
    tiles.drawTileFooter(tile, tr(STR_BROWSE));
    return;
  }
  tiles.drawTileHeader(tile, tr(STR_BOOKORBIT), &icon_home_refresh_24);
  drawSyncAge(context, tile);
  tiles.drawTileFooter(tile, tr(STR_HOME_SYNC_NOW));
}

void drawAlsoReading(const BookOrbitHomePageContext& context, const Rect& tile) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeModel& model = context.model;
  tiles.drawTileHeader(tile, tr(STR_HOME_ALSO_READING));
  char link[24];
  formatAlsoReadingLink(model, link, sizeof(link));
  // At the title's height, which centres it in the ring alsoReadingLinkRect() draws.
  tiles.drawRightAligned(SMALL_FONT_ID, tile.x + tile.width - kPad, tile.y + 7, link);

  for (int i = 0; i < model.rowCount; i++) {
    const BookOrbitHomeModel::Row& book = model.rows[static_cast<size_t>(i)];
    const Rect row = bookRowRect(tile, i);
    const Rect slot = coverSlot(context.grid, model.showStats, i + 1);
    const char* title = "";
    if (book.local != nullptr) {
      tiles.drawCover(*book.local, slot, false);
      title = displayTitle(*book.local);
    } else {
      char path[64];
      BookOrbitHomeStore::coverPath(*book.remote, slot.width, slot.height, path, sizeof(path));
      tiles.drawCoverFile(path, slot, false);
      title = book.remote->title;
    }

    const int x = slot.x + slot.width + 12;
    const int width = tile.x + tile.width - kPad - x;
    tiles.drawFitted(UI_10_FONT_ID, x, row.y + 4, title, width, true);
    if (!book.onCard) {
      drawLucideIcon(context.renderer, icon_home_cloud_down_24, tile.x + tile.width - kPad - 66, row.y + 28);
    }
    if (book.progress >= 0.0f) {
      char percent[8];
      formatPercent(book.progress, percent, sizeof(percent));
      tiles.drawProgressBar(x, row.y + 36, width - 76, 7, book.progress / 100.0f);
      tiles.drawRightAligned(SMALL_FONT_ID, tile.x + tile.width - kPad, row.y + 29, percent);
    }
  }
}
}  // namespace

void addStops(const BookOrbitHomePageContext& context, BookOrbitHomeStops& stops) {
  const BookOrbitHomeModel& model = context.model;
  const auto& grid = context.grid;
  // The current book comes first: it is what Confirm reopens straight away.
  const bool hasBooks = model.data.books != nullptr && !model.data.books->empty();
  stops.add(currentBookTile(grid), kBookRadius, kRingOutset, hasBooks ? Action::OpenBook : Action::OpenLibrary);
  stops.add(model.showStats ? cell(grid, 1, 3) : cell(grid, 0, 2, 2), kTileRadius, kRingOutset,
            model.hasAccount ? Action::Refresh : Action::OpenLibrary);
  const Rect also = alsoReadingTile(grid, model.showStats);
  // The header's "All (N) >" leads to every book in progress, which the rows cannot hold.
  stops.add(alsoReadingLinkRect(context, also), 6, 0,
            model.hasAccount ? Action::OpenContinueReading : Action::OpenLibrary);
  for (int i = 0; i < model.rowCount; i++) {
    const BookOrbitHomeModel::Row& row = model.rows[static_cast<size_t>(i)];
    if (row.local != nullptr) {
      stops.add(bookRowRect(also, i), 6, 0, Action::OpenBook, i + 1);
    } else {
      stops.add(bookRowRect(also, i), 6, 0, Action::OpenCatalogBook, row.remoteArg);
    }
  }
}

void draw(const BookOrbitHomePageContext& context) {
  const BookOrbitHomeModel& model = context.model;
  const auto& grid = context.grid;
  char title[64];
  const char* heading = model.hasToday ? greeting(model.today) : tr(STR_HOME);
  if (model.hasToday && model.snapshot && model.snapshot->displayName[0] != '\0') {
    snprintf(title, sizeof(title), "%s, %s", heading, model.snapshot->displayName);
  } else {
    snprintf(title, sizeof(title), "%s", heading);
  }
  context.tiles.drawTitleRow(title, context.pageCount, context.currentPage);

  drawCurrentBook(context, currentBookTile(grid));
  if (model.showStats) {
    drawStreak(context, cell(grid, 0, 2));
    drawWeek(context, cell(grid, 1, 2));
    drawGoal(context, cell(grid, 0, 3));
    drawAccountTile(context, cell(grid, 1, 3));
  } else {
    drawAccountTile(context, cell(grid, 0, 2, 2));
  }
  drawAlsoReading(context, alsoReadingTile(grid, model.showStats));
}

}  // namespace BookOrbitHomeHomePage
