#include "MinimalTheme.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FreeInkUIGfxRenderer.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "components/TouchRegistry.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/afternoon.h"
#include "components/icons/book24.h"
#include "components/icons/bookOrbitIcons.h"
#include "components/icons/evening.h"
#include "components/icons/morning.h"
#include "components/icons/night.h"
#include "components/icons/streak.h"
#include "components/themes/bookorbit/BookOrbitHomeLayout.h"
#include "fontIds.h"

namespace {
struct MinimalQuote {
  const char* text;
  const char* author;
};

bool tabSlotIndexFromPoint(const Rect rect, const int tabCount, const int x, const int y, int& index) {
  if (tabCount <= 0 || rect.width <= 0 || y < rect.y || y >= rect.y + rect.height || x < rect.x ||
      x >= rect.x + rect.width) {
    return false;
  }

  index = std::min(tabCount - 1, ((x - rect.x) * tabCount) / rect.width);
  return true;
}

constexpr MinimalQuote kQuotes[] = {
    {"\"Nobody can guess how a person’s life or a people’s fate may be changed by one book, or one poem, or even a "
     "single sentence.\"",
     "Ursula K. Le Guin"},
    {"\"I have always imagined that Paradise will be a kind of library.\"", "Jorge Luis Borges"},
    {"\"A reader lives a thousand lives before he dies. The man who never reads lives only one.\"",
     "George R.R. Martin"},
    {"\"So many books, so little time.\"", "Frank Zappa"},
    {"\"If you only read the books that everyone else is reading, you can only think what everyone else is thinking.\"",
     "Haruki Murakami"},
    {"\"Books are mirrors: you only see in them what you already have inside you.\"", "Carlos Ruiz Zafón"},
    {"\"Books are a uniquely portable magic.\"", "Stephen King"}};

constexpr int kCoverCornerRadius = 8;
constexpr int kProgressBarHeight = 6;
constexpr int kButtonCornerRadius = 6;
constexpr int kFileBrowserIconSize = 24;
constexpr int kFileBrowserRowVerticalPadding = 6;
constexpr int kFileBrowserTextGap = 8;
constexpr int kFileBrowserValueMaxWidth = 76;
constexpr int kCoverTopOffset = 0;
constexpr int kProgressBlockGap = 8;
constexpr int kProgressBarGap = 4;
constexpr int kProgressLabelGap = 5;
constexpr int kStatsFooterReaderIconSize = 24;
constexpr int kStatsFooterStreakIconSize = 24;
constexpr int kStatsFooterSideInset = 48;
int homeButtonHintSelection = -1;

// The Home menu: tiles drawn like the BookOrbit Home's, in as many columns as the width
// holds, from where the BookOrbit Home's tiles start down to the button hints. Anchored at
// the top like Home's pages, a tile keeps its place whatever the number of entries.
using BookOrbitHomeLayout::kGap;
using BookOrbitHomeLayout::kMargin;
using BookOrbitHomeLayout::kPad;
using BookOrbitHomeLayout::kRingOutset;
using BookOrbitHomeLayout::kRingWidth;
using BookOrbitHomeLayout::kTileRadius;
using BookOrbitHomeLayout::kTitleRowTop;
constexpr int kMenuMinTileWidth = 200;  // two columns in portrait, three in landscape
constexpr int kMenuIconSize = 32;
constexpr int kMenuIconGap = 8;
constexpr int kMenuLabelLines = 2;
constexpr int kMenuDotRadius = 4;
constexpr int kMenuDotPitch = 15;

struct MenuGrid {
  Rect block;  // a full page of tiles
  int columns = 1;
  int pageRows = 1;
  int tileHeight = 0;

  int pageItems() const { return columns * pageRows; }
};

MenuGrid menuGrid(const GfxRenderer& renderer, const int buttonCount) {
  MenuGrid grid;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !gpio.hasTouch(), false);
  const int top = BookOrbitHomeLayout::grid(renderer).top;
  const int height = std::max(0, safe.y + safe.height - kGap - top);
  const int width = safe.width - 2 * kMargin;

  grid.columns = std::clamp((width + kGap) / (kMenuMinTileWidth + kGap), 1, std::max(1, buttonCount));
  grid.tileHeight =
      2 * kPad + kMenuIconSize + kMenuIconGap + kMenuLabelLines * renderer.getLineHeight(uiScaleSpec().bodyFontId);
  const int rows = (buttonCount + grid.columns - 1) / grid.columns;
  grid.pageRows = std::clamp((height + kGap) / (grid.tileHeight + kGap), 1, std::max(1, rows));

  const int blockHeight = grid.pageRows * grid.tileHeight + (grid.pageRows - 1) * kGap;
  grid.block = Rect{safe.x + kMargin, top, width, blockHeight};
  return grid;
}

