// The Stats page: the year's heatmap, when the current book should be finished, when
// and on which days reading happens, the records, and the server's highlight of the day.
// Only shown while reading stats are tracked.
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

#include "BookOrbitHomeModel.h"
#include "BookOrbitHomePages.h"
#include "BookOrbitHomeText.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UiAppHelpers.h"
#include "components/icons/afternoon.h"
#include "components/icons/bookOrbitHomeIcons.h"
#include "components/icons/evening.h"
#include "components/icons/morning.h"
#include "components/icons/night.h"
#include "fontIds.h"

namespace BookOrbitHomeStatsPage {
namespace {
using Action = BookOrbitHomeStops::Action;
using BookOrbitHomeLayout::kIconSize;
using BookOrbitHomeLayout::kPad;
using BookOrbitHomeLayout::kRingOutset;
using BookOrbitHomeLayout::kTileRadius;
using BookOrbitHomeLayout::statsCell;
using BookOrbitHomeText::copyInitial;
using BookOrbitHomeText::copyToken;
using BookOrbitHomeText::formatHours;

constexpr int kYearCellPitch = 8;
constexpr int kYearCellSize = 7;

void drawYear(const BookOrbitHomePageContext& context, const Rect& tile) {
  GfxRenderer& renderer = context.renderer;
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeReadingData& reading = context.model.reading;
  constexpr int YEAR_WEEKS = BookOrbitHomeReadingData::YEAR_WEEKS;
  tiles.drawTileHeader(tile, tr(STR_HOME_YEAR_IN_READING));

  const int gridX = tile.x + tile.width - kPad - YEAR_WEEKS * kYearCellPitch;
  const int gridY = tile.y + 50;

  // Month names every other month: every month would run into its neighbour.
  const char* months = tr(STR_HOME_MONTHS_SHORT);
  int previousMonth = 0;
  int monthsSeen = 0;
  for (int week = 0; week < YEAR_WEEKS && reading.yearCellCount > 0; week++) {
    ReadingStatsDate monday;
    if (!readingStatsDateFromDayIndex(reading.yearFirstDay + static_cast<uint32_t>(week) * 7u, monday)) continue;
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

  for (uint16_t cellIndex = 0; cellIndex < reading.yearCellCount; cellIndex++) {
    const int x = gridX + (cellIndex / 7) * kYearCellPitch;
    const int y = gridY + (cellIndex % 7) * kYearCellPitch;
    switch ((reading.yearLevels[cellIndex / 4] >> ((cellIndex % 4) * 2)) & 0x3) {
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
  formatHours(reading.yearMinutes / 60u, hours, sizeof(hours));
  char best[16];
  formatHours(reading.yearBestMonthMinutes / 60u, best, sizeof(best));
  char daysRead[8];
  snprintf(daysRead, sizeof(daysRead), "%u", static_cast<unsigned>(reading.yearDaysRead));
  const std::array<std::pair<const char*, const char*>, 3> columns{
      {{daysRead, tr(STR_HOME_DAYS_READ)}, {hours, tr(STR_HOME_OVER_A_YEAR)}, {best, tr(STR_HOME_BEST_MONTH)}}};
  const int statsY = gridY + 7 * kYearCellPitch + 6;
  const int columnWidth = (tile.width - 2 * kPad) / 3;
  int x = tile.x + kPad;
  for (const auto& [value, label] : columns) {
    renderer.drawText(UI_12_FONT_ID, x, statsY, value, true, EpdFontFamily::BOLD);
    tiles.drawFitted(SMALL_FONT_ID, x, statsY + 28, label, columnWidth - 6);
    x += columnWidth;
  }
}

void drawForecast(const BookOrbitHomePageContext& context, const Rect& tile) {
  GfxRenderer& renderer = context.renderer;
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeReadingData& reading = context.model.reading;
  tiles.drawTileHeader(tile, tr(STR_HOME_FINISH_BY), &icon_home_calendar_24);
  if (!reading.hasForecast) {
    renderer.drawText(UI_12_FONT_ID, tile.x + kPad, tile.y + 37, "-", true, EpdFontFamily::BOLD);
    return;
  }
  char month[8];
  copyToken(tr(STR_HOME_MONTHS_SHORT), reading.finishDate.month - 1, month, sizeof(month));
  char date[24];
  snprintf(date, sizeof(date), "%u %s", static_cast<unsigned>(reading.finishDate.day), month);
  renderer.drawText(UI_12_FONT_ID, tile.x + kPad, tile.y + 37, date, true, EpdFontFamily::BOLD);
  char duration[16];
  formatCompactReadingDuration(reading.estimatedSecondsLeft, duration, sizeof(duration));
  char pace[48];
  snprintf(pace, sizeof(pace), tr(STR_HOME_PACE_FORMAT), duration, static_cast<unsigned>(reading.dailyPaceMinutes));
  tiles.drawTileFooter(tile, pace);
}

void drawTimeOfDay(const BookOrbitHomePageContext& context, const Rect& tile) {
  GfxRenderer& renderer = context.renderer;
  const BookOrbitHomeTiles& tiles = context.tiles;
  tiles.drawTileHeader(tile, tr(STR_HOME_TIME_OF_DAY));

  const std::array<const uint8_t*, READING_TIME_BUCKET_COUNT> icons{MorningReaderIcon, AfternoonReaderIcon,
                                                                    EveningReaderIcon, NightReaderIcon};
  const auto& seconds = context.model.reading.timeOfDaySeconds;
  const uint32_t highest = std::max<uint32_t>(1, *std::max_element(seconds.begin(), seconds.end()));
  const int columnWidth = (tile.width - 2 * kPad) / static_cast<int>(seconds.size());
  const int base = tile.y + tile.height - kIconSize - 8;
  const int chartHeight = base - (tile.y + 36);
  for (size_t i = 0; i < seconds.size(); i++) {
    const int x = tile.x + kPad + static_cast<int>(i) * columnWidth;
    tiles.drawBar(x + 8, base, columnWidth - 16, chartHeight, seconds[i], highest, seconds[i] == highest);
    renderer.drawIcon(icons[i], x + (columnWidth - kIconSize) / 2, base + 4, kIconSize, kIconSize);
  }
  renderer.drawLine(tile.x + kPad, base, tile.x + tile.width - kPad - 1, base);
}

void drawWeekdays(const BookOrbitHomePageContext& context, const Rect& tile) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  const auto& weekdayMinutes = context.model.reading.weekdayMinutes;
  tiles.drawTileHeader(tile, tr(STR_HOME_WEEKDAYS));
  const auto highest = std::max_element(weekdayMinutes.begin(), weekdayMinutes.end());
  tiles.drawColumnChart(Rect{tile.x + kPad, tile.y + 36, tile.width - 2 * kPad, tile.height - 36 - 4},
                        weekdayMinutes.data(), static_cast<int>(weekdayMinutes.size()),
                        static_cast<int>(highest - weekdayMinutes.begin()), tr(STR_HOME_WEEKDAY_INITIALS));
}

void drawRecords(const BookOrbitHomePageContext& context, const Rect& tile) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeModel& model = context.model;
  if (model.data.stats == nullptr) return tiles.drawTileFrame(tile);
  const GlobalReadingStats& stats = *model.data.stats;
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
  snprintf(best, sizeof(best), "%u", static_cast<unsigned>(model.reading.longestStreak));
  const char* values[] = {time, pages, books, best};
  const char* labels[] = {tr(STR_HOME_TOTAL_TIME), tr(STR_HOME_PAGES), tr(STR_HOME_BOOKS_FINISHED),
                          tr(STR_HOME_BEST_STREAK)};
  tiles.drawMetrics(tile, values, labels, 4);
}

// Where the server's highlight of the day goes when there is none to show.
void drawActivity(const BookOrbitHomePageContext& context, const Rect& tile) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeModel& model = context.model;
  if (model.data.stats == nullptr) return tiles.drawTileFrame(tile);
  const GlobalReadingStats& stats = *model.data.stats;
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
  snprintf(days, sizeof(days), "%u", static_cast<unsigned>(model.reading.yearDaysRead));
  const char* values[] = {sessions, perSession, pace, days};
  const char* labels[] = {tr(STR_HOME_SESSIONS), tr(STR_HOME_PER_SESSION), tr(STR_HOME_PAGES_PER_MIN),
                          tr(STR_HOME_DAYS_READ)};
  tiles.drawMetrics(tile, values, labels, 4);
}

void drawBadge(const BookOrbitHomePageContext& context, const Rect& tile) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeSnapshot& snapshot = *context.model.snapshot;
  tiles.drawTileHeader(tile, tr(STR_HOME_NEXT_BADGE), &icon_home_award_24);
  tiles.drawFitted(UI_10_FONT_ID, tile.x + kPad, tile.y + 38, snapshot.badgeName, tile.width - 2 * kPad, true);
  tiles.drawProgressBar(tile.x + kPad, tile.y + 71, tile.width - 2 * kPad, 8,
                        static_cast<float>(snapshot.badgeProgress) / static_cast<float>(snapshot.badgeThreshold));
  char progress[24];
  snprintf(progress, sizeof(progress), tr(STR_HOME_BADGE_PROGRESS_FORMAT),
           static_cast<unsigned>(snapshot.badgeProgress), static_cast<unsigned>(snapshot.badgeThreshold));
  tiles.drawTileFooter(tile, progress);
}

void drawHighlight(const BookOrbitHomePageContext& context, const Rect& tile) {
  GfxRenderer& renderer = context.renderer;
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeSnapshot& snapshot = *context.model.snapshot;
  tiles.drawTileFrame(tile);
  drawLucideIcon(renderer, icon_home_quote_24, tile.x + kPad - 2, tile.y + 6);
  renderer.drawText(UI_10_FONT_ID, tile.x + kPad + kIconSize + 4, tile.y + 6, tr(STR_HOME_HIGHLIGHT), true,
                    EpdFontFamily::BOLD);
  const bool hasBook = snapshot.highlightBook[0] != '\0';
  const int textWidth = tile.width - 2 * kPad;
  const int lines = (tile.height - 33 - (hasBook ? 26 : 6)) / renderer.getLineHeight(UI_10_FONT_ID);
  int y = tile.y + 33;
  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, snapshot.highlightText, textWidth, lines)) {
    renderer.drawText(UI_10_FONT_ID, tile.x + kPad, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  if (hasBook) {
    tiles.drawTileFooter(tile, snapshot.highlightBook);
  }
}

bool hasBadge(const BookOrbitHomeModel& model) { return model.snapshot && model.snapshot->badgeThreshold > 0; }

bool hasHighlight(const BookOrbitHomeModel& model) {
  return model.snapshot && model.snapshot->highlightText[0] != '\0';
}
}  // namespace

