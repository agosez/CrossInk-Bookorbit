#pragma once

#include "BookOrbitHomeStore.h"

class GfxRenderer;
struct Rect;

// The BookOrbit Home's page grid and where its tiles and covers sit, apart from its
// drawing: the syncs that fetch covers need the sizes Home draws them at, and nothing
// else of its UI.
namespace BookOrbitHomeLayout {

constexpr int kMaxBooks = 4;  // the current book and three rows
constexpr int kMargin = 12;
constexpr int kGap = 8;
constexpr int kPad = 12;
constexpr int kRowCount = 6;
constexpr int kIconSize = 24;  // the icons in tile headers
constexpr int kBookPad = 13;   // inside the current book's tile
constexpr int kBookRowHeaderHeight = 33;
constexpr int kBookRowCoverInset = 10;
constexpr int kShelfCoverTop = 36;
constexpr int kShelfCaptionHeight = 22;
// How far the selection ring is drawn outside what it selects.
constexpr int kRingOutset = 4;
// The tiles' look, shared with the Home menu's tiles (MinimalTheme::drawButtonMenu).
constexpr int kTileRadius = 10;
constexpr int kRingWidth = 3;
constexpr int kTitleRowTop = 27;  // the page title, under the status bar

struct Grid {
  int top = 0;
  int contentWidth = 0;
  int columnWidth = 0;
  int rowHeight = 0;
  int height = 0;
};

Grid grid(const GfxRenderer& renderer);

// How tall a tile spanning this many rows is.
int spanHeight(const Grid& grid, int rows);

// The grid rows the other books' tile spans: more of them without the reading tiles.
int alsoReadingRows(bool showStats);

// How tall one book row is in a tile this tall.
int bookRowHeight(int tileHeight);

// The sizes covers are drawn at on this screen.
BookOrbitHomeStore::CoverSizes coverSizes(const GfxRenderer& renderer);

// A tile of the Home and Library pages: columns is 1 or 2, rows any span of the grid.
Rect cell(const Grid& grid, int column, int row, int columns = 1, int rows = 1);

// A tile of the Stats page, whose rows are not the grid's (see the definition).
Rect statsCell(const Grid& grid, int column, int row, int columns = 1);

Rect currentBookTile(const Grid& grid);
Rect alsoReadingTile(const Grid& grid, bool showStats);
Rect bookRowRect(const Rect& tile, int rowIndex);

// One cover of a shelf tile, with its caption under it.
Rect shelfCoverRect(const GfxRenderer& renderer, const Rect& tile, int index);

// The exact slot a book's cover is drawn in: 0 is the current book, then the rows.
Rect coverSlot(const Grid& grid, bool showStats, int bookIndex);

}  // namespace BookOrbitHomeLayout
