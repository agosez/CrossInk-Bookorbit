// A clock that was never set: bookmarks are stamped 0, as upstream always stamps them.
#include "WallClock.h"

bool WallClock::now(uint32_t& outEpochSeconds) {
  outEpochSeconds = 0;
  return false;
}
