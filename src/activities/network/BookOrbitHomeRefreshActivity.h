#pragma once

#include <string>

#include "BookOrbitDeviceScan.h"
#include "activities/Activity.h"

/**
 * The Home tile's sync: connects, replaces the BookOrbit Home snapshot, then restarts to
 * Home. Runs in its own network boot (NetworkBootTarget::BOOKORBIT_HOME_REFRESH), which
 * also lets it decode the fetched covers from a heap Wi-Fi has just given back. Unlike
 * the book sync it needs no open book.
 *
 * It also runs the card scan (BookOrbitDeviceScan), which can take minutes on a card full
 * of new books: the screen names each step with its count, and ends on what the scan did.
 */
class BookOrbitHomeRefreshActivity final : public Activity {
 public:
  BookOrbitHomeRefreshActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookOrbitHomeRefresh", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state != State::Failed; }

 private:
  enum class State { Connecting, Scanning, Refreshing, DecodingCovers, ScanSummary, Failed };

  // Runs the scan's offline half, then joins Wi-Fi.
  void connect();
  // Puts the step's message on screen before the blocking work it announces.
  void showStep(State step);
  void runRefresh();
  // Repaints the scan's step: at once when the step changes, then at most every few seconds.
  static void onScanProgress(void* context, BookOrbitDeviceScan::Step step, size_t done, size_t total);
  void setScanSummary(const BookOrbitDeviceScan::Summary& summary);

  State state = State::Connecting;
  bool scanPending = false;
  bool refreshPending = false;
  unsigned long failedAt = 0;
  unsigned long summaryAt = 0;
  // The scan's step on screen, and when it was last painted.
  BookOrbitDeviceScan::Step scanStep = BookOrbitDeviceScan::Step::ReadingCard;
  size_t scanDone = 0;
  size_t scanTotal = 0;
  unsigned long scanPaintedAt = 0;
  // What the scan did, shown under the covers step and before the restart; empty when it
  // recognised nothing.
  std::string summaryLines[2];
};
