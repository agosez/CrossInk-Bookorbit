#pragma once

#include <cstddef>

/**
 * What a sync phase tells the sync screen, and the one input it checks.
 *
 * The reading stats and the highlights go out (or come in) one batch per request, so a long
 * backlog takes a minute: the screen draws the phase's status under the title with a
 * "done / total" count and a bar, or it holds a single unchanging line and looks stalled.
 * Implemented by BookOrbitSyncActivity, which owns the screen state and its RenderLock.
 */
class BookOrbitSyncReporter {
 public:
  // Replaces the status message and starts the bar at 0 of total (0 total = no bar).
  virtual void beginPhase(const char* status, size_t total) = 0;
  virtual void reportProgress(size_t done, size_t total) = 0;
  // Clears the bar without a redraw: the next phase's status message must not be drawn
  // over a stale one.
  virtual void endPhase() = 0;
  // A sleep sync was woken. A phase stops between two requests, where every file it
  // touches is consistent, and the activity ends the sync once it returns.
  virtual bool wakeRequested() const = 0;

 protected:
  ~BookOrbitSyncReporter() = default;
};
