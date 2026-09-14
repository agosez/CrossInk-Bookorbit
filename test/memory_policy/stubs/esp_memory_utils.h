#pragma once
#include <esp_heap_caps.h>
inline bool esp_ptr_external_ram(const void* p) {
  const auto i = fakeheap::live.find(p);
  return i != fakeheap::live.end() && i->second.external;
}
