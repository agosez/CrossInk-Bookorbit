#pragma once

#include <cstdint>

// Small, allocation-free state for the temporary input lock.  Unsigned
// subtraction keeps the timeout correct across millis() wraparound.
class QuickLockState {
 public:
  bool toggle(uint32_t nowMs) {
    locked_ = !locked_;
    lockedAtMs_ = locked_ ? nowMs : 0U;
    return locked_;
  }

  bool isLocked() const { return locked_; }

  bool shouldSleep(uint32_t nowMs, uint32_t timeoutMs) const {
    return locked_ && timeoutMs != 0U && static_cast<uint32_t>(nowMs - lockedAtMs_) >= timeoutMs;
  }

 private:
  bool locked_ = false;
  uint32_t lockedAtMs_ = 0U;
};
