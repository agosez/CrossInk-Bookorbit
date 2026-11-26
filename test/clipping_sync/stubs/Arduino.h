#pragma once
#include <cstdint>

inline unsigned long millis() { return 123; }

// The BookOrbit sync load checks the largest free block (ClippingStore::loadForBookOrbitSync).
struct EspHostStub {
  uint32_t getFreeHeap() const { return UINT32_MAX; }
  uint32_t getMaxAllocHeap() const { return UINT32_MAX; }
};

inline EspHostStub ESP;
