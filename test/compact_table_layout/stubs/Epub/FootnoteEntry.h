#pragma once

#include <cstdint>

struct FootnoteEntry {
  char number[32] = {};
  char href[96] = {};
  uint8_t linkId = 0;
};