void addStops(const BookOrbitHomePageContext& context, BookOrbitHomeStops& stops) {
  const BookOrbitHomeModel& model = context.model;
  if (!model.showStats) return;
  const auto& grid = context.grid;
  const bool hasBooks = model.data.books != nullptr && !model.data.books->empty();
  stops.add(statsCell(grid, 0, 0, 2), kTileRadius, kRingOutset, Action::OpenStats);
  if (hasBooks) stops.add(statsCell(grid, 0, 1, hasBadge(model) ? 1 : 2), kTileRadius, kRingOutset, Action::OpenBook);
  if (hasHighlight(model)) stops.add(statsCell(grid, 0, 4, 2), kTileRadius, kRingOutset, Action::OpenSavedItems);
}

void draw(const BookOrbitHomePageContext& context) {
  const BookOrbitHomeModel& model = context.model;
  const auto& grid = context.grid;
  context.tiles.drawTitleRow(tr(STR_HOME_STATS), context.pageCount, context.currentPage);
  drawYear(context, statsCell(grid, 0, 0, 2));
  if (hasBadge(model)) {
    drawForecast(context, statsCell(grid, 0, 1));
    drawBadge(context, statsCell(grid, 1, 1));
  } else {
    drawForecast(context, statsCell(grid, 0, 1, 2));
  }
  drawTimeOfDay(context, statsCell(grid, 0, 2));
  drawWeekdays(context, statsCell(grid, 1, 2));
  drawRecords(context, statsCell(grid, 0, 3, 2));
  if (hasHighlight(model)) {
    drawHighlight(context, statsCell(grid, 0, 4, 2));
  } else {
    drawActivity(context, statsCell(grid, 0, 4, 2));
  }
}

}  // namespace BookOrbitHomeStatsPage