// A row with fewer tiles than columns, the last one, spreads them over the whole width.
Rect menuTileRect(const MenuGrid& grid, const int buttonCount, const int index) {
  const int slot = index % grid.pageItems();
  const int column = slot % grid.columns;
  const int inRow = std::min(grid.columns, buttonCount - (index - column));
  const int width = (grid.block.width - (inRow - 1) * kGap) / inRow;
  const int x = grid.block.x + column * (width + kGap);
  const int right = column == inRow - 1 ? grid.block.x + grid.block.width : x + width;
  return Rect{x, grid.block.y + slot / grid.columns * (grid.tileHeight + kGap), right - x, grid.tileHeight};
}

freeink::ui::Rect uiRect(const int x, const int y, const int width, const int height) {
  return freeink::ui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(width),
                           static_cast<int16_t>(height)};
}

// The icon over its label, wrapped on two lines; a tile three times wider than tall, the
// last row's, puts them side by side on one line instead.
void drawMenuTileContent(const GfxRenderer& renderer, freeink::ui::DrawTarget& target, const Rect& tile,
                         const char* label, const freeink::Icon* icon, freeink::ui::TextStyle style) {
  const int iconSpace = icon != nullptr ? kMenuIconSize + kMenuIconGap : 0;
  if (tile.width >= 3 * tile.height) {
    style.maxLines = 1;
    const int available = tile.width - 2 * kPad - iconSpace;
    const int labelWidth = std::min<int>(available, target.measureText(style.font, label, style).width);
    const int x = tile.x + (tile.width - iconSpace - labelWidth) / 2;
    if (icon != nullptr) drawLucideIcon(renderer, *icon, x, tile.y + (tile.height - kMenuIconSize) / 2);
    target.text(uiRect(x + iconSpace, tile.y, labelWidth, tile.height), label, style);
    return;
  }

  const int labelWidth = tile.width - 2 * kPad;
  const int labelHeight = freeink::ui::measureWrappedText(target, label, style, labelWidth).height;
  const int y = tile.y + (tile.height - iconSpace - labelHeight) / 2;
  if (icon != nullptr) drawLucideIcon(renderer, *icon, tile.x + (tile.width - kMenuIconSize) / 2, y);
  target.text(uiRect(tile.x + kPad, y + iconSpace, labelWidth, labelHeight), label, style);
}

bool dominantReaderTypeBucket(const GlobalReadingStats& globalStats, ReadingTimeBucket& bucketOut) {
  const auto& values = globalStats.timeOfDaySeconds;
  const uint32_t totalSeconds = std::accumulate(values.begin(), values.end(), 0u);
  if (totalSeconds == 0) {
    return false;
  }

  const size_t dominantIndex =
      static_cast<size_t>(std::distance(values.begin(), std::max_element(values.begin(), values.end())));
  bucketOut = static_cast<ReadingTimeBucket>(dominantIndex);
  return true;
}

const char* readerTypeLabel(const GlobalReadingStats& globalStats) {
  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(globalStats, bucket)) {
    return tr(STR_STATS_NEW_READER);
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return tr(STR_STATS_MORNING_READER);
    case ReadingTimeBucket::Afternoon:
      return tr(STR_STATS_AFTERNOON_READER);
    case ReadingTimeBucket::Evening:
      return tr(STR_STATS_EVENING_READER);
    case ReadingTimeBucket::Night:
    default:
      return tr(STR_STATS_NIGHT_READER);
  }
}

const uint8_t* readerTypeIcon(const GlobalReadingStats& globalStats) {
  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantReaderTypeBucket(globalStats, bucket)) {
    return Book24Icon;
  }

  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return MorningReaderIcon;
    case ReadingTimeBucket::Afternoon:
      return AfternoonReaderIcon;
    case ReadingTimeBucket::Evening:
      return EveningReaderIcon;
    case ReadingTimeBucket::Night:
    default:
      return NightReaderIcon;
  }
}

void formatStreakStat(const GlobalReadingStats& globalStats, char* buf, const size_t len) {
  if (len == 0) {
    return;
  }

  ReadingStatsDateTime today;
  const uint16_t streak =
      getCurrentLocalReadingStatsDateTime(today) ? globalStats.currentReadingStreak(&today.date) : 0;
  if (streak == 0) {
    snprintf(buf, len, "%s", tr(STR_STATS_NO_STREAK));
    return;
  }

  snprintf(buf, len, tr(STR_STATS_DAY_STREAK_FORMAT), static_cast<unsigned>(streak));
}

Rect coverImageRectForFrame(const Rect& coverRect);

