#include "PxcV2.h"

#include <algorithm>
using namespace OptimizerFormat;
PxcV2::PxcV2(PxcV2Workspace& workspace, Print& output, const Record& r, uint16_t w, uint16_t h)
    : ws(workspace), out(output), record(r), targetW(w), targetH(h) {
  failed = !valid(r) || r.format != 2 || !dimensions(w, h);
}
bool PxcV2::header() {
  const uint32_t raw = uint32_t((record.width + 3) / 4) * record.height;
  if (memcmp(meta, "PXC2", 4) || meta[4] != 2 || meta[5] != 1 || u16(meta + 6) != 32 || u16(meta + 8) != record.width ||
      u16(meta + 10) != record.height || u16(meta + 12) != (record.width + 3) / 4 || u16(meta + 14) != 2048 ||
      u16(meta + 16) != (raw + 2047) / 2048 || u16(meta + 18) || u32(meta + 20) != raw ||
      u32(meta + 24) != record.pixelCrc || u32(meta + 28) != record.bytes)
    return false;
  uint8_t dest[4];
  put16(dest, targetW);
  put16(dest + 2, targetH);
  return out.write(dest, 4) == 4;
}
bool PxcV2::block() {
  const uint32_t total = uint32_t((record.width + 3) / 4) * record.height;
  codec = meta[0];
  rawLength = u16(meta + 2);
  encodedLength = u16(meta + 4);
  blockCrc = u32(meta + 8);
  return rawTotal < total && codec <= 2 && !meta[1] && rawLength == std::min<uint32_t>(2048, total - rawTotal) &&
         encodedLength && encodedLength <= 2048 && u16(meta + 6) == sequence &&
         ((codec == 0 && encodedLength == rawLength) || (codec != 0 && encodedLength < rawLength));
}
bool PxcV2::pixels() {
  if (codec == 0)
    memcpy(ws.decoded, ws.encoded, rawLength);
  else if (codec == 1) {
    ws.inflate.init();
    ws.inflate.setSource(ws.encoded, encodedLength);
    if (!ws.inflate.readExact(ws.decoded, rawLength)) return false;
  } else {
    size_t i = 0, j = 0;
    while (i < encodedLength) {
      const uint8_t c = ws.encoded[i++];
      if (c == 128) return false;
      const size_t n = c < 128 ? c + 1 : 257 - c;
      if (j + n > rawLength) return false;
      if (c < 128) {
        if (i + n > encodedLength) return false;
        memcpy(ws.decoded + j, ws.encoded + i, n);
        i += n;
      } else {
        if (i == encodedLength) return false;
        memset(ws.decoded + j, ws.encoded[i++], n);
      }
      j += n;
    }
    if (j != rawLength) return false;
  }
  if (crc(0, ws.decoded, rawLength) != blockCrc) return false;
  pixelCrc = crc(pixelCrc, ws.decoded, rawLength);
  const uint16_t rowBytes = (record.width + 3) / 4;
  for (size_t i = 0; i < rawLength; ++i) {
    ws.sourceRow[rowUsed++] = ws.decoded[i];
    if (rowUsed != rowBytes) continue;
    while (targetY < targetH && uint32_t(targetY) * record.height / targetH == sourceY) {
      const size_t targetBytes = (targetW + 3) / 4;
      if (targetW == record.width)
        memcpy(ws.targetRow, ws.sourceRow, targetBytes);
      else {
        memset(ws.targetRow, 0, targetBytes);
        for (uint16_t x = 0; x < targetW; ++x) {
          const uint16_t sx = uint32_t(x) * record.width / targetW;
          const uint8_t v = (ws.sourceRow[sx / 4] >> (6 - 2 * (sx % 4))) & 3;
          ws.targetRow[x / 4] |= v << (6 - 2 * (x % 4));
        }
      }
      if (out.write(ws.targetRow, targetBytes) != targetBytes) return false;
      ++targetY;
    }
    ++sourceY;
    rowUsed = 0;
  }
  rawTotal += rawLength;
  ++sequence;
  return true;
}
size_t PxcV2::write(const uint8_t* data, size_t size) {
  if (failed) return 0;
  const size_t original = size;
  while (size) {
    if (fileTotal >= record.bytes) {
      failed = true;
      return 0;
    }
    const size_t n = std::min<size_t>(size, needed - used);
    memcpy((state == 2 ? ws.encoded : meta) + used, data, n);
    used += n;
    data += n;
    size -= n;
    fileTotal += n;
    if (used != needed) continue;
    bool ok;
    if (state == 0) {
      ok = header();
      state = 1;
      needed = 12;
    } else if (state == 1) {
      ok = block();
      state = 2;
      needed = encodedLength;
    } else {
      ok = pixels();
      state = 1;
      needed = 12;
    }
    used = 0;
    if (!ok) {
      failed = true;
      return 0;
    }
  }
  return original;
}
bool PxcV2::finish() const {
  return !failed && state == 1 && !used && fileTotal == record.bytes &&
         rawTotal == uint32_t((record.width + 3) / 4) * record.height && pixelCrc == record.pixelCrc &&
         sourceY == record.height && targetY == targetH && !rowUsed;
}
