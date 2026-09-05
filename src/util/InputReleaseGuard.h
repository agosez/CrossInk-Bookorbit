#pragma once

#include "MappedInputManager.h"

namespace InputReleaseGuard {

// Activities opened while a button is held own that button's release. This keeps
// the release from being interpreted as the first action in the new activity.
inline bool consumeInitialRelease(MappedInputManager& input, const MappedInputManager::Button button, bool& pending) {
  if (!pending) return false;
  if (input.wasReleased(button) || !input.isPressed(button)) {
    pending = false;
  }
  return true;
}

}  // namespace InputReleaseGuard
