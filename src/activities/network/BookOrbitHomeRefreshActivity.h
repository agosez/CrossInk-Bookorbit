#pragma once

#include "activities/Activity.h"

/**
 * The Home tile's sync: connects, replaces the BookOrbit Home snapshot, then restarts to
 * Home. Runs in its own network boot (NetworkBootTarget::BOOKORBIT_HOME_REFRESH), which
 * also lets it decode the fetched covers from a heap Wi-Fi has just given back. Unlike
 * the book sync it needs no open book.
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
  enum class State { Connecting, Refreshing, DecodingCovers, Failed };

  // Runs the scan's offline half, then joins Wi-Fi.
  void connect();
  // Puts the step's message on screen before the blocking work it announces.
  void showStep(State step);
  void runRefresh();

  State state = State::Connecting;
  bool scanPending = false;
  bool refreshPending = false;
  unsigned long failedAt = 0;
};
