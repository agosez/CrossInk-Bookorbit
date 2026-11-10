// The Library page: the server's shelves, its catalog, authors and series, and what the
// card holds. Only shown once a sync has saved a snapshot.
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "BookOrbitHomeModel.h"
#include "BookOrbitHomePages.h"
#include "components/UiAppHelpers.h"
#include "components/icons/bookOrbitHomeIcons.h"
#include "fontIds.h"

namespace BookOrbitHomeLibraryPage {
namespace {
using Action = BookOrbitHomeStops::Action;
using BookOrbitHomeLayout::cell;
using BookOrbitHomeLayout::kPad;
using BookOrbitHomeLayout::kRingOutset;
using BookOrbitHomeLayout::kShelfCaptionHeight;
using BookOrbitHomeLayout::kTileRadius;
using BookOrbitHomeLayout::shelfCoverRect;

// Decimal gigabytes, as card capacities are sold: one decimal under 10 GB.
void formatGigabytes(const uint64_t bytes, char* out, const size_t outSize) {
  const auto tenths = static_cast<unsigned long>((bytes + 50000000ull) / 100000000ull);
  if (tenths >= 100) {
    snprintf(out, outSize, "%lu", (tenths + 5) / 10);
  } else {
    snprintf(out, outSize, "%lu.%lu", tenths / 10, tenths % 10);
  }
}

void drawShelf(const BookOrbitHomePageContext& context, const Rect& tile, const char* title, const freeink::Icon& icon,
               const BookOrbitHomeBook* books, const int count, const uint8_t onCardMask) {
  GfxRenderer& renderer = context.renderer;
  const BookOrbitHomeTiles& tiles = context.tiles;
  tiles.drawTileHeader(tile, title, &icon);

  for (int i = 0; i < count; i++) {
    const Rect rect = shelfCoverRect(renderer, tile, i);
    const Rect slot{rect.x, rect.y, rect.width, rect.height - kShelfCaptionHeight};
    char path[64];
    BookOrbitHomeStore::coverPath(books[i], slot.width, slot.height, path, sizeof(path));
    tiles.drawCoverFile(path, slot, false);
    tiles.drawFitted(SMALL_FONT_ID, slot.x, slot.y + slot.height + 1, books[i].title, slot.width);
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

void drawLink(const BookOrbitHomePageContext& context, const Rect& tile, const char* title, const freeink::Icon& icon,
              const int32_t value, const char* unit, const char* detail) {
  const BookOrbitHomeTiles& tiles = context.tiles;
  tiles.drawTileHeader(tile, title, &icon);
  if (value >= 0) {
    char number[12];
    snprintf(number, sizeof(number), "%ld", static_cast<long>(value));
    tiles.drawBigValue(tile, number, unit);
  }
  tiles.drawTileFooter(tile, detail);
}

// The Library's books and the ones the catalog downloaded, over a bar of how full the
// card is with its used and total space beside it.
void drawCardTile(const BookOrbitHomePageContext& context, const Rect& tile) {
  GfxRenderer& renderer = context.renderer;
  const BookOrbitHomeTiles& tiles = context.tiles;
  const BookOrbitHomeModel& model = context.model;
  tiles.drawTileHeader(tile, tr(STR_HOME_ON_CARD), &icon_home_card_24);
  char books[12] = "-";
  if (model.cardBookCount >= 0) snprintf(books, sizeof(books), "%ld", static_cast<long>(model.cardBookCount));
  char server[12];
  snprintf(server, sizeof(server), "%ld", static_cast<long>(model.serverBookCount));
  const char* values[] = {books, server};
  const char* labels[] = {tr(STR_HOME_BOOKS), tr(STR_BOOKORBIT)};
  const int valueY = tile.y + 30;
  tiles.drawMetricColumns(tile, valueY, valueY, valueY + 44, values, labels, 2);
  if (model.cardTotalBytes == 0) return;

  char used[8];
  char total[8];
  formatGigabytes(model.cardUsedBytes, used, sizeof(used));
  formatGigabytes(model.cardTotalBytes, total, sizeof(total));
  char space[24];
  snprintf(space, sizeof(space), tr(STR_HOME_STORAGE_FORMAT), used, total);
  constexpr int kBarHeight = 10;
  const int barY = tile.y + tile.height - kPad - kBarHeight;
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, space);
  const int barWidth = tile.width - 2 * kPad - textWidth - 8;
  tiles.drawProgressBar(
      tile.x + kPad, barY, barWidth, kBarHeight,
      static_cast<float>(static_cast<double>(model.cardUsedBytes) / static_cast<double>(model.cardTotalBytes)));
  renderer.drawText(SMALL_FONT_ID, tile.x + tile.width - kPad - textWidth,
                    barY + (kBarHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2, space);
}
}  // namespace

void addStops(const BookOrbitHomePageContext& context, BookOrbitHomeStops& stops) {
  const BookOrbitHomeSnapshot* snapshot = context.model.snapshot.get();
  if (!snapshot) return;
  const auto& grid = context.grid;
  const Rect discover = cell(grid, 0, 0, 2, 2);
  for (int i = 0; i < snapshot->discoverCount; i++) {
    stops.add(shelfCoverRect(context.renderer, discover, i), 2, kRingOutset, Action::OpenCatalogBook,
              BookOrbitHomeModel::kDiscoverArgBase + i);
  }
  const Rect recent = cell(grid, 0, 2, 2, 2);
  for (int i = 0; i < snapshot->recentCount; i++) {
    stops.add(shelfCoverRect(context.renderer, recent, i), 2, kRingOutset, Action::OpenCatalogBook,
              BookOrbitHomeModel::kRecentArgBase + i);
  }
  stops.add(cell(grid, 0, 4), kTileRadius, kRingOutset, Action::OpenCatalog);
  stops.add(cell(grid, 1, 4), kTileRadius, kRingOutset, Action::OpenAuthors);
  stops.add(cell(grid, 0, 5), kTileRadius, kRingOutset, Action::OpenSeries);
  stops.add(cell(grid, 1, 5), kTileRadius, kRingOutset, Action::OpenLibrary);
}

void draw(const BookOrbitHomePageContext& context) {
  context.tiles.drawTitleRow(tr(STR_LIBRARY), context.pageCount, context.currentPage);
  const BookOrbitHomeModel& model = context.model;
  const BookOrbitHomeSnapshot* snapshot = model.snapshot.get();
  if (!snapshot) return;
  const auto& grid = context.grid;
  drawShelf(context, cell(grid, 0, 0, 2, 2), tr(STR_HOME_DISCOVER), icon_home_shuffle_24, snapshot->discover,
            snapshot->discoverCount, model.discoverOnCard);
  drawShelf(context, cell(grid, 0, 2, 2, 2), tr(STR_HOME_RECENTLY_ADDED), icon_home_clock_24, snapshot->recent,
            snapshot->recentCount, model.recentOnCard);

  char collections[32];
  snprintf(collections, sizeof(collections), tr(STR_HOME_COLLECTIONS_FORMAT),
           static_cast<unsigned>(std::max<int32_t>(0, snapshot->collections)));
  drawLink(context, cell(grid, 0, 4), tr(STR_HOME_CATALOG), icon_home_books_24, snapshot->totalBooks,
           tr(STR_HOME_BOOKS), collections);
  drawLink(context, cell(grid, 1, 4), tr(STR_HOME_AUTHORS), icon_home_authors_24, snapshot->authors, nullptr,
           tr(STR_BROWSE));
  drawLink(context, cell(grid, 0, 5), tr(STR_HOME_SERIES), icon_home_series_24, snapshot->series, nullptr,
           tr(STR_BROWSE));
  drawCardTile(context, cell(grid, 1, 5));
}

}  // namespace BookOrbitHomeLibraryPage
