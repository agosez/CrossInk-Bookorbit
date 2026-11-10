#include "BookOrbitHomeLayout.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

namespace BookOrbitHomeLayout {
namespace {
constexpr int kContentTop = 70;
constexpr int kContentBottomInset = 48;  // the button hints or the touch footer, and a gap
constexpr int kBookRowsPadding = 6;
constexpr int kStatsYearTileHeight = 172;
constexpr int kStatsRows = 4;
}  // namespace

Grid grid(const GfxRenderer& renderer) {
  Grid out;
  out.top = kContentTop + UITheme::getTopStatusBarInset(renderer);
  out.contentWidth = renderer.getScreenWidth() - 2 * kMargin;
  out.columnWidth = (out.contentWidth - kGap) / 2;
  out.height = renderer.getScreenHeight() - kContentBottomInset - out.top;
  out.rowHeight = (out.height - (kRowCount - 1) * kGap) / kRowCount;
  return out;
}

int spanHeight(const Grid& grid, const int rows) { return rows * grid.rowHeight + (rows - 1) * kGap; }

int alsoReadingRows(const bool showStats) { return showStats ? 2 : 3; }

int bookRowHeight(const int tileHeight) {
  return (tileHeight - kBookRowHeaderHeight - kBookRowsPadding) / (kMaxBooks - 1);
}

BookOrbitHomeStore::CoverSizes coverSizes(const GfxRenderer& renderer) {
  const Grid layout = grid(renderer);
  const int count = BookOrbitHomeSnapshot::SHELF_COUNT;

  // Under a shelf's captions, room for the selection ring and 4 px of white above the
  // tile's border. Only the X3's short screen lets the height limit the covers; the
  // other boards keep their size, and so their cached covers.
  constexpr int kShelfBottomReserve = kRingOutset + 4;

  BookOrbitHomeStore::CoverSizes sizes;
  sizes.shelfWidth =
      std::min((spanHeight(layout, 2) - kShelfCoverTop - kShelfCaptionHeight - kShelfBottomReserve) * 2 / 3,
               (layout.contentWidth - 2 * kPad - (count - 1) * kGap) / count);
  sizes.shelfHeight = sizes.shelfWidth * 3 / 2;
  const int rows = alsoReadingRows(SETTINGS.shouldTrackReadingStats());
  sizes.rowHeight = bookRowHeight(spanHeight(layout, rows)) - kBookRowCoverInset;
  sizes.rowWidth = sizes.rowHeight * 2 / 3;
  return sizes;
}

Rect cell(const Grid& grid, const int column, const int row, const int columns, const int rows) {
  const int x = kMargin + column * (grid.columnWidth + kGap);
  const int y = grid.top + row * (grid.rowHeight + kGap);
  const int width = columns == 2 ? grid.contentWidth : grid.columnWidth;
  return Rect{x, y, width, spanHeight(grid, rows)};
}

// The stats page's rows: the year tile takes the height it needs, the rest shares what
// is left, so no tile holds a band of empty space.
Rect statsCell(const Grid& grid, const int column, const int row, const int columns) {
  const int x = kMargin + column * (grid.columnWidth + kGap);
  const int width = columns == 2 ? grid.contentWidth : grid.columnWidth;
  if (row == 0) return Rect{x, grid.top, width, kStatsYearTileHeight};
  const int rowHeight = (grid.height - kStatsYearTileHeight - kStatsRows * kGap) / kStatsRows;
  return Rect{x, grid.top + kStatsYearTileHeight + kGap + (row - 1) * (rowHeight + kGap), width, rowHeight};
}

Rect currentBookTile(const Grid& grid) { return cell(grid, 0, 0, 2, 2); }

Rect alsoReadingTile(const Grid& grid, const bool showStats) {
  const int rows = alsoReadingRows(showStats);
  return cell(grid, 0, kRowCount - rows, 2, rows);
}

Rect bookRowRect(const Rect& tile, const int rowIndex) {
  const int rowHeight = bookRowHeight(tile.height);
  return Rect{tile.x + 5, tile.y + kBookRowHeaderHeight + rowIndex * rowHeight, tile.width - 10, rowHeight};
}

// The covers on a shelf, and the room under them for a caption. The ring needs the
// title's last pixels to end above it, and the neighbouring cover to start clear of it.
Rect shelfCoverRect(const GfxRenderer& renderer, const Rect& tile, const int index) {
  const auto sizes = coverSizes(renderer);
  const int count = BookOrbitHomeSnapshot::SHELF_COUNT;
  const int gap = (tile.width - 2 * kPad - count * sizes.shelfWidth) / (count - 1);
  return Rect{tile.x + kPad + index * (sizes.shelfWidth + gap), tile.y + kShelfCoverTop, sizes.shelfWidth,
              sizes.shelfHeight + kShelfCaptionHeight};
}

Rect coverSlot(const Grid& grid, const bool showStats, const int bookIndex) {
  if (bookIndex == 0) {
    const Rect tile = currentBookTile(grid);
    const int height = tile.height - 2 * kBookPad;
    return Rect{tile.x + kBookPad, tile.y + kBookPad, height * 2 / 3, height};
  }
  const Rect row = bookRowRect(alsoReadingTile(grid, showStats), bookIndex - 1);
  const int height = row.height - kBookRowCoverInset;
  return Rect{row.x + 7, row.y + 5, height * 2 / 3, height};
}

}  // namespace BookOrbitHomeLayout
