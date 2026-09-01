#pragma once

#include <FreeInkApp.h>

#include <algorithm>

#include "MappedInputManager.h"

namespace DrawerHandle {

namespace fui = freeink::ui;

inline int16_t bandHeight(const fui::SheetProps& props) {
  return static_cast<int16_t>(props.grabberMargin + props.grabberHeight + props.grabberInset);
}

inline fui::Rect hitRect(const fui::Rect content, const fui::Rect safe, const fui::SheetProps& props) {
  const int16_t height = bandHeight(props);
  const int16_t y = props.anchor == fui::SheetEdge::Top ? content.bottom() : static_cast<int16_t>(content.y - height);
  const int16_t width = std::min<int16_t>(
      safe.width, std::max<int16_t>(48, static_cast<int16_t>(props.grabberWidth + props.grabberMargin * 2)));
  return fui::Rect{static_cast<int16_t>(safe.x + (safe.width - width) / 2), y, width, height};
}

template <size_t MaxInteractions>
fui::Rect registerTap(fui::Frame<MaxInteractions>& frame, const fui::Rect content, const fui::SheetProps& props,
                      const fui::ActionId action) {
  const fui::Rect rect = hitRect(content, frame.safeRect(), props);
  frame.hit(rect, action, 0, fui::InputTouch);
  return rect;
}

inline bool wasDismissSwipe(const MappedInputManager& input, const fui::Rect handle, const fui::SheetEdge anchor) {
  MappedInputManager::SwipeDir direction = MappedInputManager::SwipeDir::None;
  int startX = 0;
  int startY = 0;
  int endX = 0;
  int endY = 0;
  if (!input.wasSwipeWithPoints(direction, startX, startY, endX, endY) ||
      !handle.contains(static_cast<int16_t>(startX), static_cast<int16_t>(startY))) {
    return false;
  }
  return anchor == fui::SheetEdge::Top ? direction == MappedInputManager::SwipeDir::Up
                                       : direction == MappedInputManager::SwipeDir::Down;
}

}  // namespace DrawerHandle
