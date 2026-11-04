#pragma once

#include <cstdint>

// Only what ReadingStatsUtils and ReadingDailyMinutes ask of the clock. Starts as a board
// without a clock chip: getDateTime fails while the system clock is set.
class HalClock {
 public:
  bool getDateTime(uint16_t&, uint8_t&, uint8_t&, uint8_t&, uint8_t&) const { return false; }
  bool hasCurrentTime() const { return systemClockSet; }
  bool systemClockSet = true;
};

inline HalClock halClock;
