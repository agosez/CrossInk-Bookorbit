#pragma once

#include <cstdint>

inline uint32_t utf8NextCodepoint(const unsigned char** cursor) {
  const auto* value = *cursor;
  if (*value == '\0') return 0;
  ++value;
  *cursor = value;
  return static_cast<uint32_t>(value[-1]);
}
