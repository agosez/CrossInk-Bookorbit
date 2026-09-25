#pragma once

#include <HalGPIO.h>

#include "AppCapabilities.h"

inline bool deviceHasEdgeSideButtons(const HalGPIO& gpio) {
#ifdef SIMULATOR
  return gpio.deviceIsX3();
#else
  return gpio.hasEdgeSideButtons();
#endif
}

inline bool deviceUsesSideButtonHintGutters(const HalGPIO& gpio) {
  if (!deviceHasEdgeSideButtons(gpio)) return false;
#if CROSSINK_APP_CAP_TOUCH
  return !gpio.hasTouch();
#else
  return true;
#endif
}

// Touch readers can safely dedicate both side buttons to a chord because Back
// and Confirm remain available on-screen. X4 Classic has the same two edge
// buttons plus a separate four-button front cluster, so it has the same escape
// path without a touchscreen.
inline bool deviceSupportsSideButtonChord(const HalGPIO& gpio) {
#if CROSSINK_APP_DEVICE_X4CLASSIC || defined(SIMULATOR_DEVICE_X4_CLASSIC)
  return true;
#else
  return gpio.hasTouch();
#endif
}
