#pragma once

#include <cstddef>
#include <cstdint>

class HalFile;

// Flash a firmware image from an SD-card path into the next OTA app
// partition, then switch otadata so the X3/X4 stock bootloader picks it up
// on next boot. Mirrors the web flasher: raw esp_partition_erase_range +
// esp_partition_write + ota_boot::switchTo (no Arduino Update class, no
// esp_image_verify — those reject our patched image on X4 silicon).
//
// Both the SD update activity and the OTA path land here. OTA first
// downloads the firmware to an SD-card cache file, then calls this.

namespace firmware_flash {

enum class Result {
  OK,
  OPEN_FAIL,
  TOO_SMALL,
  TOO_LARGE,
  BAD_MAGIC,
  BAD_SEGMENTS,  // segment table malformed or runs past EOF
  BAD_CHECKSUM,  // ESP image XOR checksum mismatch
  BAD_SHA,       // SHA256 trailer mismatch (hash_appended images)
  BAD_CHIP,      // image chip_id doesn't match the running MCU family
  WRONG_BOARD,   // image carries a board tag naming a different board
  BAD_SIZE,      // body+pad+sha length doesn't match file size
  NO_PARTITION,
  OOM,
  READ_FAIL,
  ERASE_FAIL,
  WRITE_FAIL,
  OTADATA_FAIL,
};

// Progress callback: called after every chunk write. `written`/`total` are bytes.
using ProgressCb = void (*)(size_t written, size_t total, void* ctx);

// Open `sdPath`, validate it looks like an ESP32 image, then stream it into the
// next OTA app partition with interleaved 64 KiB erase + sector writes. On
// success switches otadata via ota_boot::switchTo. Caller is responsible for
// ESP.restart() afterwards.
//
// This pathname-based entry point opens one file object, validates it, then
// flashes that same open file. Use flashValidatedFile only when the caller
// already holds a just-validated HalFile through flashing.
Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx);

// Full-image integrity check that mirrors the bootloader's verification:
// header magic, segment table walk, XOR checksum, and SHA256 trailer (when
// hash_appended == 1). Also scans for the embedded board tag and rejects an
// image tagged for a different board. Run this before flashing a candidate
// firmware so a truncated/corrupted/wrong-board .bin never reaches otadata.
//
// `partitionSize` is the size of the destination OTA partition; pass 0 to
// skip the size-fits-partition check (e.g. when validating ahead of partition
// lookup). Streams the file in CHUNK-sized reads; the file is rewound on
// success so the caller can immediately reread it for flashing.
Result validateImageFile(const char* sdPath, size_t partitionSize);

// Validate an already-open image and rewind it on success. The caller keeps
// ownership of `file`, allowing the OTA path to validate and flash the same
// file object without a pathname re-open window.
Result validateOpenImageFile(HalFile& file, size_t partitionSize);

// Flash a file that validateOpenImageFile() has just accepted. This function
// never reopens `file`; the caller must keep it open until this returns.
Result flashValidatedFile(HalFile& file, ProgressCb onProgress, void* ctx);

const char* resultName(Result r);

// Returns the chip_id at byte 12 of the running app image, or 0xFFFF when it
// cannot be read. The running image booted successfully, so its ID is the
// authoritative compatibility value for candidate firmware.
uint16_t runningPartitionChipId();

}  // namespace firmware_flash