void drawCenteredStatsRow(const GfxRenderer& renderer, const uint8_t* icon, const int iconSize, const char* label,
                          const int regionTop, const int regionBottom, const bool inverted) {
  const int screenWidth = renderer.getScreenWidth();
  const int regionHeight = regionBottom - regionTop;
  if (regionHeight <= 0) {
    return;
  }

  const int labelLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowHeight = std::max(labelLineHeight, iconSize);
  const int topY = regionTop + std::max(0, regionHeight - rowHeight) / 2;
  const int availableWidth = std::max(1, screenWidth - kStatsFooterSideInset * 2);
  const int iconTextGap = 10;
  const std::string text = renderer.truncatedText(UI_10_FONT_ID, label, availableWidth - iconSize - iconTextGap);
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
  const int blockWidth = iconSize + iconTextGap + textWidth;
  const int iconX = (screenWidth - blockWidth) / 2;
  const int iconY = topY + (rowHeight - iconSize) / 2;
  const int textX = iconX + iconSize + iconTextGap;
  const int textY = topY + (rowHeight - labelLineHeight) / 2;

  if (inverted) {
    renderer.drawIcon(icon, iconX, iconY, iconSize, iconSize);
  } else {
    renderer.drawIconInverted(icon, iconX, iconY, iconSize, iconSize);
  }
  renderer.drawText(UI_10_FONT_ID, textX, textY, text.c_str(), inverted);
}

int progressLabelBottomY(const GfxRenderer& renderer, const Rect& coverRect, const float progressPercent) {
  if (progressPercent < 0.0f) {
    return coverRect.y + coverRect.height;
  }

  const int durationY = coverRect.y + coverRect.height + kProgressBlockGap;
  const int barY = durationY + renderer.getLineHeight(UI_10_FONT_ID) + kProgressBarGap;
  const int labelY = barY + kProgressBarHeight + kProgressLabelGap;
  return labelY + renderer.getLineHeight(UI_10_FONT_ID);
}

void drawStatsOverlay(const GfxRenderer& renderer, const GlobalReadingStats& globalStats, const Rect& coverRect,
                      const float progressPercent, const bool inverted) {
  if (!gpio.deviceIsX3()) {
    return;
  }

  char streakBuf[48];
  formatStreakStat(globalStats, streakBuf, sizeof(streakBuf));
  const char* readerLabel = readerTypeLabel(globalStats);

  const int readerRegionTop = 0;
  const int readerRegionBottom = coverImageRectForFrame(coverRect).y;
  drawCenteredStatsRow(renderer, readerTypeIcon(globalStats), kStatsFooterReaderIconSize, readerLabel, readerRegionTop,
                       readerRegionBottom, inverted);

  const int streakRegionTop = progressLabelBottomY(renderer, coverRect, progressPercent);
  const int streakRegionBottom = renderer.getScreenHeight();
  drawCenteredStatsRow(renderer, StreakIcon, kStatsFooterStreakIconSize, streakBuf, streakRegionTop, streakRegionBottom,
                       inverted);
}

Rect coverRectForScreen(const GfxRenderer& renderer, const Rect& rect) {
  const int coverH = MinimalMetrics::values.homeCoverHeight;
  const int coverW = MinimalMetrics::homeCoverWidth;
  const int coverX = (renderer.getScreenWidth() - coverW) / 2;
  const int coverY = rect.y + kCoverTopOffset;
  return Rect{coverX, coverY, coverW, coverH};
}

Rect coverImageRectForFrame(const Rect& coverRect) {
  const int imageW = std::min(coverRect.width, MinimalMetrics::homeCoverImageWidth);
  const int imageH = std::min(coverRect.height, MinimalMetrics::homeCoverImageHeight);
  return Rect{coverRect.x + (coverRect.width - imageW) / 2, coverRect.y + (coverRect.height - imageH) / 2, imageW,
              imageH};
}

std::string coverPathForImageRect(const RecentBook& book, const Rect& imageRect) {
  if (book.coverBmpPath.empty()) {
    return {};
  }

  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint").getAdaptiveThumbBmpPath(imageRect.width, imageRect.height);
  }

  std::string coverBmpPath = UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.width, imageRect.height);
  if (coverBmpPath.empty() || !Storage.exists(coverBmpPath.c_str())) {
    coverBmpPath = UITheme::getCoverThumbPath(book.coverBmpPath, imageRect.height);
  }
  return coverBmpPath;
}

uint8_t selectedQuoteIndex() {
  static bool initialized = false;
  static uint8_t index = 0;
  if (!initialized) {
    index = static_cast<uint8_t>((millis() / 137u) % (sizeof(kQuotes) / sizeof(kQuotes[0])));
    initialized = true;
  }
  return index;
}

int centeredRowY(const int rowY, const int rowHeight, const int contentHeight) {
  return rowY + std::max(0, rowHeight - contentHeight) / 2;
}

