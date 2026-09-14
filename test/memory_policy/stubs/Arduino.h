#pragma once
#include <esp_heap_caps.h>

#include <cstdint>
struct EspHostStub {
  void restart() const { std::abort(); }
  uint32_t getFreeHeap() const { return fakeheap::internal.free; }
  uint32_t getMaxAllocHeap() const { return fakeheap::internal.largest; }
};
inline EspHostStub ESP;
inline uint32_t micros() { return 0; }
inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}
