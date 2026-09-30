#pragma once

#include <cstddef>
#include <cstdint>

namespace SleepWakePolicy {

enum class Resume : uint8_t {
  Splash,
  Silent,
  Network,
  SplashlessWake,
};

constexpr bool hasValidSavedFrame(const bool exists, const size_t actualSize, const size_t expectedSize) {
  return exists && actualSize == expectedSize;
}

// A UC8279 X3 can skip its initial resync only after a saved Quick Resume frame
// has been verified. Other resume paths retain their existing behavior.
constexpr bool shouldInitializeSeamlessly(const Resume resume, const bool isUc8279X3, const bool hasValidFrame) {
  return resume != Resume::Splash && !(resume == Resume::SplashlessWake && isUc8279X3 && !hasValidFrame);
}

// Recognizes a wake press while the device only looks asleep: the BookOrbit sleep sync
// runs behind the sleep screen, and the press that would have woken the device must
// cancel it instead. Fed with power-button samples; latches once the press has been
// held for holdMs. heldAtStart means the press that put the device to sleep is still
// down, so nothing counts until it has been released.
class WakePressDetector {
 public:
  constexpr WakePressDetector(const unsigned long holdMs, const bool heldAtStart)
      : holdMs(holdMs), armed(!heldAtStart) {}

  // Returns true from the sample that completes the hold onwards.
  constexpr bool update(const bool pressed, const unsigned long nowMs) {
    if (latched) return true;
    if (!pressed) {
      armed = true;
      pressing = false;
      return false;
    }
    if (!armed) return false;
    if (!pressing) {
      pressing = true;
      pressStartMs = nowMs;
    }
    latched = nowMs - pressStartMs >= holdMs;
    return latched;
  }

 private:
  unsigned long holdMs;
  unsigned long pressStartMs = 0;
  bool armed;
  bool pressing = false;
  bool latched = false;
};

}  // namespace SleepWakePolicy
