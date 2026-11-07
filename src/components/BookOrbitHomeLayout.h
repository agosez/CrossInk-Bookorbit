#pragma once

#include "BookOrbitHomeStore.h"

class GfxRenderer;

// The BookOrbit Home's page grid, apart from its drawing: the syncs that fetch covers
// need the sizes Home draws them at, and nothing else of its UI.
namespace BookOrbitHomeLayout {

constexpr int kMaxBooks = 4;  // the current book and three rows
constexpr int kMargin = 12;
constexpr int kGap = 8;
constexpr int kPad = 12;
constexpr int kRowCount = 6;
constexpr int kBookRowHeaderHeight = 33;
constexpr int kBookRowCoverInset = 10;
constexpr int kShelfCoverTop = 36;
constexpr int kShelfCaptionHeight = 22;
// How far the selection ring is drawn outside what it selects.
constexpr int kRingOutset = 4;

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

}  // namespace BookOrbitHomeLayout
