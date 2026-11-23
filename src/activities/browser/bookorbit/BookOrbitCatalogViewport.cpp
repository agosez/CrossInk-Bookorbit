#include "BookOrbitCatalogViewport.h"

#include <algorithm>

void BookOrbitCatalogViewport::beginFrame(const int top, const int selection) {
  pass = 0;
  passAgain = false;
  fromTop = top;
  fromSelection = selection;
}

freeink::ui::ListNav* BookOrbitCatalogViewport::measureNav() {
  measure.reset();
  return &measure;
}

bool BookOrbitCatalogViewport::endPass(const Pass& drawn, const bool sections) {
  const int top = drawn.top;
  nextTop = top;
  if (sections) {
    const int rows = measure.drawnRows;
    const int relativeTop = top - drawn.windowStart;
    const int windowEnd = drawn.windowStart + drawn.rowCount;
    int wanted = top;
    if (rows > 0 && drawn.selected >= relativeTop && drawn.selected >= relativeTop + rows) {
      // The selection fell below what fitted: down just far enough for it.
      wanted = top + drawn.selected - (relativeTop + rows) + 1;
    } else if (rows > 0 && drawn.listingEnds && relativeTop + rows < drawn.rowCount &&
               top >= windowEnd - drawn.visibleRows) {
      // The listing ends on this screen, but its last rows did not fit under the headings.
      wanted = top + drawn.rowCount - (relativeTop + rows);
      if (drawn.selected >= relativeTop) wanted = std::min(wanted, drawn.windowStart + drawn.selected);
    }
    if (wanted > top && pass + 1 < MAX_PASSES) {
      nextTop = wanted;
      passAgain = true;
      return true;
    }
  }
  // What the loop may take for its next move (adoptedTop, screenRows).
  renderedTop.store(top);
  renderedRows.store(sections ? measure.drawnRows : 0);
  renderedFromTop.store(fromTop);
  renderedFromSelection.store(fromSelection);
  renderedListing.store(listingGeneration, std::memory_order_release);
  return false;
}

bool BookOrbitCatalogViewport::takeAnotherPass() {
  if (!passAgain) return false;
  passAgain = false;
  pass++;
  return true;
}

int BookOrbitCatalogViewport::adoptedTop(const int top, const int selection, const int windowStart,
                                         const int windowEnd) const {
  if (renderedListing.load(std::memory_order_acquire) != listingGeneration) return top;
  if (renderedFromTop.load() != top || renderedFromSelection.load() != selection) return top;
  const int rendered = renderedTop.load();
  return rendered >= windowStart && rendered < windowEnd ? rendered : top;
}

int BookOrbitCatalogViewport::screenRows(const int top, const int visibleRows) const {
  const int rows = renderedRows.load();
  if (renderedListing.load(std::memory_order_acquire) != listingGeneration || renderedTop.load() != top || rows <= 0) {
    return visibleRows;
  }
  return std::min(rows, visibleRows);
}
