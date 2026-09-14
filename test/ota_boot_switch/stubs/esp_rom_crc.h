#pragma once
#include <cstdint>
// ESP's complemented CRC32-LE API has the same seed/result convention as zlib.
// This substitutes the ROM primitive, not the boot selection implementation.
#include <zlib.h>
inline uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t* data, uint32_t length) {
  return static_cast<uint32_t>(crc32(seed, data, length));
}
