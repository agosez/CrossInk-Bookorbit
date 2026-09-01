#pragma once

#include <cstdint>
#include <string>
#include <utility>

enum class PendingOverlayOrigin : uint8_t { None = 0, Reader, Home };
enum class PendingOverlayType : uint8_t { None = 0, ReaderDrawer, FrontlightDrawer };

struct PendingOverlayResume {
  PendingOverlayOrigin origin = PendingOverlayOrigin::None;
  PendingOverlayType overlay = PendingOverlayType::None;
  uint8_t tab = 0;
  uint8_t pane = 0;
  int16_t selectedIndex = 0;
  int16_t scrollPosition = 0;
  std::string bookPath;
  bool returnHomeAfterReaderFlow = false;
  // A book can override global orientation. Preserve that layout while a
  // reader-originated sync restarts through the network boot flow.
  uint8_t readerOrientation = 0;
  bool preserveReaderOrientation = false;

  bool valid() const { return origin != PendingOverlayOrigin::None && overlay != PendingOverlayType::None; }
  void clear() { *this = PendingOverlayResume{}; }
};

inline bool consumePendingOverlayResumeOnce(PendingOverlayResume& stored, PendingOverlayResume& value) {
  if (!stored.valid()) return false;
  value = std::move(stored);
  stored.clear();
  return true;
}
