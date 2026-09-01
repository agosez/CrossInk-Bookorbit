#pragma once

class HalGPIO {
 public:
  void update() const {}
  bool hasTouch() const { return false; }
  void suppressTouchContact() const {}
};

inline HalGPIO gpio;
