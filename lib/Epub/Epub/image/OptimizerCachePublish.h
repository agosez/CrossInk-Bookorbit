#pragma once

namespace OptimizerFormat {
// SdFat cannot rename over an existing destination. Keep the previous file
// recoverable until the new name is published; a failed rollback retains backup.
template <typename Filesystem>
bool recoverCache(Filesystem& fs, const char* destination, const char* backup) {
  if (!fs.exists(backup)) return true;
  // Both names means the checked, synced new file was published before cleanup.
  if (fs.exists(destination)) return fs.remove(backup);
  return fs.rename(backup, destination);
}

template <typename Filesystem>
bool publishCache(Filesystem& fs, const char* temporary, const char* destination, const char* backup) {
  if (!recoverCache(fs, destination, backup) || fs.exists(backup)) return false;
  const bool previous = fs.exists(destination);
  if (previous && !fs.rename(destination, backup)) return false;
  if (!fs.rename(temporary, destination)) {
    if (previous) fs.rename(backup, destination);
    return false;
  }
  if (previous) fs.remove(backup);
  return true;
}

template <typename Output, typename Filesystem>
bool finishCache(Output& output, Filesystem& fs, bool valid, const char* temporary, const char* destination,
                 const char* backup) {
  const bool synced = valid && output.sync();
  const bool closed = output.close();
  return synced && closed && publishCache(fs, temporary, destination, backup);
}
}  // namespace OptimizerFormat
