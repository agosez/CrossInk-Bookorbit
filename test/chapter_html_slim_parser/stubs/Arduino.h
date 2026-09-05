#pragma once

#include <cstdint>

struct EspHostStub {
  uint32_t getFreeHeap() const { return UINT32_MAX; }
  uint32_t getMaxAllocHeap() const { return UINT32_MAX; }
};

inline EspHostStub ESP;
inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}