void drawProgressBlock(const GfxRenderer& renderer, const Rect& coverRect, const BookReadingStats* stats,
                       float progressPercent, const bool inverted) {
  if ((stats == nullptr || stats->totalReadingSeconds == 0) && progressPercent < 0.0f) {
    return;
  }

  const int barW = coverRect.width;
  const int barX = coverRect.x;
  const int durationY = coverRect.y + coverRect.height + kProgressBlockGap;
  const int barY = durationY + renderer.getLineHeight(UI_10_FONT_ID) + kProgressBarGap;
  const bool textBlack = !inverted;

  if (stats != nullptr && stats->totalReadingSeconds > 0) {
    char duration[32];
    BookReadingStats::formatDuration(stats->totalReadingSeconds, duration, sizeof(duration));
    renderer.drawText(UI_10_FONT_ID, barX, durationY, duration, textBlack);
  }

  if (progressPercent < 0.0f) {
    return;
  }

  const int progress = std::clamp(static_cast<int>(progressPercent + 0.5f), 0, 100);
  const int fillW = (barW * progress) / 100;
  if (inverted) {
    renderer.drawRect(barX, barY, barW, kProgressBarHeight, false);
    if (fillW > 0) {
      renderer.fillRect(barX, barY, fillW, kProgressBarHeight, false);
    }
  } else {
    renderer.fillRectDither(barX, barY, barW, kProgressBarHeight, Color::LightGray);
    if (fillW > 0) {
      renderer.fillRectDither(barX, barY, fillW, kProgressBarHeight, Color::DarkGray);
    }
  }

  char progressLabel[12];
  snprintf(progressLabel, sizeof(progressLabel), "%d%%", progress);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, progressLabel);
  renderer.drawText(UI_10_FONT_ID, barX + barW - labelW, barY + kProgressBarHeight + kProgressLabelGap, progressLabel,
                    textBlack);
}

void drawMissingBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book) {
  constexpr int commonBookCoverHeightRatio = 3;
  constexpr int commonBookCoverWidthRatio = 2;
  const int placeholderHeight =
      std::min(coverRect.height, (coverRect.width * commonBookCoverHeightRatio) / commonBookCoverWidthRatio);
  const Rect placeholderRect{coverRect.x, coverRect.y + (coverRect.height - placeholderHeight) / 2, coverRect.width,
                             placeholderHeight};

  renderer.fillRoundedRect(placeholderRect.x, placeholderRect.y, placeholderRect.width, placeholderRect.height,
                           kCoverCornerRadius, Color::White);
  renderer.drawRoundedRect(placeholderRect.x, placeholderRect.y, placeholderRect.width, placeholderRect.height, 1,
                           kCoverCornerRadius, true);

  const int dividerY = placeholderRect.y + placeholderRect.height / 3;
  renderer.drawLine(placeholderRect.x, dividerY, placeholderRect.x + placeholderRect.width - 1, dividerY, true);

  constexpr int iconSize = 32;
  drawLucideIcon(renderer, icon_book_open_32, placeholderRect.x + (placeholderRect.width - iconSize) / 2,
                 placeholderRect.y + (placeholderRect.height / 3 - iconSize) / 2);

  constexpr int textPadding = 16;
  constexpr int textVerticalPadding = 22;
  constexpr int titleAuthorGap = 28;
  const int textW = placeholderRect.width - textPadding * 2;
  const std::string& titleText = book.title.empty() ? book.path : book.title;
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int authorLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const bool hasAuthor = !book.author.empty();
  auto authorLines =
      hasAuthor ? renderer.wrappedText(UI_10_FONT_ID, book.author.c_str(), textW, 2) : std::vector<std::string>{};
  const int lowerAreaHeight = placeholderRect.y + placeholderRect.height - dividerY;
  const int authorBlockHeight = authorLineHeight * static_cast<int>(authorLines.size());
  const int authorGap = authorLines.empty() ? 0 : titleAuthorGap;
  const int availableTitleHeight = lowerAreaHeight - textVerticalPadding * 2 - authorBlockHeight - authorGap;
  const int maxTitleLines = std::clamp(availableTitleHeight / titleLineHeight, 1, 4);
  auto titleLines = renderer.wrappedText(UI_12_FONT_ID, titleText.c_str(), textW, maxTitleLines);

  const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
  const int totalTextHeight = titleBlockHeight + authorBlockHeight + authorGap;
  int textY = dividerY + std::max(textVerticalPadding, (lowerAreaHeight - totalTextHeight) / 2);

  for (const auto& line : titleLines) {
    const int lineW = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
    renderer.drawText(UI_12_FONT_ID, placeholderRect.x + (placeholderRect.width - lineW) / 2, textY, line.c_str());
    textY += titleLineHeight;
  }

  if (!authorLines.empty()) {
    textY += titleAuthorGap;
    for (const auto& line : authorLines) {
      const int lineW = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
      renderer.drawText(UI_10_FONT_ID, placeholderRect.x + (placeholderRect.width - lineW) / 2, textY, line.c_str());
      textY += authorLineHeight;
    }
  }
}

void drawBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                   const Color backgroundColor) {
  bool hasCover = false;
  if (!book.coverBmpPath.empty()) {
    const Rect imageRect = coverImageRectForFrame(coverRect);
    const std::string coverBmpPath = coverPathForImageRect(book, imageRect);
    if (!coverBmpPath.empty() && Storage.exists(coverBmpPath.c_str())) {
      FsFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          const Rect bitmapRect = fittedBitmapRect(bitmap, imageRect);
          renderer.fillRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, kCoverCornerRadius,
                                   backgroundColor);
          renderer.fillRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, kCoverCornerRadius,
                                   Color::White);
          renderer.drawBitmap(bitmap, bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height);
          renderer.maskRoundedRectOutsideCorners(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height,
                                                 kCoverCornerRadius, backgroundColor);
          renderer.drawRoundedRect(bitmapRect.x, bitmapRect.y, bitmapRect.width, bitmapRect.height, 1,
                                   kCoverCornerRadius, true);
          hasCover = true;
        }
        file.close();
      }
    }
  }

  if (!hasCover) {
    drawMissingBookCover(renderer, coverRect, book);
  }
}
}  // namespace

void MinimalTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                              const bool readerContext, const bool showStatus) const {
  (void)subtitle;
  (void)readerContext;

  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  if (title) {
    constexpr int titleInsetX = 12;
    const int titleRight = rect.x + rect.width - 12;
    const int maxTitleWidth = titleRight - rect.x - titleInsetX - MinimalMetrics::values.contentSidePadding;
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + titleInsetX, rect.y + MinimalMetrics::values.batteryBarHeight + 3,
                      truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }

  if (showStatus) {
    drawDisplayStatusBar(renderer, rect.y);
  }
}

void MinimalTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                              const bool selected) const {
  (void)selected;

  if (tabs.empty()) {
    return;
  }

  const int tabCount = static_cast<int>(tabs.size());
  const int lineY = rect.y + rect.height - 1;
  renderer.drawLine(rect.x, rect.y, rect.x + rect.width - 1, rect.y, true);
  renderer.drawLine(rect.x, lineY, rect.x + rect.width - 1, lineY, true);

  for (int i = 0; i < tabCount; i++) {
    const int slotX = rect.x + (i * rect.width) / tabCount;
    const int nextSlotX = rect.x + ((i + 1) * rect.width) / tabCount;
    const int slotWidth = nextSlotX - slotX;
    const auto& tab = tabs[i];
    TouchRegistry::getInstance().add(Rect{slotX, rect.y, slotWidth, rect.height}, i, TouchRegistry::Tab);
    const auto fontStyle = tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, fontStyle);
    const int textX = slotX + (slotWidth - textWidth) / 2;
    const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, tab.label, true, fontStyle);
  }
}

bool MinimalTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                     const int x, const int y, int& index) const {
  (void)renderer;
  return tabSlotIndexFromPoint(rect, static_cast<int>(tabs.size()), x, y, index);
}

int MinimalTheme::compactFileBrowserRowHeightFor(const GfxRenderer& renderer) {
  const int textHeight = renderer.getLineHeight(UI_10_FONT_ID) * 2 + kFileBrowserRowVerticalPadding;
  return std::max(kFileBrowserIconSize + kFileBrowserRowVerticalPadding, textHeight);
}

int MinimalTheme::compactFileBrowserRowHeight(const GfxRenderer& renderer) const {
  return compactFileBrowserRowHeightFor(renderer);
}

void MinimalTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                            const std::function<std::string(int index)>& rowTitle,
                            const std::function<std::string(int index)>& rowSubtitle,
                            const std::function<UIIcon(int index)>& rowIcon,
                            const std::function<std::string(int index)>& rowValue, bool highlightValue,
                            const std::function<bool(int index)>& rowDimmed,
                            const std::function<bool(int index)>& isHeader, const int rowHeightScale,
                            const bool showSelection, const int totalItemCount) const {
  const bool compactFileRows = rowSubtitle != nullptr && rowIcon != nullptr && rowValue != nullptr;
  if (!compactFileRows) {
    LyraTheme::drawList(renderer, rect, itemCount, selectedIndex, rowTitle, rowSubtitle, rowIcon, rowValue,
                        highlightValue, rowDimmed, isHeader, rowHeightScale, showSelection, totalItemCount);
    return;
  }

  drawCompactFileBrowserList(renderer, rect, itemCount, selectedIndex, rowTitle, rowSubtitle, rowIcon, rowValue,
                             rowDimmed, showSelection);
}

