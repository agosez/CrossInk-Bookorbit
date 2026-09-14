#pragma once

#include <uzlib.h>

#include <cstddef>
#include <cstdint>

// Small one-shot deflate decompressor wrapping uzlib.
//
// Retained only for FontDecompressor's tiny flash-resident group
// decompressions, where uzlib's ~1KB state beats tinfl's ~11KB on the
// OOM-sensitive render path. All throughput paths (zip entries, PNG IDAT) use
// InflateStream (lib/miniz), which decodes several times faster.
class InflateReader {
 public:
  InflateReader() = default;
  InflateReader(const InflateReader&) = delete;
  InflateReader& operator=(const InflateReader&) = delete;

  // Initialize one-shot mode. The destination buffer holds the entire output,
  // so back-references resolve inside it without a separate 32KB dictionary.
  void init();

  // Requires len + 1 bytes of destination capacity to reject extra output.
  bool readExact(uint8_t* dest, size_t len);

  // Set the entire compressed input as a contiguous memory buffer.
  // Used before the single read() call.
  void setSource(const uint8_t* src, size_t len);

  // Decompress exactly len bytes into dest.
  // Returns false if the stream ends before producing len bytes, or on error.
  bool read(uint8_t* dest, size_t len);

 private:
  uzlib_uncomp decomp = {};
};
