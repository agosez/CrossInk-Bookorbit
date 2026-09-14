#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

struct ManualPageTurnRequest {
  bool isForward = true;
  const char* source = "unknown";
};

class ManualPageTurnQueue {
 public:
  static constexpr size_t MAX_PENDING_TURNS = 5;

  enum class EnqueueResult : uint8_t {
    Queued,
    Cancelled,
  };

  bool hasPending() const { return count > 0; }

  EnqueueResult enqueue(const ManualPageTurnRequest request) {
    if ((count > 0 && turns[0].isForward != request.isForward) ||
        (hasDispatchedTurn && dispatchedTurn.isForward != request.isForward)) {
      clear();
      return EnqueueResult::Cancelled;
    }
    if (count < MAX_PENDING_TURNS) {
      turns[count++] = request;
    }
    return EnqueueResult::Queued;
  }

  bool takeNext(ManualPageTurnRequest& request) {
    if (count == 0) return false;

    request = turns[0];
    for (size_t i = 1; i < count; ++i) {
      turns[i - 1] = turns[i];
    }
    --count;
    return true;
  }

  void clear() {
    count = 0;
    hasDispatchedTurn = false;
  }

  void markDispatched(const ManualPageTurnRequest request) {
    dispatchedTurn = request;
    hasDispatchedTurn = true;
  }

  void finishDispatched() { hasDispatchedTurn = false; }

  bool hasDispatched() const { return hasDispatchedTurn; }

  size_t size() const { return count; }

 private:
  std::array<ManualPageTurnRequest, MAX_PENDING_TURNS> turns{};
  uint8_t count = 0;
  ManualPageTurnRequest dispatchedTurn{};
  bool hasDispatchedTurn = false;
};