void MinimalTheme::drawCompactFileBrowserList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                              const std::function<std::string(int index)>& rowTitle,
                                              const std::function<std::string(int index)>& rowSubtitle,
                                              const std::function<UIIcon(int index)>& rowIcon,
                                              const std::function<std::string(int index)>& rowValue,
                                              const std::function<bool(int index)>& rowDimmed, const bool showSelection,
                                              const bool uniformRowHeight, const bool allowTwoLineTitles) {
  if (itemCount <= 0) return;

  const int fileRowHeight = compactFileBrowserRowHeightFor(renderer);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int folderRowHeight = MinimalMetrics::values.listRowHeight;
  const auto isFolderRow = [&](int index) { return rowSubtitle(index) == "folder"; };
  const auto rowHeightFor = [&](int index) {
    return !uniformRowHeight && isFolderRow(index) ? folderRowHeight : fileRowHeight;
  };
  const auto pageEndFor = [&](int startIndex) {
    int usedHeight = 0;
    int endIndex = startIndex;
    while (endIndex < itemCount) {
      const int nextRowHeight = rowHeightFor(endIndex);
      if (endIndex > startIndex && usedHeight + nextRowHeight > rect.height) break;
      usedHeight += nextRowHeight;
      endIndex++;
    }
    return std::max(startIndex + 1, endIndex);
  };

  int pageStartIndex = 0;
  int pageEndIndex = pageEndFor(pageStartIndex);
  while (selectedIndex >= pageEndIndex && pageEndIndex < itemCount) {
    pageStartIndex = pageEndIndex;
    pageEndIndex = pageEndFor(pageStartIndex);
  }

  int totalPages = 0;
  int currentPage = 0;
  for (int pageStart = 0; pageStart < itemCount;) {
    if (pageStart == pageStartIndex) currentPage = totalPages;
    totalPages++;
    const int nextPageStart = pageEndFor(pageStart);
    if (nextPageStart <= pageStart) break;
    pageStart = nextPageStart;
  }

  const int contentWidth =
      rect.width -
      (totalPages > 1 ? (MinimalMetrics::values.scrollBarWidth + MinimalMetrics::values.scrollBarRightOffset) : 1);

  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;
    const int scrollBarHeight = std::max(MinimalMetrics::values.scrollBarWidth, scrollAreaHeight / totalPages);
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - MinimalMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - MinimalMetrics::values.scrollBarWidth, scrollBarY,
                      MinimalMetrics::values.scrollBarWidth, scrollBarHeight, true);
  }

  if (showSelection && selectedIndex >= 0) {
    int selectedY = rect.y;
    for (int i = pageStartIndex; i < selectedIndex; i++) {
      selectedY += rowHeightFor(i);
    }
    const int selectedRowHeight = rowHeightFor(selectedIndex);
    renderer.fillRoundedRect(rect.x + MinimalMetrics::values.contentSidePadding, selectedY,
                             contentWidth - MinimalMetrics::values.contentSidePadding * 2, selectedRowHeight, 6,
                             Color::LightGray);
  }

  const int iconX = rect.x + MinimalMetrics::values.contentSidePadding + kFileBrowserTextGap;
  const int textX = iconX + kFileBrowserIconSize + kFileBrowserTextGap;

  int itemY = rect.y;
  for (int i = pageStartIndex; i < itemCount && i < pageEndIndex; i++) {
    const int rowHeight = rowHeightFor(i);
    const bool folderRow = isFolderRow(i);
    const bool selectedRow = showSelection && i == selectedIndex;
    TouchRegistry::getInstance().add(Rect{rect.x, itemY, contentWidth, rowHeight}, i, TouchRegistry::Item);

    std::string valueText = rowValue(i);
    if (!valueText.empty()) {
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), kFileBrowserValueMaxWidth);
    }
    const int valueWidth = valueText.empty() ? 0 : renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
    const int valueGap = valueText.empty() ? 0 : kFileBrowserTextGap;
    const int textWidth =
        std::max(1, contentWidth - textX - MinimalMetrics::values.contentSidePadding - valueWidth - valueGap);

    const freeink::Icon* iconBitmap = iconForName(rowIcon(i), kFileBrowserIconSize);
    if (iconBitmap != nullptr) {
      const int iconY = centeredRowY(itemY, rowHeight, kFileBrowserIconSize);
      drawLucideIcon(renderer, *iconBitmap, iconX, iconY);
    }

    const int maxTitleLines = folderRow || !allowTwoLineTitles ? 1 : 2;
    auto lines = renderer.wrappedText(UI_10_FONT_ID, rowTitle(i).c_str(), textWidth, maxTitleLines);
    const int textBlockHeight = static_cast<int>(lines.size()) * lineHeight;
    int textY = centeredRowY(itemY, rowHeight, textBlockHeight);
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, textX, textY, line.c_str(), true);
      textY += lineHeight;
    }

    if (!valueText.empty()) {
      const int valueY = centeredRowY(itemY, rowHeight, lineHeight);
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - MinimalMetrics::values.contentSidePadding - valueWidth,
                        valueY, valueText.c_str(), true);
    }

    if (rowDimmed && rowDimmed(i) && !selectedRow) {
      const int dimHeight = std::max(lineHeight, textBlockHeight);
      for (int py = itemY; py < itemY + dimHeight; py++) {
        for (int px = textX; px < textX + textWidth; px++) {
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
        }
      }
    }
    itemY += rowHeight;
  }
}

