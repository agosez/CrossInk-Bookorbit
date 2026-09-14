#pragma once

#include <algorithm>

// Slider tracks cover a wide range, so a direct tap should land on a useful
// coarse value. The adjacent +/- controls retain the precise adjustment path.
constexpr int snapSliderTapValue(const int value, const int minimum, const int maximum, const int step) {
  const int clamped = std::clamp(value, minimum, maximum);
  if (step <= 1 || minimum >= maximum) return clamped;

  const int offset = clamped - minimum;
  const int rounded = minimum + ((offset + step / 2) / step) * step;
  return std::clamp(rounded, minimum, maximum);
}
