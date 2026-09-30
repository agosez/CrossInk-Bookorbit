#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#include "SleepWakePolicy.h"

// Watches the power button during the BookOrbit sleep sync, whose network calls block
// the main loop for seconds at a time. A small task samples the button so that a press
// starting and ending inside one request still counts; SleepWakePolicy's
// WakePressDetector decides whether it is a wake press.
class PowerButtonWakeWatcher {
 public:
  PowerButtonWakeWatcher() = default;
  PowerButtonWakeWatcher(const PowerButtonWakeWatcher&) = delete;
  PowerButtonWakeWatcher& operator=(const PowerButtonWakeWatcher&) = delete;
  ~PowerButtonWakeWatcher() { stop(); }

  // Returns false when the sampling task could not be created.
  bool start(unsigned long holdMs, bool heldAtStart);
  void stop();
  // Latched: stays true once a wake press has been seen.
  bool wakeRequested();

 private:
  static void taskEntry(void* context);

  SleepWakePolicy::WakePressDetector detector{0, false};
  std::atomic<bool> requested{false};
  TaskHandle_t taskHandle = nullptr;
};
