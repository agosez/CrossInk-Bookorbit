#include "CardSpace.h"

#include <HalStorage.h>

namespace CardSpace {

bool isQuickToCount() {
#ifdef SIMULATOR
  // The host storage shim does not expose card capacity.
  return false;
#else
  if (Storage.totalBytes() == 0) return false;
  // SdFat keeps the volume's format to itself, but the two formats record a directory's
  // size differently: FAT always stores 0, exFAT the bytes of the clusters it holds, and
  // an exFAT directory holds at least one. The firmware's state directory exists from
  // the first boot.
  HalFile dir = Storage.open("/.crosspoint");
  const bool exFat = dir && dir.isDirectory() && dir.fileSize64() > 0;
  if (dir) dir.close();
  return exFat;
#endif
}

}  // namespace CardSpace
