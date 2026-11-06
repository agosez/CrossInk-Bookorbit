#pragma once

#include <atomic>
#include <string>

class OtaUpdater {
  bool updateAvailable = false;
  bool includePrereleases = false;
  std::string latestVersion;
  std::string otaUrl;
  std::string otaSha256;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;

 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    CANCELLED_ERROR,
    HASH_MISMATCH_ERROR,
    WRONG_DEVICE_ERROR,
    SD_CARD_FULL_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  // Progress work covers both staging and flashing so it never moves backwards.
  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  // Lets checkForUpdate() offer the newest release candidate too, when it is newer than both
  // the installed firmware and the latest final release. Inline, so the simulator's own
  // OtaUpdater definitions (crossink-simulator) need nothing new.
  void setIncludePrereleases(const bool include) { includePrereleases = include; }
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr,
                                std::atomic<bool>* cancelRequested = nullptr);
};
