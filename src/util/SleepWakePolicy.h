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

}  // namespace SleepWakePolicy
