#pragma once

#include <cstdint>

inline uint32_t utf8NextCodepoint(const unsigned char** cursor) {
  const auto* p = *cursor;
  if (*p == '\0') return 0;
  uint32_t cp = 0;
  if ((*p & 0x80) == 0) {
    cp = *p++;
  } else if ((*p & 0xE0) == 0xC0) {
    cp = *p++ & 0x1F;
    cp = (cp << 6) | (*p++ & 0x3F);
  } else if ((*p & 0xF0) == 0xE0) {
    cp = *p++ & 0x0F;
    cp = (cp << 6) | (*p++ & 0x3F);
    cp = (cp << 6) | (*p++ & 0x3F);
  } else {
    cp = *p++ & 0x07;
    cp = (cp << 6) | (*p++ & 0x3F);
    cp = (cp << 6) | (*p++ & 0x3F);
    cp = (cp << 6) | (*p++ & 0x3F);
  }
  *cursor = p;
  return cp;
}
