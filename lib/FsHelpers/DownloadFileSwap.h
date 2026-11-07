#pragma once
#include <HalStorage.h>
#include <Logging.h>

#include <string>

// .part and .old are reserved sidecars for one synchronous download per destination.
namespace DownloadFileSwap {
inline bool recover(const std::string& destination) {
  const std::string backup = destination + ".old";
  if (!Storage.exists(backup.c_str())) return true;
  // A committed destination wins. Otherwise restore the last complete book.
  const bool ok = Storage.exists(destination.c_str()) ? Storage.remove(backup.c_str())
                                                      : Storage.rename(backup.c_str(), destination.c_str());
  if (!ok) LOG_ERR("HTTP", "Could not recover download backup: %s", backup.c_str());
  return ok;
}
inline bool publishFrom(const std::string& destination, const std::string& partial, bool requireBackupCleanup = false) {
  if (!recover(destination)) return false;
  const std::string backup = destination + ".old";
  const bool replacing = Storage.exists(destination.c_str());
  if (replacing && !Storage.rename(destination.c_str(), backup.c_str())) {
    LOG_ERR("HTTP", "Could not preserve existing book: %s", destination.c_str());
    return false;
  }
  if (!Storage.rename(partial.c_str(), destination.c_str())) {
    LOG_ERR("HTTP", "Could not publish downloaded book: %s", destination.c_str());
    if (replacing && !Storage.rename(backup.c_str(), destination.c_str())) {
      LOG_ERR("HTTP", "Book preserved in backup: %s", backup.c_str());
    }
    return false;
  }
  // Asset sources require completed cleanup; legacy book/font callers retain
  // their successful-publication behavior. Both keep the valid new destination.
  if (replacing && !Storage.remove(backup.c_str())) {
    LOG_ERR("HTTP", "Could not remove download backup: %s", backup.c_str());
    return !requireBackupCleanup;
  }
  return true;
}
inline bool publish(const std::string& destination, bool requireBackupCleanup = false) {
  return publishFrom(destination, destination + ".part", requireBackupCleanup);
}
}  // namespace DownloadFileSwap
