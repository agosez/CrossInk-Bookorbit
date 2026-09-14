#pragma once

// Tracks one-shot button-release suppression across an activity transition.
// A suppression remains armed for the release frame that follows the shortcut
// press, then expires automatically. That prevents a child activity that does
// not read that particular button from accidentally consuming a later, real
// user action.
class ReleaseSuppression {
 public:
  struct FrameState {
    bool backHeld = false;
    bool backReleased = false;
    bool confirmHeld = false;
    bool confirmReleased = false;
    bool powerHeld = false;
    bool powerReleased = false;
  };

  void suppressBack() { suppressBackRelease_ = true; }
  void suppressConfirm() { suppressConfirmRelease_ = true; }
  void suppressPower() { suppressPowerRelease_ = true; }
  void suppressPowerConfirm() { suppressPowerConfirmRelease_ = true; }

  bool consumeBackRelease() {
    if (!suppressBackRelease_) return false;
    suppressBackRelease_ = false;
    return true;
  }

  bool consumeConfirmRelease() {
    if (!suppressConfirmRelease_) return false;
    suppressConfirmRelease_ = false;
    // Confirm can be delivered through the Power fallback. Keep both paths in
    // sync when that release has already been claimed by the popup selection.
    suppressPowerConfirmRelease_ = false;
    return true;
  }

  bool consumePowerRelease() {
    if (!suppressPowerRelease_) return false;
    suppressPowerRelease_ = false;
    return true;
  }

  bool consumePowerConfirmRelease() {
    if (!suppressPowerConfirmRelease_) return false;
    suppressPowerConfirmRelease_ = false;
    return true;
  }

  bool isPowerReleaseSuppressed() const { return suppressPowerRelease_; }

  void expireAfterReleaseFrame(const FrameState& state) {
    if (!state.backHeld && !state.backReleased) suppressBackRelease_ = false;

    // Confirm may be a physical Confirm button or Power's Confirm fallback.
    if (!state.confirmHeld && !state.confirmReleased && !state.powerHeld && !state.powerReleased) {
      suppressConfirmRelease_ = false;
    }

    if (!state.powerHeld && !state.powerReleased) {
      suppressPowerRelease_ = false;
      suppressPowerConfirmRelease_ = false;
    }
  }

 private:
  bool suppressBackRelease_ = false;
  bool suppressConfirmRelease_ = false;
  bool suppressPowerRelease_ = false;
  bool suppressPowerConfirmRelease_ = false;
};
