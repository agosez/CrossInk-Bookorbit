#pragma once

#include "BookOrbitHomeLayout.h"
#include "BookOrbitHomeStops.h"
#include "BookOrbitHomeTiles.h"

class BookOrbitHomeModel;
class GfxRenderer;

// What a page lays out and draws from, built for one call.
struct BookOrbitHomePageContext {
  GfxRenderer& renderer;
  BookOrbitHomeTiles tiles;
  const BookOrbitHomeModel& model;
  const BookOrbitHomeLayout::Grid& grid;
  bool showRing;  // button boards ring the selected stop; touch boards draw none
  int pageCount;
  int currentPage;
};

// Each page adds its stops in the order the buttons walk them, and draws the tiles
// under them. Both read the same rects, so each page keeps them in one file.

namespace BookOrbitHomeHomePage {
void addStops(const BookOrbitHomePageContext& context, BookOrbitHomeStops& stops);
void draw(const BookOrbitHomePageContext& context);
}  // namespace BookOrbitHomeHomePage

namespace BookOrbitHomeLibraryPage {
void addStops(const BookOrbitHomePageContext& context, BookOrbitHomeStops& stops);
void draw(const BookOrbitHomePageContext& context);
}  // namespace BookOrbitHomeLibraryPage

namespace BookOrbitHomeStatsPage {
void addStops(const BookOrbitHomePageContext& context, BookOrbitHomeStops& stops);
void draw(const BookOrbitHomePageContext& context);
}  // namespace BookOrbitHomeStatsPage
