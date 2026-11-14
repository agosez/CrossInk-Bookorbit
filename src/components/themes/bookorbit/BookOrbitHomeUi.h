#pragma once

#include <HalDisplay.h>

#include <cstdint>
#include <string>

#include "BookOrbitHomeLayout.h"
#include "BookOrbitHomeModel.h"
#include "BookOrbitHomeStops.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;
class MappedInputManager;
struct BookOrbitHomePageContext;

// The BookOrbit theme's Home: pages of tiles over the firmware's own status bar.
//
// Everything on screen comes from the SD card (BookOrbitHomeModel), so opening Home never
// waits on the network. The stop table (BookOrbitHomeStops) is computed once in begin()
// and stays immutable while the render task reads it, which lets loop() move the
// selection without taking the render lock. Moving the selection only touches the ring
// in the white gutter around a tile (see render()): no redraw, no SD read and no extra
// buffer. Each page lays out and draws its own tiles (BookOrbitHomePages.h).
class BookOrbitHomeUi final {
 public:
  using Action = BookOrbitHomeStops::Action;
  using Page = BookOrbitHomeStops::Page;
  using Stop = BookOrbitHomeStops::Stop;
  using Source = BookOrbitHomeSource;

  static constexpr int MAX_BOOKS = BookOrbitHomeLayout::kMaxBooks;
  static constexpr int TAP_PREVIOUS_PAGE = 100;
  static constexpr int TAP_NEXT_PAGE = 101;

  BookOrbitHomeUi(GfxRenderer& renderer, const MappedInputManager& mappedInput);

  // Reads what the pages show and lays them out. Call again when that data changed;
  // the next render() repaints the page.
  void begin(const Source& source);

  // Paints the current page, or only moves the ring when that is all that changed.
  void render(HalDisplay::RefreshMode refreshMode);

  // Moves through the current page's tiles, wrapping at either end.
  void moveSelection(const int delta) { stops.moveSelection(delta); }
  void movePage(const int delta) { stops.movePage(delta); }
  // Null when the page has no stop to activate.
  const Stop* selectedStop() const { return stops.selectedStop(); }
  const Stop* stopAt(const int index) const { return stops.stopAt(index); }
  // The server book behind an OpenCatalogBook stop.
  const BookOrbitHomeBook* catalogBook(const int arg) const { return model.catalogBook(arg); }

  // The exact slot a book's cover is drawn in. Rescaling a dithered 1-bit image aliases
  // badly, so thumbnails are generated at this size.
  Rect coverSlot(int bookIndex) const;
  // Empty when the book has no cover file to draw.
  static std::string coverPath(const RecentBook& book, int width, int height);

 private:
  BookOrbitHomePageContext pageContext() const;
  void paintPage(Page page);
  void drawTopBar() const;
  void drawNavigation(Page page) const;
  void drawRing(int stopIndex, bool state) const;
  void registerTouchTargets() const;
  uint32_t framebufferHash() const;

  GfxRenderer& renderer;
  const MappedInputManager& mappedInput;
  BookOrbitHomeModel model;
  BookOrbitHomeLayout::Grid layout;
  BookOrbitHomeStops stops;
  bool showRing = false;

  // What the framebuffer holds, owned by the render task.
  bool painted = false;
  bool repaintRequested = true;
  int paintedPage = -1;
  int paintedStop = -1;
  uint32_t paintedHash = 0;
  int pageTurnsSinceHalfRefresh = 0;
};