void MinimalTheme::setHomeButtonHintSelection(const int selectedIndex) { homeButtonHintSelection = selectedIndex; }

Rect MinimalTheme::buttonMenuPanelRect(const GfxRenderer& renderer, const int buttonCount) {
  if (buttonCount <= 0) {
    return Rect{0, 0, 0, 0};
  }
  return menuGrid(renderer, buttonCount).block;
}

const freeink::Icon* MinimalTheme::menuIcon(const UIIcon icon) {
  switch (icon) {
    case UIIcon::BookmarkIcon:  // Lyra draws its own ribbon instead
      return &icon_bookmark_32;
    case UIIcon::BookOrbit:
      return &icon_bookorbit_32;
    default:
      return iconForName(icon, kMenuIconSize);
  }
}

void MinimalTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                   const char* btn4, const bool allowInvertedText) const {
  if (gpio.hasTouch()) {
    homeButtonHintSelection = -1;
    return;
  }

  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  const bool invertText = allowInvertedText && origOrientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = MinimalMetrics::values.buttonHintsHeight;
  constexpr int buttonY = MinimalMetrics::values.buttonHintsHeight;
  constexpr int textYOffset = 7;
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = screenWidth > 500 ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const int selectedIndex = homeButtonHintSelection;
  homeButtonHintSelection = -1;

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    const bool hasLabel = labels[i] != nullptr && labels[i][0] != '\0';
    if (hasLabel) {
      TouchRegistry::getInstance().add(Rect{x, pageHeight - buttonY, buttonWidth, buttonHeight}, i,
                                       TouchRegistry::Button);
      const Color background = i == selectedIndex ? Color::LightGray : Color::White;
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, kButtonCornerRadius, background);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, kButtonCornerRadius, true, true,
                               false, false, true);
    } else if (labels[i] != nullptr) {
      // Clear the previous full-sized hint before drawing the inactive marker.
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      const int smallButtonY = pageHeight - smallButtonHeight;
      renderer.fillRoundedRect(x, smallButtonY, buttonWidth, smallButtonHeight, kButtonCornerRadius, Color::White);
      renderer.drawRoundedRect(x, smallButtonY, buttonWidth, smallButtonHeight, 1, kButtonCornerRadius, true, true,
                               false, false, true);
    }
  }

  renderer.setOrientation(invertText ? GfxRenderer::Orientation::PortraitInverted : GfxRenderer::Orientation::Portrait);
  const int textY = invertText ? textYOffset : pageHeight - buttonY + textYOffset;

  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[invertText ? 3 - i : i];
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, textY, labels[i]);
    }
  }

  renderer.setOrientation(origOrientation);
}

void MinimalTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                       int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                       bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                       const BookReadingStats* stats, float progressPercent,
                                       const GlobalReadingStats* globalStats, const char* currentChapterTitle) const {
  (void)selectorIndex;
  (void)bufferRestored;
  (void)globalStats;
  (void)currentChapterTitle;

  const Rect coverRect = coverRectForScreen(renderer, rect);
  if (recentBooks.empty()) {
    renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);

    const MinimalQuote& quote = kQuotes[selectedQuoteIndex()];
    constexpr int quotePadding = 18;
    const int textW = coverRect.width - quotePadding * 2;
    auto lines = renderer.wrappedText(UI_12_FONT_ID, quote.text, textW, 6);
    int lineY = coverRect.y + 88;
    const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    for (const auto& line : lines) {
      renderer.drawText(UI_12_FONT_ID, coverRect.x + quotePadding, lineY, line.c_str());
      lineY += lineH;
    }

    const int authorW = renderer.getTextWidth(UI_10_FONT_ID, quote.author, EpdFontFamily::ITALIC);
    renderer.drawText(UI_10_FONT_ID, coverRect.x + coverRect.width - quotePadding - authorW,
                      coverRect.y + coverRect.height - 110, quote.author, true, EpdFontFamily::ITALIC);
    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  if (!coverRendered) {
    drawBookCover(renderer, coverRect, recentBooks[0], Color::White);
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }
  TouchRegistry::getInstance().add(coverRect, 0, TouchRegistry::Cover);

  drawProgressBlock(renderer, coverRect, stats, progressPercent, false);
}

