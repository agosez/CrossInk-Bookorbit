#pragma once

#include <array>
#include <cstddef>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power };

  bool wasReleased(const Button button) const { return released[indexOf(button)]; }
  bool isPressed(const Button button) const { return pressed[indexOf(button)]; }

  void setPressed(const Button button, const bool value) { pressed[indexOf(button)] = value; }
  void setReleased(const Button button, const bool value) { released[indexOf(button)] = value; }

 private:
  static constexpr size_t indexOf(const Button button) { return static_cast<size_t>(button); }

  std::array<bool, 7> pressed{};
  std::array<bool, 7> released{};
};
