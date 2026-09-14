#include "InflateReader.h"

#include <cstring>
void InflateReader::init() {
  memset(&decomp, 0, sizeof(decomp));
  uzlib_uncompress_init(&decomp, nullptr, 0);
}

void InflateReader::setSource(const uint8_t* src, size_t len) {
  decomp.source = src;
  decomp.source_limit = src + len;
}

bool InflateReader::read(uint8_t* dest, size_t len) {
  decomp.dest_start = dest;
  decomp.dest = dest;
  decomp.dest_limit = dest + len;

  const int res = uzlib_uncompress(&decomp);
  if (res < 0) return false;
  return decomp.dest == decomp.dest_limit;
}

bool InflateReader::readExact(uint8_t* dest, size_t len) {
  decomp.dest_start = dest;
  decomp.dest = dest;
  decomp.dest_limit = dest + len + 1;
  const int result = uzlib_uncompress(&decomp);
  return result == TINF_DONE && !decomp.eof && decomp.dest == dest + len && decomp.source == decomp.source_limit;
}