void MinimalTheme::drawSleepScreen(const GfxRenderer& renderer, const RecentBook& book, const BookReadingStats* stats,
                                   const float progressPercent, const bool inverted) const {
  renderer.clearScreen(inverted ? 0xFF : 0x00);

  const Rect contentRect{0, MinimalMetrics::values.homeTopPadding, renderer.getScreenWidth(),
                         MinimalMetrics::values.homeCoverTileHeight};
  const Rect coverRect = coverRectForScreen(renderer, contentRect);
  drawBookCover(renderer, coverRect, book, inverted ? Color::White : Color::Black);
  drawProgressBlock(renderer, coverRect, stats, progressPercent, !inverted);
}

void MinimalTheme::drawStatsSleepScreen(const GfxRenderer& renderer, const RecentBook& book,
                                        const BookReadingStats* stats, const GlobalReadingStats* globalStats,
                                        const float progressPercent, const bool inverted) const {
  drawSleepScreen(renderer, book, stats, progressPercent, inverted);
  if (globalStats != nullptr) {
    const Rect contentRect{0, MinimalMetrics::values.homeTopPadding, renderer.getScreenWidth(),
                           MinimalMetrics::values.homeCoverTileHeight};
    const Rect coverRect = coverRectForScreen(renderer, contentRect);
    drawStatsOverlay(renderer, *globalStats, coverRect, progressPercent, inverted);
  }
}

void MinimalTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                  const std::function<const char*(int index)>& buttonLabel,
                                  const std::function<UIIcon(int index)>& rowIcon) const {
  // The menu places itself in the screen's free space, the same way buttonMenuPanelRect()
  // reports it to HomeActivity's tap test, so the caller's rect is not used.
  (void)rect;

  if (buttonCount <= 0) {
    return;
  }

  const MenuGrid grid = menuGrid(renderer, buttonCount);
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !gpio.hasTouch(), false);
  const int titleY = kTitleRowTop + UITheme::getTopStatusBarInset(renderer);
  renderer.drawText(UI_12_FONT_ID, safe.x + kMargin, titleY, tr(STR_MENU), true, EpdFontFamily::BOLD);

  const int pageItems = grid.pageItems();
  const int pages = (buttonCount + pageItems - 1) / pageItems;
  const int page = std::clamp(selectedIndex, 0, buttonCount - 1) / pageItems;
  if (pages > 1) {
    const int dotTop = titleY + 17 - kMenuDotRadius;
    const int dotSize = 2 * kMenuDotRadius + 1;
    int left = safe.x + safe.width - kMargin - dotSize;
    for (int i = pages - 1; i >= 0; i--, left -= kMenuDotPitch) {
      if (i == page) {
        renderer.fillRoundedRect(left, dotTop, dotSize, dotSize, kMenuDotRadius, Color::Black);
      } else {
        renderer.drawRoundedRect(left, dotTop, dotSize, dotSize, 1, kMenuDotRadius, true);
      }
    }
  }

  const auto scale = uiScaleSpec();
  freeink::ui::GfxRendererFrame<1> ui(renderer, scale.smallFontId, scale.bodyFontId, scale.titleFontId);
  freeink::ui::TextStyle labelText = uiThemeTokens(ui.target).bodyText;
  labelText.align = freeink::ui::TextAlign::Center;
  labelText.bold = true;
  labelText.maxLines = kMenuLabelLines;

  const int first = page * pageItems;
  for (int i = first; i < std::min(buttonCount, first + pageItems); ++i) {
    const Rect tile = menuTileRect(grid, buttonCount, i);
    TouchRegistry::getInstance().add(tile, i, TouchRegistry::Item);
    renderer.drawRoundedRect(tile.x, tile.y, tile.width, tile.height, 1, kTileRadius, true);
    if (i == selectedIndex) {
      renderer.drawRoundedRect(tile.x - kRingOutset, tile.y - kRingOutset, tile.width + 2 * kRingOutset,
                               tile.height + 2 * kRingOutset, kRingWidth, kTileRadius + kRingOutset, true);
    }
    const char* label = buttonLabel != nullptr ? buttonLabel(i) : nullptr;
    drawMenuTileContent(renderer, ui.target, tile, label != nullptr ? label : "",
                        rowIcon != nullptr ? menuIcon(rowIcon(i)) : nullptr, labelText);
  }
}
