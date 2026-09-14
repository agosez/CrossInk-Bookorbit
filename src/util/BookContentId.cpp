#include "BookContentId.h"

#include <Epub.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

namespace {

// Guards the single-entry memo: the reader's render task asks for the hash while minting
// highlight positions, while the main task warmed it during onEnter's clipping load.
SemaphoreHandle_t memoLock() {
  static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
  return lock;
}

std::string memoPath;
std::string memoHash;
size_t memoFileSize = 0;       // guards the memo against a file replaced in place (see below)
std::string migratedStateDir;  // state dir whose legacy migration already ran this boot

// The memo key is the path, but a web-portal or USB transfer can replace the file at that
// path with a different book without a reboot. The size is one cheap open to check and
// catches that; a same-size replacement still slips through until the next boot.
size_t fileSizeOf(const std::string& filePath) {
  FsFile file;
  if (!Storage.openFileForRead("BCID", filePath, file)) {
    return 0;
  }
  const size_t size = file.fileSize();
  file.close();
  return size;
}

// Move any bookorbit_* files left in the book's path-keyed cache directory into the
// content-keyed state directory. A file already present at the destination wins: it is
// the one current code has been writing to.
void migrateLegacyStateFiles(const std::string& filePath, const std::string& stateDir) {
  const std::string cacheDir = Epub::cachePathForFilePath(filePath, "/.crosspoint");
  if (cacheDir.empty() || !Storage.exists(cacheDir.c_str())) {
    return;
  }
  for (const auto& name : Storage.listFiles(cacheDir.c_str())) {
    if (std::strncmp(name.c_str(), "bookorbit_", 10) != 0) {
      continue;
    }
    const std::string src = cacheDir + "/" + name.c_str();
    const std::string dst = stateDir + "/" + name.c_str();
    if (Storage.exists(dst.c_str())) {
      continue;
    }
    if (Storage.rename(src.c_str(), dst.c_str())) {
      LOG_INF("BCID", "Migrated %s to %s", src.c_str(), stateDir.c_str());
    } else {
      LOG_ERR("BCID", "Failed to migrate %s; it stays path-keyed", src.c_str());
    }
  }
}

}  // namespace

std::string BookContentId::contentHash(const std::string& filePath) {
  if (filePath.empty()) {
    return "";
  }

  SemaphoreHandle_t lock = memoLock();
  if (lock && xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE) {
    std::string cached;
    size_t cachedSize = 0;
    if (memoPath == filePath && !memoHash.empty()) {
      cached = memoHash;
      cachedSize = memoFileSize;
    }
    xSemaphoreGive(lock);
    if (!cached.empty() && fileSizeOf(filePath) == cachedSize) {
      return cached;
    }
  }

  // Computed outside the lock: ~11 reads of 1 KB. Two tasks racing here just compute the
  // same value twice; the memo only ever holds a correct (path, hash, size) triple.
  const size_t size = fileSizeOf(filePath);
  std::string hash = KOReaderDocumentId::calculate(filePath);
  if (hash.empty()) {
    LOG_ERR("BCID", "Cannot hash book content: %s", filePath.c_str());
    return "";
  }

  if (lock && xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE) {
    memoPath = filePath;
    memoHash = hash;
    memoFileSize = size;
    xSemaphoreGive(lock);
  }
  return hash;
}

std::string BookContentId::bookStateDirName(const std::string& filePath) {
  const std::string hash = contentHash(filePath);
  if (hash.empty()) {
    return "";
  }
  return "/.crosspoint/book_" + hash;
}

std::string BookContentId::bookStateDir(const std::string& filePath) {
  const std::string stateDir = bookStateDirName(filePath);
  if (stateDir.empty()) {
    return "";
  }

  if (!Storage.exists(stateDir.c_str()) && !Storage.mkdir(stateDir.c_str())) {
    LOG_ERR("BCID", "Cannot create book state dir: %s", stateDir.c_str());
    return "";
  }

  bool runMigration = false;
  SemaphoreHandle_t lock = memoLock();
  if (lock && xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE) {
    if (migratedStateDir != stateDir) {
      migratedStateDir = stateDir;
      runMigration = true;
    }
    xSemaphoreGive(lock);
  }
  if (runMigration) {
    migrateLegacyStateFiles(filePath, stateDir);
  }
  return stateDir;
}
