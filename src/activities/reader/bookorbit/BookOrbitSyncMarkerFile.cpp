#include "BookOrbitSyncMarkerFile.h"

#include <HalStorage.h>
#include <Logging.h>

namespace BookOrbitSyncMarkerFile {
namespace {
constexpr char SYNC_MARKER_FILE[] = "/bookorbit_sync.bin";
}  // namespace

BookOrbitSmartSync::Marker read(const std::string& bookStateDir) {
  if (bookStateDir.empty()) return {};  // book not hashable: no history is the honest answer
  const std::string path = bookStateDir + SYNC_MARKER_FILE;
  FsFile file;
  if (!Storage.openFileForRead("BookOrbit", path.c_str(), file)) return {};
  uint8_t raw[BookOrbitSmartSync::MARKER_SIZE];
  const int length = file.read(raw, sizeof(raw));
  file.close();
  if (length <= 0) return {};
  return BookOrbitSmartSync::decodeMarker(raw, static_cast<size_t>(length));
}

void write(const std::string& bookStateDir, const int64_t timestamp, const BookOrbitSmartSync::Position& position) {
  if (timestamp <= 0) return;        // nothing learned; keep whatever history exists
  if (bookStateDir.empty()) return;  // book not hashable: nowhere safe to write
  const std::string path = bookStateDir + SYNC_MARKER_FILE;
  FsFile file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("BookOrbit", "Could not write sync marker %s", path.c_str());
    return;
  }
  uint8_t raw[BookOrbitSmartSync::MARKER_SIZE];
  BookOrbitSmartSync::encodeMarker(BookOrbitSmartSync::makeMarker(timestamp, position), raw);
  const bool ok = file.write(raw, sizeof(raw)) == sizeof(raw);
  file.close();
  if (!ok) {
    // What a short write leaves behind could still read as a date without its
    // position; removing it degrades to "unknown" instead.
    LOG_ERR("BookOrbit", "Short write on sync marker %s", path.c_str());
    Storage.remove(path.c_str());
  }
}

}  // namespace BookOrbitSyncMarkerFile
