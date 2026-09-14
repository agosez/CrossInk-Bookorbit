#pragma once
#include <InflateReader.h>
#include <Print.h>

#include "OptimizerFormat.h"

// One session-owned allocation; too large for the render task stack.
struct PxcV2Workspace {
  InflateReader inflate;
  uint8_t encoded[2048];
  uint8_t decoded[2049];  // Extra byte proves the DEFLATE stream ends exactly.
  uint8_t sourceRow[256];
  uint8_t targetRow[256];
};
static_assert(sizeof(PxcV2Workspace) < 6500, "Keep the decoder allocation below 6.5 KB");

class PxcV2 final : public Print {
 public:
  PxcV2(PxcV2Workspace& workspace, Print& output, const OptimizerFormat::Record& record, uint16_t w, uint16_t h);
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* data, size_t size) override;
  bool finish() const;

 private:
  bool header();
  bool block();
  bool pixels();
  PxcV2Workspace& ws;
  Print& out;
  const OptimizerFormat::Record& record;
  uint16_t targetW, targetH;
  uint8_t meta[32] = {};
  uint16_t used = 0, needed = 32, sequence = 0, rawLength = 0, encodedLength = 0;
  uint16_t rowUsed = 0, sourceY = 0, targetY = 0;
  uint8_t codec = 0, state = 0;
  uint32_t blockCrc = 0, pixelCrc = 0, rawTotal = 0, fileTotal = 0;
  bool failed = false;
};
