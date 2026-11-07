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

}  // namespace BookOrbitHomeLayout
