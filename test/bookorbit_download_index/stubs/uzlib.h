#pragma once

#include <cstdint>

// Any stable checksum will do: the index only compares it with itself.
inline uint32_t uzlib_crc32(const void* data, unsigned int length, uint32_t crc) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (unsigned int i = 0; i < length; i++) crc = (crc ^ bytes[i]) * 16777619u;
  return crc;
}
