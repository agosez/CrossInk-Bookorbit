#pragma once
#include "OptimizerFormat.h"

namespace OptimizerFormat {
// Cold setup only. Hashes avoid repeatedly seeking the SD card for distinct
// hrefs; collisions still compare full strings. No metadata table is retained.
struct IndexScratch {
  uint8_t bytes[208] = {};
  Record record;
  char previousHref[129] = {};
  uint64_t hashes[256] = {};
};
using IndexReadAt = bool (*)(void*, uint32_t, uint8_t*, size_t);
inline bool validateIndex(IndexReadAt read, void* context, size_t size, uint32_t manifestCrc, uint32_t manifestSize,
                          IndexScratch& scratch, uint16_t& count) {
  if (!read(context, 0, scratch.bytes, 32) || !validHeader(scratch.bytes, manifestCrc, manifestSize, size))
    return false;
  const uint16_t records = u16(scratch.bytes + 10);
  const uint32_t expectedCrc = u32(scratch.bytes + 24);
  uint32_t recordsCrc = 0;
  for (uint16_t i = 0; i < records; ++i) {
    if (!read(context, 32U + 208U * i, scratch.bytes, 208) || !decodeRecord(scratch.bytes, scratch.record))
      return false;
    recordsCrc = crc(recordsCrc, scratch.bytes, 208);
    uint64_t hash = 14695981039346656037ULL;
    for (const char* p = scratch.record.href; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 1099511628211ULL;
    memcpy(scratch.previousHref, scratch.record.href, 129);
    for (uint16_t j = 0; j < i; ++j) {
      if (scratch.hashes[j] == hash && (!read(context, 32U + 208U * j, scratch.bytes, 129) ||
                                        !strcmp(scratch.previousHref, reinterpret_cast<char*>(scratch.bytes))))
        return false;
    }
    scratch.hashes[i] = hash;
  }
  if (recordsCrc != expectedCrc) return false;
  count = records;
  return true;
}
}  // namespace OptimizerFormat
