#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <optional>
#include <string>

#include "ProgressRecord.h"
#include "util/BookContentId.h"
#include "util/InPlaceFileWrite.h"

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
};

inline progress_record::Slot readProgressSlot(const char* moduleName, const std::string& path) {
  if (!Storage.exists(path.c_str())) return {};
  FsFile file;
  if (!Storage.openFileForRead(moduleName, path, file)) return {};
  uint8_t data[progress_record::RECORD_BYTES] = {};
  const size_t size = file.fileSize();
  const size_t wanted = std::min(size, sizeof(data));
  const int n = file.read(data, wanted);
  file.close();
  if (n < 0 || static_cast<size_t>(n) != wanted) return {};
  return progress_record::decode(data, size);
}

inline bool copyProgress(const progress_record::Slot& slot, Progress& progress) {
  if (slot.kind == progress_record::SlotKind::Invalid) return false;
  const auto& f = slot.fields;
  progress = {f.spineIndex,        f.pageNumber == UINT16_MAX ? 0 : f.pageNumber,
              f.pageCount,         f.hasPageCount,
              f.visibleTextOffset, f.hasVisibleTextOffset};
  return true;
}

// Read one explicit file (used when importing a legacy snapshot).
inline bool readProgressFile(const char* moduleName, const std::string& path, Progress& progress) {
  return copyProgress(readProgressSlot(moduleName, path), progress);
}

inline bool loadProgressPath(const std::string& path, Progress& progress, const char* moduleName = "ERS") {
  const auto primary = readProgressSlot(moduleName, path);
  const auto backup = readProgressSlot(moduleName, path + ".bak");
  const auto newest = progress_record::newest(primary, backup);
  return newest != progress_record::NONE &&
         copyProgress(newest == progress_record::PRIMARY ? primary : backup, progress);
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
// position, and vice versa. Each home is a pair of slots (ProgressRecord.h).
inline bool loadProgress(const Epub& epub, Progress& progress, const char* moduleName = "ERS") {
  Progress pathCopy;
  const bool hasPathCopy = loadProgressPath(epub.getCachePath() + "/progress.bin", pathCopy, moduleName);

  Progress stateCopy;
  bool hasStateCopy = false;
  // Name-only lookup: reading must not create directories for every book a list renders.
  const std::string stateDir = BookContentId::bookStateDirName(epub.getPath());
  if (!stateDir.empty()) {
    hasStateCopy = loadProgressPath(stateDir + "/progress.bin", stateCopy, moduleName);
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

// Writes one progress home: the older of its two slots is overwritten in place, and an
// unchanged position is not written at all.
inline bool saveProgressPath(const std::string& path, const progress_record::Fields& fields) {
  const std::string backupPath = path + ".bak";
  const auto primary = readProgressSlot("ERS", path);
  const auto backup = readProgressSlot("ERS", backupPath);
  const auto newest = progress_record::newest(primary, backup);
  if (newest != progress_record::NONE &&
      progress_record::sameFields(newest == progress_record::PRIMARY ? primary.fields : backup.fields, fields))
    return true;
  uint8_t record[progress_record::RECORD_BYTES];
  progress_record::encode(fields, progress_record::nextSeq(primary, backup), record);
  const auto& target = progress_record::target(primary, backup) == progress_record::PRIMARY ? path : backupPath;
  return writeFileInPlace("ERS", target.c_str(), record, sizeof(record));
}

// Persists reader progress for an EPUB to both of its homes (see loadProgress). Returns
// true when every attempted write succeeded.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount,
                         const std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  const progress_record::Fields fields{static_cast<uint16_t>(spineIndex), static_cast<uint16_t>(pageNumber),
                                       static_cast<uint16_t>(pageCount),  true,
                                       visibleTextOffset.value_or(0),     visibleTextOffset.has_value()};
  // Saves are debounced and skip unchanged positions, so the second home costs little
  // next to what it buys: the content-keyed copy survives moves and renames, the
  // path-keyed copy keeps stock CrossInk fully working after a firmware revert.
  const bool pathOk = saveProgressPath(epub.getCachePath() + "/progress.bin", fields);
  bool stateOk = true;
  const std::string stateDir = BookContentId::bookStateDir(epub.getPath());
  if (!stateDir.empty()) {
    stateOk = saveProgressPath(stateDir + "/progress.bin", fields);
  }
  return pathOk && stateOk;
}

}  // namespace EpubReaderUtils
