#pragma once

#include <cmath>
#include <cstdint>

// Fixed-size one-step recognizer. It is intentionally independent from input,
// fonts, and time so it can be host-tested without reader activity state.
class ReaderPinchGesture {
 public:
  enum class Action : uint8_t { None, Increase, Decrease };

  static constexpr uint8_t MIN_DISTANCE_CHANGE_PX = 20;
  static constexpr uint8_t SCALE_PERCENT = 110;
  // tan(12 degrees) is about 21%, so this keeps the hot touch path on integer
  // math while locking out rotation well before the SDK's 20-degree action.
  static constexpr uint8_t MAX_DIRECTION_CHANGE_TANGENT_PERCENT = 21;

  bool isActive() const { return active_; }

  void reset() {
    active_ = false;
    stepApplied_ = false;
    translationLocked_ = false;
    rotationLocked_ = false;
    pendingAction_ = Action::None;
    startDistanceSq_ = 0;
    startCenterXTwice_ = 0;
    startCenterYTwice_ = 0;
    startDx_ = 0;
    startDy_ = 0;
  }

  // Returns exactly one action per two-contact sequence. Call reset when the
  // touch controller reports fewer than two contacts.
  Action update(const int x1, const int y1, const int x2, const int y2) {
    const uint32_t distanceSq = separationSq(x1, y1, x2, y2);
    if (!active_) {
      active_ = true;
      stepApplied_ = false;
      translationLocked_ = false;
      rotationLocked_ = false;
      pendingAction_ = Action::None;
      startDistanceSq_ = distanceSq;
      startCenterXTwice_ = static_cast<int64_t>(x1) + x2;
      startCenterYTwice_ = static_cast<int64_t>(y1) + y2;
      startDx_ = static_cast<int64_t>(x2) - x1;
      startDy_ = static_cast<int64_t>(y2) - y1;
      return Action::None;
    }
    if (stepApplied_ || translationLocked_ || rotationLocked_) return Action::None;

    const int64_t currentDx = static_cast<int64_t>(x2) - x1;
    const int64_t currentDy = static_cast<int64_t>(y2) - y1;
    if (directionChangedTooFar(startDx_, startDy_, currentDx, currentDy)) {
      // A rotation can introduce a brief separation wobble as the fingers
      // move around one another. Lock it out before that wobble can resize the
      // font; the completed rotation recognizer owns the sequence instead.
      rotationLocked_ = true;
      pendingAction_ = Action::None;
      return Action::None;
    }

    const uint32_t startDistance = distanceFromSq(startDistanceSq_);
    const uint32_t currentDistance = distanceFromSq(distanceSq);
    const uint32_t distanceChange =
        startDistance > currentDistance ? startDistance - currentDistance : currentDistance - startDistance;
    const int64_t centerDeltaX = static_cast<int64_t>(x1) + x2 - startCenterXTwice_;
    const int64_t centerDeltaY = static_cast<int64_t>(y1) + y2 - startCenterYTwice_;
    const uint64_t centerTravelTwiceSq = centerDeltaX * centerDeltaX + centerDeltaY * centerDeltaY;
    const uint64_t distanceChangeSq = static_cast<uint64_t>(distanceChange) * distanceChange;

    // A swipe moves the two-contact midpoint farther than it changes the
    // contact separation. Once that evidence is clear, keep the sequence out
    // of pinch handling until both contacts leave the screen.
    // centerDelta* is twice the midpoint travel, so both comparisons scale the
    // right-hand side by 2^2 rather than halving (and rounding) the deltas.
    if (centerTravelTwiceSq > 4 * distanceChangeSq &&
        centerTravelTwiceSq >= 4 * static_cast<uint64_t>(MIN_DISTANCE_CHANGE_PX) * MIN_DISTANCE_CHANGE_PX) {
      translationLocked_ = true;
      pendingAction_ = Action::None;
      return Action::None;
    }
    if (distanceChange < MIN_DISTANCE_CHANGE_PX) {
      pendingAction_ = Action::None;
      return Action::None;
    }

    constexpr uint64_t scaleSquared = static_cast<uint64_t>(SCALE_PERCENT) * SCALE_PERCENT;
    constexpr uint64_t percentSquared = 100ULL * 100ULL;

    const uint64_t current = distanceSq;
    const uint64_t start = startDistanceSq_;
    Action candidate = Action::None;
    if (current > start && current * percentSquared >= start * scaleSquared) {
      candidate = Action::Increase;
    } else if (current < start && current * scaleSquared <= start * percentSquared) {
      candidate = Action::Decrease;
    }
    if (candidate == Action::None) {
      pendingAction_ = Action::None;
      return Action::None;
    }

    // Require the same result on two consecutive controller frames. Real
    // pinches remain beyond the threshold, while a single noisy span sample
    // during rotation no longer changes the font.
    if (pendingAction_ != candidate) {
      pendingAction_ = candidate;
      return Action::None;
    }
    stepApplied_ = true;
    pendingAction_ = Action::None;
    return candidate;
  }

 private:
  static uint32_t separationSq(const int x1, const int y1, const int x2, const int y2) {
    const int64_t dx = static_cast<int64_t>(x2) - x1;
    const int64_t dy = static_cast<int64_t>(y2) - y1;
    return static_cast<uint32_t>(dx * dx + dy * dy);
  }

  static uint32_t distanceFromSq(const uint32_t distanceSq) {
    return static_cast<uint32_t>(std::sqrt(static_cast<double>(distanceSq)));
  }

  static bool directionChangedTooFar(const int64_t startDx, const int64_t startDy, const int64_t currentDx,
                                     const int64_t currentDy) {
    const int64_t cross = startDx * currentDy - startDy * currentDx;
    const int64_t dot = startDx * currentDx + startDy * currentDy;
    // Contact order can change between controller frames. Taking the absolute
    // dot treats a swapped pair as the same undirected two-finger line.
    const uint64_t absoluteCross = static_cast<uint64_t>(cross < 0 ? -cross : cross);
    const uint64_t absoluteDot = static_cast<uint64_t>(dot < 0 ? -dot : dot);
    if (absoluteCross == 0 && absoluteDot == 0) return false;
    return absoluteCross * 100 >= absoluteDot * MAX_DIRECTION_CHANGE_TANGENT_PERCENT;
  }

  bool active_ = false;
  bool stepApplied_ = false;
  bool translationLocked_ = false;
  bool rotationLocked_ = false;
  Action pendingAction_ = Action::None;
  uint32_t startDistanceSq_ = 0;
  int64_t startCenterXTwice_ = 0;
  int64_t startCenterYTwice_ = 0;
  int64_t startDx_ = 0;
  int64_t startDy_ = 0;
};
