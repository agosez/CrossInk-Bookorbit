#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <optional>
#include <string>

#include "util/BookContentId.h"

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
};

inline bool readProgressFile(const char* moduleName, const std::string& path, Progress& progress) {
  if (!Storage.exists(path.c_str())) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead(moduleName, path, f)) {
    return false;
  }

  uint8_t data[10];
  const int dataSize = f.read(data, sizeof(data));
  f.close();
  if (dataSize != 4 && dataSize != 6 && dataSize != 10) {
    LOG_ERR(moduleName, "Progress file has unexpected size: %d", dataSize);
    return false;
  }

  progress.spineIndex = static_cast<int>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
  progress.pageNumber = static_cast<int>(static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8));
  if (progress.pageNumber == UINT16_MAX) {
    progress.pageNumber = 0;
  }
  if (dataSize == 6 || dataSize == 10) {
    progress.pageCount = static_cast<int>(static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8));
    progress.hasPageCount = true;
  } else {
    progress.pageCount = 0;
    progress.hasPageCount = false;
  }
  if (dataSize == 10) {
    progress.visibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                 (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    progress.hasVisibleTextOffset = true;
  }
  return true;
}

// Reads one progress home (main file, then its .bak) into progress.
inline bool readProgressWithBackup(const char* moduleName, const std::string& progressPath, Progress& progress) {
  if (readProgressFile(moduleName, progressPath, progress)) {
    return true;
  }
  if (readProgressFile(moduleName, progressPath + ".bak", progress)) {
    LOG_DBG("ERS", "Recovered progress from backup");
    return true;
  }
  return false;
}

// Whether a sits deeper in the book than b. Spine first; within a chapter the character
// offset when both sides carry one — page numbers are not comparable across paginations —
// and the page fraction otherwise.
inline bool progressIsFurther(const Progress& a, const Progress& b) {
  if (a.spineIndex != b.spineIndex) {
    return a.spineIndex > b.spineIndex;
  }
  if (a.hasVisibleTextOffset && b.hasVisibleTextOffset && a.visibleTextOffset != b.visibleTextOffset) {
    return a.visibleTextOffset > b.visibleTextOffset;
  }
  const float aFraction = a.pageCount > 0 ? static_cast<float>(a.pageNumber) / static_cast<float>(a.pageCount) : 0.0f;
  const float bFraction = b.pageCount > 0 ? static_cast<float>(b.pageNumber) / static_cast<float>(b.pageCount) : 0.0f;
  return aFraction > bFraction;
}

// Progress lives in two homes. The content-keyed copy (book_<hash>/) follows the book
// through moves and renames; the path-keyed copy in the render cache is what stock
// CrossInk reads and writes, so a firmware revert keeps working. When both exist, the
// further position wins: a detour through stock firmware never rewinds the fork's
// position, and vice versa.
inline bool loadProgress(const Epub& epub, Progress& progress, const char* moduleName = "ERS") {
  Progress pathCopy;
  const bool hasPathCopy = readProgressWithBackup(moduleName, epub.getCachePath() + "/progress.bin", pathCopy);

  Progress stateCopy;
  bool hasStateCopy = false;
  // Name-only lookup: reading must not create directories for every book a list renders.
  const std::string stateDir = BookContentId::bookStateDirName(epub.getPath());
  if (!stateDir.empty()) {
    hasStateCopy = readProgressWithBackup(moduleName, stateDir + "/progress.bin", stateCopy);
  }

  if (hasPathCopy && hasStateCopy) {
    progress = progressIsFurther(pathCopy, stateCopy) ? pathCopy : stateCopy;
    return true;
  }
  if (hasStateCopy) {
    progress = stateCopy;
    return true;
  }
  if (hasPathCopy) {
    progress = pathCopy;
    return true;
  }
  return false;
}

// Atomically replaces progressPath with data: write to .tmp, rotate the old file to .bak,
// rename .tmp into place. The .bak survives as the recovery copy readProgressWithBackup uses.
inline bool writeProgressFileAtomic(const std::string& progressPath, const uint8_t* data, const size_t dataSize) {
  const std::string tmpPath = progressPath + ".tmp";
  const std::string backupPath = progressPath + ".bak";

  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR("ERS", "Could not remove stale progress temp file");
    return false;
  }

  FsFile f;
  if (!Storage.openFileForWrite("ERS", tmpPath, f)) {
    LOG_ERR("ERS", "Could not open progress temp file for write!");
    return false;
  }
  const size_t written = f.write(data, dataSize);
  if (written != dataSize) {
    LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", (unsigned)written, (unsigned)dataSize);
    f.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  f.flush();
  if (!f.sync()) {
    LOG_ERR("ERS", "Failed to sync progress temp file");
    f.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!f.close()) {
    LOG_ERR("ERS", "Failed to close progress temp file");
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("ERS", "Could not remove old progress backup");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (Storage.exists(progressPath.c_str()) && !Storage.rename(progressPath.c_str(), backupPath.c_str())) {
    LOG_ERR("ERS", "Could not rotate progress backup");
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), progressPath.c_str())) {
    LOG_ERR("ERS", "Could not replace progress file");
    if (Storage.exists(backupPath.c_str()) && !Storage.exists(progressPath.c_str())) {
      Storage.rename(backupPath.c_str(), progressPath.c_str());
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

// Persists reader progress for an EPUB to both of its homes (see loadProgress). Returns
// true when every attempted write succeeded.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount,
                         const std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  uint8_t data[10];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  size_t dataSize = 6;
  if (visibleTextOffset.has_value()) {
    data[6] = *visibleTextOffset & 0xFF;
    data[7] = (*visibleTextOffset >> 8) & 0xFF;
    data[8] = (*visibleTextOffset >> 16) & 0xFF;
    data[9] = (*visibleTextOffset >> 24) & 0xFF;
    dataSize = sizeof(data);
  }

  // Saves are debounced, so the second ~10-byte write costs little next to what it buys:
  // the content-keyed copy survives moves and renames, the path-keyed copy keeps stock
  // CrossInk fully working after a firmware revert.
  const bool pathOk = writeProgressFileAtomic(epub.getCachePath() + "/progress.bin", data, dataSize);
  bool stateOk = true;
  const std::string stateDir = BookContentId::bookStateDir(epub.getPath());
  if (!stateDir.empty()) {
    stateOk = writeProgressFileAtomic(stateDir + "/progress.bin", data, dataSize);
  }
  return pathOk && stateOk;
}

}  // namespace EpubReaderUtils
