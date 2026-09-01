#pragma once

#include <cstddef>
#include <cstdint>

// Board-identity tag embedded in every CrossInk image, plus a streaming
// scanner the firmware update paths use to reject an image built for a
// different board before it can boot and drive another device's pins. Sticky
// and X4 Pro both use ESP32-S3, so the ESP image chip ID cannot distinguish
// them.
//
// The tag is "CROSSPOINT-BOARD-V1:<board>;" stored once in .rodata. Keep the
// established marker for compatibility with the upstream scanner. Images
// without a tag (older releases, forks, or other projects) remain valid; only
// a present tag naming a different board is rejected.

namespace board_tag {

// Full tag: magic prefix + board name + ';'.
extern const char TAG[];

// Board name of the running firmware (pointer into TAG; not null-terminated at
// the name boundary — always pair with boardNameLen()).
const char* boardName();
size_t boardNameLen();

// Incremental scanner: feed every byte of a candidate image in stream order,
// then check mismatch(). State persists across feed() calls, so chunk
// boundaries splitting the tag are handled.
class Scanner {
 public:
  void feed(const uint8_t* data, size_t len);
  // True once a tag naming a different board has been seen. Valid mid-stream:
  // callers may abort a download as soon as this turns true.
  bool mismatch() const { return mismatchFound; }
  // Board name from the offending tag, for logging (empty until mismatch()).
  const char* foundName() const { return mismatchFound ? captured : ""; }

 private:
  static constexpr size_t MAX_NAME = 23;
  char captured[MAX_NAME + 1] = {0};
  size_t nameLen = 0;
  size_t magicMatched = 0;
  bool capturing = false;
  bool mismatchFound = false;
};

}  // namespace board_tag
