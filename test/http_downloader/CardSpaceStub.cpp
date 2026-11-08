#include "TestPlatform.h"
#include "util/CardSpace.h"

// The real check opens a directory on the card; the tests choose the format instead.
namespace CardSpace {
bool isQuickToCount() { return cardQuickToCount; }
}  // namespace CardSpace
