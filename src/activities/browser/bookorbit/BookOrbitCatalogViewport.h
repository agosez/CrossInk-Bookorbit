#pragma once

#include <FreeInkApp.h>

#include <atomic>
#include <cstdint>

/**
 * What the BookOrbit catalog's list screen really drew, handed from the render task to the
 * loop.
 *
 * Section headings take room, so a screen holds fewer rows than visibleRows assumes. The
 * render task measures how many it drew and, when the selection or the listing's end fell
 * below the screen, draws again further down. The loop keeps owning topIndex: it takes the
 * render's correction only for the very top and selection it rendered from (adoptedTop),
 * never mid-walk.
 *
 * Each member says which task calls it. The render task's state is its own; the loop only
 * reads what endPass() published, and changes newListing() under the RenderLock.
 */
class BookOrbitCatalogViewport {
 public:
  // What one pass of the list screen drew from.
  struct Pass {
    int top;           // the first row drawn, counted from the listing's start
    int selected;      // the selection's index in the rows in memory, -1 when not there
    int windowStart;   // the listing row of the first row in memory
    int rowCount;      // rows in memory
    int visibleRows;   // rows a screen holds without headings
    bool listingEnds;  // no page follows the rows in memory
  };

  // Loop, under the RenderLock: a new listing, so what was measured belongs to the last one.
  void newListing() { listingGeneration++; }

  // Render task: a frame starts from the loop's top and selection.
  void beginFrame(int top, int selection);
  // Render task: the row the current pass draws from; firstPassTop on the frame's first pass.
  int passTop(int firstPassTop) const { return pass == 0 ? firstPassTop : nextTop; }
  // Render task: a nav for the list to report how many rows it drew. Handing the list a nav
  // also makes it skip its fixed-height clamp.
  freeink::ui::ListNav* measureNav();
  // Render task: after the list drew from drawn.top. With sections, when the selection or the
  // listing's end fell below the screen, asks for another pass further down and returns true;
  // otherwise publishes what the loop may take for its next move.
  bool endPass(const Pass& drawn, bool sections);
  // Render task: true once after a pass asked for another, which the caller then draws on a
  // blank frame.
  bool takeAnotherPass();

  // Loop: the top the last frame was really drawn from, when it was drawn for this top and
  // selection and that row is in memory ([windowStart, windowEnd)); `top` otherwise.
  int adoptedTop(int top, int selection, int windowStart, int windowEnd) const;
  // Loop: rows a screen holds from `top`: what the last frame drew, when headings made that
  // fewer than visibleRows.
  int screenRows(int top, int visibleRows) const;

 private:
  static constexpr int MAX_PASSES = 4;

  freeink::ui::ListNav measure;  // render task: measures rows drawn, never scrolls
  int pass = 0;                  // render task: 0 for a frame's first layout
  int nextTop = 0;               // render task: the row the next pass is drawn from
  bool passAgain = false;        // render task: the frame needs a pass from nextTop
  int fromTop = 0;               // render task: topIndex and selectorIndex as the
  int fromSelection = 0;         //   frame started, which its correction answers
  uint32_t listingGeneration = 0;
  std::atomic<uint32_t> renderedListing{0};
  std::atomic<int> renderedFromTop{-1};
  std::atomic<int> renderedFromSelection{-1};
  std::atomic<int> renderedTop{0};
  std::atomic<int> renderedRows{0};
};
